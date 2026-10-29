#!/usr/bin/env python3
"""Differential tests: native x86-64 (reference) vs FEX's ARM64 JIT.

Each generated case is one flat x86-64 snippet in FEX's unittest .asm format. It starts from
random register, memory and MXCSR state biased toward edge values, runs a random instruction
sequence, then folds the observable state into registers:
  R15 = RFLAGS masked to the flags the x86 manual defines at that point
  R14 = MXCSR (rounding, FTZ/DAZ, sticky exception flags)
  XMM8..15 = the 128-byte scratch area, so every memory write is compared
The reference values come from running the same bytes natively on this x86-64 host
(tests/diff/native). They are written into the case's RegData, and FEX's own
TestHarnessRunner checks them on its ARM64 backend (natively on ARM64, or in the vixl
simulator on an x86-64 host).

  diff.py gen     --seed S --count N --out DIR      generate cases with native expectations
  diff.py run     --cases DIR --runner PATH          run cases under FEX, report mismatches
  diff.py selftest --runner PATH                     prove an injected mismatch is caught
  diff.py all     --seed S --count N --runner PATH   gen + run + selftest
Exit status is non-zero if any case mismatches (except in selftest, where the opposite holds).
"""
import argparse
import json
import os
import pathlib
import random
import shutil
import subprocess
import sys
import concurrent.futures as cf

ROOT = pathlib.Path(__file__).resolve().parents[2]
FEX_SCRIPTS = ROOT / "external/fex/Scripts"
NATIVE_DIR = ROOT / "tests/diff/native"
NATIVE_RUNNER = ROOT / "build/diff/native/runner"

SCRATCH = 0x100000000
DATA_SIZE = 0x80          # 0x00-0x7f: memory operands; reloaded into XMM8..15 at the end
MXCSR_OFF = 0xC0          # initial MXCSR
MXCSR_OUT = 0xC4          # stmxcsr target
XMM_INIT = 0x100          # 8 x 16 bytes of initial XMM0..7
MEM_IMAGE = 0x180

CF, PF, AF, ZF, SF, OF = 0x1, 0x4, 0x10, 0x40, 0x80, 0x800
ALLF = CF | PF | AF | ZF | SF | OF

GPRS = ["rax", "rbx", "rcx", "rdx", "rsi", "rdi", "rbp", "r8", "r9", "r10", "r11", "r12"]
SUB = {  # 64, 32, 16, 8
    "rax": ("rax", "eax", "ax", "al"), "rbx": ("rbx", "ebx", "bx", "bl"),
    "rcx": ("rcx", "ecx", "cx", "cl"), "rdx": ("rdx", "edx", "dx", "dl"),
    "rsi": ("rsi", "esi", "si", "sil"), "rdi": ("rdi", "edi", "di", "dil"),
    "rbp": ("rbp", "ebp", "bp", "bpl"),
}
for _r in ("r8", "r9", "r10", "r11", "r12"):
    SUB[_r] = (_r, _r + "d", _r + "w", _r + "b")
SIZES = {64: 0, 32: 1, 16: 2, 8: 3}
PTR = {64: "qword", 32: "dword", 16: "word", 8: "byte"}
XMMS = [f"xmm{i}" for i in range(8)]

F32_SPECIAL = [0x00000000, 0x80000000, 0x3F800000, 0xBF800000, 0x00000001, 0x807FFFFF, 0x00800000,
               0x7F7FFFFF, 0x7F800000, 0xFF800000, 0x7FC00000, 0xFFC00000, 0x7FA00000, 0x7F800001,
               0x4F000000, 0xCF000000, 0x5F000000, 0x3EFFFFFF, 0x3F000000, 0x3FC00000, 0x40200000]
F64_SPECIAL = [0x0, 0x8000000000000000, 0x3FF0000000000000, 0xBFF0000000000000, 0x1,
               0x800FFFFFFFFFFFFF, 0x0010000000000000, 0x7FEFFFFFFFFFFFFF, 0x7FF0000000000000,
               0xFFF0000000000000, 0x7FF8000000000000, 0xFFF8000000000000, 0x7FF4000000000000,
               0x41E0000000000000, 0xC1E0000000000000, 0x43E0000000000000, 0x3FE0000000000000,
               0x3FF8000000000000, 0x4004000000000000]
