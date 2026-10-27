#include "gt3_hud.h"

#include "gt3_camera.h"
#include "gt3_race_info.h"
#include "gt3_race_report.h"

#include "raylib.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

const char *const gt3hud::kKeys[gt3hud::WidgetCount] = {"speed", "rpm", "gear", "shift", "position", "lap", "times", "map", "tower", "splits", "zoommap", "lights", "odo"};

float gt3hud::aspect(int widget, int style, int columns)
{
    switch (widget)
    {
    case Speed: return 1.9f;
    case Rpm:
    {
        static const float a[5] = {5.0f, 1.2f, 1.45f, 6.0f, 2.3f}; // bar, dial, arc, LED strip, digital
        return a[std::clamp(style, 0, 4)];
    }
    case Gear: return 0.9f;
    case Shift: return 6.0f;
    case Position: return 1.8f;
    case Lap: return 2.2f;
    case Times:
    {
        static const float a[3] = {2.4f, 2.4f, 4.2f};
        return a[std::clamp(style, 0, 2)];
    }
    case Map: return 1.0f;
    case Tower: return style == kTowerCustom ? towerCustomAspect(columns) : (style >= 3 ? 2.3f : 1.3f); // the tables are wide
    case Splits: return 2.8f;
    case ZoomMap: return 1.0f;
    case Lights: return 3.8f;
    case Odo: return 3.0f;
    default: return 1.0f;
    }
}

namespace
{
    using namespace gt3hud;

    Box g_boxes[WidgetCount] = {
        {false, 88, 84, 11, 60},         // speed
        {false, 50, 93, 6, 60},          // rpm
        {false, 77, 84, 11, 60},         // gear
        {false, 50, 7, 4, 50},           // shift lights
        {false, 9, 9, 9, 60},            // position
        {false, 9, 19, 6, 60},           // lap
        {false, 86, 12, 14, 60},         // lap times
        {false, 12, 46, 26, 45},         // map
        {false, 10, 55, 30, 55},         // timing tower
        {false, 50, 17, 7, 60},          // splits
        {false, 88, 50, 26, 45, 0, 300}, // zoomed map
        {false, 50, 86, 5, 60},          // lamps
        {false, 50, 97, 4, 60},          // odometer
    };
    int g_speedUnits = 1; // 0 km/h, 1 mph
    // The timing tower's names: 0 the cars', 1 the drivers' (gt3report::driverName), 2 switching
    // between them every kTowerSwitchSeconds (GT3_HUD_TOWER_NAMES=cars|drivers|switch; Ctrl+F5);
    // its time column switching between best and last lap (GT3_HUD_TOWER_TIME_SWITCH=1; Shift+F5).
    int g_towerNames = [] {
        const char *v = std::getenv("GT3_HUD_TOWER_NAMES");
        return !v ? 0 : (std::strcmp(v, "drivers") == 0 ? 1 : (std::strcmp(v, "switch") == 0 ? 2 : 0));
    }();
    bool g_towerTimeSwitch = [] {
        const char *v = std::getenv("GT3_HUD_TOWER_TIME_SWITCH");
        return v && *v == '1';
    }();
    constexpr double kTowerSwitchSeconds = 5.0;

    // GT3_HUD_<NAME>=on,x,y,size,opacity[,style[,range[,columns[,time]]]]
    const bool g_envRead = [] {
        for (int w = 0; w < WidgetCount; ++w)
        {
            std::string name = std::string("GT3_HUD_") + kKeys[w];
            for (char &c : name)
                c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
            const char *v = std::getenv(name.c_str());
            if (!v || !*v)
                continue;
            int on = 0, x = 0, y = 0, size = 0, opacity = 0, style = 0, range = 0, columns = 0, time = 0;
            const int n = std::sscanf(v, "%d,%d,%d,%d,%d,%d,%d,%d,%d", &on, &x, &y, &size, &opacity, &style, &range, &columns, &time);
            Box &b = g_boxes[w];
            if (n >= 1)
                b.on = on != 0;
            if (n >= 3)
            {
                b.x = std::clamp(x, 0, 100);
                b.y = std::clamp(y, 0, 100);
            }
            if (n >= 4)
                b.size = std::clamp(size, 2, 60);
            if (n >= 5)
                b.opacity = std::clamp(opacity, 0, 100);
            if (n >= 6)
                b.style = std::clamp(style, 0, kStyleCount[w] - 1);
            if (n >= 7)
                b.range = std::clamp(range, 100, 1500);
            if (n >= 8)
                b.columns = std::clamp(columns, 0, 255);
            if (n >= 9)
                b.timeMode = std::clamp(time, 0, kTowerTimeCount - 1);
        }
        return true;
    }();

    // The HUD's area across (as the radar's, gt3_radar.cpp hudArea).
    void hudArea(float x, float w, float h, float &left, float &width)
    {
        static const bool anchored = [] {
            const char *v = std::getenv("GT3_HUD_ANCHOR");
            return !(v && *v == '0');
        }();
        static const float bezel = [] {
            const char *v = std::getenv("GT3_TRIPLE_BEZEL");
            return v && *v ? std::clamp(static_cast<float>(std::atof(v)), 0.0f, 0.5f) : 0.0f;
        }();
        const float aspect = w / std::max(1.0f, h);
        width = std::min(w, h * 4.0f / 3.0f);
        if (anchored && aspect > 4.0f / 3.0f + 0.01f)
            width = aspect <= 2.5f ? w : w * (bezel > 0.0f ? 1.0f / (3.0f + 2.0f * bezel) : (16.0f / 9.0f) / aspect);
        left = x + (w - width) * 0.5f;
    }

    Font &font()
    {
        static Font f = [] {
            const char *path = "C:\\Windows\\Fonts\\segoeuib.ttf";
            if (FileExists(path))
            {
                Font loaded = LoadFontEx(path, 96, nullptr, 0);
                GenTextureMipmaps(&loaded.texture);
                SetTextureFilter(loaded.texture, TEXTURE_FILTER_TRILINEAR);
                return loaded;
            }
            return GetFontDefault();
        }();
        return f;
    }
    void textAt(const char *s, float cx, float cy, float size, Color c, float align = 0.5f)
    {
        const Vector2 m = MeasureTextEx(font(), s, size, 1.0f);
        DrawTextEx(font(), s, Vector2{std::round(cx - m.x * align), std::round(cy - m.y * 0.5f)}, size, 1.0f, c);
    }
    // The text size (at most `size`) at which s fits `width` pixels.
    float fitSize(const char *s, float size, float width)
    {
        const float w = MeasureTextEx(font(), s, size, 1.0f).x;
        return w > width && w > 0.0f ? size * width / w : size;
    }
    // s cut to fit `width` pixels at `size`.
    std::string fitted(std::string s, float size, float width)
    {
        while (s.size() > 1u && MeasureTextEx(font(), s.c_str(), size, 1.0f).x > width)
            s.pop_back();
        while (!s.empty() && s.back() == ' ')
            s.pop_back();
        return s;
    }
    std::string lapTime(long long ms)
    {
        if (ms < 0)
            return "-'--.---";
        char b[32];
        std::snprintf(b, sizeof(b), "%lld'%02lld.%03lld", ms / 60000, (ms / 1000) % 60, ms % 1000);
        return b;
    }
    // "23.456", or "1'02.345" from a minute.
    std::string seconds(long long ms)
    {
        if (ms < 0)
            return "--.---";
        if (ms >= 60000)
            return lapTime(ms);
        char b[32];
        std::snprintf(b, sizeof(b), "%lld.%03lld", ms / 1000, ms % 1000);
        return b;
    }
    std::string signedSeconds(long long ms) { return (ms < 0 ? "-" : "+") + seconds(ms < 0 ? -ms : ms); }
    std::string ordinal(int n)
    {
        const char *suffix = (n % 100 >= 11 && n % 100 <= 13) ? "th" : (n % 10 == 1 ? "st" : (n % 10 == 2 ? "nd" : (n % 10 == 3 ? "rd" : "th")));
        return std::to_string(n) + suffix;
    }
    bool validLap(uint32_t ms) { return ms > 0u && ms < 359999999u; }

