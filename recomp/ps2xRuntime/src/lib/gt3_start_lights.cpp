#include "gt3_start_lights.h"

#include "gt3_race_info.h"
#include "ps2_runtime.h"
#include "runtime/gs/gs_frontend.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>

namespace gt3startlights
{
    namespace
    {
        constexpr uint32_t kRamMask = 0x01FFFFFFu;
        constexpr uint32_t kStartControl = 0x17a5e8u; // the start controller (see gt3_start_lights.h)
        constexpr uint32_t kAiDriver = 0x1f7ae0u;     // GT3's AI driver (a0 = the car's controller; see gt3_ai_pilot.cpp)
        constexpr uint32_t kVblankCounter = 0x4a69c0u;
        constexpr uint32_t kAsked = 0x50u, kKind = 0x54u, kSeconds = 0x58u, kFrames = 0x5cu;
        constexpr uint32_t kControllerOffset = 0xec8u, kFirstCar = 0xc8u, kCarSize = 0x16dcu;
        constexpr uint32_t kCtlThrottle = 0xa0u, kCtlBrake = 0xa4u;
        constexpr uint32_t kDigitsFrom = 185u; // frames to go when GT3's "3" is about to show (180)
        constexpr uint32_t kStartShown = 150u; // VSyncs GT3's "START" may stay up after the start
        constexpr uint32_t kPanelAfter = 60u;  // VSyncs the dark panel stays up after the lights go out

        PS2Runtime::RecompiledFunction g_next = nullptr;   // the start controller
        PS2Runtime::RecompiledFunction g_aiNext = nullptr; // the AI driver (or the wrappers before ours)
        bool g_lights = false;      // GT3_START_LIGHTS
        bool g_hideDigits = false;  // GT3's "3 2 1 START" hidden (with the lights, or GT3_START_HIDE_DIGITS)
        bool g_link = false;        // GT3_LINK_MODE set: no random wait
        int g_count = 5;
        float g_waitMin = 0.2f, g_waitMax = 3.0f;
        float g_reactMin = 0.0f, g_reactMax = 0.0f; // 0: AI reaction off
        int g_fixedHold = 0;        // GT3_START_HOLD (diagnostic)
        std::mt19937 g_rng{static_cast<unsigned>(std::chrono::steady_clock::now().time_since_epoch().count())};

        int g_holdTarget = 0;       // frames to hold at the last frame, this start
        int g_held = -1;            // frames held so far (-1: not holding)
        uint32_t g_lastFrames = 0;
        std::atomic<uint32_t> g_startVblank{0u}; // when the last START was sent
        uint32_t g_react[6]{};      // the AI cars' waits after START (frames), this start

        std::atomic<bool> g_panelShow{false};
        std::atomic<int> g_panelLit{0};

        uint32_t u32(const uint8_t *rdram, uint32_t a)
        {
            uint32_t v;
            std::memcpy(&v, rdram + (a & kRamMask), 4);
            return v;
        }

        void putF32(uint8_t *rdram, uint32_t a, float v) { std::memcpy(rdram + (a & kRamMask), &v, 4); }

        bool replay(const uint8_t *rdram)
        {
            gt3race::Snapshot s;
            return gt3race::read(rdram, s, false) && s.replay;
        }

        uint32_t raceClockMs(const uint8_t *rdram)
        {
            gt3race::Snapshot s;
            return gt3race::read(rdram, s, false) ? s.clockMs : 0xffffffffu;
        }

        float uniform(float lo, float hi)
        {
            if (hi <= lo)
                return lo;
            return std::uniform_real_distribution<float>(lo, hi)(g_rng);
        }

        // Lights lit with `frames` to go: light i (0 = leftmost) from 1 + 60 * (count - 1 - i),
        // so the last comes on as the countdown reaches its last frame (where the hold starts).
        int litAt(uint32_t frames)
        {
            int lit = 0;
            for (int i = 0; i < g_count; ++i)
                if (frames <= 1u + 60u * static_cast<uint32_t>(g_count - 1 - i))
                    ++lit;
            return lit;
        }

