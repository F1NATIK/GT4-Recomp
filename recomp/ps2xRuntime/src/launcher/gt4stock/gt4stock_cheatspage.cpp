// GT4 Recompilation launcher (Classic edition; internal names keep "stock"): the Cheats page (new in 1.4;
// analysis/cheats/CHEATS-TAB-DESIGN.md 3 and 7, the mock-ups in analysis/cheats/mockup). GT4's five button
// codes and its hidden Options pages: what each does, its history, what it needs, the code as big numbered
// PS2 buttons, Show big, Practise with my pad (the launcher reads player 1's pad; nothing is sent to GT4),
// Save card (a PNG in the Photos folder), Save PDF and Print (Windows' print window, only on a click).
// The switches (GT4_CHEATS noday / secret / missionfix, GT4_CHEAT_CARD) are off by default.

#include "gt4stock_app.h"

#include "gt4save.h"
#include "gt4stock_cheats.h"
#include "gt4stock_util.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>

namespace gt4stock
{
    using ui::Align;
    using ui::Icon;
    using ui::Style;
    using ui::Weight;

    namespace
    {
        constexpr float kSidebarW = 232.0f;
        constexpr float kTop = 104.0f;
        constexpr float kGap = 16.0f;

        Rectangle R(float x, float y, float w, float h) { return Rectangle{x, y, w, h}; }

        const char *const kDot = "  \xc2\xb7  ";

        // ---------------------------------------------------------------- PS2 buttons, drawn (no Sony artwork)
        struct PadPalette
        {
            Color tile, edge, cross, arrow, text, num, L, R, doneTile, doneEdge, nextEdge, bg;
        };

        PadPalette darkPad()
        {
            const ui::Theme &t = ui::theme();
            return PadPalette{t.panel3,
                              Color{60, 70, 88, 255},
                              Color{54, 63, 80, 255},
                              WHITE,
                              WHITE,
                              t.muted, // the press numbers (were t.faint: too faint on the dark card)
                              Color{128, 176, 255, 255},
                              Color{240, 192, 96, 255},
                              ui::mixColor(t.panel, t.ok, 0.30f),
                              t.ok,
                              t.accent,
                              t.panel};
        }

        PadPalette paperPad()
        {
            return PadPalette{WHITE,
                              Color{70, 74, 82, 255},
                              Color{226, 229, 234, 255},
                              Color{20, 22, 26, 255},
                              Color{20, 22, 26, 255},
                              Color{110, 116, 126, 255},
                              Color{30, 90, 200, 255},
                              Color{176, 104, 0, 255},
                              Color{220, 245, 226, 255},
                              Color{40, 150, 70, 255},
                              Color{30, 90, 200, 255},
                              WHITE};
        }

        // The smallest the tiles' words and numbers get (dp; 1 dp = 1 px in a 1280 x 800 window): the 36 dp tiles of the
        // smaller windows would give 7 px "SELECT" and 9 px numbers (1.4 review).
        constexpr float kTileLabelMin = 8.5f, kTileNumberMin = 11.0f;

        // state: 0 normal, 1 done (pressed in Practise), 2 next
        void drawPadTile(ui::Ui &u, uint16_t b, float x, float y, float s, int state, const PadPalette &p, int number, Color bg)
        {
            const float r = s * 0.2f;
            Color tile = p.tile, edge = p.edge;
            if (state == 1)
            {
                tile = p.doneTile;
                edge = p.doneEdge;
            }
            if (state == 2)
                u.round(R(x - s * 0.09f, y - s * 0.09f, s * 1.18f, s * 1.18f), r * 1.4f, ui::mixColor(bg, p.nextEdge, 0.25f));
            u.round(R(x, y, s, s), r, tile);
            u.roundLine(R(x, y, s, s), r, std::max(1.0f, s * (state == 2 ? 0.05f : 0.025f)), state == 2 ? p.nextEdge : edge);
            const float cx = x + s * 0.5f, cy = y + s * 0.5f;
            const Color glyph = state == 1 ? ui::mixColor(p.doneEdge, WHITE, 0.55f) : p.arrow;
            if (b == gt4cheats::Up || b == gt4cheats::Down || b == gt4cheats::Left || b == gt4cheats::Right)
            {
                const float arm = s * 0.36f, thick = s * 0.24f;
                const Color cross = state == 1 ? ui::mixColor(tile, WHITE, 0.12f) : p.cross;
                u.round(R(cx - thick * 0.5f, cy - arm, thick, arm * 2.0f), s * 0.05f, cross);
                u.round(R(cx - arm, cy - thick * 0.5f, arm * 2.0f, thick), s * 0.05f, cross);
                const float dx = b == gt4cheats::Left ? -1.0f : b == gt4cheats::Right ? 1.0f : 0.0f;
                const float dy = b == gt4cheats::Up ? -1.0f : b == gt4cheats::Down ? 1.0f : 0.0f;
                const float a = s * 0.16f;
                const float tipx = cx + dx * s * 0.34f, tipy = cy + dy * s * 0.34f, bx = cx + dx * s * 0.12f, by = cy + dy * s * 0.12f;
                const float px = -dy, py = dx;
                u.triangle(Vector2{tipx, tipy}, Vector2{bx + px * a, by + py * a}, Vector2{bx - px * a, by - py * a}, glyph);
            }
            else if (b == gt4cheats::L1 || b == gt4cheats::L2 || b == gt4cheats::R1 || b == gt4cheats::R2)
            {
                const bool left = b == gt4cheats::L1 || b == gt4cheats::L2, one = b == gt4cheats::L1 || b == gt4cheats::R1;
                Color col = left ? p.L : p.R;
                if (state == 1)
                    col = ui::mixColor(col, WHITE, 0.25f);
                const float bw = s * 0.5f;
                if (one)
                    u.round(R(cx - bw * 0.5f, y + s * 0.16f, bw, s * 0.09f), s * 0.04f, col);
                else
                {
                    u.round(R(cx - bw * 0.5f, y + s * 0.12f, bw, s * 0.16f), s * 0.08f, col);
                    u.fill(R(cx - bw * 0.5f, y + s * 0.21f, bw, s * 0.07f), col);
                }
                u.text(left ? (one ? "L1" : "L2") : (one ? "R1" : "R2"), cx, y + s * 0.30f, s * 0.46f, col, Weight::Bold, Align::Center);
            }
            else if (b == gt4cheats::Select)
            {
                const Color col = state == 1 ? glyph : p.text;
                u.round(R(cx - s * 0.2f, y + s * 0.28f, s * 0.4f, s * 0.13f), s * 0.065f, col);
                u.text("SELECT", cx, y + s * 0.50f, std::max(s * 0.19f, kTileLabelMin), col, Weight::Semibold, Align::Center);
            }
            else if (b == gt4cheats::Start)
            {
                const Color col = state == 1 ? glyph : p.text;
                u.triangle(Vector2{cx - s * 0.1f, y + s * 0.22f}, Vector2{cx - s * 0.1f, y + s * 0.46f}, Vector2{cx + s * 0.14f, y + s * 0.34f}, col);
                u.text("START", cx, y + s * 0.50f, std::max(s * 0.19f, kTileLabelMin), col, Weight::Semibold, Align::Center);
            }
            else
            {
                const Color col = state == 1 ? glyph : p.text;
                u.text(std::string(gt4cheats::buttonName(b)), cx, cy - s * 0.17f, s * 0.24f, col, Weight::Semibold, Align::Center);
            }
            if (number > 0)
                u.text(std::to_string(number), cx, y + s + s * 0.06f, std::max(s * 0.26f, kTileNumberMin), p.num, Weight::Regular, Align::Center);
        }

        // The code's groups (the memory aid's), as lists of press indices.
        std::vector<std::vector<int>> groupsOf(const gt4cheats::Code &c)
        {
            std::vector<std::vector<int>> g;
            int at = 0;
            for (int i = 0; i < c.groupCount && at < c.count; ++i)
            {
                std::vector<int> one;
                for (int k = 0; k < c.groups[static_cast<size_t>(i)] && at < c.count; ++k)
                    one.push_back(at++);
                g.push_back(one);
            }
            if (at < c.count)
            {
                std::vector<int> rest;
                while (at < c.count)
                    rest.push_back(at++);
                g.push_back(rest);
            }
            return g;
        }

        struct SeqGeometry
        {
            float gap, ggap, rowGap;
            std::vector<std::vector<std::vector<int>>> rows; // rows -> groups -> presses
        };

