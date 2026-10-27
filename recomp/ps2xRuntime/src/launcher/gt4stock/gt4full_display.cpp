// GT4 Recompilation launcher, Full edition: the Display page (FULL-LAUNCHER-PLAN.md 2.2, the mock-up's six tabs) and its
// preview (1.8: the split view, full screen, side-by-side zoom with an adjustable level that follows the mouse).
// What each control writes is in gt4full_settings.cpp; the developer's "not for now" items have no control here at all.

#include "gt4stock_app.h"

#include "gt4full_ui.h"
#include "gt4stock_util.h"

#include <algorithm>
#include <cmath>

namespace gt4stock
{
    using ui::Align;
    using ui::Icon;
    using ui::Style;
    using ui::Weight;
    using fullui::Pill;

    namespace
    {
        constexpr float kSidebarW = 232.0f;
        constexpr float kTop = 104.0f;
        constexpr float kGap = 16.0f;
        Rectangle R(float x, float y, float w, float h) { return Rectangle{x, y, w, h}; }
        const char *const kTabNames[9] = {"Picture", "Resolution", "Screen", "Graphics", "Texture packs", "HUD", "Cameras", "Upscaler", "Lighting"};
    }

    std::vector<int> App::displayTabsShown() const
    {
        // Detail and textures and HUD hold choices GT4.exe may not have: a release build hides them until its GT4 program has
        // them (their markers). Resolution is shown with a GT4 program whose internal resolution was checked with GT4 (full-m2,
        // Resolution).
        std::vector<int> v{0};
        if (showUnbuilt_ || (full_.read && full_.internalRes))
            v.push_back(1);
        v.push_back(3); // (no Screen tab: its Display card is on Resolution, developer 2026-10-19)
        // Cameras (full-m2, Cameras): with a GT4 program that has the race cameras (its marker).
        if (showUnbuilt_ || (full_.read && full_.cameras))
            v.push_back(6);
        // Detail and textures (full-m2, Detail): with a GT4 program that has car and wheel detail (its marker); a release
        // build then shows only the live Cars card.
        if (showUnbuilt_ || (full_.read && full_.texDump))
            v.push_back(4);
        // (The HUD tab, only a pointer to Driving > Race HUD, is gone: developer 2026-10-19.)
        return v;
    }

    int App::previewFilterMode() const
    {
        // The preview shader's modes (gt4stock_scanline.cpp; the Full edition's shader adds 4 bicubic and 5 Lanczos-3).
        switch (st_.full.scaling)
        {
        case FullScaling::Nearest: return scan::kFilterNearest;
        case FullScaling::Bicubic: return 4;
        case FullScaling::Lanczos: return 5;
        default: return scan::kFilterSmooth; // Bilinear (and the scalers not built yet, which give GT4.exe bilinear)
        }
    }

    // ------------------------------------------------------------------ the page
    void App::pageDisplayFull()
    {
        drawHeader("Display", "The picture, the screen and how GT4 draws it. Changes are saved as you make them.");
        const float x0 = kSidebarW + 32, x1 = ui_.width - 32, bottom = ui_.height - 48;
        // A resolution preset sets the internal resolution for the monitor GT4 opens on (every tab shows it: Graphics too).
        if ((!full_.read || full_.internalRes) && st_.full.resPreset != 0)
        {
            const MonitorInfo *mon = selectedMonitor();
            const int want = fullPresetScale(st_.full.resPreset, mon ? mon->h : 0);
            if (want >= 1 && st_.full.internalRes != want)
            {
                st_.full.internalRes = want;
                markDirty();
            }
        }
        const std::vector<int> shown = displayTabsShown();
        if (std::find(shown.begin(), shown.end(), st_.full.displayTab) == shown.end())
            st_.full.displayTab = 0;
        std::vector<std::string> labels;
        int sel = 0;
        for (size_t i = 0; i < shown.size(); ++i)
        {
            labels.push_back(kTabNames[shown[i]]);
            if (shown[i] == st_.full.displayTab)
                sel = static_cast<int>(i);
        }
        if (fullui::tabs(ui_, "display-tabs", x0, kTop - 10, x1 - x0, labels, sel))
        {
            st_.full.displayTab = shown[static_cast<size_t>(sel)];
            ui_.closePopups();
            markDirty();
        }
        const float y0 = kTop + 40;
        switch (st_.full.displayTab)
        {
        case 1: displayResolution(x0, y0, x1, bottom); break;
        case 2: displayShape(x0, y0, x1, bottom); break;
        case 3: displaySmoothness(x0, y0, x1, bottom); break;
        case 4: displayDetail(x0, y0, x1, bottom); break;
        case 5: displayHud(x0, y0, x1, bottom); break;
        case 6: displayCameras(x0, y0, x1, bottom); break;
        default: displayPicture(x0, y0, x1, bottom); break;
        }
    }

