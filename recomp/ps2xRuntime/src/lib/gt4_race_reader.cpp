// GT4's race state, read-only: see gt4_race_reader.h and analysis/design/race-reader/RACE-STATE.md.
#include "gt4_race_reader.h"
#include "gt4_drive_log.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>

namespace gt4race
{
    namespace
    {
        constexpr uint32_t kRam = 0x02000000u;
        std::mutex g_lock;
        State g_state;
        bool g_have = false;
        int g_coreOk = -1;
        std::FILE *g_csv = nullptr;
        bool g_csvTried = false;
        // Course length: the largest lap distance seen before a car's distance wrapped back near 0.
        uint32_t g_lenCourse = ~0u;
        float g_lenMax[kMaxCars] = {}, g_lenPrev[kMaxCars] = {}, g_courseLength = 0.0f;
        std::chrono::steady_clock::time_point g_lastFrame{};

        struct Mem
        {
            const uint8_t *ram;
            bool ok(uint32_t a, uint32_t n = 4) const { return a != 0u && a < kRam && a + n <= kRam; }
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
            int16_t s16(uint32_t a) const
            {
                int16_t v = 0;
                if (ok(a, 2))
                    std::memcpy(&v, ram + a, 2);
                return v;
            }
            uint8_t u8(uint32_t a) const { return ok(a, 1) ? ram[a] : 0u; }
            void str(uint32_t a, char *out, size_t n) const
            {
                size_t i = 0;
                for (; i + 1 < n && ok(a + static_cast<uint32_t>(i), 1); ++i)
                {
                    const char c = static_cast<char>(ram[a + i]);
                    if (c == 0)
                        break;
                    out[i] = (c == '`') ? '\'' : ((c >= 32 && c < 127) ? c : '?');
                }
                out[i] = 0;
            }
        };

        const char *phaseName(Phase p)
        {
            switch (p)
            {
            case Phase::PreRace: return "prerace";
            case Phase::Starting: return "starting";
            case Phase::Racing: return "racing";
            case Phase::Paused: return "paused";
            case Phase::Finished: return "finished";
            case Phase::Replay: return "replay";
            default: return "none";
            }
        }

        void writeCsv(const State &s)
        {
            if (!g_csvTried)
            {
                g_csvTried = true;
                if (const char *path = std::getenv("GT4_RACE_READER_CSV"); path && *path)
                {
                    g_csv = std::fopen(path, "w");
                    if (g_csv)
                        writeCsvHeader(g_csv, true);
                    std::fprintf(stderr, "[gt4-race] race reader CSV (GT4_RACE_READER_CSV): %s%s\n", path, g_csv ? "" : " (could not open)");
                }
            }
            if (!g_csv)
                return;
            writeCsvRows(g_csv, s, true);
            if ((s.raceFrames & 255u) == 0u)
                std::fflush(g_csv);
        }

