#!/usr/bin/env python3
"""Merge DXMT shader caches pulled from the device into RLShaders.bundle.

DXMT already caches compiled shaders on the device in SQLite (`shaders_<metal-version>.db`
under DXMT_SHADER_CACHE_PATH; key = DXBC SHA-1 + variant digest, value = compiled shader;
external/dxmt/src/dxmt/dxmt_shader_cache.cpp, src/d3d11/d3d11_pipeline_cache.cpp:147-157,
schema in src/winemetal/unix/cache.c:132-135: table cache_<version>(key BLOB PRIMARY KEY,
value BLOB NOT NULL)). Translating offline would only cover shaders the phone has already
seen, which are already in that cache, so the bundle is the union of pulled caches.

  build_bundle.py --in DIR [--in DIR ...] --out work/shaders
writes OUT/RLShaders.bundle/shaders_<v>.db (one per Metal version found).
"""
import argparse
import pathlib
import re
import shutil
import sqlite3
import sys

DB_RE = re.compile(r"^shaders_(\d+)\.db$")


def merge(sources, dest):
    dest.parent.mkdir(parents=True, exist_ok=True)
    out = sqlite3.connect(dest)
    added = 0
    for src in sources:
        db = sqlite3.connect(f"file:{src}?mode=ro", uri=True)
        tables = [r[0] for r in db.execute("SELECT name FROM sqlite_master WHERE type='table' AND name LIKE 'cache_%'")]
        for t in tables:
            if not re.fullmatch(r"cache_\d+", t):
                continue
            out.execute(f"CREATE TABLE IF NOT EXISTS {t} (key BLOB PRIMARY KEY, value BLOB NOT NULL)")
            before = out.execute(f"SELECT COUNT(*) FROM {t}").fetchone()[0]
            out.executemany(f"INSERT OR IGNORE INTO {t} (key, value) VALUES (?, ?)",
                            db.execute(f"SELECT key, value FROM {t}"))
            added += out.execute(f"SELECT COUNT(*) FROM {t}").fetchone()[0] - before
        db.close()
    out.commit()
    out.execute("VACUUM")
    out.close()
    return added


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--in", dest="inputs", action="append", required=True)
    ap.add_argument("--out", default="work/shaders")
    a = ap.parse_args()
    by_version = {}
    for d in a.inputs:
        for p in sorted(pathlib.Path(d).rglob("shaders_*.db")):
            m = DB_RE.match(p.name)
            if m:
                by_version.setdefault(m.group(1), []).append(p)
    bundle = pathlib.Path(a.out) / "RLShaders.bundle"
    if not by_version:
        print(f"no shaders_*.db under {a.inputs}; nothing to bundle")
        return 0
    # Start from the existing bundle so repeated loops accumulate.
    for v, srcs in sorted(by_version.items()):
        dest = bundle / f"shaders_{v}.db"
        if dest.exists():
            tmp = dest.with_suffix(".prev.db")
            shutil.copy(dest, tmp)
            srcs = [tmp] + srcs
            dest.unlink()
        n = merge(srcs, dest)
        total = sum(sqlite3.connect(dest).execute(f"SELECT COUNT(*) FROM {t}").fetchone()[0]
                    for (t,) in sqlite3.connect(dest).execute("SELECT name FROM sqlite_master WHERE type='table'"))
        for s in srcs:
            if s.name.endswith(".prev.db"):
                s.unlink()
        print(f"shaders_{v}.db: {total} entries ({n} merged from {len(srcs)} source(s)) -> {dest}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
