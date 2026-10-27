#include "gt3_aspect.h"

#include "ps2_runtime.h"
#include "runtime/gs/gs_frontend.h"
#if !defined(GT3_STOCK_BUILD)
#include "gt3_camera.h"
#endif

#include <algorithm>
#include <atomic>
#include <cmath>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <map>
#include <mutex>
#include <string>
#include <tuple>

namespace gt3aspect
{
    namespace
    {
        constexpr uint32_t kFlag = 0x2f424cu;            // display block 0x2f4230 + 0x1c: 16:9
        constexpr uint32_t kScale = 0x257f04u;           // scale the current VU0 matrix (f12, f13, f14)
        constexpr uint32_t kTextScale = 0x18a938u;       // returns 1.0, or 0.75 when the flag is set
        constexpr uint32_t kRaceCamera = 0x17e740u;      // the race camera
        constexpr uint32_t kRaceCameraReturn = 0x17e8acu; // after its jal 0x257f04 (at 0x17e8a4)
        constexpr uint32_t kVblankCount = 0x4a69c0u;     // GT3's VBlank counter (handler 0x25a360)
        constexpr uint32_t kRaceHold = 10u;              // VBlanks the wide picture outlasts the race camera
        float g_aspect = 0.0f, g_factor = 1.0f;
        // GT3_FOV_ZOOM=<k>: the race camera's view narrowed k times (both
        // axes; 1 = GT3's own 60 x 47.8 degrees at 4:3). For triple screens
        // with the field of view matched to the seat (the launcher works k
        // out from the screen size and the distance).
        float g_zoom = 1.0f;
        bool g_active = false, g_coreOk = false, g_hudSqueezed = false;
        bool g_gameWide = false; // wide shapes through GT3's own Screen Size option (install)
        float g_hudFactor = 1.0f; // the race HUD / mirror squeeze (install)
        std::atomic<bool> g_raceSeen{false};
        bool g_inRaceCamera = false;     // raceCameraHook's call is under way (the flag is set)
        bool g_flagPending = false;      // raceCameraCalled set the flag; the scale puts it back
        uint32_t g_flagSaved = 0u;
        std::atomic<uint32_t> g_raceVblank{0u};
        PS2Runtime::RecompiledFunction g_scale = nullptr, g_textScale = nullptr, g_raceCamera = nullptr;

        uint32_t word(const uint8_t *rdram, uint32_t address)
        {
            uint32_t v;
            std::memcpy(&v, rdram + address, 4);
            return v;
        }

        float parseAspect(const char *v)
        {
            if (!v || !*v)
                return 0.0f;
            const std::string s(v);
            const size_t colon = s.find(':');
            if (colon != std::string::npos)
            {
                const float w = std::strtof(s.substr(0, colon).c_str(), nullptr), h = std::strtof(s.substr(colon + 1).c_str(), nullptr);
                return h > 0.0f ? w / h : 0.0f;
            }
            return std::strtof(s.c_str(), nullptr);
        }

