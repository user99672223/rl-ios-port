#!/usr/bin/env bash
# Copy game data, the Wine prefix and caches into the app's Documents folder over USB.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=../lib/env.sh disable=SC1091
. "$SCRIPT_DIR/../lib/env.sh"
# shellcheck source=lib.sh disable=SC1091
. "$SCRIPT_DIR/lib.sh"
export RL_TAG=push

usage() {
  cat <<EOF
Usage: push_game.sh [options]

Copies to the app's Documents folder with rsync over an ifuse mount
(ifuse --documents \$RLPORT_BUNDLE_ID). Safe to re-run: files whose size already
matches on the phone are skipped, so an interrupted copy resumes.

  game      \$GAME_DIR                  -> Documents/game/           (read-only source; never written)
  prefix    \$WORK_DIR/prefix/          -> Documents/prefix/
  textures  \$WORK_DIR/textures/out/    -> Documents/cache/textures/
  aot       \$WORK_DIR/aot/cache/       -> Documents/<FEX cache dir>/cache/   (TODO: lead owns the path)

Options:
  --only LIST         comma-separated subset of: game,prefix,textures,aot (repeatable).
                      Default: all four; missing prefix/textures/aot sources are skipped with a
                      warning. With --only, a missing source is an error.
  --dry-run, -n       show what would be copied and the space needed; change nothing on the phone
  --skip-space-check  do not compare against free space on the phone
  --append-partial    rsync --append-verify: resume a half-copied file by appending after checksumming
                      the part already on the phone. Faster than re-sending the huge .tfc/.upk files.
  --bundle-id ID      override RLPORT_BUNDLE_ID
  -h, --help          this help

Environment:
  RLPORT_BUNDLE_ID          app bundle id (default dev.rlport.app). Set it to whatever id your signing
                            tool gave the installed app.
  RLPORT_UDID               pick one device when several are attached
  GAME_DIR, WORK_DIR        from tools/lib/env.sh
  RLPORT_GAME_EXCLUDES      newline- or colon-separated paths, relative to GAME_DIR, that are not copied.
                            Default (D-005, the runtime starts RocketLeague.exe directly):
                              Binaries/Win64/RocketLeague_EAC.exe
                              Binaries/Win64/EasyAntiCheat/
                              Binaries/Win64/Launcher.exe
  RLPORT_MIN_FREE_MARGIN_MB spare space to keep on the phone after the copy (default 2048)
  RLPORT_RSYNC_LINKS=1      also copy symlinks as symlinks (default: skipped; never followed)
  RLPORT_DEV_*_DEST         destination paths under Documents (see tools/device/lib.sh)
  RLPORT_FAKE_DEVICE_DIR    use a local directory instead of a phone (tests)

Exit status is non-zero when no device or app is found, free space is insufficient, or rsync fails.
EOF
}

ONLY=""
DRY=0
SKIP_SPACE=0
APPEND=0
while [ $# -gt 0 ]; do
  case "$1" in
    --only) [ $# -ge 2 ] || rl_die "--only needs a value"; ONLY="${ONLY:+$ONLY,}$2"; shift 2 ;;
    --only=*) ONLY="${ONLY:+$ONLY,}${1#--only=}"; shift ;;
    --dry-run|-n) DRY=1; shift ;;
    --skip-space-check) SKIP_SPACE=1; shift ;;
    --append-partial) APPEND=1; shift ;;
    --bundle-id) [ $# -ge 2 ] || rl_die "--bundle-id needs a value"; RLPORT_BUNDLE_ID="$2"; shift 2 ;;
    -h|--help) usage; exit 0 ;;
    *) usage >&2; rl_die "unknown argument: $1" ;;
  esac
done

export RLPORT_BUNDLE_ID
declare -A SRC DEST
SRC[game]="$GAME_DIR";                DEST[game]="$RLPORT_DEV_GAME_DEST"
SRC[prefix]="$WORK_DIR/prefix";       DEST[prefix]="$RLPORT_DEV_PREFIX_DEST"
SRC[textures]="$WORK_DIR/textures/out"; DEST[textures]="$RLPORT_DEV_TEXTURES_DIR"
SRC[aot]="$WORK_DIR/aot/cache";       DEST[aot]="$RLPORT_DEV_AOT_DEST"
ALL_SETS=(game prefix textures aot)

SETS=()
EXPLICIT=0
if [ -n "$ONLY" ]; then
  EXPLICIT=1
  IFS=',' read -r -a req <<<"$ONLY"
  for s in "${req[@]}"; do
    [ -n "$s" ] || continue
    [ -n "${SRC[$s]:-}" ] || rl_die "unknown set '$s' (valid: game,prefix,textures,aot)"
    SETS+=("$s")
  done
else
  SETS=("${ALL_SETS[@]}")
fi
[ "${#SETS[@]}" -gt 0 ] || rl_die "no sets selected"

rl_need_cmd rsync "apt install rsync"

# Drop sets whose source is missing (or fail when they were asked for explicitly).
SELECTED=()
for s in "${SETS[@]}"; do
  if [ ! -d "${SRC[$s]}" ]; then
    if [ "$s" = game ] || [ "$EXPLICIT" = 1 ]; then
      rl_die "source for '$s' not found: ${SRC[$s]}"
    fi
    rl_warn "skipping '$s': ${SRC[$s]} does not exist"
    continue
  fi
  SELECTED+=("$s")
