// GT4 Recompilation launcher, Full edition: Driving > Race HUD > Timing (developer, 20 Oct 2026: "can we have a page for timing
// show like there is for radar an lights"). The timing tower (GT4_HUD_TOWER=on,<x>,<y>,<size>,<see-through>,<name>,<columns>,
// <rows>; HUD hand-off 7 C) and the split times (GT4_HUD_SPLITS=on,<x>,<y>,<size>,<see-through>) on a page of their own, as the
// Radar and Start lights pages are: the stock chase picture with both boxes where GT4.exe puts them (dragged and resized there
// too), a close-up of the tower with a sample race, and the settings. The same keys and values the HUD layout page's tower
// and split times boxes edit (both pages change the same FullSettings); nothing new for GT4.exe. The tower's Driver names
// come from the drivers the Race Lab sent to GT4 (racelab-data\ai-lines\gt4-drivers.txt, GT4_AI_DRIVERS; gt4_ai_drivers.h).

#include "gt4stock_app.h"

#include "gt4full_racelab.h"
#include "gt4full_racetiming.h"
#include "gt4full_ui.h"
#include "gt4stock_resources.h"
#include "gt4stock_util.h"
#include "shared/gt4_hud_look.h"

#include <algorithm>
#include <cmath>
#include <vector>

namespace gt4stock
{
    using ui::Align;
    using ui::Style;
    using ui::Weight;

    extern Rectangle g_raceHudBoxDp[12]; // (gt4full_racehud.cpp: our boxes as last drawn, kOwn's order: the tower 5, the splits 7)

    namespace
    {
        constexpr float kSidebarW = 232.0f;
        Rectangle R(float x, float y, float w, float h) { return Rectangle{x, y, w, h}; }

        // ---- the drivers GT4 would show (gt4_ai_drivers.h: "car <k> <name>", AI car k = the k-th car that is not yours)
        constexpr int kMaxAi = 7;
        struct Drivers
        {
            std::string names[kMaxAi + 1];
            bool on = true;
            int test = 0;
            double at = -100.0;
            fs::path root;
        };
        Drivers g_drv;
        // (the Race Lab's first five default drivers, as its AI drivers tab gives them to AI cars 1-5)
        const char *const kTestDrivers[5] = {"Kenji Arata", "Sofia Lindqvist", "Dale Brennan", "Am\xc3\xa9lie Duret", "Tom\xc3\xa1s Varga"};

        void readDrivers(const fs::path &file, std::string (&names)[kMaxAi + 1])
        {
            for (std::string &n : names)
                n.clear();
            std::string text;
            if (!readText(file, text))
                return;
            bool header = false;
            size_t i = 0;
            while (i <= text.size())
            {
                size_t e = text.find('\n', i);
                if (e == std::string::npos)
                    e = text.size();
                const std::string l = trim(std::string_view(text).substr(i, e - i));
                i = e + 1;
                if (l.empty() || l[0] == '#')
                    continue;
                if (!header)
                {
                    if (!startsWith(l, "gt4-ai-drivers "))
                        return;
                    header = true;
                    continue;
                }
                if (startsWith(l, "car "))
                {
                    const size_t sp = l.find(' ', 4);
                    const int k = std::atoi(l.c_str() + 4);
                    if (k >= 1 && k <= kMaxAi && sp != std::string::npos)
                        names[k] = trim(std::string_view(l).substr(sp + 1));
                }
            }
        }

        // A name in plain ASCII letters for our HUD's font, as GT4.exe spells it (gt4_ai_drivers.cpp asciiName: U+00C0..U+00FF
        // without their accents; any other letter '?').
        std::string asciiName(const std::string &u)
        {
            static const char kLatin1[] = "AAAAAAACEEEEIIIIDNOOOOOxOUUUUYTsaaaaaaaceeeeiiiidnooooo/ouuuuyty";
            std::string out;
            for (size_t i = 0; i < u.size();)
            {
                const unsigned char c = static_cast<unsigned char>(u[i]);
                if (c < 0x80)
                {
                    out += static_cast<char>(c);
                    ++i;
                    continue;
                }
                const int len = c >= 0xf0 ? 4 : c >= 0xe0 ? 3 : c >= 0xc0 ? 2 : 1;
                if (len == 2 && i + 1 < u.size())
                {
                    const unsigned cp = ((c & 0x1fu) << 6) | (static_cast<unsigned char>(u[i + 1]) & 0x3fu);
                    out += cp >= 0xc0 && cp <= 0xff ? kLatin1[cp - 0xc0] : '?';
                }
                else
                    out += '?';
                i += static_cast<size_t>(len);
            }
            return out;
        }

