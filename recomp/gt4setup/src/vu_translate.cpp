// vu-translate: port of recomp/gt3-tools/vu1_translate.py (with vu1_decode.py).
// Translates a VU1 (or, with --vu0, VU0) microcode image to C++ for
// ps2xRuntime/include/runtime/ps2_vu1_translated.h. The output is byte-identical
// to the Python generator's. Writes <out_dir>/<unit>_image_<hash>.cpp and
// regenerates <out_dir>/<unit>_images.cpp (the registry of every image there).
//
// Generation options are read from the same environment variables as the Python
// script (GT3_VU1T_SIMD, GT3_VU1T_ZERO_FAST, GT3_VU1T_STICKY, GT3_VU1T_SIMD_MINMAX,
// GT3_VU1T_NORM_SKIP, GT3_VU1T_VEC_STORE, GT3_VU1T_LOCALS, GT3_VU1T_WHOLE_READY);
// --no-sticky is the same as GT3_VU1T_STICKY=0.
#include "util.h"
#include "vu_decode.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <map>
#include <set>

namespace gt4setup {

namespace {

using namespace vu;
using Lines = std::vector<std::string>;

struct Unsupported {
    std::string why;
};

bool envFlag(const char *name, const char *def)
{
    const char *v = std::getenv(name);
    return std::string(v ? v : def) != "0";
}

uint64_t fnv64(const Bytes &data)
{
    uint64_t h = 1469598103934665603ull;
    for (uint8_t b : data)
        h = (h ^ b) * 1099511628211ull;
    return h;
}

std::vector<int> lanesOf(int dest)
{
    std::vector<int> v;
    for (int k = 0; k < 4; ++k)
        if (dest & lane(k))
            v.push_back(k);
    return v;
}

int destBits(int dest)
{
    int b = 0;
    for (int k : lanesOf(dest))
        b |= 1 << k;
    return b;
}

std::string join(const Lines &v, const std::string &sep)
{
    std::string s;
    for (size_t i = 0; i < v.size(); ++i) {
        if (i)
            s += sep;
        s += v[i];
    }
    return s;
}

bool oneOf(const std::string &n, std::initializer_list<const char *> set)
{
    for (const char *x : set)
        if (n == x)
            return true;
    return false;
}

// ------------------------------------------------------------------ upper ops
enum Kind { K_ADD, K_SUB, K_MADD, K_MSUB, K_MAX, K_MINI, K_MUL, K_OPMSUB, K_OPMULA, K_ITOF, K_FTOI, K_ABS, K_CLIP, K_NOP };
enum OpType { O_NONE, O_BC, O_Q, O_I, O_VT, O_CONV };
constexpr int T_NONE = -1, T_ACC = -2; // else a VF register

struct UpperInfo {
    Kind kind;
    OpType opt;
    int arg; // bc lane (O_BC) or conversion index (O_CONV)
    int target;
    bool productSum;
};

bool isFmacKind(Kind k) { return k == K_ADD || k == K_SUB || k == K_MADD || k == K_MSUB || k == K_MUL || k == K_OPMSUB || k == K_OPMULA; }
bool isNormalisedKind(Kind k)
{
    return k == K_ADD || k == K_SUB || k == K_MUL || k == K_MADD || k == K_MSUB || k == K_OPMULA || k == K_OPMSUB ||
           k == K_MAX || k == K_MINI || k == K_ABS || k == K_ITOF;
}

UpperInfo upperInfo(uint32_t u)
{
    const int op = u & 0x3F;
    const int ft = FT(u), fd = FD(u);
    if (op < 0x3C) {
        if (op <= 0x1B) {
            static const Kind kinds[] = {K_ADD, K_SUB, K_MADD, K_MSUB, K_MAX, K_MINI, K_MUL};
            const Kind kind = kinds[op >> 2];
            return {kind, O_BC, op & 3, fd, kind == K_MADD || kind == K_MSUB};
        }
        Kind kind;
        OpType o;
        switch (op) {
        case 0x1C: kind = K_MUL; o = O_Q; break;
        case 0x1D: kind = K_MAX; o = O_I; break;
        case 0x1E: kind = K_MUL; o = O_I; break;
        case 0x1F: kind = K_MINI; o = O_I; break;
        case 0x20: kind = K_ADD; o = O_Q; break;
        case 0x21: kind = K_MADD; o = O_Q; break;
        case 0x22: kind = K_ADD; o = O_I; break;
        case 0x23: kind = K_MADD; o = O_I; break;
        case 0x24: kind = K_SUB; o = O_Q; break;
        case 0x25: kind = K_MSUB; o = O_Q; break;
        case 0x26: kind = K_SUB; o = O_I; break;
        case 0x27: kind = K_MSUB; o = O_I; break;
        case 0x28: kind = K_ADD; o = O_VT; break;
        case 0x29: kind = K_MADD; o = O_VT; break;
        case 0x2A: kind = K_MUL; o = O_VT; break;
        case 0x2B: kind = K_MAX; o = O_VT; break;
        case 0x2C: kind = K_SUB; o = O_VT; break;
        case 0x2D: kind = K_MSUB; o = O_VT; break;
        case 0x2E: kind = K_OPMSUB; o = O_VT; break;
        case 0x2F: kind = K_MINI; o = O_VT; break;
        default: throw Unsupported{strf("upper op %x", op)};
        }
        const bool ps = op == 0x21 || op == 0x23 || op == 0x25 || op == 0x27 || op == 0x29 || op == 0x2D || op == 0x2E;
        return {kind, o, 0, fd, ps};
    }
    const int sp = (u & 3) | ((u >> 4) & 0x7C);
    if (sp <= 0x0F) {
        static const Kind kinds[] = {K_ADD, K_SUB, K_MADD, K_MSUB};
        const Kind kind = kinds[sp >> 2];
        return {kind, O_BC, sp & 3, T_ACC, kind == K_MADD || kind == K_MSUB};
    }
    if (0x10 <= sp && sp <= 0x13)
        return {K_ITOF, O_CONV, sp - 0x10, ft, false};
    if (0x14 <= sp && sp <= 0x17)
        return {K_FTOI, O_CONV, sp - 0x14, ft, false};
    if (0x18 <= sp && sp <= 0x1B)
        return {K_MUL, O_BC, sp & 3, T_ACC, false};
    Kind kind = K_NOP;
    OpType o = O_NONE;
    bool found = true;
    switch (sp) {
    case 0x1C: kind = K_MUL; o = O_Q; break;
    case 0x1E: kind = K_MUL; o = O_I; break;
    case 0x20: kind = K_ADD; o = O_Q; break;
    case 0x21: kind = K_MADD; o = O_Q; break;
    case 0x22: kind = K_ADD; o = O_I; break;
    case 0x23: kind = K_MADD; o = O_I; break;
    case 0x24: kind = K_SUB; o = O_Q; break;
    case 0x25: kind = K_MSUB; o = O_Q; break;
    case 0x26: kind = K_SUB; o = O_I; break;
    case 0x27: kind = K_MSUB; o = O_I; break;
    case 0x28: kind = K_ADD; o = O_VT; break;
    case 0x29: kind = K_MADD; o = O_VT; break;
    case 0x2A: kind = K_MUL; o = O_VT; break;
    case 0x2C: kind = K_SUB; o = O_VT; break;
    case 0x2D: kind = K_MSUB; o = O_VT; break;
    case 0x2E: kind = K_OPMULA; o = O_VT; break;
    default: found = false; break;
    }
    if (found) {
        const bool ps = sp == 0x21 || sp == 0x23 || sp == 0x25 || sp == 0x27 || sp == 0x29 || sp == 0x2D;
        return {kind, o, 0, T_ACC, ps};
    }
    if (sp == 0x1D)
        return {K_ABS, O_NONE, 0, ft, false};
    if (sp == 0x1F)
        return {K_CLIP, O_NONE, 0, T_NONE, false};
    if (sp == 0x2F || sp == 0x30)
        return {K_NOP, O_NONE, 0, T_NONE, false};
    throw Unsupported{strf("upper special %x", sp)};
}

const int CROSS_L[4] = {1, 2, 0, 3};
const int CROSS_R[4] = {2, 0, 1, 3};

// register keys: VF reg r lane k -> r*4+k; VI reg r -> 128+r
constexpr int NKEYS = 144;
int vfKey(int reg, int k) { return reg * 4 + k; }
int viKey(int reg) { return 128 + reg; }

std::string readyKey(int key)
{
    if (key < 128)
        return strf("c.vfReady[%d][%d]", key / 4, key % 4);
    return strf("c.viReady[%d]", key - 128);
}

struct Br {
    enum Type { NONE, B, COND, JR } type = NONE;
    std::string expr;
    uint32_t target = 0;
};

struct Timing {
    std::set<int> need, marks;
    int lastLat = 0;
};

struct Term {
    enum Type { FALL, BRANCH, END } type;
    uint32_t pc;
};

using BranchVi = std::function<std::string(int)>;

class Translator {
public:
    // unit
    bool vu0 = false;
    uint32_t codeSize = 0x4000, mask = 0x3FFF, dataMask = 0x3FFF;
    // options
    bool simdFmac = envFlag("GT3_VU1T_SIMD", "1");
    bool zeroFast = envFlag("GT3_VU1T_ZERO_FAST", "1");
    bool keepSticky = envFlag("GT3_VU1T_STICKY", "1");
    bool simdMinmax = envFlag("GT3_VU1T_SIMD_MINMAX", "1");
    bool normSkip = envFlag("GT3_VU1T_NORM_SKIP", "1");
    bool vecStores = envFlag("GT3_VU1T_VEC_STORE", "1");
    bool locals = [] {
        const char *v = std::getenv("GT3_VU1T_LOCALS");
        return std::string(v ? v : "0") == "1";
    }();
    bool wholeReady = envFlag("GT3_VU1T_WHOLE_READY", "1");

    // per-image state
    bool deadFlagsOk = false;
    int norm[33] = {}; // VF 0..31, 32 = ACC
    std::vector<std::pair<int, int>> lowerVfWrites;
    bool upperVec = false;
    std::vector<Pair> pairs;
    std::set<uint32_t> leaders;
    std::map<uint32_t, std::string> unsupported;
    bool haveEntryPending = false;
    std::vector<std::array<uint8_t, NKEYS>> entryPending;
    std::vector<bool> liveMac;

    int knownLanes(int reg) const { return normSkip ? norm[reg] : 0; }
    static constexpr int ACC = 32;
    uint32_t npairs() const { return codeSize / 8; }
    const Pair &at(uint32_t pc) const { return pairs[pc / 8]; }
    bool isUnsupported(uint32_t pc) const { return unsupported.count(pc) != 0; }

