#!/usr/bin/env bash
# Tests for tools/device/*.sh without a phone: fake-device mode (RLPORT_FAKE_DEVICE_DIR) and a tiny
# synthetic game tree built in a temp dir. The real game directory is never read or written.
#   tests/device/run.sh            run everything
#   tests/device/run.sh --keep     keep the temp dir for inspection
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$HERE/../.." && pwd)"
D="$REPO/tools/device"
KEEP=0
[ "${1:-}" = "--keep" ] && KEEP=1

T="$(mktemp -d "${TMPDIR:-/tmp}/rlport-devtest.XXXXXX")"
cleanup() {
  chmod -R u+w "$T" 2>/dev/null || true
  if [ "$KEEP" = 1 ]; then echo "kept: $T"; else rm -rf "$T"; fi
}
trap cleanup EXIT

PASS=0; FAIL=0
ok()   { PASS=$((PASS + 1)); printf '  ok   %s\n' "$1"; }
bad()  { FAIL=$((FAIL + 1)); printf '  FAIL %s\n' "$1"; }
check() { # <description> <command...>
  local d="$1"; shift
  if "$@" >/dev/null 2>&1; then ok "$d"; else bad "$d"; fi
}
check_not() { # <description> <command...>
  local d="$1"; shift
  if "$@" >/dev/null 2>&1; then bad "$d"; else ok "$d"; fi
}
has() { [ -e "$1" ]; }
same() { cmp -s "$1" "$2"; }

# ---------------------------------------------------------------- synthetic fixtures
GAME="$T/game"          # stands in for GAME_DIR; synthetic bytes only
WORK="$T/work"
PHONE="$T/phone"        # the fake app Documents folder
mkdir -p "$GAME/Binaries/Win64/EasyAntiCheat/sub" "$GAME/TAGame/CookedPCConsole" "$GAME/Engine/Config" "$WORK" "$PHONE"
printf 'MZ-fake-game-exe\n'  >"$GAME/Binaries/Win64/RocketLeague.exe"
printf 'MZ-fake-eac-exe\n'   >"$GAME/Binaries/Win64/RocketLeague_EAC.exe"
printf 'MZ-fake-launcher\n'  >"$GAME/Binaries/Win64/Launcher.exe"
printf 'MZ-fake-eos-dll\n'   >"$GAME/Binaries/Win64/EOSSDK-Win64-Shipping.dll"
printf 'eac-setup\n'         >"$GAME/Binaries/Win64/EasyAntiCheat/EasyAntiCheat_EOS_Setup.exe"
printf 'eac-nested\n'        >"$GAME/Binaries/Win64/EasyAntiCheat/sub/settings.json"
head -c 70000 /dev/zero | tr '\0' 'U' >"$GAME/TAGame/CookedPCConsole/Fake_SF.upk"
printf '[Core]\nfake=1\n'    >"$GAME/Engine/Config/BaseEngine.ini"
chmod -R a-w "$GAME"    # the source must be usable read-only
SNAP_BEFORE="$(cd "$GAME" && find . -type f -exec sha256sum {} + | sort; find "$GAME" -printf '%p %m %T@\n' | sort)"

mkdir -p "$WORK/prefix/drive_c/windows" "$WORK/prefix/dosdevices" "$WORK/textures/out" "$WORK/aot/cache"
printf 'REGEDIT4\n' >"$WORK/prefix/system.reg"
printf 'dll\n'      >"$WORK/prefix/drive_c/windows/fake.dll"
ln -s ../drive_c "$WORK/prefix/dosdevices/c:"
ln -s /          "$WORK/prefix/dosdevices/z:"       # must never be followed
printf 'astc-aaa\n' >"$WORK/textures/out/aaa.astc"
printf 'fexcache\n' >"$WORK/aot/cache/RocketLeague.exe-00112233445566aa-0000000000000000"

