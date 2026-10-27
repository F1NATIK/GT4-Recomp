// GT4 Recompilation launcher, Full edition: Driving > Race HUD > HUD layout (HUD hand-off 7 A2,
// analysis/design/hud-pages/handoff7). A race picture GT4.exe saved (its hud-pictures folder, gt4_hud_pictures.h; the stock
// set the launcher carries before the first race) with GT4's own HUD parts cut from the same frame and our own boxes drawn
// with sample numbers in gt4hud's look (shared/gt4_hud_look.h: GT3's HUD look, the same calls GT4.exe draws with); the boxes are dragged, resized with the mouse wheel or the keys, ticked on and off.
// What it writes: GT4_HUD_<WIDGET> (the new form), GT4_START_LIGHTS / _COUNT / _LIGHTS_BOX / _LOOK, GT4_HUD_HIDE, GT4_MIRROR,
// GT4_HUD_MOVE (GT4's own parts dragged and resized, HUD hand-off 7 B4; gt4full_settings.cpp).
// Driving > Race HUD > Radar (HUD hand-off 7 D; GT3's radar settings since 20 Oct 2026): GT4_HUD_RADAR's show, place, size
// and shape, and GT4_RADAR_SHOW / _RANGE (10-80 m) / _CARS / _BG / _COLOURS / _LABELS (_ZOOM written as off), with the stock
// chase picture and a close-up at the range.

#include "gt4stock_app.h"

#include "gt4full_racetiming.h"
#include "gt4full_ui.h"
#include "gt4stock_resources.h"
#include "gt4stock_util.h"
#include "shared/gt4_hud_look.h"

