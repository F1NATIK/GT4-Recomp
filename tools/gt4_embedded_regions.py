"""Find non-EE-code content inside GT4 CORE's .text/.data and the VU microprograms
(2026-10-03).

GT3 lessons: CORE.GT3 carried copied kernel code in .data (5 Ghidra functions
excluded at their storage addresses), and data decoded as instructions broke
translation. This script lists, with the evidence used:

  * constructor/destructor tables, .reginfo copy, .eh_frame (parsed records)
  * embedded ELF images (IRX modules) - IOP code, never EE
  * kernel-patch code copied into kernel memory by the SDK (found from the copy
    calls: jal with a0 = kernel/low address, a1 = .data source, a2 = size)
  * EE code that runs from .data (j/jal targets in .data, Ghidra functions there)
  * VU microprograms: found from the MPG upload routine (the only code that
    builds VIF MPG codes, lui 0x4a00), its callers' constant arguments, the VIF
    channel table, and any in-place decoder run on the source buffer first.
    Decoded images are written for vu1_translate.py (padded to 4/16 KiB).
  * Ghidra coverage gaps in .text, with an opcode-plausibility count.

Outputs (in --out): no-translate-regions.csv, vu-microprograms.csv,
ee-code-in-data.csv, text-gaps.csv, vu/*.bin, embedded-regions-evidence.txt

Usage: python tools/gt4_embedded_regions.py [--elf ...] [--ghidra ...] [--out DIR]
VU validation imports recomp/gt3-tools/vu1_decode.py (read only).
"""
import argparse
import csv
import hashlib
import os
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from gt4_core_common import (DEFAULT_ELF, DEFAULT_GHIDRA, DEFAULT_OUT, JR_RA, ROOT, Core, FuncMap, const_args,
                             disasm, fields, has_delay_slot, imm_text_refs, jal_sites, jump_target, listing,
                             plausible_ee, recomp_boundary)
import gt4_ctor_tables

sys.dont_write_bytecode = True  # do not leave __pycache__ in recomp/gt3-tools
sys.path.insert(0, os.path.join(ROOT, 'recomp', 'gt3-tools'))
try:
    from vu1_decode import decode_pair, fmt_pair, lower_usage, upper_usage  # noqa: E402
except ImportError:  # pragma: no cover
    lower_usage = upper_usage = fmt_pair = decode_pair = None

DMA_CHANNEL_NAMES = {0x10008000: 'VIF0', 0x10009000: 'VIF1', 0x1000A000: 'GIF', 0x1000B000: 'IPU_FROM',
                     0x1000B400: 'IPU_TO', 0x1000C000: 'SIF0', 0x1000C400: 'SIF1', 0x1000C800: 'SIF2',
                     0x1000D000: 'SPR_FROM', 0x1000D400: 'SPR_TO'}


def vu_check(blob):
    """(reserved pair count, E-bit count) using GT3's VU decoder."""
    if upper_usage is None:
        return None, None
    bad = ebits = 0
    for pc in range(0, len(blob) - 7, 8):
        lo, up = struct.unpack_from('<II', blob, pc)
        if upper_usage(up).reserved or (not (up >> 31) and lower_usage(lo).reserved):
            bad += 1
        if up & 0x40000000:
            ebits += 1
    return bad, ebits


def function_words(core, f):
    return [(a, core.word(a)) for a in range(f[0], f[1], 4)]


def parse_eh_frame(core, start):
    """CIE/FDE records (32-bit lengths, absolute pc_begin) until a zero length."""
    a = start
    cies, fdes = 0, []
    limit = core.data[1]
    while a + 8 <= limit:
        ln = core.word(a)
        if ln == 0:
            return a + 4, cies, fdes
        if ln > 0x10000:
            break
        if core.word(a + 4) == 0:
            cies += 1
        else:
            fdes.append((core.word(a + 8), core.word(a + 12), a))
        a += 4 + ln
    return None, cies, fdes


