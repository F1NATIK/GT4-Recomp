#ifndef PS2_RUNTIME_MACROS_H
#define PS2_RUNTIME_MACROS_H
#include <cstdint>
#include <cmath>
#include <cstring>
#include <bit>
#if defined(_MSC_VER)
#include <intrin.h>
#elif defined(USE_SSE2NEON)
#include "sse2neon.h"
#else
#include <immintrin.h> // For SSE/AVX intrinsics
#endif

#include "ps2_runtime.h"

static inline int32_t Ps2ExtractEpi32(__m128i v, int index)
{
    switch (index & 3)
    {
    case 0:
        return _mm_extract_epi32(v, 0);
    case 1:
        return _mm_extract_epi32(v, 1);
    case 2:
        return _mm_extract_epi32(v, 2);
    default:
        return _mm_extract_epi32(v, 3);
    }
}

static inline int64_t Ps2ExtractEpi64(__m128i v, int index)
{
    if ((index & 1) == 0)
    {
        return _mm_cvtsi128_si64(v);
    }
    else
    {
        return _mm_extract_epi64(v, 1);
    }
}

static inline uint32_t ps2_clz32(uint32_t x)
{
    return static_cast<uint32_t>(std::countl_zero(x));
}

static inline uint64_t Ps2HiLoToU64(uint64_t hi, uint64_t lo)
{
    return ((hi & 0xFFFFFFFFull) << 32) | (lo & 0xFFFFFFFFull);
}

static inline uint64_t Ps2SignExt32ToU64(uint32_t v)
{
    return (uint64_t)(int64_t)(int32_t)v;
}

// PLZCW: Count leading bits that match the sign bit, minus 1.
// For positive values: count leading zeros minus 1 (excludes sign bit).
// For negative values: count leading ones minus 1 (excludes sign bit).
// Special cases: 0x00000000 -> 31, 0xFFFFFFFF -> 31.
static inline uint32_t ps2_plzcw32(uint32_t x)
{
    if (x == 0 || x == 0xFFFFFFFF)
        return 31;
    if (x & 0x80000000u)
        x = ~x; // If sign bit set, invert to count leading ones as zeros
    return static_cast<uint32_t>(std::countl_zero(x)) - 1;
}

#define PS2_BLENDV_PS(a, b, mask) _mm_blendv_ps((a), (b), (mask))
#define PS2_MIN_EPI32(a, b) _mm_min_epi32((a), (b))
#define PS2_MAX_EPI32(a, b) _mm_max_epi32((a), (b))
#define PS2_SHUFFLE_EPI8(v, mask) _mm_shuffle_epi8((v), (mask))

#define PS2_EXTRACT_EPI32(v, i) Ps2ExtractEpi32((v), (i))
#define PS2_EXTRACT_EPI64(v, i) Ps2ExtractEpi64((v), (i))

#define PS2_EXTRACT_EPI32_0(v) Ps2ExtractEpi32((v), 0)
#define PS2_EXTRACT_EPI32_1(v) Ps2ExtractEpi32((v), 1)
#define PS2_EXTRACT_EPI32_2(v) Ps2ExtractEpi32((v), 2)
#define PS2_EXTRACT_EPI32_3(v) Ps2ExtractEpi32((v), 3)

#define PS2_EXTRACT_EPI64_0(v) Ps2ExtractEpi64((v), 0)
#define PS2_EXTRACT_EPI64_1(v) Ps2ExtractEpi64((v), 1)

// Basic MIPS arithmetic operations
#define ADD32(a, b) ((uint32_t)((a) + (b)))
#define ADD32_OV(rs, rt, result32, overflow)              \
    do                                                    \
    {                                                     \
        int32_t _a = (int32_t)(rs);                       \
        int32_t _b = (int32_t)(rt);                       \
        int32_t _r = _a + _b;                             \
        overflow = (((_a ^ _b) >= 0) && ((_a ^ _r) < 0)); \
        result32 = (uint32_t)_r;                          \
    } while (0);
#define SUB32(a, b) ((uint32_t)((a) - (b)))
#define SUB32_OV(rs, rt, result32, overflow)             \
    do                                                   \
    {                                                    \
        int32_t _a = (int32_t)(rs);                      \
        int32_t _b = (int32_t)(rt);                      \
        int32_t _r = _a - _b;                            \
        overflow = (((_a ^ _b) < 0) && ((_a ^ _r) < 0)); \
        result32 = (uint32_t)_r;                         \
    } while (0);
#define MUL32(a, b) ((uint32_t)((a) * (b)))
#define DIV32(a, b) ((uint32_t)((a) / (b)))
#define AND32(a, b) ((uint32_t)((a) & (b)))
#define OR32(a, b) ((uint32_t)((a) | (b)))
#define XOR32(a, b) ((uint32_t)((a) ^ (b)))
#define NOR32(a, b) ((uint32_t)(~((a) | (b))))
#define SLL32(a, b) ((uint32_t)((a) << (b)))
#define SRL32(a, b) ((uint32_t)((a) >> (b)))
#define SRA32(a, b) ((uint32_t)((int32_t)(a) >> (b)))
#define SLT32(a, b) ((uint32_t)((int32_t)(a) < (int32_t)(b) ? 1 : 0))
#define SLTU32(a, b) ((uint32_t)((a) < (b) ? 1 : 0))

// PS2-specific 128-bit MMI operations
#define PS2_PEXTLW(a, b) _mm_unpacklo_epi32((__m128i)(b), (__m128i)(a))
#define PS2_PEXTUW(a, b) _mm_unpackhi_epi32((__m128i)(b), (__m128i)(a))
#define PS2_PEXTLH(a, b) _mm_unpacklo_epi16((__m128i)(b), (__m128i)(a))
#define PS2_PEXTUH(a, b) _mm_unpackhi_epi16((__m128i)(b), (__m128i)(a))
#define PS2_PEXTLB(a, b) _mm_unpacklo_epi8((__m128i)(b), (__m128i)(a))
#define PS2_PEXTUB(a, b) _mm_unpackhi_epi8((__m128i)(b), (__m128i)(a))
#define PS2_PADDW(a, b) _mm_add_epi32((__m128i)(a), (__m128i)(b))
#define PS2_PSUBW(a, b) _mm_sub_epi32((__m128i)(a), (__m128i)(b))
#define PS2_PMAXW(a, b) PS2_MAX_EPI32((__m128i)(a), (__m128i)(b))
#define PS2_PMINW(a, b) PS2_MIN_EPI32((__m128i)(a), (__m128i)(b))
#define PS2_PADDH(a, b) _mm_add_epi16((__m128i)(a), (__m128i)(b))
#define PS2_PSUBH(a, b) _mm_sub_epi16((__m128i)(a), (__m128i)(b))
#define PS2_PMAXH(a, b) _mm_max_epi16((__m128i)(a), (__m128i)(b))
#define PS2_PMINH(a, b) _mm_min_epi16((__m128i)(a), (__m128i)(b))
#define PS2_PADDB(a, b) _mm_add_epi8((__m128i)(a), (__m128i)(b))
#define PS2_PSUBB(a, b) _mm_sub_epi8((__m128i)(a), (__m128i)(b))
#define PS2_PAND(a, b) _mm_and_si128((__m128i)(a), (__m128i)(b))
#define PS2_POR(a, b) _mm_or_si128((__m128i)(a), (__m128i)(b))
#define PS2_PXOR(a, b) _mm_xor_si128((__m128i)(a), (__m128i)(b))
#define PS2_PNOR(a, b) _mm_xor_si128(_mm_or_si128((__m128i)(a), (__m128i)(b)), _mm_set1_epi32(0xFFFFFFFF))

