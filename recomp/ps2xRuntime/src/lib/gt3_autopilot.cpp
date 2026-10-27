#include "gt3_autopilot.h"

#include "gt3_race_info.h"
#include "gt3_shot.h"
#include "ps2_runtime.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <sstream>
#include <string>
#include <vector>

namespace gt3shot
{
    std::atomic<int> request{-1};
}

namespace gt3autopilot
{
    namespace
    {
        constexpr float kBin = 2.0f;        // metres of track progress per line step
        constexpr uint16_t kCross = 0x4000u, kSquare = 0x8000u;
        constexpr float kMaxScale = 1.6f; // learned speed: at most this times the AI's
        // The learned grip factors' ceiling (GT3_AUTOPILOT_GRIP_MAX, default 2.5:
        // with the base grip of 8 m/s^2, 20 m/s^2 in the plan at most; it binds
        // mostly where the car is power-limited anyway).
        float gripMax()
        {
            static const float value = [] {
                const char *v = std::getenv("GT3_AUTOPILOT_GRIP_MAX");
                return std::clamp(v && *v ? static_cast<float>(std::atof(v)) : 2.5f, 1.0f, 3.0f); // 2.5: 0.3-0.5 s a lap over 2.0 (`ap-exp` J vs K), 2.0 ~0.3 s over 1.6 (B vs A)
            }();
            return value;
        }
        // The learned braking rate's ceiling (GT3_AUTOPILOT_BRAKE_MAX, m/s^2,
        // default 12) and a forced start (GT3_AUTOPILOT_BRAKE, m/s^2; unset:
        // the saved plan's).
        float brakeMax()
        {
            static const float value = [] {
                const char *v = std::getenv("GT3_AUTOPILOT_BRAKE_MAX");
                return std::clamp(v && *v ? static_cast<float>(std::atof(v)) : 12.0f, 5.0f, 16.0f);
            }();
            return value;
        }
        float brakeStart()
        {
            static const float value = [] {
                const char *v = std::getenv("GT3_AUTOPILOT_BRAKE");
                return v && *v ? std::clamp(static_cast<float>(std::atof(v)), 5.0f, 16.0f) : -1.0f;
            }();
            return value;
        }

        struct Bin
        {
            double sx = 0.0, sz = 0.0, sv = 0.0;
            uint32_t n = 0, nv = 0;
            float scale = 1.0f; // learned fraction of the AI's speed
        };

        struct Tracked
        {
            float x = 0.0f, z = 0.0f;
            uint32_t clock = 0; // race clock (ms) of the sample
            bool valid = false;
        };

        std::mutex g_mutex;
        std::vector<Bin> g_bins;
        std::vector<float> g_routeX, g_routeZ; // the racing line per step (NaN: none there), empty = the AI's line
        float g_lapLength = 0.0f;
        uint32_t g_race = 0;
        Tracked g_cars[8];
        uint64_t g_lastTick = ~0ull;
        std::array<uint8_t, 32> g_packet{};

        // GT3's AI driver (0x1f7ae0, a0 = a car's controller, car + 0xec8).
        // When it drives the player's car - Free Run's AUTO DRIVE up to the
        // start line, the menus' moving course previews (the Free Run menu's:
        // the autopilot took the pad there and its brake, Square, backed out
        // of the menu, 2026-10-01) - the autopilot keeps its hands off the pad
        // and learns nothing (it "learned" braking rates from GT3's own driving).
        constexpr uint32_t kAiDriver = 0x1f7ae0u, kVblankCounter = 0x4a69c0u;
        PS2Runtime::RecompiledFunction g_aiDriver = nullptr;
        std::atomic<uint32_t> g_aiControllers[4]{}; // the last controllers it ran for
        std::atomic<uint32_t> g_aiVblank[4]{};      // and when (GT3's VBlank counter)
        std::atomic<uint32_t> g_aiNext{0};
        void aiDriverHook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            const uint32_t controller = static_cast<uint32_t>(_mm_extract_epi32(ctx->r[4], 0)) & 0x1FFFFFFu;
            uint32_t vblank;
            std::memcpy(&vblank, rdram + kVblankCounter, 4);
            // One slot per controller (AI cars run every frame too).
            int slot = -1;
            for (int i = 0; i < 4; ++i)
                if (g_aiControllers[i].load(std::memory_order_relaxed) == controller)
                    slot = i;
            if (slot < 0)
                slot = static_cast<int>(g_aiNext.fetch_add(1, std::memory_order_relaxed) & 3u);
            g_aiControllers[slot].store(controller, std::memory_order_relaxed);
            g_aiVblank[slot].store(vblank, std::memory_order_relaxed);
            g_aiDriver(rdram, ctx, runtime);
        }
        // The AI driver ran for this car's controller in the last ~half second.
        bool aiDrives(const uint8_t *rdram, uint32_t car)
        {
            if (!g_aiDriver)
                return false;
            uint32_t vblank;
            std::memcpy(&vblank, rdram + kVblankCounter, 4);
            const uint32_t controller = (car + 0xec8u) & 0x1FFFFFFu;
            for (int i = 0; i < 4; ++i)
                if (g_aiControllers[i].load(std::memory_order_relaxed) == controller && vblank - g_aiVblank[i].load(std::memory_order_relaxed) < 30u)
                    return true;
            return false;
        }
        bool g_driving = false;
        float g_speed = 0.0f, g_prevSpeed = 0.0f;
        uint32_t g_prevSpeedClock = 0;
        constexpr uint32_t kHistory = 32;
        struct SpeedSample
        {
            float speed = 0.0f;
            uint32_t clock = 0;
        };
        Tracked g_history[kHistory];
        SpeedSample g_speeds[kHistory];
        uint32_t g_historyHead = 0;
        float g_throttleAcc = 0.0f, g_brakeAcc = 0.0f;
        uint64_t g_stuckSince = 0, g_reverseUntil = 0;
        int g_lastLap = -1;
        uint32_t g_lastLapClock = 0; // race clock at the last lap line (0: none yet)
        uint32_t g_lastLapMs = 0;
        uint32_t g_incidents = 0;
        uint64_t g_lastSave = 0, g_frames = 0;
        uint32_t g_lastClock = 0;
        bool g_loaded = false;
        bool g_learningFromGt3 = false; // the line is being learned from GT3 driving the player's car
        void ensureBins(float progress);

        // Saved lines: <dir>/line-<stem>.txt (GT3_AUTOPILOT_DIR, default
        // "autopilot" in the working folder), the stem being the lap in metres,
        // or "<metres>-<n>" when another course's line has that length (courses
        // of nearly one length exist: Mid-Field II 3588 m, Deep Forest 3589 m,
        // Mid-Field 3598 m). A course is recognised by its lap length (within
        // 15 m) and the line passing near the player's car, or (no lap length
        // yet) by the place alone. walls-, route- and plan- files share the stem.
        std::string g_lineStem; // set when a line is loaded or first saved
        std::filesystem::path lineDir()
        {
            const char *v = std::getenv("GT3_AUTOPILOT_DIR");
            return std::filesystem::path(v && *v ? v : "autopilot");
        }
        // Whether a saved line (its steps after the header, read from `in`)
        // passes within `tolerance` metres of (x, z) at `progress`.
        bool linePassesNear(std::istream &in, float length, float x, float z, float progress, double tolerance)
        {
            if (!std::isfinite(progress) || progress < 0.0f || progress > length + 50.0f || length < 1.0f)
                return false;
            const size_t want = static_cast<size_t>(std::fmod(progress, length) / kBin);
            size_t i;
            double px, pz, v;
            uint32_t n, nv;
            float scale;
            while (in >> i >> px >> pz >> v >> n >> nv >> scale)
                if (i + 2 >= want && i <= want + 2)
                    return std::hypot(px - x, pz - z) < tolerance;
            return false;
        }
        // The stem for this course's files: the loaded line's, else the lap
        // length, with "-2", "-3", ... while a line of another course holds the
        // name (a third or more of the steps both have more than 20 m apart).
        std::string lineStem()
        {
            if (!g_lineStem.empty())
                return g_lineStem;
            const std::string base = std::to_string(static_cast<int>(g_lapLength + 0.5f));
            if (g_lapLength < 100.0f || g_bins.empty())
                return base; // (decided once there is a line to compare)
            for (int k = 1; k < 10; ++k)
            {
                const std::string stem = k == 1 ? base : base + "-" + std::to_string(k);
                std::ifstream in(lineDir() / ("line-" + stem + ".txt"));
                std::string magic;
                int version = 0;
                float length = 0.0f, bin = 0.0f;
                if (!in || !(in >> magic >> version >> length >> bin) || magic != "gt3-autopilot-line")
                    return g_lineStem = stem; // free (or not a line)
                size_t i, both = 0, apart = 0;
                double px, pz, v;
                uint32_t n, nv;
                float scale;
                while (in >> i >> px >> pz >> v >> n >> nv >> scale)
                    if (i < g_bins.size() && g_bins[i].n > 0 && (i % 25u) == 0u)
                    {
                        ++both;
                        if (std::hypot(px - g_bins[i].sx / g_bins[i].n, pz - g_bins[i].sz / g_bins[i].n) > 20.0)
                            ++apart;
                    }
                if (both == 0)
                    return stem; // nothing to compare yet
                if (apart * 3 < both)
                    return g_lineStem = stem; // this course's own line
            }
            return base;
        }

        void saveLine()
        {
            if (g_lapLength < 100.0f || g_bins.empty())
                return;
            std::error_code ec;
            std::filesystem::create_directories(lineDir(), ec);
            const std::filesystem::path file = lineDir() / ("line-" + lineStem() + ".txt");
            std::ofstream out(file, std::ios::trunc);
            if (!out)
                return;
            out << "gt3-autopilot-line 1 " << g_lapLength << " " << kBin << "\n";
            for (size_t i = 0; i < g_bins.size(); ++i)
            {
                const Bin &b = g_bins[i];
                if (b.n == 0)
                    continue;
                out << i << " " << b.sx / b.n << " " << b.sz / b.n << " " << (b.nv ? b.sv / b.nv : -1.0) << " " << std::min<uint32_t>(b.n, 30u)
                    << " " << std::min<uint32_t>(b.nv, 30u) << " " << b.scale << "\n";
            }
        }