def find_lcg_decoder(core, fm, decoder):
    """Recognise an in-place 'byte ^= next() & 0xff' decoder whose next() is an LCG
    state = state * M + C kept in a .data word. Returns dict or None."""
    f = fm.owner(decoder)
    if not f:
        return None
    words = function_words(core, f)
    has_xor = any((w >> 26) == 0 and (w & 63) == 0x26 for _a, w in words)
    flag = None
    callee = None
    for a, w in words:
        if (w >> 26) == 3:
            callee = jump_target(a, w)
    if not (has_xor and callee):
        return None
    g = fm.owner(callee)
    if not g:
        return None
    gw = function_words(core, g)
    mult = None
    state = None
    inc = None
    regs = {}
    for a, w in gw:
        op, rs, rt, _rd, _sa, fn, imm, simm = fields(w)
        if op == 0x0F:
            regs[rt] = imm << 16
        elif op == 0x0D and rs in regs:
            regs[rt] = regs[rs] | imm
        elif op == 0x09 and rs in regs:
            regs[rt] = (regs[rs] + simm) & 0xFFFFFFFF
            if core.in_data(regs[rt]):
                state = regs[rt]
        elif op == 0 and fn in (0x18, 0x19):
            for r in (rs, rt):
                if r in regs and not core.in_data(regs[r]):
                    mult = regs[r]
        elif op in (0x09, 0x19) and rs == rt and mult is not None and inc is None:
            inc = simm  # first addiu/daddiu r,r,imm after the multiply
        elif op in (0x20, 0x21, 0x23, 0x24, 0x25, 0x27, 0x37):
            regs.pop(rt, None)
    # flag byte: lb/lbu/sb via lui+offset inside the decoder
    hi = {}
    for a, w in words:
        op, rs, rt, _rd, _sa, _fn, imm, simm = fields(w)
        if op == 0x0F:
            hi[rt] = imm << 16
        elif op in (0x20, 0x24, 0x28) and rs in hi:
            flag = (hi[rs] + simm) & 0xFFFFFFFF
    if mult is None or state is None:
        return None
    return {'decoder': f[0], 'keystream': g[0], 'multiplier': mult, 'increment': inc if inc is not None else 1,
            'state_addr': state, 'state_init': core.word(state), 'done_flag': flag,
            'done_flag_init': core.bytes(flag, 1)[0] if flag else None}


def lcg_decode(blob, init, mult, inc):
    out = bytearray(blob)
    s = init
    for i in range(len(out)):
        s = (s * mult + inc) & 0xFFFFFFFF
        out[i] ^= s & 0xFF
    return bytes(out)


def dma_channel_table(core, fm, uploader):
    """Indexed word table (lui/addu/lw) in the uploader's callees whose entries are
    DMA channel register bases."""
    f = fm.owner(uploader)
    callees = {jump_target(a, w) for a, w in function_words(core, f) if (w >> 26) in (2, 3)}
    for c in sorted(callees):
        g = fm.owner(c)
        if not g:
            continue
        gw = function_words(core, g)
        for i, (a, w) in enumerate(gw):
            op, rs, rt, _rd, _sa, _fn, imm, _simm = fields(w)
            if op != 0x0F:
                continue
            for b, x in gw[i + 1:i + 6]:
                xo, xrs, xrt, _xrd, _xsa, _xfn, _ximm, xsimm = fields(x)
                if xo == 0x23 and xrs == rt:
                    t = ((imm << 16) + xsimm) & 0xFFFFFFFF
                    vals = [core.word(t + 4 * k) for k in range(4)]
                    if vals[0] in DMA_CHANNEL_NAMES:
                        return t, vals, g[0], b
    return None


