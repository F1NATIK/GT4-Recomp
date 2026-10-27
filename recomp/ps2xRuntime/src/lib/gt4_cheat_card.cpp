#include "gt4_cheat_card.h"

#include "gt4_cheat_card_rules.h"
#include "present_overlay.h"
#include "present_scanline.h"
#include "ps2x/iop/gt4_cheats_served.h"
#include "raylib.h"
#include "runtime/gt4_cheat_codes.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <map>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

// The in-game cheat card (gt4_cheat_card.h). Drawn with raylib shapes and Segoe UI only (no
// textures, no Sony artwork), in the look of the GT4 Recompilation launcher (gt4stock_ui.h) and
// of the mock-ups (analysis/cheats/mockup/render_mockups.py: in_game, in_game_window, pad_glyph).
// What it says about GT4_CHEATS and whose presses it follows: gt4_cheat_card_rules.h.

namespace gt4cheatcard
{
    namespace
    {
        using Clock = std::chrono::steady_clock;
        using Event = gt4cheats::Follower::Event;

        // ---------------------------------------------------------------- theme (gt4stock_ui.h)
        constexpr Color kPanel{22, 27, 35, 255};
        constexpr Color kPanel3{40, 48, 62, 255};
        constexpr Color kBorder{44, 52, 66, 255};
        constexpr Color kText{232, 236, 242, 255};
        constexpr Color kMuted{154, 164, 178, 255};
        constexpr Color kFaint{104, 114, 130, 255};
        constexpr Color kAccent{76, 141, 255, 255};
        constexpr Color kOk{63, 185, 80, 255};
        constexpr Color kWarn{227, 179, 65, 255};
        constexpr Color kBad{248, 81, 73, 255};
        constexpr Color kEdge{60, 70, 88, 255};
        constexpr Color kCrossArm{54, 63, 80, 255};
        constexpr Color kLCol{128, 176, 255, 255};
        constexpr Color kRCol{240, 192, 96, 255};
        constexpr Color kWhite{255, 255, 255, 255};

        Color mix(Color a, Color b, float t)
        {
            auto m = [t](unsigned char x, unsigned char y) { return static_cast<unsigned char>(std::lround(x + (y - x) * t)); };
            return Color{m(a.r, b.r), m(a.g, b.g), m(a.b, b.b), 255};
        }
        Color withAlpha(Color c, unsigned char a) { return Color{c.r, c.g, c.b, a}; }
        Color toneColour(rules::Tone t) { return t == rules::Tone::Ok ? kOk : t == rules::Tone::Bad ? kBad : kWarn; }

        // ---------------------------------------------------------------- settings
        enum class Place
        {
            Auto,
            Left,
            Right,
            TopRight,
        };

        struct Settings
        {
            bool on = false;
            std::vector<const gt4cheats::Code *> list;
            int key = KEY_F2;
            std::string keyName = "F2";
            Place place = Place::Auto;
            bool follow = true;
            uint8_t asked = 0; // GT4_CHEATS, by the overlay's own parser (ps2x/iop/gt4_cheats_served.h)
            // GT4_CHEAT_CARD_TEST (development builds)
            bool test = false;
            int testShow = 1, testProgress = 0;
            bool testReset = false, testDone = false, testLive = false, testAll = false;
            uint64_t testShowAt = 0; // the vsync the test card appears at (0: at once), like an F2 press
            std::string shotPrefix;
            std::vector<std::pair<int, int>> shotSizes;
            uint64_t shotAt = 900;
        };

        std::string env(const char *key)
        {
            const char *v = std::getenv(key);
            return v ? std::string(v) : std::string();
        }
        std::string lower(std::string s)
        {
            for (char &c : s)
                if (c >= 'A' && c <= 'Z')
                    c = char(c - 'A' + 'a');
            return s;
        }
        std::vector<std::string> words(const std::string &text, const char *separators)
        {
            std::vector<std::string> out;
            std::string w;
            for (char c : text)
            {
                if (std::strchr(separators, c))
                {
                    if (!w.empty())
                        out.push_back(w);
                    w.clear();
                }
                else
                    w.push_back(c);
            }
            if (!w.empty())
                out.push_back(w);
            return out;
        }
        std::string askedText(uint8_t asked)
        {
            std::string t;
            if (asked & ps2x::iop::gt4::cheats::ServedNoday)
                t += "noday";
            if (asked & ps2x::iop::gt4::cheats::ServedMissionfix)
                t += std::string(t.empty() ? "" : ", ") + "missionfix";
            if (asked & ps2x::iop::gt4::cheats::ServedSecret)
                t += std::string(t.empty() ? "" : ", ") + "secret";
            return t.empty() ? "off" : t;
        }

