#ifndef PS2_VU1_TRANSLATED_H
#define PS2_VU1_TRANSLATED_H

// Runtime support for VU1 microprograms translated to C++ ahead of time
// (gt3-tools/vu1_translate.py). A translated program reproduces the
// VU1Interpreter bit for bit (registers, flags, VU memory, XGKICK packets and
// cycle counts; checked with gt3-tools/vu1_replay_bench.cpp) but runs the
// microcode as native code:
// - VF/VI/ACC results are written at issue. The interpreter stalls every
//   reader until a write is ready, so only the ready cycles are tracked.
// - Q, P, MAC/status/clip flags become visible after their latency; those
//   pipelines are kept and committed lazily, in the interpreter's order,
//   before anything reads them.
// - XGKICK copies one qword every 2 cycles from the cycle after the kick; the
//   copies are caught up lazily before each VU memory store, so the packet sees
//   memory exactly as the interpreter's per-cycle transfer does.

#include "runtime/ps2_vu1.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <smmintrin.h> // SSE4.1 (the runtime already requires it)

namespace ps2vu1t
{
    constexpr uint32_t kDataSize = 0x4000u;
    constexpr uint32_t kCodeSize = 0x4000u;
    constexpr uint32_t kEnd = 0xFFFFFFFFu;

    struct FlagEntry
    {
        uint64_t ready = 0;
        uint64_t issue = 0;
        uint32_t mac = 0;
        uint32_t status = 0;
        uint32_t extraSticky = 0;
        uint32_t clip = 0;
        uint8_t writes = 0; // kWrite* bits
    };
    constexpr uint8_t kWriteMac = 1u, kWriteStatus = 2u, kWriteSticky = 4u, kWriteClip = 8u;

    struct Kick
    {
        static constexpr uint32_t kBufferSize = 0x10000u;
        bool active = false;
        bool tagEop = false;
        uint32_t source = 0;
        uint32_t copied = 0;
        uint32_t tagEnd = 0;
        uint32_t total = 0;
        uint64_t nextCopy = 0; // cycle of the next qword copy
        uint8_t packet[kBufferSize];
    };

    struct Ctx
    {
        VU1State *s = nullptr;
        uint8_t *mem = nullptr;
        uint64_t cycle = 0;
        uint64_t vfReady[32][4];
        uint64_t viReady[16];
        uint64_t accReady[4];
        uint64_t lastReady = 0; // latest ready cycle of any issued VF/VI/ACC write or store

        bool qPending = false;
        uint64_t qReady = 0;
        float qValue = 0.0f;
        uint32_t qDi = 0;

        bool efuValid[2] = {false, false};
        uint64_t efuReady[2] = {0, 0};
        float efuValue[2] = {0.0f, 0.0f};
        uint64_t efuResourceReady = 0;

        // Flag results in issue order. They are committed lazily (at flag reads,
        // Q/P issue, the end, or when the ring fills); the interpreter never has
        // more than 8 in flight (4-cycle latency, at most 2 per cycle).
        static constexpr uint32_t kFlagRing = 64u;
        FlagEntry flags[kFlagRing];
        uint32_t flagHead = 0, flagTail = 0;
        uint32_t workingClip = 0;
        uint64_t nextCommit = std::numeric_limits<uint64_t>::max();
        // Flag results no instruction can read (the translator drops them in
        // images without status reads): only their sticky bits (status bits
        // 0-3 of the result and the product-sum ones), ORed in at finish.
        uint32_t stickyAcc = 0;

        // VI value a branch sees when the previous pair's integer op wrote it.
        bool bbValid = false;
        uint8_t bbReg = 0;
        int32_t bbValue = 0;

        // Branch taken by the pair just stepped (pair-stepping path).
        bool brTaken = false;
        uint32_t brTarget = 0;
        bool pendingEndAfterNext = false; // a block's delay slot had the E bit
        uint64_t blockRuns = 0;
        uint64_t steppedPairs = 0;

        Kick kick;
        bool (*submit)(void *user, const uint8_t *packet, uint32_t bytes) = nullptr;
        void *user = nullptr;
        uint32_t errors = 0;

