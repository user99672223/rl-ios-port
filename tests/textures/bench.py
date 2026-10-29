#!/usr/bin/env python3
"""Optional benchmark (not part of run.sh): decode / library-ASTC-encode / CLI-ASTC-encode cost
and peak memory per texture size on THIS host, for the notes' device-cost estimate. Synthetic
data. Numbers from an x86-64 desktop are only a relative guide for the A16 efficiency cores.

  build/venv/bin/python tests/textures/bench.py [--sizes 512,1024,2048]
"""
import argparse
import os
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))
sys.path.insert(0, HERE)
import texgen as tg  # noqa: E402

RLTEX = os.environ.get("RLTEX", os.path.join(ROOT, "build/textures/rltex/rltex"))
ASTCENC = os.environ.get("ASTCENC", os.path.join(ROOT, "build/astcenc/Source/astcenc-avx2"))
TMP = os.path.join(ROOT, "build/tmp/bench")
os.makedirs(TMP, exist_ok=True)


def timed(cmd):
    """Returns (seconds, peak RSS MiB of that child process, via wait4's per-child rusage)."""
    t0 = time.time()
    p = subprocess.Popen(list(map(str, cmd)), stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    _pid, status, ru = os.wait4(p.pid, 0)
    p.returncode = 0  # reaped by wait4
    if status != 0:
        raise RuntimeError("%s failed (status %d)" % (cmd, status))
    return time.time() - t0, ru.ru_maxrss / 1024.0


def one(name, size):
    kind = tg.kind_of(name)
    img = tg.img_organic(size, size, seed=3)
    enc = "etcpak" if kind in ("BC1", "BC3", "BC7") else "ispc"
    blob = tg.encode(kind, img, enc)
    bc = os.path.join(TMP, "b.bc")
    open(bc, "wb").write(tg.bc_file(tg.DXGI[name], size, size, 0, 0, blob))
    raw = os.path.join(TMP, "b.raw")
    d_dec = timed([RLTEX, "decode", bc, raw])[0]
    d_lib = timed([RLTEX, "encode", "--quality", "fast", bc, os.path.join(TMP, "b.astc")])
    ktx = os.path.join(TMP, "b.ktx")
    out = subprocess.run([RLTEX, "prepare", bc, ktx], capture_output=True, text=True).stdout
    import json
    plan = json.loads(out)
    d_cli = timed([ASTCENC, plan["astcenc_mode"], ktx, os.path.join(TMP, "c.astc"), plan["block"], "-thorough", "-j", "1", "-silent"])
    astc = os.path.getsize(os.path.join(TMP, "b.astc"))
    print("%-5s %4d  BC %6.2f MiB  decoded %6.2f MiB  ASTC %5.2f MiB (%s) | decode+exec %.2fs | lib fast %.2fs  peak %.0f MiB | CLI thorough j1 %.2fs" % (
        name, size, len(blob) / 2**20, os.path.getsize(raw) / 2**20, astc / 2**20, plan["block"], d_dec, d_lib[0], d_lib[1], d_cli[0]))


def block_sweep():
    """PSNR (vs the bcdec decode) and bits per texel for BC1/BC4 across ASTC block sizes, thorough
    preset, to justify the 6x6 defaults. 128x128 synthetic images."""
    import json
    import numpy as np
    import test_crosscheck as tc
    print("block-size sweep, ASTC thorough, PSNR vs BC decode (dB); bpp = 128 / texels per block")
    for name, imgs in (("BC1", ("gradient", "wheel", "organic")), ("BC4", ("height", "organic"))):
        kind = tg.kind_of(name)
        key = "astc_bc1_block" if kind == "BC1" else "astc_bc4_block"
        for blk in ("4x4", "5x5", "6x6", "8x8"):
            cfg = os.path.join(TMP, "sweep.toml")
            open(cfg, "w").write('%s = "%s"\n' % (key, blk))
            vals = []
            for imgname in imgs:
                img = tc.ALL_IMG[imgname](128, 128)
                blob = tg.encode(kind, img, "etcpak" if kind == "BC4" else "ispc")
                bc = tc.write_bc("sw", tg.DXGI[name], 128, 128, blob)
                ref = tc.rl_decode(tg.DXGI[name], 128, 128, blob, tag="sw_ref")
                ktx = os.path.join(TMP, "sw.ktx")
                plan = json.loads(tc.run(RLTEX, "prepare", "--config", cfg, bc, ktx))
                astc = os.path.join(TMP, "sw.astc")
                cmd = [ASTCENC, plan["astcenc_mode"], ktx, astc, plan["block"], "-thorough", "-j", "1", "-silent"]
                if plan["esw"] != "rgba":
                    cmd += ["-esw", plan["esw"]]
                if plan["ssw"] != "rgba":
                    cmd += ["-ssw", plan["ssw"]]
                tc.run(*cmd)
                dec = tc.astc_decode_cli(astc, plan, "sw_dec")
                ncomp = 1 if kind == "BC4" else 4
                got = tc.apply_view(dec, plan["view_swizzle"], ncomp)
                r = ref[..., None] if ncomp == 1 else ref
                vals.append("%s %.1f" % (imgname, tg.psnr(got, r)))
            bx, by = map(int, blk.split("x"))
            print("  %-4s %-5s %5.2f bpp  %s" % (name, blk, 128.0 / (bx * by), "   ".join(vals)))


def swizzle_sweep():
    """Encode-swizzle comparison for BC4 / BC5 (the evidence behind the fixed rrr1 / rrrg mapping).
    PSNR vs the bcdec decode, astcenc thorough, 64x64 synthetic images; every row is
    (-esw, -ssw, view swizzle). `rgba` / `r001`... rows store the channels as they are."""
    import json
    import numpy as np
    import test_crosscheck as tc
    from PIL import Image
    ae = ASTCENC
    print("encode-swizzle sweep (PSNR dB vs BC decode)")
    cases = (("BC5", "normal", tg.img_normalmap), ("BC5", "organic", tg.img_organic),
             ("BC4", "height", tg.img_height), ("BC4", "organic", tg.img_organic))
    for kind, imgname, fn in cases:
        blob = tg.encode(kind, fn(64, 64), "etcpak")
        bc = tc.write_bc("zz", tg.DXGI[kind], 64, 64, blob)
        ref = tc.rl_decode(tg.DXGI[kind], 64, 64, blob, tag="zz_ref")
        ref = ref[..., None] if kind == "BC4" else ref
        ktx = os.path.join(TMP, "zz.ktx")
        tc.run(RLTEX, "prepare", bc, ktx)
        variants = ([("rgba", "rgba", "rg01"), ("rg01", "rg", "rg01"), ("rrrg", "ra", "ra01"), ("rrrg", "rgba", "ra01")]
                    if kind == "BC5" else
                    [("rgba", "rgba", "r001"), ("r001", "r", "r001"), ("rrr1", "r", "r001"), ("rrr1", "rgba", "r001")])
        for bs in ("4x4", "6x6"):
            parts = []
            for esw, ssw, view in variants:
                astc = os.path.join(TMP, "zz.astc")
                tc.run(ae, "-cl", ktx, astc, bs, "-thorough", "-j", "1", "-esw", esw, "-ssw", ssw, "-silent")
                png = os.path.join(TMP, "zz.png")
                tc.run(ae, "-dl", astc, png, "-silent")
                dec = np.array(Image.open(png).convert("RGBA"))
                got = tc.apply_view(dec, view, ref.shape[-1])
                parts.append("%s/%s %.2f" % (esw, ssw, tg.psnr(got, ref)))
            print("  %s %-7s %s  %s" % (kind, imgname, bs, "   ".join(parts)))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--sizes", default="512,1024,2048")
    ap.add_argument("--blocks", action="store_true", help="run the block-size sweep instead")
    ap.add_argument("--swizzle", action="store_true", help="run the BC4/BC5 encode-swizzle sweep instead")
    a = ap.parse_args()
    if a.swizzle:
        swizzle_sweep()
        return
    if a.blocks:
        block_sweep()
        return
    for size in map(int, a.sizes.split(",")):
        for name in ("BC1", "BC3", "BC7"):
            one(name, size)


if __name__ == "__main__":
    main()