        void startHook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            const uint32_t obj = static_cast<uint32_t>(_mm_extract_epi32(ctx->r[4], 0)) & kRamMask;
            const bool valid = obj >= 0x100000u && obj + 0x100u < 0x02000000u;
            if (valid && u32(rdram, obj + kAsked) == 0u && u32(rdram, obj + kFrames) == 1u && g_holdTarget > 0)
            {
                if (g_held < 0)
                {
                    g_held = 0;
                    std::fprintf(stderr, "[start-lights] all lights on at vblank %u (race clock %u ms); out in %d frames\n",
                                 u32(rdram, kVblankCounter), raceClockMs(rdram), g_holdTarget);
                }
                if (g_held < g_holdTarget)
                {
                    ++g_held;
                    if (g_lights)
                    {
                        g_panelShow.store(true, std::memory_order_relaxed);
                        g_panelLit.store(g_count, std::memory_order_relaxed);
                    }
                    ctx->pc = static_cast<uint32_t>(_mm_extract_epi32(ctx->r[31], 0)); // GT3's controller not run this frame
                    return;
                }
            }
            g_next(rdram, ctx, runtime);
            if (!valid)
                return;
            const uint32_t now = u32(rdram, kVblankCounter);
            const bool asked = u32(rdram, obj + kAsked) != 0u;
            const uint32_t frames = u32(rdram, obj + kFrames);
            if (!asked && frames > g_lastFrames + 1u)
            {
                // A new countdown: this start's wait.
                const bool noWait = g_link || replay(rdram);
                g_holdTarget = g_fixedHold > 0 ? g_fixedHold : (g_lights && !noWait ? static_cast<int>(uniform(g_waitMin, g_waitMax) * 60.0f + 0.5f) : 0);
                g_held = -1;
                g_startVblank.store(0u, std::memory_order_relaxed);
                std::fprintf(stderr, "[start-lights] countdown set: %u frames (%u s, kind %u) at vblank %u; lights out %d frames after the last one%s\n",
                             frames, u32(rdram, obj + kSeconds), u32(rdram, obj + kKind), now, g_holdTarget,
                             noWait && g_lights ? (g_link ? " (i.LINK race: no random wait)" : " (replay: no random wait)") : "");
            }
            if (frames == 0u && g_lastFrames == 1u)
            {
                g_startVblank.store(now == 0u ? 1u : now, std::memory_order_relaxed);
                for (int k = 1; k <= 5; ++k)
                    g_react[k] = static_cast<uint32_t>(uniform(g_reactMin, g_reactMax) * 60.0f + 0.5f);
                std::fprintf(stderr, "[start-lights] START (lights out) at vblank %u after %d frames' wait, race clock %u ms", now,
                             g_held < 0 ? 0 : g_held, raceClockMs(rdram));
                if (g_reactMax > 0.0f)
                    std::fprintf(stderr, "; AI reactions %u %u %u %u %u frames", g_react[1], g_react[2], g_react[3], g_react[4], g_react[5]);
                std::fputc('\n', stderr);
                g_held = -1;
            }
            g_lastFrames = asked ? 0u : frames;
            const uint32_t started = g_startVblank.load(std::memory_order_relaxed);
            const bool counting = !asked && frames > 0u;
            if (g_lights)
            {
                const bool up = (counting && frames <= 60u * static_cast<uint32_t>(g_count) + 30u) || (started != 0u && now - started < kPanelAfter);
                g_panelShow.store(up, std::memory_order_relaxed);
                g_panelLit.store(counting ? litAt(frames) : 0, std::memory_order_relaxed);
            }
            if (g_hideDigits)
                GS::setHudHideMiddle((counting && frames <= kDigitsFrom) || (started != 0u && now - started < kStartShown));
        }

