"""Translate a VU1 microcode image to C++ (see ps2xRuntime/include/runtime/ps2_vu1_translated.h).

usage: python vu1_translate.py <code.bin | kicks.bin> <out_dir> [--leaders kicks.bin ...]

Writes <out_dir>/vu1_image_<hash>.cpp and regenerates <out_dir>/vu1_images.cpp
(the registry of every vu1_image_*.cpp in the directory).

Every pair gets a stepping function (exact per-pair semantics, generic branch
handling); every leader (entry points, branch targets, fall-through after a
delay slot, return addresses, recorded entry points) also gets a block
function that runs straight-line code to the next branch/leader/end. The
semantics mirror ps2xRuntime/src/lib/vu/ps2_vu1_{core,upper,lower}.cpp.
"""
import glob
import os
import re
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from vu1_decode import (DEST, FD, FS, FT, IMM11, PIPE_BRANCH, PIPE_EFU, PIPE_FDIV, PIPE_XGKICK, VID, VIS, VIT,
                        decode_pair, fmt_pair, lane, load_code, lower_name, upper_name)

CODE_SIZE = 0x4000
MASK = 0x3FFF
# --vu0: VU0 microprograms (4 KiB code and data, no EFU/XGKICK).
UNIT = 'vu1'
DATA_MASK = 0x3FFF
BC = 'xyzw'


def fnv64(data):
    h = 1469598103934665603
    for b in data:
        h = ((h ^ b) * 1099511628211) & 0xFFFFFFFFFFFFFFFF
    return h


def lanes_of(dest):
    return [k for k in range(4) if dest & lane(k)]


class Unsupported(Exception):
    pass


FMAC_KINDS = ('add', 'sub', 'madd', 'msub', 'mul', 'opmsub', 'opmula')
MAC_READS = ('fmand', 'fmeq', 'fmor')
STATUS_USES = ('fsand', 'fseq', 'fsor', 'fsset')
# Set per image (gen_image): no instruction reads the status flags or sets
# the sticky ones, so a flag result only matters through the MAC flags (and
# its sticky bits, accumulated in Ctx::stickyAcc).
DEAD_FLAGS_OK = False


# ------------------------------------------------------------------ upper ops
def upper_info(u):
    """Returns (kind, operand, target, flags) for the upper instruction.
    kind: add sub madd msub mul max mini opmsub opmula itof ftoi abs clip nop
    operand: ('bc', lane) | 'q' | 'i' | 'vt' | None
    target: ('vf', reg) | 'acc' | None
    product_sum: bool (product sticky flags)"""
    op = u & 0x3F
    fs, ft, fd = FS(u), FT(u), FD(u)
    if op < 0x3C:
        if op <= 0x1B:
            kind = ['add', 'sub', 'madd', 'msub', 'max', 'mini', 'mul'][op >> 2]
            return kind, ('bc', op & 3), ('vf', fd), kind in ('madd', 'msub')
        table = {0x1C: ('mul', 'q'), 0x1D: ('max', 'i'), 0x1E: ('mul', 'i'), 0x1F: ('mini', 'i'),
                 0x20: ('add', 'q'), 0x21: ('madd', 'q'), 0x22: ('add', 'i'), 0x23: ('madd', 'i'),
                 0x24: ('sub', 'q'), 0x25: ('msub', 'q'), 0x26: ('sub', 'i'), 0x27: ('msub', 'i'),
                 0x28: ('add', 'vt'), 0x29: ('madd', 'vt'), 0x2A: ('mul', 'vt'), 0x2B: ('max', 'vt'),
                 0x2C: ('sub', 'vt'), 0x2D: ('msub', 'vt'), 0x2E: ('opmsub', 'vt'), 0x2F: ('mini', 'vt')}
        if op not in table:
            raise Unsupported('upper op %x' % op)
        kind, operand = table[op]
        return kind, operand, ('vf', fd), op in (0x21, 0x23, 0x25, 0x27, 0x29, 0x2D, 0x2E)
    sp = (u & 3) | ((u >> 4) & 0x7C)
    if sp <= 0x0F:
        kind = ['add', 'sub', 'madd', 'msub'][sp >> 2]
        return kind, ('bc', sp & 3), 'acc', kind in ('madd', 'msub')
    if 0x10 <= sp <= 0x13:
        return 'itof', sp - 0x10, ('vf', ft), False
    if 0x14 <= sp <= 0x17:
        return 'ftoi', sp - 0x14, ('vf', ft), False
    if 0x18 <= sp <= 0x1B:
        return 'mul', ('bc', sp & 3), 'acc', False
    table = {0x1C: ('mul', 'q'), 0x1E: ('mul', 'i'), 0x20: ('add', 'q'), 0x21: ('madd', 'q'), 0x22: ('add', 'i'),
             0x23: ('madd', 'i'), 0x24: ('sub', 'q'), 0x25: ('msub', 'q'), 0x26: ('sub', 'i'), 0x27: ('msub', 'i'),
             0x28: ('add', 'vt'), 0x29: ('madd', 'vt'), 0x2A: ('mul', 'vt'), 0x2C: ('sub', 'vt'),
             0x2D: ('msub', 'vt'), 0x2E: ('opmula', 'vt')}
    if sp in table:
        kind, operand = table[sp]
        return kind, operand, 'acc', sp in (0x21, 0x23, 0x25, 0x27, 0x29, 0x2D)
    if sp == 0x1D:
        return 'abs', None, ('vf', ft), False
    if sp == 0x1F:
        return 'clip', None, None, False
    if sp in (0x2F, 0x30):
        return 'nop', None, None, False
    raise Unsupported('upper special %x' % sp)


CROSS_L = [1, 2, 0, 3]
CROSS_R = [2, 0, 1, 3]


SIMD_FMAC = os.environ.get('GT3_VU1T_SIMD', '1') != '0'
# Exact-zero lanes (a product with a zero operand, a sum of opposites, ...)
# stay on the vector fast path (fastMulV/fastAddV/fastSubV/fastMaddZV in
# ps2_vu1_translated.h). GT3_VU1T_ZERO_FAST=0 at generation time: fastV/fastMaddV.
ZERO_FAST = os.environ.get('GT3_VU1T_ZERO_FAST', '1') != '0'
# GT3_VU1T_STICKY=0 at generation time: a dropped flag result (dead_flag_pairs)
# no longer feeds the sticky status bits (Ctx::stickyAcc), so its SIMD FMAC
# computes no flags at all: values stay exact (keepMulV/keepAddV/keepSubV/
# keepMaddV decide, else the scalar fx per lane), but status bits 6-9 can miss
# a bit only such a result would have set. GT3's image never reads the status
# flags (no FSAND/FSEQ/FSOR/FSSET), and the EE does not read VU1's.
KEEP_STICKY = os.environ.get('GT3_VU1T_STICKY', '1') != '0'
# MAX/MINI as one SSE MAXPS/MINPS (GT3_VU1T_SIMD_MINMAX=0 at generation: per lane).
SIMD_MINMAX = os.environ.get('GT3_VU1T_SIMD_MINMAX', '1') != '0'


# Lanes known to be normalised at the current point of a block: results of
# upper FMAC/MAX/MINI/ABS/ITOF operations written earlier in the same block
# (fx leaves no denormal or infinity; the others come from N()-ed operands or
# small integers). N()/NV() is the identity on them, so their reads skip it.
# Blocks start with nothing known; a lower-unit write (LQ, MOVE, MFIR, ...)
# forgets the lanes it writes. GT3_VU1T_NORM_SKIP=0 at generation: always
# normalise.
NORM_SKIP = os.environ.get('GT3_VU1T_NORM_SKIP', '1') != '0'
NORM = {}             # VF register number or 'acc' -> lane bits (1 << k, x = 0)
LOWER_VF_WRITES = []  # (register, dest) the lower of the pair being emitted writes
NORMALISED_KINDS = ('add', 'sub', 'mul', 'madd', 'msub', 'opmula', 'opmsub', 'max', 'mini', 'abs', 'itof')


def known_lanes(reg):
    return NORM.get(reg, 0) if NORM_SKIP else 0


def dest_bits(dest):
    return sum(1 << k for k in lanes_of(dest))


