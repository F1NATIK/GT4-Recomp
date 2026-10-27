// GT4 Recompilation launcher (Classic edition; internal names keep "stock"): the Cheats page's files (1.4;
// CHEATS-TAB-DESIGN.md 7). "Save card" draws one card as a 1920 x 1080 picture and saves it as a PNG in the
// Photos folder (GT4's USB stick, <GT4_USB_ROOT>\GT4; its long name has no 8.3 form, so GT4's stick skips it).
// "Save PDF" draws the ticked cards on A4 or Letter paper at 300 dpi and writes them as a PDF (one picture
// per page, gt4stock_cheats.cpp). "Print..." opens Windows' print window, only when the player clicks it;
// the hidden test mode never does (sys::setNoShell) and writes its files to --cheat-files instead of the
// GT4 folder. Every picture is drawn off screen, outside a frame (prepareCheats).

#include "gt4stock_app.h"

#include "gt4stock_cheats.h"
#include "gt4stock_util.h"

#include <algorithm>
#include <cmath>
#include <ctime>
#include <fstream>

namespace gt4stock
{
    using ui::Align;
    using ui::Icon;
    using ui::Weight;

    namespace
    {
        Rectangle R(float x, float y, float w, float h) { return Rectangle{x, y, w, h}; }
        const char *const kDot = "  \xc2\xb7  ";

        std::string todayText()
        {
            static const char *const months[12] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
            const std::time_t now = std::time(nullptr);
            std::tm tm{};
            localtime_s(&tm, &now);
            return strf("%d %s %d", tm.tm_mday, months[tm.tm_mon], tm.tm_year + 1900);
        }
    }

    // A picture drawn with the page's own UI at w x h pixels (dp = 1280 x 800 scaled to fit, as a window of that size),
    // into a render target: no window is shown or resized. The buttons and tiles are the page's (cheatTile,
    // cheatSequence in gt4stock_cheatspage.cpp).
    Image App::renderOffscreen(int w, int h, Color clear, const std::function<void()> &draw)
    {
        RenderTexture2D rt = LoadRenderTexture(w, h);
        if (rt.id == 0)
            return Image{};
        BeginTextureMode(rt);
        ClearBackground(clear);
        ui::Input none;
        ui_.begin(w, h, none);
        draw();
        EndTextureMode();
        Image img = LoadImageFromTexture(rt.texture);
        UnloadRenderTexture(rt);
        ImageFlipVertical(&img);
        ImageFormat(&img, PIXELFORMAT_UNCOMPRESSED_R8G8B8);
        return img;
    }

    bool App::exportPng(const Image &img, const fs::path &file)
    {
        // raylib's ExportImage takes a narrow (ANSI code page) path; a Photos folder with other letters would fail there.
        if (!img.data)
            return false;
        int size = 0;
        unsigned char *png = ExportImageToMemory(img, ".png", &size);
        if (!png || size <= 0)
            return false;
        bool ok = false;
        {
            std::ofstream out(file, std::ios::binary | std::ios::trunc);
            out.write(reinterpret_cast<const char *>(png), size);
            ok = static_cast<bool>(out);
        }
        MemFree(png);
        if (!ok)
        {
            std::error_code ec;
            fs::remove(file, ec);
        }
        return ok;
    }