export GAME_DIR="$GAME" WORK_DIR="$WORK" OUT_DIR="$T/out"
export RLPORT_FAKE_DEVICE_DIR="$PHONE" RLPORT_BUNDLE_ID="dev.rlport.test"
unset RLPORT_FAKE_NO_DEVICE RLPORT_FAKE_NO_APP RLPORT_FAKE_FREE_BYTES RLPORT_GAME_EXCLUDES || true

OUTF="$T/last.out"
run() { "$@" >"$OUTF" 2>&1; }       # run, capture output, keep the exit status

echo "== --help"
for s in push_game pull_logs loop; do
  check "$s.sh --help exits 0 and prints Usage" bash -c "'$D/$s.sh' --help | grep -q '^Usage:'"
done

echo "== error paths"
check_not "push: no device -> non-zero" env RLPORT_FAKE_NO_DEVICE=1 "$D/push_game.sh" --dry-run
RLPORT_FAKE_NO_DEVICE=1 run "$D/push_game.sh" --dry-run || true
check "push: no device -> clear message" grep -q 'no iOS device found' "$OUTF"
RLPORT_FAKE_NO_APP=1 run "$D/push_game.sh" --dry-run || true
check "push: no app -> clear message" grep -q 'not found on the device' "$OUTF"
RLPORT_FAKE_NO_DEVICE=1 run "$D/pull_logs.sh" || true
check "pull: no device -> clear message" grep -q 'no iOS device found' "$OUTF"
run "$D/push_game.sh" --only bogus || true
check "push: unknown --only value rejected" grep -q "unknown set 'bogus'" "$OUTF"

echo "== push --dry-run changes nothing"
run "$D/push_game.sh" --dry-run
check "dry run succeeded and reports a plan" grep -q 'plan (bundle id dev.rlport.test)' "$OUTF"
check "dry run left the phone empty" test -z "$(find "$PHONE" -mindepth 1 -print -quit)"

echo "== push: space check"
RLPORT_FAKE_FREE_BYTES=1000 run "$D/push_game.sh" || true
check "too little space -> refused" grep -q 'not enough space on the phone' "$OUTF"
check "refused push copied nothing" test -z "$(find "$PHONE" -mindepth 1 -print -quit)"
RLPORT_FAKE_FREE_BYTES=abc run "$D/push_game.sh" || true
check "unreadable free space -> clear error" grep -q 'could not read free space' "$OUTF"

echo "== push: full copy"
RLPORT_FAKE_FREE_BYTES=100000000000 run "$D/push_game.sh"
check "push succeeded" grep -q 'push complete' "$OUTF"
check "game exe copied"            same "$GAME/Binaries/Win64/RocketLeague.exe" "$PHONE/game/Binaries/Win64/RocketLeague.exe"
check "game dll copied"            same "$GAME/Binaries/Win64/EOSSDK-Win64-Shipping.dll" "$PHONE/game/Binaries/Win64/EOSSDK-Win64-Shipping.dll"
check "big upk copied"             same "$GAME/TAGame/CookedPCConsole/Fake_SF.upk" "$PHONE/game/TAGame/CookedPCConsole/Fake_SF.upk"
check "ini copied"                 same "$GAME/Engine/Config/BaseEngine.ini" "$PHONE/game/Engine/Config/BaseEngine.ini"
check_not "RocketLeague_EAC.exe excluded" has "$PHONE/game/Binaries/Win64/RocketLeague_EAC.exe"
check_not "Launcher.exe excluded"         has "$PHONE/game/Binaries/Win64/Launcher.exe"
check_not "EasyAntiCheat/ excluded"       has "$PHONE/game/Binaries/Win64/EasyAntiCheat"
check "prefix file copied"         same "$WORK/prefix/system.reg" "$PHONE/prefix/system.reg"
check "prefix nested file copied"  same "$WORK/prefix/drive_c/windows/fake.dll" "$PHONE/prefix/drive_c/windows/fake.dll"
check_not "prefix symlink c: skipped" has "$PHONE/prefix/dosdevices/c:"
check_not "prefix symlink z: -> / skipped, not followed" has "$PHONE/prefix/dosdevices/z:"
check "textures copied to cache/textures" same "$WORK/textures/out/aaa.astc" "$PHONE/cache/textures/aaa.astc"
check "aot cache copied to FEX cache dir" same "$WORK/aot/cache/RocketLeague.exe-00112233445566aa-0000000000000000" "$PHONE/cache/fex/cache/RocketLeague.exe-00112233445566aa-0000000000000000"
check "GAME_DIR unchanged (content, modes, mtimes)" test "$SNAP_BEFORE" = "$(cd "$GAME" && find . -type f -exec sha256sum {} + | sort; find "$GAME" -printf '%p %m %T@\n' | sort)"

