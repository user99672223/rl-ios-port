#!/usr/bin/env bash
# Build FEX's ARM64EC emulator DLL (libarm64ecfex.dll) with llvm-mingw, using FEX's own CI recipe:
# external/fex/.github/workflows/wine_build/action.yml (cmake flags) and
# .github/workflows/mingw_build.yml (toolchain_mingw.cmake + MINGW_TRIPLE). Runs on Linux or macOS:
# it is a cross-build whose product is a PE DLL.
#
# Output: $FEX_DLL_DIR/libarm64ecfex.dll
. "$(dirname "$0")/lib.sh"
toolchain_path

[ -f "$FEX_SRC/CMakeLists.txt" ] || die "no FEX source at $FEX_SRC"
for d in External/vixl Source/Common/cpp-optparse External/fmt External/xxhash External/range-v3 \
         External/unordered_dense External/rpmalloc External/jemalloc_glibc External/zydis External/Vulkan-Headers; do
  [ -n "$(ls -A "$FEX_SRC/$d" 2>/dev/null)" ] || die "FEX submodule $d is empty (git submodule update --init --depth 1 in external/fex)"
done

group "configure FEX (arm64ec-w64-mingw32)"
rm -rf "$FEX_BUILD"
# Flags copied from external/fex/.github/workflows/wine_build/action.yml (target arm64ec, Release).
# FEX_TEB_IN_DARWIN_TSD: ARM64EC code reads the TEB from pthread TSD slot 6, not x18, which the Darwin
# kernel does not preserve (patches/fex/0300, patches/wine/0002). Set FEX_TEB_TSD=0 for stock behaviour.
TEB_DEF=""; [ "${FEX_TEB_TSD:-1}" = 1 ] && TEB_DEF="-DFEX_TEB_IN_DARWIN_TSD"
cmake -S "$FEX_SRC" -B "$FEX_BUILD" -G Ninja \
  -DCMAKE_C_FLAGS="$TEB_DEF" -DCMAKE_CXX_FLAGS="$TEB_DEF" -DCMAKE_ASM_FLAGS="$TEB_DEF" \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_TOOLCHAIN_FILE="$FEX_SRC/Data/CMake/toolchain_mingw.cmake" \
  -DMINGW_TRIPLE=arm64ec-w64-mingw32 \
  -DCMAKE_INSTALL_LIBDIR=/usr/lib/wine/aarch64-windows \
  -DENABLE_LTO=False -DENABLE_ASSERTIONS=False -DENABLE_JEMALLOC_GLIBC_ALLOC=False \
  -DBUILD_TESTING=False -DCMAKE_INSTALL_PREFIX=/usr \
  -DTUNE_ARCH=generic -DTUNE_CPU=none -DRANGES_NATIVE=OFF \
  ${FEX_CMAKE_EXTRA:-} 2>&1 | tee "$LOG_DIR/fex-configure.log" | tail -5
endgroup

group "build arm64ecfex"
ninja -C "$FEX_BUILD" arm64ecfex 2>&1 | tee "$LOG_DIR/fex-build.log" | tail -5
endgroup

mkdir -p "$FEX_DLL_DIR"
cp "$FEX_BUILD/Bin/libarm64ecfex.dll" "$FEX_DLL_DIR/"
llvm-readobj --file-headers "$FEX_DLL_DIR/libarm64ecfex.dll" | grep -E "Machine|SectionAlignment" || true
ls -l "$FEX_DLL_DIR/libarm64ecfex.dll"
echo "sha256 $(sha256_of "$FEX_DLL_DIR/libarm64ecfex.dll")"
