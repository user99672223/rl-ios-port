#!/usr/bin/env bash
# One-command coverage loop: pull -> AOT -> shaders -> textures -> push caches -> package IPA.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=../lib/env.sh disable=SC1091
. "$SCRIPT_DIR/../lib/env.sh"
# shellcheck source=lib.sh disable=SC1091
. "$SCRIPT_DIR/lib.sh"
export RL_TAG=loop

usage() {
  cat <<EOF
Usage: loop.sh [options]

Runs the coverage loop after a play session on the phone. Stages, in order:

  1 pull      tools/device/pull_logs.sh                       (phone -> work/device/<ts>, latest symlink)
  2 aot       python3 aot/rlaot.py build --codemaps work/device/latest/codemaps --out work/aot
  3 shaders   python3 tools/shaders/build_bundle.py --in work/device/latest/dxbc --out work/shaders
  4 textures  python3 tools/textures/encode.py --in work/device/latest/textures_bc --out work/textures/out
  5 push      tools/device/push_game.sh --only textures,aot   (needs the phone; fake dir in tests)
  6 package   tools/package_ipa.sh                            (IPA with the new shader bundle -> out/RLPort.ipa)

A stage whose tool does not exist yet, or whose input is empty, is skipped with a message.
A stage that exists and fails stops the loop (set -e).

Options:
  --skip-pull --skip-aot --skip-shaders --skip-textures --skip-push --skip-package
  --skip LIST          comma-separated stage names to skip, e.g. --skip aot,package
  --syslog SECONDS     pass --syslog SECONDS to pull_logs.sh
  --dry-run            print the stages and commands without running them
  -h, --help           this help

Environment: WORK_DIR, RLPORT_BUNDLE_ID, RLPORT_UDID, RLPORT_FAKE_DEVICE_DIR (see lib.sh).
  Tool locations (defaults relative to the repo; override for tests):
  RLPORT_LOOP_PULL, RLPORT_LOOP_AOT, RLPORT_LOOP_SHADERS, RLPORT_LOOP_TEXTURES,
  RLPORT_LOOP_PUSH, RLPORT_LOOP_PACKAGE, RLPORT_LOOP_PYTHON (default python3).

Log: \$WORK_DIR/logs/loop-<UTC timestamp>.log
EOF
}

declare -A SKIP=()
PULL_EXTRA=()
DRY=0
while [ $# -gt 0 ]; do
  case "$1" in
    --skip-pull) SKIP[pull]=1; shift ;;
    --skip-aot) SKIP[aot]=1; shift ;;
    --skip-shaders) SKIP[shaders]=1; shift ;;
    --skip-textures) SKIP[textures]=1; shift ;;
    --skip-push) SKIP[push]=1; shift ;;
    --skip-package) SKIP[package]=1; shift ;;
    --skip) [ $# -ge 2 ] || rl_die "--skip needs a list"
            IFS=',' read -r -a _l <<<"$2"
            for x in "${_l[@]}"; do
              case "$x" in pull|aot|shaders|textures|push|package) SKIP[$x]=1 ;; *) rl_die "unknown stage '$x'" ;; esac
            done; shift 2 ;;
    --syslog) [ $# -ge 2 ] || rl_die "--syslog needs SECONDS"; PULL_EXTRA=(--syslog "$2"); shift 2 ;;
    --dry-run) DRY=1; shift ;;
    -h|--help) usage; exit 0 ;;
    *) usage >&2; rl_die "unknown argument: $1" ;;
  esac
done

: "${RLPORT_LOOP_PYTHON:=python3}"
: "${RLPORT_LOOP_PULL:=$SCRIPT_DIR/pull_logs.sh}"
: "${RLPORT_LOOP_PUSH:=$SCRIPT_DIR/push_game.sh}"
: "${RLPORT_LOOP_AOT:=$REPO_ROOT/aot/rlaot.py}"
: "${RLPORT_LOOP_SHADERS:=$REPO_ROOT/tools/shaders/build_bundle.py}"
: "${RLPORT_LOOP_TEXTURES:=$REPO_ROOT/tools/textures/encode.py}"
: "${RLPORT_LOOP_PACKAGE:=$REPO_ROOT/tools/package_ipa.sh}"

mkdir -p "$WORK_DIR/logs"
LOG="$WORK_DIR/logs/loop-$(date -u +%Y%m%dT%H%M%SZ).log"
# Everything printed from here on goes to the terminal and the log file.
exec > >(tee -a "$LOG") 2>&1
TEE_PID=$!
rl_log "loop starting; log: $LOG"

