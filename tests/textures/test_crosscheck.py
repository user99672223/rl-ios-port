#!/usr/bin/env python3
"""Cross-check and round-trip tests for the rltex texture pipeline. Synthetic data only.

  1. bcdec (via `rltex decode`) vs independent decoders -- Pillow's DDS/BCn decoder and
     texture2ddecoder -- on BC blobs produced by two independent encoders (ISPC texcomp, etcpak)
     from synthetic images, at several sizes including non-multiples of 4.
  2. The same on random (but valid) blocks, which exercise every BC7/BC6H mode and partition.
  3. Key derivation: independent Python implementation vs the C library vs golden values.
  4. BC -> RGBA -> ASTC -> decode round trips with the real astcenc CLI (thorough, as the offline
     tool does) and with the library path (`rltex encode`, the on-device background encoder),
     PSNR measured against the bcdec decode of the same blob, with per-format thresholds.

Usage: test_crosscheck.py [--results FILE]   (env: RLTEX, ASTCENC; run via tests/textures/run.sh)
"""
import argparse
import json
import os
import struct
import subprocess
import sys
import time

import numpy as np
from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))
sys.path.insert(0, HERE)
import texgen as tg  # noqa: E402

RLTEX = os.environ.get("RLTEX", os.path.join(ROOT, "build/textures/rltex/rltex"))
ASTCENC = os.environ.get("ASTCENC", os.path.join(ROOT, "build/astcenc/Source/astcenc-avx2"))
TMP = os.path.join(ROOT, "build/tmp/crosscheck")
os.makedirs(TMP, exist_ok=True)

FAILS = []
ROWS = []  # (section, name, value, limit, ok)


def check(ok, what):
    if not ok:
        FAILS.append(what)
        print("  FAIL:", what)


def row(section, name, value, limit, ok, unit=""):
    ROWS.append((section, name, value, limit, ok))
    if not ok:
        FAILS.append("%s %s: %s (limit %s)" % (section, name, value, limit))
    print("  %-8s %-46s %8s%s  (limit %s) %s" % (section, name, ("%.2f" % value) if isinstance(value, float) else value,
                                                  unit, limit, "ok" if ok else "FAIL"))


def run(*cmd):
    p = subprocess.run(list(map(str, cmd)), capture_output=True, text=True)
    if p.returncode != 0:
        raise RuntimeError("%s -> %d\n%s%s" % (cmd, p.returncode, p.stdout, p.stderr))
    return p.stdout


# ----------------------------------------------------------------------------------------------
# rltex decode wrapper
# ----------------------------------------------------------------------------------------------
def write_bc(name, dxgi, w, h, blob, mip=0, sl=0):
    path = os.path.join(TMP, name + ".bc")
    with open(path, "wb") as f:
        f.write(tg.bc_file(dxgi, w, h, mip, sl, blob))
    return path


def rl_decode(dxgi, w, h, blob, tag="d"):
    """Decode with the C library. Returns array: RGBA8 (h,w,4) | R8 (h,w) | RG8 (h,w,2) | f16 (h,w,4)
    | int8 for SNORM."""
    bc = write_bc(tag, dxgi, w, h, blob)
    raw = os.path.join(TMP, tag + ".raw")
    out = run(RLTEX, "decode", bc, raw).split()
    fmt = out[1]
    data = open(raw, "rb").read()
    if fmt == "RGBA8_UNORM":
        return np.frombuffer(data, np.uint8).reshape(h, w, 4)
    if fmt == "R8_UNORM":
        return np.frombuffer(data, np.uint8).reshape(h, w)
    if fmt == "RG8_UNORM":
        return np.frombuffer(data, np.uint8).reshape(h, w, 2)
    if fmt == "R8_SNORM":
        return np.frombuffer(data, np.int8).reshape(h, w)
    if fmt == "RG8_SNORM":
        return np.frombuffer(data, np.int8).reshape(h, w, 2)
    if fmt == "RGBA16_FLOAT":
        return np.frombuffer(data, np.float16).reshape(h, w, 4)
    raise RuntimeError(fmt)


def maxdiff(a, b):
    return int(np.max(np.abs(a.astype(np.int32) - b.astype(np.int32))))


def f16_to_u8(a):
    return np.rint(np.clip(np.nan_to_num(a.astype(np.float32)), 0, 1) * 255).astype(np.int32)