    // ------------------------------------------------------------------ Picture
    void App::displayPicture(float x0, float y0, float x1, float y1)
    {
        const ui::Theme &t = ui::theme();
        FullSettings &f = st_.full;
        const bool dev = showUnbuilt_;
        const float cw = x1 - x0;
        const float lw = std::floor(std::min(480.0f, cw * 0.46f));
        const Rectangle view = R(x0, y0, lw + 10, y1 - y0);
        const float off = ui_.beginScroll("pic-left", view);
        float y = y0 - off;
        const float w = lw;

        // Video mode and colour (the 480p update's keys). Races are 480p; GT4's menus, car select and garage stay its own 480i
        // (analysis/design/480p-colour DESIGN.md D5). With a GT4 program that does not read these keys the card is greyed
        // (development builds) or left out (release builds), and Play does not pass them (adjustFullLaunch).
        const bool exeLacks = full_.read && !full_.has480p();
        if (dev || !exeLacks)
        {
            const std::string what = "Races, replays, menus, car select and the garage in 480p: whole pictures, no interlacing. GT4's logos and "
                                     "2-player races keep its own 480i.";
            const std::string colourHint = f.colour == 16 ? "As the PS2 drew its races: 16-bit colour with its fine dithering pattern."
                                                          : "GT4's 16-bit race pictures drawn in full colour: smooth shades, no dithering.";
            const float wh = ui_.wrapHeight(what, w - 40, 12.5f), ch = ui_.wrapHeight(colourHint, w - 40, 12.5f);
            const float h = 52 + 24 + wh + 16 + 34 + 8 + ch + 4 + (dev && exeLacks ? 20 : 0) + 14;
            const Rectangle c = R(x0, y, w, h);
            fullui::card(ui_, c, "Video mode", {Pill::Full});
            float yy = c.y + 50;
            ui_.text("Races and menus in 480p", c.x + 20, yy, 15, exeLacks ? t.faint : t.text, Weight::Semibold);
            yy += 24;
            yy += ui_.wrap(what, c.x + 20, yy, w - 40, 12.5f, exeLacks ? t.faint : t.muted) + 16;
            ui_.text("Race colour", c.x + 20, yy + 8, 14, exeLacks ? t.faint : t.text);
            int colour = f.colour == 16 ? 1 : 0;
            if (ui_.segmented("colour", R(c.x + c.width - 20 - 300, yy, 300, 34), {"32-bit (full colour)", "16-bit (PS2 dithering)"}, colour, !exeLacks, 12.5f))
            {
                f.colour = colour == 1 ? 16 : 32;
                markDirty();
            }
            yy += 42;
            yy += ui_.wrap(colourHint, c.x + 20, yy, w - 40, 12.5f, t.faint) + 4;
            if (dev && exeLacks)
                fullui::notBuilt(ui_, c.x + 20, yy, w - 40, "the GT4 program here does not read the video mode and colour yet (the 480p update).");
            y += h + 12;
        }
        // Scaling
        {
            struct Choice
            {
                FullScaling s;
                const char *hint;
            };
            static const Choice choices[] = {{FullScaling::Nearest, "hard-edged pixels"},
                                             {FullScaling::Bilinear, "smooth (the default)"},
                                             {FullScaling::Bicubic, "smooth with crisper edges"},
                                             {FullScaling::Lanczos, "the sharpest of the smooth filters"}};
            // Bicubic and Lanczos need a GT4 program that reads them (the 480p update): otherwise greyed (development builds)
            // or left out (release builds), and Play gives that GT4 program Bilinear (adjustFullLaunch).
            auto live = [&](FullScaling sc) {
                return fullScalingBuilt(sc) && !(full_.read && ((sc == FullScaling::Bicubic && !full_.bicubic) || (sc == FullScaling::Lanczos && !full_.lanczos)));
            };
            int rows = 0;
            for (const Choice &ch : choices)
                rows += live(ch.s) || dev;
            const char *scale = fullScalingLabel(live(f.scaling) ? f.scaling : FullScaling::Bilinear);
            const bool exeOld = full_.read && ((f.scaling == FullScaling::Bicubic && !full_.bicubic) || (f.scaling == FullScaling::Lanczos && !full_.lanczos));
            const float h = 52 + rows * 32 + 10 + (dev && exeOld ? 20 : 0) + 14;
            const Rectangle c = R(x0, y, w, h);
            fullui::card(ui_, c, "Scaling", {Pill::ClassicToo});
            float yy = c.y + 48;
            for (const Choice &ch : choices)
            {
                const bool built = live(ch.s);
                if (!built && !dev)
                    continue;
                if (fullui::radio(ui_, std::string("scale-") + fullScalingKey(ch.s), R(c.x + 10, yy, c.width - 20, 30), f.scaling == ch.s, fullScalingLabel(ch.s),
                                  ch.hint, built, !built))
                {
                    f.scaling = ch.s;
                    st_.filter = ch.s == FullScaling::Nearest ? PictureFilter::Nearest : PictureFilter::Smooth;
                    markDirty();
                }
                yy += 32;
            }
            yy += 10;
            if (dev && exeOld)
                fullui::notBuilt(ui_, c.x + 20, yy + 2, w - 40, "the GT4 program here has no " + std::string(scale) + " yet: Play gives it Bilinear.");
            y += h + 12;
        }
        // Colour (analysis/design/colour-lut): a colour LUT on the picture, before the TV lines; off by default. Live with a GT4
        // program that has it (its marker, full_.lut); otherwise greyed (development builds) or left out (release builds).
        if (dev || !(full_.read && !full_.lut))
        {
            const bool lutLacks = full_.read && !full_.lut;
            static const char *const hints[3] = {
                "GT4's own colours, as it drew them (the default).",
                "GT3's colours on GT4: matched from GT3 and GT4 race pictures. A little darker in the middle tones, shadows a "
                "touch lifted and cooler, slightly less colourful.",
                "Your own colour grade: a 3D .cube LUT file, as Resolve, Photoshop and other tools make them."};
            const int choice = std::clamp(f.lut, 0, 2);
            const std::string hint = hints[choice];
            const float hh = ui_.wrapHeight(hint, w - 40, 12.5f);
            const float h = 48 + 34 + 8 + hh + 10 + (choice == 2 ? 44 : 0) + 56 + (dev && lutLacks ? 20 : 0) + 10;
            const Rectangle c = R(x0, y, w, h);
            fullui::card(ui_, c, "Colour", {Pill::Full});
            float yy = c.y + 48;
            int sel = choice;
            if (ui_.segmented("lut", R(c.x + 20, yy, c.width - 40, 34), {"Off", "GT3 look", "Your own .cube file"}, sel, !lutLacks, 12.5f))
            {
                f.lut = sel;
                markDirty();
            }
            yy += 42;
            yy += ui_.wrap(hint, c.x + 20, yy, w - 40, 12.5f, t.faint) + 10;
            if (choice == 2)
            {
                // The file box: the chosen .cube file (GT4 gets no LUT until one is chosen) and Browse.
                const float bw = 104;
                const Rectangle box = R(c.x + 20, yy, c.width - 40 - bw - 10, 34);
                ui_.fill(box, ui::alpha(t.faint, 0.12f));
                const std::string shown = f.lutFile.empty() ? std::string("No file chosen: GT4 gets no LUT") : f.lutFile;
                ui_.text(ui_.fit(shown, box.width - 20, 12.5f, Weight::Regular, true), box.x + 10, box.y + 9, 12.5f, f.lutFile.empty() ? t.faint : t.text);
                if (ui_.button("lut-browse", R(box.x + box.width + 10, yy, bw, 34), "Browse...", Style::Secondary, !lutLacks))
                {
                    static const wchar_t filter[] = L"Colour LUTs (*.cube)\0*.cube\0All files\0*.*\0";
                    const fs::path p = sys::browseForFile(GetWindowHandle(), L"A colour LUT (.cube file)", std::wstring(filter, std::size(filter)),
                                                          pathFrom(f.lutFile));
                    if (!p.empty())
                    {
                        f.lutFile = pathText(p);
                        markDirty();
                    }
                }
                yy += 44;
            }
            const bool strengthOn = !lutLacks && choice != 0;
            ui_.text("Strength", c.x + 20, yy, 14, strengthOn ? t.text : t.faint);
            ui_.text(strf("%d%%", f.lutStrength), c.x + c.width - 20, yy + 1, 14, t.muted, Weight::Regular, Align::Right);
            float lv = static_cast<float>(f.lutStrength);
            if (ui_.slider("lut-strength", R(c.x + 24, yy + 24, c.width - 48, 24), lv, 0, 100, 1, strengthOn))
            {
                f.lutStrength = static_cast<int>(std::lround(lv));
                markDirty();
            }
            yy += 56;
            if (dev && lutLacks)
                fullui::notBuilt(ui_, c.x + 20, yy, w - 40, "the GT4 program here has no colour LUTs yet.");
            y += h + 12;
        }
        // (Other video modes, 480i / 1080i races, deinterlacing and flicker: gone from the Full edition, developer 2026-10-19 "no one will
        // use them". Races and menus are 480p; GT4's logos and 2-player races keep their 480i with the default Blend.)
        ui_.endScroll("pic-left", y + off - y0);

        // The preview (right)
        {
            const float rx = x0 + lw + kGap + 6;
            const Rectangle c = R(rx, y0, x1 - rx, y1 - y0);
            ui_.card(c, "Preview");
            int pic = st_.preview == "menu" ? 1 : st_.preview == "title" ? 2 : 0;
            if (ui_.segmented("preview-pic", R(c.x + c.width - 236, c.y + 12, 216, 30), {"Race", "Menu", "Title"}, pic, true, 12.5f))
            {
                st_.preview = pic == 1 ? "menu" : pic == 2 ? "title" : "race";
                markDirty();
            }
            const float inner = c.width - 32;
            const float zoomRowH = 40;
            // The caption under the zoom panes: wrapped (two lines at most at the launcher's sizes).
            const MonitorInfo *mon = selectedMonitor();
            int capW = 0, capH = 0;
            const scan::Placement pl = fullPreviewPlacement(mon, capW, capH);
            const bool race480p = pic == 0 && !f.gameVideoModes && !(full_.read && !full_.raceMode);
            const std::string shape = f.aspect == FullAspect::Normal ? "4:3" : f.aspect == FullAspect::SquarePixels ? "4:3, square pixels" : "16:9";
            const bool colour16 = f.colour == 16 && !(full_.read && !full_.raceColour);
            const std::string what = pic == 0 ? std::string(race480p ? "a 480p race" : "a race") + (colour16 ? " in 16-bit colour (PS2 dithering)" : " in full colour")
                                              : std::string(pic == 1 ? "a 480i menu" : "the 480i title") + ", deinterlaced (" +
                                                    (st_.deinterlace == Deinterlace::Blend      ? "Blend"
                                                     : st_.deinterlace == Deinterlace::Adaptive ? "Adaptive, shown still"
                                                     : st_.deinterlace == Deinterlace::Bob      ? "Bob"
                                                                                                 : "Weave") +
                                                    ")";
            const std::string cap = strf("Real pixels (1:1) as GT4 draws %s %s %s: %s. Click the picture for full screen; the zoom follows the mouse.",
                                         what.c_str(), st_.fullscreen ? "full screen on" : "in a window on", mon ? mon->name.c_str() : "this screen", shape.c_str());
            const float capH2 = std::min(ui_.wrapHeight(cap, c.width - 40, 12), 34.0f);
            const float avail = c.height - 56 - (capH2 + 18) - zoomRowH - 12;
            const float splitH = std::floor(std::clamp(avail * 0.50f, 120.0f, 520.0f));
            const Rectangle pr = R(c.x + 16, c.y + 54, inner, splitH);
            drawFullPreview(pr);
            float yy = pr.y + pr.height + 10;
            // Zoom level (slider or mouse wheel) and full screen.
            ui_.text("Zoom", c.x + 20, yy + 6, 13.5f, t.text);
            float z = static_cast<float>(f.previewZoom);
            if (ui_.slider("preview-zoom", R(c.x + 70, yy + 4, std::max(80.0f, inner - 250), 22), z, 1, 8, 1))
            {
                f.previewZoom = static_cast<int>(std::lround(z));
                markDirty();
            }
            ui_.text(strf("%d\xc3\x97", f.previewZoom), c.x + 70 + std::max(80.0f, inner - 250) + 12, yy + 6, 13.5f, t.muted);
            if (ui_.iconButton("preview-full", R(c.x + c.width - 16 - 128, yy, 128, 32), Icon::Eye, "Full screen", Style::Secondary, true, 13))
            {
                previewFullscreen_ = true;
                previewJustOpened_ = true;
            }
            yy += zoomRowH;
            const Rectangle zr = R(c.x + 16, yy, inner, c.y + c.height - (capH2 + 18) - yy);
            drawZoomPanes(zr);
            ui_.wrap(cap, c.x + 20, c.y + c.height - capH2 - 10, c.width - 40, 12, t.faint);
            // The mouse wheel over the preview: the zoom level.
            const Vector2 m = ui_.mouseDp();
            if (ui_.inputOn() && !ui_.popupOpen() && ui_.input().wheel != 0.0f && CheckCollisionPointRec(m, R(c.x, pr.y, c.width, c.y + c.height - pr.y)))
            {
                f.previewZoom = std::clamp(f.previewZoom + (ui_.input().wheel > 0 ? 1 : -1), 1, 8);
                markDirty();
            }
        }
    }

    float App::fullAspectRatio() const
    {
        // The picture's shape GT4 shows: its 16:9 (GT4_ASPECT, the default) or 4:3; square pixels: the 640 x 448 frame's own shape.
        const FullAspect a = st_.full.aspect;
        return a == FullAspect::Normal ? 4.0f / 3.0f : a == FullAspect::SquarePixels ? 640.0f / 448.0f : 16.0f / 9.0f;
    }

    scan::Placement App::fullPreviewPlacement(const MonitorInfo *m, int &ww, int &wh) const
    {
        // The window (or screen) the preview stands for, kept to a render target's size: across several monitors the
        // picture keeps its height-limited size in the middle, so a narrower target shows the same pixels.
        fullPlacement(m, &ww, &wh);
        ww = std::clamp(ww, 320, 4096);
        wh = std::clamp(wh, 224, 4096);
        return scan::fitPicture(static_cast<float>(ww), static_cast<float>(wh), scan::kLines, fullAspectRatio(), st_.fit);
    }

