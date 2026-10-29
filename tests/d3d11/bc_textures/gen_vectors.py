#!/usr/bin/env python3
"""Generates vectors.h for the D3D11 BC texture test: hand-built BC blocks and the colours a
conforming decoder returns for them (Direct3D 11 functional spec, "Texture Block Compression").

Everything is formula driven (a fixed LCG), stdlib only, no game data. The expected values are
an independent pure-Python implementation of the decode rules; the test compares them with what
the GPU returns (native BC) or what the emulation uploads, within a tolerance that covers the
rounding the spec leaves to implementations.

Two content modes per subresource:
  pattern   every 4x4 block differs (endpoints, indices, alpha), so block order, texel order inside
            a block and the edge handling of non-multiple-of-4 sizes are all checked
  flat      one colour for the whole subresource (still a valid block of the format), which the
            ASTC re-encode of the cache run reproduces to within a few levels

Usage: gen_vectors.py > vectors.h
"""
import math
import struct
import sys

VARIANTS = [("A", 8, 8, 4), ("B", 10, 6, 4), ("C", 2, 2, 2)]  # name, width, height, mips

# name, texture DXGI format, SRV DXGI format, block kind, sRGB view
FORMATS = [
    ("BC1_UNORM", "DXGI_FORMAT_BC1_UNORM", "DXGI_FORMAT_BC1_UNORM", "BC1", False),
    ("BC1_UNORM_SRGB", "DXGI_FORMAT_BC1_UNORM_SRGB", "DXGI_FORMAT_BC1_UNORM_SRGB", "BC1", True),
    ("BC1_TYPELESS_SRGB_VIEW", "DXGI_FORMAT_BC1_TYPELESS", "DXGI_FORMAT_BC1_UNORM_SRGB", "BC1", True),
    ("BC2_UNORM", "DXGI_FORMAT_BC2_UNORM", "DXGI_FORMAT_BC2_UNORM", "BC2", False),
    ("BC3_UNORM", "DXGI_FORMAT_BC3_UNORM", "DXGI_FORMAT_BC3_UNORM", "BC3", False),
    ("BC4_UNORM", "DXGI_FORMAT_BC4_UNORM", "DXGI_FORMAT_BC4_UNORM", "BC4", False),
    ("BC5_UNORM", "DXGI_FORMAT_BC5_UNORM", "DXGI_FORMAT_BC5_UNORM", "BC5", False),
    ("BC6H_UF16", "DXGI_FORMAT_BC6H_UF16", "DXGI_FORMAT_BC6H_UF16", "BC6H", False),
    ("BC7_UNORM", "DXGI_FORMAT_BC7_UNORM", "DXGI_FORMAT_BC7_UNORM", "BC7", False),
    ("BC7_UNORM_SRGB", "DXGI_FORMAT_BC7_UNORM_SRGB", "DXGI_FORMAT_BC7_UNORM_SRGB", "BC7", True),
]
BLOCK_BYTES = {"BC1": 8, "BC2": 16, "BC3": 16, "BC4": 8, "BC5": 16, "BC6H": 16, "BC7": 16}
KIND_ID = {"BC1": 1, "BC2": 2, "BC3": 3, "BC4": 4, "BC5": 5, "BC6H": 6, "BC7": 7}


class Rng:
    def __init__(self, seed):
        self.s = (seed * 2654435761 + 12345) & 0xFFFFFFFF

    def next(self, n):
        self.s = (self.s * 1664525 + 1013904223) & 0xFFFFFFFF
        return (self.s >> 8) % n


# --------------------------------------------------------------------------------------------
# colour helpers
# --------------------------------------------------------------------------------------------
def rgb565(r, g, b):
    return (r << 11) | (g << 5) | b


def exp565(c):
    r, g, b = (c >> 11) & 31, (c >> 5) & 63, c & 31
    return ((r << 3) | (r >> 2), (g << 2) | (g >> 4), (b << 3) | (b >> 2))


def rnd_div(a, b):
    return (2 * a + b) // (2 * b)  # round to nearest


def pack_idx2(idx):
    v = 0
    for t, i in enumerate(idx):
        v |= (i & 3) << (2 * t)
    return v


def pack_idx3(idx):
    v = 0
    for t, i in enumerate(idx):
        v |= (i & 7) << (3 * t)
    return v


# --------------------------------------------------------------------------------------------
# BC1 colour block / BC2-BC3 colour half
# --------------------------------------------------------------------------------------------
def color_block(c0, c1, idx):
    return struct.pack("<HHI", c0, c1, pack_idx2(idx))


