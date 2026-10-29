"""Synthetic texture generation and BC encoding helpers for the rltex tests.

Everything is generated from formulas and a fixed RNG seed. No game data is read.

Encoders used (all pip packages inside build/venv):
  * ispc_texcomp  - Intel ISPC Texture Compressor bindings: BC1, BC3, BC4, BC5, BC6H, BC7
  * etcpak        - BC1, BC3, BC4, BC5, BC7 (a second, different encoder)
BC2 has no encoder in either package, so it is assembled from an ISPC BC1 colour block plus
an explicit 4-bit alpha block (the BC2 layout from the Direct3D 11 block-compression docs).
"""
import hashlib
import io
import struct

import numpy as np

# DXGI_FORMAT values (mingw-w64 dxgiformat.h)
DXGI = {
    "BC1": 0x47, "BC1_SRGB": 0x48, "BC2": 0x4A, "BC2_SRGB": 0x4B, "BC3": 0x4D, "BC3_SRGB": 0x4E,
    "BC4": 0x50, "BC4_SNORM": 0x51, "BC5": 0x53, "BC5_SNORM": 0x54,
    "BC6H": 0x5F, "BC6H_SF16": 0x60, "BC7": 0x62, "BC7_SRGB": 0x63,
}
BLOCK_BYTES = {"BC1": 8, "BC2": 16, "BC3": 16, "BC4": 8, "BC5": 16, "BC6H": 16, "BC7": 16}


def kind_of(name):
    return name.split("_")[0]


