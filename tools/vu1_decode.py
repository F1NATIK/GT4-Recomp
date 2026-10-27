"""VU1 microcode decoder mirroring ps2xRuntime/src/lib/vu (ps2_vu1_core.cpp
decodeUpperUsage/decodeLowerUsage/decodeInstructionPair, ps2_vu1_upper.cpp,
ps2_vu1_lower.cpp). Used by the disassembler below and by vu1_translate.py.

usage: python vu1_decode.py <code.bin | kicks.bin> [start_hex end_hex]
"""
import struct
import sys
from dataclasses import dataclass, field

FMAC_LATENCY = 4
ACC_FORWARD_LATENCY = 1

PIPE_NONE, PIPE_FMAC, PIPE_LSU, PIPE_FDIV, PIPE_EFU, PIPE_IALU, PIPE_BRANCH, PIPE_XGKICK = range(8)


def lane(component):
    return 1 << (3 - component)


def DEST(i): return (i >> 21) & 0xF
def FT(i): return (i >> 16) & 0x1F
def FS(i): return (i >> 11) & 0x1F
def FD(i): return (i >> 6) & 0x1F
def VIT(i): return (i >> 16) & 0xF
def VIS(i): return (i >> 11) & 0xF
def VID(i): return (i >> 6) & 0xF


def IMM11(i):
    v = i & 0x7FF
    return v - 0x800 if v & 0x400 else v


@dataclass
class Usage:
    vf_read: list = field(default_factory=list)  # [(reg, lanes)] in declaration order (max 2)
    vf_write: tuple = (0, 0)
    vi_read: int = 0
    vi_write: int = 0
    acc_read: int = 0
    acc_write: int = 0
    latency: int = 0
    vf_latency: int = 0
    vi_latency: int = 0
    pipeline: int = PIPE_NONE
    wait_q: bool = False
    wait_p: bool = False
    reads_clip: bool = False
    writes_clip: bool = False
    delays_next_branch_read: bool = False
    reserved: bool = False

    def add_vf_read(self, reg, lanes):
        if lanes == 0:
            return
        for idx, (r, l) in enumerate(self.vf_read):
            if r == reg:
                self.vf_read[idx] = (r, l | lanes)
                return
        if len(self.vf_read) < 2:
            self.vf_read.append((reg, lanes))

    def add_vf_write(self, reg, lanes):
        if reg == 0 or lanes == 0:
            return
        if self.vf_write[0] == 0:
            self.vf_write = (reg, lanes)
        elif self.vf_write[0] == reg:
            self.vf_write = (reg, self.vf_write[1] | lanes)

    def vf_read_lanes(self, reg):
        for r, l in self.vf_read:
            if r == reg:
                return l
        return 0


def upper_usage(upper):
    u = Usage(pipeline=PIPE_FMAC, latency=FMAC_LATENCY)
    op = upper & 0x3F
    dest, fs, ft, fd = DEST(upper), FS(upper), FT(upper), FD(upper)
    if op <= 0x2F:
        u.add_vf_read(fs, dest)
        u.add_vf_write(fd, dest)
        if op <= 0x1B:
            u.add_vf_read(ft, lane(op & 3))
        elif op >= 0x28:
            u.add_vf_read(ft, 0xE if op == 0x2E else dest)
        if op in (0x08, 0x09, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F, 0x21, 0x23, 0x25, 0x27, 0x29, 0x2D, 0x2E):
            u.acc_read = dest
        return u
    if op >= 0x3C:
        sp = (upper & 3) | ((upper >> 4) & 0x7C)
        writes_acc = sp <= 0x0F or 0x18 <= sp <= 0x1C or sp == 0x1E or 0x20 <= sp <= 0x2A or 0x2C <= sp <= 0x2E
        if writes_acc:
            u.add_vf_read(fs, dest)
            if sp <= 0x1B:
                u.add_vf_read(ft, lane(sp & 3))
            elif 0x28 <= sp <= 0x2E:
                u.add_vf_read(ft, 0xE if sp == 0x2E else dest)
            u.acc_write = dest
            if 0x08 <= sp <= 0x0F or sp in (0x21, 0x23, 0x25, 0x27, 0x29, 0x2D):
                u.acc_read = dest
        elif 0x10 <= sp <= 0x17:
            u.add_vf_read(fs, dest)
            u.add_vf_write(ft, dest)
        elif sp == 0x1D:
            u.add_vf_read(fs, dest)
            u.add_vf_write(ft, dest)
        elif sp == 0x1F:
            u.add_vf_read(fs, 0xE)
            u.add_vf_read(ft, 0x1)
            u.writes_clip = True
        elif sp not in (0x2F, 0x30):
            u.reserved = True
        return u
    u.reserved = True
    return u