done
[ "${#SELECTED[@]}" -gt 0 ] || rl_die "nothing to push"

# Exclusions for the game tree.
DEFAULT_EXCLUDES="Binaries/Win64/RocketLeague_EAC.exe
Binaries/Win64/EasyAntiCheat/
Binaries/Win64/Launcher.exe"
EXCLUDES_RAW="${RLPORT_GAME_EXCLUDES:-$DEFAULT_EXCLUDES}"
TMP_EXC="$(mktemp "${TMPDIR:-/tmp}/rlport-excl.XXXXXX")"
printf '%s\n' "$EXCLUDES_RAW" | tr ':' '\n' | sed -e 's#^/*##' -e '/^[[:space:]]*$/d' -e 's#^#/#' >"$TMP_EXC"

cleanup() { rm -f "$TMP_EXC"; rl_unmount; }
trap cleanup EXIT
trap 'exit 130' INT
trap 'exit 143' TERM

rl_require_device_and_app
rl_mount

rl_rsync_flags
RSYNC=(rsync "${RL_RSYNC_FLAGS[@]}")
if [ "$APPEND" = 1 ]; then RSYNC+=(--append-verify); fi

run_rsync() { # set, extra args...
  local s="$1"; shift
  local extra=()
  if [ "$s" = game ]; then extra=(--exclude-from="$TMP_EXC"); fi
  # Trailing slashes: copy the directory's contents, and anchor exclude patterns at its root.
  LC_ALL=C "${RSYNC[@]}" "${extra[@]}" "$@" "${SRC[$s]%/}/" "$RL_DOCS/${DEST[$s]%/}/"
}

# ---- plan: what would be copied, and how much space it needs ----
rl_log "plan (bundle id $RLPORT_BUNDLE_ID):"
declare -A PLAN_FILES
TOTAL=0
for s in "${SELECTED[@]}"; do
  out="$(run_rsync "$s" --dry-run --stats 2>&1)" || { printf '%s\n' "$out" | tail -n 20 >&2; rl_die "rsync planning failed for '$s'"; }
  b="$(printf '%s\n' "$out" | rl_stat_bytes)"; f="$(printf '%s\n' "$out" | rl_stat_files)"
  PLAN_FILES[$s]="${f:-0}"
  TOTAL=$((TOTAL + ${b:-0}))
  rl_log "  $s: ${f:-0} file(s), $(rl_fmt_bytes "${b:-0}") to copy  (${SRC[$s]} -> Documents/${DEST[$s]})"
done
if grep -q . "$TMP_EXC" && printf '%s\n' "${SELECTED[@]}" | grep -qx game; then
  rl_log "  game excludes: $(tr '\n' ' ' <"$TMP_EXC")"
fi
rl_log "total to copy: $(rl_fmt_bytes "$TOTAL")"

# ---- free-space check ----
if [ "$SKIP_SPACE" = 1 ]; then
  rl_warn "space check skipped (--skip-space-check)"
else
  FREE="$(rl_free_bytes)"
  if ! [[ "$FREE" =~ ^[0-9]+$ ]]; then
    rl_die "could not read free space (got '${FREE}') from: ideviceinfo -q com.apple.disk_usage -k AmountDataAvailable. Unlock the phone, or rerun with --skip-space-check."
  fi
  MARGIN=$(( ${RLPORT_MIN_FREE_MARGIN_MB:-2048} * 1024 * 1024 ))
  rl_log "free on phone: $(rl_fmt_bytes "$FREE"); needed: $(rl_fmt_bytes "$TOTAL") + margin $(rl_fmt_bytes "$MARGIN")"
  if [ $((TOTAL + MARGIN)) -gt "$FREE" ]; then
    rl_die "not enough space on the phone: need $(rl_fmt_bytes $((TOTAL + MARGIN))) incl. margin, have $(rl_fmt_bytes "$FREE"). Free space (or lower RLPORT_MIN_FREE_MARGIN_MB) and re-run; finished files are kept."
  fi
fi

if [ "$DRY" = 1 ]; then
  rl_log "dry run: nothing was changed on the phone"
  exit 0
fi

# ---- copy ----
for s in "${SELECTED[@]}"; do
  if [ "${PLAN_FILES[$s]}" = 0 ]; then
    rl_log "$s: already up to date"
    continue
  fi
  rl_log "copying $s ..."
  prog=()
  if [ -t 1 ]; then prog=(--info=progress2); fi
  if ! run_rsync "$s" "${prog[@]}" >"${TMPDIR:-/tmp}/rlport-push-$s.out" 2>&1; then
    tail -n 20 "${TMPDIR:-/tmp}/rlport-push-$s.out" >&2 || true
    rl_die "rsync failed for '$s'. Re-run the same command to resume."
  fi
  skipped="$(grep -c 'skipping non-regular file' "${TMPDIR:-/tmp}/rlport-push-$s.out" || true)"
  if [ "${skipped:-0}" -gt 0 ]; then rl_log "$s: $skipped symlink(s) skipped (set RLPORT_RSYNC_LINKS=1 to copy them)"; fi
  rm -f "${TMPDIR:-/tmp}/rlport-push-$s.out"
  rl_log "$s: done"
done
rl_log "push complete"
