// GT4 Recompilation launcher, Full edition: the Multi-screen, Network, Sound and Saves > Game progress pages
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

    // ------------------------------------------------------------------ Multi-screen
    // Built (full-m2, Multi): three screens side by side with one wide picture (GT4_SCREENS=3, with a GT4.exe that has the
    // marker); GT4's own Multi Monitor Mode (analysis/design/multi-monitor: a GT4 copy per screen, linked, with a GT4.exe that
    // has its marker). Not built yet (test builds only, greyed): side views in every mode, one GT4 drawing every view,
    // 6 screens, the HUD on every screen.
    void App::pageMultiscreen()
    {
        const ui::Theme &t = ui::theme();
        FullSettings &f = st_.full;
        const bool live = !full_.read || full_.screens;
        const bool three = f.mmMode == kMmThreeWide;
        const bool ownLive = !full_.read || full_.multiMonitor; // GT4's own Multi Monitor Mode (its marker)
        const bool own = f.mmMode == kMmGt4Own;
        drawHeader("Multi-screen", "GT4 across three monitors side by side. The defaults are your rig: three 31.5\" screens, 70 cm away.");
        const float x0 = kSidebarW + 32, x1 = ui_.width - 32, bottom = ui_.height - 48, cw = x1 - x0;
        const float lw = std::floor(std::min(520.0f, cw * 0.5f));
        // Mode
        float y = kTop;
        {
            struct Mode
            {
                int value;
                const char *label, *hint;
                bool built;
            };
            static const Mode modes[] = {{0, "One screen", "GT4 on one monitor (Display > Screen shape)", true},
                                         {kMmThreeWide, "Three screens, one wide picture", "races and replays across the three, in every mode", true},
                                         {kMmGt4Own, "GT4's own Multi Monitor Mode", "a GT4 window per screen, linked (Arcade > Multi-LAN Race)", ownLive},
                                         {2, "GT4's side views in every mode", "each side screen its own angled view", false},
                                         {3, "One GT4 drawing every view", "up to 6 screens and a live view", false}};
            const int rows = showUnbuilt_ ? 5 : ownLive ? 3 : 2;
            const float h = 46 + rows * 34 + 12;
            const Rectangle c = R(x0, y, lw, h);
            fullui::card(ui_, c, "Mode", {Pill::Full});
            float yy = c.y + 46;
            for (int i = 0; i < rows; ++i)
            {
                const Mode &m = modes[i];
                const bool enabled = m.built && (m.value != kMmThreeWide || live) && (m.value != kMmGt4Own || ownLive); // (chosen while GT4 runs too: read at its next start)
                if (fullui::radio(ui_, strf("mm-mode-%d", m.value), R(c.x + 10, yy, c.width - 20, 32), f.mmMode == m.value, m.label, m.hint, enabled, !m.built))
                {
                    f.mmMode = m.value;
                    markDirty();
                }
                yy += 34;
            }
            y += h + kGap;
        }
        // GT4's own Multi Monitor Mode (live): the screens found, the rig, the top screen, how to start a race.
        if (own)
        {
            const Rectangle c = R(x0, y, lw, bottom - y);
            fullui::card(ui_, c, "GT4's own Multi Monitor Mode", {Pill::Experimental, Pill::Full});
            float yy = c.y + 48;
            const bool on = ownLive; // (set while GT4 runs too: read at its next start)
            if (!ownLive)
            {
                yy += ui_.wrap("This GT4 program does not have GT4's own Multi Monitor Mode yet: use the GT4 program that came with this launcher.",
                               c.x + 20, yy, c.width - 40, 12.5f, t.warn);
                yy += 6;
            }
            auto row = [&](const char *id, const char *label, double &value, float lo, float hi, float step, const std::string &shown) {
                ui_.text(label, c.x + 20, yy + 6, 13.5f, t.faint);
                float v = static_cast<float>(value);
                if (ui_.slider(id, R(c.x + 190, yy, c.width - 290, 28), v, lo, hi, step, on))
                {
                    value = v;
                    markDirty();
                }
                ui_.text(shown, c.x + c.width - 20, yy + 6, 13.5f, t.text, Weight::Semibold, Align::Right);
                yy += 32;
            };
            row("mmo-w", "Screen picture width", f.mmScreenW, 300.0f, 1500.0f, 0.5f, strf("%.1f mm", f.mmScreenW));
            row("mmo-h", "Screen picture height", f.mmScreenH, 150.0f, 900.0f, 0.5f, strf("%.1f mm", f.mmScreenH));
            row("mmo-gap", "Gap between pictures", f.mmBezel, 0.0f, 60.0f, 0.5f, f.mmPanorama ? std::string("not used") : strf("%.1f mm", f.mmBezel));
            double dist = f.mmDistanceCm;
            row("mmo-d", "Your eyes to the middle", dist, 30.0f, 200.0f, 1.0f, strf("%d cm", f.mmDistanceCm));
            if (static_cast<int>(std::lround(dist)) != f.mmDistanceCm)
            {
                f.mmDistanceCm = static_cast<int>(std::lround(dist));
                markDirty();
            }
            int sides = f.mmPanorama ? 2 : f.mmAngled ? 0 : 1;
            ui_.text("Side screens", c.x + 20, yy + 7, 13.5f, t.faint);
            if (ui_.segmented("mmo-sides", R(c.x + 150, yy, c.width - 170, 30),
                              {strf("Angled (%.0f\xc2\xb0)", fullMmGt4Angle(f)), "Flat", "Panorama"}, sides, on, 12.5f))
            {
                f.mmAngled = sides == 0;
                f.mmAutoAngle = true;
                f.mmPanorama = sides == 2;
                markDirty();
            }
            yy += 38;
            int top = std::clamp(f.mmTop, 0, 2);
            ui_.text("Top screen", c.x + 20, yy + 7, 13.5f, t.faint);
            if (ui_.segmented("mmo-top", R(c.x + 150, yy, c.width - 170, 30), {"None", "Top centre view", "Live view"}, top, on, 12.5f))
            {
                f.mmTop = top;
                markDirty();
            }
            yy += 40;
            // The screens found (the middle one: Display > Screen shape).
            std::string found;
            bool missing = false;
            for (const FullMmCopy &cp : fullMmCopies(f))
            {
                const MonitorInfo *m = mmMonitor(cp.role);
                missing = missing || !m;
                found += std::string(found.empty() ? "" : "   ") + (cp.role == "center" ? "Middle" : cp.role == "left" ? "Left" : cp.role == "right" ? "Right" : "Top") +
                         ": " + (m ? m->name : std::string("none found"));
            }
            yy += ui_.wrap(found + ".", c.x + 20, yy, c.width - 40, 12.5f, missing ? t.warn : t.muted) + 6;
            yy += ui_.wrap("Play starts one GT4 per screen. With your controller on the middle screen go to Arcade Mode > Multi-LAN Race: "
                           "the other screens follow you there, connect and wait. Pick the course and the car on the middle screen; "
                           "the others show the race from their side.",
                           c.x + 20, yy, c.width - 40, 12.5f, t.faint) +
                  4;
            if (yy + 40 < c.y + c.height)
                ui_.wrap("GT4's Multi-LAN races have no AI cars. Only the middle screen has sound and your wheel; each screen's GT4 uses a copy of "
                         "your memory card, so your save is never shared.",
                         c.x + 20, yy, c.width - 40, 12.5f, t.faint);
        }
        // Three screens (live)
        else
        {
            const Rectangle c = R(x0, y, lw, bottom - y);
            fullui::card(ui_, c, "Three screens, one wide picture", {Pill::Experimental, Pill::Full});
            float yy = c.y + 48;
            const bool on = live; // (set while GT4 runs too: read at its next start)
            if (!live)
            {
                yy += ui_.wrap("This GT4 program does not have three screens yet: use the GT4 program that came with this launcher.", c.x + 20, yy, c.width - 40, 12.5f,
                               t.warn);
                yy += 6;
            }
            auto row = [&](const char *id, const char *label, double &value, float lo, float hi, const std::string &shown) {
                ui_.text(label, c.x + 20, yy + 6, 13.5f, t.faint);
                float v = static_cast<float>(value);
                if (ui_.slider(id, R(c.x + 190, yy, c.width - 290, 28), v, lo, hi, 0.5f, on))
                {
                    value = v;
                    markDirty();
                }
                ui_.text(shown, c.x + c.width - 20, yy + 6, 13.5f, t.text, Weight::Semibold, Align::Right);
                yy += 34;
            };
            row("mm-w", "Screen picture width", f.mmScreenW, 300.0f, 1500.0f, strf("%.1f mm", f.mmScreenW));
            row("mm-h", "Screen picture height", f.mmScreenH, 150.0f, 900.0f, strf("%.1f mm", f.mmScreenH));
            row("mm-gap", "Gap between pictures", f.mmBezel, 0.0f, 60.0f, f.mmPanorama ? std::string("not used") : strf("%.1f mm", f.mmBezel));
            ui_.text(ui_.fit("Panorama (nothing hidden behind the frames)", c.width - 100, 13.5f), c.x + 20, yy + 2, 13.5f, t.faint);
            if (ui_.toggle("mm-panorama", c.x + c.width - 60, yy, f.mmPanorama, on))
                markDirty();
            yy += 32;
            // The middle monitor and its neighbours, as GT4.exe picks them (gt4ThreeScreens: right next to it, at least half
            // its height shared).
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
            const std::string where = mid ? strf("Middle screen: %s (Display > Screen shape). Left: %s. Right: %s.", mid->name.c_str(), nm(l).c_str(), nm(r).c_str())
                                          : std::string("Middle screen: the monitor GT4 opens on.");
            yy += ui_.wrap(where, c.x + 20, yy, c.width - 40, 12.5f, three && mid && (!l || !r) ? t.warn : t.muted) + 4;
            yy += ui_.wrap("The race HUD and the mirror stay on the middle screen; GT4's menus are 16:9 on the middle screen. The picture is "
                           "flat: GT4's view simply goes on across the side screens.",
                           c.x + 20, yy, c.width - 40, 12.5f, t.faint) +
                  4;
            if (yy + 30 < c.y + c.height)
                ui_.wrap("Use 2x or 3x (Display > Resolution) so the side screens stay sharp.", c.x + 20, yy, c.width - 40, 12.5f, t.faint);
        }
        // Right: GT4's view across the screens. (The test builds' "More screens and angled sides" card, choices not built yet, is
        // gone: it left the rig picture no room, developer 2026-10-19 "your rig from above dosen't seem to be showing correctly".)
        const float rx = x0 + lw + kGap;
        const float nbH = 0.0f;
        const float teleH = 0.0f; // (the Telemetry screen card is on its own page now)
        {
            const Rectangle c = R(rx, kTop, x1 - rx, bottom - kTop - (nbH > 0 ? nbH + kGap : 0.0f) - (teleH > 0 ? teleH + kGap : 0.0f));
            ui_.card(c, three ? "GT4's view across your screens" : "Your rig from above");
            // GT4's own mode: GT4's angled screens face you at its own angle for the distance (the rig view's auto angle).
            const RigView rv = rigView(f.mmScreenW, f.mmBezel, f.mmDistanceCm * 10.0, own ? f.mmAngled && !f.mmPanorama : f.mmAngled,
                                       own ? true : f.mmAutoAngle, f.mmAngleDeg);
            const double pi = 3.14159265358979323846, W = f.mmScreenW, g = three && f.mmPanorama ? 0.0 : f.mmBezel, d = f.mmDistanceCm * 10.0;
            const double th = three ? 0.0 : rv.sideAngleDeg * pi / 180.0; // the wide picture is one flat plane
            const float area = std::max(40.0f, std::min(c.width - 60, (c.height - 150) * 1.7f));
            const float k = area / static_cast<float>(W * 3.2);
            const float ex = c.x + c.width * 0.5f, ey = c.y + 70 + static_cast<float>(d) * k;
            auto P = [&](double x, double z) { return Vector2{ex + static_cast<float>(x) * k, ey - static_cast<float>(z) * k}; };
            const Vector2 eye = P(0, 0);
            const double fx = W / 2 + g + W * std::cos(th), fz = d - W * std::sin(th);
            ui_.line(eye.x, eye.y, P(fx, fz).x, P(fx, fz).y, 1.0f, ui::alpha(t.accent, 0.4f));
            ui_.line(eye.x, eye.y, P(-fx, fz).x, P(-fx, fz).y, 1.0f, ui::alpha(t.accent, 0.4f));
            auto screen = [&](double x1s, double z1s, double x2s, double z2s, Color col) {
                const Vector2 a = P(x1s, z1s), b = P(x2s, z2s);
                ui_.line(a.x, a.y, b.x, b.y, 5.0f, col);
            };
            screen(-W / 2, d, W / 2, d, t.accent);
            screen(W / 2 + g, d, fx, fz, ui::mixColor(t.accent, t.muted, 0.3f));
            screen(-W / 2 - g, d, -fx, fz, ui::mixColor(t.accent, t.muted, 0.3f));
            ui_.circle(eye.x, eye.y, 6, t.text);
            ui_.text("you", eye.x, eye.y + 10, 12, t.muted, Weight::Regular, Align::Center);
            float yy = ey + 40;
            if (three)
            {
                // GT4's own 16:9 view on the middle screen (425 mm wide at 276 mm, GT4's preset), widened across the three
                // screens and the gaps: the same height of view, so the cars are the size they are on one screen.
                const double a = W / std::max(1.0, f.mmScreenH), units = 3.0 + 2.0 * g / W;
                const double middle = 2.0 * std::atan(425.0 * a * 9.0 / 16.0 / 2.0 / 276.0) * 180.0 / pi;
                const double across = 2.0 * std::atan(425.0 * a * units * 9.0 / 16.0 / 2.0 / 276.0) * 180.0 / pi;
                ui_.text(strf("%.0f\xc2\xb0 of the track across the three screens", across), c.x + 20, yy, 18, t.text, Weight::Bold);
                yy += 30;
                ui_.text(ui_.fit(strf("The middle screen shows GT4's own %.0f\xc2\xb0 view; the cars are the size they are on one screen", middle), c.width - 40, 13.5f),
                         c.x + 20, yy, 13.5f, t.muted);
                yy += 24;
                ui_.wrap(g > 0 ? "What lies behind the frames is left out, so lines run straight across them, as through a car's pillars."
                               : "Panorama: one continuous picture; lines step a little at the frames.",
                         c.x + 20, yy, c.width - 40, 12.5f, t.faint);
            }
            else
            {
                ui_.text(strf("%.0f\xc2\xb0 seen across the three screens", rv.totalFovDeg), c.x + 20, yy, 18, t.text, Weight::Bold);
                yy += 30;
                ui_.text(strf("Middle screen %.1f\xc2\xb0 \xc2\xb7 side screens at %.1f\xc2\xb0", rv.centreFovDeg, rv.sideAngleDeg), c.x + 20, yy, 13.5f, t.muted);
                yy += 24;
                ui_.wrap(own ? "Each screen's GT4 draws its own view: the side ones turned to face you (Angled), or the picture going on flat "
                               "across them (Flat, Panorama). Measure your side screens' real angle and your eyes' distance to set this."
                             : "Pick \"Three screens, one wide picture\" to play GT4 across them.",
                         c.x + 20, yy, c.width - 40, 12.5f, t.faint);
            }
        }
        // (The Telemetry screen card moved to its own page, Driving > Telemetry screen: gt4full_telemetry.cpp, HUD hand-off 8.)
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
