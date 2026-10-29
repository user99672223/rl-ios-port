#!/usr/bin/env bash
# Build CPython for iOS (PEP 730): Python.xcframework with a device slice (arm64) and a
# simulator slice (arm64 + x86_64).  macOS + full Xcode only.
#
# This is CPython's own procedure, unchanged: `python Apple build iOS`
# (external/cpython/Apple/iOS/README.md:56-69, Apple/__main__.py:751-782).  It
#   1. configures and builds a CPython for the build machine,
#   2. downloads prebuilt iOS binary dependencies (BZip2, libFFI, OpenSSL, XZ, mpdecimal,
#      zstd) from github.com/beeware/cpython-apple-source-deps (Apple/__main__.py:327-341),
#   3. builds Python.framework for arm64-apple-ios, arm64-apple-ios-simulator and
#      x86_64-apple-ios-simulator (HOSTS, Apple/__main__.py:69-77),
#   4. merges them into Python.xcframework and a .tar.gz
#      (create_xcframework / package, Apple/__main__.py:522-748).
# `Apple/__main__.py package` runs `Apple/testbed` by a path relative to the cwd
# (line 721-730), so it must be started from the CPython checkout; build products go to
# an out-of-tree --cross-build-dir so the submodule stays clean.
#
# Usage:   tools/cpython/build_ios.sh [--clean]
# Env:     PYTHON           python >= 3.10 used to run Apple/__main__.py (default: python3)
#          CPYTHON_IOS_OUT  where the result is copied (default: build/cpython-ios)
#          CACHE_DIR        download cache for the iOS dependency archives (read by Apple/__main__.py)
#          DEVELOPER_DIR    Xcode to use; IOS_SDK_VERSION to pin the SDK (Apple/iOS/README.md:150-158)
# Output:  $CPYTHON_IOS_OUT/Python.xcframework and python-<version>-iOS-XCframework.tar.gz
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
CPYTHON_SRC="${CPYTHON_SRC:-$REPO_ROOT/external/cpython}"
OUT="${CPYTHON_IOS_OUT:-$REPO_ROOT/build/cpython-ios}"
PYTHON="${PYTHON:-python3}"
CROSS_BUILD_DIR="$OUT/cross-build"

die() { echo "error: $*" >&2; exit 1; }

# ---- preconditions --------------------------------------------------------------------
[ "$(uname -s)" = "Darwin" ] || die "this script builds Python for iOS and only runs on macOS (found $(uname -s)); use the cpython-ios GitHub workflow on a macOS runner"
command -v xcodebuild >/dev/null || die "xcodebuild not found: install full Xcode (Command Line Tools are not enough; Apple/iOS/README.md:16-21)"
command -v xcrun >/dev/null || die "xcrun not found"
xcrun --sdk iphoneos --show-sdk-path >/dev/null 2>&1 || die "iphoneos SDK not found: select a full Xcode with xcode-select -s or DEVELOPER_DIR"
xcrun --sdk iphonesimulator --show-sdk-path >/dev/null 2>&1 || die "iphonesimulator SDK not found: install the iOS Simulator platform in Xcode"
command -v "$PYTHON" >/dev/null || die "$PYTHON not found"
"$PYTHON" -c 'import sys; sys.exit(0 if sys.version_info >= (3, 10) else 1)' \
  || die "$PYTHON is $("$PYTHON" -V 2>&1); Apple/__main__.py needs Python >= 3.10"
for tool in make curl lipo plutil; do command -v "$tool" >/dev/null || die "$tool not found"; done
[ -f "$CPYTHON_SRC/Apple/__main__.py" ] \
  || die "$CPYTHON_SRC/Apple/__main__.py missing (git submodule update --init external/cpython)"

PYVER="$(sed -n 's/^#define PY_VERSION[[:space:]]*"\(.*\)"/\1/p' "$CPYTHON_SRC/Include/patchlevel.h")"
echo "CPython source: $CPYTHON_SRC (version $PYVER)"
echo "Xcode: $(xcodebuild -version | tr '\n' ' ')"
echo "Output: $OUT"

clean_flag=()
[ "${1:-}" = "--clean" ] && clean_flag=(--clean)

# ---- build ----------------------------------------------------------------------------
mkdir -p "$OUT"
cd "$CPYTHON_SRC"          # `package` invokes Apple/testbed via a relative path
"$PYTHON" Apple build iOS --cross-build-dir "$CROSS_BUILD_DIR" ${clean_flag[@]+"${clean_flag[@]}"}

# ---- verify what was produced ---------------------------------------------------------
XCF="$CROSS_BUILD_DIR/iOS/Python.xcframework"
[ -d "$XCF" ] || die "expected $XCF was not produced"

DEVICE="$XCF/ios-arm64/Python.framework/Python"
SIM="$XCF/ios-arm64_x86_64-simulator/Python.framework/Python"
[ -f "$DEVICE" ] || die "device slice missing: $DEVICE"
[ -f "$SIM" ] || die "simulator slice missing: $SIM"

echo "--- slices"
plutil -p "$XCF/Info.plist" | grep -E 'LibraryIdentifier|SupportedPlatform|SupportedArchitectures' || true
echo "device    : $(lipo -info "$DEVICE")"
echo "simulator : $(lipo -info "$SIM")"
lipo -info "$DEVICE" | grep -q 'arm64$' || die "device slice is not arm64"
lipo -info "$SIM" | grep -q 'x86_64' || die "simulator slice lacks x86_64"
lipo -info "$SIM" | grep -q 'arm64' || die "simulator slice lacks arm64"
if command -v vtool >/dev/null; then
  vtool -show-build "$DEVICE" | grep -q 'platform IOS$' || die "device slice is not built for the iOS platform"
  vtool -show-build "$SIM" | grep -q 'platform IOSSIMULATOR' || die "simulator slice is not built for the iOS simulator platform"
fi
STDLIB="$(find "$XCF/lib" -maxdepth 1 -type d -name 'python3.*' | head -1)"
[ -n "$STDLIB" ] || die "stdlib missing from $XCF/lib"
[ -f "$XCF/build/utils.sh" ] || die "$XCF/build/utils.sh missing (Xcode build-phase helpers)"
echo "stdlib    : $STDLIB"

# ---- publish --------------------------------------------------------------------------
rm -rf "$OUT/Python.xcframework"
cp -R "$XCF" "$OUT/Python.xcframework"
cp "$CROSS_BUILD_DIR"/dist/python-*-iOS-XCframework.tar.gz "$OUT/"
echo "done: $OUT/Python.xcframework"
echo "Xcode: add it under 'Frameworks, Libraries and Embedded Content' (Embed & Sign) and run"
echo "       'install_python Python.xcframework app app_packages' in a Run Script phase"
echo "       (Doc/using/ios.rst, 'Adding Python to an iOS project', steps 5-7)."