    void App::prepareFullPreview()
    {
        // The Classic preview targets with the Full edition's filter (the window GT4 would show, lines on and off), plus the
        // same picture without the picture settings (nearest, no lines, no blend) for the zoom's left pane. It follows the
        // Full settings: the screen shape (16:9 by default), the race colour (16-bit: GT4's dithering, below) and the video
        // mode (a 480p race is a whole picture: no deinterlacing; the menu and title pictures are GT4's 480i).
        const MonitorInfo *m = selectedMonitor();
        int ww = 0, wh = 0;
        const scan::Placement pl = fullPreviewPlacement(m, ww, wh);
        const int pic = st_.preview == "menu" ? 1 : st_.preview == "title" ? 2 : 0;
        const int filter = previewFilterMode();
        // What the GT4 program here does: a 480p race and the race colour only with the 480p update (full_, its strings).
        const bool race16 = pic == 0 && st_.full.colour == 16 && !(full_.read && !full_.raceColour);
        const bool progressive = pic == 0 && !st_.full.gameVideoModes && !(full_.read && !full_.raceMode);
        const std::string key = strf("full|%d|%d|%d|%d|%dx%d|%d|%d|%.4f|%d|%d", pic, st_.strength, static_cast<int>(st_.fit), preview_.ready() ? 1 : 0, ww, wh, filter,
                                     static_cast<int>(st_.deinterlace), fullAspectRatio(), race16 ? 1 : 0, progressive ? 1 : 0);
        if (key == previewKey_)
            return;
        previewKey_ = key;
        if (race16 && race16_.id == 0 && pictures_[0].id)
        {
            // GT4's 16-bit race picture: the full-colour race frame with the GS dither GT4 sets for the copy into its 16-bit
            // display buffer (one of its two DIMX matrices, 0x0667022307760332, analysis/tb2-fixes/dither/README.md 1;
            // DM[y & 3][x & 3] added, clamped (COLCLAMP 1), then each channel cut to 5 bits: gs_dither.h).
            Image img = LoadImageFromTexture(pictures_[0]);
            ImageFormat(&img, PIXELFORMAT_UNCOMPRESSED_R8G8B8A8);
            auto *px = static_cast<unsigned char *>(img.data);
            constexpr uint64_t kDimx = 0x0667022307760332ull;
            for (int y = 0; y < img.height; ++y)
                for (int x = 0; x < img.width; ++x)
                {
                    const int v = static_cast<int>((kDimx >> ((y & 3) * 16 + (x & 3) * 4)) & 7u);
                    const int d = v >= 4 ? v - 8 : v;
                    for (int ch = 0; ch < 3; ++ch)
                    {
                        unsigned char &c = px[(static_cast<size_t>(y) * img.width + x) * 4 + ch];
                        c = static_cast<unsigned char>(std::clamp(c + d, 0, 255) & 0xF8);
                    }
                }
            race16_ = LoadTextureFromImage(img);
            UnloadImage(img);
        }
        const Texture2D &base = race16 && race16_.id ? race16_ : pictures_[pic];
        const Texture2D &tex = !progressive && st_.deinterlace == Deinterlace::Bob && picturesBob_[pic].id && !race16 ? picturesBob_[pic] : base;
        const bool blend = !progressive && st_.deinterlace == Deinterlace::Blend;
        preview_.renderWindowAt(tex, pictureW_[pic], pictureH_[pic], ww, wh, pl, 0.0f, false, previewOn_, filter, blend); // (no TV lines)
        preview_.renderWindowAt(tex, pictureW_[pic], pictureH_[pic], ww, wh, pl, st_.strength / 100.0f, false, previewOff_, filter, blend);
        preview_.renderWindowAt(pictures_[pic], pictureW_[pic], pictureH_[pic], ww, wh, pl, 0.0f, false, previewRaw_, scan::kFilterNearest, false);
        SetTextureFilter(previewOn_.texture, TEXTURE_FILTER_POINT);
        SetTextureFilter(previewOff_.texture, TEXTURE_FILTER_POINT);
        SetTextureFilter(previewRaw_.texture, TEXTURE_FILTER_POINT);
    }

    void App::drawFullPreview(Rectangle r)
    {
        const ui::Theme &t = ui::theme();
        if (previewOn_.id == 0)
            return;
        const MonitorInfo *m = selectedMonitor();
        int pww = 0, pwh = 0;
        const scan::Placement pl = fullPreviewPlacement(m, pww, pwh);
        const Rectangle rp = ui_.px(r);
        const float texW = static_cast<float>(previewOn_.texture.width), texH = static_cast<float>(previewOn_.texture.height);
        const int pic = st_.preview == "menu" ? 1 : st_.preview == "title" ? 2 : 0;
        const float fx = pic == 0 ? 0.30f : 0.45f, fy = pic == 0 ? 0.22f : 0.47f;
        float sx = std::round(pl.x + pl.w * fx - rp.width * 0.5f);
        float sy = std::round(pl.y + pl.h * fy - rp.height * 0.5f);
        sx = std::clamp(sx, 0.0f, std::max(0.0f, texW - rp.width));
        sy = std::clamp(sy, 0.0f, std::max(0.0f, texH - rp.height));
        const float cw = std::min(rp.width, texW), chh = std::min(rp.height, texH);
        auto blit = [&](const RenderTexture2D &rt, float from, float to) {
            if (to <= from)
                return;
            const Rectangle src{sx + from, texH - (sy + chh), to - from, -chh};
            DrawTextureRec(rt.texture, src, Vector2{rp.x + from, rp.y}, WHITE);
        };
        DrawRectangleRec(rp, BLACK);
        blit(previewOn_, 0, cw);
        // The zoom's spot, outlined (its size: the zoom panes' pixels / the zoom level).
        const float spot = std::max(4.0f, 150.0f * ui_.s / static_cast<float>(std::max(1, st_.full.previewZoom)));
        const float zx = pl.x + pl.w * zoomFocus_.x, zy = pl.y + pl.h * zoomFocus_.y;
        const float ox = (zx - sx - spot * 0.5f) / ui_.s + r.x, oy = (zy - sy - spot * 0.5f) / ui_.s + r.y;
        if (ox + spot / ui_.s > r.x && ox < r.x + r.width && oy + spot / ui_.s > r.y && oy < r.y + r.height)
            ui_.roundLine(R(std::max(ox, r.x), std::max(oy, r.y), std::min(spot / ui_.s, r.x + r.width - std::max(ox, r.x)),
                            std::min(spot / ui_.s, r.y + r.height - std::max(oy, r.y))),
                          2, 1.5f, ui::alpha(t.accent, 0.95f));
        // A click on the picture fills the window (Esc back, launcher 1.5); the zoom follows the mouse.
        const Vector2 mouse = ui_.mouseDp();
        const bool over = ui_.inputOn() && CheckCollisionPointRec(mouse, r);
        if (over && ui_.input().pressed)
        {
            previewFullscreen_ = true;
            previewJustOpened_ = true; // this press is not the full-screen view's "click goes back"
        }
        if (over && pl.w > 0 && pl.h > 0)
        {
            const float wx = sx + (mouse.x - r.x) * ui_.s, wy = sy + (mouse.y - r.y) * ui_.s;
            zoomFocus_.x = std::clamp((wx - pl.x) / pl.w, 0.0f, 1.0f);
            zoomFocus_.y = std::clamp((wy - pl.y) / pl.h, 0.0f, 1.0f);
        }
    }

    void App::drawZoomPanes(Rectangle r)
    {
        const ui::Theme &t = ui::theme();
        if (previewOn_.id == 0 || previewRaw_.id == 0 || r.height < 40)
            return;
        const MonitorInfo *m = selectedMonitor();
        int pww = 0, pwh = 0;
        const scan::Placement pl = fullPreviewPlacement(m, pww, pwh);
        const float gap = 12;
        const float side = std::floor(std::min((r.width - gap) * 0.5f, r.height));
        const float ox = r.x + (r.width - (2 * side + gap)) * 0.5f;
        const int zoom = std::clamp(st_.full.previewZoom, 1, 8);
        const float texW = static_cast<float>(previewOn_.texture.width), texH = static_cast<float>(previewOn_.texture.height);
        const Rectangle pane0 = R(ox, r.y, side, side), pane1 = R(ox + side + gap, r.y, side, side);
        const Rectangle p0 = ui_.px(pane0);
        const float src = std::max(2.0f, std::floor(p0.width / static_cast<float>(zoom)));
        const float cx = pl.x + pl.w * zoomFocus_.x, cy = pl.y + pl.h * zoomFocus_.y;
        const float zx = std::clamp(std::round(cx - src * 0.5f), 0.0f, std::max(0.0f, texW - src));
        const float zy = std::clamp(std::round(cy - src * 0.5f), 0.0f, std::max(0.0f, texH - src));
        auto pane = [&](const RenderTexture2D &rt, Rectangle pr) {
            const Rectangle pp = ui_.px(pr);
            DrawRectangleRec(pp, BLACK);
            DrawTexturePro(rt.texture, Rectangle{zx, texH - (zy + src), src, -src}, pp, Vector2{0, 0}, 0, WHITE);
            ui_.roundLine(pr, 3, 1.2f, ui::alpha(t.border, 0.9f));
        };
        pane(previewRaw_, pane0);
        pane(previewOn_, pane1);
        ui_.chip(pane0.x + 6, pane0.y + 6, "Without the picture settings", ui::alpha(BLACK, 0.7f), t.text, 11.5f);
        ui_.chip(pane1.x + 6, pane1.y + 6, strf("With your settings \xc2\xb7 %d\xc3\x97", zoom), ui::alpha(BLACK, 0.7f), t.text, 11.5f);
    }

