"""Find GT4 CORE code addresses that are reached without a direct call Ghidra
already knows, and write the recompiler's extra_entry_points_file (2026-10-03).

GT3 lesson (gt3-analysis/CORE-data-referenced-entries.md): vtable-only setters
and callbacks that Ghidra did not mark as functions were merged into the
preceding function or never compiled; calls to them were lost at run time. The
fix was ps2_recomp's [general] extra_entry_points_file (3,830 addresses).

Sources scanned here (each target is tagged with every source that names it):
  data:vtable    .data words in g++ 2.95 vtables (8-byte {delta,index | pfn} rows)
  data:ptrtable  other runs of >= 2 .text pointers in .data
  data:stray     a single .text pointer in .data
  data:ctor/dtor constructor/destructor table slots (gt4_ctor_tables.py)
  jumptable      switch tables found from code (lui/addiu base, addu, lw, jr)
  calltable      the same pattern ending in jalr (function-pointer tables)
  imm            lui+addiu/ori pairs in .text building a .text address
  jal            direct call targets that are not Ghidra function starts
  jtail          j targets outside the jumping function that are not starts
  ehframe        .eh_frame FDE pc_begin values (compiler-emitted function starts)
Words inside regions that are not EE data (embedded code, IRX, VU microcode;
gt4_embedded_regions.py) are not used as pointer sources.

Outputs (in --out, default analysis/recomp-prep):
  core-extra-entry-points.txt  GT3 format: "0xADDR # sources"; '?' marks
                               low-confidence entries (stray value, no boundary)
  data-code-refs.csv           every referenced target with status and sources
  pointer-tables.csv           grouped .data pointer runs (vtables etc.)
  jump-tables.csv              code-found tables + predicted ps2_recomp outcome
  jump-tables-config.toml      [[jump_tables.table]] for tables ps2_recomp finds
                               the base of but not the bound (config can fix)
  extra-functions.csv          uncovered starts the recompiler will NOT create
                               (its boundary rule fails) - Ghidra CSV format
  entry-points-summary.json    counts

Usage: python tools/gt4_data_entry_points.py [--elf ...] [--ghidra ...] [--out DIR]
"""
import argparse
import bisect
import collections
import csv
import json
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from gt4_core_common import (DEFAULT_ELF, DEFAULT_GHIDRA, DEFAULT_OUT, JR_RA, Core, FuncMap, disasm, fields,
                             has_delay_slot, imm_text_refs, is_stack_prologue, jal_sites, jump_target,
                             recomp_boundary)
import gt4_ctor_tables
import gt4_embedded_regions

NOT_POINTER_SOURCES = ('VU', 'embedded-IRX', 'embedded-ELF', 'kernel-patch-code', 'EE-code-in-data',
                       'reginfo-copy', 'eh_frame', 'ctor-dtor-tables')


