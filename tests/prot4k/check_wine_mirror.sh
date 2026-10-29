#!/usr/bin/env bash
# Verify the copy of prot4k.{h,c} embedded in patches/wine/0100-*.patch is byte-identical to
# runtime/prot4k/.  (Skipped, with a notice, while the patch does not exist yet.)
set -euo pipefail
cd "$(dirname "$0")/../.."
shopt -s nullglob
patch=(patches/wine/0100-*.patch)
if [ ${#patch[@]} -eq 0 ]; then echo "check_wine_mirror: no patches/wine/0100-*.patch yet, skipped"; exit 0; fi
tmp=$(mktemp -d); trap 'rm -rf "$tmp"' EXIT
rc=0
for f in prot4k.h prot4k.c; do
  # lines of the new file in the patch: strip the leading '+' of the hunk body
  awk -v want="b/dlls/ntdll/unix/$f" '
    $0 ~ "^diff --git " { infile = ($4 == want); inhunk = 0; next }
    infile && /^@@/ { inhunk = 1; next }
    infile && inhunk && /^\+/ { print substr($0, 2) }
  ' "${patch[0]}" > "$tmp/$f"
  if cmp -s "$tmp/$f" "runtime/prot4k/$f"; then echo "check_wine_mirror: $f identical"
  else echo "check_wine_mirror: $f DIFFERS from runtime/prot4k/$f"; diff "$tmp/$f" "runtime/prot4k/$f" | head -20; rc=1; fi
done
exit $rc
