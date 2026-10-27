// GT4 Recompilation launcher, Full edition: Driving > Telemetry screen (HUD hand-off 8,
// analysis/design/hud-pages/handoff7/HANDOFF-8-TELEMETRY-SCREEN-TAB.md). Three tabs:
//   Monitor: the monitors as Windows arranges them (EDID names, resolution, Hz, each one's job), the one picked for the
//     telemetry window (GT4_TELEMETRY_MONITOR=<DISPLAYn>,<name>,<W>x<H>,<x>,<y>; shared/gt4_tele_monitors.h), Identify
//     monitors, Refresh list, Show it now.
//   Phone or tablet: the pages on the home network (GT4_PAGES=lan) and a QR code of this PC's address (gt4full_qr.h).
//   Layouts: the monitor's, the phone upright and the phone sideways layouts (telemetry-layout.json beside gt4-start.env,
//     gt4full_telelayout.h), boxes dragged and resized with gt4full_boxedit.h.
// The rear-view mirror in a layout in use writes GT4_MIRROR=page (GT4's own mirror off the game screen), or both when the
// player keeps GT4's mirror on the game screen too (GT4L_TELE_MIRROR; gt4full_settings.cpp).

#include "gt4stock_app.h"

#include "gt4full_boxedit.h"
#include "gt4full_qr.h"
#include "gt4full_telelayout.h"
#include "gt4full_telesys.h"
#include "gt4full_ui.h"
#include "gt4stock_system.h"
#include "gt4stock_util.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <fstream>
#include <sstream>

namespace gt4stock
{
    using ui::Align;
    using ui::Style;
    using ui::Weight;
    using fullui::Pill;

    struct TeleState
    {
        bool loaded = false;
        bool dirty = false;          // the layouts changed since the file was written
        tele::Layout layout;
        int tab = 0;                 // 0 Monitor, 1 Phone or tablet, 2 Layouts
        int target = tele::Monitor;
        boxedit::State edit[tele::kTargets];
        bool snap = true;
        std::vector<gt4tele::Monitor> mons;
        double monsAt = -100.0;
        std::string lan, adapter;
        double lanAt = -100.0;
        qr::Code code;
        std::string codeFor;
        sys::Process show;           // "Show it now": Edge's window
        std::string note;            // the Monitor tab's last message
    };

    namespace
    {
        constexpr float kSidebarW = 232.0f;
        constexpr float kTop = 104.0f;
        Rectangle R(float x, float y, float w, float h) { return Rectangle{x, y, w, h}; }
        const char *const kDot = " \xc2\xb7 ";

        std::string fileUrl(const fs::path &p)
        {
            std::string s = pathText(p), out = "file:///";
            for (char c : s)
            {
                if (c == '\\')
                    out += '/';
                else if (c == ' ')
                    out += "%20";
                else if (c == '#')
                    out += "%23";
                else if (c == '&')
                    out += "%26";
                else
                    out += c;
            }
            return out;
        }
        // A query value: every byte outside A-Z a-z 0-9 - _ . ~ as %XX.
        std::string queryValue(const std::string &s)
        {
            static const char *hex = "0123456789ABCDEF";
            std::string out;
            for (unsigned char c : s)
            {
                if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~')
                    out += static_cast<char>(c);
                else
                {
                    out += '%';
                    out += hex[c >> 4];
                    out += hex[c & 15];
                }
            }
            return out;
        }
    } // namespace

    TeleState &App::teleState()
    {
        if (!tele_)
            tele_ = std::make_shared<TeleState>();
        TeleState &T = *tele_;
        if (!T.loaded)
        {
            T.loaded = true;
            T.layout = tele::defaultLayout();
            std::ifstream in(teleLayoutFile(), std::ios::binary);
            bool read = false;
            if (in)
            {
                std::stringstream ss;
                ss << in.rdbuf();
                read = tele::fromJson(ss.str(), T.layout);
            }
            // (the file's background wins; GT4_TELE_BG follows it)
            if (read)
                st_.full.teleBlack = T.layout.black;
            else
                T.layout.black = st_.full.teleBlack;
        }
        return T;
    }

    fs::path App::teleLayoutFile() const { return inst_.settingsFile.parent_path() / L"telemetry-layout.json"; }

    // Writes telemetry-layout.json when the layouts changed, or when a telemetry screen is on and the file is not there yet
    // (GT4.exe serves it at /layout; with it the pages draw the layouts at / too).
    void App::teleWrite()
    {
        if (!tele_ || !tele_->loaded || (hidden_ && !opt_.saveSettings))
            return;
        TeleState &T = *tele_;
        std::error_code ec;
        const fs::path file = teleLayoutFile();
        const bool inUse = (st_.full.telemetryScreen > 0 && !st_.full.telemetryRect.empty()) || st_.full.phonePages;
        if (!T.dirty && !(inUse && !fs::exists(file, ec)))
            return;
        std::ofstream out(file, std::ios::binary | std::ios::trunc);
        if (out)
        {
            out << tele::toJson(T.layout);
            T.dirty = false;
        }
    }

    // GT4L_TELE_MIRROR: the telemetry screens in use show the mirror. When that starts, GT4's own mirror leaves the game screen
    // (developer, 20 Oct: the rear view on the telemetry monitor or the phone in every view, not on the game screen) unless the
    // player keeps it there (the tick on this page or the Race HUD page).
    void App::teleMirrorCheck()
    {
        TeleState &T = teleState();
        FullSettings &f = st_.full;
        const bool mon = f.telemetryScreen > 0 && !f.telemetryRect.empty();
        const bool want = (mon && tele::has(T.layout.t[tele::Monitor], "mirror")) ||
                          (f.phonePages && (tele::has(T.layout.t[tele::Upright], "mirror") || tele::has(T.layout.t[tele::Sideways], "mirror")));
        // (21 Oct, developer: GT4's own mirror stays on the game screen in the bumper view; the pages get the rear view in every
        // view whatever GT4_MIRROR is, so the mirror in a layout no longer takes it off the screen)
        if (want != f.telemetryMirror)
        {
            f.telemetryMirror = want;
            markDirty();
        }
    }