def blob_size(kind, w, h):
    return ((w + 3) // 4) * ((h + 3) // 4) * BLOCK_BYTES[kind]


# --------------------------------------------------------------------------------------
# .bc file / key (independent Python implementation of docs/notes/textures.md)
# --------------------------------------------------------------------------------------
def key_header(dxgi, w, h, mip, slice_, nbytes):
    return b"RLK1" + struct.pack("<IIIIIQ", dxgi, w, h, mip, slice_, nbytes)


def key_of(dxgi, w, h, mip, slice_, blob):
    return hashlib.sha256(key_header(dxgi, w, h, mip, slice_, len(blob)) + blob).hexdigest()


def bc_file(dxgi, w, h, mip, slice_, blob):
    return b"RLBC" + struct.pack("<I", 1) + key_header(dxgi, w, h, mip, slice_, len(blob)) + blob


# --------------------------------------------------------------------------------------
# Synthetic images (float in [0,1] unless noted), shape (h, w, 4)
# --------------------------------------------------------------------------------------
def _grid(w, h):
    y, x = np.mgrid[0:h, 0:w].astype(np.float32)
    return x / max(w - 1, 1), y / max(h - 1, 1)


def img_gradient(w, h):
    x, y = _grid(w, h)
    return np.stack([x, y, 0.5 * (x + y), 1.0 - 0.8 * x], -1).astype(np.float32)


def img_checker(w, h):
    x, y = np.mgrid[0:h, 0:w]
    c = (((x // 8) + (y // 8)) % 2).astype(np.float32)
    r = 0.15 + 0.8 * c
    g = 0.7 - 0.5 * c
    b = 0.3 + 0.1 * c
    a = np.where(((x // 4) % 3) == 0, 0.0, 1.0).astype(np.float32)
    return np.stack([r, g, b, a], -1).astype(np.float32)


def img_organic(w, h, seed=7):
    """Smooth multi-octave sine texture with a few hard edges (photo-like statistics)."""
    rng = np.random.default_rng(seed)
    x, y = _grid(w, h)
    out = np.zeros((h, w, 4), np.float32)
    for ch in range(4):
        v = np.zeros((h, w), np.float32)
        for octave in range(4):
            fx, fy = rng.uniform(1.0, 6.0) * 2**octave, rng.uniform(1.0, 6.0) * 2**octave
            p = rng.uniform(0, 6.28)
            v += np.sin(6.28 * (fx * x + fy * y) + p) / 2**octave
        v = (v - v.min()) / (v.max() - v.min() + 1e-6)
        out[..., ch] = v
    out[..., 3] = np.clip(out[..., 3] * 1.4 - 0.2, 0, 1)
    edge = (np.hypot(x - 0.5, y - 0.5) < 0.25)
    out[..., :3][edge] = 1.0 - out[..., :3][edge]
    return out


def img_wheel(w, h):
    x, y = _grid(w, h)
    dx, dy = x - 0.5, y - 0.5
    ang = np.arctan2(dy, dx)
    rad = np.clip(np.hypot(dx, dy) * 2, 0, 1)
    r = 0.5 + 0.5 * np.cos(ang)
    g = 0.5 + 0.5 * np.cos(ang - 2.094)
    b = 0.5 + 0.5 * np.cos(ang + 2.094)
    return np.stack([r * rad, g * rad, b * rad, np.ones_like(r)], -1).astype(np.float32)


LDR_IMAGES = {"gradient": img_gradient, "checker": img_checker, "organic": img_organic, "wheel": img_wheel}


def img_normalmap(w, h):
    """RG normal map of smooth bumps (x, y in [0,1] = (n+1)/2)."""
    x, y = _grid(w, h)
    gx = 9 * np.cos(9 * x) * np.cos(7 * y) + 0.4 * 23 * np.cos(23 * x + 3 * y)  # d/dx of the height field
    gy = -7 * np.sin(9 * x) * np.sin(7 * y) + 0.4 * 3 * np.cos(23 * x + 3 * y)   # d/dy
    n = np.stack([-gx * 0.08, -gy * 0.08, np.ones_like(gx)], -1)
    n /= np.linalg.norm(n, axis=-1, keepdims=True)
    out = np.zeros((h, w, 4), np.float32)
    out[..., 0] = n[..., 0] * 0.5 + 0.5
    out[..., 1] = n[..., 1] * 0.5 + 0.5
    out[..., 3] = 1.0
    return out


def img_height(w, h):
    """Single channel (roughness / mask style): smooth ramps plus stripes."""
    x, y = _grid(w, h)
    v = 0.5 + 0.35 * np.sin(8 * x + 2 * y) + 0.15 * np.sign(np.sin(40 * y))
    out = np.zeros((h, w, 4), np.float32)
    out[..., 0] = np.clip(v, 0, 1)
    out[..., 3] = 1.0
    return out


def img_hdr(w, h):
    """Linear HDR RGB, range ~[0.01, 16] (smooth, with a bright blob)."""
    x, y = _grid(w, h)
    base = 0.2 + 0.8 * np.stack([x, y, 0.5 * (x + y)], -1)
    blob = 16.0 * np.exp(-((x - 0.3) ** 2 + (y - 0.6) ** 2) / 0.01)
    rgb = base * (1.0 + 2.0 * np.sin(5 * x) ** 2)[..., None] + blob[..., None] * np.array([1.0, 0.8, 0.5])
    out = np.ones((h, w, 4), np.float32)
    out[..., :3] = rgb
    return out


def to_u8(img):
    return np.clip(np.rint(img * 255.0), 0, 255).astype(np.uint8)


def pad4(a):
    """Edge-replicate to multiples of 4 (encoders need whole blocks)."""
    h, w = a.shape[:2]
    ph, pw = (-h) % 4, (-w) % 4
    if ph or pw:
        a = np.pad(a, ((0, ph), (0, pw)) + ((0, 0),) * (a.ndim - 2), mode="edge")
    return a


# --------------------------------------------------------------------------------------
# Encoders
# --------------------------------------------------------------------------------------
def encode(kind, img_f32, encoder="ispc"):
    """Encode a float image (h,w,4) to a BC blob of the padded size. Returns (blob, w, h) for
    the ORIGINAL size (blob covers the 4-aligned padded area)."""
    h, w = img_f32.shape[:2]
    if kind == "BC6H":
        padded = pad4(img_f32.astype(np.float16))
        import ispc_texcomp as ic
        ph, pw = padded.shape[:2]
        surf = ic.RGBASurface(padded.tobytes(), pw, ph, pw * 8)
        blob = ic.compress_blocks_bc6h(surf, ic.BC6HEncSettings.from_profile("veryslow"))
    else:
        u8 = pad4(to_u8(img_f32))
        ph, pw = u8.shape[:2]
        if encoder == "ispc":
            import ispc_texcomp as ic
            surf = ic.RGBASurface(u8.tobytes(), pw, ph)
            if kind == "BC1":
                blob = ic.compress_blocks_bc1(surf)[: blob_size("BC1", pw, ph)]
            elif kind == "BC2":
                blob = _bc2_from_bc1(ic, surf, u8)
            elif kind == "BC3":
                blob = ic.compress_blocks_bc3(surf)
            elif kind == "BC4":
                blob = ic.compress_blocks_bc4(surf)
            elif kind == "BC5":
                blob = ic.compress_blocks_bc5(surf)
            elif kind == "BC7":
                blob = ic.compress_blocks_bc7(surf, ic.BC7EncSettings.from_profile("alpha_slow"))
            else:
                raise ValueError(kind)
        elif encoder == "etcpak":
            import etcpak
            raw = u8.tobytes()
            fn = {"BC1": etcpak.compress_bc1, "BC3": etcpak.compress_bc3, "BC4": etcpak.compress_bc4,
                  "BC5": etcpak.compress_bc5, "BC7": etcpak.compress_bc7}[kind]
            blob = fn(raw, pw, ph)
        else:
            raise ValueError(encoder)
    blob = bytes(blob)
    assert len(blob) == blob_size(kind, w, h), (kind, encoder, len(blob), blob_size(kind, w, h))
    return blob


def _bc2_from_bc1(ic, surf, u8):
    """BC2 = 8 bytes explicit alpha (4 bits/texel, row-major, LSB first) + BC1 colour block."""
    ph, pw = u8.shape[:2]
    col = ic.compress_blocks_bc1(surf)[: blob_size("BC1", pw, ph)]
    out = bytearray()
    bw, bh = pw // 4, ph // 4
    for by in range(bh):
        for bx in range(bw):
            a = u8[by * 4:by * 4 + 4, bx * 4:bx * 4 + 4, 3].astype(np.int32)
            n = (a * 15 + 127) // 255  # nearest 4-bit value
            alpha = bytearray()
            for row in range(4):
                v = 0
                for j in range(4):
                    v |= int(n[row, j]) << (4 * j)
                alpha += struct.pack("<H", v)
            i = by * bw + bx
            c = bytearray(col[i * 8:(i + 1) * 8])
            # BC2 colour blocks are always decoded in 4-colour mode, but keep c0 > c1 anyway so
            # the same block is also valid as BC1 for decoders that look at the ordering.
            c0, c1 = struct.unpack("<HH", c[:4])
            if c0 < c1:
                c0, c1 = c1, c0
                idx = struct.unpack("<I", c[4:])[0]
                swapped = 0
                for p in range(16):
                    s = (idx >> (2 * p)) & 3
                    s = {0: 1, 1: 0, 2: 3, 3: 2}[s]
                    swapped |= s << (2 * p)
                c = bytearray(struct.pack("<HHI", c0, c1, swapped))
            out += alpha + c
    return bytes(out)


# --------------------------------------------------------------------------------------
# Independent decoders (texture2ddecoder, Pillow). Return arrays cropped to w x h.
# --------------------------------------------------------------------------------------
def _crop(a, w, h):
    return a[:h, :w]


def decode_t2d(kind, blob, w, h):
    """texture2ddecoder: returns uint8 (h,w,4) RGBA. Its BGRA output is reordered here.
    Supports BC1, BC3, BC4, BC5, BC6H (clamped to 8 bit), BC7. No BC2."""
    import texture2ddecoder as t
    pw, ph = ((w + 3) // 4) * 4, ((h + 3) // 4) * 4
    fn = {"BC1": t.decode_bc1, "BC3": t.decode_bc3, "BC4": t.decode_bc4, "BC5": t.decode_bc5,
          "BC6H": t.decode_bc6, "BC7": t.decode_bc7}[kind]
    raw = np.frombuffer(fn(blob, pw, ph), np.uint8).reshape(ph, pw, 4)
    rgba = raw[..., [2, 1, 0, 3]]
    return _crop(rgba, w, h)


def _dds(dxgi, w, h, data):
    hdr = struct.pack("<4sIIIIIII", b"DDS ", 124, 0x1007 | 0x80000, h, w, len(data), 0, 1) + b"\0" * 44
    pf = struct.pack("<II4sIIIII", 32, 4, b"DX10", 0, 0, 0, 0, 0)
    caps = struct.pack("<IIIII", 0x1000, 0, 0, 0, 0)
    dx10 = struct.pack("<IIIII", dxgi, 3, 0, 1, 0)
    return hdr + pf + caps + dx10 + data


def decode_pillow(name, blob, w, h):
    """Pillow's DDS plugin (libImaging BcnDecode). Returns uint8 (h,w,C) with C = 4 (BC1/2/3/7),
    1 (BC4), 3 (BC5: r,g,0) or 3 (BC6H, tone-mapped 8 bit)."""
    from PIL import Image
    im = Image.open(io.BytesIO(_dds(DXGI[name], w, h, blob)))
    im.load()
    a = np.array(im)
    return a


def ref_bc45(blob, w, h, channels, signed):
    """Pure-Python BC4/BC5 reference from the Direct3D 11 block-compression spec: float
    interpolation of the two 8-bit endpoints, rounded to nearest UNORM8 / SNORM8 (x127).
    Returns int32 (h, w, channels); unsigned in 0..255, signed in -127..127."""
    pw, ph = ((w + 3) // 4) * 4, ((h + 3) // 4) * 4
    out = np.zeros((ph, pw, channels), np.int32)
    bb = 8 * channels
    bw = pw // 4
    for bi in range(len(blob) // bb):
        for ch in range(channels):
            b = blob[bi * bb + ch * 8: bi * bb + ch * 8 + 8]
            if signed:
                e0, e1 = [max(int(np.int8(np.uint8(v))), -127) for v in b[:2]]
                scale, lo, hi = 127.0, -1.0, 1.0
            else:
                e0, e1 = b[0], b[1]
                scale, lo, hi = 255.0, 0.0, 1.0
            f0, f1 = e0 / scale, e1 / scale
            if e0 > e1:
                pal = [f0, f1] + [((7 - k) * f0 + k * f1) / 7.0 for k in range(1, 7)]
            else:
                pal = [f0, f1] + [((5 - k) * f0 + k * f1) / 5.0 for k in range(1, 5)] + [lo, hi]
            idx = int.from_bytes(b[2:8], "little")
            by, bx = divmod(bi, bw)
            for t in range(16):
                v = pal[(idx >> (3 * t)) & 7] * scale
                out[by * 4 + t // 4, bx * 4 + t % 4, ch] = int(np.floor(v + 0.5))
    return out[:h, :w]


# --------------------------------------------------------------------------------------
# metrics
# --------------------------------------------------------------------------------------
def psnr(a, b, peak=255.0):
    a = a.astype(np.float64)
    b = b.astype(np.float64)
    mse = np.mean((a - b) ** 2)
    if mse == 0:
        return 99.0
    return 10.0 * np.log10(peak * peak / mse)