    void App::drawPreviewFullscreen()
    {
        const ui::Theme &t = ui::theme();
        ui_.fill(R(0, 0, ui_.width, ui_.height), BLACK);
        if (previewOn_.id)
        {
            // The whole window GT4 would show, as large as the launcher's window allows (1:1 when it is that size).
            const float tw = static_cast<float>(previewOn_.texture.width), th = static_cast<float>(previewOn_.texture.height);
            const float k = std::min(static_cast<float>(ui_.widthPx) / tw, static_cast<float>(ui_.heightPx) / th);
            const float w = tw * k, h = th * k;
            const Rectangle dst{(ui_.widthPx - w) * 0.5f, (ui_.heightPx - h) * 0.5f, w, h};
            SetTextureFilter(previewOn_.texture, k < 1.0f ? TEXTURE_FILTER_BILINEAR : TEXTURE_FILTER_POINT);
            DrawTexturePro(previewOn_.texture, Rectangle{0, th, tw, -th}, dst, Vector2{0, 0}, 0, WHITE);
            SetTextureFilter(previewOn_.texture, TEXTURE_FILTER_POINT);
            // The zoom, bottom right, where the mouse points.
            const MonitorInfo *m = selectedMonitor();
            int pww = 0, pwh = 0;
            const scan::Placement pl = fullPreviewPlacement(m, pww, pwh);
            const Vector2 mouse = ui_.input().mouse;
            if (ui_.input().enabled && CheckCollisionPointRec(mouse, dst) && pl.w > 0)
            {
                zoomFocus_.x = std::clamp(((mouse.x - dst.x) / k - pl.x) / pl.w, 0.0f, 1.0f);
                zoomFocus_.y = std::clamp(((mouse.y - dst.y) / k - pl.y) / pl.h, 0.0f, 1.0f);
            }
            const float zs = std::min(300.0f, ui_.height * 0.36f);
            const Rectangle zr = R(ui_.width - zs - 20, ui_.height - zs - 20, zs, zs);
            const Rectangle zp = ui_.px(zr);
            const int zoom = std::clamp(st_.full.previewZoom, 1, 8);
            const float src = std::max(2.0f, std::floor(zp.width / static_cast<float>(zoom)));
            const float cx = pl.x + pl.w * zoomFocus_.x, cy = pl.y + pl.h * zoomFocus_.y;
            const float zx = std::clamp(std::round(cx - src * 0.5f), 0.0f, std::max(0.0f, tw - src));
            const float zy = std::clamp(std::round(cy - src * 0.5f), 0.0f, std::max(0.0f, th - src));
            DrawRectangleRec(zp, BLACK);
            DrawTexturePro(previewOn_.texture, Rectangle{zx, th - (zy + src), src, -src}, zp, Vector2{0, 0}, 0, WHITE);
            ui_.roundLine(zr, 3, 1.5f, ui::alpha(WHITE, 0.85f));
            ui_.chip(zr.x + 8, zr.y + 8, strf("%d\xc3\x97 zoom", zoom), ui::alpha(BLACK, 0.7f), t.text, 12);
        }
        ui_.chip(20, 20, "Full-screen preview \xc2\xb7 Esc or a click goes back \xc2\xb7 the mouse wheel zooms", ui::alpha(BLACK, 0.7f), t.text, 13);
        if (ui_.input().enabled && ui_.input().wheel != 0.0f)
        {
            st_.full.previewZoom = std::clamp(st_.full.previewZoom + (ui_.input().wheel > 0 ? 1 : -1), 1, 8);
            markDirty();
        }
        if (previewJustOpened_)
            previewJustOpened_ = false;
        else if (ui_.input().enabled && (IsKeyPressed(KEY_ESCAPE) || ui_.input().pressed))
            previewFullscreen_ = false;
    }

    // ------------------------------------------------------------------ Resolution (full-m2, Resolution)
    // Live with a GT4 program whose internal resolution was checked with GT4 (full_.internalRes, its marker string): Play
    // passes GT3_GS_GL_SCALE and GT3_GS_GL_FAST. Otherwise greyed with the "Not built yet" line (development builds; a
    // release build hides the tab) and Play gives that program native (adjustFullLaunch).
    void App::displayResolution(float x0, float y0, float x1, float y1)
    {
        const ui::Theme &t = ui::theme();
        FullSettings &f = st_.full;
        const bool dev = showUnbuilt_;
        const bool live = !full_.read || full_.internalRes;
        const float cw = x1 - x0, colW = std::floor((cw - kGap) * 0.5f);
        (void)y1;
        // Presets (developer, 2026-10-19): Automatic / 720p / 1080p / 1440p / 4K set the internal resolution below (and a window's
        // size); Custom leaves it as chosen. The screen's shape is on the same card. The other cards stay as they were.
        {
            const MonitorInfo *mon = selectedMonitor();
            const int monH = mon ? mon->h : 0;
            static const char *const kPresetSize[kFullResPresets] = {"", "", "1280 \xc3\x97 720", "1920 \xc3\x97 1080", "2560 \xc3\x97 1440", "3840 \xc3\x97 2160"};
            const std::string note = "Full screen keeps your monitor's own resolution; in a window the preset sets the window's size too. Custom: the "
                                     "internal resolution below, as you choose it.";
            const float h = 48 + 38 + 6 + 34 + 8 + ui_.wrapHeight(note, cw - 40, 12.5f) + 12;
            const Rectangle c = R(x0, y0, cw, h);
            if (live)
                fullui::card(ui_, c, "Resolution", {Pill::Full});
            else
                fullui::card(ui_, c, "Resolution", {Pill::NotBuilt, Pill::Full});
            const int order[kFullResPresets] = {1, 2, 3, 4, 5, 0}; // (Automatic first, Custom last)
            const float bw = std::floor((c.width - 20 - (kFullResPresets - 1) * 8) / static_cast<float>(kFullResPresets));
            for (int i = 0; i < kFullResPresets; ++i)
            {
                const int p = order[i];
                std::string hint;
                if (p == 0)
                    hint = strf("%s, set below", fullScaleLabel(std::clamp(f.internalRes, 1, kFullScales)));
                else if (p == 1)
                    hint = monH > 0 ? strf("%d\xc3\x97 here", fullPresetScale(1, monH)) : std::string("your screen");
                else
                    hint = strf("%s \xc2\xb7 %d\xc3\x97", kPresetSize[p], fullPresetScale(p, monH));
                if (fullui::radio(ui_, strf("res-preset-%d", p), R(c.x + 10 + i * (bw + 8), c.y + 48, bw, 32), f.resPreset == p, fullPresetLabel(p), hint, live,
                                  !live && dev))
                {
                    f.resPreset = p;
                    static const int kW[kFullResPresets] = {0, 0, 1280, 1920, 2560, 3840}, kH[kFullResPresets] = {0, 0, 720, 1080, 1440, 2160};
                    if (!st_.fullscreen && kW[p] > 0 && (!mon || (kW[p] <= mon->w && kH[p] <= mon->h)))
                        st_.windowW = kW[p], st_.windowH = kH[p];
                    markDirty();
                }
            }
            // The shape: 16:9, 21:9, 32:9 here; the others (4:3, three screens, square pixels) on Screen shape.
            float y = c.y + 48 + 38 + 6;
            ui_.text("Shape", c.x + 20, y + 8, 14, t.text);
            const bool exeLacks = full_.read && !full_.aspect, exeLacksWide = full_.read && !full_.wideShapes;
            int shape = f.aspect == FullAspect::Wide ? 0 : f.aspect == FullAspect::Ultra21 ? 1 : f.aspect == FullAspect::Ultra32 ? 2 : f.aspect == FullAspect::Triple48 ? 3 : -1;
            const float sw = std::min(620.0f, c.width - 140);
            if (ui_.segmented("res-shape", R(c.x + 100, y, sw, 34), {"Widescreen 16:9", "Ultrawide 21:9", "Super ultrawide 32:9", "Three screens 48:9"}, shape,
                              !exeLacks && !exeLacksWide, 12.5f))
            {
                f.aspect = shape == 1 ? FullAspect::Ultra21 : shape == 2 ? FullAspect::Ultra32 : shape == 3 ? FullAspect::Triple48 : FullAspect::Wide;
                markDirty();
            }
            if (shape < 0)
                ui_.text(ui_.fit(strf("Now: %s (Screen shape tab)", fullAspectLabel(f.aspect)), c.width - sw - 140, 12.5f), c.x + 100 + sw + 16, y + 9, 12.5f, t.muted);
            y += 34 + 8;
            ui_.wrap(note, c.x + 20, y, c.width - 40, 12.5f, ui::alpha(t.faint, 0.9f));
            y0 = c.y + c.height + 14;
        }
        const int scale = std::clamp(f.internalRes, 1, kFullScales);
        // A scale that is not offered yet (4x: greyed with its reason in test builds, left out in release builds, as the
        // other choices GT4 does not have yet): GT4 gets native (gt4full_settings.cpp, adjustFullLaunch).
        const bool offeredNow = live && fullScaleBuilt(scale);
        const int given = offeredNow ? scale : 1;
        const bool fast = fullDrawingFast(given, f.drawing);
        int rows = 0;
        for (int s = 1; s <= kFullScales; ++s)
            rows += fullScaleBuilt(s) || dev;
        float leftBottom = y0;
        {
            const float h = 48 + rows * 34 + 10 + 60 + (live ? 0 : 22) + 12 + 60;
            const Rectangle c = R(x0, y0, colW, h);
            if (live)
                fullui::card(ui_, c, "Internal resolution", {Pill::Full});
            else
                fullui::card(ui_, c, "Internal resolution", {Pill::NotBuilt, Pill::Full});
            float y = c.y + 48;
            for (int s = 1; s <= kFullScales; ++s)
            {
                if (!fullScaleBuilt(s) && !dev)
                    continue;
                const bool offered = live && fullScaleBuilt(s);
                std::string hint = fullScaleSize(s);
                if (s == 1)
                    hint += " (GT4's own)";
                else if (*fullScaleSpeed(s))
                    hint += std::string(" \xc2\xb7 ") + fullScaleSpeed(s);
                if (live && !fullScaleBuilt(s))
                    hint = fullScaleWhy(s);
                if (fullui::radio(ui_, strf("internal-res-%d", s), R(c.x + 10, y, c.width - 20, 32), scale == s, fullScaleLabel(s), hint, offered,
                                  !live && dev))
                {
                    f.internalRes = s;
                    f.resPreset = 0; // (a choice here: Custom)
                    markDirty();
                }
                y += 34;
            }
            y += 10;
            y += ui_.wrap("GT4 draws its pictures at this many times the PS2's size, then scales them to your screen: sharper edges on the cars and the "
                          "track. Speeds measured on a laptop with an RTX 5060.",
                          c.x + 20, y, c.width - 40, 12.5f, ui::alpha(t.faint, 0.9f)) +
                 4;
            if (!live && dev)
                fullui::notBuilt(ui_, c.x + 20, y + 4, c.width - 40, "the GT4 program here was not checked with GT4 at these sizes: it gets Native.");
            // Texture smoothing GT4_TEX_SMOOTH (analysis/design/tex-smooth; moved here from Detail and textures, developer 2026-10-19:
            // it only works at 2x and above). A GT4 program without it (its marker, full_.texSmooth): greyed.
            {
                const bool smoothLacks = full_.read && !full_.texSmooth;
                const float sy = c.y + c.height - 60;
                ui_.fill(R(c.x + 20, sy - 8, c.width - 40, 1), t.border);
                ui_.text("Texture smoothing", c.x + 20, sy + 4, 14, smoothLacks || scale <= 1 ? t.faint : t.text);
                if (ui_.toggle("tex-smooth", c.x + c.width - 60, sy + 2, f.texSmooth, !smoothLacks))
                    markDirty();
                ui_.text(ui_.fit("At 2\xc3\x97 and above: softens fine textures that shimmer. Native stays as the PS2 drew it.", c.width - 100, 12), c.x + 20, sy + 26, 12,
                         ui::alpha(t.faint, 0.85f));
            }
            leftBottom = c.y + c.height;
        }
        // (Drawing and Effects resolution: on the Graphics tab now, developer 2026-10-19.)
        (void)fast;
        (void)leftBottom;
        // Full screen / window, the monitor and the window's size (the Screen tab's Display card; developer 2026-10-19).
        displayShape(x0 + colW + kGap, y0, x1, y1);
    }