# ----------------------------------------------------------------------------------------------
# 1. Decoder cross-check on encoder output
# ----------------------------------------------------------------------------------------------
# Tolerances (max abs difference in 8-bit units). BC7 is an integer-exact spec so we demand 0.
# BC1..BC5 differ by <=1 between implementations only through rounding of the 1/3, 2/3 and k/7
# interpolants; BC6H is compared after clamping to [0,1] (the other decoders output 8 bit).
TOL = {"BC1": 1, "BC2": 1, "BC3": 1, "BC4": 1, "BC5": 1, "BC6H": 1, "BC7": 0}


def compare_to_pillow(kind, name, blob, w, h, ours):
    pl = tg.decode_pillow(name, blob, w, h)
    if kind in ("BC1", "BC2", "BC3", "BC7"):
        return maxdiff(ours, pl)
    if kind == "BC4":
        return maxdiff(ours, pl)
    if kind == "BC5":
        return maxdiff(ours, pl[..., :2])
    if kind == "BC6H":
        return maxdiff(f16_to_u8(ours[..., :3]), pl[..., :3])
    raise ValueError(kind)


def compare_to_t2d(kind, blob, w, h, ours):
    t = tg.decode_t2d(kind, blob, w, h)
    if kind in ("BC1", "BC3", "BC7"):
        return maxdiff(ours[..., :3], t[..., :3]), maxdiff(ours[..., 3], t[..., 3])
    if kind == "BC4":
        return maxdiff(ours, t[..., 0]), 0
    if kind == "BC5":
        return maxdiff(ours, t[..., :2]), 0
    if kind == "BC6H":
        return maxdiff(f16_to_u8(ours[..., :3]), t[..., :3]), 0
    raise ValueError(kind)


def test_decode_crosscheck():
    print("== 1. bcdec vs independent decoders on encoder output")
    sizes = [(64, 64), (37, 23), (13, 5), (4, 4), (1, 1)]
    worst = {}
    n = 0
    plan = []
    for kind in ("BC1", "BC2", "BC3", "BC4", "BC5", "BC7"):
        encs = ["ispc"] if kind == "BC2" else ["ispc", "etcpak"]
        for enc in encs:
            for imgname, fn in list(tg.LDR_IMAGES.items()) + [("normal", tg.img_normalmap), ("height", tg.img_height)]:
                for (w, h) in sizes:
                    plan.append((kind, enc, imgname, fn, w, h))
    for (kind, enc, imgname, fn, w, h) in plan:
        img = fn(w, h)
        blob = tg.encode(kind, img, enc)
        dx = tg.DXGI[kind]
        ours = rl_decode(dx, w, h, blob)
        d_pl = compare_to_pillow(kind, kind, blob, w, h, ours)
        key = (kind, "pillow")
        worst[key] = max(worst.get(key, 0), d_pl)
        if kind != "BC2":
            d_t, d_a = compare_to_t2d(kind, blob, w, h, ours)
            worst[(kind, "t2d")] = max(worst.get((kind, "t2d"), 0), d_t)
            worst[(kind, "t2d-alpha")] = max(worst.get((kind, "t2d-alpha"), 0), d_a)
        n += 1
    for kind in ("BC6H",):
        for enc in ("ispc",):
            for (w, h) in sizes:
                for imgname, fn in (("hdr", tg.img_hdr), ("wheel", tg.img_wheel), ("gradient", tg.img_gradient)):
                    img = fn(w, h)
                    blob = tg.encode(kind, img, enc)
                    ours = rl_decode(tg.DXGI[kind], w, h, blob)
                    worst[(kind, "pillow")] = max(worst.get((kind, "pillow"), 0), compare_to_pillow(kind, kind, blob, w, h, ours))
                    d_t, _ = compare_to_t2d(kind, blob, w, h, ours)
                    worst[(kind, "t2d")] = max(worst.get((kind, "t2d"), 0), d_t)
                    n += 1
    print("  %d texture/encoder/size combinations compared" % n)
    for (kind, dec), v in sorted(worst.items()):
        if dec == "t2d-alpha":
            continue
        if (kind, dec) == ("BC6H", "t2d"):
            # texture2ddecoder disagrees with both bcdec and Pillow (which agree within 1) on some
            # BC6H blocks; reported, not asserted. Pillow is the arbiter.
            row("decode", "BC6H vs t2d (informational, not asserted)", v, "n/a", True)
            continue
        row("decode", "%s vs %s (encoder output): max |diff|" % (kind, dec), v, TOL[kind], v <= TOL[kind])