# ------------------------------------------------------------------ jump tables
def symbolic_tables(core, fm):
    """Forward evaluation over the 40 instructions before each jr/jalr (not $ra)."""
    t0, words = core.text_words()
    found = []
    for i, w in enumerate(words):
        op, rs, _rt, rd, _sa, fn, _imm, _simm = fields(w)
        if not (op == 0 and fn in (8, 9) and rs != 31):
            continue
        jr = t0 + 4 * i
        owner = fm.owner(jr)
        lo = max(t0, jr - 160, owner[0] if owner else t0)
        st = {0: ('c', 0)}
        bound = None
        for a in range(lo, jr, 4):
            x = core.word(a)
            xo, xrs, xrt, xrd, xsa, xfn, ximm, xsimm = fields(x)
            src = st.get(xrs)
            if xo == 0x0F:
                st[xrt] = ('c', ximm << 16)
            elif xo in (0x09, 0x19):
                if src and src[0] == 'c':
                    st[xrt] = ('c', (src[1] + xsimm) & 0xFFFFFFFF)
                elif src and src[0] == 't':
                    st[xrt] = ('t', (src[1] + xsimm) & 0xFFFFFFFF)
                else:
                    st.pop(xrt, None)
            elif xo == 0x0D:
                st[xrt] = ('c', src[1] | ximm) if src and src[0] == 'c' else None
            elif xo in (0x0A, 0x0B):
                bound = ximm if xo == 0x0B else xsimm
                st.pop(xrt, None)
            elif xo == 0 and xfn in (0x21, 0x2D, 0x20):
                a1, a2 = st.get(xrs), st.get(xrt)
                if xrs == 0 or xrt == 0:
                    st[xrd] = a2 if xrs == 0 else a1
                elif a1 and a2 and a1[0] == 'c' and a2[0] == 'c':
                    st[xrd] = ('c', (a1[1] + a2[1]) & 0xFFFFFFFF)
                elif bool(a1 and a1[0] == 'c') != bool(a2 and a2[0] == 'c'):
                    c = a1 if a1 and a1[0] == 'c' else a2
                    st[xrd] = ('t', c[1])
                else:
                    st.pop(xrd, None)
            elif xo in (0x23, 0x27):
                if src and src[0] in ('c', 't'):
                    st[xrt] = ('L', (src[1] + xsimm) & 0xFFFFFFFF, src[0])
                else:
                    st.pop(xrt, None)
            elif xo == 3 or (xo == 0 and xfn == 9):
                st = {0: ('c', 0)}
            elif xo == 0:
                if xfn not in (0x08, 0x0C, 0x0D, 0x0F, 0x18, 0x19, 0x1A, 0x1B, 0x11, 0x13):
                    st.pop(xrd, None)
            elif xo not in (0x28, 0x29, 0x2B, 0x3F, 0x1F, 0x39, 0x3E, 0x04, 0x05, 0x06, 0x07, 0x14, 0x15,
                            0x16, 0x17, 0x01, 0x02, 0x10, 0x11, 0x12, 0x1C, 0x2F, 0x33):
                st.pop(xrt, None)
            st[0] = ('c', 0)
        v = st.get(rs)
        if not v or v[0] != 'L' or v[2] != 't':
            continue
        table = v[1]
        if not (core.in_data(table) or core.in_text(table)):
            continue
        n = bound if bound and 0 < bound <= 4096 else None
        entries = []
        limit = n if n else 512
        for k in range(limit):
            e = core.word(table + 4 * k)
            if e is None or not core.in_text(e) or e % 4:
                if n:
                    entries = None
                break
            if not n and owner and not (owner[0] - 0x10000 <= e < owner[1] + 0x10000):
                break
            entries.append(e)
        if not entries:
            continue
        found.append({'site': jr, 'kind': 'jumptable' if fn == 8 else 'calltable', 'reg': rs, 'table': table,
                      'bound': n, 'entries': entries, 'owner': owner})
    return found


def recomp_predict(core, f, jr):
    """Mirror of ControlFlowAnalyzer's jump-table recovery (control_flow_analyzer.cpp):
    returns (table_address or None, num_cases, targets inside f)."""
    if not f:
        return None, 0, []
    start, end = f[0], f[1]
    idx = lambda a: (a - start) // 4
    w = core.word(jr)
    jr_reg = (w >> 21) & 31
    ji = idx(jr)
    lw_i = None
    for i in range(ji - 1, max(-1, ji - 21), -1):
        op, rs, rt, _rd, _sa, _fn, _imm, simm = fields(core.word(start + 4 * i))
        if op in (0x23, 0x27) and rt == jr_reg:
            lw_i, base_reg, lw_off = i, rs, simm
            break
    if lw_i is None:
        return None, 0, []
    addu_i = None
    for i in range(lw_i - 1, max(-1, lw_i - 11), -1):
        op, rs, rt, rd, _sa, fn, _imm, _simm = fields(core.word(start + 4 * i))
        if op == 0 and fn == 0x21 and rd == base_reg:
            addu_i, tb, ix = i, rs, rt
            break
    if addu_i is None:
        return None, 0, []
    table = None
    for i in range(addu_i - 1, max(-1, addu_i - 21), -1):
        op, _rs, rt, _rd, _sa, _fn, imm, _simm = fields(core.word(start + 4 * i))
        if op == 0x0F and rt in (tb, ix):
            high = imm << 16
            low = 0
            for j in range(i + 1, addu_i):
                lo_op, lrs, lrt, _lrd, _lsa, _lfn, limm, lsimm = fields(core.word(start + 4 * j))
                if lrs == rt and lrt == rt:
                    if lo_op == 0x09:
                        low = lsimm & 0xFFFFFFFF
                    elif lo_op == 0x0D:
                        low = limm
            table = (high + low + lw_off) & 0xFFFFFFFF
            break
    if table is None:
        return None, 0, []
    unshifted = 0
    for i in range(addu_i - 1, max(-1, addu_i - 11), -1):
        op, _rs, rt, rd, _sa, fn, _imm, _simm = fields(core.word(start + 4 * i))
        if op == 0 and fn == 0 and rd in (tb, ix):
            unshifted = rt
            break
    num = 0
    if unshifted:
        for i in range(addu_i - 1, max(-1, addu_i - 31), -1):
            op, rs, _rt, _rd, _sa, _fn, imm, _simm = fields(core.word(start + 4 * i))
            if op in (0x0A, 0x0B) and rs == unshifted:
                num = imm
                break
    targets = []
    if 0 < num <= 1000:
        for k in range(num):
            e = core.word(table + 4 * k)
            if e is None:
                return table, num, []
            if start <= e < end:
                targets.append(e)
    return table, num, targets



