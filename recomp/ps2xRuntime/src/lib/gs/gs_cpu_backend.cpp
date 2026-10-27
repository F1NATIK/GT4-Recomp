#include "runtime/gs/gs_cpu_backend.h"
#include "runtime/gs/gs_crt.h"
#include "runtime/gs/gs_dither.h"
#include "runtime/gs/gs_fullcolour_addr.h"
#include "runtime/gs/gs_mip.h"
#include "runtime/gs/ps2_gs_common.h"
#include "runtime/gs/ps2_gs_psmct16.h"
#include "runtime/gs/ps2_gs_psmct32.h"
#include "runtime/gs/ps2_gs_psmt4.h"
#include "runtime/gs/ps2_gs_psmt8.h"
#include "runtime/gs/ps2_gs_memory.h"
#include "ps2_log.h"
#include <atomic>
#include <functional>
#include <thread>
#include <condition_variable>
#include <mutex>
#include <string>
#include <map>
#include <chrono>
#include <algorithm>
#include <cstdlib>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iostream>

#include <climits>
#include <emmintrin.h>

using namespace GSInternal;

// Small per-pixel helpers must be inlined into the raster loops.
#if defined(_MSC_VER)
#define GS_PIXEL_INLINE __forceinline
#else
#define GS_PIXEL_INLINE inline __attribute__((always_inline))
#endif

namespace
{
    float fabsQ(float q)
    {
        return (std::fabs(q) > 1.0e-8f) ? q : 1.0f;
    }

    GS_PIXEL_INLINE u16 Rgba8888ToRgba5551(u32 c)
    {
        uint32_t r = ((c >> 0) & 0xFF) >> 3;
        uint32_t g = ((c >> 8) & 0xFF) >> 3;
        uint32_t b = ((c >> 16) & 0xFF) >> 3;
        uint32_t a = ((c >> 24) & 0xFF) >> 7;

        return (r | (g << 5) | (b << 10) | (a << 15));
    }

    GS_PIXEL_INLINE u32 Rgba5551ToRgba8888(u16 c)
    {
        u32 r = ((c >> 0) & 0x1F) << 3;
        u32 g = ((c >> 5) & 0x1F) << 3;
        u32 b = ((c >> 10) & 0x1F) << 3;
        u32 a = ((c >> 15) & 0x01) << 7;

        return (r | (g << 8) | (b << 16) | (a << 24));
    }

    GS_PIXEL_INLINE u32 pack32(u8 r, u8 g, u8 b, u8 a)
    {
        return static_cast<u32>(r) | (g << 8) | (b << 16) | (a << 24);
    }

    GS_PIXEL_INLINE uint32_t applyTexa(const GSTexaReg &texa, uint8_t psm, uint32_t texel)
    {
        if (psm == GS_PSM_CT32)
            return texel;

        const uint8_t r = static_cast<uint8_t>(texel & 0xFFu);
        const uint8_t g = static_cast<uint8_t>((texel >> 8) & 0xFFu);
        const uint8_t b = static_cast<uint8_t>((texel >> 16) & 0xFFu);
        const bool rgbZero = r == 0u && g == 0u && b == 0u;
        uint8_t a = static_cast<uint8_t>((texel >> 24) & 0xFFu);

        switch (psm)
        {
        case GS_PSM_CT24:
            a = (texa.aem && rgbZero) ? 0u : texa.ta0;
            break;
        case GS_PSM_CT16:
        case GS_PSM_CT16S:
            if ((a & 0x80u) != 0u)
                a = texa.ta1;
            else
                a = (texa.aem && rgbZero) ? 0u : texa.ta0;
            break;
        default:
            break;
        }

        return (texel & 0x00FFFFFFu) | (static_cast<uint32_t>(a) << 24);
    }

    uint32_t addrPSMCT16Family(uint32_t basePtr, uint32_t width, uint8_t psm, uint32_t x, uint32_t y)
    {
        switch (psm)
        {
        case GS_PSM_CT16:
            return GSPSMCT16::addrPSMCT16(basePtr, width, x, y);
        case GS_PSM_CT16S:
            return GSPSMCT16::addrPSMCT16S(basePtr, width, x, y);
        case GS_PSM_Z16:
            return GSPSMCT16::addrPSMZ16(basePtr, width, x, y);
        case GS_PSM_Z16S:
            return GSPSMCT16::addrPSMZ16S(basePtr, width, x, y);
        default:
            return 0u;
        }
    }

    std::atomic<uint32_t> s_debugPrimitiveCount{0};
    std::atomic<uint32_t> s_debugPixelCount{0};
    std::atomic<uint32_t> s_debugContext1PrimitiveCount{0};
    std::atomic<uint32_t> s_debugFbp150PixelCount{0};

    GS_PIXEL_INLINE int wrapTextureCoordinate(int coordinate,
                              int textureSize,
                              uint8_t mode,
                              uint16_t regionMin,
                              uint16_t regionMax)
    {
        switch (mode & 0x3u)
        {
        case 0: // REPEAT
            return static_cast<int>(static_cast<uint32_t>(coordinate) & static_cast<uint32_t>(textureSize - 1));
        case 1: // CLAMP
            return clampInt(coordinate, 0, textureSize - 1);
        case 2: // REGION_CLAMP
            return std::min(std::max(coordinate, static_cast<int>(regionMin)), static_cast<int>(regionMax));
        case 3: // REGION_REPEAT
            return static_cast<int>((static_cast<uint32_t>(coordinate) & static_cast<uint32_t>(regionMin)) | static_cast<uint32_t>(regionMax));
        default:
            return coordinate;
        }
    }

    GS_PIXEL_INLINE bool passesAlphaTest(uint64_t testReg, uint8_t alpha)
    {
        if ((testReg & 0x1u) == 0u)
            return true;

        const uint8_t atst = static_cast<uint8_t>((testReg >> 1) & 0x7u);
        const uint8_t aref = static_cast<uint8_t>((testReg >> 4) & 0xFFu);

        switch (atst)
        {
        case 0:
            return false;
        case 1:
            return true;
        case 2:
            return alpha < aref;
        case 3:
            return alpha <= aref;
        case 4:
            return alpha == aref;
        case 5:
            return alpha >= aref;
        case 6:
            return alpha > aref;
        case 7:
            return alpha != aref;
        default:
            return true;
        }
    }

    struct PixelWriteMask
    {
        bool writeRgb = true;
        bool writeAlpha = true;
        bool writeDepth = true;

        bool writesFramebuffer() const
        {
            return writeRgb || writeAlpha;
        }

        bool writesAnything() const
        {
            return writesFramebuffer() || writeDepth;
        }
    };

    GS_PIXEL_INLINE PixelWriteMask classifyAlphaTest(uint64_t testReg, uint8_t alpha, uint8_t framePsm)
    {
        const bool pass = passesAlphaTest(testReg, alpha);
        if (pass)
            return {};

        // TEST.AFAIL controls what happens when the alpha comparison fails.
        switch (static_cast<uint8_t>((testReg >> 12) & 0x3u))
        {
        case 1: // FB_ONLY
            return {true, true, false};
        case 2: // ZB_ONLY
            return {false, false, true};
        case 3: // RGB_ONLY
            // RGB_ONLY is only distinct for RGBA32. The GS treats it as
            // FB_ONLY for RGB24 and RGBA16 framebuffers.
            if (framePsm == GS_PSM_CT32)
                return {true, false, false};
            return {true, true, false};
        case 0: // KEEP
        default:
            return {false, false, false};
        }
    }

    GS_PIXEL_INLINE bool passesDestinationAlphaTest(uint64_t testReg, uint8_t framePsm, uint32_t rawFramebufferPixel)
    {
        const bool date = ((testReg >> 14) & 0x1u) != 0u;
        if (!date)
            return true;

        const bool datm = ((testReg >> 15) & 0x1u) != 0u;
        switch (framePsm)
        {
        case GS_PSM_CT32:
            return (((rawFramebufferPixel >> 31) & 0x1u) != 0u) == datm;
        case GS_PSM_CT16:
        case GS_PSM_CT16S:
            return (((rawFramebufferPixel >> 15) & 0x1u) != 0u) == datm;
        case GS_PSM_CT24:
            // RGB24 has no destination alpha, so DATE always passes.
            return true;
        default:
            return true;
        }
    }

    struct TextureCombineResult
    {
        uint8_t r;
        uint8_t g;
        uint8_t b;
        uint8_t a;
    };

    GS_PIXEL_INLINE TextureCombineResult combineTexture(const GSTex0Reg &tex,
                                        uint8_t vr,
                                        uint8_t vg,
                                        uint8_t vb,
                                        uint8_t va,
                                        uint8_t tr,
                                        uint8_t tg,
                                        uint8_t tb,
                                        uint8_t ta)
    {
        const bool textureHasAlpha = tex.tcc != 0u;
        TextureCombineResult out{tr, tg, tb, textureHasAlpha ? ta : va};

        switch (tex.tfx)
        {
        case 0: // MODULATE
            out.r = clampU8((tr * vr) >> 7);
            out.g = clampU8((tg * vg) >> 7);
            out.b = clampU8((tb * vb) >> 7);
            out.a = textureHasAlpha ? clampU8((ta * va) >> 7) : va;
            break;
        case 1: // DECAL
            out.r = tr;
            out.g = tg;
            out.b = tb;
            out.a = textureHasAlpha ? ta : va;
            break;
        case 2: // HIGHLIGHT
            out.r = clampU8(((tr * vr) >> 7) + va);
            out.g = clampU8(((tg * vg) >> 7) + va);
            out.b = clampU8(((tb * vb) >> 7) + va);
            out.a = textureHasAlpha ? clampU8(ta + va) : va;
            break;
        case 3: // HIGHLIGHT2
            out.r = clampU8(((tr * vr) >> 7) + va);
            out.g = clampU8(((tg * vg) >> 7) + va);
            out.b = clampU8(((tb * vb) >> 7) + va);
            out.a = textureHasAlpha ? ta : va;
            break;
        default:
            out.r = tr;
            out.g = tg;
            out.b = tb;
            out.a = textureHasAlpha ? ta : va;
            break;
        }

        return out;
    }

    uint32_t swizzleClutIndexCSM1(uint32_t index)
    {
        // CSM1 swaps address bits 3 and 4. Preserve the remaining bits:
        // 16-bit CLUTs expose a ninth address bit through CSA[4].
        return (index & ~0x18u) | ((index & 0x08u) << 1u) | ((index & 0x10u) >> 1u);
    }


    uint8_t lerpChannel(uint8_t c00, uint8_t c10, uint8_t c01, uint8_t c11, float fx, float fy)
    {
        const float top = static_cast<float>(c00) + (static_cast<float>(c10) - static_cast<float>(c00)) * fx;
        const float bottom = static_cast<float>(c01) + (static_cast<float>(c11) - static_cast<float>(c01)) * fx;
        return clampU8(static_cast<int>(std::lround(top + (bottom - top) * fy)));
    }

    // ------------------------------------------------------------------
    // Per-draw pipelines for the triangle and sprite loops: the state that
    // GSCpuBackend::WritePixel / SampleTexture decode for every pixel,
    // decoded once per draw. The pixel results are the same.
    using PipeReadFn = uint32_t (*)(uint8_t *, uint32_t, uint32_t, uint32_t, uint32_t);
    using PipeWriteFn = void (*)(uint8_t *, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t);

    // VRAM access for the pipes: the GSMem accessors inlined for the formats
    // GT3 draws with (same templates and tables as GSMem::Read*/Write*), the
    // handler table for the rest. The switch is on a per-draw constant.
    template <GSMem::PixelStorageMode M>
    using PipeTraits = GSMem::PixelStorageTraits<M>;

    GS_PIXEL_INLINE uint32_t pipeRead(uint32_t psm, PipeReadFn fallback, uint8_t *vram, uint32_t bp, uint32_t bw, uint32_t x, uint32_t y)
    {
        using M = GSMem::PixelStorageMode;
        switch (psm)
        {
        case GS_PSM_CT32:
            return PipeTraits<M::C32>::Read(GSMem::PageTableC32, vram, bp, bw, x, y);
        case GS_PSM_CT24:
            return PipeTraits<M::C24>::Read(GSMem::PageTableC32, vram, bp, bw, x, y);
        case GS_PSM_Z24:
            return PipeTraits<M::Z24>::Read(GSMem::PageTableZ32, vram, bp, bw, x, y);
        case GS_PSM_Z32:
            return PipeTraits<M::Z32>::Read(GSMem::PageTableZ32, vram, bp, bw, x, y);
        case GS_PSM_T8:
            return PipeTraits<M::P8>::Read(GSMem::PageTableP8, vram, bp, bw, x, y);
        case GS_PSM_T8H:
            return PipeTraits<M::P8H>::Read(GSMem::PageTableC32, vram, bp, bw, x, y);
        case GS_PSM_T4:
            return PipeTraits<M::P4>::Read(GSMem::PageTableP4, vram, bp, bw, x, y);
        default:
            return fallback(vram, bp, bw, x, y);
        }
    }

    GS_PIXEL_INLINE void pipeWrite(uint32_t psm, PipeWriteFn fallback, uint8_t *vram, uint32_t bp, uint32_t bw, uint32_t x, uint32_t y, uint32_t value)
    {
        using M = GSMem::PixelStorageMode;
        switch (psm)
        {
        case GS_PSM_CT32:
            PipeTraits<M::C32>::Write(GSMem::PageTableC32, vram, bp, bw, x, y, value);
            return;
        case GS_PSM_CT24:
            PipeTraits<M::C24>::Write(GSMem::PageTableC32, vram, bp, bw, x, y, value);
            return;
        case GS_PSM_Z24:
            PipeTraits<M::Z24>::Write(GSMem::PageTableZ32, vram, bp, bw, x, y, value);
            return;
        case GS_PSM_Z32:
            PipeTraits<M::Z32>::Write(GSMem::PageTableZ32, vram, bp, bw, x, y, value);
            return;
        default:
            fallback(vram, bp, bw, x, y, value);
            return;
        }
    }

    // The most a Z buffer of format zpsm holds: a pixel's Z above it is clamped to it before the depth test and the
    // write (analysis/design/menus-480p/Z16-DESIGN.md 2.1), as the GL renderer clamps (Z24 already, Z16 with
    // PS2X_GS_GL_Z16), so the two renderers agree. GT3_GS_CPU_ZCLAMP=0: the old wrap (the stored value is the low
    // bits of Z, the test on the unclamped Z); GT3 builds keep the wrap unless =1.
    inline uint32_t zFormatMax(uint32_t zpsm)
    {
        static const bool clamp = [] {
            const char *v = std::getenv("GT3_GS_CPU_ZCLAMP");
#if defined(PS2X_GAME_GT3)
            return v && *v && *v != '0';
#else
            return !(v && *v == '0');
#endif
        }();
        if (!clamp)
            return 0xFFFFFFFFu;
        switch (zpsm)
        {
        case GS_PSM_Z16:
        case GS_PSM_Z16S:
            return 0xFFFFu;
        case GS_PSM_Z24:
            return 0xFFFFFFu;
        default:
            return 0xFFFFFFFFu;
        }
    }

    struct PixelPipe
    {
        uint8_t *vram = nullptr;
        int scX0 = 0, scX1 = 0, scY0 = 0, scY1 = 0;
        bool fge = false;
        uint8_t fogR = 0, fogG = 0, fogB = 0;
        uint32_t fbp = 0, fbw = 1, fpsm = 0, zbp = 0, zpsm = 0;
        uint64_t test = 0;
        bool depthEnabled = false;
        uint32_t ztestMethod = 1;
        uint32_t zMax = 0xFFFFFFFFu; // zFormatMax(zpsm): Z clamped to the Z buffer format's range
        bool abe = false, pabe = false;
        uint8_t asel = 0, bsel = 0, csel = 0, dsel = 0, fix = 0;
        uint32_t fbmsk = 0;
        bool fba = false, frame16 = false, frameCt24 = false, dateNeedsRead = false, zmask = false;
        // GS dithering (gs_dither.h): DTHE on a 16-bit frame; COLCLAMP 1 clamps, 0 keeps the low 8 bits.
        bool dither = false, colclamp = true;
        uint64_t dimx = 0;
        PipeReadFn readFrame = nullptr, readZ = nullptr;
        PipeWriteFn writeFrame = nullptr, writeZ = nullptr;
        // GT4's race colour (gs_fullcolour.h): fc 32, a CT16 / CT16S frame drawn with the CT32 frame's rules,
        // its full colour in plane; plane16: a 16-bit draw (fc 0) into a CT16 / CT16S frame while the plane
        // exists, whose plane words are set to its 16-bit colour (v << 3), as the GL renderer's 16-bit target
        // texels hold it.
        uint8_t fc = 0;
        bool plane16 = false;
        uint32_t *plane = nullptr;
    };

    PixelPipe makePixelPipe(const GSDrawState &state, uint8_t *vram, const PipeReadFn *reads, const PipeWriteFn *writes, uint32_t *plane)
    {
        const auto &ctx = state.context;
        PixelPipe p;
        p.vram = vram;
        p.scX0 = ctx.scissor.x0;
        p.scX1 = ctx.scissor.x1;
        p.scY0 = ctx.scissor.y0;
        p.scY1 = ctx.scissor.y1;
        p.fge = state.prim.fge;
        p.fogR = state.fogR;
        p.fogG = state.fogG;
        p.fogB = state.fogB;
        p.fbp = GSInternal::framePageBaseToBlock(ctx.frame.fbp);
        p.fbw = std::max<u32>(ctx.frame.fbw, 1u);
        p.fpsm = ctx.frame.psm;
        p.zbp = GSInternal::framePageBaseToBlock(ctx.zbuf.zbp);
        p.zpsm = ctx.zbuf.psm;
        p.zMax = zFormatMax(p.zpsm);
        p.test = ctx.test;
        p.depthEnabled = (ctx.test & (1ull << 16)) != 0u;
        p.ztestMethod = p.depthEnabled ? static_cast<uint32_t>((ctx.test >> 17) & 3u) : 1u;
        p.abe = state.prim.abe;
        p.pabe = state.pabe;
        const uint64_t alphaReg = ctx.alpha;
        p.asel = alphaReg & 3;
        p.bsel = (alphaReg >> 2) & 3;
        p.csel = (alphaReg >> 4) & 3;
        p.dsel = (alphaReg >> 6) & 3;
        p.fix = static_cast<uint8_t>((alphaReg >> 32) & 0xFF);
        p.fbmsk = ctx.frame.fbmsk;
        p.fba = (ctx.fba & 0x1ull) != 0ull && ctx.frame.psm != GS_PSM_CT24;
        p.frame16 = bitsPerPixel(p.fpsm) == 16;
        p.dither = gsdither::applies(state);
        p.colclamp = (state.colclamp & 1u) != 0u;
        p.dimx = state.dimx;
        p.frameCt24 = p.fpsm == GS_PSM_CT24;
        p.dateNeedsRead = ((ctx.test >> 14) & 0x1u) != 0u &&
                          (p.fpsm == GS_PSM_CT32 || p.fpsm == GS_PSM_CT16 || p.fpsm == GS_PSM_CT16S);
        p.zmask = ctx.zbuf.zmask;
        p.readFrame = reads[p.fpsm & 0x3Fu];
        p.readZ = reads[p.zpsm & 0x3Fu];
        p.writeFrame = writes[p.fpsm & 0x3Fu];
        p.writeZ = writes[p.zpsm & 0x3Fu];
        if (state.fc != 0u && gsfc::frame16(p.fpsm) && plane)
        {
            // Full colour (gs_fullcolour.h): the CT32 frame's rules; FBA as the memory format (CT16) keeps it;
            // DATE reads the full colour's alpha bit 7.
            p.fc = state.fc;
            p.plane = plane;
            p.dither = false;
            p.dateNeedsRead = ((ctx.test >> 14) & 0x1u) != 0u;
        }
        else if (plane && gsfc::frame16(p.fpsm))
        {
            p.plane16 = true;
            p.plane = plane;
        }
        return p;
    }

    GS_PIXEL_INLINE uint16_t readHalf(const uint8_t *vram, uint32_t h)
    {
        uint16_t v;
        std::memcpy(&v, vram + static_cast<size_t>(h) * 2u, 2);
        return v;
    }
    GS_PIXEL_INLINE void writeHalf(uint8_t *vram, uint32_t h, uint16_t v) { std::memcpy(vram + static_cast<size_t>(h) * 2u, &v, 2); }

    inline int pickBlendRgb(uint8_t sel, int cs, int cd)
    {
        if (sel == 0)
            return cs;
        if (sel == 1)
            return cd;
        return 0;
    }

    // A full-colour pixel (gs_fullcolour.h, PixelPipe::fc): as a CT32 frame computes it, from the pixel's full
    // colour F (Cd; Ad = F.a; DATE on F.a bit 7); no dithering; the result goes into the plane and its 16-bit
    // reduction into GS memory. (Fog is applied by the caller.)
    inline void writePixelPipeFc(const PixelPipe &p, int x, int y, int z, uint8_t r, uint8_t g, uint8_t b, uint8_t a)
    {
        const PixelWriteMask writeMask = classifyAlphaTest(p.test, a, GS_PSM_CT32);
        if (!writeMask.writesAnything())
            return;
        const bool preserveDestinationAlpha = writeMask.writeRgb && !writeMask.writeAlpha;
        const bool needF = p.dateNeedsRead || (writeMask.writesFramebuffer() && (p.fbmsk != 0 || p.abe || preserveDestinationAlpha));
        const uint32_t h = gsfc::halfIndex(p.fpsm, p.fbp, p.fbw, static_cast<uint32_t>(x), static_cast<uint32_t>(y));
        uint32_t f = 0;
        if (needF)
            f = gsfc::full(p.plane[h], readHalf(p.vram, h));
        if (p.dateNeedsRead && (((f >> 31) & 1u) != 0u) != (((p.test >> 15) & 1u) != 0u))
            return;
        const uint32_t zc = std::min(static_cast<uint32_t>(z), p.zMax); // (zFormatMax)
        switch (p.ztestMethod)
        {
        case 0:
            return;
        case 2:
            if (!(zc >= pipeRead(p.zpsm, p.readZ, p.vram, p.zbp, p.fbw, x, y)))
                return;
            break;
        case 3:
            if (!(zc > pipeRead(p.zpsm, p.readZ, p.vram, p.zbp, p.fbw, x, y)))
                return;
            break;
        default:
            break;
        }
        if (writeMask.writesFramebuffer())
        {
            if (p.abe && !(p.pabe && (a & 0x80u) == 0u))
            {
                const int dr = f & 0xFF, dg = (f >> 8) & 0xFF, db = (f >> 16) & 0xFF;
                const int da = static_cast<int>(f >> 24);
                const int cAlpha = (p.csel == 0) ? a : (p.csel == 1) ? da : p.fix;
                // COLCLAMP as the GS applies it to a 32-bit frame: 1 clamps to 0..255, 0 keeps the low 8 bits
                // (as the 16-bit draw's dithering does, gsdither::channel). The GL renderer draws COLCLAMP 0 here.
                const auto colclamp = [&p](int v) { return p.colclamp ? clampU8(v) : static_cast<uint8_t>(v & 0xFF); };
                r = colclamp(((pickBlendRgb(p.asel, r, dr) - pickBlendRgb(p.bsel, r, dr)) * cAlpha >> 7) + pickBlendRgb(p.dsel, r, dr));
                g = colclamp(((pickBlendRgb(p.asel, g, dg) - pickBlendRgb(p.bsel, g, dg)) * cAlpha >> 7) + pickBlendRgb(p.dsel, g, dg));
                b = colclamp(((pickBlendRgb(p.asel, b, db) - pickBlendRgb(p.bsel, b, db)) * cAlpha >> 7) + pickBlendRgb(p.dsel, b, db));
            }
            if (writeMask.writeAlpha && p.fba)
                a = static_cast<uint8_t>(a | 0x80u);
            u32 pixel = pack32(r, g, b, a);
            if (p.fbmsk != 0)
                pixel = (pixel & ~p.fbmsk) | (f & p.fbmsk);
            if (preserveDestinationAlpha)
                pixel = (pixel & 0x00FFFFFFu) | (f & 0xFF000000u);
            p.plane[h] = pixel;
            writeHalf(p.vram, h, gsfc::reduce(pixel));
        }
        if (p.depthEnabled && writeMask.writeDepth && !p.zmask)
            pipeWrite(p.zpsm, p.writeZ, p.vram, p.zbp, p.fbw, x, y, zc);
    }

