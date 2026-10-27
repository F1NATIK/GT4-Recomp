// GT4 Recompilation launcher (Classic edition; internal names keep "stock"): the frame, the sidebar and the six pages (the
// Cheats page is in gt4stock_cheatspage.cpp).

#include "gt4stock_app.h"

#include "gt4stock_util.h"

#include <algorithm>
#include <cmath>

namespace gt4stock
{
    using ui::Align;
    using ui::Icon;
    using ui::Style;
    using ui::Weight;

    struct PadView
    {
        std::array<bool, kPadButtons> pressed{};
        std::array<float, kPadPressures> pressure{};
        float lx = 0, ly = 0, rx = 0, ry = 0;
        bool live = false;
    };

    namespace
    {
        constexpr float kSidebarW = 232.0f;
        constexpr float kTop = 104.0f;
        constexpr float kGap = 16.0f;

        Rectangle R(float x, float y, float w, float h) { return Rectangle{x, y, w, h}; }

        Icon kindIcon(DeviceKind k)
        {
            switch (k)
            {
            case DeviceKind::Wheel: return Icon::Wheel;
            case DeviceKind::Pedals: return Icon::Wheel;
            default: return Icon::Gamepad;
            }
        }

        float wheelAxisValue(const DeviceInfo *d, const std::string &axis)
        {
            if (!d)
                return 0.0f;
            for (int a = 0; a < kWheelAxes; ++a)
                if (axis == kWheelAxisKeys[a])
                    return static_cast<float>(d->axes[static_cast<size_t>(a)]);
            return 0.0f;
        }

        int axisIndex(const std::string &axis)
        {
            for (int a = 0; a < kWheelAxes; ++a)
                if (axis == kWheelAxisKeys[a])
                    return a;
            return -1;
        }

        // "2026-10-04 18:22" (the settings file's stamp) -> "4 Oct 2026 18:22", as the other dates are shown.
        std::string friendlyStamp(const std::string &stamp)
        {
            static const char *const months[12] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
            long long y = 0, mo = 0, d = 0, h = 0, mi = 0;
            if (stamp.size() != 16 || stamp[4] != '-' || stamp[7] != '-' || stamp[10] != ' ' || stamp[13] != ':' || !parseInt(stamp.substr(0, 4), y) ||
                !parseInt(stamp.substr(5, 2), mo) || !parseInt(stamp.substr(8, 2), d) || !parseInt(stamp.substr(11, 2), h) ||
                !parseInt(stamp.substr(14, 2), mi) || mo < 1 || mo > 12 || d < 1 || d > 31)
                return stamp;
            return strf("%lld %s %lld %02lld:%02lld", d, months[mo - 1], y, h, mi);
        }

        // Pedal travel 0..1 as the adapter computes it (gt4_wheel_di.cpp diPedalTravel):
        // calibrated, or self-calibrating from where the pedal rests (here: the far
        // end from 32767, the usual rest of a DirectInput pedal axis).
        float pedalTravel(float raw, const PedalSettings &p, float deadzone)
        {
            float released = 32767.0f, full = -32768.0f;
            if (p.calibrated)
            {
                released = static_cast<float>(p.released);
                full = static_cast<float>(p.full);
            }
            const float t = std::clamp((raw - released) / (full - released), 0.0f, 1.0f);
            return t <= deadzone ? 0.0f : (t - deadzone) / (1.0f - deadzone);
        }
    }

    // ------------------------------------------------------------------ frame
    void App::frame(int widthPx, int heightPx, const ui::Input &input)
    {
        ui_.begin(widthPx, heightPx, input);
        const ui::Theme &t = ui::theme();
        ui_.fill(R(0, 0, ui_.width, ui_.height), t.bg);
#if defined(GT4L_EDITION_FULL)
        // The Full edition: its sidebar and pages (gt4full_*.cpp); the shared pages draw as in the Classic edition.
        ui_.blockInput(modal_ != Modal::None || previewFullscreen_);
        drawFullSidebar();
        switch (page_)
        {
        case Page::Play: pagePlay(); break;
        case Page::Display: pageDisplayFull(); break;
        case Page::Controllers: pageControllers(); break;
        case Page::Saves:
            if (savesProgress_ && !(savesView_ == 1 && detailsOpen_))
                pageProgress();
            else
                pageSaves();
            break;
        case Page::Cheats: pageCheats(); break;
        case Page::About: pageAbout(); break;
        case Page::Multiscreen: pageMultiscreen(); break;
        case Page::Garage: pageGarage(); break;
        case Page::Network: pageNetwork(); break;
        case Page::Sound: pageSound(); break;
        case Page::Roadmap: pageRoadmap(); break;
        case Page::RaceLab: pageRaceLab(); break;
        case Page::RaceHud: pageRaceHud(); break;
        case Page::Telemetry: pageTelemetry(); break;
        }
        drawStatusLine();
        ui_.blockInput(false);
        if (modal_ != Modal::None)
            drawModal();
        if (previewFullscreen_)
            drawPreviewFullscreen();
        ui_.end();
#else
        ui_.blockInput(modal_ != Modal::None);
        drawSidebar();
        switch (page_)
        {
        case Page::Play: pagePlay(); break;
        case Page::Display: pageDisplay(); break;
        case Page::Controllers: pageControllers(); break;
        case Page::Saves: pageSaves(); break;
        case Page::Cheats: pageCheats(); break;
        case Page::About: pageAbout(); break;
        }
        drawStatusLine();
        ui_.blockInput(false);
        if (modal_ != Modal::None)
            drawModal();
        ui_.end();
#endif
    }

    void App::drawSidebar()
    {
        const ui::Theme &t = ui::theme();
        const float H = ui_.height;
        ui_.fill(R(0, 0, kSidebarW, H), t.sidebar);
        ui_.fill(R(kSidebarW - 1, 0, 1, H), t.border);
        // The mark: a "4" on scanlines.
        const Rectangle tile = R(24, 26, 46, 46);
        ui_.round(tile, 11, t.red);
        for (int i = 0; i < 9; ++i)
            ui_.fill(R(tile.x + 4, tile.y + 6 + i * 4.2f, tile.width - 8, 1.4f), ui::alpha(BLACK, 0.18f));
        ui_.text("4", tile.x + tile.width * 0.5f, tile.y + 3, 32, WHITE, Weight::Bold, Align::Center);
        // The name on two lines ("GT4" / "Recompilation"), then the edition.
        {
            const float room = kSidebarW - 84 - 14;
            float size = 18.0f;
            while (size > 13.0f && ui_.textWidth("Recompilation", size, Weight::Bold) > room)
                size -= 0.5f;
            ui_.text("GT4", 84, 18, size, t.text, Weight::Bold);
            ui_.text("Recompilation", 84, 18 + size * 1.25f, size, t.text, Weight::Bold);
            ui_.chip(84, 18 + size * 2.6f, upper(kEdition), ui::alpha(t.red, 0.22f), ui::mixColor(t.redHi, t.text, 0.35f), 10.5f);
        }

        struct Item
        {
            Page page;
            Icon icon;
            const char *label;
        };
        const Item items[] = {{Page::Play, Icon::Play, "Play"},
                              {Page::Display, Icon::Display, "Display"},
                              {Page::Controllers, Icon::Gamepad, "Controllers"},
                              {Page::Saves, Icon::Card, "Saves"},
                              {Page::Cheats, Icon::Sparkle, "Cheats"},
                              {Page::About, Icon::Info, "About"}};
        float y = 112;
        for (const Item &it : items)
        {
            const Rectangle r = R(12, y, kSidebarW - 24, 46);
            const bool sel = page_ == it.page;
            const bool hot = ui_.hovered(r);
            if (sel)
            {
                ui_.round(r, 10, t.panel2);
                ui_.round(R(12, y + 11, 4, 24), 2, t.accent);
            }
            else if (hot)
                ui_.round(r, 10, ui::alpha(t.panel2, 0.6f));
            ui_.icon(it.icon, R(32, y + 12, 22, 22), sel ? t.text : t.muted);
            const float lw = ui_.text(it.label, 66, y + 12, 15.5f, sel ? t.text : t.muted, sel ? Weight::Semibold : Weight::Regular);
            if (it.page == Page::Cheats) // new in 1.4
                ui_.chip(66 + lw + 10, y + 14, "NEW", ui::mixColor(sel ? t.panel2 : t.sidebar, t.accent, 0.2f), t.accent, 9.5f);
            if (ui_.clicked(r))
            {
                page_ = it.page;
                if (page_ == Page::Cheats)
                    readCheatSave(true);
                if (page_ == Page::Saves)
                {
                    savesView_ = 0; // back to the cards from Save details
                    detailsOpen_ = false;
                    refreshCards();
                }
                if (page_ == Page::About)
                    aboutView_ = 0;
            }
            y += 50;
        }

        // At the bottom: are the pieces there?
        float by = H - 142;
        ui_.fill(R(20, by - 14, kSidebarW - 40, 1), t.border);
        auto row = [&](bool ok, const std::string &label) {
            ui_.statusDot(30, by + 9, ok ? t.ok : t.bad);
            ui_.text(label, 44, by, 13, ok ? t.muted : t.text);
            by += 24;
        };
        row(inst_.exeFound, inst_.exeFound ? "GT4 program found" : "GT4 program missing");
        row(inst_.coreFound, inst_.coreFound ? "Game files found" : "game\\CORE.elf missing");
        row(disc_.exists, disc_.exists ? "Disc image found" : "No disc image");
        row(settingsFileExisted_ || hidden_, dirty_ ? "Saving settings..." : "Settings saved");
        ui_.text(std::string("v") + kVersion, 20, H - 30, 12, t.faint);
    }

    void App::drawHeader(const std::string &title, const std::string &subtitle)
    {
        const ui::Theme &t = ui::theme();
        const float x0 = kSidebarW + 32;
        ui_.text(title, x0, 28, 28, t.text, Weight::Bold);
        ui_.text(ui_.fit(subtitle, ui_.width - x0 - 32, 14.5f), x0, 66, 14.5f, t.muted);
    }

    void App::drawStatusLine()
    {
        const ui::Theme &t = ui::theme();
        const float x0 = kSidebarW + 32, y = ui_.height - 32;
        if (!status_.empty())
            ui_.text(ui_.fit(status_, ui_.width - x0 - 300, 13), x0, y, 13, statusColor_);
        else
        {
            const std::string where = "Settings: " + shortPath(inst_.settingsFile, inst_.root) + "  (GT4.exe reads it at start)";
            ui_.text(ui_.fit(where, ui_.width - x0 - 300, 12.5f, Weight::Regular, true), x0, y, 12.5f, t.faint);
        }
        float rx = ui_.width - 32;
        auto badge = [&](const std::string &s, Color bg, Color fg) {
            const float w = ui_.textWidth(s, 12, Weight::Semibold) + 18;
            rx -= w;
            ui_.chip(rx, y - 3, s, bg, fg, 12);
            rx -= 8;
        };
        if (hidden_)
            badge("Test mode: hidden window", ui::alpha(t.warn, 0.18f), t.warn);
        if (devices_.demo())
            badge("Demo devices", ui::alpha(t.accent, 0.18f), t.accent);
    }