# ------------------------------------------------------------------ helpers
STRONG = ('data:vtable', 'data:ptrtable', 'data:ctor', 'data:dtor', 'jumptable', 'calltable', 'imm', 'jal',
          'jtail', 'ehframe')


def is_terminator(w):
    """Instruction after whose delay slot a function may end."""
    op, rs, rt, _rd, _sa, fn, _imm, _simm = fields(w)
    return ((op == 0 and fn == 8) or op == 2 or (op == 4 and rs == 0 and rt == 0) or
            (op == 1 and rs == 0 and rt == 1) or op == 3)


def start_reason(core, t):
    """Why an uncovered address may start a function that the recompiler's boundary
    rule (jr $ra / j, delay slot, at most 3 zero words) does not accept."""
    a = t - 4
    pad = 0
    while a >= core.text[0] and core.word(a) == 0 and pad < 4096:
        a -= 4
        pad += 1
    w = core.word(a)
    if w == 0x42000018:
        return 'after-eret'
    prev = core.word(a - 4)
    if prev is not None and is_terminator(prev):
        op = prev >> 26
        kind = ('jr-ra' if prev == JR_RA else 'jr-reg' if op == 0 else 'j' if op == 2 else
                'jal-noreturn' if op == 3 else 'b')
        return 'after-%s%s' % (kind, '+pad%d' % pad if pad else '')
    if pad and is_terminator(w):
        return 'after-terminator-in-pad'
    return 'mid-code'


def classify_run(core, fm, run):
    """vtable / ptrtable for a run of .data pointers whose targets mostly look like
    function starts; 'numeric' when they do not (e.g. a 16-bit counter table whose
    words happen to fall in the .text range)."""
    if len(run) == 1:
        return 'stray'
    targets = [core.word(a) for a in run]
    good = sum(1 for t in targets if fm.is_start(t) or recomp_boundary(core, t) or is_stack_prologue(core.word(t)))
    if good * 2 < len(targets):
        return 'numeric'
    diffs = {b - a for a, b in zip(run, run[1:])}
    if diffs == {8}:
        def small(x):
            lo = (x & 0xFFFF) - (0x10000 if x & 0x8000 else 0)
            hi = (x >> 16) - (0x10000 if x & 0x80000000 else 0)
            return abs(lo) < 0x800 and abs(hi) < 0x800
        if all(small(core.word(a - 4)) for a in run):
            return 'vtable'
    return 'ptrtable'


def created_functions(core, starts, uncovered_boundary):
    """ps2_recomp's new functions for uncovered extra entries at a real boundary:
    each ends at the next known start (Ghidra or another created one)."""
    allstarts = sorted(set(starts) | set(uncovered_boundary))
    out = []
    for t in sorted(uncovered_boundary):
        k = bisect.bisect_right(allstarts, t)
        end = allstarts[k] if k < len(allstarts) else core.text[1]
        out.append((t, min(end, core.text[1]), 'sub_%08X' % t))
    return out


