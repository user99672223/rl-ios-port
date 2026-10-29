#!/usr/bin/env bash
# Runs the C tests under AddressSanitizer+UBSan and under ThreadSanitizer (clang).
# Needs a prior tests/textures/build.sh (astc-encoder is linked unsanitized).
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
BUILD="$ROOT/build"
mkdir -p "$BUILD/tmp"
export TMPDIR="$BUILD/tmp" CC=clang

EXT="$(grep -m1 '^RLTEX_BCDEC_DIR' "$BUILD/textures/CMakeCache.txt" | cut -d= -f2)"
EXT="$(dirname "$EXT")"
for san in address,undefined thread; do
	dir="$BUILD/textures-san-${san//,/-}"
	echo "=== -fsanitize=$san"
	cmake -G Ninja -S "$HERE" -B "$dir" -DCMAKE_BUILD_TYPE=Debug \
		-DCMAKE_C_FLAGS="-fsanitize=$san -fno-omit-frame-pointer -O1" \
		-DCMAKE_EXE_LINKER_FLAGS="-fsanitize=$san -fuse-ld=lld" \
		-DRLTEX_BCDEC_DIR="$EXT/bcdec" -DRLTEX_ASTCENC_INCLUDE="$EXT/astc-encoder/Source" \
		-DRLTEX_ASTCENC_LIB="$BUILD/astcenc/Source/libastcenc-avx2-static.a" > "$dir.log" 2>&1
	cmake --build "$dir" > "$dir.build.log" 2>&1 || { cat "$dir.build.log"; exit 1; }
	export UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 ASAN_OPTIONS=detect_leaks=1
	"$dir/test_core"
	"$dir/test_cache"
done
echo "SANITIZERS CLEAN"