        void begin(VU1State &state, uint8_t *data, uint64_t startCycle)
        {
            s = &state;
            mem = data;
            cycle = startCycle;
            std::memset(vfReady, 0, sizeof(vfReady));
            std::memset(viReady, 0, sizeof(viReady));
            std::memset(accReady, 0, sizeof(accReady));
            lastReady = 0;
            qPending = false;
            efuValid[0] = efuValid[1] = false;
            efuResourceReady = 0;
            flagHead = flagTail = 0;
            stickyAcc = 0;
            workingClip = state.clip;
            nextCommit = std::numeric_limits<uint64_t>::max();
            bbValid = false;
            kick.active = false;
            errors = 0;
        }
    };

    // ---------------------------------------------------------------- values
    inline float N(float value) // PS2 operand rule (VU1Interpreter::normalizeOperand)
    {
        uint32_t bits = 0;
        std::memcpy(&bits, &value, sizeof(bits));
        const uint32_t exponent = (bits >> 23) & 0xFFu;
        if (exponent == 0u)
            bits &= 0x80000000u;
        else if (exponent == 0xFFu)
            bits = (bits & 0x80000000u) | 0x7F7FFFFFu;
        std::memcpy(&value, &bits, sizeof(value));
        return value;
    }

    inline float normalizeResult(float value) // VU1Interpreter::normalizeResult (flags unused)
    {
        uint32_t bits = 0;
        std::memcpy(&bits, &value, sizeof(bits));
        const uint32_t sign = bits & 0x80000000u;
        const uint32_t exponent = (bits >> 23) & 0xFFu;
        if ((bits & 0x7FFFFFFFu) != 0u)
        {
            if (exponent == 0u)
                bits = sign;
            else if (exponent == 0xFFu)
                bits = sign | 0x7F7FFFFFu;
        }
        std::memcpy(&value, &bits, sizeof(value));
        return value;
    }

    // VU1Interpreter::normalizeFmacExactResult: lane flags Z1 S2 U4 O8; the
    // value is replaced on zero, overflow and underflow.
    inline uint8_t fx(float &value, double exact)
    {
        const bool negative = std::signbit(exact);
        const double magnitude = std::fabs(exact);
        uint8_t flags = negative ? 0x2u : 0u;
        uint32_t bits = negative ? 0x80000000u : 0u;
        if (magnitude == 0.0)
        {
            flags |= 0x1u;
            std::memcpy(&value, &bits, sizeof(value));
        }
        else if (magnitude > static_cast<double>(std::numeric_limits<float>::max()))
        {
            flags |= 0x8u;
            bits |= 0x7F7FFFFFu;
            std::memcpy(&value, &bits, sizeof(value));
        }
        else if (magnitude < static_cast<double>(std::numeric_limits<float>::min()))
        {
            flags |= 0x5u;
            std::memcpy(&value, &bits, sizeof(value));
        }
        return flags;
    }

    inline uint8_t exactFlags(double exact)
    {
        float ignored = 0.0f;
        return fx(ignored, exact);
    }

    // Fast paths for fx. Under round-toward-zero a float result r of one
    // operation satisfies |r| <= |exact| < |r| + ulp(r), so when r's exponent is
    // in [1, 253] the exact value is a normal float of the same sign: fx
    // returns only the sign flag and keeps r. Otherwise the interpreter's
    // double computation decides.
    inline uint32_t expOf(float value)
    {
        uint32_t bits;
        std::memcpy(&bits, &value, sizeof(bits));
        return (bits >> 23) & 0xFFu;
    }

    inline uint8_t signFlag(float value)
    {
        uint32_t bits;
        std::memcpy(&bits, &value, sizeof(bits));
        return static_cast<uint8_t>((bits >> 30) & 2u);
    }

    inline uint8_t fxAdd(float &r, float a, float b)
    {
        if (expOf(r) - 1u <= 252u)
            return signFlag(r);
        return fx(r, static_cast<double>(a) + static_cast<double>(b));
    }

    inline uint8_t fxSub(float &r, float a, float b)
    {
        if (expOf(r) - 1u <= 252u)
            return signFlag(r);
        return fx(r, static_cast<double>(a) - static_cast<double>(b));
    }

    inline uint8_t fxMul(float &r, float a, float b)
    {
        if (expOf(r) - 1u <= 252u)
            return signFlag(r);
        return fx(r, static_cast<double>(a) * static_cast<double>(b));
    }