        SeqGeometry seqGeometry(const gt4cheats::Code &c, float maxW, float s, bool numbers)
        {
            SeqGeometry g{s * 0.14f, s * 0.5f, s * (numbers ? 0.62f : 0.3f), {}};
            const std::vector<std::vector<int>> groups = groupsOf(c);
            auto groupW = [&](const std::vector<int> &grp) { return static_cast<float>(grp.size()) * s + static_cast<float>(grp.size() - 1) * g.gap; };
            // Groups are never split; a row takes whole groups while they fit in `limit`.
            auto fill = [&](float limit, std::vector<float> *widths) {
                std::vector<std::vector<std::vector<int>>> rows;
                std::vector<std::vector<int>> cur;
                float w = 0.0f;
                for (const std::vector<int> &grp : groups)
                {
                    const float gw = groupW(grp);
                    const float add = cur.empty() ? gw : w + g.ggap + gw;
                    if (!cur.empty() && add > limit + 0.01f)
                    {
                        rows.push_back(cur);
                        if (widths)
                            widths->push_back(w);
                        cur = {grp};
                        w = gw;
                    }
                    else
                    {
                        cur.push_back(grp);
                        w = add;
                    }
                }
                if (!cur.empty())
                {
                    rows.push_back(cur);
                    if (widths)
                        widths->push_back(w);
                }
                return rows;
            };
            g.rows = fill(maxW, nullptr);
            if (g.rows.size() > 1)
            {
                // More than one row: balance them (no row of one button under a full row). The candidate limits are
                // the widths of every run of whole groups; the narrowest that keeps the same number of rows with no
                // row wider than the one above it is taken (the mock-ups' 10 | 6), else the narrowest.
                std::vector<float> limits;
                for (size_t a = 0; a < groups.size(); ++a)
                {
                    float w = 0.0f;
                    for (size_t b = a; b < groups.size(); ++b)
                    {
                        w += (b > a ? g.ggap : 0.0f) + groupW(groups[b]);
                        if (w <= maxW + 0.01f)
                            limits.push_back(w);
                    }
                }
                std::sort(limits.begin(), limits.end());
                std::vector<std::vector<std::vector<int>>> narrowest;
                for (float limit : limits)
                {
                    std::vector<float> widths;
                    auto rows = fill(limit, &widths);
                    if (rows.size() != g.rows.size())
                        continue;
                    if (narrowest.empty())
                        narrowest = rows;
                    bool falling = true;
                    for (size_t i = 1; i < widths.size(); ++i)
                        falling = falling && widths[i] <= widths[i - 1] + 0.01f;
                    if (falling)
                    {
                        narrowest = rows;
                        break;
                    }
                }
                if (!narrowest.empty())
                    g.rows = narrowest;
            }
            return g;
        }

        float sequenceHeight(const gt4cheats::Code &c, float maxW, float s, bool numbers)
        {
            const SeqGeometry g = seqGeometry(c, maxW, s, numbers);
            const float n = static_cast<float>(g.rows.size());
            return n * (s + g.rowGap) - g.rowGap + (numbers ? s * 0.4f : 0.0f);
        }

        // progress < 0: no follow-along. Returns the height used.
        float drawSequence(ui::Ui &u, const gt4cheats::Code &c, float x, float y, float maxW, float s, int progress, const PadPalette &p, bool numbers,
                           bool center, Color bg)
        {
            const SeqGeometry g = seqGeometry(c, maxW, s, numbers);
            float yy = y;
            for (const auto &row : g.rows)
            {
                float rw = 0.0f;
                for (size_t gi = 0; gi < row.size(); ++gi)
                    rw += static_cast<float>(row[gi].size()) * s + static_cast<float>(row[gi].size() - 1) * g.gap + (gi ? g.ggap : 0.0f);
                float xx = x + (center ? std::floor((maxW - rw) * 0.5f) : 0.0f);
                for (size_t gi = 0; gi < row.size(); ++gi)
                {
                    const auto &grp = row[gi];
                    for (size_t k = 0; k < grp.size(); ++k)
                    {
                        const int idx = grp[k];
                        int state = 0;
                        if (progress >= 0)
                            state = idx < progress ? 1 : idx == progress ? 2 : 0;
                        drawPadTile(u, c.presses[static_cast<size_t>(idx)], xx, yy, s, state, p, numbers ? idx + 1 : 0, bg);
                        xx += s + (k + 1 < grp.size() ? g.gap : 0.0f);
                    }
                    if (gi + 1 < row.size())
                    {
                        u.circle(xx + g.ggap * 0.5f, yy + s * 0.5f, std::max(1.2f, s * 0.045f), p.num);
                        xx += g.ggap;
                    }
                }
                yy += s + g.rowGap;
            }
            return yy - y - g.rowGap + (numbers ? s * 0.4f : 0.0f);
        }

        // ---------------------------------------------------------------- the cards' coloured tiles
        Color cardBase(std::string_view id)
        {
            if (id == "credits")
                return Color{196, 150, 40, 255};
            if (id == "licence")
                return Color{60, 120, 220, 255};
            if (id == "test")
                return Color{200, 160, 50, 255};
            if (id == "event")
                return Color{60, 160, 90, 255};
            if (id == "mission")
                return Color{190, 80, 70, 255};
            return Color{120, 110, 200, 255};
        }

        void drawCardTile(ui::Ui &u, std::string_view id, float x, float y, float s, Color panel)
        {
            const Color base = cardBase(id);
            const Color bg = ui::mixColor(panel, base, 0.28f);
            u.round(R(x, y, s, s), s * 0.24f, bg);
            u.roundLine(R(x, y, s, s), s * 0.24f, 1.0f, ui::mixColor(panel, base, 0.6f));
            const Color g = ui::mixColor(base, WHITE, 0.45f);
            const float cx = x + s * 0.5f, cy = y + s * 0.5f;
            if (id == "credits")
            {
                u.ring(cx, cy, s * 0.27f, s * 0.33f, g);
                u.text("Cr", cx, cy - s * 0.215f, s * 0.32f, g, Weight::Bold, Align::Center);
            }
            else if (id == "licence")
            {
                u.roundLine(R(x + s * 0.18f, y + s * 0.26f, s * 0.64f, s * 0.48f), s * 0.06f, s * 0.05f, g);
                u.text("B", x + s * 0.36f, cy - s * 0.23f, s * 0.34f, g, Weight::Bold, Align::Center);
                u.line(x + s * 0.52f, y + s * 0.42f, x + s * 0.72f, y + s * 0.42f, s * 0.04f, g);
                u.line(x + s * 0.52f, y + s * 0.56f, x + s * 0.68f, y + s * 0.56f, s * 0.04f, g);
            }
            else if (id == "test")
            {
                u.triangle(Vector2{cx - s * 0.2f, y + s * 0.14f}, Vector2{cx - s * 0.06f, y + s * 0.14f}, Vector2{cx + s * 0.02f, y + s * 0.40f}, g);
                u.triangle(Vector2{cx - s * 0.2f, y + s * 0.14f}, Vector2{cx + s * 0.02f, y + s * 0.40f}, Vector2{cx - s * 0.12f, y + s * 0.40f}, g);
                u.triangle(Vector2{cx + s * 0.2f, y + s * 0.14f}, Vector2{cx + s * 0.06f, y + s * 0.14f}, Vector2{cx - s * 0.02f, y + s * 0.40f}, g);
                u.triangle(Vector2{cx + s * 0.2f, y + s * 0.14f}, Vector2{cx - s * 0.02f, y + s * 0.40f}, Vector2{cx + s * 0.12f, y + s * 0.40f}, g);
                u.circle(cx, cy + s * 0.08f, s * 0.22f, g);
                u.text("1", cx, cy + s * 0.08f - s * 0.17f, s * 0.26f, bg, Weight::Bold, Align::Center);
            }
            else if (id == "event")
            {
                const float fx = x + s * 0.28f, fy = y + s * 0.22f, fw = s * 0.48f, fh = s * 0.34f;
                u.line(fx, fy - s * 0.02f, fx, y + s * 0.82f, s * 0.05f, g);
                for (int i = 0; i < 4; ++i)
                    for (int j = 0; j < 3; ++j)
                        if ((i + j) % 2 == 0)
                            u.fill(R(fx + static_cast<float>(i) * fw / 4.0f, fy + static_cast<float>(j) * fh / 3.0f, fw / 4.0f, fh / 3.0f), g);
                u.roundLine(R(fx, fy, fw, fh), 0.0f, s * 0.03f, g);
            }
            else if (id == "mission")
            {
                u.ring(cx, cy, s * 0.275f, s * 0.325f, g);
                u.ring(cx, cy, s * 0.135f, s * 0.185f, g);
                u.circle(cx, cy, s * 0.05f, g);
            }
            else
            {
                // an open padlock: the body, the shackle raised on the left
                u.round(R(x + s * 0.24f, y + s * 0.44f, s * 0.52f, s * 0.36f), s * 0.06f, g);
                DrawRing(Vector2{(x + s * 0.56f) * u.s, (y + s * 0.30f) * u.s}, s * 0.11f * u.s, s * 0.17f * u.s, 180.0f, 360.0f, 24, g);
                u.fill(R(x + s * 0.67f, y + s * 0.29f, s * 0.06f, s * 0.16f), g);
                u.fill(R(x + s * 0.39f, y + s * 0.29f, s * 0.06f, s * 0.07f), g);
                u.circle(cx, y + s * 0.60f, s * 0.05f, bg);
                u.fill(R(cx - s * 0.02f, y + s * 0.60f, s * 0.04f, s * 0.1f), bg);
            }
        }

        Color statusColour(CheatStatus st) { return cheatStatusReady(st) ? ui::theme().ok : ui::theme().warn; }
    }

    // ------------------------------------------------------------------ the drawing, for the card and the sheet
    void App::cheatTile(const gt4cheats::Code &code, float x, float y, float s, Color panel) { drawCardTile(ui_, code.id, x, y, s, panel); }

