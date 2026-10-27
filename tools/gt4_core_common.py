"""Shared read-only helpers for the GT4 CORE analysis scripts (2026-10-03).

Reads the reconstructed program ELF (game/CORE.elf) and the Ghidra function
map (analysis/core.csv, columns Name,Start,End,Size). Nothing here writes.

Contents:
  Core            ELF loader: sections, file-backed words, text/data tests.
  FuncMap         Ghidra function ranges: is_start(), owner(), next_start().
  fields() etc.   minimal R5900 field decoding used by the scanners.
  recomp_boundary mirror of ps2xRecomp's "real boundary" rule for extra entry
                  points (ps2_recompiler.cpp, extra_entry_points handling).
  imm_text_refs   GT3's lui+addiu/ori constant scan (scan_core_code_refs.py).
  disasm()        small disassembler for evidence listings (common EE ops only).
Python 3 standard library only.
"""
import bisect
import csv
import hashlib
import os
import struct

ROOT = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), '..'))
DEFAULT_ELF = os.path.join(ROOT, 'game', 'CORE.elf')
DEFAULT_GHIDRA = os.path.join(ROOT, 'analysis', 'core.csv')
DEFAULT_OUT = os.path.join(ROOT, 'analysis', 'recomp-prep')

REG = ['zero', 'at', 'v0', 'v1', 'a0', 'a1', 'a2', 'a3', 't0', 't1', 't2', 't3', 't4', 't5', 't6', 't7',
       's0', 's1', 's2', 's3', 's4', 's5', 's6', 's7', 't8', 't9', 'k0', 'k1', 'gp', 'sp', 'fp', 'ra']

JR_RA = 0x03E00008


def sha256_file(path):
    h = hashlib.sha256()
    with open(path, 'rb') as f:
        for chunk in iter(lambda: f.read(1 << 20), b''):
            h.update(chunk)
    return h.hexdigest()


class Core:
    """Program ELF with file-backed segments. Addresses are EE virtual addresses."""

    def __init__(self, path=DEFAULT_ELF):
        self.path = path
        self.raw = open(path, 'rb').read()
        self.sha256 = hashlib.sha256(self.raw).hexdigest()
        raw = self.raw
        assert raw[:4] == b'\x7fELF', 'not an ELF'
        self.entry, phoff, shoff = struct.unpack_from('<III', raw, 0x18)
        phentsize, phnum, shentsize, shnum, shstrndx = struct.unpack_from('<HHHHH', raw, 0x2a)
        self.segments = []  # (vaddr, offset, filesz, memsz, flags)
        for i in range(phnum):
            p_type, off, vaddr, _paddr, filesz, memsz, flags, _align = struct.unpack_from(
                '<8I', raw, phoff + i * phentsize)
            if p_type == 1:
                self.segments.append((vaddr, off, filesz, memsz, flags))
        self.sections = {}
        sh = [struct.unpack_from('<10I', raw, shoff + i * shentsize) for i in range(shnum)]
        strtab = sh[shstrndx][4] if shnum else 0
        for s in sh:
            name = raw[strtab + s[0]:raw.index(b'\0', strtab + s[0])].decode('latin1')
            if name:
                self.sections[name] = (s[3], s[3] + s[5], s[4], s[2])  # start, end, offset, flags
        self.text = self.sections['.text'][:2]
        self.data = self.sections['.data'][:2]
        self.bss = self.sections['.bss'][:2] if '.bss' in self.sections else (0, 0)
        self._seg_starts = sorted(self.segments)

    def offset(self, addr, n=4):
        for vaddr, off, filesz, _memsz, _flags in self.segments:
            if vaddr <= addr and addr + n <= vaddr + filesz:
                return off + addr - vaddr
        return None

    def word(self, addr):
        o = self.offset(addr, 4)
        return None if o is None else struct.unpack_from('<I', self.raw, o)[0]

    def bytes(self, addr, n):
        o = self.offset(addr, n)
        return None if o is None else self.raw[o:o + n]

    def cstr(self, addr, limit=128):
        o = self.offset(addr, 1)
        if o is None:
            return None
        end = self.raw.find(b'\0', o, o + limit)
        return self.raw[o:end if end >= 0 else o + limit].decode('latin1')

    def in_text(self, addr):
        return self.text[0] <= addr < self.text[1]

    def in_data(self, addr):
        return self.data[0] <= addr < self.data[1]

    def text_words(self):
        t0, t1 = self.text
        o = self.offset(t0)
        n = (t1 - t0) // 4
        return t0, struct.unpack_from('<%dI' % n, self.raw, o)

    def data_words(self):
        d0, d1 = self.data
        o = self.offset(d0)
        n = (d1 - d0) // 4
        return d0, struct.unpack_from('<%dI' % n, self.raw, o)