    // r = acc +/- p with p = RZ(s * t). With exponent(r) in [2, 252] and
    // |p| < 2^17 |r| the two roundings move r by less than 2^-5 |r| from the
    // interpreter's double result, which is then normal with r's sign.
    inline uint8_t fxMadd(float &r, float p, float acc, float s, float t)
    {
        const uint32_t re = expOf(r);
        if (re - 2u <= 250u && expOf(p) <= re + 16u)
            return signFlag(r);
        return fx(r, static_cast<double>(acc) + static_cast<double>(s) * static_cast<double>(t));
    }

    inline uint8_t fxMsub(float &r, float p, float acc, float s, float t)
    {
        const uint32_t re = expOf(r);
        if (re - 2u <= 250u && expOf(p) <= re + 16u)
            return signFlag(r);
        return fx(r, static_cast<double>(acc) - static_cast<double>(s) * static_cast<double>(t));
    }

    // Flags of the exact product s * t (product-sum sticky flags); p = RZ(s * t).
    inline uint8_t prodFlags(float p, float s, float t)
    {
        if (expOf(p) - 1u <= 252u)
            return signFlag(p);
        return static_cast<uint8_t>(exactFlags(static_cast<double>(s) * static_cast<double>(t)) & 0xFu);
    }

    // ------------------------------------------------ 4-lane (SSE) FMAC helpers
    // The same single-precision operations as the scalar forms (same MXCSR), so
    // results are bit-identical; the vector checks only decide whether every
    // destination lane takes the scalar fast path. `lanes` has bit k for lane k
    // (x = 0 .. w = 3). Otherwise the caller runs the exact scalar fx* per lane.
    inline __m128 NV(__m128 v) // N() on four lanes
    {
        const __m128i bits = _mm_castps_si128(v);
        const __m128i exp = _mm_and_si128(bits, _mm_set1_epi32(0x7F800000));
        const __m128i sign = _mm_and_si128(bits, _mm_set1_epi32(static_cast<int>(0x80000000u)));
        __m128i out = _mm_blendv_epi8(bits, sign, _mm_cmpeq_epi32(exp, _mm_setzero_si128()));
        out = _mm_blendv_epi8(out, _mm_or_si128(sign, _mm_set1_epi32(0x7F7FFFFF)),
                              _mm_cmpeq_epi32(exp, _mm_set1_epi32(0x7F800000)));
        return _mm_castsi128_ps(out);
    }
    inline __m128 loadNV(const float *v) { return NV(_mm_loadu_ps(v)); }
    inline __m128i expV(__m128 v) { return _mm_and_si128(_mm_srli_epi32(_mm_castps_si128(v), 23), _mm_set1_epi32(0xFF)); }
    // Sign flags (fast-path flag value) of the lanes.
    inline void signFlagsV(__m128 r, uint8_t f[4])
    {
        const int signs = _mm_movemask_ps(r);
        f[0] = static_cast<uint8_t>((signs << 1) & 2);
        f[1] = static_cast<uint8_t>(signs & 2);
        f[2] = static_cast<uint8_t>((signs >> 1) & 2);
        f[3] = static_cast<uint8_t>((signs >> 2) & 2);
    }
    // fxAdd/fxSub/fxMul fast path on every lane: exponent in [1, 253].
    inline bool fastV(__m128 r, int lanes, uint8_t f[4])
    {
        const __m128i e = expV(r);
        const __m128i bad = _mm_or_si128(_mm_cmpeq_epi32(e, _mm_setzero_si128()), _mm_cmpgt_epi32(e, _mm_set1_epi32(253)));
        if (_mm_movemask_ps(_mm_castsi128_ps(bad)) & lanes)
            return false;
        signFlagsV(r, f);
        return true;
    }
    // fxMadd/fxMsub fast path on every lane: exponent(r) in [2, 252] and
    // exponent(p) <= exponent(r) + 16.
    inline bool fastMaddV(__m128 r, __m128 p, int lanes, uint8_t f[4])
    {
        const __m128i re = expV(r), pe = expV(p);
        const __m128i bad = _mm_or_si128(
            _mm_or_si128(_mm_cmplt_epi32(re, _mm_set1_epi32(2)), _mm_cmpgt_epi32(re, _mm_set1_epi32(252))),
            _mm_cmpgt_epi32(pe, _mm_add_epi32(re, _mm_set1_epi32(16))));
        if (_mm_movemask_ps(_mm_castsi128_ps(bad)) & lanes)
            return false;
        signFlagsV(r, f);
        return true;
    }
    // Exact-zero lanes. A result lane that is +-0 because the exact value is
    // zero gets fx's flags directly: Z, and S for -0 (the value is kept). The
    // exact value is zero for a product with a zero operand, a sum of
    // opposites (s == -t), a difference of equals (s == t), and acc +- s*t
    // with a zero acc and a zero operand. The float and the interpreter's
    // double computation then round the same way (same MXCSR), so they agree
    // on the zero's sign. Operands are N()-normalised: no denormals, no
    // infinities. GT3's vectors often have zero lanes (w = 0), and one such
    // lane sent every lane of the instruction to the scalar fx path.
    inline __m128i zeroBitsV(__m128 v) // lanes that are +-0
    {
        return _mm_cmpeq_epi32(_mm_and_si128(_mm_castps_si128(v), _mm_set1_epi32(0x7FFFFFFF)), _mm_setzero_si128());
    }
    inline __m128i normalExpV(__m128 r) // lanes with an exponent in [1, 253]
    {
        const __m128i e = expV(r);
        return _mm_andnot_si128(_mm_or_si128(_mm_cmpeq_epi32(e, _mm_setzero_si128()), _mm_cmpgt_epi32(e, _mm_set1_epi32(253))),
                                _mm_set1_epi32(-1));
    }
    // fast when every lane is normal or an exact zero; flags as fx. The
    // all-normal case is checked first (as fastV: most instructions).
    inline bool fastOrZeroV(__m128 r, __m128i exactZero, int lanes, uint8_t f[4])
    {
        if (((~_mm_movemask_ps(_mm_castsi128_ps(normalExpV(r)))) & lanes) == 0)
        {
            signFlagsV(r, f);
            return true;
        }
        const __m128i zero = _mm_and_si128(exactZero, zeroBitsV(r));
        const __m128i ok = _mm_or_si128(normalExpV(r), zero);
        if ((~_mm_movemask_ps(_mm_castsi128_ps(ok))) & lanes)
            return false;
        signFlagsV(r, f);
        const int z = _mm_movemask_ps(_mm_castsi128_ps(zero));
        f[0] = static_cast<uint8_t>(f[0] | (z & 1));
        f[1] = static_cast<uint8_t>(f[1] | ((z >> 1) & 1));
        f[2] = static_cast<uint8_t>(f[2] | ((z >> 2) & 1));
        f[3] = static_cast<uint8_t>(f[3] | ((z >> 3) & 1));
        return true;
    }
    inline bool fastMulV(__m128 r, __m128 s, __m128 t, int lanes, uint8_t f[4])
    {
        if (fastV(r, lanes, f))
            return true;
        return fastOrZeroV(r, _mm_or_si128(zeroBitsV(s), zeroBitsV(t)), lanes, f);
    }
    inline bool fastAddV(__m128 r, __m128 s, __m128 t, int lanes, uint8_t f[4])
    {
        if (fastV(r, lanes, f))
            return true;
        const __m128 negT = _mm_xor_ps(t, _mm_castsi128_ps(_mm_set1_epi32(static_cast<int>(0x80000000u))));
        return fastOrZeroV(r, _mm_castps_si128(_mm_cmpeq_ps(s, negT)), lanes, f);
    }
    inline bool fastSubV(__m128 r, __m128 s, __m128 t, int lanes, uint8_t f[4])
    {
        if (fastV(r, lanes, f))
            return true;
        return fastOrZeroV(r, _mm_castps_si128(_mm_cmpeq_ps(s, t)), lanes, f);
    }
    // fastMaddV, plus exact zeros (acc +-0 and a zero product operand).
    inline bool fastMaddZV(__m128 r, __m128 p, __m128 acc, __m128 s, __m128 t, int lanes, uint8_t f[4])
    {
        if (fastMaddV(r, p, lanes, f))
            return true;
        const __m128i re = expV(r), pe = expV(p);
        const __m128i bad = _mm_or_si128(
            _mm_or_si128(_mm_cmplt_epi32(re, _mm_set1_epi32(2)), _mm_cmpgt_epi32(re, _mm_set1_epi32(252))),
            _mm_cmpgt_epi32(pe, _mm_add_epi32(re, _mm_set1_epi32(16))));
        const __m128i zero =
            _mm_and_si128(_mm_and_si128(zeroBitsV(acc), _mm_or_si128(zeroBitsV(s), zeroBitsV(t))), zeroBitsV(r));
        const __m128i ok = _mm_or_si128(_mm_andnot_si128(bad, _mm_set1_epi32(-1)), zero);
        if ((~_mm_movemask_ps(_mm_castsi128_ps(ok))) & lanes)
            return false;
        signFlagsV(r, f);
        const int z = _mm_movemask_ps(_mm_castsi128_ps(zero));
        f[0] = static_cast<uint8_t>(f[0] | (z & 1));
        f[1] = static_cast<uint8_t>(f[1] | ((z >> 1) & 1));
        f[2] = static_cast<uint8_t>(f[2] | ((z >> 2) & 1));
        f[3] = static_cast<uint8_t>(f[3] | ((z >> 3) & 1));
        return true;
    }

