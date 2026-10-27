#pragma once

// GS mipmapping (analysis/phase5/specs/mipmap-design.md, sections 3-4): the LOD
// rules, defined once in integer fixed point and shared by the frontend (each
// textured primitive is resolved to one level when its whole LOD range allows),
// the CPU reference renderer (per-pixel sampling) and the GL renderer's host
// code. The GL shader repeats log2Fixed16 and pick in GLSL with the same
// integer operations (gs_gl_shaders.h, GS_MIPMAP), so CPU and GL agree exactly.
//
// Switches (read once):
//   PS2X_GS_MIPMAP=0/1      mipmapping (GT4 default 1, GT3 default 0: GT3 draws as before)
//   PS2X_GS_MIP_COLLAPSE=0  STQ triangles are never resolved to one level: every
//                           mip-capable (or MAG != MIN) one is drawn per pixel (diagnostic)
//   PS2X_GS_MIP_STATS=<s>   [gs-mip] counts every s seconds (and the CPU renderer's
//                           pixels per level, with a check of each resolved primitive)
//   PS2X_GS_MIP_TINT=1      texels tinted by level (L1 red, L2 green, L3 blue, L4
//                           yellow, L5 magenta, L6 cyan): CPU renderer, and the GL
//                           renderer at native (its fast-mode programs do not tint);
//                           =2 the CPU renderer only (GT3_GS_COMPARE then reports
//                           the tinted pixels each frame shows as its differences)
//   PS2X_GS_MIP_RANGE_CLAMP=0  a per-pixel primitive's level is not confined to the
//                           range resolve() gave it (confine; diagnostic, as first built)

#include "runtime/gs/gs_mip_lut.h"
#include "runtime/gs/gs_types.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>

namespace gsmip
{
    inline bool envSwitch(const char *name, bool fallback)
    {
        const char *v = std::getenv(name);
        if (!v || !*v)
            return fallback;
        return *v != '0';
    }

    inline bool enabled()
    {
#if defined(PS2X_GAME_GT3)
        static const bool on = envSwitch("PS2X_GS_MIPMAP", false);
#else
        static const bool on = envSwitch("PS2X_GS_MIPMAP", true);
#endif
        return on;
    }

    inline bool collapseEnabled()
    {
        static const bool on = envSwitch("PS2X_GS_MIP_COLLAPSE", true);
        return on;
    }

    // A per-pixel primitive's pick kept inside its mipLo..mipHi (confine).
    inline bool rangeClampEnabled()
    {
        static const bool on = envSwitch("PS2X_GS_MIP_RANGE_CLAMP", true);
        return on;
    }

    // Seconds between [gs-mip] reports; 0: no statistics.
    inline double statsSeconds()
    {
        static const double s = [] {
            const char *v = std::getenv("PS2X_GS_MIP_STATS");
            const double d = (v && *v) ? std::atof(v) : 0.0;
            return d > 0.0 ? d : 0.0;
        }();
        return s;
    }
    inline bool statsOn() { return enabled() && statsSeconds() > 0.0; }

    // PS2X_GS_MIP_TINT: 0 off, 1 both renderers tint, 2 the CPU renderer only
    // (with GT3_GS_COMPARE a frame's differing pixels are then the tinted pixels
    // it shows, and the compare dumps show where).
    inline int tintMode()
    {
        static const int m = [] {
            const char *v = std::getenv("PS2X_GS_MIP_TINT");
            const int n = (v && *v) ? std::atoi(v) : 0;
            return enabled() ? std::clamp(n, 0, 2) : 0;
        }();
        return m;
    }
    inline bool tintEnabled() { return tintMode() != 0; } // the CPU renderer
    inline bool tintGl() { return tintMode() == 1; }

    // Trilinear blend weight precision: out = (c0 * (256 - w) + c1 * w + 128) >> 8.
    constexpr int kBlendBits = 8;

    // GSDrawState::mipTag: 0 not resolved (as before), 1 + n resolved to level n
    // (TEX0 / CLAMP / size / filter of that level), kTagPerPixel: per pixel.
    constexpr uint8_t kTagPerPixel = 0x80u;

