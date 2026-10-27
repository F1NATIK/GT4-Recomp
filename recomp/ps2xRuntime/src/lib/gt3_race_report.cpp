#include "gt3_race_report.h"

#include "gt3_race_info.h"
#include "raylib.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace gt3report
{
    namespace
    {
        bool g_on = false, g_mph = true;
        std::string g_dir; // where reports are saved (empty: not saved)
        // Names (user 2026-10-03: "can we make switch between the drivers name and their car"): GT3's AI
        // cars have no drivers, so they get GT_DRIVER_NAMES (the GT Race Lab's made-up roster by
        // default), the player GT3_PLAYER_NAME; GT3_RACE_REPORT_NAMES=cars starts on the car names;
        // the report's Drivers / Cars switch (click, or Ctrl+F2) changes it.
        bool g_showDrivers = true;
        std::string g_playerName = "You";
        std::vector<std::string> g_driverNames = {"Kenji Arata", "Sofia Lindqvist", "Dale Brennan", "Amelie Duret",
                                                  "Tomas Varga", "Rhys Calder", "Haruto Moriyama", "Nadia Okafor"};
        void loadNames()
        {
            static const bool once = [] {
                if (const char *p = std::getenv("GT3_PLAYER_NAME"); p && *p)
                    g_playerName = p;
                if (const char *d = std::getenv("GT3_DRIVER_NAMES"); d && *d)
                {
                    std::vector<std::string> list;
                    std::string cur;
                    for (const char *q = d;; ++q)
                    {
                        if (*q == ',' || *q == 0)
                        {
                            const size_t a = cur.find_first_not_of(' '), b = cur.find_last_not_of(' ');
                            if (a != std::string::npos)
                                list.push_back(cur.substr(a, b - a + 1));
                            cur.clear();
                            if (*q == 0)
                                break;
                        }
                        else
                            cur += *q;
                    }
                    if (!list.empty())
                        g_driverNames = list;
                }
                if (const char *n = std::getenv("GT3_RACE_REPORT_NAMES"); n && std::strcmp(n, "cars") == 0)
                    g_showDrivers = false;
                return true;
            }();
            (void)once;
        }

        struct Sample
        {
            uint32_t clock;
            int lap;
            float progress;
            int pos;
        };
        struct CarLog
        {
            std::vector<Sample> trace;
            std::vector<uint32_t> lapMs;  // GT3's lap times, as its HUD shows them
            std::vector<float> lapTop;    // top speed (m/s) in each lap
            uint32_t prevLast = 0;
            int prevLap = -1;
            float curTop = 0.0f, top = 0.0f;
            int grid = 0;
        };
        struct RaceLog
        {
            uint32_t race = 0, lastClock = 0;
            int totalLaps = 0;
            float lapLength = 1.0f; // the largest progress seen
            std::vector<CarLog> cars;
            bool playerDone = false;
            double doneAt = 0.0;
        };
        RaceLog g_log;

        struct CarReport
        {
            int index = 0;
            bool player = false;
            std::string name;   // as shown: the driver's or the car's (setNames)
            std::string car, driver;
            int pos = 0, grid = 0, laps = 0, led = 0;
            bool finished = false;
            double time = -1.0; // ms, the race time when finished
            std::vector<uint32_t> lapMs;
            std::vector<std::array<double, 3>> sectors; // per lap, ms (-1: not known)
            std::vector<int> lapPos;
            int bestLap = -1;   // index into lapMs
            double avg = 0.0, sd = 0.0;
            double bestSec[3] = {-1, -1, -1}, ideal = -1.0;
            float top = 0.0f;
        };
        struct Report
        {
            bool valid = false, finished = false;
            int totalLaps = 0, lapsShown = 0;
            float lapLength = 0.0f;
            std::vector<CarReport> cars; // by finishing place
            double fastest = -1.0;
            int fastestCar = -1;
            double bestSecAll[3] = {-1, -1, -1};
            std::string when;
        };
        Report g_report;          // the one shown
        bool g_reportFromFinish = false;
        int g_page = 0;
        bool g_visible = false, g_autoShown = false, g_saved = false;
        double g_lastBuild = 0.0;

        std::mutex g_namesMutex;
        std::vector<std::string> g_names; // by car index, from the worker
        std::atomic<bool> g_namesBusy{false};
        const uint8_t *g_rdram = nullptr;

        double raceDistance(int lap, float progress, float L) { return (lap - 1) * static_cast<double>(L) + progress; }
        // Clock (ms) when car c first reached race distance d (-1: not yet).
        double timeAt(const CarLog &c, double d, float L)
        {
            const auto &tr = c.trace;
            if (tr.empty() || raceDistance(tr.back().lap, tr.back().progress, L) < d)
                return -1.0;
            size_t lo = 0, hi = tr.size() - 1;
            while (lo < hi)
            {
                const size_t mid = (lo + hi) / 2;
                if (raceDistance(tr[mid].lap, tr[mid].progress, L) < d)
                    lo = mid + 1;
                else
                    hi = mid;
            }
            if (lo == 0)
                return tr[0].clock;
            const double d0 = raceDistance(tr[lo - 1].lap, tr[lo - 1].progress, L), d1 = raceDistance(tr[lo].lap, tr[lo].progress, L);
            return d1 > d0 ? tr[lo - 1].clock + (static_cast<double>(tr[lo].clock) - tr[lo - 1].clock) * (d - d0) / (d1 - d0) : tr[lo].clock;
        }
        int posAt(const CarLog &c, double clock)
        {
            int p = c.grid;
            for (const Sample &s : c.trace)
            {
                if (s.clock > clock)
                    break;
                p = s.pos;
            }
            return p;
        }

        void fetchNames()
        {
            if (!g_rdram || g_namesBusy.exchange(true))
                return;
            std::thread([] {
                gt3race::Snapshot s;
                if (gt3race::read(g_rdram, s, true))
                {
                    std::lock_guard<std::mutex> lock(g_namesMutex);
                    g_names.assign(s.cars.size(), std::string());
                    for (const gt3race::Car &c : s.cars)
                        if (c.index >= 0 && static_cast<size_t>(c.index) < g_names.size())
                            g_names[static_cast<size_t>(c.index)] = c.name;
                }
                g_namesBusy = false;
            }).detach();
        }

        std::string lapText(double ms)
        {
            if (ms < 0.0)
                return "-";
            const long long t = static_cast<long long>(ms + 0.5);
            char b[32];
            std::snprintf(b, sizeof(b), "%lld'%02lld.%03lld", t / 60000, (t / 1000) % 60, t % 1000);
            return b;
        }
        std::string secText(double ms)
        {
            if (ms < 0.0)
                return "-";
            char b[32];
            std::snprintf(b, sizeof(b), "%.3f", ms / 1000.0);
            return b;
        }
        std::string gapText(double ms)
        {
            char b[32];
            std::snprintf(b, sizeof(b), "+%.3f", ms / 1000.0);
            return b;
        }
        std::string speedText(float ms)
        {
            char b[32];
            std::snprintf(b, sizeof(b), "%.0f %s", ms * (g_mph ? 2.23694f : 3.6f), g_mph ? "mph" : "km/h");
            return b;
        }

        // The names shown: the driver's or the car's; the player's car marked (an AI car may drive the same model).
        void setNames(Report &r)
        {
            for (CarReport &c : r.cars)
                c.name = g_showDrivers ? c.driver : c.car + (c.player ? " (you)" : "");
        }

        Report build()
        {
            Report r;
            const RaceLog &g = g_log;
            const float L = g.lapLength;
            if (g.cars.empty() || L < 100.0f)
                return r;
            r.valid = true;
            r.totalLaps = g.totalLaps;
            r.lapLength = L;
            r.finished = g.playerDone;
            std::vector<std::string> names;
            {
                std::lock_guard<std::mutex> lock(g_namesMutex);
                names = g_names;
            }
            for (size_t k = 0; k < g.cars.size(); ++k)
            {
                const CarLog &c = g.cars[k];
                CarReport cr;
                cr.index = static_cast<int>(k);
                cr.player = k == 0;
                cr.car = k < names.size() && !names[k].empty() ? names[k] : "Car " + std::to_string(k + 1);
                cr.driver = driverName(static_cast<int>(k));
                cr.grid = c.grid;
                cr.lapMs = c.lapMs;
                cr.laps = static_cast<int>(c.lapMs.size());
                cr.top = c.top;
                cr.pos = c.trace.empty() ? 0 : c.trace.back().pos;
                cr.finished = g.totalLaps > 0 && cr.laps >= g.totalLaps;
                if (cr.finished)
                    cr.time = timeAt(c, static_cast<double>(g.totalLaps) * L, L);
                for (int n = 1; n <= cr.laps; ++n)
                {
                    const double base = (n - 1) * static_cast<double>(L);
                    const double t0 = timeAt(c, base, L), t1 = timeAt(c, base + L / 3.0, L), t2 = timeAt(c, base + 2.0 * L / 3.0, L);
                    std::array<double, 3> s{-1.0, -1.0, -1.0};
                    if (t0 >= 0.0 && t1 >= t0 && t2 >= t1)
                    {
                        s[0] = t1 - t0;
                        s[1] = t2 - t1;
                        s[2] = cr.lapMs[static_cast<size_t>(n - 1)] - s[0] - s[1]; // the three add up to GT3's lap time
                        if (s[2] < 0.0)
                            s = {-1.0, -1.0, -1.0};
                    }
                    cr.sectors.push_back(s);
                    const double tEnd = timeAt(c, n * static_cast<double>(L), L);
                    const int p = tEnd >= 0.0 ? posAt(c, tEnd + 1.0) : cr.pos;
                    cr.lapPos.push_back(p);
                    cr.led += p == 1 ? 1 : 0;
                }
                for (int i = 0; i < cr.laps; ++i)
                    if (cr.bestLap < 0 || cr.lapMs[static_cast<size_t>(i)] < cr.lapMs[static_cast<size_t>(cr.bestLap)])
                        cr.bestLap = i;
                // Average and spread of the flying laps (lap 1 starts from the grid / pits).
                const int from = cr.laps >= 3 ? 1 : 0;
                double sum = 0.0, sq = 0.0;
                int n = 0;
                for (int i = from; i < cr.laps; ++i, ++n)
                {
                    sum += cr.lapMs[static_cast<size_t>(i)];
                    sq += static_cast<double>(cr.lapMs[static_cast<size_t>(i)]) * cr.lapMs[static_cast<size_t>(i)];
                }
                if (n > 0)
                {
                    cr.avg = sum / n;
                    cr.sd = n > 1 ? std::sqrt(std::max(0.0, sq / n - cr.avg * cr.avg)) : 0.0;
                }
                for (const auto &s : cr.sectors)
                    for (int j = 0; j < 3; ++j)
                        if (s[static_cast<size_t>(j)] >= 0.0 && (cr.bestSec[j] < 0.0 || s[static_cast<size_t>(j)] < cr.bestSec[j]))
                            cr.bestSec[j] = s[static_cast<size_t>(j)];
                if (cr.bestSec[0] >= 0.0 && cr.bestSec[1] >= 0.0 && cr.bestSec[2] >= 0.0)
                    cr.ideal = cr.bestSec[0] + cr.bestSec[1] + cr.bestSec[2];
                r.lapsShown = std::max(r.lapsShown, cr.laps);
                r.cars.push_back(cr);
            }
            // Finishing order: finished by time, then by laps and race distance (GT3's position).
            std::sort(r.cars.begin(), r.cars.end(), [](const CarReport &a, const CarReport &b) {
                if (a.finished != b.finished)
                    return a.finished;
                if (a.finished && a.time >= 0.0 && b.time >= 0.0 && a.time != b.time)
                    return a.time < b.time;
                return (a.pos > 0 ? a.pos : 99) < (b.pos > 0 ? b.pos : 99);
            });
            for (size_t i = 0; i < r.cars.size(); ++i)
                r.cars[i].pos = static_cast<int>(i) + 1;
            for (const CarReport &c : r.cars)
            {
                if (c.bestLap >= 0 && (r.fastest < 0.0 || c.lapMs[static_cast<size_t>(c.bestLap)] < r.fastest))
                {
                    r.fastest = c.lapMs[static_cast<size_t>(c.bestLap)];
                    r.fastestCar = c.index;
                }
                for (int j = 0; j < 3; ++j)
                    if (c.bestSec[j] >= 0.0 && (r.bestSecAll[j] < 0.0 || c.bestSec[j] < r.bestSecAll[j]))
                        r.bestSecAll[j] = c.bestSec[j];
            }
            const std::time_t now = std::time(nullptr);
            char when[64];
            std::strftime(when, sizeof(when), "%Y-%m-%d %H:%M", std::localtime(&now));
            r.when = when;
            setNames(r);
            return r;
        }

        // ---- the saved report (HTML) ----
        std::string esc(const std::string &s)
        {
            std::string o;
            for (char ch : s)
                o += ch == '<' ? "&lt;" : ch == '>' ? "&gt;" : ch == '&' ? "&amp;" : std::string(1, ch);
            return o;
        }
        // The cars' colours as the timing tower and the maps draw them (gt3_hud.cpp carColour): the
        // player white, the others by car number.
        Color carColour(int index)
        {
            static const Color colours[8] = {{255, 210, 74, 255},  {74, 210, 255, 255},  {255, 106, 106, 255}, {125, 255, 122, 255},
                                             {209, 140, 255, 255}, {255, 162, 74, 255},  {255, 122, 217, 255}, {170, 170, 170, 255}};
            return index == 0 ? Color{255, 255, 255, 255} : colours[index % 8];
        }
        std::string carColourHex(int index)
        {
            const Color c = carColour(index);
            char b[16];
            std::snprintf(b, sizeof(b), "#%02x%02x%02x", c.r, c.g, c.b);
            return b;
        }

        void save(const Report &r)
        {
            if (g_dir.empty() || !r.valid)
                return;
            std::error_code ec;
            std::filesystem::create_directories(g_dir, ec);
            const std::time_t now = std::time(nullptr);
            char stamp[64];
            std::strftime(stamp, sizeof(stamp), "%Y-%m-%d_%H%M%S", std::localtime(&now));
            const std::string path = g_dir + "\\race-" + stamp + ".html";
            std::ofstream f(path);
            if (!f)
                return;
            f << "<!doctype html><html><head><meta charset=\"utf-8\"><title>GT3 race " << r.when << "</title><style>"
              << "body{background:#14161b;color:#e8eaee;font-family:Segoe UI,Arial,sans-serif;margin:24px}h1{margin:0 0 4px}"
              << "h2{margin:28px 0 8px;color:#9fb3ff}table{border-collapse:collapse}td,th{padding:4px 10px;text-align:right;"
              << "border-bottom:1px solid #2a2e37}th{color:#9aa0aa;font-weight:600}td.l,th.l{text-align:left}.you{background:#22283a}"
              << ".best{color:#56c880;font-weight:700}.fast{color:#c58bff;font-weight:700}.muted{color:#9aa0aa}</style></head><body>";
            f << "<h1>Race report</h1><div class=muted>" << r.when << " &middot; " << r.totalLaps << " laps &middot; lap "
              << std::fixed;
            f.precision(2);
            f << r.lapLength / 1000.0f << " km</div>";
            f << "<h2>Results</h2><table><tr><th>P</th><th class=l>Driver</th><th class=l>Car</th><th>Laps</th><th>Time</th><th>Gap</th><th>Best lap</th><th>Top speed</th><th>Grid</th><th>Led</th></tr>";
            const CarReport *winner = r.cars.empty() ? nullptr : &r.cars.front();
            for (const CarReport &c : r.cars)
            {
                std::string gap = "-";
                if (winner && c.finished && winner->time >= 0.0 && c.time >= 0.0)
                    gap = &c == winner ? "" : gapText(c.time - winner->time);
                else if (winner && !c.finished)
                    gap = "+" + std::to_string(std::max(0, r.totalLaps - c.laps)) + " lap" + (r.totalLaps - c.laps == 1 ? "" : "s");
                const double best = c.bestLap >= 0 ? c.lapMs[static_cast<size_t>(c.bestLap)] : -1.0;
                f << "<tr" << (c.player ? " class=you" : "") << "><td>" << c.pos << "</td><td class=l>" << esc(c.driver) << "</td><td class=l>"
                  << esc(c.car) << "</td><td>" << c.laps
                  << "</td><td>" << (c.finished ? lapText(c.time) : "-") << "</td><td>" << gap << "</td><td class=" << (c.index == r.fastestCar ? "fast" : "")
                  << ">" << lapText(best) << "</td><td>" << speedText(c.top) << "</td><td>" << c.grid << "</td><td>" << c.led << "</td></tr>";
            }
            f << "</table><h2>Lap times</h2><table><tr><th class=l>Driver</th>";
            for (int n = 1; n <= r.lapsShown; ++n)
                f << "<th>L" << n << "</th>";
            f << "</tr>";
            for (const CarReport &c : r.cars)
            {
                f << "<tr" << (c.player ? " class=you" : "") << "><td class=l>" << esc(c.driver) << " <span class=muted>" << esc(c.car) << "</span></td>";
                for (int n = 0; n < r.lapsShown; ++n)
                {
                    if (n >= c.laps)
                    {
                        f << "<td></td>";
                        continue;
                    }
                    const double t = c.lapMs[static_cast<size_t>(n)];
                    const char *cls = t == r.fastest ? "fast" : (n == c.bestLap ? "best" : "");
                    f << "<td class=" << cls << ">" << lapText(t) << "</td>";
                }
                f << "</tr>";
            }
            f << "</table><h2>Sectors (the lap in thirds)</h2><table><tr><th class=l>Driver</th><th>Best S1</th><th>Best S2</th><th>Best S3</th><th>Ideal lap</th><th>Best lap</th><th>Lost</th></tr>";
            for (const CarReport &c : r.cars)
            {
                const double best = c.bestLap >= 0 ? c.lapMs[static_cast<size_t>(c.bestLap)] : -1.0;
                f << "<tr" << (c.player ? " class=you" : "") << "><td class=l>" << esc(c.driver) << " <span class=muted>" << esc(c.car) << "</span></td>";
                for (int j = 0; j < 3; ++j)
                    f << "<td class=" << (c.bestSec[j] >= 0.0 && c.bestSec[j] == r.bestSecAll[j] ? "fast" : "") << ">" << secText(c.bestSec[j]) << "</td>";
                f << "<td>" << lapText(c.ideal) << "</td><td>" << lapText(best) << "</td><td>"
                  << (c.ideal >= 0.0 && best >= 0.0 ? gapText(best - c.ideal) : "-") << "</td></tr>";
            }
            f << "</table><h2>Positions</h2>";
            // An SVG chart: places by lap.
            const int n = static_cast<int>(r.cars.size()), laps = std::max(1, r.lapsShown);
            const int W = 760, H = 60 + 40 * n;
            f << "<svg width=" << W << " height=" << H << " style=\"background:#1b1e25\">";
            for (int p = 1; p <= n; ++p)
                f << "<text x=8 y=" << 30 + 40 * (p - 1) + 5 << " fill=#9aa0aa font-size=14>P" << p << "</text>";
            for (int l = 0; l <= laps; ++l)
                f << "<text x=" << 50 + l * (W - 220) / laps - 6 << " y=" << H - 8 << " fill=#9aa0aa font-size=12>" << (l == 0 ? "grid" : std::to_string(l))
                  << "</text>";
            for (const CarReport &c : r.cars)
            {
                f << "<polyline fill=none stroke-width=" << (c.player ? 4 : 2) << " stroke=\"" << carColourHex(c.index) << "\" points=\"";
                f << 50 << "," << 30 + 40 * (std::max(1, c.grid) - 1) << " ";
                for (int l = 0; l < c.laps; ++l)
                    f << 50 + (l + 1) * (W - 220) / laps << "," << 30 + 40 * (std::max(1, c.lapPos[static_cast<size_t>(l)]) - 1) << " ";
                f << "\"/><text x=" << W - 160 << " y=" << 30 + 40 * (c.pos - 1) + 5 << " fill=\"" << carColourHex(c.index) << "\" font-size=13>"
                  << esc(c.name) << "</text>";
            }
            f << "</svg></body></html>\n";
            std::fprintf(stderr, "[race-report] saved %s\n", path.c_str());
        }

        // ---- drawing ----
        Font &font()
        {
            static Font f = [] {
                const char *path = "C:\\Windows\\Fonts\\segoeuib.ttf";
                if (FileExists(path))
                {
                    Font loaded = LoadFontEx(path, 64, nullptr, 0);
                    GenTextureMipmaps(&loaded.texture);
                    SetTextureFilter(loaded.texture, TEXTURE_FILTER_TRILINEAR);
                    return loaded;
                }
                return GetFontDefault();
            }();
            return f;
        }
        const Color kText{232, 234, 238, 255}, kMuted{154, 160, 170, 255}, kBest{86, 200, 128, 255}, kFast{197, 139, 255, 255},
            kAccent{62, 126, 255, 255}, kRow{34, 40, 58, 255};
        // align: 0 left, 1 right, 0.5 centred.
        void text(const std::string &s, float x, float y, float size, Color c, float align = 0.0f)
        {
            const Vector2 m = MeasureTextEx(font(), s.c_str(), size, 1.0f);
            DrawTextEx(font(), s.c_str(), Vector2{std::round(x - m.x * align), std::round(y)}, size, 1.0f, c);
        }
        std::string fit(std::string s, float size, float width)
        {
            while (s.size() > 3 && MeasureTextEx(font(), s.c_str(), size, 1.0f).x > width)
                s.pop_back();
            return s;
        }

        const char *const kPages[5] = {"RESULTS", "LAP TIMES", "SECTORS", "POSITIONS", "YOUR RACE"};

        // A row's place, the car's colour and its name (as on Results), within `width`.
        void placeAndName(const CarReport &c, float x, float y, float fs, float width, float size)
        {
            text(std::to_string(c.pos), x, y, size, kText);
            DrawRectangleRec(Rectangle{x + fs * 1.3f, y + fs * 0.1f, fs * 0.25f, fs * 0.9f}, carColour(c.index));
            text(fit(c.name, size, width - fs * 2.0f), x + fs * 1.8f, y, size, kText);
        }
        const char *nameHeading() { return g_showDrivers ? "DRIVER" : "CAR"; }

        void pageResults(const Report &r, Rectangle a, float fs)
        {
            // (right-aligned columns end just before the next one starts; top speed gets room for "177 km/h")
            const float cols[9] = {0.00f, 0.05f, 0.33f, 0.42f, 0.53f, 0.64f, 0.75f, 0.87f, 0.965f};
            const char *heads[9] = {"P", nameHeading(), "LAPS", "TIME", "GAP", "BEST LAP", "TOP SPEED", "GRID", "LED"};
            const float right[9] = {0, 0, 1, 1, 1, 1, 1, 1, 1};
            const float rowH = fs * 1.7f;
            float y = a.y;
            for (int i = 0; i < 9; ++i)
                text(heads[i], a.x + a.width * (right[i] ? (i + 1 < 9 ? cols[i + 1] - 0.01f : 1.0f) : cols[i]), y, fs * 0.8f, kMuted, right[i]);
            y += rowH;
            const CarReport *winner = r.cars.empty() ? nullptr : &r.cars.front();
            for (const CarReport &c : r.cars)
            {
                if (c.player)
                    DrawRectangleRec(Rectangle{a.x - fs * 0.3f, y - fs * 0.2f, a.width + fs * 0.6f, rowH}, kRow);
                const double best = c.bestLap >= 0 ? c.lapMs[static_cast<size_t>(c.bestLap)] : -1.0;
                std::string gap;
                if (winner && c.finished && winner->time >= 0.0 && c.time >= 0.0)
                    gap = &c == winner ? "" : gapText(c.time - winner->time);
                else if (!c.finished && r.totalLaps > 0)
                    gap = "+" + std::to_string(std::max(0, r.totalLaps - c.laps)) + (r.totalLaps - c.laps == 1 ? " lap" : " laps");
                const int moved = c.grid > 0 ? c.grid - c.pos : 0;
                const std::string cells[9] = {std::to_string(c.pos),
                                              c.name,
                                              std::to_string(c.laps),
                                              c.finished ? lapText(c.time) : "-",
                                              gap,
                                              lapText(best),
                                              speedText(c.top),
                                              c.grid > 0 ? std::to_string(c.grid) + (moved > 0 ? "  +" + std::to_string(moved) : moved < 0 ? "  " + std::to_string(moved) : "")
                                                         : "-",
                                              std::to_string(c.led)};
                for (int i = 0; i < 9; ++i)
                {
                    Color col = kText;
                    if (i == 5 && c.index == r.fastestCar)
                        col = kFast;
                    if (i == 7 && moved != 0)
                        col = moved > 0 ? kBest : Color{255, 110, 100, 255};
                    if (i == 1)
                    {
                        DrawRectangleRec(Rectangle{a.x + a.width * cols[1] - fs * 0.5f, y + fs * 0.1f, fs * 0.25f, fs * 0.9f}, carColour(c.index));
                        text(fit(cells[i], fs, a.width * (cols[2] - cols[1] - 0.02f)), a.x + a.width * cols[1], y, fs, col);
                        continue;
                    }
                    const float xr = right[i] ? (i + 1 < 9 ? cols[i + 1] - 0.01f : 1.0f) : cols[i];
                    text(cells[i], a.x + a.width * xr, y, fs, col, right[i]);
                }
                y += rowH;
            }
            if (r.fastestCar >= 0)
            {
                y += fs * 0.6f;
                for (const CarReport &c : r.cars)
                    if (c.index == r.fastestCar)
                        text("Fastest lap: " + c.name + "  " + lapText(r.fastest) + "  (lap " + std::to_string(c.bestLap + 1) + ")", a.x, y, fs * 0.9f, kFast);
            }
        }

        void pageLaps(const Report &r, Rectangle a, float fs)
        {
            const int maxCols = 12;
            const int first = std::max(0, r.lapsShown - maxCols);
            const int cols = r.lapsShown - first;
            // The names' column: wider when there are few laps to show.
            const float nameW = a.width * (cols <= 6 ? 0.34f : 0.24f), colW = cols > 0 ? (a.width - nameW) / static_cast<float>(cols) : 0.0f;
            const float size = std::min(fs, colW / 4.6f);
            const float rowH = fs * 1.7f;
            float y = a.y;
            text("P", a.x, y, fs * 0.8f, kMuted);
            text(nameHeading(), a.x + fs * 1.8f, y, fs * 0.8f, kMuted);
            for (int n = first; n < r.lapsShown; ++n)
                text("L" + std::to_string(n + 1), a.x + nameW + colW * (n - first + 1) - fs * 0.3f, y, fs * 0.8f, kMuted, 1.0f);
            y += rowH;
            for (const CarReport &c : r.cars)
            {
                if (c.player)
                    DrawRectangleRec(Rectangle{a.x - fs * 0.3f, y - fs * 0.2f, a.width + fs * 0.6f, rowH}, kRow);
                placeAndName(c, a.x, y, fs, nameW - fs * 0.5f, fs * 0.85f);
                for (int n = first; n < c.laps; ++n)
                {
                    const double t = c.lapMs[static_cast<size_t>(n)];
                    const Color col = t == r.fastest ? kFast : (n == c.bestLap ? kBest : kText);
                    text(lapText(t), a.x + nameW + colW * (n - first + 1) - fs * 0.3f, y + (fs - size) * 0.5f, size, col, 1.0f);
                }
                y += rowH;
            }
            y += fs * 0.5f;
            text(first > 0 ? "The last 12 laps. Green: the driver's best lap; purple: the fastest lap of the race."
                           : "Green: the driver's best lap; purple: the fastest lap of the race.",
                 a.x, y, fs * 0.75f, kMuted);
        }

        void pageSectors(const Report &r, Rectangle a, float fs)
        {
            const float cols[7] = {0.0f, 0.36f, 0.47f, 0.58f, 0.71f, 0.84f, 0.96f};
            const char *heads[7] = {nameHeading(), "BEST S1", "BEST S2", "BEST S3", "IDEAL LAP", "BEST LAP", "LOST"};
            const float rowH = fs * 1.7f;
            float y = a.y;
            text("P", a.x, y, fs * 0.8f, kMuted);
            for (int i = 0; i < 7; ++i)
                text(heads[i], a.x + (i ? a.width * cols[i] : fs * 1.8f), y, fs * 0.8f, kMuted, i ? 1.0f : 0.0f);
            y += rowH;
            for (const CarReport &c : r.cars)
            {
                if (c.player)
                    DrawRectangleRec(Rectangle{a.x - fs * 0.3f, y - fs * 0.2f, a.width + fs * 0.6f, rowH}, kRow);
                const double best = c.bestLap >= 0 ? c.lapMs[static_cast<size_t>(c.bestLap)] : -1.0;
                placeAndName(c, a.x, y, fs, a.width * 0.27f, fs);
                for (int j = 0; j < 3; ++j)
                    text(secText(c.bestSec[j]), a.x + a.width * cols[j + 1], y, fs, c.bestSec[j] >= 0.0 && c.bestSec[j] == r.bestSecAll[j] ? kFast : kText, 1.0f);
                text(lapText(c.ideal), a.x + a.width * cols[4], y, fs, kText, 1.0f);
                text(lapText(best), a.x + a.width * cols[5], y, fs, c.index == r.fastestCar ? kFast : kText, 1.0f);
                text(c.ideal >= 0.0 && best >= 0.0 ? gapText(best - c.ideal) : "-", a.x + a.width * cols[6], y, fs, kMuted, 1.0f);
                y += rowH;
            }
            y += fs * 0.5f;
            text("Sectors: the lap in thirds by distance. Ideal lap: the best three sectors added up; lost: best lap minus ideal.", a.x, y,
                 fs * 0.75f, kMuted);
            y += fs * 1.2f;
            text("Purple: the fastest in the race.", a.x, y, fs * 0.75f, kMuted);
        }

        void pagePositions(const Report &r, Rectangle a, float fs)
        {
            const int n = static_cast<int>(r.cars.size()), laps = std::max(1, r.lapsShown);
            if (n == 0)
                return;
            const Rectangle g{a.x + fs * 2.0f, a.y + fs * 0.5f, a.width * 0.68f, a.height - fs * 3.0f};
            const float stepY = n > 1 ? g.height / static_cast<float>(n - 1) : 0.0f;
            for (int p = 1; p <= n; ++p)
            {
                const float y = g.y + stepY * (p - 1);
                DrawLineEx(Vector2{g.x, y}, Vector2{g.x + g.width, y}, 1.0f, Color{50, 55, 66, 255});
                text("P" + std::to_string(p), g.x - fs * 0.4f, y - fs * 0.5f, fs * 0.85f, kMuted, 1.0f);
            }
            for (int l = 0; l <= laps; ++l)
            {
                const float x = g.x + g.width * l / laps;
                if (laps <= 20 || l % 5 == 0 || l == laps)
                    text(l == 0 ? "grid" : std::to_string(l), x, g.y + g.height + fs * 0.5f, fs * 0.75f, kMuted, 0.5f);
            }
            for (const CarReport &c : r.cars)
            {
                const Color col = carColour(c.index);
                Vector2 prev{g.x, g.y + stepY * (std::max(1, c.grid) - 1)};
                for (int l = 0; l < c.laps; ++l)
                {
                    const Vector2 p{g.x + g.width * (l + 1) / laps, g.y + stepY * (std::clamp(c.lapPos[static_cast<size_t>(l)], 1, n) - 1)};
                    DrawLineEx(prev, p, c.player ? 5.0f : 2.5f, col);
                    DrawCircleV(p, c.player ? 5.0f : 3.0f, col);
                    prev = p;
                }
                const float ly = g.y + stepY * (c.pos - 1) - fs * 0.5f;
                text(fit(std::to_string(c.pos) + "  " + c.name, fs * 0.85f, a.width * 0.26f), g.x + g.width + fs * 1.2f, ly, fs * 0.85f, col);
            }
        }

        void pageYou(const Report &r, Rectangle a, float fs)
        {
            const CarReport *you = nullptr;
            for (const CarReport &c : r.cars)
                if (c.player)
                    you = &c;
            if (!you)
                return;
            const double best = you->bestLap >= 0 ? you->lapMs[static_cast<size_t>(you->bestLap)] : -1.0;
            const CarReport *winner = &r.cars.front();
            float y = a.y;
            const float col2 = a.x + a.width * 0.30f;
            auto line = [&](const std::string &k, const std::string &v, Color c = kText) {
                text(k, a.x, y, fs * 0.9f, kMuted);
                text(v, col2, y, fs * 0.9f, c);
                y += fs * 1.45f;
            };
            const int moved = you->grid > 0 ? you->grid - you->pos : 0;
            line("Finished", "P" + std::to_string(you->pos) + " of " + std::to_string(r.cars.size()) +
                                 (you->grid > 0 ? "   (from P" + std::to_string(you->grid) + (moved > 0 ? ", +" + std::to_string(moved) : moved < 0 ? ", " + std::to_string(moved) : "") + ")" : ""));
            if (winner != you && winner->finished && you->finished && winner->time >= 0.0 && you->time >= 0.0)
                line("Behind the winner", gapText(you->time - winner->time) + " s");
            line("Best lap", lapText(best) + (you->bestLap >= 0 ? "   (lap " + std::to_string(you->bestLap + 1) + ")" : ""), you->index == r.fastestCar ? kFast : kBest);
            if (r.fastest >= 0.0 && best >= 0.0 && you->index != r.fastestCar)
                line("Off the fastest lap", gapText(best - r.fastest) + " s");
            if (you->avg > 0.0)
                line("Average lap", lapText(you->avg) + "   (spread " + secText(you->sd) + " s)");
            line("Ideal lap (your best sectors)", lapText(you->ideal));
            line("Top speed", speedText(you->top));
            if (you->led > 0)
                line("Laps led", std::to_string(you->led));
            // Your best sectors against the race's best.
            y += fs * 0.3f;
            for (int j = 0; j < 3; ++j)
            {
                const double d = you->bestSec[j] >= 0.0 && r.bestSecAll[j] >= 0.0 ? you->bestSec[j] - r.bestSecAll[j] : -1.0;
                line("Sector " + std::to_string(j + 1) + " vs the best", d < 0.0 ? "-" : (d < 0.5 ? "fastest" : gapText(d) + " s"), d >= 0.0 && d < 0.5 ? kFast : kText);
            }
            // Your laps as bars.
            const Rectangle g{a.x + a.width * 0.62f, a.y + fs * 0.4f, a.width * 0.38f, a.height - fs * 2.5f};
            if (you->laps > 0)
            {
                uint32_t lo = UINT32_MAX, hi = 0;
                for (uint32_t t : you->lapMs)
                {
                    lo = std::min(lo, t);
                    hi = std::max(hi, t);
                }
                const double floor = lo * 0.97, span = std::max(1.0, hi - floor);
                const float barH = std::min(fs * 1.4f, g.height / you->laps);
                for (int i = 0; i < you->laps; ++i)
                {
                    const double t = you->lapMs[static_cast<size_t>(i)];
                    const float w = static_cast<float>((t - floor) / span) * (g.width - fs * 6.0f);
                    const float yy = g.y + barH * i;
                    const Color col = t == r.fastest ? kFast : (i == you->bestLap ? kBest : kAccent);
                    text("L" + std::to_string(i + 1), g.x, yy, barH * 0.7f, kMuted);
                    DrawRectangleRec(Rectangle{g.x + fs * 1.8f, yy + barH * 0.12f, std::max(2.0f, w), barH * 0.7f}, col);
                    text(lapText(t), g.x + fs * 2.0f + w, yy, barH * 0.7f, kText);
                }
            }
        }
    }

    std::string driverName(int carIndex)
    {
        loadNames();
        if (carIndex <= 0)
            return g_playerName;
        return g_driverNames.empty() ? "Driver " + std::to_string(carIndex + 1)
                                     : g_driverNames[static_cast<size_t>(carIndex - 1) % g_driverNames.size()];
    }

    void frame(const uint8_t *rdram)
    {
        loadNames();
        static const bool init = [] {
            const char *v = std::getenv("GT3_RACE_REPORT");
            g_on = v && *v == '1';
            const char *d = std::getenv("GT3_RACE_REPORT_DIR");
            g_dir = d ? d : "";
            const char *u = std::getenv("GT3_UNITS_SPEED");
            g_mph = !(u && std::strcmp(u, "kmh") == 0);
            return true;
        }();
        (void)init;
        if (!g_on || !rdram)
            return;
        g_rdram = rdram;
        gt3race::Snapshot s;
        if (!gt3race::read(rdram, s, false) || s.replay || s.cars.empty())
            return;
        RaceLog &g = g_log;
        const bool newRace = s.race != g.race || s.clockMs + 2000u < g.lastClock || g.cars.size() != s.cars.size();
        if (newRace)
        {
            g = RaceLog{};
            g.race = s.race;
            g.cars.resize(s.cars.size());
            {
                std::lock_guard<std::mutex> lock(g_namesMutex);
                g_names.clear();
            }
            fetchNames();
            g_visible = false;
            g_autoShown = false;
            g_saved = false;
            g_reportFromFinish = false;
        }
        if (s.totalLaps > 0)
            g.totalLaps = s.totalLaps;
        if (s.clockMs == g.lastClock && !newRace)
            return;
        g.lastClock = s.clockMs;
        for (const gt3race::Car &c : s.cars)
        {
            if (c.index < 0 || static_cast<size_t>(c.index) >= g.cars.size())
                continue;
            CarLog &cl = g.cars[static_cast<size_t>(c.index)];
            g.lapLength = std::max(g.lapLength, c.progress);
            if (cl.trace.empty())
                cl.grid = c.position;
            if (s.clockMs == 0u)
                cl.grid = c.position; // on the grid, until the start
            cl.trace.push_back({s.clockMs, c.lap, c.progress, c.position});
            cl.curTop = std::max(cl.curTop, c.speed);
            cl.top = std::max(cl.top, c.speed);
            // A lap done: GT3's lap time when it changes (a frame or so after the line).
            if (c.lastLapMs != cl.prevLast && c.lastLapMs != 0u && c.lastLapMs < 3600000u && c.lap >= 2)
            {
                cl.lapMs.push_back(c.lastLapMs);
                cl.lapTop.push_back(cl.curTop);
                cl.curTop = 0.0f;
            }
            cl.prevLast = c.lastLapMs;
            cl.prevLap = c.lap;
        }
        // The player's car finished: the report, shown 4 s later (and saved once the names are in).
        if (!g.playerDone && g.totalLaps > 0 && !g.cars.empty() && static_cast<int>(g.cars[0].lapMs.size()) >= g.totalLaps)
        {
            g.playerDone = true;
            g.doneAt = GetTime();
            fetchNames();
            std::fprintf(stderr, "[race-report] the player finished (%d laps) at race clock %u ms\n", g.totalLaps, s.clockMs);
        }
        if (g.playerDone && !g_autoShown && GetTime() - g.doneAt > 4.0)
        {
            g_autoShown = true;
            g_report = build();
            g_reportFromFinish = true;
            g_visible = g_report.valid;
            g_page = 0;
        }
        // Saved once: every car finished, or a minute after the player.
        if (g.playerDone && !g_saved)
        {
            bool all = true;
            for (const CarLog &cl : g.cars)
                all &= static_cast<int>(cl.lapMs.size()) >= g.totalLaps;
            if (all || GetTime() - g.doneAt > 60.0)
            {
                g_saved = true;
                save(build());
            }
        }
    }

    void draw(float x, float y, float w, float h)
    {
        if (!g_on)
            return;
        // F2: show / next page (the last race's report, or this race's so far); Shift+F2: close.
        if (IsKeyPressed(KEY_F2))
        {
            const bool shift = IsKeyDown(KEY_LEFT_SHIFT) || IsKeyDown(KEY_RIGHT_SHIFT);
            const bool ctrl = IsKeyDown(KEY_LEFT_CONTROL) || IsKeyDown(KEY_RIGHT_CONTROL);
            if (ctrl && g_visible)
                g_showDrivers = !g_showDrivers; // Ctrl+F2: drivers' / cars' names
            else if (shift)
                g_visible = false;
            else if (!g_visible)
            {
                if (!g_log.playerDone || !g_reportFromFinish)
                {
                    const Report live = build();
                    if (live.valid)
                    {
                        g_report = live;
                        g_reportFromFinish = g_log.playerDone;
                    }
                }
                g_visible = g_report.valid;
                g_page = 0;
            }
            else if (++g_page >= 5)
                g_visible = false;
        }
        if (!g_visible || !g_report.valid)
            return;
        // GT3_RACE_REPORT_CYCLE=1 (tests, no key presses): the pages turn every 3 s.
        static const bool cycle = [] {
            const char *v = std::getenv("GT3_RACE_REPORT_CYCLE");
            return v && *v == '1';
        }();
        if (cycle)
            g_page = static_cast<int>(GetTime() / 3.0) % 5;
        // Rebuilt once a second while shown and the race still runs (the AI cars finishing after the player).
        if (GetTime() - g_lastBuild > 1.0 && (!g_reportFromFinish || !g_saved))
        {
            g_lastBuild = GetTime();
            const Report live = build();
            if (live.valid)
                g_report = live;
        }
        if (g_report.cars.empty())
            return;
        {
            std::lock_guard<std::mutex> lock(g_namesMutex);
            for (CarReport &c : g_report.cars)
                if (static_cast<size_t>(c.index) < g_names.size() && !g_names[static_cast<size_t>(c.index)].empty())
                    c.car = g_names[static_cast<size_t>(c.index)];
        }
        setNames(g_report);
        const float pw = std::min(w * 0.94f, h * 1.62f), ph = h * 0.88f;
        const Rectangle panel{x + (w - pw) * 0.5f, y + (h - ph) * 0.5f, pw, ph};
        DrawRectangleRounded(panel, 0.03f, 8, Color{14, 16, 21, 236});
        const float fs = ph / 30.0f;
        // Title and tabs.
        text("RACE REPORT", panel.x + fs * 1.2f, panel.y + fs * 0.8f, fs * 1.3f, kText);
        char sub[96];
        std::snprintf(sub, sizeof(sub), "%s%d laps  -  lap %.2f km", g_reportFromFinish ? "" : "so far  -  ", g_report.totalLaps,
                      g_report.lapLength / 1000.0f);
        text(sub, panel.x + fs * 1.2f, panel.y + fs * 2.4f, fs * 0.75f, kMuted);
        float tx = panel.x + panel.width * 0.36f;
        for (int i = 0; i < 5; ++i)
        {
            const Vector2 m = MeasureTextEx(font(), kPages[i], fs * 0.8f, 1.0f);
            const Rectangle tab{tx, panel.y + fs * 0.9f, m.x + fs * 1.2f, fs * 1.6f};
            if (i == g_page)
                DrawRectangleRounded(tab, 0.3f, 6, kAccent);
            if (CheckCollisionPointRec(GetMousePosition(), tab) && IsMouseButtonPressed(MOUSE_BUTTON_LEFT))
                g_page = i;
            text(kPages[i], tab.x + fs * 0.6f, tab.y + fs * 0.35f, fs * 0.8f, i == g_page ? WHITE : kMuted);
            tx += tab.width + fs * 0.4f;
        }
        const Rectangle area{panel.x + fs * 1.2f, panel.y + fs * 4.4f, panel.width - fs * 2.4f, panel.height - fs * 6.4f};
        switch (g_page)
        {
        case 0: pageResults(g_report, area, fs); break;
        case 1: pageLaps(g_report, area, fs); break;
        case 2: pageSectors(g_report, area, fs); break;
        case 3: pagePositions(g_report, area, fs); break;
        default: pageYou(g_report, area, fs); break;
        }
        text("F2: next page    Shift+F2: close    Ctrl+F2: drivers / cars    or click", panel.x + fs * 1.2f, panel.y + panel.height - fs * 1.6f,
             fs * 0.7f, kMuted);
        // The names switch, bottom right: DRIVERS | CARS.
        {
            const char *opts[2] = {"DRIVERS", "CARS"};
            float bx = panel.x + panel.width - fs * 1.2f;
            for (int i = 1; i >= 0; --i)
            {
                const Vector2 m = MeasureTextEx(font(), opts[i], fs * 0.7f, 1.0f);
                const Rectangle b{bx - m.x - fs * 1.0f, panel.y + panel.height - fs * 2.0f, m.x + fs * 1.0f, fs * 1.3f};
                const bool on = (i == 0) == g_showDrivers;
                DrawRectangleRounded(b, 0.3f, 6, on ? kAccent : Color{34, 38, 48, 255});
                if (CheckCollisionPointRec(GetMousePosition(), b) && IsMouseButtonPressed(MOUSE_BUTTON_LEFT))
                    g_showDrivers = i == 0;
                text(opts[i], b.x + fs * 0.5f, b.y + fs * 0.28f, fs * 0.7f, on ? WHITE : kMuted);
                bx = b.x - fs * 0.3f;
            }
            const Vector2 m = MeasureTextEx(font(), "NAMES", fs * 0.7f, 1.0f);
            text("NAMES", bx - m.x - fs * 0.3f, panel.y + panel.height - fs * 1.72f, fs * 0.7f, kMuted);
        }
    }
}