def lower_usage(lower):
    u = Usage()
    if lower == 0 or lower == 0x8000033C:
        return u
    op = (lower >> 25) & 0x7F
    vfT, vfS, viT, viS, viD, dest = FT(lower), FS(lower), VIT(lower), VIS(lower), VID(lower), DEST(lower)

    def rvi(r):
        if r:
            u.vi_read |= 1 << r

    def wvi(r):
        if r:
            u.vi_write |= 1 << r

    if op == 0x00:
        u.pipeline, u.latency = PIPE_LSU, 4; rvi(viS); u.add_vf_write(vfT, dest); return u
    if op == 0x01:
        u.pipeline, u.latency = PIPE_LSU, 1; rvi(viT); u.add_vf_read(vfS, dest); return u
    if op == 0x04:
        u.pipeline, u.latency = PIPE_LSU, 4; rvi(viS); wvi(viT); return u
    if op == 0x05:
        u.pipeline, u.latency = PIPE_LSU, 1; rvi(viS); rvi(viT); return u
    if op in (0x08, 0x09):
        u.pipeline, u.latency = PIPE_IALU, 1; u.delays_next_branch_read = True; rvi(viS); wvi(viT); return u
    if op in (0x10, 0x12, 0x13):
        u.pipeline, u.latency = PIPE_IALU, 1; u.reads_clip = True; wvi(1); return u
    if op == 0x11:
        u.pipeline, u.latency = PIPE_FMAC, FMAC_LATENCY; u.writes_clip = True; return u
    if op in (0x14, 0x16, 0x17):
        u.pipeline, u.latency = PIPE_IALU, 1; wvi(viT); return u
    if op == 0x15:
        u.pipeline, u.latency = PIPE_FMAC, FMAC_LATENCY; return u
    if op in (0x18, 0x1A, 0x1B):
        u.pipeline, u.latency = PIPE_IALU, 1; rvi(viS); wvi(viT); return u
    if op == 0x1C:
        u.pipeline, u.latency = PIPE_IALU, 1; u.reads_clip = True; wvi(viT); return u
    if op == 0x20:
        u.pipeline = PIPE_BRANCH; return u
    if op == 0x21:
        u.pipeline, u.latency = PIPE_BRANCH, 1; wvi(viT); return u
    if op == 0x24:
        u.pipeline = PIPE_BRANCH; rvi(viS); return u
    if op == 0x25:
        u.pipeline, u.latency = PIPE_BRANCH, 1; rvi(viS); wvi(viT); return u
    if op in (0x28, 0x29):
        u.pipeline = PIPE_BRANCH; rvi(viS); rvi(viT); return u
    if op in (0x2C, 0x2D, 0x2E, 0x2F):
        u.pipeline = PIPE_BRANCH; rvi(viS); return u
    if op != 0x40:
        u.reserved = True
        return u
    direct = lower & 0x3F
    if direct in (0x30, 0x31, 0x34, 0x35):
        u.pipeline, u.latency = PIPE_IALU, 1; u.delays_next_branch_read = True; rvi(viS); rvi(viT); wvi(viD); return u
    if direct == 0x32:
        u.pipeline, u.latency = PIPE_IALU, 1; u.delays_next_branch_read = True; rvi(viS); wvi(viT); return u
    if direct < 0x3C:
        u.reserved = True
        return u
    sp = (lower & 3) | ((lower >> 4) & 0x7C)
    if sp in (0x30, 0x31):
        u.pipeline, u.latency = PIPE_FMAC, 4; u.add_vf_read(vfS, 0xF if sp == 0x31 else dest); u.add_vf_write(vfT, dest)
    elif sp in (0x34, 0x36):
        u.pipeline, u.latency, u.vi_latency = PIPE_LSU, 4, 1; u.delays_next_branch_read = True
        rvi(viS); wvi(viS); u.add_vf_write(vfT, dest)
    elif sp in (0x35, 0x37):
        u.pipeline, u.latency = PIPE_LSU, 1; u.delays_next_branch_read = True
        rvi(viT); wvi(viT); u.add_vf_read(vfS, dest)
    elif sp == 0x38:
        u.pipeline, u.latency = PIPE_FDIV, 7
        u.add_vf_read(vfS, lane((lower >> 21) & 3)); u.add_vf_read(vfT, lane((lower >> 23) & 3))
    elif sp == 0x39:
        u.pipeline, u.latency = PIPE_FDIV, 7; u.add_vf_read(vfT, lane((lower >> 23) & 3))
    elif sp == 0x3A:
        u.pipeline, u.latency = PIPE_FDIV, 13
        u.add_vf_read(vfS, lane((lower >> 21) & 3)); u.add_vf_read(vfT, lane((lower >> 23) & 3))
    elif sp == 0x3B:
        u.pipeline = PIPE_FDIV; u.wait_q = True
    elif sp == 0x3C:
        u.pipeline, u.latency = PIPE_IALU, 1; u.delays_next_branch_read = True
        u.add_vf_read(vfS, lane((lower >> 21) & 3)); wvi(viT)
    elif sp == 0x3D:
        u.pipeline, u.latency = PIPE_FMAC, 4; rvi(viS); u.add_vf_write(vfT, dest)
    elif sp == 0x3E:
        u.pipeline, u.latency = PIPE_LSU, 4; rvi(viS); wvi(viT)
    elif sp == 0x3F:
        u.pipeline, u.latency = PIPE_LSU, 1; rvi(viS); rvi(viT)
    elif sp in (0x40, 0x41):
        u.pipeline, u.latency = PIPE_FMAC, 4; u.add_vf_write(vfT, dest)
    elif sp in (0x42, 0x43):
        u.pipeline, u.latency = PIPE_IALU, 1; u.add_vf_read(vfS, lane((lower >> 21) & 3))
    elif sp == 0x64:
        u.pipeline, u.latency = PIPE_FMAC, 4; u.add_vf_write(vfT, dest)
    elif sp in (0x68, 0x69):
        u.pipeline, u.latency = PIPE_IALU, 1; wvi(viT)
    elif sp == 0x6C:
        u.pipeline, u.latency = PIPE_XGKICK, 2; rvi(viS)
    elif sp in (0x70, 0x71, 0x72, 0x73, 0x74, 0x75, 0x76, 0x77, 0x78, 0x79, 0x7A, 0x7C, 0x7D):
        u.pipeline = PIPE_EFU
        u.latency = {0x70: 11, 0x71: 18, 0x72: 18, 0x77: 18, 0x73: 24, 0x74: 54, 0x75: 54, 0x7C: 54,
                     0x76: 12, 0x78: 12, 0x7A: 12, 0x79: 29, 0x7D: 44}[sp]
        if 0x70 <= sp <= 0x73:
            u.add_vf_read(vfS, 0xE)
        elif sp == 0x74:
            u.add_vf_read(vfS, 0xC)
        elif sp == 0x75:
            u.add_vf_read(vfS, 0xA)
        elif sp == 0x76:
            u.add_vf_read(vfS, 0xF)
        else:
            u.add_vf_read(vfS, lane((lower >> 21) & 3))
    elif sp == 0x7B:
        u.pipeline = PIPE_EFU; u.wait_p = True
    else:
        u.reserved = True
    return u


