#!/usr/bin/env bash
# Platform probe: what does this macOS ARM64 runner allow? Facts for Wine's VM manager and FEX's JIT.
# Cheap (seconds), runs before the Wine build. Output lines start with PROBE.
. "$(dirname "$0")/lib.sh"
[ "$IS_DARWIN" = 1 ] || die "macOS only"
dir=$WORK/probe; mkdir -p "$dir"

echo "PROBE uname: $(uname -a)"
sw_vers | sed -e 's/^/PROBE sw_vers: /'
echo "PROBE xcode: $(xcode-select -p) $(xcodebuild -version 2>/dev/null | tr '\n' ' ')"
echo "PROBE clang: $(clang --version | head -1)"
echo "PROBE SDK: $(xcrun --show-sdk-path) $(xcrun --show-sdk-version)"
csrutil status 2>&1 | sed -e 's/^/PROBE csrutil: /' || true
echo "PROBE DevToolsSecurity: $(DevToolsSecurity -status 2>&1 | head -1)"

clang -O1 -o "$dir/probe" "$MACOS_TOOLS_DIR/probe/probe.c"
codesign -s - --force "$dir/probe" 2>/dev/null || true
"$dir/probe"

echo "PROBE ---- W+X mmap with a debugger attached (stand-in for the StikDebug / CS_DEBUGGED case) ----"
if command -v lldb >/dev/null 2>&1; then
  sudo DevToolsSecurity -enable >/dev/null 2>&1 || true
  rc=0
  run_timeout 120 lldb --batch -o 'settings set auto-confirm true' -o run -o quit -- "$dir/probe" rwx 2>&1 | sed -e 's/^/PROBE lldb: /' || rc=$?
  echo "PROBE lldb exit: $rc"
else
  echo "PROBE lldb: not installed"
fi

echo "PROBE ---- reduced __PAGEZERO (spec 5: low 4 GB unmappable; MR 11638 says the kernel rejects smaller page zeros) ----"
cat > "$dir/tiny.c" <<'CEOF'
#include <stdio.h>
#include <sys/mman.h>
int main(void){ void *p = mmap((void*)0x7ffe0000, 0x1000, PROT_READ|PROT_WRITE, MAP_PRIVATE|MAP_ANON|MAP_FIXED, -1, 0);
  printf("tiny: started, mmap(0x7ffe0000)=%p\n", p); return 0; }
CEOF
for pz in 0x100000000 0x10000 0x1000; do
  clang -o "$dir/tiny-$pz" "$dir/tiny.c" -Wl,-pagezero_size,$pz 2>&1 | head -3
  rc=0; "$dir/tiny-$pz" 2>&1 | sed -e "s/^/PROBE pagezero=$pz: /" || rc=$?
  echo "PROBE pagezero=$pz: exit status ${PIPESTATUS[0]}"
done