    const Color kText{235, 238, 244, 255};
    const Color kMuted{150, 158, 172, 255};
    const Color kAccent{255, 184, 48, 255};
    const Color kGreen{70, 220, 110, 255};
    const Color kYellow{255, 210, 60, 255};
    const Color kRed{255, 70, 60, 255};
    const Color kPurple{190, 110, 255, 255};
    const Color kPlayer{120, 220, 255, 255};

    // Race distance of a car: laps done times the lap plus the progress into
    // this one (on the grid, lap 0, just below zero).
    struct TracePoint
    {
        int lap;
        float progress;
        uint32_t clock;
    };

    // The road between the course's two edges as triangles (world x, z).
    struct Tri
    {
        float x[3], z[3];
        float cx, cz, r; // bounding circle
    };

    // Per race.
    struct Race
    {
        uint32_t race = 0, lastClock = 0;
        std::map<int, uint32_t> best; // best lap by car
        std::vector<std::vector<float>> edges;
        std::vector<Tri> road;
        float edgeX0 = 0, edgeX1 = 0, edgeZ0 = 0, edgeZ1 = 0;
        double edgesNext = 0.0;
        int edgesTries = 0;
        float lapLength = 1.0f; // the largest progress seen (on the grid already just short of a lap)
        std::vector<std::vector<TracePoint>> traces; // per car, for gaps at equal distance
        // The player's lap: sectors (thirds of the lap), the trace of this
        // lap (progress, lap time) for the difference to the best lap.
        int lap = -1;
        float prevProgress = -1.0f;
        long long sector[2] = {-1, -1};
        std::vector<std::pair<float, long long>> trace, bestTrace;
        long long bestTraceMs = -1;
        long long lastSectors[3] = {-1, -1, -1}, bestSectors[3] = {-1, -1, -1};
        bool lastWasBest[3] = {};
        // Every car's sectors, timed the same way as the player's (thirds of the lap from the
        // car's lap start; S3 from GT3's lap time), for the fastest sector of the race so far
        // (purple in the Splits widget; the user: "does it give you purple sectors if you went fastest").
        struct CarSectors
        {
            int lap = -1;
            float prev = -1.0f;
            long long s[2] = {-1, -1};
            bool pending = false;
            uint32_t lastBefore = 0, prevLast = 0;
            double at = 0.0;
            long long ps[2] = {-1, -1};
        };
        std::vector<CarSectors> carSectors;
        long long fieldBest[3] = {-1, -1, -1};
        bool lastWasFastest[3] = {};
        uint32_t prevLastLap = 0;
        // A lap just done, its time not in yet (GT3 may set it a frame later).
        bool pending = false;
        uint32_t pendingLastBefore = 0;
        double pendingAt = 0.0;
        long long pendingSectors[2] = {-1, -1};
        std::vector<std::pair<float, long long>> pendingTrace;
    };
    Race g;

    double raceDistance(int lap, float progress) { return (lap - 1) * static_cast<double>(g.lapLength) + progress; }
    // Clock time when car k first reached race distance d (-1: not yet).
    double timeAt(int k, double d)
    {
        if (k < 0 || static_cast<size_t>(k) >= g.traces.size())
            return -1.0;
        const auto &tr = g.traces[static_cast<size_t>(k)];
        if (tr.empty() || raceDistance(tr.back().lap, tr.back().progress) < d)
            return -1.0;
        size_t lo = 0, hi = tr.size() - 1;
        while (lo < hi)
        {
            const size_t mid = (lo + hi) / 2;
            if (raceDistance(tr[mid].lap, tr[mid].progress) < d)
                lo = mid + 1;
            else
                hi = mid;
        }
        if (lo == 0)
            return tr[0].clock;
        const double d0 = raceDistance(tr[lo - 1].lap, tr[lo - 1].progress), d1 = raceDistance(tr[lo].lap, tr[lo].progress);
        return d1 > d0 ? tr[lo - 1].clock + (tr[lo].clock - static_cast<double>(tr[lo - 1].clock)) * (d - d0) / (d1 - d0) : tr[lo].clock;
    }
    // The best lap's time at progress p (-1: none).
    long long bestTimeAt(float p)
    {
        const auto &tr = g.bestTrace;
        if (tr.size() < 2u || p < tr.front().first || p > tr.back().first)
            return -1;
        const auto it = std::lower_bound(tr.begin(), tr.end(), p, [](const std::pair<float, long long> &a, float v) { return a.first < v; });
        if (it == tr.begin())
            return it->second;
        const auto prev = it - 1;
        const float span = it->first - prev->first;
        return span > 0.0f ? prev->second + static_cast<long long>((it->second - prev->second) * (p - prev->first) / span) : it->second;
    }

    // The road: the two edges walked together (whichever step makes the
    // shorter new side), both from their closest points and the same way.
    void buildRoad()
    {
        g.road.clear();
        if (g.edges.size() < 2u || g.edges[0].size() < 6u || g.edges[1].size() < 6u)
            return;
        struct P
        {
            float x, z;
        };
        std::vector<P> a, b;
        for (size_t i = 0; i + 1 < g.edges[0].size(); i += 2)
            a.push_back({g.edges[0][i], g.edges[0][i + 1]});
        for (size_t i = 0; i + 1 < g.edges[1].size(); i += 2)
            b.push_back({g.edges[1][i], g.edges[1][i + 1]});
        auto d2 = [](P p, P q) { return (p.x - q.x) * (p.x - q.x) + (p.z - q.z) * (p.z - q.z); };
        size_t j0 = 0;
        for (size_t j = 1; j < b.size(); ++j)
            if (d2(a[0], b[j]) < d2(a[0], b[j0]))
                j0 = j;
        std::rotate(b.begin(), b.begin() + static_cast<long>(j0), b.end());
        // The same way round: a's first step against b's.
        const float ax = a[1].x - a[0].x, az = a[1].z - a[0].z;
        const float bx = b[1].x - b[0].x, bz = b[1].z - b[0].z;
        if (ax * bx + az * bz < 0.0f)
            std::reverse(b.begin() + 1, b.end());
        const size_t n = a.size(), m = b.size();
        size_t i = 0, j = 0;
        auto add = [&](P p, P q, P r) {
            Tri t;
            t.x[0] = p.x, t.z[0] = p.z, t.x[1] = q.x, t.z[1] = q.z, t.x[2] = r.x, t.z[2] = r.z;
            t.cx = (p.x + q.x + r.x) / 3.0f;
            t.cz = (p.z + q.z + r.z) / 3.0f;
            t.r = std::sqrt(std::max({d2(P{t.cx, t.cz}, p), d2(P{t.cx, t.cz}, q), d2(P{t.cx, t.cz}, r)}));
            g.road.push_back(t);
        };
        while (i < n || j < m)
        {
            const P ai = a[i % n], bj = b[j % m];
            const bool stepA = j >= m || (i < n && d2(a[(i + 1) % n], bj) <= d2(ai, b[(j + 1) % m]));
            if (stepA)
            {
                add(ai, a[(i + 1) % n], bj);
                ++i;
            }
            else
            {
                add(ai, b[(j + 1) % m], bj);
                ++j;
            }
        }
    }

    void updateEdges(const uint8_t *rdram, const gt3race::Snapshot &s)
    {
        if (!g.edges.empty() || g.edgesTries >= 20 || GetTime() < g.edgesNext)
            return;
        ++g.edgesTries;
        g.edgesNext = GetTime() + 2.0;
        if (!gt3race::findCourseEdges(rdram, s, g.edges))
            return;
        g.edgeX0 = g.edgeZ0 = 1e9f;
        g.edgeX1 = g.edgeZ1 = -1e9f;
        for (const auto &e : g.edges)
            for (size_t i = 0; i + 1 < e.size(); i += 2)
            {
                g.edgeX0 = std::min(g.edgeX0, e[i]);
                g.edgeX1 = std::max(g.edgeX1, e[i]);
                g.edgeZ0 = std::min(g.edgeZ0, e[i + 1]);
                g.edgeZ1 = std::max(g.edgeZ1, e[i + 1]);
            }
        buildRoad();
    }

