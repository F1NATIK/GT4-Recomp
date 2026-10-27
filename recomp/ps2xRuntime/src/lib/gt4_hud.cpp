#include "gt4_hud.h"
#include "gt4_ai_drivers.h"
#include "gt4_mirror.h"
#include "gt4_race_reader.h"
#include "present_overlay.h"
#include "../launcher/gt4stock/shared/gt4_hud_boxes.h"
#include "../launcher/gt4stock/shared/gt4_hud_look.h"

#include "raylib.h"
#include "rlgl.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <functional>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#if defined(_WIN32)
// (the pages folder next to GT4.exe, as gt4_pages.cpp finds it; windows.h's names clash with raylib's)
extern "C" __declspec(dllimport) unsigned long __stdcall GetModuleFileNameA(void *module, char *name, unsigned long size);
#endif

// Our own race HUD widgets and start lights (gt4_hud.h), in GT3's HUD look (shared/gt4_hud_look.h: the launcher's Race HUD
// previews draw with the same calls, 20 Oct 2026).

namespace gt4hud
{
    namespace
    {
        // The boxes, in GT4_HUD_<KEY>'s order (shared/gt4_hud_boxes.h: the launcher's page reads the same sizes).
        using gt4hudbox::Box;
        constexpr int WidgetCount = gt4hudbox::kCount;
        constexpr Box Radar = gt4hudbox::Radar, Delta = gt4hudbox::Delta, Pedals = gt4hudbox::Pedals, GMeter = gt4hudbox::GMeter,
                      Tower = gt4hudbox::Tower, Map = gt4hudbox::Map, Splits = gt4hudbox::Splits, Revs = gt4hudbox::Revs, Speed = gt4hudbox::Speed,
                      Gear = gt4hudbox::Gear, LapBox = gt4hudbox::Lap;
        constexpr const char *kKeys[WidgetCount] = {"GT4_HUD_RADAR", "GT4_HUD_DELTA", "GT4_HUD_PEDALS", "GT4_HUD_GMETER", "GT4_HUD_TOWER", "GT4_HUD_MAP",
                                                    "GT4_HUD_SPLITS", "GT4_HUD_REVS", "GT4_HUD_SPEED", "GT4_HUD_GEAR", "GT4_HUD_LAP"};

        struct Spec
        {
            bool on = false;
            int place = 0;      // 0 bottom right, 1 bottom left, 2 top right, 3 top left, 4 bottom centre, 5 top centre, 6 above GT4's dash,
                                // 7 automatic (clear of GT4's HUD in every view: draw())
            bool xy = false;    // the new form (HUD hand-off 7 A3): the box's centre at x, y (fractions of the HUD area)
            float x = 0.5f, y = 0.5f;
            float size = 1.0f;  // 0.5 .. 2 (the new form: 0.4 .. 2.5)
            float alpha = 0.8f; // the panel's: 1 - see-through
            int style = 0;      // radar 0 round, 1 square; delta 0 bar, 1 number; g-meter 0 trail, 1 dot; map 0 locked, 1 rotating;
                                // revs 0 bar, 1 lights; lap 0 lap, 1 clock
            bool simple = false;// the map's detail (HUD hand-off 10): the centreline instead of GT4's art
            int towerName = 0;  // the tower: 0 car, 1 short, 2 driver
            int towerCols = gt4hudbox::kTowerColsDefault; // bits: 1 gap, 2 int, 4 best, 8 last
            int towerRows = gt4hudbox::kTowerRowsDefault;
        };
        struct Settings
        {
            Spec w[WidgetCount];
            bool lights = false;
            int count = 5;
            bool lightsBox = false; // GT4_START_LIGHTS_BOX given: the row's centre and size (else today's top centre)
            float lx = 0.5f, ly = 0.075f, lsize = 1.0f;
            bool twoRows = false;   // GT4_START_LOOK=two
            // (HUD hand-off 7 D / 9) the radar's page settings
            bool radarNear = false;                 // GT4_RADAR_SHOW=near
            float radarRange = 25.0f;               // GT4_RADAR_RANGE (m, 10..80, the GT3 radar settings): each race starts here
            std::vector<float> radarZoom;           // GT4_RADAR_ZOOM, smallest first (empty: off, the default since the GT3 radar
                                                    // settings; the zoom code stays, dormant, for a list set by hand)
            bool radarLabels = true;                // GT4_RADAR_LABELS
            float radarCars = 0.9f;                 // GT4_RADAR_CARS / 100: the cars' opacity (yours 0.1 more, as GT3 draws it)
            float radarBg = -1.0f;                  // GT4_RADAR_BG / 100: the background's opacity (-1 not set: the box's 1 - see-through)
            uint32_t radarCol[5] = {0x78dcff, 0xffe65a, 0xffe65a, 0xff8c00, 0x000000}; // you, other, close (drawn as other), alongside,
                                                                                       // background (GT3's defaults)
            int zoomKey = 90;                       // GT4_HUD_ZOOM_KEY (raylib KEY_Z), 0 none
            int zoomPad = -1;                       // GT4_HUD_ZOOM_PAD (raylib GamepadButton), -1 none
        };

        // The fields of a comma list, spaces left out.
        std::vector<std::string> fields(const char *v)
        {
            std::vector<std::string> out(1);
            for (const char *c = v; *c; ++c)
                if (*c == ',')
                    out.emplace_back();
                else if (*c != ' ' && *c != '\t')
                    out.back() += *c;
            return out;
        }
        bool number(const std::string &t, float &out)
        {
            char *end = nullptr;
            const float f = std::strtof(t.c_str(), &end);
            if (t.empty() || !end || *end || !std::isfinite(f))
                return false;
            out = f;
            return true;
        }