class FuncMap:
    """Ghidra export (Name,Start,End,Size). End is exclusive."""

    def __init__(self, path=DEFAULT_GHIDRA, clamp=True, extra_rows=()):
        """clamp: cut each End at the next function start. ExportPS2Functions.java
        writes End = max address of the Ghidra body + 1, so a non-contiguous body
        (a far-away shared tail) gives ranges of megabytes (GT4: FUN_001f3398 ends at
        0x4f0db0); ps2_recomp decodes [Start, End) linearly. clamped holds
        (start, old_end, new_end, name) for every row changed."""
        self.path = path
        self.sha256 = sha256_file(path) if os.path.exists(path) else None
        rows = []
        if os.path.exists(path):
            with open(path, newline='') as f:
                for r in csv.DictReader(f):
                    rows.append((int(r['Start'], 16), int(r['End'], 16), r['Name']))
        rows.extend(extra_rows)
        rows.sort()
        self.clamped = []
        if clamp:
            fixed = []
            for i, (s, e, n) in enumerate(rows):
                j = i + 1
                while j < len(rows) and rows[j][0] <= s:
                    j += 1
                nxt = rows[j][0] if j < len(rows) else None
                if nxt is not None and e > nxt:
                    self.clamped.append((s, e, nxt, n))
                    e = nxt
                fixed.append((s, e, n))
            rows = fixed
        self.rows = rows
        self.starts = [r[0] for r in rows]
        self.start_set = set(self.starts)

    def __len__(self):
        return len(self.rows)

    def is_start(self, a):
        return a in self.start_set

    def owner(self, a):
        # Ranges normally do not overlap; look back a few rows in case they do.
        i = bisect.bisect_right(self.starts, a) - 1
        for j in range(i, max(i - 4, -1), -1):
            s, e, _n = self.rows[j]
            if s <= a < e:
                return self.rows[j]
        return None

    def next_start(self, a):
        i = bisect.bisect_right(self.starts, a)
        return self.starts[i] if i < len(self.starts) else None


# ----------------------------------------------------------------- decoding
def fields(w):
    op = w >> 26
    rs = (w >> 21) & 31
    rt = (w >> 16) & 31
    rd = (w >> 11) & 31
    sa = (w >> 6) & 31
    fn = w & 63
    imm = w & 0xFFFF
    simm = imm - 0x10000 if imm & 0x8000 else imm
    return op, rs, rt, rd, sa, fn, imm, simm


def has_delay_slot(w):
    """Branch or jump with a delay slot (same set as GT3's scan_core_code_refs.is_branch,
    plus BC0/BC2 coprocessor branches)."""
    op, rs, rt, _rd, _sa, fn, _imm, _simm = fields(w)
    if op == 0 and fn in (8, 9):
        return True
    if op in (2, 3, 4, 5, 6, 7, 20, 21, 22, 23):
        return True
    if op == 1 and rt in (0, 1, 2, 3, 16, 17, 18, 19):
        return True
    if op in (0x10, 0x11, 0x12) and rs == 8:
        return True
    return False