    // Everything kept per race, from this frame's state.
    void track(const gt3race::Snapshot &s)
    {
        const gt3race::Car &me = s.cars[0];
        if (s.race != g.race || s.clockMs + 1000u < g.lastClock)
        {
            // A new race (or a restart): all afresh.
            g = Race{};
            g.race = s.race;
            g.prevLastLap = me.lastLapMs;
        }
        g.lastClock = s.clockMs;
        for (const gt3race::Car &c : s.cars)
        {
            if (validLap(c.lastLapMs) && (g.best[c.index] == 0u || c.lastLapMs < g.best[c.index]))
                g.best[c.index] = c.lastLapMs;
            if (std::isfinite(c.progress) && c.progress < 1e5f)
                g.lapLength = std::max(g.lapLength, c.progress + 1.0f);
        }
        g.traces.resize(std::max(g.traces.size(), s.cars.size()));
        for (const gt3race::Car &c : s.cars)
        {
            if (!std::isfinite(c.progress) || c.progress > 1e5f)
                continue;
            auto &tr = g.traces[static_cast<size_t>(c.index)];
            if (tr.empty() || raceDistance(c.lap, c.progress) > raceDistance(tr.back().lap, tr.back().progress) + 1.0)
                tr.push_back(TracePoint{c.lap, c.progress, s.clockMs});
        }

        // Every car's sectors (the field's best for purple), as the player's below.
        g.carSectors.resize(std::max(g.carSectors.size(), s.cars.size()));
        for (const gt3race::Car &c : s.cars)
        {
            Race::CarSectors &cs = g.carSectors[static_cast<size_t>(c.index)];
            if (c.lap != cs.lap)
            {
                if (cs.lap >= 1 && c.lap == cs.lap + 1)
                {
                    cs.pending = true;
                    cs.lastBefore = cs.prevLast;
                    cs.at = GetTime();
                    cs.ps[0] = cs.s[0];
                    cs.ps[1] = cs.s[1];
                }
                cs.lap = c.lap;
                cs.s[0] = cs.s[1] = -1;
                cs.prev = -1.0f;
            }
            const auto better = [](long long t, long long best) { return t > 0 && (best < 0 || t < best); };
            if (cs.pending && ((validLap(c.lastLapMs) && c.lastLapMs != cs.lastBefore) || GetTime() - cs.at > 2.0))
            {
                cs.pending = false;
                if (validLap(c.lastLapMs) && cs.ps[0] >= 0 && cs.ps[1] >= 0)
                {
                    const long long s3 = static_cast<long long>(c.lastLapMs) - cs.ps[0] - cs.ps[1];
                    if (better(s3, g.fieldBest[2]))
                        g.fieldBest[2] = s3;
                }
            }
            cs.prevLast = c.lastLapMs;
            if (c.lap >= 1 && s.clockMs >= c.lapStartMs && std::isfinite(c.progress))
            {
                const long long t = static_cast<long long>(s.clockMs - c.lapStartMs);
                const float p = c.progress, L = g.lapLength;
                auto crossed = [&](float line) { return cs.prev >= 0.0f && cs.prev < line && p >= line && p - cs.prev < 60.0f; };
                if (cs.s[0] < 0 && crossed(L / 3.0f))
                {
                    cs.s[0] = t;
                    if (better(t, g.fieldBest[0]))
                        g.fieldBest[0] = t;
                }
                else if (cs.s[0] >= 0 && cs.s[1] < 0 && crossed(L * 2.0f / 3.0f))
                {
                    cs.s[1] = t - cs.s[0];
                    if (better(cs.s[1], g.fieldBest[1]))
                        g.fieldBest[1] = cs.s[1];
                }
                cs.prev = p;
            }
        }

        // The player's lap.
        if (me.lap != g.lap)
        {
            if (g.lap >= 1 && me.lap == g.lap + 1)
            {
                g.pending = true;
                g.pendingLastBefore = g.prevLastLap;
                g.pendingAt = GetTime();
                g.pendingSectors[0] = g.sector[0];
                g.pendingSectors[1] = g.sector[1];
                g.pendingTrace = std::move(g.trace);
            }
            g.lap = me.lap;
            g.sector[0] = g.sector[1] = -1;
            g.trace.clear();
            g.prevProgress = -1.0f;
        }
        if (g.pending && ((validLap(me.lastLapMs) && me.lastLapMs != g.pendingLastBefore) || GetTime() - g.pendingAt > 2.0))
        {
            g.pending = false;
            if (validLap(me.lastLapMs))
            {
                const long long lapMs = me.lastLapMs;
                const long long s3 = g.pendingSectors[0] >= 0 && g.pendingSectors[1] >= 0 ? lapMs - g.pendingSectors[0] - g.pendingSectors[1] : -1;
                const long long done[3] = {g.pendingSectors[0], g.pendingSectors[1], s3};
                for (int k = 0; k < 3; ++k)
                {
                    g.lastSectors[k] = done[k];
                    g.lastWasFastest[k] = done[k] > 0 && g.fieldBest[k] >= 0 && done[k] <= g.fieldBest[k];
                    g.lastWasBest[k] = done[k] > 0 && (g.bestSectors[k] < 0 || done[k] <= g.bestSectors[k]);
                    if (g.lastWasBest[k])
                        g.bestSectors[k] = done[k];
                }
                if (g.pendingTrace.size() > 10u && (g.bestTraceMs < 0 || lapMs < g.bestTraceMs))
                {
                    g.bestTrace = std::move(g.pendingTrace);
                    g.bestTraceMs = lapMs;
                }
            }
            g.pendingTrace.clear();
        }
        g.prevLastLap = me.lastLapMs;
        if (me.lap >= 1 && s.clockMs >= me.lapStartMs && std::isfinite(me.progress))
        {
            const long long t = static_cast<long long>(s.clockMs - me.lapStartMs);
            const float p = me.progress, L = g.lapLength;
            if (g.trace.empty() ? p < L * 0.25f : p > g.trace.back().first + 1.0f)
                g.trace.emplace_back(p, t);
            // A sector line crossed: from just before it to past it.
            auto crossed = [&](float line) { return g.prevProgress >= 0.0f && g.prevProgress < line && p >= line && p - g.prevProgress < 60.0f; };
            if (g.sector[0] < 0 && crossed(L / 3.0f))
                g.sector[0] = t;
            else if (g.sector[0] >= 0 && g.sector[1] < 0 && crossed(L * 2.0f / 3.0f))
                g.sector[1] = t - g.sector[0];
            g.prevProgress = p;
        }
    }