    // Value-only forms of the checks above, for results whose flags nobody
    // can read (GT3_VU1T_STICKY=0 at generation): true when every lane keeps
    // the float result, so fx would leave it unchanged; otherwise the caller
    // runs the exact scalar fx per lane for its value (its flags unused).
    inline bool normalV(__m128 r, int lanes)
    {
        return ((~_mm_movemask_ps(_mm_castsi128_ps(normalExpV(r)))) & lanes) == 0;
    }
    inline bool normalOrZeroV(__m128 r, __m128i exactZero, int lanes)
    {
        const __m128i ok = _mm_or_si128(normalExpV(r), _mm_and_si128(exactZero, zeroBitsV(r)));
        return ((~_mm_movemask_ps(_mm_castsi128_ps(ok))) & lanes) == 0;
    }
    inline bool keepMulV(__m128 r, __m128 s, __m128 t, int lanes)
    {
        return normalV(r, lanes) || normalOrZeroV(r, _mm_or_si128(zeroBitsV(s), zeroBitsV(t)), lanes);
    }
    inline bool keepAddV(__m128 r, __m128 s, __m128 t, int lanes)
    {
        if (normalV(r, lanes))
            return true;
        const __m128 negT = _mm_xor_ps(t, _mm_castsi128_ps(_mm_set1_epi32(static_cast<int>(0x80000000u))));
        return normalOrZeroV(r, _mm_castps_si128(_mm_cmpeq_ps(s, negT)), lanes);
    }
    inline bool keepSubV(__m128 r, __m128 s, __m128 t, int lanes)
    {
        return normalV(r, lanes) || normalOrZeroV(r, _mm_castps_si128(_mm_cmpeq_ps(s, t)), lanes);
    }
    inline bool keepMaddV(__m128 r, __m128 p, __m128 acc, __m128 s, __m128 t, int lanes)
    {
        const __m128i re = expV(r), pe = expV(p);
        const __m128i bad = _mm_or_si128(
            _mm_or_si128(_mm_cmplt_epi32(re, _mm_set1_epi32(2)), _mm_cmpgt_epi32(re, _mm_set1_epi32(252))),
            _mm_cmpgt_epi32(pe, _mm_add_epi32(re, _mm_set1_epi32(16))));
        if ((_mm_movemask_ps(_mm_castsi128_ps(bad)) & lanes) == 0)
            return true;
        const __m128i zero =
            _mm_and_si128(_mm_and_si128(zeroBitsV(acc), _mm_or_si128(zeroBitsV(s), zeroBitsV(t))), zeroBitsV(r));
        const __m128i ok = _mm_or_si128(_mm_andnot_si128(bad, _mm_set1_epi32(-1)), zero);
        return ((~_mm_movemask_ps(_mm_castsi128_ps(ok))) & lanes) == 0;
    }

