// GT4 Recompilation launcher, Full edition: its hidden test modes (FULL-LAUNCHER-PLAN.md 1.7, F6). --test-shots renders
// every page, Display tab, dialog and state into PNG files without a visible window (the window is created hidden and
// checked after every picture); --dry-run reports what Play would pass, and what is stored but never passed.

#include "gt4stock_app.h"
#include "gt4full_racelab.h"
#include "gt4full_racetiming.h"

#include "gt4save.h"
#include "gt4stock_util.h"
#include "shared/gt4_full_keys.h"

#include <algorithm>
#include <cmath>
#include <functional>
#include <sstream>

extern "C" int ps2x_skip_hidden_swap;

namespace gt4stock
{
    extern Rectangle g_raceHudBoxDp[12]; // (gt4full_racehud.cpp: our boxes as last drawn)
    extern Rectangle g_raceHudGt4Dp[11]; // (gt4full_racehud.cpp: GT4's parts as last drawn)

    void App::reportFullDryRun(const sys::Launch &l)
    {
        report("Full edition: GT4 program reads (its strings, read only): " + full_.text() + (opt_.releaseLook ? "; release look" : ""));
        // Every key Play passes must be on the Full list; no GT4L_ line ever reaches GT4.exe.
        int unknown = 0, launcherKeys = 0;
        bool renderer = false;
        for (const auto &[k, v] : l.env)
        {
            if (startsWith(k, "GT4L_"))
                ++launcherKeys;
            else if (!gt4fullkeys::fullKey(k))
            {
                ++unknown;
                report("  not on the Full list (kept from the file as written): " + k);
            }
            if (k == "GT3_RENDERER" && (v == "auto" || v == "vk" || v == "gl"))
                renderer = true;
        }
        report(strf("check: launcher (GT4L_) keys passed to GT4.exe: %d (%s)", launcherKeys, launcherKeys == 0 ? "ok" : "FAIL"));
        report(strf("check: GT3_RENDERER=auto|vk|gl passed: %s", renderer ? "yes (ok)" : "NO (FAIL)"));
        report(strf("check: keys passed that are not on the Full list: %d", unknown));
        // GT4's own Multi Monitor Mode: the side screens' copies Play would start with this one (their own keys).
        if (mmOwnChosen())
        {
            const std::vector<sys::Launch> sides = mmSideLaunches(l);
            report(strf("GT4 Multi Monitor Mode: chosen; %s; %zu side copy(ies) with this monitor layout", mmOwnLive() ? "live" : "not live here",
                        sides.size()));
            for (const sys::Launch &s : sides)
            {
                std::string keys;
                for (const auto &[k, v] : s.env)
                    if (startsWith(k, "GT4_MM_") || startsWith(k, "GT4_NET") || k == "GT4_MONITOR" || k == "GT3_SOUND_OUTPUT" || k == "PS2X_WHEEL")
                        keys += " " + k + "=" + v;
                report("  side copy:" + keys);
            }
        }
        // The stored "Not built yet" choices, as the file holds them.
        std::istringstream in(renderSettings(st_, kVersion));
        std::string line;
        std::vector<std::string> nb;
        while (std::getline(in, line))
        {
            if (!line.empty() && line.back() == '\r')
                line.pop_back();
            if (gt4fullkeys::notBuiltKey(line))
                nb.push_back(line);
        }
        report(strf("stored, never passed (GT4L_NB_): %zu line(s)", nb.size()));
        for (const std::string &s : nb)
            report("  " + s);
        std::string fileFilter, envFilter;
        for (const auto &[k, v] : settingsEnvironment(st_, true))
            if (k == "GT4_PICTURE_FILTER")
                fileFilter = v;
        for (const auto &[k, v] : l.env)
            if (k == "GT4_PICTURE_FILTER")
                envFilter = v;
        std::string fileBass = "<absent>", envBass = "<absent>";
        for (const auto &[k, v] : settingsEnvironment(st_, true))
            if (k == "GT4_ENGINE_BASS")
                fileBass = v;
        for (const auto &[k, v] : l.env)
            if (k == "GT4_ENGINE_BASS")
                envBass = v;
        report("engine bass: the file says " + fileBass + ", Play passes " + envBass + (full_.read && !full_.engineBass ? " (this GT4 program does not read it)" : ""));
        for (const char *key : gt4fullkeys::kSoundPacks)
        {
            std::string fileV = "<absent>", envV = "<absent>";
            for (const auto &[k, v] : settingsEnvironment(st_, true))
                if (k == key)
                    fileV = v;
            for (const auto &[k, v] : l.env)
                if (k == key)
                    envV = v;
            report(std::string("sound packs: ") + key + ": the file says " + fileV + ", Play passes " + envV +
                   (full_.read && !full_.soundPacks ? " (this GT4 program does not read it)" : ""));
        }
        report("picture filter: the file says " + fileFilter + ", Play passes " + envFilter +
               (fileFilter != envFilter ? " (this GT4 program does not read " + fileFilter + ")" : std::string()));
        // Display > Resolution: what the file holds and what Play passes (native to a GT4 program without the marker).
        std::string fileScale, fileFast, envScale = "(none)", envFast = "(none)";
        for (const auto &[k, v] : settingsEnvironment(st_, true))
        {
            if (k == "GT3_GS_GL_SCALE")
                fileScale = v;
            else if (k == "GT3_GS_GL_FAST")
                fileFast = v;
        }
        for (const auto &[k, v] : l.env)
        {
            if (k == "GT3_GS_GL_SCALE")
                envScale = v;
            else if (k == "GT3_GS_GL_FAST")
                envFast = v;
        }
        report("internal resolution: the file says GT3_GS_GL_SCALE=" + fileScale + " GT3_GS_GL_FAST=" + fileFast + " (drawing " +
               (st_.full.drawing == 1 ? "exact" : st_.full.drawing == 2 ? "fast" : "auto") + "), Play passes GT3_GS_GL_SCALE=" + envScale +
               " GT3_GS_GL_FAST=" + envFast + (full_.read && !full_.internalRes ? " (this GT4 program's internal resolution was not checked with GT4: native)" : ""));
    }

    void App::reportGarage(const std::string &scene)
    {
        const Garage &g = garage_;
        report(strf("%s: garage %s: %zu cars (%d full records), oil due %d, need a wash %d, Body Rigidity %d; player '%s', Day %lld, %s Cr.; car data %s",
                    scene.c_str(), garageTitle_.c_str(), g.cars.size(), g.fullRecords, g.oilDue, g.needsWash, g.withRigidity, g.player.c_str(),
                    static_cast<long long>(g.gameDay), groupedNumber(g.credits).c_str(),
                    cat_ ? strf("%zu cars from the disc in %.0f ms", cat_->carCount(), catMs_).c_str() : ("none (" + catError_ + ")").c_str()));
        if (const GarageCar *c = g.bySlot(garageSel_))
            report(strf("  shown: slot %d %s (%s) %.2f PS %d kg, %s; oil age %.1f km%s, dirt %.0f%%, body wear %.1f km, rigidity %s; disc: %s %d PS %.1f kgfm %d kg %s %s/%s; "
                        "estimates 0-100 %.1f s, 0-200 %.1f s, 100-0 %.0f m, top %.0f km/h",
                        c->slot, c->name.c_str(), c->kind == 2 ? "riding" : c->kind == 3 ? "stored" : "stock", c->powerPs, c->weightKg, c->acquired.c_str(), c->oilAgeKm,
                        c->oilDue ? " (due)" : "", c->dirt01 * 100.0, c->bodyWearKm, c->rigidity ? "fitted" : "no", c->disc ? c->disc->label.c_str() : "-",
                        c->disc ? c->disc->powerPs : 0, c->disc ? c->disc->torqueX100 / 100.0 : 0.0, c->disc ? c->disc->weightKg : 0,
                        c->disc ? drivetrainName(c->disc->drivetrain) : "-", c->disc ? tyreText(c->disc->front).c_str() : "-",
                        c->disc ? tyreText(c->disc->rear).c_str() : "-", c->est.zeroTo100, c->est.zeroTo200, c->est.brake100, c->est.topSpeedKmh));
    }