    // ---------------------------------------------------------------- upper
    std::string emitFmacSimd(Lines &L, Kind kind, OpType opt, int bc, bool productSum, const std::vector<int> &lines,
                             int fs, int ft, bool needFlags)
    {
        int lanes = 0;
        for (int k : lines)
            lanes |= 1 << k;
        auto vec = [&](int reg) -> std::string {
            if (reg == 0)
                return "_mm_setr_ps(0.0f, 0.0f, 0.0f, 1.0f)";
            if ((knownLanes(reg) & lanes) == lanes)
                return strf("_mm_loadu_ps(s.vf[%d])", reg);
            return strf("loadNV(s.vf[%d])", reg);
        };
        L.push_back("const __m128 vsv = " + vec(fs) + ";");
        if (opt == O_Q) {
            L.push_back("const __m128 vtv = _mm_set1_ps(N(s.q));");
        } else if (opt == O_I) {
            L.push_back("const __m128 vtv = _mm_set1_ps(N(s.i));");
        } else if (opt == O_BC) {
            if (ft == 0)
                L.push_back(strf("const __m128 vtv = _mm_set1_ps(%s);", bc == 3 ? "1.0f" : "0.0f"));
            else if (knownLanes(ft) & (1 << bc))
                L.push_back(strf("const __m128 vtv = _mm_set1_ps(s.vf[%d][%d]);", ft, bc));
            else
                L.push_back(strf("const __m128 vtv = _mm_set1_ps(N(s.vf[%d][%d]));", ft, bc));
        } else {
            L.push_back("const __m128 vtv = " + vec(ft) + ";");
        }
        const bool madd = kind == K_MADD || kind == K_MSUB;
        if (madd) {
            L.push_back(strf("const __m128 acv = %s;",
                             (knownLanes(ACC) & lanes) == lanes ? "_mm_loadu_ps(s.acc)" : "loadNV(s.acc)"));
            L.push_back("const __m128 prv = _mm_mul_ps(vsv, vtv);");
            L.push_back(strf("const __m128 rv = %s(acv, prv);", kind == K_MADD ? "_mm_add_ps" : "_mm_sub_ps"));
        } else {
            L.push_back(strf("const __m128 rv = %s(vsv, vtv);",
                             kind == K_ADD ? "_mm_add_ps" : kind == K_SUB ? "_mm_sub_ps" : "_mm_mul_ps"));
        }
        const char *fx = kind == K_ADD ? "fxAdd" : kind == K_SUB ? "fxSub" : "fxMul";
        if (!needFlags) {
            L.push_back("alignas(16) float urv[4]; _mm_store_ps(urv, rv);");
            if (madd) {
                const char *fn = kind == K_MADD ? "fxMadd" : "fxMsub";
                L.push_back(strf("if (!keepMaddV(rv, prv, acv, vsv, vtv, %d)) {", lanes));
                L.push_back("  alignas(16) float sa[4], ta[4], aa[4], pa[4];");
                L.push_back("  _mm_store_ps(sa, vsv); _mm_store_ps(ta, vtv); _mm_store_ps(aa, acv); _mm_store_ps(pa, prv);");
                for (int k : lines)
                    L.push_back(strf("  (void)%s(urv[%d], pa[%d], aa[%d], sa[%d], ta[%d]);", fn, k, k, k, k, k));
            } else {
                const char *keep = kind == K_ADD ? "keepAddV" : kind == K_SUB ? "keepSubV" : "keepMulV";
                L.push_back(strf("if (!%s(rv, vsv, vtv, %d)) {", keep, lanes));
                L.push_back("  alignas(16) float sa[4], ta[4]; _mm_store_ps(sa, vsv); _mm_store_ps(ta, vtv);");
                for (int k : lines)
                    L.push_back(strf("  (void)%s(urv[%d], sa[%d], ta[%d]);", fx, k, k, k));
            }
            L.push_back("}");
            for (int k : lines)
                L.push_back(strf("float ur%d = urv[%d];", k, k));
            return "";
        }
        L.push_back("alignas(16) float urv[4]; _mm_store_ps(urv, rv); uint8_t ufv[4];");
        if (madd) {
            const char *fn = kind == K_MADD ? "fxMadd" : "fxMsub";
            if (zeroFast)
                L.push_back(strf("if (!fastMaddZV(rv, prv, acv, vsv, vtv, %d, ufv)) {", lanes));
            else
                L.push_back(strf("if (!fastMaddV(rv, prv, %d, ufv)) {", lanes));
            L.push_back("  alignas(16) float sa[4], ta[4], aa[4], pa[4];");
            L.push_back("  _mm_store_ps(sa, vsv); _mm_store_ps(ta, vtv); _mm_store_ps(aa, acv); _mm_store_ps(pa, prv);");
            for (int k : lines)
                L.push_back(strf("  ufv[%d] = %s(urv[%d], pa[%d], aa[%d], sa[%d], ta[%d]);", k, fn, k, k, k, k, k));
            L.push_back("}");
        } else {
            if (zeroFast) {
                const char *fast = kind == K_ADD ? "fastAddV" : kind == K_SUB ? "fastSubV" : "fastMulV";
                L.push_back(strf("if (!%s(rv, vsv, vtv, %d, ufv)) {", fast, lanes));
            } else {
                L.push_back(strf("if (!fastV(rv, %d, ufv)) {", lanes));
            }
            L.push_back("  alignas(16) float sa[4], ta[4]; _mm_store_ps(sa, vsv); _mm_store_ps(ta, vtv);");
            for (int k : lines)
                L.push_back(strf("  ufv[%d] = %s(urv[%d], sa[%d], ta[%d]);", k, fx, k, k, k));
            L.push_back("}");
        }
        for (int k : lines)
            L.push_back(strf("float ur%d = urv[%d]; const uint8_t uf%d = ufv[%d];", k, k, k, k));
        if (productSum) {
            if (!madd)
                throw Unsupported{"product-sum without a product"};
            L.push_back(strf("const uint32_t xs = prodFlagsV(prv, vsv, vtv, %d);", lanes));
            return "xs";
        }
        return "0u";
    }

    std::string lanesVec(int dest, const std::array<std::string, 4> &values) const
    {
        std::vector<std::string> used;
        for (int k : lanesOf(dest))
            used.push_back(values[k]);
        if (!used.empty() && std::all_of(used.begin(), used.end(), [&](const std::string &v) { return v == used[0]; }))
            return "_mm_set1_ps(" + used[0] + ")";
        if (values[0] == "t[0]" && values[1] == "t[1]" && values[2] == "t[2]" && values[3] == "t[3]")
            return "_mm_loadu_ps(t)";
        Lines parts;
        for (int k = 0; k < 4; ++k)
            parts.push_back((dest & lane(k)) ? values[k] : "0.0f");
        return "_mm_setr_ps(" + join(parts, ", ") + ")";
    }

    std::string urVec(int dest) const
    {
        if (upperVec)
            return "_mm_load_ps(urv)";
        return lanesVec(dest, {"ur0", "ur1", "ur2", "ur3"});
    }

    void vecStore(Lines &L, const std::string &dst, int dest, const std::string &vec) const
    {
        const int m = destBits(dest);
        if (m == 0xF)
            L.push_back("_mm_storeu_ps(" + dst + ", " + vec + ");");
        else if (m)
            L.push_back("_mm_storeu_ps(" + dst + ", _mm_blend_ps(_mm_loadu_ps(" + dst + "), " + vec + ", " +
                        std::to_string(m) + "));");
    }

    void storeAcc(Lines &L, int dest)
    {
        if (vecStores) {
            vecStore(L, "s.acc", dest, urVec(dest));
        } else {
            for (int k : lanesOf(dest))
                L.push_back(strf("s.acc[%d] = ur%d;", k, k));
        }
    }