        Settings parseSettings()
        {
            Settings s;
            const std::string mode = lower(env("GT4_CHEAT_CARD"));
            if (mode == "key" || mode == "on" || mode == "1")
                s.on = true;
            else if (mode == "auto")
            {
                s.on = true;
                std::cerr << "[cheats] GT4_CHEAT_CARD=auto (the card by itself on the right screen) is not built yet: F2 shows it" << std::endl;
            }
            else if (!mode.empty() && mode != "off" && mode != "0")
                std::cerr << "[cheats] GT4_CHEAT_CARD=" << mode << " is not off / key: the card stays off" << std::endl;

            // The switches as asked; what the card claims follows what the overlay serves (switchesNow()).
            s.asked = ps2x::iop::gt4::cheats::askedSwitches();

            // Absent = all six. An empty value reaches GT4.exe as absent (main.cpp's start file: _putenv_s
            // with "" removes the variable), so "no cards" is written as none (or off).
            const std::string listText = env("GT4_CHEAT_CARD_LIST");
            const std::vector<std::string> listWords = words(lower(listText), ", ;\t");
            const bool none = listWords.size() == 1 && (listWords[0] == "none" || listWords[0] == "off");
            std::vector<std::string> unknown;
            if (!none)
                s.list = gt4cheats::parseList(listText.empty() ? std::string("all") : listText, &unknown);
            for (const std::string &u : unknown)
                std::cerr << "[cheats] GT4_CHEAT_CARD_LIST: '" << u << "' is not a card (credits, licence, test, event, mission, options)" << std::endl;

            if (const std::string k = lower(env("GT4_CHEAT_CARD_KEY")); !k.empty())
            {
                const int n = (k.size() >= 2 && k[0] == 'f') ? std::atoi(k.c_str() + 1) : 0;
                if (n >= 2 && n <= 12)
                {
                    s.key = KEY_F1 + (n - 1);
                    s.keyName = "F" + std::to_string(n);
                }
                else
                    std::cerr << "[cheats] GT4_CHEAT_CARD_KEY=" << k << " is not F2..F12: F2" << std::endl;
            }
            if (const std::string p = lower(env("GT4_CHEAT_CARD_PLACE")); !p.empty())
            {
                if (p == "left")
                    s.place = Place::Left;
                else if (p == "right")
                    s.place = Place::Right;
                else if (p == "top-right" || p == "topright" || p == "corner")
                    s.place = Place::TopRight;
                else if (p != "auto")
                    std::cerr << "[cheats] GT4_CHEAT_CARD_PLACE=" << p << " is not auto / left / right / top-right: auto" << std::endl;
            }
            if (const std::string f = env("GT4_CHEAT_CARD_FOLLOW"); !f.empty())
                s.follow = f != "0";

            const std::string test = env("GT4_CHEAT_CARD_TEST");
#if defined(GT4_STOCK_BUILD)
            if (!test.empty())
                std::cerr << "[cheats] GT4_CHEAT_CARD_TEST is a development switch: ignored in the stock build" << std::endl;
#else
            if (!test.empty())
            {
                s.test = true;
                s.on = true;
                for (const std::string &item : words(test, ","))
                {
                    const size_t colon = item.find(':');
                    const std::string key = lower(item.substr(0, colon));
                    const std::string value = colon == std::string::npos ? std::string() : item.substr(colon + 1);
                    if (key == "show")
                        s.testShow = std::max(1, std::atoi(value.c_str()));
                    else if (key == "progress")
                        s.testProgress = std::max(0, std::atoi(value.c_str()));
                    else if (key == "reset")
                        s.testReset = true;
                    else if (key == "done")
                        s.testDone = true;
                    else if (key == "all")
                        s.testAll = true; // shots of every card in the list (each at progress min(k, its presses))
                    else if (key == "live")
                        s.testLive = true; // the pad replay drives the card (else the test state holds)
                    else if (key == "showat")
                        s.testShowAt = std::strtoull(value.c_str(), nullptr, 10); // hidden until then (pre-warm, then "F2")
                    else if (key == "shot")
                        s.shotPrefix = value;
                    else if (key == "at")
                        s.shotAt = std::strtoull(value.c_str(), nullptr, 10);
                    else if (key == "sizes")
                        for (const std::string &size : words(value, "+;"))
                        {
                            int w = 0, h = 0;
                            if (std::sscanf(size.c_str(), "%dx%d", &w, &h) == 2 && w >= 320 && h >= 200 && w <= 8192 && h <= 8192)
                                s.shotSizes.emplace_back(w, h);
                        }
                }
                if (!s.shotPrefix.empty() && s.shotSizes.empty())
                    s.shotSizes = {{2560, 1440}, {1280, 960}};
            }
#endif
            if (s.on && s.list.empty())
            {
                std::cerr << "[cheats] in-game card: no cards in GT4_CHEAT_CARD_LIST" << (none ? " (" + listWords[0] + ")" : std::string()) << "; off"
                          << std::endl;
                s.on = false;
            }
            if (s.on)
            {
                std::string ids;
                for (const gt4cheats::Code *c : s.list)
                    ids += (ids.empty() ? "" : ",") + std::string(c->id);
                const char *places[] = {"auto", "left", "right", "top-right"};
                std::cerr << "[cheats] in-game card: " << s.keyName << " shows " << s.list.size() << " card(s) (" << ids << "), place "
                          << places[int(s.place)] << ", follow " << (s.follow ? "on (controller 1; controller 2 when fed)" : "off")
                          << "; GT4_CHEATS asked: " << askedText(s.asked) << " (the card says what the overlay serves)"
                          << (s.test ? "; test mode (GT4_CHEAT_CARD_TEST)" : "") << std::endl;
            }
            return s;
        }

        const Settings &settings()
        {
            static const Settings s = parseSettings();
            return s;
        }

        // GT4_CHEATS as GT4 is actually given it (the IOP overlay's result; pending until it is built).
        rules::Switches switchesNow()
        {
            const ps2x::iop::gt4::cheats::ServedState st = ps2x::iop::gt4::cheats::servedState();
            return rules::fromOverlay(settings().asked, st.known, st.served);
        }

        // ---------------------------------------------------------------- state (EE pad poll + presenter)
        std::mutex g_mutex;
        std::atomic<bool> g_following{false};
        std::array<std::atomic<uint16_t>, 2> g_held{};
        std::atomic<bool> g_port1Fed{false}; // something feeds port 1 (the player-2 path); never in a 1-pad build
        struct State
        {
            int index = -1; // the card shown (in settings().list), -1 hidden
            rules::PadFollower follower;
            Clock::time_point resetAt{}, doneAt{};
            bool started = false;
            uint32_t completed = 0, resets = 0;
        } g_state;