@dataclass
class Pair:
    pc: int
    lower: int
    upper: int
    i_bit: bool
    e_bit: bool
    m_bit: bool
    d_bit: bool
    t_bit: bool
    uu: Usage
    lu: Usage
    upper_shadow: int = 0
    suppressed_lower_vf: int = 0


def decode_pair(code, pc):
    lower, upper = struct.unpack_from('<II', code, pc)
    uu = upper_usage(upper)
    i_bit = bool(upper & 0x80000000)
    lu = Usage() if i_bit else lower_usage(lower)
    p = Pair(pc, lower, upper, i_bit, bool(upper & 0x40000000), bool(upper & 0x20000000),
             bool(upper & 0x10000000), bool(upper & 0x08000000), uu, lu)
    w = uu.vf_write[0]
    if w and (lu.vf_read_lanes(w) or lu.vf_write[0] == w):
        p.upper_shadow = w
        if lu.vf_write[0] == w:
            p.suppressed_lower_vf = w
    return p


# ---------------------------------------------------------------- disassembly
BC = 'xyzw'


def dest_str(d):
    return ''.join(c for k, c in enumerate('xyzw') if d & lane(k))


UPPER_NAMES = {}
for k, n in enumerate(['add', 'sub', 'madd', 'msub', 'max', 'mini', 'mul']):
    for b in range(4):
        UPPER_NAMES[k * 4 + b] = n + BC[b]
