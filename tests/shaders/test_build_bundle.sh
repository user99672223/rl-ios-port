#!/usr/bin/env bash
# build_bundle.py on synthetic DXMT-format caches: union, dedup, accumulation across runs.
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
T=$(mktemp -d); trap 'rm -rf "$T"' EXIT
mk() { python3 - "$1" "$2" "$3" <<'PY'
import sqlite3, sys
db = sqlite3.connect(sys.argv[1]); t = f"cache_{sys.argv[2]}"
db.execute(f"CREATE TABLE {t} (key BLOB PRIMARY KEY, value BLOB NOT NULL)")
for k in sys.argv[3].split(","):
    db.execute(f"INSERT INTO {t} VALUES (?, ?)", (k.encode(), b"metallib-" + k.encode()))
db.commit()
PY
}
mkdir -p "$T/pull1/cache/shaders" "$T/pull2/cache/shaders"
mk "$T/pull1/cache/shaders/shaders_310.db" 18 a,b,c
mk "$T/pull2/cache/shaders/shaders_310.db" 18 c,d
mk "$T/pull2/cache/shaders/shaders_320.db" 18 x
python3 "$ROOT/tools/shaders/build_bundle.py" --in "$T/pull1" --in "$T/pull2" --out "$T/out"
n() { python3 -c "import sqlite3,sys;print(sqlite3.connect(sys.argv[1]).execute('SELECT COUNT(*) FROM cache_18').fetchone()[0])" "$1"; }
[ "$(n "$T/out/RLShaders.bundle/shaders_310.db")" = 4 ] && echo "ok: union of a,b,c + c,d = 4"
[ "$(n "$T/out/RLShaders.bundle/shaders_320.db")" = 1 ] && echo "ok: per-version databases"
mkdir -p "$T/pull3"; mk "$T/pull3/shaders_310.db" 18 e
python3 "$ROOT/tools/shaders/build_bundle.py" --in "$T/pull3" --out "$T/out" >/dev/null
[ "$(n "$T/out/RLShaders.bundle/shaders_310.db")" = 5 ] && echo "ok: later pulls accumulate"
echo PASS