GPR_SPECIAL = [0, 1, 2, 0xFFFFFFFFFFFFFFFF, 0x8000000000000000, 0x7FFFFFFFFFFFFFFF, 0x80000000,
               0x7FFFFFFF, 0xFFFFFFFF, 0x100000000, 0xFF, 0x80, 0x7F, 0xFFFF, 0x8000]

# condition code -> flags it reads
CC_READS = {"o": OF, "no": OF, "b": CF, "ae": CF, "e": ZF, "ne": ZF, "be": CF | ZF, "a": CF | ZF,
            "s": SF, "ns": SF, "p": PF, "np": PF, "l": SF | OF, "ge": SF | OF,
            "le": ZF | SF | OF, "g": ZF | SF | OF}


class Gen:
    """Builds one case, tracking which RFLAGS bits are architecturally undefined."""

    def __init__(self, rng):
        self.r = rng
        self.lines = []
        self.undef = ALLF          # flags are unknown at entry
        self.cover = set()

    # ---- helpers -------------------------------------------------------------------
    def gpr(self, size=None):
        size = size or self.r.choice([64, 64, 32, 32, 16, 8])
        return SUB[self.r.choice(GPRS)][SIZES[size]], size

    def gimm(self):
        r = self.r.random()
        if r < 0.4:
            return self.r.choice(GPR_SPECIAL)
        if r < 0.7:
            return self.r.getrandbits(self.r.choice([8, 16, 32]))
        return self.r.getrandbits(64)

    def mem(self, size, align=1):
        d = self.r.randrange(0, DATA_SIZE - size // 8 + 1)
        d -= d % align
        return f"{PTR[size]} [r13 + {d}]" if size <= 64 else f"[r13 + {d}]"

    def xmm(self):
        return self.r.choice(XMMS)

    def xsrc(self):
        """XMM source: a register, or an aligned 16-byte memory operand."""
        if self.r.random() < 0.25:
            return f"[r13 + {16 * self.r.randrange(0, DATA_SIZE // 16)}]"
        return self.xmm()

    def emit(self, line, writes=0, undef=0, reads=0, tag=None):
        """writes: flags given defined values; undef: flags left undefined; reads: flags consumed."""
        if reads & self.undef:
            # Define every flag first so the consumer never sees an undefined value.
            self.lines.append(f"cmp {self.gpr(64)[0]}, {self.gpr(64)[0]}")
            self.undef = 0
        self.lines.append(line)
        self.undef = (self.undef & ~(writes | undef)) | undef
        self.cover.add(tag or line.split()[0])

    # ---- instruction families ------------------------------------------------------
    def op_alu(self):
        op = self.r.choice(["add", "sub", "and", "or", "xor", "cmp", "test", "adc", "sbb"])
        size = self.r.choice([64, 32, 16, 8])
        dst, _ = self.gpr(size)
        form = self.r.random()
        if form < 0.45:
            src = self.gpr(size)[0]
            line = f"{op} {dst}, {src}"
        elif form < 0.75 and op != "test":
            imm = self.gimm() & ((1 << min(size, 32)) - 1)
            if size == 64 and imm >= 0x80000000:
                imm -= 1 << 32           # imm32 is sign-extended for 64-bit operations
            line = f"{op} {dst}, {imm}"
        elif self.r.random() < 0.5:
            line = f"{op} {dst}, {self.mem(size)}"
        else:
            line = f"{op} {self.mem(size)}, {dst}"
        logic = op in ("and", "or", "xor", "test")
        self.emit(line, writes=ALLF & ~(AF if logic else 0), undef=AF if logic else 0,
                  reads=CF if op in ("adc", "sbb") else 0, tag=op)

    def op_unary(self):
        op = self.r.choice(["inc", "dec", "neg", "not"])
        dst = self.gpr()[0] if self.r.random() < 0.8 else self.mem(self.r.choice([64, 32, 16, 8]))
        w = {"inc": ALLF & ~CF, "dec": ALLF & ~CF, "neg": ALLF, "not": 0}[op]
        self.emit(f"{op} {dst}", writes=w, tag=op)

    def op_shift(self):
        op = self.r.choice(["shl", "shr", "sar", "rol", "ror", "rcl", "rcr"])
        size = self.r.choice([64, 32] if op in ("rcl", "rcr") else [64, 32, 16, 8])
        dst = self.gpr(size)[0]
        if self.r.random() < 0.2:
            # Count from CL may be zero, which leaves flags untouched: treat all as undefined.
            self.emit(f"{op} {dst}, cl", undef=ALLF, reads=CF if op in ("rcl", "rcr") else 0, tag=op + "_cl")
            return
        cnt = self.r.choice([1, 1, self.r.randrange(1, size)])
        if op in ("shl", "shr", "sar"):
            self.emit(f"{op} {dst}, {cnt}", writes=ALLF, undef=AF | (0 if cnt == 1 else OF), tag=op)
        else:
            self.emit(f"{op} {dst}, {cnt}", writes=CF | OF, undef=0 if cnt == 1 else OF,
                      reads=CF if op in ("rcl", "rcr") else 0, tag=op)

    def op_mul(self):
        k = self.r.random()
        if k < 0.4:
            size = self.r.choice([64, 32, 16])
            self.emit(f"imul {self.gpr(size)[0]}, {self.gpr(size)[0]}", writes=CF | OF, undef=SF | ZF | AF | PF, tag="imul2")
        elif k < 0.6:
            size = self.r.choice([64, 32, 16])
            self.emit(f"imul {self.gpr(size)[0]}, {self.gpr(size)[0]}, {self.r.randrange(-128, 128)}",
                      writes=CF | OF, undef=SF | ZF | AF | PF, tag="imul3")
        else:
            op = self.r.choice(["mul", "imul"])
            size = self.r.choice([64, 32, 16, 8])
            self.emit(f"{op} {self.gpr(size)[0]}", writes=CF | OF, undef=SF | ZF | AF | PF, tag=op + "1")

    def op_div(self):
        # Unsigned divide with RDX=0 and a non-zero divisor never faults.
        size = self.r.choice([64, 32])
        d = self.r.choice(["rbx", "rsi", "rdi", "rbp", "r8", "r9", "r10", "r11", "r12"])
        dreg = SUB[d][SIZES[size]]
        self.lines.append(f"xor edx, edx")
        self.lines.append(f"or {SUB[d][1]}, 1")
        self.emit(f"div {dreg}", undef=ALLF, tag="div")

    def op_bits(self):
        k = self.r.random()
        if k < 0.4:
            op = self.r.choice(["bt", "bts", "btr", "btc"])
            size = self.r.choice([64, 32, 16])
            self.emit(f"{op} {self.gpr(size)[0]}, {self.r.randrange(0, size)}", writes=CF, undef=OF | SF | AF | PF, tag=op)
        elif k < 0.6:
            size = self.r.choice([64, 32, 16])
            self.emit(f"popcnt {self.gpr(size)[0]}, {self.gpr(size)[0]}", writes=ALLF, tag="popcnt")
        elif k < 0.75:
            self.emit(f"bswap {self.gpr(self.r.choice([64, 32]))[0]}", tag="bswap")
        else:
            size = self.r.choice([32, 64])
            # crc32 r64 only accepts r/m8 or r/m64 sources
            src = self.gpr(self.r.choice([8, 16, 32] if size == 32 else [8, 64]))[0]
            self.emit(f"crc32 {SUB[self.r.choice(GPRS)][SIZES[size]]}, {src}", tag="crc32")

    def op_move(self):
        k = self.r.random()
        if k < 0.25:
            cc = self.r.choice(list(CC_READS))
            size = self.r.choice([64, 32, 16])
            self.emit(f"cmov{cc} {self.gpr(size)[0]}, {self.gpr(size)[0]}", reads=CC_READS[cc], tag="cmovcc")
        elif k < 0.4:
            cc = self.r.choice(list(CC_READS))
            self.emit(f"set{cc} {self.gpr(8)[0]}", reads=CC_READS[cc], tag="setcc")
        elif k < 0.55:
            ext = self.r.choice(["movzx", "movsx"])
            dsize = self.r.choice([64, 32])
            self.emit(f"{ext} {self.gpr(dsize)[0]}, {self.gpr(self.r.choice([8, 16]))[0]}", tag=ext)
        elif k < 0.65:
            self.emit(f"movsxd {self.gpr(64)[0]}, {self.gpr(32)[0]}", tag="movsxd")
        elif k < 0.75:
            size = self.r.choice([64, 32, 16, 8])
            self.emit(f"mov {self.mem(size)}, {self.gpr(size)[0]}", tag="store")
        elif k < 0.85:
            size = self.r.choice([64, 32, 16, 8])
            self.emit(f"mov {self.gpr(size)[0]}, {self.mem(size)}", tag="load")
        elif k < 0.92:
            size = self.r.choice([64, 32])
            op = self.r.choice(["xadd", "cmpxchg"])
            self.emit(f"{op} {self.gpr(size)[0]}, {self.gpr(size)[0]}", writes=ALLF, tag=op)
        else:
            a, b = self.r.randrange(0, DATA_SIZE), self.r.randrange(0, DATA_SIZE)
            n = self.r.randrange(0, DATA_SIZE - max(a, b) + 1)
            self.lines += [f"lea rsi, [r13 + {a}]", f"lea rdi, [r13 + {b}]", f"mov ecx, {n}"]
            self.emit("rep movsb", tag="rep_movsb")

    def op_sse_float(self):
        x = self.xmm()
        k = self.r.random()
        if k < 0.35:
            op = self.r.choice(["addps", "subps", "mulps", "divps", "minps", "maxps", "sqrtps",
                                "addpd", "subpd", "mulpd", "divpd", "minpd", "maxpd", "sqrtpd",
                                "addsubps", "addsubpd", "haddps", "haddpd", "andps", "andnps",
                                "orps", "xorps", "unpcklps", "unpckhps", "unpcklpd", "unpckhpd"])
            self.emit(f"{op} {x}, {self.xsrc()}", tag=op)
        elif k < 0.6:
            op = self.r.choice(["addss", "subss", "mulss", "divss", "minss", "maxss", "sqrtss",
                                "addsd", "subsd", "mulsd", "divsd", "minsd", "maxsd", "sqrtsd",
                                "cvtss2sd", "cvtsd2ss"])
            self.emit(f"{op} {x}, {self.xmm()}", tag=op)
        elif k < 0.7:
            op = self.r.choice(["cmpps", "cmppd", "cmpss", "cmpsd"])
            src = self.xsrc() if op in ("cmpps", "cmppd") else self.xmm()
            self.emit(f"{op} {x}, {src}, {self.r.randrange(0, 8)}", tag=op)
        elif k < 0.78:
            op = self.r.choice(["comiss", "ucomiss", "comisd", "ucomisd"])
            self.emit(f"{op} {x}, {self.xmm()}", writes=ALLF, tag=op)
        elif k < 0.9:
            op = self.r.choice(["roundps", "roundpd", "roundss", "roundsd"])
            self.emit(f"{op} {x}, {self.xmm()}, {self.r.randrange(0, 16)}", tag=op)
        else:
            op = self.r.choice(["dpps", "dppd", "blendps", "blendpd", "shufps", "shufpd", "insertps"])
            imm = self.r.randrange(0, 256)
            if op in ("blendpd", "shufpd"):
                imm &= 3
            if op == "dppd":
                imm &= 0x33
            self.emit(f"{op} {x}, {self.xmm()}, {imm}", tag=op)

    def op_sse_cvt(self):
        k = self.r.random()
        if k < 0.5:
            op = self.r.choice(["cvtps2pd", "cvtpd2ps", "cvtdq2ps", "cvtps2dq", "cvttps2dq",
                                "cvtdq2pd", "cvtpd2dq", "cvttpd2dq"])
            self.emit(f"{op} {self.xmm()}, {self.xmm()}", tag=op)
        elif k < 0.8:
            op = self.r.choice(["cvtss2si", "cvttss2si", "cvtsd2si", "cvttsd2si"])
            self.emit(f"{op} {self.gpr(self.r.choice([64, 32]))[0]}, {self.xmm()}", tag=op)
        else:
            op = self.r.choice(["cvtsi2ss", "cvtsi2sd"])
            self.emit(f"{op} {self.xmm()}, {self.gpr(self.r.choice([64, 32]))[0]}", tag=op)

    def op_sse_int(self):
        x = self.xmm()
        k = self.r.random()
        if k < 0.55:
            op = self.r.choice([
                "paddb", "paddw", "paddd", "paddq", "psubb", "psubw", "psubd", "psubq", "paddsb",
                "paddsw", "paddusb", "paddusw", "psubsb", "psubsw", "psubusb", "psubusw", "pmullw",
                "pmulhw", "pmulhuw", "pmuludq", "pmaddwd", "psadbw", "pavgb", "pavgw", "pminub",
                "pmaxub", "pminsw", "pmaxsw", "pcmpeqb", "pcmpeqw", "pcmpeqd", "pcmpgtb", "pcmpgtw",
                "pcmpgtd", "pand", "pandn", "por", "pxor", "packsswb", "packssdw", "packuswb",
                "punpcklbw", "punpcklwd", "punpckldq", "punpcklqdq", "punpckhbw", "punpckhwd",
                "punpckhdq", "punpckhqdq",
                # SSSE3
                "pshufb", "phaddw", "phaddd", "phsubw", "pmaddubsw", "pmulhrsw", "pabsb", "pabsw",
                "pabsd", "psignb", "psignw", "psignd",
                # SSE4.1 / 4.2
                "pminsb", "pmaxsb", "pminsd", "pmaxsd", "pminuw", "pmaxuw", "pminud", "pmaxud",
                "pmulld", "pmuldq", "packusdw", "pcmpeqq", "pcmpgtq", "phminposuw",
                "pmovzxbw", "pmovsxbw", "pmovzxwd", "pmovsxwd", "pmovzxdq", "pmovsxdq"])
            self.emit(f"{op} {x}, {self.xsrc()}", tag=op)
        elif k < 0.75:
            op = self.r.choice(["pshufd", "pshufhw", "pshuflw", "palignr", "mpsadbw", "pblendw"])
            self.emit(f"{op} {x}, {self.xmm()}, {self.r.randrange(0, 256)}", tag=op)
        elif k < 0.85:
            op = self.r.choice(["psllw", "pslld", "psllq", "psrlw", "psrld", "psrlq", "psraw",
                                "psrad", "pslldq", "psrldq"])
            self.emit(f"{op} {x}, {self.r.choice([0, 1, 7, 8, 15, 16, 31, 32, 63, 64, self.r.randrange(0, 256)])}", tag=op)
        elif k < 0.92:
            op = self.r.choice(["pmovmskb", "movmskps", "movmskpd"])
            self.emit(f"{op} {SUB[self.r.choice(GPRS)][1]}, {self.xmm()}", tag=op)
        elif k < 0.96:
            self.emit(f"ptest {x}, {self.xmm()}", writes=ALLF, tag="ptest")
        else:
            imm = self.r.randrange(0, 128)
            self.emit(f"pcmpistri {x}, {self.xmm()}, {imm}", writes=ALLF, tag="pcmpistri")

    def op_sse_move(self):
        k = self.r.random()
        if k < 0.3:
            self.emit(f"movdqu [r13 + {self.r.randrange(0, DATA_SIZE - 15)}], {self.xmm()}", tag="movdqu_st")
        elif k < 0.5:
            self.emit(f"movdqu {self.xmm()}, [r13 + {self.r.randrange(0, DATA_SIZE - 15)}]", tag="movdqu_ld")
        elif k < 0.65:
            op = self.r.choice(["movss", "movsd", "movhlps", "movlhps"])
            self.emit(f"{op} {self.xmm()}, {self.xmm()}", tag=op)
        elif k < 0.8:
            op = self.r.choice(["movq", "movd"])
            size = 64 if op == "movq" else 32
            if self.r.random() < 0.5:
                self.emit(f"{op} {self.xmm()}, {self.gpr(size)[0]}", tag=op + "_x")
            else:
                self.emit(f"{op} {self.gpr(size)[0]}, {self.xmm()}", tag=op + "_g")
        else:
            op = self.r.choice(["pextrb", "pextrw", "pextrd", "pextrq", "pinsrb", "pinsrw", "pinsrd", "pinsrq", "extractps"])
            lanes = {"b": 16, "w": 8, "d": 4, "q": 2, "s": 4}[op[-1]]
            g = SUB[self.r.choice(GPRS)][0 if op.endswith("q") else 1]
            if op.startswith("pins"):
                self.emit(f"{op} {self.xmm()}, {g}, {self.r.randrange(0, lanes)}", tag=op)
            else:
                self.emit(f"{op} {g}, {self.xmm()}, {self.r.randrange(0, lanes)}", tag=op)

    FAMILIES = [("op_alu", 18), ("op_unary", 5), ("op_shift", 8), ("op_mul", 5), ("op_div", 2),
                ("op_bits", 5), ("op_move", 10), ("op_sse_float", 16), ("op_sse_cvt", 7),
                ("op_sse_int", 16), ("op_sse_move", 8)]

    def body(self, n):
        names = [f for f, _ in self.FAMILIES]
        weights = [w for _, w in self.FAMILIES]
        for _ in range(n):
            getattr(self, self.r.choices(names, weights)[0])()


def f32s(r, n):
    out = b""
    for _ in range(n):
        v = r.choice(F32_SPECIAL) if r.random() < 0.6 else r.getrandbits(32)
        out += v.to_bytes(4, "little")
    return out


def f64s(r, n):
    out = b""
    for _ in range(n):
        v = r.choice(F64_SPECIAL) if r.random() < 0.6 else r.getrandbits(64)
        out += v.to_bytes(8, "little")
    return out


def make_case(seed, idx):
    r = random.Random(f"{seed}:{idx}")
    mem = bytearray(MEM_IMAGE)
    for off in range(0, DATA_SIZE, 16):
        mem[off:off + 16] = r.choice([f32s(r, 4), f64s(r, 2), r.randbytes(16)])
    for i in range(8):
        mem[XMM_INIT + 16 * i:XMM_INIT + 16 * i + 16] = r.choice([f32s(r, 4), f64s(r, 2), r.randbytes(16)])
    mxcsr = 0x1F80 | (r.randrange(0, 4) << 13)                       # all exceptions masked; RC
    if r.random() < 0.3:
        mxcsr |= 0x8000                                               # FTZ
    if r.random() < 0.3:
        mxcsr |= 0x0040                                               # DAZ
    mem[MXCSR_OFF:MXCSR_OFF + 4] = mxcsr.to_bytes(4, "little")

    g = Gen(r)
    pro = [f"mov r13, {SCRATCH:#x}", f"ldmxcsr [r13 + {MXCSR_OFF:#x}]"]
    pro += [f"movdqu xmm{i}, [r13 + {XMM_INIT + 16 * i:#x}]" for i in range(8)]
    pro += [f"mov {reg}, {g.gimm():#x}" for reg in GPRS]
    g.body(r.randrange(6, 25))
    mask = ALLF & ~g.undef
    epi = ["pushfq", "pop r15", f"and r15, {mask:#x}",
           f"stmxcsr [r13 + {MXCSR_OUT:#x}]", f"mov r14d, dword [r13 + {MXCSR_OUT:#x}]"]
    epi += [f"movdqu xmm{8 + i}, [r13 + {16 * i:#x}]" for i in range(8)]
    return {"name": f"diff_{seed}_{idx:05d}", "mem": bytes(mem), "lines": pro + g.lines + epi,
            "flag_mask": mask, "mxcsr": mxcsr, "cover": sorted(g.cover)}


def render(case, regdata=None, native=False):
    lines = list(case["lines"])
    if not native:
        # FEX's harness provides no usable stack (its own tests set RSP); use the top of the
        # scratch page. Natively the caller's real stack is used. RSP is never compared.
        lines.insert(1, "lea rsp, [r13 + 0xff0]")
    body = "\n".join(lines)
    end = "ret" if native else "hlt"
    hdr = ""
    if regdata is not None:
        cfg = {"Match": "All", "RegData": regdata,
               "MemoryRegions": {f"{SCRATCH:#x}": "4096"},
               "MemoryData": {f"{SCRATCH:#x}": " ".join(f"{b:02X}" for b in case["mem"])}}
        hdr = "%ifdef CONFIG\n" + json.dumps(cfg, indent=2) + "\n%endif\n"
    return f"{hdr}BITS 64\n; flags mask {case['flag_mask']:#x}, mxcsr {case['mxcsr']:#x}\n{body}\n{end}\n"


def sh(cmd, **kw):
    return subprocess.run(cmd, capture_output=True, text=True, **kw)


def build_native_runner():
    p = sh(["make", "-s", "-C", str(NATIVE_DIR)])
    if p.returncode:
        sys.exit("native runner build failed:\n" + p.stdout + p.stderr)


def gen_one(args):
    seed, idx, out = args
    c = make_case(seed, idx)
    d = pathlib.Path(out)
    nat = d / "native" / (c["name"] + ".asm")
    nat.write_text(render(c, native=True))
    nbin = nat.with_suffix(".bin")
    p = sh(["nasm", "-f", "bin", str(nat), "-o", str(nbin)])
    if p.returncode:
        return {"name": c["name"], "error": "nasm: " + p.stderr.strip()}
    (d / "native" / (c["name"] + ".hex")).write_text(c["mem"].hex())
    p = sh([str(NATIVE_RUNNER), str(nbin), str(d / "native" / (c["name"] + ".hex"))], timeout=20)
    if p.returncode:
        return {"name": c["name"], "error": f"native run exit {p.returncode}: {p.stderr.strip()}"}
    regs = json.loads(p.stdout)
    (d / (c["name"] + ".asm")).write_text(render(c, regdata=regs))
    return {"name": c["name"], "cover": c["cover"]}


def cmd_gen(seed, count, out, jobs):
    build_native_runner()
    out = pathlib.Path(out)
    shutil.rmtree(out, ignore_errors=True)
    (out / "native").mkdir(parents=True)
    with cf.ThreadPoolExecutor(jobs) as ex:
        res = list(ex.map(gen_one, [(seed, i, str(out)) for i in range(count)]))
    errs = [x for x in res if "error" in x]
    cover = sorted({t for x in res if "cover" in x for t in x["cover"]})
    (out / "coverage.json").write_text(json.dumps(cover, indent=0))
    print(f"generated {count - len(errs)} cases in {out} ({len(cover)} distinct instruction forms)")
    for e in errs[:20]:
        print(f"  generator error {e['name']}: {e['error']}")
    return 1 if errs else 0


def fex_case(args):
    asm, runner, env_extra = args
    name = asm.stem
    work = asm.parent / "fex"
    tmp = work / (name + ".asm")
    binf, cfgf = work / (name + ".bin"), work / (name + ".config.bin")
    shutil.copy(asm, tmp)
    p = sh(["nasm", "-f", "bin", str(tmp), "-o", str(binf)])
    if p.returncode:
        return name, "error", p.stderr
    p = sh([sys.executable, str(FEX_SCRIPTS / "json_asm_config_parse.py"), str(asm), str(cfgf)], cwd=FEX_SCRIPTS)
    if p.returncode:
        return name, "error", p.stdout + p.stderr
    env = dict(os.environ, FEX_SILENTLOG="0", FEX_DUMPGPRS="1", **env_extra)
    try:
        p = sh([runner, str(binf), str(cfgf)], env=env, timeout=120)
    except subprocess.TimeoutExpired:
        return name, "fail", "timeout"
    return name, ("pass" if p.returncode == 0 else "fail"), (p.stdout + p.stderr)[-4000:]


# Same JIT configurations FEX's own CMake uses for ASM tests on ARM64 / the vixl simulator.
FEX_CONFIGS = {
    "jit_1": {"FEX_MAXINST": "1", "FEX_MULTIBLOCK": "0", "FEX_TSOENABLED": "0"},
    "jit_500_m": {"FEX_MAXINST": "500", "FEX_MULTIBLOCK": "1", "FEX_TSOENABLED": "0"},
}


def cmd_run(cases, runner, jobs, configs, quiet=False):
    # FEX's config parser runs with cwd=FEX_SCRIPTS, so every path handed to it is absolute.
    cases = pathlib.Path(cases).resolve()
    runner = str(pathlib.Path(runner).resolve())
    (cases / "fex").mkdir(exist_ok=True)
    asms = sorted(cases.glob("diff_*.asm"))
    total_fail = 0
    report = {}
    for cname in configs:
        env = FEX_CONFIGS[cname]
        with cf.ThreadPoolExecutor(jobs) as ex:
            res = list(ex.map(fex_case, [(a, runner, env) for a in asms]))
        fails = [(n, s, o) for n, s, o in res if s != "pass"]
        report[cname] = {"cases": len(res), "fail": [n for n, _, _ in fails]}
        total_fail += len(fails)
        if not quiet:
            print(f"[{cname}] {len(res) - len(fails)}/{len(res)} pass")
            for n, s, o in fails[:15]:
                print(f"  {s.upper()} {n}\n    " + "\n    ".join(l for l in o.splitlines() if l.strip())[-1500:])
    (cases / "report.json").write_text(json.dumps(report, indent=1))
    return total_fail


def cmd_selftest(runner, cases=None):
    """Corrupt one expectation per kind in cases FEX passes, and require FEX's runner to fail.
    With --cases, reuse already generated cases (e.g. on an ARM64 host, which can't run the
    native x86 side); otherwise generate a fresh batch."""
    out = ROOT / "build/diff/selftest"
    runner = str(pathlib.Path(runner).resolve())
    shutil.rmtree(out, ignore_errors=True)
    if cases:
        out.mkdir(parents=True)
        for a in sorted(pathlib.Path(cases).glob("diff_*.asm"))[:200]:
            shutil.copy(a, out / a.name)
    elif cmd_gen("selftest", 40, out, os.cpu_count()):
        return 1
    (out / "fex").mkdir(exist_ok=True)
    # Injection only means something on cases FEX already passes; pick the first three.
    clean = []
    for asm in sorted(out.glob("diff_*.asm")):
        if fex_case((asm, runner, FEX_CONFIGS["jit_500_m"]))[1] == "pass":
            clean.append(asm)
        if len(clean) == 3:
            break
    if len(clean) < 3:
        print("selftest: fewer than 3 clean cases; cannot judge injection")
        return 1
    caught = 0
    kinds = [("RAX", 0), ("R15", 0), ("XMM8", 1)]   # a GPR, the flags register, a memory-derived XMM
    for asm, (reg, lane) in zip(clean, kinds):
        t = asm.read_text()
        head, rest = t.split("%endif", 1)
        cfg = json.loads(head.split("%ifdef CONFIG", 1)[1])
        v = cfg["RegData"][reg]
        if isinstance(v, list):
            v[lane] = hex(int(v[lane], 16) ^ 1)
        else:
            cfg["RegData"][reg] = hex(int(v, 16) ^ 1)
        asm.write_text("%ifdef CONFIG\n" + json.dumps(cfg, indent=2) + "\n%endif" + rest)
        name, status, _ = fex_case((asm, runner, FEX_CONFIGS["jit_500_m"]))
        print(f"selftest: flipped one bit of {reg} in {name}: FEX runner {status}")
        caught += status == "fail"
    ok = caught == len(kinds)
    print("selftest: " + ("PASS, every injected mismatch was detected" if ok else "FAIL, an injected mismatch went undetected"))
    return 0 if ok else 1


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sp = ap.add_subparsers(dest="cmd", required=True)
    for name in ("gen", "run", "selftest", "all"):
        p = sp.add_parser(name)
        p.add_argument("--jobs", type=int, default=os.cpu_count())
        if name in ("gen", "all"):
            p.add_argument("--seed", default="1")
            p.add_argument("--count", type=int, default=500)
        if name in ("gen",):
            p.add_argument("--out", default=str(ROOT / "build/diff/cases"))
        if name in ("run",):
            p.add_argument("--cases", default=str(ROOT / "build/diff/cases"))
        if name in ("selftest",):
            p.add_argument("--cases", default=None, help="reuse generated cases instead of generating")
        if name in ("run", "selftest", "all"):
            p.add_argument("--runner", default=str(ROOT / "build/fex-sim/Bin/TestHarnessRunner"))
        if name in ("run", "all"):
            p.add_argument("--configs", default=",".join(FEX_CONFIGS))
    a = ap.parse_args()
    if a.cmd == "gen":
        return cmd_gen(a.seed, a.count, a.out, a.jobs)
    if a.cmd == "run":
        return 1 if cmd_run(a.cases, a.runner, a.jobs, a.configs.split(",")) else 0
    if a.cmd == "selftest":
        return cmd_selftest(a.runner, a.cases)
    out = ROOT / "build/diff/cases"
    rc = cmd_gen(a.seed, a.count, out, a.jobs)
    rc |= 1 if cmd_run(out, a.runner, a.jobs, a.configs.split(",")) else 0
    rc |= cmd_selftest(a.runner)
    return rc


if __name__ == "__main__":
    sys.exit(main())