    struct Tex1
    {
        uint8_t lcm = 0, mxl = 0, mmag = 0, mmin = 0, mtba = 0, l = 0;
        uint16_t kRaw = 0; // K as written: signed 12 bits, 4 fraction bits
        int32_t k16 = 0;   // K in 16.16
    };

    inline Tex1 decodeTex1(uint64_t tex1)
    {
        Tex1 t;
        t.lcm = static_cast<uint8_t>(tex1 & 1u);
        t.mxl = static_cast<uint8_t>(std::min<uint32_t>(static_cast<uint32_t>((tex1 >> 2) & 7u), 6u)); // MXL 7 as 6
        t.mmag = static_cast<uint8_t>((tex1 >> 5) & 1u);
        t.mmin = static_cast<uint8_t>((tex1 >> 6) & 7u);
        t.mtba = static_cast<uint8_t>((tex1 >> 9) & 1u);
        t.l = static_cast<uint8_t>((tex1 >> 19) & 3u);
        t.kRaw = static_cast<uint16_t>((tex1 >> 32) & 0xFFFu);
        const int32_t k = t.kRaw >= 0x800u ? static_cast<int32_t>(t.kRaw) - 0x1000 : static_cast<int32_t>(t.kRaw);
        t.k16 = k * 4096;
        return t;
    }

    inline bool minLinear(uint8_t mmin) { return mmin == 1u || (mmin & 4u) != 0u; }
    inline bool mipCapable(const Tex1 &t) { return t.mxl > 0u && t.mmin >= 2u && t.mmin <= 5u; }
    inline bool trilinear(const Tex1 &t) { return t.mmin == 3u || t.mmin == 5u; }

    // Level n's memory and geometry: n = 0 TEX0 / CLAMP as they are; n >= 1 base
    // and width from MIPTBP1 (1-3) / MIPTBP2 (4-6), size halved per level but
    // never below 2 texels, CLAMP region fields shifted right by n.
    struct Level
    {
        uint32_t tbp = 0, tbw = 0;
        uint8_t tw = 0, th = 0;
        uint16_t w = 1, h = 1;
        uint16_t minU = 0, maxU = 0, minV = 0, maxV = 0;
    };

    inline Level level(const GSContext &ctx, int n)
    {
        Level L;
        const int tw0 = std::min<int>(ctx.tex0.tw, 10), th0 = std::min<int>(ctx.tex0.th, 10);
        const uint64_t clamp = ctx.clamp;
        uint32_t minU = static_cast<uint32_t>((clamp >> 4) & 0x3FFu), maxU = static_cast<uint32_t>((clamp >> 14) & 0x3FFu);
        uint32_t minV = static_cast<uint32_t>((clamp >> 24) & 0x3FFu), maxV = static_cast<uint32_t>((clamp >> 34) & 0x3FFu);
        if (n <= 0)
        {
            L.tbp = ctx.tex0.tbp0;
            L.tbw = ctx.tex0.tbw;
            L.tw = static_cast<uint8_t>(tw0);
            L.th = static_cast<uint8_t>(th0);
        }
        else
        {
            const uint64_t words = n <= 3 ? ctx.miptbp1 : ctx.miptbp2;
            const uint64_t f = words >> (20 * ((n - 1) % 3));
            L.tbp = static_cast<uint32_t>(f & 0x3FFFu);
            L.tbw = static_cast<uint32_t>((f >> 14) & 0x3Fu);
            L.tw = static_cast<uint8_t>(tw0 > n ? tw0 - n : 1);
            L.th = static_cast<uint8_t>(th0 > n ? th0 - n : 1);
            minU >>= n;
            maxU >>= n;
            minV >>= n;
            maxV >>= n;
        }
        L.w = static_cast<uint16_t>(1u << L.tw);
        L.h = static_cast<uint16_t>(1u << L.th);
        L.minU = static_cast<uint16_t>(minU);
        L.maxU = static_cast<uint16_t>(maxU);
        L.minV = static_cast<uint16_t>(minV);
        L.maxV = static_cast<uint16_t>(maxV);
        return L;
    }

