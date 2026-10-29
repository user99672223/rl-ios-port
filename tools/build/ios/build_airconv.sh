#!/usr/bin/env bash
# Builds DXMT's DXBC->AIR translator (airconv) for iOS arm64, following DXMT's own recipe
# (docs/DEVELOPMENT.md: LLVM llvmorg-15.0.7, no targets, no tools, assertions on) but
# cross-compiled for arm64-apple-ios. macOS with Xcode only.
#   build_airconv.sh llvm      -> $OUT/llvm-ios (LLVM static libs + headers for iOS)
#   build_airconv.sh airconv   -> $OUT/airconv/libairconv.a (+ libDXBCParser objects)
# Env: OUT (default build/ios-deps), IOS_MIN (17.0)
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/../../.." && pwd)
OUT=${OUT:-$ROOT/build/ios-deps}; IOS_MIN=${IOS_MIN:-17.0}
LLVM_TAG=llvmorg-15.0.7
SDK=$(xcrun --sdk iphoneos --show-sdk-path)
DXMT="$ROOT/external/dxmt"
mkdir -p "$OUT"

build_llvm() {
  local src="$OUT/llvm-project"
  [ -d "$src/llvm" ] || git clone -q --depth 1 --branch "$LLVM_TAG" https://github.com/llvm/llvm-project.git "$src"
  # Host tablegen (the iOS build can't run its own).
  cmake -S "$src/llvm" -B "$OUT/llvm-host" -G Ninja -DCMAKE_BUILD_TYPE=Release \
    -DLLVM_TARGETS_TO_BUILD="" -DLLVM_ENABLE_ZSTD=Off -DLLVM_INCLUDE_TESTS=Off \
    -DLLVM_INCLUDE_EXAMPLES=Off -DLLVM_INCLUDE_BENCHMARKS=Off >/dev/null
  ninja -C "$OUT/llvm-host" llvm-tblgen
  cmake -S "$src/llvm" -B "$OUT/llvm-ios-build" -G Ninja \
    -DCMAKE_SYSTEM_NAME=iOS -DCMAKE_OSX_SYSROOT="$SDK" -DCMAKE_OSX_ARCHITECTURES=arm64 \
    -DCMAKE_OSX_DEPLOYMENT_TARGET="$IOS_MIN" -DLLVM_HOST_TRIPLE=arm64-apple-ios \
    -DLLVM_TABLEGEN="$OUT/llvm-host/bin/llvm-tblgen" \
    -DCMAKE_INSTALL_PREFIX="$OUT/llvm-ios" -DCMAKE_BUILD_TYPE=Release \
    -DLLVM_ENABLE_ASSERTIONS=On -DLLVM_TARGETS_TO_BUILD="" \
    -DLLVM_BUILD_TOOLS=Off -DLLVM_INCLUDE_TOOLS=Off -DLLVM_INCLUDE_UTILS=Off \
    -DLLVM_INCLUDE_TESTS=Off -DLLVM_INCLUDE_EXAMPLES=Off -DLLVM_INCLUDE_BENCHMARKS=Off \
    -DLLVM_INCLUDE_DOCS=Off -DLLVM_ENABLE_ZSTD=Off -DLLVM_ENABLE_ZLIB=Off \
    -DLLVM_ENABLE_LIBXML2=Off -DLLVM_ENABLE_TERMINFO=Off -DLLVM_ENABLE_LIBEDIT=Off \
    -DLLVM_VERSION_PRINTER_SHOW_HOST_TARGET_INFO=Off
  cmake --build "$OUT/llvm-ios-build"
  cmake --install "$OUT/llvm-ios-build"
  echo "LLVM iOS libs: $(find "$OUT/llvm-ios/lib" -name 'libLLVM*.a' | wc -l)"
}

build_airconv() {
  local gen="$OUT/airconv/gen" obj="$OUT/airconv/obj"
  rm -rf "$OUT/airconv"; mkdir -p "$gen" "$obj"
  # Helper shaders: same as DXMT's metalir_generator + hexdump_generator, iOS AIR target.
  for m in air_msad air_samplepos air_tessellation; do
    xcrun -sdk iphoneos metal -std=metal3.1 --target=air64-apple-ios"$IOS_MIN" \
      -c "$DXMT/src/airconv/shaders/$m.metal" -o "$gen/$m.air"
    (cd "$gen" && xxd -n "$m" -i "$m.air" "$m.h")
  done
  local cxx=(xcrun --sdk iphoneos clang++ -target "arm64-apple-ios$IOS_MIN" -isysroot "$SDK"
    -std=c++20 -O2 -fno-exceptions -funwind-tables -fno-rtti -Wno-extern-c-compat -Wno-unknown-pragmas
    -D_FILE_OFFSET_BITS=64 -D__STDC_CONSTANT_MACROS -D__STDC_FORMAT_MACROS -D__STDC_LIMIT_MACROS
    -I"$DXMT/include" -I"$DXMT/libs" -I"$DXMT/include/native/windows" -I"$DXMT/include/native/directx"
    -I"$DXMT/src/airconv" -I"$OUT/llvm-ios/include" -I"$gen")
  local srcs=(airconv_context air_type air_signature air_operations dxbc_converter dxbc_converter_gs
    dxbc_converter_ts dxbc_converter_basicblock dxbc_converter_cfg dxbc_instructions dxbc_signature
    metallib_writer nt/air_builder nt/dxbc_converter_base transforms/lower_16bit_texread
    transforms/simdgroup_implicit_membarrier)
  for s in "${srcs[@]}"; do
    "${cxx[@]}" -c "$DXMT/src/airconv/$s.cpp" -o "$obj/$(echo "$s" | tr / _).o"
  done
  for s in DXBCUtils BlobContainer ShaderBinary; do
    "${cxx[@]}" -c "$DXMT/libs/DXBCParser/$s.cpp" -o "$obj/dxbc_$s.o"
  done
  xcrun --sdk iphoneos libtool -static -o "$OUT/airconv/libairconv.a" "$obj"/*.o
  echo "built $OUT/airconv/libairconv.a"
}

case "${1:?llvm|airconv}" in
  llvm) build_llvm ;;
  airconv) build_airconv ;;
  *) echo "usage: $0 llvm|airconv" >&2; exit 2 ;;
esac
