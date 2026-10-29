#!/usr/bin/env bash
# Install FEX's ARM64EC DLL into the Wine install, create a prefix, and point Wine at it.
#
# How Wine picks the ARM64EC emulator (read in the pinned tree, external/wine):
#   dlls/ntdll/loader.c  load_arm64ec_module(): opens \Registry\Machine\Software\Microsoft\Wow64\amd64,
#   reads its default value (REG_SZ) as a file name inside C:\windows\system32, falls back to
#   "xtajit64.dll", then load_dll()s it and calls arm64ec_process_init() on it.
#   loader/wine.inf.in:[CurrentVersionWow64.ntarm64] writes that key's default to "xtajit64.dll".
# So: set HKLM\Software\Microsoft\Wow64\amd64 (default) = libarm64ecfex.dll, and make the DLL available
# as a builtin (FEX patches its own header with Source/Windows/wine_builtin.bin for exactly this,
# external/fex/Source/Windows/CMakeLists.txt patch_library_wine()), i.e. in lib/wine/aarch64-windows
# *before* wineboot so wineboot creates the placeholder in system32.
. "$(dirname "$0")/lib.sh"
wine_env

dll=$FEX_DLL_DIR/libarm64ecfex.dll
[ -f "$dll" ] || die "$dll missing (run build-fex.sh)"
dest=$WINE_PREFIX_DIR/lib/wine/aarch64-windows
[ -d "$dest" ] || die "$dest missing: is Wine installed in $WINE_PREFIX_DIR?"
cp "$dll" "$dest/"
log "installed $(basename "$dll") into $dest"

rm -rf "$WINEPREFIX"
group "wineboot --init"
rc=0
run_timeout "${WINEBOOT_TIMEOUT:-600}" wine wineboot --init > "$LOG_DIR/wineboot.log" 2>&1 || rc=$?
tail -30 "$LOG_DIR/wineboot.log"
endgroup
if [ "$rc" -ne 0 ]; then warn "wineboot --init exited $rc"; fi
wineserver -w 2>/dev/null || true

group "register FEX as the amd64 emulator"
rc=0
run_timeout 120 wine reg add 'HKLM\Software\Microsoft\Wow64\amd64' /ve /t REG_SZ /d libarm64ecfex.dll /f > "$LOG_DIR/reg-add.log" 2>&1 || rc=$?
cat "$LOG_DIR/reg-add.log"
run_timeout 120 wine reg query 'HKLM\Software\Microsoft\Wow64\amd64' 2>&1 | tee -a "$LOG_DIR/reg-add.log" || true
wineserver -w 2>/dev/null || true
endgroup
[ "$rc" -eq 0 ] || warn "reg add exited $rc"
exit 0
