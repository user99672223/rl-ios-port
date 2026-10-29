#!/usr/bin/env bash
# Build and run a documented subset of Wine's own conformance tests as x86-64 PE programs.
# Under an ARM64EC prefix these run through FEX, so this exercises the emulator with real Windows API tests.
#
# Subset (spec 6.10: kernel32, ntdll, ws2_32, xinput, mmdevapi). Whole-DLL test executables are heavy under
# emulation, so each DLL runs the units that map to what the port depends on; every unit has its own timeout
# and its own result line. Override with CONFORMANCE_UNITS="kernel32:virtual,sync ntdll:exception".
#
# Test executable naming (tools/makedep.c:3762, 4117): <dll>/tests/<arch>-windows/<dll>_test.exe, one unit per
# source file (the unit is the .c file name without extension), run as: wine <dll>_test.exe <unit>.
# Result lines start with "CONF ".
. "$(dirname "$0")/lib.sh"
wine_env
toolchain_path
export WINETEST_DEBUG=${WINETEST_DEBUG:-1}      # quiet mode of the test harness; failures are always printed

DEFAULT_UNITS="
kernel32:virtual,sync,thread,fiber,file,path,time,timer,process,heap,loader,module,environ,directory,version
ntdll:exception,virtual,sync,thread,threadpool,time,info,om,rtl,unwind,wow64
ws2_32:sock,protocol
mmdevapi:mmdevenum,render,capture,propstore,dependency
xinput1_3:xinput
"
units=${CONFORMANCE_UNITS:-$DEFAULT_UNITS}
arch=${CONFORMANCE_ARCH:-x86_64}
timeout=${CONFORMANCE_UNIT_TIMEOUT:-420}
: > "$LOG_DIR/conformance-summary.txt"

CONF_BIN_DIR=${CONF_BIN_DIR:-$WORK/conformance-bin}      # filled by build-conformance.sh

total_fail=0; total_ok=0; total_unit_fail=0
for entry in $units; do
  dll=${entry%%:*}; list=${entry#*:}
  exe="$CONF_BIN_DIR/${dll}_test.exe"
  [ -f "$exe" ] || { echo "CONF $dll: no test executable"; echo "$dll * missing" >> "$LOG_DIR/conformance-summary.txt"; continue; }
  for unit in $(echo "$list" | tr ',' ' '); do
    out="$LOG_DIR/conformance-$dll-$unit.log"
    rc=0
    start=$(date +%s)
    run_timeout "$timeout" wine "$exe" "$unit" > "$out" 2>&1 || rc=$?
    secs=$(( $(date +%s) - start ))
    # Wine's harness prints "<unit>: N tests executed (M marked as todo, K failures), S skipped."
    summary=$(tr -d '\r' < "$out" | grep -E "tests executed" | tail -1 || true)
    nfail=$(tr -d '\r' < "$out" | grep -cE ":[0-9]+: Test failed:" || true)
    if [ "$rc" -eq 0 ] && [ "$nfail" -eq 0 ]; then status=PASS; total_ok=$((total_ok+1)); else status=FAIL; total_unit_fail=$((total_unit_fail+1)); fi
    total_fail=$((total_fail+nfail))
    line="CONF $dll:$unit: $status rc=$rc failures=$nfail ${secs}s ${summary:-(no summary line)}"
    echo "$line" | tee -a "$LOG_DIR/conformance-summary.txt"
    if [ "$status" = FAIL ]; then tr -d '\r' < "$out" | grep -E "Test failed|Unhandled|err:|wine:" | head -8 | sed -e 's/^/    | /'; fi
  done
done
echo "CONF totals: units passed=$total_ok units failed=$total_unit_fail individual failed checks=$total_fail"