def emit_fmac_simd(L, kind, operand, product_sum, lines, fs, ft, need_flags=True):
    """SSE form of add/sub/mul/madd/msub: defines ur<k>/uf<k> for the lanes
    (and xs for product-sum sticky flags). Returns the extra sticky expression.
    need_flags False (see KEEP_STICKY): only ur<k>, exact values, no flags."""
    lanes = sum(1 << k for k in lines)

    def vec(reg):
        # vf0 reads as (0, 0, 0, 1) in upper instructions
        if reg == 0:
            return '_mm_setr_ps(0.0f, 0.0f, 0.0f, 1.0f)'
        if (known_lanes(reg) & lanes) == lanes:
            return '_mm_loadu_ps(s.vf[%d])' % reg
        return 'loadNV(s.vf[%d])' % reg

    L.append('const __m128 vsv = %s;' % vec(fs))
    if operand == 'q':
        L.append('const __m128 vtv = _mm_set1_ps(N(s.q));')
    elif operand == 'i':
        L.append('const __m128 vtv = _mm_set1_ps(N(s.i));')
    elif isinstance(operand, tuple):
        bc = operand[1]
        if ft == 0:
            L.append('const __m128 vtv = _mm_set1_ps(%s);' % ('1.0f' if bc == 3 else '0.0f'))
        else:
            if known_lanes(ft) & (1 << bc):
                L.append('const __m128 vtv = _mm_set1_ps(s.vf[%d][%d]);' % (ft, bc))
            else:
                L.append('const __m128 vtv = _mm_set1_ps(N(s.vf[%d][%d]));' % (ft, bc))
    else:
        L.append('const __m128 vtv = %s;' % vec(ft))
    madd = kind in ('madd', 'msub')
    if madd:
        L.append('const __m128 acv = %s;' % ('_mm_loadu_ps(s.acc)' if (known_lanes('acc') & lanes) == lanes else 'loadNV(s.acc)'))
        L.append('const __m128 prv = _mm_mul_ps(vsv, vtv);')
        L.append('const __m128 rv = %s(acv, prv);' % ('_mm_add_ps' if kind == 'madd' else '_mm_sub_ps'))
    else:
        L.append('const __m128 rv = %s(vsv, vtv);' % {'add': '_mm_add_ps', 'sub': '_mm_sub_ps', 'mul': '_mm_mul_ps'}[kind])
    if not need_flags:
        L.append('alignas(16) float urv[4]; _mm_store_ps(urv, rv);')
        if madd:
            fn = 'fxMadd' if kind == 'madd' else 'fxMsub'
            L.append('if (!keepMaddV(rv, prv, acv, vsv, vtv, %d)) {' % lanes)
            L.append('  alignas(16) float sa[4], ta[4], aa[4], pa[4];')
            L.append('  _mm_store_ps(sa, vsv); _mm_store_ps(ta, vtv); _mm_store_ps(aa, acv); _mm_store_ps(pa, prv);')
            for k in lines:
                L.append('  (void)%s(urv[%d], pa[%d], aa[%d], sa[%d], ta[%d]);' % (fn, k, k, k, k, k))
        else:
            fn = {'add': 'fxAdd', 'sub': 'fxSub', 'mul': 'fxMul'}[kind]
            L.append('if (!%s(rv, vsv, vtv, %d)) {' % ({'add': 'keepAddV', 'sub': 'keepSubV', 'mul': 'keepMulV'}[kind], lanes))
            L.append('  alignas(16) float sa[4], ta[4]; _mm_store_ps(sa, vsv); _mm_store_ps(ta, vtv);')
            for k in lines:
                L.append('  (void)%s(urv[%d], sa[%d], ta[%d]);' % (fn, k, k, k))
        L.append('}')
        for k in lines:
            L.append('float ur%d = urv[%d];' % (k, k))
        return None
    L.append('alignas(16) float urv[4]; _mm_store_ps(urv, rv); uint8_t ufv[4];')
    if madd:
        fn = 'fxMadd' if kind == 'madd' else 'fxMsub'
        if ZERO_FAST:
            L.append('if (!fastMaddZV(rv, prv, acv, vsv, vtv, %d, ufv)) {' % lanes)
        else:
            L.append('if (!fastMaddV(rv, prv, %d, ufv)) {' % lanes)
        L.append('  alignas(16) float sa[4], ta[4], aa[4], pa[4];')
        L.append('  _mm_store_ps(sa, vsv); _mm_store_ps(ta, vtv); _mm_store_ps(aa, acv); _mm_store_ps(pa, prv);')
        for k in lines:
            L.append('  ufv[%d] = %s(urv[%d], pa[%d], aa[%d], sa[%d], ta[%d]);' % (k, fn, k, k, k, k, k))
        L.append('}')
    else:
        fn = {'add': 'fxAdd', 'sub': 'fxSub', 'mul': 'fxMul'}[kind]
        if ZERO_FAST:
            L.append('if (!%s(rv, vsv, vtv, %d, ufv)) {' % ({'add': 'fastAddV', 'sub': 'fastSubV', 'mul': 'fastMulV'}[kind], lanes))
        else:
            L.append('if (!fastV(rv, %d, ufv)) {' % lanes)
        L.append('  alignas(16) float sa[4], ta[4]; _mm_store_ps(sa, vsv); _mm_store_ps(ta, vtv);')
        for k in lines:
            L.append('  ufv[%d] = %s(urv[%d], sa[%d], ta[%d]);' % (k, fn, k, k, k))
        L.append('}')
    for k in lines:
        L.append('float ur%d = urv[%d]; const uint8_t uf%d = ufv[%d];' % (k, k, k, k))
    if product_sum:
        if not madd:
            raise Unsupported('product-sum without a product')
        L.append('const uint32_t xs = prodFlagsV(prv, vsv, vtv, %d);' % lanes)
        return 'xs'
    return '0u'


