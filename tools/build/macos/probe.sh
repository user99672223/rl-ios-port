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

echo "PROBE ---- does x18 survive depending on the binary's deployment target? (MR 11638 links Wine with -mmacosx-version-min=12)"
for v in 11.0 12.0 13.0 14.0 15.0; do
  clang -O1 -mmacosx-version-min=$v -o "$dir/probe-x18-$v" "$MACOS_TOOLS_DIR/probe/probe.c" 2>&1 | head -2
  codesign -s - --force "$dir/probe-x18-$v" 2>/dev/null || true
  echo "PROBE minos=$v: $(otool -l "$dir/probe-x18-$v" | grep -A4 LC_BUILD_VERSION | grep -E 'minos|sdk' | tr -s ' ')"
  "$dir/probe-x18-$v" x18 | grep -E "x18_(after_signal_return|after_sleep|preemption)" | sed -e "s/^PROBE /PROBE minos=$v /"
done

echo "PROBE ---- x18 and the SDK version recorded in LC_BUILD_VERSION (xnu-11417.140.69 osfmk/arm64/machine_task.c:331: preserved only if platform==macOS and sdk < 13.0, or entitled)"
for v in 12.0 13.0; do
  clang -O1 -o "$dir/probe-sdk-$v" "$MACOS_TOOLS_DIR/probe/probe.c" -Wl,-platform_version,macos,$v,$v 2>&1 | head -3
  codesign -s - --force "$dir/probe-sdk-$v" 2>/dev/null || true
  echo "PROBE ld -platform_version macos $v $v: $(otool -l "$dir/probe-sdk-$v" | grep -A4 LC_BUILD_VERSION | grep -E 'minos|sdk' | tr -s ' ' | tr '\n' ' ')"
  "$dir/probe-sdk-$v" x18 | grep -E "x18_(after_signal_return|after_sleep|preemption)" | sed -e "s/^PROBE /PROBE sdk=$v /"
done
# Rewriting an already linked binary (what build-wine.sh does to bin/wine and bin/wineserver):
vtool -set-build-version macos 12.0 12.0 -replace -output "$dir/probe-vtool" "$dir/probe" 2>&1 | head -3
codesign -s - --force "$dir/probe-vtool" 2>/dev/null || true
echo "PROBE vtool -set-build-version macos 12.0 12.0: $(otool -l "$dir/probe-vtool" | grep -A4 LC_BUILD_VERSION | grep -E 'minos|sdk' | tr -s ' ' | tr '\n' ' ')"
"$dir/probe-vtool" x18 | grep -E "x18_(after_signal_return|after_sleep|preemption)" | sed -e "s/^PROBE /PROBE vtool-sdk12 /"

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

echo "PROBE ---- segment alignment (MR 11638 links the loader with -Wl,-segalign,0x1000; Apple Silicon pages are 16 KB)"
for sa in 0x1000 0x4000; do
  clang -o "$dir/tiny-sa-$sa" "$dir/tiny.c" -Wl,-segalign,$sa 2>&1 | head -2
  rc=0; "$dir/tiny-sa-$sa" > /dev/null 2>&1 || rc=$?
  echo "PROBE segalign=$sa: exit status $rc (137 = SIGKILL at exec)"
done
printf '<?xml version="1.0"?><plist version="1.0"><dict><key>CFBundleIdentifier</key><string>x.y</string></dict></plist>' > "$dir/x.plist"
clang -o "$dir/tiny-sa-plist" "$dir/tiny.c" -mmacosx-version-min=12 -Wl,-segalign,0x4000,-sectcreate,__TEXT,__info_plist,"$dir/x.plist" 2>&1 | head -2
rc=0; "$dir/tiny-sa-plist" > /dev/null 2>&1 || rc=$?
echo "PROBE segalign=0x4000 + __info_plist section: exit status $rc"

echo "PROBE ---- plain RWX mmap (no MAP_JIT) under hardened runtime / entitlements: can a signed Wine loader get W+X without patching FEX?"
for ent in com.apple.security.cs.allow-unsigned-executable-memory com.apple.security.cs.allow-jit \
           com.apple.security.cs.disable-executable-page-protection com.apple.security.get-task-allow; do
  for hardened in 1 0; do
    cat > "$dir/ent.plist" <<EOF2
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0"><dict><key>$ent</key><true/></dict></plist>
EOF2
    cp "$dir/probe" "$dir/probe-ent"
    opts=""; [ "$hardened" = 1 ] && opts="-o runtime"
    codesign -s - --force $opts --entitlements "$dir/ent.plist" "$dir/probe-ent" 2>&1 | head -2
    echo "PROBE entitlement $ent hardened=$hardened:"
    "$dir/probe-ent" rwx 2>&1 | sed -e 's/^/PROBE    /' || true
  done
done