    // ------------------------------------------------------------------ Play
    void App::pagePlay()
    {
        const ui::Theme &t = ui::theme();
        // 1.4: with a cheat switch on, the game is no longer only the original plus the scanlines: say so (only the switches
        // the GT4 program here acts on).
        const CheatSettings cheatsOn = cheatsInEffect(st_.cheats, gt4_);
#if defined(GT4L_EDITION_FULL)
        drawHeader("Play", cheatsOn.any() ? "Gran Turismo 4 with your picture and screen choices, and the cheat switches you turned on."
                                          : "Gran Turismo 4 with your picture and screen choices: 480p, full colour and 16:9 unless you change them.");
#else
        drawHeader("Play", cheatsOn.any() ? "Gran Turismo 4 as it was on a PS2 and a TV, with the scanline filter and the cheat switches you turned on."
                                          : "Gran Turismo 4 as it was on a PS2 and a TV: the original game, with only the scanline filter added.");
#endif
        const float x0 = kSidebarW + 32, x1 = ui_.width - 32, cw = x1 - x0, bottom = ui_.height - 48;

        // Hero: GT4's title screen behind, darkened towards the text.
        const Rectangle hero = R(x0, kTop, cw, 288);
        {
            const Rectangle hp = ui_.px(hero);
            const Texture2D &pic = pictures_[2];
            const float srcAspect = static_cast<float>(pictureW_[2]) / std::max(1, pictureH_[2]);
            const float dstAspect = hp.width / std::max(1.0f, hp.height);
            Rectangle src{0, 0, static_cast<float>(pictureW_[2]), static_cast<float>(pictureH_[2])};
            if (dstAspect > srcAspect)
            {
                src.height = src.width / dstAspect;
                src.y = (pictureH_[2] - src.height) * 0.42f;
            }
            else
            {
                src.width = src.height * dstAspect;
                src.x = (pictureW_[2] - src.width) * 0.5f;
            }
            DrawTexturePro(pic, src, hp, Vector2{0, 0}, 0, Color{255, 255, 255, 255});
            ui_.gradientH(R(hero.x, hero.y, hero.width * 0.72f, hero.height), ui::alpha(t.bg, 0.97f), ui::alpha(t.bg, 0.30f));
            ui_.gradientH(R(hero.x + hero.width * 0.72f, hero.y, hero.width * 0.28f, hero.height), ui::alpha(t.bg, 0.30f), ui::alpha(t.bg, 0.05f));
            // the picture's own scanline hint
            for (float yy = hero.y; yy < hero.y + hero.height; yy += 3.0f)
                ui_.fill(R(hero.x, yy, hero.width, 1.0f), ui::alpha(BLACK, 0.10f));
            ui_.roundLine(hero, 2, 1, t.border);
        }
        const float hx = hero.x + 36;
        ui_.text("GRAN TURISMO 4", hx, hero.y + 34, 40, t.text, Weight::Bold);
#if defined(GT4L_EDITION_FULL)
        ui_.text("The Real Driving Simulator", hx, hero.y + 86, 16, ui::mixColor(t.text, t.muted, 0.4f)); // (no edition name: developer 2026-10-20)
#else
        ui_.text(std::string("The Real Driving Simulator  \xc2\xb7  ") + kEdition, hx, hero.y + 86, 16, ui::mixColor(t.text, t.muted, 0.4f));
#endif
#if defined(GT4L_EDITION_FULL)
        // A release without GT4.exe yet (its build-kit next to the launcher): Build GT4 in place of the chips and Play, until
        // it is built.
        const bool buildFirst = !buildKit().empty() && (!inst_.exeFound || !inst_.coreFound || build_.running || build_.failed);
        if (!buildFirst)
#endif
        {
            float cx = hx;
            const MonitorInfo *m = selectedMonitor();
            int ww = 0, wh = 0;
            const scan::Placement pl = gamePlacement(m, &ww, &wh);
#if defined(GT4L_EDITION_FULL)
            // The Full edition: its video mode, colour and shape, and the renderer.
            (void)pl;
            const FullSettings &fs = st_.full;
            cx += ui_.chip(cx, hero.y + 122, fs.gameVideoModes ? std::string("GT4's own video modes") : strf("480p \xc2\xb7 %d-bit", fs.colour == 16 ? 16 : 32),
                           ui::alpha(t.panel3, 0.9f), t.text) + 8;
            cx += ui_.chip(cx, hero.y + 122, std::string(fs.aspect == FullAspect::Normal || fs.aspect == FullAspect::SquarePixels ? "4:3" : "16:9") + " \xc2\xb7 " +
                                                 fullScalingLabel(fullScalingBuilt(fs.scaling) ? fs.scaling : FullScaling::Bilinear),
                           ui::alpha(t.panel3, 0.9f), t.text) + 8;
            cx += ui_.chip(cx, hero.y + 122, !st_.fullscreen ? strf("Window %d\xc3\x97%d", ww, wh) : fs.span ? std::string("Across all monitors") : std::string("Full screen"),
                           ui::alpha(t.panel3, 0.9f), t.text) + 8;
            if (full_.read && full_.vulkan && !preparedForThisExe())
            {
                // Vulkan's shaders not prepared for this GT4 program: one click to the Smoothness tab.
                const std::string label = preparing_ ? std::string("Preparing shaders...") : std::string("Vulkan: prepare shaders");
                const float w = ui_.textWidth(label, 12.5f, Weight::Semibold) + 18;
                if (cx + w < hero.x + hero.width - 16)
                {
                    ui_.chip(cx, hero.y + 122, label, ui::mixColor(t.bg, t.accent, 0.22f), t.accent);
                    if (ui_.clicked(R(cx, hero.y + 122, w, 12.5f * 1.33f + 8)))
                    {
                        page_ = Page::Display;
                        st_.full.displayTab = 3;
                    }
                    cx += w + 8;
                }
            }
#else
            cx += ui_.chip(cx, hero.y + 122, "SCUS-97328 \xc2\xb7 USA", ui::alpha(t.panel3, 0.9f), t.text) + 8;
            cx += ui_.chip(cx, hero.y + 122, st_.linesOn ? strf("Scanlines %d", st_.strength) : std::string("Scanlines off at start"),
                           ui::alpha(t.panel3, 0.9f), t.text) + 8;
            const std::string screen = st_.fullscreen ? strf("Full screen \xc2\xb7 %d\xc3\x97%d picture", static_cast<int>(std::lround(pl.w)), static_cast<int>(std::lround(pl.h)))
                                                      : strf("Window %d\xc3\x97%d", ww, wh);
            cx += ui_.chip(cx, hero.y + 122, screen, ui::alpha(t.panel3, 0.9f), t.text) + 8;
#endif
            // 1.4: a run that is not GT4 as shipped is never silent: any cheat switch that is on.
            const std::vector<std::string> on = cheatSwitchesOn(cheatsOn);
            if (!on.empty())
            {
                std::string label = "Cheats: " + join(on, ", ");
                if (cx + ui_.textWidth(label, 12.5f, Weight::Semibold) + 18 > hero.x + hero.width - 16)
                    label = strf("Cheats on: %zu switch%s (Cheats page)", on.size(), on.size() == 1 ? "" : "es");
                const float w = ui_.textWidth(label, 12.5f, Weight::Semibold) + 18;
                const Rectangle cr = R(cx, hero.y + 122, w, 12.5f * 1.33f + 8);
                ui_.chip(cx, hero.y + 122, label, ui::mixColor(t.bg, t.warn, 0.22f), ui::mixColor(t.warn, WHITE, 0.25f));
                if (ui_.clicked(cr))
                {
                    page_ = Page::Cheats;
                    readCheatSave(true);
                }
            }
        }
        const Rectangle playR = R(hx, hero.y + 176, 248, 66);
#if defined(GT4L_EDITION_FULL)
        if (buildFirst && !gameRunning_)
            drawBuild(hx, hero.y + 124, std::min(640.0f, hero.width - 72));
        else
#endif
        if (!gameRunning_)
        {
            const bool ready = inst_.exeFound && inst_.coreFound && disc_.exists && !otherGameRunning_;
            if (ui_.iconButton("play", playR, Icon::Play, "PLAY", Style::Play, ready, 22))
                play();
#if defined(GT4L_EDITION_FULL)
            std::string why = "Starts Gran Turismo 4 with these settings."; // (no TV scanlines in the Full edition)
#else
            std::string why = "Starts Gran Turismo 4 with these settings.  F1 in the game: scanlines on and off.";
#endif
            if (cheatsOn.card)
                why += "  " + st_.cheats.key + ": the cheat card.";
            if (!inst_.exeFound)
                why = "The GT4 program was not found next to the launcher (see About).";
            else if (!inst_.coreFound)
                why = "game\\CORE.elf is missing: the game files are not next to the launcher.";
            else if (!disc_.exists)
                why = "Choose the Gran Turismo 4 disc image below.";
            else if (otherGameRunning_)
                why = "Another GT4 is already running.";
#if defined(GT4L_EDITION_FULL)
            const bool classicExe = ready && full_.read && full_.classic;
            if (classicExe)
                why = "This GT4 program does not read these settings: use the GT4.exe that came with this launcher.";
            else if (ready && full_.read && !full_.vulkan)
                why = "This GT4 program was built without Vulkan, GT4's main renderer here.";
            ui_.text(why, hx, hero.y + 252, 13, ready && !classicExe ? t.muted : t.warn);
#else
            ui_.text(why, hx, hero.y + 252, 13, ready ? t.muted : t.warn);
#endif
        }
        else
        {
            const int minutes = static_cast<int>((sys::tickMs() - game_.startMs) / 60000u);
            ui_.round(R(playR.x, playR.y, 300, playR.height), 12, ui::alpha(t.ok, 0.16f));
            ui_.statusDot(playR.x + 24, playR.y + playR.height * 0.5f, t.ok);
            ui_.text("GT4 is running", playR.x + 42, playR.y + 10, 20, t.text, Weight::Semibold);
            ui_.text(minutes < 1 ? std::string("started just now") : strf("for %d min", minutes), playR.x + 42, playR.y + 38, 13.5f, t.muted);
            if (ui_.iconButton("stop", R(playR.x + 316, playR.y + 13, game_.closeSent ? 150 : 128, 40), Icon::Stop,
                               game_.closeSent ? "Force stop" : "Stop GT4", Style::Danger, true, 14))
                stopGame();
            ui_.text("Stop = the PS2's power button: GT4 closes cleanly. Your progress is what you saved in the game.", hx, hero.y + 252, 13,
                     t.muted);
        }

        // Three cards: the disc, the settings, the last session.
        const float cy = hero.y + hero.height + 20, ch = bottom - cy;
        const float w1 = std::floor((cw - 2 * kGap) * 0.35f), w2 = std::floor((cw - 2 * kGap) * 0.38f), w3 = cw - 2 * kGap - w1 - w2;
        // Disc
        {
            const Rectangle c = R(x0, cy, w1, ch);
            ui_.card(c, "Disc image");
            float y = c.y + 52;
            const bool good = disc_.exists && disc_.sizeMatches && disc_.volumeMatches;
            ui_.icon(good ? Icon::Check : disc_.exists ? Icon::Warn : Icon::Cross, R(c.x + 20, y, 20, 20), good ? t.ok : disc_.exists ? t.warn : t.bad);
            ui_.text(good ? "Gran Turismo 4 (USA)" : disc_.exists ? "Found, but not the expected image" : "Not found", c.x + 48, y + 1, 15,
                     t.text, Weight::Semibold);
            y += 30;
            const std::string name = disc_.path.empty() ? std::string("(none chosen)") : pathText(pathFrom(disc_.path).filename());
            ui_.text(ui_.fit(name, c.width - 40, 13.5f, Weight::Regular, true), c.x + 20, y, 13.5f, t.text);
            y += 22;
            const std::string folder = disc_.path.empty() ? std::string() : pathText(pathFrom(disc_.path).parent_path());
            ui_.text(ui_.fit(folder, c.width - 40, 12.5f, Weight::Regular, true), c.x + 20, y, 12.5f, t.faint);
            y += 22;
            if (disc_.exists)
                ui_.text(strf("%s bytes \xc2\xb7 volume %s", groupedNumber(disc_.bytes).c_str(), disc_.volume.empty() ? "?" : disc_.volume.c_str()), c.x + 20,
                         y, 12.5f, good ? t.muted : t.warn);
            y += 30;
            const float bw = (c.width - 40 - 10) * 0.5f;
            if (ui_.button("browse-disc", R(c.x + 20, y, bw, 34), "Browse...", Style::Secondary, !busy()))
            {
                const fs::path p = sys::browseForFile(GetWindowHandle(), L"The Gran Turismo 4 disc image",
                                                      std::wstring(L"Disc images (*.iso;*.bin)\0*.iso;*.bin\0All files\0*.*\0\0", 52),
                                                      pathFrom(st_.disc));
                if (!p.empty())
                {
                    st_.disc = pathText(p);
                    checkDisc();
                    loadShirtDesign();
                    startNames(); // Save details' names, from the new disc
                    markDirty();
                }
            }
            if (ui_.button("open-disc", R(c.x + 30 + bw, y, bw, 34), "Open folder", Style::Secondary, disc_.exists))
                sys::openInExplorer(pathFrom(disc_.path).parent_path());
            y += 50;
            ui_.fill(R(c.x + 20, y, c.width - 40, 1), t.border);
            y += 12;
            auto fileRow = [&](bool ok, const std::string &label, const std::string &value) {
                ui_.statusDot(c.x + 26, y + 9, ok ? t.ok : t.bad);
                ui_.text(label, c.x + 40, y, 13, t.muted);
                ui_.text(ui_.fit(value, c.width - 150, 13, Weight::Regular, true), c.x + c.width - 20, y, 13, t.text, Weight::Regular, Align::Right);
                y += 24;
            };
            fileRow(inst_.exeFound, "Program", inst_.exeFound ? pathText(inst_.exe.filename()) : std::string("not found"));
            fileRow(inst_.coreFound, "Game files", inst_.coreFound ? "game\\CORE.elf, " + sizeText(inst_.coreBytes) : std::string("missing"));
        }
        // Settings summary
        {
            const Rectangle c = R(x0 + w1 + kGap, cy, w2, ch);
            ui_.card(c, "Your settings");
            float y = c.y + 52;
            const MonitorInfo *m = selectedMonitor();
            int ww = 0, wh = 0;
#if defined(GT4L_EDITION_FULL)
            const scan::Placement pl = fullPlacement(m, &ww, &wh); // the picture's shape (16:9 or 4:3)
#else
            const scan::Placement pl = gamePlacement(m, &ww, &wh);
#endif
#if defined(GT4L_EDITION_FULL)
            const std::string rows = "picture fills the screen"; // (no TV lines in the Full edition)
#else
            const std::string rows = pl.integer ? strf("%d rows per line", pl.integer) : strf("%.2f rows per line", pl.rowsPerLine);
#endif
            // Six rows (the Full edition: seven, with Sound): 52 dp each when the card has the room, closer on a short window.
#if defined(GT4L_EDITION_FULL)
            const float rowH = std::clamp(std::floor((c.height - 56) / 7.0f), 40.0f, 52.0f);
#else
            const float rowH = std::clamp(std::floor((c.height - 56) / 6.0f), 42.0f, 52.0f);
#endif
            auto row = [&](Icon ic, const std::string &label, const std::string &value, Page go, int tab, Color valueColor) {
                const Rectangle r = R(c.x + 10, y - 6, c.width - 20, rowH - 2);
                if (ui_.rowButton("sum-" + label, r, false))
                {
                    page_ = go;
                    controllersTab_ = tab;
                }
                ui_.icon(ic, R(c.x + 20, y + 2, 20, 20), t.muted);
                ui_.text(label, c.x + 52, y - 2, 12.5f, t.faint);
                ui_.text(ui_.fit(value, c.width - 72, 14), c.x + 52, y + 16, 14, valueColor);
                y += rowH;
            };
#if defined(GT4L_EDITION_FULL)
            {
                const FullSettings &fs = st_.full;
                row(Icon::Display, "Picture",
                    (fs.gameVideoModes ? std::string("GT4's own video modes") : strf("480p, %d-bit colour", fs.colour == 16 ? 16 : 32)) + " \xc2\xb7 " +
                        fullScalingLabel(fullScalingBuilt(fs.scaling) ? fs.scaling : FullScaling::Bilinear) + " \xc2\xb7 " +
                        (fs.aspect == FullAspect::Normal ? "4:3" : fs.aspect == FullAspect::SquarePixels ? "square pixels" : "16:9"),
                    Page::Display, 0, t.text);
            }
#else
            row(Icon::Display, "Picture",
                (st_.linesOn ? strf("Scanlines on, strength %d \xc2\xb7 fit %s", st_.strength, lower(fitLabel(st_.fit)).c_str())
                             : strf("Scanlines off at start (F1) \xc2\xb7 strength %d", st_.strength)) +
                    strf(" \xc2\xb7 %s \xc2\xb7 interlacing %s", lower(pictureFilterLabel(st_.filter)).c_str(), lower(deinterlaceLabel(st_.deinterlace)).c_str()),
                Page::Display, 0, t.text);
#endif
#if defined(GT4L_EDITION_FULL)
            if (st_.fullscreen && st_.full.span)
                row(Icon::Display, "Screen", strf("Across all monitors (%zu) \xc2\xb7 one borderless window", monitors_.size()), Page::Display, 0, t.text);
            else
#endif
            row(Icon::Display, "Screen",
                st_.fullscreen ? strf("Full screen on %s \xc2\xb7 %d\xc3\x97%d, %s", m ? m->name.c_str() : "the main display", static_cast<int>(std::lround(pl.w)),
                                      static_cast<int>(std::lround(pl.h)), rows.c_str())
                               : strf("Window %d\xc3\x97%d \xc2\xb7 picture %d\xc3\x97%d, %s", ww, wh, static_cast<int>(std::lround(pl.w)),
                                      static_cast<int>(std::lround(pl.h)), rows.c_str()),
                Page::Display, 0, t.text);
            std::string pad = "Automatic: the first game device";
            for (const DeviceInfo &d : devices_.list())
                if (!st_.pad.sourceGuid.empty() && d.glfwGuid == st_.pad.sourceGuid)
                    pad = d.name;
            row(Icon::Gamepad, "Controller (player 1)", pad + (st_.vibration ? " \xc2\xb7 vibration on" : " \xc2\xb7 vibration off"), Page::Controllers, 0, t.text);
            const DeviceInfo *w = wheelDevice();
            const std::string wheel = !st_.wheel.enabled ? std::string("No wheel")
                                                          : strf("%s \xc2\xb7 %d\xc2\xb0 \xc2\xb7 force feedback %s",
                                                                 w ? w->name.c_str() : (st_.wheel.deviceName.empty() ? "a wheel (not connected)" : st_.wheel.deviceName.c_str()),
                                                                 st_.wheel.rotation, st_.wheel.ffb ? strf("%d%%", static_cast<int>(std::lround(st_.wheel.ffbGain * 100))).c_str() : "off");
            row(Icon::Wheel, "Wheel and pedals", wheel, Page::Controllers, 1, t.text);
            const CardInfo &c1 = cards_[0];
            const CardFileInfo &f1 = cardFiles_[0];
            std::string card;
            if (cardIsFile(1))
                card = pathText(cardFile(1).filename()) + (f1.gt4Save ? ": GT4 save, changed " + f1.gt4.when : f1.card ? std::string(": no GT4 save yet") : std::string(": made at GT4's first use"));
            else
                card = c1.gt4Save ? "Folder card: GT4 save, changed " + c1.gt4.when : (c1.exists ? std::string("Folder card: no GT4 save yet") : std::string("Folder card (made at the first save)"));
            row(Icon::Card, "Memory card (slot 1)", card, Page::Saves, 0, t.text);
            const bool cameraMissing = st_.eyetoy == EyeToy::Webcam && (cameras_.list().listed || !cameras_.list().error.empty()) && !chosenCamera();
            row(Icon::Camera, "EyeToy camera", cameraSummary(), Page::Controllers, 2, cameraMissing ? t.warn : t.text);
#if defined(GT4L_EDITION_FULL)
            // The Sound page's engine bass (a GT4 program without it plays GT4's own engine sound).
            // Sound packs (Sound > Sound packs) follow when one is chosen and this GT4 program reads them.
            {
                std::string sound = full_.read && !full_.engineBass ? std::string("GT4's own engine sound (this GT4 program has no engine bass)")
                                                                    : std::string("Engine bass: ") + fullEngineBassLabel(std::clamp(st_.full.engineBass, 0, 3));
                const FullSettings &sf = st_.full;
                if ((!full_.read || full_.soundPacks) && sf.soundMode == FullSoundMode::EveryCar && !sf.soundPackAll.empty())
                    sound += ", sound pack " + pathText(pathFrom(sf.soundPackAll).filename());
                else if ((!full_.read || full_.soundPacks) && sf.soundMode == FullSoundMode::PerCar && (!sf.soundCars.empty() || !sf.soundPackAll.empty()))
                    sound += strf(", sound packs for %zu car%s", sf.soundCars.size(), sf.soundCars.size() == 1 ? "" : "s");
                row(Icon::Speaker, "Sound", sound, Page::Sound, 0, t.text);
            }
#endif
        }
        // Last session
        {
            const Rectangle c = R(x0 + w1 + w2 + 2 * kGap, cy, w3, ch);
            ui_.card(c, "Last session");
            float y = c.y + 52;
            if (st_.lastPlayed.empty())
                ui_.text("Not played from here yet.", c.x + 20, y, 14, t.muted);
            else
            {
                ui_.text(friendlyStamp(st_.lastPlayed), c.x + 20, y, 15, t.text, Weight::Semibold);
                y += 24;
                std::string how = st_.lastExit == 0 ? "Closed normally" : strf("Ended with code %d", st_.lastExit);
                if (st_.lastMinutes >= 0)
                    how += strf(" \xc2\xb7 %d min", st_.lastMinutes);
                ui_.text(how, c.x + 20, y, 13, st_.lastExit == 0 ? t.muted : t.warn);
            }
            y += 30;
            std::error_code ec;
            const bool haveLog = fs::exists(logFile(), ec);
            if (ui_.button("open-log", R(c.x + 20, y, c.width - 40, 32), "Open the log", Style::Secondary, haveLog))
                sys::openInExplorer(logFile());
            y += 48;
            ui_.text("In the game", c.x + 20, y, 13, t.faint, Weight::Semibold);
            y += 22;
#if !defined(GT4L_EDITION_FULL)
            y += ui_.wrap("F1  scanlines on and off", c.x + 20, y, c.width - 40, 13, t.muted);
            y += 4;
#endif
            if (cheatsInEffect(st_.cheats, gt4_).card) // 1.4: the in-game cheat card is on (Cheats page)
            {
                y += ui_.wrap(st_.cheats.key + "  the cheat card (the cards you ticked)", c.x + 20, y, c.width - 40, 13, t.muted);
                y += 4;
            }
            y += ui_.wrap("Closing the window = the PS2 power button: a clean quit. Esc leaves full screen", c.x + 20, y, c.width - 40, 13, t.muted);
            y += 4;
#if defined(GT4L_EDITION_FULL)
            ui_.wrap("Units and the wheel's own force strength are in GT4's Options", c.x + 20, y, c.width - 40, 13, t.muted);
#else
            ui_.wrap("Widescreen, units and the wheel's own force strength are in GT4's Options", c.x + 20, y, c.width - 40, 13, t.muted);
#endif
        }
    }

    // ------------------------------------------------------------------ Display
    void App::preparePreview()
    {
        const MonitorInfo *m = selectedMonitor();
        int ww = 0, wh = 0;
        gamePlacement(m, &ww, &wh);
        ww = std::clamp(ww, 320, 4096);
        wh = std::clamp(wh, 224, 4096);
        const int pic = st_.preview == "menu" ? 1 : st_.preview == "title" ? 2 : 0;
        const std::string key = strf("%d|%d|%d|%d|%dx%d|%d|%d", pic, st_.strength, static_cast<int>(st_.fit), preview_.ready() ? 1 : 0, ww, wh,
                                     static_cast<int>(st_.filter), static_cast<int>(st_.deinterlace));
        if (key == previewKey_)
            return;
        previewKey_ = key;
        // The pictures are woven frames held still: Off and Adaptive show them as they are (nothing moves), Blend
        // blends their lines, Bob shows one field with the lines between filled in.
        const Texture2D &tex = st_.deinterlace == Deinterlace::Bob && picturesBob_[pic].id ? picturesBob_[pic] : pictures_[pic];
        const bool blend = st_.deinterlace == Deinterlace::Blend;
        const int filter = scan::filterMode(st_.filter);
        preview_.renderWindow(tex, pictureW_[pic], pictureH_[pic], ww, wh, st_.fit, st_.strength / 100.0f, true, previewOn_, filter, blend);
        preview_.renderWindow(tex, pictureW_[pic], pictureH_[pic], ww, wh, st_.fit, st_.strength / 100.0f, false, previewOff_, filter, blend);
        SetTextureFilter(previewOn_.texture, TEXTURE_FILTER_POINT);
        SetTextureFilter(previewOff_.texture, TEXTURE_FILTER_POINT);
    }

    void App::drawPreview(Rectangle r)
    {
        const ui::Theme &t = ui::theme();
        if (previewOn_.id == 0)
            return;
        const MonitorInfo *m = selectedMonitor();
        const scan::Placement pl = gamePlacement(m);
        const Rectangle rp = ui_.px(r);
        const float texW = static_cast<float>(previewOn_.texture.width), texH = static_cast<float>(previewOn_.texture.height);
        // A 1:1 crop around a detailed part of the picture (the race HUD, the menu text).
        const int pic = st_.preview == "menu" ? 1 : st_.preview == "title" ? 2 : 0;
        const float fx = pic == 0 ? 0.30f : 0.45f, fy = pic == 0 ? 0.22f : 0.47f;
        float sx = std::round(pl.x + pl.w * fx - rp.width * 0.5f);
        float sy = std::round(pl.y + pl.h * fy - rp.height * 0.5f);
        sx = std::clamp(sx, 0.0f, std::max(0.0f, texW - rp.width));
        sy = std::clamp(sy, 0.0f, std::max(0.0f, texH - rp.height));
        const float cw = std::min(rp.width, texW), chh = std::min(rp.height, texH);
        // Lines off left of the divider, on to the right.
        const float split = std::round(cw * std::clamp(previewSplit_, 0.0f, 1.0f));
        auto blit = [&](const RenderTexture2D &rt, float from, float to) {
            if (to <= from)
                return;
            // A render target is stored bottom row first: the source is flipped.
            const Rectangle src{sx + from, texH - (sy + chh), to - from, -chh};
            DrawTextureRec(rt.texture, src, Vector2{rp.x + from, rp.y}, WHITE);
        };
        DrawRectangleRec(rp, BLACK);
        blit(previewOff_, 0, split);
        blit(previewOn_, split, cw);
        // Divider and handle.
        const float dx = (rp.x + split) / ui_.s;
        ui_.fill(R(dx - 1, r.y, 2, r.height), ui::alpha(WHITE, 0.85f));
        ui_.circle(dx, r.y + r.height * 0.5f, 13, ui::alpha(WHITE, 0.92f));
        ui_.line(dx - 6, r.y + r.height * 0.5f, dx - 2, r.y + r.height * 0.5f - 4, 1.6f, t.bg);
        ui_.line(dx - 6, r.y + r.height * 0.5f, dx - 2, r.y + r.height * 0.5f + 4, 1.6f, t.bg);
        ui_.line(dx + 6, r.y + r.height * 0.5f, dx + 2, r.y + r.height * 0.5f - 4, 1.6f, t.bg);
        ui_.line(dx + 6, r.y + r.height * 0.5f, dx + 2, r.y + r.height * 0.5f + 4, 1.6f, t.bg);
        ui_.chip(r.x + 10, r.y + 10, "Lines off", ui::alpha(BLACK, 0.65f), t.text, 12);
        const float onW = ui_.textWidth("Lines on " + std::to_string(st_.strength), 12, Weight::Semibold) + 18;
        ui_.chip(r.x + r.width - onW - 10, r.y + 10, "Lines on " + std::to_string(st_.strength), ui::alpha(BLACK, 0.65f), t.text, 12);
        // A 4x zoom of the lines (nearest), bottom right.
        {
            const float zs = std::min(150.0f, r.height * 0.45f);
            const Rectangle zr = R(r.x + r.width - zs - 10, r.y + r.height - zs - 10, zs, zs);
            const Rectangle zp = ui_.px(zr);
            const float srcSize = std::floor(zp.width / 4.0f);
            // A detailed spot: the race HUD's lap digits, the menu's text, the title's logo.
            const float zfx = pic == 0 ? 0.235f : pic == 1 ? 0.22f : 0.52f, zfy = pic == 0 ? 0.135f : pic == 1 ? 0.215f : 0.47f;
            const float zx = std::clamp(std::round(pl.x + pl.w * zfx - srcSize * 0.5f), 0.0f, texW - srcSize);
            const float zy = std::clamp(std::round(pl.y + pl.h * zfy - srcSize * 0.5f), 0.0f, texH - srcSize);
            DrawTexturePro(previewOn_.texture, Rectangle{zx, texH - (zy + srcSize), srcSize, -srcSize}, zp, Vector2{0, 0}, 0, WHITE);
            ui_.roundLine(zr, 2, 1.5f, ui::alpha(WHITE, 0.9f));
            ui_.chip(zr.x + 6, zr.y + 6, "4\xc3\x97 zoom", ui::alpha(BLACK, 0.7f), t.text, 11.5f);
        }
        // Drag to compare.
        const Vector2 mouse = ui_.mouseDp();
        if (ui_.inputOn() && ui_.input().pressed && CheckCollisionPointRec(mouse, r))
            draggingSplit_ = true;
        if (!ui_.input().down)
            draggingSplit_ = false;
        if (draggingSplit_)
            previewSplit_ = std::clamp((mouse.x - r.x) / std::max(1.0f, r.width), 0.0f, 1.0f);
    }