def emit_upper(p, L, drop_flags=False):
    """Emits the upper instruction. Results for a VF target go to ur0..ur3
    (written back by the caller); ACC results are written here. Returns
    (target, dest). drop_flags: the flag result can never be read (see
    dead_flag_pairs); only its sticky bits are kept."""
    u = p.upper
    kind, operand, target, product_sum = upper_info(u)
    dest, fs, ft = DEST(u), FS(u), FT(u)
    op = u & 0x3F
    UPPER_VEC[0] = False
    if kind == 'nop':
        return None, 0
    if kind == 'clip':
        L.append('{ const uint32_t wb = floatBits(s.vf[%d][3]);' % ft)
        L.append('  const int32_t lim = (wb & 0x7F800000u) != 0u ? static_cast<int32_t>(wb & 0x7FFFFFFFu) : 0x007FFFFF;')
        L.append('  uint32_t cf = 0u;')
        for k in range(3):
            L.append('  { const uint32_t b = floatBits(s.vf[%d][%d]); if (static_cast<int32_t>(b) > lim) cf |= 0x%xu;'
                     ' if (static_cast<int32_t>(b ^ 0x80000000u) > lim) cf |= 0x%xu; }' % (fs, k, 1 << (2 * k), 2 << (2 * k)))
        L.append('  pushClip(c, cf); }')
        return None, 0
    if dest == 0 and kind not in ('itof', 'ftoi', 'abs', 'max', 'mini'):
        # FMAC with no lanes: no result and no flag entry.
        return None, 0
    if operand == 'q':
        L.append('commit(c);')
    lines = lanes_of(dest)
    if kind in ('opmsub', 'opmula') and dest != 0xE:
        raise Unsupported('opmsub/opmula with dest %x (undeclared cross reads)' % dest)

    def S(k):
        return 'vs%d' % k

    def T(k):
        if operand == 'q':
            return 'qq'
        if operand == 'i':
            return 'ii'
        if isinstance(operand, tuple):
            return 'vt%d' % operand[1]
        return 'vt%d' % k

    if SIMD_MINMAX and kind in ('max', 'mini') and lines:
        # MINPS/MAXPS return the first operand when the comparison holds, else
        # the second: the scalar "(vs < vt) ? vs : vt" / "(vs > vt) ? vs : vt"
        # exactly (N()-ed operands: no NaN, no denormal). No flags.
        lanes = dest_bits(dest)

        def mvec(reg):
            if reg == 0:
                return '_mm_setr_ps(0.0f, 0.0f, 0.0f, 1.0f)'
            if (known_lanes(reg) & lanes) == lanes:
                return '_mm_loadu_ps(s.vf[%d])' % reg
            return 'loadNV(s.vf[%d])' % reg
        if operand == 'i':
            vt = '_mm_set1_ps(N(s.i))'
        elif isinstance(operand, tuple):
            bc = operand[1]
            if ft == 0:
                vt = '_mm_set1_ps(%s)' % ('1.0f' if bc == 3 else '0.0f')
            elif known_lanes(ft) & (1 << bc):
                vt = '_mm_set1_ps(s.vf[%d][%d])' % (ft, bc)
            else:
                vt = '_mm_set1_ps(N(s.vf[%d][%d]))' % (ft, bc)
        else:
            vt = mvec(ft)
        L.append('alignas(16) float urv[4]; _mm_store_ps(urv, %s(%s, %s));' % ('_mm_min_ps' if kind == 'mini' else '_mm_max_ps', mvec(fs), vt))
        for k in lines:
            L.append('float ur%d = urv[%d];' % (k, k))
        UPPER_VEC[0] = True
        return target, dest

    if SIMD_MINMAX and kind == 'ftoi' and lines and fs != 0:
        # floatToInt per lane: double(v) * scale, saturated, truncated. The
        # scales are powers of two, so the float product is exact (or, past
        # the float range, FLT_MAX under round-toward-zero: saturated alike);
        # CVTTPS2DQ truncates and gives INT_MIN outside the range, and lanes
        # >= 2^31 become INT_MAX.
        lanes = dest_bits(dest)
        scale = ['1.0f', '16.0f', '4096.0f', '32768.0f'][operand]
        src = '_mm_loadu_ps(s.vf[%d])' % fs if (known_lanes(fs) & lanes) == lanes else 'loadNV(s.vf[%d])' % fs
        L.append('alignas(16) float urv[4];')
        L.append('{ const __m128 fm = _mm_mul_ps(%s, _mm_set1_ps(%s));' % (src, scale))
        L.append('  const __m128i fi = _mm_blendv_epi8(_mm_cvttps_epi32(fm), _mm_set1_epi32(0x7FFFFFFF), '
                 '_mm_castps_si128(_mm_cmpge_ps(fm, _mm_set1_ps(2147483648.0f))));')
        L.append('  _mm_store_ps(urv, _mm_castsi128_ps(fi)); }')
        for k in lines:
            L.append('float ur%d = urv[%d];' % (k, k))
        UPPER_VEC[0] = True
        return target, dest

    if SIMD_MINMAX and kind == 'itof' and lines and fs != 0:
        # static_cast<float>(int32) per lane (CVTSI2SS, MXCSR rounding) /
        # 2^n: CVTDQ2PS rounds the same way; the power-of-two division is exact.
        div = ['', ', _mm_set1_ps(16.0f)', ', _mm_set1_ps(4096.0f)', ', _mm_set1_ps(32768.0f)'][operand]
        conv = '_mm_cvtepi32_ps(_mm_loadu_si128(reinterpret_cast<const __m128i *>(s.vf[%d])))' % fs
        L.append('alignas(16) float urv[4]; _mm_store_ps(urv, %s);' % (('_mm_div_ps(%s%s)' % (conv, div)) if div else conv))
        for k in lines:
            L.append('float ur%d = urv[%d];' % (k, k))
        UPPER_VEC[0] = True
        return target, dest

    if SIMD_FMAC and kind in ('add', 'sub', 'mul', 'madd', 'msub'):
        # Four lanes in one SSE operation (bit-identical to the scalar lanes);
        # the exact scalar fx* runs per lane only when a lane leaves the fast path.
        need_flags = not (drop_flags and not KEEP_STICKY)
        extra = emit_fmac_simd(L, kind, operand, product_sum, lines, fs, ft, need_flags)
        UPPER_VEC[0] = True
        mac = ' | '.join('macBits(uf%d, 0x%xu)' % (k, lane(k)) for k in lines)
        st = ' | '.join('uf%d' % k for k in lines)
        if not need_flags:
            pass
        elif drop_flags:
            L.append('c.stickyAcc |= static_cast<uint32_t>(%s) | %s;' % (st, extra))
        else:
            L.append('pushFlags(c, %s, static_cast<uint32_t>(%s), %s);' % (mac, st, extra))
        if target == 'acc':
            if VEC_STORES:
                vec_store(L, 's.acc', dest, ur_vec(dest))
            else:
                for k in lines:
                    L.append('s.acc[%d] = ur%d;' % (k, k))
            return 'acc', dest
        return target, dest

    need_vs = set()
    need_vt = set()
    need_acc = set()
    for k in lines:
        if kind in ('opmsub', 'opmula'):
            if k < 3:
                need_vs.add(CROSS_L[k]); need_vt.add(CROSS_R[k])
                if kind == 'opmsub':
                    need_acc.add(k)
            continue
        need_vs.add(k)
        if isinstance(operand, tuple):
            need_vt.add(operand[1])
        elif operand == 'vt':
            need_vt.add(k)
        if kind in ('madd', 'msub'):
            need_acc.add(k)
    if product_sum:
        for k in lines:
            need_vs.add(CROSS_L[k] if op == 0x2E else k)
            if isinstance(operand, tuple):
                need_vt.add(operand[1])
            elif operand == 'vt':
                need_vt.add(CROSS_R[k] if op == 0x2E else k)
    if kind in ('itof',):
        need_vs = set()
    def vfop(reg, k):
        # vf0 is (0,0,0,1) whenever an upper instruction reads it
        if reg == 0:
            return '1.0f' if k == 3 else '0.0f'
        if known_lanes(reg) & (1 << k):
            return 's.vf[%d][%d]' % (reg, k)
        return 'N(s.vf[%d][%d])' % (reg, k)
    for k in sorted(need_vs):
        L.append('const float vs%d = %s;' % (k, vfop(fs, k)))
    for k in sorted(need_vt):
        L.append('const float vt%d = %s;' % (k, vfop(ft, k)))
    for k in sorted(need_acc):
        L.append('const float ac%d = %s;' % (k, ('s.acc[%d]' % k) if known_lanes('acc') & (1 << k) else ('N(s.acc[%d])' % k)))
    if operand == 'q':
        L.append('const float qq = N(s.q);')
    if operand == 'i':
        L.append('const float ii = N(s.i);')

    fmac = kind in ('add', 'sub', 'madd', 'msub', 'mul', 'opmsub', 'opmula')
    res = {}
    prod = {}  # lane -> (product variable, left, right) for product-sum sticky flags
    for k in lines:
        if kind in ('opmsub', 'opmula') and k == 3:
            L.append('float ur3 = 0.0f;')
            L.append('const uint8_t uf3 = fx(ur3, 0.0);')
        elif kind == 'add':
            L.append('float ur%d = %s + %s;' % (k, S(k), T(k)))
            L.append('const uint8_t uf%d = fxAdd(ur%d, %s, %s);' % (k, k, S(k), T(k)))
        elif kind == 'sub':
            L.append('float ur%d = %s - %s;' % (k, S(k), T(k)))
            L.append('const uint8_t uf%d = fxSub(ur%d, %s, %s);' % (k, k, S(k), T(k)))
        elif kind == 'mul':
            L.append('float ur%d = %s * %s;' % (k, S(k), T(k)))
            L.append('const uint8_t uf%d = fxMul(ur%d, %s, %s);' % (k, k, S(k), T(k)))
        elif kind == 'opmula':
            a, b = 'vs%d' % CROSS_L[k], 'vt%d' % CROSS_R[k]
            L.append('float ur%d = %s * %s;' % (k, a, b))
            L.append('const uint8_t uf%d = fxMul(ur%d, %s, %s);' % (k, k, a, b))
        elif kind in ('madd', 'msub', 'opmsub'):
            if kind == 'opmsub':
                a, b = 'vs%d' % CROSS_L[k], 'vt%d' % CROSS_R[k]
            else:
                a, b = S(k), T(k)
            sign = '+' if kind == 'madd' else '-'
            fn = 'fxMadd' if kind == 'madd' else 'fxMsub'
            # same float operations as the interpreter's "acc + vs * t"
            L.append('const float pr%d = %s * %s;' % (k, a, b))
            L.append('float ur%d = ac%d %s pr%d;' % (k, k, sign, k))
            L.append('const uint8_t uf%d = %s(ur%d, pr%d, ac%d, %s, %s);' % (k, fn, k, k, k, a, b))
            prod[k] = ('pr%d' % k, a, b)
        elif kind == 'max':
            L.append('float ur%d = (%s > %s) ? %s : %s;' % (k, S(k), T(k), S(k), T(k)))
        elif kind == 'mini':
            L.append('float ur%d = (%s < %s) ? %s : %s;' % (k, S(k), T(k), S(k), T(k)))
        elif kind == 'itof':
            div = ['', ' / 16.0f', ' / 4096.0f', ' / 32768.0f'][operand]
            L.append('float ur%d = static_cast<float>(static_cast<int32_t>(floatBits(s.vf[%d][%d])))%s;' % (k, fs, k, div))
        elif kind == 'ftoi':
            scale = ['1.0f', '16.0f', '4096.0f', '32768.0f'][operand]
            L.append('float ur%d = bitsToFloat(static_cast<uint32_t>(floatToInt(vs%d, %s)));' % (k, k, scale))
        elif kind == 'abs':
            L.append('float ur%d = std::fabs(vs%d);' % (k, k))
        else:
            raise Unsupported(kind)
        res[k] = True
    if fmac:
        extra = '0u'
        if product_sum:
            parts = []
            for k in lines:
                if k in prod:
                    pv, a, b = prod[k]
                    parts.append('prodFlags(%s, %s, %s)' % (pv, a, b))
                else:
                    raise Unsupported('product-sum lane without product')
            L.append('const uint32_t xs = %s;' % ' | '.join('static_cast<uint32_t>(%s)' % x for x in parts))
            extra = 'xs'
        mac = ' | '.join('macBits(uf%d, 0x%xu)' % (k, lane(k)) for k in lines)
        st = ' | '.join('uf%d' % k for k in lines)
        if drop_flags:
            L.append('c.stickyAcc |= static_cast<uint32_t>(%s) | %s;' % (st, extra))
        else:
            L.append('pushFlags(c, %s, static_cast<uint32_t>(%s), %s);' % (mac, st, extra))
    if target == 'acc':
        if VEC_STORES:
            vec_store(L, 's.acc', dest, ur_vec(dest))
        else:
            for k in lines:
                L.append('s.acc[%d] = ur%d;' % (k, k))
        return 'acc', dest
    return target, dest


# ------------------------------------------------------------------ lower ops
def addr_imm(base, imm):
    return '((static_cast<uint32_t>(static_cast<int32_t>(s.vi[%d] + (%d))) * 16u) & 0x%XFu)' % (base, imm, DATA_MASK >> 4)


def addr_reg(base):
    return '((static_cast<uint32_t>(static_cast<uint16_t>(s.vi[%d])) * 16u) & 0x%XFu)' % (base, DATA_MASK >> 4)


def first_lane(dest):
    if dest & 8:
        return 0
    if dest & 4:
        return 1
    if dest & 2:
        return 2
    return 3


# Register writes as one 16-byte store (a blend with the old value for a
# partial dest). Four scalar stores made the next 16-byte load of the register
# (the following instruction's operand, usually) a store-forwarding stall.
# GT3_VU1T_VEC_STORE=0 at generation time: scalar stores as before.
VEC_STORES = os.environ.get('GT3_VU1T_VEC_STORE', '1') != '0'


def vec_store(L, dst, dest, vec):
    """Stores the lanes of dest (x = lane 0) of the __m128 expression vec into
    the 4-float register dst (s.vf[n] or s.acc)."""
    mask = sum(1 << k for k in lanes_of(dest))
    if mask == 0xF:
        L.append('_mm_storeu_ps(%s, %s);' % (dst, vec))
    elif mask:
        L.append('_mm_storeu_ps(%s, _mm_blend_ps(_mm_loadu_ps(%s), %s, %d));' % (dst, dst, vec, mask))


def lanes_vec(dest, values):
    """__m128 of per-lane C float expressions (lanes outside dest unused)."""
    used = [values[k] for k in lanes_of(dest)]
    if used and all(v == used[0] for v in used):
        return '_mm_set1_ps(%s)' % used[0]
    if list(values) == ['t[0]', 't[1]', 't[2]', 't[3]']:
        return '_mm_loadu_ps(t)'
    return '_mm_setr_ps(%s)' % ', '.join(values[k] if dest & lane(k) else '0.0f' for k in range(4))


def ur_vec(dest):
    """The upper result as a vector: urv (SIMD FMAC path) or ur0..ur3."""
    if UPPER_VEC[0]:
        return '_mm_load_ps(urv)'
    return lanes_vec(dest, ['ur%d' % k for k in range(4)])


UPPER_VEC = [False]  # the last emit_upper left its result in urv


def vf_write(L, reg, dest, values, skip):
    """values: list of 4 C expressions (lanes)."""
    if reg == 0 or skip:
        return
    LOWER_VF_WRITES.append((reg, dest))
    if VEC_STORES:
        vec_store(L, 's.vf[%d]' % reg, dest, lanes_vec(dest, values))
        return
    for k in lanes_of(dest):
        L.append('s.vf[%d][%d] = %s;' % (reg, k, values[k]))


