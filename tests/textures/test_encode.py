#!/usr/bin/env python3
"""End-to-end tests for tools/textures/encode.py on synthetic .bc blobs.

Covers: output files and ASTC header/magic, formats with no mapping, a corrupt input, incremental
skip, --force, determinism (job count, ASTC SIMD backend, repeated runs), --config block sizes,
error exits, name/key mismatch handling, and that the runtime library (`rltex lookup`) takes the
cache-hit path on exactly what encode.py wrote.
"""
import hashlib
import json
import os
import shutil
import subprocess
import sys
import tempfile

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(ROOT, "tools", "textures"))
import texgen as tg  # noqa: E402
import encode  # noqa: E402

RLTEX = os.environ.get("RLTEX", os.path.join(ROOT, "build/textures/rltex/rltex"))
AVX2 = os.path.join(ROOT, "build/astcenc/Source/astcenc-avx2")
SSE41 = os.path.join(ROOT, "build/astcenc/Source/astcenc-sse4.1")
ENCODE_PY = os.path.join(ROOT, "tools/textures/encode.py")
TMPROOT = os.path.join(ROOT, "build/tmp")
os.makedirs(TMPROOT, exist_ok=True)

FAILS = []
N = 0


def check(ok, what):
    global N
    N += 1
    if not ok:
        FAILS.append(what)
        print("  FAIL:", what)


def sh(*cmd, env=None):
    p = subprocess.run([str(c) for c in cmd], capture_output=True, text=True, env=env)
    return p.returncode, p.stdout, p.stderr


def run_encode(*args, env=None):
    return sh(sys.executable, ENCODE_PY, *args, env=env)


def sha_dir(d):
    out = {}
    for n in sorted(os.listdir(d)):
        if n.endswith(".astc"):
            out[n] = hashlib.sha256(open(os.path.join(d, n), "rb").read()).hexdigest()
    return out


def make_inputs(d):
    """Writes one .bc per format/variant. Returns {key: (name, w, h, supported)}."""
    os.makedirs(os.path.join(d, "sub"), exist_ok=True)
    meta = {}
    specs = [("BC1", 37, 23), ("BC1_SRGB", 64, 64), ("BC2", 40, 20), ("BC3", 64, 36), ("BC3_SRGB", 33, 33),
             ("BC4", 50, 30), ("BC5", 64, 64), ("BC6H", 40, 24), ("BC7", 64, 64), ("BC7_SRGB", 37, 37)]
    imgs = {"BC4": tg.img_height, "BC5": tg.img_normalmap, "BC6H": tg.img_hdr}
    for i, (name, w, h) in enumerate(specs):
        kind = tg.kind_of(name)
        img = imgs.get(kind, tg.img_organic)(w, h)
        blob = tg.encode(kind, img, "etcpak" if kind in ("BC4", "BC5") else "ispc")
        key = tg.key_of(tg.DXGI[name], w, h, i % 3, i % 2, blob)
        sub = "sub" if i % 2 else ""
        open(os.path.join(d, sub, key + ".bc"), "wb").write(tg.bc_file(tg.DXGI[name], w, h, i % 3, i % 2, blob))
        meta[key] = (name, w, h, True)
    # formats with no ASTC mapping
    rng = np.random.default_rng(11)
    for name in ("BC4_SNORM", "BC5_SNORM", "BC6H_SF16"):
        kind = tg.kind_of(name)
        blob = bytes(rng.integers(0, 256, tg.blob_size(kind, 16, 16), dtype=np.uint8))
        if kind == "BC6H":
            blob = bytes(16 * 16)  # all-zero blocks: valid black
        key = tg.key_of(tg.DXGI[name], 16, 16, 0, 0, blob)
        open(os.path.join(d, key + ".bc"), "wb").write(tg.bc_file(tg.DXGI[name], 16, 16, 0, 0, blob))
        meta[key] = (name, 16, 16, False)
    return meta


def main():
    for t in (RLTEX, AVX2):
        if not os.access(t, os.X_OK):
            print("missing tool:", t, "(run tests/textures/build.sh)")
            return 2
    work = tempfile.mkdtemp(prefix="encode-", dir=TMPROOT)
    try:
        run(work)
    finally:
        shutil.rmtree(work, ignore_errors=True)
    print("\ntest_encode: %d checks, %d failures" % (N, len(FAILS)))
    for f in FAILS:
        print("FAIL:", f)
    return 1 if FAILS else 0


