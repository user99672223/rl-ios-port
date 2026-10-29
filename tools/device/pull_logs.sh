#!/usr/bin/env bash
# Pull logs, captured DXBC/BC blobs and FEX code maps from the phone into work/device/<UTC timestamp>/.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=../lib/env.sh disable=SC1091
. "$SCRIPT_DIR/../lib/env.sh"
# shellcheck source=lib.sh disable=SC1091
. "$SCRIPT_DIR/lib.sh"
export RL_TAG=pull

usage() {
  cat <<EOF
Usage: pull_logs.sh [options]

Copies, read-only, from the app's Documents (ifuse --documents \$RLPORT_BUNDLE_ID) into
\$WORK_DIR/device/<UTC timestamp>/ and points \$WORK_DIR/device/latest at it:

  Documents/logs/*                       -> <dir>/logs/       cpu_miss.log, cpu_profile.log,
                                                              faults.log, frametimes.csv, ...
  Documents/cache/shaders/dxbc/          -> <dir>/dxbc/       input of tools/shaders/build_bundle.py
  Documents/cache/textures/bc/           -> <dir>/textures_bc/  input of tools/textures/encode.py
  <FEX cache dir>/codemap/new/           -> <dir>/codemaps/new/    FEX code maps written while playing
  <FEX cache dir>/codemap/ready/         -> <dir>/codemaps/ready/  (aggregated maps, if the phone made them)

The code-map layout mirrors FEX: new maps are <Name>-<fileid:016x>.<time>.bin
(Source/Windows/Common/ImageTracker.cpp:158-164), aggregated ones <Name>-<fileid:016x>
(Source/Tools/FEXOfflineCompiler/Main.cpp:731). Nothing on the phone is changed or deleted.

Options:
  --syslog SECONDS   before pulling, capture idevicesyslog for SECONDS, filtered to the app's process
                     name, into <dir>/syslog.txt. Start the game on the phone while it runs.
  --syslog-only SECONDS  only capture the syslog (no ifuse mount, no file pull)
  --process NAME     process name for the syslog filter (RLPORT_PROCESS_NAME, default RLPort)
  --no-link-dest     do not hard-link unchanged files against the previous pull (default: link)
  --bundle-id ID     override RLPORT_BUNDLE_ID
  -h, --help         this help

Environment: RLPORT_BUNDLE_ID (default dev.rlport.app), RLPORT_UDID, WORK_DIR,
  RLPORT_PROCESS_NAME, RLPORT_DEV_FEX_CACHE_DIR (TODO: lead owns the FEX cache path),
  RLPORT_FAKE_DEVICE_DIR (use a local directory instead of a phone),
  RLPORT_FAKE_SYSLOG_FILE (fake mode: a file whose lines stand in for idevicesyslog output).
EOF
}

SYSLOG_SECS=0
SYSLOG_ONLY=0
LINK=1
: "${RLPORT_PROCESS_NAME:=RLPort}"
while [ $# -gt 0 ]; do
  case "$1" in
    --syslog) [ $# -ge 2 ] || rl_die "--syslog needs SECONDS"; SYSLOG_SECS="$2"; shift 2 ;;
    --syslog-only) [ $# -ge 2 ] || rl_die "--syslog-only needs SECONDS"; SYSLOG_SECS="$2"; SYSLOG_ONLY=1; shift 2 ;;
    --process) [ $# -ge 2 ] || rl_die "--process needs a value"; RLPORT_PROCESS_NAME="$2"; shift 2 ;;
    --no-link-dest) LINK=0; shift ;;
    --bundle-id) [ $# -ge 2 ] || rl_die "--bundle-id needs a value"; RLPORT_BUNDLE_ID="$2"; shift 2 ;;
    -h|--help) usage; exit 0 ;;
    *) usage >&2; rl_die "unknown argument: $1" ;;
  esac
done
export RLPORT_BUNDLE_ID
[[ "$SYSLOG_SECS" =~ ^[0-9]+$ ]] || rl_die "SECONDS must be a whole number, got '$SYSLOG_SECS'"

DEVICE_ROOT="$WORK_DIR/device"
TS="$(date -u +%Y%m%dT%H%M%SZ)"
OUT="$DEVICE_ROOT/$TS"
PREV=""
if [ -L "$DEVICE_ROOT/latest" ]; then PREV="$(readlink -f "$DEVICE_ROOT/latest" || true)"; fi

# Guard: the output must stay under WORK_DIR and never inside the game directory.
case "$(readlink -m "$OUT")" in
  "$(readlink -m "$GAME_DIR")"/*) rl_die "refusing to write inside GAME_DIR" ;;
esac

capture_syslog() {
  local dest="$1"
  rl_log "capturing syslog for ${SYSLOG_SECS}s, process filter '$RLPORT_PROCESS_NAME' -> $dest"
  if rl_fake; then
    # shellcheck disable=SC2154
    if [ -n "${RLPORT_FAKE_SYSLOG_FILE:-}" ] && [ -f "$RLPORT_FAKE_SYSLOG_FILE" ]; then
      grep -F -- "$RLPORT_PROCESS_NAME" "$RLPORT_FAKE_SYSLOG_FILE" >"$dest" || true
    else
      : >"$dest"
    fi
    return 0
  fi
  rl_need_cmd idevicesyslog "apt install libimobiledevice-utils"
  local udid_args=()
  mapfile -t udid_args < <(rl_udid_args)
  # Flags from `idevicesyslog --help`: -u UDID, -p PROCESS, -o FILE. SIGINT ends it cleanly.
  timeout -s INT "$SYSLOG_SECS" idevicesyslog "${udid_args[@]}" --no-colors -p "$RLPORT_PROCESS_NAME" -o "$dest" || {
    rc=$?
    # 124 = timeout expired, which is how we expect it to end.
    [ "$rc" = 124 ] || [ "$rc" = 130 ] || rl_warn "idevicesyslog exited with status $rc"
  }
}

if [ "$SYSLOG_ONLY" = 1 ]; then
  rl_require_device_and_app
  mkdir -p "$OUT"
  capture_syslog "$OUT/syslog.txt"
  rl_log "syslog saved: $OUT/syslog.txt"
  exit 0
fi

trap 'rl_unmount' EXIT
trap 'exit 130' INT
trap 'exit 143' TERM

rl_require_device_and_app
mkdir -p "$OUT"
if [ "$SYSLOG_SECS" -gt 0 ]; then capture_syslog "$OUT/syslog.txt"; fi
rl_mount
rl_need_cmd rsync "apt install rsync"

rl_rsync_flags
pull_dir() { # <device-relative dir> <local subdir> <label>
  local rel="${1%/}" sub="$2" label="$3" src dst
  src="$RL_DOCS/$rel"
  dst="$OUT/$sub"
  if [ ! -d "$src" ]; then
    rl_log "$label: not on the phone yet (Documents/$rel)"
    return 0
  fi
  mkdir -p "$dst"
  local ld=()
  if [ "$LINK" = 1 ] && [ -n "$PREV" ] && [ -d "$PREV/$sub" ]; then ld=(--link-dest="$PREV/$sub"); fi
  # Read-only on the phone side: the phone path is only ever a source.
  LC_ALL=C rsync "${RL_RSYNC_FLAGS[@]}" "${ld[@]}" "$src/" "$dst/" >/dev/null
  local n b
  n="$(find "$dst" -type f | wc -l)"
  b="$(du -sb "$dst" | cut -f1)"
  rl_log "$label: $n file(s), $(rl_fmt_bytes "$b")  -> $dst"
}

# Plain log files live directly in Documents/logs; copy the whole directory.
pull_dir "$RLPORT_DEV_LOGS_DIR"        logs         "logs"
pull_dir "$RLPORT_DEV_DXBC_DIR"        dxbc         "DXBC blobs"
pull_dir "$RLPORT_DEV_TEXTURES_BC_DIR" textures_bc  "BC texture blobs"
pull_dir "$RLPORT_DEV_CODEMAP_NEW"     codemaps/new   "FEX code maps (new)"
pull_dir "$RLPORT_DEV_CODEMAP_READY"   codemaps/ready "FEX code maps (ready)"

# The loop's rlaot stage wants the directory to exist even if the phone had no code maps yet.
mkdir -p "$OUT/codemaps"

# Update the 'latest' symlink atomically (relative target, so the work/ tree can move).
ln -sfn "$TS" "$DEVICE_ROOT/latest.tmp.$$"
mv -T "$DEVICE_ROOT/latest.tmp.$$" "$DEVICE_ROOT/latest"
rl_log "pulled to $OUT (work/device/latest -> $TS)"

# Quick summary of what the logs say, to point at the next step.
if [ -f "$OUT/logs/cpu_miss.log" ]; then
  rl_log "cpu_miss.log: $(wc -l <"$OUT/logs/cpu_miss.log") line(s)"
fi
if [ -s "$OUT/logs/faults.log" ]; then
  rl_warn "faults.log is not empty ($(wc -l <"$OUT/logs/faults.log") line(s)): read it first"
fi