// PS2 float semantics (EE FPU and VU): there is no Inf/NaN. Exponent 255 is an
// ordinary huge value, denormals are zero, and overflow saturates to +/-Fmax
// instead of producing Inf. Host IEEE math instead turns x/0 into Inf and then
// Inf*0 into NaN (0xFFC00000), which poisoned GT3's race geometry. Operands and
// results are therefore clamped: exponent 255 -> +/-FLT_MAX (the host stand-in
// for Fmax, as in the VU1 interpreter), exponent 0 -> signed zero.
inline float ps2_float_clamp(float value)
{
    uint32_t bits = std::bit_cast<uint32_t>(value);
    const uint32_t exponent = bits & 0x7F800000u;
    if (exponent == 0x7F800000u)
        bits = (bits & 0x80000000u) | 0x7F7FFFFFu;
    else if (exponent == 0u)
        bits &= 0x80000000u;
    return std::bit_cast<float>(bits);
}
inline __m128 ps2_vector_clamp(__m128 value)
{
    const __m128i bits = _mm_castps_si128(value);
    const __m128i exponentMask = _mm_set1_epi32(0x7F800000);
    const __m128i exponent = _mm_and_si128(bits, exponentMask);
    const __m128i isHuge = _mm_cmpeq_epi32(exponent, exponentMask);
    const __m128i isZero = _mm_cmpeq_epi32(exponent, _mm_setzero_si128());
    const __m128i sign = _mm_and_si128(bits, _mm_set1_epi32(static_cast<int>(0x80000000u)));
    const __m128i fmax = _mm_or_si128(sign, _mm_set1_epi32(0x7F7FFFFF));
    __m128i result = _mm_or_si128(_mm_andnot_si128(isHuge, bits), _mm_and_si128(isHuge, fmax));
    result = _mm_or_si128(_mm_andnot_si128(isZero, result), _mm_and_si128(isZero, sign));
    return _mm_castsi128_ps(result);
}

// PS2 VU (Vector Unit) operations
#define PS2_VADD(a, b) ps2_vector_clamp(_mm_add_ps(ps2_vector_clamp((__m128)(a)), ps2_vector_clamp((__m128)(b))))
#define PS2_VSUB(a, b) ps2_vector_clamp(_mm_sub_ps(ps2_vector_clamp((__m128)(a)), ps2_vector_clamp((__m128)(b))))
#define PS2_VMUL(a, b) ps2_vector_clamp(_mm_mul_ps(ps2_vector_clamp((__m128)(a)), ps2_vector_clamp((__m128)(b))))
#define PS2_VDIV(a, b) ps2_vector_clamp(_mm_div_ps(ps2_vector_clamp((__m128)(a)), ps2_vector_clamp((__m128)(b))))
#define PS2_VMULQ(a, q) PS2_VMUL((a), _mm_set1_ps(q))
#define PS2_VBLEND(a, b, mask) PS2_BLENDV_PS((__m128)(a), (__m128)(b), (__m128)(mask))

// Memory access helpers - Hybrid Fast/Slow Path
// Fast path: Direct RDRAM access (masked).
// Slow path: Full runtime->Load/Store

static inline bool Ps2FastRangeIsContiguous(uint32_t offset, uint32_t bytes)
{
    return offset <= (PS2_RAM_SIZE - bytes);
}

static inline uint8_t Ps2FastRead8(const uint8_t *rdram, uint32_t addr)
{
    return rdram[addr & PS2_RAM_MASK];
}

static inline uint16_t Ps2FastRead16(const uint8_t *rdram, uint32_t addr)
{
    const uint32_t offset = addr & PS2_RAM_MASK;
    if (!Ps2FastRangeIsContiguous(offset, sizeof(uint16_t)))
    {
        uint8_t wrapped[sizeof(uint16_t)];
        for (uint32_t i = 0; i < sizeof(uint16_t); ++i)
        {
            wrapped[i] = rdram[(offset + i) & PS2_RAM_MASK];
        }
        uint16_t value;
        std::memcpy(&value, wrapped, sizeof(value));
        return value;
    }

    uint16_t value;
    std::memcpy(&value, rdram + offset, sizeof(value));
    return value;
}

static inline uint32_t Ps2FastRead32(const uint8_t *rdram, uint32_t addr)
{
    const uint32_t offset = addr & PS2_RAM_MASK;
    if (!Ps2FastRangeIsContiguous(offset, sizeof(uint32_t)))
    {
        uint8_t wrapped[sizeof(uint32_t)];
        for (uint32_t i = 0; i < sizeof(uint32_t); ++i)
        {
            wrapped[i] = rdram[(offset + i) & PS2_RAM_MASK];
        }
        uint32_t value;
        std::memcpy(&value, wrapped, sizeof(value));
        return value;
    }

    uint32_t value;
    std::memcpy(&value, rdram + offset, sizeof(value));
    return value;
}

static inline uint64_t Ps2FastRead64(const uint8_t *rdram, uint32_t addr)
{
    const uint32_t offset = addr & PS2_RAM_MASK;
    if (!Ps2FastRangeIsContiguous(offset, sizeof(uint64_t)))
    {
        uint8_t wrapped[sizeof(uint64_t)];
        for (uint32_t i = 0; i < sizeof(uint64_t); ++i)
        {
            wrapped[i] = rdram[(offset + i) & PS2_RAM_MASK];
        }
        uint64_t value;
        std::memcpy(&value, wrapped, sizeof(value));
        return value;
    }

    uint64_t value;
    std::memcpy(&value, rdram + offset, sizeof(value));
    return value;
}

static inline __m128i Ps2FastRead128(const uint8_t *rdram, uint32_t addr)
{
    const uint32_t offset = addr & PS2_RAM_MASK;
    if (!Ps2FastRangeIsContiguous(offset, sizeof(__m128i)))
    {
        alignas(16) uint8_t wrapped[sizeof(__m128i)];
        for (uint32_t i = 0; i < sizeof(__m128i); ++i)
        {
            wrapped[i] = rdram[(offset + i) & PS2_RAM_MASK];
        }
        __m128i value;
        std::memcpy(&value, wrapped, sizeof(value));
        return value;
    }

    __m128i value;
    std::memcpy(&value, rdram + offset, sizeof(value));
    return value;
}

static inline void Ps2FastWrite8(uint8_t *rdram, uint32_t addr, uint8_t value)
{
    rdram[addr & PS2_RAM_MASK] = value;
}

static inline void Ps2FastWrite16(uint8_t *rdram, uint32_t addr, uint16_t value)
{
    const uint32_t offset = addr & PS2_RAM_MASK;
    if (!Ps2FastRangeIsContiguous(offset, sizeof(uint16_t)))
    {
        uint8_t wrapped[sizeof(uint16_t)];
        std::memcpy(wrapped, &value, sizeof(value));
        for (uint32_t i = 0; i < sizeof(uint16_t); ++i)
        {
            rdram[(offset + i) & PS2_RAM_MASK] = wrapped[i];
        }
        return;
    }
    std::memcpy(rdram + offset, &value, sizeof(value));
}

static inline void Ps2FastWrite32(uint8_t *rdram, uint32_t addr, uint32_t value)
{
    const uint32_t offset = addr & PS2_RAM_MASK;
    if (!Ps2FastRangeIsContiguous(offset, sizeof(uint32_t)))
    {
        uint8_t wrapped[sizeof(uint32_t)];
        std::memcpy(wrapped, &value, sizeof(value));
        for (uint32_t i = 0; i < sizeof(uint32_t); ++i)
        {
            rdram[(offset + i) & PS2_RAM_MASK] = wrapped[i];
        }
        return;
    }
    std::memcpy(rdram + offset, &value, sizeof(value));
}

static inline void Ps2FastWrite64(uint8_t *rdram, uint32_t addr, uint64_t value)
{
    const uint32_t offset = addr & PS2_RAM_MASK;
    if (!Ps2FastRangeIsContiguous(offset, sizeof(uint64_t)))
    {
        uint8_t wrapped[sizeof(uint64_t)];
        std::memcpy(wrapped, &value, sizeof(value));
        for (uint32_t i = 0; i < sizeof(uint64_t); ++i)
        {
            rdram[(offset + i) & PS2_RAM_MASK] = wrapped[i];
        }
        return;
    }
    std::memcpy(rdram + offset, &value, sizeof(value));
}

