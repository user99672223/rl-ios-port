#!/usr/bin/env bash
# Run a ladder of progressively harder programs under the built Wine and report each rung.
# A rung that fails does not stop the ladder, so one CI run shows exactly where the stack breaks:
#   L0 wine --version
#   L1 hello-aarch64.exe  native ARM64 process (Wine on macOS alone, no emulation)
#   L2 hello-arm64ec.exe  ARM64EC process (loads the emulator DLL named in the registry)
#   L3 hello-x86_64.exe   x86-64 process (needs FEX to translate)
#   L4 smoke.exe          every section separately, then the whole program
# Summary lines start with "LADDER ". Logs: $LOG_DIR/ladder-*.log
. "$(dirname "$0")/lib.sh"
wine_env
fail=0

rung() {            # rung NAME TIMEOUT cmd...
  local name=$1 t=$2; shift 2
  local out="$LOG_DIR/ladder-$name.log" rc=0
  run_timeout "$t" "$@" > "$out" 2>&1 || rc=$?
  if [ "$rc" -eq 0 ]; then echo "LADDER $name: PASS"; else echo "LADDER $name: FAIL rc=$rc"; fail=$((fail+1)); fi
  sed -e 's/^/    | /' "$out" | head -${LADDER_LINES:-60}
  [ "$rc" -eq 0 ] || echo "    | (see $out)"
  return 0
}

rung L0-wine-version      60 wine --version
rung L1-hello-aarch64    120 wine "$SMOKE_DIR/hello-aarch64.exe"
rung L2-hello-arm64ec    120 wine "$SMOKE_DIR/hello-arm64ec.exe"
rung L3-hello-x86_64     120 wine "$SMOKE_DIR/hello-x86_64.exe"

sections=$(wine "$SMOKE_DIR/smoke.exe" --list 2>/dev/null | tr -d '\r' || true)
[ -n "$sections" ] || sections="env threads fileio seh timers tls virtual fibers threadname"
for s in $sections; do
  rung "L4-smoke-$s" "${SMOKE_SECTION_TIMEOUT:-300}" wine "$SMOKE_DIR/smoke.exe" "$s"
done
LADDER_LINES=400 rung L4-smoke-all 900 wine "$SMOKE_DIR/smoke.exe"

# Aggregate PASS/FAIL lines from the per-section runs.
{
  echo "---- smoke check totals (per-section runs) ----"
  cat "$LOG_DIR"/ladder-L4-smoke-*.log 2>/dev/null | tr -d '\r' | grep -E '^(PASS|FAIL) ' | grep -v '^$' | \
    awk '{c[$1]++} END{printf "PASS=%d FAIL=%d\n", c["PASS"], c["FAIL"]}'
  cat "$LOG_DIR"/ladder-L4-smoke-*.log 2>/dev/null | tr -d '\r' | grep -E '^FAIL ' || true
} | tee "$LOG_DIR/smoke-summary.txt"

echo "LADDER rungs failed: $fail"
exit 0