    // returns (target, dest); target T_NONE / T_ACC / VF register
    std::pair<int, int> emitUpper(const Pair &p, Lines &L, bool dropFlags)
    {
        const uint32_t u = p.upper;
        const UpperInfo info = upperInfo(u);
        const Kind kind = info.kind;
        const OpType opt = info.opt;
        const int target = info.target;
        const bool productSum = info.productSum;
        const int dest = DEST(u), fs = FS(u), ft = FT(u);
        const int op = u & 0x3F;
        upperVec = false;
        if (kind == K_NOP)
            return {T_NONE, 0};
        if (kind == K_CLIP) {
            L.push_back(strf("{ const uint32_t wb = floatBits(s.vf[%d][3]);", ft));
            L.push_back("  const int32_t lim = (wb & 0x7F800000u) != 0u ? static_cast<int32_t>(wb & 0x7FFFFFFFu) : 0x007FFFFF;");
            L.push_back("  uint32_t cf = 0u;");
            for (int k = 0; k < 3; ++k)
                L.push_back(strf("  { const uint32_t b = floatBits(s.vf[%d][%d]); if (static_cast<int32_t>(b) > lim) cf |= 0x%xu;"
                                 " if (static_cast<int32_t>(b ^ 0x80000000u) > lim) cf |= 0x%xu; }",
                                 fs, k, 1 << (2 * k), 2 << (2 * k)));
            L.push_back("  pushClip(c, cf); }");
            return {T_NONE, 0};
        }
        if (dest == 0 && !(kind == K_ITOF || kind == K_FTOI || kind == K_ABS || kind == K_MAX || kind == K_MINI))
            return {T_NONE, 0};
        if (opt == O_Q)
            L.push_back("commit(c);");
        const std::vector<int> lines = lanesOf(dest);
        if ((kind == K_OPMSUB || kind == K_OPMULA) && dest != 0xE)
            throw Unsupported{strf("opmsub/opmula with dest %x (undeclared cross reads)", dest)};

        auto S = [](int k) { return strf("vs%d", k); };
        auto T = [&](int k) -> std::string {
            if (opt == O_Q)
                return "qq";
            if (opt == O_I)
                return "ii";
            if (opt == O_BC)
                return strf("vt%d", info.arg);
            return strf("vt%d", k);
        };

        if (simdMinmax && (kind == K_MAX || kind == K_MINI) && !lines.empty()) {
            const int lanes = destBits(dest);
            auto mvec = [&](int reg) -> std::string {
                if (reg == 0)
                    return "_mm_setr_ps(0.0f, 0.0f, 0.0f, 1.0f)";
                if ((knownLanes(reg) & lanes) == lanes)
                    return strf("_mm_loadu_ps(s.vf[%d])", reg);
                return strf("loadNV(s.vf[%d])", reg);
            };
            std::string vt;
            if (opt == O_I) {
                vt = "_mm_set1_ps(N(s.i))";
            } else if (opt == O_BC) {
                const int bc = info.arg;
                if (ft == 0)
                    vt = strf("_mm_set1_ps(%s)", bc == 3 ? "1.0f" : "0.0f");
                else if (knownLanes(ft) & (1 << bc))
                    vt = strf("_mm_set1_ps(s.vf[%d][%d])", ft, bc);
                else
                    vt = strf("_mm_set1_ps(N(s.vf[%d][%d]))", ft, bc);
            } else {
                vt = mvec(ft);
            }
            L.push_back(strf("alignas(16) float urv[4]; _mm_store_ps(urv, %s(%s, %s));",
                             kind == K_MINI ? "_mm_min_ps" : "_mm_max_ps", mvec(fs).c_str(), vt.c_str()));
            for (int k : lines)
                L.push_back(strf("float ur%d = urv[%d];", k, k));
            upperVec = true;
            return {target, dest};
        }

        if (simdMinmax && kind == K_FTOI && !lines.empty() && fs != 0) {
            const int lanes = destBits(dest);
            static const char *const scales[] = {"1.0f", "16.0f", "4096.0f", "32768.0f"};
            const char *scale = scales[info.arg];
            const std::string src = (knownLanes(fs) & lanes) == lanes ? strf("_mm_loadu_ps(s.vf[%d])", fs)
                                                                     : strf("loadNV(s.vf[%d])", fs);
            L.push_back("alignas(16) float urv[4];");
            L.push_back(strf("{ const __m128 fm = _mm_mul_ps(%s, _mm_set1_ps(%s));", src.c_str(), scale));
            L.push_back("  const __m128i fi = _mm_blendv_epi8(_mm_cvttps_epi32(fm), _mm_set1_epi32(0x7FFFFFFF), "
                        "_mm_castps_si128(_mm_cmpge_ps(fm, _mm_set1_ps(2147483648.0f))));");
            L.push_back("  _mm_store_ps(urv, _mm_castsi128_ps(fi)); }");
            for (int k : lines)
                L.push_back(strf("float ur%d = urv[%d];", k, k));
            upperVec = true;
            return {target, dest};
        }

        if (simdMinmax && kind == K_ITOF && !lines.empty() && fs != 0) {
            static const char *const divs[] = {"", ", _mm_set1_ps(16.0f)", ", _mm_set1_ps(4096.0f)", ", _mm_set1_ps(32768.0f)"};
            const std::string div = divs[info.arg];
            const std::string conv =
                strf("_mm_cvtepi32_ps(_mm_loadu_si128(reinterpret_cast<const __m128i *>(s.vf[%d])))", fs);
            L.push_back("alignas(16) float urv[4]; _mm_store_ps(urv, " +
                        (!div.empty() ? "_mm_div_ps(" + conv + div + ")" : conv) + ");");
            for (int k : lines)
                L.push_back(strf("float ur%d = urv[%d];", k, k));
            upperVec = true;
            return {target, dest};
        }

        if (simdFmac && (kind == K_ADD || kind == K_SUB || kind == K_MUL || kind == K_MADD || kind == K_MSUB)) {
            const bool needFlags = !(dropFlags && !keepSticky);
            const std::string extra = emitFmacSimd(L, kind, opt, info.arg, productSum, lines, fs, ft, needFlags);
            upperVec = true;
            Lines macs, sts;
            for (int k : lines) {
                macs.push_back(strf("macBits(uf%d, 0x%xu)", k, lane(k)));
                sts.push_back(strf("uf%d", k));
            }
            const std::string mac = join(macs, " | "), st = join(sts, " | ");
            if (!needFlags) {
            } else if (dropFlags) {
                L.push_back("c.stickyAcc |= static_cast<uint32_t>(" + st + ") | " + extra + ";");
            } else {
                L.push_back("pushFlags(c, " + mac + ", static_cast<uint32_t>(" + st + "), " + extra + ");");
            }
            if (target == T_ACC) {
                storeAcc(L, dest);
                return {T_ACC, dest};
            }
            return {target, dest};
        }

        std::set<int> needVs, needVt, needAcc;
        for (int k : lines) {
            if (kind == K_OPMSUB || kind == K_OPMULA) {
                if (k < 3) {
                    needVs.insert(CROSS_L[k]);
                    needVt.insert(CROSS_R[k]);
                    if (kind == K_OPMSUB)
                        needAcc.insert(k);
                }
                continue;
            }
            needVs.insert(k);
            if (opt == O_BC)
                needVt.insert(info.arg);
            else if (opt == O_VT)
                needVt.insert(k);
            if (kind == K_MADD || kind == K_MSUB)
                needAcc.insert(k);
        }
        if (productSum) {
            for (int k : lines) {
                needVs.insert(op == 0x2E ? CROSS_L[k] : k);
                if (opt == O_BC)
                    needVt.insert(info.arg);
                else if (opt == O_VT)
                    needVt.insert(op == 0x2E ? CROSS_R[k] : k);
            }
        }
        if (kind == K_ITOF)
            needVs.clear();
        auto vfop = [&](int reg, int k) -> std::string {
            if (reg == 0)
                return k == 3 ? "1.0f" : "0.0f";
            if (knownLanes(reg) & (1 << k))
                return strf("s.vf[%d][%d]", reg, k);
            return strf("N(s.vf[%d][%d])", reg, k);
        };
        for (int k : needVs)
            L.push_back(strf("const float vs%d = %s;", k, vfop(fs, k).c_str()));
        for (int k : needVt)
            L.push_back(strf("const float vt%d = %s;", k, vfop(ft, k).c_str()));
        for (int k : needAcc)
            L.push_back(strf("const float ac%d = %s;", k,
                             (knownLanes(ACC) & (1 << k)) ? strf("s.acc[%d]", k).c_str() : strf("N(s.acc[%d])", k).c_str()));
        if (opt == O_Q)
            L.push_back("const float qq = N(s.q);");
        if (opt == O_I)
            L.push_back("const float ii = N(s.i);");

        const bool fmac = isFmacKind(kind);
        struct Prod {
            std::string pv, a, b;
        };
        std::map<int, Prod> prod;
        for (int k : lines) {
            if ((kind == K_OPMSUB || kind == K_OPMULA) && k == 3) {
                L.push_back("float ur3 = 0.0f;");
                L.push_back("const uint8_t uf3 = fx(ur3, 0.0);");
            } else if (kind == K_ADD) {
                L.push_back(strf("float ur%d = %s + %s;", k, S(k).c_str(), T(k).c_str()));
                L.push_back(strf("const uint8_t uf%d = fxAdd(ur%d, %s, %s);", k, k, S(k).c_str(), T(k).c_str()));
            } else if (kind == K_SUB) {
                L.push_back(strf("float ur%d = %s - %s;", k, S(k).c_str(), T(k).c_str()));
                L.push_back(strf("const uint8_t uf%d = fxSub(ur%d, %s, %s);", k, k, S(k).c_str(), T(k).c_str()));
            } else if (kind == K_MUL) {
                L.push_back(strf("float ur%d = %s * %s;", k, S(k).c_str(), T(k).c_str()));
                L.push_back(strf("const uint8_t uf%d = fxMul(ur%d, %s, %s);", k, k, S(k).c_str(), T(k).c_str()));
            } else if (kind == K_OPMULA) {
                const std::string a = strf("vs%d", CROSS_L[k]), b = strf("vt%d", CROSS_R[k]);
                L.push_back(strf("float ur%d = %s * %s;", k, a.c_str(), b.c_str()));
                L.push_back(strf("const uint8_t uf%d = fxMul(ur%d, %s, %s);", k, k, a.c_str(), b.c_str()));
            } else if (kind == K_MADD || kind == K_MSUB || kind == K_OPMSUB) {
                std::string a, b;
                if (kind == K_OPMSUB) {
                    a = strf("vs%d", CROSS_L[k]);
                    b = strf("vt%d", CROSS_R[k]);
                } else {
                    a = S(k);
                    b = T(k);
                }
                const char *sign = kind == K_MADD ? "+" : "-";
                const char *fn = kind == K_MADD ? "fxMadd" : "fxMsub";
                L.push_back(strf("const float pr%d = %s * %s;", k, a.c_str(), b.c_str()));
                L.push_back(strf("float ur%d = ac%d %s pr%d;", k, k, sign, k));
                L.push_back(strf("const uint8_t uf%d = %s(ur%d, pr%d, ac%d, %s, %s);", k, fn, k, k, k, a.c_str(), b.c_str()));
                prod[k] = {strf("pr%d", k), a, b};
            } else if (kind == K_MAX) {
                L.push_back(strf("float ur%d = (%s > %s) ? %s : %s;", k, S(k).c_str(), T(k).c_str(), S(k).c_str(), T(k).c_str()));
            } else if (kind == K_MINI) {
                L.push_back(strf("float ur%d = (%s < %s) ? %s : %s;", k, S(k).c_str(), T(k).c_str(), S(k).c_str(), T(k).c_str()));
            } else if (kind == K_ITOF) {
                static const char *const divs[] = {"", " / 16.0f", " / 4096.0f", " / 32768.0f"};
                L.push_back(strf("float ur%d = static_cast<float>(static_cast<int32_t>(floatBits(s.vf[%d][%d])))%s;", k, fs,
                                 k, divs[info.arg]));
            } else if (kind == K_FTOI) {
                static const char *const scales[] = {"1.0f", "16.0f", "4096.0f", "32768.0f"};
                L.push_back(strf("float ur%d = bitsToFloat(static_cast<uint32_t>(floatToInt(vs%d, %s)));", k, k,
                                 scales[info.arg]));
            } else if (kind == K_ABS) {
                L.push_back(strf("float ur%d = std::fabs(vs%d);", k, k));
            } else {
                throw Unsupported{"kind"};
            }
        }
        if (fmac) {
            std::string extra = "0u";
            if (productSum) {
                Lines parts;
                for (int k : lines) {
                    const auto it = prod.find(k);
                    if (it == prod.end())
                        throw Unsupported{"product-sum lane without product"};
                    parts.push_back("static_cast<uint32_t>(prodFlags(" + it->second.pv + ", " + it->second.a + ", " +
                                    it->second.b + "))");
                }
                L.push_back("const uint32_t xs = " + join(parts, " | ") + ";");
                extra = "xs";
            }
            Lines macs, sts;
            for (int k : lines) {
                macs.push_back(strf("macBits(uf%d, 0x%xu)", k, lane(k)));
                sts.push_back(strf("uf%d", k));
            }
            if (dropFlags)
                L.push_back("c.stickyAcc |= static_cast<uint32_t>(" + join(sts, " | ") + ") | " + extra + ";");
            else
                L.push_back("pushFlags(c, " + join(macs, " | ") + ", static_cast<uint32_t>(" + join(sts, " | ") + "), " +
                            extra + ");");
        }
        if (target == T_ACC) {
            storeAcc(L, dest);
            return {T_ACC, dest};
        }
        return {target, dest};
    }

    // ---------------------------------------------------------------- lower
    std::string addrImm(int base, int imm) const
    {
        return strf("((static_cast<uint32_t>(static_cast<int32_t>(s.vi[%d] + (%d))) * 16u) & 0x%XFu)", base, imm,
                    dataMask >> 4);
    }
    std::string addrReg(int base) const
    {
        return strf("((static_cast<uint32_t>(static_cast<uint16_t>(s.vi[%d])) * 16u) & 0x%XFu)", base, dataMask >> 4);
    }
    static int firstLane(int dest)
    {
        if (dest & 8)
            return 0;
        if (dest & 4)
            return 1;
        if (dest & 2)
            return 2;
        return 3;
    }

    void vfWrite(Lines &L, int reg, int dest, const std::array<std::string, 4> &values, bool skip)
    {
        if (reg == 0 || skip)
            return;
        lowerVfWrites.push_back({reg, dest});
        if (vecStores) {
            vecStore(L, strf("s.vf[%d]", reg), dest, lanesVec(dest, values));
            return;
        }
        for (int k : lanesOf(dest))
            L.push_back(strf("s.vf[%d][%d] = %s;", reg, k, values[k].c_str()));
    }