LATEST="$WORK_DIR/device/latest"
RESULTS=()
FAILED_STAGE=""
on_exit() {
  local rc=$?
  if [ -n "$FAILED_STAGE" ]; then rl_warn "stage '$FAILED_STAGE' failed (exit $rc); see $LOG"; fi
  rl_log "summary:"; for r in "${RESULTS[@]}"; do rl_log "  $r"; done
  # Close our stdout so tee finishes writing the log before we exit.
  exec >&- 2>&-
  wait "$TEE_PID" 2>/dev/null || true
  exit "$rc"
}
trap on_exit EXIT

# run_stage <name> <description> <command...>
run_stage() {
  local name="$1" desc="$2"; shift 2
  rl_log "=== stage: $name - $desc"
  if [ "$DRY" = 1 ]; then rl_log "dry run: $*"; RESULTS+=("$name: dry run"); return 0; fi
  FAILED_STAGE="$name"
  "$@"
  FAILED_STAGE=""
  RESULTS+=("$name: ok")
}
skip_stage() { rl_log "=== stage: $1 - SKIPPED: $2"; RESULTS+=("$1: skipped ($2)"); }

need_latest() { # <stage>
  [ -e "$LATEST" ] || rl_die "stage '$1' needs $LATEST; run without --skip-pull (or run pull_logs.sh) first"
}
dir_nonempty() { [ -d "$1" ] && [ -n "$(find "$1" -type f -print -quit 2>/dev/null)" ]; }

# --- 1 pull
if [ -n "${SKIP[pull]:-}" ]; then skip_stage pull "--skip-pull"
elif [ ! -x "$RLPORT_LOOP_PULL" ]; then skip_stage pull "tool missing: $RLPORT_LOOP_PULL"
else run_stage pull "phone -> work/device" "$RLPORT_LOOP_PULL" "${PULL_EXTRA[@]}"; fi

# --- 2 aot
if [ -n "${SKIP[aot]:-}" ]; then skip_stage aot "--skip-aot"
elif [ ! -f "$RLPORT_LOOP_AOT" ]; then skip_stage aot "tool missing: $RLPORT_LOOP_AOT (written by the lead)"
else
  [ "$DRY" = 1 ] || need_latest aot
  if [ "$DRY" != 1 ] && ! dir_nonempty "$LATEST/codemaps"; then
    skip_stage aot "no code maps in $LATEST/codemaps"
  else
    run_stage aot "FEX cache from code maps" "$RLPORT_LOOP_PYTHON" "$RLPORT_LOOP_AOT" build \
      --codemaps "$LATEST/codemaps" --out "$WORK_DIR/aot"
  fi
fi

# --- 3 shaders
if [ -n "${SKIP[shaders]:-}" ]; then skip_stage shaders "--skip-shaders"
elif [ ! -f "$RLPORT_LOOP_SHADERS" ]; then skip_stage shaders "tool missing: $RLPORT_LOOP_SHADERS (not written yet)"
else
  [ "$DRY" = 1 ] || need_latest shaders
  if [ "$DRY" != 1 ] && ! dir_nonempty "$LATEST/dxbc"; then
    skip_stage shaders "no DXBC blobs in $LATEST/dxbc"
  else
    run_stage shaders "shader bundle from DXBC blobs" "$RLPORT_LOOP_PYTHON" "$RLPORT_LOOP_SHADERS" \
      --in "$LATEST/dxbc" --out "$WORK_DIR/shaders"
  fi
fi

# --- 4 textures
if [ -n "${SKIP[textures]:-}" ]; then skip_stage textures "--skip-textures"
elif [ ! -f "$RLPORT_LOOP_TEXTURES" ]; then skip_stage textures "tool missing: $RLPORT_LOOP_TEXTURES (not written yet)"
else
  [ "$DRY" = 1 ] || need_latest textures
  if [ "$DRY" != 1 ] && ! dir_nonempty "$LATEST/textures_bc"; then
    skip_stage textures "no BC blobs in $LATEST/textures_bc"
  else
    run_stage textures "BC -> ASTC" "$RLPORT_LOOP_PYTHON" "$RLPORT_LOOP_TEXTURES" \
      --in "$LATEST/textures_bc" --out "$WORK_DIR/textures/out"
  fi
fi

# --- 5 push caches
if [ -n "${SKIP[push]:-}" ]; then skip_stage push "--skip-push"
elif [ ! -x "$RLPORT_LOOP_PUSH" ]; then skip_stage push "tool missing: $RLPORT_LOOP_PUSH"
else run_stage push "caches -> phone" "$RLPORT_LOOP_PUSH" --only textures,aot; fi

# --- 6 package
if [ -n "${SKIP[package]:-}" ]; then skip_stage package "--skip-package"
elif [ ! -f "$RLPORT_LOOP_PACKAGE" ]; then skip_stage package "tool missing: $RLPORT_LOOP_PACKAGE (not written yet)"
else run_stage package "build out/RLPort.ipa" bash "$RLPORT_LOOP_PACKAGE"; fi

rl_log "loop finished"