    // The samplers' Q (|q| <= 1e-8 -> 1.0), the same function as the CPU
    // renderer's and the shader's fabsQ.
    inline float fabsQ(float q) { return (std::fabs(q) > 1.0e-8f) ? q : 1.0f; }

    // log2(aq) in 16.16 from the float's bits: exponent, then the table at the
    // top 10 mantissa bits, linear over the low 13. aq: |fabsQ(q)| (finite and
    // normal, or +inf = 128.0).
    inline int32_t log2Fixed16(float aq)
    {
        uint32_t b;
        std::memcpy(&b, &aq, sizeof(b));
        b &= 0x7FFFFFFFu;
        const int32_t e = static_cast<int32_t>(b >> 23) - 127;
        const uint32_t m = b & 0x7FFFFFu, i = m >> 13, f = m & 0x1FFFu;
        const int32_t lo = static_cast<int32_t>(kLog2Lut[i]), hi = static_cast<int32_t>(kLog2Lut[i + 1u]);
        return e * 65536 + lo + static_cast<int32_t>((static_cast<uint32_t>(hi - lo) * f) >> 13);
    }

    // LOD in 16.16 at a pixel whose interpolated Q is q (as passed to the
    // sampler): LCM 0: (log2(1/|Q|) << L) + K; LCM 1: K. Multiplication, not
    // shifts of negative numbers, so C++ and GLSL agree.
    inline int32_t lod16(float q, const Tex1 &t)
    {
        if (t.lcm)
            return t.k16;
        return t.k16 - log2Fixed16(std::fabs(fabsQ(q))) * (1 << t.l);
    }

    struct Pick
    {
        uint8_t level = 0;
        uint8_t blend = 0; // weight of level + 1, 0..255 (trilinear)
        bool linear = false;
        bool operator==(const Pick &o) const { return level == o.level && blend == o.blend && linear == o.linear; }
        bool operator!=(const Pick &o) const { return !(*this == o); }
    };

    // The level(s) and filter a LOD selects (design table 3.3): LOD <= 0 level 0
    // with MMAG; above 0 MMIN's filter, and with MXL > 0 and MMIN 2-5 the level:
    // *_MIPMAP_NEAREST rounds half up, *_MIPMAP_LINEAR blends n and n + 1.
    inline Pick pick(int32_t lod, const Tex1 &t)
    {
        Pick p;
        if (lod <= 0)
        {
            p.linear = t.mmag != 0u;
            return p;
        }
        p.linear = minLinear(t.mmin);
        if (!mipCapable(t))
            return p;
        const int32_t c = std::min<int32_t>(lod, static_cast<int32_t>(t.mxl) << 16);
        if (!trilinear(t))
        {
            p.level = static_cast<uint8_t>(std::min<int32_t>((c + 0x8000) >> 16, t.mxl));
            return p;
        }
        p.level = static_cast<uint8_t>(c >> 16);
        p.blend = p.level >= t.mxl ? 0u : static_cast<uint8_t>((c & 0xFFFF) >> (16 - kBlendBits));
        return p;
    }

    // A per-pixel primitive's pick kept inside the levels resolve() gave it
    // (mipLo..mipHi), the only levels whose pages the renderers check and
    // upload for it: below lo, level lo alone (LOD > 0 there, so MMIN's
    // filter); at or above hi, level hi alone. At native scale a pixel's Q
    // stays within resolve()'s 2^-20 margin and this changes nothing (the
    // PS2X_GS_MIP_STATS check counts picks outside before it). At an internal
    // resolution the GL renderer's float edge functions (and fast mode's
    // rasterizer) can carry a pixel's Q past the vertices' range by more than
    // that margin. The shader's sampleMip does the same (u_mipCtl bit 15).
    // PS2X_GS_MIP_RANGE_CLAMP=0: not confined.
    inline Pick confine(Pick p, const Tex1 &t, uint8_t lo, uint8_t hi)
    {
        if (p.level < lo)
        {
            p.level = lo;
            p.blend = 0u;
            p.linear = minLinear(t.mmin);
        }
        else if (p.level >= hi)
        {
            p.level = hi;
            p.blend = 0u;
        }
        return p;
    }

