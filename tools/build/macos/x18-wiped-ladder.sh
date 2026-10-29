#!/usr/bin/env bash
# The ladder again, with x18 wiped by the kernel as on iOS.
# build-wine.sh stamps the loader executables with LC_BUILD_VERSION sdk=12.0, which makes xnu
# preserve x18 for the task (osfmk/arm64/machine_task.c: macOS && sdk < 13.0). iOS has no such
# exception, so this re-stamps them with the SDK actually used, restoring the default behaviour
# (x18 zeroed on context switch / signal return), and reruns the ladder. With patches/wine/0002 and
# patches/fex/0300 the results must match the stamped run. Logs go to $LOG_DIR/x18-wiped/.
set -euo pipefail
# shellcheck source=tools/build/macos/lib.sh
. "$(dirname "$0")/lib.sh"
[ "$IS_DARWIN" = 1 ] || die "macOS only"
sdk=$(xcrun --sdk macosx --show-sdk-version)
group "re-stamp loader executables with sdk $sdk (x18 no longer preserved)"
for f in "$WINE_PREFIX_DIR"/bin/wine "$WINE_PREFIX_DIR"/lib/wine/*-unix/wine "$WINE_PREFIX_DIR"/bin/wineserver; do
  if [ ! -f "$f" ] || [ -L "$f" ]; then continue; fi
  vtool -set-build-version macos 12.0 "$sdk" -replace -output "$f.vtool" "$f"
  mv "$f.vtool" "$f"
  codesign -s - --force "$f"
  echo "$f: $(otool -l "$f" | grep -A4 LC_BUILD_VERSION | grep -E 'minos|sdk' | tr -s ' ' | tr '\n' ' ')"
done
endgroup
wineserver -k 2>/dev/null || true
LOG_DIR="$LOG_DIR/x18-wiped" "$(dirname "$0")/run-ladder.sh"
