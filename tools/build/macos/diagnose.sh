#!/usr/bin/env bash
# Collect evidence for the first failing rungs of the ladder. Facts only; output lines start with DIAG.
#   1. loader image facts (the exec'd lib/wine/<arch>-unix/wine: page zero, segment layout, SDK field for x18)
#   2. hello-arm64ec and hello-x86_64 with Wine's +seh,+virtual,+loaddll channels (first and last lines)
#   3. hello-aarch64 (which hangs): sample(1) and an lldb "thread backtrace all" of the live process
. "$(dirname "$0")/lib.sh"
wine_env
bin=$WINE_PREFIX_DIR/bin
unixdir=$(ls -d "$WINE_PREFIX_DIR"/lib/wine/*-unix | head -1)
out=$LOG_DIR/diagnose.log
exec > >(tee "$out") 2>&1

echo "DIAG ---- the exec'd loader image: $unixdir/wine"
otool -l "$unixdir/wine" | grep -E "segname|vmaddr|vmsize|minos|sdk|LC_BUILD_VERSION" | head -30 | sed -e 's/^/DIAG   /'

traced() {   # traced LABEL EXE CHANNELS
  echo "DIAG ---- $1 with WINEDEBUG=$3"
  local f="$LOG_DIR/diag-$1.log"
  WINEDEBUG=$3 run_timeout 120 wine "$2" > "$f" 2>&1 || echo "DIAG   exit status $?"
  echo "DIAG   $(wc -l < "$f") lines in $f"
  head -${DIAG_HEAD:-70} "$f" | cut -c1-260 | sed -e 's/^/DIAG   /'
  echo "DIAG   ... tail:"
  tail -${DIAG_TAIL:-30} "$f" | cut -c1-260 | sed -e 's/^/DIAG   /'
}
traced hello-arm64ec "$SMOKE_DIR/hello-arm64ec.exe" "err+all,+loaddll,+seh,+virtual"
traced hello-x86_64  "$SMOKE_DIR/hello-x86_64.exe"  "err+all,+loaddll,+seh,+virtual"

echo "DIAG ---- hello-aarch64 hangs: sample the live process"
wine "$SMOKE_DIR/hello-aarch64.exe" > "$LOG_DIR/diag-hello-aarch64.log" 2>&1 &
sleep 25
pids=$(pgrep -f 'hello-aarch64.exe' || true)
echo "DIAG   pids: $pids"
ps -o pid,ppid,stat,etime,command -p $(echo $pids | tr ' ' ',') 2>/dev/null | sed -e 's/^/DIAG   /'
for p in $pids; do
  sample "$p" 3 -file "$LOG_DIR/sample-$p.txt" > /dev/null 2>&1 || true
  head -60 "$LOG_DIR/sample-$p.txt" 2>/dev/null | cut -c1-200 | sed -e "s/^/DIAG   sample[$p] /"
done
if command -v lldb >/dev/null 2>&1; then
  for p in $pids; do
    run_timeout 90 lldb --batch -p "$p" -o 'thread backtrace all' -o detach -o quit 2>&1 | head -80 | cut -c1-220 | sed -e "s/^/DIAG   lldb[$p] /" || true
  done
fi
pkill -f 'hello-aarch64.exe' || true
wineserver -k 2>/dev/null || true
exit 0