    // GSCpuBackend::WritePixel with the per-draw decoding done by makePixelPipe.
    inline void writePixelPipe(const PixelPipe &p, int x, int y, int z, uint8_t r, uint8_t g, uint8_t b, uint8_t a, uint8_t fog)
    {
        if (x < p.scX0 || x > p.scX1 || y < p.scY0 || y > p.scY1)
            return;
        if (p.fge)
        {
            const uint32_t inverseFog = 255u - fog;
            r = static_cast<uint8_t>(((static_cast<uint32_t>(fog) * r) >> 8) + ((inverseFog * p.fogR) >> 8));
            g = static_cast<uint8_t>(((static_cast<uint32_t>(fog) * g) >> 8) + ((inverseFog * p.fogG) >> 8));
            b = static_cast<uint8_t>(((static_cast<uint32_t>(fog) * b) >> 8) + ((inverseFog * p.fogB) >> 8));
        }
        if (p.fc)
        {
            writePixelPipeFc(p, x, y, z, r, g, b, a);
            return;
        }
        const PixelWriteMask writeMask = classifyAlphaTest(p.test, a, static_cast<uint8_t>(p.fpsm));
        if (!writeMask.writesAnything())
            return;
        const bool preserveDestinationAlpha = writeMask.writeRgb && !writeMask.writeAlpha && p.fpsm == GS_PSM_CT32;
        const bool frmw = p.dateNeedsRead || (writeMask.writesFramebuffer() && (p.fbmsk != 0 || p.abe || preserveDestinationAlpha));
        u32 rawFramebufferPixel = 0;
        u32 fbrgba = 0;
        if (frmw)
        {
            rawFramebufferPixel = pipeRead(p.fpsm, p.readFrame, p.vram, p.fbp, p.fbw, x, y);
            fbrgba = rawFramebufferPixel;
            if (p.frame16)
                fbrgba = Rgba5551ToRgba8888(static_cast<u16>(fbrgba));
            else if (p.frameCt24)
                fbrgba |= 0x80000000u;
        }
        if (!passesDestinationAlphaTest(p.test, static_cast<uint8_t>(p.fpsm), rawFramebufferPixel))
            return;
        const uint32_t zc = std::min(static_cast<uint32_t>(z), p.zMax); // (zFormatMax)
        switch (p.ztestMethod)
        {
        case 0:
            return;
        case 2:
            if (!(zc >= pipeRead(p.zpsm, p.readZ, p.vram, p.zbp, p.fbw, x, y)))
                return;
            break;
        case 3:
            if (!(zc > pipeRead(p.zpsm, p.readZ, p.vram, p.zbp, p.fbw, x, y)))
                return;
            break;
        default:
            break;
        }
        if (writeMask.writesFramebuffer())
        {
            if (p.dither)
            {
                // GS dithering: the blend result unclamped, plus DIMX at the pixel's (x & 3, y & 3), then
                // COLCLAMP; the 16-bit conversion below drops the low 3 bits.
                int ri = r, gi = g, bi = b;
                if (p.abe && !(p.pabe && (a & 0x80u) == 0u))
                {
                    const int dr = fbrgba & 0xFF, dg = (fbrgba >> 8) & 0xFF, db = (fbrgba >> 16) & 0xFF, da = (fbrgba >> 24) & 0xFF;
                    const int cAlpha = (p.csel == 0) ? a : (p.csel == 1) ? da : p.fix;
                    ri = ((pickBlendRgb(p.asel, r, dr) - pickBlendRgb(p.bsel, r, dr)) * cAlpha >> 7) + pickBlendRgb(p.dsel, r, dr);
                    gi = ((pickBlendRgb(p.asel, g, dg) - pickBlendRgb(p.bsel, g, dg)) * cAlpha >> 7) + pickBlendRgb(p.dsel, g, dg);
                    bi = ((pickBlendRgb(p.asel, b, db) - pickBlendRgb(p.bsel, b, db)) * cAlpha >> 7) + pickBlendRgb(p.dsel, b, db);
                }
                const int d = gsdither::offset(p.dimx, x, y);
                r = gsdither::channel(ri, d, p.colclamp);
                g = gsdither::channel(gi, d, p.colclamp);
                b = gsdither::channel(bi, d, p.colclamp);
            }
            else if (p.abe && !(p.pabe && (a & 0x80u) == 0u))
            {
                const uint8_t dr = fbrgba & 0xFF;
                const uint8_t dg = (fbrgba >> 8) & 0xFF;
                const uint8_t db = (fbrgba >> 16) & 0xFF;
                const uint8_t da = (fbrgba >> 24) & 0xFF;
                const int cAlpha = (p.csel == 0) ? a : (p.csel == 1) ? da : p.fix;
                r = clampU8(((pickBlendRgb(p.asel, r, dr) - pickBlendRgb(p.bsel, r, dr)) * cAlpha >> 7) + pickBlendRgb(p.dsel, r, dr));
                g = clampU8(((pickBlendRgb(p.asel, g, dg) - pickBlendRgb(p.bsel, g, dg)) * cAlpha >> 7) + pickBlendRgb(p.dsel, g, dg));
                b = clampU8(((pickBlendRgb(p.asel, b, db) - pickBlendRgb(p.bsel, b, db)) * cAlpha >> 7) + pickBlendRgb(p.dsel, b, db));
            }
            if (writeMask.writeAlpha && p.fba)
                a = static_cast<uint8_t>(a | 0x80u);
            u32 pixel = pack32(r, g, b, a);
            if (p.fbmsk != 0)
                pixel = (pixel & ~p.fbmsk) | (fbrgba & p.fbmsk);
            if (preserveDestinationAlpha)
                pixel = (pixel & 0x00FFFFFFu) | (fbrgba & 0xFF000000u);
            if (p.frame16)
                pixel = Rgba8888ToRgba5551(pixel);
            pipeWrite(p.fpsm, p.writeFrame, p.vram, p.fbp, p.fbw, x, y, pixel);
            if (p.plane16)
                p.plane[gsfc::halfIndex(p.fpsm, p.fbp, p.fbw, static_cast<uint32_t>(x), static_cast<uint32_t>(y))] = gsfc::expand(pixel);
        }
        if (p.depthEnabled && writeMask.writeDepth && !p.zmask)
            pipeWrite(p.zpsm, p.writeZ, p.vram, p.zbp, p.fbw, x, y, zc);
    }

    struct TexPipe
    {
        uint8_t *vram = nullptr;
        PipeReadFn read = nullptr;
        uint32_t psm = 0, tbp0 = 0, tbw = 0;
        int texW = 1, texH = 1;
        uint8_t wrapU = 0, wrapV = 0;
        uint16_t minU = 0, maxU = 0, minV = 0, maxV = 0;
        GSTexaReg texa{};
        uint8_t cpsm = 0, csa = 0;
        bool fourBit = false, clutFormat = false, format16 = false;
        // GT4's race colour (GSDrawState::ftex, gs_fullcolour.h texFlag): a CT16 display buffer read as its texels'
        // full colour from the plane (alpha as the 16-bit value gives it). nullptr: the 16-bit value.
        const uint32_t *fullPlane = nullptr;
        const uint16_t *clut = nullptr;
        bool linear = false, fst = false;
        // 4-bit formats: the 16 CLUT colours, looked up once per draw.
        uint32_t palette16[16];
        // GS mipmapping (gs_mip.h). mip: a per-pixel primitive (mipTag 0x80),
        // its levels 0..MXL in lv (filled only then). extraTag: a primitive
        // resolved to one level (mipTag 1 + n) whose texels are tinted
        // (PS2X_GS_MIP_TINT) or whose pixels are checked (PS2X_GS_MIP_STATS).
        bool mip = false;
        bool mipConfine = false; // gsmip::rangeClampEnabled: picks kept in mipLo..mipHi
        uint8_t mipLo = 0, mipHi = 0, extraTag = 0;
        gsmip::Tex1 t1{};
        struct LevelPipe
        {
            uint32_t tbp0, tbw;
            int w, h;
            uint16_t minU, maxU, minV, maxV;
        };
        LevelPipe lv[7];
    };

    GS_PIXEL_INLINE uint32_t lookupClutPipe(const TexPipe &t, uint8_t index);

    TexPipe makeTexPipe(const GSDrawState &state, uint8_t *vram, const PipeReadFn *reads, const uint16_t *clut, const uint32_t *plane)
    {
        const auto &ctx = state.context;
        const auto &tex = ctx.tex0;
        TexPipe t;
        t.vram = vram;
        t.read = reads[tex.psm & 0x3Fu];
        t.psm = tex.psm;
        t.tbp0 = tex.tbp0;
        t.tbw = tex.tbw;
        t.texW = state.textureWidth;
        t.texH = state.textureHeight;
        const uint64_t clamp = ctx.clamp;
        t.wrapU = static_cast<uint8_t>(clamp & 0x3u);
        t.wrapV = static_cast<uint8_t>((clamp >> 2) & 0x3u);
        t.minU = static_cast<uint16_t>((clamp >> 4) & 0x3FFu);
        t.maxU = static_cast<uint16_t>((clamp >> 14) & 0x3FFu);
        t.minV = static_cast<uint16_t>((clamp >> 24) & 0x3FFu);
        t.maxV = static_cast<uint16_t>((clamp >> 34) & 0x3FFu);
        t.texa = state.texa;
        t.cpsm = tex.cpsm;
        t.csa = tex.csa;
        t.fourBit = tex.psm == GS_PSM_T4 || tex.psm == GS_PSM_T4HH || tex.psm == GS_PSM_T4HL;
        t.clutFormat = tex.psm == GS_PSM_T8 || tex.psm == GS_PSM_T8H || t.fourBit;
        t.format16 = tex.psm == GS_PSM_CT16 || tex.psm == GS_PSM_CT16S || tex.psm == GS_PSM_Z16 || tex.psm == GS_PSM_Z16S;
        if (state.ftex != 0u && tex.psm == GS_PSM_CT16 && state.mipTag != gsmip::kTagPerPixel)
            t.fullPlane = plane;
        t.clut = clut;
        t.linear = state.linearFilter;
        t.fst = state.prim.fst;
        if (t.fourBit)
            for (uint32_t index = 0; index < 16u; ++index)
                t.palette16[index] = lookupClutPipe(t, static_cast<uint8_t>(index));
        if (state.mipTag == gsmip::kTagPerPixel)
        {
            t.mip = true;
            t.t1 = gsmip::decodeTex1(ctx.tex1);
            t.mipConfine = gsmip::rangeClampEnabled();
            t.mipLo = state.mipLo;
            t.mipHi = state.mipHi;
            for (int n = 0; n <= static_cast<int>(t.t1.mxl); ++n)
            {
                const gsmip::Level L = gsmip::level(ctx, n);
                t.lv[n] = {L.tbp, L.tbw, static_cast<int>(L.w), static_cast<int>(L.h), L.minU, L.maxU, L.minV, L.maxV};
            }
        }
        else if (state.mipTag != 0u && (gsmip::tintEnabled() || gsmip::statsOn()))
        {
            t.extraTag = state.mipTag;
            t.t1 = gsmip::decodeTex1(ctx.tex1);
        }
        return t;
    }

    // GSCpuBackend::LookupCLUT.
    GS_PIXEL_INLINE uint32_t lookupClutPipe(const TexPipe &t, uint8_t index)
    {
        const uint32_t texel = t.fourBit ? (index & 0x0Fu) : index;
        switch (t.cpsm)
        {
        case GS_PSM_CT16:
        case GS_PSM_CT16S:
            return applyTexa(t.texa, t.cpsm, Rgba5551ToRgba8888(t.clut[(static_cast<uint32_t>(t.csa) * 16u + texel) & 0x1FFu]));
        case GS_PSM_CT32:
        case GS_PSM_CT24:
        {
            const uint32_t entry = (static_cast<uint32_t>(t.csa & 0x0Fu) * 16u + texel) & 0xFFu;
            const uint32_t value = static_cast<uint32_t>(t.clut[entry]) | (static_cast<uint32_t>(t.clut[entry + 256u]) << 16u);
            return applyTexa(t.texa, t.cpsm, value);
        }
        default:
            return 0xFFFF00FFu;
        }
    }

    // lerpChannel for the four channels of four RGBA texels at once. Each lane
    // does the same float operations in the same order, and std::lround (half
    // away from zero; the values are never negative) becomes truncation plus a
    // branch-free compare of the exact remainder with 0.5.
    GS_PIXEL_INLINE uint32_t lerpTexels(uint32_t c00, uint32_t c10, uint32_t c01, uint32_t c11, float fx, float fy)
    {
        const __m128i zero = _mm_setzero_si128();
        auto channels = [&](uint32_t texel)
        {
            return _mm_cvtepi32_ps(_mm_unpacklo_epi16(_mm_unpacklo_epi8(_mm_cvtsi32_si128(static_cast<int>(texel)), zero), zero));
        };
        const __m128 a = channels(c00), b = channels(c10), c = channels(c01), d = channels(c11);
        const __m128 vfx = _mm_set1_ps(fx), vfy = _mm_set1_ps(fy);
        const __m128 top = _mm_add_ps(a, _mm_mul_ps(_mm_sub_ps(b, a), vfx));
        const __m128 bottom = _mm_add_ps(c, _mm_mul_ps(_mm_sub_ps(d, c), vfx));
        const __m128 value = _mm_add_ps(top, _mm_mul_ps(_mm_sub_ps(bottom, top), vfy));
        const __m128i whole = _mm_cvttps_epi32(value);
        const __m128 rest = _mm_sub_ps(value, _mm_cvtepi32_ps(whole));
        // mask lanes are -1 where rest >= 0.5: subtracting the mask adds one
        const __m128i rounded = _mm_sub_epi32(whole, _mm_castps_si128(_mm_cmpge_ps(rest, _mm_set1_ps(0.5f))));
        // clampU8 for each lane, then pack to bytes
        const __m128i packed16 = _mm_packs_epi32(rounded, rounded);
        const __m128i packed8 = _mm_packus_epi16(packed16, packed16);
        return static_cast<uint32_t>(_mm_cvtsi128_si32(packed8));
    }

    // static_cast<int>(std::floor(value)) without the library call (the
    // conversion of out-of-range values gives INT_MIN in both).
    GS_PIXEL_INLINE int floorToInt(float value)
    {
        int whole = static_cast<int>(value);
        if (value < static_cast<float>(whole) && whole != INT_MIN)
            --whole;
        return whole;
    }

    // SampleTexture's samplePoint.
    GS_PIXEL_INLINE uint32_t samplePointPipe(const TexPipe &t, int u, int v)
    {
        u = wrapTextureCoordinate(u, t.texW, t.wrapU, t.minU, t.maxU);
        v = wrapTextureCoordinate(v, t.texH, t.wrapV, t.minV, t.maxV);
        const u32 out = pipeRead(t.psm, t.read, t.vram, t.tbp0, t.tbw, static_cast<uint32_t>(u), static_cast<uint32_t>(v));
        if (t.fourBit)
            return t.palette16[out & 0x0Fu];
        if (t.clutFormat)
            return lookupClutPipe(t, static_cast<u8>(out));
        switch (t.psm)
        {
        case GS_PSM_CT32:
        case GS_PSM_Z32:
        case GS_PSM_CT24:
        case GS_PSM_Z24:
            return applyTexa(t.texa, static_cast<uint8_t>(t.psm), out);
        default:
            break;
        }
        if (t.fullPlane)
        {
            // Full colour (gs_fullcolour.h): RGB from F, alpha as from the 16-bit value (bit 15, TEXA, AEM on its RGB).
            const uint32_t m = out & 0xFFFFu;
            const uint32_t f = gsfc::full(t.fullPlane[gsfc::halfIndex(GS_PSM_CT16, t.tbp0, t.tbw, static_cast<uint32_t>(u), static_cast<uint32_t>(v))], m);
            return (f & 0x00FFFFFFu) | (applyTexa(t.texa, static_cast<uint8_t>(t.psm), Rgba5551ToRgba8888(static_cast<u16>(m))) & 0xFF000000u);
        }
        if (t.format16)
            return applyTexa(t.texa, static_cast<uint8_t>(t.psm), Rgba5551ToRgba8888(static_cast<u16>(out)));
        return 0xFFFF00FFu;
    }

    // GSCpuBackend::SampleTexture.
    inline uint32_t sampleTexturePipe(const TexPipe &t, float s, float tt, float q, uint16_t u, uint16_t v)
    {
        float texUf, texVf;
        if (t.fst)
        {
            texUf = static_cast<float>(u) / 16.0f;
            texVf = static_cast<float>(v) / 16.0f;
        }
        else
        {
            const float invQ = 1.0f / fabsQ(q);
            texUf = s * invQ * static_cast<float>(t.texW);
            texVf = tt * invQ * static_cast<float>(t.texH);
        }
        if (!t.linear)
            return samplePointPipe(t, static_cast<int>(texUf), static_cast<int>(texVf));

        const float sampleU = texUf - 0.5f;
        const float sampleV = texVf - 0.5f;
        const int u0 = floorToInt(sampleU);
        const int v0 = floorToInt(sampleV);
        const float fx = sampleU - static_cast<float>(u0);
        const float fy = sampleV - static_cast<float>(v0);
        const uint32_t c00 = samplePointPipe(t, u0, v0);
        const uint32_t c10 = samplePointPipe(t, u0 + 1, v0);
        const uint32_t c01 = samplePointPipe(t, u0, v0 + 1);
        const uint32_t c11 = samplePointPipe(t, u0 + 1, v0 + 1);
        return lerpTexels(c00, c10, c01, c11, fx, fy);
    }

    // ---- GS mipmapping, per-pixel primitives (gs_mip.h; mipmap-design.md 4.3).
    // samplePointPipe with a level's memory, size and CLAMP region (the CLUT,
    // format, TEXA and wrap modes are level 0's). samplePointPipe itself is left
    // as it was, so other draws keep their code and speed.
    GS_PIXEL_INLINE uint32_t samplePointLevel(const TexPipe &t, const TexPipe::LevelPipe &L, int u, int v)
    {
        u = wrapTextureCoordinate(u, L.w, t.wrapU, L.minU, L.maxU);
        v = wrapTextureCoordinate(v, L.h, t.wrapV, L.minV, L.maxV);
        const u32 out = pipeRead(t.psm, t.read, t.vram, L.tbp0, L.tbw, static_cast<uint32_t>(u), static_cast<uint32_t>(v));
        if (t.fourBit)
            return t.palette16[out & 0x0Fu];
        if (t.clutFormat)
            return lookupClutPipe(t, static_cast<u8>(out));
        switch (t.psm)
        {
        case GS_PSM_CT32:
        case GS_PSM_Z32:
        case GS_PSM_CT24:
        case GS_PSM_Z24:
            return applyTexa(t.texa, static_cast<uint8_t>(t.psm), out);
        default:
            break;
        }
        if (t.format16)
            return applyTexa(t.texa, static_cast<uint8_t>(t.psm), Rgba5551ToRgba8888(static_cast<u16>(out)));
        return 0xFFFF00FFu;
    }

    // One level at (su, sv) = (S/Q, T/Q): texel coordinate su * width, the
    // same float operations as sampleTexturePipe's s * invQ * texW, so a level
    // read here gives the pixels a primitive resolved to that level gives.
    GS_PIXEL_INLINE uint32_t sampleLevelPipe(const TexPipe &t, const TexPipe::LevelPipe &L, float su, float sv, bool linear)
    {
        const float texUf = su * static_cast<float>(L.w);
        const float texVf = sv * static_cast<float>(L.h);
        if (!linear)
            return samplePointLevel(t, L, static_cast<int>(texUf), static_cast<int>(texVf));
        const float sampleU = texUf - 0.5f;
        const float sampleV = texVf - 0.5f;
        const int u0 = floorToInt(sampleU);
        const int v0 = floorToInt(sampleV);
        const float fx = sampleU - static_cast<float>(u0);
        const float fy = sampleV - static_cast<float>(v0);
        const uint32_t c00 = samplePointLevel(t, L, u0, v0);
        const uint32_t c10 = samplePointLevel(t, L, u0 + 1, v0);
        const uint32_t c01 = samplePointLevel(t, L, u0, v0 + 1);
        const uint32_t c11 = samplePointLevel(t, L, u0 + 1, v0 + 1);
        return lerpTexels(c00, c10, c01, c11, fx, fy);
    }

    // Per-pixel counters of one primitive (PS2X_GS_MIP_STATS), added to
    // gsmip::stats() once it is drawn.
    struct MipPixelCounts
    {
        uint64_t level[7] = {}, single[7] = {};
        uint64_t blended = 0, outside = 0, mismatch = 0;
        void flush() const
        {
            gsmip::Stats &s = gsmip::stats();
            for (int n = 0; n < 7; ++n)
            {
                if (level[n])
                    s.pixPerPixel[n].fetch_add(level[n], std::memory_order_relaxed);
                if (single[n])
                    s.pixSingle[n].fetch_add(single[n], std::memory_order_relaxed);
            }
            if (blended)
                s.pixBlended.fetch_add(blended, std::memory_order_relaxed);
            if (outside)
                s.outsideRange.fetch_add(outside, std::memory_order_relaxed);
            if (mismatch)
                s.singleMismatch.fetch_add(mismatch, std::memory_order_relaxed);
        }
    };

