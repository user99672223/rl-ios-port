#!/usr/bin/env bash
# When a program dies before printing anything (SIGKILL, rc=137), collect what the kernel / dyld / AMFI say.
# Facts only. Output lines start with DIAG. The macOS runners have SIP disabled (see probe.sh output), so
# dtruss works; it is skipped quietly if it does not.
. "$(dirname "$0")/lib.sh"
wine_env
exe=${1:-$SMOKE_DIR/hello-aarch64.exe}
bin=$WINE_PREFIX_DIR/bin
unixdir=$(ls -d "$WINE_PREFIX_DIR"/lib/wine/*-unix | head -1)
out=$LOG_DIR/diagnose.log
exec > >(tee "$out") 2>&1

echo "DIAG ---- code signatures"
for f in "$bin/wine" "$bin/wineserver" "$unixdir/ntdll.so" "$unixdir/win32u.so"; do
  echo "DIAG $f"; codesign -dvv "$f" 2>&1 | head -12 | sed -e 's/^/DIAG   /'
  codesign --verify --strict "$f" 2>&1 | sed -e 's/^/DIAG   verify: /'
  xattr -l "$f" 2>&1 | sed -e 's/^/DIAG   xattr: /'
done
echo "DIAG ---- otool -L bin/wine"; otool -L "$bin/wine" | sed -e 's/^/DIAG   /'
echo "DIAG ---- load commands of bin/wine (segments)"; otool -l "$bin/wine" | grep -E "segname|vmaddr|vmsize|cmd LC_(MAIN|LOAD_DYLIB|CODE_SIGNATURE|DYLD_CHAINED)" | head -40 | sed -e 's/^/DIAG   /'

echo "DIAG ---- the exec'd loader image"
for f in "$unixdir/wine"; do
  file "$f" | sed -e 's/^/DIAG   /'; otool -l "$f" | grep -E "segname|vmaddr|vmsize|fileoff|filesize|minos|sdk|cmd LC_(BUILD|SEGMENT)" | head -50 | sed -e 's/^/DIAG   /'
  rc=0; "$f" > /dev/null 2>&1 || rc=$?; echo "DIAG   direct exec of $f: exit status $rc"
done
echo "DIAG ---- run with loader tracing"
rc=0
WINEDEBUG=+loaddll,+process,+module,+virtual,+seh run_timeout 120 wine "$exe" 2>&1 | head -80 | sed -e 's/^/DIAG   /' || rc=$?
echo "DIAG exit status of the pipeline: ${PIPESTATUS[0]}"

echo "DIAG ---- dtruss (last 60 lines)"
if command -v dtruss >/dev/null 2>&1; then
  run_timeout 180 sudo dtruss -f -t mmap -t mprotect -t execve -t open -t posix_spawn -t exit -t kill -t sigaction "$bin/wine" "$exe" 2>&1 | tail -60 | sed -e 's/^/DIAG   /' || true
fi

echo "DIAG ---- unified log (kernel, AMFI, syspolicyd) mentioning wine, last 5 minutes"
log show --last 5m --style compact --predicate 'eventMessage CONTAINS[c] "wine" OR eventMessage CONTAINS[c] "code signature" OR eventMessage CONTAINS[c] "AMFI"' 2>/dev/null | tail -40 | cut -c1-300 | sed -e 's/^/DIAG   /' || true

echo "DIAG ---- install layout"
ls -la "$bin" "$unixdir" 2>&1 | head -60 | sed -e 's/^/DIAG   /'
find "$WINE_PREFIX_DIR" -name '*preloader*' -o -name 'wine64*' 2>/dev/null | sed -e 's/^/DIAG   found: /'

echo "DIAG ---- what does the loader try to exec / posix_spawn?"
if command -v lldb >/dev/null 2>&1; then
  run_timeout 180 lldb --batch -o 'breakpoint set -n execve' -o 'breakpoint set -n posix_spawn' -o run -o 'expression -- (char*)$x1' -o 'bt 6' -o continue -o 'expression -- (char*)$x1' -o 'bt 6' -o quit -- "$bin/wine" "$exe" 2>&1 | tail -50 | sed -e 's/^/DIAG   /' || true
fi

echo "DIAG ---- lldb"
if command -v lldb >/dev/null 2>&1; then
  run_timeout 180 lldb --batch -o run -o 'bt 12' -o 'register read x16 x17 x18 pc sp' -o quit -- "$bin/wine" "$exe" 2>&1 | tail -40 | sed -e 's/^/DIAG   /' || true
fi
exit 0