    void App::pageTelemetry()
    {
        const ui::Theme &t = ui::theme();
        FullSettings &f = st_.full;
        TeleState &T = teleState();
        drawHeader("Telemetry screen", "Live timing, the track map and the rear-view mirror on another monitor, or on a phone or tablet.");
        const float x0 = kSidebarW + 32, x1 = ui_.width - 32, bottom = ui_.height - 48, cw = x1 - x0;
        if (fullui::tabs(ui_, "tele-tabs", x0, kTop - 10, cw, {"Monitor", "Phone or tablet", "Layouts"}, T.tab))
            ui_.closePopups();
        const float top = kTop + 40;

        // ---- the monitors (Windows' own list; the launcher's demo list in test pictures)
        if (opt_.demo)
        {
            T.mons.clear();
            for (const MonitorInfo &m : monitors_)
            {
                gt4tele::Monitor g;
                g.device = strf("DISPLAY%d", m.index + 1);
                g.friendly = m.name;
                g.x = m.x, g.y = m.y, g.w = m.w, g.h = m.h, g.hz = m.hz;
                g.primary = m.primary;
                T.mons.push_back(g);
            }
        }
        else if (GetTime() - T.monsAt > 5.0)
        {
            T.mons = sys::teleMonitors();
            T.monsAt = GetTime();
        }
        const gt4tele::Setting setting = gt4tele::parse(f.telemetryRect);
        const bool monOn = f.telemetryScreen > 0 && setting.kind != gt4tele::Setting::Off;
        const int picked = monOn || setting.kind != gt4tele::Setting::Off ? gt4tele::match(setting, T.mons) : -1;
        // Game screens (Multi-screen and Display > Screen shape): the middle one, and the side ones of the three-screen modes.
        auto gameRole = [&](const gt4tele::Monitor &g) -> std::string {
            auto same = [&](const MonitorInfo *m) { return m && m->x == g.x && m->y == g.y && m->w == g.w && m->h == g.h; };
            const bool three = f.mmMode == kMmThreeWide, own = mmOwnChosen();
            if (same(mmMonitor("center")))
                return three || own ? "Game \xc2\xb7 centre" : "Game";
            if (three || own)
            {
                if (same(mmMonitor("left")))
                    return "Game \xc2\xb7 left";
                if (same(mmMonitor("right")))
                    return "Game \xc2\xb7 right";
                if (own && same(mmMonitor("top")))
                    return "Game \xc2\xb7 top";
            }
            return {};
        };
        auto pickMonitor = [&](int i) {
            const gt4tele::Monitor &m = T.mons[static_cast<size_t>(i)];
            f.telemetryScreen = i + 1;
            f.telemetryRect = gt4tele::format(m);
            markDirty();
            teleMirrorCheck();
        };
        auto mirrorTick = [&](float x, float y, float w) {
            bool onScreen = f.mirrorMode != 2;
            ui_.text(ui_.fit("GT4's own mirror on the game screen too", w - 60, 13.5f), x, y + 2, 13.5f, t.faint);
            if (ui_.toggle("tele-mirror-screen", x + w - 44, y, onScreen, !busy()))
            {
                f.mirrorMode = onScreen ? 0 : 2; // (on: GT4's own, the bumper view; the pages: every view)
                markDirty();
            }
        };

        if (T.tab == 0)
        {
            // ---- Monitor: the diagram and the buttons
            const float listW = std::min(400.0f, cw * 0.36f);
            const Rectangle c = R(x0, top, cw - listW - 16, bottom - top);
            fullui::card(ui_, c, "Your monitors", {Pill::Full});
            float y = c.y + 48;
            {
                bool on = monOn;
                ui_.text("Telemetry window on a monitor", c.x + 20, y + 3, 14, t.text);
                if (ui_.toggle("tele-mon-on", c.x + c.width - 64, y, on, !busy()))
                {
                    if (on)
                    {
                        int i = picked;
                        if (i < 0)
                            for (int k = static_cast<int>(T.mons.size()) - 1; k >= 0 && i < 0; --k)
                                if (gameRole(T.mons[static_cast<size_t>(k)]).empty())
                                    i = k;
                        if (i >= 0)
                            pickMonitor(i);
                        else
                            T.note = "Every monitor shows the game: connect another one for the telemetry window.";
                    }
                    else
                    {
                        f.telemetryScreen = 0;
                        markDirty();
                        teleMirrorCheck();
                    }
                }
                y += 34;
            }
            // The diagram: Windows' arrangement, fitted.
            const Rectangle area = R(c.x + 20, y, c.width - 40, c.y + c.height - y - 124);
            if (!T.mons.empty() && area.height > 40)
            {
                int bx0 = INT32_MAX, by0 = INT32_MAX, bx1 = INT32_MIN, by1 = INT32_MIN;
                for (const gt4tele::Monitor &m : T.mons)
                    bx0 = std::min(bx0, m.x), by0 = std::min(by0, m.y), bx1 = std::max(bx1, m.x + m.w), by1 = std::max(by1, m.y + m.h);
                const float k = std::min(area.width / std::max(1, bx1 - bx0), area.height / std::max(1, by1 - by0));
                const float ox = area.x + (area.width - (bx1 - bx0) * k) * 0.5f, oy = area.y + (area.height - (by1 - by0) * k) * 0.5f;
                for (size_t i = 0; i < T.mons.size(); ++i)
                {
                    const gt4tele::Monitor &m = T.mons[i];
                    const Rectangle r = R(ox + (m.x - bx0) * k + 3, oy + (m.y - by0) * k + 3, m.w * k - 6, m.h * k - 6);
                    const std::string role = gameRole(m);
                    const bool game = !role.empty(), mine = monOn && picked == static_cast<int>(i);
                    const bool hot = ui_.hovered(r) && !busy();
                    ui_.round(r, 8, game ? ui::alpha(t.panel3, 0.7f) : mine ? ui::alpha(t.accent, 0.22f) : hot ? t.panel3 : t.panel2);
                    if (game)
                    {
                        // striped: the game's screen (not a choice here)
                        for (float s = -r.height; s < r.width; s += 14)
                        {
                            const float ax = std::max(r.x, r.x + s), ay = s < 0 ? r.y - s : r.y;
                            const float len = std::min(r.x + r.width - ax, r.y + r.height - ay);
                            if (len > 0)
                                ui_.line(ax, ay, ax + len, ay + len, 2.0f, ui::alpha(t.faint, 0.18f));
                        }
                    }
                    ui_.roundLine(r, 8, mine ? 2.5f : 1.0f, mine ? t.accent : t.border);
                    const float big = std::clamp(r.height * 0.28f, 16.0f, 44.0f);
                    ui_.text(std::to_string(i + 1), r.x + 12, r.y + 8, big, mine ? t.accent : t.text, Weight::Bold);
                    float ty = r.y + 12 + big;
                    const float tw = r.width - 24;
                    if (r.height > 70)
                    {
                        ui_.text(ui_.fit(m.friendly, tw, 13.5f, Weight::Semibold), r.x + 12, ty, 13.5f, t.text, Weight::Semibold);
                        ty += 19;
                    }
                    if (r.height > 92)
                    {
                        ui_.text(ui_.fit(strf("%dx%d%s%d Hz", m.w, m.h, kDot, m.hz), tw, 12.5f), r.x + 12, ty, 12.5f, t.muted);
                        ty += 18;
                    }
                    const std::string job = game ? role : mine ? std::string("Telemetry") : std::string("Free");
                    ui_.chip(r.x + 12, std::min(ty + 2, r.y + r.height - 26), ui_.fit(job, tw - 12, 12), game ? t.panel2 : mine ? t.accent : t.panel3,
                             game ? t.muted : mine ? WHITE : t.muted, 12);
                    if (m.primary && r.width > 120)
                        ui_.text("main", r.x + r.width - 12, r.y + 12, 12, t.faint, Weight::Regular, Align::Right);
                    if (ui_.clicked(r) && !busy())
                    {
                        if (game)
                            T.note = strf("Monitor %d shows the game: change the game's screens on Multi-screen (and Display > Screen shape).", static_cast<int>(i) + 1);
                        else
                        {
                            pickMonitor(static_cast<int>(i));
                            T.note.clear();
                        }
                    }
                }
            }
            else if (T.mons.empty())
                ui_.text("Windows reports no monitors.", area.x, area.y + 10, 14, t.muted);
            // The picked monitor's line.
            float ly = c.y + c.height - 116;
            std::string line;
            Color lc = t.muted;
            if (!monOn)
                line = "No telemetry window. Click a free monitor to show it there.";
            else if (picked < 0)
            {
                line = setting.kind == gt4tele::Setting::Named
                           ? strf("Not connected: %s (%s, %dx%d at %d,%d). No window opens until it is back.", setting.friendly.c_str(),
                                  setting.device.c_str(), setting.w, setting.h, setting.x, setting.y)
                           : std::string("Not connected: the monitor chosen before is not there now. Pick one above.");
                lc = t.warn;
            }
            else
            {
                const gt4tele::Monitor &m = T.mons[static_cast<size_t>(picked)];
                line = strf("The telemetry window opens on monitor %d, %s (%s), full screen, behind nothing: it never takes the focus or the controller.",
                            picked + 1, m.friendly.c_str(), m.device.c_str());
            }
            ly += ui_.wrap(T.note.empty() ? line : T.note, c.x + 20, ly, c.width - 40, 12.5f, T.note.empty() ? lc : t.warn) + 6;
            // The buttons.
            const float by = c.y + c.height - 56;
            if (ui_.button("tele-identify", R(c.x + 20, by, 170, 36), "Identify monitors", Style::Secondary, !hidden_ && !T.mons.empty()))
                sys::identifyMonitors(T.mons);
            if (ui_.button("tele-refresh", R(c.x + 200, by, 130, 36), "Refresh list", Style::Secondary))
            {
                T.monsAt = -100.0;
                T.note.clear();
            }
            {
                int code = 0;
                if (T.show.handle && sys::poll(T.show, code))
                    T.show = sys::Process{};
                const bool showing = T.show.handle != nullptr;
                const bool can = !busy() && monOn && picked >= 0;
                if (ui_.button("tele-show", R(c.x + 340, by, 150, 36), showing ? "Close it" : "Show it now", showing ? Style::Secondary : Style::Primary,
                               showing || can))
                {
                    if (showing)
                    {
                        sys::terminate(T.show);
                        T.show = sys::Process{};
                    }
                    else
                    {
                        const fs::path page = inst_.exe.parent_path() / L"pages" / L"telemetry.html";
                        const std::string edge = sys::edgePath();
                        std::error_code ec;
                        if (edge.empty())
                            T.note = "Microsoft Edge is not installed: GT4 needs it for the telemetry window. The phone's address works in any browser.";
                        else if (!fs::exists(page, ec))
                            T.note = "There is no pages folder next to the GT4 program.";
                        else
                        {
                            T.dirty = true;
                            teleWrite();
                            const gt4tele::Monitor &m = T.mons[static_cast<size_t>(picked)];
                            sys::Launch l;
                            l.exe = fs::path(wide(edge));
                            // (the layouts in the address: a page opened from disk cannot fetch the file; telemetry.html reads
                            // ?layoutjson=)
                            const std::string url = fileUrl(page) + "?screen=monitor&layoutjson=" + queryValue(tele::toJson(T.layout));
                            l.args = {L"--app=" + wide(url), L"--user-data-dir=" + (page.parent_path() / L"edge-profile").wstring(),
                                      L"--no-first-run", L"--no-default-browser-check",
                                      wide(strf("--window-position=%d,%d", m.x, m.y)), wide(strf("--window-size=%d,%d", m.w, m.h)),
                                      L"--start-fullscreen"};
                            l.cwd = page.parent_path();
                            std::string err;
                            if (!sys::start(l, T.show, err))
                                T.note = "Edge did not start: " + err;
                            else
                                T.note = "Shown now: \"Waiting for a race\" until GT4 is in a race. GT4 opens it by itself when it starts.";
                        }
                    }
                }
                if (busy())
                    ui_.text("GT4 is running: it opens the window itself.", c.x + 500, by + 10, 12.5f, t.faint);
            }
            // ---- the list on the right
            const Rectangle lc2 = R(x0 + cw - listW, top, listW, bottom - top);
            fullui::card(ui_, lc2, "As Windows reports them");
            float yy = lc2.y + 48;
            for (size_t i = 0; i < T.mons.size() && yy < lc2.y + lc2.height - 150; ++i)
            {
                const gt4tele::Monitor &m = T.mons[i];
                const std::string role = gameRole(m);
                const bool mine = monOn && picked == static_cast<int>(i);
                ui_.text(strf("%d", static_cast<int>(i) + 1), lc2.x + 20, yy, 15, mine ? t.accent : t.text, Weight::Bold);
                ui_.text(ui_.fit(m.friendly, lc2.width - 70, 14, Weight::Semibold), lc2.x + 44, yy, 14, t.text, Weight::Semibold);
                ui_.text(ui_.fit(strf("%s%s%dx%d%s%d Hz%sat %d, %d", m.device.c_str(), kDot, m.w, m.h, kDot, m.hz, kDot, m.x, m.y), lc2.width - 70, 12.5f),
                         lc2.x + 44, yy + 20, 12.5f, t.muted);
                ui_.text(role.empty() ? (mine ? "Telemetry" : "Free") : role, lc2.x + 44, yy + 38, 12.5f, mine ? t.accent : t.faint);
                yy += 64;
            }
            float my = lc2.y + lc2.height - 140;
            ui_.fill(R(lc2.x + 20, my - 10, lc2.width - 40, 1), t.border);
            const bool monMirror = tele::has(T.layout.t[tele::Monitor], "mirror");
            my += ui_.wrap(monMirror ? "The monitor's layout has the rear-view mirror: GT4 sends it in every camera view (Layouts tab)."
                                     : "The monitor's layout has no rear-view mirror (Layouts tab).",
                           lc2.x + 20, my, lc2.width - 40, 12.5f, t.muted) +
                  10;
            mirrorTick(lc2.x + 20, my, lc2.width - 40);
            my += 32;
            ui_.wrap("Windows can renumber displays after a driver update or replugging: GT4 finds the monitor by its name and place first.",
                     lc2.x + 20, my, lc2.width - 40, 12, t.faint);
        }
        else if (T.tab == 1)
        {
            // ---- Phone or tablet
            if (opt_.demo)
                T.lan = "192.168.1.20", T.adapter = "Wi-Fi";
            else if (GetTime() - T.lanAt > 5.0)
            {
                T.lan = sys::lanAddress(&T.adapter);
                T.lanAt = GetTime();
            }
            const int port = 8770;
            const std::string url = T.lan.empty() ? std::string() : strf("http://%s:%d/", T.lan.c_str(), port);
            if (url != T.codeFor)
            {
                T.code = url.empty() ? qr::Code{} : qr::encode(url);
                T.codeFor = url;
            }
            const float qw = std::min(420.0f, cw * 0.42f);
            const Rectangle c = R(x0, top, cw - qw - 16, bottom - top);
            fullui::card(ui_, c, "Phone or tablet", {Pill::Full});
            float y = c.y + 48;
            ui_.text("Phones and tablets on this home network", c.x + 20, y + 3, 14, t.text);
            if (ui_.toggle("tele-phone-on", c.x + c.width - 64, y, f.phonePages, !busy()))
            {
                markDirty();
                teleMirrorCheck();
            }
            y += 36;
            y += ui_.wrap(f.phonePages ? "On: GT4 serves the pages to this home network while it runs (port 8770). Nothing goes online."
                                       : "Off: the pages stay on this PC (the telemetry monitor only).",
                          c.x + 20, y, c.width - 40, 13, f.phonePages ? t.muted : t.faint) +
                 12;
            y += ui_.wrap("The phone picks its layout from how you hold it: upright or sideways, and switches when it turns. Both layouts are "
                          "on the Layouts tab.",
                          c.x + 20, y, c.width - 40, 13, t.muted) +
                 12;
            const bool up = tele::has(T.layout.t[tele::Upright], "mirror"), side = tele::has(T.layout.t[tele::Sideways], "mirror");
            y += ui_.wrap(up || side ? strf("Rear-view mirror on the phone: %s. GT4 sends it in every camera view.", up && side ? "upright and sideways" : up ? "upright" : "sideways")
                                     : std::string("The phone layouts have no rear-view mirror."),
                          c.x + 20, y, c.width - 40, 13, t.muted) +
                 12;
            mirrorTick(c.x + 20, y, c.width - 40);
            y += 40;
            ui_.wrap("Windows may ask once whether GT4 may use the network: allow it on private networks.", c.x + 20, y, c.width - 40, 12.5f, t.faint);
            // The code.
            const Rectangle q = R(x0 + cw - qw, top, qw, bottom - top);
            fullui::card(ui_, q, "Scan to connect");
            const float side2 = std::min(q.width - 60, q.height - 190);
            const Rectangle qr = R(q.x + (q.width - side2) * 0.5f, q.y + 50, side2, side2);
            ui_.round(qr, 10, f.phonePages ? WHITE : ui::alpha(WHITE, 0.25f));
            if (T.code.size > 0)
            {
                const float mod = (side2 - 16) / static_cast<float>(T.code.size + 8);
                const float ox = qr.x + (side2 - mod * T.code.size) * 0.5f, oy = qr.y + (side2 - mod * T.code.size) * 0.5f;
                const Color ink = f.phonePages ? BLACK : ui::alpha(BLACK, 0.35f);
                for (int yy = 0; yy < T.code.size; ++yy)
                    for (int xx = 0; xx < T.code.size; ++xx)
                        if (T.code.at(xx, yy))
                            ui_.fill(R(ox + xx * mod, oy + yy * mod, mod + 0.35f, mod + 0.35f), ink);
            }
            else
                ui_.text("No home network found", qr.x + side2 * 0.5f, qr.y + side2 * 0.5f - 8, 14, t.bad, Weight::Semibold, Align::Center);
            float ty = qr.y + side2 + 14;
            ui_.text(url.empty() ? "No address" : url, q.x + q.width * 0.5f, ty, 17, f.phonePages ? t.text : t.faint, Weight::Semibold, Align::Center);
            ty += 28;
            ty += ui_.wrap("Scan with the phone's camera. It opens the mirror, track map and timing in the phone's browser. Same Wi-Fi as this PC.",
                           q.x + 24, ty, q.width - 48, 12.5f, f.phonePages ? t.muted : t.faint) +
                  4;
            if (!f.phonePages)
                ui_.wrap("Turn on \"Phones and tablets on this home network\" first.", q.x + 24, ty, q.width - 48, 12.5f, t.warn);
            else if (!T.adapter.empty())
                ui_.text(ui_.fit("Network: " + T.adapter, q.width - 48, 12), q.x + 24, ty, 12, t.faint);
        }
        else
            teleEditor(x0, top, x1, bottom);
    }

