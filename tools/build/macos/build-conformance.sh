#!/usr/bin/env bash
# Build the Wine conformance test executables for the DLLs in the subset, as PE programs of $CONFORMANCE_ARCH
# (default x86_64, so they run under FEX), and stage them in $CONF_BIN_DIR. Needs the Wine build tree.
#
# Naming (tools/makedep.c:3762 and 4117): dlls/<dll>/tests/<arch>-windows/<dll>_test.exe
. "$(dirname "$0")/lib.sh"
toolchain_path
arch=${CONFORMANCE_ARCH:-x86_64}
CONF_BIN_DIR=${CONF_BIN_DIR:-$WORK/conformance-bin}
mkdir -p "$CONF_BIN_DIR"
for dll in ${CONFORMANCE_DLLS:-kernel32 ntdll ws2_32 mmdevapi xinput1_3}; do
  t="dlls/$dll/tests/$arch-windows/${dll}_test.exe"
  if make -C "$WINE_BUILD" -j"$NCPU" "$t" > "$LOG_DIR/conformance-build-$dll.log" 2>&1; then
    cp "$WINE_BUILD/$t" "$CONF_BIN_DIR/${dll}_test.exe"
    echo "CONF build $dll: ok ($(wc -c < "$CONF_BIN_DIR/${dll}_test.exe" | tr -d ' ') bytes)"
  else
    echo "CONF build $dll: FAILED"; tail -8 "$LOG_DIR/conformance-build-$dll.log"
  fi
done