def color_decode(blk, four_color_only):
    c0, c1, bits = struct.unpack("<HHI", blk)
    e0, e1 = exp565(c0), exp565(c1)
    if c0 > c1 or four_color_only:
        pal = [e0 + (255,), e1 + (255,),
               tuple(rnd_div(2 * a + b, 3) for a, b in zip(e0, e1)) + (255,),
               tuple(rnd_div(a + 2 * b, 3) for a, b in zip(e0, e1)) + (255,)]
    else:
        pal = [e0 + (255,), e1 + (255,),
               tuple(rnd_div(a + b, 2) for a, b in zip(e0, e1)) + (255,), (0, 0, 0, 0)]
    return [pal[(bits >> (2 * t)) & 3] for t in range(16)]


def color_choice(rng, flat, kind, seed):
    """(c0, c1, indices) of a colour block."""
    if flat:
        return rgb565(21, 42, 11), 0, [0] * 16
    for _ in range(8):
        c0 = rgb565(rng.next(32), rng.next(64), rng.next(32))
        c1 = rgb565(rng.next(32), rng.next(64), rng.next(32))
        if c0 != c1:
            break
    idx = [(t + seed) % 4 for t in range(16)]
    if kind == "BC1" and seed % 7 == 3:  # 3-colour mode with a transparent index
        if c0 > c1:
            c0, c1 = c1, c0
        if c0 == c1:
            c1 = c0 + 1
        idx = [(t * 3 + seed) % 4 for t in range(16)]
    else:
        if c0 < c1:
            c0, c1 = c1, c0
        if c0 == c1:
            c0 = c1 + 1
    return c0, c1, idx


# --------------------------------------------------------------------------------------------
# BC3 alpha / BC4 / BC5 channel block
# --------------------------------------------------------------------------------------------
def chan_block(a0, a1, idx):
    return bytes([a0, a1]) + pack_idx3(idx).to_bytes(6, "little")


def chan_decode(blk):
    a0, a1 = blk[0], blk[1]
    bits = int.from_bytes(blk[2:8], "little")
    if a0 > a1:
        pal = [a0, a1] + [rnd_div((7 - k) * a0 + k * a1, 7) for k in range(1, 7)]
    else:
        pal = [a0, a1] + [rnd_div((5 - k) * a0 + k * a1, 5) for k in range(1, 5)] + [0, 255]
    return [pal[(bits >> (3 * t)) & 7] for t in range(16)]


def chan_choice(rng, flat, seed):
    if flat:
        return 150, 20, [0] * 16
    a0, a1 = rng.next(256), rng.next(256)
    if seed % 3 == 0 and a0 > a1:
        a0, a1 = a1, a0  # 6-interpolant mode with the 0 / 255 entries
    return a0, a1, [(t * 3 + seed) % 8 for t in range(16)]


# --------------------------------------------------------------------------------------------
# bit-level blocks: BC7 mode 6, BC6H mode 11
# --------------------------------------------------------------------------------------------
class Bits:
    def __init__(self):
        self.v = 0
        self.n = 0

    def put(self, val, n):
        assert 0 <= val < (1 << n), (val, n)
        self.v |= val << self.n
        self.n += n

    def data(self):
        assert self.n == 128, self.n
        return self.v.to_bytes(16, "little")


W4 = [0, 4, 9, 13, 17, 21, 26, 30, 34, 38, 43, 47, 51, 55, 60, 64]


def bc7_block(ep0, ep1, p0, p1, idx):
    """Mode 6: one subset, RGBA 7.7.7.7 + one p-bit per endpoint, 4-bit indices."""
    assert idx[0] < 8
    b = Bits()
    b.put(1 << 6, 7)  # mode 6 = six zero bits then a one
    for c in range(4):
        b.put(ep0[c], 7)
        b.put(ep1[c], 7)
    b.put(p0, 1)
    b.put(p1, 1)
    for t, i in enumerate(idx):
        b.put(i, 3 if t == 0 else 4)
    return b.data()


def bc7_decode(ep0, ep1, p0, p1, idx):
    e0 = [(c << 1) | p0 for c in ep0]
    e1 = [(c << 1) | p1 for c in ep1]
    return [tuple((e0[c] * (64 - W4[i]) + e1[c] * W4[i] + 32) >> 6 for c in range(4)) for i in idx]