        void showLocked(int index)
        {
            const Settings &s = settings();
            g_state.index = index;
            g_state.follower.reset(index >= 0 ? s.list[size_t(index)] : nullptr,
                                   {g_held[0].load(std::memory_order_relaxed), g_held[1].load(std::memory_order_relaxed)});
            g_state.resetAt = {};
            g_state.doneAt = {};
            g_following.store(index >= 0 && s.follow && (!s.test || s.testLive), std::memory_order_release);
        }

        // ---------------------------------------------------------------- fonts (presenter thread)
        // A dry pass (g_dry) lays the cards out without drawing: it loads the fonts they need (the pre-warm).
        bool g_dry = false;

        struct Fonts
        {
            bool tried = false;
            std::vector<unsigned char> data[3]; // the TTF files, read once
            std::map<std::pair<int, int>, Font> cache;
            size_t loads = 0;

            Font get(float size, int weight)
            {
                if (!tried)
                {
                    tried = true;
                    const char *files[3] = {"segoeui.ttf", "seguisb.ttf", "segoeuib.ttf"};
                    for (int w = 0; w < 3; ++w)
                        if (std::ifstream in(std::string("C:\\Windows\\Fonts\\") + files[w], std::ios::binary); in)
                            data[w].assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
                }
                const int px = std::max(6, static_cast<int>(std::lround(size)));
                const std::vector<unsigned char> *file = &data[weight];
                if (file->empty())
                    file = &data[0];
                if (file->empty())
                    return GetFontDefault();
                const auto key = std::make_pair(px, weight);
                if (auto it = cache.find(key); it != cache.end())
                    return it->second;
                std::vector<int> cps;
                for (int c = 32; c < 127; ++c)
                    cps.push_back(c);
                for (int c : {0xb7, 0x2013, 0x2014, 0x2019, 0x2026})
                    cps.push_back(c);
                Font f = LoadFontFromMemory(".ttf", file->data(), static_cast<int>(file->size()), px, cps.data(), static_cast<int>(cps.size()));
                if (f.texture.id == 0)
                    f = GetFontDefault();
                else
                    SetTextureFilter(f.texture, TEXTURE_FILTER_BILINEAR);
                cache[key] = f;
                ++loads;
                return f;
            }
            void unload()
            {
                for (auto &[k, f] : cache)
                    if (f.texture.id != GetFontDefault().texture.id)
                        UnloadFont(f);
                cache.clear();
            }
        } g_fonts;

        enum Weight
        {
            Regular = 0,
            Semibold = 1,
            Bold = 2,
        };

        float textWidth(const std::string &t, float size, int weight)
        {
            const Font f = g_fonts.get(size, weight);
            return MeasureTextEx(f, t.c_str(), std::round(size), 0.0f).x;
        }
        // align: 0 left, 1 centre, 2 right. Returns the width.
        float text(const std::string &t, float x, float y, float size, Color c, int weight, int align = 0)
        {
            const Font f = g_fonts.get(size, weight);
            const float px = std::round(size);
            const float w = MeasureTextEx(f, t.c_str(), px, 0.0f).x;
            const float x0 = align == 1 ? x - w * 0.5f : align == 2 ? x - w : x;
            if (!g_dry)
                DrawTextEx(f, t.c_str(), Vector2{std::round(x0), std::round(y)}, px, 0.0f, c);
            return w;
        }
        std::vector<std::string> wrap(const std::string &t, float maxWidth, float size, int weight)
        {
            std::vector<std::string> lines;
            std::string cur;
            for (const std::string &word : words(t, " "))
            {
                const std::string trial = cur.empty() ? word : cur + " " + word;
                if (cur.empty() || textWidth(trial, size, weight) <= maxWidth)
                    cur = trial;
                else
                {
                    lines.push_back(cur);
                    cur = word;
                }
            }
            if (!cur.empty())
                lines.push_back(cur);
            return lines;
        }

        // ---------------------------------------------------------------- shapes
        void roundRect(float x, float y, float w, float h, float r, Color c)
        {
            if (g_dry || w <= 0 || h <= 0)
                return;
            DrawRectangleRounded(Rectangle{x, y, w, h}, std::clamp(2.0f * r / std::min(w, h), 0.0f, 1.0f), 12, c);
        }
        void roundLine(float x, float y, float w, float h, float r, float thick, Color c)
        {
            if (g_dry || w <= 0 || h <= 0)
                return;
            DrawRectangleRoundedLinesEx(Rectangle{x, y, w, h}, std::clamp(2.0f * r / std::min(w, h), 0.0f, 1.0f), 12, std::max(1.0f, thick), c);
        }
        void rect(Rectangle r, Color c)
        {
            if (!g_dry)
                DrawRectangleRec(r, c);
        }
        void dot(Vector2 at, float radius, Color c)
        {
            if (!g_dry)
                DrawCircleV(at, radius, c);
        }
        void triangle(Vector2 a, Vector2 b, Vector2 c, Color col)
        {
            if (g_dry)
                return;
            // raylib wants the corners counter-clockwise as seen on the screen (y down).
            const float cross = (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x);
            if (cross > 0)
                std::swap(b, c);
            DrawTriangle(a, b, c, col);
        }

        enum class TileState
        {
            Normal,
            Done,
            Next,
        };

