#!/usr/bin/env bash
# Approach (c): fetch the prebuilt 3Shain "Wine 11.2 ARM64EC" tarball, verify its SHA-256, unpack it into
# $WINE_PREFIX_DIR and describe what is inside (architectures, drivers, ARM64EC DLLs). Nothing is executed here.
. "$(dirname "$0")/lib.sh"
toolchain_path
mkdir -p "$WORK/dl"
tb=$WORK/dl/wine3shain-$WINE3SHAIN_TAG.tar.gz
if [ ! -f "$tb" ]; then
  group "download $WINE3SHAIN_URL"
  curl -fL --retry 5 --retry-delay 5 -o "$tb.part" "$WINE3SHAIN_URL"; mv "$tb.part" "$tb"
  endgroup
fi
got=$(sha256_of "$tb"); echo "sha256 $got"
[ "$got" = "$WINE3SHAIN_SHA256" ] || die "SHA-256 mismatch: got $got expected $WINE3SHAIN_SHA256"

echo "---- tar listing (top levels) ----"
tar -tzf "$tb" | awk -F/ 'NF<=4' | head -80
rm -rf "$WINE_PREFIX_DIR"; mkdir -p "$WINE_PREFIX_DIR"
tar -xzf "$tb" --strip-components 2 -C "$WINE_PREFIX_DIR"
echo "---- layout ----"
ls "$WINE_PREFIX_DIR" "$WINE_PREFIX_DIR/bin" "$WINE_PREFIX_DIR/lib/wine" 2>&1 | head -40
echo "---- machine types ----"
for f in "$WINE_PREFIX_DIR"/bin/wine*; do [ -f "$f" ] && { printf '%s: ' "$f"; file "$f" | cut -d: -f2; }; done
for d in "$WINE_PREFIX_DIR"/lib/wine/*-windows; do
  [ -d "$d" ] || continue
  printf '%s: %s files; ntdll.dll: ' "$d" "$(ls "$d" | wc -l | tr -d ' ')"
  if [ -f "$d/ntdll.dll" ]; then llvm-readobj --file-headers "$d/ntdll.dll" | grep -m1 Machine; else echo "(none)"; fi
done
ls "$WINE_PREFIX_DIR"/lib/wine/*-unix 2>/dev/null | grep -E "ntdll|winemac|win32u" || true
for f in "$WINE_PREFIX_DIR"/lib/wine/*-unix/ntdll.so; do
  [ -f "$f" ] || continue
  file "$f"
  if command -v otool >/dev/null; then otool -l "$f" | grep -A4 LC_BUILD_VERSION | head -6; fi
done
if command -v otool >/dev/null; then
  # bin/wine is a normal PIE (4 GB page zero). Its ntdll.so re-execs the *loader* below (reexec_loader ->
  # loader_exec, dlls/ntdll/unix/loader.c:2039-2063); whichever image that is must survive exec().
  for f in "$WINE_PREFIX_DIR/bin/wine" "$WINE_PREFIX_DIR"/lib/wine/*-unix/wine; do
    [ -f "$f" ] || continue
    echo "$f:"; file "$f" | cut -d: -f2; otool -l "$f" | grep -B1 -A4 "__PAGEZERO" | head -8; otool -l "$f" | grep -A5 LC_BUILD_VERSION | grep -E "minos|sdk"
  done
fi
if [ -f "$WINE_PREFIX_DIR/lib/wine/aarch64-windows/xtajit64.dll" ]; then echo "xtajit64.dll present"; else echo "xtajit64.dll NOT present"; fi