    void App::pageDisplay()
    {
        const ui::Theme &t = ui::theme();
        drawHeader("Display", "The TV look, and where GT4 opens. Changes are saved as you make them.");
        const float x0 = kSidebarW + 32, x1 = ui_.width - 32, cw = x1 - x0, bottom = ui_.height - 48;
        const float lw = std::floor(std::min(480.0f, cw * 0.46f));
        const float rx = x0 + lw + kGap, rw = x1 - rx;
        const MonitorInfo *mon = selectedMonitor();
        int ww = 0, wh = 0;
        const scan::Placement pl = gamePlacement(mon, &ww, &wh);

        // TV scanlines
        {
            const Rectangle c = R(x0, kTop, lw, 332);
            ui_.card(c, "TV scanlines");
            float y = c.y + 54;
            ui_.text("Lines on at start", c.x + 20, y + 1, 15, t.text);
            if (ui_.toggle("lines-on", c.x + c.width - 60, y, st_.linesOn))
                markDirty();
            ui_.text("F1 turns them on and off while you play.", c.x + 20, y + 22, 12.5f, t.faint);
            y += 54;
            ui_.text("Strength", c.x + 20, y, 15, t.text);
            const char *what = st_.strength == 0 ? "no lines" : st_.strength < 40 ? "faint" : st_.strength <= 60 ? "a good TV of the time" : st_.strength < 90 ? "strong" : "darker gaps (studio monitor)";
            ui_.text(strf("%d \xc2\xb7 %s", st_.strength, what), c.x + c.width - 20, y + 1, 14, t.muted, Weight::Regular, Align::Right);
            float v = static_cast<float>(st_.strength);
            const Rectangle sr = R(c.x + 24, y + 26, c.width - 48, 24);
            if (ui_.slider("strength", sr, v, 0, 100, 1))
            {
                st_.strength = static_cast<int>(std::lround(v));
                markDirty();
            }
            for (const auto &[at, label] : {std::pair<float, const char *>{0, "0"}, {50, "50 default"}, {100, "100"}})
            {
                const float tx = sr.x + sr.width * at / 100.0f;
                ui_.fill(R(tx - 0.5f, sr.y + 22, 1, 5), t.faint);
                ui_.text(label, tx, sr.y + 28, 11.5f, t.faint, Weight::Regular, at == 0 ? Align::Left : at == 100 ? Align::Right : Align::Center);
            }
            y += 92;
            ui_.text("Picture fit", c.x + 20, y + 6, 15, t.text);
            int fit = static_cast<int>(st_.fit);
            if (ui_.segmented("fit", R(c.x + c.width - 290, y, 270, 32), {"Auto", "Fill", "Whole rows"}, fit))
            {
                st_.fit = static_cast<Fit>(fit);
                markDirty();
            }
            const char *fitText = st_.fit == Fit::Auto ? "Whole screen rows per TV line when the screen needs it for even lines (thin black borders); otherwise as large as it fits."
                                  : st_.fit == Fit::Fill ? "Always as large as the screen allows. Below 3 rows per line the lines are made weaker so they do not show bands."
                                                         : "Always a whole number of screen rows per TV line: every line the same, smaller picture.";
            ui_.wrap(fitText, c.x + 20, y + 40, c.width - 40, 12.5f, t.muted);
            y += 86;
            ui_.text("Interlace flicker", c.x + 20, y + 2, 14, t.muted);
            float f = static_cast<float>(st_.field);
            if (ui_.slider("field", R(c.x + 170, y, c.width - 270, 24), f, 0, 100, 5))
            {
                st_.field = static_cast<int>(std::lround(f));
                markDirty();
            }
            ui_.text(st_.field == 0 ? std::string("0 \xc2\xb7 off") : std::to_string(st_.field), c.x + c.width - 20, y + 2, 13.5f, t.muted, Weight::Regular, Align::Right);
        }
        // Screen
        {
            const float cy = kTop + 332 + kGap;
            const Rectangle c = R(x0, cy, lw, bottom - cy);
            ui_.card(c, "Screen");
            float y = c.y + 52;
            int mode = st_.fullscreen ? 1 : 0;
            if (ui_.segmented("screen-mode", R(c.x + 20, y, c.width - 40, 34), {"Window", "Full screen"}, mode))
            {
                st_.fullscreen = mode == 1;
                markDirty();
            }
            y += 46;
            ui_.text("Monitor", c.x + 20, y, 12.5f, t.faint, Weight::Semibold);
            y += 20;
            const float rowH = 42;
            const int maxRows = std::max(1, static_cast<int>((c.y + c.height - y - (st_.fullscreen ? 44 : 96)) / rowH));
            int shown = 0;
            for (const MonitorInfo &m : monitors_)
            {
                if (shown++ >= maxRows)
                    break;
                const Rectangle r = R(c.x + 12, y, c.width - 24, rowH - 4);
                const bool sel = mon && mon->index == m.index;
                if (ui_.rowButton("mon-" + std::to_string(m.index), r, sel))
                    pickMonitor(m.index);
                ui_.ring(r.x + 18, r.y + r.height * 0.5f, 6.5f, 8.0f, sel ? t.accent : t.faint);
                if (sel)
                    ui_.circle(r.x + 18, r.y + r.height * 0.5f, 4.0f, t.accent);
                ui_.text(ui_.fit(m.name, r.width - 230, 14.5f, Weight::Semibold), r.x + 36, r.y + 9, 14.5f, t.text, Weight::Semibold);
                const std::string res = strf("%d \xc3\x97 %d \xc2\xb7 %d Hz", m.w, m.h, m.hz);
                float tx = r.x + r.width - 12;
                if (m.primary)
                {
                    const float cwid = ui_.textWidth("main", 11.5f, Weight::Semibold) + 18;
                    tx -= cwid;
                    ui_.chip(tx, r.y + 9, "main", ui::alpha(t.accent, 0.18f), t.accent, 11.5f);
                    tx -= 8;
                }
                ui_.text(res, tx, r.y + 10, 13, t.muted, Weight::Regular, Align::Right);
                y += rowH;
            }
            if (monitors_.empty())
                ui_.text("No monitor information.", c.x + 20, y, 13, t.warn);
            if (!st_.fullscreen)
            {
                const float wy = c.y + c.height - 56;
                ui_.text("Window size", c.x + 20, wy + 7, 14, t.text);
                const auto sizes = windowSizes(mon);
                std::vector<std::string> labels;
                int sel = 0;
                for (size_t i = 0; i < sizes.size(); ++i)
                {
                    const scan::Placement p = scan::fitPicture(static_cast<float>(sizes[i].first), static_cast<float>(sizes[i].second), scan::kLines, 4.0f / 3.0f, st_.fit);
                    labels.push_back(p.integer ? strf("%d \xc3\x97 %d  \xc2\xb7  %d rows per line", sizes[i].first, sizes[i].second, p.integer)
                                               : strf("%d \xc3\x97 %d  \xc2\xb7  %.2f rows per line", sizes[i].first, sizes[i].second, p.rowsPerLine));
                    if (sizes[i].first == st_.windowW && sizes[i].second == st_.windowH)
                        sel = static_cast<int>(i);
                }
                if (ui_.dropdown("window-size", R(c.x + 140, wy, c.width - 160, 34), labels, sel))
                {
                    st_.windowW = sizes[static_cast<size_t>(sel)].first;
                    st_.windowH = sizes[static_cast<size_t>(sel)].second;
                    markDirty();
                }
            }
            else
                ui_.wrap("Borderless full screen on this monitor. The picture keeps the PS2's 4:3 shape.", c.x + 20, c.y + c.height - 40, c.width - 40,
                         12.5f, t.faint);
        }
        // Picture: scaling and interlacing, and the live preview of all the picture settings
        const float ph = std::floor((bottom - kTop - kGap) * 0.62f);
        {
            const Rectangle c = R(rx, kTop, rw, ph);
            ui_.card(c, "Picture");
            int pic = st_.preview == "menu" ? 1 : st_.preview == "title" ? 2 : 0;
            if (ui_.segmented("preview-pic", R(c.x + c.width - 236, c.y + 12, 216, 30), {"Race", "Menu", "Title"}, pic, true, 12.5f))
            {
                st_.preview = pic == 1 ? "menu" : pic == 2 ? "title" : "race";
                markDirty();
                preparePreview();
            }
            // Scaling (GT4_PICTURE_FILTER) and Interlacing (GT4_DEINTERLACE): one row when the card is wide enough,
            // else two. The dropdowns list the default first.
            const float labelW1 = ui_.textWidth("Scaling", 14), labelW2 = ui_.textWidth("Interlacing", 14);
            const float ddW = 150.0f;
            const bool oneRow = c.width - 40 >= labelW1 + labelW2 + 2 * ddW + 3 * 12 + 16;
            float y = c.y + 54;
            static const PictureFilter kFilters[] = {PictureFilter::Smooth, PictureFilter::Sharp, PictureFilter::Nearest};
            static const Deinterlace kModes[] = {Deinterlace::Blend, Deinterlace::Adaptive, Deinterlace::Bob, Deinterlace::Weave};
            int filterSel = 0, modeSel = 0;
            for (int i = 0; i < 3; ++i)
                if (kFilters[i] == st_.filter)
                    filterSel = i;
            for (int i = 0; i < 4; ++i)
                if (kModes[i] == st_.deinterlace)
                    modeSel = i;
            float x = c.x + 20;
            ui_.text("Scaling", x, y + 8, 14, t.text);
            if (ui_.dropdown("picture-filter", R(x + labelW1 + 12, y, ddW, 32), {"Smooth", "Sharp", "Nearest"}, filterSel))
            {
                st_.filter = kFilters[filterSel];
                markDirty();
                preparePreview();
            }
            if (oneRow)
                x = x + labelW1 + 12 + ddW + 16;
            else
                y += 40;
            ui_.text("Interlacing", x, y + 8, 14, t.text);
            if (ui_.dropdown("deinterlace", R(x + labelW2 + 12, y, ddW, 32), {"Blend", "Adaptive", "Bob", "Weave"}, modeSel))
            {
                st_.deinterlace = kModes[modeSel];
                markDirty();
                preparePreview();
            }
            y += 40;
            const char *how = st_.deinterlace == Deinterlace::Blend      ? "Blend: the two half pictures of each frame together, blended a little, as a TV smoothed them."
                              : st_.deinterlace == Deinterlace::Adaptive ? "Adaptive: both half pictures where the picture is still, one at a time where it moves."
                              : st_.deinterlace == Deinterlace::Bob      ? "Bob: one half picture at a time, the lines between filled in. Steady, a little softer."
                                                                          : "Weave: both half pictures as they are, no deinterlacing. Moving edges show combs.";
            ui_.text(ui_.fit(how, c.width - 40, 12.5f), c.x + 20, y - 2, 12.5f, t.faint);
            y += 20;
            const Rectangle pr = R(c.x + 16, y, c.width - 32, c.y + c.height - 40 - y);
            drawPreview(pr);
            const std::string cap = strf("Real pixels (1:1) as GT4 draws them %s %s: %s. Drag to compare.",
                                         st_.fullscreen ? "full screen on" : "in a window on", mon ? mon->name.c_str() : "this screen",
                                         pl.integer ? strf("%d screen rows per TV line", pl.integer).c_str() : strf("%.2f screen rows per TV line", pl.rowsPerLine).c_str());
            ui_.text(ui_.fit(cap, c.width - 40, 12.5f), c.x + 20, c.y + c.height - 30, 12.5f, t.faint);
        }
        // On this screen
        {
            const float cy = kTop + ph + kGap;
            const Rectangle c = R(rx, cy, rw, bottom - cy);
            ui_.card(c, "On this screen");
            // The screen to scale with the picture in it.
            const float dh = c.height - 64, dw = std::min(dh * static_cast<float>(ww) / std::max(1, wh), 220.0f);
            const float dhh = dw * static_cast<float>(wh) / std::max(1, ww);
            const Rectangle scr = R(c.x + 20, c.y + 50 + (dh - dhh) * 0.5f, dw, dhh);
            ui_.fill(scr, BLACK);
            ui_.roundLine(R(scr.x - 3, scr.y - 3, scr.width + 6, scr.height + 6), 4, 1.5f, t.faint);
            const float k = dw / std::max(1, ww);
            const Rectangle picR = R(scr.x + pl.x * k, scr.y + pl.y * k, pl.w * k, pl.h * k);
            ui_.fill(picR, ui::alpha(t.accent, 0.35f));
            for (float yy = picR.y + 2; yy < picR.y + picR.height; yy += 3)
                ui_.fill(R(picR.x, yy, picR.width, 1), ui::alpha(t.accent, 0.5f));
            const float tx = scr.x + dw + 24, tw = c.x + c.width - 20 - tx;
            float y = c.y + 50;
            ui_.text(strf("%d \xc3\x97 %d picture", static_cast<int>(std::lround(pl.w)), static_cast<int>(std::lround(pl.h))), tx, y, 20, t.text, Weight::Bold);
            y += 30;
            ui_.text(pl.integer ? strf("%d screen rows per TV line (448 lines)", pl.integer) : strf("%.2f screen rows per TV line (448 lines)", pl.rowsPerLine), tx, y,
                     14, t.text);
            y += 24;
            const int bx = static_cast<int>(std::lround(pl.x)), by = static_cast<int>(std::floor(pl.y));
            // Wrapped, not cut: at 1280x960 this line is wider than the panel.
            y += ui_.wrap(strf("%s %d\xc3\x97%d: black borders %d px left and right, %d px top and bottom", st_.fullscreen ? "Screen" : "Window", ww, wh, bx, by), tx, y,
                          tw, 13, t.muted);
            y += 6;
            const float guard = scan::moireGuard(pl.rowsPerLine, pl.integer);
            const char *verdict = pl.integer ? "Whole rows: every line looks the same." : guard >= 1.0f ? "Filled: 3 or more rows per line, lines stay even." : "Below 3 rows per line: the lines are made weaker to avoid bands.";
            ui_.icon(pl.integer || guard >= 1.0f ? Icon::Check : Icon::Warn, R(tx, y, 18, 18), pl.integer || guard >= 1.0f ? t.ok : t.warn);
            y += ui_.wrap(verdict, tx + 26, y, tw - 26, 13, pl.integer || guard >= 1.0f ? t.ok : t.warn) + 12;
            for (const MonitorInfo &m : monitors_)
            {
                if (mon && m.index == mon->index)
                    continue;
                const scan::Placement o = scan::fitPicture(static_cast<float>(m.w), static_cast<float>(m.h), scan::kLines, 4.0f / 3.0f, st_.fit);
                const std::string line = strf("%s %d\xc3\x97%d full screen: %d\xc3\x97%d, %s", m.name.c_str(), m.w, m.h, static_cast<int>(std::lround(o.w)), static_cast<int>(std::lround(o.h)),
                                              o.integer ? strf("%d rows per line", o.integer).c_str() : strf("%.2f rows per line", o.rowsPerLine).c_str());
                if (y + ui_.wrapHeight(line, tw, 12.5f) > c.y + c.height - 8)
                    break;
                y += ui_.wrap(line, tx, y, tw, 12.5f, t.faint) + 3;
            }
        }
    }

    // ------------------------------------------------------------------ Controllers
    PadView App::padView() const
    {
        PadView v;
        if (devices_.demo())
        {
            // The demo pad: the right trigger two thirds in (R2, analog), A held, a little steering.
            v.live = true;
            v.pressure[11] = 0.68f; // R2 from the right trigger
            v.pressed[9] = true;
            v.pressed[14] = true; // Cross (A)
            v.lx = -0.32f;
            v.ly = 0.05f;
            return v;
        }
        const int j = padJoystick();
        if (j < 0)
            return v;
        v.live = true;
        for (int i = 0; i < kPadButtons; ++i)
        {
            const int b = st_.pad.buttons[static_cast<size_t>(i)];
            v.pressed[static_cast<size_t>(i)] = b > 0 && IsGamepadButtonDown(j, b);
        }
        for (int p = 0; p < kPadPressures; ++p)
        {
            const int a = st_.pad.pressure[static_cast<size_t>(p)];
            if (a < 0)
                continue;
            // As the runtime: the travel up to the end point is the whole range, then the start point's dead zone comes off.
            const float full = std::clamp(st_.pad.triggerFull, 0.5f, 1.0f);
            const float raw = std::clamp((GetGamepadAxisMovement(j, a) + 1.0f) * 0.5f / full, 0.0f, 1.0f);
            const float dz = st_.pad.triggerDeadzone;
            const float val = raw <= dz ? 0.0f : (raw - dz) / (1.0f - dz);
            v.pressure[static_cast<size_t>(p)] = val;
            // The runtime presses the button from a pressure of 10 / 255 (GT4 builds,
            // gt4-pad-pressure.patch), so the drawing does too.
            for (int b = 0; b < kPadButtons; ++b)
                if (pressureOfButton(b) == p && val * 255.0f >= 9.5f)
                    v.pressed[static_cast<size_t>(b)] = true;
        }
        auto stick = [&](int s) {
            const int a = st_.pad.stickAxis[static_cast<size_t>(s)];
            float x = a >= 0 ? GetGamepadAxisMovement(j, a) : 0.0f;
            if (st_.pad.stickInvert[static_cast<size_t>(s)])
                x = -x;
            const float dz = s < 2 ? st_.pad.rightDeadzone : st_.pad.leftDeadzone;
            return std::fabs(x) <= dz ? 0.0f : std::copysign((std::fabs(x) - dz) / (1.0f - dz), x);
        };
        v.rx = stick(0);
        v.ry = stick(1);
        v.lx = stick(2);
        v.ly = stick(3);
        return v;
    }