        // One PS2 button as a square tile (render_mockups.py pad_glyph, DARK_PAD).
        void tile(uint16_t button, float x, float y, float s, TileState st, Color bg, int number)
        {
            const float r = s * 0.2f;
            Color fill = kPanel3, edge = kEdge;
            if (st == TileState::Done)
            {
                fill = mix(kPanel, kOk, 0.30f);
                edge = kOk;
            }
            if (st == TileState::Next)
                roundRect(x - s * 0.09f, y - s * 0.09f, s * 1.18f, s * 1.18f, r * 1.4f, mix(bg, kAccent, 0.25f));
            roundRect(x, y, s, s, r, fill);
            roundLine(x, y, s, s, r, std::max(1.0f, s * (st == TileState::Next ? 0.05f : 0.025f)), st == TileState::Next ? kAccent : edge);
            const float cx = x + s * 0.5f, cy = y + s * 0.5f;
            const Color glyph = st == TileState::Done ? mix(kOk, kWhite, 0.55f) : kWhite;
            using namespace gt4cheats;
            switch (button)
            {
            case Up:
            case Down:
            case Left:
            case Right:
            {
                const float arm = s * 0.36f, thick = s * 0.24f;
                const Color armCol = st == TileState::Done ? mix(fill, kWhite, 0.12f) : kCrossArm;
                roundRect(cx - thick * 0.5f, cy - arm, thick, arm * 2.0f, s * 0.05f, armCol);
                roundRect(cx - arm, cy - thick * 0.5f, arm * 2.0f, thick, s * 0.05f, armCol);
                const float dx = button == Left ? -1.0f : button == Right ? 1.0f : 0.0f;
                const float dy = button == Up ? -1.0f : button == Down ? 1.0f : 0.0f;
                const float a = s * 0.16f;
                const Vector2 tip{cx + dx * s * 0.34f, cy + dy * s * 0.34f};
                const Vector2 base{cx + dx * s * 0.12f, cy + dy * s * 0.12f};
                const float px = -dy, py = dx;
                triangle(tip, Vector2{base.x + px * a, base.y + py * a}, Vector2{base.x - px * a, base.y - py * a}, glyph);
                break;
            }
            case L1:
            case L2:
            case R1:
            case R2:
            {
                const bool left = button == L1 || button == L2;
                const bool one = button == L1 || button == R1;
                Color col = left ? kLCol : kRCol;
                if (st == TileState::Done)
                    col = mix(col, kWhite, 0.25f);
                const float bw = s * 0.5f;
                if (one)
                    roundRect(cx - bw * 0.5f, y + s * 0.16f, bw, s * 0.09f, s * 0.04f, col);
                else
                {
                    roundRect(cx - bw * 0.5f, y + s * 0.12f, bw, s * 0.16f, s * 0.08f, col);
                    rect(Rectangle{cx - bw * 0.5f, y + s * 0.21f, bw, s * 0.07f}, col);
                }
                text(std::string(buttonName(button)), cx, y + s * 0.33f, s * 0.5f, col, Bold, 1);
                break;
            }
            case Select:
                roundRect(cx - s * 0.2f, y + s * 0.28f, s * 0.4f, s * 0.13f, s * 0.065f, glyph);
                text("SELECT", cx, y + s * 0.53f, s * 0.2f, glyph, Semibold, 1);
                break;
            case Start:
                triangle(Vector2{cx - s * 0.1f, y + s * 0.22f}, Vector2{cx - s * 0.1f, y + s * 0.46f}, Vector2{cx + s * 0.14f, y + s * 0.34f}, glyph);
                text("START", cx, y + s * 0.53f, s * 0.2f, glyph, Semibold, 1);
                break;
            default:
                text(std::string(buttonName(button)), cx, y + s * 0.32f, s * 0.26f, glyph, Semibold, 1);
                break;
            }
            if (number > 0)
                text(std::to_string(number), cx, y + s + s * 0.06f, s * 0.26f, kFaint, Semibold, 1);
        }

        // ---------------------------------------------------------------- what the card shows
        struct View
        {
            const gt4cheats::Code *code = nullptr;
            int n = 0, m = 0;          // card n of m
            int progress = 0;          // presses done
            bool follow = true;        // lights the buttons
            bool notFollowable = false; // the setting is on, but GT4 takes this code only from a controller the card is not fed
            bool done = false;         // the last press just went in (2 s)
            bool flash = false;        // a wrong button just reset it (0.45 s)
            rules::Switches sw;        // GT4_CHEATS as served
            std::string keyName;
        };

        View makeView(const Settings &s, int index, int progress, const rules::Switches &sw, bool port1Fed)
        {
            View v;
            v.code = s.list[size_t(index)];
            v.n = index + 1;
            v.m = int(s.list.size());
            v.progress = progress;
            v.sw = sw;
            v.notFollowable = s.follow && !rules::followable(*v.code, sw, port1Fed);
            v.follow = s.follow && !v.notFollowable;
            v.keyName = s.keyName;
            return v;
        }

        float chip(float x, float y, const std::string &t, Color c, float size, int align)
        {
            const float w = textWidth(t, size, Semibold) + size * 1.3f;
            const float h = size * 1.33f + size * 0.57f;
            const float x0 = align == 2 ? x - w : x;
            roundRect(x0, y, w, h, h * 0.5f, mix(kPanel, c, 0.18f));
            text(t, x0 + size * 0.65f, y + size * 0.28f, size, c, Semibold);
            return w;
        }
        TileState stateOf(const View &v, int index)
        {
            if (!v.follow)
                return TileState::Normal;
            if (v.done || index < v.progress)
                return TileState::Done;
            return index == v.progress ? TileState::Next : TileState::Normal;
        }
        float keycap(float x, float y, const std::string &t, float size)
        {
            const float w = textWidth(t, size, Semibold) + size * 0.9f;
            const float h = size * 1.6f;
            roundRect(x, y, w, h, size * 0.25f, kPanel3);
            roundLine(x, y, w, h, size * 0.25f, std::max(1.0f, size * 0.07f), kEdge);
            text(t, x + w * 0.5f, y + size * 0.22f, size, kText, Semibold, 1);
            return w;
        }