static inline void Ps2FastWrite128(uint8_t *rdram, uint32_t addr, __m128i value)
{
    const uint32_t offset = addr & PS2_RAM_MASK;
    if (!Ps2FastRangeIsContiguous(offset, sizeof(__m128i)))
    {
        alignas(16) uint8_t wrapped[sizeof(__m128i)];
        std::memcpy(wrapped, &value, sizeof(value));
        for (uint32_t i = 0; i < sizeof(__m128i); ++i)
        {
            rdram[(offset + i) & PS2_RAM_MASK] = wrapped[i];
        }
        return;
    }
    std::memcpy(rdram + offset, &value, sizeof(value));
}

#define FAST_READ8(addr) Ps2FastRead8(rdram, (uint32_t)(addr))
#define FAST_READ16(addr) Ps2FastRead16(rdram, (uint32_t)(addr))
#define FAST_READ32(addr) Ps2FastRead32(rdram, (uint32_t)(addr))
#define FAST_READ64(addr) Ps2FastRead64(rdram, (uint32_t)(addr))
#define FAST_READ128(addr) Ps2FastRead128(rdram, (uint32_t)(addr))

#define FAST_WRITE8(addr, val) Ps2FastWrite8(rdram, (uint32_t)(addr), (uint8_t)(val))
#define FAST_WRITE16(addr, val) Ps2FastWrite16(rdram, (uint32_t)(addr), (uint16_t)(val))
#define FAST_WRITE32(addr, val) Ps2FastWrite32(rdram, (uint32_t)(addr), (uint32_t)(val))
#define FAST_WRITE64(addr, val) Ps2FastWrite64(rdram, (uint32_t)(addr), (uint64_t)(val))
#define FAST_WRITE128(addr, val) Ps2FastWrite128(rdram, (uint32_t)(addr), (val))

#define READ8(addr) ([&]() -> uint8_t {                       \
    uint32_t _addr = (uint32_t)(addr);                        \
    return PS2Runtime::isSpecialAddress(_addr)                \
        ? runtime->Load8(rdram, ctx, _addr)                   \
        : FAST_READ8(_addr); }())

#define READ16(addr) ([&]() -> uint16_t {                     \
    uint32_t _addr = (uint32_t)(addr);                        \
    return PS2Runtime::isSpecialAddress(_addr)                \
        ? runtime->Load16(rdram, ctx, _addr)                  \
        : FAST_READ16(_addr); }())

#define READ32(addr) ([&]() -> uint32_t {                     \
    uint32_t _addr = (uint32_t)(addr);                        \
    return PS2Runtime::isSpecialAddress(_addr)                \
        ? runtime->Load32(rdram, ctx, _addr)                  \
        : FAST_READ32(_addr); }())

#define READ64(addr) ([&]() -> uint64_t {                     \
    uint32_t _addr = (uint32_t)(addr);                        \
    return PS2Runtime::isSpecialAddress(_addr)                \
        ? runtime->Load64(rdram, ctx, _addr)                  \
        : FAST_READ64(_addr); }())

#define READ128(addr) ([&]() -> __m128i {                     \
    uint32_t _addr = (uint32_t)(addr);                        \
    return PS2Runtime::isSpecialAddress(_addr)                \
        ? runtime->Load128(rdram, ctx, _addr)                 \
        : FAST_READ128(_addr); }())

#define WRITE8(addr, val)                                                            \
    do                                                                               \
    {                                                                                \
        uint32_t _addr = (addr);                                                     \
        if (PS2Runtime::isSpecialAddress(_addr))                                     \
            runtime->Store8(rdram, ctx, _addr, (val));                               \
        else                                                                         \
        {                                                                            \
            ps2TraceGuestWrite(rdram, _addr, 1u, (uint8_t)(val), 0u, "WRITE8", ctx); \
            FAST_WRITE8(_addr, (val));                                               \
        }                                                                            \
    } while (0)

#define WRITE16(addr, val)                                                             \
    do                                                                                 \
    {                                                                                  \
        uint32_t _addr = (addr);                                                       \
        if (PS2Runtime::isSpecialAddress(_addr))                                       \
            runtime->Store16(rdram, ctx, _addr, (val));                                \
        else                                                                           \
        {                                                                              \
            ps2TraceGuestWrite(rdram, _addr, 2u, (uint16_t)(val), 0u, "WRITE16", ctx); \
            FAST_WRITE16(_addr, (val));                                                \
        }                                                                              \
    } while (0)

// GT3 debugging probe on one stack word; that address is inside GT4's main stack.
#if defined(PS2X_GAME_GT3)
#define PS2X_WRITE32_PROBE(_addr, val) \
    if (((_addr) & 0x01FFFFFFu) == 0x01FFC20Cu) { \
        std::printf("[GT3 WRITE] pc=%08X addr=%08X val=%08X\n", (uint32_t)ctx->pc, (_addr), (uint32_t)(val)); \
    }
#else
#define PS2X_WRITE32_PROBE(_addr, val) ((void)0)
#endif

#define WRITE32(addr, val)                                                             \
    do                                                                                 \
    {                                                                                  \
        uint32_t _addr = (addr); \
        PS2X_WRITE32_PROBE(_addr, (val));                                                \
        if (PS2Runtime::isSpecialAddress(_addr))                                       \
            runtime->Store32(rdram, ctx, _addr, (val));                                \
        else                                                                           \
        {                                                                              \
            ps2TraceGuestWrite(rdram, _addr, 4u, (uint32_t)(val), 0u, "WRITE32", ctx); \
            FAST_WRITE32(_addr, (val));                                                \
        }                                                                              \
    } while (0)

#define WRITE64(addr, val)                                                             \
    do                                                                                 \
    {                                                                                  \
        uint32_t _addr = (addr);                                                       \
        if (PS2Runtime::isSpecialAddress(_addr))                                       \
            runtime->Store64(rdram, ctx, _addr, (val));                                \
        else                                                                           \
        {                                                                              \
            ps2TraceGuestWrite(rdram, _addr, 8u, (uint64_t)(val), 0u, "WRITE64", ctx); \
            FAST_WRITE64(_addr, (val));                                                \
        }                                                                              \
    } while (0)

#define WRITE128(addr, val)                                                          \
    do                                                                               \
    {                                                                                \
        uint32_t _addr = (addr);                                                     \
        __m128i _value = (val);                                                      \
        if (PS2Runtime::isSpecialAddress(_addr))                                     \
            runtime->Store128(rdram, ctx, _addr, _value);                            \
        else                                                                         \
        {                                                                            \
            const uint64_t _lo = static_cast<uint64_t>(PS2_EXTRACT_EPI64_0(_value)); \
            const uint64_t _hi = static_cast<uint64_t>(PS2_EXTRACT_EPI64_1(_value)); \
            ps2TraceGuestWrite(rdram, _addr, 16u, _lo, _hi, "WRITE128", ctx);        \
            FAST_WRITE128(_addr, _value);                                            \
        }                                                                            \
    } while (0)

// Packed Compare Greater Than (PCGT)
#define PS2_PCGTW(a, b) _mm_cmpgt_epi32((__m128i)(a), (__m128i)(b))
#define PS2_PCGTH(a, b) _mm_cmpgt_epi16((__m128i)(a), (__m128i)(b))
#define PS2_PCGTB(a, b) _mm_cmpgt_epi8((__m128i)(a), (__m128i)(b))

