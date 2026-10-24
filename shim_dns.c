/*
 * shim_dns.c — getaddrinfo(3) fallback for Claude Code on Termux/Android.
 *
 * Some Android ROMs symlink /etc to a read-only /system/etc that ships no
 * resolv.conf. glibc's resolver then starts with zero nameservers and every
 * lookup fails outright ("getaddrinfo ENOTIMP"), so the glibc build of Claude
 * Code cannot reach the API even though the network is up.
 *
 * This LD_PRELOAD shim does two things:
 *
 *   1. It wraps getaddrinfo(). The real implementation runs first, so a
 *      working system resolver keeps its normal behaviour (/etc/hosts,
 *      IPv6, NSS). Only when it fails do we fall back to a minimal UDP DNS
 *      client that reads nameservers from the Termux-writable resolv.conf
 *      (or the built-in public defaults) and resolves A records itself.
 *
 *   2. It redirects reads of /etc/resolv.conf to the Termux-writable copy.
 *      Claude Code 2.x runs on Bun, and Bun's resolver (and anything else
 *      that parses resolv.conf directly, e.g. c-ares-based code) opens the
 *      file itself instead of calling getaddrinfo — with no file, such
 *      resolvers silently fall back to 127.0.0.1 and every connection
 *      times out. Hooking open/openat/fopen/stat transparently hands those
 *      readers the generated config under $PREFIX/etc.
 *
 * Deliberately small: A records only, no search domains, no TCP retry, no
 * EDNS. It exists to make one HTTPS client work, not to replace libresolv.
 *
 * Debug: set CLAUDE_DNS_SHIM_DEBUG=1 to trace decisions on stderr.
 * Build:  ./build-dns-shim.sh   (needs the glibc toolchain, not the NDK)
 */

#define _GNU_SOURCE
#include <arpa/inet.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

/* Termux's own resolv.conf lives under $PREFIX/etc, which is writable. */
#define SHIM_RESOLV_CONF "/data/data/com.termux/files/usr/etc/resolv.conf"
#define SHIM_NS_FALLBACK_1 "8.8.8.8"
#define SHIM_NS_FALLBACK_2 "8.8.4.4"

#define SHIM_MAX_NS      4
#define SHIM_NS_LEN      64
#define SHIM_MAX_ADDRS   8
#define SHIM_TIMEOUT_SEC 3
#define SHIM_DNS_PORT    53
#define SHIM_PKT_MAX     1500

static void dlog(const char *fmt, ...)
{
	if (!getenv("CLAUDE_DNS_SHIM_DEBUG"))
		return;
	va_list ap;
	va_start(ap, fmt);
	fputs("[dns-shim] ", stderr);
	vfprintf(stderr, fmt, ap);
	fputc('\n', stderr);
	va_end(ap);
}

/* Resolve the real getaddrinfo once and cache it. */
typedef int (*ga_fn)(const char *, const char *, const struct addrinfo *,
		     struct addrinfo **);

static ga_fn real_ga(void)
{
	static ga_fn cached;
	if (!cached)
		cached = (ga_fn)dlsym(RTLD_NEXT, "getaddrinfo");
	return cached;
}

/*
 * Collect nameservers: resolv.conf first, else the public fallbacks. Only
 * IPv4 literals are kept — the fallback path speaks UDP/IPv4 to the server.
 */
static int load_nameservers(char ns[SHIM_MAX_NS][SHIM_NS_LEN])
{
	int n = 0;
	FILE *f = fopen(SHIM_RESOLV_CONF, "re");

	if (f) {
		char line[256];
		while (n < SHIM_MAX_NS && fgets(line, sizeof(line), f)) {
			char key[16], val[SHIM_NS_LEN];
			struct in_addr probe;

			if (sscanf(line, "%15s %63s", key, val) != 2)
				continue;
			if (strcmp(key, "nameserver") != 0)
				continue;
			if (inet_pton(AF_INET, val, &probe) != 1)
				continue;
			memcpy(ns[n++], val, strlen(val) + 1);
		}
		fclose(f);
	}

	if (n == 0) {
		memcpy(ns[n++], SHIM_NS_FALLBACK_1, sizeof(SHIM_NS_FALLBACK_1));
		memcpy(ns[n++], SHIM_NS_FALLBACK_2, sizeof(SHIM_NS_FALLBACK_2));
	}
	return n;
}

/* Encode "example.com" as length-prefixed DNS labels. Returns bytes used. */
static int encode_qname(const char *host, unsigned char *out, size_t outlen)
{
	size_t used = 0;
	const char *p = host;

	while (*p) {
		const char *dot = strchr(p, '.');
		size_t len = dot ? (size_t)(dot - p) : strlen(p);

		if (len == 0 || len > 63)
			return -1;               /* empty or oversized label */
		if (used + len + 2 > outlen)
			return -1;
		out[used++] = (unsigned char)len;
		memcpy(out + used, p, len);
		used += len;
		if (!dot)
			break;
		p = dot + 1;
		if (!*p)
			break;                   /* trailing dot: FQDN form */
	}
	if (used + 1 > outlen)
		return -1;
	out[used++] = 0;                         /* root label */
	return (int)used;
}