        // The code's buttons in rows: rows[r] = groups, each a list of press indices.
        using Rows = std::vector<std::vector<std::vector<int>>>;
        std::vector<std::vector<int>> groupsOf(const gt4cheats::Code &c)
        {
            std::vector<std::vector<int>> groups;
            int i = 0;
            for (int g = 0; g < c.groupCount; ++g)
            {
                groups.emplace_back();
                for (int k = 0; k < c.groups[size_t(g)] && i < c.count; ++k)
                    groups.back().push_back(i++);
            }
            return groups;
        }
        // One group per row (a long group over several rows).
        Rows rowsOnePerGroup(const gt4cheats::Code &c, int perRow)
        {
            Rows rows;
            for (const auto &g : groupsOf(c))
                for (size_t i = 0; i < g.size(); i += size_t(perRow))
                    rows.push_back({std::vector<int>(g.begin() + std::ptrdiff_t(i), g.begin() + std::ptrdiff_t(std::min(g.size(), i + size_t(perRow))))});
            return rows;
        }
        // Groups packed into rows of maxW (render_mockups.py seq_rows); a group too long for a row is split.
        Rows rowsPacked(const gt4cheats::Code &c, float maxW, float s, float gap, float ggap)
        {
            const int perRow = std::max(1, int((maxW + gap) / (s + gap)));
            Rows rows;
            float w = 0;
            for (const auto &whole : groupsOf(c))
                for (size_t i = 0; i < whole.size(); i += size_t(perRow))
                {
                    std::vector<int> g(whole.begin() + std::ptrdiff_t(i), whole.begin() + std::ptrdiff_t(std::min(whole.size(), i + size_t(perRow))));
                    const float gw = float(g.size()) * s + float(g.size() - 1) * gap;
                    if (rows.empty() || w + ggap + gw > maxW)
                    {
                        rows.push_back({g});
                        w = gw;
                    }
                    else
                    {
                        rows.back().push_back(g);
                        w += ggap + gw;
                    }
                }
            return rows;
        }
        // Returns the height used (with the numbers under the last row).
        float drawRows(const View &v, const Rows &rows, float x, float y, float s, float gap, float ggap, float rowGap, Color bg)
        {
            float yy = y;
            for (const auto &row : rows)
            {
                float xx = x;
                for (size_t gi = 0; gi < row.size(); ++gi)
                {
                    for (size_t k = 0; k < row[gi].size(); ++k)
                    {
                        const int idx = row[gi][k];
                        tile(v.code->presses[size_t(idx)], xx, yy, s, stateOf(v, idx), bg, idx + 1);
                        xx += s + (k + 1 < row[gi].size() ? gap : 0.0f);
                    }
                    if (gi + 1 < row.size())
                    {
                        dot(Vector2{xx + ggap * 0.5f, yy + s * 0.5f}, std::max(1.2f, s * 0.045f), kFaint);
                        xx += ggap;
                    }
                }
                yy += s + rowGap;
            }
            return rows.empty() ? 0.0f : yy - y - rowGap + s * 0.4f;
        }
        float rowsHeight(const Rows &rows, float s, float rowGap)
        {
            return rows.empty() ? 0.0f : float(rows.size()) * (s + rowGap) - rowGap + s * 0.4f;
        }

        std::string progressText(const View &v)
        {
            if (!v.follow)
                return std::to_string(v.code->count) + " presses";
            if (v.done)
                return "Done!";
            return std::to_string(v.progress) + " of " + std::to_string(v.code->count);
        }
        float progressValue(const View &v)
        {
            return v.done ? 1.0f : float(v.progress) / float(std::max<int>(1, v.code->count));
        }
        void meter(float x, float y, float w, float h, float value, Color c)
        {
            roundRect(x, y, w, h, h * 0.5f, kPanel3);
            if (value > 0)
                roundRect(x, y, std::max(h, w * std::clamp(value, 0.0f, 1.0f)), h, h * 0.5f, c);
        }