    Br emitLower(const Pair &p, Lines &L, const BranchVi &branchVi, bool skipVf)
    {
        const uint32_t l = p.lower;
        Br br;
        if (p.iBit || l == 0 || l == 0x8000033C)
            return br;
        const uint32_t pc = p.pc;
        const std::string n = lowerName(l);
        const int dest = DEST(l);
        const int vfT = FT(l), vfS = FS(l), viT = VIT(l), viS = VIS(l), viD = VID(l);
        const int imm11 = IMM11(l);
        const uint32_t target = static_cast<uint32_t>(static_cast<int>(pc) + 8 + imm11 * 8) & mask;
        const uint32_t link = (pc + 16) / 8;
        const std::array<std::string, 4> tv = {"t[0]", "t[1]", "t[2]", "t[3]"};
        const std::array<std::string, 4> t1 = {"t", "t", "t", "t"};
        if (n == "lq") {
            L.push_back("{ const uint32_t a = " + addrImm(viS, imm11) + "; float t[4]; std::memcpy(t, c.mem + a, 16);");
            vfWrite(L, vfT, dest, tv, skipVf);
            L.push_back("}");
        } else if (n == "sq") {
            L.push_back(strf("{ uint32_t w[4]; std::memcpy(w, s.vf[%d], 16); store(c, %s, w, 0x%xu); }", vfS,
                             addrImm(viT, imm11).c_str(), dest));
        } else if (n == "ilw") {
            L.push_back(strf("{ uint32_t v; std::memcpy(&v, c.mem + %s + %d, 4);", addrImm(viS, imm11).c_str(), firstLane(dest) * 4));
            if (viT)
                L.push_back(strf("  s.vi[%d] = static_cast<int16_t>(v & 0xFFFFu);", viT));
            L.push_back("}");
        } else if (n == "isw") {
            L.push_back(strf("{ const uint32_t v = static_cast<uint16_t>(s.vi[%d] & 0xFFFF); const uint32_t w[4] = {v, v, v, v}; store(c, %s, w, 0x%xu); }",
                             viT, addrImm(viS, imm11).c_str(), dest));
        } else if (n == "iaddiu" || n == "isubiu") {
            const uint32_t imm = (l & 0x7FF) | ((l >> 10) & 0x7800);
            if (viT)
                L.push_back(strf("s.vi[%d] = static_cast<int16_t>(s.vi[%d] %s %u);", viT, viS, n == "iaddiu" ? "+" : "-", imm));
        } else if (oneOf(n, {"fceq", "fcand", "fcor"})) {
            const uint32_t imm = l & 0xFFFFFF;
            L.push_back("commit(c);");
            if (n == "fceq")
                L.push_back(strf("s.vi[1] = ((s.clip & 0xFFFFFFu) == 0x%xu) ? 1 : 0;", imm));
            else if (n == "fcand")
                L.push_back(strf("s.vi[1] = ((s.clip & 0x%xu) != 0u) ? 1 : 0;", imm));
            else
                L.push_back(strf("s.vi[1] = ((s.clip | 0x%xu) == 0xFFFFFFu) ? 1 : 0;", imm));
        } else if (n == "fcset") {
            L.push_back(strf("pushFcset(c, 0x%xu);", l & 0xFFFFFF));
        } else if (oneOf(n, {"fseq", "fsand", "fsor"})) {
            const uint32_t imm = (((l >> 21) & 1) << 11) | (l & 0x7FF);
            L.push_back("commit(c);");
            if (viT) {
                if (n == "fseq")
                    L.push_back(strf("s.vi[%d] = ((s.status & 0xFFFu) == 0x%xu) ? 1 : 0;", viT, imm));
                else if (n == "fsand")
                    L.push_back(strf("s.vi[%d] = static_cast<int32_t>((s.status & 0xFFFu) & 0x%xu);", viT, imm));
                else
                    L.push_back(strf("s.vi[%d] = static_cast<int32_t>((s.status & 0xFFFu) | 0x%xu);", viT, imm));
            }
        } else if (n == "fsset") {
            L.push_back(strf("pushFsset(c, 0x%xu);", (((l >> 21) & 1) << 11) | (l & 0x7FF)));
        } else if (oneOf(n, {"fmeq", "fmand", "fmor"})) {
            L.push_back("commit(c);");
            if (viT) {
                if (n == "fmeq")
                    L.push_back(strf("s.vi[%d] = ((s.mac & 0xFFFFu) == static_cast<uint32_t>(static_cast<uint16_t>(s.vi[%d]))) ? 1 : 0;", viT, viS));
                else if (n == "fmand")
                    L.push_back(strf("s.vi[%d] = static_cast<int32_t>(s.mac & static_cast<uint32_t>(static_cast<uint16_t>(s.vi[%d])));", viT, viS));
                else
                    L.push_back(strf("s.vi[%d] = static_cast<int32_t>(s.mac | static_cast<uint32_t>(static_cast<uint16_t>(s.vi[%d])));", viT, viS));
            }
        } else if (n == "fcget") {
            L.push_back("commit(c);");
            if (viT)
                L.push_back(strf("s.vi[%d] = static_cast<int32_t>(s.clip & 0x0FFFu);", viT));
        } else if (n == "b") {
            br.type = Br::B;
            br.target = target;
            return br;
        } else if (n == "bal") {
            if (viT)
                L.push_back(strf("s.vi[%d] = %u;", viT, link));
            br.type = Br::B;
            br.target = target;
            return br;
        } else if (n == "jr" || n == "jalr") {
            L.push_back(strf("const uint32_t jt = (static_cast<uint32_t>(static_cast<uint16_t>(%s)) * 8u) & 0x%Xu;",
                             branchVi(viS).c_str(), mask));
            if (n == "jalr" && viT)
                L.push_back(strf("s.vi[%d] = %u;", viT, link));
            br.type = Br::JR;
            br.expr = "jt";
            return br;
        } else if (n == "ibeq" || n == "ibne") {
            L.push_back(strf("const bool bt = static_cast<int16_t>(%s) %s static_cast<int16_t>(%s);", branchVi(viS).c_str(),
                             n == "ibeq" ? "==" : "!=", branchVi(viT).c_str()));
            br.type = Br::COND;
            br.expr = "bt";
            br.target = target;
            return br;
        } else if (oneOf(n, {"ibltz", "ibgtz", "iblez", "ibgez"})) {
            const char *opx = n == "ibltz" ? "<" : n == "ibgtz" ? ">" : n == "iblez" ? "<=" : ">=";
            L.push_back(strf("const bool bt = static_cast<int16_t>(%s) %s 0;", branchVi(viS).c_str(), opx));
            br.type = Br::COND;
            br.expr = "bt";
            br.target = target;
            return br;
        } else if (oneOf(n, {"iadd", "isub", "iand", "ior"})) {
            if (viD) {
                if (n == "iadd")
                    L.push_back(strf("s.vi[%d] = static_cast<int16_t>(s.vi[%d] + s.vi[%d]);", viD, viS, viT));
                else if (n == "isub")
                    L.push_back(strf("s.vi[%d] = static_cast<int16_t>(s.vi[%d] - s.vi[%d]);", viD, viS, viT));
                else if (n == "iand")
                    L.push_back(strf("s.vi[%d] = s.vi[%d] & s.vi[%d];", viD, viS, viT));
                else
                    L.push_back(strf("s.vi[%d] = s.vi[%d] | s.vi[%d];", viD, viS, viT));
            }
        } else if (n == "iaddi") {
            int imm5 = (l >> 6) & 0x1F;
            imm5 = (imm5 & 16) ? imm5 - 32 : imm5;
            if (viT)
                L.push_back(strf("s.vi[%d] = static_cast<int16_t>(s.vi[%d] + (%d));", viT, viS, imm5));
        } else if (n == "move" || n == "mr32") {
            const int src[4] = {n == "move" ? 0 : 1, n == "move" ? 1 : 2, n == "move" ? 2 : 3, n == "move" ? 3 : 0};
            if (vfT && !skipVf && vecStores) {
                lowerVfWrites.push_back({vfT, dest});
                if (n == "move")
                    L.push_back(strf("{ const __m128 tv = _mm_loadu_ps(s.vf[%d]);", vfS));
                else
                    L.push_back(strf("{ const __m128 tv0 = _mm_loadu_ps(s.vf[%d]); const __m128 tv = _mm_shuffle_ps(tv0, tv0, _MM_SHUFFLE(0, 3, 2, 1));", vfS));
                vecStore(L, strf("s.vf[%d]", vfT), dest, "tv");
                L.push_back("}");
            } else if (vfT && !skipVf) {
                L.push_back(strf("{ const float t[4] = {s.vf[%d][%d], s.vf[%d][%d], s.vf[%d][%d], s.vf[%d][%d]};", vfS, src[0],
                                 vfS, src[1], vfS, src[2], vfS, src[3]));
                vfWrite(L, vfT, dest, tv, skipVf);
                L.push_back("}");
            }
        } else if (n == "lqi" || n == "lqd") {
            L.push_back("{");
            if (n == "lqd" && viS)
                L.push_back(strf("s.vi[%d] = static_cast<int16_t>(s.vi[%d] - 1);", viS, viS));
            L.push_back("const uint32_t a = " + addrReg(viS) + "; float t[4]; std::memcpy(t, c.mem + a, 16);");
            vfWrite(L, vfT, dest, tv, skipVf);
            if (n == "lqi" && viS)
                L.push_back(strf("s.vi[%d] = static_cast<int16_t>(s.vi[%d] + 1);", viS, viS));
            L.push_back("}");
        } else if (n == "sqi" || n == "sqd") {
            L.push_back("{");
            if (n == "sqd" && viT)
                L.push_back(strf("s.vi[%d] = static_cast<int16_t>(s.vi[%d] - 1);", viT, viT));
            L.push_back(strf("uint32_t w[4]; std::memcpy(w, s.vf[%d], 16); store(c, %s, w, 0x%xu);", vfS, addrReg(viT).c_str(), dest));
            if (n == "sqi" && viT)
                L.push_back(strf("s.vi[%d] = static_cast<int16_t>(s.vi[%d] + 1);", viT, viT));
            L.push_back("}");
        } else if (n == "div") {
            const int fsf = (l >> 21) & 3, ftf = (l >> 23) & 3;
            L.push_back(strf("{ const float num = N(s.vf[%d][%d]); const float den = N(s.vf[%d][%d]); uint32_t di = 0u; float r;", vfS, fsf, vfT, ftf));
            L.push_back("  if (den == 0.0f) { di = num == 0.0f ? 0x10u : 0x20u; r = std::signbit(num) != std::signbit(den) ? -std::numeric_limits<float>::max() : std::numeric_limits<float>::max(); }");
            L.push_back("  else r = num / den;");
            L.push_back("  queueQ(c, normalizeResult(r), 7u, di); }");
        } else if (n == "sqrt") {
            const int ftf = (l >> 23) & 3;
            L.push_back(strf("{ const float v = N(s.vf[%d][%d]); queueQ(c, std::sqrt(std::fabs(v)), 7u, v < 0.0f ? 0x10u : 0u); }", vfT, ftf));
        } else if (n == "rsqrt") {
            const int fsf = (l >> 21) & 3, ftf = (l >> 23) & 3;
            L.push_back(strf("{ const float num = N(s.vf[%d][%d]); const float rad = N(s.vf[%d][%d]); const float den = std::sqrt(std::fabs(rad));", vfS, fsf, vfT, ftf));
            L.push_back("  uint32_t di = rad < 0.0f ? 0x10u : 0u; float r;");
            L.push_back("  if (den != 0.0f) r = num / den;");
            L.push_back("  else { di = num == 0.0f ? 0x10u : 0x20u; r = std::signbit(num) ? -std::numeric_limits<float>::max() : std::numeric_limits<float>::max(); }");
            L.push_back("  queueQ(c, normalizeResult(r), 13u, di); }");
        } else if (n == "waitq" || n == "waitp") {
        } else if (n == "mtir") {
            const int comp = (l >> 21) & 3;
            if (viT)
                L.push_back(strf("s.vi[%d] = static_cast<int16_t>(floatBits(s.vf[%d][%d]) & 0xFFFFu);", viT, vfS, comp));
        } else if (n == "mfir") {
            if (vfT && !skipVf) {
                L.push_back(strf("{ const float t = bitsToFloat(static_cast<uint32_t>(static_cast<int32_t>(static_cast<int16_t>(s.vi[%d] & 0xFFFF))));", viS));
                vfWrite(L, vfT, dest, t1, skipVf);
                L.push_back("}");
            }
        } else if (n == "ilwr") {
            L.push_back(strf("{ uint32_t v; std::memcpy(&v, c.mem + %s + %d, 4);", addrReg(viS).c_str(), firstLane(dest) * 4));
            if (viT)
                L.push_back(strf("  s.vi[%d] = static_cast<int16_t>(v & 0xFFFFu);", viT));
            L.push_back("}");
        } else if (n == "iswr") {
            L.push_back(strf("{ const uint32_t v = static_cast<uint16_t>(s.vi[%d] & 0xFFFF); const uint32_t w[4] = {v, v, v, v}; store(c, %s, w, 0x%xu); }",
                             viT, addrReg(viS).c_str(), dest));
        } else if (n == "rnext") {
            L.push_back("{ const uint32_t x = (s.r >> 4) & 1u; const uint32_t y = (s.r >> 22) & 1u; s.r = ((s.r << 1) ^ x ^ y) & 0x007FFFFFu; s.r |= 0x3F800000u; }");
            if (vfT && !skipVf) {
                L.push_back("{ const float t = bitsToFloat(s.r);");
                vfWrite(L, vfT, dest, t1, skipVf);
                L.push_back("}");
            }
        } else if (n == "rget") {
            if (vfT && !skipVf) {
                L.push_back("{ const float t = bitsToFloat(s.r);");
                vfWrite(L, vfT, dest, t1, skipVf);
                L.push_back("}");
            }
        } else if (n == "rinit") {
            L.push_back(strf("s.r = 0x3F800000u | (floatBits(s.vf[%d][%d]) & 0x007FFFFFu);", vfS, (l >> 21) & 3));
        } else if (n == "rxor") {
            L.push_back(strf("s.r = 0x3F800000u | ((s.r ^ floatBits(s.vf[%d][%d])) & 0x007FFFFFu);", vfS, (l >> 21) & 3));
        } else if (n == "mfp") {
            L.push_back("commit(c);");
            if (vfT && !skipVf) {
                L.push_back("{ const float t = s.p;");
                vfWrite(L, vfT, dest, t1, skipVf);
                L.push_back("}");
            }
        } else if (n == "xtop" || n == "xitop") {
            if (viT)
                L.push_back(strf("s.vi[%d] = static_cast<int32_t>(s.%s & 0x3FFu);", viT, n == "xtop" ? "top" : "itop"));
        } else if (n == "xgkick") {
            L.push_back(strf("startKick(c, static_cast<uint32_t>(static_cast<uint16_t>(s.vi[%d])));", viS));
        } else if (oneOf(n, {"esadd", "ersadd", "eleng", "erleng"})) {
            L.push_back(strf("{ const float x = N(s.vf[%d][0]); const float y = N(s.vf[%d][1]); const float z = N(s.vf[%d][2]);", vfS, vfS, vfS));
            if (n == "esadd")
                L.push_back("  queueP(c, x * x + y * y + z * z, 11u); }");
            else if (n == "ersadd")
                L.push_back("  const float sum = x * x + y * y + z * z; queueP(c, sum != 0.0f ? 1.0f / sum : sum, 18u); }");
            else if (n == "eleng")
                L.push_back("  queueP(c, std::sqrt(x * x + y * y + z * z), 18u); }");
            else
                L.push_back("  const float len = std::sqrt(x * x + y * y + z * z); queueP(c, len != 0.0f ? 1.0f / len : len, 24u); }");
        } else if (oneOf(n, {"ersqrt", "esqrt", "ercpr"})) {
            const int comp = (l >> 21) & 3;
            L.push_back(strf("{ const float v = N(s.vf[%d][%d]);", vfS, comp));
            if (n == "ersqrt")
                L.push_back("  float r = v; if (r >= 0.0f) { r = std::sqrt(r); if (r != 0.0f) r = 1.0f / r; } queueP(c, r, 18u); }");
            else if (n == "esqrt")
                L.push_back("  queueP(c, v >= 0.0f ? std::sqrt(v) : v, 12u); }");
            else
                L.push_back("  queueP(c, v != 0.0f ? 1.0f / v : v, 12u); }");
        } else if (n == "esum") {
            L.push_back(strf("{ float sum = 0.0f; for (uint32_t k = 0; k < 4u; ++k) sum += N(s.vf[%d][k]); queueP(c, sum, 12u); }", vfS));
        } else {
            throw Unsupported{strf("lower %s at %x", n.c_str(), pc)};
        }
        return br;
    }

