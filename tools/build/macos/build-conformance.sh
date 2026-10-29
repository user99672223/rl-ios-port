#!/usr/bin/env bash
# Build Wine's conformance test executables for the DLLs in the subset as plain x86-64 PE programs (so that they
# run through FEX, not natively), and stage them in $CONF_BIN_DIR.
#
# Why a second configure: the ARM64X build (build-wine.sh) only builds tests as ARM64X executables
# (dlls/<dll>/tests/aarch64-windows/<dll>_test.exe, seen in the make log); the x86_64 arch there is just the
# arm64ec_x64 helper objects. A second, x86_64-only PE configuration reuses the first build's tools
# (--with-wine-tools, documented in configure --help) and builds only the test targets.
#
# Naming (tools/makedep.c:3762 and 4117): dlls/<dll>/tests/<arch>-windows/<dll>_test.exe
. "$(dirname "$0")/lib.sh"
toolchain_path
CONF_BIN_DIR=${CONF_BIN_DIR:-$WORK/conformance-bin}
CONF_BUILD=${CONF_BUILD:-$WORK/wine-build-x64}
mkdir -p "$CONF_BIN_DIR" "$CONF_BUILD"
[ -x "$WINE_BUILD/tools/winebuild/winebuild" ] || die "need the main Wine build tree at $WINE_BUILD (tools/winebuild)"

HOST_CLANG=clang; [ "$IS_DARWIN" = 1 ] && HOST_CLANG=/usr/bin/clang
if command -v ccache >/dev/null 2>&1; then
  shim=$WORK/ccache-shim-x64; rm -rf "$shim"; mkdir -p "$shim"
  ln -s "$(command -v ccache)" "$shim/x86_64-w64-mingw32-clang"
  export PATH="$shim:$PATH" CCACHE_DIR=${CCACHE_DIR:-$WORK/ccache} CCACHE_COMPILERCHECK=content
  HOST_CC="ccache $HOST_CLANG"
else
  HOST_CC="$HOST_CLANG"
fi

cd "$CONF_BUILD"
if [ ! -f Makefile ]; then
  group "configure x86_64-only PE tree"
  CC="$HOST_CC" CFLAGS="-O1" CROSSCFLAGS="${WINE_PE_CFLAGS:--O2}" \
  "$WINE_SRC/configure" --enable-archs=x86_64 --with-wine-tools="$WINE_BUILD" --disable-win16 \
      --without-x --without-freetype --without-gnutls --without-sdl --without-vulkan --without-opengl \
      --without-gstreamer --without-pcap --without-cups --without-gphoto --without-sane --without-krb5 \
      --without-gssapi --without-opencl --without-usb --without-v4l2 --without-pulse --without-fontconfig \
      --without-ffmpeg --without-dbus --without-capi --without-pcsclite --without-wayland --without-hwloc \
      --without-netapi 2>&1 | tee "$LOG_DIR/wine-x64-configure.log" | tail -5
  endgroup
fi

for dll in ${CONFORMANCE_DLLS:-kernel32 ntdll ws2_32 mmdevapi xinput1_3}; do
  t="dlls/$dll/tests/x86_64-windows/${dll}_test.exe"
  if make -j"$NCPU" "$t" > "$LOG_DIR/conformance-build-$dll.log" 2>&1; then
    cp "$t" "$CONF_BIN_DIR/${dll}_test.exe"
    echo "CONF build $dll: ok ($(wc -c < "$CONF_BIN_DIR/${dll}_test.exe" | tr -d ' ') bytes) $(llvm-readobj --file-headers "$t" | grep -m1 Machine)"
  else
    echo "CONF build $dll: FAILED"; tail -8 "$LOG_DIR/conformance-build-$dll.log"
  fi
done