    // OR of prodFlags over the lanes (product-sum sticky flags), p = s * t.
    // A zero product of a zero operand is exact: prodFlags gives Z (+S).
    // The lanes that leave the fast path (see prodFlagsV).
    uint32_t prodFlagsSlowV(__m128 p, __m128 s, __m128 t, int lanes);
    // Forced inline: as a call it was ~3% of the VIF1/VU1 thread.
#if defined(_MSC_VER)
    __forceinline
#else
    inline __attribute__((always_inline))
#endif
    uint32_t prodFlagsV(__m128 p, __m128 s, __m128 t, int lanes)
    {
        const __m128i e = expV(p);
        const __m128i bad = _mm_or_si128(_mm_cmpeq_epi32(e, _mm_setzero_si128()), _mm_cmpgt_epi32(e, _mm_set1_epi32(253)));
        if ((_mm_movemask_ps(_mm_castsi128_ps(bad)) & lanes) == 0)
            return (_mm_movemask_ps(p) & lanes) ? 2u : 0u;
        return prodFlagsSlowV(p, s, t, lanes);
    }

    // MAC bits of one lane's flags (Z/S/U/O -> bit, bit+4, bit+8, bit+12).
    inline uint32_t macBits(uint8_t flags, uint32_t lane)
    {
        const uint32_t f = flags;
        return ((f & 1u) | ((f & 2u) << 3) | ((f & 4u) << 6) | ((f & 8u) << 9)) * lane;
    }