    // Trilinear blend of two RGBA8 texels per channel.
    inline uint32_t blendLevels(uint32_t c0, uint32_t c1, uint32_t w)
    {
        uint32_t out = 0u;
        for (int s = 0; s < 32; s += 8)
        {
            const uint32_t a = (c0 >> s) & 0xFFu, b = (c1 >> s) & 0xFFu;
            out |= (((a * ((1u << kBlendBits) - w) + b * w + (1u << (kBlendBits - 1))) >> kBlendBits) & 0xFFu) << s;
        }
        return out;
    }

    // PS2X_GS_MIP_TINT: RGB multipliers per level (256 = 1).
    inline uint32_t tint(uint32_t texel, int lvl)
    {
        static constexpr uint32_t kTint[7][3] = {{256, 256, 256}, {256, 96, 96}, {96, 256, 96}, {96, 96, 256},
                                                 {256, 256, 96}, {256, 96, 256}, {96, 256, 256}};
        const uint32_t *m = kTint[std::clamp(lvl, 0, 6)];
        const uint32_t r = ((texel & 0xFFu) * m[0]) >> 8, g = (((texel >> 8) & 0xFFu) * m[1]) >> 8,
                       b = (((texel >> 16) & 0xFFu) * m[2]) >> 8;
        return (texel & 0xFF000000u) | r | (g << 8) | (b << 16);
    }

    // MTBA: levels 1-3 packed after level 0 (TBP/TBW of MIPTBP1 written), applied
    // when TEX0 / TEX2 of a context is written with that context's TEX1.MTBA set.
    // Non-square textures count as square of the larger side.
    inline void applyMtba(GSContext &ctx)
    {
        uint32_t bpp;
        switch (ctx.tex0.psm)
        {
        case GS_PSM_CT16:
        case GS_PSM_CT16S:
        case GS_PSM_Z16:
        case GS_PSM_Z16S:
            bpp = 16u;
            break;
        case GS_PSM_T8:
            bpp = 8u;
            break;
        case GS_PSM_T4:
            bpp = 4u;
            break;
        default: // 32-bit words (CT32/24, Z32/24, and the T8H/T4HL/T4HH alpha-plane views)
            bpp = 32u;
            break;
        }
        uint32_t bp = ctx.tex0.tbp0, bw = ctx.tex0.tbw;
        uint32_t w = 1u << std::min<uint32_t>(ctx.tex0.tw, 10u), h = 1u << std::min<uint32_t>(ctx.tex0.th, 10u);
        if (h < w)
            h = w;
        uint64_t words = ctx.miptbp1 & ~0x0FFFFFFFFFFFFFFFull; // bits 60-63 kept
        for (int n = 0; n < 3; ++n)
        {
            bp = (bp + (w * h * bpp / 8u) / 256u) & 0x3FFFu;
            bw = std::max<uint32_t>(bw >> 1, 1u);
            w = std::max<uint32_t>(w >> 1, 1u);
            h = std::max<uint32_t>(h >> 1, 1u);
            words |= (static_cast<uint64_t>(bp) | (static_cast<uint64_t>(bw) << 14)) << (20 * n);
        }
        ctx.miptbp1 = words;
    }