    float App::cheatSequence(const gt4cheats::Code &code, float x, float y, float maxW, float s, int progress, bool paper, bool center, Color bg)
    {
        return drawSequence(ui_, code, x, y, maxW, s, progress, paper ? paperPad() : darkPad(), true, center, bg);
    }

    float App::cheatSequenceHeight(const gt4cheats::Code &code, float maxW, float s, bool numbers) { return sequenceHeight(code, maxW, s, numbers); }

    // ------------------------------------------------------------------ the slot-1 save's day
    void App::readCheatSave(bool force)
    {
        // Read again when the slot's card changes (or on force: GT4 closed, the page opened).
        const bool file = cardIsFile(1);
        const fs::path source = file ? cardFile(1) : cardDir(1);
        std::error_code ec;
        const auto stamp = fs::exists(source, ec) ? (fs::is_directory(source, ec) ? fs::file_time_type{} : fs::last_write_time(source, ec)) : fs::file_time_type{};
        const std::string key = pathText(source) + "|" + std::to_string(stamp.time_since_epoch().count()) + "|" +
                                (cards_[0].gt4Save ? cards_[0].gt4.when : std::string()) + "|" + (cardFiles_[0].gt4Save ? cardFiles_[0].gt4.when : std::string());
        if (!force && key == cheatSaveKey_)
            return;
        cheatSaveKey_ = key;
        cheatElapsed_ = -1;
        cheatSaveState_ = 0;
        const bool present = file ? cardFiles_[0].gt4Save : cards_[0].gt4Save;
        if (!present)
            return; // no GT4 save in slot 1: a new game starts at Day 1
        // Read only (std::ifstream in the save library), as Save details reads it.
        const gt4save::Save s = file ? gt4save::loadCardFile(source, nullptr) : gt4save::loadCardFolder(source, nullptr);
        if (s.loaded)
        {
            cheatElapsed_ = std::max<long long>(0, s.days());
            cheatSaveState_ = 1;
        }
        else
            cheatSaveState_ = 2; // there, but not readable
    }

    // ------------------------------------------------------------------ player 2 (the shared rule)
    gt4pad2rules::Inventory App::pad2Inventory() const
    {
        // The demo set, or in the hidden test mode Windows' own list (enumeration only: the test mode never calls
        // GLFW's joystick functions), both in GLFW's order.
        if (devices_.demo() || hidden_)
            return pad2InventoryFromList(devices_.list());
        // A normal session: GLFW's own list, the joysticks GT4.exe sees.
        gt4pad2rules::Inventory inv{};
        for (const GamepadInfo &g : devices_.glfwGamepads())
            if (g.joystick >= 0 && g.joystick < gt4pad2rules::Joysticks)
                inv[static_cast<size_t>(g.joystick)] = {true, g.mapped, lower(g.guid), g.name};
        return inv;
    }

    int App::pad2Pick(std::string *why) const
    {
        // As the player-2 runtime decides it (Gt4Pad2.cpp): player 1's whole mapping (PS2X_INPUT, kept rows included) and
        // the wheel adapter's device from PS2X_WHEEL_DEVICE's text as written (a literal vid:pid only, whether or not
        // PS2X_WHEEL is on).
        return pickPlayer2(st_.pad2, st_.pad, pad2Inventory(), st_.wheel.device.empty() ? std::string("auto") : st_.wheel.device, why);
    }

    bool App::player2Ready(std::string *name) const
    {
        if (st_.pad2.mode == Pad2Settings::Mode::Off || !gt4_.pad2)
            return false; // port 2 empty: Off, or a GT4 program without player 2
        const int j = pad2Pick(nullptr);
        if (j < 0)
            return false;
        if (name)
            *name = pad2Inventory()[static_cast<size_t>(j)].name;
        return true;
    }

    // ------------------------------------------------------------------ the page
    fs::path App::cheatOutDir() const
    {
        if (hidden_)
            return opt_.cheatFiles; // the test mode writes nothing to the GT4 folder
        return usbRoot() / "GT4";
    }

    void App::pageCheats()
    {
        const ui::Theme &t = ui::theme();
        CheatSettings &cs = st_.cheats;
        drawHeader("Cheats", "GT4's own button codes, found in its menu scripts in 2023, and its hidden Options pages. Pick one to see it big.");
        readCheatSave(false);
        const float x0 = kSidebarW + 32, x1 = ui_.width - 32, cw = x1 - x0, bottom = ui_.height - 48;
        cheatSel_ = std::clamp(cheatSel_, 0, static_cast<int>(gt4cheats::kCodes.size()) - 1);
        std::string p2name;
        const bool p2 = player2Ready(&p2name);

        // ---------------- the band: the save's day, Skip the 365-day wait, the in-game card
        // A switch the GT4 program here does not read is drawn off and greyed, with one plain line (1.4 review).
        const CheatSettings ce = cheatsInEffect(cs, gt4_);
        const float c1 = std::floor(cw * 0.29f), c2 = std::floor(cw * 0.645f);
        const float w2 = c2 - c1 - 44, w3 = cw - c2 - 42;
        const std::string skipText = gt4_.cheats ? "Works from Day 1. Only GT4's menu script changes, in memory. Off = GT4 as shipped."
                                                 : "The GT4 program here does not read this switch: the codes keep GT4's 365-day wait.";
        const std::string backText = "Back up your save first: a code's result is saved.";
        // The card sits beside the picture when the bars are wide enough (the monitor, or the laptop with Whole rows), else
        // in a corner over it (CHEATS-TAB-DESIGN.md 6.4). Nothing ticked: the runtime shows all six.
        const std::string cardText = !gt4_.card ? std::string("The GT4 program here has no in-game cheat card.")
                                     : ce.card && cs.ticked.empty()
                                         ? cs.key + " in GT4 shows all six cards, as none is ticked below; beside the picture, or in a corner if there "
                                                    "is no room. " + cs.key + " again: the next card, then hidden."
                                         : cs.key + " in GT4 shows the ticked cards (all six if none is) beside the picture, or in a corner if there is "
                                                    "no room. " + cs.key + " again: the next card, then hidden.";
        const float h2 = 44 + ui_.wrapHeight(skipText, w2, 12.5f, Weight::Regular, 1.3f) + 2 + ui_.wrapHeight(backText, w2 - 18, 12.5f, Weight::Regular, 1.3f);
        const float h3 = 44 + ui_.wrapHeight(cardText, w3, 12.5f, Weight::Regular, 1.3f);
        const float bandH = std::floor(std::max(92.0f, std::max(h2, h3) + 12));
        {
            const Rectangle band = R(x0, kTop, cw, bandH);
            ui_.card(band, "");
            // 1. the save's day, on GT4's own count
            float cx = x0 + 20;
            ui_.text(cardIsFile(1) ? "Your GT4 save (memory card slot 1, card file)" : "Your GT4 save (memory card slot 1)", cx, kTop + 14, 12.5f, t.muted);
            if (cheatSaveState_ == 1)
            {
                const long long day = gameDay(cheatElapsed_);
                const bool met = dayRuleMet(cheatElapsed_);
                const std::string d = strf("Day %lld", day);
                ui_.text(d, cx, kTop + 32, 24, t.text, Weight::Bold);
                const float dw = ui_.textWidth(d, 24, Weight::Bold);
                ui_.text(ui_.fit(met ? "the codes work (Day 367 or later)" : "of the 367 the codes need", c1 - 44 - dw - 10, 13), cx + dw + 10, kTop + 42, 13,
                         met ? t.ok : t.muted);
                ui_.meter(R(cx, kTop + 72, c1 - 44, 6), std::min(1.0f, static_cast<float>(day) / 367.0f), met ? t.ok : t.warn);
            }
            else
            {
                ui_.text(cheatSaveState_ == 2 ? "Day ?" : "No GT4 save yet", cx, kTop + 32, cheatSaveState_ == 2 ? 24.0f : 19.0f, t.text, Weight::Bold);
                ui_.text(cheatSaveState_ == 2 ? "The save could not be read." : "A new game starts at Day 1.", cx, kTop + 64, 12.5f,
                         cheatSaveState_ == 2 ? t.warn : t.muted);
            }
            ui_.fill(R(x0 + c1, kTop + 16, 1, bandH - 32), t.border);
            // 2. Skip the 365-day wait
            cx = x0 + c1 + 22;
            {
                bool shown = false;
                if (ui_.toggle("cheat-noday", cx, kTop + 16, gt4_.cheats ? cs.noday : shown, gt4_.cheats))
                    markDirty();
            }
            ui_.text("Skip the 365-day wait", cx + 52, kTop + 16, 15, gt4_.cheats ? t.text : t.muted, Weight::Semibold);
            ui_.chip(cx + 52 + ui_.textWidth("Skip the 365-day wait", 15, Weight::Semibold) + 10, kTop + 16, "all 5 codes", t.panel2, t.muted, 11);
            float ty = kTop + 44;
            ty += ui_.wrap(skipText, cx, ty, w2, 12.5f, t.muted, Weight::Regular, 1.3f) + 2;
            {
                // A link to the Saves page's "Back up now".
                const float bh = ui_.wrapHeight(backText, w2 - 18, 12.5f, Weight::Regular, 1.3f);
                const Rectangle link = R(cx - 4, ty - 2, w2 + 8, bh + 4);
                const bool hot = ui_.hovered(link);
                ui_.circle(cx + 6, ty + 8.5f, 6, ui::alpha(t.warn, 0.25f));
                ui_.text("!", cx + 6, ty, 12, t.warn, Weight::Bold, Align::Center);
                ui_.wrap(backText, cx + 18, ty, w2 - 18, 12.5f, hot ? ui::mixColor(t.warn, WHITE, 0.3f) : t.warn, Weight::Regular, 1.3f);
                if (ui_.clicked(link))
                {
                    page_ = Page::Saves;
                    savesView_ = 0;
                    detailsOpen_ = false;
                    refreshCards();
                }
            }
            ui_.fill(R(x0 + c2, kTop + 16, 1, bandH - 32), t.border);
            // 3. the in-game cheat card
            cx = x0 + c2 + 22;
            {
                bool shown = false;
                if (ui_.toggle("cheat-card", cx, kTop + 16, gt4_.card ? cs.card : shown, gt4_.card))
                {
                    cs.cardAuto = false;
                    markDirty();
                }
            }
            ui_.text("In-game cheat card", cx + 52, kTop + 16, 15, gt4_.card ? t.text : t.muted, Weight::Semibold);
            ui_.keycap(cx + 52 + ui_.textWidth("In-game cheat card", 15, Weight::Semibold) + 10, kTop + 14, cs.key);
            ui_.wrap(cardText, cx, kTop + 44, w3, 12.5f, ce.card && cs.ticked.empty() ? t.warn : t.muted, Weight::Regular, 1.3f);
        }

        // ---------------- left: the six cards
        const float listW = std::floor(std::clamp(cw * 0.36f, 320.0f, 372.0f));
        const float ly = kTop + bandH + kGap;
        ui_.text("GT4's button codes", x0 + 2, ly, 13, t.faint, Weight::Semibold);
        ui_.text("on card", x0 + listW - 6, ly, 12, t.faint, Weight::Regular, Align::Right);
        {
            const float itemH = std::floor(std::clamp((bottom - (ly + 24)) / 6.0f - 8.0f, 58.0f, 76.0f));
            const float pad = std::floor((itemH - 59.0f) * 0.5f + 4.5f);
            float y = ly + 24;
            for (size_t i = 0; i < gt4cheats::kCodes.size(); ++i)
            {
                const gt4cheats::Code &c = gt4cheats::kCodes[i];
                const Rectangle r = R(x0, y, listW, itemH);
                const bool sel = static_cast<int>(i) == cheatSel_;
                const Color bgc = sel ? ui::mixColor(t.bg, t.accent, 0.14f) : t.panel;
                ui_.round(r, 10, bgc);
                ui_.roundLine(r, 10, 1, sel ? ui::mixColor(t.bg, t.accent, 0.7f) : ui::alpha(t.border, 0.8f));
                const float ts = std::min(44.0f, itemH - 20);
                drawCardTile(ui_, c.id, x0 + 14, y + (itemH - ts) * 0.5f, ts, bgc);
                const float tx = x0 + 28 + ts;
                const float textW = listW - (tx - x0) - 46;
                ui_.text(ui_.fit(std::string(c.name), textW, 15, Weight::Semibold), tx, y + pad, 15, t.text, Weight::Semibold);
                ui_.text(ui_.fit(cheatScreen(c), textW, 12), tx, y + pad + 21, 12, t.muted);
                const CheatStatus st = cheatStatus(c, ce, cheatSaveState_ == 1 ? cheatElapsed_ : -1, p2);
                const Color sc = statusColour(st);
                ui_.chip(tx, y + itemH - 26, cheatStatusText(st), ui::mixColor(bgc, sc, 0.16f), sc, 10.5f);
                // "on card": the in-game card and the printed sheet
                bool on = isTicked(cs, c.id);
                const Rectangle box = R(x0 + listW - 38, y + itemH * 0.5f - 14, 30, 28);
                const bool onBox = ui_.hovered(box);
                if (ui_.checkbox("cheat-tick-" + std::string(c.id), x0 + listW - 34, y + itemH * 0.5f - 10, on, ""))
                {
                    setTicked(cs, c.id, on);
                    markDirty();
                }
                else if (!onBox && ui_.clicked(r))
                {
                    cheatSel_ = static_cast<int>(i);
                    ui_.resetScroll("cheat-body");
                }
                y += itemH + 8;
            }
        }

        // ---------------- right: the card
        const Rectangle d = R(x0 + listW + kGap, ly, x1 - (x0 + listW + kGap), bottom - ly);
        drawCheatDetail(d, p2, p2name);
    }