    // The model names, by car index: looked up on a thread of their own (a
    // scan of EE RAM the first time), for the timing tower.
    std::mutex g_nameMutex;
    std::vector<std::string> g_names;
    std::thread g_nameThread;
    std::atomic<bool> g_nameRun{false};
    std::atomic<const uint8_t *> g_nameRdram{nullptr};
    std::atomic<int64_t> g_towerSeenMs{0};
    int64_t nowMs()
    {
        return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
    }
    void nameLoop()
    {
        while (g_nameRun.load())
        {
            const uint8_t *rdram = g_nameRdram.load();
            if (rdram && nowMs() - g_towerSeenMs.load() < 1000)
            {
                gt3race::Snapshot s;
                if (gt3race::read(rdram, s, true))
                {
                    std::lock_guard<std::mutex> lock(g_nameMutex);
                    g_names.assign(s.cars.size(), std::string());
                    for (const gt3race::Car &c : s.cars)
                        if (static_cast<size_t>(c.index) < g_names.size())
                            g_names[static_cast<size_t>(c.index)] = c.model.empty() ? c.name : c.model;
                }
            }
            for (int i = 0; i < 10 && g_nameRun.load(); ++i)
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
    }
    std::string nameOf(int index)
    {
        std::lock_guard<std::mutex> lock(g_nameMutex);
        return static_cast<size_t>(index) < g_names.size() ? g_names[static_cast<size_t>(index)] : std::string();
    }

    // Each car's colour, as the phone's (gt3_phone_view.cpp: by car slot, the
    // player white), on the maps and in the timing tower's tables.
    Color carColour(const gt3race::Car &c)
    {
        static const Color colours[8] = {{255, 210, 74, 255},  {74, 210, 255, 255},  {255, 106, 106, 255}, {125, 255, 122, 255},
                                         {209, 140, 255, 255}, {255, 162, 74, 255},  {255, 122, 217, 255}, {170, 170, 170, 255}};
        return c.index == 0 ? WHITE : colours[c.index % 8];
    }

    // World (x, z) to the box: north up (x right, z down) or turned so the
    // player's heading points up; about (ox, oz) at the box point (sx, sy).
    struct MapView
    {
        float ox = 0, oz = 0, sx = 0, sy = 0, k = 1;
        bool turned = false;
        float fx = 0, fz = -1, rx = 1, rz = 0; // forward and right (world)
        Vector2 at(float x, float z) const
        {
            const float dx = x - ox, dz = z - oz;
            if (!turned)
                return Vector2{sx + dx * k, sy + dz * k};
            return Vector2{sx + (dx * rx + dz * rz) * k, sy - (dx * fx + dz * fz) * k};
        }
    };
    void drawTri(Vector2 a, Vector2 b, Vector2 c, Color col)
    {
        // raylib: counter-clockwise as seen (y down: a negative cross product).
        if ((b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x) > 0.0f)
            std::swap(b, c);
        DrawTriangle(a, b, c, col);
    }
    // The course: road, edges and cars; `cull` (world radius about the view's
    // middle, 0 all) skips what is far off the box.
    void drawCourse(const gt3race::Snapshot &s, const MapView &v, float bh, float cull, float cullX, float cullZ, bool arrow)
    {
        auto near = [&](float x, float z, float r) { return cull <= 0.0f || (x - cullX) * (x - cullX) + (z - cullZ) * (z - cullZ) < (cull + r) * (cull + r); };
        for (const Tri &t : g.road)
            if (near(t.cx, t.cz, t.r))
                drawTri(v.at(t.x[0], t.z[0]), v.at(t.x[1], t.z[1]), v.at(t.x[2], t.z[2]), Fade(WHITE, 0.22f));
        const float line = std::max(1.0f, bh * 0.012f);
        for (const auto &e : g.edges)
            for (size_t i = 0; i + 1 < e.size(); i += 2)
            {
                const size_t j = (i + 2) % e.size();
                if (near(e[i], e[i + 1], 20.0f))
                    DrawLineEx(v.at(e[i], e[i + 1]), v.at(e[j], e[j + 1]), line, Fade(WHITE, 0.6f));
            }
        for (auto it = s.cars.rbegin(); it != s.cars.rend(); ++it)
        {
            const bool player = it->index == 0;
            const Vector2 p = v.at(it->x, it->z);
            if (player && arrow)
            {
                // The player: an arrow along the heading.
                const float fx = -std::sin(it->yaw), fz = -std::cos(it->yaw);
                const Vector2 tip = v.at(it->x + fx, it->z + fz);
                float dx = tip.x - p.x, dy = tip.y - p.y;
                const float len = std::sqrt(dx * dx + dy * dy);
                if (len > 1e-6f)
                    dx /= len, dy /= len;
                const float a = std::max(4.0f, bh * 0.07f);
                const Vector2 f{p.x + dx * a, p.y + dy * a};
                const Vector2 l{p.x - dx * a * 0.6f - dy * a * 0.62f, p.y - dy * a * 0.6f + dx * a * 0.62f};
                const Vector2 r{p.x - dx * a * 0.6f + dy * a * 0.62f, p.y - dy * a * 0.6f - dx * a * 0.62f};
                drawTri(f, l, r, kPlayer);
            }
            else
            {
                // A dot in the car's colour (the phone's and the timing
                // tower's) with its place in it.
                const float rad = std::max(4.0f, bh * 0.05f);
                DrawCircleV(p, rad, carColour(*it));
                DrawCircleLinesV(p, rad, Fade(BLACK, 0.55f));
                if (rad >= 5.0f)
                    textAt(std::to_string(it->position).c_str(), p.x, p.y, rad * 1.4f, Color{20, 22, 26, 255});
            }
        }
    }

    // Rev counter styles.
    void drawRpm(const gt3race::Car &me, const Rectangle &r, int style)
    {
        const float bw = r.width, bh = r.height, cx = r.x + bw * 0.5f, cy = r.y + bh * 0.5f;
        const int red = me.redline > 2000 ? me.redline : 8000;
        const float top = std::ceil(red * 1.15f / 1000.0f) * 1000.0f;
        const float frac = std::clamp(me.rpm / top, 0.0f, 1.0f);
        const bool over = me.rpm >= red;
        char rpmText[16];
        std::snprintf(rpmText, sizeof(rpmText), "%d", me.rpm);
        // Green to yellow to red toward the red line.
        auto sweep = [&](float f) {
            const float t = std::clamp((f * top) / red, 0.0f, 1.0f);
            if (t < 0.7f)
                return kGreen;
            if (t < 0.9f)
                return kYellow;
            return kRed;
        };
        switch (style)
        {
        default:
        case 0: // bar
        {
            const Rectangle bar{r.x + bw * 0.04f, r.y + bh * 0.2f, bw * 0.72f, bh * 0.6f};
            DrawRectangleRec(bar, Fade(WHITE, 0.12f));
            const float redX = bar.x + bar.width * red / top;
            DrawRectangleRec(Rectangle{redX, bar.y, bar.x + bar.width - redX, bar.height}, Fade(kRed, 0.45f));
            DrawRectangleRec(Rectangle{bar.x, bar.y, bar.width * frac, bar.height}, over ? kRed : kText);
            for (int k = 1000; k < top; k += 1000)
            {
                const float tx = bar.x + bar.width * k / top;
                DrawLineEx(Vector2{tx, bar.y + bar.height * 0.6f}, Vector2{tx, bar.y + bar.height}, 1.0f, Fade(BLACK, 0.6f));
            }
            textAt(rpmText, r.x + bw * 0.88f, cy - bh * 0.1f, bh * 0.42f, kText);
            textAt("rpm", r.x + bw * 0.88f, cy + bh * 0.28f, bh * 0.22f, kAccent);
            break;
        }
        case 1: // dial: 270 degrees from bottom left, clockwise
        {
            const float R = std::min(bw, bh) * 0.46f;
            const Vector2 c{cx, cy + bh * 0.03f};
            auto ang = [&](float f) { return 135.0f + 270.0f * f; };
            DrawRing(c, R * 0.93f, R, ang(0.0f), ang(1.0f), 48, Fade(WHITE, 0.25f));
            DrawRing(c, R * 0.8f, R, ang(red / top), ang(1.0f), 24, Fade(kRed, 0.75f));
            for (int k = 0; k <= top; k += 1000)
            {
                const float a = ang(k / top) * DEG2RAD, ca = std::cos(a), sa = std::sin(a);
                DrawLineEx(Vector2{c.x + ca * R * 0.78f, c.y + sa * R * 0.78f}, Vector2{c.x + ca * R * 0.97f, c.y + sa * R * 0.97f},
                           std::max(1.0f, bh * 0.018f), k >= red ? kRed : kText);
                if (k + 500 < top)
                {
                    const float h = (ang((k + 500) / top)) * DEG2RAD;
                    DrawLineEx(Vector2{c.x + std::cos(h) * R * 0.87f, c.y + std::sin(h) * R * 0.87f},
                               Vector2{c.x + std::cos(h) * R * 0.97f, c.y + std::sin(h) * R * 0.97f}, 1.0f, Fade(kText, 0.6f));
                }
                char n[8];
                std::snprintf(n, sizeof(n), "%d", k / 1000);
                textAt(n, c.x + ca * R * 0.62f, c.y + sa * R * 0.62f, bh * 0.1f, k >= red ? kRed : kText);
            }
            textAt("x1000", c.x, c.y - R * 0.32f, bh * 0.07f, kMuted);
            const float a = ang(frac) * DEG2RAD;
            DrawLineEx(c, Vector2{c.x + std::cos(a) * R * 0.92f, c.y + std::sin(a) * R * 0.92f}, std::max(2.0f, bh * 0.025f), Color{255, 90, 50, 255});
            DrawCircleV(c, std::max(3.0f, bh * 0.04f), Color{40, 42, 48, 255});
            DrawCircleLinesV(c, std::max(3.0f, bh * 0.04f), kMuted);
            textAt(rpmText, c.x, c.y + R * 0.52f, bh * 0.1f, over ? kRed : kText);
            break;
        }
        case 2: // arc: a thick 240-degree sweep, coloured toward the red line
        {
            const float R = std::min(bw * 0.5f, bh * 0.62f) * 0.95f;
            const Vector2 c{cx, r.y + bh * 0.58f};
            auto ang = [&](float f) { return 150.0f + 240.0f * f; };
            DrawRing(c, R * 0.74f, R, ang(0.0f), ang(1.0f), 64, Fade(WHITE, 0.12f));
            const int steps = 60;
            for (int i = 0; i < steps; ++i)
            {
                const float f0 = static_cast<float>(i) / steps, f1 = static_cast<float>(i + 1) / steps;
                if (f0 >= frac)
                    break;
                DrawRing(c, R * 0.74f, R, ang(f0), ang(std::min(f1, frac)) + 0.3f, 2, sweep(f0));
            }
            DrawRing(c, R * 1.02f, R * 1.08f, ang(red / top), ang(1.0f), 16, kRed);
            textAt(rpmText, c.x, c.y - R * 0.12f, bh * 0.26f, over ? kRed : kText);
            textAt("rpm", c.x, c.y + R * 0.2f, bh * 0.11f, kAccent);
            break;
        }
        case 3: // LED strip: lights from 55% of the red line, blue past it
        {
            const int n = 15;
            const float start = red * 0.55f;
            const int lit = me.rpm < start ? 0 : std::clamp(static_cast<int>((me.rpm - start) / (red - start) * n) + 1, 0, n);
            const bool flash = over && std::fmod(GetTime() * 8.0, 2.0) < 1.0;
            const float gap = bw * 0.012f, segW = (bw * 0.94f - gap * (n - 1)) / n;
            for (int i = 0; i < n; ++i)
            {
                const Color on = flash ? Color{80, 150, 255, 255} : (i < 6 ? kGreen : (i < 11 ? kYellow : kRed));
                const Rectangle seg{r.x + bw * 0.03f + i * (segW + gap), r.y + bh * 0.2f, segW, bh * 0.6f};
                DrawRectangleRounded(seg, 0.3f, 4, i < lit || flash ? on : Fade(on, 0.14f));
            }
            break;
        }
        case 4: // digital: the number, a thin bar under it
        {
            textAt(rpmText, r.x + bw * 0.62f, r.y + bh * 0.4f, bh * 0.56f, over ? kRed : kText, 1.0f);
            textAt("rpm", r.x + bw * 0.66f, r.y + bh * 0.48f, bh * 0.2f, kAccent, 0.0f);
            const Rectangle bar{r.x + bw * 0.06f, r.y + bh * 0.76f, bw * 0.88f, bh * 0.1f};
            DrawRectangleRec(bar, Fade(WHITE, 0.12f));
            const float redX = bar.x + bar.width * red / top;
            DrawRectangleRec(Rectangle{redX, bar.y, bar.x + bar.width - redX, bar.height}, Fade(kRed, 0.45f));
            DrawRectangleRec(Rectangle{bar.x, bar.y, bar.width * frac, bar.height}, sweep(frac));
            break;
        }
        }
    }
}

gt3hud::Box &gt3hud::box(Widget w) { return g_boxes[w]; }
void gt3hud::setSpeedUnits(int units) { g_speedUnits = units ? 1 : 0; }

std::string gt3hud::applyLive(const std::map<std::string, int> &values)
{
    bool changed = false;
    for (int w = 0; w < WidgetCount; ++w)
    {
        Box &b = g_boxes[w];
        const std::string prefix = std::string("hud_") + kKeys[w] + "_";
        auto take = [&](const char *field, int &target, int lo, int hi) {
            const auto it = values.find(prefix + field);
            if (it == values.end())
                return;
            const int v = std::clamp(it->second, lo, hi);
            changed |= v != target;
            target = v;
        };
        int on = b.on ? 1 : 0;
        take("on", on, 0, 1);
        b.on = on != 0;
        take("x", b.x, 0, 100);
        take("y", b.y, 0, 100);
        take("size", b.size, 2, 60);
        take("alpha", b.opacity, 0, 100);
        take("style", b.style, 0, kStyleCount[w] - 1);
        take("range", b.range, 100, 1500);
        take("columns", b.columns, 0, 255);
        take("time", b.timeMode, 0, kTowerTimeCount - 1);
    }
    return changed ? "HUD layout updated" : "";
}

void gt3hud::shutdown()
{
    g_nameRun.store(false);
    if (g_nameThread.joinable())
        g_nameThread.join();
}

void gt3hud::draw(const uint8_t *rdram, float px, float py, float pw, float ph)
{
    bool any = false;
    for (const Box &b : g_boxes)
        any |= b.on;
    // Only while the race camera draws the race (not on the results screen,
    // in replays or menus, where GT3 still runs its race HUD updater).
    if (!any || !rdram || !gt3camera::raceActive())
        return;
    gt3race::Snapshot s;
    if (!gt3race::read(rdram, s, false) || s.cars.empty())
        return;
    const gt3race::Car &me = s.cars[0];
    track(s);
    if (g_boxes[Map].on || g_boxes[ZoomMap].on)
        updateEdges(rdram, s);
    if (g_boxes[Tower].on)
    {
        // F5: the tower's time column: best lap, last lap, this lap. Ctrl+F5: names - cars, drivers,
        // switching between them; Shift+F5: best and last lap switching (gt3hud::towerNames / towerTimeSwitch).
        if (IsKeyPressed(KEY_F5))
        {
            if (IsKeyDown(KEY_LEFT_CONTROL) || IsKeyDown(KEY_RIGHT_CONTROL))
                g_towerNames = (g_towerNames + 1) % 3;
            else if (IsKeyDown(KEY_LEFT_SHIFT) || IsKeyDown(KEY_RIGHT_SHIFT))
                g_towerTimeSwitch = !g_towerTimeSwitch;
            else
                g_boxes[Tower].timeMode = (g_boxes[Tower].timeMode + 1) % kTowerTimeCount;
        }
        g_nameRdram.store(rdram);
        g_towerSeenMs.store(nowMs());
        if (!g_nameRun.exchange(true))
            g_nameThread = std::thread(nameLoop);
    }

    float left = px, areaW = pw;
    hudArea(px, pw, ph, left, areaW);
    for (int w = 0; w < WidgetCount; ++w)
    {
        const Box &b = g_boxes[w];
        if (!b.on)
            continue;
        const float bh = ph * b.size / 100.0f, bw = bh * aspect(w, b.style, b.columns);
        const float cx = std::clamp(left + areaW * b.x / 100.0f, left + bw * 0.5f, left + areaW - bw * 0.5f);
        const float cy = std::clamp(py + ph * b.y / 100.0f, py + bh * 0.5f, py + ph - bh * 0.5f);
        const Rectangle r{cx - bw * 0.5f, cy - bh * 0.5f, bw, bh};
        if (b.opacity > 0)
            DrawRectangleRounded(r, w == Rpm && b.style == 1 ? 0.5f : 0.18f, 6, Fade(BLACK, b.opacity / 100.0f));
        switch (w)
        {
        case Speed:
        {
            const float v = me.speed * (g_speedUnits ? 2.23694f : 3.6f);
            char t[16];
            std::snprintf(t, sizeof(t), "%d", static_cast<int>(std::max(0.0f, v)));
            textAt(t, cx, r.y + bh * 0.42f, bh * 0.62f, kText);
            textAt(g_speedUnits ? "mph" : "km/h", cx, r.y + bh * 0.82f, bh * 0.2f, kAccent);
            break;
        }
        case Rpm:
            drawRpm(me, r, b.style);
            break;
        case Gear:
        {
            // Gear 0 is reverse: GT3's own gauge shows a red R for it (on the
            // grid rolling back, and backing up with the brake held; RAM dumps
            // `rev-*`, `ped-*` 2026-09-30 - it was shown here as N).
            char t[8];
            const bool reverse = me.gear <= 0;
            if (reverse)
                std::snprintf(t, sizeof(t), "R");
            else if (me.gear <= 9)
                std::snprintf(t, sizeof(t), "%d", me.gear);
            else
                std::snprintf(t, sizeof(t), "N");
            textAt(t, cx, cy, bh * 0.85f, reverse ? Color{235, 60, 50, 255} : kAccent);
            break;
        }
        case Shift:
        {
            const int red = me.redline > 2000 ? me.redline : 8000;
            const float start = red * 0.72f;
            const int lit = me.rpm < start ? 0 : std::clamp(static_cast<int>((me.rpm - start) / (red - start) * 10.0f) + 1, 0, 10);
            const bool flash = me.rpm >= red * 0.985f && std::fmod(GetTime() * 8.0, 2.0) < 1.0;
            const float d = std::min(bh * 0.7f, bw / 11.5f);
            for (int i = 0; i < 10; ++i)
            {
                const Color on = flash ? Color{80, 150, 255, 255} : (i < 4 ? Color{60, 220, 90, 255} : (i < 7 ? Color{255, 210, 50, 255} : Color{255, 60, 50, 255}));
                const Vector2 c{r.x + bw * (0.5f + (i - 4.5f) * (1.0f / 10.8f)), cy};
                DrawCircleV(c, d * 0.42f, i < lit || flash ? on : Fade(on, 0.15f));
            }
            break;
        }
        case Position:
        {
            textAt(ordinal(me.position).c_str(), r.x + bw * 0.42f, cy, bh * 0.7f, kAccent);
            textAt(("/" + std::to_string(s.cars.size())).c_str(), r.x + bw * 0.86f, cy + bh * 0.12f, bh * 0.34f, kMuted);
            break;
        }
        case Lap:
        {
            // LAP n (/total when GT3's HUD knows it).
            textAt("LAP", r.x + bw * 0.22f, cy, bh * 0.36f, kMuted);
            const std::string lapText = std::to_string(std::max(me.lap, 1));
            if (s.totalLaps > 0)
            {
                textAt(lapText.c_str(), r.x + bw * 0.62f, cy, bh * 0.7f, kText, 1.0f);
                textAt(("/" + std::to_string(s.totalLaps)).c_str(), r.x + bw * 0.63f, cy + bh * 0.1f, bh * 0.4f, kMuted, 0.0f);
            }
            else
                textAt(lapText.c_str(), r.x + bw * 0.7f, cy, bh * 0.7f, kText);
            break;
        }
        case Times:
        {
            const long long current = me.lap >= 1 && s.clockMs >= me.lapStartMs ? static_cast<long long>(s.clockMs - me.lapStartMs) : -1;
            const long long last = validLap(me.lastLapMs) ? static_cast<long long>(me.lastLapMs) : -1;
            const long long best = g.best.count(me.index) && g.best[me.index] ? static_cast<long long>(g.best[me.index]) : -1;
            if (b.style == 2)
            {
                // Lap and best side by side.
                textAt("LAP", r.x + bw * 0.05f, r.y + bh * 0.27f, bh * 0.22f, kMuted, 0.0f);
                textAt(lapTime(current).c_str(), r.x + bw * 0.05f, r.y + bh * 0.64f, bh * 0.4f, kText, 0.0f);
                textAt("BEST", r.x + bw * 0.53f, r.y + bh * 0.27f, bh * 0.22f, kMuted, 0.0f);
                textAt(lapTime(best).c_str(), r.x + bw * 0.53f, r.y + bh * 0.64f, bh * 0.4f, kAccent, 0.0f);
                break;
            }
            if (b.style == 1)
            {
                // Lap time, the live difference to the best lap, the best.
                textAt("LAP", r.x + bw * 0.06f, r.y + bh * 0.2f, bh * 0.19f, kMuted, 0.0f);
                textAt(lapTime(current).c_str(), r.x + bw * 0.94f, r.y + bh * 0.2f, bh * 0.24f, kText, 1.0f);
                const long long at = current >= 0 ? bestTimeAt(me.progress) : -1;
                const long long delta = at >= 0 ? current - at : 0;
                textAt("BEST", r.x + bw * 0.06f, r.y + bh * 0.8f, bh * 0.19f, kMuted, 0.0f);
                textAt(lapTime(best).c_str(), r.x + bw * 0.94f, r.y + bh * 0.8f, bh * 0.24f, kAccent, 1.0f);
                if (at >= 0)
                    textAt(signedSeconds(delta).c_str(), r.x + bw * 0.5f, r.y + bh * 0.5f, bh * 0.3f, delta <= 0 ? kGreen : kRed);
                else
                    textAt("+-.---", r.x + bw * 0.5f, r.y + bh * 0.5f, bh * 0.3f, kMuted);
                break;
            }
            const char *labels[3] = {"LAP", "LAST", "BEST"};
            const long long values[3] = {current, last, best};
            for (int i = 0; i < 3; ++i)
            {
                const float ly = r.y + bh * (0.2f + 0.3f * i);
                textAt(labels[i], r.x + bw * 0.06f, ly, bh * 0.19f, kMuted, 0.0f);
                textAt(lapTime(values[i]).c_str(), r.x + bw * 0.94f, ly, bh * 0.24f, i == 2 ? kAccent : kText, 1.0f);
            }
            break;
        }
        case Map:
        {
            // GT3's own course edges (tried every 2 s until found), else the cars only.
            float x0 = 1e9f, x1 = -1e9f, z0 = 1e9f, z1 = -1e9f;
            if (!g.edges.empty())
                x0 = g.edgeX0, x1 = g.edgeX1, z0 = g.edgeZ0, z1 = g.edgeZ1;
            else
                for (const gt3race::Car &c : s.cars)
                {
                    x0 = std::min(x0, c.x), x1 = std::max(x1, c.x);
                    z0 = std::min(z0, c.z), z1 = std::max(z1, c.z);
                }
            const float pad = bw * 0.08f;
            MapView v;
            v.ox = (x0 + x1) * 0.5f;
            v.oz = (z0 + z1) * 0.5f;
            v.sx = cx;
            v.sy = cy;
            if (b.style == 1)
            {
                // Turned with the car: the whole course fits at any angle.
                v.turned = true;
                v.fx = -std::sin(me.yaw), v.fz = -std::cos(me.yaw);
                v.rx = -v.fz, v.rz = v.fx;
                const float radius = std::max(1.0f, std::sqrt((x1 - x0) * (x1 - x0) + (z1 - z0) * (z1 - z0)) * 0.5f);
                v.k = (std::min(bw, bh) * 0.5f - pad) / radius;
            }
            else
                v.k = std::min((bw - 2 * pad) / std::max(1.0f, x1 - x0), (bh - 2 * pad) / std::max(1.0f, z1 - z0));
            drawCourse(s, v, bh, 0.0f, 0.0f, 0.0f, b.style == 1);
            break;
        }
        case ZoomMap:
        {
            // The course around the player, `range` metres across the box.
            MapView v;
            v.ox = me.x;
            v.oz = me.z;
            v.sx = cx;
            v.turned = b.style == 0;
            v.sy = v.turned ? r.y + bh * 0.62f : cy; // turned: more of the road ahead
            v.fx = -std::sin(me.yaw), v.fz = -std::cos(me.yaw);
            v.rx = -v.fz, v.rz = v.fx;
            v.k = std::min(bw, bh) / static_cast<float>(std::max(100, b.range));
            BeginScissorMode(static_cast<int>(r.x), static_cast<int>(r.y), static_cast<int>(bw), static_cast<int>(bh));
            drawCourse(s, v, bh, b.range * 0.9f, me.x, me.z, true);
            EndScissorMode();
            if (g.edges.empty())
                textAt("course not found yet", cx, r.y + bh * 0.1f, bh * 0.06f, kMuted);
            break;
        }
        case Tower:
        {
            // Every car in running order: place, name, gap / interval / last lap.
            std::vector<const gt3race::Car *> order;
            for (const gt3race::Car &c : s.cars)
                order.push_back(&c);
            std::sort(order.begin(), order.end(), [](const gt3race::Car *a, const gt3race::Car *c) { return a->position < c->position; });
            const int n = static_cast<int>(order.size());
            const gt3race::Car *first = order.front();
            // The gap of a car to the leader (or, interval, to the car ahead):
            // "+1.234", "+1 LAP"; the leader's "LEAD".
            auto gapText = [&](const gt3race::Car &c, bool interval) -> std::string {
                if (&c == first)
                    return "LEAD";
                const gt3race::Car *ahead = first;
                if (interval)
                    for (const gt3race::Car *o : order)
                        if (o->position == c.position - 1)
                            ahead = o;
                const double d = raceDistance(c.lap, c.progress);
                const double aheadD = raceDistance(ahead->lap, ahead->progress);
                const int lapsDown = g.lapLength > 1.0f ? static_cast<int>((aheadD - d) / g.lapLength) : 0;
                if (lapsDown >= 1)
                    return "+" + std::to_string(lapsDown) + (lapsDown == 1 ? " LAP" : " LAPS");
                const double at = timeAt(ahead->index, d);
                return at >= 0.0 ? signedSeconds(static_cast<long long>(s.clockMs - at)) : "";
            };
            // Names and the best / last lap column switching every 5 s (the user: "can it switch between
            // car and driver name, and ... best lap and last lap"): the same moment for both.
            const bool secondHalf = static_cast<long long>(GetTime() / kTowerSwitchSeconds) % 2 == 1;
            const bool drivers = g_towerNames == 1 || (g_towerNames == 2 && secondHalf);
            // A car's name: its driver (gt3report::driverName: the player's own name, made-up AI drivers)
            // or its model ("Car N" with numbers, or when not found).
            auto carName = [&](const gt3race::Car &c, bool numbers) {
                if (drivers && !numbers)
                    return gt3report::driverName(c.index);
                std::string name = numbers ? std::string() : nameOf(c.index);
                return name.empty() ? "Car " + std::to_string(c.index + 1) : name;
            };
            // The time column (Box::timeMode, F5 cycles it): best lap, last
            // lap or this lap; the fastest best lap in purple. Switching: best
            // and last lap in turn.
            int timeMode = std::clamp(b.timeMode, 0, kTowerTimeCount - 1);
            if (g_towerTimeSwitch && timeMode != TimeCurrent)
                timeMode = secondHalf ? TimeLast : TimeBest;
            uint32_t fastest = 0u;
            for (const auto &kv : g.best)
                if (kv.second && (fastest == 0u || kv.second < fastest))
                    fastest = kv.second;
            auto timeCell = [&](const gt3race::Car &c, Color &colour) -> std::string {
                colour = kText;
                if (timeMode == TimeCurrent)
                {
                    const long long now = c.lap >= 1 && s.clockMs >= c.lapStartMs ? static_cast<long long>(s.clockMs - c.lapStartMs) : -1;
                    return now >= 0 ? lapTime(now) : "-";
                }
                if (timeMode == TimeLast)
                    return validLap(c.lastLapMs) ? lapTime(c.lastLapMs) : "-'--.---";
                const auto it = g.best.find(c.index);
                if (it == g.best.end() || it->second == 0u)
                    return "-'--.---";
                if (it->second == fastest)
                    colour = Color{200, 130, 255, 255};
                return lapTime(it->second);
            };
            if (b.style >= 3)
            {
                // A table (as the phone's): the columns of the custom style,
                // or place, colour, car (names / "Car 1".."Car 6"), lap, time,
                // gap. Widths: gt3_tower_layout.h (the launcher's too).
                const int columns = b.style == kTowerCustom ? b.columns : (kTowerDefaultColumns | (b.style == 4 ? ColCarNumber : 0));
                const bool numbers = (columns & ColCarNumber) != 0;
                float total = kTowerMargin;
                for (int k = 0; k < kTowerColumnCount; ++k)
                    if (columns & (1 << k))
                        total += towerColumnWidth(k, numbers);
                const float unit = bw / total, rowH = bh / (n + 1.25f), size = rowH * 0.5f, pad = unit * 0.25f;
                float colX0[kTowerColumnCount] = {}, colX1[kTowerColumnCount] = {};
                {
                    float x = r.x + unit * kTowerMargin * 0.5f;
                    for (int k = 0; k < kTowerColumnCount; ++k)
                        if (columns & (1 << k))
                        {
                            colX0[k] = x;
                            x += towerColumnWidth(k, numbers) * unit;
                            colX1[k] = x;
                        }
                }
                auto on = [&](int k) { return (columns & (1 << k)) != 0; };
                auto mid = [&](int k) { return (colX0[k] + colX1[k]) * 0.5f; };
                static const char *timeHeads[kTowerTimeCount] = {"BEST LAP", "LAST LAP", "LAP TIME"};
                const float headY = r.y + rowH * 0.6f, headSize = rowH * 0.4f;
                if (on(0))
                    textAt("POS", mid(0), headY, headSize, kMuted);
                if (on(2))
                    textAt(drivers && !numbers ? "DRIVER" : "CAR", colX0[2] + pad, headY, headSize, kMuted, 0.0f);
                if (on(3))
                    textAt("LAP", mid(3), headY, headSize, kMuted);
                if (on(4))
                    textAt(timeHeads[timeMode], colX1[4] - pad, headY, headSize, kMuted, 1.0f);
                if (on(5))
                    textAt("GAP", colX1[5] - pad, headY, headSize, kMuted, 1.0f);
                if (on(6))
                    textAt("INT", colX1[6] - pad, headY, headSize, kMuted, 1.0f);
                for (int i = 0; i < n; ++i)
                {
                    const gt3race::Car &c = *order[static_cast<size_t>(i)];
                    const float ry = r.y + rowH * (1.15f + i), rowMid = ry + rowH * 0.5f;
                    if (c.index == 0)
                        DrawRectangleRounded(Rectangle{r.x + bw * 0.012f, ry + rowH * 0.05f, bw * 0.976f, rowH * 0.9f}, 0.3f, 4, Fade(kPlayer, 0.25f));
                    if (on(0))
                    {
                        const float badgeW = std::min(rowH * 0.95f, (colX1[0] - colX0[0]) * 0.8f);
                        const Rectangle badge{mid(0) - badgeW * 0.5f, ry + rowH * 0.14f, badgeW, rowH * 0.72f};
                        DrawRectangleRounded(badge, 0.25f, 4, c.position == 1 ? kAccent : Color{230, 232, 238, 255});
                        textAt(std::to_string(c.position).c_str(), mid(0), rowMid, rowH * 0.56f, Color{20, 22, 26, 255});
                    }
                    if (on(1))
                    {
                        DrawCircleV(Vector2{mid(1), rowMid}, rowH * 0.2f, carColour(c));
                        DrawCircleLinesV(Vector2{mid(1), rowMid}, rowH * 0.2f, Fade(BLACK, 0.5f));
                    }
                    if (on(2))
                        textAt(fitted(carName(c, numbers), size, colX1[2] - colX0[2] - pad * 2.0f).c_str(), colX0[2] + pad, rowMid, size,
                               c.index == 0 ? WHITE : kText, 0.0f);
                    if (on(3))
                        textAt(std::to_string(std::max(c.lap, 1)).c_str(), mid(3), rowMid, size, kText);
                    if (on(4))
                    {
                        Color tc;
                        const std::string t = timeCell(c, tc);
                        textAt(t.c_str(), colX1[4] - pad, rowMid, size, tc, 1.0f);
                    }
                    if (on(5))
                        textAt(gapText(c, false).c_str(), colX1[5] - pad, rowMid, size, &c == first ? kAccent : kText, 1.0f);
                    if (on(6))
                        textAt(gapText(c, true).c_str(), colX1[6] - pad, rowMid, size, &c == first ? kAccent : kText, 1.0f);
                }
                break;
            }
            const float rowH = bh / (n + 1.3f);
            static const char *heads[3] = {"GAP", "INT", "LAST"};
            textAt("POS", r.x + bw * 0.05f, r.y + rowH * 0.6f, rowH * 0.42f, kMuted, 0.0f);
            textAt(heads[std::clamp(b.style, 0, 2)], r.x + bw * 0.95f, r.y + rowH * 0.6f, rowH * 0.42f, kMuted, 1.0f);
            for (int i = 0; i < n; ++i)
            {
                const gt3race::Car &c = *order[static_cast<size_t>(i)];
                const float ry = r.y + rowH * (1.2f + i), mid = ry + rowH * 0.5f;
                if (c.index == 0)
                    DrawRectangleRounded(Rectangle{r.x + bw * 0.02f, ry + rowH * 0.06f, bw * 0.96f, rowH * 0.88f}, 0.3f, 4, Fade(kPlayer, 0.22f));
                const Rectangle badge{r.x + bw * 0.04f, ry + rowH * 0.14f, rowH * 0.95f, rowH * 0.72f};
                DrawRectangleRounded(badge, 0.25f, 4, c.position == 1 ? kAccent : Color{230, 232, 238, 255});
                textAt(std::to_string(c.position).c_str(), badge.x + badge.width * 0.5f, mid, rowH * 0.56f, Color{20, 22, 26, 255});
                // The car's colour (as its map dot), then its name.
                const Vector2 dot{badge.x + badge.width + rowH * 0.42f, mid};
                DrawCircleV(dot, rowH * 0.2f, carColour(c));
                DrawCircleLinesV(dot, rowH * 0.2f, Fade(BLACK, 0.5f));
                std::string name = drivers ? gt3report::driverName(c.index) : nameOf(c.index);
                if (name.empty())
                    name = c.index == 0 ? "YOU" : "CAR " + std::to_string(c.index + 1);
                const float nameX = dot.x + rowH * 0.42f;
                textAt(fitted(name, rowH * 0.5f, r.x + bw * 0.64f - nameX).c_str(), nameX, mid, rowH * 0.5f, c.index == 0 ? kPlayer : kText, 0.0f);
                std::string value;
                Color vc = kText;
                if (b.style == 2)
                    value = validLap(c.lastLapMs) ? lapTime(c.lastLapMs) : "-'--.---";
                else
                {
                    // The leader: LEAD (it showed the lap before, 2026-09-30).
                    value = gapText(c, b.style == 1);
                    if (&c == first)
                        vc = kAccent;
                }
                textAt(value.c_str(), r.x + bw * 0.95f, mid, rowH * 0.5f, vc, 1.0f);
            }
            break;
        }
        case Lights:
        {
            // GT3's gauge lamps, lit as GT3's HUD lights them.
            static const char *names[4] = {"ASM", "TCS", "P", "OIL"};
            const Color lit[4] = {kGreen, kYellow, kRed, Color{255, 150, 40, 255}};
            for (int k = 0; k < 4; ++k)
            {
                const float mx = r.x + bw * (0.14f + 0.24f * k);
                const Color c = s.lamps[k] ? lit[k] : Fade(kMuted, 0.3f);
                if (k == 2)
                {
                    // The parking brake: (P) in a ring.
                    DrawRing(Vector2{mx, cy}, bh * 0.26f, bh * 0.32f, 0.0f, 360.0f, 32, c);
                    textAt(names[k], mx, cy, bh * 0.4f, c);
                }
                else
                    textAt(names[k], mx, cy, bh * 0.44f, c);
            }
            break;
        }
        case Odo:
        {
            // GT3's gauge odometer counts this race's distance (000003 after two
            // laps of Super Speedway, 4.8 km, in mph units): car + 0x1374.
            const float d = std::max(0.0f, me.distance) / (g_speedUnits ? 1609.344f : 1000.0f);
            char t[32];
            std::snprintf(t, sizeof(t), "%.1f", d);
            textAt("ODO", r.x + bw * 0.16f, cy, bh * 0.42f, kMuted);
            textAt(t, r.x + bw * 0.58f, cy, bh * 0.62f, WHITE);
            textAt(g_speedUnits ? "mi" : "km", r.x + bw * 0.88f, cy, bh * 0.4f, kAccent);
            break;
        }
        case Splits:
        {
            // Three sectors: this lap's (done: purple if the fastest of the race
            // so far, green if the player's best, yellow if not; running: its
            // time so far), else the last lap's in grey.
            const long long now = me.lap >= 1 && s.clockMs >= me.lapStartMs ? static_cast<long long>(s.clockMs - me.lapStartMs) : -1;
            const long long done[2] = {g.sector[0], g.sector[1]};
            const int running = done[0] < 0 ? 0 : (done[1] < 0 ? 1 : 2);
            for (int k = 0; k < 3; ++k)
            {
                const float colX = r.x + bw * (0.035f + k * 0.31f), colW = bw * 0.3f, mx = colX + colW * 0.5f;
                char label[4] = {'S', static_cast<char>('1' + k), 0};
                textAt(label, mx, r.y + bh * 0.2f, bh * 0.2f, kMuted);
                std::string value;
                Color vc = kMuted, bar = Fade(WHITE, 0.15f);
                if (now >= 0 && k < running)
                {
                    const long long t = done[k];
                    const bool best = g.bestSectors[k] < 0 || t <= g.bestSectors[k];
                    const bool fastest = g.fieldBest[k] >= 0 && t <= g.fieldBest[k];
                    vc = fastest ? kPurple : (best ? kGreen : kYellow);
                    bar = vc;
                    value = b.style == 1 && g.bestSectors[k] >= 0 ? signedSeconds(t - g.bestSectors[k]) : seconds(t);
                }
                else if (now >= 0 && k == running)
                {
                    const long long t = now - (k >= 1 ? done[0] : 0) - (k >= 2 ? done[1] : 0);
                    vc = kText;
                    bar = Fade(kText, 0.5f);
                    value = b.style == 1 ? "..." : seconds(std::max(0ll, t));
                }
                else if (g.lastSectors[k] >= 0)
                {
                    value = b.style == 1 && g.bestSectors[k] >= 0 ? signedSeconds(g.lastSectors[k] - g.bestSectors[k]) : seconds(g.lastSectors[k]);
                    bar = Fade(g.lastWasFastest[k] ? kPurple : (g.lastWasBest[k] ? kGreen : kYellow), 0.35f);
                }
                else
                    value = "--.---";
                textAt(value.c_str(), mx, r.y + bh * 0.55f, fitSize(value.c_str(), bh * 0.3f, colW * 0.9f), vc);
                DrawRectangleRounded(Rectangle{colX + colW * 0.08f, r.y + bh * 0.8f, colW * 0.84f, bh * 0.08f}, 0.5f, 4, bar);
            }
            break;
        }
        default:
            break;
        }
    }
}
