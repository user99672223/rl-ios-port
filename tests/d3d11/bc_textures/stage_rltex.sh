#!/usr/bin/env bash
# Stage the project's texture library into a DXMT checkout so that DXMT's meson build
# (src/winemetal/unix/meson.build, patches/dxmt/0100) links it into winemetal.so.
#   stage_rltex.sh <repo root> <dxmt dir>
# Copies runtime/textures/{include/rltex.h,src/*} and external/bcdec/bcdec.h (read only inputs)
# into <dxmt dir>/src/winemetal/unix/rltex/. The destination is not part of any patch.
set -euo pipefail
repo=${1:?usage: stage_rltex.sh <repo root> <dxmt dir>}
dxmt=${2:?usage: stage_rltex.sh <repo root> <dxmt dir>}
dst=$dxmt/src/winemetal/unix/rltex
mkdir -p "$dst"
cp "$repo/runtime/textures/include/rltex.h" "$dst/"
cp "$repo"/runtime/textures/src/*.c "$repo"/runtime/textures/src/*.h "$dst/"
cp "$repo/external/bcdec/bcdec.h" "$dst/"
echo "staged rltex into $dst:"
ls "$dst"