    void App::drawCheatDetail(Rectangle d, bool p2, const std::string &p2name)
    {
        const ui::Theme &t = ui::theme();
        CheatSettings &cs = st_.cheats;
        // What GT4.exe acts on (a switch the GT4 program here does not read counts as off): the chips and the words.
        const CheatSettings ce = cheatsInEffect(cs, gt4_);
        const gt4cheats::Code &code = gt4cheats::kCodes[static_cast<size_t>(cheatSel_)];
        const bool options = code.controller2, mission = code.id == "mission";
        ui_.card(d, "");
        const float px = d.x + 24, iw = d.width - 48;
        float y = d.y + 18;
        const CheatStatus st = cheatStatus(code, ce, cheatSaveState_ == 1 ? cheatElapsed_ : -1, p2);
        const Color sc = statusColour(st);
        const float sw = ui_.textWidth(cheatStatusText(st), 12, Weight::Semibold) + 18;
        drawCardTile(ui_, code.id, px, y + 2, 40, t.panel);
        const std::string title = options ? "GT4's hidden menu: two Options pages" : std::string(code.name);
        ui_.text(ui_.fit(title, iw - 54 - sw - 16, 24, Weight::Bold), px + 54, y - 2, 24, t.text, Weight::Bold);
        const std::string sub = options ? (ce.secret ? "On the Options screen, a code on controller 1 or 2 adds them. No day rule."
                                                     : "On the Options screen, a code on controller 2 adds them. No day rule.")
                                        : cheatEffect(code, ce);
        ui_.text(ui_.fit(sub, iw - 54, 13), px + 54, y + 33, 13, t.muted);
        ui_.chip(d.x + d.width - 24 - sw, y + 4, cheatStatusText(st), ui::mixColor(t.panel, sc, 0.16f), sc, 12);
        y += 62;

        // The buttons at the bottom (their size decided first: the body takes the rest).
        const float buttonsY = d.y + d.height - 18 - 38;
        const float bodyTop = y, bodyBottom = buttonsY - 12;

        // ---- the body: measured at a few sizes, the first that fits is drawn; else it scrolls. What goes first when
        // room is short (1.4 review): the button size, then the keyboard line, the side note, the memory aid's box (it
        // becomes one line), the history, and last the memory aid (Show big and the saved card still have it), each of the
        // last two tried with smaller gaps too. The switch boxes and "Before you start" always stay.
        const float colGap = 24;
        struct Level
        {
            float tile;
            bool keyboard, note, history;
            int aid;            // 2: in its box, 1: one line, 0: left out
            bool tight = false; // smaller gaps between the parts (the last try before the body scrolls)
        };
        // (1.4 review, round 2) The tight tries let the Mission Hall card with its fix on fit the first window of both of
        // the developer's screens (1280 x 800 dp) without scrolling: its switch box, the code and "Before you start" in view.
        const std::vector<Level> codeLevels = {{44, true, true, true, 2},       {40, true, true, true, 2},   {36, true, true, true, 2},
                                               {36, false, true, true, 2},      {36, false, false, true, 2}, {36, false, false, true, 1},
                                               {36, false, false, false, 1},    {36, false, false, false, 1, true},
                                               {36, false, false, false, 0},    {36, false, false, false, 0, true}};
        // The hidden pages' card (its memory aid is always one line, its note is the pages' own): with controller 1
        // allowed, the keyboard line (Enter = Start there) outlasts the history.
        const std::vector<Level> optionLevels = {{44, true, true, true, 1}, {40, true, true, true, 1},   {36, true, true, true, 1},
                                                 {36, true, true, false, 1}, {36, false, true, false, 1}, {36, false, true, false, 0}};
        const std::vector<Level> &levels = options ? optionLevels : codeLevels;
        const bool met = cheatSaveState_ == 1 && dayRuleMet(cheatElapsed_);
        const long long day = cheatSaveState_ == 1 ? gameDay(cheatElapsed_) : 1;
        // "Before you start": (kind: 0 ok, 1 warn), text
        std::vector<std::pair<int, std::string>> before;
        if (!options)
        {
            if (ce.noday && !met)
                before.push_back({0, "Day 367 or later: skipped by the switch above"});
            else if (met)
                before.push_back({0, strf("Day 367 or later: your save is at Day %lld", day)});
            else
                before.push_back({1, cheatSaveState_ == 1 ? strf("Day 367 or later on GT4's Days counter (your save: Day %lld)", day)
                                                          : std::string("Day 367 or later on GT4's Days counter (a new game starts at Day 1)")});
            before.push_back({0, cheatWhere(code, ce)});
        }
        // GT4.exe reads the keyboard only while player 1 has no controller (ps2_pad.cpp: no bound device connected). No-break
        // spaces keep each "A / B" pair on one line.
        const std::string keyboard = options ? "Keyboard (controller 1, when no controller is connected): arrows, Enter = Start, Q\xc2\xa0/\xc2\xa0"
                                               "E = L1\xc2\xa0/\xc2\xa0R1, L\xc2\xa0/\xc2\xa0R Shift = L2\xc2\xa0/\xc2\xa0R2"
                                             : "Keyboard (when no controller is connected): arrows, Tab = Select, Q\xc2\xa0/\xc2\xa0"
                                               "E = L1\xc2\xa0/\xc2\xa0R1, L\xc2\xa0/\xc2\xa0R Shift = L2\xc2\xa0/\xc2\xa0R2";
        const std::string backup = options ? "Back up your save first (Saves page): what you set on the pages is saved."
                                           : "Back up your save first (Saves page): a code's result is saved.";

        auto body = [&](const Level &lv, float x, float yy, bool draw) -> float {
            const float y0 = yy;
            if (!options)
            {
                // chips, and "On my in-game card"
                const std::string cbLabel = "On my in-game card";
                const float cbW = 28 + ui_.textWidth(cbLabel, 14);
                if (draw)
                {
                    std::vector<std::string> chips{cheatScreen(code), cheatController(code, ce), cheatDays(code, ce), strf("%d presses", code.count)};
                    float total = 0;
                    for (const std::string &c : chips)
                        total += ui_.textWidth(c, 11.5f, Weight::Semibold) + 18 + 8;
                    if (total > iw - cbW - 12)
                        chips.erase(chips.begin()); // the screen is in the list and below
                    float cx = x;
                    for (const std::string &c : chips)
                    {
                        const float w = ui_.textWidth(c, 11.5f, Weight::Semibold) + 18;
                        if (cx + w > x + iw - cbW - 12)
                            break;
                        cx += ui_.chip(cx, yy, c, t.panel2, t.muted, 11.5f) + 8;
                    }
                    bool on = isTicked(cs, code.id);
                    if (ui_.checkbox("cheat-oncard", x + iw - cbW, yy + 1, on, cbLabel))
                    {
                        setTicked(cs, code.id, on);
                        markDirty();
                    }
                }
                const float gap = lv.tight ? 8.0f : 12.0f;
                yy += lv.tight ? 32 : 36;
                if (mission)
                {
                    // The Mission Hall fix (MISSION-FIX.md 0, 3.1): our fix, off by default. One text for both states, always
                    // drawn: what it does, when its results show (the parity version: the Clear mark at the next visit, a
                    // group's car at the next GT Mode load), and in amber the backup, the one-way pass and the full garage.
                    const bool can = gt4_.missionfix;
                    const std::string fixText =
                        can ? "Off: exactly as GT4 shipped, the code gives 10,000,000 credits (probably a bug). On: it passes the mission highlighted "
                              "when the last button goes in, as GT4's race code does: +1 day, no money, no A-spec points. The Clear mark shows the "
                              "next time you open the Mission Hall; a group's prize car arrives after your next race, or after Options and back. Type it "
                              "once per mission."
                            : "The GT4 program here does not have this fix: the code gives 10,000,000 credits, as GT4 shipped.";
                    const std::string fixBackup = can ? "Back up your save first (Saves page): a code's result is saved, a passed mission cannot be "
                                                        "un-passed, and with a full garage a group's prize car is lost (keep a free slot)."
                                                      : backup;
                    const float th = ui_.wrapHeight(fixText, iw - 84, 12.5f, Weight::Regular, 1.3f);
                    const float th2 = ui_.wrapHeight(fixBackup, iw - 84 - 18, 12.5f, Weight::Regular, 1.3f);
                    const float bh = 12 + 22 + 6 + th + 4 + th2 + 12;
                    if (draw)
                    {
                        ui_.round(R(x, yy, iw, bh), 10, t.panel2);
                        bool shown = can ? cs.missionfix : false;
                        if (ui_.toggle("cheat-missionfix", x + 16, yy + 12, can ? cs.missionfix : shown, can))
                            markDirty();
                        ui_.text("Fix the Mission Hall code", x + 68, yy + 12, 15, can ? t.text : t.muted, Weight::Semibold);
                        ui_.chip(x + 68 + ui_.textWidth("Fix the Mission Hall code", 15, Weight::Semibold) + 10, yy + 12, "our fix, off by default", t.panel3,
                                 t.muted, 11);
                        const float ty = yy + 40 + ui_.wrap(fixText, x + 68, yy + 40, iw - 84, 12.5f, t.muted, Weight::Regular, 1.3f) + 4;
                        ui_.circle(x + 68 + 6, ty + 8.5f, 6, ui::alpha(t.warn, 0.25f));
                        ui_.text("!", x + 68 + 6, ty, 12, t.warn, Weight::Bold, Align::Center);
                        ui_.wrap(fixBackup, x + 68 + 18, ty, iw - 84 - 18, 12.5f, t.warn, Weight::Regular, 1.3f);
                    }
                    yy += bh + gap;
                }
                if (draw)
                    ui_.text("Press, one at a time (tap, do not hold)", x, yy, 12.5f, t.faint, Weight::Semibold);
                yy += lv.tight ? 20 : 22;
                yy += (draw ? drawSequence(ui_, code, x, yy, iw, lv.tile, -1, darkPad(), true, false, t.panel) : sequenceHeight(code, iw, lv.tile, true)) + gap;
                // Remember it as: in its box, or on one line (as on the hidden pages' card) when room is short
                if (lv.aid == 1)
                {
                    const float lw = ui_.textWidth("Remember it as", 12.5f, Weight::Semibold) + 12;
                    const float ah = ui_.wrapHeight(std::string(code.aid), iw - lw, 13);
                    if (draw)
                    {
                        ui_.text("Remember it as", x, yy, 12.5f, t.accent, Weight::Semibold);
                        ui_.wrap(std::string(code.aid), x + lw, yy - 0.5f, iw - lw, 13, t.text);
                    }
                    yy += ah + gap;
                }
                else if (lv.aid == 2)
                {
                    const float labelW = ui_.textWidth("Remember it as", 12.5f, Weight::Semibold) + 32;
                    const float ah = ui_.wrapHeight(std::string(code.aid), iw - labelW - 16, 14) + 22;
                    if (draw)
                    {
                        ui_.round(R(x, yy, iw, ah), 10, ui::mixColor(t.panel, t.accent, 0.08f));
                        ui_.roundLine(R(x, yy, iw, ah), 10, 1, ui::mixColor(t.panel, t.accent, 0.35f));
                        ui_.text("Remember it as", x + 16, yy + 12, 12.5f, t.accent, Weight::Semibold);
                        ui_.wrap(std::string(code.aid), x + labelW, yy + 11, iw - labelW - 16, 14, t.text);
                    }
                    yy += ah + 16;
                }
                // Before you start, and a little history beside it (without the history, Before you start takes the width)
                const float colW = lv.history ? (iw - colGap) * 0.5f : iw;
                std::vector<std::pair<int, std::string>> items = before;
                if (lv.keyboard)
                    items.push_back({0, keyboard});
                if (!mission) // the Mission Hall card says it in its switch's box
                    items.push_back({1, backup});
                float by = yy + (lv.tight ? 20 : 24);
                for (const auto &[kind, text] : items)
                {
                    const float h = ui_.wrapHeight(text, colW - 22, 12.5f, Weight::Regular, 1.3f);
                    if (draw)
                    {
                        if (kind == 0)
                        {
                            ui_.line(x + 2, by + 9, x + 6, by + 13, 2, t.ok);
                            ui_.line(x + 6, by + 13, x + 13, by + 4, 2, t.ok);
                        }
                        else
                        {
                            ui_.circle(x + 7, by + 8.5f, 6, ui::alpha(t.warn, 0.25f));
                            ui_.text("!", x + 7, by, 12, t.warn, Weight::Bold, Align::Center);
                        }
                        ui_.wrap(text, x + 22, by, colW - 22, 12.5f, t.text, Weight::Regular, 1.3f);
                    }
                    by += h + (lv.tight ? 3 : 5);
                }
                if (draw)
                    ui_.text("Before you start", x, yy, 13, t.faint, Weight::Semibold);
                float hy = yy;
                if (lv.history)
                {
                    const std::string history = cheatHistory(code, ce), note = cheatNote(code, ce);
                    const float hx = x + colW + colGap;
                    hy = yy + 24;
                    if (draw)
                    {
                        ui_.text("A little history", hx, yy, 13, t.faint, Weight::Semibold);
                        hy += ui_.wrap(history, hx, hy, colW, 12.5f, t.muted, Weight::Regular, 1.33f);
                        if (lv.note && !note.empty())
                            hy += 6 + ui_.wrap(note, hx, hy + 6, colW, 12.5f, t.faint, Weight::Regular, 1.33f);
                    }
                    else
                    {
                        hy += ui_.wrapHeight(history, colW, 12.5f, Weight::Regular, 1.33f);
                        if (lv.note && !note.empty())
                            hy += 6 + ui_.wrapHeight(note, colW, 12.5f, Weight::Regular, 1.33f);
                    }
                }
                yy = std::max(by, hy);
                return yy - y0;
            }
            // ---- the hidden Options pages
            {
                const bool can = gt4_.cheats;
                const std::string unlockText =
                    !can        ? "The GT4 program here does not read this switch: GT4 takes this code from controller 2 only."
                    : ce.secret ? "On: GT4 also takes the code from controller 1 (one number in GT4's Options script, changed in memory)."
                                : "GT4 takes this code from controller 2 only. On: controller 1 may type it too (one number in GT4's Options "
                                  "script, changed in memory).";
                const std::string pagesNote = "The pages are gone when you leave Options, as on a PS2; what you set on them is saved with your game.";
                const float th = ui_.wrapHeight(unlockText + " " + pagesNote, iw - 84, 12.5f, Weight::Regular, 1.3f);
                const float bh2 = ui_.wrapHeight(backup, iw - 84 - 18, 12.5f, Weight::Regular, 1.3f);
                const float bh = 12 + 22 + 6 + th + 4 + bh2 + 12;
                if (draw)
                {
                    ui_.round(R(x, yy, iw, bh), 10, t.panel2);
                    bool shown = false;
                    if (ui_.toggle("cheat-secret", x + 16, yy + 12, can ? cs.secret : shown, can))
                        markDirty();
                    const std::string label = "Let controller 1 type the hidden menu code";
                    ui_.text(label, x + 68, yy + 12, 15, can ? t.text : t.muted, Weight::Semibold);
                    ui_.chip(x + 68 + ui_.textWidth(label, 15, Weight::Semibold) + 10, yy + 12, "off by default", t.panel3, t.muted, 11);
                    float ty = yy + 40;
                    ty += ui_.wrap(unlockText + " " + pagesNote, x + 68, ty, iw - 84, 12.5f, t.muted, Weight::Regular, 1.3f) + 4;
                    ui_.circle(x + 68 + 6, ty + 8.5f, 6, ui::alpha(t.warn, 0.25f));
                    ui_.text("!", x + 68 + 6, ty, 12, t.warn, Weight::Bold, Align::Center);
                    ui_.wrap(backup, x + 68 + 18, ty, iw - 84 - 18, 12.5f, t.warn, Weight::Regular, 1.3f);
                }
                yy += bh + 12;
                // the two pages
                struct PageInfo
                {
                    const char *title, *sub;
                    std::vector<std::string> rows;
                };
                const PageInfo pages[2] = {{"Special Settings",
                                            "Arcade Mode limits",
                                            {"Specific Cars: only your Favourites", "Specific Courses: only your registered courses",
                                             "Specific Opponents: from your Favourites",
                                             "Specific Makes: one make only (hides the button back to GT Mode; turning it off needs this page again)"}},
                                           {"Detail Monitor Settings",
                                            "Multi Monitor Mode geometry",
                                            {"Flag (rotate X / Y), Zoom, Distance", "Width, Height, Aspect, Overscan", "Border widths left / right / top / bottom (mm)",
                                             "Used by the side screens of GT4's LAN multi-monitor mode"}}};
                const float pw = (iw - 16) * 0.5f;
                float ph = 0;
                for (const PageInfo &pg : pages)
                {
                    float h = 36;
                    for (const std::string &r : pg.rows)
                        h += ui_.wrapHeight(r, pw - 46, 12, Weight::Regular, 1.25f) + 3;
                    ph = std::max(ph, h + 8);
                }
                if (draw)
                    for (int i = 0; i < 2; ++i)
                    {
                        const PageInfo &pg = pages[i];
                        const float bx = x + static_cast<float>(i) * (pw + 16);
                        ui_.round(R(bx, yy, pw, ph), 10, t.panel2);
                        ui_.text(pg.title, bx + 16, yy + 10, 14.5f, t.text, Weight::Semibold);
                        const float tw = ui_.textWidth(pg.title, 14.5f, Weight::Semibold);
                        ui_.text(ui_.fit(pg.sub, pw - 42 - tw, 12), bx + 26 + tw, yy + 12, 12, t.muted);
                        float ry = yy + 36;
                        for (const std::string &r : pg.rows)
                        {
                            ui_.circle(bx + 20, ry + 8, 2.4f, t.faint);
                            ry += ui_.wrap(r, bx + 30, ry, pw - 46, 12, t.text, Weight::Regular, 1.25f) + 3;
                        }
                    }
                yy += ph + 12;
                // the code, and "On my in-game card" (as on the other five cards)
                const std::string where = ce.secret ? "The code, on the Options screen (controller 1 or 2)"
                                          : can     ? "The code, on the Options screen (controller 2, or 1 with the switch on)"
                                                    : "The code, on the Options screen (controller 2)";
                const std::string cbLabel = "On my in-game card";
                const float cbW = 28 + ui_.textWidth(cbLabel, 14);
                if (draw)
                {
                    ui_.text(ui_.fit(where, iw - cbW - 16, 12.5f, Weight::Semibold), x, yy, 12.5f, t.faint, Weight::Semibold);
                    bool on = isTicked(cs, code.id);
                    if (ui_.checkbox("cheat-oncard", x + iw - cbW, yy - 3, on, cbLabel))
                    {
                        setTicked(cs, code.id, on);
                        markDirty();
                    }
                }
                yy += 24;
                const float ts = std::min(40.0f, lv.tile);
                yy += (draw ? drawSequence(ui_, code, x, yy, iw, ts, -1, darkPad(), true, false, t.panel) : sequenceHeight(code, iw, ts, true)) + 8;
                if (lv.aid)
                {
                    const float lw = ui_.textWidth("Remember it as", 12.5f, Weight::Semibold) + 12;
                    const float ah = ui_.wrapHeight(std::string(code.aid), iw - lw, 13);
                    if (draw)
                    {
                        ui_.text("Remember it as", x, yy, 12.5f, t.accent, Weight::Semibold);
                        ui_.wrap(std::string(code.aid), x + lw, yy - 0.5f, iw - lw, 13, t.text);
                    }
                    yy += ah + 8;
                }
                // With the unlock on, controller 1 may type it: the keyboard keys too (Start is Enter there).
                if (lv.keyboard && ce.secret)
                {
                    const float h = ui_.wrapHeight(keyboard, iw, 12.5f, Weight::Regular, 1.3f);
                    if (draw)
                        ui_.wrap(keyboard, x, yy, iw, 12.5f, t.muted, Weight::Regular, 1.3f);
                    yy += h + 4;
                }
                // controller 2 now (the Controllers page's Player 2), then the history
                const bool pad2Off = st_.pad2.mode == Pad2Settings::Mode::Off;
                const std::string orSwitch = can ? ", or turn the switch above on." : ".";
                const std::string c2 = ce.secret   ? std::string()
                                       : !gt4_.pad2 ? std::string("Controller 2: the GT4 program here has no second controller (port 2 stays empty), so it "
                                                                  "cannot take this code from one") + orSwitch
                                       : pad2Off    ? "Controller 2: Player 2 is Off on the Controllers page. Choose Automatic or a controller there" + orSwitch
                                       : p2         ? "Controller 2 now: " + p2name + " (Player 2 on the Controllers page)."
                                                    : "Controller 2: plug in a second controller (Player 2 on the Controllers page)" + orSwitch;
                if (!c2.empty())
                {
                    const float h = ui_.wrapHeight(c2, iw, 12.5f, Weight::Regular, 1.3f);
                    if (draw)
                        ui_.wrap(c2, x, yy, iw, 12.5f, p2 ? t.muted : t.warn, Weight::Regular, 1.3f);
                    yy += h + 4;
                }
                if (lv.history)
                {
                    const std::string h = cheatHistory(code, ce);
                    const float hh = ui_.wrapHeight(h, iw, 12.5f, Weight::Regular, 1.3f);
                    if (draw)
                        ui_.wrap(h, x, yy, iw, 12.5f, t.faint, Weight::Regular, 1.3f);
                    yy += hh;
                }
                return yy - y0;
            }
        };

        const float room = bodyBottom - bodyTop;
        const Level *pick = nullptr;
        float smallest = 0;
        for (const Level &lv : levels)
        {
            const float need = body(lv, px, bodyTop, false);
            smallest = need;
            if (need <= room)
            {
                pick = &lv;
                break;
            }
        }
        if (pick)
            body(*pick, px, bodyTop, true);
        else
        {
            // Too tall for this window (the smallest sizes): the body scrolls, with everything in it.
            const Level &lv = levels[2];
            const float h = body(lv, px, bodyTop, false);
            const float off = ui_.beginScroll("cheat-body", R(d.x + 1, bodyTop, d.width - 6, room + 4));
            body(lv, px, bodyTop - off, true);
            ui_.endScroll("cheat-body", h + 6);
        }
        cheatBodyScrolls_ = pick == nullptr;
        {
            const Level &lv = pick ? *pick : levels[2];
            std::vector<std::string> left;
            if (!lv.keyboard)
                left.push_back("keyboard line");
            if (!lv.note)
                left.push_back("side note");
            if (!lv.history)
                left.push_back("history");
            if (lv.aid == 1 && !gt4cheats::kCodes[static_cast<size_t>(cheatSel_)].controller2)
                left.push_back("the memory aid's box (on one line)");
            if (!lv.aid)
                left.push_back("memory aid");
            cheatLevelText_ = pick ? strf("buttons %.0f dp%s%s%s", lv.tile, left.empty() ? "" : "; left out: ", join(left, ", ").c_str(),
                                          lv.tight ? "; smaller gaps" : "")
                                   : strf("scrolls inside the card (everything in it; its smallest layout is %.1f dp taller than the %.1f dp of room)",
                                          smallest - room, room);
        }

        // ---- the buttons
        {
            struct B
            {
                const char *id;
                Icon icon;
                std::string label;
                Style style;
            };
            std::vector<B> bs = {{"cheat-big", Icon::Eye, "Show big", Style::Primary},
                                 {"cheat-practise", Icon::Gamepad, "Practise with my pad", Style::Secondary},
                                 {"cheat-save", Icon::Save, "Save card", Style::Secondary},
                                 {"cheat-print", Icon::Print, "Print or PDF...", Style::Secondary}};
            float size = 14.0f;
            bool icons = true;
            auto widthOf = [&](const B &b) { return std::floor(ui_.textWidth(b.label, size, Weight::Semibold) + (icons ? size * 1.25f + 8 : 0) + 40); };
            float total = 0;
            for (const B &b : bs)
                total += widthOf(b) + 10;
            if (total > iw)
            {
                size = 13.0f;
                total = 0;
                for (const B &b : bs)
                    total += widthOf(b) + 10;
            }
            if (total > iw)
                icons = false;
            float bx = px;
            for (const B &b : bs)
            {
                const float w = widthOf(b);
                const bool practise = std::string(b.id) == "cheat-practise";
                const bool clicked = icons ? ui_.iconButton(b.id, R(bx, buttonsY, w, 38), b.icon, b.label, b.style, !practise || !busy(), size)
                                           : ui_.button(b.id, R(bx, buttonsY, w, 38), b.label, b.style, !practise || !busy(), size);
                if (clicked)
                {
                    const std::string id = b.id;
                    if (id == "cheat-big" || id == "cheat-practise")
                    {
                        modal_ = Modal::CheatBig;
                        startPractise(id == "cheat-practise");
                    }
                    else if (id == "cheat-save")
                        cheatJob_ = CheatJob::Card;
                    else
                    {
                        modal_ = Modal::CheatSave;
                        cheatThumbKey_.clear();
                    }
                }
                bx += w + 10;
            }
        }
    }