    // ---------------------------------------------------------------- pair code
    struct RW {
        std::vector<int> reads;
        std::vector<std::pair<int, int>> writes;
    };

    static RW pairRw(const Pair &p)
    {
        RW rw;
        std::vector<int> reads;
        for (const Usage *u : {&p.uu, &p.lu}) {
            for (int i = 0; i < u->vfReadCount; ++i)
                if (u->vfReadReg[i])
                    for (int k : lanesOf(u->vfReadLanes[i]))
                        reads.push_back(vfKey(u->vfReadReg[i], k));
            for (int reg = 1; reg < 16; ++reg)
                if (u->viRead & (1u << reg))
                    reads.push_back(viKey(reg));
        }
        bool seen[NKEYS] = {};
        for (int k : reads)
            if (!seen[k]) {
                seen[k] = true;
                rw.reads.push_back(k);
            }
        const Usage &lu = p.lu, &uu = p.uu;
        if (lu.vfWriteReg && p.suppressedLowerVf != lu.vfWriteReg) {
            const int lat = lu.vfLatency ? lu.vfLatency : lu.latency;
            for (int k : lanesOf(lu.vfWriteLanes))
                rw.writes.push_back({vfKey(lu.vfWriteReg, k), lat});
        }
        if (uu.vfWriteReg) {
            const int lat = uu.vfLatency ? uu.vfLatency : uu.latency;
            for (int k : lanesOf(uu.vfWriteLanes))
                rw.writes.push_back({vfKey(uu.vfWriteReg, k), lat});
        }
        for (int reg = 1; reg < 16; ++reg)
            if (lu.viWrite & (1u << reg))
                rw.writes.push_back({viKey(reg), lu.viLatency ? lu.viLatency : lu.latency});
        return rw;
    }

    static Lines readyExprLines(const Pair &p, const std::set<int> *need)
    {
        Lines L, terms;
        const RW rw = pairRw(p);
        for (int key : rw.reads)
            if (!need || need->count(key))
                terms.push_back(readyKey(key));
        if (!need)
            for (const Usage *u : {&p.uu, &p.lu})
                for (int k : lanesOf(u->accRead))
                    terms.push_back(strf("c.accReady[%d]", k));
        const Usage &lu = p.lu;
        if (lu.pipeline == PIPE_FDIV)
            terms.push_back("qWaitCycle(c)");
        if (lu.pipeline == PIPE_EFU)
            terms.push_back("c.efuResourceReady");
        if (lu.waitP)
            terms.push_back("pWaitCycle(c)");
        Lines uniq;
        for (const std::string &t : terms)
            if (std::find(uniq.begin(), uniq.end(), t) == uniq.end())
                uniq.push_back(t);
        for (const std::string &t : uniq)
            L.push_back("r = std::max<uint64_t>(r, " + t + ");");
        if (lu.pipeline == PIPE_XGKICK)
            L.push_back("if (c.kick.active) r = std::max<uint64_t>(r, kickFinish(c));");
        if (L.empty())
            return {};
        Lines out = {"{ uint64_t r = c.cycle;"};
        for (const std::string &x : L)
            out.push_back("  " + x);
        out.push_back("  c.cycle = r; }");
        return out;
    }

    static Lines markLines(const Pair &p, const std::set<int> *marks, int lastLat)
    {
        Lines L;
        const RW rw = pairRw(p);
        std::vector<int> lats;
        for (const auto &w : rw.writes) {
            if (!marks || marks->count(w.first))
                L.push_back(strf("%s = c.cycle + %d;", readyKey(w.first).c_str(), w.second));
            lats.push_back(w.second);
        }
        if (!marks) {
            for (int k : lanesOf(p.uu.accWrite))
                L.push_back(strf("c.accReady[%d] = c.cycle + 1;", k));
            if (p.uu.accWrite)
                lats.push_back(1);
            lastLat = lats.empty() ? 0 : *std::max_element(lats.begin(), lats.end());
        }
        if (lastLat)
            L.push_back(strf("c.lastReady = std::max<uint64_t>(c.lastReady, c.cycle + %d);", lastLat));
        return L;
    }

    static int writtenVi(const Pair &p)
    {
        for (int reg = 1; reg < 16; ++reg)
            if (p.lu.viWrite & (1u << reg))
                return reg;
        return 0;
    }

    Br emitPairBody(const Pair &p, Lines &L, const BranchVi &branchVi, const Timing *timing, bool dropFlags)
    {
        lowerVfWrites.clear();
        const Lines rl = readyExprLines(p, timing ? &timing->need : nullptr);
        L.insert(L.end(), rl.begin(), rl.end());
        const int wvi = writtenVi(p);
        if (wvi && p.lu.delaysNextBranchRead)
            L.push_back(strf("const int32_t oldvi_%x = s.vi[%d];", p.pc, wvi));
        Lines UL;
        const auto td = emitUpper(p, UL, dropFlags);
        const int target = td.first, dest = td.second;
        const int vfTarget = target >= 0 ? target : -1; // -1: None
        L.insert(L.end(), UL.begin(), UL.end());
        if (vfTarget == 0 && dest)
            for (int k : lanesOf(dest))
                L.push_back(strf("s.vf[0][%d] = ur%d;", k, k));
        if (p.iBit)
            L.push_back(strf("s.i = N(bitsToFloat(0x%08xu));", p.lower));
        const Br br = emitLower(p, L, branchVi, p.suppressedLowerVf != 0);
        if (vfTarget > 0 && dest) {
            if (vecStores) {
                vecStore(L, strf("s.vf[%d]", vfTarget), dest, urVec(dest));
            } else {
                for (int k : lanesOf(dest))
                    L.push_back(strf("s.vf[%d][%d] = ur%d;", vfTarget, k, k));
            }
        }
        const Lines ml = markLines(p, timing ? &timing->marks : nullptr, timing ? timing->lastLat : 0);
        L.insert(L.end(), ml.begin(), ml.end());
        if (vfTarget == 0 && dest)
            L.push_back("s.vf[0][0] = 0.0f; s.vf[0][1] = 0.0f; s.vf[0][2] = 0.0f; s.vf[0][3] = 1.0f;");
        for (const auto &w : lowerVfWrites)
            norm[w.first] &= ~destBits(w.second);
        const int upperTarget = target == T_ACC ? ACC : (vfTarget > 0 ? vfTarget : -1);
        if (upperTarget >= 0 && dest) {
            if (isNormalisedKind(upperInfo(p.upper).kind))
                norm[upperTarget] |= destBits(dest);
            else
                norm[upperTarget] &= ~destBits(dest);
        }
        return br;
    }