def run(work):
    inp, out = os.path.join(work, "in"), os.path.join(work, "out")
    os.makedirs(inp)
    meta = make_inputs(inp)
    supported = {k for k, v in meta.items() if v[3]}
    n_ok, n_unsup = len(supported), len(meta) - len(supported)

    print("== first run, with one corrupt input present")
    junk = os.path.join(inp, "junk.bc")
    open(junk, "wb").write(b"not a bc file at all")
    rc, so, se = run_encode("--in", inp, "--out", out, "--jobs", "4")
    print(so.rstrip())
    check(rc == 1, "exit status 1 when an input is corrupt (got %d)" % rc)
    check("failed=1" in so and ("encoded=%d" % n_ok) in so and ("unsupported=%d" % n_unsup) in so, "summary counts")
    os.remove(junk)

    print("== outputs")
    outs = sorted(n for n in os.listdir(out) if n.endswith(".astc"))
    check(set(n[:-5] for n in outs) == supported, "exactly the supported keys have .astc files")
    check(not [n for n in os.listdir(out) if n.startswith(".encode-")], "temp directory removed")
    for key, (name, w, h, sup) in meta.items():
        path = os.path.join(out, key + ".astc")
        if not sup:
            check(not os.path.exists(path), "%s (%s) has no .astc" % (key[:12], name))
            continue
        d = open(path, "rb").read()
        kind = tg.kind_of(name)
        bx = {"BC1": 6, "BC4": 6}.get(kind, 4)
        check(d[:4] == bytes([0x13, 0xAB, 0xA1, 0x5C]), "%s magic" % name)
        check((d[4], d[5], d[6]) == (bx, bx, 1), "%s block %dx%d" % (name, bx, bx))
        check((d[7] | d[8] << 8 | d[9] << 16, d[10] | d[11] << 8 | d[12] << 16, d[13]) == (w, h, 1), "%s dims" % name)
        check(len(d) == 16 + -(-w // bx) * -(-h // bx) * 16, "%s payload size" % name)
        check(encode.astc_file_ok(path), "%s validates" % name)

    print("== runtime library takes the hit path on encode.py output")
    docs = os.path.join(work, "Documents")
    texdir = os.path.join(docs, "cache", "textures")
    os.makedirs(texdir)
    for n in outs:
        shutil.copy(os.path.join(out, n), texdir)
    for root_, _d, files in os.walk(inp):
        for n in files:
            key = n[:-3]
            rc, so, se = sh(RLTEX, "lookup", "--root", docs, os.path.join(root_, n))
            name, w, h, sup = meta[key]
            if sup:
                bx = {"BC1": 6, "BC4": 6}.get(tg.kind_of(name), 4)
                check(rc == 0 and so.startswith("hit " + key) and ("block=%dx%d" % (bx, bx)) in so,
                      "lookup hit for %s: %r %s" % (name, so.strip(), se.strip()))
            else:
                check(rc == 0 and so.startswith("miss " + key), "lookup miss (no ASTC mapping) for %s: %r" % (name, so.strip()))

    print("== second run: everything already encoded is skipped, files untouched")
    before = {n: os.stat(os.path.join(out, n)).st_mtime_ns for n in outs}
    rc, so, se = run_encode("--in", inp, "--out", out, "--quiet")
    check(rc == 0 and ("skipped=%d" % n_ok) in so and "encoded=0" in so, "skip summary: " + so.strip().splitlines()[-1])
    check(all(os.stat(os.path.join(out, n)).st_mtime_ns == before[n] for n in outs), "skipped files not rewritten")
    ref = sha_dir(out)

    print("== truncated output is re-encoded; --force rewrites identically")
    victim = os.path.join(out, outs[0])
    open(victim, "wb").write(open(victim, "rb").read()[:-3])
    rc, so, se = run_encode("--in", inp, "--out", out, "--quiet")
    check("encoded=1" in so and sha_dir(out) == ref, "invalid existing .astc re-encoded to identical bytes")
    rc, so, se = run_encode("--in", inp, "--out", out, "--quiet", "--force")
    check(("encoded=%d" % n_ok) in so and sha_dir(out) == ref, "--force re-encodes all to identical bytes")

    print("== determinism: jobs, SIMD backend, repeat")
    o1, o8, osse = (os.path.join(work, x) for x in ("o1", "o8", "osse"))
    run_encode("--in", inp, "--out", o1, "--jobs", "1", "--quiet")
    run_encode("--in", inp, "--out", o8, "--jobs", "8", "--quiet")
    check(sha_dir(o1) == ref and sha_dir(o8) == ref, "--jobs 1 / --jobs 8 byte-identical to the first run")
    if os.access(SSE41, os.X_OK):
        run_encode("--in", inp, "--out", osse, "--astcenc", SSE41, "--quiet")
        check(sha_dir(osse) == ref, "astcenc-sse4.1 output byte-identical to astcenc-avx2")
    else:
        print("  (sse4.1 binary missing, skipped)")

    print("== --config changes block sizes (and the runtime reads them from the file)")
    cfg = os.path.join(work, "port.toml")
    open(cfg, "w").write('astc_bc1_block = "8x8"\nastc_bc3_block = "5x5"\nastc_bc4_block = "10x10"\n'
                         'astc_bc7_block = "6x6"\nastc_bc6h_block = "6x6"\n')
    oc = os.path.join(work, "oc")
    rc, so, se = run_encode("--in", inp, "--out", oc, "--config", cfg, "--quiet")
    check(rc == 0, "config run exit 0: " + se.strip())
    want = {"BC1": 8, "BC2": 5, "BC3": 5, "BC4": 10, "BC5": 5, "BC6H": 6, "BC7": 6}
    for key, (name, w, h, sup) in meta.items():
        if not sup:
            continue
        d = open(os.path.join(oc, key + ".astc"), "rb").read()
        b = want[tg.kind_of(name)]
        check((d[4], d[5]) == (b, b), "%s block %dx%d under config (got %dx%d)" % (name, b, b, d[4], d[5]))
        check(encode.astc_file_ok(os.path.join(oc, key + ".astc")), "%s validates" % name)
    # stale cache from another config is still a valid hit: the runtime trusts the file header
    docs2 = os.path.join(work, "Documents2")
    os.makedirs(os.path.join(docs2, "cache", "textures"))
    for n in os.listdir(oc):
        if n.endswith(".astc"):
            shutil.copy(os.path.join(oc, n), os.path.join(docs2, "cache", "textures"))
    key = next(k for k, v in meta.items() if v[0] == "BC1")
    path = next(os.path.join(r, key + ".bc") for r, _d, f in os.walk(inp) if key + ".bc" in f)
    rc, so, se = sh(RLTEX, "lookup", "--root", docs2, path)
    check(so.startswith("hit") and "block=8x8" in so, "runtime hit reports the file's 8x8 block: " + so.strip())
    # a bad config is refused before any work
    bad = os.path.join(work, "bad.toml")
    open(bad, "w").write('astc_bc1_block = "7x7"\n')
    rc, so, se = run_encode("--in", inp, "--out", os.path.join(work, "ob"), "--config", bad)
    check(rc == 2 and "7x7" in se, "invalid config exits 2 with a message: rc=%d %s" % (rc, se.strip()))

    print("== error handling")
    rc, so, se = run_encode("--in", os.path.join(work, "nope"), "--out", out)
    check(rc == 2, "missing --in exits 2")
    rc, so, se = run_encode("--in", inp, "--out", out, "--astcenc", "/nonexistent/astcenc")
    check(rc == 2, "missing astcenc exits 2")
    rc, so, se = run_encode("--in", inp, "--out", out, "--rltex", "/nonexistent/rltex")
    check(rc == 2, "missing rltex exits 2")
    empty = os.path.join(work, "empty")
    os.makedirs(empty)
    rc, so, se = run_encode("--in", empty, "--out", os.path.join(work, "oe"))
    check(rc == 0 and "0 .bc file(s)" in so, "empty input directory exits 0")

    print("== file name that does not match its content key")
    mm = os.path.join(work, "mm")
    os.makedirs(mm)
    some = next(iter(supported))
    src = next(os.path.join(r, some + ".bc") for r, _d, f in os.walk(inp) if some + ".bc" in f)
    shutil.copy(src, os.path.join(mm, "renamed.bc"))
    rc, so, se = run_encode("--in", mm, "--out", os.path.join(work, "omm"))
    check(rc == 0 and "content key" in so and os.path.exists(os.path.join(work, "omm", some + ".astc")),
          "content-derived key used, mismatch reported")

    print("== plan JSON is what encode.py consumes")
    rc, so, se = sh(RLTEX, "plan", src)
    plan = json.loads(so)
    check(plan["key"] == some and plan["astc"] is True and plan["astcenc_mode"] in ("-cl", "-cs", "-ch"), "plan JSON well formed")


if __name__ == "__main__":
    sys.exit(main())
