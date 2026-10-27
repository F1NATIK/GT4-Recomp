#include "gt3_ai_pilot.h"

#include "gt3_course_patch.h"
#include "gt3_race_info.h"
#include "ps2_runtime.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <random>
#include <sstream>
#include <string>
#include <vector>

namespace gt3aipilot
{
    namespace
    {
        constexpr uint32_t kRamMask = 0x1FFFFFFu;
        constexpr uint32_t kAiDriver = 0x1f7ae0u;   // GT3's AI driver (a0 = the car's controller)
        constexpr uint32_t kVblankCounter = 0x4a69c0u;
        constexpr uint32_t kControllerOffset = 0xec8u;
        constexpr uint32_t kFirstCar = 0xc8u, kCarSize = 0x16dcu; // cars at race + 0xc8 + k * 0x16dc
        // The controller's outputs (GT3_AI_TRACE, Trial Mountain race, 2,550 frames a car,
        // `gt3-analysis/local/ai-trace.err.log`): +0x9c steering target in radians (the car's
        // +0x13a0 follows it, corr 0.99; positive turns the heading up), +0xa0 throttle (0..0.9
        // from GT3's Arcade AI), +0xa4 brake (0..1; the car's +0x13b0 follows it, corr 0.95).
        constexpr uint32_t kCtlSteer = 0x9cu, kCtlThrottle = 0xa0u, kCtlBrake = 0xa4u;
        // Steering response measured from the same trace: yaw rate = v * steer / (2.5 m * (1 +
        // 0.00075 v^2)) (effective wheelbase 3.3 m at 20 m/s, 4.3 at 30, 5.5 at 42). GT3's AI
        // brakes at ~16 m/s^2 at full brake (median).
        constexpr float kWheelbase = 2.5f, kUndersteer = 0.00075f, kPlanDecel = 13.0f;
        constexpr float kPi = 3.14159265f;

        PS2Runtime::RecompiledFunction g_next = nullptr; // the AI driver (or the autopilot's wrapper of it)
        bool g_trace = false;
        uint32_t g_frame[8]{};

        float f32(const uint8_t *rdram, uint32_t a)
        {
            float v;
            std::memcpy(&v, rdram + (a & kRamMask), 4);
            return v;
        }
        uint32_t u32(const uint8_t *rdram, uint32_t a)
        {
            uint32_t v;
            std::memcpy(&v, rdram + (a & kRamMask), 4);
            return v;
        }
        void put(uint8_t *rdram, uint32_t a, float v)
        {
            std::memcpy(rdram + (a & kRamMask), &v, 4);
        }
        float wrapAngle(float a)
        {
            while (a > kPi)
                a -= 2.0f * kPi;
            while (a < -kPi)
                a += 2.0f * kPi;
            return a;
        }

        // ------------------------------------------------------------------ lines
        // A line per lap metre: the autopilot's line format ("gt3-autopilot-line 1 <lap m>
        // <step m>", then "<step> <x> <z> <speed> <n> <nv> <scale>"; speed x scale is the
        // target, as the autopilot drives it). GT4-WORK tools/ai_line_from_lap.py writes recorded
        // laps the same way (scale 1).
        struct Line
        {
            float lap = 0.0f, step = 2.0f;
            std::vector<float> x, z, v;
            bool ok() const { return x.size() > 16 && lap > 100.0f; }
            float wrap(float p) const { return std::fmod(std::fmod(p, lap) + lap, lap); }
            void at(float p, float &px, float &pz) const
            {
                const float q = wrap(p) / step;
                const size_t n = x.size(), i = static_cast<size_t>(q) % n, j = (i + 1) % n;
                const float f = q - std::floor(q);
                px = x[i] + (x[j] - x[i]) * f;
                pz = z[i] + (z[j] - z[i]) * f;
            }
            float speed(float p) const
            {
                const float q = wrap(p) / step;
                const size_t n = v.size(), i = static_cast<size_t>(q) % n, j = (i + 1) % n;
                const float f = q - std::floor(q);
                return v[i] + (v[j] - v[i]) * f;
            }
            // unit tangent along the driving direction, and the normal to its left
            void frame(float p, float &tx, float &tz) const
            {
                float ax, az, bx, bz;
                at(p - 2.0f, ax, az);
                at(p + 2.0f, bx, bz);
                const float l = std::max(1e-3f, std::hypot(bx - ax, bz - az));
                tx = (bx - ax) / l;
                tz = (bz - az) / l;
            }
            // sideways place of (px, pz) from the line at p: + = the normal (-tz, tx)
            float lateral(float p, float px, float pz) const
            {
                float lx, lz, tx, tz;
                at(p, lx, lz);
                frame(p, tx, tz);
                return (px - lx) * -tz + (pz - lz) * tx;
            }
            // signed curvature (1/m) around p, over 10 m
            float curvature(float p) const
            {
                float ax, az, bx, bz;
                frame(p - 5.0f, ax, az);
                frame(p + 5.0f, bx, bz);
                return (ax * bz - az * bx) / 10.0f;
            }
        };