    void App::drawDualShock(Rectangle r, const PadView &v)
    {
        const ui::Theme &t = ui::theme();
        const float k = std::min(r.width / 520.0f, r.height / 214.0f);
        const float ox = r.x + (r.width - 520.0f * k) * 0.5f, oy = r.y + (r.height - 214.0f * k) * 0.5f;
        auto X = [&](float x) { return ox + x * k; };
        auto Y = [&](float y) { return oy + y * k; };
        auto B = [&](float x, float y, float w, float h) { return R(X(x), Y(y), w * k, h * k); };
        const Color body{46, 54, 70, 255}, bodyEdge{70, 80, 98, 255}, centre{33, 39, 51, 255}, key{60, 70, 88, 255};
        const Color hi = t.accent;
        // shoulders
        const struct
        {
            float x, y, w, h;
            int button;
            const char *label;
        } shoulders[] = {{92, 4, 78, 22, 8, "L2"}, {92, 24, 78, 20, 10, "L1"}, {350, 4, 78, 22, 9, "R2"}, {350, 24, 78, 20, 11, "R1"}};
        for (const auto &s : shoulders)
        {
            const bool on = v.pressed[static_cast<size_t>(s.button)];
            const int p = pressureOfButton(s.button);
            const float pr = p >= 0 ? v.pressure[static_cast<size_t>(p)] : 0.0f;
            ui_.round(B(s.x, s.y, s.w, s.h), 6 * k, on ? hi : ui::mixColor(key, hi, pr));
            ui_.text(s.label, X(s.x + s.w * 0.5f), Y(s.y + s.h * 0.5f) - 7.5f * std::max(0.8f, k), 11.5f * std::max(0.8f, k), on ? WHITE : t.muted,
                     Weight::Semibold, Align::Center);
        }
        // body: two grips and the bar
        ui_.circle(X(130), Y(150), 60 * k, bodyEdge);
        ui_.circle(X(390), Y(150), 60 * k, bodyEdge);
        ui_.round(B(58, 40, 404, 114), 44 * k, bodyEdge);
        ui_.circle(X(130), Y(150), 58 * k, body);
        ui_.circle(X(390), Y(150), 58 * k, body);
        ui_.round(B(60, 42, 400, 110), 42 * k, body);
        ui_.round(B(196, 64, 128, 60), 14 * k, centre);
        // d-pad
        const struct
        {
            float x, y, w, h;
            int button;
        } dpad[] = {{120, 58, 20, 26, 4}, {140, 84, 26, 20, 5}, {120, 104, 20, 26, 6}, {94, 84, 26, 20, 7}};
        ui_.round(B(120, 84, 20, 20), 2 * k, key);
        for (const auto &d : dpad)
            ui_.round(B(d.x, d.y, d.w, d.h), 4 * k, v.pressed[static_cast<size_t>(d.button)] ? hi : key);
        // select / start / analog
        ui_.round(B(222, 78, 22, 9), 4 * k, v.pressed[0] ? hi : key);
        ui_.round(B(276, 78, 26, 9), 4 * k, v.pressed[3] ? hi : key);
        ui_.text("SELECT", X(233), Y(92), 8.5f * std::max(0.9f, k), t.faint, Weight::Semibold, Align::Center);
        ui_.text("START", X(289), Y(92), 8.5f * std::max(0.9f, k), t.faint, Weight::Semibold, Align::Center);
        ui_.circle(X(260), Y(112), 5 * k, Color{120, 40, 40, 255});
        // face buttons
        const Color triC{90, 211, 182, 255}, cirC{255, 107, 107, 255}, croC{122, 167, 255, 255}, squC{228, 139, 216, 255};
        const struct
        {
            float x, y;
            int button;
            Color c;
            int shape;
        } face[] = {{390, 64, 12, triC, 0}, {418, 92, 13, cirC, 1}, {390, 120, 14, croC, 2}, {362, 92, 15, squC, 3}};
        for (const auto &f : face)
        {
            const float cx = X(f.x), cy = Y(f.y), rr = 13 * k;
            const bool on = v.pressed[static_cast<size_t>(f.button)];
            const int p = pressureOfButton(f.button);
            const float pr = p >= 0 ? v.pressure[static_cast<size_t>(p)] : 0.0f;
            ui_.circle(cx, cy, rr, on ? ui::alpha(f.c, 0.9f) : key);
            if (pr > 0.01f)
                DrawRing(Vector2{cx * ui_.s, cy * ui_.s}, (rr + 2.5f * k) * ui_.s, (rr + 5.0f * k) * ui_.s, -90.0f, -90.0f + 360.0f * pr, 40, f.c);
            const Color sc = on ? WHITE : f.c;
            const float u = rr * 0.48f;
            if (f.shape == 0)
            {
                ui_.line(cx, cy - u, cx - u * 0.95f, cy + u * 0.6f, 1.8f * k, sc);
                ui_.line(cx - u * 0.95f, cy + u * 0.6f, cx + u * 0.95f, cy + u * 0.6f, 1.8f * k, sc);
                ui_.line(cx + u * 0.95f, cy + u * 0.6f, cx, cy - u, 1.8f * k, sc);
            }
            else if (f.shape == 1)
                ui_.ring(cx, cy, u * 0.85f, u * 0.85f + 1.8f * k, sc);
            else if (f.shape == 2)
            {
                ui_.line(cx - u * 0.8f, cy - u * 0.8f, cx + u * 0.8f, cy + u * 0.8f, 1.8f * k, sc);
                ui_.line(cx - u * 0.8f, cy + u * 0.8f, cx + u * 0.8f, cy - u * 0.8f, 1.8f * k, sc);
            }
            else
                ui_.roundLine(R(cx - u * 0.75f, cy - u * 0.75f, u * 1.5f, u * 1.5f), 1, 1.8f * k, sc);
        }
        // sticks
        auto stick = [&](float x, float y, float sx, float sy, bool click) {
            ui_.circle(X(x), Y(y), 30 * k, centre);
            ui_.ring(X(x), Y(y), 29 * k, 31 * k, bodyEdge);
            const float cx = X(x + sx * 11), cy = Y(y + sy * 11);
            ui_.circle(cx, cy, 19 * k, click ? hi : key);
            ui_.ring(cx, cy, 15 * k, 16.5f * k, click ? WHITE : Color{80, 92, 112, 255});
        };
        stick(198, 152, v.lx, v.ly, v.pressed[1]);
        stick(322, 152, v.rx, v.ry, v.pressed[2]);
    }

    void App::pageControllers()
    {
        drawHeader("Controllers", controllersTab_ == 0   ? "Your controllers as PS2 DualShock 2s: player 1 and player 2. Live input shows what GT4 gets from player 1."
                                  : controllersTab_ == 1 ? "Your wheel and pedals: GT4 sees a Logitech Driving Force Pro on USB port 1."
                                                         : "The PS2 EyeToy camera: what GT4 sees when it looks for one.");
        const float x0 = kSidebarW + 32, x1 = ui_.width - 32, bottom = ui_.height - 48;
        int tab = controllersTab_;
        if (ui_.segmented("ctl-tab", R(x1 - 450, 30, 450, 36), {"Controller", "Wheel & pedals", "EyeToy camera"}, tab, true, 14))
        {
            controllersTab_ = tab;
            capture_ = -1;
            if (tab == 2)
                lastCameraList_ = -100.0; // list the webcams again now
        }
        if (controllersTab_ == 0)
            pagePad(x0, kTop, x1 - x0, bottom - kTop);
        else if (controllersTab_ == 1)
            pageWheel(x0, kTop, x1 - x0, bottom - kTop);
        else
            pageCamera(x0, kTop, x1 - x0, bottom - kTop);
    }

    void App::pagePad(float x0, float y0, float cw, float ch)
    {
        const ui::Theme &t = ui::theme();
        const float lw = std::floor(std::min(400.0f, cw * 0.39f));
        const float rx = x0 + lw + kGap, rw = x0 + cw - rx;
        const bool ps = [&] {
            for (const DeviceInfo &d : devices_.list())
                if (d.glfwGuid == st_.pad.sourceGuid && d.vid == 0x054c)
                    return true;
            return false;
        }();

        // Connected devices
        const float dh = 286;
        {
            const Rectangle c = R(x0, y0, lw, dh);
            ui_.card(c, "Connected", devices_.status());
            float y = c.y + 50;
            const auto &list = devices_.list();
            if (list.empty())
            {
                ui_.icon(Icon::Gamepad, R(c.x + 20, y + 4, 22, 22), t.faint);
                y += ui_.wrap("No game controllers found. Plug one in (or switch it on): the list updates by itself.", c.x + 52, y + 4, c.width - 72, 13.5f, t.muted);
                if (hidden_ && !devices_.demo())
                    ui_.wrap("Test mode reads only the list of devices; live input is shown in a normal session.", c.x + 52, y + 14, c.width - 72, 12.5f, t.faint);
            }
            int n = 0;
            for (const DeviceInfo &d : list)
            {
                if (n++ >= 5)
                    break;
                ui_.icon(kindIcon(d.kind), R(c.x + 20, y + 6, 24, 24), d.kind == DeviceKind::Gamepad ? t.accent : t.text);
                ui_.text(ui_.fit(d.name, c.width - 100, 14, Weight::Semibold), c.x + 56, y + 2, 14, t.text, Weight::Semibold);
                ui_.text(ui_.fit(strf("%s \xc2\xb7 %s%s", d.idText().c_str(), d.kindText(), d.forceFeedback ? " \xc2\xb7 force feedback" : ""), c.width - 100, 12.5f),
                         c.x + 56, y + 21, 12.5f, t.muted);
                bool active = false;
                for (int b = 0; b < d.buttonCount && !active; ++b)
                    active = d.buttons[static_cast<size_t>(b)] != 0;
                ui_.statusDot(c.x + c.width - 26, y + 18, d.live ? (active ? t.ok : ui::mixColor(t.ok, t.panel3, 0.5f)) : t.faint);
                y += 44;
            }
        }
        // Players
        {
            const float cy = y0 + dh + kGap;
            const Rectangle c = R(x0, cy, lw, y0 + ch - cy);
            ui_.card(c, "Players");
            float y = c.y + 52;
            ui_.text("Player 1 \xc2\xb7 DualShock 2 in port 1", c.x + 20, y, 14, t.text, Weight::Semibold);
            y += 24;
            std::vector<std::string> opts{"Automatic: the first game device"};
            std::vector<std::string> guids{std::string()};
            for (const DeviceInfo &d : devices_.list())
                if (d.kind == DeviceKind::Gamepad && !d.glfwGuid.empty() && std::find(guids.begin(), guids.end(), d.glfwGuid) == guids.end())
                {
                    opts.push_back(d.name);
                    guids.push_back(d.glfwGuid);
                }
            for (const GamepadInfo &g : devices_.glfwGamepads())
                if (g.mapped && std::find(guids.begin(), guids.end(), g.guid) == guids.end())
                {
                    opts.push_back(g.name);
                    guids.push_back(g.guid);
                }
            int sel = 0;
            for (size_t i = 0; i < guids.size(); ++i)
                if (guids[i] == st_.pad.sourceGuid)
                    sel = static_cast<int>(i);
            if (!st_.pad.sourceGuid.empty() && sel == 0)
            {
                opts.push_back("The chosen controller (not connected)");
                guids.push_back(st_.pad.sourceGuid);
                sel = static_cast<int>(guids.size()) - 1;
            }
            if (ui_.dropdown("p1-pad", R(c.x + 20, y, c.width - 40, 34), opts, sel))
            {
                st_.pad.sourceGuid = guids[static_cast<size_t>(sel)];
                markDirty();
            }
            y += 46;
            // Player 2 (1.4): GT4_PAD2, read by the player-2 runtime with the same rule shown here
            // (shared/gt4_pad2_rules.h): Automatic, Off, or one controller by its GLFW id.
            ui_.text("Player 2 \xc2\xb7 DualShock 2 in port 2", c.x + 20, y, 14, t.text, Weight::Semibold);
            y += 24;
            {
                // The list: the controllers GT4.exe can use as player 2 (connected, with a gamepad mapping, as the rule asks),
                // each id once. The label and the name kept for it are separate (the kept name never gets the label's notes).
                // (1.4.1) Off first: it is GT4.exe's default (an absent GT4_PAD2), so an untouched section shows Off.
                std::vector<std::string> o2{"Off: nothing in port 2 (the default)", "Automatic: the second controller"};
                std::vector<std::string> g2{std::string(), std::string()};
                std::vector<std::string> n2{std::string(), std::string()};
                const gt4pad2rules::Inventory inv = pad2Inventory();
                const uint32_t p1 = player1Joysticks(player1Sources(st_.pad), inv); // player 1's controllers now
                for (size_t j = 0; j < inv.size(); ++j)
                    if (inv[j].connected && inv[j].mapped && !inv[j].guid.empty() && std::find(g2.begin() + 2, g2.end(), inv[j].guid) == g2.end())
                    {
                        // Every Xbox-type pad has the same id: two of them are one entry, which picks neither (the rule).
                        int same = 0;
                        for (size_t k = 0; k < inv.size(); ++k)
                            same += inv[k].connected && inv[k].mapped && inv[k].guid == inv[j].guid ? 1 : 0;
                        if (same > 1)
                            o2.push_back(strf("%s (%d connected with one id: use Automatic)",
                                              gt4pad2rules::isXInputGuid(inv[j].guid) ? "Xbox-type controller" : inv[j].name.c_str(), same));
                        else
                            o2.push_back(inv[j].name + (((p1 >> j) & 1u) ? " (player 1's now)" : ""));
                        g2.push_back(inv[j].guid);
                        n2.push_back(inv[j].name);
                    }
                int sel2 = st_.pad2.mode == Pad2Settings::Mode::Automatic ? 1 : 0;
                if (st_.pad2.mode == Pad2Settings::Mode::Chosen)
                {
                    sel2 = -1;
                    for (size_t i = 2; i < g2.size(); ++i)
                        if (!st_.pad2.guid.empty() && g2[i] == st_.pad2.guid)
                            sel2 = static_cast<int>(i);
                    if (sel2 < 0)
                    {
                        o2.push_back(st_.pad2.guid.empty() ? strf("Joystick %d (set by hand)", st_.pad2.joystick)
                                                           : (st_.pad2.name.empty() ? std::string("The chosen controller") : st_.pad2.name) + " (not connected)");
                        g2.push_back(st_.pad2.guid);
                        n2.push_back(st_.pad2.name);
                        sel2 = static_cast<int>(g2.size()) - 1;
                    }
                }
                if (ui_.dropdown("p2-pad", R(c.x + 20, y, c.width - 40, 34), o2, sel2))
                {
                    if (sel2 == 0)
                    {
                        st_.pad2 = Pad2Settings{};
                        st_.pad2.mode = Pad2Settings::Mode::Off;
                    }
                    else if (sel2 == 1)
                    {
                        st_.pad2 = Pad2Settings{};
                        st_.pad2.mode = Pad2Settings::Mode::Automatic;
                    }
                    else if (!g2[static_cast<size_t>(sel2)].empty())
                    {
                        st_.pad2.mode = Pad2Settings::Mode::Chosen;
                        st_.pad2.guid = g2[static_cast<size_t>(sel2)];
                        st_.pad2.joystick = -1;
                        st_.pad2.name = n2[static_cast<size_t>(sel2)];
                    }
                    st_.pad2.explicitChoice = true; // picked here: written, Off too
                    markDirty();
                }
                y += 40;
                // Who is player 2 now, by the rule GT4.exe uses (a wheel or pedal set is never player 2's; a
                // lone controller stays player 1's).
                std::string why, now;
                const int pick = pad2Pick(&why);
                Color nowColour = t.muted;
                bool good = false;
                if (!gt4_.pad2)
                {
                    now = "The GT4 program here has no player 2: port 2 stays empty, whatever is chosen above.";
                    nowColour = t.warn;
                }
                else if (st_.pad2.mode == Pad2Settings::Mode::Off)
                    now = "Port 2 stays empty, as on a PS2 with one controller.";
                else if (pick >= 0)
                {
                    now = "Now: " + inv[static_cast<size_t>(pick)].name + " is player 2.";
                    nowColour = t.text;
                    good = true;
                }
                else if (why == "player 1 has no controller")
                    now = "Now: none (player 1 has no controller yet).";
                else if (why == "no second controller")
                    now = "Now: none. Plug in a second controller and it becomes player 2.";
                else if (why == "more than one connected controller has that id")
                {
                    now = "Now: none: two connected Xbox-type pads share that id. Choose Automatic.";
                    nowColour = t.warn;
                }
                else
                {
                    // The chosen controller: player 1's now, or not connected.
                    std::string mine;
                    for (size_t j = 0; j < inv.size(); ++j)
                        if (((p1 >> j) & 1u) && inv[j].connected && inv[j].guid == st_.pad2.guid)
                            mine = inv[j].name;
                    now = mine.empty() ? "Now: none (the chosen controller is not connected)."
                                       : "Now: none: " + mine + " is player 1's controller. Pick another one, or Automatic.";
                    nowColour = t.warn;
                }
                if (good)
                    ui_.statusDot(c.x + 26, y + 9, t.ok);
                y += ui_.wrap(now, c.x + (good ? 38.0f : 20.0f), y, c.width - (good ? 58.0f : 40.0f), 12.5f, nowColour) + 2;
                // Player 2's buttons: the standard layout (its own mapping, PS2X_INPUT2, is not on this page; a hand-written one is kept).
                if (gt4_.pad2 && st_.pad2.mode != Pad2Settings::Mode::Off)
                    y += ui_.wrap("Player 2 uses the standard DualShock 2 layout; the table on the right is player 1's.", c.x + 20, y, c.width - 40, 12, t.faint) + 2;
            }
            // Two traps of the runtime's choice for player 1 (ps2_input_profile.cpp resolveSource), shown
            // when they apply:
            // - every Xbox-type (XInput) pad has the same GLFW id, and an id that matches two
            //   connected pads picks neither (GT4 falls back to the keyboard);
            // - "Automatic" is GLFW's joystick 1, which can be a wheel or pedal set (not a
            //   controller GT4 can use as a pad).
            std::string padNote;
            bool padWarn = false;
            if (startsWith(st_.pad.sourceGuid, "78696e707574"))
            {
                const auto xinputPads = std::count_if(devices_.list().begin(), devices_.list().end(), [](const DeviceInfo &d) { return d.xinput; });
                if (xinputPads > 1)
                {
                    padNote = "Player 1: Xbox-type pads share one id, so with two connected GT4 uses neither for player 1. Choose Automatic.";
                    padWarn = true;
                }
            }
            else if (st_.pad.sourceGuid.empty())
            {
                const std::vector<GamepadInfo> glfwPads = devices_.glfwGamepads();
                const bool firstIsPad = !glfwPads.empty() && glfwPads.front().joystick == 0 && glfwPads.front().mapped;
                const bool anyPad = std::any_of(glfwPads.begin(), glfwPads.end(), [](const GamepadInfo &g) { return g.mapped; });
                if (!glfwPads.empty() && !firstIsPad && anyPad)
                {
                    padNote = "Automatic takes the first game device, now " +
                              (glfwPads.front().joystick == 0 ? glfwPads.front().name : std::string("none")) + ": pick your controller for player 1.";
                    padWarn = true;
                }
            }
            if (padWarn)
                y += ui_.wrap(padNote, c.x + 20, y, c.width - 40, 12, t.warn) + 2;
            y += 12;
            ui_.fill(R(c.x + 20, y, c.width - 40, 1), t.border);
            y += 14;
            // GT3_VIBRATION is both players'; the strength is player 1's mapping's (its rumble row).
            ui_.text("Vibration (both players)", c.x + 20, y + 2, 14, t.text);
            if (ui_.toggle("vibration", c.x + c.width - 60, y, st_.vibration))
                markDirty();
            y += 34;
            const std::string strengthLabel = "Player 1 strength";
            const float sx = c.x + 20 + ui_.textWidth(strengthLabel, 13.5f) + 14;
            ui_.text(strengthLabel, c.x + 20, y + 2, 13.5f, st_.vibration ? t.muted : t.faint);
            float rv = st_.pad.rumble * 100.0f;
            if (ui_.slider("rumble", R(sx, y, c.x + c.width - 80 - sx, 24), rv, 10, 100, 10, st_.vibration))
            {
                st_.pad.rumble = rv / 100.0f;
                markDirty();
            }
            ui_.text(strf("%d%%", static_cast<int>(std::lround(st_.pad.rumble * 100))), c.x + c.width - 20, y + 2, 13.5f, t.muted, Weight::Regular, Align::Right);
            y += 36;
            ui_.text("Keyboard as player 1 when no controller", c.x + 20, y + 2, 14, t.text);
            if (ui_.toggle("keyboard", c.x + c.width - 60, y, st_.pad.keyboard))
                markDirty();
        }
        // The DualShock 2 and its mapping
        {
            const Rectangle c = R(rx, y0, rw, ch);
            ui_.card(c, "DualShock 2 mapping");
            if (ui_.button("preset-ds2", R(c.x + c.width - 392, c.y + 12, 180, 30), "DualShock layout", Style::Secondary, true, 12.5f))
            {
                const PadSettings keep = st_.pad;
                st_.pad.applyStandard();
                st_.pad.sourceGuid = keep.sourceGuid;
                markDirty();
            }
            if (ui_.button("preset-trig", R(c.x + c.width - 204, c.y + 12, 184, 30), "Triggers = Cross / Square", Style::Secondary, true, 12.5f))
            {
                st_.pad.applyTriggersOnFaceButtons();
                markDirty();
            }
            const PadView view = padView();
            drawDualShock(R(c.x + 20, c.y + 52, c.width - 40, 196), view);
            if (!view.live)
                ui_.text(hidden_ ? "Live input: off in test mode" : "Live input: no controller", c.x + 24, c.y + 54, 12, t.faint);
            // The table: two columns of eight.
            const float ty = c.y + 262, rowH = 28;
            const float colW = (c.width - 40 - 16) * 0.5f;
            const int order[kPadButtons] = {14, 13, 15, 12, 10, 11, 8, 9, 4, 6, 7, 5, 0, 3, 1, 2}; // cross circle square triangle l1 r1 l2 r2 up down left right select start l3 r3
            ui_.text("PS2 button", c.x + 24, ty - 4, 11.5f, t.faint, Weight::Semibold);
            ui_.text("PS2 button", c.x + 20 + colW + 16 + 4, ty - 4, 11.5f, t.faint, Weight::Semibold);
            for (int col = 0; col < 2; ++col)
            {
                const float cx = c.x + 20 + col * (colW + 16);
                ui_.text("Button", cx + colW * 0.36f, ty - 4, 11.5f, t.faint, Weight::Semibold);
                ui_.text("Analog (pressure)", cx + colW * 0.68f, ty - 4, 11.5f, t.faint, Weight::Semibold);
            }
            for (int i = 0; i < kPadButtons; ++i)
            {
                const int b = order[i];
                const int col = i / 8, row = i % 8;
                const float cx = c.x + 20 + col * (colW + 16), y = ty + 16 + row * rowH;
                const bool lit = view.pressed[static_cast<size_t>(b)];
                if (row % 2 == 0)
                    ui_.round(R(cx, y, colW, rowH - 2), 6, ui::alpha(t.panel2, 0.5f));
                ui_.statusDot(cx + 10, y + rowH * 0.5f - 1, lit ? t.ok : t.panel3);
                ui_.text(kPadButtonLabels[b], cx + 22, y + 5, 13, t.text);
                // button cell
                const Rectangle bc = R(cx + colW * 0.34f, y + 2, colW * 0.31f, rowH - 6);
                const bool capB = capture_ == b;
                const int gb = st_.pad.buttons[static_cast<size_t>(b)];
                if (ui_.rowButton("pb-" + std::to_string(b), bc, capB, !hidden_))
                {
                    capture_ = b;
                    captureAt_ = GetTime();
                }
                if (ui_.rightClicked(bc))
                {
                    st_.pad.buttons[static_cast<size_t>(b)] = -1;
                    markDirty();
                }
                ui_.text(ui_.fit(capB ? std::string("press...") : gamepadButtonName(gb, ps), bc.width - 10, 12.5f), bc.x + 6, bc.y + 4, 12.5f,
                         capB ? t.warn : (gb > 0 ? t.muted : t.faint));
                // analog cell
                const int p = pressureOfButton(b);
                if (p >= 0)
                {
                    const Rectangle ac = R(cx + colW * 0.67f, y + 2, colW * 0.33f, rowH - 6);
                    const bool capA = capture_ == 100 + p;
                    const int ax = st_.pad.pressure[static_cast<size_t>(p)];
                    if (ui_.rowButton("pa-" + std::to_string(p), ac, capA, !hidden_))
                    {
                        capture_ = 100 + p;
                        captureAt_ = GetTime();
                        if (const int j = padJoystick(); j >= 0)
                            for (int a = 0; a < kGamepadAxisCount; ++a)
                                captureRest_[static_cast<size_t>(a)] = GetGamepadAxisMovement(j, a);
                    }
                    if (ui_.rightClicked(ac))
                    {
                        st_.pad.pressure[static_cast<size_t>(p)] = -1;
                        markDirty();
                    }
                    const float pv = view.pressure[static_cast<size_t>(p)];
                    if (pv > 0.01f)
                        ui_.round(R(ac.x, ac.y + ac.height - 4, ac.width * pv, 3), 1.5f, t.ok);
                    const std::string an = capA ? std::string("pull...") : ax >= 0 ? std::string(gamepadAxisName(ax, ps)) : std::string("full press");
                    ui_.text(ui_.fit(an, ac.width - 10, 12.5f), ac.x + 6, ac.y + 4, 12.5f, capA ? t.warn : (ax >= 0 ? t.accent : t.faint));
                }
            }
            // Sticks and dead zones
            float y = ty + 16 + 8 * rowH + 14;
            const float sw = (c.width - 40 - 48) / 4.0f;
            auto dz = [&](const char *id, const char *label, float &value, float x, float lo = 0, float hi = 30) {
                ui_.text(ui_.fit(label, sw - 40, 13), x, y, 13, t.muted);
                ui_.text(strf("%d%%", static_cast<int>(std::lround(value * 100))), x + sw, y, 13, t.text, Weight::Regular, Align::Right);
                float v = value * 100.0f;
                if (ui_.slider(id, R(x + 4, y + 20, sw - 8, 22), v, lo, hi, 1))
                {
                    value = v / 100.0f;
                    markDirty();
                }
            };
            dz("dz-left", "Left stick dead zone", st_.pad.leftDeadzone, c.x + 20);
            dz("dz-right", "Right stick dead zone", st_.pad.rightDeadzone, c.x + 20 + sw + 16);
            // The triggers' start and end points (developer, 2026-10-09): the travel before any pressure, and where it is full.
            dz("dz-trig", "Trigger start", st_.pad.triggerDeadzone, c.x + 20 + 2 * (sw + 16));
            dz("trig-full", "Trigger full at", st_.pad.triggerFull, c.x + 20 + 3 * (sw + 16), 50, 100);
            y += 54;
#if defined(GT4L_EDITION_FULL)
            {
                // GT4_TRIGGER_RANGE (developer, 2026-10-09: the triggers felt full at under half their travel): the pressure L2 / R2
                // reach at full travel; lower spreads GT4's throttle and brake over the whole trigger.
                const float rw2 = 2 * sw + 16;
                ui_.text("Trigger range (GT4's pressure at the end point)", c.x + 20, y, 13, t.muted);
                ui_.text(strf("%d%%", st_.full.triggerRange), c.x + 20 + rw2, y, 13, t.text, Weight::Regular, Align::Right);
                float tr = static_cast<float>(st_.full.triggerRange);
                if (ui_.slider("trigger-range", R(c.x + 24, y + 20, rw2 - 8, 22), tr, 30, 100, 5, true))
                {
                    st_.full.triggerRange = static_cast<int>(std::lround(tr));
                    markDirty();
                }
                // (HUD hand-off 9's "Race HUD zoom" row was here: gone with GT3's radar settings, 20 Oct 2026; the radar has one
                // range and no zoom button, so GT4_HUD_ZOOM_KEY / _PAD are not written and Z stays GT4's Square.)
                ui_.wrap("Start: how far the trigger goes before GT4 sees it. Full at: where it counts as all the way in. Range: lower it if GT4's "
                         "throttle or brake is full too early.",
                         c.x + 20 + rw2 + 32, y + 2, c.width - 40 - rw2 - 32, 11.5f, t.faint);
                y += 54;
            }
#endif
            ui_.wrap("Click a cell, then press the button or pull the trigger (Esc cancels); right-click clears. What each button does in a race "
                     "(accelerate, brake, shift) is chosen in GT4's own controller options.",
                     c.x + 20, y, c.width - 40, 12, t.faint);
        }

        // Capture (visible sessions)
        if (capture_ >= 0 && !hidden_)
        {
            const int j = padJoystick();
            if (IsKeyPressed(KEY_ESCAPE) || GetTime() - captureAt_ > 10.0)
                capture_ = -1;
            else if (j >= 0)
            {
                if (capture_ < 100)
                {
                    for (int b = 1; b < kGamepadButtonCount; ++b)
                        if (IsGamepadButtonPressed(j, b))
                        {
                            st_.pad.buttons[static_cast<size_t>(capture_)] = b;
                            capture_ = -1;
                            markDirty();
                            break;
                        }
                }
                else
                {
                    for (int a = 0; a < kGamepadAxisCount; ++a)
                        if (std::fabs(GetGamepadAxisMovement(j, a) - captureRest_[static_cast<size_t>(a)]) > 0.6f)
                        {
                            st_.pad.pressure[static_cast<size_t>(capture_ - 100)] = a;
                            capture_ = -1;
                            markDirty();
                            break;
                        }
                }
            }
        }
    }

