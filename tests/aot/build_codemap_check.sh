#!/usr/bin/env bash
# Builds build/codemap_check against an existing FEX build tree (default build/fex-sim).
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
F="$ROOT/external/fex"; B="${1:-$ROOT/build/fex-sim}"
mapfile -t LIBS < <(find "$B" -name '*.a' ! -name 'libLinuxEmulation.a')
clang++ -std=c++20 -O1 -DXXH_INLINE_ALL -I"$F/FEXCore/include" -I"$B/FEXCore/include" \
  -I"$F/External/fmt/include" -I"$F/External/xxhash" -I"$F/External/robin-map/include" \
  -I"$F/External/unordered_dense/include" "$ROOT/tests/aot/codemap_check.cpp" \
  -Wl,--start-group "${LIBS[@]}" -Wl,--end-group -lpthread -ldl -o "$ROOT/build/codemap_check"
echo "built $ROOT/build/codemap_check"