def emit_lower(p, L, branch_vi, skip_vf):
    """Emits the lower instruction. branch_vi(reg) gives the C expression a
    branch uses for a VI read. Returns a branch descriptor or None:
    ('b', target) | ('cond', cexpr, target) | ('jr', cexpr)."""
    l = p.lower
    if p.i_bit or l == 0 or l == 0x8000033C:
        return None
    pc = p.pc
    n = lower_name(l)
    dest = DEST(l)
    vfT, vfS, viT, viS, viD = FT(l), FS(l), VIT(l), VIS(l), VID(l)
    imm11 = IMM11(l)
    target = (pc + 8 + imm11 * 8) & MASK
    link = (pc + 16) // 8
    if n == 'lq':
        L.append('{ const uint32_t a = %s; float t[4]; std::memcpy(t, c.mem + a, 16);' % addr_imm(viS, imm11))
        vf_write(L, vfT, dest, ['t[0]', 't[1]', 't[2]', 't[3]'], skip_vf)
        L.append('}')
    elif n == 'sq':
        L.append('{ uint32_t w[4]; std::memcpy(w, s.vf[%d], 16); store(c, %s, w, 0x%xu); }' % (vfS, addr_imm(viT, imm11), dest))
    elif n == 'ilw':
        L.append('{ uint32_t v; std::memcpy(&v, c.mem + %s + %d, 4);' % (addr_imm(viS, imm11), first_lane(dest) * 4))
        if viT:
            L.append('  s.vi[%d] = static_cast<int16_t>(v & 0xFFFFu);' % viT)
        L.append('}')
    elif n == 'isw':
        L.append('{ const uint32_t v = static_cast<uint16_t>(s.vi[%d] & 0xFFFF); const uint32_t w[4] = {v, v, v, v}; store(c, %s, w, 0x%xu); }'
                 % (viT, addr_imm(viS, imm11), dest))
    elif n in ('iaddiu', 'isubiu'):
        imm = (l & 0x7FF) | ((l >> 10) & 0x7800)
        if viT:
            L.append('s.vi[%d] = static_cast<int16_t>(s.vi[%d] %s %d);' % (viT, viS, '+' if n == 'iaddiu' else '-', imm))
    elif n in ('fceq', 'fcand', 'fcor'):
        imm = l & 0xFFFFFF
        L.append('commit(c);')
        if n == 'fceq':
            L.append('s.vi[1] = ((s.clip & 0xFFFFFFu) == 0x%xu) ? 1 : 0;' % imm)
        elif n == 'fcand':
            L.append('s.vi[1] = ((s.clip & 0x%xu) != 0u) ? 1 : 0;' % imm)
        else:
            L.append('s.vi[1] = ((s.clip | 0x%xu) == 0xFFFFFFu) ? 1 : 0;' % imm)
    elif n == 'fcset':
        L.append('pushFcset(c, 0x%xu);' % (l & 0xFFFFFF))
    elif n in ('fseq', 'fsand', 'fsor'):
        imm = (((l >> 21) & 1) << 11) | (l & 0x7FF)
        L.append('commit(c);')
        if viT:
            if n == 'fseq':
                L.append('s.vi[%d] = ((s.status & 0xFFFu) == 0x%xu) ? 1 : 0;' % (viT, imm))
            elif n == 'fsand':
                L.append('s.vi[%d] = static_cast<int32_t>((s.status & 0xFFFu) & 0x%xu);' % (viT, imm))
            else:
                L.append('s.vi[%d] = static_cast<int32_t>((s.status & 0xFFFu) | 0x%xu);' % (viT, imm))
    elif n == 'fsset':
        L.append('pushFsset(c, 0x%xu);' % ((((l >> 21) & 1) << 11) | (l & 0x7FF)))
    elif n in ('fmeq', 'fmand', 'fmor'):
        L.append('commit(c);')
        if viT:
            if n == 'fmeq':
                L.append('s.vi[%d] = ((s.mac & 0xFFFFu) == static_cast<uint32_t>(static_cast<uint16_t>(s.vi[%d]))) ? 1 : 0;' % (viT, viS))
            elif n == 'fmand':
                L.append('s.vi[%d] = static_cast<int32_t>(s.mac & static_cast<uint32_t>(static_cast<uint16_t>(s.vi[%d])));' % (viT, viS))
            else:
                L.append('s.vi[%d] = static_cast<int32_t>(s.mac | static_cast<uint32_t>(static_cast<uint16_t>(s.vi[%d])));' % (viT, viS))
    elif n == 'fcget':
        L.append('commit(c);')
        if viT:
            L.append('s.vi[%d] = static_cast<int32_t>(s.clip & 0x0FFFu);' % viT)
    elif n == 'b':
        return ('b', target)
    elif n == 'bal':
        if viT:
            L.append('s.vi[%d] = %d;' % (viT, link))
        return ('b', target)
    elif n in ('jr', 'jalr'):
        L.append('const uint32_t jt = (static_cast<uint32_t>(static_cast<uint16_t>(%s)) * 8u) & 0x%Xu;' % (branch_vi(viS), MASK))
        if n == 'jalr' and viT:
            L.append('s.vi[%d] = %d;' % (viT, link))
        return ('jr', 'jt')
    elif n in ('ibeq', 'ibne'):
        opx = '==' if n == 'ibeq' else '!='
        L.append('const bool bt = static_cast<int16_t>(%s) %s static_cast<int16_t>(%s);' % (branch_vi(viS), opx, branch_vi(viT)))
        return ('cond', 'bt', target)
    elif n in ('ibltz', 'ibgtz', 'iblez', 'ibgez'):
        opx = {'ibltz': '<', 'ibgtz': '>', 'iblez': '<=', 'ibgez': '>='}[n]
        L.append('const bool bt = static_cast<int16_t>(%s) %s 0;' % (branch_vi(viS), opx))
        return ('cond', 'bt', target)
    elif n in ('iadd', 'isub', 'iand', 'ior'):
        if viD:
            if n == 'iadd':
                L.append('s.vi[%d] = static_cast<int16_t>(s.vi[%d] + s.vi[%d]);' % (viD, viS, viT))
            elif n == 'isub':
                L.append('s.vi[%d] = static_cast<int16_t>(s.vi[%d] - s.vi[%d]);' % (viD, viS, viT))
            elif n == 'iand':
                L.append('s.vi[%d] = s.vi[%d] & s.vi[%d];' % (viD, viS, viT))
            else:
                L.append('s.vi[%d] = s.vi[%d] | s.vi[%d];' % (viD, viS, viT))
    elif n == 'iaddi':
        imm5 = (l >> 6) & 0x1F
        imm5 = imm5 - 32 if imm5 & 16 else imm5
        if viT:
            L.append('s.vi[%d] = static_cast<int16_t>(s.vi[%d] + (%d));' % (viT, viS, imm5))
    elif n in ('move', 'mr32'):
        src = [0, 1, 2, 3] if n == 'move' else [1, 2, 3, 0]
        if vfT and not skip_vf and VEC_STORES:
            LOWER_VF_WRITES.append((vfT, dest))
            if n == 'move':
                L.append('{ const __m128 tv = _mm_loadu_ps(s.vf[%d]);' % vfS)
            else:
                L.append('{ const __m128 tv0 = _mm_loadu_ps(s.vf[%d]); const __m128 tv = _mm_shuffle_ps(tv0, tv0, _MM_SHUFFLE(0, 3, 2, 1));' % vfS)
            vec_store(L, 's.vf[%d]' % vfT, dest, 'tv')
            L.append('}')
        elif vfT and not skip_vf:
            L.append('{ const float t[4] = {s.vf[%d][%d], s.vf[%d][%d], s.vf[%d][%d], s.vf[%d][%d]};'
                     % (vfS, src[0], vfS, src[1], vfS, src[2], vfS, src[3]))
            vf_write(L, vfT, dest, ['t[0]', 't[1]', 't[2]', 't[3]'], skip_vf)
            L.append('}')
    elif n in ('lqi', 'lqd'):
        L.append('{')
        if n == 'lqd' and viS:
            L.append('s.vi[%d] = static_cast<int16_t>(s.vi[%d] - 1);' % (viS, viS))
        L.append('const uint32_t a = %s; float t[4]; std::memcpy(t, c.mem + a, 16);' % addr_reg(viS))
        vf_write(L, vfT, dest, ['t[0]', 't[1]', 't[2]', 't[3]'], skip_vf)
        if n == 'lqi' and viS:
            L.append('s.vi[%d] = static_cast<int16_t>(s.vi[%d] + 1);' % (viS, viS))
        L.append('}')
    elif n in ('sqi', 'sqd'):
        L.append('{')
        if n == 'sqd' and viT:
            L.append('s.vi[%d] = static_cast<int16_t>(s.vi[%d] - 1);' % (viT, viT))
        L.append('uint32_t w[4]; std::memcpy(w, s.vf[%d], 16); store(c, %s, w, 0x%xu);' % (vfS, addr_reg(viT), dest))
        if n == 'sqi' and viT:
            L.append('s.vi[%d] = static_cast<int16_t>(s.vi[%d] + 1);' % (viT, viT))
        L.append('}')
    elif n == 'div':
        fsf, ftf = (l >> 21) & 3, (l >> 23) & 3
        L.append('{ const float num = N(s.vf[%d][%d]); const float den = N(s.vf[%d][%d]); uint32_t di = 0u; float r;' % (vfS, fsf, vfT, ftf))
        L.append('  if (den == 0.0f) { di = num == 0.0f ? 0x10u : 0x20u; r = std::signbit(num) != std::signbit(den) ? -std::numeric_limits<float>::max() : std::numeric_limits<float>::max(); }')
        L.append('  else r = num / den;')
        L.append('  queueQ(c, normalizeResult(r), 7u, di); }')
    elif n == 'sqrt':
        ftf = (l >> 23) & 3
        L.append('{ const float v = N(s.vf[%d][%d]); queueQ(c, std::sqrt(std::fabs(v)), 7u, v < 0.0f ? 0x10u : 0u); }' % (vfT, ftf))
    elif n == 'rsqrt':
        fsf, ftf = (l >> 21) & 3, (l >> 23) & 3
        L.append('{ const float num = N(s.vf[%d][%d]); const float rad = N(s.vf[%d][%d]); const float den = std::sqrt(std::fabs(rad));' % (vfS, fsf, vfT, ftf))
        L.append('  uint32_t di = rad < 0.0f ? 0x10u : 0u; float r;')
        L.append('  if (den != 0.0f) r = num / den;')
        L.append('  else { di = num == 0.0f ? 0x10u : 0x20u; r = std::signbit(num) ? -std::numeric_limits<float>::max() : std::numeric_limits<float>::max(); }')
        L.append('  queueQ(c, normalizeResult(r), 13u, di); }')
    elif n in ('waitq', 'waitp'):
        pass
    elif n == 'mtir':
        comp = (l >> 21) & 3
        if viT:
            L.append('s.vi[%d] = static_cast<int16_t>(floatBits(s.vf[%d][%d]) & 0xFFFFu);' % (viT, vfS, comp))
    elif n == 'mfir':
        if vfT and not skip_vf:
            L.append('{ const float t = bitsToFloat(static_cast<uint32_t>(static_cast<int32_t>(static_cast<int16_t>(s.vi[%d] & 0xFFFF))));' % viS)
            vf_write(L, vfT, dest, ['t'] * 4, skip_vf)
            L.append('}')
    elif n == 'ilwr':
        L.append('{ uint32_t v; std::memcpy(&v, c.mem + %s + %d, 4);' % (addr_reg(viS), first_lane(dest) * 4))
        if viT:
            L.append('  s.vi[%d] = static_cast<int16_t>(v & 0xFFFFu);' % viT)
        L.append('}')
    elif n == 'iswr':
        L.append('{ const uint32_t v = static_cast<uint16_t>(s.vi[%d] & 0xFFFF); const uint32_t w[4] = {v, v, v, v}; store(c, %s, w, 0x%xu); }'
                 % (viT, addr_reg(viS), dest))
    elif n == 'rnext':
        L.append('{ const uint32_t x = (s.r >> 4) & 1u; const uint32_t y = (s.r >> 22) & 1u; s.r = ((s.r << 1) ^ x ^ y) & 0x007FFFFFu; s.r |= 0x3F800000u; }')
        if vfT and not skip_vf:
            L.append('{ const float t = bitsToFloat(s.r);')
            vf_write(L, vfT, dest, ['t'] * 4, skip_vf)
            L.append('}')
    elif n == 'rget':
        if vfT and not skip_vf:
            L.append('{ const float t = bitsToFloat(s.r);')
            vf_write(L, vfT, dest, ['t'] * 4, skip_vf)
            L.append('}')
    elif n == 'rinit':
        L.append('s.r = 0x3F800000u | (floatBits(s.vf[%d][%d]) & 0x007FFFFFu);' % (vfS, (l >> 21) & 3))
    elif n == 'rxor':
        L.append('s.r = 0x3F800000u | ((s.r ^ floatBits(s.vf[%d][%d])) & 0x007FFFFFu);' % (vfS, (l >> 21) & 3))
    elif n == 'mfp':
        L.append('commit(c);')
        if vfT and not skip_vf:
            L.append('{ const float t = s.p;')
            vf_write(L, vfT, dest, ['t'] * 4, skip_vf)
            L.append('}')
    elif n in ('xtop', 'xitop'):
        if viT:
            L.append('s.vi[%d] = static_cast<int32_t>(s.%s & 0x3FFu);' % (viT, 'top' if n == 'xtop' else 'itop'))
    elif n == 'xgkick':
        L.append('startKick(c, static_cast<uint32_t>(static_cast<uint16_t>(s.vi[%d])));' % viS)
    elif n in ('esadd', 'ersadd', 'eleng', 'erleng'):
        L.append('{ const float x = N(s.vf[%d][0]); const float y = N(s.vf[%d][1]); const float z = N(s.vf[%d][2]);' % (vfS, vfS, vfS))
        if n == 'esadd':
            L.append('  queueP(c, x * x + y * y + z * z, 11u); }')
        elif n == 'ersadd':
            L.append('  const float sum = x * x + y * y + z * z; queueP(c, sum != 0.0f ? 1.0f / sum : sum, 18u); }')
        elif n == 'eleng':
            L.append('  queueP(c, std::sqrt(x * x + y * y + z * z), 18u); }')
        else:
            L.append('  const float len = std::sqrt(x * x + y * y + z * z); queueP(c, len != 0.0f ? 1.0f / len : len, 24u); }')
    elif n in ('ersqrt', 'esqrt', 'ercpr'):
        comp = (l >> 21) & 3
        L.append('{ const float v = N(s.vf[%d][%d]);' % (vfS, comp))
        if n == 'ersqrt':
            L.append('  float r = v; if (r >= 0.0f) { r = std::sqrt(r); if (r != 0.0f) r = 1.0f / r; } queueP(c, r, 18u); }')
        elif n == 'esqrt':
            L.append('  queueP(c, v >= 0.0f ? std::sqrt(v) : v, 12u); }')
        else:
            L.append('  queueP(c, v != 0.0f ? 1.0f / v : v, 12u); }')
    elif n == 'esum':
        L.append('{ float sum = 0.0f; for (uint32_t k = 0; k < 4u; ++k) sum += N(s.vf[%d][k]); queueP(c, sum, 12u); }' % vfS)
    else:
        raise Unsupported('lower %s at %x' % (n, pc))
    return None


