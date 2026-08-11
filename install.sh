#!/data/data/com.termux/files/usr/bin/bash
#
# install.sh — one-command bootstrap for claude-code-termux.
#
#   curl -fsSL https://raw.githubusercontent.com/bd-loser/claude-code-termux/main/install.sh | bash
#
# Installs the dependencies, fetches this repo, and hands off to
# `claude-code-termux install`, which does the actual work.
#
# Why a separate bootstrap rather than piping the manager itself into bash:
# a piped script has no path of its own ($0 is "bash"), so the manager could
# neither copy itself to a stable location nor find shim_dns.c next to it.
# This script gives it a real directory to work from first.
#
# Honours the same overrides as the manager, plus:
#   CLAUDE_TERMUX_REPO  owner/name to fetch from   (default bd-loser/claude-code-termux)
#   CLAUDE_TERMUX_REF   branch or tag to fetch     (default main)
#   CLAUDE_TERMUX_SRC   use an existing local checkout instead of downloading
#   CLAUDE_TERMUX_NO_PATH=1   don't touch ~/.bashrc

set -euo pipefail

REPO="${CLAUDE_TERMUX_REPO:-bd-loser/claude-code-termux}"
REF="${CLAUDE_TERMUX_REF:-main}"
APP_PREFIX="${APP_PREFIX:-/data/data/com.termux/files/usr}"

C_BLUE=''; C_GREEN=''; C_YELLOW=''; C_RED=''; C_RESET=''
if [ -t 1 ]; then
  C_BLUE='\033[1;34m'; C_GREEN='\033[1;32m'; C_YELLOW='\033[1;33m'
  C_RED='\033[1;31m'; C_RESET='\033[0m'
fi
log()  { printf '%b[install]%b %s\n' "$C_BLUE"   "$C_RESET" "$*"; }
ok()   { printf '%b[install]%b %s\n' "$C_GREEN"  "$C_RESET" "$*"; }
warn() { printf '%b[install]%b %s\n' "$C_YELLOW" "$C_RESET" "$*" >&2; }
die()  { printf '%b[install]%b %s\n' "$C_RED"    "$C_RESET" "$*" >&2; exit 1; }

# --- sanity ---------------------------------------------------------------
[ -d "$APP_PREFIX" ] || die "This doesn't look like Termux (no $APP_PREFIX).
  claude-code-termux is Termux-specific. On a normal Linux distro just run:
    npm install -g @anthropic-ai/claude-code"

case "$(uname -m)" in
  aarch64|arm64|x86_64|amd64) ;;
  *) die "Unsupported architecture: $(uname -m). Needs arm64 or x86_64." ;;
esac

command -v pkg >/dev/null 2>&1 || die "'pkg' not found — is this really Termux?"

# --- dependencies ---------------------------------------------------------
# gcc-glibc is intentionally included: glibc-runner does NOT depend on it, and
# without it the DNS shim can't be built on ROMs that need it.
NEEDED=()
command -v node >/dev/null 2>&1 || NEEDED+=(nodejs)
command -v curl >/dev/null 2>&1 || NEEDED+=(curl)
command -v cc >/dev/null 2>&1 || NEEDED+=(clang)
[ -d "$APP_PREFIX/glibc/lib" ]  || NEEDED+=(glibc-runner)
[ -x "$APP_PREFIX/glibc/bin/gcc" ] || NEEDED+=(gcc-glibc)

if [ ${#NEEDED[@]} -gt 0 ]; then
  log "Installing: ${NEEDED[*]}"
  log "(this is the slow part — glibc is a few hundred MB)"
  pkg install -y "${NEEDED[@]}" || die "pkg install failed. Try 'pkg update' first."
else
  ok "Dependencies already present."
fi

# --- npm package ----------------------------------------------------------
# We need it for the version number and the install location. Its postinstall
# WILL fail (that 404 is the whole reason this project exists) and npm exits
# nonzero, so don't let set -e treat that as fatal.
if [ -d "$APP_PREFIX/lib/node_modules/@anthropic-ai/claude-code" ]; then
  ok "npm package already installed."
else
  log "Installing @anthropic-ai/claude-code (its postinstall will 404 — expected) ..."
  npm install -g @anthropic-ai/claude-code >/dev/null 2>&1 \
    || warn "npm reported an error — expected, continuing."
  [ -d "$APP_PREFIX/lib/node_modules/@anthropic-ai/claude-code" ] \
    || die "npm package still missing. Run manually to see why:
    npm install -g @anthropic-ai/claude-code"
fi

# --- fetch this repo ------------------------------------------------------
if [ -n "${CLAUDE_TERMUX_SRC:-}" ]; then
  SRC="$CLAUDE_TERMUX_SRC"
  log "Using local checkout: $SRC"
else
  SRC="$(mktemp -d)"
  trap 'rm -rf "$SRC"' EXIT
  log "Fetching $REPO@$REF ..."
  curl -fsSL "https://codeload.github.com/$REPO/tar.gz/$REF" \
    | tar -xz -C "$SRC" --strip-components=1 \
    || die "Download failed. Check your connection, or that $REPO@$REF exists."
fi

[ -f "$SRC/claude-code-termux" ] || die "Fetched archive is missing claude-code-termux."
chmod 755 "$SRC/claude-code-termux" "$SRC/build-shim.sh" 2>/dev/null || true

# --- hand off -------------------------------------------------------------
# The manager copies itself and the shim sources to a stable location, fetches
# the real binary, and writes the launcher.
log "Running installer ..."
"$SRC/claude-code-termux" install

# --- PATH -----------------------------------------------------------------
BIN_DIR="$HOME/.local/bin"
ON_PATH=1
case ":$PATH:" in
  *":$BIN_DIR:"*) ;;
  *)
    ON_PATH=0
    if [ "${CLAUDE_TERMUX_NO_PATH:-0}" = 1 ]; then
      warn "$BIN_DIR is not on PATH; add it yourself (CLAUDE_TERMUX_NO_PATH=1 set)."
    elif [ -f "$HOME/.bashrc" ] && grep -qF '.local/bin' "$HOME/.bashrc" 2>/dev/null; then
      warn "~/.bashrc already mentions .local/bin but it's not on PATH — check it by hand."
    else
      printf '\n# Added by claude-code-termux\nexport PATH="$HOME/.local/bin:$PATH"\n' >> "$HOME/.bashrc"
      ok "Added $BIN_DIR to PATH in ~/.bashrc"
    fi
    ;;
esac

echo
if [ "$ON_PATH" = 1 ]; then
  ok "Done. Start Claude Code with:  claude"
  ok "Health check:                  claude-code-termux doctor"
else
  ok "Done — but open a new session first (or run: exec bash)."
  ok "Then start Claude Code with:   claude"
  ok "Health check:                  claude-code-termux doctor"
fi
