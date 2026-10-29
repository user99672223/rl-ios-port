# shellcheck shell=bash
# Shared helpers for tools/device/*.sh. Source it, do not run it.
#
#   SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
#   . "$SCRIPT_DIR/../lib/env.sh"
#   . "$SCRIPT_DIR/lib.sh"
#
# Environment (all optional):
#   RLPORT_BUNDLE_ID         app bundle id (default dev.rlport.app); must match the id the owner's
#                            signing tool gives the installed app
#   RLPORT_UDID              target one device when several are attached
#   RLPORT_FAKE_DEVICE_DIR   fake-device mode: this directory plays the app's Documents folder;
#                            no ifuse, idevice_id or ideviceinfo is run (used by tests/device/run.sh)
#   RLPORT_FAKE_NO_DEVICE=1  (fake mode) behave as if no device is attached
#   RLPORT_FAKE_NO_APP=1     (fake mode) behave as if the app is not installed
#   RLPORT_FAKE_FREE_BYTES   (fake mode) pretend this many bytes are free (default: df of the dir)
#
# Where the app keeps things, relative to its Documents folder. Spec section 6.4/6.5/6.9.
#   RLPORT_DEV_FEX_CACHE_DIR is FEX's cache directory (FEX_APP_CACHE_LOCATION, FEX source
#   Source/Common/Config.cpp:711-716). FEX's Windows code appends "codemap/new/" for code maps
#   (Source/Windows/Common/ImageTracker.cpp:158) and reads/writes "cache/" for compiled blocks
#   (ImageTracker.cpp:238, Source/Tools/FEXOfflineCompiler/Main.cpp:527).
#   TODO(lead): the lead owns the runtime's FEX cache location. Change this one default (or set
#   the variable) when the runtime settles on a directory; every device script follows.
: "${RLPORT_BUNDLE_ID:=dev.rlport.app}"
: "${RLPORT_DEV_FEX_CACHE_DIR:=cache/fex}"
: "${RLPORT_DEV_LOGS_DIR:=logs}"
: "${RLPORT_DEV_DXBC_DIR:=cache/shaders/dxbc}"
: "${RLPORT_DEV_TEXTURES_DIR:=cache/textures}"
: "${RLPORT_DEV_TEXTURES_BC_DIR:=cache/textures/bc}"
: "${RLPORT_DEV_GAME_DEST:=game}"
: "${RLPORT_DEV_PREFIX_DEST:=prefix}"
: "${RLPORT_DEV_AOT_DEST:=$RLPORT_DEV_FEX_CACHE_DIR/cache}"
: "${RLPORT_DEV_CODEMAP_NEW:=$RLPORT_DEV_FEX_CACHE_DIR/codemap/new}"
: "${RLPORT_DEV_CODEMAP_READY:=$RLPORT_DEV_FEX_CACHE_DIR/codemap/ready}"
export RLPORT_BUNDLE_ID RLPORT_DEV_FEX_CACHE_DIR RLPORT_DEV_LOGS_DIR RLPORT_DEV_DXBC_DIR \
  RLPORT_DEV_TEXTURES_DIR RLPORT_DEV_TEXTURES_BC_DIR RLPORT_DEV_GAME_DEST RLPORT_DEV_PREFIX_DEST \
  RLPORT_DEV_AOT_DEST RLPORT_DEV_CODEMAP_NEW RLPORT_DEV_CODEMAP_READY

# shellcheck disable=SC2034  # RL_TAG is read by the log helpers
RL_TAG="${RL_TAG:-device}"
RL_DOCS=""          # root of the app's Documents folder (mount point, or the fake dir)
RL_MOUNTED=0        # 1 when this process mounted it with ifuse
RL_MOUNTPOINT=""    # temp mount point we created

rl_ts() { date -u +%Y-%m-%dT%H:%M:%SZ; }
rl_log() { printf '%s [%s] %s\n' "$(rl_ts)" "${RL_TAG:-device}" "$*"; }
rl_warn() { printf '%s [%s] WARNING: %s\n' "$(rl_ts)" "${RL_TAG:-device}" "$*" >&2; }
rl_die() { printf '%s [%s] ERROR: %s\n' "$(rl_ts)" "${RL_TAG:-device}" "$*" >&2; exit 1; }