# ---------------------------------------------------------------- pair code
def pair_rw(p):
    """Register reads (stall-checked) and writes (with latency) of a pair, as
    keys ('vf', reg, lane) / ('vi', reg). ACC is left out: its latency is one
    cycle, so it never stalls the following pair."""
    reads = []
    for u in (p.uu, p.lu):
        for reg, lanes in u.vf_read:
            if reg:
                reads += [('vf', reg, k) for k in lanes_of(lanes)]
        for reg in range(1, 16):
            if u.vi_read & (1 << reg):
                reads.append(('vi', reg))
    writes = []
    lw = p.lu.vf_write
    if lw[0] and p.suppressed_lower_vf != lw[0]:
        lat = p.lu.vf_latency or p.lu.latency
        writes += [(('vf', lw[0], k), lat) for k in lanes_of(lw[1])]
    uw = p.uu.vf_write
    if uw[0]:
        lat = p.uu.vf_latency or p.uu.latency
        writes += [(('vf', uw[0], k), lat) for k in lanes_of(uw[1])]
    for reg in range(1, 16):
        if p.lu.vi_write & (1 << reg):
            writes.append((('vi', reg), p.lu.vi_latency or p.lu.latency))
    return list(dict.fromkeys(reads)), writes


def ready_key(key):
    return 'c.vfReady[%d][%d]' % (key[1], key[2]) if key[0] == 'vf' else 'c.viReady[%d]' % key[1]


def ready_expr_lines(p, need=None):
    """Stall computation (VU1Interpreter::calculatePairReadyCycle). need: the
    register keys that can still be pending (None: all reads, plus ACC)."""
    L = []
    terms = []
    reads, _ = pair_rw(p)
    for key in reads:
        if need is None or key in need:
            terms.append(ready_key(key))
    if need is None:
        for u in (p.uu, p.lu):
            for k in lanes_of(u.acc_read):
                terms.append('c.accReady[%d]' % k)
    lu = p.lu
    if lu.pipeline == PIPE_FDIV:
        terms.append('qWaitCycle(c)')
    if lu.pipeline == PIPE_EFU:
        terms.append('c.efuResourceReady')
    if lu.wait_p:
        terms.append('pWaitCycle(c)')
    terms = list(dict.fromkeys(terms))
    for t in terms:
        L.append('r = std::max<uint64_t>(r, %s);' % t)
    if lu.pipeline == PIPE_XGKICK:
        L.append('if (c.kick.active) r = std::max<uint64_t>(r, kickFinish(c));')
    if not L:
        return []
    return ['{ uint64_t r = c.cycle;'] + ['  ' + x for x in L] + ['  c.cycle = r; }']


def mark_lines(p, marks=None, last_lat=None):
    """VU1Interpreter::markPairWrites plus the latest-ready bookkeeping.
    marks: keys whose ready cycle must be stored (None: all, plus ACC);
    last_lat: latency to fold into c.lastReady (None: the largest; 0: none)."""
    L = []
    _, writes = pair_rw(p)
    lats = []
    for key, lat in writes:
        if marks is None or key in marks:
            L.append('%s = c.cycle + %d;' % (ready_key(key), lat))
        lats.append(lat)
    if marks is None:
        for k in lanes_of(p.uu.acc_write):
            L.append('c.accReady[%d] = c.cycle + 1;' % k)
        if p.uu.acc_write:
            lats.append(1)
        last_lat = max(lats) if lats else 0
    if last_lat:
        L.append('c.lastReady = std::max<uint64_t>(c.lastReady, c.cycle + %d);' % last_lat)
    return L


def written_vi(p):
    for reg in range(1, 16):
        if p.lu.vi_write & (1 << reg):
            return reg
    return 0