        // A saved line for a lap of about `lapLength` metres, if any; else (a
        // race without AI cars, e.g. Time Trial: no lap length yet) one whose
        // point at the player's progress is within 20 m of the player.
        void loadLine(float lapLength, float x, float z, float progress)
        {
            std::error_code ec;
            std::filesystem::path chosen;
            for (int pass = 0; pass < 2 && chosen.empty(); ++pass)
                for (const auto &entry : std::filesystem::directory_iterator(lineDir(), ec))
                {
                    const std::string name = entry.path().filename().string();
                    if (name.rfind("line-", 0) != 0)
                        continue;
                    std::ifstream in(entry.path());
                    std::string magic;
                    int version = 0;
                    float length = 0.0f, bin = 0.0f;
                    if (!(in >> magic >> version >> length >> bin) || magic != "gt3-autopilot-line" || bin != kBin)
                        continue;
                    if (pass == 0)
                    {
                        // The same length, and passing near the car when its
                        // place on the lap is known (grid slots are off the line).
                        const bool placeKnown = std::isfinite(progress) && progress >= 0.0f && progress <= length + 50.0f;
                        if (std::fabs(length - lapLength) <= 15.0f && (!placeKnown || linePassesNear(in, length, x, z, progress, 40.0)))
                            chosen = entry.path();
                    }
                    else if (linePassesNear(in, length, x, z, progress, 20.0))
                        chosen = entry.path();
                    if (!chosen.empty())
                        break;
                }
            if (chosen.empty())
                return;
            {
                std::ifstream in(chosen);
                std::string magic;
                int version = 0;
                float length = 0.0f, bin = 0.0f;
                in >> magic >> version >> length >> bin;
                const auto &entry = std::filesystem::directory_entry(chosen);
                g_lineStem = chosen.stem().string().substr(5); // "line-<stem>"
                g_bins.clear();
                g_lapLength = std::max(g_lapLength, length);
                size_t i;
                double lineX, lineZ, v;
                uint32_t n, nv;
                float scale;
                size_t count = 0;
                while (in >> i >> lineX >> lineZ >> v >> n >> nv >> scale)
                {
                    if (i >= 20000u)
                        break;
                    if (i >= g_bins.size())
                        ensureBins(static_cast<float>(i) * kBin);
                    Bin &b = g_bins[i];
                    b.sx = lineX * n;
                    b.sz = lineZ * n;
                    b.n = n;
                    b.sv = v > 0.0 ? v * nv : 0.0;
                    b.nv = v > 0.0 ? nv : 0u;
                    b.scale = std::clamp(scale, 0.4f, kMaxScale);
                    ++count;
                }
                std::fprintf(stderr, "[autopilot] loaded %s: %zu steps of a %.0f m lap\n", entry.path().string().c_str(), count, length);
                return;
            }
        }

        float startScale()
        {
            static const float value = [] {
                const char *v = std::getenv("GT3_AUTOPILOT_SPEED");
                const float s = v && *v ? static_cast<float>(std::atof(v)) : 0.92f;
                return std::clamp(s, 0.5f, 1.2f);
            }();
            return value;
        }
        bool logging()
        {
            static const bool on = [] {
                const char *v = std::getenv("GT3_AUTOPILOT_LOG");
                return v && *v && *v != '0';
            }();
            return on;
        }

        size_t binOf(float progress)
        {
            if (g_bins.empty())
                return 0;
            float p = progress;
            if (g_lapLength > 1.0f)
                p = std::fmod(std::fmod(p, g_lapLength) + g_lapLength, g_lapLength);
            return std::min(g_bins.size() - 1, static_cast<size_t>(std::max(0.0f, p) / kBin));
        }

        void ensureBins(float progress)
        {
            const size_t need = static_cast<size_t>(std::max(0.0f, progress) / kBin) + 2u;
            if (need > g_bins.size() && need < 20000u)
            {
                const size_t old = g_bins.size();
                g_bins.resize(need);
                for (size_t i = old; i < need; ++i)
                    g_bins[i].scale = startScale();
            }
        }

        // The line's point and speed around `progress` (nearest learned bins).
        bool linePoint(float progress, float &x, float &z)
        {
            const size_t b = binOf(progress);
            for (size_t d = 0; d < 4; ++d)
                for (const int sign : {1, -1})
                {
                    const long i = static_cast<long>(b) + sign * static_cast<long>(d);
                    if (i < 0 || static_cast<size_t>(i) >= g_bins.size())
                        continue;
                    if (static_cast<size_t>(i) < g_routeX.size() && std::isfinite(g_routeX[static_cast<size_t>(i)]))
                    {
                        x = g_routeX[static_cast<size_t>(i)];
                        z = g_routeZ[static_cast<size_t>(i)];
                        return true;
                    }
                    const Bin &bin = g_bins[static_cast<size_t>(i)];
                    if (bin.n > 0)
                    {
                        x = static_cast<float>(bin.sx / bin.n);
                        z = static_cast<float>(bin.sz / bin.n);
                        return true;
                    }
                }
            return false;
        }
        // Target speed (m/s) at a bin, or -1 when not learned.
        float binSpeed(size_t i)
        {
            if (i >= g_bins.size() || g_bins[i].nv == 0)
                return -1.0f;
            return static_cast<float>(g_bins[i].sv / g_bins[i].nv) * g_bins[i].scale;
        }

        // Game time, not VSyncs: GT3 steps its physics once per frame and
        // the race clock follows those frames (turbo VSyncs can skip).
        void learnFromAi(const gt3race::Snapshot &s, bool playerCar = false)
        {
            for (const gt3race::Car &c : s.cars)
            {
                if ((playerCar ? c.index != 0 : (c.index <= 0 || c.index >= 8)) || !std::isfinite(c.progress) || c.progress < 0.0f ||
                    c.progress > 1e5f)
                    continue;
                Tracked &t = g_cars[c.index];
                g_lapLength = std::max(g_lapLength, c.progress + 1.0f);
                ensureBins(c.progress);
                Bin &bin = g_bins[binOf(c.progress)];
                bin.sx += c.x;
                bin.sz += c.z;
                ++bin.n;
                // Speeds over at least 50 ms of race time (see step()).
                if (t.valid && s.clockMs > t.clock && s.clockMs - t.clock < 50u)
                    continue;
                if (t.valid && s.clockMs > t.clock && s.clockMs - t.clock <= 300u)
                {
                    const float d = std::hypot(c.x - t.x, c.z - t.z);
                    const float v = d / ((s.clockMs - t.clock) / 1000.0f);
                    if (v > 1.0f && v < 120.0f)
                    {
                        bin.sv += v;
                        ++bin.nv;
                    }
                }
                t = {c.x, c.z, s.clockMs, true};
            }
        }

        // Lower the target speeds over the `metres` of line before `progress`.
        void penalise(float progress, float metres, float factor)
        {
            if (g_bins.empty() || !std::isfinite(progress))
                return;
            const int steps = static_cast<int>((metres + 4.0f) / kBin) + 1;
            for (int k = 0; k < steps; ++k)
            {
                Bin &b = g_bins[binOf(progress - metres + static_cast<float>(k) * kBin)];
                b.scale = std::max(0.4f, b.scale * factor);
            }
        }

        // ---- A racing line within the course's edges (GT3_AUTOPILOT_ROUTE=ai: the AI's own line)
        // GT3 keeps the course's two edges in RAM (gt3race::findCourseEdges).
        // Across every 2 m step of the AI's (smoothed) line the road reaches
        // lo..hi metres along the line's normal; the route is the line that
        // bends least within those bounds (kEdgeMargin in from each edge): the
        // minimum-curvature line, a close stand-in for the fastest one. It is
        // relaxed on a 24 m grid, then on a 6 m one.
        bool routeEnabled()
        {
            static const bool on = [] {
                const char *v = std::getenv("GT3_AUTOPILOT_ROUTE");
                return !(v && std::strcmp(v, "ai") == 0);
            }();
            return on;
        }
        float edgeMargin()
        {
            static const float value = [] {
                const char *v = std::getenv("GT3_AUTOPILOT_MARGIN");
                return std::clamp(v && *v ? static_cast<float>(std::atof(v)) : 1.8f, 0.5f, 6.0f);
            }();
            return value;
        }
        float routeWidth()
        {
            static const float value = [] {
                const char *v = std::getenv("GT3_AUTOPILOT_ROUTE_WIDTH");
                return std::clamp(v && *v ? static_cast<float>(std::atof(v)) : 4.5f, 0.0f, 30.0f); // 4.5: 1'53.8 vs 1'56.9 at 3 (`frw45`)
            }();
            return value;
        }
        int g_routeTries = 0;
        uint64_t g_routeCheck = 0;
        bool g_planDirty = true;

        // The AI's line smoothed (a +-4 step moving average; gaps skipped).
        void smoothLine(std::vector<float> &px, std::vector<float> &pz, std::vector<char> &have)
        {
            const size_t n = g_bins.size();
            px.assign(n, 0.0f);
            pz.assign(n, 0.0f);
            have.assign(n, 0);
            for (size_t i = 0; i < n; ++i)
            {
                double sx = 0.0, sz = 0.0;
                int c = 0;
                for (int d = -4; d <= 4; ++d)
                {
                    const Bin &b = g_bins[(i + n + static_cast<size_t>(d + static_cast<int>(n))) % n];
                    if (b.n == 0)
                        continue;
                    sx += b.sx / b.n;
                    sz += b.sz / b.n;
                    ++c;
                }
                if (c >= 3)
                {
                    px[i] = static_cast<float>(sx / c);
                    pz[i] = static_cast<float>(sz / c);
                    have[i] = 1;
                }
            }
        }

        // The line the car follows: the route where there is one, else the AI's.
        void linePoints(std::vector<float> &px, std::vector<float> &pz, std::vector<char> &have)
        {
            smoothLine(px, pz, have);
            if (g_routeX.size() != px.size())
                return;
            for (size_t i = 0; i < px.size(); ++i)
                if (std::isfinite(g_routeX[i]))
                {
                    px[i] = g_routeX[i];
                    pz[i] = g_routeZ[i];
                    have[i] = 1;
                }
        }

        // The route's frame: the AI's smoothed line, its normals and the road
        // either side (lo < 0 < hi, metres along the normal), plus limits
        // learned from hits (walls-<lap metres>.txt).
        struct RouteFrame
        {
            std::vector<float> px, pz, nx, nz, lo, hi, wallLo, wallHi;
            std::vector<char> have, ok;
            std::vector<std::vector<float>> edges;
        };
        RouteFrame g_frame;
        bool g_routeDirty = false;
        uint64_t g_routeRelaxed = 0;

