"""Recover GT4 CORE's C++ constructor / destructor tables and check them against
the Ghidra function map (2026-10-03).

GT3 lesson: Ghidra missed 19 initializers and 1 destructor reached only through
these tables (gt3-analysis/recovered-initializers.csv). This script finds the
tables from the code that walks them instead of trusting a fixed address:

  * __CTOR_LIST__: a .data word 0xffffffff followed by .text pointers and a 0
    terminator, whose address is built by lui/addiu in .text (the libgcc
    __do_global_ctors walker: counts entries, calls them last to first).
  * __DTOR_LIST__: same layout; libgcc's __do_global_dtors keeps a static
    pointer initialised to __DTOR_LIST__+1 in .data and walks forward.
  * the call chain: walker <- __main <- main <- crt0 entry.

Outputs (in --out, default analysis/recomp-prep):
  gt4-ctor-dtor-tables.csv   every slot: kind, index, slot, target, Ghidra status
  recovered-initializers.csv GT3 format (Name,Start,End,Size): table targets that
                             are NOT Ghidra function starts (may be header only)
  ctor-dtor-evidence.txt     disassembly of the walkers and the call chain

Usage: python tools/gt4_ctor_tables.py [--elf game/CORE.elf] [--ghidra analysis/core.csv] [--out DIR]
"""
import argparse
import csv
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from gt4_core_common import (DEFAULT_ELF, DEFAULT_GHIDRA, DEFAULT_OUT, JR_RA, Core, FuncMap, disasm, fields,
                             has_delay_slot, imm_text_refs, is_stack_prologue, jump_target, listing,
                             recomp_boundary)


def find_lists(core):
    """Every .data run: 0xffffffff, >=1 .text pointers, 0."""
    d0, words = core.data_words()
    out = []
    for i, w in enumerate(words[:-1]):
        if w != 0xFFFFFFFF:
            continue
        j = i + 1
        while j < len(words) and core.in_text(words[j]) and words[j] % 4 == 0:
            j += 1
        if j > i + 1 and j < len(words) and words[j] == 0:
            head = d0 + 4 * i
            out.append({'head': head, 'entries': list(words[i + 1:j]), 'end': d0 + 4 * j})
    return out


def text_jumps_to(core, target):
    t0, words = core.text_words()
    sites = []
    for i, w in enumerate(words):
        if (w >> 26) in (2, 3) and jump_target(t0 + 4 * i, w) == target:
            sites.append((t0 + 4 * i, 'jal' if (w >> 26) == 3 else 'j'))
    return sites


def estimate_end(core, fm, start):
    """Function end for a start Ghidra does not know: the jr $ra (+ delay slot) after
    which no forward branch is pending, capped at the next known start."""
    cap = fm.next_start(start) or core.text[1]
    furthest = start
    a = start
    while a < cap:
        w = core.word(a)
        op, rs, rt, _rd, _sa, fn, _imm, simm = fields(w)
        if has_delay_slot(w) and op not in (2, 3) and not (op == 0 and fn in (8, 9)):
            furthest = max(furthest, a + 4 + simm * 4)
        if w == JR_RA and a + 8 > furthest:
            return a + 8
        a += 4
    return cap