        bool loadLine(const std::filesystem::path &file, Line &line)
        {
            std::ifstream in(file);
            std::string magic;
            int version = 0;
            if (!in || !(in >> magic >> version >> line.lap >> line.step) || magic != "gt3-autopilot-line" || line.step <= 0.1f || line.lap < 100.0f)
                return false;
            const size_t n = static_cast<size_t>(std::ceil(line.lap / line.step));
            if (n < 16 || n > 200000)
                return false;
            std::vector<char> have(n, 0), haveV(n, 0);
            line.x.assign(n, 0.0f);
            line.z.assign(n, 0.0f);
            line.v.assign(n, 0.0f);
            size_t i;
            double px, pz, v;
            uint32_t cn, nv;
            float scale;
            while (in >> i >> px >> pz >> v >> cn >> nv >> scale)
            {
                if (i >= n)
                    continue;
                line.x[i] = static_cast<float>(px);
                line.z[i] = static_cast<float>(pz);
                have[i] = 1;
                if (v > 0.0)
                {
                    line.v[i] = static_cast<float>(v) * scale;
                    haveV[i] = 1;
                }
            }
            // fill the gaps (round the lap) from the neighbours either side
            auto fill = [n](std::vector<float> &a, const std::vector<char> &h) {
                size_t first = n;
                for (size_t k = 0; k < n; ++k)
                    if (h[k])
                    {
                        first = k;
                        break;
                    }
                if (first == n)
                    return false;
                size_t prev = first;
                for (size_t s = 1; s <= n; ++s)
                {
                    const size_t k = (first + s) % n;
                    if (!h[k])
                        continue;
                    const size_t gap = (k + n - prev) % n;
                    for (size_t g = 1; g < gap; ++g)
                        a[(prev + g) % n] = a[prev] + (a[k] - a[prev]) * static_cast<float>(g) / static_cast<float>(gap);
                    prev = k;
                }
                return true;
            };
            if (!fill(line.x, have) || !fill(line.z, have))
                return false;
            if (!fill(line.v, haveV))
            {
                // no speeds: from the curvature (12 m/s^2 across, 70 m/s at most)
                for (size_t k = 0; k < n; ++k)
                {
                    const float c = std::fabs(line.curvature(static_cast<float>(k) * line.step));
                    line.v[k] = std::min(70.0f, std::sqrt(12.0f / std::max(c, 1e-4f)));
                }
            }
            return true;
        }

        // ------------------------------------------------------------------ drivers
        struct Driver
        {
            bool on = false;
            std::string source;
            Line line;
            float speedPct = 0.0f, brakeM = 0.0f, pace = 1.0f, throttleMax = 1.0f;
            int mistakes = 0, passing = 1, avoid = 1; // avoid: contact 0 relaxed, 1 normal, 2 careful
            bool gt3Steer = false; // "steer gt3": GT3's own AI steers (give it the line in the course's slot 31:
                                   // gt3_lines_from_path.py); the pilot only sets the pace
            // this lap's variation
            int lap = -1;
            uint32_t lapStartMs = 0;
            float speedF = 1.0f, brakeOff = 0.0f;
            // traffic: metres to the left (+) of the line
            float offset = 0.0f;
            // a mistake in the current braking zone
            int mistake = 0; // 1 late braking, 2 wide, 3 early throttle
            float mistakeAmount = 0.0f, zoneMin = 0.0f, zoneEnd = -1e9f;
            int mistakesThisLap = 0;
            // hands back to GT3 (off the line, stuck)
            uint32_t slowSince = 0, handsOffUntil = 0;
            uint32_t lastClock = 0;
            std::mt19937 rng{12345u};
            float uniform(float lo, float hi) { return std::uniform_real_distribution<float>(lo, hi)(rng); }
        };
        Driver g_drivers[8];
        bool g_linesOn = false;
        std::filesystem::path g_folder;
        uint32_t g_race = 0;

