#!/usr/bin/env bash
# Regenerates vectors.h and checks it against the project's decoder (rltex / bcdec):
# the Python reference that supplies the test's expected colours must agree with bcdec.
#   check_vectors.sh [out dir]
set -euo pipefail
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
root="$(cd "$here/../../.." && pwd)"
out="${1:-${TMPDIR:-/tmp}/bc_check_vectors}"
mkdir -p "$out"
python3 "$here/gen_vectors.py" > "$out/vectors.h"
if ! cmp -s "$out/vectors.h" "$here/vectors.h"; then
  echo "vectors.h is out of date: run gen_vectors.py > vectors.h" >&2
  exit 1
fi
src="$root/runtime/textures/src"
cc="${CC:-gcc}"
"$cc" -O1 -Wall -I"$root/runtime/textures/include" -I"$src" -I"$root/external/bcdec" -I"$here" \
  "$here/check_vectors.c" "$src"/{sha256,format,bc_decode,config,fs,astc_encode,ctx,platform}.c \
  -lpthread -lm -o "$out/check_vectors"
"$out/check_vectors"