        std::filesystem::path wallFile()
        {
            return lineDir() / ("walls-" + lineStem() + ".txt");
        }
        void saveWalls()
        {
            const RouteFrame &f = g_frame;
            std::error_code ec;
            std::filesystem::create_directories(lineDir(), ec);
            std::ofstream out(wallFile(), std::ios::trunc);
            if (!out)
                return;
            out << "gt3-autopilot-walls 1 " << f.wallLo.size() << "\n";
            for (size_t i = 0; i < f.wallLo.size(); ++i)
                if (f.wallLo[i] > -1e8f || f.wallHi[i] < 1e8f)
                    out << i << " " << f.wallLo[i] << " " << f.wallHi[i] << "\n";
        }
        void loadWalls()
        {
            RouteFrame &f = g_frame;
            if (g_lineStem.empty())
                return; // (the stem is not settled: walls of this course only)
            std::ifstream in(wallFile());
            std::string magic;
            int version = 0;
            size_t n = 0;
            if (!(in >> magic >> version >> n) || magic != "gt3-autopilot-walls" || n != f.wallLo.size())
                return;
            size_t i, count = 0;
            float l, h;
            while (in >> i >> l >> h)
                if (i < n)
                {
                    f.wallLo[i] = l;
                    f.wallHi[i] = h;
                    ++count;
                }
            std::fprintf(stderr, "[autopilot] route: %zu steps with learned limits\n", count);
        }

        // The route between the bounds (edges, the AI line's width, the learned
        // limits): relaxation toward the least bending, each point moving to
        // where its second differences with its neighbours cancel, along its
        // normal, clamped to the road; on a 24 m grid, then a 6 m one.
        void relaxRoute(int iterations)
        {
            const RouteFrame &f = g_frame;
            const size_t n = f.px.size();
            if (n < 50)
                return;
            std::vector<float> lo(n, 0.0f), hi(n, 0.0f);
            for (size_t i = 0; i < n; ++i)
                if (f.ok[i])
                {
                    lo[i] = std::min(0.0f, std::max(f.lo[i], f.wallLo[i]));
                    hi[i] = std::max(0.0f, std::min(f.hi[i], f.wallHi[i]));
                    if (lo[i] > hi[i])
                        lo[i] = hi[i] = 0.5f * (lo[i] + hi[i]);
                }
            std::vector<float> off(n, 0.0f);
            for (const size_t stride : {size_t{12}, size_t{3}})
            {
                const size_t m = n / stride;
                std::vector<float> a(m), bl(m), bh(m);
                for (size_t j = 0; j < m; ++j)
                {
                    const size_t i = j * stride;
                    float l = -1e9f, h = 1e9f;
                    for (size_t d = 0; d < stride; ++d)
                    {
                        const size_t k = (i + n - stride / 2 + d) % n;
                        l = std::max(l, lo[k]);
                        h = std::min(h, hi[k]);
                    }
                    if (l > h)
                        l = h = 0.5f * (l + h);
                    bl[j] = l;
                    bh[j] = h;
                    a[j] = std::clamp(off[i], l, h);
                }
                for (int it = 0; it < iterations; ++it)
                    for (size_t j = 0; j < m; ++j)
                    {
                        float qx[5], qz[5];
                        for (int d = -2; d <= 2; ++d)
                        {
                            const size_t jj = (j + m + static_cast<size_t>(d + static_cast<int>(m))) % m, i = jj * stride;
                            qx[d + 2] = f.px[i] + a[jj] * f.nx[i];
                            qz[d + 2] = f.pz[i] + a[jj] * f.nz[i];
                        }
                        const float tx = (4.0f * (qx[1] + qx[3]) - (qx[0] + qx[4])) / 6.0f;
                        const float tz = (4.0f * (qz[1] + qz[3]) - (qz[0] + qz[4])) / 6.0f;
                        const size_t i = j * stride;
                        a[j] = std::clamp((tx - f.px[i]) * f.nx[i] + (tz - f.pz[i]) * f.nz[i], bl[j], bh[j]);
                    }
                for (size_t i = 0; i < n; ++i)
                {
                    const size_t j = std::min(m - 1, i / stride);
                    const float t = static_cast<float>(i - j * stride) / static_cast<float>(stride);
                    off[i] = a[j] * (1.0f - t) + a[(j + 1) % m] * t;
                }
            }
            g_routeX.assign(n, NAN);
            g_routeZ.assign(n, NAN);
            for (size_t i = 0; i < n; ++i)
                if (f.have[i] && (f.nx[i] != 0.0f || f.nz[i] != 0.0f))
                {
                    g_routeX[i] = f.px[i] + off[i] * f.nx[i];
                    g_routeZ[i] = f.pz[i] + off[i] * f.nz[i];
                }
            g_planDirty = true;
            g_routeDirty = false;
        }

        // For a look: <dir>/route-<lap metres>.txt, "s" lines (step, the AI's
        // point, its normal, the road's lo / hi along it, the route's point)
        // and "e" lines (edge number, x, z).
        void writeRoute()
        {
            const RouteFrame &f = g_frame;
            std::error_code ec;
            std::filesystem::create_directories(lineDir(), ec);
            std::ofstream out(lineDir() / ("route-" + lineStem() + ".txt"), std::ios::trunc);
            for (size_t i = 0; i < f.px.size() && out; ++i)
                if (f.have[i] && i < g_routeX.size())
                    out << "s " << i << " " << f.px[i] << " " << f.pz[i] << " " << f.nx[i] << " " << f.nz[i] << " "
                        << (f.ok[i] ? std::max(f.lo[i], f.wallLo[i]) : 0.0f) << " " << (f.ok[i] ? std::min(f.hi[i], f.wallHi[i]) : 0.0f) << " "
                        << g_routeX[i] << " " << g_routeZ[i] << "\n";
            for (size_t e = 0; e < f.edges.size() && out; ++e)
                for (size_t k = 0; k + 1 < f.edges[e].size(); k += 2)
                    out << "e " << e << " " << f.edges[e][k] << " " << f.edges[e][k + 1] << "\n";
        }

        bool buildRoute(const std::vector<std::vector<float>> &edges)
        {
            const size_t n = g_bins.size();
            RouteFrame &f = g_frame;
            smoothLine(f.px, f.pz, f.have);
            f.nx.assign(n, 0.0f);
            f.nz.assign(n, 0.0f);
            f.lo.assign(n, 0.0f);
            f.hi.assign(n, 0.0f);
            f.ok.assign(n, 0);
            f.wallLo.assign(n, -1e9f);
            f.wallHi.assign(n, 1e9f);
            f.edges = edges;
            std::vector<float> widths;
            const float margin = edgeMargin();
            for (size_t i = 0; i < n; ++i)
            {
                const size_t a = (i + n - 2) % n, c = (i + 2) % n;
                if (!f.have[i] || !f.have[a] || !f.have[c])
                    continue;
                float tx = f.px[c] - f.px[a], tz = f.pz[c] - f.pz[a];
                const float len = std::hypot(tx, tz);
                if (len < 1e-3f)
                    continue;
                tx /= len;
                tz /= len;
                const float nx = -tz, nz = tx, px = f.px[i], pz = f.pz[i];
                f.nx[i] = nx;
                f.nz[i] = nz;
                // The nearest edge crossing either side along the normal.
                float neg = -1e9f, pos = 1e9f;
                for (const std::vector<float> &e : edges)
                {
                    const size_t m = e.size() / 2;
                    for (size_t k = 0; k < m; ++k)
                    {
                        const float ax = e[2 * k], az = e[2 * k + 1], bx = e[2 * ((k + 1) % m)], bz = e[2 * ((k + 1) % m) + 1];
                        if ((ax - px > 60.0f && bx - px > 60.0f) || (px - ax > 60.0f && px - bx > 60.0f) || (az - pz > 60.0f && bz - pz > 60.0f) ||
                            (pz - az > 60.0f && pz - bz > 60.0f))
                            continue;
                        const float ex = bx - ax, ez = bz - az;
                        const float denom = nx * ez - nz * ex;
                        if (std::fabs(denom) < 1e-6f)
                            continue;
                        const float wx = ax - px, wz = az - pz;
                        const float s = (wx * ez - wz * ex) / denom;
                        const float u = (wx * nz - wz * nx) / denom;
                        if (u < 0.0f || u > 1.0f)
                            continue;
                        if (s < 0.0f)
                            neg = std::max(neg, s);
                        else
                            pos = std::min(pos, s);
                    }
                }
                if (neg < -0.5f && neg > -40.0f && pos > 0.5f && pos < 40.0f)
                {
                    // GT3's two lines are the course area's limits (walls,
                    // run-off; 25 m apart on average at Trial Mountain), not
                    // the tarmac: the route also stays within routeWidth()
                    // of the AI's line.
                    f.lo[i] = std::max(neg + margin, -routeWidth());
                    f.hi[i] = std::min(pos - margin, routeWidth());
                    f.ok[i] = 1;
                    widths.push_back(pos - neg);
                }
            }
            if (widths.size() < n * 9 / 10)
            {
                std::fprintf(stderr, "[autopilot] route: the edges cross only %zu of %zu steps; keeping the AI's line\n", widths.size(), n);
                f.px.clear();
                return false;
            }
            std::sort(widths.begin(), widths.end());
            loadWalls();
            relaxRoute(4000);
            float offMax = 0.0f;
            for (size_t i = 0; i < n; ++i)
                if (std::isfinite(g_routeX[i]))
                    offMax = std::max(offMax, std::hypot(g_routeX[i] - f.px[i], g_routeZ[i] - f.pz[i]));
            std::fprintf(stderr, "[autopilot] route: course area %.1f m wide (median; %.1f..%.1f), %.1f m in from its limits, up to %.1f m off the AI's line\n",
                         widths[widths.size() / 2], widths.front(), widths.back(), margin, offMax);
            writeRoute();
            return true;
        }

        // A hit (a sudden stop) with the car `side` metres along the normal
        // at `progress`: over the 40 m before and 10 m after, the route keeps
        // 1.5 m further in from there (never past the AI's line).
        void learnWall(float progress, float x, float z)
        {
            RouteFrame &f = g_frame;
            if (f.px.size() != g_bins.size() || f.px.empty())
                return;
            const size_t i = binOf(progress);
            if (!f.ok[i])
                return;
            const float side = (x - f.px[i]) * f.nx[i] + (z - f.pz[i]) * f.nz[i];
            if (std::fabs(side) < 0.5f)
                return;
            const size_t n = f.px.size();
            for (int d = -20; d <= 5; ++d)
            {
                const size_t k = (i + n + static_cast<size_t>(d + static_cast<int>(n))) % n;
                if (side > 0.0f)
                    f.wallHi[k] = std::max(0.0f, std::min(f.wallHi[k], side - 1.5f));
                else
                    f.wallLo[k] = std::min(0.0f, std::max(f.wallLo[k], side + 1.5f));
            }
            g_routeDirty = true;
            if (logging())
                std::fprintf(stderr, "[autopilot] route: a hit / gravel %.1f m %s of the AI's line at %.0f m; the route keeps in from there\n", std::fabs(side),
                             side > 0.0f ? "+" : "-", progress);
        }