// Packed Add with Signed Saturation Word (PADDSW)
inline __m128i ps2_paddsw(__m128i a, __m128i b)
{

    __m128i sum = _mm_add_epi32(a, b);
    // Check for over/underflow. Clamp to either INT32_MIN/INT32_MAX.
    __m128i overflow = _mm_and_si128(_mm_xor_si128(a, sum),
                                     _mm_xor_si128(b, sum));
    // Extract input sign.
    overflow = _mm_srai_epi32(overflow, 31);
    __m128i input_sign = _mm_srai_epi32(a, 31);
    // Select saturation value based on overflow sign.
    #if defined(__SSE4_1__)
    __m128i sat = _mm_blendv_epi8(
        _mm_set1_epi32(INT32_MAX),
        _mm_set1_epi32(INT32_MIN),
        input_sign);
    return _mm_blendv_epi8(sum, sat, overflow);
    #else
    __m128i sat = _mm_or_si128(_mm_and_si128(input_sign, _mm_set1_epi32(INT32_MIN)),
                               _mm_andnot_si128(input_sign, _mm_set1_epi32(INT32_MAX)));
    return _mm_or_si128(_mm_and_si128(overflow, sat),
                        _mm_andnot_si128(overflow, sum));
    #endif
}
#define PS2_PADDSW(a, b) ps2_paddsw((__m128i)(a), (__m128i)(b))

// Packed Subtract with Signed Saturation Word (PSUBSW)
inline __m128i ps2_psubsw(__m128i a, __m128i b)
{
    __m128i diff = _mm_sub_epi32(a, b);
    // Check for over/underflow. Clamp to either INT32_MIN/INT32_MAX.
    __m128i overflow = _mm_and_si128(_mm_xor_si128(a, b),
                                     _mm_xor_si128(a, diff));
    // Extract input sign.
    overflow = _mm_srai_epi32(overflow, 31);
    __m128i input_sign = _mm_srai_epi32(a, 31);
    // Select saturation value based on overflow sign.
    #if defined(__SSE4_1__)
    __m128i sat = _mm_blendv_epi8(
        _mm_set1_epi32(INT32_MAX),
        _mm_set1_epi32(INT32_MIN),
        input_sign);
    return _mm_blendv_epi8(diff, sat, overflow);
    #else
    __m128i sat = _mm_or_si128(_mm_and_si128(input_sign, _mm_set1_epi32(INT32_MIN)),
                               _mm_andnot_si128(input_sign, _mm_set1_epi32(INT32_MAX)));
    return _mm_or_si128(_mm_and_si128(overflow, sat),
                        _mm_andnot_si128(overflow, diff));
    #endif

}
#define PS2_PSUBSW(a, b) ps2_psubsw((__m128i)(a), (__m128i)(b))

// Packed Compare Equal (PCEQ)
#define PS2_PCEQW(a, b) _mm_cmpeq_epi32((__m128i)(a), (__m128i)(b))
#define PS2_PCEQH(a, b) _mm_cmpeq_epi16((__m128i)(a), (__m128i)(b))
#define PS2_PCEQB(a, b) _mm_cmpeq_epi8((__m128i)(a), (__m128i)(b))

// Packed Absolute (PABS)
#define PS2_PABSW(a) _mm_abs_epi32((__m128i)(a))
#define PS2_PABSH(a) _mm_abs_epi16((__m128i)(a))
#define PS2_PABSB(a) _mm_abs_epi8((__m128i)(a))

// Packed Pack (PPAC) - Packs larger elements into smaller ones
inline __m128i ps2_paddu32(__m128i a, __m128i b)
{
    __m128i sum = _mm_add_epi32(a, b);
    __m128i overflow = _mm_cmpgt_epi32(_mm_xor_si128(a, _mm_set1_epi32(INT32_MIN)),
                                       _mm_xor_si128(sum, _mm_set1_epi32(INT32_MIN)));
    return _mm_or_si128(sum, overflow); // overflow lanes become all-1s
}
inline __m128i ps2_psubu32(__m128i a, __m128i b)
{
    __m128i diff = _mm_sub_epi32(a, b);
    // Underflow if a < b (unsigned). Clamp to 0.
    __m128i underflow = _mm_cmpgt_epi32(_mm_xor_si128(b, _mm_set1_epi32(INT32_MIN)),
                                        _mm_xor_si128(a, _mm_set1_epi32(INT32_MIN)));
    return _mm_andnot_si128(underflow, diff); // underflow lanes become 0
}

inline __m128i ps2_ppacw(__m128i rs, __m128i rt)
{
    // rs = [rs3 rs2 rs1 rs0], rt = [rt3 rt2 rt1 rt0]
    return _mm_castps_si128(_mm_shuffle_ps(_mm_castsi128_ps(rt), _mm_castsi128_ps(rs), _MM_SHUFFLE(2, 0, 2, 0)));
}
#define PS2_PPACW(a, b) ps2_ppacw((__m128i)(a), (__m128i)(b))

inline __m128i ps2_ppach(__m128i rs, __m128i rt)
{
    const __m128i mask = _mm_setr_epi8(
        0, 1, 4, 5, 8, 9, 12, 13,  // from rt: halfwords 0,2,4,6
        0, 1, 4, 5, 8, 9, 12, 13); // from rs: halfwords 0,2,4,6
    __m128i lo = _mm_shuffle_epi8(rt, mask);
    __m128i hi = _mm_shuffle_epi8(rs, mask);
    return _mm_unpacklo_epi64(lo, hi);
}
#define PS2_PPACH(a, b) ps2_ppach((__m128i)(a), (__m128i)(b))

inline __m128i ps2_ppacb(__m128i rs, __m128i rt)
{
    const __m128i mask = _mm_setr_epi8(
        0, 2, 4, 6, 8, 10, 12, 14,  // from rt: bytes 0,2,4,6,8,10,12,14
        0, 2, 4, 6, 8, 10, 12, 14); // from rs
    __m128i lo = _mm_shuffle_epi8(rt, mask);
    __m128i hi = _mm_shuffle_epi8(rs, mask);
    return _mm_unpacklo_epi64(lo, hi);
}
#define PS2_PPACB(a, b) ps2_ppacb((__m128i)(a), (__m128i)(b))

// Packed Interleave (PINT)
#define PS2_PINTH(a, b) _mm_unpacklo_epi16(_mm_shuffle_epi32((__m128i)(b), _MM_SHUFFLE(3, 2, 1, 0)), _mm_shuffle_epi32((__m128i)(a), _MM_SHUFFLE(3, 2, 1, 0)))
#define PS2_PINTEH(a, b) _mm_unpackhi_epi16(_mm_shuffle_epi32((__m128i)(b), _MM_SHUFFLE(3, 2, 1, 0)), _mm_shuffle_epi32((__m128i)(a), _MM_SHUFFLE(3, 2, 1, 0)))

// Packed Multiply-Add (PMADD)
#define PS2_PMADDW(a, b) _mm_add_epi32(_mm_mullo_epi32(_mm_shuffle_epi32((__m128i)(a), _MM_SHUFFLE(1, 0, 3, 2)), _mm_shuffle_epi32((__m128i)(b), _MM_SHUFFLE(1, 0, 3, 2))), _mm_mullo_epi32(_mm_shuffle_epi32((__m128i)(a), _MM_SHUFFLE(3, 2, 1, 0)), _mm_shuffle_epi32((__m128i)(b), _MM_SHUFFLE(3, 2, 1, 0))))

// Packed Variable Shifts
#define PS2_PSLLVW(a, b) _mm_custom_sllv_epi32((__m128i)(a), (__m128i)(b))
#define PS2_PSRLVW(a, b) _mm_custom_srlv_epi32((__m128i)(a), (__m128i)(b))
#define PS2_PSRAVW(a, b) _mm_custom_srav_epi32((__m128i)(a), (__m128i)(b))

inline __m128i _mm_custom_sllv_epi32(__m128i a, __m128i count)
{
    alignas(16) int32_t a_arr[4];
    alignas(16) int32_t count_arr[4];
    alignas(16) int32_t result[4];

    std::memcpy(a_arr, &a, sizeof(a));
    std::memcpy(count_arr, &count, sizeof(count));

    for (int i = 0; i < 4; i++)
    {
        result[i] = a_arr[i] << (count_arr[i] & 0x1F);
    }

    __m128i out;
    std::memcpy(&out, result, sizeof(out));
    return out;
}