        // The tower's "driver" name (gt4_hud.cpp towerDriverName): the driver, two spaces, the car's short code; shortened to fit
        // as "K. Arata  FKA", then "Arata  FKA", then the driver alone.
        std::string driverName(const std::string &driver, const std::string &code, float fs, float width)
        {
            const size_t sp = driver.find_last_of(' ');
            const std::string surname = sp == std::string::npos ? driver : driver.substr(sp + 1);
            const std::string tries[3] = {driver, sp == std::string::npos ? driver : driver.substr(0, 1) + ". " + surname, surname};
            for (const std::string &d : tries)
                if (gt4hudlook::textWidth(d + "  " + code, fs) <= width)
                    return d + "  " + code;
            return driver;
        }

        // ---- the sample race (Autumn Ring Mini's Sunday Cup field, as on the HUD layout page; you are car 0, the Ka, in 3rd)
        struct SampleCar
        {
            int car, place;
            const char *name, *code, *gap, *iv, *best, *last;
        };
        constexpr SampleCar kField[] = {{3, 1, "Honda S500 '63", "HS5", "LEAD", "", "1'01.069", "1'01.069"},
                                        {2, 2, "Daihatsu Midget '63", "DMI", "+1.204", "+1.204", "1'02.410", "1'02.410"},
                                        {0, 3, "Ford Ka '01", "FKA", "+2.877", "+1.673", "1'03.802", "1'03.802"},
                                        {5, 4, "Honda N360 '67", "HN3", "+4.103", "+1.226", "1'04.115", "1'04.230"},
                                        {1, 5, "VW Beetle 1100 Standard '49", "VBE", "+6.550", "+2.447", "1'05.006", "1'05.006"},
                                        {4, 6, "Subaru 360 '58", "S36", "+9.012", "+2.462", "1'06.645", "1'06.645"}};
        constexpr int kFieldCount = static_cast<int>(sizeof(kField) / sizeof(kField[0]));

        // The tower with the sample race, as GT4.exe draws it (gt4_hud.cpp drawTower): the rows kept around you. rDp is the box
        // (dp), Hdp the HUD area's height (dp; the box's size applied here).
        void towerSample(ui::Ui &u, const Rectangle &rDp, const FullHudWidget &h, float alpha, float Hdp)
        {
            namespace look = gt4hudlook;
            const float k = Hdp * u.s * std::clamp(h.size, 40, 250) / 100.0f;
            const int name = std::clamp(h.towerName, 0, 2);
            const int rows = std::min(std::clamp(h.towerRows, 4, 16), kFieldCount);
            const int me = 2, first = std::clamp(me - rows / 2, 0, std::max(0, kFieldCount - rows));
            look::TowerRow t[kFieldCount];
            for (int i = 0; i < rows; ++i)
            {
                const SampleCar &c = kField[first + i];
                t[i].place = c.place, t[i].car = c.car, t[i].player = c.car == 0, t[i].purpleBest = first + i == 0;
                t[i].name = racetiming::towerName(name, c.car, c.car == 0, c.name, c.code, gt4hudbox::kTowerRow * k * 0.5f, racetiming::driverWidth(k));
                t[i].vals[0] = first + i == 0 ? "LEAD" : c.gap, t[i].vals[1] = c.iv, t[i].vals[2] = c.best, t[i].vals[3] = c.last;
            }
            look::tower(u.px(rDp), alpha, k, name, h.towerCols, t, rows);
        }
        void splitsSample(ui::Ui &u, const Rectangle &rDp, float alpha)
        {
            const gt4hudlook::Split cells[3] = {gt4hudlook::splitDone("22.540", 0), gt4hudlook::splitDone("28.064", 1), gt4hudlook::splitRunning("9.311")};
            gt4hudlook::splits(u.px(rDp), alpha, cells, 3);
        }

