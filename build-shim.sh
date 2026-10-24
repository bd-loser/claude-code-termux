#!/data/data/com.termux/files/usr/bin/bash
#
# build-shim.sh — compile claude-dns-shim.so from shim_dns.c.
#
# The shim is LD_PRELOAD'ed into the *glibc* Claude binary, so it must be
# built against glibc — not Termux's bionic libc. The compiler for that comes
# from the gcc-glibc package, which glibc-runner does NOT depend on:
#
#   pkg install gcc-glibc
#
# Two quirks of that toolchain, both handled below:
#
#   1. The glibc sysroot has no kernel UAPI headers ($GLIBC_PREFIX/include/asm
#      is a broken symlink), so <asm/socket.h> is missing and sys/socket.h
#      won't compile. Termux's sysroot does have them, and UAPI headers are
#      libc-independent, so we add it with -idirafter: searched *after* the
#      glibc dirs, so glibc's own stdio.h/netdb.h/etc. still win.
#
#   2. gcc drives the linker with --fix-cortex-a53-835769, which Termux's
#      default lld doesn't understand. Putting the glibc binutils first on
#      PATH picks up the matching GNU ld.
#
# Usage: ./build-shim.sh [output-path]

set -euo pipefail

# Cross-libc compiles must never inherit a host LD_PRELOAD/LD_LIBRARY_PATH:
# the glibc gcc/ld will then try to load whatever the surrounding Termux
# environment preloads (often a bionic object or a text linker script such
# as libc.so) and die with "invalid ELF header" before compiling anything.
# GitHub issue #1: build-shim failing for exactly this reason left users
# without the DNS shim, which then manifested as login DNS timeouts.
unset LD_PRELOAD LD_LIBRARY_PATH

APP_PREFIX="${APP_PREFIX:-/data/data/com.termux/files/usr}"
GLIBC_PREFIX="${GLIBC_PREFIX:-$APP_PREFIX/glibc}"
SRC="$(dirname "$(readlink -f "$0")")/shim_dns.c"
OUT="${1:-$APP_PREFIX/lib/claude-dns-shim.so}"

# Plain `gcc` rather than aarch64-linux-gnu-gcc: it's a symlink to whichever
# triplet this install ships, so the same line works on x86_64.
CC="${CC:-$GLIBC_PREFIX/bin/gcc}"

[ -f "$SRC" ] || { echo "build-shim: source not found: $SRC" >&2; exit 1; }
[ -x "$CC" ] || {
  echo "build-shim: glibc C compiler not found at $CC" >&2
  echo "  Install it with: pkg install gcc-glibc" >&2
  exit 1
}

mkdir -p "$(dirname "$OUT")"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

echo "build-shim: compiling $SRC"
PATH="$GLIBC_PREFIX/bin:$PATH" "$CC" \
  -shared -fPIC -O2 -Wall -Wextra \
  -idirafter "$APP_PREFIX/include" \
  -o "$TMP/shim.so" "$SRC" -ldl

# Sanity-check before overwriting anything: it must export getaddrinfo.
if command -v readelf >/dev/null 2>&1; then
  readelf --dyn-syms -W "$TMP/shim.so" 2>/dev/null \
    | awk '$5=="GLOBAL" && $8=="getaddrinfo"{found=1} END{exit !found}' || {
      echo "build-shim: built object does not export getaddrinfo — aborting" >&2
      exit 1
    }
fi

install -m 755 "$TMP/shim.so" "$OUT"
echo "build-shim: installed $OUT"