    void checkSupported(const Pair &p) const
    {
        if (p.dBit || p.tBit)
            throw Unsupported{strf("D/T bit at %x", p.pc)};
        if (p.uu.reserved || p.lu.reserved)
            throw Unsupported{strf("reserved at %x", p.pc)};
        if (vu0 && !p.iBit && (p.lu.pipeline == PIPE_EFU || p.lu.pipeline == PIPE_XGKICK))
            throw Unsupported{strf("EFU/XGKICK on VU0 at %x", p.pc)};
    }

    // ---------------------------------------------------------------- generator
    void analyse(const Bytes &code, const std::set<uint32_t> &extraLeaders)
    {
        pairs.clear();
        for (uint32_t pc = 0; pc < codeSize; pc += 8)
            pairs.push_back(decodePair(code.data(), pc));
        leaders = extraLeaders;
        std::set<int> jrRegs;
        for (const Pair &p : pairs)
            if (!p.iBit && oneOf(lowerName(p.lower), {"jr", "jalr"}))
                jrRegs.insert(VIS(p.lower));
        for (const Pair &p : pairs) {
            if (p.iBit)
                continue;
            const std::string n = lowerName(p.lower);
            if (oneOf(n, {"b", "bal", "ibeq", "ibne", "ibltz", "ibgtz", "iblez", "ibgez"})) {
                leaders.insert(static_cast<uint32_t>(static_cast<int>(p.pc) + 8 + IMM11(p.lower) * 8) & mask);
                leaders.insert((p.pc + 16) & mask);
            } else if (n == "jr" || n == "jalr") {
                leaders.insert((p.pc + 16) & mask);
            } else if (n == "iaddiu" && VIS(p.lower) == 0 && jrRegs.count(VIT(p.lower))) {
                const uint32_t imm = (p.lower & 0x7FF) | ((p.lower >> 10) & 0x7800);
                if (0 < imm * 8 && imm * 8 < codeSize)
                    leaders.insert(imm * 8);
            }
        }
    }

    static bool isBranch(const Pair &p) { return !p.iBit && p.lu.pipeline == PIPE_BRANCH; }

    // false: an unsupported pair was reached (Python returns None, None)
    bool blockSequence(uint32_t start, std::vector<const Pair *> &seq, Term &term) const
    {
        seq.clear();
        uint32_t pc = start;
        for (;;) {
            if (isUnsupported(pc))
                return false;
            const Pair &p = at(pc);
            seq.push_back(&p);
            if (p.eBit) {
                if (isBranch(p))
                    throw Unsupported{strf("branch with E bit at %x", pc)};
                const uint32_t nxt = (pc + 8) & mask;
                const Pair &q = at(nxt);
                if (isUnsupported(nxt) || isBranch(q) || q.eBit)
                    throw Unsupported{strf("unsupported pair after the E bit at %x", nxt)};
                seq.push_back(&q);
                term = {Term::END, (nxt + 8) & mask};
                return true;
            }
            if (isBranch(p)) {
                const uint32_t ds = (pc + 8) & mask;
                const Pair &q = at(ds);
                if (isUnsupported(ds) || isBranch(q))
                    throw Unsupported{strf("branch in delay slot at %x", ds)};
                seq.push_back(&q);
                term = {Term::BRANCH, 0};
                return true;
            }
            pc = (pc + 8) & mask;
            if (leaders.count(pc) || pc == 0) {
                term = {Term::FALL, pc};
                return true;
            }
        }
    }

    std::vector<uint32_t> returnPoints() const
    {
        std::set<uint32_t> returns;
        for (uint32_t pc = 0; pc < codeSize; pc += 8) {
            const Pair &p = at(pc);
            if (p.iBit)
                continue;
            const std::string n = lowerName(p.lower);
            if ((n == "bal" || n == "jalr") && VIT(p.lower) == 15) {
                returns.insert((pc + 16) & mask);
            } else if (n == "iaddiu" && VIS(p.lower) == 0 && VIT(p.lower) == 15) {
                const uint32_t imm = (p.lower & 0x7FF) | ((p.lower >> 10) & 0x7800);
                returns.insert((imm * 8) & mask);
            }
        }
        return std::vector<uint32_t>(returns.begin(), returns.end());
    }

    // successors of pc; false = unknown target (None); endsProgram: [] for entry_pending, None for live
    bool successors(uint32_t pc, const std::vector<uint32_t> &returns, std::vector<uint32_t> &out, bool &programEnd) const
    {
        out.clear();
        programEnd = false;
        const uint32_t prevPc = (pc - 8) & mask;
        const Pair &prev = at(prevPc);
        if (prev.eBit) {
            programEnd = true;
            return true;
        }
        if (isBranch(prev)) {
            const std::string n = lowerName(prev.lower);
            if (n == "jr" || n == "jalr") {
                if (VIS(prev.lower) == 15) {
                    out = returns;
                    return true;
                }
                return false;
            }
            const uint32_t target = static_cast<uint32_t>(static_cast<int>(prevPc) + 8 + IMM11(prev.lower) * 8) & mask;
            if (n == "b" || n == "bal") {
                out.push_back(target);
                return true;
            }
            out.push_back(target);
            out.push_back((pc + 8) & mask);
            return true;
        }
        out.push_back((pc + 8) & mask);
        return true;
    }

    void computeEntryPending()
    {
        const std::vector<uint32_t> returns = returnPoints();
        const uint32_t n = npairs();
        std::vector<std::array<uint8_t, NKEYS>> state(n);
        for (auto &s : state)
            s.fill(0);
        std::array<uint8_t, NKEYS> pool;
        pool.fill(0);
        bool poolNonEmpty = false;
        auto joinInto = [](std::array<uint8_t, NKEYS> &dst, const std::array<uint8_t, NKEYS> &src) {
            bool grew = false;
            for (int k = 0; k < NKEYS; ++k)
                if (src[k] > dst[k]) {
                    dst[k] = src[k];
                    grew = true;
                }
            return grew;
        };
        std::vector<RW> rws;
        for (const Pair &p : pairs)
            rws.push_back(pairRw(p));
        std::vector<uint32_t> nxt;
        bool changed = true;
        while (changed) {
            changed = false;
            for (uint32_t pc = 0; pc < codeSize; pc += 8) {
                if (isUnsupported(pc))
                    continue;
                std::array<uint8_t, NKEYS> out;
                out.fill(0);
                const auto &st = state[pc / 8];
                for (int k = 0; k < NKEYS; ++k)
                    if (st[k] > 1)
                        out[k] = static_cast<uint8_t>(st[k] - 1);
                for (const auto &w : rws[pc / 8].writes)
                    if (w.second > 1)
                        out[w.first] = static_cast<uint8_t>(std::max<int>(out[w.first], w.second - 1));
                bool end;
                if (!successors(pc, returns, nxt, end)) {
                    if (joinInto(pool, out))
                        changed = true;
                    for (int k = 0; k < NKEYS; ++k)
                        poolNonEmpty |= pool[k] != 0;
                    continue;
                }
                for (uint32_t t : nxt)
                    if (joinInto(state[t / 8], out))
                        changed = true;
            }
            if (poolNonEmpty)
                for (uint32_t pc = 0; pc < codeSize; pc += 8)
                    if (joinInto(state[pc / 8], pool))
                        changed = true;
        }
        entryPending = std::move(state);
        haveEntryPending = true;
    }

    std::vector<Timing> blockTiming(const std::vector<const Pair *> &seq, const std::array<uint8_t, NKEYS> *entry) const
    {
        const int n = static_cast<int>(seq.size());
        std::map<int, std::pair<int, int>> latest;
        std::vector<Timing> t(static_cast<size_t>(n));
        for (int i = 0; i < n; ++i) {
            const RW rw = pairRw(*seq[i]);
            for (int key : rw.reads) {
                const auto it = latest.find(key);
                if (it != latest.end()) {
                    const int j = it->second.first, lat = it->second.second;
                    if (i - j >= lat)
                        continue;
                    t[j].marks.insert(key);
                } else if (entry && (*entry)[key] <= i) {
                    continue;
                }
                t[i].need.insert(key);
            }
            for (const auto &w : rw.writes) {
                latest[w.first] = {i, w.second};
                if (n - i < w.second)
                    t[i].lastLat = std::max(t[i].lastLat, w.second);
            }
        }
        for (const auto &kv : latest)
            if (n - kv.second.first < 4)
                t[kv.second.first].marks.insert(kv.first);
        return t;
    }

    static bool writesMac(const Pair &p) { return isFmacKind(upperInfo(p.upper).kind) && DEST(p.upper) != 0; }
    static bool readsMac(const Pair &p) { return !p.iBit && oneOf(lowerName(p.lower), {"fmand", "fmeq", "fmor"}); }

    std::vector<bool> deadFlagPairs(const std::vector<const Pair *> &seq) const
    {
        const int n = static_cast<int>(seq.size());
        std::vector<bool> dead(static_cast<size_t>(n), false);
        if (!deadFlagsOk)
            return dead;
        std::vector<int> writers;
        for (int i = 0; i < n; ++i)
            if (writesMac(*seq[i]))
                writers.push_back(i);
        for (size_t a = 0; a < writers.size(); ++a) {
            if (a + 1 >= writers.size())
                break;
            const int i = writers[a], j = writers[a + 1];
            if (j + 3 > n - 1)
                continue;
            bool read = false;
            for (int k = i + 1; k < j + 4; ++k)
                if (readsMac(*seq[k]))
                    read = true;
            if (read)
                continue;
            dead[static_cast<size_t>(i)] = true;
        }
        return dead;
    }

    void computeLiveMac()
    {
        const std::vector<uint32_t> returns = returnPoints();
        const uint32_t n = npairs();
        liveMac.assign(n, false);
        std::vector<uint32_t> nxt;
        std::vector<uint8_t> seen(static_cast<size_t>(n) * 5);
        auto live = [&](uint32_t i) -> bool {
            bool end;
            std::vector<uint32_t> first;
            if (!successors(i, returns, first, end) || end)
                return true;
            std::fill(seen.begin(), seen.end(), uint8_t{0});
            std::vector<std::pair<uint32_t, int>> stack;
            for (uint32_t s : first)
                stack.push_back({s, 0});
            while (!stack.empty()) {
                const auto top = stack.back();
                stack.pop_back();
                const uint32_t pc = top.first;
                const int k = top.second;
                uint8_t &sn = seen[(pc / 8) * 5 + static_cast<uint32_t>(k)];
                if (sn)
                    continue;
                sn = 1;
                if (isUnsupported(pc))
                    return true;
                const Pair &p = at(pc);
                if (readsMac(p))
                    return true;
                const int k2 = k == 0 ? (writesMac(p) ? 1 : 0) : k + 1;
                if (k2 >= 4)
                    continue;
                if (!successors(pc, returns, nxt, end) || end)
                    return true;
                for (uint32_t s : nxt)
                    stack.push_back({s, k2});
            }
            return false;
        };
        for (uint32_t pc = 0; pc < codeSize; pc += 8)
            if (writesMac(at(pc)) && live(pc))
                liveMac[pc / 8] = true;
    }