        // ---- the position presets: a box's centre (% of the HUD area) from an edge of GT4's own HUD in the stock chase picture
        // (hud-pictures .json: best and last lap down to 23.4 % of the height, its right edge at 93.2 % across; the race clock
        // down to 23.7 %; the compact panel from 82.9 % down). bw, bh: the box (fractions of the width / height).
        struct Preset
        {
            const char *name;
            int kind; // 0 the centre at a, b; 1 the right edge at a, the top at b; 2 the right edge at a, the bottom at b;
                      // 3 the centre across at a, the top at b; 4 the centre across at a, the bottom at b
            float a, b;
        };
        const Preset kTowerPresets[] = {{"Right side", 0, 87.0f, 46.0f},
                                        {"Top right, below GT4's best and last lap", 1, 93.2f, 24.9f},
                                        {"Bottom right, above GT4's panel", 2, 93.2f, 81.4f},
                                        {"Top centre, below GT4's race clock", 3, 50.0f, 25.2f}};
        const Preset kSplitsPresets[] = {{"Middle", 0, 50.0f, 40.0f},
                                         {"Top centre, below GT4's race clock", 3, 50.0f, 25.2f},
                                         {"Bottom centre, above GT4's panel", 4, 50.0f, 81.4f}};
        void presetCentre(const Preset &p, float bw, float bh, float &x, float &y)
        {
            float cx = p.a / 100.0f, cy = p.b / 100.0f;
            if (p.kind == 1 || p.kind == 2)
                cx -= bw * 0.5f;
            if (p.kind == 1 || p.kind == 3)
                cy += bh * 0.5f;
            if (p.kind == 2 || p.kind == 4)
                cy -= bh * 0.5f;
            if (p.kind != 0) // (a centre is kept as given, as the defaults are: GT4.exe keeps the box inside)
            {
                cx = std::clamp(cx, std::min(bw * 0.5f, 0.5f), std::max(1.0f - bw * 0.5f, 0.5f));
                cy = std::clamp(cy, std::min(bh * 0.5f, 0.5f), std::max(1.0f - bh * 0.5f, 0.5f));
            }
            x = std::round(cx * 1000.0f) / 10.0f, y = std::round(cy * 1000.0f) / 10.0f;
        }
    } // namespace

    namespace racetiming
    {
        void refreshDrivers(const fs::path &root, bool driversOn, bool hidden)
        {
            g_drv.on = driversOn;
            if (g_drv.test == 1)
            {
                for (int k = 0; k <= kMaxAi; ++k)
                    g_drv.names[k] = k >= 1 && k <= 5 ? kTestDrivers[k - 1] : "";
                return;
            }
            const double now = GetTime();
            if (root != g_drv.root || g_drv.at < -50.0 || (!hidden && now - g_drv.at > 3.0))
            {
                readDrivers(root / L"racelab-data" / L"ai-lines" / L"gt4-drivers.txt", g_drv.names);
                g_drv.root = root, g_drv.at = now;
            }
        }

        std::string towerName(int name, int car, bool player, const char *carName, const char *code, float fs, float width)
        {
            if (name == 2)
            {
                const std::string drv = player ? std::string("You") : g_drv.on && car >= 1 && car <= kMaxAi ? asciiName(g_drv.names[car]) : std::string();
                if (!drv.empty())
                    return driverName(drv, code, fs, width);
            }
            return name == 1 ? std::string(code) : std::string(carName);
        }

        float driverWidth(float k)
        {
            // (gt4hudlook::tower's name column less its right padding: what the name is fitted to there)
            return (gt4hudbox::kTowerNameW[2] - gt4hudbox::kTowerDot * 0.1f - gt4hudbox::kTowerPad * 0.5f) * k;
        }