        // GT4_RACE_READER_SCAN=1 (tests, HUD hand-off 7 C): GT4's rev limit and driver names looked for, read only. 40 s into
        // the race and at the finish, for every car: its highest rpm so far and every word of its car record and race entry
        // that holds 2,000 to 20,000 (as a float, a u32 or a u16); once, the race entry's text.
        float g_scanMax[kMaxCars] = {};
        uint32_t g_scanCourse = ~0u;
        int g_scanDone = 0;
        uint64_t g_scanRacing = 0;
        void scan(const Mem &m, const State &s, uint32_t d, uint32_t o)
        {
            static const bool on = [] {
                const char *v = std::getenv("GT4_RACE_READER_SCAN");
                return v && *v == '1';
            }();
            if (!on)
                return;
            if (s.course != g_scanCourse || s.phase == Phase::PreRace)
            {
                g_scanCourse = s.course;
                std::fill(std::begin(g_scanMax), std::end(g_scanMax), 0.0f);
                g_scanDone = 0;
                g_scanRacing = 0;
            }
            if (s.phase != Phase::Racing && s.phase != Phase::Finished)
                return;
            for (int i = 0; i < s.carCount; ++i)
                g_scanMax[i] = std::max(g_scanMax[i], s.cars[i].rpm);
            if (s.phase == Phase::Racing)
                ++g_scanRacing;
            const bool first = g_scanDone == 0 && g_scanRacing >= 2400u, last = g_scanDone == 1 && s.phase == Phase::Finished;
            if (!first && !last)
                return;
            g_scanDone = first ? 1 : 2;
            for (int i = 0; i < s.carCount; ++i)
            {
                const uint32_t c = d + 0x6698u + static_cast<uint32_t>(i) * 0x10a0u, e = o + 0x990u + static_cast<uint32_t>(i) * 0x35e0u;
                std::string line;
                char t[64];
                auto words = [&](const char *tag, uint32_t base, uint32_t size) {
                    for (uint32_t off = 0; off + 4u <= size; off += 4u)
                    {
                        const float f = m.f32(base + off);
                        const uint32_t u = m.u32(base + off);
                        if (f >= 2000.0f && f <= 20000.0f)
                            std::snprintf(t, sizeof(t), " %s+%x=%.1f", tag, off, f), line += t;
                        else if (u >= 2000u && u <= 20000u)
                            std::snprintf(t, sizeof(t), " %s+%x:%u", tag, off, u), line += t;
                        else
                            for (uint32_t h = 0; h < 2u; ++h)
                            {
                                const uint32_t v = (u >> (16u * h)) & 0xffffu;
                                if (v >= 2000u && v <= 20000u)
                                    std::snprintf(t, sizeof(t), " %s+%x.h:%u", tag, off + 2u * h, v), line += t;
                            }
                    }
                };
                words("c", c, 0x10a0u);
                words("e", e, 0x35e0u);
                std::fprintf(stderr, "[gt4-race] scan (%s) car %d %s(%s, code %u): highest rpm %.0f;%s\n", first ? "40 s in" : "finish", i,
                             s.cars[i].player ? "player " : "", s.cars[i].name, s.cars[i].carCode, g_scanMax[i], line.c_str());
                if (first)
                {
                    std::string text;
                    for (uint32_t off = 0; off < 0x35e0u;)
                    {
                        uint32_t n = 0;
                        while (off + n < 0x35e0u && m.u8(e + off + n) >= 32u && m.u8(e + off + n) < 127u)
                            ++n;
                        if (n >= 4u)
                        {
                            char str[96];
                            m.str(e + off, str, std::min<size_t>(sizeof(str), n + 1u));
                            std::snprintf(t, sizeof(t), " e+%x '", off);
                            text += t + std::string(str) + "'";
                        }
                        off += n + 1u;
                    }
                    std::fprintf(stderr, "[gt4-race] scan: car %d's race entry text:%s\n", i, text.c_str());
                }
            }
        }
    } // namespace

    void writeCsvHeader(std::FILE *f, bool sectors)
    {
        std::fprintf(f, "vsync,frame,phase,mode,course,laps,clock_ms,car,player,place,lap,laps_done,lap_ms,last_ms,best_ms,total_ms,"
                        "finished,lap_dist,x,y,z,yaw,speed,code,name,gear,rpm,throttle,brake,steer,acc_long,acc_lat%s\n",
                     sectors ? ",sector,s0,s1,s2,s3,b0,b1,b2,b3,car_rpm" : "");
    }

    void writeCsvRows(std::FILE *f, const State &s, bool sectors)
    {
            for (int i = 0; i < s.carCount; ++i)
            {
                const Car &c = s.cars[i];
                const bool p = c.player;
                std::fprintf(f, "%llu,%llu,%s,%d,%u,%d,%u,%d,%d,%d,%d,%d,%u,%u,%u,%u,%d,%.2f,%.3f,%.3f,%.3f,%.4f,%.3f,%u,\"%s\"",
                             static_cast<unsigned long long>(s.vsync), static_cast<unsigned long long>(s.raceFrames), phaseName(s.phase), s.mode, s.course,
                             s.laps, s.clockMs, i, p ? 1 : 0, c.place, c.lap, c.lapsDone, c.lapTimeMs, c.lastLapMs, c.bestLapMs, c.totalMs,
                             c.finished ? 1 : 0, c.lapDistance, c.x, c.y, c.z, c.yaw, c.speed, c.carCode, c.name);
                if (p)
                    std::fprintf(f, ",%d,%.0f,%.3f,%.3f,%.3f,%.3f,%.3f", s.player.gear, s.player.rpm, s.player.throttle, s.player.brake, s.player.steer,
                                 s.player.accelLong, s.player.accelLat);
                else
                    std::fprintf(f, ",,,,,,,");
                if (sectors)
                    std::fprintf(f, ",%d,%u,%u,%u,%u,%u,%u,%u,%u,%.0f", c.sector, c.sectorMs[0], c.sectorMs[1], c.sectorMs[2], c.sectorMs[3], c.bestSectorMs[0],
                                 c.bestSectorMs[1], c.bestSectorMs[2], c.bestSectorMs[3], c.rpm);
                std::fputc('\n', f);
            }
    }