    // ------------------------------------------------------------------ one card, 1920 x 1080 (mock-up 7)
    Image App::renderCheatCard(const gt4cheats::Code &code)
    {
        const ui::Theme &t = ui::theme();
        const CheatSettings cs = cheatsInEffect(st_.cheats, gt4_);
        return renderOffscreen(1920, 1080, t.bg, [&] {
            // Laid out in the picture's pixels: dp = px / 1.35 at 1920 x 1080.
            const float k = 1.0f / ui_.s;
            auto P = [&](float px) { return px * k; };
            ui_.fill(R(0, 0, ui_.width, ui_.height), t.bg);
            ui_.round(R(P(48), P(48), P(1920 - 96), P(1080 - 96)), P(28), t.panel);
            ui_.roundLine(R(P(48), P(48), P(1920 - 96), P(1080 - 96)), P(28), P(2), t.border);
            const float px = P(110), iw = P(1920 - 220);
            cheatTile(code, px, P(104), P(96), t.panel);
            const std::string name = code.controller2 ? std::string("Hidden Options pages") : std::string(code.name);
            ui_.text(name, P(234), P(92), P(72), t.text, Weight::Bold);
            const std::string effect = code.controller2 ? std::string(code.effect) : cheatEffect(code, cs);
            ui_.text(ui_.fit(effect, iw - P(126), P(32)), P(236), P(170), P(32), t.muted);
            float cx = px;
            const std::vector<std::string> chips = {cheatController(code, cs), cheatDays(code, cs), strf("%d presses", code.count),
                                                    code.dayRule ? std::string("Day 367 or the skip switch") : code.controller2 ? std::string("Controller 2, or the launcher's switch")
                                                                                                                               : std::string("No day rule")};
            for (const std::string &c : chips)
                cx += ui_.chip(cx, P(246), c, t.panel2, t.muted, P(26)) + P(14);
            const float hseq = cheatSequence(code, px, P(322), iw, P(112), -1, false, true, t.panel);
            float y = P(322) + hseq + P(26);
            const float lw = P(290);
            const float ah = ui_.wrapHeight(std::string(code.aid), iw - lw - P(30), P(32)) + P(40);
            ui_.round(R(px, y, iw, ah), P(20), ui::mixColor(t.panel, t.accent, 0.08f));
            ui_.roundLine(R(px, y, iw, ah), P(20), P(2), ui::mixColor(t.panel, t.accent, 0.35f));
            ui_.text("Remember it as", px + P(30), y + P(22), P(28), t.accent, Weight::Semibold);
            ui_.wrap(std::string(code.aid), px + lw, y + P(20), iw - lw - P(30), P(32), t.text);
            y += ah + P(24);
            std::string pre = "Before you start: " + cheatWhere(code, cs) + " Tap each button once; a wrong button starts the code over.";
            if (code.id == "mission" && cs.missionfix)
                pre += " Type it once per mission: its Clear mark shows the next time you open the Mission Hall. Back up your save first, and "
                       "keep a free garage slot: with a full garage a group's prize car is lost.";
            y += ui_.wrap(pre, px, y, iw, P(26), t.text, Weight::Regular, 1.3f) + P(8);
            ui_.wrap(cheatHistory(code, cs), px, y, iw, P(24), t.muted, Weight::Regular, 1.3f);
            ui_.text("GT4 Recompilation" + std::string(kDot) + "cheat card" + kDot + "Gran Turismo 4 (USA, SCUS-97328)", P(960), P(982), P(22), t.faint,
                     Weight::Regular, Align::Center);
        });
    }