        // The card in the bar beside a 4:3 picture (mock-up 5): 336 x up to the picture's height at k = 1.
        void drawSide(const View &v, float cx, float top, float maxH, float k)
        {
            const float cw = 336.0f * k, ix = cx + 22.0f * k, iw = cw - 44.0f * k;
            const std::vector<std::string> name = wrap(rules::name(*v.code, v.sw), iw, 38.0f * k, Bold);
            const std::vector<std::string> where =
                wrap(rules::screen(*v.code, v.sw) + "  \xc2\xb7  " + rules::controllers(*v.code, v.sw), iw, 17.0f * k, Regular);
            const float chipY = 50.0f * k + float(name.size()) * 42.0f * k + 8.0f * k + float(where.size()) * 22.0f * k + 12.0f * k;
            const float tilesY = chipY + 14.0f * k * 1.9f + 30.0f * k;
            const float footerH = 150.0f * k;
            // Tiles: 74 px, one group per row; smaller (and packed) when the code needs it to fit.
            float s = 74.0f * k, gap = 12.0f * k, rowGap = 34.0f * k;
            Rows rows;
            for (int attempt = 0; attempt < 12; ++attempt)
            {
                const int perRow = std::max(1, int((iw + gap) / (s + gap)));
                rows = rowsOnePerGroup(*v.code, perRow);
                if (tilesY + rowsHeight(rows, s, rowGap) + 24.0f * k + footerH <= maxH)
                    break;
                const Rows packed = rowsPacked(*v.code, iw, s, gap, gap * 1.6f);
                if (tilesY + rowsHeight(packed, s, rowGap) + 24.0f * k + footerH <= maxH)
                {
                    rows = packed;
                    break;
                }
                s *= 0.92f;
                gap *= 0.92f;
                rowGap *= 0.92f;
            }
            const float ch = std::min(maxH, tilesY + rowsHeight(rows, s, rowGap) + 24.0f * k + footerH);
            roundRect(cx, top, cw, ch, 18.0f * k, withAlpha(kPanel, 236));
            roundLine(cx, top, cw, ch, 18.0f * k, 2.0f * k, v.flash ? kBad : kEdge);

            text("GT4 CHEAT CARD", ix, top + 20.0f * k, 15.0f * k, kFaint, Semibold);
            text(std::to_string(v.n) + " / " + std::to_string(v.m), cx + cw - 22.0f * k, top + 20.0f * k, 15.0f * k, kFaint, Semibold, 2);
            float y = top + 50.0f * k;
            for (const std::string &line : name)
            {
                text(line, ix, y, 38.0f * k, kText, Bold);
                y += 42.0f * k;
            }
            y += 8.0f * k;
            for (const std::string &line : where)
            {
                text(line, ix, y, 17.0f * k, kMuted, Regular);
                y += 22.0f * k;
            }
            const rules::Chip c = rules::chip(*v.code, v.sw);
            chip(ix, top + chipY, c.text, toneColour(c.tone), 14.0f * k, 0);
            drawRows(v, rows, ix, top + tilesY, s, gap, gap * 1.6f, rowGap, kPanel);

            const float fy = top + ch - footerH;
            rect(Rectangle{ix, fy, iw, std::max(1.0f, k)}, kBorder);
            const float pw = text(progressText(v), ix, fy + 16.0f * k, 22.0f * k, v.done ? kOk : kText, Semibold);
            if (v.follow)
            {
                const float mx = ix + std::max(92.0f * k, pw + 14.0f * k);
                meter(mx, fy + 26.0f * k, ix + iw - mx, 8.0f * k, progressValue(v), kOk);
            }
            float ry = fy + 56.0f * k;
            for (const std::string &line : wrap(rules::hint(v.notFollowable), iw, 15.0f * k, Regular))
            {
                text(line, ix, ry, 15.0f * k, v.flash ? kBad : v.notFollowable ? kWarn : kMuted, Regular);
                ry += 19.5f * k;
            }
            const float kx = ix + keycap(ix, fy + 96.0f * k, v.keyName, 15.0f * k) + 8.0f * k;
            text(v.n < v.m ? "next card, then hide" : "hide", kx, fy + 100.0f * k, 15.0f * k, kFaint, Regular);
        }

        // The compact card over the picture's top right corner (mock-up 5b): 442 px wide at k = 1 (960 rows).
        void drawCorner(const View &v, float screenW, float k)
        {
            k = std::min(k, screenW / 490.0f); // 442 wide + 24 each side
            const float cw = 16.0f * k + 9.0f * (40.0f * k + 6.0f * k) + 12.0f * k;
            const float s = 40.0f * k, gap = 6.0f * k, ggap = 16.0f * k, rowGap = 22.0f * k;
            const float x0 = screenW - cw - 24.0f * k, y0 = 24.0f * k;
            const Rows rows = rowsPacked(*v.code, cw - 32.0f * k, s, gap, ggap);
            const float ch = 74.0f * k + rowsHeight(rows, s, rowGap) + 58.0f * k;
            roundRect(x0, y0, cw, ch, 12.0f * k, withAlpha(kPanel, 225));
            roundLine(x0, y0, cw, ch, 12.0f * k, 1.5f * k, v.flash ? kBad : kEdge);
            text(rules::name(*v.code, v.sw), x0 + 16.0f * k, y0 + 12.0f * k, 22.0f * k, kText, Bold);
            const rules::Chip c = rules::chip(*v.code, v.sw);
            chip(x0 + cw - 14.0f * k, y0 + 14.0f * k, c.text, toneColour(c.tone), 12.0f * k, 2);
            text(rules::screen(*v.code, v.sw) + "  \xc2\xb7  " + rules::controllers(*v.code, v.sw) + "  \xc2\xb7  " + std::to_string(v.n) + " / " +
                     std::to_string(v.m) + "  \xc2\xb7  " + v.keyName,
                 x0 + 16.0f * k, y0 + 44.0f * k, 13.0f * k, kMuted, Regular);
            drawRows(v, rows, x0 + 16.0f * k, y0 + 74.0f * k, s, gap, ggap, rowGap, kPanel);
            const float by = y0 + ch - 44.0f * k;
            const float pw = text(progressText(v), x0 + 16.0f * k, by, 14.0f * k, v.done ? kOk : kText, Semibold);
            if (v.follow)
                meter(x0 + 16.0f * k + pw + 12.0f * k, by + 6.0f * k, cw - 44.0f * k - pw, 6.0f * k, progressValue(v), kOk);
            text(rules::hint(v.notFollowable), x0 + 16.0f * k, by + 20.0f * k, 12.0f * k, v.flash ? kBad : v.notFollowable ? kWarn : kFaint, Regular);
        }

        // Where the card goes for a window of screenW x screenH with GT4's picture at pic.
        void drawCard(const View &v, float screenW, float screenH, Rectangle pic, Place place)
        {
            if (!v.code || pic.height <= 0 || screenH <= 0)
                return;
            const float k = std::clamp(pic.height / 1344.0f, 0.4f, 3.0f);
            const float leftBar = pic.x, rightBar = screenW - (pic.x + pic.width);
            const float need = 384.0f * k - 0.5f;
            bool side = false, right = false;
            if (place == Place::Left || place == Place::Right)
            {
                side = true;
                right = place == Place::Right;
            }
            else if (place == Place::Auto && (leftBar >= need || rightBar >= need))
            {
                side = true;
                right = leftBar < need;
            }
            if (side)
            {
                const float cw = 336.0f * k, margin = 24.0f * k;
                const float bar = right ? rightBar : leftBar;
                float cx = bar >= cw + 2.0f * margin ? (bar - cw) * 0.5f : margin;
                if (right)
                    cx = screenW - cx - cw;
                const float top = std::max(margin, pic.y + 48.0f * k);
                const float maxH = std::max(200.0f * k, std::min(pic.height - 96.0f * k, screenH - top - margin));
                drawSide(v, cx, top, maxH, k);
            }
            else
                drawCorner(v, screenW, std::clamp(screenH / 960.0f, 0.5f, 3.0f));
        }