    inline int32_t floatToInt(float value, float scale) // vuFloatToInt
    {
        const double scaled = static_cast<double>(value) * static_cast<double>(scale);
        if (scaled >= static_cast<double>(std::numeric_limits<int32_t>::max()))
            return std::numeric_limits<int32_t>::max();
        if (scaled <= static_cast<double>(std::numeric_limits<int32_t>::min()))
            return std::numeric_limits<int32_t>::min();
        return static_cast<int32_t>(scaled);
    }

    inline float bitsToFloat(uint32_t bits)
    {
        float value;
        std::memcpy(&value, &bits, sizeof(value));
        return value;
    }

    inline uint32_t floatBits(float value)
    {
        uint32_t bits;
        std::memcpy(&bits, &value, sizeof(bits));
        return bits;
    }

    // --------------------------------------------------------------- commits
    void commitSlow(Ctx &c, uint64_t upTo);

    // The small per-instruction helpers below are forced inline: in the large
    // generated blocks the compiler left pushFlags as a call (~3% of the
    // VIF1/VU1 thread with angled side screens).
#if defined(_MSC_VER)
#define VU1T_INLINE __forceinline
#else
#define VU1T_INLINE inline __attribute__((always_inline))
#endif

    VU1T_INLINE void commit(Ctx &c)
    {
        if (c.nextCommit <= c.cycle)
            commitSlow(c, c.cycle);
    }

    // FMAC flag result of one instruction (VU1Interpreter::updateFmacFlags).
    VU1T_INLINE void pushFmacFlags(Ctx &c, const uint8_t laneFlags[4], uint8_t dest, uint32_t extraSticky)
    {
        if (c.flagTail - c.flagHead >= Ctx::kFlagRing - 2u)
            commitSlow(c, c.cycle);
        uint32_t mac = 0u, status = 0u;
        for (uint32_t component = 0; component < 4u; ++component)
        {
            const uint32_t lane = 1u << (3u - component);
            if ((dest & lane) == 0u)
                continue;
            const uint32_t flags = laneFlags[component];
            if (flags & 0x1u)
                mac |= lane;
            if (flags & 0x2u)
                mac |= lane << 4;
            if (flags & 0x4u)
                mac |= lane << 8;
            if (flags & 0x8u)
                mac |= lane << 12;
            status |= flags;
        }
        FlagEntry &e = c.flags[c.flagTail++ % Ctx::kFlagRing];
        e.issue = c.cycle;
        e.ready = c.cycle + 4u;
        e.mac = mac;
        e.status = status;
        e.extraSticky = extraSticky;
        e.writes = kWriteMac | kWriteStatus;
        c.nextCommit = std::min(c.nextCommit, e.ready);
    }

    // FMAC flag result with MAC and status already combined.
    VU1T_INLINE void pushFlags(Ctx &c, uint32_t mac, uint32_t status, uint32_t extraSticky)
    {
        if (c.flagTail - c.flagHead >= Ctx::kFlagRing - 2u)
            commitSlow(c, c.cycle);
        FlagEntry &e = c.flags[c.flagTail++ % Ctx::kFlagRing];
        e.issue = c.cycle;
        e.ready = c.cycle + 4u;
        e.mac = mac;
        e.status = status;
        e.extraSticky = extraSticky;
        e.writes = kWriteMac | kWriteStatus;
        c.nextCommit = std::min(c.nextCommit, e.ready);
    }