def test_encoders_sane():
    """Sanity: the decoded image resembles the source (guards against a broken encoder or a
    decoder that agrees with the others only by being equally wrong). Opaque smooth images."""
    print("== 1b. decoded vs original source image (encoder + decoder sanity)")
    w, h = 64, 64
    floors = {"BC1": 30.0, "BC2": 25.0, "BC3": 30.0, "BC4": 30.0, "BC5": 38.0, "BC7": 38.0}
    for kind in ("BC1", "BC2", "BC3", "BC4", "BC5", "BC7"):
        # The ISPC python binding's BC4/BC5 do not encode the channels they are given in a way
        # that matches the source (PSNR ~5 dB vs every source channel although all decoders agree
        # on the blocks), so quality sanity for BC4/BC5 uses etcpak only.
        for enc in (["ispc"] if kind == "BC2" else (["etcpak"] if kind in ("BC4", "BC5") else ["ispc", "etcpak"])):
            img = tg.img_gradient(w, h) if kind not in ("BC4", "BC5") else (tg.img_height(w, h) if kind == "BC4" else tg.img_normalmap(w, h))
            blob = tg.encode(kind, img, enc)
            ours = rl_decode(tg.DXGI[kind], w, h, blob)
            src = tg.to_u8(img)
            if kind == "BC4":
                p = tg.psnr(ours, src[..., 0])
            elif kind == "BC5":
                p = tg.psnr(ours, src[..., :2])
            elif kind == "BC1":
                p = tg.psnr(ours[..., :3], src[..., :3])
            else:
                p = tg.psnr(ours, src)
            row("sanity", "%s/%s gradient: PSNR vs source" % (kind, enc), float(p), ">=%.0f dB" % floors[kind], p >= floors[kind], " dB")
    # HDR: relative error in linear light
    for imgname, fn in (("hdr", tg.img_hdr),):
        img = fn(w, h)
        blob = tg.encode("BC6H", img)
        ours = rl_decode(tg.DXGI["BC6H"], w, h, blob).astype(np.float32)[..., :3]
        src = img[..., :3].astype(np.float16).astype(np.float32)
        rel = np.abs(ours - src) / np.maximum(src, 0.05)
        row("sanity", "BC6H/ispc hdr: median relative error", float(np.median(rel)), "<0.02", float(np.median(rel)) < 0.02)
        row("sanity", "BC6H/ispc hdr: 99th pct relative error", float(np.percentile(rel, 99)), "<0.15", float(np.percentile(rel, 99)) < 0.15)
        check(ours.max() > 8.0, "BC6H decode preserved HDR range above 8 (got %.2f)" % ours.max())


# ----------------------------------------------------------------------------------------------
# 2. random blocks
# ----------------------------------------------------------------------------------------------
def random_blob(kind, w, h, rng):
    n = tg.blob_size(kind, w, h)
    b = bytearray(rng.integers(0, 256, n, dtype=np.uint8).tobytes())
    bb = tg.BLOCK_BYTES[kind]
    for i in range(0, n, bb):
        if kind == "BC7" and b[i] == 0:
            b[i] = 1 + (b[i + 1] % 255)  # mode 8 does not exist
        if kind == "BC6H":
            while (b[i] & 0x1F) in (0x13, 0x17, 0x1B, 0x1F):  # reserved 5 bit modes
                b[i] = (b[i] + 1) & 0xFF
    return bytes(b)