    // A per-pixel primitive's texel: the LOD from this pixel's Q, then one
    // level (or two blended) as gsmip::pick says, kept inside the primitive's
    // level range (gsmip::confine; the check counts a pick outside it first).
    inline uint32_t sampleTextureMipPipe(const TexPipe &t, float s, float tt, float q, MipPixelCounts *counts, bool tint)
    {
        const float invQ = 1.0f / gsmip::fabsQ(q);
        const float su = s * invQ, sv = tt * invQ;
        gsmip::Pick p = gsmip::pick(gsmip::lod16(q, t.t1), t.t1);
        if (counts)
            counts->outside += (p.level < t.mipLo || p.level + (p.blend != 0u ? 1u : 0u) > t.mipHi) ? 1u : 0u;
        if (t.mipConfine)
            p = gsmip::confine(p, t.t1, t.mipLo, t.mipHi);
        if (counts)
        {
            ++counts->level[p.level];
            counts->blended += p.blend != 0u ? 1u : 0u;
        }
        uint32_t c0 = sampleLevelPipe(t, t.lv[p.level], su, sv, p.linear);
        if (tint)
            c0 = gsmip::tint(c0, p.level);
        if (p.blend == 0u)
            return c0;
        uint32_t c1 = sampleLevelPipe(t, t.lv[p.level + 1u], su, sv, p.linear);
        if (tint)
            c1 = gsmip::tint(c1, p.level + 1);
        return gsmip::blendLevels(c0, c1, p.blend);
    }

    // A resolved primitive's texel (extraTag): tinted by its level, and with
    // the statistics its own per-pixel pick checked against the level and
    // filter it was resolved to (STQ triangles; sprites and FST have one LOD).
    GS_PIXEL_INLINE uint32_t mipExtraPipe(const TexPipe &t, uint32_t texel, float q, bool checkPixel, MipPixelCounts *counts)
    {
        const int lvl = static_cast<int>(t.extraTag) - 1;
        if (counts)
        {
            ++counts->single[std::clamp(lvl, 0, 6)];
            if (checkPixel)
            {
                const gsmip::Pick p = gsmip::pick(gsmip::lod16(q, t.t1), t.t1);
                counts->mismatch += (p.level != lvl || p.blend != 0u || p.linear != t.linear) ? 1u : 0u;
            }
        }
        return gsmip::tintEnabled() ? gsmip::tint(texel, lvl) : texel;
    }
}

namespace
{
    static constexpr uint32_t kDefaultDisplayWidth = 640u;
    static constexpr uint32_t kDefaultDisplayHeight = 448u;
    static constexpr uint32_t kHostFrameWidth = 640u;
    // Host frames: 640 pixels a row, at least 512 rows; taller for GT4's 1080i
    // picture (960 display lines; analysis/design/video-modes/VIDEO-MODES.md 3.2).
    static constexpr uint32_t kHostFrameHeight = 512u;
    static constexpr uint32_t kMaxDisplayHeight = 1024u;
    size_t hostFrameBytes(uint32_t rows) { return size_t(kHostFrameWidth) * std::max(kHostFrameHeight, rows) * 4u; }

    uint16_t encodeFramePixelPSMCT16(uint8_t r, uint8_t g, uint8_t b, uint8_t a)
    {
        return static_cast<uint16_t>(((r >> 3) & 0x1Fu) |
                                     (((g >> 3) & 0x1Fu) << 5) |
                                     (((b >> 3) & 0x1Fu) << 10) |
                                     ((a >= 0x40u) ? 0x8000u : 0u));
    }

    void decodeDisplaySize(uint64_t display64, uint32_t &outWidth, uint32_t &outHeight)
    {
        const uint32_t dw = static_cast<uint32_t>((display64 >> 32) & 0x0FFFu);
        const uint32_t dh = static_cast<uint32_t>((display64 >> 44) & 0x07FFu);
        const uint32_t magh = static_cast<uint32_t>((display64 >> 23) & 0x0Fu);

        outWidth = (dw + 1u) / (magh + 1u);
        outHeight = dh + 1u;
        if (outWidth < 64u || outHeight < 64u)
        {
            outWidth = kDefaultDisplayWidth;
            outHeight = kDefaultDisplayHeight;
        }
        outWidth = std::min<uint32_t>(outWidth, kHostFrameWidth);
        outHeight = std::min<uint32_t>(outHeight, kMaxDisplayHeight);
    }

    GSFrameReg decodeDisplayFrame(uint64_t dispfb64)
    {
        GSFrameReg frame{};
        frame.fbp = static_cast<uint32_t>(dispfb64 & 0x1FFu);
        frame.fbw = static_cast<uint32_t>((dispfb64 >> 9) & 0x3Fu);
        frame.psm = static_cast<uint8_t>((dispfb64 >> 15) & 0x1Fu);
        return frame;
    }

    struct GSDisplayReadOrigin
    {
        uint32_t x = 0u;
        uint32_t y = 0u;
    };

    GSDisplayReadOrigin decodeDisplayReadOrigin(uint64_t dispfb64)
    {
        return {
            static_cast<uint32_t>((dispfb64 >> 32) & 0x7FFu),
            static_cast<uint32_t>((dispfb64 >> 43) & 0x7FFu)};
    }

    bool hasDisplaySetup(uint64_t display64, const GSFrameReg &frame)
    {
        const uint32_t dw = static_cast<uint32_t>((display64 >> 32) & 0x0FFFu);
        const uint32_t dh = static_cast<uint32_t>((display64 >> 44) & 0x07FFu);
        const uint32_t magh = static_cast<uint32_t>((display64 >> 23) & 0x0Fu);
        return frame.fbw != 0u || dw != 0u || dh != 0u || magh != 0u;
    }

    struct GSPmodeState
    {
        bool enableCrt1 = false;
        bool enableCrt2 = false;
        bool mmod = false;
        bool amod = false;
        bool slbg = false;
        uint8_t alp = 0u;
    };

    GSPmodeState decodePmode(uint64_t pmode64)
    {
        return {
            (pmode64 & 0x1ull) != 0ull,
            (pmode64 & 0x2ull) != 0ull,
            ((pmode64 >> 5) & 0x1ull) != 0ull,
            ((pmode64 >> 6) & 0x1ull) != 0ull,
            ((pmode64 >> 7) & 0x1ull) != 0ull,
            static_cast<uint8_t>((pmode64 >> 8) & 0xFFu)};
    }

    struct GSSmode2State
    {
        bool interlaced = false;
        bool frameMode = true;
    };

    GSSmode2State decodeSMode2(uint64_t smode2)
    {
        return {(smode2 & 0x1ull) != 0ull, ((smode2 >> 1) & 0x1ull) != 0ull};
    }

    void applyFieldPresentation(std::vector<uint8_t> &pixels, uint32_t width, uint32_t height, bool oddField)
    {
        if (pixels.empty() || width == 0u || height < 2u)
            return;
        const std::vector<uint8_t> source = pixels;
        for (uint32_t y = 0; y < height; ++y)
        {
            uint32_t sourceY = ((y >> 1u) << 1u) + (oddField ? 1u : 0u);
            if (sourceY >= height)
                sourceY = height - 1u;
            std::memcpy(pixels.data() + y * kHostFrameWidth * 4u,
                        source.data() + sourceY * kHostFrameWidth * 4u,
                        width * 4u);
        }
    }

    void normalizePresentationAlpha(std::vector<uint8_t> &pixels, uint32_t width, uint32_t height)
    {
        for (uint32_t y = 0; y < height; ++y)
        {
            uint8_t *row = pixels.data() + y * kHostFrameWidth * 4u;
            for (uint32_t x = 0; x < width; ++x)
                row[x * 4u + 3u] = 255u;
        }
    }

    void expandInterlacedFrame(std::vector<uint8_t> &pixels, uint32_t width, uint32_t height)
    {
        // INT+FFMD reads consecutive lines from a half-height framebuffer each field.
        // Expand those source rows for host presentation, without reading the VRAM
        // beyond the frame. Walk backwards so unread source rows remain intact.
        for (uint32_t y = height; y > 1u; --y)
            std::memcpy(pixels.data() + (y - 1u) * kHostFrameWidth * 4u,
                        pixels.data() + ((y - 1u) / 2u) * kHostFrameWidth * 4u,
                        width * 4u);
    }

    // INT+FFMD (frame mode, interlaced): each field shows one half-height
    // buffer, and the game alternates buffers per field (e.g. the movie draws
    // one 224-line half of the picture per field). A TV interleaves the two
    // fields; present the same by weaving the current field with the previous
    // one (per CRT). Falls back to line doubling when there is no directly
    // preceding field (start, mode or size change, skipped VSync).
    struct WeaveHistory
    {
        std::vector<uint8_t> rows[2]; // [0] = last field, [1] = the one before
        uint64_t tick[2] = {~0ull, ~0ull};
        int parity[2] = {-1, -1};     // the rows each belongs on (-1: by VSync)
        uint32_t width = 0u, half = 0u;
    };

    // Field parity: the field shown at an even VSync tick carries the odd
    // lines (measured on the GT3 opening movie: this order removes the
    // line-pair combing the opposite order shows). GT3_WEAVE_PARITY=0 flips
    // it; GT3_WEAVE=0 restores plain line doubling.
    int weaveParityFlip()
    {
        static const int flip = [] {
            const char *v = std::getenv("GT3_WEAVE_PARITY");
            return (v && *v == '0') ? 0 : 1;
        }();
        return flip;
    }

    bool weaveEnabled()
    {
        static const bool enabled = [] {
            const char *v = std::getenv("GT3_WEAVE");
            return !(v && *v == '0');
        }();
        return enabled;
    }

    // phaseParity: the rows the field belongs on when its draws tell
    // (GT4's 1080i fields, drawn half a line apart; -1: by VSync parity).
    void weaveInterlacedFrame(std::vector<uint8_t> &pixels, uint32_t width, uint32_t height,
                              uint64_t tick, WeaveHistory &h, bool allowWeave = true, int phaseParity = -1)
    {
        const uint32_t half = (height + 1u) / 2u;
        const size_t stride = kHostFrameWidth * 4u, rowBytes = size_t(width) * 4u;
        if (h.width != width || h.half != half)
        {
            h = WeaveHistory{};
            h.width = width;
            h.half = half;
        }
        const int tickParity = static_cast<int>((tick & 1u) ^ static_cast<uint64_t>(weaveParityFlip()));
        if (h.tick[0] != tick)
        {
            std::swap(h.rows[0], h.rows[1]);
            h.tick[1] = h.tick[0];
            h.parity[1] = h.parity[0];
            h.rows[0].resize(size_t(half) * rowBytes);
            for (uint32_t k = 0; k < half; ++k)
                std::memcpy(h.rows[0].data() + k * rowBytes, pixels.data() + k * stride, rowBytes);
            h.tick[0] = tick;
            h.parity[0] = phaseParity >= 0 ? phaseParity : tickParity;
        }
        if (!allowWeave || !weaveEnabled() || h.tick[1] + 1u != tick || h.rows[1].size() != size_t(half) * rowBytes ||
            h.parity[1] == h.parity[0])
        {
            expandInterlacedFrame(pixels, width, height);
            return;
        }
        const uint32_t parity = static_cast<uint32_t>(h.parity[0]);
        for (uint32_t k = 0; k < half; ++k)
        {
            const uint32_t yCur = 2u * k + parity, yPrev = 2u * k + (1u - parity);
            if (yCur < height)
                std::memcpy(pixels.data() + yCur * stride, h.rows[0].data() + k * rowBytes, rowBytes);
            if (yPrev < height)
                std::memcpy(pixels.data() + yPrev * stride, h.rows[1].data() + k * rowBytes, rowBytes);
        }
    }

    uint8_t blendPresentationChannel(uint8_t src, uint8_t dst, uint32_t factor)
    {
        const int delta = static_cast<int>(src) - static_cast<int>(dst);
        return GSInternal::clampU8(static_cast<int>(dst) + ((delta * static_cast<int>(factor)) / 255));
    }

    [[maybe_unused]] uint32_t countNonBlackPixels(const std::vector<uint8_t> &pixels, uint32_t width, uint32_t height)
    {
        uint32_t count = 0u;
        for (uint32_t y = 0; y < height; ++y)
        {
            const uint8_t *row = pixels.data() + y * kHostFrameWidth * 4u;
            for (uint32_t x = 0; x < width; ++x)
            {
                if (row[x * 4u] != 0u || row[x * 4u + 1u] != 0u || row[x * 4u + 2u] != 0u)
                    ++count;
            }
        }
        return count;
    }
}

GSCpuBackend::GSCpuBackend()
{
    using namespace GSMem;
    static std::once_flag lookupTablesOnce;
    std::call_once(lookupTablesOnce, []()
                   { InitLookupTables(); });
    for (size_t i = 0; i < kPsmHandlerCount; ++i)
    {
        switch (i)
        {
        case GS_PSM_CT32:
            m_readVramFuncs[i] = ReadCT32;
            m_writeVramFuncs[i] = WriteCT32;
            break;
        case GS_PSM_CT24:
            m_readVramFuncs[i] = ReadCT24;
            m_writeVramFuncs[i] = WriteCT24;
            break;
        case GS_PSM_CT16:
            m_readVramFuncs[i] = ReadCT16;
            m_writeVramFuncs[i] = WriteCT16;
            break;
        case GS_PSM_CT16S:
            m_readVramFuncs[i] = ReadCT16S;
            m_writeVramFuncs[i] = WriteCT16S;
            break;
        case GS_PSM_T8:
            m_readVramFuncs[i] = ReadP8;
            m_writeVramFuncs[i] = WriteP8;
            break;
        case GS_PSM_T8H:
            m_readVramFuncs[i] = ReadP8H;
            m_writeVramFuncs[i] = WriteP8H;
            break;
        case GS_PSM_T4:
            m_readVramFuncs[i] = ReadP4;
            m_writeVramFuncs[i] = WriteP4;
            break;
        case GS_PSM_T4HH:
            m_readVramFuncs[i] = ReadP4HH;
            m_writeVramFuncs[i] = WriteP4HH;
            break;
        case GS_PSM_T4HL:
            m_readVramFuncs[i] = ReadP4HL;
            m_writeVramFuncs[i] = WriteP4HL;
            break;
        case GS_PSM_Z32:
            m_readVramFuncs[i] = ReadZ32;
            m_writeVramFuncs[i] = WriteZ32;
            break;
        case GS_PSM_Z24:
            m_readVramFuncs[i] = ReadZ24;
            m_writeVramFuncs[i] = WriteZ24;
            break;
        case GS_PSM_Z16:
            m_readVramFuncs[i] = ReadZ16;
            m_writeVramFuncs[i] = WriteZ16;
            break;
        case GS_PSM_Z16S:
            m_readVramFuncs[i] = ReadZ16S;
            m_writeVramFuncs[i] = WriteZ16S;
            break;
        default:
            m_readVramFuncs[i] = ReadNull;
            m_writeVramFuncs[i] = WriteNull;
            break;
        }
    }
    Reset();
}

void GSCpuBackend::Initialize(uint8_t *vram, uint32_t vramSize)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    flushPendingUnlocked();
    m_vram = vram;
    m_vramSize = vramSize;
    ResetUnlocked();
}

void GSCpuBackend::Reset()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    ResetUnlocked();
}

void GSCpuBackend::ResetUnlocked()
{
    flushPendingUnlocked();
    m_transfer = {};
    m_transfer.direction = 3u;
    m_transferState = {};
    m_transferState.direction = 3u;
    m_localToHostBuffer.clear();
    m_localToHostReadPos = 0u;
}

namespace
{
    // Opt-in diagnostic: GT3_GS_PROFILE=1 accumulates rasterizer time per draw
    // kind (primitive, texture format, frame page, blending) and prints the
    // table when the process exits.
    struct DrawProfile
    {
        struct Entry
        {
            uint64_t draws = 0;
            double seconds = 0.0;
        };
        std::map<std::string, Entry> entries;
        std::mutex mutex;
        bool enabled = [] {
            const char *v = std::getenv("GT3_GS_PROFILE");
            return v && *v == '1';
        }();
        ~DrawProfile()
        {
            if (!enabled || entries.empty())
                return;
            std::vector<std::pair<std::string, Entry>> rows(entries.begin(), entries.end());
            std::sort(rows.begin(), rows.end(), [](const auto &a, const auto &b) { return a.second.seconds > b.second.seconds; });
            double total = 0.0;
            for (const auto &row : rows)
                total += row.second.seconds;
            std::fprintf(stderr, "[gs-profile] total %.3f s\n", total);
            for (size_t i = 0; i < rows.size() && i < 40; ++i)
                std::fprintf(stderr, "[gs-profile] %6.2f%% %9.3f ms %8llu draws  %s\n", 100.0 * rows[i].second.seconds / total,
                             rows[i].second.seconds * 1000.0, static_cast<unsigned long long>(rows[i].second.draws), rows[i].first.c_str());
        }
    };
    DrawProfile g_drawProfile;
}

namespace
{
    // Worker threads for batched rasterization (shared by every CPU backend).
    // GT3_GS_THREADS=<n> sets the thread count including the submitting thread.
    class RasterPool
    {
    public:
        static RasterPool &instance()
        {
            static RasterPool pool;
            return pool;
        }

        unsigned threads() const { return static_cast<unsigned>(m_workers.size()) + 1u; }

        void run(unsigned tasks, const std::function<void(unsigned)> &fn)
        {
            if (m_workers.empty() || tasks <= 1u)
            {
                for (unsigned i = 0; i < tasks; ++i)
                    fn(i);
                return;
            }
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                m_fn = &fn;
                m_tasks = tasks;
                m_next.store(0u, std::memory_order_relaxed);
                m_busy = static_cast<unsigned>(m_workers.size());
                ++m_generation;
            }
            m_wake.notify_all();
            work();
            std::unique_lock<std::mutex> lock(m_mutex);
            m_done.wait(lock, [this] { return m_busy == 0u; });
            m_fn = nullptr;
        }

    private:
        RasterPool()
        {
            unsigned count = 0u;
            if (const char *v = std::getenv("GT3_GS_THREADS"); v && *v)
                count = static_cast<unsigned>(std::strtoul(v, nullptr, 10));
            else
            {
                const unsigned hw = std::max(1u, std::thread::hardware_concurrency());
                count = hw > 4u ? std::min(hw - 3u, 12u) : 1u;
            }
            count = std::max(1u, std::min(count, 64u));
            for (unsigned i = 1; i < count; ++i)
                m_workers.emplace_back([this] { loop(); });
        }

        ~RasterPool()
        {
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                m_stop = true;
            }
            m_wake.notify_all();
            for (std::thread &worker : m_workers)
                if (worker.joinable())
                    worker.join();
        }

        void work()
        {
            for (;;)
            {
                const unsigned task = m_next.fetch_add(1u, std::memory_order_relaxed);
                if (task >= m_tasks)
                    return;
                (*m_fn)(task);
            }
        }

        void loop()
        {
            uint64_t seen = 0u;
            std::unique_lock<std::mutex> lock(m_mutex);
            for (;;)
            {
                m_wake.wait(lock, [&] { return m_stop || m_generation != seen; });
                if (m_stop)
                    return;
                seen = m_generation;
                lock.unlock();
                work();
                lock.lock();
                if (--m_busy == 0u)
                    m_done.notify_one();
            }
        }

        std::vector<std::thread> m_workers;
        std::mutex m_mutex;
        std::condition_variable m_wake, m_done;
        const std::function<void(unsigned)> *m_fn = nullptr;
        unsigned m_tasks = 0u;
        std::atomic<unsigned> m_next{0u};
        unsigned m_busy = 0u;
        uint64_t m_generation = 0u;
        bool m_stop = false;
    };

    // Opt-in diagnostic: GT3_GS_BATCH_STATS=1 prints why batches were flushed
    // and how many draws/pixels they held when the process exits.
    struct BatchStats
    {
        bool enabled = [] {
            const char *v = std::getenv("GT3_GS_BATCH_STATS");
            return v && *v == '1';
        }();
        std::atomic<uint64_t> reasons[8]{};
        std::atomic<uint64_t> serialDraws{0}, parallelFlushes{0}, parallelDraws{0}, smallFlushes{0}, smallDraws{0};
        std::atomic<uint64_t> parallelArea{0}, smallArea{0};
        std::atomic<uint64_t> parallelNs{0}, smallNs{0}, serialNs{0};
        std::mutex kindsMutex;
        std::map<std::string, std::pair<uint64_t, uint64_t>> serialKinds; // draws, ns
        ~BatchStats() { report(); }
        void reset()
        {
            for (auto &r : reasons)
                r.store(0u);
            for (auto *c : {&serialDraws, &parallelFlushes, &parallelDraws, &smallFlushes, &smallDraws, &parallelArea, &smallArea,
                            &parallelNs, &smallNs, &serialNs})
                c->store(0u);
            std::lock_guard<std::mutex> lock(kindsMutex);
            serialKinds.clear();
        }
        // Printed every 10 s while enabled (the process may exit without
        // running static destructors).
        void reportPeriodically()
        {
            static auto last = std::chrono::steady_clock::now();
            const auto now = std::chrono::steady_clock::now();
            if (now - last < std::chrono::seconds(10))
                return;
            std::fprintf(stderr, "[gs-batch] --- %.1f s window ---\n", std::chrono::duration<double>(now - last).count());
            last = now;
            report();
            reset();
        }
        void report()
        {
            if (!enabled)
                return;
            static const char *names[8] = {"serial-draw", "reads-pending-writes", "writes-pending-reads", "size", "aliased-target", "clut-source", "external", "other"};
            std::fprintf(stderr, "[gs-batch] parallel flushes %llu (%llu draws, %llu px), small flushes %llu (%llu draws, %llu px), serial draws %llu\n",
                         (unsigned long long)parallelFlushes.load(), (unsigned long long)parallelDraws.load(), (unsigned long long)parallelArea.load(),
                         (unsigned long long)smallFlushes.load(), (unsigned long long)smallDraws.load(), (unsigned long long)smallArea.load(),
                         (unsigned long long)serialDraws.load());
            std::fprintf(stderr, "[gs-batch] time: parallel %.1f ms, small %.1f ms, serial %.1f ms\n", parallelNs.load() / 1e6,
                         smallNs.load() / 1e6, serialNs.load() / 1e6);
            for (const auto &[kind, value] : serialKinds)
                std::fprintf(stderr, "[gs-batch] serial %-70s %6llu draws %8.1f ms\n", kind.c_str(), (unsigned long long)value.first, value.second / 1e6);
            for (int i = 0; i < 8; ++i)
                if (reasons[i].load())
                    std::fprintf(stderr, "[gs-batch] flush reason %-22s %llu\n", names[i], (unsigned long long)reasons[i].load());
        }
    };
    BatchStats g_batchStats;

    std::atomic<int> g_batchingOverride{-1};

    bool batchingEnabled()
    {
        const int forced = g_batchingOverride.load(std::memory_order_relaxed);
        if (forced >= 0)
            return forced != 0;
        static const bool enabled = [] {
            const char *v = std::getenv("GT3_GS_BATCH");
            return !(v && *v == '0');
        }();
        return enabled;
    }

    // Page geometry of a pixel storage format (CT32/24, Z32/24 and the
    // T8H/T4HL/T4HH formats use the 64x32 CT32 page; 16-bit 64x64; PSMT8
    // 128x64; PSMT4 128x128).
    void pageGeometry(uint32_t psm, uint32_t &pageWidth, uint32_t &pageHeight)
    {
        switch (psm & 0x3Fu)
        {
        case GS_PSM_CT16: case GS_PSM_CT16S: case GS_PSM_Z16: case GS_PSM_Z16S:
            pageWidth = 64u; pageHeight = 64u; break;
        case GS_PSM_T8:
            pageWidth = 128u; pageHeight = 64u; break;
        case GS_PSM_T4:
            pageWidth = 128u; pageHeight = 128u; break;
        default:
            pageWidth = 64u; pageHeight = 32u; break;
        }
    }

    // Calls f(page) for every 8 KiB page a rectangle of a buffer touches. A base
    // that is not page aligned (texture/CLUT block addresses) also marks the
    // following page.
    template <typename F>
    void forRectPages(uint32_t basePage, bool straddles, uint32_t bufferWidth, uint32_t psm,
                      int x0, int y0, int x1, int y1, F &&f)
    {
        uint32_t pageWidth = 64u, pageHeight = 32u;
        pageGeometry(psm, pageWidth, pageHeight);
        const uint32_t perRow = std::max(1u, (std::max(bufferWidth, 1u) * 64u) / pageWidth);
        x0 = std::max(x0, 0);
        y0 = std::max(y0, 0);
        x1 = std::min(x1, 2047);
        y1 = std::min(y1, 2047);
        if (x1 < x0 || y1 < y0)
            return;
        const uint32_t col0 = static_cast<uint32_t>(x0) / pageWidth, col1 = static_cast<uint32_t>(x1) / pageWidth;
        for (uint32_t row = static_cast<uint32_t>(y0) / pageHeight; row <= static_cast<uint32_t>(y1) / pageHeight; ++row)
            for (uint32_t col = col0; col <= col1; ++col)
            {
                // A column past the buffer width wraps into the next page row.
                const uint32_t page = basePage + row * perRow + col;
                f(page & 511u);
                if (straddles)
                    f((page + 1u) & 511u);
            }
    }
}