def jump_target(addr, w):
    return ((addr + 4) & 0xF0000000) | ((w & 0x03FFFFFF) << 2)


def is_stack_prologue(w):
    """addiu sp,sp,-N or daddiu sp,sp,-N."""
    return (w >> 16) in (0x27BD, 0x67BD) and (w & 0x8000) != 0


_VALID_OPS = set(range(0x00, 0x13)) | {0x14, 0x15, 0x16, 0x17, 0x18, 0x19, 0x1A, 0x1B, 0x1C, 0x1E, 0x1F} | \
    set(range(0x20, 0x30)) | {0x31, 0x33, 0x36, 0x37, 0x39, 0x3E, 0x3F}
_VALID_SPECIAL = {0x00, 0x02, 0x03, 0x04, 0x06, 0x07, 0x08, 0x09, 0x0A, 0x0B, 0x0C, 0x0D, 0x0F,
                  0x10, 0x11, 0x12, 0x13, 0x14, 0x16, 0x17, 0x18, 0x19, 0x1A, 0x1B,
                  0x20, 0x21, 0x22, 0x23, 0x24, 0x25, 0x26, 0x27, 0x28, 0x29, 0x2A, 0x2B,
                  0x2C, 0x2D, 0x2E, 0x2F, 0x30, 0x31, 0x32, 0x33, 0x34, 0x36,
                  0x38, 0x3A, 0x3B, 0x3C, 0x3E, 0x3F}
_VALID_REGIMM = {0x00, 0x01, 0x02, 0x03, 0x08, 0x09, 0x0A, 0x0B, 0x0C, 0x0E, 0x10, 0x11, 0x12, 0x13, 0x18, 0x19}


def plausible_ee(w):
    """Coarse R5900 opcode validity (primary/SPECIAL/REGIMM tables only)."""
    op, _rs, rt, _rd, _sa, fn, _imm, _simm = fields(w)
    if op not in _VALID_OPS:
        return False
    if op == 0:
        return fn in _VALID_SPECIAL
    if op == 1:
        return rt in _VALID_REGIMM
    return True


def recomp_boundary(core, addr):
    """Mirror of ps2_recompiler.cpp: an uncovered extra entry starts a new function
    only if, scanning back 8..20 bytes, the first jr $ra / j found is followed (after
    its delay slot) only by zero words up to addr."""
    t0, t1 = core.text
    for back in range(8, 24, 4):
        at = addr - back
        if at < t0 or at + 4 > t1:
            return False
        w = core.word(at)
        if w is None:
            return False
        if not (w == JR_RA or (w >> 26) == 0x02):
            continue
        for pad in range(at + 8, addr, 4):
            if core.word(pad) != 0:
                return False
        return True
    return False


def imm_text_refs(core, lo=None, hi=None, window=64):
    """lui + addiu/ori pairs in .text building an address in [lo,hi) (default .text).
    Same register tracking as GT3's scan_core_code_refs.py. Returns {value: [site,...]}."""
    t0, words = core.text_words()
    lo = core.text[0] if lo is None else lo
    hi = core.text[1] if hi is None else hi
    high = {}
    refs = {}
    for i, w in enumerate(words):
        a = t0 + 4 * i
        op, rs, rt, _rd, _sa, _fn, imm, _simm = fields(w)
        if op == 0x0F:
            high[rt] = (imm << 16, a)
            continue
        if op in (0x09, 0x0D) and rs in high and a - high[rs][1] <= window:
            low = (imm - 0x10000) if (op == 0x09 and imm & 0x8000) else imm
            v = (high[rs][0] + low) & 0xFFFFFFFF
            if lo <= v < hi and v % 4 == 0:
                refs.setdefault(v, []).append(a)
        if rt in high and op in (0x08, 0x09, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x20, 0x21, 0x23, 0x24, 0x25, 0x37) \
                and not (op in (0x09, 0x0D) and rs == rt):
            high.pop(rt, None)
    return refs