    // ------------------------------------------------------------------ Show big, Practise
    void App::startPractise(bool on)
    {
        const gt4cheats::Code &code = gt4cheats::kCodes[static_cast<size_t>(cheatSel_)];
        cheatPractise_ = on && !busy();
        cheatFollower_.reset(&code);
        cheatFlash_ = 0;
        cheatFlashAt_ = -100.0;
        practiseHeld_ = {};
        if (cheatPractise_ && !hidden_)
            cheatFollower_.prime(practiseWord());
    }

    uint16_t App::practiseWord()
    {
        if (hidden_ || devices_.demo())
            return 0;
        const int j = padJoystick();
        if (j >= 0)
            return padWord(st_.pad, [&](int b) { return IsGamepadButtonDown(j, b); }, [&](int a) { return GetGamepadAxisMovement(j, a); }, practiseHeld_);
        practiseHeld_ = {};
        if (!st_.pad.keyboard)
            return 0;
        KeyboardKeys k{IsKeyDown(KEY_UP) || IsKeyDown(KEY_W), IsKeyDown(KEY_DOWN) || IsKeyDown(KEY_S), IsKeyDown(KEY_LEFT) || IsKeyDown(KEY_A),
                       IsKeyDown(KEY_RIGHT) || IsKeyDown(KEY_D), IsKeyDown(KEY_TAB), IsKeyDown(KEY_ENTER), IsKeyDown(KEY_Q), IsKeyDown(KEY_E),
                       IsKeyDown(KEY_LEFT_SHIFT), IsKeyDown(KEY_RIGHT_SHIFT), IsKeyDown(KEY_X) || IsKeyDown(KEY_SPACE), IsKeyDown(KEY_C),
                       IsKeyDown(KEY_Z) || IsKeyDown(KEY_KP_0), IsKeyDown(KEY_V) || IsKeyDown(KEY_KP_1)};
        return keyboardWord(k);
    }

