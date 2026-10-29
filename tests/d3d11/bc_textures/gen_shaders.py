#!/usr/bin/env python3
"""Assembles the Shader Model 4.0 shaders of the BC texture test into DXBC containers (shaders.h).
No D3D compiler is needed at build or run time: the token streams are written out here with the
encodings of libs/DXBCParser/d3d12tokenizedprogramformat.hpp (DXMT's own parser header), and were
checked against Wine's wined3d / vkd3d-shader, which parses and disassembles them (see
docs/notes/dxmt-bc.md).

  vs_4_0   SV_VertexID -> position / uv of one oversized triangle (no vertex buffer)
             r0.x = id & 1, r0.y = id & 2 (as floats); o0 = (4 r0.x - 1, 1 - 2 r0.y, 0, 1); o1 = (2 r0.x, r0.y)
  ps_4_0   sample_l o0.xyzw, v1.xyxx, t0.xyzw, s0, l(MIP)      (one shader per mip level 0..3)

With a point sampler, a shader resource view of the whole mip chain and a render target of the
mip's size, pixel (x, y) shows texel (x, y) of level MIP: the uv at a pixel centre is
(x + 0.5) / width. Every texel of every mip is checked, exactly, without filtering.

Usage: gen_shaders.py > shaders.h
"""
import math
import struct
import sys

# opcodes (D3D10_SB_OPCODE_TYPE order in d3d12tokenizedprogramformat.hpp)
OP_AND, OP_MAD, OP_MOV, OP_MUL, OP_RET, OP_SAMPLE_L, OP_UTOF = 1, 50, 54, 56, 62, 72, 86
OP_DCL_RESOURCE, OP_DCL_SAMPLER, OP_DCL_INPUT_SGV, OP_DCL_INPUT_PS = 88, 90, 96, 98
OP_DCL_OUTPUT, OP_DCL_OUTPUT_SIV, OP_DCL_TEMPS = 101, 103, 104
# operand types
T_TEMP, T_INPUT, T_OUTPUT, T_SAMPLER, T_RESOURCE, T_IMMEDIATE32 = 0, 1, 2, 6, 7, 4
# misc enums
NAME_POSITION = 1
NAME_VERTEX_ID = 6
INTERP_LINEAR = 2
RES_DIM_TEXTURE2D = 3
RET_FLOAT = 5
SAMPLER_MODE_DEFAULT = 0
MAX_MIPS = 4


def op_token(opcode, length, ctrl=0):
    return opcode | (ctrl << 11) | (length << 24)


def operand(op_type, index, mask=None, swizzle=None, components=2):
    """Operand token + register index. mask: 4-bit write mask; swizzle: 4 component indices."""
    tok = components | (op_type << 12) | (1 << 20)  # one index dimension, immediate index
    if components == 2:
        if mask is not None:
            tok |= (0 << 2) | (mask << 4)
        else:
            tok |= (1 << 2) | ((swizzle[0] | swizzle[1] << 2 | swizzle[2] << 4 | swizzle[3] << 6) << 4)
    return [tok, index]


def scalar_immediate(float_value):
    """l(x): a one-component IMMEDIATE32 operand (no index)."""
    return [1 | (T_IMMEDIATE32 << 12), struct.unpack("<I", struct.pack("<f", float_value))[0]]


def int_immediate(value):
    return [1 | (T_IMMEDIATE32 << 12), value]


def select1(op_type, index, comp):
    """A source operand reading one component (select-1 mode)."""
    return [2 | (2 << 2) | (comp << 4) | (op_type << 12) | (1 << 20), index]


XYZW = (0, 1, 2, 3)
XYXX = (0, 1, 0, 0)