inline __m128i _mm_custom_srlv_epi32(__m128i a, __m128i count)
{
    int32_t a_arr[4], count_arr[4], result[4];
    _mm_storeu_si128((__m128i *)a_arr, a);
    _mm_storeu_si128((__m128i *)count_arr, count);
    for (int i = 0; i < 4; i++)
    {
        result[i] = (uint32_t)a_arr[i] >> (count_arr[i] & 0x1F);
    }
    return _mm_loadu_si128((__m128i *)result);
}

inline __m128i _mm_custom_srav_epi32(__m128i a, __m128i count)
{
    int32_t a_arr[4], count_arr[4], result[4];
    _mm_storeu_si128((__m128i *)a_arr, a);
    _mm_storeu_si128((__m128i *)count_arr, count);
    for (int i = 0; i < 4; i++)
    {
        result[i] = a_arr[i] >> (count_arr[i] & 0x1F);
    }
    return _mm_loadu_si128((__m128i *)result);
}

// 128-bit HI/LO as the MMI instructions see them: LO = {lo1:lo}, HI = {hi1:hi}
// (lo/hi hold bits 63:0, lo1/hi1 bits 127:64, which MULT1/DIV1 also use).
struct Ps2HiLo
{
    uint32_t lo[4];
    uint32_t hi[4];
};

static inline Ps2HiLo ps2_hilo_load(const R5900Context *ctx)
{
    Ps2HiLo v;
    v.lo[0] = (uint32_t)ctx->lo;
    v.lo[1] = (uint32_t)(ctx->lo >> 32);
    v.lo[2] = (uint32_t)ctx->lo1;
    v.lo[3] = (uint32_t)(ctx->lo1 >> 32);
    v.hi[0] = (uint32_t)ctx->hi;
    v.hi[1] = (uint32_t)(ctx->hi >> 32);
    v.hi[2] = (uint32_t)ctx->hi1;
    v.hi[3] = (uint32_t)(ctx->hi1 >> 32);
    return v;
}

static inline void ps2_hilo_store(R5900Context *ctx, const Ps2HiLo &v)
{
    ctx->lo = (uint64_t)v.lo[0] | ((uint64_t)v.lo[1] << 32);
    ctx->lo1 = (uint64_t)v.lo[2] | ((uint64_t)v.lo[3] << 32);
    ctx->hi = (uint64_t)v.hi[0] | ((uint64_t)v.hi[1] << 32);
    ctx->hi1 = (uint64_t)v.hi[2] | ((uint64_t)v.hi[3] << 32);
}

static inline __m128i ps2_words_to_vec(const uint32_t w[4])
{
    return _mm_set_epi32((int)w[3], (int)w[2], (int)w[1], (int)w[0]);
}

static inline void ps2_vec_to_words(__m128i v, uint32_t w[4])
{
    _mm_storeu_si128(reinterpret_cast<__m128i *>(w), v);
}

static inline void ps2_vec_to_halves(__m128i v, int16_t h[8])
{
    _mm_storeu_si128(reinterpret_cast<__m128i *>(h), v);
}

// PMFHI / PMFLO / PMTHI / PMTLO: the whole 128-bit HI or LO.
static inline __m128i ps2_mmi_pmfhi(const R5900Context *ctx)
{
    return _mm_set_epi64x((long long)ctx->hi1, (long long)ctx->hi);
}

static inline __m128i ps2_mmi_pmflo(const R5900Context *ctx)
{
    return _mm_set_epi64x((long long)ctx->lo1, (long long)ctx->lo);
}

static inline void ps2_mmi_pmthi(R5900Context *ctx, __m128i rs)
{
    uint32_t w[4];
    ps2_vec_to_words(rs, w);
    ctx->hi = (uint64_t)w[0] | ((uint64_t)w[1] << 32);
    ctx->hi1 = (uint64_t)w[2] | ((uint64_t)w[3] << 32);
}

static inline void ps2_mmi_pmtlo(R5900Context *ctx, __m128i rs)
{
    uint32_t w[4];
    ps2_vec_to_words(rs, w);
    ctx->lo = (uint64_t)w[0] | ((uint64_t)w[1] << 32);
    ctx->lo1 = (uint64_t)w[2] | ((uint64_t)w[3] << 32);
}

// PMULTW/PMULTUW/PMADDW/PMADDUW/PMSUBW: words 0 and 2. Each 64-bit result r
// goes to rd doubleword k, LO doubleword k = sext(r[31:0]) and HI doubleword
// k = sext(r[63:32]). op: 0 = multiply, 1 = multiply-add, 2 = multiply-subtract
// (the accumulator is HI.w[2k]:LO.w[2k]).
static inline void ps2_mmi_word_mul(R5900Context *ctx, int rd, __m128i rsv, __m128i rtv, bool isUnsigned, int op)
{
    uint32_t rs[4], rt[4], out[4];
    ps2_vec_to_words(rsv, rs);
    ps2_vec_to_words(rtv, rt);
    Ps2HiLo v = ps2_hilo_load(ctx);
    for (int k = 0; k < 2; ++k)
    {
        const int w = k * 2;
        const uint64_t product = isUnsigned
                                     ? (uint64_t)rs[w] * (uint64_t)rt[w]
                                     : (uint64_t)((int64_t)(int32_t)rs[w] * (int64_t)(int32_t)rt[w]);
        const uint64_t acc = ((uint64_t)v.hi[w] << 32) | v.lo[w];
        const uint64_t r = op == 1 ? acc + product : op == 2 ? acc - product : product;
        const uint32_t lo = (uint32_t)r;
        const uint32_t hi = (uint32_t)(r >> 32);
        v.lo[w] = lo;
        v.lo[w + 1] = (uint32_t)((int32_t)lo >> 31);
        v.hi[w] = hi;
        v.hi[w + 1] = (uint32_t)((int32_t)hi >> 31);
        out[w] = lo;
        out[w + 1] = hi;
    }
    ps2_hilo_store(ctx, v);
    if (rd != 0)
        ctx->r[rd] = ps2_words_to_vec(out);
}

// PMULTH/PMADDH/PMSUBH: product i = rs.h[i] * rt.h[i]. LO words take products
// {0,1,4,5}, HI words {2,3,6,7}; rd = {LO.w0, HI.w0, LO.w2, HI.w2}.
// op: 0 = PMULTH, 1 = PMADDH, 2 = PMSUBH.
static inline void ps2_mmi_half_mul(R5900Context *ctx, int rd, __m128i rsv, __m128i rtv, int op)
{
    int16_t rs[8], rt[8];
    ps2_vec_to_halves(rsv, rs);
    ps2_vec_to_halves(rtv, rt);
    Ps2HiLo v = ps2_hilo_load(ctx);
    static const int word[8] = {0, 1, 0, 1, 2, 3, 2, 3};
    for (int i = 0; i < 8; ++i)
    {
        const uint32_t p = (uint32_t)((int32_t)rs[i] * (int32_t)rt[i]);
        uint32_t &slot = ((i >> 1) & 1) ? v.hi[word[i]] : v.lo[word[i]];
        slot = op == 1 ? slot + p : op == 2 ? slot - p : p;
    }
    ps2_hilo_store(ctx, v);
    if (rd != 0)
    {
        const uint32_t out[4] = {v.lo[0], v.hi[0], v.lo[2], v.hi[2]};
        ctx->r[rd] = ps2_words_to_vec(out);
    }
}