    std::string App::practiseSource(bool *keyboardOnly) const
    {
        if (keyboardOnly)
            *keyboardOnly = false;
        if (devices_.demo())
        {
            for (const DeviceInfo &d : devices_.list())
                if (d.kind == DeviceKind::Gamepad && (st_.pad.sourceGuid.empty() || d.glfwGuid == st_.pad.sourceGuid))
                    return d.name;
            return "your controller";
        }
        const int j = padJoystick();
        if (j >= 0)
            for (const GamepadInfo &g : devices_.glfwGamepads())
                if (g.joystick == j)
                    return g.name;
        if (keyboardOnly)
            *keyboardOnly = true;
        return "the keyboard";
    }

    void App::drawCheatBig()
    {
        const ui::Theme &t = ui::theme();
        const gt4cheats::Code &code = gt4cheats::kCodes[static_cast<size_t>(cheatSel_)];
        const CheatSettings cs = cheatsInEffect(st_.cheats, gt4_); // what GT4.exe acts on
        const bool esc = IsKeyPressed(KEY_ESCAPE);
        // Follow the pad (a visible session; the test pictures hold a fixed state).
        if (cheatPractise_ && !hidden_)
        {
            if (busy())
                cheatPractise_ = false;
            else
            {
                const gt4cheats::Follower::Event e = cheatFollower_.sample(practiseWord());
                if (e == gt4cheats::Follower::Event::Completed)
                {
                    cheatFlash_ = 1;
                    cheatFlashAt_ = GetTime();
                }
                else if (e == gt4cheats::Follower::Event::Reset || e == gt4cheats::Follower::Event::Wrong)
                {
                    cheatFlash_ = 2;
                    cheatFlashAt_ = GetTime();
                }
            }
        }
        const int n = code.count;
        const int progress = cheatPractise_ ? (hidden_ ? std::min(cheatTestProgress_, n) : cheatFollower_.progress()) : -1;
        const double since = hidden_ ? 100.0 : GetTime() - cheatFlashAt_;

        const float mw = std::min(1180.0f, ui_.width - 60), mh = std::min(640.0f, ui_.height - 40);
        const Rectangle r = R(std::floor((ui_.width - mw) * 0.5f), std::floor((ui_.height - mh) * 0.5f), mw, mh);
        ui_.round(R(r.x + 3, r.y + 6, r.width, r.height), 16, ui::alpha(BLACK, 0.4f));
        ui_.round(r, 16, t.panel);
        ui_.roundLine(r, 16, 1, t.border);
        const float px = r.x + 36, iw = mw - 72;
        drawCardTile(ui_, code.id, px, r.y + 30, 48, t.panel);
        ui_.text(ui_.fit(code.controller2 ? std::string("Hidden Options pages") : std::string(code.name), iw - 64 - 90, 30, Weight::Bold), px + 64, r.y + 24, 30,
                 t.text, Weight::Bold);
        ui_.text(ui_.fit(cheatWhere(code, cs) + kDot + cheatController(code, cs) + kDot + cheatDays(code, cs), iw - 64 - 90, 14), px + 64, r.y + 68, 14, t.muted);
        const float kw = ui_.textWidth("Esc", 12, Weight::Semibold) + 16;
        ui_.keycap(r.x + mw - 36 - kw, r.y + 30, "Esc");
        ui_.text("closes", r.x + mw - 36, r.y + 58, 11.5f, t.faint, Weight::Regular, Align::Right);
        // the code, big
        const float s = std::min(78.0f, std::floor((mh - 360) / 2.4f));
        const float hseq = drawSequence(ui_, code, px, r.y + 128, iw, s, progress, darkPad(), true, true, t.panel);
        float y = r.y + 128 + hseq + 28;
        if (progress >= 0)
        {
            ui_.meter(R(px, y, iw, 8), static_cast<float>(progress) / static_cast<float>(n), t.ok);
            y += 22;
            if (cheatFlash_ == 1 && since < 2.0)
                ui_.text("Done: GT4 would run the cheat now. The count starts over.", px, y + 4, 18, t.ok, Weight::Semibold);
            else
            {
                ui_.text(strf("%d of %d done", progress, n), px, y + 4, 18, cheatFlash_ == 2 && since < 0.6 ? t.bad : t.text, Weight::Semibold);
                const uint16_t next = code.presses[static_cast<size_t>(std::min(progress, n - 1))];
                ui_.text("Next:", px + 150, y + 4, 18, t.muted);
                drawPadTile(ui_, next, px + 205, y - 2, 34, 2, darkPad(), 0, t.panel);
                ui_.text(strf("%s (button %d)", cheatButtonName(next).c_str(), progress + 1), px + 252, y + 4, 18, t.text, Weight::Semibold);
            }
            const std::string wrong = "Wrong button: the code starts over from 1, and that press does not count (as in GT4).";
            ui_.text(ui_.fit(wrong, iw - 560, 13.5f), r.x + mw - 36, y + 7, 13.5f, t.muted, Weight::Regular, Align::Right);
            y += 52;
        }
        const float labelW = ui_.textWidth("Remember it as", 14, Weight::Semibold) + 48;
        const float ah = ui_.wrapHeight(std::string(code.aid), iw - labelW - 18, 17) + 28;
        ui_.round(R(px, y, iw, ah), 12, ui::mixColor(t.panel, t.accent, 0.08f));
        ui_.roundLine(R(px, y, iw, ah), 12, 1, ui::mixColor(t.panel, t.accent, 0.35f));
        ui_.text("Remember it as", px + 18, y + 15, 14, t.accent, Weight::Semibold);
        ui_.wrap(std::string(code.aid), px + labelW, y + 13, iw - labelW - 18, 17, t.text);
        y += ah + 18;
        ui_.wrap("Tap each button once: a held d-pad can repeat in GT4's menus and break the code. The dots mark the memory-aid groups.", px, y, iw, 13.5f, t.faint);
        // footer
        const float fy = r.y + mh - 30 - 40;
        if (cheatPractise_)
        {
            bool keys = false;
            const std::string src = practiseSource(&keys);
            ui_.statusDot(px + 6, fy + 20, t.ok);
            ui_.text(ui_.fit("Practise is on: reading " + src + (keys ? "" : " (player 1)") + ". Nothing is sent to GT4.", iw - 340, 13.5f), px + 20, fy + 11, 13.5f,
                     t.muted);
            const bool ps = [&] {
                for (const DeviceInfo &d : devices_.list())
                    if (d.glfwGuid == st_.pad.sourceGuid && d.vid == 0x054c)
                        return true;
                return false;
            }();
            const std::string names = keys ? std::string("Keys: arrows, Tab = Select, Enter = Start, Q / E = L1 / R1, Left / Right Shift = L2 / R2")
                                           : "Your pad: " + padNamesLine(st_.pad, ps);
            ui_.text(ui_.fit(names, iw - 340, 12.5f), px + 20, fy + 31, 12.5f, t.faint);
            if (ui_.button("cheat-restart", R(r.x + mw - 36 - 300, fy + 4, 140, 38), "Start over", Style::Secondary))
            {
                cheatFollower_.restart();
                cheatFlash_ = 0;
            }
        }
        else
        {
            ui_.text(busy() ? "Practise works while GT4 is closed (the launcher lets go of your controllers while it runs)."
                            : "Practise lights each button as you press it on your controller. Nothing is sent to GT4.",
                     px, fy + 20, 13, t.faint);
            if (ui_.iconButton("cheat-practise-on", R(r.x + mw - 36 - 380, fy + 4, 220, 38), Icon::Gamepad, "Practise with my pad", Style::Secondary, !busy()))
                startPractise(true);
        }
        if (ui_.iconButton("cheat-big-save", R(r.x + mw - 36 - 150, fy + 4, 150, 38), Icon::Save, "Save card", Style::Secondary))
            cheatJob_ = CheatJob::Card;
        if (esc)
        {
            modal_ = Modal::None;
            cheatPractise_ = false;
        }
    }