        // off | on,<place>,<size %>,<see-through %> (the old form, HUD hand-off 4: read and placed as before), or the new form
        // (HUD hand-off 7 A3): on,<x %>,<y %>,<size %>,<see-through %>[,<style>], x and y the box's centre in % of the HUD
        // area (one decimal), or auto,auto (Automatic).
        Spec parse(int widget, const char *v)
        {
            Spec s;
            if (!v || std::strncmp(v, "on", 2) != 0)
                return s;
            s.on = true;
            const std::vector<std::string> f = fields(v);
            if (f.size() >= 5u)
            {
                float x = 50.0f, y = 50.0f, size = 100.0f, see = 20.0f;
                const bool autoPlace = f[1] == "auto" && f[2] == "auto";
                if ((autoPlace || (number(f[1], x) && number(f[2], y))) && number(f[3], size) && number(f[4], see))
                {
                    s.xy = !autoPlace;
                    s.place = autoPlace ? 7 : 0;
                    s.x = std::clamp(x, 0.0f, 100.0f) / 100.0f, s.y = std::clamp(y, 0.0f, 100.0f) / 100.0f;
                    s.size = std::clamp(size, 40.0f, 250.0f) / 100.0f;
                    s.alpha = 1.0f - std::clamp(see, 0.0f, 90.0f) / 100.0f;
                    if (widget == Tower)
                    {
                        // (HUD hand-off 7 C) ...,<name car|short|driver>,<columns gap+int+best+last>,<rows 4-16>
                        if (f.size() >= 6u)
                        {
                            int n = -1;
                            for (int k = 0; k < 3; ++k)
                                if (f[5] == gt4hudbox::kTowerNames[k])
                                    n = k;
                            if (n < 0)
                                std::fprintf(stderr, "[gt4-hud] GT4_HUD_TOWER: name '%s' not understood (car, short, driver): car\n", f[5].c_str());
                            s.towerName = std::max(n, 0);
                        }
                        if (f.size() >= 7u)
                        {
                            int bits = 0;
                            std::string item;
                            for (size_t i = 0; i <= f[6].size(); ++i)
                            {
                                if (i < f[6].size() && f[6][i] != '+')
                                {
                                    item += f[6][i];
                                    continue;
                                }
                                bool ok = false;
                                for (int k = 0; k < 4; ++k)
                                    if (item == gt4hudbox::kTowerCols[k])
                                        bits |= 1 << k, ok = true;
                                if (!ok && !item.empty())
                                    std::fprintf(stderr, "[gt4-hud] GT4_HUD_TOWER: column '%s' not understood (gap, int, best, last): left out\n", item.c_str());
                                item.clear();
                            }
                            s.towerCols = f[6] == "none" ? 0 : bits ? bits : gt4hudbox::kTowerColsDefault;
                        }
                        float rows = static_cast<float>(gt4hudbox::kTowerRowsDefault);
                        if (f.size() >= 8u && number(f[7], rows))
                            s.towerRows = std::clamp(static_cast<int>(std::lround(rows)), 4, 16);
                        return s;
                    }
                    if (f.size() >= 6u)
                    {
                        const std::string &st = f[5];
                        const char *const *styles = gt4hudbox::kStyles[widget];
                        if (styles[1] && st == styles[1])
                            s.style = 1;
                        else if (!(styles[0] && st == styles[0]))
                            std::fprintf(stderr, "[gt4-hud] %s: style '%s' not understood: the first style\n", kKeys[widget], st.c_str());
                    }
                    if (widget == Map && f.size() >= 7u)
                    {
                        if (f[6] == gt4hudbox::kMapDetail[1])
                            s.simple = true;
                        else if (f[6] != gt4hudbox::kMapDetail[0])
                            std::fprintf(stderr, "[gt4-hud] GT4_HUD_MAP: detail '%s' not understood (detailed, simple): detailed\n", f[6].c_str());
                    }
                    return s;
                }
                std::fprintf(stderr, "[gt4-hud] %s=%s not understood (on,<x>,<y>,<size>,<see-through>[,<style>]): bottom right\n", kKeys[widget], v);
                return s;
            }
            int place = 0, size = 100, see = 20;
            if (std::sscanf(v, "on,%d,%d,%d", &place, &size, &see) >= 1)
            {
                s.place = std::clamp(place, 0, 7);
                s.size = static_cast<float>(std::clamp(size, 50, 200)) / 100.0f;
                s.alpha = 1.0f - static_cast<float>(std::clamp(see, 0, 90)) / 100.0f;
            }
            return s;
        }
        // HUD hand-off 7 D / 9: the radar page's keys (gt4_hud.h). A value not understood keeps the default, with a line in the log.
        void readRadar(Settings &t)
        {
            auto get = [](const char *k) -> std::string {
                const char *v = std::getenv(k);
                std::string s;
                for (const char *c = v ? v : ""; *c; ++c)
                    if (*c != ' ' && *c != '\t')
                        s += static_cast<char>(*c >= 'A' && *c <= 'Z' ? *c + 32 : *c);
                return s;
            };
            const std::string show = get("GT4_RADAR_SHOW");
            t.radarNear = show == "near";
            if (!show.empty() && show != "near" && show != "always")
                std::fprintf(stderr, "[gt4-hud] GT4_RADAR_SHOW=%s not understood (always, near): always\n", show.c_str());
            float f = 0.0f;
            if (const std::string r = get("GT4_RADAR_RANGE"); !r.empty())
            {
                // 10 to 80 m in 1 m steps (the GT3 radar settings); a number outside (hand-off 9's 5 or 100) is clamped.
                if (number(r, f) && f > 0.0f && f <= 1000.0f)
                    t.radarRange = std::clamp(std::round(f), 10.0f, 80.0f);
                else
                    std::fprintf(stderr, "[gt4-hud] GT4_RADAR_RANGE=%s not understood (10 to 80 m): 25\n", r.c_str());
            }
            if (const std::string z = get("GT4_RADAR_ZOOM"); !z.empty())
            {
                if (z == "off")
                    t.radarZoom.clear();
                else
                {
                    std::vector<float> steps;
                    for (const std::string &p : fields(z.c_str()))
                        if (number(p, f) && f >= 5.0f && f <= 100.0f)
                            steps.push_back(std::round(f));
                        else if (!p.empty())
                            std::fprintf(stderr, "[gt4-hud] GT4_RADAR_ZOOM: '%s' not understood (5 to 100 m): left out\n", p.c_str());
                    std::sort(steps.begin(), steps.end());
                    steps.erase(std::unique(steps.begin(), steps.end()), steps.end());
                    if (!steps.empty())
                        t.radarZoom = steps;
                }
            }
            if (const std::string l = get("GT4_RADAR_LABELS"); !l.empty())
            {
                t.radarLabels = l != "off";
                if (l != "on" && l != "off")
                    std::fprintf(stderr, "[gt4-hud] GT4_RADAR_LABELS=%s not understood (on, off): on\n", l.c_str()), t.radarLabels = true;
            }
            if (const std::string c = get("GT4_RADAR_CARS"); !c.empty())
            {
                if (number(c, f) && f >= 10.0f && f <= 100.0f)
                    t.radarCars = f / 100.0f;
                else
                    std::fprintf(stderr, "[gt4-hud] GT4_RADAR_CARS=%s not understood (10 to 100): 90\n", c.c_str());
            }
            if (const std::string b = get("GT4_RADAR_BG"); !b.empty())
            {
                if (number(b, f) && f >= 0.0f && f <= 100.0f)
                    t.radarBg = f / 100.0f;
                else
                    std::fprintf(stderr, "[gt4-hud] GT4_RADAR_BG=%s not understood (0 to 100): the radar's see-through\n", b.c_str());
            }
            if (const std::string c = get("GT4_RADAR_COLOURS"); !c.empty())
            {
                const std::vector<std::string> p = fields(c.c_str());
                for (size_t k = 0; k < 5u && k < p.size(); ++k)
                {
                    std::string h = p[k];
                    if (!h.empty() && h[0] == '#')
                        h.erase(0, 1);
                    char *end = nullptr;
                    const unsigned long v = std::strtoul(h.c_str(), &end, 16);
                    if (h.size() == 6u && end && !*end)
                        t.radarCol[k] = static_cast<uint32_t>(v);
                    else
                        std::fprintf(stderr, "[gt4-hud] GT4_RADAR_COLOURS: colour %zu '%s' not understood (RRGGBB): its default\n", k + 1, p[k].c_str());
                }
            }
            // The zoom button: a letter or a digit (raylib's KEY_A..KEY_Z, KEY_ZERO..KEY_NINE are their ASCII capitals), none.
            if (const char *k = std::getenv("GT4_HUD_ZOOM_KEY"))
            {
                const std::string v = get("GT4_HUD_ZOOM_KEY");
                if (v == "none" || v.empty())
                    t.zoomKey = 0;
                else if (v.size() == 1u && ((v[0] >= 'a' && v[0] <= 'z') || (v[0] >= '0' && v[0] <= '9')))
                    t.zoomKey = v[0] >= 'a' ? v[0] - 32 : v[0];
                else
                    std::fprintf(stderr, "[gt4-hud] GT4_HUD_ZOOM_KEY=%s not understood (a-z, 0-9, none): z\n", k);
            }
            if (const std::string pd = get("GT4_HUD_ZOOM_PAD"); !pd.empty() && pd != "none")
            {
                if (number(pd, f) && f >= 1.0f && f <= 17.0f)
                    t.zoomPad = static_cast<int>(f);
                else
                    std::fprintf(stderr, "[gt4-hud] GT4_HUD_ZOOM_PAD=%s not understood (none, 1 to 17): none\n", pd.c_str());
            }
        }

        const Settings &settings()
        {
            static const Settings s = [] {
                Settings t;
                t.w[Radar] = parse(Radar, std::getenv("GT4_HUD_RADAR"));
                t.w[Delta] = parse(Delta, std::getenv("GT4_HUD_DELTA"));
                t.w[Pedals] = parse(Pedals, std::getenv("GT4_HUD_PEDALS"));
                t.w[GMeter] = parse(GMeter, std::getenv("GT4_HUD_GMETER"));
                t.w[Tower] = parse(Tower, std::getenv("GT4_HUD_TOWER"));
                t.w[Map] = parse(Map, std::getenv("GT4_HUD_MAP"));
                t.w[Splits] = parse(Splits, std::getenv("GT4_HUD_SPLITS"));
                t.w[Revs] = parse(Revs, std::getenv("GT4_HUD_REVS"));
                t.w[Speed] = parse(Speed, std::getenv("GT4_HUD_SPEED"));
                t.w[Gear] = parse(Gear, std::getenv("GT4_HUD_GEAR"));
                t.w[LapBox] = parse(LapBox, std::getenv("GT4_HUD_LAP"));
                const char *l = std::getenv("GT4_START_LIGHTS");
                t.lights = l && (*l == '1' || std::strcmp(l, "on") == 0);
                const char *n = std::getenv("GT4_START_COUNT");
                t.count = n && *n ? std::clamp(std::atoi(n), 3, 5) : 5;
                // GT4_START_LIGHTS_BOX=<x %>,<y %>,<size %>: the lights' centre in % of the HUD area and their size (50,7.5,100:
                // the top centre); absent: the row at the top centre as before.
                if (const char *b = std::getenv("GT4_START_LIGHTS_BOX"); b && *b)
                {
                    const std::vector<std::string> f = fields(b);
                    float x = 50.0f, y = 7.5f, size = 100.0f;
                    if (f.size() == 3u && number(f[0], x) && number(f[1], y) && number(f[2], size))
                    {
                        t.lightsBox = true;
                        t.lx = std::clamp(x, 0.0f, 100.0f) / 100.0f, t.ly = std::clamp(y, 0.0f, 100.0f) / 100.0f;
                        t.lsize = std::clamp(size, 40.0f, 250.0f) / 100.0f;
                    }
                    else
                        std::fprintf(stderr, "[gt4-hud] GT4_START_LIGHTS_BOX=%s not understood (<x>,<y>,<size>): the top centre\n", b);
                }
                // GT4_START_LOOK=row | two (two rows of lights, both lit together, as GT3 has them)
                const char *look = std::getenv("GT4_START_LOOK");
                t.twoRows = look && std::strcmp(look, "two") == 0;
                readRadar(t);
                std::string on;
                for (int i = 0; i < WidgetCount; ++i)
                    if (t.w[i].on)
                    {
                        on += (on.empty() ? "" : ", ") + std::string(kKeys[i] + 8);
                        char where[64];
                        if (t.w[i].xy)
                            std::snprintf(where, sizeof(where), " at %.1f,%.1f %d%%%s", t.w[i].x * 100.0f, t.w[i].y * 100.0f,
                                          static_cast<int>(std::lround(t.w[i].size * 100.0f)), t.w[i].style ? " style 2" : "");
                        else
                            std::snprintf(where, sizeof(where), " place %d%s", t.w[i].place, t.w[i].style ? " style 2" : "");
                        on += where;
                    }
                if (!on.empty() || t.lights)
                    std::fprintf(stderr, "[gt4-hud] GT4 own race HUD (hud-4): %s%s\n", on.empty() ? "no widgets" : on.c_str(),
                                 t.lights ? (", start lights x" + std::to_string(t.count)).c_str() : "");
                if (t.w[Radar].on)
                {
                    std::string z;
                    for (float r : t.radarZoom)
                        z += (z.empty() ? "" : ",") + std::to_string(static_cast<int>(r));
                    std::fprintf(stderr, "[gt4-hud] radar: show %s, range %.0f m, zoom %s, labels %s, cars %.0f%%, background %.0f%%, colours %06x,%06x,%06x,%06x,%06x; zoom key %d, pad button %d\n",
                                 t.radarNear ? "near" : "always", t.radarRange, z.empty() ? "off" : z.c_str(), t.radarLabels ? "on" : "off", t.radarCars * 100.0f,
                                 (t.radarBg < 0.0f ? t.w[Radar].alpha : t.radarBg) * 100.0f,
                                 t.radarCol[0], t.radarCol[1], t.radarCol[2], t.radarCol[3], t.radarCol[4], t.radarZoom.empty() ? 0 : t.zoomKey,
                                 t.radarZoom.empty() ? -1 : t.zoomPad);
                }
                return t;
            }();
            return s;
        }

        Color withAlpha(Color c, float a)
        {
            c.a = static_cast<unsigned char>(std::clamp(a, 0.0f, 1.0f) * 255.0f);
            return c;
        }
        // A box drawn: its rectangle for the Vulkan presenter's overlay.
        void mark(const Rectangle &r) { ps2x::overlay::mark(r.x, r.y, r.width, r.height); }