UPPER_NAMES.update({0x1C: 'mulq', 0x1D: 'maxi', 0x1E: 'muli', 0x1F: 'minii', 0x20: 'addq', 0x21: 'maddq', 0x22: 'addi',
                    0x23: 'maddi', 0x24: 'subq', 0x25: 'msubq', 0x26: 'subi', 0x27: 'msubi', 0x28: 'add', 0x29: 'madd',
                    0x2A: 'mul', 0x2B: 'max', 0x2C: 'sub', 0x2D: 'msub', 0x2E: 'opmsub', 0x2F: 'mini'})
UPPER_SPECIAL = {}
for k, n in enumerate(['adda', 'suba', 'madda', 'msuba']):
    for b in range(4):
        UPPER_SPECIAL[k * 4 + b] = n + BC[b]
for b in range(4):
    UPPER_SPECIAL[0x18 + b] = 'mula' + BC[b]
UPPER_SPECIAL.update({0x10: 'itof0', 0x11: 'itof4', 0x12: 'itof12', 0x13: 'itof15', 0x14: 'ftoi0', 0x15: 'ftoi4',
                      0x16: 'ftoi12', 0x17: 'ftoi15', 0x1C: 'mulaq', 0x1D: 'abs', 0x1E: 'mulai', 0x1F: 'clip',
                      0x20: 'addaq', 0x21: 'maddaq', 0x22: 'addai', 0x23: 'maddai', 0x24: 'subaq', 0x25: 'msubaq',
                      0x26: 'subai', 0x27: 'msubai', 0x28: 'adda', 0x29: 'madda', 0x2A: 'mula', 0x2C: 'suba',
                      0x2D: 'msuba', 0x2E: 'opmula', 0x2F: 'nop', 0x30: 'nop'})
LOWER_SPECIAL = {0x30: 'move', 0x31: 'mr32', 0x34: 'lqi', 0x35: 'sqi', 0x36: 'lqd', 0x37: 'sqd', 0x38: 'div',
                 0x39: 'sqrt', 0x3A: 'rsqrt', 0x3B: 'waitq', 0x3C: 'mtir', 0x3D: 'mfir', 0x3E: 'ilwr', 0x3F: 'iswr',
                 0x40: 'rnext', 0x41: 'rget', 0x42: 'rinit', 0x43: 'rxor', 0x64: 'mfp', 0x68: 'xtop', 0x69: 'xitop',
                 0x6C: 'xgkick', 0x70: 'esadd', 0x71: 'ersadd', 0x72: 'eleng', 0x73: 'erleng', 0x74: 'eatanxy',
                 0x75: 'eatanxz', 0x76: 'esum', 0x77: 'ersqrt', 0x78: 'esqrt', 0x79: 'esin', 0x7A: 'ercpr',
                 0x7B: 'waitp', 0x7C: 'eatan', 0x7D: 'eexp'}