    // ---- Layouts: the three targets' boxes
    void App::teleEditor(float x0, float top, float x1, float bottom)
    {
        const ui::Theme &t = ui::theme();
        FullSettings &f = st_.full;
        TeleState &T = teleState();
        const float colW = 330.0f, gap = 20.0f;
        const gt4tele::Setting setting = gt4tele::parse(f.telemetryRect);
        const int picked = gt4tele::match(setting, T.mons);
        const gt4tele::Monitor *pm = picked >= 0 ? &T.mons[static_cast<size_t>(picked)] : nullptr;
        {
            const std::string monLabel = pm ? strf("Monitor%sDisplay %d", kDot, picked + 1) : std::string("Monitor");
            int tg = T.target;
            if (ui_.segmented("tele-target", R(x0, top, std::min(560.0f, x1 - x0 - colW - gap), 32), {monLabel, "Phone upright", "Phone sideways"}, tg))
            {
                T.target = tg;
                ui_.closePopups();
            }
        }
        std::vector<tele::Item> &items = T.layout.t[T.target];
        boxedit::State &es = T.edit[T.target];
        // The stage: the target's shape, fitted to the room left.
        float aspect = 16.0f / 9.0f;
        if (T.target == tele::Monitor && pm && pm->h > 0)
            aspect = static_cast<float>(pm->w) / static_cast<float>(pm->h);
        else if (T.target == tele::Upright)
            aspect = 9.0f / 19.5f;
        else if (T.target == tele::Sideways)
            aspect = 19.5f / 9.0f;
        const float roomW = x1 - x0 - colW - gap, roomTop = top + 44, roomH = bottom - roomTop - 44;
        float sw = roomW, sh = sw / aspect;
        if (sh > roomH)
            sh = roomH, sw = sh * aspect;
        const Rectangle S = R(x0 + (roomW - sw) * 0.5f, roomTop, sw, sh);
        ui_.fill(R(S.x - 1, S.y - 1, S.width + 2, S.height + 2), t.border);
        ui_.fill(S, T.layout.black ? BLACK : Color{18, 25, 32, 255});
        for (int g = 1; g < 20; ++g)
        {
            ui_.fill(R(S.x + S.width * g / 20.0f, S.y, 1, S.height), ui::alpha(WHITE, 0.05f));
            ui_.fill(R(S.x, S.y + S.height * g / 20.0f, S.width, 1), ui::alpha(WHITE, 0.05f));
        }
        // Drag and resize (the boxes as % of the stage).
        std::vector<boxedit::Box> boxes;
        for (const tele::Item &i : items)
            boxes.push_back(boxedit::Box{i.x, i.y, i.w, i.h});
        boxedit::Options opt;
        opt.snap = T.snap ? 2.5f : 0.0f;
        boxedit::Mouse mouse;
        mouse.enabled = ui_.inputOn() && !ui_.popupOpen() && !busy();
        mouse.at = ui_.mouseDp();
        mouse.pressed = ui_.input().pressed;
        mouse.down = ui_.input().down;
        const boxedit::Keys keys = mouse.enabled && !hidden_ ? boxedit::keysNow() : boxedit::Keys{};
        if (boxedit::step(es, boxes, S, mouse, keys, opt))
        {
            for (size_t i = 0; i < items.size(); ++i)
                items[i].x = boxes[i].x, items[i].y = boxes[i].y, items[i].w = boxes[i].w, items[i].h = boxes[i].h;
            T.dirty = true;
            markDirty();
        }
        teleBoxDp_.clear();
        for (size_t i = 0; i < items.size(); ++i)
        {
            const Rectangle r = boxedit::onStage(boxes[i], S);
            teleBoxDp_.push_back(r);
            teleDrawItem(items[i], r, static_cast<int>(i) == es.sel);
            if (static_cast<int>(i) == es.sel)
            {
                const Rectangle h = boxedit::handle(boxes[i], S, opt);
                ui_.fill(h, t.accent);
                ui_.line(h.x + 4, h.y + h.height - 4, h.x + h.width - 4, h.y + 4, 1.5f, WHITE);
            }
        }
        const char *const help = "Drag a box to move it, its corner to resize it. Arrows move the picked box, Shift + arrows resize it.";
        ui_.text(ui_.fit(help, roomW, 12.5f), x0, S.y + S.height + 12, 12.5f, t.faint);

        // ---- the column: items, the picked one's options, the layout's buttons
        const float cx = x1 - colW;
        const Rectangle col = R(cx, top, colW, bottom - top);
        ui_.round(col, 12, t.panel);
        const float off = ui_.beginScroll("tele-col", col);
        float y = col.y + 14 - off;
        const float ix = cx + 16, iw = colW - 32;
        fullui::caps(ui_, ix, y, "ON THIS SCREEN", t.faint);
        y += 22;
        for (int k = 0; k < tele::kKindCount; ++k)
        {
            const tele::Kind &kind = tele::kKinds[k];
            const int at = tele::find(items, kind.k);
            bool on = at >= 0;
            const Rectangle row = R(ix - 6, y - 3, iw + 12, 26);
            const bool sel = on && at == es.sel;
            if (sel)
                ui_.round(row, 6, t.panel3);
            if (ui_.checkbox(std::string("tele-item-") + kind.k, ix, y, on, "", !busy()))
            {
                if (on)
                {
                    tele::Item it;
                    it.k = kind.k;
                    float w = kind.w, h = kind.h;
                    if (T.target == tele::Upright)
                        w = 94, h = std::clamp(kind.h * 0.35f, 5.0f, 35.0f);
                    else if (T.target == tele::Sideways)
                        w = std::min(47.0f, kind.w * 1.4f), h = std::clamp(kind.h * 0.8f, 10.0f, 60.0f);
                    it.w = w, it.h = h;
                    it.x = boxedit::snapTo(50 - w * 0.5f, 2.5f), it.y = boxedit::snapTo(50 - h * 0.5f, 2.5f);
                    it.x = std::clamp(it.x, 0.0f, 100.0f - w), it.y = std::clamp(it.y, 0.0f, 100.0f - h);
                    if (T.target == tele::Upright)
                        it.name = "short";
                    items.push_back(it);
                    es.sel = static_cast<int>(items.size()) - 1;
                }
                else
                {
                    items.erase(items.begin() + at);
                    es.sel = -1;
                    es.drag = -1;
                }
                T.dirty = true;
                markDirty();
                teleMirrorCheck();
            }
            ui_.text(kind.label, ix + 30, y + 1, 14, on ? t.text : t.muted, sel ? Weight::Semibold : Weight::Regular);
            if (on && ui_.clicked(R(ix + 28, y - 3, iw - 28, 26)))
                es.sel = tele::find(items, kind.k);
            y += 26;
        }
        // The picked box's options.
        y += 8;
        ui_.fill(R(ix, y, iw, 1), t.border);
        y += 12;
        if (es.sel >= 0 && es.sel < static_cast<int>(items.size()))
        {
            tele::Item &it = items[static_cast<size_t>(es.sel)];
            const int ki = tele::kindIndex(it.k);
            fullui::caps(ui_, ix, y, strf("%s OPTIONS", tele::kKinds[ki].label), t.faint);
            y += 22;
            bool changed = false;
            auto seg = [&](const char *id, const char *label, const std::vector<std::string> &opts, int &v) {
                ui_.text(label, ix, y + 6, 13, t.faint);
                if (ui_.segmented(id, R(ix + 78, y, iw - 78, 28), opts, v, !busy(), 12))
                    changed = true;
                y += 34;
            };
            const int ns = tele::styleCount(ki);
            if (it.k == "map")
            {
                int d = it.detail == "simple" ? 1 : 0;
                seg("tele-o-detail", "Detail", {"Detailed", "Simple"}, d);
                it.detail = d ? "simple" : "detailed";
                seg("tele-o-style", "View", {"Locked", "Rotating"}, it.style);
                bool nm = it.names;
                if (ui_.checkbox("tele-o-names", ix, y, nm, "Corner names", !busy()))
                    it.names = nm, changed = true;
                y += 30;
            }
            else if (ns > 0)
            {
                std::vector<std::string> o;
                for (int s = 0; s < ns; ++s)
                    o.push_back(tele::kKinds[ki].styles[s]);
                seg("tele-o-style", "Style", o, it.style);
            }
            if (it.k == "tower")
            {
                int n = it.name == "short" ? 1 : it.name == "driver" ? 2 : 0;
                seg("tele-o-name", "Names", {"Car", "Short", "Driver"}, n);
                it.name = tele::kNames[n];
                const char *colNames[4] = {"Gap to leader", "Gap to car ahead", "Best lap", "Last lap"};
                for (int k = 0; k < 4; ++k)
                {
                    bool c = (it.cols >> k) & 1;
                    if (ui_.checkbox(strf("tele-o-col%d", k), ix + (k % 2) * (iw * 0.5f), y, c, colNames[k], !busy()))
                    {
                        it.cols = c ? (it.cols | (1 << k)) : (it.cols & ~(1 << k));
                        changed = true;
                    }
                    if (k % 2)
                        y += 28;
                }
            }
            if (it.k == "speed")
            {
                int u = it.units == "kmh" ? 1 : it.units == "mph" ? 2 : 0;
                seg("tele-o-units", "Units", {"GT4's", "km/h", "mph"}, u);
                it.units = u == 1 ? "kmh" : u == 2 ? "mph" : "";
            }
            if (it.k == "radar")
                y += ui_.wrap("Colours and range from the Race HUD's radar. Tap it on the screen to zoom.", ix, y, iw, 12, t.faint) + 6;
            if (it.k == "mirror")
                y += ui_.wrap("GT4's rear view in every camera view. GT4's own mirror stays on the game screen in the bumper view unless you turn it off.", ix, y, iw, 12,
                              t.faint) +
                     6;
            if (it.k == "splits")
                y += ui_.wrap("Purple: the race's fastest. Green: your best. Yellow: slower.", ix, y, iw, 12, t.faint) + 6;
            if (changed)
            {
                T.dirty = true;
                markDirty();
            }
            if (ui_.button("tele-front", R(ix, y, iw, 30), "Bring picked box to front", Style::Secondary, !busy() && es.sel < static_cast<int>(items.size()) - 1))
            {
                tele::Item moved = it;
                items.erase(items.begin() + es.sel);
                items.push_back(moved);
                es.sel = static_cast<int>(items.size()) - 1;
                T.dirty = true;
                markDirty();
            }
            y += 38;
        }
        else
        {
            y += ui_.wrap("Click a box, or a ticked item, for its options.", ix, y, iw, 12.5f, t.faint) + 10;
        }
        ui_.fill(R(ix, y, iw, 1), t.border);
        y += 12;
        ui_.text("Snap to grid (2.5%)", ix, y + 3, 13.5f, t.text);
        ui_.toggle("tele-snap", ix + iw - 44, y, T.snap, true);
        y += 32;
        {
            int bg = T.layout.black ? 1 : 0;
            ui_.text("Background", ix, y + 6, 13, t.faint);
            if (ui_.segmented("tele-bg", R(ix + 90, y, iw - 90, 28), {"Dark grey", "Black (OLED)"}, bg, !busy(), 12))
            {
                T.layout.black = bg == 1;
                f.teleBlack = T.layout.black;
                T.dirty = true;
                markDirty();
            }
            y += 36;
        }
        if (ui_.button("tele-reset", R(ix, y, iw * 0.5f - 4, 30), "Reset this layout", Style::Secondary, !busy()))
        {
            items = tele::defaults(T.target);
            es = boxedit::State{};
            T.dirty = true;
            markDirty();
            teleMirrorCheck();
        }
        if (ui_.button("tele-clear", R(ix + iw * 0.5f + 4, y, iw * 0.5f - 4, 30), "Clear this layout", Style::Secondary, !busy() && !items.empty()))
        {
            items.clear();
            es = boxedit::State{};
            T.dirty = true;
            markDirty();
            teleMirrorCheck();
        }
        y += 40;
        ui_.endScroll("tele-col", y + off - col.y + 6);
    }