        // ---- Speed plan from the line's shape (GT3_AUTOPILOT_PLAN, default on)
        // Each 2 m step of the (smoothed) line gets its curvature k; the car
        // can take it at v = sqrt(grip / k), grip = a learned lateral
        // acceleration (m/s^2) times the step's learned factor. A backward
        // pass then limits every step to what braking at `g_brake` m/s^2 can
        // reach before the next ones: the target speed along the whole lap.
        // Learning (per lap and per incident) moves the grip factors, so the
        // car finds each corner's limit instead of following the AI's speed.
        bool planEnabled()
        {
            static const bool on = [] {
                const char *v = std::getenv("GT3_AUTOPILOT_PLAN");
                return !(v && *v == '0');
            }();
            return on;
        }
        float g_grip = 8.0f;           // m/s^2 lateral, learned
        float g_brake = 8.0f;          // m/s^2, learned from full braking
        std::vector<float> g_gripScale; // per step (0.5 .. gripMax())
        std::vector<float> g_brakeScale; // per step (0.75 .. 1): the braking rate there
        std::vector<float> g_plan;      // target speed per step (m/s), empty = not built
        std::vector<float> g_curv;      // curvature per step (1/m)

        // Braking learned per section (GT3_AUTOPILOT_BRAKE_LOCAL, default on; 0
        // off): where full
        // braking still leaves the car over 2 m/s above the plan, that zone's
        // rate goes down (into the 2,870 m hairpin the car stayed up to 4 m/s
        // over at 12 m/s^2 for over a second, `ap-O0`); back up 1% a clean lap.
        bool brakeLocal()
        {
            static const bool on = [] {
                const char *v = std::getenv("GT3_AUTOPILOT_BRAKE_LOCAL");
                return !(v && *v == '0'); // on by default (`ap-exp` P1 vs P0: ~0.2 s a lap)
            }();
            return on;
        }

        void buildPlan()
        {
            const size_t n = g_bins.size();
            g_plan.assign(n, -1.0f);
            g_curv.assign(n, 0.0f);
            if (g_gripScale.size() != n)
                g_gripScale.resize(n, 1.0f);
            if (g_brakeScale.size() != n)
                g_brakeScale.resize(n, 1.0f);
            if (n < 50 || g_lapLength < 100.0f)
                return;
            std::vector<float> px, pz;
            std::vector<char> have;
            linePoints(px, pz, have);
            // Curvature through the points 12 m either side (Menger: 4 area / abc).
            constexpr int k = 6;
            std::vector<float> limit(n, 90.0f);
            for (size_t i = 0; i < n; ++i)
            {
                const size_t a = (i + n - k) % n, c = (i + k) % n;
                if (!have[a] || !have[i] || !have[c])
                    continue;
                const float abx = px[i] - px[a], abz = pz[i] - pz[a], acx = px[c] - px[a], acz = pz[c] - pz[a];
                const float area2 = std::fabs(abx * acz - abz * acx);
                const float la = std::hypot(abx, abz), lb = std::hypot(px[c] - px[i], pz[c] - pz[i]), lc = std::hypot(acx, acz);
                const float curv = la * lb * lc > 1e-3f ? 2.0f * area2 / (la * lb * lc) : 0.0f;
                g_curv[i] = curv;
                const float grip = g_grip * g_gripScale[i];
                limit[i] = curv > 1e-4f ? std::min(90.0f, std::sqrt(grip / curv)) : 90.0f;
            }
            // Braking envelope: two backward passes around the lap.
            std::vector<float> v = limit;
            for (int pass = 0; pass < 2; ++pass)
                for (size_t step = 0; step < n; ++step)
                {
                    const size_t i = n - 1 - step, next = (i + 1) % n;
                    v[i] = std::min(v[i], std::sqrt(v[next] * v[next] + 2.0f * g_brake * g_brakeScale[i] * kBin));
                }
            g_plan = v;
            g_planDirty = false;
        }

        // The planned speed at `progress` (braking already included), or -1.
        float planSpeed(float progress)
        {
            if (g_planDirty || g_plan.size() != g_bins.size())
                buildPlan();
            const size_t i = binOf(progress);
            return i < g_plan.size() ? g_plan[i] : -1.0f;
        }

        // Grip factors over the `metres` before `progress` times `factor`.
        void adjustGrip(float progress, float metres, float factor)
        {
            if (g_gripScale.size() != g_bins.size() || g_bins.empty())
                return;
            const int steps = static_cast<int>(metres / kBin) + 1;
            for (int s = 0; s < steps; ++s)
            {
                float &f = g_gripScale[binOf(progress - metres + static_cast<float>(s) * kBin)];
                f = std::clamp(f * factor, 0.5f, gripMax());
            }
            g_planDirty = true;
        }

        // The learned plan: <dir>/plan-<lap metres>.txt (grip, braking and the
        // grip factor of every step).
        std::filesystem::path planFile()
        {
            return lineDir() / ("plan-" + lineStem() + ".txt");
        }
        void savePlan()
        {
            if (g_lapLength < 100.0f || g_gripScale.empty())
                return;
            std::error_code ec;
            std::filesystem::create_directories(lineDir(), ec);
            std::ofstream out(planFile(), std::ios::trunc);
            if (!out)
                return;
            out << "gt3-autopilot-plan 2 " << g_grip << " " << g_brake << " " << g_gripScale.size() << "\n";
            for (size_t i = 0; i < g_gripScale.size(); ++i)
                out << g_gripScale[i] << (i % 16 == 15 ? "\n" : " ");
            out << "\n";
            // Version 2: the braking factors after the grip ones.
            out << "brakes " << g_brakeScale.size() << "\n";
            for (size_t i = 0; i < g_brakeScale.size(); ++i)
                out << g_brakeScale[i] << (i % 16 == 15 ? "\n" : " ");
            out << "\n";
        }
        void loadPlan()
        {
            if (g_lineStem.empty())
                return; // no saved line for this course: no plan of its own either
            std::ifstream in(planFile());
            std::string magic;
            int version = 0;
            float grip = 0.0f, brake = 0.0f;
            size_t n = 0;
            if (!(in >> magic >> version >> grip >> brake >> n) || magic != "gt3-autopilot-plan" || n > 20000u)
                return;
            g_grip = std::clamp(grip, 4.0f, 20.0f);
            g_brake = std::clamp(brakeStart() > 0.0f ? brakeStart() : brake, 5.0f, brakeMax());
            g_gripScale.assign(std::max(n, g_bins.size()), 1.0f);
            for (size_t i = 0; i < n && in >> g_gripScale[i]; ++i)
                g_gripScale[i] = std::clamp(g_gripScale[i], 0.5f, gripMax());
            g_brakeScale.assign(g_gripScale.size(), 1.0f);
            std::string word;
            size_t nb = 0;
            if (version >= 2 && in >> word >> nb && word == "brakes" && nb <= 20000u)
                for (size_t i = 0; i < nb && i < g_brakeScale.size() && in >> g_brakeScale[i]; ++i)
                    g_brakeScale[i] = std::clamp(g_brakeScale[i], 0.75f, 1.0f);
            g_planDirty = true;
            std::fprintf(stderr, "[autopilot] plan: grip %.2f, brake %.2f, %zu steps\n", g_grip, g_brake, n);
        }

        // GT3's Key Configuration for player 1 (u32s at 0x9894a0: steering,
        // accel, brakes, ...; codes 7 / 9 the right stick up / down, 0xb..0x12
        // Circle, Cross, Triangle, Square, R1, R2, L1, L2). The pedals go
        // where GT3 reads them: GT3_PEDALS=triggers puts them on the right
        // stick (analog), the default on Cross / Square.
        struct Pedals
        {
            uint32_t accel = 0xcu, brakes = 0xeu;
        };
        Pedals g_pedals;
        uint16_t buttonOf(uint32_t code, uint16_t fallback)
        {
            switch (code)
            {
            case 0xbu: return 0x2000u;
            case 0xcu: return kCross;
            case 0xdu: return 0x1000u;
            case 0xeu: return kSquare;
            case 0xfu: return 0x0800u;
            case 0x10u: return 0x0200u;
            case 0x11u: return 0x0400u;
            case 0x12u: return 0x0100u;
            default: return fallback;
            }
        }
        bool analogPedals()
        {
            return g_pedals.accel == 7u && g_pedals.brakes == 9u;
        }

        // Changing gear (a MANUAL car; GT3_AUTOPILOT_SHIFT=1): up at the rev
        // limit, down to the lowest gear that stays under it (most power at
        // the wheels; the automatic box held 3rd at the limiter for ~300 m
        // before the finish, `ap-F` trace). The limit: GT3_AUTOPILOT_SHIFT_RPM,
        // else 100 under GT3's redline for the car (u16 car + 0x236: 8000 for
        // the Altezza, where its automatic box shifts), else 7800. Ratios
        // (rpm per m/s) are learned per gear. GT3 takes ~0.3 s to change
        // gear and shows the new gear before the revs drop (`H-manual`: a
        // second press at the old revs was ignored), so nothing is pressed
        // while a change is due, and up only 0.4 s after one.
        bool shiftEnabled()
        {
            static const bool on = [] {
                const char *v = std::getenv("GT3_AUTOPILOT_SHIFT");
                return v && *v && *v != '0';
            }();
            return on;
        }
        float shiftRpm()
        {
            static const float value = [] {
                const char *v = std::getenv("GT3_AUTOPILOT_SHIFT_RPM");
                return v && *v ? std::clamp(static_cast<float>(std::atof(v)), 3000.0f, 15000.0f) : -1.0f;
            }();
            return value;
        }
        struct Gearbox
        {
            float ratio[8] = {};    // rpm per m/s, per gear (0 = reverse)
            float limit = 7800.0f;  // shift up here
            int topGear = 7;        // two shifts up that did nothing: the top
            int upFailures = 0;
            int gear = -1;
            uint32_t gearSince = 0; // race clock the gear was engaged
            uint32_t lastPress = 0; // race clock of the last press
            int pressFrames = 0;    // frames left holding a shift button
            uint16_t pressButton = 0;
            int pressedFrom = -1;   // a change is due from this gear (-1: none)
            bool pressedUp = false;
        };
        Gearbox g_box;