LOWER_NAMES = {0x00: 'lq', 0x01: 'sq', 0x04: 'ilw', 0x05: 'isw', 0x08: 'iaddiu', 0x09: 'isubiu', 0x10: 'fceq',
               0x11: 'fcset', 0x12: 'fcand', 0x13: 'fcor', 0x14: 'fseq', 0x15: 'fsset', 0x16: 'fsand', 0x17: 'fsor',
               0x18: 'fmeq', 0x1A: 'fmand', 0x1B: 'fmor', 0x1C: 'fcget', 0x20: 'b', 0x21: 'bal', 0x24: 'jr',
               0x25: 'jalr', 0x28: 'ibeq', 0x29: 'ibne', 0x2C: 'ibltz', 0x2D: 'ibgtz', 0x2E: 'iblez', 0x2F: 'ibgez'}


def upper_name(upper):
    op = upper & 0x3F
    if op >= 0x3C:
        return UPPER_SPECIAL.get((upper & 3) | ((upper >> 4) & 0x7C), 'u?')
    return UPPER_NAMES.get(op, 'u?')


def lower_name(lower):
    if lower == 0 or lower == 0x8000033C:
        return 'nop'
    op = (lower >> 25) & 0x7F
    if op == 0x40:
        d = lower & 0x3F
        if d < 0x3C:
            return {0x30: 'iadd', 0x31: 'isub', 0x32: 'iaddi', 0x34: 'iand', 0x35: 'ior'}.get(d, 'l?')
        return LOWER_SPECIAL.get((lower & 3) | ((lower >> 4) & 0x7C), 'l?')
    return LOWER_NAMES.get(op, 'l?')


def fmt_upper(u):
    n = upper_name(u)
    d = dest_str(DEST(u))
    op = u & 0x3F
    if n == 'nop':
        return 'nop'
    if op < 0x3C:
        if op <= 0x1B:
            return '%s.%s vf%d, vf%d, vf%d%s' % (n, d, FD(u), FS(u), FT(u), BC[op & 3])
        if op in (0x1C, 0x1D, 0x1E, 0x1F, 0x20, 0x21, 0x22, 0x23, 0x24, 0x25, 0x26, 0x27):
            return '%s.%s vf%d, vf%d' % (n, d, FD(u), FS(u))
        return '%s.%s vf%d, vf%d, vf%d' % (n, d, FD(u), FS(u), FT(u))
    sp = (u & 3) | ((u >> 4) & 0x7C)
    if 0x10 <= sp <= 0x17 or sp == 0x1D:
        return '%s.%s vf%d, vf%d' % (n, d, FT(u), FS(u))
    if sp == 0x1F:
        return 'clip vf%d.xyz, vf%d.w' % (FS(u), FT(u))
    if sp <= 0x0F or 0x18 <= sp <= 0x1B:
        return '%s.%s acc, vf%d, vf%d%s' % (n, d, FS(u), FT(u), BC[sp & 3])
    return '%s.%s acc, vf%d, vf%d' % (n, d, FS(u), FT(u))


