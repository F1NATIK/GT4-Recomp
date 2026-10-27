// GT4 Recompilation launcher, Full edition: the Race Lab tab (developer, 20 Oct 2026: "can race lab just have a launcher tab as well,
// instead of opening in the browser"; analysis/design/racelab-tab/REPORT.md). The launcher is raylib with no web view, so the
// tab draws the Race Lab's main views itself from the same files the Race Lab page and GT4 use (no server needed):
//   AI drivers: AI cars 1-5 given drivers from the Race Lab's roster (racelab-data\settings\drivers.json, the page's
//     "settings/drivers"; its eight default drivers when there is none), Send to GT4 = racelab-data\ai-lines\gt4-drivers.txt,
//     which GT4 reads at each race (GT4_AI_DRIVERS, gt4_ai_drivers.h): the timing tower's Driver names, the pages' feed and the
//     recorder's race files carry them. GT4's own AI still drives every car. Drivers renamed, added and removed here too.
//   Laps: the laps the recorder wrote (racelab-laps, GT4_DRIVE_LOG), each against the best lap on that course (speed and the
//     time gained or lost along the lap).
//   Races: the recorder's whole-race files, each car's driver and car (<...>-race-cars.txt), place, best lap and time.
//   Settings: recording, the drivers' names on or off (GT4_AI_DRIVERS), and the Race Lab page in the browser (gt4RaceLab.exe),
//     kept as the secondary way in ("Open in browser" also sits beside the tabs).

#include "gt4stock_app.h"

#include "gt4full_racelab.h"
#include "gt4full_telelayout.h" // (its small JSON reader)
#include "gt4full_ui.h"
#include "gt4stock_system.h"
#include "gt4stock_util.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <ctime>
#include <fstream>
#include <process.h> // _getpid (the Race Lab server quits with this program)
#include <sstream>
#include <type_traits>

namespace gt4stock
{
    using ui::Align;
    using ui::Style;
    using ui::Weight;
    using fullui::Pill;

    namespace
    {
        constexpr float kSidebarW = 232.0f;
        constexpr float kTop = 104.0f;
        constexpr float kGap = 16.0f;
        Rectangle R(float x, float y, float w, float h) { return Rectangle{x, y, w, h}; }
        const char *const kDot = " \xc2\xb7 ";
        constexpr int kCars = 5; // the Race Lab's AI cars 1-5

        struct Driver
        {
            std::string id, name, style;
            double pace = 1.0;
            int speedPct = 0, brakeM = 0, mistakes = 0, aggression = 1, avoid = 1;
        };
        // The Race Lab page's eight (DEFAULT_DRIVERS in its index.html).
        std::vector<Driver> defaultDrivers()
        {
            return {
                {"d1", "Kenji Arata", "The metronome: the same lap, every lap", 1.0, 1, 2, 0, 1, 2},
                {"d2", "Sofia Lindqvist", "Late braker: dives down the inside", 1.0, 2, 10, 1, 2, 1},
                {"d3", "Dale Brennan", "Charger: quick, rough, leans on people", 1.01, 3, 5, 2, 2, 0},
                {"d4", "Am\xc3\xa9lie Duret", "Smooth and tidy: hard to pass, never touches anyone", 0.99, 1, 2, 0, 0, 2},
                {"d5", "Tom\xc3\xa1s Varga", "Wild card: brilliant one lap, in the gravel the next", 1.0, 8, 15, 3, 2, 0},
                {"d6", "Rhys Calder", "Rookie: a little slow, nervous in traffic", 0.96, 5, 10, 3, 0, 2},
                {"d7", "Haruto Moriyama", "Veteran: steady, picks his moment", 0.99, 2, 5, 1, 1, 2},
                {"d8", "Nadia Okafor", "Opportunist: quick, pounces on mistakes", 1.0, 3, 5, 1, 2, 1},
            };
        }

        struct LapFile
        {
            fs::path path;
            std::string stamp; // yyyymmdd-hhmmss (the race's start)
            uint32_t course = 0;
            std::string car, start;
            int lap = 0;
            uint32_t ms = 0; // GT4's lap time (0: cut short)
        };
        struct Trace
        {
            std::vector<float> dist, speed, t; // from the line: m, km/h, s
        };
        struct RaceCar
        {
            int index = 0, place = 0, lapsDone = 0;
            bool player = false, finished = false;
            std::string who, driver, car;
            uint32_t best = 0, total = 0;
        };
        struct Race
        {
            fs::path csv;
            std::string stamp;
            uint32_t course = 0;
            int laps = 0;
            bool sidecar = false; // its -race-cars.txt was there (drivers known)
            std::vector<RaceCar> cars;
        };

        struct Tab
        {
            int tab = 0;
            bool init = false;
            double scannedAt = -100.0;
            std::vector<Driver> roster;
            bool rosterFile = false;
            std::string picks[kCars + 1]; // the drivers' names given to cars 1-5 ("" none)
            std::string picksFrom;        // where they came from, said under the cars
            std::string has[kCars + 1];   // what gt4-drivers.txt holds now
            bool hasFile = false;
            std::string lastCars[kCars + 1]; // the car each AI car drove in the newest recorded race
            std::string note;
            bool noteOk = true;
            int editing = -1; // the roster row being renamed
            std::string editText;
            std::vector<LapFile> laps;
            int lapSel = 0;
            std::string traceFor;
            Trace trA, trB;
            int cmpIndex = -1; // the lap compared with (laps index)
            std::vector<Race> races;
            int raceSel = 0;
            bool testRename = false;
        };
        Tab g_tab;