def vs_tokens():
    t = []
    t += [op_token(OP_DCL_INPUT_SGV, 4)] + operand(T_INPUT, 0, mask=0x1) + [NAME_VERTEX_ID]
    t += [op_token(OP_DCL_OUTPUT_SIV, 4)] + operand(T_OUTPUT, 0, mask=0xF) + [NAME_POSITION]
    t += [op_token(OP_DCL_OUTPUT, 3)] + operand(T_OUTPUT, 1, mask=0x3)
    t += [op_token(OP_DCL_TEMPS, 2), 1]
    t += [op_token(OP_AND, 7)] + operand(T_TEMP, 0, mask=0x1) + select1(T_INPUT, 0, 0) + int_immediate(1)
    t += [op_token(OP_AND, 7)] + operand(T_TEMP, 0, mask=0x2) + select1(T_INPUT, 0, 0) + int_immediate(2)
    t += [op_token(OP_UTOF, 5)] + operand(T_TEMP, 0, mask=0x3) + operand(T_TEMP, 0, swizzle=XYXX)
    t += [op_token(OP_MAD, 9)] + operand(T_OUTPUT, 0, mask=0x1) + select1(T_TEMP, 0, 0) + scalar_immediate(4.0) + scalar_immediate(-1.0)
    t += [op_token(OP_MAD, 9)] + operand(T_OUTPUT, 0, mask=0x2) + select1(T_TEMP, 0, 1) + scalar_immediate(-2.0) + scalar_immediate(1.0)
    t += [op_token(OP_MOV, 5)] + operand(T_OUTPUT, 0, mask=0x4) + scalar_immediate(0.0)
    t += [op_token(OP_MOV, 5)] + operand(T_OUTPUT, 0, mask=0x8) + scalar_immediate(1.0)
    t += [op_token(OP_MUL, 7)] + operand(T_OUTPUT, 1, mask=0x1) + select1(T_TEMP, 0, 0) + scalar_immediate(2.0)
    t += [op_token(OP_MOV, 5)] + operand(T_OUTPUT, 1, mask=0x2) + select1(T_TEMP, 0, 1)
    t += [op_token(OP_RET, 1)]
    return t


def ps_tokens(mip):
    t = []
    t += [op_token(OP_DCL_SAMPLER, 3, SAMPLER_MODE_DEFAULT)] + operand(T_SAMPLER, 0, components=0)
    t += [op_token(OP_DCL_INPUT_PS, 3, INTERP_LINEAR)] + operand(T_INPUT, 1, mask=0x3)
    t += [op_token(OP_DCL_RESOURCE, 4, RES_DIM_TEXTURE2D)] + operand(T_RESOURCE, 0, components=0) + [
        RET_FLOAT | RET_FLOAT << 4 | RET_FLOAT << 8 | RET_FLOAT << 12]
    t += [op_token(OP_DCL_OUTPUT, 3)] + operand(T_OUTPUT, 0, mask=0xF)
    t += [op_token(OP_SAMPLE_L, 11)] + operand(T_OUTPUT, 0, mask=0xF) + operand(T_INPUT, 1, swizzle=XYXX) + \
        operand(T_RESOURCE, 0, swizzle=XYZW) + operand(T_SAMPLER, 0, components=0) + scalar_immediate(float(mip))
    t += [op_token(OP_RET, 1)]
    return t


def shader_chunk(version_token, tokens):
    body = [version_token, 2 + len(tokens)] + tokens
    return b"SHDR", struct.pack("<%dI" % len(body), *body)


def signature_chunk(fourcc, elements):
    """elements: (name, semantic_index, system_value, register, mask, rw_mask[, component_type]); float by default."""
    head = struct.pack("<II", len(elements), 8)
    names = b""
    entries = b""
    base = 8 + 24 * len(elements)
    for el in elements:
        name, sem_idx, sysval, reg, mask, rw = el[:6]
        comp_type = el[6] if len(el) > 6 else 3
        entries += struct.pack("<IIIIIBBH", base + len(names), sem_idx, sysval, comp_type, reg, mask, rw, 0)
        names += name.encode() + b"\0"
    while len(names) % 4:
        names += b"\0"
    return fourcc, head + entries + names


_K = [int(abs(math.sin(i + 1)) * 2**32) & 0xFFFFFFFF for i in range(64)]
_S = [7, 12, 17, 22] * 4 + [5, 9, 14, 20] * 4 + [4, 11, 16, 23] * 4 + [6, 10, 15, 21] * 4