    static Lines localizeVf(const Lines &L)
    {
        // s\.vf\[(\d+)\] -> lv<n>
        auto scan = [](const std::string &line, std::vector<std::pair<size_t, size_t>> &spans, std::vector<std::string> &nums) {
            size_t pos = 0;
            for (;;) {
                const size_t f = line.find("s.vf[", pos);
                if (f == std::string::npos)
                    return;
                size_t e = f + 5;
                while (e < line.size() && line[e] >= '0' && line[e] <= '9')
                    ++e;
                if (e > f + 5 && e < line.size() && line[e] == ']') {
                    spans.push_back({f, e + 1});
                    nums.push_back(line.substr(f + 5, e - f - 5));
                    pos = e + 1;
                } else {
                    pos = f + 1;
                }
            }
        };
        std::set<int> regs;
        for (size_t i = 2; i < L.size(); ++i) {
            std::vector<std::pair<size_t, size_t>> spans;
            std::vector<std::string> nums;
            scan(L[i], spans, nums);
            for (const std::string &s : nums)
                regs.insert(std::atoi(s.c_str()));
        }
        if (regs.empty())
            return L;
        std::string wb;
        for (int r : regs) {
            if (!wb.empty())
                wb += ' ';
            wb += strf("std::memcpy(s.vf[%d], lv%d, 16);", r, r);
        }
        Lines head(L.begin(), L.begin() + 2);
        for (int r : regs)
            head.push_back(strf("alignas(16) float lv%d[4]; std::memcpy(lv%d, s.vf[%d], 16);", r, r, r));
        for (size_t i = 2; i < L.size(); ++i) {
            std::vector<std::pair<size_t, size_t>> spans;
            std::vector<std::string> nums;
            scan(L[i], spans, nums);
            std::string line;
            size_t prev = 0;
            for (size_t k = 0; k < spans.size(); ++k) {
                line += L[i].substr(prev, spans[k].first - prev);
                line += "lv" + nums[k];
                prev = spans[k].second;
            }
            line += L[i].substr(prev);
            size_t ws = 0;
            while (ws < line.size() && (line[ws] == ' ' || (line[ws] >= 9 && line[ws] <= 13)))
                ++ws;
            if (line.find("return ") != std::string::npos && line.compare(ws, 2, "//") != 0) {
                std::string r;
                size_t p0 = 0;
                for (;;) {
                    const size_t f = line.find("return ", p0);
                    if (f == std::string::npos)
                        break;
                    r += line.substr(p0, f - p0) + wb + " return ";
                    p0 = f + 7;
                }
                r += line.substr(p0);
                line = r;
            }
            head.push_back(line);
        }
        return head;
    }

    bool genBlock(uint32_t start, Lines &L)
    {
        std::vector<const Pair *> seq;
        Term term{};
        if (!blockSequence(start, seq, term))
            return false;
        const std::vector<Timing> timing = blockTiming(seq, haveEntryPending ? &entryPending[start / 8] : nullptr);
        std::vector<bool> dead = deadFlagPairs(seq);
        if (deadFlagsOk && !liveMac.empty())
            for (size_t idx = 0; idx < seq.size(); ++idx)
                if (writesMac(*seq[idx]) && !liveMac[seq[idx]->pc / 8])
                    dead[idx] = true;
        L = {strf("uint32_t block_%04x(Ctx &c) {", start), "VU1State &s = *c.s;"};
        std::fill(std::begin(norm), std::end(norm), 0);

        auto bbExit = [](const Pair &q) -> std::string {
            const int wq = writtenVi(q);
            if (wq && q.lu.delaysNextBranchRead)
                return strf("c.bbValid = true; c.bbReg = %d; c.bbValue = oldvi_%x;", wq, q.pc);
            return "c.bbValid = false;";
        };
        auto staticBvi = [](const Pair *prev) -> BranchVi {
            return [prev](int reg) -> std::string {
                if (reg == 0)
                    return "0";
                if (!prev)
                    return strf("(c.bbValid && c.bbReg == %d ? c.bbValue : s.vi[%d])", reg, reg);
                if (writtenVi(*prev) == reg && prev->lu.delaysNextBranchRead)
                    return strf("oldvi_%x", prev->pc);
                return strf("s.vi[%d]", reg);
            };
        };

        const size_t n = seq.size();
        int depth = 0;
        Br br;
        for (size_t i = 0; i < n; ++i) {
            const Pair &p = *seq[i];
            const Pair *prev = i > 0 ? seq[i - 1] : nullptr;
            std::string cm = fmtPair(p);
            std::replace(cm.begin(), cm.end(), '\\', '/');
            L.push_back("// " + cm);
            L.push_back("{");
            ++depth;
            Lines body;
            const Br b = emitPairBody(p, body, staticBvi(prev), &timing[i], dead[i]);
            L.insert(L.end(), body.begin(), body.end());
            const bool lastPair = i == n - 1;
            if (term.type == Term::BRANCH && i == n - 2) {
                br = b;
                if (br.type == Br::COND)
                    L.push_back(strf("const bool taken_%x = %s;", p.pc, br.expr.c_str()));
                else if (br.type == Br::JR)
                    L.push_back(strf("const uint32_t jtarget_%x = %s;", p.pc, br.expr.c_str()));
            }
            L.push_back("c.cycle += 1;");
            if (!lastPair) {
                if (term.type != Term::BRANCH || i != n - 2) {
                    L.push_back("}");
                    --depth;
                }
                continue;
            }
            if (term.type == Term::FALL) {
                L.push_back(bbExit(p));
                L.push_back(strf("return 0x%xu;", term.pc));
            } else if (term.type == Term::END) {
                L.push_back(strf("c.s->pc = 0x%xu;", term.pc));
                L.push_back("c.bbValid = false;");
                L.push_back("return kEnd;");
            } else {
                const Pair &bp = *seq[n - 2];
                L.push_back(bbExit(p));
                if (p.eBit)
                    L.push_back("c.pendingEndAfterNext = true;");
                if (br.type == Br::B) {
                    L.push_back(strf("s.branchTarget = 0x%xu; s.branchDelay = 0;", br.target));
                    L.push_back(strf("return 0x%xu;", br.target));
                } else if (br.type == Br::COND) {
                    L.push_back(strf("if (taken_%x) { s.branchTarget = 0x%xu; s.branchDelay = 0; return 0x%xu; }", bp.pc,
                                     br.target, br.target));
                    L.push_back(strf("return 0x%xu;", (p.pc + 8) & mask));
                } else {
                    L.push_back(strf("s.branchTarget = jtarget_%x; s.branchDelay = 0;", bp.pc));
                    L.push_back(strf("return jtarget_%x;", bp.pc));
                }
            }
        }
        for (int d = 0; d < depth; ++d)
            L.push_back("}");
        L.push_back("}");
        if (locals)
            L = localizeVf(L);
        return true;
    }

    static std::string hoistOldvi(const std::string &text)
    {
        // split before every line that starts a step_/block_ function
        std::vector<size_t> cuts;
        for (size_t pos = 0; pos <= text.size(); ) {
            if (text.compare(pos, 10, "void step_") == 0 || text.compare(pos, 15, "uint32_t block_") == 0)
                if (pos != 0)
                    cuts.push_back(pos);
            const size_t nl = text.find('\n', pos);
            if (nl == std::string::npos)
                break;
            pos = nl + 1;
        }
        cuts.push_back(text.size());
        std::string res;
        size_t from = 0;
        static const std::string decl = "const int32_t oldvi_";
        for (size_t cut : cuts) {
            std::string f = text.substr(from, cut - from);
            from = cut;
            // const int32_t (oldvi_[0-9a-f]+) =
            std::set<std::string> names;
            std::string g;
            size_t p0 = 0;
            for (;;) {
                const size_t m = f.find(decl, p0);
                if (m == std::string::npos)
                    break;
                size_t e = m + decl.size();
                while (e < f.size() && ((f[e] >= '0' && f[e] <= '9') || (f[e] >= 'a' && f[e] <= 'f')))
                    ++e;
                if (e > m + decl.size() && f.compare(e, 3, " = ") == 0) {
                    const std::string name = f.substr(m + 14, e - (m + 14)); // "oldvi_..."
                    names.insert(name);
                    g += f.substr(p0, m - p0) + name + " = ";
                    p0 = e + 3;
                } else {
                    g += f.substr(p0, m + 1 - p0);
                    p0 = m + 1;
                }
            }
            g += f.substr(p0);
            if (!names.empty()) {
                Lines decls;
                for (const std::string &nm : names)
                    decls.push_back(nm + " = 0");
                static const std::string anchor = "VU1State &s = *c.s;\n";
                const size_t a = g.find(anchor);
                if (a != std::string::npos)
                    g.replace(a, anchor.size(), anchor + "int32_t " + join(decls, ", ") + ";\n");
                f = g;
            }
            res += f;
        }
        return res;
    }

    struct Result {
        uint64_t hash;
        std::string text;
        int nblocks;
    };