        std::string lapText(uint32_t ms)
        {
            if (!ms)
                return "-";
            return strf("%u:%02u.%03u", ms / 60000u, (ms / 1000u) % 60u, ms % 1000u);
        }
        std::string courseName(uint32_t c)
        {
            switch (c)
            {
            case 81: return "Autumn Ring Mini";
            case 7: return "New York";
            default: return strf("Course %u", c);
            }
        }
        // "20261019-221530" -> "19 Oct 22:15"
        std::string stampText(const std::string &s)
        {
            static const char *const mon[12] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
            if (s.size() < 13)
                return s;
            const int m = std::atoi(s.substr(4, 2).c_str());
            return strf("%d %s %s:%s", std::atoi(s.substr(6, 2).c_str()), m >= 1 && m <= 12 ? mon[m - 1] : "?", s.substr(9, 2).c_str(), s.substr(11, 2).c_str());
        }
        std::string initials(const std::string &n)
        {
            std::string out;
            bool start = true;
            for (const char c : n)
            {
                if (c == ' ')
                    start = true;
                else if (start && out.size() < 2 && static_cast<unsigned char>(c) < 0x80)
                    out += static_cast<char>(std::toupper(static_cast<unsigned char>(c))), start = false;
                else
                    start = false;
            }
            return out.empty() ? std::string("?") : out;
        }
        std::string jsonText(const std::string &s)
        {
            std::string o = "\"";
            for (const char c : s)
            {
                if (c == '"' || c == '\\')
                    o += '\\', o += c;
                else if (static_cast<unsigned char>(c) < 0x20)
                    o += ' ';
                else
                    o += c;
            }
            return o + "\"";
        }
        std::string isoNow()
        {
            const std::time_t t = std::time(nullptr);
            std::tm tm{};
            gmtime_s(&tm, &t);
            char b[32];
            std::strftime(b, sizeof(b), "%Y-%m-%dT%H:%M:%S.000Z", &tm);
            return b;
        }
        std::vector<std::string> lines(const std::string &text)
        {
            std::vector<std::string> out;
            std::istringstream in(text);
            for (std::string l; std::getline(in, l);)
            {
                if (!l.empty() && l.back() == '\r')
                    l.pop_back();
                out.push_back(l);
            }
            return out;
        }