        // The shift button (up / down) as GT3's Key Configuration has it
        // (u32 0x9894b4 shift up, 0x9894b8 shift down; default R2 / L2).
        uint16_t shiftButton(const uint8_t *rdram, bool up)
        {
            uint32_t code = 0;
            std::memcpy(&code, rdram + (up ? 0x9894b4u : 0x9894b8u), 4);
            return buttonOf(code, up ? 0x0200u : 0x0100u);
        }

        // The buttons to press this frame for the gear (0 = none).
        uint16_t shiftGears(const uint8_t *rdram, const gt3race::Car &me, uint32_t clockMs, float v, float throttle, float brake, bool reversing)
        {
            Gearbox &b = g_box;
            b.limit = shiftRpm() > 0.0f ? shiftRpm() : (me.redline >= 4000 && me.redline <= 15000 ? static_cast<float>(me.redline) - 100.0f : 7800.0f);
            const int gear = me.gear;
            if (gear != b.gear)
            {
                b.gear = gear;
                b.gearSince = clockMs;
                b.pressedFrom = -1;
                b.upFailures = 0;
            }
            // Ratios: rpm / speed in a settled gear under power.
            if (gear >= 1 && gear <= 7 && v > 6.0f && me.rpm > 2000 && throttle > 0.5f && brake <= 0.0f && clockMs - b.gearSince > 400u)
            {
                const float r = static_cast<float>(me.rpm) / v;
                b.ratio[gear] = b.ratio[gear] <= 0.0f ? r : 0.9f * b.ratio[gear] + 0.1f * r;
            }
            // A press in progress.
            if (b.pressFrames > 0)
            {
                --b.pressFrames;
                return b.pressButton;
            }
            // A change due: wait for it (0.8 s at most).
            if (b.pressedFrom >= 0)
            {
                if (clockMs - b.lastPress < 800u)
                    return 0;
                if (b.pressedUp && ++b.upFailures >= 2)
                {
                    b.topGear = gear;
                    if (logging())
                        std::fprintf(stderr, "[autopilot] gearbox: top gear %d\n", gear);
                }
                b.pressedFrom = -1;
            }
            const auto press = [&](bool up) {
                b.pressButton = shiftButton(rdram, up);
                b.pressFrames = 2; // held 3 frames in all
                b.lastPress = clockMs;
                b.pressedFrom = gear;
                b.pressedUp = up;
                return b.pressButton;
            };
            if (clockMs - b.gearSince < 100u)
                return 0;
            if (reversing)
                return gear > 0 ? press(false) : static_cast<uint16_t>(0u); // down to reverse
            if (gear <= 0)
                return press(true); // reverse -> first
            // Up at the limit (the revs settled in this gear).
            if (gear < b.topGear && clockMs - b.gearSince >= 400u && static_cast<float>(me.rpm) >= b.limit && brake <= 0.0f)
                return press(true);
            // Down while the lower gear stays well under the limit (its
            // ratio learned, else ~1.3 x this one's).
            if (gear > 1)
            {
                const float here = b.ratio[gear] > 0.0f ? b.ratio[gear] : (me.rpm > 0 && v > 3.0f ? static_cast<float>(me.rpm) / v : 0.0f);
                const float lower = b.ratio[gear - 1] > 0.0f ? b.ratio[gear - 1] : here * 1.3f;
                if (here > 0.0f && v * lower < b.limit - 700.0f)
                    return press(false);
            }
            return 0;
        }

        // GT3_AUTOPILOT_PRESSURE=0: pedals on buttons pulsed (a share of frames
        // pressed, as before 2026-10-01); default: one steady analog pressure.
        bool pressurePedals()
        {
            static const bool on = [] {
                const char *v = std::getenv("GT3_AUTOPILOT_PRESSURE");
                return !(v && *v == '0');
            }();
            return on;
        }
        // The pressure giving a pedal amount 0..1, from GT3's measured
        // response (2026-09-30, RAM car + 0x13ac / + 0x13b0 against held
        // pressures): throttle 8 -> 0.26, 16 -> 0.42, 20 -> 0.50, 24 -> 0.58,
        // 32 -> 0.74, 40 -> 0.90, 45 -> 1.0; brake 20 -> 0.31, 40 -> 0.53,
        // 60 -> 0.75, 83 -> 1.0 (between the points: straight lines).
        uint8_t pedalPressure(float amount, bool brakePedal)
        {
            static const float throttleCurve[][2] = {{0.10f, 1.0f}, {0.26f, 8.0f}, {0.42f, 16.0f}, {0.50f, 20.0f}, {0.58f, 24.0f},
                                                     {0.74f, 32.0f}, {0.90f, 40.0f}, {1.00f, 45.0f}};
            static const float brakeCurve[][2] = {{0.09f, 1.0f}, {0.31f, 20.0f}, {0.53f, 40.0f}, {0.75f, 60.0f}, {1.00f, 83.0f}};
            if (amount <= 0.0f)
                return 0u;
            const float(*curve)[2] = brakePedal ? brakeCurve : throttleCurve;
            const int n = brakePedal ? 5 : 8;
            if (amount <= curve[0][0])
                return 1u;
            for (int i = 1; i < n; ++i)
                if (amount <= curve[i][0])
                {
                    const float f = (amount - curve[i - 1][0]) / (curve[i][0] - curve[i - 1][0]);
                    return static_cast<uint8_t>(std::lround(curve[i - 1][1] + f * (curve[i][1] - curve[i - 1][1])));
                }
            return static_cast<uint8_t>(curve[n - 1][1]);
        }

        // Steering -1..1; the pedals 0..1 (on buttons: pressed when over 0).
        void setPacket(float steer, float throttle, float brake, uint16_t extra = 0)
        {
            g_packet.fill(0);
            g_packet[0] = 1;
            g_packet[1] = 0x79; // with pressure bytes (set below)
            uint16_t pressed = extra;
            g_packet[4] = g_packet[5] = g_packet[7] = 0x80; // right stick, left stick y
            if (analogPedals())
            {
                // GT3's analog pedals: nothing below ~28 / 128 of travel, full
                // at ~113 (as the triggers are mapped, ps2_input_profile).
                const auto travel = [](float f) { return f <= 0.0f ? 0.0f : 28.0f + std::min(1.0f, f) * 85.0f; };
                if (throttle > 0.0f)
                    g_packet[5] = static_cast<uint8_t>(std::clamp(128.0f - travel(throttle), 0.0f, 255.0f));
                else if (brake > 0.0f)
                    g_packet[5] = static_cast<uint8_t>(std::clamp(128.0f + travel(brake), 0.0f, 255.0f));
            }
            uint16_t throttleButton = 0, brakeButton = 0;
            if (!analogPedals())
            {
                if (throttle > 0.0f)
                    pressed |= (throttleButton = buttonOf(g_pedals.accel, kCross));
                if (brake > 0.0f)
                    pressed |= (brakeButton = buttonOf(g_pedals.brakes, kSquare));
            }
            const uint16_t released = static_cast<uint16_t>(~pressed);
            g_packet[2] = static_cast<uint8_t>(released);
            g_packet[3] = static_cast<uint8_t>(released >> 8);
            // The pressure bytes (GT3 reads them once the pad is in pressure
            // mode; DualShock 2 order: right, left, up, down, triangle, circle,
            // cross, square, L1, R1, L2, R2): the pedal buttons at the pressure
            // for their amount (pulsed pedals: full), other pressed ones full.
            constexpr uint16_t pressureMasks[12] = {0x20, 0x80, 0x10, 0x40, 0x1000, 0x2000, 0x4000, 0x8000, 0x400, 0x800, 0x100, 0x200};
            for (int i = 0; i < 12; ++i)
            {
                uint8_t value = (pressed & pressureMasks[i]) ? 255u : 0u;
                if (pressurePedals() && (throttleButton & pressureMasks[i]))
                    value = pedalPressure(throttle, false);
                else if (pressurePedals() && (brakeButton & pressureMasks[i]))
                    value = pedalPressure(brake, true);
                g_packet[8 + i] = value;
            }
            // GT3 ignores the first 37 / 127 of analog stick travel and then
            // steers about 1.27 x (travel - 37) (its steering byte, car +
            // 0x14a7, against the stick in RAM dumps, `tmc` 2026-09-30): the
            // command starts past that dead zone, so small corrections steer.
            // GT3_AUTOPILOT_DEADZONE=0: the stick as commanded.
            static const float dead = [] {
                const char *v = std::getenv("GT3_AUTOPILOT_DEADZONE");
                return std::clamp(v && *v ? static_cast<float>(std::atof(v)) : 37.0f, 0.0f, 100.0f);
            }();
            const float a = std::min(1.0f, std::fabs(steer));
            const float travel = a < 0.005f ? 0.0f : dead + a * (127.0f - dead);
            g_packet[6] = static_cast<uint8_t>(std::clamp(128.0f + std::copysign(travel, steer), 0.0f, 255.0f));
        }

        // While GT3 drives the player's car (the Free Run menu's preview laps,
        // AUTO DRIVE) on a course with no saved line: its positions and
        // speeds are learned as the line (as the AI cars' in a race), and the
        // line is saved once the car has gone round (a whole lap: its lap
        // metres wrap). A saved line is left as it is.
        void learnWhileGt3Drives(const gt3race::Snapshot &s)
        {
            static uint32_t lastClock = 0;
            static float lastProgress = -1.0f;
            static bool saved = false;
            const gt3race::Car &me = s.cars[0];
            if (s.clockMs == 0u || s.clockMs == lastClock || !std::isfinite(me.progress))
                return;
            if (s.clockMs + 1000u < lastClock) // a new preview / race
                lastProgress = -1.0f, g_cars[0].valid = false;
            lastClock = s.clockMs;
            if (!g_loaded)
            {
                g_loaded = true;
                loadLine(-1.0f, me.x, me.z, me.progress); // a saved line for this course, by position
                if (planEnabled())
                    loadPlan();
                saved = false;
            }
            if (g_lapLength >= 100.0f && !g_bins.empty() && !g_learningFromGt3)
                return; // a saved line: nothing to learn here
            if (!g_learningFromGt3)
            {
                g_learningFromGt3 = true;
                std::fprintf(stderr, "[autopilot] no line for this course: learning it from GT3's own driving\n");
            }
            learnFromAi(s, true);
            if (!saved && lastProgress > 200.0f && me.progress + 200.0f < lastProgress && g_lapLength >= 100.0f)
            {
                // Saved once the line covers the lap: Time Trial's demo starts
                // behind the line, so its first wrap comes after the run-up.
                size_t covered = 0;
                for (const Bin &b : g_bins)
                    covered += b.n > 0 ? 1u : 0u;
                if (static_cast<float>(covered) * kBin >= g_lapLength * 0.95f)
                {
                    saved = true;
                    saveLine();
                    std::fprintf(stderr, "[autopilot] learned a %.0f m line from GT3's own lap (saved)\n", g_lapLength);
                }
                else
                    std::fprintf(stderr, "[autopilot] GT3's lap: %.0f of %.0f m of line so far, learning on\n", static_cast<float>(covered) * kBin,
                                 g_lapLength);
            }
            lastProgress = me.progress;
        }