// GT4's race colour (gs_fullcolour.h): a new video mode (GSDrawState::fcEpoch). Full colour lives within one 480p /
// 1080i session: the plane is cleared (a zero word is never another pixel's full colour: F = v << 3 for every
// halfword), so no word of an earlier session can become valid again when other formats' writes in between happen
// to leave a halfword equal to its reduction. The GL renderer reduces its 16-bit targets' texels at the same draw.
void GSCpuBackend::forgetFullColourUnlocked(uint16_t epoch)
{
    m_fcEpoch = epoch;
    if (m_fcPlane.empty())
        return;
    flushPendingUnlocked();
    std::fill(m_fcPlane.begin(), m_fcPlane.end(), 0u);
}

void GSCpuBackend::ForgetFullColour(uint16_t epoch)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    forgetFullColourUnlocked(epoch);
}

void GSCpuBackend::SetBatchingOverride(int enabled)
{
    g_batchingOverride.store(enabled, std::memory_order_relaxed);
}

void GSCpuBackend::Submit(const GSPrimitiveBatch &batch)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_vram || batch.vertexCount == 0u)
        return;
    logDrawDiagnostics(batch);
    // GT4's race colour (gs_fullcolour.h): a new video mode forgets every pixel's full colour (after the raster
    // work queued before it); the plane is made at the first full-colour draw (here, on the submitting thread,
    // before any raster work that could use it).
    if (batch.state.fcEpoch != m_fcEpoch)
        forgetFullColourUnlocked(batch.state.fcEpoch);
    if (batch.state.fc != 0u && m_fcPlane.empty())
        m_fcPlane.assign(gsfc::kPlaneWords, 0u);

    if (g_drawProfile.enabled || !batchingEnabled())
    {
        flushPendingUnlocked();
        const RasterJob job{0, 2047, m_clutBuffer.data()};
        if (!g_drawProfile.enabled)
        {
            DrawPrimitive(batch, job);
            return;
        }
        const auto start = std::chrono::steady_clock::now();
        DrawPrimitive(batch, job);
        const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
        const GSDrawState &state = batch.state;
        char key[160];
        std::snprintf(key, sizeof(key), "prim=%u tme=%u tpsm=%02x abe=%u fge=%u fbp=%03x fpsm=%02x filt=%u z=%u",
                      static_cast<unsigned>(state.prim.type), static_cast<unsigned>(state.prim.tme),
                      state.prim.tme ? static_cast<unsigned>(state.context.tex0.psm) : 0u, static_cast<unsigned>(state.prim.abe),
                      static_cast<unsigned>(state.prim.fge), static_cast<unsigned>(state.context.frame.fbp),
                      static_cast<unsigned>(state.context.frame.psm), state.linearFilter ? 1u : 0u,
                      static_cast<unsigned>((state.context.test >> 16) & 1u));
        std::lock_guard<std::mutex> profileLock(g_drawProfile.mutex);
        auto &entry = g_drawProfile.entries[key];
        ++entry.draws;
        entry.seconds += seconds;
        return;
    }

    // Footprint: window rows, pages written (frame, Z) and read (texture).
    const GSDrawState &state = batch.state;
    const GSContext &ctx = state.context;
    float minX = batch.vertices[0].x, maxX = minX, minY = batch.vertices[0].y, maxY = minY;
    for (uint32_t i = 1; i < batch.vertexCount; ++i)
    {
        minX = std::min(minX, batch.vertices[i].x);
        maxX = std::max(maxX, batch.vertices[i].x);
        minY = std::min(minY, batch.vertices[i].y);
        maxY = std::max(maxY, batch.vertices[i].y);
    }
    const float ofx = static_cast<float>(ctx.xyoffset.ofx) / 16.0f, ofy = static_cast<float>(ctx.xyoffset.ofy) / 16.0f;
    const int x0 = std::max(static_cast<int>(std::floor(minX - ofx)) - 1, static_cast<int>(ctx.scissor.x0));
    const int y0 = std::max(static_cast<int>(std::floor(minY - ofy)) - 1, static_cast<int>(ctx.scissor.y0));
    const int x1 = std::min(static_cast<int>(std::ceil(maxX - ofx)) + 1, static_cast<int>(ctx.scissor.x1));
    const int y1 = std::min(static_cast<int>(std::ceil(maxY - ofy)) + 1, static_cast<int>(ctx.scissor.y1));
    if (x1 < x0 || y1 < y0)
        return; // entirely outside the scissor: draws nothing

    PageSet framePages, zPages, texturePages;
    // FBMSK covering every colour bit of the format: the draw writes no colour.
    const uint32_t colourBits = (ctx.frame.psm == GS_PSM_CT24 || ctx.frame.psm == GS_PSM_Z24) ? 0x00FFFFFFu
                                : (bitsPerPixel(ctx.frame.psm) == 16) ? 0x80F8F8F8u
                                                                     : 0xFFFFFFFFu;
    const bool writesColour = (ctx.frame.fbmsk & colourBits) != colourBits;
    const bool frameIs32 = bitsPerPixel(ctx.frame.psm) != 16;
    const bool frameWritesLow = writesColour && (!frameIs32 || (ctx.frame.fbmsk & 0x00FFFFFFu) != 0x00FFFFFFu);
    const bool frameWritesHigh = writesColour && (!frameIs32 || (ctx.frame.psm != GS_PSM_CT24 && ctx.frame.psm != GS_PSM_Z24 &&
                                                                 (ctx.frame.fbmsk & 0xFF000000u) != 0xFF000000u));
    if (writesColour)
        forRectPages(ctx.frame.fbp, false, ctx.frame.fbw, ctx.frame.psm, x0, y0, x1, y1, [&](uint32_t page) { framePages.set(page); });
    const bool zte = ((ctx.test >> 16) & 1u) != 0u;
    const uint32_t ztst = static_cast<uint32_t>((ctx.test >> 17) & 3u);
    const bool zUsed = zte && (!ctx.zbuf.zmask || ztst >= 2u);
    if (zUsed)
        forRectPages(ctx.zbuf.zbp, false, ctx.frame.fbw, ctx.zbuf.psm, x0, y0, x1, y1, [&](uint32_t page) { zPages.set(page); });
    if (state.prim.tme)
    {
        // Texels the draw can sample: the vertices' texture coordinate range
        // (perspective-correct interpolation with positive Q is a convex
        // combination of the vertices' S/Q, T/Q), one texel wider for bilinear,
        // then the wrap mode. Anything unknown falls back to the whole texture.
        const GSTex0Reg &tex = ctx.tex0;
        const int texW = 1 << std::min<uint32_t>(tex.tw, 10u), texH = 1 << std::min<uint32_t>(tex.th, 10u);
        float minU = 0.0f, maxU = static_cast<float>(texW), minV = 0.0f, maxV = static_cast<float>(texH);
        bool known = true;
        for (uint32_t i = 0; i < batch.vertexCount; ++i)
        {
            const GSVertex &v = batch.vertices[i];
            float u, t;
            if (state.prim.fst)
            {
                u = static_cast<float>(v.u) / 16.0f;
                t = static_cast<float>(v.v) / 16.0f;
            }
            else
            {
                if (!(v.q > 0.0f) || !std::isfinite(v.s) || !std::isfinite(v.t))
                {
                    known = false;
                    break;
                }
                u = v.s / v.q * static_cast<float>(texW);
                t = v.t / v.q * static_cast<float>(texH);
            }
            if (i == 0u)
            {
                minU = maxU = u;
                minV = maxV = t;
            }
            else
            {
                minU = std::min(minU, u);
                maxU = std::max(maxU, u);
                minV = std::min(minV, t);
                maxV = std::max(maxV, t);
            }
        }
        auto range = [&](float lo, float hi, int size, uint32_t mode, uint32_t regionMin, uint32_t regionMax, int &a, int &b) {
            if (mode == 2u) // REGION_CLAMP: every tap is clamped into [MIN, MAX]
            {
                a = static_cast<int>(std::min(regionMin, regionMax));
                b = static_cast<int>(regionMax);
                return;
            }
            if (mode == 3u) // REGION_REPEAT: (coord & MSK) | FIX stays within MSK|FIX
            {
                a = 0;
                b = static_cast<int>((regionMin | regionMax) & 0x3FFu);
                return;
            }
            if (!known || !(hi - lo < 4096.0f))
            {
                a = 0;
                b = size - 1;
                return;
            }
            a = static_cast<int>(std::floor(lo)) - 2;
            b = static_cast<int>(std::ceil(hi)) + 1;
            if (mode == 1u) // CLAMP
            {
                a = std::clamp(a, 0, size - 1);
                b = std::clamp(b, 0, size - 1);
            }
            else if (a < 0 || b >= size) // REPEAT across the edge: the whole texture
            {
                a = 0;
                b = size - 1;
            }
        };
        int u0 = 0, u1 = texW - 1, v0 = 0, v1 = texH - 1;
        range(minU, maxU, texW, static_cast<uint32_t>(ctx.clamp & 3u), static_cast<uint32_t>((ctx.clamp >> 4) & 0x3FFu),
              static_cast<uint32_t>((ctx.clamp >> 14) & 0x3FFu), u0, u1);
        range(minV, maxV, texH, static_cast<uint32_t>((ctx.clamp >> 2) & 3u), static_cast<uint32_t>((ctx.clamp >> 24) & 0x3FFu),
              static_cast<uint32_t>((ctx.clamp >> 34) & 0x3FFu), v0, v1);
        forRectPages(tex.tbp0 >> 5u, (tex.tbp0 & 31u) != 0u, tex.tbw, tex.psm, u0, v0, u1, v1,
                     [&](uint32_t page) { texturePages.set(page); });
        // GS mipmapping: a per-pixel primitive also reads levels mipLo..mipHi
        // (the level-0 texel range scaled to the level's size, then its own
        // size and CLAMP region). Resolved primitives carry their level's TEX0.
        if (state.mipTag == gsmip::kTagPerPixel)
            for (int n = std::max<int>(state.mipLo, 1); n <= static_cast<int>(state.mipHi); ++n)
            {
                const gsmip::Level L = gsmip::level(ctx, n);
                const float su = static_cast<float>(L.w) / static_cast<float>(texW), sv = static_cast<float>(L.h) / static_cast<float>(texH);
                int lu0 = 0, lu1 = L.w - 1, lv0 = 0, lv1 = L.h - 1;
                range(minU * su, maxU * su, L.w, static_cast<uint32_t>(ctx.clamp & 3u), L.minU, L.maxU, lu0, lu1);
                range(minV * sv, maxV * sv, L.h, static_cast<uint32_t>((ctx.clamp >> 2) & 3u), L.minV, L.maxV, lv0, lv1);
                forRectPages(L.tbp >> 5u, (L.tbp & 31u) != 0u, L.tbw, tex.psm, lu0, lv0, lu1, lv1,
                             [&](uint32_t page) { texturePages.set(page); });
            }
    }
    PlaneSets reads, writes, frameWrites, zWrites;
    if (state.prim.tme)
    {
        const uint8_t tpsm = ctx.tex0.psm;
        const bool highOnly = tpsm == GS_PSM_T8H || tpsm == GS_PSM_T4HL || tpsm == GS_PSM_T4HH;
        reads.high = texturePages;
        if (!highOnly)
            reads.low = texturePages;
    }
    if (frameWritesLow)
        frameWrites.low = framePages;
    if (frameWritesHigh)
        frameWrites.high = framePages;
    if (zUsed)
    {
        zWrites.low = zPages;
        if (ctx.zbuf.psm != GS_PSM_Z24)
            zWrites.high = zPages;
    }
    writes = frameWrites;
    writes.merge(zWrites);
    PageSet targetPages = framePages;
    targetPages.merge(zPages);
    // Rasterized alone: a draw that samples what it writes (order within the
    // draw matters across rows), or whose frame and Z buffers alias each other
    // with different layouts (a pixel's colour and depth land in other rows).
    const bool serial = reads.overlaps(writes) || (zUsed && frameWrites.overlaps(zWrites));
    const uint64_t key = static_cast<uint64_t>(ctx.frame.fbp) | (static_cast<uint64_t>(ctx.frame.fbw) << 9) |
                         (static_cast<uint64_t>(ctx.frame.psm) << 15) |
                         (zUsed ? ((static_cast<uint64_t>(ctx.zbuf.zbp) << 21) | (static_cast<uint64_t>(ctx.zbuf.psm) << 30) | (1ull << 36)) : 0ull);

    int reason = -1;
    if (!m_pending.empty())
    {
        if (serial)
            reason = 0;
        else if (reads.overlaps(m_pendingWrites))
            reason = 1;
        else if (writes.overlaps(m_pendingReads))
            reason = 2;
        else if (m_pending.size() >= 8192u)
            reason = 3;
        else
            for (const PendingTarget &target : m_pendingTargets)
                if (target.key != key && target.pages.overlaps(targetPages))
                    reason = 4;
    }
    if (reason >= 0)
    {
        if (g_batchStats.enabled)
            g_batchStats.reasons[reason].fetch_add(1u, std::memory_order_relaxed);
        flushPendingUnlocked();
    }
    if (serial)
    {
        if (g_batchStats.enabled)
            g_batchStats.serialDraws.fetch_add(1u, std::memory_order_relaxed);
        const auto start = std::chrono::steady_clock::now();
        const RasterJob job{0, 2047, m_clutBuffer.data()};
        DrawPrimitive(batch, job);
        if (g_batchStats.enabled)
        {
            const uint64_t ns = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start).count());
            g_batchStats.serialNs.fetch_add(ns, std::memory_order_relaxed);
            char kind[128];
            std::snprintf(kind, sizeof(kind), "prim=%u tme=%u tbp=%04x tpsm=%02x fbp=%03x fpsm=%02x zbp=%03x zUsed=%u fbmsk=%08x",
                          static_cast<unsigned>(state.prim.type), static_cast<unsigned>(state.prim.tme), static_cast<unsigned>(ctx.tex0.tbp0),
                          static_cast<unsigned>(ctx.tex0.psm), static_cast<unsigned>(ctx.frame.fbp), static_cast<unsigned>(ctx.frame.psm),
                          static_cast<unsigned>(ctx.zbuf.zbp), zUsed ? 1u : 0u, static_cast<unsigned>(ctx.frame.fbmsk));
            std::lock_guard<std::mutex> kindsLock(g_batchStats.kindsMutex);
            auto &entry = g_batchStats.serialKinds[kind];
            ++entry.first;
            entry.second += ns;
        }
        return;
    }

    if (m_clutChanged || m_clutVersions.empty())
    {
        m_clutVersions.push_back(m_clutBuffer);
        m_clutChanged = false;
    }
    m_pending.push_back({batch, static_cast<uint32_t>(m_clutVersions.size() - 1u), y0, y1});
    auto target = std::find_if(m_pendingTargets.begin(), m_pendingTargets.end(),
                               [key](const PendingTarget &t) { return t.key == key; });
    if (target == m_pendingTargets.end())
        target = m_pendingTargets.insert(m_pendingTargets.end(), PendingTarget{key, {}});
    target->pages.merge(targetPages);
    m_pendingWrites.merge(writes);
    m_pendingReads.merge(reads);
    m_pendingArea += static_cast<uint64_t>(x1 - x0 + 1) * static_cast<uint64_t>(y1 - y0 + 1);
}

void GSCpuBackend::flushPendingUnlocked()
{
    if (m_pending.empty())
        return;
    int y0 = m_pending.front().y0, y1 = m_pending.front().y1;
    for (const PendingDraw &draw : m_pending)
    {
        y0 = std::min(y0, draw.y0);
        y1 = std::max(y1, draw.y1);
    }
    RasterPool &pool = RasterPool::instance();
    const int rows = y1 - y0 + 1;
    // Small batches (post-processing passes of a few sprites) are not worth
    // waking the workers for.
    const bool small = pool.threads() <= 1u || m_pendingArea < 32768u || rows < 32;
    if (g_batchStats.enabled)
    {
        (small ? g_batchStats.smallFlushes : g_batchStats.parallelFlushes).fetch_add(1u, std::memory_order_relaxed);
        (small ? g_batchStats.smallDraws : g_batchStats.parallelDraws).fetch_add(m_pending.size(), std::memory_order_relaxed);
        (small ? g_batchStats.smallArea : g_batchStats.parallelArea).fetch_add(m_pendingArea, std::memory_order_relaxed);
    }
    const auto flushStart = std::chrono::steady_clock::now();
    if (small)
    {
        for (const PendingDraw &draw : m_pending)
            DrawPrimitive(draw.batch, RasterJob{0, 2047, m_clutVersions[draw.clutVersion].data()});
    }
    else
    {
        // Several bands per thread so uneven rows (the road vs the sky) balance.
        const unsigned bands = static_cast<unsigned>(std::max(1, std::min(rows / 8, static_cast<int>(pool.threads()) * 4)));
        const int bandHeight = (rows + static_cast<int>(bands) - 1) / static_cast<int>(bands);
        const std::function<void(unsigned)> task = [&](unsigned band) {
            const int by0 = y0 + static_cast<int>(band) * bandHeight;
            const int by1 = std::min(y1, by0 + bandHeight - 1);
            for (const PendingDraw &draw : m_pending)
                if (draw.y1 >= by0 && draw.y0 <= by1)
                    DrawPrimitive(draw.batch, RasterJob{by0, by1, m_clutVersions[draw.clutVersion].data()});
        };
        pool.run(bands, task);
    }
    if (g_batchStats.enabled)
        (small ? g_batchStats.smallNs : g_batchStats.parallelNs)
            .fetch_add(static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - flushStart).count()),
                       std::memory_order_relaxed);
    if (g_batchStats.enabled)
        g_batchStats.reportPeriodically();
    m_pending.clear();
    m_pendingTargets.clear();
    m_pendingWrites = {};
    m_pendingReads = {};
    m_pendingArea = 0u;
    m_clutVersions.clear();
    m_clutChanged = true;
}

void GSCpuBackend::Flush()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    flushPendingUnlocked();
}

void GSCpuBackend::TextureFlush()
{
    // CPU texture reads are coherent with local memory. Future cached/GPU
    // backends use this boundary to invalidate texture views.
}

void GSCpuBackend::Sync(GSSyncReason)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    flushPendingUnlocked();
}

uint32_t GSCpuBackend::ReadVram(uint32_t psm, uint32_t base, uint32_t bw, uint32_t x, uint32_t y) const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    const_cast<GSCpuBackend *>(this)->flushPendingUnlocked();
    return ReadVramUnlocked(psm, base, bw, x, y);
}

uint32_t GSCpuBackend::ReadVramUnlocked(uint32_t psm, uint32_t base, uint32_t bw, uint32_t x, uint32_t y) const
{
    if (!m_vram)
        return 0u;
    return m_readVramFuncs[psm & 0x3Fu](m_vram, base, bw, x, y);
}

void GSCpuBackend::WriteVram(uint32_t psm, uint32_t base, uint32_t bw, uint32_t x, uint32_t y, uint32_t value)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    flushPendingUnlocked();
    WriteVramUnlocked(psm, base, bw, x, y, value);
}

void GSCpuBackend::WriteVramUnlocked(uint32_t psm, uint32_t base, uint32_t bw, uint32_t x, uint32_t y, uint32_t value)
{
    if (!m_vram)
        return;
    m_writeVramFuncs[psm & 0x3Fu](m_vram, base, bw, x, y, value);
}

void GSCpuBackend::SnapshotVram(std::vector<uint8_t> &out) const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    const_cast<GSCpuBackend *>(this)->flushPendingUnlocked();
    if (!m_vram || m_vramSize == 0u)
    {
        out.clear();
        return;
    }
    out.resize(m_vramSize);
    std::memcpy(out.data(), m_vram, m_vramSize);
}

GSTransferSnapshot GSCpuBackend::GetTransferSnapshot() const
{
    // Transfer state does not depend on queued draws: no flush (the GS debug
    // history asks for it on every register write).
    std::lock_guard<std::mutex> lock(m_mutex);
    GSTransferSnapshot result = m_transferState;
    result.localToHostPendingBytes = m_localToHostReadPos < m_localToHostBuffer.size()
                                         ? m_localToHostBuffer.size() - m_localToHostReadPos
                                         : 0u;
    return result;
}

