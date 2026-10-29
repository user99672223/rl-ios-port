#!/usr/bin/env python3
"""Offline BC -> ASTC encoder for the hybrid texture pipeline (PROJECT_SPEC.md 6.5).

    encode.py --in DIR_OF_.bc --out DIR [--jobs N] [--config port.toml]

Reads the `.bc` blobs pulled from the device (Documents/cache/textures/bc/<key>.bc), decodes them
with the project's own bcdec-based CLI (`rltex prepare`), and runs the real `astcenc` CLI with the
thorough preset and the block size / profile / swizzles the C library's mapping selects for that
format (so the on-device runtime and this tool can never disagree). Writes `<key>.astc` (the ASTC
file format: 16 byte header, magic 13 AB A1 5C) into --out, ready to be pushed back to
Documents/cache/textures/.

Properties:
  * Deterministic: astcenc is run single-threaded per file (`-j 1`), files are processed in sorted
    order, outputs are written to a temp name and renamed. Parallelism is over files only.
  * Incremental: a key whose `<key>.astc` already exists and validates is skipped (use --force).
  * The key is re-derived from the blob (SHA-256 of the 32 byte key header + blob) and checked
    against the file name; the content-derived key wins and a mismatch is reported.

astcenc flags used (all from astc-encoder Source/astcenccli_toplevel_help.cpp):
  -cl / -cs / -ch  LDR linear / LDR sRGB / HDR RGB + LDR alpha compress
  <in> <out> <blocksize> -thorough (quality preset 98)  -j 1  -esw  -ssw  -silent
Exit status: 0 all good, 1 at least one file failed, 2 usage / tool problems.
"""
import argparse
import concurrent.futures
import hashlib
import json
import os
import shutil
import struct
import subprocess
import sys
import tempfile
import threading

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.abspath(os.path.join(HERE, "..", ".."))

ASTC_MAGIC = bytes([0x13, 0xAB, 0xA1, 0x5C])
LEGAL_BLOCKS = {(4, 4), (5, 4), (5, 5), (6, 5), (6, 6), (8, 5), (8, 6), (10, 5), (10, 6),
                (8, 8), (10, 8), (10, 10), (12, 10), (12, 12)}


# ---------------------------------------------------------------------------------------------
# .bc file format (docs/notes/textures.md); independent of the C implementation
# ---------------------------------------------------------------------------------------------
def parse_bc_file(data):
    """Returns (dxgi, w, h, mip, slice, blob, key_hex) or raises ValueError."""
    if len(data) < 40 or data[:4] != b"RLBC" or struct.unpack("<I", data[4:8])[0] != 1:
        raise ValueError("not an RLBC v1 file")
    keyhdr = data[8:40]
    if keyhdr[:4] != b"RLK1":
        raise ValueError("bad key header magic")
    dxgi, w, h, mip, sl, nbytes = struct.unpack("<IIIIIQ", keyhdr[4:])
    blob = data[40:]
    if nbytes != len(blob):
        raise ValueError("blob size %d != header %d" % (len(blob), nbytes))
    return dxgi, w, h, mip, sl, blob, hashlib.sha256(keyhdr + blob).hexdigest()


