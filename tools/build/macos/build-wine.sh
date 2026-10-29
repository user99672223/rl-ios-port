#!/usr/bin/env bash
# Configure, build and install Wine with ARM64X (aarch64 + arm64ec) PE modules.
#
#   build-wine.sh                     build $WINE_SRC (default external/wine, MR 11638 + patches/wine)
#   WINE_SRC=/path build-wine.sh      build another tree (approach (b): upstream master)
#
# Facts this relies on (all read in the pinned tree, Wine 11.15 base):
#  * --enable-archs={i386,x86_64,arm,aarch64,arm64ec,none}   configure.ac:19
#  * arm64ec implies an x86_64 PE build too                    configure.ac:417-421 (extra_arch=x86_64)
#  * aarch64+arm64ec together produce ARM64X (-marm64x) PE     tools/makedep.c:5149-5154
#  * PE compilers are looked up as <arch>-w64-mingw32-clang    configure.ac:445-451 (hence PATH=llvm-mingw/bin)
#  * the ARM64EC emulator is loaded via xtajit64.dll by default and is enabled for arm64ec
#                                                              configure.ac:2449-2452
. "$(dirname "$0")/lib.sh"
toolchain_path

[ -f "$WINE_SRC/configure" ] || die "no Wine source at $WINE_SRC (git submodule update --init --depth 1 external/wine)"
command -v bison >/dev/null && bison --version | head -1
command -v flex  >/dev/null && flex --version | head -1

# ccache for the host compiler and for the three PE compilers, via compiler-name symlinks
# (ccache masquerading: the symlink dir must come first in PATH, real compilers later in PATH).
if command -v ccache >/dev/null 2>&1; then
  shim=$WORK/ccache-shim; rm -rf "$shim"; mkdir -p "$shim"
  ccache_bin=$(command -v ccache)
  for t in x86_64 aarch64 arm64ec; do ln -s "$ccache_bin" "$shim/$t-w64-mingw32-clang"; done
  export PATH="$shim:$PATH"
  export CCACHE_DIR=${CCACHE_DIR:-$WORK/ccache}
  export CCACHE_MAXSIZE=${CCACHE_MAXSIZE:-3G}
  export CCACHE_COMPILERCHECK=content
  CC_HOST="ccache clang"
  ccache --zero-stats >/dev/null
else
  CC_HOST="clang"
fi

mkdir -p "$WINE_BUILD"; cd "$WINE_BUILD"

if [ ! -f Makefile ] || [ "${FORCE_CONFIGURE:-0}" = 1 ]; then
  group "configure wine ($WINE_SRC)"
  extra=()
  if [ "$IS_DARWIN" = 1 ]; then
    # Keep the dependency set fixed so cached results do not depend on what Homebrew has installed.
    extra+=(--without-x --without-freetype --without-gnutls --without-sdl --without-vulkan --without-opengl
            --without-gstreamer --without-pcap --without-cups --without-gphoto --without-sane --without-krb5
            --without-gssapi --without-opencl --without-usb --without-v4l2 --without-pulse --without-fontconfig
            --without-ffmpeg --without-dbus --without-capi --without-pcsclite --without-wayland --without-hwloc
            --without-netapi)
  fi
  CC="$CC_HOST" \
  CFLAGS="${WINE_HOST_CFLAGS:--O2 -g1}" \
  CROSSCFLAGS="${WINE_PE_CFLAGS:--O2}" \
  "$WINE_SRC/configure" \
      --prefix="$WINE_PREFIX_DIR" \
      --enable-archs=arm64ec,aarch64 \
      --disable-win16 \
      "${extra[@]}" ${WINE_CONFIGURE_EXTRA:-} 2>&1 | tee "$LOG_DIR/wine-configure.log"
  endgroup
fi

group "make wine"
# -k: keep going so one run reports every failing directory, not just the first.
set +e
make -k -j"$NCPU" 2>&1 | tee "$LOG_DIR/wine-make.log"
rc=${PIPESTATUS[0]}
set -e
endgroup
[ "$IS_DARWIN" = 1 ] || true
command -v ccache >/dev/null 2>&1 && ccache --show-stats || true
if [ "$rc" -ne 0 ]; then
  grep -nE "error:|Error [0-9]+|\*\*\*" "$LOG_DIR/wine-make.log" | head -60 || true
  die "make failed (rc=$rc); see $LOG_DIR/wine-make.log"
fi

group "make install"
make install 2>&1 | tail -5 | tee "$LOG_DIR/wine-install.log"
endgroup
ls "$WINE_PREFIX_DIR/bin" "$WINE_PREFIX_DIR/lib/wine" | head -40