        // Each AI car (1-5) waits its own time after the lights go out: throttle 0, brake on.
        void aiHook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            const uint32_t controller = static_cast<uint32_t>(_mm_extract_epi32(ctx->r[4], 0)) & kRamMask;
            g_aiNext(rdram, ctx, runtime);
            const uint32_t started = g_startVblank.load(std::memory_order_relaxed);
            if (started == 0u || controller < kControllerOffset + 0x100000u)
                return;
            const uint32_t since = u32(rdram, kVblankCounter) - started;
            if (since > 120u)
                return;
            const uint32_t car = controller - kControllerOffset;
            const uint32_t race = u32(rdram, car + 4u) & kRamMask;
            if (race < 0x100000u || car < race + kFirstCar)
                return;
            const uint32_t rel = car - (race + kFirstCar);
            if (rel % kCarSize != 0u)
                return;
            const uint32_t k = rel / kCarSize;
            if (k < 1u || k > 5u || since >= g_react[k])
                return;
            putF32(rdram, controller + kCtlThrottle, 0.0f);
            putF32(rdram, controller + kCtlBrake, 1.0f);
        }

        void pair(const char *name, float &a, float &b)
        {
            const char *v = std::getenv(name);
            if (!v || !*v)
                return;
            float x = 0.0f, y = 0.0f;
            const int n = std::sscanf(v, "%f,%f", &x, &y);
            if (n >= 1)
            {
                a = std::max(0.0f, x);
                b = n == 2 ? std::max(a, y) : a;
            }
        }
    }

    Panel panel()
    {
        Panel p;
        p.show = g_panelShow.load(std::memory_order_relaxed);
        p.count = g_count;
        p.lit = g_panelLit.load(std::memory_order_relaxed);
        return p;
    }

    void install(PS2Runtime &runtime, const unsigned char *rdram)
    {
        const char *l = std::getenv("GT3_START_LIGHTS");
        g_lights = l && *l == '1';
        const char *h = std::getenv("GT3_START_HOLD");
        g_fixedHold = h && *h ? std::max(0, std::atoi(h)) : 0;
        const char *d = std::getenv("GT3_START_HIDE_DIGITS");
        g_hideDigits = g_lights || (d && *d == '1');
        const char *c = std::getenv("GT3_START_LIGHTS_COUNT");
        g_count = c && *c ? std::clamp(std::atoi(c), 3, 5) : 5;
        pair("GT3_START_WAIT", g_waitMin, g_waitMax);
        g_waitMax = std::min(g_waitMax, 10.0f);
        pair("GT3_START_AI_REACTION", g_reactMin, g_reactMax);
        g_reactMax = std::min(g_reactMax, 2.0f);
        const char *link = std::getenv("GT3_LINK_MODE");
        g_link = link && *link;
        if ((!g_lights && !g_hideDigits && g_fixedHold <= 0) || !rdram)
            return;
        // addiu sp, sp, -0x30 at 0x17a5e8; the countdown's store (sw v0, 0x5c(s1)) at 0x17a718 and the
        // START message (addiu a1, zero, 4) at 0x17a720 (GT3 USA CORE)
        if (u32(rdram, kStartControl) != 0x27bdffd0u || u32(rdram, 0x17a718u) != 0xae22005cu || u32(rdram, 0x17a720u) != 0x24050004u ||
            (g_next = runtime.lookupFunction(kStartControl)) == nullptr)
        {
            std::fprintf(stderr, "[start-lights] GT3's start controller not found: off\n");
            return;
        }
        runtime.registerFunction(kStartControl, &startHook);
        // The AI cars' reactions (chains after the autopilot's / AI pilot's wrappers; addiu sp, sp, -0x110).
        if (g_lights && g_reactMax > 0.0f && !g_link)
        {
            if (u32(rdram, kAiDriver) == 0x27bdfef0u && (g_aiNext = runtime.lookupFunction(kAiDriver)) != nullptr)
                runtime.registerFunction(kAiDriver, &aiHook);
            else
                g_reactMax = 0.0f;
        }
        if (g_lights)
            std::fprintf(stderr, "[start-lights] on: %d lights, out %.2f-%.2f s after the last%s; AI reactions %s\n", g_count, g_waitMin, g_waitMax,
                         g_link ? " (i.LINK: no random wait)" : "", g_reactMax > 0.0f ? "on" : "off");
        else
            std::fprintf(stderr, "[start-lights] diagnostics: hold %d frames%s\n", g_fixedHold, g_hideDigits ? ", GT3's countdown digits hidden" : "");
    }
}