    VU1T_INLINE void pushClip(Ctx &c, uint32_t clip)
    {
        if (c.flagTail - c.flagHead >= Ctx::kFlagRing - 2u)
            commitSlow(c, c.cycle);
        c.workingClip = ((c.workingClip << 6) | (clip & 0x3Fu)) & 0xFFFFFFu;
        FlagEntry &e = c.flags[c.flagTail++ % Ctx::kFlagRing];
        e.issue = c.cycle;
        e.ready = c.cycle + 4u;
        e.clip = c.workingClip;
        e.writes = kWriteClip;
        c.nextCommit = std::min(c.nextCommit, e.ready);
    }

    void pushFsset(Ctx &c, uint32_t immediate);
    void pushFcset(Ctx &c, uint32_t clip);

    VU1T_INLINE void queueQ(Ctx &c, float value, uint32_t latency, uint32_t statusDi)
    {
        commit(c);
        c.qPending = true;
        c.qReady = c.cycle + latency;
        c.qValue = normalizeResult(value);
        c.qDi = statusDi & 0x30u;
        c.nextCommit = std::min(c.nextCommit, c.qReady);
    }

    inline void queueP(Ctx &c, float value, uint32_t latency)
    {
        commit(c);
        value = normalizeResult(value);
        for (uint32_t slot = 0; slot < 2u; ++slot)
        {
            if (!c.efuValid[slot])
            {
                c.efuValid[slot] = true;
                c.efuReady[slot] = c.cycle + latency;
                c.efuValue[slot] = value;
                c.efuResourceReady = c.cycle + (latency > 0u ? latency - 1u : 0u);
                c.nextCommit = std::min(c.nextCommit, c.efuReady[slot]);
                return;
            }
        }
        ++c.errors;
    }

    inline uint64_t qWaitCycle(const Ctx &c) { return c.qPending ? c.qReady : 0u; }

    inline uint64_t pWaitCycle(const Ctx &c)
    {
        uint64_t ready = 0u;
        for (uint32_t slot = 0; slot < 2u; ++slot)
            if (c.efuValid[slot])
                ready = std::max(ready, c.efuReady[slot]);
        return ready;
    }

    // ----------------------------------------------------------------- XGKICK
    void kickCatchUp(Ctx &c, uint64_t upTo); // copies scheduled at cycles <= upTo
    uint64_t kickFinish(Ctx &c);             // runs the transfer to its end; returns the finishing cycle

    VU1T_INLINE void beforeStore(Ctx &c)
    {
        if (c.kick.active && c.kick.nextCopy <= c.cycle)
            kickCatchUp(c, c.cycle);
    }

    inline void startKick(Ctx &c, uint32_t qwordAddress)
    {
        c.kick.active = true;
        c.kick.tagEop = false;
        c.kick.source = (qwordAddress * 16u) % kDataSize;
        c.kick.copied = 0;
        c.kick.tagEnd = 0;
        c.kick.total = 0;
        c.kick.nextCopy = c.cycle + 1u;
    }

    VU1T_INLINE void store(Ctx &c, uint32_t address, const uint32_t words[4], uint8_t dest)
    {
        beforeStore(c);
        uint8_t *q = c.mem + address;
        if (dest & 0x8u)
            std::memcpy(q + 0, &words[0], 4);
        if (dest & 0x4u)
            std::memcpy(q + 4, &words[1], 4);
        if (dest & 0x2u)
            std::memcpy(q + 8, &words[2], 4);
        if (dest & 0x1u)
            std::memcpy(q + 12, &words[3], 4);
        // (ready at cycle + 1, which the pair's own cycle already reaches)
    }

    // Ends a program: runs the XGKICK transfer out, commits every pipeline and
    // advances to the cycle at which the interpreter's flush would stop.
    void finish(Ctx &c);

    // ------------------------------------------------------------- registry
    using ProgramFn = uint32_t (*)(Ctx &c, uint32_t startPc); // returns 0 when startPc is unsupported, 1 on success

    struct TranslatedImage
    {
        uint64_t hash; // FNV-1a 64 of the 16 KiB code image
        ProgramFn run;
        const char *name;
    };

    uint64_t hashCode(const uint8_t *code, uint32_t size);
    const TranslatedImage *findImage(uint64_t hash);
    const TranslatedImage *findVu0Image(uint64_t hash); // VU0 microprograms (4 KiB code)
    constexpr uint32_t kVu0CodeSize = 0x1000u;
    constexpr uint32_t kVu0DataSize = 0x1000u;
}

#endif