def bc7_choice(rng, flat, seed):
    if flat:
        e = [60, 40, 90, 120]
        return e, e, 1, 1, [0] * 16
    ep0 = [rng.next(128) for _ in range(4)]
    ep1 = [rng.next(128) for _ in range(4)]
    idx = [(t * 3 + seed) % 16 for t in range(16)]
    idx[0] &= 7
    return ep0, ep1, rng.next(2), rng.next(2), idx


def bc6h_block(c0, c1, idx):
    """Mode 11 (0b00011): one subset, endpoints 10 bits each, no delta coding, 4-bit indices."""
    assert idx[0] < 8
    b = Bits()
    b.put(0b00011, 5)
    for c in c0:
        b.put(c, 10)
    for c in c1:
        b.put(c, 10)
    for t, i in enumerate(idx):
        b.put(i, 3 if t == 0 else 4)
    return b.data()


def unq10(v):
    if v == 0:
        return 0
    if v == 1023:
        return 0xFFFF
    return ((v << 16) + 0x8000) >> 10


def half_to_float(h):
    return struct.unpack("<e", struct.pack("<H", h))[0]


def bc6h_decode(c0, c1, idx):
    out = []
    for i in idx:
        px = []
        for ch in range(3):
            a, b = unq10(c0[ch]), unq10(c1[ch])
            v = (a * (64 - W4[i]) + b * W4[i] + 32) >> 6
            px.append(half_to_float((v * 31) >> 6))
        out.append(px)
    return out


def bc6h_choice(rng, flat, seed):
    lo, hi = 30, 470  # half values of roughly 0.06 .. 1.0 so the RGBA8 target does not clamp
    if flat:
        c = [200, 300, 400]
        return c, c, [0] * 16
    c0 = [lo + rng.next(hi - lo) for _ in range(3)]
    c1 = [lo + rng.next(hi - lo) for _ in range(3)]
    idx = [(t * 5 + seed) % 16 for t in range(16)]
    idx[0] &= 7
    return c0, c1, idx


# --------------------------------------------------------------------------------------------
# one 4x4 block: (bytes, 16 RGBA8 expected before sRGB handling)
# --------------------------------------------------------------------------------------------
def make_block(kind, seed, flat):
    rng = Rng(seed + 1000 * KIND_ID[kind])
    if kind == "BC1":
        c0, c1, idx = color_choice(rng, flat, kind, seed)
        blk = color_block(c0, c1, idx)
        return blk, color_decode(blk, False)
    if kind in ("BC2", "BC3"):
        c0, c1, idx = color_choice(rng, flat, kind, seed)
        cblk = color_block(c0, c1, idx)
        col = color_decode(cblk, True)
        if kind == "BC2":
            nib = [15 - ((t + seed) % 16) for t in range(16)] if not flat else [10] * 16
            alpha_bytes = b"".join(struct.pack("<H", sum(nib[r * 4 + j] << (4 * j) for j in range(4))) for r in range(4))
            alpha = [n * 17 for n in nib]
        else:
            a0, a1, aidx = chan_choice(rng, flat, seed)
            ablk = chan_block(a0, a1, aidx)
            alpha_bytes = ablk
            alpha = chan_decode(ablk)
        return alpha_bytes + cblk, [(c[0], c[1], c[2], a) for c, a in zip(col, alpha)]
    if kind == "BC4":
        a0, a1, idx = chan_choice(rng, flat, seed)
        blk = chan_block(a0, a1, idx)
        return blk, [(v, 0, 0, 255) for v in chan_decode(blk)]
    if kind == "BC5":
        a0, a1, i0 = chan_choice(rng, flat, seed)
        b0, b1, i1 = chan_choice(Rng(seed + 77), flat, seed + 1)
        if flat:
            b0, b1 = 90, 10
        r, g = chan_block(a0, a1, i0), chan_block(b0, b1, i1)
        return r + g, [(x, y, 0, 255) for x, y in zip(chan_decode(r), chan_decode(g))]
    if kind == "BC7":
        ep0, ep1, p0, p1, idx = bc7_choice(rng, flat, seed)
        return bc7_block(ep0, ep1, p0, p1, idx), bc7_decode(ep0, ep1, p0, p1, idx)
    if kind == "BC6H":
        c0, c1, idx = bc6h_choice(rng, flat, seed)
        px = bc6h_decode(c0, c1, idx)
        return bc6h_block(c0, c1, idx), [tuple(int(math.floor(min(max(v, 0.0), 1.0) * 255 + 0.5)) for v in p) + (255,) for p in px]
    raise ValueError(kind)


def srgb_to_linear8(c):
    x = c / 255.0
    lin = x / 12.92 if x <= 0.04045 else ((x + 0.055) / 1.055) ** 2.4
    return int(math.floor(lin * 255 + 0.5))