def emit_pair_body(p, L, branch_vi, timing=None, drop_flags=False):
    """Code for one pair without the cycle increment and control flow.
    timing: (need, marks, last_lat) from the block analysis, None for the
    generic (pair stepping) form. drop_flags: see emit_upper. Returns the
    branch descriptor."""
    need, marks, last_lat = timing if timing else (None, None, None)
    LOWER_VF_WRITES.clear()
    L.extend(ready_expr_lines(p, need))
    wvi = written_vi(p)
    if wvi and p.lu.delays_next_branch_read:
        L.append('const int32_t oldvi_%x = s.vi[%d];' % (p.pc, wvi))
    UL = []
    target, dest = emit_upper(p, UL, drop_flags)
    vf_target = target[1] if isinstance(target, tuple) else None
    L.extend(UL)
    # an upper result for vf0 is visible to the lower (the interpreter writes vf0 then resets it)
    if vf_target == 0 and dest:
        for k in lanes_of(dest):
            L.append('s.vf[0][%d] = ur%d;' % (k, k))
    if p.i_bit:
        L.append('s.i = N(bitsToFloat(0x%08xu));' % p.lower)
    # the lower sees the registers before the upper's result (VU1Interpreter's
    # shadow rule); a lower VF write to the upper's register is dropped
    br = emit_lower(p, L, branch_vi, skip_vf=bool(p.suppressed_lower_vf))
    if vf_target and dest:
        if VEC_STORES:
            vec_store(L, 's.vf[%d]' % vf_target, dest, ur_vec(dest))
        else:
            for k in lanes_of(dest):
                L.append('s.vf[%d][%d] = ur%d;' % (vf_target, k, k))
    L.extend(mark_lines(p, marks, last_lat))
    touches_vf0 = vf_target == 0 and dest
    if touches_vf0:
        L.append('s.vf[0][0] = 0.0f; s.vf[0][1] = 0.0f; s.vf[0][2] = 0.0f; s.vf[0][3] = 1.0f;')
    # what this pair leaves normalised (lower writes land first, then the upper's)
    for reg, d in LOWER_VF_WRITES:
        NORM[reg] = NORM.get(reg, 0) & ~dest_bits(d)
    upper_target = 'acc' if target == 'acc' else vf_target
    if upper_target and dest:
        if upper_info(p.upper)[0] in NORMALISED_KINDS:
            NORM[upper_target] = NORM.get(upper_target, 0) | dest_bits(dest)
        else:
            NORM[upper_target] = NORM.get(upper_target, 0) & ~dest_bits(dest)
    return br


def check_supported(p):
    if p.d_bit or p.t_bit:
        raise Unsupported('D/T bit at %x' % p.pc)
    if p.uu.reserved or p.lu.reserved:
        raise Unsupported('reserved at %x' % p.pc)
    if UNIT == 'vu0' and not p.i_bit and p.lu.pipeline in (PIPE_EFU, PIPE_XGKICK):
        raise Unsupported('EFU/XGKICK on VU0 at %x' % p.pc)  # reserved on VU0 in the interpreter
    if p.m_bit:
        pass


# ---------------------------------------------------------------- generator
def analyse(code, extra_leaders):
    pairs = {}
    for pc in range(0, CODE_SIZE, 8):
        pairs[pc] = decode_pair(code, pc)
    leaders = set(extra_leaders)
    jr_regs = set()
    for pc, p in pairs.items():
        if not p.i_bit and lower_name(p.lower) in ('jr', 'jalr'):
            jr_regs.add(VIS(p.lower))
    for pc, p in pairs.items():
        if p.i_bit:
            continue
        n = lower_name(p.lower)
        if n in ('b', 'bal', 'ibeq', 'ibne', 'ibltz', 'ibgtz', 'iblez', 'ibgez'):
            leaders.add((pc + 8 + IMM11(p.lower) * 8) & MASK)
            leaders.add((pc + 16) & MASK)
        elif n in ('jr', 'jalr'):
            leaders.add((pc + 16) & MASK)
        elif n in ('iaddiu',) and VIS(p.lower) == 0 and VIT(p.lower) in jr_regs:
            imm = (p.lower & 0x7FF) | ((p.lower >> 10) & 0x7800)
            if 0 < imm * 8 < CODE_SIZE:
                leaders.add(imm * 8)  # possible JR target (a return address loaded as a constant)
    return pairs, leaders


def is_branch(p):
    return (not p.i_bit) and p.lu.pipeline == PIPE_BRANCH


def block_sequence(start, pairs, leaders, unsupported):
    """Pairs a block executes, in order, and how it ends:
    ('fall', next_pc) | ('branch',) (last two pairs: branch, delay slot) |
    ('end', final_pc) (last two pairs: E-bit pair, following pair)."""
    seq = []
    pc = start
    while True:
        if pc in unsupported:
            return None, None
        p = pairs[pc]
        seq.append(p)
        if p.e_bit:
            if is_branch(p):
                raise Unsupported('branch with E bit at %x' % pc)
            nxt = (pc + 8) & MASK
            q = pairs[nxt]
            if nxt in unsupported or is_branch(q) or q.e_bit:
                raise Unsupported('unsupported pair after the E bit at %x' % nxt)
            seq.append(q)
            return seq, ('end', (nxt + 8) & MASK)
        if is_branch(p):
            ds = (pc + 8) & MASK
            q = pairs[ds]
            if ds in unsupported or is_branch(q):
                raise Unsupported('branch in delay slot at %x' % ds)
            seq.append(q)
            return seq, ('branch',)
        pc = (pc + 8) & MASK
        if pc in leaders or pc == 0:
            return seq, ('fall', pc)


ENTRY_PENDING = None  # pc -> {key: pairs a write may still need}, whole program


def entry_pending(pairs, unsupported):
    """Whole-program readiness: for every pair, each register key a write may
    still be pending on when the pair starts, with the most pairs it can still
    need (every pair takes at least one cycle, so a write of latency L at pair
    j is ready by pair j + L). A translated program starts with nothing pending
    (Ctx::begin zeroes the ready cycles at every MSCAL/MSCNT); the pair after an
    E-bit pair's successor ends it. JR through vi15 goes to the call return
    points (as live_mac_writers); what is pending after another JR/JALR (GT3's
    vi1 jump tables) is joined into every pair. Keys absent at a block's start
    need no stall check before any write of theirs in that block."""
    returns = set()
    for pc in range(0, CODE_SIZE, 8):
        p = pairs[pc]
        if p.i_bit:
            continue
        n = lower_name(p.lower)
        if n in ('bal', 'jalr') and VIT(p.lower) == 15:
            returns.add((pc + 16) & MASK)
        elif n == 'iaddiu' and VIS(p.lower) == 0 and VIT(p.lower) == 15:
            imm = (p.lower & 0x7FF) | ((p.lower >> 10) & 0x7800)
            returns.add((imm * 8) & MASK)

    def successors(pc):
        """Pairs that can follow pc: a list, or None for an unknown target."""
        prev_pc = (pc - 8) & MASK
        prev = pairs[prev_pc]
        if prev.e_bit:
            return []  # the pair after an E bit: the program ends
        if is_branch(prev):
            n = lower_name(prev.lower)
            if n in ('jr', 'jalr'):
                return sorted(returns) if VIS(prev.lower) == 15 else None
            target = (prev_pc + 8 + IMM11(prev.lower) * 8) & MASK
            if n in ('b', 'bal'):
                return [target]
            return [target, (pc + 8) & MASK]
        return [(pc + 8) & MASK]

    def join(dst, src):
        grew = False
        for k, v in src.items():
            if v > dst.get(k, 0):
                dst[k] = v
                grew = True
        return grew

    state = {pc: {} for pc in range(0, CODE_SIZE, 8)}
    pool = {}
    changed = True
    while changed:
        changed = False
        for pc in range(0, CODE_SIZE, 8):
            if pc in unsupported:
                continue
            out = {k: v - 1 for k, v in state[pc].items() if v > 1}
            _, writes = pair_rw(pairs[pc])
            for key, lat in writes:
                if lat > 1:
                    out[key] = max(out.get(key, 0), lat - 1)
            nxt = successors(pc)
            if nxt is None:
                changed |= join(pool, out)
                continue
            for t in nxt:
                changed |= join(state[t], out)
        if pool:
            for pc in range(0, CODE_SIZE, 8):
                changed |= join(state[pc], pool)
    return state


def block_timing(seq, entry=None):
    """Static stall analysis of a block (VU1Interpreter timing): a read whose
    latest writer is in the block at least `latency` pairs earlier is ready
    (every pair takes at least one cycle), so its check is dropped; so is a
    read, before any write in the block, of a key that cannot be pending that
    far into the block (entry: the whole-program state at the block's start).
    A ready cycle is stored only if an in-block read may still wait for it or
    a later block may (written in the last 4 pairs, the longest VF/VI latency);
    c.lastReady only takes writes still pending when the block exits."""
    n = len(seq)
    latest = {}
    needs = []
    marks = [set() for _ in seq]
    last = [0] * n
    for i, p in enumerate(seq):
        reads, writes = pair_rw(p)
        need = set()
        for key in reads:
            if key in latest:
                j, lat = latest[key]
                if i - j >= lat:
                    continue
                marks[j].add(key)
            elif entry is not None and entry.get(key, 0) <= i:
                continue
            need.add(key)
        needs.append(need)
        for key, lat in writes:
            latest[key] = (i, lat)
            if n - i < lat:
                last[i] = max(last[i], lat)
    for key, (j, lat) in latest.items():
        if n - j < 4:
            marks[j].add(key)
    return [(needs[i], marks[i], last[i]) for i in range(n)]


def writes_mac(p):
    kind = upper_info(p.upper)[0]
    return kind in FMAC_KINDS and DEST(p.upper) != 0


def reads_mac(p):
    return (not p.i_bit) and lower_name(p.lower) in MAC_READS


def dead_flag_pairs(seq):
    """Pairs of a block whose FMAC flag result can never be read. A result
    issued at cycle t is visible from t + 4 until the next flag result is
    (every pair takes at least one cycle). If the next MAC-writing pair j of
    the block has three more pairs after it in the block, and no MAC read is
    in the pairs after this one up to j + 3, nothing sees this result: later
    reads, in or after the block, come at least four pairs after j."""
    n = len(seq)
    dead = [False] * n
    if not DEAD_FLAGS_OK:
        return dead
    writers = [i for i, p in enumerate(seq) if writes_mac(p)]
    for a, i in enumerate(writers):
        if a + 1 >= len(writers):
            break
        j = writers[a + 1]
        if j + 3 > n - 1:
            continue
        if any(reads_mac(seq[k]) for k in range(i + 1, j + 4)):
            continue
        dead[i] = True
    return dead