    // ------------------------------------------------------------------ Screen shape
    // developer, 2026-10-09: "too much going on in that screen. i just want all the different ratios, then i want full screen,
    // windowed, boarderless windowed, display what monitors are connected". Two cards: the screen shape (the ratios) and the
    // display (full screen / borderless window / window, the monitors, a window's size), with one line on the black bars.
    void App::displayShape(float x0, float y0, float x1, float y1)
    {
        const ui::Theme &t = ui::theme();
        FullSettings &f = st_.full;
        const bool dev = showUnbuilt_;
        const float cw = x1 - x0, lw = std::floor(std::min(500.0f, cw * 0.48f));
        (void)y1;
        // (The shapes moved to Resolution's card with the presets: developer 2026-10-19, "Display - Screen can probably shift to
        // Resolution". This draws the Display card and On this screen, in the column it is given.)
        (void)dev;
        (void)lw;
        // Display: full screen / borderless window / window, the monitors, the window's size, and the picture on it
        {
            const float rx = x0, rw = x1 - x0;
            const MonitorInfo *mon = selectedMonitor();
            int ww = 0, wh = 0;
            const scan::Placement pl = fullPlacement(mon, &ww, &wh);
            const bool exclusiveLacks = full_.read && !full_.exclusive;
            const float rowH = 40;
            const size_t monRows = std::max<size_t>(1, std::min<size_t>(monitors_.size(), 4));
            const float h = 52 + 46 + 16 + 20 + monRows * rowH + 10 + (st_.fullscreen ? 0 : 50) + 12;
            const Rectangle c = R(rx, y0, rw, h);
            fullui::card(ui_, c, "Display", {Pill::ClassicToo});
            float y = c.y + 52;
            // 0 full screen (GT3_FULLSCREEN=exclusive), 1 borderless window over the monitor (1), 2 a window (0), 3 one borderless
            // window across all monitors (span; developer wanted it back, 2026-10-09).
            int mode = !st_.fullscreen ? 2 : f.span ? 3 : f.exclusive ? 0 : 1;
            if (ui_.segmented("screen-mode", R(c.x + 20, y, c.width - 40, 34), {"Full screen", "Borderless window", "Window", "Across all monitors"}, mode, true,
                              12.5f))
            {
                st_.fullscreen = mode != 2;
                f.exclusive = mode == 0;
                f.span = mode == 3;
                markDirty();
            }
            y += 46;
            const char *modeHint = !st_.fullscreen ? "A normal window you can move and resize. Alt+Enter makes it full screen."
                                   : f.span       ? "One window without edges over every monitor; the picture keeps its shape in the middle."
                                   : f.exclusive  ? "GT4 takes over the monitor. Esc or Alt+Enter gives a window."
                                                  : "A window without edges covering the whole monitor: quick to Alt+Tab. Esc leaves it.";
            ui_.text(ui_.fit(modeHint, c.width - 40, 12.5f), c.x + 20, y, 12.5f, t.faint);
            if (f.exclusive && st_.fullscreen && !f.span && exclusiveLacks)
                ui_.text(ui_.fit("This GT4 program has the borderless one only: Play gives it that.", c.width - 40, 12.5f), c.x + 20, y + 18, 12.5f, t.warn);
            y += 16 + (f.exclusive && st_.fullscreen && !f.span && exclusiveLacks ? 18 : 0) + 8;
            ui_.text("Monitor", c.x + 20, y, 12.5f, t.faint, Weight::Semibold);
            y += 20;
            size_t shown = 0;
            for (const MonitorInfo &m : monitors_)
            {
                if (shown++ >= monRows)
                    break;
                const Rectangle r = R(c.x + 12, y, c.width - 24, rowH - 4);
                const bool sel = mon && mon->index == m.index;
                if (ui_.rowButton("mon-" + std::to_string(m.index), r, sel, !(f.span && st_.fullscreen)))
                    pickMonitor(m.index);
                ui_.ring(r.x + 18, r.y + r.height * 0.5f, 6.5f, 8.0f, sel ? t.accent : t.faint);
                if (sel)
                    ui_.circle(r.x + 18, r.y + r.height * 0.5f, 4.0f, t.accent);
                ui_.text(ui_.fit(m.name, r.width - 230, 14, Weight::Semibold), r.x + 36, r.y + 8, 14, t.text, Weight::Semibold);
                float tx = r.x + r.width - 12;
                if (m.primary)
                {
                    const float cwid = ui_.textWidth("main", 11.5f, Weight::Semibold) + 18;
                    tx -= cwid;
                    ui_.chip(tx, r.y + 8, "main", ui::alpha(t.accent, 0.18f), t.accent, 11.5f);
                    tx -= 8;
                }
                ui_.text(strf("%d \xc3\x97 %d \xc2\xb7 %d Hz", m.w, m.h, m.hz), tx, r.y + 9, 13, t.muted, Weight::Regular, Align::Right);
                y += rowH;
            }
            y += 10;
            if (!st_.fullscreen)
            {
                ui_.text("Window size", c.x + 20, y + 7, 14, t.text);
                const auto sizes = windowSizes(mon);
                std::vector<std::string> labels;
                int sel = 0;
                for (size_t i = 0; i < sizes.size(); ++i)
                {
                    labels.push_back(strf("%d \xc3\x97 %d", sizes[i].first, sizes[i].second));
                    if (sizes[i].first == st_.windowW && sizes[i].second == st_.windowH)
                        sel = static_cast<int>(i);
                }
                if (ui_.dropdown("window-size", R(c.x + 140, y, c.width - 160, 34), labels, sel))
                {
                    st_.windowW = sizes[static_cast<size_t>(sel)].first;
                    st_.windowH = sizes[static_cast<size_t>(sel)].second;
                    markDirty();
                }
                y += 50;
            }
            // On this screen (developer kept it, 2026-10-09): the screen or window in black, the race picture on it, its size and bars;
            // when there is room for it below the Display card.
            const float cy = c.y + c.height + kGap;
            if (y1 - cy < 170.0f)
                return;
            const Rectangle oc = R(rx, cy, rw, std::min(230.0f, y1 - cy));
            ui_.card(oc, "On this screen");
            const float dh = oc.height - 70, dw = std::min(dh * static_cast<float>(ww) / std::max(1, wh), std::min(280.0f, rw * 0.4f));
            const float dhh = dw * static_cast<float>(wh) / std::max(1, ww);
            const Rectangle scr = R(oc.x + 20, oc.y + 52 + (dh - dhh) * 0.5f, dw, dhh);
            ui_.fill(scr, BLACK);
            ui_.roundLine(R(scr.x - 3, scr.y - 3, scr.width + 6, scr.height + 6), 4, 1.5f, t.faint);
            const float k = dw / std::max(1, ww);
            const Rectangle picR = R(scr.x + pl.x * k, scr.y + pl.y * k, pl.w * k, pl.h * k);
            ui_.fill(picR, ui::alpha(t.accent, 0.35f));
            const float tx = scr.x + dw + 24, tw = oc.x + oc.width - 20 - tx;
            float ty = oc.y + 56;
            const int bx = static_cast<int>(std::lround(pl.x)), by = static_cast<int>(std::floor(pl.y));
            const char *where = st_.fullscreen ? "screen" : "window";
            ty += ui_.wrap(strf("Races fill %d \xc3\x97 %d of the %s's %d \xc3\x97 %d", static_cast<int>(std::lround(pl.w)), static_cast<int>(std::lround(pl.h)), where, ww, wh),
                           tx, ty, tw, 15, t.text, Weight::Semibold) +
                  10;
            ui_.wrap(bx <= 1 && by <= 1 ? std::string("No black bars.")
                     : by > 1 && bx <= 1 ? strf("Black bars %d px above and below: the shape is wider than the %s.", by, where)
                     : bx > 1 && by <= 1 ? strf("Black bars %d px at the sides: the shape is narrower than the %s.", bx, where)
                                         : strf("Black bars %d px at the sides and %d px above and below%s.", bx, by,
                                                st_.linesOn ? " (with the TV scanlines on the picture keeps a whole number of screen rows per line)" : ""),
                     tx, ty, tw, 12.5f, t.muted);
        }
    }

