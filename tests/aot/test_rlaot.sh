#!/usr/bin/env bash
# rlaot end to end on synthetic PE files (never the game):
#   scan -> seed -> FEX's own ParseCodeMap agrees -> build merges a fake device code map
#   -> two seeds are byte-identical.
# Needs: x86_64-w64-mingw32-gcc, python3-pefile, python3-xxhash; build/codemap_check is used
# when present (built against FEX by tests/aot/build_codemap_check.sh).
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
T=$(mktemp -d); trap 'rm -rf "$T"' EXIT
G="$T/game/Binaries/Win64"; mkdir -p "$G"

cat > "$T/lib.c" <<'C'
__declspec(dllexport) int add(int a, int b) { return a + b; }
static int twice(int x) { return x * 2; }
int (*volatile fp)(int) = twice;   /* relocated pointer into .text */
__declspec(dllexport) int call_fp(int x) { return fp(x); }
C
cat > "$T/main.c" <<'C'
#include <stdio.h>
__declspec(dllimport) int add(int, int);
int main(void) { printf("%d\n", add(2, 3)); return 0; }
C
x86_64-w64-mingw32-gcc -O1 -shared -o "$G/Helper.dll" "$T/lib.c" -Wl,--out-implib,"$T/libhelper.a"
x86_64-w64-mingw32-gcc -O1 -o "$G/RocketLeague.exe" "$T/main.c" -L"$T" -lhelper

R="python3 $ROOT/aot/rlaot.py"
$R scan --game-dir "$T/game" --out "$T/aot/manifest.json"
$R seed --manifest "$T/aot/manifest.json" --out "$T/aot/codemaps/seed.codemap"
$R seed --manifest "$T/aot/manifest.json" --out "$T/seed2.codemap" >/dev/null
cmp "$T/aot/codemaps/seed.codemap" "$T/seed2.codemap" && echo "ok: seed is deterministic"

python3 - "$T" <<'PY'
import json, sys, pathlib
m = json.loads(pathlib.Path(sys.argv[1], "aot/manifest.json").read_text())
mods = {x["path"]: x for x in m["modules"]}
exe, dll = mods["Binaries/Win64/RocketLeague.exe"], mods["Binaries/Win64/Helper.dll"]
assert exe["seed_kinds"]["entry"] == 1, exe["seed_kinds"]
assert dll["seed_kinds"]["export"] == 2, dll["seed_kinds"]
assert exe["guest_path"] == "c:\\games\\rocketleague\\binaries\\win64\\rocketleague.exe"
print("ok: manifest seeds and guest paths")
PY

if [ -x "$ROOT/build/codemap_check" ]; then
  "$ROOT/build/codemap_check" "$T/aot/codemaps/seed.codemap" | sort > "$T/fex.txt"
  $R dump "$T/aot/codemaps/seed.codemap" | sort > "$T/py.txt"
  diff "$T/fex.txt" "$T/py.txt" && echo "ok: FEX's ParseCodeMap reads the seed identically"
else
  echo "skip: build/codemap_check not built"
fi

# A fake device code map: one new block in the exe, plus a module the manifest doesn't know.
python3 - "$T" "$ROOT" <<'PY'
import sys, json, pathlib
sys.path.insert(0, sys.argv[2] + "/aot")
import rlaot
t = pathlib.Path(sys.argv[1])
m = json.loads((t / "aot/manifest.json").read_text())
exe = next(x for x in m["modules"] if x["path"].endswith("RocketLeague.exe"))
fid = int(exe["file_id"], 16)
rlaot.write_codemap(t / "device/run1.codemap", {fid: exe["guest_path"], 0x1234: "c:\\windows\\system32\\other.dll"},
                    fid, {fid: [0x1000, 0x1ff0], 0x1234: [0x10]})
PY
$R build --codemaps "$T/device" --out "$T/aot" --manifest "$T/aot/manifest.json" | tee "$T/build.txt"
grep -q "not a current game module" "$T/build.txt" && echo "ok: unknown module reported"
python3 - "$T" "$ROOT" <<'PY'
import sys, json, pathlib
sys.path.insert(0, sys.argv[2] + "/aot")
import rlaot
t = pathlib.Path(sys.argv[1])
merged = rlaot.parse_codemap((t / "aot/codemaps/merged.codemap").read_bytes())
seed = rlaot.parse_codemap((t / "aot/codemaps/seed.codemap").read_bytes())
exe = next(f for f, c in merged.items() if c["bitness"] == 64)
assert seed[exe]["blocks"] | {0x1000, 0x1ff0} == merged[exe]["blocks"], "merge lost or invented blocks"
assert merged[0x1234]["blocks"] == {0x10}
print("ok: merge is the union of seed and device blocks")
PY
echo "PASS"