    // ------------------------------------------------------------------ the sheet (mock-up 6), 300 dpi
    std::vector<Image> App::renderCheatSheet(const std::vector<const gt4cheats::Code *> &codes, bool letter, bool ink)
    {
#if defined(GT4L_EDITION_FULL)
        // The Full edition (launcher 1.5; developer 2026-10-07: all six cards on ONE page): every ticked card on one page, A4 or
        // Letter. The blocks go in one or two columns, scaled together to the largest size that fits; the buttons stay at
        // least 60% of the Classic sheet's, still readable.
        const Paper paper = paperFor(letter);
        const ui::Theme &t = ui::theme();
        const Color bg = ink ? WHITE : t.bg, inkc = ink ? Color{20, 22, 26, 255} : t.text, sub = ink ? Color{90, 96, 106, 255} : t.muted;
        const Color line = ink ? Color{205, 209, 216, 255} : t.border, box = ink ? Color{243, 246, 250, 255} : t.panel2, block = ink ? WHITE : t.panel;
        const CheatSettings cs = cheatsInEffect(st_.cheats, gt4_);
        auto does = [&](const gt4cheats::Code &c) { return cheatSheetDoes(c, cs); };
        const std::string rules = "The five GT Mode codes need Day 367 or later on GT4's Days counter (or the launcher's \"Skip the 365-day wait\" switch). Any "
                                  "controller. Tap each button once, in order; a wrong button starts the code over. The hidden Options code needs "
                                  "controller 2 (or the launcher's \"Let controller 1 type the hidden menu code\" switch).";
        struct SheetFit
        {
            int cols = 1;
            float k = 1.0f;
        } best;
        float top = 0.0f;
        const float gap = 15.5f;
        auto blockHeight = [&](const gt4cheats::Code &c, float iw, float k) {
            const float hseq = cheatSequenceHeight(c, iw, 47.5f * k, true);
            const float dh = ui_.wrapHeight(does(c), iw, 16.5f * k, Weight::Regular, 1.3f) + 6 * k;
            const float ah = ui_.wrapHeight("Remember it as: " + std::string(c.aid), iw, 15.5f * k, Weight::Semibold, 1.3f);
            const float hh = ui_.wrapHeight(cheatHistory(c, cs), iw, 13.4f * k, Weight::Regular, 1.3f);
            const float wh = ui_.wrapHeight("Where: " + cheatWhere(c, cs), iw, 16.5f * k, Weight::Regular, 1.3f);
            return (17.6f + 28.9f * 1.33f + 6 + 15.5f * 1.33f + 8) * k + dh + wh + 8 * k + hseq + 18 * k + ah + 7.2f * k + hh + 17.6f * k;
        };
        // The columns' blocks: the first half on the left, the rest on the right (reading order).
        auto columnsOf = [&](int cols) {
            std::vector<std::vector<const gt4cheats::Code *>> out(static_cast<size_t>(cols));
            const size_t perCol = (codes.size() + static_cast<size_t>(cols) - 1) / static_cast<size_t>(cols);
            for (size_t i = 0; i < codes.size(); ++i)
                out[std::min(static_cast<size_t>(cols) - 1, perCol ? i / perCol : 0)].push_back(codes[i]);
            return out;
        };
        {
            ui::Input none;
            ui_.begin(paper.widthPx, paper.heightPx, none);
            const float W = ui_.width, m = 77.4f;
            top = 160 + ui_.wrapHeight(rules, W - 2 * m - 31, 17, Weight::Regular, 1.3f) + 25 + 20.6f;
            const float limit = ui_.height - 90;
            float bestK = -1.0f;
            for (int cols = 1; cols <= 2; ++cols)
            {
                const float colW = (W - 2 * m - (cols - 1) * gap) / cols, iw = colW - 31;
                const auto split = columnsOf(cols);
                for (int step = 0; step <= 20; ++step)
                {
                    const float k = 1.0f - 0.02f * static_cast<float>(step);
                    float tallest = 0;
                    for (const auto &col : split)
                    {
                        float h = 0;
                        for (const gt4cheats::Code *c : col)
                            h += blockHeight(*c, iw, k) + gap;
                        tallest = std::max(tallest, h);
                    }
                    if (top + tallest <= limit)
                    {
                        if (k > bestK + 0.001f)
                        {
                            bestK = k;
                            best = SheetFit{cols, k};
                        }
                        break;
                    }
                }
            }
            if (bestK < 0)
                best = SheetFit{2, 0.6f}; // the smallest size (GT4's six cards fit before this)
        }
        const std::string date = todayText();
        std::vector<Image> pages;
        pages.push_back(renderOffscreen(paper.widthPx, paper.heightPx, bg, [&] {
            const float W = ui_.width, H = ui_.height, m = 77.4f;
            const float k = best.k;
            ui_.fill(R(0, 0, W, H), bg);
            // the mark, the title, the rules
            const Rectangle tile = R(m, 67, 62, 62);
            ui_.round(tile, 13.4f, t.red);
            for (int i = 0; i < 9; ++i)
                ui_.fill(R(tile.x + 5, tile.y + 8.3f + i * 5.7f, tile.width - 10, 2), ui::alpha(BLACK, 0.18f));
            ui_.text("4", tile.x + tile.width * 0.5f, tile.y + 2, 49.5f, WHITE, Weight::Bold, Align::Center);
            ui_.text("GT4 cheat sheet", m + 82.6f, 64, 41.3f, inkc, Weight::Bold);
            ui_.text("Gran Turismo 4 (USA, SCUS-97328)" + std::string(kDot) + "from GT4 Recompilation" + kDot + "printed " + date, m + 83.6f, 114.6f, 17.5f, sub);
            const float rh = ui_.wrapHeight(rules, W - 2 * m - 31, 17, Weight::Regular, 1.3f) + 25;
            ui_.round(R(m, 160, W - 2 * m, rh), 9.3f, box);
            ui_.roundLine(R(m, 160, W - 2 * m, rh), 9.3f, 1.0f, line);
            ui_.wrap(rules, m + 15.5f, 160 + 12.4f, W - 2 * m - 31, 17, inkc, Weight::Regular, 1.3f);
            const float colW = (W - 2 * m - (best.cols - 1) * gap) / best.cols, iw = colW - 31;
            const auto split = columnsOf(best.cols);
            for (size_t ci = 0; ci < split.size(); ++ci)
            {
                const float bx = m + static_cast<float>(ci) * (colW + gap);
                float by = top;
                for (const gt4cheats::Code *c : split[ci])
                {
                    const float bh = blockHeight(*c, iw, k);
                    ui_.round(R(bx, by, colW, bh), 9.3f, block);
                    ui_.roundLine(R(bx, by, colW, bh), 9.3f, 1.5f, line);
                    float yy = by + 17.6f * k;
                    const std::string name = c->controller2 ? std::string("Hidden Options pages") : std::string(c->name);
                    ui_.text(name, bx + 15.5f, yy, 28.9f * k, inkc, Weight::Bold);
                    yy += 28.9f * 1.33f * k + 6 * k;
                    ui_.text(ui_.fit(cheatDays(*c, cs) + kDot + cheatController(*c, cs) + kDot + strf("%d presses", c->count), iw, 15.5f * k), bx + 15.5f, yy,
                             15.5f * k, sub);
                    yy += 15.5f * 1.33f * k + 8 * k;
                    yy += ui_.wrap(does(*c), bx + 15.5f, yy, iw, 16.5f * k, inkc, Weight::Regular, 1.3f) + 6 * k;
                    yy += ui_.wrap("Where: " + cheatWhere(*c, cs), bx + 15.5f, yy, iw, 16.5f * k, inkc, Weight::Regular, 1.3f) + 8 * k;
                    cheatSequence(*c, bx + 15.5f, yy, iw, 47.5f * k, -1, ink, false, block);
                    yy += cheatSequenceHeight(*c, iw, 47.5f * k, true) + 18 * k;
                    yy += ui_.wrap("Remember it as: " + std::string(c->aid), bx + 15.5f, yy, iw, 15.5f * k, inkc, Weight::Semibold, 1.3f) + 7.2f * k;
                    ui_.wrap(cheatHistory(*c, cs), bx + 15.5f, yy, iw, 13.4f * k, sub, Weight::Regular, 1.3f);
                    by += bh + gap;
                }
            }
            ui_.text("Codes checked against the menu scripts on the USA disc. Unofficial, fan-made, personal use; not affiliated with Sony or Polyphony "
                     "Digital.",
                     W * 0.5f, H - 62, 12.4f, sub, Weight::Regular, Align::Center);
        }));
        return pages;
#else
        const Paper paper = paperFor(letter);
        const ui::Theme &t = ui::theme();
        // White paper (ink saver), or the launcher's dark card colours.
        const Color bg = ink ? WHITE : t.bg, inkc = ink ? Color{20, 22, 26, 255} : t.text, sub = ink ? Color{90, 96, 106, 255} : t.muted;
        const Color line = ink ? Color{205, 209, 216, 255} : t.border, box = ink ? Color{243, 246, 250, 255} : t.panel2, block = ink ? WHITE : t.panel;
        const CheatSettings cs = cheatsInEffect(st_.cheats, gt4_);
        // What each block says it does (1.4 review: "Mission Hall code" alone does not say it).
        auto does = [&](const gt4cheats::Code &c) { return cheatSheetDoes(c, cs); };
        // The blocks' heights first (in dp of a 1280-dp-wide page), then pages.
        std::vector<Image> pages;
        // The blocks are measured first, at the page's scale (ui_'s text measures; nothing is drawn), and
        // laid out on as many pages as they need.
        std::vector<std::vector<const gt4cheats::Code *>> perPage;
        {
            ui::Input none;
            ui_.begin(paper.widthPx, paper.heightPx, none);
            const float W = ui_.width, m = 77.4f, inner = W - 2 * m - 31;
            const std::string rules = "The five GT Mode codes need Day 367 or later on GT4's Days counter (or the launcher's \"Skip the 365-day wait\" switch). Any "
                                      "controller. Tap each button once, in order; a wrong button starts the code over. The hidden Options code needs "
                                      "controller 2 (or the launcher's \"Let controller 1 type the hidden menu code\" switch).";
            const float limit = ui_.height - 90;
            float y = 160 + ui_.wrapHeight(rules, W - 2 * m - 31, 17, Weight::Regular, 1.3f) + 25 + 20.6f;
            std::vector<const gt4cheats::Code *> cur;
            for (const gt4cheats::Code *c : codes)
            {
                const float tile = 47.5f;
                const float hseq = cheatSequenceHeight(*c, inner, tile, true);
                const float dh = ui_.wrapHeight(does(*c), inner, 16.5f, Weight::Regular, 1.3f) + 6;
                const float ah = ui_.wrapHeight("Remember it as: " + std::string(c->aid), inner, 15.5f, Weight::Semibold, 1.3f);
                const float hh = ui_.wrapHeight(cheatHistory(*c, cs), inner, 13.4f, Weight::Regular, 1.3f);
                const float bh = 20.6f + 36 + dh + 27.9f + hseq + 18 + ah + 7.2f + hh + 17.6f;
                if (!cur.empty() && y + bh > limit)
                {
                    perPage.push_back(cur);
                    cur.clear();
                    y = 70;
                }
                cur.push_back(c);
                y += bh + 15.5f;
            }
            if (!cur.empty() || perPage.empty())
                perPage.push_back(cur);
        }
        const std::string date = todayText();
        for (size_t pi = 0; pi < perPage.size(); ++pi)
        {
            const auto &list = perPage[pi];
            pages.push_back(renderOffscreen(paper.widthPx, paper.heightPx, bg, [&] {
                const float W = ui_.width, H = ui_.height, m = 77.4f, inner = W - 2 * m - 31;
                ui_.fill(R(0, 0, W, H), bg);
                float y = 70;
                if (pi == 0)
                {
                    // the mark, the title, the rules
                    const Rectangle tile = R(m, 67, 62, 62);
                    ui_.round(tile, 13.4f, t.red);
                    for (int i = 0; i < 9; ++i)
                        ui_.fill(R(tile.x + 5, tile.y + 8.3f + i * 5.7f, tile.width - 10, 2), ui::alpha(BLACK, 0.18f));
                    ui_.text("4", tile.x + tile.width * 0.5f, tile.y + 2, 49.5f, WHITE, Weight::Bold, Align::Center);
                    ui_.text("GT4 cheat sheet", m + 82.6f, 64, 41.3f, inkc, Weight::Bold);
                    ui_.text("Gran Turismo 4 (USA, SCUS-97328)" + std::string(kDot) + "from GT4 Recompilation" + kDot + "printed " + date, m + 83.6f, 114.6f, 17.5f, sub);
                    y = 160;
                    const std::string rules = "The five GT Mode codes need Day 367 or later on GT4's Days counter (or the launcher's \"Skip the 365-day wait\" switch). Any "
                                              "controller. Tap each button once, in order; a wrong button starts the code over. The hidden Options code needs "
                                              "controller 2 (or the launcher's \"Let controller 1 type the hidden menu code\" switch).";
                    const float rh = ui_.wrapHeight(rules, W - 2 * m - 31, 17, Weight::Regular, 1.3f) + 25;
                    ui_.round(R(m, y, W - 2 * m, rh), 9.3f, box);
                    ui_.roundLine(R(m, y, W - 2 * m, rh), 9.3f, 1.0f, line);
                    ui_.wrap(rules, m + 15.5f, y + 12.4f, W - 2 * m - 31, 17, inkc, Weight::Regular, 1.3f);
                    y += rh + 20.6f;
                }
                for (const gt4cheats::Code *c : list)
                {
                    const float tile = 47.5f;
                    const float hseq = cheatSequenceHeight(*c, inner, tile, true);
                    const std::string aid = "Remember it as: " + std::string(c->aid);
                    const std::string hist = cheatHistory(*c, cs);
                    const std::string what = does(*c);
                    const float dh = ui_.wrapHeight(what, inner, 16.5f, Weight::Regular, 1.3f) + 6;
                    const float ah = ui_.wrapHeight(aid, inner, 15.5f, Weight::Semibold, 1.3f);
                    const float hh = ui_.wrapHeight(hist, inner, 13.4f, Weight::Regular, 1.3f);
                    const float bh = 20.6f + 36 + dh + 27.9f + hseq + 18 + ah + 7.2f + hh + 17.6f;
                    ui_.round(R(m, y, W - 2 * m, bh), 9.3f, block);
                    ui_.roundLine(R(m, y, W - 2 * m, bh), 9.3f, 1.5f, line);
                    float yy = y + 17.6f;
                    const std::string name = c->controller2 ? std::string("Hidden Options pages") : std::string(c->name);
                    ui_.text(name, m + 15.5f, yy, 28.9f, inkc, Weight::Bold);
                    const float nw = ui_.textWidth(name, 28.9f, Weight::Bold);
                    ui_.text(cheatDays(*c, cs) + kDot + cheatController(*c, cs) + kDot + strf("%d presses", c->count), m + 15.5f + nw + 15.5f, yy + 11,
                             15.5f, sub);
                    yy += 36;
                    yy += ui_.wrap(what, m + 15.5f, yy, inner, 16.5f, inkc, Weight::Regular, 1.3f) + 6;
                    ui_.text(ui_.fit("Where: " + cheatWhere(*c, cs), inner, 16.5f), m + 15.5f, yy, 16.5f, inkc);
                    yy += 27.9f;
                    cheatSequence(*c, m + 15.5f, yy, inner, tile, -1, ink, false, block);
                    yy += hseq + 18;
                    yy += ui_.wrap(aid, m + 15.5f, yy, inner, 15.5f, inkc, Weight::Semibold, 1.3f) + 7.2f;
                    ui_.wrap(hist, m + 15.5f, yy, inner, 13.4f, sub, Weight::Regular, 1.3f);
                    y += bh + 15.5f;
                }
                if (perPage.size() > 1)
                    ui_.text(strf("Page %zu of %zu", pi + 1, perPage.size()), W - m, H - 62, 12.4f, sub, Weight::Regular, Align::Right);
                ui_.text("Codes checked against the menu scripts on the USA disc. Unofficial, fan-made, personal use; not affiliated with Sony or Polyphony "
                         "Digital.",
                         W * 0.5f, H - 62, 12.4f, sub, Weight::Regular, Align::Center);
            }));
        }
        return pages;
#endif
    }