    // The GL shader's control word for a per-pixel primitive (u_mipCtl): bits
    // 0-2 MXL, 3-5 MMIN, 6 MMAG, 7 LCM, 8-9 L, 10 set (never 0), 16-27 K raw.
    // (The GL renderer adds bit 11 for PS2X_GS_MIP_TINT; a primitive resolved
    // to level n >= 1 then gets bit 11 and n in bits 12-14 alone.)
    inline int32_t packGlCtl(const Tex1 &t)
    {
        return static_cast<int32_t>(static_cast<uint32_t>(t.mxl) | (static_cast<uint32_t>(t.mmin) << 3) |
                                    (static_cast<uint32_t>(t.mmag) << 6) | (static_cast<uint32_t>(t.lcm) << 7) |
                                    (static_cast<uint32_t>(t.l) << 8) | (1u << 10) | (static_cast<uint32_t>(t.kRaw) << 16));
    }
    // A per-pixel primitive's level range for the shader (confine), added to
    // packGlCtl's word: bit 15 set, lo in bits 12-14 (a resolved primitive's
    // tint level uses them only without bit 10), hi in bits 28-30.
    inline int32_t packGlRange(uint8_t lo, uint8_t hi)
    {
        return static_cast<int32_t>(0x8000u | (static_cast<uint32_t>(lo & 7u) << 12) | (static_cast<uint32_t>(hi & 7u) << 28));
    }
    // Level n's (1-5) base and width for the shader: TBP | TBW << 14.
    inline int32_t glLevelWord(const GSContext &ctx, int n)
    {
        const Level L = level(ctx, n);
        return static_cast<int32_t>(L.tbp | (L.tbw << 14));
    }

    // ---- per-primitive resolve (frontend, before Submit)

    enum class Resolved : uint8_t
    {
        None,     // not textured, or nothing to resolve: drawn as before
        Single,   // one level and filter for the whole primitive (batch rewritten)
        PerPixel, // LOD crosses a decision boundary: level per pixel (mipTag kTagPerPixel)
        Approx,   // constant LOD between two levels on a sprite / FST triangle: nearer level (no blend)
    };