rl_fake() { [ -n "${RLPORT_FAKE_DEVICE_DIR:-}" ]; }

rl_need_cmd() {
  command -v "$1" >/dev/null 2>&1 || rl_die "required command not found: $1 (${2:-install it and retry})"
}

# Human-readable bytes.
rl_fmt_bytes() {
  awk -v b="$1" 'BEGIN{
    split("B KiB MiB GiB TiB", u, " "); i=1; while (b>=1024 && i<5) { b/=1024; i++ }
    printf (i==1 ? "%d %s" : "%.2f %s"), b, u[i] }'
}

# Print attached UDIDs, one per line (empty when none). idevice_id --list is documented in
# `idevice_id --help`.
rl_device_ids() {
  if rl_fake; then
    [ "${RLPORT_FAKE_NO_DEVICE:-0}" = 1 ] || echo "FAKE-UDID-0000"
    return 0
  fi
  idevice_id -l 2>/dev/null || true
}

rl_udid_args() {
  if [ -n "${RLPORT_UDID:-}" ]; then printf '%s\n' -u "$RLPORT_UDID"; fi
}

# Exit with a clear message unless a device (and the app) is available.
rl_require_device_and_app() {
  local ids
  if ! rl_fake; then
    rl_need_cmd idevice_id "apt install libimobiledevice-utils"
    rl_need_cmd ifuse "apt install ifuse"
    rl_need_cmd ideviceinfo "apt install libimobiledevice-utils"
  fi
  ids="$(rl_device_ids)"
  if [ -z "$ids" ]; then
    rl_die "no iOS device found. Connect the iPhone over USB, unlock it, tap Trust, and check 'idevice_id -l' lists it. (usbmuxd must be running.)"
  fi
  if [ -n "${RLPORT_UDID:-}" ] && ! printf '%s\n' "$ids" | grep -qxF "$RLPORT_UDID"; then
    rl_die "RLPORT_UDID=$RLPORT_UDID is not among attached devices: $(printf '%s' "$ids" | tr '\n' ' ')"
  fi
  if rl_fake; then
    [ -d "$RLPORT_FAKE_DEVICE_DIR" ] || rl_die "RLPORT_FAKE_DEVICE_DIR is not a directory: $RLPORT_FAKE_DEVICE_DIR"
    if [ "${RLPORT_FAKE_NO_APP:-0}" = 1 ]; then
      rl_die "app $RLPORT_BUNDLE_ID not found on the device (fake: RLPORT_FAKE_NO_APP=1)"
    fi
    return 0
  fi
  local udid_args=()
  mapfile -t udid_args < <(rl_udid_args)
  # `ifuse --list-apps` lists apps with file sharing enabled (see `ifuse --help`).
  # UNVERIFIED on hardware: its exact output format; we only look for the bundle id as a substring.
  if ! ifuse "${udid_args[@]}" --list-apps 2>/dev/null | grep -qF -- "$RLPORT_BUNDLE_ID"; then
    rl_die "app '$RLPORT_BUNDLE_ID' not found among the device's file-sharing apps. Check: the IPA is installed and has been opened once, its bundle id equals RLPORT_BUNDLE_ID (your signing tool may rewrite it), Info.plist has UIFileSharingEnabled, and the phone is unlocked. 'ifuse --list-apps' shows what the phone offers."
  fi
}

# Free bytes for user data on the device. Domain/key: `ideviceinfo -q com.apple.disk_usage
# -k AmountDataAvailable` (domain listed in `ideviceinfo --help`; key AmountDataAvailable is "number
# of free bytes for user storage", per community lockdownd schema hack-different/apple-knowledge
# _data/lockdownd.yaml and used by UFADE: lockdown.get_value("com.apple.disk_usage","AmountDataAvailable")).
rl_free_bytes() {
  if rl_fake; then
    if [ -n "${RLPORT_FAKE_FREE_BYTES:-}" ]; then echo "$RLPORT_FAKE_FREE_BYTES"; return 0; fi
    df -B1 --output=avail "$RLPORT_FAKE_DEVICE_DIR" | tail -n1 | tr -d ' '
    return 0
  fi
  local udid_args=() v
  mapfile -t udid_args < <(rl_udid_args)
  v="$(ideviceinfo "${udid_args[@]}" -q com.apple.disk_usage -k AmountDataAvailable 2>/dev/null | tr -d '[:space:]')" || true
  printf '%s\n' "$v"
}