/*
 * Advance past a name at *off*. Compression pointers terminate the name, so
 * we do not follow them — callers only need the length, never the text.
 */
static int skip_name(const unsigned char *pkt, int len, int off)
{
	while (off < len) {
		unsigned char l = pkt[off];

		if (l == 0)
			return off + 1;
		if ((l & 0xC0) == 0xC0)
			return off + 2 <= len ? off + 2 : -1;
		off += l + 1;
	}
	return -1;
}

/*
 * One A-record query against one server. Returns the count written to addrs,
 * or -1 on any transport/parse failure so the caller can try the next server.
 */
static int query_a(const char *server, const char *host,
		   struct in_addr *addrs, int max_addrs)
{
	unsigned char pkt[SHIM_PKT_MAX];
	struct sockaddr_in sa;
	struct timeval tv;
	struct timespec ts;
	int fd, qlen, off, qdcount, ancount, found = 0;
	uint16_t id;
	ssize_t n;

	memset(&sa, 0, sizeof(sa));
	sa.sin_family = AF_INET;
	sa.sin_port = htons(SHIM_DNS_PORT);
	if (inet_pton(AF_INET, server, &sa.sin_addr) != 1)
		return -1;

	/* No rand(): avoid perturbing the host program's PRNG sequence. */
	clock_gettime(CLOCK_MONOTONIC, &ts);
	id = (uint16_t)((ts.tv_nsec ^ (getpid() << 8)) & 0xFFFF);

	memset(pkt, 0, 12);
	pkt[0] = id >> 8;
	pkt[1] = id & 0xFF;
	pkt[2] = 0x01;                           /* RD — recursion desired */
	pkt[5] = 0x01;                           /* QDCOUNT = 1 */

	qlen = encode_qname(host, pkt + 12, sizeof(pkt) - 12 - 4);
	if (qlen < 0)
		return -1;
	off = 12 + qlen;
	pkt[off++] = 0; pkt[off++] = 1;          /* QTYPE  = A  */
	pkt[off++] = 0; pkt[off++] = 1;          /* QCLASS = IN */

	fd = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
	if (fd < 0)
		return -1;
	tv.tv_sec = SHIM_TIMEOUT_SEC;
	tv.tv_usec = 0;
	setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

	if (sendto(fd, pkt, off, 0, (struct sockaddr *)&sa, sizeof(sa)) != off) {
		close(fd);
		return -1;
	}
	n = recvfrom(fd, pkt, sizeof(pkt), 0, NULL, NULL);
	close(fd);

	if (n < 12)
		return -1;
	if (pkt[0] != (id >> 8) || pkt[1] != (id & 0xFF))
		return -1;                       /* not our transaction */
	if (pkt[3] & 0x0F) {
		dlog("rcode=%d from %s", pkt[3] & 0x0F, server);
		return -1;
	}

	qdcount = (pkt[4] << 8) | pkt[5];
	ancount = (pkt[6] << 8) | pkt[7];
	off = 12;
	for (int i = 0; i < qdcount; i++) {
		off = skip_name(pkt, (int)n, off);
		if (off < 0 || off + 4 > n)
			return -1;
		off += 4;                        /* QTYPE + QCLASS */
	}

	for (int i = 0; i < ancount && found < max_addrs; i++) {
		int type, rdlen;

		off = skip_name(pkt, (int)n, off);
		if (off < 0 || off + 10 > n)
			break;
		type  = (pkt[off] << 8) | pkt[off + 1];
		rdlen = (pkt[off + 8] << 8) | pkt[off + 9];
		off += 10;
		if (off + rdlen > n)
			break;
		if (type == 1 && rdlen == 4)     /* A record */
			memcpy(&addrs[found++], pkt + off, 4);
		off += rdlen;                    /* skip CNAME etc. */
	}
	return found;
}

/* Map the service argument to a port. Numeric first, then the two names
 * that actually matter here; anything else resolves to port 0. */
static uint16_t resolve_port(const char *service)
{
	if (!service || !*service)
		return 0;
	char *end;
	long v = strtol(service, &end, 10);
	if (*end == '\0' && v >= 0 && v <= 65535)
		return (uint16_t)v;
	if (!strcmp(service, "https"))
		return 443;
	if (!strcmp(service, "http"))
		return 80;
	return 0;
}