    bool enabled()
    {
#if defined(PS2X_GAME_GT4) && !defined(GT4_STOCK_BUILD)
        return g_coreOk != 0;
#else
        return false;
#endif
    }

    void onRaceFrame(const uint8_t *rdram, uint32_t r, uint64_t vsync)
    {
#if defined(PS2X_GAME_GT4) && !defined(GT4_STOCK_BUILD)
        const Mem m{rdram};
        if (g_coreOk < 0)
        {
            // The same CORE check as the AI pilot (the race frame update's and the pad routine's first words).
            g_coreOk = (m.u32(0x389638u) == 0x27bdffe0u && m.u32(0x349fd0u) == 0x27bdff80u) ? 1 : 0;
            std::fprintf(stderr, g_coreOk ? "[gt4-race] race reader on (read-only, every race frame)\n"
                                          : "[gt4-race] CORE does not match (0x389638 / 0x349fd0): race reader off\n");
        }
        if (g_coreOk != 1)
            return;
        r &= 0x1fffffffu;
        if (!m.ok(r, 0xd50))
            return;
        const uint32_t o = m.u32(r + 0x6c) & 0x1fffffffu, d = m.u32(r + 0x70) & 0x1fffffffu;
        if (!m.ok(o, 0x990 + kMaxCars * 0x35e0) || !m.ok(d, 0xf8c0))
            return;
        State s;
        {
            std::lock_guard<std::mutex> lk(g_lock);
            s.raceFrames = g_state.raceFrames + 1;
        }
        s.vsync = vsync;
        s.mode = static_cast<int>(m.u32(r + 0xcc8));
        s.course = m.u32(o + 0x78);
        s.laps = static_cast<int>(m.u32(o));
        s.clockMs = m.u32(d + 0xf898) / 3u;
        s.carCount = static_cast<int>(std::min<uint32_t>(m.u32(d + 0xf8a0), kMaxCars));
        bool anyPlayer = false, playerFinished = false, allFinished = s.carCount > 0;
        for (int i = 0; i < s.carCount; ++i)
        {
            const uint32_t c = d + 0x6698u + static_cast<uint32_t>(i) * 0x10a0u;
            const uint32_t e = o + 0x990u + static_cast<uint32_t>(i) * 0x35e0u;
            Car &k = s.cars[i];
            k.place = static_cast<int8_t>(m.u8(c + 0x5b3));
            k.lap = m.s16(c + 0x5b0);
            k.lapsDone = static_cast<int>(m.u32(e + 0x1ac));
            // (No lap yet: GT4 holds 359,999,999 ms = 99'59.999; given as 0.)
            auto lapMs = [](uint32_t v) { return v >= 359999999u ? 0u : v; };
            k.lastLapMs = lapMs(m.u32(e + 0x11dc));
            k.bestLapMs = lapMs(m.u32(e + 0x1198));
            k.totalMs = m.u32(e + 0x1b0);
            const uint32_t lineMs = m.u32(c + 0x718);
            k.lapTimeMs = s.clockMs > lineMs ? s.clockMs - lineMs : 0u;
            k.finished = m.u8(c + 0x5b6) == 1u;
            k.lapDistance = m.f32(c + 0x5a8);
            k.x = m.f32(c + 0x104);
            k.y = m.f32(c + 0x108);
            k.z = m.f32(c + 0x10c);
            k.yaw = std::atan2(m.f32(c + 0x114), m.f32(c + 0x110));
            k.speed = m.f32(c + 0x640);
            k.carCode = m.u32(e + 0x20);
            m.str(e + 0x350c, k.name, sizeof(k.name));
            k.rpm = m.f32(c + 0x738);
            // The split times (HUD hand-off 7 C): the timing lines passed this lap, this lap's sector times and the car's best.
            k.sector = std::clamp<int>(m.u8(c + 0x5b2), 0, kMaxSectors - 1);
            for (int j = 0; j < kMaxSectors; ++j)
            {
                const uint32_t at = static_cast<uint32_t>(i * 16 + j) * 4u;
                k.sectorMs[j] = lapMs(m.u32(d + 0xd624u + at));
                k.bestSectorMs[j] = lapMs(m.u32(d + 0xd7a4u + at));
            }
            s.sectorCount = std::max(s.sectorCount, k.sector + 1);
            const uint32_t ctrl = m.u32(c + 0xc) & 0x1fffffffu;
            k.player = m.ok(ctrl, 0x1d0) && m.u32(ctrl + 0xd0) == 0x686618u && m.u32(ctrl + 0x1cc) != 0u;
            allFinished = allFinished && k.finished;
            if (k.player && !anyPlayer)
            {
                anyPlayer = true;
                playerFinished = k.finished;
                Player &p = s.player;
                p.index = i;
                p.speed = k.speed;
                p.gear = m.u8(c + 0x624);
                p.rpm = m.f32(c + 0x738);
                p.throttle = m.f32(c + 0x58c);
                p.brake = m.f32(c + 0x590);
                p.steer = m.f32(c + 0x560);
                p.shifting = m.f32(c + 0x610) != 0.0f;
                p.accelLong = m.f32(c + 0x654);
                p.accelLat = m.f32(c + 0x658);
                p.lapTimeMs = k.lapTimeMs;
            }
        }
        if (s.course != g_lenCourse)
        {
            g_lenCourse = s.course;
            g_courseLength = 0.0f;
            std::fill(std::begin(g_lenMax), std::end(g_lenMax), 0.0f);
            std::fill(std::begin(g_lenPrev), std::end(g_lenPrev), 0.0f);
        }
        for (int i = 0; i < s.carCount; ++i)
        {
            const float dist = s.cars[i].lapDistance;
            if (g_lenPrev[i] > 200.0f && dist < g_lenPrev[i] * 0.25f && g_lenMax[i] > 200.0f)
                g_courseLength = std::max(g_courseLength, g_lenMax[i]); // wrapped: the largest distance before it
            g_lenMax[i] = dist < g_lenPrev[i] * 0.25f ? dist : std::max(g_lenMax[i], dist);
            g_lenPrev[i] = dist;
        }
        s.courseLength = g_courseLength;
        s.startHold = static_cast<int>(m.u32(o + 0xbc));
        {
            const uint32_t sp = m.u32(0x6244d8u), pw = m.u32(0x6244dcu), tq = m.u32(0x6244e0u);
            if (sp <= 1u && pw <= 6u && tq <= 2u)
                s.unitSpeed = static_cast<int>(sp), s.unitPower = static_cast<int>(pw), s.unitTorque = static_cast<int>(tq);
        }
        if (s.mode == 2)
            s.phase = Phase::Replay;
        else if (s.mode == 4 || m.u32(r + 0xd48) == 1u)
            s.phase = Phase::PreRace;
        else if (m.u32(r + 0xcd8) == 1u)
            s.phase = Phase::Paused;
        else if (anyPlayer ? playerFinished : allFinished)
            s.phase = Phase::Finished;
        else if (m.u32(r + 0xd4c) == 0u)
            s.phase = Phase::Starting;
        else
            s.phase = Phase::Racing;
        // (the course's sector count stays at what was seen this race)
        {
            static uint32_t secCourse = ~0u;
            static int secCount = 3;
            if (s.course != secCourse || s.phase == Phase::PreRace)
                secCourse = s.course, secCount = 3;
            secCount = std::max(secCount, s.sectorCount);
            s.sectorCount = secCount;
        }
        scan(m, s, d, o);
        writeCsv(s);
        gt4drivelog::onFrame(s); // (GT4_DRIVE_LOG: the Race Lab's lap and race files)
        std::lock_guard<std::mutex> lk(g_lock);
        g_state = s;
        g_have = true;
        g_lastFrame = std::chrono::steady_clock::now();
#else
        (void)rdram;
        (void)r;
        (void)vsync;
#endif
    }

    uint64_t msSinceLastFrame()
    {
        std::lock_guard<std::mutex> lk(g_lock);
        if (!g_have)
            return ~0ull;
        return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - g_lastFrame).count());
    }

    bool snapshot(State &out)
    {
        std::lock_guard<std::mutex> lk(g_lock);
        if (!g_have)
            return false;
        out = g_state;
        return true;
    }
} // namespace gt4race