# Mount (or select the fake dir) and set RL_DOCS. Call rl_install_cleanup first.
rl_mount() {
  if rl_fake; then
    RL_DOCS="$(cd "$RLPORT_FAKE_DEVICE_DIR" && pwd)"
    rl_log "fake device mode: Documents = $RL_DOCS"
    return 0
  fi
  local udid_args=()
  mapfile -t udid_args < <(rl_udid_args)
  RL_MOUNTPOINT="$(mktemp -d "${TMPDIR:-/tmp}/rlport-ifuse.XXXXXX")"
  rl_log "mounting Documents of $RLPORT_BUNDLE_ID at $RL_MOUNTPOINT"
  # Option order from `ifuse --help`: ifuse MOUNTPOINT [OPTIONS] with --documents APPID.
  if ! ifuse "$RL_MOUNTPOINT" "${udid_args[@]}" --documents "$RLPORT_BUNDLE_ID"; then
    rl_die "ifuse could not mount Documents of '$RLPORT_BUNDLE_ID'. Is the phone unlocked and trusted, is the app installed, and does it have UIFileSharingEnabled?"
  fi
  RL_MOUNTED=1
  mountpoint -q "$RL_MOUNTPOINT" || rl_die "ifuse returned success but $RL_MOUNTPOINT is not a mount point"
  RL_DOCS="$RL_MOUNTPOINT"
}

rl_unmount() {
  if [ "$RL_MOUNTED" = 1 ] && [ -n "$RL_MOUNTPOINT" ]; then
    sync || true
    local ok=0
    if command -v fusermount3 >/dev/null 2>&1 && fusermount3 -u "$RL_MOUNTPOINT" 2>/dev/null; then ok=1
    elif command -v fusermount >/dev/null 2>&1 && fusermount -u "$RL_MOUNTPOINT" 2>/dev/null; then ok=1
    elif umount "$RL_MOUNTPOINT" 2>/dev/null; then ok=1
    elif command -v fusermount >/dev/null 2>&1 && fusermount -uz "$RL_MOUNTPOINT" 2>/dev/null; then ok=1
    fi
    if [ "$ok" = 1 ]; then
      RL_MOUNTED=0
      rmdir "$RL_MOUNTPOINT" 2>/dev/null || true
    else
      rl_warn "could not unmount $RL_MOUNTPOINT; run: fusermount -u $RL_MOUNTPOINT"
    fi
  elif [ -n "$RL_MOUNTPOINT" ]; then
    rmdir "$RL_MOUNTPOINT" 2>/dev/null || true
  fi
}

# Always unmount, whatever way the script ends.
rl_install_cleanup() {
  trap 'rl_unmount' EXIT
  trap 'exit 130' INT
  trap 'exit 143' TERM
}

# rsync flags for AFC (ifuse) and plain destinations.
#   -r              recurse; symlinks are skipped (never -L: a Wine prefix has dosdevices/z: -> /)
#   --size-only     skip files whose size matches; AFC timestamps are not trusted
#   --inplace       no temp files and renames on the phone
#   --partial       keep partially transferred files
#   --mkpath        create missing destination directories (rsync >= 3.2.3; absent in a --dry-run)
#   --no-perms/owner/group/times   AFC ignores them
#   --chmod=Du+rwx,Fu+rw  new files/dirs stay writable even when the source (GAME_DIR) is read-only
# Flags confirmed with `rsync --help` (rsync 3.5.0 on the dev machine).
rl_rsync_flags() {
  RL_RSYNC_FLAGS=(-r --mkpath --size-only --inplace --partial --no-perms '--chmod=Du+rwx,Fu+rw' --no-owner --no-group --no-times --omit-dir-times)
  if [ "${RLPORT_RSYNC_LINKS:-0}" = 1 ]; then RL_RSYNC_FLAGS+=(--links); fi
}

# Parse "Total transferred file size: 1,234 bytes" out of rsync --stats output on stdin.
rl_stat_bytes() { sed -n 's/^Total transferred file size: \([0-9,.]*\) bytes.*/\1/p' | tr -d ',.' | tail -n1; }
rl_stat_files() { sed -n 's/^Number of regular files transferred: \([0-9,.]*\).*/\1/p' | tr -d ',.' | tail -n1; }