    void App::pageWheel(float x0, float y0, float cw, float ch)
    {
        const ui::Theme &t = ui::theme();
        WheelSettings &w = st_.wheel;
        const float lw = std::floor(cw * 0.48f);
        const float rx = x0 + lw + kGap, rw = x0 + cw - rx;
        const DeviceInfo *wd = w.enabled ? wheelDevice() : nullptr;
        const DeviceInfo *pd = w.enabled ? pedalDevice() : nullptr;
        const auto &list = devices_.list();

        // Devices
        const float c1h = 232;
        {
            const Rectangle c = R(x0, y0, lw, c1h);
            ui_.card(c, "Wheel and pedals");
            float y = c.y + 52;
            // wheel base
            std::vector<std::string> opts{"None: no wheel", "Automatic: the wheel GT4 finds"};
            std::vector<std::string> specs{"", "auto"};
            for (const DeviceInfo &d : list)
                if (d.kind == DeviceKind::Wheel || (d.forceFeedback && d.kind != DeviceKind::Gamepad))
                {
                    opts.push_back(d.name + "  (" + d.idText() + ")");
                    specs.push_back(d.idText());
                }
            int sel = !w.enabled ? 0 : 1;
            if (w.enabled && lower(w.device) != "auto")
            {
                sel = -1;
                for (size_t i = 2; i < specs.size(); ++i)
                    if (iequals(specs[i], w.device))
                        sel = static_cast<int>(i);
                if (sel < 0)
                {
                    opts.push_back((w.deviceName.empty() ? w.device : w.deviceName) + "  (not connected)");
                    specs.push_back(w.device);
                    sel = static_cast<int>(specs.size()) - 1;
                }
            }
            ui_.text("Wheel base", c.x + 20, y + 8, 14, t.text);
            if (ui_.dropdown("wheel-dev", R(c.x + 120, y, c.width - 140, 34), opts, sel))
            {
                w.enabled = sel != 0;
                if (sel >= 1)
                    w.device = specs[static_cast<size_t>(sel)];
                for (const DeviceInfo &d : list)
                    if (iequals(d.idText(), w.device))
                        w.deviceName = d.name;
                markDirty();
            }
            y += 44;
            std::vector<std::string> popts{"Plugged into the wheel base", "Automatic: a device named \"pedals\""};
            std::vector<std::string> pspecs{"wheel", "auto"};
            for (const DeviceInfo &d : list)
                if (d.kind != DeviceKind::Gamepad && (!wd || d.instance != wd->instance))
                {
                    popts.push_back(d.name + "  (" + d.idText() + ")");
                    pspecs.push_back(d.idText());
                }
            int psel = lower(w.pedals) == "wheel" ? 0 : lower(w.pedals) == "auto" ? 1 : -1;
            if (psel < 0)
            {
                for (size_t i = 2; i < pspecs.size(); ++i)
                    if (iequals(pspecs[i], w.pedals))
                        psel = static_cast<int>(i);
                if (psel < 0)
                {
                    popts.push_back((w.pedalsName.empty() ? w.pedals : w.pedalsName) + "  (not connected)");
                    pspecs.push_back(w.pedals);
                    psel = static_cast<int>(pspecs.size()) - 1;
                }
            }
            ui_.text("Pedals", c.x + 20, y + 8, 14, w.enabled ? t.text : t.faint);
            if (ui_.dropdown("pedal-dev", R(c.x + 120, y, c.width - 140, 34), popts, psel, w.enabled))
            {
                w.pedals = pspecs[static_cast<size_t>(psel)];
                w.pedalsName.clear();
                for (const DeviceInfo &d : list)
                    if (iequals(d.idText(), w.pedals))
                        w.pedalsName = d.name;
                markDirty();
            }
            y += 44;
            ui_.text("GT4 sees", c.x + 20, y + 7, 14, w.enabled ? t.text : t.faint);
            int model = w.model == "df" ? 1 : w.model == "gtforce" ? 2 : 0;
            if (ui_.segmented("model", R(c.x + 120, y, c.width - 140, 32), {"Driving Force Pro", "Driving Force", "GT Force"}, model, w.enabled, 12.5f))
            {
                w.model = model == 1 ? "df" : model == 2 ? "gtforce" : "dfp";
                markDirty();
            }
            y += 44;
            if (!w.enabled)
                ui_.text("No wheel: GT4 gets the controller only.", c.x + 20, y, 13, t.faint);
            else if (wd)
            {
                ui_.statusDot(c.x + 26, y + 9, wd->live ? t.ok : ui::mixColor(t.ok, t.panel3, 0.4f));
                ui_.text(ui_.fit(strf("%s \xc2\xb7 %s%s", wd->name.c_str(), wd->forceFeedback ? "force feedback" : "no force feedback",
                                      pd && pd != wd ? (" \xc2\xb7 pedals: " + pd->name).c_str() : ""),
                                 c.width - 60, 13),
                         c.x + 40, y, 13, t.muted);
            }
            else
            {
                ui_.statusDot(c.x + 26, y + 9, t.warn);
                ui_.text("Not connected: plug the wheel in and switch the base on.", c.x + 40, y, 13, t.warn);
            }
        }
        // Steering
        const float c2y = y0 + c1h + kGap, c2h = 206;
        {
            const Rectangle c = R(x0, c2y, lw, c2h);
            ui_.card(c, "Steering");
            float y = c.y + 50;
            const float raw = wheelAxisValue(wd, w.steerAxis) * (w.steerInvert ? -1.0f : 1.0f);
            const float deg = raw / 32767.0f * static_cast<float>(w.rotation) * 0.5f;
            const float lock = w.model == "dfp" ? 900.0f : 200.0f;
            const float gt4 = std::clamp(deg / (lock * 0.5f), -1.0f, 1.0f);
            ui_.centreMeter(R(c.x + 20, y + 6, c.width - 220, 10), wd ? gt4 : 0.0f, t.accent);
            ui_.text(wd ? strf("%+.0f\xc2\xb0 \xc2\xb7 %d%% of GT4's lock", deg, static_cast<int>(std::lround(gt4 * 100))) : std::string("no wheel"), c.x + c.width - 20, y + 2, 13,
                     t.muted, Weight::Regular, Align::Right);
            y += 32;
            ui_.text("Axis", c.x + 20, y + 8, 14, w.enabled ? t.text : t.faint);
            int ax = std::max(0, axisIndex(w.steerAxis));
            std::vector<std::string> axes(kWheelAxisLabels, kWheelAxisLabels + kWheelAxes);
            if (ui_.dropdown("steer-axis", R(c.x + 120, y, 150, 32), axes, ax, w.enabled))
            {
                w.steerAxis = kWheelAxisKeys[ax];
                markDirty();
            }
            ui_.text("Invert", c.x + c.width - 120, y + 8, 14, w.enabled ? t.text : t.faint);
            if (ui_.toggle("steer-invert", c.x + c.width - 60, y + 5, w.steerInvert, w.enabled))
                markDirty();
            y += 46;
            ui_.text("Rotation", c.x + 20, y + 2, 14, w.enabled ? t.text : t.faint);
            float rot = static_cast<float>(w.rotation);
            if (ui_.slider("rotation", R(c.x + 120, y, c.width - 220, 24), rot, 90, 2880, 10, w.enabled))
            {
                w.rotation = static_cast<int>(std::lround(rot));
                markDirty();
            }
            ui_.text(strf("%d\xc2\xb0", w.rotation), c.x + c.width - 20, y + 2, 14, t.text, Weight::Semibold, Align::Right);
            y += 34;
            ui_.wrap(strf("Set the same rotation on the base (Fanatec: SEN). GT4's %s turns %d\xc2\xb0: full lock at \xc2\xb1%d\xc2\xb0 of your wheel.",
                          w.model == "dfp" ? "Driving Force Pro" : w.model == "df" ? "Driving Force" : "GT Force", static_cast<int>(lock),
                          static_cast<int>(std::lround(std::min(lock, static_cast<float>(w.rotation)) * 0.5f))),
                     c.x + 20, y, c.width - 40, 12, t.faint);
        }
        // Buttons
        {
            const float cy = c2y + c2h + kGap;
            const Rectangle c = R(x0, cy, lw, y0 + ch - cy);
            ui_.card(c, "Buttons");
            float y = c.y + 50;
            int mapped = 0;
            for (int b : w.buttons)
                mapped += b != 0;
            const WheelSettings standard;
            const bool isStandard = w.buttons == standard.buttons;
            ui_.text(ui_.fit(strf("%d wheel buttons mapped%s", mapped, isStandard ? ", in the Driving Force Pro layout" : " (changed)"), c.width - 190, 13.5f), c.x + 20,
                     y + 7, 13.5f, t.text);
            if (ui_.button("edit-buttons", R(c.x + c.width - 160, y, 140, 32), "Edit buttons...", Style::Secondary, w.enabled))
                modal_ = Modal::WheelButtons;
            y += 44;
            std::string pressed;
            if (wd)
                for (int b = 0; b < 128; ++b)
                    if (wd->buttons[static_cast<size_t>(b)])
                        pressed += (pressed.empty() ? "" : ", ") + strf("%d (%s)", b + 1, kWheelButtonLabels[std::clamp(w.buttons[static_cast<size_t>(b)], 0, kWheelButtonNames - 1)]);
            ui_.text(ui_.fit("Pressed now: " + (pressed.empty() ? std::string("-") : pressed), c.width - 40, 13), c.x + 20, y, 13, t.muted);
            y += 30;
            ui_.text("D-pad", c.x + 20, y + 7, 14, w.enabled ? t.text : t.faint);
            int hat = w.hat + 1;
            if (ui_.dropdown("hat", R(c.x + 120, y, 200, 32), {"None", "POV hat 1", "POV hat 2", "POV hat 3", "POV hat 4"}, hat, w.enabled))
            {
                w.hat = hat - 1;
                markDirty();
            }
        }
        // Pedals
        const float p1h = 270;
        {
            const Rectangle c = R(rx, y0, rw, p1h);
            ui_.card(c, "Pedals");
            float y = c.y + 50;
            const DeviceInfo *src = pd ? pd : wd;
            auto pedal = [&](int which, const char *label, PedalSettings &p) {
                ui_.text(label, c.x + 20, y + 7, 15, w.enabled ? t.text : t.faint, Weight::Semibold);
                std::vector<std::string> axes(kWheelAxisLabels, kWheelAxisLabels + kWheelAxes);
                axes.insert(axes.begin(), "None");
                int a = p.axis == "none" ? 0 : axisIndex(p.axis) + 1;
                if (ui_.dropdown(std::string("pedal-axis-") + label, R(c.x + 90, y, 150, 32), axes, a, w.enabled))
                {
                    p.axis = a == 0 ? "none" : kWheelAxisKeys[a - 1];
                    p.calibrated = false;
                    markDirty();
                }
                if (ui_.button(std::string("cal-") + label, R(c.x + c.width - 150, y, 130, 32), "Calibrate...", Style::Secondary, w.enabled && src && p.axis != "none"))
                {
                    calibratePedal_ = which;
                    calibrateHaveRest_ = false;
                    modal_ = Modal::Calibrate;
                }
                y += 40;
                const float raw = wheelAxisValue(src, p.axis);
                const float tr = src && p.axis != "none" ? pedalTravel(raw, p, w.pedalDeadzone) : 0.0f;
                ui_.meter(R(c.x + 20, y + 4, c.width - 110, 10), tr, which == 0 ? t.ok : t.bad);
                ui_.text(src ? strf("%d%%", static_cast<int>(std::lround(tr * 100))) : std::string("-"), c.x + c.width - 20, y, 13, t.text, Weight::Regular, Align::Right);
                y += 20;
                // Uncalibrated, the adapter takes the pedal's first reading as "released":
                // a foot on the pedal while GT4 starts would shift it.
                ui_.text(ui_.fit(p.calibrated ? strf("Calibrated: released at %d, full at %d", p.released, p.full)
                                              : std::string("Automatic: keep your foot off it while GT4 starts"),
                                 c.width - 40, 12),
                         c.x + 20, y, 12, t.faint);
                y += 26;
            };
            pedal(0, "Gas", w.gas);
            pedal(1, "Brake", w.brake);
            ui_.text("Dead zone", c.x + 20, y + 2, 13.5f, w.enabled ? t.muted : t.faint);
            float dzv = w.pedalDeadzone * 100.0f;
            if (ui_.slider("pedal-dz", R(c.x + 110, y, c.width - 190, 24), dzv, 0, 20, 1, w.enabled))
            {
                w.pedalDeadzone = dzv / 100.0f;
                markDirty();
            }
            ui_.text(strf("%d%%", static_cast<int>(std::lround(w.pedalDeadzone * 100))), c.x + c.width - 20, y + 2, 13.5f, t.muted, Weight::Regular, Align::Right);
            y += 32;
            ui_.text("GT4's Driving Force Pro has two pedals: a clutch pedal is not used.", c.x + 20, y, 12, t.faint);
        }
        // Force feedback
        {
            const float cy = y0 + p1h + kGap;
            const Rectangle c = R(rx, cy, rw, y0 + ch - cy);
            ui_.card(c, "Force feedback");
            float y = c.y + 50;
            if (ui_.toggle("ffb", c.x + 20, y, w.ffb, w.enabled))
                markDirty();
            ui_.text(w.ffb ? "On: GT4's forces reach the wheel" : "Off: GT4 gets the wheel's input only", c.x + 72, y + 1, 14, w.enabled ? t.text : t.faint);
            y += 34;
            const float cap = w.ffbAboveCap ? 1.0f : kFfbCap;
            const bool on = w.enabled && w.ffb;
            ui_.text("Strength", c.x + 20, y + 2, 14, on ? t.text : t.faint);
            float g = std::min(w.ffbGain, cap) * 100.0f;
            const Rectangle sr = R(c.x + 110, y, c.width - 190, 24);
            if (ui_.slider("ffb-gain", sr, g, 0, cap * 100.0f, 1, on, w.ffbGain > kFfbCap ? t.warn : Color{0, 0, 0, 0}))
            {
                w.ffbGain = g / 100.0f;
                markDirty();
            }
            ui_.text(strf("%d%%", static_cast<int>(std::lround(w.ffbGain * 100))), c.x + c.width - 20, y + 2, 14, t.text, Weight::Semibold, Align::Right);
            {
                const float tx = sr.x + sr.width * (kFfbFirstTest / cap);
                ui_.fill(R(tx - 0.5f, sr.y + 21, 1, 5), t.faint);
                ui_.text("10% first test", tx, sr.y + 26, 11, t.faint, Weight::Regular, Align::Center);
                if (!w.ffbAboveCap)
                    ui_.text("25% cap", sr.x + sr.width, sr.y + 26, 11, t.faint, Weight::Regular, Align::Right);
            }
            y += 48;
            if (ui_.checkbox("ffb-above", c.x + 20, y, w.ffbAboveCap, "Allow more than 25%", on))
            {
                if (!w.ffbAboveCap)
                    w.ffbGain = std::min(w.ffbGain, kFfbCap);
                markDirty();
            }
            if (ui_.button("ffb-10", R(c.x + c.width - 140, y - 4, 120, 28), "Use 10%", Style::Secondary, on, 12.5f))
            {
                w.ffbGain = kFfbFirstTest;
                markDirty();
            }
            y += 32;
            ui_.text("Invert direction", c.x + 20, y + 2, 13.5f, on ? t.text : t.faint);
            if (ui_.toggle("ffb-invert", c.x + c.width * 0.5f - 60, y, w.ffbInvert, on))
                markDirty();
            ui_.text("Pause when GT4 is not in front", c.x + c.width * 0.5f, y + 2, 13.5f, on ? t.text : t.faint);
            if (ui_.toggle("ffb-pause", c.x + c.width - 60, y, w.ffbPauseUnfocused, on))
                markDirty();
            y += 32;
            static const char *const effects[4] = {"Constant", "Spring", "Damper", "Friction"};
            const float ew = (c.width - 40) / 4.0f;
            for (int e = 0; e < 4; ++e)
            {
                bool v = w.effects[static_cast<size_t>(e)];
                if (ui_.checkbox(std::string("eff-") + effects[e], c.x + 20 + e * ew, y, v, effects[e], on))
                {
                    w.effects[static_cast<size_t>(e)] = v;
                    markDirty();
                }
            }
            y += 34;
            const bool testable = w.enabled && wd && wd->forceFeedback && !busy();
            const float bw = (c.width - 40 - 10) * 0.5f;
            if (ui_.button("test-spring", R(c.x + 20, y, bw, 34), "Test: centring spring", Style::Secondary, testable, 13))
                devices_.startFfbTest(*wd, FfbTest::CentreSpring, w.ffbGain, w.ffbAboveCap, ffbMessage_);
            if (ui_.button("test-push", R(c.x + 30 + bw, y, bw, 34), "Test: push left, right", Style::Secondary, testable, 13))
                devices_.startFfbTest(*wd, FfbTest::LeftRight, w.ffbGain, w.ffbAboveCap, ffbMessage_);
            y += 42;
            std::string note = ffbMessage_;
            if (note.empty())
                note = !wd ? "The tests need a force-feedback wheel plugged in." : hidden_ ? "The tests run only in a normal session (never in test mode)."
                                                                                           : "Hands on the wheel first. A direct-drive base is strong: start at 10% (the tests never go past 25%).";
            y += ui_.wrap(note, c.x + 20, y, c.width - 40, 12.5f, wd ? t.warn : t.faint);
            ui_.wrap("GT4's own Options has a force-feedback strength too; both apply.", c.x + 20, y + 2, c.width - 40, 12, t.faint);
        }
    }