    // ------------------------------------------------------------------ Save or print
    void App::drawCheatSave()
    {
        const ui::Theme &t = ui::theme();
        CheatSettings &cs = st_.cheats;
        const bool esc = IsKeyPressed(KEY_ESCAPE);
        const float mw = std::min(1060.0f, ui_.width - 60), mh = std::min(620.0f, ui_.height - 40);
        const Rectangle r = R(std::floor((ui_.width - mw) * 0.5f), std::floor((ui_.height - mh) * 0.5f), mw, mh);
        ui_.round(R(r.x + 3, r.y + 6, r.width, r.height), 16, ui::alpha(BLACK, 0.4f));
        ui_.round(r, 16, t.panel);
        ui_.roundLine(r, 16, 1, t.border);
        const float px = r.x + 32;
        ui_.text("Save or print cheat cards", px, r.y + 26, 24, t.text, Weight::Bold);
        ui_.text(ui_.fit("Pick the cards, then save them as pictures next to GT4's photos, as one PDF sheet, or print the sheet.", mw - 64, 13.5f), px, r.y + 62,
                 13.5f, t.muted);
        // left: which cards, the paper
        float y = r.y + 104;
        ui_.text("Cards", px, y, 13, t.faint, Weight::Semibold);
        y += 26;
        for (const gt4cheats::Code &c : gt4cheats::kCodes)
        {
            bool on = isTicked(cs, c.id);
            if (ui_.checkbox("save-tick-" + std::string(c.id), px, y, on, std::string(c.name)))
            {
                setTicked(cs, c.id, on);
                markDirty();
            }
            y += 32;
        }
        y += 8;
        ui_.text("Paper", px, y, 13, t.faint, Weight::Semibold);
        y += 24;
        int paper = cs.letter ? 1 : 0;
        if (ui_.segmented("cheat-paper", R(px, y, 200, 32), {"A4", "Letter"}, paper, true, 13))
        {
            cs.letter = paper == 1;
            markDirty();
        }
        y += 46;
        ui_.checkbox("cheat-ink", px, y, cheatInk_, "Ink saver (white paper)");
        // middle: where, the files
        const float mx = px + 290, mwid = std::min(400.0f, mw - 290 - 236 - 96);
        y = r.y + 104;
        ui_.text("Saves to your Photos folder (GT4's USB stick)", mx, y, 13, t.faint, Weight::Semibold);
        y += 24;
        ui_.round(R(mx, y, mwid, 40), 8, t.panel2);
        ui_.icon(Icon::Folder, R(mx + 12, y + 10, 20, 20), t.muted);
        const std::string change = "Change on the Saves page";
        const float chw = ui_.textWidth(change, 11.5f);
        ui_.text(ui_.fit(shortPath(usbRoot() / "GT4", inst_.root), mwid - 60 - chw, 14, Weight::Regular, true), mx + 42, y + 11, 14, t.text);
        ui_.text(change, mx + mwid - 8, y + 13, 11.5f, t.faint, Weight::Regular, Align::Right);
        y += 54;
        ui_.text("Files", mx, y, 13, t.faint, Weight::Semibold);
        y += 24;
        const std::vector<const gt4cheats::Code *> ticked = tickedCodes(cs);
        if (ticked.empty())
        {
            ui_.text("Tick at least one card.", mx, y, 13, t.warn);
            y += 22;
        }
        const CheatSettings ce = cheatsInEffect(cs, gt4_);
        for (const gt4cheats::Code *c : ticked)
        {
            ui_.text(ui_.fit(cheatCardFileName(*c, ce), mwid, 13), mx, y, 13, t.text);
            y += 22;
        }
        if (!ticked.empty())
        {
            ui_.text(cheatSheetFileName(), mx, y, 13, t.text);
            y += 22;
        }
        y += 10;
        y += ui_.wrap("GT4 does not see these files: its USB stick shows only short 8.3 names like IMG0001.JPG and skips the rest, so they never fill "
                      "its photo list.",
                      mx, y, mwid, 12.5f, t.muted, Weight::Regular, 1.3f);
        ui_.wrap("Shift-click Save PDF to pick another folder or name (a short 8.3 name in the Photos folder is refused).", mx, y + 6, mwid, 12, t.faint,
                 Weight::Regular, 1.3f);
        // right: the sheet
        const float tw = 236, th = std::floor(236.0f * (cs.letter ? 11.0f / 8.5f : 297.0f / 210.0f));
        const float tx = r.x + mw - 32 - tw, ty = r.y + 104;
        ui_.text(cheatThumbPages_ > 1 ? strf("Sheet preview (page 1 of %d)", cheatThumbPages_) : std::string("Sheet preview"), tx, ty, 13, t.faint, Weight::Semibold);
        ui_.round(R(tx - 4, ty + 20, tw + 8, th + 8), 6, t.panel2);
        if (cheatThumb_.id)
        {
            const Rectangle dst = ui_.px(R(tx, ty + 24, tw, th));
            DrawTexturePro(cheatThumb_, Rectangle{0, 0, static_cast<float>(cheatThumb_.width), static_cast<float>(cheatThumb_.height)}, dst, Vector2{0, 0}, 0, WHITE);
        }
        else
            ui_.text("...", tx + tw * 0.5f, ty + 24 + th * 0.5f - 10, 14, t.faint, Weight::Regular, Align::Center);
        // buttons
        const float by = r.y + mh - 32 - 40;
        const bool any = !ticked.empty();
        if (ui_.iconButton("cheat-print-go", R(r.x + mw - 32 - 150, by, 150, 40), Icon::Print, "Print...", Style::Primary, any, 14.5f))
            cheatJob_ = CheatJob::Print;
        if (ui_.iconButton("cheat-pdf", R(r.x + mw - 32 - 310, by, 150, 40), Icon::Save, "Save PDF", Style::Secondary, any, 14.5f))
            cheatJob_ = IsKeyDown(KEY_LEFT_SHIFT) || IsKeyDown(KEY_RIGHT_SHIFT) ? CheatJob::PdfAs : CheatJob::Pdf;
        if (ui_.iconButton("cheat-pictures", R(r.x + mw - 32 - 500, by, 180, 40), Icon::Save, "Save pictures", Style::Secondary, any, 14.5f))
            cheatJob_ = CheatJob::Pictures;
        if (ui_.button("cheat-save-close", R(px, by, 110, 40), "Close", Style::Secondary, true, 14.5f) || esc)
            modal_ = Modal::None;
    }
}