def fmt_lower(l, pc):
    n = lower_name(l)
    if n == 'nop':
        return 'nop'
    d = dest_str(DEST(l))
    op = (l >> 25) & 0x7F
    if n in ('lq', 'sq', 'ilw', 'isw'):
        base = VIS(l) if n in ('lq', 'ilw') else VIT(l)
        base = VIS(l) if n == 'isw' else base
        reg = ('vf%d' % FT(l)) if n == 'lq' else ('vf%d' % FS(l)) if n == 'sq' else ('vi%d' % VIT(l))
        return '%s.%s %s, %d(vi%d)' % (n, d, reg, IMM11(l), base)
    if n in ('b', 'bal', 'ibeq', 'ibne', 'ibltz', 'ibgtz', 'iblez', 'ibgez'):
        tgt = (pc + 8 + IMM11(l) * 8) & 0x3FFF
        if n == 'b':
            return 'b 0x%x' % tgt
        if n == 'bal':
            return 'bal vi%d, 0x%x' % (VIT(l), tgt)
        if n in ('ibeq', 'ibne'):
            return '%s vi%d, vi%d, 0x%x' % (n, VIT(l), VIS(l), tgt)
        return '%s vi%d, 0x%x' % (n, VIS(l), tgt)
    if n == 'jr':
        return 'jr vi%d' % VIS(l)
    if n == 'jalr':
        return 'jalr vi%d, vi%d' % (VIT(l), VIS(l))
    if n in ('iaddiu', 'isubiu'):
        imm = (l & 0x7FF) | ((l >> 10) & 0x7800)
        return '%s vi%d, vi%d, 0x%x' % (n, VIT(l), VIS(l), imm)
    if n in ('iadd', 'isub', 'iand', 'ior'):
        return '%s vi%d, vi%d, vi%d' % (n, VID(l), VIS(l), VIT(l))
    if n == 'iaddi':
        imm = (l >> 6) & 0x1F
        imm = imm - 32 if imm & 16 else imm
        return 'iaddi vi%d, vi%d, %d' % (VIT(l), VIS(l), imm)
    if n in ('lqi', 'lqd'):
        return '%s.%s vf%d, (vi%d%s)' % (n, d, FT(l), VIS(l), '++' if n == 'lqi' else '--')
    if n in ('sqi', 'sqd'):
        return '%s.%s vf%d, (vi%d%s)' % (n, d, FS(l), VIT(l), '++' if n == 'sqi' else '--')
    if n in ('move', 'mr32'):
        return '%s.%s vf%d, vf%d' % (n, d, FT(l), FS(l))
    if n in ('div', 'rsqrt'):
        return '%s q, vf%d%s, vf%d%s' % (n, FS(l), BC[(l >> 21) & 3], FT(l), BC[(l >> 23) & 3])
    if n == 'sqrt':
        return 'sqrt q, vf%d%s' % (FT(l), BC[(l >> 23) & 3])
    if n == 'mtir':
        return 'mtir vi%d, vf%d%s' % (VIT(l), FS(l), BC[(l >> 21) & 3])
    if n == 'mfir':
        return 'mfir.%s vf%d, vi%d' % (d, FT(l), VIS(l))
    if n in ('ilwr', 'iswr'):
        return '%s.%s vi%d, (vi%d)' % (n, d, VIT(l), VIS(l))
    if n in ('mfp', 'rnext', 'rget'):
        return '%s.%s vf%d' % (n, d, FT(l))
    if n in ('xtop', 'xitop'):
        return '%s vi%d' % (n, VIT(l))
    if n == 'xgkick':
        return 'xgkick vi%d' % VIS(l)
    if n in ('fmeq', 'fmand', 'fmor'):
        return '%s vi%d, vi%d' % (n, VIT(l), VIS(l))
    if n in ('fsand', 'fsor', 'fseq', 'fsset'):
        return '%s vi%d, 0x%x' % (n, VIT(l), (((l >> 21) & 1) << 11) | (l & 0x7FF))
    if n in ('fcand', 'fcor', 'fceq', 'fcset'):
        return '%s vi1, 0x%x' % (n, l & 0xFFFFFF)
    if n == 'fcget':
        return 'fcget vi%d' % VIT(l)
    if n.startswith('e') or n in ('rinit', 'rxor'):
        return '%s p, vf%d' % (n, FS(l))
    return n


def fmt_pair(p):
    flags = ''.join(c for c, b in zip('IEMDT', (p.i_bit, p.e_bit, p.m_bit, p.d_bit, p.t_bit)) if b)
    lower = ('loi %r' % struct.unpack('<f', struct.pack('<I', p.lower))[0]) if p.i_bit else fmt_lower(p.lower, p.pc)
    return '%04x: %-5s %-34s %s' % (p.pc, flags, fmt_upper(p.upper), lower)


def load_code(path):
    d = open(path, 'rb').read()
    if d[:8] == b'GT3VU1K1':
        # first code image of a kick recording
        return d[8 + 32 + 8 + 664: 8 + 32 + 8 + 664 + 16384]
    return d[:16384]


if __name__ == '__main__':
    code = load_code(sys.argv[1])
    start = int(sys.argv[2], 16) if len(sys.argv) > 2 else 0
    end = int(sys.argv[3], 16) if len(sys.argv) > 3 else len(code)
    for pc in range(start, end, 8):
        print(fmt_pair(decode_pair(code, pc)))