def jal_sites(core):
    """{target: [site,...]} for every jal in .text."""
    t0, words = core.text_words()
    out = {}
    for i, w in enumerate(words):
        if (w >> 26) == 3:
            a = t0 + 4 * i
            out.setdefault(jump_target(a, w), []).append(a)
    return out


def const_args(core, site, regs=(4, 5, 6, 7), back=16):
    """Best-effort constant propagation of argument registers at a jal site
    (straight-line window of `back` instructions plus the delay slot)."""
    known = {0: 0}
    seq = [site - 4 * k for k in range(back, 0, -1)] + [site + 4]
    for a in seq:
        w = core.word(a)
        if w is None:
            continue
        op, rs, rt, rd, _sa, fn, imm, simm = fields(w)
        if op == 0x0F:
            known[rt] = imm << 16
        elif op in (0x09, 0x19):
            known[rt] = (known[rs] + simm) & 0xFFFFFFFF if known.get(rs) is not None else None
        elif op == 0x0D:
            known[rt] = (known[rs] | imm) if known.get(rs) is not None else None
        elif op == 0 and fn in (0x2D, 0x21, 0x25) and (rt == 0 or rs == 0):
            src = rs if rt == 0 else rt
            known[rd] = known.get(src)
        elif op == 3 or (op == 0 and fn == 9):
            known = {0: 0}
        elif op == 0 and fn not in (0x08, 0x0C, 0x0F, 0x18, 0x19, 0x1A, 0x1B):
            known.pop(rd, None)
        elif op not in (0x28, 0x29, 0x2B, 0x3F, 0x1F, 0x39, 0x04, 0x05, 0x14, 0x15, 0x01, 0x02, 0x06, 0x07):
            known.pop(rt, None)
        known[0] = 0
    return {r: known.get(r) for r in regs}


# ----------------------------------------------------------------- disassembly
_SPECIAL = {0x00: 'sll', 0x02: 'srl', 0x03: 'sra', 0x04: 'sllv', 0x06: 'srlv', 0x07: 'srav', 0x08: 'jr', 0x09: 'jalr',
            0x0A: 'movz', 0x0B: 'movn', 0x0C: 'syscall', 0x0D: 'break', 0x0F: 'sync', 0x10: 'mfhi', 0x11: 'mthi',
            0x12: 'mflo', 0x13: 'mtlo', 0x14: 'dsllv', 0x16: 'dsrlv', 0x17: 'dsrav', 0x18: 'mult', 0x19: 'multu',
            0x1A: 'div', 0x1B: 'divu', 0x20: 'add', 0x21: 'addu', 0x22: 'sub', 0x23: 'subu', 0x24: 'and',
            0x25: 'or', 0x26: 'xor', 0x27: 'nor', 0x28: 'mfsa', 0x29: 'mtsa', 0x2A: 'slt', 0x2B: 'sltu',
            0x2C: 'dadd', 0x2D: 'daddu', 0x2E: 'dsub', 0x2F: 'dsubu', 0x38: 'dsll', 0x3A: 'dsrl', 0x3B: 'dsra',
            0x3C: 'dsll32', 0x3E: 'dsrl32', 0x3F: 'dsra32'}
_IMM = {0x08: 'addi', 0x09: 'addiu', 0x0A: 'slti', 0x0B: 'sltiu', 0x0C: 'andi', 0x0D: 'ori', 0x0E: 'xori',
        0x18: 'daddi', 0x19: 'daddiu'}
_MEM = {0x1A: 'ldl', 0x1B: 'ldr', 0x1E: 'lq', 0x1F: 'sq', 0x20: 'lb', 0x21: 'lh', 0x22: 'lwl', 0x23: 'lw',
        0x24: 'lbu', 0x25: 'lhu', 0x26: 'lwr', 0x27: 'lwu', 0x28: 'sb', 0x29: 'sh', 0x2A: 'swl', 0x2B: 'sw',
        0x2C: 'sdl', 0x2D: 'sdr', 0x2E: 'swr', 0x2F: 'cache', 0x31: 'lwc1', 0x33: 'pref', 0x36: 'lqc2',
        0x37: 'ld', 0x39: 'swc1', 0x3E: 'sqc2', 0x3F: 'sd'}