        bool step(const uint8_t *rdram)
        {
            gt3race::Snapshot s;
            if (!gt3race::read(rdram, s, false) || s.cars.empty() || !s.cars[0].player)
            {
                g_driving = false;
                return false;
            }
            // A replay: neither driven nor learned from (its laps and "hits"
            // are the race's again).
            static bool wasReplay = false;
            if (s.replay != wasReplay)
            {
                wasReplay = s.replay;
                std::fprintf(stderr, "[autopilot] %s\n", s.replay ? "replay: not driving" : "racing");
            }
            if (s.replay)
            {
                g_driving = false;
                return false;
            }
            // GT3 itself drives the player's car (AUTO DRIVE, a menu's preview).
            {
                const bool gt3Drives = aiDrives(rdram, s.race + 0xc8u);
                static bool wasGt3 = false;
                if (gt3Drives != wasGt3)
                {
                    wasGt3 = gt3Drives;
                    std::fprintf(stderr, "[autopilot] %s\n", gt3Drives ? "GT3 drives the car (auto drive / preview): hands off" : "the car is the player's");
                }
                if (gt3Drives)
                {
                    g_driving = false;
                    learnWhileGt3Drives(s);
                    return false;
                }
            }
            // One step per game frame (the race clock moves); the countdown
            // (clock 0) is stepped on every call.
            if (s.clockMs != 0u && s.clockMs == g_lastClock && g_driving && s.race == g_race)
                return true;
            const float dt = s.clockMs > g_lastClock ? (s.clockMs - g_lastClock) / 1000.0f : 0.0f;
            // GT3 reuses its race object (a restart, the race after a replay):
            // the clock going back over a second is a new race too.
            const bool clockReset = g_lastClock > s.clockMs + 1000u;
            g_lastClock = s.clockMs;
            ++g_frames;
            if (s.race != g_race || clockReset)
            {
                // A new race: keep the line if the lap is the same length
                // (the same course), else start over.
                g_race = s.race;
                for (Tracked &t : g_cars)
                    t.valid = false;
                g_lastLap = -1;
                g_lastLapClock = 0;
                g_stuckSince = g_reverseUntil = 0;
                g_speed = g_prevSpeed = 0.0f;
                g_lastClock = 0u;
                g_historyHead = 0u;
                g_box = Gearbox{};
                std::fprintf(stderr, "[autopilot] race at 0x%x, %zu cars, line %zu steps; the car at (%.1f, %.1f, %.1f) yaw %.2f\n", s.race, s.cars.size(),
                             g_bins.size(), s.cars[0].x, s.cars[0].y, s.cars[0].z, s.cars[0].yaw);
            }
            if (!g_loaded)
            {
                // The first race frame: a saved line for this course.
                g_loaded = true;
                float longest = 0.0f;
                for (const gt3race::Car &c : s.cars)
                    if (std::isfinite(c.progress) && c.progress < 1e5f)
                        longest = std::max(longest, c.progress + 1.0f);
                loadLine(s.cars.size() > 1 ? longest : -1.0f, s.cars[0].x, s.cars[0].z, s.cars[0].progress);
                if (planEnabled())
                    loadPlan();
            }
            // The AI's line from races with other cars (Free Run's one other
            // car may be a ghost of the player's own laps).
            if (s.clockMs != 0u && s.cars.size() >= 3u)
                learnFromAi(s);
            if (g_frames - g_lastSave > 3600u)
            {
                g_lastSave = g_frames;
                saveLine();
            }
            // The racing line, once the AI's line covers the lap (a scan of
            // EE RAM for the edges and a relaxation: ~0.1 s, once).
            if (routeEnabled() && g_routeX.empty() && g_routeTries < 3 && s.clockMs != 0u && g_frames - g_routeCheck > 300u)
            {
                g_routeCheck = g_frames;
                size_t covered = 0;
                for (const Bin &b : g_bins)
                    covered += b.n > 0 ? 1u : 0u;
                if (g_lapLength > 100.0f && static_cast<float>(covered) * kBin >= g_lapLength * 0.98f)
                {
                    ++g_routeTries;
                    std::vector<std::vector<float>> edges;
                    if (gt3race::findCourseEdges(rdram, s, edges))
                        buildRoute(edges);
                    else
                        std::fprintf(stderr, "[autopilot] route: the course's edges were not found\n");
                }
            }
            // Limits learned from a hit: the route again (at most once a second).
            if (g_routeDirty && g_frames - g_routeRelaxed > 60u)
            {
                g_routeRelaxed = g_frames;
                relaxRoute(2000);
                saveWalls();
                writeRoute();
            }

            const gt3race::Car &me = s.cars[0];
            // GT3_AUTOPILOT_LOG_CARS=1: every car's place twice a second (the AI's
            // driving vs a course's lines, e.g. converted GT4 courses).
            {
                static const bool logCars = [] {
                    const char *v = std::getenv("GT3_AUTOPILOT_LOG_CARS");
                    return v && *v == '1';
                }();
                if (logCars && g_frames % 30u == 0u)
                    for (const gt3race::Car &c : s.cars)
                        std::fprintf(stderr, "[autopilot-car] t=%u car %d lap %d at %.1f (%.2f, %.2f, %.2f) v %.1f\n", s.clockMs, c.index, c.lap, c.progress, c.x,
                                     c.y, c.z, c.speed);
            }
            // GT3_SHOT_POINTS ("x z dx dz" lines: a point on the course and the
            // driving direction): a picture as the car crosses the line across
            // the road there (within 15 m of the point), once each.
            {
                struct ShotPoint
                {
                    float x, z, dx, dz, side = 0.0f;
                    bool taken = false;
                };
                static std::vector<ShotPoint> points = [] {
                    std::vector<ShotPoint> p;
                    if (const char *v = std::getenv("GT3_SHOT_POINTS"); v && *v)
                    {
                        std::ifstream in(v);
                        ShotPoint q;
                        while (in >> q.x >> q.z >> q.dx >> q.dz)
                            p.push_back(q);
                    }
                    return p;
                }();
                for (size_t k = 0; k < points.size(); ++k)
                {
                    ShotPoint &q = points[k];
                    const float along = (me.x - q.x) * q.dx + (me.z - q.z) * q.dz;
                    const float across = std::fabs((me.x - q.x) * q.dz - (me.z - q.z) * q.dx);
                    const bool crossed = q.side < 0.0f && along >= 0.0f && across < 15.0f;
                    q.side = along;
                    if (crossed && !q.taken && gt3shot::request.load() < 0)
                    {
                        q.taken = true;
                        gt3shot::request.store(static_cast<int>(k));
                        std::fprintf(stderr, "[autopilot] picture %zu at (%.0f, %.0f), lap %d\n", k, me.x, me.z, me.lap);
                    }
                }
            }
            // GT3_AUTOPILOT_LOG: each change of the wheels' surfaces (mapping
            // GT3's surface codes: 0 tarmac, 4 Laguna Seca's gravel).
            if (logging())
            {
                static uint32_t lastSurface = ~0u;
                const uint32_t surface = static_cast<uint32_t>(me.surface[0]) | static_cast<uint32_t>(me.surface[1]) << 8 |
                                         static_cast<uint32_t>(me.surface[2]) << 16 | static_cast<uint32_t>(me.surface[3]) << 24;
                if (surface != lastSurface)
                {
                    lastSurface = surface;
                    float lx = 0.0f, lz = 0.0f;
                    const float offNow = linePoint(me.progress, lx, lz) ? std::hypot(me.x - lx, me.z - lz) : -1.0f;
                    std::fprintf(stderr, "[autopilot] surface %u %u %u %u at %.0f m (lap %d, off the line %.1f m)\n", me.surface[0], me.surface[1],
                                 me.surface[2], me.surface[3], me.progress, me.lap, offNow);
                }
            }
            // Speed over at least 100 ms of race time (a pad read can catch
            // the clock of a frame before its positions), deceleration over
            // the speed about 200 ms before.
            g_history[g_historyHead % kHistory] = {me.x, me.z, s.clockMs, true};
            ++g_historyHead;
            for (uint32_t back = 1; back < kHistory && back < g_historyHead; ++back)
            {
                const Tracked &old = g_history[(g_historyHead - 1u - back) % kHistory];
                if (!old.valid || old.clock > s.clockMs)
                    break;
                if (s.clockMs - old.clock >= 100u)
                {
                    const float measured = std::hypot(me.x - old.x, me.z - old.z) / ((s.clockMs - old.clock) / 1000.0f);
                    // Over 120 m/s the car was moved (the finish, a replay), not driven.
                    if (std::isfinite(measured) && measured < 120.0f)
                        g_speed = measured;
                    break;
                }
            }
            g_speeds[g_historyHead % kHistory] = {g_speed, s.clockMs};
            g_prevSpeed = g_speed;
            for (uint32_t back = 1; back < kHistory && back < g_historyHead; ++back)
            {
                const SpeedSample &old = g_speeds[(g_historyHead - back) % kHistory];
                if (old.clock <= s.clockMs && s.clockMs - old.clock >= 200u)
                {
                    g_prevSpeed = old.speed;
                    g_prevSpeedClock = old.clock;
                    break;
                }
            }
            // GT3's own speed (car + 0x140c) when it is sane, else the measured one.
            const float v = std::isfinite(me.speed) && me.speed >= 0.0f && me.speed < 120.0f && planEnabled() ? me.speed : g_speed;

            // Laps and the per-lap log.
            if (g_lastLap >= 0 && me.lap > g_lastLap && me.lastLapMs < 359999999u)
            {
                uint32_t aiBest = 359999999u;
                for (const gt3race::Car &c : s.cars)
                    if (!c.player && c.lastLapMs > 10000u)
                        aiBest = std::min(aiBest, c.lastLapMs);
                // The lap by the race clock too (Free Run's +0x16c4 stays at
                // the best lap).
                const uint32_t byClock = g_lastLapClock != 0u && s.clockMs > g_lastLapClock ? s.clockMs - g_lastLapClock : 0u;
                g_lastLapClock = s.clockMs;
                char ai[64] = "";
                if (aiBest < 359999999u)
                    std::snprintf(ai, sizeof(ai), "; the AI's best last lap %u'%02u.%03u", aiBest / 60000u, (aiBest / 1000u) % 60u, aiBest % 1000u);
                std::fprintf(stderr, "[autopilot] lap %d: %u'%02u.%03u, by the clock %u'%02u.%03u (incidents %u, grip %.2f, brake %.2f%s)\n",
                             me.lap - 1, me.lastLapMs / 60000u, (me.lastLapMs / 1000u) % 60u, me.lastLapMs % 1000u, byClock / 60000u,
                             (byClock / 1000u) % 60u, byClock % 1000u, g_incidents, g_grip, g_brake, ai);
                // A lap: every section speeds up a little (the penalised
                // ones were cut by more than this on the way).
                for (Bin &b : g_bins)
                    b.scale = std::min(kMaxScale, b.scale * 1.04f);
                if (planEnabled())
                {
                    for (float &f : g_gripScale)
                        f = std::min(gripMax(), f * 1.03f);
                    // A clean lap: brake a little later too (the measured rate
                    // below only pulls it back when full braking falls behind
                    // the plan; the plan's own braking hardly ever did, so the
                    // rate stayed at its first value, 7.5 m/s^2, `ap-exp` A-D).
                    if (g_incidents == 0u)
                    {
                        g_brake = std::min(brakeMax(), g_brake * 1.02f);
                        for (float &f : g_brakeScale)
                            f = std::min(1.0f, f * 1.01f);
                    }
                    g_planDirty = true;
                    savePlan();
                }
                g_incidents = 0;
                saveLine();
            }
            g_lastLap = me.lap;

            const float fx = -std::sin(me.yaw), fz = -std::cos(me.yaw);
            // GT3_AUTOPILOT_LOOK: look-ahead distance x this (default 1.3; with
            // the damping below Free Run Trial Mountain went from 2'02-2'06
            // with hits to 1'59.6 .. 1'57.3, all clean, `frcomb`).
            static const float lookScale = [] {
                const char *v = std::getenv("GT3_AUTOPILOT_LOOK");
                return std::clamp(v && *v ? static_cast<float>(std::atof(v)) : 1.3f, 0.4f, 3.0f);
            }();
            const float look = std::clamp((8.0f + 0.55f * v) * lookScale, 10.0f, 60.0f);
            float tx, tz;
            float steer = 0.0f;
            bool haveLine = linePoint(me.progress + look, tx, tz);
            if (haveLine)
            {
                const float dx = tx - me.x, dz = tz - me.z;
                const float cross = fx * dz - fz * dx, dot = fx * dx + fz * dz;
                const float alpha = std::atan2(cross, dot);
                // Gain 1.5 with the dead zone passed (setPacket): the old 2.2
                // through the dead zone steered the same at a 14 degree error
                // and not at all below ~7.
                static const float gain = [] {
                    const char *v = std::getenv("GT3_AUTOPILOT_STEER_GAIN");
                    return std::clamp(v && *v ? static_cast<float>(std::atof(v)) : 1.5f, 0.3f, 5.0f);
                }();
                // Damping (GT3_AUTOPILOT_STEER_D, seconds): less steering while
                // the heading error is already closing (GT3's steering lags,
                // and the car weaved 3-4 m about the line at gain 1.5).
                static const float damp = [] {
                    const char *v = std::getenv("GT3_AUTOPILOT_STEER_D");
                    return std::clamp(v && *v ? static_cast<float>(std::atof(v)) : 0.3f, 0.0f, 2.0f);
                }();
                static float prevAlpha = 0.0f, alphaRate = 0.0f;
                if (dt > 0.0f && dt < 0.1f)
                    alphaRate = 0.6f * alphaRate + 0.4f * (alpha - prevAlpha) / dt;
                prevAlpha = alpha;
                steer = std::clamp(gain * (alpha + damp * alphaRate), -1.0f, 1.0f);
            }
            // Distance from the line here.
            float lx, lz;
            const float off = linePoint(me.progress, lx, lz) ? std::hypot(me.x - lx, me.z - lz) : 0.0f;

            // Target speed: the slowest point within braking distance.
            float target = 1e9f;
            const float reach = std::min(v * v / (2.0f * 7.0f) + 12.0f, 300.0f);
            const int steps = static_cast<int>(reach / kBin) + 1;
            for (int k = 0; k < steps; ++k)
            {
                const float ahead = static_cast<float>(k) * kBin;
                const float t = binSpeed(binOf(me.progress + ahead));
                if (t >= 0.0f)
                {
                    // Room to slow down from here to there at ~7 m/s^2.
                    const float room = std::sqrt(t * t + 2.0f * 7.0f * std::max(0.0f, ahead - 4.0f));
                    target = std::min(target, room);
                }
            }
            // The target for the brake pedal: the plan further ahead
            // (GT3_AUTOPILOT_BRAKE_LOOK, seconds; default 0.25 since 2026-10-01:
            // with pressure pedals, laps 1'49.80 / 1'49.82 against 1'50.25-1'50.35
            // at 0.12, `ap-R1b-blook25` vs `ap-S0-pressure`):
            // GT3 takes ~0.15-0.2 s to lift the throttle and apply the brake
            // (car + 0x13ac 1.00 -> 0.83 -> 0.33 -> 0, + 0x13b0 0.33 -> 1.00 in
            // the trace), by which time the plan has dropped ~2 m/s at 12 m/s^2.
            float targetBrake = 1e9f;
            if (planEnabled() && haveLine)
            {
                // The planned speed a moment ahead (the pedals act with a lag).
                const float planned = planSpeed(me.progress + std::max(2.0f, v * 0.12f));
                if (planned > 0.0f)
                    target = planned;
                static const float brakeLook = [] {
                    const char *e = std::getenv("GT3_AUTOPILOT_BRAKE_LOOK");
                    return std::clamp(e && *e ? static_cast<float>(std::atof(e)) : 0.25f, 0.0f, 1.0f);
                }();
                const float plannedBrake = planSpeed(me.progress + std::max(2.0f, v * brakeLook));
                if (plannedBrake > 0.0f)
                    targetBrake = std::min(target, plannedBrake);
            }
            if (!haveLine || target > 1e8f)
                target = 30.0f; // unknown road ahead: moderate
            if (targetBrake > 1e8f)
                targetBrake = target;

            // Incidents (for learning): a sudden stop or far off the line.
            const uint32_t span = s.clockMs - g_prevSpeedClock;
            const float decel = span >= 200u && span < 1000u ? (g_prevSpeed - v) / (span / 1000.0f) : 0.0f;
            (void)dt;
            // (Over 400 m/s^2 is a jump of the car, e.g. at the finish, not a crash.)
            // Running into a gravel trap (3+ wheels on GT3's surface 4; tarmac 0,
            // kerbs 1, grass 2, sand 3, the rally course's dirt 5 / 8: GT3's own
            // runways, GT4-WORK NOTES) counts once as it starts: no hit, but Time
            // Trial fails the run there ("FAIL").
            static bool wasInGravel = false;
            int gravelWheels = 0;
            for (const uint8_t w : me.surface)
                gravelWheels += w == 4u ? 1 : 0;
            const bool gravelNow = gravelWheels >= 3 && !wasInGravel;
            wasInGravel = gravelWheels >= 3;
            if (s.clockMs > 3000u && ((decel > 40.0f && decel < 400.0f) || off > 9.0f || gravelNow) && g_frames > g_reverseUntil)
            {
                static uint64_t lastPenalty = 0;
                if (g_frames - lastPenalty > 60u)
                {
                    lastPenalty = g_frames;
                    ++g_incidents;
                    penalise(me.progress, 70.0f, 0.9f);
                    if (planEnabled())
                        adjustGrip(me.progress, 90.0f, 0.9f);
                    // A hit or the gravel: the route keeps further in from where the
                    // car was (Laguna Seca: the car tracked ~2.5 m outside the
                    // line into the gravel at 489 m even 3 m/s slower).
                    if ((decel > 40.0f || gravelNow) && !g_routeX.empty())
                        learnWall(me.progress, me.x, me.z);
                    if (logging())
                        std::fprintf(stderr, "[autopilot] incident at %.0f m (decel %.0f, off %.1f m%s): slower there\n", me.progress, decel, off,
                                     gravelNow ? ", into the gravel" : "");
                    // Kept at once: Time Trial ends the run at a hit ("FAIL"),
                    // before the lap's end would save them.
                    saveLine();
                    if (planEnabled())
                    {
                        g_planDirty = true;
                        savePlan();
                    }
                }
            }

            // Stuck: reverse a little with the wheel turned the other way, longer
            // each time it does not get free (the automatic gearbox needs a
            // moment at a standstill before it reverses: 1.5 s barely moved the
            // car off a hairpin wall on GT4's Infineon).
            static int reverseTries = 0;
            if (s.clockMs > 4000u && v < 1.0f)
            {
                if (g_stuckSince == 0)
                    g_stuckSince = g_frames;
                else if (g_frames - g_stuckSince > 150u && g_reverseUntil < g_frames)
                {
                    g_reverseUntil = g_frames + 90u + 45u * static_cast<uint64_t>(std::min(reverseTries, 3));
                    ++reverseTries;
                    g_stuckSince = 0;
                }
            }
            else
            {
                g_stuckSince = 0;
                if (v > 8.0f)
                    reverseTries = 0;
            }

            if (planEnabled() && s.clockMs > 3000u && g_frames > g_reverseUntil)
            {
                // Running wide in a corner (over 3.5 m off the line where it
                // bends): a little less grip there.
                static uint64_t lastWide = 0;
                const size_t here = binOf(me.progress);
                if (off > 3.5f && here < g_curv.size() && g_curv[here] > 1.0f / 150.0f && g_frames - lastWide > 30u)
                {
                    lastWide = g_frames;
                    adjustGrip(me.progress, 50.0f, 0.97f);
                }
                // Braking per section: full braking and still over 2 m/s above
                // the plan -> the zone from 10 m back to 60 m ahead brakes at
                // 95% of its rate (once per zone and lap: a lower plan leaves
                // the car further "over" it at once).
                static float lastLocalAt = -1000.0f;
                static int lastLocalLap = -1;
                if (brakeLocal() && target < v - 2.0f && !(me.lap == lastLocalLap && std::fabs(me.progress - lastLocalAt) < 80.0f) &&
                    g_brakeScale.size() == g_bins.size() && !g_bins.empty())
                {
                    lastLocalAt = me.progress;
                    lastLocalLap = me.lap;
                    if (logging())
                        std::fprintf(stderr, "[autopilot] braking at %.0f m: %.1f m/s over the plan at full brake, that zone's rate x0.95\n", me.progress, v - target);
                    for (int k = -5; k <= 30; ++k)
                    {
                        float &f = g_brakeScale[binOf(me.progress + static_cast<float>(k) * kBin)];
                        // (At least 75% of the rate: into the 2,814 m hairpin the car
                        // stayed ~2 m/s over after every cut, a lag rather than
                        // the rate, `ap-P1`, so the cuts must not run away.)
                        f = std::max(0.75f, f * 0.95f);
                    }
                    g_planDirty = true;
                }
                // Braking rate: how fast the car actually slows under full brake.
                static float brakeFrom = -1.0f;
                static uint32_t brakeClock = 0;
                if (target < v - 4.0f)
                {
                    if (brakeFrom < 0.0f)
                    {
                        brakeFrom = v;
                        brakeClock = s.clockMs;
                    }
                    else if (s.clockMs - brakeClock >= 400u)
                    {
                        const float measured = (brakeFrom - v) / ((s.clockMs - brakeClock) / 1000.0f);
                        if (measured > 3.0f && measured < 15.0f)
                        {
                            // The margin kept on the measured rate (GT3_AUTOPILOT_BRAKE_MARGIN,
                            // default 0.92).
                            static const float margin = [] {
                                const char *e = std::getenv("GT3_AUTOPILOT_BRAKE_MARGIN");
                                return std::clamp(e && *e ? static_cast<float>(std::atof(e)) : 0.92f, 0.6f, 1.1f);
                            }();
                            g_brake = std::clamp(0.9f * g_brake + 0.1f * measured * margin, 5.0f, brakeMax());
                            g_planDirty = true;
                        }
                        brakeFrom = v;
                        brakeClock = s.clockMs;
                    }
                }
                else
                    brakeFrom = -1.0f;
            }

            bool throttle, brake;
            float throttleAmount = 0.0f, brakeAmount = 0.0f; // analog pedals
            {
                uint32_t accel = 0, brakes = 0;
                std::memcpy(&accel, rdram + 0x9894a4u, 4);
                std::memcpy(&brakes, rdram + 0x9894a8u, 4);
                g_pedals.accel = accel;
                g_pedals.brakes = brakes;
            }
            if (g_frames < g_reverseUntil)
            {
                steer = -steer;
                if (shiftEnabled())
                {
                    // Manual: in reverse (gear 0) with the throttle.
                    throttle = me.gear == 0;
                    brake = false;
                    throttleAmount = throttle ? 1.0f : 0.0f;
                }
                else
                {
                    throttle = false;
                    brake = true; // automatic gearbox: brake at a standstill reverses
                    brakeAmount = 1.0f;
                }
            }
            else
            {
                const float err = target - v;
                float throttleDuty = err > 1.5f ? 1.0f : (err > -0.5f ? 0.55f : 0.0f);
                float brakeDuty = err < -2.0f ? std::clamp(-err / 6.0f, 0.25f, 1.0f) : 0.0f;
                if (planEnabled())
                {
                    // GT3_AUTOPILOT_BRAKE_GAIN: m/s over the target for full
                    // braking (default 1.0; at the old 4, 2 m/s over braked
                    // half the frames and the car ran ~2 m/s over the plan
                    // through whole braking zones, `ap-F` lap 8 at 2,300 m;
                    // 1.5 was 0.4-0.9 s a lap faster, `ap-exp` M1 vs M0, and
                    // 1.0 ~0.2 s more on average, N2 vs N0).
                    static const float brakeGain = [] {
                        const char *e = std::getenv("GT3_AUTOPILOT_BRAKE_GAIN");
                        return std::clamp(e && *e ? static_cast<float>(std::atof(e)) : 1.0f, 0.5f, 10.0f);
                    }();
                    throttleDuty = err > 1.0f ? 1.0f : (err > -0.3f ? 0.6f : 0.0f);
                    const float errBrake = targetBrake - v; // the brake looks further ahead
                    brakeDuty = errBrake < -std::min(1.0f, brakeGain * 0.25f) ? std::clamp(-errBrake / brakeGain, 0.3f, 1.0f) : 0.0f;
                    if (brakeDuty > 0.0f)
                        throttleDuty = 0.0f;
                    // GT3_AUTOPILOT_EXIT_LIFT=<m> (default off): half throttle
                    // while over that far off the line in a bend and still
                    // drifting away (`ap-M1` hit the outside wall at 1,058 m
                    // after running 3-3.7 m wide out of a corner at full throttle).
                    static const float exitLift = [] {
                        const char *e = std::getenv("GT3_AUTOPILOT_EXIT_LIFT");
                        return e && *e ? std::clamp(static_cast<float>(std::atof(e)), 0.0f, 20.0f) : 0.0f;
                    }();
                    static float prevOff = 0.0f;
                    const size_t here = binOf(me.progress);
                    if (exitLift > 0.0f && off > exitLift && off > prevOff && here < g_curv.size() && g_curv[here] > 1.0f / 200.0f)
                        throttleDuty = std::min(throttleDuty, 0.5f);
                    prevOff = off;
                }
                if (s.clockMs == 0u)
                    throttleDuty = 0.4f, brakeDuty = 0.0f; // countdown
                if (analogPedals() || pressurePedals())
                {
                    // Analog pedals (the right stick, or button pressures): the
                    // duty as the pedal itself.
                    throttleAmount = throttleDuty;
                    brakeAmount = throttleDuty > 0.0f ? 0.0f : brakeDuty;
                    throttle = throttleAmount > 0.0f;
                    brake = brakeAmount > 0.0f;
                }
                else
                {
                    // Buttons: the duty as a share of frames pressed.
                    g_throttleAcc += throttleDuty;
                    g_brakeAcc += brakeDuty;
                    throttle = g_throttleAcc >= 1.0f;
                    brake = g_brakeAcc >= 1.0f;
                    if (throttle)
                        g_throttleAcc -= 1.0f;
                    if (brake)
                        g_brakeAcc -= 1.0f;
                    throttleAmount = throttle ? 1.0f : 0.0f;
                    brakeAmount = brake ? 1.0f : 0.0f;
                }
            }
            const uint16_t shift = shiftEnabled() && s.clockMs != 0u
                                       ? shiftGears(rdram, me, s.clockMs, v, throttleAmount, brakeAmount, g_frames < g_reverseUntil)
                                       : static_cast<uint16_t>(0u);
            setPacket(steer, throttleAmount, brakeAmount, shift);
            // Before the start (race clock 0: the countdown, or Time Trial's
            // menu over the live track, where the stick moved the menu to
            // DEMO) the pad stays the player's.
            g_driving = s.clockMs != 0u;

            // GT3_AUTOPILOT_TRACE=N: a line every N frames with the car's gear,
            // revs and GT3's ASM / TCS lamps (where the lap's time goes).
            static const uint64_t traceEvery = [] {
                const char *e = std::getenv("GT3_AUTOPILOT_TRACE");
                return e && *e ? static_cast<uint64_t>(std::clamp(std::atoi(e), 0, 600)) : 0u;
            }();
            if (traceEvery != 0u && s.clockMs != 0u && g_frames % traceEvery == 0u)
            {
                // GT3's pedals as the car uses them (floats car + 0x13ac /
                // + 0x13b0, what its HUD's pedal bars show: after its aids?).
                float carThrottle = 0.0f, carBrake = 0.0f;
                const uint32_t car = s.race + 0xc8u;
                if (car + 0x16dcu < 0x2000000u)
                {
                    std::memcpy(&carThrottle, rdram + car + 0x13acu, 4);
                    std::memcpy(&carBrake, rdram + car + 0x13b0u, 4);
                }
                std::fprintf(stderr, "[autopilot-trace] %u lap %d at %.1f v %.2f target %.2f gear %d rpm %d %s%s steer %+.2f off %.1f asm %d tcs %d thr %.2f brk %.2f shift %04x limit %.0f redline %d\n",
                             s.clockMs, me.lap, me.progress, v, target, me.gear, me.rpm, throttle ? "T" : "-", brake ? "B" : "-", steer, off,
                             s.lamps[0] ? 1 : 0, s.lamps[1] ? 1 : 0, carThrottle, carBrake, shift, g_box.limit, me.redline);
            }
            if (logging() && g_frames % 60u == 0u)
                std::fprintf(stderr, "[autopilot] t=%u.%us lap %d at %.0f/%.0f m v %.1f (measured %.1f) target %.1f steer %+.2f %s%s off %.1f m line %s (%.0f, %.1f, %.0f)\n",
                             s.clockMs / 1000u, (s.clockMs / 100u) % 10u, me.lap, me.progress, g_lapLength, v, g_speed, target, steer,
                             throttle ? "T" : "-", brake ? "B" : "-", off, haveLine ? "yes" : "no", me.x, me.y, me.z);
            return true;
        }
    }

    bool enabled()
    {
        static const bool on = [] {
            const char *v = std::getenv("GT3_AUTOPILOT");
            return v && *v && *v != '0';
        }();
        return on;
    }

    void install(PS2Runtime &runtime, const uint8_t *rdram)
    {
        if (!enabled())
            return;
        gt3race::install(runtime, rdram);
        // GT3's AI driver: addiu sp, sp, -0x110 at 0x1f7ae0 (GT3 USA CORE).
        uint32_t first;
        std::memcpy(&first, rdram + kAiDriver, 4);
        if (first == 0x27bdfef0u && (g_aiDriver = runtime.lookupFunction(kAiDriver)) != nullptr)
            runtime.registerFunction(kAiDriver, &aiDriverHook);
        else
            std::fprintf(stderr, "[autopilot] GT3's AI driver not found: auto drive not told apart\n");
        std::fprintf(stderr, "[autopilot] on: races are driven along the AI's line (start at %.2f of its speed)\n", startScale());
    }

    bool drive(const uint8_t *rdram, uint64_t tick, std::array<uint8_t, 32> &packet)
    {
        if (!enabled() || !rdram)
            return false;
        std::lock_guard<std::mutex> lock(g_mutex);
        if (tick != g_lastTick)
        {
            g_lastTick = tick;
            step(rdram);
            // GT3_AUTOPILOT_LOG=2: the pad packet of every VSync (to match RAM
            // dumps against: which car fields follow the controls).
            static const bool everyFrame = [] {
                const char *v = std::getenv("GT3_AUTOPILOT_LOG");
                return v && *v == '2';
            }();
            if (everyFrame && g_driving)
                std::fprintf(stderr, "[autopilot-pad] tick %llu stick %u buttons %02x%02x\n", static_cast<unsigned long long>(tick), g_packet[6],
                             g_packet[3], g_packet[2]);
        }
        if (!g_driving)
            return false;
        packet = g_packet;
        return true;
    }
}
