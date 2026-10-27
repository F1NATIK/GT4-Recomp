// Holding the start (HUD hand-off 7 E): see gt4_start_hold.h and analysis/design/hud-pages/handoff7/STAGE-E-REPORT.md.
#include "gt4_start_hold.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <atomic>
#include <random>
#include <string>

namespace gt4start
{
#if defined(PS2X_GAME_GT4) && !defined(GT4_STOCK_BUILD)
    namespace
    {
        constexpr uint32_t kRam = 0x02000000u;
        constexpr int kMaxCars = 8;

        struct Settings
        {
            bool on = false;
            // GT4_START_DELAY (ms; read and drawn in whole numbers: the game thread runs with the PS2's round-toward-zero FPU
            // mode, which turned 0.2 into 0.1 in the log's %.1f)
            int delayMinMs = 200, delayMaxMs = 3000;
            int ai = 0;                             // GT4_START_AI 0 instant, 1 quick, 2 normal, 3 slow
            bool freeCar = false;                   // GT4_START_CAR free (else held)
            int jumpSec = 5;                        // GT4_START_JUMP h5 / h10: your car held 5 / 10 s after the lights go out
            int count = 5;                          // GT4_START_COUNT 3..5: the lights, one a second (the first light: free starts)
            bool log = false;                       // GT4_START_HOLD_LOG=1 (tests)
            bool seeded = false;                    // GT4_START_SEED=<n> (tests)
            uint32_t seed = 0;
        };

        bool onOff(const char *v, bool &out)
        {
            if (!v || !*v)
                return false;
            if (!std::strcmp(v, "on") || !std::strcmp(v, "1"))
                return out = true, true;
            if (!std::strcmp(v, "off") || !std::strcmp(v, "0"))
                return out = false, true;
            return false;
        }

        const Settings &cfg()
        {
            static const Settings s = [] {
                Settings t;
                const char *v = std::getenv("GT4_START_HOLD");
                if (v && *v && !onOff(v, t.on))
                    std::fprintf(stderr, "[gt4-start] GT4_START_HOLD=%s not understood (on / off): off\n", v);
                if (const char *d = std::getenv("GT4_START_DELAY"); d && *d)
                {
                    // <s>[.<tenths>[<hundredths>]],<s>[...]: whole milliseconds, no floating point
                    auto seconds = [](const char *&p, int &ms) {
                        if (*p < '0' || *p > '9')
                            return false;
                        int v = 0;
                        while (*p >= '0' && *p <= '9' && v < 100000)
                            v = v * 10 + (*p++ - '0');
                        v *= 1000;
                        if (*p == '.')
                        {
                            ++p;
                            for (int scale = 100; *p >= '0' && *p <= '9'; ++p, scale /= 10)
                                v += (*p - '0') * scale;
                        }
                        ms = v;
                        return true;
                    };
                    const char *p = d;
                    int a = 0, b = 0;
                    if (seconds(p, a) && *p++ == ',' && seconds(p, b) && *p == 0 && b >= a && b <= 5000)
                        t.delayMinMs = a, t.delayMaxMs = b;
                    else
                        std::fprintf(stderr, "[gt4-start] GT4_START_DELAY=%s not understood (<min>,<max> s, 0 to 5): 0.2,3.0\n", d);
                }
                if (const char *a = std::getenv("GT4_START_AI"); a && *a)
                {
                    static const char *const kAi[] = {"instant", "quick", "normal", "slow"};
                    int k = 0;
                    while (k < 4 && std::strcmp(a, kAi[k]) != 0)
                        ++k;
                    if (k < 4)
                        t.ai = k;
                    else
                        std::fprintf(stderr, "[gt4-start] GT4_START_AI=%s not understood (instant / quick / normal / slow): instant\n", a);
                }
                if (const char *c = std::getenv("GT4_START_CAR"); c && *c)
                {
                    t.freeCar = !std::strcmp(c, "free");
                    if (std::strcmp(c, "free") != 0 && std::strcmp(c, "held") != 0)
                        std::fprintf(stderr, "[gt4-start] GT4_START_CAR=%s not understood (held / free): held\n", c);
                }
                if (const char *j = std::getenv("GT4_START_JUMP"); j && *j)
                {
                    // (t5 / t10, time added to your first lap, are not built: STAGE-E-REPORT.md. Read as h5 / h10.)
                    if (!std::strcmp(j, "h5") || !std::strcmp(j, "t5"))
                        t.jumpSec = 5;
                    else if (!std::strcmp(j, "h10") || !std::strcmp(j, "t10"))
                        t.jumpSec = 10;
                    else
                        std::fprintf(stderr, "[gt4-start] GT4_START_JUMP=%s not understood (h5 / h10): h5\n", j);
                    if (j[0] == 't' && (!std::strcmp(j, "t5") || !std::strcmp(j, "t10")))
                        std::fprintf(stderr, "[gt4-start] GT4_START_JUMP=%s: race-time penalties are not built; held %d s instead\n", j, t.jumpSec);
                }
                if (const char *n = std::getenv("GT4_START_COUNT"); n && *n)
                    t.count = std::clamp(std::atoi(n), 3, 5);
                const char *l = std::getenv("GT4_START_HOLD_LOG");
                t.log = l && *l == '1';
                if (const char *sd = std::getenv("GT4_START_SEED"); sd && *sd)
                    t.seeded = true, t.seed = static_cast<uint32_t>(std::strtoul(sd, nullptr, 10));
                if (t.on)
                    std::fprintf(stderr,
                                 "[gt4-start] holding the start on (GT4_START_HOLD): lights out %d to %d ms after the last light, AI %s, "
                                 "your car %s, jump start: held %d s\n",
                                 t.delayMinMs, t.delayMaxMs, t.ai == 0 ? "instant" : t.ai == 1 ? "quick" : t.ai == 2 ? "normal" : "slow",
                                 t.freeCar ? "free from the first light" : "held", t.jumpSec);
                return t;
            }();
            return s;
        }

