#include "external/glad.h" // raylib's GL loader: its function pointers are loaded by InitWindow

#include "runtime/gs/gs_gl_backend.h"
#include "runtime/gs/gs_dither.h"
#include "runtime/gs/gs_fullcolour_addr.h"
#include "runtime/gs/gs_mip.h"
#include "runtime/gs/ps2_gs_memory.h"
#include "gs_gl_shaders.h"
#include "gs_gl_context.h"
#include "gs_gl_present.h"
#include "runtime/vblank_clock.h"
#include "runtime/gs/gs_crt.h"
#include "runtime/gs/gs_car_view.h"
#if !defined(GT3_STOCK_BUILD) && !defined(PS2X_GAME_GT4)
#include "../gt3_texture_dump.h"
#include "../gt3_texture_replace.h"
#define GS_TEXTURE_DUMP 1
#elif defined(PS2X_GAME_GT4) && !defined(GT4_STOCK_BUILD)
#include "../gt4_texture_dump.h" // GT4_TEXTURE_DUMP: GT3's texture identity and dump, GT4's writer
#include "../gt4_texture_replace.h" // GT4_TEXTURE_REPLACE: GT3's replacement drawing, GT4's pictures (by sampled area)
#include "../gt4_vu1_tap.h" // GT4_VU1_TAP: lighting step 1 (verify: tapped vertices checked here)
#include "../gt4_upscale.h" // GT4_UPSCALER=dlss: DLSS at presentation (the Vulkan renderer only)
#include "../gt4_texture_owner.h" // GT4_TEXTURE_DUMP's car / course folders: the uploads' content
#include "../gt4_mirror.h" // the rear-view mirror page (GET /mirror.mjpg): read back here (Vulkan renderer)
#include "../gt4_hud_pictures.h" // the launcher's race pictures (HUD hand-off 7 A1): read back here (Vulkan renderer)
#include "../gt4_race_reader.h" // (GT4_HUD_MOVE_LOG: the race's VSync in the moved mirror's log)
#include "../gt4_hud_move.h" // GT4_HUD_MOVE's mirror (HUD hand-off 7 B3): kept aside and drawn at its new place here (Vulkan renderer)
namespace gt3texdump = gt4texdump;
namespace gt3texreplace = gt4texreplace;
#define GS_TEXTURE_DUMP 1
#endif

#ifndef GL_CONSERVATIVE_RASTERIZATION_NV
#define GL_CONSERVATIVE_RASTERIZATION_NV 0x9346
#endif
// Presentation through OpenGL's shared slots (gsgl::beginPresentSlot); the
// Vulkan build of this source (gs_vk_backend.cpp) uses them when its images
// can be shared with OpenGL, else the local memory copy.
#ifndef GS_GL_GPU_PRESENT
#define GS_GL_GPU_PRESENT 1
#endif
// Pages written into scaled targets: repeated on the GPU (a nearest blit from a
// native-size texture) instead of on the CPU. The Vulkan build sets 1.
#ifndef GS_GPU_UPSAMPLE
#define GS_GPU_UPSAMPLE 0
#endif
// The textures a batch samples through bindless handles, before its draws:
// the Vulkan build's hazard tracking needs them (OpenGL tracks them itself).
#ifndef GS_NOTE_BATCH_TEXTURES
#define GS_NOTE_BATCH_TEXTURES(textures, count) ((void)0)
#endif
// Batches per glFlush (GT3_GS_GL_FLUSH_EVERY overrides; see flushBatch). The
// Vulkan build sets 24: each flush is a queue submission there, dearer than
// OpenGL's, and its 32 command buffers in flight keep the GPU fed.
#ifndef GS_FLUSH_EVERY_DEFAULT
#define GS_FLUSH_EVERY_DEFAULT 8u
#endif
#ifndef GL_FRAGMENT_SHADER_INVOCATIONS_ARB
#define GL_FRAGMENT_SHADER_INVOCATIONS_ARB 0x82F4
#endif

#include <algorithm>
#include <optional>
#include <array>
#include <atomic>
#include <bitset>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>
#include <climits>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <map>
#include <source_location>
#include <string>
#if defined(_MSC_VER) && (defined(_M_X64) || defined(_M_IX86))
#include <intrin.h> // __rdtsc (statistics timers)
#define PS2X_GS_STAT_TSC 1
#elif defined(__x86_64__) || defined(__i386__)
#include <x86intrin.h>
#define PS2X_GS_STAT_TSC 1
#endif
#include <string_view>
#include <unordered_set>
#if defined(_M_X64) || defined(__x86_64__)
#include <emmintrin.h>
#endif

#if defined(PS2X_GAME_GT4) && !defined(GT4_STOCK_BUILD)
// DLSS step 1: a frame's sub-pixel jitter (internal-resolution pixels, -0.5..0.5): Halton (2, 3), 8 phases.
static void upscaleJitterOf(uint64_t frame, float &jx, float &jy)
{
    auto halton = [](uint32_t i, uint32_t b)
    {
        float f = 1.0f, r = 0.0f;
        while (i)
        {
            f /= static_cast<float>(b);
            r += f * static_cast<float>(i % b);
            i /= b;
        }
        return r;
    };
    const uint32_t i = static_cast<uint32_t>(frame % 8u) + 1u;
    jx = halton(i, 2u) - 0.5f;
    jy = halton(i, 3u) - 0.5f;
}
#endif

namespace
{
    constexpr uint32_t kPageCount = 512u;

    // Helper threads for the coverage masks of large batches (orderByLevels).
    // GT3_GS_GL_THREADS=<n>: helper count (0: none).
    class MaskPool
    {
    public:
        static MaskPool &instance()
        {
            static MaskPool pool;
            return pool;
        }

        unsigned helpers() const { return static_cast<unsigned>(m_workers.size()); }

        // Runs fn(0 .. tasks-1) on the helpers and the calling thread; returns when all are done.
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
        MaskPool()
        {
            unsigned count = 0u;
            if (const char *v = std::getenv("GT3_GS_GL_THREADS"); v && *v)
                count = static_cast<unsigned>(std::strtoul(v, nullptr, 10));
            else
            {
                const unsigned hw = std::max(1u, std::thread::hardware_concurrency());
                count = hw >= 12u ? 4u : (hw >= 8u ? 3u : (hw >= 6u ? 1u : 0u));
            }
            count = std::min(count, 16u);
            for (unsigned i = 0; i < count; ++i)
                m_workers.emplace_back([this] { loop(); });
        }

        ~MaskPool()
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

    // ---------------------------------------------------------------- GS memory

    bool isColor32(uint32_t psm) { return psm == GS_PSM_CT32 || psm == GS_PSM_CT24; }

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

    // Every 8 KiB page a rectangle of a buffer touches (as GSCpuBackend's
    // forRectPages); a block base that is not page aligned also marks the
    // following page.
    template <typename F>
    void forRectPages(uint32_t basePage, bool straddles, uint32_t bufferWidth, uint32_t psm, int x0, int y0, int x1, int y1, F &&f)
    {
        // (Page sizes 64/128 x 32/64/128: shifts for the divisions.)
        uint32_t wShift = 6u, hShift = 5u;
        switch (psm & 0x3Fu)
        {
        case GS_PSM_CT16: case GS_PSM_CT16S: case GS_PSM_Z16: case GS_PSM_Z16S:
            hShift = 6u; break;
        case GS_PSM_T8:
            wShift = 7u; hShift = 6u; break;
        case GS_PSM_T4:
            wShift = 7u; hShift = 7u; break;
        default:
            break;
        }
        const uint32_t perRow = std::max(1u, (std::max(bufferWidth, 1u) * 64u) >> wShift);
        x0 = std::max(x0, 0);
        y0 = std::max(y0, 0);
        x1 = std::min(x1, 2047);
        y1 = std::min(y1, 2047);
        if (x1 < x0 || y1 < y0)
            return;
        const uint32_t row1 = static_cast<uint32_t>(y1) >> hShift, col0 = static_cast<uint32_t>(x0) >> wShift,
                       col1 = static_cast<uint32_t>(x1) >> wShift;
        for (uint32_t row = static_cast<uint32_t>(y0) >> hShift; row <= row1; ++row)
            for (uint32_t col = col0; col <= col1; ++col)
            {
                const uint32_t page = basePage + row * perRow + col;
                f(page & (kPageCount - 1u));
                if (straddles)
                    f((page + 1u) & (kPageCount - 1u));
            }
    }

    uint32_t readTexel(uint8_t *vram, uint32_t psm, uint32_t bp, uint32_t bw, uint32_t x, uint32_t y)
    {
        switch (psm)
        {
        case GS_PSM_CT32: return GSMem::ReadCT32(vram, bp, bw, x, y);
        case GS_PSM_CT24: return GSMem::ReadCT24(vram, bp, bw, x, y);
        case GS_PSM_CT16: return GSMem::ReadCT16(vram, bp, bw, x, y);
        case GS_PSM_CT16S: return GSMem::ReadCT16S(vram, bp, bw, x, y);
        case GS_PSM_T8: return GSMem::ReadP8(vram, bp, bw, x, y);
        case GS_PSM_T8H: return GSMem::ReadP8H(vram, bp, bw, x, y);
        case GS_PSM_T4: return GSMem::ReadP4(vram, bp, bw, x, y);
        case GS_PSM_T4HL: return GSMem::ReadP4HL(vram, bp, bw, x, y);
        case GS_PSM_T4HH: return GSMem::ReadP4HH(vram, bp, bw, x, y);
        case GS_PSM_Z32: return GSMem::ReadZ32(vram, bp, bw, x, y);
        case GS_PSM_Z24: return GSMem::ReadZ24(vram, bp, bw, x, y);
        case GS_PSM_Z16: return GSMem::ReadZ16(vram, bp, bw, x, y);
        case GS_PSM_Z16S: return GSMem::ReadZ16S(vram, bp, bw, x, y);
        default: return 0u;
        }
    }

    uint32_t wordAddressCT32(uint32_t bp, uint32_t bw, uint32_t x, uint32_t y)
    {
        return GSMem::PixelStorageTraits<GSMem::C32>::Address(GSMem::PageTableC32, bp, bw, x, y) & 0xFFFFFu;
    }

    uint32_t rgba5551To8888(uint32_t c)
    {
        return (((c >> 0) & 0x1Fu) << 3) | ((((c >> 5) & 0x1Fu) << 3) << 8) | ((((c >> 10) & 0x1Fu) << 3) << 16) |
               ((((c >> 15) & 0x1u) << 7) << 24);
    }

    // GSCpuBackend's applyTexa.
    uint32_t applyTexa(const GSTexaReg &texa, uint32_t psm, uint32_t texel)
    {
        if (psm == GS_PSM_CT32)
            return texel;
        const bool rgbZero = (texel & 0x00FFFFFFu) == 0u;
        uint32_t a = texel >> 24;
        switch (psm)
        {
        case GS_PSM_CT24:
            a = (texa.aem && rgbZero) ? 0u : texa.ta0;
            break;
        case GS_PSM_CT16:
        case GS_PSM_CT16S:
            a = (a & 0x80u) ? texa.ta1 : ((texa.aem && rgbZero) ? 0u : texa.ta0);
            break;
        default:
            break;
        }
        return (texel & 0x00FFFFFFu) | (a << 24);
    }

    uint64_t hashBytes(const void *data, size_t size, uint64_t h = 1469598103934665603ull)
    {
        const uint8_t *p = static_cast<const uint8_t *>(data);
        size_t i = 0;
        // Palettes and CLUTs (up to 1 KB): 8 bytes a step (byte-wise FNV-1a
        // for small keys, as before).
        if (size >= 64u)
            for (; i + 8u <= size; i += 8u)
            {
                uint64_t v;
                std::memcpy(&v, p + i, 8);
                h = (h ^ v) * 0x9E3779B97F4A7C15ull;
                h ^= h >> 31;
            }
        for (; i < size; ++i)
            h = (h ^ p[i]) * 1099511628211ull;
        return h;
    }

    // static_cast<int>(std::floor(x)) / static_cast<int>(std::ceil(x)) without the CRT call (SSE2 has no rounding
    // instruction; the calls were ~3% of the renderer thread at Trial Mountain). The same int for every value,
    // including MSVC's INT_MIN for NaN and values out of range (checked for every float and 200M doubles).
    template <typename F>
    inline int floorToInt(F x)
    {
        const int i = static_cast<int>(x);
        return (i != INT_MIN && static_cast<F>(i) > x) ? i - 1 : i;
    }
    template <typename F>
    inline int ceilToInt(F x)
    {
        const int i = static_cast<int>(x);
        if (i == INT_MIN || !(static_cast<F>(i) < x))
            return i;
        return i == INT_MAX ? INT_MIN : i + 1;
    }

    // memcmp(a, b, sizeof(T)) == 0 for the per-primitive keys and states (inline, 16 bytes a step).
    template <typename T>
    inline bool sameBytes(const T &a, const T &b)
    {
        static_assert(sizeof(T) % 4u == 0u, "whole words");
        const uint8_t *pa = reinterpret_cast<const uint8_t *>(&a), *pb = reinterpret_cast<const uint8_t *>(&b);
        size_t i = 0;
#if defined(_M_X64) || defined(__x86_64__)
        for (; i + 16u <= sizeof(T); i += 16u)
            if (_mm_movemask_epi8(_mm_cmpeq_epi8(_mm_loadu_si128(reinterpret_cast<const __m128i *>(pa + i)),
                                                 _mm_loadu_si128(reinterpret_cast<const __m128i *>(pb + i)))) != 0xFFFF)
                return false;
#endif
        for (; i < sizeof(T); i += 4u)
        {
            uint32_t x, y;
            std::memcpy(&x, pa + i, 4);
            std::memcpy(&y, pb + i, 4);
            if (x != y)
                return false;
        }
        return true;
    }

    // ---------------------------------------------------------------- GPU data

    struct alignas(32) GpuPrim
    {
        double edgeC[4];
        double zv[4];
        float edgeA[4];
        float edgeB[4];
        float rgba[3][4];
        float stqf[3][4];
        float uvA[4];
        float uvB[4];
        int32_t bbox[4];
        int32_t extra[4];
    };
    static_assert(sizeof(GpuPrim) == 256, "GpuPrim must match the std430 Prim layout");

    // Lighting step 2 (GT4_CAR_LIGHTING=pixel): a lit triangle's corners' normals and light set (std430 LitTri, LightSet).
    struct GpuLitTri
    {
        float n[3][4]; // n[0][3]: the light set's index (int bits)
    };
    struct GpuLightSet
    {
        float l[3][4], amb[4], c[4][4], clampv[4], pw[4]; // pw: each light's power (M01-M03: 1, 1, 4/8/16, 4/8/16)
    };
    static_assert(sizeof(GpuLitTri) == 48 && sizeof(GpuLightSet) == 160, "the std430 LitTri / LightSet layouts");
    // Lighting step 2c: a reflection-layer triangle (M10): corners' local positions (w: Q), normals (n[0][3]: its set),
    // input colours; the set: eye, rotation rows, colour factor (std430 EnvTri, EnvSet).
    struct GpuEnvTri
    {
        float p[3][4], n[3][4], c[3][4];
    };
    struct GpuEnvSet
    {
        float eye[4], rot[3][4], fres[2][4];
    };
    static_assert(sizeof(GpuEnvTri) == 144 && sizeof(GpuEnvSet) == 96, "the std430 EnvTri / EnvSet layouts");
    static bool carLightingPerPixel()
    {
#if defined(PS2X_GAME_GT4) && !defined(GT4_STOCK_BUILD)
        static const bool on = [] {
            const char *v = std::getenv("GT4_CAR_LIGHTING");
            return v && (std::strcmp(v, "pixel") == 0 || std::strcmp(v, "split") == 0 || std::strcmp(v, "show") == 0);
        }();
        return on;
#else
        return false;
#endif
    }

    // Per-draw state of the fragment shader (std430 DrawState): bindless
    // texture and palette handles and the pixel pipeline settings.
    struct alignas(16) GpuState
    {
        uint64_t tex = 0;
        uint64_t palette = 0;
        int32_t v[15][4] = {}; // 13 used; 256 bytes, the stride of GpuPrim (both index the upload ring)
        // GS mipmapping uses the spare words v[12].w, v[13].w, v[14].z/w and, for a
        // per-pixel mip state only (never fastTarget), v[13].x/y (see Submit).
        // More state needs a 512-byte stride or a side buffer.
    };
    static_assert(sizeof(GpuState) == 256, "GpuState must match the std430 DrawState layout");

    // Everything the shader and GL state of a draw depend on besides the
    // primitive data; draws with equal keys share one GL draw call.
    struct DrawKey
    {
        int colorTarget = -1, depthTarget = -1;
        uint32_t texSource = 0; // GL texture name sampled (0: none)
        uint32_t paletteTex = 0; // CLUT as a 256x1 texture, or the 16x16 GPU CLUT (paletteMode 1)
        int32_t paletteMode = 0, clutCsa = 0, clutFour = 0, clutCt24 = 0;
        // A target sampled as the texture: the texture's base page, pages per
        // row and page size, and the target's base page, width and page height.
        int32_t texBasePage = 0, texPagesPerRow = 1, texPageW = 64, texPageH = 32;
        int32_t texTargetFbp = 0, texTargetFbw = 1, texTargetPageH = 32;
        int32_t tme, fst, iip, fge, tfx, tcc, texW, texH, linear, wrapU, wrapV, minU, maxU, minV, maxV, texMode;
        int32_t ta0, aem, ta1, fogR, fogG, fogB, ate, atst, aref, afail, dstRead, abe, pabe;
        int32_t ba, bb, bc, bd, fix, date, datm, fba, frameFormat;
        uint32_t fbmsk;
        int32_t zte, ztst, zmask;
        int32_t colorMask; // 4 bits RGBA
        int32_t needsSplit; // dst read or two-pass alpha: draw non-overlapping groups in order
        int32_t hwBlend = 0; // fast mode: GPU blending (1 add, 2 subtract, 3 reverse subtract, 4 Cs * Ad + Cd in two draws)
        int32_t fastTarget = 0, texOffX = 0, texOffY = 0; // fast mode: target read by the GPU's filter at this offset
        int32_t clutOrgX = 0, clutOrgY = 0; // paletteMode 1: the CLUT's square in paletteTex (a direct CLUT's target)
        // GS mipmapping, a per-pixel primitive (gs_mip.h; texMode 13 over every
        // level's pages): the shader's control word (gsmip::packGlCtl, 0 for
        // every other draw) and levels 1-5 as TBP | TBW << 14.
        int32_t mipCtl = 0;
        int32_t mipLv[5] = {};
        // GS dithering (gs_dither.h; CT16 frames here, other 16-bit frames are drawn by the CPU): the shader's
        // ditherOffset reads DIMX's rows 0-1 / 2-3 from v[13].w / v[14].z when v[14].w is -1 (see Submit).
        int32_t dither = 0;
        int32_t dimxLo = 0, dimxHi = 0;
        // Effects at their own scale (GT4_EFFECTS_SCALE): the sampled target's and the CLUT target's samples per
        // GS pixel when they differ from the destination's (x | y << 4; 0: the same).
        int32_t texScale = 0, paletteScale = 0;
        // GT4's race colour (GSDrawState::ftex): a CT16 target view (texMode 3) read as its texels' full colour
        // (state word v[13].z bit 1, beside fastTarget's bit 0; 0: the 16-bit value, as before).
        int32_t texFull = 0;
        bool operator==(const DrawKey &o) const { return sameBytes(*this, o); }
    };

    // Memory layout of a target's pixels: how (x, y) map to GS memory.
    enum class Layout
    {
        C32, // PSMCT32/24: 64x32 pixel pages
        C16, // PSMCT16: 64x64 pixel pages
        Z32, // PSMZ32/24: 64x32 pixel pages, Z block order
        Z16, // PSMZ16: 64x64 pixel pages, Z16 block order (depth targets only: GT4's 480p menus, PS2X_GS_GL_Z16)
    };

    uint32_t layoutPsm(Layout layout)
    {
        return layout == Layout::C16 ? GS_PSM_CT16
                                     : (layout == Layout::Z32 ? GS_PSM_Z32 : (layout == Layout::Z16 ? GS_PSM_Z16 : GS_PSM_CT32));
    }

    struct Target
    {
        bool depth = false; // D32F depth texture holding Z24 or Z16 values (z / 2^24; Layout Z32 or Z16)
        uint32_t uid = 0;   // unique per target made (findTarget), never reused: page moves back (transferPage)
        Layout layout = Layout::C32;
        uint32_t fbp = 0, fbw = 1;
        int width = 0, height = 0;
        // The height the target would have grown to without full-height
        // textures (PS2X_GS_GL_STABLE_TARGETS 2): the self-read cell grid
        // keeps that size, so its copy decisions are the same.
        int logicalHeight = 0;
        int pageHeight = 32;
        GLuint tex = 0;
        // Samples per GS pixel of tex (scaleX x scaleY, or the effects scale: effectScale).
        int sx = 1, sy = 1;
        // GT4's car view in its 480p menus (gs_car_view.h): a target of the car view's 224-row passes, held at
        // scaleX x 2 scaleY (never promoted); found by its own key, beside any normal target at the same base.
        bool tall = false;
        uint64_t lastUse = 0;
        // CPU page version this GPU copy matches; 0: never loaded.
        std::array<uint32_t, kPageCount> version{};

        int maxHeight() const
        {
            // (Remembered for the fbw and page height it was worked out for: findTarget asks for every primitive.)
            if (fbw != maxHeightFbw || pageHeight != maxHeightPageH)
            {
                maxHeightFbw = fbw;
                maxHeightPageH = pageHeight;
                maxHeightRows = static_cast<int>(std::min<uint32_t>(2048u, (kPageCount / fbw) * static_cast<uint32_t>(pageHeight)));
            }
            return maxHeightRows;
        }
        mutable uint32_t maxHeightFbw = 0;
        mutable int maxHeightPageH = 0, maxHeightRows = 0;

        bool pageRect(uint32_t page, int &x, int &y) const
        {
            const uint32_t k = (page - fbp) & (kPageCount - 1u);
            const uint32_t row = k / fbw, col = k % fbw;
            x = static_cast<int>(col * 64u);
            y = static_cast<int>(row * static_cast<uint32_t>(pageHeight));
            return y + pageHeight <= height;
        }
    };

    struct TexEntry
    {
        GLuint tex = 0;
        int width = 0, height = 0;
        uint64_t signature = 0;
        uint64_t lastUse = 0;
        // GPU-decoded textures: the page versions first seen, and when.
        uint64_t candidate = 0, candidateFrame = 0;
    };

    bool envFlag(const char *name, bool fallback)
    {
        const char *v = std::getenv(name);
        if (!v || !*v)
            return fallback;
        return *v != '0';
    }
}

struct GSGlBackend::Impl
{
    GSCpuBackend *cpu = nullptr;
    uint8_t *vram = nullptr;

    std::vector<std::unique_ptr<Target>> targets;
    std::unordered_map<uint64_t, int> targetIndex; // (depth, layout, fbp, fbw, tall) -> slot (targetIndexKey)
    // The key of a target in targetIndex. Every place that adds, looks up or removes an entry uses this one key:
    // GT4's full-height car view (Target::tall, gs_car_view.h) has targets beside normal ones at the same base, told
    // apart only by the tall bit (the developer's Test Build 30: a dropped tall target removed the normal target's entry and
    // left its own pointing at a reused slot; the car passes then drew into another buffer).
    static uint64_t targetIndexKey(bool depth, Layout layout, uint32_t fbp, uint32_t fbw, bool tall)
    {
        return (depth ? 1ull : 0ull) | (static_cast<uint64_t>(layout) << 1) | (tall ? 8ull : 0ull) | (static_cast<uint64_t>(fbp & 0x1FFu) << 4) |
               (static_cast<uint64_t>(fbw) << 16);
    }
    static uint64_t targetIndexKey(const Target &t) { return targetIndexKey(t.depth, t.layout, t.fbp, t.fbw, t.tall); }
    std::array<int, kPageCount> owner{};
    std::array<uint32_t, kPageCount> cpuVersion{};
    // Each page's content hash, taken once per CPU version (pageContentHash):
    // raw textures are cached by content, so GT3 re-streaming the same
    // texture into the same pages every frame is neither uploaded nor
    // decoded again.
    std::array<uint64_t, kPageCount> pageHash{};
    std::array<uint32_t, kPageCount> pageHashVersion{}; // cpuVersion + 1 when taken (0: never)
    uint64_t pageContentHash(uint32_t page)
    {
        page &= kPageCount - 1u;
        if (pageHashVersion[page] == cpuVersion[page] + 1u)
            return pageHash[page];
        const uint8_t *p = vram + static_cast<size_t>(page) * 8192u;
        uint64_t a = 0x9E3779B97F4A7C15ull, b = 0xC2B2AE3D27D4EB4Full, c = 0x165667B19E3779F9ull, d = 0x27D4EB2F165667C5ull;
        for (size_t i = 0; i < 8192u; i += 32u)
        {
            uint64_t w[4];
            std::memcpy(w, p + i, 32);
            a = (a ^ w[0]) * 0x100000001B3ull;
            b = (b ^ w[1]) * 0x100000001B3ull;
            c = (c ^ w[2]) * 0x100000001B3ull;
            d = (d ^ w[3]) * 0x100000001B3ull;
            a ^= a >> 29;
            b ^= b >> 29;
            c ^= c >> 29;
            d ^= d >> 29;
        }
        uint64_t h = a ^ (b * 0x9E3779B97F4A7C15ull) ^ (c * 0xC2B2AE3D27D4EB4Full) ^ (d * 0x165667B19E3779F9ull);
        h ^= h >> 31;
        pageHash[page] = h;
        pageHashVersion[page] = cpuVersion[page] + 1u;
        return h;
    }

    GLuint program = 0, vao = 0, primBuffer = 0, fbo = 0, readFbo = 0;
    // Lighting step 2: the batch's lit triangles and light sets (by record serial), and their buffers.
    std::vector<GpuLitTri> litTris;
    std::vector<GpuLightSet> lightSets;
    std::unordered_map<uint64_t, int> lightSetOf;
    GLuint litTriBuffer = 0, lightSetBuffer = 0;
    uint64_t statLitTris = 0;
    std::vector<GpuEnvTri> envTris;
    std::vector<GpuEnvSet> envSets;
    std::unordered_map<uint64_t, int> envSetOf;
    GLuint envTriBuffer = 0, envSetBuffer = 0;
    uint64_t statEnvTris = 0;
    // (a program's triangles come one after another: its set without the map)
    uint64_t lastLitSerial = 0, lastEnvSerial = 0;
    int lastLitSet = -1, lastEnvSet = -1;
#if defined(PS2X_GAME_GT4) && !defined(GT4_STOCK_BUILD)
    // A triangle whose three corners are tapped M07 vertices: its record (litTris index + 1), else 0.
    int litTriangle(const GSVertex &a, const GSVertex &b, const GSVertex &c)
    {
        const GSVertex *v[3] = {&a, &b, &c};
        const gt4tap::Record *r[3];
        gt4tap::Corner cn[3];
        for (int i = 0; i < 3; ++i)
        {
            if (!gt4tap::corner(v[i]->tapSlot, v[i]->tapK, v[i]->x, v[i]->y, v[i]->q, cn[i]))
                return 0;
            r[i] = cn[i].rec;
            if (!(r[i]->lit || r[i]->env))
                return 0;
        }
        if (r[0]->env || r[1]->env || r[2]->env)
        {
            if (!r[0]->env || !r[1]->env || !r[2]->env)
                return 0;
            int set = -1;
            if (lastEnvSet >= 0 && lastEnvSerial == r[0]->serial)
                set = lastEnvSet;
            else if (const auto it = envSetOf.find(r[0]->serial); it != envSetOf.end())
                set = it->second;
            else
            {
                GpuEnvSet s;
                std::memcpy(s.eye, r[0]->eye, sizeof(s.eye));
                std::memcpy(s.rot, r[0]->envRot, sizeof(s.rot));
                std::memcpy(s.fres, r[0]->fresnel, sizeof(s.fres));
                set = static_cast<int>(envSets.size());
                envSets.push_back(s);
                envSetOf.emplace(r[0]->serial, set);
            }
            lastEnvSerial = r[0]->serial;
            lastEnvSet = set;
            GpuEnvTri t;
            for (int i = 0; i < 3; ++i)
            {
                for (int l = 0; l < 3; ++l)
                {
                    t.p[i][l] = cn[i].pos[l];
                    t.n[i][l] = cn[i].nrm[l];
                }
                t.p[i][3] = v[i]->q;
                t.n[i][3] = 0.0f;
                for (int l = 0; l < 4; ++l)
                    t.c[i][l] = cn[i].colour[l];
            }
            std::memcpy(&t.n[0][3], &set, 4);
            envTris.push_back(t);
            ++statEnvTris;
            return -static_cast<int>(envTris.size());
        }
        if (!r[0]->lit || !r[1]->lit || !r[2]->lit)
            return 0;
        int set = -1;
        if (lastLitSet >= 0 && lastLitSerial == r[0]->serial)
            set = lastLitSet;
        else if (const auto it = lightSetOf.find(r[0]->serial); it != lightSetOf.end())
            set = it->second;
        else
        {
            GpuLightSet s;
            std::memcpy(s.l, r[0]->light, sizeof(s.l));
            std::memcpy(s.amb, r[0]->ambient, sizeof(s.amb));
            std::memcpy(s.c, r[0]->lightColour, sizeof(s.c));
            std::memcpy(s.clampv, r[0]->lightClamp, sizeof(s.clampv));
            s.pw[0] = s.pw[1] = 1.0f;
            s.pw[2] = s.pw[3] = r[0]->lightPow;
            set = static_cast<int>(lightSets.size());
            lightSets.push_back(s);
            lightSetOf.emplace(r[0]->serial, set);
        }
        lastLitSerial = r[0]->serial;
        lastLitSet = set;
        GpuLitTri t;
        for (int i = 0; i < 3; ++i)
        {
            for (int l = 0; l < 3; ++l)
                t.n[i][l] = cn[i].nrm[l];
            t.n[i][3] = 0.0f;
        }
        std::memcpy(&t.n[0][3], &set, 4);
        litTris.push_back(t);
        ++statLitTris;
        return static_cast<int>(litTris.size());
    }
#endif
    // Before a batch's draw (GT4_CAR_LIGHTING=pixel): its lit triangles and light sets at bindings 4 and 5, its
    // reflection triangles and sets at 6 and 7 (a small empty buffer where it has none: every program of this kind
    // declares them; most batches are not cars).
    GLuint litEmptyBuffer = 0;
    void bindLit()
    {
        if (!litEmptyBuffer)
        {
            const std::vector<uint8_t> zeros(256u, 0u);
            glGenBuffers(1, &litEmptyBuffer);
            glBindBuffer(GL_SHADER_STORAGE_BUFFER, litEmptyBuffer);
            glBufferData(GL_SHADER_STORAGE_BUFFER, 256, zeros.data(), GL_STATIC_DRAW);
        }
        const auto upload = [this](GLuint &buf, const void *data, size_t bytes, GLuint binding) {
            if (!bytes)
            {
                glBindBufferBase(GL_SHADER_STORAGE_BUFFER, binding, litEmptyBuffer);
                return;
            }
            if (!buf)
                glGenBuffers(1, &buf);
            glBindBuffer(GL_SHADER_STORAGE_BUFFER, buf);
            glBufferData(GL_SHADER_STORAGE_BUFFER, static_cast<GLsizeiptr>(bytes), data, GL_STREAM_DRAW);
            glBindBufferBase(GL_SHADER_STORAGE_BUFFER, binding, buf);
        };
        upload(litTriBuffer, litTris.data(), litTris.size() * sizeof(GpuLitTri), 4);
        upload(lightSetBuffer, lightSets.data(), lightSets.size() * sizeof(GpuLightSet), 5);
        upload(envTriBuffer, envTris.data(), envTris.size() * sizeof(GpuEnvTri), 6);
        upload(envSetBuffer, envSets.data(), envSets.size() * sizeof(GpuEnvSet), 7);
    }
    void clearLit()
    {
        litTris.clear();
        lightSets.clear();
        lightSetOf.clear();
        envTris.clear();
        envSets.clear();
        envSetOf.clear();
        lastLitSet = lastEnvSet = -1;
    }
    // The draw program built with GS_INTERLOCK (ARB_fragment_shader_interlock):
    // blending batches that write no depth are drawn in one draw with it.
    // Fast mode (GT3_GS_GL_FAST=1/0; default at 3x and above, the user's
    // choice: high internal resolutions may be "looks the same" instead of
    // bit-exact). Native and 2x stay exact.
    bool fastBlend = false; // blending on the GPU's blend unit where it can
    GLuint fastProgram = 0;         // GS_FAST_RASTER (see init)
    GLuint specTex15Program = 0;    // fastProgram's lean variant (GS_SPEC_TEX15)
    GLuint hwTriProgram = 0;        // ... for triangle-only batches, attributes interpolated by the GPU (GS_HW_TRI)
    GLuint hwTriNoAtProgram = 0;    // ... and no alpha test: early depth tests (GS_NO_ALPHA_TEST)
    bool fastRasterActive = false;  // fast mode draws with fastProgram
    GLuint interlockProgram = 0, interlockFbo = 0; // interlockFbo: no attachments
    bool hasInterlock = false, onInterlockFbo = false;
    int curProgram = 0; // 0: program, 1: interlockProgram (the bound one while drawing)
    uint64_t statInterlockBatches = 0, statInterlockPrims = 0;
    GLuint dstScratch = 0, texScratch = 0;
    int texScratchW = 0, texScratchH = 0; // texScratch's size in samples
    // Draws that read the target they draw into sample texScratch, a copy of
    // its rows. Consecutive such primitives of one batch share the copy
    // (until the batch is drawn), as the GS texture cache keeps the texels it
    // loaded: GT3's replay heat-haze band is ~1,500 triangles reading the
    // display buffer, each one had its own copy and batch (~17,000 draws a
    // frame, 30 fps).
    // Sharing the copy is the GS's own texture-cache behaviour (no TEXFLUSH in
    // between), not the CPU renderer's (which reads local memory afresh per
    // primitive): accumulating effects such as GT3's wheel blur in the rally
    // demo come out slightly less accumulated than the CPU reference, while
    // the license demo's heat haze is identical. Only 32-bit views laid out as
    // the target (texel = pixel) share. GT3_GS_GL_SELF_COPY_EACH=1 restores
    // the reference behaviour (for GL-vs-CPU compare runs).
    // After a TEXFLUSH a primitive still reuses the copy if the texels it can
    // read lie in no 8x8 cell written before that TEXFLUSH (those cells are
    // the same in the copy and the target). GT3's replay heat-haze particles
    // (4 triangles each, sampling the screen where they are) end with a
    // TEXFLUSH each: ~230,000 full-target copies per 10 s before this.
    // GT3_GS_GL_SELF_SHARE_SAFE=1: reuse only if the cells were not written
    // at all since the copy.
    struct SelfSnapshot
    {
        bool valid = false;
        int target = -1, firstRow = 0, lastRow = -1;
        uint64_t texFlush = 0; // texFlushSerial when the copy was taken
        int cellsW = 0, cellsH = 0;
        // 8x8-pixel cells of the target: 0 unwritten since the copy, else 1 +
        // the number of TEXFLUSHes between the copy and the cell's first write.
        std::vector<uint32_t> written;
        void mark(int x0, int y0, int x1, int y1, uint32_t tag)
        {
            const int cx0 = std::max(x0 >> 3, 0), cy0 = std::max(y0 >> 3, 0);
            const int cx1 = std::min(x1 >> 3, cellsW - 1), cy1 = std::min(y1 >> 3, cellsH - 1);
            for (int cy = cy0; cy <= cy1; ++cy)
            {
                uint32_t *row = written.data() + static_cast<size_t>(cy) * static_cast<size_t>(cellsW);
                for (int cx = cx0; cx <= cx1; ++cx)
                    if (row[cx] == 0u)
                        row[cx] = tag;
            }
            if (cx1 >= cx0 && cy1 >= cy0)
                markDirty(cx0 * 8, cy0 * 8, cx1 * 8 + 7, cy1 * 8 + 7);
        }
        // Whether a cell of the box was first written with a tag below `limit`.
        bool touched(int x0, int y0, int x1, int y1, uint32_t limit) const
        {
            const int cx0 = std::max(x0 >> 3, 0), cy0 = std::max(y0 >> 3, 0);
            const int cx1 = std::min(x1 >> 3, cellsW - 1), cy1 = std::min(y1 >> 3, cellsH - 1);
            for (int cy = cy0; cy <= cy1; ++cy)
            {
                const uint32_t *row = written.data() + static_cast<size_t>(cy) * static_cast<size_t>(cellsW);
                for (int cx = cx0; cx <= cx1; ++cx)
                    if (row[cx] != 0u && row[cx] < limit)
                        return true;
            }
            return false;
        }
        uint32_t tag(uint64_t serial) const { return static_cast<uint32_t>(std::min<uint64_t>(serial - texFlush, 0xFFFFFFF0ull)) + 1u; }
        // Box of the cells written since the copy (pixels; x1 < x0: none).
        // Only they differ from the target.
        int dirtyX0 = 0, dirtyY0 = 0, dirtyX1 = -1, dirtyY1 = -1;
        void markDirty(int x0, int y0, int x1, int y1)
        {
            x0 = std::max(x0, 0) & ~7;
            y0 = std::max(y0, 0) & ~7;
            x1 |= 7;
            y1 |= 7;
            if (x1 < x0 || y1 < y0)
                return;
            if (dirtyX1 < dirtyX0)
            {
                dirtyX0 = x0; dirtyY0 = y0; dirtyX1 = x1; dirtyY1 = y1;
                return;
            }
            dirtyX0 = std::min(dirtyX0, x0); dirtyY0 = std::min(dirtyY0, y0);
            dirtyX1 = std::max(dirtyX1, x1); dirtyY1 = std::max(dirtyY1, y1);
        }
    } selfSnap;
    uint64_t texFlushSerial = 0;
    uint64_t statSelfCopies = 0, statSelfReuses = 0;
    uint64_t statSelfPartial = 0; // copies of only the cells a self-read reads
    unsigned long long statSelfWhy[7] = {}; // why a self-reading primitive took a new copy (stats line)
    uint64_t statSelfBanded = 0; // self-reads drawn in bands (they read rows they write)
    uint64_t statSelfBandsShared = 0; // bands drawn from the copy of an earlier band (GT4: rows read not written since)
    // GL object churn (GT3_GS_GL_STATS; analysis/phase5/specs/step4-hitches.md D3):
    // textures made and deleted, targets made and grown, colour/depth attachments
    // of the draw and page-move framebuffers and how many of those were a
    // texture's first attachment (the race-view and B-Spec hitches sat in the
    // driver under these attachments).
    static inline uint64_t statTexMade = 0;
    uint64_t statTexDeleted = 0, statTargetsMade = 0, statTargetGrows = 0, statAttaches = 0, statFirstAttaches = 0;
    std::unordered_set<GLuint> attachedOnce;
    // drawOrMove: an attachment to the draw or page-move framebuffer (counted);
    // false: a read-back or blit framebuffer (only a first attachment counts).
    void noteAttach(GLuint tex, bool drawOrMove = true)
    {
        if (!stats)
            return;
        if (drawOrMove)
            ++statAttaches;
        if (tex && attachedOnce.insert(tex).second)
            ++statFirstAttaches;
    }
    // GT3_GS_GL_SELF_BAND=<rows> (default 8; 0: one snapshot per draw, as before).
    int selfBandRows = [] {
        const char *v = std::getenv("GT3_GS_GL_SELF_BAND");
        return (v && *v) ? std::max(0, std::atoi(v)) : 8;
    }();
    // Internal resolution (GT3_GS_GL_SCALE, GT3_GS_GL_PROGRESSIVE): every
    // target holds scaleX x scaleY samples per GS pixel. Pages move between
    // targets sample for sample; the CPU copy gets one sample of each pixel
    // (read backs) and gives its value to all of them (uploads). 1 x 1 is the
    // native renderer, exact.
    int scaleX = 1, scaleY = 1;
    bool progressive = false;
    GLint maxTextureSize = 0;
    bool scaled() const { return scaleX != 1 || scaleY != 1; }
    // Effects at their own scale (GT4_EFFECTS_SCALE, analysis/design/effects-scale/DESIGN.md): effect targets at
    // effectScale samples per GS pixel (both axes; 0: off, every texture at scaleX x scaleY as always). While on,
    // every target texture is listed with its scale, and a scratch / CLUT / field texture a copy wrote with the
    // scale of what it holds; a texture not listed is at scaleX x scaleY.
    int effectScale = 0;
    // GT4's car view at full height in its 480p menus (gs_car_view.h; CAR-PICTURE.md section 8): while on, the car
    // view's 224-row targets (Target::tall) hold scaleX x 2 scaleY samples per GS pixel and every target texture is
    // listed with its scale, as with the effects scale (the reads across scales are the same). On at 2x-4x pictures
    // (not native, not progressive, not DLSS's HUD mode) when gt4carview::allowed().
    bool tallOn = false;
    bool tallArmedSeen = false; // the last gt4carview::armed() a draw saw (a change resets the draw-state runs)
    uint64_t statTallTargets = 0;
    bool scalesListed() const { return effectScale != 0 || tallOn; }
    // GT4_MENU_480P_CAR_TALL (gs_car_view.h) and the D5 car picture possible at all (GT4's 480p menus): the draw
    // programs then read targets at their own scales (GS_EFFECTS_SCALE).
    static bool tallPossible()
    {
#if defined(PS2X_GAME_GT4) && !defined(GT4_STOCK_BUILD)
        static const bool on = [] {
            if (!gt4carview::allowed())
                return false;
            auto is = [](const char *name, const char *value) {
                const char *v = std::getenv(name);
                if (!v)
                    return false;
                std::string m = v;
                for (char &c : m)
                    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                return m == value;
            };
            const char *car = std::getenv("GT4_MENU_480P_CAR");
            return !is("GT4_RACE_MODE", "game") && !is("GT4_MENU_MODE", "480i") && !(car && *car && std::atoi(car) <= 0);
        }();
        return on;
#else
        return false;
#endif
    }
    // GT4's DLSS with the HUD drawn after it (gt4_upscale.h; GT4_UPSCALER=dlss, GT4_DLSS_HUD unset or 1, Vulkan): the
    // picture's scale (scaleX) is DLSS's output and every target but the shown buffers stays at the requested scale
    // (dlssHudRender, through the effects scale; depth targets too, and nothing is promoted by depth draws). The shown
    // buffers are promoted at presentation, so GT4's HUD and menus are drawn into them at the output's scale, and
    // the 3D buffer's copy into them is followed by DLSS writing the upscaled 3D over it (dlssApply). 0: off.
    int dlssHudRender = 0;
    // With DLSS (gt4up::scales): the scale the renderer draws at, and the picture's scale DLSS makes (dlssPlanned,
    // DLSS at presentation; applyPresentScale). `requested` is GT3_GS_GL_SCALE.
    int dlssPlanned = 0;
    int hudOutputScale(int requested)
    {
        dlssHudRender = 0;
        dlssPlanned = 0;
#if defined(VKGL_REDIRECT) && defined(PS2X_GAME_GT4) && !defined(GT4_STOCK_BUILD)
        int draw = requested, out = 0;
        if (!gt4up::scales(requested, draw, out))
            return requested;
        // (opt-in for now, GT4_DLSS_HUD=1: night Clubman 52.3 fps against 58.6 with DLSS at presentation, and some HUD
        // pieces went missing; analysis/design/lighting/dlss-step2)
        if (out > draw && envFlag("GT4_DLSS_HUD", false))
        {
            dlssHudRender = draw;
            std::fprintf(stderr, "[dlss] the picture at %dx, its 3D drawn at %dx and upscaled by DLSS before GT4's HUD\n", out, draw);
            return out;
        }
        dlssPlanned = out;
        return draw;
#else
        return requested;
#endif
    }
    // The scale the 3D buffer is drawn at (the motion pass, the jitter).
    int sceneScaleX() const { return dlssHudRender && effectScale ? effectScale : scaleX; }
    int sceneScaleY() const { return dlssHudRender && effectScale ? effectScale * (scaleY / std::max(scaleX, 1)) : scaleY; }
    struct TexScale
    {
        int x = 1, y = 1;
        bool operator==(const TexScale &o) const { return x == o.x && y == o.y; }
        bool operator!=(const TexScale &o) const { return !(*this == o); }
    };
    std::unordered_map<GLuint, TexScale> texScales;
    std::unordered_set<GLuint> targetTexs; // (effects on) every target's texture
    TexScale pictureScale() const { return {scaleX, scaleY}; }
    TexScale scaleOfTex(GLuint tex) const
    {
        if (texScales.empty())
            return pictureScale();
        const auto it = texScales.find(tex);
        return it == texScales.end() ? pictureScale() : it->second;
    }
    void setTexScale(GLuint tex, TexScale s)
    {
        if (!tex)
            return;
        if (s == pictureScale())
            texScales.erase(tex);
        else
            texScales[tex] = s;
    }
    static int packScale(TexScale s) { return (s.x & 15) | ((s.y & 15) << 4); }
    // A target at the effects scale found to be a scene target (drawn with a depth buffer, displayed): its texture
    // re-made at the picture's scale from its contents (each sample repeated), its base page remembered.
    uint64_t statPromotions = 0;
    void promoteTarget(int id, const char *why) { promoteTargetImpl(id, why); }
    // The presentation slots' samples per GS pixel across and per display line (with DLSS: its output's).
    int slotScaleX() const
    {
#if defined(VKGL_REDIRECT) && defined(PS2X_GAME_GT4) && !defined(GT4_STOCK_BUILD)
        if (dlssOut)
            return dlssOut;
#endif
        return scaleX;
    }
    int slotRows() const
    {
#if defined(VKGL_REDIRECT) && defined(PS2X_GAME_GT4) && !defined(GT4_STOCK_BUILD)
        if (dlssOut)
            return outRows * dlssOut / scaleX;
#endif
        return outRows;
    }
    // The presentation slots' scale: the picture's, or with DLSS its output's (GT4's Vulkan renderer).
    void applyPresentScale()
    {
#if defined(VKGL_REDIRECT) && defined(PS2X_GAME_GT4) && !defined(GT4_STOCK_BUILD)
        dlssOut = 0;
        if (dlssHudRender)
        {
            gsgl::setPresentScale(scaleX, outRows);
            return;
        }
        const int out = dlssPlanned;
        if (out > 0 && (out * scaleY) % scaleX == 0 && (out * outRows) % scaleX == 0)
            dlssOut = out;
        else if (out > 0)
            std::fprintf(stderr, "[dlss] output %dx from %dx x %d rows: not whole samples per GS pixel; DLSS off\n", out, scaleX, outRows);
        if (dlssOut)
        {
            gsgl::setPresentScale(dlssOut, outRows * dlssOut / scaleX);
            std::fprintf(stderr, "[dlss] the picture at %dx (drawn at %dx)\n", dlssOut, scaleX);
            return;
        }
#endif
        gsgl::setPresentScale(scaleX, outRows);
    }
#if defined(PS2X_GAME_GT4) && !defined(GT4_STOCK_BUILD)
    // GT4_SHOT_DIR=<folder> with GT4_SHOT_EVERY=<n> (checking; e.g. DLSS on / off side by side from the same replay):
    // the shown buffer as it is presented (HUD and all, at the picture's scale) every n presentations, as PNG.
    uint64_t shotPresents = 0;
    bool shotFromSlot = false;
    void shotAtPresent(int id, int ox, int oy, int width, int rows)
    {
        static const char *dir = std::getenv("GT4_SHOT_DIR");
        static const uint64_t every = [] {
            const char *v = std::getenv("GT4_SHOT_EVERY");
            return v && *v ? std::strtoull(v, nullptr, 10) : 0ull;
        }();
        // (GT4_SHOT_FROM / GT4_SHOT_TO=<presentation>: only between those)
        static const uint64_t from = std::getenv("GT4_SHOT_FROM") ? std::strtoull(std::getenv("GT4_SHOT_FROM"), nullptr, 10) : 0ull;
        static const uint64_t to = std::getenv("GT4_SHOT_TO") ? std::strtoull(std::getenv("GT4_SHOT_TO"), nullptr, 10) : ~0ull;
        ++shotPresents;
        if (!dir || !*dir || !every || (shotPresents % every) != 0u || shotPresents < from || shotPresents > to)
            return;
#if defined(VKGL_REDIRECT)
        if (dlssOut) // (DLSS at presentation: the picture is DLSS's output, taken in dlssPresent)
        {
            shotFromSlot = true;
            return;
        }
#endif
        flushBatch();
        // (the target the presentation reads: GT4's 480p buffers are 16-bit, beside a 32-bit target at the same base)
        if (id >= 0 && id < static_cast<int>(targets.size()))
            if (const auto &tp = targets[id]; tp && !tp->depth && tp->tex)
            {
                const int texW = tp->width * tp->sx, texH = tp->height * tp->sy;
                const int cw = std::min(width * scaleX, texW - ox * scaleX), ch = std::min(rows * scaleY, texH - oy * scaleY);
                if (cw <= 0 || ch <= 0)
                    return;
                std::vector<uint8_t> all(static_cast<size_t>(texW) * static_cast<size_t>(texH) * 4u), crop(static_cast<size_t>(cw) * ch * 4u);
                glBindTexture(GL_TEXTURE_2D, tp->tex);
                glGetTexImage(GL_TEXTURE_2D, 0, GL_RGBA, GL_UNSIGNED_BYTE, all.data());
                glBindTexture(GL_TEXTURE_2D, 0);
                for (int y = 0; y < ch; ++y)
                    std::memcpy(crop.data() + static_cast<size_t>(y) * cw * 4u,
                                all.data() + (static_cast<size_t>(oy * scaleY + y) * texW + static_cast<size_t>(ox * scaleX)) * 4u, static_cast<size_t>(cw) * 4u);
                for (size_t i = 3; i < crop.size(); i += 4)
                    crop[i] = 255;
                char path[1024];
                std::snprintf(path, sizeof(path), "%s/shot-%06llu.png", dir, static_cast<unsigned long long>(shotPresents));
                gt4tap::savePng(path, cw, ch, crop.data());
                forgetBindings();
                return;
            }
    }
#endif
#if defined(PS2X_GAME_GT4) && !defined(GT4_STOCK_BUILD)
    // Lighting step 2d (GT4_AO=1; =show: the occlusion alone, grey): ambient occlusion over GT4's 3D, from its own
    // depth. Each frame the tapped triangles that write Z into a full-width buffer give the 3D buffer and its depth
    // buffer, GS Z against Q (Z = A + B Q, Q = 1 / w: a least-squares fit, so a pixel's distance is B / (Z - A)) and
    // the projection (focal lengths: the viewport scale times the matrix's x / y column over its w column; the
    // centre: the viewport offset). When the frame's 3D is done (the first draw reading the 3D buffer into another,
    // or the next VU1 frame's first triangle), a compute pass makes the occlusion (SAO: 12 taps on a spiral within a
    // radius in GT4's units, normals from the depth) and a second blurs it (4x4, depth-aware) and darkens the 3D
    // buffer's colour by it, before GT4's post effects and HUD.
    bool aoActive = false;
    uint64_t aoFrame = 0;
    uint32_t aoFbp = 0, aoFbw = 1, aoZbp = 0;
    int aoRows = 0, aoScissorRows = 0;
    // An inset view drawn into the 3D buffer with its own projection (the rear-view mirror: a scissor under 160 rows):
    // left out of the fit and the projection, and its rectangle (frame pixels, inclusive) left alone by the passes.
    int aoSkip[4] = {0, 0, -1, -1};
    // The distance pass (lighting steps 2d and 3): the frame's Z-writing tapped triangles (each corner: frame x, y, GS Z
    // (/ 2^24), Q), split where GT4's depth partition changes (zMapping's B: far scenery, then nearer, each after a Z
    // clear), drawn at the end in that order (the depth cleared between partitions, each with its own GS Z test) into a
    // texture of distances (1 / Q, GT4's units; 0: nothing tapped, the sky). GS Z alone cannot say how far a pixel is.
    std::vector<float> distVerts;
    std::vector<uint32_t> distParts; // the first triangle of each partition
    int distPartition = 99;
    GLuint distProgram = 0, distFbo = 0, distBuffer = 0, distTex = 0, distDepth = 0;
    int distW = 0, distH = 0;
    GLint distLoc = -1;
    uint64_t statDistParts = 0;
    // Relighting: GT4's depth buffer as it stood at each Z clear after the frame's first partition (the far scenery's,
    // then the nearer), so the distance pass keeps only what GT4 drew: each partition's triangles where that
    // partition's GS Z equals theirs (not the see-through parts of alpha-tested cards: trees, fences). distSnap: per
    // partition, the snapshot holding its Z (-1: the depth buffer at the end, the last partition's).
    static constexpr int kRelightSnaps = 3;
    GLuint relightSnap[kRelightSnaps] = {};
    int relightSnapW = 0, relightSnapH = 0, relightSnaps = 0;
    std::vector<int> distSnap;
    GLint distVerifyLoc[4] = {-1, -1, -1, -1};
    uint64_t statRelightClears = 0, statRelightClearsMissed = 0;
    void aoNoteClear(const GSPrimitiveBatch &batch)
    {
        if (!aoActive || !sunLit() || batch.vertexCount != 2u || batch.state.prim.type != GS_PRIM_SPRITE ||
            (batch.vertexCount && batch.vertices[0].tapSlot))
            return;
        const GSContext &ctx = batch.state.context;
        const uint32_t test = static_cast<uint32_t>(ctx.test);
        if (ctx.zbuf.zbp != aoZbp || ctx.zbuf.zmask || ((test >> 16) & 1u) == 0u || ((test >> 17) & 3u) != 1u)
            return; // (a Z clear: Z written, the test always passing)
        if (std::fabs(batch.vertices[1].y - batch.vertices[0].y) < 160.0f) // (not an inset view's: the mirror's)
            return;
        if (distVerts.size() / 12u <= distParts.back() || relightSnaps >= kRelightSnaps)
        {
            ++statRelightClearsMissed;
            return;
        }
        flushBatch();
        const int did = findTarget(true, Layout::Z32, aoZbp, aoFbw, std::max(aoScissorRows, 1));
        if (did < 0 || !targets[did]->tex)
            return;
        const Target &dt = *targets[did];
        // (the 3D's rows only: depth targets are full height)
        const int tw = std::min(dt.width, static_cast<int>(aoFbw * 64u)) * dt.sx;
        const int th = std::min(dt.height, std::max(aoScissorRows, 1)) * dt.sy;
        if (tw > relightSnapW || th > relightSnapH)
        {
            for (GLuint &t : relightSnap)
                releaseTexture(t);
            relightSnapW = std::max(relightSnapW, tw);
            relightSnapH = std::max(relightSnapH, th);
            for (GLuint &t : relightSnap)
                t = makeTargetTexture(true, relightSnapW, relightSnapH);
        }
        const int k = relightSnaps++;
        glCopyImageSubData(dt.tex, GL_TEXTURE_2D, 0, 0, 0, 0, relightSnap[k], GL_TEXTURE_2D, 0, 0, 0, 0, tw, th, 1);
        distSnap.resize(distParts.size(), -1);
        for (int &s : distSnap)
            if (s < 0)
                s = k;
        // (what follows is a new partition, whatever its depth mapping)
        distParts.push_back(static_cast<uint32_t>(distVerts.size() / 12u));
        distSnap.push_back(-1);
        distPartition = 99;
        ++statRelightClears;
    }
    struct FitGroup
    {
        uint64_t n = 0;
        double a = 0.0, b = 0.0, zMin = 1e30, zMax = -1e30, wMin = 1e30, wMax = -1e30;
    };
    std::map<long long, FitGroup> fitGroups;
    uint64_t fitTris = 0;
    double aoN = 0.0, aoSq = 0.0, aoSz = 0.0, aoSqq = 0.0, aoSqz = 0.0;
    float aoProj[4] = {};
    bool aoHaveProj = false;
    GLuint aoProgram = 0, aoApplyProgram = 0, aoTex = 0;
    int aoTexW = 0, aoTexH = 0;
    GLint aoLoc[9] = {}, aoApplyLoc[9] = {};
    GLint relightLoc[2][7] = {{-1, -1, -1, -1, -1, -1, -1}, {-1, -1, -1, -1, -1, -1, -1}}; // (the occlusion and apply programs')
    bool relightLocs = false;
    uint64_t statRelightLogs = 0;
    bool aoBroken = false;
    uint64_t statAoFrames = 0, statAoSkipped = 0;
    static int aoMode()
    {
        static const int m = [] {
            const char *v = std::getenv("GT4_AO");
            if (!v || !*v || std::strcmp(v, "0") == 0 || std::strcmp(v, "off") == 0)
                return 0;
            return std::strcmp(v, "show") == 0 ? 2 : std::strcmp(v, "depth") == 0 ? 3 : std::strcmp(v, "normals") == 0 ? 4
                 : std::strcmp(v, "split") == 0 ? 5 : 1;
        }();
        return m;
    }
    // Relighting (GT4_RELIGHT=<0-100>, off by default; the Full edition's relighting step 2, graphics-options/RELIGHTING-PLAN.md):
    // the 3D outside the cars shaded by GT4's own sun (light 0 of a car body's light set, gt4tap::sun) from the surface's
    // normal (the occlusion pass's, from the distances): brighter facing the sun, darker facing away, a level road as GT4
    // lit it. The strength over 100; runs in the occlusion's apply pass (with GT4_AO off the occlusion itself is left out).
    static float relightStrength()
    {
        static const float s = [] {
            const char *v = std::getenv("GT4_RELIGHT");
            if (!v || !*v || std::strcmp(v, "0") == 0 || std::strcmp(v, "off") == 0)
                return 0.0f;
            const float k = std::strcmp(v, "on") == 0 ? 40.0f : static_cast<float>(std::atof(v));
            return std::clamp(k, 0.0f, 100.0f) / 100.0f;
        }();
        return s;
    }
    // Sun shadows (GT4_SHADOWS=<0-100>, off by default; relighting step 3): the cars' shadows from GT4's sun on what is
    // relit (the track and scenery), screen-space: from each pixel a march toward the sun through the distances,
    // shadowed where it passes inside a car. The strength over 100 (how dark: 100 takes 60% of the light).
    static float shadowStrength()
    {
        static const float s = [] {
            const char *v = std::getenv("GT4_SHADOWS");
            if (!v || !*v || std::strcmp(v, "0") == 0 || std::strcmp(v, "off") == 0)
                return 0.0f;
            const float k = std::strcmp(v, "on") == 0 ? 60.0f : static_cast<float>(std::atof(v));
            return std::clamp(k, 0.0f, 100.0f) / 100.0f;
        }();
        return s;
    }
    // Sun glow (GT4_SUN_GLOW=<0-100>, relighting step 4): a warm glow round GT4's sun on the sky (the first depth
    // partition, or nothing drawn there). Bloom (GT4_BLOOM=<0-100>): the 3D's brightest parts glow onto their
    // surroundings (a quarter-size bright pass, blurred, added back), the inset mirror left alone.
    static float envStrength(const char *name, float onValue)
    {
        const char *v = std::getenv(name);
        if (!v || !*v || std::strcmp(v, "0") == 0 || std::strcmp(v, "off") == 0)
            return 0.0f;
        const float k = std::strcmp(v, "on") == 0 ? onValue : static_cast<float>(std::atof(v));
        return std::clamp(k, 0.0f, 100.0f) / 100.0f;
    }
    static float glowStrength()
    {
        static const float s = envStrength("GT4_SUN_GLOW", 50.0f);
        return s;
    }
    static float bloomStrength()
    {
        static const float s = envStrength("GT4_BLOOM", 40.0f);
        return s;
    }
    static bool sunLit() { return relightStrength() > 0.0f || shadowStrength() > 0.0f || glowStrength() > 0.0f; } // (GT4's sun)
    void aoNote(const GSPrimitiveBatch &batch)
    {
        if (batch.vertexCount != 3u || batch.state.prim.type == GS_PRIM_SPRITE)
            return;
        uint64_t frame = 0;
        if (!gt4tap::markFrame(batch.vertices[0].tapSlot, frame))
            return;
        if (aoActive && frame != aoFrame)
            aoApply();
        const GSContext &ctx = batch.state.context;
        const bool writesZ = ((ctx.test >> 16) & 1u) != 0u && !ctx.zbuf.zmask && ctx.zbuf.psm == GS_PSM_Z24; // (a Z24 scene depth only)
        if (!writesZ || ctx.frame.fbw < 8u) // (the 3D buffer: full width; not the reflection map's 128 x 256)
            return;
        if (aoActive && ctx.frame.fbp != aoFbp)
            return;
        if (!aoActive)
        {
            aoActive = true;
            aoFrame = frame;
            aoFbp = ctx.frame.fbp;
            aoFbw = ctx.frame.fbw;
            aoZbp = ctx.zbuf.zbp;
            aoRows = aoScissorRows = 0;
            aoN = aoSq = aoSz = aoSqq = aoSqz = 0.0;
            aoHaveProj = false;
            aoSkip[0] = aoSkip[1] = 0;
            aoSkip[2] = aoSkip[3] = -1;
            distVerts.clear();
            distParts.assign(1, 0u);
            distPartition = 99;
            distSnap.assign(1, -1);
            relightSnaps = 0;
        }
        const int sx0 = static_cast<int>(ctx.scissor.x0), sy0 = static_cast<int>(ctx.scissor.y0);
        const int sx1 = static_cast<int>(ctx.scissor.x1), sy1 = static_cast<int>(ctx.scissor.y1);
        if (sy1 - sy0 + 1 < 160)
        {
            if (aoSkip[2] < aoSkip[0])
            {
                aoSkip[0] = sx0;
                aoSkip[1] = sy0;
                aoSkip[2] = sx1;
                aoSkip[3] = sy1;
            }
            else
            {
                aoSkip[0] = std::min(aoSkip[0], sx0);
                aoSkip[1] = std::min(aoSkip[1], sy0);
                aoSkip[2] = std::max(aoSkip[2], sx1);
                aoSkip[3] = std::max(aoSkip[3], sy1);
            }
            return;
        }
        aoScissorRows = std::max(aoScissorRows, sy1 + 1);
        const float ofx = ctx.xyoffset.ofx / 16.0f, ofy = ctx.xyoffset.ofy / 16.0f;
        {
            double zA = 0.0, zB = 0.0;
            if (gt4tap::zMapping(batch.vertices[0].tapSlot, zA, zB) && zB > 0.0)
            {
                const int part = static_cast<int>(std::lround(std::log10(zB)));
                if (part != distPartition)
                {
                    if (distPartition != 99 && distVerts.size() / 12u > distParts.back())
                        distParts.push_back(static_cast<uint32_t>(distVerts.size() / 12u));
                    distPartition = part;
                }
            }
            // (relighting: a car's triangle with its Q negated, so its distances carry the sign bit: the cars keep their own lighting)
            const float qSign = sunLit() && gt4tap::carMark(batch.vertices[0].tapSlot) ? -1.0f : 1.0f;
            if (batch.vertices[0].q > 0.0f && batch.vertices[1].q > 0.0f && batch.vertices[2].q > 0.0f && distVerts.size() < 6000000u)
                for (int i = 0; i < 3; ++i)
                {
                    const GSVertex &v = batch.vertices[i];
                    distVerts.insert(distVerts.end(), {v.x - ofx, v.y - ofy, static_cast<float>(static_cast<double>(v.z) / 16777216.0), qSign * v.q});
                }
        }
        // (GT4_AO_FITLOG=1, checking: each triangle's own Z = A + B Q from its corners, grouped, logged now and then)
        static const bool fitLog = [] {
            const char *v = std::getenv("GT4_AO_FITLOG");
            return v && *v == '1';
        }();
        if (fitLog)
        {
            const GSVertex &a = batch.vertices[0], &b = batch.vertices[1], &c = batch.vertices[2];
            const GSVertex *lo = &a, *hi = &a;
            for (const GSVertex *v : {&b, &c})
            {
                if (v->q < lo->q)
                    lo = v;
                if (v->q > hi->q)
                    hi = v;
            }
            if (hi->q > lo->q * 1.05f && lo->q > 0.0f)
            {
                const double B = (static_cast<double>(hi->z) - lo->z) / 16777216.0 / (static_cast<double>(hi->q) - lo->q);
                const double A = lo->z / 16777216.0 - B * lo->q;
                const long long key = std::llround(B * 2000.0) * 1000000ll + std::llround(A * 20000.0);
                FitGroup &g = fitGroups[key];
                ++g.n;
                g.a += A;
                g.b += B;
                g.zMin = std::min(g.zMin, std::min(lo->z, hi->z) / 16777216.0);
                g.zMax = std::max(g.zMax, std::max(lo->z, hi->z) / 16777216.0);
                g.wMin = std::min(g.wMin, 1.0 / hi->q);
                g.wMax = std::max(g.wMax, 1.0 / lo->q);
            }
            if (++fitTris % 400000u == 0u)
            {
                std::vector<std::pair<uint64_t, long long>> order;
                for (const auto &[k, g] : fitGroups)
                    order.push_back({g.n, k});
                std::sort(order.rbegin(), order.rend());
                std::fprintf(stderr, "[gt4-ao] Z = A + B Q per triangle, the largest groups of %llu triangles:\n", static_cast<unsigned long long>(fitTris));
                for (size_t i = 0; i < order.size() && i < 8; ++i)
                {
                    const FitGroup &g = fitGroups[order[i].second];
                    std::fprintf(stderr, "[gt4-ao]   %llu: A %.6f B %.6f, Z %.6f..%.6f, w %.1f..%.1f\n", static_cast<unsigned long long>(g.n),
                                 g.a / g.n, g.b / g.n, g.zMin, g.zMax, g.wMin, g.wMax);
                }
                fitGroups.clear();
            }
        }
        for (int i = 0; i < 3; ++i)
        {
            const GSVertex &v = batch.vertices[i];
            if (!(v.q > 0.0f))
                continue;
            const double q = v.q, z = static_cast<double>(v.z) / 16777216.0;
            aoN += 1.0;
            aoSq += q;
            aoSz += z;
            aoSqq += q * q;
            aoSqz += q * z;
            aoRows = std::max(aoRows, static_cast<int>(v.y - ofy) + 1);
        }
        if (!aoHaveProj && !(batch.vertices[0].tapSlot & gt4tap::kClipMark))
            if (const gt4tap::Record *r = gt4tap::record(batch.vertices[0].tapSlot - 1u))
            {
                float len[3];
                const int cols[3] = {0, 1, 3};
                for (int a = 0; a < 3; ++a)
                    len[a] = std::sqrt(r->m[0][cols[a]] * r->m[0][cols[a]] + r->m[1][cols[a]] * r->m[1][cols[a]] + r->m[2][cols[a]] * r->m[2][cols[a]]);
                if (len[2] > 0.0f && len[0] > 0.0f && len[1] > 0.0f)
                {
                    aoProj[0] = std::fabs(r->vs[0]) * len[0] / len[2];
                    aoProj[1] = std::fabs(r->vs[1]) * len[1] / len[2];
                    aoProj[2] = r->vo[0] - ofx;
                    aoProj[3] = r->vo[1] - ofy;
                    aoHaveProj = true;
                }
            }
    }
    // A draw about to read a texture: the end of the frame's 3D when it reads the 3D buffer into another.
    void aoNoteRead(const GSPrimitiveBatch &batch)
    {
        if (aoActive && batch.state.context.tex0.tbp0 == aoFbp * 32u && batch.state.context.frame.fbp != aoFbp)
            aoApply();
    }
    static std::string aoHead()
    {
        return R"GLSL(#version 430 core
layout(local_size_x = 8, local_size_y = 8) in;
uniform usampler2D u_depth;  // the 3D's distances (GT4's units, float bits; 0: nothing there, the sky): the distance pass
uniform ivec2 u_size;        // the 3D buffer's samples
uniform vec2 u_depthScale;   // depth samples per colour sample
uniform vec2 u_perPx;        // colour samples per GS pixel
uniform vec2 u_fit;          // Z = A + B / w
uniform vec4 u_proj;         // focal x, y; centre x, y (GS pixels)
uniform vec2 u_ao;           // radius (GT4's units), strength
uniform int u_show;          // checking: 1 the occlusion, 2 the distance, 3 the normals (as colours), 4 right half only
uniform ivec4 u_skip;        // an inset view's rectangle in samples (inclusive; x1 < x0: none): left alone
const float kFar = 1.0e9;
bool skipped(ivec2 s)
{
    return s.x >= u_skip.x && s.x <= u_skip.z && s.y >= u_skip.y && s.y <= u_skip.w;
}
float distanceAt(ivec2 s)
{
    uint v = texelFetch(u_depth, clamp(s, ivec2(0), u_size - 1), 0).r;
    return v == 0u ? kFar : uintBitsToFloat(v & 0x7fffffffu);
}
bool carAt(ivec2 s) // (relighting: a car's pixel, the sign bit of its distance)
{
    return (texelFetch(u_depth, clamp(s, ivec2(0), u_size - 1), 0).r & 0x80000000u) != 0u;
}
vec3 viewAt(ivec2 s, float w)
{
    vec2 gs = (vec2(s) + 0.5) / u_perPx;
    return vec3((gs.x - u_proj.z) * w / u_proj.x, (gs.y - u_proj.w) * w / u_proj.y, w);
}
// Relighting (GT4_RELIGHT): GT4's sun on the 3D outside the cars. The distances are only what GT4's depth buffer showed
// (checked partition by partition in the distance pass, which keeps the partition in the distance's two lowest bits).
uniform vec4 u_sun;          // GT4's sun (view space, as viewAt); w the strength (0: off)
uniform vec3 u_up;           // the world's up in view space
uniform uint u_relightFrom;  // the first depth partition relit (the sky's before it)
uniform float u_shadow;      // sun shadows (GT4_SHADOWS): the strength (0: off)
uniform float u_glow;        // sun glow (GT4_SUN_GLOW): the strength (0: off)
uint partitionAt(ivec2 s)
{
    return texelFetch(u_depth, clamp(s, ivec2(0), u_size - 1), 0).r & 3u;
}
bool relightable(ivec2 s)
{
    return partitionAt(s) >= u_relightFrom && !carAt(s);
}
// The surface's normal from its neighbours k samples away (the nearer side on each axis: none across silhouettes).
vec3 normalAt(ivec2 s, float w, int k)
{
    vec3 P = viewAt(s, w);
    ivec2 ex = ivec2(k, 0), ey = ivec2(0, k);
    float wl = distanceAt(s - ex), wr = distanceAt(s + ex), wu = distanceAt(s - ey), wd = distanceAt(s + ey);
    vec3 dx = abs(wr - w) < abs(w - wl) ? viewAt(s + ex, wr) - P : P - viewAt(s - ex, wl);
    vec3 dy = abs(wd - w) < abs(w - wu) ? viewAt(s + ey, wd) - P : P - viewAt(s - ey, wu);
    vec3 n = normalize(cross(dx, dy));
    return dot(n, P) > 0.0 ? -n : n;
}
// The sun's light on the surface against a level road's (normals a GS pixel across): brighter facing the sun, darker
// facing away (the sky still lights it: at most half the strength either way); GT4's own (baked) lighting kept as it is
// on a level road.
float relightFactor(ivec2 s, float w)
{
    if (u_sun.w <= 0.0 || w >= kFar || !relightable(s))
        return 1.0;
    vec3 n = normalAt(s, w, max(1, int(u_perPx.x + 0.5)));
    float lit = max(dot(n, u_sun.xyz), 0.0), level = clamp(dot(u_up, u_sun.xyz), 0.05, 0.95);
    return 1.0 + 0.5 * u_sun.w * (lit >= level ? (lit - level) / (1.0 - level) : (lit - level) / level);
}
// Sun shadows: from the surface toward GT4's sun (24 steps, closer together near it, a per-pixel start in the 4 x 4
// spin), shadowed where the march passes inside a car: a car's surface nearer the camera there, by under a car's depth,
// in the same depth partition. Screen-space: only what the screen shows of a car casts; not past 400 (too small there).
float sunShadow(ivec2 s, float w)
{
    if (u_shadow <= 0.0 || w >= 400.0 || !relightable(s))
        return 1.0;
    uint part = partitionAt(s);
    vec3 P = viewAt(s, w), L = normalize(u_sun.xyz);
    P += normalAt(s, w, max(1, int(u_perPx.x + 0.5))) * (0.002 * w); // (off the surface)
    float jitter = (float((s.x & 3) + 4 * (s.y & 3)) + 0.5) / 16.0;
    const int kSteps = 24;
    const float kReach = 40.0, kThick = 14.0; // (GT4's units: a car is about 10 wide)
    for (int i = 0; i < kSteps; ++i)
    {
        float x = (float(i) + jitter) / float(kSteps);
        vec3 Q = P + L * (kReach * x * x + 0.05);
        if (Q.z <= 0.5)
            break;
        ivec2 q = ivec2((Q.xy * u_proj.xy / Q.z + u_proj.zw) * u_perPx);
        if (q.x < 0 || q.y < 0 || q.x >= u_size.x || q.y >= u_size.y)
            break;
        if (!carAt(q) || partitionAt(q) != part)
            continue;
        float wq = distanceAt(q);
        if (wq < Q.z && wq > Q.z - kThick)
            return 1.0 - 0.6 * u_shadow;
    }
    return 1.0;
}
)GLSL";
    }
    bool aoBuild()
    {
        const std::string head = aoHead();
        const std::string occlusion = head + R"GLSL(
layout(r32ui, binding = 0) writeonly uniform uimage2D u_out;
void main()
{
    ivec2 s = ivec2(gl_GlobalInvocationID.xy);
    if (s.x >= u_size.x || s.y >= u_size.y)
        return;
    float w = distanceAt(s);
    float ao = 1.0;
    if (w < kFar && !skipped(s))
    {
        vec3 P = viewAt(s, w);
        // the surface's normal from its neighbours (the nearer side on each axis: no normals across silhouettes)
        float wl = distanceAt(s - ivec2(1, 0)), wr = distanceAt(s + ivec2(1, 0));
        float wu = distanceAt(s - ivec2(0, 1)), wd = distanceAt(s + ivec2(0, 1));
        vec3 dx = abs(wr - w) < abs(w - wl) ? viewAt(s + ivec2(1, 0), wr) - P : P - viewAt(s - ivec2(1, 0), wl);
        vec3 dy = abs(wd - w) < abs(w - wu) ? viewAt(s + ivec2(0, 1), wd) - P : P - viewAt(s - ivec2(0, 1), wu);
        vec3 n = normalize(cross(dx, dy));
        if (dot(n, P) > 0.0)
            n = -n;
        if (u_show == 3)
        {
            imageStore(u_out, s, uvec4(packUnorm4x8(vec4(n * 0.5 + 0.5, 1.0))));
            return;
        }
        float r = u_ao.x;
        float rS = r * u_proj.x / w * u_perPx.x; // the radius in samples
        if (rS >= 1.0 && u_ao.y > 0.0)
        {
            rS = min(rS, 80.0 * u_perPx.x);
            float spin = 6.2831853 * (float((s.x & 3) + 4 * (s.y & 3)) + 0.5) / 16.0;
            float sum = 0.0;
            for (int i = 0; i < 12; ++i)
            {
                float t = (float(i) + 0.5) / 12.0;
                float a = spin + t * 6.2831853 * 3.0;
                ivec2 q = clamp(s + ivec2(vec2(cos(a), sin(a)) * rS * t), ivec2(0), u_size - 1);
                float wq = distanceAt(q);
                if (wq >= kFar || skipped(q))
                    continue;
                vec3 v = viewAt(q, wq) - P;
                float vv = dot(v, v);
                float f = max(r * r - vv, 0.0);
                sum += f * f * f * max((dot(v, n) - 0.002 * w) / (0.01 * r * r + vv), 0.0);
            }
            ao = max(0.0, 1.0 - sum * u_ao.y * 5.0 / (12.0 * r * r * r * r * r * r));
        }
    }
    // (the occlusion in the low half, relighting's factor over 2 in the high)
    float f = skipped(s) ? 1.0 : relightFactor(s, w) * sunShadow(s, w);
    imageStore(u_out, s, uvec4(uint(clamp(ao, 0.0, 1.0) * 65535.0 + 0.5) | (uint(clamp(f * 0.5, 0.0, 1.0) * 65535.0 + 0.5) << 16)));
}
)GLSL";
        const std::string apply = head + R"GLSL(
uniform usampler2D u_aoTex;
layout(r32ui, binding = 1) uniform uimage2D u_color; // the 3D buffer's RGBA8 words
uniform vec4 u_sun;          // relighting: GT4's sun (view space, as viewAt); w the strength (0: off)
uniform vec3 u_up;           // relighting: the world's up in view space
uniform float u_aoOn;        // 1: the occlusion darkens; 0: relighting alone
uniform int u_relightShow;   // relighting, checking: 1 shows the factor and what is left alone
void main()
{
    ivec2 s = ivec2(gl_GlobalInvocationID.xy);
    if (s.x >= u_size.x || s.y >= u_size.y)
        return;
    float w = distanceAt(s);
    // Sun glow: on the sky (nothing tapped there, or the sky's partition), round GT4's sun when it is ahead: a wide
    // warm halo and a brighter core, screened over GT4's sky.
    if (u_glow > 0.0 && u_sun.z > 0.0 && !skipped(s) && u_relightShow != 1 && (w >= kFar || partitionAt(s) < u_relightFrom))
    {
        float d = max(dot(normalize(viewAt(s, 1.0)), normalize(u_sun.xyz)), 0.0);
        float g = u_glow * (0.35 * pow(d, 12.0) + 0.5 * pow(d, 300.0));
        if (u_relightShow == 2 && s.x <= u_size.x / 2)
            g = 0.0;
        uint word = imageLoad(u_color, s).r;
        vec4 c = unpackUnorm4x8(word);
        c.rgb = 1.0 - (1.0 - c.rgb) * (1.0 - clamp(g * vec3(1.0, 0.9, 0.7), 0.0, 1.0));
        imageStore(u_color, s, uvec4(packUnorm4x8(c)));
        if (w >= kFar)
            return;
    }
    if ((w >= kFar && (u_show == 0 || u_show == 4)) || (u_show == 4 && s.x < u_size.x / 2) || skipped(s))
        return;
    if (u_show == 3)
    {
        uint word = imageLoad(u_color, s).r;
        vec4 c = unpackUnorm4x8(word);
        c.rgb = w >= kFar ? vec3(0.0) : unpackUnorm4x8(texelFetch(u_aoTex, s, 0).r).rgb;
        imageStore(u_color, s, uvec4(packUnorm4x8(c)));
        return;
    }
    // 4 x 4 (the spin's period), samples within 5% of this one's distance: the occlusion and relighting's factor
    float sum = 0.0, sumF = 0.0, weight = 0.0;
    for (int y = -2; y < 2; ++y)
        for (int x = -2; x < 2; ++x)
        {
            ivec2 q = clamp(s + ivec2(x, y), ivec2(0), u_size - 1);
            float wq = distanceAt(q);
            float k = abs(wq - w) <= 0.05 * w ? 1.0 : 0.0;
            uint v = texelFetch(u_aoTex, q, 0).r;
            sum += k * float(v & 0xffffu) / 65535.0;
            sumF += k * float(v >> 16) / 32767.5;
            weight += k;
        }
    float ao = weight > 0.0 && u_aoOn > 0.5 ? sum / weight : 1.0;
    float f = weight > 0.0 && (u_sun.w > 0.0 || u_shadow > 0.0) ? sumF / weight : 1.0;
    if (u_relightShow == 2 && s.x <= u_size.x / 2) // (GT4_RELIGHT_SHOW=split: GT4's own lighting on the left, a line between)
        f = s.x >= u_size.x / 2 - 1 ? 0.0 : 1.0;
    if (u_relightShow == 1) // (GT4_RELIGHT_SHOW=1, checking: the factor in grey, tinted by partition; red not relit, blue a car, black the sky)
    {
        const vec3 tint[4] = vec3[4](vec3(1.0, 0.7, 0.7), vec3(1.0), vec3(0.7, 1.0, 0.7), vec3(0.7, 0.7, 1.0));
        vec3 k = w >= kFar ? vec3(0.0) : carAt(s) ? vec3(0.1, 0.2, 0.8) : !relightable(s) ? vec3(0.6, 0.1, 0.1)
               : vec3(f / 1.6) * tint[partitionAt(s)];
        imageStore(u_color, s, uvec4(packUnorm4x8(vec4(k, unpackUnorm4x8(imageLoad(u_color, s).r).a))));
        return;
    }
    ao *= f;
    uint word = imageLoad(u_color, s).r;
    vec4 c = unpackUnorm4x8(word);
    c.rgb = u_show == 2 ? (w >= kFar ? vec3(1.0, 0.0, 0.0) : vec3(clamp(log2(max(w, 1.0)) / 16.0, 0.0, 1.0)))
          : (u_show == 1 ? vec3(ao) : c.rgb * ao);
    imageStore(u_color, s, uvec4(packUnorm4x8(c)));
}
)GLSL";
        const auto link = [](const std::string &src) -> GLuint {
            const GLuint shader = compile(GL_COMPUTE_SHADER, src);
            if (!shader)
                return 0;
            GLuint p = glCreateProgram();
            glAttachShader(p, shader);
            glLinkProgram(p);
            glDeleteShader(shader);
            GLint linked = 0;
            glGetProgramiv(p, GL_LINK_STATUS, &linked);
            if (!linked)
            {
                glDeleteProgram(p);
                return 0;
            }
            return p;
        };
        aoProgram = link(occlusion);
        aoApplyProgram = link(apply);
        if (!aoProgram || !aoApplyProgram)
        {
            std::cerr << "[gt4-ao] programs failed: ambient occlusion off" << std::endl;
            return false;
        }
        const char *names[9] = {"u_depth", "u_size", "u_depthScale", "u_perPx", "u_fit", "u_proj", "u_ao", "u_show", "u_skip"};
        for (int i = 0; i < 9; ++i)
            aoLoc[i] = glGetUniformLocation(aoProgram, names[i]);
        const char *applyNames[9] = {"u_depth", "u_size", "u_depthScale", "u_perPx", "u_fit", "u_proj", "u_aoTex", "u_show", "u_skip"};
        for (int i = 0; i < 9; ++i)
            aoApplyLoc[i] = glGetUniformLocation(aoApplyProgram, applyNames[i]);
        return true;
    }
    // Lighting step 3 (GT4_SSR=1; =show: the reflections alone, =gbuf: the cars' normals as colours): screen-space
    // reflections on the cars, over GT4's own reflection layer. Each frame the reflection layer's tapped triangles (M10:
    // every car body and rim) are kept with their corners' view normals (the tap's, in the occlusion's view space: x
    // right, y down, z away) and strength (the layer's vertex colour, 128 = 1); when the 3D is done they are drawn into a
    // small G-buffer (normal, strength) where they are the visible surface (the 3D's own depth), and a compute pass
    // marches each such pixel's reflected ray through GT4's depth (the occlusion's distances and projection) and adds the
    // colour it meets, faded towards the screen's edges and for rays coming back at the camera. Rays that meet nothing
    // (the sky, off screen) leave GT4's own reflection as it is.
    std::vector<float> ssrVerts; // per corner: x, y (frame pixels), Z (/ 2^24), strength; normal x, y, z, 0
    GLuint ssrProgram = 0, ssrComputeProgram = 0, ssrFbo = 0, ssrBuffer = 0, ssrGbuf = 0, ssrDepth = 0, ssrCopy = 0;
    int ssrW = 0, ssrH = 0;
    GLint ssrLoc[4] = {}, ssrComputeLoc[11] = {};
    bool ssrBroken = false;
    uint64_t statSsrFrames = 0, statSsrTris = 0;
    static int ssrMode()
    {
        static const int m = [] {
            const char *v = std::getenv("GT4_SSR");
            if (!v || !*v || std::strcmp(v, "0") == 0 || std::strcmp(v, "off") == 0)
                return 0;
            return std::strcmp(v, "show") == 0 ? 2 : std::strcmp(v, "gbuf") == 0 ? 3 : std::strcmp(v, "rays") == 0 ? 4 : 1;
        }();
        return m;
    }
    void ssrNote(const GSPrimitiveBatch &batch)
    {
        const GSContext &ctx = batch.state.context;
        if (!aoActive || ctx.frame.fbp != aoFbp || ssrVerts.size() > 3000000u)
            return;
        const float ofx = ctx.xyoffset.ofx / 16.0f, ofy = ctx.xyoffset.ofy / 16.0f;
        float corner[3][8];
        for (int i = 0; i < 3; ++i)
        {
            const GSVertex &v = batch.vertices[i];
            gt4tap::Corner cn;
            if (!gt4tap::corner(v.tapSlot, v.tapK, v.x, v.y, v.q, cn) || !cn.rec->env)
                return;
            const gt4tap::Record *r = cn.rec;
            float vn[3];
            gt4tap::viewNormal(*r, cn.nrm, vn);
            const float sx = r->vs[0] < 0.0f ? -1.0f : 1.0f, sy = r->vs[1] < 0.0f ? -1.0f : 1.0f;
            corner[i][0] = v.x - ofx;
            corner[i][1] = v.y - ofy;
            corner[i][2] = static_cast<float>(static_cast<double>(v.z) / 16777216.0);
            corner[i][3] = std::clamp((v.r + v.g + v.b) / (3.0f * 256.0f), 0.0f, 1.0f);
            corner[i][4] = vn[0] * sx;
            corner[i][5] = vn[1] * sy;
            corner[i][6] = -vn[2];
            corner[i][7] = v.q;
        }
        for (int i = 0; i < 3; ++i)
            ssrVerts.insert(ssrVerts.end(), corner[i], corner[i] + 8);
    }
    bool ssrBuild()
    {
        const std::string vs = R"GLSL(#version 430 core
layout(std430, binding = 2) readonly buffer SsrVerts { vec4 sv[]; };
uniform vec2 u_frame;
out vec4 v_normal;
noperspective out float v_depth; // (GS Z is linear on the screen)
noperspective out float v_q;     // (and Q)
void main()
{
    vec4 a = sv[gl_VertexID * 2], b = sv[gl_VertexID * 2 + 1];
    v_normal = vec4(b.xyz, a.w);
    v_depth = a.z;
    v_q = b.w;
    gl_Position = vec4(a.xy / u_frame * 2.0 - 1.0, 0.0, 1.0);
}
)GLSL";
        const std::string fs = R"GLSL(#version 430 core
in vec4 v_normal;
noperspective in float v_depth;
noperspective in float v_q;
uniform usampler2D u_sceneDepth; // the distance pass's distances
uniform vec2 u_depthScale;
uniform vec3 u_fit; // z: how far (GT4's units) GT4's surface may be in front of this one
out vec4 o_color;
void main()
{
    // (only where this triangle is what the 3D shows: GT4's surface there no nearer than it, within u_fit.z; the
    // reflection layer is not always exactly on its car's body)
    uint ds = texelFetch(u_sceneDepth, clamp(ivec2(gl_FragCoord.xy), ivec2(0), textureSize(u_sceneDepth, 0) - 1), 0).r;
    float ws = ds == 0u ? 1.0e9 : uintBitsToFloat(ds & 0x7fffffffu), wv = v_q > 0.0 ? 1.0 / v_q : 1.0e9;
    if (wv - ws > u_fit.z || dot(v_normal.xyz, v_normal.xyz) < 0.01)
        discard;
    gl_FragDepth = v_depth;
    o_color = vec4(normalize(v_normal.xyz) * 0.5 + 0.5, v_normal.w);
}
)GLSL";
        const GLuint a = compile(GL_VERTEX_SHADER, vs), b = compile(GL_FRAGMENT_SHADER, fs);
        if (!a || !b)
            return false;
        ssrProgram = glCreateProgram();
        glAttachShader(ssrProgram, a);
        glAttachShader(ssrProgram, b);
        glLinkProgram(ssrProgram);
        glDeleteShader(a);
        glDeleteShader(b);
        GLint linked = 0;
        glGetProgramiv(ssrProgram, GL_LINK_STATUS, &linked);
        if (!linked)
            return false;
        ssrLoc[0] = glGetUniformLocation(ssrProgram, "u_frame");
        ssrLoc[1] = glGetUniformLocation(ssrProgram, "u_sceneDepth");
        ssrLoc[2] = glGetUniformLocation(ssrProgram, "u_depthScale");
        ssrLoc[3] = glGetUniformLocation(ssrProgram, "u_fit");
        const std::string cs = aoHead() + R"GLSL(
uniform sampler2D u_gbuf;
uniform sampler2D u_colorCopy;
uniform vec2 u_ssr; // strength, farthest a ray goes (GT4's units)
layout(r32ui, binding = 1) uniform uimage2D u_color; // the 3D buffer's RGBA8 words
ivec2 sampleOf(vec3 Q)
{
    vec2 gs = vec2(Q.x / Q.z * u_proj.x + u_proj.z, Q.y / Q.z * u_proj.y + u_proj.w);
    return ivec2(floor(gs * u_perPx));
}
void main()
{
    ivec2 s = ivec2(gl_GlobalInvocationID.xy);
    if (s.x >= u_size.x || s.y >= u_size.y)
        return;
    vec4 g = texelFetch(u_gbuf, s, 0);
    if (g.a <= 0.0 || (skipped(s) && u_show != 4))
        return;
    uint word = imageLoad(u_color, s).r;
    vec4 c = unpackUnorm4x8(word);
    if (u_show == 4 && (skipped(s) || distanceAt(s) >= kFar))
    {
        c.rgb = skipped(s) ? vec3(1.0, 0.0, 1.0) : vec3(1.0);
        imageStore(u_color, s, uvec4(packUnorm4x8(c)));
        return;
    }
    if (u_show == 3)
    {
        c.rgb = g.rgb;
        imageStore(u_color, s, uvec4(packUnorm4x8(c)));
        return;
    }
    float w = distanceAt(s);
    if (w >= kFar)
        return;
    vec3 P = viewAt(s, w);
    vec3 N = normalize(g.xyz * 2.0 - 1.0);
    vec3 V = normalize(P);
    vec3 R = reflect(V, N);
    float t = 0.04 * w + 1.0, prev = 0.0; // (clear of the surface itself: GT4's smooth normals on flat facets)
    bool found = false, rejected = false;
    int ended = 0; // (checking, =rays: 1 near the camera, 2 off screen, 3 an inset view, 0 out of reach)
    ivec2 hq = s;
    for (int i = 0; i < 40 && t < u_ssr.y; ++i)
    {
        vec3 Q = P + R * t;
        if (Q.z <= 0.5)
        {
            ended = 1;
            break;
        }
        ivec2 q = sampleOf(Q);
        if (any(lessThan(q, ivec2(0))) || any(greaterThanEqual(q, u_size)) || skipped(q))
        {
            ended = skipped(q) ? 3 : 2;
            break;
        }
        float ws = distanceAt(q);
        float d = Q.z - ws; // > 0: behind what the 3D shows there
        if (d > 0.0 && ws < kFar)
        {
            // the crossing between the last step in front and this one; a hit if the ray is then close behind the
            // surface (not passing behind something thin in front of it)
            float lo = prev, hi = t;
            for (int j = 0; j < 7; ++j)
            {
                float m = 0.5 * (lo + hi);
                vec3 M = P + R * m;
                ivec2 mq = clamp(sampleOf(M), ivec2(0), u_size - 1);
                if (M.z - distanceAt(mq) > 0.0)
                    hi = m;
                else
                    lo = m;
            }
            vec3 H = P + R * hi;
            hq = clamp(sampleOf(H), ivec2(0), u_size - 1);
            if (H.z - distanceAt(hq) < max(1.5, 0.06 * H.z) && hi > 0.08 * w + 2.0)
            {
                found = true;
                break;
            }
            rejected = true;
        }
        prev = t;
        t = t * 1.25 + 0.01 * w;
    }
    vec3 hit = vec3(0.0);
    float fade = 0.0;
    if (found)
    {
        hit = texelFetch(u_colorCopy, hq, 0).rgb;
        vec2 e = vec2(hq) / vec2(u_size);
        fade = clamp(min(min(e.x, 1.0 - e.x), min(e.y, 1.0 - e.y)) * 12.0, 0.0, 1.0) * clamp((R.z + 0.3) * 2.5, 0.0, 1.0);
    }
    if (u_show == 4)
        c.rgb = found ? vec3(0.0, 1.0, 0.0) : (ended == 1 ? vec3(1.0, 1.0, 0.0) : ended == 2 ? vec3(0.0, 0.0, 1.0) : ended == 3 ? vec3(1.0, 0.0, 1.0)
                : vec3(1.0, 0.0, 0.0)) * (rejected ? 0.5 : 1.0);
    else if (u_show == 1)
        c.rgb = hit * fade;
    else
        c.rgb = min(c.rgb + hit * (fade * g.a * 2.0 * u_ssr.x), vec3(1.0));
    imageStore(u_color, s, uvec4(packUnorm4x8(c)));
}
)GLSL";
        const GLuint shader = compile(GL_COMPUTE_SHADER, cs);
        if (!shader)
            return false;
        ssrComputeProgram = glCreateProgram();
        glAttachShader(ssrComputeProgram, shader);
        glLinkProgram(ssrComputeProgram);
        glDeleteShader(shader);
        glGetProgramiv(ssrComputeProgram, GL_LINK_STATUS, &linked);
        if (!linked)
            return false;
        const char *names[11] = {"u_depth", "u_size", "u_depthScale", "u_perPx", "u_fit", "u_proj", "u_gbuf", "u_colorCopy", "u_ssr", "u_show", "u_skip"};
        for (int i = 0; i < 11; ++i)
            ssrComputeLoc[i] = glGetUniformLocation(ssrComputeProgram, names[i]);
        glGenFramebuffers(1, &ssrFbo);
        glGenBuffers(1, &ssrBuffer);
        return true;
    }
    // The frame's reflections into its 3D buffer (ct, its depth dt; w x h samples, rows GS rows).
    void ssrPass(const std::vector<float> &verts, const Target &ct, const Target &dt, int w, int h, int rows, double A, double B)
    {
        if (verts.empty() || ssrBroken)
            return;
        if (!ssrProgram && !ssrBuild())
        {
            std::cerr << "[gt4-ssr] programs failed: screen-space reflections off" << std::endl;
            ssrBroken = true;
            return;
        }
        if (w > ssrW || h > ssrH)
        {
            releaseTexture(ssrGbuf);
            releaseTexture(ssrDepth);
            releaseTexture(ssrCopy);
            ssrW = std::max(w, ssrW);
            ssrH = std::max(h, ssrH);
            ssrGbuf = makeColorTexture(ssrW, ssrH);
            ssrDepth = makeTargetTexture(true, ssrW, ssrH);
            ssrCopy = makeColorTexture(ssrW, ssrH);
        }
        const float depthScale[2] = {static_cast<float>(dt.sx) / static_cast<float>(ct.sx), static_cast<float>(dt.sy) / static_cast<float>(ct.sy)};
        // the G-buffer: the reflection layer's triangles where GT4 shows them
        glBindFramebuffer(GL_FRAMEBUFFER, ssrFbo);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, ssrGbuf, 0);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, ssrDepth, 0);
        glDrawBuffer(GL_COLOR_ATTACHMENT0);
        glViewport(0, 0, w, h);
        glDisable(GL_SCISSOR_TEST);
        glDisable(GL_BLEND);
        glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
        glDepthMask(GL_TRUE);
        if (clearTexImage)
        {
            clearTexImage(ssrGbuf, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
            clearTexImage(ssrDepth, 0, GL_DEPTH_COMPONENT, GL_FLOAT, nullptr);
        }
        else
        {
            glClearDepth(0.0);
            glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
            glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        }
        glEnable(GL_DEPTH_TEST);
        glDepthFunc(GL_GREATER);
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, ssrBuffer);
        glBufferData(GL_SHADER_STORAGE_BUFFER, static_cast<GLsizeiptr>(verts.size() * sizeof(float)), verts.data(), GL_STREAM_DRAW);
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 2, ssrBuffer);
        glUseProgram(ssrProgram);
        glBindVertexArray(vao);
        glActiveTexture(GL_TEXTURE14);
        glBindTexture(GL_TEXTURE_2D, distTex);
        glUniform1i(ssrLoc[1], 14);
        glUniform2f(ssrLoc[0], static_cast<float>(w) / static_cast<float>(ct.sx), static_cast<float>(h) / static_cast<float>(ct.sy));
        glUniform2f(ssrLoc[2], depthScale[0], depthScale[1]);
        static const float layerSlack = [] {
            const char *v = std::getenv("GT4_SSR_SLACK");
            return v && *v ? std::max(0.0f, static_cast<float>(std::atof(v))) : 2.0f; // (GT4 units: ~30 cm)
        }();
        glUniform3f(ssrLoc[3], static_cast<float>(A), static_cast<float>(B), layerSlack);
        glDrawArrays(GL_TRIANGLES, 0, static_cast<GLsizei>(verts.size() / 8u));
        glBindVertexArray(0);
        glDisable(GL_DEPTH_TEST);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        statSsrTris += verts.size() / 24u;
        // (GT4_SSR_PROBE=<x>,<y> in samples, checking: every 300th frame that pixel's ray traced here, step by step, in the log)
        static const std::pair<int, int> probeAt = [] {
            const char *v = std::getenv("GT4_SSR_PROBE");
            int x = -1, y = -1;
            if (v && *v)
                std::sscanf(v, "%d,%d", &x, &y);
            return std::make_pair(x, y);
        }();
        if (probeAt.first >= 0 && probeAt.first < w && probeAt.second >= 0 && probeAt.second < h && statSsrFrames % 300u == 0u)
        {
            std::vector<uint32_t> dist(static_cast<size_t>(distW) * distH);
            std::vector<uint8_t> gb(static_cast<size_t>(ssrW) * ssrH * 4u);
            glBindTexture(GL_TEXTURE_2D, distTex);
            glGetTexImage(GL_TEXTURE_2D, 0, GL_RED_INTEGER, GL_UNSIGNED_INT, dist.data());
            glBindTexture(GL_TEXTURE_2D, ssrGbuf);
            glGetTexImage(GL_TEXTURE_2D, 0, GL_RGBA, GL_UNSIGNED_BYTE, gb.data());
            glBindTexture(GL_TEXTURE_2D, 0);
            const auto distAt = [&](int x, int y) {
                x = std::clamp(x, 0, w - 1);
                y = std::clamp(y, 0, h - 1);
                const uint32_t v = dist[static_cast<size_t>(y) * distW + x];
                float f;
                std::memcpy(&f, &v, 4);
                return v ? f : 1.0e9f;
            };
            const auto viewAt = [&](int x, int y, float d, float out[3]) {
                const float gx = (x + 0.5f) / ct.sx, gy = (y + 0.5f) / ct.sy;
                out[0] = (gx - aoProj[2]) * d / aoProj[0];
                out[1] = (gy - aoProj[3]) * d / aoProj[1];
                out[2] = d;
            };
            const int px = probeAt.first, py = probeAt.second;
            const uint8_t *g = &gb[(static_cast<size_t>(py) * ssrW + px) * 4u];
            float N[3] = {g[0] / 127.5f - 1.0f, g[1] / 127.5f - 1.0f, g[2] / 127.5f - 1.0f};
            const float nl = std::sqrt(N[0] * N[0] + N[1] * N[1] + N[2] * N[2]);
            for (float &c : N)
                c = nl > 0.0f ? c / nl : 0.0f;
            const float d0 = distAt(px, py);
            float P[3], V[3], R[3];
            viewAt(px, py, d0, P);
            const float pl = std::sqrt(P[0] * P[0] + P[1] * P[1] + P[2] * P[2]);
            for (int i = 0; i < 3; ++i)
                V[i] = P[i] / pl;
            const float vn = V[0] * N[0] + V[1] * N[1] + V[2] * N[2];
            for (int i = 0; i < 3; ++i)
                R[i] = V[i] - 2.0f * vn * N[i];
            std::fprintf(stderr, "[gt4-ssr] probe %d,%d: strength %u, distance %.2f, P %.2f %.2f %.2f, N %.3f %.3f %.3f, R %.3f %.3f %.3f\n", px, py,
                         g[3], d0, P[0], P[1], P[2], N[0], N[1], N[2], R[0], R[1], R[2]);
            float t = 0.04f * d0 + 1.0f;
            for (int i = 0; i < 40 && t < 300.0f; ++i)
            {
                const float Q[3] = {P[0] + R[0] * t, P[1] + R[1] * t, P[2] + R[2] * t};
                const float gx = Q[0] / Q[2] * aoProj[0] + aoProj[2], gy = Q[1] / Q[2] * aoProj[1] + aoProj[3];
                const int qx = static_cast<int>(std::floor(gx * ct.sx)), qy = static_cast<int>(std::floor(gy * ct.sy));
                const bool off = qx < 0 || qy < 0 || qx >= w || qy >= h;
                std::fprintf(stderr, "[gt4-ssr]   t %.2f: Q %.2f %.2f %.2f -> %d,%d%s, there %.2f\n", t, Q[0], Q[1], Q[2], qx, qy, off ? " off" : "",
                             off ? 0.0f : distAt(qx, qy));
                if (off || Q[2] <= 0.5f)
                    break;
                t = t * 1.25f + 0.01f * d0;
            }
        }
        // the 3D as it is (the rays read it while the pass adds to it)
        glCopyImageSubData(ct.tex, GL_TEXTURE_2D, 0, 0, 0, 0, ssrCopy, GL_TEXTURE_2D, 0, 0, 0, 0, w, h, 1);
        static const float strength = [] {
            const char *v = std::getenv("GT4_SSR_STRENGTH");
            return v && *v ? std::max(0.0f, static_cast<float>(std::atof(v))) : 0.5f;
        }();
        static const float reach = [] {
            const char *v = std::getenv("GT4_SSR_REACH");
            return v && *v ? std::max(1.0f, static_cast<float>(std::atof(v))) : 300.0f; // (GT4 units: a car is ~10 wide)
        }();
        glUseProgram(ssrComputeProgram);
        glActiveTexture(GL_TEXTURE14);
        glBindTexture(GL_TEXTURE_2D, distTex);
        glActiveTexture(GL_TEXTURE15);
        glBindTexture(GL_TEXTURE_2D, ssrGbuf);
        glActiveTexture(GL_TEXTURE13);
        glBindTexture(GL_TEXTURE_2D, ssrCopy);
        glUniform1i(ssrComputeLoc[0], 14);
        glUniform2i(ssrComputeLoc[1], w, h);
        glUniform2f(ssrComputeLoc[2], depthScale[0], depthScale[1]);
        glUniform2f(ssrComputeLoc[3], static_cast<float>(ct.sx), static_cast<float>(ct.sy));
        glUniform2f(ssrComputeLoc[4], static_cast<float>(A), static_cast<float>(B));
        glUniform4f(ssrComputeLoc[5], aoProj[0], aoProj[1], aoProj[2], aoProj[3]);
        glUniform1i(ssrComputeLoc[6], 15);
        glUniform1i(ssrComputeLoc[7], 13);
        glUniform2f(ssrComputeLoc[8], strength, reach);
        glUniform1i(ssrComputeLoc[9], ssrMode() == 2 ? 1 : ssrMode() == 3 ? 3 : ssrMode() == 4 ? 4 : 0);
        glUniform4i(ssrComputeLoc[10], aoSkip[0] * ct.sx, aoSkip[1] * ct.sy, aoSkip[2] * ct.sx + ct.sx - 1, aoSkip[3] * ct.sy + ct.sy - 1);
        glBindImageTexture(1, ct.tex, 0, GL_FALSE, 0, GL_READ_WRITE, GL_R32UI);
        glDispatchCompute(static_cast<GLuint>((w + 7) / 8), static_cast<GLuint>((h + 7) / 8), 1);
        glMemoryBarrier(GL_ALL_BARRIER_BITS);
        glBindImageTexture(1, 0, 0, GL_FALSE, 0, GL_READ_WRITE, GL_R32UI);
        for (GLenum unit : {GL_TEXTURE13, GL_TEXTURE14, GL_TEXTURE15})
        {
            glActiveTexture(unit);
            glBindTexture(GL_TEXTURE_2D, 0);
        }
        glActiveTexture(GL_TEXTURE0);
        glUseProgram(0);
        glStateDirty = true;
        forgetBindings();
        if (++statSsrFrames % 300u == 1u)
            std::fprintf(stderr, "[gt4-ssr] frame %llu: %zu reflection-layer triangles into %dx%d samples (%d rows); %llu in all; "
                                 "inset view %d,%d-%d,%d; Z = %.6f + %.6f / w, focal %.1f x %.1f centre %.1f, %.1f\n",
                         static_cast<unsigned long long>(statSsrFrames), verts.size() / 24u, w, h, rows,
                         static_cast<unsigned long long>(statSsrTris), aoSkip[0], aoSkip[1], aoSkip[2], aoSkip[3], A, B, aoProj[0], aoProj[1],
                         aoProj[2], aoProj[3]);
    }
    bool distPass(const std::vector<float> &verts, const Target &ct, const Target &dt, int w, int h)
    {
        if (verts.size() < 12u)
            return false;
        if (!distProgram)
        {
            const std::string vs = R"GLSL(#version 430 core
layout(std430, binding = 2) readonly buffer DistVerts { vec4 dv[]; };
uniform vec2 u_frame;
noperspective out float v_depth;
noperspective out float v_q;
void main()
{
    vec4 a = dv[gl_VertexID];
    v_depth = a.z;
    v_q = a.w;
    gl_Position = vec4(a.xy / u_frame * 2.0 - 1.0, 0.0, 1.0);
}
)GLSL";
            const std::string fs = R"GLSL(#version 430 core
noperspective in float v_depth;
noperspective in float v_q;
layout(location = 0) out uint o_dist;
uniform int u_verify;     // relighting: only where this partition's GS Z (u_gsZ) is this triangle's
uniform sampler2D u_gsZ;
uniform vec2 u_zScale;    // GS depth samples per sample here
uniform uint u_part;      // relighting: the partition (0 first), in the distance's two lowest bits
void main()
{
    if (u_verify != 0)
    {
        float g = texelFetch(u_gsZ, ivec2(gl_FragCoord.xy * u_zScale), 0).r;
        if (abs(g - v_depth) > 0.01 * v_depth + 4.0 / 16777216.0)
            discard;
    }
    gl_FragDepth = v_depth;
    uint d = floatBitsToUint(1.0 / max(abs(v_q), 1.0e-9));
    if (u_verify != 0)
        d = (d & ~3u) | min(u_part, 3u);
    o_dist = v_q < 0.0 ? (d | 0x80000000u) : d; // (the sign bit: a car's, relighting)
}
)GLSL";
            const GLuint a = compile(GL_VERTEX_SHADER, vs), b = compile(GL_FRAGMENT_SHADER, fs);
            if (!a || !b)
                return false;
            distProgram = glCreateProgram();
            glAttachShader(distProgram, a);
            glAttachShader(distProgram, b);
            glLinkProgram(distProgram);
            glDeleteShader(a);
            glDeleteShader(b);
            GLint linked = 0;
            glGetProgramiv(distProgram, GL_LINK_STATUS, &linked);
            if (!linked)
            {
                glDeleteProgram(distProgram);
                distProgram = 0;
                std::cerr << "[gt4-ao] distance pass failed" << std::endl;
                return false;
            }
            distLoc = glGetUniformLocation(distProgram, "u_frame");
            distVerifyLoc[0] = glGetUniformLocation(distProgram, "u_verify");
            distVerifyLoc[1] = glGetUniformLocation(distProgram, "u_gsZ");
            distVerifyLoc[2] = glGetUniformLocation(distProgram, "u_zScale");
            distVerifyLoc[3] = glGetUniformLocation(distProgram, "u_part");
            glGenFramebuffers(1, &distFbo);
            glGenBuffers(1, &distBuffer);
        }
        if (w > distW || h > distH)
        {
            releaseTexture(distTex);
            releaseTexture(distDepth);
            distW = std::max(w, distW);
            distH = std::max(h, distH);
            glGenTextures(1, &distTex);
            glBindTexture(GL_TEXTURE_2D, distTex);
            glTexStorage2D(GL_TEXTURE_2D, 1, GL_R32UI, distW, distH);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
            glBindTexture(GL_TEXTURE_2D, 0);
            distDepth = makeTargetTexture(true, distW, distH);
        }
        glBindFramebuffer(GL_FRAMEBUFFER, distFbo);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, distTex, 0);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, distDepth, 0);
        glDrawBuffer(GL_COLOR_ATTACHMENT0);
        glViewport(0, 0, w, h);
        glDisable(GL_SCISSOR_TEST);
        glDisable(GL_BLEND);
        glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
        glDepthMask(GL_TRUE);
        if (clearTexImage)
            clearTexImage(distTex, 0, GL_RED_INTEGER, GL_UNSIGNED_INT, nullptr);
        glUseProgram(distProgram);
        glBindVertexArray(vao);
        glUniform2f(distLoc, static_cast<float>(w) / static_cast<float>(ct.sx), static_cast<float>(h) / static_cast<float>(ct.sy));
        glEnable(GL_DEPTH_TEST);
        glDepthFunc(GL_GREATER);
        const bool verify = sunLit();
        glUniform1i(distVerifyLoc[0], verify ? 1 : 0);
        glUniform1i(distVerifyLoc[1], 13);
        glUniform2f(distVerifyLoc[2], static_cast<float>(dt.sx) / static_cast<float>(ct.sx), static_cast<float>(dt.sy) / static_cast<float>(ct.sy));
        const uint32_t tris = static_cast<uint32_t>(verts.size() / 12u);
        for (size_t p = 0; p < distParts.size(); ++p)
        {
            const uint32_t first = distParts[p], last = p + 1 < distParts.size() ? distParts[p + 1] : tris;
            if (last <= first)
                continue;
            // (each partition after GT4's Z clear: its own depth test only)
            if (clearTexImage)
                clearTexImage(distDepth, 0, GL_DEPTH_COMPONENT, GL_FLOAT, nullptr);
            if (verify)
            {
                // (relighting: checked against GT4's depth buffer as this partition left it)
                const int snap = p < distSnap.size() ? distSnap[p] : -1;
                glActiveTexture(GL_TEXTURE13);
                glBindTexture(GL_TEXTURE_2D, snap >= 0 ? relightSnap[snap] : dt.tex);
                glActiveTexture(GL_TEXTURE0);
                glUniform1ui(distVerifyLoc[3], static_cast<GLuint>(p));
            }
            // (each partition's own upload, drawn from vertex 0: gl_VertexID with a first vertex is not the same everywhere)
            glBindBuffer(GL_SHADER_STORAGE_BUFFER, distBuffer);
            glBufferData(GL_SHADER_STORAGE_BUFFER, static_cast<GLsizeiptr>((last - first) * 12u * sizeof(float)), verts.data() + first * 12u,
                         GL_STREAM_DRAW);
            glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 2, distBuffer);
            glDrawArrays(GL_TRIANGLES, 0, static_cast<GLsizei>((last - first) * 3u));
        }
        statDistParts += distParts.size();
        if (verify)
        {
            glActiveTexture(GL_TEXTURE13);
            glBindTexture(GL_TEXTURE_2D, 0);
            glActiveTexture(GL_TEXTURE0);
            // (now and then: each partition's triangles, its depth snapshot and its distances)
            static uint64_t logs = 0;
            if (logs++ % 1200u == 0u)
            {
                std::fprintf(stderr, "[gt4-relight] partitions %zu (Z clears seen %llu, missed %llu):", distParts.size(),
                             static_cast<unsigned long long>(statRelightClears), static_cast<unsigned long long>(statRelightClearsMissed));
                for (size_t p = 0; p < distParts.size(); ++p)
                {
                    const uint32_t first = distParts[p], last = p + 1 < distParts.size() ? distParts[p + 1] : tris;
                    float lo = 1e30f, hi = 0.0f;
                    for (uint32_t t = first; t < last; ++t)
                        for (int c = 0; c < 3; ++c)
                        {
                            const float d = 1.0f / std::max(std::fabs(verts[(t * 3u + c) * 4u + 3u]), 1e-9f);
                            lo = std::min(lo, d);
                            hi = std::max(hi, d);
                        }
                    std::fprintf(stderr, " [%zu: %u triangles, snapshot %d, distance %.0f-%.0f]", p, last - first,
                                 p < distSnap.size() ? distSnap[p] : -1, last > first ? lo : 0.0f, hi);
                }
                std::fprintf(stderr, "\n");
            }
        }
        glBindVertexArray(0);
        glUseProgram(0);
        glDisable(GL_DEPTH_TEST);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        glStateDirty = true;
        return true;
    }
    // Bloom (GT4_BLOOM): at the end of the frame's 3D, before GT4's post effects and HUD. A quarter-size bright pass
    // (what is above 0.75 of full brightness), blurred across and down twice, screened back over the 3D buffer at the
    // strength; the inset mirror's rectangle left alone. Textures: RGBA8 words in R32UI (the Vulkan layer's images).
    GLuint bloomProgram[3] = {}, bloomTex[2] = {};
    GLint bloomLoc[3][5] = {};
    int bloomW = 0, bloomH = 0;
    bool bloomBroken = false;
    bool bloomBuild()
    {
        const std::string head = R"GLSL(#version 430 core
layout(local_size_x = 8, local_size_y = 8) in;
uniform ivec2 u_size;    // the 3D buffer's samples
uniform ivec4 u_skip;    // the inset mirror's rectangle in samples (inclusive; x1 < x0: none)
uniform ivec2 u_qsize;   // the quarter-size textures
uniform ivec2 u_dir;     // blur: the step
uniform float u_strength;
bool skipped(ivec2 s)
{
    return s.x >= u_skip.x && s.x <= u_skip.z && s.y >= u_skip.y && s.y <= u_skip.w;
}
)GLSL";
        const std::string down = head + R"GLSL(
layout(r32ui, binding = 1) readonly uniform uimage2D u_color;
layout(r32ui, binding = 2) writeonly uniform uimage2D u_out;
void main()
{
    ivec2 o = ivec2(gl_GlobalInvocationID.xy);
    if (o.x >= u_qsize.x || o.y >= u_qsize.y)
        return;
    vec3 sum = vec3(0.0);
    for (int y = 0; y < 4; ++y)
        for (int x = 0; x < 4; ++x)
        {
            ivec2 s = min(o * 4 + ivec2(x, y), u_size - 1);
            if (skipped(s))
                continue;
            vec3 c = unpackUnorm4x8(imageLoad(u_color, s).r).rgb;
            float l = max(c.r, max(c.g, c.b));
            sum += c * (max(l - 0.75, 0.0) / max(l, 1.0e-3));
        }
    imageStore(u_out, o, uvec4(packUnorm4x8(vec4(clamp(sum / (16.0 * 0.25), 0.0, 1.0), 1.0))));
}
)GLSL";
        const std::string blur = head + R"GLSL(
uniform usampler2D u_in;
layout(r32ui, binding = 2) writeonly uniform uimage2D u_out;
void main()
{
    ivec2 o = ivec2(gl_GlobalInvocationID.xy);
    if (o.x >= u_qsize.x || o.y >= u_qsize.y)
        return;
    const float k[5] = float[5](0.227, 0.194, 0.121, 0.054, 0.016);
    vec3 sum = unpackUnorm4x8(texelFetch(u_in, o, 0).r).rgb * k[0];
    for (int i = 1; i < 5; ++i)
    {
        sum += unpackUnorm4x8(texelFetch(u_in, clamp(o + u_dir * i, ivec2(0), u_qsize - 1), 0).r).rgb * k[i];
        sum += unpackUnorm4x8(texelFetch(u_in, clamp(o - u_dir * i, ivec2(0), u_qsize - 1), 0).r).rgb * k[i];
    }
    imageStore(u_out, o, uvec4(packUnorm4x8(vec4(sum, 1.0))));
}
)GLSL";
        const std::string add = head + R"GLSL(
uniform usampler2D u_in;
layout(r32ui, binding = 1) uniform uimage2D u_color;
vec3 bloomAt(ivec2 q)
{
    return unpackUnorm4x8(texelFetch(u_in, clamp(q, ivec2(0), u_qsize - 1), 0).r).rgb;
}
void main()
{
    ivec2 s = ivec2(gl_GlobalInvocationID.xy);
    if (s.x >= u_size.x || s.y >= u_size.y || skipped(s))
        return;
    vec2 p = (vec2(s) + 0.5) / 4.0 - 0.5;
    ivec2 q = ivec2(floor(p));
    vec2 f = p - vec2(q);
    vec3 b = mix(mix(bloomAt(q), bloomAt(q + ivec2(1, 0)), f.x), mix(bloomAt(q + ivec2(0, 1)), bloomAt(q + ivec2(1, 1)), f.x), f.y);
    vec4 c = unpackUnorm4x8(imageLoad(u_color, s).r);
    c.rgb = 1.0 - (1.0 - c.rgb) * (1.0 - clamp(b * u_strength, 0.0, 1.0));
    imageStore(u_color, s, uvec4(packUnorm4x8(c)));
}
)GLSL";
        const std::string *srcs[3] = {&down, &blur, &add};
        for (int i = 0; i < 3; ++i)
        {
            const GLuint shader = compile(GL_COMPUTE_SHADER, *srcs[i]);
            if (!shader)
                return false;
            GLuint prog = glCreateProgram();
            glAttachShader(prog, shader);
            glLinkProgram(prog);
            glDeleteShader(shader);
            GLint linked = 0;
            glGetProgramiv(prog, GL_LINK_STATUS, &linked);
            if (!linked)
            {
                glDeleteProgram(prog);
                return false;
            }
            bloomProgram[i] = prog;
            const char *names[5] = {"u_size", "u_skip", "u_qsize", "u_dir", "u_strength"};
            for (int n = 0; n < 5; ++n)
                bloomLoc[i][n] = glGetUniformLocation(prog, names[n]);
        }
        return true;
    }
    void bloomPass(const Target &ct, int w, int h)
    {
        if (bloomStrength() <= 0.0f || bloomBroken)
            return;
        if (!bloomProgram[0] && !bloomBuild())
        {
            std::cerr << "[gt4-bloom] programs failed: bloom off" << std::endl;
            bloomBroken = true;
            return;
        }
        const int qw = (w + 3) / 4, qh = (h + 3) / 4;
        if (qw > bloomW || qh > bloomH)
        {
            bloomW = std::max(qw, bloomW);
            bloomH = std::max(qh, bloomH);
            for (GLuint &t : bloomTex)
            {
                if (t)
                    glDeleteTextures(1, &t);
                glGenTextures(1, &t);
                glBindTexture(GL_TEXTURE_2D, t);
                glTexStorage2D(GL_TEXTURE_2D, 1, GL_R32UI, bloomW, bloomH);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
            }
            glBindTexture(GL_TEXTURE_2D, 0);
        }
        const auto common = [&](int i) {
            glUseProgram(bloomProgram[i]);
            glUniform2i(bloomLoc[i][0], w, h);
            glUniform4i(bloomLoc[i][1], aoSkip[0] * ct.sx, aoSkip[1] * ct.sy, aoSkip[2] * ct.sx + ct.sx - 1, aoSkip[3] * ct.sy + ct.sy - 1);
            glUniform2i(bloomLoc[i][2], qw, qh);
            glUniform1f(bloomLoc[i][4], bloomStrength());
        };
        const GLuint qx = static_cast<GLuint>((qw + 7) / 8), qy = static_cast<GLuint>((qh + 7) / 8);
        // (the bright pass: the 3D buffer into bloomTex[0])
        common(0);
        glBindImageTexture(1, ct.tex, 0, GL_FALSE, 0, GL_READ_ONLY, GL_R32UI);
        glBindImageTexture(2, bloomTex[0], 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_R32UI);
        glDispatchCompute(qx, qy, 1);
        glMemoryBarrier(GL_TEXTURE_FETCH_BARRIER_BIT | GL_SHADER_IMAGE_ACCESS_BARRIER_BIT);
        // (the blur: across 0 to 1, down 1 to 0; twice, the second time twice as wide)
        common(1);
        glActiveTexture(GL_TEXTURE15);
        glUniform1i(glGetUniformLocation(bloomProgram[1], "u_in"), 15);
        for (int pass = 0; pass < 4; ++pass)
        {
            const int step = pass < 2 ? 1 : 2;
            glBindTexture(GL_TEXTURE_2D, bloomTex[pass & 1]);
            glBindImageTexture(2, bloomTex[(pass & 1) ^ 1], 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_R32UI);
            glUniform2i(bloomLoc[1][3], (pass & 1) == 0 ? step : 0, (pass & 1) == 0 ? 0 : step);
            glDispatchCompute(qx, qy, 1);
            glMemoryBarrier(GL_TEXTURE_FETCH_BARRIER_BIT | GL_SHADER_IMAGE_ACCESS_BARRIER_BIT);
        }
        // (bloomTex[0] screened over the 3D buffer)
        common(2);
        glBindTexture(GL_TEXTURE_2D, bloomTex[0]);
        glUniform1i(glGetUniformLocation(bloomProgram[2], "u_in"), 15);
        glBindImageTexture(1, ct.tex, 0, GL_FALSE, 0, GL_READ_WRITE, GL_R32UI);
        glDispatchCompute(static_cast<GLuint>((w + 7) / 8), static_cast<GLuint>((h + 7) / 8), 1);
        glMemoryBarrier(GL_ALL_BARRIER_BITS);
        glBindImageTexture(1, 0, 0, GL_FALSE, 0, GL_READ_WRITE, GL_R32UI);
        glBindImageTexture(2, 0, 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_R32UI);
        glBindTexture(GL_TEXTURE_2D, 0);
        glActiveTexture(GL_TEXTURE0);
        glUseProgram(0);
        glStateDirty = true;
        forgetBindings();
        static uint64_t logs = 0;
        if (logs++ % 1200u == 0u)
            std::fprintf(stderr, "[gt4-bloom] strength %.2f over %dx%d (quarter %dx%d)\n", bloomStrength(), w, h, qw, qh);
    }
    void aoApply()
    {
        aoActive = false;
        std::vector<float> ssr;
        ssr.swap(ssrVerts); // (this frame's reflection layer: used below or dropped)
        if (aoBroken)
            return;
        const double den = aoN * aoSqq - aoSq * aoSq;
        if (aoN < 64.0 || !aoHaveProj || !(std::fabs(den) > 1e-12))
        {
            ++statAoSkipped;
            return;
        }
        const double B = (aoN * aoSqz - aoSq * aoSz) / den, A = (aoSz - B * aoSq) / aoN;
        if (!(B > 0.0))
        {
            ++statAoSkipped;
            return;
        }
        flushBatch();
        if (!aoProgram && !aoBuild())
        {
            aoBroken = true;
            return;
        }
        const int rows = std::max(std::min(aoRows, aoScissorRows), 1);
        const int cid = findTarget(false, Layout::C32, aoFbp, aoFbw, rows);
        const int did = findTarget(true, Layout::Z32, aoZbp, aoFbw, rows);
        if (cid < 0 || did < 0 || !targets[cid]->tex || !targets[did]->tex)
        {
            ++statAoSkipped;
            return;
        }
        const Target &ct = *targets[cid], &dt = *targets[did];
        const int w = std::min(ct.width, static_cast<int>(aoFbw * 64u)) * ct.sx, h = std::min(ct.height, rows) * ct.sy;
        if (w <= 0 || h <= 0)
            return;
        if (aoMode() == 0 && ssrMode() == 0 && !sunLit()) // (bloom alone)
        {
            bloomPass(ct, w, h);
            return;
        }
        std::vector<float> dist;
        dist.swap(distVerts);
        if (!distPass(dist, ct, dt, w, h))
        {
            ++statAoSkipped;
            return;
        }
        // Lighting step 3: the reflections first (they read the 3D before the occlusion darkens it).
        if (ssrMode() != 0)
            ssrPass(ssr, ct, dt, w, h, rows, A, B);
        if (aoMode() == 0 && !sunLit())
        {
            bloomPass(ct, w, h);
            return;
        }
        if (aoTexW < w || aoTexH < h)
        {
            if (aoTex)
                glDeleteTextures(1, &aoTex);
            aoTexW = std::max(aoTexW, w);
            aoTexH = std::max(aoTexH, h);
            glGenTextures(1, &aoTex);
            glBindTexture(GL_TEXTURE_2D, aoTex);
            glTexStorage2D(GL_TEXTURE_2D, 1, GL_R32UI, aoTexW, aoTexH);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
            glBindTexture(GL_TEXTURE_2D, 0);
        }
        static const float radius = [] {
            const char *v = std::getenv("GT4_AO_RADIUS");
            return v && *v ? std::max(0.05f, static_cast<float>(std::atof(v))) : 6.0f; // (GT4 units: a car is ~10 wide)
        }();
        static const float strength = [] {
            const char *v = std::getenv("GT4_AO_STRENGTH");
            return v && *v ? std::max(0.0f, static_cast<float>(std::atof(v))) : 1.0f;
        }();
        const float depthScale[2] = {static_cast<float>(dt.sx) / static_cast<float>(ct.sx), static_cast<float>(dt.sy) / static_cast<float>(ct.sy)};
        const auto common = [&](const GLint *loc) {
            glActiveTexture(GL_TEXTURE14);
            glBindTexture(GL_TEXTURE_2D, distTex);
            glUniform1i(loc[0], 14);
            glUniform2i(loc[1], w, h);
            glUniform2f(loc[2], depthScale[0], depthScale[1]);
            glUniform2f(loc[3], static_cast<float>(ct.sx), static_cast<float>(ct.sy));
            glUniform2f(loc[4], static_cast<float>(A), static_cast<float>(B));
            glUniform4f(loc[5], aoProj[0], aoProj[1], aoProj[2], aoProj[3]);
            glUniform4i(loc[8], aoSkip[0] * ct.sx, aoSkip[1] * ct.sy, aoSkip[2] * ct.sx + ct.sx - 1, aoSkip[3] * ct.sy + ct.sy - 1);
        };
        const int show = aoMode() == 2 ? 1 : aoMode() == 3 ? 2 : aoMode() == 4 ? 3 : aoMode() == 5 ? 4 : 0;
        // (relighting: GT4's sun and the world's up, from the newest car body's light set; none yet: no relighting)
        float L[3] = {}, up[3] = {0.0f, -1.0f, 0.0f};
        uint64_t sunFrame = 0;
        const bool sunOk = sunLit() && gt4tap::sun(L, up, sunFrame);
        if (!relightLocs)
        {
            for (int i = 0; i < 2; ++i)
            {
                const GLuint prog = i == 0 ? aoProgram : aoApplyProgram;
                relightLoc[i][0] = glGetUniformLocation(prog, "u_sun");
                relightLoc[i][1] = glGetUniformLocation(prog, "u_up");
                relightLoc[i][2] = glGetUniformLocation(prog, "u_relightFrom");
                relightLoc[i][3] = glGetUniformLocation(prog, "u_aoOn");
                relightLoc[i][4] = glGetUniformLocation(prog, "u_relightShow");
                relightLoc[i][5] = glGetUniformLocation(prog, "u_shadow");
                relightLoc[i][6] = glGetUniformLocation(prog, "u_glow");
            }
            relightLocs = true;
        }
        static const unsigned relightFrom = [] {
            const char *v = std::getenv("GT4_RELIGHT_FROM"); // (the first depth partition relit: 1, the sky's left alone)
            return v && *v ? static_cast<unsigned>(std::atoi(v)) : 1u;
        }();
        static const int relightShow = [] {
            const char *v = std::getenv("GT4_RELIGHT_SHOW");
            return !v ? 0 : *v == '1' ? 1 : std::strcmp(v, "split") == 0 ? 2 : 0;
        }();
        const auto relightUniforms = [&](int i) {
            glUniform4f(relightLoc[i][0], L[0], L[1], L[2], sunOk ? relightStrength() : 0.0f);
            glUniform3f(relightLoc[i][1], up[0], up[1], up[2]);
            glUniform1ui(relightLoc[i][2], relightFrom);
            glUniform1f(relightLoc[i][3], aoMode() != 0 ? 1.0f : 0.0f);
            glUniform1i(relightLoc[i][4], sunOk ? relightShow : 0);
            glUniform1f(relightLoc[i][5], sunOk ? shadowStrength() : 0.0f);
            glUniform1f(relightLoc[i][6], sunOk ? glowStrength() : 0.0f);
        };
        if (sunOk && statRelightLogs++ % 1200u == 0u)
            std::fprintf(stderr, "[gt4-relight] strength %.2f (shadows %.2f): GT4's sun %.3f %.3f %.3f, up %.3f %.3f %.3f (%.0f deg up), from VU1 frame %llu\n",
                         relightStrength(), shadowStrength(), L[0], L[1], L[2], up[0], up[1], up[2],
                         std::asin(std::clamp(L[0] * up[0] + L[1] * up[1] + L[2] * up[2], -1.0f, 1.0f)) * 57.29578f,
                         static_cast<unsigned long long>(sunFrame));
        {
            glUseProgram(aoProgram);
            common(aoLoc);
            glUniform2f(aoLoc[6], radius, aoMode() != 0 ? strength : 0.0f); // (relighting alone: no occlusion)
            glUniform1i(aoLoc[7], show);
            relightUniforms(0);
            glBindImageTexture(0, aoTex, 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_R32UI);
            glDispatchCompute(static_cast<GLuint>((w + 7) / 8), static_cast<GLuint>((h + 7) / 8), 1);
            glMemoryBarrier(GL_TEXTURE_FETCH_BARRIER_BIT | GL_SHADER_IMAGE_ACCESS_BARRIER_BIT);
            glBindImageTexture(0, 0, 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_R32UI);
        }
        // (GT4_AO_PROBE=1, checking: the occlusion texture's values now and then, and the uniform locations)
        static const bool probe = [] {
            const char *v = std::getenv("GT4_AO_PROBE");
            return v && *v == '1';
        }();
        if (probe && aoMode() != 0 && statAoFrames % 600u == 0u)
        {
            std::vector<uint32_t> px(static_cast<size_t>(aoTexW) * static_cast<size_t>(aoTexH));
            glBindTexture(GL_TEXTURE_2D, aoTex);
            glGetTexImage(GL_TEXTURE_2D, 0, GL_RED_INTEGER, GL_UNSIGNED_INT, px.data());
            glBindTexture(GL_TEXTURE_2D, 0);
            uint32_t lo = ~0u, hi = 0u;
            uint64_t sum = 0, n = 0;
            for (int y = 0; y < h; y += 7)
                for (int x = 0; x < w; x += 7)
                {
                    const uint32_t v = px[static_cast<size_t>(y) * static_cast<size_t>(aoTexW) + static_cast<size_t>(x)];
                    lo = std::min(lo, v);
                    hi = std::max(hi, v);
                    sum += v;
                    ++n;
                }
            std::fprintf(stderr, "[gt4-ao] probe: occlusion texture %dx%d (of %dx%d): min %u max %u mean %.1f; locations", w, h, aoTexW, aoTexH,
                         lo, hi, n ? static_cast<double>(sum) / static_cast<double>(n) : 0.0);
            for (int i = 0; i < 8; ++i)
                std::fprintf(stderr, " %d", aoLoc[i]);
            std::fprintf(stderr, " /");
            for (int i = 0; i < 8; ++i)
                std::fprintf(stderr, " %d", aoApplyLoc[i]);
            std::fprintf(stderr, "\n");
        }
        glUseProgram(aoApplyProgram);
        common(aoApplyLoc);
        glActiveTexture(GL_TEXTURE15);
        glBindTexture(GL_TEXTURE_2D, aoTex);
        glUniform1i(aoApplyLoc[6], 15);
        glUniform1i(aoApplyLoc[7], show);
        relightUniforms(1);
        glBindImageTexture(1, ct.tex, 0, GL_FALSE, 0, GL_READ_WRITE, GL_R32UI);
        glDispatchCompute(static_cast<GLuint>((w + 7) / 8), static_cast<GLuint>((h + 7) / 8), 1);
        glMemoryBarrier(GL_ALL_BARRIER_BITS);
        glBindImageTexture(1, 0, 0, GL_FALSE, 0, GL_READ_WRITE, GL_R32UI);
        glActiveTexture(GL_TEXTURE15);
        glBindTexture(GL_TEXTURE_2D, 0);
        glActiveTexture(GL_TEXTURE14);
        glBindTexture(GL_TEXTURE_2D, 0);
        glActiveTexture(GL_TEXTURE0);
        glUseProgram(0);
        glStateDirty = true;
        forgetBindings();
        bloomPass(ct, w, h);
        if (++statAoFrames % 600u == 1u)
            std::fprintf(stderr, "[gt4-ao] frame %llu: 3D %03x (depth %03x, scale %d/%d) %dx%d samples, Z = %.6f + %.6f / w from %.0f vertices "
                                 "(mean 1/Q %.2f), focal %.1f x %.1f centre %.1f, %.1f; skipped %llu; depth partitions %.2f a frame\n",
                         static_cast<unsigned long long>(statAoFrames), aoFbp, aoZbp, dt.sx, ct.sx, w, h, A, B, aoN, aoN / std::max(aoSq, 1e-9),
                         aoProj[0], aoProj[1], aoProj[2], aoProj[3], static_cast<unsigned long long>(statAoSkipped),
                         static_cast<double>(statDistParts) / static_cast<double>(std::max<uint64_t>(statAoFrames, 1u)));
    }
#endif
#if defined(PS2X_GAME_GT4) && !defined(GT4_STOCK_BUILD)
    // Lighting step 1d (GT4_VU1_TAP): the 3D scene's per-pixel motion, from the tapped triangles. Each frame the
    // triangles whose three corners carry a tap with last frame's position are kept (GS pixels of their frame
    // buffer, GS Z, motion in GS pixels); when the frame's 3D is done (tapFrameDone) they are drawn into a motion
    // texture (RG16F, the 3D buffer's internal size) with their own depth test (the nearest wins; triangles that
    // write no Z, the sky, count as farthest). GT4_VU1_TAP=gbuffer: colour pictures of it (checking only).
    // A frame's 3D is done at the first draw that reads the 3D buffer as a texture (GT4's post effects and its copy
    // to the shown buffer; checked with GT4_TAP_SEQ: GT4's GS presents in the middle of the next frame's 3D, the
    // presentation is no frame boundary), or, failing that, when a triangle of the next VU1 frame comes.
    struct TapTri
    {
        uint32_t fbp, fbw;
        float v[3][5]; // x, y (frame pixels), depth 0..1, motion x, y
        uint32_t part; // GT4's depth partition run it belongs to (tapPartSeq; see distPass): depth cleared between runs
    };
    std::vector<TapTri> tapTris;
    uint32_t tapPartSeq = 0;
    int tapPartition = 99;
    GLuint tapProgram = 0, tapFbo = 0, tapDepth = 0, tapMotion = 0, tapBuffer = 0;
    int tapW = 0, tapH = 0;
    GLint tapLoc[3] = {-1, -1, -1};
    uint64_t statTapTris = 0, statTapClipTris = 0, statTapParts = 0;
    // The collected triangles' VU1 frame and 3D buffer; frames ended by a texture read / by the next frame.
    uint64_t tapCurFrame = ~0ull, statTapEndRead = 0, statTapEndNext = 0;
    uint32_t tapSceneFbp = ~0u;

    void tapCollect(const GSPrimitiveBatch &batch)
    {
        if (batch.vertexCount != 3u || batch.state.prim.type == GS_PRIM_SPRITE || tapTris.size() > 400000u)
            return;
        TapTri t;
        t.fbp = batch.state.context.frame.fbp;
        t.fbw = std::max<uint32_t>(batch.state.context.frame.fbw, 1u);
        uint64_t frame = 0;
        if (!gt4tap::markFrame(batch.vertices[0].tapSlot, frame))
            return;
        if (!tapTris.empty() && frame != tapCurFrame)
        {
            ++statTapEndNext;
            tapFrameDone();
        }
        tapCurFrame = frame;
        const float ofx = batch.state.context.xyoffset.ofx / 16.0f, ofy = batch.state.context.xyoffset.ofy / 16.0f;
        // (no Z written: farthest, below anything that writes Z)
        const bool writesZ = ((batch.state.context.test >> 16) & 1u) != 0u && !batch.state.context.zbuf.zmask;
        bool clip = false;
        for (int i = 0; i < 3; ++i)
        {
            const GSVertex &v = batch.vertices[i];
            float px = 0.0f, py = 0.0f;
            if (!gt4tap::vertexPrev(v.tapSlot, v.tapK, v.x, v.y, v.q, px, py))
                return;
            t.v[i][0] = v.x - ofx;
            t.v[i][1] = v.y - ofy;
            t.v[i][2] = writesZ ? static_cast<float>(v.z / 16777216.0) : 1.0e-7f;
            t.v[i][3] = v.x - px;
            t.v[i][4] = v.y - py;
            clip = clip || (v.tapSlot & gt4tap::kClipMark) != 0u;
        }
        statTapClipTris += clip ? 1u : 0u;
        {
            double zA = 0.0, zB = 0.0;
            if (writesZ && gt4tap::zMapping(batch.vertices[0].tapSlot, zA, zB) && zB > 0.0)
            {
                const int part = static_cast<int>(std::lround(std::log10(zB)));
                if (part != tapPartition)
                {
                    tapPartition = part;
                    ++tapPartSeq;
                }
            }
        }
        t.part = tapPartSeq;
        tapSceneFbp = t.fbp;
        if (writesZ && batch.state.context.zbuf.psm == GS_PSM_Z24) // (DLSS's scene depth: a Z24 buffer only)
            tapSceneZbp = batch.state.context.zbuf.zbp;
        tapTris.push_back(t);
    }

    // A draw about to read a texture (not a tapped one): the end of the frame's 3D when it reads the 3D buffer.
    void tapNoteRead(const GSPrimitiveBatch &batch)
    {
        const uint32_t tbp = batch.state.context.tex0.tbp0, dstFbp = batch.state.context.frame.fbp;
        // (only into another buffer: GT4 also reads it into itself before and during its 3D)
        if (!tapTris.empty() && tbp == tapSceneFbp * 32u && dstFbp != tapSceneFbp)
        {
            ++statTapEndRead;
            tapEndByRead = true;
            tapFrameDone();
            tapEndByRead = false;
        }
#if defined(VKGL_REDIRECT)
        static std::unordered_map<uint32_t, int> readsLogged; // (per scene buffer and destination: the first 3)
        if (dlssOut && tbp == dlssSceneFbp * 32u && dstFbp != dlssSceneFbp && readsLogged[(dlssSceneFbp << 16) | dstFbp]++ < 3)
        {
            const GSVertex &a = batch.vertices[0];
            std::fprintf(stderr, "[dlss] scene %03x read into %03x: prim %d fst %d, %u vertices, first xy %.1f,%.1f uv %.1f,%.1f st %.4f,%.4f\n",
                         dlssSceneFbp, dstFbp, static_cast<int>(batch.state.prim.type), batch.state.prim.fst ? 1 : 0,
                         static_cast<unsigned>(batch.vertexCount), a.x - batch.state.context.xyoffset.ofx / 16.0f,
                         a.y - batch.state.context.xyoffset.ofy / 16.0f, a.u / 16.0f, a.v / 16.0f, a.s, a.t);
        }
        // DLSS: where the finished 3D buffer goes in the buffers it is copied into (sprites, texel for pixel).
        if (dlssOut && tbp == dlssSceneFbp * 32u && dstFbp != dlssSceneFbp && batch.state.prim.type == GS_PRIM_SPRITE &&
            batch.vertexCount >= 2u && batch.state.prim.fst)
        {
            const GSVertex &a = batch.vertices[0], &b = batch.vertices[1];
            const float ofx = batch.state.context.xyoffset.ofx / 16.0f, ofy = batch.state.context.xyoffset.ofy / 16.0f;
            const float x0 = a.x - ofx, y0 = a.y - ofy, x1 = b.x - ofx, y1 = b.y - ofy;
            const float u0 = a.u / 16.0f, v0 = a.v / 16.0f, u1 = b.u / 16.0f, v1 = b.v / 16.0f;
            DlssCopy &m = dlssCopies[dstFbp];
            m.oneToOne = std::fabs((x1 - x0) - (u1 - u0)) < 0.5f && std::fabs((y1 - y0) - (v1 - v0)) < 0.5f;
            // (pixel p's centre samples u0 + p - x0: texel p + floor(u0 - x0); GT4 draws from -0.5 with u 0)
            m.dx = -static_cast<int>(std::floor(u0 - x0 + 0.001f));
            m.dy = -static_cast<int>(std::floor(v0 - y0 + 0.001f));
            m.serial = dlssSerial;
        }
        // (HUD mode: a copy into a shown buffer, at the picture's scale; DLSS over it once the copy is done)
        if (dlssHudRender && tbp == dlssSceneFbp * 32u && dstFbp != dlssSceneFbp && batch.state.prim.type == GS_PRIM_SPRITE &&
            batch.vertexCount >= 2u && batch.state.prim.fst && sceneFbps.count((dstFbp & (kPageCount - 1u)) | (batch.state.context.frame.fbw << 16)))
        {
            const GSVertex &a = batch.vertices[0], &b = batch.vertices[1];
            const float ofx = batch.state.context.xyoffset.ofx / 16.0f, ofy = batch.state.context.xyoffset.ofy / 16.0f;
            const float x0 = a.x - ofx, y0 = a.y - ofy, x1 = b.x - ofx, y1 = b.y - ofy;
            const float u0 = a.u / 16.0f, v0 = a.v / 16.0f, u1 = b.u / 16.0f, v1 = b.v / 16.0f;
            if (std::fabs((x1 - x0) - (u1 - u0)) < 0.5f && std::fabs((y1 - y0) - (v1 - v0)) < 0.5f)
            {
                if (!dlssPending || dlssPend.fbp != dstFbp || dlssPend.serial != dlssSerial)
                {
                    dlssPend = DlssPend{};
                    dlssPend.fbp = dstFbp;
                    dlssPend.fbw = std::max<uint32_t>(batch.state.context.frame.fbw, 1u);
                    dlssPend.dx = -static_cast<int>(std::floor(u0 - x0 + 0.001f));
                    dlssPend.dy = -static_cast<int>(std::floor(v0 - y0 + 0.001f));
                    dlssPend.serial = dlssSerial;
                    dlssPending = true;
                }
                dlssPend.w = std::max(dlssPend.w, static_cast<int>(std::ceil(std::max(u0, u1) - 0.001f)));
                dlssPend.h = std::max(dlssPend.h, static_cast<int>(std::ceil(std::max(v0, v1) - 0.001f)));
            }
        }
#endif
    }
#if defined(VKGL_REDIRECT)
    // The copy (HUD mode) into a shown buffer waiting for DLSS: a draw that is not part of it applies DLSS first.
    bool dlssCopyPart(const GSPrimitiveBatch &batch) const
    {
        return batch.state.prim.tme && batch.state.prim.type == GS_PRIM_SPRITE && batch.state.context.tex0.tbp0 == dlssSceneFbp * 32u &&
               batch.state.context.frame.fbp == dlssPend.fbp;
    }
#endif
    bool tapEndByRead = false;
    uint32_t tapSceneZbp = ~0u;
#if defined(VKGL_REDIRECT)
    // DLSS step 2 (gt4_upscale.h, GT4_UPSCALER=dlss): at the end of a frame's 3D (ended by a read) its motion and
    // depth are kept (copies: the presentation comes in the middle of the next frame's 3D) with its jitter; the 3D
    // buffer's copies into other buffers note their offset; the presentation of one of those buffers then has DLSS
    // make the picture at dlssOut samples per GS pixel from the one at scaleX (gpuPresent).
    int dlssOut = 0;
    GLuint dlssIn = 0, dlssOutTex = 0;
    int dlssInW = 0, dlssInH = 0, dlssOutW = 0, dlssOutH = 0;
    uint32_t dlssSceneFbp = ~0u;
    uint64_t dlssSerial = 0, dlssShownSerial = 0, dlssEvaluations = 0, dlssPlain = 0, dlssRepeats = 0;
    // A kept frame: its motion and depth (copies), its jitter, its number (serial). A few are kept: GT4 shows the
    // buffer it finished a frame before the one it is drawing (the presentation comes in the middle of the next
    // frame's 3D), and a frame's 3D can end twice (a read into the post-effect buffer, more 3D, the copy to the shown
    // buffer): the same VU1 frame then keeps its slot, the motion pass adding to its motion.
    struct DlssSlot
    {
        GLuint depth = 0, motion = 0;
        int w = 0, h = 0;
        float jx = 0.0f, jy = 0.0f;
        uint64_t serial = 0, vuFrame = ~0ull;
        bool gsDepth = false;
    };
    DlssSlot dlssSlots[4];
    int dlssCur = -1;
    struct DlssCopy
    {
        int dx = 0, dy = 0;
        bool oneToOne = false;
        uint64_t serial = 0;
    };
    std::unordered_map<uint32_t, DlssCopy> dlssCopies;
    struct DlssPend
    {
        uint32_t fbp = 0, fbw = 1;
        int dx = 0, dy = 0, w = 0, h = 0; // the copy's offset into the shown buffer, the 3D buffer's part copied (GS pixels)
        uint64_t serial = 0;
    };
    DlssPend dlssPend;
    bool dlssPending = false;
    uint32_t dlssDumpShown = ~0u;
    // GT4_DLSS_DUMP: the shown buffer at its presentation after a dumped DLSS frame (the HUD drawn over it).
    void dlssDumpPresented(uint32_t fbp, int ox, int oy, int width, int rows)
    {
        if (dlssDumpShown != fbp)
            return;
        dlssDumpShown = ~0u;
        static const char *dump = std::getenv("GT4_DLSS_DUMP");
        for (const auto &tp : targets)
            if (tp && !tp->depth && tp->tex && tp->fbp == fbp && tp->sx == scaleX)
            {
                const int texW = tp->width * tp->sx, texH = tp->height * tp->sy;
                const int cw = std::min(width * scaleX, texW - ox * scaleX), ch = std::min(rows * scaleY, texH - oy * scaleY);
                if (cw <= 0 || ch <= 0)
                    return;
                std::vector<uint8_t> all(static_cast<size_t>(texW) * static_cast<size_t>(texH) * 4u), crop(static_cast<size_t>(cw) * ch * 4u);
                glBindTexture(GL_TEXTURE_2D, tp->tex);
                glGetTexImage(GL_TEXTURE_2D, 0, GL_RGBA, GL_UNSIGNED_BYTE, all.data());
                glBindTexture(GL_TEXTURE_2D, 0);
                for (int y = 0; y < ch; ++y)
                    std::memcpy(crop.data() + static_cast<size_t>(y) * cw * 4u,
                                all.data() + (static_cast<size_t>(oy * scaleY + y) * texW + static_cast<size_t>(ox * scaleX)) * 4u, static_cast<size_t>(cw) * 4u);
                for (size_t i = 3; i < crop.size(); i += 4)
                    crop[i] = 255;
                char path[1024];
                std::snprintf(path, sizeof(path), "%s/hud-%06llu-shown.png", dump, static_cast<unsigned long long>(dlssHudEvaluations));
                gt4tap::savePng(path, cw, ch, crop.data());
                forgetBindings();
                return;
            }
    }
    uint64_t dlssHudEvaluations = 0, dlssHudMissed = 0;

#if defined(PS2X_GAME_GT4) && !defined(GT4_STOCK_BUILD)
    // The rear-view mirror page (gt4_mirror.h): at a copy of GT4's 3D buffer (FBP 0x0d2) into another buffer, while
    // someone watches and GT4 drew its mirror this frame, the mirror's rectangle of the 3D buffer's texture (at its scale)
    // read back without waiting; the reads that have finished go to the encoder.
    uint64_t mirrorReads = 0, mirrorRoads = 0;
    // GT4_MIRROR=page: the road under the mirror, kept from the 3D buffer's texture before GT4's mirror pass (its first draw
    // into the 3D buffer clipped to the mirror's rectangle) and put back at the copy, after the read.
    GLuint mirrorRoad = 0;
    int mirrorRoadW = 0, mirrorRoadH = 0;
    struct MirrorKept
    {
        GLuint target = 0;
        int x = 0, y = 0, w = 0, h = 0; // (texels)
        bool show = true;               // (the mirror belongs on the game screen: GT4's own, or GT4_MIRROR=both)
        bool remove = false;            // (the road goes back over it once its pass ends)
        bool done = false;              // (read, and the road back where it leaves the screen: mirrorFinish)
    } mirrorKept;
    const Target *mirrorScene() const
    {
        // (HUD hand-off 7 C: only GT4's whole 3D buffer. GT4 also draws a 128 x 256 picture at FBP 0x0d2 with a narrower
        // width now and then; when that one was the latest used, the mirror's road was not kept, so GT4's own mirror showed
        // on the screen for that frame with GT4_MIRROR=page and with the mirror moved, and the page missed the frame.)
        const Target *scene = nullptr;
        int n = 0;
        for (const auto &tp : targets)
            if (tp && !tp->depth && tp->tex && tp->fbp == 0xd2u && tp->layout == Layout::C32)
            {
                ++n;
                if (tp->width >= 512 && tp->height >= 256 && (!scene || tp->lastUse > scene->lastUse))
                    scene = tp.get();
            }
        // (GT4_HUD_MOVE_LOG / GT4_MIRROR_LOG: the 3D buffer's targets, each time their number changes)
        static const bool log = [] {
            const char *a = std::getenv("GT4_HUD_MOVE_LOG"), *b = std::getenv("GT4_MIRROR_LOG");
            return (a && *a == '1') || (b && *b == '1');
        }();
        static int lastN = -1, said = 0;
        if (log && n != lastN && said < 20)
        {
            lastN = n, ++said;
            for (const auto &tp : targets)
                if (tp && !tp->depth && tp->tex && tp->fbp == 0xd2u)
                    std::fprintf(stderr, "[gt4-mirror] FBP 0d2 target: %dx%d fbw %u layout %d scale %dx%d last use %llu%s%c", tp->width, tp->height, tp->fbw,
                                 static_cast<int>(tp->layout), tp->sx, tp->sy, static_cast<unsigned long long>(tp->lastUse), tp.get() == scene ? " (the 3D buffer)" : "", 10);
        }
        return scene;
    }
    void mirrorKeepRoad(const GSPrimitiveBatch &batch)
    {
        int x = 0, y = 0, w = 0, h = 0;
        const auto &sc = batch.state.context.scissor;
        // (GT4_HUD_MOVE_LOG: the 3D buffer's draws this frame and their scissors, up to four kinds)
        ++mvD2;
        {
            const uint32_t key = (static_cast<uint32_t>(sc.x0) << 21) ^ (static_cast<uint32_t>(sc.y0) << 11) ^ (static_cast<uint32_t>(sc.x1) << 1) ^ sc.y1;
            bool have = false;
            for (int k = 0; k < mvScN; ++k)
                have = have || mvScKey[k] == key;
            if (!have && mvScN < 4)
                mvScKey[mvScN] = key, mvSc[mvScN][0] = sc.x0, mvSc[mvScN][1] = sc.y0, mvSc[mvScN][2] = sc.x1, mvSc[mvScN][3] = sc.y1, ++mvScN;
        }
        // (1 GT4's own mirror, 2 one drawn only for the page: GT4_MIRROR=screen's other views, one row shorter; gt4_mirror.h)
        const int kind = gt4mirror::scissorKind(static_cast<int>(sc.x0), static_cast<int>(sc.y0), static_cast<int>(sc.x1), static_cast<int>(sc.y1));
        if (!kind && mirrorKept.target)
        {
            // (d: a draw into the 3D buffer after the mirror's pass began, before the copy: the road kept lacks it)
            // (21 Oct: every race frame draws more into the 3D buffer after the mirror's pass (the effects over the road); the road
            // kept lacked them, so where it went back at the copy a box of road without them showed at the mirror's place.
            // The mirror's pass is over at the first such draw: read it and put the road back there, under them.)
            if (!mirrorKept.done)
            {
                if (mvEv.size() < 64u)
                    mvEv += 'd';
                mirrorFinish();
            }
            return;
        }
        if (!kind || !gt4mirror::lastRect(x, y, w, h))
            return;
        if (kind == 2)
            --h;
        if (mirrorKept.target)
        {
            if ((mvEv.empty() || mvEv.back() != 'k') && mvEv.size() < 64u)
                mvEv += 'k'; // (a mirror draw while the road is kept already)
            return;
        }
        const Target *scene = mirrorScene();
        if (!scene || x + w > scene->width || y + h > scene->height)
            return;
        const int tw = w * scene->sx, th = h * scene->sy;
        if (!mirrorRoad || mirrorRoadW < tw || mirrorRoadH < th)
        {
            if (mirrorRoad)
                releaseTexture(mirrorRoad);
            mirrorRoadW = std::max(mirrorRoadW, tw), mirrorRoadH = std::max(mirrorRoadH, th);
            mirrorRoad = makeColorTexture(mirrorRoadW, mirrorRoadH);
        }
        glCopyImageSubData(scene->tex, GL_TEXTURE_2D, 0, x * scene->sx, y * scene->sy, 0, mirrorRoad, GL_TEXTURE_2D, 0, 0, 0, 0, tw, th, 1);
        // (on the screen: GT4's own mirror unless GT4_MIRROR=page; a page-only one never. Off it: the road goes back; a moved
        // mirror (GT4_HUD_MOVE) always leaves GT4's place and is drawn at its own when it belongs on the screen)
        static const int mode = gt4mirror::mode();
        static const bool moved = gt4hudmove::mirrorMoved();
        const bool show = kind == 1 ? mode != 2 : mode == 1;
        mirrorKept = MirrorKept{scene->tex, x * scene->sx, y * scene->sy, tw, th, show, !show || moved};
        if (mvEv.size() < 64u)
            mvEv += kind == 2 ? 'P' : 'K';
    }
    // (GT4_HUD_MOVE_LOG, HUD hand-off 7 C: per frame, from one RaceDisplay begin marker to the next, what the moved mirror's
    // path did: K the road kept at the mirror's first draw, k more mirror draws, C a copy of the 3D buffer that put the road
    // back, T one whose 3D target was not the kept one, c a copy with nothing kept (once per run of copies), D the moved mirror
    // drawn at the begin marker; a frame unlike its neighbours logged)
    std::string mvEv, mvLastEv;
    int mvSaid = 0, mvD2 = 0, mvScN = 0;
    uint32_t mvScKey[4] = {};
    int mvSc[4][4] = {};
    uint64_t mvFrames = 0;
    // The launcher's race pictures (gt4_hud_pictures.h): once a frame while a capture is armed, the whole 3D buffer read
    // back at its copy (GT4's mirror taken out: the road kept under it put in for the read, the mirror put back after).
    bool picReadDone = false;
    uint64_t picReads = 0;
    GLuint picMirror = 0;
    int picMirrorW = 0, picMirrorH = 0;
    void drainReads()
    {
        std::vector<uint8_t> px;
        int rw = 0, rh = 0;
        uint32_t tag = 0;
        while (vkgl::readTake(px, rw, rh, tag))
        {
            if (tag == 0u)
                gt4mirror::submit(std::move(px), rw, rh);
            else
                gt4hudpic::sceneRead(tag, std::move(px), rw, rh);
        }
    }
    // The mirror's pass is over (the first other draw into the 3D buffer after it, or the copy): the page's picture read, the
    // moved mirror kept, and the road put back where the mirror leaves the game screen. Once a frame.
    void mirrorFinish()
    {
        mirrorKept.done = true;
        bool alive = false;
        for (const auto &tp : targets)
            alive = alive || (tp && tp->tex == mirrorKept.target);
        if (!alive)
            return;
        int x = 0, y = 0, w = 0, h = 0;
        static const bool mirrorPage = gt4mirror::mode() == 2;
        static const bool mirrorMove = gt4hudmove::mirrorMoved();
        const Target *scene = mirrorScene();
        // (the page's picture of the mirror, before the road goes back over it)
        if (gt4mirror::takeRect(x, y, w, h) && gt4mirror::wanted() &&
            vkgl::readStart(mirrorKept.target, mirrorKept.x, mirrorKept.y, mirrorKept.w, mirrorKept.h, 0u))
        {
            if (mvEv.size() < 64u)
                mvEv += 'R';
            if (mirrorReads++ == 0)
                std::fprintf(stderr, "[gt4-mirror] reading the mirror back as its pass ends: %d,%d %dx%d texels%s",
                             mirrorKept.x, mirrorKept.y, mirrorKept.w, mirrorKept.h, "\n");
        }
        if (mirrorMove && mirrorKept.show)
        {
            if (!movedMirror || movedMirrorW < mirrorKept.w || movedMirrorH < mirrorKept.h)
            {
                if (movedMirror)
                    releaseTexture(movedMirror);
                movedMirrorW = std::max(movedMirrorW, mirrorKept.w), movedMirrorH = std::max(movedMirrorH, mirrorKept.h);
                movedMirror = makeColorTexture(movedMirrorW, movedMirrorH);
            }
            glCopyImageSubData(mirrorKept.target, GL_TEXTURE_2D, 0, mirrorKept.x, mirrorKept.y, 0, movedMirror, GL_TEXTURE_2D, 0, 0, 0, 0, mirrorKept.w,
                               mirrorKept.h, 1);
            movedTw = mirrorKept.w, movedTh = mirrorKept.h;
            const int ksx = scene ? scene->sx : sceneScaleX(), ksy = scene ? scene->sy : sceneScaleY();
            movedFx = static_cast<float>(mirrorKept.x) / ksx, movedFy = static_cast<float>(mirrorKept.y) / ksy;
            movedFw = static_cast<float>(mirrorKept.w) / ksx, movedFh = static_cast<float>(mirrorKept.h) / ksy;
            movedMirrorReady = true;
        }
        if (mirrorKept.remove)
            glCopyImageSubData(mirrorRoad, GL_TEXTURE_2D, 0, 0, 0, 0, mirrorKept.target, GL_TEXTURE_2D, 0, mirrorKept.x, mirrorKept.y, 0, mirrorKept.w,
                               mirrorKept.h, 1);
        if (mirrorKept.remove && !mirrorMove && mirrorRoads++ == 0)
            std::fprintf(stderr, "[gt4-mirror] GT4_MIRROR=%s: the road put back under the mirror (%dx%d texels at %d,%d)\n", mirrorPage ? "page" : "screen", mirrorKept.w,
                         mirrorKept.h, mirrorKept.x, mirrorKept.y);
    }
    void mirrorCapture()
    {
        drainReads();
        // (HUD hand-off 7 C; 21 Oct, every mode: the mirror is read once its pass is over (mirrorFinish; its road kept at its
        // first draw, mirrorKeepRoad), just before the road goes back where it leaves the screen. A copy of the 3D buffer comes
        // before GT4's mirror pass in each race frame (cKC): reading at the first copy gave the page the sky under the mirror.
        // GT4_HUD_MOVE's mirror: kept aside there and drawn at its new place at the start of GT4's HUD, mirrorMoveDraw.)
        const bool pic = gt4hudpic::gsArmed() && !picReadDone;
        const Target *scene = mirrorScene();
        bool mirrorSaved = false;
        if (!mirrorKept.target && (mvEv.empty() || mvEv.back() != 'c') && mvEv.size() < 64u)
            mvEv += 'c';
        // (the road back, after the read was recorded: the copy then takes the 3D without the mirror)
        if (mirrorKept.target)
        {
            // (the kept 3D buffer itself while it is still a target: the newest 3D target is not always it (T), and a road
            // not put back showed GT4's mirror on the screen for a frame)
            bool alive = false;
            for (const auto &tp : targets)
                alive = alive || (tp && tp->tex == mirrorKept.target);
            if (mvEv.size() < 64u)
                mvEv += scene && scene->tex == mirrorKept.target ? 'C' : alive ? 'T' : 'X';
            if (alive)
            {
                if (!mirrorKept.done)
                    mirrorFinish();
                if (pic && !mirrorKept.remove)
                {
                    // (a race picture: the mirror kept aside, put back once the picture is read)
                    if (!picMirror || picMirrorW < mirrorKept.w || picMirrorH < mirrorKept.h)
                    {
                        if (picMirror)
                            releaseTexture(picMirror);
                        picMirrorW = std::max(picMirrorW, mirrorKept.w), picMirrorH = std::max(picMirrorH, mirrorKept.h);
                        picMirror = makeColorTexture(picMirrorW, picMirrorH);
                    }
                    glCopyImageSubData(mirrorKept.target, GL_TEXTURE_2D, 0, mirrorKept.x, mirrorKept.y, 0, picMirror, GL_TEXTURE_2D, 0, 0, 0, 0, mirrorKept.w,
                                       mirrorKept.h, 1);
                    mirrorSaved = true;
                }
                if (mirrorSaved)
                    glCopyImageSubData(mirrorRoad, GL_TEXTURE_2D, 0, 0, 0, 0, mirrorKept.target, GL_TEXTURE_2D, 0, mirrorKept.x, mirrorKept.y, 0, mirrorKept.w,
                                       mirrorKept.h, 1);
            }
        }
        if (pic && scene)
        {
            picReadDone = true;
            const uint32_t tag = gt4hudpic::sceneReadTag();
            // (GT4's 3D buffer is 640 x 448; its target may be taller)
            const int rows = std::min(scene->height, 448);
            if (vkgl::readStart(scene->tex, 0, 0, scene->width * scene->sx, rows * scene->sy, tag) && picReads++ < 4)
                std::fprintf(stderr, "[gt4-hud-pic] reading the 3D buffer back for a race picture: %dx%d at %dx%d%s\n", scene->width, rows, scene->sx,
                             scene->sy, mirrorSaved ? " (the mirror taken out)" : "");
        }
        if (mirrorSaved)
            glCopyImageSubData(picMirror, GL_TEXTURE_2D, 0, 0, 0, 0, mirrorKept.target, GL_TEXTURE_2D, 0, mirrorKept.x, mirrorKept.y, 0, mirrorKept.w,
                               mirrorKept.h, 1);
        mirrorKept = MirrorKept{};
    }
    // GT4_HUD_MOVE's mirror (gt4_hud_move.h): GT4's mirror as kept at the frame's copy (movedMirror, texels of the 3D buffer),
    // drawn into the shown buffer at its new place and size (bilinear) when GT4's HUD begins (the RaceDisplay marker), so
    // GT4's HUD stays over it as before. A frame without GT4's mirror draws none.
    GLuint movedMirror = 0;
    int movedMirrorW = 0, movedMirrorH = 0, movedTw = 0, movedTh = 0;
    float movedFx = 0, movedFy = 0, movedFw = 0, movedFh = 0; // (GT4's place, frame pixels)
    bool movedMirrorReady = false;
    int movedAge = 1000; // (begin markers since the moved mirror was last kept)
    uint32_t movedCopyFbp = ~0u;
    uint64_t movedDraws = 0;
    void mirrorMoveDraw()
    {
        if (!movedMirrorReady)
            return;
        movedMirrorReady = false;
        float cx = 0, cy = 0, s = 1, fw = 640, fh = 448;
        if (!gt4hudmove::mirrorPlace(cx, cy, s, fw, fh) || movedCopyFbp == ~0u)
            return;
        const Target *shown = nullptr;
        for (const auto &tp : targets)
            if (tp && !tp->depth && tp->tex && tp->fbp == movedCopyFbp && (!shown || tp->lastUse > shown->lastUse))
                shown = tp.get();
        if (!shown)
            return;
        const float w = movedFw * s, h = movedFh * s;
        const float x0 = std::clamp(cx - w * 0.5f, 0.0f, std::max(0.0f, fw - w)), y0 = std::clamp(cy - h * 0.5f, 0.0f, std::max(0.0f, fh - h));
        const int dx0 = static_cast<int>(std::lround(x0 * shown->sx)), dy0 = static_cast<int>(std::lround(y0 * shown->sy));
        const int dx1 = std::min(shown->width * shown->sx, static_cast<int>(std::lround((x0 + w) * shown->sx)));
        const int dy1 = std::min(shown->height * shown->sy, static_cast<int>(std::lround((y0 + h) * shown->sy)));
        if (dx1 - dx0 < 2 || dy1 - dy0 < 2)
            return;
        flushPendingPatches();
        flushBatch();
        blitTex(false, movedMirror, 0, 0, movedTw, movedTh, shown->tex, dx0, dy0, dx1, dy1, GL_LINEAR);
        if (movedDraws++ == 0)
            std::fprintf(stderr, "[gt4-hud-move] the mirror moved: GT4's %.0f,%.0f %.0fx%.0f drawn at %.1f,%.1f %.1fx%.1f (FBP %03x, %dx%d)\n", movedFx,
                         movedFy, movedFw, movedFh, x0, y0, w, h, movedCopyFbp, shown->sx, shown->sy);
    }
#endif

    // HUD mode: DLSS from the 3D buffer (3D scale) over its copy in the shown buffer (the picture's scale), before
    // anything else is drawn there.
    void dlssApply()
    {
        dlssPending = false;
        const DlssPend p = dlssPend;
        const DlssSlot *k = nullptr;
        for (const DlssSlot &c : dlssSlots)
            if (c.serial != 0 && c.serial == p.serial)
                k = &c;
        const Target *scene = nullptr, *shown = nullptr;
        for (const auto &tp : targets)
            if (tp && !tp->depth && tp->tex)
            {
                if (tp->fbp == dlssSceneFbp && tp->fbw == p.fbw && tp->sx == sceneScaleX())
                    scene = tp.get();
                if (tp->fbp == p.fbp && tp->fbw == p.fbw && tp->sx == scaleX)
                    shown = tp.get();
            }
        const int rx = sceneScaleX(), ry = sceneScaleY();
        const int w = std::min(p.w, 640), h = std::min(p.h, 512);
        if (!k || !scene || !shown || w < 64 || h < 64 || w * rx > k->w || h * ry > k->h || w * rx > scene->width * scene->sx ||
            h * ry > scene->height * scene->sy || p.dx < 0 || p.dy < 0 || (p.dx + w) * scaleX > shown->width * shown->sx ||
            (p.dy + h) * scaleY > shown->height * shown->sy)
        {
            if (dlssHudMissed++ < 8)
            {
                std::string have;
                for (const auto &tp : targets)
                    if (tp && (tp->fbp == dlssSceneFbp || tp->fbp == p.fbp))
                        have += " " + std::to_string(tp->fbp) + "/" + std::to_string(tp->fbw) + (tp->depth ? "z" : "c") + "@" + std::to_string(tp->sx);
                std::fprintf(stderr, "[dlss] copy into %03x not upscaled: kept frame %s, 3D buffer %s, shown buffer %s, %dx%d at %d,%d (targets:%s)\n",
                             p.fbp, k ? "found" : "gone", scene ? "found" : "missing", shown ? "found" : "missing", w, h, p.dx, p.dy, have.c_str());
            }
            dlssLastOk = false;
            return;
        }
        flushBatch();
        static const float jitterSign = envFlag("GT4_DLSS_JITTER_FLIP", false) ? -1.0f : 1.0f;
        static const float motionSign = envFlag("GT4_DLSS_MOTION_FLIP", false) ? 1.0f : -1.0f;
        gt4up::Frame f;
        f.color = dlssImage(scene->tex, 0, 0, w * rx, h * ry);
        f.depth = dlssImage(k->depth, 0, 0, w * rx, h * ry);
        f.motion = dlssImage(k->motion, 0, 0, w * rx, h * ry);
        f.out = dlssImage(shown->tex, p.dx * scaleX, p.dy * scaleY, w * scaleX, h * scaleY);
        f.jitterX = k->jx * jitterSign;
        f.jitterY = k->jy * jitterSign;
        f.motionScaleX = motionSign / static_cast<float>(w);
        f.motionScaleY = motionSign / static_cast<float>(h);
        f.reset = !dlssLastOk || dlssShownSerial + 1u != k->serial;
        f.split = &vkgl::splitExternal;
        DlssCall call{&f, false};
        const GLuint reads[] = {scene->tex, k->depth, k->motion};
        const GLuint writes[] = {shown->tex};
        vkgl::recordExternal(reads, 3, writes, 1, &dlssRecord, &call);
        dlssLastOk = call.ok;
        if (!call.ok)
            return;
        dlssShownSerial = k->serial;
        ++dlssHudEvaluations;
        if ((dlssHudEvaluations % 600u) == 1u)
            std::fprintf(stderr, "[dlss] %llu frames upscaled before the HUD (%dx%d -> %dx%d), %llu copies missed\n",
                         static_cast<unsigned long long>(dlssHudEvaluations), w * rx, h * ry, w * scaleX, h * scaleY,
                         static_cast<unsigned long long>(dlssHudMissed));
        // GT4_DLSS_DUMP=<folder> (checking): the 3D buffer and DLSS's output, 3 frames in a row every 300.
        static const char *dump = std::getenv("GT4_DLSS_DUMP");
        if (dump && *dump && (dlssHudEvaluations % 300u) < 3u && dlssHudEvaluations > 3u)
        {
            const auto save = [&](GLuint tex, int texW, int texH, int x, int y, int cw, int ch, const char *what)
            {
                std::vector<uint8_t> all(static_cast<size_t>(texW) * static_cast<size_t>(texH) * 4u), crop(static_cast<size_t>(cw) * ch * 4u);
                glBindTexture(GL_TEXTURE_2D, tex);
                glGetTexImage(GL_TEXTURE_2D, 0, GL_RGBA, GL_UNSIGNED_BYTE, all.data());
                glBindTexture(GL_TEXTURE_2D, 0);
                for (int yy = 0; yy < ch; ++yy)
                    std::memcpy(crop.data() + static_cast<size_t>(yy) * cw * 4u, all.data() + (static_cast<size_t>(y + yy) * texW + x) * 4u, static_cast<size_t>(cw) * 4u);
                for (size_t i = 3; i < crop.size(); i += 4)
                    crop[i] = 255;
                char path[1024];
                std::snprintf(path, sizeof(path), "%s/hud-%06llu-%s.png", dump, static_cast<unsigned long long>(dlssHudEvaluations), what);
                gt4tap::savePng(path, cw, ch, crop.data());
            };
            save(scene->tex, scene->width * scene->sx, scene->height * scene->sy, 0, 0, w * rx, h * ry, "in");
            save(shown->tex, shown->width * shown->sx, shown->height * shown->sy, p.dx * scaleX, p.dy * scaleY, w * scaleX, h * scaleY, "out");
            dlssDumpShown = p.fbp; // (and the buffer as shown, with the HUD, at its presentation)
            forgetBindings();
        }
    }

    // At a frame's end (tapRender, the motion just drawn): its motion, depth and jitter kept.
    void dlssKeep(uint32_t fbp, uint32_t fbw, int width, int height, int w, int h)
    {
        // (a whole picture's 3D only: GT4 also draws reflection maps and such into smaller ones, ended by reads too)
        if ((!dlssOut && !dlssHudRender) || !tapEndByRead || width < 512 || height < 384)
            return;
        if (dlssCur < 0 || dlssSlots[dlssCur].vuFrame != tapCurFrame)
        {
            dlssCur = (dlssCur + 1) % 4;
            DlssSlot &n = dlssSlots[dlssCur];
            n.vuFrame = tapCurFrame;
            n.serial = ++dlssSerial;
            upscaleJitterOf(tapCurFrame, n.jx, n.jy);
        }
        DlssSlot &k = dlssSlots[dlssCur];
        if (!k.depth || k.w < tapW || k.h < tapH)
        {
            releaseTexture(k.depth);
            releaseTexture(k.motion);
            k.w = tapW;
            k.h = tapH;
            k.depth = makeTargetTexture(true, k.w, k.h);
            glGenTextures(1, &k.motion);
            glBindTexture(GL_TEXTURE_2D, k.motion);
            glTexStorage2D(GL_TEXTURE_2D, 1, GL_RG16F, k.w, k.h);
            glBindTexture(GL_TEXTURE_2D, 0);
        }
        glCopyImageSubData(tapMotion, GL_TEXTURE_2D, 0, 0, 0, 0, k.motion, GL_TEXTURE_2D, 0, 0, 0, 0, w, h, 1);
        k.gsDepth = false;
        for (const auto &tp : targets)
            if (tp && tp->depth && tp->layout == Layout::Z32 && tp->fbp == tapSceneZbp && tp->fbw == fbw && tp->tex && tp->sx == sceneScaleX() &&
                tp->sy == sceneScaleY())
            {
                const int cw = std::min(w, tp->width * tp->sx), ch = std::min(h, tp->height * tp->sy);
                if (clearTexImage)
                    clearTexImage(k.depth, 0, GL_DEPTH_COMPONENT, GL_FLOAT, nullptr);
                glCopyImageSubData(tp->tex, GL_TEXTURE_2D, 0, 0, 0, 0, k.depth, GL_TEXTURE_2D, 0, 0, 0, 0, cw, ch, 1);
                k.gsDepth = true;
                break;
            }
        if (!k.gsDepth) // (the motion pass's own depth: the tapped triangles')
            glCopyImageSubData(tapDepth, GL_TEXTURE_2D, 0, 0, 0, 0, k.depth, GL_TEXTURE_2D, 0, 0, 0, 0, w, h, 1);
        dlssSceneFbp = fbp;
    }

    // The presentation draws its picture (inW x inH, scaleX) here instead of into its slot.
    GLuint dlssInput(int inW, int inH)
    {
        if (!dlssIn || dlssInW < inW || dlssInH < inH)
        {
            releaseTexture(dlssIn);
            dlssInW = std::max(dlssInW, inW);
            dlssInH = std::max(dlssInH, inH);
            dlssIn = makeColorTexture(dlssInW, dlssInH);
        }
        return dlssIn;
    }

    struct DlssCall
    {
        const gt4up::Frame *f;
        bool ok;
    };
    static void dlssRecord(void *cmd, void *user)
    {
        DlssCall *c = static_cast<DlssCall *>(user);
        c->ok = gt4up::evaluate(cmd, *c->f);
    }
    static gt4up::Image dlssImage(GLuint tex, int x, int y, int w, int h)
    {
        gt4up::Image i;
        vkgl::NativeImage n;
        if (vkgl::nativeImage(tex, n))
        {
            i.image = n.image;
            i.view = n.view;
            i.format = n.format;
            i.usage = n.usage;
            i.width = n.width;
            i.height = n.height;
        }
        i.x = x;
        i.y = y;
        i.w = w;
        i.h = h;
        return i;
    }

    // The picture (dlssIn, inW x inH) of buffer fbp's GS pixels (ox, oy, width x rows) into the slot `out` (outW x
    // outH): DLSS when the newest kept frame was copied into this buffer texel for texel, its last output again for up
    // to 2 presentations without a new frame (GT4 below 60), else the picture as it is, each sample repeated.
    int dlssRepeat = 0;
    bool dlssLastOk = false;
    void dlssPresent(GLuint out, uint32_t fbp, int ox, int oy, int width, int rows, int inW, int inH, int outW, int outH)
    {
        flushBatch();
        if (!dlssOutTex || dlssOutW < outW || dlssOutH < outH)
        {
            releaseTexture(dlssOutTex);
            dlssOutW = std::max(dlssOutW, outW);
            dlssOutH = std::max(dlssOutH, outH);
            dlssOutTex = makeColorTexture(dlssOutW, dlssOutH);
        }
        // The kept frame that was copied into this buffer.
        const auto it = dlssCopies.find(fbp);
        const DlssSlot *k = nullptr;
        if (it != dlssCopies.end() && it->second.oneToOne)
            for (const DlssSlot &c : dlssSlots)
                if (c.serial != 0 && c.serial == it->second.serial)
                    k = &c;
        const bool fresh = k && k->serial != dlssShownSerial;
        bool done = false;
        if (fresh && inH == rows * scaleY && inW == width * scaleX)
        {
            const int sx = (ox - it->second.dx) * scaleX, sy = (oy - it->second.dy) * scaleY;
            if (sx >= 0 && sy >= 0 && sx + inW <= k->w && sy + inH <= k->h)
            {
                static const float jitterSign = envFlag("GT4_DLSS_JITTER_FLIP", false) ? -1.0f : 1.0f;
                static const float motionSign = envFlag("GT4_DLSS_MOTION_FLIP", false) ? 1.0f : -1.0f;
                gt4up::Frame f;
                f.color = dlssImage(dlssIn, 0, 0, inW, inH);
                f.depth = dlssImage(k->depth, sx, sy, inW, inH);
                f.motion = dlssImage(k->motion, sx, sy, inW, inH);
                f.out = dlssImage(dlssOutTex, 0, 0, outW, outH);
                f.jitterX = k->jx * jitterSign;
                f.jitterY = k->jy * jitterSign;
                // (motion: this frame's position minus last frame's, in GS pixels; DLSS wants the way back, in the
                // picture's [0, 1])
                f.motionScaleX = motionSign / static_cast<float>(width);
                f.motionScaleY = motionSign / static_cast<float>(rows);
                f.reset = !dlssLastOk || dlssShownSerial + 1u != k->serial;
                f.split = &vkgl::splitExternal;
                DlssCall call{&f, false};
                const GLuint reads[] = {dlssIn, k->depth, k->motion};
                const GLuint writes[] = {dlssOutTex};
                vkgl::recordExternal(reads, 3, writes, 1, &dlssRecord, &call);
                done = call.ok;
            }
        }
        else if (dlssSerial != 0)
        {
            static std::unordered_map<uint64_t, int> whyLogged; // (per buffer and size: the first 3)
            if (whyLogged[(static_cast<uint64_t>(fbp) << 32) | (static_cast<uint64_t>(inH) << 16) | static_cast<uint64_t>(rows)]++ < 3)
                std::fprintf(stderr, "[dlss] not used for buffer %03x (%d,%d %dx%d, picture %dx%d at %dx): copy %s, kept frame %s%s\n", fbp, ox,
                             oy, width, rows, inW, inH, scaleX, it == dlssCopies.end() ? "none" : it->second.oneToOne ? "1:1" : "scaled",
                             k ? "found" : "gone", k && !fresh ? " (shown)" : "");
        }
        if (done)
        {
            ++dlssEvaluations;
            dlssShownSerial = k->serial;
            dlssRepeat = 0;
            dlssLastOk = true;
            // GT4_DLSS_DUMP=<folder> (checking): DLSS's input and output as PNGs, 3 frames in a row every 300.
            static const char *dump = std::getenv("GT4_DLSS_DUMP");
            if (dump && *dump && (dlssEvaluations % 300u) < 3u && dlssEvaluations > 3u)
            {
                const auto save = [&](GLuint tex, int texW, int texH, int w, int h, const char *what)
                {
                    std::vector<uint8_t> all(static_cast<size_t>(texW) * static_cast<size_t>(texH) * 4u), crop(static_cast<size_t>(w) * h * 4u);
                    glBindTexture(GL_TEXTURE_2D, tex);
                    glGetTexImage(GL_TEXTURE_2D, 0, GL_RGBA, GL_UNSIGNED_BYTE, all.data());
                    glBindTexture(GL_TEXTURE_2D, 0);
                    for (int y = 0; y < h; ++y)
                        std::memcpy(crop.data() + static_cast<size_t>(y) * w * 4u, all.data() + static_cast<size_t>(y) * texW * 4u, static_cast<size_t>(w) * 4u);
                    for (size_t i = 3; i < crop.size(); i += 4)
                        crop[i] = 255;
                    char path[1024];
                    std::snprintf(path, sizeof(path), "%s/dlss-%06llu-%s.png", dump, static_cast<unsigned long long>(dlssEvaluations), what);
                    gt4tap::savePng(path, w, h, crop.data());
                };
                save(dlssIn, dlssInW, dlssInH, inW, inH, "in");
                save(dlssOutTex, dlssOutW, dlssOutH, outW, outH, "out");
                forgetBindings();
            }
        }
        // (the same frame shown again, GT4 below 60: DLSS's last picture, up to twice)
        if (done || (dlssLastOk && k && k->serial == dlssShownSerial && dlssRepeat < 2))
        {
            if (!done)
            {
                ++dlssRepeat;
                ++dlssRepeats;
            }
            glCopyImageSubData(dlssOutTex, GL_TEXTURE_2D, 0, 0, 0, 0, out, GL_TEXTURE_2D, 0, 0, 0, 0, outW, outH, 1);
        }
        else
        {
            dlssLastOk = false;
            ++dlssPlain;
            blitTex(false, dlssIn, 0, 0, inW, inH, out, 0, 0, outW, outH);
        }
        if (((dlssEvaluations + dlssPlain + dlssRepeats) % 600u) == 1u)
            std::fprintf(stderr, "[dlss] presentations: %llu through DLSS, %llu repeated, %llu plain; kept frames %llu\n",
                         static_cast<unsigned long long>(dlssEvaluations), static_cast<unsigned long long>(dlssRepeats),
                         static_cast<unsigned long long>(dlssPlain), static_cast<unsigned long long>(dlssSerial));
        if (shotFromSlot) // (GT4_SHOT_DIR: this presentation's picture, as it goes to the window)
        {
            shotFromSlot = false;
            static const char *dir = std::getenv("GT4_SHOT_DIR");
            vkgl::NativeImage n;
            if (dir && vkgl::nativeImage(out, n) && outW <= n.width && outH <= n.height)
            {
                std::vector<uint8_t> all(static_cast<size_t>(n.width) * static_cast<size_t>(n.height) * 4u), crop(static_cast<size_t>(outW) * outH * 4u);
                glBindTexture(GL_TEXTURE_2D, out);
                glGetTexImage(GL_TEXTURE_2D, 0, GL_RGBA, GL_UNSIGNED_BYTE, all.data());
                glBindTexture(GL_TEXTURE_2D, 0);
                for (int y = 0; y < outH; ++y)
                    std::memcpy(crop.data() + static_cast<size_t>(y) * outW * 4u, all.data() + static_cast<size_t>(y) * n.width * 4u, static_cast<size_t>(outW) * 4u);
                for (size_t i = 3; i < crop.size(); i += 4)
                    crop[i] = 255;
                char path[1024];
                std::snprintf(path, sizeof(path), "%s/shot-%06llu.png", dir, static_cast<unsigned long long>(shotPresents));
                gt4tap::savePng(path, outW, outH, crop.data());
                forgetBindings();
            }
        }
    }
#endif
    void tapFrameDone()
    {
        static const bool debug = gt4tap::debugView() == 3;
        tapRender(debug);
    }

    // At the end of a frame's 3D: its 3D buffer (the one that got the most tapped triangles: GT4 draws its 3D scene
    // into one buffer and copies it to the shown one) into the motion texture, the size of that buffer at the
    // internal scale. GT4_VU1_TAP=gbuffer: also colour pictures (the 3D buffer, the motion, the depth), saved as PNG
    // for 2 frames in every 300 to GT4_VU1_TAP_DUMP (a folder; checking only). tapPresents: presentations (GT4_TAP_SEQ).
    uint64_t tapPresents = 0, tapFrames = 0, tapLastFrame = ~0ull;
    uint32_t tapLastFbp = ~0u;
    int tapLastW = 0, tapLastH = 0;
    GLuint tapDebugTex = 0;
    void tapRender(bool debug)
    {
        ++tapFrames;
        if (tapTris.empty())
            return;
        std::unordered_map<uint32_t, size_t> per;
        for (const TapTri &t : tapTris)
            ++per[t.fbp];
        uint32_t fbp = 0;
        size_t most = 0;
        for (const auto &[f, n] : per)
            if (n > most)
            {
                most = n;
                fbp = f;
            }
        uint32_t fbw = 1;
        float maxY = 0.0f;
        std::vector<float> verts;
        verts.reserve(most * 24u);
        // (where GT4's depth partition changes: GS Z is only comparable within one; the depth is cleared between them)
        std::vector<size_t> partStarts;
        uint32_t lastPart = ~0u;
        for (const TapTri &t : tapTris)
            if (t.fbp == fbp)
            {
                fbw = t.fbw;
                if (t.part != lastPart)
                {
                    partStarts.push_back(verts.size() / 8u);
                    lastPart = t.part;
                }
                for (int i = 0; i < 3; ++i)
                {
                    verts.insert(verts.end(), {t.v[i][0], t.v[i][1], t.v[i][2], 0.0f, t.v[i][3], t.v[i][4], 0.0f, 0.0f});
                    maxY = std::max(maxY, t.v[i][1]);
                }
            }
        tapTris.clear();
        const int width = static_cast<int>(fbw * 64u), height = std::clamp(((static_cast<int>(maxY) + 32) / 32) * 32, 32, 512);
        if ((tapFrames % 300u) == 0u && verts.size() >= 24u)
            std::fprintf(stderr, "[gt4-tap] motion pass %llu: buffer %03x/%u %dx%d, %zu triangles (clipper so far %llu; frames ended by a read %llu, by the next frame %llu); first (%.1f,%.1f z %.4f m %.2f,%.2f) (%.1f,%.1f) (%.1f,%.1f)\n",
                         static_cast<unsigned long long>(tapFrames), fbp, fbw, width, height, verts.size() / 24u,
                         static_cast<unsigned long long>(statTapClipTris), static_cast<unsigned long long>(statTapEndRead),
                         static_cast<unsigned long long>(statTapEndNext), verts[0], verts[1], verts[2], verts[4],
                         verts[5], verts[8], verts[9], verts[16], verts[17]);
        const GLuint into = debug ? tapDebugTex : 0;
        if (!tapProgram)
        {
            const std::string vs = R"GLSL(#version 430 core
layout(std430, binding = 2) readonly buffer TapVerts { vec4 tv[]; };
uniform vec2 u_size;
out vec2 v_motion;
out float v_depth;
void main()
{
    vec4 a = tv[gl_VertexID * 2], b = tv[gl_VertexID * 2 + 1];
    v_motion = b.xy;
    v_depth = a.z;
    gl_Position = vec4(a.xy / u_size * 2.0 - 1.0, 0.0, 1.0);
}
)GLSL";
            const std::string fs = R"GLSL(#version 430 core
in vec2 v_motion;
in float v_depth;
uniform int u_debug;
out vec4 o_color;
void main()
{
    gl_FragDepth = v_depth;
    o_color = u_debug != 0 ? vec4(min(abs(v_motion) * (8.0 / 255.0), vec2(1.0)), 0.5, 1.0) : vec4(v_motion, 0.0, 1.0);
}
)GLSL";
            const GLuint a = compile(GL_VERTEX_SHADER, vs), b = compile(GL_FRAGMENT_SHADER, fs);
            if (!a || !b)
                return;
            tapProgram = glCreateProgram();
            glAttachShader(tapProgram, a);
            glAttachShader(tapProgram, b);
            glLinkProgram(tapProgram);
            glDeleteShader(a);
            glDeleteShader(b);
            tapLoc[0] = glGetUniformLocation(tapProgram, "u_size");
            tapLoc[1] = glGetUniformLocation(tapProgram, "u_debug");
            glGenFramebuffers(1, &tapFbo);
            glGenBuffers(1, &tapBuffer);
        }
        const int w = width * sceneScaleX(), h = height * sceneScaleY();
        if (w > tapW || h > tapH)
        {
            releaseTexture(tapDepth);
            releaseTexture(tapMotion);
            tapW = std::max(w, tapW);
            tapH = std::max(h, tapH);
            tapDepth = makeTargetTexture(true, tapW, tapH);
            glGenTextures(1, &tapMotion);
            glBindTexture(GL_TEXTURE_2D, tapMotion);
            glTexStorage2D(GL_TEXTURE_2D, 1, GL_RG16F, tapW, tapH);
            glBindTexture(GL_TEXTURE_2D, 0);
            releaseTexture(tapDebugTex);
            tapDebugTex = makeColorTexture(tapW, tapH);
        }
        const GLuint target = debug ? tapDebugTex : tapMotion;
        flushBatch();
        glBindFramebuffer(GL_FRAMEBUFFER, tapFbo);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, target, 0);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, tapDepth, 0);
        glDrawBuffer(GL_COLOR_ATTACHMENT0);
        glViewport(0, 0, w, h);
        glDisable(GL_SCISSOR_TEST);
        glDisable(GL_BLEND);
        glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
        glDepthMask(GL_TRUE);
        // (the same VU1 frame's 3D going on after a read into another buffer: added to its motion, not cleared)
        const bool continuing = !debug && tapLastFrame == tapCurFrame && tapLastFbp == fbp && tapLastW == w && tapLastH == h;
        tapLastFrame = tapCurFrame;
        tapLastFbp = fbp;
        tapLastW = w;
        tapLastH = h;
        // (vkgl has no glClear: whole-texture clears to zero; depth 0 = farthest for GT4's GREATER test)
        if (continuing)
            ;
        else if (clearTexImage)
        {
            clearTexImage(target, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
            clearTexImage(tapDepth, 0, GL_DEPTH_COMPONENT, GL_FLOAT, nullptr);
        }
        else
        {
            glClearDepth(0.0);
            glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
            glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        }
        glEnable(GL_DEPTH_TEST);
        glDepthFunc(GL_GREATER);
        if (!verts.empty())
        {
            glUseProgram(tapProgram);
            glBindVertexArray(vao);
            glUniform2f(tapLoc[0], static_cast<float>(width), static_cast<float>(height));
            glUniform1i(tapLoc[1], debug ? 1 : 0);
            const size_t count = verts.size() / 8u;
            for (size_t p = 0; p < partStarts.size(); ++p)
            {
                const size_t first = partStarts[p], last = p + 1 < partStarts.size() ? partStarts[p + 1] : count;
                if (last <= first)
                    continue;
                // (a later partition after GT4's Z clear; the first goes on from a continued frame's depth)
                if (p > 0 && clearTexImage)
                    clearTexImage(tapDepth, 0, GL_DEPTH_COMPONENT, GL_FLOAT, nullptr);
                // (each run its own upload, drawn from vertex 0: gl_VertexID with a first vertex is not the same everywhere)
                glBindBuffer(GL_SHADER_STORAGE_BUFFER, tapBuffer);
                glBufferData(GL_SHADER_STORAGE_BUFFER, static_cast<GLsizeiptr>((last - first) * 8u * sizeof(float)), verts.data() + first * 8u,
                             GL_STREAM_DRAW);
                glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 2, tapBuffer);
                glDrawArrays(GL_TRIANGLES, 0, static_cast<GLsizei>(last - first));
            }
            glBindVertexArray(0);
            glUseProgram(0);
            statTapTris += verts.size() / 24u;
            statTapParts += partStarts.size();
        }
        glDisable(GL_DEPTH_TEST);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        glStateDirty = true;
        (void)into;
#if defined(VKGL_REDIRECT)
        if (!debug)
            dlssKeep(fbp, fbw, width, height, w, h);
#endif
        static const char *dump = std::getenv("GT4_VU1_TAP_DUMP");
        if (debug && dump && *dump && (tapFrames % 300u) <= 1u)
        {
            // (with it: the 3D buffer's own picture, and the depth the motion pass kept, brighter = nearer)
            for (const auto &tp : targets)
                if (tp && !tp->depth && tp->fbp == fbp && tp->fbw == fbw && tp->tex)
                {
                    const int tw = tp->width * tp->sx, th = tp->height * tp->sy;
                    const int cw = std::min(tw, tp->sx * width), ch = std::min(th, tp->sy * height);
                    std::vector<uint8_t> all(static_cast<size_t>(tw) * static_cast<size_t>(th) * 4u);
                    glBindTexture(GL_TEXTURE_2D, tp->tex);
                    glGetTexImage(GL_TEXTURE_2D, 0, GL_RGBA, GL_UNSIGNED_BYTE, all.data());
                    glBindTexture(GL_TEXTURE_2D, 0);
                    std::vector<uint8_t> crop(static_cast<size_t>(cw) * static_cast<size_t>(ch) * 4u);
                    for (int y = 0; y < ch; ++y)
                        std::memcpy(crop.data() + static_cast<size_t>(y) * cw * 4u, all.data() + static_cast<size_t>(y) * tw * 4u, static_cast<size_t>(cw) * 4u);
                    for (size_t i = 3; i < crop.size(); i += 4)
                        crop[i] = 255;
                    char path[1024];
                    std::snprintf(path, sizeof(path), "%s/scene-%06llu-%03x.png", dump, static_cast<unsigned long long>(tapFrames), fbp);
                    gt4tap::savePng(path, cw, ch, crop.data());
                    break;
                }
            {
                std::vector<float> d(static_cast<size_t>(tapW) * static_cast<size_t>(tapH));
                glBindTexture(GL_TEXTURE_2D, tapDepth);
                glGetTexImage(GL_TEXTURE_2D, 0, GL_DEPTH_COMPONENT, GL_FLOAT, d.data());
                glBindTexture(GL_TEXTURE_2D, 0);
                float hi = 0.0f;
                for (int y = 0; y < h; ++y)
                    for (int x = 0; x < w; ++x)
                        hi = std::max(hi, d[static_cast<size_t>(y) * tapW + x]);
                std::vector<uint8_t> g(static_cast<size_t>(w) * static_cast<size_t>(h) * 4u);
                for (int y = 0; y < h; ++y)
                    for (int x = 0; x < w; ++x)
                    {
                        const float v = d[static_cast<size_t>(y) * tapW + x];
                        const uint8_t c = (hi > 0.0f && v > 0.0f) ? static_cast<uint8_t>(std::clamp(std::sqrt(v / hi), 0.0f, 1.0f) * 255.0f) : 0u;
                        uint8_t *o = g.data() + (static_cast<size_t>(y) * w + x) * 4u;
                        o[0] = o[1] = o[2] = c;
                        o[3] = 255;
                    }
                char path[1024];
                std::snprintf(path, sizeof(path), "%s/depth-%06llu-%03x.png", dump, static_cast<unsigned long long>(tapFrames), fbp);
                gt4tap::savePng(path, w, h, g.data());
            }
            std::vector<uint8_t> px(static_cast<size_t>(tapW) * static_cast<size_t>(tapH) * 4u);
            glBindTexture(GL_TEXTURE_2D, tapDebugTex);
            glGetTexImage(GL_TEXTURE_2D, 0, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
            glBindTexture(GL_TEXTURE_2D, 0);
            forgetBindings();
            std::vector<uint8_t> crop(static_cast<size_t>(w) * static_cast<size_t>(h) * 4u);
            for (int y = 0; y < h; ++y)
                std::memcpy(crop.data() + static_cast<size_t>(y) * w * 4u, px.data() + static_cast<size_t>(y) * tapW * 4u, static_cast<size_t>(w) * 4u);
            for (size_t i = 3; i < crop.size(); i += 4)
                crop[i] = 255;
            char path[1024];
            std::snprintf(path, sizeof(path), "%s/motion-%06llu-%03x.png", dump, static_cast<unsigned long long>(tapFrames), fbp);
            gt4tap::savePng(path, w, h, crop.data());
        }
    }
#endif

    void forgetTargetTex(GLuint tex)
    {
        if (texScales.empty() && targetTexs.empty())
            return;
        texScales.erase(tex);
        targetTexs.erase(tex);
    }
    // Frame base pages (| width << 16) of targets found to be scene targets (drawn with a depth buffer, displayed).
    std::unordered_set<uint32_t> sceneFbps;
    // Texture smoothing (GT4_TEX_SMOOTH=on, the Full edition's Display > Detail and textures; analysis/design/tex-smooth):
    // at an internal resolution of 2x and above (scaleX >= 2), a 3D triangle GT4 textures with nearest sampling is
    // filtered bilinearly instead (after the palette lookup, as GT4's own bilinear textures are). Not sprites (HUD,
    // fonts, 2D menus, copies), not textures read from a target (post effects, reflections, the mirror) or Z. Off by
    // default; never at native (exact). Not in the Classic build. With it the programs also get GS_SHADE_EXACT
    // (texSmoothDefines, gs_gl_shaders.h shadeSlack): the Gouraud colour's float rounding no longer fails GT4's alpha
    // test on Trial Mountain's rock faces (the dotted grid); the shader applies it at 2x and above only.
    bool texSmooth = false;
    bool texSmoothForced = false; // the primitive in Submit is one this smoothing made bilinear
    // GT3_GS_GL_STATS: textured 3D triangles made bilinear, of those set back (a target's texture), already bilinear in GT4.
    uint64_t statSmoothPrims = 0, statSmoothTarget = 0, statSmoothAlready = 0;
    GLuint blitFbo[2] = {0, 0};
    GLuint nativeColor = 0, nativeDepth = 0; // 64x64 GS pixels, for scaled reads and writes
    // Where each texel of a PSMT8 (128x64) / PSMT4 (128x128) page lies in the
    // same page read as PSMCT32 (64x32): R16UI x | y << 6 | byte << 11 (| nibble << 13).
    GLuint lut8 = 0, lut4 = 0;
    // Page moves between targets on the GPU (a Z buffer and a colour buffer
    // sharing memory): the pixel of the other 32-bit layout holding the same
    // word, R16UI x | y << 6, and the program that copies/converts a page.
    GLuint lutZfromC = 0, lutCfromZ = 0, convProgram = 0, convFbo = 0;
    // Vulkan: the partial-word merge (u_lut 6) as its own program, reading its
    // own pixel of the target as u_dst (an input attachment read in vkgl).
    GLuint mergeProgram = 0;
    bool layoutTablesOk = false;
    // PSMCT16 <-> PSMCT32 pages: a CT16 pixel's CT32 pixel and half, and a
    // CT32 pixel's two CT16 pixels (low half, high half).
    GLuint lut16from32 = 0, lut32from16 = 0;
    bool c16TablesOk = false;
    // PSMZ16 <-> PSMCT32 / PSMCT16 pages (conversion luts 7-10): a Z16 pixel's CT32 pixel and half (R16UI 64x64,
    // lut16from32's format), a CT32 pixel's two Z16 pixels (RG16UI 64x32, lut32from16's), a Z16 pixel's CT16 pixel
    // of the same halfword and a CT16 pixel's Z16 pixel (R16UI 64x64, x | y << 6).
    GLuint lutZ16from32 = 0, lut32fromZ16 = 0, lutC16ofZ16 = 0, lutZ16ofC16 = 0;
    bool z16TablesOk = false;
    // Per-format pixel offsets inside a page (fetchRaw), 128x352 R16UI.
    GLuint addrLut = 0;
    // GS mipmapping (gs_mip.h): the LOD's log2 table as a 1025x1 R32UI texture
    // on unit 12 (u_log2Lut), made when PS2X_GS_MIPMAP is on; per-pixel mip
    // primitives drawn on the GPU, and the raw textures over their levels.
    GLuint log2Lut = 0;
    uint64_t statMipPrims = 0, statMipSpanUploads = 0;
    uint64_t statRawUploads = 0;
    uint64_t statParallelBatches = 0; // batches whose masks ran on the helper threads
    bool rawTextures = true; // GT3_GS_GL_RAW_TEXTURES=0: decode on the CPU
    // GT3_GS_GL_GPU_DECODE=1: raw textures decoded once on the GPU into each
    // texel's value (a compute pass over the raw pages, cached by their
    // versions; only pages unchanged since an earlier frame), so draws read
    // one texel instead of addressing the pages per fetch (fetchRaw was ~30%
    // of the GPU time of the rally demo at 2x). Off by default: GT3 streams
    // most textures, and the decodes and lookups cost more than they saved
    // (Trial Mountain 2x 54.1 vs 57.0 fps; rally demo 2x +0.7, noise).
    GLuint decodeProgram = 0;
    GLint decodeLoc[5] = {-1, -1, -1, -1, -1}; // u_raw u_addrLut u_tex u_pages u_size
    bool gpuDecode = false;
    uint64_t statGpuDecodes = 0;
    // Fast mode (scaled, not exact): bilinear-filtered raw textures' colours
    // (palette, 16-bit expansion, TEXA alpha applied) decoded once on the GPU
    // into RGBA8 and sampled with the GPU's own bilinear filter (texMode 15)
    // instead of four addressed fetches and palette lookups per fragment
    // (texMode 13 was ~half the GPU time at 4x). Cached by the pages'
    // contents and the palette (~30 decodes a frame at Trial Mountain).
    // Default from 16 samples per pixel (4x), where the GPU limits: license
    // demo 4x 58.9 vs 56.8 fps, Trial Mountain 4x 59.1 vs 58.8; at 3x
    // 59.8 vs 60.0 and at 2x progressive 58.9 vs 59.6 it did not pay.
    // (Before the shader's register-pressure fix it lost at 4x too.)
    // GT3_GS_GL_FAST_TEXTURES=1/0: on/off at any fast-mode scale.
    GLuint colourProgram = 0;
    bool fastTexturesActive = false; // applyScalePolicies
    GLint colourLoc[12] = {};
    // 256-colour CT16 CLUTs (CSM1, CSA 0) in a GPU target's pages: copied on
    // the GPU into a CLUT ring texture as halfwords (gpuClut16), expanded with
    // TEXA at lookup (u_clut.z 2). GT3's rear-view mirror at the main view's
    // detail loads ~70 a second drawn in its Z buffer's memory; each was a
    // page read back that waited for the GPU (Trial Mountain 4x 48 fps).
    GLuint clut16Program = 0; // ~0u: failed
    GLint clut16Loc[4] = {};
    GLuint clut16Lut = 0; // 16x16 R32UI: per CLUT position x | y << 12 | high half << 24 in the target
    // gpuRawPagesTexture: its program (0 until first needed, -1u if it failed),
    // the Z32 page's word per pixel, and a raw texture per page range.
    GLuint depthRawProgram = 0;
    GLint depthRawLoc[8] = {};
    GLuint z32WordLut = 0;
    struct GpuRawEntry
    {
        GLuint tex = 0;
        uint64_t signature = 0;
    };
    std::unordered_map<uint64_t, GpuRawEntry> gpuRawTextures;
    uint64_t statGpuRawFills = 0;
    struct ColourEntry
    {
        GLuint tex = 0;
        int w = 0, h = 0;
        uint64_t lastUse = 0;
    };
    std::unordered_map<uint64_t, ColourEntry> colourTextures;
    // gpuColourTexture's last hit (element pointers stay valid until erase:
    // cleared by recycleColourTextures, the only place entries go).
    struct
    {
        ColourEntry *entry = nullptr;
        uint64_t signature = 0;
        uint32_t fields[16] = {};
    } lastColour;
    std::unordered_map<uint64_t, std::vector<GLuint>> colourPool; // unused textures by size (w << 32 | h)
    uint64_t statColourDecodes = 0, statColourHits = 0;
    uint64_t statRawHits = 0, statRawMissEpoch = 0, statRawMissKey = 0; // rawPagesTexture's last-answer cache
    // Host -> local uploads into pages a GPU target holds (CLUTs, small
    // textures in a Z buffer's or frame's pages): the words each image chunk
    // writes are drawn into the target (patchUpload) instead of copying the
    // whole page back first. Word -> pixel of a page, CT32 and Z32 swizzles.
    uint16_t pixelOfWordC[2048] = {}, pixelOfWordZ[2048] = {};
    uint16_t wordOfPixelC[2048] = {}, wordOfPixelZ[2048] = {}; // page pixel (x + 64 y) -> word
    // Words of a GPU-held page that are current in the CPU copy too (the
    // words an upload patched in, until the GPU writes the page): CPU readers
    // of only such words need no copy back.
    std::vector<uint64_t> cpuValid = std::vector<uint64_t>(kPageCount * 32u, 0ull);
    std::vector<uint8_t> cpuValidAny = std::vector<uint8_t>(kPageCount, 0u);
    std::vector<uint8_t> needPage = std::vector<uint8_t>(kPageCount, 0u);
    // A draw over (x0, y0)-(x1, y1) of target t wrote page: the CPU-valid words
    // of the 8x8 blocks it may have touched are no longer (32-bit and Z
    // layouts; a CT16 target clears the page).
    void clearCpuValidRect(const Target &t, uint32_t page, int x0, int y0, int x1, int y1)
    {
        if (!cpuValidAny[page] || t.layout == Layout::C16 || t.layout == Layout::Z16)
        {
            clearCpuValid(page);
            return;
        }
        int px = 0, py = 0;
        if (!const_cast<Target &>(t).pageRect(page, px, py))
        {
            clearCpuValid(page);
            return;
        }
        ++pageEpoch[page];
        const int ix0 = std::max(x0, px), iy0 = std::max(y0, py), ix1 = std::min(x1, px + 63), iy1 = std::min(y1, py + 31);
        if (ix0 > ix1 || iy0 > iy1)
            return;
        const uint16_t *wordOf = (t.depth || t.layout == Layout::Z32) ? wordOfPixelZ : wordOfPixelC;
        for (int ty = (iy0 - py) & ~7; ty <= iy1 - py; ty += 8)
            for (int tx = (ix0 - px) & ~7; tx <= ix1 - px; tx += 8)
                cpuValid[page * 32u + (wordOf[ty * 64 + tx] >> 6u)] = 0ull;
        bool any = false;
        for (uint32_t b = 0; b < 32u && !any; ++b)
            any = cpuValid[page * 32u + b] != 0ull;
        cpuValidAny[page] = any ? 1u : 0u;
    }

    // GT3_GS_GL_DEBUG_PAGE=<hex page>: log that page's ownership changes.
    int debugPage = [] {
        const char *v = std::getenv("GT3_GS_GL_DEBUG_PAGE");
        return (v && *v) ? static_cast<int>(std::strtoul(v, nullptr, 16)) : -1;
    }();
    void debugOwner(uint32_t page, int to, const char *what)
    {
        if (static_cast<int>(page) != debugPage)
            return;
        auto name = [this](int id) {
            char b[48];
            if (id < 0 || !targets[static_cast<size_t>(id)])
                std::snprintf(b, sizeof(b), "cpu");
            else
                std::snprintf(b, sizeof(b), "%s %03x/%u h%d", layoutName(*targets[static_cast<size_t>(id)]), targets[static_cast<size_t>(id)]->fbp,
                              targets[static_cast<size_t>(id)]->fbw, targets[static_cast<size_t>(id)]->height);
            return std::string(b);
        };
        std::fprintf(stderr, "[gs-gl-debug] frame %llu page %03x %s: %s -> %s (cause %s, batch target %d)\n", (unsigned long long)frameCounter, page,
                     what, name(owner[page]).c_str(), name(to).c_str(), cause.c_str(), haveBatch ? key.colorTarget : -1);
    }

    // Bumped when a page's CPU-valid words may change or the GPU writes it.
    std::vector<uint32_t> pageEpoch = std::vector<uint32_t>(kPageCount, 0u);
    std::unordered_map<uint64_t, uint64_t> syncOk; // syncPagesToCpu query -> page signature it needed nothing at
    void clearCpuValid(uint32_t page)
    {
        ++pageEpoch[page];
        if (!cpuValidAny[page])
            return;
        std::fill_n(&cpuValid[page * 32u], 32, 0ull);
        cpuValidAny[page] = 0u;
    }
    // The memory word (index in 32-bit words) holding pixel (x, y); ~0u for
    // formats without an address here.
    static uint32_t wordAddress(uint32_t psm, uint32_t bp, uint32_t bw, uint32_t x, uint32_t y)
    {
        using namespace GSMem;
        switch (psm)
        {
        case GS_PSM_CT32:
        case GS_PSM_CT24:
        case GS_PSM_T8H:
        case GS_PSM_T4HL:
        case GS_PSM_T4HH:
            return PixelStorageTraits<C32>::Address(PageTableC32, bp, bw, x, y) & 0xFFFFFu;
        case GS_PSM_Z32:
        case GS_PSM_Z24:
            return PixelStorageTraits<Z32>::Address(PageTableZ32, bp, bw, x, y) & 0xFFFFFu;
        case GS_PSM_CT16:
            return (PixelStorageTraits<C16>::Address(PageTableC16, bp, bw, x, y) >> 1u) & 0xFFFFFu;
        case GS_PSM_CT16S:
            return (PixelStorageTraits<C16S>::Address(PageTableC16S, bp, bw, x, y) >> 1u) & 0xFFFFFu;
        case GS_PSM_Z16:
            return (PixelStorageTraits<Z16>::Address(PageTableZ16, bp, bw, x, y) >> 1u) & 0xFFFFFu;
        case GS_PSM_Z16S:
            return (PixelStorageTraits<Z16S>::Address(PageTableZ16S, bp, bw, x, y) >> 1u) & 0xFFFFFu;
        case GS_PSM_T8:
            return (PixelStorageTraits<P8>::Address(PageTableP8, bp, bw, x, y) >> 2u) & 0xFFFFFu;
        case GS_PSM_T4:
            return (PixelStorageTraits<P4>::Address(PageTableP4, bp, bw, x, y) >> 3u) & 0xFFFFFu;
        default:
            return ~0u;
        }
    }
    std::vector<uint32_t> patchPages;   // this transfer's destination pages patched on the GPU
    std::vector<int16_t> patchSlot = std::vector<int16_t>(kPageCount, static_cast<int16_t>(-1));
    std::vector<uint32_t> patchWords;   // per patch page: 2048 words
    std::vector<uint8_t> patchMask;     // per patch page: 2048 written flags
    GSTransferCommand patchCommand{};
    GLuint patchTex[8] = {}, patchMaskTex[8] = {};
    uint32_t patchRing = 0;
    GLuint convMaskTex = 0;
    uint64_t statPatches = 0;
    // Upload patches coalesced per page: the words a run of host -> local
    // uploads writes into GPU-held pages are recorded (pendingMask) and drawn
    // in at the next backend call that is not an upload (flushPendingPatches),
    // one page at a time: a page the run wrote whole is one page upload, any
    // other page one masked patch. Meanwhile the words live in the CPU copy
    // (the CPU backend writes every upload there). Exact: between two upload
    // calls nothing reads or writes the targets (every other entry point
    // flushes first, and a page copied back is applied first), uploads of
    // distinct words commute, and the CPU copy holds each word's last value.
    // GT4's title and menus upload the 640x448 movie picture as 1,120 16x16
    // transfers every VSync into pages its boot clear handed to one target:
    // ~776 patch draws a frame, 4.5-7.6 ms of the GL thread
    // (analysis/phase5/specs/step3-menu-upload.md). GT4 default on; GT3 keeps
    // one patch per transfer unless GT3_GS_GL_PATCH_COALESCE=1.
#if defined(PS2X_GAME_GT3)
    bool coalescePatches = envFlag("GT3_GS_GL_PATCH_COALESCE", false);
#else
    bool coalescePatches = envFlag("GT3_GS_GL_PATCH_COALESCE", true);
#endif
    std::vector<uint64_t> pendingMask = std::vector<uint64_t>(kPageCount * 32u, 0ull); // per page: its 2048 words
    std::vector<uint8_t> pendingAny = std::vector<uint8_t>(kPageCount, 0u);
    // PS2X_GS_GL_PARTIAL_PATCH (analysis/phase5/specs/step4-hitches.md P1; default
    // 0 until it shows a gate benefit (step 5 verify: exact, but the loading frame
    // and race-start download time were unchanged and the patch path cost more);
    // needs the coalesced patches and a texture barrier):
    // small PSMT8 / PSMT4 uploads into pages a 32-bit colour target holds
    // (GT4's race: 3,000-8,600 per 10 s, each a page copied back with a full
    // GPU round trip before the CPU merge) are recorded as the bits of each
    // word they write (pendingBits) and merged into the target on the GPU:
    // new = (old & ~bits) | (CPU copy & bits), the CPU copy holding each
    // written bit's last upload. Depth targets still copy back.
#if defined(VKGL_REDIRECT)
    // The Vulkan build of this source (gs_vk_backend.cpp, PS2X_GS_VK): on by
    // default (night course at 4x: ~6 page downloads a frame, each a GPU drain,
    // analysis/frame-times/night-4x). The merge runs as mergeProgram, whose
    // read of its own pixel (u_dst) becomes a local read; without local read
    // (no texture barrier) partialPatchable is false and pages copy back.
    // PS2X_GS_GL_PARTIAL_PATCH=0: off.
    bool partialPatches = envFlag("PS2X_GS_GL_PARTIAL_PATCH", true);
#else
    bool partialPatches = envFlag("PS2X_GS_GL_PARTIAL_PATCH", false);
#endif
    std::vector<uint32_t> pendingBits = std::vector<uint32_t>(kPageCount * 2048u, 0u); // per page word: bits written
    std::vector<uint8_t> pendingPartial = std::vector<uint8_t>(kPageCount, 0u);
    GLuint patchBitsTex[8] = {};
    std::vector<uint32_t> applyBits = std::vector<uint32_t>(2048u, 0u);
    uint64_t statPartialPatches = 0;
    // A page a PSMT8/PSMT4 upload may merge on the GPU: held by a 32-bit colour target.
    bool partialPatchable(uint32_t page)
    {
        if (!partialPatches || !coalescePatches || !textureBarrier || !layoutTablesOk)
            return false;
        const int id = owner[page];
        if (id < 0 || !targets[id])
            return false;
        Target &t = *targets[id];
        int x = 0, y = 0;
        return !t.depth && t.layout != Layout::C16 && t.pageRect(page, x, y);
    }
    std::vector<uint32_t> pendingPages, pendingSpare;
    std::vector<uint32_t> applyWords = std::vector<uint32_t>(2048u, 0u);
    std::vector<uint8_t> applyMask = std::vector<uint8_t>(2048u, 0u);
    // Statistics: time in the patch path, uploads recorded, pages applied whole / masked.
    uint64_t statPatchNs = 0, statPatchRecorded = 0, statPatchWhole = 0, statPatchMasked = 0;
    // Diagnostics: GT3_GS_GL_NO_PATCH (copy pages back for uploads),
    // GT3_GS_GL_NO_C16MOVE (16/32-bit page moves through the CPU copy),
    // GT3_GS_GL_NO_BATCHMOVE (page conversions drawn one by one).
    bool noPatch = envFlag("GT3_GS_GL_NO_PATCH", false);
    bool noC16Move = envFlag("GT3_GS_GL_NO_C16MOVE", false);
    bool noBatchMove = envFlag("GT3_GS_GL_NO_BATCHMOVE", false);
    bool noClutMerge = envFlag("GT3_GS_GL_NO_CLUT_MERGE", false); // GT3_GS_GL_NO_CLUT_MERGE: 16-colour loads through the CPU buffer
    uint64_t statValidReads = 0; // GPU pages a CPU reader used without a copy back
    std::unordered_map<std::string, GLint> convUniforms;
    // Pages a Z buffer owns keep bits 24-31 (the high plane, which Z24 writes
    // never touch) in this colour target's alpha, or in the CPU copy (-1).
    // (A 16-bit colour target can hold it too, as its whole page: a Z buffer
    // that took a CT16 page over whole; GT4's 480p and 1080i.)
    std::array<int, kPageCount> highPlane{};
    // A Z buffer's page into a 16-bit colour target on the GPU, through a
    // 32-bit view of the page (pageView): GT4's 480p and 1080i put one
    // field's Z buffer over the other display buffer, so 50-70 pages pass
    // from Z to CT16 every frame, each a copy back to the CPU otherwise
    // (analysis/design/video-modes). PS2X_GS_GL_Z_TO_C16=0: through the CPU
    // (GT3 builds: the CPU way unless =1, as before this change).
#if defined(PS2X_GAME_GT3)
    bool zToC16Gpu = envFlag("PS2X_GS_GL_Z_TO_C16", false);
#else
    bool zToC16Gpu = envFlag("PS2X_GS_GL_Z_TO_C16", true);
#endif
    uint64_t statZToC16 = 0;
    // PSMZ16 Z buffers on the GPU (analysis/design/menus-480p/Z16-DESIGN.md): a D32F depth target holding z16 / 2^24
    // laid out as PSMZ16 pages (Layout::Z16, 64x64), Z clamped to 0xFFFF before the test and the write. GT4's 480p
    // menus draw their 3D cars with a Z16 buffer (~3,000 primitives a second through the CPU renderer before).
    // PS2X_GS_GL_Z16=0: every Z-tested primitive with a Z16 buffer is drawn by the CPU renderer, as before (GT3
    // builds: that way unless =1). Pages a Z16 target shares with a colour target or another Z16 view move on the
    // GPU (conversion luts 7-10); PS2X_GS_GL_Z16_MOVES=0 (diagnostic): through the CPU copy. Z24 <-> Z16 and
    // Z32-layout colour <-> Z16 always go through the CPU copy.
#if defined(PS2X_GAME_GT3)
    bool z16Gpu = envFlag("PS2X_GS_GL_Z16", false);
#else
    bool z16Gpu = envFlag("PS2X_GS_GL_Z16", true);
#endif
    bool z16Moves = envFlag("PS2X_GS_GL_Z16_MOVES", true);
    // GT3_GS_GL_STATS: Z16 primitives drawn on the GPU, of them with a vertex Z above 0xFFFF (clamped), Z16 page moves.
    uint64_t statZ16Prims = 0, statZ16ZAbove = 0, statZ16Moves = 0;
    // GT3_GS_GL_STATS: GT4's race colour texture reads (GSDrawState::ftex) from a 16-bit target (full colour) and
    // not (no target for them: the 16-bit value; never expected).
    uint64_t statFullTexReads = 0, statFullTexMiss = 0;
    // Page moves back without a copy (transferPage): a page that goes back on the GPU to the target it last left by
    // a GPU move, with nothing written to it since (writeSerial unchanged), is that target's again as its copy there
    // still holds it (the same words; a 16-bit target's texels also keep their full colour, which a CT16 -> CT32 ->
    // CT16 round trip of GT4's 480p texture views otherwise reduced to 16 bits). Not for Z24 buffers (their high
    // plane). PS2X_GS_GL_MOVE_BACK=0: every move copies, as before.
    bool moveBack = envFlag("PS2X_GS_GL_MOVE_BACK", true);
    uint32_t nextTargetUid = 0;
    // Per page, its last GPU move: the target it left and the one it went to (uids), writeSerial and cpuVersion
    // after it (any GPU write or CPU change since breaks the chain).
    std::vector<uint32_t> movedOutUid = std::vector<uint32_t>(kPageCount, 0u), movedToUid = std::vector<uint32_t>(kPageCount, 0u);
    std::vector<uint32_t> movedOutSerial = std::vector<uint32_t>(kPageCount, 0u), movedOutCpu = std::vector<uint32_t>(kPageCount, 0u);
    uint64_t statMoveBack = 0;
    // An acquire's Z -> CT16 moves (transferPage), run at its end in three
    // batches: the high planes into the page view, Z into its low 24 bits, the
    // words into the CT16 targets (per page the order matters; across pages
    // they are independent).
    struct ZToC16Move
    {
        Target *holder = nullptr; // a CT16 / Z32-layout colour target holding the high plane (nullptr: already in the view)
        Target *from = nullptr, *to = nullptr;
        int hx = 0, hy = 0, fx = 0, fy = 0, vx = 0, vy = 0, tx = 0, ty = 0;
    };
    std::vector<ZToC16Move> zToC16Moves;
    void flushZToC16()
    {
        if (zToC16Moves.empty())
            return;
        Target &v = pageViewTarget();
        const bool defer = deferMoves;
        deferMoves = true;
        for (const ZToC16Move &m : zToC16Moves)
            if (m.holder)
                convertPage(*m.holder, v, m.hx, m.hy, m.vx, m.vy, 0, m.holder->layout == Layout::C16 ? 4 : 2, false);
        flushMoves();
        for (const ZToC16Move &m : zToC16Moves)
            convertPage(*m.from, v, m.fx, m.fy, m.vx, m.vy, 2, 2, true);
        flushMoves();
        for (const ZToC16Move &m : zToC16Moves)
            convertPage(v, *m.to, m.vx, m.vy, m.tx, m.ty, 0, 3, false);
        flushMoves();
        deferMoves = defer;
        zToC16Moves.clear();
    }
    // Every GS page as a CT32 page (2048 x 512: page p at (p % 32, p / 32) x
    // (64, 32)): the 32-bit words of a page on their way from a Z buffer to a
    // 16-bit target. Never an owner; made on first use, dropped by rescale.
    std::unique_ptr<Target> pageView;
    Target &pageViewTarget()
    {
        if (!pageView)
        {
            pageView = std::make_unique<Target>();
            pageView->depth = false;
            pageView->layout = Layout::C32;
            pageView->fbp = 0;
            pageView->fbw = 32;
            pageView->width = 2048;
            pageView->height = pageView->logicalHeight = 512;
            pageView->pageHeight = 32;
            pageView->sx = scaleX;
            pageView->sy = scaleY;
            pageView->tex = makeTargetTexture(false, 2048 * scaleX, 512 * scaleY);
            if (scalesListed())
                targetTexs.insert(pageView->tex); // (a target-like texture at the picture's scale)
        }
        return *pageView;
    }
    // Bumped whenever a page's newest contents may change on the GPU; a
    // presentation peek copies a page back only when it changed since the last.
    std::array<uint32_t, kPageCount> writeSerial{};
    std::array<uint32_t, kPageCount> peekSerial{};
    uint64_t statGpuMoves = 0;
    uint64_t statFlushCount = 0;
    uint64_t statSplitBatches = 0, statSplitPrims = 0, statSplitLevels = 0;
    // A CLUT load from a GPU target is copied on the GPU (16x16 CSM1 block);
    // the CPU backend's CLUT buffer then lags until something needs it.
    bool gpuClutActive = false;
    GLuint gpuClutTex = 0;
    uint32_t gpuClutCpsm = 0;
    // A full CLUT load from a GPU target is read straight from that target
    // (gpuClutTex = its texture, the 16x16 square at clutOriginX/Y GS pixels)
    // until something is about to write that texture; then it is copied into
    // a CLUT ring texture as before (materializeDirectClut). GT3's colour
    // grading loads ~430 CLUTs a frame from one target: the copies were ~8%
    // of the Vulkan frame time at 4x progressive (each ended a render pass).
    // GT3_GS_GL_CLUT_DIRECT=0: copy at every load.
    bool clutDirect = false;
    int clutOriginX = 0, clutOriginY = 0;
    // Halfwords of the CPU copy that lag the GPU CLUT (0-255 low halves,
    // 256-511 high halves, as the CLUT buffer). A CPU-path load refreshes
    // the ones it writes; the rest are read back only when a draw needs one.
    std::bitset<512> clutStale;
    std::vector<GLuint> clutRing;
    size_t clutRingNext = 0;
    uint64_t statGpuCluts = 0;
    uint64_t statGpuCluts16 = 0; // of them CT16 (gpuClut16)
    // CLUT loads (GT3_GS_GL_STATS): calls, CPU-path loads, materialized GPU CLUTs, time.
    uint64_t statClutCalls = 0, statClutCpu = 0, statClutMaterialize = 0, statClutNs = 0, statClutCpuNs = 0, statClutRepeats = 0;
    // Per 16-halfword block of the CLUT buffer: the load that last wrote it
    // (a hash of its parameters and source page state; 0: unknown).
    std::array<uint64_t, 32> clutBlockWriter{};
    std::array<uint64_t, 32> clutBlockParams{}; // statistics: parameters only
    uint64_t statClutWhy[3] = {0, 0, 0};         // reloads: CPU source changed, GPU source changed, other parameters
    size_t primBufferBytes = 0;
    // Persistently mapped upload ring for the primitive and draw-state buffers
    // (GL 4.4 buffer storage): written in place and bound by range, no driver
    // copy. Four regions, each fenced after its last use before it is reused.
    GLuint uploadRing = 0;
    uint8_t *uploadRingPtr = nullptr;
    static constexpr size_t kRingRegion = 8u << 20u, kRingRegions = 4u;
    size_t ringOffset = 0; // write position in bytes, within region ringRegion
    size_t ringRegion = 0; // region being filled
    GLsync ringFence[kRingRegions] = {};
    GLint ssboAlign = 256;
    // The whole ring stays bound to both SSBO bindings (primitives and draw
    // states, 256-byte elements) and a batch passes its first element as
    // u_firstPrim/u_stateBase: two glBindBufferRange calls per batch were
    // about 2% of the GS thread. Needs the ring to fit one storage block.
    bool ringWhole = false;
    bool ringBound = false;
    void(GLAD_API_PTR *bufferStorage)(GLenum, GLsizeiptr, const void *, GLbitfield) = nullptr;

    // Space for `bytes` in the ring (aligned); nullptr if it does not fit a region.
    // The region is tracked explicitly: deriving it from the write position made
    // an allocation ending exactly on a boundary skip the fence/wait for the next
    // region, and past the last region it wrote beyond the mapped buffer
    // (access violation in flushBatch's memcpy, Simulation-mode Car Dealer).
    uint8_t *ringAlloc(size_t bytes, size_t &offset)
    {
        if (!uploadRingPtr || bytes > kRingRegion)
            return nullptr;
        const size_t align = std::max<size_t>(static_cast<size_t>(ssboAlign), sizeof(GpuPrim)); // powers of two
        size_t at = (ringOffset + align - 1u) & ~(align - 1u);
        if (at + bytes > (ringRegion + 1u) * kRingRegion)
        {
            // Into the next region: fence the one just finished, wait for the next.
            const size_t done = ringRegion, next = (ringRegion + 1u) % kRingRegions;
            ringRegion = next;
            if (ringFence[done])
                glDeleteSync(ringFence[done]);
            ringFence[done] = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
            if (ringFence[next])
            {
                glClientWaitSync(ringFence[next], GL_SYNC_FLUSH_COMMANDS_BIT, 1000000000ull);
                glDeleteSync(ringFence[next]);
                ringFence[next] = nullptr;
            }
            at = next * kRingRegion;
        }
        ringOffset = at + bytes;
        offset = at;
        return uploadRingPtr + at;
    }
    std::unordered_map<std::string, GLint> uniforms;

    // Pending draw
    bool haveBatch = false;
    // A Z16 batch (key.zte 2) with a vertex Z above 0xFFFF: drawn with the exact program in fast mode (the Z clamp
    // per pixel). Without one, interpolated Z stays within 0xFFFF (a convex combination of the vertices) and the
    // batch keeps fast mode's rasterizing program, as Z24 batches do (PS2X_GS_GL_Z16_FAST=0: always exact).
    bool batchZ16Above = false;
    DrawKey key{};
    std::vector<GpuPrim> prims;

    std::unordered_map<uint64_t, TexEntry> textures;
    uint64_t frameCounter = 0;
    uint32_t clutVersion = 1;

    // Transfer in progress (host -> local): its destination pages.
    std::vector<uint32_t> transferDestPages;
    // Submit's texture footprint pages (reused: a new vector per textured
    // primitive was about 6% of the GS thread in heap calls).
    std::vector<uint32_t> footprintScratch;
    // findTarget's last results per colour (0) / depth (1), most recent first: a primitive looks up its colour
    // target and the target its texture lies in (and depth), so one entry each missed every other call. An entry
    // is used only while its slot holds a target of that key, and the index names one target per key, so the
    // answer is the index's.
    static constexpr int kFoundRecent = 4;
    int lastFound[2][kFoundRecent] = {{-1, -1, -1, -1}, {-1, -1, -1, -1}};
    uint64_t lastFoundKey[2][kFoundRecent] = {{~0ull, ~0ull, ~0ull, ~0ull}, {~0ull, ~0ull, ~0ull, ~0ull}};

    // Statistics (GT3_GS_GL_STATS=1)
    bool stats = envFlag("GT3_GS_GL_STATS", false);
    uint64_t statGpuPrims = 0, statCpuPrims = 0, statDraws = 0, statGroups = 0, statDownloads = 0, statUploads = 0, statTexDecodes = 0;
    // Time spent (ns): 0 page downloads, 1 page uploads, 2 texture decodes, 3 GL batch flushes, 4 whole Submit,
    // within flushes: 5 primitive buffer upload, 6 GL state and uniforms, 7 overlap grouping, 8 draw calls and barriers.
    uint64_t statNs[9] = {0, 0, 0, 0, 0, 0, 0, 0, 0};
    // The statistics' clock (GT3_GS_GL_STATS). steady_clock is
    // QueryPerformanceCounter, and two reads per Submit plus one in
    // reportStats were ~65 of the GL thread's ~480 profiler samples per 10 s
    // in the colour select: the stats alone pushed that thread to 97% of a
    // core and made the scene drift (analysis/phase5/specs/step6-colour-swap.md
    // 4.1). On x86 it reads the time-stamp counter instead (invariant on the
    // CPUs this runs on), scaled to ns with a rate measured once against
    // steady_clock (a 5 ms spin at the first timer), and reportStats looks
    // at the time only every 0.1 s. PS2X_GS_GL_STATS_TSC=0: steady_clock and
    // the per-call check as before. GT4 only by default: GT3 builds keep
    // steady_clock unless PS2X_GS_GL_STATS_TSC=1.
    struct StatClock
    {
        static bool tsc()
        {
#if defined(PS2X_GS_STAT_TSC)
            static const bool on = [] {
                const char *v = std::getenv("PS2X_GS_GL_STATS_TSC");
#if defined(PS2X_GAME_GT3)
                return v && *v && *v != '0';
#else
                return !(v && *v == '0');
#endif
            }();
            return on;
#else
            return false;
#endif
        }
        static uint64_t steadyNs()
        {
            return static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
        }
        // Ticks: TSC cycles, or steady_clock ns.
        static uint64_t now()
        {
#if defined(PS2X_GS_STAT_TSC)
            if (tsc())
                return __rdtsc();
#endif
            return steadyNs();
        }
        static double nsPerTick()
        {
            static const double rate = [] {
                if (!tsc())
                    return 1.0;
                const uint64_t ns0 = steadyNs(), t0 = now();
                uint64_t ns1 = ns0, t1 = t0;
                while (ns1 - ns0 < 5000000u)
                {
                    ns1 = steadyNs();
                    t1 = now();
                }
                return t1 > t0 ? static_cast<double>(ns1 - ns0) / static_cast<double>(t1 - t0) : 1.0;
            }();
            return rate;
        }
        static uint64_t toNs(uint64_t ticks) { return tsc() ? static_cast<uint64_t>(static_cast<double>(ticks) * nsPerTick()) : ticks; }
        static uint64_t fromNs(uint64_t ns) { return tsc() ? static_cast<uint64_t>(static_cast<double>(ns) / nsPerTick()) : ns; }
    };
    // Timers for the statistics only (GT3_GS_GL_STATS): QueryPerformanceCounter
    // on every flush and texture is not free.
    struct ScopedNs
    {
        static bool &enabled()
        {
            static bool on = envFlag("GT3_GS_GL_STATS", false);
            return on;
        }
        uint64_t &acc;
        uint64_t start = 0;
        explicit ScopedNs(uint64_t &a) : acc(a)
        {
            if (enabled())
            {
                (void)StatClock::nsPerTick(); // (calibrated before the first reading)
                start = StatClock::now();
            }
        }
        ~ScopedNs()
        {
            if (enabled())
                acc += StatClock::toNs(StatClock::now() - start);
        }
    };
    uint64_t statNextCheck = 0; // reportStats: the next StatClock tick to look at the time
    // Step 7 T0 (analysis/design/ny-throughput/STEP7-THROUGHPUT.md): cheaper
    // statistics. Submit's own timer reads the clock for one call in
    // statSample, chosen pseudo-randomly (a periodic draw pattern cannot bias
    // it), and counts that call's time statSample times: an unbiased estimate
    // of "Submit total" without two clock reads per primitive; reportStats
    // looks at the clock once per 1,024 Submit calls (and at every
    // presentation). PS2X_GS_GL_STATS_SAMPLE=<n>: GT4 default 64; 1 times every
    // call and checks the clock every call (as before; the GT3 default).
    uint32_t statSample = [] {
        const char *v = std::getenv("PS2X_GS_GL_STATS_SAMPLE");
#if defined(PS2X_GAME_GT3)
        const long fallback = 1;
#else
        const long fallback = 64;
#endif
        const long n = (v && *v) ? std::strtol(v, nullptr, 10) : fallback;
        return static_cast<uint32_t>(std::clamp<long>(n, 1, 1L << 20));
    }();
    uint32_t statSampleRng = 0x9E3779B9u;
    uint32_t statReportCalls = 0;
    bool sampleThisCall()
    {
        if (statSample <= 1u)
            return true;
        uint32_t x = statSampleRng; // xorshift32
        x ^= x << 13;
        x ^= x >> 17;
        x ^= x << 5;
        statSampleRng = x;
        return x % statSample == 0u;
    }
    // A ScopedNs for one call in statSample (Submit), weighted by statSample.
    struct SampledNs
    {
        uint64_t &acc;
        uint64_t start = 0;
        uint32_t weight = 0;
        SampledNs(uint64_t &a, Impl &im) : acc(a)
        {
            if (ScopedNs::enabled() && im.sampleThisCall())
            {
                weight = im.statSample;
                (void)StatClock::nsPerTick();
                start = StatClock::now();
            }
        }
        ~SampledNs()
        {
            if (weight)
                acc += StatClock::toNs(StatClock::now() - start) * weight;
        }
    };
    // Step 7 T0: batch breaks counted by the first DrawKey word that changed
    // (formatted as "key <name>" only in reportStats, not per batch).
    uint64_t statBreakWord[sizeof(DrawKey) / 4u + 1u] = {};
    // Step 7 T0: flushBatch calls by their call site (GT3_GS_GL_STATS).
    struct FlushSite
    {
        uint64_t count = 0;
        const char *function = "";
    };
    std::unordered_map<uint32_t, FlushSite> statFlushSites; // by source line
    // Step 7 T0: runs of one draw state. Primitives (Submit calls past the
    // point/line split), those whose GSDrawState equals the previous one's,
    // those reaching the batch step and of them those whose resolved DrawKey
    // equals the previous one's; GPU draw states per batch.
    GSDrawState statPrevState{};
    bool statPrevStateValid = false;
    DrawKey statPrevKey{};
    bool statPrevKeyValid = false;
    uint64_t statRunPrims = 0, statRunSameState = 0, statRunKeyed = 0, statRunSameKey = 0, statBatchStates = 0, statStateBatches = 0;
    // Step 7 T0: batch breaks between a plain, interlocked or split batch
    // (x Z test x Z mask) where dstRead changed: [previous kind][next kind].
    uint64_t statDstPairs[12][12] = {};
    int batchKind(const DrawKey &k) const
    {
        const bool twoPass = k.ate && k.atst != 1 && k.afail != 0;
        const int mode = !k.needsSplit ? 0 : ((useInterlock() && k.dstRead && !twoPass && !(k.zte && !k.zmask)) ? 1 : 2);
        return mode * 4 + (k.zte ? 2 : 0) + (k.zmask ? 1 : 0);
    }
    // Step 7 T0: CLUT loads done (not skipped as repeats) by size, those whose
    // source has no GPU-held page, and those whose blocks were all loaded
    // before and not read by any draw since (the earlier load was wasted).
    uint64_t statClut16 = 0, statClut256 = 0, statClutCpuSource = 0, statClutUnread = 0;
    uint32_t clutUnreadBlocks = 0; // CLUT buffer blocks loaded since a draw last read them
    // The 16-halfword blocks of the CLUT buffer a lookup with this TEX0 reads
    // (the blocks a load with it writes, as LoadClut).
    static uint32_t clutBlocksOf(const GSTex0Reg &tex)
    {
        const bool fourBit = tex.psm == GS_PSM_T4 || tex.psm == GS_PSM_T4HL || tex.psm == GS_PSM_T4HH;
        const bool eightBit = tex.psm == GS_PSM_T8 || tex.psm == GS_PSM_T8H;
        const uint32_t count = fourBit ? 16u : (eightBit ? 256u : 0u);
        const bool clut16 = tex.cpsm == GS_PSM_CT16 || tex.cpsm == GS_PSM_CT16S;
        uint32_t blocks = 0;
        for (uint32_t j = 0; j < count / 16u; ++j)
        {
            if (clut16)
                blocks |= 1u << ((tex.csa + j) & 31u);
            else
            {
                const uint32_t b = ((tex.csa & 15u) + j) & 15u;
                blocks |= (1u << b) | (1u << (b + 16u));
            }
        }
        return blocks;
    }
    void noteClutRead(const GSTex0Reg &tex)
    {
        if (stats)
            clutUnreadBlocks &= ~clutBlocksOf(tex);
    }
    // Step 7 T0: interlocked batches' and split batches' pixel states counted
    // by their fields, formatted (as before) only in reportStats.
    using StatKey = std::array<int32_t, 24>;
    std::map<StatKey, uint64_t> statInterlockKeys, statSplitGroupKeys, statSplitPrimKeys;
    StatKey groupKey{}; // the current split batch's group kind
    static int layoutCode(const Target &t)
    {
        return t.depth ? (t.layout == Layout::Z16 ? 4 : 0) : (t.layout == Layout::C16 ? 1 : (t.layout == Layout::Z32 ? 2 : 3));
    }
    static const char *layoutCodeName(int code)
    {
        static const char *names[8] = {"Z", "C16", "Z32c", "C", "Z16", "?", "?", "?"};
        return names[code & 7];
    }

    std::unordered_map<std::string, uint64_t> statFallbackReasons;
    std::unordered_map<std::string, uint64_t> statDownloadCauses;
    std::unordered_map<std::string, uint64_t> statUploadCauses; // CPU copy -> target page uploads by (target, cause)
    std::unordered_map<std::string, uint64_t> statMoveCauses; // GPU page moves by (from, to, cause)
    std::unordered_map<std::string, std::pair<uint64_t, uint64_t>> statViewMoves; // texture views' page moves (of them back) by read
    std::unordered_map<std::string, uint64_t> statSplitGroups; // split batches' groups by blend/alpha mode
    std::map<std::string, uint64_t> statSplitPrimKinds; // split batches' primitives by depth writes / two-pass alpha
    std::map<std::string, uint64_t> statInterlockKinds; // interlocked batches' primitives by pixel state
    // GS_HW_TRI batches' primitives by pixel state, and how many batches share
    // one such state (whether per-batch shader specialisation would apply).
    std::map<std::string, uint64_t> statHwTriKinds;
    uint64_t statHwTriBatches = 0, statHwTriUniform = 0, statHwTriPrims = 0, statHwTriUniformPrims = 0;
    std::unordered_map<std::string, uint64_t> statBatchBreaks;  // why batches ended
    // Why pages are being copied back (statistics). A CPU-path CLUT load's
    // cause names its parameters: formatted only when something reads it (a
    // download or upload during the load), not at every load (step 7 T0).
    struct Cause
    {
        struct Clut
        {
            uint32_t cbp = 0, cpsm = 0, psm = 0, csm = 0, csa = 0, fbp = 0;
            const char *from = "cpu";
        };
        mutable std::string text = "other";
        mutable bool lazy = false;
        Clut clut{};
        Cause &operator=(const char *s)
        {
            lazy = false;
            text = s;
            return *this;
        }
        Cause &operator=(const std::string &s)
        {
            lazy = false;
            text = s;
            return *this;
        }
        void setClut(const Clut &c)
        {
            clut = c;
            lazy = true;
        }
        const std::string &str() const
        {
            if (lazy)
            {
                char t[96];
                std::snprintf(t, sizeof(t), "CLUT load cbp %04x cpsm %02x psm %02x csm %u csa %u from %s %03x", clut.cbp, clut.cpsm, clut.psm,
                              clut.csm, clut.csa, clut.from, clut.fbp);
                text = t;
                lazy = false;
            }
            return text;
        }
        operator const std::string &() const { return str(); }
        const char *c_str() const { return str().c_str(); }
    } cause;
    std::chrono::steady_clock::time_point statStart = std::chrono::steady_clock::now();

    std::vector<uint8_t> pixelScratch;
    // Pixels covered by the current group, per 8x8 cell (a cell's mask counts
    // only while its stamp is the group's).
    // Draw levels of a batch that must keep overlapping primitives in order:
    // per 8x8 cell, up to four (level, pixel mask) pairs of the primitives seen
    // so far (merged upwards when full: a pixel may then report a higher level
    // than it has, never a lower one).
    struct CellLevels
    {
        uint32_t stamp = 0;
        uint8_t count = 0;
        uint16_t level[4] = {0, 0, 0, 0};
        uint64_t mask[4] = {0, 0, 0, 0};
    };
    std::vector<CellLevels> cellLevels = std::vector<CellLevels>(256u * 256u);
    uint32_t levelStamp = 1u;
    std::vector<uint64_t> primMasks;
    std::vector<uint16_t> primLevels;
    std::vector<GpuPrim> levelOrder;
    std::vector<size_t> levelStart;
    // glTextureBarrier (GL 4.5 / ARB/NV_texture_barrier): blends read the
    // target itself between barriers instead of a copy of it.
    void(GLAD_API_PTR *textureBarrier)() = nullptr;
    bool hasBindless = false;
    bool conservativeRaster = false; // GL_NV_conservative_raster: triangles drawn as triangles

    // GT3_GS_GL_GPU_PROFILE=1 (with GT3_GS_GL_STATS=1): each batch's GPU time
    // and fragment shader invocations, summed by kind of batch; the queries
    // are read when their results are ready (no wait).
    struct GpuQuery
    {
        GLuint time = 0, frags = 0;
        std::string key;
    };
    bool gpuProfiling = false, pipelineStats = false;
    int batchTraceReports = 0, batchTraceLeft = 0;
    std::deque<GpuQuery> gpuQueries;
    std::vector<GLuint> freeQueries;
    struct GpuCost
    {
        uint64_t ns = 0, frags = 0, batches = 0, prims = 0;
    };
    std::unordered_map<std::string, GpuCost> gpuProfile;
    GLuint takeQuery()
    {
        if (freeQueries.empty())
        {
            GLuint q = 0;
            glGenQueries(1, &q);
            return q;
        }
        const GLuint q = freeQueries.back();
        freeQueries.pop_back();
        return q;
    }
    void pollGpuQueries()
    {
        while (!gpuQueries.empty())
        {
            GpuQuery &g = gpuQueries.front();
            GLint ready = 0;
            glGetQueryObjectiv(g.time, GL_QUERY_RESULT_AVAILABLE, &ready);
            if (!ready)
                break;
            GLuint64 ns = 0, frags = 0;
            glGetQueryObjectui64v(g.time, GL_QUERY_RESULT, &ns);
            if (g.frags)
                glGetQueryObjectui64v(g.frags, GL_QUERY_RESULT, &frags);
            GpuCost &c = gpuProfile[g.key];
            c.ns += ns;
            c.frags += frags;
            freeQueries.push_back(g.time);
            if (g.frags)
                freeQueries.push_back(g.frags);
            gpuQueries.pop_front();
        }
    }
    // GPU presentation: the display buffer's current field (and the one
    // before, for weaving interlaced fields) and the compose program. The
    // fields are a ring of fieldRing entries: 2 (current and previous), or 4
    // for the motion-adaptive mode (GT4_DEINTERLACE=adaptive), which also
    // compares each with the field two before it (fieldTex[2] / [3] are made
    // when that mode is first used).
    GLuint presentProgram = 0, presentFbo = 0;
    GLuint fieldTex[4] = {0, 0, 0, 0};
    uint64_t fieldTick[4] = {~0ull, ~0ull, ~0ull, ~0ull};
    uint64_t fieldSeq[4] = {0, 0, 0, 0};
    int fieldParity[4] = {0, 0, 0, 0};
    bool fieldPhased[4] = {false, false, false, false};
    uint32_t fieldW[4] = {0, 0, 0, 0}, fieldH[4] = {0, 0, 0, 0};
    int fieldCur = 0;
    int fieldRing = 2;
    std::unordered_map<std::string, GLint> presentUniforms;
    GLint presentUniform(const char *name)
    {
        auto it = presentUniforms.find(name);
        if (it != presentUniforms.end())
            return it->second;
        const GLint loc = glGetUniformLocation(presentProgram, name);
        presentUniforms.emplace(name, loc);
        return loc;
    }
    // GL_ARB_bindless_texture entry points and the handles made resident.
    uint64_t(GLAD_API_PTR *getTextureHandle)(GLuint) = nullptr;
    void(GLAD_API_PTR *makeHandleResident)(uint64_t) = nullptr;
    void(GLAD_API_PTR *makeHandleNonResident)(uint64_t) = nullptr;
    std::unordered_map<GLuint, uint64_t> handles;
    // handles / linearHandles by texture name (vkgl and GL names are small): handleOf and linearHandleOf run
    // for every primitive. Kept equal to the maps (releaseTexture); names past the table use the maps alone.
    struct FlatHandles
    {
        static constexpr GLuint kMax = 1u << 16;
        std::vector<uint64_t> value;
        std::vector<uint8_t> known;
        const uint64_t *find(GLuint name) const { return (name < known.size() && known[name]) ? &value[name] : nullptr; }
        void set(GLuint name, uint64_t h)
        {
            if (name >= kMax)
                return;
            if (name >= known.size())
            {
                value.resize(name + 1u, 0u);
                known.resize(name + 1u, 0u);
            }
            value[name] = h;
            known[name] = 1u;
        }
        void erase(GLuint name)
        {
            if (name < known.size())
                known[name] = 0u;
        }
    } flatHandles, flatLinearHandles;
    // Fast mode (from 16 samples per pixel, as the fast textures): target
    // reads (texModes 1-3, bilinear) sampled by the GPU's own filter, through
    // a second bindless handle of the target with a linear sampler, when the
    // texture's pages lie in the target as one rectangle (texOffX/Y).
    // GT3_GS_GL_FAST_TARGETS=1/0 forces on/off at any fast-mode scale.
    bool fastTargetsActive = false;
    GLuint linearSampler = 0;
    uint64_t(GLAD_API_PTR *getTextureSamplerHandle)(GLuint, GLuint) = nullptr;
    std::unordered_map<GLuint, uint64_t> linearHandles;
    uint64_t linearHandleOf(GLuint tex)
    {
        if (!tex)
            return 0;
        if (const uint64_t *known = flatLinearHandles.find(tex))
            return *known;
        auto it = linearHandles.find(tex);
        if (it != linearHandles.end())
            return it->second;
        const uint64_t h = getTextureSamplerHandle(tex, linearSampler);
        makeHandleResident(h);
        linearHandles.emplace(tex, h);
        flatLinearHandles.set(tex, h);
        return h;
    }
    // States of the pending batch (deduplicated against the previous one) and
    // the textures its draws sample (which must not change before it is drawn).
    std::vector<GpuState> states;
    GpuState lastState{};
    int32_t lastStateIndex = -1;
    GLuint stateBuffer = 0;
    size_t stateBufferBytes = 0;
    std::vector<GLuint> batchTextures;

    uint64_t handleOf(GLuint tex)
    {
        if (!tex)
            return 0;
        if (const uint64_t *known = flatHandles.find(tex))
            return *known;
        auto it = handles.find(tex);
        if (it != handles.end())
            return it->second;
        const uint64_t h = getTextureHandle(tex);
        makeHandleResident(h);
        handles.emplace(tex, h);
        flatHandles.set(tex, h);
        return h;
    }
    // ---------------------------------------------- stable GL targets (P2)
    // PS2X_GS_GL_STABLE_TARGETS (analysis/phase5/specs/step4-hitches.md P2;
    // GT4 default 1, GT3 default 0): the race view and B-Spec hitches sat in
    // the driver (GPU memory allocation and mapping) under the colour/depth
    // attachments of the one shared draw framebuffer and page-move framebuffer,
    // while ~100-300 targets a 10 s window were made, dropped and made again.
    // 1: target textures that a dropped (or grown) target releases go to a pool
    //    by (format, size) and are reused (cleared to 0, as a new texture) before
    //    a new one is made, with their bindless handles kept (at most 256 MB
    //    unused at native, 512 MB at any scale; the oldest go first); every target
    //    texture gets framebuffers of its own, attached once (one per colour/
    //    depth pair for draws, one per texture for page moves, uploads' patches
    //    and read backs). 2: also, at native resolution, a target is made at
    //    its full height at once (it never grows; up to ~4 MB each): opt-in, as
    //    one race6 run (p5s5-race6-on) had a 10-VSync loading frame and a
    //    4-VSync race-view frame with it (the ~250 race-view targets allocated
    //    at full size), against 4-5 and 2 without. 0: as before. Output unchanged
    //    by construction (which framebuffer a texture is attached to, and which
    //    texture name holds a target, are not visible in the pixels).
#if defined(PS2X_GAME_GT3)
    int stableTargets = [] {
        const char *v = std::getenv("PS2X_GS_GL_STABLE_TARGETS");
        return (v && *v) ? std::max(0, std::atoi(v)) : 0;
    }();
#else
    int stableTargets = [] {
        const char *v = std::getenv("PS2X_GS_GL_STABLE_TARGETS");
        return (v && *v) ? std::max(0, std::atoi(v)) : 1;
    }();
#endif
    struct PooledTexture
    {
        GLuint tex = 0;
        uint64_t key = 0;
        size_t bytes = 0;
        bool spare = false; // made in idle time (PS2X_GS_GL_PREWARM), not used yet
    };
    std::deque<PooledTexture> targetPool; // oldest first
    size_t targetPoolBytes = 0;
    // ------------------------------------------- spare targets (prewarm, F2)
    // PS2X_GS_GL_PREWARM=<n> (analysis/phase5/specs/step6-loading-frame.md F2;
    // opt-in: default 0 in GT4 and GT3; needs stable targets): the loading
    // frame's readback chain draws 6 cars' images, each through new small
    // targets, and the driver's first-use work for each new texture and its
    // framebuffer (~25-30 ms over the chain) sat inside the chain on the GL
    // thread. Spares of small target
    // sizes (at most 256 KB at native) are made ahead: the first pool miss of a
    // size asks for one spare, a later miss of the same size for n, and each
    // spare used for one more (so a run of same-size targets, as the chain's
    // cars, keeps one ready, and a size seen once costs one spare). The GL
    // thread makes them one per idle call (GSAsyncBackend: the command ring
    // empty for its spin window), each with its page-move framebuffer attached
    // and its storage cleared (so the driver allocates it then), and puts them
    // in the pool, where the next targets of that size find them (cleared on
    // reuse like any pooled texture). The size asked for last is served first;
    // at most 4 MB of unused spares (at native) at a time, the oldest unused
    // spares of other sizes deleted to make room (a size that missed long ago
    // is the poorest guess). Output unchanged by construction: only when GL
    // objects are made changes. Measured (step 6, race6, display on): no gain.
    // Each car of the chain makes ~17 new targets of ~8 sizes, and the GL
    // thread is idle long enough for about one spare between cars (loading
    // frame 4 VSyncs with and without, one run 3 and one 5; PS2X_GS_GL_PREWARM_LOG).
    int prewarm = [] {
        const char *v = std::getenv("PS2X_GS_GL_PREWARM");
        return (v && *v) ? std::max(0, std::atoi(v)) : 0;
    }();
    std::vector<std::pair<uint64_t, int>> prewarmWanted; // (pool key, spares still to make), the last asked last
    std::unordered_map<uint64_t, uint32_t> prewarmMisses; // pool misses per small size
    void wantSpares(uint64_t k, int n)
    {
        int have = 0;
        for (auto it = prewarmWanted.begin(); it != prewarmWanted.end(); ++it)
            if (it->first == k)
            {
                have = it->second;
                prewarmWanted.erase(it);
                break;
            }
        prewarmWanted.emplace_back(k, std::max(have, n));
    }
    size_t spareBytes = 0;                           // unused spares in the pool
    uint64_t statSparesMade = 0, statSparesUsed = 0, statSparesDropped = 0;
    size_t spareMaxBytes() const { return static_cast<size_t>(scaleX * scaleY) * (size_t{256} << 10); }
    // PS2X_GS_GL_PREWARM_LOG=1 (diagnostics): one line per pool miss, spare made and spare used.
    static bool prewarmLog()
    {
        static const bool on = [] {
            const char *v = std::getenv("PS2X_GS_GL_PREWARM_LOG");
            return v && *v == '1';
        }();
        return on;
    }
    static void notePrewarm(const char *what, uint64_t k)
    {
        uint64_t tick = 0;
        ps2x::vblank::Clock::time_point due{}, next{};
        ps2x::vblank::grid(tick, due, next);
        std::fprintf(stderr, "[gs-prewarm] vsync %llu %s %s %ux%u\n", static_cast<unsigned long long>(tick), what,
                     (k >> 63) ? "depth" : "colour", static_cast<unsigned>((k >> 32) & 0x7FFFFFFFu), static_cast<unsigned>(k & 0xFFFFFFFFu));
    }
    size_t spareBudgetBytes() const { return static_cast<size_t>(scaleX * scaleY) * (size_t{4} << 20); }
    // A pooled texture leaves the pool (reused, or deleted).
    void unpool(const PooledTexture &p, bool reused)
    {
        targetPoolBytes -= p.bytes;
        if (p.spare)
        {
            spareBytes -= p.bytes;
            ++(reused ? statSparesUsed : statSparesDropped);
        }
    }
    std::unordered_map<uint64_t, GLuint> drawFbos; // colour texture << 32 | depth texture -> framebuffer
    std::unordered_map<GLuint, GLuint> soloFbos;   // texture -> framebuffer with only it attached
    GLuint curDrawFbo = 0xFFFFFFFFu;                // the draw framebuffer bound by flushBatch (stableTargets)
    uint64_t statPoolHits = 0, statPoolMisses = 0, statFbosMade = 0;
    void(GLAD_API_PTR *clearTexImage)(GLuint, GLint, GLenum, GLenum, const void *) = nullptr;
    static uint64_t poolKey(bool depth, int w, int h)
    {
        return (depth ? (1ull << 63) : 0ull) | (static_cast<uint64_t>(static_cast<uint32_t>(w)) << 32) | static_cast<uint32_t>(h);
    }
    // A target texture of w x h samples: a pooled one (cleared) or a new one.
    GLuint acquireTargetTexture(bool depth, int w, int h)
    {
        if (stableTargets)
        {
            const uint64_t k = poolKey(depth, w, h);
            for (auto it = targetPool.rbegin(); it != targetPool.rend(); ++it)
                if (it->key == k)
                {
                    const GLuint tex = it->tex;
                    if (it->spare)
                    {
                        if (prewarmLog())
                            notePrewarm("spare used", k);
                        wantSpares(k, 1); // the next one of this size
                    }
                    unpool(*it, true);
                    targetPool.erase(std::next(it).base());
                    ++statPoolHits;
                    if (clearTexImage)
                        clearTexImage(tex, 0, depth ? GL_DEPTH_COMPONENT : GL_RGBA, depth ? GL_FLOAT : GL_UNSIGNED_BYTE, nullptr);
                    return tex;
                }
            ++statPoolMisses;
            if (prewarmLog())
                notePrewarm("pool miss", k);
            // More of this size are likely to follow: spares in idle time.
            if (prewarm > 0 && static_cast<size_t>(w) * static_cast<size_t>(h) * 4u <= spareMaxBytes())
                wantSpares(k, prewarmMisses[k]++ == 0u ? 1 : prewarm);
        }
        return makeTargetTexture(depth, w, h);
    }
    // A target's texture it no longer uses (w x h samples): to the pool (its
    // framebuffers and bindless handles stay), else deleted.
    void recycleTargetTexture(GLuint &tex, bool depth, int w, int h)
    {
        if (!tex)
            return;
        touchResolve(); // (step 7 T1)
        if (!stableTargets)
        {
            releaseTexture(tex);
            return;
        }
        if (clutDirect && tex == gpuClutTex)
            materializeDirectClut(); // (a direct CLUT's target dropped)
        selfSnap.valid = false;
        forgetBindings();
        const size_t bytes = static_cast<size_t>(w) * static_cast<size_t>(h) * 4u;
        targetPool.push_back({tex, poolKey(depth, w, h), bytes});
        targetPoolBytes += bytes;
        tex = 0;
        trimTargetPool();
    }
    // At most 256 MB of unused targets at native, 512 MB at any scale (the
    // oldest go first). Pooled textures keep their bindless handles
    // resident, so a cap per sample (16 GB at 8x) could exceed the VRAM.
    void trimTargetPool()
    {
        const size_t cap = std::min(static_cast<size_t>(scaleX * scaleY) * (size_t{256} << 20), size_t{512} << 20);
        while (targetPoolBytes > cap && !targetPool.empty())
        {
            PooledTexture old = targetPool.front();
            targetPool.pop_front();
            unpool(old, false);
            releaseTexture(old.tex);
        }
    }
    void clearTargetPool()
    {
        while (!targetPool.empty())
        {
            PooledTexture old = targetPool.front();
            targetPool.pop_front();
            if (old.spare)
                ++statSparesDropped;
            releaseTexture(old.tex);
        }
        targetPoolBytes = 0;
        spareBytes = 0;
        prewarmWanted.clear();
        prewarmMisses.clear();
    }
    // GL thread idle time (IdleWork): one spare target texture of a size a
    // pool miss asked for, with its page-move framebuffer attached and its
    // storage cleared, into the pool. True while more are wanted.
    bool makeSpareTarget()
    {
        while (!prewarmWanted.empty())
        {
            const uint64_t k = prewarmWanted.back().first;
            if (--prewarmWanted.back().second <= 0)
                prewarmWanted.pop_back();
            const bool depth = (k >> 63) != 0u;
            const int w = static_cast<int>((k >> 32) & 0x7FFFFFFFu), h = static_cast<int>(k & 0xFFFFFFFFu);
            const size_t bytes = static_cast<size_t>(w) * static_cast<size_t>(h) * 4u;
            if (!stableTargets || bytes > spareBudgetBytes())
                continue;
            // Room: the oldest unused spares of other sizes go.
            for (auto it = targetPool.begin(); spareBytes + bytes > spareBudgetBytes() && it != targetPool.end();)
            {
                if (!it->spare || it->key == k)
                {
                    ++it;
                    continue;
                }
                const PooledTexture old = *it;
                it = targetPool.erase(it);
                unpool(old, false);
                GLuint dropped = old.tex;
                releaseTexture(dropped);
            }
            if (spareBytes + bytes > spareBudgetBytes())
                continue; // (the budget holds only spares of this size: enough of them)
            const GLuint tex = makeTargetTexture(depth, w, h);
            soloFboFor(tex, depth); // made and attached now (it restores the framebuffer bindings)
            if (clearTexImage)
                clearTexImage(tex, 0, depth ? GL_DEPTH_COMPONENT : GL_RGBA, depth ? GL_FLOAT : GL_UNSIGNED_BYTE, nullptr);
            targetPool.push_back({tex, k, bytes, true});
            targetPoolBytes += bytes;
            spareBytes += bytes;
            ++statSparesMade;
            if (prewarmLog())
                notePrewarm("spare made", k);
            trimTargetPool();
            return !prewarmWanted.empty();
        }
        return false;
    }
    // The draw framebuffer of a colour target (and depth target, 0: none),
    // made and attached on first use.
    GLuint drawFboFor(GLuint color, GLuint depth)
    {
        const uint64_t k = (static_cast<uint64_t>(color) << 32) | depth;
        if (auto it = drawFbos.find(k); it != drawFbos.end())
            return it->second;
        GLuint f = 0;
        glGenFramebuffers(1, &f);
        glBindFramebuffer(GL_FRAMEBUFFER, f);
        noteAttach(color);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, color, 0);
        if (depth)
        {
            noteAttach(depth);
            glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, depth, 0);
        }
        glDrawBuffer(GL_COLOR_ATTACHMENT0);
        drawFbos.emplace(k, f);
        ++statFbosMade;
        return f;
    }
    // The framebuffer with only texture tex attached (colour, or depth), made
    // on first use (draw and read buffer set for it). Making one leaves the
    // read and draw framebuffer bindings as they were: a readback bound only
    // the read framebuffer, and a draw framebuffer changed behind flushBatch's
    // back (curDrawFbo, onInterlockFbo) took the next interlocked batch out of
    // interlockFbo; a blit's source binding was lost the same way.
    GLuint soloFboFor(GLuint tex, bool depth)
    {
        if (auto it = soloFbos.find(tex); it != soloFbos.end())
            return it->second;
        GLint drawWas = 0, readWas = 0;
        glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &drawWas);
        glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &readWas);
        GLuint f = 0;
        glGenFramebuffers(1, &f);
        glBindFramebuffer(GL_FRAMEBUFFER, f);
        noteAttach(tex, false);
        glFramebufferTexture2D(GL_FRAMEBUFFER, depth ? GL_DEPTH_ATTACHMENT : GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
        glDrawBuffer(depth ? GL_NONE : GL_COLOR_ATTACHMENT0);
        glReadBuffer(depth ? GL_NONE : GL_COLOR_ATTACHMENT0);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, static_cast<GLuint>(drawWas));
        glBindFramebuffer(GL_READ_FRAMEBUFFER, static_cast<GLuint>(readWas));
        soloFbos.emplace(tex, f);
        ++statFbosMade;
        return f;
    }
    // That framebuffer, bound at target (only there).
    void bindSoloFbo(GLenum target, GLuint tex, bool depth)
    {
        glBindFramebuffer(target, soloFboFor(tex, depth));
        if (target != GL_READ_FRAMEBUFFER)
        {
            curDrawFbo = 0xFFFFFFFFu;
            onInterlockFbo = false;
        }
    }
    // A texture is deleted: so are the framebuffers it is attached to.
    void dropFbosOf(GLuint tex)
    {
        if (auto it = soloFbos.find(tex); it != soloFbos.end())
        {
            glDeleteFramebuffers(1, &it->second);
            soloFbos.erase(it);
        }
        for (auto it = drawFbos.begin(); it != drawFbos.end();)
        {
            if (static_cast<GLuint>(it->first >> 32) == tex || static_cast<GLuint>(it->first & 0xFFFFFFFFu) == tex)
            {
                glDeleteFramebuffers(1, &it->second);
                it = drawFbos.erase(it);
            }
            else
                ++it;
        }
        curDrawFbo = 0xFFFFFFFFu;
    }

    // Deletes a texture (dropping its bindless handle and cached bindings).
    void releaseTexture(GLuint &tex)
    {
        if (!tex)
            return;
        touchResolve(); // (step 7 T1: a memo may name it)
        if (clutDirect && tex == gpuClutTex)
            materializeDirectClut(); // (a direct CLUT's target dropped)
        if (!soloFbos.empty() || !drawFbos.empty())
            dropFbosOf(tex);
        auto it = handles.find(tex);
        if (it != handles.end())
        {
            makeHandleNonResident(it->second);
            handles.erase(it);
        }
        flatHandles.erase(tex);
        if (auto lit = linearHandles.find(tex); lit != linearHandles.end())
        {
            makeHandleNonResident(lit->second);
            linearHandles.erase(lit);
        }
        flatLinearHandles.erase(tex);
        forgetBindings();
        if (stats)
        {
            ++statTexDeleted;
            attachedOnce.erase(tex);
        }
        glDeleteTextures(1, &tex);
        tex = 0;
    }
    bool batchUses(GLuint tex) const
    {
        return haveBatch && tex && std::find(batchTextures.begin(), batchTextures.end(), tex) != batchTextures.end();
    }

    Impl()
    {
        highPlane.fill(-1);
        owner.fill(-1);
        cpuVersion.fill(1u);
    }

    GLint uniform(const char *name)
    {
        auto it = uniforms.find(name);
        if (it != uniforms.end())
            return it->second;
        const GLint loc = glGetUniformLocation(program, name);
        uniforms.emplace(name, loc);
        return loc;
    }

    // ------------------------------------------------------------ setup

    static GLuint compile(GLenum type, const std::string &source)
    {
        const GLuint shader = glCreateShader(type);
        const char *text = source.c_str();
        glShaderSource(shader, 1, &text, nullptr);
        glCompileShader(shader);
        GLint ok = 0;
        glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
        if (!ok)
        {
            char log[4096];
            glGetShaderInfoLog(shader, sizeof(log), nullptr, log);
            std::cerr << "[gs-gl] shader compile failed:\n" << log << std::endl;
            glDeleteShader(shader);
            return 0;
        }
        return shader;
    }

    static void GLAD_API_PTR debugMessage(GLenum source, GLenum type, GLuint id, GLenum severity, GLsizei length, const GLchar *message,
                                      const void *user)
    {
        (void)source; (void)id; (void)length; (void)user;
        if (type != GL_DEBUG_TYPE_ERROR && severity != GL_DEBUG_SEVERITY_HIGH)
            return;
        static int reported = 0;
        if (reported++ < 50)
            std::cerr << "[gs-gl] GL " << (type == GL_DEBUG_TYPE_ERROR ? "error" : "warning") << ": " << message << std::endl;
    }

    bool init()
    {
        glEnable(GL_DEBUG_OUTPUT);
        glEnable(GL_DEBUG_OUTPUT_SYNCHRONOUS);
        glDebugMessageCallback(&Impl::debugMessage, nullptr);
        {
            // Targets are up to 4096 x 2048 GS pixels.
            glGetIntegerv(GL_MAX_TEXTURE_SIZE, &maxTextureSize);
            chooseScale(hudOutputScale(gsgl::renderScale()), gsgl::progressiveScan());
            scaleRequest = gsgl::renderScale(); // (with DLSS the picture's scale is its output's: not a new request)
            applyPresentScale();
            if (scaled())
                std::cerr << "[gs-gl] internal resolution: " << scaleX << " x " << scaleY << " samples per GS pixel"
                          << (progressive ? " (progressive: each field drawn as a whole frame)" : "") << std::endl;
        }
        {
            GLint extensionCount = 0;
            glGetIntegerv(GL_NUM_EXTENSIONS, &extensionCount);
            static const bool listExtensions = envFlag("GT3_GS_GL_LIST_EXTENSIONS", false); // diagnostic
            for (GLint i = 0; i < extensionCount; ++i)
            {
                const char *name = reinterpret_cast<const char *>(glGetStringi(GL_EXTENSIONS, static_cast<GLuint>(i)));
                if (listExtensions && name)
                    std::cerr << "[gs-gl] extension " << name << std::endl;
                if (name && std::strcmp(name, "GL_ARB_bindless_texture") == 0)
                    hasBindless = true;
                if (name && std::strcmp(name, "GL_NV_conservative_raster") == 0)
                    conservativeRaster = true;
                if (name && std::strcmp(name, "GL_ARB_pipeline_statistics_query") == 0)
                    pipelineStats = true;
                if (name && std::strcmp(name, "GL_ARB_fragment_shader_interlock") == 0)
                    hasInterlock = true;
                if (name && (std::strcmp(name, "GL_KHR_parallel_shader_compile") == 0 || std::strcmp(name, "GL_ARB_parallel_shader_compile") == 0))
                    parallelCompile = true;
            }
            // Triangles drawn as themselves need conservative rasterization
            // (GT3_GS_GL_TRI_BOXES=1: as their bounding boxes, as before).
            if (envFlag("GT3_GS_GL_TRI_BOXES", false))
                conservativeRaster = false;
            gpuProfiling = stats && envFlag("GT3_GS_GL_GPU_PROFILE", false);
            std::cerr << "[gs-gl] triangles drawn as " << (conservativeRaster ? "triangles (conservative rasterization)" : "bounding boxes")
                      << std::endl;
            if (hasBindless && !envFlag("GT3_GS_GL_NO_BINDLESS", false))
            {
                getTextureHandle = reinterpret_cast<uint64_t(GLAD_API_PTR *)(GLuint)>(gsgl::getProcAddress("glGetTextureHandleARB"));
                makeHandleResident = reinterpret_cast<void(GLAD_API_PTR *)(uint64_t)>(gsgl::getProcAddress("glMakeTextureHandleResidentARB"));
                makeHandleNonResident =
                    reinterpret_cast<void(GLAD_API_PTR *)(uint64_t)>(gsgl::getProcAddress("glMakeTextureHandleNonResidentARB"));
                hasBindless = getTextureHandle && makeHandleResident && makeHandleNonResident;
                getTextureSamplerHandle =
                    reinterpret_cast<uint64_t(GLAD_API_PTR *)(GLuint, GLuint)>(gsgl::getProcAddress("glGetTextureSamplerHandleARB"));
                if (hasBindless && getTextureSamplerHandle)
                {
                    glGenSamplers(1, &linearSampler);
                    glSamplerParameteri(linearSampler, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
                    glSamplerParameteri(linearSampler, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
                    glSamplerParameteri(linearSampler, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
                    glSamplerParameteri(linearSampler, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
                }
            }
            else
            {
                hasBindless = false;
            }
            std::cerr << "[gs-gl] textures: " << (hasBindless ? "bindless (one draw per pipeline state)" : "bound per batch") << std::endl;
        }
        // GS_MIPMAP: the per-pixel mip sampler (PS2X_GS_MIPMAP; GT3 builds leave
        // it out, so their programs are as before).
        const std::string header = std::string("#version 430 core\n") +
                                   (hasBindless ? "#extension GL_ARB_bindless_texture : require\n#define GS_BINDLESS 1\n" : "") +
                                   (conservativeRaster ? "#define GS_TRI_GEOMETRY 1\n" : "") +
                                   (gsmip::enabled() ? "#define GS_MIPMAP 1\n" : "") + texSmoothDefines() + experimentDefines() + gsgl::kPrimStruct;
        const GLuint vs = compile(GL_VERTEX_SHADER, header + gsgl::kVertexShader);
        const GLuint fs = compile(GL_FRAGMENT_SHADER, header + gsgl::kFragmentShader);
        if (!vs || !fs)
            return false;
        program = glCreateProgram();
        glAttachShader(program, vs);
        glAttachShader(program, fs);
        glLinkProgram(program);
        glDeleteShader(vs);
        glDeleteShader(fs);
        GLint ok = 0;
        glGetProgramiv(program, GL_LINK_STATUS, &ok);
        if (!ok)
        {
            char log[4096];
            glGetProgramInfoLog(program, sizeof(log), nullptr, log);
            std::cerr << "[gs-gl] program link failed:\n" << log << std::endl;
            return false;
        }
        resolveUniforms(program, 0);
        if (gsmip::enabled() && !log2Lut)
        {
            // The mip LOD's log2 table (gs_mip_lut.h) on unit 12 (u_log2Lut), which
            // nothing else binds.
            glGenTextures(1, &log2Lut);
            glBindTexture(GL_TEXTURE_2D, log2Lut);
            glTexStorage2D(GL_TEXTURE_2D, 1, GL_R32UI, 1025, 1);
            glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
            glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 1025, 1, GL_RED_INTEGER, GL_UNSIGNED_INT, gsmip::kLog2Lut);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
            glBindTexture(GL_TEXTURE_2D, 0);
            glActiveTexture(GL_TEXTURE12);
            glBindTexture(GL_TEXTURE_2D, log2Lut);
            glActiveTexture(GL_TEXTURE0);
            std::cerr << "[gs-gl] GS mipmapping: per-pixel mip primitives sampled on the GPU (PS2X_GS_MIPMAP)" << std::endl;
        }
        // The interlock program is built whenever the GPU has the extension
        // (GT3_GS_GL_INTERLOCK=0: never); whether batches use it follows the
        // scale (applyScalePolicies): by default up to 4 samples per pixel
        // (native, 2x, progressive native), where the renderer thread is the
        // limit (Trial Mountain's start at 2x 59.0 -> 59.8). At 3x/4x the GPU
        // is, and the interlock costs it more than the levels (4x 39.2 ->
        // 37.4). (The rally demo's rare whole-frame compare bursts happen
        // without it too: 1 of 9 runs off, 2 of 9 on.)
        const char *interlockEnv = std::getenv("GT3_GS_GL_INTERLOCK");
        if (hasInterlock && !(interlockEnv && std::strcmp(interlockEnv, "0") == 0))
        {
            // The #extension directive before any other line of the stage.
            const std::string version = "#version 430 core\n";
            const GLuint vsI = compile(GL_VERTEX_SHADER, header + gsgl::kVertexShader);
            const GLuint fsI = compile(GL_FRAGMENT_SHADER, version + "#extension GL_ARB_fragment_shader_interlock : require\n#define GS_INTERLOCK 1\n" +
                                                               header.substr(version.size()) + gsgl::kFragmentShader);
            if (vsI && fsI)
            {
                interlockProgram = glCreateProgram();
                glAttachShader(interlockProgram, vsI);
                glAttachShader(interlockProgram, fsI);
                glLinkProgram(interlockProgram);
                GLint linked = 0;
                glGetProgramiv(interlockProgram, GL_LINK_STATUS, &linked);
                if (!linked)
                {
                    char log[4096];
                    glGetProgramInfoLog(interlockProgram, sizeof(log), nullptr, log);
                    std::cerr << "[gs-gl] interlock program link failed:\n" << log << std::endl;
                    glDeleteProgram(interlockProgram);
                    interlockProgram = 0;
                }
            }
            if (vsI)
                glDeleteShader(vsI);
            if (fsI)
                glDeleteShader(fsI);
            if (interlockProgram)
                resolveUniforms(interlockProgram, 1);
        }
        // Fast mode's rasterizing program (GS_FAST_RASTER): the GPU decides
        // coverage and depth from the vertices, so its early depth test skips
        // hidden samples; built always, used in fast mode (3x/4x).
        {
            const std::string fastHeader = header + "#define GS_FAST_RASTER 1\n" + (conservativeRaster ? "" : "#define GS_TRI_GEOMETRY 1\n");
            const GLuint vsF = compile(GL_VERTEX_SHADER, fastHeader + gsgl::kVertexShader);
            const GLuint fsF = compile(GL_FRAGMENT_SHADER, fastHeader + gsgl::kFragmentShader);
            if (vsF && fsF)
            {
                fastProgram = glCreateProgram();
                glAttachShader(fastProgram, vsF);
                glAttachShader(fastProgram, fsF);
                glLinkProgram(fastProgram);
                GLint linked = 0;
                glGetProgramiv(fastProgram, GL_LINK_STATUS, &linked);
                if (!linked)
                {
                    glDeleteProgram(fastProgram);
                    fastProgram = 0;
                }
            }
            if (vsF)
                glDeleteShader(vsF);
            if (fsF)
                glDeleteShader(fsF);
            if (fastProgram)
                resolveUniforms(fastProgram, 2);
            // Its lean variant (GS_SPEC_TEX15; GT3_GS_GL_SPEC=0 leaves it out):
            // batches whose states all sample a GPU-decoded colour texture and
            // read no target.
            if (fastProgram && !(std::getenv("GT3_GS_GL_SPEC") && std::strcmp(std::getenv("GT3_GS_GL_SPEC"), "0") == 0))
            {
                const std::string specHeader = fastHeader + "#define GS_SPEC_TEX15 1\n";
                const GLuint vsS = compile(GL_VERTEX_SHADER, specHeader + gsgl::kVertexShader);
                const GLuint fsS = compile(GL_FRAGMENT_SHADER, specHeader + gsgl::kFragmentShader);
                if (vsS && fsS)
                {
                    specTex15Program = glCreateProgram();
                    glAttachShader(specTex15Program, vsS);
                    glAttachShader(specTex15Program, fsS);
                    glLinkProgram(specTex15Program);
                    GLint linked = 0;
                    glGetProgramiv(specTex15Program, GL_LINK_STATUS, &linked);
                    if (!linked)
                    {
                        glDeleteProgram(specTex15Program);
                        specTex15Program = 0;
                    }
                }
                if (vsS)
                    glDeleteShader(vsS);
                if (fsS)
                    glDeleteShader(fsS);
                if (specTex15Program)
                    resolveUniforms(specTex15Program, 3);
            }
            // And for batches of triangles only (GS_HW_TRI; GT3_GS_GL_HWTRI=0
            // leaves it out): the GPU interpolates the corners' attributes, so
            // fragments read no primitive record.
            if (specTex15Program && !(std::getenv("GT3_GS_GL_HWTRI") && std::strcmp(std::getenv("GT3_GS_GL_HWTRI"), "0") == 0))
            {
                const std::string hwHeader = fastHeader + "#define GS_SPEC_TEX15 1\n#define GS_HW_TRI 1\n";
                const GLuint vsH = compile(GL_VERTEX_SHADER, hwHeader + gsgl::kVertexShader);
                const GLuint fsH = compile(GL_FRAGMENT_SHADER, hwHeader + gsgl::kFragmentShader);
                if (vsH && fsH)
                {
                    hwTriProgram = glCreateProgram();
                    glAttachShader(hwTriProgram, vsH);
                    glAttachShader(hwTriProgram, fsH);
                    glLinkProgram(hwTriProgram);
                    GLint linked = 0;
                    glGetProgramiv(hwTriProgram, GL_LINK_STATUS, &linked);
                    if (!linked)
                    {
                        char log[4096];
                        glGetProgramInfoLog(hwTriProgram, sizeof(log), nullptr, log);
                        std::cerr << "[gs-gl] GS_HW_TRI program link failed:\n" << log << std::endl;
                        glDeleteProgram(hwTriProgram);
                        hwTriProgram = 0;
                    }
                }
                if (vsH)
                    glDeleteShader(vsH);
                if (fsH)
                    glDeleteShader(fsH);
                if (hwTriProgram)
                {
                    resolveUniforms(hwTriProgram, 4);
                    hwTriHeader = hwHeader;
                    specialise = envFlag("GT3_GS_GL_SPECIALISE", true);
                    // As many compiler threads as the driver likes.
                    auto maxThreads = reinterpret_cast<void(GLAD_API_PTR *)(GLuint)>(gsgl::getProcAddress("glMaxShaderCompilerThreadsKHR"));
                    if (!maxThreads)
                        maxThreads = reinterpret_cast<void(GLAD_API_PTR *)(GLuint)>(gsgl::getProcAddress("glMaxShaderCompilerThreadsARB"));
                    if (maxThreads && parallelCompile)
                        maxThreads(0xFFFFFFFFu);
                    else
                        parallelCompile = false;
                }
                if (hwTriProgram && !(std::getenv("GT3_GS_GL_NOAT") && std::strcmp(std::getenv("GT3_GS_GL_NOAT"), "0") == 0))
                {
                    const std::string noAtHeader = hwHeader + "#define GS_NO_ALPHA_TEST 1\n";
                    const GLuint vsN = compile(GL_VERTEX_SHADER, noAtHeader + gsgl::kVertexShader);
                    const GLuint fsN = compile(GL_FRAGMENT_SHADER, noAtHeader + gsgl::kFragmentShader);
                    if (vsN && fsN)
                    {
                        hwTriNoAtProgram = glCreateProgram();
                        glAttachShader(hwTriNoAtProgram, vsN);
                        glAttachShader(hwTriNoAtProgram, fsN);
                        glLinkProgram(hwTriNoAtProgram);
                        GLint linked = 0;
                        glGetProgramiv(hwTriNoAtProgram, GL_LINK_STATUS, &linked);
                        if (!linked)
                        {
                            glDeleteProgram(hwTriNoAtProgram);
                            hwTriNoAtProgram = 0;
                        }
                    }
                    if (vsN)
                        glDeleteShader(vsN);
                    if (fsN)
                        glDeleteShader(fsN);
                    if (hwTriNoAtProgram)
                        resolveUniforms(hwTriNoAtProgram, 5);
                }
            }
            // GT3_GS_GL_DUMP_PROGRAMS=<prefix>: the drivers' program binaries
            // (NVIDIA's include the compiled assembly: register counts).
            if (const char *dump = std::getenv("GT3_GS_GL_DUMP_PROGRAMS"); dump && *dump)
            {
                const std::pair<const char *, GLuint> programs[] = {
                    {"main", program}, {"fast", fastProgram}, {"interlock", interlockProgram}, {"spec", specTex15Program}, {"hwtri", hwTriProgram}, {"hwtri-noat", hwTriNoAtProgram}};
                for (const auto &[name, id] : programs)
                {
                    if (!id)
                        continue;
                    GLint length = 0;
                    glGetProgramiv(id, GL_PROGRAM_BINARY_LENGTH, &length);
                    std::vector<char> binary(static_cast<size_t>(std::max(length, 0)));
                    GLenum format = 0;
                    GLsizei written = 0;
                    if (length > 0)
                        glGetProgramBinary(id, length, &written, &format, binary.data());
                    if (FILE *f = std::fopen((std::string(dump) + "-" + name + ".bin").c_str(), "wb"))
                    {
                        std::fwrite(binary.data(), 1, static_cast<size_t>(written), f);
                        std::fclose(f);
                    }
                }
            }
            // Depth range 0..1 as it is (Z / 2^24 exactly in float): the other
            // programs write gl_FragDepth, so only this one depends on it.
            auto clipControl = reinterpret_cast<void(GLAD_API_PTR *)(GLenum, GLenum)>(gsgl::getProcAddress("glClipControl"));
            if (clipControl)
            {
                clipControl(0x8CA1 /* GL_LOWER_LEFT */, 0x935F /* GL_ZERO_TO_ONE */);
                // Z beyond 2^24 at a vertex: no clipping, each pixel's depth
                // clamped after interpolation (as the GS clamps).
                glEnable(GL_DEPTH_CLAMP);
            }
            else if (fastProgram)
            {
                glDeleteProgram(fastProgram);
                fastProgram = 0;
                if (specTex15Program)
                    glDeleteProgram(specTex15Program);
                specTex15Program = 0;
                if (hwTriProgram)
                    glDeleteProgram(hwTriProgram);
                hwTriProgram = 0;
                if (hwTriNoAtProgram)
                    glDeleteProgram(hwTriNoAtProgram);
                hwTriNoAtProgram = 0;
            }
        }
        applyScalePolicies();
        std::cerr << "[gs-gl] blending batches that write no depth: "
                  << (interlockActive ? "one draw each (fragment shader interlock)" : "overlap levels") << std::endl;
        if (fastBlend)
            std::cerr << "[gs-gl] fast mode: blending on the GPU's blend unit where it can (not bit-exact)" << std::endl;
#if defined(PS2X_GAME_GT4) && !defined(GT4_STOCK_BUILD)
        // GT4 (the Full launcher's Display > Resolution, analysis/design/full-m2 README, Resolution): the internal resolution
        // (GT3_GS_GL_SCALE) and the drawing (GT3_GS_GL_FAST) in one line at every start, native included. The launcher
        // reads the marker string in GT4.exe (gt4full_model.h kInternalResMarker): its internal resolution was checked with
        // GT4 at native, 2x, 3x and 4x on Vulkan. Not in the Classic build (always native: gs_gl_present.cpp initialScale).
        std::cerr << "[gs-gl] " << "GT4 internal resolution (full-m2)" << ": " << scaleX << "x, GT4's " << 640 << " x " << 448 << " picture drawn at "
                  << 640 * scaleX << " x " << 448 * outRows << ", " << (fastBlend ? "fast" : "exact") << " drawing" << std::endl;
        {
            // The launcher reads this marker from GT4.exe (gt4full_model.h kTexSmoothMarker) before it passes GT4_TEX_SMOOTH.
            const char *e = std::getenv("GT4_TEX_SMOOTH");
            const bool asked = e && (std::strcmp(e, "on") == 0 || std::strcmp(e, "1") == 0);
            texSmooth = asked && scaleX >= 2;
            std::cerr << "[gs-gl] " << "GT4 texture smoothing (tex-smooth)" << ": " << (texSmooth ? "on" : "off")
                      << (asked && !texSmooth ? " (native: GT4's own sampling)" : "") << std::endl;
        }
#endif
        glGenVertexArrays(1, &vao);
        glGenBuffers(1, &primBuffer);
        bufferStorage = reinterpret_cast<void(GLAD_API_PTR *)(GLenum, GLsizeiptr, const void *, GLbitfield)>(
            gsgl::getProcAddress("glBufferStorage"));
        if (bufferStorage && !envFlag("GT3_GS_GL_NO_RING", false))
        {
            glGetIntegerv(GL_SHADER_STORAGE_BUFFER_OFFSET_ALIGNMENT, &ssboAlign);
            ssboAlign = std::max(ssboAlign, 16);
            glGenBuffers(1, &uploadRing);
            glBindBuffer(GL_SHADER_STORAGE_BUFFER, uploadRing);
            const GLbitfield flags = GL_MAP_WRITE_BIT | GL_MAP_PERSISTENT_BIT | GL_MAP_COHERENT_BIT;
            bufferStorage(GL_SHADER_STORAGE_BUFFER, static_cast<GLsizeiptr>(kRingRegion * kRingRegions), nullptr, flags);
            uploadRingPtr = static_cast<uint8_t *>(
                glMapBufferRange(GL_SHADER_STORAGE_BUFFER, 0, static_cast<GLsizeiptr>(kRingRegion * kRingRegions), flags));
            glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);
            GLint maxBlock = 0;
            glGetIntegerv(GL_MAX_SHADER_STORAGE_BLOCK_SIZE, &maxBlock);
            ringWhole = uploadRingPtr && static_cast<uint64_t>(static_cast<uint32_t>(maxBlock)) >= kRingRegion * kRingRegions;
            std::cerr << "[gs-gl] primitive upload ring " << (uploadRingPtr ? "mapped" : "unavailable") << (ringWhole ? ", bound whole" : "")
                      << std::endl;
        }
        glGenBuffers(1, &stateBuffer);
        glGenFramebuffers(1, &fbo);
        glGenFramebuffers(1, &readFbo);
        if (interlockProgram)
        {
            // Big enough for any target's viewport (4096 x 2048 GS pixels) at
            // any scale it can switch to (4x, progressive: 4 x 8 samples).
            GLint maxW = 16384, maxH = 16384;
            glGetIntegerv(GL_MAX_FRAMEBUFFER_WIDTH, &maxW);
            glGetIntegerv(GL_MAX_FRAMEBUFFER_HEIGHT, &maxH);
            glGenFramebuffers(1, &interlockFbo);
            glBindFramebuffer(GL_FRAMEBUFFER, interlockFbo);
            glFramebufferParameteri(GL_FRAMEBUFFER, GL_FRAMEBUFFER_DEFAULT_WIDTH, std::min(maxW, 4096 * 4));
            glFramebufferParameteri(GL_FRAMEBUFFER, GL_FRAMEBUFFER_DEFAULT_HEIGHT, std::min(maxH, 2048 * 8));
            glDrawBuffer(GL_NONE);
            glBindFramebuffer(GL_FRAMEBUFFER, 0);
        }
        buildViewTables();
        if (!envFlag("GT3_GS_GL_NO_BARRIER", false))
        {
            textureBarrier = reinterpret_cast<void(GLAD_API_PTR *)()>(gsgl::getProcAddress("glTextureBarrier"));
            if (!textureBarrier)
                textureBarrier = reinterpret_cast<void(GLAD_API_PTR *)()>(gsgl::getProcAddress("glTextureBarrierNV"));
        }
        std::cerr << "[gs-gl] blending reads the target " << (textureBarrier ? "in place (texture barrier)" : "through a copy") << std::endl;
        clearTexImage = reinterpret_cast<void(GLAD_API_PTR *)(GLuint, GLint, GLenum, GLenum, const void *)>(gsgl::getProcAddress("glClearTexImage"));
        if (stableTargets)
            std::cerr << "[gs-gl] stable targets " << stableTargets << " (PS2X_GS_GL_STABLE_TARGETS): pooled target textures"
                      << (clearTexImage ? " (cleared on reuse)" : "") << ", framebuffers per texture"
                      << (stableTargets >= 2 ? ", full height at native" : "") << std::endl;
        if (partialPatches)
            std::cerr << "[gs-gl] PSMT8/PSMT4 uploads into GPU-held 32-bit pages merged on the GPU (PS2X_GS_GL_PARTIAL_PATCH)"
                      << (textureBarrier ? (coalescePatches ? "" : ": off (needs the coalesced upload patches)") : ": off (no texture barrier)")
                      << std::endl;
        std::cerr << "[gs-gl] PSMZ16 Z buffers: "
                  << (z16Gpu ? (z16Moves ? "on the GPU, their page moves too (PS2X_GS_GL_Z16)" : "on the GPU, page moves through the CPU copy (PS2X_GS_GL_Z16_MOVES=0)")
                             : "drawn by the CPU renderer (PS2X_GS_GL_Z16=0)")
                  << std::endl;

        {
            const std::string convVs = R"GLSL(#version 430 core
uniform ivec4 u_rect; // destination x y w h
uniform vec2 u_size;  // destination texture size
// Batched page moves: one instance per page, (source x y, destination x y)
// from the buffer; each a 64 x u_pageH rectangle.
uniform int u_instanced;
uniform int u_pageH;
layout(std430, binding = 2) readonly buffer Moves { ivec4 moves[]; };
flat out ivec4 v_move;
void main()
{
    int c = gl_VertexID % 6;
    int cx = (c == 1 || c == 3 || c == 4) ? 1 : 0;
    int cy = (c == 2 || c == 4 || c == 5) ? 1 : 0;
    ivec4 r = u_rect;
    v_move = ivec4(0);
    if (u_instanced != 0)
    {
        v_move = moves[gl_InstanceID];
        r = ivec4(v_move.zw, 64, u_pageH);
    }
    vec2 p = vec2(r.x + cx * r.z, r.y + cy * r.w);
    gl_Position = vec4(p / u_size * 2.0 - 1.0, 0.0, 1.0);
}
)GLSL";
            const std::string convFs = R"GLSL(#version 430 core
uniform int u_mode;      // 0 colour -> colour, 1 colour -> Z, 2 Z -> colour (RGB), 3 CT16 -> its 16-bit value
uniform int u_lut;       // 0 same layout, 1 destination Z32 from CT32, 2 destination CT32 from Z32,
                         // 3 destination CT16 from CT32, 4 destination CT32 from CT16,
                         // 7 destination Z16 (depth) from CT32, 8 destination CT32 from Z16 (depth),
                         // 9 destination Z16 (depth) from CT16, 10 destination CT16 from Z16 (depth)
                         // (7 and 9 read their table as u_lut16from32, 8 and 10 as u_lut32from16)
uniform ivec2 u_srcOrigin; // source page origin (single move)
uniform ivec2 u_dstOrigin; // destination page origin (single move)
uniform int u_instanced;
uniform ivec2 u_scale;   // samples per GS pixel of the targets (the page image of 5 is native)
uniform ivec2 u_srcScale; // the source's, when it differs (effects at their own scale); 0 0: u_scale
flat in ivec4 v_move;
ivec2 u_src, u_dst;
uniform sampler2D u_tex;
uniform usampler2D u_lutZfromC;
uniform usampler2D u_lutCfromZ;
uniform usampler2D u_lut16from32;
uniform usampler2D u_lut32from16;
uniform usampler2D u_mask; // 5: pixels to write; 6: the bits of each pixel's word to write
uniform sampler2D u_dstTex; // 6: the destination itself (texture barrier: each fragment reads its own sample)
out vec4 o_color;
// A CT16 target texel (5551 in the top bits of each byte) as its 16-bit value.
uint pack16(vec4 c)
{
    uvec4 b = uvec4(round(c * 255.0));
    return (b.r >> 3) | ((b.g >> 3) << 5) | ((b.b >> 3) << 10) | ((b.a >> 7) << 15);
}
// A Z16 depth sample (z / 2^24) as its 16-bit value.
uint z16(float d)
{
    return min(uint(round(max(d, 0.0) * 16777216.0)), 65535u);
}
void main()
{
    u_src = u_instanced != 0 ? v_move.xy : u_srcOrigin;
    u_dst = u_instanced != 0 ? v_move.zw : u_dstOrigin;
    ivec2 fc = ivec2(gl_FragCoord.xy);
    ivec2 sub = fc % u_scale; // the sample of the pixel, copied to the same sample
    ivec2 local = fc / u_scale - u_dst;
    ivec2 ss = u_srcScale.x == 0 ? u_scale : u_srcScale;
    ivec2 ssub = ss == u_scale ? sub : (sub * ss) / u_scale;
    ivec2 s = local;
    if (u_lut == 5)
    {
        // Words from the CPU at their pixels (u_tex a page-sized image).
        if (texelFetch(u_mask, local, 0).r == 0u)
            discard;
        vec4 c = texelFetch(u_tex, local, 0);
        if (u_mode == 1)
        {
            uvec4 b = uvec4(round(c * 255.0));
            gl_FragDepth = float(b.r | (b.g << 8) | (b.b << 16)) / 16777216.0;
            o_color = vec4(0.0);
        }
        else
        {
            o_color = c;
        }
        return;
    }
    if (u_lut == 6)
    {
        // Upload bits merged into the destination's words (PSMT8/PSMT4 uploads
        // into a 32-bit target page): new = (old & ~m) | (upload & m).
        uint m = texelFetch(u_mask, local, 0).r;
        if (m == 0u)
            discard;
        uvec4 nb = uvec4(round(texelFetch(u_tex, local, 0) * 255.0));
        uvec4 ob = uvec4(round(texelFetch(u_dstTex, fc, 0) * 255.0));
        uint nw = nb.r | (nb.g << 8) | (nb.b << 16) | (nb.a << 24);
        uint ow = ob.r | (ob.g << 8) | (ob.b << 16) | (ob.a << 24);
        uint w = (ow & ~m) | (nw & m);
        o_color = vec4(float(w & 255u), float((w >> 8) & 255u), float((w >> 16) & 255u), float(w >> 24)) / 255.0;
        return;
    }
    if (u_lut == 7 || u_lut == 9)
    {
        // Z16 depth from a CT32 page (7: the half of the CT32 pixel's word holding this Z16 pixel's halfword, the
        // table as lut 3's) or a CT16 page (9: the CT16 pixel of the same halfword, its 16-bit value).
        uint e = texelFetch(u_lut16from32, local, 0).r;
        uint h;
        if (u_lut == 7)
        {
            uvec4 b = uvec4(round(texelFetch(u_tex, (u_src + ivec2(int(e & 63u), int((e >> 6) & 31u))) * ss + ssub, 0) * 255.0));
            h = ((e >> 11) & 1u) != 0u ? (b.b | (b.a << 8)) : (b.r | (b.g << 8));
        }
        else
            h = pack16(texelFetch(u_tex, (u_src + ivec2(int(e & 63u), int(e >> 6))) * ss + ssub, 0));
        gl_FragDepth = float(h) / 16777216.0;
        o_color = vec4(0.0);
        return;
    }
    if (u_lut == 8)
    {
        // A CT32 word from its two Z16 pixels (low half, high half).
        uvec2 e = texelFetch(u_lut32from16, local, 0).rg;
        uint lo = z16(texelFetch(u_tex, (u_src + ivec2(int(e.x & 63u), int(e.x >> 6))) * ss + ssub, 0).r);
        uint hi = z16(texelFetch(u_tex, (u_src + ivec2(int(e.y & 63u), int(e.y >> 6))) * ss + ssub, 0).r);
        o_color = vec4(float(lo & 255u), float(lo >> 8), float(hi & 255u), float(hi >> 8)) / 255.0;
        return;
    }
    if (u_lut == 10)
    {
        // A CT16 pixel from the Z16 pixel of the same halfword (5551 in the top bits of each byte, as lut 3).
        uint e = texelFetch(u_lut32from16, local, 0).r;
        uint h = z16(texelFetch(u_tex, (u_src + ivec2(int(e & 63u), int(e >> 6))) * ss + ssub, 0).r);
        o_color = vec4(float((h & 31u) << 3), float(((h >> 5) & 31u) << 3), float(((h >> 10) & 31u) << 3), float(((h >> 15) & 1u) << 7)) / 255.0;
        return;
    }
    if (u_lut == 3)
    {
        uint e = texelFetch(u_lut16from32, local, 0).r;
        uvec4 b = uvec4(round(texelFetch(u_tex, (u_src + ivec2(int(e & 63u), int((e >> 6) & 31u))) * ss + ssub, 0) * 255.0));
        uint h = ((e >> 11) & 1u) != 0u ? (b.b | (b.a << 8)) : (b.r | (b.g << 8));
        o_color = vec4(float((h & 31u) << 3), float(((h >> 5) & 31u) << 3), float(((h >> 10) & 31u) << 3), float(((h >> 15) & 1u) << 7)) / 255.0;
        return;
    }
    if (u_lut == 4)
    {
        uvec2 e = texelFetch(u_lut32from16, local, 0).rg;
        uint lo = pack16(texelFetch(u_tex, (u_src + ivec2(int(e.x & 63u), int(e.x >> 6))) * ss + ssub, 0));
        uint hi = pack16(texelFetch(u_tex, (u_src + ivec2(int(e.y & 63u), int(e.y >> 6))) * ss + ssub, 0));
        o_color = vec4(float(lo & 255u), float(lo >> 8), float(hi & 255u), float(hi >> 8)) / 255.0;
        return;
    }
    if (u_lut == 1)
    {
        uint e = texelFetch(u_lutZfromC, local, 0).r;
        s = ivec2(int(e & 63u), int(e >> 6));
    }
    else if (u_lut == 2)
    {
        uint e = texelFetch(u_lutCfromZ, local, 0).r;
        s = ivec2(int(e & 63u), int(e >> 6));
    }
    vec4 c = texelFetch(u_tex, (u_src + s) * ss + ssub, 0);
    if (u_mode == 2)
    {
        // Exact: depth is z / 2^24 (adding 0.5 in float would round odd z above 2^23 up).
        uint z = min(uint(round(c.r * 16777216.0)), 16777215u);
        o_color = vec4(float(z & 255u), float((z >> 8) & 255u), float((z >> 16) & 255u), 0.0) / 255.0;
    }
    else if (u_mode == 1)
    {
        uvec4 b = uvec4(round(c * 255.0));
        gl_FragDepth = float(b.r | (b.g << 8) | (b.b << 16)) / 16777216.0;
        o_color = vec4(0.0);
    }
    else if (u_mode == 3)
    {
        // A CT16 target's sample as its 16-bit value (GT4's race colour: a new video mode forgets full colour).
        uvec4 b = uvec4(round(c * 255.0)) & uvec4(0xF8u, 0xF8u, 0xF8u, 0x80u);
        o_color = vec4(b) / 255.0;
    }
    else
    {
        o_color = c;
    }
}
)GLSL";
            const GLuint cvs = compile(GL_VERTEX_SHADER, convVs);
            const GLuint cfs = compile(GL_FRAGMENT_SHADER, convFs);
            if (!cvs || !cfs)
                return false;
            convProgram = glCreateProgram();
            glAttachShader(convProgram, cvs);
            glAttachShader(convProgram, cfs);
            glLinkProgram(convProgram);
            glDeleteShader(cvs);
            glDeleteShader(cfs);
            GLint linked = 0;
            glGetProgramiv(convProgram, GL_LINK_STATUS, &linked);
            if (!linked)
                return false;
            glGenFramebuffers(1, &convFbo);
#if defined(VKGL_REDIRECT)
            if (partialPatches && textureBarrier)
            {
                // convFs's u_lut 6 with the destination read as u_dst at its own
                // pixel (vkgl turns that read into an input attachment read).
                const std::string mergeFs = R"GLSL(#version 430 core
uniform ivec2 u_scale;
uniform ivec2 u_dstOrigin;
uniform sampler2D u_tex;
uniform usampler2D u_mask;
uniform sampler2D u_dst;
out vec4 o_color;
void main()
{
    ivec2 pix = ivec2(gl_FragCoord.xy);
    ivec2 local = pix / u_scale - u_dstOrigin;
    uint m = texelFetch(u_mask, local, 0).r;
    if (m == 0u)
        discard;
    uvec4 nb = uvec4(round(texelFetch(u_tex, local, 0) * 255.0));
    uvec4 ob = uvec4(round(texelFetch(u_dst, pix, 0) * 255.0));
    uint nw = nb.r | (nb.g << 8) | (nb.b << 16) | (nb.a << 24);
    uint ow = ob.r | (ob.g << 8) | (ob.b << 16) | (ob.a << 24);
    uint w = (ow & ~m) | (nw & m);
    o_color = vec4(float(w & 255u), float((w >> 8) & 255u), float((w >> 16) & 255u), float(w >> 24)) / 255.0;
}
)GLSL";
                const GLuint mvs = compile(GL_VERTEX_SHADER, convVs);
                const GLuint mfs = compile(GL_FRAGMENT_SHADER, mergeFs);
                GLint mergeLinked = 0;
                if (mvs && mfs)
                {
                    mergeProgram = glCreateProgram();
                    glAttachShader(mergeProgram, mvs);
                    glAttachShader(mergeProgram, mfs);
                    glLinkProgram(mergeProgram);
                    glGetProgramiv(mergeProgram, GL_LINK_STATUS, &mergeLinked);
                }
                if (mvs)
                    glDeleteShader(mvs);
                if (mfs)
                    glDeleteShader(mfs);
                if (!mergeLinked)
                {
                    std::cerr << "[gs-vk] partial-word merge program did not build: PSMT8/PSMT4 uploads copy pages back" << std::endl;
                    if (mergeProgram)
                        glDeleteProgram(mergeProgram);
                    mergeProgram = 0;
                }
            }
            if (!mergeProgram)
                partialPatches = false;
#endif
        }
        {
            const std::string vs = R"GLSL(#version 430 core
void main()
{
    int c = gl_VertexID % 6;
    vec2 p = vec2((c == 1 || c == 3 || c == 4) ? 1.0 : -1.0, (c == 2 || c == 4 || c == 5) ? 1.0 : -1.0);
    gl_Position = vec4(p, 0.0, 1.0);
}
)GLSL";
            // Rows as GSCpuBackend::PresentFromLocalMemory: 0 as read; for an
            // interlaced half-height field on rows of parity u_parity: 1 line-
            // doubled, 2 woven with the previous field, 3 bob (the other rows
            // interpolated), 4 adaptive: the previous field's row clamped to
            // the range of the current field's rows above and below (plus
            // u_margin), so static detail weaves and moving edges do not comb;
            // 5 progressive: the current field's sample rows as they are,
            // u_shift lower; 6 motion adaptive (GT4_DEINTERLACE=adaptive): the
            // previous field's row where the picture is still, the current
            // field's rows above and below averaged where it moves, motion
            // being a difference above u_motion against the fields two before
            // (u_prev2 the current field's parity, u_prev3 the previous one's;
            // the rule of PCSX2's FastMAD). Fields hold u_scale samples per GS
            // pixel; the output u_outRows rows per display line (mode 0 with
            // twice the sample rows averages each pair). Alpha 255.
            // u_c16: the fields hold a PSMCT16 picture (GT4's 480p and 1080i;
            // its 5-bit channels as v << 3, Layout::C16): each channel read as
            // the CPU presenter does, v << 3 | v >> 2. u_mergeDx (-1 / +1, 0
            // off): two read circuits of the buffer, circuit 1 offset by less
            // than a GS pixel (GT4's 1080i softening): each pixel mixed with
            // that neighbour by u_mergeW = ALP / 255 x offset / VCKs per pixel
            // (the merge circuit's blend, box-filtered over the pixel's VCKs;
            // analysis/design/video-modes/VIDEO-MODES.md 3.4). u_black: black.
            const std::string fs = R"GLSL(#version 430 core
uniform sampler2D u_cur;
uniform sampler2D u_prev;
uniform sampler2D u_prev2;
uniform sampler2D u_prev3;
uniform float u_motion;
uniform int u_mode;
uniform int u_parity;
uniform int u_rows;
uniform float u_margin;
uniform ivec2 u_scale;
uniform int u_outRows;
uniform int u_shift;
uniform int u_c16;
uniform int u_cols;
uniform int u_mergeDx;
uniform float u_mergeW;
uniform int u_black;
out vec4 o_color;
int g_sub;
vec4 px(sampler2D t, ivec2 q)
{
    vec4 c = texelFetch(t, q, 0);
    if (u_c16 != 0)
    {
        vec3 v = floor(floor(c.rgb * 255.0 + 0.5) / 8.0);
        c.rgb = (v * 8.0 + floor(v / 4.0)) / 255.0;
    }
    return c;
}
vec4 field(sampler2D t, int x, int k) { return px(t, ivec2(x, clamp(k, 0, u_rows - 1) * u_scale.y + g_sub)); }
vec4 compose(ivec2 p)
{
    int line = p.y / u_outRows;
    g_sub = p.y - line * u_outRows;
    vec4 c;
    int k = line >> 1;
    if (u_mode == 0)
    {
        if (u_scale.y == u_outRows)
            c = px(u_cur, p);
        else
            c = 0.5 * (px(u_cur, ivec2(p.x, 2 * p.y)) + px(u_cur, ivec2(p.x, 2 * p.y + 1)));
    }
    else if (u_mode == 5)
        c = px(u_cur, ivec2(p.x, clamp(p.y - u_shift, 0, u_rows * u_scale.y - 1)));
    else if (u_mode == 1)
        c = field(u_cur, p.x, k);
    else if ((line & 1) == u_parity)
        c = field(u_cur, p.x, k);
    else
    {
        // Current-field rows above and below this one.
        int above = u_parity == 0 ? k : k - 1;
        vec4 a = field(u_cur, p.x, above), b = field(u_cur, p.x, above + 1);
        vec4 w = field(u_prev, p.x, k);
        if (u_mode == 2)
            c = w;
        else if (u_mode == 3)
            c = 0.5 * (a + b);
        else if (u_mode == 6 && (above < 0 || above + 1 >= u_rows))
            c = w; // the top and bottom edge rows: woven, as PCSX2's FastMAD does
        else if (u_mode == 6)
        {
            // Motion: this place in the current and the previous field
            // against the same field parity two fields earlier.
            vec4 ao = field(u_prev2, p.x, above), bo = field(u_prev2, p.x, above + 1), wo = field(u_prev3, p.x, k);
            vec3 ma = abs(a.rgb - ao.rgb), mb = abs(b.rgb - bo.rgb), mw = abs(w.rgb - wo.rgb);
            float m = max(max(max(ma.r, ma.g), ma.b), max(max(max(mb.r, mb.g), mb.b), max(max(mw.r, mw.g), mw.b)));
            if (m > u_motion)
                c = 0.5 * (a + b);
            else if (m > 0.0)
            {
                // Nearly still: the previous field's row unless it stands
                // out from the rows above and below while those two agree.
                vec3 ab = abs(a.rgb - b.rgb), aw = abs(a.rgb - w.rgb);
                float dab = max(max(ab.r, ab.g), ab.b) - u_motion, daw = max(max(aw.r, aw.g), aw.b) - u_motion;
                c = (dab < 0.0 && daw >= dab * 0.9) ? 0.5 * (a + b) : w;
            }
            else
                c = w;
        }
        else
            c = clamp(w, min(a, b) - u_margin, max(a, b) + u_margin);
    }
    return c;
}
void main()
{
    if (u_black != 0)
    {
        o_color = vec4(0.0, 0.0, 0.0, 1.0);
        return;
    }
    ivec2 p = ivec2(gl_FragCoord.xy);
    vec4 c = compose(p);
    if (u_mergeDx != 0)
    {
        int nx = clamp(p.x + u_mergeDx * u_scale.x, 0, u_cols * u_scale.x - 1);
        c = mix(c, compose(ivec2(nx, p.y)), u_mergeW);
        // Rounded to 8 bits here, as the CPU presenter rounds (lround): the
        // driver's own conversion may land 1 lower (NVIDIA).
        c.rgb = floor(c.rgb * 255.0 + 0.5) / 255.0;
    }
    o_color = vec4(c.rgb, 1.0);
}
)GLSL";
            const GLuint pvs = compile(GL_VERTEX_SHADER, vs);
            const GLuint pfs = compile(GL_FRAGMENT_SHADER, fs);
            if (!pvs || !pfs)
                return false;
            presentProgram = glCreateProgram();
            glAttachShader(presentProgram, pvs);
            glAttachShader(presentProgram, pfs);
            glLinkProgram(presentProgram);
            glDeleteShader(pvs);
            glDeleteShader(pfs);
            GLint linked = 0;
            glGetProgramiv(presentProgram, GL_LINK_STATUS, &linked);
            if (!linked)
                return false;
            glGenFramebuffers(1, &presentFbo);
            fieldTex[0] = makeColorTexture(640 * scaleX, 512 * scaleY);
            fieldTex[1] = makeColorTexture(640 * scaleX, 512 * scaleY);
        }
        // Copies of target rows. Natively 2048 x 2048 each; at a scale the
        // copy for blending (only without a texture barrier) is made at that
        // scale, and the copy for self-reading draws grows as they need it.
        if (!scaled() || !textureBarrier)
            dstScratch = makeColorTexture(2048 * scaleX, 2048 * scaleY);
        if (!scaled())
        {
            texScratch = makeColorTexture(2048, 2048);
            texScratchW = texScratchH = 2048;
        }
        if (envFlag("GT3_GS_GL_SELFTEST", false) && !scaled())
            selfTestConversions();
        return glGetError() == GL_NO_ERROR;
    }

    // GT3_GS_GL_SELFTEST=1: a CT32 page -> Z24 depth -> CT32 (RGB) round trip
    // through the conversion shaders, compared word by word.
    void selfTestConversions()
    {
        auto makeTarget = [this](bool depth) {
            auto t = std::make_unique<Target>();
            t->depth = depth;
            t->layout = depth ? Layout::Z32 : Layout::C32;
            t->fbp = 0;
            t->fbw = 1;
            t->width = 64;
            t->height = 32;
            t->pageHeight = 32;
            t->sx = scaleX;
            t->sy = scaleY;
            t->tex = makeTargetTexture(depth, 64, 32);
            return t;
        };
        auto a = makeTarget(false), z = makeTarget(true), b = makeTarget(false);
        std::vector<uint32_t> src(64 * 32), alpha(64 * 32, 0x5A000000u);
        for (uint32_t i = 0; i < src.size(); ++i)
            src[i] = (i * 2654435761u) ^ (i << 7);
        src[0] = 0x80FFFFFFu;
        src[1] = 0x80000000u;
        src[2] = 0x00FFFFFEu;
        glBindTexture(GL_TEXTURE_2D, a->tex);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 64, 32, GL_RGBA, GL_UNSIGNED_BYTE, src.data());
        glBindTexture(GL_TEXTURE_2D, b->tex);
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 64, 32, GL_RGBA, GL_UNSIGNED_BYTE, alpha.data());
        glBindTexture(GL_TEXTURE_2D, 0);
        convertTex(a->tex, *z, 0, 0, 0, 0, 1, 1, 15);
        convertTex(z->tex, *b, 0, 0, 0, 0, 2, 2, 7);
        std::vector<uint32_t> out(64 * 32);
        std::vector<float> depth(64 * 32);
        glBindTexture(GL_TEXTURE_2D, b->tex);
        glGetTexImage(GL_TEXTURE_2D, 0, GL_RGBA, GL_UNSIGNED_BYTE, out.data());
        glBindTexture(GL_TEXTURE_2D, z->tex);
        glGetTexImage(GL_TEXTURE_2D, 0, GL_DEPTH_COMPONENT, GL_FLOAT, depth.data());
        glBindTexture(GL_TEXTURE_2D, 0);
        int bad = 0;
        for (uint32_t i = 0; i < out.size(); ++i)
        {
            const uint32_t want = (src[i] & 0x00FFFFFFu) | 0x5A000000u;
            if (out[i] != want)
            {
                if (bad < 8)
                    std::fprintf(stderr, "[gs-gl] selftest C->Z->C pixel %u: got %08x want %08x\n", i, out[i], want);
                ++bad;
            }
        }
        // The depth value of pixel 0's word: its Z32 pixel.
        const uint32_t zp = pixelOfWordZ[wordOfPixelC[0]];
        std::fprintf(stderr, "[gs-gl] selftest C->Z->C: %d of 2048 words differ; depth of word 0 = %.9f (x %u)\n", bad,
                     depth[(zp >> 6u) * 64u + (zp & 63u)], static_cast<unsigned>(depth[(zp >> 6u) * 64u + (zp & 63u)] * 16777216.0f));
        releaseTexture(a->tex);
        releaseTexture(z->tex);
        releaseTexture(b->tex);
        forgetBindings();
        selfTestZ16();
    }

    // GT3_GS_GL_SELFTEST=1, PSMZ16 (conversion luts 7-10): CT32 -> Z16 -> CT32 and CT16 -> Z16 -> CT16 round trips,
    // and the Z16 depth against GSMem::ReadZ16 of the same memory.
    void selfTestZ16()
    {
        if (!z16TablesOk)
        {
            std::fprintf(stderr, "[gs-gl] selftest Z16: tables incomplete, not run\n");
            return;
        }
        auto makeTarget = [this](bool depth, Layout layout) {
            auto t = std::make_unique<Target>();
            t->depth = depth;
            t->layout = layout;
            t->fbp = 0;
            t->fbw = 1;
            t->width = 64;
            t->pageHeight = layout == Layout::C32 ? 32 : 64;
            t->height = t->logicalHeight = t->pageHeight;
            t->sx = scaleX;
            t->sy = scaleY;
            t->tex = makeTargetTexture(depth, 64, t->height);
            return t;
        };
        auto a32 = makeTarget(false, Layout::C32), b32 = makeTarget(false, Layout::C32), z = makeTarget(true, Layout::Z16);
        auto a16 = makeTarget(false, Layout::C16), b16 = makeTarget(false, Layout::C16);
        std::vector<uint8_t> mem(64u * 1024u, 0u); // page 0 as GS memory
        std::vector<uint32_t> src32(64 * 32), out32(64 * 32), src16(64 * 64), out16(64 * 64);
        std::vector<float> depth(64 * 64);
        for (uint32_t i = 0; i < src32.size(); ++i)
            src32[i] = (i * 2654435761u) ^ (i << 9) ^ 0x8000u;
        for (uint32_t y = 0; y < 32u; ++y)
            for (uint32_t x = 0; x < 64u; ++x)
                GSMem::WriteCT32(mem.data(), 0u, 1u, x, y, src32[y * 64u + x]);
        glBindTexture(GL_TEXTURE_2D, a32->tex);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 64, 32, GL_RGBA, GL_UNSIGNED_BYTE, src32.data());
        glBindTexture(GL_TEXTURE_2D, 0);
        convertTex(a32->tex, *z, 0, 0, 0, 0, 1, 7, 15);
        convertTex(z->tex, *b32, 0, 0, 0, 0, 0, 8, 15);
        glBindTexture(GL_TEXTURE_2D, b32->tex);
        glGetTexImage(GL_TEXTURE_2D, 0, GL_RGBA, GL_UNSIGNED_BYTE, out32.data());
        glBindTexture(GL_TEXTURE_2D, z->tex);
        glGetTexImage(GL_TEXTURE_2D, 0, GL_DEPTH_COMPONENT, GL_FLOAT, depth.data());
        glBindTexture(GL_TEXTURE_2D, 0);
        int bad32 = 0, badZ = 0;
        for (uint32_t i = 0; i < out32.size(); ++i)
            bad32 += out32[i] != src32[i] ? 1 : 0;
        for (uint32_t y = 0; y < 64u; ++y)
            for (uint32_t x = 0; x < 64u; ++x)
            {
                const uint32_t want = GSMem::ReadZ16(mem.data(), 0u, 1u, x, y) & 0xFFFFu;
                const uint32_t got = static_cast<uint32_t>(std::llround(static_cast<double>(depth[y * 64u + x]) * 16777216.0));
                if (got != want && badZ++ < 4)
                    std::fprintf(stderr, "[gs-gl] selftest C32->Z16 pixel %u,%u: depth %u want %u\n", x, y, got, want);
            }
        // CT16 -> Z16 -> CT16: texels 5551 in the top bits of each byte, as a C16 target holds them.
        for (uint32_t i = 0; i < src16.size(); ++i)
        {
            const uint32_t h = (i * 40503u) & 0xFFFFu;
            src16[i] = ((h & 31u) << 3) | (((h >> 5) & 31u) << 11) | (((h >> 10) & 31u) << 19) | (((h >> 15) & 1u) << 31);
        }
        glBindTexture(GL_TEXTURE_2D, a16->tex);
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 64, 64, GL_RGBA, GL_UNSIGNED_BYTE, src16.data());
        glBindTexture(GL_TEXTURE_2D, 0);
        convertTex(a16->tex, *z, 0, 0, 0, 0, 1, 9, 15);
        convertTex(z->tex, *b16, 0, 0, 0, 0, 0, 10, 15);
        glBindTexture(GL_TEXTURE_2D, b16->tex);
        glGetTexImage(GL_TEXTURE_2D, 0, GL_RGBA, GL_UNSIGNED_BYTE, out16.data());
        glBindTexture(GL_TEXTURE_2D, 0);
        int bad16 = 0;
        for (uint32_t i = 0; i < out16.size(); ++i)
            bad16 += out16[i] != src16[i] ? 1 : 0;
        std::fprintf(stderr, "[gs-gl] selftest Z16: C32->Z16->C32 %d of 2048 words differ, Z16 depth vs ReadZ16 %d of 4096, C16->Z16->C16 %d of 4096\n",
                     bad32, badZ, bad16);
        for (auto *t : {a32.get(), b32.get(), z.get(), a16.get(), b16.get()})
            releaseTexture(t->tex);
        forgetBindings();
    }

    // The page swizzles, found with the GSMem accessors on a scratch page:
    // byte offsets through byte-wise patterns, CT32 words through word indices.
    void buildViewTables()
    {
        std::vector<uint8_t> mem(4u << 20u, 0u);
        uint16_t pixelOfWord[2048];
        std::fill(std::begin(pixelOfWord), std::end(pixelOfWord), static_cast<uint16_t>(0xFFFFu));
        for (uint32_t w = 0; w < 2048u; ++w)
            std::memcpy(&mem[w * 4u], &w, 4);
        for (uint32_t y = 0; y < 32u; ++y)
            for (uint32_t x = 0; x < 64u; ++x)
                pixelOfWord[GSMem::ReadCT32(mem.data(), 0u, 1u, x, y) & 2047u] = static_cast<uint16_t>(x | (y << 6u));

        std::vector<uint32_t> byteOffset8(128u * 64u, 0u), byteOffset4(128u * 128u, 0u);
        for (uint32_t shift = 0; shift < 16u; shift += 4u)
        {
            // Each byte holds a 4-bit slice of its own offset in both nibbles.
            for (uint32_t i = 0; i < 8192u; ++i)
                mem[i] = static_cast<uint8_t>(((i >> shift) & 0xFu) * 0x11u);
            for (uint32_t v = 0; v < 64u; ++v)
                for (uint32_t u = 0; u < 128u; ++u)
                    byteOffset8[v * 128u + u] |= (GSMem::ReadP8(mem.data(), 0u, 2u, u, v) & 0xFu) << shift;
            for (uint32_t v = 0; v < 128u; ++v)
                for (uint32_t u = 0; u < 128u; ++u)
                    byteOffset4[v * 128u + u] |= (GSMem::ReadP4(mem.data(), 0u, 2u, u, v) & 0xFu) << shift;
        }
        std::vector<uint16_t> table8(128u * 64u), table4(128u * 128u);
        for (uint32_t i = 0; i < 128u * 64u; ++i)
        {
            const uint32_t offset = byteOffset8[i] & 8191u;
            table8[i] = static_cast<uint16_t>(pixelOfWord[offset >> 2u] | ((offset & 3u) << 11u));
        }
        for (uint32_t i = 0; i < 8192u; ++i)
            mem[i] = 0x10u; // low nibble 0, high nibble 1
        for (uint32_t v = 0; v < 128u; ++v)
            for (uint32_t u = 0; u < 128u; ++u)
            {
                const uint32_t i = v * 128u + u;
                const uint32_t offset = byteOffset4[i] & 8191u;
                const uint32_t nibble = GSMem::ReadP4(mem.data(), 0u, 2u, u, v) & 1u;
                table4[i] = static_cast<uint16_t>(pixelOfWord[offset >> 2u] | ((offset & 3u) << 11u) | (nibble << 13u));
            }
        auto upload = [](const std::vector<uint16_t> &data, int w, int h) {
            GLuint tex = 0;
            glGenTextures(1, &tex);
            glBindTexture(GL_TEXTURE_2D, tex);
            glTexStorage2D(GL_TEXTURE_2D, 1, GL_R16UI, w, h);
            glPixelStorei(GL_UNPACK_ALIGNMENT, 2);
            glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, w, h, GL_RED_INTEGER, GL_UNSIGNED_SHORT, data.data());
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
            glBindTexture(GL_TEXTURE_2D, 0);
            return tex;
        };
        lut8 = upload(table8, 128, 64);
        lut4 = upload(table4, 128, 128);

        // PSMCT32 <-> PSMZ32: the same memory word at different pixels.
        uint16_t zPixelOfWord[2048];
        std::fill(std::begin(zPixelOfWord), std::end(zPixelOfWord), static_cast<uint16_t>(0xFFFFu));
        for (uint32_t w = 0; w < 2048u; ++w)
            std::memcpy(&mem[w * 4u], &w, 4);
        for (uint32_t y = 0; y < 32u; ++y)
            for (uint32_t x = 0; x < 64u; ++x)
            {
                const uint32_t word = GSMem::ReadZ32(mem.data(), 0u, 1u, x, y);
                if (word < 2048u)
                    zPixelOfWord[word] = static_cast<uint16_t>(x | (y << 6u));
            }
        std::vector<uint16_t> zFromC(64u * 32u, 0u), cFromZ(64u * 32u, 0u);
        int missing = 0;
        for (uint32_t w = 0; w < 2048u; ++w)
        {
            const uint16_t c = pixelOfWord[w], z = zPixelOfWord[w];
            if (c == 0xFFFFu || z == 0xFFFFu)
            {
                ++missing;
                continue;
            }
            zFromC[(z >> 6u) * 64u + (z & 63u)] = c; // Z32 pixel -> its CT32 pixel
            cFromZ[(c >> 6u) * 64u + (c & 63u)] = z; // CT32 pixel -> its Z32 pixel
        }
        layoutTablesOk = missing == 0;
        if (!layoutTablesOk)
            std::cerr << "[gs-gl] CT32/Z32 page tables incomplete (" << missing << " words); Z/colour page moves use the CPU copy" << std::endl;
        lutZfromC = upload(zFromC, 64, 32);
        lutCfromZ = upload(cFromZ, 64, 32);

        // PSMCT16 <-> PSMCT32: halfword indices read back through ReadCT16.
        for (uint32_t h = 0; h < 4096u; ++h)
        {
            const uint16_t v = static_cast<uint16_t>(h);
            std::memcpy(&mem[h * 2u], &v, 2);
        }
        std::vector<uint16_t> t16from32(64u * 64u, 0u);
        std::vector<uint16_t> t32from16(64u * 32u * 2u, 0u);
        std::vector<uint8_t> halfSeen(4096u, 0u);
        int missing16 = 0;
        for (uint32_t y = 0; y < 64u; ++y)
            for (uint32_t x = 0; x < 64u; ++x)
            {
                const uint32_t half = GSMem::ReadCT16(mem.data(), 0u, 1u, x, y) & 4095u;
                const uint16_t c = pixelOfWord[half >> 1u];
                if (c == 0xFFFFu || halfSeen[half])
                {
                    ++missing16;
                    continue;
                }
                halfSeen[half] = 1u;
                t16from32[y * 64u + x] = static_cast<uint16_t>(c | ((half & 1u) << 11u));
                t32from16[((c >> 6u) * 64u + (c & 63u)) * 2u + (half & 1u)] = static_cast<uint16_t>(x | (y << 6u));
            }
        c16TablesOk = layoutTablesOk && missing16 == 0;
        std::copy(std::begin(pixelOfWord), std::end(pixelOfWord), pixelOfWordC);
        {
            std::vector<uint16_t> addr(128u * 352u, 0u);
            for (uint32_t y = 0; y < 128u; ++y)
                for (uint32_t x = 0; x < 128u; ++x)
                    addr[y * 128u + x] = static_cast<uint16_t>(GSMem::PageTableP4[0][y][x]);
            for (uint32_t y = 0; y < 64u; ++y)
                for (uint32_t x = 0; x < 128u; ++x)
                    addr[(128u + y) * 128u + x] = static_cast<uint16_t>(GSMem::PageTableP8[0][y][x]);
            for (uint32_t y = 0; y < 32u; ++y)
                for (uint32_t x = 0; x < 64u; ++x)
                    addr[(192u + y) * 128u + x] = static_cast<uint16_t>(GSMem::PageTableC32[0][y][x]);
            for (uint32_t y = 0; y < 64u; ++y)
                for (uint32_t x = 0; x < 64u; ++x)
                {
                    addr[(224u + y) * 128u + x] = static_cast<uint16_t>(GSMem::PageTableC16[0][y][x]);
                    addr[(288u + y) * 128u + x] = static_cast<uint16_t>(GSMem::PageTableC16S[0][y][x]);
                }
            addrLut = upload(addr, 128, 352);
            glActiveTexture(GL_TEXTURE8);
            glBindTexture(GL_TEXTURE_2D, addrLut);
            glActiveTexture(GL_TEXTURE0);
            rawTextures = envFlag("GT3_GS_GL_RAW_TEXTURES", true);
            if (rawTextures && envFlag("GT3_GS_GL_GPU_DECODE", false))
                buildDecodeProgram();
            // Used by default from 16 samples per pixel (applyScalePolicies).
            if (rawTextures && hasBindless && envFlag("GT3_GS_GL_FAST_TEXTURES", true))
            {
                buildColourProgram();
                applyScalePolicies(); // (first applied before this program existed)
            }
        }
        std::copy(std::begin(zPixelOfWord), std::end(zPixelOfWord), pixelOfWordZ);
        for (uint32_t w = 0; w < 2048u; ++w)
        {
            if (pixelOfWordC[w] < 2048u)
                wordOfPixelC[pixelOfWordC[w]] = static_cast<uint16_t>(w);
            if (pixelOfWordZ[w] < 2048u)
                wordOfPixelZ[pixelOfWordZ[w]] = static_cast<uint16_t>(w);
        }
        for (int i = 0; i < 8; ++i)
        {
            patchTex[i] = makeColorTexture(64, 32);
            glGenTextures(1, &patchMaskTex[i]);
            glBindTexture(GL_TEXTURE_2D, patchMaskTex[i]);
            glTexStorage2D(GL_TEXTURE_2D, 1, GL_R8UI, 64, 32);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
            glGenTextures(1, &patchBitsTex[i]);
            glBindTexture(GL_TEXTURE_2D, patchBitsTex[i]);
            glTexStorage2D(GL_TEXTURE_2D, 1, GL_R32UI, 64, 32);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
            glBindTexture(GL_TEXTURE_2D, 0);
        }
        if (!c16TablesOk)
            std::cerr << "[gs-gl] CT16/CT32 page tables incomplete (" << missing16 << " halves); 16/32-bit page moves use the CPU copy" << std::endl;
        lut16from32 = upload(t16from32, 64, 64);
        {
            glGenTextures(1, &lut32from16);
            glBindTexture(GL_TEXTURE_2D, lut32from16);
            glTexStorage2D(GL_TEXTURE_2D, 1, GL_RG16UI, 64, 32);
            glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
            glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 64, 32, GL_RG_INTEGER, GL_UNSIGNED_SHORT, t32from16.data());
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
            glBindTexture(GL_TEXTURE_2D, 0);
        }

        // PSMZ16 <-> PSMCT32 / PSMCT16 (conversion luts 7-10): the same halfword-indexed scratch page (still in mem)
        // read through ReadZ16 and ReadCT16.
        {
            std::vector<uint16_t> c16PixelOfHalf(4096u, 0xFFFFu);
            for (uint32_t y = 0; y < 64u; ++y)
                for (uint32_t x = 0; x < 64u; ++x)
                    c16PixelOfHalf[GSMem::ReadCT16(mem.data(), 0u, 1u, x, y) & 4095u] = static_cast<uint16_t>(x | (y << 6u));
            std::vector<uint16_t> tZ16from32(64u * 64u, 0u), t32fromZ16(64u * 32u * 2u, 0u), tC16ofZ16(64u * 64u, 0u), tZ16ofC16(64u * 64u, 0u);
            std::vector<uint8_t> zSeen(4096u, 0u);
            int missingZ16 = 0;
            for (uint32_t y = 0; y < 64u; ++y)
                for (uint32_t x = 0; x < 64u; ++x)
                {
                    const uint32_t half = GSMem::ReadZ16(mem.data(), 0u, 1u, x, y) & 4095u;
                    const uint16_t c = pixelOfWordC[half >> 1u], cp = c16PixelOfHalf[half];
                    if (c == 0xFFFFu || cp == 0xFFFFu || zSeen[half])
                    {
                        ++missingZ16;
                        continue;
                    }
                    zSeen[half] = 1u;
                    const uint16_t zp = static_cast<uint16_t>(x | (y << 6u));
                    tZ16from32[y * 64u + x] = static_cast<uint16_t>(c | ((half & 1u) << 11u));
                    t32fromZ16[((c >> 6u) * 64u + (c & 63u)) * 2u + (half & 1u)] = zp;
                    tC16ofZ16[y * 64u + x] = cp;
                    tZ16ofC16[(cp >> 6u) * 64u + (cp & 63u)] = zp;
                }
            z16TablesOk = c16TablesOk && missingZ16 == 0;
            if (!z16TablesOk)
                std::cerr << "[gs-gl] Z16 page tables incomplete (" << missingZ16 << " halves); Z16 page moves use the CPU copy" << std::endl;
            lutZ16from32 = upload(tZ16from32, 64, 64);
            lutC16ofZ16 = upload(tC16ofZ16, 64, 64);
            lutZ16ofC16 = upload(tZ16ofC16, 64, 64);
            glGenTextures(1, &lut32fromZ16);
            glBindTexture(GL_TEXTURE_2D, lut32fromZ16);
            glTexStorage2D(GL_TEXTURE_2D, 1, GL_RG16UI, 64, 32);
            glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
            glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 64, 32, GL_RG_INTEGER, GL_UNSIGNED_SHORT, t32fromZ16.data());
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
            glBindTexture(GL_TEXTURE_2D, 0);
        }
    }

    static GLuint makeColorTexture(int width, int height)
    {
        GLuint tex = 0;
        ++statTexMade;
        glGenTextures(1, &tex);
        glBindTexture(GL_TEXTURE_2D, tex);
        glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA8, width, height);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glBindTexture(GL_TEXTURE_2D, 0);
        return tex;
    }

    // ------------------------------------------------------------ targets

    static GLuint makeTargetTexture(bool depth, int width, int height)
    {
        GLuint tex = 0;
        ++statTexMade;
        glGenTextures(1, &tex);
        glBindTexture(GL_TEXTURE_2D, tex);
        glTexStorage2D(GL_TEXTURE_2D, 1, depth ? GL_DEPTH_COMPONENT32F : GL_RGBA8, width, height);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glBindTexture(GL_TEXTURE_2D, 0);
        return tex;
    }

    // glCopyImageSubData of GS pixels (x, y, w, h) between two textures held at
    // the target scale (targets, the field and scratch copies).
    void copyScaled(GLuint src, int x, int y, GLuint dst, int dx, int dy, int w, int h, bool depth = false)
    {
        if (!texScales.empty() || !targetTexs.empty())
        {
            // Effects at their own scale: each side at its own scale. A texture
            // that is not a target (scratch, CLUT, field copies) takes the
            // source's; between two scales a nearest blit (depth textures too:
            // GT4's full-height car view has depth targets of its own scale).
            const TexScale s = scaleOfTex(src);
            TexScale d = scaleOfTex(dst);
            if (!targetTexs.count(dst))
            {
                // (A CLUT ring texture is made at the picture's scale: a CLUT from a full-height car view target is
                // copied to it at that scale.)
                if ((s.x > scaleX || s.y > scaleY) && std::find(clutRing.begin(), clutRing.end(), dst) != clutRing.end())
                    d = pictureScale();
                else
                    d = s;
                setTexScale(dst, d);
            }
            if (s != d)
            {
                beforeTextureWrite(dst);
                blitTex(depth, src, x * s.x, y * s.y, (x + w) * s.x, (y + h) * s.y, dst, dx * d.x, dy * d.y, (dx + w) * d.x, (dy + h) * d.y);
                return;
            }
            beforeTextureWrite(dst);
            glCopyImageSubData(src, GL_TEXTURE_2D, 0, x * s.x, y * s.y, 0, dst, GL_TEXTURE_2D, 0, dx * s.x, dy * s.y, 0, w * s.x, h * s.y, 1);
            return;
        }
        beforeTextureWrite(dst);
        glCopyImageSubData(src, GL_TEXTURE_2D, 0, x * scaleX, y * scaleY, 0, dst, GL_TEXTURE_2D, 0, dx * scaleX, dy * scaleY, 0, w * scaleX,
                           h * scaleY, 1);
    }

    // texScratch holds at least w x h samples (grown only at a scale; the
    // caller has flushed the batch that may sample it).
    void ensureTexScratch(int w, int h)
    {
        if (w <= texScratchW && h <= texScratchH)
            return;
        releaseTexture(texScratch);
        texScratchW = std::min(std::max(texScratchW, (w + 255) & ~255), static_cast<int>(maxTextureSize));
        texScratchH = std::min(std::max(texScratchH, (h + 255) & ~255), static_cast<int>(maxTextureSize));
        texScratch = makeColorTexture(texScratchW, texScratchH);
    }

    GLuint nativeScratch(bool depth)
    {
        GLuint &tex = depth ? nativeDepth : nativeColor;
        if (!tex)
            tex = makeTargetTexture(depth, 64, 64);
        return tex;
    }

    // Attaches a colour or depth texture to the framebuffer bound at target
    // (the other attachment cleared).
    static void attachOnly(GLenum target, bool depth, GLuint tex)
    {
        glFramebufferTexture2D(target, depth ? GL_COLOR_ATTACHMENT0 : GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, 0, 0);
        glFramebufferTexture2D(target, depth ? GL_DEPTH_ATTACHMENT : GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
    }

    // Nearest blit of (sx0, sy0)-(sx1, sy1) of src to (dx0, dy0)-(dx1, dy1) of
    // dst, both colour or both depth. Shrinking picks the sample nearest each
    // destination pixel's centre; enlarging repeats each source pixel.
    void blitTex(bool depth, GLuint src, int sx0, int sy0, int sx1, int sy1, GLuint dst, int dx0, int dy0, int dx1, int dy1,
                 GLenum filter = GL_NEAREST)
    {
        if (stableTargets)
        {
            // Both made before either is bound.
            const GLuint readFb = soloFboFor(src, depth), drawFb = soloFboFor(dst, depth);
            glBindFramebuffer(GL_READ_FRAMEBUFFER, readFb);
            glBindFramebuffer(GL_DRAW_FRAMEBUFFER, drawFb);
            curDrawFbo = 0xFFFFFFFFu;
            onInterlockFbo = false;
        }
        else
        {
            if (!blitFbo[0])
                glGenFramebuffers(2, blitFbo);
            glBindFramebuffer(GL_READ_FRAMEBUFFER, blitFbo[0]);
            noteAttach(src, false);
            attachOnly(GL_READ_FRAMEBUFFER, depth, src);
            glReadBuffer(depth ? GL_NONE : GL_COLOR_ATTACHMENT0);
            glBindFramebuffer(GL_DRAW_FRAMEBUFFER, blitFbo[1]);
            noteAttach(dst, false);
            attachOnly(GL_DRAW_FRAMEBUFFER, depth, dst);
            glDrawBuffer(depth ? GL_NONE : GL_COLOR_ATTACHMENT0);
        }
        glDisable(GL_SCISSOR_TEST);
        glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
        glDepthMask(GL_TRUE);
        glBlitFramebuffer(sx0, sy0, sx1, sy1, dx0, dy0, dx1, dy1, depth ? GL_DEPTH_BUFFER_BIT : GL_COLOR_BUFFER_BIT, depth ? GL_NEAREST : filter);
        glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
        glStateDirty = true;
    }

    // Reads GS pixels (x, y, w, h), at most 64 x 64, of a target texture: one
    // sample of each at a scale.
    void readTargetPixels(bool depth, GLuint tex, int x, int y, int w, int h, GLenum format, GLenum type, void *out)
    {
        const TexScale texSc = scaleOfTex(tex); // (effects at their own scale: the target's)
        if (scaled())
        {
            const GLuint scratch = nativeScratch(depth);
            blitTex(depth, tex, x * texSc.x, y * texSc.y, (x + w) * texSc.x, (y + h) * texSc.y, scratch, 0, 0, w, h);
            tex = scratch;
            x = y = 0;
        }
        if (stableTargets)
        {
            bindSoloFbo(GL_READ_FRAMEBUFFER, tex, depth);
            glReadPixels(x, y, w, h, format, type, out);
            glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
            return;
        }
        glBindFramebuffer(GL_READ_FRAMEBUFFER, readFbo);
        noteAttach(tex, false);
        attachOnly(GL_READ_FRAMEBUFFER, depth, tex);
        if (!depth)
            glReadBuffer(GL_COLOR_ATTACHMENT0);
        glReadPixels(x, y, w, h, format, type, out);
        glFramebufferTexture2D(GL_READ_FRAMEBUFFER, depth ? GL_DEPTH_ATTACHMENT : GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, 0, 0);
        glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
    }

    // Writes GS pixels (x, y, w, h) of a target texture (4 bytes each: RGBA8
    // or float depth): at a scale every sample of each, the pixels repeated
    // on the CPU and uploaded at once (a blit from a native copy per page
    // made the driver submit work each time: a third of the GS GL thread).
    std::vector<uint32_t> upsampleBuf;
    std::vector<GLuint> upsampleColorRing, upsampleDepthRing; // GS_GPU_UPSAMPLE
    size_t upsampleColorNext = 0, upsampleDepthNext = 0;
    void writeTargetPixels(bool depth, GLuint tex, int x, int y, int w, int h, GLenum format, GLenum type, const void *data)
    {
        const TexScale texSc = scaleOfTex(tex); // (effects at their own scale: the target's)
        (void)depth;
        beforeTextureWrite(tex);
        glBindTexture(GL_TEXTURE_2D, tex);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
        if (!scaled())
        {
            glTexSubImage2D(GL_TEXTURE_2D, 0, x, y, w, h, format, type, data);
        }
        else if (GS_GPU_UPSAMPLE && w <= 64 && h <= 64)
        {
            // The pixels at their own size into one of a ring of 64x64 native
            // textures, repeated into the target by a nearest blit on the GPU
            // (Vulkan: a copy command, 16-32 times less data than the pixels
            // repeated on the CPU; OpenGL drivers made each blit costly).
            std::vector<GLuint> &ring = depth ? upsampleDepthRing : upsampleColorRing;
            if (ring.empty())
                for (int i = 0; i < 16; ++i)
                    ring.push_back(makeTargetTexture(depth, 64, 64));
            size_t &next = depth ? upsampleDepthNext : upsampleColorNext;
            const GLuint native = ring[next];
            next = (next + 1u) % ring.size();
            glBindTexture(GL_TEXTURE_2D, native);
            glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, w, h, format, type, data);
            glBindTexture(GL_TEXTURE_2D, 0);
            blitTex(depth, native, 0, 0, w, h, tex, x * texSc.x, y * texSc.y, (x + w) * texSc.x, (y + h) * texSc.y);
            return;
        }
        else
        {
            const int sw = w * texSc.x, sh = h * texSc.y;
            upsampleBuf.resize(static_cast<size_t>(sw) * static_cast<size_t>(sh));
            const uint32_t *src = static_cast<const uint32_t *>(data);
            for (int j = 0; j < h; ++j)
            {
                uint32_t *row = upsampleBuf.data() + static_cast<size_t>(j) * static_cast<size_t>(texSc.y) * static_cast<size_t>(sw);
                for (int i = 0; i < w; ++i)
                    std::fill_n(row + static_cast<size_t>(i) * static_cast<size_t>(texSc.x), texSc.x, src[static_cast<size_t>(j) * static_cast<size_t>(w) + static_cast<size_t>(i)]);
                for (int r = 1; r < texSc.y; ++r)
                    std::memcpy(row + static_cast<size_t>(r) * static_cast<size_t>(sw), row, static_cast<size_t>(sw) * sizeof(uint32_t));
            }
            glTexSubImage2D(GL_TEXTURE_2D, 0, x * texSc.x, y * texSc.y, sw, sh, format, type, upsampleBuf.data());
        }
        glBindTexture(GL_TEXTURE_2D, 0);
    }

    // The target for a GS buffer, at least rows tall (-1 if it cannot be).
    int findTarget(bool depth, Layout layout, uint32_t fbp, uint32_t fbw, int rows, bool tall = false)
    {
        fbw = std::max(fbw, 1u);
        tall = tall && tallOn;
        const uint64_t indexKey = targetIndexKey(depth, layout, fbp, fbw, tall);
        int id = -1;
        int freeSlot = -1;
        // Consecutive primitives nearly always draw into the same targets: the
        // last one found (per colour/depth) before the index (checked, since a
        // target can be dropped in between).
        int *const recent = lastFound[depth ? 1 : 0];
        uint64_t *const recentKey = lastFoundKey[depth ? 1 : 0];
        for (int r = 0; r < kFoundRecent && id < 0; ++r)
        {
            const int last = recent[r];
            if (last >= 0 && static_cast<size_t>(last) < targets.size() && targets[last] && recentKey[r] == indexKey &&
                targets[last]->depth == depth && targets[last]->layout == layout && (targets[last]->fbp & 0x1FFu) == (fbp & 0x1FFu) &&
                targets[last]->fbw == fbw && targets[last]->tall == tall)
                id = last;
        }
        if (id < 0)
            if (auto found = targetIndex.find(indexKey); found != targetIndex.end())
            {
                // (The slot must still hold this buffer's target: a slot is reused once its target is dropped.)
                const Target *t = targets[found->second].get();
                if (t && targetIndexKey(*t) == indexKey)
                    id = found->second;
                else
                    targetIndex.erase(found);
            }
        if (id < 0)
            for (size_t i = 0; i < targets.size(); ++i)
                if (!targets[i])
                {
                    freeSlot = static_cast<int>(i);
                    break;
                }
        if (id < 0)
        {
            auto target = std::make_unique<Target>();
            target->depth = depth;
            target->layout = layout;
            target->fbp = fbp;
            target->fbw = fbw;
            target->uid = ++nextTargetUid;
            target->pageHeight = (layout == Layout::C16 || layout == Layout::Z16) ? 64 : 32;
            target->width = static_cast<int>(fbw * 64u);
            // Effects at their own scale: a colour target not known as a scene target (sceneFbps) starts at the
            // effects scale; a draw with a depth buffer or the display promotes it (promoteTarget).
            const bool known = sceneFbps.count((fbp & (kPageCount - 1u)) | (fbw << 16)) != 0;
            // (DLSS with the HUD after it: every depth target at the 3D's scale, even where a shown buffer is)
            const bool zLayout = layout == Layout::Z32 || layout == Layout::Z16;
            const bool effect = effectScale && (dlssHudRender ? (depth || zLayout || !known) : (!depth && !zLayout && !known));
            target->sx = effect ? effectScale : scaleX;
            target->sy = effect ? effectScale * (scaleY / std::max(scaleX, 1)) : scaleY;
            if (tall)
            {
                // GT4's car view at full height (tallOn): twice the picture's samples down each GS pixel.
                target->tall = true;
                target->sx = scaleX;
                target->sy = 2 * scaleY;
                if (statTallTargets++ < 16)
                    std::fprintf(stderr, "[gs-gl] GT4's car view at full height: %s target %03x/%u (%s) at %d x %d samples per GS pixel\n",
                                 depth ? "depth" : "colour", fbp & 0x1FFu, fbw,
                                 layout == Layout::Z16 ? "Z16" : layout == Layout::Z32 ? "Z32" : layout == Layout::C16 ? "C16" : "C32", target->sx, target->sy);
            }
            if (freeSlot >= 0)
            {
                targets[freeSlot] = std::move(target);
                id = freeSlot;
            }
            else
            {
                targets.push_back(std::move(target));
                id = static_cast<int>(targets.size() - 1u);
            }
            targetIndex[indexKey] = id;
            ++statTargetsMade;
            resetRuns(); // (step 7 T1: a new target, maybe in a dropped one's slot)
        }
        if (recent[0] != id || recentKey[0] != indexKey)
        {
            int r = 1;
            while (r < kFoundRecent - 1 && !(recent[r] == id && recentKey[r] == indexKey))
                ++r;
            for (; r > 0; --r)
            {
                recent[r] = recent[r - 1];
                recentKey[r] = recentKey[r - 1];
            }
            recent[0] = id;
            recentKey[0] = indexKey;
        }
        Target &t = *targets[id];
        t.lastUse = frameCounter;
        if (rows > t.maxHeight())
            return -1;
        if (rows > t.logicalHeight)
            t.logicalHeight = std::min(t.maxHeight(), ((rows + t.pageHeight - 1) / t.pageHeight) * t.pageHeight + t.pageHeight * 2);
        if (rows > t.height)
        {
            // Grow (in whole page rows); the rows below keep their contents.
            if (haveBatch && (key.colorTarget == id || key.depthTarget == id || batchUses(t.tex)))
                flushBatch();
            int newHeight = std::min(t.maxHeight(), ((rows + t.pageHeight - 1) / t.pageHeight) * t.pageHeight + t.pageHeight * 2);
            if (stableTargets >= 2 && !scaled())
                newHeight = t.maxHeight(); // P2 (opt-in): at its full height at once (at most ~4 MB at native)
            const GLuint tex = acquireTargetTexture(depth, t.width * t.sx, newHeight * t.sy);
            if (scalesListed())
            {
                targetTexs.insert(tex);
                setTexScale(tex, {t.sx, t.sy});
            }
            if (t.tex)
            {
                ++statTargetGrows;
                copyScaled(t.tex, 0, 0, tex, 0, 0, t.width, t.height, depth);
                forgetTargetTex(t.tex);
                recycleTargetTexture(t.tex, depth, t.width * t.sx, t.height * t.sy);
            }
            t.tex = tex;
            t.height = newHeight;
            touchResolve(); // (step 7 T1)
        }
        return id;
    }

    void promoteTargetImpl(int id, const char *why)
    {
        Target &t = *targets[id];
        if (t.tall)
            return; // (GT4's car view at full height stays at its own scale)
        sceneFbps.insert((t.fbp & (kPageCount - 1u)) | (t.fbw << 16));
        if (t.sx == scaleX && t.sy == scaleY)
            return;
        if (haveBatch && (key.colorTarget == id || key.depthTarget == id || batchUses(t.tex)))
            flushBatch();
        flushMoves();
        const int oldSx = t.sx, oldSy = t.sy;
        const GLuint tex = acquireTargetTexture(t.depth, t.width * scaleX, t.height * scaleY);
        targetTexs.insert(tex);
        setTexScale(tex, pictureScale());
        if (t.tex)
        {
            blitTex(false, t.tex, 0, 0, t.width * oldSx, t.height * oldSy, tex, 0, 0, t.width * scaleX, t.height * scaleY);
            forgetTargetTex(t.tex);
            recycleTargetTexture(t.tex, t.depth, t.width * oldSx, t.height * oldSy);
        }
        t.tex = tex;
        t.sx = scaleX;
        t.sy = scaleY;
        selfSnap.valid = false;
        touchResolve();
        resetRuns();
        if (statPromotions++ < 64)
            std::fprintf(stderr, "[gs-gl] effects scale: target %03x/%u promoted to the picture's scale (%dx -> %dx, %s)\n", t.fbp, t.fbw, oldSx, scaleX, why);
    }

    // GT4's race colour (gs_fullcolour.h): the CPU copy's full-colour plane, made when the race colour is 32
    // (null with 16: nothing changes then).
    uint32_t *fullColourPlane()
    {
        if (gsfc::mode() == 16)
            return nullptr;
        if (uint32_t *plane = cpu->FullColourPlane())
            return plane;
        return cpu->EnsureFullColourPlane();
    }

    // GT4's race colour: the video mode epoch of the last draw (GSDrawState::fcEpoch), and whether a full-colour
    // draw came since the last change (only then can a 16-bit target's texels hold more than their 16-bit value).
    uint16_t fcEpoch = 0;
    bool fcSeen = false;

    // A new video mode: every pixel's full colour is forgotten, as the CPU reference renderer forgets it
    // (GSCpuBackend::ForgetFullColour): the CPU copy's plane is cleared and every 16-bit target's texels are
    // reduced to their 16-bit value (every sample at a scale), so both renderers hold each pixel as v << 3 until a
    // full-colour draw writes it again. GS memory (the 16-bit values) is unchanged: no page changes hands.
    void forgetFullColour(uint16_t epoch)
    {
        fcEpoch = epoch;
        cpu->ForgetFullColour(epoch);
        if (!fcSeen)
            return;
        fcSeen = false;
        flushBatch();
        flushMoves();
        int masked = 0;
        for (auto &tp : targets)
            if (tp && !tp->depth && tp->layout == Layout::C16 && tp->tex)
            {
                Target &t = *tp;
                // A copy of the target, then each page drawn back from it as its 16-bit value (conversion mode 3).
                ensureTexScratch(t.width * t.sx, t.height * t.sy);
                copyScaled(t.tex, 0, 0, texScratch, 0, 0, t.width, t.height);
                for (int y = 0; y < t.height; y += t.pageHeight)
                    for (int x = 0; x < t.width; x += 64)
                        convertTex(texScratch, t, x, y, x, y, 3, 0, 15);
                ++masked;
            }
        static int logged = 0;
        if (logged < 8)
        {
            ++logged;
            std::fprintf(stderr, "[gs-colour] video mode changed (epoch %u): full colour forgotten, %d 16-bit target(s) reduced to 16 bits\n",
                         static_cast<unsigned>(epoch), masked);
        }
    }

    // GPU copy of a page -> the CPU copy.
    // keepOwner: a presentation peek, which copies the page into the CPU copy
    // for display without handing it over (the GPU target stays its owner).
    void downloadPage(uint32_t page, bool keepOwner = false)
    {
        if (pendingAny[page & (kPageCount - 1u)])
            applyPendingPage(page); // recorded upload words first: the copy back overwrites the CPU copy
        const int id = owner[page];
        if (id < 0)
            return;
        if (haveBatch && (id == key.colorTarget || id == key.depthTarget))
            flushBatch();
        ScopedNs timer(statNs[0]);
        Target &t = *targets[id];
        int x = 0, y = 0;
        if (!t.pageRect(page, x, y))
        {
            debugOwner(page, -1, "download (outside target)");
            owner[page] = -1;
            pageChanged(page);
            return;
        }
        const int w = 64, h = t.pageHeight;
        const uint32_t bp = t.fbp * 32u;
        if (t.depth && t.layout == Layout::Z16)
        {
            // PSMZ16: the page's 4096 halfwords are all Z (no high plane), through the page table as the CT16
            // upload reads them; depth is z / 2^24 exactly (the shader clamps Z16 draws to 0xFFFF).
            float depth[64 * 64];
            readTargetPixels(true, t.tex, x, y, 64, 64, GL_DEPTH_COMPONENT, GL_FLOAT, depth);
            uint16_t *const halves = reinterpret_cast<uint16_t *>(vram + static_cast<size_t>(page & (kPageCount - 1u)) * 8192u);
            const auto &table = GSMem::PageTableZ16[0];
            for (int j = 0; j < 64; ++j)
                for (int i = 0; i < 64; ++i)
                {
                    const long long z = std::llround(static_cast<double>(depth[j * 64 + i]) * 16777216.0);
                    halves[table[j][i]] = static_cast<uint16_t>(std::clamp<long long>(z, 0, 0xFFFF));
                }
            if (!keepOwner)
                highPlane[page] = -1;
        }
        else if (t.depth)
        {
            // A 16-bit colour target holding the high plane (it gave the page to
            // this Z buffer whole): its page first, as CT16, then Z replaces the
            // low 24 bits of each word (WriteZ24 keeps bits 24-31).
            const int holder16 = highPlane[page];
            int h16x = 0, h16y = 0;
            const bool high16 = holder16 >= 0 && targets[holder16] && !targets[holder16]->depth &&
                                targets[holder16]->layout == Layout::C16 && targets[holder16]->pageRect(page, h16x, h16y);
            if (high16)
            {
                const Target &hc = *targets[holder16];
                uint32_t halves[64 * 64];
                readTargetPixels(false, hc.tex, h16x, h16y, 64, 64, GL_RGBA, GL_UNSIGNED_BYTE, halves);
                uint32_t *const plane = fullColourPlane();
                const auto &table16 = GSMem::PageTableC16[0];
                for (int j = 0; j < 64; ++j)
                    for (int i = 0; i < 64; ++i)
                    {
                        const uint32_t c = halves[j * 64 + i];
                        GSMem::WriteCT16(vram, hc.fbp * 32u, hc.fbw, static_cast<uint32_t>(h16x + i), static_cast<uint32_t>(h16y + j),
                                         ((c >> 3) & 0x1Fu) | (((c >> 11) & 0x1Fu) << 5) | (((c >> 19) & 0x1Fu) << 10) | (((c >> 31) & 1u) << 15));
                        if (plane)
                            plane[(page & (kPageCount - 1u)) * 4096u + table16[j][i]] = c; // GT4's race colour: the texel's full colour
                    }
            }
            float depth[64 * 64];
            readTargetPixels(true, t.tex, x, y, w, h, GL_DEPTH_COMPONENT, GL_FLOAT, depth);
            for (int j = 0; j < h; ++j)
                for (int i = 0; i < w; ++i)
                {
                    // depth is z / 2^24 exactly; round in double (in float, + 0.5 moves odd z above 2^23).
                    const uint32_t z = static_cast<uint32_t>(std::llround(static_cast<double>(depth[j * w + i]) * 16777216.0));
                    GSMem::WriteZ24(vram, bp, t.fbw, static_cast<uint32_t>(x + i), static_cast<uint32_t>(y + j), std::min(z, 0xFFFFFFu));
                }
            const int holder = high16 ? -1 : highPlane[page];
            int hx = 0, hy = 0;
            if (holder >= 0 && targets[holder] && targets[holder]->pageRect(page, hx, hy))
            {
                // The high plane sits in that colour target's alpha.
                const Target &h = *targets[holder];
                uint32_t words[64 * 32];
                readTargetPixels(false, h.tex, hx, hy, 64, 32, GL_RGBA, GL_UNSIGNED_BYTE, words);
                const uint32_t hbp = h.fbp * 32u;
                for (int j = 0; j < 32; ++j)
                    for (int i = 0; i < 64; ++i)
                    {
                        const uint32_t px = static_cast<uint32_t>(hx + i), py = static_cast<uint32_t>(hy + j);
                        if (h.layout == Layout::Z32)
                        {
                            const uint32_t old = GSMem::ReadZ32(vram, hbp, h.fbw, px, py);
                            GSMem::WriteZ32(vram, hbp, h.fbw, px, py, (old & 0x00FFFFFFu) | (words[j * 64 + i] & 0xFF000000u));
                        }
                        else
                        {
                            GSMem::WriteP8H(vram, hbp, h.fbw, px, py, words[j * 64 + i] >> 24);
                        }
                    }
            }
            if (!keepOwner)
                highPlane[page] = -1;
        }
        else
        {
            uint32_t pixels[64 * 64];
            readTargetPixels(false, t.tex, x, y, w, h, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
            // GT4's race colour (gs_fullcolour.h): a 16-bit target's texels hold each pixel's full colour; the
            // CPU copy keeps them in its plane (the 16-bit value above them, as before).
            uint32_t *const plane = t.layout == Layout::C16 ? fullColourPlane() : nullptr;
            const auto &table16 = GSMem::PageTableC16[0];
            for (int j = 0; j < h; ++j)
                for (int i = 0; i < w; ++i)
                {
                    const uint32_t px = static_cast<uint32_t>(x + i), py = static_cast<uint32_t>(y + j);
                    const uint32_t c = pixels[j * w + i];
                    if (plane)
                        plane[(page & (kPageCount - 1u)) * 4096u + table16[j][i]] = c;
                    if (t.layout == Layout::C16)
                        GSMem::WriteCT16(vram, bp, t.fbw, px, py,
                                         ((c >> 3) & 0x1Fu) | (((c >> 11) & 0x1Fu) << 5) | (((c >> 19) & 0x1Fu) << 10) | (((c >> 31) & 1u) << 15));
                    else if (t.layout == Layout::Z32)
                        GSMem::WriteZ32(vram, bp, t.fbw, px, py, c);
                    else
                        GSMem::WriteCT32(vram, bp, t.fbw, px, py, c);
                }
        }
        ++statDownloads;
        if (stats)
            ++statDownloadCauses[cause];
        if (keepOwner)
        {
            peekSerial[page] = writeSerial[page];
            pageObserved(page);
            return;
        }
        debugOwner(page, -1, "download");
        owner[page] = -1;
        clearCpuValid(page);
        ++cpuVersion[page];
        pageChangedAt[page] = ++cpuVersionEpoch;
        pageChanged(page);
        t.version[page] = cpuVersion[page];
    }

    // CPU copy of a page -> a target's GPU copy (the page must be CPU owned).
    void uploadPage(Target &t, uint32_t page)
    {
        if (static_cast<int>(page) == debugPage)
            std::fprintf(stderr, "[gs-gl-debug] frame %llu page %03x uploaded from the CPU copy into %s %03x/%u (owner %d, cause %s)\n",
                         (unsigned long long)frameCounter, page, layoutName(t), t.fbp, t.fbw, owner[page], cause.c_str());
        ScopedNs timer(statNs[1]);
        int x = 0, y = 0;
        if (!t.pageRect(page, x, y))
            return;
        const int w = 64, h = t.pageHeight;
        // The page's 8 KB read through GSMem's page tables (buffers start on
        // a page, so a pixel's place in its page is table[0][y][x]), instead
        // of GSMem's per-pixel addressing (the ~70 Z32c page uploads a frame).
        const uint8_t *pageBytes = vram + static_cast<size_t>(page & (kPageCount - 1u)) * 8192u;
        if (t.depth && t.layout == Layout::Z16)
        {
            // PSMZ16: each pixel's halfword through the Z16 page table (64x64).
            float depth[64 * 64];
            const uint16_t *halves = reinterpret_cast<const uint16_t *>(pageBytes);
            const auto &table = GSMem::PageTableZ16[0];
            for (int j = 0; j < 64; ++j)
                for (int i = 0; i < 64; ++i)
                    depth[j * 64 + i] = static_cast<float>(halves[table[j][i]]) / 16777216.0f;
            writeTargetPixels(true, t.tex, x, y, 64, 64, GL_DEPTH_COMPONENT, GL_FLOAT, depth);
        }
        else if (t.depth)
        {
            float depth[64 * 64];
            const uint32_t *words = reinterpret_cast<const uint32_t *>(pageBytes);
            const auto &table = GSMem::PageTableZ32[0];
            for (int j = 0; j < h; ++j)
                for (int i = 0; i < w; ++i)
                    depth[j * w + i] = static_cast<float>(words[table[j][i]] & 0xFFFFFFu) / 16777216.0f;
            writeTargetPixels(true, t.tex, x, y, w, h, GL_DEPTH_COMPONENT, GL_FLOAT, depth);
        }
        else
        {
            uint32_t pixels[64 * 64];
            if (t.layout == Layout::C16)
            {
                const uint16_t *halves = reinterpret_cast<const uint16_t *>(pageBytes);
                const auto &table = GSMem::PageTableC16[0];
                // GT4's race colour (gs_fullcolour.h): each texel the pixel's full colour (its plane word while
                // that reduces to the 16-bit value, else v << 3, as before).
                const uint32_t *const plane = fullColourPlane();
                const uint32_t *const pagePlane = plane ? plane + static_cast<size_t>(page & (kPageCount - 1u)) * 4096u : nullptr;
                for (int j = 0; j < h; ++j)
                    for (int i = 0; i < w; ++i)
                        pixels[j * w + i] = pagePlane ? gsfc::full(pagePlane[table[j][i]], halves[table[j][i]]) : rgba5551To8888(halves[table[j][i]]);
            }
            else
            {
                const uint32_t *words = reinterpret_cast<const uint32_t *>(pageBytes);
                const auto &table = t.layout == Layout::Z32 ? GSMem::PageTableZ32[0] : GSMem::PageTableC32[0];
                for (int j = 0; j < h; ++j)
                    for (int i = 0; i < w; ++i)
                        pixels[j * w + i] = words[table[j][i]];
            }
            writeTargetPixels(false, t.tex, x, y, w, h, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
        }
        t.version[page] = cpuVersion[page];
        ++statUploads;
        if (stats)
        {
            char text[160];
            std::snprintf(text, sizeof(text), "%s %03x/%u page %03x (%s)", layoutName(t), t.fbp, t.fbw, page, cause.c_str());
            ++statUploadCauses[text];
        }
    }

    // Makes a target's copy of the pages current (downloading from another
    // owner first) and, when it will write them, makes it their owner.
    GLint convUniform(const char *name)
    {
        auto it = convUniforms.find(name);
        if (it != convUniforms.end())
            return it->second;
        const GLint loc = glGetUniformLocation(convProgram, name);
        convUniforms.emplace(name, loc);
        return loc;
    }

    // The conversion program's tables per u_lut: unit 6 (u_lutZfromC / u_lut16from32) and unit 7 (u_lutCfromZ /
    // u_lut32from16).
    GLuint convLut6(int lut) const
    {
        return lut == 3 ? lut16from32 : lut == 7 ? lutZ16from32 : lut == 9 ? lutC16ofZ16 : lutZfromC;
    }
    GLuint convLut7(int lut) const
    {
        return lut == 4 ? lut32from16 : lut == 8 ? lut32fromZ16 : lut == 10 ? lutZ16ofC16 : lutCfromZ;
    }

    // The conversion program's source scale (effects at their own scale): 0 0 when it is the destination's.
    void setConvSrcScale(GLuint srcTex, const Target &to)
    {
        const TexScale s = scaleOfTex(srcTex);
        if (s.x == to.sx && s.y == to.sy)
            glUniform2i(convUniform("u_srcScale"), 0, 0);
        else
            glUniform2i(convUniform("u_srcScale"), s.x, s.y);
    }

    // Copies/converts page `page` of target `from` into target `to` with the
    // conversion program (mode/lut as its shader). mask: colour write mask.
    void convertPage(Target &from, Target &to, int fx, int fy, int tx, int ty, int mode, int lut, bool rgbOnly)
    {
        queueConvert(from.tex, to, fx, fy, tx, ty, mode, lut, rgbOnly ? 7 : 15);
    }

    // Page conversions made while acquiring pages for a draw or a texture are
    // queued (deferMoves) and drawn together, one instanced draw per run of
    // moves with the same source, destination and conversion (flushMoves at
    // the end of the acquire). They write distinct pages, so the order among
    // them and the other page work of the acquire does not matter.
    struct MoveGroup
    {
        GLuint src = 0;
        Target *to = nullptr;
        int mode = 0, lut = 0, colorMask = 15;
        bool same(GLuint s, const Target *t, int m, int l, int c) const
        {
            return src == s && to == t && mode == m && lut == l && colorMask == c;
        }
    };
    MoveGroup moveGroup;
    std::vector<int32_t> pendingMoves;
    bool deferMoves = false;
    GLuint moveBuffer = 0;
    uint64_t statMoveDraws = 0;

    void queueConvert(GLuint srcTex, Target &to, int fx, int fy, int tx, int ty, int mode, int lut, int colorMask)
    {
        if (!deferMoves || noBatchMove)
        {
            convertTex(srcTex, to, fx, fy, tx, ty, mode, lut, colorMask);
            ++statMoveDraws;
            return;
        }
        if (!pendingMoves.empty() && !moveGroup.same(srcTex, &to, mode, lut, colorMask))
            flushMoves();
        moveGroup = {srcTex, &to, mode, lut, colorMask};
        pendingMoves.insert(pendingMoves.end(), {fx, fy, tx, ty});
    }

    void flushMoves()
    {
        if (pendingMoves.empty())
            return;
        const MoveGroup g = moveGroup;
        Target &to = *g.to;
        beforeTextureWrite(to.tex);
        const GLsizei count = static_cast<GLsizei>(pendingMoves.size() / 4u);
        if (!moveBuffer)
            glGenBuffers(1, &moveBuffer);
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, moveBuffer);
        glBufferData(GL_SHADER_STORAGE_BUFFER, static_cast<GLsizeiptr>(pendingMoves.size() * sizeof(int32_t)), pendingMoves.data(),
                     GL_STREAM_DRAW);
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 2, moveBuffer);
        pendingMoves.clear();
        if (stableTargets)
            bindSoloFbo(GL_FRAMEBUFFER, to.tex, to.depth);
        else
        {
            glBindFramebuffer(GL_FRAMEBUFFER, convFbo);
            noteAttach(to.tex);
        }
        if (to.depth)
        {
            if (!stableTargets)
            {
                glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, 0, 0);
                glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, to.tex, 0);
            }
            glDrawBuffer(GL_NONE);
            glEnable(GL_DEPTH_TEST);
            glDepthFunc(GL_ALWAYS);
            glDepthMask(GL_TRUE);
        }
        else
        {
            if (!stableTargets)
            {
                glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, 0, 0);
                glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, to.tex, 0);
            }
            glDrawBuffer(GL_COLOR_ATTACHMENT0);
            glDisable(GL_DEPTH_TEST);
            glColorMask((g.colorMask & 1) != 0, (g.colorMask & 2) != 0, (g.colorMask & 4) != 0, (g.colorMask & 8) != 0);
        }
        glDisable(GL_BLEND);
        glDisable(GL_SCISSOR_TEST);
        glDisable(GL_DITHER);
        glViewport(0, 0, to.width * to.sx, to.height * to.sy);
        glUseProgram(convProgram);
        glBindVertexArray(vao);
        glUniform2f(convUniform("u_size"), static_cast<float>(to.width), static_cast<float>(to.height));
        glUniform2i(convUniform("u_scale"), to.sx, to.sy);
        setConvSrcScale(g.src, to);
        glUniform1i(convUniform("u_instanced"), 1);
        glUniform1i(convUniform("u_pageH"), to.pageHeight);
        glUniform1i(convUniform("u_mode"), g.mode);
        glUniform1i(convUniform("u_lut"), g.lut);
        glUniform1i(convUniform("u_tex"), 0);
        glUniform1i(convUniform("u_lutZfromC"), 6);
        glUniform1i(convUniform("u_lutCfromZ"), 7);
        glUniform1i(convUniform("u_lut16from32"), 6);
        glUniform1i(convUniform("u_lut32from16"), 7);
        glUniform1i(convUniform("u_mask"), 6);
        glActiveTexture(GL_TEXTURE6);
        glBindTexture(GL_TEXTURE_2D, convLut6(g.lut));
        glActiveTexture(GL_TEXTURE7);
        glBindTexture(GL_TEXTURE_2D, convLut7(g.lut));
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, g.src);
        glDrawArraysInstanced(GL_TRIANGLES, 0, 6, count);
        ++statMoveDraws;
        glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
        glDepthMask(GL_TRUE);
        glBindVertexArray(0);
        glUseProgram(0);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        glStateDirty = true;
    }

    // convertPage from any texture; colorMask: RGBA write bits of a colour destination.
    void convertTex(GLuint srcTex, Target &to, int fx, int fy, int tx, int ty, int mode, int lut, int colorMask)
    {
        beforeTextureWrite(to.tex);
        if (stableTargets)
            bindSoloFbo(GL_FRAMEBUFFER, to.tex, to.depth);
        else
        {
            glBindFramebuffer(GL_FRAMEBUFFER, convFbo);
            noteAttach(to.tex);
        }
        if (to.depth)
        {
            if (!stableTargets)
            {
                glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, 0, 0);
                glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, to.tex, 0);
            }
            glDrawBuffer(GL_NONE);
            glEnable(GL_DEPTH_TEST);
            glDepthFunc(GL_ALWAYS);
            glDepthMask(GL_TRUE);
        }
        else
        {
            if (!stableTargets)
            {
                glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, 0, 0);
                glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, to.tex, 0);
            }
            glDrawBuffer(GL_COLOR_ATTACHMENT0);
            glDisable(GL_DEPTH_TEST);
            glColorMask((colorMask & 1) != 0, (colorMask & 2) != 0, (colorMask & 4) != 0, (colorMask & 8) != 0);
        }
        glDisable(GL_BLEND);
        glDisable(GL_SCISSOR_TEST);
        glDisable(GL_DITHER);
        glViewport(0, 0, to.width * to.sx, to.height * to.sy);
#if defined(VKGL_REDIRECT)
        if (lut == 6 && mergeProgram)
        {
            // Partial-word merge on Vulkan: u_dst is the destination's own
            // pixel (local read after the texture barrier), as convFs's u_lut 6.
            glUseProgram(mergeProgram);
            glBindVertexArray(vao);
            glUniform4i(glGetUniformLocation(mergeProgram, "u_rect"), tx, ty, 64, to.pageHeight);
            glUniform2f(glGetUniformLocation(mergeProgram, "u_size"), static_cast<float>(to.width), static_cast<float>(to.height));
            glUniform1i(glGetUniformLocation(mergeProgram, "u_instanced"), 0);
            glUniform2i(glGetUniformLocation(mergeProgram, "u_scale"), to.sx, to.sy);
            glUniform2i(glGetUniformLocation(mergeProgram, "u_dstOrigin"), tx, ty);
            glUniform1i(glGetUniformLocation(mergeProgram, "u_tex"), 0);
            glUniform1i(glGetUniformLocation(mergeProgram, "u_mask"), 6);
            glActiveTexture(GL_TEXTURE6);
            glBindTexture(GL_TEXTURE_2D, convMaskTex);
            glActiveTexture(GL_TEXTURE0);
            glBindTexture(GL_TEXTURE_2D, srcTex);
            textureBarrier();
            glDrawArrays(GL_TRIANGLES, 0, 6);
            glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
            glDepthMask(GL_TRUE);
            glBindVertexArray(0);
            glUseProgram(0);
            glBindFramebuffer(GL_FRAMEBUFFER, 0);
            glStateDirty = true;
            return;
        }
#endif
        glUseProgram(convProgram);
        glBindVertexArray(vao);
        glUniform4i(convUniform("u_rect"), tx, ty, 64, to.pageHeight);
        glUniform2f(convUniform("u_size"), static_cast<float>(to.width), static_cast<float>(to.height));
        glUniform2i(convUniform("u_scale"), to.sx, to.sy);
        setConvSrcScale(srcTex, to);
        glUniform1i(convUniform("u_mode"), mode);
        glUniform1i(convUniform("u_lut"), lut);
        glUniform2i(convUniform("u_srcOrigin"), fx, fy);
        glUniform2i(convUniform("u_dstOrigin"), tx, ty);
        glUniform1i(convUniform("u_instanced"), 0);
        glUniform1i(convUniform("u_tex"), 0);
        glUniform1i(convUniform("u_lutZfromC"), 6);
        glUniform1i(convUniform("u_lutCfromZ"), 7);
        glUniform1i(convUniform("u_lut16from32"), 6);
        glUniform1i(convUniform("u_lut32from16"), 7);
        glUniform1i(convUniform("u_mask"), 6);
        glActiveTexture(GL_TEXTURE6);
        glBindTexture(GL_TEXTURE_2D, (lut == 5 || lut == 6) ? convMaskTex : convLut6(lut));
        glActiveTexture(GL_TEXTURE7);
        glBindTexture(GL_TEXTURE_2D, convLut7(lut));
        if (lut == 6)
        {
            // Merge (P1b): the destination read at each fragment's own sample,
            // after the writes before it (texture barrier).
            glUniform1i(convUniform("u_dstTex"), 11);
            glActiveTexture(GL_TEXTURE11);
            glBindTexture(GL_TEXTURE_2D, to.tex);
            textureBarrier();
        }
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, srcTex);
        glDrawArrays(GL_TRIANGLES, 0, 6);
        if (lut == 6)
        {
            glActiveTexture(GL_TEXTURE11);
            glBindTexture(GL_TEXTURE_2D, 0);
            glActiveTexture(GL_TEXTURE0);
        }
        glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
        glDepthMask(GL_TRUE);
        glBindVertexArray(0);
        glUseProgram(0);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        glStateDirty = true;
    }

    // 16-bit and 32-bit colour layouts of a page convert on the GPU (CT16 <->
    // CT32 only; not with the Z32 swizzle or a depth buffer).
    bool mixed16Ok(const Target &from, const Target &to) const
    {
        if ((from.layout == Layout::C16) == (to.layout == Layout::C16))
            return true;
        return !noC16Move && c16TablesOk && !from.depth && !to.depth && (from.layout == Layout::C32 || to.layout == Layout::C32);
    }

    // A whole-word copy of a page between two colour targets of any 32-bit
    // layout, the same 16-bit layout, or CT16 and CT32.
    bool copyColorPage(Target &from, Target &to, uint32_t page)
    {
        int fx = 0, fy = 0, tx = 0, ty = 0;
        if (from.depth || to.depth || !from.pageRect(page, fx, fy) || !to.pageRect(page, tx, ty))
            return false;
        if (!mixed16Ok(from, to))
            return false;
        if ((from.layout == Layout::C16) != (to.layout == Layout::C16))
            convertPage(from, to, fx, fy, tx, ty, 0, to.layout == Layout::C16 ? 3 : 4, false);
        else if (from.layout == to.layout)
            copyScaled(from.tex, fx, fy, to.tex, tx, ty, 64, from.pageHeight);
        else
            convertPage(from, to, fx, fy, tx, ty, 0, to.layout == Layout::Z32 ? 1 : 2, false);
        return true;
    }

    // Moves page ownership from target fromId to toId on the GPU. Returns
    // false when the formats need the CPU copy in between.
    bool transferPage(uint32_t page, int fromId, int toId)
    {
        if (!layoutTablesOk)
            return false;
        Target &from = *targets[fromId];
        Target &to = *targets[toId];
        int fx = 0, fy = 0, tx = 0, ty = 0;
        if (!from.pageRect(page, fx, fy) || !to.pageRect(page, tx, ty))
            return false;
        if (moveBack && to.uid != 0u && movedOutUid[page] == to.uid && movedToUid[page] == from.uid && movedOutSerial[page] == writeSerial[page] &&
            movedOutCpu[page] == cpuVersion[page] && !(from.depth && from.layout == Layout::Z32) && !(to.depth && to.layout == Layout::Z32))
        {
            // Back to the target it last left, unchanged since: its copy there is current (no copy, no flush: no
            // texture changes). Not Z24 (high plane); the other layouts hold every bit of the page.
            highPlane[page] = -1;
            ++statMoveBack;
            transferDone(page, from, to, toId, true);
            return true;
        }
        if (from.layout == Layout::Z16 || to.layout == Layout::Z16)
        {
            // PSMZ16 pages (Z16-DESIGN.md 3.2): another Z16 view of the same pages (FBW 5 / FBW 10), a CT32 or a CT16
            // colour target, on the GPU; anything else (Z24, Z32-layout colour) through the CPU copy. A Z16 page has
            // no high plane: it owns every bit of each word, and a colour target receiving it takes the whole word.
            if (!z16Moves)
                return false;
            int lut = -1; // -1: CPU copy; 0: Z16 -> Z16 copy; else the conversion lut
            if (from.layout == Layout::Z16 && to.layout == Layout::Z16)
                lut = 0;
            else if (!z16TablesOk)
                lut = -1;
            else if (to.layout == Layout::Z16 && !from.depth && from.layout == Layout::C32)
                lut = 7;
            else if (to.layout == Layout::Z16 && !from.depth && from.layout == Layout::C16)
                lut = 9;
            else if (from.layout == Layout::Z16 && !to.depth && to.layout == Layout::C32)
                lut = 8;
            else if (from.layout == Layout::Z16 && !to.depth && to.layout == Layout::C16)
                lut = 10;
            if (lut < 0)
                return false;
            if (haveBatch && (key.colorTarget == fromId || key.colorTarget == toId || key.depthTarget == fromId || key.depthTarget == toId ||
                              batchUses(from.tex) || batchUses(to.tex)))
                flushBatch();
            if (lut == 0)
                copyScaled(from.tex, fx, fy, to.tex, tx, ty, 64, 64, true);
            else
                convertPage(from, to, fx, fy, tx, ty, to.depth ? 1 : 0, lut, false);
            highPlane[page] = -1;
            ++statZ16Moves;
            transferDone(page, from, to, toId);
            return true;
        }
        const bool depthTo16 = from.depth && !to.depth && to.layout == Layout::C16;
        if (depthTo16 ? !(zToC16Gpu && !noC16Move && c16TablesOk) : !mixed16Ok(from, to))
            return false;
        if (haveBatch && (key.colorTarget == fromId || key.colorTarget == toId || key.depthTarget == fromId || key.depthTarget == toId ||
                          batchUses(from.tex) || batchUses(to.tex)))
            flushBatch();
        if (!from.depth && !to.depth)
        {
            copyColorPage(from, to, page);
            highPlane[page] = -1;
        }
        else if (!from.depth && to.depth)
        {
            // Z buffer from colour: the low 24 bits become Z; the colour target
            // keeps the high plane in its alpha.
            convertPage(from, to, fx, fy, tx, ty, 1, from.layout == Layout::C32 ? 1 : 0, false);
            highPlane[page] = fromId;
        }
        else if (from.depth && to.depth)
        {
            copyScaled(from.tex, fx, fy, to.tex, tx, ty, 64, from.pageHeight, true);
        }
        else if (depthTo16)
        {
            // CT16 from a Z buffer: the page's 32-bit words assembled in the
            // page view (the high plane as it is held, then Z into the low 24
            // bits), then laid out as 16-bit pixels (the CT16 <-> CT32 tables).
            // In an acquire the three steps of all its pages run as three
            // batches at its end (flushZToC16).
            Target &v = pageViewTarget();
            ZToC16Move m{};
            m.from = &from;
            m.to = &to;
            m.fx = fx;
            m.fy = fy;
            m.tx = tx;
            m.ty = ty;
            v.pageRect(page, m.vx, m.vy);
            const int holder = highPlane[page];
            if (holder >= 0 && targets[holder] && !targets[holder]->depth && targets[holder]->pageRect(page, m.hx, m.hy) &&
                mixed16Ok(*targets[holder], v))
                m.holder = targets[holder].get();
            if (!m.holder)
                uploadPage(v, page); // the CPU copy holds the high plane
            else if (m.holder->layout == Layout::C32)
            {
                copyScaled(m.holder->tex, m.hx, m.hy, v.tex, m.vx, m.vy, 64, 32);
                m.holder = nullptr; // done
            }
            if (deferMoves)
                zToC16Moves.push_back(m);
            else
            {
                if (m.holder)
                    convertPage(*m.holder, v, m.hx, m.hy, m.vx, m.vy, 0, m.holder->layout == Layout::C16 ? 4 : 2, false);
                convertPage(from, v, fx, fy, m.vx, m.vy, 2, 2, true);
                convertPage(v, to, m.vx, m.vy, tx, ty, 0, 3, false);
            }
            highPlane[page] = -1;
            ++statZToC16;
        }
        else
        {
            // Colour from a Z buffer: the high plane first (as it is held), then
            // Z into the low 24 bits.
            const int holder = highPlane[page];
            if (holder != toId)
            {
                if (holder >= 0 && targets[holder] && copyColorPage(*targets[holder], to, page))
                {
                }
                else
                {
                    // The CPU copy holds the high plane (its low bits are stale; overwritten below).
                    uploadPage(to, page);
                }
            }
            convertPage(from, to, fx, fy, tx, ty, 2, to.layout == Layout::C32 ? 2 : 0, true);
            highPlane[page] = -1;
        }
        transferDone(page, from, to, toId);
        return true;
    }

    // transferPage's common end: page ownership handed to toId (back: without a copy, see moveBack).
    void transferDone(uint32_t page, Target &from, Target &to, int toId, bool back = false)
    {
        from.version[page] = 0u;
        debugOwner(page, toId, back ? "gpu move back (no copy)" : "gpu move");
        owner[page] = toId;
        pageChanged(page);
        ++writeSerial[page];
        movedOutUid[page] = from.uid;
        movedToUid[page] = to.uid;
        movedOutSerial[page] = writeSerial[page];
        movedOutCpu[page] = cpuVersion[page];
        ++statGpuMoves;
        if (stats)
        {
            char text[112];
            std::snprintf(text, sizeof(text), "%s %03x/%u -> %s %03x/%u (%s)%s", layoutName(from), from.fbp, from.fbw, layoutName(to), to.fbp,
                          to.fbw, cause.c_str(), back ? " back, no copy" : "");
            ++statMoveCauses[text];
        }
    }

    // A page an upload may patch on the GPU: held by a 32-bit colour target or
    // a Z buffer (whose high plane is in memory or a 32-bit colour target).
    // Not a Z16 page (its halfwords would be patched as Z24 words in Z32 order): the upload copies it back.
    bool patchable(uint32_t page)
    {
        const int id = owner[page];
        if (id < 0 || !layoutTablesOk || !targets[id])
            return false;
        Target &t = *targets[id];
        if (t.layout == Layout::Z16)
            return false;
        int x = 0, y = 0;
        if (!t.pageRect(page, x, y))
            return false;
        if (!t.depth)
            return t.layout != Layout::C16;
        const int holder = highPlane[page];
        if (holder < 0)
            return true;
        return targets[holder] && !targets[holder]->depth && targets[holder]->layout != Layout::C16 &&
               targets[holder]->pageRect(page, x, y);
    }

    // Pixels [c0, c1) of the upload (raster order) were written to the CPU
    // copy: draw those words into the targets holding patch pages.
    void patchUpload(uint32_t c0, uint32_t c1)
    {
        if (c1 <= c0 || patchPages.empty())
            return;
        const auto &b = patchCommand.bitbltbuf;
        const auto &p = patchCommand.trxpos;
        const uint32_t rrw = std::max<uint32_t>(patchCommand.trxreg.rrw, 1u);
        const uint32_t dbw = std::max<uint32_t>(b.dbw, 1u);
        if (coalescePatches)
        {
            {
                ScopedNs timer(statPatchNs);
                const bool partial = b.dpsm == GS_PSM_T8 || b.dpsm == GS_PSM_T4; // (partialPatchable pages only)
                for (uint32_t i = c0; i < c1; ++i)
                {
                    const uint32_t x = p.dsax + i % rrw, y = p.dsay + i / rrw;
                    uint32_t w = 0, bits = 0;
                    if (partial)
                    {
                        // The byte (PSMT8) or nibble (PSMT4) of its word the texel writes.
                        using namespace GSMem;
                        if (b.dpsm == GS_PSM_T8)
                        {
                            const uint32_t a = PixelStorageTraits<P8>::Address(PageTableP8, b.dbp, dbw, x, y);
                            w = (a >> 2u) & 0xFFFFFu;
                            bits = 0xFFu << ((a & 3u) * 8u);
                        }
                        else
                        {
                            const uint32_t a = PixelStorageTraits<P4>::Address(PageTableP4, b.dbp, dbw, x, y);
                            w = (a >> 3u) & 0xFFFFFu;
                            bits = 0xFu << ((a & 7u) * 4u);
                        }
                    }
                    else
                        w = wordAddress(b.dpsm, b.dbp, dbw, x, y);
                    const uint32_t page = w >> 11u;
                    if (patchSlot[page] < 0)
                    {
                        // (A page-aligned PSMT8 / PSMT4 upload: the rest of this row inside the same 128-texel-wide page
                        // is not patched either; skipped. Large uploads - GT Mode's 640x480 car view background - patch
                        // only a few pages.)
                        if (partial && (b.dbp & 31u) == 0u)
                        {
                            const uint32_t rowEnd = p.dsax + rrw, pageEnd = (x | 127u) + 1u;
                            const uint32_t stop = std::min(rowEnd, pageEnd);
                            if (stop > x + 1u)
                                i += stop - x - 1u;
                        }
                        continue;
                    }
                    if (partial)
                    {
                        pendingBits[page * 2048u + (w & 2047u)] |= bits;
                        pendingPartial[page] = 1u;
                    }
                    else
                        pendingMask[page * 32u + ((w & 2047u) >> 6u)] |= 1ull << (w & 63u);
                    if (!pendingAny[page])
                    {
                        pendingAny[page] = 1u;
                        pendingPages.push_back(page);
                    }
                }
                ++statPatchRecorded;
            }
            if (pendingPages.size() > 256u)
                flushPendingPatches();
            return;
        }
        ScopedNs timer(statPatchNs);
        patchWords.assign(patchPages.size() * 2048u, 0u);
        patchMask.assign(patchPages.size() * 2048u, 0u);
        for (uint32_t i = c0; i < c1; ++i)
        {
            const uint32_t x = p.dsax + i % rrw, y = p.dsay + i / rrw;
            const uint32_t w = wordAddress(b.dpsm, b.dbp, dbw, x, y);
            const int slot = patchSlot[w >> 11u];
            if (slot < 0)
                continue;
            uint32_t value = 0;
            std::memcpy(&value, vram + static_cast<size_t>(w) * 4u, 4);
            patchWords[static_cast<size_t>(slot) * 2048u + (w & 2047u)] = value;
            patchMask[static_cast<size_t>(slot) * 2048u + (w & 2047u)] = 1u;
        }
        for (size_t slot = 0; slot < patchPages.size(); ++slot)
            patchPage(slot);
        forgetBindings();
    }

    void patchPage(size_t slot)
    {
        patchPageWords(patchPages[slot], &patchWords[slot * 2048u], &patchMask[slot * 2048u]);
    }

    // Draws the words of page `page` flagged in mask (2048 flags) with their
    // values from words (2048) into the target holding the page.
    void patchPageWords(uint32_t page, const uint32_t *words, const uint8_t *mask)
    {
        const int id = owner[page];
        if (id < 0 || !targets[id])
            return; // copied back meanwhile: the CPU copy has the words
        if (std::find(mask, mask + 2048, static_cast<uint8_t>(1u)) == mask + 2048)
            return;
        Target &t = *targets[id];
        if (t.layout == Layout::Z16)
        {
            // Never reached: patchable refuses Z16 pages (an upload copies them back first).
            static int warned = 0;
            if (warned++ < 4)
                std::fprintf(stderr, "[gs-gl] upload patch of Z16 page %03x dropped (unexpected)\n", page);
            return;
        }
        const int holder = t.depth ? highPlane[page] : -1;
        if (haveBatch && (key.colorTarget == id || key.depthTarget == id || batchUses(t.tex) ||
                          (holder >= 0 && (key.colorTarget == holder || key.depthTarget == holder || batchUses(targets[holder]->tex)))))
            flushBatch();
        auto draw = [&](Target &dst, bool zSwizzle, int mode, int colorMask) {
            int tx = 0, ty = 0;
            if (!dst.pageRect(page, tx, ty))
                return;
            uint32_t image[64 * 32];
            uint8_t written[64 * 32];
            std::memset(image, 0, sizeof(image));
            std::memset(written, 0, sizeof(written));
            const uint16_t *table = zSwizzle ? pixelOfWordZ : pixelOfWordC;
            for (uint32_t w = 0; w < 2048u; ++w)
                if (mask[w])
                {
                    const uint32_t px = table[w];
                    image[(px >> 6u) * 64u + (px & 63u)] = words[w];
                    written[(px >> 6u) * 64u + (px & 63u)] = 1u;
                }
            const uint32_t ring = patchRing++ % 8u;
            glActiveTexture(GL_TEXTURE0);
            glBindTexture(GL_TEXTURE_2D, patchTex[ring]);
            glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
            glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 64, 32, GL_RGBA, GL_UNSIGNED_BYTE, image);
            glBindTexture(GL_TEXTURE_2D, patchMaskTex[ring]);
            glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
            glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 64, 32, GL_RED_INTEGER, GL_UNSIGNED_BYTE, written);
            glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
            glBindTexture(GL_TEXTURE_2D, 0);
            convMaskTex = patchMaskTex[ring];
            convertTex(patchTex[ring], dst, 0, 0, tx, ty, mode, 5, colorMask);
        };
        if (t.depth)
        {
            draw(t, true, 1, 15); // Z: the low 24 bits
            if (holder >= 0 && targets[holder])
                draw(*targets[holder], targets[holder]->layout == Layout::Z32, 0, 8); // the high plane (alpha)
        }
        else
        {
            draw(t, t.layout == Layout::Z32, 0, 15);
        }
        ++writeSerial[page];
        for (uint32_t w = 0; w < 2048u; ++w)
            if (mask[w])
                cpuValid[page * 32u + (w >> 6u)] |= 1ull << (w & 63u);
        cpuValidAny[page] = 1u;
        pageChanged(page);
        ++pageEpoch[page];
        ++statPatches;
    }

    // Draws page `page`'s recorded upload words (coalescePatches) into its
    // holder, with their values from the CPU copy (their last upload).
    // A page with PSMT8/PSMT4 upload bits (pendingPartial): every recorded bit
    // (whole words from pendingMask, bytes and nibbles from pendingBits)
    // merged into its holder on the GPU, values from the CPU copy.
    void applyPartialPage(uint32_t page)
    {
        uint64_t *whole = &pendingMask[page * 32u];
        uint32_t *bits = &pendingBits[page * 2048u];
        pendingPartial[page] = 0u;
        const int id = owner[page];
        int tx = 0, ty = 0;
        if (id < 0 || !targets[id] || targets[id]->depth || targets[id]->layout == Layout::C16 || !targets[id]->pageRect(page, tx, ty) ||
            !textureBarrier)
        {
            // Copied back meanwhile: the CPU copy has the bits. (A holder that
            // cannot merge never got partial bits: BeginTransfer copies back.)
            static int warned = 0;
            if (id >= 0 && targets[id] && warned++ < 4)
                std::fprintf(stderr, "[gs-gl] partial-word patch of page %03x dropped: its holder cannot merge (unexpected)\n", page);
            std::fill_n(whole, 32, 0ull);
            std::fill_n(bits, 2048, 0u);
            return;
        }
        Target &t = *targets[id];
        if (haveBatch && (key.colorTarget == id || key.depthTarget == id || batchUses(t.tex)))
            flushBatch();
        const uint32_t *pageWords = reinterpret_cast<const uint32_t *>(vram + static_cast<size_t>(page) * 8192u);
        const uint16_t *table = t.layout == Layout::Z32 ? pixelOfWordZ : pixelOfWordC;
        uint32_t image[64 * 32];
        std::fill(std::begin(image), std::end(image), 0u);
        std::fill(applyBits.begin(), applyBits.end(), 0u);
        for (uint32_t w = 0; w < 2048u; ++w)
        {
            const uint32_t m = (((whole[w >> 6u] >> (w & 63u)) & 1ull) != 0ull) ? 0xFFFFFFFFu : bits[w];
            if (m == 0u)
                continue;
            const uint32_t px = table[w];
            image[(px >> 6u) * 64u + (px & 63u)] = pageWords[w];
            applyBits[(px >> 6u) * 64u + (px & 63u)] = m;
            if (m == 0xFFFFFFFFu)
                cpuValid[page * 32u + (w >> 6u)] |= 1ull << (w & 63u); // whole words are current in both
        }
        const uint32_t ring = patchRing++ % 8u;
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, patchTex[ring]);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 64, 32, GL_RGBA, GL_UNSIGNED_BYTE, image);
        glBindTexture(GL_TEXTURE_2D, patchBitsTex[ring]);
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 64, 32, GL_RED_INTEGER, GL_UNSIGNED_INT, applyBits.data());
        glBindTexture(GL_TEXTURE_2D, 0);
        convMaskTex = patchBitsTex[ring];
        // PS2X_GS_GL_PARTIAL_VERIFY=1 (diagnostics, slow): the page read back
        // before and after the merge (one sample per pixel), the merge's
        // result checked against (before & ~bits) | (upload & bits).
        static const bool verify = envFlag("PS2X_GS_GL_PARTIAL_VERIFY", false);
        static uint64_t verifyPages = 0, verifyBad = 0;
        static std::vector<uint32_t> before(64u * 32u), after(64u * 32u);
        if (verify)
            readTargetPixels(false, t.tex, tx, ty, 64, 32, GL_RGBA, GL_UNSIGNED_BYTE, before.data());
        convertTex(patchTex[ring], t, 0, 0, tx, ty, 0, 6, 15);
        if (verify)
        {
            readTargetPixels(false, t.tex, tx, ty, 64, 32, GL_RGBA, GL_UNSIGNED_BYTE, after.data());
            uint32_t bad = 0;
            for (uint32_t i = 0; i < 64u * 32u; ++i)
            {
                const uint32_t want = (before[i] & ~applyBits[i]) | (image[i] & applyBits[i]);
                if (after[i] != want && bad++ < 2u && verifyBad < 8u)
                    std::fprintf(stderr, "[gs-gl] partial verify: page %03x pixel %u,%u bits %08x before %08x upload %08x after %08x\n", page,
                                 i & 63u, i >> 6u, applyBits[i], before[i], image[i], after[i]);
            }
            ++verifyPages;
            verifyBad += bad ? 1u : 0u;
            if ((verifyPages & 1023u) == 0u || (bad && verifyBad <= 8u))
                std::fprintf(stderr, "[gs-gl] partial verify: %llu pages merged, %llu with a wrong pixel\n", (unsigned long long)verifyPages,
                             (unsigned long long)verifyBad);
        }
        ++writeSerial[page];
        ++pageEpoch[page];
        for (uint32_t i = 0; i < 32u && !cpuValidAny[page]; ++i)
            cpuValidAny[page] = cpuValid[page * 32u + i] != 0ull ? 1u : 0u;
        pageChanged(page);
        ++statPatches;
        ++statPartialPatches;
        std::fill_n(whole, 32, 0ull);
        std::fill_n(bits, 2048, 0u);
    }

    void applyPendingPage(uint32_t page)
    {
        page &= kPageCount - 1u;
        if (!pendingAny[page])
            return;
        pendingAny[page] = 0u;
        if (pendingPartial[page])
        {
            applyPartialPage(page);
            return;
        }
        uint64_t *bits = &pendingMask[page * 32u];
        uint64_t all = ~0ull;
        for (uint32_t i = 0; i < 32u; ++i)
            all &= bits[i];
        const int id = owner[page];
        if (id < 0 || !targets[id])
        {
            std::fill_n(bits, 32, 0ull); // copied back meanwhile: the CPU copy has the words
            return;
        }
        Target &t = *targets[id];
        int tx = 0, ty = 0;
        if (all == ~0ull && !t.depth && t.layout != Layout::C16 && t.pageRect(page, tx, ty))
        {
            // Written whole: the page's words straight from the CPU copy (the
            // same 2048 words a patch of every word draws), no mask or draw.
            if (haveBatch && (key.colorTarget == id || key.depthTarget == id || batchUses(t.tex)))
                flushBatch();
            const uint32_t version = t.version[page];
            std::string why;
            if (stats || static_cast<int>(page) == debugPage)
            {
                why = cause;
                cause = "upload patch (whole page)";
            }
            uploadPage(t, page);
            if (stats || static_cast<int>(page) == debugPage)
                cause = why;
            t.version[page] = version; // as after a patch: the holder's CPU version is unchanged
            ++writeSerial[page];
            std::fill_n(&cpuValid[page * 32u], 32, ~0ull);
            cpuValidAny[page] = 1u;
            pageChanged(page);
            ++pageEpoch[page];
            ++statPatches;
            ++statPatchWhole;
        }
        else
        {
            const uint32_t *pageWords = reinterpret_cast<const uint32_t *>(vram + static_cast<size_t>(page) * 8192u);
            for (uint32_t w = 0; w < 2048u; ++w)
            {
                const bool set = ((bits[w >> 6u] >> (w & 63u)) & 1ull) != 0ull;
                applyMask[w] = set ? 1u : 0u;
                applyWords[w] = set ? pageWords[w] : 0u;
            }
            patchPageWords(page, applyWords.data(), applyMask.data());
            ++statPatchMasked;
        }
        std::fill_n(bits, 32, 0ull);
    }

    // Applies every recorded upload word (coalescePatches). Called at the start
    // of every backend call that is not an upload, and before a page with
    // recorded words is copied back (downloadPage).
    void flushPendingPatches()
    {
        if (pendingPages.empty())
            return;
        ScopedNs timer(statPatchNs);
        std::vector<uint32_t> pages;
        pages.swap(pendingPages); // applying may copy pages back (downloadPage applies its own page first)
        pendingPages.swap(pendingSpare);
        pendingPages.clear();
        for (const uint32_t page : pages)
            applyPendingPage(page);
        pages.clear();
        pendingSpare.swap(pages); // keep the capacity
        forgetBindings();
    }

    static const char *layoutName(const Target &t)
    {
        if (t.depth)
            return t.layout == Layout::Z16 ? "Z16" : "Z";
        return t.layout == Layout::C16 ? "C16" : (t.layout == Layout::Z32 ? "Z32c" : (t.layout == Layout::Z16 ? "Z16c" : "C"));
    }

    // coverX0..coverY1: a rectangle the draw is known to overwrite completely
    // (opaque, no test that can keep old pixels): pages inside it need no load.
    void acquirePages(int id, int x0, int y0, int x1, int y1, bool write, int coverX0 = 1, int coverY0 = 1, int coverX1 = 0,
                      int coverY1 = 0)
    {
        Target &t = *targets[id];
        deferMoves = true;
        forRectPages(t.fbp, false, t.fbw, layoutPsm(t.layout), x0, y0, x1, y1, [&](uint32_t page) {
            if (owner[page] == id)
                return;
            int px = 0, py = 0;
            if (write && t.pageRect(page, px, py) && px >= coverX0 && py >= coverY0 && px + 63 <= coverX1 &&
                py + t.pageHeight - 1 <= coverY1)
            {
                // Overwritten whole: take it over without its old contents. A Z
                // buffer leaves the high plane where it is.
                int previous = owner[page];
                if (t.depth && t.layout == Layout::Z16)
                {
                    highPlane[page] = -1; // Z16 writes every bit of the page
                }
                else if (t.depth)
                {
                    if (previous >= 0 && targets[previous]->layout == Layout::Z16)
                    {
                        // A Z24 buffer over a Z16 page: Z24 keeps bits 24-31, which the Z16 target holds; the page
                        // goes back to the CPU copy first, which then holds them (highPlane -1).
                        const std::string why = stats ? cause : std::string();
                        if (stats)
                            cause = "Z24 over a Z16 page";
                        downloadPage(page);
                        if (stats)
                            cause = why;
                        previous = owner[page];
                        highPlane[page] = -1;
                    }
                    if (previous >= 0 && !targets[previous]->depth)
                        highPlane[page] = previous;
                }
                else
                {
                    highPlane[page] = -1;
                }
                if (previous >= 0)
                    targets[previous]->version[page] = 0u;
                debugOwner(page, id, "overwritten whole");
                owner[page] = id;
                pageChanged(page);
                return;
            }
            if (owner[page] >= 0)
            {
                if (transferPage(page, owner[page], id))
                    return;
                if (stats)
                {
                    const Target &from = *targets[owner[page]];
                    char text[96];
                    std::snprintf(text, sizeof(text), "switch %s %03x/%u -> %s %03x/%u", layoutName(from), from.fbp, from.fbw,
                                  layoutName(t), t.fbp, t.fbw);
                    cause = text;
                }
                downloadPage(page);
            }
            if (t.version[page] != cpuVersion[page])
                uploadPage(t, page);
            if (write)
            {
                if (owner[page] < 0 && t.layout != Layout::C16 && t.layout != Layout::Z16)
                {
                    // Taken from the CPU copy: every word is the same in both
                    // until the draws below (clearCpuValidRect) change some.
                    std::fill_n(&cpuValid[page * 32u], 32, ~0ull);
                    cpuValidAny[page] = 1u;
                    ++pageEpoch[page];
                }
                if (owner[page] != id)
                {
                    debugOwner(page, id, "draw write");
                    pageChanged(page); // (and the CPU-valid words set above)
                }
                owner[page] = id;
            }
        });
        deferMoves = false;
        flushMoves();
        flushZToC16();
        if (write)
            forRectPages(t.fbp, false, t.fbw, layoutPsm(t.layout), x0, y0, x1, y1, [&](uint32_t page) {
                ++writeSerial[page];
                clearCpuValidRect(t, page, x0, y0, x1, y1);
            });
    }

    // acquirePages for an explicit page list (read only).
    void acquirePageList(int id, const std::vector<uint32_t> &pages)
    {
        Target &t = *targets[id];
        deferMoves = true;
        for (const uint32_t page : pages)
        {
            if (owner[page] == id)
                continue;
            int px = 0, py = 0;
            if (!t.pageRect(page, px, py))
                continue;
            if (owner[page] >= 0)
            {
                if (transferPage(page, owner[page], id))
                    continue;
                if (stats)
                {
                    const Target &from = *targets[owner[page]];
                    char text[96];
                    std::snprintf(text, sizeof(text), "texture page of %s %03x/%u into %s %03x/%u", layoutName(from), from.fbp,
                                  from.fbw, layoutName(t), t.fbp, t.fbw);
                    cause = text;
                }
                downloadPage(page);
            }
            if (t.version[page] != cpuVersion[page])
                uploadPage(t, page);
        }
        deferMoves = false;
        flushMoves();
        flushZToC16();
    }

    // Makes the CPU copy of the pixels (x0, y0)-(x1, y1) of a buffer current.
    void syncPagesToCpu(uint32_t bp, uint32_t bw, uint32_t psm, int x0, int y0, int x1, int y1)
    {
        const uint32_t basePage = bp >> 5u;
        const bool straddles = (bp & 31u) != 0u;
        // GT3_GS_GL_NO_CPUVALID=1: always copy GPU pages back (diagnostic).
        static const bool noValid = envFlag("GT3_GS_GL_NO_CPUVALID", false);
        if (noValid)
        {
            forRectPages(basePage, straddles, bw, psm, x0, y0, x1, y1, [&](uint32_t page) {
                if (owner[page] >= 0)
                    downloadPage(page);
            });
            return;
        }
        bool check = false;
        uint64_t signature = 0x9E3779B97F4A7C15ull;
        forRectPages(basePage, straddles, bw, psm, x0, y0, x1, y1, [&](uint32_t page) {
            if (owner[page] >= 0 && cpuValidAny[page])
                check = true;
            const uint32_t v[3] = {page, pageEpoch[page], owner[page] >= 0 ? 1u : 0u};
            signature = hashBytes(v, sizeof(v), signature);
            pageObserved(page);
        });
        const int32_t q[7] = {static_cast<int32_t>(bp), static_cast<int32_t>(bw), static_cast<int32_t>(psm), x0, y0, x1, y1};
        const uint64_t query = hashBytes(q, sizeof(q), 0x53594E43ull);
        if (check)
        {
            auto it = syncOk.find(query);
            if (it != syncOk.end() && it->second == signature)
            {
                ++statValidReads;
                return;
            }
        }
        // A buffer starting inside a page: forRectPages also names the page
        // after it, which a small area may not reach. Decide by address.
        const bool exact = check || (straddles && (x1 - x0 + 1) * (y1 - y0 + 1) <= 4096);
        if (exact && x1 >= x0 && y1 >= y0 && (x1 - x0 + 1) * (y1 - y0 + 1) <= 65536)
        {
            // Copy back only the GPU pages with a word read here that the CPU
            // copy does not have. A check that needed nothing is remembered
            // until one of its pages changes (syncOk, pageEpoch).
            std::fill(needPage.begin(), needPage.end(), static_cast<uint8_t>(0u));
            const uint32_t w = std::max<uint32_t>(bw, 1u);
            bool addressed = true;
            for (int y = y0; y <= y1 && addressed; ++y)
                for (int x = x0; x <= x1; ++x)
                {
                    const uint32_t word = wordAddress(psm, bp, w, static_cast<uint32_t>(x), static_cast<uint32_t>(y));
                    if (word == ~0u)
                    {
                        addressed = false;
                        break;
                    }
                    const uint32_t page = word >> 11u;
                    if (owner[page] >= 0 && ((cpuValid[page * 32u + ((word & 2047u) >> 6u)] >> (word & 63u)) & 1ull) == 0ull)
                        needPage[page] = 1u;
                }
            if (addressed)
            {
                bool downloaded = false;
                forRectPages(basePage, straddles, bw, psm, x0, y0, x1, y1, [&](uint32_t page) {
                    if (owner[page] >= 0 && needPage[page])
                    {
                        downloadPage(page);
                        downloaded = true;
                    }
                    else if (owner[page] >= 0)
                        ++statValidReads;
                });
                if (!downloaded)
                {
                    if (syncOk.size() > 4096u)
                        syncOk.clear();
                    syncOk[query] = signature;
                }
                return;
            }
        }
        forRectPages(basePage, straddles, bw, psm, x0, y0, x1, y1, [&](uint32_t page) {
            if (owner[page] >= 0)
                downloadPage(page);
        });
    }

    void bumpPages(uint32_t basePage, bool straddles, uint32_t bw, uint32_t psm, int x0, int y0, int x1, int y1)
    {
        forRectPages(basePage, straddles, bw, psm, x0, y0, x1, y1, [&](uint32_t page) {
            if (owner[page] >= 0)
                downloadPage(page);
            ++cpuVersion[page];
            pageChangedAt[page] = ++cpuVersionEpoch;
            pageChanged(page);
        });
    }

    void syncAllToCpu()
    {
        for (uint32_t page = 0; page < kPageCount; ++page)
            if (owner[page] >= 0)
                downloadPage(page);
    }

    // The scale-dependent choices: fast mode (GT3_GS_GL_FAST=1/0, default from
    // 8 samples per pixel: 2x progressive, 3x, 4x; never native; plain 2x
    // holds 60 exact) and the interlock (GT3_GS_GL_INTERLOCK=1/0, default up
    // to 4 samples per pixel). 2x progressive (2 x 4 samples) exact spent a
    // fifth of the GS GL thread ordering overlapping blends into levels:
    // 53-56 fps at the Trial Mountain and Smokey Mountain starts; fast 60.
    int scaleRequest = 1;       // the requested scale last applied (scaleX may be lower: texture size limit)
    int outRows = 1;            // presented rows per display line (scaleX unless split for a wide picture)
    bool interlockActive = false;
    // GT4's 480p menus at 3x/4x (analysis/design/car-select-fps/REPORT.md): the car pages drew ~410 overlap levels a
    // frame (each an in-pass barrier). GT4_MENU_480P_CAR_ILOCK=1 (experiment only): while the car view is armed,
    // those batches take the interlock instead. Off: the interlock program tests depth in its shader, and GT4's
    // reflection pass over the car (GEQUAL against the car pass's hardware depth, deep Z) then speckled and lost
    // the window reflections. What is on instead: orderFreeMasks below.
    bool menuInterlock = false;
    // A batch into a full-height car-view target that writes only alpha bits (FBMSK keeps the whole RGB and the
    // same bits for every primitive), no DATE, no alpha-fail pass, no depth writes: the pixel it leaves is the
    // kept bits (which nothing in the batch changes) and bits from the source alpha alone, so the order of its
    // primitives does not matter and it is drawn as one level (rasterization order still lands the last write).
    // GT4's shadow planes are such batches (alpha bit 7, 6, ... one pass each). GT4_MENU_480P_CAR_ORDERFREE=0:
    // overlap levels as before.
    bool orderFreeMasks = false;
    uint64_t statOrderFreeBatches = 0;
    // ... and in fast mode (GT3_GS_GL_FAST, never exact drawing) at 3x/4x a linear read of a full-height target
    // (3 x 6 / 4 x 8 samples per GS pixel) takes fast mode's GPU filter (8-bit weights), as every target read
    // already does from 16 samples per pixel (fastTargetsActive, 4x). With orderFreeMasks: 3x car page ~52 -> ~59 fps.
    // GT4_MENU_480P_CAR_FAST_READS=0: the shader's exact filter as before.
    bool tallFastReads = false;
    bool useInterlock() const { return interlockActive || (menuInterlock && tallArmedSeen); }
    void applyScalePolicies()
    {
        const char *fast = std::getenv("GT3_GS_GL_FAST");
        fastBlend = scaled() && ((fast && *fast) ? std::strcmp(fast, "0") != 0 : scaleX * scaleY >= 8);
        const char *ft = std::getenv("GT3_GS_GL_FAST_TEXTURES");
        fastTexturesActive = fastBlend && colourProgram && ((ft && *ft) ? std::strcmp(ft, "0") != 0 : scaleX * scaleY >= 16);
        const char *ftg = std::getenv("GT3_GS_GL_FAST_TARGETS");
        fastTargetsActive = fastBlend && linearSampler && ((ftg && *ftg) ? std::strcmp(ftg, "0") != 0 : scaleX * scaleY >= 16);
        const char *il = std::getenv("GT3_GS_GL_INTERLOCK");
        interlockActive = interlockProgram && ((il && *il) ? std::strcmp(il, "0") != 0 : scaleX * scaleY <= 4);
        // GT4_EFFECTS_SCALE=2..4 (Full edition: Display, "Effects resolution"; analysis/design/effects-scale):
        // effect targets at that many samples per GS pixel when it is below the picture's (not progressive).
        // 0 / unset / at or above the picture's: Normal (every target at the picture's scale, as before).
        effectScale = 0;
#if defined(PS2X_GAME_GT4) && !defined(GT4_STOCK_BUILD)
        if (const char *es = std::getenv("GT4_EFFECTS_SCALE"); es && *es)
        {
            const int v = std::atoi(es);
            if (v >= 2 && v < scaleX && scaleX == scaleY)
                effectScale = v;
        }
        if (dlssHudRender)
        {
            if (dlssHudRender < scaleX && scaleX == scaleY)
                effectScale = dlssHudRender;
            else
            {
                std::fprintf(stderr, "[dlss] the HUD-after-DLSS mode needs a square picture scale (%dx%d): off\n", scaleX, scaleY);
                dlssHudRender = 0;
            }
        }
#endif
        if (effectScale)
            std::cerr << "[gs-gl] effects scale: effect targets at " << effectScale << "x inside the " << scaleX << "x picture (GT4_EFFECTS_SCALE)"
                      << std::endl;
        // GT4's car view at full height in the 480p menus (tallOn): 2 x scaleY samples down a GS pixel, at most 8
        // (the programs' scale table).
        tallOn = tallPossible() && !progressive && !dlssHudRender && scaleX >= 2 && scaleY >= 2 && scaleY <= 4;
        gt4carview::g_tallActive.store(tallOn, std::memory_order_relaxed);
        menuInterlock = tallOn && interlockProgram && envFlag("GT4_MENU_480P_CAR_ILOCK", false);
        orderFreeMasks = tallOn && envFlag("GT4_MENU_480P_CAR_ORDERFREE", true);
        tallFastReads = tallOn && fastBlend && linearSampler && scaleX >= 3 && envFlag("GT4_MENU_480P_CAR_FAST_READS", true);
        if (tallOn)
            std::fprintf(stderr, "[gs-gl] GT4's 480p menus at %dx: interlock %d (picture's policy %d), alpha-bit batches in one level %d, "
                                 "fast reads of full-height targets %d\n",
                         scaleX, menuInterlock ? 1 : 0, interlockActive ? 1 : 0, orderFreeMasks ? 1 : 0, tallFastReads ? 1 : 0);
        if (tallPossible())
        {
            static int said = 0;
            if (said++ < 4)
            {
                if (tallOn)
                    std::fprintf(stderr, "[gs-gl] GT4's 480p menus: the car view's 224-row targets at %d x %d samples per GS pixel (full height)\n", scaleX,
                                 2 * scaleY);
                else
                    std::fprintf(stderr, "[gs-gl] GT4's 480p menus: the car view's 224-row targets as GT4 draws them (picture scale %d x %d)\n", scaleX,
                                 scaleY);
            }
        }
        // GT3_GS_GL_FAST_RASTER=0: fast mode keeps the exact coverage program.
        const char *fr = std::getenv("GT3_GS_GL_FAST_RASTER");
        fastRasterActive = fastBlend && fastProgram && !(fr && std::strcmp(fr, "0") == 0);
    }
    void chooseScale(int requested, bool progressiveRequested)
    {
        scaleRequest = requested;
        progressive = progressiveRequested;
        int sx = requested, sy = requested;
        gsgl::wideScaleSplit(requested, sx, sy);
        if (sx == sy)
        {
            // Targets up to 2048 GS pixels wide (the GS window; GT3's are at
            // most 1024) and 2048 rows (twice that progressive) must fit.
            scaleX = requested;
            while (scaleX > 1 && (2048 * scaleX > maxTextureSize || 2048 * scaleX * (progressive ? 2 : 1) > maxTextureSize))
                --scaleX;
            scaleY = progressive ? 2 * scaleX : scaleX;
        }
        else
        {
            // Wide split (more samples across): targets up to 2048 GS pixels
            // wide here (GT3's are at most 1024), so AMD's 16384 allows 8.
            while (sx > 1 && 2048 * sx > maxTextureSize)
                --sx;
            while (sy > 1 && 2048 * sy * (progressive ? 2 : 1) > maxTextureSize)
                --sy;
            scaleX = sx;
            scaleY = progressive ? 2 * sy : sy;
        }
        outRows = progressive ? scaleY / 2 : scaleY;
        // Sample coordinates of targets up to 4096 x 2048 GS pixels.
        sampleCellLimit = scaled() ? (4096 * std::max(scaleX, scaleY)) / 8 - 1 : 255;
    }

    // The internal resolution changed while the game runs (live settings):
    // every page the GPU holds goes back to the local memory copy, then every
    // texture held at the old scale is dropped; targets come back at the new
    // scale as they are drawn or read (from that copy), as at startup.
    void rescale(int requested, bool progressiveRequested)
    {
        resetRuns(); // (step 7 T1)
        flushBatch();
        materializeClut();
        syncAllToCpu();
        gpuClutActive = false;
        clutDirect = false;
        clutBlockWriter.fill(0ull); // the GPU CLUT copies go: loads run again
        for (auto &t : targets)
            if (t)
                releaseTexture(t->tex);
        clearTargetPool();
        targets.clear();
        targetIndex.clear();
        texScales.clear();
        targetTexs.clear();
        owner.fill(-1);
        highPlane.fill(-1);
        if (pageView)
        {
            releaseTexture(pageView->tex);
            pageView.reset();
        }
        releaseTexture(texScratch);
        texScratchW = texScratchH = 0;
        releaseTexture(dstScratch);
        for (GLuint &t : clutRing)
            releaseTexture(t);
        clutRing.clear();
        clutRingNext = 0;
        gpuClutTex = 0;
        for (GLuint &t : fieldTex)
            releaseTexture(t);
        for (uint64_t &t : fieldTick)
            t = ~0ull;
        fieldRing = 2;
        fieldCur = 0;
        selfSnap.valid = false;
        chooseScale(hudOutputScale(requested), progressiveRequested);
        scaleRequest = requested; // (with DLSS the picture's scale is its output's: not a new request)
        applyScalePolicies();
        resetRuns(); // (step 7 T1: owners and targets went)
        if (presentProgram)
        {
            fieldTex[0] = makeColorTexture(640 * scaleX, 512 * scaleY);
            fieldTex[1] = makeColorTexture(640 * scaleX, 512 * scaleY);
        }
        if (!scaled() || !textureBarrier)
            dstScratch = makeColorTexture(2048 * scaleX, 2048 * scaleY);
        if (!scaled())
        {
            texScratch = makeColorTexture(2048, 2048);
            texScratchW = texScratchH = 2048;
        }
        forgetBindings();
        glStateDirty = true;
        applyPresentScale();
        std::cerr << "[gs-gl] internal resolution now " << scaleX << " x " << scaleY << " samples per GS pixel"
                  << (progressive ? " (progressive)" : "") << (fastBlend ? ", fast mode" : "") << (interlockActive ? ", interlock" : "")
                  << std::endl;
    }

    // ------------------------------------------------------------ textures

    // The CLUT entries a paletted texture indexes (as lookupClutPipe; TEXA applied).
    void buildPalette(const GSDrawState &state, uint32_t palette[256])
    {
        const GSTex0Reg &tex = state.context.tex0;
        const bool fourBit = tex.psm == GS_PSM_T4 || tex.psm == GS_PSM_T4HL || tex.psm == GS_PSM_T4HH;
        const auto &clut = cpu->clutBuffer();
        const uint32_t count = fourBit ? 16u : 256u;
        for (uint32_t i = 0; i < 256u; ++i)
        {
            if (i >= count)
                palette[i] = 0u;
            else if (tex.cpsm == GS_PSM_CT16 || tex.cpsm == GS_PSM_CT16S)
                palette[i] = applyTexa(state.texa, tex.cpsm, rgba5551To8888(clut[(static_cast<uint32_t>(tex.csa) * 16u + i) & 0x1FFu]));
            else if (tex.cpsm == GS_PSM_CT32 || tex.cpsm == GS_PSM_CT24)
            {
                const uint32_t entry = (static_cast<uint32_t>(tex.csa & 0x0Fu) * 16u + i) & 0xFFu;
                palette[i] = applyTexa(state.texa, tex.cpsm, static_cast<uint32_t>(clut[entry]) | (static_cast<uint32_t>(clut[entry + 256u]) << 16u));
            }
            else
                palette[i] = 0xFFFF00FFu;
        }
    }

#if defined(GS_TEXTURE_DUMP)
    // Textures from GS memory known by the picture they hold (the hash of
    // their decoded texels and size: the same picture wherever and however
    // GT3 loads it), for GT3_TEXTURE_DUMP (gt3_texture_dump.h) and
    // GT3_TEXTURE_REPLACE (gt3_texture_replace.h). Only textures whose pages
    // are current in the CPU copy (not ones GT3 drew: a target holds those).
    // A texture is decoded (declared size, palette and TEXA, as
    // decodedTexture) only when its pages' contents or palette are new.
    struct TexIdent
    {
        uint64_t content = 0, hash = 0; // hash 0: not identified (video)
    };
    std::unordered_map<uint64_t, TexIdent> texIdents; // (address, format, size, palette or its version) -> pages' content, hash
    struct DumpRate
    {
        uint64_t lastFrame = 0, videoUntil = 0;
        int burst = 0;
    };
    std::unordered_map<uint64_t, DumpRate> dumpRates; // (address, format, size) -> how often new contents come
    // The texel hash (0: none - too small, a lagging palette, video); the
    // texels when decoded now and `texelsOut` is given.
    uint64_t textureIdentity(const GSDrawState &state, std::vector<uint32_t> *texelsOut)
    {
        const GSTex0Reg &tex = state.context.tex0;
        const int w = std::min<int>(state.textureWidth, 1024), h = std::min<int>(state.textureHeight, 1024);
        if (w < 8 || h < 8)
            return 0;
        const uint32_t psm = tex.psm;
        const bool fourBit = psm == GS_PSM_T4 || psm == GS_PSM_T4HL || psm == GS_PSM_T4HH;
        const bool paletted = fourBit || psm == GS_PSM_T8 || psm == GS_PSM_T8H;
        if (paletted && lookupReadsStaleClut(tex))
            return 0; // the CPU's palette lags: identified when it is current
        uint64_t content = 0x9E3779B97F4A7C15ull;
        forRectPages(tex.tbp0 >> 5u, (tex.tbp0 & 31u) != 0u, tex.tbw, psm, 0, 0, w - 1, h - 1, [&](uint32_t page) {
            const uint64_t ph = pageContentHash(page);
            content = hashBytes(&ph, sizeof(ph), content);
        });
        const uint32_t fields[] = {tex.tbp0, tex.tbw, psm, static_cast<uint32_t>(w), static_cast<uint32_t>(h), state.texa.ta0,
                                   state.texa.aem ? 1u : 0u, state.texa.ta1, paletted ? tex.cpsm : 0u, paletted ? tex.csa : 0u};
        const uint64_t place = hashBytes(fields, sizeof(fields), 0x44554D50ull);
        // Quick: the same draw state and palette version as a texture seen
        // with these contents (no palette built).
        const uint64_t quick = paletted ? hashBytes(&clutVersion, sizeof(clutVersion), place) : place;
        if (auto q = texIdents.find(quick); q != texIdents.end() && q->second.content == content)
            return q->second.hash;
        uint32_t palette[256];
        uint64_t key = quick;
        if (paletted)
        {
            buildPalette(state, palette);
            key = hashBytes(palette, (fourBit ? 16u : 256u) * sizeof(uint32_t), place);
            if (auto it = texIdents.find(key); it != texIdents.end() && it->second.content == content)
            {
                texIdents[quick] = it->second;
                return it->second.hash;
            }
        }
        if (texIdents.size() > 200000u)
            texIdents.clear(); // bounded
        // Video (the menus' moving backgrounds: new contents at the same
        // place nearly every frame; 345 dump files in one test) is not
        // identified: a place with new contents in 6 frames in a row, each
        // within 4 frames of the last, is skipped until quiet for 300 frames.
        // (Several textures loaded into one place within a frame count once.)
        {
            const uint32_t spot[] = {tex.tbp0, psm, static_cast<uint32_t>(w), static_cast<uint32_t>(h)};
            DumpRate &r = dumpRates[hashBytes(spot, sizeof(spot), 0x564944ull)];
            if (frameCounter != r.lastFrame)
            {
                r.burst = frameCounter - r.lastFrame <= 4u ? r.burst + 1 : 0;
                r.lastFrame = frameCounter;
                if (r.burst >= 6)
                {
                    if (r.videoUntil <= frameCounter)
                        std::fprintf(stderr, "[texture-dump] %04x psm %02x %dx%d changes every frame (video): not saved or replaced\n", tex.tbp0,
                                     psm, w, h);
                    r.videoUntil = frameCounter + 300u;
                }
            }
            if (frameCounter < r.videoUntil)
            {
                texIdents[quick] = TexIdent{content, 0};
                return 0;
            }
        }
        std::vector<uint32_t> texels(static_cast<size_t>(w) * static_cast<size_t>(h));
        const uint32_t tbw = std::max<uint32_t>(tex.tbw, 1u);
        for (int v = 0; v < h; ++v)
            for (int u = 0; u < w; ++u)
            {
                const uint32_t raw = readTexel(vram, psm, tex.tbp0, tbw, static_cast<uint32_t>(u), static_cast<uint32_t>(v));
                uint32_t value;
                if (fourBit)
                    value = palette[raw & 0x0Fu];
                else if (paletted)
                    value = palette[raw & 0xFFu];
                else if (psm == GS_PSM_CT16 || psm == GS_PSM_CT16S || psm == GS_PSM_Z16 || psm == GS_PSM_Z16S)
                    value = applyTexa(state.texa, psm, rgba5551To8888(raw));
                else
                    value = applyTexa(state.texa, psm, raw);
                texels[static_cast<size_t>(v) * static_cast<size_t>(w) + static_cast<size_t>(u)] = value;
            }
        const int32_t size[2] = {w, h};
        const uint64_t hash = hashBytes(texels.data(), texels.size() * sizeof(uint32_t), hashBytes(size, sizeof(size)));
        texIdents[quick] = TexIdent{content, hash};
        texIdents[key] = TexIdent{content, hash};
        if (texelsOut)
            *texelsOut = std::move(texels);
        return hash;
    }

    // GT3_TEXTURE_DUMP: the texture saved once (gt3_texture_dump.h).
    void dumpTexture(const GSDrawState &state, uint64_t hash, std::vector<uint32_t> &texels)
    {
        if (!hash || texels.empty() || gt3texdump::known(hash))
            return;
        const GSTex0Reg &tex = state.context.tex0;
        const int w = std::min<int>(state.textureWidth, 1024), h = std::min<int>(state.textureHeight, 1024);
        const bool paletted = tex.psm == GS_PSM_T4 || tex.psm == GS_PSM_T4HL || tex.psm == GS_PSM_T4HH || tex.psm == GS_PSM_T8 || tex.psm == GS_PSM_T8H;
        char info[160];
        std::snprintf(info, sizeof(info), "psm%02x,%04x,%u,%04x,%u,%llu", tex.psm, tex.tbp0, tex.tbw, paletted ? tex.cbp : 0u,
                      paletted ? static_cast<unsigned>(tex.csa) : 0u, static_cast<unsigned long long>(frameCounter));
        gt3texdump::submit(hash, w, h, std::move(texels), info);
    }
#if defined(PS2X_GAME_GT4)
    // GT4_TEXTURE_REPLACE: replacements are named by the dumped picture (the texels of the area GT4 samples, the rest
    // cleared: gt4texdump::maskedHash), so a texture's own hash (all its texels) is matched once, when it is first
    // decoded, against the areas the replacement folder names for its size. 0: none.
    std::unordered_map<uint64_t, uint64_t> replaceKeys; // texture hash -> replacement's hash (0: none)
    uint64_t replaceKey(const GSDrawState &state, uint64_t id, const std::vector<uint32_t> &texels)
    {
        if (!id)
            return 0;
        if (auto it = replaceKeys.find(id); it != replaceKeys.end())
            return it->second;
        const int w = std::min<int>(state.textureWidth, 1024), h = std::min<int>(state.textureHeight, 1024);
        if (texels.size() != static_cast<size_t>(w) * static_cast<size_t>(h))
            return 0; // not decoded now (asked again when it is)
        uint64_t key = 0;
        for (const gt4texreplace::Rect &r : gt4texreplace::rects(w, h))
        {
            const uint64_t m = gt4texdump::maskedHash(texels.data(), w, h, r.u0, r.v0, r.u1, r.v1);
            if (gt4texreplace::has(m))
            {
                key = m;
                static int said = 0;
                if (said++ < 20)
                    std::fprintf(stderr, "[texture-replace] texture %016llx (%04x psm %02x %dx%d) is %016llx\n", static_cast<unsigned long long>(id),
                                 state.context.tex0.tbp0, state.context.tex0.psm, w, h, static_cast<unsigned long long>(m));
                break;
            }
        }
        if (replaceKeys.size() > 200000u)
            replaceKeys.clear(); // bounded
        replaceKeys.emplace(id, key);
        return key;
    }
#endif

    // GT3_TEXTURE_REPLACE: the replacement picture for a texel hash as a GL
    // texture (0: none, or still loading - the original is drawn meanwhile).
    static constexpr GLuint kReplacePending = ~0u;
    std::unordered_map<uint64_t, GLuint> replacements; // hash -> texture, 0 none, kReplacePending loading
    GLuint replacementFor(uint64_t hash)
    {
        if (!hash)
            return 0;
        auto it = replacements.find(hash);
        if (it == replacements.end())
        {
            if (!gt3texreplace::has(hash))
            {
                replacements.emplace(hash, 0u);
                return 0;
            }
            it = replacements.emplace(hash, kReplacePending).first;
        }
        if (it->second != kReplacePending)
            return it->second;
        int w = 0, h = 0;
        std::vector<uint32_t> rgba;
        if (!gt3texreplace::take(hash, w, h, rgba))
            return 0;
        GLuint t = 0;
        glGenTextures(1, &t);
        glBindTexture(GL_TEXTURE_2D, t);
        glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA8, w, h);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, rgba.data());
        glBindTexture(GL_TEXTURE_2D, 0);
        it->second = t;
        static int logged = 0;
        if (logged++ < 20)
            std::fprintf(stderr, "[texture-replace] %016llx: %dx%d in use\n", static_cast<unsigned long long>(hash), w, h);
        return t;
    }
#endif

    // Whether a paletted lookup with this TEX0 reads a lagging CPU CLUT
    // halfword (the entries buildPalette reads).
    bool lookupReadsStaleClut(const GSTex0Reg &tex) const
    {
        const bool fourBit = tex.psm == GS_PSM_T4 || tex.psm == GS_PSM_T4HL || tex.psm == GS_PSM_T4HH;
        const uint32_t count = fourBit ? 16u : 256u;
        if (tex.cpsm == GS_PSM_CT16 || tex.cpsm == GS_PSM_CT16S)
        {
            for (uint32_t i = 0; i < count; ++i)
                if (clutStale[(static_cast<uint32_t>(tex.csa) * 16u + i) & 0x1FFu])
                    return true;
        }
        else if (tex.cpsm == GS_PSM_CT32 || tex.cpsm == GS_PSM_CT24)
        {
            for (uint32_t i = 0; i < count; ++i)
            {
                const uint32_t entry = (static_cast<uint32_t>(tex.csa & 0x0Fu) * 16u + i) & 0xFFu;
                if (clutStale[entry] || clutStale[entry + 256u])
                    return true;
            }
        }
        return false;
    }

    // The CPU backend's CLUT buffer brought up to date (after GPU CLUT loads):
    // its lagging halfwords read back from the GPU CLUT, always, or with a
    // TEX0 (need) only when that lookup reads one of them.
    void materializeClut(const GSTex0Reg *need = nullptr)
    {
        if (clutStale.none() || (need && !lookupReadsStaleClut(*need)))
            return;
        ++statClutMaterialize;
        // GT3_CLUT_LOG=<n>: the first n read backs.
        static long clutLog = [] {
            const char *v = std::getenv("GT3_CLUT_LOG");
            return v ? std::strtol(v, nullptr, 10) : 0L;
        }();
        if (clutLog > 0)
        {
            --clutLog;
            if (need)
                std::fprintf(stderr, "[clut] read back %zu halfwords for a lookup: cpsm %02x psm %02x csa %u (gpu cpsm %02x)\n",
                             clutStale.count(), need->cpsm, need->psm, need->csa, gpuClutCpsm);
            else
                std::fprintf(stderr, "[clut] read back %zu halfwords for a CPU draw or reset\n", clutStale.count());
        }
        // From the GPU copy taken at load time (its source may have changed
        // since): 256 entries at CSA 0 in CSM1 order.
        // (At a scale: each entry's middle sample.)
        materializeDirectClut(); // (the read back below is of a 16x16 CLUT texture)
        std::vector<uint32_t> samples(static_cast<size_t>(16 * scaleX) * static_cast<size_t>(16 * scaleY));
        glBindTexture(GL_TEXTURE_2D, gpuClutTex);
        glGetTexImage(GL_TEXTURE_2D, 0, GL_RGBA, GL_UNSIGNED_BYTE, samples.data());
        glBindTexture(GL_TEXTURE_2D, 0);
        forgetBindings();
        uint32_t texels[16 * 16];
        const TexScale cs = scaleOfTex(gpuClutTex); // (effects at their own scale; rows stay 16 * scaleX wide)
        for (int y = 0; y < 16; ++y)
            for (int x = 0; x < 16; ++x)
                texels[y * 16 + x] = samples[static_cast<size_t>(y * cs.y + cs.y / 2) * static_cast<size_t>(16 * scaleX) +
                                             static_cast<size_t>(x * cs.x + cs.x / 2)];
        std::array<uint16_t, 512> clut = cpu->clutBuffer();
        for (uint32_t j = 0; j < 256u; ++j)
        {
            const uint32_t pos = (j & ~0x18u) | ((j & 0x08u) << 1u) | ((j & 0x10u) >> 1u);
            const uint32_t value = texels[pos];
            if (clutStale[j])
                clut[j] = static_cast<uint16_t>(value);
            if (clutStale[j + 256u])
                clut[j + 256u] = static_cast<uint16_t>(value >> 16u);
        }
        cpu->SetClutBuffer(clut);
        clutStale.reset();
        // Palettes are cached by a hash of the whole buffer, taken once per
        // version: the read back halfwords need a new one.
        ++clutVersion;
    }

    // A direct GPU CLUT (see clutDirect) into a CLUT ring texture: its source
    // target is about to be written (or the CLUT is read some other way).
    // (Also called from flushBatch before its draws: the ring slot is one
    // the batch does not read, never a flush.)
    void materializeDirectClut()
    {
        if (!clutDirect)
            return;
        touchResolve(); // (step 7 T1: gpuClutTex changes)
        clutDirect = false; // (before the copy: copyScaled's own check)
        if (clutRing.empty())
            for (int i = 0; i < 64; ++i)
                clutRing.push_back(Impl::makeColorTexture(16 * scaleX, 16 * scaleY));
        auto readByBatch = [&](GLuint t) { return std::find(batchTextures.begin(), batchTextures.end(), t) != batchTextures.end(); };
        for (size_t tries = 0; tries < clutRing.size() && readByBatch(clutRing[clutRingNext]); ++tries)
            clutRingNext = (clutRingNext + 1u) % clutRing.size();
        const GLuint clut = clutRing[clutRingNext];
        clutRingNext = (clutRingNext + 1u) % clutRing.size();
        copyScaled(gpuClutTex, clutOriginX, clutOriginY, clut, 0, 0, 16, 16);
        gpuClutTex = clut;
        clutOriginX = clutOriginY = 0;
    }

    // A 256-colour CT16 CLUT (CSM1, CSA 0) whose words target `holder` holds,
    // copied into a CLUT ring texture on the GPU (R, G: the halfword's low and
    // high byte; every sample of each entry at a scale). False: not here.
    bool gpuClut16(const GSTex0Reg &tex, int holder)
    {
        Target &t = *targets[holder];
        if (t.depth || (t.layout != Layout::C32 && t.layout != Layout::Z32) || !layoutTablesOk || clut16Program == ~0u)
            return false;
        uint32_t lut[256];
        const bool s = tex.cpsm == GS_PSM_CT16S;
        for (uint32_t pos = 0; pos < 256u; ++pos)
        {
            const uint32_t x = pos & 15u, y = pos >> 4u;
            const uint32_t half = s ? GSMem::PixelStorageTraits<GSMem::C16S>::Address(GSMem::PageTableC16S, tex.cbp, 1u, x, y)
                                    : GSMem::PixelStorageTraits<GSMem::C16>::Address(GSMem::PageTableC16, tex.cbp, 1u, x, y);
            const uint32_t word = (half >> 1u) & 0xFFFFFu, page = (word >> 11u) & (kPageCount - 1u);
            int px = 0, py = 0;
            if (owner[page] != holder || !t.pageRect(page, px, py))
                return false;
            const uint16_t pixel = (t.layout == Layout::Z32 ? pixelOfWordZ : pixelOfWordC)[word & 2047u];
            if (pixel >= 2048u)
                return false;
            lut[pos] = static_cast<uint32_t>(px + (pixel & 63u)) | (static_cast<uint32_t>(py + (pixel >> 6u)) << 12u) | ((half & 1u) << 24u);
        }
        if (!clut16Program)
        {
            const std::string cs = R"GLSL(#version 430 core
layout(local_size_x = 8, local_size_y = 8) in;
uniform sampler2D u_src;  // the target holding the CLUT's words
uniform usampler2D u_lut; // per CLUT position: x | y << 12 | high half << 24
uniform ivec2 u_scale;
layout(rgba8, binding = 0) writeonly uniform image2D u_out;
void main()
{
    ivec2 g = ivec2(gl_GlobalInvocationID.xy);
    if (g.x >= 16 * u_scale.x || g.y >= 16 * u_scale.y)
        return;
    ivec2 e = g / u_scale;
    uint v = texelFetch(u_lut, e, 0).r;
    vec4 c = texelFetch(u_src, ivec2(int(v & 0xFFFu), int((v >> 12) & 0xFFFu)) * u_scale + (g - e * u_scale), 0);
    imageStore(u_out, g, (v >> 24) != 0u ? vec4(c.b, c.a, 0.0, 0.0) : vec4(c.r, c.g, 0.0, 0.0));
}
)GLSL";
            const GLuint shader = compile(GL_COMPUTE_SHADER, cs);
            GLint linked = 0;
            if (shader)
            {
                clut16Program = glCreateProgram();
                glAttachShader(clut16Program, shader);
                glLinkProgram(clut16Program);
                glDeleteShader(shader);
                glGetProgramiv(clut16Program, GL_LINK_STATUS, &linked);
            }
            if (!linked)
            {
                if (clut16Program)
                    glDeleteProgram(clut16Program);
                clut16Program = ~0u;
                std::fprintf(stderr, "[gs-gl] CT16 GPU CLUT program failed: such CLUTs are read back\n");
                return false;
            }
            const char *names[4] = {"u_src", "u_lut", "u_scale", "u_out"};
            for (int i = 0; i < 4; ++i)
                clut16Loc[i] = glGetUniformLocation(clut16Program, names[i]);
            glGenTextures(1, &clut16Lut);
            glBindTexture(GL_TEXTURE_2D, clut16Lut);
            glTexStorage2D(GL_TEXTURE_2D, 1, GL_R32UI, 16, 16);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
            glBindTexture(GL_TEXTURE_2D, 0);
        }
        if (clutRing.empty())
            for (int i = 0; i < 64; ++i)
                clutRing.push_back(Impl::makeColorTexture(16 * scaleX, 16 * scaleY));
        leaveDirectClut(); // (gpuClutTex becomes this copy)
        const GLuint clut = clutRing[clutRingNext];
        // The copy must see queued draws into its source (and the lookup
        // table's previous contents their dispatch), and must not replace a
        // CLUT the queued draws still use.
        if (haveBatch && (key.colorTarget == holder || batchUses(clut)))
            flushBatch();
        clutRingNext = (clutRingNext + 1u) % clutRing.size();
        glBindTexture(GL_TEXTURE_2D, clut16Lut);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 16, 16, GL_RED_INTEGER, GL_UNSIGNED_INT, lut);
        glBindTexture(GL_TEXTURE_2D, 0);
        glUseProgram(clut16Program);
        glActiveTexture(GL_TEXTURE13);
        glBindTexture(GL_TEXTURE_2D, t.tex);
        glActiveTexture(GL_TEXTURE14);
        glBindTexture(GL_TEXTURE_2D, clut16Lut);
        glActiveTexture(GL_TEXTURE0);
        glUniform1i(clut16Loc[0], 13);
        glUniform1i(clut16Loc[1], 14);
        // (Effects at their own scale: the CLUT holds the samples of its target's scale.)
        glUniform2i(clut16Loc[2], t.sx, t.sy);
        if (!texScales.empty() || !targetTexs.empty())
            setTexScale(clut, {t.sx, t.sy});
        glBindImageTexture(0, clut, 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_RGBA8);
        glDispatchCompute(static_cast<GLuint>((16 * t.sx + 7) / 8), static_cast<GLuint>((16 * t.sy + 7) / 8), 1);
        glMemoryBarrier(GL_TEXTURE_FETCH_BARRIER_BIT | GL_TEXTURE_UPDATE_BARRIER_BIT);
        glBindImageTexture(0, 0, 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_RGBA8);
        glUseProgram(0);
        forgetBindings();
        glStateDirty = true;
        gpuClutActive = true;
        gpuClutTex = clut;
        gpuClutCpsm = tex.cpsm;
        for (uint32_t j = 0; j < 256u; ++j)
            clutStale.set(j);
        ++clutVersion;
        ++statGpuCluts;
        ++statGpuCluts16;
        return true;
    }

    // A CLUT that is not direct replaces a direct one: the queued draws that
    // read the direct one go first (they lose its protection).
    void leaveDirectClut()
    {
        if (!clutDirect)
            return;
        touchResolve(); // (step 7 T1)
        if (haveBatch && batchUses(gpuClutTex))
            flushBatch();
        clutDirect = false;
        clutOriginX = clutOriginY = 0;
    }

    // Before anything outside flushBatch writes texture tex: a direct CLUT
    // read from it is copied first, after the queued draws that read it.
    // Before anything outside flushBatch writes texture tex (copies into it,
    // page uploads and moves, upload patches): a direct CLUT read from it is
    // kept, and a self-read copy of it is no longer trusted (it tracks only
    // the batches' own draws).
    void beforeTextureWrite(GLuint tex)
    {
        if (selfSnap.valid && selfSnap.target >= 0 && static_cast<size_t>(selfSnap.target) < targets.size() && targets[selfSnap.target] &&
            targets[selfSnap.target]->tex == tex)
            selfSnap.valid = false;
        protectDirectClut(tex);
    }
    void protectDirectClut(GLuint tex)
    {
        if (!clutDirect || tex != gpuClutTex)
            return;
        if (haveBatch && batchUses(tex))
            flushBatch();
        materializeDirectClut();
    }

    // Sets a draw key's palette: the GPU CLUT when the last load came from a
    // GPU target, else the resolved CPU CLUT.
    void choosePalette(DrawKey &k, const GSDrawState &state)
    {
        const GSTex0Reg &tex = state.context.tex0;
        noteClutRead(tex);
        const bool fourBit = tex.psm == GS_PSM_T4 || tex.psm == GS_PSM_T4HL || tex.psm == GS_PSM_T4HH;
        // A draw into the direct CLUT's own target reads the copy (not the
        // texture it writes).
        if (clutDirect && gpuClutActive && k.colorTarget >= 0 && targets[k.colorTarget] && targets[k.colorTarget]->tex == gpuClutTex)
            protectDirectClut(gpuClutTex);
        // A CT16 GPU CLUT holds halfwords 0-255 (a CSA 0 load): other
        // lookups read the CPU buffer.
        const bool clut16 = gpuClutCpsm == GS_PSM_CT16 || gpuClutCpsm == GS_PSM_CT16S;
        const bool gpuHolds = !clut16 || (fourBit ? tex.csa < 16u : tex.csa == 0u);
        if (gpuClutActive && tex.cpsm == gpuClutCpsm && gpuHolds)
        {
            k.paletteMode = 1;
            k.paletteTex = gpuClutTex;
            k.clutOrgX = clutOriginX;
            k.clutOrgY = clutOriginY;
            k.clutCsa = tex.csa;
            k.clutFour = fourBit ? 1 : 0;
            k.clutCt24 = clut16 ? 2 : tex.cpsm == GS_PSM_CT24 ? 1 : 0;
        }
        else
        {
            materializeClut(&tex);
            k.paletteMode = 0;
            k.clutOrgX = k.clutOrgY = 0;
            k.paletteTex = paletteTexture(state);
        }
    }

    // Indices (0-255) of a paletted texture held in the CPU copy, cached by
    // content: palette changes do not need a new decode.
    GLuint decodedIndexTexture(const GSDrawState &state, int decodeW, int decodeH)
    {
        const GSTex0Reg &tex = state.context.tex0;
        const uint32_t psm = tex.psm;
        uint64_t id = hashBytes(&tex.tbp0, sizeof(tex.tbp0), 0x494E444558ull);
        const uint32_t fields[] = {tex.tbw, psm, static_cast<uint32_t>(decodeW), static_cast<uint32_t>(decodeH)};
        id = hashBytes(fields, sizeof(fields), id);
        uint64_t signature = 0x9E3779B97F4A7C15ull;
        forRectPages(tex.tbp0 >> 5u, (tex.tbp0 & 31u) != 0u, tex.tbw, psm, 0, 0, decodeW - 1, decodeH - 1, [&](uint32_t page) {
            signature = hashBytes(&cpuVersion[page], sizeof(uint32_t), hashBytes(&page, sizeof(page), signature));
        });
        TexEntry &entry = textures[id];
        entry.lastUse = frameCounter;
        if (entry.tex && entry.signature == signature && entry.width == decodeW && entry.height == decodeH)
            return entry.tex;
        if (batchUses(entry.tex))
            flushBatch(); // queued draws sample the old texels
        if (!entry.tex || entry.width != decodeW || entry.height != decodeH)
        {
            if (entry.tex)
            {
                if (batchUses(entry.tex))
                    flushBatch();
                releaseTexture(entry.tex);
            }
            glGenTextures(1, &entry.tex);
            glBindTexture(GL_TEXTURE_2D, entry.tex);
            glTexStorage2D(GL_TEXTURE_2D, 1, GL_R8, decodeW, decodeH);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
            entry.width = decodeW;
            entry.height = decodeH;
        }
        ScopedNs timer(statNs[2]);
        std::vector<uint8_t> indices(static_cast<size_t>(decodeW) * decodeH);
        const uint32_t tbw = std::max<uint32_t>(tex.tbw, 1u);
        for (int v = 0; v < decodeH; ++v)
            for (int u = 0; u < decodeW; ++u)
                indices[static_cast<size_t>(v) * decodeW + u] =
                    static_cast<uint8_t>(readTexel(vram, psm, tex.tbp0, tbw, static_cast<uint32_t>(u), static_cast<uint32_t>(v)));
        glBindTexture(GL_TEXTURE_2D, entry.tex);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, decodeW, decodeH, GL_RED, GL_UNSIGNED_BYTE, indices.data());
        glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
        glBindTexture(GL_TEXTURE_2D, 0);
        entry.signature = signature;
        ++statTexDecodes;
        return entry.tex;
    }

    // The palette as a 256x1 texture, cached by content.
    GLuint paletteTexture(const GSDrawState &state)
    {
        // The palette only changes with a CLUT load or the lookup parameters.
        const GSTex0Reg &tex = state.context.tex0;
        const bool fourBit = tex.psm == GS_PSM_T4 || tex.psm == GS_PSM_T4HL || tex.psm == GS_PSM_T4HH;
        const uint64_t quickKey = static_cast<uint64_t>(clutVersion) | (static_cast<uint64_t>(tex.csa & 31u) << 32) |
                                  (static_cast<uint64_t>(tex.cpsm & 63u) << 37) | (static_cast<uint64_t>(fourBit ? 1u : 0u) << 43) |
                                  (static_cast<uint64_t>(state.texa.ta0) << 44) | (static_cast<uint64_t>(state.texa.aem ? 1u : 0u) << 52) |
                                  (static_cast<uint64_t>(state.texa.ta1 & 0x7Fu) << 53);
        if (quickKey == lastPaletteKey && lastPaletteTex != 0 && (state.texa.ta1 >> 7) == lastPaletteTa1High)
        {
            lastPaletteEntry->lastUse = frameCounter;
            return lastPaletteTex;
        }
        // Recent palettes by key: draws that alternate between CLUT offsets
        // (4-bit textures) or CLUTs missed the single last-palette entry.
        PaletteSlot &slot = paletteSlots[(quickKey * 0x9E3779B97F4A7C15ull) >> 58];
        if (slot.key == quickKey && slot.tex != 0 && slot.ta1High == (state.texa.ta1 >> 7))
        {
            slot.entry->lastUse = frameCounter;
            lastPaletteKey = quickKey;
            lastPaletteTa1High = slot.ta1High;
            lastPaletteTex = slot.tex;
            lastPaletteEntry = slot.entry;
            return slot.tex;
        }
        // Keyed by the CLUT's content (hashed once per CLUT version) and the
        // lookup parameters: the palette is only built for a new texture.
        if (clutHashVersion != clutVersion)
        {
            const auto &clut = cpu->clutBuffer();
            clutHashValue = hashBytes(clut.data(), clut.size() * sizeof(clut[0]), 0x434C555448415348ull);
            clutHashVersion = clutVersion;
        }
        const uint64_t params[2] = {quickKey & ~0xFFFFFFFFull, static_cast<uint64_t>(state.texa.ta1)};
        const uint64_t id = hashBytes(params, sizeof(params), clutHashValue ^ 0x50414C4554544531ull);
        TexEntry &entry = textures[id];
        entry.lastUse = frameCounter;
        lastPaletteKey = quickKey;
        lastPaletteTa1High = state.texa.ta1 >> 7;
        if (!entry.tex)
        {
            uint32_t palette[256];
            buildPalette(state, palette);
            entry.tex = makeColorTexture(256, 1);
            entry.width = 256;
            entry.height = 1;
            glBindTexture(GL_TEXTURE_2D, entry.tex);
            glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
            glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 256, 1, GL_RGBA, GL_UNSIGNED_BYTE, palette);
            glBindTexture(GL_TEXTURE_2D, 0);
        }
        lastPaletteTex = entry.tex;
        lastPaletteEntry = &entry;
        slot = {quickKey, lastPaletteTa1High, entry.tex, &entry};
        return entry.tex;
    }
    struct PaletteSlot
    {
        uint64_t key = ~0ull;
        uint32_t ta1High = 0;
        GLuint tex = 0;
        TexEntry *entry = nullptr;
    };
    std::array<PaletteSlot, 64> paletteSlots{};
    uint32_t clutHashVersion = 0;
    uint64_t clutHashValue = 0;
    TexEntry *lastPaletteEntry = nullptr;
    uint64_t lastPaletteKey = ~0ull;
    GLuint lastPaletteTex = 0;
    uint32_t lastPaletteTa1High = 0;

    // GT3_GS_GL_SHADER_DEFINES=A,B (diagnostics): "#define A 1" ... ahead of the
    // draw shaders, for measuring shader variants (the GPU cost experiments
    // of 2026-09-28 used it with temporary #ifdef blocks).
    // Texture smoothing's shader part (GT4_TEX_SMOOTH=on, Full GT4 builds; analysis/design/tex-smooth, "The dot grid"):
    // GS_SHADE_EXACT, the Gouraud colour's slack (gs_gl_shaders.h shadeSlack), which the shader applies only while the
    // internal resolution is 2x or above (u_scale), so native stays as before even after a live change of resolution.
    static std::string texSmoothDefines()
    {
        // developer, 2026-10-10: the Gouraud slack in every GT4 build (Classic too) at every resolution: it is a fix towards the
        // GS's own fixed-point colour, not an enhancement, so it no longer waits for GT4_TEX_SMOOTH.
#if defined(PS2X_GAME_GT4)
        std::string d = "#define GS_SHADE_EXACT 1\n";
#if !defined(GT4_STOCK_BUILD)
        // Effects at their own scale (GT4_EFFECTS_SCALE 2-4): the target reads take each source's scale. Without it
        // the programs are as before (every target at the picture's scale).
        // GT4's car view at full height in the 480p menus (tallPossible) reads targets at their own scales the same way.
        if (const char *es = std::getenv("GT4_EFFECTS_SCALE"); (es && std::atoi(es) >= 2) || tallPossible())
            d += "#define GS_EFFECTS_SCALE 1\n";
        // GT4's car lighting per pixel (GT4_CAR_LIGHTING=pixel, lighting step 2; the VU1 tap is then on).
        if (carLightingPerPixel())
            d += "#define GS_LIT 1\n";
        // (GT4_CAR_LIGHTING=split, checking: per pixel right of the frame's middle only, a wipe on the same frame)
        if (carLightingPerPixel() && std::strcmp(std::getenv("GT4_CAR_LIGHTING"), "split") == 0)
            d += "#define GS_LIT_SPLIT 320.0\n";
        // (=show, checking: the pixels lit per pixel tinted red, the reflections green)
        if (carLightingPerPixel() && std::strcmp(std::getenv("GT4_CAR_LIGHTING"), "show") == 0)
            d += "#define GS_LIT_SHOW 1\n";
#endif
        return d;
#else
        return {};
#endif
    }

    static std::string experimentDefines()
    {
        const char *v = std::getenv("GT3_GS_GL_SHADER_DEFINES");
        std::string out;
        if (!v)
            return out;
        std::string list = v;
        size_t start = 0;
        while (start < list.size())
        {
            size_t comma = list.find(',', start);
            if (comma == std::string::npos)
                comma = list.size();
            if (comma > start)
                out += "#define " + list.substr(start, comma - start) + " 1\n";
            start = comma + 1;
        }
        return out;
    }

    void buildDecodeProgram()
    {
        const std::string cs = R"GLSL(#version 430 core
layout(local_size_x = 8, local_size_y = 8) in;
// Each texel's raw value as fetchRaw reads it (shifted to bit 0, masked to
// its format), from the texture's pages (u_pages.y pages from u_pages.x, 64
// words a row, 32 rows a page).
uniform usampler2D u_raw;
uniform usampler2D u_addrLut;
uniform ivec3 u_tex; // TBP0, TBW, PSM
uniform ivec2 u_pages;
uniform ivec2 u_size;
writeonly uniform uimage2D u_out;
void main()
{
    ivec2 t = ivec2(gl_GlobalInvocationID.xy);
    if (t.x >= u_size.x || t.y >= u_size.y)
        return;
    int psm = u_tex.z;
    uint bp = uint(u_tex.x), bw = uint(u_tex.y);
    uint pw = 64u, ph = 32u, row = 192u, ppp = 2048u, ppb = 64u, width = 32u, bitOffset = 0u;
    if (psm == 0x14) { pw = 128u; ph = 128u; row = 0u; ppp = 16384u; ppb = 512u; width = 4u; }
    else if (psm == 0x13) { pw = 128u; ph = 64u; row = 128u; ppp = 8192u; ppb = 256u; width = 8u; }
    else if (psm == 0x02) { ph = 64u; row = 224u; ppp = 4096u; ppb = 128u; width = 16u; }
    else if (psm == 0x0A) { ph = 64u; row = 288u; ppp = 4096u; ppb = 128u; width = 16u; }
    else if (psm == 0x1B || psm == 0x24) bitOffset = 24u;
    else if (psm == 0x2C) bitOffset = 28u;
    uint uu = uint(t.x), vv = uint(t.y);
    uint page = bp / 32u + (vv / ph) * ((bw * 64u) / pw) + uu / pw;
    uint offset = texelFetch(u_addrLut, ivec2(int(uu % pw), int(row + vv % ph)), 0).r;
    uint bits = (page * ppp + (bp % 32u) * ppb + offset) * width + bitOffset;
    uint rel = (((bits >> 5) & 0xFFFFFu) - uint(u_pages.x) * 2048u) & 0xFFFFFu;
    uint value = rel < uint(u_pages.y) * 2048u ? texelFetch(u_raw, ivec2(int(rel & 63u), int(rel >> 6)), 0).r >> (bits & 31u) : 0u;
    uint mask = width == 32u ? (psm == 0x1B ? 0xFFu : ((psm == 0x24 || psm == 0x2C) ? 0xFu : 0xFFFFFFFFu)) : ((1u << width) - 1u);
    imageStore(u_out, t, uvec4(value & mask));
}
)GLSL";
        const GLuint shader = compile(GL_COMPUTE_SHADER, cs);
        if (!shader)
            return;
        decodeProgram = glCreateProgram();
        glAttachShader(decodeProgram, shader);
        glLinkProgram(decodeProgram);
        glDeleteShader(shader);
        GLint linked = 0;
        glGetProgramiv(decodeProgram, GL_LINK_STATUS, &linked);
        if (!linked)
        {
            glDeleteProgram(decodeProgram);
            decodeProgram = 0;
            return;
        }
        const char *names[5] = {"u_raw", "u_addrLut", "u_tex", "u_pages", "u_size"};
        for (int i = 0; i < 5; ++i)
            decodeLoc[i] = glGetUniformLocation(decodeProgram, names[i]);
        gpuDecode = true;
        std::cerr << "[gs-gl] raw textures decoded once on the GPU" << std::endl;
    }

    // The texel values of a raw texture (rawPagesTexture's pages, whose
    // version signature rawSignature is) as an R8UI/R16UI/R32UI texture of
    // the decode area, made by the decode program and cached until the pages
    // change.
    GLuint gpuDecodedTexture(const GSTex0Reg &tex, int w, int h, GLuint raw, int firstPage, int pageCount, uint64_t rawSignature)
    {
        const uint32_t psm = tex.psm;
        const uint32_t fields[] = {tex.tbp0, tex.tbw, psm, static_cast<uint32_t>(w), static_cast<uint32_t>(h), static_cast<uint32_t>(firstPage),
                                   static_cast<uint32_t>(pageCount)};
        const uint64_t id = hashBytes(fields, sizeof(fields), 0x4750554445434F44ull);
        TexEntry &entry = textures[id];
        entry.lastUse = frameCounter;
        if (entry.tex && entry.signature == rawSignature)
            return entry.tex;
        // Only textures whose pages stay the same from one frame to a later
        // one: GT3 streams most textures through a few pages (a decode per
        // upload, ~250 a frame, cost more than the fetches it saved).
        // GT3_GS_GL_GPU_DECODE=2: every texture, stable or not.
        static const bool decodeAll = [] {
            const char *v = std::getenv("GT3_GS_GL_GPU_DECODE");
            return v && std::strcmp(v, "2") == 0;
        }();
        if (!decodeAll && entry.candidate != rawSignature)
        {
            entry.candidate = rawSignature;
            entry.candidateFrame = frameCounter;
            return 0;
        }
        if (!decodeAll && entry.candidateFrame == frameCounter)
            return 0;
        if (batchUses(entry.tex))
            flushBatch(); // queued draws read the old values
        const bool index8 = psm == GS_PSM_T8 || psm == GS_PSM_T8H || psm == GS_PSM_T4 || psm == GS_PSM_T4HL || psm == GS_PSM_T4HH;
        const bool bits16 = psm == GS_PSM_CT16 || psm == GS_PSM_CT16S;
        const GLenum format = index8 ? GL_R8UI : (bits16 ? GL_R16UI : GL_R32UI);
        if (!entry.tex)
        {
            glGenTextures(1, &entry.tex);
            glBindTexture(GL_TEXTURE_2D, entry.tex);
            glTexStorage2D(GL_TEXTURE_2D, 1, format, w, h);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
            glBindTexture(GL_TEXTURE_2D, 0);
            entry.width = w;
            entry.height = h;
        }
        glUseProgram(decodeProgram);
        glActiveTexture(GL_TEXTURE14);
        glBindTexture(GL_TEXTURE_2D, raw);
        glActiveTexture(GL_TEXTURE15);
        glBindTexture(GL_TEXTURE_2D, addrLut);
        glActiveTexture(GL_TEXTURE0);
        glUniform1i(decodeLoc[0], 14);
        glUniform1i(decodeLoc[1], 15);
        glUniform3i(decodeLoc[2], static_cast<GLint>(tex.tbp0), static_cast<GLint>(tex.tbw), static_cast<GLint>(psm));
        glUniform2i(decodeLoc[3], firstPage, pageCount);
        glUniform2i(decodeLoc[4], w, h);
        glBindImageTexture(0, entry.tex, 0, GL_FALSE, 0, GL_WRITE_ONLY, format);
        glDispatchCompute(static_cast<GLuint>((w + 7) / 8), static_cast<GLuint>((h + 7) / 8), 1);
        glMemoryBarrier(GL_TEXTURE_FETCH_BARRIER_BIT);
        glBindImageTexture(0, 0, 0, GL_FALSE, 0, GL_WRITE_ONLY, format);
        glUseProgram(0);
        glStateDirty = true;
        entry.signature = rawSignature;
        ++statGpuDecodes;
        return entry.tex;
    }

    void buildColourProgram()
    {
        const std::string cs = R"GLSL(#version 430 core
layout(local_size_x = 8, local_size_y = 8) in;
// Each texel's colour as fetchRaw + rawColor give it (the draw shader's
// texMode 13 for one texel), written as RGBA8 for the GPU's bilinear filter.
uniform usampler2D u_raw;
uniform usampler2D u_addrLut;
uniform sampler2D u_palette; // resolved CLUT 256x1 (u_paletteMode 0) or the GPU CLUT (1)
uniform ivec3 u_tex;         // TBP0, TBW, PSM
uniform ivec2 u_pages;
uniform ivec2 u_size;
uniform ivec3 u_texa;        // TA0, AEM, TA1
uniform int u_paletteMode;
uniform ivec3 u_clut;        // CSA, 4-bit texture, CLUT is CT24
uniform ivec2 u_clutOrigin;  // the GPU CLUT's square in u_palette (read straight from its target)
uniform ivec2 u_scale;
layout(rgba8, binding = 0) writeonly uniform image2D u_out;
uvec4 paletteLookup(uint index)
{
    if (u_paletteMode == 0)
        return uvec4(round(texelFetch(u_palette, ivec2(int(index), 0), 0) * 255.0));
    uint entry = (u_clut.y != 0) ? (uint(u_clut.x) * 16u + (index & 15u)) : ((uint(u_clut.x) & 15u) * 16u + index);
    entry &= 255u;
    uint pos = (entry & ~0x18u) | ((entry & 0x08u) << 1) | ((entry & 0x10u) >> 1);
    uvec4 c = uvec4(round(texelFetch(u_palette, (ivec2(int(pos & 15u), int(pos >> 4)) + u_clutOrigin) * u_scale + u_scale / 2, 0) * 255.0));
    if (u_clut.z == 2)
    {
        uint h = c.r | (c.g << 8);
        c = uvec4((h & 31u) << 3, ((h >> 5) & 31u) << 3, ((h >> 10) & 31u) << 3, 0u);
        bool rgbZero16 = c.r == 0u && c.g == 0u && c.b == 0u;
        c.a = (h & 0x8000u) != 0u ? uint(u_texa.z) : ((u_texa.y != 0 && rgbZero16) ? 0u : uint(u_texa.x));
    }
    else if (u_clut.z != 0)
    {
        bool rgbZero = c.r == 0u && c.g == 0u && c.b == 0u;
        c.a = (u_texa.y != 0 && rgbZero) ? 0u : uint(u_texa.x);
    }
    return c;
}
uvec4 rawColor(uint value, int psm)
{
    if (psm == 0x13 || psm == 0x1B)
        return paletteLookup(value & 0xFFu);
    if (psm == 0x14 || psm == 0x24 || psm == 0x2C)
        return paletteLookup(value & 0x0Fu);
    uvec4 c;
    if (psm == 0x02 || psm == 0x0A)
    {
        c = uvec4((value & 31u) << 3, ((value >> 5) & 31u) << 3, ((value >> 10) & 31u) << 3, 0u);
        bool rgbZero16 = c.r == 0u && c.g == 0u && c.b == 0u;
        c.a = (value & 0x8000u) != 0u ? uint(u_texa.z) : ((u_texa.y != 0 && rgbZero16) ? 0u : uint(u_texa.x));
        return c;
    }
    c = uvec4(value & 255u, (value >> 8) & 255u, (value >> 16) & 255u, value >> 24);
    if (psm == 0x01)
    {
        bool rgbZero24 = c.r == 0u && c.g == 0u && c.b == 0u;
        c.a = (u_texa.y != 0 && rgbZero24) ? 0u : uint(u_texa.x);
    }
    return c;
}
void main()
{
    ivec2 t = ivec2(gl_GlobalInvocationID.xy);
    if (t.x >= u_size.x || t.y >= u_size.y)
        return;
    int psm = u_tex.z;
    uint bp = uint(u_tex.x), bw = uint(u_tex.y);
    uint pw = 64u, ph = 32u, row = 192u, ppp = 2048u, ppb = 64u, width = 32u, bitOffset = 0u;
    if (psm == 0x14) { pw = 128u; ph = 128u; row = 0u; ppp = 16384u; ppb = 512u; width = 4u; }
    else if (psm == 0x13) { pw = 128u; ph = 64u; row = 128u; ppp = 8192u; ppb = 256u; width = 8u; }
    else if (psm == 0x02) { ph = 64u; row = 224u; ppp = 4096u; ppb = 128u; width = 16u; }
    else if (psm == 0x0A) { ph = 64u; row = 288u; ppp = 4096u; ppb = 128u; width = 16u; }
    else if (psm == 0x1B || psm == 0x24) bitOffset = 24u;
    else if (psm == 0x2C) bitOffset = 28u;
    uint uu = uint(t.x), vv = uint(t.y);
    uint page = bp / 32u + (vv / ph) * ((bw * 64u) / pw) + uu / pw;
    uint offset = texelFetch(u_addrLut, ivec2(int(uu % pw), int(row + vv % ph)), 0).r;
    uint bits = (page * ppp + (bp % 32u) * ppb + offset) * width + bitOffset;
    uint rel = (((bits >> 5) & 0xFFFFFu) - uint(u_pages.x) * 2048u) & 0xFFFFFu;
    uvec4 c = uvec4(0u);
    if (rel < uint(u_pages.y) * 2048u)
        c = rawColor(texelFetch(u_raw, ivec2(int(rel & 63u), int(rel >> 6)), 0).r >> (bits & 31u), psm);
    imageStore(u_out, t, vec4(c) / 255.0);
}
)GLSL";
        const GLuint shader = compile(GL_COMPUTE_SHADER, cs);
        if (!shader)
            return;
        colourProgram = glCreateProgram();
        glAttachShader(colourProgram, shader);
        glLinkProgram(colourProgram);
        glDeleteShader(shader);
        GLint linked = 0;
        glGetProgramiv(colourProgram, GL_LINK_STATUS, &linked);
        if (!linked)
        {
            glDeleteProgram(colourProgram);
            colourProgram = 0;
            return;
        }
        const char *names[12] = {"u_raw", "u_addrLut", "u_palette", "u_tex", "u_pages", "u_size", "u_texa", "u_paletteMode", "u_clut", "u_scale", "u_out", "u_clutOrigin"};
        for (int i = 0; i < 12; ++i)
            colourLoc[i] = glGetUniformLocation(colourProgram, names[i]);
    }

    // A raw texture's colours (fast mode, texMode 15): RGBA8, decoded by the
    // colour program from rawPagesTexture's pages, cached by their versions
    // and (for palettes, 16-bit and 24-bit texels) the palette and TEXA.
    GLuint gpuColourTexture(const GSTex0Reg &tex, const DrawKey &k, const GSDrawState &state, int w, int h, GLuint raw, int firstPage,
                            int pageCount, uint64_t rawSignature)
    {
        const uint32_t psm = tex.psm;
        const bool paletted = psm == GS_PSM_T8 || psm == GS_PSM_T8H || psm == GS_PSM_T4 || psm == GS_PSM_T4HL || psm == GS_PSM_T4HH;
        const bool usesTexa = paletted || psm == GS_PSM_CT16 || psm == GS_PSM_CT16S || psm == GS_PSM_CT24;
        uint32_t fields[16] = {tex.tbp0, tex.tbw, psm, static_cast<uint32_t>(w), static_cast<uint32_t>(h), static_cast<uint32_t>(firstPage),
                               static_cast<uint32_t>(pageCount), 0, 0, 0, 0, 0, 0, 0, 0, 0};
        if (paletted)
        {
            // The resolved palette texture is cached by content (its id names
            // the colours); the GPU CLUT changes in place with clutVersion.
            fields[7] = k.paletteMode == 1 ? clutVersion : 0u;
            fields[8] = static_cast<uint32_t>(k.paletteMode);
            fields[9] = static_cast<uint32_t>(k.clutCsa);
            fields[10] = static_cast<uint32_t>(k.clutFour);
            fields[11] = static_cast<uint32_t>(k.clutCt24);
            fields[12] = k.paletteTex;
            fields[13] = tex.cpsm;
        }
        if (usesTexa)
        {
            fields[14] = state.texa.ta0 | (state.texa.aem ? 0x100u : 0u);
            fields[15] = state.texa.ta1;
        }
        // The previous call's texture when nothing differs (consecutive
        // primitives mostly share one: no hash, no lookup); cleared when
        // colour textures are recycled.
        if (lastColour.entry && lastColour.signature == rawSignature && std::memcmp(lastColour.fields, fields, sizeof(fields)) == 0)
        {
            lastColour.entry->lastUse = frameCounter;
            ++statColourHits;
            return lastColour.entry->tex;
        }
        const uint64_t key = hashBytes(fields, sizeof(fields), rawSignature ^ 0x434F4C4F5552ull);
        if (auto it = colourTextures.find(key); it != colourTextures.end())
        {
            it->second.lastUse = frameCounter;
            ++statColourHits;
            lastColour.entry = &it->second;
            lastColour.signature = rawSignature;
            std::memcpy(lastColour.fields, fields, sizeof(fields));
            return it->second.tex;
        }
        ColourEntry entry;
        entry.w = w;
        entry.h = h;
        entry.lastUse = frameCounter;
        std::vector<GLuint> &pool = colourPool[(static_cast<uint64_t>(w) << 32) | static_cast<uint32_t>(h)];
        if (!pool.empty())
        {
            entry.tex = pool.back();
            pool.pop_back();
        }
        else
        {
            glGenTextures(1, &entry.tex);
            glBindTexture(GL_TEXTURE_2D, entry.tex);
            glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA8, w, h);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
            glBindTexture(GL_TEXTURE_2D, 0);
        }
        glUseProgram(colourProgram);
        glActiveTexture(GL_TEXTURE13);
        glBindTexture(GL_TEXTURE_2D, paletted ? k.paletteTex : 0);
        glActiveTexture(GL_TEXTURE14);
        glBindTexture(GL_TEXTURE_2D, raw);
        glActiveTexture(GL_TEXTURE15);
        glBindTexture(GL_TEXTURE_2D, addrLut);
        glActiveTexture(GL_TEXTURE0);
        glUniform1i(colourLoc[0], 14);
        glUniform1i(colourLoc[1], 15);
        glUniform1i(colourLoc[2], 13);
        glUniform3i(colourLoc[3], static_cast<GLint>(tex.tbp0), static_cast<GLint>(tex.tbw), static_cast<GLint>(psm));
        glUniform2i(colourLoc[4], firstPage, pageCount);
        glUniform2i(colourLoc[5], w, h);
        glUniform3i(colourLoc[6], static_cast<GLint>(state.texa.ta0), state.texa.aem ? 1 : 0, static_cast<GLint>(state.texa.ta1));
        glUniform1i(colourLoc[7], k.paletteMode);
        glUniform3i(colourLoc[8], k.clutCsa, k.clutFour, k.clutCt24);
        {
            const TexScale ps = k.paletteMode == 1 && k.paletteTex ? scaleOfTex(k.paletteTex) : pictureScale();
            glUniform2i(colourLoc[9], ps.x, ps.y); // (effects at their own scale: the CLUT's)
        }
        glUniform2i(colourLoc[11], k.clutOrgX, k.clutOrgY);
        glBindImageTexture(0, entry.tex, 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_RGBA8);
        glDispatchCompute(static_cast<GLuint>((w + 7) / 8), static_cast<GLuint>((h + 7) / 8), 1);
        glMemoryBarrier(GL_TEXTURE_FETCH_BARRIER_BIT);
        glBindImageTexture(0, 0, 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_RGBA8);
        glUseProgram(0);
        forgetBindings();
        glStateDirty = true;
        colourTextures.emplace(key, entry);
        ++statColourDecodes;
        return entry.tex;
    }

    // Colour textures unused for two frames go back to the pool (queued draws
    // of those frames have been drawn); at most 32 spare textures a size.
    void recycleColourTextures()
    {
        lastColour.entry = nullptr;
        for (auto it = colourTextures.begin(); it != colourTextures.end();)
        {
            if (it->second.lastUse + 2u < frameCounter)
            {
                std::vector<GLuint> &pool = colourPool[(static_cast<uint64_t>(it->second.w) << 32) | static_cast<uint32_t>(it->second.h)];
                if (pool.size() < 32u)
                    pool.push_back(it->second.tex);
                else
                    releaseTexture(it->second.tex);
                it = colourTextures.erase(it);
            }
            else
                ++it;
        }
    }

    // The local memory pages a CPU-held texture reads (its decode area), as
    // raw words for fetchRaw: no decode on the CPU, only a copy of the pages.
    // Cached by page range and CPU versions (textures sharing memory share
    // it). Returns 0 when the pages span more than 64.
    // The last rawPagesTexture answer: consecutive primitives mostly read the
    // same texture, and with no CPU page version changed since (epoch) it is
    // the same (the page walk, version hash and map lookup were ~5% of the
    // GS GL thread at 2x).
    // (Eight answers, not one: GT4's cars alternate between a few textures per
    // part, and each switch walked and hashed every page again; a miss was
    // ~2 us, 25,000 a second at Trial Mountain, 2026-10-11.)
    struct LastRaw
    {
        bool valid = false;
        uint32_t tbp0 = 0, tbw = 0, psm = 0;
        int w = 0, h = 0, first = 0, span = 0;
        uint64_t epoch = 0, signature = 0;
        TexEntry *entry = nullptr;
    } lastRaw;
    static constexpr int kRawRecent = 32;
    LastRaw rawRecent[kRawRecent];
    int rawRecentNext = 0;
    void forgetRawAnswers()
    {
        lastRaw.valid = false;
        for (LastRaw &r : rawRecent)
            r.valid = false;
    }
    uint64_t cpuVersionEpoch = 0; // bumped with any cpuVersion
    // The epoch each page's CPU version last changed at: an answer stays good while none of its own pages changed
    // since it was taken (any page's change made every answer miss: GT4 streams textures into other pages ~90
    // times a frame, and each miss walked and hashed the pages again).
    std::array<uint64_t, kPageCount> pageChangedAt{};
    bool pagesUnchangedSince(const LastRaw &r) const
    {
        if (r.epoch == cpuVersionEpoch)
            return true;
        for (int i = 0; i < r.span; ++i)
            if (pageChangedAt[static_cast<uint32_t>(r.first + i) & (kPageCount - 1u)] > r.epoch)
                return false;
        return true;
    }

    GLuint rawPagesTexture(const GSTex0Reg &tex, int decodeW, int decodeH, int &firstPage, int &pageCount, uint64_t *signatureOut = nullptr)
    {
        auto matches = [&](const LastRaw &r) {
            return r.valid && r.tbp0 == tex.tbp0 && r.tbw == tex.tbw && r.psm == tex.psm && r.w == decodeW && r.h == decodeH;
        };
        auto answer = [&](LastRaw &r) {
            r.entry->lastUse = frameCounter;
            firstPage = r.first;
            pageCount = r.span;
            if (signatureOut)
                *signatureOut = r.signature;
            return r.entry->tex;
        };
        if (matches(lastRaw) && pagesUnchangedSince(lastRaw) && lastRaw.entry->tex && lastRaw.entry->signature == lastRaw.signature)
        {
            if (stats)
                ++statRawHits;
            return answer(lastRaw);
        }
        // An earlier answer: valid while no CPU page version changed since
        // (epoch) and its entry still holds that upload (signature).
        for (LastRaw &r : rawRecent)
            if (matches(r) && pagesUnchangedSince(r) && r.entry->tex && r.entry->signature == r.signature)
            {
                if (stats)
                    ++statRawHits;
                lastRaw = r;
                return answer(r);
            }
        if (stats)
            ++(matches(lastRaw) ? statRawMissEpoch : statRawMissKey);
        const GLuint made = rawPagesTextureUncached(tex, decodeW, decodeH, firstPage, pageCount, signatureOut);
        if (lastRaw.valid)
        {
            LastRaw *slot = nullptr;
            for (LastRaw &r : rawRecent)
                if (matches(r))
                    slot = &r;
            if (!slot)
            {
                slot = &rawRecent[rawRecentNext];
                rawRecentNext = (rawRecentNext + 1) % kRawRecent;
            }
            *slot = lastRaw;
        }
        return made;
    }

    GLuint rawPagesTextureUncached(const GSTex0Reg &tex, int decodeW, int decodeH, int &firstPage, int &pageCount, uint64_t *signatureOut)
    {
        lastRaw.valid = false;
        const uint32_t first = tex.tbp0 >> 5u;
        uint32_t span = 1u;
        forRectPages(first, (tex.tbp0 & 31u) != 0u, tex.tbw, tex.psm, 0, 0, decodeW - 1, decodeH - 1,
                     [&](uint32_t page) { span = std::max(span, ((page - first) & (kPageCount - 1u)) + 1u); });
        if (span > 64u)
            return 0;
        // By content (pageContentHash), not version: the same texture streamed
        // into the same pages again is not uploaded again.
        uint64_t signature = 0x9E3779B97F4A7C15ull;
        for (uint32_t i = 0; i < span; ++i)
        {
            const uint64_t h = pageContentHash(first + i);
            signature = hashBytes(&h, sizeof(h), signature);
        }
        const uint32_t fields[] = {first, span};
        const uint64_t id = hashBytes(fields, sizeof(fields), 0x524157504147u);
        TexEntry &entry = textures[id];
        entry.lastUse = frameCounter;
        firstPage = static_cast<int>(first);
        pageCount = static_cast<int>(span);
        if (signatureOut)
            *signatureOut = signature;
        lastRaw.tbp0 = tex.tbp0;
        lastRaw.tbw = tex.tbw;
        lastRaw.psm = tex.psm;
        lastRaw.w = decodeW;
        lastRaw.h = decodeH;
        lastRaw.first = static_cast<int>(first);
        lastRaw.span = static_cast<int>(span);
        lastRaw.epoch = cpuVersionEpoch;
        lastRaw.signature = signature;
        lastRaw.entry = &entry;
        lastRaw.valid = true; // entry.signature == signature on both ways out
        if (entry.tex && entry.signature == signature)
            return entry.tex;
        if (batchUses(entry.tex))
            flushBatch(); // queued draws read the old words
        if (!entry.tex)
        {
            glGenTextures(1, &entry.tex);
            glBindTexture(GL_TEXTURE_2D, entry.tex);
            glTexStorage2D(GL_TEXTURE_2D, 1, GL_R32UI, 64, static_cast<GLsizei>(span * 32u));
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
            entry.width = 64;
            entry.height = static_cast<int>(span * 32u);
        }
        ScopedNs timer(statNs[2]);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, entry.tex);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
        const uint32_t before = std::min(span, kPageCount - first); // pages before the wrap to page 0
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 64, static_cast<GLsizei>(before * 32u), GL_RED_INTEGER, GL_UNSIGNED_INT,
                        vram + static_cast<size_t>(first) * 8192u);
        if (before < span)
            glTexSubImage2D(GL_TEXTURE_2D, 0, 0, static_cast<GLint>(before * 32u), 64, static_cast<GLsizei>((span - before) * 32u),
                            GL_RED_INTEGER, GL_UNSIGNED_INT, vram);
        glBindTexture(GL_TEXTURE_2D, 0);
        entry.signature = signature;
        ++statRawUploads;
        static const bool verify = envFlag("GT3_GS_GL_RAW_VERIFY", false);
        if (verify)
            verifyRaw(tex, decodeW, decodeH, first, span);
        return entry.tex;
    }

    // GS mipmapping: raw words of pages [first, first + span) (wrapping past
    // page 511), the same cache entries as rawPagesTexture (keyed by first and
    // span, uploaded again when the pages' contents change). Returns 0 when the
    // span is over 64 pages.
    struct LastSpan
    {
        bool valid = false;
        uint32_t first = 0, span = 0;
        uint64_t epoch = 0, signature = 0;
        TexEntry *entry = nullptr;
    } lastSpan;
    GLuint rawPagesSpanTexture(uint32_t first, uint32_t span)
    {
        first &= kPageCount - 1u;
        if (span == 0u || span > 64u)
            return 0;
        if (lastSpan.valid && lastSpan.epoch == cpuVersionEpoch && lastSpan.first == first && lastSpan.span == span && lastSpan.entry->tex &&
            lastSpan.entry->signature == lastSpan.signature)
        {
            lastSpan.entry->lastUse = frameCounter;
            return lastSpan.entry->tex;
        }
        lastSpan.valid = false;
        uint64_t signature = 0x9E3779B97F4A7C15ull;
        for (uint32_t i = 0; i < span; ++i)
        {
            const uint64_t h = pageContentHash(first + i);
            signature = hashBytes(&h, sizeof(h), signature);
        }
        const uint32_t fields[] = {first, span};
        const uint64_t id = hashBytes(fields, sizeof(fields), 0x524157504147u);
        TexEntry &entry = textures[id];
        entry.lastUse = frameCounter;
        lastSpan = {true, first, span, cpuVersionEpoch, signature, &entry};
        if (entry.tex && entry.signature == signature)
            return entry.tex;
        if (batchUses(entry.tex))
            flushBatch(); // queued draws read the old words
        if (!entry.tex)
        {
            glGenTextures(1, &entry.tex);
            glBindTexture(GL_TEXTURE_2D, entry.tex);
            glTexStorage2D(GL_TEXTURE_2D, 1, GL_R32UI, 64, static_cast<GLsizei>(span * 32u));
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
            entry.width = 64;
            entry.height = static_cast<int>(span * 32u);
        }
        ScopedNs timer(statNs[2]);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, entry.tex);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
        const uint32_t before = std::min(span, kPageCount - first); // pages before the wrap to page 0
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 64, static_cast<GLsizei>(before * 32u), GL_RED_INTEGER, GL_UNSIGNED_INT,
                        vram + static_cast<size_t>(first) * 8192u);
        if (before < span)
            glTexSubImage2D(GL_TEXTURE_2D, 0, 0, static_cast<GLint>(before * 32u), 64, static_cast<GLsizei>((span - before) * 32u),
                            GL_RED_INTEGER, GL_UNSIGNED_INT, vram);
        glBindTexture(GL_TEXTURE_2D, 0);
        entry.signature = signature;
        ++statRawUploads;
        ++statMipSpanUploads;
        return entry.tex;
    }

    // GS mipmapping, a per-pixel primitive's texture (mipmap-design.md 4.4):
    // levels mipLo..mipHi (and level 0, whose TEX0 the state keeps) read raw
    // through one texture over all their pages (texMode 13; the shader moves
    // the base / width per level). Pages a GPU target holds are copied back
    // first, as the level-0 raw path does. fu0..fv1: level 0's footprint;
    // texLo / texHi (U, V): the vertices' level-0 texel range, texKnown false
    // when it is not known (Submit's footprint then takes the whole texture).
    // Returns the CPU fallback reason, or null with k set up.
    const char *mipRawSource(DrawKey &k, const GSDrawState &state, int fu0, int fv0, int fu1, int fv1, const float (&texLo)[2],
                             const float (&texHi)[2], bool texKnown)
    {
        const GSContext &ctx = state.context;
        const GSTex0Reg &tex = ctx.tex0;
        const gsmip::Tex1 t1 = gsmip::decodeTex1(ctx.tex1);
        const int wrapU = static_cast<int>(ctx.clamp & 3u), wrapV = static_cast<int>((ctx.clamp >> 2) & 3u);
        auto extent = [](int size, int mode, int rmin, int rmax) {
            if (mode == 2) return std::max(size, rmax + 1);
            if (mode == 3) return std::max(size, (rmin | rmax) + 1);
            return size;
        };
        // Texels a level's taps can reach (Submit's footprint rule, here at the
        // level's size and CLAMP region): the range one wider for bilinear,
        // REPEAT across the edge the whole level.
        auto levelRange = [texKnown](float l, float h, int size, int mode, int rmin, int rmax, int &a, int &b) {
            if (mode == 2) { a = std::min(rmin, rmax); b = rmax; return; }
            if (mode == 3) { a = 0; b = (rmin | rmax) & 0x3FF; return; }
            if (!texKnown || !(h - l < 4096.0f)) { a = 0; b = size - 1; return; }
            a = floorToInt(l) - 2;
            b = ceilToInt(h) + 1;
            if (mode == 1) { a = std::clamp(a, 0, size - 1); b = std::clamp(b, 0, size - 1); }
            else if (a < 0 || b >= size) { a = 0; b = size - 1; }
        };
        uint64_t used[kPageCount / 64u] = {};
        const int w0 = std::max<int>(state.textureWidth, 1), h0 = std::max<int>(state.textureHeight, 1);
        const int top = std::min<int>(state.mipHi, 5);
        for (int n = 0; n <= top; ++n)
        {
            if (n != 0 && n < static_cast<int>(state.mipLo))
                continue;
            const gsmip::Level L = gsmip::level(ctx, n);
            const int dw = std::min(1024, extent(static_cast<int>(L.w), wrapU, L.minU, L.maxU));
            const int dh = std::min(1024, extent(static_cast<int>(L.h), wrapV, L.minV, L.maxV));
            // The texels it can read: the vertices' texel range scaled to this
            // level, then this level's size, wrap mode and CLAMP region, as
            // GSCpuBackend::Submit's hazard footprint. (Level 0's footprint
            // scaled down misses REPEAT across a smaller level's edge: a range
            // from u = 2 stays inside level 0, but bilinear at level 3 reads
            // u = -1, the level's last column.)
            int a0 = fu0, b0 = fv0, a1 = fu1, b1 = fv1;
            if (n != 0)
            {
                const float su = static_cast<float>(L.w) / static_cast<float>(w0), sv = static_cast<float>(L.h) / static_cast<float>(h0);
                levelRange(texLo[0] * su, texHi[0] * su, static_cast<int>(L.w), wrapU, L.minU, L.maxU, a0, a1);
                levelRange(texLo[1] * sv, texHi[1] * sv, static_cast<int>(L.h), wrapV, L.minV, L.maxV, b0, b1);
            }
            a0 = std::clamp(a0, 0, dw - 1);
            a1 = std::clamp(a1, a0, dw - 1);
            b0 = std::clamp(b0, 0, dh - 1);
            b1 = std::clamp(b1, b0, dh - 1);
            const uint32_t basePage = L.tbp >> 5u;
            const bool straddles = (L.tbp & 31u) != 0u;
            bool gpuPages = false, selfPage = false;
            forRectPages(basePage, straddles, L.tbw, tex.psm, a0, b0, a1, b1, [&](uint32_t page) {
                const int o = owner[page & (kPageCount - 1u)];
                if (o >= 0)
                {
                    gpuPages = true;
                    selfPage = selfPage || o == k.colorTarget || o == k.depthTarget;
                }
            });
            if (selfPage)
                return "mipmap self-read";
            if (gpuPages)
            {
                if (stats)
                    cause = "mipmap level texture";
                syncPagesToCpu(L.tbp, L.tbw, tex.psm, a0, b0, a1, b1);
            }
            forRectPages(basePage, straddles, L.tbw, tex.psm, 0, 0, dw - 1, dh - 1, [&](uint32_t page) {
                page &= kPageCount - 1u;
                used[page >> 6u] |= 1ull << (page & 63u);
            });
        }
        // The smallest page range (circular) holding every used page: after the
        // longest run of unused pages.
        auto isUsed = [&](uint32_t page) { return ((used[(page & (kPageCount - 1u)) >> 6u] >> (page & 63u)) & 1ull) != 0ull; };
        uint32_t bestGap = 0, bestEnd = 0, run = 0;
        for (uint32_t i = 0; i < 2u * kPageCount; ++i)
        {
            if (isUsed(i))
                run = 0;
            else if (++run > bestGap && run <= kPageCount)
            {
                bestGap = run;
                bestEnd = i + 1u;
            }
        }
        if (bestGap >= kPageCount)
            return "mipmap no pages";
        const uint32_t first = bestEnd & (kPageCount - 1u), span = kPageCount - bestGap;
        if (span > 64u)
            return "mipmap span over 64 pages";
        const GLuint raw = rawPagesSpanTexture(first, span);
        if (!raw)
            return "mipmap span over 64 pages";
        k.texMode = 13;
        k.texSource = raw;
        k.texBasePage = static_cast<int32_t>(tex.tbp0);
        k.texPagesPerRow = static_cast<int32_t>(tex.tbw);
        k.texPageW = static_cast<int32_t>(tex.psm);
        k.texPageH = 0;
        k.texTargetFbp = static_cast<int32_t>(first);
        k.texTargetFbw = static_cast<int32_t>(span);
        k.texTargetPageH = 0;
        const uint32_t tpsm = tex.psm;
        if (tpsm == GS_PSM_T8 || tpsm == GS_PSM_T4 || tpsm == GS_PSM_T8H || tpsm == GS_PSM_T4HL || tpsm == GS_PSM_T4HH)
            choosePalette(k, state);
        k.mipCtl = gsmip::packGlCtl(t1) | (gsmip::tintGl() ? 0x800 : 0) |
                   (gsmip::rangeClampEnabled() ? gsmip::packGlRange(state.mipLo, static_cast<uint8_t>(top)) : 0);
        for (int n = 1; n <= 5; ++n)
            k.mipLv[n - 1] = gsmip::glLevelWord(ctx, n);
        return nullptr;
    }

    bool buildDepthRawProgram()
    {
        const std::string cs = R"GLSL(#version 430 core
layout(local_size_x = 8, local_size_y = 8) in;
// A depth target's page as the raw words syncPagesToCpu would leave in the
// CPU copy: Z (24 bits, as glReadPixels' float depth rounded) under the high
// plane - the alpha of the Z32-layout colour target holding it, else the
// CPU copy's own high byte (uploaded into u_raw first).
uniform sampler2D u_depth;   // the page's depth at native samples
uniform sampler2D u_high;    // the high plane holder's page (alpha)
uniform usampler2D u_z32Lut; // pixel (x, y) of a Z32 page -> its word
uniform ivec2 u_depthOrigin;
uniform ivec2 u_highOrigin;
uniform int u_hasHigh;
uniform int u_slot;          // the page's first row in u_raw
layout(r32ui, binding = 0) uniform uimage2D u_raw;
void main()
{
    ivec2 p = ivec2(gl_GlobalInvocationID.xy);
    if (p.x >= 64 || p.y >= 32)
        return;
    uint word = texelFetch(u_z32Lut, p, 0).r;
    ivec2 at = ivec2(int(word & 63u), u_slot + int(word >> 6));
    double d = double(texelFetch(u_depth, u_depthOrigin + p, 0).r);
    uint z = uint(min(floor(d * 16777216.0lf + 0.5lf), 16777215.0lf));
    uint high = u_hasHigh != 0 ? (uint(round(texelFetch(u_high, u_highOrigin + p, 0).a * 255.0)) << 24)
                               : (imageLoad(u_raw, at).r & 0xFF000000u);
    imageStore(u_raw, at, uvec4(high | z, 0u, 0u, 0u));
}
)GLSL";
        const GLuint shader = compile(GL_COMPUTE_SHADER, cs);
        depthRawProgram = ~0u;
        if (!shader)
            return false;
        const GLuint program = glCreateProgram();
        glAttachShader(program, shader);
        glLinkProgram(program);
        glDeleteShader(shader);
        GLint linked = 0;
        glGetProgramiv(program, GL_LINK_STATUS, &linked);
        if (!linked)
        {
            glDeleteProgram(program);
            return false;
        }
        depthRawProgram = program;
        const char *names[8] = {"u_depth", "u_high", "u_z32Lut", "u_depthOrigin", "u_highOrigin", "u_hasHigh", "u_slot", "u_raw"};
        for (int i = 0; i < 8; ++i)
            depthRawLoc[i] = glGetUniformLocation(program, names[i]);
        std::vector<uint16_t> lut(64u * 32u);
        for (uint32_t y = 0; y < 32u; ++y)
            for (uint32_t x = 0; x < 64u; ++x)
                lut[y * 64u + x] = static_cast<uint16_t>(GSMem::PageTableZ32[0][y][x]);
        glGenTextures(1, &z32WordLut);
        glBindTexture(GL_TEXTURE_2D, z32WordLut);
        glTexStorage2D(GL_TEXTURE_2D, 1, GL_R16UI, 64, 32);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 2);
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 64, 32, GL_RED_INTEGER, GL_UNSIGNED_SHORT, lut.data());
        glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
        glBindTexture(GL_TEXTURE_2D, 0);
        return true;
    }

    // A texture in pages a depth target holds, as rawPagesTexture's raw words
    // made on the GPU from the target instead of downloading the pages first
    // (each download waits for the GPU to finish all queued work). GT3 reads
    // a strip of its Z buffer as a 4-bit texture every frame (the sun glare's
    // occlusion in replays): ~44 downloads a second, half the frame time
    // waiting at 4x progressive. Only when every GPU-held page of the read
    // footprint is a depth target's (32-bit, the high plane in the CPU copy
    // or a Z32-layout colour target); else 0 and the caller downloads.
    // GT3_GS_GL_GPU_DEPTH_TEX=0: off.
    GLuint gpuRawPagesTexture(const GSTex0Reg &tex, int decodeW, int decodeH, int fu0, int fv0, int fu1, int fv1, int &firstPage, int &pageCount,
                              uint64_t *signatureOut)
    {
        static const bool enabled = envFlag("GT3_GS_GL_GPU_DEPTH_TEX", true);
        if (!enabled || depthRawProgram == ~0u)
            return 0;
        const uint32_t first = tex.tbp0 >> 5u;
        uint32_t span = 1u;
        forRectPages(first, (tex.tbp0 & 31u) != 0u, tex.tbw, tex.psm, 0, 0, decodeW - 1, decodeH - 1,
                     [&](uint32_t page) { span = std::max(span, ((page - first) & (kPageCount - 1u)) + 1u); });
        if (span > 64u)
            return 0;
        // The footprint's GPU pages (those syncPagesToCpu would download).
        bool ok = true;
        std::vector<uint32_t> &gpuPages = gpuRawScratch;
        gpuPages.clear();
        forRectPages(first, (tex.tbp0 & 31u) != 0u, tex.tbw, tex.psm, fu0, fv0, fu1, fv1, [&](uint32_t page) {
            if (!ok || owner[page] < 0)
                return;
            const Target *t = targets[static_cast<size_t>(owner[page])].get();
            int x = 0, y = 0;
            if (!t || !t->depth || t->pageHeight != 32 || !t->pageRect(page, x, y) || ((page - first) & (kPageCount - 1u)) >= span)
            {
                ok = false;
                return;
            }
            const int holder = highPlane[page];
            int hx = 0, hy = 0;
            if (holder >= 0 && (!targets[static_cast<size_t>(holder)] || targets[static_cast<size_t>(holder)]->layout != Layout::Z32 ||
                                !targets[static_cast<size_t>(holder)]->pageRect(page, hx, hy)))
            {
                ok = false;
                return;
            }
            if (std::find(gpuPages.begin(), gpuPages.end(), page) == gpuPages.end())
                gpuPages.push_back(page);
        });
        if (!ok || gpuPages.empty())
            return 0;
        if (!depthRawProgram && !buildDepthRawProgram())
            return 0;
        // Same words as last time: no GPU write to its GPU pages (writeSerial
        // counts the target's and the holder's), the same holders and CPU words.
        uint64_t signature = 0x4750555241570000ull;
        for (uint32_t i = 0; i < span; ++i)
        {
            const uint32_t page = (first + i) & (kPageCount - 1u);
            const bool gpu = std::find(gpuPages.begin(), gpuPages.end(), page) != gpuPages.end();
            const uint64_t fields[4] = {gpu ? 1ull : 0ull, pageContentHash(page), gpu ? writeSerial[page] : 0ull,
                                        gpu ? static_cast<uint64_t>(static_cast<int64_t>(owner[page])) ^ (static_cast<uint64_t>(highPlane[page] + 1) << 32) : 0ull};
            signature = hashBytes(fields, sizeof(fields), signature);
            if (gpu)
                pageObserved(page); // (step 7 T1: its write serial is recorded)
        }
        firstPage = static_cast<int>(first);
        pageCount = static_cast<int>(span);
        if (signatureOut)
            *signatureOut = signature;
        GpuRawEntry &entry = gpuRawTextures[(static_cast<uint64_t>(first) << 32) | span];
        if (entry.tex && entry.signature == signature)
            return entry.tex;
        if (entry.tex && batchUses(entry.tex))
            flushBatch(); // queued draws read the old words
        if (haveBatch)
            for (const uint32_t page : gpuPages)
                if (key.depthTarget == owner[page] || key.colorTarget == owner[page] ||
                    (highPlane[page] >= 0 && key.colorTarget == highPlane[page]))
                {
                    flushBatch(); // queued draws write these pages
                    break;
                }
        if (!entry.tex)
        {
            glGenTextures(1, &entry.tex);
            glBindTexture(GL_TEXTURE_2D, entry.tex);
            glTexStorage2D(GL_TEXTURE_2D, 1, GL_R32UI, 64, static_cast<GLsizei>(span * 32u));
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        }
        // The CPU words of every page first (as rawPagesTexture uploads them),
        // then the GPU pages over them.
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, entry.tex);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
        const uint32_t before = std::min(span, kPageCount - first);
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 64, static_cast<GLsizei>(before * 32u), GL_RED_INTEGER, GL_UNSIGNED_INT,
                        vram + static_cast<size_t>(first) * 8192u);
        if (before < span)
            glTexSubImage2D(GL_TEXTURE_2D, 0, 0, static_cast<GLint>(before * 32u), 64, static_cast<GLsizei>((span - before) * 32u), GL_RED_INTEGER,
                            GL_UNSIGNED_INT, vram);
        glBindTexture(GL_TEXTURE_2D, 0);
        for (const uint32_t page : gpuPages)
        {
            const Target &t = *targets[static_cast<size_t>(owner[page])];
            int x = 0, y = 0, hx = 0, hy = 0;
            t.pageRect(page, x, y);
            const int holder = highPlane[page];
            const Target *h = holder >= 0 ? targets[static_cast<size_t>(holder)].get() : nullptr;
            if (h)
                h->pageRect(page, hx, hy);
            // At a scale the native samples first, as readTargetPixels takes them.
            GLuint depthTex = t.tex, highTex = h ? h->tex : 0;
            int dx = x, dy = y;
            if (scaled())
            {
                depthTex = nativeScratch(true);
                blitTex(true, t.tex, x * t.sx, y * t.sy, (x + 64) * t.sx, (y + 32) * t.sy, depthTex, 0, 0, 64, 32);
                dx = dy = 0;
                if (h)
                {
                    highTex = nativeScratch(false);
                    blitTex(false, h->tex, hx * h->sx, hy * h->sy, (hx + 64) * h->sx, (hy + 32) * h->sy, highTex, 0, 0, 64, 32);
                    hx = hy = 0;
                }
            }
            glUseProgram(depthRawProgram);
            glActiveTexture(GL_TEXTURE13);
            glBindTexture(GL_TEXTURE_2D, depthTex);
            glActiveTexture(GL_TEXTURE14);
            glBindTexture(GL_TEXTURE_2D, highTex ? highTex : depthTex);
            glActiveTexture(GL_TEXTURE15);
            glBindTexture(GL_TEXTURE_2D, z32WordLut);
            glActiveTexture(GL_TEXTURE0);
            glUniform1i(depthRawLoc[0], 13);
            glUniform1i(depthRawLoc[1], 14);
            glUniform1i(depthRawLoc[2], 15);
            glUniform2i(depthRawLoc[3], dx, dy);
            glUniform2i(depthRawLoc[4], hx, hy);
            glUniform1i(depthRawLoc[5], h ? 1 : 0);
            glUniform1i(depthRawLoc[6], static_cast<GLint>(((page - first) & (kPageCount - 1u)) * 32u));
            glBindImageTexture(0, entry.tex, 0, GL_FALSE, 0, GL_READ_WRITE, GL_R32UI);
            glDispatchCompute(8, 4, 1);
            glMemoryBarrier(GL_TEXTURE_FETCH_BARRIER_BIT | GL_SHADER_IMAGE_ACCESS_BARRIER_BIT);
            glBindImageTexture(0, 0, 0, GL_FALSE, 0, GL_READ_WRITE, GL_R32UI);
        }
        glUseProgram(0);
        forgetBindings();
        glStateDirty = true;
        entry.signature = signature;
        ++statGpuRawFills;
        forgetRawAnswers();
        lastSpan.valid = false;
        // GT3_GS_GL_GPU_DEPTH_TEX_VERIFY=1: the words read back and compared
        // with a download of the same pages (which then leaves them CPU held).
        static const bool verify = envFlag("GT3_GS_GL_GPU_DEPTH_TEX_VERIFY", false);
        if (verify)
        {
            std::vector<uint32_t> words(static_cast<size_t>(span) * 2048u);
            glBindTexture(GL_TEXTURE_2D, entry.tex);
            glPixelStorei(GL_PACK_ALIGNMENT, 4);
            glGetTexImage(GL_TEXTURE_2D, 0, GL_RED_INTEGER, GL_UNSIGNED_INT, words.data());
            glBindTexture(GL_TEXTURE_2D, 0);
            const std::vector<uint32_t> pages = gpuPages;
            syncPagesToCpu(tex.tbp0, tex.tbw, tex.psm, fu0, fv0, fu1, fv1);
            // The words the texture reads (its footprint), in the GPU pages.
            static uint64_t checked = 0, bad = 0;
            const uint32_t bw = std::max<uint32_t>(tex.tbw, 1u);
            for (int v = fv0; v <= fv1; ++v)
                for (int u = fu0; u <= fu1; ++u)
                {
                    const uint32_t word = wordAddress(tex.psm, tex.tbp0, bw, static_cast<uint32_t>(u), static_cast<uint32_t>(v));
                    if (word == ~0u)
                        continue;
                    const uint32_t page = (word >> 11u) & (kPageCount - 1u);
                    if (std::find(pages.begin(), pages.end(), page) == pages.end())
                        continue;
                    const uint32_t slot = (page - first) & (kPageCount - 1u);
                    uint32_t cpu = 0;
                    std::memcpy(&cpu, vram + (static_cast<size_t>(page) * 2048u + (word & 2047u)) * 4u, 4);
                    const uint32_t gpu = words[slot * 2048u + (word & 2047u)];
                    ++checked;
                    if (gpu != cpu && bad++ < 20u)
                        std::fprintf(stderr, "[gpu-depth-tex] page %03x word %u (texel %d,%d): gpu %08x cpu %08x\n", page, word & 2047u, u, v, gpu, cpu);
                }
            static uint64_t lastReport = 0;
            if (checked - lastReport >= 200000u)
            {
                lastReport = checked;
                std::fprintf(stderr, "[gpu-depth-tex] verified %llu words, %llu differ\n", (unsigned long long)checked, (unsigned long long)bad);
            }
        }
        return entry.tex;
    }
    std::vector<uint32_t> gpuRawScratch;

    // GT3_GS_GL_RAW_VERIFY=1: fetchRaw's addressing done on the CPU for every
    // texel of the decode area, compared with GSMem's own reads.
    void verifyRaw(const GSTex0Reg &tex, int decodeW, int decodeH, uint32_t first, uint32_t span)
    {
        static uint64_t checked = 0, mismatches = 0, outside = 0;
        const uint32_t psm = tex.psm, bp = tex.tbp0, bw = tex.tbw;
        uint32_t pw = 64u, ph = 32u, ppp = 2048u, ppb = 64u, width = 32u, bitOffset = 0u;
        const uint16_t *table = nullptr;
        auto at = [](const auto &t, uint32_t x, uint32_t y) { return static_cast<uint32_t>(t[0][y][x]); };
        for (int v = 0; v < decodeH; ++v)
            for (int u = 0; u < decodeW; ++u)
            {
                const uint32_t uu = static_cast<uint32_t>(u), vv = static_cast<uint32_t>(v);
                uint32_t offset = 0;
                pw = 64u; ph = 32u; ppp = 2048u; ppb = 64u; width = 32u; bitOffset = 0u;
                if (psm == GS_PSM_T4) { pw = 128u; ph = 128u; ppp = 16384u; ppb = 512u; width = 4u; offset = at(GSMem::PageTableP4, uu % pw, vv % ph); }
                else if (psm == GS_PSM_T8) { pw = 128u; ph = 64u; ppp = 8192u; ppb = 256u; width = 8u; offset = at(GSMem::PageTableP8, uu % pw, vv % ph); }
                else if (psm == GS_PSM_CT16) { ph = 64u; ppp = 4096u; ppb = 128u; width = 16u; offset = at(GSMem::PageTableC16, uu % pw, vv % ph); }
                else if (psm == GS_PSM_CT16S) { ph = 64u; ppp = 4096u; ppb = 128u; width = 16u; offset = at(GSMem::PageTableC16S, uu % pw, vv % ph); }
                else
                {
                    bitOffset = (psm == GS_PSM_T8H || psm == GS_PSM_T4HL) ? 24u : (psm == GS_PSM_T4HH ? 28u : 0u);
                    offset = at(GSMem::PageTableC32, uu % pw, vv % ph);
                }
                const uint32_t page = bp / 32u + (vv / ph) * ((bw * 64u) / pw) + uu / pw;
                const uint32_t bits = (page * ppp + (bp % 32u) * ppb + offset) * width + bitOffset;
                const uint32_t rel = (((bits >> 5) & 0xFFFFFu) - first * 2048u) & 0xFFFFFu;
                ++checked;
                if (rel >= span * 2048u)
                {
                    ++outside;
                    continue;
                }
                uint32_t word = 0;
                std::memcpy(&word, vram + static_cast<size_t>((first * 2048u + rel) & 0xFFFFFu) * 4u, 4);
                uint32_t value = word >> (bits & 31u);
                const uint32_t mask = width == 32u ? (psm == GS_PSM_CT24 ? 0xFFFFFFu : 0xFFFFFFFFu) : ((1u << width) - 1u);
                if (psm == GS_PSM_T8H) value &= 0xFFu;
                else if (psm == GS_PSM_T4HL || psm == GS_PSM_T4HH) value &= 0x0Fu;
                else value &= mask;
                const uint32_t expect = readTexel(vram, psm, bp, std::max<uint32_t>(bw, 1u), uu, vv) &
                                        ((psm == GS_PSM_T8H) ? 0xFFu : (psm == GS_PSM_T4HL || psm == GS_PSM_T4HH) ? 0x0Fu : mask);
                if (value != expect && mismatches++ < 20u)
                    std::fprintf(stderr, "[gs-gl] raw verify MISMATCH tbp %04x tbw %u psm %02x (%u,%u): raw %08x gsmem %08x\n", bp, bw, psm, uu, vv,
                                 value, expect);
            }
        (void)table;
        if ((checked & 0xFFFFF00000ull) != ((checked - static_cast<uint64_t>(decodeW) * decodeH) & 0xFFFFF00000ull))
            std::fprintf(stderr, "[gs-gl] raw verify: %llu texels, %llu mismatches, %llu outside the pages\n", (unsigned long long)checked,
                         (unsigned long long)mismatches, (unsigned long long)outside);
    }

    // Decoded texels (CLUT and TEXA applied, as samplePointPipe) of a texture
    // held in the CPU copy, cached by content version.
    GLuint decodedTexture(const GSDrawState &state, int decodeW, int decodeH)
    {
        const GSTex0Reg &tex = state.context.tex0;
        const uint32_t psm = tex.psm;
        const bool fourBit = psm == GS_PSM_T4 || psm == GS_PSM_T4HL || psm == GS_PSM_T4HH;
        const bool paletted = fourBit || psm == GS_PSM_T8 || psm == GS_PSM_T8H;

        uint32_t palette[256];
        if (paletted)
            buildPalette(state, palette);

        uint64_t id = hashBytes(&tex.tbp0, sizeof(tex.tbp0));
        const uint32_t fields[] = {tex.tbw, psm, static_cast<uint32_t>(decodeW), static_cast<uint32_t>(decodeH), state.texa.ta0,
                                   state.texa.aem ? 1u : 0u, state.texa.ta1};
        id = hashBytes(fields, sizeof(fields), id);
        if (paletted)
            id = hashBytes(palette, (fourBit ? 16u : 256u) * sizeof(uint32_t), id);

        uint64_t signature = 0x9E3779B97F4A7C15ull;
        forRectPages(tex.tbp0 >> 5u, (tex.tbp0 & 31u) != 0u, tex.tbw, psm, 0, 0, decodeW - 1, decodeH - 1, [&](uint32_t page) {
            signature = hashBytes(&cpuVersion[page], sizeof(uint32_t), hashBytes(&page, sizeof(page), signature));
        });

        TexEntry &entry = textures[id];
        entry.lastUse = frameCounter;
        if (entry.tex && entry.signature == signature && entry.width == decodeW && entry.height == decodeH)
            return entry.tex;
        if (batchUses(entry.tex))
            flushBatch(); // queued draws sample the old texels
        if (!entry.tex || entry.width != decodeW || entry.height != decodeH)
        {
            if (entry.tex)
            {
                if (batchUses(entry.tex))
                    flushBatch();
                releaseTexture(entry.tex);
            }
            entry.tex = makeColorTexture(decodeW, decodeH);
            entry.width = decodeW;
            entry.height = decodeH;
        }
        ScopedNs timer(statNs[2]);
        std::vector<uint32_t> texels(static_cast<size_t>(decodeW) * decodeH);
        const uint32_t tbw = std::max<uint32_t>(tex.tbw, 1u);
        for (int v = 0; v < decodeH; ++v)
            for (int u = 0; u < decodeW; ++u)
            {
                const uint32_t raw = readTexel(vram, psm, tex.tbp0, tbw, static_cast<uint32_t>(u), static_cast<uint32_t>(v));
                uint32_t value;
                if (fourBit)
                    value = palette[raw & 0x0Fu];
                else if (paletted)
                    value = palette[raw & 0xFFu];
                else if (psm == GS_PSM_CT16 || psm == GS_PSM_CT16S || psm == GS_PSM_Z16 || psm == GS_PSM_Z16S)
                    value = applyTexa(state.texa, psm, rgba5551To8888(raw));
                else
                    value = applyTexa(state.texa, psm, raw);
                texels[static_cast<size_t>(v) * decodeW + u] = value;
            }
        glBindTexture(GL_TEXTURE_2D, entry.tex);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, decodeW, decodeH, GL_RGBA, GL_UNSIGNED_BYTE, texels.data());
        glBindTexture(GL_TEXTURE_2D, 0);
        entry.signature = signature;
        ++statTexDecodes;
        return entry.tex;
    }

    // ------------------------------------------------------------ drawing

    // Uniforms of the draw program: locations resolved once, values sent only
    // when they change (samplers are fixed units, set once at link time).
    enum Uniform : int
    {
        U_tme, U_fst, U_iip, U_fge, U_tfx, U_tcc, U_texSize, U_linear, U_wrap, U_region, U_texMode, U_texa, U_fogColor, U_ate,
        U_atst, U_aref, U_dstRead, U_abe, U_pabe, U_blend, U_fix, U_date, U_datm, U_fba, U_frameFormat, U_texPages, U_texTarget,
        U_paletteMode, U_clut, U_fbmsk, U_zWrite, U_firstPrim, U_alphaPass, U_targetSize, U_stateBase, U_scale, U_zTest, U_colorMaskBits,
        U_count
    };
    static constexpr const char *kUniformNames[U_count] = {
        "u_tme", "u_fst", "u_iip", "u_fge", "u_tfx", "u_tcc", "u_texSize", "u_linear", "u_wrap", "u_region", "u_texMode", "u_texa",
        "u_fogColor", "u_ate", "u_atst", "u_aref", "u_dstRead", "u_abe", "u_pabe", "u_blend", "u_fix", "u_date", "u_datm", "u_fba",
        "u_frameFormat", "u_texPages", "u_texTarget", "u_paletteMode", "u_clut", "u_fbmsk", "u_zWrite", "u_firstPrim", "u_alphaPass",
        "u_targetSize", "u_stateBase", "u_scale", "u_zTest", "u_colorMaskBits"};
    // Per draw program (curProgram): locations and the values last sent.
    // Slots 0-5: the fixed programs; from kSpecFirst: specialised ones.
    static constexpr int kSpecFirst = 6, kSpecMax = 96;
    GLint uniformLoc[kSpecFirst + kSpecMax][U_count] = {};
    int32_t uniformValue[kSpecFirst + kSpecMax][U_count][4] = {};
    bool uniformSet[kSpecFirst + kSpecMax][U_count] = {};

    // Per-batch specialised programs (fast mode; GS_HW_TRI batches whose
    // states all share the SPEC_* fields): the fields as constants, built on
    // first use - in the background where the driver compiles in parallel
    // (GL_KHR_parallel_shader_compile) - with the generic program meanwhile.
    // GT3_GS_GL_SPECIALISE=0: off.
    struct SpecProgram
    {
        GLuint prog = 0;
        int slot = -1;
        bool ready = false, failed = false;
    };
    std::unordered_map<std::string, SpecProgram> specPrograms;
    GLuint specSlotProgram[kSpecMax] = {};
    int specSlots = 0;
    bool parallelCompile = false, specialise = false;
    std::string hwTriHeader;
    uint64_t statSpecBatches = 0, statSpecBuilt = 0;

    static bool sameSpecFields(const GpuState &a, const GpuState &b)
    {
        return a.v[0][1] == b.v[0][1] && a.v[0][2] == b.v[0][2] && a.v[0][3] == b.v[0][3] && a.v[1][0] == b.v[1][0] && a.v[1][1] == b.v[1][1] &&
               a.v[2][1] == b.v[2][1] && a.v[2][2] == b.v[2][2] && a.v[7][0] == b.v[7][0] && a.v[7][1] == b.v[7][1] && a.v[9][0] == b.v[9][0] &&
               a.v[9][1] == b.v[9][1] && a.v[8][0] == b.v[8][0] && a.v[8][1] == b.v[8][1] && a.v[8][2] == b.v[8][2] && a.v[8][3] == b.v[8][3] &&
               a.v[9][3] == b.v[9][3] && a.v[10][3] == b.v[10][3] && a.v[6][3] == b.v[6][3] && a.v[5][3] == b.v[5][3];
    }

    // The slot of the specialised program for a batch's states (-1: not (yet)
    // ready): each SPEC_* field all the states share becomes a constant; the
    // fields that differ stay per-fragment reads.
    int specProgramFor(const std::vector<GpuState> &st, bool noAlphaTest)
    {
        struct Field
        {
            const char *names[4];
            int row, col, count;
        };
        static constexpr Field kFields[] = {
            {{"SPEC_FST"}, 0, 1, 1},  {{"SPEC_IIP"}, 0, 2, 1},  {{"SPEC_FGE"}, 0, 3, 1},  {{"SPEC_TFX"}, 1, 0, 1},
            {{"SPEC_TCC"}, 1, 1, 1},  {{"SPEC_WRAPU", "SPEC_WRAPV"}, 2, 1, 2},             {{"SPEC_ATE"}, 7, 0, 1},
            {{"SPEC_ATST"}, 7, 1, 1}, {{"SPEC_ABE"}, 9, 0, 1},  {{"SPEC_PABE"}, 9, 1, 1},
            {{"SPEC_BLEND_A", "SPEC_BLEND_B", "SPEC_BLEND_C", "SPEC_BLEND_D"}, 8, 0, 4},  {{"SPEC_FBA"}, 9, 3, 1},
            {{"SPEC_HWBLEND"}, 10, 3, 1}, {{"SPEC_FRAME_FORMAT"}, 6, 3, 1},             {{"SPEC_FBMSK"}, 5, 3, 1}};
        std::string defines;
        for (const Field &f : kFields)
        {
            bool uniform = true;
            for (size_t i = 1; uniform && i < st.size(); ++i)
                for (int c = 0; c < f.count; ++c)
                    uniform = uniform && st[i].v[f.row][f.col + c] == st[0].v[f.row][f.col + c];
            if (!uniform)
                continue;
            for (int c = 0; c < f.count; ++c)
            {
                char line[64];
                if (f.row == 5)
                    std::snprintf(line, sizeof(line), "#define %s %uu\n", f.names[c], static_cast<unsigned>(st[0].v[f.row][f.col + c]));
                else
                    std::snprintf(line, sizeof(line), "#define %s %d\n", f.names[c], st[0].v[f.row][f.col + c]);
                defines += line;
            }
        }
        if (noAlphaTest)
            defines += "#define GS_NO_ALPHA_TEST 1\n";
        SpecProgram &sp = specPrograms[defines];
        if (sp.failed)
            return -1;
        if (!sp.prog)
        {
            if (specSlots >= kSpecMax)
            {
                sp.failed = true;
                return -1;
            }
            // Compiled without waiting for the status (that would block).
            auto start = [](GLenum type, const std::string &source) {
                const GLuint shader = glCreateShader(type);
                const char *text = source.c_str();
                glShaderSource(shader, 1, &text, nullptr);
                glCompileShader(shader);
                return shader;
            };
            const GLuint vs = start(GL_VERTEX_SHADER, hwTriHeader + defines + gsgl::kVertexShader);
            const GLuint fs = start(GL_FRAGMENT_SHADER, hwTriHeader + defines + gsgl::kFragmentShader);
            sp.prog = glCreateProgram();
            glAttachShader(sp.prog, vs);
            glAttachShader(sp.prog, fs);
            glLinkProgram(sp.prog);
            glDeleteShader(vs);
            glDeleteShader(fs);
            sp.slot = kSpecFirst + specSlots;
            specSlotProgram[specSlots++] = sp.prog;
        }
        if (!sp.ready)
        {
            if (parallelCompile)
            {
                GLint done = 0;
                glGetProgramiv(sp.prog, 0x91B1 /* GL_COMPLETION_STATUS_KHR */, &done);
                if (!done)
                    return -1;
            }
            GLint linked = 0;
            glGetProgramiv(sp.prog, GL_LINK_STATUS, &linked);
            if (!linked)
            {
                char log[2048];
                glGetProgramInfoLog(sp.prog, sizeof(log), nullptr, log);
                std::cerr << "[gs-gl] specialised program failed:\n" << log << std::endl;
                sp.failed = true;
                return -1;
            }
            resolveUniforms(sp.prog, sp.slot); // (unbinds the program)
            curProgram = -1;
            glStateDirty = true;
            sp.ready = true;
            ++statSpecBuilt;
        }
        return sp.slot;
    }

    void resolveUniforms(GLuint prog, int index)
    {
        glUseProgram(prog);
        for (int i = 0; i < U_count; ++i)
            uniformLoc[index][i] = glGetUniformLocation(prog, kUniformNames[i]);
        glUniform1i(glGetUniformLocation(prog, "u_tex"), 0);
        glUniform1i(glGetUniformLocation(prog, "u_dst"), 1);
        glUniform1i(glGetUniformLocation(prog, "u_texDepth"), 2);
        glUniform1i(glGetUniformLocation(prog, "u_palette"), 3);
        glUniform1i(glGetUniformLocation(prog, "u_clut16"), 3);
        glUniform1i(glGetUniformLocation(prog, "u_lut8"), 4);
        glUniform1i(glGetUniformLocation(prog, "u_lut4"), 5);
        glUniform1i(glGetUniformLocation(prog, "u_zbuf"), 10);
        glUniform1i(glGetUniformLocation(prog, "u_addrLut"), 8);
        glUniform1i(glGetUniformLocation(prog, "u_texRaw"), 9);
        if (gsmip::enabled())
            glUniform1i(glGetUniformLocation(prog, "u_log2Lut"), 12);
        glUseProgram(0);
    }

    void setU(Uniform u, int n, int32_t a, int32_t b = 0, int32_t c = 0, int32_t d = 0)
    {
        int32_t *v = uniformValue[curProgram][u];
        if (uniformSet[curProgram][u] && v[0] == a && v[1] == b && v[2] == c && v[3] == d)
            return;
        v[0] = a; v[1] = b; v[2] = c; v[3] = d;
        uniformSet[curProgram][u] = true;
        const GLint loc = uniformLoc[curProgram][u];
        if (u == U_fbmsk)
            glUniform1ui(loc, static_cast<GLuint>(a));
        else if (u == U_targetSize)
            glUniform2f(loc, static_cast<float>(a), static_cast<float>(b));
        else if (n == 1)
            glUniform1i(loc, a);
        else if (n == 2)
            glUniform2i(loc, a, b);
        else if (n == 3)
            glUniform3i(loc, a, b, c);
        else
            glUniform4i(loc, a, b, c, d);
    }

    void setUniforms(const DrawKey &k)
    {
        setU(U_tme, 1, k.tme);
        setU(U_fst, 1, k.fst);
        setU(U_iip, 1, k.iip);
        setU(U_fge, 1, k.fge);
        setU(U_tfx, 1, k.tfx);
        setU(U_tcc, 1, k.tcc);
        setU(U_texSize, 2, k.texW, k.texH);
        setU(U_linear, 1, k.linear);
        setU(U_wrap, 2, k.wrapU, k.wrapV);
        setU(U_region, 4, k.minU, k.maxU, k.minV, k.maxV);
        setU(U_texMode, 1, k.texMode);
        setU(U_texa, 3, k.ta0, k.aem, k.ta1);
        setU(U_fogColor, 3, k.fogR, k.fogG, k.fogB);
        setU(U_ate, 1, k.ate);
        setU(U_atst, 1, k.atst);
        setU(U_aref, 1, k.aref);
        setU(U_dstRead, 1, k.dstRead);
        setU(U_abe, 1, k.abe);
        setU(U_pabe, 1, k.pabe);
        setU(U_blend, 4, k.ba, k.bb, k.bc, k.bd);
        setU(U_fix, 1, k.fix);
        setU(U_date, 1, k.date);
        setU(U_datm, 1, k.datm);
        setU(U_fba, 1, k.fba);
        setU(U_frameFormat, 1, k.frameFormat);
        setU(U_texPages, 4, k.texBasePage, k.texPagesPerRow, k.texPageW, k.texPageH);
        setU(U_texTarget, 3, k.texTargetFbp, k.texTargetFbw, k.texTargetPageH);
        setU(U_paletteMode, 1, k.paletteMode);
        setU(U_clut, 3, k.clutCsa, k.clutFour, k.clutCt24);
        setU(U_fbmsk, 1, static_cast<int32_t>(k.fbmsk));
        setU(U_zWrite, 1, k.zte);
    }

    // Texture units 1-5 as last bound by flushBatch (unit 0 is also used for
    // uploads elsewhere and is always rebound), and the draw framebuffer's
    // attachments.
    GLuint boundUnit[11] = {}; // units 1-5 and 10 (6-9 belong to other passes)
    GLuint fboColor = 0, fboDepth = 0;
    GLuint boundImage = 0; // image unit 1 (the interlocked batches' target)
    void bindUnit(int unit, GLuint tex)
    {
        if (unit != 0 && boundUnit[unit] == tex)
            return;
        boundUnit[unit] = tex;
        glActiveTexture(GL_TEXTURE0 + unit);
        glBindTexture(GL_TEXTURE_2D, tex);
    }
    // After textures are deleted or replaced: forget cached bindings.
    void forgetBindings()
    {
        for (GLuint &t : boundUnit)
            t = 0xFFFFFFFFu;
        fboColor = fboDepth = 0xFFFFFFFFu;
        boundImage = 0xFFFFFFFFu;
        curDrawFbo = 0xFFFFFFFFu;
    }

    // Colour/depth write masks of the alpha pass (0: passing, 1: failing).
    void applyMasks(const DrawKey &k, int alphaPass)
    {
        int colorMask = k.colorMask;
        bool depthWrite = k.zte && !k.zmask;
        if (alphaPass == 1)
        {
            switch (k.afail)
            {
            case 1: depthWrite = false; break;                            // FB_ONLY
            case 2: colorMask = 0; break;                                  // ZB_ONLY
            case 3:                                                        // RGB_ONLY
                depthWrite = false;
                if (k.frameFormat == 0)
                    colorMask &= 0x7;
                break;
            default: colorMask = 0; depthWrite = false; break;
            }
        }
        const int masks = colorMask | (depthWrite ? 16 : 0);
        if (masks == curMasks)
            return;
        curMasks = masks;
        glColorMask((colorMask & 1) != 0, (colorMask & 2) != 0, (colorMask & 4) != 0, (colorMask & 8) != 0);
        glDepthMask(depthWrite ? GL_TRUE : GL_FALSE);
    }

    // A triangle's edge functions in integers: vertex positions are 12.4 fixed
    // point, so A and B are multiples of 1/16 and C of 1/256, and at a pixel
    // centre 512 * E = a * (2x + 1) * 16 + b * (2y + 1) * 16 + 2 * c exactly,
    // with a = 16 A, b = 16 B, c = 256 C.
    struct IntEdges
    {
        int64_t a[3], b[3], c[3];
        int own = 0;
    };
    // The coefficients are whole numbers here, so rounding to nearest (one
    // instruction on x64) gives what llround (a library call) did.
    static int64_t roundWhole(double x)
    {
#if defined(_M_X64) || defined(__x86_64__)
        return _mm_cvtsd_si64(_mm_set_sd(x));
#else
        return std::llround(x);
#endif
    }
    static IntEdges intEdges(const GpuPrim &p)
    {
        IntEdges e;
        for (int i = 0; i < 3; ++i)
        {
            e.a[i] = roundWhole(static_cast<double>(p.edgeA[i]) * 16.0);
            e.b[i] = roundWhole(static_cast<double>(p.edgeB[i]) * 16.0);
            e.c[i] = roundWhole(p.edgeC[i] * 256.0);
        }
        e.own = p.extra[1];
        return e;
    }

    // Span stepping of a triangle's edges over its cells. At pixel centre x of
    // a row an edge's value is v(x) = v(X) + A (x - X), A = 32 a; the pixel is
    // inside when v >= t (t 0 on an owned edge, else 1). For A > 0 that is
    // k = x - X >= ceil(D / A) with D = t - v(X); for A < 0, k <= floor(E / -A)
    // with E = v(X) - t; for A == 0 the whole row or nothing. The quotients
    // and remainders are kept exactly: one step per row (the value changes by
    // B = 32 b), eight per cell row, and -8 in the quotient per cell to the
    // right; so only the first cell of a triangle divides.
    struct SpanSteps
    {
        struct Edge
        {
            int64_t A = 0, B = 0;
            int64_t q = 0, r = 0;   // at the cell-row origin (cx0 * 8, cy * 8)
            int64_t qB = 0, rB = 0; // B / |A|
            int64_t q8 = 0, r8 = 0; // 8 B / |A|
            int64_t v = 0;          // A == 0: v - t at the cell-row origin
            int kind = 0;           // 1: A > 0, 2: A < 0, 0: A == 0
        } e[3];
        int cx0 = 0;
    };
    static void floorDiv(int64_t n, int64_t d, int64_t &q, int64_t &r)
    {
        q = n / d;
        r = n % d;
        if (r < 0)
        {
            r += d;
            --q;
        }
    }
    // cellRows: also the eight-row steps (nextCellRow); the row-by-row masks
    // (triangleMasksRows) step one row at a time and skip that division.
    static SpanSteps spanSteps(const IntEdges &e, int cx0, int cy0, bool cellRows = true)
    {
        SpanSteps s;
        s.cx0 = cx0;
        const int64_t X = static_cast<int64_t>(cx0) * 8, Y = static_cast<int64_t>(cy0) * 8;
        for (int i = 0; i < 3; ++i)
        {
            SpanSteps::Edge &g = s.e[i];
            const int64_t t = (e.own & (1 << i)) != 0 ? 0 : 1;
            const int64_t v = e.a[i] * (2 * X + 1) * 16 + e.b[i] * (2 * Y + 1) * 16 + 2 * e.c[i] - t;
            g.A = e.a[i] * 32;
            g.B = e.b[i] * 32;
            if (g.A > 0)
            {
                g.kind = 1;
                floorDiv(-v, g.A, g.q, g.r);
                floorDiv(g.B, g.A, g.qB, g.rB);
                if (cellRows)
                    floorDiv(8 * g.B, g.A, g.q8, g.r8);
            }
            else if (g.A < 0)
            {
                g.kind = 2;
                floorDiv(v, -g.A, g.q, g.r);
                floorDiv(g.B, -g.A, g.qB, g.rB);
                if (cellRows)
                    floorDiv(8 * g.B, -g.A, g.q8, g.r8);
            }
            else
            {
                g.kind = 0;
                g.v = v;
            }
        }
        return s;
    }
    // To the next cell row (eight pixel rows down).
    static void nextCellRow(SpanSteps &s)
    {
        for (SpanSteps::Edge &g : s.e)
        {
            if (g.kind == 1)
            {
                g.q -= g.q8;
                g.r -= g.r8;
                if (g.r < 0)
                {
                    g.r += g.A;
                    --g.q;
                }
            }
            else if (g.kind == 2)
            {
                g.q += g.q8;
                g.r += g.r8;
                if (g.r >= -g.A)
                {
                    g.r += g.A;
                    ++g.q;
                }
            }
            else
            {
                g.v += 8 * g.B;
            }
        }
    }
    // The pixels of rows y0..y1, columns x0..x1 of cell (cx, cy) inside every
    // edge crossing the cell (edgeInside false), exactly as the per-pixel test.
    static uint64_t spanMask(const SpanSteps &s, const bool edgeInside[3], int x0, int y0, int x1, int y1, int cx, int cy)
    {
        int64_t q[3] = {0, 0, 0}, r[3] = {0, 0, 0}, v[3] = {0, 0, 0};
        int idx[3];
        int count = 0;
        const int64_t dcx = cx - s.cx0;
        for (int i = 0; i < 3; ++i)
            if (!edgeInside[i])
            {
                idx[count++] = i;
                q[i] = s.e[i].q - 8 * dcx;
                r[i] = s.e[i].r;
                v[i] = s.e[i].v;
            }
        const int64_t kx0 = x0 - cx * 8, kx1 = x1 - cx * 8;
        const int j0 = y0 - cy * 8, j1 = y1 - cy * 8;
        uint64_t mask = 0ull;
        for (int j = 0; j <= j1; ++j)
        {
            int64_t lo = kx0, hi = kx1;
            for (int n = 0; n < count; ++n)
            {
                const int i = idx[n];
                const SpanSteps::Edge &g = s.e[i];
                if (g.kind == 1)
                {
                    lo = std::max(lo, q[i] + (r[i] != 0 ? 1 : 0));
                    q[i] -= g.qB;
                    r[i] -= g.rB;
                    if (r[i] < 0)
                    {
                        r[i] += g.A;
                        --q[i];
                    }
                }
                else if (g.kind == 2)
                {
                    hi = std::min(hi, q[i]);
                    q[i] += g.qB;
                    r[i] += g.rB;
                    if (r[i] >= -g.A)
                    {
                        r[i] += g.A;
                        ++q[i];
                    }
                }
                else
                {
                    if (v[i] < 0)
                        hi = -1;
                    v[i] += g.B;
                }
            }
            if (j >= j0 && lo <= hi)
                mask |= (((hi - lo == 7) ? 0xFFull : ((1ull << (hi - lo + 1)) - 1ull)) << lo) << (j * 8);
        }
        return mask;
    }

    // The pixels of an 8x8 cell (cx, cy) a primitive covers, as the shader
    // decides coverage (exact edge functions and edge ownership for triangles).
    static uint64_t coverageMask(const GpuPrim &p, const IntEdges &e, const SpanSteps &steps, int cx, int cy)
    {
        const int x0 = std::max(p.bbox[0], cx * 8), x1 = std::min(p.bbox[2], cx * 8 + 7);
        const int y0 = std::max(p.bbox[1], cy * 8), y1 = std::min(p.bbox[3], cy * 8 + 7);
        if (x1 < x0 || y1 < y0)
            return 0ull;
        const uint64_t rowBits = ((x1 - x0 == 7) ? 0xFFull : ((1ull << (x1 - x0 + 1)) - 1ull)) << (x0 - cx * 8);
        if (p.extra[0] != 0)
        {
            uint64_t rect = 0ull; // sprite: its box
            for (int y = y0; y <= y1; ++y)
                rect |= rowBits << ((y - cy * 8) * 8);
            return rect;
        }
        // Corners of the clipped cell: all inside every edge (a triangle is
        // convex) -> the whole rectangle; all outside one edge -> nothing.
        bool allInside = true;
        bool edgeInside[3];
        for (int i = 0; i < 3; ++i)
        {
            const int64_t base = 2 * e.c[i];
            const int64_t vx0 = e.a[i] * (2 * x0 + 1) * 16, vx1 = e.a[i] * (2 * x1 + 1) * 16;
            const int64_t vy0 = e.b[i] * (2 * y0 + 1) * 16, vy1 = e.b[i] * (2 * y1 + 1) * 16;
            const int64_t c00 = vx0 + vy0 + base, c10 = vx1 + vy0 + base, c01 = vx0 + vy1 + base, c11 = vx1 + vy1 + base;
            if (c00 < 0 && c10 < 0 && c01 < 0 && c11 < 0)
                return 0ull;
            const bool own = (e.own & (1 << i)) != 0;
            auto in = [own](int64_t v) { return v > 0 || (v == 0 && own); };
            edgeInside[i] = in(c00) && in(c10) && in(c01) && in(c11);
            if (!edgeInside[i])
                allInside = false;
        }
        if (allInside)
        {
            uint64_t rect = 0ull;
            for (int y = y0; y <= y1; ++y)
                rect |= rowBits << ((y - cy * 8) * 8);
            return rect;
        }
        static const bool verify = envFlag("GT3_GS_GL_COVER_VERIFY", false);
        const uint64_t spans = spanMask(steps, edgeInside, x0, y0, x1, y1, cx, cy);
        if (!verify)
            return spans;
        uint64_t mask = 0ull;
        for (int y = y0; y <= y1; ++y)
        {
            int64_t v[3];
            for (int i = 0; i < 3; ++i)
                v[i] = e.a[i] * (2 * x0 + 1) * 16 + e.b[i] * (2 * y + 1) * 16 + 2 * e.c[i];
            const int shift = (y - cy * 8) * 8 - cx * 8;
            for (int x = x0; x <= x1; ++x)
            {
                bool in = true;
                for (int i = 0; i < 3; ++i)
                    if (v[i] < 0 || (v[i] == 0 && (e.own & (1 << i)) == 0))
                        in = false;
                if (in)
                    mask |= 1ull << (shift + x);
                for (int i = 0; i < 3; ++i)
                    v[i] += e.a[i] * 32;
            }
        }
        static uint64_t cells = 0, wrong = 0;
        ++cells;
        if (mask != spans && wrong++ < 20u)
            std::fprintf(stderr, "[gs-gl] cover verify MISMATCH cell %d,%d: pixels %016llx spans %016llx\n", cx, cy, (unsigned long long)mask,
                         (unsigned long long)spans);
        if ((cells & 0xFFFFFu) == 0u)
            std::fprintf(stderr, "[gs-gl] cover verify: %llu partial cells, %llu mismatches\n", (unsigned long long)cells, (unsigned long long)wrong);
        return mask;
    }



    // Levels for a batch whose primitives read the target (blending, DATE) or
    // are drawn in two alpha passes: a primitive's level is one more than the
    // highest level of an earlier primitive sharing a pixel with it (coverage
    // exact, as the shader decides it), so overlapping primitives keep their
    // order while each level is one draw. prims is reordered level by level,
    // stable within a level; levelStart[l] is where level l begins.
    // At an internal resolution the levels come from the primitives in
    // sample coordinates (toSamples), so coverage is exact per sample. Cells
    // are 8x8 pixels (samples); natively the grid is 256x256 cells from 0,
    // else the batch's cell rectangle (cellLimit: the largest cell index).
    static inline int cellLimit = 255; // this batch's (255 for pixel levels)
    int sampleCellLimit = 255;
    struct GridFrame
    {
        int x0 = 0, y0 = 0, pitch = 256;
    };
    GridFrame grid;
    std::vector<GpuPrim> samplePrims;
    // GT3_GS_GL_NATIVE_LEVELS=1 (at a scale): levels from pixel-centre
    // coverage (faster; overlaps thinner than a pixel may then race). A batch
    // of sprites only uses them anyway: sprites cover whole pixels, so pixel
    // levels are exact per sample (batchSampleLevels, set per batch).
    bool batchSampleLevels = false;
    bool sampleLevels() const
    {
        static const bool nativeLevels = envFlag("GT3_GS_GL_NATIVE_LEVELS", false);
        return scaled() && !nativeLevels;
    }

    // A primitive's cell rectangle (clipped to the grid).
    static void cellRect(const GpuPrim &p, int &cx0, int &cy0, int &cx1, int &cy1)
    {
        cx0 = std::max(p.bbox[0] >> 3, 0);
        cy0 = std::max(p.bbox[1] >> 3, 0);
        cx1 = std::min(p.bbox[2] >> 3, cellLimit);
        cy1 = std::min(p.bbox[3] >> 3, cellLimit);
    }

    // A primitive in sample coordinates: its box, and a triangle's edge
    // functions multiplied by scaleX * scaleY (A by scaleY, B by scaleX), which
    // at sample centres have the signs the shader's have (exactly for scales
    // of 1, 2, 4 and 8, as its sample positions are exact then).
    GpuPrim toSamples(const GpuPrim &p) const
    {
        // (The batch's destination scale: effects at their own scale.)
        const Target *bt = key.colorTarget >= 0 && static_cast<size_t>(key.colorTarget) < targets.size() ? targets[key.colorTarget].get() : nullptr;
        const int scaleX = bt ? bt->sx : this->scaleX, scaleY = bt ? bt->sy : this->scaleY;
        GpuPrim q = p;
        q.bbox[0] = p.bbox[0] * scaleX;
        q.bbox[1] = p.bbox[1] * scaleY;
        q.bbox[2] = (p.bbox[2] + 1) * scaleX - 1;
        q.bbox[3] = (p.bbox[3] + 1) * scaleY - 1;
        if (!p.extra[0])
            for (int i = 0; i < 3; ++i)
            {
                q.edgeA[i] = p.edgeA[i] * static_cast<float>(scaleY);
                q.edgeB[i] = p.edgeB[i] * static_cast<float>(scaleX);
                q.edgeC[i] = p.edgeC[i] * static_cast<double>(scaleX * scaleY);
            }
        return q;
    }

    // The masks of every cell of triangle p, row by row, into out.
    void triangleMasks(const GpuPrim &p, uint64_t *out) const
    {
        // GT3_GS_GL_ROW_MASKS=0: per-cell masks (triangleMasksOf); =verify:
        // both, compared (mismatches logged).
        static const int rowMode = [] {
            const char *v = std::getenv("GT3_GS_GL_ROW_MASKS");
            if (v && std::strcmp(v, "0") == 0)
                return 0;
            if (v && std::strcmp(v, "verify") == 0)
                return 2;
            return 1;
        }();
        static const bool boxLevels = envFlag("GT3_GS_GL_BOX_LEVELS", false);
        static const bool coverVerify = envFlag("GT3_GS_GL_COVER_VERIFY", false);
        if (rowMode == 0 || boxLevels || coverVerify)
        {
            triangleMasksOf(p, out);
            return;
        }
        triangleMasksRows(p, out);
        if (rowMode == 2)
        {
            int cx0, cy0, cx1, cy1;
            cellRect(p, cx0, cy0, cx1, cy1);
            const size_t cells = static_cast<size_t>(cx1 - cx0 + 1) * static_cast<size_t>(cy1 - cy0 + 1);
            std::vector<uint64_t> ref(cells);
            triangleMasksOf(p, ref.data());
            static std::atomic<uint64_t> checked{0}, wrong{0};
            const uint64_t n = ++checked;
            if (std::memcmp(ref.data(), out, cells * sizeof(uint64_t)) != 0 && wrong++ < 20u)
                std::fprintf(stderr, "[gs-gl] row masks MISMATCH: bbox %d,%d-%d,%d\n", p.bbox[0], p.bbox[1], p.bbox[2], p.bbox[3]);
            if ((n & 0xFFFFFu) == 0u)
                std::fprintf(stderr, "[gs-gl] row masks verify: %llu triangles, %llu mismatches\n", static_cast<unsigned long long>(n),
                             static_cast<unsigned long long>(wrong.load()));
        }
    }

    // The same masks as triangleMasksOf, built a pixel row at a time over the
    // whole cell rectangle: each row's span inside all three edges comes
    // from the same exact integer stepping (SpanSteps) once, and its bits go
    // into the cells it crosses. Per cell, the edges were stepped again for
    // every cell of a row (the masks were ~12% of the GS renderer thread).
    static void triangleMasksRows(const GpuPrim &p, uint64_t *out)
    {
        int cx0, cy0, cx1, cy1;
        cellRect(p, cx0, cy0, cx1, cy1);
        if (cx1 < cx0 || cy1 < cy0)
            return;
        const int cw = cx1 - cx0 + 1, ch = cy1 - cy0 + 1;
        std::fill_n(out, static_cast<size_t>(cw) * static_cast<size_t>(ch), 0ull);
        const IntEdges edges = intEdges(p);
        const SpanSteps s = spanSteps(edges, cx0, cy0, false);
        int64_t q[3], r[3], v[3];
        for (int i = 0; i < 3; ++i)
        {
            q[i] = s.e[i].q;
            r[i] = s.e[i].r;
            v[i] = s.e[i].v;
        }
        const int X0 = cx0 * 8, Y0 = cy0 * 8;
        const int64_t kx0 = std::max(p.bbox[0], X0) - X0, kx1 = std::min(p.bbox[2], cx1 * 8 + 7) - X0;
        const int y0 = std::max(p.bbox[1], Y0), y1 = std::min(p.bbox[3], cy1 * 8 + 7);
        for (int y = Y0; y <= y1; ++y)
        {
            int64_t lo = kx0, hi = kx1;
            for (int i = 0; i < 3; ++i)
            {
                const SpanSteps::Edge &g = s.e[i];
                if (g.kind == 1)
                {
                    lo = std::max(lo, q[i] + (r[i] != 0 ? 1 : 0));
                    q[i] -= g.qB;
                    r[i] -= g.rB;
                    if (r[i] < 0)
                    {
                        r[i] += g.A;
                        --q[i];
                    }
                }
                else if (g.kind == 2)
                {
                    hi = std::min(hi, q[i]);
                    q[i] += g.qB;
                    r[i] += g.rB;
                    if (r[i] >= -g.A)
                    {
                        r[i] += g.A;
                        ++q[i];
                    }
                }
                else
                {
                    if (v[i] < 0)
                        hi = -1;
                    v[i] += g.B;
                }
            }
            if (y < y0 || lo > hi)
                continue;
            uint64_t *row = out + static_cast<size_t>((y - Y0) >> 3) * static_cast<size_t>(cw);
            const int shift = ((y - Y0) & 7) * 8;
            const int c0 = static_cast<int>(lo >> 3), c1 = static_cast<int>(hi >> 3);
            for (int c = c0; c <= c1; ++c)
            {
                const int b0 = c == c0 ? static_cast<int>(lo & 7) : 0, b1 = c == c1 ? static_cast<int>(hi & 7) : 7;
                row[c] |= (((b1 - b0 == 7) ? 0xFFull : ((1ull << (b1 - b0 + 1)) - 1ull)) << b0) << shift;
            }
        }
    }
    static void triangleMasksOf(const GpuPrim &p, uint64_t *out)
    {
        // GT3_GS_GL_BOX_LEVELS=1 (diagnostic): whole cells of the box, conservative.
        static const bool boxLevels = envFlag("GT3_GS_GL_BOX_LEVELS", false);
        if (boxLevels)
        {
            int bx0, by0, bx1, by1;
            cellRect(p, bx0, by0, bx1, by1);
            for (int cy = by0; cy <= by1; ++cy)
                for (int cx = bx0; cx <= bx1; ++cx)
                    *out++ = ~0ull;
            return;
        }
        const IntEdges edges = intEdges(p);
        int cx0, cy0, cx1, cy1;
        cellRect(p, cx0, cy0, cx1, cy1);
        SpanSteps steps = spanSteps(edges, cx0, cy0);
        for (int cy = cy0; cy <= cy1; ++cy, nextCellRow(steps))
            for (int cx = cx0; cx <= cx1; ++cx)
                *out++ = coverageMask(p, edges, steps, cx, cy);
    }

    // A primitive's level from the cells it covers (masks: one per cell of
    // its cell rectangle, row by row), and its pixels added to those cells.
    static int placeInCells(CellLevels *grid, const GridFrame &frame, uint32_t stamp, int cx0, int cy0, int cx1, int cy1, const uint64_t *masks)
    {
        int level = 0;
        size_t mi = 0;
        for (int cy = cy0; cy <= cy1; ++cy)
            for (int cx = cx0; cx <= cx1; ++cx)
            {
                const uint64_t mask = masks[mi++];
                if (!mask)
                    continue;
                const CellLevels &c = grid[static_cast<size_t>(cy - frame.y0) * static_cast<size_t>(frame.pitch) + static_cast<size_t>(cx - frame.x0)];
                if (c.stamp != stamp)
                    continue;
                for (int e = 0; e < c.count; ++e)
                    if ((c.mask[e] & mask) != 0ull)
                        level = std::max(level, c.level[e] + 1);
            }
        level = std::min(level, 65535);
        size_t m = 0;
        for (int cy = cy0; cy <= cy1; ++cy)
            for (int cx = cx0; cx <= cx1; ++cx, ++m)
            {
                const uint64_t mask = masks[m];
                if (!mask)
                    continue;
                CellLevels &c = grid[static_cast<size_t>(cy - frame.y0) * static_cast<size_t>(frame.pitch) + static_cast<size_t>(cx - frame.x0)];
                if (c.stamp != stamp)
                {
                    c.stamp = stamp;
                    c.count = 0;
                }
                int slot = -1;
                for (int e = 0; e < c.count; ++e)
                    if (c.level[e] == level)
                        slot = e;
                if (slot < 0 && c.count == 4)
                {
                    // Full: merge the lowest level into the next lowest.
                    int lo = 0;
                    for (int e = 1; e < 4; ++e)
                        if (c.level[e] < c.level[lo])
                            lo = e;
                    int next = lo == 0 ? 1 : 0;
                    for (int e = 0; e < 4; ++e)
                        if (e != lo && c.level[e] < c.level[next])
                            next = e;
                    c.mask[next] |= c.mask[lo];
                    c.level[lo] = c.level[c.count - 1];
                    c.mask[lo] = c.mask[c.count - 1];
                    --c.count;
                    for (int e = 0; e < c.count; ++e)
                        if (c.level[e] == level)
                            slot = e;
                }
                if (slot < 0)
                {
                    slot = c.count++;
                    c.level[slot] = static_cast<uint16_t>(level);
                    c.mask[slot] = 0ull;
                }
                c.mask[slot] |= mask;
            }
        return level;
    }

    // GL state flushBatch keeps between batches (the conversion, presentation
    // and self-test passes change it and set glStateDirty).
    bool glStateDirty = true;
    int curVpW = -1, curVpH = -1, curDepthTest = -1, curDepthFunc = -1, curMasks = -1;

    std::vector<size_t> maskOffset;
    std::vector<uint64_t> maskBuf;
    std::vector<size_t> maskChunk;

    // Levels of prims[first, last) placed in grid (after [0, first)) into
    // levels[i]; maxLevel is raised to the highest. Triangle masks first:
    // independent per primitive, so a large range computes them on the helper
    // threads. Sprites' are made in the pass.
    void placeRange(CellLevels *grid, uint32_t stamp, size_t first, size_t last, uint16_t *levels, uint16_t &maxLevel)
    {
        const GpuPrim *src = batchSampleLevels ? samplePrims.data() : prims.data();
        const size_t n = last - first;
        maskOffset.resize(n + 1u);
        size_t total = 0;
        for (size_t k = 0; k < n; ++k)
        {
            maskOffset[k] = total;
            const GpuPrim &p = src[first + k];
            int cx0, cy0, cx1, cy1;
            cellRect(p, cx0, cy0, cx1, cy1);
            if (cx1 >= cx0 && cy1 >= cy0)
                total += static_cast<size_t>(cx1 - cx0 + 1) * static_cast<size_t>(cy1 - cy0 + 1);
        }
        maskOffset[n] = total;
        if (maskBuf.size() < total)
            maskBuf.resize(total);
        // Every primitive's masks (a sprite's: its box), on the helpers for a large range.
        auto computeRange = [&](size_t from, size_t to) {
            for (size_t k = from; k < to; ++k)
            {
                if (maskOffset[k + 1] == maskOffset[k])
                    continue;
                const GpuPrim &p = src[first + k];
                uint64_t *out = maskBuf.data() + maskOffset[k];
                if (!p.extra[0])
                {
                    triangleMasks(p, out);
                    continue;
                }
                int cx0, cy0, cx1, cy1;
                cellRect(p, cx0, cy0, cx1, cy1);
                const SpanSteps none{};
                for (int cy = cy0; cy <= cy1; ++cy)
                    for (int cx = cx0; cx <= cx1; ++cx)
                        *out++ = coverageMask(p, IntEdges{}, none, cx, cy);
            }
        };
        static const bool verifying = envFlag("GT3_GS_GL_COVER_VERIFY", false);
        MaskPool &pool = MaskPool::instance();
        if (!verifying && total >= 2048u && n >= 8u && pool.helpers() > 0u)
        {
            const size_t tasks = std::min<size_t>(n, static_cast<size_t>(pool.helpers() + 1u) * 3u);
            maskChunk.resize(tasks + 1u);
            for (size_t t = 0; t <= tasks; ++t)
                maskChunk[t] = static_cast<size_t>(std::lower_bound(maskOffset.begin(), maskOffset.begin() + static_cast<std::ptrdiff_t>(n) + 1,
                                                                    total * t / tasks) -
                                                   maskOffset.begin());
            maskChunk[tasks] = n;
            pool.run(static_cast<unsigned>(tasks), [&](unsigned t) { computeRange(maskChunk[t], maskChunk[t + 1u]); });
            ++statParallelBatches;
        }
        else
        {
            computeRange(0, n);
        }

        for (size_t k = 0; k < n; ++k)
        {
            const GpuPrim &p = src[first + k];
            int cx0, cy0, cx1, cy1;
            cellRect(p, cx0, cy0, cx1, cy1);
            const uint64_t *masks = maskBuf.data() + maskOffset[k];
            if (maskOffset[k + 1] == maskOffset[k])
                cx1 = cx0 - 1; // no cells
            const int level = placeInCells(grid, this->grid, stamp, cx0, cy0, cx1, cy1, masks);
            levels[first + k] = static_cast<uint16_t>(level);
            maxLevel = std::max<uint16_t>(maxLevel, static_cast<uint16_t>(level));
        }
    }

    // All levels here, at the flush (no helper).
    void orderByLevels()
    {
        ScopedNs timer(statNs[7]);
        ++levelStamp;
        const size_t n = prims.size();
        primLevels.resize(n);
        uint16_t maxLevel = 0;
        if (n == 1)
        {
            // One primitive: one level.
            primLevels[0] = 0;
            sortByLevels(0);
            return;
        }
        batchSampleLevels = sampleLevels() && std::any_of(prims.begin(), prims.end(), [](const GpuPrim &p) { return p.extra[0] == 0; });
        cellLimit = batchSampleLevels ? sampleCellLimit : 255;
        grid = GridFrame{};
        if (batchSampleLevels)
        {
            samplePrims.resize(n);
            int gx0 = INT32_MAX, gy0 = INT32_MAX, gx1 = -1, gy1 = -1;
            for (size_t i = 0; i < n; ++i)
            {
                samplePrims[i] = toSamples(prims[i]);
                int cx0, cy0, cx1, cy1;
                cellRect(samplePrims[i], cx0, cy0, cx1, cy1);
                gx0 = std::min(gx0, cx0);
                gy0 = std::min(gy0, cy0);
                gx1 = std::max(gx1, cx1);
                gy1 = std::max(gy1, cy1);
            }
            if (gx1 < gx0 || gy1 < gy0)
                gx0 = gy0 = gx1 = gy1 = 0;
            grid = {gx0, gy0, gx1 - gx0 + 1};
            const size_t cells = static_cast<size_t>(grid.pitch) * static_cast<size_t>(gy1 - gy0 + 1);
            if (cellLevels.size() < cells)
                cellLevels.resize(cells);
        }
        placeRange(cellLevels.data(), levelStamp, 0, n, primLevels.data(), maxLevel);
        sortByLevels(maxLevel);
    }

    // prims reordered by primLevels (stable counting sort); levelStart[l] is
    // where level l begins.
    void sortByLevels(uint16_t maxLevel)
    {
        levelStart.assign(static_cast<size_t>(maxLevel) + 2u, 0u);
        for (const uint16_t level : primLevels)
            ++levelStart[static_cast<size_t>(level) + 1u];
        for (size_t l = 1; l < levelStart.size(); ++l)
            levelStart[l] += levelStart[l - 1];
        if (maxLevel == 0)
            return;
        levelOrder.resize(prims.size());
        std::vector<size_t> cursor(levelStart.begin(), levelStart.end() - 1);
        for (size_t i = 0; i < prims.size(); ++i)
            levelOrder[cursor[primLevels[i]]++] = prims[i];
        prims.swap(levelOrder);
    }

    // where: the caller (statistics: flushes by call site, step 7 T0).
    void flushBatch(std::source_location where = std::source_location::current())
    {
        if (!haveBatch)
            return;
        ++statFlushCount;
        if (stats)
        {
            FlushSite &site = statFlushSites[where.line()];
            ++site.count;
            site.function = where.function_name();
            if (!prims.empty())
            {
                statBatchStates += states.size();
                ++statStateBatches;
            }
        }
        // glFlush every 8 batches (GT3_GS_GL_FLUSH_EVERY=<n>, 0: never), so the
        // GPU works through queued draws as they come instead of when a read
        // back forces it. GT3's race reads a 64x8 area back every frame; that
        // wait went from about 75 to 6 ms per second, and the busy race from
        // 54 to 58 fps (every batch: 55-56, every 4-8: 57-58). Every 2 (the
        // old default) the flushes were 7% of the GS thread at Trial
        // Mountain's start (8: 2-3 fps more there).
        static const uint32_t flushEvery = []
        {
            const char *v = std::getenv("GT3_GS_GL_FLUSH_EVERY");
            return v ? static_cast<uint32_t>(std::strtoul(v, nullptr, 10)) : GS_FLUSH_EVERY_DEFAULT;
        }();
        if (flushEvery != 0u && (statFlushCount % flushEvery) == 0u)
            glFlush();
        ScopedNs timer(statNs[3]);
        haveBatch = false;
        if (prims.empty())
        {
            states.clear();
            lastStateIndex = -1;
            batchTextures.clear();
            return;
        }
        // The batch writes its colour target: a direct CLUT read from it is
        // copied first (the batch itself reads the copy: see choosePalette).
        if (clutDirect && targets[key.colorTarget] && targets[key.colorTarget]->tex == gpuClutTex)
            materializeDirectClut();
        const DrawKey k = key;
        Target &color = *targets[k.colorTarget];
        const bool twoPass = k.ate && k.atst != 1 && k.afail != 0;
        // A blending batch that writes no depth: one draw with the interlock
        // program (no levels).
        // GT3_GS_GL_INTERLOCK_SKIP_FF=<frame format>: those batches keep levels (diagnostic).
        static const int skipFrameFormat = [] {
            const char *v = std::getenv("GT3_GS_GL_INTERLOCK_SKIP_FF");
            return (v && *v) ? std::atoi(v) : -1;
        }();
        const bool interlocked = useInterlock() && k.needsSplit && k.dstRead && !twoPass && !(k.zte && !k.zmask) && k.frameFormat != skipFrameFormat;

        // Fast mode: an alpha test with a fail action and no target read in two
        // whole-batch passes (passing fragments, then failing ones), without
        // overlap levels, as PCSX2 draws it. Not exact where a failing fragment
        // and a later primitive's passing one overlap; never native.
        // GT3_GS_GL_WHOLE_TWO_PASS=0: fast mode keeps overlap levels here (diagnostic).
        static const bool wholeTwoPassOn = envFlag("GT3_GS_GL_WHOLE_TWO_PASS", true);
        const bool wholeTwoPass = fastBlend && wholeTwoPassOn && twoPass && !k.dstRead;
        // (orderFreeMasks: one level; see there.)
        bool orderFree = orderFreeMasks && color.tall && k.needsSplit && !interlocked && !wholeTwoPass && !twoPass && !k.date &&
                         k.frameFormat == 0 && !(k.zte && !k.zmask) && !prims.empty();
        if (orderFree)
        {
            const int32_t mask = states[static_cast<size_t>(prims[0].extra[3])].v[5][3];
            orderFree = (static_cast<uint32_t>(mask) & 0x00FFFFFFu) == 0x00FFFFFFu;
            for (size_t i = 0; orderFree && i < states.size(); ++i)
                orderFree = states[i].v[5][3] == mask && states[i].v[10][1] == 0;
        }
        if (orderFree)
        {
            levelStart.assign({size_t{0}, prims.size()});
            ++statOrderFreeBatches;
        }
        else if (k.needsSplit && !interlocked && !wholeTwoPass)
        {
            orderByLevels();
        }
        std::optional<ScopedNs> uploadTimer(std::in_place, statNs[5]); // (no allocation: once per batch)
        const size_t bytes = prims.size() * sizeof(GpuPrim);
        const size_t stateBytesRing = states.size() * sizeof(GpuState);
        size_t primOffset = 0, stateOffset = 0;
        uint8_t *primDst = ringAlloc(bytes, primOffset);
        uint8_t *stateDst = primDst ? ringAlloc(stateBytesRing, stateOffset) : nullptr;
        int32_t primBase = 0, stateBase = 0;
        if (primDst && stateDst)
        {
            std::memcpy(primDst, prims.data(), bytes);
            std::memcpy(stateDst, states.data(), stateBytesRing);
            if (ringWhole)
            {
                if (!ringBound)
                {
                    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, uploadRing);
                    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 1, uploadRing);
                    ringBound = true;
                }
                primBase = static_cast<int32_t>(primOffset / sizeof(GpuPrim));
                stateBase = static_cast<int32_t>(stateOffset / sizeof(GpuState));
            }
            else
            {
                glBindBufferRange(GL_SHADER_STORAGE_BUFFER, 0, uploadRing, static_cast<GLintptr>(primOffset), static_cast<GLsizeiptr>(bytes));
                glBindBufferRange(GL_SHADER_STORAGE_BUFFER, 1, uploadRing, static_cast<GLintptr>(stateOffset),
                                  static_cast<GLsizeiptr>(std::max<size_t>(stateBytesRing, 16u)));
            }
        }
        else
        {
        ringBound = false;
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, primBuffer);
        if (bytes > primBufferBytes)
        {
            primBufferBytes = std::max(bytes, primBufferBytes * 2u);
            glBufferData(GL_SHADER_STORAGE_BUFFER, static_cast<GLsizeiptr>(primBufferBytes), nullptr, GL_STREAM_DRAW);
        }
        glBufferSubData(GL_SHADER_STORAGE_BUFFER, 0, static_cast<GLsizeiptr>(bytes), prims.data());
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, primBuffer);
        const size_t stateBytes = states.size() * sizeof(GpuState);
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, stateBuffer);
        if (stateBytes > stateBufferBytes)
        {
            stateBufferBytes = std::max(stateBytes, stateBufferBytes * 2u);
            glBufferData(GL_SHADER_STORAGE_BUFFER, static_cast<GLsizeiptr>(stateBufferBytes), nullptr, GL_STREAM_DRAW);
        }
        glBufferSubData(GL_SHADER_STORAGE_BUFFER, 0, static_cast<GLsizeiptr>(stateBytes), states.data());
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 1, stateBuffer);
        }
        if (carLightingPerPixel())
            bindLit();
        uploadTimer.reset();
        std::optional<ScopedNs> stateTimer(std::in_place, statNs[6]);

        static const bool noStateCache = envFlag("GT3_GS_GL_NO_STATE_CACHE", false); // diagnostic
        if (noStateCache)
            glStateDirty = true;
        if (glStateDirty)
        {
            glBindFramebuffer(GL_FRAMEBUFFER, fbo);
            curDrawFbo = 0xFFFFFFFFu;
            glDrawBuffer(GL_COLOR_ATTACHMENT0);
            glDisable(GL_DITHER);
            glDisable(GL_SCISSOR_TEST);
            glDisable(GL_BLEND);
            glDisable(GL_CULL_FACE);
            glDisable(GL_STENCIL_TEST);
            glUseProgram(program);
            curProgram = 0;
            glBindVertexArray(vao);
            curVpW = curVpH = curDepthTest = curDepthFunc = curMasks = -1;
            onInterlockFbo = false;
            glStateDirty = false;
        }
        // Fast mode: the rasterizing program (the GPU's coverage and depth). Not for a Z16 buffer (k.zte 2) with a
        // vertex Z above 0xFFFF: its Z is clamped to 0xFFFF per pixel after interpolation, which the exact program
        // does (depth clamp: 2^24 only); below that the interpolated Z never exceeds 0xFFFF (GT4: never above).
        static const bool z16Fast = envFlag("PS2X_GS_GL_Z16_FAST", true);
        const bool fastRaster = fastRasterActive && !interlocked && (k.zte != 2 || (z16Fast && !batchZ16Above));
        // The lean variant when every state of the batch fits it.
        bool spec = fastRaster && specTex15Program != 0;
        for (size_t i = 0; spec && i < states.size(); ++i)
        {
            const GpuState &st = states[i];
            spec = st.v[0][0] == 1 && st.v[2][3] == 15 && st.v[10][0] == 0 && st.v[10][1] == 0;
        }
        // ... and when the batch is triangles only, the variant whose
        // attributes the GPU interpolates.
        bool hwTri = spec && hwTriProgram != 0;
        for (size_t i = 0; hwTri && i < prims.size(); ++i)
            hwTri = prims[i].extra[0] == 0;
        // ... and when no state alpha-tests (and one pass), the variant with early depth tests.
        bool noAlphaTest = hwTri && hwTriNoAtProgram != 0 && !twoPass;
        for (size_t i = 0; noAlphaTest && i < states.size(); ++i)
            noAlphaTest = states[i].v[7][0] == 0 || states[i].v[7][1] == 1; // ATE off, or ATST ALWAYS
        // ... and when every state shares the SPEC_* fields, the program with them as constants.
        int specSlot = -1;
        if (hwTri && specialise)
        {
            specSlot = specProgramFor(states, noAlphaTest);
            if (specSlot >= 0)
                ++statSpecBatches;
        }
        const int wantProgram = specSlot >= 0 ? specSlot : interlocked ? 1 : (fastRaster ? (hwTri ? (noAlphaTest ? 5 : 4) : (spec ? 3 : 2)) : 0);
        if (stats && hwTri)
        {
            auto describe = [](const GpuState &s) {
                char text[200];
                std::snprintf(text, sizeof(text), "fst %d iip %d fge %d tfx %d tcc %d wrap %d%d ate %d atst %d aref %d abe %d blend %d%d%d%d fix %d hwb %d fba %d ff %d fbmsk %x",
                              s.v[0][1], s.v[0][2], s.v[0][3], s.v[1][0], s.v[1][1], s.v[2][1], s.v[2][2], s.v[7][0], s.v[7][1], s.v[7][2], s.v[9][0],
                              s.v[8][0], s.v[8][1], s.v[8][2], s.v[8][3], s.v[9][2], s.v[10][3], s.v[9][3], s.v[6][3], static_cast<unsigned>(s.v[5][3]));
                return std::string(text);
            };
            std::vector<std::string> names(states.size());
            for (size_t i = 0; i < states.size(); ++i)
                names[i] = describe(states[i]);
            bool uniform = true;
            for (size_t i = 1; i < names.size() && uniform; ++i)
                uniform = names[i] == names[0];
            for (const GpuPrim &pr : prims)
                ++statHwTriKinds[names[static_cast<size_t>(pr.extra[3])]];
            ++statHwTriBatches;
            statHwTriPrims += prims.size();
            if (uniform)
            {
                ++statHwTriUniform;
                statHwTriUniformPrims += prims.size();
            }
        }
        if (curProgram != wantProgram)
        {
            curProgram = wantProgram;
            glUseProgram(wantProgram == 1   ? interlockProgram
                         : wantProgram == 2 ? fastProgram
                         : wantProgram == 3 ? specTex15Program
                         : wantProgram == 4 ? hwTriProgram
                         : wantProgram == 5 ? hwTriNoAtProgram
                         : wantProgram >= kSpecFirst ? specSlotProgram[wantProgram - kSpecFirst]
                                            : program);
        }
        const GLuint depthTex = k.depthTarget >= 0 ? targets[k.depthTarget]->tex : 0;
        if (interlocked)
        {
            // An interlocked batch loads and stores the target as an image and
            // samples the depth target: drawn into a framebuffer with no
            // attachments, so neither is attached meanwhile (a feedback loop,
            // even with the writes masked).
            if (!onInterlockFbo)
            {
                glBindFramebuffer(GL_FRAMEBUFFER, interlockFbo);
                onInterlockFbo = true;
            }
        }
        else
        {
            if (stableTargets)
            {
                // P2: this colour/depth pair's own framebuffer (attached once).
                const GLuint want = drawFboFor(color.tex, depthTex);
                if (onInterlockFbo || curDrawFbo != want)
                {
                    glBindFramebuffer(GL_FRAMEBUFFER, want);
                    curDrawFbo = want;
                    onInterlockFbo = false;
                }
            }
            else
            {
            if (onInterlockFbo)
            {
                glBindFramebuffer(GL_FRAMEBUFFER, fbo);
                onInterlockFbo = false;
            }
            if (fboColor != color.tex)
            {
                noteAttach(color.tex);
                glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, color.tex, 0);
                fboColor = color.tex;
            }
            if (fboDepth != depthTex)
            {
                noteAttach(depthTex);
                glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, depthTex, 0);
                fboDepth = depthTex;
            }
            }
        }
        // Fast mode: this batch blends on the GPU's blend unit (see hwBlend):
        // the shader outputs Cs*ks and, as the second source, kd.
        if (k.hwBlend == 4)
        {
            // Cs * Ad + Cd: the destination's alpha is Cs's factor (drawn twice, see drawRange).
            glEnable(GL_BLEND);
            glBlendEquationSeparate(GL_FUNC_ADD, GL_FUNC_ADD);
            glBlendFuncSeparate(GL_DST_ALPHA, GL_ONE, GL_ONE, GL_ZERO);
        }
        else if (k.hwBlend)
        {
            glEnable(GL_BLEND);
            glBlendEquationSeparate(k.hwBlend == 1 ? GL_FUNC_ADD : (k.hwBlend == 2 ? GL_FUNC_SUBTRACT : GL_FUNC_REVERSE_SUBTRACT), GL_FUNC_ADD);
            glBlendFuncSeparate(GL_ONE, GL_SRC1_COLOR, GL_ONE, GL_ZERO);
        }
        if (curVpW != color.width * color.sx || curVpH != color.height * color.sy)
        {
            glViewport(0, 0, color.width * color.sx, color.height * color.sy);
            curVpW = color.width * color.sx;
            curVpH = color.height * color.sy;
        }
        // An interlocked batch tests depth in its shader (and writes none).
        const bool glDepthTest = k.zte && !interlocked;
        if (curDepthTest != (glDepthTest ? 1 : 0))
        {
            if (glDepthTest)
                glEnable(GL_DEPTH_TEST);
            else
                glDisable(GL_DEPTH_TEST);
            curDepthTest = glDepthTest ? 1 : 0;
        }
        if (glDepthTest && curDepthFunc != k.ztst)
        {
            glDepthFunc(k.ztst == 1 ? GL_ALWAYS : (k.ztst == 2 ? GL_GEQUAL : GL_GREATER));
            curDepthFunc = k.ztst;
        }
        setU(U_targetSize, 2, color.width, color.height);
        setU(U_stateBase, 1, stateBase);
        setU(U_scale, 2, color.sx, color.sy);
        if (interlocked)
        {
            setU(U_zTest, 1, k.zte ? (k.ztst == 1 ? 1 : (k.ztst == 2 ? 2 : 3)) : 0);
            setU(U_colorMaskBits, 1, k.colorMask);
            if (k.zte)
                bindUnit(10, depthTex);
            // The RGBA8 words as they are (formats compatible by size). Bound
            // again for every batch, even the same texture: rendered to in
            // between, the driver prepares it for image access at the bind
            // (with the binding cached, the rally demo's full-screen overlay
            // now and then blended over a stale frame; GT3_GS_GL_IMAGE_CACHE=1
            // keeps the cache, diagnostic).
            static const bool cacheImage = envFlag("GT3_GS_GL_IMAGE_CACHE", false);
            if (!cacheImage || boundImage != color.tex)
            {
                glBindImageTexture(1, color.tex, 0, GL_FALSE, 0, GL_READ_WRITE, GL_R32UI);
                boundImage = color.tex;
            }
        }
        if (!hasBindless)
        {
            // One texture per batch: its unit for colour and for depth reads.
            bindUnit(0, k.texSource);
            bindUnit(2, k.texSource);
            bindUnit(3, k.paletteTex);
        }
        bindUnit(4, lut8);
        bindUnit(5, lut4);
        bindUnit(1, k.dstRead ? (textureBarrier ? color.tex : dstScratch) : 0);
        glActiveTexture(GL_TEXTURE0);
        GS_NOTE_BATCH_TEXTURES(batchTextures.data(), batchTextures.size());

        stateTimer.reset();
        auto drawRange = [&](size_t first, size_t count) {
            ScopedNs drawTimer(statNs[8]);
            if (k.dstRead && textureBarrier)
            {
                // The blend reads the target itself: earlier groups' writes must land first.
                textureBarrier();
            }
            else if (k.dstRead)
            {
                // The destination as it is before this group, for the blend.
                int x0 = INT32_MAX, y0 = INT32_MAX, x1 = -1, y1 = -1;
                for (size_t i = first; i < first + count; ++i)
                {
                    x0 = std::min(x0, prims[i].bbox[0]);
                    y0 = std::min(y0, prims[i].bbox[1]);
                    x1 = std::max(x1, prims[i].bbox[2]);
                    y1 = std::max(y1, prims[i].bbox[3]);
                }
                copyScaled(color.tex, x0, y0, dstScratch, x0, y0, x1 - x0 + 1, y1 - y0 + 1);
            }
            setU(U_firstPrim, 1, primBase + static_cast<int32_t>(first));
            if (k.hwBlend == 4)
            {
                // Twice Cs * Ad: colour only, then colour and alpha (the
                // batch writes no depth: see hwBlend 4).
                setU(U_alphaPass, 1, 0);
                for (int pass = 0; pass < 2; ++pass)
                {
                    const int masks = pass == 0 ? (k.colorMask & 7) : k.colorMask;
                    if (masks != curMasks)
                    {
                        curMasks = masks;
                        glColorMask((masks & 1) != 0, (masks & 2) != 0, (masks & 4) != 0, (masks & 8) != 0);
                        glDepthMask(GL_FALSE);
                    }
                    glDrawArrays(GL_TRIANGLES, 0, static_cast<GLsizei>(count * 6u));
                    ++statDraws;
                }
                return;
            }
            for (int pass = 0; pass < (twoPass ? 2 : 1); ++pass)
            {
                setU(U_alphaPass, 1, pass);
                applyMasks(k, pass);
                glDrawArrays(GL_TRIANGLES, 0, static_cast<GLsizei>(count * 6u));
                ++statDraws;
            }
        };

        if (k.needsSplit && stats)
        {
            // The first primitive's pixel state (the batch key holds only the
            // pipeline state: blending, alpha test and masks are per primitive).
            // Its fields; formatted in reportStats (splitGroupText).
            const GpuState &s = states[static_cast<size_t>(prims[0].extra[3])];
            groupKey = StatKey{s.v[9][0], s.v[8][0], s.v[8][1], s.v[8][2], s.v[8][3], s.v[9][2], s.v[7][0], s.v[7][1], k.afail, s.v[10][1],
                               s.v[5][3], k.frameFormat, k.zte, k.ztst, k.zmask, layoutCode(color), static_cast<int32_t>(color.fbp),
                               prims[0].extra[0] ? 1 : 0};
        }
        GpuQuery query;
        if (gpuProfiling)
        {
            // Kind: target, split or not, the first primitive's type, texture mode and filtering.
            const GpuState &s0 = states[static_cast<size_t>(prims[0].extra[3])];
            char text[200];
            std::snprintf(text, sizeof(text), "%s %03x/%u %s %s tme %d texMode %d linear %d dst %d", layoutName(color), color.fbp, color.fbw,
                          interlocked ? "ilock" : (k.needsSplit ? "split" : "one  "), prims[0].extra[0] ? "sprite" : "tri   ", s0.v[0][0], s0.v[2][3], s0.v[2][0],
                          s0.v[10][0]);
            // GT3_GS_GL_GPU_PROFILE=2: also the texture read (its target page, size, the fast filter) and the blend
            static const bool detail = [] {
                const char *v = std::getenv("GT3_GS_GL_GPU_PROFILE");
                return v && std::strcmp(v, "2") == 0;
            }();
            if (detail && s0.v[0][0])
                std::snprintf(text + std::strlen(text), sizeof(text) - std::strlen(text), " src %03x %dx%d fast %d abe %d", s0.v[12][0], s0.v[1][2],
                              s0.v[1][3], s0.v[13][2], s0.v[9][0]);
            query.key = text;
            query.time = takeQuery();
            glBeginQuery(GL_TIME_ELAPSED, query.time);
            if (pipelineStats)
            {
                query.frags = takeQuery();
                glBeginQuery(GL_FRAGMENT_SHADER_INVOCATIONS_ARB, query.frags);
            }
            GpuCost &c = gpuProfile[query.key];
            ++c.batches;
            c.prims += prims.size();
            if (detail && !pipelineStats)
                for (const auto &pr : prims) // (no fragment counts on Vulkan: the primitives' boxes, in GS pixels)
                    c.frags += static_cast<uint64_t>(std::max(0, pr.bbox[2] - pr.bbox[0] + 1)) * static_cast<uint64_t>(std::max(0, pr.bbox[3] - pr.bbox[1] + 1));
        }
        if (batchTraceLeft > 0)
        {
            --batchTraceLeft;
            const GpuState &t0 = states[static_cast<size_t>(prims[0].extra[3])];
            int x0 = INT32_MAX, y0 = INT32_MAX, x1 = -1, y1 = -1;
            for (const auto &pr : prims)
            {
                x0 = std::min(x0, pr.bbox[0]);
                y0 = std::min(y0, pr.bbox[1]);
                x1 = std::max(x1, pr.bbox[2]);
                y1 = std::max(y1, pr.bbox[3]);
            }
            std::fprintf(stderr, "[trace] %s %03x/%u %s %s n %zu tme %d mode %d src %03x tbp %04x %dx%d lin %d abe %d box %d,%d-%d,%d\n", layoutName(color),
                         color.fbp, color.fbw, k.needsSplit ? "split" : "one", prims[0].extra[0] ? "spr" : "tri", prims.size(), t0.v[0][0], t0.v[2][3],
                         t0.v[12][0], t0.v[11][0] << 5, t0.v[1][2], t0.v[1][3], t0.v[2][0], t0.v[9][0], x0, y0, x1, y1);
        }
        // GT3_GS_GL_SKIP=kind,kind (diagnostics only: what a kind of batch
        // really costs, waits included; pictures break): split (target-reading
        // blends in groups), tri15 (GS_HW_TRI batches), t10 (sprites through
        // 8-bit views of targets), raw (sprites from raw memory), t1 (sprites
        // reading a target), noz (anything with no depth target), fastt1 (fast-filtered target sprites), fill (untextured sprites).
        {
            static const std::string skipList = [] {
                const char *v = std::getenv("GT3_GS_GL_SKIP");
                return std::string(v ? v : "");
            }();
            if (!skipList.empty())
            {
                const GpuState &s0 = states[static_cast<size_t>(prims[0].extra[3])];
                const bool sprite = prims[0].extra[0] != 0;
                auto listed = [&](const char *kind) { return (',' + skipList + ',').find(std::string(",") + kind + ",") != std::string::npos; };
                const bool skip = (listed("split") && k.needsSplit && !interlocked) || (listed("tri15") && hwTri) ||
                                  (listed("t10") && sprite && s0.v[2][3] == 10) || (listed("raw") && sprite && s0.v[2][3] == 13) ||
                                  (listed("t1") && sprite && s0.v[2][3] == 1) || (listed("noz") && k.depthTarget < 0) ||
                                  (listed("fastt1") && sprite && s0.v[2][3] == 1 && (s0.v[13][2] & 1) != 0) ||
                                  (listed("fill") && sprite && s0.v[0][0] == 0 && !k.needsSplit);
                if (skip)
                {
                    if (k.hwBlend)
                        glDisable(GL_BLEND);
                    prims.clear();
                    clearLit();
                    states.clear();
                    lastStateIndex = -1;
                    batchTextures.clear();
                    return;
                }
            }
        }
        const bool conservativeDraw = conservativeRaster && !fastRaster;
        if (conservativeDraw)
            glEnable(GL_CONSERVATIVE_RASTERIZATION_NV);
        if (fastRaster)
            for (int plane = 0; plane < 4; ++plane)
                glEnable(GL_CLIP_DISTANCE0 + plane); // the primitives' boxes (scissor)
        if (interlocked)
        {
            ScopedNs drawTimer(statNs[8]);
            ++statInterlockBatches;
            statInterlockPrims += prims.size();
            if (stats)
            {
                // Its fields; formatted in reportStats (interlockKindText).
                const GpuState &s = states[static_cast<size_t>(prims[0].extra[3])];
                const StatKey kind{s.v[9][0], s.v[8][0], s.v[8][1], s.v[8][2], s.v[8][3], s.v[9][2], s.v[9][1], s.v[7][0], s.v[7][1], s.v[10][1],
                                   s.v[5][3], k.frameFormat, k.zte, k.ztst, k.colorMask, s.v[0][0], s.v[2][3], prims[0].extra[0] ? 1 : 0,
                                   layoutCode(color), static_cast<int32_t>(color.fbp), static_cast<int32_t>(color.fbw)};
                statInterlockKeys[kind] += prims.size();
            }
            // Earlier draws' writes of the target (through the other
            // framebuffer), for the image loads. Needed: without it the rally
            // demo's afterimage layer diverged now and then (a compare burst
            // of ~116,000 pixels, `local/ilknbdemo`).
            if (textureBarrier)
                textureBarrier();
            // GT3_GS_GL_INTERLOCK_FINISH=1: glFinish after each interlocked
            // draw; =2: before it as well (diagnostics: sync or logic?).
            static const int finishMode = [] {
                const char *v = std::getenv("GT3_GS_GL_INTERLOCK_FINISH");
                return v ? std::atoi(v) : 0;
            }();
            if (finishMode >= 2)
                glFinish();
            if (curMasks != 0)
            {
                // The shader stores the pixels itself.
                glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);
                glDepthMask(GL_FALSE);
                curMasks = 0;
            }
            setU(U_firstPrim, 1, primBase);
            setU(U_alphaPass, 1, 0);
            glDrawArrays(GL_TRIANGLES, 0, static_cast<GLsizei>(prims.size() * 6u));
            ++statDraws;
            ++statGroups;
            // The image stores, for whatever reads or draws the target next
            // (all bits: page moves and self-read copies are glCopyImageSubData).
            glMemoryBarrier(GL_ALL_BARRIER_BITS);
            if (finishMode >= 1)
                glFinish();
        }
        else if (k.needsSplit && !wholeTwoPass)
        {
            ++statSplitBatches;
            statSplitPrims += prims.size();
            if (stats)
                statSplitPrimKeys[StatKey{k.zte, k.zmask, twoPass ? 1 : 0, k.dstRead, levelStart.size() > 2u ? 1 : 0}] += prims.size();
            // One draw per level (see orderByLevels); each level reads the target
            // after the earlier levels have landed.
            for (size_t level = 0; level + 1 < levelStart.size(); ++level)
            {
                const size_t first = levelStart[level], count = levelStart[level + 1] - first;
                if (count == 0)
                    continue;
                drawRange(first, count);
                ++statGroups;
                ++statSplitLevels;
                if (stats)
                    ++statSplitGroupKeys[groupKey];
            }
        }
        else
        {
            drawRange(0, prims.size());
            ++statGroups;
        }
        if (conservativeDraw)
            glDisable(GL_CONSERVATIVE_RASTERIZATION_NV);
        if (fastRaster)
            for (int plane = 0; plane < 4; ++plane)
                glDisable(GL_CLIP_DISTANCE0 + plane);
        if (k.hwBlend)
            glDisable(GL_BLEND); // the other passes draw without blending
        if (gpuProfiling)
        {
            glEndQuery(GL_TIME_ELAPSED);
            if (query.frags)
                glEndQuery(GL_FRAGMENT_SHADER_INVOCATIONS_ARB);
            gpuQueries.push_back(std::move(query));
            pollGpuQueries();
        }

        selfSnap.valid = false; // the target has moved on from the copy
        statGpuPrims += prims.size();
        prims.clear();
        clearLit();
        states.clear();
        lastStateIndex = -1;
        batchTextures.clear();
    }

    // ------------------------------------------- draw-state runs (step 7 T1)
    // analysis/design/ny-throughput/STEP7-THROUGHPUT.md T1. A primitive whose
    // draw state equals the previous call's (SubmitSameState, from
    // GSAsyncBackend's SubmitSame records) reuses the DrawKey, GL key and GPU
    // state that call resolved (target ids, texture and palette names and
    // handles: the memo) instead of resolving them again; only its own part is
    // done (setup and box, the target height check, its pages, the primitive).
    // The memo holds while nothing it was resolved from changed:
    //  - resolveEpoch (touchResolve): targets made, grown or dropped, textures
    //    deleted or recycled, a direct GPU CLUT copied or left, scale, reset;
    //  - frameCounter (cache ages), clutVersion (CLUT loads and read backs),
    //    cpuVersionEpoch (downloads, CPU draws: rawPagesTexture's own key);
    //  - its texture's pages (the decode area, and a raw texture's page range):
    //    an owner or CPU version change of one of them ends it (pageChanged).
    //    They are taken at the memo's first reuse (no cost for runs of one), if
    //    no page changed anywhere since it was made; a decode area with a GPU
    //    held page ends it.
    // Never memoised: CPU fallbacks, points and lines, per-pixel mipmapping,
    // textures read from a GPU target (self-reads included), GPU decode,
    // texture dump/replace. Fast mode (3x and above) is memoised since
    // 2026-10-14 (analysis/frame-times): its blend-unit choice also reads
    // whether a vertex alpha is above 0x80, so the memo keeps that bit too.
    // PS2X_GS_GL_STATE_RUNS=0: off (the GT3 default; GSAsyncBackend reads it too).
    // The Vulkan renderer (this source compiled again, VKGL_REDIRECT) has them
    // on as well since 2026-10-14 (checked there with the self-check below).
    // PS2X_GS_GL_RUN_VERIFY=1 (self-check): every primitive the memo would serve
    // (memoState 0 and its targets fit) is resolved in full instead and
    // compared with the memo: DrawKey, GL key, GPU state and draw state equal,
    // and the full resolution did nothing the memo path skips (no texture
    // decode, upload, made or deleted texture, batch flush, download, upload,
    // page move or CLUT read back; resolveEpoch, page changes, CPU version
    // epoch and CLUT version unchanged; no CPU fallback). The output is the
    // full path's. On a match the memo is kept as it is (as the memo path
    // keeps it), so long-lived memos and their per-page invalidation are what
    // is checked; on a mismatch it is made again from this resolution. Each
    // page-run skip does the full acquire, checks that it changed nothing but
    // one writeSerial and pageEpoch bump per page written (owners, CPU-valid
    // words, downloads, uploads, page moves), and then puts those serials back,
    // so the readers see the serials of the skip. Mismatches counted, the
    // first 16 logged, a "[gs-gl] run verify" line every 300 frames.
    //
    // Page runs (PS2X_GS_GL_ACQUIRE_RUNS=0: off; the GT3 default): a draw's
    // acquirePages is skipped when every page of its box is in its target's
    // run: pages a draw into that target acquired for writing, which it owns,
    // with no CPU-valid words, and with no owner, CPU version or CPU-valid
    // change (pageChanged) and no reader recording their write serial or page
    // epoch (pageObserved) since. Such an acquire would only bump writeSerial
    // and pageEpoch again; every reader compares them with a value it recorded,
    // and the first write after a record is a full acquire, so the readers see
    // the same changes.
    bool stateRuns = [] {
        const char *v = std::getenv("PS2X_GS_GL_STATE_RUNS");
#if defined(PS2X_GAME_GT3) // (GT4's Vulkan build too since 2026-10-14: frame-times, verified there)
        return v && *v && *v != '0';
#else
        return !(v && *v == '0');
#endif
    }();
    bool acquireRuns = [] {
        const char *v = std::getenv("PS2X_GS_GL_ACQUIRE_RUNS");
#if defined(PS2X_GAME_GT3) // (GT4's Vulkan build too since 2026-10-14: frame-times, verified there)
        return v && *v && *v != '0';
#else
        return !(v && *v == '0');
#endif
    }();
    bool runVerify = envFlag("PS2X_GS_GL_RUN_VERIFY", false);
    uint64_t resolveEpoch = 1;
    uint64_t pageChanges = 0;
    struct RunMemo
    {
        bool valid = false;
        bool pagesKnown = false;
        uint8_t vertexCount = 0;
        uint8_t alphaAbove = 0; // fast mode: whether a vertex alpha is above 0x80 (the blend-unit choice reads it)
        uint64_t epoch = 0, frame = 0, cpuEpoch = 0, pageChanges = 0;
        uint32_t clutVersion = 0;
        int decodeW = 0, decodeH = 0;
        GSTex0Reg tex{};
        DrawKey k{}, glKey{};
        GpuState st{};
        GSDrawState state{}; // (PS2X_GS_GL_RUN_VERIFY only)
        std::bitset<kPageCount> pages;
    } memo;
    // Per page: the target whose page run holds it (-1: none).
    std::array<int16_t, kPageCount> acquireRunOf = [] {
        std::array<int16_t, kPageCount> a{};
        a.fill(static_cast<int16_t>(-1));
        return a;
    }();
    uint64_t statSameCalls = 0, statMemoHits = 0, statMemoMade = 0, statMemoTargetMiss = 0;
    uint64_t statMemoWhy[8] = {}; // why a same-state call was resolved again (memoState)
    uint64_t statMemoChecks = 0, statMemoMismatch = 0, statAcquireSkips = 0, statAcquireFull = 0, statAcquireChecks = 0, statAcquireMismatch = 0;
    uint64_t statMemoKept = 0; // (PS2X_GS_GL_RUN_VERIFY) checks that matched: the memo kept as it is

    // PS2X_GS_GL_RUN_VERIFY: what a full resolution can do that the memo path
    // never does. A primitive the memo would serve must change none of it.
    static constexpr size_t kResolveWork = 13;
    static const char *resolveWorkName(size_t i)
    {
        static const char *names[kResolveWork] = {"texture decodes", "raw uploads", "textures made", "textures deleted", "batch flushes",
                                                  "downloads", "uploads", "page moves", "CLUT read backs", "resolveEpoch",
                                                  "page changes", "CPU version epoch", "CLUT version"};
        return i < kResolveWork ? names[i] : "?";
    }
    std::array<uint64_t, kResolveWork> resolveWork() const
    {
        return {statTexDecodes, statRawUploads, statTexMade, statTexDeleted, statFlushCount, statDownloads, statUploads, statGpuMoves,
                statClutMaterialize, resolveEpoch, pageChanges, cpuVersionEpoch, clutVersion};
    }
    bool memoCheckPending = false; // a checked primitive between beginMemoCheck and afterResolve
    std::array<uint64_t, kResolveWork> memoCheckWork{};
    // Per page: times the box being checked visits it (acquireDrawPages; 0 between checks).
    std::array<uint16_t, kPageCount> verifyVisits{};
    struct VerifyPage
    {
        uint32_t page, write, epoch;
    };
    std::vector<VerifyPage> verifyPages;

    void touchResolve() { ++resolveEpoch; }
    void resetRuns()
    {
        memo.valid = false;
        ++resolveEpoch;
        acquireRunOf.fill(static_cast<int16_t>(-1));
    }
    // An owner, CPU version or CPU-valid change of a page.
    void pageChanged(uint32_t page)
    {
        page &= kPageCount - 1u;
        ++pageChanges;
        if (memo.valid && memo.pagesKnown && memo.pages.test(page))
            memo.valid = false;
        acquireRunOf[page] = -1;
    }
    // A reader recorded the page's write serial or page epoch.
    void pageObserved(uint32_t page) { acquireRunOf[page & (kPageCount - 1u)] = -1; }

    // 0: the memo serves this same-state primitive; else why not (statistics:
    // 1 none or a page of it changed, 2 resolveEpoch, 3 frame, 4 CLUT version,
    // 5 CPU version epoch, 6 vertex count, 7 off).
    int memoState(const GSPrimitiveBatch &batch)
    {
        if (!stateRuns)
            return 7;
        if (!memo.valid)
            return 1;
        if (memo.epoch != resolveEpoch)
            return 2;
        if (memo.frame != frameCounter)
            return 3;
        if (memo.clutVersion != clutVersion)
            return 4;
        if (memo.cpuEpoch != cpuVersionEpoch)
            return 5;
        if (batch.vertexCount != memo.vertexCount)
            return 6;
        // Fast mode: the blend unit's choice reads the vertices' alpha (untextured batches), the one input
        // of the resolution besides the draw state (2026-10-14, frame-times).
        if (fastBlend && vertexAlphaAbove(batch) != memo.alphaAbove)
            return 6;
        if (!memo.pagesKnown)
        {
            // Its texture's pages, as when it was made (no page changed since).
            if (memo.pageChanges != pageChanges)
            {
                memo.valid = false;
                return 1;
            }
            memo.pages.reset();
            if (memo.k.tme)
            {
                bool gpuHeld = false;
                forRectPages(memo.tex.tbp0 >> 5u, (memo.tex.tbp0 & 31u) != 0u, memo.tex.tbw, memo.tex.psm, 0, 0, memo.decodeW - 1,
                             memo.decodeH - 1, [&](uint32_t page) {
                                 gpuHeld = gpuHeld || owner[page] >= 0;
                                 memo.pages.set(page);
                             });
                if (gpuHeld)
                {
                    memo.valid = false;
                    return 1;
                }
                if (memo.k.texMode == 13) // a raw texture uploads its whole page range
                    for (int32_t i = 0; i < memo.k.texTargetFbw; ++i)
                        memo.pages.set(static_cast<uint32_t>(memo.k.texTargetFbp + i) & (kPageCount - 1u));
            }
            memo.pagesKnown = true;
        }
        return 0;
    }
    static uint8_t vertexAlphaAbove(const GSPrimitiveBatch &batch)
    {
        bool above = false;
        for (uint32_t i = 0; i < batch.vertexCount && i < 3u; ++i)
            above = above || batch.vertices[i].a > 0x80u;
        return above ? 1u : 0u;
    }
    void noteSameState(int why)
    {
        ++statSameCalls;
        ++statMemoWhy[why & 7];
    }
    // The targets of the memo hold the primitive's rows without growing (as
    // findTarget would return them, with no side effect but lastUse).
    bool memoTargetsFit(int bx1, int by1) const
    {
        const int rows = by1 + 1;
        const Target *c = targets[static_cast<size_t>(memo.k.colorTarget)].get();
        if (!c || rows > c->height || rows > c->logicalHeight || bx1 >= c->width)
            return false;
        if (memo.k.depthTarget >= 0)
        {
            const Target *d = targets[static_cast<size_t>(memo.k.depthTarget)].get();
            if (!d || rows > d->height || rows > d->logicalHeight)
                return false;
        }
        return true;
    }
    // PS2X_GS_GL_RUN_VERIFY: a primitive the memo would serve is about to be
    // resolved in full (submitPrimitive); afterResolve compares.
    void beginMemoCheck()
    {
        memoCheckPending = true;
        memoCheckWork = resolveWork();
    }
    // ... and it was drawn on the CPU instead (left before afterResolve).
    void memoCheckLost()
    {
        memoCheckPending = false;
        ++statMemoChecks;
        ++statMemoMismatch;
        memo.valid = false;
        if (statMemoMismatch <= 16u)
            std::fprintf(stderr, "[gs-gl] RUN VERIFY mismatch %llu (frame %llu): the full path drew it on the CPU (the memo would have drawn it on the GPU)\n",
                         (unsigned long long)statMemoMismatch, (unsigned long long)frameCounter);
    }
    // After a full resolution: the memo for the next primitive of this state
    // (memoisable: see above), or with PS2X_GS_GL_RUN_VERIFY the comparison
    // with the memo that would have served this one (check; on a match the
    // memo is kept as it is).
    void afterResolve(const GSPrimitiveBatch &batch, const DrawKey &k, const DrawKey &glKey, const GpuState &st, bool check, bool memoisable,
                      int decodeW, int decodeH)
    {
        if (check)
        {
            memoCheckPending = false;
            ++statMemoChecks;
            const bool sameK = std::memcmp(&k, &memo.k, sizeof(DrawKey)) == 0;
            const bool sameGl = std::memcmp(&glKey, &memo.glKey, sizeof(DrawKey)) == 0;
            const bool sameSt = std::memcmp(&st, &memo.st, sizeof(GpuState)) == 0;
            const bool sameState = gsSameDrawState(batch.state, memo.state);
            const std::array<uint64_t, kResolveWork> work = resolveWork();
            size_t workChanged = kResolveWork;
            for (size_t i = 0; i < kResolveWork && workChanged == kResolveWork; ++i)
                if (work[i] != memoCheckWork[i])
                    workChanged = i;
            // (memo.valid false here: a page of its texture changed during the resolution.)
            const bool still = memo.valid && memoisable;
            if (sameK && sameGl && sameSt && sameState && workChanged == kResolveWork && still)
            {
                ++statMemoKept;
                return; // the memo stays as it is
            }
            ++statMemoMismatch;
            if (statMemoMismatch <= 16u)
            {
                const int32_t *a = reinterpret_cast<const int32_t *>(&k), *b = reinterpret_cast<const int32_t *>(&memo.k);
                size_t word = 0;
                while (word < sizeof(DrawKey) / 4u && a[word] == b[word])
                    ++word;
                std::fprintf(stderr,
                             "[gs-gl] RUN VERIFY mismatch %llu (frame %llu): key %s (word %zu: %d vs memo %d), GL key %s, GPU state %s, draw state %s; "
                             "full resolution did %s%s (+%llu); memo %s, memoisable %d; texMode %d tex %u palette %u\n",
                             (unsigned long long)statMemoMismatch, (unsigned long long)frameCounter, sameK ? "same" : "DIFFERS", word,
                             word < sizeof(DrawKey) / 4u ? a[word] : 0, word < sizeof(DrawKey) / 4u ? b[word] : 0, sameGl ? "same" : "DIFFERS",
                             sameSt ? "same" : "DIFFERS", sameState ? "same" : "DIFFERS", workChanged == kResolveWork ? "nothing" : "",
                             workChanged == kResolveWork ? "" : resolveWorkName(workChanged),
                             workChanged == kResolveWork ? 0ull : (unsigned long long)(work[workChanged] - memoCheckWork[workChanged]),
                             memo.valid ? "valid" : "ENDED (a page of its texture changed)", memoisable ? 1 : 0, k.texMode, k.texSource, k.paletteTex);
            }
            memo.valid = false; // made again below from this resolution
        }
        if (!stateRuns || !memoisable)
            return; // (memo.valid is already false)
        memo.valid = true;
        memo.pagesKnown = false;
        memo.vertexCount = batch.vertexCount;
        memo.alphaAbove = fastBlend ? vertexAlphaAbove(batch) : 0u;
        memo.epoch = resolveEpoch;
        memo.frame = frameCounter;
        memo.cpuEpoch = cpuVersionEpoch;
        memo.clutVersion = clutVersion;
        memo.pageChanges = pageChanges;
        memo.decodeW = decodeW;
        memo.decodeH = decodeH;
        memo.tex = batch.state.context.tex0;
        memo.k = k;
        memo.glKey = glKey;
        memo.st = st;
        if (runVerify)
            memo.state = batch.state;
        ++statMemoMade;
    }

    // ---- a primitive's draw once its draw state is resolved (shared by the
    // full path and the memo; a self-read adds its copies in between).
    // The batch it joins: a GL key unlike the pending batch's flushes it.
    void breakBatchFor(const DrawKey &glKey)
    {
        if (haveBatch && !(key == glKey))
        {
            if (stats)
            {
                // Which DrawKey word changed first (named in reportStats), and
                // between which kinds of batch when dstRead changed (step 7 T0).
                const int32_t *a = reinterpret_cast<const int32_t *>(&key), *b = reinterpret_cast<const int32_t *>(&glKey);
                size_t i = 0;
                while (i < sizeof(DrawKey) / 4u && a[i] == b[i])
                    ++i;
                ++statBreakWord[i];
                if (key.dstRead != glKey.dstRead)
                    ++statDstPairs[batchKind(key)][batchKind(glKey)];
            }
            flushBatch();
        }
    }
    // The target copies of the pages this primitive touches must be current.
    void acquireForDraw(const DrawKey &k, const GSContext &ctx, bool sprite, int bx0, int by0, int bx1, int by1)
    {
        const bool writesColor = k.colorMask != 0 || k.afail == 1 || k.afail == 3;
        // A sprite that writes every pixel of its rectangle regardless of what was
        // there (no blending, destination or alpha test, full colour mask, Z test
        // ALWAYS or off) replaces the pages it covers completely.
        const bool alphaTestKeeps = k.ate && k.atst != 1;
        // (Fast mode's blend-unit blending clears dstRead, so a blended sprite
        // counts as replacing its pages. GT3_GS_GL_HW_BLEND_OVERWRITES=0
        // (diagnostic): not counted. Tried for GT3's blocky rear-view mirror at
        // 4x: no difference over 6-frame samples (the mirror is at native detail
        // in every mode, exact included), main view unchanged. 2026-09-29.)
        static const bool hwBlendOverwrites = envFlag("GT3_GS_GL_HW_BLEND_OVERWRITES", true);
        const bool overwrites = sprite && !k.dstRead && (k.hwBlend == 0 || hwBlendOverwrites) && !alphaTestKeeps && (!k.zte || k.ztst == 1);
        // The frame's memory format (k.frameFormat before GT4's race colour may have made a C16 frame a CT32 one).
        const uint32_t fpsm = ctx.frame.psm;
        const int memoryFormat = fpsm == GS_PSM_CT24 ? 1 : (fpsm == GS_PSM_CT16 ? 2 : (fpsm == GS_PSM_Z24 ? 3 : (fpsm == GS_PSM_Z32 ? 4 : 0)));
        const bool colorWhole = overwrites && k.colorMask == (memoryFormat == 1 || memoryFormat == 3 ? 0x7 : 0xF) && memoryFormat != 1 &&
                                memoryFormat != 3;
        if (stats || debugPage >= 0)
            cause = "draw target";
        if (debugPage >= 0)
        {
            char text[200];
            std::snprintf(text, sizeof(text), "draw colour fbp %03x/%u psm %02x zbp %03x psm %02x zte %d ztst %d zmask %d box %d,%d-%d,%d sprite %d",
                          ctx.frame.fbp, ctx.frame.fbw, ctx.frame.psm, ctx.zbuf.zbp, ctx.zbuf.psm, k.zte, k.ztst, k.zmask, bx0, by0, bx1, by1,
                          sprite ? 1 : 0);
            cause = text;
        }
        if (colorWhole)
            acquireDrawPages(k.colorTarget, bx0, by0, bx1, by1, writesColor, bx0, by0, bx1, by1);
        else
            acquireDrawPages(k.colorTarget, bx0, by0, bx1, by1, writesColor);
        if (debugPage >= 0 && k.depthTarget >= 0)
        {
            const std::string c = cause;
            cause = "depth of " + c.substr(std::min<size_t>(c.size(), 5));
        }
        if (k.depthTarget >= 0)
        {
            if (overwrites && !k.zmask)
                acquireDrawPages(k.depthTarget, bx0, by0, bx1, by1, true, bx0, by0, bx1, by1);
            else
                acquireDrawPages(k.depthTarget, bx0, by0, bx1, by1, !k.zmask);
        }
    }
    // acquirePages for a draw, skipped when its pages are in the target's page run.
    void acquireDrawPages(int id, int x0, int y0, int x1, int y1, bool write, int coverX0 = 1, int coverY0 = 1, int coverX1 = 0, int coverY1 = 0)
    {
        const Target &t = *targets[static_cast<size_t>(id)];
        const uint32_t psm = layoutPsm(t.layout);
        bool check = false, ok = true;
        uint64_t downloads = 0, uploads = 0, moves = 0, moveDraws = 0;
        if (acquireRuns)
        {
            bool covered = true;
            forRectPages(t.fbp, false, t.fbw, psm, x0, y0, x1, y1, [&](uint32_t page) { covered = covered && acquireRunOf[page] == id; });
            if (covered && !runVerify)
            {
                ++statAcquireSkips;
                return;
            }
            if (covered)
            {
                // Self-check: the full acquire changes nothing but one
                // writeSerial and pageEpoch bump per page written; those are
                // put back below, as the skip leaves them.
                check = true;
                downloads = statDownloads;
                uploads = statUploads;
                moves = statGpuMoves;
                moveDraws = statMoveDraws;
                verifyPages.clear();
                forRectPages(t.fbp, false, t.fbw, psm, x0, y0, x1, y1, [&](uint32_t page) {
                    ok = ok && owner[page] == id && !cpuValidAny[page];
                    if (verifyVisits[page]++ == 0u)
                        verifyPages.push_back({page, writeSerial[page], pageEpoch[page]});
                });
            }
        }
        acquirePages(id, x0, y0, x1, y1, write, coverX0, coverY0, coverX1, coverY1);
        if (check)
        {
            for (const VerifyPage &v : verifyPages)
            {
                const uint32_t bumps = write ? verifyVisits[v.page] : 0u;
                ok = ok && owner[v.page] == id && !cpuValidAny[v.page] && writeSerial[v.page] == v.write + bumps && pageEpoch[v.page] == v.epoch + bumps;
                verifyVisits[v.page] = 0;
            }
            ok = ok && downloads == statDownloads && uploads == statUploads && moves == statGpuMoves && moveDraws == statMoveDraws;
            ++statAcquireChecks;
            if (ok)
            {
                // As the skip: the serials as they were (the readers see the skip's).
                for (const VerifyPage &v : verifyPages)
                {
                    writeSerial[v.page] = v.write;
                    pageEpoch[v.page] = v.epoch;
                }
                ++statAcquireSkips;
                return; // (the run's pages stay in it, as after a skip)
            }
            if (++statAcquireMismatch <= 16u)
                std::fprintf(stderr, "[gs-gl] RUN VERIFY page run mismatch %llu (frame %llu): target %d %s %03x/%u box %d,%d-%d,%d write %d\n",
                             (unsigned long long)statAcquireMismatch, (unsigned long long)frameCounter, id, layoutName(t), t.fbp, t.fbw, x0, y0, x1, y1,
                             write ? 1 : 0);
        }
        ++statAcquireFull;
        if (acquireRuns && write)
            forRectPages(t.fbp, false, t.fbw, psm, x0, y0, x1, y1, [&](uint32_t page) {
                acquireRunOf[page] = (owner[page] == id && !cpuValidAny[page]) ? static_cast<int16_t>(id) : static_cast<int16_t>(-1);
            });
    }
    // The primitive into the pending batch (with its GPU state and textures).
    void pushPrim(GpuPrim &p, const DrawKey &k, const DrawKey &glKey, const GpuState &st)
    {
        if (!haveBatch)
        {
            key = glKey;
            haveBatch = true;
            batchZ16Above = false;
        }
        if (lastStateIndex < 0 || !sameBytes(st, lastState))
        {
            states.push_back(st);
            lastState = st;
            lastStateIndex = static_cast<int32_t>(states.size() - 1u);
        }
        p.extra[3] = lastStateIndex;
        for (const GLuint used : {static_cast<GLuint>(k.texSource), static_cast<GLuint>(k.paletteTex)})
            if (const size_t n = batchTextures.size(); used && !(n >= 1 && batchTextures[n - 1] == used) && !(n >= 2 && batchTextures[n - 2] == used) && // (mostly the last two)
                std::find(batchTextures.begin(), batchTextures.end(), used) == batchTextures.end())
                batchTextures.push_back(used);
        if (k.zte == 2 && !batchZ16Above)
            batchZ16Above = p.extra[0] ? p.zv[3] > 65535.0 : (p.zv[0] > 65535.0 || p.zv[1] > 65535.0 || p.zv[2] > 65535.0);
        prims.push_back(p);
        if (selfSnap.valid && k.colorTarget == selfSnap.target)
            selfSnap.mark(p.bbox[0], p.bbox[1], p.bbox[2], p.bbox[3], selfSnap.tag(texFlushSerial));
    }
    // A same-state primitive served from the memo.
    void drawFromMemo(GpuPrim &prim, const GSContext &ctx, bool sprite, int bx0, int by0, int bx1, int by1)
    {
        ++statMemoHits;
        if (stats)
        {
            ++statRunKeyed;
            if (statPrevKeyValid && statPrevKey == memo.k)
                ++statRunSameKey;
            statPrevKey = memo.k;
            statPrevKeyValid = true;
            if (memo.k.paletteMode == 1 || memo.k.paletteTex)
                noteClutRead(memo.tex);
        }
        breakBatchFor(memo.glKey);
        acquireForDraw(memo.k, ctx, sprite, bx0, by0, bx1, by1);
        pushPrim(prim, memo.k, memo.glKey, memo.st);
        reportStats();
    }
    void reportRunVerify()
    {
        if (runVerify && (frameCounter % 300u) == 0u)
            std::fprintf(stderr,
                         "[gs-gl] run verify: draw states checked %llu (memo kept %llu), mismatches %llu; page runs checked %llu (serials put back as "
                         "skipped), mismatches %llu\n",
                         (unsigned long long)statMemoChecks, (unsigned long long)statMemoKept, (unsigned long long)statMemoMismatch,
                         (unsigned long long)statAcquireChecks, (unsigned long long)statAcquireMismatch);
    }

    // DrawKey word names by position (batch-break statistics).
    static const char *drawKeyWordName(size_t i)
    {
        static const char *names[] = {"colorTarget", "depthTarget", "texSource", "paletteTex", "paletteMode", "clutCsa", "clutFour",
                                      "clutCt24", "texBasePage", "texPagesPerRow", "texPageW", "texPageH", "texTargetFbp",
                                      "texTargetFbw", "texTargetPageH"};
        return i < sizeof(names) / sizeof(names[0]) ? names[i] : nullptr;
    }
    static std::string interlockKindText(const StatKey &q)
    {
        char kind[200];
        std::snprintf(kind, sizeof(kind), "abe %d alpha %d%d%d%d fix %02x pabe %d ate %d atst %d date %d fbmsk %08x ff %d z %d%d cm %x tme %d tm %d %s -> %s %03x/%u",
                      q[0], q[1], q[2], q[3], q[4], q[5], q[6], q[7], q[8], q[9], static_cast<uint32_t>(q[10]), q[11], q[12], q[13], q[14], q[15],
                      q[16], q[17] ? "sprite" : "tri", layoutCodeName(q[18]), static_cast<uint32_t>(q[19]), static_cast<uint32_t>(q[20]));
        return kind;
    }
    static std::string splitGroupText(const StatKey &q)
    {
        char text[160];
        std::snprintf(text, sizeof(text), "abe %d alpha %d%d%d%d fix %02x ate %d atst %d afail %d date %d fbmsk %08x ff %d z %d%d%d -> %s %03x prim %s",
                      q[0], q[1], q[2], q[3], q[4], q[5], q[6], q[7], q[8], q[9], static_cast<uint32_t>(q[10]), q[11], q[12], q[13], q[14],
                      layoutCodeName(q[15]), static_cast<uint32_t>(q[16]), q[17] ? "sprite" : "tri");
        return text;
    }
    static std::string splitPrimText(const StatKey &q)
    {
        char kind[96];
        std::snprintf(kind, sizeof(kind), "zte %d zmask %d twoPass %d dstRead %d levels %s", q[0], q[1], q[2], q[3], q[4] ? "2+" : "1");
        return kind;
    }

    // everyCall: look at the time now (presentation); else, with statistics
    // sampling (PS2X_GS_GL_STATS_SAMPLE above 1), once per 1,024 calls.
    void reportStats(bool everyCall = false)
    {
        if (!stats)
            return;
        if (!everyCall && statSample > 1u && (++statReportCalls & 1023u) != 0u)
            return;
        // (Called per Submit: with the TSC clock the cheap clock first, the
        // window's time every 0.1 s; with steady_clock every call, as before.)
        if (StatClock::tsc())
        {
            const uint64_t tick = StatClock::now();
            if (tick < statNextCheck)
                return;
            statNextCheck = tick + StatClock::fromNs(100000000u);
        }
        const auto now = std::chrono::steady_clock::now();
        const double seconds = std::chrono::duration<double>(now - statStart).count();
        if (seconds < 10.0)
            return;
        if (statZToC16)
        {
            std::fprintf(stderr, "[gs-gl]   Z -> CT16 page moves on the GPU (PS2X_GS_GL_Z_TO_C16): %llu\n", (unsigned long long)statZToC16);
            statZToC16 = 0;
        }
        if (statZ16Prims || statZ16Moves)
        {
            std::fprintf(stderr, "[gs-gl]   Z16 buffers on the GPU (PS2X_GS_GL_Z16): primitives %llu, of them with a vertex Z above 0xFFFF (clamped) %llu; Z16 page moves on the GPU %llu\n",
                         (unsigned long long)statZ16Prims, (unsigned long long)statZ16ZAbove, (unsigned long long)statZ16Moves);
            statZ16Prims = statZ16ZAbove = statZ16Moves = 0;
        }
        if (statFullTexReads || statFullTexMiss)
        {
            std::fprintf(stderr, "[gs-gl]   full-colour CT16 texture reads (GT4_RACE_COLOUR_TEXTURES): %llu primitives from 16-bit targets, %llu without a target (16-bit)\n",
                         (unsigned long long)statFullTexReads, (unsigned long long)statFullTexMiss);
            statFullTexReads = statFullTexMiss = 0;
        }
        std::fprintf(stderr,
                     "[gs-gl] %.1f s: gpu prims %llu, cpu prims %llu, gl draws %llu, groups %llu, page downloads %llu, uploads %llu, gpu page moves %llu, upload patches %llu (pages read without copy back %llu), texture decodes %llu, raw texture uploads %llu, parallel mask batches %llu, page move draws %llu, targets %zu, GPU texture decodes %llu\n",
                     seconds, (unsigned long long)statGpuPrims, (unsigned long long)statCpuPrims, (unsigned long long)statDraws,
                     (unsigned long long)statGroups, (unsigned long long)statDownloads, (unsigned long long)statUploads,
                     (unsigned long long)statGpuMoves, (unsigned long long)statPatches, (unsigned long long)statValidReads, (unsigned long long)statTexDecodes, (unsigned long long)statRawUploads, (unsigned long long)statParallelBatches, (unsigned long long)statMoveDraws, targets.size(), (unsigned long long)statGpuDecodes);
        statGpuDecodes = 0;
        if (colourProgram)
            std::fprintf(stderr, "[gs-gl]   fast colour textures: %llu decoded, %llu reused, %zu cached\n", (unsigned long long)statColourDecodes,
                         (unsigned long long)statColourHits, colourTextures.size());
        statColourDecodes = statColourHits = 0;
        std::fprintf(stderr, "[gs-gl]   raw texture lookups: last answer reused %llu, missed (pages changed) %llu, missed (other texture) %llu\n",
                     (unsigned long long)statRawHits, (unsigned long long)statRawMissEpoch, (unsigned long long)statRawMissKey);
        statRawHits = statRawMissEpoch = statRawMissKey = 0;
        if (statGpuRawFills)
            std::fprintf(stderr, "[gs-gl]   textures in depth targets made on the GPU (no download): %llu\n", (unsigned long long)statGpuRawFills);
        statGpuRawFills = 0;
        std::fprintf(stderr, "[gs-gl]   self-reading draws: %llu target copies, %llu primitives reused their batch's copy\n",
                     (unsigned long long)statSelfCopies, (unsigned long long)statSelfReuses);
        std::fprintf(stderr, "[gs-gl]   self-read copy causes: first/other view %llu, no batch %llu, no copy %llu, other target %llu, TEXFLUSH %llu, rows %llu, touched %llu\n",
                     (unsigned long long)statSelfWhy[0], (unsigned long long)statSelfWhy[1], (unsigned long long)statSelfWhy[2],
                     (unsigned long long)statSelfWhy[3], (unsigned long long)statSelfWhy[4], (unsigned long long)statSelfWhy[5],
                     (unsigned long long)statSelfWhy[6]);
        std::fprintf(stderr, "[gs-gl]   self-reads drawn in bands (reading rows they write): %llu; copies of only the cells read: %llu\n",
                     (unsigned long long)statSelfBanded, (unsigned long long)statSelfPartial);
#if !defined(PS2X_GAME_GT3)
        std::fprintf(stderr, "[gs-gl]   self-read bands drawn from an earlier band's copy (no new copy needed): %llu\n",
                     (unsigned long long)statSelfBandsShared);
#endif
        statSelfCopies = statSelfReuses = statSelfBanded = statSelfPartial = statSelfBandsShared = 0;
        std::fprintf(stderr, "[gs-gl]   GL objects: target/scratch textures made %llu, textures deleted %llu; targets made %llu, grown %llu; draw/move framebuffer attachments %llu, first attachments of a texture %llu\n",
                     (unsigned long long)statTexMade, (unsigned long long)statTexDeleted, (unsigned long long)statTargetsMade,
                     (unsigned long long)statTargetGrows, (unsigned long long)statAttaches, (unsigned long long)statFirstAttaches);
        statTexMade = statTexDeleted = statTargetsMade = statTargetGrows = statAttaches = statFirstAttaches = 0;
        if (tallOn)
        {
            std::string tall;
            for (const auto &tp : targets)
                if (tp && tp->tall)
                {
                    char one[32];
                    std::snprintf(one, sizeof(one), " %s%03x/%u", tp->depth ? "Z " : "", tp->fbp, tp->fbw);
                    tall += one;
                }
            std::fprintf(stderr, "[gs-gl]   GT4's car view at full height: armed %d; tall targets:%s\n", tallArmedSeen ? 1 : 0,
                         tall.empty() ? " none" : tall.c_str());
        }
        if (effectScale)
        {
            std::string small;
            for (const auto &tp : targets)
                if (tp && tp->sx != scaleX)
                {
                    char one[24];
                    std::snprintf(one, sizeof(one), " %s%03x/%u", tp->layout == Layout::C16 ? "C16 " : "", tp->fbp, tp->fbw);
                    small += one;
                }
            std::fprintf(stderr, "[gs-gl]   effects scale: %llu promotions so far; targets at %dx:%s\n", (unsigned long long)statPromotions,
                         effectScale, small.c_str());
        }
        if (stableTargets)
            std::fprintf(stderr, "[gs-gl]   stable targets: pooled target textures reused %llu, new %llu, pool %zu (%.0f MB); framebuffers made %llu, held %zu draw + %zu single\n",
                         (unsigned long long)statPoolHits, (unsigned long long)statPoolMisses, targetPool.size(), static_cast<double>(targetPoolBytes) / 1048576.0,
                         (unsigned long long)statFbosMade, drawFbos.size(), soloFbos.size());
        statPoolHits = statPoolMisses = statFbosMade = 0;
        if (stableTargets && prewarm > 0)
            std::fprintf(stderr, "[gs-gl]   prewarm: spares made %llu, used %llu, dropped %llu; unused %.2f MB, sizes waiting %zu\n",
                         (unsigned long long)statSparesMade, (unsigned long long)statSparesUsed, (unsigned long long)statSparesDropped,
                         static_cast<double>(spareBytes) / 1048576.0, prewarmWanted.size());
        statSparesMade = statSparesUsed = statSparesDropped = 0;
        if (partialPatches)
            std::fprintf(stderr, "[gs-gl]   PSMT8/PSMT4 uploads merged into GPU pages (partial-word patches): pages %llu\n",
                         (unsigned long long)statPartialPatches);
        statPartialPatches = 0;
        if (log2Lut)
            std::fprintf(stderr, "[gs-gl]   mipmap: per-pixel primitives on the GPU %llu, raw textures over their levels uploaded %llu\n",
                         (unsigned long long)statMipPrims, (unsigned long long)statMipSpanUploads);
        statMipPrims = statMipSpanUploads = 0;
        if (texSmooth)
            std::fprintf(stderr, "[gs-gl]   texture smoothing: 3D triangles made bilinear %llu (a target's texture, kept nearest %llu), already bilinear %llu\n",
                         (unsigned long long)statSmoothPrims, (unsigned long long)statSmoothTarget, (unsigned long long)statSmoothAlready);
        statSmoothPrims = statSmoothTarget = statSmoothAlready = 0;
        std::fill(std::begin(statSelfWhy), std::end(statSelfWhy), 0ull);
        statMoveDraws = 0;
        statRawUploads = 0;
        statParallelBatches = 0;
        statGpuMoves = 0;
        statPatches = 0;
        statValidReads = 0;
        std::vector<std::pair<uint64_t, std::string>> reasons;
        for (const auto &[reason, count] : statFallbackReasons)
            reasons.emplace_back(count, reason);
        std::sort(reasons.rbegin(), reasons.rend());
        for (size_t i = 0; i < reasons.size() && i < 12u; ++i)
            std::fprintf(stderr, "[gs-gl]   cpu fallback %-48s %llu\n", reasons[i].second.c_str(), (unsigned long long)reasons[i].first);
        std::fprintf(stderr, "[gs-gl]   ms: downloads %.0f, uploads %.0f, texture decodes %.0f, GL flushes %.0f (prim buffer %.0f, state %.0f, grouping incl. draws %.0f, draws %.0f), Submit total %.0f\n",
                     statNs[0] / 1e6, statNs[1] / 1e6, statNs[2] / 1e6, statNs[3] / 1e6, statNs[5] / 1e6, statNs[6] / 1e6, statNs[7] / 1e6,
                     statNs[8] / 1e6, statNs[4] / 1e6);
        std::fprintf(stderr, "[gs-gl]   upload patch path %.0f ms (coalesced %d: %llu upload chunks recorded, pages applied whole %llu, masked %llu)\n",
                     statPatchNs / 1e6, coalescePatches ? 1 : 0, (unsigned long long)statPatchRecorded, (unsigned long long)statPatchWhole,
                     (unsigned long long)statPatchMasked);
        statPatchNs = statPatchRecorded = statPatchWhole = statPatchMasked = 0;
        std::fprintf(stderr,
                     "[gs-gl]   CLUT loads %llu (%.0f ms; repeats skipped %llu; CPU path %llu, CPU backend load %.0f ms; GPU %llu, of them CT16 %llu; GPU CLUT read backs %llu)\n",
                     (unsigned long long)statClutCalls, statClutNs / 1e6, (unsigned long long)statClutRepeats, (unsigned long long)statClutCpu,
                     statClutCpuNs / 1e6, (unsigned long long)statGpuCluts, (unsigned long long)statGpuCluts16, (unsigned long long)statClutMaterialize);
        std::fprintf(stderr, "[gs-gl]   CLUT loads done: same load with its CPU source changed %llu, GPU source changed %llu, other %llu\n",
                     (unsigned long long)statClutWhy[0], (unsigned long long)statClutWhy[1], (unsigned long long)statClutWhy[2]);
        statClutWhy[0] = statClutWhy[1] = statClutWhy[2] = 0;
        statClutCalls = statClutCpu = statClutMaterialize = statClutNs = statClutCpuNs = statGpuCluts = statClutRepeats = statGpuCluts16 = 0;
        for (uint64_t &ns : statNs)
            ns = 0;
        statGpuPrims = statCpuPrims = statDraws = statGroups = statDownloads = statUploads = statTexDecodes = 0;
        statFallbackReasons.clear();
        // GT4_BATCH_TRACE=<n>: after the n-th stats report, the next 1,500 batches logged (target, texture, boxes).
        if (const char *v = std::getenv("GT4_BATCH_TRACE"); v && *v && ++batchTraceReports == std::atoi(v))
            batchTraceLeft = 1500;
        if (gpuProfiling && !gpuProfile.empty())
        {
            std::vector<std::pair<uint64_t, std::string>> costs;
            uint64_t totalNs = 0, totalFrags = 0;
            for (const auto &[key, c] : gpuProfile)
            {
                costs.emplace_back(c.ns, key);
                totalNs += c.ns;
                totalFrags += c.frags;
            }
            std::sort(costs.rbegin(), costs.rend());
            std::fprintf(stderr, "[gs-gl]   GPU draws: %.1f ms, %.1f M fragment shader runs\n", totalNs / 1e6, totalFrags / 1e6);
            for (size_t i = 0; i < costs.size() && i < 24u; ++i)
            {
                const GpuCost &c = gpuProfile[costs[i].second];
                std::fprintf(stderr, "[gs-gl]   gpu %7.1f ms %7.2f M frags %6llu batches %8llu prims  %s\n", c.ns / 1e6, c.frags / 1e6,
                             (unsigned long long)c.batches, (unsigned long long)c.prims, costs[i].second.c_str());
            }
            gpuProfile.clear();
        }
        std::vector<std::pair<uint64_t, std::string>> causes;
        for (const auto &[what, count] : statDownloadCauses)
            causes.emplace_back(count, what);
        std::sort(causes.rbegin(), causes.rend());
        for (size_t i = 0; i < causes.size() && i < 16u; ++i)
            std::fprintf(stderr, "[gs-gl]   page download for %-40s %llu\n", causes[i].second.c_str(), (unsigned long long)causes[i].first);
        statDownloadCauses.clear();
        {
            std::vector<std::pair<uint64_t, std::string>> ups;
            for (const auto &[what, count] : statUploadCauses)
                ups.emplace_back(count, what);
            std::sort(ups.rbegin(), ups.rend());
            for (size_t i = 0; i < ups.size() && i < 12u; ++i)
                std::fprintf(stderr, "[gs-gl]   page upload into %-60s %llu\n", ups[i].second.c_str(), (unsigned long long)ups[i].first);
            statUploadCauses.clear();
        }
        {
            std::vector<std::pair<uint64_t, std::string>> moves;
            for (const auto &[what, count] : statMoveCauses)
                moves.emplace_back(count, what);
            std::sort(moves.rbegin(), moves.rend());
            for (size_t i = 0; i < moves.size() && i < 12u; ++i)
                std::fprintf(stderr, "[gs-gl]   page move %-60s %llu\n", moves[i].second.c_str(), (unsigned long long)moves[i].first);
            statMoveCauses.clear();
            std::vector<std::pair<std::pair<uint64_t, uint64_t>, std::string>> views;
            for (const auto &[what, count] : statViewMoves)
                views.emplace_back(count, what);
            std::sort(views.rbegin(), views.rend());
            for (size_t i = 0; i < views.size() && i < 12u; ++i)
                std::fprintf(stderr, "[gs-gl]   texture view moves %llu (back %llu): %s\n", (unsigned long long)views[i].first.first,
                             (unsigned long long)views[i].first.second, views[i].second.c_str());
            statViewMoves.clear();
        }
        // (Counted by their fields; the same text as before.)
        for (const auto &[q, count] : statSplitGroupKeys)
            statSplitGroups[splitGroupText(q)] += count;
        statSplitGroupKeys.clear();
        for (const auto &[q, count] : statSplitPrimKeys)
            statSplitPrimKinds[splitPrimText(q)] += count;
        statSplitPrimKeys.clear();
        for (const auto &[q, count] : statInterlockKeys)
            statInterlockKinds[interlockKindText(q)] += count;
        statInterlockKeys.clear();
        for (size_t i = 0; i < sizeof(statBreakWord) / sizeof(statBreakWord[0]); ++i)
        {
            if (!statBreakWord[i])
                continue;
            char text[64];
            if (const char *name = drawKeyWordName(i))
                std::snprintf(text, sizeof(text), "key %s", name);
            else
                std::snprintf(text, sizeof(text), "key word %zu", i);
            statBatchBreaks[text] += statBreakWord[i];
            statBreakWord[i] = 0;
        }
        std::vector<std::pair<uint64_t, std::string>> groups;
        for (const auto &[what, count] : statSplitGroups)
            groups.emplace_back(count, what);
        std::sort(groups.rbegin(), groups.rend());
        for (size_t i = 0; i < groups.size() && i < 10u; ++i)
            std::fprintf(stderr, "[gs-gl]   split groups %-64s %llu\n", groups[i].second.c_str(), (unsigned long long)groups[i].first);
        statSplitGroups.clear();
        for (const auto &[what, count] : statSplitPrimKinds)
            std::fprintf(stderr, "[gs-gl]   split primitives %-40s %llu\n", what.c_str(), (unsigned long long)count);
        statSplitPrimKinds.clear();
        for (const auto &[what, count] : statInterlockKinds)
            std::fprintf(stderr, "[gs-gl]   interlocked %s: %llu\n", what.c_str(), (unsigned long long)count);
        statInterlockKinds.clear();
        if (statHwTriBatches)
        {
            std::fprintf(stderr,
                         "[gs-gl]   hw triangles: %llu batches (%llu of one pixel state), %llu primitives (%llu in those); specialised: %llu batches, "
                         "%llu programs built (%zu known)\n",
                         (unsigned long long)statHwTriBatches, (unsigned long long)statHwTriUniform, (unsigned long long)statHwTriPrims,
                         (unsigned long long)statHwTriUniformPrims, (unsigned long long)statSpecBatches, (unsigned long long)statSpecBuilt,
                         specPrograms.size());
            statSpecBatches = 0;
            std::vector<std::pair<uint64_t, std::string>> kinds;
            for (const auto &[what, count] : statHwTriKinds)
                kinds.emplace_back(count, what);
            std::sort(kinds.rbegin(), kinds.rend());
            for (size_t i = 0; i < kinds.size() && i < 12u; ++i)
                std::fprintf(stderr, "[gs-gl]   hw triangles %8llu  %s\n", (unsigned long long)kinds[i].first, kinds[i].second.c_str());
            statHwTriKinds.clear();
            statHwTriBatches = statHwTriUniform = statHwTriPrims = statHwTriUniformPrims = 0;
        }
        std::vector<std::pair<uint64_t, std::string>> breaks;
        for (const auto &[what, count] : statBatchBreaks)
            breaks.emplace_back(count, what);
        std::sort(breaks.rbegin(), breaks.rend());
        for (size_t i = 0; i < breaks.size() && i < 12u; ++i)
            std::fprintf(stderr, "[gs-gl]   batch break %-40s %llu\n", breaks[i].second.c_str(), (unsigned long long)breaks[i].first);
        statBatchBreaks.clear();
        if (statOrderFreeBatches)
            std::fprintf(stderr, "[gs-gl]   alpha-bit batches drawn in one level (orderFreeMasks): %llu\n", (unsigned long long)statOrderFreeBatches);
        statOrderFreeBatches = 0;
        std::fprintf(stderr, "[gs-gl]   batches flushed %llu (split %llu: %llu prims in %llu levels; interlocked %llu: %llu prims)\n",
                     (unsigned long long)statFlushCount, (unsigned long long)statSplitBatches, (unsigned long long)statSplitPrims,
                     (unsigned long long)statSplitLevels, (unsigned long long)statInterlockBatches, (unsigned long long)statInterlockPrims);
        // Step 7 T0 counters.
        {
            std::vector<std::pair<uint64_t, uint32_t>> sites;
            for (const auto &[line, s] : statFlushSites)
                sites.emplace_back(s.count, line);
            std::sort(sites.rbegin(), sites.rend());
            for (size_t i = 0; i < sites.size() && i < 16u; ++i)
            {
                // "void __cdecl GSGlBackend::Impl::downloadPage(uint32_t,bool)" -> "GSGlBackend::Impl::downloadPage"
                std::string fn = statFlushSites[sites[i].second].function;
                if (const size_t paren = fn.find('('); paren != std::string::npos)
                    fn.resize(paren);
                if (const size_t space = fn.rfind(' '); space != std::string::npos)
                    fn.erase(0, space + 1);
                std::fprintf(stderr, "[gs-gl]   flush site %s line %u: %llu\n", fn.c_str(), sites[i].second, (unsigned long long)sites[i].first);
            }
            statFlushSites.clear();
            const auto pct = [](uint64_t part, uint64_t whole) { return whole ? 100.0 * static_cast<double>(part) / static_cast<double>(whole) : 0.0; };
            const uint64_t runs = statRunPrims - statRunSameState;
            std::fprintf(stderr,
                         "[gs-gl]   draw-state runs: primitives %llu, same draw state as the previous %llu (%.1f%%), runs %llu (mean %.1f); "
                         "resolved %llu, same key as the previous %llu (%.1f%%); draw states %llu in %llu batches (%.1f per batch)\n",
                         (unsigned long long)statRunPrims, (unsigned long long)statRunSameState, pct(statRunSameState, statRunPrims),
                         (unsigned long long)runs, runs ? static_cast<double>(statRunPrims) / static_cast<double>(runs) : 0.0,
                         (unsigned long long)statRunKeyed, (unsigned long long)statRunSameKey, pct(statRunSameKey, statRunKeyed),
                         (unsigned long long)statBatchStates, (unsigned long long)statStateBatches,
                         statStateBatches ? static_cast<double>(statBatchStates) / static_cast<double>(statStateBatches) : 0.0);
            statRunPrims = statRunSameState = statRunKeyed = statRunSameKey = statBatchStates = statStateBatches = 0;
            static const char *modes[3] = {"plain", "interlocked", "split"};
            std::vector<std::pair<uint64_t, int>> pairs;
            for (int a = 0; a < 12; ++a)
                for (int b = 0; b < 12; ++b)
                    if (statDstPairs[a][b])
                        pairs.emplace_back(statDstPairs[a][b], a * 12 + b);
            std::sort(pairs.rbegin(), pairs.rend());
            for (size_t i = 0; i < pairs.size() && i < 8u; ++i)
            {
                const int a = pairs[i].second / 12, b = pairs[i].second % 12;
                std::fprintf(stderr, "[gs-gl]   dstRead break %s zte %d zmask %d -> %s zte %d zmask %d: %llu\n", modes[a / 4], (a >> 1) & 1, a & 1,
                             modes[b / 4], (b >> 1) & 1, b & 1, (unsigned long long)pairs[i].first);
            }
            std::memset(statDstPairs, 0, sizeof(statDstPairs));
            std::fprintf(stderr,
                         "[gs-gl]   CLUT loads done by size: 16 entries %llu, 256 entries %llu; source all CPU-held %llu; all blocks loaded before and "
                         "unread since %llu\n",
                         (unsigned long long)statClut16, (unsigned long long)statClut256, (unsigned long long)statClutCpuSource,
                         (unsigned long long)statClutUnread);
            statClut16 = statClut256 = statClutCpuSource = statClutUnread = 0;
        }
        if (stateRuns || acquireRuns)
        {
            // Step 7 T1.
            std::fprintf(stderr,
                         "[gs-gl]   draw-state runs (T1): same-state calls %llu, drawn from the memo %llu; resolved again: no memo %llu, "
                         "targets/textures/direct CLUT changed %llu, new frame %llu, CLUT %llu, CPU pages %llu, vertex count %llu, a target grew %llu; "
                         "memos made %llu; page runs: acquires skipped %llu, done %llu\n",
                         (unsigned long long)statSameCalls, (unsigned long long)statMemoHits, (unsigned long long)statMemoWhy[1],
                         (unsigned long long)statMemoWhy[2], (unsigned long long)statMemoWhy[3], (unsigned long long)statMemoWhy[4],
                         (unsigned long long)statMemoWhy[5], (unsigned long long)statMemoWhy[6], (unsigned long long)statMemoTargetMiss,
                         (unsigned long long)statMemoMade, (unsigned long long)statAcquireSkips, (unsigned long long)statAcquireFull);
            statSameCalls = statMemoHits = statMemoTargetMiss = statMemoMade = statAcquireSkips = statAcquireFull = 0;
            std::fill(std::begin(statMemoWhy), std::end(statMemoWhy), 0ull);
        }
        statFlushCount = 0;
        statSplitBatches = statSplitPrims = statSplitLevels = 0;
        statInterlockBatches = statInterlockPrims = 0;
        statStart = now;
    }
};

GSGlBackend::GSGlBackend(RunOnGlThread runOnGlThread, RunOnGlThread runOnGlThreadNext)
    : m_runOnGlThread(std::move(runOnGlThread)), m_runOnGlThreadNext(std::move(runOnGlThreadNext)), m_impl(std::make_unique<Impl>())
{
    m_impl->cpu = &m_cpu;
}

GSGlBackend::~GSGlBackend() = default;

void GSGlBackend::onGl(const std::function<void()> &task) const
{
    if (!m_glReady || std::this_thread::get_id() == m_glThread || !m_runOnGlThread)
        task();
    else
        m_runOnGlThread(task);
}

void GSGlBackend::Initialize(uint8_t *vram, uint32_t vramSize)
{
    m_cpu.Initialize(vram, vramSize);
    m_impl->vram = vram;
    if (m_runOnGlThread)
        m_runOnGlThread([this] { initGl(); });
}

void GSGlBackend::initGl()
{
    if (m_glReady)
        return;
    const char *version = reinterpret_cast<const char *>(glGetString(GL_VERSION));
    const char *renderer = reinterpret_cast<const char *>(glGetString(GL_RENDERER));
    const char *vendor = reinterpret_cast<const char *>(glGetString(GL_VENDOR));
    if (!version || !renderer)
    {
        std::cerr << "[gs-gl] no OpenGL context on the GS thread" << std::endl;
        return;
    }
    GLint major = 0, minor = 0;
    glGetIntegerv(GL_MAJOR_VERSION, &major);
    glGetIntegerv(GL_MINOR_VERSION, &minor);
    m_glDescription = std::string(vendor ? vendor : "?") + " / " + renderer + " / " + version;
    const bool versionOk = major > 4 || (major == 4 && minor >= 3);
    const bool pipelineOk = versionOk && m_impl->init();
    m_glReady = pipelineOk;
    m_glThread = std::this_thread::get_id();
    std::cerr << "[gs-gl] " << m_glDescription << " (GL " << major << "." << minor << ")"
              << (m_glReady ? " ready" : " NOT ready: drawing stays on the CPU") << std::endl;
}

// ------------------------------------------------------------------ drawing

namespace
{
// Where block `block` (0-31) of a page held in PSMCT32 or PSMZ32 order starts
// (its top-left pixel), if that order keeps PSMCT32's word order inside the
// block (checked once through the GS memory helpers). A 256-colour CSM1 CT32
// CLUT is blocks CBP..CBP+3: its 8x8 quarters.
bool clutBlockOrigin(bool z32Order, uint32_t block, int &x, int &y)
{
    struct Table
    {
        bool ok[2] = {false, false};
        int x[2][32] = {}, y[2][32] = {};
    };
    static const Table table = []
    {
        Table t;
        std::vector<uint8_t> mem(8192u);
        for (uint32_t w = 0; w < 2048u; ++w)
            std::memcpy(&mem[w * 4u], &w, 4);
        int cx[2048], cy[2048], zx[2048], zy[2048];
        for (uint32_t py = 0; py < 32u; ++py)
            for (uint32_t px = 0; px < 64u; ++px)
            {
                const uint32_t c = GSMem::ReadCT32(mem.data(), 0u, 1u, px, py) & 2047u;
                const uint32_t z = GSMem::ReadZ32(mem.data(), 0u, 1u, px, py) & 2047u;
                cx[c] = static_cast<int>(px); cy[c] = static_cast<int>(py);
                zx[z] = static_cast<int>(px); zy[z] = static_cast<int>(py);
            }
        for (int order = 0; order < 2; ++order)
        {
            const int *ax = order ? zx : cx, *ay = order ? zy : cy;
            bool ok = true;
            for (uint32_t b = 0; b < 32u; ++b)
            {
                t.x[order][b] = ax[b * 64u] & ~7;
                t.y[order][b] = ay[b * 64u] & ~7;
                // Same word order inside the block as PSMCT32.
                for (uint32_t w = 0; w < 64u; ++w)
                    ok = ok && ax[b * 64u + w] - t.x[order][b] == cx[b * 64u + w] - (cx[b * 64u] & ~7) &&
                         ay[b * 64u + w] - t.y[order][b] == cy[b * 64u + w] - (cy[b * 64u] & ~7);
            }
            t.ok[order] = ok;
        }
        return t;
    }();
    const int order = z32Order ? 1 : 0;
    if (!table.ok[order] || block >= 32u)
        return false;
    x = table.x[order][block];
    y = table.y[order][block];
    return true;
}

// The pixels GSCpuBackend draws for a point or a line (its DrawLine walk:
// both ends included, colour interpolated when IIP, Z and fog interpolated,
// no texture), for fn(x, y, r, g, b, a, z, fog) in frame coordinates.
template <typename Fn>
void forEachPointOrLinePixel(const GSPrimitiveBatch &batch, Fn &&fn)
{
    const GSDrawState &state = batch.state;
    const int ofx = state.context.xyoffset.ofx >> 4, ofy = state.context.xyoffset.ofy >> 4;
    if (state.prim.type == GS_PRIM_POINT)
    {
        const GSVertex &v = batch.vertices[0];
        fn(static_cast<int>(v.x) - ofx, static_cast<int>(v.y) - ofy, v.r, v.g, v.b, v.a, v.z, v.fog);
        return;
    }
    const GSVertex &v0 = batch.vertices[0];
    const GSVertex &v1 = batch.vertices[1];
    int x0 = static_cast<int>(v0.x) - ofx, y0 = static_cast<int>(v0.y) - ofy;
    const int x1 = static_cast<int>(v1.x) - ofx, y1 = static_cast<int>(v1.y) - ofy;
    const int dx = std::abs(x1 - x0), dy = -std::abs(y1 - y0);
    const int sx = x0 < x1 ? 1 : -1, sy = y0 < y1 ? 1 : -1;
    int err = dx + dy;
    const int totalSteps = std::max(1, std::max(std::abs(x1 - x0), std::abs(y1 - y0)));
    auto clamp8 = [](int value) { return static_cast<uint8_t>(std::clamp(value, 0, 255)); };
    for (int step = 0;; ++step)
    {
        const float t = static_cast<float>(step) / static_cast<float>(totalSteps);
        uint8_t r = v1.r, g = v1.g, b = v1.b, a = v1.a;
        if (state.prim.iip)
        {
            r = clamp8(static_cast<int>(v0.r + (v1.r - v0.r) * t));
            g = clamp8(static_cast<int>(v0.g + (v1.g - v0.g) * t));
            b = clamp8(static_cast<int>(v0.b + (v1.b - v0.b) * t));
            a = clamp8(static_cast<int>(v0.a + (v1.a - v0.a) * t));
        }
        const double z = v0.z + (v1.z - v0.z) * t;
        const uint8_t fog = clamp8(static_cast<int>(v0.fog + (v1.fog - v0.fog) * t));
        fn(x0, y0, r, g, b, a, z, fog);
        if (x0 == x1 && y0 == y1)
            break;
        const int e2 = 2 * err;
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
    }
}
}

void GSGlBackend::Submit(const GSPrimitiveBatch &batch) { submitPrimitive(batch, false); }

// Step 7 T1: this primitive's draw state is the previous call's (GSRasterBackend).
void GSGlBackend::SubmitSameState(const GSPrimitiveBatch &batch) { submitPrimitive(batch, true); }

void GSGlBackend::submitPrimitive(const GSPrimitiveBatch &batch, bool sameState)
{
    if (!m_glReady)
    {
        m_cpu.Submit(batch);
        return;
    }
    // GT4_GS_SKIP_DRAWS=1 (diagnostics only: nothing is drawn): what the rest of the emulation runs at
    // without the renderer's per-primitive work.
    static const bool skipDraws = envFlag("GT4_GS_SKIP_DRAWS", false);
    if (skipDraws)
        return;
    m_impl->flushPendingPatches();
#if defined(PS2X_GAME_GT4) && !defined(GT4_STOCK_BUILD)
    // GT4_VU1_TAP=verify: every tapped vertex that reaches the renderer against its tap record.
    static const bool tapVerify = gt4tap::verifying();
    static thread_local bool inJitter = false;
    if (tapVerify && !inJitter)
        for (uint32_t i = 0; i < batch.vertexCount && i < 3u; ++i)
            if (batch.vertices[i].tapSlot)
                gt4tap::checkVertex(batch.vertices[i].tapSlot - 1u, batch.vertices[i].tapK, batch.vertices[i].x, batch.vertices[i].y);
    // Lighting step 1d: the tapped triangles kept for the frame's motion texture.
    static const bool tapOn = gt4tap::motion();
#if defined(VKGL_REDIRECT)
    if (m_impl->dlssPending && !inJitter && !m_impl->dlssCopyPart(batch))
        m_impl->dlssApply();
    // The rear-view mirror page (gt4_mirror.h): at a copy of GT4's 3D buffer (TBP 0x1a40) into another buffer; with
    // GT4_MIRROR=page the road under the mirror kept at its pass's first draw into the 3D buffer.
    static const bool mirrorPage = gt4mirror::mode() == 2;
    // (the launcher's race pictures, gt4_hud_pictures.h: the frame's HUD rectangles, the 3D buffer's copy)
    static bool picVulkan = (gt4hudpic::vulkanSeen(), true);
    (void)picVulkan;
    const bool picArmed = gt4hudpic::gsArmed();
    static const bool mirrorMove = gt4hudmove::mirrorMoved(); // (GT4_HUD_MOVE's mirror: the page's path, then drawn at its place)
    // (and while a page watches the mirror or GT4 draws it in every view, whatever GT4_MIRROR: read after its pass)
    if ((mirrorPage || picArmed || mirrorMove || gt4mirror::wanted() || gt4mirror::forcing()) && !inJitter && batch.state.context.frame.fbp == 0xd2u)
        m_impl->mirrorKeepRoad(batch);
    if ((mirrorPage || mirrorMove || gt4mirror::wanted() || gt4hudpic::gsBusy()) && !inJitter && batch.state.prim.tme &&
        batch.state.context.tex0.tbp0 == 0x1a40u && batch.state.context.frame.fbp != 0xd2u)
    {
        if (mirrorMove)
            m_impl->movedCopyFbp = batch.state.context.frame.fbp;
        m_impl->mirrorCapture();
    }
    if (picArmed && !inJitter && batch.vertexCount)
    {
        const auto &ctx = batch.state.context;
        const float ofx = static_cast<float>(ctx.xyoffset.ofx) / 16.0f, ofy = static_cast<float>(ctx.xyoffset.ofy) / 16.0f;
        float x0 = 1e9f, y0 = 1e9f, x1 = -1e9f, y1 = -1e9f;
        for (uint32_t i = 0; i < batch.vertexCount && i < 3u; ++i)
        {
            x0 = std::min(x0, batch.vertices[i].x - ofx), x1 = std::max(x1, batch.vertices[i].x - ofx);
            y0 = std::min(y0, batch.vertices[i].y - ofy), y1 = std::max(y1, batch.vertices[i].y - ofy);
        }
        x0 = std::max(x0, static_cast<float>(ctx.scissor.x0)), y0 = std::max(y0, static_cast<float>(ctx.scissor.y0));
        x1 = std::min(x1, static_cast<float>(ctx.scissor.x1) + 1.0f), y1 = std::min(y1, static_cast<float>(ctx.scissor.y1) + 1.0f);
        if (batch.state.prim.tme && ctx.tex0.tbp0 == 0x1a40u && ctx.frame.fbp != 0xd2u)
            gt4hudpic::gsCopy(ctx.frame.fbp, x0, y0, x1, y1);
        else
            gt4hudpic::gsPrimitive(ctx.frame.fbp, x0, y0, x1, y1);
    }
#endif
    if (tapOn && !inJitter && batch.state.prim.tme && !(batch.vertexCount && batch.vertices[0].tapSlot))
        m_impl->tapNoteRead(batch);
    // Lighting step 2d: ambient occlusion over the frame's 3D once it is done.
    static const bool aoOn = (Impl::aoMode() != 0 || Impl::ssrMode() != 0 || Impl::sunLit() || Impl::bloomStrength() > 0.0f) && gt4tap::enabled();
    if (aoOn && !inJitter && batch.state.prim.tme && !(batch.vertexCount && batch.vertices[0].tapSlot))
        m_impl->aoNoteRead(batch);
    if (aoOn && !inJitter && batch.vertexCount == 3u && batch.vertices[0].tapSlot)
        m_impl->aoNote(batch);
    if (aoOn && !inJitter && batch.vertexCount == 2u)
        m_impl->aoNoteClear(batch);
    if (tapOn && !inJitter && batch.vertexCount == 3u && batch.vertices[0].tapSlot)
        m_impl->tapCollect(batch);
    // GT4_TAP_SEQ=<presentation> (checking): the draw sequence around that presentation and the two after it, one
    // line per run of like draws (T tapped / U not; destination base; texture base; the tapped record's VU1 frame).
    static const uint64_t tapSeqAt = []
    {
        const char *v = std::getenv("GT4_TAP_SEQ");
        return v && *v ? std::strtoull(v, nullptr, 10) : 0ull;
    }();
    if (tapSeqAt && !inJitter && m_impl->tapPresents >= tapSeqAt && m_impl->tapPresents < tapSeqAt + 3u)
    {
        static uint64_t runKey = ~0ull, runN = 0, runPresent = 0;
        const bool tapped = batch.vertexCount && batch.vertices[0].tapSlot;
        uint64_t fr = 0;
        const bool hasFrame = tapped && gt4tap::markFrame(batch.vertices[0].tapSlot, fr);
        const bool clipMark = tapped && (batch.vertices[0].tapSlot & gt4tap::kClipMark) != 0u;
        const uint32_t tb = batch.state.prim.tme ? batch.state.context.tex0.tbp0 : 0xFFFFu;
        const uint64_t key = (tapped ? 1ull : 0ull) | (static_cast<uint64_t>(batch.state.context.frame.fbp & 0x1FFu) << 1) |
                             (static_cast<uint64_t>(tb & 0xFFFFu) << 10) | ((hasFrame ? (fr & 0xFFFFFull) : 0ull) << 26) |
                             (static_cast<uint64_t>(batch.state.prim.type) << 46) | (clipMark ? (1ull << 50) : 0ull);
        if (key != runKey || runPresent != m_impl->tapPresents)
        {
            if (runN)
                std::fprintf(stderr, "[gt4-seq] p%llu %c%s dst %03x tex %04x frame %llu prim %u x%llu\n",
                             static_cast<unsigned long long>(runPresent), (runKey & 1ull) ? 'T' : 'U', (runKey >> 50) & 1ull ? "c" : "",
                             static_cast<unsigned>((runKey >> 1) & 0x1FFu), static_cast<unsigned>((runKey >> 10) & 0xFFFFu),
                             static_cast<unsigned long long>((runKey >> 26) & 0xFFFFFull), static_cast<unsigned>((runKey >> 46) & 7u),
                             static_cast<unsigned long long>(runN));
            runKey = key;
            runN = 0;
            runPresent = m_impl->tapPresents;
        }
        ++runN;
    }
    // DLSS step 1 (GT4_UPSCALE_JITTER=1 with the tap on; checking only for now): every tapped vertex moved by its
    // VU1 frame's sub-pixel jitter, a Halton (2, 3) sequence of 8, at most half a pixel of the internal resolution
    // either way. The motion above is the unmoved vertices' (the upscaler takes the jitter separately).
    // (and always while DLSS is in use, which takes the jitter out again)
    static const bool jitterEnv = tapOn && envFlag("GT4_UPSCALE_JITTER", false);
#if defined(VKGL_REDIRECT)
    const bool jitterOn = jitterEnv || (tapOn && (m_impl->dlssOut || m_impl->dlssHudRender) && gt4up::usable() &&
                                        !m_impl->sceneFbps.count((batch.state.context.frame.fbp & (kPageCount - 1u)) | (batch.state.context.frame.fbw << 16)));
#else
    const bool jitterOn = jitterEnv;
#endif
    if (jitterOn && !inJitter && batch.vertexCount && batch.vertices[0].tapSlot)
    {
        GSPrimitiveBatch moved = batch;
        for (uint32_t i = 0; i < moved.vertexCount && i < 3u; ++i)
        {
            GSVertex &v = moved.vertices[i];
            uint64_t fr = 0;
            if (!gt4tap::markFrame(v.tapSlot, fr))
                continue;
            float jx, jy;
            upscaleJitterOf(fr, jx, jy);
            v.x += jx / static_cast<float>(m_impl->sceneScaleX());
            v.y += jy / static_cast<float>(m_impl->sceneScaleY());
        }
        inJitter = true;
        submitPrimitive(moved, sameState);
        inJitter = false;
        return;
    }
    // GT4_VU1_TAP=normals / motion (debug views, lighting step 1c): tapped vertices drawn untextured in their
    // view-space normal (n * 0.5 + 0.5) or their motion since the last frame (red |dx|, green |dy|, 1/8 px per step;
    // blue: no history); untapped ones as GT4 drew them.
    static const int tapView = gt4tap::debugView() <= 2 ? gt4tap::debugView() : 0;
    static thread_local bool inTapView = false;
    if (tapView && !inTapView && batch.vertexCount && batch.vertices[0].tapSlot)
    {
        GSPrimitiveBatch shown = batch;
        for (uint32_t i = 0; i < shown.vertexCount && i < 3u; ++i)
        {
            GSVertex &v = shown.vertices[i];
            const gt4tap::Record *r = v.tapSlot ? gt4tap::record(v.tapSlot - 1u) : nullptr;
            uint8_t c[3] = {40, 40, 40};
            if (r && v.tapK < r->count)
            {
                if (tapView == 1 && r->hasNormal)
                    for (int a = 0; a < 3; ++a)
                        c[a] = static_cast<uint8_t>(std::clamp(r->viewN[v.tapK][a] * 0.5f + 0.5f, 0.0f, 1.0f) * 255.0f);
                else if (tapView == 2 && r->hasPrev && r->prev[v.tapK][0] > -1.0e8f)
                {
                    c[0] = static_cast<uint8_t>(std::min(255.0f, std::fabs(v.x - r->prev[v.tapK][0]) * 8.0f));
                    c[1] = static_cast<uint8_t>(std::min(255.0f, std::fabs(v.y - r->prev[v.tapK][1]) * 8.0f));
                    c[2] = 0;
                }
                else if (tapView == 2)
                    c[2] = 200;
            }
            v.r = c[0];
            v.g = c[1];
            v.b = c[2];
            v.a = 128;
        }
        shown.state.prim.tme = false;
        shown.state.prim.fge = false;
        shown.state.prim.abe = false; // (the car's additive reflection layer would wash the colours out)
        shown.state.prim.iip = true;
        inTapView = true;
        submitPrimitive(shown, false);
        inTapView = false;
        return;
    }
#endif
    // GT4's race colour (gs_fullcolour.h): a new video mode forgets every pixel's full colour (as the CPU
    // reference renderer does at this draw); a full-colour draw (drawn here or on the CPU) may leave 16-bit
    // targets' texels in full colour until the next change.
    if (batch.state.fcEpoch != m_impl->fcEpoch)
        m_impl->forgetFullColour(batch.state.fcEpoch);
    if (batch.state.fc != 0u)
        m_impl->fcSeen = true;
    // Points and lines go to the GPU as one 1x1 sprite per pixel, the pixels
    // the CPU renderer draws. Drawn on the CPU instead, each needed its frame
    // and Z pages read back from the GPU (a pipeline stall; GT3's race draws
    // a few per frame). GT3_GS_GL_CPU_LINES=1 keeps the CPU path.
    if (batch.state.prim.type == GS_PRIM_POINT || batch.state.prim.type == GS_PRIM_LINE ||
        batch.state.prim.type == GS_PRIM_LINESTRIP)
    {
        static const bool cpuLines = []
        {
            const char *v = std::getenv("GT3_GS_GL_CPU_LINES");
            return v && *v == '1';
        }();
        if (!cpuLines && (batch.state.prim.type == GS_PRIM_POINT || batch.vertexCount >= 2u))
        {
            GSPrimitiveBatch pixel = batch;
            pixel.state.prim.type = GS_PRIM_SPRITE;
            pixel.state.prim.tme = false;
            pixel.vertexCount = 2u;
            const float ofx = static_cast<float>(batch.state.context.xyoffset.ofx >> 4);
            const float ofy = static_cast<float>(batch.state.context.xyoffset.ofy >> 4);
            forEachPointOrLinePixel(batch, [&](int x, int y, uint8_t r, uint8_t g, uint8_t b, uint8_t a, double z, uint8_t fog)
            {
                GSVertex &p0 = pixel.vertices[0], &p1 = pixel.vertices[1];
                p0 = GSVertex{};
                p0.x = static_cast<float>(x) + ofx;
                p0.y = static_cast<float>(y) + ofy;
                p1 = p0;
                p1.x += 1.0f;
                p1.y += 1.0f;
                p1.r = r; p1.g = g; p1.b = b; p1.a = a;
                p1.z = z;
                p1.fog = fog;
                Submit(pixel);
            });
            m_impl->memo.valid = false; // (made for the pixels' sprite state)
            return;
        }
    }
    Impl &im = *m_impl;
#if defined(PS2X_GAME_GT4) && !defined(GT4_STOCK_BUILD)
    // Diagnostic (analysis/design/tex-smooth, "The dot grid"; inert unless these are set): GT4_DOTS_PROBE=x0,y0,x1,y1,fromVsync,vsyncs (GS window pixels): every triangle / sprite meeting the box, grouped by draw state, logged at each frame's end;
    // GT4_DOTS_DUMP=<folder>: their textures as .pam (palette applied, PS2 alpha). GT4_DOTS_HIDE=<tbp hex>[,..]: those
    // textures' primitives not drawn. GT4_DOTS_NOATE=1: alpha test off on textured 3D triangles.
    if (!im.texSmoothForced)
    {
        struct DotsProbe
        {
            int box[4] = {0, 0, -1, -1}, focus[4] = {0, 0, -1, -1};
            unsigned long long from = 0, frames = 0;
            std::string dump;
            std::vector<uint32_t> hide;
            bool noAte = false;
            struct Cls { int order = 0; unsigned long long prims = 0; double area = 0, tppMin = 1e9, tppMax = 0; std::vector<uint8_t> mask; };
            int masksWritten = 0;
            std::map<std::string, Cls> classes;
            unsigned long long frame = ~0ull, tick = 0;
            std::unordered_set<std::string> dumped;
        };
        static DotsProbe dp = [] {
            DotsProbe p;
            if (const char *v = std::getenv("GT4_DOTS_PROBE"))
                std::sscanf(v, "%d,%d,%d,%d,%llu,%llu", &p.box[0], &p.box[1], &p.box[2], &p.box[3], &p.from, &p.frames);
            if (const char *v = std::getenv("GT4_DOTS_DUMP"))
                p.dump = v;
            if (const char *v = std::getenv("GT4_DOTS_FOCUS"))
                std::sscanf(v, "%d,%d,%d,%d", &p.focus[0], &p.focus[1], &p.focus[2], &p.focus[3]);
            if (const char *v = std::getenv("GT4_DOTS_HIDE"))
            {
                for (const char *c = v; *c;)
                {
                    char *end = nullptr;
                    const unsigned long t = std::strtoul(c, &end, 16);
                    if (end == c)
                        break;
                    p.hide.push_back(static_cast<uint32_t>(t));
                    c = *end ? end + 1 : end;
                }
            }
            const char *n = std::getenv("GT4_DOTS_NOATE");
            p.noAte = n && *n == '1';
            return p;
        }();
        const bool tri3 = batch.state.prim.type == GS_PRIM_TRIANGLE || batch.state.prim.type == GS_PRIM_TRISTRIP ||
                          batch.state.prim.type == GS_PRIM_TRIFAN;
        if (dp.frames && im.frameCounter != dp.frame)
        {
            if (dp.frame != ~0ull && !dp.classes.empty())
            {
                std::vector<std::pair<int, std::string>> rows;
                for (const auto &[k, c] : dp.classes)
                {
                    char t[96];
                    std::snprintf(t, sizeof(t), " | prims %llu area %.0f texel/px %.2f-%.2f", c.prims, c.area, c.tppMin, c.tppMax);
                    rows.emplace_back(c.order, k + t);
                }
                std::sort(rows.begin(), rows.end());
                std::fprintf(stderr, "[dots] frame %llu vsync %llu: %zu draw states meet the box\n", dp.frame, dp.tick, rows.size());
                for (const auto &r : rows)
                    std::fprintf(stderr, "[dots]  #%d %s\n", r.first, r.second.c_str());
                // the first frame's coverage per draw state (pixel centres inside its triangles / sprites), box-sized PGMs
                if (!dp.dump.empty() && dp.masksWritten++ == 0)
                {
                    const int bw = dp.box[2] - dp.box[0] + 1, bh = dp.box[3] - dp.box[1] + 1;
                    for (const auto &[k, c] : dp.classes)
                    {
                        if (c.mask.empty())
                            continue;
                        char name[64];
                        std::snprintf(name, sizeof(name), "\\mask-%03d.pgm", c.order);
                        if (FILE *f = std::fopen((dp.dump + name).c_str(), "wb"))
                        {
                            std::fprintf(f, "P5\n%d %d\n255\n", bw, bh);
                            std::fwrite(c.mask.data(), 1, c.mask.size(), f);
                            std::fclose(f);
                        }
                    }
                }
            }
            dp.classes.clear();
            dp.frame = im.frameCounter;
            ps2x::vblank::Clock::time_point due{}, next{};
            uint64_t tick = 0;
            ps2x::vblank::grid(tick, due, next);
            dp.tick = tick;
        }
        if (dp.frames && dp.tick >= dp.from && dp.tick < dp.from + dp.frames && batch.vertexCount >= 2u &&
            (tri3 || batch.state.prim.type == GS_PRIM_SPRITE))
        {
            const GSContext &c = batch.state.context;
            const float ox = static_cast<float>(c.xyoffset.ofx) / 16.0f, oy = static_cast<float>(c.xyoffset.ofy) / 16.0f;
            float xs[3] = {}, ys[3] = {};
            const int nv = tri3 ? 3 : 2;
            float bx0 = 1e9f, by0 = 1e9f, bx1 = -1e9f, by1 = -1e9f;
            for (int i = 0; i < nv; ++i)
            {
                xs[i] = batch.vertices[i].x - ox;
                ys[i] = batch.vertices[i].y - oy;
                bx0 = std::min(bx0, xs[i]); bx1 = std::max(bx1, xs[i]);
                by0 = std::min(by0, ys[i]); by1 = std::max(by1, ys[i]);
            }
            if (bx1 >= dp.box[0] && bx0 <= dp.box[2] && by1 >= dp.box[1] && by0 <= dp.box[3])
            {
                const GSTex0Reg &t0 = c.tex0;
                const uint64_t t1 = c.tex1;
                const uint32_t ate = static_cast<uint32_t>(c.test & 1u), atst = static_cast<uint32_t>((c.test >> 1) & 7u),
                               aref = static_cast<uint32_t>((c.test >> 4) & 0xFFu), afail = static_cast<uint32_t>((c.test >> 12) & 3u),
                               ztst = static_cast<uint32_t>((c.test >> 17) & 3u), zte = static_cast<uint32_t>((c.test >> 16) & 1u);
                char key[420];
                std::snprintf(key, sizeof(key),
                              "fbp %03x/%u psm %02x | prim %d tme %d fge %d abe %d fst %d iip %d | tex %04x/%u psm %02x %ux%u (log2 %u/%u) tcc %u tfx %u "
                              "cbp %04x cpsm %02x csa %u | tex1 lcm %u mxl %u mmag %u mmin %u l %u k %d mip %u (%u-%u) linear %d | ate %u atst %u aref %02x afail %u "
                              "zte %u ztst %u zmsk %d | alpha %010llx pabe %d fba %llu | clamp %llx | texa %02x/%d/%02x fbmsk %08x",
                              c.frame.fbp, c.frame.fbw, c.frame.psm, static_cast<int>(batch.state.prim.type), batch.state.prim.tme ? 1 : 0,
                              batch.state.prim.fge ? 1 : 0, batch.state.prim.abe ? 1 : 0, batch.state.prim.fst ? 1 : 0, batch.state.prim.iip ? 1 : 0,
                              t0.tbp0, t0.tbw, t0.psm, batch.state.textureWidth, batch.state.textureHeight, t0.tw, t0.th, t0.tcc, t0.tfx, t0.cbp,
                              t0.cpsm, t0.csa, static_cast<unsigned>(t1 & 1u), static_cast<unsigned>((t1 >> 2) & 7u),
                              static_cast<unsigned>((t1 >> 5) & 1u), static_cast<unsigned>((t1 >> 6) & 7u), static_cast<unsigned>((t1 >> 19) & 3u),
                              static_cast<int>(static_cast<int16_t>(static_cast<uint16_t>(((t1 >> 32) & 0xFFFu) << 4)) >> 4),
                              batch.state.mipTag, batch.state.mipLo, batch.state.mipHi, batch.state.linearFilter ? 1 : 0, ate, atst, aref, afail,
                              zte, ztst, c.zbuf.zmask ? 1 : 0, static_cast<unsigned long long>(c.alpha), batch.state.pabe ? 1 : 0,
                              static_cast<unsigned long long>(c.fba), static_cast<unsigned long long>(c.clamp), batch.state.texa.ta0,
                              batch.state.texa.aem ? 1 : 0, batch.state.texa.ta1, c.frame.fbmsk);
                auto it = dp.classes.find(key);
                if (it == dp.classes.end())
                {
                    it = dp.classes.emplace(key, DotsProbe::Cls{}).first;
                    it->second.order = static_cast<int>(dp.classes.size());
                }
                DotsProbe::Cls &cl = it->second;
                ++cl.prims;
                bool inFocus = false;
                {
                    const int bw = dp.box[2] - dp.box[0] + 1, bh = dp.box[3] - dp.box[1] + 1;
                    if (cl.mask.empty())
                        cl.mask.assign(static_cast<size_t>(bw) * static_cast<size_t>(bh), 0);
                    const int x0 = std::max(dp.box[0], floorToInt(bx0)), x1 = std::min(dp.box[2], ceilToInt(bx1));
                    const int y0 = std::max(dp.box[1], floorToInt(by0)), y1 = std::min(dp.box[3], ceilToInt(by1));
                    for (int y = y0; y <= y1; ++y)
                        for (int x = x0; x <= x1; ++x)
                        {
                            const float px = x + 0.5f, py = y + 0.5f;
                            bool in;
                            if (tri3)
                            {
                                const float e0 = (xs[1] - xs[0]) * (py - ys[0]) - (ys[1] - ys[0]) * (px - xs[0]);
                                const float e1 = (xs[2] - xs[1]) * (py - ys[1]) - (ys[2] - ys[1]) * (px - xs[1]);
                                const float e2 = (xs[0] - xs[2]) * (py - ys[2]) - (ys[0] - ys[2]) * (px - xs[2]);
                                in = (e0 >= 0 && e1 >= 0 && e2 >= 0) || (e0 <= 0 && e1 <= 0 && e2 <= 0);
                            }
                            else
                                in = px >= bx0 && px < bx1 && py >= by0 && py < by1;
                            if (in)
                            {
                                cl.mask[static_cast<size_t>(y - dp.box[1]) * static_cast<size_t>(bw) + static_cast<size_t>(x - dp.box[0])] = 255;
                                if (x >= dp.focus[0] && x <= dp.focus[2] && y >= dp.focus[1] && y <= dp.focus[3])
                                    inFocus = true;
                            }
                        }
                }
                if (tri3 && batch.state.prim.tme)
                {
                    const double as = 0.5 * std::fabs((xs[1] - xs[0]) * (ys[2] - ys[0]) - (xs[2] - xs[0]) * (ys[1] - ys[0]));
                    double us[3], vs[3];
                    for (int i = 0; i < 3; ++i)
                    {
                        const GSVertex &v = batch.vertices[i];
                        if (batch.state.prim.fst)
                        {
                            us[i] = v.u / 16.0;
                            vs[i] = v.v / 16.0;
                        }
                        else
                        {
                            const double q = v.q != 0.0f ? v.q : 1.0;
                            us[i] = v.s / q * batch.state.textureWidth;
                            vs[i] = v.t / q * batch.state.textureHeight;
                        }
                    }
                    const double at = 0.5 * std::fabs((us[1] - us[0]) * (vs[2] - vs[0]) - (us[2] - us[0]) * (vs[1] - vs[0]));
                    cl.area += as;
                    if (as > 4.0)
                    {
                        const double tpp = std::sqrt(at / as);
                        cl.tppMin = std::min(cl.tppMin, tpp);
                        cl.tppMax = std::max(cl.tppMax, tpp);
                    }
                }
                if (!dp.dump.empty() && batch.state.prim.tme && inFocus)
                {
                    // the texture as it is at this draw (GT4 streams textures through the same addresses within a frame):
                    // one file per draw state and content
                    uint64_t content = 0x9E3779B97F4A7C15ull;
                    for (int y = 0; y < batch.state.textureHeight && y < 1024; ++y)
                        for (int x = 0; x < batch.state.textureWidth && x < 1024; ++x)
                        {
                            const uint32_t raw = readTexel(im.vram, t0.psm, t0.tbp0, std::max<uint32_t>(t0.tbw, 1u), static_cast<uint32_t>(x),
                                                           static_cast<uint32_t>(y));
                            content = (content ^ raw) * 0x100000001B3ull;
                        }
                    char name[160];
                    std::snprintf(name, sizeof(name), "f%03d-%04x-psm%02x-%ux%u-cbp%04x-csa%u-%08x", cl.order, t0.tbp0, t0.psm,
                                  batch.state.textureWidth, batch.state.textureHeight, t0.cbp, t0.csa, static_cast<uint32_t>(content));
                    if (dp.dumped.insert(name).second && batch.state.textureWidth <= 1024 && batch.state.textureHeight <= 1024)
                    {
                        std::fprintf(stderr, "[dots] focus draw #%d %s: v0 %.1f,%.1f uv %.3f,%.3f q %.3f | v1 %.1f,%.1f uv %.3f,%.3f | v2 %.1f,%.1f uv %.3f,%.3f\n",
                                     cl.order, name, xs[0], ys[0], batch.vertices[0].s, batch.vertices[0].t, batch.vertices[0].q, xs[1], ys[1],
                                     batch.vertices[1].s, batch.vertices[1].t, xs[2], ys[2], batch.vertices[2].s, batch.vertices[2].t);
                        const uint32_t psm = t0.psm;
                        const bool fourBit = psm == GS_PSM_T4 || psm == GS_PSM_T4HL || psm == GS_PSM_T4HH;
                        const bool paletted = fourBit || psm == GS_PSM_T8 || psm == GS_PSM_T8H;
                        uint32_t palette[256];
                        if (paletted)
                            im.buildPalette(batch.state, palette);
                        const int w = batch.state.textureWidth, h = batch.state.textureHeight;
                        std::string path = dp.dump + "\\" + name + ".pam";
                        if (FILE *f = std::fopen(path.c_str(), "wb"))
                        {
                            std::fprintf(f, "P7\nWIDTH %d\nHEIGHT %d\nDEPTH 4\nMAXVAL 255\nTUPLTYPE RGB_ALPHA\nENDHDR\n", w, h);
                            std::vector<uint32_t> row(static_cast<size_t>(w));
                            for (int y = 0; y < h; ++y)
                            {
                                for (int x = 0; x < w; ++x)
                                {
                                    const uint32_t raw = readTexel(im.vram, psm, t0.tbp0, std::max<uint32_t>(t0.tbw, 1u),
                                                                      static_cast<uint32_t>(x), static_cast<uint32_t>(y));
                                    row[static_cast<size_t>(x)] =
                                        fourBit ? palette[raw & 0x0Fu]
                                        : paletted ? palette[raw & 0xFFu]
                                        : (psm == GS_PSM_CT16 || psm == GS_PSM_CT16S) ? applyTexa(batch.state.texa, psm, rgba5551To8888(raw))
                                                                                     : applyTexa(batch.state.texa, psm, raw);
                                }
                                std::fwrite(row.data(), 4, row.size(), f);
                            }
                            std::fclose(f);
                        }
                    }
                }
            }
        }
        if (batch.state.prim.tme && !dp.hide.empty() &&
            std::find(dp.hide.begin(), dp.hide.end(), batch.state.context.tex0.tbp0) != dp.hide.end())
            return;
        if (dp.noAte && tri3 && batch.state.prim.tme && (batch.state.context.test & 1u))
        {
            GSPrimitiveBatch b = batch;
            b.state.context.test &= ~uint64_t{1};
            im.texSmoothForced = true;
            Submit(b);
            im.texSmoothForced = false;
            return;
        }
    }
    // Texture smoothing (Impl::texSmooth): a nearest-sampled 3D triangle's texture made bilinear (TEX1 MMAG linear, MMIN's
    // base level linear, its mip choice kept); Z formats never. A texture a target holds is set back to nearest below
    // (texSmoothForced), after the footprint (one texel wider, as for bilinear) has found its pages.
    if (im.texSmooth && im.stats && !im.texSmoothForced && batch.state.prim.tme && batch.state.linearFilter &&
        (batch.state.prim.type == GS_PRIM_TRIANGLE || batch.state.prim.type == GS_PRIM_TRISTRIP || batch.state.prim.type == GS_PRIM_TRIFAN))
        ++im.statSmoothAlready;
    if (im.texSmooth && !im.texSmoothForced && batch.state.prim.tme && !batch.state.linearFilter &&
        (batch.state.prim.type == GS_PRIM_TRIANGLE || batch.state.prim.type == GS_PRIM_TRISTRIP ||
         batch.state.prim.type == GS_PRIM_TRIFAN) &&
        batch.state.context.tex0.psm != GS_PSM_Z24 && batch.state.context.tex0.psm != GS_PSM_Z32 &&
        batch.state.context.tex0.psm != GS_PSM_Z16 && batch.state.context.tex0.psm != GS_PSM_Z16S)
    {
        GSPrimitiveBatch smooth = batch;
        uint64_t &t1 = smooth.state.context.tex1;
        const uint64_t mmin = (t1 >> 6u) & 7u;
        const uint64_t linMin = mmin == 0u ? 1u : mmin == 2u ? 4u : mmin == 3u ? 5u : mmin;
        t1 = (t1 & ~(uint64_t{0xF} << 5u)) | (uint64_t{1} << 5u) | (linMin << 6u);
        smooth.state.linearFilter = true;
        im.texSmoothForced = true;
        ++im.statSmoothPrims;
        submitPrimitive(smooth, sameState); // (same state in, same smoothed state out: step 7 T1's memo serves it)
        im.texSmoothForced = false;
        return;
    }
#endif
    Impl::SampledNs submitTimer(im.statNs[4], im);
    const GSDrawState &state = batch.state;
    const GSContext &ctx = state.context;
    const GSTex0Reg &tex = ctx.tex0;
    if (im.stats)
    {
        // Step 7 T0: runs of one draw state.
        ++im.statRunPrims;
        if (im.statPrevStateValid && gsSameDrawState(state, im.statPrevState))
            ++im.statRunSameState;
        else
        {
            im.statPrevState = state;
            im.statPrevStateValid = true;
        }
    }
    // Step 7 T1 (draw-state runs): a primitive with the previous call's draw
    // state is drawn with what that call resolved (the memo) when nothing it
    // depends on changed; else it is resolved in full and the memo made again.
    // With PS2X_GS_GL_RUN_VERIFY=1 a primitive the memo would serve is
    // resolved in full and compared, and the memo kept if they match.
    // GT4's car view at full height (gs_car_view.h): armed while the 480p menus run; a change resets the runs (their
    // memo resolved the targets with the other choice).
    // (Only while the display is 480p: GT4's boot screens and 2 Player Battle are 480i fields of the same shape.)
    const bool tallArmed = im.tallOn && gt4carview::armed() && ps2x::crt::is480p(ps2x::crt::current());
    if (tallArmed != im.tallArmedSeen)
    {
        im.tallArmedSeen = tallArmed;
        im.resetRuns();
    }
    const int memoWhy = sameState ? im.memoState(batch) : -1;
    bool memoHit = memoWhy == 0 && !im.runVerify;
    bool memoCheck = memoWhy == 0 && im.runVerify;
    if (sameState)
        im.noteSameState(memoWhy);
    if (!memoHit && !memoCheck)
        im.memo.valid = false;
    // (PS2X_GS_GL_RUN_VERIFY: a checked primitive the full path then draws on
    // the CPU, leaving before afterResolve, is a mismatch.)
    struct MemoCheckEnd
    {
        Impl &im;
        ~MemoCheckEnd()
        {
            if (im.memoCheckPending)
                im.memoCheckLost();
        }
    } memoCheckEnd{im};

    const char *reason = nullptr;
    char reasonText[96];
    const GSPrimType type = state.prim.type;
    const bool triangle = type == GS_PRIM_TRIANGLE || type == GS_PRIM_TRISTRIP || type == GS_PRIM_TRIFAN;
    const bool sprite = type == GS_PRIM_SPRITE;
    const uint32_t ztst = static_cast<uint32_t>((ctx.test >> 17) & 3u);
    // Z test ALWAYS with Z writes masked neither reads nor writes Z.
    const bool zte = ((ctx.test >> 16) & 1u) != 0u && !(ztst == 1u && ctx.zbuf.zmask);
    if (!triangle && !sprite)
    {
        // Which primitives (0 point, 1 line, 2 line strip) into which buffer.
        std::snprintf(reasonText, sizeof(reasonText), "primitive type %u fbp 0x%x psm 0x%x tme %u", static_cast<unsigned>(type),
                      static_cast<unsigned>(ctx.frame.fbp), static_cast<unsigned>(ctx.frame.psm), state.prim.tme ? 1u : 0u);
        reason = reasonText;
    }
    else if ((triangle && batch.vertexCount < 3u) || (sprite && batch.vertexCount < 2u))
        reason = "vertex count";
    else if (!isColor32(ctx.frame.psm) && ctx.frame.psm != GS_PSM_CT16 && ctx.frame.psm != GS_PSM_Z24 && ctx.frame.psm != GS_PSM_Z32)
    {
        std::snprintf(reasonText, sizeof(reasonText), "frame format 0x%02x", static_cast<unsigned>(ctx.frame.psm));
        reason = reasonText;
    }
    else if (zte && ctx.zbuf.psm != GS_PSM_Z24 && !(ctx.zbuf.psm == GS_PSM_Z16 && im.z16Gpu))
    {
        std::snprintf(reasonText, sizeof(reasonText), "Z format 0x%02x ztst %u zmask %u", static_cast<unsigned>(ctx.zbuf.psm), ztst,
                      ctx.zbuf.zmask ? 1u : 0u);
        reason = reasonText;
    }
    else if ((state.colclamp & 1u) == 0u)
        reason = "colour wrap";
    else if (state.prim.tme && (state.textureWidth > 1024u || state.textureHeight > 1024u))
        reason = "texture size";
    // GS mipmapping, a per-pixel primitive (gs_mip.h): sampled on the GPU from
    // one raw texture over its levels' pages (texMode 13, bindless only).
    const bool mipPerPixel = state.prim.tme && state.mipTag == gsmip::kTagPerPixel;
    if (!reason && mipPerPixel)
    {
        const uint32_t mpsm = tex.psm;
        if (!im.hasBindless || !im.rawTextures)
            reason = "mipmap without bindless raw textures";
        else if (state.mipHi >= 6u)
            reason = "mipmap level 6";
        else if (!(isColor32(mpsm) || mpsm == GS_PSM_CT16 || mpsm == GS_PSM_CT16S || mpsm == GS_PSM_T8 || mpsm == GS_PSM_T4 ||
                   mpsm == GS_PSM_T8H || mpsm == GS_PSM_T4HL || mpsm == GS_PSM_T4HH))
            reason = "mipmap texture format";
    }
    // GS dithering: its matrix uses the state words of a per-pixel mip primitive's levels 3-5 (v[13].w,
    // v[14].z/w): such a primitive, dithered, is drawn by the CPU (none seen in GT4: its dithered draws are
    // the sprites copying the picture into the 16-bit display buffer).
    const bool dither = gsdither::applies(state);
    if (!reason && dither && mipPerPixel)
        reason = "dither with a per-pixel mip primitive";
    // Texels a draw can reach through the wrap modes (decode / sync extent).
    const int wrapU = static_cast<int>(ctx.clamp & 3u), wrapV = static_cast<int>((ctx.clamp >> 2) & 3u);
    const int minU = static_cast<int>((ctx.clamp >> 4) & 0x3FFu), maxU = static_cast<int>((ctx.clamp >> 14) & 0x3FFu);
    const int minV = static_cast<int>((ctx.clamp >> 24) & 0x3FFu), maxV = static_cast<int>((ctx.clamp >> 34) & 0x3FFu);
    auto extent = [](int size, int mode, int rmin, int rmax) {
        if (mode == 2) return std::max(size, rmax + 1);
        if (mode == 3) return std::max(size, (rmin | rmax) + 1);
        return size;
    };
    const int decodeW = std::min(1024, extent(static_cast<int>(state.textureWidth), wrapU, minU, maxU));
    const int decodeH = std::min(1024, extent(static_cast<int>(state.textureHeight), wrapV, minV, maxV));

    // Texels this primitive can actually sample (as GSCpuBackend::Submit's
    // footprint: the vertices' texel range, one wider for bilinear, then the
    // wrap mode); the declared TW x TH often exceeds the buffer it reads.
    int fu0 = 0, fu1 = decodeW - 1, fv0 = 0, fv1 = decodeH - 1;
    int tfu0 = fu0, tfu1 = fu1, tfv0 = fv0, tfv1 = fv1; // texels it can read (self-read test)
    // The texels the vertices' range samples, without margins (nearest: the
    // texel under each, bilinear: the pair around u - 0.5); readKnown when it
    // stays inside the texture (no wrap-around).
    bool readKnown = false;
    int ru0 = 0, ru1 = -1, rv0 = 0, rv1 = -1;
    // The most rows a pixel reads above its own row (y - v, largest at a
    // vertex as both are affine over the primitive).
    float readAbove = -1e9f;
    // The vertices' level-0 texel range (mipRawSource scales it to each level).
    float texLo[2] = {0.0f, 0.0f}, texHi[2] = {0.0f, 0.0f};
    bool texKnown = false;
    // (A lambda: a primitive served from the memo (step 7 T1) needs it only
    // if it falls back to a full resolution.)
    auto footprint = [&] {
        const int texW = static_cast<int>(state.textureWidth), texH = static_cast<int>(state.textureHeight);
        float lo[2] = {0.0f, 0.0f}, hi[2] = {static_cast<float>(texW), static_cast<float>(texH)};
        bool known = true;
        const uint32_t count = std::min<uint32_t>(batch.vertexCount, 3u);
        for (uint32_t i = 0; i < count; ++i)
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
            readAbove = std::max(readAbove, (v.y - static_cast<float>(ctx.xyoffset.ofy) / 16.0f) - t);
            if (i == 0u)
            {
                lo[0] = hi[0] = u;
                lo[1] = hi[1] = t;
            }
            else
            {
                lo[0] = std::min(lo[0], u); hi[0] = std::max(hi[0], u);
                lo[1] = std::min(lo[1], t); hi[1] = std::max(hi[1], t);
            }
        }
        auto range = [&](float l, float h, int size, int mode, int rmin, int rmax, int &a, int &b) {
            if (mode == 2) { a = std::min(rmin, rmax); b = rmax; return; }
            if (mode == 3) { a = 0; b = (rmin | rmax) & 0x3FF; return; }
            if (!known || !(h - l < 4096.0f)) { a = 0; b = size - 1; return; }
            a = floorToInt(l) - 2;
            b = ceilToInt(h) + 1;
            // An FST sprite, nearest: both renderers clamp a pixel's 12.4 texel coordinate at 0 (GSCpuBackend's
            // DrawSprite, the shader's iu / iv), so no read lies left of / above texel 0 and REPEAT never wraps there: the margin stops at 0 instead of making the footprint the whole texture. GT4's 480p car views
            // copy their 640-wide composite (a 1024 x 1024 CT32 texture, TBW 10, REPEAT) in 64-wide strips from u 0:
            // each strip's footprint was all 512 pages of GS memory, moved into the composite's target (~270,000
            // page moves per 10 s on the car pages). PS2X_GS_GL_FST_FOOTPRINT=0: the margin as before.
            static const bool fstFootprint = envFlag("PS2X_GS_GL_FST_FOOTPRINT", true);
            if (fstFootprint && sprite && state.prim.fst && !state.linearFilter && mode == 0)
                a = std::max(a, 0);
            if (mode == 1) { a = std::clamp(a, 0, size - 1); b = std::clamp(b, 0, size - 1); }
            else if (a < 0 || b >= size) { a = 0; b = size - 1; }
        };
        range(lo[0], hi[0], texW, wrapU, minU, maxU, fu0, fu1);
        range(lo[1], hi[1], texH, wrapV, minV, maxV, fv0, fv1);
        texLo[0] = lo[0]; texLo[1] = lo[1]; texHi[0] = hi[0]; texHi[1] = hi[1];
        texKnown = known;
        fu0 = std::clamp(fu0, 0, decodeW - 1); fu1 = std::clamp(fu1, fu0, decodeW - 1);
        fv0 = std::clamp(fv0, 0, decodeH - 1); fv1 = std::clamp(fv1, fv0, decodeH - 1);
        // Region clamp reads only the vertices' texel range clamped into the
        // region (the footprint above keeps the whole region).
        auto tight = [&](float l, float h, int mode, int a, int b, int &ta, int &tb) {
            ta = a;
            tb = b;
            if (mode != 2 || !known || !(h - l < 4096.0f))
                return;
            ta = std::clamp(floorToInt(l) - 2, a, b);
            tb = std::clamp(ceilToInt(h) + 1, a, b);
        };
        tight(lo[0], hi[0], wrapU, fu0, fu1, tfu0, tfu1);
        tight(lo[1], hi[1], wrapV, fv0, fv1, tfv0, tfv1);
        if (known && hi[0] - lo[0] < 4096.0f && hi[1] - lo[1] < 4096.0f)
        {
            const float m = state.linearFilter ? 0.5f : 0.0f;
            ru0 = floorToInt(lo[0] - m);
            ru1 = floorToInt(hi[0] - m) + (state.linearFilter ? 1 : 0);
            rv0 = floorToInt(lo[1] - m);
            rv1 = floorToInt(hi[1] - m) + (state.linearFilter ? 1 : 0);
#if !defined(PS2X_GAME_GT3)
            // CLAMP / REGION_CLAMP fold a bilinear pair at the edge back inside,
            // so the read is known. GT4's wheel blur samples u 0..64 of its
            // 64-wide target (REGION_CLAMP 0..63) while writing rows it reads;
            // without the fold it was never banded and its bottom rows read a
            // stale copy (R2, analysis/phase5/specs/step4-picture.md 3).
            // GT3_GS_GL_SELF_FOLD=0: not folded (as before).
            static const bool foldEdges = envFlag("GT3_GS_GL_SELF_FOLD", true);
            if (foldEdges)
            {
                auto fold = [](int &a, int &b, int mode, int size, int rmin, int rmax) {
                    if (mode == 1)
                    {
                        a = std::clamp(a, 0, size - 1);
                        b = std::clamp(b, 0, size - 1);
                    }
                    else if (mode == 2)
                    {
                        const int lo2 = std::min(rmin, rmax), hi2 = std::max(rmin, rmax);
                        a = std::clamp(a, lo2, hi2);
                        b = std::clamp(b, lo2, hi2);
                    }
                };
                fold(ru0, ru1, wrapU, texW, minU, maxU);
                fold(rv0, rv1, wrapV, texH, minV, maxV);
            }
#endif
            readKnown = ru0 >= 0 && rv0 >= 0 && ru1 < texW && rv1 < texH;
        }
    };
    if (state.prim.tme && !memoHit)
        footprint();

    // Drawn by the CPU backend after the pages it reads and writes are
    // brought back to the CPU copy.
    auto drawOnCpu = [&](const char *why) {
        im.flushBatch();
        const float ofxf = static_cast<float>(ctx.xyoffset.ofx) / 16.0f, ofyf = static_cast<float>(ctx.xyoffset.ofy) / 16.0f;
        float minX = 4096.0f, minY = 4096.0f, maxX = -4096.0f, maxY = -4096.0f;
        for (uint32_t i = 0; i < std::max<uint32_t>(batch.vertexCount, 1u) && i < 3u; ++i)
        {
            minX = std::min(minX, batch.vertices[i].x - ofxf);
            maxX = std::max(maxX, batch.vertices[i].x - ofxf);
            minY = std::min(minY, batch.vertices[i].y - ofyf);
            maxY = std::max(maxY, batch.vertices[i].y - ofyf);
        }
        const int x0 = std::max<int>(floorToInt(minX) - 1, ctx.scissor.x0);
        const int y0 = std::max<int>(floorToInt(minY) - 1, ctx.scissor.y0);
        const int x1 = std::min<int>(ceilToInt(maxX) + 1, ctx.scissor.x1);
        const int y1 = std::min<int>(ceilToInt(maxY) + 1, ctx.scissor.y1);
        if (x1 >= x0 && y1 >= y0)
        {
            im.bumpPages(ctx.frame.fbp, false, ctx.frame.fbw, ctx.frame.psm, x0, y0, x1, y1);
            if (zte)
                im.bumpPages(ctx.zbuf.zbp, false, ctx.frame.fbw, ctx.zbuf.psm, x0, y0, x1, y1);
        }
        im.materializeClut();
        im.cause = "cpu draw";
        if (state.prim.tme)
            im.syncPagesToCpu(tex.tbp0, tex.tbw, tex.psm, 0, 0, decodeW - 1, decodeH - 1);
        // A per-pixel mip primitive also reads its other levels.
        if (mipPerPixel)
            for (int n = std::max<int>(state.mipLo, 1); n <= static_cast<int>(state.mipHi); ++n)
            {
                const gsmip::Level L = gsmip::level(ctx, n);
                const int dw = std::min(1024, extent(static_cast<int>(L.w), wrapU, L.minU, L.maxU));
                const int dh = std::min(1024, extent(static_cast<int>(L.h), wrapV, L.minV, L.maxV));
                im.syncPagesToCpu(L.tbp, L.tbw, tex.psm, 0, 0, dw - 1, dh - 1);
            }
        m_cpu.Submit(batch);
        m_cpu.Flush();
        ++im.statCpuPrims;
        ++im.statFallbackReasons[why];
        im.clutUnreadBlocks = 0u; // (statistics: it may read any CLUT block)
        im.reportStats();
    };
    if (reason)
    {
        drawOnCpu(reason);
        return;
    }
    if (zte && ztst == 0u)
        return; // Z test NEVER: nothing is drawn
    // A PSMZ16 Z buffer on the GPU (PS2X_GS_GL_Z16): Layout::Z16 depth target, the shader clamps Z to 0xFFFF.
    const bool zbuf16 = zte && ctx.zbuf.psm == GS_PSM_Z16;
    if (zbuf16 && im.stats)
    {
        ++im.statZ16Prims;
        const double zTop = sprite ? batch.vertices[1].z
                                   : std::max(std::max(batch.vertices[0].z, batch.vertices[1].z), batch.vertices[2].z);
        if (zTop > 65535.0)
            ++im.statZ16ZAbove;
    }

    // ---- primitive setup, exactly as GSCpuBackend::DrawTriangle / DrawSprite
    GpuPrim prim{};
    // A draw reading the target it draws into (see selfSnap).
    bool selfRead = false, selfTexelIsPixel = false;
    int selfTarget = -1, selfFirstRow = 0, selfLastRow = -1;
    int selfU0 = 0, selfV0 = 0, selfU1 = -1, selfV1 = -1; // texels it can read
    const int ofx = ctx.xyoffset.ofx >> 4, ofy = ctx.xyoffset.ofy >> 4;
    int bx0, by0, bx1, by1;
    if (triangle)
    {
        const GSVertex &v0 = batch.vertices[0], &v1 = batch.vertices[1], &v2 = batch.vertices[2];
        const float fx0 = v0.x - static_cast<float>(ofx), fy0 = v0.y - static_cast<float>(ofy);
        const float fx1 = v1.x - static_cast<float>(ofx), fy1 = v1.y - static_cast<float>(ofy);
        const float fx2 = v2.x - static_cast<float>(ofx), fy2 = v2.y - static_cast<float>(ofy);
        bx0 = floorToInt(std::min(std::min(fx0, fx1), fx2));
        bx1 = ceilToInt(std::max(std::max(fx0, fx1), fx2));
        by0 = floorToInt(std::min(std::min(fy0, fy1), fy2));
        by1 = ceilToInt(std::max(std::max(fy0, fy1), fy2));
        bx0 = std::clamp<int>(bx0, ctx.scissor.x0, ctx.scissor.x1);
        bx1 = std::clamp<int>(bx1, ctx.scissor.x0, ctx.scissor.x1);
        by0 = std::clamp<int>(by0, ctx.scissor.y0, ctx.scissor.y1);
        by1 = std::clamp<int>(by1, ctx.scissor.y0, ctx.scissor.y1);
        if (by1 < by0)
            return;
        const float denom = (fy1 - fy2) * (fx0 - fx2) + (fx2 - fx1) * (fy0 - fy2);
        if (std::fabs(denom) < 0.001f)
            return;
        const double X0 = fx0, Y0 = fy0, X1 = fx1, Y1 = fy1, X2 = fx2, Y2 = fy2;
        const double wind = (denom < 0.0f) ? -1.0 : 1.0;
        const double A0 = (Y1 - Y2) * wind, B0 = (X2 - X1) * wind, C0 = -(A0 * X2 + B0 * Y2);
        const double A1 = (Y2 - Y0) * wind, B1 = (X0 - X2) * wind, C1 = -(A1 * X2 + B1 * Y2);
        const double A2 = (Y0 - Y1) * wind, B2 = (X1 - X0) * wind, C2 = -(A2 * X0 + B2 * Y0);
        auto ownsEdge = [](double a, double b) { return a > 0.0 || (a == 0.0 && b > 0.0); };
        const double area = A0 * X0 + B0 * Y0 + C0;
        if (!(area > 0.0))
            return;
        prim.edgeC[0] = C0; prim.edgeC[1] = C1; prim.edgeC[2] = C2; prim.edgeC[3] = 1.0 / area;
        prim.edgeA[0] = static_cast<float>(A0); prim.edgeA[1] = static_cast<float>(A1); prim.edgeA[2] = static_cast<float>(A2);
        prim.edgeB[0] = static_cast<float>(B0); prim.edgeB[1] = static_cast<float>(B1); prim.edgeB[2] = static_cast<float>(B2);
        prim.zv[0] = v0.z; prim.zv[1] = v1.z; prim.zv[2] = v2.z;
        const GSVertex *vs[3] = {&v0, &v1, &v2};
        for (int i = 0; i < 3; ++i)
        {
            prim.rgba[i][0] = vs[i]->r; prim.rgba[i][1] = vs[i]->g; prim.rgba[i][2] = vs[i]->b; prim.rgba[i][3] = vs[i]->a;
            prim.stqf[i][0] = vs[i]->s; prim.stqf[i][1] = vs[i]->t; prim.stqf[i][2] = vs[i]->q; prim.stqf[i][3] = vs[i]->fog;
        }
        prim.uvA[0] = v0.u; prim.uvA[1] = v0.v; prim.uvA[2] = v1.u; prim.uvA[3] = v1.v;
        prim.uvB[0] = v2.u; prim.uvB[1] = v2.v;
        // Float copies for the internal-resolution coverage test (the shader's float path).
        prim.edgeA[3] = static_cast<float>(C0); prim.edgeB[3] = static_cast<float>(C1);
        prim.uvB[2] = static_cast<float>(C2); prim.uvB[3] = static_cast<float>(1.0 / area);
        prim.extra[0] = 0;
        prim.extra[1] = (ownsEdge(A0, B0) ? 1 : 0) | (ownsEdge(A1, B1) ? 2 : 0) | (ownsEdge(A2, B2) ? 4 : 0);
#if defined(PS2X_GAME_GT4) && !defined(GT4_STOCK_BUILD)
        // Lighting step 2: a tapped car-body triangle lit per pixel (the shader's GS_LIT).
        if (carLightingPerPixel())
            prim.extra[2] = im.litTriangle(v0, v1, v2);
        // Lighting step 3: the reflection layer's triangles for the frame's screen-space reflections.
        if (Impl::ssrMode() != 0 && v0.tapSlot && v1.tapSlot && v2.tapSlot)
            im.ssrNote(batch);
#endif
    }
    else
    {
        const GSVertex &v0 = batch.vertices[0], &v1 = batch.vertices[1];
        // Progressive: OFY's fraction dropped, as for triangles. GT3 draws its
        // odd fields with OFY half a line higher (interlace placement); the
        // triangles (backgrounds, logos, the race) already ignore it, so the
        // sprites (menu text) alone moved - "the text moves up and down"
        // (2026-09-30: help text 4 output rows at 4x, the logo still).
        const float fofx = static_cast<float>(ctx.xyoffset.ofx) / 16.0f;
        // GT3_GS_GL_SPRITE_OFY=1 (diagnostic): the fraction kept (as before).
        static const bool spriteOfyFraction = envFlag("GT3_GS_GL_SPRITE_OFY", false);
        const float fofy = im.progressive && !spriteOfyFraction ? static_cast<float>(ctx.xyoffset.ofy >> 4)
                                                                : static_cast<float>(ctx.xyoffset.ofy) / 16.0f;
        const float X0 = v0.x - fofx, Y0 = v0.y - fofy, X1 = v1.x - fofx, Y1 = v1.y - fofy;
        const int pixelX0 = ceilToInt(std::min(X0, X1));
        const int pixelX1 = ceilToInt(std::max(X0, X1)) - 1;
        const int pixelY0 = ceilToInt(std::min(Y0, Y1));
        const int pixelY1 = ceilToInt(std::max(Y0, Y1)) - 1;
        if (pixelX1 < pixelX0 || pixelY1 < pixelY0)
            return;
        if (pixelX1 < ctx.scissor.x0 || pixelX0 > ctx.scissor.x1 || pixelY1 < ctx.scissor.y0 || pixelY0 > ctx.scissor.y1)
            return;
        bx0 = std::max<int>(pixelX0, ctx.scissor.x0);
        by0 = std::max<int>(pixelY0, ctx.scissor.y0);
        bx1 = std::min<int>(pixelX1, ctx.scissor.x1);
        by1 = std::min<int>(pixelY1, ctx.scissor.y1);
        prim.rgba[0][0] = v1.r; prim.rgba[0][1] = v1.g; prim.rgba[0][2] = v1.b; prim.rgba[0][3] = v1.a;
        prim.zv[3] = static_cast<double>(static_cast<uint32_t>(v1.z));
        prim.extra[0] = 1;
        prim.extra[2] = v1.fog;
        if (state.prim.tme)
        {
            const int texW = state.textureWidth, texH = state.textureHeight;
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
                const float q0 = (std::fabs(v0.q) > 1.0e-8f) ? v0.q : 1.0f;
                const float q1 = (std::fabs(v1.q) > 1.0e-8f) ? v1.q : 1.0f;
                u0f = (v0.s / q0) * static_cast<float>(texW);
                v0f = (v0.t / q0) * static_cast<float>(texH);
                u1f = (v1.s / q1) * static_cast<float>(texW);
                v1f = (v1.t / q1) * static_cast<float>(texH);
            }
            prim.uvA[0] = u0f;
            prim.uvA[1] = v0f;
            prim.uvA[2] = (X1 != X0) ? (u1f - u0f) / (X1 - X0) : 0.0f;
            prim.uvA[3] = (Y1 != Y0) ? (v1f - v0f) / (Y1 - Y0) : 0.0f;
            prim.uvB[0] = X0;
            prim.uvB[1] = Y0;
        }
    }
    if (bx1 < bx0 || by1 < by0)
        return;
    // GT3_GS_GL_SKIP_FB=<hex fbp>/<fbw> (diagnostic, pictures change): draws
    // into that frame buffer and width are dropped.
    static const std::pair<int, int> skipFb = [] {
        std::pair<int, int> r{-1, -1};
        if (const char *v = std::getenv("GT3_GS_GL_SKIP_FB"))
            std::sscanf(v, "%x/%d", &r.first, &r.second);
        return r;
    }();
    if (skipFb.first >= 0 && static_cast<int>(ctx.frame.fbp) == skipFb.first && static_cast<int>(ctx.frame.fbw) == skipFb.second)
        return;
    prim.bbox[0] = bx0; prim.bbox[1] = by0; prim.bbox[2] = bx1; prim.bbox[3] = by1;

    // Step 7 T1: drawn with the memo's draw state if its targets need not grow
    // (PS2X_GS_GL_RUN_VERIFY: then resolved in full below and compared).
    if (memoHit || memoCheck)
    {
        if (im.memoTargetsFit(bx1, by1))
        {
            if (memoHit)
            {
                im.drawFromMemo(prim, ctx, sprite, bx0, by0, bx1, by1);
                return;
            }
            im.beginMemoCheck();
        }
        else
        {
            if (memoHit && state.prim.tme)
                footprint(); // (not computed for a memo hit)
            memoHit = memoCheck = false; // a target grows: resolved in full
            im.memo.valid = false;
            ++im.statMemoTargetMiss;
        }
    }

    // ---- draw state
    DrawKey k{};
    const uint32_t fpsm = ctx.frame.psm;
    const Layout frameLayout = fpsm == GS_PSM_CT16 ? Layout::C16 : ((fpsm == GS_PSM_Z24 || fpsm == GS_PSM_Z32) ? Layout::Z32 : Layout::C32);
    // GT4's car view at full height in its 480p menus (gs_car_view.h; CAR-PICTURE.md section 8): a 32-bit draw with
    // the scissor of a 480i field shape target - the car target and composite (640 x 224: 0..639 x 0..223, GT Mode's
    // 0..222; FBW 10), the shadow planes (320 x 224, FBW 5) and the background blur planes (320 x 112, FBW 5) - draws
    // into a tall target (scaleX x 2 scaleY), with a depth buffer of the same scale. Only while armed (the menus
    // are 480p, no race session); GT4's 2D menus there are 16-bit and 448 rows.
    bool tall = false;
    if (tallArmed && (fpsm == GS_PSM_CT32 || fpsm == GS_PSM_CT24) && ctx.scissor.x0 == 0 && ctx.scissor.y0 == 0)
    {
        const uint32_t sx1 = ctx.scissor.x1, sy1 = ctx.scissor.y1;
        // GT4_MENU_480P_CAR_TALL_PLANES (development): bit 0 the shadow planes tall too, bit 1 the blur planes.
        static const int tallPlanes = [] {
            const char *v = std::getenv("GT4_MENU_480P_CAR_TALL_PLANES");
            return v && *v ? std::atoi(v) : 3;
        }();
        tall = (ctx.frame.fbw == 10u && sx1 == 639u && (sy1 == 223u || sy1 == 222u)) ||
               (ctx.frame.fbw == 5u && sx1 == 319u && ((sy1 == 223u && (tallPlanes & 1)) || (sy1 == 111u && (tallPlanes & 2))));
    }
    k.colorTarget = im.findTarget(false, frameLayout, ctx.frame.fbp, ctx.frame.fbw, by1 + 1, tall);
    if (k.colorTarget < 0 || bx1 >= im.targets[k.colorTarget]->width)
    {
        drawOnCpu("outside the target");
        return;
    }
    // (the shader's u_zWrite: 2 clamps Z to 16 bits; a tall car-view draw keeps GT4's deep Z unclamped: gs_car_view.h)
    k.zte = zte ? (zbuf16 && !(tall && gt4carview::deepZAllowed()) ? 2 : 1) : 0;
    k.ztst = static_cast<int32_t>(ztst);
    k.zmask = ctx.zbuf.zmask ? 1 : 0;
    if (zte)
    {
        k.depthTarget = im.findTarget(true, zbuf16 ? Layout::Z16 : Layout::Z32, ctx.zbuf.zbp, ctx.frame.fbw, by1 + 1, tall);
        if (k.depthTarget < 0)
        {
            drawOnCpu("Z outside the target");
            return;
        }
        // Effects at their own scale: a target drawn with a depth buffer (zte: tested or written) is a scene target.
        if (im.effectScale && !im.dlssHudRender && im.targets[k.colorTarget]->sx != im.scaleX)
            im.promoteTarget(k.colorTarget, "depth");
    }
    k.tme = state.prim.tme ? 1 : 0;
    k.fst = state.prim.fst ? 1 : 0;
    k.iip = state.prim.iip ? 1 : 0;
    k.fge = state.prim.fge ? 1 : 0;
    k.fogR = state.fogR; k.fogG = state.fogG; k.fogB = state.fogB;
    k.ate = (ctx.test & 1u) ? 1 : 0;
    k.atst = static_cast<int32_t>((ctx.test >> 1) & 7u);
    k.aref = static_cast<int32_t>((ctx.test >> 4) & 0xFFu);
    k.afail = static_cast<int32_t>((ctx.test >> 12) & 3u);
    // Frame formats whose alpha byte (the high plane) the GS leaves alone.
    const bool frameCt24 = fpsm == GS_PSM_CT24 || fpsm == GS_PSM_Z24;
    k.frameFormat = fpsm == GS_PSM_CT24 ? 1 : (fpsm == GS_PSM_CT16 ? 2 : (fpsm == GS_PSM_Z24 ? 3 : (fpsm == GS_PSM_Z32 ? 4 : 0)));
    k.date = (((ctx.test >> 14) & 1u) != 0u && (fpsm == GS_PSM_CT32 || fpsm == GS_PSM_CT16)) ? 1 : 0;
    k.datm = ((ctx.test >> 15) & 1u) ? 1 : 0;
    k.abe = state.prim.abe ? 1 : 0;
    k.pabe = state.pabe ? 1 : 0;
    k.ba = static_cast<int32_t>(ctx.alpha & 3u);
    k.bb = static_cast<int32_t>((ctx.alpha >> 2) & 3u);
    k.bc = static_cast<int32_t>((ctx.alpha >> 4) & 3u);
    k.bd = static_cast<int32_t>((ctx.alpha >> 6) & 3u);
    k.fix = static_cast<int32_t>((ctx.alpha >> 32) & 0xFFu);
    k.fba = ((ctx.fba & 1u) != 0u && fpsm != GS_PSM_CT24) ? 1 : 0;
    // The frame's memory format (frameFormat as GS memory lays it out; whole-target overwrites test it).
    const int memoryFormat = k.frameFormat;
    // GT4's race colour (gs_fullcolour.h): a full-colour draw into a 16-bit (C16) target is the CT32 shader
    // path (frameFormat 0: 8-bit destination colour and alpha, DATE on alpha bit 7, no 5-bit mask, RGB_ONLY
    // keeping alpha); the texels keep the full colour. FBA and the alpha write stay the memory format's
    // (CT16); never dithered (gsdither::applies).
    if (state.fc != 0u && fpsm == GS_PSM_CT16)
    {
        k.frameFormat = 0;
        k.date = ((ctx.test >> 14) & 1u) != 0u ? 1 : 0;
    }
    if (dither)
    {
        k.dither = 1;
        k.dimxLo = static_cast<int32_t>(static_cast<uint32_t>(state.dimx));
        k.dimxHi = static_cast<int32_t>(static_cast<uint32_t>(state.dimx >> 32));
    }
    const uint32_t fbmsk = ctx.frame.fbmsk;
    bool byteMask = true;
    int colorMask = 0;
    for (int i = 0; i < 4; ++i)
    {
        const uint32_t byte = (fbmsk >> (i * 8)) & 0xFFu;
        if (byte != 0u && byte != 0xFFu)
            byteMask = false;
        if (byte == 0u)
            colorMask |= 1 << i;
    }
    if (frameCt24)
        colorMask &= 0x7;
    k.dstRead = (k.abe || k.date || !byteMask) ? 1 : 0;
    // GT4's race colour (gs_fullcolour.h): a 16-bit-rule draw into a 16-bit target while texels may hold a full
    // colour (a full-colour draw came since the last video mode change) leaves every pixel it writes as its
    // 16-bit value, the bytes FBMSK keeps included, as the CPU reference renderer does: a colour write mask would
    // keep those bytes' full colour, so the target is read (the shader masks it) and written whole.
    if (k.frameFormat == 2 && im.fcSeen && colorMask != 0xF)
        k.dstRead = 1;
    // Fast mode (3x/4x): blending the GPU's blend unit can do, (A-B)*C+D as
    // Cs*ks + Cd*kd with dual-source factors, needs no read of the target
    // and no overlap levels. Not exact (float blending, factors clamped to
    // 1): never at native. Not with C = destination alpha, DATE, bit masks,
    // CT16 frames, or a factor on Cd above 1 or both negative.
    // GT3_GS_GL_HW_BLEND=0: fast mode blends as exact mode does (diagnostic);
    // =2: not inside an inset view (GT3's rear-view mirror: depth-tested, a
    // scissor narrower than the target around its middle).
    static const int hwBlendMode = [] {
        const char *v = std::getenv("GT3_GS_GL_HW_BLEND");
        return v && *v ? std::atoi(v) : 1;
    }();
    const bool insetScissor = ((ctx.test >> 16) & 1u) != 0u && ((ctx.test >> 17) & 3u) >= 2u && ctx.scissor.x0 > 0u &&
                              ctx.scissor.x1 + 1u < ctx.frame.fbw * 64u && ctx.scissor.x0 < ctx.frame.fbw * 32u &&
                              ctx.scissor.x1 >= ctx.frame.fbw * 32u;
    const bool hwBlendOn = hwBlendMode == 1 || (hwBlendMode == 2 && !insetScissor);
    if (im.fastBlend && hwBlendOn && k.abe && !k.date && byteMask && fpsm != GS_PSM_CT16 && k.bc != 1 && k.ba != 3 && k.bb != 3 && k.bd != 3)
    {
        const int cs = (k.ba == 0 ? 1 : 0) - (k.bb == 0 ? 1 : 0), ds = k.bd == 0 ? 1 : 0;
        const int cd = (k.ba == 1 ? 1 : 0) - (k.bb == 1 ? 1 : 0), dd = k.bd == 1 ? 1 : 0;
        const bool srcNeg = ds == 0 && cs < 0, dstNeg = dd == 0 && cd < 0;
        const bool dstAboveOne = dd == 1 && cd > 0;
        // GT3_GS_GL_HW_BLEND_ALPHA_EXACT=1 (experiment): a (1 - As) factor
        // with an untextured alpha above 0x80 (below 0: exact subtracts and
        // clamps) blends exactly. Off: the shader clamps black sources only
        // (GT3's loading cover), which is exact.
        static const bool alphaExact = envFlag("GT3_GS_GL_HW_BLEND_ALPHA_EXACT", false);
        bool alphaAboveOne = false;
        if (alphaExact && k.bc == 0 && !state.prim.tme && ((dd == 1 && cd < 0) || (ds == 1 && cs < 0)))
            for (uint32_t i = 0; i < batch.vertexCount; ++i)
                alphaAboveOne = alphaAboveOne || batch.vertices[i].a > 0x80u;
#if defined(PS2X_GAME_GT4)
        // GT4 (the Full launcher's internal resolution, analysis/design/full-m2 README, Resolution): factors the blend
        // unit cannot give blend exactly (the target read), as at native. Dual-source factors are clamped to 0..1 on
        // GT4's 8-bit targets, so Cd * FIX with FIX above 0x80 came out as Cd: GT4 brightens its whole 3D race picture
        // every frame with (Cd - 0) * 0xFF + 0 (ALPHA 0xff000000a9 on its main target), which at 3x and 4x left the
        // scene about half as bright. With C = FIX the factors are known: Cd's above 1, a negative factor the equation
        // would add (the shader takes its size), or Cs's above 1 against a subtracted Cd blends exactly. With C = As
        // (per pixel, up to 0xFF / 0x80) a positive Cd term (Cd's factor As / 128) blends exactly unless the batch is
        // untextured with every vertex alpha at most 0x80. (GT3 builds: unchanged.)
        // Review (full-m2 README, Resolution, review): an untextured batch with a vertex alpha above 0x80 also blends
        // exactly when As makes any other factor leave 0..1 (Cd's 1 - As or -As, Cs's 1 - As): on the night course GT4
        // draws untextured (Cs - Cd) * As + Cd batches with vertex alpha up to 0xC1 into small targets, and the blend
        // unit added Cd * (As - 1) where the GS subtracts it; it showed on the cars' bodies and windows (a temporary
        // shader marker found 2,818 such pixels in 38 shots at 3x; none with this rule).
        if (k.bc == 2)
        {
            const double c = static_cast<double>(k.fix) / 128.0;
            const double ksv = cs * c + ds, kdv = cd * c + dd;
            alphaAboveOne = alphaAboveOne || kdv > 1.0 || (kdv < 0.0 && !dstNeg) || (ksv < 0.0 && !srcNeg) || (ksv > 1.0 && kdv < 0.0);
        }
        else if (k.bc == 0 && (cd != 0 || (cs < 0 && ds == 1)))
        {
            bool above = state.prim.tme && cd > 0;
            for (uint32_t i = 0; !above && !state.prim.tme && i < batch.vertexCount; ++i)
                above = batch.vertices[i].a > 0x80u;
            alphaAboveOne = alphaAboveOne || above;
        }
#endif
        if (!(srcNeg && dstNeg) && !dstAboveOne && !(k.pabe && (srcNeg || dstNeg)) && !alphaAboveOne)
        {
            k.hwBlend = srcNeg ? 3 : (dstNeg ? 2 : 1); // add, subtract, reverse subtract
            k.dstRead = 0;
            // GT3_GS_GL_HW_BLEND_LOG=<n>: the first n distinct hardware-blended
            // batches (diagnostic).
            static long hwBlendLog = [] {
                const char *v = std::getenv("GT3_GS_GL_HW_BLEND_LOG");
                return v ? std::strtol(v, nullptr, 10) : 0L;
            }();
            static const unsigned long long hwBlendFrom = [] {
                const char *v = std::getenv("GT3_GS_GL_HW_BLEND_FROM");
                return v ? std::strtoull(v, nullptr, 10) : 0ull;
            }();
            // GT3_GS_GL_HW_BLEND_RECT=x0,y0,x1,y1: only batches whose box meets it.
            static const std::array<int, 4> hwBlendRect = [] {
                std::array<int, 4> r{-100000, -100000, 100000, 100000};
                if (const char *v = std::getenv("GT3_GS_GL_HW_BLEND_RECT"))
                    std::sscanf(v, "%d,%d,%d,%d", &r[0], &r[1], &r[2], &r[3]);
                return r;
            }();
            if (hwBlendLog > 0 && im.frameCounter >= hwBlendFrom && bx1 >= hwBlendRect[0] && bx0 <= hwBlendRect[2] && by1 >= hwBlendRect[1] &&
                by0 <= hwBlendRect[3])
            {
                static std::unordered_map<std::string, int> seen;
                char text[300];
                std::snprintf(text, sizeof(text), "fbp %03x/%u psm %02x prim %d tme %d tex %04x/%u psm %02x %dx%d alpha %llx linear %d box %d,%d-%d,%d",
                              ctx.frame.fbp, ctx.frame.fbw, ctx.frame.psm, static_cast<int>(state.prim.type), state.prim.tme ? 1 : 0,
                              ctx.tex0.tbp0, ctx.tex0.tbw, ctx.tex0.psm, state.textureWidth, state.textureHeight,
                              static_cast<unsigned long long>(ctx.alpha), state.linearFilter ? 1 : 0, bx0 / 16 * 16, by0 / 16 * 16,
                              bx1 / 16 * 16, by1 / 16 * 16);
                if (seen[text]++ == 0)
                {
                    --hwBlendLog;
                    std::fprintf(stderr, "[hw-blend] frame %llu %s\n", static_cast<unsigned long long>(im.frameCounter), text);
                }
            }
        }
    }
    // ... and Cs * Ad + Cd (A Cs, B 0, C Ad, D Cd: GT3's car reflections on the
    // Arcade car select, ~4.6 M primitives per 10 s that otherwise go through
    // overlap levels: 51-56 fps at 4x on the laptop, user 2026-10-03 "it just
    // stayed in the low 50s"): the blend unit with the destination's alpha as
    // Cs's factor, drawn twice (the target keeps alpha / 255, the GS scales it
    // by 1 / 128), alpha written by the second draw only (hwBlend 4). Not exact
    // where the batch's own primitives overlap (both draws read the alpha the
    // batch found); no depth writes, no alpha-test fail action, CT32 only.
    // GT3_GS_GL_HW_BLEND_DST_ALPHA=0: overlap levels as before.
    static const bool hwDstAlpha = envFlag("GT3_GS_GL_HW_BLEND_DST_ALPHA", true);
    if (im.fastBlend && hwBlendOn && hwDstAlpha && k.hwBlend == 0 && k.abe && !k.pabe && !k.date && byteMask && fpsm == GS_PSM_CT32 && k.ba == 0 &&
        k.bb == 2 && k.bc == 1 && k.bd == 1 && !(k.ate && k.atst != 1 && k.afail != 0) && !(k.zte && !k.zmask))
    {
        k.hwBlend = 4;
        k.dstRead = 0;
    }
    if (k.dstRead)
    {
        k.fbmsk = fbmsk;
        colorMask = frameCt24 ? 0x7 : 0xF;
    }
    k.colorMask = colorMask;
    const bool twoPass = k.ate && k.atst != 1 && k.afail != 0;
    k.needsSplit = (k.dstRead || twoPass) ? 1 : 0;

    // ---- texture
    const int texW = state.textureWidth, texH = state.textureHeight;
    if (k.tme)
    {
        k.tfx = tex.tfx;
        k.tcc = tex.tcc;
        k.texW = texW;
        k.texH = texH;
        k.linear = state.linearFilter ? 1 : 0;
        k.wrapU = static_cast<int32_t>(ctx.clamp & 3u);
        k.wrapV = static_cast<int32_t>((ctx.clamp >> 2) & 3u);
        k.minU = static_cast<int32_t>((ctx.clamp >> 4) & 0x3FFu);
        k.maxU = static_cast<int32_t>((ctx.clamp >> 14) & 0x3FFu);
        k.minV = static_cast<int32_t>((ctx.clamp >> 24) & 0x3FFu);
        k.maxV = static_cast<int32_t>((ctx.clamp >> 34) & 0x3FFu);
        k.ta0 = state.texa.ta0; k.aem = state.texa.aem ? 1 : 0; k.ta1 = state.texa.ta1;
    }
    // GS mipmapping, a per-pixel primitive: raw memory over all its levels
    // (never a target view, decoded or colour texture).
    if (k.tme && mipPerPixel)
    {
        if (const char *why = im.mipRawSource(k, state, fu0, fv0, fu1, fv1, texLo, texHi, texKnown))
        {
            drawOnCpu(why);
            return;
        }
        ++im.statMipPrims;
    }
    // PS2X_GS_MIP_TINT: a primitive resolved to level n >= 1 is tinted (shader
    // sampleTextureMipmapped), as the CPU renderer tints it.
    if (k.tme && !mipPerPixel && state.mipTag > 1u && state.mipTag < gsmip::kTagPerPixel && gsmip::tintGl())
        k.mipCtl = 0x800 | ((static_cast<int32_t>(state.mipTag) - 1) << 12);
    if (k.tme && !mipPerPixel)
    {
        // A texture whose sampled pages a GPU target owns is read from that
        // target in place, page by page (any page-aligned view of it: another
        // width, a single tile, or its PSMT8/T4 and alpha-plane views).
        std::vector<uint32_t> &footprintPages = im.footprintScratch;
        footprintPages.clear();
        forRectPages(tex.tbp0 >> 5u, (tex.tbp0 & 31u) != 0u, tex.tbw, tex.psm, fu0, fv0, fu1, fv1,
                     [&](uint32_t page) { footprintPages.push_back(page); });
        int sourceTarget = -1;
        bool anyGpuPage = false;
        for (const uint32_t page : footprintPages)
            if (im.owner[page] >= 0)
            {
                anyGpuPage = true;
                sourceTarget = im.owner[page];
            }
#if defined(PS2X_GAME_GT4) && !defined(GT4_STOCK_BUILD)
        if (anyGpuPage && im.texSmoothForced)
        {
            k.linear = 0; // texture smoothing: a texture GT4 drew (a target) keeps GT4's nearest sampling
            ++im.statSmoothTarget;
        }
#endif
        bool fromTarget = false;
        const uint32_t tpsm = tex.psm;
        const bool texC32 = isColor32(tpsm), texC16 = tpsm == GS_PSM_CT16, texZ = tpsm == GS_PSM_Z24 || tpsm == GS_PSM_Z32;
        const bool texAlphaPlane = tpsm == GS_PSM_T8H || tpsm == GS_PSM_T4HL || tpsm == GS_PSM_T4HH;
        const bool texIndexed = tpsm == GS_PSM_T8 || tpsm == GS_PSM_T4;
        // GT4's race colour (GSDrawState::ftex, gs_fullcolour.h texFlag): a display buffer read back as a CT16
        // texture is read from its 16-bit target, whose texels hold each pixel's full colour (pages the CPU copy
        // holds are loaded into it with their full colour, uploadPage), never from raw memory or a decode.
        const bool texFull = state.ftex != 0u && texC16;
        if ((anyGpuPage || texFull) && (texC32 || texC16 || texZ || texAlphaPlane || texIndexed) && (tex.tbp0 & 31u) == 0u)
        {
            const Layout texLayout = texC16 ? Layout::C16 : (texZ ? Layout::Z32 : Layout::C32);
            auto compatible = [&](const Target &t) {
                if (t.layout != texLayout)
                    return false;
                if (t.depth)
                    return texZ; // a Z buffer is read only as Z
                return true;
            };
            // Which target to read: the compatible owner of the most sampled
            // pages whose rows can hold them all; else the buffer based at the
            // texture's own page. Pages another target owns are moved over.
            auto rowsNeeded = [&](const Target &t) {
                int last = -1;
                for (const uint32_t page : footprintPages)
                    last = std::max(last, static_cast<int>(((page - t.fbp) & (kPageCount - 1u)) / t.fbw));
                return (last + 1) * t.pageHeight;
            };
            int best = -1, bestCount = 0;
            for (const uint32_t page : footprintPages)
            {
                const int id = im.owner[page];
                if (id < 0 || id == best || !compatible(*im.targets[id]) || (im.targets[id]->depth && id == k.depthTarget))
                    continue;
                if (rowsNeeded(*im.targets[id]) > im.targets[id]->maxHeight())
                    continue;
                int count = 0;
                for (const uint32_t other : footprintPages)
                    count += im.owner[other] == id ? 1 : 0;
                if (count > bestCount)
                {
                    best = id;
                    bestCount = count;
                }
            }
            if (best < 0 && !texZ)
            {
                const int own = im.findTarget(false, texLayout, tex.tbp0 >> 5u, std::max<uint32_t>(tex.tbw, 1u), 0);
                if (own >= 0 && own != k.depthTarget && rowsNeeded(*im.targets[own]) <= im.targets[own]->maxHeight())
                    best = own;
            }
            if (best >= 0)
                sourceTarget = best;
            // (A full-colour read with no GPU page and no target for it: sourceTarget is still -1.)
            if (best >= 0 && compatible(*im.targets[best]) && !(im.targets[best]->depth && best == k.depthTarget))
            {
                const Target &chosen = *im.targets[best];
                // Rows of the target the sampled pages fall in.
                int firstRow = INT32_MAX, lastRow = -1;
                for (const uint32_t page : footprintPages)
                {
                    const int row = static_cast<int>(((page - chosen.fbp) & (kPageCount - 1u)) / chosen.fbw);
                    firstRow = std::min(firstRow, row);
                    lastRow = std::max(lastRow, row);
                }
                const int pageH = chosen.pageHeight;
                const int src = im.findTarget(chosen.depth, chosen.layout, chosen.fbp, chosen.fbw, (lastRow + 1) * pageH, chosen.tall);
                if (src >= 0)
                {
                    fromTarget = true;
                    Target &srcTarget = *im.targets[src];
                    if (im.stats)
                        im.cause = "texture view";
                    const uint64_t movesBefore = im.statGpuMoves, backBefore = im.statMoveBack;
                    im.acquirePageList(src, footprintPages);
                    if (im.stats && im.statGpuMoves != movesBefore)
                    {
                        // Which texture reads move pages into their view (GT3_GS_GL_STATS): moves (of them back without a copy) by
                        // texture, view target and frame.
                        char text[200];
                        std::snprintf(text, sizeof(text), "tex %04x/%u psm %02x %ux%u footprint %zu pages -> %s %03x/%u; frame %03x/%u psm %02x %s",
                                      tex.tbp0, tex.tbw, static_cast<unsigned>(tpsm), static_cast<unsigned>(state.textureWidth),
                                      static_cast<unsigned>(state.textureHeight), footprintPages.size(), im.layoutName(srcTarget), srcTarget.fbp,
                                      srcTarget.fbw, ctx.frame.fbp, ctx.frame.fbw, static_cast<unsigned>(ctx.frame.psm), sprite ? "sprite" : "tri");
                        auto &e = im.statViewMoves[text];
                        e.first += im.statGpuMoves - movesBefore;
                        e.second += im.statMoveBack - backBefore;
                    }
                    // GT3_GS_GL_VIEWMOVE_LOG=<n> (diagnostic): the first n texture reads that moved 32 or more pages into
                    // their view, with their texel range and sampling.
                    static long viewMoveLog = [] {
                        const char *v = std::getenv("GT3_GS_GL_VIEWMOVE_LOG");
                        return v ? std::strtol(v, nullptr, 10) : 0L;
                    }();
                    if (viewMoveLog > 0 && im.statGpuMoves - movesBefore >= 32u)
                    {
                        --viewMoveLog;
                        std::fprintf(stderr,
                                     "[gs-viewmove] frame %llu %llu moves: tex %04x/%u psm %02x %dx%d fst %d linear %d wrap %d/%d region %d-%d,%d-%d "
                                     "texels %d-%d,%d-%d (read %d-%d,%d-%d known %d) footprint %zu pages -> %s %03x/%u; frame %03x/%u psm %02x %s box "
                                     "%d,%d-%d,%d uv %.3f,%.3f-%.3f,%.3f\n",
                                     (unsigned long long)im.frameCounter, (unsigned long long)(im.statGpuMoves - movesBefore), tex.tbp0, tex.tbw,
                                     static_cast<unsigned>(tpsm), texW, texH, k.fst, k.linear, wrapU, wrapV, minU, maxU, minV, maxV, fu0, fu1, fv0, fv1,
                                     ru0, ru1, rv0, rv1, readKnown ? 1 : 0, footprintPages.size(), im.layoutName(srcTarget), srcTarget.fbp, srcTarget.fbw,
                                     ctx.frame.fbp, ctx.frame.fbw, static_cast<unsigned>(ctx.frame.psm), sprite ? "sprite" : "tri", bx0, by0, bx1, by1,
                                     texLo[0], texLo[1], texHi[0], texHi[1]);
                    }
                    uint32_t texPageW = 64u, texPageH = 32u;
                    pageGeometry(tpsm, texPageW, texPageH);
                    k.texBasePage = static_cast<int32_t>(tex.tbp0 >> 5u);
                    k.texPagesPerRow = static_cast<int32_t>(std::max(1u, (std::max<uint32_t>(tex.tbw, 1u) * 64u) / texPageW));
                    k.texPageW = static_cast<int32_t>(texPageW);
                    k.texPageH = static_cast<int32_t>(texPageH);
                    k.texTargetFbp = static_cast<int32_t>(srcTarget.fbp);
                    k.texTargetFbw = static_cast<int32_t>(srcTarget.fbw);
                    k.texTargetPageH = pageH;
                    if (srcTarget.depth)
                        k.texMode = 4;
                    else if (texZ)
                        k.texMode = tpsm == GS_PSM_Z24 ? 5 : 6;
                    else if (texC16)
                    {
                        k.texMode = 3;
                        k.texFull = texFull ? 1 : 0;
                    }
                    else if (texAlphaPlane || texIndexed)
                    {
                        k.texMode = tpsm == GS_PSM_T8H ? 7 : tpsm == GS_PSM_T4HL ? 8 : tpsm == GS_PSM_T4HH ? 9 : tpsm == GS_PSM_T8 ? 10 : 11;
                        im.choosePalette(k, state);
                    }
                    else
                        k.texMode = tpsm == GS_PSM_CT24 ? 2 : 1;
                    // Fast mode: a colour view whose pages lie in the target as one
                    // rectangle (same width in pages, same page size) is read by the
                    // GPU's bilinear filter at that rectangle's offset.
                    if ((im.fastTargetsActive || (im.tallFastReads && srcTarget.tall)) && k.linear && k.texMode >= 1 && k.texMode <= 3 && wrapU != 3 && wrapV != 3 &&
                        k.texPagesPerRow == k.texTargetFbw && k.texPageW == 64 && k.texPageH == k.texTargetPageH)
                    {
                        const int r = (k.texBasePage - k.texTargetFbp) & static_cast<int>(kPageCount - 1u);
                        const int row = r / std::max(1, k.texTargetFbw);
                        k.fastTarget = 1;
                        k.texOffX = (r - row * k.texTargetFbw) * 64;
                        k.texOffY = row * k.texTargetPageH;
                    }
                    // GT3_GS_GL_VIEW_LOG=<n>: the first n distinct target reads (format, geometry).
                    static long viewLog = [] {
                        const char *v = std::getenv("GT3_GS_GL_VIEW_LOG");
                        return v ? std::strtol(v, nullptr, 10) : 0L;
                    }();
                    if (viewLog > 0)
                    {
                        static std::unordered_map<std::string, int> seen;
                        char text[400];
                        std::snprintf(text, sizeof(text),
                                      "texMode %d tpsm %02x tbp %04x tbw %u tw %d th %d -> %s %03x/%u; draw fbp %03x/%u psm %02x fbmsk %08x %s du %.3f dv %.3f "
                                      "linear %d",
                                      k.texMode, tpsm, tex.tbp0, tex.tbw, state.textureWidth, state.textureHeight, im.layoutName(srcTarget),
                                      srcTarget.fbp, srcTarget.fbw, ctx.frame.fbp, ctx.frame.fbw, ctx.frame.psm, ctx.frame.fbmsk,
                                      sprite ? "sprite" : "tri", sprite ? prim.uvA[2] : 0.0f, sprite ? prim.uvA[3] : 0.0f, k.linear);
                        if (seen[text]++ == 0)
                        {
                            --viewLog;
                            std::fprintf(stderr, "[gs-view] frame %llu %s box %d,%d-%d,%d u0 %.2f v0 %.2f\n", (unsigned long long)im.frameCounter,
                                         text, bx0, by0, bx1, by1, prim.uvA[0], prim.uvA[1]);
                        }
                    }
                    if (src == k.colorTarget)
                    {
                        // Reads what it draws: sample a copy of those rows taken before the draw.
                        static long selfLog = [] {
                            const char *v = std::getenv("GT3_GS_GL_SELF_LOG");
                            return v ? std::strtol(v, nullptr, 10) : 0L;
                        }();
                        if (selfLog > 0)
                        {
                            --selfLog;
                            std::fprintf(stderr, "[self] frame %llu tbp %04x psm %02x texMode %d rows %d-%d footprint %d,%d-%d,%d box %d,%d-%d,%d %s du %.3f dv %.3f u0 %.2f v0 %.2f linear %d dst %d\n",
                                         (unsigned long long)im.frameCounter, tex.tbp0, tpsm, k.texMode, firstRow, lastRow, fu0, fv0, fu1, fv1, bx0, by0,
                                         bx1, by1, sprite ? "sprite" : "tri", sprite ? prim.uvA[2] : 0.0f, sprite ? prim.uvA[3] : 0.0f, prim.uvA[0],
                                         prim.uvA[1], k.linear, k.dstRead);
                        }
                        // The copy is made (or the batch's copy reused) below, once
                        // the batch this primitive joins is known.
                        selfRead = true;
                        selfTarget = src;
                        selfFirstRow = firstRow;
                        selfLastRow = lastRow;
                        selfTexelIsPixel = (k.texMode == 1 || k.texMode == 2) && tex.tbp0 == srcTarget.fbp * 32u &&
                                           std::max<uint32_t>(tex.tbw, 1u) == srcTarget.fbw;
                        selfU0 = tfu0;
                        selfV0 = tfv0;
                        selfU1 = tfu1;
                        selfV1 = tfv1;
                        k.texSource = im.texScratch;
                    }
                    else
                    {
                        k.texSource = srcTarget.tex;
                    }
                }
            }
        }
        if (texFull && im.stats)
            ++(fromTarget ? im.statFullTexReads : im.statFullTexMiss);
        if (texFull)
        {
            // GT3_GS_GL_FULLTEX_LOG=<n>: the first n full-colour CT16 texture reads (diagnostic: which draws read GT4's
            // 480p / 1080i display buffers back).
            static long fullTexLog = [] {
                const char *v = std::getenv("GT3_GS_GL_FULLTEX_LOG");
                return v ? std::strtol(v, nullptr, 10) : 0L;
            }();
            if (fullTexLog > 0)
            {
                --fullTexLog;
                std::fprintf(stderr, "[gs-fulltex] frame %llu tex %04x/%u %ux%u%s -> frame %03x/%u psm %02x fbmsk %08x %s box %d,%d-%d,%d linear %d %s\n",
                             (unsigned long long)im.frameCounter, tex.tbp0, tex.tbw, static_cast<unsigned>(state.textureWidth),
                             static_cast<unsigned>(state.textureHeight), fromTarget ? "" : " (no target)", ctx.frame.fbp, ctx.frame.fbw,
                             static_cast<unsigned>(ctx.frame.psm), ctx.frame.fbmsk, sprite ? "sprite" : "tri", bx0, by0, bx1, by1, k.linear,
                             state.prim.abe ? "blended" : "");
            }
        }
        // GT3_GS_GL_NO_DEPTH_TEX_DOWNLOAD=1 (diagnostics, not exact): textures
        // in a depth target's pages read the stale CPU copy instead of
        // downloading it (what those downloads cost).
        static const bool noDepthTexDownload = envFlag("GT3_GS_GL_NO_DEPTH_TEX_DOWNLOAD", false);
        if (!fromTarget && anyGpuPage && noDepthTexDownload && sourceTarget >= 0 && im.targets[sourceTarget] && im.targets[sourceTarget]->depth)
            anyGpuPage = false;
        if (!fromTarget)
        {
            const bool rawFormat = texC32 || tpsm == GS_PSM_CT16 || tpsm == GS_PSM_CT16S || texAlphaPlane || texIndexed;
            int rawFirst = 0, rawCount = 0;
            uint64_t rawSignature = 0;
            // Pages a depth target holds: their raw words made on the GPU
            // (gpuRawPagesTexture) instead of a download.
            GLuint raw = (anyGpuPage && im.hasBindless && im.rawTextures && rawFormat)
                             ? im.gpuRawPagesTexture(tex, decodeW, decodeH, fu0, fv0, fu1, fv1, rawFirst, rawCount, &rawSignature)
                             : 0;
            if (anyGpuPage && !raw)
            {
                if (im.stats)
                {
                    const Target &o = *im.targets[sourceTarget];
                    char text[96];
                    std::snprintf(text, sizeof(text), "texture %04x/%u psm %02x from %s %03x/%u", tex.tbp0, tex.tbw,
                                  static_cast<unsigned>(tex.psm), o.depth ? "Z" : "C", o.fbp, o.fbw);
                    im.cause = text;
                }
                im.syncPagesToCpu(tex.tbp0, tex.tbw, tex.psm, fu0, fv0, fu1, fv1);
            }
            GLuint replacement = 0;
#if !defined(GT3_STOCK_BUILD) && !defined(PS2X_GAME_GT4)
            // GT3_TEXTURE_DUMP / GT3_TEXTURE_REPLACE: textures from GS memory
            // (not ones GT3 drew: those have pages a target holds) saved, or
            // drawn from a replacement picture (bindless: the colour mode's
            // path; REGION_REPEAT and textures read past their size keep GT3's).
            static const bool textureDump = gt3texdump::enabled();
            static const bool textureReplace = im.hasBindless && gt3texreplace::enabled();
            if ((textureDump || textureReplace) && !anyGpuPage)
            {
                std::vector<uint32_t> texels;
                const uint64_t id = im.textureIdentity(state, textureDump ? &texels : nullptr);
                if (textureDump)
                    im.dumpTexture(state, id, texels);
                if (textureReplace && id && wrapU != 3 && wrapV != 3 && decodeW == texW && decodeH == texH)
                    replacement = im.replacementFor(id);
            }
#elif defined(GS_TEXTURE_DUMP)
            // GT4_TEXTURE_DUMP (gt4_texture_dump.h): textures from GS memory saved once, only the texels the
            // draws sample (this primitive's footprint, fu0..fv1). GT4_TEXTURE_REPLACE (gt4_texture_replace.h): drawn
            // from a replacement picture instead (bindless, as GT3's; REGION_REPEAT and textures read past their
            // size keep GT4's).
            static const bool textureDump = gt3texdump::enabled();
            static const bool textureReplace = im.hasBindless && gt4texreplace::enabled();
            if ((textureDump || textureReplace) && !anyGpuPage)
            {
                std::vector<uint32_t> texels;
                const uint64_t id = im.textureIdentity(state, &texels);
                // (matched when first decoded, whichever draw that is; drawn where the draw allows it)
                const uint64_t key = textureReplace ? im.replaceKey(state, id, texels) : 0;
                if (key && wrapU != 3 && wrapV != 3 && decodeW == texW && decodeH == texH)
                    replacement = im.replacementFor(key);
                if (textureDump)
                {
                    im.dumpTexture(state, id, texels);
                    if (id)
                        gt4texdump::touch(id, fu0, fv0, fu1, fv1, im.frameCounter);
                }
            }
#endif
            if (!raw && !replacement)
                raw = (im.hasBindless && im.rawTextures && rawFormat) ? im.rawPagesTexture(tex, decodeW, decodeH, rawFirst, rawCount, &rawSignature) : 0;
            // Fast mode, bilinear: the colours decoded once and filtered by the
            // GPU (texMode 15), unless the wrap mode is REGION_REPEAT or a
            // REPEAT axis covers less than the whole texture.
            // GT3_GS_GL_COLOUR_NEAREST=1: nearest-sampled textures too. Negative
            // result (2026-09-29, 4x progressive): 43.8 vs 53.3 fps - those
            // textures change far more often, and the decodes cost more than
            // the samples save.
            static const bool colourNearest = envFlag("GT3_GS_GL_COLOUR_NEAREST", false);
            const bool colourEligible = im.fastTexturesActive && (k.linear || colourNearest) && wrapU != 3 && wrapV != 3 &&
                                        (wrapU != 0 || decodeW == texW) && (wrapV != 0 || decodeH == texH);
            if (replacement)
            {
                // A replaced texture: its final colours, any size, filtered
                // (the colour mode; u_texPages.xy the size it stands for).
                k.texMode = 15;
                k.texSource = replacement;
                k.linear = 1;
                k.texBasePage = texW;
                k.texPagesPerRow = texH;
                k.texPageW = 0;
                k.texPageH = 0;
            }
            else if (raw && colourEligible)
            {
                if (texAlphaPlane || texIndexed)
                    im.choosePalette(k, state);
                const GLuint colours = im.gpuColourTexture(tex, k, state, decodeW, decodeH, raw, rawFirst, rawCount, rawSignature);
                k.texMode = 15;
                k.texSource = colours;
            }
            else if (raw)
            {
                const GLuint decoded = im.gpuDecode ? im.gpuDecodedTexture(tex, decodeW, decodeH, raw, rawFirst, rawCount, rawSignature) : 0;
                k.texMode = decoded ? 14 : 13;
                k.texSource = decoded ? decoded : raw;
                k.texBasePage = static_cast<int32_t>(tex.tbp0);
                k.texPagesPerRow = static_cast<int32_t>(tex.tbw);
                k.texPageW = static_cast<int32_t>(tpsm);
                k.texPageH = 0;
                k.texTargetFbp = rawFirst;
                k.texTargetFbw = rawCount;
                k.texTargetPageH = 0;
                if (texAlphaPlane || texIndexed)
                    im.choosePalette(k, state);
            }
            else if (texAlphaPlane || texIndexed)
            {
                k.texMode = 12;
                k.texSource = im.decodedIndexTexture(state, decodeW, decodeH);
                im.choosePalette(k, state);
            }
            else
            {
                k.texMode = 0;
                k.texSource = im.decodedTexture(state, decodeW, decodeH);
            }
        }
    }

    if (im.scalesListed())
    {
        // Effects at their own scale (and GT4's full-height car view): a target read as the texture (modes 1-11; a
        // self-read's copy is at the destination's scale) and a CLUT read from a target, when their scale is not the
        // destination's.
        const Target &dstT = *im.targets[k.colorTarget];
        const Impl::TexScale dstScale{dstT.sx, dstT.sy};
        if (k.tme && k.texMode >= 1 && k.texMode <= 11 && !selfRead)
        {
            const Impl::TexScale ts = im.scaleOfTex(k.texSource);
            k.texScale = ts == dstScale ? 0 : Impl::packScale(ts);
        }
        if (k.tme && k.paletteMode == 1 && k.paletteTex)
        {
            const Impl::TexScale ps = im.scaleOfTex(k.paletteTex);
            k.paletteScale = ps == dstScale ? 0 : Impl::packScale(ps);
        }
    }

    if (im.stats)
    {
        // Step 7 T0: primitives whose resolved state equals the previous one's.
        ++im.statRunKeyed;
        if (im.statPrevKeyValid && im.statPrevKey == k)
            ++im.statRunSameKey;
        im.statPrevKey = k;
        im.statPrevKeyValid = true;
    }

    // ---- batch: primitives share a draw while the GL pipeline state (targets,
    // depth, masks, ordered blending) is the same; texture, palette and pixel
    // settings are per primitive (DrawState) when textures are bindless.
    DrawKey glKey{};
    glKey.colorTarget = k.colorTarget;
    glKey.depthTarget = k.depthTarget;
    if (!im.hasBindless)
    {
        glKey.texSource = k.texSource;
        glKey.paletteTex = k.paletteTex;
    }
    glKey.zte = k.zte;
    glKey.ztst = k.ztst;
    glKey.zmask = k.zmask;
    glKey.colorMask = k.colorMask;
    glKey.needsSplit = k.needsSplit;
    glKey.dstRead = k.dstRead;
    glKey.hwBlend = k.hwBlend;
    // The alpha test is per primitive in the shader; only the two-pass
    // alpha-fail modes (different write masks per pass) are pipeline state.
    if (k.ate && k.atst != 1 && k.afail != 0)
    {
        glKey.ate = k.ate;
        glKey.atst = k.atst;
        glKey.afail = k.afail;
    }
    glKey.frameFormat = k.frameFormat;
    GpuState st{};
    st.tex = (im.hasBindless && k.texSource) ? im.handleOf(k.texSource) : 0u;
    st.palette = (im.hasBindless && k.paletteTex) ? im.handleOf(k.paletteTex) : 0u;
    if (k.fastTarget && k.texSource)
        st.palette = im.linearHandleOf(k.texSource); // modes 1-3 use no palette
    const int32_t sv[15][4] = {{k.tme, k.fst, k.iip, k.fge},
                               {k.tfx, k.tcc, k.texW, k.texH},
                               {k.linear, k.wrapU, k.wrapV, k.texMode},
                               {k.minU, k.maxU, k.minV, k.maxV},
                               {k.ta0, k.aem, k.ta1, k.paletteMode},
                               {k.clutCsa, k.clutFour, k.clutCt24, static_cast<int32_t>(k.fbmsk)},
                               {k.fogR, k.fogG, k.fogB, k.frameFormat},
                               {k.ate, k.atst, k.aref, k.zte},
                               {k.ba, k.bb, k.bc, k.bd},
                               {k.abe, k.pabe, k.fix, k.fba},
                               {k.dstRead, k.date, k.datm, k.hwBlend},
                               {k.texBasePage, k.texPagesPerRow, k.texPageW, k.texPageH},
                               {k.texTargetFbp, k.texTargetFbw, k.texTargetPageH, 0},
                               {k.texOffX, k.texOffY, k.fastTarget | (k.texFull << 1), 0},
                               {k.clutOrgX, k.clutOrgY, 0, 0}};
    std::memcpy(st.v, sv, sizeof(sv));
    // (Shader scaleField: page height / CLUT x stay below 2^16.)
    st.v[12][2] |= (k.texScale & 0xFF) << 16;
    st.v[14][0] |= (k.paletteScale & 0xFF) << 16;
    if (k.mipCtl != 0)
    {
        // GS mipmapping (shader u_mipCtl, mipWord): the control word in v[12].w;
        // a per-pixel state (bit 10) also its levels 1-5 in v[13].x, v[13].y,
        // v[13].w, v[14].z, v[14].w. v[13].x/y otherwise hold texOffX/Y, read
        // only with fastTarget (v[13].z), which a per-pixel state (texMode 13,
        // no target view) never has. A resolved primitive's tint (bit 11 and its
        // level, PS2X_GS_MIP_TINT) is v[12].w only.
        st.v[12][3] = k.mipCtl;
        if ((k.mipCtl & 0x400) != 0)
        {
            st.v[13][0] = k.mipLv[0];
            st.v[13][1] = k.mipLv[1];
            st.v[13][3] = k.mipLv[2];
            st.v[14][2] = k.mipLv[3];
            st.v[14][3] = k.mipLv[4];
            st.v[13][2] = 0; // (fastTarget: never set together with a per-pixel mip state)
        }
    }
    if (k.dither)
    {
        // GS dithering (shader ditherOffset): DIMX rows 0-1 in v[13].w, rows 2-3 in v[14].z, on when v[14].w is
        // -1 (a per-pixel mip state's level words there are never negative, and never dithered: CPU, above).
        st.v[13][3] = k.dimxLo;
        st.v[14][2] = k.dimxHi;
        st.v[14][3] = -1;
    }
    {
        // Step 7 T1: the memo for this state's next primitive (or, verifying,
        // the comparison with the memo that would have served this one).
        // Memoised: exact mode, no self-read, per-pixel mip or GPU decode, and
        // textures drawn from the CPU copy (decoded 0, indices 12, raw 13).
        // (Fast mode too since 2026-10-14: memoState also compares the vertex-alpha bit its blend-unit choice reads.)
        bool memoisable = !mipPerPixel && !selfRead && !im.gpuDecode &&
                          (!k.tme || k.texMode == 0 || k.texMode == 12 || k.texMode == 13);
#if !defined(GT3_STOCK_BUILD) && !defined(PS2X_GAME_GT4)
        memoisable = memoisable && !gt3texdump::enabled() && !gt3texreplace::enabled();
#endif
#if defined(PS2X_GAME_GT4) && !defined(GT4_STOCK_BUILD)
        memoisable = memoisable && !gt4texdump::enabled() && !gt4texreplace::enabled(); // (diagnostics / replacement: always resolved in full)
#endif
        im.afterResolve(batch, k, glKey, st, memoCheck, memoisable, decodeW, decodeH);
    }
    im.breakBatchFor(glKey);
    // A self-read that can read rows it wrote itself (see the banding at the
    // end): some pixel reads a row at least one above its own, and the rows
    // it reads meet the rows it writes. Drawn in bands of selfBandRows from
    // fresh copies. (Reading its own pixel or rows below needs none.)
    const bool selfFeedback = selfRead && selfTexelIsPixel && im.selfBandRows > 0 && readKnown && readAbove >= 1.0f && ru0 <= bx1 &&
                              ru1 >= bx0 && rv0 <= by1 && rv1 >= by0 && by1 - by0 + 1 > im.selfBandRows;
    // A draw reading the target it draws into samples texScratch: the copy
    // made for the batch it joins, or a new one after what is queued is drawn.
    if (selfRead)
    {
        static const bool copyEach = envFlag("GT3_GS_GL_SELF_COPY_EACH", false);
#if defined(PS2X_GAME_GT3)
        // GT3_GS_GL_SELF_SHARE_SAFE=1: reuse only if the written-cells test passes.
        static const bool shareAll = !envFlag("GT3_GS_GL_SELF_SHARE_SAFE", false);
#else
        // GT4's wheel blur reads rows the previous sprite wrote with no TEXFLUSH between, so
        // sharing the batch's copy draws stale rims (race max diff 147 against the CPU
        // reference, analysis/phase5/specs/step3-compare.md 3.1). Exact copies by default;
        // GT3_GS_GL_SELF_SHARE_ALL=1 restores GT3's texture-cache rule.
        static const bool shareAll = envFlag("GT3_GS_GL_SELF_SHARE_ALL", false);
#endif
        // Cells first written before this tag's TEXFLUSH are off limits (any
        // written cell in the safe mode).
        // GT3_GS_GL_SELF_TEXFLUSH_COPY=1: a new copy after every TEXFLUSH (as before).
        static const bool flushCopies = envFlag("GT3_GS_GL_SELF_TEXFLUSH_COPY", false);
        const bool live = !copyEach && selfTexelIsPixel && im.haveBatch && im.selfSnap.valid && im.selfSnap.target == selfTarget &&
                          selfFirstRow >= im.selfSnap.firstRow && selfLastRow <= im.selfSnap.lastRow &&
                          !(flushCopies && im.selfSnap.texFlush != im.texFlushSerial);
        const uint32_t limit = live ? (shareAll ? im.selfSnap.tag(im.texFlushSerial) : 0xFFFFFFFFu) : 0u;
        const bool reuse = live && !selfFeedback && !im.selfSnap.touched(selfU0, selfV0, selfU1, selfV1, limit);
        // GT3_GS_GL_SELF_PARTIAL=0: a whole new copy of the rows instead.
        static const bool partial = envFlag("GT3_GS_GL_SELF_PARTIAL", true);
        if (reuse)
        {
            ++im.statSelfReuses;
        }
        else if (live && !selfFeedback && partial)
        {
            // Cells it reads were drawn since the copy: after the queued draws
            // the copy is brought up to date by copying only the box of the
            // cells drawn since (the rest is the same as the target), and is
            // then as good as a new copy of its rows. A whole copy of the rows
            // is ~12 MB at 4x; GT3's replay heat-haze particles took ~130 a
            // frame. Nothing else reaches the target before this primitive
            // joins the next batch.
            ++im.statSelfPartial;
            im.flushBatch();
            Target &from = *im.targets[selfTarget];
            const int x0 = im.selfSnap.dirtyX0, x1 = std::min(im.selfSnap.dirtyX1, from.width - 1);
            const int y0 = std::max(im.selfSnap.dirtyY0, im.selfSnap.firstRow * from.pageHeight);
            const int y1 = std::min(im.selfSnap.dirtyY1, (im.selfSnap.lastRow + 1) * from.pageHeight - 1);
            if (x1 >= x0 && y1 >= y0)
                im.copyScaled(from.tex, x0, y0, im.texScratch, x0, y0, x1 - x0 + 1, y1 - y0 + 1);
            // GT3_GS_GL_SELF_PARTIAL_CHECK=<n> (tests, slow): the first n such
            // copies are compared with a whole copy of the rows (read back).
            static long partialChecks = [] {
                const char *v = std::getenv("GT3_GS_GL_SELF_PARTIAL_CHECK");
                return v ? std::strtol(v, nullptr, 10) : 0L;
            }();
            if (partialChecks > 0)
            {
                --partialChecks;
                static GLuint check = 0;
                static int checkW = 0, checkH = 0;
                if (check && (checkW != im.texScratchW || checkH != im.texScratchH))
                    im.releaseTexture(check);
                if (!check)
                {
                    checkW = im.texScratchW;
                    checkH = im.texScratchH;
                    check = Impl::makeColorTexture(checkW, checkH);
                }
                const int pageH = from.pageHeight, rowY = im.selfSnap.firstRow * pageH;
                const int rowH = (im.selfSnap.lastRow - im.selfSnap.firstRow + 1) * pageH;
                im.copyScaled(from.tex, 0, rowY, check, 0, rowY, from.width, rowH);
                std::vector<uint32_t> a(static_cast<size_t>(checkW) * checkH), b(a.size());
                glBindTexture(GL_TEXTURE_2D, im.texScratch);
                glGetTexImage(GL_TEXTURE_2D, 0, GL_RGBA, GL_UNSIGNED_BYTE, a.data());
                glBindTexture(GL_TEXTURE_2D, check);
                glGetTexImage(GL_TEXTURE_2D, 0, GL_RGBA, GL_UNSIGNED_BYTE, b.data());
                glBindTexture(GL_TEXTURE_2D, 0);
                uint64_t differ = 0;
                for (int y = rowY * from.sy; y < (rowY + rowH) * from.sy && y < checkH; ++y)
                    for (int x = 0; x < from.width * from.sx && x < checkW; ++x)
                        differ += a[static_cast<size_t>(y) * checkW + x] != b[static_cast<size_t>(y) * checkW + x];
                std::fprintf(stderr, "[self-partial] frame %llu rows %d-%d dirty %d,%d-%d,%d: %llu samples differ from a whole copy\n",
                             (unsigned long long)im.frameCounter, im.selfSnap.firstRow, im.selfSnap.lastRow, x0, y0, x1, y1,
                             (unsigned long long)differ);
            }
            im.selfSnap.valid = true;
            im.selfSnap.texFlush = im.texFlushSerial;
            std::fill(im.selfSnap.written.begin(), im.selfSnap.written.end(), 0u);
            im.selfSnap.dirtyX0 = im.selfSnap.dirtyY0 = 0;
            im.selfSnap.dirtyX1 = im.selfSnap.dirtyY1 = -1;
        }
        else
        {
            if (im.stats)
            {
                const int why = (copyEach || !selfTexelIsPixel) ? 0
                                : !im.haveBatch                  ? 1
                                : !im.selfSnap.valid             ? 2
                                : im.selfSnap.target != selfTarget ? 3
                                : (selfFirstRow < im.selfSnap.firstRow || selfLastRow > im.selfSnap.lastRow) ? 5
                                : im.selfSnap.texFlush != im.texFlushSerial ? 4
                                                                 : 6;
                ++im.statSelfWhy[why];
            }
            im.flushBatch();
            Target &from = *im.targets[selfTarget];
            const int pageH = from.pageHeight;
            im.ensureTexScratch(from.width * from.sx, (selfLastRow + 1) * pageH * from.sy);
            im.copyScaled(from.tex, 0, selfFirstRow * pageH, im.texScratch, 0, selfFirstRow * pageH, from.width,
                          (selfLastRow - selfFirstRow + 1) * pageH);
            im.selfSnap.valid = true;
            im.selfSnap.target = selfTarget;
            im.selfSnap.firstRow = selfFirstRow;
            im.selfSnap.lastRow = selfLastRow;
            im.selfSnap.texFlush = im.texFlushSerial;
            im.selfSnap.cellsW = (from.width + 7) / 8;
            im.selfSnap.cellsH = (from.logicalHeight + 7) / 8;
            im.selfSnap.written.assign(static_cast<size_t>(im.selfSnap.cellsW) * static_cast<size_t>(im.selfSnap.cellsH), 0u);
            im.selfSnap.dirtyX0 = im.selfSnap.dirtyY0 = 0;
            im.selfSnap.dirtyX1 = im.selfSnap.dirtyY1 = -1;
            ++im.statSelfCopies;
            // texScratch may be a new texture now.
            k.texSource = im.texScratch;
            st.tex = im.hasBindless ? im.handleOf(im.texScratch) : 0u;
            if (!im.hasBindless)
                glKey.texSource = im.texScratch;
        }
    }

    // The target copies of the pages this primitive touches must be current.
    im.acquireForDraw(k, ctx, sprite, bx0, by0, bx1, by1);

    auto pushPrim = [&](GpuPrim &p) { im.pushPrim(p, k, glKey, st); };
    if (!selfFeedback)
    {
        pushPrim(prim);
    }
    else
    {
        // A draw that reads pixels it writes: in bands of rows, each from a
        // fresh copy, so later rows see what earlier ones wrote (as the GS
        // and the CPU renderer draw rows in order). GT3's auto-exposure CLUTs
        // are built so: a warped mesh reading the top of a 64-pixel target
        // writes below it, and when the mesh reaches above row 64 it reads
        // rows it wrote; from one snapshot those CLUTs came out wrong and the
        // whole screen's brightness jumped for a few frames (rally demo).
        Target &from = *im.targets[selfTarget];
        const int pageH = from.pageHeight;
#if !defined(PS2X_GAME_GT3)
        // GT4: a band takes a new copy only when the texel rows it can read
        // meet rows this primitive wrote since the last copy; otherwise it is
        // drawn from that copy, whose rows it reads are still the target's
        // (the result equals a copy before every band). GT4's wheel blur then
        // needs about one copy per triangle instead of 16-20
        // (analysis/phase5/specs/step4-picture.md 3.4, P2b).
        // GT3_GS_GL_SELF_BAND_COPY_ALL=1: a copy before every band (as GT3).
        static const bool copyAllBands = envFlag("GT3_GS_GL_SELF_BAND_COPY_ALL", false);
        // The texel rows [r0, r1] band rows [ya, yb] can read: v at the corners
        // of the primitive clipped to the band (affine for UV; linear-fractional
        // for S/T/Q, extremes at the corners too), the bilinear pair, one row of
        // margin each side, folded as readKnown's footprint. false: not known.
        auto bandReadRows = [&](int ya, int yb, int &r0, int &r1) -> bool {
            if (wrapV == 3)
                return false; // REGION_REPEAT maps rows
            const int texH = static_cast<int>(state.textureHeight);
            const float y0 = static_cast<float>(ya), y1 = static_cast<float>(yb) + 1.0f;
            float lo = 1e30f, hi = -1e30f;
            if (sprite)
            {
                const float va = prim.uvA[1] + (y0 - prim.uvB[1]) * prim.uvA[3];
                const float vb = prim.uvA[1] + (y1 - prim.uvB[1]) * prim.uvA[3];
                lo = std::min(va, vb);
                hi = std::max(va, vb);
            }
            else
            {
                bool ok = true;
                auto take = [&](float t, float q, float v) {
                    float tv = 0.0f;
                    if (state.prim.fst)
                        tv = v / 16.0f;
                    else if (q > 0.0f)
                        tv = t / q * static_cast<float>(texH);
                    else
                    {
                        ok = false;
                        return;
                    }
                    lo = std::min(lo, tv);
                    hi = std::max(hi, tv);
                };
                for (int i = 0; i < 3; ++i)
                {
                    const GSVertex &a = batch.vertices[i], &b = batch.vertices[(i + 1) % 3];
                    const float ay = a.y - static_cast<float>(ofy), bY = b.y - static_cast<float>(ofy);
                    if (ay >= y0 && ay <= y1)
                        take(a.t, a.q, static_cast<float>(a.v));
                    for (const float yc : {y0, y1})
                        if ((ay - yc) * (bY - yc) < 0.0f)
                        {
                            const float f = (yc - ay) / (bY - ay);
                            take(a.t + (b.t - a.t) * f, a.q + (b.q - a.q) * f,
                                 static_cast<float>(a.v) + (static_cast<float>(b.v) - static_cast<float>(a.v)) * f);
                        }
                }
                if (!ok)
                    return false;
            }
            if (!std::isfinite(lo) || !std::isfinite(hi) || lo > hi)
                return false;
            const float m = state.linearFilter ? 0.5f : 0.0f;
            r0 = floorToInt(lo - m) - 1;
            r1 = floorToInt(hi - m) + (state.linearFilter ? 1 : 0) + 1;
            if (wrapV == 1)
            {
                r0 = std::clamp(r0, 0, texH - 1);
                r1 = std::clamp(r1, 0, texH - 1);
            }
            else if (wrapV == 2)
            {
                r0 = std::clamp(r0, std::min(minV, maxV), std::max(minV, maxV));
                r1 = std::clamp(r1, std::min(minV, maxV), std::max(minV, maxV));
            }
            else if (r0 < 0 || r1 >= texH)
                return false; // REPEAT across the edge
            return true;
        };
        int copiedUpTo = by0; // rows of this primitive above this one are in the copy
#endif
        for (int y = by0; y <= by1; y += im.selfBandRows)
        {
            const int yEnd = std::min(by1, y + im.selfBandRows - 1);
#if defined(PS2X_GAME_GT3)
            const bool fresh = y != by0;
#else
            int r0 = 0, r1 = -1;
            // Rows this primitive wrote since the copy: [copiedUpTo, y - 1].
            const bool fresh = y != by0 && (copyAllBands || !bandReadRows(y, yEnd, r0, r1) || (r1 >= copiedUpTo && r0 <= y - 1));
            if (y != by0 && !fresh)
                ++im.statSelfBandsShared;
#endif
            if (fresh)
            {
                im.flushBatch();
                im.copyScaled(from.tex, 0, selfFirstRow * pageH, im.texScratch, 0, selfFirstRow * pageH, from.width,
                              (selfLastRow - selfFirstRow + 1) * pageH);
                ++im.statSelfCopies;
#if !defined(PS2X_GAME_GT3)
                copiedUpTo = y;
#endif
            }
            GpuPrim band = prim;
            band.bbox[1] = y;
            band.bbox[3] = yEnd;
            pushPrim(band);
        }
        im.flushBatch(); // the snapshot is of the last band only
        ++im.statSelfBanded;
    }
    im.reportStats();
}

void GSGlBackend::LoadClut(const GSTex0Reg &tex, const GSTexClutReg &texclut)
{
    onGl([&] {
        Impl::ScopedNs clutTimer(m_impl->statClutNs);
        ++m_impl->statClutCalls;
        bool cpuClutLoad = false, cpuClutWasGpu = false;
        std::array<uint16_t, 512> clutBefore{};
        // Records, on every way out, which load last wrote the CLUT blocks.
        struct RecordWriter
        {
            Impl *im = nullptr;
            uint32_t blocks = 0;
            uint64_t id = 0;
            ~RecordWriter()
            {
                if (im)
                    for (uint32_t b = 0; b < 32u; ++b)
                        if (blocks & (1u << b))
                            im->clutBlockWriter[b] = id;
            }
        } recordWriter;
        if (m_glReady)
        {
            Impl &im = *m_impl;
            im.flushPendingPatches();
            // GT3 loads the same CLUTs again and again (about 60,000 loads a
            // second in a race, alternating between a few). A load is skipped
            // when every 16-halfword block of the CLUT buffer it writes was
            // last written by the same load (parameters, and the owner, CPU
            // version and GPU write serial of the pages it reads).
            {
                const bool fourBit = tex.psm == GS_PSM_T4 || tex.psm == GS_PSM_T4HL || tex.psm == GS_PSM_T4HH;
                const bool eightBit = tex.psm == GS_PSM_T8 || tex.psm == GS_PSM_T8H;
                const bool csm2 = tex.csm != 0u;
                const uint32_t count = fourBit ? 16u : (eightBit ? 256u : 0u);
                const uint32_t params[8] = {tex.cbp, tex.cpsm, tex.csm, tex.csa, count, csm2 ? texclut.cbw : 0u, csm2 ? texclut.cou : 0u,
                                            csm2 ? texclut.cov : 0u};
                uint64_t id = hashBytes(params, sizeof(params), 0x434C55544C4F4144ull);
                bool anyGpuSource = false; // (statistics)
                auto visit = [&](uint32_t page) {
                    const uint32_t v[4] = {page, static_cast<uint32_t>(im.owner[page]), im.cpuVersion[page], im.writeSerial[page]};
                    id = hashBytes(v, sizeof(v), id);
                    anyGpuSource = anyGpuSource || im.owner[page] >= 0;
                    im.pageObserved(page);
                };
                if (!csm2)
                    forRectPages(tex.cbp >> 5u, (tex.cbp & 31u) != 0u, 1u, tex.cpsm, 0, 0, fourBit ? 7 : 15, fourBit ? 1 : 15, visit);
                else
                    forRectPages(tex.cbp >> 5u, (tex.cbp & 31u) != 0u, texclut.cbw, tex.cpsm, texclut.cou * 16, texclut.cov, texclut.cou * 16 + 255,
                                 texclut.cov, visit);
                id |= 1ull; // 0: a block no known load wrote
                // The blocks written (as GSCpuBackend::LoadClut): CT16 entries
                // are halfwords CSA*16.., CT32 ones low and high halfwords.
                uint32_t blocks = 0;
                const bool clut16 = tex.cpsm == GS_PSM_CT16 || tex.cpsm == GS_PSM_CT16S;
                for (uint32_t j = 0; j < count / 16u; ++j)
                {
                    if (clut16)
                        blocks |= 1u << ((tex.csa + j) & 31u);
                    else
                    {
                        const uint32_t b = ((tex.csa & 15u) + j) & 15u;
                        blocks |= (1u << b) | (1u << (b + 16u));
                    }
                }
                bool repeat = true;
                for (uint32_t b = 0; b < 32u && repeat; ++b)
                    if ((blocks & (1u << b)) && im.clutBlockWriter[b] != id)
                        repeat = false;
                if (im.stats && !repeat)
                {
                    // Why not: the same parameters with changed source pages
                    // (by owner: CPU or a GPU target), or other parameters.
                    const uint64_t paramsId = hashBytes(params, sizeof(params), 0x50415241ull) | 1ull;
                    bool sameParams = true;
                    for (uint32_t b = 0; b < 32u; ++b)
                        if ((blocks & (1u << b)) && im.clutBlockParams[b] != paramsId)
                            sameParams = false;
                    const int o = im.owner[(tex.cbp >> 5u) & (kPageCount - 1u)];
                    ++im.statClutWhy[sameParams ? (o >= 0 ? 1 : 0) : 2];
                    for (uint32_t b = 0; b < 32u; ++b)
                        if (blocks & (1u << b))
                            im.clutBlockParams[b] = paramsId;
                }
                if (repeat)
                {
                    ++im.statClutRepeats;
                    return;
                }
                if (im.stats)
                {
                    // Step 7 T0: loads done by size and source; loads whose
                    // blocks were all loaded before and not read since.
                    im.statClut16 += count == 16u ? 1u : 0u;
                    im.statClut256 += count == 256u ? 1u : 0u;
                    im.statClutCpuSource += anyGpuSource ? 0u : 1u;
                    if (blocks != 0u && (blocks & ~im.clutUnreadBlocks) == 0u)
                        ++im.statClutUnread;
                    im.clutUnreadBlocks |= blocks;
                }
                recordWriter.im = &im;
                recordWriter.blocks = blocks;
                recordWriter.id = id;
            }
            // A full 256-colour CSM1 load (CSA 0) from a 32-bit GPU target:
            // copy its 16x16 block on the GPU. The target may hold its pages in
            // PSMZ32 order (GT3's race loads a CLUT drawn inside a Z buffer's
            // memory, CBP 0x3120, about 27 times a second; each was a page read
            // back from the GPU).
            const uint32_t page = tex.cbp >> 5u, block = tex.cbp & 31u;
            int owner = im.owner[page];
            const bool fullLoad = tex.csm == 0u && tex.csa == 0u && (tex.psm == GS_PSM_T8 || tex.psm == GS_PSM_T8H) &&
                                  (tex.cpsm == GS_PSM_CT32 || tex.cpsm == GS_PSM_CT24);
            // A full load from a page a Z buffer holds (GT3's rally demo: ~40
            // a second, CBP 0x2f4f/0x2f50 in the Z buffer at 0x16f): the page
            // moves on the GPU to the colour target laid out as that buffer (Z
            // into RGB, the high plane from its holder or the CPU copy), then
            // the copy below. Each was a read back that waited for the GPU
            // (~6 ms at 2x internal resolution). GT3_GS_GL_NO_CLUT_FROM_Z=1: read back.
            static const bool noClutFromZ = envFlag("GT3_GS_GL_NO_CLUT_FROM_Z", false);
            // GT3_GS_GL_CLUT_FROM_Z_VERIFY=1 (native): the CLUT read back from
            // the Z buffer as before, compared with the GPU copy.
            static const bool verifyClutFromZ = envFlag("GT3_GS_GL_CLUT_FROM_Z_VERIFY", false);
            bool verifyThis = false;
            uint32_t truth[16 * 16] = {};
            if (!noClutFromZ && fullLoad && owner >= 0 && im.targets[owner]->depth && im.targets[owner]->layout == Layout::Z32 && block + 3u < 32u)
            {
                if (verifyClutFromZ && !im.scaled())
                {
                    im.downloadPage(page, true);
                    for (int y = 0; y < 16; ++y)
                        for (int x = 0; x < 16; ++x)
                            truth[y * 16 + x] = GSMem::ReadCT32(im.vram, tex.cbp, 1u, static_cast<uint32_t>(x), static_cast<uint32_t>(y));
                    verifyThis = true;
                }
                Target &z = *im.targets[owner];
                int zx = 0, zy = 0;
                if (z.pageRect(page, zx, zy))
                {
                    const uint32_t zfbp = z.fbp, zfbw = z.fbw;
                    const int from = owner;
                    if (im.stats)
                        im.cause = "CLUT from a Z buffer";
                    const int cid = im.findTarget(false, Layout::Z32, zfbp, zfbw, zy + z.pageHeight);
                    if (cid >= 0 && im.transferPage(page, from, cid))
                        owner = cid;
                }
            }
            // The CLUT's four 8x8 blocks (CBP..CBP+3, in one page) in the target.
            int quarterX[4] = {}, quarterY[4] = {};
            bool quartersFound = owner >= 0 && block + 3u < 32u && !im.targets[owner]->depth &&
                                 (im.targets[owner]->layout == Layout::C32 || im.targets[owner]->layout == Layout::Z32);
            for (uint32_t q = 0; quartersFound && q < 4u; ++q)
                quartersFound = clutBlockOrigin(im.targets[owner]->layout == Layout::Z32, block + q, quarterX[q], quarterY[q]);
            if (fullLoad && quartersFound)
            {
                Target &t = *im.targets[owner];
                int px = 0, py = 0;
                if (t.pageRect(page, px, py))
                {
                    // At a scale GPU CLUTs are held at the target scale too (copied
                    // sample for sample; lookups read each entry's middle sample).
                    if (im.clutRing.empty())
                        for (int i = 0; i < 64; ++i)
                            im.clutRing.push_back(Impl::makeColorTexture(16 * im.scaleX, 16 * im.scaleY));
                    const GLuint clut = im.clutRing[im.clutRingNext];
                    // The copy must see queued draws into its source, and must not
                    // replace a CLUT the queued draws still use.
                    if (im.haveBatch && (im.key.colorTarget == owner || im.batchUses(clut)))
                        im.flushBatch();
                    im.clutRingNext = (im.clutRingNext + 1u) % im.clutRing.size();
                    // The four blocks as one 16x16 square (CBP a multiple of 4 in
                    // CT32 order): one copy instead of four (each copy call was
                    // about 1% of the GS thread in the race).
                    const bool square = quarterX[1] == quarterX[0] + 8 && quarterY[1] == quarterY[0] && quarterX[2] == quarterX[0] &&
                                        quarterY[2] == quarterY[0] + 8 && quarterX[3] == quarterX[0] + 8 && quarterY[3] == quarterY[0] + 8;
                    // GT3_GS_GL_CLUT_NO_COPY=1: diagnostics only (what the copies
                    // cost; colours break).
                    static const bool noCopy = envFlag("GT3_GS_GL_CLUT_NO_COPY", false);
                    static const bool direct = envFlag("GT3_GS_GL_CLUT_DIRECT", true);
                    if (noCopy)
                    {
                    }
                    else if (square && direct && !verifyThis)
                    {
                        // Read straight from the target until it is written (clutDirect).
                        // Queued draws reading a previous direct CLUT of another
                        // target go first (its protection moves to this one).
                        if (im.clutDirect && im.gpuClutTex != t.tex && im.haveBatch && im.batchUses(im.gpuClutTex))
                            im.flushBatch();
                        im.gpuClutActive = true;
                        im.gpuClutTex = t.tex;
                        im.clutDirect = true;
                        im.clutOriginX = px + quarterX[0];
                        im.clutOriginY = py + quarterY[0];
                        // GT3_CLUT_DIRECT_LOG=<n>: the first n direct loads (diagnostics).
                        static long directLog = [] {
                            const char *v = std::getenv("GT3_CLUT_DIRECT_LOG");
                            return v ? std::strtol(v, nullptr, 10) : 0L;
                        }();
                        if (directLog > 0)
                        {
                            --directLog;
                            std::fprintf(stderr, "[clut-direct] cbp %04x cpsm %02x from target %d layout %d %03x/%u %dx%d at %d,%d (tex %u)\n", tex.cbp, tex.cpsm,
                                         owner, static_cast<int>(t.layout), t.fbp, t.fbw, t.width, t.height, im.clutOriginX, im.clutOriginY, t.tex);
                        }
                        im.gpuClutCpsm = tex.cpsm;
                        im.clutStale.set();
                        ++im.clutVersion;
                        ++im.statGpuCluts;
                        // GT3_GS_GL_CLUT_DIRECT=2 (diagnostics): copied at once, as before.
                        static const bool copyAtOnce = std::getenv("GT3_GS_GL_CLUT_DIRECT") && std::strcmp(std::getenv("GT3_GS_GL_CLUT_DIRECT"), "2") == 0;
                        if (copyAtOnce)
                            im.materializeDirectClut();
                        return;
                    }
                    else
                    {
                        im.leaveDirectClut(); // (gpuClutTex becomes this copy below)
                        if (square)
                            im.copyScaled(t.tex, px + quarterX[0], py + quarterY[0], clut, 0, 0, 16, 16);
                        else
                            for (int q = 0; q < 4; ++q)
                                im.copyScaled(t.tex, px + quarterX[q], py + quarterY[q], clut, (q & 1) * 8, (q >> 1) * 8, 8, 8);
                    }
                    if (verifyThis)
                    {
                        uint32_t gpu[16 * 16];
                        glBindTexture(GL_TEXTURE_2D, clut);
                        glGetTexImage(GL_TEXTURE_2D, 0, GL_RGBA, GL_UNSIGNED_BYTE, gpu);
                        glBindTexture(GL_TEXTURE_2D, 0);
                        im.forgetBindings();
                        static uint64_t checked = 0, wrong = 0;
                        int bad = 0, first = -1;
                        for (int i = 0; i < 256; ++i)
                            if (gpu[i] != truth[i])
                            {
                                ++bad;
                                if (first < 0)
                                    first = i;
                            }
                        ++checked;
                        if (bad)
                            ++wrong;
                        if (bad && wrong <= 20)
                            std::fprintf(stderr, "[clut-z] MISMATCH cbp %04x: %d of 256 entries, first %d gpu %08x read back %08x\n", tex.cbp, bad,
                                         first, gpu[first], truth[first]);
                        if ((checked & 255u) == 0u)
                            std::fprintf(stderr, "[clut-z] verify: %llu loads, %llu mismatching\n", (unsigned long long)checked,
                                         (unsigned long long)wrong);
                    }
                    im.gpuClutActive = true;
                    im.gpuClutTex = clut;
                    im.gpuClutCpsm = tex.cpsm;
                    im.clutStale.set();
                    ++im.clutVersion;
                    ++im.statGpuCluts;
                    return;
                }
            }
            // A 256-colour CT16 load (CSM1, CSA 0) from a 32-bit GPU target:
            // copied on the GPU (gpuClut16). GT3_GS_GL_NO_GPU_CLUT16=1: read back.
            // GT3_GS_GL_CLUT16_VERIFY=1 (native): compared with a read back.
            static const bool noClut16 = envFlag("GT3_GS_GL_NO_GPU_CLUT16", false);
            static const bool verifyClut16 = envFlag("GT3_GS_GL_CLUT16_VERIFY", false);
            const bool clut16Load = tex.csm == 0u && tex.csa == 0u && (tex.psm == GS_PSM_T8 || tex.psm == GS_PSM_T8H) &&
                                    (tex.cpsm == GS_PSM_CT16 || tex.cpsm == GS_PSM_CT16S);
            if (!noClut16 && clut16Load && owner >= 0 && im.gpuClut16(tex, owner))
            {
                if (verifyClut16 && !im.scaled())
                {
                    std::vector<uint32_t> gpu(256);
                    glBindTexture(GL_TEXTURE_2D, im.gpuClutTex);
                    glGetTexImage(GL_TEXTURE_2D, 0, GL_RGBA, GL_UNSIGNED_BYTE, gpu.data());
                    glBindTexture(GL_TEXTURE_2D, 0);
                    im.forgetBindings();
                    im.cause = "CT16 CLUT verify";
                    im.syncPagesToCpu(tex.cbp, 1u, tex.cpsm, 0, 0, 15, 15);
                    static uint64_t checked = 0, wrong = 0;
                    int bad = 0, first = -1;
                    for (uint32_t pos = 0; pos < 256u; ++pos)
                    {
                        const uint32_t readBack = tex.cpsm == GS_PSM_CT16S ? GSMem::ReadCT16S(im.vram, tex.cbp, 1u, pos & 15u, pos >> 4u)
                                                                            : GSMem::ReadCT16(im.vram, tex.cbp, 1u, pos & 15u, pos >> 4u);
                        if ((gpu[pos] & 0xFFFFu) != (readBack & 0xFFFFu))
                        {
                            ++bad;
                            if (first < 0)
                                first = static_cast<int>(pos);
                        }
                    }
                    ++checked;
                    if (bad)
                        ++wrong;
                    if (bad && wrong <= 20)
                        std::fprintf(stderr, "[clut16] MISMATCH cbp %04x: %d of 256, first %d gpu %04x read back %04x\n", tex.cbp, bad, first,
                                     gpu[first] & 0xFFFFu,
                                     (tex.cpsm == GS_PSM_CT16S ? GSMem::ReadCT16S(im.vram, tex.cbp, 1u, first & 15, first >> 4)
                                                               : GSMem::ReadCT16(im.vram, tex.cbp, 1u, first & 15, first >> 4)) & 0xFFFFu);
                    if ((checked & 63u) == 0u)
                        std::fprintf(stderr, "[clut16] verify: %llu loads, %llu mismatching\n", (unsigned long long)checked, (unsigned long long)wrong);
                }
                return;
            }
            // 16 CT32 entries (a CSM1 T4 load) while the CLUT buffer lives on the
            // GPU: merged into a copy of the GPU CLUT (entries CSA*16.. are the
            // 8x2 area at ((CSA & 1) * 8, (CSA >> 1 & 7) * 2) of its CSM1 16x16
            // layout, as the source's own 8x2), from a 32-bit GPU target or the
            // CPU copy. No read back of the GPU CLUT (materializeClut).
            const bool fourBitLoad = tex.psm == GS_PSM_T4 || tex.psm == GS_PSM_T4HL || tex.psm == GS_PSM_T4HH;
            if (!im.noClutMerge && im.gpuClutActive && tex.csm == 0u && fourBitLoad && tex.cpsm == im.gpuClutCpsm &&
                (tex.cpsm == GS_PSM_CT32 || tex.cpsm == GS_PSM_CT24))
            {
                const bool fromTarget = owner >= 0 && !im.targets[owner]->depth && im.targets[owner]->layout == Layout::C32;
                int px = 0, py = 0;
                const bool targetOk = fromTarget && im.targets[owner]->pageRect(page, px, py);
                // The 8x2 area must stay in one block (CBP's) of the page.
                if ((targetOk || owner < 0) && im.clutRing.size() >= 2u)
                {
                    im.materializeDirectClut(); // (merged into a copy of the 16x16 CLUT below)
                    const GLuint clut = im.clutRing[im.clutRingNext];
                    if (im.haveBatch && ((fromTarget && im.key.colorTarget == owner) || im.batchUses(clut)))
                        im.flushBatch();
                    im.clutRingNext = (im.clutRingNext + 1u) % im.clutRing.size();
                    im.copyScaled(im.gpuClutTex, 0, 0, clut, 0, 0, 16, 16);
                    const int k = static_cast<int>(tex.csa & 15u);
                    const int dx = (k & 1) * 8, dy = (k >> 1) * 2;
                    if (targetOk)
                    {
                        static constexpr uint8_t blockTable32[4][8] = {
                            {0, 1, 4, 5, 16, 17, 20, 21}, {2, 3, 6, 7, 18, 19, 22, 23}, {8, 9, 12, 13, 24, 25, 28, 29}, {10, 11, 14, 15, 26, 27, 30, 31}};
                        int bx = 0, by = 0;
                        for (int y = 0; y < 4; ++y)
                            for (int x = 0; x < 8; ++x)
                                if (blockTable32[y][x] == block)
                                {
                                    bx = x;
                                    by = y;
                                }
                        im.copyScaled(im.targets[owner]->tex, px + bx * 8, py + by * 8, clut, dx, dy, 8, 2);
                    }
                    else
                    {
                        uint32_t words[16];
                        for (uint32_t j = 0; j < 16u; ++j)
                            words[j] = GSMem::ReadCT32(im.vram, tex.cbp, 1u, j & 7u, j >> 3u);
                        glActiveTexture(GL_TEXTURE0);
                        im.writeTargetPixels(false, clut, dx, dy, 8, 2, GL_RGBA, GL_UNSIGNED_BYTE, words);
                    }
                    im.gpuClutTex = clut;
                    for (uint32_t j = 0; j < 16u; ++j)
                    {
                        const uint32_t entry = static_cast<uint32_t>(k) * 16u + j;
                        im.clutStale.set(entry);
                        im.clutStale.set(entry + 256u);
                    }
                    ++im.clutVersion;
                    ++im.statGpuCluts;
                    return;
                }
            }
            // A load the CPU buffer takes. Halfwords it does not write may
            // still lag the GPU CLUT (clutStale): read back only if a draw
            // needs one (GT3 loads 16 CT16 entries over its race CT32 CLUT).
            cpuClutWasGpu = im.gpuClutActive;
            im.gpuClutActive = false;
            ++im.statClutCpu;
            clutBefore = m_cpu.clutBuffer();
            cpuClutLoad = true;
            im.cause = "CLUT load";
            if (im.stats)
            {
                // The cause's text is made only if a download or upload reads it (step 7 T0).
                const uint32_t cpage = (tex.cbp >> 5u) & (kPageCount - 1u);
                const int o = im.owner[cpage];
                Impl::Cause::Clut c;
                c.cbp = tex.cbp;
                c.cpsm = tex.cpsm;
                c.psm = tex.psm;
                c.csm = tex.csm;
                c.csa = tex.csa;
                c.from = o >= 0 ? Impl::layoutName(*im.targets[o]) : "cpu";
                c.fbp = o >= 0 ? im.targets[o]->fbp : 0u;
                im.cause.setClut(c);
            }
            // GT3_GS_GL_CLUT_NO_SYNC=1 (diagnostics only; colours break): CLUT
            // loads read the CPU copy as it is (what their read backs cost).
            static const bool clutNoSync = envFlag("GT3_GS_GL_CLUT_NO_SYNC", false);
            if (clutNoSync)
            {
            }
            else if (tex.csm == 0u)
            {
                // CSM1: 16 entries are an 8x2 area, 256 a 16x16 one.
                const bool fourBit = tex.psm == GS_PSM_T4 || tex.psm == GS_PSM_T4HL || tex.psm == GS_PSM_T4HH;
                im.syncPagesToCpu(tex.cbp, 1u, tex.cpsm, 0, 0, fourBit ? 7 : 15, fourBit ? 1 : 15);
            }
            else
                im.syncPagesToCpu(tex.cbp, texclut.cbw, tex.cpsm, texclut.cou * 16, texclut.cov,
                                  texclut.cou * 16 + 255, texclut.cov);
        }
        {
            Impl::ScopedNs cpuTimer(m_impl->statClutCpuNs);
            m_cpu.LoadClut(tex, texclut);
        }
        if (cpuClutLoad)
        {
            // The halfwords the load wrote (as GSCpuBackend::LoadClut).
            const bool fourBit = tex.psm == GS_PSM_T4 || tex.psm == GS_PSM_T4HL || tex.psm == GS_PSM_T4HH;
            const bool eightBit = tex.psm == GS_PSM_T8 || tex.psm == GS_PSM_T8H;
            const uint32_t count = fourBit ? 16u : eightBit ? 256u : 0u;
            const bool clut16 = tex.cpsm == GS_PSM_CT16 || tex.cpsm == GS_PSM_CT16S;
            for (uint32_t j = 0; j < count; ++j)
            {
                if (clut16)
                    m_impl->clutStale.reset((static_cast<uint32_t>(tex.csa) * 16u + j) & 0x1FFu);
                else
                {
                    const uint32_t entry = (static_cast<uint32_t>(tex.csa & 0x0Fu) * 16u + j) & 0xFFu;
                    m_impl->clutStale.reset(entry);
                    m_impl->clutStale.reset(entry + 256u);
                }
            }
        }
        // A new palette version only when the CLUT actually changed: GT3
        // reloads the same CLUT before many draws, and every version rebuilt
        // and re-hashed the palette texture (about 5% of the GS thread).
        if (cpuClutLoad && (cpuClutWasGpu || m_cpu.clutBuffer() != clutBefore))
            ++m_impl->clutVersion;
    });
}

void GSGlBackend::BeginTransfer(const GSTransferCommand &command)
{
    onGl([&] {
        if (m_glReady)
        {
            Impl &im = *m_impl;
            // Host -> local uploads keep recording patch words (coalescePatches);
            // a read or local copy sees them applied first.
            if (command.direction != 0u)
                im.flushPendingPatches();
            // No flush: queued draws sample their own GL textures; pages they
            // own are copied back (and the batch flushed) only when touched.
            const auto &b = command.bitbltbuf;
            const auto &p = command.trxpos;
            const auto &r = command.trxreg;
            const int w = static_cast<int>(r.rrw), h = static_cast<int>(r.rrh);
            im.transferDestPages.clear();
            for (const uint32_t page : im.patchPages)
                im.patchSlot[page] = -1;
            im.patchPages.clear();
            im.cause = "transfer";
            if (im.stats)
            {
                char text[96];
                std::snprintf(text, sizeof(text), "transfer source dir %u spsm %02x %dx%d", command.direction, b.spsm, w, h);
                im.cause = text;
                static uint32_t logged = 0;
                if (command.direction != 0u && logged < 12u)
                {
                    ++logged;
                    std::fprintf(stderr, "[gs-gl] transfer dir %u from sbp 0x%x sbw %u spsm 0x%02x (%u,%u) %dx%d\n", command.direction,
                                 b.sbp, b.sbw, b.spsm, p.ssax, p.ssay, w, h);
                }
            }
            if (command.direction == 1u || command.direction == 2u)
                im.syncPagesToCpu(b.sbp, b.sbw, b.spsm, p.ssax, p.ssay, p.ssax + w - 1, p.ssay + h - 1);
            if (command.direction == 0u || command.direction == 2u)
            {
                forRectPages(b.dbp >> 5u, (b.dbp & 31u) != 0u, b.dbw, b.dpsm, p.dsax, p.dsay, p.dsax + w - 1, p.dsay + h - 1,
                             [&](uint32_t page) { im.transferDestPages.push_back(page); });
                // Pages an upload replaces entirely (formats that write whole
                // words, a page-aligned buffer) need no copy back first.
                const uint32_t dpsm = b.dpsm;
                const bool wholeWords = dpsm == GS_PSM_CT32 || dpsm == GS_PSM_CT16 || dpsm == GS_PSM_CT16S || dpsm == GS_PSM_T8 ||
                                        dpsm == GS_PSM_T4 || dpsm == GS_PSM_Z32 || dpsm == GS_PSM_Z16 || dpsm == GS_PSM_Z16S;
                std::vector<uint32_t> replaced;
                if (command.direction == 0u && wholeWords && (b.dbp & 31u) == 0u)
                {
                    uint32_t pageW = 64u, pageH = 32u;
                    pageGeometry(dpsm, pageW, pageH);
                    const uint32_t perRow = std::max(1u, (std::max<uint32_t>(b.dbw, 1u) * 64u) / pageW);
                    const int x0 = p.dsax, y0 = p.dsay, x1 = p.dsax + w - 1, y1 = p.dsay + h - 1;
                    for (int row = y0 / static_cast<int>(pageH); row <= y1 / static_cast<int>(pageH); ++row)
                        for (int col = x0 / static_cast<int>(pageW); col <= x1 / static_cast<int>(pageW); ++col)
                        {
                            const int px = col * static_cast<int>(pageW), py = row * static_cast<int>(pageH);
                            if (col < static_cast<int>(perRow) && px >= x0 && py >= y0 && px + static_cast<int>(pageW) - 1 <= x1 &&
                                py + static_cast<int>(pageH) - 1 <= y1)
                                replaced.push_back(((b.dbp >> 5u) + static_cast<uint32_t>(row) * perRow + static_cast<uint32_t>(col)) &
                                                   (kPageCount - 1u));
                        }
                }
                for (const uint32_t page : im.transferDestPages)
                {
                    if (im.owner[page] < 0)
                        continue;
                    if (std::find(replaced.begin(), replaced.end(), page) != replaced.end())
                    {
                        if (im.haveBatch && (im.owner[page] == im.key.colorTarget || im.owner[page] == im.key.depthTarget))
                            im.flushBatch();
                        im.targets[im.owner[page]]->version[page] = 0u;
                        im.debugOwner(page, -1, "transfer replaces");
                        im.owner[page] = -1;
                        im.pageChanged(page);
                        im.highPlane[page] = -1;
                        im.clearCpuValid(page);
                        if (im.pendingAny[page])
                        {
                            // Recorded upload words are in the CPU copy, which holds the page now.
                            im.pendingAny[page] = 0u;
                            std::fill_n(&im.pendingMask[page * 32u], 32, 0ull);
                            if (im.pendingPartial[page])
                            {
                                im.pendingPartial[page] = 0u;
                                std::fill_n(&im.pendingBits[page * 2048u], 2048, 0u);
                            }
                        }
                    }
                    else if (!im.noPatch && command.direction == 0u && (dpsm == GS_PSM_CT32 || dpsm == GS_PSM_Z32) && w * h <= 16384 &&
                             im.patchPages.size() < 16u && im.patchable(page))
                    {
                        // Stays on the GPU: UploadImage draws the words in.
                        im.patchSlot[page] = static_cast<int16_t>(im.patchPages.size());
                        im.patchPages.push_back(page);
                        im.patchCommand = command;
                    }
                    else if (!im.noPatch && command.direction == 0u && (dpsm == GS_PSM_T8 || dpsm == GS_PSM_T4) &&
                             (w * h <= 65536 || (im.targets[im.owner[page]]->tall && (b.dbp & 31u) == 0u)) && im.patchPages.size() < 16u &&
                             im.partialPatchable(page))
                    {
                        // (Any size into a page of GT4's full-height car view: GT Mode's 640x480 PSMT8 background over
                        // its composite, every frame; a copy back each time drained the GPU, longer with the car view's
                        // doubled rows. patchUpload skips the rows of the pages it does not patch.)
                        // Stays on the GPU: UploadImage records the bytes / nibbles,
                        // merged into the target (applyPartialPage).
                        im.patchSlot[page] = static_cast<int16_t>(im.patchPages.size());
                        im.patchPages.push_back(page);
                        im.patchCommand = command;
                    }
                    else
                    {
                        if (im.stats)
                        {
                            char text[112];
                            const Target &holder = *im.targets[im.owner[page]];
                            std::snprintf(text, sizeof(text), "transfer dest dir %u dpsm %02x %dx%d at %d,%d of %s %03x/%u", command.direction, b.dpsm,
                                          w, h, static_cast<int>(p.dsax), static_cast<int>(p.dsay), Impl::layoutName(holder), holder.fbp, holder.fbw);
                            im.cause = text;
                        }
                        im.downloadPage(page);
                    }
                }
            }
        }
        m_cpu.BeginTransfer(command);
#if defined(GS_TEXTURE_DUMP) && defined(PS2X_GAME_GT4)
        // GT4_TEXTURE_DUMP: which car or course an upload's bytes come from (gt4_texture_owner.h).
        static const bool ownerTags = gt4texdump::enabled();
        if (ownerTags && m_glReady && command.direction == 0u)
        {
            const uint32_t psm = command.bitbltbuf.dpsm;
            const uint64_t bits = psm == GS_PSM_T4 || psm == GS_PSM_T4HL || psm == GS_PSM_T4HH ? 4u
                                  : psm == GS_PSM_T8 || psm == GS_PSM_T8H                     ? 8u
                                  : psm == GS_PSM_CT16 || psm == GS_PSM_CT16S || psm == GS_PSM_Z16 || psm == GS_PSM_Z16S ? 16u
                                                                                                                        : 32u;
            gt4texowner::beginUpload(m_impl->transferDestPages, uint64_t(command.trxreg.rrw) * command.trxreg.rrh * bits / 8u);
        }
        else if (ownerTags && m_glReady && command.direction == 2u)
            gt4texowner::beginUpload(m_impl->transferDestPages, 0); // a copy inside GS memory: those pages' owner unknown
#endif
        if (m_glReady && command.direction == 2u)
            for (const uint32_t page : m_impl->transferDestPages)
            {
                ++m_impl->cpuVersion[page];
                m_impl->pageChangedAt[page] = ++m_impl->cpuVersionEpoch; // the raw-texture lookups see it (main-pc-merge: 0007 regression)
                m_impl->pageChanged(page);
            }
    });
}

void GSGlBackend::UploadImage(const uint8_t *data, uint32_t sizeBytes)
{
    onGl([&] {
        const uint32_t before = m_glReady && !m_impl->patchPages.empty() ? m_cpu.uploadedPixels() : 0u;
        m_cpu.UploadImage(data, sizeBytes);
#if defined(GS_TEXTURE_DUMP) && defined(PS2X_GAME_GT4)
        static const bool ownerTags = gt4texdump::enabled();
        if (ownerTags && m_glReady)
            gt4texowner::uploadData(data, sizeBytes);
#endif
        if (m_glReady)
        {
            Impl &im = *m_impl;
            if (!im.patchPages.empty())
                im.patchUpload(before, m_cpu.uploadedPixels());
            for (const uint32_t page : im.transferDestPages)
            {
                if (im.owner[page] >= 0 && im.patchSlot[page] < 0)
                    im.downloadPage(page);
                ++im.cpuVersion[page];
                im.pageChangedAt[page] = ++im.cpuVersionEpoch; // as BeginTransfer (main-pc-merge)
                im.pageChanged(page);
            }
        }
    });
}

void GSGlBackend::Flush()
{
    onGl([&] {
        if (m_glReady)
        {
            m_impl->flushPendingPatches();
            m_impl->flushBatch();
        }
        m_cpu.Flush();
    });
}

void GSGlBackend::TextureFlush()
{
    m_cpu.TextureFlush();
    if (m_impl)
        ++m_impl->texFlushSerial; // self-reading draws take a new copy of their target
}

void GSGlBackend::Marker(uint32_t code)
{
#if defined(VKGL_REDIRECT) && defined(PS2X_GAME_GT4) && !defined(GT4_STOCK_BUILD)
    gt4hudpic::gsMarker(code); // (the launcher's race pictures: in order with the primitives, on this thread)
    // The frame's RaceDisplay draw begins: the shown buffer read back as it is now (the 3D copied in, GT4's colours, no HUD).
    uint32_t fbp = 0;
    float x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    const uint32_t tag = (code & 0xffu) == 0xfeu && m_glReady ? gt4hudpic::shownReadTag(fbp, x0, y0, x1, y1) : 0u;
    if (tag)
    {
        Impl &im = *m_impl;
        im.flushPendingPatches();
        im.flushBatch();
        decltype(im.targets.front().get()) shown = nullptr;
        for (const auto &tp : im.targets)
            if (tp && !tp->depth && tp->tex && tp->fbp == fbp && (!shown || tp->lastUse > shown->lastUse))
                shown = tp.get();
        const int rx = std::max(0, static_cast<int>(x0)), ry = std::max(0, static_cast<int>(y0));
        bool ok = false;
        if (shown)
        {
            const int rw = std::min(shown->width, static_cast<int>(std::ceil(x1))) - rx, rh = std::min(shown->height, static_cast<int>(std::ceil(y1))) - ry;
            ok = rw > 16 && rh > 16 && vkgl::readStart(shown->tex, rx * shown->sx, ry * shown->sy, rw * shown->sx, rh * shown->sy, tag);
            static int said = 0;
            if (said++ < 4)
                std::fprintf(stderr, "[gt4-hud-pic] reading the shown buffer back (FBP %03x, %d,%d %dx%d at %dx%d): %s\n", fbp, rx, ry, rw, rh, shown->sx, shown->sy,
                             ok ? "started" : "not possible (the 3D buffer is used)");
        }
        if (!ok)
            gt4hudpic::shownReadDropped();
    }
    // GT4_HUD_MOVE's mirror: drawn at its new place as GT4's HUD begins (after the race picture's read of the clean frame).
    if ((code & 0xffu) == 0xfeu && m_glReady)
    {
        Impl &im = *m_impl;
        static const bool mvLog = [] {
            const char *v = std::getenv("GT4_HUD_MOVE_LOG"), *g = std::getenv("GT4_MIRROR_LOG");
            // (GT4_MIRROR_LOG=1: the same per-frame path of GT4's mirror: K kept, P a page-only one kept, d its pass over, R read)
            return (v && *v == '1' && gt4hudmove::mirrorMoved()) || (g && *g == '1');
        }();
        if (mvLog)
        {
            ++im.mvFrames;
            if (im.movedMirrorReady)
                im.mvEv += 'D';
            // (GT4_MIRROR_LOG: every 300 HUD frames, how many frames took each path; R, a read started, left out)
            {
                static std::map<std::string, int> paths;
                std::string key = im.mvEv;
                key.erase(std::remove(key.begin(), key.end(), 'R'), key.end());
                ++paths[key];
                if (im.mvFrames % 300u == 0u)
                {
                    gt4race::State rs;
                    std::string sum;
                    for (const auto &kv : paths)
                        sum += " " + (kv.first.empty() ? std::string("-") : kv.first) + ":" + std::to_string(kv.second);
                    std::fprintf(stderr, "[gt4-mirror] paths, 300 HUD frames to race vsync %llu:%s\n",
                                 gt4race::snapshot(rs) ? static_cast<unsigned long long>(rs.vsync) : 0ull, sum.c_str());
                    paths.clear();
                }
            }
            // (the usual frame: KCD; logged when it changes, after the race's first 600 HUD frames)
            if (im.mvEv != im.mvLastEv && im.mvFrames > 600 && im.mvSaid < 400 && im.mvEv.find('R') == std::string::npos &&
                im.mvLastEv.find('R') == std::string::npos)
            {
                ++im.mvSaid;
                gt4race::State rs;
                const unsigned long long vs = gt4race::snapshot(rs) ? static_cast<unsigned long long>(rs.vsync) : 0ull;
                char scs[160] = {};
                for (int k = 0; k < im.mvScN; ++k)
                    std::snprintf(scs + std::strlen(scs), sizeof(scs) - std::strlen(scs), " [%d,%d-%d,%d]", im.mvSc[k][0], im.mvSc[k][1], im.mvSc[k][2], im.mvSc[k][3]);
                int lx = 0, ly = 0, lw = 0, lh = 0;
                gt4mirror::lastRect(lx, ly, lw, lh);
                std::fprintf(stderr, "[gt4-hud-move] HUD frame %llu (race vsync %llu): mirror path %s (was %s); %d draws into the 3D buffer, scissors%s; mirror %d,%d %dx%d%c",
                             static_cast<unsigned long long>(im.mvFrames), vs, im.mvEv.c_str(), im.mvLastEv.c_str(), im.mvD2, scs, lx, ly, lw, lh, 10);
            }
            im.mvLastEv = im.mvEv;
        }
        im.mvEv.clear();
        im.mvD2 = im.mvScN = 0;
        // (a mirror kept with no copy after it is not carried into the next frame, where it would decide that frame's mirror)
        im.mirrorKept = Impl::MirrorKept{};
        // (HUD hand-off 7 C: a frame where GT4 skips its mirror pass, as it does now and then in the chase views with
        // GT4_MIRROR=both, shows the last mirror kept, up to 3 frames old, so the moved mirror does not blink)
        if (im.movedMirrorReady)
            im.movedAge = 0;
        else if (im.movedAge < 1000)
            ++im.movedAge;
        if (im.movedAge <= 3 && im.movedTw > 0)
        {
            im.movedMirrorReady = true;
            im.mirrorMoveDraw();
        }
    }
#else
    (void)code;
#endif
}

void GSGlBackend::Sync(GSSyncReason reason)
{
    onGl([&] {
        if (m_glReady)
        {
            m_impl->flushPendingPatches();
            m_impl->flushBatch();
        }
        m_cpu.Sync(reason);
    });
}

// The GT3 display case on the GPU: one read circuit of a 32-bit buffer,
// progressive or interlaced frame mode (INT+FFMD: weave the half-height fields).
// Anything else returns false and is composed by the CPU backend.
static bool gpuPresent(GSGlBackend::Impl &im, const GSPresentationRequest &request, PresentationFrame &frame);

PresentationFrame GSGlBackend::Present(const GSPresentationRequest &request)
{
    PresentationFrame frame{};
    // At the next packet boundary: the request was chosen from the GS state as
    // it is now, not after the packets still queued (a later frame's Z buffer
    // may overwrite the chosen buffer).
    const auto present = [&] {
        if (m_glReady)
        {
            Impl &im = *m_impl;
            im.flushPendingPatches();
            // Internal resolution changed (live settings, F4): switch now, between frames.
            if (gsgl::renderScale() != im.scaleRequest || gsgl::progressiveScan() != im.progressive)
                im.rescale(gsgl::renderScale(), gsgl::progressiveScan());
            im.flushBatch();
            ++im.frameCounter;
#if defined(VKGL_REDIRECT) && defined(PS2X_GAME_GT4) && !defined(GT4_STOCK_BUILD)
            im.picReadDone = false; // (the launcher's race pictures: one read of the 3D buffer a frame)
#endif
            im.reportRunVerify(); // (step 7 T1 self-check, every 300 frames)
            if (!im.colourTextures.empty())
                im.recycleColourTextures();
            // Targets unused for 5 s of frames that hold no newest pages are dropped.
            if ((im.frameCounter & 63u) == 0u)
            {
                std::vector<bool> owns(im.targets.size(), false);
                for (const int id : im.owner)
                    if (id >= 0)
                        owns[static_cast<size_t>(id)] = true;
                for (const int id : im.highPlane)
                    if (id >= 0 && static_cast<size_t>(id) < owns.size())
                        owns[static_cast<size_t>(id)] = true;
                for (size_t i = 0; i < im.targets.size(); ++i)
                    if (im.targets[i] && !owns[i] && im.targets[i]->lastUse + 300u < im.frameCounter)
                    {
                        const Target &t = *im.targets[i];
                        // (Its own entry only: the key with the tall bit, and only while it names this slot.)
                        if (auto it = im.targetIndex.find(Impl::targetIndexKey(t)); it != im.targetIndex.end() && it->second == static_cast<int>(i))
                            im.targetIndex.erase(it);
                        im.forgetTargetTex(im.targets[i]->tex);
                        im.recycleTargetTexture(im.targets[i]->tex, t.depth, t.width * t.sx, t.height * t.sy);
                        im.targets[i].reset();
                        im.resetRuns(); // (step 7 T1)
                    }
            }
            // Decoded textures unused for 10 s of presented frames are dropped.
            if (im.textures.size() > 512u)
                for (auto it = im.textures.begin(); it != im.textures.end();)
                {
                    if (it->second.lastUse + 600u < im.frameCounter)
                    {
                        im.lastPaletteKey = ~0ull;
                        im.lastPaletteTex = 0;
                        im.paletteSlots.fill({});
                        im.forgetRawAnswers();
                        im.lastSpan.valid = false;
                        im.releaseTexture(it->second.tex);
                        it = im.textures.erase(it);
                    }
                    else
                        ++it;
                }
            if (gpuPresent(im, request, frame))
            {
                im.reportStats(true);
                return;
            }
            for (int circuit = 0; circuit < 2; ++circuit)
            {
                if (((request.pmode >> circuit) & 1u) == 0u)
                    continue;
                const uint64_t dispfb = circuit ? request.dispfb2 : request.dispfb1;
                const uint64_t display = circuit ? request.display2 : request.display1;
                const uint32_t fbp = static_cast<uint32_t>(dispfb & 0x1FFu);
                const uint32_t fbw = std::max<uint32_t>(1u, static_cast<uint32_t>((dispfb >> 9) & 0x3Fu));
                const uint32_t psm = static_cast<uint32_t>((dispfb >> 15) & 0x1Fu);
                const int dbx = static_cast<int>((dispfb >> 32) & 0x7FFu), dby = static_cast<int>((dispfb >> 43) & 0x7FFu);
                const int magh = static_cast<int>((display >> 23) & 0xFu);
                const int width = static_cast<int>(((display >> 32) & 0xFFFu) + 1u) / (magh + 1);
                const int height = static_cast<int>(((display >> 44) & 0x7FFu) + 1u);
                im.cause = "presentation";
                if (im.stats)
                {
                    // Diagnostic: displayed pages a Z buffer holds.
                    int depthPages = 0, colorPages = 0, cpuPages = 0;
                    int depthOwner = -1;
                    forRectPages(fbp, false, fbw, psm, dbx, dby, dbx + width - 1, dby + height - 1, [&](uint32_t page) {
                        const int id = im.owner[page];
                        if (id < 0)
                            ++cpuPages;
                        else if (im.targets[id]->depth)
                        {
                            ++depthPages;
                            depthOwner = id;
                        }
                        else
                            ++colorPages;
                    });
                    if (depthPages)
                        std::fprintf(stderr, "[gs-gl] present tick %llu dispfb %03x: %d pages from Z %03x/%u, %d colour, %d cpu\n",
                                     static_cast<unsigned long long>(request.vsyncTick), fbp, depthPages, im.targets[depthOwner]->fbp,
                                     im.targets[depthOwner]->fbw, colorPages, cpuPages);
                }
                forRectPages(fbp, false, fbw, psm, dbx, dby, dbx + width - 1, dby + height - 1, [&](uint32_t page) {
                    if (im.owner[page] >= 0 && im.peekSerial[page] != im.writeSerial[page])
                        im.downloadPage(page, true);
                });
            }
            im.reportStats(true);
        }
        frame = m_cpu.Present(request);
    };
    if (!m_glReady || std::this_thread::get_id() == m_glThread)
        present();
    else if (m_runOnGlThreadNext)
        m_runOnGlThreadNext(present);
    else
        onGl(present);
    return frame;
}

bool GSGlBackend::ClearFramebuffer(const GSContext &context, uint32_t rgba)
{
    bool result = false;
    onGl([&] {
        if (m_glReady)
        {
            m_impl->flushPendingPatches();
            m_impl->flushBatch();
            m_impl->bumpPages(context.frame.fbp, false, context.frame.fbw, context.frame.psm, context.scissor.x0, context.scissor.y0,
                              context.scissor.x1, context.scissor.y1);
        }
        result = m_cpu.ClearFramebuffer(context, rgba);
    });
    return result;
}

uint32_t GSGlBackend::ConsumeLocalToHostBytes(uint8_t *dst, uint32_t maxBytes) { return m_cpu.ConsumeLocalToHostBytes(dst, maxBytes); }

uint32_t GSGlBackend::ReadVram(uint32_t psm, uint32_t base, uint32_t bw, uint32_t x, uint32_t y) const
{
    uint32_t value = 0u;
    onGl([&] {
        if (m_glReady)
        {
            m_impl->flushPendingPatches();
            m_impl->flushBatch();
            m_impl->cause = "ReadVram";
            m_impl->syncPagesToCpu(base, bw, psm, static_cast<int>(x), static_cast<int>(y),
                                   static_cast<int>(x), static_cast<int>(y));
        }
        value = m_cpu.ReadVram(psm, base, bw, x, y);
    });
    return value;
}

void GSGlBackend::WriteVram(uint32_t psm, uint32_t base, uint32_t bw, uint32_t x, uint32_t y, uint32_t value)
{
    onGl([&] {
        if (m_glReady)
        {
            m_impl->flushPendingPatches();
            m_impl->flushBatch();
            m_impl->bumpPages(base >> 5u, (base & 31u) != 0u, bw, psm, static_cast<int>(x), static_cast<int>(y), static_cast<int>(x),
                              static_cast<int>(y));
        }
        m_cpu.WriteVram(psm, base, bw, x, y, value);
    });
}

void GSGlBackend::SnapshotVram(std::vector<uint8_t> &out) const
{
    onGl([&] {
        if (m_glReady)
        {
            m_impl->flushPendingPatches();
            m_impl->flushBatch();
            m_impl->syncAllToCpu();
        }
        m_cpu.SnapshotVram(out);
    });
}

GSTransferSnapshot GSGlBackend::GetTransferSnapshot() const { return m_cpu.GetTransferSnapshot(); }

// GL thread idle time (GSAsyncBackend::run): one spare target texture
// (PS2X_GS_GL_PREWARM), only on the GL thread once GL is up.
bool GSGlBackend::IdleWork()
{
    if (!m_glReady || std::this_thread::get_id() != m_glThread)
        return false;
    return m_impl->makeSpareTarget();
}

void GSGlBackend::Reset()
{
    onGl([&] {
        if (m_glReady)
        {
            m_impl->flushPendingPatches();
            m_impl->flushBatch();
            m_impl->materializeClut();
            m_impl->syncAllToCpu();
            m_impl->clutBlockWriter.fill(0ull);
            m_impl->resetRuns(); // (step 7 T1)
        }
        m_cpu.Reset();
    });
}

// Both read circuits on one buffer, the same size and magnification, at the
// same height and less than one GS pixel apart across, blended by PMODE.ALP
// (MMOD 1, no SLBG): GT4's 1080i "softening" (GS-lib state 0x17, PMODE
// 0x8063, DISPLAY1 = DISPLAY2 + 1 VCK; VIDEO-MODES.md 1.5). Merged as one
// picture: each GS pixel mixed with its left (circuit 1 later) or right
// neighbour by weight = ALP / 255 x |DX1 - DX2| / (MAGH + 1).
struct CircuitMerge
{
    bool active = false;
    int dx = 0;        // -1: mixed with the pixel to the left, +1 the right, 0 none (the same place)
    float weight = 0.f;
};
static CircuitMerge circuitMerge(const GSPresentationRequest &r)
{
    CircuitMerge m;
#if defined(PS2X_GAME_GT3)
    (void)r; // GT3 builds: every two-circuit setup goes to the CPU path, as before
    return m;
#else
    // DISPFB: FBP, FBW, PSM, DBX, DBY; DISPLAY: MAGH, MAGV, DW, DH (DX, DY apart).
    constexpr uint64_t fbMask = 0x1FFull | (0x3Full << 9) | (0x1Full << 15) | (0x7FFull << 32) | (0x7FFull << 43);
    constexpr uint64_t sizeMask = (0xFull << 23) | (0x3ull << 27) | (0xFFFull << 32) | (0x7FFull << 44);
    if ((r.dispfb1 & fbMask) != (r.dispfb2 & fbMask) || (r.display1 & sizeMask) != (r.display2 & sizeMask))
        return m;
    if (((r.display1 >> 12) & 0x7FFu) != ((r.display2 >> 12) & 0x7FFu))
        return m; // a vertical offset (GS-lib state 0x16): the CPU path
    const bool mmod = ((r.pmode >> 5) & 1u) != 0u, slbg = ((r.pmode >> 7) & 1u) != 0u;
    if (!mmod || slbg)
        return m;
    const int vck = static_cast<int>((r.display2 >> 23) & 0xFu) + 1;
    const int d = static_cast<int>(r.display1 & 0xFFFu) - static_cast<int>(r.display2 & 0xFFFu);
    if (d <= -vck || d >= vck)
        return m;
    m.active = true;
    m.dx = d > 0 ? -1 : (d < 0 ? 1 : 0);
    m.weight = static_cast<float>((r.pmode >> 8) & 0xFFu) / 255.0f * static_cast<float>(d < 0 ? -d : d) / static_cast<float>(vck);
    return m;
#endif
}

#if !defined(PS2X_GAME_GT3)
// PS2X_BLACK_WHEN_OFF=0: with no read circuit on, keep showing the last frame
// (as before) instead of black.
static bool blackWhenOff()
{
    static const bool on = envFlag("PS2X_BLACK_WHEN_OFF", true);
    return on;
}

// No read circuit shows anything (GT4 turns PMODE's circuits off while it
// switches the video mode, until the first frame in the new mode is drawn):
// a black frame, as a TV shows it (VIDEO-MODES.md 3.5).
static bool presentBlack(GSGlBackend::Impl &im, const GSPresentationRequest &request, PresentationFrame &frame)
{
    uint32_t width = 640u, height = 448u;
    const uint64_t display = ((request.display2 >> 32) & 0xFFFu) != 0u ? request.display2 : request.display1;
    const uint32_t dw = static_cast<uint32_t>((display >> 32) & 0xFFFu), dh = static_cast<uint32_t>((display >> 44) & 0x7FFu);
    const uint32_t magh = static_cast<uint32_t>((display >> 23) & 0xFu);
    if ((dw + 1u) / (magh + 1u) >= 64u && dh + 1u >= 64u)
    {
        width = std::min((dw + 1u) / (magh + 1u), 640u);
        height = std::min(dh + 1u, 1024u);
    }
    gsgl::setPresentRows(height);
    const GLuint out = gsgl::beginPresentSlot();
    const int outWidth = static_cast<int>(width) * im.slotScaleX(), outHeight = static_cast<int>(height) * im.slotRows();
    glBindFramebuffer(GL_FRAMEBUFFER, im.presentFbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, out, 0);
    glDrawBuffer(GL_COLOR_ATTACHMENT0);
    glViewport(0, 0, outWidth, outHeight);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_BLEND);
    glDisable(GL_SCISSOR_TEST);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glUseProgram(im.presentProgram);
    glBindVertexArray(im.vao);
    glUniform1i(im.presentUniform("u_black"), 1);
    glDrawArrays(GL_TRIANGLES, 0, 6);
    glUniform1i(im.presentUniform("u_black"), 0);
    glBindVertexArray(0);
    glUseProgram(0);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    im.forgetBindings();
    im.glStateDirty = true;
    gsgl::notePresentTick(ps2x::vblank::composingPresentation());
    gsgl::notePresentField(-1, 0, request.crt, static_cast<int>(height));
    gsgl::endPresentSlot(static_cast<uint32_t>(outWidth), static_cast<uint32_t>(outHeight));
    frame.gpuFrame = true;
    frame.width = width;
    frame.height = height;
    frame.displayFbp = frame.sourceFbp = 0u;
    return true;
}
#endif

static bool gpuPresent(GSGlBackend::Impl &im, const GSPresentationRequest &request, PresentationFrame &frame)
{
    static const bool disabled = envFlag("GT3_GS_GL_CPU_PRESENT", false);
    static const bool weave = !envFlag("GT3_WEAVE", true) ? false : true;
    static const int parityFlip = [] {
        const char *v = std::getenv("GT3_WEAVE_PARITY");
        return (v && *v == '0') ? 0 : 1;
    }();
    // GT3_DEINTERLACE: adaptive (default), weave, bob, double (GT3_WEAVE=0:
    // double); live (gsgl::deinterlaceMode: 0 adaptive, 1 weave, 2 bob, 3 double,
    // 4 blend: woven here, the lines blended by the host's picture pass, 5 motion
    // adaptive; GT4_DEINTERLACE in GT4 builds).
    const int liveMode = gsgl::deinterlaceMode();
    const int deinterlace = !weave || liveMode == 3 ? 1 : liveMode == 1 || liveMode == 4 ? 2 : liveMode == 2 ? 3 : liveMode == 5 ? 6 : 4;
    static const float margin = [] {
        const char *v = std::getenv("GT3_DEINTERLACE_MARGIN");
        return v && *v ? std::clamp(std::strtof(v, nullptr), 0.f, 1.f) : 0.06f;
    }();
    // Motion adaptive: a change above this (0..1 per channel) is movement
    // (PCSX2's FastMAD sensitivity, 0.08). GT3_DEINTERLACE_MOTION (diagnostics).
    static const float motion = [] {
        const char *v = std::getenv("GT3_DEINTERLACE_MOTION");
        return v && *v ? std::clamp(std::strtof(v, nullptr), 0.f, 1.f) : 0.08f;
    }();
    // Motion adaptive keeps four fields (each compared with the one two before
    // it); the other modes two. A change of ring starts the history again.
    const int ring = deinterlace == 6 ? 4 : 2;
    if (ring != im.fieldRing)
    {
        im.fieldRing = ring;
        im.fieldCur = 0;
        for (uint64_t &ft : im.fieldTick)
            ft = ~0ull;
        for (int i = 2; i < ring; ++i)
            if (!im.fieldTex[i] && im.fieldTex[0])
                im.fieldTex[i] = im.makeColorTexture(640 * im.scaleX, 512 * im.scaleY);
    }
    // GT3_FIELD_PARITY=flip swaps the rows of phased fields (diagnostics).
    static const int phaseFlip = [] {
        const char *v = std::getenv("GT3_FIELD_PARITY");
        return v && std::string_view(v) == "flip" ? 1 : 0;
    }();
    if (disabled || !im.presentProgram || !GS_GL_GPU_PRESENT)
        return false;
    // GT3_PRESENT_WHY=1 (diagnostic): why the GPU presentation gives up (the
    // CPU path shows the frame at the PS2's resolution), every 120 calls.
    static const bool why = envFlag("GT3_PRESENT_WHY", false);
    static uint64_t whyCount = 0;
    auto giveUp = [&](const char *reason) {
        if (why && (whyCount++ % 120u) == 0u)
            std::fprintf(stderr, "[present-why] CPU path: %s (pref %d dest %03x src %03x/%u)\n", reason, request.hasPreferredSource ? 1 : 0,
                         request.preferredDestFbp, request.preferredSource.fbp, request.preferredSource.fbw);
        return false;
    };
    const bool crt1 = (request.pmode & 1u) != 0u, crt2 = (request.pmode & 2u) != 0u;
    auto setup = [](uint64_t display, uint64_t dispfb) {
        const uint32_t fbw = static_cast<uint32_t>((dispfb >> 9) & 0x3Fu);
        return fbw != 0u || ((display >> 32) & 0xFFFu) != 0u || ((display >> 44) & 0x7FFu) != 0u || ((display >> 23) & 0xFu) != 0u;
    };
    const bool valid1 = crt1 && setup(request.display1, request.dispfb1);
    const bool valid2 = crt2 && setup(request.display2, request.dispfb2);
#if !defined(PS2X_GAME_GT3)
    if (!valid1 && !valid2 && blackWhenOff())
        return presentBlack(im, request, frame);
#endif
    // Both circuits on one buffer less than a pixel apart (GT4's 1080i
    // softening): merged in the shader. Other two-circuit setups: the CPU path.
    const CircuitMerge merge = valid1 && valid2 ? circuitMerge(request) : CircuitMerge{};
    if (valid1 == valid2 && !merge.active)
        return giveUp("circuits"); // none, or both circuits blended: the CPU path
    const uint64_t dispfb = valid2 ? request.dispfb2 : request.dispfb1;
    const uint64_t display = valid2 ? request.display2 : request.display1;
    const uint32_t fbp = static_cast<uint32_t>(dispfb & 0x1FFu);
    const uint32_t fbw = static_cast<uint32_t>((dispfb >> 9) & 0x3Fu);
    const uint32_t psm = static_cast<uint32_t>((dispfb >> 15) & 0x1Fu);
    const int ox = static_cast<int>((dispfb >> 32) & 0x7FFu), oy = static_cast<int>((dispfb >> 43) & 0x7FFu);
    // PSMCT16: GT4's 480p and 1080i display buffers (VIDEO-MODES.md 1.3).
    const bool c16 = psm == GS_PSM_CT16;
    if (fbw == 0u || (psm != GS_PSM_CT32 && psm != GS_PSM_CT24 && !c16))
        return giveUp("format");
    if (request.hasPreferredSource && request.preferredDestFbp == fbp &&
        (request.preferredSource.fbw != 0u || request.preferredSource.fbp != fbp))
        return giveUp("preferred display source");
    const bool interlaced = (request.smode2 & 1u) != 0u, ffmd = (request.smode2 & 2u) != 0u;
    if (interlaced && !ffmd)
        return giveUp("field presentation"); // field presentation: the CPU path
    const bool halfHeight = interlaced && ffmd;
    // decodeDisplaySize
    const uint32_t dw = static_cast<uint32_t>((display >> 32) & 0xFFFu), dh = static_cast<uint32_t>((display >> 44) & 0x7FFu);
    const uint32_t magh = static_cast<uint32_t>((display >> 23) & 0xFu);
    uint32_t width = (dw + 1u) / (magh + 1u), height = dh + 1u;
    if (width < 64u || height < 64u)
    {
        width = 640u;
        height = 448u;
    }
    width = std::min(width, 640u);
    // 1080i: 960 display lines, two 480-row fields (VIDEO-MODES.md 3.2).
    height = std::min(height, 1024u);
    const uint32_t readHeight = halfHeight ? (height + 1u) / 2u : height;
    if (static_cast<uint32_t>(ox) + width > fbw * 64u)
        return giveUp("width");
    if (readHeight > 512u)
        return giveUp("height"); // the field textures hold 512 rows

#if defined(VKGL_REDIRECT) && defined(PS2X_GAME_GT4) && !defined(GT4_STOCK_BUILD)
    if (im.dlssPending) // (HUD mode: the 3D's copy shown with nothing drawn after it yet)
        im.dlssApply();
#endif
    // The displayed rows on a GPU target laid out like the display buffer.
    const int id = im.findTarget(false, c16 ? Layout::C16 : Layout::C32, fbp, fbw, oy + static_cast<int>(readHeight));
    if (id < 0)
        return giveUp("no target");
    std::vector<uint32_t> pages;
    forRectPages(fbp, false, fbw, psm, ox, oy, ox + static_cast<int>(width) - 1, oy + static_cast<int>(readHeight) - 1,
                 [&](uint32_t page) { pages.push_back(page); });
    im.cause = "presentation";
    im.acquirePageList(id, pages);
    if (im.effectScale && (im.targets[id]->sx != im.scaleX || im.targets[id]->sy != im.scaleY))
        im.promoteTarget(id, "display"); // (effects at their own scale: the displayed frame is a scene target)
    Target &t = *im.targets[id];
#if defined(VKGL_REDIRECT) && defined(PS2X_GAME_GT4) && !defined(GT4_STOCK_BUILD)
    if (im.dlssDumpShown != ~0u)
        im.dlssDumpPresented(fbp, ox, oy, static_cast<int>(width), static_cast<int>(readHeight));
#endif
#if defined(PS2X_GAME_GT4) && !defined(GT4_STOCK_BUILD)
    im.shotAtPresent(id, ox, oy, static_cast<int>(width), static_cast<int>(readHeight));
#endif

    // Field history: the last two distinct fields, each with the row parity
    // it belongs on. With a known half-line phase (GT3 draws each field with
    // a fractional OFY of 0 or 0.5 line: 0.5 samples half a field line lower,
    // the odd rows) a field is new when its write sequence changes, so a field
    // shown for several VSyncs (below 60 fps) keeps its rows. Without a phase
    // (movies) one field per VSync with the tick's parity, as before.
    const uint64_t tick = request.vsyncTick;
    const bool phased = halfHeight && request.fieldPhase != 0xFFu;
    const bool fresh = phased ? (im.fieldSeq[im.fieldCur] != request.fieldSeq || im.fieldTick[im.fieldCur] == ~0ull)
                              : im.fieldTick[im.fieldCur] != tick;
    if (fresh)
    {
        im.fieldCur = (im.fieldCur + 1) % im.fieldRing;
        im.fieldTick[im.fieldCur] = tick;
        im.fieldSeq[im.fieldCur] = request.fieldSeq;
        im.fieldPhased[im.fieldCur] = phased;
        im.fieldParity[im.fieldCur] = phased ? (((request.fieldPhase >> 3) & 1) ^ phaseFlip)
                                             : static_cast<int>((tick & 1u) ^ static_cast<uint64_t>(parityFlip));
        im.fieldW[im.fieldCur] = width;
        im.fieldH[im.fieldCur] = readHeight;
        im.copyScaled(t.tex, ox, oy, im.fieldTex[im.fieldCur], 0, 0, static_cast<int>(width), static_cast<int>(readHeight));
#if defined(PS2X_GAME_GT4) && !defined(GT4_STOCK_BUILD)
        // (the VU1 tap counts presentations; its motion pass runs at the end of each frame's 3D, tapFrameDone)
        static const bool tapOn = gt4tap::motion();
        if (tapOn)
            ++im.tapPresents;
#endif
    }
    const int cur = im.fieldCur, prev = (im.fieldCur + im.fieldRing - 1) % im.fieldRing;
    // Motion adaptive: the fields two and three before the current one.
    const int prev2 = (im.fieldCur + 2 * im.fieldRing - 2) % im.fieldRing, prev3 = (im.fieldCur + 2 * im.fieldRing - 3) % im.fieldRing;
    int mode = 0;
    if (halfHeight)
    {
        // Two fields combine only when they fill opposite rows and are recent
        // (a phased field up to 4 VSyncs old; otherwise the previous VSync).
        const bool pair = im.fieldW[prev] == width && im.fieldH[prev] == readHeight &&
                          im.fieldParity[prev] != im.fieldParity[cur] && im.fieldTick[prev] != ~0ull &&
                          (im.fieldPhased[cur] ? im.fieldPhased[prev] && tick - im.fieldTick[prev] <= 4u
                                               : request.allowWeave && im.fieldTick[prev] + 1u == tick);
        mode = deinterlace == 1 ? 1 : pair ? deinterlace : 3;
        if (mode == 6)
        {
            // ... and motion adaptive needs the two fields before those, of
            // alternating parity, the same size and as recent (a phased field
            // up to 12 VSyncs back; otherwise one per VSync); until then, bob.
            const auto same = [&](int i) { return im.fieldTex[i] && im.fieldTick[i] != ~0ull && im.fieldW[i] == width && im.fieldH[i] == readHeight; };
            const bool history = same(prev2) && same(prev3) && im.fieldParity[prev2] == im.fieldParity[cur] &&
                                 im.fieldParity[prev3] == im.fieldParity[prev] &&
                                 (im.fieldPhased[cur] ? im.fieldPhased[prev2] && im.fieldPhased[prev3] && tick - im.fieldTick[prev3] <= 12u
                                                      : im.fieldTick[prev2] + 2u == tick && im.fieldTick[prev3] + 3u == tick);
            if (!history)
                mode = 3;
        }
    }
    const int parity = im.fieldParity[cur];
    // Progressive (fields drawn at twice the rows): the newest field alone, in
    // place. Measured (gt3-tools/frame_shift.py, Trial Mountain start, still
    // car): GT3's odd fields already cover the same rows as the even ones
    // (edges line up; only texture sampling differs slightly), so the
    // half-line shift this used to apply to odd fields made the whole picture
    // jump a line every field ("the screen twitches", native and 2x).
    int shift = 0;
    // GT3_GS_GL_PROGRESSIVE_SHIFT (diagnostics): odd-field shift in half field
    // lines (1 lower, 0 none, -1 higher).
    static const int shiftSign = [] {
        const char *v = std::getenv("GT3_GS_GL_PROGRESSIVE_SHIFT");
        return v && *v ? std::atoi(v) : 0;
    }();
    if (halfHeight && im.progressive && im.fieldPhased[cur])
    {
        mode = 5;
        shift = parity != 0 ? im.outRows * shiftSign : 0;
    }
    // Output: scaleX pixels per GS pixel across, outRows per display line.
    const int outWidth = static_cast<int>(width) * im.scaleX, outHeight = static_cast<int>(height) * im.outRows;
    // GT3_FIELD_LOG=<first tick>: 300 presentations from that tick.
    static const uint64_t fieldLogFrom = [] {
        const char *v = std::getenv("GT3_FIELD_LOG");
        return v && *v ? std::strtoull(v, nullptr, 10) : ~0ull;
    }();
    static int fieldLogLeft = 300;
    if (tick >= fieldLogFrom && fieldLogLeft > 0)
    {
        --fieldLogLeft;
        std::fprintf(stderr, "[field] tick=%llu phase=%u seq=%llu fresh=%d parity=%d/%d prevTick=%llu mode=%d crt=%s %ux%u%s%s\n",
                     static_cast<unsigned long long>(tick), request.fieldPhase,
                     static_cast<unsigned long long>(request.fieldSeq), fresh ? 1 : 0, im.fieldParity[cur], im.fieldParity[prev],
                     static_cast<unsigned long long>(im.fieldTick[prev]), mode, ps2x::crt::name(request.crt), width, height,
                     c16 ? " ct16" : "", merge.active ? " merged" : "");
    }

    gsgl::setPresentRows(height);
    const GLuint out = gsgl::beginPresentSlot();
    // (GT4's DLSS: the picture drawn into its input, DLSS into the slot below at the slot's scale)
    GLuint drawInto = out;
#if defined(VKGL_REDIRECT) && defined(PS2X_GAME_GT4) && !defined(GT4_STOCK_BUILD)
    const bool dlssPresent = im.dlssOut != 0 && out != 0;
    if (dlssPresent)
        drawInto = im.dlssInput(outWidth, outHeight);
#endif
    glBindFramebuffer(GL_FRAMEBUFFER, im.presentFbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, drawInto, 0);
    glDrawBuffer(GL_COLOR_ATTACHMENT0);
    glViewport(0, 0, outWidth, outHeight);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_BLEND);
    glDisable(GL_SCISSOR_TEST);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glUseProgram(im.presentProgram);
    glBindVertexArray(im.vao);
    glUniform1i(im.presentUniform("u_black"), 0);
    // GT4's race colour (gs_fullcolour.h): with 32-bit a 16-bit buffer in 480p / 1080i holds each pixel's
    // full colour in its texels and is shown as it is (the CPU presenter shows the same F).
    glUniform1i(im.presentUniform("u_c16"), c16 && !gsfc::shows(psm, request.crt) ? 1 : 0);
    glUniform1i(im.presentUniform("u_cols"), static_cast<GLint>(width));
    glUniform1i(im.presentUniform("u_mergeDx"), merge.active ? merge.dx : 0);
    glUniform1f(im.presentUniform("u_mergeW"), merge.active ? merge.weight : 0.0f);
    glUniform1i(im.presentUniform("u_cur"), 0);
    glUniform1i(im.presentUniform("u_prev"), 1);
    glUniform1i(im.presentUniform("u_prev2"), 2);
    glUniform1i(im.presentUniform("u_prev3"), 3);
    glUniform1f(im.presentUniform("u_motion"), motion);
    glUniform1i(im.presentUniform("u_mode"), mode);
    glUniform1i(im.presentUniform("u_parity"), parity);
    glUniform1i(im.presentUniform("u_rows"), static_cast<GLint>(readHeight));
    glUniform1f(im.presentUniform("u_margin"), margin);
    glUniform2i(im.presentUniform("u_scale"), im.scaleX, im.scaleY);
    glUniform1i(im.presentUniform("u_outRows"), im.outRows);
    glUniform1i(im.presentUniform("u_shift"), shift);
    // Units 2 and 3: the motion-adaptive mode's older fields (otherwise the
    // previous field again, never read). forgetBindings below drops the
    // renderer's cached bindings of units 1-5.
    glActiveTexture(GL_TEXTURE3);
    glBindTexture(GL_TEXTURE_2D, im.fieldTex[mode == 6 ? prev3 : prev]);
    glActiveTexture(GL_TEXTURE2);
    glBindTexture(GL_TEXTURE_2D, im.fieldTex[mode == 6 ? prev2 : prev]);
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, im.fieldTex[prev]);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, im.fieldTex[cur]);
    glDrawArrays(GL_TRIANGLES, 0, 6);
    glBindVertexArray(0);
    glUseProgram(0);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    im.forgetBindings();
    im.glStateDirty = true;
    int slotW = outWidth, slotH = outHeight;
#if defined(VKGL_REDIRECT) && defined(PS2X_GAME_GT4) && !defined(GT4_STOCK_BUILD)
    if (dlssPresent)
    {
        slotW = static_cast<int>(width) * im.slotScaleX();
        slotH = static_cast<int>(height) * im.slotRows();
        im.dlssPresent(out, fbp, ox, oy, static_cast<int>(width), static_cast<int>(readHeight), outWidth, outHeight, slotW, slotH);
        im.forgetBindings();
        im.glStateDirty = true;
    }
#endif
    // Labelled with the latch's own tick (`tick` is the shown buffer's), for
    // the host loop's grid pacing.
    gsgl::notePresentTick(ps2x::vblank::composingPresentation());
    // The newest field's line parity for the host's scanline pass (field
    // emphasis); none for a frame without fields or drawn progressive.
    gsgl::notePresentField(halfHeight && mode != 5 ? parity : -1, mode, request.crt, static_cast<int>(height));
    gsgl::endPresentSlot(static_cast<uint32_t>(slotW), static_cast<uint32_t>(slotH));

    frame.gpuFrame = true;
    frame.width = width;
    frame.height = height;
    frame.displayFbp = fbp;
    frame.sourceFbp = fbp;
    return true;
}

void GSGlBackend::PeekToLocalMemory(uint32_t fbp, uint32_t fbw, uint32_t psm, int x0, int y0, int x1, int y1)
{
    onGl([&] {
        if (!m_glReady)
            return;
        Impl &im = *m_impl;
        im.flushPendingPatches();
        im.flushBatch();
        im.cause = "compare";
        forRectPages(fbp, false, std::max<uint32_t>(fbw, 1u), psm, x0, y0, x1, y1, [&](uint32_t page) {
            if (im.owner[page] >= 0)
                im.downloadPage(page, true);
        });
    });
}

uint32_t GSGlBackend::ProbeTarget(uint32_t fbp, uint32_t fbw, uint32_t psm, int x, int y, int &id)
{
    uint32_t value = 0xDEADBEEFu;
    id = -1;
    onGl([&] {
        if (!m_glReady)
            return;
        Impl &im = *m_impl;
        im.flushPendingPatches();
        im.flushBatch();
        const Layout layout = psm == GS_PSM_CT16 ? Layout::C16 : ((psm == GS_PSM_Z24 || psm == GS_PSM_Z32) ? Layout::Z32 : Layout::C32);
        fbw = std::max(fbw, 1u);
        const uint64_t indexKey = (static_cast<uint64_t>(layout) << 1) | (static_cast<uint64_t>(fbp & 0x1FFu) << 4) | (static_cast<uint64_t>(fbw) << 16);
        auto it = im.targetIndex.find(indexKey);
        if (it == im.targetIndex.end() || !im.targets[it->second])
            return;
        id = it->second;
        const Target &t = *im.targets[id];
        if (x >= t.width || y >= t.height)
            return;
        im.readTargetPixels(false, t.tex, x, y, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, &value);
    });
    return value;
}

std::string GSGlBackend::DescribeClutEntry(const GSTex0Reg &tex, uint32_t index)
{
    std::string text;
    onGl([&] {
        if (!m_glReady)
            return;
        Impl &im = *m_impl;
        im.flushPendingPatches();
        const bool fourBit = tex.psm == GS_PSM_T4 || tex.psm == GS_PSM_T4HL || tex.psm == GS_PSM_T4HH;
        uint32_t entry = fourBit ? (tex.csa * 16u + (index & 15u)) : ((tex.csa & 15u) * 16u + index);
        entry &= 255u;
        const std::array<uint16_t, 512> &buf = m_cpu.clutBuffer();
        const uint32_t cpuValue = buf[entry] | (static_cast<uint32_t>(buf[entry + 256u]) << 16u);
        char out[200];
        if (im.gpuClutActive && tex.cpsm == im.gpuClutCpsm)
        {
            const uint32_t pos = (entry & ~0x18u) | ((entry & 0x08u) << 1u) | ((entry & 0x10u) >> 1u);
            uint32_t value = 0;
            im.protectDirectClut(im.gpuClutTex); // (a 16x16 CLUT texture to read)
            glBindTexture(GL_TEXTURE_2D, im.gpuClutTex);
            std::vector<uint32_t> all(static_cast<size_t>(16 * im.scaleX) * static_cast<size_t>(16 * im.scaleY));
            glGetTexImage(GL_TEXTURE_2D, 0, GL_RGBA, GL_UNSIGNED_BYTE, all.data());
            glBindTexture(GL_TEXTURE_2D, 0);
            im.forgetBindings();
            const auto cs = im.scaleOfTex(im.gpuClutTex);
            value = all[static_cast<size_t>((pos >> 4) * cs.y + cs.y / 2) * static_cast<size_t>(16 * im.scaleX) +
                        static_cast<size_t>((pos & 15u) * cs.x + cs.x / 2)];
            std::snprintf(out, sizeof(out), "gpu CLUT (cpsm %02x) entry %u = %08x (cpu buffer %08x, stale %d/%d)", im.gpuClutCpsm, entry, value,
                          cpuValue, im.clutStale[entry] ? 1 : 0, im.clutStale[entry + 256u] ? 1 : 0);
        }
        else
            std::snprintf(out, sizeof(out), "cpu buffer entry %u = %08x (stale %d/%d, gpu active %d cpsm %02x)", entry, cpuValue,
                          im.clutStale[entry] ? 1 : 0, im.clutStale[entry + 256u] ? 1 : 0, im.gpuClutActive ? 1 : 0, im.gpuClutCpsm);
        text = out;
    });
    return text;
}

std::string GSGlBackend::DescribePage(uint32_t page)
{
    std::string text;
    onGl([&] {
        if (!m_glReady)
            return;
        Impl &im = *m_impl;
        page &= kPageCount - 1u;
        char buf[256];
        const int o = im.owner[page];
        std::snprintf(buf, sizeof(buf), "owner %s %03x/%u (id %d) high %d cpuVersion %u valid %d", o >= 0 ? Impl::layoutName(*im.targets[o]) : "cpu",
                      o >= 0 ? im.targets[o]->fbp : 0u, o >= 0 ? im.targets[o]->fbw : 0u, o, im.highPlane[page], im.cpuVersion[page],
                      im.cpuValidAny[page]);
        text = buf;
        for (size_t i = 0; i < im.targets.size(); ++i)
            if (im.targets[i])
            {
                int x = 0, y = 0;
                if (im.targets[i]->pageRect(page, x, y) && im.targets[i]->version[page] != 0u)
                {
                    std::snprintf(buf, sizeof(buf), "; %s %03x/%u v%u", Impl::layoutName(*im.targets[i]), im.targets[i]->fbp, im.targets[i]->fbw,
                                  im.targets[i]->version[page]);
                    text += buf;
                }
            }
    });
    return text;
}

GSCompareBackend::GSCompareBackend(std::unique_ptr<GSGlBackend> gl) : m_gl(std::move(gl))
{
    if (const char *v = std::getenv("GT3_GS_COMPARE_MEMWATCH_FROM"); v && *v)
        m_traceFrom = std::strtoull(v, nullptr, 10);
    if (const char *v = std::getenv("GT3_GS_COMPARE_MEMWATCH"); v && *v)
    {
        std::string list(v);
        size_t pos = 0;
        while (pos < list.size())
        {
            const size_t end = list.find(';', pos);
            const std::string item = list.substr(pos, end == std::string::npos ? std::string::npos : end - pos);
            unsigned fbp = 0, fbw = 0, x = 0, y = 0;
            if (std::sscanf(item.c_str(), "%x,%u,%u,%u", &fbp, &fbw, &x, &y) == 4)
            {
                MemWatch w;
                w.word = wordAddressCT32(fbp * 32u, std::max(fbw, 1u), x, y);
                w.name = item;
                m_memWatch.push_back(w);
            }
            if (end == std::string::npos)
                break;
            pos = end + 1;
        }
    }
    if (const char *v = std::getenv("GT3_GS_COMPARE_WATCH"); v && *v)
    {
        std::string list(v);
        size_t pos = 0;
        while (pos < list.size())
        {
            const size_t end = list.find(';', pos);
            const std::string item = list.substr(pos, end == std::string::npos ? std::string::npos : end - pos);
            int x = 0, y = 0;
            if (std::sscanf(item.c_str(), "%d,%d", &x, &y) == 2)
                m_watch.emplace_back(x, y);
            if (end == std::string::npos)
                break;
            pos = end + 1;
        }
    }
}

void GSCompareBackend::checkMemWatch(const char *event, const std::vector<uint32_t> &pages)
{
    for (MemWatch &w : m_memWatch)
    {
        const uint32_t page = w.word >> 11u;
        if (std::find(pages.begin(), pages.end(), page) == pages.end())
            continue;
        m_gl->Flush();
        m_shadow.Flush();
        m_shadow.Sync(GSSyncReason::DebugReadback);
        m_gl->PeekToLocalMemory(page, 1u, GS_PSM_CT32, 0, 0, 63, 31);
        uint32_t a = 0, b = 0;
        std::memcpy(&a, m_vram + static_cast<size_t>(w.word) * 4u, 4);
        std::memcpy(&b, m_shadowVram.data() + static_cast<size_t>(w.word) * 4u, 4);
        int d = 0;
        for (int ch = 0; ch < 4; ++ch)
            d = std::max(d, std::abs(static_cast<int>((a >> (ch * 8)) & 0xFFu) - static_cast<int>((b >> (ch * 8)) & 0xFFu)));
        const bool differs = d >= 16;
        // GT3_GS_COMPARE_MEMWATCH_FROM=<tick>: every event from then on (400 lines).
        {
            char line[640];
            std::snprintf(line, sizeof(line), "tick %llu %s: gl %08x cpu %08x | %s", (unsigned long long)m_lastTick, event, a, b,
                          m_gl->DescribePage(page).c_str());
            ringNote(line);
        }
        const bool changed = a != w.lastGl || b != w.lastCpu;
        w.lastGl = a;
        w.lastCpu = b;
        if (changed && m_lastTick >= m_traceFrom && m_traceLines < 400u)
        {
            ++m_traceLines;
            std::fprintf(stderr, "[gs-compare] TRACE tick %llu %s: gl %08x cpu %08x%s | %s\n", (unsigned long long)m_lastTick, event, a, b,
                         differs ? " DIFF" : "", m_gl->DescribePage(page).c_str());
        }
        if (differs && !w.differs && m_watchReports < 60u)
        {
            ++m_watchReports;
            std::fprintf(stderr, "[gs-compare] MEMWATCH %s (word %05x, page %03x) diverged after %s: gl %08x cpu %08x\n", w.name.c_str(), w.word,
                         page, event, a, b);
            std::fprintf(stderr, "[gs-compare]   page before: %s\n[gs-compare]   page after:  %s\n", m_memWatchBefore.c_str(),
                         m_gl->DescribePage(page).c_str());
            if (m_watchReports <= 3u)
            {
                const size_t n = m_ring.size();
                for (size_t k = 0; k < n; ++k)
                    std::fprintf(stderr, "[gs-compare]   recent: %s\n", m_ring[(m_ringNext + k) % n].c_str());
            }
        }
        w.differs = differs;
    }
}

void GSCompareBackend::watchDraw(const GSPrimitiveBatch &batch)
{
    const GSContext &c = batch.state.context;
    const float ofx = static_cast<float>(c.xyoffset.ofx) / 16.0f, ofy = static_cast<float>(c.xyoffset.ofy) / 16.0f;
    float minX = 1e9f, minY = 1e9f, maxX = -1e9f, maxY = -1e9f;
    for (uint32_t v = 0; v < batch.vertexCount && v < 3u; ++v)
    {
        minX = std::min(minX, batch.vertices[v].x - ofx);
        maxX = std::max(maxX, batch.vertices[v].x - ofx);
        minY = std::min(minY, batch.vertices[v].y - ofy);
        maxY = std::max(maxY, batch.vertices[v].y - ofy);
    }
    const uint32_t fbp = c.frame.fbp & 0x1FFu, fbw = std::max<uint32_t>(c.frame.fbw, 1u);
    const bool frame32 = c.frame.psm == GS_PSM_CT32 || c.frame.psm == GS_PSM_CT24;
    // GT3_GS_COMPARE_WATCH_FBP=<hex>,<hex>..: only draws into these buffers (fewer read backs).
    static const std::vector<uint32_t> watchFbps = [] {
        std::vector<uint32_t> list;
        if (const char *v = std::getenv("GT3_GS_COMPARE_WATCH_FBP"))
            for (const char *p = v; *p;)
            {
                char *end = nullptr;
                const unsigned long f = std::strtoul(p, &end, 16);
                if (end == p)
                    break;
                list.push_back(static_cast<uint32_t>(f));
                p = (*end == ',') ? end + 1 : end;
            }
        return list;
    }();
    const bool fbpWatched = watchFbps.empty() || std::find(watchFbps.begin(), watchFbps.end(), fbp) != watchFbps.end();
    std::vector<std::pair<int, int>> hit;
    for (const auto &[x, y] : m_watch)
        if (fbpWatched && frame32 && x >= minX - 1.0f && x <= maxX + 1.0f && y >= minY - 1.0f && y <= maxY + 1.0f && x >= c.scissor.x0 &&
            x <= c.scissor.x1 && y >= c.scissor.y0 && y <= c.scissor.y1)
            hit.emplace_back(x, y);
    if (hit.empty())
    {
        m_gl->Submit(batch);
        m_shadow.Submit(batch);
        return;
    }
    auto read = [&](std::vector<uint32_t> &gl, std::vector<uint32_t> &cpu) {
        m_gl->Flush();
        m_shadow.Flush();
        m_shadow.Sync(GSSyncReason::DebugReadback);
        gl.clear();
        cpu.clear();
        for (const auto &[x, y] : hit)
        {
            m_gl->PeekToLocalMemory(fbp, fbw, c.frame.psm, x, y, x, y);
            gl.push_back(GSMem::ReadCT32(m_vram, fbp * 32u, fbw, static_cast<uint32_t>(x), static_cast<uint32_t>(y)));
            cpu.push_back(GSMem::ReadCT32(m_shadowVram.data(), fbp * 32u, fbw, static_cast<uint32_t>(x), static_cast<uint32_t>(y)));
        }
    };
    std::vector<uint32_t> gl0, cpu0, gl1, cpu1;
    read(gl0, cpu0);
    m_gl->Submit(batch);
    m_shadow.Submit(batch);
    read(gl1, cpu1);
    const uint32_t mask = c.frame.psm == GS_PSM_CT24 ? 0x00FFFFFFu : 0xFFFFFFFFu;
    // GT3_GS_COMPARE_WATCH_MIN=<n>: the channel difference that counts as diverged (16).
    static const int watchMin = [] {
        const char *v = std::getenv("GT3_GS_COMPARE_WATCH_MIN");
        return v ? std::max(1, std::atoi(v)) : 16;
    }();
    for (size_t i = 0; i < hit.size(); ++i)
    {
        auto far = [mask](uint32_t a, uint32_t b) {
            int d = 0;
            for (int ch = 0; ch < 4; ++ch)
                if ((mask >> (ch * 8)) & 0xFFu)
                    d = std::max(d, std::abs(static_cast<int>((a >> (ch * 8)) & 0xFFu) - static_cast<int>((b >> (ch * 8)) & 0xFFu)));
            return d >= watchMin;
        };
        const bool before = !far(gl0[i], cpu0[i]), after = !far(gl1[i], cpu1[i]);
        if (before && !after && m_watchReports < 40u)
        {
            ++m_watchReports;
            {
                int id = -1;
                const uint32_t probe = m_gl->ProbeTarget(fbp, fbw, c.frame.psm, hit[i].first, hit[i].second, id);
                const uint32_t word = wordAddressCT32(fbp * 32u, fbw, static_cast<uint32_t>(hit[i].first), static_cast<uint32_t>(hit[i].second));
                std::fprintf(stderr, "[gs-compare]   drawn target id %d texel %08x; page %03x: %s\n", id, probe, word >> 11u,
                             m_gl->DescribePage(word >> 11u).c_str());
            }
            const GSTex0Reg &t = c.tex0;
            std::fprintf(stderr,
                         "[gs-compare] WATCH (%d,%d) fbp %03x diverged at draw #%zu: before gl %08x cpu %08x, after gl %08x cpu %08x; prim %d tme %d "
                         "fst %d abe %d iip %d box %.1f,%.1f-%.1f,%.1f tex tbp %04x tbw %u psm %02x %ux%u tcc %u tfx %u cbp %04x cpsm %02x csa %u "
                         "clamp %llx alpha %llx test %llx fbmsk %08x fpsm %02x zbp %03x zmsk %d colclamp %llx dthe %llx pabe %d scissor %d,%d-%d,%d fbw %u\n",
                         hit[i].first, hit[i].second, fbp, m_frameDraws.size(), gl0[i], cpu0[i], gl1[i], cpu1[i],
                         static_cast<int>(batch.state.prim.type), batch.state.prim.tme ? 1 : 0, batch.state.prim.fst ? 1 : 0,
                         batch.state.prim.abe ? 1 : 0, batch.state.prim.iip ? 1 : 0, minX, minY, maxX, maxY, t.tbp0, t.tbw, t.psm,
                         static_cast<unsigned>(batch.state.textureWidth), static_cast<unsigned>(batch.state.textureHeight), t.tcc, t.tfx, t.cbp,
                         t.cpsm, t.csa, (unsigned long long)c.clamp, (unsigned long long)c.alpha, (unsigned long long)c.test, c.frame.fbmsk,
                         c.frame.psm, c.zbuf.zbp, c.zbuf.zmask ? 1 : 0, (unsigned long long)batch.state.colclamp,
                         (unsigned long long)batch.state.dthe, batch.state.pabe ? 1 : 0, static_cast<int>(c.scissor.x0), static_cast<int>(c.scissor.y0),
                         static_cast<int>(c.scissor.x1), static_cast<int>(c.scissor.y1), c.frame.fbw);
            if (batch.state.prim.tme && batch.state.prim.fst && batch.state.prim.type == GS_PRIM_SPRITE && batch.vertexCount >= 2u &&
                (t.psm == GS_PSM_CT32 || t.psm == GS_PSM_CT24))
            {
                // The texels this pixel samples, in both local memories.
                const GSVertex &a = batch.vertices[0], &b = batch.vertices[1];
                const float fx = (static_cast<float>(hit[i].first) + 0.5f - (a.x - ofx)) / std::max(1e-6f, (b.x - a.x));
                const float fy = (static_cast<float>(hit[i].second) + 0.5f - (a.y - ofy)) / std::max(1e-6f, (b.y - a.y));
                const int u = static_cast<int>((a.u + fx * (static_cast<float>(b.u) - a.u)) / 16.0f);
                const int v = static_cast<int>((a.v + fy * (static_cast<float>(b.v) - a.v)) / 16.0f);
                const uint32_t tbw = std::max<uint32_t>(t.tbw, 1u);
                m_gl->PeekToLocalMemory(t.tbp0 >> 5u, tbw, t.psm, std::max(u - 1, 0), std::max(v - 1, 0), u + 1, v + 1);
                std::fprintf(stderr, "[gs-compare]   texel ~(%d,%d):", u, v);
                for (int dv = -1; dv <= 1; ++dv)
                    for (int du = -1; du <= 1; ++du)
                    {
                        const uint32_t tu = static_cast<uint32_t>(std::max(u + du, 0)), tv = static_cast<uint32_t>(std::max(v + dv, 0));
                        std::fprintf(stderr, " %08x/%08x", GSMem::ReadCT32(m_vram, t.tbp0, tbw, tu, tv),
                                     GSMem::ReadCT32(m_shadowVram.data(), t.tbp0, tbw, tu, tv));
                    }
                std::fprintf(stderr, " (gl/cpu)\n");
            }
            if (batch.state.prim.tme && batch.state.prim.fst && batch.state.prim.type == GS_PRIM_SPRITE && batch.vertexCount >= 2u &&
                (t.psm == GS_PSM_T8 || t.psm == GS_PSM_T8H))
            {
                // The index this pixel samples in both local memories, and
                // the CLUT entry each renderer maps it through.
                const GSVertex &a = batch.vertices[0], &b = batch.vertices[1];
                const float fx = (static_cast<float>(hit[i].first) + 0.5f - (a.x - ofx)) / std::max(1e-6f, (b.x - a.x));
                const float fy = (static_cast<float>(hit[i].second) + 0.5f - (a.y - ofy)) / std::max(1e-6f, (b.y - a.y));
                const uint32_t u = static_cast<uint32_t>(std::max(0.0f, (a.u + fx * (static_cast<float>(b.u) - a.u)) / 16.0f));
                const uint32_t v = static_cast<uint32_t>(std::max(0.0f, (a.v + fy * (static_cast<float>(b.v) - a.v)) / 16.0f));
                const uint32_t tbw = std::max<uint32_t>(t.tbw, 1u);
                m_gl->PeekToLocalMemory(t.tbp0 >> 5u, tbw, t.psm, static_cast<int>(u), static_cast<int>(v), static_cast<int>(u), static_cast<int>(v));
                const bool high = t.psm == GS_PSM_T8H;
                const uint32_t ig = high ? GSMem::ReadP8H(m_vram, t.tbp0, tbw, u, v) : GSMem::ReadP8(m_vram, t.tbp0, tbw, u, v);
                const uint32_t ic = high ? GSMem::ReadP8H(m_shadowVram.data(), t.tbp0, tbw, u, v) : GSMem::ReadP8(m_shadowVram.data(), t.tbp0, tbw, u, v);
                const uint32_t entry = ((t.csa & 15u) * 16u + ic) & 255u;
                const std::array<uint16_t, 512> &sb = m_shadow.clutBuffer();
                std::fprintf(stderr, "[gs-compare]   index ~(%u,%u) gl %u cpu %u; gl %s; cpu CLUT entry %u = %08x\n", u, v, ig, ic,
                             m_gl->DescribeClutEntry(t, ig).c_str(), entry, sb[entry] | (static_cast<uint32_t>(sb[entry + 256u]) << 16u));
            }
            for (uint32_t v = 0; v < batch.vertexCount && v < 3u; ++v)
            {
                const GSVertex &vx = batch.vertices[v];
                std::fprintf(stderr, "[gs-compare]   v%u xy %.4f,%.4f z %u rgba %u,%u,%u,%u stq %g,%g,%g uv %u,%u\n", v, vx.x - ofx, vx.y - ofy,
                             static_cast<unsigned>(vx.z), vx.r, vx.g, vx.b, vx.a, vx.s, vx.t, vx.q, static_cast<unsigned>(vx.u),
                             static_cast<unsigned>(vx.v));
            }
        }
    }
}
GSCompareBackend::~GSCompareBackend() = default;

void GSCompareBackend::Initialize(uint8_t *vram, uint32_t vramSize)
{
    m_vram = vram;
    m_shadowVram.assign(vram, vram + vramSize);
    m_gl->Initialize(vram, vramSize);
    m_shadow.Initialize(m_shadowVram.data(), vramSize);
}
void GSCompareBackend::Reset()
{
    m_gl->Reset();
    m_shadow.Reset();
}
void GSCompareBackend::Submit(const GSPrimitiveBatch &batch) { submitBoth(batch, false); }
void GSCompareBackend::SubmitSameState(const GSPrimitiveBatch &batch) { submitBoth(batch, true); }
void GSCompareBackend::submitBoth(const GSPrimitiveBatch &batch, bool sameState)
{
    if (m_frameDraws.size() < 200000u)
    {
        m_frameDraws.push_back(batch);
        m_frameDrawEvents.push_back(m_events);
    }
    if (!m_memWatch.empty())
        m_memWatchBefore = m_gl->DescribePage(m_memWatch.front().word >> 11u);
    // GT3_GS_COMPARE_WATCH_FROM=<tick>: no pixel watch before that VSync (it reads back per draw).
    static const uint64_t watchFrom = [] {
        const char *v = std::getenv("GT3_GS_COMPARE_WATCH_FROM");
        return (v && *v) ? std::strtoull(v, nullptr, 10) : 0ull;
    }();
    if (!m_watch.empty() && m_lastTick >= watchFrom)
        watchDraw(batch);
    else
    {
        if (sameState)
            m_gl->SubmitSameState(batch);
        else
            m_gl->Submit(batch);
        m_shadow.Submit(batch);
    }
    if (!m_memWatch.empty())
    {
        const GSContext &c = batch.state.context;
        const float ofx = static_cast<float>(c.xyoffset.ofx) / 16.0f, ofy = static_cast<float>(c.xyoffset.ofy) / 16.0f;
        float minX = 1e9f, minY = 1e9f, maxX = -1e9f, maxY = -1e9f;
        for (uint32_t v = 0; v < batch.vertexCount && v < 3u; ++v)
        {
            minX = std::min(minX, batch.vertices[v].x - ofx);
            maxX = std::max(maxX, batch.vertices[v].x - ofx);
            minY = std::min(minY, batch.vertices[v].y - ofy);
            maxY = std::max(maxY, batch.vertices[v].y - ofy);
        }
        const int x0 = std::max(static_cast<int>(minX) - 1, static_cast<int>(c.scissor.x0));
        const int y0 = std::max(static_cast<int>(minY) - 1, static_cast<int>(c.scissor.y0));
        const int x1 = std::min(static_cast<int>(maxX) + 1, static_cast<int>(c.scissor.x1));
        const int y1 = std::min(static_cast<int>(maxY) + 1, static_cast<int>(c.scissor.y1));
        if (x1 >= x0 && y1 >= y0)
        {
            std::vector<uint32_t> pages;
            forRectPages(c.frame.fbp & 0x1FFu, false, std::max<uint32_t>(c.frame.fbw, 1u), c.frame.psm, x0, y0, x1, y1,
                         [&](uint32_t page) { pages.push_back(page); });
            forRectPages(c.zbuf.zbp & 0x1FFu, false, std::max<uint32_t>(c.frame.fbw, 1u), c.zbuf.psm | 0x30u, x0, y0, x1, y1,
                         [&](uint32_t page) { pages.push_back(page); });
            char event[256];
            std::snprintf(event, sizeof(event), "draw #%zu prim %d fbp %03x/%u fpsm %02x fbmsk %08x zbp %03x zpsm %02x zmsk %d test %llx box %d,%d-%d,%d tme %d tbp %04x psm %02x abe %d alpha %llx",
                          m_frameDraws.size(), static_cast<int>(batch.state.prim.type), c.frame.fbp, c.frame.fbw, c.frame.psm, c.frame.fbmsk,
                          c.zbuf.zbp, c.zbuf.psm, c.zbuf.zmask ? 1 : 0, (unsigned long long)c.test, x0, y0, x1, y1,
                          batch.state.prim.tme ? 1 : 0, c.tex0.tbp0, c.tex0.psm, batch.state.prim.abe ? 1 : 0, (unsigned long long)c.alpha);
            checkMemWatch(event, pages);
        }
    }
}
void GSCompareBackend::LoadClut(const GSTex0Reg &tex, const GSTexClutReg &texclut)
{
    ++m_events;
    m_gl->LoadClut(tex, texclut);
    m_shadow.LoadClut(tex, texclut);
}
void GSCompareBackend::BeginTransfer(const GSTransferCommand &command)
{
    ++m_events;
    m_lastTransfer = command;
    m_gl->BeginTransfer(command);
    m_shadow.BeginTransfer(command);
    if (!m_memWatch.empty() && command.direction == 2u)
    {
        const auto &b = command.bitbltbuf;
        const auto &p = command.trxpos;
        const auto &r = command.trxreg;
        std::vector<uint32_t> pages;
        forRectPages(b.dbp >> 5u, (b.dbp & 31u) != 0u, std::max<uint32_t>(b.dbw, 1u), b.dpsm, p.dsax, p.dsay, p.dsax + r.rrw - 1, p.dsay + r.rrh - 1,
                     [&](uint32_t page) { pages.push_back(page); });
        char event[160];
        std::snprintf(event, sizeof(event), "local->local transfer sbp %04x/%u spsm %02x -> dbp %04x/%u dpsm %02x %ux%u at %u,%u", b.sbp, b.sbw,
                      b.spsm, b.dbp, b.dbw, b.dpsm, r.rrw, r.rrh, p.dsax, p.dsay);
        checkMemWatch(event, pages);
    }
}
void GSCompareBackend::UploadImage(const uint8_t *data, uint32_t sizeBytes)
{
    m_gl->UploadImage(data, sizeBytes);
    m_shadow.UploadImage(data, sizeBytes);
    if (!m_memWatch.empty())
    {
        const auto &b = m_lastTransfer.bitbltbuf;
        const auto &p = m_lastTransfer.trxpos;
        const auto &r = m_lastTransfer.trxreg;
        std::vector<uint32_t> pages;
        forRectPages(b.dbp >> 5u, (b.dbp & 31u) != 0u, std::max<uint32_t>(b.dbw, 1u), b.dpsm, p.dsax, p.dsay, p.dsax + r.rrw - 1, p.dsay + r.rrh - 1,
                     [&](uint32_t page) { pages.push_back(page); });
        char event[160];
        std::snprintf(event, sizeof(event), "upload dbp %04x/%u dpsm %02x %ux%u at %u,%u (%u bytes)", b.dbp, b.dbw, b.dpsm, r.rrw, r.rrh, p.dsax,
                      p.dsay, sizeBytes);
        checkMemWatch(event, pages);
    }
}
void GSCompareBackend::Flush()
{
    m_gl->Flush();
    m_shadow.Flush();
}
void GSCompareBackend::TextureFlush()
{
    m_gl->TextureFlush();
    m_shadow.TextureFlush();
}
void GSCompareBackend::Sync(GSSyncReason reason)
{
    m_gl->Sync(reason);
    m_shadow.Sync(reason);
}
PresentationFrame GSCompareBackend::Present(const GSPresentationRequest &request)
{
    PresentationFrame frame = m_gl->Present(request);
    m_lastTick = request.vsyncTick;
    static const bool presentCheck = envFlag("GT3_GS_COMPARE_MEMWATCH_PRESENT", false);
    if (!m_memWatch.empty() && presentCheck)
    {
        std::vector<uint32_t> pages;
        for (const MemWatch &w : m_memWatch)
            pages.push_back(w.word >> 11u);
        char event[160];
        std::snprintf(event, sizeof(event), "present dispfb1 %llx dispfb2 %llx pmode %llx smode2 %llx", (unsigned long long)request.dispfb1,
                      (unsigned long long)request.dispfb2, (unsigned long long)request.pmode, (unsigned long long)request.smode2);
        checkMemWatch(event, pages);
    }
    compare(request);
    m_prevDraws.swap(m_frameDraws);
    m_frameDraws.clear();
    m_frameDrawEvents.clear();
    return frame;
}
bool GSCompareBackend::ClearFramebuffer(const GSContext &context, uint32_t rgba)
{
    m_shadow.ClearFramebuffer(context, rgba);
    return m_gl->ClearFramebuffer(context, rgba);
}
uint32_t GSCompareBackend::ConsumeLocalToHostBytes(uint8_t *dst, uint32_t maxBytes)
{
    std::vector<uint8_t> discard(maxBytes);
    m_shadow.ConsumeLocalToHostBytes(discard.data(), maxBytes);
    return m_gl->ConsumeLocalToHostBytes(dst, maxBytes);
}
uint32_t GSCompareBackend::ReadVram(uint32_t psm, uint32_t base, uint32_t bw, uint32_t x, uint32_t y) const
{
    return m_gl->ReadVram(psm, base, bw, x, y);
}
void GSCompareBackend::WriteVram(uint32_t psm, uint32_t base, uint32_t bw, uint32_t x, uint32_t y, uint32_t value)
{
    m_gl->WriteVram(psm, base, bw, x, y, value);
    m_shadow.WriteVram(psm, base, bw, x, y, value);
}
void GSCompareBackend::SnapshotVram(std::vector<uint8_t> &out) const { m_gl->SnapshotVram(out); }
GSTransferSnapshot GSCompareBackend::GetTransferSnapshot() const { return m_gl->GetTransferSnapshot(); }
bool GSCompareBackend::IdleWork() { return m_gl->IdleWork(); }

void GSCompareBackend::compare(const GSPresentationRequest &request)
{
    // The displayed buffer: the circuit whose setup is valid (as gpuPresent).
    auto valid = [](uint64_t display, uint64_t dispfb) {
        return ((dispfb >> 9) & 0x3Fu) != 0u || ((display >> 32) & 0xFFFu) != 0u;
    };
    uint64_t dispfb = 0, display = 0;
    if ((request.pmode & 2u) && valid(request.display2, request.dispfb2))
    {
        dispfb = request.dispfb2;
        display = request.display2;
    }
    else if ((request.pmode & 1u) && valid(request.display1, request.dispfb1))
    {
        dispfb = request.dispfb1;
        display = request.display1;
    }
    else
        return;
    const uint32_t fbp = static_cast<uint32_t>(dispfb & 0x1FFu), fbw = static_cast<uint32_t>((dispfb >> 9) & 0x3Fu);
    const uint32_t psm = static_cast<uint32_t>((dispfb >> 15) & 0x1Fu);
    const int ox = static_cast<int>((dispfb >> 32) & 0x7FFu), oy = static_cast<int>((dispfb >> 43) & 0x7FFu);
    // PSMCT16 (GT4's 480p / 1080i display buffers): RGB555 + A compared,
    // shown as 8 bits per channel (v << 3) in the report and the dumps.
    // GT4's race colour 32 (gs_fullcolour.h): each pixel's full colour F
    // (both renderers' planes) compared as 32-bit words instead; the report
    // counts the pixels whose 16-bit value differs, those that differ only in
    // the full colour, and per renderer the shown pixels that are not full
    // colour (no full-colour draw wrote them since their 16-bit value last
    // changed; expected 0 in a race), and the dumps show F.
    const bool c16 = psm == GS_PSM_CT16;
    const bool fcShown = c16 && gsfc::shows(psm, request.crt);
    if (fbw == 0u || (psm != GS_PSM_CT32 && psm != GS_PSM_CT24 && !c16))
        return;
    const uint32_t magh = static_cast<uint32_t>((display >> 23) & 0xFu);
    uint32_t width = (static_cast<uint32_t>((display >> 32) & 0xFFFu) + 1u) / (magh + 1u);
    uint32_t height = static_cast<uint32_t>((display >> 44) & 0x7FFu) + 1u;
    if ((request.smode2 & 3u) == 3u)
        height = (height + 1u) / 2u; // interlaced frame mode: half-height fields
    width = std::min(width, fbw * 64u);
    height = std::min(height, 1024u);
    m_gl->PeekToLocalMemory(fbp, fbw, psm, ox, oy, ox + static_cast<int>(width) - 1, oy + static_cast<int>(height) - 1);
    const uint32_t mask = psm == GS_PSM_CT24 ? 0x00FFFFFFu : 0xFFFFFFFFu;
    const uint32_t *const glPlane = fcShown ? m_gl->cpuCopy().FullColourPlane() : nullptr;
    const uint32_t *const cpuPlane = fcShown ? m_shadow.FullColourPlane() : nullptr;
    const auto readShown = [&](uint8_t *mem, uint32_t gx, uint32_t gy) -> uint32_t {
        if (fcShown)
        {
            const uint32_t m16 = GSMem::ReadCT16(mem, fbp * 32u, fbw, gx, gy) & 0xFFFFu;
            const uint32_t *plane = mem == m_vram ? glPlane : cpuPlane;
            return plane ? gsfc::full(plane[gsfc::halfIndex(GS_PSM_CT16, fbp * 32u, fbw, gx, gy)], m16) : gsfc::expand(m16);
        }
        return c16 ? rgba5551To8888(GSMem::ReadCT16(mem, fbp * 32u, fbw, gx, gy) & 0xFFFFu) : GSMem::ReadCT32(mem, fbp * 32u, fbw, gx, gy) & mask;
    };
    // (full colour) pixels whose 16-bit values differ / only the full colour; shown pixels not full colour.
    uint64_t differ16 = 0, differFull = 0, notFullGl = 0, notFullCpu = 0;
    if (fcShown)
        for (uint32_t y = 0; y < height; ++y)
            for (uint32_t x = 0; x < width; ++x)
            {
                const uint32_t gx = static_cast<uint32_t>(ox) + x, gy = static_cast<uint32_t>(oy) + y;
                const uint32_t h = gsfc::halfIndex(GS_PSM_CT16, fbp * 32u, fbw, gx, gy);
                const uint32_t a16 = GSMem::ReadCT16(m_vram, fbp * 32u, fbw, gx, gy) & 0xFFFFu;
                const uint32_t b16 = GSMem::ReadCT16(m_shadowVram.data(), fbp * 32u, fbw, gx, gy) & 0xFFFFu;
                const bool aFull = glPlane && gsfc::valid(glPlane[h], a16), bFull = cpuPlane && gsfc::valid(cpuPlane[h], b16);
                notFullGl += aFull ? 0u : 1u;
                notFullCpu += bFull ? 0u : 1u;
                if (a16 != b16)
                    ++differ16;
                else if ((aFull ? glPlane[h] : gsfc::expand(a16)) != (bFull ? cpuPlane[h] : gsfc::expand(b16)))
                    ++differFull;
            }
    uint64_t differing = 0;
    int maxDiff = 0, firstX = -1, firstY = -1, bigX = -1, bigY = -1;
    uint32_t firstGl = 0, firstCpu = 0, bigGl = 0, bigCpu = 0;
    for (uint32_t y = 0; y < height; ++y)
        for (uint32_t x = 0; x < width; ++x)
        {
            const uint32_t gx = static_cast<uint32_t>(ox) + x, gy = static_cast<uint32_t>(oy) + y;
            const uint32_t a = readShown(m_vram, gx, gy);
            const uint32_t b = readShown(m_shadowVram.data(), gx, gy);
            if (a == b)
                continue;
            ++differing;
            int pixelDiff = 0;
            for (int c = 0; c < 4; ++c)
                pixelDiff = std::max(pixelDiff, std::abs(static_cast<int>((a >> (c * 8)) & 0xFFu) - static_cast<int>((b >> (c * 8)) & 0xFFu)));
            maxDiff = std::max(maxDiff, pixelDiff);
            if (pixelDiff >= 16 && bigX < 0)
            {
                bigX = static_cast<int>(x);
                bigY = static_cast<int>(y);
                bigGl = a;
                bigCpu = b;
            }
            if (firstX < 0)
            {
                firstX = static_cast<int>(x);
                firstY = static_cast<int>(y);
                firstGl = a;
                firstCpu = b;
            }
        }
    ++m_frames;
    if (differing)
        ++m_differingFrames;
    if (differing || (m_frames % 60u) == 0u)
        std::fprintf(stderr, "[gs-compare] tick %llu fbp %03x: %llu of %u pixels differ (max channel diff %d)%s first (%d,%d) gl %08x cpu %08x; %llu/%llu frames differ\n",
                     (unsigned long long)request.vsyncTick, fbp, (unsigned long long)differing, width * height, maxDiff,
                     differing ? "" : " -", firstX, firstY, firstGl, firstCpu, (unsigned long long)m_differingFrames,
                     (unsigned long long)m_frames);
    if (fcShown)
    {
        ++m_fcFrames;
        if (differFull)
            ++m_fcOnlyFrames;
        if (notFullGl || notFullCpu)
            ++m_fcPartialFrames;
        if (differing || notFullGl || notFullCpu || (m_fcFrames % 60u) == 0u)
            std::fprintf(stderr,
                         "[gs-compare] tick %llu fbp %03x full colour (%d-bit): %llu pixels differ in 16 bits, %llu only in the full colour; "
                         "shown pixels not full colour: gl %llu cpu %llu; frames: %llu/%llu with full-colour-only differences, %llu with pixels not full colour\n",
                         (unsigned long long)request.vsyncTick, fbp, gsfc::mode(), (unsigned long long)differ16, (unsigned long long)differFull,
                         (unsigned long long)notFullGl, (unsigned long long)notFullCpu, (unsigned long long)m_fcOnlyFrames,
                         (unsigned long long)m_fcFrames, (unsigned long long)m_fcPartialFrames);
    }
    // The draws of this frame over the first strongly differing pixel
    // (GT3_GS_COMPARE_REPORT_MIN=<n>: only frames with n differing pixels).
    static const uint64_t reportMin = [] {
        const char *v = std::getenv("GT3_GS_COMPARE_REPORT_MIN");
        return (v && *v) ? std::strtoull(v, nullptr, 10) : 0ull;
    }();
    // GT3_GS_COMPARE_REPORTS=<n>: at most n draw reports per run (default 12).
    static const uint64_t reportCap = [] {
        const char *v = std::getenv("GT3_GS_COMPARE_REPORTS");
        return (v && *v) ? std::strtoull(v, nullptr, 10) : 12ull;
    }();
    if (bigX >= 0 && m_drawReports < reportCap && differing >= reportMin)
    {
        ++m_drawReports;
        const int px = ox + bigX, py = oy + bigY;
        std::fprintf(stderr, "[gs-compare] tick %llu pixel (%d,%d) of fbp %03x: gl %08x cpu %08x; draws over it (of %zu):\n",
                     (unsigned long long)request.vsyncTick, px, py, fbp, bigGl, bigCpu, m_frameDraws.size());
        int shown = 0;
        // This frame's draws, then the previous frame's (the displayed buffer
        // may have been drawn before the last presentation).
        std::vector<const GSPrimitiveBatch *> all;
        for (const GSPrimitiveBatch &b : m_prevDraws)
            all.push_back(&b);
        for (const GSPrimitiveBatch &b : m_frameDraws)
            all.push_back(&b);
        for (size_t i = all.size(); i-- > 0 && shown < 24;)
        {
            const GSPrimitiveBatch &b = *all[i];
            const GSContext &c = b.state.context;
            if ((c.frame.fbp & 0x1FFu) != fbp)
                continue;
            const float ofx = static_cast<float>(c.xyoffset.ofx) / 16.0f, ofy = static_cast<float>(c.xyoffset.ofy) / 16.0f;
            float minX = 1e9f, minY = 1e9f, maxX = -1e9f, maxY = -1e9f;
            for (uint32_t v = 0; v < b.vertexCount && v < 3u; ++v)
            {
                minX = std::min(minX, b.vertices[v].x - ofx);
                maxX = std::max(maxX, b.vertices[v].x - ofx);
                minY = std::min(minY, b.vertices[v].y - ofy);
                maxY = std::max(maxY, b.vertices[v].y - ofy);
            }
            if (px < minX - 1.0f || px > maxX + 1.0f || py < minY - 1.0f || py > maxY + 1.0f)
                continue;
            ++shown;
            const GSTex0Reg &t = c.tex0;
            std::fprintf(stderr,
                         "[gs-compare]   #%zu (events %u) prim %d tme %d fst %d abe %d fge %d box %.1f,%.1f-%.1f,%.1f tex tbp %04x tbw %u psm %02x %ux%u "
                         "tcc %u tfx %u cbp %04x cpsm %02x csm %u csa %u clamp %llx alpha %llx test %llx fbmsk %08x fpsm %02x zbp %03x zpsm %02x zmsk %d "
                         "fba %llx texa %u/%u/%u linear %d mip %02x/%u..%u tex1 %llx\n",
                         i, i < m_prevDraws.size() ? 0u : m_frameDrawEvents[i - m_prevDraws.size()], static_cast<int>(b.state.prim.type), b.state.prim.tme ? 1 : 0, b.state.prim.fst ? 1 : 0,
                         b.state.prim.abe ? 1 : 0, b.state.prim.fge ? 1 : 0, minX, minY, maxX, maxY, t.tbp0, t.tbw, t.psm,
                         static_cast<unsigned>(b.state.textureWidth), static_cast<unsigned>(b.state.textureHeight), t.tcc, t.tfx, t.cbp, t.cpsm,
                         t.csm, t.csa, (unsigned long long)c.clamp, (unsigned long long)c.alpha, (unsigned long long)c.test, c.frame.fbmsk,
                         c.frame.psm, c.zbuf.zbp, c.zbuf.psm, c.zbuf.zmask ? 1 : 0, (unsigned long long)c.fba, b.state.texa.ta0,
                         b.state.texa.aem ? 1u : 0u, b.state.texa.ta1, b.state.linearFilter ? 1 : 0, b.state.mipTag, b.state.mipLo,
                         b.state.mipHi, (unsigned long long)c.tex1);
        }
    }
    static const char *dumpPrefix = std::getenv("GT3_GS_COMPARE_DUMP");
    // Dump frames that differ visibly (GT3_GS_COMPARE_DUMP_MIN pixels, default 1000, by 16 or more).
    static const uint64_t dumpMin = [] {
        const char *v = std::getenv("GT3_GS_COMPARE_DUMP_MIN");
        return (v && *v) ? std::strtoull(v, nullptr, 10) : 1000ull;
    }();
    // GT3_GS_COMPARE_DUMP_FROM=<tick>: none before that VSync.
    static const uint64_t dumpFrom = [] {
        const char *v = std::getenv("GT3_GS_COMPARE_DUMP_FROM");
        return (v && *v) ? std::strtoull(v, nullptr, 10) : 0ull;
    }();
    if (differing >= dumpMin && maxDiff >= 16 && dumpPrefix && *dumpPrefix && m_dumps < 8u && request.vsyncTick >= dumpFrom)
    {
        ++m_dumps;
        for (int which = 0; which < 2; ++which)
        {
            char path[512];
            std::snprintf(path, sizeof(path), "%s-%06llu-%s.ppm", dumpPrefix, (unsigned long long)request.vsyncTick, which ? "cpu" : "gl");
            if (FILE *f = std::fopen(path, "wb"))
            {
                std::fprintf(f, "P6\n%u %u\n255\n", width, height);
                const uint8_t *mem = which ? m_shadowVram.data() : m_vram;
                for (uint32_t y = 0; y < height; ++y)
                    for (uint32_t x = 0; x < width; ++x)
                    {
                        const uint32_t v = readShown(const_cast<uint8_t *>(mem), static_cast<uint32_t>(ox) + x, static_cast<uint32_t>(oy) + y);
                        const uint8_t rgb[3] = {static_cast<uint8_t>(v), static_cast<uint8_t>(v >> 8), static_cast<uint8_t>(v >> 16)};
                        std::fwrite(rgb, 1, 3, f);
                    }
                std::fclose(f);
            }
        }
        // GT3_GS_COMPARE_DUMP_FBP=<fbp>/<fbw>/<rows>[,...] (hex fbp): the same
        // frame's other buffers, both renderers, read as CT32 into RGBA PAMs (to
        // find which stage of a chain of targets diverged first;
        // analysis/phase5/specs/step4-picture.md 5, D1). Shows each buffer's
        // state at this presentation.
        static const std::vector<std::array<uint32_t, 3>> extraDumps = [] {
            std::vector<std::array<uint32_t, 3>> out;
            if (const char *v = std::getenv("GT3_GS_COMPARE_DUMP_FBP"))
                for (const char *p = v; *p;)
                {
                    unsigned f = 0, w = 0, h = 0;
                    int n = 0;
                    if (std::sscanf(p, "%x/%u/%u%n", &f, &w, &h, &n) != 3 || n <= 0)
                        break;
                    out.push_back({f & 0x1FFu, std::clamp(w, 1u, 32u), std::min(h, 2048u)});
                    p += n;
                    if (*p == ',')
                        ++p;
                }
            return out;
        }();
        for (const auto &[xf, xw, xh] : extraDumps)
        {
            if (xh == 0u)
                continue;
            m_gl->PeekToLocalMemory(xf, xw, GS_PSM_CT32, 0, 0, static_cast<int>(xw * 64u) - 1, static_cast<int>(xh) - 1);
            for (int which = 0; which < 2; ++which)
            {
                char path[512];
                std::snprintf(path, sizeof(path), "%s-%06llu-%03x-%s.pam", dumpPrefix, (unsigned long long)request.vsyncTick, xf,
                              which ? "cpu" : "gl");
                if (FILE *f = std::fopen(path, "wb"))
                {
                    std::fprintf(f, "P7\nWIDTH %u\nHEIGHT %u\nDEPTH 4\nMAXVAL 255\nTUPLTYPE RGB_ALPHA\nENDHDR\n", xw * 64u, xh);
                    const uint8_t *mem = which ? m_shadowVram.data() : m_vram;
                    for (uint32_t y = 0; y < xh; ++y)
                        for (uint32_t x = 0; x < xw * 64u; ++x)
                        {
                            const uint32_t v = GSMem::ReadCT32(const_cast<uint8_t *>(mem), xf * 32u, xw, x, y);
                            std::fwrite(&v, 1, 4, f);
                        }
                    std::fclose(f);
                }
            }
        }
    }
}