        std::string courseKey()
        {
            uint32_t crc = 0;
            std::string replacement;
            if (!gt3coursepatch::lastCourse(crc, replacement))
                return "";
            if (!replacement.empty())
                return replacement;
            char buf[32];
            std::snprintf(buf, sizeof(buf), "runway-%08x", crc);
            return buf;
        }

        // <folder>/<course key>.txt: "gt3-ai-cars 1", then per car
        // "car <1-5> line <file> [speed <±%>] [brake <±m>] [mistakes <0-3>] [passing <0-2>] [avoid <0-2>] [pace <x>] [throttle <max>]"
        // ("car <k> gt3" or no line: GT3's own AI drives it).
        void configure()
        {
            for (int k = 0; k < 8; ++k)
                g_drivers[k] = Driver{};
            const std::string key = courseKey();
            if (key.empty())
                return;
            const std::filesystem::path file = g_folder / (key + ".txt");
            std::ifstream in(file);
            std::string magic;
            int version = 0;
            if (!in || !(in >> magic >> version) || magic != "gt3-ai-cars")
            {
                std::fprintf(stderr, "[ai-pilot] %s: no %s, GT3's own AI drives\n", key.c_str(), file.string().c_str());
                return;
            }
            std::string row;
            std::getline(in, row);
            while (std::getline(in, row))
            {
                std::istringstream words(row);
                std::string w;
                int k = 0;
                if (!(words >> w) || w != "car" || !(words >> k) || k < 1 || k > 5)
                    continue;
                Driver &d = g_drivers[k];
                d.rng.seed(0x9e3779b9u * static_cast<uint32_t>(k) ^ static_cast<uint32_t>(std::time(nullptr)));
                std::string lineFile;
                while (words >> w)
                {
                    if (w == "line")
                        words >> lineFile;
                    else if (w == "speed")
                        words >> d.speedPct;
                    else if (w == "brake")
                        words >> d.brakeM;
                    else if (w == "mistakes")
                        words >> d.mistakes;
                    else if (w == "passing")
                        words >> d.passing;
                    else if (w == "pace")
                        words >> d.pace;
                    else if (w == "avoid")
                        words >> d.avoid;
                    else if (w == "steer")
                    {
                        std::string who;
                        words >> who;
                        d.gt3Steer = who == "gt3";
                    }
                    else if (w == "throttle")
                        words >> d.throttleMax;
                }
                d.speedPct = std::clamp(d.speedPct, 0.0f, 20.0f);
                d.brakeM = std::clamp(d.brakeM, 0.0f, 40.0f);
                d.mistakes = std::clamp(d.mistakes, 0, 3);
                d.passing = std::clamp(d.passing, 0, 2);
                d.avoid = std::clamp(d.avoid, 0, 2);
                d.pace = std::clamp(d.pace, 0.5f, 1.3f);
                d.throttleMax = std::clamp(d.throttleMax, 0.3f, 1.0f);
                if (lineFile.empty())
                    continue;
                if (!loadLine(g_folder / lineFile, d.line) || !d.line.ok())
                {
                    std::fprintf(stderr, "[ai-pilot] car %d: line %s not read: GT3's own AI drives\n", k, lineFile.c_str());
                    continue;
                }
                d.on = true;
                d.source = lineFile;
                std::fprintf(stderr, "[ai-pilot] %s car %d: %s (%.0f m, %zu steps), speed ±%.0f%%, brake ±%.0f m, mistakes %d, passing %d, contact %d, pace %.2f\n",
                             key.c_str(), k, lineFile.c_str(), d.line.lap, d.line.x.size(), d.speedPct, d.brakeM, d.mistakes, d.passing, d.avoid, d.pace);
            }
        }

        const gt3race::Car *carOf(const gt3race::Snapshot &s, int k)
        {
            for (const auto &c : s.cars)
                if (c.index == k)
                    return &c;
            return nullptr;
        }