    // One box on the stage: a tile in the page's look with a sketch of what it shows.
    void App::teleDrawItem(const tele::Item &it, Rectangle r, bool sel)
    {
        const ui::Theme &t = ui::theme();
        const Color panel{16, 20, 26, 255}, line{35, 42, 51, 255}, mute{141, 151, 166, 255}, me{242, 179, 61, 255}, fg{238, 241, 245, 255};
        const bool bare = it.k == "map", clear = it.k == "radar";
        if (!clear)
            ui_.round(r, 4, bare ? Color{7, 9, 12, 255} : panel);
        ui_.roundLine(r, 4, sel ? 2.0f : 1.0f, sel ? t.accent : line);
        const int ki = tele::kindIndex(it.k);
        const float m = std::min(r.width, r.height);
        const float cx = r.x + r.width * 0.5f, cy = r.y + r.height * 0.5f;
        auto big = [&](const std::string &s, float frac, Color c) {
            const float size = std::clamp(std::min(r.height * frac, r.width / std::max(1.0f, static_cast<float>(s.size())) * 1.1f), 8.0f, 90.0f);
            ui_.text(s, cx, cy - size * 0.62f, size, c, Weight::Bold, Align::Center);
        };
        if (it.k == "tower" || it.k == "laptimes")
        {
            const int rows = std::clamp(static_cast<int>(r.height / 16), 2, 9);
            const float rh = r.height / (rows + 1);
            for (int i = 0; i < rows; ++i)
            {
                const float yy = r.y + rh * (i + 1.2f);
                if (it.k == "tower")
                    ui_.circle(r.x + rh * 0.6f, yy + rh * 0.35f, rh * 0.3f, i == 2 ? me : Color{79, 139, 214, 255});
                ui_.fill(R(r.x + rh * 1.2f, yy + rh * 0.25f, r.width * 0.35f, std::max(1.5f, rh * 0.18f)), i == 2 ? me : ui::alpha(fg, 0.55f));
                ui_.fill(R(r.x + r.width * 0.62f, yy + rh * 0.25f, r.width * 0.3f, std::max(1.5f, rh * 0.18f)), i == 0 ? Color{182, 124, 240, 255} : ui::alpha(fg, 0.35f));
            }
        }
        else if (it.k == "map")
        {
            // a course outline (Autumn Ring Mini's kind of shape)
            const float a = r.width * 0.36f, b = r.height * 0.34f;
            Vector2 prev{};
            for (int i = 0; i <= 48; ++i)
            {
                const float th = i / 48.0f * 6.2832f;
                const float rr = 1.0f + 0.18f * std::sin(th * 3) + 0.08f * std::cos(th * 2);
                const Vector2 p{cx + std::cos(th) * a * rr, cy + std::sin(th) * b * rr};
                if (i)
                    ui_.line(prev.x, prev.y, p.x, p.y, std::max(2.0f, m * 0.025f), Color{201, 209, 219, 255});
                prev = p;
            }
            ui_.circle(cx + a * 1.05f, cy, std::max(3.0f, m * 0.035f), me);
            ui_.circle(cx - a * 0.6f, cy - b * 0.9f, std::max(3.0f, m * 0.03f), Color{79, 139, 214, 255});
        }
        else if (it.k == "mirror")
        {
            ui_.gradientV(R(r.x + 3, r.y + 3, r.width - 6, r.height - 6), Color{70, 96, 130, 255}, Color{40, 44, 48, 255});
            ui_.fill(R(r.x + 3, cy, r.width - 6, r.height * 0.5f - 3), Color{52, 55, 60, 255});
            ui_.triangle(Vector2{cx - r.width * 0.06f, cy}, Vector2{cx + r.width * 0.06f, cy}, Vector2{cx, cy - r.height * 0.05f}, Color{90, 92, 96, 255});
        }
        else if (it.k == "tacho" && it.style == 0)
        {
            const float rad = m * 0.42f;
            for (int i = 0; i <= 16; ++i)
            {
                const float a = 2.356f + i / 16.0f * 4.712f;
                ui_.line(cx + std::cos(a) * rad * 0.8f, cy + std::sin(a) * rad * 0.8f, cx + std::cos(a) * rad, cy + std::sin(a) * rad, 1.5f,
                         i > 13 ? Color{229, 72, 77, 255} : ui::alpha(fg, 0.7f));
            }
            const float a = 2.356f + 0.62f * 4.712f;
            ui_.line(cx, cy, cx + std::cos(a) * rad * 0.85f, cy + std::sin(a) * rad * 0.85f, 2.5f, me);
        }
        else if (it.k == "tacho")
        {
            for (int i = 0; i < 10; ++i)
                ui_.circle(r.x + r.width * (i + 0.5f) / 10.0f, cy, std::min(r.width / 24.0f, r.height * 0.3f),
                           i < 6 ? (i < 4 ? Color{62, 207, 142, 255} : Color{247, 214, 74, 255}) : ui::alpha(fg, 0.12f));
        }
        else if (it.k == "radar")
        {
            const float rad = m * 0.46f;
            if (it.style == 1)
                ui_.round(R(cx - rad, cy - rad, rad * 2, rad * 2), 3, Color{16, 20, 26, 235});
            else
                ui_.circle(cx, cy, rad, ui::alpha(WHITE, 0.05f));
            ui_.ring(cx, cy, rad * 0.32f, rad * 0.32f + 1, ui::alpha(WHITE, 0.2f));
            ui_.ring(cx, cy, rad * 0.65f, rad * 0.65f + 1, ui::alpha(WHITE, 0.2f));
            ui_.fill(R(cx - rad * 0.06f, cy - rad * 0.14f, rad * 0.12f, rad * 0.28f), me);
            ui_.fill(R(cx + rad * 0.25f, cy - rad * 0.5f, rad * 0.12f, rad * 0.28f), fg);
        }
        else if (it.k == "pedals")
        {
            ui_.fill(R(r.x + r.width * 0.12f, r.y + r.height * 0.06f, r.width * 0.32f, r.height * 0.88f), ui::alpha(WHITE, 0.1f));
            ui_.fill(R(r.x + r.width * 0.56f, r.y + r.height * 0.06f, r.width * 0.32f, r.height * 0.88f), ui::alpha(WHITE, 0.1f));
            ui_.fill(R(r.x + r.width * 0.56f, r.y + r.height * 0.3f, r.width * 0.32f, r.height * 0.64f), Color{62, 207, 142, 255});
        }
        else if (it.k == "gmeter")
        {
            const float rad = m * 0.45f;
            for (float fr : {0.33f, 0.66f, 1.0f})
                ui_.ring(cx, cy, rad * fr - 1, rad * fr, ui::alpha(WHITE, 0.15f));
            ui_.circle(cx + rad * 0.3f, cy - rad * 0.2f, std::max(2.0f, rad * 0.08f), Color{88, 196, 220, 255});
        }
        else if (it.k == "splits")
        {
            for (int k = 0; k < 3; ++k)
            {
                const Rectangle s = R(r.x + 4 + k * (r.width - 8) / 3.0f, r.y + r.height * 0.18f, (r.width - 8) / 3.0f - 4, r.height * 0.64f);
                ui_.fill(s, ui::alpha(WHITE, 0.07f));
                ui_.fill(R(s.x, s.y, std::max(2.0f, s.width * 0.05f), s.height), k == 0 ? Color{182, 124, 240, 255} : k == 1 ? Color{62, 207, 142, 255} : Color{247, 214, 74, 255});
            }
        }
        else if (it.k == "speed")
            big(it.units == "mph" ? "98" : "158", 0.5f, fg);
        else if (it.k == "gear")
            big("3", 0.62f, me);
        else if (it.k == "position")
            big("3/6", 0.5f, me);
        else if (it.k == "lapbox")
            big(it.style == 1 ? "2/3  1'24.6" : "LAP 2/3", 0.4f, fg);
        else if (it.k == "delta")
            big("-0.284", 0.4f, Color{62, 207, 142, 255});
        // The label, top left.
        if (ki >= 0 && r.height > 16 && r.width > 30)
        {
            const std::string label = ui_.fit(tele::kKinds[ki].label, r.width - 8, 11, Weight::Semibold);
            const float lw = ui_.textWidth(label, 11, Weight::Semibold) + 8;
            ui_.round(R(r.x + 2, r.y + 2, lw, 16), 3, ui::alpha(BLACK, 0.55f));
            ui_.text(label, r.x + 6, r.y + 3, 11, sel ? fg : mute, Weight::Semibold);
        }
    }

    // The hidden test pictures' scenes (gt4full_shots.cpp): 0 the monitor list, 1 the phone with its code, 2..4 the three
    // layouts in the editor (a box picked on each).
    void App::teleShot(int scene)
    {
        TeleState &T = teleState();
        T.layout = tele::defaultLayout();
        T.dirty = false;
        for (boxedit::State &s : T.edit)
            s = boxedit::State{};
        T.note.clear();
        T.target = tele::Monitor;
        T.tab = scene == 0 ? 0 : scene == 1 ? 1 : 2;
        if (scene >= 2 && scene <= 4)
            T.target = scene - 2;
        if (scene == 2)
            T.edit[tele::Monitor].sel = 0;
        if (scene == 3)
            T.edit[tele::Upright].sel = 3;
        if (scene == 4)
            T.edit[tele::Sideways].sel = 2;
        // (as a player's click leaves it: the mirror in a layout in use takes GT4's own mirror off the game screen)
        teleMirrorCheck();
        dirty_ = false;
    }

    const std::vector<Rectangle> &App::teleBoxes() const { return teleBoxDp_; }
} // namespace gt4stock