    // ------------------------------------------------------------------ Smoothness
    void App::displaySmoothness(float x0, float y0, float x1, float y1)
    {
        const ui::Theme &t = ui::theme();
        FullSettings &f = st_.full;
        const bool dev = showUnbuilt_;
        const float cw = x1 - x0, colW = std::floor((cw - kGap) * 0.5f);
        // Graphics scrolls (the Cars card joined it, developer 2026-10-19).
        const float gTop = y0;
        const float gOff = ui_.beginScroll("graphics", R(x0, y0, cw + 12, y1 - y0));
        y0 -= gOff;
        float gBottom = y0;
        // Renderer: Automatic (Vulkan on an NVIDIA or AMD card, OpenGL on any other), Vulkan or OpenGL (GT3_RENDERER auto / vk / gl;
        // a GT4 program without the choice, !full_.rendererAuto, draws with Vulkan and the others are greyed).
        {
            const float h = 290;
            const Rectangle c = R(x0, y0, colW, h);
            fullui::card(ui_, c, "Renderer", {Pill::Full});
            float y = c.y + 48;
            const bool choice = !full_.read || full_.rendererAuto;
            const int chosen = choice ? std::clamp(f.renderer, 0, 2) : 1;
            if (fullui::radio(ui_, "renderer-auto", R(c.x + 10, y, c.width - 20, 32), chosen == 0, "Automatic", "Vulkan on NVIDIA and AMD, OpenGL on other cards", choice))
            {
                f.renderer = 0;
                markDirty();
            }
            y += 34;
            if (fullui::radio(ui_, "renderer-vk", R(c.x + 10, y, c.width - 20, 32), chosen == 1, "Vulkan", "GT4's main renderer", choice))
            {
                f.renderer = 1;
                markDirty();
            }
            y += 34;
            if (fullui::radio(ui_, "renderer-gl", R(c.x + 10, y, c.width - 20, 32), chosen == 2, "OpenGL",
                              choice ? "for cards where Vulkan has trouble (no shaders to prepare)" : "not in this GT4 program", choice))
            {
                f.renderer = 2;
                markDirty();
            }
            y += 40;
            // Prepare shaders is for Vulkan: greyed with OpenGL chosen.
            const bool vk = (!full_.read || full_.vulkan) && chosen != 2;
            ui_.text("Prepare shaders", c.x + 20, y, 15, t.text, Weight::Semibold);
            y += 24;
            y += ui_.wrap("Vulkan builds its shaders the first time GT4 needs them, which can pause a race for a moment. Prepare them now: GT4 starts "
                          "hidden for a few seconds and closes again.",
                          c.x + 20, y, c.width - 40, 12.5f, t.muted) +
                 12;
            const bool ready = preparedForThisExe();
            if (ui_.iconButton("prepare", R(c.x + 20, y, 180, 36), Icon::Refresh, preparing_ ? "Preparing..." : "Prepare shaders", Style::Primary,
                               vk && !preparing_ && !busy(), 13.5f))
                startPrepare();
            std::string state;
            Color sc = t.muted;
            if (chosen == 2)
                state = "OpenGL has no shaders to prepare.";
            else if (!vk)
            {
                state = "This GT4 program was built without Vulkan.";
                sc = t.warn;
            }
            else if (preparing_)
                state = "GT4 is preparing them, hidden.";
            else if (ready)
            {
                state = "Ready for this GT4 program.";
                sc = t.ok;
            }
            else if (endsWith(f.prepared, " novulkan") && startsWith(f.prepared, exeStamp()))
            {
                state = "Vulkan could not start on this PC.";
                sc = t.warn;
            }
            else if (endsWith(f.prepared, " failed") && startsWith(f.prepared, exeStamp()))
            {
                state = "The last try did not finish: see logs\\prepare-shaders.log.";
                sc = t.warn;
            }
            else
                state = f.prepared.empty() ? "Not prepared yet." : "Not prepared for this GT4 program yet (it changed).";
            ui_.wrap(state, c.x + 216, y + 2, c.width - 236, 13, sc);
        }
        // Drawing and Effects resolution (moved here from Resolution: developer 2026-10-19, the Graphics tab).
        const bool resLive = !full_.read || full_.internalRes;
        const int scale = std::clamp(f.internalRes, 1, kFullScales);
        const bool offeredNow = resLive && fullScaleBuilt(scale);
        const int given = offeredNow ? scale : 1;
        const bool fast = fullDrawingFast(given, f.drawing);
        {
            const Rectangle c = R(x0, y0 + 290 + kGap, colW, 48 + 2 * 34 + 10 + 30 + 12);
            if (resLive)
                fullui::card(ui_, c, "Drawing", {Pill::Full});
            else
                fullui::card(ui_, c, "Drawing", {Pill::NotBuilt, Pill::Full});
            float y = c.y + 48;
            struct Choice
            {
                int value;
                const char *label, *hint;
            };
            // Exact and Fast only (developer, 2026-10-19: "just keep exact and fast"). A file's "auto" (the old Follow the resolution)
            // shows as what it gives now (exact at Native and 2x, fast at 3x and 4x) until one is picked.
            static const Choice choices[] = {{1, "Exact", "the PS2's blending, exact for every pixel"},
                                             {2, "Fast", "the graphics card's own blending: tiny differences, quicker"}};
            const int shownDrawing = f.drawing == 1 || f.drawing == 2 ? f.drawing : (fast ? 2 : 1);
            for (const Choice &ch : choices)
            {
                if (fullui::radio(ui_, strf("drawing-%d", ch.value), R(c.x + 10, y, c.width - 20, 32), shownDrawing == ch.value, ch.label, ch.hint,
                                  resLive, !resLive && dev))
                {
                    f.drawing = ch.value;
                    markDirty();
                }
                y += 34;
            }
            y += 10;
            const std::string now = given != scale ? strf("%s is not offered yet: GT4 draws at Native, exactly.", fullScaleLabel(scale))
                                    : scale <= 1   ? std::string("At Native GT4 always draws exactly; this choice applies at 2\xc3\x97 and above.")
                                                   : strf("Now: %s, %s.", fullScaleLabel(scale), fast ? "fast" : "exact");
            y += ui_.wrap(now, c.x + 20, y, c.width - 40, 12.5f, ui::alpha(t.faint, 0.9f)) + 4;
            // Exact at 3x and 4x: too slow for New York on the laptop (full-m2 runs); said where it is chosen.
            if (offeredNow && !fast && *fullScaleExactSpeed(scale))
                ui_.text(ui_.fit(fullScaleExactSpeed(scale), c.width - 40, 12.5f), c.x + 20, y + 2, 12.5f, t.warn);
        }
        gBottom = displayCars(x0, y0 + 290 + kGap + 48 + 2 * 34 + 10 + 30 + 12 + kGap, colW);
        float frBottom = y0;
        // Frame rate: the FPS counter, Off / Simple / Detailed with the detailed stats ticked (GT4_FPS_COUNTER, GT4_FPS_STATS;
        // a GT4 program without it gets nothing and the choice is greyed; Test Build 5's GT4.exe has on / off only: Simple and
        // Detailed both give it on, the stats are greyed). (The greyed VSync placeholder is gone: it did nothing; GT4 keeps its
        // own 60 frames a second. developer, 2026-10-19.)
        {
            const Rectangle cc = R(x0 + colW + kGap, y0, cw - colW - kGap, 0);
            const std::string intro = "Simple: frames per second and the latest frame's time (16.7 ms is a frame at 60 fps). Detailed: the stats "
                                      "ticked below. Drawn over the window only: GT4's photos and replays never show it. F3 in GT4: off, simple, "
                                      "detailed.";
            const float ih = ui_.wrapHeight(intro, cc.width - 40, 12.5f);
            const int rows = (kFullFpsStatCount + 1) / 2;
            const float h = 54 + 26 + 40 + ih + 10 + 24 + rows * 28.0f + 12 + 26 + 4;
            const Rectangle c = R(cc.x, y0, cc.width, h);
            frBottom = c.y + c.height;
            fullui::card(ui_, c, "Frame rate", {Pill::Full});
            float y = c.y + 54;
            const bool fpsLive = !full_.read || full_.fpsCounter;
            const bool statsLive = !full_.read || full_.fpsStats;
            ui_.text("FPS counter", c.x + 20, y, 15, fpsLive ? t.text : t.faint);
            y += 26;
            int mode = fpsLive ? std::clamp(f.fpsCounter, 0, 2) : 0;
            if (ui_.segmented("fps-mode", R(c.x + 20, y, c.width - 40, 34), {"Off", "Simple", "Detailed"}, mode, fpsLive, 12.5f))
            {
                f.fpsCounter = mode;
                markDirty();
            }
            y += 40;
            y += ui_.wrap(intro, c.x + 20, y, c.width - 40, 12.5f, ui::alpha(t.faint, 0.85f)) + 10;
            const bool ticks = fpsLive && statsLive && f.fpsCounter == 2;
            ui_.text("Detailed shows", c.x + 20, y, 13.5f, ticks ? t.text : t.faint, Weight::Semibold);
            y += 24;
            const float colX[2] = {c.x + 20, c.x + 20 + std::floor((c.width - 40) * 0.5f)};
            for (int i = 0; i < kFullFpsStatCount; ++i)
            {
                bool on = (f.fpsStats >> i) & 1;
                if (ui_.checkbox(std::string("fps-stat-") + kFullFpsStatKeys[i], colX[i / rows], y + static_cast<float>(i % rows) * 28.0f, on, kFullFpsStatLabels[i], ticks))
                {
                    f.fpsStats ^= 1 << i;
                    if ((f.fpsStats & 0x7FF) == 0)
                        f.fpsStats = 1; // at least one: frames per second
                    markDirty();
                }
            }
            y += rows * 28.0f + 12;
            std::string note = "CPU and GPU load: the whole PC and GT4's share. RAM and VRAM: GT4's and the PC's.";
            Color nc = ui::alpha(t.faint, 0.85f);
            if (!fpsLive)
            {
                note = "This GT4 program has no FPS counter.";
                nc = t.warn;
            }
            else if (!statsLive)
            {
                note = "This GT4 program has the older counter (on or off): Simple and Detailed both show it.";
                nc = t.warn;
            }
            ui_.text(ui_.fit(note, c.width - 40, 12.5f), c.x + 20, y, 12.5f, nc);
        }
        // Effects resolution (developer, 2026-10-14: "a toggle for effects normal, 2x, 3x, 4x"; analysis/design/effects-scale):
        // GT4's glow, light and reflection buffers at a smaller size than the picture. Only sizes below the picture's are live;
        // Normal draws them at the picture's size (as before).
        {
            const char *note = "GT4's glow, light and reflection effects drawn smaller than the picture: lighter work for the graphics card, a "
                               "little softer glow. Cars, track and text stay sharp.";
            const float w = cw - colW - kGap;
            const float h = 48 + 2 * 38 + 6 + ui_.wrapHeight(note, w - 40, 12.5f) + 14;
            const Rectangle c = R(x0 + colW + kGap, frBottom + kGap, w, h);
            fullui::card(ui_, c, "Effects resolution", {Pill::Full});
            const int choice = f.effectsScale;
            const float bw = std::floor((c.width - 20 - 8) / 2.0f);
            for (int i = 0; i < 4; ++i)
            {
                const int value = i == 0 ? 0 : i + 1; // Normal, 2x, 3x, 4x
                const bool offered = resLive && (value == 0 || value <= given);
                const std::string label = value == 0 ? std::string("Normal") : strf("%d\xc3\x97", value);
                const std::string hint = value == 0        ? std::string("as the picture")
                                         : value < given ? std::string("smaller than the picture")
                                         : value == given ? std::string("as Normal")
                                                          : std::string("bigger: as Normal");
                if (fullui::radio(ui_, strf("effects-res-%d", value), R(c.x + 10 + (i % 2) * (bw + 8), c.y + 48 + (i / 2) * 38, bw, 32), choice == value, label, hint,
                                  offered, false))
                {
                    f.effectsScale = value;
                    markDirty();
                }
            }
            ui_.wrap(note, c.x + 20, c.y + 48 + 76 + 6, c.width - 40, 12.5f, ui::alpha(t.faint, 0.9f));
            gBottom = std::max(gBottom, c.y + c.height);
        }
        ui_.endScroll("graphics", gBottom + gOff - gTop + 8);
    }