def compute(core, fm, out_dir=None, write=True):
    ev = []
    regions = []  # dicts

    def region(start, end, section, kind, translate, evidence):
        regions.append({'Start': '0x%06x' % start, 'End': '0x%06x' % end, 'Size': '0x%x' % (end - start),
                        'Section': section, 'Kind': kind, 'TranslateAsEE': translate, 'Evidence': evidence})

    # 1. ctor/dtor tables and .reginfo copy
    lists = gt4_ctor_tables.find_lists(core)
    if lists:
        lo = min(l['head'] for l in lists)
        hi = max(l['end'] for l in lists) + 4
        start = lo - 4 if core.word(lo - 4) == 0 and lo - 4 >= core.data[0] else lo
        region(start, hi, '.data', 'ctor-dtor-tables', 'no',
               '__CTOR_LIST__ 0x%x (%d) and __DTOR_LIST__ 0x%x (%d); see gt4_ctor_tables.py' % (
                   lists[0]['head'], len(lists[0]['entries']), lists[-1]['head'], len(lists[-1]['entries'])))
        if '.reginfo' in core.sections:
            ri = core.sections['.reginfo']
            rbytes = core.raw[ri[2]:ri[2] + 0x18]
            if core.bytes(hi, 0x18) == rbytes:
                region(hi, hi + 0x18, '.data', 'reginfo-copy', 'no',
                       '0x18 bytes equal the ELF .reginfo section (gp 0x%x)' % struct.unpack_from('<I', rbytes, 0x14)[0])

    # 2. .eh_frame from __register_frame_info's first argument in the ctor walker
    ctor_ref = imm_text_refs(core, lists[0]['head'], lists[0]['head'] + 4).get(lists[0]['head'], []) if lists else []
    eh = None
    if ctor_ref:
        walker = fm.owner(ctor_ref[0])
        for a, w in function_words(core, walker):
            if (w >> 26) == 3:
                args = const_args(core, a)
                if args[4] and core.in_data(args[4]):
                    end, cies, fdes = parse_eh_frame(core, args[4])
                    if end:
                        eh = (args[4], end, cies, fdes, a)
                        break
    if eh:
        fde_known = sum(1 for p, _r, _a in eh[3] if fm.is_start(p))
        region(eh[0], eh[1], '.data', 'eh_frame', 'no',
               'first argument of jal at 0x%x in the ctor walker; %d CIE + %d FDE records, zero terminator; '
               'FDE pc_begin values that are Ghidra starts: %d/%d' % (eh[4], eh[2], len(eh[3]), fde_known, len(eh[3])))
        ev.append('eh_frame 0x%x..0x%x: %d CIEs, %d FDEs, pc range 0x%x..0x%x' % (
            eh[0], eh[1], eh[2], len(eh[3]), min(p for p, _r, _a in eh[3]), max(p + r for p, r, _a in eh[3])))

    # 3. embedded ELF images
    for sec, (lo, hi) in (('.text', core.text), ('.data', core.data)):
        o0 = core.offset(lo)
        blob = core.raw[o0:o0 + hi - lo]
        i = blob.find(b'\x7fELF')
        while i >= 0:
            a = lo + i
            hdr = core.bytes(a, 0x34)
            e_type, e_machine = struct.unpack_from('<HH', hdr, 16)
            phoff, shoff = struct.unpack_from('<II', hdr, 0x1c)
            shentsize, shnum, shstrndx = struct.unpack_from('<HHH', hdr, 0x2e)
            img = core.bytes(a, min(hi - a, 0x100000))
            end = shoff + shnum * shentsize
            secs = []
            for k in range(shnum):
                s = struct.unpack_from('<10I', img, shoff + k * shentsize)
                if s[1] != 8:
                    end = max(end, s[4] + s[5])
                secs.append(s)
            name = ''
            ver = 0
            for s in secs:
                if s[1] == 0x70000080:  # .iopmod
                    ver = struct.unpack_from('<H', img, s[4] + 0x18)[0]
                    nm = img[s[4] + 0x1a:]
                    name = nm[:nm.index(b'\0')].decode('latin1')
            sha = hashlib.sha256(img[:end]).hexdigest()
            refs = imm_text_refs(core, a, a + end + 8)
            region(a, a + end, sec, 'embedded-IRX' if e_type == 0xFF80 else 'embedded-ELF', 'no (IOP code)',
                   'ELF type 0x%x machine %d, module "%s" version 0x%x, %d bytes, sha256 %s; address built at %s' % (
                       e_type, e_machine, name, ver, end, sha[:16],
                       ', '.join('0x%x->0x%x' % (s, v) for v, ss in sorted(refs.items()) for s in ss)))
            ev.append('embedded ELF at 0x%x: type 0x%x module %s ver 0x%x size 0x%x sha256 %s' % (
                a, e_type, name, ver, end, sha))
            i = blob.find(b'\x7fELF', i + 4)

    # 4. kernel copies: jal(a0=kernel/low dest, a1=.data source, a2=size)
    kernel = []
    for target, sites in sorted(jal_sites(core).items()):
        for s in sites:
            args = const_args(core, s)
            dst, src, size = args[4], args[5], args[6]
            if src is None or dst is None or size is None or not core.in_data(src):
                continue
            if not (dst >= 0x80000000 or 0x10000 <= dst < 0x00100000) or not (0 < size <= 0x10000):
                continue
            kernel.append((src, size, dst, s, target))
    for src, size, dst, s, target in sorted(kernel):
        jr = sum(1 for a in range(src, src + size, 4) if core.word(a) == JR_RA)
        region(src, src + size, '.data', 'kernel-patch-code', 'no (copied to 0x%08x)' % dst,
               'jal 0x%x at 0x%x copies 0x%x bytes to 0x%08x; %d jr $ra inside' % (target, s, size, dst, jr))
        ev.append('kernel copy: 0x%x..0x%x -> 0x%08x (jal 0x%x at 0x%x), jr ra x%d' % (
            src, src + size, dst, target, s, jr))

    # 5. EE code executed from .data
    jumps_into_data = {}
    t0, words = core.text_words()
    for i, w in enumerate(words):
        if (w >> 26) in (2, 3):
            t = jump_target(t0 + 4 * i, w)
            if core.in_data(t):
                jumps_into_data.setdefault(t, []).append(t0 + 4 * i)
    ghidra_data_fns = [r for r in fm.rows if core.in_data(r[0])]
    ee_rows = []
    covered = []
    for s, e, n in ghidra_data_fns:
        callers = jumps_into_data.get(s, [])
        ee_rows.append({'Start': '0x%06x' % s, 'End': '0x%06x' % e, 'Size': e - s, 'GhidraName': n,
                        'DirectCallers': ' '.join('0x%x' % c for c in callers),
                        'CallerFunctions': ' '.join(sorted({(fm.owner(c) or (0, 0, '?'))[2] for c in callers}))})
        covered.append((s, e))
    for t, callers in sorted(jumps_into_data.items()):
        if not any(s <= t < e for s, e in covered):
            ee_rows.append({'Start': '0x%06x' % t, 'End': '', 'Size': '', 'GhidraName': '(none)',
                            'DirectCallers': ' '.join('0x%x' % c for c in callers),
                            'CallerFunctions': ' '.join(sorted({(fm.owner(c) or (0, 0, '?'))[2] for c in callers}))})
    if covered:
        lo = min(s for s, _e in covered)
        hi = max(e for _s, e in covered)
        before = core.bytes(lo - 0x100, 0x100) or b''
        k = before.rfind(b'$Header')
        strings = [before[k:before.index(b'\0', k)].decode('latin1')] if k >= 0 else []
        region(lo, hi, '.data', 'EE-code-in-data', 'YES - called directly from .text, not translated today',
               '%d Ghidra functions; %d jal/j sites in .text target them; source tag %s' % (
                   len(covered), sum(len(jumps_into_data.get(s, [])) for s, _e in covered),
                   (strings[0][:70] if strings else '?')))

    # 6. VU microprograms via the MPG upload routine
    vu_rows = []
    mpg_builders = set()
    for i, w in enumerate(words):
        if (w >> 26) == 0x0F and (w & 0xFFFF) in (0x4A00, 0xCA00):
            f = fm.owner(t0 + 4 * i)
            if f:
                mpg_builders.add(f[0])
    ev.append('functions building VIF MPG codes (lui 0x4a00): %s' % [hex(x) for x in sorted(mpg_builders)])
    jals = jal_sites(core)
    vu_dir = os.path.join(out_dir, 'vu') if out_dir else None
    for b in sorted(mpg_builders):
        chan = dma_channel_table(core, fm, b)
        if chan:
            ev.append('  DMA channel table 0x%x = %s (read at 0x%x in 0x%x)' % (
                chan[0], [hex(v) for v in chan[1]], chan[3], chan[2]))
        for site in jals.get(b, []):
            args = const_args(core, site)
            unit, vuaddr, size, src = args[4], args[5], args[6], args[7]
            if None in (unit, size, src):
                ev.append('  upload at 0x%x: arguments not constant %s' % (site, args))
                continue
            chname = DMA_CHANNEL_NAMES.get(chan[1][unit]) if chan and unit < 4 else None
            raw = core.bytes(src, size)
            caller = fm.owner(site)
            decoder = None
            for a in range(caller[0], site, 4):
                w = core.word(a)
                if (w >> 26) == 3:
                    ca = const_args(core, a)
                    if ca[4] == src and ca[5] == size:
                        decoder = (jump_target(a, w), a)
            info = None
            blob = raw
            if decoder:
                info = find_lcg_decoder(core, fm, decoder[0])
                if info:
                    blob = lcg_decode(raw, info['state_init'], info['multiplier'], info['increment'])
            bad_raw, e_raw = vu_check(raw)
            bad, ebits = vu_check(blob)
            unitname = {'VIF0': 'VU0', 'VIF1': 'VU1'}.get(chname, 'unit%d' % unit)
            fname = ''
            if vu_dir and write and bad == 0:
                os.makedirs(vu_dir, exist_ok=True)
                fname = 'gt4_%s_0x%06x_%s.bin' % (unitname.lower(), src, 'decoded' if info else 'raw')
                with open(os.path.join(vu_dir, fname), 'wb') as f:
                    f.write(blob)
                pad = 0x4000 if unitname == 'VU1' else 0x1000
                vuaddr_b = (vuaddr or 0) * 8
                if vuaddr_b + len(blob) <= pad:
                    img = bytearray(pad)
                    img[vuaddr_b:vuaddr_b + len(blob)] = blob
                    with open(os.path.join(vu_dir, 'gt4_%s_code_image.bin' % unitname.lower()), 'wb') as f:
                        f.write(img)
                    if fmt_pair is not None:
                        with open(os.path.join(vu_dir, 'gt4_%s_listing.txt' % unitname.lower()), 'w') as f:
                            f.write('# %s micro memory image from CORE 0x%x (0x%x bytes%s), vu1_decode.py format%s' % (
                                unitname, src, size, ', LCG-decoded' if info else '', os.linesep[-1]))
                            for pc in range(vuaddr_b, vuaddr_b + len(blob), 8):
                                f.write(fmt_pair(decode_pair(bytes(img), pc)) + chr(10))
            vu_rows.append({
                'Unit': unitname, 'DmaChannel': chname or '?', 'VuMicroAddr': '0x%x' % ((vuaddr or 0) * 8),
                'Source': '0x%06x' % src, 'Size': '0x%x' % size, 'UploadSite': '0x%x' % site,
                'Uploader': '0x%x' % b, 'Caller': caller[2],
                'InPlaceDecoder': ('0x%x (called at 0x%x): LCG state@0x%x init 0x%08x, x*0x%08x+%d, byte ^= state&0xff; '
                                   'done flag byte @0x%x = %s' % (
                                       info['decoder'], decoder[1], info['state_addr'], info['state_init'],
                                       info['multiplier'], info['increment'], info['done_flag'] or 0,
                                       info['done_flag_init'])) if info else (
                    'call 0x%x at 0x%x (not recognised)' % decoder if decoder else 'none'),
                'ReservedPairsRaw': bad_raw, 'ReservedPairsDecoded': bad, 'EBits': ebits,
                'DecodedSha256': hashlib.sha256(blob).hexdigest(), 'File': fname})
            kind = 'VU%s-microcode' % unitname[-1] if unitname.startswith('VU') else 'vu-microcode'
            region(src, src + size, '.data', kind + (' (obfuscated)' if info else ''), 'no (VU code)',
                   'uploaded by 0x%x at 0x%x via %s MPG to micro address 0x%x; reserved pairs %s raw / %s decoded; '
                   '%s E-bits' % (b, site, chname, (vuaddr or 0) * 8, bad_raw, bad, ebits))

    # 7. jr $ra clusters in .data not explained above
    d0, dwords = core.data_words()
    jrs = [d0 + 4 * i for i, w in enumerate(dwords) if w == JR_RA]
    spans = [(int(r['Start'], 16), int(r['End'], 16)) for r in regions]
    unexplained = [a for a in jrs if not any(s <= a < e for s, e in spans)]
    ev.append('jr $ra words in .data: %d; outside the regions above: %d %s' % (
        len(jrs), len(unexplained), [hex(a) for a in unexplained[:20]]))
    # text scans
    o0 = core.offset(core.text[0])
    tblob = core.raw[o0:o0 + core.text[1] - core.text[0]]
    lnop = sum(1 for k in range(0, len(tblob) - 7, 8)
               if struct.unpack_from('<I', tblob, k)[0] in (0x8000033C, 0x80000035))
    ev.append('.text: VU lower-NOP words (0x8000033c/0x80000035) at 8-aligned offsets: %d; ELF magic: %d' % (
        lnop, tblob.count(b'\x7fELF')))

    # 8. Ghidra gaps in .text
    gaps = []
    prev = core.text[0]
    for s, e, _n in fm.rows:
        if s >= core.text[1]:
            break
        if s > prev:
            gaps.append((prev, s))
        prev = max(prev, e)
    if prev < core.text[1]:
        gaps.append((prev, core.text[1]))
    gap_rows = []
    for g0, g1 in gaps:
        ws = [core.word(a) for a in range(g0, g1, 4)]
        nz = [w for w in ws if w]
        if not nz:
            continue
        first = next(a for a in range(g0, g1, 4) if core.word(a))
        bad = sum(1 for w in nz if not plausible_ee(w))
        gap_rows.append({'Start': '0x%06x' % g0, 'End': '0x%06x' % g1, 'Size': g1 - g0, 'NonZeroWords': len(nz),
                         'ImplausibleWords': bad, 'JrRa': sum(1 for w in ws if w == JR_RA),
                         'FirstNonZero': '0x%06x' % first, 'FirstInsn': disasm(core.word(first), first),
                         'FirstIsRecompBoundary': int(recomp_boundary(core, first))})

    if write and out_dir:
        os.makedirs(out_dir, exist_ok=True)
        regions.sort(key=lambda r: int(r['Start'], 16))
        for name, rows in (('no-translate-regions.csv', regions), ('vu-microprograms.csv', vu_rows),
                           ('ee-code-in-data.csv', ee_rows), ('text-gaps.csv', gap_rows)):
            if not rows:
                continue
            with open(os.path.join(out_dir, name), 'w', newline='') as f:
                wr = csv.DictWriter(f, fieldnames=list(rows[0].keys()))
                wr.writeheader()
                wr.writerows(rows)
        with open(os.path.join(out_dir, 'embedded-regions-evidence.txt'), 'w') as f:
            f.write('CORE.elf sha256 %s\nGhidra map sha256 %s\n\n' % (core.sha256, fm.sha256))
            f.write('\n'.join(ev) + '\n')
            for r in vu_rows:
                f.write('\nupload %s:\n' % r['UploadSite'])
                f.write('\n'.join(listing(core, int(r['UploadSite'], 16) - 0x24, 11)) + '\n')
    return {'regions': regions, 'vu': vu_rows, 'ee_in_data': ee_rows, 'gaps': gap_rows, 'evidence': ev,
            'eh_frame': eh}


def main(argv):
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('--elf', default=DEFAULT_ELF)
    ap.add_argument('--ghidra', default=DEFAULT_GHIDRA)
    ap.add_argument('--out', default=DEFAULT_OUT)
    args = ap.parse_args(argv)
    core = Core(args.elf)
    fm = FuncMap(args.ghidra)
    res = compute(core, fm, args.out)
    for r in res['regions']:
        print('%s..%s %-8s %-28s %s' % (r['Start'], r['End'], r['Section'], r['Kind'], r['TranslateAsEE']))
    for r in res['vu']:
        print('%s %s src %s size %s reserved raw/decoded %s/%s E %s %s' % (
            r['Unit'], r['DmaChannel'], r['Source'], r['Size'], r['ReservedPairsRaw'], r['ReservedPairsDecoded'],
            r['EBits'], r['File']))
    print('EE functions in .data: %d; .text gaps with non-zero words: %d (%d with implausible opcodes)' % (
        len(res['ee_in_data']), len(res['gaps']), sum(1 for g in res['gaps'] if g['ImplausibleWords'])))
    for line in res['evidence']:
        print(line)


if __name__ == '__main__':
    main(sys.argv[1:])
