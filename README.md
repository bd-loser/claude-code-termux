# claude-code-termux

**Install and run [Claude Code](https://claude.com/claude-code) on Android with
Termux — no root, no proot, no Ubuntu chroot.**

Claude Code has no official Android build. This fixes that: it installs
Anthropic's real `linux-arm64` binary and runs it natively under Termux via
`glibc-runner`. Works on arm64 and x86_64.

```sh
curl -fsSL https://raw.githubusercontent.com/bd-loser/claude-code-termux/main/install.sh | bash
```

That's the whole install. It pulls the dependencies, fetches Claude Code, sets
up the launcher, and puts `~/.local/bin` on your `PATH`. Then:

```sh
exec bash   # only needed the first time, to pick up PATH
claude
```

Prefer to read before you run? Same thing, in two steps:

```sh
curl -fsSL -o install.sh https://raw.githubusercontent.com/bd-loser/claude-code-termux/main/install.sh
less install.sh && bash install.sh
```

If you landed here from one of these errors, you're in the right place:

- `npm error 404 Not Found - GET https://registry.npmjs.org/@anthropic-ai%2fclaude-code-linux-arm64-android`
- `claude: command not found` after `npm install -g @anthropic-ai/claude-code`
- `getaddrinfo ENOTIMP` when Claude Code tries to reach the API
- `-G: cannot open shared object file` from `grep` or `find` inside a session
- `Claude Code is not supported on this platform` / an installed `claude` that
  exits silently and does nothing

## Why this exists

Claude Code 2.x ships as a native binary rather than JavaScript. Its npm
postinstall picks a platform package based on `process.platform`, and on Termux
that resolves to `@anthropic-ai/claude-code-linux-arm64-android` — a package
Anthropic has never published. npm returns 404, the postinstall gives up, and
`claude` is left as a stub that does nothing.

The binary itself is fine, though. It's an ordinary glibc `linux-arm64` build,
and Termux can run glibc binaries via the `glibc-runner` package. So this tool:

1. Reads the version of the `@anthropic-ai/claude-code` package you installed.
2. Downloads the matching `@anthropic-ai/claude-code-linux-arm64` tarball — the
   glibc build, not the missing android one.
3. Installs a launcher at `~/.local/bin/claude` that runs it through the glibc
   dynamic linker.

## Install

Requires Termux (from [F-Droid](https://f-droid.org/packages/com.termux/) or
GitHub — the Play Store build is too old), and roughly 1.5 GB free for
`glibc-runner` plus the ~270 MB binary.

```sh
pkg install nodejs glibc-runner gcc-glibc
npm install -g @anthropic-ai/claude-code   # gives you the stub + version info

git clone https://github.com/bd-loser/claude-code-termux
cd claude-code-termux
./claude-code-termux install
```

(`gcc-glibc` is only needed if your device turns out to need the
[DNS shim](#the-dns-shim). Harmless to install either way.)

`~/.local/bin` is **not** on Termux's default `PATH`. If `install` tells you so,
add it:

```sh
echo 'export PATH="$HOME/.local/bin:$PATH"' >> ~/.bashrc
exec bash
```

Then `claude --version` should print a version, and `claude` should start.

## Usage

```
claude-code-termux install [version]   Download the glibc binary + install the launcher
claude-code-termux update              Re-download to match the npm package version
claude-code-termux uninstall [--purge] Remove binary, launcher, shim, manager copy
claude-code-termux doctor              Health check; exits nonzero if anything is broken
claude-code-termux build-shim          Rebuild the DNS fallback shim
claude-code-termux launcher-path       Print the launcher path
claude-code-termux manager-path        Print where this script installs itself
```

`install` with no version argument matches whatever `@anthropic-ai/claude-code`
you have installed via npm, which keeps the binary and the package in sync. Pass
a version to pin one:

```sh
claude-code-termux install 2.1.220
```

`doctor` is the thing to run first when something breaks. It checks
dependencies, architecture, the glibc install, package/binary version drift, the
launcher, `PATH`, the shim, and finally actually executes the binary.

## What gets installed where

| Path | What |
| --- | --- |
| `$PREFIX/lib/node_modules/@anthropic-ai/claude-code/bin/claude-native` | the real glibc binary, replacing the stub |
| `~/.local/bin/claude` | the launcher (`launcher-path`) |
| `~/.local/share/claude-code-termux/` | a copy of this script + shim sources (`manager-path`) |
| `$PREFIX/lib/claude-dns-shim.so` | the DNS shim, if it was needed |

The manager copies itself to `~/.local/share/claude-code-termux/` because the
launcher self-heals: if it finds the binary missing or reverted to a stub (an
`npm update` will do that), it re-runs `install` automatically. Pointing that at
the clone you happened to run from would break as soon as you deleted or moved
it. Override with `CLAUDE_TERMUX_HOME`.

## The DNS shim

Some Android ROMs symlink `/etc` to a read-only `/system/etc` that ships no
`resolv.conf`. Bionic doesn't care — it gets its resolver config from the
system property service — but glibc reads `/etc/resolv.conf` and finds nothing.
With zero configured nameservers, every lookup fails immediately:

```
getaddrinfo ENOTIMP
```

If `install` detects no usable `resolv.conf`, it compiles `shim_dns.c` — a small
`getaddrinfo` interposer — and the launcher preloads it. The shim tries the real
`getaddrinfo` first and only steps in when that fails, at which point it does a
plain UDP A-record query against the nameservers in
`$PREFIX/etc/resolv.conf`, falling back to `8.8.8.8` / `8.8.4.4`.

On a device with working resolver config the shim is never built and never
loaded. To check, or to rebuild it by hand:

```sh
claude-code-termux doctor
claude-code-termux build-shim
CLAUDE_DNS_SHIM_DEBUG=1 claude --version   # trace shim decisions on stderr
```

Building it needs `gcc-glibc` (a separate package — `glibc-runner` does not pull
it in). Two toolchain quirks `build-shim.sh` works around, in case you build by
hand:

- the glibc sysroot has no kernel UAPI headers (its `include/asm` is a broken
  symlink), so the build adds `-idirafter $PREFIX/include` to borrow Termux's —
  UAPI headers are libc-independent, and `-idirafter` searches last so glibc's
  own headers still win;
- Termux's default `ld` is `lld`, which rejects the `--fix-cortex-a53-835769`
  flag the glibc gcc passes, so the build puts the glibc `bin` first on `PATH`
  to get GNU `ld`.

## Environment overrides

| Variable | Default | Effect |
| --- | --- | --- |
| `APP_PREFIX` | `/data/data/com.termux/files/usr` | Termux prefix |
| `GLIBC_PREFIX` | `$APP_PREFIX/glibc` | where `glibc-runner` lives |
| `CLAUDE_TERMUX_HOME` | `~/.local/share/claude-code-termux` | manager copy location |
| `CLAUDE_DNS_SHIM` | `$APP_PREFIX/lib/claude-dns-shim.so` | shim path |
| `CLAUDE_NATIVE` | *(baked into launcher)* | binary the launcher runs |
| `CLAUDE_DNS_SHIM_DEBUG` | unset | shim logs to stderr when set |

## Troubleshooting

**`claude: command not found`** — `~/.local/bin` isn't on `PATH`. See Install.

**`getaddrinfo ENOTIMP`** — the shim isn't loaded. `claude-code-termux doctor`,
then `build-shim`.

**`-G: cannot open shared object file`** from `grep` or `find` inside a session
— Claude Code's shell snapshot re-invokes `$CLAUDE_CODE_EXECPATH` with a custom
`argv[0]` to select its embedded tools. The installer builds a tiny Bionic
launcher that preserves that tool name while invoking Termux's glibc loader,
so tool flags reach ugrep/bfs instead of being parsed by the dynamic loader.

**Version drift after `npm update`** — `claude-code-termux update`.

**`claude` was overwritten by something else** — `install` adopts a foreign
`~/.local/bin/claude` but warns first. `uninstall` refuses to remove a launcher
it doesn't recognise.

## Uninstall

```sh
claude-code-termux uninstall           # binary, launcher, shim, manager copy
claude-code-termux uninstall --purge   # the above + npm uninstall -g
```

Neither touches `~/.claude/` or `~/.claude.json`; remove those by hand if you
want your config gone too.

## FAQ

**Does Claude Code work on Android?** Yes, through this. There's no official
Android build, but the Linux arm64 binary runs fine on a phone or tablet under
Termux once you give it a glibc loader.

**Do I need root?** No. No root, no Magisk, no unlocked bootloader.

**Do I need proot-distro, Ubuntu, or a chroot?** No. Those work too, but they
cost you a second filesystem and a slower shell. This runs the binary directly
in your Termux environment, so `claude` sees your real files, your real
`$HOME`, and your normal `pkg`-installed tools.

**Will this work on a Chromebook / Android tablet / Samsung DeX?** Anything that
runs Termux on arm64 or x86_64 should be fine.

**Is this safe? What is it downloading?** Anthropic's own published npm tarball
(`@anthropic-ai/claude-code-linux-arm64`), fetched with `npm pack` from the
public registry. Nothing is patched or repackaged. The only original binary
component is `shim_dns.c`, which is in this repo and compiled on your device.
To build the shim you need `gcc-glibc` — note that `glibc-runner` does NOT
pull it in as a dependency:

```sh
pkg install gcc-glibc
```

**Does it work with a Claude Pro/Max subscription and with API keys?** Yes —
it's the stock binary, so authentication behaves exactly as it does on desktop.

**Why does it need glibc?** Android uses Bionic as its libc; the binary is
linked against glibc. `glibc-runner` provides a glibc sysroot and loader, and
the launcher invokes the binary through `ld-linux-aarch64.so.1`.

**Will it survive `npm update -g @anthropic-ai/claude-code`?** An update
restores the broken stub, but the launcher notices and re-installs the real
binary on the next run. `claude-code-termux update` does it eagerly.

## Caveats

- Unofficial. Not affiliated with or supported by Anthropic. It downloads
  Anthropic's published binaries and runs them; it doesn't patch or repackage
  anything.
- Running glibc binaries under Termux costs a little startup latency and some
  disk for `glibc-runner`.
- If Anthropic ever publishes the android platform package, none of this is
  needed — `npm install -g @anthropic-ai/claude-code` will just work.

## License

MIT — see [LICENSE](LICENSE).

---

<sub>Keywords: Claude Code Android, Claude Code Termux, install Claude Code on
Android, Claude Code arm64, Anthropic Claude CLI Android, claude-code-linux-arm64-android
404, Claude Code without root, Termux glibc-runner, Claude Code getaddrinfo
ENOTIMP, AI coding agent on Android.</sub>