        // ---- The race's own bits kept between frames: the player's lap traces (delta), the g trail, the start.
        struct Keep
        {
            uint64_t frames = 0;
            uint32_t course = ~0u;
            int lapsDone = -1;
            std::vector<std::pair<float, uint32_t>> cur, best; // (lap distance m, lap time ms)
            uint32_t bestMs = 0;
            std::vector<Vector2> trail;
            bool holdSeen = false;  // a standing start's countdown seen this race
            double outAt = -1.0;    // GetTime() when it reached 0
            int lastHold = 0;
            // (HUD hand-off 7 C) the timing tower's gaps, as hud/pages/telemetry.html works them out: each car's progress
            // history (lap, lap distance, race clock), then the time another car was at the same progress.
            struct Hist
            {
                int lap;
                float dist;
                uint32_t clock;
            };
            std::vector<Hist> hist[gt4race::kMaxCars];
            float maxDist = 0.0f;
            // The race map without course art: the course the cars drive, x / y every 4 m of lap distance (smoothed).
            std::vector<Vector2> outline;
            std::vector<uint8_t> outlineSeen;
            // The rev lights: the last 0.5 s of the player's rpm, and the highest rpm held for those 0.5 s this race.
            float rpmWin[30] = {};
            int rpmAt = 0, rpmN = 0;
            float rpmLimit = 0.0f;
            // The split times: this lap's sectors as each timing line is passed (ms, 0: not yet) with their colours (0
            // purple, 1 green, 2 yellow), and the lap before's, shown for 4 s after the line.
            int splitLap = -1, splitSector = 0;
            uint32_t split[gt4race::kMaxSectors] = {};
            int splitCol[gt4race::kMaxSectors] = {};
            uint32_t prevSplit[gt4race::kMaxSectors] = {};
            int prevCol[gt4race::kMaxSectors] = {};
            double prevUntil = -1.0;
            int prevCount = 0;
            // The radar (HUD hand-off 7 D / 9): its range (0: not set yet this race, GT4_RADAR_RANGE), the ease from the range
            // before a zoom (0.25 s), the pill's end, and "when cars are near": how shown it is (0..1), when a car was last in range.
            float radarRange = 0.0f, radarFrom = 0.0f;
            double radarEaseAt = -1.0, radarPillUntil = -1.0;
            float radarVis = 0.0f;
            double radarNearAt = -1e9, radarLastT = -1.0;
        } g;
        std::atomic<int> g_zoomPresses{0};

        float courseLen(const gt4race::State &s) { return s.courseLength > 0.0f ? s.courseLength : std::max(g.maxDist, 1.0f); }
        double progress(const gt4race::State &s, int lap, float dist) { return static_cast<double>(std::max(lap, 1) - 1) * courseLen(s) + dist; }
        // When the car reached progress p (race clock ms; -1: not in its history).
        double timeAtProgress(const gt4race::State &s, int car, double p)
        {
            const auto &h = g.hist[car];
            if (h.empty())
                return -1.0;
            auto P = [&](size_t i) { return progress(s, h[i].lap, h[i].dist); };
            size_t lo = 0, hi = h.size() - 1;
            if (p < P(0) || p > P(hi))
                return -1.0;
            while (hi - lo > 1)
            {
                const size_t m = (lo + hi) / 2;
                (P(m) <= p ? lo : hi) = m;
            }
            const double p0 = P(lo), p1 = P(hi), u = p1 > p0 ? (p - p0) / (p1 - p0) : 0.0;
            return h[lo].clock + (static_cast<double>(h[hi].clock) - h[lo].clock) * u;
        }
        // A split's colour: 0 purple (as quick as every car's best for that sector this race), 1 green (your own best), 2
        // yellow (slower than your best).
        int splitColour(const gt4race::State &s, int me, int k, uint32_t t)
        {
            uint32_t all = 0;
            for (int i = 0; i < s.carCount; ++i)
                if (const uint32_t b = s.cars[i].bestSectorMs[k]; b > 0u && (all == 0u || b < all))
                    all = b;
            const uint32_t mine = s.cars[me].bestSectorMs[k];
            if (all == 0u || t <= all)
                return 0;
            return mine == 0u || t <= mine ? 1 : 2;
        }

        // Stage C's per-frame bits: the cars' progress history, the course outline, the rpm window, the split times.
        void trackBoxes(const gt4race::State &s, int pi)
        {
            for (int i = 0; i < s.carCount; ++i)
            {
                const gt4race::Car &c = s.cars[i];
                if (s.courseLength <= 0.0f)
                    g.maxDist = std::max(g.maxDist, c.lapDistance);
                auto &h = g.hist[i];
                if (c.lap >= 1 && (h.empty() || s.clockMs > h.back().clock))
                {
                    if (h.empty() || progress(s, c.lap, c.lapDistance) >= progress(s, h.back().lap, h.back().dist))
                        h.push_back(Keep::Hist{c.lap, c.lapDistance, s.clockMs});
                    if (h.size() > 40000u)
                        h.erase(h.begin(), h.begin() + 10000);
                }
                // (the course the cars drive, for a course without art)
                if (c.lap >= 1 && c.lapDistance >= 0.0f && c.lapDistance < 20000.0f)
                {
                    const size_t b = static_cast<size_t>(c.lapDistance / 4.0f);
                    if (b >= g.outline.size())
                        g.outline.resize(b + 1), g.outlineSeen.resize(b + 1, 0);
                    if (g.outlineSeen[b])
                        g.outline[b].x += (c.x - g.outline[b].x) * 0.05f, g.outline[b].y += (c.y - g.outline[b].y) * 0.05f;
                    else
                        g.outline[b] = Vector2{c.x, c.y}, g.outlineSeen[b] = 1;
                }
            }
            // The rev limit (until GT4's own is found): the highest rpm held for 0.5 s this race (the lowest of the last 30
            // frames, at its highest).
            g.rpmWin[g.rpmAt] = s.player.rpm;
            g.rpmAt = (g.rpmAt + 1) % 30;
            g.rpmN = std::min(g.rpmN + 1, 30);
            if (g.rpmN == 30)
                g.rpmLimit = std::max(g.rpmLimit, *std::min_element(std::begin(g.rpmWin), std::end(g.rpmWin)));
            // The split times: a timing line passed (car+0x5b2 up by one) gives the sector just done (GT4's time for it, else
            // the lap time so far less the sectors before); the finish line gives the last sector (the lap time less the others).
            const gt4race::Car &me = s.cars[pi];
            const int n = std::clamp(s.sectorCount, 1, gt4race::kMaxSectors);
            if (g.splitLap < 0)
                g.splitLap = me.lap, g.splitSector = me.sector;
            if (me.lap != g.splitLap)
            {
                if (g.splitLap >= 1 && me.lap == g.splitLap + 1 && me.lastLapMs > 0u)
                {
                    uint32_t before = 0;
                    for (int k = 0; k < n - 1; ++k)
                        before += g.split[k];
                    const int last = n - 1;
                    if (g.split[last] == 0u && me.lastLapMs > before)
                    {
                        g.split[last] = me.sectorMs[last] > 0u && me.sectorMs[last] + before == me.lastLapMs ? me.sectorMs[last] : me.lastLapMs - before;
                        g.splitCol[last] = splitColour(s, pi, last, g.split[last]);
                    }
                    std::copy(std::begin(g.split), std::end(g.split), std::begin(g.prevSplit));
                    std::copy(std::begin(g.splitCol), std::end(g.splitCol), std::begin(g.prevCol));
                    g.prevCount = n;
                    g.prevUntil = GetTime() + 4.0;
                }
                std::fill(std::begin(g.split), std::end(g.split), 0u);
                g.splitLap = me.lap, g.splitSector = 0;
            }
            while (g.splitSector < me.sector && g.splitSector < n - 1)
            {
                const int k = g.splitSector++;
                uint32_t before = 0;
                for (int j = 0; j < k; ++j)
                    before += g.split[j];
                const uint32_t t = me.sectorMs[k] > 0u ? me.sectorMs[k] : me.lapTimeMs > before ? me.lapTimeMs - before : 0u;
                g.split[k] = t;
                g.splitCol[k] = t ? splitColour(s, pi, k, t) : 2;
                static int said = 0; // (tests: the first splits, against GT4's own numbers)
                if (said < 12 && std::getenv("GT4_HUD_SPLITS_LOG"))
                    ++said, std::fprintf(stderr, "[gt4-hud] split: lap %d sector %d %u ms (GT4 %u, lap time %u, best %u) colour %d\n", me.lap, k + 1, t,
                                         me.sectorMs[k], me.lapTimeMs, me.bestSectorMs[k], g.splitCol[k]);
            }
        }

        void track(const gt4race::State &s)
        {
            if (s.raceFrames == g.frames)
                return;
            const bool newRace = s.raceFrames < g.frames || s.course != g.course || s.phase == gt4race::Phase::PreRace;
            g.frames = s.raceFrames;
            if (newRace)
            {
                const uint32_t course = s.course;
                g = Keep{};
                g.frames = s.raceFrames;
                g.course = course;
            }
            // The start: the countdown (frames left) going to 0 on a standing start.
            if (s.startHold > 0)
                g.holdSeen = true, g.outAt = -1.0;
            else if (g.holdSeen && g.lastHold > 0 && g.outAt < 0.0)
                g.outAt = GetTime();
            g.lastHold = s.startHold;
            const int pi = s.player.index;
            if (pi < 0 || pi >= s.carCount)
                return;
            const gt4race::Car &me = s.cars[pi];
            // Delta: the lap just done becomes the best trace when it is the best so far.
            if (g.lapsDone >= 0 && me.lapsDone > g.lapsDone)
            {
                const uint32_t t = me.lastLapMs;
                if (t > 0u && g.cur.size() > 20u && (g.best.empty() || t < g.bestMs))
                    g.best = g.cur, g.bestMs = t;
                g.cur.clear();
            }
            g.lapsDone = me.lapsDone;
            if (me.lap >= 1 && me.lapTimeMs > 0u && !me.finished && (g.cur.empty() || me.lapDistance >= g.cur.back().first))
                g.cur.emplace_back(me.lapDistance, me.lapTimeMs);
            // The g trail (the force the driver feels: right = pushed right, up = pushed forward).
            g.trail.push_back(Vector2{-s.player.accelLat / 9.81f, -s.player.accelLong / 9.81f});
            if (g.trail.size() > 30u)
                g.trail.erase(g.trail.begin());
            trackBoxes(s, pi);
        }
        // The time on the best lap's trace at this distance (linear between its points; -1: none).
        double bestAt(float dist)
        {
            const auto &b = g.best;
            if (b.size() < 2u || dist < b.front().first || dist > b.back().first)
                return -1.0;
            const auto it = std::lower_bound(b.begin(), b.end(), dist, [](const std::pair<float, uint32_t> &p, float d) { return p.first < d; });
            if (it == b.begin())
                return it->second;
            const auto lo = it - 1;
            const float span = it->first - lo->first;
            const double f = span > 1e-4f ? (dist - lo->first) / span : 0.0;
            return lo->second + f * (static_cast<double>(it->second) - lo->second);
        }