/* ---------------------------------------------------------------------
 * resolv.conf redirection.
 *
 * Bun's resolver and other libc-independent resolvers open
 * /etc/resolv.conf themselves; on ROMs without the file they fall back
 * to 127.0.0.1 and time out. Interposing the open/read calls and handing
 * back the Termux-writable copy fixes those readers the same way the
 * getaddrinfo wrapper above fixes glibc ones.
 */

static const char *redirect_target(void)
{
	static const char *cached;
	if (cached)
		return cached;
	const char *env = getenv("CLAUDE_DNS_RESOLV_CONF");
	if (env && *env && access(env, R_OK) == 0) {
		cached = env;
		return cached;
	}
	if (access(SHIM_RESOLV_CONF, R_OK) == 0) {
		cached = SHIM_RESOLV_CONF;
		return cached;
	}
	return NULL;                       /* nothing usable — don't redirect */
}

static int is_etc_resolv(const char *path)
{
	return path && !strcmp(path, "/etc/resolv.conf");
}

/* open with variadic mode — needs hand-rolling, macros can't expand it. */
typedef int (*open_fn)(const char *, int, ...);
static open_fn open_real(void)
{
	static open_fn cached;
	if (!cached)
		cached = (open_fn)dlsym(RTLD_NEXT, "open");
	return cached;
}

int open(const char *path, int flags, ...)
{
	mode_t mode = 0;
	if (flags & (O_CREAT | O_TMPFILE)) {
		va_list ap;
		va_start(ap, flags);
		mode = (mode_t)va_arg(ap, int);
		va_end(ap);
	}
	const char *r = is_etc_resolv(path) ? redirect_target() : NULL;
	if (r) {
		dlog("redirected open(\"%s\") -> \"%s\"", path, r);
		return open_real()(r, flags, mode);
	}
	return open_real()(path, flags, mode);
}

typedef int (*open64_fn)(const char *, int, ...);
static open64_fn open64_real(void)
{
	static open64_fn cached;
	if (!cached)
		cached = (open64_fn)dlsym(RTLD_NEXT, "open64");
	return cached ? cached : (open64_fn)open_real();
}

int open64(const char *path, int flags, ...)
{
	mode_t mode = 0;
	if (flags & (O_CREAT | O_TMPFILE)) {
		va_list ap;
		va_start(ap, flags);
		mode = (mode_t)va_arg(ap, int);
		va_end(ap);
	}
	const char *r = is_etc_resolv(path) ? redirect_target() : NULL;
	if (r) {
		dlog("redirected open64(\"%s\") -> \"%s\"", path, r);
		return open64_real()(r, flags, mode);
	}
	return open64_real()(path, flags, mode);
}

typedef int (*openat_fn)(int, const char *, int, ...);
static openat_fn openat_real(void)
{
	static openat_fn cached;
	if (!cached)
		cached = (openat_fn)dlsym(RTLD_NEXT, "openat");
	return cached;
}

int openat(int dirfd, const char *path, int flags, ...)
{
	mode_t mode = 0;
	if (flags & (O_CREAT | O_TMPFILE)) {
		va_list ap;
		va_start(ap, flags);
		mode = (mode_t)va_arg(ap, int);
		va_end(ap);
	}
	const char *r = is_etc_resolv(path) ? redirect_target() : NULL;
	if (r) {
		dlog("redirected openat(\"%s\") -> \"%s\"", path, r);
		return openat_real()(dirfd, r, flags, mode);
	}
	return openat_real()(dirfd, path, flags, mode);
}

typedef int (*openat64_fn)(int, const char *, int, ...);
static openat64_fn openat64_real(void)
{
	static openat64_fn cached;
	if (!cached)
		cached = (openat64_fn)dlsym(RTLD_NEXT, "openat64");
	return cached ? cached : (openat64_fn)openat_real();
}

int openat64(int dirfd, const char *path, int flags, ...)
{
	mode_t mode = 0;
	if (flags & (O_CREAT | O_TMPFILE)) {
		va_list ap;
		va_start(ap, flags);
		mode = (mode_t)va_arg(ap, int);
		va_end(ap);
	}
	const char *r = is_etc_resolv(path) ? redirect_target() : NULL;
	if (r) {
		dlog("redirected openat64(\"%s\") -> \"%s\"", path, r);
		return openat64_real()(dirfd, r, flags, mode);
	}
	return openat64_real()(dirfd, path, flags, mode);
}

typedef FILE *(*fopen_fn)(const char *, const char *);
static fopen_fn fopen_real(void)
{
	static fopen_fn cached;
	if (!cached)
		cached = (fopen_fn)dlsym(RTLD_NEXT, "fopen");
	return cached;
}

FILE *fopen(const char *path, const char *mode)
{
	const char *r = is_etc_resolv(path) ? redirect_target() : NULL;
	if (r) {
		dlog("redirected fopen(\"%s\") -> \"%s\"", path, r);
		return fopen_real()(r, mode);
	}
	return fopen_real()(path, mode);
}