        uint32_t returnAddress(const R5900Context *ctx)
        {
            return static_cast<uint32_t>(_mm_extract_epi32(ctx->r[31], 0));
        }

#if !defined(GT3_STOCK_BUILD)
        // GT3_MENU_PAGE_LOG=1 (diagnostic): every menu page GT3 sets by name,
        // 0x1c7338(a0 = menu + 8, a1 = name) -> 1 / 0, and, when the name is not
        // in the menu's page table (GT3 then shows "cannot set page [name]",
        // 0x1b673c), the names the table holds: [a0] = resource, [res] = count,
        // [res + 4] = table of pointers to records that start with the name
        // (binary search 0x1c7988).
        constexpr uint32_t kSetPage = 0x1c7338u;
        PS2Runtime::RecompiledFunction g_setPage = nullptr;
        std::string guestString(const uint8_t *rdram, uint32_t address)
        {
            std::string s;
            for (uint32_t i = 0; i < 64 && address + i < 0x2000000u && rdram[address + i]; ++i)
                s += static_cast<char>(rdram[address + i]);
            return s;
        }
        void setPageHook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            const uint32_t object = static_cast<uint32_t>(_mm_extract_epi32(ctx->r[4], 0)) & 0x1ffffffu;
            const uint32_t name = static_cast<uint32_t>(_mm_extract_epi32(ctx->r[5], 0)) & 0x1ffffffu;
            const uint32_t caller = returnAddress(ctx);
            const std::string wanted = guestString(rdram, name);
            g_setPage(rdram, ctx, runtime);
            const uint32_t ok = static_cast<uint32_t>(_mm_extract_epi32(ctx->r[2], 0));
            const uint32_t res = object + 4u <= 0x2000000u ? word(rdram, object) & 0x1ffffffu : 0u;
            const uint32_t count = res && res + 8u <= 0x2000000u ? word(rdram, res) : 0u;
            const uint32_t table = res && res + 8u <= 0x2000000u ? word(rdram, res + 4u) & 0x1ffffffu : 0u;
            std::fprintf(stderr, "[menu-page] %s \"%s\" (menu 0x%x, resource 0x%x: %u pages at 0x%x; from 0x%x)\n", ok ? "set" : "NOT FOUND",
                         wanted.c_str(), object, res, count, table, caller);
            if (!ok && count && count < 1000u && table + 4u * count <= 0x2000000u)
            {
                std::string names;
                for (uint32_t k = 0; k < count; ++k)
                    names += (k ? ", " : "") + guestString(rdram, word(rdram, table + 4u * k) & 0x1ffffffu);
                std::fprintf(stderr, "[menu-page]   the table: %s\n", names.c_str());
            }
        }
#endif

#if !defined(GT3_STOCK_BUILD)
        // GT3_ASPECT_TRACE=1: the callers of the scale (return address, a0,
        // f12) every 5 s; how the race and mirror cameras were told apart.
        void trace(const R5900Context *ctx)
        {
            static const bool on = std::getenv("GT3_ASPECT_TRACE") != nullptr;
            if (!on)
                return;
            static std::mutex m;
            static std::map<std::tuple<uint32_t, uint32_t, float>, uint32_t> sites;
            static auto last = std::chrono::steady_clock::now();
            std::lock_guard<std::mutex> lock(m);
            ++sites[{returnAddress(ctx), static_cast<uint32_t>(_mm_extract_epi32(ctx->r[4], 0)), ctx->f[12]}];
            if (std::chrono::steady_clock::now() - last >= std::chrono::seconds(5))
            {
                last = std::chrono::steady_clock::now();
                std::cerr << "[aspect-trace]";
                for (const auto &[k, n] : sites)
                    std::cerr << std::hex << " ra=" << std::get<0>(k) << "/a0=" << std::get<1>(k) << std::dec << "/f12=" << std::get<2>(k) << "x" << n;
                std::cerr << std::endl;
                sites.clear();
            }
        }

        // The race camera sees GT3's 16:9 flag set (only during this call; the
        // saved Screen Size option is left alone), so it widens its view.
        void raceCameraHook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            uint32_t saved;
            std::memcpy(&saved, rdram + kFlag, 4);
            const uint32_t one = 1u;
            std::memcpy(rdram + kFlag, &one, 4);
            g_inRaceCamera = true;
            g_raceCamera(rdram, ctx, runtime);
            g_inRaceCamera = false;
            std::memcpy(rdram + kFlag, &saved, 4);
            g_raceVblank.store(word(rdram, kVblankCount), std::memory_order_relaxed);
            g_raceSeen.store(true, std::memory_order_relaxed);
        }

        // The race camera's 0.75 becomes (4/3) / aspect. Any other 0.75 (GT3's
        // own 16:9 option: the mirror, other cameras) becomes 1: the picture's
        // shape comes from GT3_ASPECT alone.
        void scaleHook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            trace(ctx);
            if (gt3camera::sideScreen() != 0)
            {
                // An angled side screen drawn through the mirror render: its
                // rectangle's scale (return 0x180538) becomes the race camera's
                // (so both views have one scale), the mirror flip (return
                // 0x180564) none, GT3's 16:9 0.75 (return 0x1804cc) 1.
                const uint32_t ra = returnAddress(ctx);
                static int sideLog = std::getenv("GT3_SIDE_LOG") ? 40 : 0;
                if (sideLog > 0)
                {
                    --sideLog;
                    std::fprintf(stderr, "[side-scale] side %d ra 0x%x f12 %.3f f13 %.3f f14 %.3f\n", gt3camera::sideScreen(), ra, ctx->f[12], ctx->f[13],
                                 ctx->f[14]);
                }
                if (ra == 0x180538u)
                {
                    // The race camera's scale for the whole picture, times the
                    // picture's width in screens: the rectangle is one screen.
                    // GT3_SIDE_XSIGN / GT3_SIDE_ZSIGN (tests): the signs of the
                    // side view's x scale here and of the flip below.
                    static const float xSign = [] {
                        const char *v = std::getenv("GT3_SIDE_XSIGN");
                        return v && *v == '-' ? -1.0f : 1.0f;
                    }();
                    ctx->f[12] = xSign * g_factor * g_zoom * gt3camera::sideScreenWidths();
                    ctx->f[13] = g_zoom;
                    ctx->f[14] = 1.0f;
                }
                else if (ra == 0x180564u)
                {
                    static const float zSign = [] {
                        const char *v = std::getenv("GT3_SIDE_ZSIGN");
                        return v && *v == '-' ? -1.0f : 1.0f;
                    }();
                    ctx->f[12] = ctx->f[13] = 1.0f;
                    ctx->f[14] = zSign;
                }
                else if (ctx->f[12] == 0.75f)
                    ctx->f[12] = 1.0f;
                g_scale(rdram, ctx, runtime);
                return;
            }
            const bool race = returnAddress(ctx) == kRaceCameraReturn;
            if (ctx->f[12] == 0.75f && (race || !g_gameWide))
            {
                // (Angled sides: the main view's rectangle is the middle screen.)
                ctx->f[12] = race ? g_factor * g_zoom * gt3camera::mainRectScale() : 1.0f;
                if (race)
                    ctx->f[13] *= g_zoom; // y (f14, z, stays)
            }
            if (race && g_flagPending)
            {
                std::memcpy(rdram + kFlag, &g_flagSaved, 4);
                g_flagPending = false;
            }
            g_scale(rdram, ctx, runtime);
        }

        void textScaleHook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            g_textScale(rdram, ctx, runtime);
            if (ctx->f[0] == 0.75f)
                ctx->f[0] = 1.0f;
        }