def subresource(kind, w, h, mip, variant_id, flat, srgb):
    bw, bh = (w + 3) // 4, (h + 3) // 4
    blocks = bytearray()
    expect = [[None] * w for _ in range(h)]
    for by in range(bh):
        for bx in range(bw):
            seed = mip * 31 + by * 7 + bx * 3 + variant_id * 11 + 1
            blk, px = make_block(kind, seed, flat)
            blocks += blk
            for t in range(16):
                x, y = bx * 4 + (t % 4), by * 4 + (t // 4)
                if x < w and y < h:
                    r, g, b, a = px[t]
                    if srgb:
                        r, g, b = srgb_to_linear8(r), srgb_to_linear8(g), srgb_to_linear8(b)
                    expect[y][x] = (r, g, b, a)
    exp = bytearray()
    for row in expect:
        for p in row:
            exp += bytes(p)
    assert len(blocks) == bw * bh * BLOCK_BYTES[kind]
    return bytes(blocks), bytes(exp)


def c_array(name, data):
    lines = []
    for i in range(0, len(data), 16):
        lines.append("  " + ", ".join("0x%02x" % b for b in data[i:i + 16]) + ",")
    if not data:
        lines = ["  0,"]
    return "static const unsigned char %s[] = {\n%s\n};\n" % (name, "\n".join(lines))


def main():
    out = []
    out.append("// Generated by gen_vectors.py. Do not edit.\n"
               "// Hand-built BC blocks and the RGBA8 a conforming decoder returns for them.\n"
               "#pragma once\n")
    out.append("typedef struct SubVec { const unsigned char *blocks; unsigned blocks_size; const unsigned char *expect; unsigned w, h; } SubVec;\n")
    table = []
    for fi, (fname, tex, srv, kind, srgb) in enumerate(FORMATS):
        for mode, flat in ((0, False), (1, True)):
            for vi, (vname, vw, vh, vm) in enumerate(VARIANTS):
                for mip in range(4):
                    sym = "v_%s_%s_%s_m%d" % (fname, "f" if flat else "p", vname, mip)
                    if mip >= vm:
                        table.append((fi, mode, vi, mip, None))
                        continue
                    w, h = max(1, vw >> mip), max(1, vh >> mip)
                    blocks, exp = subresource(kind, w, h, mip, vi, flat, srgb)
                    out.append(c_array(sym + "_b", blocks))
                    out.append(c_array(sym + "_e", exp))
                    table.append((fi, mode, vi, mip, (sym, len(blocks), w, h)))
    out.append("static const unsigned NUM_VEC_FORMATS = %d;\n" % len(FORMATS))
    out.append("static const SubVec g_vec[%d][2][%d][4] = {\n" % (len(FORMATS), len(VARIANTS)))
    for fi in range(len(FORMATS)):
        out.append("  { // %s\n" % FORMATS[fi][0])
        for mode in range(2):
            out.append("    {\n")
            for vi in range(len(VARIANTS)):
                row = []
                for mip in range(4):
                    e = [t for t in table if t[:4] == (fi, mode, vi, mip)][0][4]
                    if e is None:
                        row.append("{0, 0, 0, 0, 0}")
                    else:
                        sym, n, w, h = e
                        row.append("{%s_b, %d, %s_e, %d, %d}" % (sym, n, sym, w, h))
                out.append("      { %s },\n" % ", ".join(row))
            out.append("    },\n")
        out.append("  },\n")
    out.append("};\n")
    out.append("typedef struct FormatInfo { const char *name; unsigned tex_format; unsigned srv_format; unsigned kind; unsigned block_bytes; int srgb; } FormatInfo;\n")
    out.append("static const FormatInfo g_formats[%d] = {\n" % len(FORMATS))
    for fname, tex, srv, kind, srgb in FORMATS:
        out.append('  {"%s", %s, %s, %d, %d, %d},\n' % (fname, tex, srv, KIND_ID[kind], BLOCK_BYTES[kind], 1 if srgb else 0))
    out.append("};\n")
    out.append("typedef struct VariantInfo { const char *name; unsigned w, h, mips; } VariantInfo;\n")
    out.append("static const VariantInfo g_variants[%d] = {\n" % len(VARIANTS))
    for vname, w, h, m in VARIANTS:
        out.append('  {"%s", %d, %d, %d},\n' % (vname, w, h, m))
    out.append("};\n")
    sys.stdout.write("".join(out))


if __name__ == "__main__":
    main()