def _md5_block(state, block):
    """RFC 1321 compression function over one 64-byte block (little-endian words)."""
    m = struct.unpack("<16I", bytes(block))
    a, b, c, d = state
    for i in range(64):
        if i < 16:
            f, g = (b & c) | (~b & d), i
        elif i < 32:
            f, g = (d & b) | (~d & c), (5 * i + 1) % 16
        elif i < 48:
            f, g = b ^ c ^ d, (3 * i + 5) % 16
        else:
            f, g = c ^ (b | ~d), (7 * i) % 16
        f = (f + a + _K[i] + m[g]) & 0xFFFFFFFF
        a, d, c = d, c, b
        b = (b + (((f << _S[i]) | (f >> (32 - _S[i]))) & 0xFFFFFFFF)) & 0xFFFFFFFF
    return [(x + y) & 0xFFFFFFFF for x, y in zip(state, (a, b, c, d))]


def dxbc_checksum(data):
    """The DXBC hash: MD5 over everything after the 20-byte "DXBC" + hash prefix, with
    Microsoft's different final padding (the length goes first and last, data shifted by
    4 bytes). Port of vkd3d-shader libs/vkd3d-shader/checksum.c (VKD3D_MD5_DXBC)."""
    msg = data[20:]
    state = [0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476]
    bits = (len(msg) * 8) & 0xFFFFFFFF
    full = len(msg) // 64
    for i in range(full):
        state = _md5_block(state, msg[i * 64:(i + 1) * 64])
    rest = msg[full * 64:]
    count = len(rest) + 1  # data plus the 0x80 byte
    tail = bytearray(rest) + b"\x80"
    if 64 - count < 8:
        state = _md5_block(state, tail + b"\0" * (64 - count))
        blk = bytearray(64)
    else:
        blk = bytearray(4) + tail
        blk += b"\0" * (64 - len(blk))
    blk[0:4] = struct.pack("<I", bits)
    blk[60:64] = struct.pack("<I", ((bits >> 2) | 1) & 0xFFFFFFFF)
    state = _md5_block(state, blk)
    return struct.pack("<4I", *state)


def container(chunks):
    n = len(chunks)
    offset = 32 + 4 * n
    offsets = []
    body = b""
    for fourcc, data in chunks:
        offsets.append(offset + len(body))
        body += fourcc + struct.pack("<I", len(data)) + data
        while len(body) % 4:
            body += b"\0"
    total = offset + len(body)
    out = bytearray(b"DXBC" + b"\0" * 16 + struct.pack("<III", 1, total, n) + struct.pack("<%dI" % n, *offsets) + body)
    out[4:20] = dxbc_checksum(bytes(out))  # Wine's vkd3d-shader verifies it; DXMT's parser does not
    return bytes(out)


def array(name, data):
    lines = []
    for i in range(0, len(data), 16):
        lines.append("  " + ", ".join("0x%02x" % b for b in data[i:i + 16]) + ",")
    return "static const unsigned char %s[%d] = {\n%s\n};\n" % (name, len(data), "\n".join(lines))


def main():
    vs = container([
        signature_chunk(b"ISGN", [("SV_VertexID", 0, NAME_VERTEX_ID, 0, 0x1, 0x1, 1)]),
        signature_chunk(b"OSGN", [("SV_Position", 0, NAME_POSITION, 0, 0xF, 0), ("TEXCOORD", 0, 0, 1, 0x3, 0xC)]),
        shader_chunk(0x00010040, vs_tokens()),
    ])
    sys.stdout.write("// Generated by gen_shaders.py. Do not edit.\n#pragma once\n")
    sys.stdout.write(array("g_vs_blob", vs))
    for mip in range(MAX_MIPS):
        ps = container([
            signature_chunk(b"ISGN", [("SV_Position", 0, NAME_POSITION, 0, 0xF, 0), ("TEXCOORD", 0, 0, 1, 0x3, 0x3)]),
            signature_chunk(b"OSGN", [("SV_Target", 0, 0, 0, 0xF, 0)]),
            shader_chunk(0x00000040, ps_tokens(mip)),
        ])
        sys.stdout.write(array("g_ps_blob_mip%d" % mip, ps))
    sys.stdout.write("static const unsigned char *const g_ps_blobs[%d] = {%s};\n" % (
        MAX_MIPS, ", ".join("g_ps_blob_mip%d" % m for m in range(MAX_MIPS))))
    sys.stdout.write("static const unsigned g_ps_blob_size = sizeof(g_ps_blob_mip0);\n")


if __name__ == "__main__":
    main()
