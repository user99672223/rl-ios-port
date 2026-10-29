# Shared helpers for tools/build/macos/*.sh. Source, don't execute.
# shellcheck shell=bash
set -euo pipefail

MACOS_TOOLS_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
REPO_ROOT=${REPO_ROOT:-$(cd "$MACOS_TOOLS_DIR/../../.." && pwd)}
# shellcheck source=versions.env
. "$MACOS_TOOLS_DIR/versions.env"

WORK=${WORK:-$REPO_ROOT/work/macos}
TOOLCHAIN=${TOOLCHAIN:-$WORK/llvm-mingw}            # llvm-mingw root
WINE_SRC=${WINE_SRC:-$REPO_ROOT/external/wine}
FEX_SRC=${FEX_SRC:-$REPO_ROOT/external/fex}
WINE_BUILD=${WINE_BUILD:-$WORK/wine-build}
WINE_PREFIX_DIR=${WINE_PREFIX_DIR:-$WORK/wine-install}   # `make install` destination (--prefix)
FEX_BUILD=${FEX_BUILD:-$WORK/fex-build}
FEX_DLL_DIR=${FEX_DLL_DIR:-$WORK/fex-dll}                # where libarm64ecfex.dll is staged
SMOKE_DIR=${SMOKE_DIR:-$WORK/smoke}
LOG_DIR=${LOG_DIR:-$WORK/logs}
WINEPREFIX=${WINEPREFIX:-$WORK/wineprefix}
export WORK TOOLCHAIN WINE_SRC FEX_SRC WINE_BUILD WINE_PREFIX_DIR FEX_BUILD FEX_DLL_DIR SMOKE_DIR LOG_DIR WINEPREFIX

NCPU=$( (sysctl -n hw.ncpu 2>/dev/null || nproc 2>/dev/null || echo 2) | head -1)
IS_DARWIN=0; [ "$(uname -s)" = Darwin ] && IS_DARWIN=1

log()   { printf '[%s] %s\n' "$(date +%H:%M:%S)" "$*"; }
group() { if [ -n "${GITHUB_ACTIONS:-}" ]; then echo "::group::$*"; else log "== $*"; fi; }
endgroup() { if [ -n "${GITHUB_ACTIONS:-}" ]; then echo "::endgroup::"; fi; }
notice() { if [ -n "${GITHUB_ACTIONS:-}" ]; then echo "::notice::$*"; else log "NOTICE: $*"; fi; }
warn()   { if [ -n "${GITHUB_ACTIONS:-}" ]; then echo "::warning::$*"; else log "WARNING: $*"; fi; }
die()    { if [ -n "${GITHUB_ACTIONS:-}" ]; then echo "::error::$*"; else log "ERROR: $*"; fi; exit 1; }

mkdir -p "$WORK" "$LOG_DIR"

sha256_of() { if command -v sha256sum >/dev/null 2>&1; then sha256sum "$1" | cut -d' ' -f1; else shasum -a 256 "$1" | cut -d' ' -f1; fi; }

# Run a command with a wall-clock limit (macOS has no timeout(1)). Exit 124 on timeout.
# usage: run_timeout SECONDS cmd args...
run_timeout() {
  local secs=$1; shift
  local flag="$WORK/.timeout.$$.$RANDOM"
  rm -f "$flag"
  "$@" &
  local pid=$!
  ( sleep "$secs"; if kill -0 "$pid" 2>/dev/null; then : > "$flag"; kill -TERM "$pid" 2>/dev/null; sleep 5; kill -KILL "$pid" 2>/dev/null; fi ) &
  local watchdog=$!
  local rc=0
  wait "$pid" 2>/dev/null || rc=$?
  kill "$watchdog" 2>/dev/null || true
  wait "$watchdog" 2>/dev/null || true
  if [ -f "$flag" ]; then rm -f "$flag"; return 124; fi
  return "$rc"
}

toolchain_path() { export PATH="$TOOLCHAIN/bin:$PATH"; }
wine_env() {
  export PATH="$WINE_PREFIX_DIR/bin:$PATH"
  export WINEPREFIX
  export WINEDEBUG=${WINEDEBUG:-err+all,fixme-all}
  export WINEDLLOVERRIDES=${WINEDLLOVERRIDES:-mscoree,mshtml=}     # no Mono/Gecko prompts
}