typedef FILE *(*fopen64_fn)(const char *, const char *);
static fopen64_fn fopen64_real(void)
{
	static fopen64_fn cached;
	if (!cached)
		cached = (fopen64_fn)dlsym(RTLD_NEXT, "fopen64");
	return cached ? cached : (fopen64_fn)fopen_real();
}

FILE *fopen64(const char *path, const char *mode)
{
	const char *r = is_etc_resolv(path) ? redirect_target() : NULL;
	if (r) {
		dlog("redirected fopen64(\"%s\") -> \"%s\"", path, r);
		return fopen64_real()(r, mode);
	}
	return fopen64_real()(path, mode);
}

typedef int (*stat_fn)(const char *, struct stat *);
static stat_fn stat_real(void)
{
	static stat_fn cached;
	if (!cached)
		cached = (stat_fn)dlsym(RTLD_NEXT, "stat");
	return cached;
}

int stat(const char *path, struct stat *buf)
{
	const char *r = is_etc_resolv(path) ? redirect_target() : NULL;
	if (r) {
		dlog("redirected stat(\"%s\") -> \"%s\"", path, r);
		return stat_real()(r, buf);
	}
	return stat_real()(path, buf);
}

typedef int (*stat64_fn)(const char *, struct stat64 *);
static stat64_fn stat64_real(void)
{
	static stat64_fn cached;
	if (!cached)
		cached = (stat64_fn)dlsym(RTLD_NEXT, "stat64");
	return cached;
}

int stat64(const char *path, struct stat64 *buf)
{
	const char *r = is_etc_resolv(path) ? redirect_target() : NULL;
	if (r) {
		dlog("redirected stat64(\"%s\") -> \"%s\"", path, r);
		return stat64_real()(r, buf);
	}
	return stat64_real()(path, buf);
}

/* Build the addrinfo chain glibc would have returned. */
static int build_result(const struct in_addr *addrs, int count, uint16_t port,
			const struct addrinfo *hints, struct addrinfo **res)
{
	struct addrinfo *head = NULL, *tail = NULL;
	int socktype = hints && hints->ai_socktype ? hints->ai_socktype
						   : SOCK_STREAM;
	int protocol = hints && hints->ai_protocol ? hints->ai_protocol
						   : (socktype == SOCK_DGRAM
						      ? IPPROTO_UDP
						      : IPPROTO_TCP);

	for (int i = 0; i < count; i++) {
		struct addrinfo *ai = calloc(1, sizeof(*ai));
		struct sockaddr_in *sin = calloc(1, sizeof(*sin));

		if (!ai || !sin) {
			free(ai);
			free(sin);
			freeaddrinfo(head);      /* frees what we built so far */
			return EAI_MEMORY;
		}
		sin->sin_family = AF_INET;
		sin->sin_port = htons(port);
		sin->sin_addr = addrs[i];

		ai->ai_family = AF_INET;
		ai->ai_socktype = socktype;
		ai->ai_protocol = protocol;
		ai->ai_addrlen = sizeof(*sin);
		ai->ai_addr = (struct sockaddr *)sin;

		if (tail)
			tail->ai_next = ai;
		else
			head = ai;
		tail = ai;
	}
	if (!head)
		return EAI_NONAME;
	*res = head;
	return 0;
}

int getaddrinfo(const char *node, const char *service,
		const struct addrinfo *hints, struct addrinfo **res)
{
	ga_fn orig = real_ga();
	struct in_addr addrs[SHIM_MAX_ADDRS];
	char ns[SHIM_MAX_NS][SHIM_NS_LEN];
	int rc, nns;

	if (!orig)                               /* nothing to wrap */
		return EAI_SYSTEM;

	rc = orig(node, service, hints, res);
	if (rc == 0)
		return 0;

	/* Only name lookups are worth retrying: no host, or a caller that
	 * wants a numeric-only or non-IPv4 answer, we leave to glibc. */
	if (!node || !*node)
		return rc;
	if (hints) {
		if (hints->ai_flags & AI_NUMERICHOST)
			return rc;
		if (hints->ai_family != AF_UNSPEC &&
		    hints->ai_family != AF_INET)
			return rc;
	}

	dlog("real getaddrinfo(%s) failed rc=%d -> fallback", node, rc);

	nns = load_nameservers(ns);
	for (int i = 0; i < nns; i++) {
		int found = query_a(ns[i], node, addrs, SHIM_MAX_ADDRS);

		if (found > 0) {
			int brc = build_result(addrs, found,
					       resolve_port(service),
					       hints, res);
			if (brc != 0)
				return brc;
			dlog("fallback resolved %s -> %d address(es)",
			     node, found);
			return 0;
		}
	}
	dlog("fallback failed for %s (%d nameserver(s) tried)", node, nns);
	return rc;
}