        View viewNowLocked(const Settings &s, const rules::Switches &sw)
        {
            if (g_state.index < 0)
                return View{};
            View v = makeView(s, g_state.index, g_state.follower.progress(), sw, g_port1Fed.load(std::memory_order_relaxed));
            const auto now = Clock::now();
            v.done = g_state.doneAt != Clock::time_point{} && now - g_state.doneAt < std::chrono::seconds(2);
            v.flash = g_state.resetAt != Clock::time_point{} && now - g_state.resetAt < std::chrono::milliseconds(450);
            return v;
        }

        // ---------------------------------------------------------------- pre-warm (presenter thread)
        // The first F2 must not stall a frame loading fonts. While the card is hidden and the window and
        // GT4's picture have kept their size for 30 frames, every card of the list is laid out dry at that
        // size: the fonts it will need are loaded then (at the start, during GT4's boot). After a size change
        // only the sizes not loaded yet are added, so a picture that switches back and forth costs nothing
        // after the first time. Over 96 cached sizes (many window sizes) the cache starts again.
        struct LayoutKey
        {
            float w = -1, h = -1, px = -1, py = -1, pw = -1, ph = -1;
            int switches = -1;
            bool operator==(const LayoutKey &o) const
            {
                return w == o.w && h == o.h && px == o.px && py == o.py && pw == o.pw && ph == o.ph && switches == o.switches;
            }
        };
        LayoutKey g_seen, g_warmed;
        int g_settled = 0;

        int signature(const rules::Switches &sw) { return int(sw.noday) | int(sw.secret) << 4 | int(sw.missionfix) << 8; }

        void prewarm(const Settings &s, const Frame &f, const rules::Switches &sw)
        {
            const LayoutKey key{f.screenW, f.screenH, f.picX, f.picY, f.picW, f.picH, signature(sw)};
            if (!(key == g_seen))
            {
                g_seen = key;
                g_settled = 0;
                return;
            }
            if (key == g_warmed || ++g_settled < 30)
                return;
            g_warmed = key;
            const auto t0 = Clock::now();
            if (g_fonts.cache.size() > 96)
                g_fonts.unload(); // hidden: no draw call of this frame uses them
            const size_t before = g_fonts.loads;
            g_dry = true;
            for (size_t i = 0; i < s.list.size(); ++i)
                drawCard(makeView(s, int(i), 0, sw, g_port1Fed.load(std::memory_order_relaxed)), f.screenW, f.screenH,
                         Rectangle{f.picX, f.picY, f.picW, f.picH}, s.place);
            g_dry = false;
            if (g_fonts.loads == before)
                return; // every size already loaded: nothing to say
            const double ms = std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
            char line[200];
            std::snprintf(line, sizeof(line), "[cheats] card fonts ready for %.0fx%.0f: %zu size(s) in %.1f ms (at vsync %llu, the card hidden)",
                          double(f.screenW), double(f.screenH), g_fonts.loads - before, ms, static_cast<unsigned long long>(f.vsync));
            std::cerr << line << std::endl;
        }

#if !defined(GT4_STOCK_BUILD)
        // GT4_CHEAT_CARD_TEST shot: GT4's picture as the stock build shows it (scanline pass, lines at
        // GT3_CRT_SCANLINES, default 50; the Classic picture settings GT4_PICTURE_FILTER / GT4_DEINTERLACE,
        // default smooth + blend, the blend only for a field picture; lines off drawn by the pass at strength 0)
        // with the card, offscreen at each size, as PNGs.
        void testShots(const Settings &s, const Frame &f, const View &shown)
        {
            static ps2x::scanline::Pass pass;
            static const bool passReady = pass.init();
            ps2x::scanline::Settings sc = ps2x::scanline::parse(std::getenv("GT3_DISPLAY_FILTER"), std::getenv("GT3_CRT_SCANLINES"),
                                                                 std::getenv("GT4_SCANLINE_FIELD"), std::getenv("GT4_PICTURE_FIT"), true);
            ps2x::scanline::parsePicture(sc, std::getenv("GT4_PICTURE_FILTER"), std::getenv("GT4_DEINTERLACE"), true);
            const ps2x::scanline::Look look{sc.filter, ps2x::scanline::blendLines(sc.deinterlace, f.interlaced)};
            std::vector<std::pair<std::string, View>> views{{"", shown}};
            if (s.testAll)
            {
                views.clear();
                for (size_t i = 0; i < s.list.size(); ++i)
                {
                    View v = makeView(s, int(i), 0, shown.sw, g_port1Fed.load(std::memory_order_relaxed));
                    v.progress = std::min<int>(s.testProgress, v.code->count - 1);
                    v.done = shown.done;
                    v.flash = shown.flash;
                    views.emplace_back("-" + std::string(v.code->id), v);
                }
            }
            for (const auto &[tag, v] : views)
                for (const auto &[w, h] : s.shotSizes)
                {
                    RenderTexture2D target = LoadRenderTexture(w, h);
                    if (target.id == 0)
                        continue;
                    const int rows = ps2x::scanline::rowsPerDisplayLine(std::max(1, f.srcH));
                    const ps2x::scanline::Placement place =
                        ps2x::scanline::fitPicture(float(w), float(h), std::max(1, f.srcH) / rows, f.aspect, sc.fit);
                    BeginTextureMode(target);
                    ClearBackground(BLACK);
                    if (passReady)
                        pass.draw(*f.frame, f.srcW, f.srcH, place, sc.on ? sc.strength : 0.0f, 0.0f, -1, 0, look);
                    else
                        DrawTexturePro(*f.frame, Rectangle{0, 0, float(f.srcW), float(f.srcH)}, Rectangle{place.x, place.y, place.w, place.h},
                                       Vector2{0, 0}, 0.0f, WHITE);
                    drawCard(v, float(w), float(h), Rectangle{place.x, place.y, place.w, place.h}, s.place);
                    EndTextureMode();
                    Image image = LoadImageFromTexture(target.texture);
                    ImageFlipVertical(&image);
                    const std::string path = s.shotPrefix + tag + "-" + std::to_string(w) + "x" + std::to_string(h) + ".png";
                    const bool ok = ExportImage(image, path.c_str());
                    UnloadImage(image);
                    UnloadRenderTexture(target);
                    std::cerr << "[cheats] card test shot " << (ok ? "" : "FAILED ") << path << " (picture " << place.w << "x" << place.h
                              << " at " << place.x << "," << place.y << ", " << ps2x::scanline::filterName(look.filter)
                              << (look.blend ? " + blend" : "") << ", lines " << (sc.on ? int(std::lround(sc.strength * 100.0f)) : 0) << "; card "
                              << v.n << " / " << v.m << " " << v.code->id << ", "
                              << v.progress << " done" << (v.notFollowable ? ", not followed" : "") << ")" << std::endl;
                }
        }
#endif
    }