// PHMADH/PHMSBH: LO.w0 = p1 +/- p0, HI.w0 = p3 +/- p2, LO.w2 = p5 +/- p4,
// HI.w2 = p7 +/- p6; rd = {LO.w0, HI.w0, LO.w2, HI.w2}. Words 1 and 3 are
// undefined on hardware; they get PCSX2's values.
static inline void ps2_mmi_half_hmul(R5900Context *ctx, int rd, __m128i rsv, __m128i rtv, bool subtract)
{
    int16_t rs[8], rt[8];
    ps2_vec_to_halves(rsv, rs);
    ps2_vec_to_halves(rtv, rt);
    Ps2HiLo v;
    for (int pair = 0; pair < 4; ++pair)
    {
        const int n = pair * 2;
        const uint32_t first = (uint32_t)((int32_t)rs[n + 1] * (int32_t)rt[n + 1]);
        const uint32_t second = (uint32_t)((int32_t)rs[n] * (int32_t)rt[n]);
        uint32_t *dst = (pair & 1) ? v.hi : v.lo;
        const int w = (pair >> 1) * 2;
        dst[w] = subtract ? first - second : first + second;
        dst[w + 1] = subtract ? ~first : first;
    }
    ps2_hilo_store(ctx, v);
    if (rd != 0)
    {
        const uint32_t out[4] = {v.lo[0], v.hi[0], v.lo[2], v.hi[2]};
        ctx->r[rd] = ps2_words_to_vec(out);
    }
}

// PDIVW/PDIVUW: words 0 and 2. LO doubleword k = sext(quotient), HI
// doubleword k = sext(remainder).
static inline void ps2_mmi_pdivw(R5900Context *ctx, __m128i rsv, __m128i rtv, bool isUnsigned)
{
    uint32_t rs[4], rt[4];
    ps2_vec_to_words(rsv, rs);
    ps2_vec_to_words(rtv, rt);
    uint64_t q[2], r[2];
    for (int k = 0; k < 2; ++k)
    {
        const uint32_t a = rs[k * 2];
        const uint32_t b = rt[k * 2];
        uint32_t quotient, remainder;
        if (isUnsigned)
        {
            quotient = b ? a / b : 0xFFFFFFFFu;
            remainder = b ? a % b : a;
        }
        else
        {
            const int32_t sa = (int32_t)a;
            const int32_t sb = (int32_t)b;
            if (sb == 0)
            {
                quotient = sa < 0 ? 1u : 0xFFFFFFFFu;
                remainder = a;
            }
            else if (sa == INT32_MIN && sb == -1)
            {
                quotient = (uint32_t)INT32_MIN;
                remainder = 0;
            }
            else
            {
                quotient = (uint32_t)(sa / sb);
                remainder = (uint32_t)(sa % sb);
            }
        }
        q[k] = (uint64_t)(int64_t)(int32_t)quotient;
        r[k] = (uint64_t)(int64_t)(int32_t)remainder;
    }
    ctx->lo = q[0];
    ctx->lo1 = q[1];
    ctx->hi = r[0];
    ctx->hi1 = r[1];
}

// PDIVBW: each word of rs divided by halfword 0 of rt.
static inline void ps2_mmi_pdivbw(R5900Context *ctx, __m128i rsv, __m128i rtv)
{
    uint32_t rs[4];
    ps2_vec_to_words(rsv, rs);
    const int32_t divisor = (int16_t)_mm_cvtsi128_si32(rtv);
    Ps2HiLo v;
    for (int i = 0; i < 4; ++i)
    {
        const int32_t a = (int32_t)rs[i];
        if (divisor == 0)
        {
            v.lo[i] = a < 0 ? 1u : 0xFFFFFFFFu;
            v.hi[i] = (uint32_t)a;
        }
        else if (a == INT32_MIN && divisor == -1)
        {
            v.lo[i] = (uint32_t)INT32_MIN;
            v.hi[i] = 0;
        }
        else
        {
            v.lo[i] = (uint32_t)(a / divisor);
            v.hi[i] = (uint32_t)(int32_t)(int16_t)(a % divisor);
        }
    }
    ps2_hilo_store(ctx, v);
}

static inline int16_t ps2_sat16(uint32_t w)
{
    const int32_t s = (int32_t)w;
    return (int16_t)(s > 0x7FFF ? 0x7FFF : s < -0x8000 ? -0x8000 : s);
}

// PMFHL.fmt (sa field: 0 LW, 1 UW, 2 SLW, 3 LH, 4 SH).
static inline __m128i ps2_mmi_pmfhl(const R5900Context *ctx, int fmt)
{
    const Ps2HiLo v = ps2_hilo_load(ctx);
    switch (fmt)
    {
    case 0:
    {
        const uint32_t out[4] = {v.lo[0], v.hi[0], v.lo[2], v.hi[2]};
        return ps2_words_to_vec(out);
    }
    case 1:
    {
        const uint32_t out[4] = {v.lo[1], v.hi[1], v.lo[3], v.hi[3]};
        return ps2_words_to_vec(out);
    }
    case 2:
    {
        int64_t d[2];
        for (int k = 0; k < 2; ++k)
        {
            const int64_t value = (int64_t)(((uint64_t)v.hi[k * 2] << 32) | v.lo[k * 2]);
            d[k] = value > INT32_MAX ? INT32_MAX : value < INT32_MIN ? INT32_MIN : (int64_t)(int32_t)v.lo[k * 2];
        }
        return _mm_set_epi64x(d[1], d[0]);
    }
    case 3:
    {
        const uint32_t out[4] = {
            (v.lo[0] & 0xFFFFu) | (v.lo[1] << 16), (v.hi[0] & 0xFFFFu) | (v.hi[1] << 16),
            (v.lo[2] & 0xFFFFu) | (v.lo[3] << 16), (v.hi[2] & 0xFFFFu) | (v.hi[3] << 16)};
        return ps2_words_to_vec(out);
    }
    default:
    {
        const int16_t h[8] = {
            ps2_sat16(v.lo[0]), ps2_sat16(v.lo[1]), ps2_sat16(v.hi[0]), ps2_sat16(v.hi[1]),
            ps2_sat16(v.lo[2]), ps2_sat16(v.lo[3]), ps2_sat16(v.hi[2]), ps2_sat16(v.hi[3])};
        return _mm_loadu_si128(reinterpret_cast<const __m128i *>(h));
    }
    }
}

// PMTHL.LW: LO.w0/w2 from rs words 0/2, HI.w0/w2 from rs words 1/3.
static inline void ps2_mmi_pmthl_lw(R5900Context *ctx, __m128i rsv)
{
    uint32_t rs[4];
    ps2_vec_to_words(rsv, rs);
    Ps2HiLo v = ps2_hilo_load(ctx);
    v.lo[0] = rs[0];
    v.hi[0] = rs[1];
    v.lo[2] = rs[2];
    v.hi[2] = rs[3];
    ps2_hilo_store(ctx, v);
}

