#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define DEFAULT_PREFIX "/data/data/com.termux/files/usr"

static const char *value_or(const char *name, const char *fallback) {
    const char *value = getenv(name);
    return value && *value ? value : fallback;
}

int main(int argc, char **argv) {
    const char *ld = value_or("GLIBC_LD", DEFAULT_PREFIX "/glibc/lib/ld-linux-aarch64.so.1");
    const char *lib = value_or("GLIBC_LIB_DIR", DEFAULT_PREFIX "/glibc/lib");
    const char *bin = value_or("CLAUDE_NATIVE", DEFAULT_PREFIX "/lib/node_modules/@anthropic-ai/claude-code/bin/claude-native");
    const char *shim = value_or("CLAUDE_DNS_SHIM", DEFAULT_PREFIX "/lib/claude-dns-shim.so");
    int preload = access(shim, R_OK) == 0;
    size_t fixed = preload ? 8 : 6;
    char **next = calloc((size_t)argc + fixed, sizeof(*next));
    size_t i = 0;

    if (!next) {
        perror("claude-exec: calloc");
        return 126;
    }

    next[i++] = (char *)ld;
    next[i++] = "--library-path";
    next[i++] = (char *)lib;
    if (preload) {
        next[i++] = "--preload";
        next[i++] = (char *)shim;
    }
    next[i++] = "--argv0";
    next[i++] = argv[0];
    next[i++] = (char *)bin;
    for (int arg = 1; arg < argc; arg++) next[i++] = argv[arg];
    next[i] = NULL;

    unsetenv("LD_PRELOAD");
    unsetenv("LD_LIBRARY_PATH");
    execv(ld, next);
    fprintf(stderr, "claude-exec: %s: %s\n", ld, strerror(errno));
    return 126;
}