#endif

        // (Replays: GT3 draws them through the same race camera 0x17e740 -
        // ~550 calls per 5 s in `GT3_ASPECT_TRACE` during a replay - so they are
        // widened and shown in the race shape like the race; measured: the
        // replay's front car is 172-177 px of 640 in all modes, 2026-09-30.)
        bool raceActive(const uint8_t *rdram)
        {
            return g_raceSeen.load(std::memory_order_relaxed) &&
                   word(rdram, kVblankCount) - g_raceVblank.load(std::memory_order_relaxed) < kRaceHold;
        }
    }

    void raceCameraCalled(uint8_t *rdram)
    {
        if (!g_active || g_inRaceCamera)
            return;
        if (!g_flagPending)
        {
            std::memcpy(&g_flagSaved, rdram + kFlag, 4);
            g_flagPending = true;
        }
        const uint32_t one = 1u;
        std::memcpy(rdram + kFlag, &one, 4);
        g_raceVblank.store(word(rdram, kVblankCount), std::memory_order_relaxed);
        g_raceSeen.store(true, std::memory_order_relaxed);
    }

    void install(PS2Runtime &runtime, const uint8_t *rdram)
    {
        // The code this relies on (GT3 USA CORE): lw $v0, 0x424c($v1) in the
        // text helper, lui $v0, 0x7000 at the scale entry, and the race
        // camera's flag test (lw $v0, 0x1c($s7)) and jal 0x257f04.
        g_coreOk = word(rdram, kTextScale + 0xc) == 0x8c62424cu && word(rdram, kScale) == 0x3c027000u;
#if defined(GT3_STOCK_BUILD)
        (void)runtime; // the stock build: GT3's own 16:9 option only (displayAspect)
#else
        if (const char *v = std::getenv("GT3_MENU_PAGE_LOG"); v && *v == '1' && word(rdram, kSetPage) == 0x27bdffe0u)
            if ((g_setPage = runtime.lookupFunction(kSetPage)))
            {
                runtime.registerFunction(kSetPage, &setPageHook);
                std::cerr << "[menu-page] logging GT3's menu page changes (GT3_MENU_PAGE_LOG)" << std::endl;
            }
        float aspect = parseAspect(std::getenv("GT3_ASPECT"));
        if (const char *z = std::getenv("GT3_FOV_ZOOM"); z && *z)
            g_zoom = std::clamp(static_cast<float>(std::atof(z)), 0.4f, 4.0f);
        // Wide shapes use GT3's own widescreen (GT3_ASPECT_GAME=0: the old
        // design, GT3's option off, 4:3 menus, the 4:3 HUD squeezed back): its
        // Screen Size option is kept at 16:9 (perFrame), so menus, HUD, results
        // and replays use GT3's own widescreen layouts, shown at 16:9 as on a
        // PS2 with a widescreen TV. At 16:9 that is all (no hooks: GT3 widens
        // its own cameras); wider shapes widen the race camera further and keep
        // GT3's 16:9 HUD and mirror in shape in the middle.
        if (aspect > 16.0f / 9.0f - 0.01f && aspect < 16.0f && g_coreOk)
        {
            const char *game = std::getenv("GT3_ASPECT_GAME");
            g_gameWide = !(game && *game == '0');
            if (g_gameWide && std::fabs(aspect - 16.0f / 9.0f) < 0.01f && std::fabs(g_zoom - 1.0f) < 0.001f)
            {
                std::cerr << "[aspect] 16:9 is GT3's own widescreen: its Screen Size option is kept at 16:9 (menus, HUD, results laid out by GT3)"
                          << std::endl;
                return;
            }
        }
        if (!(aspect > 4.0f / 3.0f + 0.01f && aspect < 16.0f))
        {
            if (std::fabs(g_zoom - 1.0f) < 0.001f)
                return;
            aspect = 4.0f / 3.0f; // the field of view alone: the same path at 4:3
        }
        if (!g_coreOk || word(rdram, 0x17e888u) != 0x8ee2001cu || word(rdram, kRaceCameraReturn - 8u) != 0x0c095fc1u)
        {
            std::cerr << "[aspect] GT3_ASPECT ignored: this CORE differs at 0x18a944 / 0x257f04 / 0x17e888 / 0x17e8a4" << std::endl;
            return;
        }
        g_scale = runtime.lookupFunction(kScale);
        g_textScale = runtime.lookupFunction(kTextScale);
        g_raceCamera = runtime.lookupFunction(kRaceCamera);
        if (!g_scale || !g_textScale || !g_raceCamera)
        {
            std::cerr << "[aspect] GT3_ASPECT ignored: functions not found" << std::endl;
            return;
        }
        runtime.registerFunction(kScale, &scaleHook);
        if (!g_gameWide)
            runtime.registerFunction(kTextScale, &textScaleHook);
        runtime.registerFunction(kRaceCamera, &raceCameraHook);
        g_aspect = aspect;
        g_factor = (4.0f / 3.0f) / aspect;
        // The HUD squeeze: GT3's 16:9 layout into 16:9 in the middle, or (the
        // old design) its 4:3 layout into 4:3.
        g_hudFactor = g_gameWide ? (16.0f / 9.0f) / aspect : g_factor;
        g_active = true;
        // In races the HUD and the rear-view mirror are squeezed back to their
        // 4:3 shape in the middle (GT3_ASPECT_HUD=stretch: stretched).
        const char *hud = std::getenv("GT3_ASPECT_HUD");
        g_hudSqueezed = !(hud && std::strcmp(hud, "stretch") == 0);
        std::cerr << "[aspect] races " << aspect << ":1 (camera x squeeze " << g_factor << "), HUD and mirror "
                  << (g_hudSqueezed ? (g_gameWide ? "GT3's 16:9 layout kept 16:9 in the middle" : "kept 4:3 in the middle") : "stretched")
                  << (g_gameWide ? "; menus GT3's own 16:9 (its Screen Size option kept at 16:9)" : "; menus 4:3");
        if (std::fabs(g_zoom - 1.0f) >= 0.001f)
            std::cerr << "; field of view narrowed " << g_zoom << "x (GT3_FOV_ZOOM)";
        std::cerr << std::endl;
#endif
    }

    void perFrame(uint8_t *rdram)
    {
#if !defined(GT3_STOCK_BUILD)
        if (g_gameWide && rdram)
        {
            // Keep GT3's Screen Size at 16:9 (the memory card's saved option or
            // the Options screen may set 4:3; the launcher's shape wins).
            if (word(rdram, kFlag) != 1u)
            {
                const uint32_t one = 1u;
                std::memcpy(rdram + kFlag, &one, 4);
            }
            if (!g_active)
                return;
        }
#endif
        if (!g_active || !rdram)
            return;
#if !defined(GT3_STOCK_BUILD)
        // GT3_ASPECT_LOG=1: when the picture switches between the race shape
        // and the menu shape (the race camera ran within kRaceHold VBlanks).
        static const bool logShape = std::getenv("GT3_ASPECT_LOG") != nullptr;
        if (logShape)
        {
            static int lastRace = -1;
            const int race = raceActive(rdram) ? 1 : 0;
            if (race != lastRace)
            {
                lastRace = race;
                std::fprintf(stderr, "[aspect] picture %s at GT3 VBlank %u\n", race ? "race shape" : "menu shape", word(rdram, kVblankCount));
            }
        }
#endif
        static float applied = 1.0f;
        const float squeeze = g_hudSqueezed && raceActive(rdram) ? g_hudFactor : 1.0f;
        if (squeeze != applied)
        {
            GS::setHudSqueeze(squeeze);
            applied = squeeze;
        }
    }

    float displayAspect(const uint8_t *rdram)
    {
        if (g_active)
            return rdram && raceActive(rdram) ? g_aspect : (g_gameWide ? 16.0f / 9.0f : 0.0f);
        // GT3's own Screen Size 16:9: show its anamorphic frame as 16:9.
        if (g_coreOk && rdram && word(rdram, kFlag) != 0u)
            return 16.0f / 9.0f;
        return 0.0f;
    }
}