        struct Mem
        {
            uint8_t *ram;
            static bool ok(uint32_t a, uint32_t n = 4) { return a != 0u && a < kRam && a + n <= kRam; }
            uint32_t u32(uint32_t a) const
            {
                uint32_t v = 0;
                if (ok(a))
                    std::memcpy(&v, ram + a, 4);
                return v;
            }
            float f32(uint32_t a) const
            {
                const uint32_t v = u32(a);
                float f;
                std::memcpy(&f, &v, 4);
                return std::isfinite(f) ? f : 0.0f;
            }
            uint8_t u8(uint32_t a) const { return ok(a, 1) ? ram[a] : 0u; }
            void put32(uint32_t a, uint32_t v) const
            {
                if (ok(a))
                    std::memcpy(ram + a, &v, 4);
            }
        };

        // The race classes held (their vptr at R+0x64, from CORE's type info): RaceArcadeSingle, RaceGTmode, RaceChampionship.
        // Not RaceLanBattle 0x677c28, RaceNetBattle 0x6787c0, RaceNetSinglePlayer 0x685168, RaceNetRallyBattle 0x678d48,
        // RaceSplitBattle 0x676c48, RaceArcadeDemo 0x67c520, RaceLicense 0x6807a8, RaceMission 0x6815e0, RaceFreeRun 0x680140, ...
        const char *raceClass(uint32_t vptr)
        {
            switch (vptr)
            {
            case 0x67bad8u: return "RaceArcadeSingle";
            case 0x684678u: return "RaceGTmode";
            case 0x684bf0u: return "RaceChampionship";
            default: return nullptr;
            }
        }

        enum class St
        {
            Idle,     // no countdown seen yet in this race
            Counting, // GT4's grid countdown running (or held at 1)
            Released, // the lights went out
            Off,      // not a race we hold (logged once)
        };

        struct Race
        {
            uint32_t r = 0;
            St st = St::Idle;
            int lastBc = 0;
            int extraTotal = 0, extraLeft = 0; // frames held after GT4's own start
            uint64_t countVsync = 0, releaseVsync = 0;
            int player = -1;
            uint32_t playerCar = 0;
            bool gridSet = false, jumped = false; // free: your grid place taken at the first light; a jump start seen
            float gridX = 0.0f, gridY = 0.0f;
        };
        Race g;
        int g_coreOk = -1;
        std::mt19937 g_rng;
        // Free mode: your car's record while it is let go (0: none); read by afterHoldLoop on the game thread.
        std::atomic<uint32_t> g_freeCar{0};

        // The AI's reaction ranges (s), GT4_START_AI quick / normal / slow.
        // (frames: quick 0.1-0.3 s, normal 0.2-0.6 s, slow 0.4-1.0 s)
        constexpr int kAiMin[] = {0, 6, 12, 24}, kAiMax[] = {0, 18, 36, 60};

        // Your car: the one a pad drives (its controller a live RaceInput, vptr 0x686618 at +0xd0, +0x1cc != 0), as the race reader.
        int findPlayer(const Mem &m, uint32_t d, uint32_t &car)
        {
            const int cars = static_cast<int>(std::min<uint32_t>(m.u32(d + 0xf8a0), kMaxCars));
            for (int i = 0; i < cars; ++i)
            {
                const uint32_t c = d + 0x6698u + static_cast<uint32_t>(i) * 0x10a0u, ctrl = m.u32(c + 0xc) & 0x1fffffffu;
                if (Mem::ok(ctrl, 0x1d0) && m.u32(ctrl + 0xd0) == 0x686618u && m.u32(ctrl + 0x1cc) != 0u)
                    return car = c, i;
            }
            return car = 0, -1;
        }