        void drive(uint8_t *rdram, uint32_t controller, const gt3race::Snapshot &s, int k)
        {
            Driver &d = g_drivers[k];
            const gt3race::Car *me = carOf(s, k);
            if (!me || s.clockMs == 0u || !std::isfinite(me->progress))
                return; // the grid and the countdown stay GT3's
            const Line &L = d.line;
            const float v = std::max(0.0f, me->speed), p = me->progress;
            const float dt = d.lastClock && s.clockMs > d.lastClock ? std::min(0.1f, (s.clockMs - d.lastClock) / 1000.0f) : 1.0f / 60.0f;
            d.lastClock = s.clockMs;

            // a new lap: this lap's ± speed and braking points
            if (me->lap != d.lap)
            {
                if (d.lap >= 1 && me->lap == d.lap + 1)
                {
                    const uint32_t ms = s.clockMs - d.lapStartMs;
                    std::fprintf(stderr, "[ai-pilot] car %d lap %d %u'%06.3f (speed x%.3f, braking %+.1f m, %d mistakes)\n", k, d.lap, ms / 60000u,
                                 (ms % 60000u) / 1000.0, d.speedF, d.brakeOff, d.mistakesThisLap);
                }
                d.lap = me->lap;
                d.lapStartMs = s.clockMs;
                d.speedF = 1.0f + d.uniform(-1.0f, 1.0f) * d.speedPct / 100.0f;
                d.brakeOff = d.uniform(-1.0f, 1.0f) * d.brakeM;
                d.mistakesThisLap = 0;
            }

            // off the line or stuck: GT3's own AI drives for a while (it knows how to get back)
            float lx, lz;
            L.at(p, lx, lz);
            const float error = std::hypot(me->x - lx, me->z - lz);
            if (s.clockMs < d.handsOffUntil)
                return;
            if (error > 14.0f)
            {
                d.handsOffUntil = s.clockMs + 3000u;
                std::fprintf(stderr, "[ai-pilot] car %d %.0f m off its line at %.0f m: GT3 drives for 3 s\n", k, error, p);
                return;
            }
            if (v < 2.0f && s.clockMs > 6000u)
            {
                if (!d.slowSince)
                    d.slowSince = s.clockMs;
                else if (s.clockMs - d.slowSince > 2500u)
                {
                    d.slowSince = 0;
                    d.handsOffUntil = s.clockMs + 4000u;
                    std::fprintf(stderr, "[ai-pilot] car %d stuck at %.0f m: GT3 drives for 4 s\n", k, p);
                    return;
                }
            }
            else
                d.slowSince = 0;

            auto lineV = [&](float q) { return L.speed(q) * d.pace * d.speedF; };

            // mistakes: one roll per braking zone
            const float vHere = lineV(p);
            if (me->lap >= 1 && (p > d.zoneEnd || p < d.zoneEnd - 600.0f))
            {
                d.mistake = 0;
                float vMin = vHere, at = 0.0f;
                for (float dd = 10.0f; dd < 160.0f; dd += L.step)
                    if (lineV(p + dd) < vMin)
                    {
                        vMin = lineV(p + dd);
                        at = dd;
                    }
                if (vMin < vHere - 5.0f)
                {
                    d.zoneMin = p + at;
                    d.zoneEnd = p + at + 70.0f;
                    static const float chance[4] = {0.0f, 0.04f, 0.10f, 0.20f};
                    if (d.uniform(0.0f, 1.0f) < chance[d.mistakes])
                    {
                        d.mistake = 1 + static_cast<int>(d.uniform(0.0f, 2.999f));
                        d.mistakeAmount = d.mistake == 1 ? d.uniform(8.0f, 18.0f) : d.mistake == 2 ? d.uniform(1.2f, 2.4f) : d.uniform(0.05f, 0.09f);
                        ++d.mistakesThisLap;
                        static const char *names[4] = {"", "brakes late", "runs wide", "gets on the throttle early"};
                        std::fprintf(stderr, "[ai-pilot] car %d %s at %.0f m (%.2f)\n", k, names[d.mistake], d.zoneMin, d.mistakeAmount);
                    }
                }
            }

            // traffic. Passing (0-2) is how keen the car is to go by; contact (avoid 0 relaxed, 1
            // normal, 2 careful) is how much room it keeps. Every car's place over the next moments
            // comes from its real speed and heading: a closest approach inside the margin means
            // backing off from a car ahead or moving over from one alongside (planned offsets alone
            // let car 5 run into car 4 at 10 m/s closing, run ai-lines4)
            static const float gapTime[3] = {0.55f, 0.40f, 0.28f};
            static const float sideRoom[3] = {2.3f, 3.0f, 3.6f}; // centre to centre across (cars are ~1.9 m wide)
            static const float lookTime[3] = {1.2f, 2.0f, 2.8f};
            static const float extraGap[3] = {0.0f, 0.12f, 0.30f};
            const int av = d.avoid;
            const float curveAhead = std::max(std::fabs(L.curvature(p + 20.0f)), std::fabs(L.curvature(p + 45.0f)));
            const float maxOff = curveAhead < 1.0f / 250.0f ? (d.passing == 2 ? 4.0f : 3.5f) : 1.8f;
            const float myLat = L.lateral(p, me->x, me->z);
            const float fx = -std::sin(me->yaw), fz = -std::cos(me->yaw);
            float want = 0.0f, cap = 1e9f;
            bool busy = false; // another car close: GT3's own avoidance counts too (steer gt3)
            for (const auto &o : s.cars)
            {
                if (o.index == k || !std::isfinite(o.progress))
                    continue;
                float dp = o.progress - p;
                if (dp > L.lap / 2.0f)
                    dp -= L.lap;
                if (dp < -L.lap / 2.0f)
                    dp += L.lap;
                if (dp < -12.0f || dp > 80.0f)
                    continue;
                const float oLat = L.lateral(o.progress, o.x, o.z);
                // closest approach within lookTime, from both cars' velocities
                const float ovx = -std::sin(o.yaw) * o.speed, ovz = -std::cos(o.yaw) * o.speed;
                const float rx = o.x - me->x, rz = o.z - me->z, wx = ovx - fx * v, wz = ovz - fz * v;
                const float w2 = wx * wx + wz * wz;
                const float tc = w2 > 1e-3f ? std::clamp(-(rx * wx + rz * wz) / w2, 0.0f, lookTime[av]) : 0.0f;
                const float cx = rx + wx * tc, cz = rz + wz * tc;
                const float along = cx * fx + cz * fz, across = cx * -fz + cz * fx; // at the closest approach
                const float ahead = rx * fx + rz * fz;                              // now: + = in front
                if ((dp > -6.0f && dp < 30.0f && std::fabs(oLat - myLat) < 3.5f) || (std::fabs(along) < 4.8f + 0.4f * av && std::fabs(across) < sideRoom[av]))
                    busy = true;
                if (std::fabs(along) < 4.8f + 0.4f * av && std::fabs(across) < sideRoom[av])
                {
                    if (ahead > 2.5f)
                    {
                        const float oAlong = ovx * fx + ovz * fz; // its speed in our direction
                        cap = std::min(cap, oAlong + std::max(0.0f, ahead - 5.0f - v * extraGap[av]) * 0.5f);
                    }
                    else if (ahead > -3.0f)
                        want = oLat + (myLat >= oLat ? sideRoom[av] : -sideRoom[av]); // alongside: make room
                }
                if (dp > 4.5f)
                {
                    const float closing = v - o.speed;
                    const float gap = 5.0f + v * (gapTime[d.passing] + extraGap[av]);
                    if (std::min(std::fabs(oLat - d.offset), std::fabs(oLat - myLat)) > 2.2f || dp > gap + std::max(0.0f, closing) * 1.6f)
                        continue;
                    if (d.passing > 0 || closing > 4.0f)
                    {
                        // the side nearer the line, unless already on one side
                        const float left = oLat + 2.6f, right = oLat - 2.6f;
                        float side = std::fabs(left) < std::fabs(right) ? left : right;
                        if (std::fabs(d.offset - oLat) > 0.6f)
                            side = d.offset > oLat ? left : right;
                        if (std::fabs(side) <= maxOff)
                        {
                            want = side;
                            continue;
                        }
                    }
                    cap = std::min(cap, o.speed + (dp - gap) * 0.6f);
                }
                else if (std::fabs(oLat - myLat) < sideRoom[av])
                {
                    want = oLat + (myLat >= oLat ? sideRoom[av] : -sideRoom[av]);
                    if (dp > 0.0f && std::fabs(oLat - myLat) < 1.9f)
                        cap = std::min(cap, o.speed - 1.0f); // its rear bumper is right there
                }
            }
            want = std::clamp(want, -maxOff, maxOff);
            const float rate = 1.6f * dt; // metres a second sideways
            d.offset += std::clamp(want - d.offset, -rate, rate);
            if (d.gt3Steer)
                d.offset = 0.0f; // GT3 picks its own way round other cars

            // speed: the line's, braking points moved by this lap's offset (+ = later)
            const float lead = v * 0.12f;
            const float shift = d.brakeOff + (d.mistake == 1 ? d.mistakeAmount : 0.0f);
            float target = lineV(p + lead);
            if (shift != 0.0f && lineV(p + lead + 10.0f) < lineV(p + lead) - 0.5f)
                target = lineV(p + lead - shift);
            const float reach = v * v / (2.0f * kPlanDecel) + 30.0f;
            for (float dd = 4.0f; dd < reach; dd += L.step)
            {
                const float vt = lineV(p + dd - shift);
                target = std::min(target, std::sqrt(vt * vt + 2.0f * kPlanDecel * std::max(0.0f, dd - lead)));
            }
            if (d.mistake == 3 && p > d.zoneMin && p < d.zoneEnd)
                target *= 1.0f + d.mistakeAmount;
            target = std::min(target, cap);
            // well off the line (pushed wide, avoiding a car): slower until back on it
            const float offLine = std::fabs(myLat - d.offset);
            if (offLine > 3.0f)
                target = std::min(target, lineV(p) * std::max(0.6f, 1.0f - 0.04f * (offLine - 3.0f)));

            // steering: pure pursuit to a point ahead on the line (and the traffic offset)
            const float look = std::clamp(5.0f + 0.32f * v, 7.0f, 30.0f);
            float tx, tz, ux, uz; // the point ahead, and the line's direction there
            L.at(p + look, tx, tz);
            L.frame(p + look, ux, uz);
            float side = d.offset;
            if (d.mistake == 2 && p > d.zoneMin - 40.0f && p < d.zoneEnd)
                side += (L.curvature(d.zoneMin) > 0.0f ? -1.0f : 1.0f) * d.mistakeAmount; // to the outside of the corner
            tx += -uz * side;
            tz += ux * side;
            const float alpha = wrapAngle(std::atan2(-(tx - me->x), -(tz - me->z)) - me->yaw);
            const float kappa = 2.0f * std::sin(alpha) / look;
            const float need = kappa * kWheelbase * (1.0f + kUndersteer * v * v);
            // GT3's AI never asks for more than ~0.14 rad at racing speed (trace); past the tyres'
            // limit more lock only pushes the front wide, so: a cap by speed, and lift / brake
            // while the line needs more than that
            const float lock = std::clamp(0.55f - 0.018f * v, 0.14f, 0.55f); // 0.19 at 20 m/s, 0.14 from 23 (a spin at 0.6, run ai-lines2)
            // steer gt3: GT3's AI has just set its steering (on the course's own line); keep it
            const float steer = d.gt3Steer ? f32(rdram, controller + kCtlSteer) : std::clamp(need, -lock, lock);
            const float over = d.gt3Steer ? 0.0f : std::fabs(need) / lock;
            const float gt3Throttle = f32(rdram, controller + kCtlThrottle), gt3Brake = f32(rdram, controller + kCtlBrake);

            const float e = target - v;
            float throttle = 0.0f, brake = 0.0f;
            if (e < -1.2f)
                brake = std::clamp(0.3f + (-e - 1.2f) * 0.25f, 0.0f, 1.0f);
            else
                throttle = std::clamp(0.55f + 0.45f * e, 0.0f, d.throttleMax);
            // the tyres' grip is shared: less brake the more lock is on (hard braking with full
            // lock spun car 3 at 2340 m, run ai-lines3); out of lock: lift, don't brake
            brake *= std::clamp(1.15f - std::fabs(steer) / lock * 0.7f, 0.35f, 1.0f);
            if (over > 0.9f && v > 12.0f)
                throttle = std::min(throttle, 0.15f);
            // turning past the road's direction (a slide): off the brakes, a little throttle, the
            // steering already points back along the line
            float rx, rz;
            L.frame(p, rx, rz);
            const float slide = wrapAngle(me->yaw - std::atan2(-rx, -rz));
            if (std::fabs(slide) > 0.35f && v > 5.0f)
            {
                brake = std::min(brake, 0.1f);
                throttle = std::min(std::max(throttle, 0.2f), 0.35f);
            }

            // with GT3 steering, its own avoidance is respected near other cars (it brakes / lifts for them)
            if (d.gt3Steer && busy)
            {
                brake = std::max(brake, gt3Brake);
                throttle = std::min(throttle, gt3Throttle + 0.1f);
            }
            if (!d.gt3Steer)
                put(rdram, controller + kCtlSteer, steer);
            put(rdram, controller + kCtlThrottle, throttle);
            put(rdram, controller + kCtlBrake, brake);
        }