def main(argv):
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('--elf', default=DEFAULT_ELF)
    ap.add_argument('--ghidra', default=DEFAULT_GHIDRA)
    ap.add_argument('--out', default=DEFAULT_OUT)
    args = ap.parse_args(argv)
    core = Core(args.elf)
    fm = FuncMap(args.ghidra)
    os.makedirs(args.out, exist_ok=True)
    ev = ['CORE.elf sha256 %s' % core.sha256, 'Ghidra map %s sha256 %s (%d functions)' % (
        args.ghidra, fm.sha256, len(fm)), '']

    lists = find_lists(core)
    data_refs = imm_text_refs(core, core.data[0], core.data[1])
    d0, dwords = core.data_words()
    ctor = dtor = None
    for lst in lists:
        sites = data_refs.get(lst['head'], [])
        cells = [d0 + 4 * i for i, w in enumerate(dwords) if w == lst['head'] + 4]
        lst['code_refs'] = sites
        lst['ptr_cells'] = cells
        ev.append('list head 0x%x: %d entries, terminator 0x%x, lui/addiu refs %s, .data cells holding head+4 %s' % (
            lst['head'], len(lst['entries']), lst['end'], [hex(s) for s in sites], [hex(c) for c in cells]))
        if sites and ctor is None:
            ctor = lst
        elif cells and dtor is None:
            dtor = lst
    if ctor is None:
        raise SystemExit('no constructor list referenced from code')
    ev.append('')

    # Walkers and call chain.
    ctor_walker = fm.owner(ctor['code_refs'][0])
    ev.append('__CTOR_LIST__ = 0x%x (word 0x%08x), walker %s 0x%x..0x%x (refs at %s)' % (
        ctor['head'], core.word(ctor['head']), ctor_walker[2], ctor_walker[0], ctor_walker[1],
        ', '.join(hex(s) for s in ctor['code_refs'])))
    ev += ['  ' + l for l in listing(core, ctor_walker[0], (ctor_walker[1] - ctor_walker[0]) // 4)]
    dtor_walker = None
    if dtor:
        cell = dtor['ptr_cells'][0]
        cell_refs = imm_text_refs(core, cell, cell + 4)
        # lw/sw via lui+offset are not addiu pairs; also accept lui+addiu to the cell.
        sites = cell_refs.get(cell, [])
        if sites:
            dtor_walker = fm.owner(sites[0])
        ev.append('')
        ev.append('__DTOR_LIST__ = 0x%x (word 0x%08x); static pointer cell 0x%x = 0x%x; walker %s' % (
            dtor['head'], core.word(dtor['head']), cell, core.word(cell),
            ('%s 0x%x..0x%x (cell built at %s)' % (dtor_walker[2], dtor_walker[0], dtor_walker[1],
                                                   ', '.join(hex(s) for s in sites))) if dtor_walker else 'not found'))
        if dtor_walker:
            ev += ['  ' + l for l in listing(core, dtor_walker[0], (dtor_walker[1] - dtor_walker[0]) // 4)]
            # atexit registration of the dtor walker inside the ctor walker
            reg = imm_text_refs(core, dtor_walker[0], dtor_walker[0] + 4).get(dtor_walker[0], [])
            ev.append('  dtor walker address built at %s (inside ctor walker: %s)' % (
                [hex(s) for s in reg], any(ctor_walker[0] <= s < ctor_walker[1] for s in reg)))
    ev.append('')
    chain = []
    callers = text_jumps_to(core, ctor_walker[0])
    ev.append('callers of ctor walker 0x%x: %s' % (ctor_walker[0], [(hex(a), k) for a, k in callers]))
    for site, kind in callers:
        f = fm.owner(site)
        if f:
            chain.append(f)
            ev += ['  ' + l for l in listing(core, f[0], (f[1] - f[0]) // 4)]
            up = text_jumps_to(core, f[0])
            ev.append('  callers of %s 0x%x: %s' % (f[2], f[0], [(hex(a), k, (fm.owner(a) or (0, 0, '?'))[2]) for a, k in up]))
            for a, _k in up:
                g = fm.owner(a)
                if g:
                    ev.append('    %s' % '\n    '.join(listing(core, a - 8, 4)))
    ev.append('')
    ev.append('crt0 entry 0x%x: calls in order' % core.entry)
    a = core.entry
    while a < core.entry + 0x400:
        w = core.word(a)
        if (w >> 26) in (2, 3):
            t = jump_target(a, w)
            local = core.entry <= t < core.entry + 0x400
            if not local:
                own = fm.owner(t)
                ev.append('  %08x: %-16s -> %s' % (a, disasm(w, a), own[2] if own else '?'))
            if (w >> 26) == 2 and not local:
                break
        a += 4

    # Per-slot table.
    rows = []
    missing = []
    for kind, lst in (('ctor', ctor), ('dtor', dtor)):
        if not lst:
            continue
        for k, target in enumerate(lst['entries']):
            slot = lst['head'] + 4 + 4 * k
            own = fm.owner(target)
            if fm.is_start(target):
                status = 'ghidra-start'
            elif own:
                status = 'inside:%s' % own[2]
            else:
                status = 'uncovered'
            w = core.word(target)
            rows.append({
                'Kind': kind, 'Index': k, 'Slot': '0x%06x' % slot, 'Target': '0x%06x' % target,
                'Status': status,
                'GhidraName': own[2] if own else '',
                'GhidraEnd': '0x%06x' % own[1] if own else '',
                'FirstInsn': '%08x %s' % (w, disasm(w, target)),
                'Prologue': int(is_stack_prologue(w)),
                'RecompBoundary': int(recomp_boundary(core, target)),
                'CallOrder': (len(lst['entries']) - k) if kind == 'ctor' else k + 1,
            })
            if status != 'ghidra-start':
                end = estimate_end(core, fm, target)
                missing.append(('FUN_%08x' % target, target, end, kind))
    with open(os.path.join(args.out, 'gt4-ctor-dtor-tables.csv'), 'w', newline='') as f:
        wr = csv.DictWriter(f, fieldnames=list(rows[0].keys()))
        wr.writeheader()
        wr.writerows(rows)
    with open(os.path.join(args.out, 'recovered-initializers.csv'), 'w', newline='') as f:
        f.write('Name,Start,End,Size\n')
        for name, s, e, _kind in sorted(set(missing), key=lambda r: r[1]):
            f.write('%s,0x%08X,0x%08X,%d\n' % (name, s, e, e - s))
    with open(os.path.join(args.out, 'ctor-dtor-evidence.txt'), 'w') as f:
        f.write('\n'.join(ev) + '\n')
    nct = len(ctor['entries'])
    ndt = len(dtor['entries']) if dtor else 0
    distinct = len({r['Target'] for r in rows})
    print('ctor list 0x%x: %d entries (end 0x%x); dtor list %s: %d entries; distinct targets %d' % (
        ctor['head'], nct, ctor['end'], hex(dtor['head']) if dtor else '-', ndt, distinct))
    print('not Ghidra starts: %d (written to recovered-initializers.csv)' % len(set(m[1] for m in missing)))
    print('first-instruction prologue: %d/%d; recompiler boundary rule true: %d/%d' % (
        sum(r['Prologue'] for r in rows), len(rows), sum(r['RecompBoundary'] for r in rows), len(rows)))


if __name__ == '__main__':
    main(sys.argv[1:])