    // ------------------------------------------------------------------ Cameras (full-m2, Cameras and MoreCams)
    void App::displayCameras(float x0, float y0, float x1, float y1)
    {
        const ui::Theme &t = ui::theme();
        FullSettings &f = st_.full;
        const bool dev = showUnbuilt_;
        const float cw = x1 - x0, colW = std::floor(std::min(600.0f, (cw - kGap) * 0.5f));
        (void)y1;
        // GT4_CAM_FAR_CHASE and GT4_CAM_LOOK reach a GT4 program that has them (its marker, full_.cameras); GT4_CAM_VIEWS and
        // GT4_CAM_LOCK one that has more race cameras (full_.moreCameras). With an older one the controls it lacks are greyed
        // (development builds; a release build hides the tab without the race cameras) and Play does not pass them.
        const bool exeLacks = full_.read && !full_.cameras;
        const bool moreLacks = full_.read && !full_.moreCameras;
        const std::string block = cameraLookBlock();
        {
            static const char *const names[6] = {"Bumper", "Bonnet", "Roof", "Close chase", "Chase", "Far chase"};
            static const char *const notes[6] = {"GT4's own, from the front of the car.",
                                                 "GT4's own, over the bonnet.",
                                                 "Higher and further back than the bonnet: over the roof.",
                                                 "Nearer and a little higher than GT4's chase view.",
                                                 "GT4's own chase view.",
                                                 "1.5 times as far back and as high as GT4's chase view."};
            const std::string intro = "The view button (Select on GT4's standard controller layout) goes through the ticked views, in "
                                      "this order.";
            const std::string lockHint = "Locked: the view tilts with the car's body (hills, braking, body roll), as GT4's bumper and "
                                         "bonnet views do. Not locked: the horizon stays level in the bumper, bonnet and roof views; "
                                         "the chase views follow the car as GT4's chase view does, swinging out a little. Locked "
                                         "chase views stay straight behind the car and tilt with it.";
            const float ih = ui_.wrapHeight(intro, colW - 40, 12.5f), lh = ui_.wrapHeight(lockHint, colW - 40, 12.5f);
            const float rowH = 40.0f;
            const float h = 54 + ih + 12 + 22 + 6 * rowH + 8 + lh + 12 + 22 + (dev && (exeLacks || moreLacks) ? 26 : 0) + 14;
            const Rectangle c = R(x0, y0, colW, h);
            if (exeLacks || moreLacks)
                fullui::card(ui_, c, "Race views", {Pill::NotBuilt, Pill::Full});
            else
                fullui::card(ui_, c, "Race views", {Pill::Full});
            float y = c.y + 54;
            y += ui_.wrap(intro, c.x + 20, y, c.width - 40, 12.5f, ui::alpha(t.faint, 0.85f)) + 12;
            const float lockX = c.x + c.width - 128;
            ui_.text("In the cycle", c.x + 20, y, 12.5f, t.faint);
            ui_.text("Locked to tilt", lockX, y, 12.5f, t.faint);
            y += 22;
            for (int v = 0; v < 6; ++v)
            {
                // A GT4 program with the race cameras but not more of them: only the far chase can be chosen (after GT4's own views).
                const bool live = !exeLacks && (!moreLacks || v == 5);
                bool on = (f.camViews >> v) & 1;
                if (ui_.checkbox(std::string("cam-view-") + kFullCamViewKeys[v], c.x + 20, y + 2, on, names[v], live))
                {
                    f.camViews ^= 1 << v;
                    if ((f.camViews & 0x3F) == 0)
                        f.camViews = 1 << 4; // at least one view: GT4's chase
                    markDirty();
                }
                ui_.text(notes[v], c.x + 48, y + 22, 11.5f, ui::alpha(t.faint, 0.85f));
                bool locked = (f.camLock >> v) & 1;
                if (ui_.checkbox(std::string("cam-lock-") + kFullCamViewKeys[v], lockX, y + 2, locked, "Locked", !exeLacks && !moreLacks))
                {
                    f.camLock ^= 1 << v;
                    markDirty();
                }
                y += rowH;
            }
            y += 8;
            y += ui_.wrap(lockHint, c.x + 20, y, c.width - 40, 12.5f, ui::alpha(t.faint, 0.85f)) + 12;
            ui_.text("Replays keep GT4's own replay cameras.", c.x + 20, y, 12.5f, ui::alpha(t.faint, 0.85f));
            y += 22;
            if (dev && exeLacks)
                fullui::notBuilt(ui_, c.x + 20, y + 4, c.width - 40, "the GT4 program here does not have the race cameras yet.");
            else if (dev && moreLacks)
                fullui::notBuilt(ui_, c.x + 20, y + 4, c.width - 40, "the GT4 program here has only the far chase (no roof, close chase or tilt).");
        }
        {
            const float rx = x0 + colW + kGap, rw = x1 - rx;
            const std::string lookHint = "Push the right stick to look all the way round the car in the chase views, or around you from the "
                                         "bumper, bonnet and roof views. It springs back when you let go. The keypad (4, 6, 2, 7, 9) and the "
                                         "mouse with its right button held turn the view too.";
            std::vector<std::pair<std::string, Color>> lines;
            if (f.look360 && !block.empty())
                lines.push_back({"The 360 look is off with this mapping: " + block + ". Put the controller's right stick back there to use it.", t.warn});
            // GT4_CAM_LOOK_STICK (developer, 2026-10-09): GT4's Controller Setup cannot take Accelerate and Brake off the DualShock 2's
            // right stick, so with the 360 look on the look takes the stick in races (ticked, the default); unticked GT4 keeps it
            // for players who accelerate and brake with the stick.
            if (f.lookTakeStick)
                lines.push_back({"Ticked: in races the right stick only turns the view. GT4 does not see it there, so accelerate and brake with "
                                 "the triggers (R2 and L2) or Cross and Square. In menus the right stick works as usual.",
                                 t.text});
            else
                lines.push_back({"Not ticked: GT4 keeps the right stick. GT4's standard DualShock 2 layout accelerates and brakes with it (up and "
                                 "down), and GT4's Controller Setup cannot take that off, so the right stick does not turn the view then; the "
                                 "keypad and mouse still do.",
                                 t.text});
            lines.push_back({"With a wheel the controller's right stick is free: it turns the view.", t.text});
            float h = 54 + 44 + 34 + ui_.wrapHeight(lookHint, rw - 40, 12.5f) + 14 + 1 + 14;
            for (const auto &l : lines)
                h += ui_.wrapHeight(l.first, rw - 40, 13) + 12;
            const Rectangle c = R(rx, y0, rw, h);
            if (exeLacks)
                fullui::card(ui_, c, "360 look and the right stick", {Pill::NotBuilt, Pill::Full});
            else
                fullui::card(ui_, c, "360 look and the right stick", {Pill::Full});
            float y = c.y + 54;
            int look = f.look360 ? 1 : 0;
            if (ui_.segmented("look-360", R(c.x + 20, y, c.width - 40, 34), {"Off", "360 on the right stick"}, look, !exeLacks, 12.5f))
            {
                f.look360 = look == 1;
                markDirty();
            }
            y += 44;
            bool take = f.lookTakeStick;
            if (ui_.checkbox("look-take-stick", c.x + 20, y + 2, take, "The right stick turns the camera in races (accelerate and brake with the triggers)",
                             !exeLacks && f.look360))
            {
                f.lookTakeStick = take;
                markDirty();
            }
            y += 34;
            y += ui_.wrap(lookHint, c.x + 20, y, c.width - 40, 12.5f, ui::alpha(t.faint, 0.85f)) + 14;
            ui_.fill(R(c.x + 20, y, c.width - 40, 1), t.border);
            y += 14;
            for (const auto &l : lines)
                y += ui_.wrap(l.first, c.x + 20, y, c.width - 40, 13, l.second) + 12;
        }
    }