        void trace(const uint8_t *rdram, uint32_t controller, const gt3race::Snapshot &s, uint32_t k)
        {
            if (++g_frame[k] % 6u != 0u)
                return;
            const gt3race::Car *c = carOf(s, static_cast<int>(k));
            if (!c)
                return;
            const uint32_t car = controller - kControllerOffset;
            // controller words +0x80..+0xbc as floats (and the two shorts at +0xa8 / +0xaa),
            // the car's own throttle (+0x13ac), brake (+0x13b0) and steering target (+0x13a0)
            std::fprintf(stderr, "[ai-trace] car %u clock %u lap_m %.1f v %.2f yaw %.4f x %.2f z %.2f | car thr %.3f brk %.3f steer %.4f | ctl",
                         k, s.clockMs, c->progress, c->speed, c->yaw, c->x, c->z, f32(rdram, car + 0x13acu), f32(rdram, car + 0x13b0u),
                         f32(rdram, car + 0x13a0u));
            for (uint32_t o = 0x80u; o < 0xc0u; o += 4u)
            {
                const uint32_t w = u32(rdram, controller + o);
                const float f = f32(rdram, controller + o);
                if (o == 0xa8u)
                    std::fprintf(stderr, " %02x:%04x/%04x", o, w & 0xffffu, w >> 16);
                else if (f == f && f > -1e6f && f < 1e6f && (w == 0u || ((w >> 23) & 0xffu) > 0x60u))
                    std::fprintf(stderr, " %02x:%.4f", o, f);
                else
                    std::fprintf(stderr, " %02x:#%08x", o, w);
            }
            std::fputc('\n', stderr);
        }