# ------------------------------------------------------------------ main scan
def main(argv):
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('--elf', default=DEFAULT_ELF)
    ap.add_argument('--ghidra', default=DEFAULT_GHIDRA)
    ap.add_argument('--out', default=DEFAULT_OUT)
    args = ap.parse_args(argv)
    core = Core(args.elf)
    fm = FuncMap(args.ghidra, clamp=True)
    os.makedirs(args.out, exist_ok=True)
    emb = gt4_embedded_regions.compute(core, fm, args.out, write=False)
    skip = [(int(r['Start'], 16), int(r['End'], 16), r['Kind']) for r in emb['regions']
            if r['Kind'].startswith(NOT_POINTER_SOURCES)]

    sources = collections.defaultdict(set)
    srcaddrs = collections.defaultdict(list)

    def add(target, cls, at):
        sources[target].add(cls)
        if len(srcaddrs[target]) < 6:
            srcaddrs[target].append(at)

    for lst, cls in zip(gt4_ctor_tables.find_lists(core), ('data:ctor', 'data:dtor')):
        for k, t in enumerate(lst['entries']):
            add(t, cls, lst['head'] + 4 + 4 * k)
    fde = emb['eh_frame'][3] if emb['eh_frame'] else []
    for pc, _rng, at in fde:
        if core.in_text(pc):
            add(pc, 'ehframe', at)

    tables = symbolic_tables(core, fm)
    table_words = set()
    for t in tables:
        for k, e in enumerate(t['entries']):
            add(e, t['kind'], t['table'] + 4 * k)
            table_words.add(t['table'] + 4 * k)

    d0, dwords = core.data_words()
    ptr_src = []
    for i, w in enumerate(dwords):
        a = d0 + 4 * i
        if not core.in_text(w) or w % 4 or a in table_words:
            continue
        if any(s <= a < e for s, e, _k in skip):
            continue
        ptr_src.append(a)
    runs = []
    for a in ptr_src:
        if runs and a - runs[-1][-1] <= 8:
            runs[-1].append(a)
        else:
            runs.append([a])
    ptables = []
    for run in runs:
        kind = classify_run(core, fm, run)
        for a in run:
            add(core.word(a), 'data:' + kind, a)
        if len(run) >= 2:
            tg = [core.word(a) for a in run]
            diffs = {b - a for a, b in zip(run, run[1:])}
            ptables.append({'Start': '0x%06x' % run[0], 'End': '0x%06x' % (run[-1] + 4), 'Kind': kind,
                            'Stride': '/'.join(str(d) for d in sorted(diffs)), 'Entries': len(run),
                            'DistinctTargets': len(set(tg)),
                            'TargetsNotGhidraStart': sum(1 for t in set(tg) if not fm.is_start(t))})

    for t, sites in imm_text_refs(core).items():
        for s in sites:
            add(t, 'imm', s)
    t0, words = core.text_words()
    for i, w in enumerate(words):
        op = w >> 26
        if op not in (2, 3):
            continue
        a = t0 + 4 * i
        t = jump_target(a, w)
        if not core.in_text(t) or fm.is_start(t):
            continue
        if op == 3:
            add(t, 'jal', a)
        else:
            own = fm.owner(a)
            if not own or not (own[0] <= t < own[1]):
                add(t, 'jtail', a)

    # ---- per-target status against the clamped Ghidra map
    info = {}
    for t in sorted(sources):
        if not core.in_text(t) or t % 4:
            continue
        cls = sorted(sources[t])
        w = core.word(t)
        prev = core.word(t - 4) if t > core.text[0] else 0
        own = fm.owner(t)
        if fm.is_start(t):
            status = 'ghidra-start'
        elif prev is not None and has_delay_slot(prev):
            status = 'delay-slot'
        elif own:
            status = 'inside'
        else:
            status = 'uncovered'
        boundary = recomp_boundary(core, t)
        prologue = is_stack_prologue(w)
        strong = any(c in STRONG for c in cls)
        info[t] = {'cls': cls, 'status': status, 'own': own, 'boundary': boundary, 'prologue': prologue,
                   'low': not strong and not boundary and not prologue}

    text_starts = [s for s, _e, _n in fm.rows if core.in_text(s)]
    # Uncovered targets named only by jump tables are case labels of a function that is
    # missing from Ghidra; listing them would make ps2_recomp split that function at
    # every case that follows a j / jr. They are left out of the extra entry list.
    listed = lambda v: v['status'] == 'uncovered' and set(v['cls']) - {'jumptable'}
    unc_boundary = [t for t, v in info.items() if listed(v) and v['boundary']]
    unc_other = [t for t, v in info.items() if listed(v) and not v['boundary']]
    created = created_functions(core, text_starts, unc_boundary)

    # ---- uncovered starts the boundary rule rejects: recommend real ones as functions
    created_starts = set(s for s, _e, _n in created)
    known = sorted(set(text_starts) | created_starts)
    xf_rows = []
    recommended = []
    for t in sorted(unc_other):
        v = info[t]
        reason = start_reason(core, t)
        k = bisect.bisect_right(known, t) - 1
        inside_created = k >= 0 and known[k] in created_starts
        entry_cls = set(v['cls']) - {'jumptable'}
        if not entry_cls or v['low']:
            rec = False          # case labels: ps2_recomp labels them inside the parent
        elif v['prologue']:
            rec = True
        elif reason == 'mid-code':
            rec = False
        elif entry_cls == {'imm'} and reason.startswith('after-b'):
            rec = False          # resume address inside a function (setjmp-style), not an entry
        else:
            rec = True
        nxt = known[k + 1] if k + 1 < len(known) else core.text[1]
        end = min(gt4_ctor_tables.estimate_end(core, fm, t), nxt)
        xf_rows.append({'Name': 'FUN_%08x' % t, 'Start': '0x%08X' % t, 'End': '0x%08X' % end, 'Size': end - t,
                        'Classes': ' '.join(v['cls']), 'Reason': reason, 'Prologue': int(v['prologue']),
                        'InsideCreatedFunction': int(inside_created), 'Recommended': int(rec),
                        'FirstInsn': disasm(core.word(t), t)})
        if rec:
            recommended.append((t, end, 'FUN_%08x' % t))
    # Recommended starts also cut the functions the recompiler creates.
    created = created_functions(core, text_starts + [r[0] for r in recommended], unc_boundary)
    created_starts = set(s for s, _e, _n in created)
    eff = FuncMap(args.ghidra, clamp=True, extra_rows=list(recommended) + created)
    # Branch / j targets of translated code that fall outside every function would be
    # dropped by ps2_recomp (queueExternalEntryTarget needs an owner). Typical case:
    # Ghidra ended a switch function before its shared tail, or split a loop body
    # off a thunk. Make each such target a function start (ends at the next start).
    for _round in range(4):
        lost = {}
        for s0, e0, n0 in eff.rows:
            if not core.in_text(s0):
                continue
            for a in range(s0, e0, 4):
                w = core.word(a)
                op = w >> 26
                if op == 2:
                    t = jump_target(a, w)
                elif has_delay_slot(w) and op not in (0, 3):
                    t = a + 4 + fields(w)[7] * 4
                else:
                    continue
                if s0 <= t < e0 or not core.in_text(t) or eff.owner(t):
                    continue
                lost.setdefault(t, []).append(a)
        if not lost:
            break
        allk = sorted(set(eff.starts) | set(lost))
        for t, sites in sorted(lost.items()):
            k = bisect.bisect_right(allk, t)
            end = allk[k] if k < len(allk) else core.text[1]
            recommended.append((t, end, 'FUN_%08x' % t))
            xf_rows.append({'Name': 'FUN_%08x' % t, 'Start': '0x%08X' % t, 'End': '0x%08X' % end,
                            'Size': end - t, 'Classes': 'branch',
                            'Reason': 'branch-target-outside-functions (from %s)' % ' '.join('0x%x' % x for x in sites),
                            'Prologue': int(is_stack_prologue(core.word(t))), 'InsideCreatedFunction': 0,
                            'Recommended': 1, 'FirstInsn': disasm(core.word(t), t)})
        created = created_functions(core, text_starts + [r[0] for r in recommended], unc_boundary)
        created_starts = set(s for s, _e, _n in created)
        eff = FuncMap(args.ghidra, clamp=True, extra_rows=list(recommended) + created)

    # ---- final fate of every referenced target under the effective map
    rows = []
    extra = []
    fate = collections.Counter()
    for t, v in info.items():
        own_e = eff.owner(t)
        if v['status'] in ('ghidra-start', 'delay-slot'):
            f = v['status']
        elif v['status'] == 'uncovered' and not set(v['cls']) - {'jumptable'}:
            f = 'case-label-in-%s' % ('function' if own_e else 'NO-function')
        elif t in created_starts:
            f = 'new-function'
        elif eff.is_start(t):
            f = 'recommended-function'
        elif own_e:
            f = 'label'
        else:
            f = 'ignored-no-owner'
        fate[f] += 1
        rows.append({'Target': '0x%06x' % t, 'Status': v['status'], 'Fate': f,
                     'Owner': v['own'][2] if v['own'] else '',
                     'OwnerRange': '0x%06x..0x%06x' % (v['own'][0], v['own'][1]) if v['own'] else '',
                     'Classes': ' '.join(v['cls']), 'RecompBoundary': int(v['boundary']),
                     'Prologue': int(v['prologue']), 'LowConfidence': int(v['low']),
                     'FirstInsn': disasm(core.word(t), t), 'Sources': ' '.join('0x%x' % s for s in srcaddrs[t])})
        if v['status'] == 'inside' or (v['status'] == 'uncovered' and set(v['cls']) - {'jumptable'}):
            extra.append((t, v, f))

    # ---- jump tables, predicted with the effective map
    jt_rows = []
    cfg = []
    pred = collections.Counter()
    for t in tables:
        f = eff.owner(t['site'])
        rtab, rnum, rtargets = recomp_predict(core, f, t['site'])
        if t['kind'] == 'calltable':
            outcome = 'n/a (jalr call table)'
        elif not f:
            outcome = 'not translated: no owning function'
        elif rtab is None:
            outcome = 'fallback: base not found'
        elif rtab != t['table']:
            outcome = 'fallback: wrong base 0x%x' % rtab
        elif not (0 < rnum <= 1000):
            outcome = 'fallback: bound not found (config table fixes)'
        elif not rtargets:
            outcome = 'fallback: no targets inside function'
        else:
            n_in, n_all = len(set(rtargets)), len(set(t['entries']))
            outcome = 'resolved' + ('' if n_in == n_all else ' (%d of %d targets inside)' % (n_in, n_all))
        pred[outcome.split(' (')[0].split(' 0x')[0]] += 1
        if outcome.startswith('fallback: bound not found'):
            cfg.append((t, f))
        jt_rows.append({'Site': '0x%06x' % t['site'], 'Kind': t['kind'], 'Owner': f[2] if f else '',
                        'OwnerRange': '0x%06x..0x%06x' % (f[0], f[1]) if f else '', 'Table': '0x%06x' % t['table'],
                        'Bound': t['bound'] or '', 'Entries': len(t['entries']),
                        'DistinctTargets': len(set(t['entries'])),
                        'TargetsOutsideOwner': len([e for e in t['entries'] if not (f and f[0] <= e < f[1])]),
                        'RecompPrediction': outcome,
                        'RecompTable': '0x%x' % rtab if rtab is not None else '', 'RecompCases': rnum or ''})
    jr_all = [t0 + 4 * i for i, w in enumerate(words) if (w & 0xFC1FFFFF) == 0x00000008 and ((w >> 21) & 31) != 31]
    found_sites = {t['site'] for t in tables}
    jr_other = []
    for a in jr_all:
        if a in found_sites:
            continue
        f = eff.owner(a)
        jr_other.append({'Site': '0x%06x' % a, 'Kind': 'jr-not-table', 'Owner': f[2] if f else '',
                         'OwnerRange': '0x%06x..0x%06x' % (f[0], f[1]) if f else '', 'Table': '', 'Bound': '',
                         'Entries': '', 'DistinctTargets': '', 'TargetsOutsideOwner': '',
                         'RecompPrediction': ('indirect fallback (every instruction of the function becomes an entry)'
                                              if f else 'not translated: no owning function'),
                         'RecompTable': '', 'RecompCases': ''})

    # ---- .text code still outside every function after ps2_recomp's own processing
    left = []
    prev_end = core.text[0]
    for s0, e0, _n in eff.rows:
        if not core.in_text(s0):
            continue
        if s0 > prev_end:
            nz = [a for a in range(prev_end, s0, 4) if core.word(a)]
            if nz:
                left.append({'Start': '0x%06x' % nz[0], 'End': '0x%06x' % (nz[-1] + 4),
                             'NonZeroWords': len(nz), 'JrRa': sum(1 for a in nz if core.word(a) == JR_RA),
                             'FirstInsn': disasm(core.word(nz[0]), nz[0]),
                             'Note': 'zero-filled before' if nz[0] > prev_end else ''})
        prev_end = max(prev_end, e0)

    # ---- write
    def wcsv(name, rws):
        with open(os.path.join(args.out, name), 'w', newline='') as fh:
            if rws:
                wr = csv.DictWriter(fh, fieldnames=list(rws[0].keys()))
                wr.writeheader()
                wr.writerows(rws)
    wcsv('data-code-refs.csv', rows)
    wcsv('pointer-tables.csv', ptables)
    wcsv('jump-tables.csv', jt_rows + jr_other)
    wcsv('extra-functions.csv', xf_rows)
    wcsv('text-uncovered-after.csv', left)
    wcsv('ghidra-range-clamps.csv', [{'Name': n, 'Start': '0x%06x' % s, 'GhidraEnd': '0x%06x' % e,
                                      'ClampedEnd': '0x%06x' % ne, 'BytesRemoved': e - ne}
                                     for s, e, ne, n in fm.clamped])
    # Function map for ps2_recomp: clamped Ghidra .text rows + recommended starts.
    merged = FuncMap(args.ghidra, clamp=True, extra_rows=recommended)
    merged_rows = [r for r in merged.rows if not core.in_data(r[0])]
    with open(os.path.join(args.out, 'core-functions-gt4.csv'), 'w', newline='') as fh:
        fh.write('Name,Start,End,Size\n')
        for s, e, n in merged_rows:
            fh.write('%s,0x%08X,0x%08X,%d\n' % (n, s, e, e - s))
    # Same rows with auto names (FUN_/sub_/LAB_/DAT_) renamed gt4f_: ps2_recomp's ElfParser
    # treats only non-auto map names as authoritative; auto-named rows that are also jal
    # targets get End = max(map End, next jal target) from its ScanJalTargetsFallback.
    with open(os.path.join(args.out, 'core-functions-gt4-authoritative.csv'), 'w', newline='') as fh:
        fh.write('Name,Start,End,Size\n')
        for s, e, n in merged_rows:
            if n.startswith(('FUN_', 'sub_', 'LAB_', 'DAT_')):
                n = 'gt4f_%08x' % s
            fh.write('%s,0x%08X,0x%08X,%d\n' % (n, s, e, e - s))
    # ps2_recomp's effective ranges for the auto-named file (inferred from elf_parser.cpp)
    jt_sorted = sorted(t for t in jal_sites(core) if core.in_text(t))
    jt_set = set(jt_sorted)
    grown = 0
    grown_bytes = 0
    for s, e, n in merged_rows:
        if s in jt_set and n.startswith(('FUN_', 'sub_', 'LAB_', 'DAT_')):
            k = bisect.bisect_right(jt_sorted, s)
            fe = jt_sorted[k] if k < len(jt_sorted) else core.text[1]
            if fe > e:
                grown += 1
                grown_bytes += fe - e
    with open(os.path.join(args.out, 'jump-tables-config.toml'), 'w') as fh:
        fh.write('# Jump tables whose base ps2_recomp computes itself but whose case count it does not\n'
                 '# find (gt4_data_entry_points.py). ps2_recomp consults [jump_tables] only after it has\n'
                 '# computed the same table address (control_flow_analyzer.cpp).\n')
        if cfg:
            fh.write('[jump_tables]\n')
        for t, f in cfg:
            fh.write('\n[[jump_tables.table]]\n# jr at 0x%x in %s, %s\naddress = "0x%x"\nentries = [\n' % (
                t['site'], f[2] if f else '?', ('bound sltiu %d' % t['bound']) if t['bound'] else
                'no bound in code: entries read while they point near the function', t['table']))
            for k, e in enumerate(t['entries']):
                fh.write('  { index = %d, target = "0x%x" },\n' % (k, e))
            fh.write(']\n')
    with open(os.path.join(args.out, 'core-extra-entry-points.txt'), 'w') as fh:
        fh.write('# GT4 CORE (SCUS-97328) code addresses reached through data, constants, tables or\n'
                 '# calls that are not Ghidra function starts (analysis/core.csv sha256 %s).\n'
                 '# Generated by tools/gt4_data_entry_points.py from game/CORE.elf sha256 %s.\n'
                 '# Format: address # sources [fate]. "?" = low confidence (single stray or numeric\n'
                 '# .data value, no boundary or prologue); harmless as a label.\n'
                 '# Fates (predicted with core-functions-gt4.csv): label = resumable label in its owner;\n'
                 '# new-function = ps2_recomp creates it (uncovered, real boundary);\n'
                 '# recommended-function = start listed in core-functions-gt4.csv.\n'
                 '# Delay-slot addresses and Ghidra starts are omitted.\n' % (fm.sha256, core.sha256))
        for t, v, f in sorted(extra, key=lambda x: x[0]):
            short = [c.replace('data:', '') for c in v['cls']]
            fh.write('0x%06x # %s [%s]%s\n' % (t, ','.join(short), f, ' ?' if v['low'] else ''))
    status_c = collections.Counter(v['status'] for v in info.values())
    cls_c = collections.Counter(c for _t, v, _f in extra for c in v['cls'])
    summary = {
        'core_sha256': core.sha256, 'ghidra_csv': args.ghidra, 'ghidra_sha256': fm.sha256,
        'ghidra_functions': len(fm), 'ghidra_functions_in_data': sum(1 for s, _e, _n in fm.rows if core.in_data(s)),
        'ghidra_ranges_clamped': len(fm.clamped),
        'ghidra_bytes_removed_by_clamp': sum(e - ne for _s, e, ne, _n in fm.clamped),
        'referenced_targets': len(info), 'status': dict(status_c),
        'extra_entry_points': len(extra), 'extra_low_confidence': sum(1 for _t, v, _f in extra if v['low']),
        'extra_by_class': dict(cls_c), 'fate': dict(fate),
        'new_functions_created_by_recompiler': len(created),
        'uncovered_rejected_by_boundary_rule': len(unc_other),
        'recommended_extra_functions': len(recommended),
        'core_functions_gt4_csv_rows': len(merged_rows),
        'auto_named_rows_grown_by_jal_fallback': grown,
        'auto_named_extra_bytes_decoded': grown_bytes,
        'pointer_runs': dict(collections.Counter(p['Kind'] for p in ptables)),
        'pointer_run_entries': {k: sum(p['Entries'] for p in ptables if p['Kind'] == k)
                                for k in sorted(set(p['Kind'] for p in ptables))},
        'stray_pointer_words': sum(1 for r in runs if len(r) == 1),
        'data_pointer_words_used': len(ptr_src),
        'code_tables': dict(collections.Counter(t['kind'] for t in tables)),
        'jr_non_ra_total': len(jr_all), 'jr_not_tables': len(jr_other),
        'jump_table_prediction': dict(pred),
        'jump_tables_config_entries': len(cfg),
        'text_regions_outside_all_functions_after': len(left),
        'text_nonzero_words_outside_all_functions_after': sum(r['NonZeroWords'] for r in left),
        'skipped_source_regions': [(hex(s), hex(e), k) for s, e, k in skip],
    }
    with open(os.path.join(args.out, 'entry-points-summary.json'), 'w') as fh:
        json.dump(summary, fh, indent=1)
    print(json.dumps(summary, indent=1))


if __name__ == '__main__':
    main(sys.argv[1:])