def test_random_blocks():
    print("== 2. random blocks (all BC7/BC6H modes), bcdec vs Pillow")
    rng = np.random.default_rng(2024)
    w = h = 128  # 1024 blocks per format
    for name in ("BC1", "BC2", "BC3", "BC4", "BC5", "BC6H", "BC7", "BC4_SNORM", "BC5_SNORM"):
        kind = tg.kind_of(name)
        blob = random_blob(kind, w, h, rng)
        ours = rl_decode(tg.DXGI[name], w, h, blob, tag="rnd_" + name)
        if name == "BC4_SNORM":
            d = maxdiff(ours, tg.ref_bc45(blob, w, h, 1, True)[..., 0])
        elif name == "BC5_SNORM":
            # Pillow biases signed output in a way that is off by up to 2; use the pure-Python
            # D3D-spec reference instead.
            d = maxdiff(ours, tg.ref_bc45(blob, w, h, 2, True))
        else:
            d = compare_to_pillow(kind, name, blob, w, h, ours)
        tol = TOL[kind]
        ref_name = "spec-ref" if name.endswith("SNORM") else "Pillow"
        row("random", "%s %d blocks: max |diff| vs %s" % (name, (w // 4) * (h // 4), ref_name), d, tol, d <= tol)
        if kind in ("BC4", "BC5") and not name.endswith("SNORM"):
            nch = 1 if kind == "BC4" else 2
            ref = tg.ref_bc45(blob, w, h, nch, False)
            dd = maxdiff(ours.reshape(h, w, nch), ref)
            row("random", "%s %d blocks: max |diff| vs python spec reference" % (name, (w // 4) * (h // 4)), dd, 1, dd <= 1)
        if name == "BC7":
            modes = np.bincount([min((blob[i] & -blob[i]).bit_length() - 1, 8) for i in range(0, len(blob), 16)], minlength=9)
            check(all(modes[:8] > 0), "random BC7 covered all 8 modes: %s" % modes[:8].tolist())
            print("         BC7 mode histogram:", modes[:8].tolist())
    # exact 0..255 coverage of the endpoint expansion: every 565 value in BC1 vs Pillow
    c = bytearray()
    for v in range(0, 65536, 17):
        c += struct.pack("<HHI", 0xFFFF, v, 0xE4E4E4E4 & 0xFFFFFFFF)
    blob = bytes(c)
    nb = len(blob) // 8
    wb = nb  # one row of blocks
    ours = rl_decode(tg.DXGI["BC1"], wb * 4, 4, blob, tag="bc1_565")
    d = compare_to_pillow("BC1", "BC1", blob, wb * 4, 4, ours)
    row("random", "BC1 all-565 sweep (%d blocks, c0=white)" % nb, d, 1, d <= 1)


def find_external():
    cands = [os.environ.get("RLTEX_EXTERNAL"), os.path.join(ROOT, "external")]
    common = subprocess.run(["git", "-C", ROOT, "rev-parse", "--git-common-dir"], capture_output=True, text=True).stdout.strip()
    if common:
        cands.append(os.path.join(os.path.dirname(os.path.abspath(os.path.join(ROOT, common))), "external"))
    for c in cands:
        if c and os.path.isfile(os.path.join(c, "bcdec", "bcdec.h")):
            return c
    return None


def test_signed_bc6h_real():
    """Signed BC6H on a real encoded image shipped with the bcdec test suite (upstream test asset,
    not game data; read in place, never copied). Pillow's signed BC6H is unreliable near zero
    (it returns 255 where bcdec and the neighbouring texels say ~1e-4; random signed blocks also
    disagree wildly), so compare only texels whose RGB are all >= 0.01 and check the rest are
    tiny; this validates the signed path on well-formed data."""
    print("== 2b. signed BC6H (SF16) on the bcdec test image vs Pillow")
    ext = find_external()
    path = ext and os.path.join(ext, "bcdec", "test_images", "lythwood_room_1k_bc6h_signed.dds")
    if not path or not os.path.isfile(path):
        print("  (bcdec test image not found, skipped)")
        return
    d = open(path, "rb").read()
    h, w = struct.unpack("<II", d[12:20])
    check(d[84:88] == b"DX10" and struct.unpack("<I", d[128:132])[0] == 96, "test image is DXGI BC6H_SF16")
    blob = d[148:]
    ours = rl_decode(tg.DXGI["BC6H_SF16"], w, h, blob, tag="sf_real")
    pl = tg.decode_pillow("BC6H_SF16", blob, w, h).astype(np.int32)
    o = ours[..., :3].astype(np.float32)
    mask = np.all(o >= 0.01, axis=-1)
    diff = np.abs(f16_to_u8(ours[..., :3]) - pl[..., :3])[mask]
    row("signed", "BC6H_SF16 real image: %d of %d texels, max |diff| vs Pillow" % (int(mask.sum()), mask.size), int(diff.max()), 1, int(diff.max()) <= 1)
    check(float(o.min()) > -0.01, "signed image decodes without large negatives (min %.5f)" % float(o.min()))
    check(float(o.max()) > 0.9, "signed image has bright texels (max %.3f)" % float(o.max()))


# ----------------------------------------------------------------------------------------------
# 3. keys
# ----------------------------------------------------------------------------------------------
def test_keys():
    print("== 3. key derivation (Python reference vs C library vs golden)")
    # golden constants, also asserted in test_core.c
    hdr = tg.key_header(0x47, 8, 4, 0, 0, 16)
    check(hdr.hex() == "524c4b3147000000080000000400000000000000000000001000000000000000", "golden key header")
    check(tg.key_of(0x47, 8, 4, 0, 0, bytes(range(16))) == "7a4946eadfd85f1cb288b73b7d5263cf0308dc4c9f5c66aa5ee59d14628b69c6", "golden key 1")
    check(tg.key_of(0x4D, 16, 16, 3, 5, bytes((i * 7 + 3) & 255 for i in range(256))) == "9f0b7f6a22c6640ab7bdd141d9ac78a3272b0e9cc0274ee51b93b7ca432e00f2", "golden key 2")
    rng = np.random.default_rng(3)
    n = 0
    for name in ("BC1", "BC1_SRGB", "BC2", "BC3", "BC4", "BC5", "BC6H", "BC7", "BC7_SRGB"):
        for (w, h, mip, sl) in ((16, 16, 0, 0), (37, 23, 3, 7), (1, 1, 11, 0), (4096, 8, 0, 63)):
            kind = tg.kind_of(name)
            blob = rng.integers(0, 256, tg.blob_size(kind, w, h), dtype=np.uint8).tobytes()
            p = os.path.join(TMP, "k.bc")
            open(p, "wb").write(tg.bc_file(tg.DXGI[name], w, h, mip, sl, blob))
            out = run(RLTEX, "key", p).split()
            want = tg.key_of(tg.DXGI[name], w, h, mip, sl, blob)
            check(out[0] == want, "C key == Python key for %s %dx%d mip%d slice%d" % (name, w, h, mip, sl))
            check(out[1] == "dxgi=%d" % tg.DXGI[name], "dxgi in key output")
            n += 1
    print("  %d key comparisons" % n)
    row("keys", "C == Python for all formats/sizes/mips/slices", n, n, True)


# ----------------------------------------------------------------------------------------------
# 4. ASTC round trips
# ----------------------------------------------------------------------------------------------
def read_ktx_f16(path):
    d = open(path, "rb").read()
    assert d[:12] == bytes([0xAB, 0x4B, 0x54, 0x58, 0x20, 0x31, 0x31, 0xBB, 0x0D, 0x0A, 0x1A, 0x0A])
    vals = struct.unpack("<13I", d[12:64])
    gl_type, gl_format, w, h, kv = vals[1], vals[3], vals[6], vals[7], vals[12]
    off = 64 + kv
    size = struct.unpack("<I", d[off:off + 4])[0]
    data = d[off + 4:off + 4 + size]
    # the astcenc KTX writer drops the alpha channel when it is all 1 (RGB, 0x1907)
    assert gl_type == 0x140B and gl_format in (0x1907, 0x1908), (hex(gl_type), hex(gl_format))
    return np.frombuffer(data, np.float16).reshape(h, w, 3 if gl_format == 0x1907 else 4)


def astc_decode_cli(astc_path, plan, tag):
    """Decode with the astcenc CLI and return an array in the same layout as rl_decode()."""
    mode = plan["astcenc_mode"]
    dmode = "-d" + mode[2]
    if plan["input"] == "rgba16f":
        out = os.path.join(TMP, tag + ".ktx")
        run(ASTCENC, dmode, astc_path, out, "-silent")
        return read_ktx_f16(out)
    out = os.path.join(TMP, tag + ".png")
    run(ASTCENC, dmode, astc_path, out, "-silent")
    return np.array(Image.open(out).convert("RGBA"))


def apply_view(tex, view, ncomp):
    """Apply the runtime's view swizzle and keep the first ncomp channels (what the BC texture
    would have sampled as)."""
    idx = {"r": 0, "g": 1, "b": 2, "a": 3}
    chans = []
    for c in view[:ncomp]:
        chans.append(tex[..., idx[c]] if c in idx else (np.zeros_like(tex[..., 0]) if c == "0" else np.full_like(tex[..., 0], 255)))
    return np.stack(chans, -1)


def hdr_metric(a, b):
    """PSNR of log-encoded HDR (log2(1+x) mapped so 16 -> 255): a perceptual-ish HDR error."""
    def enc(x):
        return np.log2(1.0 + np.maximum(x.astype(np.float64), 0.0)) / np.log2(17.0) * 255.0
    return tg.psnr(enc(a), enc(b))


# Thresholds in dB, (smooth content, detailed content). "smooth" = gradient, wheel, height, normal
# map, HDR; "detailed" = organic (multi-octave, near-aliased at 37x23) and checker (hard edges).
# Each is the minimum measured over the whole set (see docs/notes/textures.md, "Measured PSNR")
# minus about 2 dB, rounded down. They guard against regressions (wrong swizzle, profile, block
# size, corrupt output), not against encoder noise.
THRESH = {
    ("BC1", "cli"): (33, 21), ("BC1", "lib"): (31, 20),
    ("BC1_SRGB", "cli"): (33, 21), ("BC1_SRGB", "lib"): (31, 20),
    ("BC2", "cli"): (44, 30), ("BC2", "lib"): (35, 26),
    ("BC3", "cli"): (44, 30), ("BC3", "lib"): (36, 26),
    ("BC3_SRGB", "cli"): (57, 30), ("BC3_SRGB", "lib"): (56, 26),
    ("BC4", "cli"): (37, 25), ("BC4", "lib"): (34, 25),
    ("BC5", "cli"): (40, 26), ("BC5", "lib"): (39, 25),
    ("BC6H", "cli"): (46, 0), ("BC6H", "lib"): (43, 0),
    ("BC7", "cli"): (39, 27), ("BC7", "lib"): (35, 25),
    ("BC7_SRGB", "cli"): (40, 27), ("BC7_SRGB", "lib"): (37, 25),
}
SMOOTH = {"gradient", "wheel", "height", "normal", "hdr"}

# images per format; sizes include non-multiples of the block size
ASTC_SETS = {
    "BC1": ["gradient", "organic", "checker", "wheel"], "BC1_SRGB": ["gradient", "organic", "wheel"],
    "BC2": ["gradient", "organic"], "BC3": ["gradient", "organic", "checker", "wheel"],
    "BC3_SRGB": ["organic", "wheel"], "BC4": ["height", "organic"], "BC5": ["normal", "organic"],
    "BC6H": ["hdr"], "BC7": ["gradient", "organic", "wheel"], "BC7_SRGB": ["organic", "wheel"],
}
ALL_IMG = dict(tg.LDR_IMAGES, normal=tg.img_normalmap, height=tg.img_height, hdr=tg.img_hdr)
ASTC_SIZES = [(64, 64), (37, 23)]


def test_astc_roundtrip(config=None):
    print("== 4. BC -> ASTC round trip (astcenc CLI thorough = offline path, library fast = device path)")
    measured = {}
    for name, images in ASTC_SETS.items():
        kind = tg.kind_of(name)
        for imgname in images:
            for (w, h) in ASTC_SIZES:
                img = ALL_IMG[imgname](w, h)
                blob = tg.encode(kind, img)
                bc = write_bc("rt", tg.DXGI[name], w, h, blob)
                ref = rl_decode(tg.DXGI[name], w, h, blob, tag="rt_ref")
                ktx = os.path.join(TMP, "rt.ktx")
                cfgargs = ["--config", config] if config else []
                plan = json.loads(run(RLTEX, "prepare", *cfgargs, bc, ktx))
                check(plan["astc"] is True, "%s has an ASTC mapping" % name)
                # expected mapping (spec section 6.5 defaults)
                want_block = {"BC1": "6x6", "BC4": "6x6"}.get(kind, "4x4")
                if not config:
                    check(plan["block"] == want_block, "%s block %s == %s" % (name, plan["block"], want_block))
                want_mode = "-ch" if kind == "BC6H" else ("-cs" if name.endswith("SRGB") else "-cl")
                check(plan["astcenc_mode"] == want_mode, "%s mode %s == %s" % (name, plan["astcenc_mode"], want_mode))

                # ---- offline path: real astcenc CLI, thorough preset
                astc = os.path.join(TMP, "rt_cli.astc")
                cmd = [ASTCENC, plan["astcenc_mode"], ktx, astc, plan["block"], "-thorough", "-j", "1", "-silent"]
                if plan["esw"] != "rgba":
                    cmd += ["-esw", plan["esw"]]
                if plan["ssw"] != "rgba":
                    cmd += ["-ssw", plan["ssw"]]
                run(*cmd)
                # ---- device path: the C library at the runtime preset (fast)
                astc_lib = os.path.join(TMP, "rt_lib.astc")
                run(RLTEX, "encode", *cfgargs, "--quality", "fast", bc, astc_lib)
                for path, f in (("cli", astc), ("lib", astc_lib)):
                    d = open(f, "rb").read()
                    bx, by = (int(v) for v in plan["block"].split("x"))
                    check(d[:4] == bytes([0x13, 0xAB, 0xA1, 0x5C]), "%s/%s astc magic" % (name, path))
                    check((d[4], d[5], d[6]) == (bx, by, 1), "%s/%s astc block bytes" % (name, path))
                    check(d[7] | d[8] << 8 | d[9] << 16 == w and d[10] | d[11] << 8 | d[12] << 16 == h and d[13] == 1, "%s/%s astc dims" % (name, path))
                    check(len(d) == 16 + ((w + bx - 1) // bx) * ((h + by - 1) // by) * 16, "%s/%s astc size" % (name, path))
                    dec = astc_decode_cli(f, plan, "rt_dec")
                    if plan["input"] == "rgba16f":
                        got = dec[..., :3].astype(np.float32)
                        p = hdr_metric(got, ref[..., :3].astype(np.float32))
                    else:
                        ncomp = 1 if kind == "BC4" else (2 if kind == "BC5" else 4)
                        got = apply_view(dec, plan["view_swizzle"], ncomp)
                        r = ref if ncomp == 4 else (ref[..., None] if ncomp == 1 else ref)
                        if kind == "BC1":  # punch-through alpha only matters if present; compare all 4
                            pass
                        p = tg.psnr(got, r)
                    k = (name, path)
                    measured.setdefault(k, []).append((p, "%s %dx%d" % (imgname, w, h), plan["block"], imgname in SMOOTH))
    print()
    for (name, path), vals in sorted(measured.items()):
        blk = vals[0][2]
        for cls, idx in (("smooth", 0), ("detail", 1)):
            sel = [v for v in vals if v[3] == (cls == "smooth")]
            if not sel:
                continue
            lo, hi = min(v[0] for v in sel), max(v[0] for v in sel)
            lim = THRESH[(name, path)][idx]
            worst = min(sel)[1]
            row("astc", "%-9s %-3s %s %-6s [%.1f..%.1f] worst=%s" % (name, path, blk, cls, lo, hi, worst), float(lo), ">=%.0f dB" % lim, lo >= lim, " dB")
    return measured


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--results", help="write the measured table here")
    ap.add_argument("--only", default="", help="comma list of sections: decode,sanity,random,keys,astc")
    a = ap.parse_args()
    only = set(filter(None, a.only.split(",")))
    for tool in (RLTEX, ASTCENC):
        if not os.access(tool, os.X_OK):
            print("missing tool:", tool, "(run tests/textures/build.sh)")
            return 2
    t0 = time.time()
    if not only or "decode" in only:
        test_decode_crosscheck()
    if not only or "sanity" in only:
        test_encoders_sane()
    if not only or "random" in only:
        test_random_blocks()
    if not only or "random" in only:
        test_signed_bc6h_real()
    if not only or "keys" in only:
        test_keys()
    if not only or "astc" in only:
        test_astc_roundtrip()
    print("\ntest_crosscheck: %d rows, %d failures, %.1fs" % (len(ROWS), len(FAILS), time.time() - t0))
    if a.results:
        with open(a.results, "w") as f:
            for r in ROWS:
                f.write("%s\t%s\t%s\t%s\t%s\n" % (r[0], r[1], r[2], r[3], "ok" if r[4] else "FAIL"))
    for f in FAILS:
        print("FAIL:", f)
    return 1 if FAILS else 0


if __name__ == "__main__":
    sys.exit(main())