    int App::runFullTestShots()
    {
        // The names and the car data from the disc first (worker threads started at launch), then the save copy
        // (--save-copy, read only) for the Garage and Game progress.
        pollNames(true);
        pollCatalogue(true);
        report(names_ ? strf("names from the disc: %zu cars, read in %.0f ms", names_->carNameCount(), namesMs_) : "names from the disc: none (" + namesError_ + ")");
        size_t prizeOnly = 0;
        if (cat_)
            for (const PrizeCar &p : cat_->prizes())
                prizeOnly += p.prizeOnly();
        report(cat_ ? strf("car data from the disc: %zu cars; GT4's prize table: %zu rows, %zu cars, %zu of them prizes only; read in %.0f ms", cat_->carCount(),
                           cat_->prizeRowCount(), cat_->prizes().size(), prizeOnly, catMs_)
                    : "car data from the disc: none (" + catError_ + ")");
        if (cat_)
        {
            // GT Mode prizes: every group, its entries and prizes, and the names the launcher had to make from GT4's keys.
            const std::vector<GuideGroup> guide = buildPrizeGuide(*cat_, names_.get(), nullptr);
            size_t rows = 0, carsN = 0, courses = 0, papers = 0, made = 0;
            std::string madeList;
            for (const GuideGroup &g : guide)
            {
                size_t gp = 0;
                for (const GuideEntry &e : g.entries)
                {
                    gp += e.prizes.size();
                    for (const GuidePrize &p : e.prizes)
                        (p.kind == "car" ? carsN : p.kind == "course" ? courses : papers)++;
                    if (!e.gt4Name && e.key.rfind("win:", 0) == 0)
                    {
                        ++made;
                        madeList += " " + e.key.substr(4);
                    }
                }
                rows += gp;
                report(strf("  GT Mode prizes group %s \"%s\"%s: %zu entries, %zu prizes", g.key.c_str(), g.title.c_str(), g.gt4Title ? "" : " (launcher's title)",
                            g.entries.size(), gp));
            }
            report(strf("GT Mode prizes: %zu prizes listed of the table's %zu rows (%zu cars, %zu courses, %zu papers); event names made from keys: %zu%s", rows,
                        cat_->presentRows().size(), carsN, courses, papers, made, madeList.c_str()));
        }
        std::error_code ec;
        const fs::path saveCopy = opt_.saveCopies.empty() ? fs::path() : fs::absolute(opt_.saveCopies.front(), ec);
        garageCopy_ = saveCopy;
        loadGarage(true);
        fs::create_directories(opt_.shots, ec);
        const Settings base = st_;
        const FullFeatures baseFeatures = full_;
        const bool baseExeFound = inst_.exeFound;
        const auto baseDisc = disc_;
        const bool baseUnbuilt = showUnbuilt_;
        const std::vector<MonitorInfo> baseMonitors = monitors_;
        // (the GT3 look's scenes: every box on as look-runs.ps1 gives GT4.exe; styles = its boxesB)
        auto hudAllBoxesShot = [&](bool styles) {
            FullSettings &f = st_.full;
            auto at = [&](int w, float x, float y, int size, int style) -> FullHudWidget & {
                FullHudWidget &h = f.liveWidgets[static_cast<size_t>(w)];
                h.on = true, h.xy = true, h.x = x, h.y = y, h.size = size, h.seeThrough = 20, h.style = style;
                return h;
            };
            at(0, 90, 60, 90, styles ? 1 : 0);
            at(1, 50, 30, 100, styles ? 1 : 0);
            at(2, 97, 88, 100, 0);
            at(3, 7, 88, 100, styles ? 1 : 0);
            FullHudWidget &tw = at(kFullHudTower, styles ? 10.0f : 12.0f, 44, styles ? 100 : 90, 0);
            tw.towerName = styles ? 1 : 0, tw.towerCols = styles ? (1 | 8) : (1 | 2 | 4), tw.towerRows = styles ? 6 : 8;
            at(kFullHudMap, 88, 24, styles ? 100 : 90, styles ? 1 : 0);
            at(kFullHudSplits, 50, 19, 100, 0);
            at(kFullHudRevs, 50, 8, 100, styles ? 1 : 0);
            at(kFullHudSpeed, 76, 90, 100, 0);
            at(kFullHudGear, 68, 90, 100, 0);
            at(kFullHudLap, 26, 7, 100, styles ? 0 : 1);
            f.radarShow = 0, f.radarRange = styles ? 40 : 25, f.radarLabels = true;
            f.startLights = true, f.startCount = 5, f.startLook = styles ? 1 : 0;
            f.lightsX = 50.0f, f.lightsY = 45.0f, f.lightsSize = 100;
            hudSel_.clear();
        };
        // the developer's main PC (MULTI-MONITOR-NOTES.md): three MSI G321Q side by side, the middle one the main display, a 27-inch
        // monitor above the middle one (GT4's own Multi Monitor Mode pictures).
        auto demoRig = [&] {
            MonitorInfo c;
            c.index = 0;
            c.name = "MSI G321Q";
            c.w = c.workW = 2560;
            c.h = 1440;
            c.workH = 1392;
            c.hz = 170;
            c.primary = true;
            c.demo = true;
            MonitorInfo l = c, r = c, t = c;
            l.index = 1;
            l.x = -2560;
            l.primary = false;
            r.index = 2;
            r.x = 2560;
            r.primary = false;
            t.index = 3;
            t.name = "27-inch monitor";
            t.y = -1440;
            t.hz = 144;
            t.primary = false;
            monitors_ = {c, l, r, t};
        };
        struct Scene
        {
            const char *name;
            Page page;
            std::function<void()> setup;
        };
        auto car = [&](auto pred) {
            for (const GarageCar &c : garage_.cars)
                if (pred(c))
                    return c.slot;
            return garage_.cars.empty() ? -1 : garage_.cars.front().slot;
        };
        // The Garage's changes (build part 2): the confirmations as the player sees them, made from the save on memory card
        // slot 1 of the test GT4 folder (read only: a test picture never confirms, so nothing is written).
        auto confirm = [&](auto pred, GarageAction a) {
            garageTab_ = 0;
            garageSel_ = car(pred);
            if (const GarageCar *c = garage_.bySlot(garageSel_))
                openGarageEdit(serviceEdit(*c, a));
        };
        // colour: an index of the car's GT4 colour list chosen on the page (-1: the car's default).
        auto dealer = [&](int section, const std::string &label, bool pay, bool open, int colour = -1) {
            garageTab_ = 2;
            dealerSection_ = section;
            dealerSel_ = label;
            st_.full.dealerPay = pay;
            dealerColourFor_.clear();
            if (colour >= 0)
            {
                dealerColourFor_ = label;
                dealerColour_ = colour;
            }
            if (!open || !cat_)
                return;
            if (section == 0)
            {
                for (const HiddenCar &h : kHiddenCars)
                    if (label == h.label)
                        openGarageEdit(colour >= 0 ? withColour(hiddenCarEdit(*cat_, h, pay), colour, &h) : hiddenCarEdit(*cat_, h, pay));
            }
            else if (const DiscCar *d = cat_->carByLabel(label))
                openGarageEdit(colour >= 0 ? withColour(prizeCarEdit(*d, pay), colour, nullptr) : prizeCarEdit(*d, pay));
        };
        // Prize Cars with a maker chip chosen ("All" the first; the chips follow the prize cars' order) and a car selected.
        auto prizeMakerScene = [&](const std::string &maker, const std::string &label) {
            dealer(1, label, false, false);
            std::vector<std::string> makers{"All"};
            if (cat_)
                for (const PrizeCar &pc : cat_->prizes())
                    if (const DiscCar *d = cat_->car(pc.code); d && std::find(makers.begin(), makers.end(), d->maker) == makers.end())
                        makers.push_back(d->maker);
            const auto it = std::find(makers.begin(), makers.end(), maker);
            dealerMaker_ = it == makers.end() ? 0 : static_cast<int>(it - makers.begin());
        };
        // Save details of the n-th --save-copy (the first when there is no n-th).
        auto details = [&](size_t n) {
            const fs::path p = opt_.saveCopies.size() > n ? fs::absolute(opt_.saveCopies[n], ec) : saveCopy;
            openDetailsPath(p, std::string("Test copy") + (n ? " (clean save)" : " (the old memory-card bug's extra data)"));
        };
        std::string prizeLabel; // the first prize-only car GT4's data gives a price under the test save's credits
        if (cat_)
            for (const PrizeCar &pc : cat_->prizes())
                if (const DiscCar *d = cat_->car(pc.code); d && pc.prizeOnly() && d->price > 0 && d->price < garage_.credits && prizeLabel.empty())
                    prizeLabel = d->label;
        std::string colourLabel; // the first prize car with at least 3 GT4 colours, priced under the test save's credits
        if (cat_ && names_)
            for (const PrizeCar &pc : cat_->prizes())
                if (const DiscCar *d = cat_->car(pc.code); d && colourLabel.empty() && d->price > 0 && (d->price < garage_.credits || !garage_.loaded) && names_->colourCount(d->code) >= 3)
                    colourLabel = d->label;
        const std::vector<Scene> scenes = {
            {"F01-play", Page::Play, [] {}},
            {"F01b-play-running", Page::Play, [&] { gameRunning_ = true; }},
            // A release before its first build (no GT4.exe, a build-kit): Build GT4 in place of Play; the build at step 4 of 7
            // for 2 min 10 s; stopped with gt4setup's error; and with no disc chosen yet.
            {"F01c-play-build", Page::Play, [&] { buildShot_ = true, inst_.exeFound = false; }},
            {"F01d-play-building", Page::Play, [&] {
                 buildShot_ = true, inst_.exeFound = false;
                 build_.running = true, build_.step = 4, build_.of = 7, build_.what = "recompiling GT4's game code";
                 build_.startMs = sys::tickMs() - 130000u, build_.stepMs = sys::tickMs() - 25000u;
             }},
            {"F01e-play-build-stopped", Page::Play, [&] {
                 buildShot_ = true, inst_.exeFound = false;
                 build_.failed = true;
                 build_.error = "The GT4 disc image not found: D:\\Games\\GT4.iso.";
             }},
            {"F01f-play-build-no-disc", Page::Play, [&] {
                 buildShot_ = true, inst_.exeFound = false;
                 disc_.exists = false;
             }},
            {"F02-display-picture", Page::Display, [&] { st_.full.displayTab = 0; }},
            {"F02b-display-picture-other-modes", Page::Display, [&] {
                 st_.full.displayTab = 0;
                 st_.full.otherModesOpen = true;
                 ui_.scrollTo("pic-left", 1e6f);
                 ui_.openPopup("deinterlace");
             }},
            {"F02h-display-picture-colour-gt3", Page::Display, [&] {
                 st_.full.displayTab = 0;
                 st_.full.lut = 1;
                 ui_.scrollTo("pic-left", 560.0f);
             }},
            {"F02i-display-picture-colour-cube-60", Page::Display, [&] {
                 st_.full.displayTab = 0;
                 st_.full.lut = 2;
                 st_.full.lutFile = "C:\\LUTs\\film look.cube";
                 st_.full.lutStrength = 60;
                 ui_.scrollTo("pic-left", 560.0f);
             }},
            {"F02c-display-picture-lanczos-16bit-lines-off", Page::Display, [&] {
                 st_.full.displayTab = 0;
                 st_.full.scaling = FullScaling::Lanczos;
                 st_.full.colour = 16;
                 st_.linesOn = false;
                 st_.preview = "menu";
             }},
            {"F02d-display-preview-full-screen", Page::Display, [&] {
                 st_.full.displayTab = 0;
                 previewFullscreen_ = true;
             }},
            {"F02e-display-preview-zoom-8x", Page::Display, [&] {
                 st_.full.displayTab = 0;
                 st_.full.previewZoom = 8;
                 st_.full.scaling = FullScaling::Bicubic;
                 zoomFocus_ = Vector2{0.62f, 0.55f};
             }},
            {"F02f-display-preview-race-16bit-zoom-6x", Page::Display, [&] {
                 // 16-bit race colour: the race picture with GT4's dithering, zoomed on the sky (review finding 2).
                 st_.full.displayTab = 0;
                 st_.full.colour = 16;
                 st_.full.previewZoom = 6;
                 st_.preview = "race";
                 zoomFocus_ = Vector2{0.16f, 0.50f}; // the grass's smooth shades, where the dither pattern shows
             }},
            {"F02g-display-preview-4x3", Page::Display, [&] {
                 st_.full.displayTab = 0;
                 st_.full.aspect = FullAspect::Normal;
                 st_.full.scaling = FullScaling::Lanczos;
             }},
            {"F03-display-resolution", Page::Display, [&] { st_.full.displayTab = 1; }},
            // The presets (developer, 2026-10-19): 1080p picked; Automatic with the ultrawide shape.
            {"F03p-display-resolution-preset-1080p", Page::Display, [&] {
                 st_.full.displayTab = 1;
                 st_.full.resPreset = 3;
             }},
            {"F03q-display-resolution-auto-ultrawide", Page::Display, [&] {
                 st_.full.displayTab = 1;
                 st_.full.resPreset = 1;
                 st_.full.aspect = FullAspect::Ultra21;
             }},
            // Internal resolution (full-m2, Resolution): 2x and 3x following the resolution (exact / fast), 3x exact (its speed
            // warning), 4x chosen while it is not offered yet (greyed with its reason; GT4 gets native), a release build at 3x,
            // and a GT4 program without the check (greyed in a test build; Play gives it native).
            {"F03b-display-resolution-2x", Page::Display, [&] {
                 st_.full.displayTab = 1;
                 st_.full.internalRes = 2; st_.full.resPreset = 0;
                 st_.full.drawing = 0;
             }},
            {"F03c-display-resolution-4x-not-yet", Page::Display, [&] {
                 st_.full.displayTab = 1;
                 st_.full.internalRes = 4; st_.full.resPreset = 0;
                 st_.full.drawing = 0;
             }},
            // Effects resolution (analysis/design/effects-scale): 2x inside a 4x picture.
            {"F03h-display-resolution-4x-effects-2x", Page::Display, [&] {
                 st_.full.displayTab = 1;
                 st_.full.internalRes = 4; st_.full.resPreset = 0;
                 st_.full.drawing = 0;
                 st_.full.effectsScale = 2;
             }},
            {"F03g-display-resolution-3x", Page::Display, [&] {
                 st_.full.displayTab = 1;
                 st_.full.internalRes = 3; st_.full.resPreset = 0;
                 st_.full.drawing = 0;
             }},
            {"F03d-display-resolution-3x-exact", Page::Display, [&] {
                 st_.full.displayTab = 1;
                 st_.full.internalRes = 3; st_.full.resPreset = 0;
                 st_.full.drawing = 1;
             }},
            {"F03e-release-look-resolution-3x", Page::Display, [&] {
                 showUnbuilt_ = false;
                 st_.full.displayTab = 1;
                 st_.full.internalRes = 3; st_.full.resPreset = 0;
                 st_.full.drawing = 0;
             }},
            {"F03f-display-resolution-gt4-without-the-check", Page::Display, [&] {
                 full_.internalRes = false;
                 st_.full.displayTab = 1;
                 st_.full.internalRes = 2; st_.full.resPreset = 0;
             }},
            {"F04-display-screen-shape", Page::Display, [&] { st_.full.displayTab = 2; }},
            {"F04b-display-screen-shape-window-4x3", Page::Display, [&] {
                 st_.full.displayTab = 2;
                 st_.fullscreen = false;
                 st_.full.aspect = FullAspect::Normal;
             }},
            {"F04c-display-screen-shape-across-monitors", Page::Display, [&] {
                 st_.full.displayTab = 2;
                 st_.fullscreen = true;
                 st_.full.span = true;
             }},
            // The wider shapes (full-m2, Wide): 21:9, 32:9, 48:9 on one screen, a release build at 32:9, and a GT4 program
            // without them (greyed in a test build; Play gives it 16:9).
            {"F04d-display-screen-shape-21x9", Page::Display, [&] {
                 st_.full.displayTab = 2;
                 st_.full.aspect = FullAspect::Ultra21;
             }},
            {"F04e-display-screen-shape-32x9", Page::Display, [&] {
                 st_.full.displayTab = 2;
                 st_.full.aspect = FullAspect::Ultra32;
             }},
            {"F04f-display-screen-shape-48x9", Page::Display, [&] {
                 st_.full.displayTab = 2;
                 st_.full.aspect = FullAspect::Triple48;
             }},
            {"F04g-release-look-screen-shape-32x9", Page::Display, [&] {
                 showUnbuilt_ = false;
                 st_.full.displayTab = 2;
                 st_.full.aspect = FullAspect::Ultra32;
             }},
            {"F04h-display-screen-shape-gt4-without-wider-shapes", Page::Display, [&] {
                 full_.wideShapes = false;
                 st_.full.displayTab = 2;
                 st_.full.aspect = FullAspect::Ultra32;
             }},
            // The race cameras (full-m2, Cameras): both on (the defaults), a release build, a controller mapping that feeds GT4's
            // right stick from a trigger (the 360 look given off, with the reason), and a GT4 program without them (greyed in a
            // test build; Play does not pass them).
            {"F04i-display-cameras", Page::Display, [&] { st_.full.displayTab = 6; }},
            {"F04j-release-look-cameras", Page::Display, [&] {
                 showUnbuilt_ = false;
                 st_.full.displayTab = 6;
             }},
            {"F04k-display-cameras-trigger-on-right-stick", Page::Display, [&] {
                 st_.full.displayTab = 6;
                 st_.pad.stickAxis[1] = 5;
             }},
            {"F04l-display-cameras-gt4-without-them", Page::Display, [&] {
                 full_.cameras = full_.moreCameras = false;
                 st_.full.displayTab = 6;
             }},
            // More race cameras (full-m2, MoreCams): a chosen set (no bumper, the far chase out, the chase views locked and the
            // bonnet level), and a GT4 program with only the far chase (the Cameras track's: the rest greyed, not passed).
            {"F04m-display-cameras-views-and-tilt", Page::Display, [&] {
                 st_.full.displayTab = 6;
                 st_.full.camViews = 0x1E; // bonnet, roof, close chase, chase
                 st_.full.camLock = 0x1D;  // bumper, roof, close chase, chase locked; bonnet level
             }},
            {"F04n-display-cameras-gt4-with-only-the-far-chase", Page::Display, [&] {
                 full_.moreCameras = false;
                 st_.full.displayTab = 6;
             }},
            {"F05-display-smoothness", Page::Display, [&] { st_.full.displayTab = 3; }},
            {"F05b-display-smoothness-not-prepared", Page::Display, [&] {
                 st_.full.displayTab = 3;
                 st_.full.prepared.clear();
             }},
            // The FPS counter (TB6, developer): simple, detailed with every stat, detailed with a chosen few, and Test Build 5's GT4
            // program (on / off only: the stats greyed).
            {"F05c-display-smoothness-fps-simple", Page::Display, [&] {
                 st_.full.displayTab = 3;
                 st_.full.fpsCounter = 1;
             }},
            {"F05d-display-smoothness-fps-detailed", Page::Display, [&] {
                 st_.full.displayTab = 3;
                 st_.full.fpsCounter = 2;
                 st_.full.fpsStats = 0x7FF;
             }},
            {"F05e-display-smoothness-fps-detailed-chosen", Page::Display, [&] {
                 st_.full.displayTab = 3;
                 st_.full.fpsCounter = 2;
                 st_.full.fpsStats = 0x1 | 0x2 | 0x8 | 0x20 | 0x40 | 0x100; // fps, frame time, 1% low, CPU, GPU, VRAM
             }},
            {"F05f-display-smoothness-fps-older-gt4", Page::Display, [&] {
                 full_.fpsStats = false;
                 st_.full.displayTab = 3;
                 st_.full.fpsCounter = 2;
             }},
            {"F06-display-detail-and-textures", Page::Display, [&] { st_.full.displayTab = 4; }},
            // Car and wheel detail (full-m2, Detail): four times as far and always full, a release build, and a GT4 program
            // without them (greyed in a test build; Play does not pass them).
            {"F06b-display-detail-4x-and-full", Page::Display, [&] {
                 st_.full.displayTab = 4;
                 st_.full.carDetail = 2;
                 st_.full.wheelDetail = 3;
                 st_.full.wheelBlur = false;
             }},
            {"F06c-release-look-detail", Page::Display, [&] {
                 showUnbuilt_ = false;
                 st_.full.displayTab = 4;
                 st_.full.carDetail = 2;
                 st_.full.wheelDetail = 3;
             }},
            {"F06d-display-detail-gt4-without-it", Page::Display, [&] {
                 full_.detail = false;
                 st_.full.displayTab = 4;
                 st_.full.carDetail = 2;
             }},
            {"F07-display-hud", Page::Display, [&] { st_.full.displayTab = 5; }},
            // Driving > Race HUD > HUD layout (HUD hand-off 7 A2): the defaults, the widgets dragged to the four corners (the
            // values a drag to each corner writes: the box kept inside the picture), the styles, GT4's HUD hidden, the mirror in
            // every view. The page shows the newest set in the test GT4 folder's hud-pictures (the stock set without one).
            {"F23-race-hud-layout", Page::RaceHud, [&] { hudSel_ = "radar"; }},
            {"F23b-race-hud-four-corners", Page::RaceHud, [&] {
                 auto at = [&](int w, float x, float y) {
                     FullHudWidget &h = st_.full.liveWidgets[static_cast<size_t>(w)];
                     h.on = true, h.xy = true, h.x = x, h.y = y;
                 };
                 at(0, 0.0f, 0.0f);     // radar: top left
                 at(1, 100.0f, 0.0f);   // delta: top right
                 at(2, 0.0f, 100.0f);   // pedals: bottom left
                 at(3, 100.0f, 100.0f); // g-meter: bottom right
                 st_.full.startLights = true;
                 st_.full.lightsX = 50.0f, st_.full.lightsY = 50.0f;
                 hudSel_ = "gmeter";
             }},
            {"F23c-race-hud-styles", Page::RaceHud, [&] {
                 FullHudWidget &r = st_.full.liveWidgets[0];
                 r.xy = true, r.x = 85.0f, r.y = 72.0f, r.size = 120, r.style = 1;
                 st_.full.liveWidgets[1].style = 1;
                 FullHudWidget &g = st_.full.liveWidgets[3];
                 g.on = true, g.xy = true, g.x = 12.0f, g.y = 80.0f, g.style = 1;
                 st_.full.startLights = true;
                 st_.full.startCount = 4;
                 st_.full.startLook = 1;
                 hudSel_ = "delta";
             }},
            {"F23d-race-hud-gt4-hidden", Page::RaceHud, [&] {
                 st_.full.hudHide = 0x3F;
                 st_.full.mirrorMode = 2;
                 hudSel_ = "position";
             }},
            {"F23e-race-hud-mirror-every-view", Page::RaceHud, [&] {
                 st_.full.mirrorMode = 1;
                 hudSel_ = "mirror";
             }},
            // The mouse: each of our boxes pressed at its centre and dragged up and left, a frame at a time (the drag
            // itself runs in the loop below, before the picture).
            {"F23g-race-hud-dragged", Page::RaceHud, [&] {
                 st_.full.liveWidgets[2].on = true;
                 st_.full.liveWidgets[3].on = true;
                 st_.full.startLights = true;
             }},
            // HUD hand-off 7 B4: GT4's own parts moved and resized (the values hud7b-all gave GT4.exe), and each one dragged
            // with the mouse (the drag runs in the loop below, as F23g's).
            {"F23h-race-hud-gt4-moved", Page::RaceHud, [&] {
                 FullHudMoves &m = st_.full.hudMoves;
                 auto at = [&](const char *part, float x, float y, int size) { m[static_cast<size_t>(fullHudMovePart(part))] = FullHudMove{true, x, y, size}; };
                 at("position", 50, 30, 100), at("lap", 72, 30, 150), at("clock", 50, 62, 80), at("besttimes", 14, 84, 100), at("map", 84, 52, 70);
                 at("panel", 50, 46, 60), at("fuel", 94, 30, 150), at("warnings", 50, 80, 150), at("gbar", 50, 12, 100), at("gearspeed", 22, 62, 120);
                 at("mirror", 26, 88, 70);
                 hudSel_ = "gearspeed";
             }},
            {"F23i-race-hud-gt4-dragged", Page::RaceHud, [&] { hudSel_ = "position"; }},
            {"F23f-race-hud-start-lights", Page::RaceHud, [&] {
                 st_.full.startLights = true;
                 hudSel_ = "lights";
             }},
            // HUD hand-off 7 C: the new boxes at their default places (the tower picked: its name, columns and rows), the other
            // choices (the values the hud7c-boxes2 race run gave GT4.exe), and each new box dragged with the mouse.
            {"F23j-race-hud-new-boxes", Page::RaceHud, [&] {
                 for (int w = kFullHudTower; w <= kFullHudLap; ++w)
                     st_.full.liveWidgets[static_cast<size_t>(w)].on = true;
                 hudSel_ = "tower";
             }},
            {"F23k-race-hud-new-choices", Page::RaceHud, [&] {
                 auto at = [&](int w, float x, float y, int size, int see, int style) -> FullHudWidget & {
                     FullHudWidget &h = st_.full.liveWidgets[static_cast<size_t>(w)];
                     h.on = true, h.xy = true, h.x = x, h.y = y, h.size = size, h.seeThrough = see, h.style = style;
                     return h;
                 };
                 FullHudWidget &tw = at(kFullHudTower, 14, 50, 90, 30, 0);
                 tw.towerName = 1, tw.towerCols = 1 | 8, tw.towerRows = 6;
                 at(kFullHudMap, 86, 30, 120, 10, 1);
                 at(kFullHudSplits, 50, 22, 90, 20, 0);
                 at(kFullHudRevs, 50, 8, 120, 20, 1);
                 at(kFullHudSpeed, 60, 90, 100, 20, 0);
                 at(kFullHudGear, 40, 90, 140, 20, 0);
                 at(kFullHudLap, 86, 70, 100, 20, 0);
                 hudSel_ = "tower";
                 ui_.scrollTo("racehud-col", 1000.0f); // (the boxes card scrolled down to the tower's name, columns and rows)
             }},
            // HUD hand-off 10: the Track map on Simple (locked, then rotating) and the Lap counter on Lap and race time, each picked.
            {"F23o-race-hud-map-simple", Page::RaceHud, [&] {
                 FullHudWidget &m = st_.full.liveWidgets[static_cast<size_t>(kFullHudMap)];
                 m.on = true, m.xy = true, m.x = 84, m.y = 32, m.size = 120, m.simple = true;
                 hudSel_ = "map";
                 ui_.scrollTo("racehud-col", 1000.0f);
             }},
            {"F23p-race-hud-map-simple-rotating", Page::RaceHud, [&] {
                 FullHudWidget &m = st_.full.liveWidgets[static_cast<size_t>(kFullHudMap)];
                 m.on = true, m.xy = true, m.x = 84, m.y = 32, m.size = 120, m.simple = true, m.style = 1;
                 hudSel_ = "map";
                 ui_.scrollTo("racehud-col", 1000.0f);
             }},
            {"F23q-race-hud-lap-race-time", Page::RaceHud, [&] {
                 FullHudWidget &l = st_.full.liveWidgets[static_cast<size_t>(kFullHudLap)];
                 l.on = true, l.xy = true, l.x = 86, l.y = 70, l.style = 1;
                 hudSel_ = "lap";
                 ui_.scrollTo("racehud-col", 1000.0f);
             }},
            {"F23l-race-hud-new-dragged", Page::RaceHud, [&] {
                 for (int w = kFullHudTower; w <= kFullHudLap; ++w)
                     st_.full.liveWidgets[static_cast<size_t>(w)].on = true;
                 hudSel_ = "map";
             }},
            // HUD hand-off 7 D with GT3's radar settings (20 Oct 2026): Driving > Race HUD > Radar: the defaults (25 m, cars 90 %,
            // background 35 %, GT3's colours); other choices (near, square, top right below GT4's best and last lap, 10 m, cars 60 %,
            // background 75 %, other colours from GT3's eleven); 80 m with the metres off and an older file's colours (hand-off 9's
            // defaults: not GT3 colours, each row shows the file's as a twelfth swatch); the radar off. And the Controllers page
            // with an older file's Race HUD zoom key and button: no zoom row any more.
            {"F23m-race-hud-radar", Page::RaceHud, [&] { raceHudTab_ = 1; }},
            {"F23n-race-hud-radar-choices", Page::RaceHud, [&] {
                 raceHudTab_ = 1;
                 FullSettings &f = st_.full;
                 FullHudWidget &h = f.liveWidgets[0];
                 h.xy = true, h.style = 1, h.size = 90, h.seeThrough = 25;
                 h.x = 87.1f, h.y = 35.7f; // (the top right preset at 90 % in 16:9)
                 f.radarShow = 1;
                 f.radarRange = 10;
                 f.radarCars = 60;
                 f.radarBg = 75;
                 f.radarColours = {0x56c880, 0xffffff, 0xffffff, 0xff3b30, 0x3e7eff};
             }},
            {"F23o-race-hud-radar-80m", Page::RaceHud, [&] {
                 raceHudTab_ = 1;
                 st_.full.radarLabels = false;
                 st_.full.radarRange = 80;
                 st_.full.radarColours = {0xf2b33d, 0xffffff, 0xf08a3c, 0xe5484d, 0x000000};
             }},
            {"F23p-race-hud-radar-off", Page::RaceHud, [&] {
                 raceHudTab_ = 1;
                 st_.full.liveWidgets[0].on = false;
             }},
            // HUD hand-off 7 E: Driving > Race HUD > Start lights: the defaults (the hold off); the hold on with other choices (two
            // rows, 0.5-2.5 s, AI slow, your car free, held 10 s) and "Try a start" with three lights lit; the jump-start row.
            {"F23q-race-hud-lights", Page::RaceHud, [&] { raceHudTab_ = 2; }},
            {"F23r-race-hud-lights-hold", Page::RaceHud, [&] {
                 raceHudTab_ = 2;
                 FullSettings &f = st_.full;
                 f.startLights = true, f.startLook = 1, f.startCount = 5;
                 f.startHold = true, f.startDelayMin = 0.5f, f.startDelayMax = 2.5f, f.startAi = 3, f.startFree = true, f.startJump = 1;
                 lightsTryAt_ = GetTime() - 2.5, lightsTryOut_ = GetTime() + 600.0;
             }},
            {"F23s-race-hud-lights-scrolled", Page::RaceHud, [&] {
                 raceHudTab_ = 2;
                 FullSettings &f = st_.full;
                 f.startHold = true, f.startFree = true;
                 lightsTryResult_ = "Reaction: 0.231 s";
                 ui_.scrollTo("lights-form", 1000.0f);
             }},
            // The GT3 look (20 Oct 2026, analysis/design/hud-gt3-look): every box on at the places the race runs gave GT4.exe
            // (look-runs.ps1 boxesA: the first styles; boxesB: the other styles), for the previews against the game's.
            {"F23t-race-hud-all-boxes", Page::RaceHud, [&] { hudAllBoxesShot(false); }},
            {"F23u-race-hud-all-boxes-styles", Page::RaceHud, [&] { hudAllBoxesShot(true); }},
            // Driving > Race HUD > Timing (developer, 20 Oct 2026): the defaults (both off); the tower and the split times on with car
            // names; Driver names with the Race Lab's five default drivers sent (gap, best and last, six rows, 110 %); Driver with
            // no drivers sent (four rows); Driver with the Race Lab's names off; both dragged in the picture; the card scrolled to
            // the split times (short names, no columns, 16 rows); and the HUD layout page's tower box with the same drivers.
            {"F23v-race-hud-timing", Page::RaceHud, [&] { raceHudTab_ = 3; }},
            {"F23w-race-hud-timing-on", Page::RaceHud, [&] {
                 raceHudTab_ = 3;
                 st_.full.liveWidgets[static_cast<size_t>(kFullHudTower)].on = true;
                 st_.full.liveWidgets[static_cast<size_t>(kFullHudSplits)].on = true;
             }},
            {"F23x-race-hud-timing-drivers", Page::RaceHud, [&] {
                 raceHudTab_ = 3;
                 racetiming::driversForTest(1);
                 FullHudWidget &tw = st_.full.liveWidgets[static_cast<size_t>(kFullHudTower)];
                 tw.on = true, tw.towerName = 2, tw.towerCols = 1 | 4 | 8, tw.towerRows = 6, tw.size = 110, tw.seeThrough = 10;
                 st_.full.liveWidgets[static_cast<size_t>(kFullHudSplits)].on = true;
             }},
            {"F23y-race-hud-timing-no-drivers", Page::RaceHud, [&] {
                 raceHudTab_ = 3;
                 FullHudWidget &tw = st_.full.liveWidgets[static_cast<size_t>(kFullHudTower)];
                 tw.on = true, tw.towerName = 2, tw.towerRows = 4;
             }},
            {"F23ya-race-hud-timing-names-off", Page::RaceHud, [&] {
                 raceHudTab_ = 3;
                 racetiming::driversForTest(1);
                 st_.full.raceLabDrivers = false;
                 FullHudWidget &tw = st_.full.liveWidgets[static_cast<size_t>(kFullHudTower)];
                 tw.on = true, tw.towerName = 2;
             }},
            {"F23z-race-hud-timing-dragged", Page::RaceHud, [&] {
                 raceHudTab_ = 3;
                 st_.full.liveWidgets[static_cast<size_t>(kFullHudTower)].on = true;
                 st_.full.liveWidgets[static_cast<size_t>(kFullHudSplits)].on = true;
             }},
            {"F23za-race-hud-timing-scrolled", Page::RaceHud, [&] {
                 raceHudTab_ = 3;
                 FullHudWidget &tw = st_.full.liveWidgets[static_cast<size_t>(kFullHudTower)];
                 FullHudWidget &sp = st_.full.liveWidgets[static_cast<size_t>(kFullHudSplits)];
                 tw.on = true, tw.towerName = 1, tw.towerCols = 0, tw.towerRows = 16;
                 sp.on = true, sp.size = 120, sp.seeThrough = 40;
                 ui_.scrollTo("timing-col", 1000.0f);
             }},
            {"F23zb-race-hud-layout-tower-drivers", Page::RaceHud, [&] {
                 racetiming::driversForTest(1);
                 FullHudWidget &tw = st_.full.liveWidgets[static_cast<size_t>(kFullHudTower)];
                 tw.on = true, tw.towerName = 2;
                 hudSel_ = "tower";
             }},
            {"F09d-controllers-hud-zoom", Page::Controllers, [&] {
                 controllersTab_ = 0;
                 st_.full.hudZoomKey = 'Z';
                 st_.full.hudZoomPad = 9; // (an older file's LB: read, but no row shows it and nothing is written)
             }},
            // HUD hand-off 8: Driving > Telemetry screen on the developer's rig (three MSI G321Q, the 27-inch on top for the telemetry
            // window); the phone's QR code; the three layouts in the editor, a box picked on each.
            {"F24-telemetry-monitors", Page::Telemetry, [&] {
                 demoRig();
                 st_.full.telemetryScreen = 4;
                 st_.full.telemetryRect = "DISPLAY4,27-inch monitor,2560x1440,0,-1440";
                 st_.full.mmMode = kMmThreeWide;
                 teleShot(0);
             }},
            {"F24b-telemetry-phone-qr", Page::Telemetry, [&] {
                 st_.full.phonePages = true;
                 teleShot(1);
             }},
            {"F24c-telemetry-layout-monitor", Page::Telemetry, [&] {
                 demoRig();
                 st_.full.telemetryScreen = 4;
                 st_.full.telemetryRect = "DISPLAY4,27-inch monitor,2560x1440,0,-1440";
                 teleShot(2);
             }},
            {"F24d-telemetry-layout-phone-upright", Page::Telemetry, [&] { teleShot(3); }},
            {"F24e-telemetry-layout-phone-sideways", Page::Telemetry, [&] { teleShot(4); }},
            {"F24g-telemetry-layout-dragged", Page::Telemetry, [&] {
                 demoRig();
                 st_.full.telemetryScreen = 4;
                 st_.full.telemetryRect = "DISPLAY4,27-inch monitor,2560x1440,0,-1440";
                 teleShot(2);
             }},
            {"F24f-telemetry-monitor-not-connected", Page::Telemetry, [&] {
                 st_.full.telemetryScreen = 3;
                 st_.full.telemetryRect = "DISPLAY7,MSI G321Q,2560x1440,2560,0";
                 teleShot(0);
             }},
            {"F08-multi-screen", Page::Multiscreen, [] {}},
            {"F08b-multi-screen-three-screens", Page::Multiscreen, [&] { st_.full.mmMode = kMmThreeWide; }},
            {"F08c-multi-screen-three-screens-panorama", Page::Multiscreen, [&] {
                 st_.full.mmMode = kMmThreeWide;
                 st_.full.mmPanorama = true;
             }},
            {"F08d-release-look-multi-screen", Page::Multiscreen, [&] {
                 showUnbuilt_ = false;
                 st_.full.mmMode = kMmThreeWide;
             }},
            {"F08e-multi-screen-gt4-without-it", Page::Multiscreen, [&] {
                 full_.screens = false;
                 st_.full.mmMode = 0;
             }},
            // GT4's own Multi Monitor Mode (analysis/design/multi-monitor): the developer's rig (three screens and the 27-inch on top),
            // angled sides; the top screen as the Live view; the laptop's layout (no side screen found); a GT4 without it.
            {"F08f-multi-screen-gt4-own-demo-rig", Page::Multiscreen, [&] {
                 demoRig();
                 st_.full.mmMode = kMmGt4Own;
             }},
            {"F08g-multi-screen-gt4-own-top-live-release", Page::Multiscreen, [&] {
                 demoRig();
                 showUnbuilt_ = false;
                 st_.full.mmMode = kMmGt4Own;
                 st_.full.mmTop = 2;
             }},
            {"F08h-multi-screen-gt4-own-flat-two-monitors", Page::Multiscreen, [&] {
                 st_.full.mmMode = kMmGt4Own;
                 st_.full.mmAngled = false;
             }},
            {"F08i-multi-screen-gt4-own-gt4-without-it", Page::Multiscreen, [&] {
                 demoRig();
                 full_.multiMonitor = false;
                 st_.full.mmMode = kMmGt4Own;
             }},
            {"F09-controllers", Page::Controllers, [&] { controllersTab_ = 0; }},
            {"F09b-wheel", Page::Controllers, [&] { controllersTab_ = 1; }},
            {"F09c-eyetoy", Page::Controllers, [&] { controllersTab_ = 2; }},
            {"F10-saves", Page::Saves, [] {}},
            {"F10b-progress-overview", Page::Saves, [&] {
                 savesProgress_ = true;
                 progressTab_ = 0;
             }},
            {"F10c-progress-gt-mode", Page::Saves, [&] {
                 savesProgress_ = true;
                 progressTab_ = 1;
             }},
            {"F10d-progress-gt-mode-events", Page::Saves, [&] {
                 savesProgress_ = true;
                 progressTab_ = 1;
                 ui_.scrollTo("progress-gt", 560.0f);
             }},
            {"F10e-progress-arcade", Page::Saves, [&] {
                 savesProgress_ = true;
                 progressTab_ = 2;
             }},
            {"F11-garage-my-garage", Page::Garage, [&] {
                 garageTab_ = 0;
                 garageSel_ = car([](const GarageCar &c) { return c.kind == 2; });
             }},
            {"F11b-garage-rigidity", Page::Garage, [&] {
                 garageTab_ = 0;
                 garageSel_ = car([](const GarageCar &c) { return c.rigidity; });
                 ui_.scrollTo("garage-car", 1e6f); // down to Fitted parts (Increase Rigidity and its Remove button)
             }},
            {"F11c-garage-stock-car", Page::Garage, [&] {
                 garageTab_ = 0;
                 garageSort_ = 2;
                 garageSel_ = car([](const GarageCar &c) { return c.kind == 1; });
             }},
            {"F11d-garage-compare", Page::Garage, [&] { garageTab_ = 1; }},
            {"F11e-garage-compare-kw-lbft", Page::Garage, [&] {
                 garageTab_ = 1;
                 st_.full.powerUnit = 2;
                 st_.full.torqueUnit = 2;
                 ui_.scrollTo("compare-table", 1e6f);
             }},
            {"F11h-garage-compare-stats-chosen", Page::Garage, [&] {
                 // "Stats to show": a few chips off (remembered in GT4L_COMPARE_STATS).
                 garageTab_ = 1;
                 st_.full.compareStats = "power,torque,weight,ratio,tyretype,0-100,0-200,0-300,100-0,200-0,top,oil";
             }},
            {"F11k-garage-compare-stats-dropdown", Page::Garage, [&] {
                 // "Stats to show" as a drop-down (developer): the list of tick boxes open, a few stats off.
                 garageTab_ = 1;
                 st_.full.compareStats = "power,torque,weight,ratio,drive,tyres,0-100,0-200,top,oil";
                 compareStatsOpen_ = true;
             }},
            {"F11f-car-dealers-hidden", Page::Garage, [&] {
                 garageTab_ = 2;
                 dealerSection_ = 0;
                 dealerSel_ = "r390_race_98";
             }},
            {"F11g-car-dealers-prize", Page::Garage, [&] {
                 garageTab_ = 2;
                 dealerSection_ = 1;
                 dealerSel_.clear();
             }},
            // Extra Cars (analysis/jp-cars): the ESSO Supra '01 (a test GT4 folder with the pack next to --exe; no switch), its
            // review, and the garage car it becomes. Without the pack Car Dealers has no Extra Cars chip.
            {"F11x-car-dealers-extra-esso", Page::Garage, [&] { dealer(2, "esso_supra_01", false, false); }},
            {"F11x2-car-dealers-extra-esso-review", Page::Garage, [&] { dealer(2, "esso_supra_01", true, true); }},
            {"F11x3-garage-esso", Page::Garage, [&] {
                 garageTab_ = 0;
                 garageSel_ = car([](const GarageCar &c) { return c.label == "esso_supra_01"; });
             }},
            // the developer's second round (2026-10-08): Hidden Cars' rare cars (the list at its end, one selected), a black car selected
            // (its picture darkened to GT4's black), and Prize Cars' Dodge VIPER GTSR Concept '00 (GT4's Viper picture, not the
            // AE86 stand-in its own name holds), the Dodge chip chosen.
            {"F11p-car-dealers-hidden-tt-r", Page::Garage, [&] {
                 garageTab_ = 2;
                 dealerSection_ = 0;
                 dealerSel_ = "tt_dtm_02"; // the Abt Audi TT-R: the developer's one rare car GT4 gives as no prize, with the black cars
             }},
            // TB6 (developer: "aren't they just prize cars?"): Prize Cars holds every car of GT4's prize table; his rare race cars are
            // among them (the CALSONIC SKYLINE GT-R and the BMW McLaren F1 GTR selected, their maker chips chosen).
            {"F11s-car-dealers-prize-calsonic", Page::Garage, [&] {
                 garageTab_ = 2;
                 dealerSection_ = 1;
                 dealerSel_ = "calsonic_skyline_gtr_gpa_93";
                 std::vector<std::string> makers{"All"};
                 if (cat_)
                     for (const PrizeCar &pc : cat_->prizes())
                         if (const DiscCar *d = cat_->car(pc.code); d && std::find(makers.begin(), makers.end(), d->maker) == makers.end())
                             makers.push_back(d->maker);
                 const auto it = std::find(makers.begin(), makers.end(), std::string("Nissan"));
                 dealerMaker_ = it == makers.end() ? 0 : static_cast<int>(it - makers.begin());
             }},
            {"F11s2-car-dealers-prize-mclaren", Page::Garage, [&] {
                 garageTab_ = 2;
                 dealerSection_ = 1;
                 dealerSel_ = "mclaren_f1_gtr_long_97";
                 std::vector<std::string> makers{"All"};
                 if (cat_)
                     for (const PrizeCar &pc : cat_->prizes())
                         if (const DiscCar *d = cat_->car(pc.code); d && std::find(makers.begin(), makers.end(), d->maker) == makers.end())
                             makers.push_back(d->maker);
                 const auto it = std::find(makers.begin(), makers.end(), std::string("BMW"));
                 dealerMaker_ = it == makers.end() ? 0 : static_cast<int>(it - makers.begin());
             }},
            // GT Mode prizes (developer, 2026-10-09: "the progression thing for gt mode"): every group, the test save's results
            // (--save-copy, read only); then the link from Prize Cars (the Ford GT LM Spec II: GT World Championship).
            {"P01-gtmode-prizes-beginner", Page::Garage, [&] { garageTab_ = 3; prizeGroup_ = 0; }},
            {"P02-gtmode-prizes-professional", Page::Garage, [&] { garageTab_ = 3; prizeGroup_ = 1; }},
            {"P03-gtmode-prizes-extreme", Page::Garage, [&] { garageTab_ = 3; prizeGroup_ = 2; }},
            {"P04-gtmode-prizes-american", Page::Garage, [&] { garageTab_ = 3; prizeGroup_ = 3; }},
            {"P05-gtmode-prizes-european", Page::Garage, [&] { garageTab_ = 3; prizeGroup_ = 4; }},
            {"P06-gtmode-prizes-japanese", Page::Garage, [&] { garageTab_ = 3; prizeGroup_ = 5; }},
            {"P07-gtmode-prizes-special-condition", Page::Garage, [&] { garageTab_ = 3; prizeGroup_ = 6; }},
            {"P08-gtmode-prizes-endurance", Page::Garage, [&] { garageTab_ = 3; prizeGroup_ = 7; }},
            {"P09-gtmode-prizes-car-makers", Page::Garage, [&] { garageTab_ = 3; prizeGroup_ = 8; }},
            {"P10-gtmode-prizes-licences", Page::Garage, [&] { garageTab_ = 3; prizeGroup_ = 9; }},
            {"P11-gtmode-prizes-missions", Page::Garage, [&] { garageTab_ = 3; prizeGroup_ = 10; }},
            {"P12-gtmode-prizes-completion", Page::Garage, [&] { garageTab_ = 3; prizeGroup_ = 11; }},
            {"P13-gtmode-prizes-new-courses", Page::Garage, [&] { garageTab_ = 3; prizeGroup_ = 12; }},
            {"P14-car-dealers-prize-link-to-gtmode", Page::Garage, [&] {
                 garageTab_ = 2;
                 dealerSection_ = 1;
                 dealerSel_ = "ford_gt_lm_sp2_04";
             }},
            {"P15-gtmode-prizes-from-prize-cars", Page::Garage, [&] {
                 garageTab_ = 3;
                 if (cat_)
                     for (const PrizeCar &pc : cat_->prizes())
                         if (pc.label == "ford_gt_lm_sp2_04" && !pc.given.empty())
                             prizeFocus_ = guideKey(pc.given.front());
             }},
            {"P16-gtmode-prizes-professional-end", Page::Garage, [&] {
                 garageTab_ = 3;
                 prizeGroup_ = 1;
                 ui_.scrollTo("prize-list", 1e6f);
             }},
            {"F11q-car-dealers-hidden-black-gt-one", Page::Garage, [&] {
                 garageTab_ = 2;
                 dealerSection_ = 0;
                 dealerSel_ = "gt_one_race_exxon_99";
             }},
            {"F11r-car-dealers-prize-viper", Page::Garage, [&] {
                 garageTab_ = 2;
                 dealerSection_ = 1;
                 dealerSel_ = "viper_gtsr_00";
                 // The maker chips follow the prize cars' order (garageDealers): "All", then each maker as first met.
                 std::vector<std::string> makers{"All"};
                 if (cat_)
                     for (const PrizeCar &pc : cat_->prizes())
                         if (const DiscCar *d = cat_->car(pc.code); d && std::find(makers.begin(), makers.end(), d->maker) == makers.end())
                             makers.push_back(d->maker);
                 const auto it = std::find(makers.begin(), makers.end(), std::string("Dodge"));
                 dealerMaker_ = it == makers.end() ? 0 : static_cast<int>(it - makers.begin());
             }},
            // Prize cars with no dealer picture on GT4's disc (analysis/design/prize-cars, 2026-10-19): Prize Cars with a maker
            // chosen and one of them selected (the launcher's render of GT4's car screen, or GT4's picture under its Japanese
            // name), and GT Mode prizes' completion rewards.
            {"P17-car-dealers-prize-mazda-pictures", Page::Garage, [&] { prizeMakerScene("Mazda", "mazdaspeed_6_05"); }},
            {"P18-car-dealers-prize-mercedes-pictures", Page::Garage, [&] { prizeMakerScene("Mercedes-Benz", "motor_carriage_886"); }},
            {"P19-car-dealers-prize-audi-pictures", Page::Garage, [&] { prizeMakerScene("Audi", "nuvolari_quattro_03"); }},
            {"P20-car-dealers-prize-acura-dn-x", Page::Garage, [&] { prizeMakerScene("Acura", "acura_dnx_02"); }},
            {"P21-car-dealers-prize-all-end", Page::Garage, [&] {
                 prizeMakerScene("All", "xjr9_race_88");
                 ui_.scrollTo("dealer-list", 1e6f);
             }},
            {"P22-car-dealers-prize-formula-gt", Page::Garage, [&] { prizeMakerScene("All", "formula_gran_turismo_04"); }},
            {"F12-cheats", Page::Cheats, [&] {
                 st_.cheats.noday = true;
                 st_.cheats.card = true;
                 st_.cheats.ticked = {"credits", "licence", "test", "event", "mission", "options"};
             }},
            {"F12b-cheats-one-page-sheet", Page::Cheats, [&] {
                 st_.cheats.ticked = {"credits", "licence", "test", "event", "mission", "options"};
                 modal_ = Modal::CheatSave;
             }},
            {"F13-network", Page::Network, [] {}},
            {"F13b-network-lan", Page::Network, [&] { st_.full.netMode = 1; }},
            {"F13c-network-windows-on-this-pc", Page::Network, [&] {
                 st_.full.netMode = 2;
                 st_.full.netIndex = 2;
             }},
            // Sound (engine bass): the default (medium), off and high; a GT4 program without the engine bass in a test build (greyed,
            // "Not built yet") and in a release build; the real GT4 program named (its strings).
            {"F18-sound", Page::Sound, [] {}},
            {"F18b-sound-off", Page::Sound, [&] { st_.full.engineBass = 0; }},
            {"F18c-sound-high", Page::Sound, [&] { st_.full.engineBass = 3; }},
            {"F18d-sound-gt4-without-engine-bass", Page::Sound, [&] { full_.engineBass = false; }},
            {"F18e-release-look-sound-gt4-without-engine-bass", Page::Sound, [&] {
                 showUnbuilt_ = false;
                 full_.engineBass = false;
             }},
            {"F18f-sound-with-the-real-gt4", Page::Sound, [&] { full_ = inst_.exeFound ? fullFeatures(inst_.exe) : FullFeatures{}; }},
            // (developer 2026-10-20 "why cant we access engine bass anymore?": with any GT4.exe running, a test GT4 too, the choices were greyed)
            {"F18g-sound-while-a-gt4-runs", Page::Sound, [&] { otherGameRunning_ = true; }},
            // Sound > Sound packs (analysis/design/sound-packs): every car, per car, off, a GT4 program without them, the real one.
            {"F21-sound-packs-every-car", Page::Sound, [&] {
                 st_.full.soundTab = 1;
                 st_.full.soundMode = FullSoundMode::EveryCar;
                 st_.full.soundPackAll = pathText(soundPacksFolder() / "starter-v6");
             }},
            {"F21b-sound-packs-per-car", Page::Sound, [&] {
                 st_.full.soundTab = 1;
                 st_.full.soundMode = FullSoundMode::PerCar;
                 st_.full.soundPackAll = pathText(soundPacksFolder() / "starter-v6");
                 st_.full.soundCars = {{82, pathText(soundPacksFolder() / "My Supra recording")}};
             }},
            {"F21c-sound-packs-off", Page::Sound, [&] {
                 st_.full.soundTab = 1;
                 st_.full.soundMode = FullSoundMode::Off;
             }},
            {"F21d-sound-packs-gt4-without-sound-packs", Page::Sound, [&] {
                 st_.full.soundTab = 1;
                 full_.soundPacks = false;
             }},
            {"F21e-sound-packs-with-the-real-gt4", Page::Sound, [&] {
                 st_.full.soundTab = 1;
                 st_.full.soundMode = FullSoundMode::EveryCar;
                 st_.full.soundPackAll = pathText(soundPacksFolder() / "starter-v6");
                 full_ = inst_.exeFound ? fullFeatures(inst_.exe) : FullFeatures{};
             }},
            {"F14-about", Page::About, [&] { aboutView_ = 0; }},
            {"F22-racelab", Page::RaceLab, [&] { st_.full.raceLabRecord = true; racelab::showTabForTest(0); }},
            // The Race Lab tab (analysis/design/racelab-tab): AI drivers with a name being changed, Laps, Races, Settings.
            {"F22b-racelab-rename", Page::RaceLab, [&] { racelab::showTabForTest(0, true); }},
            {"F22c-racelab-laps", Page::RaceLab, [&] { racelab::showTabForTest(1); }},
            {"F22d-racelab-races", Page::RaceLab, [&] { racelab::showTabForTest(2); }},
            {"F22e-racelab-settings", Page::RaceLab, [&] { st_.full.raceLabRecord = true; racelab::showTabForTest(3); }},
            {"F14b-how-it-works", Page::About, [&] {
                 aboutView_ = 1;
                 ui_.scrollTo("how", 1e6f);
             }},
            // As a release build shows them: "Not built yet" hidden.
            {"F15-release-look-display", Page::Display, [&] {
                 showUnbuilt_ = false;
                 st_.full.displayTab = 0;
             }},
            {"F15b-release-look-smoothness", Page::Display, [&] {
                 showUnbuilt_ = false;
                 st_.full.displayTab = 3;
             }},
            {"F15c-release-look-garage", Page::Garage, [&] {
                 showUnbuilt_ = false;
                 garageTab_ = 0;
                 garageSel_ = car([](const GarageCar &c) { return c.rigidity; });
             }},
            {"F15d-release-look-car-dealers", Page::Garage, [&] {
                 showUnbuilt_ = false;
                 garageTab_ = 2;
                 dealerSection_ = 0;
             }},
            // The Garage's changes (build part 2), on memory card slot 1 (a folder card holding a copy of the 40-day test save).
            {"F17a-garage-oil-change-confirm", Page::Garage, [&] { confirm([](const GarageCar &c) { return c.slot == 0; }, GarageAction::OilChange); }},
            {"F17b-garage-wash-confirm", Page::Garage, [&] { confirm([](const GarageCar &c) { return c.slot == 0; }, GarageAction::Wash); }},
            {"F17c-garage-remove-rigidity-confirm", Page::Garage, [&] { confirm([](const GarageCar &c) { return c.rigidity; }, GarageAction::RemoveRigidity); }},
            // Rigidity Refresh (developer, 2026-10-08): the button next to Oil change and Wash car on a car GT4 offers it for (the
            // car being driven: body wear 3,000 km), on the RX-7 (Increase Rigidity, 15 km of wear: GT4 says already done),
            // its confirmation and its result.
            {"F11i-garage-rigidity-refresh", Page::Garage, [&] {
                 garageTab_ = 0;
                 garageSel_ = car([](const GarageCar &c) { return c.kind == 2; });
             }},
            {"F11j-garage-rigidity-refresh-fresh-body", Page::Garage, [&] {
                 garageTab_ = 0;
                 garageSel_ = car([](const GarageCar &c) { return c.rigidity; });
             }},
            {"F17n-garage-rigidity-refresh-confirm", Page::Garage, [&] { confirm([](const GarageCar &c) { return c.kind == 2; }, GarageAction::RefreshBody); }},
            {"F17o-garage-rigidity-refresh-done", Page::Garage, [&] {
                 confirm([](const GarageCar &c) { return c.kind == 2; }, GarageAction::RefreshBody);
                 editResult_ = EditResult{};
                 editResult_.ok = true;
                 editResult_.message = editDoneText(editReq_, backupDir() / "GT4-mc0-2026-10-08_1200-00-before-edit.zip");
                 modal_ = Modal::GarageResult;
             }},
            {"F17d-dealers-hidden-not-enough-credits", Page::Garage, [&] { dealer(0, "r390_race_98", true, false); }},
            {"F17e-dealers-hidden-add-free-confirm", Page::Garage, [&] { dealer(0, "r390_race_98", false, true); }},
            {"F17f-dealers-prize-buy-confirm", Page::Garage, [&] { dealer(1, prizeLabel, true, true); }},
            {"F17p-dealers-rare-buy-confirm", Page::Garage, [&] { dealer(0, "skyline_2000gtb_67", true, true); }},
            {"F17q-dealers-rare-add-free-confirm", Page::Garage, [&] { dealer(0, "mclaren_f1_gtr_long_97", false, true); }},
            // Prize Cars' maker list open (developer, 2026-10-11: it flashed and closed; the long list now fits and scrolls).
            {"F17r-dealers-prize-maker-list-open", Page::Garage, [&] {
                 dealer(1, prizeLabel, true, false);
                 ui_.openDropdownForTest("dealer-maker");
             }},
            // Colour choice when buying (developer, 2026-10-11): the black Formula GT in Hidden Cars, a prize car's colours with its
            // third colour chosen, and that purchase's Review with the colour.
            {"F17s1-dealers-hidden-formula-gt-black", Page::Garage, [&] { dealer(0, "formula_gran_turismo_04", false, false); }},
            {"F17s2-dealers-prize-colours", Page::Garage, [&] {
                 dealer(1, colourLabel, true, false, 2);
             }},
            {"F17s3-dealers-prize-colour-review", Page::Garage, [&] {
                 dealer(1, colourLabel, true, true, 2);
             }},
            {"F17g-dealers-specifications", Page::Garage, [&] {
                 dealer(0, "_787b_91", false, false);
                 specsLabel_ = "_787b_91";
                 modal_ = Modal::CarSpecs;
             }},
            {"F17h-garage-done", Page::Garage, [&] {
                 confirm([](const GarageCar &c) { return c.slot == 0; }, GarageAction::OilChange);
                 // The result dialog with the text a change gives (editDoneText); the backup name is this picture's example.
                 editResult_ = EditResult{};
                 editResult_.ok = true;
                 editResult_.message = editDoneText(editReq_, backupDir() / "GT4-mc0-2026-10-08_0315-07-before-edit.zip");
                 modal_ = Modal::GarageResult;
             }},
            {"F17i-garage-gt4-running", Page::Garage, [&] {
                 gameRunning_ = true;
                 garageTab_ = 0;
                 garageSel_ = car([](const GarageCar &c) { return c.rigidity; });
             }},
            {"F17j-garage-stock-car", Page::Garage, [&] {
                 garageTab_ = 0;
                 garageSort_ = 2;
                 garageSel_ = car([](const GarageCar &c) { return c.kind == 1; });
             }},
            {"F17k-garage-card-file", Page::Garage, [&] {
                 st_.cards[0].file = true;
                 st_.cards[0].path = pathText(saveCopy); // the .ps2 test copy (read only), as a card file in slot 1
                 garageTab_ = 0;
                 garageSel_ = car([](const GarageCar &c) { return c.rigidity; });
             }},
            {"F17l-saves-backups", Page::Saves, [] {}},
            {"F17m-garage-change-cut-off", Page::Garage, [&] {
                 // What the launcher shows at its start when a Garage change was cut off (its note still in game\backups and
                 // the save half changed): this picture's example values; nothing is read or written.
                 garageTab_ = 0;
                 pending_ = PendingChange{};
                 pending_.present = true;
                 pending_.state = "half";
                 pending_.slot = 1;
                 pending_.title = "Buy the Nissan R390 GT1 Race Car '98?";
                 pending_.backup = "GT4-mc0-2026-10-08_0315-07-before-edit.zip";
                 modal_ = Modal::GarageCutOff;
             }},
            // Today's GT4.exe, as it is (its strings): without the 480p update the development builds say so.
            {"F16-display-with-todays-gt4", Page::Display, [&] {
                 full_ = inst_.exeFound ? fullFeatures(inst_.exe) : FullFeatures{};
                 st_.full.displayTab = 0;
                 st_.full.scaling = FullScaling::Bicubic;
             }},
            {"F16b-release-look-with-todays-gt4", Page::Display, [&] {
                 // A release build with a GT4 program that lacks the 480p update: its video mode, colour and shape are left out.
                 full_ = inst_.exeFound ? fullFeatures(inst_.exe) : FullFeatures{};
                 showUnbuilt_ = false;
                 st_.full.displayTab = 0;
             }},
            {"F16c-release-look-shape-with-todays-gt4", Page::Display, [&] {
                 full_ = inst_.exeFound ? fullFeatures(inst_.exe) : FullFeatures{};
                 showUnbuilt_ = false;
                 st_.full.displayTab = 2;
             }},
            // Save details' Repair and Change name (developer 2026-10-09). The first --save-copy: a save with the old memory-card
            // bug's extra data (the developer's GT4 Test Build 11 save, a copy); the second, when given: a clean save (the repaired copy).
            // Read only: a test picture never confirms.
            {"F20a-save-details-repair", Page::Saves, [&] { details(0); }},
            {"F20b-save-repair-confirm", Page::Saves, [&] {
                 details(0);
                 const EditTarget target = detailsEditTarget();
                 openGarageEdit(repairEdit(), &target);
             }},
            {"F20c-save-repair-done", Page::Saves, [&] {
                 details(0);
                 const EditTarget target = detailsEditTarget();
                 openGarageEdit(repairEdit(), &target);
                 editResult_ = EditResult{};
                 editResult_.ok = true;
                 editResult_.message = editDoneText(editReq_, backupDir() / "GT4-mc0-2026-10-09_1200-00-before-edit.zip");
                 modal_ = Modal::GarageResult;
             }},
            {"F20d-save-rename-repair-first", Page::Saves, [&] {
                 details(0);
                 nameEdit_ = "Player";
                 modal_ = Modal::RenameSave;
             }},
            {"F20e-save-details-clean", Page::Saves, [&] { details(1); }},
            {"F20f-save-rename-entry", Page::Saves, [&] {
                 details(1);
                 nameEdit_ = "Player";
                 modal_ = Modal::RenameSave;
             }},
            {"F20g-save-rename-too-long", Page::Saves, [&] {
                 details(1);
                 nameEdit_ = "Aaa Racing1";
                 modal_ = Modal::RenameSave;
             }},
            {"F20h-save-rename-confirm", Page::Saves, [&] {
                 details(1);
                 const EditTarget target = detailsEditTarget();
                 openGarageEdit(renameEdit("Player"), &target);
             }},
        };
        int written = 0, failed = 0, visibleChecks = 0;
        bool sheetDone = false;
        for (const auto &[w, h] : opt_.sizes)
        {
            RenderTexture2D rt = LoadRenderTexture(w, h);
            if (rt.id == 0)
            {
                report(strf("FAIL: no %dx%d render target", w, h));
                ++failed;
                continue;
            }
            for (const Scene &sc : scenes)
            {
                const std::string sn = sc.name;
                if (!opt_.only.empty() && !startsWith(sn, opt_.only))
                    continue;
                // The same starting point for every picture.
                st_ = base;
                full_ = baseFeatures;
                showUnbuilt_ = baseUnbuilt;
                monitors_ = baseMonitors;
                page_ = sc.page;
                modal_ = Modal::None;
                gameRunning_ = false;
                otherGameRunning_ = false;
                game_.startMs = sys::tickMs() - 12u * 60000u;
                buildShot_ = false;
                build_ = BuildJob{};
                inst_.exeFound = baseExeFound;
                disc_ = baseDisc;
                controllersTab_ = 0;
                aboutView_ = 0;
                savesProgress_ = false;
                progressTab_ = 0;
                garageTab_ = 0;
                garageSort_ = 0;
                dealerSection_ = 0;
                dealerMaker_ = 0;
                prizeGroup_ = 0;
                prizeFocus_.clear();
                prizeFocusShown_ = false;
                guideKey_.clear();
                compareStatsOpen_ = false;
                previewFullscreen_ = false;
                zoomFocus_ = Vector2{0.235f, 0.135f};
                editPlan_ = EditPlan{};
                editResult_ = EditResult{};
                pending_ = PendingChange{};
                savesView_ = 0;
                detailsOpen_ = false;
                {
                    // The Garage and its changes: the folder card of memory card slot 1 (as a player has it); the other
                    // pages: the save copy, as before.
                    const bool card = startsWith(sn, "F11") || startsWith(sn, "F15c") || startsWith(sn, "F15d") || startsWith(sn, "F17");
                    const fs::path want = card ? fs::path() : saveCopy;
                    if (want != garageCopy_)
                    {
                        garageCopy_ = want;
                        loadGarage(true);
                    }
                }
                hudSel_ = "radar";
                hudDrag_.clear();
                raceHudTab_ = 0;
                racetiming::driversForTest(0);
                lightsTryAt_ = -1e9, lightsTryResult_.clear();
                for (const char *id : {"racehud-col", "radar-col", "lights-form", "timing-col", "pic-left", "progress-gt", "progress-arcade", "compare-table", "garage-list", "garage-car", "dealer-list", "dealer-car", "prize-list", "how", "roadmap"})
                    ui_.resetScroll(id);
                ui_.closePopups();
                dirty_ = false; // the pictures show the settings as saved (the test mode writes nothing)
                sc.setup();
                if (sc.page == Page::Cheats)
                {
                    readCheatSave(true);
                    cheatThumbKey_.clear();
                }
                if (sn == "F23g-race-hud-dragged")
                {
                    auto run = [&](const ui::Input &in) {
                        prepareFullPreview();
                        prepareCheats(w, h);
                        BeginTextureMode(rt);
                        ClearBackground(ui::theme().bg);
                        frame(w, h, in);
                        EndTextureMode();
                    };
                    run(ui::Input{});
                    const char *ids[] = {"radar", "delta", "pedals", "gmeter", "lights"};
                    for (int k = 0; k < 5; ++k)
                    {
                        const Rectangle r = g_raceHudBoxDp[k];
                        const float dx = -60.0f, dy = k == 4 ? 40.0f : -40.0f; // (dp; the start lights sit at the top)
                        const Vector2 p{(r.x + r.width * 0.5f) * ui_.s, (r.y + r.height * 0.5f) * ui_.s};
                        ui::Input in;
                        in.enabled = true;
                        in.mouse = p;
                        in.pressed = in.down = true;
                        run(in);
                        in.pressed = false;
                        for (int i = 1; i <= 6; ++i)
                        {
                            in.mouse = Vector2{p.x + dx * ui_.s * i / 6.0f, p.y + dy * ui_.s * i / 6.0f};
                            run(in);
                        }
                        in.down = false;
                        in.released = true;
                        run(in);
                        run(ui::Input{});
                        const Rectangle q = g_raceHudBoxDp[k];
                        report(strf("drag %s: centre %.0f,%.0f -> %.0f,%.0f dp (asked %+.0f,%+.0f); picked %s", ids[k], r.x + r.width * 0.5f,
                                    r.y + r.height * 0.5f, q.x + q.width * 0.5f, q.y + q.height * 0.5f, dx, dy, hudSel_.c_str()));
                    }
                }
                if (sn == "F23z-race-hud-timing-dragged")
                {
                    // Driving > Race HUD > Timing: the split times dragged (0, -40 dp), then the tower (-80, +30 dp) in the picture.
                    auto run = [&](const ui::Input &in) {
                        prepareFullPreview();
                        prepareCheats(w, h);
                        BeginTextureMode(rt);
                        ClearBackground(ui::theme().bg);
                        frame(w, h, in);
                        EndTextureMode();
                    };
                    run(ui::Input{});
                    const int idx[2] = {7, 5}; // (g_raceHudBoxDp: the split times, the tower)
                    const float dxs[2] = {0.0f, -80.0f}, dys[2] = {-40.0f, 30.0f};
                    for (int k = 0; k < 2; ++k)
                    {
                        const Rectangle r = g_raceHudBoxDp[idx[k]];
                        const Vector2 p{(r.x + r.width * 0.5f) * ui_.s, (r.y + r.height * 0.5f) * ui_.s};
                        ui::Input in;
                        in.enabled = true;
                        in.mouse = p;
                        in.pressed = in.down = true;
                        run(in);
                        in.pressed = false;
                        for (int i = 1; i <= 6; ++i)
                        {
                            in.mouse = Vector2{p.x + dxs[k] * ui_.s * i / 6.0f, p.y + dys[k] * ui_.s * i / 6.0f};
                            run(in);
                        }
                        in.down = false;
                        in.released = true;
                        run(in);
                        run(ui::Input{});
                        const Rectangle q = g_raceHudBoxDp[idx[k]];
                        const FullHudWidget &hw = st_.full.liveWidgets[static_cast<size_t>(k == 1 ? kFullHudTower : kFullHudSplits)];
                        report(strf("timing drag %s: centre %.0f,%.0f -> %.0f,%.0f dp (asked %+.0f,%+.0f); now at %.1f,%.1f %%", k == 1 ? "tower" : "splits",
                                    r.x + r.width * 0.5f, r.y + r.height * 0.5f, q.x + q.width * 0.5f, q.y + q.height * 0.5f, dxs[k], dys[k], hw.x, hw.y));
                    }
                }
                if (sn == "F24g-telemetry-layout-dragged")
                {
                    // HUD hand-off 8 T3: the monitor layout's timing tower's corner handle dragged (-40, +30 dp), then the
                    // box dragged (+30, +40 dp): it changes width and height on their own, then moves.
                    auto run = [&](const ui::Input &in) {
                        BeginTextureMode(rt);
                        ClearBackground(ui::theme().bg);
                        frame(w, h, in);
                        EndTextureMode();
                    };
                    run(ui::Input{});
                    auto drag = [&](Vector2 from, Vector2 by, const char *what) {
                        const Rectangle r = teleBoxes().empty() ? Rectangle{} : teleBoxes()[0];
                        ui::Input in;
                        in.enabled = true;
                        in.mouse = Vector2{from.x * ui_.s, from.y * ui_.s};
                        in.pressed = in.down = true;
                        run(in);
                        in.pressed = false;
                        for (int i = 1; i <= 6; ++i)
                        {
                            in.mouse = Vector2{(from.x + by.x * i / 6.0f) * ui_.s, (from.y + by.y * i / 6.0f) * ui_.s};
                            run(in);
                        }
                        in.down = false;
                        in.released = true;
                        run(in);
                        run(ui::Input{});
                        const Rectangle q = teleBoxes().empty() ? Rectangle{} : teleBoxes()[0];
                        report(strf("tele %s: %.0f,%.0f %.0fx%.0f -> %.0f,%.0f %.0fx%.0f dp (asked %+.0f,%+.0f)", what, r.x, r.y, r.width, r.height, q.x, q.y,
                                    q.width, q.height, by.x, by.y));
                    };
                    if (!teleBoxes().empty())
                    {
                        const Rectangle r = teleBoxes()[0];
                        drag(Vector2{r.x + r.width - 5, r.y + r.height - 5}, Vector2{-40, 30}, "resize tower");
                        const Rectangle q = teleBoxes()[0];
                        drag(Vector2{q.x + q.width * 0.3f, q.y + q.height * 0.3f}, Vector2{30, 40}, "move tower");
                    }
                }
                if (sn == "F23l-race-hud-new-dragged")
                {
                    // HUD hand-off 7 C's boxes (kOwn 5..11): each pressed at its centre and dragged 60 dp towards the picture's
                    // middle, then one mouse-wheel step down (smaller); where it went and the value written each time.
                    auto run = [&](const ui::Input &in) {
                        prepareFullPreview();
                        prepareCheats(w, h);
                        BeginTextureMode(rt);
                        ClearBackground(ui::theme().bg);
                        frame(w, h, in);
                        EndTextureMode();
                    };
                    run(ui::Input{});
                    const char *ids[] = {"tower", "map", "splits", "revs", "speed", "gear", "lap"};
                    for (int k = 5; k < 12; ++k)
                    {
                        const Rectangle r = g_raceHudBoxDp[k];
                        const float cxNow = r.x + r.width * 0.5f, cyNow = r.y + r.height * 0.5f;
                        // (towards the middle of the picture: the HUD area is 16:9 at the page's left; a fixed step left or right)
                        const float dx = cxNow > 580.0f ? -60.0f : 60.0f, dy = cyNow > 280.0f ? -40.0f : 40.0f;
                        const Vector2 p{cxNow * ui_.s, cyNow * ui_.s};
                        ui::Input in;
                        in.enabled = true;
                        in.mouse = p;
                        in.pressed = in.down = true;
                        run(in);
                        in.pressed = false;
                        for (int i = 1; i <= 6; ++i)
                        {
                            in.mouse = Vector2{p.x + dx * ui_.s * i / 6.0f, p.y + dy * ui_.s * i / 6.0f};
                            run(in);
                        }
                        in.down = false;
                        in.released = true;
                        run(in);
                        run(ui::Input{});
                        const Rectangle q = g_raceHudBoxDp[k];
                        ui::Input wh;
                        wh.enabled = true;
                        wh.mouse = Vector2{(q.x + q.width * 0.5f) * ui_.s, (q.y + q.height * 0.5f) * ui_.s};
                        wh.wheel = -1.0f;
                        run(wh);
                        run(ui::Input{});
                        const FullHudWidget &hw = st_.full.liveWidgets[static_cast<size_t>(k - 1)];
                        report(strf("drag %s: centre %.0f,%.0f -> %.0f,%.0f dp (asked %+.0f,%+.0f); picked %s; then a wheel step: GT4_HUD_%s=%s", ids[k - 5], cxNow,
                                    cyNow, q.x + q.width * 0.5f, q.y + q.height * 0.5f, dx, dy, hudSel_.c_str(), kFullHudLiveKeys[k - 1],
                                    fullHudValue(k - 1, hw, 16.0f / 9.0f).c_str()));
                    }
                }
                if (sn == "F23i-race-hud-gt4-dragged")
                {
                    // GT4's parts: each one drawn in the set's view pressed at its centre, dragged 60 dp towards the picture's
                    // middle, then one mouse-wheel step up (bigger); the value written each time.
                    auto run = [&](const ui::Input &in) {
                        prepareFullPreview();
                        prepareCheats(w, h);
                        BeginTextureMode(rt);
                        ClearBackground(ui::theme().bg);
                        frame(w, h, in);
                        EndTextureMode();
                    };
                    run(ui::Input{});
                    static const char *const ids[11] = {"position", "lap", "clock", "besttimes", "map", "panel", "fuel", "warnings", "gbar", "gearspeed", "mirror"};
                    for (int k = 0; k < 11; ++k)
                    {
                        const Rectangle r = g_raceHudGt4Dp[k];
                        if (r.width <= 0.0f)
                        {
                            report(strf("drag %s: not drawn in this set's view", ids[k]));
                            continue;
                        }
                        const float dx = r.x + r.width * 0.5f < 700.0f ? 60.0f : -60.0f, dy = r.y + r.height * 0.5f < 330.0f ? 40.0f : -40.0f;
                        const Vector2 p{(r.x + r.width * 0.5f) * ui_.s, (r.y + r.height * 0.5f) * ui_.s};
                        ui::Input in;
                        in.enabled = true;
                        in.mouse = p;
                        in.pressed = in.down = true;
                        run(in);
                        in.pressed = false;
                        for (int i = 1; i <= 6; ++i)
                        {
                            in.mouse = Vector2{p.x + dx * ui_.s * i / 6.0f, p.y + dy * ui_.s * i / 6.0f};
                            run(in);
                        }
                        in.down = false;
                        in.released = true;
                        run(in);
                        ui::Input wheel;
                        wheel.enabled = true;
                        wheel.mouse = in.mouse;
                        wheel.wheel = 1.0f;
                        run(wheel);
                        run(ui::Input{});
                        const Rectangle q = g_raceHudGt4Dp[k];
                        report(strf("drag %s: centre %.0f,%.0f -> %.0f,%.0f dp (asked %+.0f,%+.0f), %.0fx%.0f -> %.0fx%.0f dp after one wheel step; picked %s",
                                    ids[k], r.x + r.width * 0.5f, r.y + r.height * 0.5f, q.x + q.width * 0.5f, q.y + q.height * 0.5f, dx, dy, r.width,
                                    r.height, q.width, q.height, hudSel_.c_str()));
                    }
                    report("GT4_HUD_MOVE=" + fullHudMoveList(st_.full.hudMoves));
                }
                for (int pass = 0; pass < 2; ++pass)
                {
                    if (pass == 1)
                        sc.setup(); // again: a scroll to the end needs the content's height from the first pass
                    prepareFullPreview();
                    prepareCheats(w, h);
                    BeginTextureMode(rt);
                    ClearBackground(ui::theme().bg);
                    frame(w, h, ui::Input{});
                    EndTextureMode();
                }
                Image img = LoadImageFromTexture(rt.texture);
                ImageFlipVertical(&img);
                ImageFormat(&img, PIXELFORMAT_UNCOMPRESSED_R8G8B8);
                const std::string file = pathText(opt_.shots / pathFrom(strf("%s-%dx%d.png", sc.name, w, h)));
                if (ExportImage(img, file.c_str()))
                {
                    ++written;
                    report("shot " + file);
                }
                else
                {
                    ++failed;
                    report("FAIL: shot " + file);
                }
                UnloadImage(img);
                ui_.closePopups();
                // What the scene's settings give GT4.exe (the picture scenes), and the garage / progress numbers.
                if (startsWith(sn, "F02") || startsWith(sn, "F03") || startsWith(sn, "F04") || startsWith(sn, "F05") || startsWith(sn, "F06") || startsWith(sn, "F08") || startsWith(sn, "F16") || startsWith(sn, "F23"))
                {
                    std::string keys;
                    for (const auto &[k, v] : playLaunch().env)
                    {
                        if (k == "GT4_RACE_MODE" || k == "GT4_RACE_COLOUR" || k == "GT4_PICTURE_FILTER" || k == "GT4_ASPECT" || k == "GT3_DISPLAY_ASPECT" ||
                            k == "GT3_FULLSCREEN" || k == "GT3_DISPLAY_FILTER" || k == "GT3_RENDERER" || k == "GT3_GS_GL_SCALE" || k == "GT3_GS_GL_FAST" ||
                            k == "GT4_CAM_FAR_CHASE" || k == "GT4_CAM_LOOK" || k == "GT4_CAM_VIEWS" || k == "GT4_CAM_LOCK" || k == "GT4_CAR_DETAIL" || k == "GT4_WHEEL_DETAIL" || k == "GT4_WHEEL_BLUR" ||
                            k == "GT4_SCREENS" || k == "GT4_SCREENS_GEOMETRY" || k == "GT4_MONITOR" || k == "GT4_FPS_COUNTER" || k == "GT4_FPS_STATS" ||
                            startsWith(k, "GT4_MM_") || k == "GT4_NET" || k == "GT4_NET_INDEX" || k == "GT4_NET_PEERS" || startsWith(k, "GT4_HUD_") ||
                            startsWith(k, "GT4_START_") || k == "GT4_MIRROR")
                            keys += " " + k + "=" + v;
                    }
                    report(sn + ": Play passes" + keys + "; preview filter mode " + std::to_string(previewFilterMode()));
                    // GT4's own Multi Monitor Mode: the side screens' copies Play would start with it.
                    for (const sys::Launch &side : mmSideLaunches(playLaunch()))
                    {
                        std::string sk;
                        for (const auto &[k, v] : side.env)
                            if (startsWith(k, "GT4_MM_") || k == "GT4_NET_INDEX" || k == "GT4_MONITOR" || k == "GT3_SOUND_OUTPUT" || k == "PS2X_WHEEL")
                                sk += " " + k + "=" + v;
                        report(sn + ": side copy" + sk);
                    }
                }
                if ((startsWith(sn, "F11") || startsWith(sn, "F17") || startsWith(sn, "F20")) && w == opt_.sizes.front().first && h == opt_.sizes.front().second)
                {
                    reportGarage(sn);
                    if (modal_ == Modal::GarageConfirm)
                    {
                        report("  confirmation: " + (editPlan_.ok ? editPlan_.title + " [" + editPlan_.confirm + "]" : "refused: " + editPlan_.why));
                        for (const std::string &c : editPlan_.changes)
                            report("    - " + c);
                    }
                    report("  the Garage can change the save: " + (garageEditBlock().empty() ? std::string("yes") : "no (" + garageEditBlock() + ")"));
                }
                if (startsWith(sn, "F10b") && w == opt_.sizes.front().first && h == opt_.sizes.front().second)
                {
                    const Progress &p = progress_;
                    report(strf("%s: progress: credits %s, Day %lld, %zu cars, races %llu won %u, distance %.1f km, prize money %s, A-spec points %llu, prize cars %u, "
                                "event races won %d of %d, licences %zu, missions %d/34, coffee breaks %d/5, events listed %zu, Arcade courses %zu, machine records %zu, "
                                "available cars %zu courses %zu",
                                sn.c_str(), groupedNumber(p.details.credits).c_str(), static_cast<long long>(p.details.gameDay), garage_.cars.size(),
                                static_cast<unsigned long long>(p.races), p.wins, p.distanceM / 1000.0, groupedNumber(p.details.prizeMoney).c_str(),
                                static_cast<unsigned long long>(p.details.aspecPoints), p.details.prizeCars, p.eventRacesWon, p.eventRaces, p.licences.size(),
                                p.details.missionsDone, p.details.coffeeDone, p.events.size(), p.courses.size(), p.machineRecords.size(), p.availableCars,
                                p.availableCourses));
                }
                if (startsWith(sn, "F12b") && !sheetDone && !opt_.cheatFiles.empty())
                {
                    // The printable sheet: all six cards on ONE page (developer), A4 and Letter, saved as pictures (never printed here).
                    sheetDone = true;
                    for (int letter = 0; letter < 2; ++letter)
                    {
                        std::vector<Image> pages = renderCheatSheet(tickedCodes(st_.cheats), letter == 1, true);
                        report(strf("cheats: six-card sheet (%s): %zu page(s)%s", letter ? "Letter" : "A4", pages.size(), pages.size() == 1 ? " (ok: one page)" : " (FAIL: not one page)"));
                        if (pages.size() != 1)
                            ++failed;
                        for (size_t pi = 0; pi < pages.size(); ++pi)
                        {
                            const fs::path pp = opt_.cheatFiles / strf("sheet-six-%s-page%zu.png", letter ? "letter" : "a4", pi + 1);
                            fs::create_directories(opt_.cheatFiles, ec);
                            if (exportPng(pages[pi], pp))
                                report("cheats: sheet picture " + pathText(pp) + strf(" (%dx%d)", pages[pi].width, pages[pi].height));
                        }
                        for (Image &pg : pages)
                            UnloadImage(pg);
                    }
                }
                ++visibleChecks;
                if (sys::anyVisibleWindow())
                {
                    report("FAIL: a launcher window became visible");
                    UnloadRenderTexture(rt);
                    return 4;
                }
            }
            UnloadRenderTexture(rt);
        }
        st_ = base;
        full_ = baseFeatures;
        showUnbuilt_ = baseUnbuilt;
        gameRunning_ = false;
        modal_ = Modal::None;
        previewFullscreen_ = false;
        garageCopy_ = saveCopy;
        report("Full edition: GT4 program reads (the real program, its strings): " + (inst_.exeFound ? fullFeatures(inst_.exe).text() : std::string("no GT4 program")));
        report(strf("monitors: %zu%s", monitors_.size(), opt_.demo ? " (demo)" : ""));
        report(strf("network addresses listed: %zu%s", netAddresses_.size(), opt_.demo ? " (demo)" : ""));
        report(strf("devices opened by the launcher: %d (enumeration only in this mode)", devices_.openedCount()));
        report(strf("raylib: the guarded build (flag ps2x_skip_hidden_swap at %p)", static_cast<void *>(&ps2x_skip_hidden_swap)));
        report(strf("visible-window checks: %d, none visible", visibleChecks));
        report(strf("test shots: %d written, %d failed", written, failed));
        return failed ? 1 : 0;
    }
}