        // one race state a frame (the AI driver runs once a car)
        gt3race::Snapshot g_snap;
        uint32_t g_snapVblank = ~0u;
        bool g_snapOk = false;

        void aiHook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            const uint32_t controller = static_cast<uint32_t>(_mm_extract_epi32(ctx->r[4], 0)) & kRamMask;
            g_next(rdram, ctx, runtime);
            const uint32_t vblank = u32(rdram, kVblankCounter);
            if (vblank != g_snapVblank)
            {
                g_snapVblank = vblank;
                g_snapOk = gt3race::read(rdram, g_snap, false) && !g_snap.replay;
            }
            if (!g_snapOk)
                return;
            const gt3race::Snapshot &s = g_snap;
            const uint32_t car = controller - kControllerOffset;
            const uint32_t rel = car - (s.race + kFirstCar);
            if (rel % kCarSize != 0u)
                return;
            const uint32_t k = rel / kCarSize;
            if (k < 1u || k > 5u)
                return;
            if (g_linesOn)
            {
                if (s.race != g_race)
                {
                    g_race = s.race;
                    configure();
                }
                if (g_drivers[k].on)
                    drive(rdram, controller, s, static_cast<int>(k));
            }
            if (g_trace)
                trace(rdram, controller, s, k);
        }
    }

    void install(PS2Runtime &runtime, const uint8_t *rdram)
    {
        const char *t = std::getenv("GT3_AI_TRACE");
        const char *l = std::getenv("GT3_AI_LINES");
        g_trace = t && *t && *t != '0';
        g_linesOn = l && *l;
        if ((!g_trace && !g_linesOn) || !rdram)
            return;
        if (g_linesOn)
            g_folder = l;
        uint32_t first;
        std::memcpy(&first, rdram + kAiDriver, 4);
        // addiu sp, sp, -0x110 (GT3 USA CORE); chains after the autopilot's wrapper when it has one
        if (first != 0x27bdfef0u || (g_next = runtime.lookupFunction(kAiDriver)) == nullptr)
        {
            std::fprintf(stderr, "[ai-pilot] GT3's AI driver not found: off\n");
            g_trace = g_linesOn = false;
            return;
        }
        runtime.registerFunction(kAiDriver, &aiHook);
        if (g_trace)
            std::fprintf(stderr, "[ai-trace] on: AI cars 1-5, every 6th frame\n");
        if (g_linesOn)
            std::fprintf(stderr, "[ai-pilot] on: AI cars 1-5 drive the lines in %s (<course>.txt)\n", g_folder.string().c_str());
    }
}