_BR = {0x04: 'beq', 0x05: 'bne', 0x06: 'blez', 0x07: 'bgtz', 0x14: 'beql', 0x15: 'bnel', 0x16: 'blezl',
       0x17: 'bgtzl'}
_REGIMM = {0x00: 'bltz', 0x01: 'bgez', 0x02: 'bltzl', 0x03: 'bgezl', 0x10: 'bltzal', 0x11: 'bgezal'}


def disasm(w, pc):
    op, rs, rt, rd, sa, fn, imm, simm = fields(w)
    R = REG
    if w == 0:
        return 'nop'
    if op == 0:
        n = _SPECIAL.get(fn)
        if n is None:
            return '.word 0x%08x' % w
        if fn in (0x00, 0x02, 0x03, 0x38, 0x3A, 0x3B, 0x3C, 0x3E, 0x3F):
            return '%s %s, %s, %d' % (n, R[rd], R[rt], sa)
        if fn == 0x08:
            return 'jr %s' % R[rs]
        if fn == 0x09:
            return 'jalr %s, %s' % (R[rd], R[rs])
        if fn in (0x0C, 0x0D, 0x0F):
            return n
        if fn in (0x10, 0x12):
            return '%s %s' % (n, R[rd])
        if fn in (0x11, 0x13):
            return '%s %s' % (n, R[rs])
        if fn in (0x18, 0x19, 0x1A, 0x1B):
            return '%s %s, %s' % (n, R[rs], R[rt])
        if fn in (0x04, 0x06, 0x07, 0x14, 0x16, 0x17):
            return '%s %s, %s, %s' % (n, R[rd], R[rt], R[rs])
        return '%s %s, %s, %s' % (n, R[rd], R[rs], R[rt])
    if op == 1:
        n = _REGIMM.get(rt)
        return '%s %s, 0x%x' % (n, R[rs], pc + 4 + simm * 4) if n else '.word 0x%08x' % w
    if op in (2, 3):
        return '%s 0x%x' % ('j' if op == 2 else 'jal', jump_target(pc, w))
    if op in _BR:
        if op in (4, 5, 0x14, 0x15):
            return '%s %s, %s, 0x%x' % (_BR[op], R[rs], R[rt], pc + 4 + simm * 4)
        return '%s %s, 0x%x' % (_BR[op], R[rs], pc + 4 + simm * 4)
    if op == 0x0F:
        return 'lui %s, 0x%x' % (R[rt], imm)
    if op in _IMM:
        v = imm if op in (0x0C, 0x0D, 0x0E) else simm
        return '%s %s, %s, %s' % (_IMM[op], R[rt], R[rs], hex(v) if op in (0x0C, 0x0D, 0x0E) else v)
    if op in _MEM:
        reg = ('f%d' % rt) if op in (0x31, 0x39) else ('vf%d' % rt) if op in (0x36, 0x3E) else R[rt]
        return '%s %s, %d(%s)' % (_MEM[op], reg, simm, R[rs])
    if op == 0x10:
        return 'cop0 0x%08x' % w if w != 0x42000038 else 'ei'
    if op == 0x11:
        return 'cop1 0x%08x' % w
    if op == 0x12:
        return 'cop2 0x%08x' % w
    if op == 0x1C:
        return 'mmi 0x%08x' % w
    return '.word 0x%08x' % w


def listing(core, start, count):
    lines = []
    for k in range(count):
        a = start + 4 * k
        w = core.word(a)
        if w is None:
            break
        lines.append('%08x: %08x  %s' % (a, w, disasm(w, a)))
    return lines