        void logFrame(const Mem &m, uint32_t r, uint32_t o, uint32_t d, uint64_t vsync, const char *what)
        {
            char line[2048];
            size_t used = 0;
            auto add = [&](int n) {
                if (n > 0)
                    used = std::min(sizeof(line) - 2u, used + static_cast<size_t>(n));
            };
            add(std::snprintf(line, sizeof(line), "[gt4-start] f vsync=%llu %s bc=%d d4c=%u cd8=%u d48=%u clock=%u clock2=%u held=%d/%d |",
                              static_cast<unsigned long long>(vsync), what, static_cast<int32_t>(m.u32(o + 0xbc)), m.u32(r + 0xd4c),
                              m.u32(r + 0xcd8), m.u32(r + 0xd48), m.u32(d + 0xf898), m.u32(d + 0xf89c), g.extraTotal - g.extraLeft,
                              g.extraTotal));
            const int cars = static_cast<int>(std::min<uint32_t>(m.u32(d + 0xf8a0), kMaxCars));
            for (int i = 0; i < cars; ++i)
            {
                const uint32_t c = d + 0x6698u + static_cast<uint32_t>(i) * 0x10a0u;
                add(std::snprintf(line + used, sizeof(line) - used, " c%d h=%u dly=%d pl=%d x=%.3f y=%.3f v=%.3f thr=%.2f;", i, m.u8(c + 0x5c0),
                                  static_cast<int32_t>(m.u32(d + 0xcbfc + static_cast<uint32_t>(i) * 4u)), static_cast<int8_t>(m.u8(c + 0x5b3)),
                                  m.f32(c + 0x104), m.f32(c + 0x108), m.f32(c + 0x640), m.f32(c + 0x58c)));
            }
            line[used++] = '\n';
            line[used] = 0;
            std::fputs(line, stderr);
        }
    } // namespace

    bool wanted() { return cfg().on; }