#include "rlgl.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <functional>
#include <sstream>

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
        Rectangle R(float x, float y, float w, float h) { return Rectangle{x, y, w, h}; }

        // Our own boxes and GT4's parts, in the page's order.
        struct OwnBox
        {
            const char *id, *name;
            int widget; // kFullHudLiveKeys' index; -1 the start lights
        };
        // (HUD hand-off 7 C's boxes after the start lights, so the lights keep their index for the test scenes)
        constexpr OwnBox kOwn[] = {{"radar", "Radar", 0},        {"delta", "Delta", 1},          {"pedals", "Pedals", 2},      {"gmeter", "G-meter", 3},
                                   {"lights", "Start lights", -1}, {"tower", "Timing tower", 4},   {"map", "Track map", 5},       {"splits", "Split times", 6},
                                   {"revs", "Rev lights", 7},      {"speed", "Speed", 8},          {"gear", "Gear", 9},          {"lap", "Lap counter", 10}};
        constexpr int kOwnCount = static_cast<int>(sizeof(kOwn) / sizeof(kOwn[0]));
        struct Gt4Part
        {
            const char *id, *name;
            int hideBit; // kFullHudHideKeys' index; -1 the mirror (GT4_MIRROR)
        };
        // (HUD hand-off 7 B4: each one moved and resized with GT4_HUD_MOVE; the chase views' compact panel is four pieces of its
        // own, which share the panel's tick. "panel" itself is the bumper and bonnet views' dash.)
        constexpr Gt4Part kGt4[] = {{"position", "Position", 0},       {"lap", "Lap counter", 1},       {"clock", "Race clock", 2},
                                    {"besttimes", "Best and last lap", 3}, {"map", "Course map", 5},       {"panel", "Speedo panel", 4},
                                    {"fuel", "Fuel gauge", 4},             {"warnings", "Warning lights", 4}, {"gbar", "G bar", 4},
                                    {"gearspeed", "Gear and speed", 4},    {"mirror", "Rear-view mirror", -1}};
        constexpr int kGt4Count = static_cast<int>(sizeof(kGt4) / sizeof(kGt4[0]));
        bool isPiece(const Gt4Part &p) { return p.hideBit == 4 && std::strcmp(p.id, "panel") != 0; }
    } // namespace

    // Our boxes as last drawn (dp, kOwn's order; for the hidden test mode's drag scenes, gt4full_shots.cpp).
    Rectangle g_raceHudBoxDp[12]{};
    // GT4's parts as last drawn (dp, kGt4's order: position, lap, clock, besttimes, map, panel, fuel, warnings, gbar, gearspeed,
    // mirror; empty: not drawn), for the same scenes.
    Rectangle g_raceHudGt4Dp[11]{};

    namespace
    {

        const char *courseName(int id)
        {
            switch (id)
            {
            case 7: return "New York";
            case 81: return "Autumn Ring Mini";
            default: return nullptr;
            }
        }

        // ---- the .json GT4.exe writes (gt4_hud_pictures.cpp): a small reader for its own layout
        size_t skipWs(const std::string &t, size_t i)
        {
            while (i < t.size() && (t[i] == ' ' || t[i] == '\n' || t[i] == '\r' || t[i] == '\t'))
                ++i;
            return i;
        }
        bool numberAfter(const std::string &t, const std::string &key, double &out)
        {
            const size_t k = t.find("\"" + key + "\"");
            if (k == std::string::npos)
                return false;
            size_t i = t.find(':', k);
            if (i == std::string::npos)
                return false;
            i = skipWs(t, i + 1);
            char *end = nullptr;
            out = std::strtod(t.c_str() + i, &end);
            return end != t.c_str() + i;
        }
        bool stringAfter(const std::string &t, const std::string &key, std::string &out)
        {
            const size_t k = t.find("\"" + key + "\"");
            if (k == std::string::npos)
                return false;
            const size_t a = t.find('"', t.find(':', k) + 1), b = a == std::string::npos ? a : t.find('"', a + 1);
            if (a == std::string::npos || b == std::string::npos)
                return false;
            out = t.substr(a + 1, b - a - 1);
            return true;
        }

        // Sample race numbers in gt4hud's look (shared/gt4_hud_look.h: GT4.exe's own calls), drawn in the window's pixels.
        Color withA(Color c, float a)
        {
            c.a = static_cast<unsigned char>(std::clamp(a, 0.0f, 1.0f) * 255.0f);
            return c;
        }
        Color hex(uint32_t v, float a = 1.0f) { return gt4hudlook::hex(v, a); }
    }

    namespace
    {
        // ---- HUD hand-off 7 C's boxes with sample numbers, drawn by gt4hudlook as GT4.exe draws them (gt4_hud.cpp; sizes from
        // shared/gt4_hud_boxes.h). r is the box in dp, H the picture's height in dp; everything is drawn in pixels (u.px).
        struct SampleCar
        {
            int car, place;
            const char *name, *code, *gap, *iv, *best, *last;
        };
        // (Autumn Ring Mini's Sunday Cup field; you are car 0, the Ka)
        constexpr SampleCar kSample[] = {{3, 1, "Honda S500 '63", "HS5", "LEAD", "", "1'01.069", "1'01.069"},
                                         {2, 2, "Daihatsu Midget '63", "DMI", "+1.204", "+1.204", "1'02.410", "1'02.410"},
                                         {0, 3, "Ford Ka '01", "FKA", "+2.877", "+1.673", "1'03.802", "1'03.802"},
                                         {5, 4, "Honda N360 '67", "HN3", "+4.103", "+1.226", "1'04.115", "1'04.230"},
                                         {1, 5, "VW Beetle 1100 Standard '49", "VBE", "+6.550", "+2.447", "1'05.006", "1'05.006"},
                                         {4, 6, "Subaru 360 '58", "S36", "+9.012", "+2.462", "1'06.645", "1'06.645"}};
        Color sampleColour(int car) { return gt4hudlook::carColour(car, car == 0); }
        void drawNewBoxSample(ui::Ui &u, int w, const Rectangle &rDp, const FullHudWidget &h, float alpha, float H, const Texture2D *art, float artW,
                              float artH, const std::vector<Vector2> &line)
        {
            namespace look = gt4hudlook;
            const Rectangle r = u.px(rDp);
            const float k = H * u.s * std::clamp(h.size, 40, 250) / 100.0f; // (the box's height unit in pixels, as GT4.exe's)
            if (w == kFullHudTower)
            {
                const int rows = std::min(std::clamp(h.towerRows, 4, 16), 6);
                look::TowerRow t[6];
                for (int i = 0; i < rows; ++i)
                {
                    const SampleCar &c = kSample[i];
                    t[i].place = c.place, t[i].car = c.car, t[i].player = c.car == 0, t[i].purpleBest = i == 0;
                    t[i].name = racetiming::towerName(std::clamp(h.towerName, 0, 2), c.car, c.car == 0, c.name, c.code, gt4hudbox::kTowerRow * k * 0.5f,
                                                      racetiming::driverWidth(k));
                    t[i].vals[0] = c.gap, t[i].vals[1] = c.iv, t[i].vals[2] = c.best, t[i].vals[3] = c.last;
                }
                look::tower(r, alpha, k, std::clamp(h.towerName, 0, 2), h.towerCols, t, rows);
                return;
            }
            if (w == kFullHudMap)
            {
                const float dotR = std::max(3.0f, std::min(r.width, r.height) * (h.style == 1 ? 0.045f : 0.05f));
                const float fr[6] = {0.47f, 0.455f, 0.42f, 0.40f, 0.37f, 0.33f}; // (lap fractions of the sample field, by place)
                auto at = [&](float f) {
                    const size_t n = line.size();
                    const float xx = (f - std::floor(f)) * static_cast<float>(n);
                    const size_t i = static_cast<size_t>(xx) % n, j = (i + 1) % n;
                    const float t = xx - std::floor(xx);
                    return Vector2{line[i].x + (line[j].x - line[i].x) * t, line[i].y + (line[j].y - line[i].y) * t};
                };
                const bool haveArt = art && art->id && line.size() >= 8u && artW > 0 && artH > 0;
                // Simple (HUD hand-off 10): the art's line instead of its picture, as GT4.exe draws it (a dark edge under a light
                // grey stroke about 2.5 % of the box high; on the round map only inside the disc).
                auto simpleLine = [&](const std::function<Vector2(Vector2)> &toBox, Vector2 c0, float R) {
                    const float lw = std::max(2.0f, r.height * 0.025f);
                    for (int pass = 0; pass < 2; ++pass)
                        for (size_t i = 0; i < line.size(); ++i)
                        {
                            const Vector2 p = toBox(line[i]), q = toBox(line[(i + 1) % line.size()]);
                            if (R > 0.0f && (std::hypot(p.x - c0.x, p.y - c0.y) > R * 0.97f || std::hypot(q.x - c0.x, q.y - c0.y) > R * 0.97f))
                                continue;
                            look::line(p, q, pass ? lw : lw + 2.0f, pass ? Color{201, 209, 219, 255} : look::fade(BLACK, 0.8f));
                        }
                };
                if (h.style == 1)
                {
                    const float R = r.width * 0.5f;
                    const Vector2 c0{r.x + R, r.y + R}, anchor{c0.x, r.y + r.height * 0.62f};
                    look::circle(c0, R, look::fade(BLACK, alpha));
                    if (haveArt)
                    {
                        // (the art as a textured disc, turned so the sample car's way is up, as GT4.exe draws it)
                        const Vector2 P = at(fr[2]), Q = at(fr[2] + 0.012f);
                        const float ang = -1.5707963f - std::atan2(Q.y - P.y, Q.x - P.x), sc = r.width / (artW * 0.3f);
                        const float ca = std::cos(ang), sa = std::sin(ang);
                        if (h.simple)
                            simpleLine([&](Vector2 p) {
                                const float dx = (p.x - P.x) * sc, dy = (p.y - P.y) * sc;
                                return Vector2{anchor.x + dx * ca - dy * sa, anchor.y + dx * sa + dy * ca};
                            }, c0, R);
                        else
                        {
                        rlDrawRenderBatchActive();
                        rlDisableBackfaceCulling();
                        rlBegin(RL_TRIANGLES);
                        rlSetTexture(art->id); // (after rlBegin: a new mode's draw call starts with the default texture)
                        rlColor4ub(255, 255, 255, static_cast<unsigned char>(std::clamp(0.35f + alpha, 0.0f, 1.0f) * 255.0f));
                        auto uv = [&](float qx, float qy) {
                            const float dx = qx - anchor.x, dy = qy - anchor.y;
                            rlTexCoord2f(((dx * ca + dy * sa) / sc + P.x) / artW, ((-dx * sa + dy * ca) / sc + P.y) / artH);
                            rlVertex2f(qx, qy);
                        };
                        const float rr = R * 0.97f;
                        for (int s2 = 0; s2 < 64; ++s2)
                        {
                            const float t0 = 6.2831853f * s2 / 64.0f, t1 = 6.2831853f * (s2 + 1) / 64.0f;
                            uv(c0.x, c0.y);
                            uv(c0.x + rr * std::cos(t0), c0.y + rr * std::sin(t0));
                            uv(c0.x + rr * std::cos(t1), c0.y + rr * std::sin(t1));
                        }
                        rlEnd();
                        rlSetTexture(0);
                        rlDrawRenderBatchActive();
                        rlEnableBackfaceCulling();
                        }
                        for (int i = 5; i >= 0; --i)
                        {
                            if (kSample[i].car == 0)
                                continue;
                            const Vector2 p = at(fr[i]);
                            const float dx = (p.x - P.x) * sc, dy = (p.y - P.y) * sc;
                            const float qx = anchor.x + dx * ca - dy * sa, qy = anchor.y + dx * sa + dy * ca;
                            if (std::hypot(qx - c0.x, qy - c0.y) < R - dotR)
                                look::mapDot(Vector2{qx, qy}, dotR, sampleColour(kSample[i].car), kSample[i].place);
                        }
                    }
                    look::mapArrow(anchor, dotR * 1.5f, look::me());
                    look::ring(c0, R, std::max(1.5f, R * 0.02f), look::fade(WHITE, 0.35f));
                    return;
                }
                look::panel(r, alpha);
                if (haveArt)
                {
                    const float pad = std::min(r.width, r.height) * 0.04f;
                    const float sc = std::min((r.width - pad * 2) / artW, (r.height - pad * 2) / artH);
                    const float ox = r.x + (r.width - artW * sc) * 0.5f, oy = r.y + (r.height - artH * sc) * 0.5f;
                    if (h.simple)
                        simpleLine([&](Vector2 p) { return Vector2{ox + p.x * sc, oy + p.y * sc}; }, Vector2{}, 0.0f);
                    else
                        DrawTexturePro(*art, Rectangle{0, 0, static_cast<float>(art->width), static_cast<float>(art->height)}, Rectangle{ox, oy, artW * sc, artH * sc},
                                       Vector2{0, 0}, 0.0f, withA(WHITE, std::clamp(0.35f + alpha, 0.0f, 1.0f)));
                    for (int i = 5; i >= 0; --i)
                    {
                        const Vector2 p = at(fr[i]);
                        if (kSample[i].car != 0)
                            look::mapDot(Vector2{ox + p.x * sc, oy + p.y * sc}, dotR, sampleColour(kSample[i].car), kSample[i].place);
                    }
                    const Vector2 p = at(fr[2]);
                    look::mapDot(Vector2{ox + p.x * sc, oy + p.y * sc}, dotR * 1.2f, look::me(), 3);
                }
                else
                {
                    // (no course art next to GT4.exe: the outline the cars drive, as GT4.exe draws it)
                    const Rectangle o{r.x + r.width * 0.12f, r.y + r.height * 0.15f, r.width * 0.76f, r.height * 0.7f};
                    look::roundRectLines(o, 0.9f, std::max(1.5f, dotR * 0.55f), Color{176, 182, 192, 255});
                    for (int i = 5; i >= 0; --i)
                        look::mapDot(Vector2{o.x + o.width * (0.25f + 0.09f * i), o.y + o.height}, kSample[i].car == 0 ? dotR * 1.2f : dotR,
                                     sampleColour(kSample[i].car), kSample[i].place);
                }
                return;
            }
            if (w == kFullHudSplits)
            {
                const look::Split cells[3] = {look::splitDone("22.540", 0), look::splitDone("28.064", 1), look::splitRunning("9.311")};
                look::splits(r, alpha, cells, 3);
            }
            else if (w == kFullHudRevs)
                look::revs(r, alpha, h.style, 0.92f, true);
            else if (w == kFullHudSpeed)
                look::speed(r, alpha, "112", "km/h");
            else if (w == kFullHudGear)
                look::gear(r, alpha, "3", false);
            else if (w == kFullHudLap)
            {
                const std::string clock = "0'58.150";
                look::lap(r, alpha, 1, 2, h.style == 1 ? &clock : nullptr);
            }
        }
        // The other boxes' samples (radar aside): delta, pedals, g-meter, the start lights (3 lit).
        void drawOldBoxSample(ui::Ui &u, int w, const Rectangle &rDp, float alpha, int style, int lightsCount, int lightsRows)
        {
            namespace look = gt4hudlook;
            const Rectangle r = u.px(rDp);
            if (w == 1)
                look::delta(r, alpha, style, "-0.214", -1, -0.43f);
            else if (w == 2)
                look::pedals(r, alpha, 0.15f, 0.8f);
            else if (w == 3)
            {
                Vector2 trail[12];
                for (int i = 0; i < 12; ++i)
                    trail[i] = Vector2{(0.05f + i * 0.03f) * 1.5f, (0.02f + i * 0.015f) * 1.5f}; // (g: right, forward)
                trail[11] = Vector2{0.41f * 1.5f, 0.2f * 1.5f};
                look::gmeter(r, alpha, style, trail, 12, "0.68 g");
            }
            else
                look::lights(r, lightsCount, lightsRows, 3, 1.0f);
        }

        // The radar with sample cars, as GT4.exe draws it (gt4_hud.cpp drawRadar, gt4hudlook::radar; HUD hand-off 7 D, with GT3's
        // radar settings): the page's colours, the background at bgAlpha, every car at the car opacity (yours 10 % more), close
        // cars in the other cars' colour, rings at 1/3, 2/3 and the edge of `range` m with their metres (when the radar is 90 race
        // pixels or more: racePx). r is the box in dp.
        void radarSample(ui::Ui &u, const Rectangle &rDp, float bgAlpha, int style, const FullSettings &f, float range, float racePx)
        {
            const float ca = std::clamp(f.radarCars, 10, 100) / 100.0f;
            // (left / forward metres: alongside on the left, one ahead, one close behind, two further away)
            const struct
            {
                float l, fw;
            } sample[] = {{3.2f, 2.0f}, {-1.5f, 14.0f}, {-4.0f, -9.0f}, {1.0f, -24.0f}, {6.0f, 41.0f}};
            gt4hudlook::RadarCar cars[5];
            int n = 0;
            for (const auto &c : sample)
            {
                const float d = std::hypot(c.l, c.fw);
                if (style == 1 ? std::max(std::fabs(c.l), std::fabs(c.fw)) > range : d > range)
                    continue;
                cars[n++] = gt4hudlook::RadarCar{c.l, c.fw, 0.0f, gt4hudlook::hex(d < 6.0f ? f.radarColours[3] : f.radarColours[1], ca)};
            }
            gt4hudlook::radar(u.px(rDp), style, gt4hudlook::hex(f.radarColours[4], bgAlpha), 1.0f, range, f.radarLabels && racePx >= 90.0f, range, cars, n,
                              gt4hudlook::hex(f.radarColours[0], std::min(1.0f, ca + 0.1f)), true, false, gt4hudlook::hex(f.radarColours[3]));
        }

        // One object of the .json's rectangles, "<key>": {"<name>": [x, y, w, h], ...} (false: no such object).
        bool rectObject(const std::string &text, const char *key, std::vector<std::pair<std::string, Rectangle>> &out)
        {
            out.clear();
            const size_t p = text.find(std::string("\"") + key + "\"");
            if (p == std::string::npos)
                return false;
            size_t i = text.find('{', p);
            const size_t end = i == std::string::npos ? i : text.find('}', i);
            if (end == std::string::npos)
                return false;
            while (true)
            {
                const size_t a = text.find('"', i + 1);
                if (a == std::string::npos || a > end)
                    break;
                const size_t b = text.find('"', a + 1);
                const size_t lb = text.find('[', b), rb = text.find(']', lb == std::string::npos ? b : lb);
                if (b == std::string::npos || lb == std::string::npos || rb == std::string::npos || rb > end)
                    break;
                double v[4] = {};
                int n = 0;
                const char *c = text.c_str() + lb + 1;
                while (n < 4)
                {
                    char *e = nullptr;
                    v[n] = std::strtod(c, &e);
                    if (e == c)
                        break;
                    ++n;
                    c = e;
                    while (*c == ',' || *c == ' ')
                        ++c;
                }
                if (n == 4)
                    out.emplace_back(text.substr(a + 1, b - a - 1),
                                     Rectangle{static_cast<float>(v[0]), static_cast<float>(v[1]), static_cast<float>(v[2]), static_cast<float>(v[3])});
                i = rb;
            }
            return true;
        }
    }

    bool parseRaceHudJson(const std::string &text, RaceHudSet &out)
    {
        double course = 0, w = 0, h = 0;
        if (!numberAfter(text, "course", course) || !numberAfter(text, "w", w) || !numberAfter(text, "h", h) || !stringAfter(text, "view", out.view))
            return false;
        out.course = static_cast<int>(course), out.w = static_cast<int>(w), out.h = static_cast<int>(h);
        if (!rectObject(text, "parts", out.parts))
            return false;
        rectObject(text, "shown", out.shown); // (GT4_HUD_MOVE's moved parts in the -hud picture; none in an older set)
        // A set from before the compact panel's pieces were parts of their own: its four pieces (left to right) in "pieces".
        if ((out.view == "chase" || out.view == "far") && !out.part("fuel"))
        {
            const size_t pp = text.find("\"pieces\"");
            const size_t pan = pp == std::string::npos ? pp : text.find("\"panel\"", pp);
            const size_t close = pan == std::string::npos ? pan : text.find("]]", pan);
            if (pan != std::string::npos && close != std::string::npos && pan < close)
            {
                std::vector<std::pair<std::string, Rectangle>> list;
                std::string inner = "{";
                const size_t lb = text.find('[', pan);
                int k = 0;
                static const char *const kNames[4] = {"fuel", "warnings", "gbar", "gearspeed"};
                for (size_t at = lb + 1; at < close && k < 4; ++k)
                {
                    const size_t a = text.find('[', at), b = a == std::string::npos ? a : text.find(']', a);
                    if (a == std::string::npos || b == std::string::npos || a > close)
                        break;
                    inner += std::string(k ? ", " : "") + "\"" + kNames[k] + "\": " + text.substr(a, b - a + 1);
                    at = b + 1;
                }
                inner += "}";
                if (k == 4 && rectObject("\"p\": " + inner, "p", list) && list.size() == 4u)
                    out.parts.insert(out.parts.end(), list.begin(), list.end());
            }
        }
        return true;
    }

    fs::path App::raceHudFolder() const { return (inst_.exeFound ? inst_.exe.parent_path() : inst_.root) / "hud-pictures"; }

    void App::raceHudLoadSets(bool force)
    {
        if (!force && GetTime() - hudSetsAt_ < 2.0)
            return;
        hudSetsAt_ = GetTime();
        std::vector<RaceHudSet> sets;
        std::error_code ec;
        const fs::path dir = raceHudFolder();
        if (fs::is_directory(dir, ec))
            for (const auto &e : fs::directory_iterator(dir, ec))
            {
                if (e.path().extension() != ".json")
                    continue;
                RaceHudSet s;
                s.name = e.path().stem().string();
                s.clean = dir / (s.name + ".jpg");
                s.hud = dir / (s.name + "-hud.jpg");
                std::ifstream in(e.path(), std::ios::binary);
                std::stringstream ss;
                ss << in.rdbuf();
                if (parseRaceHudJson(ss.str(), s) && fs::is_regular_file(s.clean, ec) && fs::is_regular_file(s.hud, ec))
                    sets.push_back(std::move(s));
            }
        std::sort(sets.begin(), sets.end(), [](const RaceHudSet &a, const RaceHudSet &b) { return a.name > b.name; }); // newest first
        if (sets.empty())
        {
            // Before the first race: the stock set the launcher carries (a chase view, Autumn Ring Mini).
            RaceHudSet s;
            const std::vector<uint8_t> js = sys::resource(GT4FULL_IDR_HUD_STOCK_JSON);
            if (parseRaceHudJson(std::string(js.begin(), js.end()), s))
            {
                s.name = "stock";
                s.stock = true;
                sets.push_back(std::move(s));
            }
        }
        const std::string cur = hudSetIdx_ >= 0 && hudSetIdx_ < static_cast<int>(hudSets_.size()) ? hudSets_[static_cast<size_t>(hudSetIdx_)].name : std::string();
        hudSets_ = std::move(sets);
        hudSetIdx_ = 0;
        for (size_t i = 0; i < hudSets_.size(); ++i)
            if (hudSets_[i].name == cur)
                hudSetIdx_ = static_cast<int>(i);
    }

    bool App::raceHudTextures()
    {
        const bool own = !st_.full.hudPicture.empty();
        const RaceHudSet *set = hudSetIdx_ >= 0 && hudSetIdx_ < static_cast<int>(hudSets_.size()) ? &hudSets_[static_cast<size_t>(hudSetIdx_)] : nullptr;
        if (!set && !own)
            return false;
        const std::string key = (own ? "own:" + st_.full.hudPicture + "|" : std::string()) + (set ? set->name : std::string());
        if (key == hudTexKey_)
            return hudClean_.id != 0;
        hudTexKey_ = key;
        if (hudClean_.id)
            UnloadTexture(hudClean_);
        if (hudHud_.id)
            UnloadTexture(hudHud_);
        if (hudCut_.id)
            UnloadTexture(hudCut_);
        hudClean_ = hudHud_ = hudCut_ = Texture2D{};
        auto loadImage = [](const fs::path &file, int resource) {
            Image img{};
            if (resource)
            {
                const std::vector<uint8_t> bytes = sys::resource(resource);
                if (!bytes.empty())
                    img = LoadImageFromMemory(".jpg", bytes.data(), static_cast<int>(bytes.size()));
            }
            else
            {
                std::ifstream in(file, std::ios::binary);
                const std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
                std::string ext = lower(pathText(file.extension()));
                if (ext == ".jpeg")
                    ext = ".jpg";
                if (!bytes.empty())
                    img = LoadImageFromMemory(ext.c_str(), bytes.data(), static_cast<int>(bytes.size()));
            }
            if (img.data)
                ImageFormat(&img, PIXELFORMAT_UNCOMPRESSED_R8G8B8A8);
            return img;
        };
        auto texture = [](const Image &img) {
            Texture2D t{};
            if (img.data)
            {
                t = LoadTextureFromImage(img);
                if (t.id)
                    SetTextureFilter(t, TEXTURE_FILTER_BILINEAR);
            }
            return t;
        };
        Image clean{}, hud{};
        if (own)
            clean = loadImage(pathFrom(st_.full.hudPicture), 0);
        else if (set)
            clean = loadImage(set->clean, set->stock ? GT4FULL_IDR_HUD_STOCK_CLEAN : 0);
        if (set)
            hud = loadImage(set->hud, set->stock ? GT4FULL_IDR_HUD_STOCK_HUD : 0);
        hudClean_ = texture(clean);
        hudHud_ = texture(hud);
        // GT4's parts cut out of the -hud picture (HUD hand-off 7 B4): a pixel shows as much as it differs from the clean
        // picture of the same frame, so a moved part carries no square of the road with it.
        if (set && !own && hud.data && clean.data && hud.width == clean.width && hud.height == clean.height)
        {
            Image cut = ImageCopy(hud);
            auto *c = static_cast<uint8_t *>(cut.data);
            const auto *b = static_cast<const uint8_t *>(clean.data);
            const size_t n = static_cast<size_t>(cut.width) * static_cast<size_t>(cut.height);
            for (size_t i = 0; i < n; ++i)
            {
                int d = 0;
                for (int k = 0; k < 3; ++k)
                    d = std::max(d, std::abs(static_cast<int>(c[i * 4 + k]) - static_cast<int>(b[i * 4 + k])));
                c[i * 4 + 3] = static_cast<uint8_t>(std::clamp((d - 14) * 255 / 40, 0, 255));
            }
            hudCut_ = texture(cut);
            UnloadImage(cut);
        }
        if (clean.data)
            UnloadImage(clean);
        if (hud.data)
            UnloadImage(hud);
        return hudClean_.id != 0;
    }

    bool App::raceHudArt(int course)
    {
        if (course == hudArtCourse_)
            return hudArt_.id != 0;
        hudArtCourse_ = course;
        if (hudArt_.id)
            UnloadTexture(hudArt_);
        hudArt_ = Texture2D{};
        hudArtLine_.clear();
        hudArtW_ = hudArtH_ = 0;
        // pages\tracks\<course>.json next to GT4.exe (the course art the race map draws; FEED.md): "w", "h", "line" (points
        // in the picture's pixels, evenly along the lap) and "img" (a base64 JPEG).
        const fs::path file = (inst_.exeFound ? inst_.exe.parent_path() : inst_.root) / "pages" / "tracks" / (std::to_string(course) + ".json");
        std::ifstream in(file, std::ios::binary);
        std::stringstream ss;
        ss << in.rdbuf();
        const std::string t = ss.str();
        double w = 0, h = 0;
        const size_t ln = t.find("\"line\":"), im = t.find("base64,");
        if (t.empty() || !numberAfter(t, "w", w) || !numberAfter(t, "h", h) || ln == std::string::npos || im == std::string::npos || w <= 0 || h <= 0)
            return false;
        const size_t end = t.find("]]", ln);
        for (const char *c = t.c_str() + ln + 7, *stop = end == std::string::npos ? c : t.c_str() + end; c < stop;)
        {
            float v[2];
            int got = 0;
            while (got < 2 && c < stop)
            {
                while (c < stop && !(*c == '-' || (*c >= '0' && *c <= '9')))
                    ++c;
                char *e = nullptr;
                v[got] = std::strtof(c, &e);
                if (!e || e == c)
                    break;
                c = e, ++got;
            }
            if (got < 2)
                break;
            hudArtLine_.push_back(Vector2{v[0], v[1]});
        }
        std::vector<uint8_t> jpg;
        uint32_t acc = 0;
        int bits = 0;
        for (size_t i = im + 7; i < t.size() && t[i] != '"'; ++i)
        {
            const char c = t[i];
            const int v = c >= 'A' && c <= 'Z' ? c - 'A' : c >= 'a' && c <= 'z' ? c - 'a' + 26 : c >= '0' && c <= '9' ? c - '0' + 52 : c == '+' ? 62 : c == '/' ? 63 : -1;
            if (v < 0)
                continue;
            acc = (acc << 6) | static_cast<uint32_t>(v), bits += 6;
            if (bits >= 8)
                bits -= 8, jpg.push_back(static_cast<uint8_t>(acc >> bits));
        }
        if (hudArtLine_.size() < 8u || jpg.empty())
            return false;
        Image img = LoadImageFromMemory(".jpg", jpg.data(), static_cast<int>(jpg.size()));
        if (!img.data)
            return false;
        hudArt_ = LoadTextureFromImage(img);
        UnloadImage(img);
        SetTextureFilter(hudArt_, TEXTURE_FILTER_BILINEAR);
        SetTextureWrap(hudArt_, TEXTURE_WRAP_CLAMP);
        hudArtW_ = static_cast<float>(w), hudArtH_ = static_cast<float>(h);
        return hudArt_.id != 0;
    }

    void App::pageRaceHud()
    {
        // Driving > Race HUD's pages (HUD hand-off 7: HUD layout, D's Radar, E's Start lights; Timing (20 Oct 2026,
        // gt4full_racetiming.cpp); Phone is a later stage).
        drawHeader("Race HUD", raceHudTab_ == 1   ? "The cars around you, seen from above, and how far it looks. Changes are saved as you make them."
                               : raceHudTab_ == 2 ? "Standing starts: lights in place of GT4's countdown, and how the start is held. Changes are saved as you make them."
                               : raceHudTab_ == 3 ? "Who is where and how far apart: the timing tower and the split times. Changes are saved as you make them."
                                                  : "Drag boxes to move them, mouse wheel to resize. Changes are saved as you make them.");
        const float x0 = kSidebarW + 32, x1 = ui_.width - 32;
        // (the tower's Driver names in the previews: the drivers the Race Lab sent to GT4)
        racetiming::refreshDrivers(inst_.root, st_.full.raceLabDrivers, hidden_);
        if (fullui::tabs(ui_, "racehud-tabs", x0, kTop - 10, x1 - x0, {"HUD layout", "Radar", "Start lights", "Timing"}, raceHudTab_))
        {
            ui_.closePopups();
            hudDrag_.clear();
        }
        if (raceHudTab_ == 1)
            pageRaceHudRadar(kTop + 40);
        else if (raceHudTab_ == 2)
            pageRaceHudLights(kTop + 40);
        else if (raceHudTab_ == 3)
            pageRaceHudTiming(kTop + 40);
        else
            pageRaceHudLayout(kTop + 40);
    }

    void App::pageRaceHudLayout(float top)
    {
        const ui::Theme &t = ui::theme();
        FullSettings &f = st_.full;
        raceHudLoadSets();
        const float x0 = kSidebarW + 32, x1 = ui_.width - 32, bottom = ui_.height - 48;
        const float colW = 330.0f, gap = 22.0f;
        const float leftW = x1 - x0 - colW - gap;
        const float y0 = top;

        // ---- the picture: the set shown (or the player's own picture, GT4's parts as in a chase view)
        const bool own = !f.hudPicture.empty();
        const RaceHudSet *set = hudSetIdx_ >= 0 && hudSetIdx_ < static_cast<int>(hudSets_.size()) ? &hudSets_[static_cast<size_t>(hudSetIdx_)] : nullptr;
        const RaceHudSet *partsSet = set;
        if (own)
        {
            // (GT4's parts from the newest chase view, else the set shown)
            for (const RaceHudSet &s : hudSets_)
                if (s.view == "chase" || s.view == "far")
                {
                    partsSet = &s;
                    break;
                }
        }
        const bool haveTex = raceHudTextures();
        float aspect = set && set->h > 0 ? static_cast<float>(set->w) / static_cast<float>(set->h) : 16.0f / 9.0f;
        if (own && hudClean_.id && hudClean_.height > 0)
            aspect = static_cast<float>(hudClean_.width) / static_cast<float>(hudClean_.height);
        const float helpH = 64.0f, barH = 44.0f;
        float sw = leftW, sh = sw / aspect;
        if (sh > bottom - y0 - barH - helpH)
        {
            sh = std::max(120.0f, bottom - y0 - barH - helpH);
            sw = sh * aspect;
        }
        const Rectangle S = R(x0, y0, sw, sh);
        ui_.fill(R(S.x - 1, S.y - 1, S.width + 2, S.height + 2), t.border);
        ui_.fill(S, BLACK);
        if (haveTex)
            DrawTexturePro(hudClean_, Rectangle{0, 0, static_cast<float>(hudClean_.width), static_cast<float>(hudClean_.height)}, ui_.px(S), Vector2{0, 0}, 0,
                           WHITE);
        else
            ui_.text("No race picture yet: GT4 saves one 20 seconds into each race.", S.x + S.width * 0.5f, S.y + S.height * 0.5f - 8, 14, t.muted,
                     Weight::Regular, Align::Center);
        const std::string view = own ? std::string("chase") : set ? set->view : std::string("chase");
        const bool chaseView = view == "chase" || view == "far";
        const bool hidePanel = (f.hudHide >> kFullHudHidePanel) & 1;

        // ---- geometry of our boxes (dp) and GT4's parts
        auto clampBox = [&](Rectangle r) {
            r.x = std::clamp(r.x, S.x, std::max(S.x, S.x + S.width - r.width));
            r.y = std::clamp(r.y, S.y, std::max(S.y, S.y + S.height - r.height));
            return r;
        };
        // A widget's centre in fractions (Automatic: gt4hud's rules for the view shown).
        auto autoCentre = [&](int w, float bw, float bh, float &cx, float &cy) {
            const float m = 0.02f, A = S.width / S.height;
            if (w == 1)
            {
                cx = 0.5f;
                if (chaseView)
                {
                    float top = f.mirrorMode == 1 ? 0.25f : m;
                    // (under the start lights while they are at the top centre, as GT4.exe puts it before the start)
                    const int n = std::clamp(f.startCount, 3, 5), rows = f.startLook == 1 ? 2 : 1;
                    const float cell = 0.06f * std::clamp(f.lightsSize, 40, 250) / 100.0f, lw = (cell * n + cell * 0.25f * (n + 1)) / A, lh = cell * rows;
                    const float ly0 = std::clamp(f.lightsY / 100.0f - lh * 0.5f, 0.0f, 1.0f - lh);
                    if (f.startLights && ly0 < 0.12f && std::fabs(std::clamp(f.lightsX / 100.0f, lw * 0.5f, 1.0f - lw * 0.5f) - 0.5f) < 0.25f)
                        top = std::max(top, ly0 + lh + m * 0.5f);
                    cy = top + bh * 0.5f;
                }
                else
                    cy = 0.69f - bh * 0.5f;
                return;
            }
            cx = 1.0f - (m + bw * 0.5f) / A;
            const float floor = chaseView && !hidePanel ? 1.0f - 0.14f : 1.0f - m;
            cy = floor - bh * 0.5f;
        };
        auto ownRect = [&](const OwnBox &b) {
            if (b.widget < 0)
            {
                const int n = std::clamp(f.startCount, 3, 5), rows = f.startLook == 1 ? 2 : 1;
                const float cell = 0.06f * S.height * std::clamp(f.lightsSize, 40, 250) / 100.0f;
                const float bw = cell * n + cell * 0.25f * (n + 1), bh = cell * rows;
                return clampBox(R(S.x + f.lightsX / 100.0f * S.width - bw * 0.5f, S.y + f.lightsY / 100.0f * S.height - bh * 0.5f, bw, bh));
            }
            const FullHudWidget &h = f.liveWidgets[static_cast<size_t>(b.widget)];
            const float s = std::clamp(h.size, 40, 250) / 100.0f;
            float baseW = 0, baseH = 0;
            gt4hudbox::baseSize(b.widget, h.style, h.towerName, h.towerCols, h.towerRows, baseW, baseH);
            const float bwH = baseW * s, bhH = baseH * s; // (of the height)
            float cx = 0.5f, cy = 0.5f;
            if (h.xy)
                cx = h.x / 100.0f, cy = h.y / 100.0f;
            else if (h.place == 7)
                autoCentre(b.widget, bwH, bhH, cx, cy);
            else
            {
                fullHudPlaceCentre(b.widget, h.place, h.size, S.width / S.height, cx, cy);
                cx /= 100.0f, cy /= 100.0f;
            }
            const float bw = bwH * S.height, bh = bhH * S.height;
            return clampBox(R(S.x + cx * S.width - bw * 0.5f, S.y + cy * S.height - bh * 0.5f, bw, bh));
        };
        auto ownOn = [&](const OwnBox &b) { return b.widget < 0 ? f.startLights : f.liveWidgets[static_cast<size_t>(b.widget)].on; };
        auto gt4Shown = [&](const Gt4Part &p) { return p.hideBit < 0 ? f.mirrorMode != 2 : !((f.hudHide >> p.hideBit) & 1); };
        // (a chase view's set: the compact panel as its four pieces, not as "panel")
        const bool pieceView = partsSet && partsSet->part("fuel");
        auto gt4InView = [&](const Gt4Part &p) { return pieceView ? std::strcmp(p.id, "panel") != 0 : !isPiece(p); };
        auto gt4Move = [&](const Gt4Part &p) -> FullHudMove & { return f.hudMoves[static_cast<size_t>(std::max(0, fullHudMovePart(p.id)))]; };
        // GT4's place of a part in the set's view (dp; the mirror only where GT4 drew it, or every view with "Every view").
        auto gt4Base = [&](const Gt4Part &p, bool &fromSet) {
            fromSet = false;
            const Rectangle *r = partsSet && gt4InView(p) ? partsSet->part(p.id) : nullptr;
            if (r)
            {
                fromSet = true;
                return R(S.x + r->x * S.width, S.y + r->y * S.height, r->width * S.width, r->height * S.height);
            }
            if (p.hideBit < 0 && f.mirrorMode == 1)
            {
                Rectangle m{0.296875f, 0.044643f, 0.40625f, 0.129464f}; // (GT4's 16:9 mirror, 190,20 260x58 of 640x448)
                for (const RaceHudSet &s : hudSets_)
                    if (const Rectangle *mr = s.part("mirror"))
                    {
                        m = *mr;
                        break;
                    }
                return R(S.x + m.x * S.width, S.y + m.y * S.height, m.width * S.width, m.height * S.height);
            }
            return R(0, 0, 0, 0);
        };
        // Where it is drawn: GT4's place, or moved (GT4_HUD_MOVE: centred at x, y, scaled about it, kept inside the picture as
        // GT4.exe keeps it).
        auto gt4Rect = [&](const Gt4Part &p, bool &fromSet) {
            Rectangle r = gt4Base(p, fromSet);
            const FullHudMove &mv = gt4Move(p);
            if (r.width <= 0.0f || !mv.on)
                return r;
            const float s = std::clamp(mv.size, 40, 250) / 100.0f, w = r.width * s, h = r.height * s;
            return clampBox(R(S.x + mv.x / 100.0f * S.width - w * 0.5f, S.y + mv.y / 100.0f * S.height - h * 0.5f, w, h));
        };

        // ---- GT4's parts: crops of the -hud picture (a part moved when the picture was taken: from where it was drawn)
        for (int gi = 0; gi < kGt4Count; ++gi)
        {
            const Gt4Part &p = kGt4[gi];
            g_raceHudGt4Dp[gi] = R(0, 0, 0, 0);
            if (!gt4Shown(p) || !gt4InView(p))
                continue;
            bool fromSet = false;
            const Rectangle r = gt4Rect(p, fromSet);
            if (r.width <= 0.0f)
                continue;
            g_raceHudGt4Dp[gi] = r;
            if (fromSet && hudHud_.id && partsSet == set)
            {
                // (cut out of the -hud picture; the mirror whole: it is a picture of its own)
                const Texture2D &tx = hudCut_.id && p.hideBit >= 0 ? hudCut_ : hudHud_;
                const Rectangle *fr = partsSet->shownPart(p.id);
                const Rectangle src{fr->x * tx.width, fr->y * tx.height, fr->width * tx.width, fr->height * tx.height};
                DrawTexturePro(tx, src, ui_.px(r), Vector2{0, 0}, 0, WHITE);
            }
            else if (fromSet && hudHud_.id && own)
            {
                // (the player's picture: GT4's parts from the chase set's own -hud picture are not loaded; a frame shows where)
                ui_.round(r, 3, ui::alpha(BLACK, 0.45f));
                ui_.text(p.name, r.x + 4, r.y + 2, 11, ui::alpha(WHITE, 0.8f));
            }
            else
            {
                ui_.round(r, 3, ui::alpha(BLACK, 0.55f));
                ui_.text(p.name, r.x + r.width * 0.5f, r.y + r.height * 0.5f - 7, 11, ui::alpha(WHITE, 0.8f), Weight::Regular, Align::Center);
            }
            if (hudSel_ == p.id)
            {
                ui_.roundLine(R(r.x - 2, r.y - 2, r.width + 4, r.height + 4), 4, 2, t.accent);
                ui_.chip(r.x - 2, r.y - 20, p.name, t.accent, Color{5, 33, 30, 255}, 10.5f);
            }
            else if (ui_.hovered(r))
                ui_.roundLine(R(r.x - 1, r.y - 1, r.width + 2, r.height + 2), 4, 1, ui::alpha(WHITE, 0.35f));
        }

        // ---- our boxes, with sample numbers (the race map with the picture's course art when GT4.exe's pages folder has it)
        const bool haveArt = f.liveWidgets[static_cast<size_t>(kFullHudMap)].on && raceHudArt(partsSet ? partsSet->course : 81);
        for (const OwnBox &b : kOwn)
        {
            if (!ownOn(b))
                continue;
            const Rectangle r = ownRect(b);
            g_raceHudBoxDp[static_cast<size_t>(&b - kOwn)] = r;
            const float alpha = b.widget < 0 ? 0.85f : 1.0f - std::clamp(f.liveWidgets[static_cast<size_t>(b.widget)].seeThrough, 0, 90) / 100.0f;
            const int style = b.widget < 0 ? 0 : f.liveWidgets[static_cast<size_t>(b.widget)].style;
            const float W = r.width;
            if (b.widget == 0)
                radarSample(ui_, r, std::clamp(f.radarBg, 0, 100) / 100.0f, style, f, static_cast<float>(f.radarRange), W / S.height * 1080.0f);
            else if (b.widget >= 1 && b.widget <= 3)
                drawOldBoxSample(ui_, b.widget, r, alpha, style, 0, 1);
            else if (b.widget >= 4)
                drawNewBoxSample(ui_, b.widget, r, f.liveWidgets[static_cast<size_t>(b.widget)], alpha, S.height, haveArt ? &hudArt_ : nullptr, hudArtW_,
                                 hudArtH_, hudArtLine_);
            else
                drawOldBoxSample(ui_, -1, r, 0.85f, 0, std::clamp(f.startCount, 3, 5), f.startLook == 1 ? 2 : 1);
            const bool autoPlaced = b.widget >= 0 && !f.liveWidgets[static_cast<size_t>(b.widget)].xy && f.liveWidgets[static_cast<size_t>(b.widget)].place == 7;
            if (autoPlaced)
                ui_.chip(r.x + r.width - 34, r.y + 2, "auto", ui::alpha(BLACK, 0.6f), t.accent, 9.5f);
            if (hudSel_ == b.id)
            {
                ui_.roundLine(R(r.x - 2, r.y - 2, r.width + 4, r.height + 4), 4, 2, t.accent);
                ui_.chip(r.x - 2, r.y - 20, b.name, t.accent, Color{5, 33, 30, 255}, 10.5f);
            }
            else if (ui_.hovered(r))
                ui_.roundLine(R(r.x - 1, r.y - 1, r.width + 2, r.height + 2), 4, 1, ui::alpha(t.accent, 0.7f));
        }

        // ---- the mouse and the keys: pick, drag, resize
        auto moveOwn = [&](const OwnBox &b, float cxFrac, float cyFrac) {
            const Rectangle r = ownRect(b);
            const float hw = r.width * 0.5f / S.width, hh = r.height * 0.5f / S.height;
            const float x = std::round(std::clamp(cxFrac, std::min(hw, 0.5f), std::max(1.0f - hw, 0.5f)) * 1000.0f) / 10.0f;
            const float y = std::round(std::clamp(cyFrac, std::min(hh, 0.5f), std::max(1.0f - hh, 0.5f)) * 1000.0f) / 10.0f;
            if (b.widget < 0)
                f.lightsX = x, f.lightsY = y;
            else
            {
                FullHudWidget &h = f.liveWidgets[static_cast<size_t>(b.widget)];
                h.xy = true; // (dragging: "Where I put it")
                h.x = x, h.y = y;
            }
            markDirty();
        };
        auto resizeOwn = [&](const OwnBox &b, int d) {
            int &sz = b.widget < 0 ? f.lightsSize : f.liveWidgets[static_cast<size_t>(b.widget)].size;
            const Rectangle r = ownRect(b);
            const float cx = (r.x + r.width * 0.5f - S.x) / S.width, cy = (r.y + r.height * 0.5f - S.y) / S.height;
            sz = std::clamp(sz + d, 40, 250);
            // (the centre kept where it is)
            if (b.widget < 0 || f.liveWidgets[static_cast<size_t>(b.widget)].xy)
                moveOwn(b, cx, cy);
            markDirty();
        };
        // GT4's parts (GT4_HUD_MOVE): moved by their centre, kept inside the picture; a resize keeps the centre.
        auto moveGt4 = [&](const Gt4Part &p, float cxFrac, float cyFrac) {
            bool fs = false;
            const Rectangle r = gt4Rect(p, fs);
            if (r.width <= 0.0f)
                return;
            const float hw = r.width * 0.5f / S.width, hh = r.height * 0.5f / S.height;
            FullHudMove &mv = gt4Move(p);
            if (!mv.on)
                mv.size = 100;
            mv.on = true;
            mv.x = std::round(std::clamp(cxFrac, std::min(hw, 0.5f), std::max(1.0f - hw, 0.5f)) * 1000.0f) / 10.0f;
            mv.y = std::round(std::clamp(cyFrac, std::min(hh, 0.5f), std::max(1.0f - hh, 0.5f)) * 1000.0f) / 10.0f;
            markDirty();
        };
        auto resizeGt4 = [&](const Gt4Part &p, int d) {
            bool fs = false;
            const Rectangle r = gt4Rect(p, fs);
            if (r.width <= 0.0f)
                return;
            FullHudMove &mv = gt4Move(p);
            const int size = std::clamp((mv.on ? mv.size : 100) + d, 40, 250);
            mv.on = true;
            mv.size = size;
            moveGt4(p, (r.x + r.width * 0.5f - S.x) / S.width, (r.y + r.height * 0.5f - S.y) / S.height);
        };
        const OwnBox *selOwn = nullptr;
        for (const OwnBox &b : kOwn)
            if (hudSel_ == b.id)
                selOwn = &b;
        if (ui_.inputOn() && !ui_.popupOpen())
        {
            const Vector2 m = ui_.mouseDp();
            const ui::Input &in = ui_.input();
            const OwnBox *hotOwn = nullptr;
            for (const OwnBox &b : kOwn)
                if (ownOn(b) && CheckCollisionPointRec(m, ownRect(b)))
                    hotOwn = &b; // (the last drawn is on top)
            const Gt4Part *hotGt4 = nullptr;
            for (const Gt4Part &p : kGt4)
            {
                bool fs = false;
                const Rectangle r = gt4Rect(p, fs);
                if (gt4Shown(p) && gt4InView(p) && r.width > 0 && CheckCollisionPointRec(m, r))
                    hotGt4 = &p;
            }
            if (in.pressed && CheckCollisionPointRec(m, S))
            {
                if (hotOwn)
                {
                    hudSel_ = hotOwn->id;
                    hudDrag_ = hotOwn->id;
                    const Rectangle r = ownRect(*hotOwn);
                    hudDragOff_ = Vector2{m.x - (r.x + r.width * 0.5f), m.y - (r.y + r.height * 0.5f)};
                }
                else if (hotGt4)
                {
                    hudSel_ = hotGt4->id;
                    hudDrag_ = hotGt4->id;
                    bool fs = false;
                    const Rectangle r = gt4Rect(*hotGt4, fs);
                    hudDragOff_ = Vector2{m.x - (r.x + r.width * 0.5f), m.y - (r.y + r.height * 0.5f)};
                }
            }
            if (!in.down)
                hudDrag_.clear();
            if (!hudDrag_.empty())
                for (const Gt4Part &p : kGt4)
                    if (hudDrag_ == p.id)
                    {
                        bool fs = false;
                        const Rectangle r = gt4Rect(p, fs);
                        const float cxNow = r.x + r.width * 0.5f, cyNow = r.y + r.height * 0.5f;
                        const float cx = m.x - hudDragOff_.x, cy = m.y - hudDragOff_.y;
                        if (std::fabs(cx - cxNow) > 0.25f || std::fabs(cy - cyNow) > 0.25f)
                            moveGt4(p, (cx - S.x) / S.width, (cy - S.y) / S.height);
                    }
            if (!hudDrag_.empty())
                for (const OwnBox &b : kOwn)
                    if (hudDrag_ == b.id)
                    {
                        const Rectangle r = ownRect(b);
                        const float cxNow = r.x + r.width * 0.5f, cyNow = r.y + r.height * 0.5f;
                        const float cx = m.x - hudDragOff_.x, cy = m.y - hudDragOff_.y;
                        if (std::fabs(cx - cxNow) > 0.25f || std::fabs(cy - cyNow) > 0.25f)
                            moveOwn(b, (cx - S.x) / S.width, (cy - S.y) / S.height);
                    }
            if (in.wheel != 0.0f && hotOwn)
            {
                hudSel_ = hotOwn->id;
                resizeOwn(*hotOwn, in.wheel > 0 ? 5 : -5);
            }
            else if (in.wheel != 0.0f && hotGt4)
            {
                hudSel_ = hotGt4->id;
                resizeGt4(*hotGt4, in.wheel > 0 ? 5 : -5);
            }
            for (const OwnBox &b : kOwn)
                if (hudSel_ == b.id)
                    selOwn = &b;
            if (selOwn && ownOn(*selOwn))
            {
                const float step = IsKeyDown(KEY_LEFT_SHIFT) || IsKeyDown(KEY_RIGHT_SHIFT) ? 0.02f : 0.005f;
                float dx = 0, dy = 0;
                if (IsKeyPressed(KEY_LEFT) || IsKeyPressedRepeat(KEY_LEFT))
                    dx = -step;
                if (IsKeyPressed(KEY_RIGHT) || IsKeyPressedRepeat(KEY_RIGHT))
                    dx = step;
                if (IsKeyPressed(KEY_UP) || IsKeyPressedRepeat(KEY_UP))
                    dy = -step;
                if (IsKeyPressed(KEY_DOWN) || IsKeyPressedRepeat(KEY_DOWN))
                    dy = step;
                if (dx != 0.0f || dy != 0.0f)
                {
                    const Rectangle r = ownRect(*selOwn);
                    moveOwn(*selOwn, (r.x + r.width * 0.5f - S.x) / S.width + dx, (r.y + r.height * 0.5f - S.y) / S.height + dy);
                }
                if (IsKeyPressed(KEY_EQUAL) || IsKeyPressed(KEY_KP_ADD))
                    resizeOwn(*selOwn, 5);
                if (IsKeyPressed(KEY_MINUS) || IsKeyPressed(KEY_KP_SUBTRACT))
                    resizeOwn(*selOwn, -5);
            }
            const Gt4Part *selGt4 = nullptr;
            for (const Gt4Part &p : kGt4)
                if (hudSel_ == p.id && gt4Shown(p) && gt4InView(p))
                    selGt4 = &p;
            if (selGt4)
            {
                const float step = IsKeyDown(KEY_LEFT_SHIFT) || IsKeyDown(KEY_RIGHT_SHIFT) ? 0.02f : 0.005f;
                float dx = 0, dy = 0;
                if (IsKeyPressed(KEY_LEFT) || IsKeyPressedRepeat(KEY_LEFT))
                    dx = -step;
                if (IsKeyPressed(KEY_RIGHT) || IsKeyPressedRepeat(KEY_RIGHT))
                    dx = step;
                if (IsKeyPressed(KEY_UP) || IsKeyPressedRepeat(KEY_UP))
                    dy = -step;
                if (IsKeyPressed(KEY_DOWN) || IsKeyPressedRepeat(KEY_DOWN))
                    dy = step;
                if (dx != 0.0f || dy != 0.0f)
                {
                    bool fs = false;
                    const Rectangle r = gt4Rect(*selGt4, fs);
                    moveGt4(*selGt4, (r.x + r.width * 0.5f - S.x) / S.width + dx, (r.y + r.height * 0.5f - S.y) / S.height + dy);
                }
                if (IsKeyPressed(KEY_EQUAL) || IsKeyPressed(KEY_KP_ADD))
                    resizeGt4(*selGt4, 5);
                if (IsKeyPressed(KEY_MINUS) || IsKeyPressed(KEY_KP_SUBTRACT))
                    resizeGt4(*selGt4, -5);
            }
        }

        // ---- the picture bar
        float by = S.y + S.height + 10;
        const int n = static_cast<int>(hudSets_.size());
        if (ui_.button("hud-prev", R(x0, by, 38, 32), "<", Style::Secondary, n > 1 || own))
        {
            f.hudPicture.clear();
            hudSetIdx_ = n ? (hudSetIdx_ + n - 1) % n : 0;
            markDirty();
        }
        if (ui_.button("hud-next", R(x0 + 44, by, 38, 32), ">", Style::Secondary, n > 1 || own))
        {
            f.hudPicture.clear();
            hudSetIdx_ = n ? (hudSetIdx_ + 1) % n : 0;
            markDirty();
        }
        if (ui_.button("hud-own", R(x0 + 88, by, 118, 32), "Own picture...", Style::Secondary, !hidden_))
        {
            static const wchar_t filter[] = L"Pictures (*.jpg, *.png)\0*.jpg;*.jpeg;*.png\0All files\0*.*\0";
            const fs::path p = sys::browseForFile(GetWindowHandle(), L"A picture to lay the HUD out on", std::wstring(filter, std::size(filter)),
                                                  own ? pathFrom(f.hudPicture) : raceHudFolder());
            if (!p.empty())
            {
                f.hudPicture = pathText(p);
                markDirty();
            }
        }
        if (ui_.button("hud-latest", R(x0 + 212, by, 104, 32), "Latest race", Style::Secondary, n > 0))
        {
            f.hudPicture.clear();
            raceHudLoadSets(true);
            hudSetIdx_ = 0;
            markDirty();
        }
        {
            std::string cap;
            if (own)
                cap = "Your own picture (GT4's parts shown as in the chase view)";
            else if (set && set->stock)
                cap = "Before your first race: GT4's own chase view, Autumn Ring Mini";
            else if (set)
            {
                const char *cn = courseName(set->course);
                const std::string vw = set->view == "far" ? "far chase" : set->view;
                cap = (hudSetIdx_ == 0 ? std::string("The latest race: ") : std::string()) + (cn ? std::string(cn) : strf("Course %d", set->course)) + ", " + vw +
                      " view  (" + std::to_string(hudSetIdx_ + 1) + " of " + std::to_string(n) + ")";
            }
            ui_.text(ui_.fit(cap, x0 + leftW - (x0 + 328), 13), x0 + 328, by + 8, 13, t.muted);
        }
        by += 44;
        ui_.wrap("GT4 keeps a picture from each race, taken 20 seconds in (the hud-pictures folder next to GT4.exe, the last 16). GT4's own parts "
                 "are cut from the same frame, so they look as they do in a race. Keys: the arrows move the picked box (Shift: bigger steps), + and "
                 "- resize it.",
                 x0, by, leftW, 12.5f, t.muted);
        by += 54;
        {
            // What this page writes to gt4-start.env (as the mock-up shows it), when there is room for it.
            std::vector<std::string> env;
            const float A = S.width / S.height;
            for (int i = 0; i < kFullHudLiveWidgets; ++i)
                env.push_back(std::string("GT4_HUD_") + kFullHudLiveKeys[i] + "=" + fullHudValue(i, f.liveWidgets[static_cast<size_t>(i)], A));
            env.push_back(strf("GT4_START_LIGHTS=%d   GT4_START_COUNT=%d   GT4_START_LOOK=%s", f.startLights ? 1 : 0, std::clamp(f.startCount, 3, 5),
                               f.startLook == 1 ? "two" : "row"));
            env.push_back(strf("GT4_START_LIGHTS_BOX=%.1f,%.1f,%d", f.lightsX, f.lightsY, std::clamp(f.lightsSize, 40, 250)));
            env.push_back("GT4_HUD_HIDE=" + fullHudHideList(f.hudHide));
            env.push_back(std::string("GT4_MIRROR=") + (f.mirrorMode == 1 ? "both" : f.mirrorMode == 2 ? "page" : "screen"));
            env.push_back("GT4_HUD_MOVE=" + fullHudMoveList(f.hudMoves));
            const float boxH = 26 + 17.0f * env.size();
            if (by + boxH < bottom)
            {
                const Rectangle eb = R(x0, by, leftW, boxH);
                ui_.round(eb, 8, ui::mixColor(t.bg, BLACK, 0.3f));
                ui_.roundLine(eb, 8, 1, t.border);
                ui_.text("# gt4-start.env (what this page writes)", x0 + 12, by + 8, 11.5f, t.faint, Weight::Mono);
                float ey = by + 26;
                for (const std::string &l : env)
                {
                    ui_.text(ui_.fit(l, leftW - 24, 12, Weight::Mono), x0 + 12, ey, 12, Color{0x9f, 0xd9, 0xcf, 255}, Weight::Mono);
                    ey += 17;
                }
            }
        }

        // ---- the boxes card
        const Rectangle card = R(x1 - colW, y0, colW, bottom - y0);
        ui_.round(card, 12, t.panel);
        ui_.roundLine(card, 12, 1, t.border);
        const Rectangle view2 = R(card.x, card.y + 8, card.width, card.height - 16);
        const float off = ui_.beginScroll("racehud-col", view2);
        float y = card.y + 16 - off;
        const float cx0 = card.x + 16, cw = card.width - 32;
        ui_.text("Boxes", cx0, y, 15, t.text, Weight::Semibold);
        y += 28;
        fullui::caps(ui_, cx0, y, "YOURS", t.faint);
        y += 20;
        for (const OwnBox &b : kOwn)
        {
            bool on = ownOn(b);
            const bool cur = hudSel_ == b.id;
            if (cur)
                ui_.round(R(cx0 - 6, y - 3, cw + 12, 28), 6, t.panel2);
            if (ui_.checkbox(std::string("hud-tick-") + b.id, cx0, y, on, ""))
            {
                if (b.widget < 0)
                    f.startLights = on;
                else
                    f.liveWidgets[static_cast<size_t>(b.widget)].on = on;
                hudSel_ = b.id;
                markDirty();
            }
            const Rectangle nameR = R(cx0 + 30, y - 2, cw - 30, 26);
            ui_.text(b.name, cx0 + 30, y + 1, 13.5f, on ? t.text : t.muted);
            if (ui_.clicked(nameR))
                hudSel_ = b.id;
            y += 28;
        }
        y += 6;
        fullui::caps(ui_, cx0, y, "GT4'S OWN", t.faint);
        y += 20;
        for (const Gt4Part &p : kGt4)
        {
            bool on = gt4Shown(p);
            const bool cur = hudSel_ == p.id;
            if (isPiece(p))
            {
                // (the chase views' compact panel's pieces, under "Speedo panel": picked here, ticked with it; shown in a chase set)
                if (!pieceView)
                    continue;
                if (cur)
                    ui_.round(R(cx0 + 24, y - 3, cw - 18, 26), 6, t.panel2);
                ui_.text(p.name, cx0 + 44, y + 1, 12.5f, on ? t.text : t.muted);
                if (ui_.clicked(R(cx0 + 30, y - 2, cw - 30, 24)))
                    hudSel_ = p.id;
                y += 24;
                continue;
            }
            if (cur)
                ui_.round(R(cx0 - 6, y - 3, cw + 12, 28), 6, t.panel2);
            // (HUD hand-off 10: no "moved" mark on a part GT4_HUD_MOVE places; it read as moved to another tab)
            if (ui_.checkbox(std::string("hud-gt4-") + p.id, cx0, y, on, ""))
            {
                if (p.hideBit < 0)
                    f.mirrorMode = on ? (f.mirrorMode == 1 ? 1 : 0) : 2;
                else
                    f.hudHide = on ? f.hudHide & ~(1 << p.hideBit) : f.hudHide | (1 << p.hideBit);
                hudSel_ = p.id;
                markDirty();
            }
            ui_.text(p.name, cx0 + 30, y + 1, 13.5f, on ? t.text : t.muted);
            if (ui_.clicked(R(cx0 + 30, y - 2, cw - 30, 26)))
                hudSel_ = p.id;
            y += 28;
        }
        // ---- the picked box's settings
        y += 8;
        ui_.fill(R(cx0, y, cw, 1), t.border);
        y += 12;
        const OwnBox *ob = nullptr;
        const Gt4Part *gp = nullptr;
        for (const OwnBox &b : kOwn)
            if (hudSel_ == b.id)
                ob = &b;
        for (const Gt4Part &p : kGt4)
            if (hudSel_ == p.id)
                gp = &p;
        auto sliderRow = [&](const std::string &id, const char *label, int &value, int lo, int hi, int step, bool enabled) {
            ui_.text(label, cx0, y + 3, 13, t.muted);
            float v = static_cast<float>(value);
            if (ui_.slider(id, R(cx0 + 92, y + 2, cw - 92 - 50, 22), v, static_cast<float>(lo), static_cast<float>(hi), static_cast<float>(step), enabled))
            {
                value = static_cast<int>(std::lround(v));
                markDirty();
                return true;
            }
            ui_.text(strf("%d%%", value), cx0 + cw, y + 3, 13, t.text, Weight::Regular, Align::Right);
            y += 32;
            return false;
        };
        if (ob)
        {
            ui_.text(ob->name, cx0, y, 15, t.accent, Weight::Semibold);
            y += 28;
            if (ob->widget < 0)
            {
                int sz = f.lightsSize;
                if (sliderRow("hud-l-size", "Size", sz, 40, 250, 5, true))
                    resizeOwn(*ob, sz - f.lightsSize);
                ui_.text("Lights", cx0, y + 7, 13, t.muted);
                int c = std::clamp(f.startCount, 3, 5) - 3;
                if (ui_.segmented("hud-l-count", R(cx0 + 92, y, cw - 92, 30), {"3", "4", "5"}, c, true, 12.5f))
                {
                    f.startCount = c + 3;
                    markDirty();
                }
                y += 38;
                ui_.text("Look", cx0, y + 7, 13, t.muted);
                int look = f.startLook == 1 ? 1 : 0;
                if (ui_.segmented("hud-l-look", R(cx0 + 92, y, cw - 92, 30), {"One row", "Two rows"}, look, true, 12.5f))
                {
                    f.startLook = look;
                    markDirty();
                }
                y += 38;
                y += ui_.wrap("Only before a standing start: they follow GT4's own countdown and go out at the start.", cx0, y, cw, 12, t.muted) + 8;
            }
            else
            {
                FullHudWidget &h = f.liveWidgets[static_cast<size_t>(ob->widget)];
                int sz = h.size;
                if (sliderRow("hud-w-size", "Size", sz, 40, 250, 5, true))
                    resizeOwn(*ob, sz - h.size);
                if (ob->widget == 0)
                {
                    // The radar: its background's opacity (GT4_RADAR_BG, the Radar tab's Background), kept with the see-through.
                    if (sliderRow("hud-w-see", "Background", f.radarBg, 0, 100, 1, true))
                        h.seeThrough = std::clamp(100 - f.radarBg, 0, 90);
                }
                else
                    sliderRow("hud-w-see", "See-through", h.seeThrough, 0, 90, 5, true);
                if (ob->widget == kFullHudMap)
                {
                    // (HUD hand-off 10: the Telemetry screen's Map choice, Detailed or Simple, above the View row)
                    ui_.text("Detail", cx0, y + 7, 13, t.muted);
                    int d = h.simple ? 1 : 0;
                    if (ui_.segmented("hud-w-detail", R(cx0 + 92, y, cw - 92, 30), {"Detailed", "Simple"}, d, true, 12))
                    {
                        h.simple = d == 1;
                        markDirty();
                    }
                    y += 38;
                }
                if (kFullHudStyleKeys[ob->widget][0])
                {
                    static const char *const kStyleLabels[kFullHudLiveWidgets][2] = {
                        {"Round", "Square"}, {"Bar and number", "Number only"}, {"", ""}, {"With trail", "Dot only"}, {"", ""}, {"Locked", "Rotating"},
                        {"", ""},            {"Lights and bar", "Lights only"}, {"", ""}, {"", ""},                   {"Lap", "Lap and race time"}};
                    ui_.text(ob->widget == kFullHudMap ? "View" : "Style", cx0, y + 7, 13, t.muted);
                    int stl = std::clamp(h.style, 0, 1);
                    if (ui_.segmented("hud-w-style", R(cx0 + 92, y, cw - 92, 30), {kStyleLabels[ob->widget][0], kStyleLabels[ob->widget][1]}, stl, true, 12))
                    {
                        h.style = stl;
                        markDirty();
                    }
                    y += 38;
                }
                if (ob->widget == kFullHudTower)
                {
                    // (HUD hand-off 7 C: the tower's name, columns and rows)
                    ui_.text("Name", cx0, y + 7, 13, t.muted);
                    int nm = std::clamp(h.towerName, 0, 2);
                    if (ui_.segmented("hud-t-name", R(cx0 + 92, y, cw - 92, 30), {"Car", "Short", "Driver"}, nm, true, 12))
                    {
                        h.towerName = nm;
                        markDirty();
                    }
                    y += 38;
                    ui_.text("Columns", cx0, y + 3, 13, t.muted);
                    static const char *const kColLabels[4] = {"Gap", "Interval", "Best lap", "Last lap"};
                    for (int c = 0; c < 4; ++c)
                    {
                        bool on = (h.towerCols >> c) & 1;
                        const float bx = cx0 + 92 + (c % 2) * ((cw - 92) * 0.5f), byy = y + (c / 2) * 28;
                        if (ui_.checkbox(std::string("hud-t-col-") + gt4hudbox::kTowerCols[c], bx, byy, on, kColLabels[c]))
                        {
                            h.towerCols = on ? h.towerCols | (1 << c) : h.towerCols & ~(1 << c);
                            moveOwn(*ob, h.x / 100.0f, h.y / 100.0f); // (its width changes: kept inside the picture)
                            markDirty();
                        }
                    }
                    y += 60;
                    int rows = std::clamp(h.towerRows, 4, 16);
                    ui_.text("Rows", cx0, y + 3, 13, t.muted);
                    float rv = static_cast<float>(rows);
                    if (ui_.slider("hud-t-rows", R(cx0 + 92, y + 2, cw - 92 - 50, 22), rv, 4.0f, 16.0f, 1.0f, true))
                    {
                        h.towerRows = static_cast<int>(std::lround(rv));
                        moveOwn(*ob, h.x / 100.0f, h.y / 100.0f);
                        markDirty();
                    }
                    ui_.text(std::to_string(h.towerRows), cx0 + cw, y + 3, 13, t.text, Weight::Regular, Align::Right);
                    y += 32;
                }
                if (ob->widget == kFullHudTower || ob->widget == kFullHudSplits)
                {
                    // (the same settings on the Timing tab, with a close-up and the Race Lab's drivers)
                    if (ui_.button("hud-timing-tab", R(cx0, y, cw, 30), "More on the Timing tab", Style::Secondary, true, 13))
                    {
                        raceHudTab_ = 3;
                        ui_.closePopups();
                        hudDrag_.clear();
                    }
                    y += 38;
                }
                if (ob->widget == 0 || ob->widget == 1)
                {
                    ui_.text("Place", cx0, y + 7, 13, t.muted);
                    int pl = !h.xy && h.place == 7 ? 0 : 1;
                    if (ui_.segmented("hud-w-place", R(cx0 + 92, y, cw - 92, 30), {"Automatic", "Where I put it"}, pl, true, 12))
                    {
                        if (pl == 0)
                            h.xy = false, h.place = 7;
                        else
                        {
                            const Rectangle r = ownRect(*ob);
                            h.xy = true;
                            h.x = std::round((r.x + r.width * 0.5f - S.x) / S.width * 1000.0f) / 10.0f;
                            h.y = std::round((r.y + r.height * 0.5f - S.y) / S.height * 1000.0f) / 10.0f;
                        }
                        markDirty();
                    }
                    y += 38;
                }
                const char *note = ob->widget == 0 ? "Automatic: bottom right, lifted over GT4's gear and speed box in the chase views."
                                   : ob->widget == 1 ? "Automatic: above GT4's dash in the bumper and bonnet views, at the top centre in the chase views."
                                   : ob->widget == 2 ? "Throttle and brake."
                                   : ob->widget == 3 ? "The g-forces on the car (1.5 g at the rim)."
                                   : ob->widget == kFullHudTower
                                       ? "Every car by place, with its colour dot (yours gold), as on the phone and top-screen pages. Short: a three-letter "
                                         "code from the car's name. Driver: the drivers sent from the Race Lab (yours: You; an AI car without one keeps its "
                                         "car's name). The rows stay around you."
                                   : ob->widget == kFullHudMap
                                       ? "GT4's own course art with every car's dot (yours gold and bigger). Simple: only the course's line, as on "
                                         "the phone. Rotating: a round map around you, the way you are going always up. A course without art shows "
                                         "the line the cars drive. The Telemetry screen's Track map is a box of its own."
                                   : ob->widget == kFullHudSplits
                                       ? "GT4's three timing lines a lap. Purple: the quickest of every car this race; green: your best; yellow: slower "
                                         "than your best. The sector you are in counts up."
                                   : ob->widget == kFullHudRevs
                                       ? "Ten shift lights from 85 % to 98 % of the rev limit; all flash at the limiter. The limit is the highest rpm "
                                         "held for half a second this race."
                                   : ob->widget == kFullHudSpeed ? "In GT4's own unit (its Options: km/h or mph)."
                                   : ob->widget == kFullHudGear  ? "N in neutral, R in reverse."
                                                                 : "Our own lap counter; GT4's own can stay, move or hide. Lap and race time adds the race "
                                                                   "time. The Telemetry screen's Lap counter is a box of its own.";
                y += ui_.wrap(note, cx0, y, cw, 12, t.muted) + 8;
            }
        }
        else if (gp)
        {
            ui_.text(gp->name, cx0, y, 15, t.accent, Weight::Semibold);
            y += 28;
            // GT4_HUD_MOVE: its size (100 % where GT4 puts it) and the way back.
            FullHudMove &mv = gt4Move(*gp);
            bool fsNow = false;
            const bool drawnHere = gt4Rect(*gp, fsNow).width > 0.0f && gt4Shown(*gp) && gt4InView(*gp);
            {
                int sz = mv.on ? mv.size : 100;
                const int before = sz;
                if (sliderRow("hud-g-size", "Size", sz, 40, 250, 5, drawnHere))
                    resizeGt4(*gp, sz - before);
            }
            if (ui_.button("hud-g-back", R(cx0, y, cw, 30), "Back to GT4's place", Style::Secondary, mv.on))
            {
                mv = FullHudMove{};
                markDirty();
            }
            y += 38;
            if (mv.on)
                y += ui_.wrap(strf("Centre %.1f %% across, %.1f %% down, %d %%.", mv.x, mv.y, mv.size), cx0, y, cw, 12, t.muted) + 6;
            if (gp->hideBit < 0)
            {
                ui_.text("Shows in", cx0, y + 7, 13, t.muted);
                int every = f.mirrorMode == 1 ? 1 : 0;
                if (ui_.segmented("hud-mirror-every", R(cx0 + 92, y, cw - 92, 30), {"GT4's views", "Every view"}, every, f.mirrorMode != 2, 12))
                {
                    f.mirrorMode = every;
                    markDirty();
                }
                y += 38;
                y += ui_.wrap("GT4 draws its mirror in the bumper view; Every view draws it in the others too. Unticked: off on the screen, still on a "
                              "phone (Multi-screen). Moved, GT4's own place shows the road (Vulkan only).",
                              cx0, y, cw, 12, t.muted) + 8;
            }
            else
            {
                bool fs = false;
                const Rectangle r = gt4Rect(*gp, fs);
                const char *note = pieceView && std::strcmp(gp->id, "panel") == 0
                                       ? "In the chase views the panel is four pieces (listed under it): pick one to move it. Untick to hide them."
                                   : r.width <= 0 ? "GT4 does not draw it in this view. Untick to hide it in races."
                                   : isPiece(*gp)  ? "One piece of the chase views' panel: it moves on its own. Untick Speedo panel to hide the panel."
                                   : std::strcmp(gp->id, "panel") == 0 ? "The bumper and bonnet views' dash. In the chase views its pieces move one by one."
                                                                        : "Drag it, or the mouse wheel to resize. Untick to hide it in races.";
                y += ui_.wrap(note, cx0, y, cw, 12, t.muted) + 8;
            }
        }
        y += 6;
        bool allHidden = (f.hudHide & 0x3F) == 0x3F && f.mirrorMode == 2;
        if (ui_.button("hud-hide-all", R(cx0, y, cw, 34), allHidden ? "Show GT4's own HUD" : "Hide GT4's own HUD", allHidden ? Style::Primary : Style::Secondary))
        {
            if (allHidden)
                f.hudHide = 0, f.mirrorMode = 0;
            else
                f.hudHide = 0x3F, f.mirrorMode = 2;
            markDirty();
        }
        y += 42;
        if (ui_.button("hud-reset", R(cx0, y, cw, 34), "Reset layout", Style::Secondary))
        {
            for (int i = 0; i < kFullHudLiveWidgets; ++i)
                f.liveWidgets[static_cast<size_t>(i)] = kFullHudLiveDefaults[i];
            f.lightsX = kFullLightsX, f.lightsY = kFullLightsY, f.lightsSize = 100;
            f.startLook = 0;
            f.hudHide = 0;
            f.hudMoves = FullHudMoves{};
            f.mirrorMode = f.mirrorMode == 2 ? 0 : f.mirrorMode;
            markDirty();
        }
        y += 46;
        ui_.endScroll("racehud-col", y + off - card.y);
    }

    // ---- Driving > Race HUD > Radar (HUD hand-off 7 D; since 20 Oct 2026 GT3's radar settings: one range 10-80 m, car and
    // background opacity, GT3's eleven colours for your car, the other cars, alongside and the background, and a reset)
    namespace
    {
        // The position presets (the radar's centre, % of the HUD area, for its size at the player's picture shape A): 0 Automatic
        // (auto,auto), 1 bottom right, 2 bottom left, 3 top right below GT4's best and last lap, 4 top left below GT4's map, 5
        // bottom centre, 6 above GT4's dash. GT4's best / last lap and map boxes are the stock chase picture's (hud-pictures .json:
        // best and last lap down to 23.4 % of the height, its right edge at 93.2 % across; the map to 57.4 %, its left edge at 4.0 %).
        constexpr int kRadarPresets = 7;
        const char *const kRadarPresetNames[kRadarPresets] = {"Automatic",
                                                              "Bottom right",
                                                              "Bottom left",
                                                              "Top right, below GT4's best and last lap",
                                                              "Top left, below GT4's map",
                                                              "Bottom centre",
                                                              "Above GT4's dash"};
        void radarPresetCentre(int preset, int size, float A, float &x, float &y)
        {
            const float bh = 0.24f * std::clamp(size, 40, 250) / 100.0f, bw = bh / A; // (fractions of the height / the width)
            static const int kPlace[kRadarPresets] = {-1, 0, 1, -1, -1, 4, 6};
            if (preset == 3 || preset == 4)
            {
                x = preset == 3 ? 0.932f - bw * 0.5f : 0.040f + bw * 0.5f;
                y = (preset == 3 ? 0.234f : 0.574f) + 0.015f + bh * 0.5f;
                x = std::round(std::clamp(x, bw * 0.5f, 1.0f - bw * 0.5f) * 1000.0f) / 10.0f;
                y = std::round(std::clamp(y, bh * 0.5f, 1.0f - bh * 0.5f) * 1000.0f) / 10.0f;
                return;
            }
            fullHudPlaceCentre(0, kPlace[std::clamp(preset, 1, kRadarPresets - 1)], size, A, x, y);
            x = std::round(x * 10.0f) / 10.0f, y = std::round(y * 10.0f) / 10.0f;
        }
        // GT3's four colour rows (gt3_launcher.cpp pageRadar), each GT4_RADAR_COLOURS' colour (the third, close, is not offered:
        // it is written as the other cars' colour).
        const char *const kRadarColourNames[4] = {"Your car", "Other cars", "Alongside", "Background"};
        const int kRadarColourSlot[4] = {0, 1, 3, 4};
    }

    void App::pageRaceHudRadar(float top)
    {
        const ui::Theme &t = ui::theme();
        FullSettings &f = st_.full;
        FullHudWidget &h = f.liveWidgets[0];
        const float x0 = kSidebarW + 32, x1 = ui_.width - 32, bottom = ui_.height - 48;
        const float colW = 360.0f, gap = 22.0f;
        const float leftW = x1 - x0 - colW - gap;
        const float A = fullAspectRatioOf(f.aspect);
        const float bgAlpha = std::clamp(f.radarBg, 0, 100) / 100.0f;

        // ---- the preview: the stock chase picture (GT4's own HUD as a race draws it) with the radar where it goes
        static RaceHudSet stock;
        static bool stockRead = false;
        if (!stockRead)
        {
            stockRead = true;
            const std::vector<uint8_t> js = sys::resource(GT4FULL_IDR_HUD_STOCK_JSON);
            parseRaceHudJson(std::string(js.begin(), js.end()), stock);
        }
        if (!radarStockHud_.id)
        {
            const std::vector<uint8_t> bytes = sys::resource(GT4FULL_IDR_HUD_STOCK_HUD);
            if (!bytes.empty())
            {
                Image img = LoadImageFromMemory(".jpg", bytes.data(), static_cast<int>(bytes.size()));
                if (img.data)
                {
                    radarStockHud_ = LoadTextureFromImage(img);
                    SetTextureFilter(radarStockHud_, TEXTURE_FILTER_BILINEAR);
                    UnloadImage(img);
                }
            }
        }
        const float closeH = std::clamp((bottom - top) * 0.36f, 150.0f, 250.0f);
        float sw = leftW, sh = sw / (16.0f / 9.0f);
        if (sh > bottom - top - closeH - 40.0f)
        {
            sh = std::max(120.0f, bottom - top - closeH - 40.0f);
            sw = sh * 16.0f / 9.0f;
        }
        const Rectangle S = R(x0, top, sw, sh);
        ui_.fill(R(S.x - 1, S.y - 1, S.width + 2, S.height + 2), t.border);
        ui_.fill(S, BLACK);
        if (radarStockHud_.id)
            DrawTexturePro(radarStockHud_, Rectangle{0, 0, static_cast<float>(radarStockHud_.width), static_cast<float>(radarStockHud_.height)}, ui_.px(S),
                           Vector2{0, 0}, 0, WHITE);
        // The radar's box in the picture: as GT4.exe places it (gt4_hud.cpp draw): its centre, or Automatic's place in a chase view
        // (bottom right, lifted over GT4's gear and speed box unless GT4's panel is hidden).
        const float size = std::clamp(h.size, 40, 250) / 100.0f, bh = 0.24f * size * S.height, bw = bh;
        auto radarRect = [&]() {
            float cx = 0.5f, cy = 0.5f;
            if (h.xy)
                cx = h.x / 100.0f, cy = h.y / 100.0f;
            else if (h.place == 7)
            {
                const bool panelHidden = (f.hudHide >> kFullHudHidePanel) & 1;
                cx = 1.0f - (0.02f * S.height + bw * 0.5f) / S.width;
                cy = (panelHidden ? 1.0f - 0.02f : 1.0f - 0.14f) - bh * 0.5f / S.height;
            }
            else
            {
                fullHudPlaceCentre(0, h.place, h.size, S.width / S.height, cx, cy);
                cx /= 100.0f, cy /= 100.0f;
            }
            Rectangle r = R(S.x + cx * S.width - bw * 0.5f, S.y + cy * S.height - bh * 0.5f, bw, bh);
            r.x = std::clamp(r.x, S.x, S.x + S.width - r.width);
            r.y = std::clamp(r.y, S.y, S.y + S.height - r.height);
            return r;
        };
        const Rectangle rr = radarRect();
        if (h.on)
        {
            radarSample(ui_, rr, bgAlpha, h.style, f, static_cast<float>(f.radarRange), bh / S.height * 1080.0f);
            ui_.roundLine(R(rr.x - 2, rr.y - 2, rr.width + 4, rr.height + 4), 4, ui_.hovered(rr) || !hudDrag_.empty() ? 2.0f : 1.0f,
                          ui::alpha(t.accent, ui_.hovered(rr) || !hudDrag_.empty() ? 1.0f : 0.6f));
            g_raceHudBoxDp[0] = rr;
        }
        else
        {
            ui_.round(R(S.x + S.width * 0.5f - 150, S.y + S.height * 0.5f - 18, 300, 36), 8, ui::alpha(BLACK, 0.6f));
            ui_.text("The radar is off", S.x + S.width * 0.5f, S.y + S.height * 0.5f - 9, 14, t.text, Weight::Semibold, Align::Center);
        }
        // Drag it in the picture (as on the HUD layout page: "Where I put it").
        auto moveTo = [&](float cxFrac, float cyFrac) {
            const float hw = bw * 0.5f / S.width, hh = bh * 0.5f / S.height;
            h.xy = true;
            h.x = std::round(std::clamp(cxFrac, std::min(hw, 0.5f), std::max(1.0f - hw, 0.5f)) * 1000.0f) / 10.0f;
            h.y = std::round(std::clamp(cyFrac, std::min(hh, 0.5f), std::max(1.0f - hh, 0.5f)) * 1000.0f) / 10.0f;
            markDirty();
        };
        if (ui_.inputOn() && !ui_.popupOpen() && h.on)
        {
            const Vector2 m = ui_.mouseDp();
            const ui::Input &in = ui_.input();
            if (in.pressed && CheckCollisionPointRec(m, rr))
            {
                hudDrag_ = "radar";
                hudDragOff_ = Vector2{m.x - (rr.x + rr.width * 0.5f), m.y - (rr.y + rr.height * 0.5f)};
            }
            if (!in.down)
                hudDrag_.clear();
            if (hudDrag_ == "radar")
            {
                const float cx = m.x - hudDragOff_.x, cy = m.y - hudDragOff_.y;
                if (std::fabs(cx - (rr.x + rr.width * 0.5f)) > 0.25f || std::fabs(cy - (rr.y + rr.height * 0.5f)) > 0.25f)
                    moveTo((cx - S.x) / S.width, (cy - S.y) / S.height);
            }
            if (in.wheel != 0.0f && CheckCollisionPointRec(m, rr))
            {
                h.size = std::clamp(h.size + (in.wheel > 0 ? 5 : -5), 40, 250);
                markDirty();
            }
        }
        ui_.text("GT4's own chase view with your radar: drag it to move it, the mouse wheel to resize it.", x0, S.y + S.height + 8, 12.5f, t.muted);

        // ---- the close-up at the range, with what each colour is
        const float cy0 = S.y + S.height + 32;
        const float cs = std::min(closeH, bottom - cy0);
        const Rectangle C = R(x0, cy0, cs, cs);
        radarSample(ui_, C, std::max(bgAlpha, 0.6f), h.style, f, static_cast<float>(f.radarRange), 1080.0f);
        const float tx = C.x + C.width + 20, tw = x0 + leftW - tx;
        float ty = cy0;
        ui_.text(strf("Close-up: %d m", f.radarRange), tx, ty, 14, t.text, Weight::Semibold);
        ty += 24;
        ty += ui_.wrap(strf("The radar looks %d m around your car: the rings are at %d, %d and %d m. Cars alongside (under 6 m) and the bars "
                            "at the sides are in the Alongside colour, every other car in Other cars.",
                            f.radarRange, static_cast<int>(std::lround(f.radarRange / 3.0)), static_cast<int>(std::lround(f.radarRange * 2 / 3.0)),
                            f.radarRange),
                       tx, ty, tw, 12.5f, t.muted) + 8;
        if (h.on && f.radarShow == 1)
            ty += ui_.wrap("Cars near: it fades in while a car is inside the range and out 2 s after the last one leaves.", tx, ty, tw, 12.5f, t.muted) + 8;
        ty += ui_.wrap("The phone and top-screen radars use the same range and colours.", tx, ty, tw, 12.5f, t.muted) + 10;
        {
            // What this page writes to gt4-start.env, when there is room for it.
            std::array<uint32_t, 5> cols = f.radarColours;
            cols[2] = cols[1];
            std::vector<std::string> env = {"GT4_HUD_RADAR=" + fullHudValue(0, h, A),
                                            strf("GT4_RADAR_SHOW=%s   GT4_RADAR_RANGE=%d", f.radarShow == 1 ? "near" : "always", f.radarRange),
                                            strf("GT4_RADAR_CARS=%d   GT4_RADAR_BG=%d", f.radarCars, f.radarBg),
                                            "GT4_RADAR_COLOURS=" + fullRadarColourList(cols),
                                            std::string("GT4_RADAR_LABELS=") + (f.radarLabels ? "on" : "off") + "   GT4_RADAR_ZOOM=off"};
            const float boxH = 26 + 16.0f * env.size();
            if (ty + boxH <= bottom && tw > 260)
            {
                const Rectangle eb = R(tx, ty, tw, boxH);
                ui_.round(eb, 8, ui::mixColor(t.bg, BLACK, 0.3f));
                ui_.roundLine(eb, 8, 1, t.border);
                ui_.text("# gt4-start.env (what this page writes)", tx + 12, ty + 8, 11.5f, t.faint, Weight::Mono);
                float ey = ty + 26;
                for (const std::string &l : env)
                {
                    ui_.text(ui_.fit(l, tw - 24, 11.5f, Weight::Mono), tx + 12, ey, 11.5f, Color{0x9f, 0xd9, 0xcf, 255}, Weight::Mono);
                    ey += 16;
                }
            }
        }

        // ---- the settings card, in GT3's order: show, position, across, down, size, range, car opacity, background, the colours
        const Rectangle card = R(x1 - colW, top, colW, bottom - top);
        ui_.round(card, 12, t.panel);
        ui_.roundLine(card, 12, 1, t.border);
        const Rectangle view2 = R(card.x, card.y + 8, card.width, card.height - 16);
        const float off = ui_.beginScroll("radar-col", view2);
        float y = card.y + 14 - off;
        const float cx0 = card.x + 16, cw = card.width - 32, lx = cx0 + 92, lw = cw - 92;
        auto label = [&](const char *s, float dy = 7.0f) { ui_.text(s, cx0, y + dy, 13, t.muted); };
        auto slider = [&](const std::string &id, const char *name, float &value, float lo, float hi, float step, const std::string &shownText, bool enabled) {
            label(name, 3);
            const bool ch = ui_.slider(id, R(lx, y + 2, lw - 56, 22), value, lo, hi, step, enabled);
            ui_.text(shownText, cx0 + cw, y + 3, 13, enabled ? t.text : t.faint, Weight::Regular, Align::Right);
            y += 31;
            return ch;
        };
        fullui::caps(ui_, cx0, y, "THE RADAR", t.faint);
        y += 20;
        label("Show");
        int show = !h.on ? 0 : f.radarShow == 1 ? 2 : 1;
        if (ui_.segmented("radar-show", R(lx, y, lw, 30), {"Off", "Always", "Cars near"}, show, true, 12))
        {
            h.on = show != 0;
            if (show)
                f.radarShow = show == 2 ? 1 : 0;
            markDirty();
        }
        y += 36;
        label("Shape");
        int shape = std::clamp(h.style, 0, 1);
        if (ui_.segmented("radar-shape", R(lx, y, lw, 30), {"Round", "Square"}, shape, h.on, 12))
        {
            h.style = shape;
            markDirty();
        }
        y += 36;
        label("Position");
        int preset = !h.xy && h.place == 7 ? 0 : -1;
        if (preset < 0)
            for (int p = 1; p < kRadarPresets; ++p)
            {
                float px = 0, py = 0;
                radarPresetCentre(p, h.size, A, px, py);
                float hx = h.x, hy = h.y;
                if (!h.xy)
                    fullHudPlaceCentre(0, h.place, h.size, A, hx, hy);
                if (std::fabs(px - hx) < 0.15f && std::fabs(py - hy) < 0.15f)
                    preset = p;
            }
        const float placeY = y;
        y += 36;
        {
            // Across / Down: the same x and y (the box's centre in % of the picture); Automatic shows where it goes in a chase view.
            float ax = (rr.x + rr.width * 0.5f - S.x) / S.width * 100.0f, ay = (rr.y + rr.height * 0.5f - S.y) / S.height * 100.0f;
            if (h.xy)
                ax = h.x, ay = h.y;
            if (slider("radar-x", "Across", ax, 0.0f, 100.0f, 0.5f, strf("%.1f %%", ax), h.on))
                moveTo(ax / 100.0f, (h.xy ? h.y : ay) / 100.0f);
            if (slider("radar-y", "Down", ay, 0.0f, 100.0f, 0.5f, strf("%.1f %%", ay), h.on))
                moveTo((h.xy ? h.x : ax) / 100.0f, ay / 100.0f);
            float sz = static_cast<float>(h.size);
            if (slider("radar-size", "Size", sz, 40.0f, 250.0f, 5.0f, strf("%d %%", h.size), h.on))
            {
                h.size = static_cast<int>(std::lround(sz));
                markDirty();
            }
        }
        // GT3's range, car opacity and background opacity
        {
            float rg = static_cast<float>(f.radarRange);
            if (slider("radar-range", "Range", rg, 10.0f, 80.0f, 1.0f, strf("%d m", f.radarRange), h.on))
            {
                f.radarRange = std::clamp(static_cast<int>(std::lround(rg)), 10, 80);
                markDirty();
            }
            float cars = static_cast<float>(f.radarCars);
            if (slider("radar-cars", "Car opacity", cars, 10.0f, 100.0f, 1.0f, strf("%d %%", f.radarCars), h.on))
            {
                f.radarCars = std::clamp(static_cast<int>(std::lround(cars)), 10, 100);
                markDirty();
            }
            float bg = static_cast<float>(f.radarBg);
            if (slider("radar-bg", "Background", bg, 0.0f, 100.0f, 1.0f, strf("%d %%", f.radarBg), h.on))
            {
                f.radarBg = std::clamp(static_cast<int>(std::lround(bg)), 0, 100);
                h.seeThrough = std::clamp(100 - f.radarBg, 0, 90); // (GT4_HUD_RADAR's see-through kept with it)
                markDirty();
            }
        }
        y += 3;
        fullui::caps(ui_, cx0, y, "COLOURS", t.faint);
        y += 20;
        {
            // GT3's eleven colours in a row each (a colour from the file that is not one of them shows as a twelfth).
            // (the same pitch in every row: twelve places when any row has a colour of its own)
            const float sx0 = cx0 + 80, area = cx0 + cw - sx0;
            auto inPalette = [&](uint32_t v) {
                for (uint32_t c : kFullRadarPaletteRgb)
                    if (c == v)
                        return true;
                return false;
            };
            int places = kFullRadarPalette;
            for (int k = 0; k < 4; ++k)
                if (!inPalette(f.radarColours[static_cast<size_t>(kRadarColourSlot[k])]))
                    places = kFullRadarPalette + 1;
            const float pitch = area / places, sws = std::min(22.0f, pitch - 5.0f);
            for (int k = 0; k < 4; ++k)
            {
                const size_t slot = static_cast<size_t>(kRadarColourSlot[k]);
                ui_.text(kRadarColourNames[k], cx0, y + 3, 13, t.muted);
                const int n = inPalette(f.radarColours[slot]) ? kFullRadarPalette : kFullRadarPalette + 1;
                for (int j = 0; j < n; ++j)
                {
                    const uint32_t c = j < kFullRadarPalette ? kFullRadarPaletteRgb[j] : f.radarColours[slot];
                    const Rectangle sr = R(sx0 + 2 + j * pitch, y + 1, sws, sws);
                    const bool sel = c == f.radarColours[slot];
                    ui_.round(sr, 4, hex(c, h.on ? 1.0f : 0.5f));
                    ui_.roundLine(sr, 4, 1.0f, ui_.hovered(sr) && h.on ? t.muted : ui::alpha(WHITE, 0.25f));
                    if (sel) // (the chosen one: a white ring round it, as GT3 marks it)
                        ui_.roundLine(R(sr.x - 2.5f, sr.y - 2.5f, sr.width + 5, sr.height + 5), 6, 2.0f, h.on ? WHITE : t.muted);
                    if (h.on && ui_.clicked(sr))
                    {
                        f.radarColours[slot] = c;
                        if (slot == 1)
                            f.radarColours[2] = c; // (close cars: the other cars' colour)
                        markDirty();
                    }
                }
                y += 27;
            }
        }
        y += 4;
        bool labels = f.radarLabels;
        if (ui_.checkbox("radar-labels", cx0, y, labels, "Show metres on the rings", h.on))
        {
            f.radarLabels = labels;
            markDirty();
        }
        y += 32;
        if (ui_.button("radar-reset", R(cx0, y, cw, 30), "Reset radar to defaults", Style::Secondary, true, 13))
        {
            // GT3's reset: show, place, size, range, opacities and colours (and here the shape and the metres on the rings).
            h.on = true, h.xy = false, h.place = 7, h.size = 100, h.style = 0;
            f.radarShow = 0;
            f.radarRange = kFullRadarRangeDefault;
            f.radarCars = kFullRadarCarsDefault;
            f.radarBg = kFullRadarBgDefault;
            h.seeThrough = std::clamp(100 - f.radarBg, 0, 90);
            f.radarLabels = true;
            for (int k = 0; k < 5; ++k)
                f.radarColours[static_cast<size_t>(k)] = kFullRadarColours[k];
            markDirty();
        }
        y += 40;
        // The position list last, so its open list is drawn over the rows below it.
        {
            const float yy = y;
            y = placeY;
            std::vector<std::string> opts(kRadarPresetNames, kRadarPresetNames + kRadarPresets);
            int sel = preset;
            if (preset < 0)
                opts.push_back("Where I put it"), sel = kRadarPresets;
            if (ui_.dropdown("radar-place", R(lx, y, lw, 30), opts, sel, h.on, 12.5f) && sel < kRadarPresets)
            {
                if (sel == 0)
                    h.xy = false, h.place = 7;
                else
                {
                    radarPresetCentre(sel, h.size, A, h.x, h.y);
                    h.xy = true;
                }
                markDirty();
            }
            y = yy;
        }
        ui_.endScroll("radar-col", y + off - card.y);
    }

    void App::pageRaceHudLights(float top)
    {
        // Driving > Race HUD > Start lights (HUD hand-off 7 E): GT4.exe's start lights (gt4_hud.h) and holding the start
        // (gt4_start_hold.h): the lights-out wait, the AI's reaction, your car held or free, the jump start.
        const ui::Theme &t = ui::theme();
        FullSettings &f = st_.full;
        const float x0 = kSidebarW + 32, x1 = ui_.width - 32, bottom = ui_.height - 48;
        const float colW = 330.0f, gap = 22.0f;
        const float leftW = x1 - x0 - colW - gap;
        const int n = std::clamp(f.startCount, 3, 5);

        // ---- the settings (left), scrolling
        const Rectangle view = R(x0, top, leftW, bottom - top);
        const float off = ui_.beginScroll("lights-form", view);
        float y = top - off;
        const float lw = std::min(170.0f, leftW * 0.3f), cx = x0 + lw, cw = std::min(leftW - lw - 22.0f, 620.0f); // (22: clear of the scroll bar)
        auto label = [&](const char *s) { ui_.text(s, x0, y + 6, 14.5f, t.text); };
        label("Grid starts");
        int mode = f.startLights ? 1 : 0;
        if (ui_.segmented("lights-mode", R(cx, y, cw, 32), {"GT4's countdown", "Start lights"}, mode, true, 13))
        {
            f.startLights = mode == 1;
            markDirty();
        }
        y += 38;
        y += ui_.wrap("Start lights: they come on one a second and all go out on the start, in place of GT4's 3 2 1. Where they sit is "
                      "set on HUD layout.",
                      cx, y, cw, 12.5f, t.muted) + 14;
        label("Lights");
        int c = n - 3;
        if (ui_.segmented("lights-count", R(cx, y, std::min(cw, 240.0f), 32), {"3", "4", "5"}, c, true, 13))
        {
            f.startCount = c + 3;
            markDirty();
        }
        y += 38;
        y += ui_.wrap("How many lights come on before they go out.", cx, y, cw, 12.5f, t.muted) + 14;
        label("Look");
        int look = f.startLook == 1 ? 1 : 0;
        if (ui_.segmented("lights-look", R(cx, y, std::min(cw, 320.0f), 32), {"One row", "Two rows"}, look, f.startLights, 13))
        {
            f.startLook = look;
            markDirty();
        }
        y += 50;

        // ---- holding the start (built in stage E: the box is live, no longer the mock-up's greyed "Next step")
        const float by0 = y;
        const float bw = lw + cw;
        y += 16;
        const float ix = x0 + 16, ilw = std::min(150.0f, lw), icx = ix + ilw, icw = bw - 32 - ilw;
        ui_.text("Holding the start", ix, y, 15, t.accent, Weight::Semibold);
        y += 26;
        y += ui_.wrap("These hold GT4's cars and clock at a standing start, so they change how the race runs. Offline races only: LAN "
                      "races, replays and rolling starts start as GT4 does.",
                      ix, y, bw - 32, 12.5f, t.muted) + 12;
        auto ilabel = [&](const char *s, const char *small = nullptr) {
            ui_.text(s, ix, y + 6, 14, t.text);
            if (small)
                ui_.text(small, ix, y + 25, 11.5f, t.faint);
        };
        ilabel("Hold the start");
        int hold = f.startHold ? 1 : 0;
        if (ui_.segmented("lights-hold", R(icx, y, std::min(icw, 320.0f), 32), {"Off", "On"}, hold, true, 13))
        {
            f.startHold = hold == 1;
            markDirty();
        }
        y += 38;
        y += ui_.wrap(f.startHold ? "On: the settings below are used in every offline race with a standing start."
                                  : "Off: GT4's own start (the settings below are kept for when it is on).",
                      icx, y, icw, 12.5f, t.muted) + 14;
        const bool on = f.startHold;
        ilabel("Lights out", "after the last one");
        {
            float a = f.startDelayMin, b = f.startDelayMax;
            const float sw = icw - 70;
            if (ui_.slider("lights-min", R(icx, y + 4, sw, 22), a, 0.0f, 5.0f, 0.1f, on))
            {
                f.startDelayMin = std::round(a * 10.0f) / 10.0f;
                f.startDelayMax = std::max(f.startDelayMax, f.startDelayMin);
                markDirty();
            }
            ui_.text(strf("%.1f s", f.startDelayMin), icx + icw, y + 5, 13, on ? t.text : t.faint, Weight::Regular, Align::Right);
            y += 30;
            if (ui_.slider("lights-max", R(icx, y + 4, sw, 22), b, 0.0f, 5.0f, 0.1f, on))
            {
                f.startDelayMax = std::round(b * 10.0f) / 10.0f;
                f.startDelayMin = std::min(f.startDelayMin, f.startDelayMax);
                markDirty();
            }
            ui_.text(strf("%.1f s", f.startDelayMax), icx + icw, y + 5, 13, on ? t.text : t.faint, Weight::Regular, Align::Right);
            y += 32;
            y += ui_.wrap("Somewhere in between each start, so you can't time it (F1: 0.2 to 3 s).", icx, y, icw, 12.5f, t.muted) + 14;
        }
        ilabel("AI reaction");
        int ai = std::clamp(f.startAi, 0, 3);
        if (ui_.segmented("lights-ai", R(icx, y, icw, 32), {"Instant", "Quick", "Normal", "Slow"}, ai, on, 13))
        {
            f.startAi = ai;
            markDirty();
        }
        y += 38;
        static const char *const kAiNote[] = {"Instant: GT4's own, every AI car at once.", "Quick: each AI car waits 0.1 to 0.3 s after the lights go out.",
                                              "Normal: each AI car waits 0.2 to 0.6 s after the lights go out.",
                                              "Slow: each AI car waits 0.4 to 1.0 s after the lights go out."};
        y += ui_.wrap(kAiNote[ai], icx, y, icw, 12.5f, t.muted) + 14;
        ilabel("Your car");
        int car = f.startFree ? 1 : 0;
        if (ui_.segmented("lights-car", R(icx, y, std::min(icw, 320.0f), 32), {"Held", "Free"}, car, on, 13))
        {
            f.startFree = car == 1;
            markDirty();
        }
        y += 38;
        y += ui_.wrap(f.startFree ? strf("Free: yours from the first light (%d s before GT4's own start; hold the brake to rev) while the AI cars "
                                         "stay held. Moving more than half a metre before the lights go out is a jump start.",
                                         n)
                                  : std::string("Held: GT4 holds your car with the others until the lights go out."),
                      icx, y, icw, 12.5f, t.muted) + 14;
        ilabel("Jump start");
        int jump = f.startJump == 1 ? 1 : 0;
        if (ui_.segmented("lights-jump", R(icx, y, std::min(icw, 320.0f), 32), {"Held 5 s", "Held 10 s"}, jump, on && f.startFree, 13))
        {
            f.startJump = jump;
            markDirty();
        }
        y += 38;
        y += ui_.wrap("Your car is stopped where it is and can't move until 5 or 10 s after the lights go out. Race-time penalties (+5 or "
                      "+10 s) are not offered: GT4's results board puts the cars in the order they cross the line, so added time would "
                      "not match it.",
                      icx, y, icw, 12.5f, t.muted) + 16;
        ui_.roundLine(R(x0, by0, bw, y - by0), 12, 1, on ? ui::alpha(t.accent, 0.7f) : t.border);
        y += 16;
        ui_.endScroll("lights-form", y + off - top);

        // ---- Try a start (launcher only) and what this page writes, on the right
        const float rx = x1 - colW;
        float ry = top;
        ui_.text("Try a start", rx, ry, 14, t.muted);
        ry += 24;
        const double now = GetTime();
        const bool running = lightsTryAt_ > -1e8; // (-1e9: not started; GetTime() can be under 2.5 s in the test pictures)
        int lit = 0;
        bool out = false;
        if (running)
        {
            lit = std::clamp(static_cast<int>(now - lightsTryAt_) + 1, 0, n);
            out = now >= lightsTryOut_;
            if (out && now - lightsTryOut_ > 3.0)
            {
                lightsTryAt_ = -1e9;
                lightsTryResult_ = "Too slow: no Go! within 3 s.";
            }
        }
        {
            const int rows = f.startLook == 1 && f.startLights ? 2 : 1;
            const float cell = 40.0f;
            const Rectangle lb = R(rx, ry, cell * n + cell * 0.25f * (n + 1), cell * rows);
            gt4hudlook::lights(ui_.px(lb), n, rows, running && !out ? lit : 0, 1.0f);
            ry += lb.height + 12;
        }
        if (ui_.button("lights-try", R(rx, ry, 90, 32), "Start", Style::Secondary, true, 13))
        {
            // The random wait: the Lights out range on the left (the hand-off's 0.2 to 3 s while the hold is off).
            const float a = f.startHold ? f.startDelayMin : 0.2f, b = f.startHold ? std::max(f.startDelayMax, f.startDelayMin) : 3.0f;
            const float wait = a + (b - a) * static_cast<float>(GetRandomValue(0, 1000)) / 1000.0f;
            lightsTryAt_ = now;
            lightsTryOut_ = now + n + wait;
            lightsTryResult_.clear();
        }
        const bool go = ui_.button("lights-go", R(rx + 100, ry, 90, 32), "Go!", Style::Primary, running, 13) ||
                        (running && ui_.inputOn() && IsKeyPressed(KEY_SPACE));
        if (go && running)
        {
            lightsTryResult_ = out ? strf("Reaction: %.3f s", now - lightsTryOut_) : std::string("Jump start!");
            lightsTryAt_ = -1e9;
        }
        ry += 44;
        if (!lightsTryResult_.empty())
            ui_.text(lightsTryResult_, rx, ry, 22, lightsTryResult_[0] == 'R' ? t.text : t.warn, Weight::Semibold);
        ry += 36;
        ry += ui_.wrap("Press Start, then Go! (or the space bar) the moment the lights go out. The random wait uses the Lights out range.",
                       rx, ry, colW, 12.5f, t.muted) + 14;
        {
            static const char *const kAi[] = {"instant", "quick", "normal", "slow"};
            const std::vector<std::string> env = {
                strf("GT4_START_LIGHTS=%d   GT4_START_COUNT=%d", f.startLights ? 1 : 0, n),
                std::string("GT4_START_LOOK=") + (f.startLook == 1 ? "two" : "row"),
                std::string("GT4_START_HOLD=") + (f.startHold ? "on" : "off"),
                strf("GT4_START_DELAY=%.1f,%.1f", f.startDelayMin, std::max(f.startDelayMax, f.startDelayMin)),
                std::string("GT4_START_AI=") + kAi[std::clamp(f.startAi, 0, 3)],
                std::string("GT4_START_CAR=") + (f.startFree ? "free" : "held") + "   GT4_START_JUMP=" + (f.startJump == 1 ? "h10" : "h5")};
            const float boxH = 26 + 16.0f * env.size();
            if (ry + boxH <= bottom)
            {
                const Rectangle eb = R(rx, ry, colW, boxH);
                ui_.round(eb, 8, ui::mixColor(t.bg, BLACK, 0.3f));
                ui_.roundLine(eb, 8, 1, t.border);
                ui_.text("# gt4-start.env (what this page writes)", rx + 12, ry + 8, 11.5f, t.faint, Weight::Mono);
                float ey = ry + 26;
                for (const std::string &l : env)
                {
                    ui_.text(ui_.fit(l, colW - 24, 11.5f, Weight::Mono), rx + 12, ey, 11.5f, Color{0x9f, 0xd9, 0xcf, 255}, Weight::Mono);
                    ey += 16;
                }
            }
        }
    }
}