void GSCpuBackend::logDrawDiagnostics(const GSPrimitiveBatch &batch)
{
    const GSDrawState &state = batch.state;
    const auto &ctx = state.context;
    (void)ctx;
    PS2_IF_AGRESSIVE_LOGS({
        const uint32_t primitiveIndex = s_debugPrimitiveCount.fetch_add(1u, std::memory_order_relaxed);
        if (primitiveIndex < 64u)
        {
            std::cout << "[gs:prim] idx=" << primitiveIndex
                      << " type=" << static_cast<uint32_t>(state.prim.type)
                      << " tme=" << static_cast<uint32_t>(state.prim.tme)
                      << " abe=" << static_cast<uint32_t>(state.prim.abe)
                      << " fst=" << static_cast<uint32_t>(state.prim.fst)
                      << " ctxt=" << static_cast<uint32_t>(state.prim.ctxt)
                      << " fbp=" << ctx.frame.fbp
                      << " fbw=" << ctx.frame.fbw
                      << " psm=0x" << std::hex << static_cast<uint32_t>(ctx.frame.psm) << std::dec
                      << " tex0=("
                      << "tbp0=" << ctx.tex0.tbp0
                      << " tbw=" << static_cast<uint32_t>(ctx.tex0.tbw)
                      << " psm=0x" << std::hex << static_cast<uint32_t>(ctx.tex0.psm) << std::dec
                      << " tw=" << static_cast<uint32_t>(ctx.tex0.tw)
                      << " th=" << static_cast<uint32_t>(ctx.tex0.th)
                      << " tcc=" << static_cast<uint32_t>(ctx.tex0.tcc)
                      << " tfx=" << static_cast<uint32_t>(ctx.tex0.tfx)
                      << " cbp=" << ctx.tex0.cbp
                      << " cpsm=0x" << std::hex << static_cast<uint32_t>(ctx.tex0.cpsm) << std::dec
                      << " csm=" << static_cast<uint32_t>(ctx.tex0.csm)
                      << " csa=" << static_cast<uint32_t>(ctx.tex0.csa)
                      << ")"
                      << " texclut=("
                      << "cbw=" << static_cast<uint32_t>(state.texclut.cbw)
                      << " cou=" << static_cast<uint32_t>(state.texclut.cou)
                      << " cov=" << state.texclut.cov
                      << ")"
                      << " ofx=" << (ctx.xyoffset.ofx >> 4)
                      << " ofy=" << (ctx.xyoffset.ofy >> 4)
                      << " scissor=(" << ctx.scissor.x0
                      << "," << ctx.scissor.y0
                      << ")-(" << ctx.scissor.x1
                      << "," << ctx.scissor.y1 << ")"
                      << " test=0x" << std::hex << ctx.test
                      << " alpha=0x" << ctx.alpha
                      << std::dec
                      << " v0=(" << batch.vertices[0].x << "," << batch.vertices[0].y << ")"
                      << " uv0=(" << (batch.vertices[0].u >> 4) << "," << (batch.vertices[0].v >> 4) << ")"
                      << " stq0=(" << batch.vertices[0].s << "," << batch.vertices[0].t << "," << batch.vertices[0].q << ")"
                      << " v1=(" << batch.vertices[1].x << "," << batch.vertices[1].y << ")"
                      << " uv1=(" << (batch.vertices[1].u >> 4) << "," << (batch.vertices[1].v >> 4) << ")"
                      << " stq1=(" << batch.vertices[1].s << "," << batch.vertices[1].t << "," << batch.vertices[1].q << ")"
                      << " v2=(" << batch.vertices[2].x << "," << batch.vertices[2].y << ")"
                      << " uv2=(" << (batch.vertices[2].u >> 4) << "," << (batch.vertices[2].v >> 4) << ")"
                      << " stq2=(" << batch.vertices[2].s << "," << batch.vertices[2].t << "," << batch.vertices[2].q << ")"
                      << " rgba0=(" << static_cast<uint32_t>(batch.vertices[0].r) << ","
                      << static_cast<uint32_t>(batch.vertices[0].g) << ","
                      << static_cast<uint32_t>(batch.vertices[0].b) << ","
                      << static_cast<uint32_t>(batch.vertices[0].a) << ")"
                      << " rgba1=(" << static_cast<uint32_t>(batch.vertices[1].r) << ","
                      << static_cast<uint32_t>(batch.vertices[1].g) << ","
                      << static_cast<uint32_t>(batch.vertices[1].b) << ","
                      << static_cast<uint32_t>(batch.vertices[1].a) << ")"
                      << " rgba2=(" << static_cast<uint32_t>(batch.vertices[2].r) << ","
                      << static_cast<uint32_t>(batch.vertices[2].g) << ","
                      << static_cast<uint32_t>(batch.vertices[2].b) << ","
                      << static_cast<uint32_t>(batch.vertices[2].a) << ")"
                      << std::endl;
        }
    });

    PS2_IF_AGRESSIVE_LOGS({
        if ((state.prim.ctxt != 0u || ctx.frame.fbp == 150u) &&
            s_debugContext1PrimitiveCount.fetch_add(1u, std::memory_order_relaxed) < 32u)
        {
            std::cout << "[gs:copy-prim]"
                      << " type=" << static_cast<uint32_t>(state.prim.type)
                      << " tme=" << static_cast<uint32_t>(state.prim.tme)
                      << " abe=" << static_cast<uint32_t>(state.prim.abe)
                      << " fst=" << static_cast<uint32_t>(state.prim.fst)
                      << " ctxt=" << static_cast<uint32_t>(state.prim.ctxt)
                      << " fbp=" << ctx.frame.fbp
                      << " fbw=" << ctx.frame.fbw
                      << " psm=0x" << std::hex << static_cast<uint32_t>(ctx.frame.psm) << std::dec
                      << " tex0=("
                      << "tbp0=" << ctx.tex0.tbp0
                      << " tbw=" << static_cast<uint32_t>(ctx.tex0.tbw)
                      << " psm=0x" << std::hex << static_cast<uint32_t>(ctx.tex0.psm) << std::dec
                      << " tcc=" << static_cast<uint32_t>(ctx.tex0.tcc)
                      << " tfx=" << static_cast<uint32_t>(ctx.tex0.tfx)
                      << " cbp=" << ctx.tex0.cbp
                      << " cpsm=0x" << std::hex << static_cast<uint32_t>(ctx.tex0.cpsm) << std::dec
                      << " csm=" << static_cast<uint32_t>(ctx.tex0.csm)
                      << " csa=" << static_cast<uint32_t>(ctx.tex0.csa)
                      << ")"
                      << " texclut=("
                      << "cbw=" << static_cast<uint32_t>(state.texclut.cbw)
                      << " cou=" << static_cast<uint32_t>(state.texclut.cou)
                      << " cov=" << state.texclut.cov
                      << ")"
                      << " ofx=" << (ctx.xyoffset.ofx >> 4)
                      << " ofy=" << (ctx.xyoffset.ofy >> 4)
                      << " scissor=(" << ctx.scissor.x0
                      << "," << ctx.scissor.y0
                      << ")-(" << ctx.scissor.x1
                      << "," << ctx.scissor.y1 << ")"
                      << " test=0x" << std::hex << ctx.test
                      << " alpha=0x" << ctx.alpha
                      << std::dec << std::endl;
        }
    });

    {
        // Opt-in diagnostic: GT3_GS_DRAWLOG=<max lines> [GT3_GS_DRAWLOG_FBP=<hex fbp>[,...]]
        // [GT3_GS_DRAWLOG_TEST=<hex TEST value>] [GT3_GS_DRAWLOG_ARMED=1: start at the first VRAM snapshot]
        // logs draws (to stderr) whose frame page matches the filter.
        static const long drawLogMax = [] {
            const char *v = std::getenv("GT3_GS_DRAWLOG");
            return (v && *v) ? std::strtol(v, nullptr, 10) : 0L;
        }();
        static const std::vector<uint32_t> drawLogFbp = [] {
            std::vector<uint32_t> out;
            const char *v = std::getenv("GT3_GS_DRAWLOG_FBP");
            for (const char *p = v ? v : ""; *p;)
            {
                char *end = nullptr;
                out.push_back(static_cast<uint32_t>(std::strtoul(p, &end, 16)));
                if (end == p) break;
                p = (*end == ',') ? end + 1 : end;
            }
            return out;
        }();
        static const long long drawLogTest = [] {
            const char *v = std::getenv("GT3_GS_DRAWLOG_TEST");
            return (v && *v) ? static_cast<long long>(std::strtoull(v, nullptr, 16)) : -1LL;
        }();
        static const bool drawLogArmed = [] {
            const char *v = std::getenv("GT3_GS_DRAWLOG_ARMED");
            return v && *v == '1';
        }();
        static long drawLogCount = 0;
        if (drawLogCount < drawLogMax && (!drawLogArmed || g_gsDiagSnapshotTaken.load(std::memory_order_relaxed)) &&
            (drawLogTest < 0 || static_cast<long long>(ctx.test) == drawLogTest) &&
            (drawLogFbp.empty() || std::find(drawLogFbp.begin(), drawLogFbp.end(), ctx.frame.fbp) != drawLogFbp.end()))
        {
            ++drawLogCount;
            std::cerr << std::hex << "[gs-draw] n=" << drawLogCount << " prim=" << static_cast<uint32_t>(state.prim.type)
                      << " tme=" << static_cast<uint32_t>(state.prim.tme) << " abe=" << static_cast<uint32_t>(state.prim.abe)
                      << " ctxt=" << static_cast<uint32_t>(state.prim.ctxt) << " fbp=" << ctx.frame.fbp
                      << " fbw=" << ctx.frame.fbw << " fpsm=" << static_cast<uint32_t>(ctx.frame.psm)
                      << " fbmsk=" << ctx.frame.fbmsk << " tbp=" << ctx.tex0.tbp0 << " tbw=" << static_cast<uint32_t>(ctx.tex0.tbw)
                      << " tpsm=" << static_cast<uint32_t>(ctx.tex0.psm) << " tfx=" << static_cast<uint32_t>(ctx.tex0.tfx)
                      << " tw=" << static_cast<uint32_t>(ctx.tex0.tw) << " th=" << static_cast<uint32_t>(ctx.tex0.th)
                      << " cbp=" << ctx.tex0.cbp << " cpsm=" << static_cast<uint32_t>(ctx.tex0.cpsm)
                      << " csm=" << static_cast<uint32_t>(ctx.tex0.csm) << " csa=" << static_cast<uint32_t>(ctx.tex0.csa)
                      << " tex1=" << ctx.tex1 << " fst=" << static_cast<uint32_t>(state.prim.fst)
                      << " clamp=" << ctx.clamp << " zbuf=" << ctx.zbuf.zbp << "/" << static_cast<uint32_t>(ctx.zbuf.psm) << "/" << static_cast<uint32_t>(ctx.zbuf.zmask) << " test=" << ctx.test << " alpha=" << ctx.alpha
                      << std::dec << " ofs=(" << (ctx.xyoffset.ofx >> 4) << "," << (ctx.xyoffset.ofy >> 4) << ")"
                      << " sc=(" << ctx.scissor.x0 << "," << ctx.scissor.y0 << ")-(" << ctx.scissor.x1 << "," << ctx.scissor.y1 << ")"
                      << " v0=(" << batch.vertices[0].x << "," << batch.vertices[0].y << " uv " << batch.vertices[0].u / 16.0 << "," << batch.vertices[0].v / 16.0 << ")"
                      << " v1=(" << batch.vertices[1].x << "," << batch.vertices[1].y << " uv " << batch.vertices[1].u / 16.0 << "," << batch.vertices[1].v / 16.0 << ")"
                      << " stq=" << batch.vertices[0].s << "/" << batch.vertices[0].t << "/" << batch.vertices[0].q << ";" << batch.vertices[1].s << "/" << batch.vertices[1].t << "/" << batch.vertices[1].q << ";" << batch.vertices[2].s << "/" << batch.vertices[2].t << "/" << batch.vertices[2].q
                      << " z=" << batch.vertices[1].z << " rgba=" << static_cast<uint32_t>(batch.vertices[1].r) << "," << static_cast<uint32_t>(batch.vertices[1].g)
                      << "," << static_cast<uint32_t>(batch.vertices[1].b) << "," << static_cast<uint32_t>(batch.vertices[1].a)
                      << std::endl;
        }
    }
}

void GSCpuBackend::DrawPrimitive(const GSPrimitiveBatch &batch, const RasterJob &job)
{
    const GSDrawState &state = batch.state;
    switch (state.prim.type)
    {
    case GS_PRIM_SPRITE:
        DrawSprite(batch, job);
        break;
    case GS_PRIM_TRIANGLE:
    case GS_PRIM_TRISTRIP:
    case GS_PRIM_TRIFAN:
        DrawTriangle(batch, job);
        break;
    case GS_PRIM_LINE:
    case GS_PRIM_LINESTRIP:
        DrawLine(batch, job);
        break;
    case GS_PRIM_POINT:
    {
        const GSVertex &v = batch.vertices[0];
        const auto &ctx = state.context;
        int px = static_cast<int>(v.x) - (ctx.xyoffset.ofx >> 4);
        int py = static_cast<int>(v.y) - (ctx.xyoffset.ofy >> 4);
        if (py >= job.y0 && py <= job.y1)
            WritePixel(state, px, py, static_cast<u32>(v.z), v.r, v.g, v.b, v.a, v.fog);
        break;
    }
    default:
        break;
    }
}

void GSCpuBackend::WritePixel(const GSDrawState &state, int x, int y, int z, uint8_t r, uint8_t g, uint8_t b, uint8_t a, uint8_t fog)
{
    const auto &ctx = state.context;
    if (x < ctx.scissor.x0 || x > ctx.scissor.x1 || y < ctx.scissor.y0 || y > ctx.scissor.y1)
        return;

    if (state.prim.fge)
    {
        const uint32_t inverseFog = 255u - fog;
        auto applyFog = [&](uint8_t input, uint8_t fogColor) -> uint8_t
        {
            return static_cast<uint8_t>(((static_cast<uint32_t>(fog) * input) >> 8) + ((inverseFog * fogColor) >> 8));
        };

        r = applyFog(r, state.fogR);
        g = applyFog(g, state.fogG);
        b = applyFog(b, state.fogB);
    }

    // GT4's race colour (gs_fullcolour.h): a full-colour point or line pixel takes the pipe's full-colour path.
    if (state.fc != 0u && gsfc::frame16(ctx.frame.psm) && fcPlaneOrNull())
    {
        const PixelPipe pipe = makePixelPipe(state, m_vram, m_readVramFuncs.data(), m_writeVramFuncs.data(), fcPlaneOrNull());
        writePixelPipeFc(pipe, x, y, z, r, g, b, a);
        return;
    }

    const u32 fbp = GSInternal::framePageBaseToBlock(ctx.frame.fbp);
    const u32 fbw = std::max<u32>(ctx.frame.fbw, 1u);
    const u32 fpsm = ctx.frame.psm;
    const u32 zbp = GSInternal::framePageBaseToBlock(ctx.zbuf.zbp);
    const u32 zpsm = ctx.zbuf.psm;

    const PixelWriteMask writeMask = classifyAlphaTest(ctx.test, a, static_cast<uint8_t>(fpsm));
    if (!writeMask.writesAnything())
    {
        return;
    }

    const bool depthEnabled = (ctx.test & (1ull << 16)) != 0u;
    // ZTE gates both the comparison and depth writes. GT3 clears the frame
    // with TEST=0; its unused ZTST=NEVER must not reject those color writes.
    const uint32_t ztestMethod = depthEnabled ? static_cast<uint32_t>((ctx.test >> 17) & 3u) : 1u;
    const bool alphaBlendEnabled = state.prim.abe;
    const bool preserveDestinationAlpha = writeMask.writeRgb && !writeMask.writeAlpha && fpsm == GS_PSM_CT32;
    const bool destinationAlphaTestNeedsRead = ((ctx.test >> 14) & 0x1u) != 0u && (fpsm == GS_PSM_CT32 || fpsm == GS_PSM_CT16 || fpsm == GS_PSM_CT16S);

    // small optimization, avoid reading the framebuffer for simple draws
    // TODO: only one address lookup for rmw
    const bool frmw = destinationAlphaTestNeedsRead || (writeMask.writesFramebuffer() && ((ctx.frame.fbmsk != 0) || alphaBlendEnabled || preserveDestinationAlpha));

    u32 rawFramebufferPixel = 0;
    u32 fbrgba = 0;
    if (frmw)
    {
        rawFramebufferPixel = ReadVramUnlocked(fpsm, fbp, fbw, x, y);
        fbrgba = rawFramebufferPixel;

        if (bitsPerPixel(fpsm) == 16)
        {
            fbrgba = Rgba5551ToRgba8888(fbrgba);
        }
        else if (fpsm == GS_PSM_CT24)
        {
            // The GS supplies 0x80 as destination alpha for RGB24 blending.
            fbrgba |= 0x80000000u;
        }
    }

    if (!passesDestinationAlphaTest(ctx.test, static_cast<uint8_t>(fpsm), rawFramebufferPixel))
    {
        return;
    }

    bool zpass = false;
    uint32_t storedZ = 0u;
    const uint32_t zc = std::min(static_cast<uint32_t>(z), zFormatMax(zpsm)); // (Z clamped to the Z format's range)
    switch (ztestMethod)
    {
    case 0:
        zpass = false;
        break;
    case 1:
        zpass = true;
        break;
    case 2:
        storedZ = ReadVramUnlocked(zpsm, zbp, fbw, x, y);
        zpass = zc >= storedZ;
        break;
    case 3:
        storedZ = ReadVramUnlocked(zpsm, zbp, fbw, x, y);
        zpass = zc > storedZ;
        break;
    }

    if (!zpass)
    {
        return;
    }

    if (writeMask.writesFramebuffer())
    {
        const u8 srcR = r;
        const u8 srcG = g;
        const u8 srcB = b;

        if (state.prim.abe)
        {
            uint8_t dr = fbrgba & 0xFF;
            uint8_t dg = (fbrgba >> 8) & 0xFF;
            uint8_t db = (fbrgba >> 16) & 0xFF;
            uint8_t da = (fbrgba >> 24) & 0xFF;

            // PABE disables alpha blending when the source alpha MSB is clear.
            if (!(state.pabe && (a & 0x80u) == 0u))
            {
                uint64_t alphaReg = ctx.alpha;
                uint8_t asel = alphaReg & 3;
                uint8_t bsel = (alphaReg >> 2) & 3;
                uint8_t csel = (alphaReg >> 4) & 3;
                uint8_t dsel = (alphaReg >> 6) & 3;
                uint8_t fix = static_cast<uint8_t>((alphaReg >> 32) & 0xFF);

                auto pickRGB = [&](uint8_t sel, int cs, int cd) -> int
                {
                    if (sel == 0)
                        return cs;
                    if (sel == 1)
                        return cd;
                    return 0;
                };
                int cAlpha = (csel == 0) ? a : (csel == 1) ? da
                                                           : fix;

                if (gsdither::applies(state))
                {
                    // GS dithering (gs_dither.h): the unclamped blend result plus DIMX, then COLCLAMP.
                    const int d = gsdither::offset(state.dimx, x, y);
                    const bool clamp = (state.colclamp & 1u) != 0u;
                    r = gsdither::channel(((pickRGB(asel, r, dr) - pickRGB(bsel, r, dr)) * cAlpha >> 7) + pickRGB(dsel, r, dr), d, clamp);
                    g = gsdither::channel(((pickRGB(asel, g, dg) - pickRGB(bsel, g, dg)) * cAlpha >> 7) + pickRGB(dsel, g, dg), d, clamp);
                    b = gsdither::channel(((pickRGB(asel, b, db) - pickRGB(bsel, b, db)) * cAlpha >> 7) + pickRGB(dsel, b, db), d, clamp);
                }
                else
                {
                    r = clampU8(((pickRGB(asel, r, dr) - pickRGB(bsel, r, dr)) * cAlpha >> 7) + pickRGB(dsel, r, dr));
                    g = clampU8(((pickRGB(asel, g, dg) - pickRGB(bsel, g, dg)) * cAlpha >> 7) + pickRGB(dsel, g, dg));
                    b = clampU8(((pickRGB(asel, b, db) - pickRGB(bsel, b, db)) * cAlpha >> 7) + pickRGB(dsel, b, db));
                }
            }
            else
            {
                r = srcR;
                g = srcG;
                b = srcB;
                if (gsdither::applies(state))
                {
                    const int d = gsdither::offset(state.dimx, x, y);
                    const bool clamp = (state.colclamp & 1u) != 0u;
                    r = gsdither::channel(r, d, clamp);
                    g = gsdither::channel(g, d, clamp);
                    b = gsdither::channel(b, d, clamp);
                }
            }
        }
        else if (gsdither::applies(state))
        {
            // Not blending: the colour plus DIMX, then COLCLAMP.
            const int d = gsdither::offset(state.dimx, x, y);
            const bool clamp = (state.colclamp & 1u) != 0u;
            r = gsdither::channel(r, d, clamp);
            g = gsdither::channel(g, d, clamp);
            b = gsdither::channel(b, d, clamp);
        }

        if (writeMask.writeAlpha && (ctx.fba & 0x1ull) != 0ull && ctx.frame.psm != GS_PSM_CT24)
        {
            a = static_cast<uint8_t>(a | 0x80u);
        }

        u32 pixel = pack32(r, g, b, a);

        if (ctx.frame.fbmsk != 0)
        {
            pixel = (pixel & ~ctx.frame.fbmsk) | (fbrgba & ctx.frame.fbmsk);
        }

        if (preserveDestinationAlpha)
        {
            pixel = (pixel & 0x00FFFFFFu) | (fbrgba & 0xFF000000u);
        }

        // format conversion
        if (bitsPerPixel(fpsm) == 16)
        {
            pixel = Rgba8888ToRgba5551(pixel);
        }

        WriteVramUnlocked(fpsm, fbp, fbw, x, y, pixel);
        if (gsfc::frame16(fpsm) && fcPlaneOrNull())
            fcPlaneOrNull()[gsfc::halfIndex(fpsm, fbp, fbw, static_cast<uint32_t>(x), static_cast<uint32_t>(y))] = gsfc::expand(pixel);
    }

    if (depthEnabled && writeMask.writeDepth && !ctx.zbuf.zmask)
    {
        WriteVramUnlocked(zpsm, zbp, fbw, x, y, zc);
    }
}

namespace
{
    // A row run of pixels (x .. x + n - 1 on row y) of a buffer: each pixel's address (PixelStorageTraits'
    // Address), a page at a time: one page id and one row of the page table per page instead of the
    // divisions and lookups per pixel (UploadImage's writes were ~9% of the GS renderer thread at Trial
    // Mountain). fn(i, pixelAddress).
    template <GSMem::PixelStorageMode M, typename Fn>
    inline void forRowPixelAddresses(const typename GSMem::PixelStorageTraits<M>::PageLookupTableT &table, uint32_t dbp, uint32_t dbw,
                                     uint32_t x, uint32_t y, uint32_t n, Fn &&fn)
    {
        using T = GSMem::PixelStorageTraits<M>;
        constexpr auto pe = T::PageExtent();
        const auto &rowTable = table[dbp % T::BlocksPerPage()][y % pe.y];
        uint32_t i = 0;
        while (i < n)
        {
            const uint32_t xx = x + i;
            const uint32_t inPage = xx % pe.x;
            const uint32_t seg = std::min<uint32_t>(n - i, pe.x - inPage);
            const uint32_t base = static_cast<uint32_t>(T::PageId(dbp, dbw, xx, y) * T::PixelsPerPage());
            for (uint32_t j = 0; j < seg; ++j)
                fn(i + j, base + rowTable[inPage + j]);
            i += seg;
        }
    }