        // ---- the files
        bool readRoster(const fs::path &file, std::vector<Driver> &out)
        {
            std::string text;
            if (!readText(file, text))
                return false;
            tele::json::Reader r{text.data(), text.data() + text.size()};
            const tele::json::Value v = r.value();
            const tele::json::Value *list = r.ok ? v.get("drivers") : nullptr;
            if (!list || list->type != tele::json::Value::Array || list->a.empty())
                return false;
            out.clear();
            for (const tele::json::Value &d : list->a)
            {
                Driver x;
                auto str = [&](const char *k, std::string &s) { if (const auto *p = d.get(k); p && p->type == tele::json::Value::String) s = p->s; };
                auto num = [&](const char *k, auto &n) { if (const auto *p = d.get(k); p && p->type == tele::json::Value::Number) n = static_cast<std::remove_reference_t<decltype(n)>>(p->n); };
                str("id", x.id), str("name", x.name), str("style", x.style);
                num("pace", x.pace), num("speedPct", x.speedPct), num("brakeM", x.brakeM), num("mistakes", x.mistakes), num("aggression", x.aggression),
                    num("avoid", x.avoid);
                if (x.id.empty())
                    x.id = strf("d%zu", out.size() + 1);
                out.push_back(x);
            }
            return true;
        }
        std::string rosterJson(const std::vector<Driver> &roster)
        {
            std::string o = "{\"drivers\":[";
            for (size_t i = 0; i < roster.size(); ++i)
            {
                const Driver &d = roster[i];
                o += (i ? "," : "") + std::string("{\"id\":") + jsonText(d.id) + ",\"name\":" + jsonText(d.name) + ",\"style\":" + jsonText(d.style) +
                     ",\"pace\":" + numberText(d.pace, 2) + strf(",\"speedPct\":%d,\"brakeM\":%d,\"mistakes\":%d,\"aggression\":%d,\"avoid\":%d}", d.speedPct,
                                                                 d.brakeM, d.mistakes, d.aggression, d.avoid);
            }
            return o + "],\"savedAt\":" + jsonText(isoNow()) + "}";
        }
        // gt4-drivers.txt: "car <k> <name>" lines (gt4_ai_drivers.h)
        bool readGt4Drivers(const fs::path &file, std::string (&names)[kCars + 1])
        {
            for (std::string &n : names)
                n.clear();
            std::string text;
            if (!readText(file, text))
                return false;
            const std::vector<std::string> ls = lines(text);
            if (ls.empty() || !startsWith(ls[0], "gt4-ai-drivers "))
                return false;
            for (const std::string &l : ls)
                if (startsWith(l, "car "))
                {
                    const size_t sp = l.find(' ', 4);
                    const int k = std::atoi(l.substr(4).c_str());
                    if (k >= 1 && k <= kCars && sp != std::string::npos)
                        names[k] = trim(l.substr(sp + 1));
                }
            return true;
        }
        // The newest of the Race Lab page's AI cars' documents (racelab-data\ai\<track>.json, its Send to AI cars): each car's
        // driverName, and the track's name.
        bool readNewestAiDoc(const fs::path &dir, std::string (&names)[kCars + 1], std::string &track)
        {
            fs::path newest;
            fs::file_time_type when{};
            std::error_code ec;
            for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec))
                if (it->is_regular_file(ec) && it->path().extension() == L".json" && (newest.empty() || it->last_write_time(ec) > when))
                    newest = it->path(), when = it->last_write_time(ec);
            std::string text;
            if (newest.empty() || !readText(newest, text))
                return false;
            tele::json::Reader r{text.data(), text.data() + text.size()};
            const tele::json::Value v = r.value();
            const tele::json::Value *cars = r.ok ? v.get("cars") : nullptr;
            if (!cars || cars->type != tele::json::Value::Object)
                return false;
            bool any = false;
            for (int k = 1; k <= kCars; ++k)
                if (const tele::json::Value *c = cars->get(std::to_string(k).c_str()))
                    if (const tele::json::Value *n = c->get("driverName"); n && n->type == tele::json::Value::String && !n->s.empty())
                        names[k] = n->s, any = true;
            if (const tele::json::Value *n = v.get("name"); n && n->type == tele::json::Value::String)
                track = n->s;
            return any;
        }

        // A lap file's "# key value" lines (the first 16 lines).
        bool readLapHead(const fs::path &p, LapFile &l)
        {
            std::ifstream in(p, std::ios::binary);
            if (!in)
                return false;
            std::string line;
            bool any = false;
            for (int i = 0; i < 16 && std::getline(in, line); ++i)
            {
                if (!line.empty() && line.back() == '\r')
                    line.pop_back();
                if (line.empty() || line[0] != '#')
                    break;
                const std::string kv = trim(line.substr(1));
                const size_t sp = kv.find(' ');
                const std::string k = kv.substr(0, sp), v = sp == std::string::npos ? std::string() : kv.substr(sp + 1);
                any = true;
                if (k == "reader_course")
                    l.course = static_cast<uint32_t>(std::strtoul(v.c_str(), nullptr, 10));
                else if (k == "car")
                    l.car = v;
                else if (k == "lap")
                    l.lap = std::atoi(v.c_str());
                else if (k == "lap_time_ms")
                    l.ms = static_cast<uint32_t>(std::strtoul(v.c_str(), nullptr, 10));
                else if (k == "start")
                    l.start = v;
            }
            return any;
        }
        // The lap's rows from where its own line begins (a lap 1 from the grid starts before the line), as distance, km/h, time.
        Trace readTrace(const fs::path &p)
        {
            Trace t;
            std::string text;
            if (!readText(p, text))
                return t;
            std::vector<float> d, v, s;
            for (const std::string &l : lines(text))
            {
                if (l.empty() || !std::isdigit(static_cast<unsigned char>(l[0])))
                    continue;
                const std::vector<std::string> w = split(l, ',');
                if (w.size() < 12)
                    continue;
                s.push_back(static_cast<float>(std::atof(w[0].c_str()) / 1000.0));
                d.push_back(static_cast<float>(std::atof(w[1].c_str())));
                v.push_back(static_cast<float>(std::atof(w[5].c_str()) * 3.6));
            }
            size_t first = 0;
            for (size_t i = 1; i < d.size(); ++i)
                if (d[i] < d[i - 1] - 100.0f)
                {
                    first = i;
                    break;
                }
            const float t0 = first < s.size() ? s[first] : 0.0f;
            for (size_t i = first; i < d.size(); ++i)
            {
                if (!t.dist.empty() && d[i] < t.dist.back() - 50.0f)
                    break; // (past the line again)
                t.dist.push_back(d[i]), t.speed.push_back(v[i]), t.t.push_back(s[i] - t0);
            }
            return t;
        }
        float timeAt(const Trace &tr, float d)
        {
            if (tr.dist.empty())
                return 0.0f;
            auto it = std::lower_bound(tr.dist.begin(), tr.dist.end(), d);
            if (it == tr.dist.begin())
                return tr.t.front();
            if (it == tr.dist.end())
                return tr.t.back();
            const size_t i = static_cast<size_t>(it - tr.dist.begin());
            const float a = tr.dist[i - 1], b = tr.dist[i];
            const float f = b > a ? (d - a) / (b - a) : 0.0f;
            return tr.t[i - 1] + (tr.t[i] - tr.t[i - 1]) * f;
        }

        // The end of a whole-race file: each car's last row (place, laps, best, total, finished, name).
        void readRaceTail(Race &r)
        {
            std::ifstream in(r.csv, std::ios::binary | std::ios::ate);
            if (!in)
                return;
            const std::streamoff size = in.tellg();
            const std::streamoff from = std::max<std::streamoff>(0, size - 96 * 1024);
            in.seekg(from);
            std::string tail(static_cast<size_t>(size - from), '\0');
            in.read(tail.data(), static_cast<std::streamsize>(tail.size()));
            std::vector<std::string> ls = lines(tail);
            if (from > 0 && !ls.empty())
                ls.erase(ls.begin()); // (cut part way)
            std::vector<RaceCar> last(8);
            std::vector<bool> seen(8, false);
            for (const std::string &l : ls)
            {
                if (l.empty() || !std::isdigit(static_cast<unsigned char>(l[0])))
                    continue;
                const size_t q = l.find('"');
                const std::vector<std::string> w = split(q == std::string::npos ? l : l.substr(0, q), ',');
                if (w.size() < 17)
                    continue;
                const int i = std::atoi(w[7].c_str());
                if (i < 0 || i >= 8)
                    continue;
                RaceCar &c = last[static_cast<size_t>(i)];
                seen[static_cast<size_t>(i)] = true;
                r.course = static_cast<uint32_t>(std::strtoul(w[4].c_str(), nullptr, 10));
                r.laps = std::atoi(w[5].c_str());
                c.index = i;
                c.player = w[8] == "1";
                c.place = std::atoi(w[9].c_str());
                c.lapsDone = std::atoi(w[11].c_str());
                c.best = static_cast<uint32_t>(std::strtoul(w[14].c_str(), nullptr, 10));
                c.total = static_cast<uint32_t>(std::strtoul(w[15].c_str(), nullptr, 10));
                c.finished = w[16] == "1";
                if (q != std::string::npos)
                {
                    const size_t q2 = l.find('"', q + 1);
                    c.car = l.substr(q + 1, q2 == std::string::npos ? std::string::npos : q2 - q - 1);
                }
            }
            // who drove which (the recorder's sidecar, newer builds)
            std::string side = pathText(r.csv);
            side = side.substr(0, side.size() - 4) + "-cars.txt";
            std::string text;
            if (readText(pathFrom(side), text))
            {
                r.sidecar = true;
                for (const std::string &l : lines(text))
                    if (startsWith(l, "car "))
                    {
                        const std::vector<std::string> f = split(l.substr(4), '\t');
                        const int i = f.empty() ? -1 : std::atoi(f[0].c_str());
                        if (f.size() >= 4 && i >= 0 && i < 8)
                        {
                            RaceCar &c = last[static_cast<size_t>(i)];
                            c.who = f[1], c.driver = f[2];
                            if (c.car.empty())
                                c.car = f[3];
                            if (!seen[static_cast<size_t>(i)])
                                c.index = i, c.player = f[1] == "you", seen[static_cast<size_t>(i)] = true;
                        }
                    }
            }
            for (size_t i = 0; i < last.size(); ++i)
                if (seen[i])
                    r.cars.push_back(last[i]);
            std::sort(r.cars.begin(), r.cars.end(), [](const RaceCar &a, const RaceCar &b) { return (a.place ? a.place : 99) < (b.place ? b.place : 99); });
        }

        void scanFiles(Tab &T, const fs::path &root)
        {
            const fs::path data = root / L"racelab-data", lapsDir = root / L"racelab-laps";
            std::vector<Driver> r;
            T.rosterFile = readRoster(data / L"settings" / L"drivers.json", r);
            if (T.editing < 0)
                T.roster = T.rosterFile ? r : defaultDrivers();
            T.hasFile = readGt4Drivers(data / L"ai-lines" / L"gt4-drivers.txt", T.has);
            if (!T.init)
            {
                T.init = true;
                bool any = false;
                for (int k = 1; k <= kCars; ++k)
                    any = any || !T.has[k].empty();
                if (T.hasFile)
                {
                    for (int k = 1; k <= kCars; ++k)
                        T.picks[k] = T.has[k];
                    T.picksFrom = any ? "What GT4 has now." : "GT4 has no drivers now: its AI cars show their car names.";
                }
                else
                {
                    std::string track;
                    if (readNewestAiDoc(data / L"ai", T.picks, track))
                        T.picksFrom = "From your last Send to AI cars in the Race Lab page" + (track.empty() ? std::string() : " (" + track + ")") +
                                      ": not in GT4 yet. Send to GT4 gives GT4 these names.";
                    else
                        T.picksFrom = "No drivers sent to GT4 yet: pick them, then Send to GT4.";
                }
            }
            // the recorder's laps and races
            T.laps.clear();
            std::vector<Race> races;
            std::error_code ec;
            for (fs::directory_iterator it(lapsDir, ec), end; !ec && it != end; it.increment(ec))
            {
                if (!it->is_regular_file(ec))
                    continue;
                const std::string n = pathText(it->path().filename());
                if (!startsWith(n, "gt4-") || !endsWith(n, ".csv"))
                    continue;
                const std::vector<std::string> parts = split(n.substr(0, n.size() - 4), '-'); // gt4, course, date, time, lapNN | race
                if (parts.size() < 5)
                    continue;
                if (parts.back() == "race")
                {
                    Race rc;
                    rc.csv = it->path();
                    rc.stamp = parts[2] + "-" + parts[3];
                    rc.course = static_cast<uint32_t>(std::strtoul(parts[1].c_str(), nullptr, 10));
                    races.push_back(rc);
                }
                else if (startsWith(parts.back(), "lap"))
                {
                    LapFile l;
                    l.path = it->path();
                    l.stamp = parts[2] + "-" + parts[3];
                    if (readLapHead(it->path(), l) && l.ms > 0)
                        T.laps.push_back(l);
                }
            }
            std::sort(T.laps.begin(), T.laps.end(), [](const LapFile &a, const LapFile &b) { return a.stamp != b.stamp ? a.stamp > b.stamp : a.lap > b.lap; });
            std::sort(races.begin(), races.end(), [](const Race &a, const Race &b) { return a.stamp > b.stamp; });
            if (races.size() > 12)
                races.resize(12);
            for (Race &rc : races)
                readRaceTail(rc);
            T.races = races;
            for (std::string &c : T.lastCars)
                c.clear();
            for (const Race &rc : T.races)
                if (rc.sidecar)
                {
                    for (const RaceCar &c : rc.cars)
                        if (startsWith(c.who, "ai"))
                            if (const int k = std::atoi(c.who.substr(2).c_str()); k >= 1 && k <= kCars)
                                T.lastCars[k] = c.car;
                    break;
                }
            T.lapSel = std::clamp(T.lapSel, 0, std::max(0, static_cast<int>(T.laps.size()) - 1));
            T.raceSel = std::clamp(T.raceSel, 0, std::max(0, static_cast<int>(T.races.size()) - 1));
        }

        void appendUtf8(std::string &s, int cp)
        {
            if (cp < 0x80)
                s += static_cast<char>(cp);
            else if (cp < 0x800)
                s += static_cast<char>(0xc0 | (cp >> 6)), s += static_cast<char>(0x80 | (cp & 0x3f));
            else
                s += static_cast<char>(0xe0 | (cp >> 12)), s += static_cast<char>(0x80 | ((cp >> 6) & 0x3f)), s += static_cast<char>(0x80 | (cp & 0x3f));
        }
        void popUtf8(std::string &s)
        {
            while (!s.empty() && (static_cast<unsigned char>(s.back()) & 0xc0) == 0x80)
                s.pop_back();
            if (!s.empty())
                s.pop_back();
        }
    } // namespace

    namespace racelab
    {
        void showTabForTest(int tab, bool renaming)
        {
            g_tab.tab = tab;
            g_tab.testRename = renaming;
            g_tab.editing = -1;
        }
    }

    // ------------------------------------------------------------------ Race Lab
    void App::pageRaceLab()
    {
        const ui::Theme &t = ui::theme();
        FullSettings &f = st_.full;
        Tab &T = g_tab;
        drawHeader("Race Lab", "Your AI drivers, your laps and your races on GT4's courses, here in the launcher (or the whole Race Lab in your browser).");
        const float x0 = kSidebarW + 32, x1 = ui_.width - 32, bottom = ui_.height - 48, cw = x1 - x0;
        const fs::path data = inst_.root / L"racelab-data", lapsDir = inst_.root / L"racelab-laps";
        if (GetTime() - T.scannedAt > 3.0 && !(hidden_ && T.scannedAt > -50.0))
        {
            scanFiles(T, inst_.root);
            T.scannedAt = GetTime();
        }
        if (T.testRename && T.editing < 0 && !T.roster.empty())
            T.editing = 0, T.editText = T.roster[0].name + " Jr";

        // ---- the Race Lab page in the browser (gt4RaceLab.exe), the secondary way in
        const fs::path exe = inst_.exe.parent_path() / L"gt4RaceLab.exe";
        std::error_code ec;
        const bool haveExe = fs::exists(exe, ec);
        int code = 0;
        if (raceLab_.handle && sys::poll(raceLab_, code))
            raceLabNote_ = "The Race Lab page closed.";
        const bool running = raceLab_.handle != nullptr;
        auto openBrowser = [&] {
            sys::Launch l;
            l.exe = exe;
            l.args = {L"--root", inst_.root.wstring(), L"--recordings", lapsDir.wstring(), L"--parent", std::to_wstring(_getpid())};
            l.cwd = inst_.root;
            l.log = inst_.root / L"logs" / L"racelab.log";
            std::string error;
            raceLabNote_ = sys::start(l, raceLab_, error) ? std::string("Opening the Race Lab page in your browser...") : error;
        };

        if (fullui::tabs(ui_, "racelab-tabs", x0, kTop - 10, cw, {"AI drivers", "Laps", "Races", "Settings"}, T.tab))
        {
            ui_.closePopups();
            T.editing = -1;
        }
        if (ui_.button("racelab-browser", R(x1 - 170, kTop - 8, 170, 30), running ? "Open in browser (open)" : "Open in browser", Style::Secondary,
                       haveExe && !running, 13))
            openBrowser();
        const float top = kTop + 40;

        if (T.tab == 0)
        {
            // ---- AI cars 1-5: a driver each, sent to GT4
            const float lw = std::floor(std::min(600.0f, cw * 0.56f));
            {
                const Rectangle c = R(x0, top, lw, bottom - top);
                fullui::card(ui_, c, "AI cars 1\xe2\x80\x93" "5", {Pill::Full});
                float y = c.y + 48;
                std::vector<std::string> names = {"No driver: the car's name"};
                for (const Driver &d : T.roster)
                    names.push_back(d.name.empty() ? std::string("(no name)") : d.name);
                for (int k = 1; k <= kCars; ++k)
                {
                    // (a name GT4 has that isn't in the roster any more stays pickable)
                    if (!T.picks[k].empty() && std::find(names.begin() + 1, names.end(), T.picks[k]) == names.end())
                        names.push_back(T.picks[k]);
                }
                for (int k = 1; k <= kCars; ++k)
                {
                    const Rectangle row = R(c.x + 12, y, c.width - 24, 52);
                    ui_.round(row, 8, t.panel2);
                    ui_.text(strf("Car %d", k), row.x + 14, row.y + 8, 15, t.text, Weight::Semibold);
                    const std::string drove = T.lastCars[k].empty() ? std::string("GT4 picks its car") : T.lastCars[k];
                    ui_.text(ui_.fit(drove, 150, 12), row.x + 14, row.y + 29, 12, t.faint);
                    int sel = 0;
                    for (size_t i = 1; i < names.size(); ++i)
                        if (names[i] == T.picks[k])
                            sel = static_cast<int>(i);
                    if (ui_.dropdown(strf("rl-car-%d", k), R(row.x + 176, row.y + 10, row.width - 190, 32), names, sel, !busy()))
                        T.picks[k] = sel == 0 ? std::string() : names[static_cast<size_t>(sel)];
                    y += 58;
                }
                y += 4;
                y += ui_.wrap(T.picksFrom, c.x + 20, y, c.width - 40, 12.5f, t.muted) + 6;
                // what GT4 has now
                std::string now;
                for (int k = 1; k <= kCars; ++k)
                    if (!T.has[k].empty())
                        now += (now.empty() ? "" : ", ") + T.has[k] + strf(" (%d)", k);
                bool same = true;
                for (int k = 1; k <= kCars; ++k)
                    same = same && T.picks[k] == T.has[k];
                y += ui_.wrap("GT4 has: " + (now.empty() ? std::string("no drivers (the AI cars show their car names)") : now) +
                                  (T.hasFile && same ? "" : !T.hasFile ? "" : "  \xe2\x80\x94 changed here, not sent yet"),
                              c.x + 20, y, c.width - 40, 12.5f, same && T.hasFile ? t.ok : t.warn) + 10;
                if (ui_.button("rl-send", R(c.x + 20, y, 200, 36), "Send to GT4", Style::Primary, !busy()))
                {
                    std::string txt = "gt4-ai-drivers 1\n# from the launcher's Race Lab tab, " + isoNow() +
                                      "\n# AI car k is the k-th car in the race that isn't yours; GT4's own AI drives every car\n";
                    std::string sent;
                    for (int k = 1; k <= kCars; ++k)
                    {
                        if (T.picks[k].empty())
                            continue;
                        txt += strf("car %d ", k) + T.picks[k] + "\n";
                        for (const Driver &d : T.roster)
                            if (d.name == T.picks[k] && !d.style.empty())
                                txt += strf("style %d ", k) + d.style + "\n";
                        sent += (sent.empty() ? "" : ", ") + T.picks[k];
                    }
                    std::string error;
                    fs::create_directories(data / L"ai-lines", ec);
                    if (writeFileAtomic(data / L"ai-lines" / L"gt4-drivers.txt", txt, error))
                    {
                        T.note = sent.empty() ? "Sent: no drivers, so GT4's AI cars show their car names again."
                                              : "Sent to GT4: " + sent + ". Their names show from the next race" +
                                                    (f.raceLabDrivers ? "." : " once Settings > Drivers' names in GT4 is on.");
                        T.noteOk = true;
                    }
                    else
                        T.note = "Couldn't write gt4-drivers.txt: " + error, T.noteOk = false;
                    T.scannedAt = -100.0;
                }
                if (ui_.button("rl-clear", R(c.x + 232, y, 130, 36), "Clear all", Style::Secondary, !busy()))
                    for (int k = 1; k <= kCars; ++k)
                        T.picks[k].clear();
                y += 46;
                if (!T.note.empty())
                    y += ui_.wrap(T.note, c.x + 20, y, c.width - 40, 13, T.noteOk ? t.ok : t.bad) + 8;
                ui_.wrap("Where the names show: the timing tower with Names set to Driver (Driving > Race HUD), the telemetry and phone "
                         "pages, and Races here. AI car 1 is the first car in the race that isn't yours, and so on. GT4's own AI still "
                         "drives every car; the drivers' styles (Drivers) don't change how it drives yet.",
                         c.x + 20, std::max(y, c.y + c.height - 80), c.width - 40, 12, ui::alpha(t.faint, 0.95f));
            }
            {
                // ---- the drivers (the Race Lab's roster)
                const Rectangle c = R(x0 + lw + kGap, top, cw - lw - kGap, bottom - top);
                fullui::card(ui_, c, "Drivers");
                const Rectangle view = R(c.x + 8, c.y + 44, c.width - 16, c.height - 44 - 60);
                const float off = ui_.beginScroll("rl-roster", view);
                float y = view.y - off;
                bool changed = false;
                int remove = -1;
                for (size_t i = 0; i < T.roster.size(); ++i)
                {
                    Driver &d = T.roster[i];
                    const bool ed = T.editing == static_cast<int>(i);
                    const float h = ed ? 92.0f : 54.0f;
                    const Rectangle row = R(view.x + 4, y, view.width - 8, h - 6);
                    ui_.round(row, 8, ed ? ui::alpha(t.accent, 0.12f) : t.panel2);
                    ui_.circle(row.x + 24, row.y + 24, 16, t.accentDim);
                    ui_.text(initials(d.name), row.x + 24, row.y + 15, 13, t.text, Weight::Semibold, Align::Center);
                    if (ed)
                    {
                        const Rectangle box = R(row.x + 50, row.y + 6, row.width - 60, 34);
                        ui_.round(box, 6, t.panel);
                        ui_.roundLine(box, 6, 1.5f, t.accent);
                        const float w = ui_.text(T.editText, box.x + 10, box.y + 8, 15, t.text, Weight::Semibold);
                        if (hidden_ || static_cast<int>(GetTime() * 2.0) % 2 == 0)
                            ui_.fill(R(box.x + 12 + w, box.y + 7, 2, 20), t.accent);
                        if (ui_.button(strf("rl-done-%zu", i), R(row.x + 50, row.y + 46, 90, 30), "Done", Style::Primary, true, 13) ||
                            (ui_.inputOn() && !hidden_ && IsKeyPressed(KEY_ENTER)))
                        {
                            const std::string n = trim(T.editText);
                            if (!n.empty())
                            {
                                // (the cars given the old name keep their driver)
                                for (int k = 1; k <= kCars; ++k)
                                    if (T.picks[k] == d.name)
                                        T.picks[k] = n;
                                d.name = n;
                                changed = true;
                            }
                            T.editing = -1;
                        }
                        if (ui_.button(strf("rl-cancel-%zu", i), R(row.x + 148, row.y + 46, 90, 30), "Cancel", Style::Secondary, true, 13) ||
                            (ui_.inputOn() && !hidden_ && IsKeyPressed(KEY_ESCAPE)))
                            T.editing = -1;
                        if (ui_.button(strf("rl-remove-%zu", i), R(row.x + row.width - 104, row.y + 46, 96, 30), "Remove", Style::Danger, true, 13))
                            remove = static_cast<int>(i), T.editing = -1;
                    }
                    else
                    {
                        ui_.text(ui_.fit(d.name.empty() ? std::string("(no name)") : d.name, row.width - 120, 14.5f, Weight::Semibold), row.x + 50, row.y + 7,
                                 14.5f, t.text, Weight::Semibold);
                        ui_.text(ui_.fit(d.style.empty() ? std::string("-") : d.style, row.width - 60, 12), row.x + 50, row.y + 28, 12, t.faint);
                        if (ui_.button(strf("rl-rename-%zu", i), R(row.x + row.width - 76, row.y + 8, 68, 26), "Rename", Style::Ghost, !busy(), 12.5f))
                            T.editing = static_cast<int>(i), T.editText = d.name;
                    }
                    y += h;
                }
                ui_.endScroll("rl-roster", y + off - view.y);
                if (remove >= 0)
                {
                    T.roster.erase(T.roster.begin() + remove);
                    changed = true;
                }
                // typing the new name
                if (T.editing >= 0 && ui_.inputOn() && !hidden_)
                {
                    for (int ch = GetCharPressed(); ch > 0; ch = GetCharPressed())
                        if (ch >= 32 && ch <= 0xff && ch != 127 && T.editText.size() < 40)
                            appendUtf8(T.editText, ch);
                    if ((IsKeyPressed(KEY_BACKSPACE) || IsKeyPressedRepeat(KEY_BACKSPACE)) && !T.editText.empty())
                        popUtf8(T.editText);
                }
                const float by = c.y + c.height - 50;
                if (ui_.button("rl-add", R(c.x + 16, by, 130, 34), "Add driver", Style::Secondary, !busy() && T.roster.size() < 30, 13))
                {
                    Driver d;
                    d.id = strf("d%llx", static_cast<unsigned long long>(std::time(nullptr)));
                    d.name = "New driver";
                    d.speedPct = 2, d.brakeM = 2, d.mistakes = 1;
                    T.roster.push_back(d);
                    T.editing = static_cast<int>(T.roster.size()) - 1, T.editText = d.name;
                    ui_.scrollTo("rl-roster", 1e6f);
                    changed = true;
                }
                if (ui_.button("rl-defaults", R(c.x + 154, by, 150, 34), "Default drivers", Style::Ghost, !busy(), 13))
                {
                    T.roster = defaultDrivers();
                    T.editing = -1;
                    changed = true;
                }
                if (changed && !hidden_)
                {
                    std::string error;
                    fs::create_directories(data / L"settings", ec);
                    if (!writeFileAtomic(data / L"settings" / L"drivers.json", rosterJson(T.roster), error))
                        T.note = "Couldn't save the drivers: " + error, T.noteOk = false;
                    T.rosterFile = true;
                }
                ui_.text(T.rosterFile ? "Shared with the Race Lab page (racelab-data)." : "The Race Lab's eight drivers.", c.x + 316, by + 9, 12, t.faint);
            }
        }
        else if (T.tab == 1)
        {
            // ---- the recorded laps, each against the course's best
            const float lw = std::floor(std::min(560.0f, cw * 0.48f));
            const Rectangle lc = R(x0, top, lw, bottom - top);
            fullui::card(ui_, lc, strf("Your laps (%zu)", T.laps.size()), {Pill::Full});
            if (T.laps.empty())
                ui_.wrap(f.raceLabRecord ? "No laps yet: drive a race in GT4 (F10 stops and starts the recording)."
                                         : "No laps yet. Turn on Settings > Record my laps, then drive a race in GT4.",
                         lc.x + 20, lc.y + 52, lc.width - 40, 13, t.muted);
            std::vector<uint32_t> bestOf; // per lap: the best time on its course
            for (const LapFile &l : T.laps)
            {
                uint32_t b = 0;
                for (const LapFile &m : T.laps)
                    if (m.course == l.course && (b == 0 || m.ms < b))
                        b = m.ms;
                bestOf.push_back(b);
            }
            const Rectangle view = R(lc.x + 8, lc.y + 44, lc.width - 16, lc.height - 52);
            const float off = ui_.beginScroll("rl-laps", view);
            float y = view.y - off;
            for (size_t i = 0; i < T.laps.size(); ++i)
            {
                const LapFile &l = T.laps[i];
                const Rectangle row = R(view.x + 4, y, view.width - 8, 40);
                if (ui_.rowButton(strf("rl-lap-%zu", i), row, static_cast<int>(i) == T.lapSel))
                    T.lapSel = static_cast<int>(i);
                const bool best = l.ms == bestOf[i];
                if (best)
                    ui_.circle(row.x + 12, row.y + 20, 4, Color{182, 124, 240, 255});
                ui_.text(lapText(l.ms), row.x + 24, row.y + 4, 15, best ? Color{198, 150, 250, 255} : t.text, Weight::Semibold);
                ui_.text(ui_.fit(courseName(l.course) + kDot + strf("lap %d", l.lap) + (l.start == "grid" ? " (from the grid)" : ""), row.width - 230, 12.5f),
                         row.x + 120, row.y + 5, 12.5f, t.muted);
                ui_.text(ui_.fit(l.car, row.width - 230, 12), row.x + 120, row.y + 22, 12, t.faint);
                ui_.text(stampText(l.stamp), row.x + row.width - 10, row.y + 12, 12, t.faint, Weight::Regular, Align::Right);
                y += 44;
            }
            ui_.endScroll("rl-laps", y + off - view.y);

            const Rectangle cc = R(x0 + lw + kGap, top, cw - lw - kGap, bottom - top);
            fullui::card(ui_, cc, "Compare");
            if (!T.laps.empty())
            {
                const LapFile &a = T.laps[static_cast<size_t>(T.lapSel)];
                // compared with the best lap on that course (the second best when this one is the best)
                int bi = -1;
                for (size_t i = 0; i < T.laps.size(); ++i)
                    if (static_cast<int>(i) != T.lapSel && T.laps[i].course == a.course && (bi < 0 || T.laps[i].ms < T.laps[static_cast<size_t>(bi)].ms))
                        bi = static_cast<int>(i);
                const std::string key = pathText(a.path) + "|" + (bi >= 0 ? pathText(T.laps[static_cast<size_t>(bi)].path) : std::string());
                if (key != T.traceFor)
                {
                    T.traceFor = key;
                    T.trA = readTrace(a.path);
                    T.trB = bi >= 0 ? readTrace(T.laps[static_cast<size_t>(bi)].path) : Trace{};
                    T.cmpIndex = bi;
                }
                float y2 = cc.y + 48;
                const Color colA{242, 179, 61, 255}, colB{110, 160, 255, 255};
                ui_.fill(R(cc.x + 20, y2 + 7, 14, 3), colA);
                ui_.text(lapText(a.ms) + kDot + courseName(a.course) + strf(", lap %d", a.lap), cc.x + 40, y2, 13.5f, t.text, Weight::Semibold);
                y2 += 22;
                if (bi >= 0)
                {
                    const LapFile &b = T.laps[static_cast<size_t>(bi)];
                    ui_.fill(R(cc.x + 20, y2 + 7, 14, 3), colB);
                    const double d = (static_cast<double>(a.ms) - b.ms) / 1000.0;
                    ui_.text(lapText(b.ms) + kDot + strf("lap %d, ", b.lap) + stampText(b.stamp) + (d >= 0 ? strf("  (you +%.3f s)", d) : strf("  (you %.3f s)", d)),
                             cc.x + 40, y2, 13.5f, t.muted);
                }
                else
                    ui_.text("No other lap on this course to compare with yet.", cc.x + 40, y2, 13, t.faint);
                y2 += 30;
                // speed along the lap
                const Rectangle g = R(cc.x + 56, y2, cc.width - 76, std::max(120.0f, (cc.y + cc.height - y2) * 0.58f));
                const Rectangle g2 = R(g.x, g.y + g.height + 34, g.width, std::max(70.0f, cc.y + cc.height - (g.y + g.height + 34) - 40));
                ui_.round(R(g.x - 4, g.y - 4, g.width + 8, g.height + 8), 6, t.panel2);
                float dmax = 0, vmax = 60;
                for (const Trace *tr : {&T.trA, &T.trB})
                    for (size_t i = 0; i < tr->dist.size(); ++i)
                        dmax = std::max(dmax, tr->dist[i]), vmax = std::max(vmax, tr->speed[i]);
                vmax = std::ceil(vmax / 50.0f) * 50.0f;
                for (int s = 0; s <= static_cast<int>(vmax); s += 50)
                {
                    const float yy = g.y + g.height - g.height * s / vmax;
                    ui_.line(g.x, yy, g.x + g.width, yy, 1, ui::alpha(t.border, 0.8f));
                    ui_.text(strf("%d", s), g.x - 8, yy - 7, 11, t.faint, Weight::Regular, Align::Right);
                }
                ui_.text("km/h", g.x - 8, g.y - 22, 11, t.faint, Weight::Regular, Align::Right);
                if (dmax > 0)
                {
                    for (const auto &[tr, col] : {std::pair<const Trace *, Color>{&T.trB, colB}, std::pair<const Trace *, Color>{&T.trA, colA}})
                        for (size_t i = 1; i < tr->dist.size(); ++i)
                            ui_.line(g.x + g.width * tr->dist[i - 1] / dmax, g.y + g.height - g.height * tr->speed[i - 1] / vmax, g.x + g.width * tr->dist[i] / dmax,
                                     g.y + g.height - g.height * tr->speed[i] / vmax, 1.6f, col);
                    // time gained (below the line) or lost (above) against the other lap, along the lap
                    ui_.round(R(g2.x - 4, g2.y - 4, g2.width + 8, g2.height + 8), 6, t.panel2);
                    ui_.text("Time against the other lap (up: you lose time)", g2.x, g2.y - 24, 12, t.faint);
                    if (!T.trB.dist.empty())
                    {
                        const float dEnd = std::min(T.trA.dist.back(), T.trB.dist.back());
                        std::vector<float> dd;
                        float span = 0.25f;
                        for (float d = 0; d <= dEnd; d += 5.0f)
                        {
                            dd.push_back(timeAt(T.trA, d) - timeAt(T.trB, d));
                            span = std::max(span, std::fabs(dd.back()));
                        }
                        span = std::ceil(span * 4.0f) / 4.0f;
                        const float mid = g2.y + g2.height * 0.5f;
                        ui_.line(g2.x, mid, g2.x + g2.width, mid, 1, t.border);
                        ui_.text(strf("+%.2f s", span), g2.x - 8, g2.y - 6, 11, t.faint, Weight::Regular, Align::Right);
                        ui_.text(strf("-%.2f s", span), g2.x - 8, g2.y + g2.height - 8, 11, t.faint, Weight::Regular, Align::Right);
                        for (size_t i = 1; i < dd.size(); ++i)
                            ui_.line(g2.x + g2.width * (i - 1) * 5.0f / dmax, mid - g2.height * 0.5f * dd[i - 1] / span, g2.x + g2.width * i * 5.0f / dmax,
                                     mid - g2.height * 0.5f * dd[i] / span, 1.6f, dd[i] > 0 ? t.bad : t.ok);
                    }
                    ui_.text(strf("%.0f m", dmax), g2.x + g2.width, g2.y + g2.height + 8, 11, t.faint, Weight::Regular, Align::Right);
                }
            }
        }
        else if (T.tab == 2)
        {
            // ---- the recorded races: who drove what, and how it ended
            const float lw = std::floor(std::min(360.0f, cw * 0.34f));
            const Rectangle lc = R(x0, top, lw, bottom - top);
            fullui::card(ui_, lc, "Your races", {Pill::Full});
            if (T.races.empty())
                ui_.wrap("No races recorded yet: with Settings > Record my laps on, every race is kept with all its cars.", lc.x + 20, lc.y + 52, lc.width - 40,
                         13, t.muted);
            float y = lc.y + 48;
            for (size_t i = 0; i < T.races.size() && y < lc.y + lc.height - 50; ++i)
            {
                const Race &r = T.races[i];
                const Rectangle row = R(lc.x + 10, y, lc.width - 20, 46);
                if (ui_.rowButton(strf("rl-race-%zu", i), row, static_cast<int>(i) == T.raceSel))
                    T.raceSel = static_cast<int>(i);
                ui_.text(courseName(r.course), row.x + 12, row.y + 5, 14, t.text, Weight::Semibold);
                ui_.text(stampText(r.stamp) + kDot + strf("%zu cars", r.cars.size()) + (r.laps > 0 ? kDot + strf("%d laps", r.laps) : std::string()), row.x + 12,
                         row.y + 25, 12, t.faint);
                y += 50;
            }
            const Rectangle rc = R(x0 + lw + kGap, top, cw - lw - kGap, bottom - top);
            fullui::card(ui_, rc, "Result");
            if (!T.races.empty())
            {
                const Race &r = T.races[static_cast<size_t>(T.raceSel)];
                float y2 = rc.y + 48;
                ui_.text(courseName(r.course) + kDot + stampText(r.stamp), rc.x + 20, y2, 14, t.muted);
                y2 += 28;
                const float cPos = rc.x + 20, cDrv = rc.x + 64, cCar = cDrv + (rc.width - 84) * 0.36f, cBest = rc.x + rc.width - 150, cTot = rc.x + rc.width - 20;
                fullui::caps(ui_, cPos, y2, "Pos", t.faint);
                fullui::caps(ui_, cDrv, y2, "Driver", t.faint);
                fullui::caps(ui_, cCar, y2, "Car", t.faint);
                ui_.text("BEST", cBest, y2, 11, t.faint, Weight::Semibold, Align::Right);
                ui_.text("TIME", cTot, y2, 11, t.faint, Weight::Semibold, Align::Right);
                y2 += 22;
                uint32_t fastest = 0, winner = 0;
                for (const RaceCar &c : r.cars)
                {
                    if (c.best && (!fastest || c.best < fastest))
                        fastest = c.best;
                    if (c.place == 1 && c.finished)
                        winner = c.total;
                }
                for (const RaceCar &c : r.cars)
                {
                    const Rectangle row = R(rc.x + 10, y2, rc.width - 20, 38);
                    ui_.round(row, 6, c.player ? ui::alpha(Color{242, 179, 61, 255}, 0.14f) : t.panel2);
                    ui_.text(c.place ? std::to_string(c.place) : std::string("-"), cPos + 6, row.y + 10, 15, t.text, Weight::Bold);
                    const std::string drv = c.player ? std::string("You") : !c.driver.empty() ? c.driver : r.sidecar ? std::string("(no driver)") : std::string("-");
                    ui_.text(ui_.fit(drv, cCar - cDrv - 10, 14, Weight::Semibold), cDrv, row.y + 10, 14, c.driver.empty() && !c.player ? t.faint : t.text, Weight::Semibold);
                    ui_.text(ui_.fit(c.car, cBest - cCar - 70, 13), cCar, row.y + 11, 13, t.muted);
                    ui_.text(lapText(c.best), cBest, row.y + 11, 13, c.best && c.best == fastest ? Color{198, 150, 250, 255} : t.text, Weight::Regular, Align::Right);
                    const std::string tot = !c.finished ? strf("%d lap%s", c.lapsDone, c.lapsDone == 1 ? "" : "s") : c.place == 1 || !winner ? lapText(c.total)
                                                                                                                : strf("+%.3f", (static_cast<double>(c.total) - winner) / 1000.0);
                    ui_.text(tot, cTot, row.y + 11, 13, t.text, Weight::Regular, Align::Right);
                    y2 += 42;
                }
                y2 += 8;
                ui_.wrap(r.sidecar ? "Drivers as the Race Lab named them for this race (AI car 1 is the first car that isn't yours, and so on)."
                                   : "Recorded before drivers were kept with each race: car names only.",
                         rc.x + 20, y2, rc.width - 40, 12, t.faint);
            }
        }
        else
        {
            // ---- settings
            float y = top;
            {
                const char *note = "Every lap you drive goes to its own file, with GT4's lap time, and every race to one file with all the cars "
                                   "and their drivers. F10 in a race stops and starts the recording; a red REC shows top right while it records.";
                const float h = 48 + 34 + 8 + ui_.wrapHeight(note, cw - 40, 12.5f) + 16;
                const Rectangle c = R(x0, y, cw, h);
                fullui::card(ui_, c, "Record laps for the Race Lab", {Pill::Full});
                float yy = c.y + 48;
                ui_.text("Record my laps", c.x + 20, yy + 4, 14, t.text);
                if (ui_.toggle("racelab-record", c.x + c.width - 60, yy, f.raceLabRecord, !busy()))
                    markDirty();
                yy += 42;
                ui_.wrap(note, c.x + 20, yy, c.width - 40, 12.5f, ui::alpha(t.faint, 0.9f));
                y = c.y + c.height + kGap;
            }
            {
                const char *note = "GT4's AI cars carry the names you send from AI drivers (or the Race Lab page's Send to AI cars): the timing tower "
                                   "with Driver names, the telemetry and phone pages, and the races recorded. Off: they keep their car names.";
                const float h = 48 + 34 + 8 + ui_.wrapHeight(note, cw - 40, 12.5f) + 16;
                const Rectangle c = R(x0, y, cw, h);
                fullui::card(ui_, c, "Drivers' names in GT4", {Pill::Full});
                float yy = c.y + 48;
                ui_.text("Give GT4's AI cars the Race Lab's drivers", c.x + 20, yy + 4, 14, t.text);
                if (ui_.toggle("racelab-drivers", c.x + c.width - 60, yy, f.raceLabDrivers, !busy()))
                    markDirty();
                yy += 42;
                ui_.wrap(note, c.x + 20, yy, c.width - 40, 12.5f, ui::alpha(t.faint, 0.9f));
                y = c.y + c.height + kGap;
            }
            {
                const char *note = "The whole Race Lab in your browser, served from this PC only (127.0.0.1): racing lines on GT4's course maps, braking "
                                   "points, the AI cars and drivers, laps under Laps > \"Add laps from the game\". It saves beside gt4-start.env, in "
                                   "racelab-data, the same files this tab uses.";
                const float h = 48 + 36 + 8 + ui_.wrapHeight(note, cw - 40, 12.5f) + 16 + (raceLabNote_.empty() ? 0 : 22);
                const Rectangle c = R(x0, y, cw, h);
                fullui::card(ui_, c, "The Race Lab page in your browser", {Pill::Full});
                float yy = c.y + 48;
                if (ui_.button("racelab-open", R(c.x + 20, yy, 240, 34), running ? "The Race Lab page is open" : "Open in browser", Style::Secondary,
                               haveExe && !running))
                    openBrowser();
                if (ui_.button("racelab-folder", R(c.x + 272, yy, 200, 34), "Open racelab-data", Style::Ghost, !hidden_))
                {
                    fs::create_directories(data, ec);
                    sys::openInExplorer(data);
                }
                if (!haveExe)
                    ui_.text("gt4RaceLab.exe is not beside this program.", c.x + 490, yy + 9, 13, t.warn);
                yy += 44;
                yy += ui_.wrap(note, c.x + 20, yy, c.width - 40, 12.5f, ui::alpha(t.faint, 0.9f));
                if (!raceLabNote_.empty())
                    ui_.text(raceLabNote_, c.x + 20, yy + 4, 13, t.muted);
            }
        }
        if (T.tab != 3 && !raceLabNote_.empty())
            ui_.text(ui_.fit(raceLabNote_, 360, 12), x1 - 180, kTop + 26, 12, t.faint, Weight::Regular, Align::Right);
    }
} // namespace gt4stock