    // ------------------------------------------------------------------ Cars (on Graphics since 2026-10-19)
    float App::displayCars(float x0, float y0, float colW)
    {
        const ui::Theme &t = ui::theme();
        FullSettings &f = st_.full;
        const bool dev = showUnbuilt_;
        float bottomY = y0;
        // GT4_CAR_DETAIL, GT4_WHEEL_DETAIL and GT4_WHEEL_BLUR reach a GT4 program that has them (its marker, full_.detail); with
        // an older one the controls are greyed (development builds; a release build hides the tab) and Play does not pass them.
        const bool exeLacks = full_.read && !full_.detail;
        const std::vector<std::string> detail = {"GT4's own", "Full detail twice as far", "Four times as far", "Always full detail"};
        {
            const std::string carHint = "GT4 draws cars and their wheels more simply as they get further away. Further keeps the full "
                                        "model longer; always full never switches. Races and replays alike; your save is not changed.";
            const float hh = ui_.wrapHeight(carHint, colW - 40, 12.5f);
            const float h = 54 + hh + 14 + 44 + 48 + 52 + (dev && exeLacks ? 26 : 0) + 18;
            const Rectangle c = R(x0, y0, colW, h);
            if (exeLacks)
                fullui::card(ui_, c, "Cars", {Pill::NotBuilt, Pill::Full});
            else
                fullui::card(ui_, c, "Cars", {Pill::Full});
            float y = c.y + 54;
            y += ui_.wrap(carHint, c.x + 20, y, c.width - 40, 12.5f, ui::alpha(t.faint, 0.85f)) + 14;
            ui_.text("Car detail", c.x + 20, y + 8, 14, exeLacks ? t.faint : t.text);
            int cd = std::clamp(f.carDetail, 0, 3);
            if (ui_.dropdown("car-detail", R(c.x + c.width - 20 - 230, y, 230, 32), detail, cd, !exeLacks))
            {
                f.carDetail = cd;
                markDirty();
            }
            y += 44;
            ui_.text("Wheel and rim detail", c.x + 20, y + 8, 14, exeLacks ? t.faint : t.text);
            int wd = std::clamp(f.wheelDetail, 0, 3);
            if (ui_.dropdown("wheel-detail", R(c.x + c.width - 20 - 230, y, 230, 32), detail, wd, !exeLacks))
            {
                f.wheelDetail = wd;
                markDirty();
            }
            y += 48;
            ui_.text("Wheel-spin blur", c.x + 20, y + 1, 14, exeLacks ? t.faint : t.text);
            if (ui_.toggle("wheel-blur", c.x + c.width - 60, y, f.wheelBlur, !exeLacks))
                markDirty();
            ui_.text("GT4's own blurred rims at speed. Off: sharp rims.", c.x + 20, y + 22, 12, ui::alpha(t.faint, 0.85f));
            y += 52;
            // (The greyed "Full-detail mirror" placeholder is gone, developer 2026-10-19: it was never built.)
            if (dev && exeLacks)
                fullui::notBuilt(ui_, c.x + 20, y + 4, c.width - 40, "the GT4 program here does not have car and wheel detail yet.");
            bottomY = c.y + c.height;
        }
        return bottomY;
    }

    // ------------------------------------------------------------------ Texture packs (was Detail and textures; developer 2026-10-19:
    // the Cars card moved to Graphics, this tab "just Texture packs")
    void App::displayDetail(float x0, float y0, float x1, float y1)
    {
        const ui::Theme &t = ui::theme();
        FullSettings &f = st_.full;
        const bool dev = showUnbuilt_;
        const float cw = x1 - x0;
        (void)y1;
        // Texture packs (developer, 2026-10-19: the dump / replace section renamed; texture smoothing moved to Resolution; the
        // not-built texture sharpness and sharper reflections rows gone). GT4_TEXTURE_DUMP / _OPAQUE / GT4_TEXTURE_REPLACE,
        // folders next to GT4.exe.
        const bool texLacks = full_.read && !full_.texDump;
        if (dev || !texLacks)
        {
            const std::string intro = "Make your own textures: save GT4's as pictures, edit them, then load your edited ones in place of GT4's.";
            const float w = std::min(cw, 680.0f);
            const Rectangle c = R(x0, y0, w, 54 + ui_.wrapHeight(intro, w - 40, 12.5f) + 14 + 3 * 56 + 10);
            if (texLacks)
                fullui::card(ui_, c, "Texture packs", {Pill::NotBuilt, Pill::Full});
            else
                fullui::card(ui_, c, "Texture packs", {Pill::Full});
            float y = c.y + 54;
            y += ui_.wrap(intro, c.x + 20, y, c.width - 40, 12.5f, ui::alpha(t.faint, 0.85f)) + 14;
            const struct
            {
                const char *id, *label, *note;
                bool *value;
            } texRows[] = {
                {"tex-dump", "Save GT4's textures", "Each texture GT4 draws, saved once as a PNG in texture-dump next to GT4.exe.", &f.texDump},
                {"tex-dump-opaque", "Also save copies without transparency", "In texture-dump\\_opaque, easier to view and edit.", &f.texDumpOpaque},
                {"tex-replace", "Load my textures", "GT4 draws your PNGs from texture-replace instead (named like the saved ones, any size).", &f.texReplace},
            };
            for (const auto &row : texRows)
            {
                ui_.text(row.label, c.x + 20, y + 1, 14, texLacks ? t.faint : t.text);
                if (ui_.toggle(row.id, c.x + c.width - 60, y, *row.value, !texLacks))
                    markDirty();
                y += 22;
                y += ui_.wrap(row.note, c.x + 20, y, c.width - 90, 12, ui::alpha(t.faint, 0.85f)) + 12;
            }
            if (dev && texLacks)
                fullui::notBuilt(ui_, c.x + 20, y, c.width - 40, "the GT4 program here cannot save or load textures.");
        }
    }

    // ------------------------------------------------------------------ HUD
    void App::displayHud(float x0, float y0, float x1, float y1)
    {
        // HUD hand-off 7: the HUD's choices moved to Driving > Race HUD (the HUD layout page); this tab keeps one line pointing there.
        (void)y1;
        const ui::Theme &t = ui::theme();
        const Rectangle c = R(x0, y0, std::min(560.0f, x1 - x0), 132);
        fullui::card(ui_, c, "HUD", {Pill::Full});
        ui_.text("The HUD's settings are on Driving \xe2\x80\xba Race HUD", c.x + 20, c.y + 52, 14.5f, t.text);
        if (ui_.button("hud-moved", R(c.x + 20, c.y + 82, 180, 34), "Open Race HUD", Style::Primary))
        {
            page_ = Page::RaceHud;
            ui_.closePopups();
        }
    }

}