    // ------------------------------------------------------------------ EyeToy camera
    // GT4_EYETOY (the EyeToy service, analysis/phase5b/eyetoy): GT4 looks for the camera on
    // GT Mode's USA page; with one, a Nike dealer appears, and its EyeToy screen gives the
    // Nike One 2022 when it recognises the GT shirt design (gtmode.adc, p5b-usb-eyetoy.md 4.2).
    void App::pageCamera(float x0, float y0, float cw, float ch)
    {
        const ui::Theme &t = ui::theme();
        const float lw = std::floor(cw * 0.52f);
        const float rx = x0 + lw + kGap, rw = x0 + cw - rx;
        const CameraList &cl = cameras_.list();

        // The choice
        {
            const Rectangle c = R(x0, y0, lw, ch);
            ui_.card(c, "EyeToy camera", cl.demo ? "Demo cameras" : "");
            float y = c.y + 52;
            struct Choice
            {
                EyeToy mode;
                const char *title;
                std::string text;
            };
            std::vector<Choice> choices = {
                {EyeToy::Off, "Off", "No camera, as a PS2 without an EyeToy. GT4 hides the Nike dealer until you have the car."},
                {EyeToy::Shirt, "Built-in GT shirt picture",
                 "No camera and no printout needed: GT4 sees the GT shirt design (read from your disc) and gives you the car a few seconds "
                 "after its EyeToy screen opens."},
                {EyeToy::Webcam, "Webcam", "The original way: hold the GT shirt design up to your webcam, printed or on a phone screen."},
            };
            if (st_.eyetoy == EyeToy::Image)
                choices.push_back({EyeToy::Image, "Picture file", "Set in the settings file: " + st_.eyetoyImage});
            for (const Choice &o : choices)
            {
                const float textW = c.width - 24 - 64;
                const float h = std::max(60.0f, 36.0f + ui_.wrapHeight(o.text, textW, 12.5f) + 8.0f);
                const Rectangle r = R(c.x + 12, y, c.width - 24, h);
                const bool sel = st_.eyetoy == o.mode;
                if (ui_.rowButton(std::string("eyetoy-") + o.title, r, sel) && !sel)
                {
                    st_.eyetoy = o.mode;
                    if (o.mode == EyeToy::Webcam)
                    {
                        resolveCamera();
                        lastCameraList_ = -100.0;
                    }
                    markDirty();
                }
                ui_.ring(r.x + 24, r.y + 22, 7.0f, 8.8f, sel ? t.accent : t.faint);
                if (sel)
                    ui_.circle(r.x + 24, r.y + 22, 4.4f, t.accent);
                ui_.text(o.title, r.x + 46, r.y + 11, 15, t.text, Weight::Semibold);
                ui_.wrap(o.text, r.x + 46, r.y + 35, textW, 12.5f, t.muted);
                y += h + 8;
            }

            // Which webcam: every camera Windows lists, except an infrared one.
            y += 8;
            ui_.fill(R(c.x + 20, y, c.width - 40, 1), t.border);
            y += 16;
            const bool web = st_.eyetoy == EyeToy::Webcam;
            std::vector<std::string> opts, names;
            for (const CameraInfo &cam : cl.devices)
                if (!cam.infrared)
                {
                    opts.push_back(cam.name);
                    names.push_back(cam.name);
                }
            int sel = -1;
            const std::string shown = !st_.eyetoyCamera.empty() ? st_.eyetoyCamera : [&] {
                const int d = defaultCamera(cl.devices);
                return d >= 0 ? cl.devices[static_cast<size_t>(d)].name : std::string();
            }();
            for (size_t i = 0; i < names.size(); ++i)
                if (iequals(names[i], shown))
                    sel = static_cast<int>(i);
            if (sel < 0 && !st_.eyetoyCamera.empty())
            {
                opts.push_back(st_.eyetoyCamera + "  (not connected)");
                names.push_back(st_.eyetoyCamera);
                sel = static_cast<int>(names.size()) - 1;
            }
            if (opts.empty())
            {
                opts.push_back(!cl.listed && cl.error.empty() ? "Looking for webcams..." : "No webcam found");
                names.push_back(std::string());
            }
            sel = std::max(sel, 0);
            ui_.text("Webcam", c.x + 20, y + 8, 14, web ? t.text : t.faint);
            if (ui_.dropdown("eyetoy-cam", R(c.x + 120, y, c.width - 140, 34), opts, sel, web && !names[static_cast<size_t>(sel)].empty()) &&
                !names[static_cast<size_t>(sel)].empty())
            {
                st_.eyetoyCamera = names[static_cast<size_t>(sel)];
                resolveCamera();
                markDirty();
            }
            y += 46;
            const CameraInfo *cam = chosenCamera();
            Color dot = t.faint;
            bool warn = false;
            std::string state;
            if (!cl.error.empty())
            {
                dot = t.warn;
                warn = true;
                state = "Windows' camera list cannot be read: " + cl.error + ".";
            }
            else if (!web)
                state = "Used only when Webcam is chosen. The Logitech BRIO is offered first.";
            else if (!cl.listed)
                state = "Looking for webcams...";
            else if (cam)
            {
                dot = t.ok;
                state = strf("Connected (%s). GT4 opens it as Windows camera #%d, only on its EyeToy screen.", cam->idText().c_str(), cam->index);
            }
            else
            {
                dot = t.warn;
                warn = true;
                state = "Not connected: GT4 gets no camera (and no Nike dealer) until it is plugged in.";
            }
            ui_.statusDot(c.x + 26, y + 9, dot);
            y += ui_.wrap(state, c.x + 40, y, c.width - 60, 13, warn ? t.warn : t.muted) + 8;
            const CameraPrivacy &priv = cameras_.privacy();
            if (priv.blocked())
            {
                ui_.statusDot(c.x + 26, y + 9, t.warn);
                y += ui_.wrap("Windows blocks the camera for desktop apps: Settings > Privacy & security > Camera.", c.x + 40, y, c.width - 60, 13, t.warn) + 8;
            }
            y += 4;
            y += ui_.wrap("Infrared (Windows Hello) cameras are never offered: GT4 needs a colour picture. Windows' camera list is only read here; "
                          "no camera is switched on.",
                          c.x + 20, y, c.width - 40, 12, t.faint);
            y += 14;
            ui_.fill(R(c.x + 20, y, c.width - 40, 1), t.border);
            y += 14;
            ui_.text("In the settings file", c.x + 20, y, 12.5f, t.faint);
            ui_.chip(c.x + 150, y - 4, "GT4_EYETOY=" + eyeToyValue(st_), t.panel3, t.text, 12.5f);
            if (st_.eyetoy == EyeToy::Webcam && !st_.eyetoyCamera.empty())
                ui_.text(ui_.fit("GT4L_EYETOY_CAMERA=" + st_.eyetoyCamera + " (the launcher's own: the camera's name)", c.width - 170, 12), c.x + 150,
                         y + 26, 12, t.faint);
        }

        // What it does
        {
            const Rectangle c = R(rx, y0, rw, ch);
            ui_.card(c, "What it does");
            float y = c.y + 52;
            y += ui_.wrap("GT4 used the PlayStation 2 EyeToy camera for one thing: unlocking the Nike One 2022 concept car by holding the design "
                          "printed on a special Gran Turismo T-shirt up to the camera. Pick \xe2\x80\x9c"
                          "Built-in GT shirt picture\xe2\x80\x9d to unlock it without a camera or a printout, or choose your webcam to do it "
                          "the original way. The camera is only used while GT4's EyeToy screen is open.",
                          c.x + 20, y, c.width - 40, 13.5f, t.text);
            y += 14;
            // The design itself, as printed.
            const float pw = std::min(250.0f, (c.width - 40) * 0.5f);
            if (shirtTex_.id)
            {
                const float ph = pw * static_cast<float>(shirtTex_.height) / static_cast<float>(shirtTex_.width);
                const Rectangle pr = R(c.x + 20, y, pw, ph);
                DrawTexturePro(shirtTex_, Rectangle{0, 0, static_cast<float>(shirtTex_.width), static_cast<float>(shirtTex_.height)}, ui_.px(pr),
                               Vector2{0, 0}, 0, WHITE);
                ui_.roundLine(pr, 2, 1, t.border);
                const float tx = c.x + 20 + pw + 16, tw = c.width - 40 - pw - 16;
                float ty = y + 2;
                ty += ui_.wrap("The GT shirt design GT4 looks for, read from your disc.", tx, ty, tw, 12.5f, t.muted);
                ty += 10;
                // Enabled as in a normal session (the test modes give it no input, and
                // saveShirtPrint() does nothing there).
                if (ui_.button("save-shirt", R(tx, ty, std::min(tw, 170.0f), 32), "Save it to print...", Style::Secondary, true, 12.5f))
                    saveShirtPrint();
                ty += 40;
                ui_.wrap("Print it with its black border, or show it full screen on a phone.", tx, ty, tw, 12, t.faint);
                y += std::max(ph, ty - y + 30) + 16;
            }
            else
            {
                y += ui_.wrap("The GT shirt design: " + shirtError_, c.x + 20, y, c.width - 40, 12.5f, t.faint) + 14;
            }
            ui_.text("How to get the car", c.x + 20, y, 14, t.text, Weight::Semibold);
            y += 26;
            const char *const steps[] = {
                "In GT Mode, open the USA page. GT4 checks for a camera there; with the built-in picture or a connected webcam, a Nike "
                "dealer appears.",
                "Choose the Nike dealer: GT4 finds the camera and opens its EyeToy screen.",
                "Built-in picture: just wait. Webcam: hold the design facing the camera so it fills about two thirds of the picture's width, "
                "with its black border showing, and keep it still for a few seconds.",
                "\xe2\x80\x9cYou got a car\xe2\x80\x9d: the Nike One 2022 is in your garage, and the Nike dealer stays on the USA page.",
            };
            int n = 1;
            for (const char *s : steps)
            {
                ui_.text(std::to_string(n++), c.x + 22, y, 13, t.accent, Weight::Bold);
                y += ui_.wrap(s, c.x + 40, y, c.width - 60, 13, t.muted) + 6;
            }
            y += 6;
            ui_.wrap("The original shirt's print reflected the EyeToy's light, which GT4 checks for; GT4 Recompilation adds that reflection, "
                     "so a printout or a phone screen works too.",
                     c.x + 20, y, c.width - 40, 12, t.faint);
        }
    }