LIVE_MAC = None  # pcs of MAC-writing pairs whose result may be read (whole program)


def live_mac_writers(pairs, unsupported):
    """Whole-program (sound) MAC liveness: the pcs of MAC-writing pairs whose
    flag result some reader may see. From writer i, walk the control flow
    tracking k = pairs since the first later writer (0: none yet). Once that
    writer is k >= 4 pairs back it is visible (every pair takes at least a
    cycle) and newer than i, which hides i on that path. i is live if a MAC read (fmand/fmeq/fmor) is reached
    while k < 4, or the walk leaves known code: a program end (the flags carry
    into the next microprogram), an unknown JR/JALR target or unsupported code.
    JR/JALR through vi15, the link register, returns to a call's return point:
    GT3's image writes vi15 only with BAL (return pc + 16), IADDIU constants and
    one ILW (a saved return address). Other JR registers (jump tables) escape.
    GT3_VU1_VERIFY checks the result against the interpreter."""

    returns = set()
    for pc in range(0, CODE_SIZE, 8):
        p = pairs[pc]
        if p.i_bit:
            continue
        n = lower_name(p.lower)
        if n in ('bal', 'jalr') and VIT(p.lower) == 15:
            returns.add((pc + 16) & MASK)
        elif n == 'iaddiu' and VIS(p.lower) == 0 and VIT(p.lower) == 15:
            imm = (p.lower & 0x7FF) | ((p.lower >> 10) & 0x7800)
            returns.add((imm * 8) & MASK)

    def successors(pc):
        prev_pc = (pc - 8) & MASK
        prev = pairs[prev_pc]
        if prev.e_bit:
            return None  # the pair after an E bit: the program ends
        if is_branch(prev):
            n = lower_name(prev.lower)
            if n in ('jr', 'jalr'):
                return sorted(returns) if VIS(prev.lower) == 15 else None
            target = (prev_pc + 8 + IMM11(prev.lower) * 8) & MASK
            if n in ('b', 'bal'):
                return [target]
            return [target, (pc + 8) & MASK]
        return [(pc + 8) & MASK]

    def live(i):
        first = successors(i)
        if first is None:
            return True
        seen = set()
        stack = [(s, 0) for s in first]
        while stack:
            pc, k = stack.pop()
            if (pc, k) in seen:
                continue
            seen.add((pc, k))
            if pc in unsupported:
                return True
            p = pairs[pc]
            if reads_mac(p):
                return True
            k2 = (1 if writes_mac(p) else 0) if k == 0 else k + 1
            if k2 >= 4:
                continue
            nxt = successors(pc)
            if nxt is None:
                return True
            stack.extend((s, k2) for s in nxt)
        return False

    return {pc for pc in range(0, CODE_SIZE, 8) if writes_mac(pairs[pc]) and live(pc)}


# GT3_VU1T_LOCALS=1 at generation time (experiment): a block keeps the VF
# registers it uses in locals (copied in at entry, all written back before
# every return), so its VU memory stores (through uint8_t *, which may alias
# anything) no longer force the compiler to reload them. Only the emitted
# code touches VF registers (no runtime helper reads them).
LOCALS = os.environ.get('GT3_VU1T_LOCALS', '0') == '1'
_VF_REF = re.compile(r's\.vf\[(\d+)\]')


def localize_vf(L):
    regs = sorted({int(m.group(1)) for line in L[2:] for m in _VF_REF.finditer(line)})
    if not regs:
        return L
    body = [_VF_REF.sub(lambda m: 'lv%s' % m.group(1), line) for line in L[2:]]
    wb = ' '.join('std::memcpy(s.vf[%d], lv%d, 16);' % (r, r) for r in regs)
    body = [line.replace('return ', wb + ' return ') if 'return ' in line and not line.lstrip().startswith('//') else line
            for line in body]
    head = L[:2] + ['alignas(16) float lv%d[4]; std::memcpy(lv%d, s.vf[%d], 16);' % (r, r, r) for r in regs]
    return head + body


def gen_block(start, pairs, leaders, unsupported):
    seq, term = block_sequence(start, pairs, leaders, unsupported)
    if seq is None:
        return None
    timing = block_timing(seq, ENTRY_PENDING.get(start) if ENTRY_PENDING is not None else None)
    dead = dead_flag_pairs(seq)
    if DEAD_FLAGS_OK and LIVE_MAC is not None:
        for idx, q in enumerate(seq):
            if writes_mac(q) and q.pc not in LIVE_MAC:
                dead[idx] = True
    L = ['uint32_t block_%04x(Ctx &c) {' % start, 'VU1State &s = *c.s;']
    NORM.clear()

    def bb_exit(q):
        wq = written_vi(q)
        if wq and q.lu.delays_next_branch_read:
            return 'c.bbValid = true; c.bbReg = %d; c.bbValue = oldvi_%x;' % (wq, q.pc)
        return 'c.bbValid = false;'

    def static_bvi(prev):
        def f(reg):
            if reg == 0:
                return '0'
            if prev is None:
                return '(c.bbValid && c.bbReg == %d ? c.bbValue : s.vi[%d])' % (reg, reg)
            if written_vi(prev) == reg and prev.lu.delays_next_branch_read:
                return 'oldvi_%x' % prev.pc
            return 's.vi[%d]' % reg
        return f

    n = len(seq)
    depth = 0
    br = None
    for i, p in enumerate(seq):
        prev = seq[i - 1] if i > 0 else None
        L.append('// %s' % fmt_pair(p).replace('\\', '/'))
        L.append('{')
        depth += 1
        body = []
        b = emit_pair_body(p, body, static_bvi(prev), timing[i], dead[i])
        L.extend(body)
        last_pair = i == n - 1
        if term[0] == 'branch' and i == n - 2:
            br = b
            if br[0] == 'cond':
                L.append('const bool taken_%x = %s;' % (p.pc, br[1]))
            elif br[0] == 'jr':
                L.append('const uint32_t jtarget_%x = %s;' % (p.pc, br[1]))
        L.append('c.cycle += 1;')
        if not last_pair:
            if term[0] != 'branch' or i != n - 2:
                # plain pair: close its scope before the next one
                L.append('}')
                depth -= 1
            continue
        # block exit
        if term[0] == 'fall':
            L.append(bb_exit(p))
            L.append('return 0x%xu;' % term[1])
        elif term[0] == 'end':
            L.append('c.s->pc = 0x%xu;' % term[1])
            L.append('c.bbValid = false;')
            L.append('return kEnd;')
        else:
            bp = seq[n - 2]
            L.append(bb_exit(p))
            if p.e_bit:
                # the pair at the branch destination runs, then the program ends
                L.append('c.pendingEndAfterNext = true;')
            if br[0] == 'b':
                L.append('s.branchTarget = 0x%xu; s.branchDelay = 0;' % br[1])
                L.append('return 0x%xu;' % br[1])
            elif br[0] == 'cond':
                L.append('if (taken_%x) { s.branchTarget = 0x%xu; s.branchDelay = 0; return 0x%xu; }' % (bp.pc, br[2], br[2]))
                L.append('return 0x%xu;' % ((p.pc + 8) & MASK))
            else:
                L.append('s.branchTarget = jtarget_%x; s.branchDelay = 0;' % bp.pc)
                L.append('return jtarget_%x;' % bp.pc)
    L.extend(['}'] * depth)
    L.append('}')
    if LOCALS:
        L = localize_vf(L)
    return L