        // ---- Stage C (HUD hand-off 7 C): the timing tower, the race map, the split times, the rev lights, speed, gear, lap.
        Color rgb(uint32_t v, float a = 1.0f) { return Color{static_cast<unsigned char>(v >> 16), static_cast<unsigned char>(v >> 8), static_cast<unsigned char>(v), static_cast<unsigned char>(std::clamp(a, 0.0f, 1.0f) * 255.0f)}; }
        Color carColour(const gt4race::Car &c, int i) { return rgb(c.player ? gt4hudbox::kMe : gt4hudbox::kCarColours[i % 16]); }
        // m'ss.sss (a lap or the race clock)
        std::string lapText(uint32_t ms)
        {
            if (ms == 0u)
                return "-'--.---";
            char t[32];
            std::snprintf(t, sizeof(t), "%u'%02u.%03u", ms / 60000u, ms / 1000u % 60u, ms % 1000u);
            return t;
        }
        // +s.sss, +m'ss.sss from a minute
        std::string gapText(double ms)
        {
            char t[32];
            if (ms >= 60000.0)
                return "+" + lapText(static_cast<uint32_t>(ms));
            std::snprintf(t, sizeof(t), "+%.3f", std::max(0.0, ms) / 1000.0);
            return t;
        }
        // A sector time: s.sss, or m'ss.sss from a minute.
        std::string splitText(uint32_t ms)
        {
            if (ms >= 60000u)
                return lapText(ms);
            char t[32];
            std::snprintf(t, sizeof(t), "%u.%03u", ms / 1000u, ms % 1000u);
            return t;
        }
        // A three-letter code from a car's name: the maker's first letter and the model's first two (Ford Ka -> FKA, VW Beetle ->
        // VBE); a one-word name: its first three.
        std::string shortCode(const char *name)
        {
            std::vector<std::string> words(1);
            for (const char *c = name; *c; ++c)
            {
                const char ch = *c;
                if ((ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9'))
                    words.back() += static_cast<char>(ch >= 'a' && ch <= 'z' ? ch - 32 : ch);
                else if (!words.back().empty())
                    words.emplace_back();
            }
            if (words.back().empty())
                words.pop_back();
            std::string code;
            if (words.size() >= 2u)
                code = words[0].substr(0, 1) + words[1].substr(0, 2);
            else if (!words.empty())
                code = words[0].substr(0, 3);
            return code.empty() ? std::string("CAR") : code;
        }
        // The tower's "driver" name: the driver, two spaces, the car's short code ("Kenji Arata  FKA"); shortened to fit the name
        // column as "K. Arata  FKA", then "Arata  FKA", then the driver alone.
        std::string towerDriverName(const std::string &driver, const std::string &code, float fs, float width)
        {
            const size_t sp = driver.find_last_of(' ');
            const std::string surname = sp == std::string::npos ? driver : driver.substr(sp + 1);
            const std::string tries[3] = {driver, sp == std::string::npos ? driver : driver.substr(0, 1) + ". " + surname, surname};
            for (const std::string &d : tries)
                if (gt4hudlook::textWidth(d + "  " + code, fs) <= width)
                    return d + "  " + code;
            return driver;
        }
        // ---- The timing tower: place, name, then the columns picked; the rows kept around you.
        void drawTower(const Rectangle &r, float alpha, const gt4race::State &s, const Spec &sp, float H)
        {
            const float k = H * sp.size;
            // The cars by place.
            int order[gt4race::kMaxCars];
            const int n = s.carCount;
            for (int i = 0; i < n; ++i)
                order[i] = i;
            std::sort(order, order + n, [&](int a, int b) { return s.cars[a].place < s.cars[b].place; });
            int mePos = 0;
            for (int j = 0; j < n; ++j)
                if (s.cars[order[j]].player)
                    mePos = j;
            const int rows = std::min(sp.towerRows, n);
            const int first = std::clamp(mePos - rows / 2, 0, std::max(0, n - rows));
            const gt4race::Car *leader = n ? &s.cars[order[0]] : nullptr;
            uint32_t fastest = 0;
            for (int i = 0; i < n; ++i)
                if (s.cars[i].bestLapMs > 0u && (fastest == 0u || s.cars[i].bestLapMs < fastest))
                    fastest = s.cars[i].bestLapMs;
            const double L = courseLen(s);
            gt4hudlook::TowerRow shown[gt4race::kMaxCars];
            for (int row = 0; row < rows; ++row)
            {
                const int j = first + row, ci = order[j];
                const gt4race::Car &c = s.cars[ci];
                gt4hudlook::TowerRow &t = shown[row];
                t.place = c.place, t.car = ci, t.player = c.player;
                t.purpleBest = c.bestLapMs > 0u && c.bestLapMs == fastest;
                // (driver: the AI cars' drivers the Race Lab sent (gt4_ai_drivers.h) and "You", each with its car's short code after
                // it; an AI car the Race Lab named no driver for keeps the car's name. LAN names are not read yet.)
                // (fitted to the width gt4hudlook::tower draws the name in: the column less a tenth of the dot's and half the pad)
                // (21 Oct, developer: the drivers did not show with Names on Car, the default: an AI car the Race Lab named a driver for
                // shows its driver and car code there too; Short keeps the codes, its column too narrow for names)
                const std::string drv = sp.towerName == 2 ? (c.player ? std::string("You") : gt4drivers::asciiName(gt4drivers::driverOf(s, ci)))
                                        : sp.towerName == 0 && !c.player ? gt4drivers::asciiName(gt4drivers::driverOf(s, ci))
                                                                         : std::string();
                t.name = !drv.empty() ? towerDriverName(drv, shortCode(c.name), gt4hudbox::kTowerRow * k * 0.5f,
                                                        (gt4hudbox::kTowerNameW[std::clamp(sp.towerName, 0, 2)] - gt4hudbox::kTowerDot * 0.1f - gt4hudbox::kTowerPad * 0.5f) * k)
                         : sp.towerName == 1 ? shortCode(c.name) : std::string(c.name);
                // Gap to the leader and to the car ahead (the page's maths: the time at the same progress; finished cars by total time).
                std::string gap, iv;
                if (j == 0)
                    gap = "LEAD";
                else if (leader)
                {
                    const gt4race::Car &a = s.cars[order[j - 1]];
                    if (c.finished && leader->finished && c.totalMs > 0u && leader->totalMs > 0u)
                    {
                        gap = gapText(static_cast<double>(c.totalMs) - leader->totalMs);
                        if (a.finished && a.totalMs > 0u)
                            iv = gapText(static_cast<double>(c.totalMs) - a.totalMs);
                    }
                    else
                    {
                        const double pc = progress(s, c.lap, c.lapDistance), pl = progress(s, leader->lap, leader->lapDistance);
                        if (pl - pc >= L)
                            gap = "+" + std::to_string(static_cast<int>((pl - pc) / L)) + " LAP";
                        else if (const double t0 = timeAtProgress(s, order[0], pc); t0 >= 0.0)
                            gap = gapText(s.clockMs - t0);
                        const double ta = timeAtProgress(s, order[j - 1], pc);
                        if (ta >= 0.0 && progress(s, a.lap, a.lapDistance) - pc < L)
                            iv = gapText(s.clockMs - ta);
                    }
                }
                t.vals[0] = gap, t.vals[1] = iv, t.vals[2] = lapText(c.bestLapMs), t.vals[3] = lapText(c.lastLapMs);
            }
            gt4hudlook::tower(r, alpha, k, sp.towerName, sp.towerCols, shown, rows);
            mark(Rectangle{r.x, r.y, r.width, std::min(r.height, (gt4hudbox::kTowerHead + gt4hudbox::kTowerRow * static_cast<float>(rows)) * k)});
        }

        // ---- The race map's course art: pages/tracks/<course>.json (GT4's own course data; hud/pages/dev/make_track.py),
        // read on a thread of its own, the picture made a texture here.
        struct Art
        {
            uint32_t course = ~0u;
            bool done = false, ok = false;
            float w = 0, h = 0;
            std::vector<Vector2> line; // the course in picture pixels, evenly along the lap
            Image img{};
            Texture2D tex{};
        };
        std::mutex g_artMutex;
        Art g_art;
        std::string pagesDir()
        {
            if (const char *d = std::getenv("GT4_PAGES_DIR"); d && *d)
                return d;
#if defined(_WIN32)
            char path[1024] = {};
            GetModuleFileNameA(nullptr, path, sizeof(path));
            std::string p = path;
            const size_t cut = p.find_last_of("\\/");
            return (cut == std::string::npos ? std::string(".") : p.substr(0, cut)) + "/pages";
#else
            return "pages";
#endif
        }
        std::vector<uint8_t> base64(const std::string &t, size_t from)
        {
            std::vector<uint8_t> out;
            out.reserve((t.size() - from) * 3 / 4);
            uint32_t acc = 0;
            int bits = 0;
            for (size_t i = from; i < t.size() && t[i] != '"'; ++i)
            {
                const char c = t[i];
                int v = c >= 'A' && c <= 'Z' ? c - 'A' : c >= 'a' && c <= 'z' ? c - 'a' + 26 : c >= '0' && c <= '9' ? c - '0' + 52 : c == '+' ? 62 : c == '/' ? 63 : -1;
                if (v < 0)
                    continue;
                acc = (acc << 6) | static_cast<uint32_t>(v);
                bits += 6;
                if (bits >= 8)
                    bits -= 8, out.push_back(static_cast<uint8_t>(acc >> bits));
            }
            return out;
        }
        void loadArt(uint32_t course)
        {
            Art a;
            a.course = course;
            const std::string file = pagesDir() + "/tracks/" + std::to_string(course) + ".json";
            std::ifstream in(file, std::ios::binary);
            std::stringstream ss;
            ss << in.rdbuf();
            const std::string t = ss.str();
            auto num = [&](const char *key, float &out) {
                const size_t k = t.find(std::string("\"") + key + "\":");
                if (k == std::string::npos)
                    return false;
                out = std::strtof(t.c_str() + k + std::strlen(key) + 3, nullptr);
                return true;
            };
            const size_t ln = t.find("\"line\":");
            const size_t im = t.find("base64,");
            if (!t.empty() && num("w", a.w) && num("h", a.h) && ln != std::string::npos && im != std::string::npos && a.w > 0 && a.h > 0)
            {
                const size_t end = t.find("]]", ln);
                const char *c = t.c_str() + ln + 7;
                const char *stop = end == std::string::npos ? c : t.c_str() + end;
                while (c < stop)
                {
                    while (c < stop && !(*c == '-' || (*c >= '0' && *c <= '9')))
                        ++c;
                    char *e = nullptr;
                    const float x = std::strtof(c, &e);
                    if (!e || e == c)
                        break;
                    c = e;
                    while (c < stop && !(*c == '-' || (*c >= '0' && *c <= '9')))
                        ++c;
                    const float y = std::strtof(c, &e);
                    if (!e || e == c)
                        break;
                    c = e;
                    a.line.push_back(Vector2{x, y});
                }
                const std::vector<uint8_t> jpg = base64(t, im + 7);
                if (a.line.size() >= 8u && !jpg.empty())
                {
                    a.img = LoadImageFromMemory(".jpg", jpg.data(), static_cast<int>(jpg.size()));
                    a.ok = a.img.data != nullptr;
                }
            }
            std::fprintf(stderr, "[gt4-hud] race map: %s %s\n", file.c_str(),
                         a.ok ? ("read (" + std::to_string(a.line.size()) + " points, " + std::to_string(a.img.width) + "x" + std::to_string(a.img.height) + ")").c_str()
                              : "not there or not understood: the outline the cars drive");
            a.done = true;
            std::lock_guard<std::mutex> lock(g_artMutex);
            if (g_art.course == course)
                g_art = std::move(a);
            else if (a.img.data)
                UnloadImage(a.img);
        }
        // The art for this course (presenter thread): null until it is read, or when there is none.
        const Art *art(uint32_t course)
        {
            std::lock_guard<std::mutex> lock(g_artMutex);
            if (g_art.course != course)
            {
                if (g_art.tex.id)
                    UnloadTexture(g_art.tex);
                if (g_art.img.data)
                    UnloadImage(g_art.img);
                g_art = Art{};
                g_art.course = course;
                std::thread(loadArt, course).detach();
                return nullptr;
            }
            if (!g_art.done || !g_art.ok)
                return nullptr;
            if (!g_art.tex.id && g_art.img.data)
            {
                g_art.tex = LoadTextureFromImage(g_art.img);
                SetTextureFilter(g_art.tex, TEXTURE_FILTER_BILINEAR);
                SetTextureWrap(g_art.tex, TEXTURE_WRAP_CLAMP);
                UnloadImage(g_art.img);
                g_art.img = Image{};
            }
            return g_art.tex.id ? &g_art : nullptr;
        }
        // A point along the art's line at a lap fraction (0..1), as the page's at().
        Vector2 artAt(const Art &a, float f)
        {
            const size_t n = a.line.size();
            const float x = (f - std::floor(f)) * static_cast<float>(n);
            const size_t i = static_cast<size_t>(x) % n, j = (i + 1) % n;
            const float u = x - std::floor(x);
            return Vector2{a.line[i].x + (a.line[j].x - a.line[i].x) * u, a.line[i].y + (a.line[j].y - a.line[i].y) * u};
        }

        // ---- The race map: locked (the whole course, north up) or rotating (a round box around you, the way you go up).
        // Without course art: is the outline the cars drive known for (nearly) the whole lap? Until then the map draws
        // nothing (a line growing behind the cars looked broken during the first lap).
        bool outlineKnown(const gt4race::State &s)
        {
            if (s.courseLength <= 0.0f)
                return false;
            const size_t n = std::min(g.outlineSeen.size(), static_cast<size_t>(s.courseLength / 4.0f));
            if (n < 8u)
                return false;
            size_t seen = 0;
            for (size_t b = 0; b < n; ++b)
                seen += g.outlineSeen[b] ? 1u : 0u;
            return seen * 100u >= static_cast<size_t>(s.courseLength / 4.0f) * 97u;
        }

        // simple (HUD hand-off 10): the art's line (the phone's Simple map's) instead of the picture; dots and turning the same.
        void drawMap(const Rectangle &r, float alpha, const gt4race::State &s, int pi, int style, bool simple)
        {
            const Art *a = art(s.course);
            if (!a && !outlineKnown(s))
                return;
            const float L = static_cast<float>(courseLen(s));
            auto frac = [&](const gt4race::Car &c) { const float f = c.lapDistance / L; return f - std::floor(f); };
            const gt4race::Car &me = s.cars[pi];
            const float dotR = std::max(4.0f, std::min(r.width, r.height) * (style == 1 ? 0.045f : 0.05f));
            // Where each place is drawn: the art's pixels (or the world's metres without art) to the box.
            std::function<Vector2(Vector2)> toBox;
            std::function<Vector2(const gt4race::Car &)> carAt;
            const Vector2 c0{r.x + r.width * 0.5f, r.y + r.height * 0.5f};
            const float R = r.width * 0.5f;
            if (style == 1)
            {
                // Round: the box's disc (GT3's black), you 62 % down it.
                gt4hudlook::circle(c0, R, gt4hudlook::fade(BLACK, alpha));
                mark(r);
                const Vector2 anchor{c0.x, r.y + r.height * 0.62f};
                float ang = 0.0f, sc = 1.0f;
                Vector2 P{};
                if (a)
                {
                    P = artAt(*a, frac(me));
                    const Vector2 Q = artAt(*a, frac(me) + 0.012f);
                    ang = -1.5707963f - std::atan2(Q.y - P.y, Q.x - P.x);
                    sc = r.width / (a->w * 0.3f);
                }
                else
                {
                    // (no art: world metres, north up before the turn; about 30 % of the course's width across the box)
                    float x0 = 1e9f, x1 = -1e9f;
                    for (size_t b = 0; b < g.outline.size(); ++b)
                        if (g.outlineSeen[b])
                            x0 = std::min(x0, g.outline[b].x), x1 = std::max(x1, g.outline[b].x);
                    P = Vector2{me.x, -me.y};
                    ang = -1.5707963f - std::atan2(-std::sin(me.yaw), std::cos(me.yaw));
                    sc = r.width / std::max(60.0f, (x1 - x0) * 0.3f);
                }
                const float ca = std::cos(ang), sa = std::sin(ang);
                toBox = [=](Vector2 p) {
                    const float dx = (p.x - P.x) * sc, dy = (p.y - P.y) * sc;
                    return Vector2{anchor.x + dx * ca - dy * sa, anchor.y + dx * sa + dy * ca};
                };
                if (a && !simple)
                {
                    // The art as a textured disc (the inverse turn per rim point): no clipping needed.
                    rlDrawRenderBatchActive();
                    rlDisableBackfaceCulling();
                    rlBegin(RL_TRIANGLES);
                    rlSetTexture(a->tex.id); // (after rlBegin: a new mode's draw call starts with the default texture)
                    rlColor4ub(255, 255, 255, static_cast<unsigned char>(std::clamp(0.35f + alpha, 0.0f, 1.0f) * 255.0f));
                    auto uv = [&](Vector2 q) {
                        const float dx = q.x - anchor.x, dy = q.y - anchor.y;
                        const float px = (dx * ca + dy * sa) / sc + P.x, py = (-dx * sa + dy * ca) / sc + P.y;
                        rlTexCoord2f(px / a->w, py / a->h);
                        rlVertex2f(q.x, q.y);
                    };
                    const int seg = 64;
                    const float rr = R * 0.97f;
                    for (int k2 = 0; k2 < seg; ++k2)
                    {
                        const float t0 = 6.2831853f * static_cast<float>(k2) / seg, t1 = 6.2831853f * static_cast<float>(k2 + 1) / seg;
                        uv(c0);
                        uv(Vector2{c0.x + rr * std::cos(t0), c0.y + rr * std::sin(t0)});
                        uv(Vector2{c0.x + rr * std::cos(t1), c0.y + rr * std::sin(t1)});
                    }
                    rlEnd();
                    rlSetTexture(0);
                    rlDrawRenderBatchActive();
                    rlEnableBackfaceCulling();
                }
                gt4hudlook::ring(c0, R, std::max(1.5f, R * 0.02f), gt4hudlook::fade(WHITE, 0.35f));
            }
            else
            {
                gt4hudlook::panel(r, alpha);
                mark(r);
                if (a)
                {
                    const float pad = std::min(r.width, r.height) * 0.04f;
                    const float sc = std::min((r.width - pad * 2.0f) / a->w, (r.height - pad * 2.0f) / a->h);
                    const float ox = r.x + (r.width - a->w * sc) * 0.5f, oy = r.y + (r.height - a->h * sc) * 0.5f;
                    if (!simple)
                        DrawTexturePro(a->tex, Rectangle{0, 0, static_cast<float>(a->tex.width), static_cast<float>(a->tex.height)},
                                       Rectangle{ox, oy, a->w * sc, a->h * sc}, Vector2{0, 0}, 0.0f, withAlpha(WHITE, std::clamp(0.35f + alpha, 0.0f, 1.0f)));
                    toBox = [=](Vector2 p) { return Vector2{ox + p.x * sc, oy + p.y * sc}; };
                }
                else
                {
                    // (no art: the outline the cars drive, north up: world y up the box)
                    float x0 = 1e9f, y0 = 1e9f, x1 = -1e9f, y1 = -1e9f;
                    auto add = [&](float x, float y) { x0 = std::min(x0, x), x1 = std::max(x1, x), y0 = std::min(y0, y), y1 = std::max(y1, y); };
                    for (size_t b = 0; b < g.outline.size(); ++b)
                        if (g.outlineSeen[b])
                            add(g.outline[b].x, g.outline[b].y);
                    for (int i = 0; i < s.carCount; ++i)
                        add(s.cars[i].x, s.cars[i].y);
                    const float pad = dotR * 2.4f;
                    const float sc = std::min((r.width - pad * 2.0f) / std::max(x1 - x0, 1.0f), (r.height - pad * 2.0f) / std::max(y1 - y0, 1.0f));
                    const float ox = r.x + (r.width - (x1 - x0) * sc) * 0.5f, oy = r.y + (r.height - (y1 - y0) * sc) * 0.5f;
                    toBox = [=](Vector2 p) { return Vector2{ox + (p.x - x0) * sc, oy + (y1 + p.y) * sc}; }; // (p.y = -world y)
                }
            }
            if (a && simple)
            {
                // The centreline, closed: a dark edge under a light grey stroke about 2.5 % of the box high (the rotating map's
                // segments only inside its disc).
                const float lw = std::max(2.0f, r.height * 0.025f);
                const size_t n = a->line.size();
                for (int pass = 0; pass < 2; ++pass)
                    for (size_t i = 0; i < n; ++i)
                    {
                        const Vector2 p = toBox(a->line[i]), q = toBox(a->line[(i + 1) % n]);
                        if (style == 1 && (std::hypot(p.x - c0.x, p.y - c0.y) > R * 0.97f || std::hypot(q.x - c0.x, q.y - c0.y) > R * 0.97f))
                            continue;
                        gt4hudlook::line(p, q, pass ? lw : lw + 2.0f, pass ? Color{201, 209, 219, 255} : gt4hudlook::fade(BLACK, 0.8f));
                    }
            }
            if (a)
                carAt = [&](const gt4race::Car &c) { return toBox(artAt(*a, frac(c))); };
            else
            {
                // The outline (world x, -world y), then the cars at their world places.
                const float lw = std::max(2.0f, dotR * 0.55f);
                Vector2 prev{};
                bool have = false;
                size_t lastB = 0;
                for (size_t b = 0; b < g.outline.size(); ++b)
                {
                    if (!g.outlineSeen[b])
                        continue;
                    const Vector2 q = toBox(Vector2{g.outline[b].x, -g.outline[b].y});
                    if (have && b - lastB <= 3u && (style != 1 || (std::hypot(q.x - c0.x, q.y - c0.y) < R && std::hypot(prev.x - c0.x, prev.y - c0.y) < R)))
                        gt4hudlook::line(prev, q, lw, Color{176, 182, 192, 255});
                    prev = q, have = true, lastB = b;
                }
                carAt = [&](const gt4race::Car &c) { return toBox(Vector2{c.x, -c.y}); };
            }
            // The cars, the leader last (on top), you over all of them.
            int order[gt4race::kMaxCars];
            for (int i = 0; i < s.carCount; ++i)
                order[i] = i;
            std::sort(order, order + s.carCount, [&](int x, int y) {
                if (s.cars[x].player != s.cars[y].player)
                    return !s.cars[x].player;
                return s.cars[x].place > s.cars[y].place;
            });
            for (int j = 0; j < s.carCount; ++j)
            {
                const gt4race::Car &c = s.cars[order[j]];
                const Vector2 q = carAt(c);
                const float rr = c.player ? dotR * 1.2f : dotR;
                if (style == 1 && std::hypot(q.x - c0.x, q.y - c0.y) > R - rr)
                    continue;
                // (GT3's map: dots with the place in them; you on the turning map GT3's arrow, the way you go up)
                if (c.player && style == 1)
                    gt4hudlook::mapArrow(q, dotR * 1.5f, gt4hudlook::me());
                else
                    gt4hudlook::mapDot(q, rr, carColour(c, order[j]), c.place);
            }
        }

        // ---- The split times: a cell per sector; done ones in purple / green / yellow, the current one counting up.
        void drawSplits(const Rectangle &r, float alpha, const gt4race::State &s, int pi)
        {
            const gt4race::Car &me = s.cars[pi];
            const bool showPrev = g.prevUntil > 0.0 && GetTime() < g.prevUntil && g.prevCount > 0;
            const int n = showPrev ? g.prevCount : std::clamp(s.sectorCount, 1, gt4race::kMaxSectors);
            gt4hudlook::Split cells[gt4race::kMaxSectors];
            uint32_t before = 0;
            for (int k = 0; k < n; ++k)
            {
                if (showPrev)
                    cells[k] = g.prevSplit[k] ? gt4hudlook::splitDone(splitText(g.prevSplit[k]), g.prevCol[k], true) : gt4hudlook::splitNone();
                else if (me.lap < 1)
                    cells[k] = gt4hudlook::splitNone();
                else if (k < g.splitSector && g.split[k])
                    cells[k] = gt4hudlook::splitDone(splitText(g.split[k]), g.splitCol[k]);
                else if (k == g.splitSector && !me.finished)
                    cells[k] = gt4hudlook::splitRunning(splitText(me.lapTimeMs > before ? me.lapTimeMs - before : 0u));
                else
                    cells[k] = gt4hudlook::splitNone();
                if (k < g.splitSector)
                    before += g.split[k];
            }
            gt4hudlook::splits(r, alpha, cells, n);
            mark(r);
        }

        // ---- The rev lights: 10 lights (1 at 85 % of the rev limit, 10 at 98 %), all flashing at the limiter; style 0 with a
        // bar under them, 1 the lights only.
        void drawRevs(const Rectangle &r, float alpha, const gt4race::State &s, int style)
        {
            const float limit = g.rpmLimit > 1000.0f ? g.rpmLimit : 7000.0f;
            gt4hudlook::revs(r, alpha, style, s.player.rpm / limit, std::fmod(GetTime(), 0.16) < 0.08);
            mark(r);
        }

        // ---- Speed (GT4's own unit from its Options), gear, lap.
        void drawSpeed(const Rectangle &r, float alpha, const gt4race::State &s, const gt4race::Car &me)
        {
            const bool mph = s.unitSpeed == 1;
            const int v = static_cast<int>(std::lround(std::fabs(me.speed) * (mph ? 2.23694f : 3.6f)));
            gt4hudlook::speed(r, alpha, std::to_string(v), mph ? "mph" : "km/h");
            mark(r);
        }
        void drawGear(const Rectangle &r, float alpha, const gt4race::State &s, const gt4race::Car &me)
        {
            // (GT4's gear byte: 1 = first; 0 is neutral, or reverse while rolling back; a value past 0x80 reads as reverse)
            const int gr = s.player.gear;
            const bool reverse = gr >= 0x80 || (gr == 0 && me.speed < -0.5f);
            gt4hudlook::gear(r, alpha, reverse ? "R" : gr == 0 ? "N" : std::to_string(gr), reverse);
            mark(r);
        }
        void drawLap(const Rectangle &r, float alpha, const gt4race::State &s, const gt4race::Car &me, int style)
        {
            const int laps = std::max(1, s.laps), shown = me.finished ? laps : std::clamp(me.lap, 1, laps);
            const std::string clock = lapText(s.clockMs);
            gt4hudlook::lap(r, alpha, shown, laps, style == 1 ? &clock : nullptr);
            mark(r);
        }

        // ---- The widgets (the Dash page's drawDash).
        // The radar's range now (m): GT4_RADAR_RANGE at the start of each race, eased over 0.25 s to the range a zoom stepped to.
        float radarNow(const Settings &t)
        {
            if (g.radarRange <= 0.0f)
                g.radarRange = g.radarFrom = t.radarRange;
            const double u = g.radarEaseAt < 0.0 ? 1.0 : std::clamp((GetTime() - g.radarEaseAt) / 0.25, 0.0, 1.0);
            const float e = static_cast<float>(u * u * (3.0 - 2.0 * u));
            return g.radarFrom + (g.radarRange - g.radarFrom) * e;
        }
        // The Race HUD zoom button (HUD hand-off 9): the next larger range of GT4_RADAR_ZOOM, after the largest the smallest.
        void radarZoomStep(const Settings &t, const gt4race::State &s)
        {
            if (t.radarZoom.empty())
                return;
            const float now = radarNow(t);
            float next = t.radarZoom.front();
            for (float z : t.radarZoom)
                if (z > g.radarRange + 0.01f)
                {
                    next = z;
                    break;
                }
            static int said = 0;
            if (said < 40)
                ++said, std::fprintf(stderr, "[gt4-hud] radar zoom: %.0f m -> %.0f m (vsync %llu)\n", g.radarRange, next, static_cast<unsigned long long>(s.vsync));
            g.radarFrom = now, g.radarRange = next;
            g.radarEaseAt = GetTime(), g.radarPillUntil = GetTime() + 1.5;
        }
        // style 0 round (as HUD hand-off 4 drew it), 1 square (HUD hand-off 7 A4: square range lines, the cars out to the
        // square's corners). HUD hand-off 7 D / 9: the range zooms (rings at 1/3, 2/3 and the edge, their metres right of each
        // ring's top), the page's colours and car opacity, "when cars are near".
        void drawRadar(const Rectangle &r, float alpha, const gt4race::State &s, const gt4race::Car &me, int style)
        {
            const Settings &t = settings();
            const float range = radarNow(t);
            constexpr float carL = 4.2f;
            const float W = r.width, R = W * 0.5f - W * 0.03f, sc = R / range; // (as gt4hudlook::radar draws it)
            const Vector2 c0{r.x + W * 0.5f, r.y + W * 0.5f};
            const float cy = std::cos(me.yaw), sy = std::sin(me.yaw);
            auto inRange = [&](float fwd, float left) {
                return style == 1 ? std::fabs(fwd) <= range && std::fabs(left) <= range : std::hypot(fwd, left) <= range;
            };
            // "When cars are near": in over 0.25 s while a car is inside the range now, out 2 s after the last one left.
            float vis = 1.0f;
            if (t.radarNear)
            {
                bool anyIn = false;
                for (int i = 0; i < s.carCount && !anyIn; ++i)
                {
                    const gt4race::Car &c = s.cars[i];
                    const float dx = c.x - me.x, dy = c.y - me.y;
                    anyIn = !c.player && inRange(dx * cy + dy * sy, -dx * sy + dy * cy);
                }
                const double now = GetTime();
                const float dt = g.radarLastT < 0.0 ? 0.0f : static_cast<float>(std::clamp(now - g.radarLastT, 0.0, 0.1));
                g.radarLastT = now;
                if (anyIn)
                    g.radarNearAt = now;
                const bool want = now - g.radarNearAt < 2.0;
                g.radarVis = std::clamp(g.radarVis + (want ? dt : -dt) / 0.25f, 0.0f, 1.0f);
                vis = g.radarVis;
                if (vis <= 0.0f)
                    return;
            }
            bool sideL = false, sideR = false;
            // (GT3's radar settings: the car opacity is every car's, yours 0.1 more; close cars, under 12 m, in the other cars'
            // colour: GT4_RADAR_COLOURS' third colour is read but not drawn)
            const Color you = rgb(t.radarCol[0], std::min(1.0f, t.radarCars + 0.1f) * vis), other = rgb(t.radarCol[1], t.radarCars * vis),
                        close = other, along = rgb(t.radarCol[3], t.radarCars * vis);
            gt4hudlook::RadarCar cars[gt4race::kMaxCars];
            int shownCars = 0;
            for (int i = 0; i < s.carCount; ++i)
            {
                const gt4race::Car &c = s.cars[i];
                if (c.player)
                    continue;
                const float dx = c.x - me.x, dy = c.y - me.y, fwd = dx * cy + dy * sy, left = -dx * sy + dy * cy;
                const float d = std::hypot(fwd, left);
                if (std::fabs(fwd) < carL && std::fabs(left) < 4.5f)
                    (left > 0.0f ? sideL : sideR) = true;
                if (!inRange(fwd, left))
                    continue;
                {
                    // (tests, GT4_HUD_RADARLOG=<n>: the first n race frames with a car in range, each car's place on the
                    // radar against the Dash page's maths: forward / left metres and the rotation)
                    static const int radarLog = [] {
                        const char *v = std::getenv("GT4_HUD_RADARLOG");
                        return v ? std::atoi(v) : 0;
                    }();
                    static int logged = 0;
                    static uint64_t lastFrame = 0;
                    if (logged < radarLog && d <= range)
                    {
                        if (s.raceFrames != lastFrame)
                            lastFrame = s.raceFrames, ++logged;
                        std::fprintf(stderr, "[gt4-hud] radar frame %llu car %d: fwd %.3f left %.3f d %.3f rot %.2f deg at %.1f, %.1f px (radius %.1f, range %.1f m)\n",
                                     static_cast<unsigned long long>(s.raceFrames), i, fwd, left, d, -(c.yaw - me.yaw) * 57.29578f, -left * sc, -fwd * sc, R, range);
                    }
                }
                cars[shownCars++] = gt4hudlook::RadarCar{left, fwd, -(c.yaw - me.yaw) * 57.29578f, d < 6.0f ? along : d < 12.0f ? close : other};
            }
            gt4hudlook::radar(r, style, rgb(t.radarCol[4], t.radarBg < 0.0f ? alpha : t.radarBg), vis, range, t.radarLabels && W >= 90.0f, g.radarRange, cars,
                              shownCars, you, sideL, sideR, rgb(t.radarCol[3], vis));
            mark(r);
            // Just zoomed: the new range in a small dark pill under the centre for 1.5 s (the last 0.2 s fading).
            if (const double left = g.radarPillUntil - GetTime(); left > 0.0)
            {
                const float pa = vis * static_cast<float>(std::clamp(left / 0.2, 0.0, 1.0));
                const std::string m = std::to_string(static_cast<int>(std::lround(g.radarRange))) + " m";
                const float fs = std::max(10.0f, W * 0.08f);
                const float tw = gt4hudlook::textWidth(m, fs);
                const Rectangle pill{c0.x - tw * 0.5f - fs * 0.5f, c0.y + W * 0.26f, tw + fs, fs * 1.5f};
                gt4hudlook::roundRect(pill, 1.0f, gt4hudlook::fade(BLACK, 0.85f * pa));
                gt4hudlook::text(m, c0.x, pill.y + pill.height * 0.5f, fs, gt4hudlook::fade(gt4hudlook::kText, pa));
            }
        }

        // style 0 the number and the bar, 1 the number only (bigger, in the middle).
        void drawDelta(const Rectangle &r, float alpha, const gt4race::Car &me, int style)
        {
            const double b = me.lap >= 1 && !me.finished ? bestAt(me.lapDistance) : -1.0;
            char t[32];
            int sign = 0;
            float frac = 0.0f;
            if (b < 0.0)
                std::snprintf(t, sizeof(t), "-.---");
            else
            {
                const double d = (static_cast<double>(me.lapTimeMs) - b) / 1000.0;
                std::snprintf(t, sizeof(t), "%c%.3f", d < 0.0 ? '-' : '+', std::fabs(d));
                sign = d < 0.0 ? -1 : 1;
                frac = static_cast<float>(std::clamp(d / 0.5, -1.0, 1.0));
            }
            gt4hudlook::delta(r, alpha, style, t, sign, frac);
            mark(r);
        }

        void drawPedals(const Rectangle &r, float alpha, const gt4race::Player &p)
        {
            gt4hudlook::pedals(r, alpha, p.brake, p.throttle);
            mark(r);
        }

        // style 0 the dot with its trail, 1 the dot only.
        void drawGMeter(const Rectangle &r, float alpha, int style)
        {
            char t[24] = "";
            if (!g.trail.empty())
                std::snprintf(t, sizeof(t), "%.2f g", std::hypot(g.trail.back().x, g.trail.back().y));
            gt4hudlook::gmeter(r, alpha, style, g.trail.data(), static_cast<int>(g.trail.size()), t);
            mark(r);
        }

        // The start lights: a row of N; light k (1..N) comes on when GT4's countdown has N - k + 1 seconds or less left
        // (the last one 1 s before the start); all go out on GT4's own start, then the row fades over 0.5 s.
        // rows: 1, or 2 (GT4_START_LOOK=two: two rows lit together, as GT3 has them); the box holds rows x one cell.
        bool drawLights(const Rectangle &row, int n, int hold, int rows = 1)
        {
            float fade = 1.0f;
            if (hold <= 0)
            {
                if (g.outAt < 0.0)
                    return false;
                fade = 1.0f - static_cast<float>((GetTime() - g.outAt) / 0.5);
                if (fade <= 0.0f)
                    return false;
            }
            int lit = 0;
            for (int k = 1; k <= n; ++k)
                if (hold > 0 && hold <= (n - k + 1) * 60)
                    ++lit;
            gt4hudlook::lights(row, n, rows, lit, fade);
            mark(row);
            return true;
        }

        // Whether GT4's view shows its compact panel (1: the chase views) or its onboard panel (0: bumper, bonnet; -1 not
        // known yet), and whether GT4's panel is hidden (GT4_HUD_HIDE panel): the game thread's, for the presenter's widget
        // places.
        std::atomic<int> g_compactView{-1};
        std::atomic<bool> g_panelHidden{false};
    } // namespace

    void beforeRaceDisplayDraw(uint8_t *rdram, uint32_t rd)
    {
        // The parts and their RaceDisplay members (HUD-HANDOFF-3-REPORT.md B3; offsets and vtables from CORE.elf). Each
        // part's static label goes with it (HUD hand-off 6 A1; the label sprites RaceDisplay draws after the part's own
        // member, 0x39fcb0 / 0x39fd30 / 0x39fda8: "Position" +0x1668, "Lap" +0x1684, "Best Lap" +0x16a0, "Last Lap" +0x16bc).
        struct Part
        {
            const char *name;
            uint32_t members[8];
            int count;
        };
        static const Part kParts[] = {
            {"position", {0x1398u, 0x1668u}, 2}, // RaceRankDisplay, its label
            {"lap", {0x1300u, 0x1684u}, 2},      // RaceLapDisplay, its label
            {"times", {0x13f0u, 0x1438u, 0x14a8u, 0x1518u, 0x1588u, 0x15f8u, 0x16a0u, 0x16bcu}, 8}, // RaceLapTimesDisplay + five RaceTimeDisplay, two labels
            {"panel", {0x108u, 0xd58u}, 2},      // RaceOnboardPanel + the chase view's compact panel
            {"map", {0x1108u}, 1},               // RaceMiniMap
            // HUD hand-off 7 A3: "times" split in two (members found by the race pictures' rectangles, gt4_hud_pictures.cpp):
            // the race clock (top centre) and best / last lap (top right, their values and labels).
            {"clock", {0x15f8u}, 1},
            {"besttimes", {0x13f0u, 0x1438u, 0x14a8u, 0x1518u, 0x1588u, 0x16a0u, 0x16bcu}, 7},
        };
        constexpr unsigned kPartCount = static_cast<unsigned>(sizeof(kParts) / sizeof(kParts[0]));
        static const unsigned mask = [] {
            unsigned m = 0;
            const char *v = std::getenv("GT4_HUD_HIDE");
            std::string list = v ? v : "", item, said;
            for (size_t i = 0; i <= list.size(); ++i)
            {
                if (i < list.size() && list[i] != ',')
                {
                    if (list[i] != ' ')
                        item += list[i];
                    continue;
                }
                bool found = false;
                for (unsigned k = 0; k < kPartCount; ++k)
                    if (item == kParts[k].name)
                        m |= 1u << k, found = true, said += (said.empty() ? "" : ", ") + item;
                if (!found && !item.empty())
                    std::fprintf(stderr, "[gt4-hud] GT4_HUD_HIDE: '%s' not understood (position, lap, clock, besttimes, times, panel, map): left out\n", item.c_str());
                item.clear();
            }
            if (m)
                std::fprintf(stderr, "[gt4-hud] GT4's own HUD: hiding %s\n", said.c_str());
            return m;
        }();
        // Which of GT4's panels this view has, for the widgets' automatic places: GT4 sets the onboard panel's flag
        // (RD+0x108) in the bumper and bonnet views and the compact panel's (RD+0xd58) in the chase views as the view
        // changes (read before the hiding below clears them; both clear: as before). The first changes logged.
        {
            const uint32_t r = rd & 0x1fffffffu;
            uint32_t vt = 0;
            if (r + 0xd60u < 0x02000000u)
                std::memcpy(&vt, rdram + r, 4);
            const bool onboard = vt == 0x67e278u && rdram[r + 0x108u] != 0u, compact = vt == 0x67e278u && rdram[r + 0xd58u] != 0u;
            const int view = compact && !onboard ? 1 : onboard && !compact ? 0 : -1;
            if (view >= 0 && g_compactView.exchange(view) != view)
            {
                static int said = 0;
                gt4race::State ls;
                if (said < 12 && gt4race::snapshot(ls))
                    ++said, std::fprintf(stderr, "[gt4-hud] GT4's %s panel (vsync %llu)\n", view ? "compact" : "onboard", static_cast<unsigned long long>(ls.vsync));
            }
        }
        // (tests, GT4_HUD_HIDE_TEST=<first vsync>,<vsyncs each>,<member offset hex>,...: one RaceDisplay member's flag
        // cleared at a time, in turn, the previous one put back: which member draws what)
        {
            static const std::vector<uint32_t> test = [] {
                std::vector<uint32_t> v;
                if (const char *e = std::getenv("GT4_HUD_HIDE_TEST"))
                    for (const char *c = e; *c;)
                    {
                        v.push_back(static_cast<uint32_t>(std::strtoul(c, nullptr, v.size() < 2u ? 10 : 16)));
                        while (*c && *c != ',')
                            ++c;
                        if (*c == ',')
                            ++c;
                    }
                return v;
            }();
            if (test.size() > 2u)
            {
                static uint32_t lastOff = 0, lastRd = 0;
                gt4race::State ts;
                if (gt4race::snapshot(ts) && ts.vsync >= test[0])
                {
                    const size_t k = static_cast<size_t>((ts.vsync - test[0]) / std::max<uint32_t>(1u, test[1]));
                    const uint32_t off = k < test.size() - 2u ? test[2 + k] : 0u;
                    const uint32_t r = rd & 0x1fffffffu;
                    if (off != lastOff)
                    {
                        if (lastOff && lastRd + lastOff < 0x02000000u)
                            rdram[lastRd + lastOff] = 1u;
                        std::fprintf(stderr, "[gt4-hud] hide test: vsync %llu member +0x%x\n", static_cast<unsigned long long>(ts.vsync), off);
                        lastOff = off, lastRd = r;
                    }
                    if (off && r + off < 0x02000000u)
                        rdram[r + off] = 0u;
                }
            }
        }
        g_panelHidden.store(((mask >> 3) & 1u) != 0u, std::memory_order_relaxed);
        const unsigned eff = mask;
        // The flags cleared here (GT4's RaceDisplay, member offset), put back when the race is not live or a part is no
        // longer hidden.
        static std::vector<std::pair<uint32_t, uint32_t>> cleared;
        static unsigned lastEff = 0;
        if (eff != lastEff)
        {
            for (const auto &c : cleared)
                if (c.first + c.second < 0x02000000u)
                    rdram[c.first + c.second] = 1u;
            cleared.clear();
            lastEff = eff;
        }
        if (!eff)
            return;
        rd &= 0x1fffffffu;
        if (rd + 0x1700u >= 0x02000000u)
            return;
        uint32_t vt = 0;
        std::memcpy(&vt, rdram + rd, 4);
        if (vt != 0x67e278u)
        {
            static bool said = false;
            if (!said)
                std::fprintf(stderr, "[gt4-hud] GT4_HUD_HIDE: RaceDisplay's vtable %08x is not GT4 USA's (0x67e278): GT4's HUD left as it is\n", vt), said = true;
            return;
        }
        gt4race::State s;
        const bool live = gt4race::msSinceLastFrame() < 500u && gt4race::snapshot(s) &&
                          (s.phase == gt4race::Phase::Starting || s.phase == gt4race::Phase::Racing || s.phase == gt4race::Phase::Finished ||
                           s.phase == gt4race::Phase::Paused);
        if (!live)
        {
            for (const auto &c : cleared)
                if (c.first + c.second < 0x02000000u)
                    rdram[c.first + c.second] = 1u;
            cleared.clear();
            return;
        }
        for (unsigned k = 0; k < kPartCount; ++k)
        {
            if (!((eff >> k) & 1u))
                continue;
            for (int j = 0; j < kParts[k].count; ++j)
            {
                const uint32_t off = kParts[k].members[j];
                if (rdram[rd + off] != 0u)
                {
                    rdram[rd + off] = 0u;
                    if (std::find(cleared.begin(), cleared.end(), std::make_pair(rd, off)) == cleared.end())
                        cleared.emplace_back(rd, off);
                }
            }
        }
    }

    int zoomKey()
    {
        const Settings &t = settings();
        return t.w[Radar].on && !t.radarZoom.empty() ? t.zoomKey : 0;
    }
    int zoomPadButton()
    {
        const Settings &t = settings();
        return t.w[Radar].on && !t.radarZoom.empty() ? t.zoomPad : -1;
    }
    void zoomPress() { g_zoomPresses.fetch_add(1, std::memory_order_relaxed); }

    bool enabled()
    {
        const Settings &t = settings();
        bool any = t.lights;
        for (const Spec &sp : t.w)
            any = any || sp.on;
        return any;
    }

    void draw(float screenW, float screenH, float x, float y, float w, float h)
    {
        (void)screenW;
        (void)screenH;
        const Settings &t = settings();
        if (!enabled() || w < 16.0f || h < 16.0f)
            return;
        gt4race::State s;
        if (gt4race::msSinceLastFrame() > 500u || !gt4race::snapshot(s))
            return;
        track(s);
        const bool live = s.phase == gt4race::Phase::Starting || s.phase == gt4race::Phase::Racing || s.phase == gt4race::Phase::Finished;
        const int pi = s.player.index;
        // The Race HUD zoom button's presses (HUD hand-off 9), taken in a live race only (tests: GT4_HUD_ZOOM_TEST's vsyncs).
        {
            int presses = g_zoomPresses.exchange(0);
            static const std::vector<uint64_t> test = [] {
                std::vector<uint64_t> v;
                if (const char *e = std::getenv("GT4_HUD_ZOOM_TEST"))
                    for (const std::string &f : fields(e))
                        if (!f.empty())
                            v.push_back(std::strtoull(f.c_str(), nullptr, 10));
                return v;
            }();
            static size_t testAt = 0;
            while (testAt < test.size() && s.vsync >= test[testAt])
                ++testAt, ++presses, std::fprintf(stderr, "[gt4-hud] zoom test: a press at vsync %llu\n", static_cast<unsigned long long>(s.vsync));
            if (live && t.w[Radar].on)
                for (; presses > 0; --presses)
                    radarZoomStep(t, s);
        }
        if (!live || pi < 0 || pi >= s.carCount)
            return;
        const gt4race::Car &me = s.cars[pi];
        const float H = h, m = 0.02f * H;
        float used[8] = {}; // per place, the room taken so far (stacked away from the edge; 7: bottom right, lifted)
        // The start lights take the top centre first.
        if (t.lights && s.phase != gt4race::Phase::Finished)
        {
            const int rows = t.twoRows ? 2 : 1;
            const float cell = 0.06f * H * (t.lightsBox ? t.lsize : 1.0f);
            const float rowW = cell * static_cast<float>(t.count) + cell * 0.25f * static_cast<float>(t.count + 1), rowH = cell * static_cast<float>(rows);
            Rectangle row{x + (w - rowW) * 0.5f, y + m, rowW, rowH};
            if (t.lightsBox)
            {
                // (GT4_START_LIGHTS_BOX: centred at its x, y in the HUD area, kept inside it)
                row.x = std::clamp(x + t.lx * w - rowW * 0.5f, x, std::max(x, x + w - rowW));
                row.y = std::clamp(y + t.ly * h - rowH * 0.5f, y, std::max(y, y + h - rowH));
            }
            // (at the top centre, as before or in their box: the top centre's widgets go under them)
            if (drawLights(row, t.count, s.startHold, rows) && (!t.lightsBox || (row.y < y + 0.12f * H && std::fabs(row.x + rowW * 0.5f - (x + w * 0.5f)) < 0.25f * w)))
                used[5] = std::max(used[5], row.y + rowH - (y + m) + m * 0.5f);
        }
        auto drawBox = [&](int i, const Rectangle &r) {
            const Spec &sp = t.w[i];
            switch (i)
            {
            case Radar: drawRadar(r, sp.alpha, s, me, sp.style); break;
            case Delta: drawDelta(r, sp.alpha, me, sp.style); break;
            case Pedals: drawPedals(r, sp.alpha, s.player); break;
            case GMeter: drawGMeter(r, sp.alpha, sp.style); break;
            case Tower: drawTower(r, sp.alpha, s, sp, H); break;
            case Map: drawMap(r, sp.alpha, s, pi, sp.style, sp.simple); break;
            case Splits: drawSplits(r, sp.alpha, s, pi); break;
            case Revs: drawRevs(r, sp.alpha, s, sp.style); break;
            case Speed: drawSpeed(r, sp.alpha, s, me); break;
            case Gear: drawGear(r, sp.alpha, s, me); break;
            case LapBox: drawLap(r, sp.alpha, s, me, sp.style); break;
            default: break;
            }
        };
        for (int i = 0; i < WidgetCount; ++i)
        {
            const Spec &sp = t.w[i];
            if (!sp.on)
                continue;
            float bw = 0.0f, bh = 0.0f;
            gt4hudbox::baseSize(i, sp.style, sp.towerName, sp.towerCols, sp.towerRows, bw, bh);
            const float ww = bw * H * sp.size, wh = bh * H * sp.size;
            if (sp.xy)
            {
                // The new form (HUD hand-off 7 A3): centred where the launcher's page put it, kept inside the HUD area.
                const Rectangle r{std::clamp(x + sp.x * w - ww * 0.5f, x, std::max(x, x + w - ww)), std::clamp(y + sp.y * h - wh * 0.5f, y, std::max(y, y + h - wh)),
                                  ww, wh};
                drawBox(i, r);
                continue;
            }
            int p = sp.place, slot = p;
            float lift = 0.0f; // (a bottom place's bottom edge this far above the picture's bottom, of its height; 0: the margin)
            if (p == 7)
            {
                // Automatic (HUD hand-off 6 A2): clear of GT4's HUD in the view shown. The delta: above GT4's dash in the bumper
                // and bonnet views, at the top centre in the chase views (the car's roof is where the dash is; their top
                // centre has no mirror). The others: bottom right, lifted over the chase views' compact gear and speed box
                // (its top at about 87 % of the picture's height) unless GT4's panel is hidden, and over TT's chase meter (its
                // top at 316 / 480) while that is drawn.
                const bool otherView = g_compactView.load(std::memory_order_relaxed) == 1;
                if (i == Delta)
                {
                    p = slot = otherView ? 5 : 6;
                    // (GT4_MIRROR=both: GT4's mirror at the top centre in the chase views too, its race clock under it (to
                    // about 24 % of the picture's height); the delta goes under both)
                    if (otherView && gt4mirror::mode() == 1)
                        used[5] = std::max(used[5], 0.25f * H);
                }
                else
                {
                    p = slot = 0;
                    if (otherView && !g_panelHidden.load(std::memory_order_relaxed))
                        lift = 0.14f, slot = 7;
                }
            }
            const bool bottom = p == 0 || p == 1 || p == 4 || p == 6, right = p == 0 || p == 2, centre = p == 4 || p == 5 || p == 6;
            const float wx = centre ? x + (w - ww) * 0.5f : right ? x + w - m - ww : x + m;
            // (6: above GT4's dash, the bumper view's onboard panel taking about the bottom 29 % of the picture)
            const float floor = p == 6 ? y + h - 0.31f * H : lift > 0.0f ? y + h - lift * H : y + h - m;
            const float wy = bottom ? floor - wh - used[slot] : y + m + used[slot];
            used[slot] += wh + m * 0.5f;
            drawBox(i, Rectangle{wx, wy, ww, wh});
        }
    }
} // namespace gt4hud