    // GT4_UPLOAD_SELFTEST=1 (tests): once, the row addresses checked against Address for many buffers and rows.
    void uploadRowSelfTest()
    {
        static bool done = [] {
            const char *v = std::getenv("GT4_UPLOAD_SELFTEST");
            return !(v && *v == '1'); // (read once: this runs before every upload)
        }();
        if (done)
            return;
        done = true;
        using namespace GSMem;
        uint64_t checked = 0, wrong = 0;
        uint32_t seed = 12345u;
        auto rnd = [&seed](uint32_t m) { seed = seed * 1664525u + 1013904223u; return (seed >> 8) % m; };
        for (int k = 0; k < 20000; ++k)
        {
            const uint32_t dbp = rnd(0x4000u), dbw = 1u + rnd(16u), x = rnd(1024u), y = rnd(1024u), n = 1u + rnd(300u);
            forRowPixelAddresses<C32>(PageTableC32, dbp, dbw, x, y, n, [&](uint32_t i, uint32_t a) {
                ++checked;
                wrong += a != PixelStorageTraits<C32>::Address(PageTableC32, dbp, dbw, x + i, y);
            });
            forRowPixelAddresses<Z32>(PageTableZ32, dbp, dbw, x, y, n, [&](uint32_t i, uint32_t a) {
                ++checked;
                wrong += a != PixelStorageTraits<Z32>::Address(PageTableZ32, dbp, dbw, x + i, y);
            });
            const uint32_t bw8 = (dbw + 1u) & ~1u;
            forRowPixelAddresses<P8>(PageTableP8, dbp, bw8, x, y, n, [&](uint32_t i, uint32_t a) {
                ++checked;
                wrong += a != PixelStorageTraits<P8>::Address(PageTableP8, dbp, bw8, x + i, y);
            });
            forRowPixelAddresses<P4>(PageTableP4, dbp, bw8, x, y, n, [&](uint32_t i, uint32_t a) {
                ++checked;
                wrong += a != PixelStorageTraits<P4>::Address(PageTableP4, dbp, bw8, x + i, y);
            });
        }
        std::fprintf(stderr, "[gs-upload] row address self-test: %llu pixels, %llu wrong\n", (unsigned long long)checked,
                     (unsigned long long)wrong);
    }
}

void GSCpuBackend::LoadClut(const GSTex0Reg &tex, const GSTexClutReg &texclut)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_vram)
        return;
    if (!m_pending.empty())
    {
        // The CLUT is read from VRAM now; queued draws that write its source
        // must land first (GT3's post chain loads CLUTs from what it just drew).
        PageSet source;
        if (tex.csm == 0u)
            forRectPages(tex.cbp >> 5u, (tex.cbp & 31u) != 0u, 1u, tex.cpsm, 0, 0, 15, 15, [&](uint32_t page) { source.set(page); });
        else
            forRectPages(tex.cbp >> 5u, (tex.cbp & 31u) != 0u, texclut.cbw, tex.cpsm, texclut.cou * 16, texclut.cov,
                         texclut.cou * 16 + 255, texclut.cov, [&](uint32_t page) { source.set(page); });
        if (source.overlaps(m_pendingWrites.low) || source.overlaps(m_pendingWrites.high))
        {
            if (g_batchStats.enabled)
                g_batchStats.reasons[5].fetch_add(1u, std::memory_order_relaxed);
            flushPendingUnlocked();
        }
    }
    m_clutChanged = true;
    const bool fourBit = tex.psm == GS_PSM_T4 || tex.psm == GS_PSM_T4HH || tex.psm == GS_PSM_T4HL;
    const bool eightBit = tex.psm == GS_PSM_T8 || tex.psm == GS_PSM_T8H;
    if (!fourBit && !eightBit)
        return;
    const bool clut16 = tex.cpsm == GS_PSM_CT16 || tex.cpsm == GS_PSM_CT16S;
    const uint32_t count = fourBit ? 16u : 256u;
    if (tex.csm == 0u && !clut16)
    {
        // CSM1, 32-bit entries (the usual load; ~28,000 a second at Trial Mountain): the 16x16 (or 8x2) area a
        // row at a time (forRowPixelAddresses: the same addresses as ReadCT32). Entry j is at position
        // swizzleClutIndexCSM1(j), and that swap of bits 3 and 4 is its own inverse.
        const uint32_t rows = fourBit ? 2u : 16u, cols = fourBit ? 8u : 16u;
        const uint8_t *const vram = m_vram;
        for (uint32_t y = 0; y < rows; ++y)
            forRowPixelAddresses<GSMem::C32>(GSMem::PageTableC32, tex.cbp, 1u, 0u, y, cols, [&](uint32_t x, uint32_t a) {
                uint32_t value;
                std::memcpy(&value, vram + ((a * 4u) & (GSMem::MEMORY_SIZE - 4u)), 4);
                const uint32_t j = swizzleClutIndexCSM1(y * 16u + x);
                const uint32_t entry = (static_cast<uint32_t>(tex.csa & 0x0Fu) * 16u + j) & 0xFFu;
                m_clutBuffer[entry] = static_cast<uint16_t>(value);
                m_clutBuffer[entry + 256u] = static_cast<uint16_t>(value >> 16u);
            });
        return;
    }
    for (uint32_t j = 0; j < count; ++j)
    {
        // CSM1: 16 entries are an 8x2 area, 256 a 16x16 area, with index
        // bits 3 and 4 swapped (the CSM1 layout), in a one-page-wide buffer
        // at CBP. CSM2: a line from TEXCLUT (CBW, COU*16, COV).
        uint32_t x, y, width;
        if (tex.csm == 0u)
        {
            const uint32_t position = swizzleClutIndexCSM1(j);
            x = position & 0x0Fu;
            y = position >> 4u;
            width = 1u;
        }
        else
        {
            x = static_cast<uint32_t>(texclut.cou) * 16u + j;
            y = texclut.cov;
            width = texclut.cbw != 0u ? texclut.cbw : 1u;
        }
        uint32_t value = 0u;
        switch (tex.cpsm)
        {
        case GS_PSM_CT16:
            value = GSMem::ReadCT16(m_vram, tex.cbp, width, x, y);
            break;
        case GS_PSM_CT16S:
            value = GSMem::ReadCT16S(m_vram, tex.cbp, width, x, y);
            break;
        default:
            value = GSMem::ReadCT32(m_vram, tex.cbp, width, x, y);
            break;
        }
        // The load lands at CSA*16 in the buffer (GS CLUT buffer semantics).
        if (clut16)
        {
            m_clutBuffer[(static_cast<uint32_t>(tex.csa) * 16u + j) & 0x1FFu] = static_cast<uint16_t>(value);
        }
        else
        {
            const uint32_t entry = (static_cast<uint32_t>(tex.csa & 0x0Fu) * 16u + j) & 0xFFu;
            m_clutBuffer[entry] = static_cast<uint16_t>(value);
            m_clutBuffer[entry + 256u] = static_cast<uint16_t>(value >> 16u);
        }
    }
}

uint32_t GSCpuBackend::LookupCLUT(const GSDrawState &state,
                                  uint8_t index,
                                  uint8_t cpsm,
                                  uint8_t csa,
                                  uint8_t sourcePsm,
                                  const uint16_t *clut) const
{
    // Sampling reads the CLUT buffer loaded by an earlier TEX0/TEX2 CLD, not
    // VRAM at this TEX0's CBP (GT3 loads palettes with TEX2 CLD=1 and draws
    // with TEX0 CLD=0 and an unrelated CBP). The buffer is the one current when
    // the draw was submitted (draws are rasterized later, in batches).
    const bool fourBit = sourcePsm == GS_PSM_T4 || sourcePsm == GS_PSM_T4HH || sourcePsm == GS_PSM_T4HL;
    const uint32_t texel = fourBit ? (index & 0x0Fu) : index;
    switch (cpsm)
    {
    case GS_PSM_CT16:
    case GS_PSM_CT16S:
        return applyTexa(state.texa, cpsm,
                         Rgba5551ToRgba8888(clut[(static_cast<uint32_t>(csa) * 16u + texel) & 0x1FFu]));
    case GS_PSM_CT32:
    case GS_PSM_CT24:
    {
        const uint32_t entry = (static_cast<uint32_t>(csa & 0x0Fu) * 16u + texel) & 0xFFu;
        const uint32_t value = static_cast<uint32_t>(clut[entry]) |
                               (static_cast<uint32_t>(clut[entry + 256u]) << 16u);
        return applyTexa(state.texa, cpsm, value);
    }
    default:
        break;
    }

    return 0xFFFF00FFu;
}

uint32_t GSCpuBackend::SampleTexture(const GSDrawState &state, float s, float t, float q, uint16_t u, uint16_t v, const RasterJob &job)
{
    const auto &ctx = state.context;
    const auto &tex = ctx.tex0;

    const int texW = state.textureWidth;
    const int texH = state.textureHeight;
    const uint64_t clamp = ctx.clamp;
    const uint8_t wrapU = static_cast<uint8_t>(clamp & 0x3u);
    const uint8_t wrapV = static_cast<uint8_t>((clamp >> 2) & 0x3u);
    const uint16_t minU = static_cast<uint16_t>((clamp >> 4) & 0x3FFu);
    const uint16_t maxU = static_cast<uint16_t>((clamp >> 14) & 0x3FFu);
    const uint16_t minV = static_cast<uint16_t>((clamp >> 24) & 0x3FFu);
    const uint16_t maxV = static_cast<uint16_t>((clamp >> 34) & 0x3FFu);

    float texUf, texVf;
    if (state.prim.fst)
    {
        texUf = static_cast<float>(u) / 16.0f;
        texVf = static_cast<float>(v) / 16.0f;
    }
    else
    {
        const float invQ = 1.0f / fabsQ(q);
        texUf = s * invQ * static_cast<float>(texW);
        texVf = t * invQ * static_cast<float>(texH);
    }

    auto samplePoint = [&](int sampleU, int sampleV) -> uint32_t
    {
        sampleU = wrapTextureCoordinate(sampleU, texW, wrapU, minU, maxU);
        sampleV = wrapTextureCoordinate(sampleV, texH, wrapV, minV, maxV);

        u32 out = ReadVramUnlocked(tex.psm, tex.tbp0, tex.tbw, sampleU, sampleV);

        switch (tex.psm)
        {
        case GS_PSM_CT32:
        case GS_PSM_Z32:
        case GS_PSM_CT24:
        case GS_PSM_Z24:
            return applyTexa(state.texa, tex.psm, out);
        case GS_PSM_CT16:
        case GS_PSM_CT16S:
        case GS_PSM_Z16:
        case GS_PSM_Z16S:
            return applyTexa(state.texa, tex.psm, Rgba5551ToRgba8888(out));
        case GS_PSM_T8:
        case GS_PSM_T8H:
        case GS_PSM_T4:
        case GS_PSM_T4HL:
        case GS_PSM_T4HH:
            return LookupCLUT(state, static_cast<u8>(out), tex.cpsm, tex.csa, tex.psm, job.clut);
        }

        return 0xFFFF00FFu;
    };

    if (!state.linearFilter)
    {
        return samplePoint(static_cast<int>(texUf), static_cast<int>(texVf));
    }

    const float sampleU = texUf - 0.5f;
    const float sampleV = texVf - 0.5f;
    const int u0 = static_cast<int>(std::floor(sampleU));
    const int v0 = static_cast<int>(std::floor(sampleV));
    const int u1 = u0 + 1;
    const int v1 = v0 + 1;
    const float fx = sampleU - static_cast<float>(u0);
    const float fy = sampleV - static_cast<float>(v0);

    const uint32_t c00 = samplePoint(u0, v0);
    const uint32_t c10 = samplePoint(u1, v0);
    const uint32_t c01 = samplePoint(u0, v1);
    const uint32_t c11 = samplePoint(u1, v1);

    const uint8_t r = lerpChannel(static_cast<uint8_t>(c00 & 0xFFu),
                                  static_cast<uint8_t>(c10 & 0xFFu),
                                  static_cast<uint8_t>(c01 & 0xFFu),
                                  static_cast<uint8_t>(c11 & 0xFFu),
                                  fx, fy);
    const uint8_t g = lerpChannel(static_cast<uint8_t>((c00 >> 8) & 0xFFu),
                                  static_cast<uint8_t>((c10 >> 8) & 0xFFu),
                                  static_cast<uint8_t>((c01 >> 8) & 0xFFu),
                                  static_cast<uint8_t>((c11 >> 8) & 0xFFu),
                                  fx, fy);
    const uint8_t b = lerpChannel(static_cast<uint8_t>((c00 >> 16) & 0xFFu),
                                  static_cast<uint8_t>((c10 >> 16) & 0xFFu),
                                  static_cast<uint8_t>((c01 >> 16) & 0xFFu),
                                  static_cast<uint8_t>((c11 >> 16) & 0xFFu),
                                  fx, fy);
    const uint8_t a = lerpChannel(static_cast<uint8_t>((c00 >> 24) & 0xFFu),
                                  static_cast<uint8_t>((c10 >> 24) & 0xFFu),
                                  static_cast<uint8_t>((c01 >> 24) & 0xFFu),
                                  static_cast<uint8_t>((c11 >> 24) & 0xFFu),
                                  fx, fy);

    return static_cast<uint32_t>(r) |
           (static_cast<uint32_t>(g) << 8) |
           (static_cast<uint32_t>(b) << 16) |
           (static_cast<uint32_t>(a) << 24);
}

void GSCpuBackend::DrawSprite(const GSPrimitiveBatch &batch, const RasterJob &job)
{
    const GSDrawState &state = batch.state;
    const GSVertex &v0 = batch.vertices[0];
    const GSVertex &v1 = batch.vertices[1];
    const auto &ctx = state.context;

    // GS sprite rules: vertex XY keep their 1/16 fraction; a pixel (x, y) is
    // covered when X0 <= x < X1 (top-left rule, sample point at the integer
    // pixel coordinate, not its centre), and UV is evaluated there. GT3's
    // colour-correction pass reads the frame buffer as PSMT8 with sprites of
    // 8x16 pixels over 8x64 texels: row y must read texel row 4*y exactly, or
    // it samples another byte (channel) of a neighbouring pixel.
    const float ofx = static_cast<float>(ctx.xyoffset.ofx) / 16.0f;
    const float ofy = static_cast<float>(ctx.xyoffset.ofy) / 16.0f;
    const float X0 = v0.x - ofx;
    const float Y0 = v0.y - ofy;
    const float X1 = v1.x - ofx;
    const float Y1 = v1.y - ofy;
    u32 z1 = static_cast<u32>(v1.z);

    const int pixelX0 = static_cast<int>(std::ceil(std::min(X0, X1)));
    const int pixelX1 = static_cast<int>(std::ceil(std::max(X0, X1))) - 1;
    const int pixelY0 = static_cast<int>(std::ceil(std::min(Y0, Y1)));
    const int pixelY1 = static_cast<int>(std::ceil(std::max(Y0, Y1))) - 1;
    if (pixelX1 < pixelX0 || pixelY1 < pixelY0)
        return;

    // If the sprite rectangle is fully outside scissor, nothing should render.
    if (pixelX1 < ctx.scissor.x0 || pixelX0 > ctx.scissor.x1 ||
        pixelY1 < ctx.scissor.y0 || pixelY0 > ctx.scissor.y1)
        return;

    const int drawX0 = std::max<int>(pixelX0, ctx.scissor.x0);
    const int drawY0 = std::max<int>(std::max<int>(pixelY0, ctx.scissor.y0), job.y0);
    const int drawX1 = std::min<int>(pixelX1, ctx.scissor.x1);
    const int drawY1 = std::min<int>(std::min<int>(pixelY1, ctx.scissor.y1), job.y1);
    if (drawY1 < drawY0)
        return;

    uint8_t r = v1.r, g = v1.g, b = v1.b, a = v1.a;
    const PixelPipe pipe = makePixelPipe(state, m_vram, m_readVramFuncs.data(), m_writeVramFuncs.data(), fcPlaneOrNull());

    if (state.prim.tme)
    {
        const TexPipe texPipe = makeTexPipe(state, m_vram, m_readVramFuncs.data(), job.clut, fcPlaneOrNull());
        static const bool mipTint = gsmip::tintEnabled();
        const auto &tex = ctx.tex0;
        const int texW = state.textureWidth;
        const int texH = state.textureHeight;

        float u0f, v0f, u1f, v1f;
        if (state.prim.fst)
        {
            u0f = static_cast<float>(v0.u) / 16.0f;
            v0f = static_cast<float>(v0.v) / 16.0f;
            u1f = static_cast<float>(v1.u) / 16.0f;
            v1f = static_cast<float>(v1.v) / 16.0f;
        }
        else
        {
            const float q0 = fabsQ(v0.q);
            const float q1 = fabsQ(v1.q);
            u0f = (v0.s / q0) * static_cast<float>(texW);
            v0f = (v0.t / q0) * static_cast<float>(texH);
            u1f = (v1.s / q1) * static_cast<float>(texW);
            v1f = (v1.t / q1) * static_cast<float>(texH);
        }

        const float dudx = (X1 != X0) ? (u1f - u0f) / (X1 - X0) : 0.0f;
        const float dvdy = (Y1 != Y0) ? (v1f - v0f) / (Y1 - Y0) : 0.0f;

        for (int y = drawY0; y <= drawY1; ++y)
        {
            const float texVf = v0f + (static_cast<float>(y) - Y0) * dvdy;

            for (int x = drawX0; x <= drawX1; ++x)
            {
                const float texUf = u0f + (static_cast<float>(x) - X0) * dudx;
                uint32_t texel = 0xFFFF00FFu;
                if (state.prim.fst)
                {
                    const int fixedU = floorToInt(texUf * 16.0f);
                    const int fixedV = floorToInt(texVf * 16.0f);
                    const uint16_t sampleU = static_cast<uint16_t>(clampInt(fixedU, 0, 0xFFFF));
                    const uint16_t sampleV = static_cast<uint16_t>(clampInt(fixedV, 0, 0xFFFF));
                    texel = sampleTexturePipe(texPipe, 0.0f, 0.0f, 1.0f, sampleU, sampleV);
                }
                else
                {
                    texel = sampleTexturePipe(texPipe, texUf / static_cast<float>(texW), texVf / static_cast<float>(texH), 1.0f, 0u, 0u);
                }
                // GS mipmapping: a sprite is always resolved to one level (one
                // LOD); PS2X_GS_MIP_TINT tints it by that level.
                if (texPipe.extraTag != 0u && mipTint)
                    texel = gsmip::tint(texel, static_cast<int>(texPipe.extraTag) - 1);

                uint8_t tr = static_cast<uint8_t>(texel & 0xFF);
                uint8_t tg = static_cast<uint8_t>((texel >> 8) & 0xFF);
                uint8_t tb = static_cast<uint8_t>((texel >> 16) & 0xFF);
                uint8_t ta = static_cast<uint8_t>((texel >> 24) & 0xFF);

                const TextureCombineResult color = combineTexture(tex, r, g, b, a, tr, tg, tb, ta);
                writePixelPipe(pipe, x, y, z1, color.r, color.g, color.b, color.a, v1.fog);
            }
        }
    }
    else
    {
        for (int y = drawY0; y <= drawY1; ++y)
            for (int x = drawX0; x <= drawX1; ++x)
                writePixelPipe(pipe, x, y, z1, r, g, b, a, v1.fog);
    }
}