        void driversForTest(int mode)
        {
            g_drv.test = mode;
            g_drv.at = -100.0; // (read again: the test GT4 folder's file, if any)
            if (mode != 1)
                for (std::string &n : g_drv.names)
                    n.clear();
        }
    } // namespace racetiming

    void App::pageRaceHudTiming(float top)
    {
        const ui::Theme &t = ui::theme();
        FullSettings &f = st_.full;
        FullHudWidget &tw = f.liveWidgets[static_cast<size_t>(kFullHudTower)];
        FullHudWidget &sp = f.liveWidgets[static_cast<size_t>(kFullHudSplits)];
        const float x0 = kSidebarW + 32, x1 = ui_.width - 32, bottom = ui_.height - 48;
        const float colW = 360.0f, gap = 22.0f;
        const float leftW = x1 - x0 - colW - gap;
        const float A = fullAspectRatioOf(f.aspect);
        racetiming::refreshDrivers(inst_.root, f.raceLabDrivers, hidden_);
        int sent = 0; // (AI cars with a driver GT4 would show)
        for (int k = 1; k <= kMaxAi; ++k)
            sent += f.raceLabDrivers && !g_drv.names[k].empty() ? 1 : 0;

        // ---- the preview: the stock chase picture (GT4's own HUD as a race draws it; the Radar page's) with both boxes
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
        // A box in the picture as GT4.exe places it (gt4_hud.cpp draw): its centre kept inside the picture; an older file's place,
        // or Automatic (bottom right, lifted over the chase views' panel unless GT4's panel is hidden).
        const bool panelHidden = (f.hudHide >> kFullHudHidePanel) & 1;
        auto boxRect = [&](int w, const FullHudWidget &h) {
            float bw = 0, bh = 0;
            gt4hudbox::baseSize(w, h.style, h.towerName, h.towerCols, h.towerRows, bw, bh);
            const float s = std::clamp(h.size, 40, 250) / 100.0f, ww = bw * s * S.height, wh = bh * s * S.height;
            if (h.xy)
                return R(std::clamp(S.x + h.x / 100.0f * S.width - ww * 0.5f, S.x, std::max(S.x, S.x + S.width - ww)),
                         std::clamp(S.y + h.y / 100.0f * S.height - wh * 0.5f, S.y, std::max(S.y, S.y + S.height - wh)), ww, wh);
            int p = std::clamp(h.place, 0, 7);
            float lift = 0.0f;
            if (p == 7)
                p = 0, lift = panelHidden ? 0.0f : 0.14f;
            const float m = 0.02f * S.height;
            const bool bot = p == 0 || p == 1 || p == 4 || p == 6, right = p == 0 || p == 2, centre = p == 4 || p == 5 || p == 6;
            const float wx = centre ? S.x + (S.width - ww) * 0.5f : right ? S.x + S.width - m - ww : S.x + m;
            const float floor = p == 6 ? S.y + S.height * 0.69f : lift > 0.0f ? S.y + S.height * (1.0f - lift) : S.y + S.height - m;
            return R(wx, bot ? floor - wh : S.y + m, ww, wh);
        };
        // The tower's panel covers the rows there are cars for (six in the sample race), from the top of its box.
        auto towerShown = [&](const Rectangle &r) {
            const int rows = std::min(std::clamp(tw.towerRows, 4, 16), kFieldCount);
            const float hh = (gt4hudbox::kTowerHead + gt4hudbox::kTowerRow * rows) * std::clamp(tw.size, 40, 250) / 100.0f * S.height;
            return R(r.x, r.y, r.width, std::min(r.height, hh));
        };
        const Rectangle tr = boxRect(kFullHudTower, tw), sr = boxRect(kFullHudSplits, sp), trS = towerShown(tr);
        const float tAlpha = 1.0f - std::clamp(tw.seeThrough, 0, 90) / 100.0f, sAlpha = 1.0f - std::clamp(sp.seeThrough, 0, 90) / 100.0f;
        g_raceHudBoxDp[5] = R(0, 0, 0, 0), g_raceHudBoxDp[7] = R(0, 0, 0, 0);
        auto outline = [&](const Rectangle &r, const char *id) {
            const bool hot = hudDrag_ == id || (hudDrag_.empty() && ui_.hovered(r));
            ui_.roundLine(R(r.x - 2, r.y - 2, r.width + 4, r.height + 4), 4, hot ? 2.0f : 1.0f, ui::alpha(t.accent, hot ? 1.0f : 0.6f));
        };
        if (sp.on)
        {
            splitsSample(ui_, sr, sAlpha);
            outline(sr, "splits");
            g_raceHudBoxDp[7] = sr;
        }
        if (tw.on)
        {
            towerSample(ui_, tr, tw, tAlpha, S.height);
            outline(trS, "tower");
            g_raceHudBoxDp[5] = trS;
        }
        if (!tw.on && !sp.on)
        {
            ui_.round(R(S.x + S.width * 0.5f - 190, S.y + S.height * 0.5f - 18, 380, 36), 8, ui::alpha(BLACK, 0.6f));
            ui_.text("The timing tower and the split times are off", S.x + S.width * 0.5f, S.y + S.height * 0.5f - 9, 14, t.text, Weight::Semibold,
                     Align::Center);
        }
        // Drag them in the picture (their centre, % of the picture: "Where I put it"), the mouse wheel to resize them.
        auto moveTo = [&](FullHudWidget &h, const Rectangle &r, float cxFrac, float cyFrac) {
            const float hw = r.width * 0.5f / S.width, hh = r.height * 0.5f / S.height;
            h.xy = true;
            h.x = std::round(std::clamp(cxFrac, std::min(hw, 0.5f), std::max(1.0f - hw, 0.5f)) * 1000.0f) / 10.0f;
            h.y = std::round(std::clamp(cyFrac, std::min(hh, 0.5f), std::max(1.0f - hh, 0.5f)) * 1000.0f) / 10.0f;
            markDirty();
        };
        if (ui_.inputOn() && !ui_.popupOpen())
        {
            const Vector2 m = ui_.mouseDp();
            const ui::Input &in = ui_.input();
            const bool onTower = tw.on && CheckCollisionPointRec(m, trS), onSplits = sp.on && CheckCollisionPointRec(m, sr);
            if (in.pressed && (onTower || onSplits))
            {
                const Rectangle &r = onTower ? tr : sr;
                hudDrag_ = onTower ? "tower" : "splits";
                hudDragOff_ = Vector2{m.x - (r.x + r.width * 0.5f), m.y - (r.y + r.height * 0.5f)};
            }
            if (!in.down)
                hudDrag_.clear();
            if (hudDrag_ == "tower" || hudDrag_ == "splits")
            {
                const bool isTower = hudDrag_ == "tower";
                const Rectangle &r = isTower ? tr : sr;
                const float cx = m.x - hudDragOff_.x, cy = m.y - hudDragOff_.y;
                if (std::fabs(cx - (r.x + r.width * 0.5f)) > 0.25f || std::fabs(cy - (r.y + r.height * 0.5f)) > 0.25f)
                    moveTo(isTower ? tw : sp, r, (cx - S.x) / S.width, (cy - S.y) / S.height);
            }
            if (in.wheel != 0.0f && (onTower || onSplits))
            {
                FullHudWidget &h = onTower ? tw : sp;
                h.size = std::clamp(h.size + (in.wheel > 0 ? 5 : -5), 40, 250);
                markDirty();
            }
        }
        ui_.text(ui_.fit("GT4's own chase view with your timing tower and split times: drag them to move them, the mouse wheel to resize them.", sw,
                         12.5f),
                 x0, S.y + S.height + 8, 12.5f, t.muted);

        // ---- the close-up: the tower at a readable size, with what each column is
        const float cy0 = S.y + S.height + 32;
        const float chH = std::max(60.0f, bottom - cy0);
        float tbw = 0, tbh = 0;
        gt4hudbox::towerSize(tw.towerName, tw.towerCols, tw.towerRows, tbw, tbh);
        const int shown = std::min(std::clamp(tw.towerRows, 4, 16), kFieldCount);
        const float kc = std::min({chH / (gt4hudbox::kTowerHead + gt4hudbox::kTowerRow * shown), leftW * 0.56f / std::max(tbw, 0.1f), 900.0f});
        {
            FullHudWidget c = tw;
            c.size = 100;
            towerSample(ui_, R(x0, cy0, tbw * kc, tbh * kc), c, std::max(tAlpha, 0.75f), kc);
        }
        const float tx = x0 + tbw * kc + 20, txw = x0 + leftW - tx;
        float ty = cy0;
        ui_.text(tw.on ? "Close-up" : "Close-up (the tower is off)", tx, ty, 14, t.text, Weight::Semibold);
        ty += 24;
        std::string cols;
        static const char *const kColWords[4] = {"GAP: behind the leader", "INT: behind the car ahead", "BEST: each car's best lap (purple: the quickest this race)",
                                                 "LAST: its last lap"};
        for (int c = 0; c < 4; ++c)
            if ((tw.towerCols >> c) & 1)
                cols += std::string(cols.empty() ? "" : ". ") + kColWords[c];
        ty += ui_.wrap(cols.empty() ? std::string("No columns: place, colour dot and name only.") : cols + ".", tx, ty, txw, 12.5f, t.muted) + 8;
        const int rowsSet = std::clamp(tw.towerRows, 4, 16);
        ty += ui_.wrap(rowsSet < kFieldCount ? strf("%d rows: GT4's races have six cars, so the tower shows the %d around you. Yours is gold.", rowsSet, rowsSet)
                         : rowsSet == kFieldCount ? strf("%d rows: every car of GT4's six. Yours is gold.", rowsSet)
                                                  : strf("%d rows: every car of GT4's six (the box keeps room for %d). Yours is gold.", rowsSet, rowsSet),
                       tx, ty, txw, 12.5f, t.muted) +
              10;
        {
            // What this page writes to gt4-start.env, when there is room for it.
            const std::vector<std::string> env = {"GT4_HUD_TOWER=" + fullHudValue(kFullHudTower, tw, A), "GT4_HUD_SPLITS=" + fullHudValue(kFullHudSplits, sp, A),
                                                  std::string("GT4_AI_DRIVERS=") + (f.raceLabDrivers ? "racelab-data\\ai-lines" : "off")};
            const float boxH = 26 + 16.0f * env.size();
            if (ty + boxH <= bottom && txw > 200)
            {
                const Rectangle eb = R(tx, ty, txw, boxH);
                ui_.round(eb, 8, ui::mixColor(t.bg, BLACK, 0.3f));
                ui_.roundLine(eb, 8, 1, t.border);
                ui_.text(ui_.fit("# gt4-start.env", txw - 24, 11.5f, Weight::Mono), tx + 12, ty + 8, 11.5f, t.faint, Weight::Mono);
                float ey = ty + 26;
                for (const std::string &l : env)
                {
                    ui_.text(ui_.fit(l, txw - 24, 11.5f, Weight::Mono), tx + 12, ey, 11.5f, Color{0x9f, 0xd9, 0xcf, 255}, Weight::Mono);
                    ey += 16;
                }
            }
        }

        // ---- the settings card: the tower (show, position, across, down, size, see-through, names, columns, rows), then the
        // split times (show, position, size, see-through), and a reset
        const Rectangle card = R(x1 - colW, top, colW, bottom - top);
        ui_.round(card, 12, t.panel);
        ui_.roundLine(card, 12, 1, t.border);
        const Rectangle view2 = R(card.x, card.y + 8, card.width, card.height - 16);
        const float off = ui_.beginScroll("timing-col", view2);
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
        // A box's show, across, down, size and see-through rows (the position list's place kept for last: placeY).
        auto placeRows = [&](const char *pre, FullHudWidget &h, const Rectangle &r, float &placeY) {
            label("Show");
            int show = h.on ? 1 : 0;
            if (ui_.segmented(std::string(pre) + "-show", R(lx, y, lw, 30), {"Off", "On"}, show, true, 12))
            {
                h.on = show == 1;
                markDirty();
            }
            y += 36;
            label("Position");
            placeY = y;
            y += 36;
            float ax = (r.x + r.width * 0.5f - S.x) / S.width * 100.0f, ay = (r.y + r.height * 0.5f - S.y) / S.height * 100.0f;
            if (h.xy)
                ax = h.x, ay = h.y;
            if (slider(std::string(pre) + "-x", "Across", ax, 0.0f, 100.0f, 0.5f, strf("%.1f %%", ax), h.on))
                moveTo(h, r, ax / 100.0f, (h.xy ? h.y : ay) / 100.0f);
            if (slider(std::string(pre) + "-y", "Down", ay, 0.0f, 100.0f, 0.5f, strf("%.1f %%", ay), h.on))
                moveTo(h, r, (h.xy ? h.x : ax) / 100.0f, ay / 100.0f);
            float sz = static_cast<float>(h.size);
            if (slider(std::string(pre) + "-size", "Size", sz, 40.0f, 250.0f, 5.0f, strf("%d %%", h.size), h.on))
            {
                h.size = std::clamp(static_cast<int>(std::lround(sz)), 40, 250);
                markDirty();
            }
            float see = static_cast<float>(h.seeThrough);
            if (slider(std::string(pre) + "-see", "See-through", see, 0.0f, 90.0f, 5.0f, strf("%d %%", h.seeThrough), h.on))
            {
                h.seeThrough = std::clamp(static_cast<int>(std::lround(see)), 0, 90);
                markDirty();
            }
        };
        // The position list: the presets, Automatic (an older file's), or "Where I put it" (drawn last, over the rows below).
        auto placeList = [&](const char *pre, int w, FullHudWidget &h, const Preset *presets, int count, float placeY) {
            float bw = 0, bh = 0;
            gt4hudbox::baseSize(w, h.style, h.towerName, h.towerCols, h.towerRows, bw, bh);
            const float s = std::clamp(h.size, 40, 250) / 100.0f;
            bw = bw * s / std::max(A, 0.1f), bh *= s;
            std::vector<std::string> opts;
            int sel = -1;
            for (int p = 0; p < count; ++p)
            {
                opts.push_back(presets[p].name);
                float px = 0, py = 0;
                presetCentre(presets[p], bw, bh, px, py);
                if (h.xy && std::fabs(px - h.x) < 0.15f && std::fabs(py - h.y) < 0.15f)
                    sel = p;
            }
            if (sel < 0)
                opts.push_back(!h.xy && h.place == 7 ? "Automatic" : "Where I put it"), sel = count;
            const float yy = y;
            y = placeY;
            if (ui_.dropdown(std::string(pre) + "-place", R(lx, y, lw, 30), opts, sel, h.on, 12.5f) && sel < count)
            {
                presetCentre(presets[sel], bw, bh, h.x, h.y);
                h.xy = true;
                markDirty();
            }
            y = yy;
        };

        fullui::caps(ui_, cx0, y, "THE TIMING TOWER", t.faint);
        y += 20;
        float towerPlaceY = 0, splitsPlaceY = 0;
        placeRows("timing-t", tw, tr, towerPlaceY);
        label("Names");
        int nm = std::clamp(tw.towerName, 0, 2);
        if (ui_.segmented("timing-t-name", R(lx, y, lw, 30), {"Car", "Short", "Driver"}, nm, tw.on, 12))
        {
            tw.towerName = nm;
            markDirty();
        }
        y += 36;
        {
            // What Driver needs: the drivers sent from the Race Lab (its AI drivers tab, Send to GT4), and its names on.
            std::string note;
            Color nc = t.muted;
            if (nm != 2)
                note = "Car: the car's name. Short: a three-letter code from it. Driver: the drivers you send from the Race Lab.";
            else if (!f.raceLabDrivers)
            {
                note = "Driver needs the Race Lab's drivers, and Race Lab > Settings > Drivers' names in GT4 is off: GT4's AI cars show their car "
                       "names. Yours shows as You.";
                nc = t.warn;
            }
            else if (sent == 0)
            {
                note = "Driver needs drivers sent from the Race Lab: none yet, so GT4's AI cars show their car names (yours shows as You). Pick "
                       "them in Race Lab > AI drivers, then Send to GT4.";
                nc = t.warn;
            }
            else
            {
                std::string who;
                for (int k = 1; k <= kMaxAi; ++k)
                    if (!g_drv.names[k].empty())
                        who += (who.empty() ? "" : ", ") + g_drv.names[k];
                note = strf("Driver: the drivers sent from the Race Lab (%s), each with its car's short code; yours shows as You. An AI car "
                            "without one keeps its car's name.",
                            who.c_str());
            }
            y += ui_.wrap(note, cx0, y, cw, 12, nc) + 8;
            if (ui_.button("timing-racelab", R(cx0, y, cw, 30), "Open the Race Lab's AI drivers", nm == 2 && sent == 0 ? Style::Primary : Style::Secondary,
                           true, 13))
            {
                page_ = Page::RaceLab;
                racelab::showTabForTest(0); // (its AI drivers tab)
                ui_.closePopups();
                hudDrag_.clear();
            }
            y += 40;
        }
        label("Columns", 3);
        {
            static const char *const kColLabels[4] = {"Gap to the leader", "Gap to the car ahead", "Best lap", "Last lap"};
            for (int c = 0; c < 4; ++c)
            {
                bool on = (tw.towerCols >> c) & 1;
                if (ui_.checkbox(std::string("timing-t-col-") + gt4hudbox::kTowerCols[c], lx, y, on, kColLabels[c], tw.on))
                {
                    tw.towerCols = on ? tw.towerCols | (1 << c) : tw.towerCols & ~(1 << c);
                    markDirty();
                }
                y += 28;
            }
            y += 4;
        }
        {
            float rows = static_cast<float>(std::clamp(tw.towerRows, 4, 16));
            if (slider("timing-t-rows", "Rows", rows, 4.0f, 16.0f, 1.0f, std::to_string(std::clamp(tw.towerRows, 4, 16)), tw.on))
            {
                tw.towerRows = std::clamp(static_cast<int>(std::lround(rows)), 4, 16);
                markDirty();
            }
            y += ui_.wrap("How many places it shows at most. The rows stay around you; GT4's own races have six cars.", cx0, y, cw, 12, t.muted) + 12;
        }

        fullui::caps(ui_, cx0, y, "SPLIT TIMES", t.faint);
        y += 20;
        placeRows("timing-s", sp, sr, splitsPlaceY);
        y += ui_.wrap("GT4's three timing lines a lap. Purple: the quickest of every car this race; green: your best; yellow: slower than your "
                      "best. The sector you are in counts up.",
                      cx0, y, cw, 12, t.muted) +
             12;
        if (ui_.button("timing-reset", R(cx0, y, cw, 30), "Reset timing to defaults", Style::Secondary, true, 13))
        {
            // Each box's defaults (kFullHudLiveDefaults: place, size, see-through, the tower's names, columns and rows), shown or
            // not as it was.
            const bool tOn = tw.on, sOn = sp.on;
            tw = kFullHudLiveDefaults[kFullHudTower], sp = kFullHudLiveDefaults[kFullHudSplits];
            tw.on = tOn, sp.on = sOn;
            markDirty();
        }
        y += 40;
        // The position lists last, the lower one first, so an open list is drawn over the rows below it.
        placeList("timing-s", kFullHudSplits, sp, kSplitsPresets, static_cast<int>(sizeof(kSplitsPresets) / sizeof(kSplitsPresets[0])), splitsPlaceY);
        placeList("timing-t", kFullHudTower, tw, kTowerPresets, static_cast<int>(sizeof(kTowerPresets) / sizeof(kTowerPresets[0])), towerPlaceY);
        ui_.endScroll("timing-col", y + off - card.y);
    }
} // namespace gt4stock
