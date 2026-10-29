#!/usr/bin/env bash
# Builds astc-encoder (CLI + static lib), the rltex library, the rltex CLI and the C tests,
# all out of tree under <repo>/build (gitignored). Upstream sources are only read.
#
# Environment:
#   RLTEX_EXTERNAL   directory holding bcdec/ and astc-encoder/ (default: <repo>/external, or,
#                    when that submodule checkout is empty (git worktree), the main checkout's)
#   CC / CXX         default clang / clang++
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
BUILD="$ROOT/build"

find_external() {
	if [[ -n "${RLTEX_EXTERNAL:-}" ]]; then echo "$RLTEX_EXTERNAL"; return; fi
	if [[ -f "$ROOT/external/bcdec/bcdec.h" && -f "$ROOT/external/astc-encoder/Source/astcenc.h" ]]; then
		echo "$ROOT/external"; return
	fi
	# git worktree: submodules are populated in the main checkout only
	local common
	common="$(git -C "$ROOT" rev-parse --git-common-dir 2>/dev/null || true)"
	if [[ -n "$common" ]]; then
		common="$(cd "$ROOT" && cd "$common" && pwd)"
		local main="${common%/.git}"
		if [[ -f "$main/external/bcdec/bcdec.h" ]]; then echo "$main/external"; return; fi
	fi
	echo "cannot find external/bcdec and external/astc-encoder; set RLTEX_EXTERNAL" >&2
	exit 1
}

EXT="$(find_external)"
export CC="${CC:-clang}" CXX="${CXX:-clang++}"
echo "[build] external sources: $EXT"

ASTC_LIB="$BUILD/astcenc/Source/libastcenc-avx2-static.a"
ASTC_CLI="$BUILD/astcenc/Source/astcenc-avx2"
if [[ ! -f "$ASTC_LIB" || ! -x "$ASTC_CLI" ]]; then
	echo "[build] astc-encoder (avx2 + sse4.1)"
	cmake -G Ninja -S "$EXT/astc-encoder" -B "$BUILD/astcenc" -DCMAKE_BUILD_TYPE=Release \
		-DASTCENC_ISA_AVX2=ON -DASTCENC_ISA_SSE41=ON -DASTCENC_CLI=ON -DASTCENC_WERROR=OFF \
		> "$BUILD/astcenc-configure.log" 2>&1
	cmake --build "$BUILD/astcenc" > "$BUILD/astcenc-build.log" 2>&1
fi

# astc-encoder release builds use LTO (bitcode objects); lld links them with clang.
echo "[build] rltex + tests (clang -Wall -Wextra -Werror)"
cmake -G Ninja -S "$HERE" -B "$BUILD/textures" -DCMAKE_BUILD_TYPE=Release \
	-DRLTEX_BCDEC_DIR="$EXT/bcdec" -DRLTEX_ASTCENC_INCLUDE="$EXT/astc-encoder/Source" \
	-DRLTEX_ASTCENC_LIB="$ASTC_LIB" \
	-DCMAKE_EXE_LINKER_FLAGS=-fuse-ld=lld > "$BUILD/textures-configure.log" 2>&1
cmake --build "$BUILD/textures"

echo "[build] rltex CLI: $BUILD/textures/rltex/rltex"
echo "[build] astcenc CLI: $ASTC_CLI"
