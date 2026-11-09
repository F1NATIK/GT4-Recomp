// GT4 Recompilation launcher, Full edition: the Screens, Network, Sound and Saves > Game progress pages
// (FULL-LAUNCHER-PLAN.md 2.3, 2.8, 3.2; Sound: analysis/design/engine-bass).

#include "gt4stock_app.h"

#include "gt4full_ui.h"
#include "gt4stock_util.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <process.h> // _getpid (the Race Lab server quits with this program)

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
        const char *const kDot = " \xc2\xb7 ";

        Color medal(char m)
        {
            switch (m)
            {
            case 'G': return Color{222, 182, 62, 255};
            case 'S': return Color{178, 188, 200, 255};
            case 'B': return Color{190, 124, 70, 255};
            case 'P': return Color{63, 185, 80, 255};
            default: return Color{40, 48, 62, 255};
            }
        }

        std::string km(uint64_t metres)
        {
            if (metres < 100000)
                return strf("%.1f km", metres / 1000.0);
            return groupedNumber((metres + 500) / 1000) + " km";
        }
    }

    // ------------------------------------------------------------------ Screens
    // developer 2026-10-30: "simplify triple monitors, theres to many different screen options in to many different tabs" and
    // "GT4 own triple monitor wont work, it runs at 30fps, so we can remove that". Every screen choice is here now, with one
    // choice at the top: one screen (the shape; full screen / borderless window / window, the monitor, a window's size) or
    // three screens (one GT4 drawing one wide picture across them, GT4_SCREENS=3: the geometry, the middle monitor). Gone:
    // GT4's own Multi Monitor Mode (a GT4 copy per screen), 48:9 on one window and the window across all monitors.
    void App::pageMultiscreen()
    {
        const ui::Theme &t = ui::theme();
        FullSettings &f = st_.full;
        const bool live = !full_.read || full_.screens;
        if (f.mmMode != 0 && f.mmMode != kMmThreeWide)
            f.mmMode = 0;
        const bool three = f.mmMode == kMmThreeWide;
        drawHeader("Screens", "One monitor, or three side by side. Changes are saved as you make them.");
        const float x0 = kSidebarW + 32, x1 = ui_.width - 32, bottom = ui_.height - 48, cw = x1 - x0;
        const float lw = std::floor(std::min(560.0f, (cw - kGap) * 0.5f));
        // Three screens scroll (developer 2026-10-30: the lineup picture across the page, its sliders under it).
        const float off = three ? ui_.beginScroll("screens", R(x0, kTop, cw + 12, bottom - kTop)) : 0.0f;
        float y = kTop - off;
        // How many screens
        float modeBottom = y;
        {
            const float h = 46 + 2 * 36 + 10;
            const Rectangle c = R(x0, y, lw, h);
            fullui::card(ui_, c, "Screens", {Pill::Full});
            float yy = c.y + 46;
            if (fullui::radio(ui_, "screens-one", R(c.x + 10, yy, c.width - 20, 34), !three, "One screen", "GT4 on one monitor", true))
            {
                f.mmMode = 0;
                markDirty();
            }
            yy += 36;
            if (fullui::radio(ui_, "screens-three", R(c.x + 10, yy, c.width - 20, 34), three, "Three screens",
                              live ? "three monitors side by side: angled sides or one picture across" : "this GT4 program does not have them", live))
            {
                f.mmMode = kMmThreeWide;
                markDirty();
            }
            modeBottom = c.y + c.height;
            y += h + kGap;
        }
        if (!three)
        {
            // The shape (16:9, or wider for an ultrawide monitor; GT4's menus stay 16:9).
            const bool exeLacks = full_.read && (!full_.aspect || !full_.wideShapes);
            const std::string note = "21:9 and 32:9 are for an ultrawide monitor: races and replays fill it (GT4's view widened), GT4's menus stay 16:9.";
            const float h = 46 + 34 + 10 + ui_.wrapHeight(note, lw - 40, 12.5f) + 14;
            const Rectangle c = R(x0, y, lw, h);
            fullui::card(ui_, c, "Shape", {Pill::Full});
            int shape = f.aspect == FullAspect::Ultra21 ? 1 : f.aspect == FullAspect::Ultra32 ? 2 : 0;
            if (ui_.segmented("screens-shape", R(c.x + 20, c.y + 46, c.width - 40, 34), {"16:9", "Ultrawide 21:9", "Super ultrawide 32:9"}, shape, !exeLacks, 12.5f))
            {
                f.aspect = shape == 1 ? FullAspect::Ultra21 : shape == 2 ? FullAspect::Ultra32 : FullAspect::Wide;
                markDirty();
            }
            ui_.wrap(note, c.x + 20, c.y + 46 + 34 + 10, c.width - 40, 12.5f, t.faint);
            // Right: full screen / borderless window / window, the monitor, a window's size, On this screen (gt4full_display.cpp).
            displayShape(x0 + lw + kGap, kTop, x1, bottom);
            return;
        }
        // ---- Three screens. Row 1: the choice above and the middle monitor beside it.
        const float monitorBottom = displayShape(x0 + lw + kGap, kTop - off, x1, bottom);
        y = std::max(modeBottom, monitorBottom) + kGap;
        // Row 2: per camera, with the lineup picture across the page (lining up is what this page is mostly for).
        if (f.screensAngled)
            y = screensCameras(x0, y, x1) + kGap;
        // Row 3: the three screens' style and geometry, two columns across the page.
        {
            const MonitorInfo *mid = selectedMonitor();
            auto beside = [&](bool left) -> const MonitorInfo * {
                if (!mid)
                    return nullptr;
                for (const MonitorInfo &m : monitors_)
                {
                    if (&m == mid || std::min(m.y + m.h, mid->y + mid->h) - std::max(m.y, mid->y) < mid->h / 2)
                        continue;
                    if (left ? std::abs(m.x + m.w - mid->x) <= 8 : std::abs(m.x - (mid->x + mid->w)) <= 8)
                        return &m;
                }
                return nullptr;
            };
            const MonitorInfo *l = beside(true), *r = beside(false);
            auto nm = [](const MonitorInfo *m) { return m ? m->name : std::string("none found"); };
            const std::string where = mid ? strf("Left: %s. Middle: %s. Right: %s.", nm(l).c_str(), mid->name.c_str(), nm(r).c_str())
                                          : std::string("Middle: the monitor GT4 opens on.");
            const bool missing = mid && (!l || !r);
            const std::string about =
                f.screensAngled ? strf("Each side screen shows its own view, turned %d\xc2\xb0 in: set the angle your side screens really stand at, and the "
                                       "road runs on straight across the frames. The middle screen keeps GT4's own view at full sharpness, with the HUD and "
                                       "the mirror; GT4's menus are 16:9 on it.",
                                       f.screensAngle)
                                : std::string("One flat picture across the three: what lies behind the frames is left out, so lines run straight across them. "
                                              "The HUD and the mirror stay on the middle screen; GT4's menus are 16:9 on it.");
            const float colW = std::floor((cw - 40 - 32) * 0.5f);
            const std::string specNote = "Reads each monitor's own description (its EDID) on this PC: nothing is looked up online.";
            const float specH = 32 + 8 + ui_.wrapHeight(screensSpec_.empty() ? specNote : screensSpec_, cw - 40, 12.5f) + 12;
            const float h = 48 + 42 + 3 * 34 + 10 + specH + ui_.wrapHeight(where, cw - 40, 13.5f) + 6 + (missing ? 22 : 0) + ui_.wrapHeight(about, cw - 40, 12.5f) + 16;
            const Rectangle c = R(x0, y, cw, h);
            fullui::card(ui_, c, "Your three screens", {Pill::Experimental, Pill::Full});
            const bool on = live; // (set while GT4 runs too: read at its next start)
            float yy = c.y + 48;
            {
                int style = f.screensAngled ? 0 : 1;
                ui_.text("Style", c.x + 20, yy + 8, 13.5f, t.faint);
                if (ui_.segmented("screens-style", R(c.x + 170, yy, std::min(520.0f, c.width - 190), 32), {"Angled sides", "One picture across"}, style, on, 12.5f))
                {
                    f.screensAngled = style == 0;
                    markDirty();
                }
                yy += 42;
            }
            // Two columns of sliders: the left one's rows, then the right one's, from the same top.
            const float rowsTop = yy;
            auto row = [&](float cx, float &ry, const char *id, const char *label, double &value, float lo, float hi, float step, const std::string &shown) {
                ui_.text(label, cx, ry + 6, 13.5f, t.faint);
                float v = static_cast<float>(value);
                if (ui_.slider(id, R(cx + 170, ry, colW - 280, 28), v, lo, hi, step, on))
                {
                    value = v;
                    markDirty();
                }
                ui_.text(shown, cx + colW, ry + 6, 13.5f, t.text, Weight::Semibold, Align::Right);
                ry += 34;
            };
            float ly = rowsTop, ry = rowsTop;
            const float lx = c.x + 20, rx = c.x + 20 + colW + 32;
            if (f.screensAngled)
            {
                double angle = f.screensAngle;
                row(lx, ly, "mm-angle", "Side screens' angle", angle, 0.0f, 70.0f, 1.0f, strf("%d\xc2\xb0 in from flat", f.screensAngle));
                if (static_cast<int>(std::lround(angle)) != f.screensAngle)
                {
                    f.screensAngle = static_cast<int>(std::lround(angle));
                    markDirty();
                }
            }
            row(lx, ly, "mm-gap", "Gap between pictures", f.mmBezel, 0.0f, 60.0f, 0.5f, f.mmPanorama ? std::string("not used") : strf("%.1f mm", f.mmBezel));
            ui_.text(ui_.fit("Panorama (nothing hidden behind the frames)", colW - 70, 13.5f), lx, ly + 2, 13.5f, t.faint);
            if (ui_.toggle("mm-panorama", lx + colW - 40, ly, f.mmPanorama, on))
                markDirty();
            ly += 34;
            row(rx, ry, "mm-w", "Screen picture width", f.mmScreenW, 300.0f, 1500.0f, 0.5f, strf("%.1f mm", f.mmScreenW));
            row(rx, ry, "mm-h", "Screen picture height", f.mmScreenH, 150.0f, 900.0f, 0.5f, strf("%.1f mm", f.mmScreenH));
            // ("Your eye to the middle screen" (Build 57) is gone since Build 68: only the side views took its zoom, so with
            // the eye anywhere but GT4's own they no longer joined the middle one (the developer's 685 mm: sides ~1.8x the middle).
            // GT4_SCREENS_EYE is still kept in the settings file and ignored.)
            yy = std::max(ly, ry) + 10;
            // Get screen specs (developer 2026-10-30): the picture's size from the three monitors' EDIDs, shown first; Apply sets it
            // (the gap too for a monitor in the table below; otherwise the gap is left as it is). Nothing changes until Apply.
            if (ui_.button("screens-specs", R(c.x + 20, yy, 170, 32), "Get screen specs", Style::Secondary, mid != nullptr, 13))
            {
                // Known bezels: picture width and height and the gap between two pictures side by side, in mm.
                struct Known
                {
                    const char *model;
                    double w, h, gap;
                };
                static const Known kKnown[] = {{"G321Q", 698.1, 392.7, 18.1}}; // MSI G321Q (the developer's)
                std::string said;
                double w = 0, hh = 0, gap = -1;
                bool same = true;
                std::string first;
                const MonitorInfo *three[3] = {l, mid, r};
                const char *names[3] = {"Left", "Middle", "Right"};
                for (int i = 0; i < 3; ++i)
                {
                    if (!three[i])
                    {
                        said += strf("%s: no monitor. ", names[i]);
                        same = false;
                        continue;
                    }
                    const sys::MonitorSpec s = sys::monitorSpec(three[i]->x + three[i]->w / 2, three[i]->y + three[i]->h / 2);
                    if (!s.ok)
                    {
                        said += strf("%s: %s. ", names[i], s.why.c_str());
                        same = false;
                        continue;
                    }
                    const std::string who = s.maker + (s.model.empty() ? std::string() : " " + s.model);
                    said += strf("%s: %s, %d x %d mm from the monitor. ", names[i], who.c_str(), s.widthMm, s.heightMm);
                    if (first.empty())
                        first = who + strf("|%d|%d", s.widthMm, s.heightMm);
                    else if (first != who + strf("|%d|%d", s.widthMm, s.heightMm))
                        same = false;
                    if (i == 1)
                    {
                        w = s.widthMm, hh = s.heightMm;
                        for (const Known &k : kKnown)
                            if (s.model.find(k.model) != std::string::npos)
                                w = k.w, hh = k.h, gap = k.gap;
                    }
                }
                if (w > 0 && hh > 0)
                {
                    said += strf("Apply sets the picture to %.1f x %.1f mm", w, hh);
                    said += gap >= 0 ? strf(" and the gap to %.1f mm (this model's bezels).", gap)
                                     : std::string(". The gap is not known for this model: measure from one picture's edge to the next one's and enter it.");
                    if (!same)
                        said += " The three monitors are not all the same: the middle one's size is used.";
                }
                else
                    said += "Nothing to apply: enter the sizes by hand.";
                screensSpec_ = said;
                screensSpecReady_ = w > 0 && hh > 0;
                screensSpecW_ = w, screensSpecH_ = hh, screensSpecGap_ = gap;
            }
            if (screensSpecReady_ && ui_.button("screens-specs-apply", R(c.x + 20 + 180, yy, 110, 32), "Apply", Style::Primary, true, 13))
            {
                f.mmScreenW = screensSpecW_;
                f.mmScreenH = screensSpecH_;
                if (screensSpecGap_ >= 0)
                {
                    f.mmBezel = screensSpecGap_;
                    f.mmPanorama = false;
                }
                screensSpecReady_ = false;
                screensSpec_ += " Applied.";
                markDirty();
            }
            yy += 40;
            yy += ui_.wrap(screensSpec_.empty() ? specNote : screensSpec_, c.x + 20, yy, c.width - 40, 12.5f, screensSpec_.empty() ? t.faint : t.text) + 12;
            yy += ui_.wrap(where, c.x + 20, yy, c.width - 40, 13.5f, missing ? t.warn : t.text, Weight::Semibold) + 6;
            if (missing)
            {
                ui_.text(ui_.fit("Plug in the side monitors and place them right next to the middle one in Windows' display settings.", c.width - 40, 12.5f),
                         c.x + 20, yy, 12.5f, t.warn);
                yy += 22;
            }
            ui_.wrap(about, c.x + 20, yy, c.width - 40, 12.5f, t.faint);
            y = c.y + c.height + kGap;
        }
        ui_.endScroll("screens", y + off - kTop);
        // The lineup picture enlarged (a click on it): the whole picture, and both bezel seams magnified under it. A click or Esc
        // closes it.
        if (lineupZoom_ && lineupTex_.id)
        {
            const Rectangle all = ui_.px(R(0, 0, ui_.width, ui_.height));
            DrawRectangleRec(all, Color{0, 0, 0, 225});
            const float tw = static_cast<float>(lineupTex_.width), th = static_cast<float>(lineupTex_.height);
            const float pw = ui_.width - 64, ph = pw * th / tw;
            const Rectangle pic = R(32, 40, pw, ph);
            DrawTexturePro(lineupTex_, Rectangle{0, 0, tw, th}, ui_.px(pic), Vector2{0, 0}, 0, WHITE);
            // The seams: a strip 6% of the picture wide around each third's edge, three times bigger.
            const float sw = tw * 0.06f, sh = th * 0.7f, sy = th * 0.15f;
            const float iw = (ui_.width - 64 - kGap) * 0.5f, ih = iw * sh / sw > (ui_.height - ph - 140) ? (ui_.height - ph - 140) : iw * sh / sw;
            const float iwFit = ih * sw / sh;
            for (int s = 0; s < 2; ++s)
            {
                const float cx = tw * (s == 0 ? 1.0f / 3.0f : 2.0f / 3.0f);
                const float bx = 32 + s * (iw + kGap) + (iw - iwFit) * 0.5f, by = 40 + ph + 46;
                DrawTexturePro(lineupTex_, Rectangle{cx - sw * 0.5f, sy, sw, sh}, ui_.px(R(bx, by, iwFit, ih)), Vector2{0, 0}, 0, WHITE);
                ui_.text(s == 0 ? "Left seam" : "Right seam", bx, by - 24, 14, t.text, Weight::Semibold);
            }
            ui_.text("Click anywhere or press Esc to close", 32, 12, 13.5f, t.faint);
            if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT) || IsKeyPressed(KEY_ESCAPE))
                lineupZoom_ = false;
        }
    }

    // Angled triple screens per race camera (developer 2026-10-30): each camera's field of view, side angle trim, bezels and the side
    // screens' up/down offsets (GT4_SCREENS_CAMERAS), with the lineup picture GT4.exe saved for it (F12 in a race, or taken
    // again by GT4 as these sliders move through the live link: screens-lineup\<camera>.png) across the page, the sliders
    // under it. Keyed by GT4's camera (gt4cam's views), whatever moves the cameras themselves (Driving). Returns its bottom.
    float App::screensCameras(float x0, float y0, float x1)
    {
        const ui::Theme &t = ui::theme();
        FullSettings &f = st_.full;
        const fs::path gt4Dir = inst_.exeFound ? inst_.exe.parent_path() : inst_.root;
        // The camera GT4 is in now (screens-lineup\current.txt, written by GT4.exe as it changes): this tab follows it.
        if (liveFrames_++ % 20u == 0u)
        {
            std::string now;
            if (FILE *cf = std::fopen((gt4Dir / "screens-lineup" / "current.txt").string().c_str(), "r"))
            {
                char buf[64] = {};
                if (std::fgets(buf, sizeof(buf), cf))
                    now = trim(buf);
                std::fclose(cf);
            }
            if (!now.empty() && now != gt4CameraNow_)
            {
                gt4CameraNow_ = now;
                for (int i = 0; i < kFullScreenCameras; ++i)
                    if (now == kFullScreenCameraKeys[i] && f.screensCamTab != i)
                    {
                        f.screensCamTab = i;
                        markDirty();
                    }
            }
        }
        const int tab = std::clamp(f.screensCamTab, 0, kFullScreenCameras - 1);
        // The live link: these settings for the running GT4 (screens-live.txt beside GT4.exe), written as they change; GT4 takes
        // them in at once and takes the camera's lineup picture again, so the picture follows the sliders.
        {
            std::string cams;
            for (int i = 0; i < kFullScreenCameras; ++i)
            {
                const FullSettings::ScreensCamera &k = f.screensCams[i];
                cams += strf("%s%s:%d,%d,%d,%d,%d,%d,%d", i ? ";" : "", kFullScreenCameraKeys[i], k.fov, k.trim, k.bezels ? 1 : 0, k.upLeft, k.upRight,
                             k.zoomLeft, k.zoomRight);
                if (k.tiltLeft != 0 || k.tiltRight != 0)
                    cams += strf(",%d,%d", k.tiltLeft, k.tiltRight);
            }
            const std::string live = strf("GT4_SCREENS_ANGLE=%d\nGT4_SCREENS_EYE=%d\nGT4_SCREENS_CAMERAS=%s\n", f.screensAngle, f.screensEye, cams.c_str());
            // (not in the hidden test mode: its scenes' values must never reach a GT4 folder, or a GT4 running there)
            if (live != liveWritten_ && !hidden_)
            {
                liveWritten_ = live;
                if (FILE *lf = std::fopen((gt4Dir / "screens-live.txt").string().c_str(), "w"))
                {
                    std::fputs(live.c_str(), lf);
                    std::fclose(lf);
                }
            }
        }
        // The lineup picture, loaded again when the file changes.
        const fs::path file = gt4Dir / "screens-lineup" / (std::string(kFullScreenCameraKeys[tab]) + ".png");
        std::error_code ec;
        const auto when = fs::last_write_time(file, ec);
        const std::string key = ec ? std::string() : file.string() + "|" + std::to_string(static_cast<long long>(when.time_since_epoch().count()));
        if (key != lineupKey_)
        {
            if (lineupTex_.id)
                UnloadTexture(lineupTex_);
            lineupTex_ = Texture2D{};
            lineupKey_ = key;
            if (!key.empty())
            {
                Image img = LoadImage(file.string().c_str());
                if (img.data)
                {
                    lineupTex_ = LoadTextureFromImage(img);
                    SetTextureFilter(lineupTex_, TEXTURE_FILTER_BILINEAR);
                }
                UnloadImage(img);
            }
        }
        // The card: tabs, the picture across it, then two columns of sliders.
        const float cw = x1 - x0, pw = cw - 40;
        const float ph = lineupTex_.id ? pw * static_cast<float>(lineupTex_.height) / std::max(1, lineupTex_.width) : 96.0f;
        const float colW = std::floor((cw - 40 - 32) * 0.5f);
        // (rows: the taller of the two columns; the left has 4 (field of view, side angle, bezels, the reset buttons), the right
        // 6 (up/down, zoom and tilt for each side screen: Build 69's tilt rows fell below the card before this counted them)
        constexpr int kLeftRows = 4, kRightRows = 6;
        const float h = 48 + 42 + (gt4CameraNow_.empty() ? 0 : 24) + ph + 30 + std::max(kLeftRows, kRightRows) * 34 + 14;
        const Rectangle c = R(x0, y0, cw, h);
        fullui::card(ui_, c, "Per camera", {Pill::Experimental, Pill::Full});
        float yy = c.y + 48;
        int tabSel = tab;
        if (ui_.segmented("screens-cam", R(c.x + 20, yy, std::min(720.0f, c.width - 40), 32), {"Bumper", "Bonnet", "Roof", "Close", "Chase", "Far"}, tabSel, true,
                          12.0f))
        {
            f.screensCamTab = tabSel;
            markDirty();
        }
        yy += 42;
        if (!gt4CameraNow_.empty())
        {
            const char *label = gt4CameraNow_.c_str();
            for (int i = 0; i < kFullScreenCameras; ++i)
                if (gt4CameraNow_ == kFullScreenCameraKeys[i])
                    label = kFullScreenCameraLabels[i];
            ui_.text(ui_.fit(strf("GT4 is in the %s camera: your screens and this picture follow the sliders.", label), c.width - 40, 12.5f), c.x + 20, yy, 12.5f,
                     t.ok);
            yy += 24;
        }
        const Rectangle picR = R(c.x + 20, yy, pw, ph);
        if (lineupTex_.id)
        {
            DrawTexturePro(lineupTex_, Rectangle{0, 0, static_cast<float>(lineupTex_.width), static_cast<float>(lineupTex_.height)}, ui_.px(picR), Vector2{0, 0}, 0,
                           WHITE);
            if (!lineupZoom_ && IsMouseButtonReleased(MOUSE_BUTTON_LEFT) && CheckCollisionPointRec(GetMousePosition(), ui_.px(picR)))
                lineupZoom_ = true;
            yy += ph + 6;
            ui_.text(ui_.fit(strf("The %s camera's lineup picture: click it for the bezel seams up close. F12 in a race takes one any time.",
                                  kFullScreenCameraLabels[tab]),
                             c.width - 40, 12.5f),
                     c.x + 20, yy, 12.5f, t.faint);
        }
        else
        {
            ui_.fill(picR, ui::alpha(t.border, 0.5f));
            ui_.wrap(strf("No lineup picture for the %s camera yet: in a race in that view, press F12. It saves the three screens as shown, and it "
                          "appears here; then GT4 takes it again by itself as you move these sliders.",
                          kFullScreenCameraLabels[tab]),
                     c.x + 40, yy + 30, pw - 40, 13.0f, t.muted);
            yy += ph + 6;
        }
        yy += 24;
        FullSettings::ScreensCamera &cam = f.screensCams[tab];
        auto row = [&](float cx, float &ry, const char *id, const char *label, int &value, int lo, int hi, const std::string &shown) {
            ui_.text(label, cx, ry + 6, 13.5f, t.faint);
            float v = static_cast<float>(value);
            if (ui_.slider(id, R(cx + 170, ry, colW - 280, 28), v, static_cast<float>(lo), static_cast<float>(hi), 1.0f, true))
            {
                value = static_cast<int>(std::lround(v));
                markDirty();
            }
            ui_.text(shown, cx + colW, ry + 6, 13.5f, t.text, Weight::Semibold, Align::Right);
            ry += 34;
        };
        float ly = yy, ry = yy;
        const float lx = c.x + 20, rx = c.x + 20 + colW + 32;
        row(lx, ly, "cam-fov", "Field of view", cam.fov, 40, 160, cam.fov == 100 ? std::string("GT4's own") : strf("%d%% of GT4's", cam.fov));
        row(lx, ly, "cam-trim", "Side angle", cam.trim, -15, 15, strf("%d\xc2\xb0 (%+d)", f.screensAngle + cam.trim, cam.trim));
        ui_.text("Leave out what is behind the bezels", lx, ly + 2, 13.5f, t.faint);
        if (ui_.toggle("cam-bezels", lx + colW - 40, ly, cam.bezels, true))
            markDirty();
        ly += 34;
        // Back to the settings that match the screens (developer 2026-10-31, after the side views' fixes: sliders set to make up for
        // the old errors now push the pictures apart). Field of view 100, angle as the screens, bezels left out, level, 100%.
        {
            const FullSettings::ScreensCamera standard;
            auto same = [&](const FullSettings::ScreensCamera &k) {
                return k.fov == standard.fov && k.trim == standard.trim && k.bezels == standard.bezels && k.upLeft == standard.upLeft &&
                       k.upRight == standard.upRight && k.zoomLeft == standard.zoomLeft && k.zoomRight == standard.zoomRight &&
                       k.tiltLeft == standard.tiltLeft && k.tiltRight == standard.tiltRight;
            };
            bool allSame = true;
            for (int i = 0; i < kFullScreenCameras; ++i)
                allSame = allSame && same(f.screensCams[i]);
            const float bw = std::floor((colW - 10) * 0.5f);
            if (ui_.button("cam-reset", R(lx, ly - 2, bw, 30), "Reset this camera", Style::Secondary, !same(cam), 12.5f))
            {
                cam = standard;
                markDirty();
            }
            if (ui_.button("cam-reset-all", R(lx + bw + 10, ly - 2, bw, 30), "Reset all cameras", Style::Secondary, !allSame, 12.5f))
            {
                for (int i = 0; i < kFullScreenCameras; ++i)
                    f.screensCams[i] = standard;
                markDirty();
            }
            ly += 34;
        }
        row(rx, ry, "cam-up-left", "Left screen up/down", cam.upLeft, -20, 20, cam.upLeft == 0 ? std::string("level") : strf("%+d%%", cam.upLeft));
        row(rx, ry, "cam-up-right", "Right screen up/down", cam.upRight, -20, 20, cam.upRight == 0 ? std::string("level") : strf("%+d%%", cam.upRight));
        row(rx, ry, "cam-zoom-left", "Left screen zoom", cam.zoomLeft, 50, 200, cam.zoomLeft == 100 ? std::string("as it is") : strf("%d%%", cam.zoomLeft));
        row(rx, ry, "cam-zoom-right", "Right screen zoom", cam.zoomRight, 50, 200, cam.zoomRight == 100 ? std::string("as it is") : strf("%d%%", cam.zoomRight));
        // (Build 69, developer 2026-10-31: Builds 54-55's up/down look back as its own slider; any tilt breaks the straight joins)
        row(rx, ry, "cam-tilt-left", "Left screen tilt", cam.tiltLeft, -20, 20, cam.tiltLeft == 0 ? std::string("none") : strf("%+d%%", cam.tiltLeft));
        row(rx, ry, "cam-tilt-right", "Right screen tilt", cam.tiltRight, -20, 20, cam.tiltRight == 0 ? std::string("none") : strf("%+d%%", cam.tiltRight));
        return c.y + c.height;
    }

    // (Race Lab: its own tab, gt4full_racelab.cpp.)

    // ------------------------------------------------------------------ Network (GT4's LAN Battle; off by default)
    void App::pageNetwork()
    {
        const ui::Theme &t = ui::theme();
        FullSettings &f = st_.full;
        drawHeader("Network/Online", "GT4's LAN Battle (Arcade): GT4 on other PCs on this network, or several GT4 windows on this PC. Off by default.");
        const float x0 = kSidebarW + 32, x1 = ui_.width - 32, bottom = ui_.height - 48, cw = x1 - x0;
        const float lw = std::floor(std::min(560.0f, cw * 0.55f));
        const bool live = !full_.read || full_.net;
        {
            const Rectangle c = R(x0, kTop, lw, bottom - kTop);
            fullui::card(ui_, c, "LAN play", {Pill::Experimental, Pill::Full});
            float y = c.y + 48;
            static const char *const labels[3] = {"Off", "This network (other PCs)", "GT4 windows on this PC"};
            static const char *const hints[3] = {"no network adapter in GT4 (the default)", "GT4 on PCs on your home network", "two or more GT4 windows, linked"};
            for (int i = 0; i < 3; ++i)
            {
                if (fullui::radio(ui_, strf("net-%d", i), R(c.x + 10, y, c.width - 20, 34), f.netMode == i, labels[i], hints[i], live))
                {
                    f.netMode = i;
                    markDirty();
                }
                y += 36;
            }
            y += 10;
            if (f.netMode == 1)
            {
                ui_.text("This PC's address", c.x + 20, y + 8, 14, t.text);
                std::vector<std::string> opts{"Automatic"};
                int sel = 0;
                for (const sys::NetAddress &a : netAddresses_)
                {
                    opts.push_back(a.address + " (" + a.adapter + ")");
                    if (a.address == f.netIp)
                        sel = static_cast<int>(opts.size()) - 1;
                }
                if (!f.netIp.empty() && sel == 0)
                {
                    opts.push_back(f.netIp + " (not on this PC now)");
                    sel = static_cast<int>(opts.size()) - 1;
                }
                if (ui_.dropdown("net-ip", R(c.x + 200, y, c.width - 220, 32), opts, sel, live, 12.5f))
                {
                    f.netIp = sel == 0 || sel > static_cast<int>(netAddresses_.size()) ? std::string() : netAddresses_[static_cast<size_t>(sel - 1)].address;
                    markDirty();
                }
                y += 42;
                y += ui_.wrap("Automatic: the adapter with a private address and a router. Choose one when this PC has several.", c.x + 20, y, c.width - 40, 12.5f,
                              t.faint) +
                     10;
            }
            else if (f.netMode == 2)
            {
                std::vector<std::string> nums;
                for (int i = 1; i <= 16; ++i)
                    nums.push_back(std::to_string(i));
                ui_.text("This window's number", c.x + 20, y + 8, 14, t.text);
                int idx = std::clamp(f.netIndex, 1, 16) - 1;
                if (ui_.dropdown("net-index", R(c.x + c.width - 20 - 120, y, 120, 32), nums, idx, live))
                {
                    f.netIndex = idx + 1;
                    markDirty();
                }
                y += 42;
                ui_.text("GT4 windows on this PC", c.x + 20, y + 8, 14, t.text);
                int peers = std::clamp(f.netPeers, 1, 16) - 1;
                if (ui_.dropdown("net-peers", R(c.x + c.width - 20 - 120, y, 120, 32), nums, peers, live))
                {
                    f.netPeers = peers + 1;
                    markDirty();
                }
                y += 42;
                y += ui_.wrap("Each GT4 window gets its own number; start them from separate GT4 folders, each with its own memory card.", c.x + 20, y, c.width - 40,
                              12.5f, t.faint) +
                     10;
            }
            if (!live)
                y += ui_.wrap("This GT4 program has no network adapter: LAN play is off.", c.x + 20, y, c.width - 40, 13, t.warn) + 8;
            y += 6;
            ui_.fill(R(c.x + 20, y, c.width - 40, 1), t.border);
            y += 14;
            ui_.text("What GT4 gets", c.x + 20, y, 12.5f, t.faint, Weight::Semibold);
            y += 22;
            std::string keys = "GT4_NET=" + fullNetModeKey(f.netMode);
            if (f.netMode == 1 && !f.netIp.empty())
                keys += "  GT4_NET_IP=" + f.netIp;
            if (f.netMode == 2)
                keys += strf("  GT4_NET_INDEX=%d  GT4_NET_PEERS=%d", f.netIndex, f.netPeers);
            ui_.text(ui_.fit(keys, c.width - 40, 13, Weight::Mono), c.x + 20, y, 13, t.muted, Weight::Mono);
            y += 26;
            ui_.wrap("Your PC's network is only used for GT4's own LAN messages, on this network; nothing goes to the internet.", c.x + 20, y, c.width - 40, 12.5f,
                     t.faint);
        }
        {
            const float rx = x0 + lw + kGap;
            const Rectangle c = R(rx, kTop, x1 - rx, 290);
            ui_.card(c, "How to start a LAN Battle");
            float y = c.y + 50;
            const char *steps[] = {"1. Every PC (or window) with LAN play on, each with its own GT4 and memory card.",
                                   "2. In GT4: Arcade > LAN Battle. One player hosts, the others join.",
                                   "3. GT4's own LAN settings (console name, host) are in GT4's Options.",
                                   "Up to 6 players; LAN Battle has no AI cars."};
            for (const char *s : steps)
                y += ui_.wrap(s, c.x + 20, y, c.width - 40, 13, t.muted) + 8;
            // Online play (developer, 2026-10-08): coming, with GT4 on hosted servers; said in every build.
            const std::string online = "Online multiplayer is coming soon: fully hosted servers and reworked net code, so you can race GT4 over the internet, "
                                       "not only on your home network.";
            const float onlineH = std::floor(50 + ui_.wrapHeight(online, x1 - rx - 40, 13) + 18);
            {
                const Rectangle co = R(rx, kTop + 290 + kGap, x1 - rx, onlineH);
                ui_.card(co, "Online");
                ui_.wrap(online, co.x + 20, co.y + 50, co.width - 40, 13, t.text);
            }
            if (showUnbuilt_)
            {
                const Rectangle c2 = R(rx, kTop + 290 + kGap + onlineH + kGap, x1 - rx, 130);
                fullui::card(ui_, c2, "Smoother races over the internet", {Pill::NotBuilt});
                fullui::notBuilt(ui_, c2.x + 20, c2.y + 52, c2.width - 40, "opponents' cars predicted between GT4's updates, for distant players.");
            }
        }
    }

    // ------------------------------------------------------------------ Roadmap (developer, 2026-10-08; first named "Future features")
    // What is coming, in players' words (PLAN.md, FULL-LAUNCHER-PLAN.md, inputs/GRAPHICS-OPTIONS-LIST.md, the developer's list of
    // 2026-10-08): only what is planned and not built. Left out: the "Not for now" list (DLSS, FSR, ray tracing, 6x / 8x,
    // frame generation, G-Sync / FreeSync, texture packs, the benchmark button) and what is built (wider screens, the
    // cameras, internal resolution, the FPS counter, the engine bass).
    void App::pageRoadmap()
    {
        const ui::Theme &t = ui::theme();
        drawHeader("Roadmap", "What is coming to GT4 Recompilation. Plans, not promises: the order can change.");
        struct Group
        {
            const char *title;
            std::vector<const char *> items;
        };
        static const Group kGroups[] = {
            {"Tourist Trophy",
             {"Tourist Trophy recompiled for PC first, the same way as GT4.",
              "Then motorbikes racing alongside cars: to begin with, a ghost bike from Tourist Trophy riding in a GT4 race.",
              "Next, contact between bikes and cars.", "Then AI riders on bikes in the races.",
              "And the bikes in GT4's menus and in your garage."}},
            {"One launcher",
             {"One launcher with GT3, GT4 and Tourist Trophy tabs.", "Later, GT5 and GT6 tabs too."}},
            {"Online",
             {"Online play through a hosted relay server: no port forwarding.",
              "Smooth opponents: their cars are predicted between updates, so far-away players do not jump about.",
              "Your own car always runs on your own PC, so it answers your wheel at once.",
              "The host decides the number of laps and the finishing order, so every player gets the same result."}},
            {"Engine sound",
             {"Backfire pops when you lift off and when you change down.", "Exhaust flames to go with them.",
              "Engine sounds from other recordings, to choose for a car."}},
            {"Garage",
             {"Compare cars: a real performance test, each car driven with GT4's own physics for exact times, not estimates.",
              "Downforce at speed, from GT4's car data.", "Garage changes on .ps2 memory card files too (folder cards today)."}},
            {"Screens",
             {"GT4's own Multi Monitor Mode: up to 6 screens, each one a view of the same race.",
              "Side views for side screens, with bezel correction for angled screens.",
              "Triple screens from one PC: one GT4 drawing the centre and both sides."}},
            {"Picture",
             {"Menus, car select and the garage in 480p too (races are in 480p already).", "VSync on or off.", "NVIDIA NIS sharpening.",
              "TV (CRT) upscaling and the CRT look: screen curve, glow and shadow mask.",
              "Sharper distant textures, and sharper reflections and shadows at higher resolutions."}},
            {"Car and wheel detail",
             {"Cars at full detail further away: twice as far, four times as far, or always.", "The same choices for wheels and rims.",
              "Wheel-spin blur on or off."}},
            {"HUD",
             {"More of your own HUD: speed, revs, gear, map and a timing tower, each placed and sized as you like.",
              "The rear-view mirror on your phone's browser."}},
            {"Classic edition",
             {"The PS2-faithful GT4: the game as it was on the PS2, with the scanline filter and a few extras you turn on."}},
        };
        const float x0 = kSidebarW + 32, x1 = ui_.width - 32, bottom = ui_.height - 48, cw = x1 - x0;
        const int cols = cw >= 900 ? 3 : 2;
        const float colW = std::floor((cw - 14 - (cols - 1) * kGap) / cols), textW = colW - 40 - 16;
        auto cardHeight = [&](const Group &g) {
            float h = 50;
            for (const char *it : g.items)
                h += ui_.wrapHeight(it, textW, 13.5f) + 8;
            return std::floor(h + 10);
        };
        const Rectangle view = R(x0, kTop, cw, bottom - kTop);
        const float off = ui_.beginScroll("roadmap", view);
        std::vector<float> colY(static_cast<size_t>(cols), kTop - off);
        for (const Group &g : kGroups)
        {
            // Each card goes in the shortest column (the groups keep their order across the columns).
            const size_t col = static_cast<size_t>(std::min_element(colY.begin(), colY.end()) - colY.begin());
            const Rectangle c = R(x0 + col * (colW + kGap), colY[col], colW, cardHeight(g));
            ui_.card(c, g.title);
            float y = c.y + 50;
            for (const char *it : g.items)
            {
                ui_.circle(c.x + 24, y + 9, 3, t.accent);
                y += ui_.wrap(it, c.x + 36, y, textW, 13.5f, t.text) + 8;
            }
            colY[col] += c.height + kGap;
        }
        ui_.endScroll("roadmap", *std::max_element(colY.begin(), colY.end()) + off - kTop);
    }

    // ------------------------------------------------------------------ Sound (the engines' bass; Full edition only)
    namespace
    {
        // The RBJ low shelf GT4.exe applies to the engine voices (gt4_spu2.cpp: S = 1, 150 Hz, 48 kHz): gain in dB at f.
        float shelfGainDb(float gainDb, float f)
        {
            if (gainDb == 0.0f)
                return 0.0f;
            const double a = std::pow(10.0, gainDb / 40.0), w0 = 2.0 * 3.14159265358979 * 150.0 / 48000.0, cw = std::cos(w0),
                         sa = 2.0 * std::sqrt(a) * std::sin(w0) / 2.0 * std::sqrt(2.0);
            const double b0 = a * ((a + 1) - (a - 1) * cw + sa), b1 = 2 * a * ((a - 1) - (a + 1) * cw), b2 = a * ((a + 1) - (a - 1) * cw - sa);
            const double a0 = (a + 1) + (a - 1) * cw + sa, a1 = -2 * ((a - 1) + (a + 1) * cw), a2 = (a + 1) + (a - 1) * cw - sa;
            const double w = 2.0 * 3.14159265358979 * f / 48000.0;
            auto mag = [&](double c0, double c1, double c2) {
                const double re = c0 + c1 * std::cos(w) + c2 * std::cos(2 * w), im = -(c1 * std::sin(w) + c2 * std::sin(2 * w));
                return std::sqrt(re * re + im * im);
            };
            return static_cast<float>(20.0 * std::log10(mag(b0, b1, b2) / mag(a0, a1, a2)));
        }
    }

    void App::pageSound()
    {
        const ui::Theme &t = ui::theme();
        FullSettings &f = st_.full;
        drawHeader("Sound", "GT4's own sound, with the cars' engines a little deeper if you like, or your own engine sounds. GT4 takes it when it starts.");
        const float x0 = kSidebarW + 32, x1 = ui_.width - 32, bottom = ui_.height - 48, cw = x1 - x0;
        // Two tabs: Engine bass (GT4_ENGINE_BASS) and Sound packs (GT4_SOUND_PACK, gt4full_sounds.cpp).
        int tab = std::clamp(f.soundTab, 0, 1);
        if (fullui::tabs(ui_, "sound-tabs", x0, kTop - 10, cw, {"Engine bass", "Sound packs"}, tab))
        {
            f.soundTab = tab;
            markDirty();
        }
        const float top = kTop + 40;
        if (tab == 1)
        {
            pageSoundPacks(x0, top, x1, bottom);
            return;
        }
        const float lw = std::floor(std::min(560.0f, cw * 0.55f));
        // F3: a GT4 program without the engine bass gets nothing (Play leaves GT4_ENGINE_BASS out); test builds grey the
        // choice with the "Not built yet" line, release builds say what that GT4 plays.
        const bool live = !full_.read || full_.engineBass;
        const int level = std::clamp(f.engineBass, 0, 3);
        {
            const Rectangle c = R(x0, top, lw, bottom - top);
            fullui::card(ui_, c, "Engine bass", {Pill::Full});
            float y = c.y + 50;
            y += ui_.wrap("Makes the cars' engines and exhausts sound deeper, turbo or not. Outside a race (menus, music, movies) the sound is "
                          "exactly GT4's; in a race the other sounds are as GT4 made them, except that its loudest peaks are rounded off "
                          "instead of cut.",
                          c.x + 20, y, c.width - 40, 13.5f, t.muted) +
                 12;
            if (live || showUnbuilt_)
            {
                static const char *const hints[4] = {"GT4's own engine sound", "a little deeper", "deeper (the default)", "the deepest"};
                for (int i = 0; i < 4; ++i)
                {
                    if (fullui::radio(ui_, strf("bass-%d", i), R(c.x + 10, y, c.width - 20, 34), level == i, fullEngineBassLabel(i), hints[i], live,
                                      !live))
                    {
                        f.engineBass = i;
                        markDirty();
                    }
                    y += 36;
                }
                y += 6;
                if (!live)
                    y += fullui::notBuilt(ui_, c.x + 20, y, c.width - 40, "this GT4 program has no engine bass (it plays GT4's own engine sound).") + 8;
            }
            else
                y += ui_.wrap("This GT4 program plays GT4's own engine sound: it has no engine bass.", c.x + 20, y, c.width - 40, 13, t.warn) + 8;
            if (live || showUnbuilt_)
                y += ui_.wrap("When GT4 itself stays near full volume (a race start), the extra bass waits; a bump or a crash does not make it "
                              "dip. Takes effect the next time GT4 starts.",
                              c.x + 20, y, c.width - 40, 12.5f, t.faint) +
                     12;
            ui_.fill(R(c.x + 20, y, c.width - 40, 1), t.border);
            y += 14;
            ui_.text("What GT4 gets", c.x + 20, y, 12.5f, t.faint, Weight::Semibold);
            y += 22;
            ui_.text(live ? std::string("GT4_ENGINE_BASS=") + fullEngineBassKey(level) : std::string("nothing (this GT4 program does not read GT4_ENGINE_BASS)"), c.x + 20,
                     y, 13, t.muted, live ? Weight::Mono : Weight::Regular);
        }
        {
            // What the four choices do: the shelf's gain from 20 Hz to 2 kHz (a log scale), the chosen one bright.
            const float rx = x0 + lw + kGap;
            const Rectangle c = R(rx, top, x1 - rx, std::min(bottom - top, 430.0f));
            ui_.card(c, "What it does");
            float y = c.y + 50;
            const Rectangle g = R(c.x + 52, y + 6, c.width - 52 - 24, std::max(120.0f, std::min(200.0f, c.height - 50 - 150)));
            const float fLo = 20.0f, fHi = 2000.0f, dbTop = 10.0f;
            auto gx = [&](float hz) { return g.x + g.width * std::log10(hz / fLo) / std::log10(fHi / fLo); };
            auto gy = [&](float db) { return g.y + g.height * (1.0f - db / dbTop); };
            for (float db : {0.0f, 3.0f, 6.0f, 9.0f})
            {
                ui_.fill(R(g.x, gy(db), g.width, 1), db == 0.0f ? t.border : ui::alpha(t.border, 0.5f));
                ui_.text(strf("%+.0f dB", db), g.x - 8, gy(db) - 8, 11.5f, t.faint, Weight::Regular, Align::Right);
            }
            for (float hz : {50.0f, 150.0f, 500.0f, 1000.0f})
            {
                ui_.fill(R(gx(hz), g.y, 1, g.height), ui::alpha(t.border, hz == 150.0f ? 0.9f : 0.4f));
                ui_.text(hz >= 1000.0f ? strf("%.0f kHz", hz / 1000.0f) : strf("%.0f Hz", hz), gx(hz), g.y + g.height + 6, 11.5f, t.faint, Weight::Regular, Align::Center);
            }
            for (int pass = 0; pass < 2; ++pass)
                for (int i = 1; i <= 3; ++i)
                {
                    const bool chosen = live && i == level;
                    if ((pass == 0) == chosen)
                        continue; // the chosen curve last, on top
                    const Color col = chosen ? t.accent : ui::alpha(t.muted, 0.45f);
                    float px = 0, py = 0;
                    for (int k = 0; k <= 80; ++k)
                    {
                        const float hz = fLo * std::pow(fHi / fLo, k / 80.0f);
                        const float x = gx(hz), yv = gy(shelfGainDb(static_cast<float>(fullEngineBassGainDb(i)), hz));
                        if (k)
                            ui_.line(px, py, x, yv, chosen ? 2.6f : 1.4f, col);
                        px = x;
                        py = yv;
                    }
                    ui_.text(fullEngineBassLabel(i), g.x + 6, gy(static_cast<float>(fullEngineBassGainDb(i))) - 17, 11.5f, col, chosen ? Weight::Semibold : Weight::Regular);
                }
            y = g.y + g.height + 30;
            const char *lines[] = {"The engines' lowest notes get louder: up to +3 / +6 / +9 dB well below 150 Hz, half that at 150 Hz; above about "
                                   "400 Hz nothing changes. GT4's engines are deepest at 110-200 Hz, so you hear about +1 / +2.5 / +4 dB there.",
                                   "Every car in the race, turbo or not: GT4's engine and exhaust sounds only.",
                                   "Outside a race nothing changes. In a race GT4's loudest peaks are rounded off instead of cut."};
            for (const char *s : lines)
                if (y + 16 < c.y + c.height)
                    y += ui_.wrap(s, c.x + 20, y, c.width - 40, 12.5f, t.muted) + 6;
        }
    }

    // ------------------------------------------------------------------ Saves > Game progress (read only, real save data only)
    void App::pageProgress()
    {
        const ui::Theme &t = ui::theme();
        loadGarage();
        // Game progress and save details (developer, 2026-10-19: Save details showed nothing Game progress does not; its Change name
        // and Repair are here now).
        drawHeader("Game progress and save details", "What your GT4 save holds. Only Change name and Repair change it (each asks first and backs up the card).");
        const float x0 = kSidebarW + 32, x1 = ui_.width - 32, bottom = ui_.height - 48, cw = x1 - x0;
        if (ui_.button("progress-back", R(x1 - 170, 26, 170, 32), "\xe2\x86\x90  Memory cards", Style::Secondary, true, 13))
        {
            savesProgress_ = false;
            refreshCards();
        }
        if (progress_.loaded && ui_.button("progress-rename", R(x1 - 170 - 10 - 140, 26, 140, 32), "Change name", Style::Secondary, !busy(), 13))
        {
            details_ = progress_.details; // (the rename window reads the name and the old bug's mark from details_)
            nameEdit_ = details_.player;
            modal_ = Modal::RenameSave;
        }
        int tab = progressTab_;
        if (fullui::tabs(ui_, "progress-tabs", x0, kTop - 10, cw, {"Overview", "GT Mode", "Arcade"}, tab))
            progressTab_ = tab;
        const float y0 = kTop + 40;
        garageSourceLine(x0, y0, cw);
        const float top = y0 + 30;
        const Progress &p = progress_;
        const SaveDetails &d = p.details;
        if (!p.loaded)
        {
            ui_.wrap(d.problems.empty() ? std::string("No GT4 save on this card yet: GT4 makes one the first time you save in the game.")
                                        : d.problems.front().title + ": " + d.problems.front().text,
                     x0, top + 10, cw, 14, t.muted);
            return;
        }
        if (progressTab_ == 0)
        {
            // Overview: the headline numbers, then the licence collection.
            const int cols = cw > 1000 ? 5 : 4;
            const float tw = std::floor((cw - (cols - 1) * 12) / cols), th = 74;
            struct Tile
            {
                std::string value, label;
                Color c;
            };
            const double winRate = p.races ? 100.0 * p.wins / static_cast<double>(p.races) : 0.0;
            // GT4's own completion rate first (Home "Complete", Game Status), as GT4 shows it (the remainder dropped).
            const Completion &cp = p.completion;
            std::vector<Tile> tiles;
            if (cp.known)
                tiles.push_back({cp.shown(), strf("Game completion (%d of %d races and missions won)", cp.won, cp.total), t.accent});
            tiles.insert(tiles.end(), {
                {d.hasCredits ? "Cr. " + groupedNumber(d.credits) : "-", "Credits", t.text},
                {strf("Day %lld", static_cast<long long>(d.gameDay)), d.date.empty() ? std::string("GT4's Days counter") : "GT4's Days counter" + std::string(kDot) + d.date, t.text},
                {std::to_string(garage_.cars.size()), "Cars in the garage", t.text},
                {p.races ? strf("%.0f%%", winRate) : std::string("-"), strf("Win rate (%u of %llu races won)", p.wins, static_cast<unsigned long long>(p.races)), t.text},
                {km(p.distanceM), "Distance driven (A-spec and B-spec)", t.text},
                {"Cr. " + groupedNumber(d.prizeMoney), "Prize money won", t.text},
                {groupedNumber(d.aspecPoints), "A-spec points (the best score of every race)", t.accent},
                {std::to_string(d.prizeCars), "Prize cars", t.text},
                {strf("%d of %d", p.eventRacesWon, p.eventRaces), "Event races won (every event race on the disc)", t.accent},
                {strf("%d / 34", d.missionsDone), strf("Missions \xc2\xb7 coffee breaks %d / 5", d.coffeeDone), t.text},
            });
            float y = top;
            // The save's problems (as Save details showed them), with Repair for the old memory-card bug's extra data.
            if (!d.problems.empty())
            {
                const DetailProblem &pb = d.problems.front();
                const Color pc = pb.level >= 2 ? t.bad : t.warn;
                const Rectangle pr = R(x0, y, cw, 40);
                ui_.round(pr, 8, ui::alpha(pc, 0.10f));
                ui_.roundLine(pr, 8, 1, ui::alpha(pc, 0.45f));
                ui_.icon(Icon::Warn, R(pr.x + 10, pr.y + 11, 18, 18), pc);
                const float room = pr.width - 50 - (d.appendBug ? 110 : 0);
                const std::string more = d.problems.size() > 1 ? strf(" (and %zu more)", d.problems.size() - 1) : std::string();
                ui_.text(ui_.fit(pb.title + ": " + pb.text + more, room, 13), pr.x + 36, pr.y + 11, 13, t.text);
                if (d.appendBug && ui_.button("progress-repair", R(pr.x + pr.width - 8 - 96, pr.y + 6, 96, 28), "Repair", Style::Primary, !busy(), 12.5f))
                {
                    details_ = d;
                    openGarageEdit(repairEdit(), nullptr); // (the card Game progress reads: the Garage's target)
                }
                y += 48;
            }
            for (size_t i = 0; i < tiles.size(); ++i)
            {
                const int col = static_cast<int>(i % cols), row = static_cast<int>(i / cols);
                fullui::statTile(ui_, R(x0 + col * (tw + 12), y + row * (th + 12), tw, th), tiles[i].value, tiles[i].label, tiles[i].c);
            }
            y += ((tiles.size() + cols - 1) / cols) * (th + 12) + 8;
            // The next completion prize (GT4's prize table: achievement 25 / 50 / 100; the Endurance hall opens at 25%).
            if (cp.known && cp.total > 0)
            {
                std::vector<std::pair<int, const PresentRow *>> steps; // per mille, the prize row
                if (cat_)
                    for (const PresentRow &r : cat_->presentRows())
                        if (r.condition == "achievement" && std::atoi(r.detail.c_str()) > 0)
                            steps.push_back({std::atoi(r.detail.c_str()) * 10, &r});
                if (steps.empty())
                    for (int need : {250, 500, 1000})
                        steps.push_back({need, nullptr});
                std::sort(steps.begin(), steps.end(), [](const auto &a, const auto &b) { return a.first < b.first; });
                std::string line;
                for (const auto &[need, row] : steps)
                {
                    if (cp.perMille >= need)
                        continue;
                    std::string prize = "a prize car";
                    if (row && row->type == "car")
                        if (const DiscCar *car = cat_->carByLabel(row->content))
                        {
                            prize = !car->fullName.empty() ? car->fullName : !car->name.empty() ? car->name : car->label;
                            const bool setColour = !row->extra.empty() && std::isdigit(static_cast<unsigned char>(row->extra[0]));
                            if (setColour && names_)
                                if (const auto col = names_->colour(car->code, std::atoi(row->extra.c_str())))
                                    if (!col->name.empty())
                                        prize += " in " + col->name;
                        }
                    if (need == 250)
                        prize += ", and the Endurance hall opens";
                    const int wins = (need * cp.total + 999) / 1000; // the fewest wins with won * 1000 / total >= need
                    line = strf("Next completion prize at %d.%d%%: ", need / 10, need % 10) + prize + kDot +
                           strf("%d wins needed, %d so far", wins, cp.won);
                    break;
                }
                if (line.empty())
                    line = "Every completion prize reached (GT4 gives each one once).";
                const Rectangle sr = R(x0, y, cw, 34);
                ui_.round(sr, 8, t.panel2);
                ui_.icon(Icon::Trophy, R(sr.x + 10, sr.y + 8, 18, 18), t.accent);
                ui_.text(ui_.fit(line, sr.width - 50, 13), sr.x + 36, sr.y + 9, 13, t.text);
                y += 42;
            }
            const Rectangle c = R(x0, y, cw, bottom - y);
            ui_.card(c, "Licence collection");
            const float bw = std::floor((cw - 40 - 4 * 12) / 5);
            float yy = c.y + 52;
            for (size_t i = 0; i < 5; ++i)
            {
                const Rectangle b = R(c.x + 20 + i * (bw + 12), yy, bw, std::min(110.0f, c.height - 70));
                ui_.round(b, 10, t.panel2);
                if (i < p.licences.size())
                {
                    const ProgressLicence &l = p.licences[i];
                    const Color badge = l.allGold ? medal('G') : l.passed ? t.ok : t.faint;
                    ui_.text(l.key, b.x + 14, b.y + 10, 26, badge, Weight::Bold);
                    ui_.text(ui_.fit(l.passed ? (l.allGold ? "All gold" : "Passed") : strf("%d of 16 tests", l.done), bw - 70, 13), b.x + b.width - 12, b.y + 18, 13,
                             l.passed ? t.text : t.muted, Weight::Regular, Align::Right);
                    float mx = b.x + 14;
                    for (const auto &[m, n] : {std::pair<char, int>{'G', l.gold}, {'S', l.silver}, {'B', l.bronze}})
                    {
                        ui_.circle(mx + 6, b.y + 62, 6, medal(m));
                        mx += 16 + ui_.text(std::to_string(n), mx + 16, b.y + 54, 13, t.muted) + 10;
                    }
                }
            }
            return;
        }
        // GT Mode and Arcade: one scrolling list.
        const Rectangle view = R(x0, top, cw, bottom - top);
        const float off = ui_.beginScroll(progressTab_ == 1 ? "progress-gt" : "progress-arcade", view);
        float y = top - off;
        const float w = cw - 14;
        auto heading = [&](const std::string &s, const std::string &right) {
            ui_.text(s, x0, y, 16.5f, t.text, Weight::Semibold);
            if (!right.empty())
                ui_.text(right, x0 + w, y + 3, 13, t.muted, Weight::Regular, Align::Right);
            y += 32;
        };
        if (progressTab_ == 1)
        {
            heading("Licences", strf("B, A, IB, IA, S \xc2\xb7 16 tests each"));
            for (const ProgressLicence &l : p.licences)
            {
                ui_.text(l.key, x0, y + 4, 15, l.passed ? t.text : t.muted, Weight::Bold);
                ui_.text(ui_.fit(l.name, 150, 12), x0 + 34, y + 7, 12, t.faint);
                const float cell = std::floor(std::min(58.0f, (w - 200) / 16.0f));
                for (size_t i = 0; i < l.tests.size(); ++i)
                {
                    const ProgressTest &pt = l.tests[i];
                    const Rectangle r = R(x0 + 190 + i * cell, y, cell - 4, 38);
                    ui_.round(r, 5, pt.medal == '-' ? t.panel2 : ui::alpha(medal(pt.medal), 0.28f));
                    ui_.text(std::to_string(i + 1), r.x + r.width * 0.5f, r.y + 2, 11, pt.medal == '-' ? t.faint : medal(pt.medal), Weight::Semibold, Align::Center);
                    ui_.text(pt.bestMs ? lapTime(pt.bestMs) : std::string(pt.medal == '-' ? "" : "-"), r.x + r.width * 0.5f, r.y + 20, 9.5f, t.muted, Weight::Regular,
                             Align::Center);
                }
                y += 46;
            }
            y += 10;
            heading("Missions", strf("%d of 34", d.missionsDone));
            {
                const float cell = std::floor(std::min(58.0f, w / 17.0f));
                for (size_t i = 0; i < p.missions.size(); ++i)
                {
                    const ProgressTest &pt = p.missions[i];
                    const int col = static_cast<int>(i % 17), row = static_cast<int>(i / 17);
                    const Rectangle r = R(x0 + col * cell, y + row * 42, cell - 4, 38);
                    ui_.round(r, 5, pt.medal == '-' ? t.panel2 : ui::alpha(t.ok, 0.25f));
                    ui_.text(std::to_string(i + 1), r.x + r.width * 0.5f, r.y + 2, 11, pt.medal == '-' ? t.faint : t.ok, Weight::Semibold, Align::Center);
                    if (pt.bestMs)
                        ui_.text(lapTime(pt.bestMs), r.x + r.width * 0.5f, r.y + 20, 9.5f, t.muted, Weight::Regular, Align::Center);
                }
                y += ((p.missions.size() + 16) / 17) * 42 + 14;
            }
            heading("Coffee breaks", strf("%d of 5", d.coffeeDone));
            for (size_t i = 0; i < p.coffee.size(); ++i)
            {
                const ProgressTest &pt = p.coffee[i];
                const Rectangle r = R(x0 + i * 62, y, 58, 38);
                ui_.round(r, 5, pt.medal == '-' ? t.panel2 : ui::alpha(t.ok, 0.25f));
                ui_.text(std::to_string(i + 1), r.x + r.width * 0.5f, r.y + 2, 11, pt.medal == '-' ? t.faint : t.ok, Weight::Semibold, Align::Center);
                if (pt.bestMs)
                    ui_.text(lapTime(pt.bestMs), r.x + r.width * 0.5f, r.y + 20, 9.5f, t.muted, Weight::Regular, Align::Center);
            }
            y += 56;
            heading("Events and championships", strf("%d of %d races won \xc2\xb7 GT4's hall order", p.eventRacesWon, p.eventRaces));
            if (!d.championship.empty())
            {
                ui_.text(ui_.fit("Saved during a championship: " + d.championship, w, 13), x0, y, 13, t.warn);
                y += 24;
            }
            std::string hall;
            for (const ProgressEvent &e : p.events)
            {
                if (e.hall != hall)
                {
                    hall = e.hall;
                    y += 4;
                    fullui::caps(ui_, x0, y, hall.empty() ? std::string("Other events") : hall, t.faint);
                    y += 22;
                }
                const bool any = e.done > 0;
                ui_.text(ui_.fit(e.name, 260, 13.5f, any ? Weight::Semibold : Weight::Regular), x0, y + 3, 13.5f, any ? t.text : t.muted, any ? Weight::Semibold : Weight::Regular);
                const float cell = std::floor(std::min(34.0f, (w - 420) / std::max<size_t>(1, e.races.size())));
                for (size_t i = 0; i < e.races.size(); ++i)
                {
                    const ProgressRace &r = e.races[i];
                    const Rectangle rr = R(x0 + 270 + i * cell, y, cell - 3, 24);
                    const Color bg = !r.run ? t.panel2 : r.result == 0 ? ui::alpha(medal('G'), 0.35f) : ui::alpha(t.accent, 0.22f);
                    ui_.round(rr, 4, bg);
                    if (r.run)
                        ui_.text(r.result == 0 ? std::string("1") : std::to_string(r.result + 1), rr.x + rr.width * 0.5f, rr.y + 4, 11, t.text, Weight::Semibold, Align::Center);
                }
                ui_.text(any ? strf("%d / %zu won \xc2\xb7 %d pts", e.won, e.races.size(), e.points) : strf("%zu races", e.races.size()), x0 + w, y + 4, 12.5f,
                         any ? t.muted : t.faint, Weight::Regular, Align::Right);
                y += 30;
            }
        }
        else
        {
            heading("Arcade", strf("%zu cars and %zu courses available \xc2\xb7 course score total %d", p.availableCars, p.availableCourses, p.arcadeScoreTotal));
            ui_.wrap("GT4's Arcade keeps a best score and the time rankings per course, its 4 machine records, the cars and courses it lists and your "
                     "favourites. It keeps nothing else for Arcade.",
                     x0, y, w, 12.5f, t.faint);
            y += 44;
            const float c1 = x0, c2 = x0 + 300, c3 = x0 + 400, c4 = x0 + 520, c5 = x0 + w - 220, c6 = x0 + w;
            auto table = [&](const std::vector<ProgressCourse> &rows, const char *empty) {
                ui_.text("Course", c1, y, 12, t.faint, Weight::Semibold);
                ui_.text("Score", c2, y, 12, t.faint, Weight::Semibold);
                ui_.text("Best time", c3, y, 12, t.faint, Weight::Semibold);
                ui_.text("Car", c4, y, 12, t.faint, Weight::Semibold);
                ui_.text("Driver, date", c6, y, 12, t.faint, Weight::Semibold, Align::Right);
                y += 22;
                if (rows.empty())
                {
                    ui_.text(empty, c1, y, 13, t.muted);
                    y += 26;
                }
                for (const ProgressCourse &r : rows)
                {
                    ui_.text(ui_.fit(r.name, c2 - c1 - 12, 13.5f), c1, y, 13.5f, t.text);
                    ui_.text(r.bestScore ? strf("%03d", r.bestScore) : std::string("-"), c2, y, 13.5f, t.text);
                    ui_.text(lapTime(r.bestMs), c3, y, 13.5f, t.text, Weight::Mono);
                    ui_.text(ui_.fit(r.car.empty() ? std::string("-") : r.car, c5 - c4 - 12, 13), c4, y, 13, t.muted);
                    ui_.text(ui_.fit((r.driver.empty() ? std::string("-") : r.driver) + (r.date.empty() ? std::string() : kDot + r.date), c6 - c5, 12.5f), c6, y + 1,
                             12.5f, t.faint, Weight::Regular, Align::Right);
                    y += 26;
                }
            };
            heading("Course records", strf("%zu course(s)", p.courses.size()));
            table(p.courses, "No Arcade records yet.");
            y += 12;
            heading("Machine records", "GT4's 4 machine records");
            table(p.machineRecords, "None yet.");
            y += 12;
            heading("Favourites", "");
            ui_.wrap("Cars: " + (p.favouriteCars.empty() ? std::string("none") : join(p.favouriteCars, ", ")), x0, y, w, 13, t.muted);
            y += 24;
            ui_.wrap("Courses: " + (p.favouriteCourses.empty() ? std::string("none") : join(p.favouriteCourses, ", ")), x0, y, w, 13, t.muted);
            y += 30;
        }
        ui_.endScroll(progressTab_ == 1 ? "progress-gt" : "progress-arcade", y + off - top + 10);
    }

    void App::garageSourceLine(float x, float y, float w)
    {
        const ui::Theme &t = ui::theme();
        std::string line = garageTitle_;
        if (garage_.loaded)
            line += kDot + (garage_.player.empty() ? std::string("no name") : garage_.player) + kDot + strf("Day %lld", static_cast<long long>(garage_.gameDay)) +
                    (garage_.hasCredits ? kDot + ("Cr. " + groupedNumber(garage_.credits)) : std::string());
        ui_.icon(Icon::Card, R(x, y + 1, 16, 16), t.muted);
        ui_.text(ui_.fit(line, w - 260, 13), x + 24, y, 13, t.muted);
        // The card the Garage and Game progress read (slot 2 when it holds a card file).
        if (garageCopy_.empty() && cardIsFile(2))
        {
            int slot = std::clamp(st_.full.garageCard, 1, 2) - 1;
            if (ui_.segmented("garage-card", R(x + w - 230, y - 6, 230, 28), {"Slot 1", "Slot 2"}, slot, true, 12))
            {
                st_.full.garageCard = slot + 1;
                markDirty();
                loadGarage(true);
            }
        }
    }
}