    Result genImage(const Bytes &code, const std::string &nameHint, const std::set<uint32_t> &extraLeaders)
    {
        const uint64_t h = fnv64(code);
        const std::string ns = strf(vu0 ? "v0img_%016llx" : "img_%016llx", static_cast<unsigned long long>(h));
        analyse(code, extraLeaders);
        std::set<std::string> lowerOps;
        for (const Pair &p : pairs)
            if (!p.iBit)
                lowerOps.insert(lowerName(p.lower));
        deadFlagsOk = !(lowerOps.count("fsand") || lowerOps.count("fseq") || lowerOps.count("fsor") || lowerOps.count("fsset"));
        Lines out;
        out.push_back("// Generated by gt3-tools/vu1_translate.py from " + nameHint + ". Do not edit.");
        out.push_back(strf("// VU1 code image FNV-1a 64 %016llx.", static_cast<unsigned long long>(h)));
        out.push_back("#include \"runtime/ps2_vu1_translated.h\"");
        out.push_back("");
        out.push_back("namespace ps2vu1t { namespace " + ns + " {");
        out.push_back("namespace {");
        out.push_back("using PairFn = void (*)(Ctx &);");
        unsupported.clear();

        const BranchVi stepBvi = [](int reg) -> std::string {
            return reg == 0 ? std::string("0") : strf("(c.bbValid && c.bbReg == %d ? c.bbValue : s.vi[%d])", reg, reg);
        };
        for (uint32_t pc = 0; pc < codeSize; pc += 8) {
            const Pair &p = at(pc);
            Lines L;
            Br br;
            try {
                checkSupported(p);
                std::fill(std::begin(norm), std::end(norm), 0);
                br = emitPairBody(p, L, stepBvi, nullptr, false);
            } catch (const Unsupported &e) {
                unsupported[pc] = e.why;
                continue;
            }
            std::string cm = fmtPair(p);
            std::replace(cm.begin(), cm.end(), '\\', '/');
            out.push_back("// " + cm);
            out.push_back(strf("void step_%04x(Ctx &c) {", pc));
            out.push_back("VU1State &s = *c.s;");
            out.insert(out.end(), L.begin(), L.end());
            if (br.type == Br::B)
                out.push_back(strf("c.brTaken = true; c.brTarget = 0x%xu;", br.target));
            else if (br.type == Br::COND)
                out.push_back(strf("if (%s) { c.brTaken = true; c.brTarget = 0x%xu; }", br.expr.c_str(), br.target));
            else if (br.type == Br::JR)
                out.push_back("c.brTaken = true; c.brTarget = " + br.expr + ";");
            const int wvi = writtenVi(p);
            if (wvi && p.lu.delaysNextBranchRead)
                out.push_back(strf("c.bbValid = true; c.bbReg = %d; c.bbValue = oldvi_%x;", wvi, pc));
            else
                out.push_back("c.bbValid = false;");
            out.push_back("c.cycle += 1;");
            out.push_back("}");
        }

        haveEntryPending = false;
        if (wholeReady)
            computeEntryPending();
        computeLiveMac();
        int writers = 0, live = 0;
        for (uint32_t pc = 0; pc < codeSize; pc += 8) {
            if (writesMac(at(pc)))
                ++writers;
            if (liveMac[pc / 8])
                ++live;
        }
        std::printf("  MAC writers %d, live (whole-program) %d\n", writers, live);
        std::vector<uint32_t> blockPcs;
        for (uint32_t start : leaders) {
            if (isUnsupported(start))
                continue;
            Lines lines;
            try {
                if (!genBlock(start, lines))
                    continue;
            } catch (const Unsupported &e) {
                std::printf("  block %04x not generated: %s\n", start, e.why.c_str());
                continue;
            }
            out.insert(out.end(), lines.begin(), lines.end());
            blockPcs.push_back(start);
        }

        std::string text = hoistOldvi(join(out, "\n"));
        out = {text};
        out.push_back("} // namespace");
        out.push_back(strf("const PairFn kSteps[%u] = {", codeSize / 8));
        Lines row;
        for (uint32_t pc = 0; pc < codeSize; pc += 8)
            row.push_back(isUnsupported(pc) ? "nullptr" : strf("step_%04x", pc));
        for (size_t i = 0; i < row.size(); i += 8)
            out.push_back("  " + join(Lines(row.begin() + static_cast<std::ptrdiff_t>(i),
                                            row.begin() + static_cast<std::ptrdiff_t>(std::min(row.size(), i + 8))), ", ") + ",");
        out.push_back("};");
        out.push_back("using BlockFn = uint32_t (*)(Ctx &);");
        out.push_back(strf("const BlockFn kBlocks[%u] = {", codeSize / 8));
        const std::set<uint32_t> bset(blockPcs.begin(), blockPcs.end());
        row.clear();
        for (uint32_t pc = 0; pc < codeSize; pc += 8)
            row.push_back(bset.count(pc) ? strf("block_%04x", pc) : "nullptr");
        for (size_t i = 0; i < row.size(); i += 8)
            out.push_back("  " + join(Lines(row.begin() + static_cast<std::ptrdiff_t>(i),
                                            row.begin() + static_cast<std::ptrdiff_t>(std::min(row.size(), i + 8))), ", ") + ",");
        out.push_back("};");
        out.push_back(strf("const uint8_t kEbit[%u] = {", codeSize / 8));
        row.clear();
        for (uint32_t pc = 0; pc < codeSize; pc += 8)
            row.push_back(at(pc).eBit && !isUnsupported(pc) ? "1" : "0");
        for (size_t i = 0; i < row.size(); i += 32)
            out.push_back("  " + join(Lines(row.begin() + static_cast<std::ptrdiff_t>(i),
                                            row.begin() + static_cast<std::ptrdiff_t>(std::min(row.size(), i + 32))), ", ") + ",");
        out.push_back("};");
        out.push_back(runTemplate());
        out.push_back("}} // namespace ps2vu1t::" + ns);
        return {h, join(out, "\n") + "\n", static_cast<int>(blockPcs.size())};
    }

    std::string runTemplate() const
    {
        std::string t = RUN_TEMPLATE;
        auto replaceAll = [&](const std::string &from, const std::string &to) {
            size_t p = 0;
            while ((p = t.find(from, p)) != std::string::npos) {
                t.replace(p, from.size(), to);
                p += to.size();
            }
        };
        replaceAll("0x3FF8u", strf("0x%Xu", mask & ~7u));
        replaceAll("0x3FFFu", strf("0x%Xu", mask));
        return t;
    }

    static const char *const RUN_TEMPLATE;
};

const char *const Translator::RUN_TEMPLATE = R"TPL(
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
)TPL";

bool isHex16Lower(const std::string &s)
{
    if (s.size() != 16)
        return false;
    for (char c : s)
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f')))
            return false;
    return true;
}

void writeRegistry(const fs::path &outDir, bool vu0)
{
    const std::string prefix = vu0 ? "vu0_image_" : "vu1_image_";
    const std::string ns = vu0 ? "v0img_" : "img_";
    const std::string table = vu0 ? "kTranslatedVu0Images" : "kTranslatedImages";
    const std::string count = vu0 ? "kTranslatedVu0ImageCount" : "kTranslatedImageCount";
    std::vector<std::string> images;
    for (const auto &e : fs::directory_iterator(outDir)) {
        const std::string name = pathstr(e.path().filename());
        if (name.size() == prefix.size() + 16 + 4 && name.compare(0, prefix.size(), prefix) == 0 &&
            name.compare(name.size() - 4, 4, ".cpp") == 0 && isHex16Lower(name.substr(prefix.size(), 16)))
            images.push_back(name.substr(prefix.size(), 16));
    }
    std::sort(images.begin(), images.end());
    Lines L = {"// Generated by gt3-tools/vu1_translate.py. Do not edit.", "#include \"runtime/ps2_vu1_translated.h\"", "",
               "namespace ps2vu1t {"};
    for (const std::string &h : images)
        L.push_back("namespace " + ns + h + " { uint32_t run(Ctx &c, uint32_t startPc); }");
    L.push_back("extern const TranslatedImage " + table + "[];");
    L.push_back("extern const uint32_t " + count + ";");
    L.push_back("const TranslatedImage " + table + "[] = {");
    for (const std::string &h : images)
        L.push_back("    {0x" + h + "ull, &" + ns + h + "::run, \"" + h + "\"},");
    if (images.empty())
        L.push_back("    {0ull, nullptr, \"\"},");
    L.push_back("};");
    L.push_back("const uint32_t " + count + " = " + std::to_string(images.size()) + ";");
    L.push_back("}");
    writeFile(outDir / ((vu0 ? std::string("vu0") : std::string("vu1")) + "_images.cpp"), join(L, "\n") + "\n");
}

std::set<uint32_t> recordedEntries(const Bytes &d)
{
    size_t pos = 8;
    std::set<uint32_t> pcs;
    while (pos + 32 <= d.size()) {
        uint32_t hdr[8];
        for (int k = 0; k < 8; ++k)
            hdr[k] = rd32(&d[pos + 4 * k]);
        if (hdr[0] != 0x4B43494B)
            break;
        pcs.insert(hdr[2] & 0x3FF8);
        pos += 32 + 8 + 664 + (hdr[6] ? 16384 : 0) + 16384 + 8 + 664 + 32;
    }
    return pcs;
}

uint32_t parseHex(const std::string &x)
{
    std::string s;
    for (char c : x)
        if (c != '_' && c != ' ' && c != '\t')
            s += c;
    if (s.size() > 2 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X'))
        s = s.substr(2);
    if (s.empty())
        throw Error("bad hex number '" + x + "'");
    uint32_t v = 0;
    for (char c : s) {
        int d;
        if (c >= '0' && c <= '9')
            d = c - '0';
        else if (c >= 'a' && c <= 'f')
            d = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F')
            d = c - 'A' + 10;
        else
            throw Error("bad hex number '" + x + "'");
        v = v * 16 + static_cast<uint32_t>(d);
    }
    return v;
}

} // namespace

int cmdVuTranslate(const Args &argsIn)
{
    Args args = argsIn;
    Translator t;
    auto take = [&](const char *flag) {
        const auto it = std::find(args.begin(), args.end(), flag);
        if (it == args.end())
            return false;
        args.erase(it);
        return true;
    };
    if (take("--no-sticky"))
        t.keepSticky = false;
    if (take("--vu0")) {
        t.vu0 = true;
        t.codeSize = 0x1000;
        t.mask = 0xFFF;
        t.dataMask = 0xFFF;
    }
    std::set<uint32_t> entries;
    const auto it = std::find(args.begin(), args.end(), "--entries");
    if (it != args.end()) {
        if (it + 1 == args.end())
            throw Error("--entries needs a comma-separated list of hex pcs");
        const std::string list = *(it + 1);
        size_t p = 0;
        while (p <= list.size()) {
            const size_t c = list.find(',', p);
            const std::string x = list.substr(p, c == std::string::npos ? std::string::npos : c - p);
            if (!x.empty())
                entries.insert(parseHex(x) & t.mask);
            if (c == std::string::npos)
                break;
            p = c + 1;
        }
        args.erase(it, it + 2);
    }
    if (args.size() != 2) {
        std::fprintf(stderr, "usage: gt4setup vu-translate [--vu0] [--no-sticky] [--entries pc,pc,...] <code.bin | kicks.bin> <out_dir>\n");
        return 2;
    }
    const std::string src = args[0];
    const fs::path outDir = u8path(args[1]);
    const Bytes file = readFile(u8path(src));
    std::set<uint32_t> extra = entries;
    const bool isKicks = file.size() >= 8 && std::memcmp(file.data(), "GT3VU1K1", 8) == 0;
    if (!t.vu0 && src.size() >= 4 && src.compare(src.size() - 4, 4, ".bin") == 0 && isKicks) {
        const std::set<uint32_t> rec = recordedEntries(file);
        extra.insert(rec.begin(), rec.end());
    }
    Bytes code = t.vu0 ? Bytes(file.begin(), file.begin() + static_cast<std::ptrdiff_t>(std::min<size_t>(file.size(), t.codeSize)))
                       : loadCode(file);
    if (code.size() != t.codeSize)
        throw Error(strf("code image must be %u bytes", t.codeSize));
    if (t.vu0) {
        extra.insert(0);
        for (uint32_t pc = 0; pc < t.codeSize; pc += 8)
            if (decodePair(code.data(), pc).eBit)
                extra.insert((pc + 16) & t.mask);
    }
    const std::string nameHint = pathstr(u8path(src).filename());
    const Translator::Result r = t.genImage(code, nameHint, extra);
    std::error_code ec;
    fs::create_directories(outDir, ec);
    const fs::path path = outDir / strf("%s_image_%016llx.cpp", t.vu0 ? "vu0" : "vu1", static_cast<unsigned long long>(r.hash));
    writeFile(path, r.text);
    writeRegistry(outDir, t.vu0);
    std::printf("image %016llx: %d blocks, %d unsupported pairs -> %s\n", static_cast<unsigned long long>(r.hash), r.nblocks,
                static_cast<int>(t.unsupported.size()), pathstr(path).c_str());
    int shown = 0;
    for (const auto &u : t.unsupported) {
        if (shown++ >= 20)
            break;
        bool nonzero = false;
        for (uint32_t k = 0; k < 8; ++k)
            nonzero |= code[u.first + k] != 0;
        if (nonzero)
            std::printf("  unsupported %04x: %s\n", u.first, u.second.c_str());
    }
    return 0;
}

int cmdVuDisasm(const Args &args)
{
    if (args.empty() || args.size() > 3) {
        std::fprintf(stderr, "usage: gt4setup vu-disasm <code.bin | kicks.bin> [start_hex end_hex]\n");
        return 2;
    }
    const Bytes code = loadCode(readFile(u8path(args[0])));
    const uint32_t start = args.size() > 1 ? parseHex(args[1]) : 0;
    const uint32_t end = args.size() > 2 ? parseHex(args[2]) : static_cast<uint32_t>(code.size());
    for (uint32_t pc = start; pc < end && pc + 8 <= code.size(); pc += 8)
        std::printf("%s\n", fmtPair(decodePair(code.data(), pc)).c_str());
    return 0;
}

} // namespace gt4setup