    // ------------------------------------------------------------------ outside the frame: thumbnails, saves, print
    void App::prepareCheats(int widthPx, int heightPx)
    {
        const CheatSettings &cs = st_.cheats;
        if (modal_ == Modal::CheatSave)
        {
            // Not keyed by the window's size (1.4 review): the page is made once at a size good for every window and drawn
            // scaled (mipmaps), so resizing the window never redraws the sheet.
            const CheatSettings ce = cheatsInEffect(cs, gt4_);
            std::string key = join(cs.ticked, ",") + (cs.letter ? "|letter" : "|a4") + (cheatInk_ ? "|ink" : "|dark") + (ce.missionfix ? "|fix" : "") +
                              (ce.secret ? "|secret" : "");
            if (key != cheatThumbKey_)
            {
                cheatThumbKey_ = key;
                if (cheatThumb_.id)
                    UnloadTexture(cheatThumb_);
                cheatThumb_ = Texture2D{};
                cheatThumbPages_ = 0;
                const std::vector<const gt4cheats::Code *> codes = tickedCodes(cs);
                if (!codes.empty())
                {
                    std::vector<Image> pages = renderCheatSheet(codes, cs.letter, cheatInk_);
                    cheatThumbPages_ = static_cast<int>(pages.size());
                    if (!pages.empty() && pages[0].data)
                    {
                        const int tw = 640; // 236 dp at up to 2.7 px per dp; smaller windows draw it scaled down (mipmaps)
                        const int th = static_cast<int>(std::lround(tw * pages[0].height / static_cast<float>(pages[0].width)));
                        ImageResize(&pages[0], tw, th);
                        cheatThumb_ = LoadTextureFromImage(pages[0]);
                        GenTextureMipmaps(&cheatThumb_);
                        SetTextureFilter(cheatThumb_, TEXTURE_FILTER_TRILINEAR);
                    }
                    for (Image &p : pages)
                        UnloadImage(p);
                }
            }
        }
        if (cheatJob_ == CheatJob::None)
            return;
        (void)widthPx;
        (void)heightPx;
        const CheatJob job = cheatJob_;
        cheatJob_ = CheatJob::None;
        const ui::Theme &t = ui::theme();
        const CheatSettings ce = cheatsInEffect(cs, gt4_);
        const fs::path dir = cheatOutDir();
        if (dir.empty())
            return; // the test mode without --cheat-files: nothing is written
        std::error_code ec;
        if (job == CheatJob::Card || job == CheatJob::Pictures)
        {
            std::vector<const gt4cheats::Code *> codes;
            if (job == CheatJob::Card)
                codes.push_back(&gt4cheats::kCodes[static_cast<size_t>(cheatSel_)]);
            else
                codes = tickedCodes(cs);
            fs::create_directories(dir, ec);
            std::vector<std::string> saved;
            for (const gt4cheats::Code *c : codes)
            {
                Image img = renderCheatCard(*c);
                const fs::path p = uniqueFile(dir, cheatCardFileName(*c, ce));
                const bool ok = exportPng(img, p);
                UnloadImage(img);
                if (!ok)
                {
                    setStatus("Could not save " + pathText(p) + ".", t.bad);
                    report("cheats: FAIL saving " + pathText(p));
                    return;
                }
                saved.push_back(pathText(p.filename()));
                lastCheatFiles_.push_back(p);
                report("cheats: saved card " + pathText(p));
            }
            setStatus((saved.size() == 1 ? "Saved " + saved[0] : strf("Saved %zu cards", saved.size())) + " in " + shortPath(dir, inst_.root) +
                          " (GT4's photo list skips it: its name is not a short 8.3 name).",
                      t.ok);
            return;
        }
        const std::vector<const gt4cheats::Code *> codes = tickedCodes(cs);
        if (codes.empty())
            return;
        std::vector<Image> pages = renderCheatSheet(codes, cs.letter, cheatInk_);
        if (job == CheatJob::Print)
        {
            std::vector<sys::PrintPage> pp;
            for (const Image &img : pages)
                pp.push_back(sys::PrintPage{img.width, img.height, static_cast<const uint8_t *>(img.data)});
            std::string message;
            const bool ok = sys::printPages(GetWindowHandle(), L"GT4 cheat sheet", pp, message);
            setStatus(message, ok ? t.ok : t.muted);
            report("cheats: print: " + message);
        }
        else
        {
            const Paper paper = paperFor(cs.letter);
            std::vector<PdfPage> pdfPages;
            for (const Image &img : pages)
            {
                PdfPage p;
                p.width = img.width;
                p.height = img.height;
                p.widthPt = paper.widthPt;
                p.heightPt = paper.heightPt;
                const uint8_t *d = static_cast<const uint8_t *>(img.data);
                p.rgb.assign(d, d + static_cast<size_t>(img.width) * static_cast<size_t>(img.height) * 3u);
                pdfPages.push_back(std::move(p));
            }
            const std::vector<uint8_t> pdf = makePdf(pdfPages, "GT4 cheat sheet");
            fs::path target;
            if (job == CheatJob::PdfAs)
            {
                static const wchar_t filter[] = L"PDF document (*.pdf)\0*.pdf\0"; // the array ends in the second \0
                target = sys::browseForSaveFile(GetWindowHandle(), L"Save the GT4 cheat sheet", std::wstring(filter, std::size(filter)),
                                                dir / pathFrom(cheatSheetFileName()), L"pdf");
                // Save as... takes any folder and name. Inside the Photos folder (GT4's USB stick, every folder below its
                // root) a short 8.3 name would be listed by GT4: refused, as the page promises (1.4 review).
                if (!target.empty())
                {
                    std::error_code e1, e2;
                    const fs::path folder = fs::weakly_canonical(target.parent_path(), e1), root = fs::weakly_canonical(usbRoot(), e2);
                    const std::string fl = lower(pathText(folder)), rl = lower(pathText(root));
                    const bool inside = !e1 && !e2 && !rl.empty() && (fl == rl || startsWith(fl, rl + "\\") || startsWith(fl, rl + "/"));
                    if (inside && hasShortNameForm(pathText(target.filename())))
                    {
                        setStatus("Not saved: GT4 would list " + pathText(target.filename()) +
                                      " on its USB stick. Pick a longer name (with a space), or another folder.",
                                  t.warn);
                        report("cheats: refused " + pathText(target));
                        target.clear();
                    }
                }
            }
            else
            {
                fs::create_directories(dir, ec);
                target = uniqueFile(dir, cheatSheetFileName());
            }
            if (!target.empty())
            {
                std::ofstream out(target, std::ios::binary | std::ios::trunc);
                out.write(reinterpret_cast<const char *>(pdf.data()), static_cast<std::streamsize>(pdf.size()));
                const bool ok = static_cast<bool>(out);
                out.close();
                if (ok)
                {
                    lastCheatFiles_.push_back(target);
                    setStatus(strf("Saved %s (%zu page%s, %s) in %s.", pathText(target.filename()).c_str(), pages.size(), pages.size() == 1 ? "" : "s",
                                   sizeText(pdf.size()).c_str(), shortPath(target.parent_path(), inst_.root).c_str()),
                              t.ok);
                    report(strf("cheats: saved sheet %s: %zu page(s) %s %dx%d, %zu bytes", pathText(target).c_str(), pages.size(), paper.name,
                                pages.empty() ? 0 : pages[0].width, pages.empty() ? 0 : pages[0].height, pdf.size()));
                }
                else
                    setStatus("Could not save " + pathText(target) + ".", t.bad);
            }
        }
        for (Image &p : pages)
            UnloadImage(p);
    }
}