void GSCpuBackend::DrawTriangle(const GSPrimitiveBatch &batch, const RasterJob &job)
{
    const GSDrawState &state = batch.state;
    const GSVertex &v0 = batch.vertices[0];
    const GSVertex &v1 = batch.vertices[1];
    const GSVertex &v2 = batch.vertices[2];
    const auto &ctx = state.context;

    int ofx = ctx.xyoffset.ofx >> 4;
    int ofy = ctx.xyoffset.ofy >> 4;

    float fx0 = v0.x - static_cast<float>(ofx);
    float fy0 = v0.y - static_cast<float>(ofy);
    float fx1 = v1.x - static_cast<float>(ofx);
    float fy1 = v1.y - static_cast<float>(ofy);
    float fx2 = v2.x - static_cast<float>(ofx);
    float fy2 = v2.y - static_cast<float>(ofy);

    int minX = static_cast<int>(std::floor(std::min(std::min(fx0, fx1), fx2)));
    int maxX = static_cast<int>(std::ceil(std::max(std::max(fx0, fx1), fx2)));
    int minY = static_cast<int>(std::floor(std::min(std::min(fy0, fy1), fy2)));
    int maxY = static_cast<int>(std::ceil(std::max(std::max(fy0, fy1), fy2)));

    minX = clampInt(minX, ctx.scissor.x0, ctx.scissor.x1);
    maxX = clampInt(maxX, ctx.scissor.x0, ctx.scissor.x1);
    minY = clampInt(minY, ctx.scissor.y0, ctx.scissor.y1);
    maxY = clampInt(maxY, ctx.scissor.y0, ctx.scissor.y1);
    minY = std::max(minY, job.y0);
    maxY = std::min(maxY, job.y1);
    if (maxY < minY)
        return;

    float denom = (fy1 - fy2) * (fx0 - fx2) + (fx2 - fx1) * (fy0 - fy2);
    if (std::fabs(denom) < 0.001f)
        return;

    // Edge functions in double: vertex positions are 12.4 fixed point and the
    // sample points are at +0.5, so every product and sum here is exact.
    // A pixel exactly on an edge belongs to only one of the two triangles
    // sharing that edge (tie-break on the edge gradient, a top-left style
    // fill rule, as the GS does). Covering it twice doubled alpha blending
    // along shared diagonals, e.g. dark dots on the movie's full-screen quad.
    const double X0 = fx0, Y0 = fy0, X1 = fx1, Y1 = fy1, X2 = fx2, Y2 = fy2;
    const double wind = (denom < 0.0f) ? -1.0 : 1.0;
    // E_i(px,py) = A_i*px + B_i*py + C_i, positive inside (after winding).
    const double A0 = (Y1 - Y2) * wind, B0 = (X2 - X1) * wind, C0 = -(A0 * X2 + B0 * Y2);
    const double A1 = (Y2 - Y0) * wind, B1 = (X0 - X2) * wind, C1 = -(A1 * X2 + B1 * Y2);
    const double A2 = (Y0 - Y1) * wind, B2 = (X1 - X0) * wind, C2 = -(A2 * X0 + B2 * Y0);
    auto ownsEdge = [](double a, double b) { return a > 0.0 || (a == 0.0 && b > 0.0); };
    const bool own0 = ownsEdge(A0, B0), own1 = ownsEdge(A1, B1), own2 = ownsEdge(A2, B2);
    const double area = A0 * X0 + B0 * Y0 + C0; // == |denom|
    if (!(area > 0.0))
        return;
    const double invArea = 1.0 / area;

    const PixelPipe pipe = makePixelPipe(state, m_vram, m_readVramFuncs.data(), m_writeVramFuncs.data(), fcPlaneOrNull());
    TexPipe texPipe;
    if (state.prim.tme)
        texPipe = makeTexPipe(state, m_vram, m_readVramFuncs.data(), job.clut, fcPlaneOrNull());
    // GS mipmapping: a per-pixel primitive (texPipe.mip) samples its levels per
    // pixel; a resolved one (texPipe.extraTag) is tinted / checked.
    static const bool mipTint = gsmip::tintEnabled();
    const bool mipCount = state.prim.tme && (texPipe.mip || texPipe.extraTag != 0u) && gsmip::statsOn();
    MipPixelCounts mipCounts;
    // Vertex colours and S/T/Q/fog as float lanes: each lane is interpolated
    // with the same float operations, in the same order, as the scalar form
    // v0.x * w0 + v1.x * w1 + v2.x * w2.
    const __m128 rgba0 = _mm_setr_ps(v0.r, v0.g, v0.b, v0.a);
    const __m128 rgba1 = _mm_setr_ps(v1.r, v1.g, v1.b, v1.a);
    const __m128 rgba2 = _mm_setr_ps(v2.r, v2.g, v2.b, v2.a);
    const __m128 stqf0 = _mm_setr_ps(v0.s, v0.t, v0.q, v0.fog);
    const __m128 stqf1 = _mm_setr_ps(v1.s, v1.t, v1.q, v1.fog);
    const __m128 stqf2 = _mm_setr_ps(v2.s, v2.t, v2.q, v2.fog);

    for (int y = minY; y <= maxY; ++y)
    {
        const double py = static_cast<double>(y) + 0.5;
        const double px0 = static_cast<double>(minX) + 0.5;
        // Stepping by A_i per pixel gives the same values as evaluating each
        // pixel: every term is a small multiple of 1/256, exact in double.
        double e0 = A0 * px0 + B0 * py + C0;
        double e1 = A1 * px0 + B1 * py + C1;
        double e2 = A2 * px0 + B2 * py + C2;
        for (int x = minX; x <= maxX; ++x, e0 += A0, e1 += A1, e2 += A2)
        {
            if (e0 < 0.0 || e1 < 0.0 || e2 < 0.0)
                continue;
            if ((e0 == 0.0 && !own0) || (e1 == 0.0 && !own1) || (e2 == 0.0 && !own2))
                continue;

            const float w0 = static_cast<float>(e0 * invArea);
            const float w1 = static_cast<float>(e1 * invArea);
            const float w2 = static_cast<float>(e2 * invArea);

            double z = v0.z * w0 + v1.z * w1 + v2.z * w2;
            const __m128 vw0 = _mm_set1_ps(w0), vw1 = _mm_set1_ps(w1), vw2 = _mm_set1_ps(w2);
            // s, t, q and fog for this pixel (lanes 0..3)
            const __m128 stqf = _mm_add_ps(_mm_add_ps(_mm_mul_ps(stqf0, vw0), _mm_mul_ps(stqf1, vw1)), _mm_mul_ps(stqf2, vw2));

            uint8_t r, g, b, a;
            if (state.prim.iip)
            {
                // clampU8(static_cast<int>(...)) per lane: truncate, then saturate to 0..255
                const __m128 shade = _mm_add_ps(_mm_add_ps(_mm_mul_ps(rgba0, vw0), _mm_mul_ps(rgba1, vw1)), _mm_mul_ps(rgba2, vw2));
                const __m128i shadeI = _mm_cvttps_epi32(shade);
                const __m128i shade16 = _mm_packs_epi32(shadeI, shadeI);
                const uint32_t packed = static_cast<uint32_t>(_mm_cvtsi128_si32(_mm_packus_epi16(shade16, shade16)));
                r = static_cast<uint8_t>(packed);
                g = static_cast<uint8_t>(packed >> 8);
                b = static_cast<uint8_t>(packed >> 16);
                a = static_cast<uint8_t>(packed >> 24);
            }
            else
            {
                r = v2.r;
                g = v2.g;
                b = v2.b;
                a = v2.a;
            }

            if (state.prim.tme)
            {
                float is, it, iq;
                uint16_t iu, iv;
                if (state.prim.fst)
                {
                    iu = static_cast<uint16_t>(v0.u * w0 + v1.u * w1 + v2.u * w2);
                    iv = static_cast<uint16_t>(v0.v * w0 + v1.v * w1 + v2.v * w2);
                    is = 0.0f;
                    it = 0.0f;
                    iq = 1.0f;
                }
                else
                {
                    // The GS DDA interpolates the homogeneous S, T and Q
                    // values. Texel coordinates are calculated from S/Q and
                    // T/Q only after interpolation.
                    alignas(16) float lanes[4];
                    _mm_store_ps(lanes, stqf);
                    is = lanes[0];
                    it = lanes[1];
                    iq = lanes[2];
                    iu = 0;
                    iv = 0;
                }

                uint32_t texel;
                if (texPipe.mip)
                    texel = sampleTextureMipPipe(texPipe, is, it, iq, mipCount ? &mipCounts : nullptr, mipTint);
                else
                {
                    texel = sampleTexturePipe(texPipe, is, it, iq, iu, iv);
                    if (texPipe.extraTag != 0u)
                        texel = mipExtraPipe(texPipe, texel, iq, !state.prim.fst, mipCount ? &mipCounts : nullptr);
                }

                uint8_t tr = static_cast<uint8_t>(texel & 0xFF);
                uint8_t tg = static_cast<uint8_t>((texel >> 8) & 0xFF);
                uint8_t tb = static_cast<uint8_t>((texel >> 16) & 0xFF);
                uint8_t ta = static_cast<uint8_t>((texel >> 24) & 0xFF);

                const auto &tex = ctx.tex0;
                const uint8_t shadeR = r;
                const uint8_t shadeG = g;
                const uint8_t shadeB = b;
                const uint8_t shadeA = a;
                const TextureCombineResult color = combineTexture(tex, shadeR, shadeG, shadeB, shadeA, tr, tg, tb, ta);

                r = color.r;
                g = color.g;
                b = color.b;
                a = color.a;
            }

            const uint8_t fog = clampU8(static_cast<int>(_mm_cvtss_f32(_mm_shuffle_ps(stqf, stqf, _MM_SHUFFLE(3, 3, 3, 3)))));
            writePixelPipe(pipe, x, y, static_cast<u32>(z + 0.5), r, g, b, a, fog);
        }
    }
    if (mipCount)
        mipCounts.flush();
}

void GSCpuBackend::DrawLine(const GSPrimitiveBatch &batch, const RasterJob &job)
{
    const GSDrawState &state = batch.state;
    const GSVertex &v0 = batch.vertices[0];
    const GSVertex &v1 = batch.vertices[1];
    const auto &ctx = state.context;

    int ofx = ctx.xyoffset.ofx >> 4;
    int ofy = ctx.xyoffset.ofy >> 4;

    int x0 = static_cast<int>(v0.x) - ofx;
    int y0 = static_cast<int>(v0.y) - ofy;
    int x1 = static_cast<int>(v1.x) - ofx;
    int y1 = static_cast<int>(v1.y) - ofy;

    int dx = std::abs(x1 - x0);
    int dy = -std::abs(y1 - y0);
    int sx = (x0 < x1) ? 1 : -1;
    int sy = (y0 < y1) ? 1 : -1;
    int err = dx + dy;

    int totalSteps = std::max(std::abs(x1 - x0), std::abs(y1 - y0));
    if (totalSteps == 0)
        totalSteps = 1;
    int step = 0;

    for (;;)
    {
        float t = static_cast<float>(step) / static_cast<float>(totalSteps);
        uint8_t r, g, b, a;
        if (state.prim.iip)
        {
            r = clampU8(static_cast<int>(v0.r + (v1.r - v0.r) * t));
            g = clampU8(static_cast<int>(v0.g + (v1.g - v0.g) * t));
            b = clampU8(static_cast<int>(v0.b + (v1.b - v0.b) * t));
            a = clampU8(static_cast<int>(v0.a + (v1.a - v0.a) * t));
        }
        else
        {
            r = v1.r;
            g = v1.g;
            b = v1.b;
            a = v1.a;
        }

        double z = (v0.z + (v1.z - v0.z) * t);
        const uint8_t fog = clampU8(static_cast<int>(v0.fog + (v1.fog - v0.fog) * t));
        if (y0 >= job.y0 && y0 <= job.y1)
            WritePixel(state, x0, y0, static_cast<u32>(z), r, g, b, a, fog);

        if (x0 == x1 && y0 == y1)
            break;

        int e2 = 2 * err;
        if (e2 >= dy)
        {
            err += dy;
            x0 += sx;
        }
        if (e2 <= dx)
        {
            err += dx;
            y0 += sy;
        }
        ++step;
    }
}

void GSCpuBackend::BeginTransfer(const GSTransferCommand &command)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    flushPendingUnlocked();
    m_transfer = command;
    m_transferState.x = command.trxpos.dsax;
    m_transferState.y = command.trxpos.dsay;
    m_transferState.totalPixels = static_cast<uint32_t>(command.trxreg.rrw) * static_cast<uint32_t>(command.trxreg.rrh);
    m_transferState.copiedPixels = 0u;
    m_transferState.direction = command.direction;
    m_transferState.localToHostPendingBytes = 0u;

    if (command.direction == 2u)
        PerformLocalToLocalTransfer();
    else if (command.direction == 1u)
        PerformLocalToHostTransfer();
}

void GSCpuBackend::UploadImage(const uint8_t *data, uint32_t sizeBytes)
{
    uploadRowSelfTest();
    std::lock_guard<std::mutex> lock(m_mutex);
    flushPendingUnlocked();
    if (!data || sizeBytes == 0u || !m_vram || m_transferState.direction != 0u)
        return;
    if (m_transfer.trxreg.rrw == 0u || m_transfer.trxreg.rrh == 0u || m_transferState.totalPixels == 0u)
        return;

    const uint32_t dbp = m_transfer.bitbltbuf.dbp;
    const uint32_t dbw = std::max<uint32_t>(m_transfer.bitbltbuf.dbw, 1u);
    const uint8_t dpsm = m_transfer.bitbltbuf.dpsm;
    const uint32_t rrw = m_transfer.trxreg.rrw;
    const uint32_t dsax = m_transfer.trxpos.dsax;
    uint32_t offset = 0u;

    auto advancePixel = [&](uint32_t count)
    {
        const uint32_t totalPixels = m_transferState.totalPixels;
        m_transferState.copiedPixels =
            std::min<uint32_t>(totalPixels, m_transferState.copiedPixels + count);

        if (m_transferState.copiedPixels >= totalPixels)
        {
            m_transferState.direction = 3u;
            m_transferState.totalPixels = 0u;
            return;
        }

        m_transferState.x = dsax + (m_transferState.copiedPixels % rrw);
        m_transferState.y = m_transfer.trxpos.dsay + (m_transferState.copiedPixels / rrw);
    };

    // Common formats a row run at a time: the inline page-table writers and one
    // position update per run (the loop below does one per pixel, with a
    // division). Odd T4 positions and every other format go through the loop.
    if (dpsm == GS_PSM_CT32 || dpsm == GS_PSM_Z32 || dpsm == GS_PSM_T8 || (dpsm == GS_PSM_T4 && (rrw & 1u) == 0u))
    {
        using namespace GSMem;
        const uint32_t dsay = m_transfer.trxpos.dsay;
        while (offset < sizeBytes && m_transferState.direction == 0u)
        {
            const uint32_t copied = m_transferState.copiedPixels;
            const uint32_t col = copied % rrw, row = copied / rrw;
            if (dpsm == GS_PSM_T4 && (col & 1u) != 0u)
                break;
            const uint32_t bytesPerPixel = dpsm == GS_PSM_T8 ? 1u : 4u;
            uint32_t run = std::min(rrw - col, m_transferState.totalPixels - copied);
            if (dpsm == GS_PSM_T4)
                run = std::min(run, (sizeBytes - offset) * 2u) & ~1u;
            else
                run = std::min(run, (sizeBytes - offset) / bytesPerPixel);
            if (run == 0u)
                break;
            const uint32_t x = dsax + col, y = dsay + row;
            const uint8_t *src = data + offset;
            // (As PixelStorageTraits::Write: the byte address masked into the 4 MB, a 4-bit pixel read-modify-written.)
            uint8_t *const vram = m_vram;
            switch (dpsm)
            {
            case GS_PSM_CT32:
                forRowPixelAddresses<C32>(PageTableC32, dbp, dbw, x, y, run, [&](uint32_t i, uint32_t a) {
                    std::memcpy(vram + ((a * 4u) & (MEMORY_SIZE - 4u)), src + i * 4u, 4);
                });
                offset += run * 4u;
                break;
            case GS_PSM_Z32:
                forRowPixelAddresses<Z32>(PageTableZ32, dbp, dbw, x, y, run, [&](uint32_t i, uint32_t a) {
                    std::memcpy(vram + ((a * 4u) & (MEMORY_SIZE - 4u)), src + i * 4u, 4);
                });
                offset += run * 4u;
                break;
            case GS_PSM_T8:
                forRowPixelAddresses<P8>(PageTableP8, dbp, dbw, x, y, run, [&](uint32_t i, uint32_t a) {
                    vram[a & (MEMORY_SIZE - 1u)] = src[i];
                });
                offset += run;
                break;
            default: // T4: two pixels a byte, low nibble first
                forRowPixelAddresses<P4>(PageTableP4, dbp, dbw, x, y, run, [&](uint32_t i, uint32_t a) {
                    const uint32_t bits = a * 4u;
                    uint8_t &b = vram[(bits / 8u) & (MEMORY_SIZE - 1u)];
                    const uint8_t nibble = (i & 1u) ? static_cast<uint8_t>((src[i / 2u] >> 4u) & 0x0Fu) : static_cast<uint8_t>(src[i / 2u] & 0x0Fu);
                    const uint32_t shift = bits % 8u;
                    b = static_cast<uint8_t>((b & ~(0x0Fu << shift)) | (nibble << shift));
                });
                offset += run / 2u;
                break;
            }
            advancePixel(run);
        }
    }

    while (offset < sizeBytes && m_transferState.direction == 0u)
    {
        switch (dpsm)
        {
        case GS_PSM_CT32:
        case GS_PSM_Z32:
        {
            if (sizeBytes - offset < 4u)
                return;
            uint32_t value = 0u;
            std::memcpy(&value, data + offset, sizeof(value));
            WriteVramUnlocked(dpsm, dbp, dbw, m_transferState.x, m_transferState.y, value);
            offset += 4u;
            advancePixel(1u);
            break;
        }
        case GS_PSM_CT24:
        case GS_PSM_Z24:
        {
            if (sizeBytes - offset < 3u)
                return;
            const uint32_t value = static_cast<uint32_t>(data[offset]) |
                                   (static_cast<uint32_t>(data[offset + 1u]) << 8u) |
                                   (static_cast<uint32_t>(data[offset + 2u]) << 16u);
            WriteVramUnlocked(dpsm, dbp, dbw, m_transferState.x, m_transferState.y, value);
            offset += 3u;
            advancePixel(1u);
            break;
        }
        case GS_PSM_CT16:
        case GS_PSM_CT16S:
        case GS_PSM_Z16:
        case GS_PSM_Z16S:
        {
            if (sizeBytes - offset < 2u)
                return;
            uint16_t value = 0u;
            std::memcpy(&value, data + offset, sizeof(value));
            WriteVramUnlocked(dpsm, dbp, dbw, m_transferState.x, m_transferState.y, value);
            offset += 2u;
            advancePixel(1u);
            break;
        }
        case GS_PSM_T8:
        case GS_PSM_T8H:
            WriteVramUnlocked(dpsm, dbp, dbw, m_transferState.x, m_transferState.y, data[offset++]);
            advancePixel(1u);
            break;
        case GS_PSM_T4:
        case GS_PSM_T4HL:
        case GS_PSM_T4HH:
        {
            const uint8_t packed = data[offset++];
            const uint32_t firstPixel = m_transferState.copiedPixels;
            WriteVramUnlocked(dpsm, dbp, dbw,
                              dsax + (firstPixel % rrw),
                              m_transfer.trxpos.dsay + (firstPixel / rrw),
                              packed & 0x0Fu);
            if (firstPixel + 1u < m_transferState.totalPixels)
            {
                const uint32_t secondPixel = firstPixel + 1u;
                WriteVramUnlocked(dpsm, dbp, dbw,
                                  dsax + (secondPixel % rrw),
                                  m_transfer.trxpos.dsay + (secondPixel / rrw),
                                  (packed >> 4u) & 0x0Fu);
            }
            advancePixel(std::min<uint32_t>(2u, m_transferState.totalPixels - firstPixel));
            break;
        }
        default:
            return;
        }
    }
}

void GSCpuBackend::PerformLocalToLocalTransfer()
{
    if (!m_vram)
        return;

    const uint32_t rrw = m_transfer.trxreg.rrw;
    const uint32_t rrh = m_transfer.trxreg.rrh;
    const uint32_t total = rrw * rrh;
    if (total == 0u)
    {
        m_transferState.direction = 3u;
        return;
    }
    uint32_t *const plane = fcPlaneOrNull();
    const bool copiesPlane = plane && gsfc::frame16(m_transfer.bitbltbuf.spsm) && gsfc::frame16(m_transfer.bitbltbuf.dpsm);

    for (uint32_t pixel = 0; pixel < total; ++pixel)
    {
        uint32_t x = pixel % rrw;
        uint32_t y = pixel / rrw;
        if ((m_transfer.trxpos.dir & 0x2u) != 0u)
            x = rrw - x - 1u;
        if ((m_transfer.trxpos.dir & 0x1u) != 0u)
            y = rrh - y - 1u;

        const uint32_t value = ReadVramUnlocked(m_transfer.bitbltbuf.spsm,
                                                m_transfer.bitbltbuf.sbp,
                                                std::max<uint32_t>(m_transfer.bitbltbuf.sbw, 1u),
                                                x + m_transfer.trxpos.ssax,
                                                y + m_transfer.trxpos.ssay);
        WriteVramUnlocked(m_transfer.bitbltbuf.dpsm,
                          m_transfer.bitbltbuf.dbp,
                          std::max<uint32_t>(m_transfer.bitbltbuf.dbw, 1u),
                          x + m_transfer.trxpos.dsax,
                          y + m_transfer.trxpos.dsay,
                          value);
        // GT4's race colour (gs_fullcolour.h): a 16-bit to 16-bit copy takes each pixel's plane word along (the
        // GL renderer copies the target texels, which hold the full colour), so the copy's full colour is the
        // source's wherever the source's was valid.
        if (copiesPlane)
            plane[gsfc::halfIndex(m_transfer.bitbltbuf.dpsm, m_transfer.bitbltbuf.dbp, std::max<uint32_t>(m_transfer.bitbltbuf.dbw, 1u),
                                  x + m_transfer.trxpos.dsax, y + m_transfer.trxpos.dsay)] =
                plane[gsfc::halfIndex(m_transfer.bitbltbuf.spsm, m_transfer.bitbltbuf.sbp, std::max<uint32_t>(m_transfer.bitbltbuf.sbw, 1u),
                                      x + m_transfer.trxpos.ssax, y + m_transfer.trxpos.ssay)];
    }

    m_transferState.copiedPixels = total;
    m_transferState.direction = 3u;
}

void GSCpuBackend::PerformLocalToHostTransfer()
{
    m_localToHostBuffer.clear();
    m_localToHostReadPos = 0u;
    if (!m_vram)
        return;

    const uint32_t rrw = m_transfer.trxreg.rrw;
    const uint32_t rrh = m_transfer.trxreg.rrh;
    const uint32_t sbw = std::max<uint32_t>(m_transfer.bitbltbuf.sbw, 1u);
    const uint8_t spsm = m_transfer.bitbltbuf.spsm;
    const uint32_t bpp = static_cast<uint32_t>(GSMem::BitsPerPixel(static_cast<GSMem::PixelStorageMode>(spsm)));
    const uint32_t total = rrw * rrh;
    m_localToHostBuffer.reserve((static_cast<size_t>(total) * bpp + 7u) / 8u);

    for (uint32_t pixel = 0u; pixel < total; ++pixel)
    {
        const uint32_t x = pixel % rrw;
        const uint32_t y = pixel / rrw;
        const uint32_t value = ReadVramUnlocked(spsm,
                                                m_transfer.bitbltbuf.sbp,
                                                sbw,
                                                x + m_transfer.trxpos.ssax,
                                                y + m_transfer.trxpos.ssay);
        switch (bpp)
        {
        case 32:
            m_localToHostBuffer.push_back(static_cast<uint8_t>(value));
            m_localToHostBuffer.push_back(static_cast<uint8_t>(value >> 8u));
            m_localToHostBuffer.push_back(static_cast<uint8_t>(value >> 16u));
            m_localToHostBuffer.push_back(static_cast<uint8_t>(value >> 24u));
            break;
        case 24:
            m_localToHostBuffer.push_back(static_cast<uint8_t>(value));
            m_localToHostBuffer.push_back(static_cast<uint8_t>(value >> 8u));
            m_localToHostBuffer.push_back(static_cast<uint8_t>(value >> 16u));
            break;
        case 16:
            m_localToHostBuffer.push_back(static_cast<uint8_t>(value));
            m_localToHostBuffer.push_back(static_cast<uint8_t>(value >> 8u));
            break;
        case 8:
            m_localToHostBuffer.push_back(static_cast<uint8_t>(value));
            break;
        case 4:
        {
            if ((pixel & 1u) != 0u)
                break;
            uint32_t next = 0u;
            if (pixel + 1u < total)
            {
                const uint32_t nextPixel = pixel + 1u;
                const uint32_t nextX = nextPixel % rrw;
                const uint32_t nextY = nextPixel / rrw;
                next = ReadVramUnlocked(spsm, m_transfer.bitbltbuf.sbp, sbw,
                                        nextX + m_transfer.trxpos.ssax,
                                        nextY + m_transfer.trxpos.ssay);
            }
            m_localToHostBuffer.push_back(static_cast<uint8_t>((value & 0x0Fu) | ((next & 0x0Fu) << 4u)));
            break;
        }
        default:
            break;
        }
    }

    m_transferState.copiedPixels = total;
    m_transferState.localToHostPendingBytes = m_localToHostBuffer.size();

    // Opt-in diagnostic: GT3_GS_READBACK_LOG=1 logs each GS local->host transfer
    // (source rectangle and, for 32-bit data, the mean of each byte channel).
    static const bool readbackLog = [] {
        const char *v = std::getenv("GT3_GS_READBACK_LOG");
        return v && *v == '1';
    }();
    if (readbackLog)
    {
        double sum[4] = {0.0, 0.0, 0.0, 0.0};
        const size_t n = m_localToHostBuffer.size() / 4u;
        for (size_t i = 0; i < n * 4u; ++i)
            sum[i & 3u] += m_localToHostBuffer[i];
        std::fprintf(stderr, "[gs-readback] sbp=0x%x sbw=%u spsm=0x%x ssa=%u,%u rr=%ux%u bytes=%zu mean=%.1f/%.1f/%.1f/%.1f\n",
                     m_transfer.bitbltbuf.sbp, sbw, spsm, m_transfer.trxpos.ssax, m_transfer.trxpos.ssay, rrw, rrh,
                     m_localToHostBuffer.size(), n ? sum[0] / n : 0.0, n ? sum[1] / n : 0.0, n ? sum[2] / n : 0.0,
                     n ? sum[3] / n : 0.0);
    }
}

uint32_t GSCpuBackend::ConsumeLocalToHostBytes(uint8_t *dst, uint32_t maxBytes)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!dst || maxBytes == 0u || m_localToHostReadPos >= m_localToHostBuffer.size())
        return 0u;
    const size_t count = std::min<size_t>(maxBytes, m_localToHostBuffer.size() - m_localToHostReadPos);
    std::memcpy(dst, m_localToHostBuffer.data() + m_localToHostReadPos, count);
    m_localToHostReadPos += count;
    m_transferState.localToHostPendingBytes = m_localToHostBuffer.size() - m_localToHostReadPos;
    return static_cast<uint32_t>(count);
}