// FPU (COP1) operations
#define FPU_SET_ACC(ctx, res) (ctx->f_acc = res)
// PS2 semantics via ps2_float_clamp (defined with the VU helpers above).
#define FPU_ADD_S(a, b) ps2_float_clamp(ps2_float_clamp((float)(a)) + ps2_float_clamp((float)(b)))
#define FPU_SUB_S(a, b) ps2_float_clamp(ps2_float_clamp((float)(a)) - ps2_float_clamp((float)(b)))
#define FPU_MUL_S(a, b) ps2_float_clamp(ps2_float_clamp((float)(a)) * ps2_float_clamp((float)(b)))
#define FPU_DIV_S(a, b) ps2_float_clamp(ps2_float_clamp((float)(a)) / ps2_float_clamp((float)(b)))
// DIV.S and RSQRT.S as the EE FPU performs them (as PCSX2's interpreter models
// them): operands clamped; a zero or denormal divisor gives +/-Fmax with the xor
// of the operand signs and the D flag (I for 0/0); RSQRT.S fd = fs / sqrt(|ft|),
// with the I flag for a negative ft. FCR31: I 0x20000, D 0x10000, SI 0x40, SD 0x20.
inline float ps2_fpu_div(float fs, float ft, uint32_t &fcr31)
{
    const uint32_t s = std::bit_cast<uint32_t>(fs), t = std::bit_cast<uint32_t>(ft);
    fcr31 &= ~0x00030000u;
    if ((t & 0x7F800000u) == 0u)
    {
        fcr31 |= (s & 0x7F800000u) == 0u ? 0x00020040u : 0x00010020u;
        return std::bit_cast<float>(((s ^ t) & 0x80000000u) | 0x7F7FFFFFu);
    }
    return ps2_float_clamp(ps2_float_clamp(fs) / ps2_float_clamp(ft));
}
inline float ps2_fpu_rsqrt(float fs, float ft, uint32_t &fcr31)
{
    const uint32_t s = std::bit_cast<uint32_t>(fs), t = std::bit_cast<uint32_t>(ft);
    fcr31 &= ~0x00030000u;
    if ((t & 0x7F800000u) == 0u)
    {
        fcr31 |= 0x00010020u;
        return std::bit_cast<float>(((s ^ t) & 0x80000000u) | 0x7F7FFFFFu);
    }
    if (t & 0x80000000u)
        fcr31 |= 0x00020040u;
    return ps2_float_clamp(ps2_float_clamp(fs) / sqrtf(std::fabs(ps2_float_clamp(ft))));
}
// The EE's SQRT.S ignores the sign of its operand.
#define FPU_SQRT_S(a) ps2_float_clamp(sqrtf(std::fabs(ps2_float_clamp((float)(a)))))
#define FPU_ABS_S(a) fabsf((float)(a))
#define FPU_MOV_S(a) ((float)(a))
#define FPU_NEG_S(a) (-(float)(a))
#define FPU_ROUND_L_S(a) ((int64_t)roundf((float)(a)))
#define FPU_TRUNC_L_S(a) ((int64_t)(float)(a))
#define FPU_CEIL_L_S(a) ((int64_t)ceilf((float)(a)))
#define FPU_FLOOR_L_S(a) ((int64_t)floorf((float)(a)))
#define FPU_ROUND_W_S(a) ((int32_t)nearbyintf((float)(a)))
#define FPU_TRUNC_W_S(a) ((int32_t)(float)(a))
#define FPU_CEIL_W_S(a) ((int32_t)ceilf((float)(a)))
#define FPU_FLOOR_W_S(a) ((int32_t)floorf((float)(a)))
#define FPU_CVT_S_W(a) ((float)(int32_t)(a))
#define FPU_CVT_S_L(a) ((float)(int64_t)(a))
// EE CVT.W.S always rounds toward zero (the EE FPU has no other rounding mode)
// and saturates by sign when |a| >= 2^31; exponent 0xff is an ordinary large
// value on the EE, not Inf/NaN. Round-to-nearest here made GT3's random pick
// int(rand * n) return n (race car 0 left empty, engine sound 00000 hang).
inline int32_t ps2_fpu_cvt_w_s(float value)
{
    const uint32_t bits = std::bit_cast<uint32_t>(value);
    if (((bits >> 23) & 0xffu) >= 158u)
        return (bits & 0x80000000u) ? INT32_MIN : INT32_MAX;
    return static_cast<int32_t>(value);
}
#define FPU_CVT_W_S(a) ps2_fpu_cvt_w_s((float)(a))
#define FPU_CVT_L_S(a) ((int64_t)(float)(a))
// EE compares see operands as the EE does: denormals are zero and exponent 255
// is an ordinary huge value (a host denormal != 0 would take a guarded path,
// e.g. a divide, that the EE skips). There is no unordered case on the EE.
#define FPU_CMP_OPERAND(a) ps2_float_clamp((float)(a))
#define FPU_C_F_S(a, b) (0)
#define FPU_C_UN_S(a, b) (0)
#define FPU_C_EQ_S(a, b) (FPU_CMP_OPERAND(a) == FPU_CMP_OPERAND(b))
#define FPU_C_UEQ_S(a, b) (FPU_CMP_OPERAND(a) == FPU_CMP_OPERAND(b))
#define FPU_C_OLT_S(a, b) (FPU_CMP_OPERAND(a) < FPU_CMP_OPERAND(b))
#define FPU_C_ULT_S(a, b) (FPU_CMP_OPERAND(a) < FPU_CMP_OPERAND(b))
#define FPU_C_OLE_S(a, b) (FPU_CMP_OPERAND(a) <= FPU_CMP_OPERAND(b))
#define FPU_C_ULE_S(a, b) (FPU_CMP_OPERAND(a) <= FPU_CMP_OPERAND(b))
#define FPU_C_SF_S(a, b) (0)
#define FPU_C_NGLE_S(a, b) (0)
#define FPU_C_SEQ_S(a, b) (FPU_CMP_OPERAND(a) == FPU_CMP_OPERAND(b))
#define FPU_C_NGL_S(a, b) (FPU_CMP_OPERAND(a) == FPU_CMP_OPERAND(b))
#define FPU_C_LT_S(a, b) (FPU_CMP_OPERAND(a) < FPU_CMP_OPERAND(b))
#define FPU_C_NGE_S(a, b) (FPU_CMP_OPERAND(a) < FPU_CMP_OPERAND(b))
#define FPU_C_LE_S(a, b) (FPU_CMP_OPERAND(a) <= FPU_CMP_OPERAND(b))
#define FPU_C_NGT_S(a, b) (FPU_CMP_OPERAND(a) <= FPU_CMP_OPERAND(b))

// QFSRV: Quadword Funnel Shift Right Variable
// Concatenates rs || rt (256 bits) and right-shifts by SA bits, taking lower 128 bits.
inline __m128i ps2_qfsrv(__m128i rs, __m128i rt, uint32_t sa)
{
    if (sa == 0)
        return rt;
    if (sa >= 128)
    {
        if (sa >= 256)
            return _mm_setzero_si128();
        uint32_t shift = sa - 128;
        if (shift == 0)
            return rs;
        // Shift rs right by (sa-128) bits
        uint32_t byteShift = shift / 8;
        uint32_t bitShift = shift % 8;
        // Byte shift rs right
        alignas(16) uint8_t buf[16] = {};
        alignas(16) uint8_t src[16];
        _mm_store_si128((__m128i *)src, rs);
        for (uint32_t i = 0; i + byteShift < 16; i++)
            buf[i] = src[i + byteShift];
        __m128i result = _mm_load_si128((__m128i *)buf);
        if (bitShift > 0)
            result = _mm_or_si128(_mm_srli_epi64(result, bitShift),
                                  _mm_slli_epi64(_mm_bsrli_si128(result, 8), 64 - bitShift));
        return result;
    }
    // sa is 1..127: result = (rs || rt) >> sa, lower 128 bits
    uint32_t byteShift = sa / 8;
    uint32_t bitShift = sa % 8;
    alignas(16) uint8_t combined[32];
    _mm_store_si128((__m128i *)(combined), rt);      // low 128 bits
    _mm_store_si128((__m128i *)(combined + 16), rs); // high 128 bits
    // Shift right by byteShift bytes
    alignas(16) uint8_t shifted[16];
    for (uint32_t i = 0; i < 16; i++)
        shifted[i] = (i + byteShift < 32) ? combined[i + byteShift] : 0;
    __m128i result = _mm_load_si128((__m128i *)shifted);
    if (bitShift > 0)
    {
        uint8_t extra = (byteShift + 16 < 32) ? combined[byteShift + 16] : 0;
        __m128i hi_byte = _mm_insert_epi8(_mm_setzero_si128(), extra, 15);
        alignas(16) uint8_t src32[32];
        for (uint32_t i = 0; i < 32; i++)
            src32[i] = combined[i];
        uint64_t lo0, lo1, hi0, hi1;
        std::memcpy(&lo0, src32, 8);
        std::memcpy(&lo1, src32 + 8, 8);
        std::memcpy(&hi0, src32 + 16, 8);
        std::memcpy(&hi1, src32 + 24, 8);
        // 256-bit right shift by sa bits
        uint64_t r0, r1;
        if (sa < 64)
        {
            r0 = (lo0 >> sa) | (lo1 << (64 - sa));
            r1 = (lo1 >> sa) | (hi0 << (64 - sa));
        }
        else if (sa < 128)
        {
            uint32_t s = sa - 64;
            if (s == 0)
            {
                r0 = lo1;
                r1 = hi0;
            }
            else
            {
                r0 = (lo1 >> s) | (hi0 << (64 - s));
                r1 = (hi0 >> s) | (hi1 << (64 - s));
            }
        }
        else
        {
            r0 = 0;
            r1 = 0; // handled above
        }
        result = _mm_set_epi64x((long long)r1, (long long)r0);
    }
    return result;
}
#define PS2_QFSRV(rs, rt, sa) ps2_qfsrv((__m128i)(rs), (__m128i)(rt), (uint32_t)(sa))
#define PS2_PCPYLD(rs, rt) _mm_unpacklo_epi64(rt, rs)
#define PS2_PEXEH(rs) _mm_shufflelo_epi16(_mm_shufflehi_epi16(rs, _MM_SHUFFLE(2, 3, 0, 1)), _MM_SHUFFLE(2, 3, 0, 1))
#define PS2_PEXEW(rs) _mm_shuffle_epi32(rs, _MM_SHUFFLE(2, 3, 0, 1))
#define PS2_PROT3W(rs) _mm_shuffle_epi32(rs, _MM_SHUFFLE(0, 3, 2, 1))