echo "== push: resume / idempotence"
M0="$(stat -c %Y "$PHONE/game/Engine/Config/BaseEngine.ini")"
sleep 1.1
: >"$PHONE/game/TAGame/CookedPCConsole/Fake_SF.upk"                       # simulate an interrupted copy
head -c 1000 "$GAME/TAGame/CookedPCConsole/Fake_SF.upk" >"$PHONE/game/TAGame/CookedPCConsole/Fake_SF.upk"
rm -f "$PHONE/prefix/system.reg"
RLPORT_FAKE_FREE_BYTES=100000000000 run "$D/push_game.sh"
check "truncated file re-sent"     same "$GAME/TAGame/CookedPCConsole/Fake_SF.upk" "$PHONE/game/TAGame/CookedPCConsole/Fake_SF.upk"
check "deleted prefix file restored" same "$WORK/prefix/system.reg" "$PHONE/prefix/system.reg"
check "unchanged file not touched" test "$M0" = "$(stat -c %Y "$PHONE/game/Engine/Config/BaseEngine.ini")"
RLPORT_FAKE_FREE_BYTES=100000000000 run "$D/push_game.sh"
check "second identical run copies nothing" grep -q 'total to copy: 0 B' "$OUTF"

echo "== push: --only and custom excludes"
rm -rf "${PHONE:?}"/*
RLPORT_FAKE_FREE_BYTES=100000000000 run "$D/push_game.sh" --only textures,aot
check "--only textures,aot copied textures" has "$PHONE/cache/textures/aaa.astc"
check_not "--only textures,aot did not copy the game" has "$PHONE/game"
check_not "--only textures,aot did not copy the prefix" has "$PHONE/prefix"
RLPORT_FAKE_FREE_BYTES=100000000000 RLPORT_GAME_EXCLUDES='Engine/:Binaries/Win64/Launcher.exe' run "$D/push_game.sh" --only game
check "custom exclude: Engine/ skipped"    test ! -e "$PHONE/game/Engine"
check "custom exclude: EAC exe now copied" has "$PHONE/game/Binaries/Win64/RocketLeague_EAC.exe"
check_not "custom exclude: Launcher.exe skipped" has "$PHONE/game/Binaries/Win64/Launcher.exe"
RLPORT_FAKE_FREE_BYTES=100000000000 WORK_DIR="$T/empty-work" run "$D/push_game.sh" --only prefix || true
check "--only with a missing source is an error" grep -q "source for 'prefix' not found" "$OUTF"

echo "== simulate the app writing logs and blobs"
mkdir -p "$PHONE/logs" "$PHONE/cache/shaders/dxbc" "$PHONE/cache/textures/bc" "$PHONE/cache/fex/codemap/new" "$PHONE/cache/fex/codemap/ready"
printf 'RocketLeague.exe,deadbeef,1a2b3c\n' >"$PHONE/logs/cpu_miss.log"
printf 'deadbeef,1a2b3c,99\n'               >"$PHONE/logs/cpu_profile.log"
: >"$PHONE/logs/faults.log"
printf 't_ms,frame_ms\n1,16.6\n'            >"$PHONE/logs/frametimes.csv"
printf 'other\n'                              >"$PHONE/logs/startup.log"
printf 'DXBCfake1'  >"$PHONE/cache/shaders/dxbc/0011aa.dxbc"
printf 'DXBCfake2'  >"$PHONE/cache/shaders/dxbc/0022bb.dxbc"
printf 'BCfake'     >"$PHONE/cache/textures/bc/ccdd.bc"
printf 'cm-new'     >"$PHONE/cache/fex/codemap/new/RocketLeague.exe-00112233445566aa.133700000.bin"
printf 'cm-ready'   >"$PHONE/cache/fex/codemap/ready/RocketLeague.exe-00112233445566aa"
cat >"$T/syslog.fake" <<'EOF'
Oct 29 12:00:00 iPhone RLPort(libsystem)[123] <Notice>: JIT region ready
Oct 29 12:00:01 iPhone SpringBoard[42] <Notice>: unrelated
Oct 29 12:00:02 iPhone RLPort(libsystem)[123] <Error>: wine: started
EOF

echo "== pull"
run "$D/pull_logs.sh" --syslog 1
check "pull succeeded" grep -q 'pulled to' "$OUTF"
L="$WORK/device/latest"
check "latest is a symlink" test -L "$L"
check "cpu_miss.log pulled"      same "$PHONE/logs/cpu_miss.log" "$L/logs/cpu_miss.log"
check "cpu_profile.log pulled"   same "$PHONE/logs/cpu_profile.log" "$L/logs/cpu_profile.log"
check "frametimes.csv pulled"    same "$PHONE/logs/frametimes.csv" "$L/logs/frametimes.csv"
check "other log pulled"         same "$PHONE/logs/startup.log" "$L/logs/startup.log"
check "faults.log pulled"        has "$L/logs/faults.log"
check "dxbc pulled"              same "$PHONE/cache/shaders/dxbc/0011aa.dxbc" "$L/dxbc/0011aa.dxbc"
check "bc blob pulled"           same "$PHONE/cache/textures/bc/ccdd.bc" "$L/textures_bc/ccdd.bc"
check "code map (new) pulled"    same "$PHONE/cache/fex/codemap/new/RocketLeague.exe-00112233445566aa.133700000.bin" "$L/codemaps/new/RocketLeague.exe-00112233445566aa.133700000.bin"
check "code map (ready) pulled"  same "$PHONE/cache/fex/codemap/ready/RocketLeague.exe-00112233445566aa" "$L/codemaps/ready/RocketLeague.exe-00112233445566aa"
check "timestamp dir name is UTC YYYYmmddTHHMMSSZ" bash -c "readlink '$L' | grep -Eq '^[0-9]{8}T[0-9]{6}Z$'"
RLPORT_FAKE_SYSLOG_FILE="$T/syslog.fake" run "$D/pull_logs.sh" --syslog 1
check "syslog filtered to the process name" bash -c "[ \"\$(wc -l < '$WORK/device/latest/syslog.txt')\" = 2 ] && ! grep -q SpringBoard '$WORK/device/latest/syslog.txt'"
check "phone files still there after pull (read-only)" has "$PHONE/logs/cpu_miss.log"
FIRST="$(readlink "$L")"
sleep 1.1
printf 'RocketLeague.exe,deadbeef,ffffff\n' >>"$PHONE/logs/cpu_miss.log"
run "$D/pull_logs.sh"
check "second pull makes a new dir and moves latest" test "$(readlink "$L")" != "$FIRST"
check "second pull has the new line" same "$PHONE/logs/cpu_miss.log" "$L/logs/cpu_miss.log"
check "unchanged blob hard-linked to previous pull" test "$(stat -c %i "$L/dxbc/0011aa.dxbc")" = "$(stat -c %i "$WORK/device/$FIRST/dxbc/0011aa.dxbc")"
run "$D/pull_logs.sh" --syslog-only 1
check "--syslog-only succeeds" grep -q 'syslog saved' "$OUTF"

echo "== loop with stub stage tools"
STUBS="$T/stubs"; mkdir -p "$STUBS"
cat >"$STUBS/aot.py" <<'EOF'
import sys, pathlib
args = sys.argv[1:]
open(sys.argv[0] + ".args", "w").write(" ".join(args) + "\n")
assert args[0] == "build"
cm = pathlib.Path(args[args.index("--codemaps") + 1]); out = pathlib.Path(args[args.index("--out") + 1])
assert any(p.is_file() for p in cm.rglob("*")), "no codemaps"
(out / "cache").mkdir(parents=True, exist_ok=True)
(out / "cache" / "NewlyBuilt.exe-feedface-0000000000000000").write_text("rebuilt\n")
EOF
cat >"$STUBS/shaders.py" <<'EOF'
import sys, pathlib
args = sys.argv[1:]
open(sys.argv[0] + ".args", "w").write(" ".join(args) + "\n")
src = pathlib.Path(args[args.index("--in") + 1]); out = pathlib.Path(args[args.index("--out") + 1])
out.mkdir(parents=True, exist_ok=True)
(out / "count.txt").write_text(str(len(list(src.glob("*.dxbc")))) + "\n")
EOF
cat >"$STUBS/textures.py" <<'EOF'
import sys, pathlib
args = sys.argv[1:]
open(sys.argv[0] + ".args", "w").write(" ".join(args) + "\n")
src = pathlib.Path(args[args.index("--in") + 1]); out = pathlib.Path(args[args.index("--out") + 1])
out.mkdir(parents=True, exist_ok=True)
for p in src.glob("*.bc"):
    (out / (p.stem + ".astc")).write_text("encoded " + p.name + "\n")
EOF
cat >"$STUBS/package.sh" <<EOF
#!/usr/bin/env bash
mkdir -p "$T/out" && echo "ipa" >"$T/out/RLPort.ipa"
EOF
rm -rf "${PHONE:?}"/cache/fex/cache "${PHONE:?}"/cache/textures/*.astc "${PHONE:?}"/game
export RLPORT_LOOP_AOT="$STUBS/aot.py" RLPORT_LOOP_SHADERS="$STUBS/shaders.py" RLPORT_LOOP_TEXTURES="$STUBS/textures.py" RLPORT_LOOP_PACKAGE="$STUBS/package.sh"
RLPORT_FAKE_FREE_BYTES=100000000000 run "$D/loop.sh"
check "loop succeeded" grep -q 'loop finished' "$OUTF"
check "aot called with the exact interface" grep -qx "build --codemaps $WORK/device/latest/codemaps --out $WORK/aot" "$STUBS/aot.py.args"
check "shaders called with the exact interface" grep -qx -- "--in $WORK/device/latest/dxbc --out $WORK/shaders" "$STUBS/shaders.py.args"
check "textures called with the exact interface" grep -qx -- "--in $WORK/device/latest/textures_bc --out $WORK/textures/out" "$STUBS/textures.py.args"
check "shader stub saw both pulled blobs" grep -qx 2 "$WORK/shaders/count.txt"
check "new texture pushed to the phone" has "$PHONE/cache/textures/ccdd.astc"
check "old texture still pushed too" has "$PHONE/cache/textures/aaa.astc"
check "new AOT cache pushed to the phone" has "$PHONE/cache/fex/cache/NewlyBuilt.exe-feedface-0000000000000000"
check_not "loop push did not copy the game" has "$PHONE/game"
check "package stage ran" has "$T/out/RLPort.ipa"
check "loop log written under work/logs" bash -c "ls '$WORK'/logs/loop-*.log"
check "loop log is timestamped and has the summary" bash -c "grep -Eq '^[0-9]{4}-[0-9]{2}-[0-9]{2}T[0-9:]+Z \\[loop\\] ' \$(ls -t '$WORK'/logs/loop-*.log | head -1) && grep -q 'summary:' \$(ls -t '$WORK'/logs/loop-*.log | head -1)"

echo "== loop: skip flags and missing tools"
rm -f "$STUBS"/*.args "$T/out/RLPort.ipa"
sleep 1.1
RLPORT_LOOP_SHADERS="$T/does-not-exist.py" RLPORT_FAKE_FREE_BYTES=100000000000 run "$D/loop.sh" --skip-pull --skip-aot --skip-textures --skip-push
check "missing shader tool is skipped with a message" grep -q 'tool missing' "$OUTF"
check "skipped stages did not run" test ! -e "$STUBS/aot.py.args" -a ! -e "$STUBS/textures.py.args"
check "package still ran" has "$T/out/RLPort.ipa"
rm -f "$T/out/RLPort.ipa"
RLPORT_FAKE_FREE_BYTES=100000000000 run "$D/loop.sh" --skip pull,aot,shaders,textures,push,package
check "--skip LIST skips everything" bash -c "grep -c SKIPPED '$OUTF' | grep -qx 6"
RLPORT_LOOP_AOT="$T/does-not-exist.py" RLPORT_FAKE_FREE_BYTES=100000000000 run "$D/loop.sh" --skip-pull --skip-push --skip-package
check "missing AOT tool is skipped, not fatal" grep -q 'tool missing' "$OUTF"
printf 'import sys; sys.exit(3)\n' >"$STUBS/failing.py"
RLPORT_LOOP_TEXTURES="$STUBS/failing.py" RLPORT_FAKE_FREE_BYTES=100000000000 run "$D/loop.sh" --skip-pull --skip-push --skip-package --skip-aot --skip-shaders || true
check "failing stage stops the loop with an error" grep -q "stage 'textures' failed" "$OUTF"
RLPORT_FAKE_NO_DEVICE=1 run "$D/loop.sh" --skip-aot --skip-shaders --skip-textures --skip-push --skip-package || true
check "loop surfaces the no-device error from pull" grep -q 'no iOS device found' "$OUTF"
unset RLPORT_LOOP_AOT RLPORT_LOOP_SHADERS RLPORT_LOOP_TEXTURES RLPORT_LOOP_PACKAGE

echo "== loop with the real default tool paths (some do not exist yet) and --dry-run"
RLPORT_FAKE_FREE_BYTES=100000000000 run "$D/loop.sh" --dry-run
check "dry run completes" grep -q 'loop finished' "$OUTF"

echo "== Info.plist"
if python3 - "$REPO/ios/App/Info.plist" <<'EOF'
import plistlib, sys
d = plistlib.load(open(sys.argv[1], "rb"))
assert d["UIFileSharingEnabled"] is True
assert d["LSSupportsOpeningDocumentsInPlace"] is True
assert d["UIRequiresFullScreen"] is True
assert d["GCSupportsControllerUserInteraction"] is True
roles = d["UIApplicationSceneManifest"]["UISceneConfigurations"]
assert "UIWindowSceneSessionRoleApplication" in roles
assert "UIWindowSceneSessionRoleExternalDisplayNonInteractive" in roles
o = d["UISupportedInterfaceOrientations"]
assert o and all("Landscape" in x for x in o), o
assert d["CFBundleIdentifier"] == "$(RLPORT_BUNDLE_ID)"
EOF
then ok "Info.plist parses with plistlib and has the required keys"; else bad "Info.plist"; fi

echo "== shellcheck"
if command -v shellcheck >/dev/null 2>&1; then
  check "shellcheck clean: tools/device + tests/device" shellcheck -x "$D"/*.sh "$HERE/run.sh"
else
  bad "shellcheck not installed (sudo apt-get install -y shellcheck)"
fi

echo
echo "passed: $PASS  failed: $FAIL"
[ "$FAIL" = 0 ]