    // The primitive's LOD interval decides: if one (level, filter) holds over it,
    // the batch is rewritten to that level (TEX0 base/width/size, CLAMP region,
    // texture size, filter) and both renderers draw it with their usual code. pick
    // is monotone in LOD and the filter changes only at LOD 0, so equal picks at
    // both ends hold in between. A pixel's Q is a convex combination of the
    // vertex Qs (float weights >= 0 summing to 1 within a few ulp), so it stays
    // within ~2^-22 relative of [min, max]; the 2^-20 margin covers that.
    inline Resolved resolve(GSPrimitiveBatch &batch, bool collapse)
    {
        GSDrawState &st = batch.state;
        if (!st.prim.tme || batch.vertexCount == 0u)
            return Resolved::None;
        const GSPrimType type = st.prim.type;
        const bool triangle = type == GS_PRIM_TRIANGLE || type == GS_PRIM_TRISTRIP || type == GS_PRIM_TRIFAN;
        const bool sprite = type == GS_PRIM_SPRITE;
        if (!triangle && !sprite)
            return Resolved::None; // points and lines are drawn untextured
        const Tex1 t = decodeTex1(st.context.tex1);
        const bool magMin = (t.mmag != 0u) != minLinear(t.mmin);
        if (!mipCapable(t) && !magMin)
            return Resolved::None; // linearFilter (MMAG || MMIN linear) is already right
        const uint8_t top = mipCapable(t) ? t.mxl : 0u;

        auto single = [&](const Pick &p) {
            st.linearFilter = p.linear;
            st.mipTag = static_cast<uint8_t>(1u + p.level);
            if (p.level > 0u)
            {
                const Level L = level(st.context, p.level);
                GSTex0Reg &tex = st.context.tex0;
                tex.tbp0 = L.tbp;
                tex.tbw = static_cast<uint8_t>(L.tbw);
                tex.tw = L.tw;
                tex.th = L.th;
                uint64_t clamp = st.context.clamp & ~(0xFFFFFFFFFFull << 4);
                clamp |= (static_cast<uint64_t>(L.minU) << 4) | (static_cast<uint64_t>(L.maxU) << 14) |
                         (static_cast<uint64_t>(L.minV) << 24) | (static_cast<uint64_t>(L.maxV) << 34);
                st.context.clamp = clamp;
                st.textureWidth = L.w;
                st.textureHeight = L.h;
                // FST: UV are level 0 texels (12.4); level n reads UV / 2^n
                // (truncated: exact for nearest; no GT4 FST draw has MXL > 0).
                if (st.prim.fst)
                    for (uint32_t i = 0; i < batch.vertexCount; ++i)
                    {
                        batch.vertices[i].u = static_cast<uint16_t>(batch.vertices[i].u >> p.level);
                        batch.vertices[i].v = static_cast<uint16_t>(batch.vertices[i].v >> p.level);
                    }
            }
        };
        auto perPixel = [&](uint8_t lo, uint8_t hi) {
            st.mipTag = kTagPerPixel;
            st.mipLo = lo;
            st.mipHi = std::max(lo, hi);
            st.linearFilter = t.mmag != 0u || minLinear(t.mmin); // footprints: the bilinear margin
        };

        // Constant LOD: LCM, FST (the last vertex's Q), sprites (no per-pixel Q).
        const bool constant = t.lcm != 0u || st.prim.fst || sprite || batch.vertexCount < 3u;
        if (constant)
        {
            const int32_t lod = t.lcm ? t.k16 : lod16(batch.vertices[batch.vertexCount - 1u].q, t);
            Pick p = pick(lod, t);
            if (p.blend == 0u)
            {
                single(p);
                return Resolved::Single;
            }
            if (triangle && !st.prim.fst && batch.vertexCount >= 3u)
            {
                perPixel(p.level, static_cast<uint8_t>(p.level + 1u));
                return Resolved::PerPixel;
            }
            // A sprite / FST triangle between two levels: the nearer level
            // (the renderers have no per-pixel path for them; none seen in GT4).
            if (p.blend >= 128u && p.level < top)
                ++p.level;
            p.blend = 0u;
            single(p);
            return Resolved::Approx;
        }

        float qMin = 0.0f, qMax = 0.0f;
        bool positive = false, negative = false, unusable = false;
        for (uint32_t i = 0; i < 3u; ++i)
        {
            const float q = batch.vertices[i].q;
            if (!std::isfinite(q) || std::fabs(q) <= 2.0e-8f)
            {
                unusable = true;
                break;
            }
            positive = positive || q > 0.0f;
            negative = negative || q < 0.0f;
            const float a = std::fabs(q);
            qMin = i == 0u ? a : std::min(qMin, a);
            qMax = i == 0u ? a : std::max(qMax, a);
        }
        if (unusable || (positive && negative))
        {
            // |Q| can come near 0 (or is replaced by 1): any level.
            perPixel(0u, top);
            return Resolved::PerPixel;
        }
        const int32_t lodHi = lod16(qMin * (1.0f - 1.0f / 1048576.0f), t); // LOD falls as |Q| rises
        const int32_t lodLo = lod16(qMax * (1.0f + 1.0f / 1048576.0f), t);
        const Pick pA = pick(lodLo, t), pB = pick(lodHi, t);
        if (collapse && pA == pB && pA.blend == 0u)
        {
            single(pA);
            return Resolved::Single;
        }
        perPixel(pA.level, static_cast<uint8_t>(std::min<int>(pB.level + (trilinear(t) && mipCapable(t) ? 1 : 0), top)));
        return Resolved::PerPixel;
    }

    // ---- statistics (PS2X_GS_MIP_STATS)
    struct Stats
    {
        // frontend
        std::atomic<uint64_t> prims{0}, lcm{0}, single[7]{}, perPixel{0}, approx{0}, range[7][7]{};
        // CPU renderer: pixels per level (per-pixel primitives: the level read,
        // level + 1 counted in blended), resolved primitives' pixels per level,
        // and the checks (a pixel outside its primitive's level range, a
        // resolved primitive's pixel whose own pick differs).
        std::atomic<uint64_t> pixPerPixel[7]{}, pixBlended{0}, pixSingle[7]{}, outsideRange{0}, singleMismatch{0};
    };
    inline Stats &stats()
    {
        static Stats s;
        return s;
    }
}