    // ------------------------------------------------------------------ Saves
    void App::pageSaves()
    {
        if (savesView_ == 1 && detailsOpen_)
        {
            pageSaveDetails();
            return;
        }
        const ui::Theme &t = ui::theme();
        drawHeader("Saves", "GT4's memory cards and its photo folder. Back the cards up any time; restore asks first.");
        const float x0 = kSidebarW + 32, x1 = ui_.width - 32, cw = x1 - x0, bottom = ui_.height - 48;
        const float lw = std::floor(cw * 0.56f);
        const float rx = x0 + lw + kGap, rw = x1 - rx;
        if (busy())
            ui_.chip(x1 - 270, 34, "GT4 is running: backup and restore wait", ui::alpha(t.warn, 0.18f), t.warn, 12.5f);

        // A save: GT4's own in a box (with its read-only Save details), the others in one line.
        auto saveBox = [&](const Rectangle &box, bool present, const SaveEntry &s, int slot) {
            ui_.round(box, 10, t.panel2);
            if (present)
            {
#if defined(GT4L_EDITION_FULL)
                // The Full edition: one page, Game progress and save details (Overview, GT Mode, Arcade, with Change name and
                // Repair; developer, 2026-10-19: Save details only repeated it).
                const float pw = 262;
                const Rectangle pr = R(box.x + box.width - 12 - pw, box.y + (box.height - 34) * 0.5f, pw, 34);
                ui_.roundLine(pr, 8, 1, ui::alpha(t.accent, 0.6f));
                if (ui_.iconButton("progress-" + std::to_string(slot), pr, Icon::Trophy, "Game progress and save details", Style::Ghost, true, 13))
                {
                    st_.full.garageCard = slot;
                    savesProgress_ = true;
                    progressTab_ = 0;
                    markDirty();
                    loadGarage(true);
                }
                const float room = pw; // the text stops before the button
#else
                const float bwid = 128;
                const float room = bwid;
#endif
                ui_.icon(Icon::Card, R(box.x + 14, box.y + (box.height - 28) * 0.5f, 28, 28), t.accent);
                ui_.text(ui_.fit(s.title.empty() ? std::string("Gran Turismo 4 game data") : s.title, box.width - 54 - room - 20, 15, Weight::Semibold),
                         box.x + 54, box.y + 11, 15, t.text, Weight::Semibold);
#if defined(GT4L_EDITION_FULL)
                // A save holding the ESSO (analysis/jp-cars): the line says so, in the warning colour when GT4 would not know the car.
                if (!savesExtra_[static_cast<size_t>(slot - 1)].slots.empty())
                {
                    const bool served = extraCarsServed();
                    const std::string w = served ? "Holds the ESSO Supra '01: keep extra-cars\esso.gt4x" : "Holds the ESSO Supra '01: no esso.gt4x pack now";
                    ui_.icon(Icon::Warn, R(box.x + 54, box.y + 34, 14, 14), served ? t.muted : t.warn);
                    ui_.text(ui_.fit(w, box.width - 54 - room - 40, 12.5f), box.x + 74, box.y + 34, 12.5f, served ? t.muted : t.warn);
                }
                else
#endif
                ui_.text(ui_.fit(strf("%s \xc2\xb7 %s \xc2\xb7 changed %s", kGt4SaveFolder, sizeText(s.bytes).c_str(), s.when.c_str()), box.width - 54 - room - 20,
                                 12.5f),
                         box.x + 54, box.y + 34, 12.5f, t.muted);
#if !defined(GT4L_EDITION_FULL)
                // A view, not an action: an outlined button beside "Back up now".
                const Rectangle br = R(box.x + box.width - bwid - 12, box.y + (box.height - 34) * 0.5f, bwid, 34);
                ui_.roundLine(br, 8, 1, ui::alpha(t.accent, 0.6f));
                if (ui_.button("details-" + std::to_string(slot), br, "Save details  \xe2\x86\x92", Style::Ghost, true, 13))
                    openDetails(slot);
#endif
            }
            else
            {
                ui_.icon(Icon::Card, R(box.x + 14, box.y + (box.height - 28) * 0.5f, 28, 28), t.faint);
                ui_.wrap("No GT4 save yet: GT4 makes one the first time you save in the game.", box.x + 54, box.y + 13, box.width - 70, 13, t.muted);
            }
        };
        auto others = [&](const std::vector<SaveEntry> &saves, float x, float y, float w) {
            std::vector<std::string> names;
            for (const SaveEntry &s : saves)
                if (!s.gt4)
                    names.push_back((s.title.empty() ? s.folder : s.title) + (s.gt3 ? " (GT4 reads it)" : "")); // the save's own title: no GT3 wording of ours (PLAN)
            if (!names.empty())
                ui_.text(ui_.fit("Also on this card: " + join(names, ", "), w, 12.5f), x, y, 12.5f, t.muted);
        };

#if defined(GT4L_EDITION_FULL)
        // A save holding the ESSO (an extra car, analysis/jp-cars): GT4 knows it only with extra-cars\esso.gt4x next to a Full
        // GT4.exe. Without it (the Classic GT4, or the pack removed) GT4 still loads the save, but the car has no name or
        // model there (README-BUILD.md, the hidden runs with an unknown car code).
        auto extraWarn = [&](int slot, float x, float y, float w, float maxY) {
            const SaveExtraCars &xc = savesExtra_[static_cast<size_t>(slot - 1)];
            if (xc.slots.empty() || y + 14 > maxY)
                return;
            const bool served = extraCarsServed();
            const std::string what =
                std::string("This save has the Toyota ESSO Ultraflo Supra '01") + (xc.riding ? " (the car you drive)" : "") + ", an extra car. ";
            const std::string text =
                served ? what + "Only GT4 with extra-cars\\esso.gt4x next to it knows the car: without the pack it is a blank car with wrong "
                                "numbers, so do not drive it there."
                       : what + "GT4 does not know it now (extra-cars\\esso.gt4x is not next to GT4.exe, or is off): it shows as a blank car "
                                "with wrong numbers. Put the pack back before you play this save.";
            const Color col = served ? t.muted : t.warn;
            ui_.icon(Icon::Warn, R(x, y, 16, 16), col);
            if (y + ui_.wrapHeight(text, w - 22, 12) <= maxY)
                ui_.wrap(text, x + 22, y, w - 22, 12, col);
            else
                ui_.text(ui_.fit(text, w - 22, 12), x + 22, y, 12, col);
        };
#endif
        auto cardBox = [&](const Rectangle &c, int slot) {
            const CardInfo &folder = cards_[slot - 1];
            const CardFileInfo &file = cardFiles_[slot - 1];
            const bool isFile = cardIsFile(slot);
            const bool chosenFile = st_.cards[static_cast<size_t>(slot - 1)].file;
            ui_.card(c, strf("Memory card slot %d", slot));
            // Folder card (slot 1) or empty (slot 2), or a .ps2 card file.
            int kind = chosenFile ? 1 : 0;
            if (ui_.segmented("mc-kind-" + std::to_string(slot), R(c.x + c.width - 250, c.y + 12, 230, 30),
                              {slot == 1 ? "Folder card" : "Empty", "Card file (.ps2)"}, kind, !busy(), 12.5f))
            {
                st_.cards[static_cast<size_t>(slot - 1)].file = kind == 1;
                if (kind == 1 && st_.cards[static_cast<size_t>(slot - 1)].path.empty())
                    st_.cards[static_cast<size_t>(slot - 1)].path = strf("Mcd00%d.ps2", slot); // game\Mcd001.ps2, as PCSX2 names them
                markDirty();
                refreshCards();
            }
            float y = c.y + 52;
            const float buttonsY = c.y + c.height - 46;
            if (!chosenFile && slot == 2)
            {
                y += ui_.wrap("No memory card in slot 2, like a PS2 with one card. To put one in, choose \"Card file (.ps2)\".", c.x + 20, y,
                              c.width - 40, 13, t.muted);
                return;
            }
            if (!isFile)
            {
                ui_.icon(Icon::Folder, R(c.x + 20, y, 20, 20), t.muted);
                ui_.text(ui_.fit(shortPath(folder.dir, inst_.root), c.width - 200, 14, Weight::Regular, true), c.x + 48, y + 1, 14, t.text);
                if (ui_.button("open-mc" + std::to_string(slot), R(c.x + c.width - 140, y - 6, 120, 32), "Open folder", Style::Secondary, folder.exists))
                    sys::openInExplorer(folder.dir);
                y += 26;
                ui_.text(folder.exists && folder.files ? strf("Folder card \xc2\xb7 %s of 8 MB \xc2\xb7 %zu files", sizeText(folder.bytes).c_str(), folder.files)
                                                       : std::string(folder.exists ? "Folder card \xc2\xb7 empty" : "Folder card \xc2\xb7 made at GT4's first save"),
                         c.x + 48, y, 12.5f, t.muted);
                y += 24;
                saveBox(R(c.x + 20, y, c.width - 40, 60), folder.gt4Save, folder.gt4, slot);
                y += 68;
                others(folder.saves, c.x + 20, y, c.width - 40);
#if defined(GT4L_EDITION_FULL)
                if (folder.gt4Save)
                    extraWarn(slot, c.x + 20, y + 22, c.width - 40, buttonsY - 6);
#endif
            }
            else
            {
                const fs::path f = cardFile(slot);
                ui_.icon(Icon::Card, R(c.x + 20, y, 20, 20), t.muted);
                ui_.text(ui_.fit(shortPath(f, inst_.root), c.width - 200, 14, Weight::Regular, true), c.x + 48, y + 1, 14, t.text);
                if (ui_.button("choose-mc" + std::to_string(slot), R(c.x + c.width - 140, y - 6, 120, 32), "Choose...", Style::Secondary, !busy()))
                {
                    const fs::path p = sys::browseForFile(GetWindowHandle(), L"A PS2 memory card file (a new name makes a new card)",
                                                          std::wstring(L"PS2 memory cards (*.ps2)\0*.ps2\0All files\0*.*\0\0", 45), f, false);
                    const int other = slot == 1 ? 2 : 1;
                    const auto full = [](const fs::path &q) {
                        std::error_code aec;
                        return pathText(fs::absolute(q, aec).lexically_normal());
                    };
                    if (!p.empty() && cardIsFile(other) && iequals(full(p), full(cardFile(other))))
                    {
                        // GT4.exe (ps2-card) refuses one file in both slots: slot 2 would stay empty.
                        setStatus(strf("Slot %d already uses that card file: choose another one for slot %d.", other, slot), t.warn);
                    }
                    else if (!p.empty())
                    {
                        std::string chosen = shortPath(p, inst_.gameDir); // relative inside game\, as GT4.exe takes it
                        st_.cards[static_cast<size_t>(slot - 1)].path = chosen;
                        markDirty();
                        refreshCards();
                    }
                }
                y += 26;
                std::string state;
                Color stateColor = t.muted;
                if (!file.exists)
                    state = "A new, formatted card is made when GT4 first uses it";
                else if (!file.card)
                {
                    state = "Not usable: " + file.problem + ". GT4 will not use it";
                    stateColor = t.warn;
                }
                else
                    state = strf("PS2 card (PCSX2 format) \xc2\xb7 %s KB free of 8,000", groupedNumber(static_cast<uint64_t>(file.freeKB)).c_str());
                ui_.text(ui_.fit(state, c.width - 70, 12.5f), c.x + 48, y, 12.5f, stateColor);
                y += 24;
                if (file.card)
                {
                    saveBox(R(c.x + 20, y, c.width - 40, 60), file.gt4Save, file.gt4, slot);
                    y += 68;
                    others(file.saves, c.x + 20, y, c.width - 40);
#if defined(GT4L_EDITION_FULL)
                    if (file.gt4Save)
                        extraWarn(slot, c.x + 20, y + 22, c.width - 40, buttonsY - 6);
#endif
                }
                else
                    ui_.wrap("Card files need a GT4 program that supports them (the ps2-card change); an older one keeps using the folder card.",
                             c.x + 20, y + 4, c.width - 40, 12, t.faint);
            }
            const bool exists = isFile ? file.exists : (folder.exists && folder.files > 0);
            if (ui_.iconButton("backup-" + std::to_string(slot), R(c.x + 20, buttonsY, 170, 34), Icon::Card, "Back up now", Style::Primary,
                               !busy() && exists, 13.5f))
                backup(slot);
            std::vector<BackupInfo> mine;
            for (const BackupInfo &b : backups_)
                if (b.slot == slot && b.cardFile == isFile)
                    mine.push_back(b);
            if (ui_.button("restore-" + std::to_string(slot), R(c.x + 200, buttonsY, 150, 34), "Restore...", Style::Secondary, !busy() && !mine.empty(), 13.5f))
            {
                restoreZip_ = mine.front().file;
                restoreSlot_ = slot;
                modal_ = Modal::Restore;
            }
            ui_.text(mine.empty() ? std::string("no backups yet") : ui_.fit("latest " + timeText(mine.front().time), c.width - 380, 12.5f), c.x + 362,
                     buttonsY + 9, 12.5f, t.faint);
        };
        const float usbH = 166;
        const float slotH = std::floor((bottom - kTop - usbH - 2 * kGap) * 0.5f);
        cardBox(R(x0, kTop, lw, slotH), 1);
        cardBox(R(x0, kTop + slotH + kGap, lw, slotH), 2);
        // USB stick: where GT4's photos go (changeable: GT4_USB_ROOT).
        {
            const float cy = kTop + 2 * (slotH + kGap);
            const Rectangle c = R(x0, cy, lw, bottom - cy);
            ui_.card(c, "Photos (GT4's USB stick)");
            if (ui_.toggle("usb", c.x + c.width - 60, c.y + 16, st_.usb))
                markDirty();
            ui_.text("plugged in", c.x + c.width - 70, c.y + 18, 12.5f, st_.usb ? t.muted : t.faint, Weight::Regular, Align::Right);
            float y = c.y + 52;
            ui_.icon(Icon::Folder, R(c.x + 20, y, 20, 20), t.muted);
            ui_.text(ui_.fit(shortPath(photos_.dir, inst_.root), c.width - 330, 14, Weight::Regular, true), c.x + 48, y + 1, 14, st_.usb ? t.text : t.faint);
            const float bx = c.x + c.width - 20;
            if (ui_.button("open-photos", R(bx - 100, y - 6, 100, 32), "Open", Style::Secondary, photos_.exists, 12.5f))
                sys::openInExplorer(photos_.dir);
            if (ui_.button("usb-default", R(bx - 196, y - 6, 88, 32), "Default", Style::Secondary, !st_.usbRoot.empty() && !busy(), 12.5f))
            {
                st_.usbRoot.clear();
                markDirty();
                refreshCards();
            }
            if (ui_.button("usb-change", R(bx - 302, y - 6, 98, 32), "Change...", Style::Secondary, !busy(), 12.5f))
            {
                const fs::path p = sys::browseForFolder(GetWindowHandle(), L"Where GT4's photos go (its USB stick; they land in a GT4 folder inside)", usbRoot());
                if (!p.empty())
                {
                    st_.usbRoot = pathText(p);
                    markDirty();
                    refreshCards();
                }
            }
            y += 30;
            const std::string info = photos_.exists ? strf("%zu photo%s \xc2\xb7 %s%s", photos_.count, photos_.count == 1 ? "" : "s", sizeText(photos_.bytes).c_str(),
                                                            photos_.newest.empty() ? "" : (" \xc2\xb7 newest " + photos_.newest).c_str())
                                                      : std::string("No photos yet");
            ui_.text(ui_.fit(info + (st_.usbRoot.empty() ? "  (the default, game\\usb0\\GT4)" : "  (your folder)"), c.width - 68, 12.5f), c.x + 48, y, 12.5f,
                     t.muted);
            y += 26;
            // The runtime builds GT4's 128 MB stick from the whole folder (gt4_ustorage.cpp).
            ui_.wrap("GT4 saves Photo Mode pictures to its USB stick as JPEG files; they land here. If you change the folder, pick an empty one: "
                     "GT4 sees everything in it as its 128 MB stick.",
                     c.x + 20, y, c.width - 40, 12, t.faint);
        }
        // Backups
        {
            const Rectangle c = R(rx, kTop, rw, bottom - kTop);
            ui_.card(c, "Backups");
            if (ui_.button("open-backups", R(c.x + c.width - 140, c.y + 12, 120, 30), "Open folder", Style::Secondary, true, 12.5f))
            {
                std::error_code ec;
                fs::create_directories(backupDir(), ec);
                sys::openInExplorer(backupDir());
            }
            float y = c.y + 54;
            ui_.text(ui_.fit(shortPath(backupDir(), inst_.root), c.width - 40, 12.5f, Weight::Regular, true), c.x + 20, y, 12.5f, t.faint);
            y += 26;
            const float rowH = 58;
            const int maxRows = std::max(1, static_cast<int>((c.y + c.height - y - 70) / rowH));
            if (backups_.empty())
                y += ui_.wrap("No backups yet. \"Back up now\" saves a folder card as a zip file, and a card file as a dated copy, here.", c.x + 20, y,
                              c.width - 40, 13, t.muted);
            int n = 0;
            for (const BackupInfo &b : backups_)
            {
                if (n++ >= maxRows)
                    break;
                const Rectangle r = R(c.x + 12, y, c.width - 24, rowH - 6);
                ui_.round(r, 8, ui::alpha(t.panel2, 0.6f));
                ui_.icon(b.cardFile ? Icon::Card : Icon::Folder, R(r.x + 12, r.y + 14, 22, 22), b.beforeRestore ? t.faint : t.muted);
                ui_.text(ui_.fit(pathText(b.file.filename()), r.width - 170, 13.5f, Weight::Semibold), r.x + 46, r.y + 8, 13.5f, t.text, Weight::Semibold);
#if defined(GT4L_EDITION_FULL)
                // The Full edition's Garage backs the card up before each change ("-before-edit").
                const char *made = b.beforeRestore ? " \xc2\xb7 before a restore"
                                   : lower(pathText(b.file.filename())).find("before-edit") != std::string::npos ? " \xc2\xb7 before a Garage change"
                                                                                                                 : "";
#else
                const char *made = b.beforeRestore ? " \xc2\xb7 before a restore" : "";
#endif
                ui_.text(ui_.fit(strf("slot %d \xc2\xb7 %s \xc2\xb7 %s \xc2\xb7 %s%s", std::max(1, b.slot), b.cardFile ? "card file" : "folder card",
                                      sizeText(b.bytes).c_str(), timeText(b.time).c_str(), made),
                                 r.width - 170, 12),
                         r.x + 46, r.y + 29, 12, t.muted);
                if (ui_.button("restore-" + pathText(b.file.filename()), R(r.x + r.width - 104, r.y + 10, 92, 32), "Restore", Style::Secondary, !busy(), 12.5f))
                {
                    restoreZip_ = b.file;
                    restoreSlot_ = std::clamp(b.slot, 1, 2);
                    modal_ = Modal::Restore;
                }
                y += rowH;
            }
            ui_.wrap("A restore first backs up the card as it is (a folder card is also moved to backups\\replaced). Nothing is deleted.", c.x + 20,
                     c.y + c.height - 52, c.width - 40, 12, t.faint);
        }
    }