    void onRaceFrame(uint8_t *rdram, uint32_t r, uint64_t vsync)
    {
        const Settings &s = cfg();
        if (!s.on)
            return;
        const Mem m{rdram};
        if (g_coreOk < 0)
        {
            // CORE's words this relies on: the race frame update, the dynamics' hold loop (sb s4,0x4bc(a0): car+0x5c0 = 1) and
            // the race clock's hold test (jal 0x34c210 at 0x357f54).
            g_coreOk = (m.u32(0x389638u) == 0x27bdffe0u && m.u32(0x34ddacu) == 0xa09404bcu && m.u32(0x357f54u) == 0x0c0d3084u) ? 1 : 0;
            if (!g_coreOk)
                std::fprintf(stderr, "[gt4-start] CORE does not match (0x389638 / 0x34ddac / 0x357f54): holding the start off\n");
            if (s.seeded)
                g_rng.seed(s.seed);
            else
                g_rng.seed(std::random_device{}());
        }
        if (g_coreOk != 1)
            return;
        r &= 0x1fffffffu;
        if (!Mem::ok(r, 0xd50))
            return;
        const uint32_t o = m.u32(r + 0x6c) & 0x1fffffffu, d = m.u32(r + 0x70) & 0x1fffffffu;
        if (!Mem::ok(o, 0x990) || !Mem::ok(d, 0x10000))
            return;
        if (r != g.r)
            g = Race{}, g.r = r, g_freeCar = 0;
        const int bc = static_cast<int32_t>(m.u32(o + 0xbc));
        const uint32_t mode = m.u32(r + 0xcc8), started = m.u32(r + 0xd4c);
        const uint32_t vptr = m.u32(r + 0x64);
        const char *cls = raceClass(vptr);
        const bool live = cls && mode == 0u;
        const bool stepping = m.u32(r + 0xcd8) == 0u && m.u32(r + 0xd48) == 0u;
        // A new countdown at the same R (the race started again): start over.
        if (g.st != St::Idle && bc > g.lastBc + 1)
        {
            if (s.log)
                std::fprintf(stderr, "[gt4-start] vsync %llu: a new countdown (%d): starting over\n", static_cast<unsigned long long>(vsync), bc);
            g = Race{}, g.r = r, g_freeCar = 0;
        }
        g.lastBc = bc;
        if (g.st == St::Idle && bc > 0 && started == 0u)
        {
            if (!live)
            {
                g.st = St::Off;
                std::fprintf(stderr, "[gt4-start] vsync %llu: grid countdown in a race not held (%s, mode %u): GT4's own start\n",
                             static_cast<unsigned long long>(vsync), cls ? cls : "another race class", mode);
                return;
            }
            g.st = St::Counting;
            g.countVsync = vsync;
            g.player = findPlayer(m, d, g.playerCar);
            std::uniform_int_distribution<int> delay(s.delayMinMs, s.delayMaxMs);
            const int ms = delay(g_rng);
            g.extraTotal = g.extraLeft = (ms * 60 + 500) / 1000;
            std::fprintf(stderr, "[gt4-start] vsync %llu: standing start (%s, countdown %d frames): lights out %d ms (%d frames) after GT4's own start\n",
                         static_cast<unsigned long long>(vsync), cls, bc, ms, g.extraTotal);
        }
        if (g.st == St::Counting)
        {
            if (!live)
            {
                g.st = St::Off;
                g_freeCar = 0;
                return;
            }
            const char *what = "count";
            // E4 (the half metre: GT4_START_CAR in gt4_start_hold.h): your car free from the first light (the AI cars stay held) until the lights go out or it jumps the start.
            if (s.freeCar && g.player >= 0 && !g.jumped && bc <= s.count * 60)
            {
                const float x = m.f32(g.playerCar + 0x104), y = m.f32(g.playerCar + 0x108);
                if (!g.gridSet)
                {
                    g.gridSet = true, g.gridX = x, g.gridY = y;
                    std::fprintf(stderr, "[gt4-start] vsync %llu: first light: your car (car %d) is yours, the AI cars stay held\n",
                                 static_cast<unsigned long long>(vsync), g.player);
                }
                // (forward only, along the car's heading, rotation row 0: rolling back on a sloping grid is no jump start)
                const float moved = (x - g.gridX) * m.f32(g.playerCar + 0x110) + (y - g.gridY) * m.f32(g.playerCar + 0x114);
                if (moved > 0.5f)
                {
                    // E5: a jump start. Held again at once (GT4's own hold, as the AI cars), then GT4_START_JUMP after the lights.
                    g.jumped = true;
                    g_freeCar = 0;
                    std::fprintf(stderr, "[gt4-start] vsync %llu: jump start: your car moved %.2f m before the lights went out; held %d s after them\n",
                                 static_cast<unsigned long long>(vsync), moved, s.jumpSec);
                }
                else
                    g_freeCar = g.playerCar;
            }
            if (bc == 1 && stepping)
            {
                if (g.extraLeft > 0)
                {
                    // GT4's countdown would reach 0 in this frame's step: kept at 1 instead (the step takes it from 2 to 1).
                    m.put32(o + 0xbc, 2u);
                    --g.extraLeft;
                    what = "hold";
                }
                else
                {
                    // Let go: this frame's step takes it to 0 and GT4 starts the race (event 4), the lights go out. Each AI car's own
                    // delay (E3) and, after a jump start, yours (E5) are set now: GT4 counts them down from this step on.
                    g.st = St::Released;
                    g.releaseVsync = vsync;
                    g_freeCar = 0;
                    what = "release";
                    std::string ai;
                    const int cars = static_cast<int>(std::min<uint32_t>(m.u32(d + 0xf8a0), kMaxCars));
                    for (int i = 0; i < cars; ++i)
                    {
                        int frames = 0;
                        if (i == g.player)
                            frames = g.jumped ? s.jumpSec * 60 : 0;
                        else if (s.ai > 0)
                        {
                            std::uniform_int_distribution<int> react(kAiMin[s.ai], kAiMax[s.ai]);
                            frames = react(g_rng);
                            ai += " c" + std::to_string(i) + " " + std::to_string(frames);
                        }
                        if (frames > 0)
                            m.put32(d + 0xcbfcu + static_cast<uint32_t>(i) * 4u, static_cast<uint32_t>(frames));
                    }
                    const std::string you = g.jumped ? "held " + std::to_string(s.jumpSec) + " s (jump start)" : std::string("goes");
                    std::fprintf(stderr, "[gt4-start] vsync %llu: lights out, %d frames after GT4's own start; AI reaction (frames):%s; your car %s\n",
                                 static_cast<unsigned long long>(vsync), g.extraTotal, ai.empty() ? " none" : ai.c_str(), you.c_str());
                }
            }
            if (s.log)
                logFrame(m, r, o, d, vsync, what);
            return;
        }
        if (g.st == St::Released && s.log && vsync < g.releaseVsync + 1200u)
            logFrame(m, r, o, d, vsync, "after");
    }

    void afterHoldLoop(uint8_t *rdram)
    {
        // (GT4's hold loop has just set car+0x5c0 = 1 for every car: yours cleared again, before the cars' passes read it)
        const uint32_t car = g_freeCar.load(std::memory_order_relaxed);
        if (car && Mem::ok(car + 0x5c0, 1))
            rdram[car + 0x5c0] = 0;
    }

#else
    bool wanted() { return false; }
    void onRaceFrame(uint8_t *, uint32_t, uint64_t) {}
    void afterHoldLoop(uint8_t *) {}
#endif
} // namespace gt4start