bool GSCpuBackend::ClearFramebuffer(const GSContext &context, uint32_t rgba)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    flushPendingUnlocked();
    if (!m_vram || context.frame.fbw == 0u)
        return false;

    const uint32_t x0 = context.scissor.x0;
    const uint32_t x1 = std::max<uint32_t>(x0, context.scissor.x1);
    const uint32_t y0 = context.scissor.y0;
    const uint32_t y1 = std::max<uint32_t>(y0, context.scissor.y1);
    uint8_t r = static_cast<uint8_t>(rgba);
    uint8_t g = static_cast<uint8_t>(rgba >> 8u);
    uint8_t b = static_cast<uint8_t>(rgba >> 16u);
    uint8_t a = static_cast<uint8_t>(rgba >> 24u);
    if ((context.fba & 1ull) != 0ull && context.frame.psm != GS_PSM_CT24)
        a |= 0x80u;

    const uint32_t fbp = GSInternal::framePageBaseToBlock(context.frame.fbp);
    const uint32_t fbw = std::max<uint32_t>(context.frame.fbw, 1u);
    if (context.frame.psm == GS_PSM_CT32 || context.frame.psm == GS_PSM_CT24)
    {
        const uint32_t source = static_cast<uint32_t>(r) |
                                (static_cast<uint32_t>(g) << 8u) |
                                (static_cast<uint32_t>(b) << 16u) |
                                (static_cast<uint32_t>(a) << 24u);
        for (uint32_t y = y0; y <= y1; ++y)
            for (uint32_t x = x0; x <= x1; ++x)
            {
                uint32_t pixel = source;
                if (context.frame.fbmsk != 0u)
                {
                    const uint32_t old = ReadVramUnlocked(context.frame.psm, fbp, fbw, x, y);
                    pixel = (pixel & ~context.frame.fbmsk) | (old & context.frame.fbmsk);
                }
                WriteVramUnlocked(context.frame.psm, fbp, fbw, x, y, pixel);
            }
        return true;
    }

    if (context.frame.psm == GS_PSM_CT16 || context.frame.psm == GS_PSM_CT16S)
    {
        const uint16_t source = encodeFramePixelPSMCT16(r, g, b, a);
        const uint16_t mask = static_cast<uint16_t>(context.frame.fbmsk);
        for (uint32_t y = y0; y <= y1; ++y)
            for (uint32_t x = x0; x <= x1; ++x)
            {
                uint16_t pixel = source;
                if (mask != 0u)
                {
                    const uint16_t old = static_cast<uint16_t>(ReadVramUnlocked(context.frame.psm, fbp, fbw, x, y));
                    pixel = static_cast<uint16_t>((pixel & ~mask) | (old & mask));
                }
                WriteVramUnlocked(context.frame.psm, fbp, fbw, x, y, pixel);
                // (GT4's race colour, gs_fullcolour.h: the HLE clear is a 16-bit write; GT4's own GS library
                // never uses it.)
                if (uint32_t *plane = fcPlaneOrNull())
                    plane[gsfc::halfIndex(context.frame.psm, fbp, fbw, x, y)] = gsfc::expand(pixel);
            }
        return true;
    }
    return false;
}

bool GSCpuBackend::CopyFrameToHostRgba(const GSFrameReg &frame,
                                       uint32_t width,
                                       uint32_t height,
                                       std::vector<uint8_t> &outPixels,
                                       bool preserveAlpha,
                                       bool useLocalMemoryLayout,
                                       bool frameBaseIsPages,
                                       uint32_t sourceOriginX,
                                       uint32_t sourceOriginY,
                                       bool fullColour) const
{
    if (!m_vram || m_vramSize == 0u)
        return false;
    // GT4's race colour (gs_fullcolour.h): a 16-bit display buffer in 480p / 1080i with 32-bit is shown in
    // full colour, F = the plane word while it still reduces to the 16-bit value, else v << 3 (as the GL
    // renderer's 16-bit target holds a pixel no full-colour draw wrote). Present's snapshot carries the
    // displayed pages' plane words (m_fcView).
    const uint32_t *const fcPlane = fullColour ? (m_fcView ? m_fcView : (m_fcPlane.empty() ? nullptr : m_fcPlane.data())) : nullptr;

    outPixels.assign(hostFrameBytes(height), 0u);
    const uint32_t baseBytes = frameBaseIsPages ? frame.fbp * 8192u : frame.fbp * 256u;
    const uint32_t basePtr = frameBaseIsPages ? GSInternal::framePageBaseToBlock(frame.fbp) : frame.fbp;
    const uint32_t fbw = frame.fbw ? frame.fbw : kHostFrameWidth / 64u;
    const uint32_t bytesPerPixel = (frame.psm == GS_PSM_CT16 || frame.psm == GS_PSM_CT16S) ? 2u : 4u;
    const uint32_t stride = fbw * 64u * bytesPerPixel;

    for (uint32_t y = 0; y < height; ++y)
    {
        uint8_t *dst = outPixels.data() + y * kHostFrameWidth * 4u;
        for (uint32_t x = 0; x < width; ++x)
        {
            const uint32_t sx = sourceOriginX + x;
            const uint32_t sy = sourceOriginY + y;
            if (frame.psm == GS_PSM_CT32 || frame.psm == GS_PSM_CT24)
            {
                uint32_t color = 0u;
                if (useLocalMemoryLayout)
                    color = ReadVramUnlocked(frame.psm, basePtr, fbw, sx, sy);
                else
                {
                    const uint32_t pixelBytes = frame.psm == GS_PSM_CT24 ? 3u : 4u;
                    const uint64_t offset = static_cast<uint64_t>(baseBytes) + static_cast<uint64_t>(sy) * stride + static_cast<uint64_t>(sx) * pixelBytes;
                    if (offset + pixelBytes > m_vramSize)
                        return false;
                    color = m_vram[offset] | (static_cast<uint32_t>(m_vram[offset + 1u]) << 8u) |
                            (static_cast<uint32_t>(m_vram[offset + 2u]) << 16u);
                    if (pixelBytes == 4u)
                        color |= static_cast<uint32_t>(m_vram[offset + 3u]) << 24u;
                }
                dst[x * 4u] = static_cast<uint8_t>(color);
                dst[x * 4u + 1u] = static_cast<uint8_t>(color >> 8u);
                dst[x * 4u + 2u] = static_cast<uint8_t>(color >> 16u);
                dst[x * 4u + 3u] = preserveAlpha && frame.psm != GS_PSM_CT24 ? static_cast<uint8_t>(color >> 24u) : 255u;
            }
            else if (frame.psm == GS_PSM_CT16 || frame.psm == GS_PSM_CT16S)
            {
                uint16_t color = 0u;
                if (useLocalMemoryLayout)
                    color = static_cast<uint16_t>(ReadVramUnlocked(frame.psm, basePtr, fbw, sx, sy));
                else
                {
                    const uint64_t offset = static_cast<uint64_t>(baseBytes) + static_cast<uint64_t>(sy) * stride + static_cast<uint64_t>(sx) * 2u;
                    if (offset + 2u > m_vramSize)
                        return false;
                    std::memcpy(&color, m_vram + offset, sizeof(color));
                }
                if (fullColour && useLocalMemoryLayout)
                {
                    const uint32_t f = fcPlane ? gsfc::full(fcPlane[gsfc::halfIndex(frame.psm, basePtr, fbw, sx, sy)], color) : gsfc::expand(color);
                    dst[x * 4u] = static_cast<uint8_t>(f);
                    dst[x * 4u + 1u] = static_cast<uint8_t>(f >> 8u);
                    dst[x * 4u + 2u] = static_cast<uint8_t>(f >> 16u);
                    dst[x * 4u + 3u] = preserveAlpha ? static_cast<uint8_t>(f >> 24u) : 255u;
                    continue;
                }
                const uint32_t r = color & 31u;
                const uint32_t g = (color >> 5u) & 31u;
                const uint32_t b = (color >> 10u) & 31u;
                dst[x * 4u] = static_cast<uint8_t>((r << 3u) | (r >> 2u));
                dst[x * 4u + 1u] = static_cast<uint8_t>((g << 3u) | (g >> 2u));
                dst[x * 4u + 2u] = static_cast<uint8_t>((b << 3u) | (b >> 2u));
                dst[x * 4u + 3u] = preserveAlpha ? ((color & 0x8000u) ? 0x80u : 0u) : 255u;
            }
            else
            {
                outPixels.clear();
                return false;
            }
        }
    }
    return true;
}

PresentationFrame GSCpuBackend::Present(const GSPresentationRequest &request)
{
    // Snapshot local memory under the backend lock, then perform the expensive
    // display conversion without holding the producer-side raster lock.
    thread_local std::vector<uint8_t> snapshot;
    // GT4's race colour (gs_fullcolour.h): the plane words of the displayed 16-bit buffers, taken under the
    // same lock (only pages copied for this request are read: those it shows).
    thread_local std::vector<uint32_t> planeSnapshot;
    bool planeCopied = false;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        flushPendingUnlocked();
        if (!m_vram || m_vramSize == 0u)
            snapshot.clear();
        else
        {
            snapshot.resize(m_vramSize);
            std::memcpy(snapshot.data(), m_vram, m_vramSize);
        }
        if (!m_fcPlane.empty())
            for (int circuit = 0; circuit < 2; ++circuit)
            {
                if (((request.pmode >> circuit) & 1u) == 0u)
                    continue;
                const uint64_t dispfb = circuit ? request.dispfb2 : request.dispfb1;
                const uint64_t display = circuit ? request.display2 : request.display1;
                const GSFrameReg frame = decodeDisplayFrame(dispfb);
                if (frame.fbw == 0u || !gsfc::shows(frame.psm, request.crt))
                    continue;
                const GSDisplayReadOrigin origin = decodeDisplayReadOrigin(dispfb);
                uint32_t width = 0u, height = 0u;
                decodeDisplaySize(display, width, height);
                if (planeSnapshot.size() != gsfc::kPlaneWords)
                    planeSnapshot.assign(gsfc::kPlaneWords, 0u);
                forRectPages(frame.fbp, false, frame.fbw, frame.psm, static_cast<int>(origin.x), static_cast<int>(origin.y),
                             static_cast<int>(origin.x + width) - 1, static_cast<int>(origin.y + height) - 1, [&](uint32_t page) {
                                 std::memcpy(planeSnapshot.data() + static_cast<size_t>(page) * 4096u,
                                             m_fcPlane.data() + static_cast<size_t>(page) * 4096u, 4096u * sizeof(uint32_t));
                             });
                planeCopied = true;
            }
    }
    if (snapshot.empty())
        return {};

    thread_local GSCpuBackend snapshotBackend;
    snapshotBackend.Initialize(snapshot.data(), static_cast<uint32_t>(snapshot.size()));
    snapshotBackend.m_fcView = planeCopied ? planeSnapshot.data() : nullptr;
    return snapshotBackend.PresentFromLocalMemory(request);
}

PresentationFrame GSCpuBackend::PresentFromLocalMemory(const GSPresentationRequest &request)
{
    PresentationFrame result{};
    const GSPmodeState pmode = decodePmode(request.pmode);
    const GSSmode2State smode2 = decodeSMode2(request.smode2);
    const bool fieldMode = smode2.interlaced && !smode2.frameMode;
    const bool halfHeightFrame = smode2.interlaced && smode2.frameMode;
#if defined(PS2X_GAME_GT3)
    const bool lineDoubleField = fieldMode;
#else
    // GT4 (picture track, analysis/tb2-fixes/picture): an interlaced picture read from one full-height buffer every
    // other line (SMODE2 INT with FFMD 0, GT4's video-mode id 2 screens) is shown whole, as a TV's two fields of it
    // make it; line-doubling the newest field halved its lines and made it bounce a line every VSync.
    // PS2X_FRAME_MODE_FIELD=1 (diagnostics): the newest field line-doubled, as before.
    static const bool frameModeField = [] {
        const char *v = std::getenv("PS2X_FRAME_MODE_FIELD");
        return v && *v == '1';
    }();
    const bool lineDoubleField = fieldMode && frameModeField;
#endif
    const bool oddField = (request.vsyncTick & 1ull) != 0ull;
    const GSFrameReg displayFrame1 = decodeDisplayFrame(request.dispfb1);
    const GSFrameReg displayFrame2 = decodeDisplayFrame(request.dispfb2);
    const GSDisplayReadOrigin origin1 = decodeDisplayReadOrigin(request.dispfb1);
    const GSDisplayReadOrigin origin2 = decodeDisplayReadOrigin(request.dispfb2);
    uint32_t width1 = 0u, height1 = 0u, width2 = 0u, height2 = 0u;
    decodeDisplaySize(request.display1, width1, height1);
    decodeDisplaySize(request.display2, width2, height2);
    const bool valid1 = pmode.enableCrt1 && hasDisplaySetup(request.display1, displayFrame1);
    const bool valid2 = pmode.enableCrt2 && hasDisplaySetup(request.display2, displayFrame2);
    if (!valid1 && !valid2)
    {
#if !defined(PS2X_GAME_GT3)
        // No read circuit shows a buffer (GT4 turns both off while it
        // switches the video mode): black, as a TV shows it, instead of
        // nothing new (the host kept the last frame).
        // PS2X_BLACK_WHEN_OFF=0: as before.
        static const bool blackWhenOff = [] {
            const char *v = std::getenv("PS2X_BLACK_WHEN_OFF");
            return !(v && *v == '0');
        }();
        if (blackWhenOff)
        {
            const uint64_t display = ((request.display2 >> 32) & 0xFFFu) != 0u ? request.display2 : request.display1;
            decodeDisplaySize(display, result.width, result.height);
            result.pixels.assign(hostFrameBytes(result.height), 0u);
            normalizePresentationAlpha(result.pixels, result.width, result.height);
        }
#endif
        return result;
    }

    auto copySource = [&](const GSFrameReg &displayFrame,
                          const GSDisplayReadOrigin &origin,
                          uint32_t width,
                          uint32_t height,
                          bool allowPreferred,
                          bool preserveAlpha,
                          GSFrameReg &selected,
                          std::vector<uint8_t> &pixels,
                          bool &usedPreferred) -> bool
    {
        selected = displayFrame;
        pixels.clear();
        usedPreferred = false;
        const uint32_t readHeight = halfHeightFrame ? (height + 1u) / 2u : height;
        if (allowPreferred && request.hasPreferredSource && request.preferredDestFbp == displayFrame.fbp &&
            (request.preferredSource.fbw != 0u || request.preferredSource.fbp != displayFrame.fbp) &&
            CopyFrameToHostRgba(request.preferredSource, width, readHeight, pixels, preserveAlpha, true, false, 0u, 0u))
        {
            selected = request.preferredSource;
            usedPreferred = true;
        }
        if (pixels.empty() && !CopyFrameToHostRgba(displayFrame, width, readHeight, pixels, preserveAlpha, true, true, origin.x, origin.y,
                                                   gsfc::shows(displayFrame.psm, request.crt)))
            return false;

#if defined(PS2X_GAME_GT3)
        // GT3 heuristic: an all-black buffer at FBP 0 shows a context frame instead. GT4's
        // display buffer 0 is at FBP 0 and GT4 shows real black fields (boot gaps, fades),
        // so other games show the buffer as is (analysis/phase5/specs/step3-compare.md 4.2).
        if (!usedPreferred && displayFrame.fbp == 0u && countNonBlackPixels(pixels, width, readHeight) == 0u)
        {
            for (const GSFrameReg &candidate : request.contextFrames)
            {
                if (candidate.fbp == selected.fbp && candidate.fbw == selected.fbw && candidate.psm == selected.psm)
                    continue;
                std::vector<uint8_t> candidatePixels;
                if (!CopyFrameToHostRgba(candidate, width, readHeight, candidatePixels, preserveAlpha, true, true, 0u, 0u))
                    continue;
                if (countNonBlackPixels(candidatePixels, width, readHeight) == 0u)
                    continue;
                selected = candidate;
                pixels.swap(candidatePixels);
                break;
            }
        }
#endif
        if (halfHeightFrame)
        {
            // The woven picture is twice the rows read (1080i: 960).
            if (pixels.size() < hostFrameBytes(height))
                pixels.resize(hostFrameBytes(height), 0u);
            const int phaseParity =
                ps2x::crt::is1080i(request.crt) && request.fieldPhase != 0xFFu ? static_cast<int>((request.fieldPhase >> 3) & 1u) : -1;
            thread_local WeaveHistory history[2];
            weaveInterlacedFrame(pixels, width, height, request.vsyncTick, history[&displayFrame == &displayFrame2 ? 1 : 0], request.allowWeave,
                                 phaseParity);
        }
        return true;
    };

    if (valid1 && valid2)
    {
        GSFrameReg selected1{}, selected2{};
        std::vector<uint8_t> crt1, crt2;
        bool preferred1 = false, preferred2 = false;
        if (copySource(displayFrame1, origin1, width1, height1, false, true, selected1, crt1, preferred1) &&
            copySource(displayFrame2, origin2, width2, height2, false, true, selected2, crt2, preferred2))
        {
            result.width = std::max(width1, width2);
            result.height = std::max(height1, height2);
            result.pixels.assign(hostFrameBytes(result.height), 0u);
            // One buffer read by both circuits at the same height, less than
            // a GS pixel apart across (GT4's 1080i softening, PMODE 0x8063,
            // DISPLAY1 = DISPLAY2 + 1 VCK; VIDEO-MODES.md 1.5): the merge
            // circuit blends circuit 1 over circuit 2 at each VCK, so over a
            // pixel's MAGH+1 VCKs it is mixed with its neighbour by
            // ALP / 255 x |DX1 - DX2| / (MAGH + 1). The GL presenter's merge
            // does the same. (Picture track) The vertical version, GT4's GS-lib
            // state 0x16 (DISPLAY1 = DISPLAY2 one line lower, ALP 0x30;
            // VIDEO-MODES.md 1.5): each line mixed with the line above (circuit 1
            // lower) or below by ALP / 255. Other offsets: both at the same
            // origin, as before. (GT3 builds: as before, no merge.)
#if !defined(PS2X_GAME_GT3)
            {
                const auto field = [](uint64_t v, int shift, uint64_t mask) { return static_cast<int>((v >> shift) & mask); };
                const int vck = field(request.display2, 23, 0xFu) + 1;
                const int d = field(request.display1, 0, 0xFFFu) - field(request.display2, 0, 0xFFFu);
                const int dy = field(request.display1, 12, 0x7FFu) - field(request.display2, 12, 0x7FFu);
                const bool sameBuffer = selected1.fbp == selected2.fbp && selected1.fbw == selected2.fbw && selected1.psm == selected2.psm &&
                                        origin1.x == origin2.x && origin1.y == origin2.y && width1 == width2 && height1 == height2 &&
                                        field(request.display1, 23, 0xFu) == field(request.display2, 23, 0xFu);
                const bool across = dy == 0 && d > -vck && d < vck, down = d == 0 && (dy == 1 || dy == -1);
                if (sameBuffer && pmode.mmod && !pmode.slbg && (across || down))
                {
                    const int dir = across ? (d > 0 ? -1 : (d < 0 ? 1 : 0)) : 0;
                    const int dirY = down ? -dy : 0;
                    const float w = static_cast<float>(pmode.alp) / 255.0f *
                                    (down ? 1.0f : static_cast<float>(d < 0 ? -d : d) / static_cast<float>(vck));
                    for (uint32_t y = 0; y < result.height; ++y)
                        for (uint32_t x = 0; x < result.width; ++x)
                        {
                            const uint32_t nx = static_cast<uint32_t>(std::clamp(static_cast<int>(x) + dir, 0, static_cast<int>(result.width) - 1));
                            const uint32_t ny = static_cast<uint32_t>(std::clamp(static_cast<int>(y) + dirY, 0, static_cast<int>(result.height) - 1));
                            const uint8_t *a = crt2.data() + (y * kHostFrameWidth + x) * 4u;
                            const uint8_t *b = crt2.data() + (ny * kHostFrameWidth + nx) * 4u;
                            uint8_t *dst = result.pixels.data() + (y * kHostFrameWidth + x) * 4u;
                            for (int c = 0; c < 3; ++c)
                                dst[c] = static_cast<uint8_t>(std::lround(static_cast<float>(a[c]) + w * (static_cast<float>(b[c]) - static_cast<float>(a[c]))));
                            dst[3] = 255u;
                        }
                    if (lineDoubleField)
                        applyFieldPresentation(result.pixels, result.width, result.height, oddField);
                    result.displayFbp = displayFrame2.fbp;
                    result.sourceFbp = selected2.fbp;
                    return result;
                }
            }
#endif
            const uint8_t bgR = static_cast<uint8_t>(request.bgcolor);
            const uint8_t bgG = static_cast<uint8_t>(request.bgcolor >> 8u);
            const uint8_t bgB = static_cast<uint8_t>(request.bgcolor >> 16u);
            for (uint32_t y = 0; y < result.height; ++y)
                for (uint32_t x = 0; x < result.width; ++x)
                {
                    uint8_t *dst = result.pixels.data() + (y * kHostFrameWidth + x) * 4u;
                    dst[0] = bgR;
                    dst[1] = bgG;
                    dst[2] = bgB;
                    dst[3] = pmode.alp;
                }
            if (!pmode.slbg)
                for (uint32_t y = 0; y < height2; ++y)
                    std::memcpy(result.pixels.data() + y * kHostFrameWidth * 4u, crt2.data() + y * kHostFrameWidth * 4u, width2 * 4u);
            for (uint32_t y = 0; y < height1; ++y)
                for (uint32_t x = 0; x < width1; ++x)
                {
                    const uint8_t *src = crt1.data() + (y * kHostFrameWidth + x) * 4u;
                    uint8_t *dst = result.pixels.data() + (y * kHostFrameWidth + x) * 4u;
                    const uint32_t factor = pmode.mmod ? pmode.alp : std::min<uint32_t>(255u, static_cast<uint32_t>(src[3]) * 2u);
                    dst[0] = blendPresentationChannel(src[0], dst[0], factor);
                    dst[1] = blendPresentationChannel(src[1], dst[1], factor);
                    dst[2] = blendPresentationChannel(src[2], dst[2], factor);
                    dst[3] = pmode.amod ? dst[3] : src[3];
                }
            normalizePresentationAlpha(result.pixels, result.width, result.height);
            if (lineDoubleField)
                applyFieldPresentation(result.pixels, result.width, result.height, oddField);
            result.displayFbp = displayFrame1.fbp;
            result.sourceFbp = selected1.fbp;
            return result;
        }
    }

    const GSFrameReg &displayFrame = valid1 ? displayFrame1 : displayFrame2;
    const GSDisplayReadOrigin &origin = valid1 ? origin1 : origin2;
    result.width = valid1 ? width1 : width2;
    result.height = valid1 ? height1 : height2;
    GSFrameReg selected = displayFrame;
    if (!copySource(displayFrame, origin, result.width, result.height, true, false, selected, result.pixels, result.usedPreferred))
        return {};
    if (lineDoubleField)
        applyFieldPresentation(result.pixels, result.width, result.height, oddField);
    normalizePresentationAlpha(result.pixels, result.width, result.height);
    result.displayFbp = displayFrame.fbp;
    result.sourceFbp = selected.fbp;
    return result;
}