// Additional VU0 operations
#define PS2_VSQRT(x) sqrtf(x)
#define PS2_VRSQRT(x) (1.0f / sqrtf(x))

// VU0 macro DIV/SQRT/RSQRT (Q) with the VU's rules (as PCSX2's interpreter):
// operands are clamped (exponent 255 -> Fmax, denormal -> 0); SQRT/RSQRT take
// |ft|; DIV by zero gives +/-Fmax (sign fs^ft), RSQRT by zero gives +/-Fmax, or
// a signed zero when fs is zero too.
inline float ps2_vu_div(float fs, float ft)
{
    fs = ps2_float_clamp(fs);
    ft = ps2_float_clamp(ft);
    if (ft == 0.0f)
    {
        const uint32_t sign = (std::bit_cast<uint32_t>(fs) ^ std::bit_cast<uint32_t>(ft)) & 0x80000000u;
        return std::bit_cast<float>(sign | 0x7F7FFFFFu);
    }
    return ps2_float_clamp(fs / ft);
}
inline float ps2_vu_sqrt(float ft)
{
    return ps2_float_clamp(std::sqrt(std::fabs(ps2_float_clamp(ft))));
}
inline float ps2_vu_rsqrt(float fs, float ft)
{
    fs = ps2_float_clamp(fs);
    ft = ps2_float_clamp(ft);
    const uint32_t sign = (std::bit_cast<uint32_t>(fs) ^ std::bit_cast<uint32_t>(ft)) & 0x80000000u;
    if (ft == 0.0f)
        return std::bit_cast<float>(fs == 0.0f ? sign : (sign | 0x7F7FFFFFu));
    return ps2_float_clamp(fs / std::sqrt(std::fabs(ft)));
}
// VFTOI: truncate, saturating to 0x7FFFFFFF / 0x80000000 (cvttps gives
// 0x80000000 for every out-of-range or exponent-255 lane).
inline __m128i ps2_vu_ftoi(__m128 scaled)
{
    const __m128i result = _mm_cvttps_epi32(scaled);
    const __m128i indefinite = _mm_cmpeq_epi32(result, _mm_set1_epi32(static_cast<int>(0x80000000u)));
    const __m128i positive = _mm_cmpgt_epi32(_mm_castps_si128(scaled), _mm_set1_epi32(-1)); // sign bit clear
    return _mm_add_epi32(result, _mm_and_si128(_mm_and_si128(indefinite, positive), _mm_set1_epi32(-1))); // 0x80000000 - 1
}
// VMAX/VMINI compare the raw words as sign-magnitude integers (no clamping).
inline __m128 ps2_vu_max(__m128 a, __m128 b)
{
    const __m128i ai = _mm_castps_si128(a), bi = _mm_castps_si128(b);
    const __m128i bothNegative = _mm_and_si128(_mm_srai_epi32(ai, 31), _mm_srai_epi32(bi, 31));
    const __m128i pickA = _mm_xor_si128(_mm_cmpgt_epi32(ai, bi), bothNegative);
    return _mm_castsi128_ps(_mm_or_si128(_mm_and_si128(pickA, ai), _mm_andnot_si128(pickA, bi)));
}
inline __m128 ps2_vu_min(__m128 a, __m128 b)
{
    const __m128i ai = _mm_castps_si128(a), bi = _mm_castps_si128(b);
    const __m128i bothNegative = _mm_and_si128(_mm_srai_epi32(ai, 31), _mm_srai_epi32(bi, 31));
    const __m128i pickA = _mm_xor_si128(_mm_cmpgt_epi32(bi, ai), bothNegative);
    return _mm_castsi128_ps(_mm_or_si128(_mm_and_si128(pickA, ai), _mm_andnot_si128(pickA, bi)));
}

#define GPR_U32(ctx_ptr, reg_idx) ((reg_idx == 0) ? 0U : static_cast<uint32_t>(PS2_EXTRACT_EPI32_0(ctx_ptr->r[reg_idx])))
#define GPR_S32(ctx_ptr, reg_idx) ((reg_idx == 0) ? 0 : PS2_EXTRACT_EPI32_0(ctx_ptr->r[reg_idx]))
#define GPR_U64(ctx_ptr, reg_idx) ((reg_idx == 0) ? 0ULL : static_cast<uint64_t>(PS2_EXTRACT_EPI64_0(ctx_ptr->r[reg_idx])))
#define GPR_S64(ctx_ptr, reg_idx) ((reg_idx == 0) ? 0LL : PS2_EXTRACT_EPI64_0(ctx_ptr->r[reg_idx]))
#define GPR_VEC(ctx_ptr, reg_idx) ((reg_idx == 0) ? _mm_setzero_si128() : ctx_ptr->r[reg_idx])

static inline void Ps2SetGprLow64(R5900Context *ctx, int reg, __m128i new_low)
{
    if (reg != 0)
    {
        ctx->r[reg] = _mm_castpd_si128(_mm_move_sd(_mm_castsi128_pd(ctx->r[reg]), _mm_castsi128_pd(new_low)));
    }
}

#define SET_GPR_U32(ctx_ptr, reg_idx, val)                                \
    do                                                                    \
    {                                                                     \
        if ((reg_idx) != 0)                                               \
        {                                                                 \
            __m128i _newVal = _mm_cvtsi64_si128((int64_t)(int32_t)(val)); \
                                                                          \
            Ps2SetGprLow64(ctx_ptr, reg_idx, _newVal);                    \
        }                                                                 \
    } while (0)

#define SET_GPR_S32(ctx_ptr, reg_idx, val)                                \
    do                                                                    \
    {                                                                     \
        if ((reg_idx) != 0)                                               \
        {                                                                 \
            __m128i _newVal = _mm_cvtsi64_si128((int64_t)(int32_t)(val)); \
            Ps2SetGprLow64(ctx_ptr, reg_idx, _newVal);                    \
        }                                                                 \
    } while (0)

#define SET_GPR_U64(ctx_ptr, reg_idx, val)                       \
    do                                                           \
    {                                                            \
        if ((reg_idx) != 0)                                      \
        {                                                        \
            __m128i _newVal = _mm_cvtsi64_si128((int64_t)(val)); \
            Ps2SetGprLow64(ctx_ptr, reg_idx, _newVal);           \
        }                                                        \
    } while (0)

#define SET_GPR_S64(ctx_ptr, reg_idx, val) SET_GPR_U64(ctx_ptr, reg_idx, val)

#define SET_GPR_VEC(ctx_ptr, reg_idx, val) \
    do                                     \
    {                                      \
        if (reg_idx != 0)                  \
            ctx_ptr->r[reg_idx] = (val);   \
    } while (0)

#endif // PS2_RUNTIME_MACROS_H