    // ------------------------------------------------------------------ About
    void App::pageAbout()
    {
        if (aboutView_ == 1)
        {
            pageHowItWorks();
            return;
        }
        const ui::Theme &t = ui::theme();
#if defined(GT4L_EDITION_FULL)
        // developer (2026-10-08): it says it is a work in progress, and who designed it.
        drawHeader("About", strf("%s: a work in progress (WIP). Designed by F1NATIK.", kProductName)); // (no edition name: developer 2026-10-20)
#else
        drawHeader("About", strf("%s, %s edition: the settings, controllers, saves and cheats in one place.", kProductName, kEdition));
#endif
        const float x0 = kSidebarW + 32, x1 = ui_.width - 32, cw = x1 - x0, bottom = ui_.height - 48;
        float top = kTop;

        // How it works: the short version here, the whole text on its own page.
        {
            const std::string brief = howDoc_.shortVersion().empty()
                                          ? std::string("GT4 Recompilation runs Gran Turismo 4 as a native Windows program, translated ahead of time from the PS2's code.")
                                          : howDoc_.shortVersion();
            const float bw = 190;
            const float textW = cw - 40 - bw - 24;
            const float h = std::floor(46 + ui_.wrapHeight(brief, textW, 13.5f, Weight::Regular, 1.4f) + 12);
            const Rectangle c = R(x0, top, cw, h);
            ui_.round(c, 12, t.panel);
            ui_.roundLine(c, 12, 1, ui::alpha(t.border, 0.8f));
            ui_.round(R(c.x, c.y + 14, 4, c.height - 28), 2, t.accent);
            ui_.text("How it works", c.x + 20, c.y + 14, 16.5f, t.text, Weight::Semibold);
            ui_.wrap(brief, c.x + 20, c.y + 42, textW, 13.5f, ui::mixColor(t.text, t.muted, 0.2f), Weight::Regular, 1.4f);
            if (ui_.button("how-open", R(c.x + c.width - 20 - bw, c.y + (c.height - 38) * 0.5f, bw, 38), "Read how it works  \xe2\x86\x92", Style::Primary, true,
                           13.5f))
            {
                aboutView_ = 1;
                howJump_ = -1;
                ui_.resetScroll("how");
            }
            top += h + kGap;
        }

        // Is it a full recompilation? The answer from the text, highlighted: the headline and the
        // function counts on the left, the key points on the right.
        if (recomp_.found)
        {
            const float inner = cw - 40;
            const float lzw = std::floor(std::clamp(inner * 0.45f, 360.0f, 540.0f));
            const float opW = 24;
            const float tileW = std::floor((lzw - 2 * opW) / 3);
            const std::string values[3] = {recomp_.total, recomp_.translated, recomp_.replaced};
            const char *const labels[3] = {"functions in GT4's main program", "statically recompiled (translated ahead of time)",
                                           "Sony system routines, replaced by native code"};
            float labelH = 0;
            for (const char *l : labels)
                labelH = std::max(labelH, ui_.wrapHeight(l, tileW - 20, 11.5f, Weight::Regular, 1.3f));
            const float tileH = std::floor(9 + 22 * 1.33f + 2 + labelH + 9);
            const float leftH = 16 + 24 + 32 + 12 + tileH;
            const float pzx = x0 + 20 + lzw + 30, pzw = x1 - 20 - pzx;
            float pointsH = 0;
            for (const RecompAnswer::Point &p : recomp_.points)
                pointsH += ui_.wrapHeight(p.text, pzw - 28, 12.5f, Weight::Regular, 1.4f) + 7;
            const float h = std::floor(std::max(leftH, 18 + pointsH) + 14);
            const Rectangle c = R(x0, top, cw, h);
            const Color fill = ui::mixColor(t.panel, t.accent, 0.10f), edge = ui::mixColor(t.panel, t.accent, 0.5f);
            ui_.round(c, 12, fill);
            ui_.roundLine(c, 12, 1.2f, edge);
            ui_.round(R(c.x, c.y + 14, 4, c.height - 28), 2, t.accent);
            float y = c.y + 15;
            ui_.text(recomp_.question, c.x + 20, y, 13.5f, Color{150, 188, 255, 255}, Weight::Semibold);
            y += 24;
            ui_.circle(c.x + 20 + 15, y + 16, 15, t.ok);
            ui_.icon(Icon::Check, R(c.x + 20 + 3, y + 4, 24, 24), t.panel);
            ui_.text(recomp_.headline, c.x + 20 + 42, y + (32 - 21 * 1.33f) * 0.5f, 21, t.text, Weight::Bold);
            y += 32 + 12;
            for (int i = 0; i < 3; ++i)
            {
                const float tx = c.x + 20 + static_cast<float>(i) * (tileW + opW);
                const Rectangle tile = R(tx, y, tileW, tileH);
                ui_.round(tile, 9, ui::alpha(t.panel, 0.9f));
                ui_.roundLine(tile, 9, 1, i == 0 ? edge : ui::alpha(t.border, 0.9f));
                ui_.text(values[i], tx + 10, y + 9, 22, i == 0 ? Color{150, 188, 255, 255} : t.text, Weight::Bold);
                ui_.wrap(labels[i], tx + 10, y + 9 + 22 * 1.33f + 2, tileW - 20, 11.5f, t.muted, Weight::Regular, 1.3f);
                if (i < 2)
                    ui_.text(i == 0 ? "=" : "+", tx + tileW + opW * 0.5f, y + (tileH - 20 * 1.33f) * 0.5f, 20, t.muted, Weight::Semibold, Align::Center);
            }
            ui_.fill(R(pzx - 15, c.y + 18, 1, c.height - 36), ui::alpha(edge, 0.6f));
            float py = c.y + 17;
            for (const RecompAnswer::Point &p : recomp_.points)
            {
                const float cx = pzx + 9, cy = py + 9;
                if (p.kind == 0)
                {
                    ui_.circle(cx, cy, 9, ui::mixColor(t.panel, t.ok, 0.22f));
                    ui_.icon(Icon::Check, R(cx - 7, cy - 7, 14, 14), t.ok);
                }
                else
                {
                    const Color dot = p.kind == 1 ? t.warn : t.accent;
                    ui_.circle(cx, cy, 9, ui::mixColor(t.panel, dot, 0.2f));
                    ui_.circle(cx, cy, 2.6f, dot);
                }
                py += ui_.wrap(p.text, pzx + 28, py, pzw - 28, 12.5f, p.kind == 0 ? t.text : ui::mixColor(t.text, t.muted, 0.3f), Weight::Regular, 1.4f) + 7;
            }
            top += h + kGap;
        }

        // Three columns below: this build, this GT4 (with its folders), the keys and the notice.
        const float room = bottom - top;
        const float roomy = std::clamp((room - 330.0f) / 160.0f, 0.0f, 1.0f); // 0 at the smallest size
        const float colW = std::floor((cw - 2 * kGap) / 3);
        const float c1x = x0, c2x = x0 + colW + kGap, c3x = x0 + 2 * (colW + kGap), c3w = x1 - c3x;
        const float rowStep = 21 + 4 * roomy, buttonStep = std::max(rowStep, 25.0f + 3 * roomy);
        auto labelWidth = [&](std::initializer_list<const char *> labels) {
            float w = 0;
            for (const char *l : labels)
                w = std::max(w, ui_.textWidth(l, 12.5f));
            return std::ceil(w + 14);
        };
        // One row: the label, the value (wrapped, or a path shortened in the middle) and an Open button.
        auto row = [&](const Rectangle &c, float &y, float labelW, const std::string &k, const std::string &v, bool path = false,
                       const fs::path *open = nullptr) {
            ui_.text(k, c.x + 18, y + 1, 12.5f, t.faint);
            const float vx = c.x + 18 + labelW;
            float vw = c.x + c.width - 18 - vx;
            float step = rowStep;
            if (open)
            {
                vw -= 64;
                step = buttonStep;
                std::error_code ec;
                if (ui_.button("open-" + k, R(c.x + c.width - 18 - 56, y - 3, 56, 22), "Open", Style::Secondary, fs::exists(*open, ec), 12))
                    sys::openInExplorer(*open);
            }
            if (path || ui_.textWidth(v, 13.5f) <= vw)
                ui_.text(ui_.fit(v, vw, 13.5f, Weight::Regular, path), vx, y, 13.5f, t.text);
            else
                step += ui_.wrap(v, vx, y, vw, 13.5f, t.text, Weight::Regular, 1.3f) - 13.5f * 1.3f;
            y += step;
        };
        {
            const Rectangle c = R(c1x, top, colW, bottom - top);
            ui_.card(c, kProductName);
            float y = c.y + 50;
            const float lw = labelWidth({"Edition", "Version", "Built", "Source", "Compiler", "Libraries", "Graphics", "Scanline preview"});
#if defined(GT4L_EDITION_FULL)
            // (no Edition row: one launcher, developer 2026-10-20 "stop labling thing FULL, and Classic")
#else
            row(c, y, lw, "Edition", strf("%s: GT4 as it was, plus the scanline filter and extras you turn on (Cheats page)", kEdition));
#endif
            row(c, y, lw, "Version", std::string(kVersion));
            row(c, y, lw, "Built", std::string(__DATE__) + " " + __TIME__);
            row(c, y, lw, "Source", GT4STOCK_COMMIT);
#if defined(_MSC_VER)
            row(c, y, lw, "Compiler", strf("MSVC %d.%d (%d)", _MSC_VER / 100, _MSC_VER % 100, _MSC_FULL_VER));
#endif
            row(c, y, lw, "Libraries", std::string("raylib ") + RAYLIB_VERSION + " with GLFW; DirectInput 8");
            // The adapter GT4.exe runs on (its own request, Windows' setting, DXGI's list), not the launcher's.
            row(c, y, lw, "Graphics", gpu_.label());
#if defined(GT4L_EDITION_FULL)
            row(c, y, lw, "Picture preview", preview_.ready() ? std::string("the preview shader (compiled)") : std::string("plain picture (the shader did not compile)"));
#else
            row(c, y, lw, "Scanline preview", preview_.ready() ? strf("the %s edition's scanline shader (compiled)", kEdition) : std::string("plain picture (the shader did not compile)"));
#endif
        }
        {
            const Rectangle c = R(c2x, top, colW, bottom - top);
            ui_.card(c, "This GT4");
            float y = c.y + 50;
            const float lw = labelWidth({"Layout", "Folder", "Program", "CORE.elf", "Disc", "Settings", "Logs", "Cards", "Backups", "Photos"});
            std::error_code ec;
            const uint64_t exeBytes = inst_.exeFound ? fs::file_size(inst_.exe, ec) : 0;
            const fs::path settingsDir = inst_.settingsFile.parent_path(), logs = logFile().parent_path(), backups = backupDir(),
                           photos = usbRoot() / "GT4";
            row(c, y, lw, "Layout", inst_.layout + (inst_.package ? " (started without arguments)" : " (CORE.elf passed, settings as variables)"));
            row(c, y, lw, "Folder", pathText(inst_.root), true, &inst_.root);
            row(c, y, lw, "Program", inst_.exeFound ? shortPath(inst_.exe, inst_.root) + " \xc2\xb7 " + sizeText(exeBytes) : std::string("not found"), true);
            row(c, y, lw, "CORE.elf",
                inst_.coreFound ? strf("%s \xc2\xb7 CRC-32 %08x%s", sizeText(inst_.coreBytes).c_str(), inst_.coreCrc, inst_.coreCrc == 0x19257b64u ? " (the build's)" : "")
                                : std::string("not found"));
            row(c, y, lw, "Disc", disc_.exists ? (disc_.sizeMatches && disc_.volumeMatches ? "Gran Turismo 4 (USA), SCUS-97328" : "found (unexpected size or volume)") : "not found");
            row(c, y, lw, "Settings", shortPath(inst_.settingsFile, inst_.root), true, &settingsDir);
            row(c, y, lw, "Logs", shortPath(logs, inst_.root), true, &logs);
            row(c, y, lw, "Cards", shortPath(inst_.gameDir, inst_.root), true, &inst_.gameDir);
            row(c, y, lw, "Backups", shortPath(backups, inst_.root), true, &backups);
            row(c, y, lw, "Photos", shortPath(photos, inst_.root), true, &photos);
        }
#if defined(GT4L_EDITION_FULL)
        const float keyStep = ui_.height < 900 ? 22.0f : 26 + 4 * roomy; // five rows; the notice below needs the room on short windows
#else
        const float keyStep = 26 + 4 * roomy;
#endif
        const bool cardKey = gt4_.card; // F2: only with a GT4 program that has the in-game card
#if defined(GT4L_EDITION_FULL)
        const float h1 = std::floor(50 + (cardKey ? 4 : 3) * keyStep + 6); // the card key, Esc, Close, Options (no F1: no TV scanlines)
#else
        const float h1 = std::floor(50 + (cardKey ? 4 : 3) * keyStep);
#endif
        {
            const Rectangle c = R(c3x, top, c3w, h1);
            ui_.card(c, "In the game");
            float y = c.y + 50;
            auto key = [&](const std::string &k, const std::string &what) {
                const float w = ui_.chip(c.x + 18, y - 3, k, t.panel3, t.text, 12.0f);
                ui_.text(ui_.fit(what, c.width - 48 - w, 13), c.x + 30 + w, y, 13, t.muted);
                y += keyStep;
            };
#if !defined(GT4L_EDITION_FULL)
            key("F1", "Scanlines on and off");
#endif
            if (cardKey)
                key(st_.cheats.key, "The cheat card, when it is on (Cheats page)");
            key("Esc", "Leaves full screen (Alt+Enter: back)");
            key("Close", "The PS2 power button: GT4 quits cleanly");
#if defined(GT4L_EDITION_FULL)
            key("Options", "GT4's own: units, force strength");
#else
            key("Options", "GT4's own: widescreen, units, force strength");
#endif
        }
        {
            const float cy = top + h1 + kGap;
            const Rectangle c = R(c3x, cy, c3w, bottom - cy);
            ui_.card(c, "Notice and credits");
            float y = c.y + 48;
#if defined(GT4L_EDITION_FULL)
            // The Full edition's notice is longer (the Vulkan renderer's licences) and its "In the game" card one row taller:
            // the text gets a little smaller where the card is short (1280 x 800), so it stays inside the card.
            const std::string notice = "An unofficial, fan-made native Windows recompilation for personal use. Gran Turismo is a trademark of Sony "
                                       "Interactive Entertainment; this project is not affiliated with Sony or Polyphony Digital.";
            const std::string licences = "It needs your own copy of Gran Turismo 4 (USA). Licences: GT4.exe is built on the open-source PS2Recomp (GPLv3); "
                                         "raylib and GLFW (zlib); GT4's Vulkan renderer uses the Vulkan headers, volk and glslang (their own open "
                                         "licences); the text is Windows' Segoe UI.";
            const std::string designer = "Designed by F1NATIK \xc2\xb7 a work in progress (WIP)"; // developer, 2026-10-08
            float ns = 12.5f, textTop = 48.0f;
            auto needs = [&](float sz, float t0) {
                return t0 + (sz + 1.0f) * 1.3f + 6 + ui_.wrapHeight(notice, c.width - 36, sz, Weight::Regular, 1.3f) + 6 +
                       ui_.wrapHeight(licences, c.width - 36, sz, Weight::Regular, 1.3f) + 8;
            };
            while (ns > 10.0f && needs(ns, textTop) > c.height)
                ns -= 0.5f;
            if (needs(ns, textTop) > c.height)
                textTop = 40.0f; // the title closer to the text
            y = c.y + textTop;
            ui_.text(ui_.fit(designer, c.width - 36, ns + 1.0f, Weight::Semibold), c.x + 18, y, ns + 1.0f, t.accent, Weight::Semibold);
            y += (ns + 1.0f) * 1.3f + 6;
            y += ui_.wrap(notice, c.x + 18, y, c.width - 36, ns, t.text, Weight::Regular, 1.3f);
            y += 6;
            ui_.wrap(licences, c.x + 18, y, c.width - 36, ns, t.muted, Weight::Regular, 1.3f);
#else
            y += ui_.wrap("An unofficial, fan-made native Windows recompilation for personal use. Gran Turismo is a trademark of Sony Interactive "
                          "Entertainment; this project is not affiliated with Sony or Polyphony Digital.",
                          c.x + 18, y, c.width - 36, 12.5f, t.text, Weight::Regular, 1.3f);
            y += 6;
            ui_.wrap("It needs your own copy of Gran Turismo 4 (USA). Licences: GT4.exe is built on the open-source PS2Recomp (GPLv3); raylib and "
                     "GLFW (zlib); the text is Windows' Segoe UI.",
                     c.x + 18, y, c.width - 36, 12.5f, t.muted, Weight::Regular, 1.3f);
#endif
        }
    }

    // ------------------------------------------------------------------ dialogs
    void App::drawModal()
    {
        const ui::Theme &t = ui::theme();
        ui_.fill(R(0, 0, ui_.width, ui_.height), ui::alpha(BLACK, 0.55f));
        auto panel = [&](float w, float h) {
            const Rectangle r = R(std::floor((ui_.width - w) * 0.5f), std::floor((ui_.height - h) * 0.5f), w, h);
            ui_.round(R(r.x + 3, r.y + 6, r.width, r.height), 14, ui::alpha(BLACK, 0.4f));
            ui_.round(r, 14, t.panel);
            ui_.roundLine(r, 14, 1, t.border);
            return r;
        };
        const bool esc = IsKeyPressed(KEY_ESCAPE);
        if (modal_ == Modal::Restore)
        {
            const Rectangle r = panel(620, 270);
            ui_.icon(Icon::Warn, R(r.x + 28, r.y + 28, 26, 26), t.warn);
            ui_.text(strf("Restore memory card slot %d?", restoreSlot_), r.x + 66, r.y + 28, 20, t.text, Weight::Bold);
            float y = r.y + 76;
            const CardInfo &card = cards_[restoreSlot_ - 1];
            const bool file = cardIsFile(restoreSlot_);
            y += ui_.wrap((file ? "The card file " + pathText(cardFile(restoreSlot_).filename()) + " is replaced with " : std::string("The card's files are replaced with the ones in ")) +
                              pathText(restoreZip_.filename()) + ".",
                          r.x + 28, y, r.width - 56, 14, t.text);
            y += 10;
            const SaveEntry &save = file ? cardFiles_[restoreSlot_ - 1].gt4 : card.gt4;
            const bool hasSave = file ? cardFiles_[restoreSlot_ - 1].gt4Save : card.gt4Save;
            const std::string now = std::string("The card as it is now") + (hasSave ? " (GT4 save from " + save.when + ")" : std::string()) +
                                    (file ? " is copied to the backups first. Nothing is deleted." : " is backed up first and then moved to backups\\replaced. Nothing is deleted.");
            y += ui_.wrap(now, r.x + 28, y, r.width - 56, 13.5f, t.muted);
            y += 8;
            ui_.wrap("GT4 must be closed while the card changes.", r.x + 28, y, r.width - 56, 13, busy() ? t.bad : t.faint);
            if (ui_.button("restore-cancel", R(r.x + r.width - 324, r.y + r.height - 60, 140, 38), "Cancel", Style::Secondary) || esc)
                modal_ = Modal::None;
            if (ui_.button("restore-go", R(r.x + r.width - 168, r.y + r.height - 60, 140, 38), "Restore", Style::Danger, !busy()))
            {
                modal_ = Modal::None;
                restore(restoreZip_, restoreSlot_);
            }
        }
        else if (modal_ == Modal::Calibrate)
        {
            PedalSettings &p = calibratePedal_ == 0 ? st_.wheel.gas : st_.wheel.brake;
            const DeviceInfo *src = pedalDevice() ? pedalDevice() : wheelDevice();
            const int ai = axisIndex(p.axis);
            const int raw = src && ai >= 0 ? src->axes[static_cast<size_t>(ai)] : 0;
            if (!hidden_)
            {
                if (!calibrateHaveRest_)
                {
                    calibrateRest_ = raw;
                    calibrateFar_ = raw;
                    calibrateHaveRest_ = true;
                }
                if (std::abs(raw - calibrateRest_) > std::abs(calibrateFar_ - calibrateRest_))
                    calibrateFar_ = raw;
            }
            const Rectangle r = panel(600, 330);
            ui_.text(strf("Calibrate the %s pedal", calibratePedal_ == 0 ? "gas" : "brake"), r.x + 28, r.y + 26, 20, t.text, Weight::Bold);
            float y = r.y + 68;
            y += ui_.wrap("1. Leave the pedal released while this opens.\n2. Press it all the way down once, then let it go.\n3. Save.", r.x + 28, y,
                          r.width - 56, 14, t.text);
            y += 14;
            ui_.text(strf("%s of %s", wheelAxisLabel(p.axis), src ? src->name.c_str() : "?"), r.x + 28, y, 13, t.faint);
            y += 24;
            // The axis' whole range, with the pressed part lit from the "released" mark to
            // where the pedal is now (a pedal axis often runs from +32767 released down to
            // -32768: lit from the left edge, the bar would look full with the pedal up).
            const Rectangle bar = R(r.x + 28, y, r.width - 56, 12);
            const auto at = [&](int v) { return bar.x + bar.width * ((static_cast<float>(v) + 32768.0f) / 65535.0f); };
            ui_.round(bar, bar.height * 0.5f, t.panel3);
            {
                const float a = at(calibrateRest_), b = at(raw);
                const float lo = std::min(a, b), hi = std::max(a, b);
                if (hi - lo > 0.5f)
                    ui_.round(R(lo, bar.y, std::max(bar.height, hi - lo), bar.height), bar.height * 0.5f, t.accent);
            }
            const auto mark = [&](int v, const char *label) {
                const float mx = at(v);
                ui_.fill(R(mx - 1, y - 4, 2, 20), t.text);
                ui_.text(label, mx, y + 18, 11.5f, t.muted, Weight::Regular, Align::Center);
            };
            mark(calibrateRest_, "released");
            mark(calibrateFar_, "full");
            y += 46;
            const bool enough = std::abs(calibrateFar_ - calibrateRest_) > 8000;
            ui_.text(strf("Now %d \xc2\xb7 released %d \xc2\xb7 full %d", raw, calibrateRest_, calibrateFar_), r.x + 28, y, 13.5f, enough ? t.ok : t.muted);
            if (ui_.button("cal-auto", R(r.x + 28, r.y + r.height - 60, 190, 38), "Automatic instead", Style::Secondary))
            {
                p.calibrated = false;
                markDirty();
                modal_ = Modal::None;
            }
            if (ui_.button("cal-cancel", R(r.x + r.width - 324, r.y + r.height - 60, 140, 38), "Cancel", Style::Secondary) || esc)
                modal_ = Modal::None;
            if (ui_.button("cal-save", R(r.x + r.width - 168, r.y + r.height - 60, 140, 38), "Save", Style::Primary, enough))
            {
                p.calibrated = true;
                p.released = calibrateRest_;
                p.full = calibrateFar_;
                markDirty();
                modal_ = Modal::None;
            }
        }
        else if (modal_ == Modal::WheelButtons)
        {
            const Rectangle r = panel(760, 640);
            ui_.text("Wheel buttons", r.x + 28, r.y + 24, 20, t.text, Weight::Bold);
            ui_.text("Each wheel button gives a Driving Force Pro button in GT4. Press one to see which it is.", r.x + 28, r.y + 56, 13.5f, t.muted);
            const DeviceInfo *wd = wheelDevice();
            std::vector<std::string> names(kWheelButtonLabels, kWheelButtonLabels + kWheelButtonNames);
            const float colW = (r.width - 56 - 20) * 0.5f;
            for (int b = 0; b < 24; ++b)
            {
                const int col = b / 12, row = b % 12;
                const float x = r.x + 28 + col * (colW + 20), y = r.y + 92 + row * 36;
                const bool down = wd && wd->buttons[static_cast<size_t>(b)];
                ui_.statusDot(x + 8, y + 15, down ? t.ok : t.panel3);
                ui_.text(strf("Wheel button %d", b + 1), x + 22, y + 6, 13.5f, t.text);
                int v = std::clamp(st_.wheel.buttons[static_cast<size_t>(b)], 0, kWheelButtonNames - 1);
                if (ui_.dropdown("wb-" + std::to_string(b), R(x + colW - 170, y, 170, 30), names, v, true, 13))
                {
                    st_.wheel.buttons[static_cast<size_t>(b)] = v;
                    markDirty();
                }
            }
            if (ui_.button("wb-reset", R(r.x + 28, r.y + r.height - 56, 230, 36), "Driving Force Pro layout", Style::Secondary))
            {
                st_.wheel.buttons = WheelSettings().buttons;
                markDirty();
            }
            if (ui_.button("wb-done", R(r.x + r.width - 168, r.y + r.height - 56, 140, 36), "Done", Style::Primary) || esc)
                modal_ = Modal::None;
        }
        else if (modal_ == Modal::CheatBig)
            drawCheatBig();
        else if (modal_ == Modal::CheatSave)
            drawCheatSave();
#if defined(GT4L_EDITION_FULL)
        else if (modal_ == Modal::GarageConfirm)
            drawGarageEditModal(); // the Garage's changes (gt4full_garageedit.cpp)
        else if (modal_ == Modal::GarageResult)
            drawGarageDoneModal();
        else if (modal_ == Modal::CarSpecs)
            drawCarSpecsModal();
        else if (modal_ == Modal::GarageCutOff)
            drawGarageCutOffModal();
        else if (modal_ == Modal::RenameSave)
            drawRenameModal();
#endif
        else if (modal_ == Modal::Message)
        {
            const Rectangle r = panel(520, 220);
            ui_.text(messageTitle_, r.x + 28, r.y + 26, 20, t.text, Weight::Bold);
            ui_.wrap(messageText_, r.x + 28, r.y + 66, r.width - 56, 14, t.muted);
            if (ui_.button("msg-ok", R(r.x + r.width - 168, r.y + r.height - 60, 140, 38), "OK", Style::Primary) || esc)
                modal_ = Modal::None;
        }
    }
}