def gen_image(code, name_hint, extra_leaders):
    global DEAD_FLAGS_OK, LIVE_MAC
    h = fnv64(code)
    ns = ('v0img_%016x' if UNIT == 'vu0' else 'img_%016x') % h
    pairs, leaders = analyse(code, extra_leaders)
    lower_ops = set(lower_name(pairs[pc].lower) for pc in range(0, CODE_SIZE, 8) if not pairs[pc].i_bit)
    DEAD_FLAGS_OK = not (lower_ops & set(STATUS_USES))
    # only translate reachable-looking code: all nonzero pairs + leaders
    out = []
    out.append('// Generated by gt3-tools/vu1_translate.py from %s. Do not edit.' % name_hint)
    out.append('// VU1 code image FNV-1a 64 %016x.' % h)
    out.append('#include "runtime/ps2_vu1_translated.h"')
    out.append('')
    out.append('namespace ps2vu1t { namespace %s {' % ns)
    out.append('namespace {')
    out.append('using PairFn = void (*)(Ctx &);')
    unsupported = {}

    # ---- stepping functions (one per pair)
    for pc in range(0, CODE_SIZE, 8):
        p = pairs[pc]
        L = []
        try:
            check_supported(p)
            NORM.clear()
            br = emit_pair_body(p, L, lambda reg: ('0' if reg == 0 else '(c.bbValid && c.bbReg == %d ? c.bbValue : s.vi[%d])' % (reg, reg)))
        except Unsupported as e:
            unsupported[pc] = str(e)
            continue
        out.append('// %s' % fmt_pair(p).replace('\\', '/'))
        out.append('void step_%04x(Ctx &c) {' % pc)
        out.append('VU1State &s = *c.s;')
        out.extend(L)
        if br:
            if br[0] == 'b':
                out.append('c.brTaken = true; c.brTarget = 0x%xu;' % br[1])
            elif br[0] == 'cond':
                out.append('if (%s) { c.brTaken = true; c.brTarget = 0x%xu; }' % (br[1], br[2]))
            else:
                out.append('c.brTaken = true; c.brTarget = %s;' % br[1])
        wvi = written_vi(p)
        if wvi and p.lu.delays_next_branch_read:
            out.append('c.bbValid = true; c.bbReg = %d; c.bbValue = oldvi_%x;' % (wvi, pc))
        else:
            out.append('c.bbValid = false;')
        out.append('c.cycle += 1;')
        out.append('}')

    # ---- blocks
    global ENTRY_PENDING
    ENTRY_PENDING = entry_pending(pairs, unsupported) if os.environ.get('GT3_VU1T_WHOLE_READY', '1') != '0' else None
    LIVE_MAC = live_mac_writers(pairs, unsupported)
    writers = sum(1 for pc in range(0, CODE_SIZE, 8) if writes_mac(pairs[pc]))
    print('  MAC writers %d, live (whole-program) %d' % (writers, len(LIVE_MAC)))
    block_pcs = []
    for start in sorted(leaders):
        if start in unsupported:
            continue
        try:
            lines = gen_block(start, pairs, leaders, unsupported)
        except Unsupported as e:
            print('  block %04x not generated: %s' % (start, e))
            continue
        if lines is None:
            continue
        out.extend(lines)
        block_pcs.append(start)

    # oldvi_ locals are declared inside per-pair braces; blocks reference the
    # previous pair's copy, so hoist: declare them at function scope instead.
    text = '\n'.join(out)
    text = hoist_oldvi(text)
    out = [text]

    out.append('} // namespace')
    # tables
    out.append('const PairFn kSteps[%d] = {' % (CODE_SIZE // 8))
    row = []
    for pc in range(0, CODE_SIZE, 8):
        row.append('nullptr' if pc in unsupported else 'step_%04x' % pc)
    for i in range(0, len(row), 8):
        out.append('  ' + ', '.join(row[i:i + 8]) + ',')
    out.append('};')
    out.append('using BlockFn = uint32_t (*)(Ctx &);')
    out.append('const BlockFn kBlocks[%d] = {' % (CODE_SIZE // 8))
    bset = set(block_pcs)
    row = ['block_%04x' % pc if pc in bset else 'nullptr' for pc in range(0, CODE_SIZE, 8)]
    for i in range(0, len(row), 8):
        out.append('  ' + ', '.join(row[i:i + 8]) + ',')
    out.append('};')
    ebits = [pc for pc in range(0, CODE_SIZE, 8) if pairs[pc].e_bit and pc not in unsupported]
    out.append('const uint8_t kEbit[%d] = {' % (CODE_SIZE // 8))
    row = ['1' if pc in ebits else '0' for pc in range(0, CODE_SIZE, 8)]
    for i in range(0, len(row), 32):
        out.append('  ' + ', '.join(row[i:i + 32]) + ',')
    out.append('};')
    out.append(RUN_TEMPLATE.replace('0x3FF8u', '0x%Xu' % (MASK & ~7)).replace('0x3FFFu', '0x%Xu' % MASK))
    out.append('}} // namespace ps2vu1t::%s' % ns)
    return h, ns, '\n'.join(out) + '\n', unsupported, len(block_pcs)


RUN_TEMPLATE = r'''
// Runs from startPc to the end of the program: blocks where one starts,
// pair stepping (the interpreter's branch/delay-slot/E-bit rules) elsewhere.
bool startsSupported(uint32_t startPc) { return kSteps[(startPc & 0x3FF8u) / 8u] != nullptr; }

uint32_t run(Ctx &c, uint32_t startPc)
{
    VU1State &s = *c.s;
    uint32_t pc = startPc & 0x3FF8u;
    if (!kSteps[pc / 8u])
        return 0u;
    bool endAfterThis = false; // VU1State::ebit
    c.pendingEndAfterNext = false;
    for (uint64_t guard = 0;; ++guard)
    {
        if (guard > 100000000ull)
        {
            ++c.errors; // runaway program
            s.pc = pc;
            return 2u;
        }
        if (!s.branchPending && !endAfterThis && !c.pendingEndAfterNext && kBlocks[pc / 8u])
        {
            pc = kBlocks[pc / 8u](c);
            ++c.blockRuns;
            if (pc == kEnd)
                return 1u;
            continue;
        }
        const PairFn step = kSteps[pc / 8u];
        if (!step)
        {
            ++c.errors;
            s.pc = pc;
            return 2u;
        }
        ++c.steppedPairs;
        c.brTaken = false;
        step(c);
        if (c.pendingEndAfterNext)
        {
            // a block's delay slot had the E bit: the program ends after this pair
            c.pendingEndAfterNext = false;
            endAfterThis = true;
        }
        // VU1Interpreter::run: the branch is set while the pair executes, then
        // the pc advances through the delay slot.
        if (c.brTaken)
        {
            s.branchPending = true;
            s.branchTarget = c.brTarget;
            s.branchDelay = 1u;
        }
        uint32_t next = (pc + 8u) & 0x3FFFu;
        if (s.branchPending)
        {
            if (s.branchDelay == 0u)
            {
                next = s.branchTarget;
                s.branchPending = false;
            }
            else
                --s.branchDelay;
        }
        if (endAfterThis)
        {
            s.pc = next;
            return 1u;
        }
        if (kEbit[pc / 8u])
            endAfterThis = true;
        pc = next;
    }
}
'''


def hoist_oldvi(text):
    # "const int32_t oldvi_x = s.vi[n];" declared inside nested braces but read
    # by the following pair: turn declarations into assignments to function-
    # scope variables declared at the top of each function.
    funcs = re.split(r'(?m)^(?=(?:void step_|uint32_t block_))', text)
    res = []
    for f in funcs:
        names = sorted(set(re.findall(r'const int32_t (oldvi_[0-9a-f]+) = ', f)))
        if names:
            f = re.sub(r'const int32_t (oldvi_[0-9a-f]+) = ', r'\1 = ', f)
            f = f.replace('VU1State &s = *c.s;\n', 'VU1State &s = *c.s;\nint32_t %s;\n' % ', '.join(n + ' = 0' for n in names), 1)
        res.append(f)
    return ''.join(res)


def write_registry(out_dir, unit='vu1'):
    """Registry of every generated image of one unit: vu1_images.cpp
    (kTranslatedImages) or vu0_images.cpp (kTranslatedVu0Images)."""
    if unit == 'vu0':
        prefix, ns, table = 'vu0_image_', 'v0img_', 'kTranslatedVu0Images'
    else:
        prefix, ns, table = 'vu1_image_', 'img_', 'kTranslatedImages'
    count = table.replace('Images', 'ImageCount')
    images = []
    for path in sorted(glob.glob(os.path.join(out_dir, prefix + '*.cpp'))):
        m = re.search(prefix + r'([0-9a-f]{16})\.cpp$', path)
        if m:
            images.append(m.group(1))
    L = ['// Generated by gt3-tools/vu1_translate.py. Do not edit.', '#include "runtime/ps2_vu1_translated.h"', '',
         'namespace ps2vu1t {']
    for h in images:
        L.append('namespace %s%s { uint32_t run(Ctx &c, uint32_t startPc); }' % (ns, h))
    L.append('extern const TranslatedImage %s[];' % table)
    L.append('extern const uint32_t %s;' % count)
    L.append('const TranslatedImage %s[] = {' % table)
    for h in images:
        L.append('    {0x%sull, &%s%s::run, "%s"},' % (h, ns, h, h))
    if not images:
        L.append('    {0ull, nullptr, ""},')
    L.append('};')
    L.append('const uint32_t %s = %d;' % (count, len(images)))
    L.append('}')
    with open(os.path.join(out_dir, '%s_images.cpp' % unit), 'w', newline='\n') as f:
        f.write('\n'.join(L) + '\n')


def recorded_entries(path):
    d = open(path, 'rb').read()
    pos = 8
    pcs = set()
    while pos + 32 <= len(d):
        hdr = struct.unpack_from('<8I', d, pos)
        if hdr[0] != 0x4B43494B:
            break
        pcs.add(hdr[2] & 0x3FF8)
        pos += 32 + 8 + 664 + (16384 if hdr[6] else 0) + 16384 + 8 + 664 + 32
    return pcs


def main():
    global UNIT, CODE_SIZE, MASK, DATA_MASK
    args = sys.argv[1:]
    if '--vu0' in args:
        # VU0 microprograms: a raw 4 KiB code image (GT3_VU0_RECORD output).
        args.remove('--vu0')
        UNIT, CODE_SIZE, MASK, DATA_MASK = 'vu0', 0x1000, 0xFFF, 0xFFF
    entries = set()
    if '--entries' in args:
        # extra block leaders, e.g. the VCALLMS start pcs counted by GT3_VU0_RECORD
        i = args.index('--entries')
        entries = {int(x, 16) & MASK for x in args[i + 1].split(',') if x}
        del args[i:i + 2]
    src, out_dir = args[0], args[1]
    extra = set(entries)
    if UNIT == 'vu1' and src.endswith('.bin') and open(src, 'rb').read(8) == b'GT3VU1K1':
        extra |= recorded_entries(src)
    code = load_code(src) if UNIT == 'vu1' else open(src, 'rb').read()[:CODE_SIZE]
    if len(code) != CODE_SIZE:
        raise SystemExit('code image must be %d bytes' % CODE_SIZE)
    if UNIT == 'vu0':
        # a program usually starts after the previous one's E-bit pair and its delay slot
        extra.add(0)
        for pc in range(0, CODE_SIZE, 8):
            if decode_pair(code, pc).e_bit:
                extra.add((pc + 16) & MASK)
    h, ns, text, unsupported, nblocks = gen_image(code, os.path.basename(src), extra)
    os.makedirs(out_dir, exist_ok=True)
    path = os.path.join(out_dir, '%s_image_%016x.cpp' % (UNIT, h))
    with open(path, 'w', newline='\n') as f:
        f.write(text)
    write_registry(out_dir, UNIT)
    print('image %016x: %d blocks, %d unsupported pairs -> %s' % (h, nblocks, len(unsupported), path))
    for pc, why in sorted(unsupported.items())[:20]:
        if (code[pc:pc + 8] != b'\0' * 8):
            print('  unsupported %04x: %s' % (pc, why))


if __name__ == '__main__':
    main()