    void onPad(int port, const uint8_t *packet, size_t bytes)
    {
        if (bytes < 4 || port < 0 || port > 1 || !settings().on)
            return;
        const uint16_t held = static_cast<uint16_t>(~(packet[2] | (packet[3] << 8)));
        g_held[size_t(port)].store(held, std::memory_order_relaxed);
        if (port == 1)
            g_port1Fed.store(true, std::memory_order_relaxed);
        if (!g_following.load(std::memory_order_acquire))
            return;
        const rules::Switches sw = switchesNow();
        std::lock_guard lock(g_mutex);
        if (g_state.index < 0)
            return;
        const Event e = g_state.follower.sample(port, held, sw);
        if (e == Event::Completed)
        {
            g_state.doneAt = Clock::now();
            ++g_state.completed;
            std::cerr << "[cheats] card: " << g_state.follower.code()->id << " typed to the end (controller " << port + 1 << ")" << std::endl;
        }
        else if (e == Event::Reset)
        {
            g_state.resetAt = Clock::now();
            ++g_state.resets;
        }
    }

    void frame(const Frame &f)
    {
        const Settings &s = settings();
        if (!s.on)
            return;
        const rules::Switches sw = switchesNow();
        View v;
        bool shownNow = false;
        {
            std::lock_guard lock(g_mutex);
            if (!g_state.started && (!s.test || f.vsync >= s.testShowAt))
            {
                g_state.started = true;
                if (s.test)
                {
                    showLocked(std::min(int(s.list.size()), s.testShow) - 1);
                    shownNow = true;
                    const gt4cheats::Code *code = s.list[size_t(g_state.index)];
                    for (int i = 0; i < s.testProgress && i < code->count; ++i)
                        g_state.follower.key(code->presses[size_t(i)]);
                    if (s.testReset)
                        g_state.resetAt = Clock::now() + std::chrono::hours(1);
                    if (s.testDone)
                        g_state.doneAt = Clock::now() + std::chrono::hours(1);
                }
            }
            if (f.readKeys && IsKeyPressed(s.key))
            {
                int next = g_state.index + 1;
                if (next >= int(s.list.size()))
                    next = -1;
                showLocked(next);
                shownNow = next >= 0;
            }
            v = viewNowLocked(s, sw);
            if (s.test && !s.testLive)
            {
                // Test pictures hold their state (a timed flash or "Done" would depend on the clock).
                v.flash = s.testReset;
                v.done = s.testDone;
            }
        }
        if (!v.code)
        {
            prewarm(s, f, sw);
            return;
        }
        const size_t loadsBefore = g_fonts.loads;
        const auto t0 = Clock::now();
        ps2x::overlay::mark(0.0f, 0.0f, ps2x::overlay::kAll, ps2x::overlay::kAll); // (shown rarely: F2)
        drawCard(v, f.screenW, f.screenH, Rectangle{f.picX, f.picY, f.picW, f.picH}, s.place);
        if (shownNow)
        {
            // What a press of F2 costs this frame (the pre-warm should leave no font to load).
            char line[240];
            std::snprintf(line, sizeof(line), "[cheats] card %d / %d (%s) shown at vsync %llu: drawn in %.2f ms, %zu font size(s) loaded for it%s", v.n, v.m,
                          std::string(v.code->id).c_str(), static_cast<unsigned long long>(f.vsync),
                          std::chrono::duration<double, std::milli>(Clock::now() - t0).count(), g_fonts.loads - loadsBefore,
                          v.notFollowable ? "; not followed (controller 2 only)" : "");
            std::cerr << line << std::endl;
        }
#if !defined(GT4_STOCK_BUILD)
        static bool shotsTaken = false;
        if (s.test && !s.shotPrefix.empty() && !shotsTaken && f.vsync >= s.shotAt && f.frame && f.srcW > 0 && f.srcH > 0)
        {
            shotsTaken = true;
            testShots(s, f, v);
        }
#endif
    }

    void shutdown()
    {
        g_fonts.unload();
    }
}