def astc_file_ok(path):
    """True if `path` is a structurally valid .astc (magic, 2D, legal block, exact payload size)."""
    try:
        with open(path, "rb") as f:
            data = f.read()
    except OSError:
        return False
    if len(data) < 16 or data[:4] != ASTC_MAGIC:
        return False
    bx, by, bz = data[4], data[5], data[6]
    dx = data[7] | data[8] << 8 | data[9] << 16
    dy = data[10] | data[11] << 8 | data[12] << 16
    dz = data[13] | data[14] << 8 | data[15] << 16
    if bz != 1 or dz != 1 or (bx, by) not in LEGAL_BLOCKS or not dx or not dy:
        return False
    return len(data) == 16 + ((dx + bx - 1) // bx) * ((dy + by - 1) // by) * 16


# ---------------------------------------------------------------------------------------------
# tool discovery
# ---------------------------------------------------------------------------------------------
def _cpu_has(flag):
    try:
        with open("/proc/cpuinfo") as f:
            for line in f:
                if line.startswith("flags"):
                    return flag in line.split()
    except OSError:
        pass
    return False


def find_rltex(explicit):
    if explicit:  # an explicit path is never silently replaced by a fallback
        return explicit if os.path.isfile(explicit) and os.access(explicit, os.X_OK) else None
    cands = [os.environ.get("RLTEX"), os.path.join(REPO, "build/textures/rltex/rltex"),
             shutil.which("rltex")]
    for c in cands:
        if c and os.path.isfile(c) and os.access(c, os.X_OK):
            return c
    return None


def find_astcenc(explicit):
    if explicit:
        return explicit if os.path.isfile(explicit) and os.access(explicit, os.X_OK) else None
    base = os.path.join(REPO, "build/astcenc/Source")
    isa = ["astcenc-avx2", "astcenc-sse4.1"] if _cpu_has("avx2") else ["astcenc-sse4.1", "astcenc-avx2"]
    cands = [os.environ.get("ASTCENC")] + [os.path.join(base, n) for n in isa]
    cands += [shutil.which(n) for n in ("astcenc", "astcenc-avx2", "astcenc-sse4.1", "astcenc-neon")]
    for c in cands:
        if c and os.path.isfile(c) and os.access(c, os.X_OK):
            return c
    return None


# ---------------------------------------------------------------------------------------------
# per-file work
# ---------------------------------------------------------------------------------------------
class Tools:
    def __init__(self, rltex, astcenc, config, quality):
        self.rltex, self.astcenc, self.config, self.quality = rltex, astcenc, config, quality


def run(cmd):
    p = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    if p.returncode != 0:
        raise RuntimeError("%s failed (%d): %s" % (os.path.basename(cmd[0]), p.returncode,
                                                  (p.stderr or p.stdout).strip()))
    return p.stdout


def encode_one(tools, bc_path, out_dir, tmp_dir, force):
    """Returns (status, key, message). status in encoded|skipped|unsupported|error."""
    with open(bc_path, "rb") as f:
        raw = f.read()
    try:
        dxgi, w, h, mip, sl, blob, key = parse_bc_file(raw)
    except ValueError as e:
        return "error", os.path.basename(bc_path), "invalid .bc: %s" % e
    msg = ""
    stem = os.path.splitext(os.path.basename(bc_path))[0]
    if stem != key:
        msg = "file name key %s != content key %s (using content key)" % (stem, key)
    out_path = os.path.join(out_dir, key + ".astc")
    if not force and astc_file_ok(out_path):
        return "skipped", key, msg
    uniq = "%s.%d" % (key, threading.get_ident())  # duplicate keys may run concurrently
    ktx = os.path.join(tmp_dir, uniq + ".ktx")
    tmp_astc = os.path.join(tmp_dir, uniq + ".astc")
    try:
        cmd = [tools.rltex, "prepare"]
        if tools.config:
            cmd += ["--config", tools.config]
        plan = json.loads(run(cmd + [bc_path, ktx]))
        if not plan["astc"]:
            return "unsupported", key, "no ASTC mapping for %s%s" % (
                plan["format"], " (signed)" if plan["signed"] else "")
        q = "-" + tools.quality if not tools.quality.replace(".", "", 1).isdigit() else tools.quality
        cmd = [tools.astcenc, plan["astcenc_mode"], ktx, tmp_astc, plan["block"], q, "-j", "1", "-silent"]
        if plan["esw"] != "rgba":
            cmd += ["-esw", plan["esw"]]
        if plan["ssw"] != "rgba":
            cmd += ["-ssw", plan["ssw"]]
        run(cmd)
        if not astc_file_ok(tmp_astc):
            return "error", key, "astcenc produced an invalid .astc"
        os.replace(tmp_astc, out_path)  # same filesystem (tmp_dir lives in --out): atomic
        return "encoded", key, msg
    except (RuntimeError, KeyError, json.JSONDecodeError) as e:
        return "error", key, str(e)
    finally:
        for p in (ktx, tmp_astc):
            try:
                os.remove(p)
            except OSError:
                pass


def collect(in_dir):
    out = []
    for root, _dirs, files in os.walk(in_dir):
        out += [os.path.join(root, n) for n in files if n.endswith(".bc")]
    return sorted(out)


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--in", dest="in_dir", required=True, help="directory of .bc blobs (searched recursively)")
    ap.add_argument("--out", dest="out_dir", required=True, help="directory to write <key>.astc into")
    ap.add_argument("--jobs", type=int, default=os.cpu_count() or 1, help="parallel files (default: CPUs)")
    ap.add_argument("--config", help="port.toml (astc_*_block keys)")
    ap.add_argument("--quality", default="thorough",
                    help="astcenc preset name or 0-100 (default thorough, as the spec requires)")
    ap.add_argument("--force", action="store_true", help="re-encode keys that already have an .astc")
    ap.add_argument("--rltex", help="path to the rltex CLI")
    ap.add_argument("--astcenc", help="path to the astcenc CLI")
    ap.add_argument("--quiet", action="store_true")
    a = ap.parse_args(argv)

    rltex, astcenc = find_rltex(a.rltex), find_astcenc(a.astcenc)
    if not rltex:
        print("encode.py: rltex CLI not found (build with tests/textures/build.sh, or --rltex/RLTEX)", file=sys.stderr)
        return 2
    if not astcenc:
        print("encode.py: astcenc CLI not found (build astc-encoder, or --astcenc/ASTCENC)", file=sys.stderr)
        return 2
    if not os.path.isdir(a.in_dir):
        print("encode.py: --in %s is not a directory" % a.in_dir, file=sys.stderr)
        return 2
    if a.config and not os.path.isfile(a.config):
        print("encode.py: --config %s not found" % a.config, file=sys.stderr)
        return 2
    if a.config:  # reject an invalid port.toml up front instead of failing every file
        p = subprocess.run([rltex, "config", "--config", a.config], stdout=subprocess.PIPE,
                           stderr=subprocess.PIPE, text=True)
        if p.returncode != 0:
            print("encode.py: invalid config %s:\n%s" % (a.config, p.stderr.strip()), file=sys.stderr)
            return 2
        print("encode.py: config %s -> %s" % (a.config, p.stdout.strip()))
    os.makedirs(a.out_dir, exist_ok=True)

    files = collect(a.in_dir)
    tools = Tools(rltex, astcenc, a.config, a.quality)
    counts = {"encoded": 0, "skipped": 0, "unsupported": 0, "error": 0}
    lock = threading.Lock()
    print("encode.py: %d .bc file(s), astcenc=%s, preset=%s, jobs=%d" %
          (len(files), astcenc, a.quality, max(1, a.jobs)))
    with tempfile.TemporaryDirectory(prefix=".encode-", dir=a.out_dir) as tmp:
        def work(p):
            r = encode_one(tools, p, a.out_dir, tmp, a.force)
            with lock:
                counts[r[0]] += 1
                if not a.quiet or r[0] == "error":
                    print("  %-11s %s%s" % (r[0], r[1][:16] + ("..." if len(r[1]) > 16 else ""),
                                            ("  " + r[2]) if r[2] else ""))
            return r
        with concurrent.futures.ThreadPoolExecutor(max_workers=max(1, a.jobs)) as ex:
            list(ex.map(work, files))
    print("encode.py: encoded=%(encoded)d skipped=%(skipped)d unsupported=%(unsupported)d failed=%(error)d" % counts)
    return 1 if counts["error"] else 0


if __name__ == "__main__":
    sys.exit(main())
