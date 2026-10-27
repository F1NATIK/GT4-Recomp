// GT4 Recompilation launcher, Full edition: its setup, sidebar, the GT4 program's features, Prepare shaders, the car data
// and the garage it reads (FULL-LAUNCHER-PLAN.md 1). Compiled into the Full target only (GT4L_EDITION_FULL).

#include "gt4stock_app.h"

#include "gt4full_ui.h"
#include "gt4save.h"
#include "gt4stock_system.h"
#include "gt4stock_util.h"
#include "shared/gt4_full_keys.h"

#include <algorithm>
#include <chrono>
#include <process.h>
#include <climits>
#include <cmath>
#include <cstdint>
#include <cstring>

#ifndef GT4L_SHOW_UNBUILT
#define GT4L_SHOW_UNBUILT 0
#endif
#ifndef GT4STOCK_COMMIT
#define GT4STOCK_COMMIT "unknown"
#endif

namespace gt4stock
{
    using ui::Align;
    using ui::Icon;
    using ui::Style;
    using ui::Weight;

    namespace
    {
        constexpr float kSidebarW = 232.0f;
        Rectangle R(float x, float y, float w, float h) { return Rectangle{x, y, w, h}; }

        // A whole-text replacement; the text is left as it is when the phrase is not found.
        std::string replaced(std::string s, const std::string &from, const std::string &to)
        {
            const size_t at = s.find(from);
            if (at != std::string::npos)
                s.replace(at, from.size(), to);
            return s;
        }
    }

    std::string fullHowText(const std::string &md)
    {
        // The explainer is the Classic edition's (gt4stock_how_it_works.md, byte for byte the about-text source); two
        // sentences name the edition and its renderer.
        std::string s = replaced(md, "A Vulkan renderer works in a separate test build, matched just as closely in the scenes tried, and is being added as an option; the Classic edition uses OpenGL.",
                                 "A Vulkan renderer is matched just as closely in the scenes tried: GT4 draws with it, with OpenGL there for graphics cards where Vulkan has trouble.");
        s = replaced(s, "GT4 Recompilation, Classic edition, a small program that starts GT4.exe with your settings. Pages: Play, Display (scanline filter), Controllers (pad, wheel, pedals, player 2, EyeToy webcam), Saves (cards, backups, photo folder, Save details), Cheats and About.",
                     "the GT4 Recompilation launcher, a small program that starts GT4.exe with your settings. Pages: Play, Display (picture, screen shape, renderer and more), Sound (engine bass, sound packs), Controllers (pad, wheel, pedals, player 2, EyeToy webcam), Saves (cards, backups, photo folder, Game progress and save details), Garage, Cheats, Multi-screen, Race Lab, Network/Online and About.");
        return s;
    }

    // ------------------------------------------------------------------ setup
    void App::fullSetup()
    {
        full_ = inst_.exeFound ? fullFeatures(inst_.exe) : FullFeatures{};
        if (!inst_.exeFound)
            full_.error = "no GT4 program";
        showUnbuilt_ = GT4L_SHOW_UNBUILT != 0 && !opt_.releaseLook;
        if (opt_.demo)
        {
            // The demo pictures show the GT4 program the Full edition is made for: the 480p update and Vulkan in (as the
            // Classic demo shows its runtimes); the test report also gives the real program's answer.
            full_ = FullFeatures{};
            full_.read = true;
            full_.raceMode = full_.raceColour = full_.aspect = full_.bicubic = full_.lanczos = full_.vulkan = full_.renderer = full_.squarePixels =
                full_.span = full_.exclusive = full_.net = full_.engineBass = full_.fpsCounter = full_.fpsStats = full_.internalRes = full_.wideShapes = full_.cameras = full_.moreCameras = full_.hud = full_.detail = full_.screens = full_.multiMonitor = full_.lut = full_.texSmooth = full_.texDump = full_.rendererAuto = full_.extraCars = full_.soundPacks = true;
            st_.full.prepared = exeStamp() + " ok";
        }
        st_.full.soundMapFile = pathText(soundPacksFolder() / "cars.txt"); // Sound packs: the per-car map file GT4.exe reads
        netAddresses_ = sys::ipv4Addresses(opt_.demo);
        netListed_ = true;
        checkPendingChange(); // a Garage change cut off last time: offered back at the start
        applyExtraCars();
    }

    // ------------------------------------------------------------------ extra cars (analysis/jp-cars)
    fs::path App::extraCarsPack() const { return (inst_.exeFound ? inst_.exe.parent_path() : inst_.root) / "extra-cars" / "esso.gt4x"; }

    void App::applyExtraCars()
    {
        // No switch (developer 2026-10-14): the pack is served whenever it sits next to a GT4 program that serves it too (the Full
        // GT4.exe has the key's name; the Classic one never serves a pack), unless GT4_EXTRA_CARS=off is in the environment
        // (the hidden way out, which GT4.exe reads as well). Looked for every ~2 s (120 frames), and at once the first time.
        if (extraCarsKey_ != "?" && ++extraCarsTick_ < 120)
            return;
        extraCarsTick_ = 0;
        const char *env = std::getenv("GT4_EXTRA_CARS");
        const std::string e = env ? lower(trim(std::string(env))) : std::string();
        const bool off = e == "off" || e == "0" || e == "none";
        // The key's name is in a Full GT4.exe only (the Classic one never serves a pack); full_.classic is no help here, as a
        // development GT4.exe names GT4_CLASSIC_BUILD=1 too.
        const bool exeServes = !full_.read || full_.extraCars;
        std::error_code ec;
        const fs::path pack = extraCarsPack();
        const bool on = !off && exeServes && fs::is_regular_file(pack, ec);
        const std::string key = on ? "esso|" + pathText(pack) + strf("|%lld", static_cast<long long>(fs::last_write_time(pack, ec).time_since_epoch().count()))
                                   : std::string();
        if (key == extraCarsKey_)
            return;
        extraCarsKey_ = key;
        extraCarsNote_.clear();
        std::string err;
        if (!gt4save::Gt4Disc::useExtraCars(on ? pack : fs::path(), err))
            extraCarsNote_ = err;
        // The names and the car data read again through the pack (or without it).
        names_.reset();
        cat_.reset();
        catError_.clear();
        startNames();
        for (SaveExtraCars &x : savesExtra_)
            x.key.clear();
        scanExtraCarsInSaves();
    }

    void App::scanExtraCarsInSaves()
    {
        for (int slot = 1; slot <= 2; ++slot)
        {
            SaveExtraCars &x = savesExtra_[static_cast<size_t>(slot - 1)];
            const bool file = cardIsFile(slot);
            const fs::path source = file ? cardFile(slot) : cardDir(slot);
            const fs::path gamedata = file ? source : source / gt4save::kSaveFolder / gt4save::kGamedataFile;
            std::error_code ec;
            const bool exists = fs::is_regular_file(gamedata, ec);
            const std::string key = pathText(gamedata) + (exists ? strf("|%lld", static_cast<long long>(fs::last_write_time(gamedata, ec).time_since_epoch().count())) : std::string("|-"));
            if (key == x.key)
                continue;
            x = SaveExtraCars{};
            x.key = key;
            if (!exists)
                continue;
            const gt4save::Save s = file ? gt4save::loadCardFile(source) : gt4save::loadCardFolder(source);
            if (!s.loaded)
                continue;
            for (const gt4save::GarageEntry &g : s.fields.garageIndex)
                if (g.inGarage && g.code && *g.code == kExtraCarCode)
                {
                    x.slots.push_back(g.slot);
                    x.riding = x.riding || g.slot == s.fields.garageFields.ridingSlot;
                }
        }
    }

    bool App::pageVisible(Page p) const
    {
        // Multi-screen: a release build shows it only for a GT4 program that has three screens (its marker, full_.screens);
        // everything else on it is not built yet.
        if (p == Page::Multiscreen)
            return showUnbuilt_ || full_.screens;
        // Race HUD (HUD hand-off 7): with a GT4 program that draws our own widgets (its marker), as Display > HUD was.
        if (p == Page::RaceHud)
            return showUnbuilt_ || (full_.read && full_.hud);
        // Telemetry screen (HUD hand-off 8): with a GT4 program that has the pages service and our own HUD (the same marker).
        if (p == Page::Telemetry)
            return showUnbuilt_ || (full_.read && full_.hud);
        return true;
    }

    std::string App::exeStamp() const
    {
        std::error_code ec;
        if (!inst_.exeFound)
            return "none";
        const uint64_t bytes = fs::file_size(inst_.exe, ec);
        const auto when = fs::last_write_time(inst_.exe, ec).time_since_epoch().count();
        return strf("%llu-%lld", static_cast<unsigned long long>(bytes), static_cast<long long>(when));
    }

    bool App::preparedForThisExe() const { return st_.full.prepared == exeStamp() + " ok"; }

    scan::Placement App::fullPlacement(const MonitorInfo *m, int *winW, int *winH) const
    {
        int w = st_.windowW, h = st_.windowH;
        if (st_.fullscreen && st_.full.span && !monitors_.empty())
        {
            int x0 = INT32_MAX, y0 = INT32_MAX, x1 = INT32_MIN, y1 = INT32_MIN;
            for (const MonitorInfo &mm : monitors_)
            {
                x0 = std::min(x0, mm.x);
                y0 = std::min(y0, mm.y);
                x1 = std::max(x1, mm.x + mm.w);
                y1 = std::max(y1, mm.y + mm.h);
            }
            w = x1 - x0;
            h = y1 - y0;
        }
        else if (st_.fullscreen && m)
        {
            w = m->w;
            h = m->h;
        }
        if (winW)
            *winW = w;
        if (winH)
            *winH = h;
        // The picture's shape: GT4's 16:9 (GT4_ASPECT) or 4:3; square pixels show the 640 x 448 frame's own shape; the wider
        // shapes: the race picture (GT4's menus are 16:9 in the middle).
        const float aspect = fullAspectRatioOf(st_.full.aspect);
        const Fit fit = Fit::Fill; // (no TV lines in the Full edition: the picture fills, as GT4.exe does with lines off)
        return scan::fitPicture(static_cast<float>(w), static_cast<float>(h), scan::kLines, aspect, fit);
    }

    std::string App::cameraLookBlock() const
    {
        // GT4's right stick (PS2 rx / ry) is the controller's right stick (raylib axes 2 / 3) in the standard mapping; the
        // 360 look reads what feeds it, so another axis there (a trigger, the left stick) would turn the view with it.
        const int rx = st_.pad.stickAxis[0], ry = st_.pad.stickAxis[1];
        if (rx == 2 && ry == 3)
            return "";
        const int other = rx != 2 ? rx : ry;
        return strf("your controller mapping (Controllers > Controller) puts %s on GT4's right stick", other < 0 ? "nothing" : gamepadAxisName(other, false));
    }

    void App::adjustFullLaunch(sys::Launch &l) const
    {
        // Bicubic and Lanczos reach a GT4 program that reads them; an older one gets bilinear, its own smooth picture
        // (an unknown value would give a development GT4.exe its old Box picture, present_scanline_math.h parsePicture).
        // In the package layout the variable wins over gt4-start.env, which keeps the player's choice.
        for (auto &[k, v] : l.env)
            if (k == "GT3_FULLSCREEN" && v == "exclusive" && full_.read && !full_.exclusive)
                v = "1";
        for (auto &[k, v] : l.env)
            if (k == "GT4_PICTURE_FILTER" && full_.read && ((v == "bicubic" && !full_.bicubic) || (v == "lanczos" && !full_.lanczos)))
                v = "bilinear";
        // The 480p update's keys reach only a GT4 program that reads them (F3: a setting GT4.exe has no side for is stored,
        // never passed; its page control is greyed or hidden). Square pixels go with GT4_ASPECT (4:3 with square pixels).
        if (full_.read)
            l.env.erase(std::remove_if(l.env.begin(), l.env.end(),
                                       [&](const std::pair<std::string, std::string> &kv) {
                                           return (kv.first == "GT4_RACE_MODE" && !full_.raceMode) || (kv.first == "GT4_RACE_COLOUR" && !full_.raceColour) ||
                                                  ((kv.first == "GT4_ASPECT" || kv.first == "GT3_DISPLAY_ASPECT") && !full_.aspect) ||
                                                  (kv.first == "GT4_ENGINE_BASS" && !full_.engineBass) || // the Sound page's engine bass
                                                  ((kv.first == "GT4_SOUND_PACK" || kv.first == "GT4_SOUND_PACK_MAP") && !full_.soundPacks) || // Sound packs
                                                  ((kv.first == "GT4_FPS_COUNTER" || kv.first == "GT4_FPS_STATS") && !full_.fpsCounter) || // Smoothness, FPS counter
                                                  (kv.first == "GT4_FPS_STATS" && !full_.fpsStats);
                                       }),
                        l.env.end());
        // A GT4 program with the FPS counter but not its simple and detailed modes (Test Build 5's) reads on / off only: it
        // gets on for either.
        if (full_.read && full_.fpsCounter && !full_.fpsStats)
            for (auto &[k, v] : l.env)
                if (k == "GT4_FPS_COUNTER" && v != "off")
                    v = "on";
        // The race cameras (Display > Cameras) reach only a GT4 program that has them (its marker, full_.cameras). The 360 look
        // is given off when this launcher's controller mapping feeds GT4's right stick from something other than the
        // controller's right stick (cameraLookBlock: the look would turn with it).
        // Colour LUTs (Display > Picture, Colour) reach only a GT4 program that has them (its marker, full_.lut).
        if (full_.read && !full_.lut)
            l.env.erase(std::remove_if(l.env.begin(), l.env.end(),
                                       [](const std::pair<std::string, std::string> &kv) { return kv.first == "GT4_LUT" || kv.first == "GT4_LUT_STRENGTH"; }),
                        l.env.end());
        if (full_.read && !full_.cameras)
            l.env.erase(std::remove_if(l.env.begin(), l.env.end(),
                                       [](const std::pair<std::string, std::string> &kv) { return kv.first == "GT4_CAM_FAR_CHASE" || kv.first == "GT4_CAM_LOOK" || kv.first == "GT4_CAM_LOOK_STICK"; }),
                        l.env.end());
        for (auto &[k, v] : l.env)
            if (k == "GT4_CAM_LOOK" && v == "360" && !cameraLookBlock().empty())
                v = "off";
        // The view button's views and the views locked to the car's tilt (Display > Cameras, MoreCams) reach only a GT4 program
        // that has them (its marker, full_.moreCameras); an older one with the race cameras still gets the far chase.
        if (full_.read && (!full_.moreCameras || !full_.cameras))
            l.env.erase(std::remove_if(l.env.begin(), l.env.end(),
                                       [](const std::pair<std::string, std::string> &kv) { return kv.first == "GT4_CAM_VIEWS" || kv.first == "GT4_CAM_LOCK"; }),
                        l.env.end());
        // Texture smoothing (Display > Detail and textures) reaches only a GT4 program that has it (its marker, full_.texSmooth).
        if (full_.read && !full_.texSmooth)
            l.env.erase(std::remove_if(l.env.begin(), l.env.end(),
                                       [](const std::pair<std::string, std::string> &kv) { return kv.first == "GT4_TEX_SMOOTH"; }),
                        l.env.end());
        // The renderer choice (Display > Smoothness): an older GT4 program reads vk only (gl and auto were ignored there).
        if (full_.read && !full_.rendererAuto)
            for (auto &[k, v] : l.env)
                if (k == "GT3_RENDERER")
                    v = "vk";
        // Texture dump and replacement reach only a GT4 program that has them (full_.texDump).
        if (full_.read && !full_.texDump)
            l.env.erase(std::remove_if(l.env.begin(), l.env.end(),
                                       [](const std::pair<std::string, std::string> &kv) {
                                           return kv.first == "GT4_TEXTURE_DUMP" || kv.first == "GT4_TEXTURE_DUMP_OPAQUE" || kv.first == "GT4_TEXTURE_REPLACE";
                                       }),
                        l.env.end());
        // Car and wheel detail (Display > Detail) reach only a GT4 program that has them (its marker, full_.detail).
        if (full_.read && !full_.detail)
            l.env.erase(std::remove_if(l.env.begin(), l.env.end(),
                                       [](const std::pair<std::string, std::string> &kv) {
                                           return kv.first == "GT4_CAR_DETAIL" || kv.first == "GT4_WHEEL_DETAIL" || kv.first == "GT4_WHEEL_BLUR";
                                       }),
                        l.env.end());
        // Three screens (Multi-screen) reach only a GT4 program that has them (its marker, full_.screens).
        if (full_.read && !full_.screens)
            l.env.erase(std::remove_if(l.env.begin(), l.env.end(),
                                       [](const std::pair<std::string, std::string> &kv) { return kv.first == "GT4_SCREENS" || kv.first == "GT4_SCREENS_GEOMETRY"; }),
                        l.env.end());
        // The wider shapes (21:9, 32:9, 48:9) reach only a GT4 program that has them (its marker, full_.wideShapes); an older one
        // reads GT4_ASPECT too (16:9 / 4:3 only), so it is given 16:9.
        for (auto &[k, v] : l.env)
            if (k == "GT4_ASPECT" && full_.read && !full_.wideShapes && (v == "21:9" || v == "32:9" || v == "48:9"))
                v = "16:9";
        // Internal resolution (Display > Resolution): only a GT4 program whose internal resolution was checked with GT4 (its
        // marker, full_.internalRes) gets the choice. An older development GT4.exe reads GT3_GS_GL_SCALE too, so it is given
        // native, which wins over a gt4-start.env line in the package layout; a scale that is not offered is given native too.
        // A resolution preset (Display > Resolution) gives the scale for the monitor GT4 starts on (Automatic follows it).
        if (st_.full.resPreset != 0)
        {
            const MonitorInfo *pm = selectedMonitor();
            const int ps = fullPresetScale(st_.full.resPreset, pm ? pm->h : 0);
            if (ps >= 1)
                for (auto &[k, v] : l.env)
                {
                    if (k == "GT3_GS_GL_SCALE")
                        v = std::to_string(ps);
                    else if (k == "GT3_GS_GL_FAST")
                        v = fullDrawingFast(ps, st_.full.drawing) ? "1" : "0";
                }
        }
        const bool scaleLive = !full_.read || full_.internalRes;
        for (auto &[k, v] : l.env)
            if (k == "GT3_GS_GL_SCALE")
            {
                long long n = 0;
                if (!scaleLive || !parseInt(v, n) || !fullScaleBuilt(static_cast<int>(n)))
                    v = "1";
            }
        if (!scaleLive)
            l.env.erase(std::remove_if(l.env.begin(), l.env.end(), [](const std::pair<std::string, std::string> &kv) { return kv.first == "GT3_GS_GL_FAST"; }),
                        l.env.end());
        // GT4's own Multi Monitor Mode: the middle copy hosts (Play / Center) on the middle monitor; the side copies are
        // started after it (startMmSides). A GT4 program without it, or no side screen found: one GT4 as before.
        if (mmOwnLive())
        {
            const std::vector<FullMmCopy> plan = mmPlan();
            for (const auto &[k, v] : fullMmKeys(st_.full, plan.front(), plan.size(), mmGroup()))
                setLaunchKey(l, k, v);
            if (const MonitorInfo *m = mmMonitor("center"))
                setLaunchKey(l, "GT4_MONITOR", strf("%d,%d", m->x, m->y) + (m->name.empty() ? std::string() : " " + m->name));
            l.env.erase(std::remove_if(l.env.begin(), l.env.end(), [](const std::pair<std::string, std::string> &kv) { return kv.first == "GT3_WINDOW_SIZE"; }),
                        l.env.end());
        }
    }

    // ------------------------------------------------------------------ GT4's own Multi Monitor Mode (Multi-screen)
    // Several GT4 copies on this PC, one per screen, linked by the LAN service (GT4_NET=loop), each told its screen by
    // GT4_MM_* (GT4.exe's gt4_multimon.h; analysis/design/multi-monitor). The middle copy is the one you drive; the side
    // copies follow its controller through the menus to Multi-LAN Race, then connect and show their views by themselves.
    void App::setLaunchKey(sys::Launch &l, const std::string &key, const std::string &value)
    {
        for (auto &[k, v] : l.env)
            if (k == key)
            {
                v = value;
                return;
            }
        l.env.emplace_back(key, value);
    }

    bool App::mmOwnLive() const
    {
        if (!mmOwnChosen() || (full_.read && !full_.multiMonitor))
            return false;
        return mmPlan().size() >= 2;
    }

    const MonitorInfo *App::mmMonitor(const std::string &role) const
    {
        const MonitorInfo *mid = selectedMonitor();
        if (!mid || role == "center")
            return mid;
        for (const MonitorInfo &m : monitors_)
        {
            if (&m == mid)
                continue;
            const int sharedV = std::min(m.y + m.h, mid->y + mid->h) - std::max(m.y, mid->y);
            const int sharedH = std::min(m.x + m.w, mid->x + mid->w) - std::max(m.x, mid->x);
            if (role == "left" && sharedV >= mid->h / 2 && std::abs(m.x + m.w - mid->x) <= 8)
                return &m;
            if (role == "right" && sharedV >= mid->h / 2 && std::abs(m.x - (mid->x + mid->w)) <= 8)
                return &m;
            if (role == "top" && sharedH >= std::min(m.w, mid->w) / 2 && std::abs(m.y + m.h - mid->y) <= 8)
                return &m;
        }
        return nullptr;
    }

    std::vector<FullMmCopy> App::mmPlan() const
    {
        std::vector<FullMmCopy> plan;
        for (const FullMmCopy &c : fullMmCopies(st_.full))
            if (mmMonitor(c.role))
                plan.push_back(c);
        if (plan.empty() || plan.front().role != "center")
            plan.clear();
        return plan;
    }

    std::string App::mmGroup() const { return strf("gt4l-%d", _getpid()); }

    bool App::mmCardCopy(const std::string &role, sys::Launch &l, std::string &why) const
    {
        // Each side copy gets a fresh copy of memory card slot 1 (the player's own card is never given to two GT4s): a card
        // file is copied to <GT4 folder>\multi-monitor\<screen>\card.ps2, the folder card game\mc0 to ...\<screen>\mc0.
        std::error_code ec;
        const fs::path dir = inst_.root / "multi-monitor" / role;
        fs::remove_all(dir, ec);
        fs::create_directories(dir, ec);
        if (ec)
        {
            why = "could not make " + pathText(dir);
            return false;
        }
        if (cardIsFile(1))
        {
            const fs::path src = cardFile(1), dst = dir / "card.ps2";
            if (fs::is_regular_file(src, ec) && !fs::copy_file(src, dst, fs::copy_options::overwrite_existing, ec))
            {
                why = "could not copy " + pathText(src);
                return false;
            }
            setLaunchKey(l, "GT4_MC0", dst.string()); // a missing card: GT4 makes a new one (never the player's)
            return true;
        }
        const fs::path src = inst_.gameDir / "mc0", dst = dir / "mc0";
        if (fs::is_directory(src, ec))
            fs::copy(src, dst, fs::copy_options::recursive | fs::copy_options::overwrite_existing, ec);
        else
            fs::create_directories(dst, ec);
        if (ec)
        {
            why = "could not copy " + pathText(src);
            return false;
        }
        setLaunchKey(l, "GT4_MC0", "folder");
        setLaunchKey(l, "GT4_MC0_FOLDER", dst.string());
        return true;
    }

    std::vector<sys::Launch> App::mmSideLaunches(const sys::Launch &centre) const
    {
        std::vector<sys::Launch> sides;
        if (!mmOwnLive())
            return sides;
        const std::vector<FullMmCopy> plan = mmPlan();
        for (size_t i = 1; i < plan.size(); ++i)
        {
            sys::Launch l = centre;
            for (const auto &[k, v] : fullMmKeys(st_.full, plan[i], plan.size(), mmGroup()))
                setLaunchKey(l, k, v);
            if (const MonitorInfo *m = mmMonitor(plan[i].role))
                setLaunchKey(l, "GT4_MONITOR", strf("%d,%d", m->x, m->y) + (m->name.empty() ? std::string() : " " + m->name));
            if (!l.log.empty())
                l.log = l.log.parent_path() / ("gt4-" + plan[i].role + ".log");
            sides.push_back(std::move(l));
        }
        return sides;
    }

    void App::startMmSides(const sys::Launch &centre)
    {
        mmSides_.clear();
        const std::vector<FullMmCopy> plan = mmPlan();
        std::vector<sys::Launch> sides = mmSideLaunches(centre);
        std::string problems;
        for (size_t i = 0; i < sides.size() && i + 1 < plan.size(); ++i)
        {
            std::string why;
            if (!mmCardCopy(plan[i + 1].role, sides[i], why))
            {
                problems += " " + plan[i + 1].name + ": " + why + ".";
                continue;
            }
            sys::Process p;
            std::string error;
            if (sys::start(sides[i], p, error))
                mmSides_.push_back(p);
            else
                problems += " " + plan[i + 1].name + ": " + error;
        }
        if (!problems.empty())
            setStatus("GT4 Multi Monitor Mode: not every screen started." + problems, ui::theme().warn);
        else if (!sides.empty())
            setStatus(strf("GT4 Multi Monitor Mode: %d GT4 windows. Go to Arcade Mode > Multi-LAN Race with your controller on the middle "
                           "screen; the other screens follow you there, then show their views.", static_cast<int>(sides.size()) + 1),
                      ui::theme().ok);
    }

    void App::closeMmSides(bool hard)
    {
        for (sys::Process &p : mmSides_)
        {
            int code = 0;
            if (!p.handle || sys::poll(p, code))
                continue;
            if (hard || p.closeSent)
                sys::terminate(p);
            else
                sys::requestClose(p);
        }
        mmSides_.erase(std::remove_if(mmSides_.begin(), mmSides_.end(), [](sys::Process &p) { int code = 0; return !p.handle || sys::poll(p, code); }),
                       mmSides_.end());
    }

    // ------------------------------------------------------------------ sidebar
    void App::drawFullSidebar()
    {
        const ui::Theme &t = ui::theme();
        if (!pageVisible(page_))
            page_ = Page::Play; // a page a release build hides (a --page from a test build)
        const float H = ui_.height;
        ui_.fill(R(0, 0, kSidebarW, H), t.sidebar);
        ui_.fill(R(kSidebarW - 1, 0, 1, H), t.border);
        // The mark: a "4" on scanlines (as the Classic edition), and "GT4 Recompilation / Full launcher" (the mock-ups).
        const Rectangle tile = R(24, 26, 46, 46);
        ui_.round(tile, 11, t.red);
        for (int i = 0; i < 9; ++i)
            ui_.fill(R(tile.x + 4, tile.y + 6 + i * 4.2f, tile.width - 8, 1.4f), ui::alpha(BLACK, 0.18f));
        ui_.text("4", tile.x + tile.width * 0.5f, tile.y + 3, 32, WHITE, Weight::Bold, Align::Center);
        {
            const float room = kSidebarW - 84 - 14;
            float size = 18.0f;
            while (size > 13.0f && ui_.textWidth("Recompilation", size, Weight::Bold) > room)
                size -= 0.5f;
            // (no "FULL LAUNCHER" chip: one launcher, no edition labels, developer 2026-10-20)
            ui_.text("GT4", 84, 26, size, t.text, Weight::Bold);
            ui_.text("Recompilation", 84, 26 + size * 1.25f, size, t.text, Weight::Bold);
        }
        // The pages in sections (developer, 2026-10-18: "organize the rest better, into relevant categories"; the Liveries and
        // Roadmap pages removed: their code is on the local branch parked/launcher-extras).
        struct Item
        {
            Page page;
            Icon icon;
            const char *label;
            const char *section = nullptr; // a section heading above this item
        };
        const Item items[] = {{Page::Play, Icon::Play, "Play"},
                              {Page::Display, Icon::Display, "Display", "PICTURE AND SOUND"},
                              {Page::Sound, Icon::Speaker, "Sound"},
                              {Page::Multiscreen, Icon::Screens, "Multi-screen"},
                              {Page::Controllers, Icon::Gamepad, "Controllers", "DRIVING"},
                              {Page::RaceHud, Icon::Eye, "Race HUD"},
                              {Page::Telemetry, Icon::Screens, "Telemetry screen"},
                              {Page::Cheats, Icon::Sparkle, "Cheats"},
                              {Page::Saves, Icon::Card, "Saves", "YOUR GAME"},
                              {Page::Garage, Icon::Garage, "Garage"},
                              {Page::RaceLab, Icon::Trophy, "Race Lab", "RACING TOGETHER"},
                              {Page::Network, Icon::Network, "Network/Online"},
                              {Page::About, Icon::Info, "About", ""}};
        float y = 100;
        int shown = 0, sections = 0;
        for (const Item &it : items)
            if (pageVisible(it.page))
                shown += 1, sections += it.section ? 1 : 0;
        const float head = 24.0f;
        // (32 at the least: thirteen pages with Telemetry screen still fit an 800-high window above the status rows)
        const float step = std::clamp((H - 142 - 24 - 100 - sections * head) / static_cast<float>(std::max(shown, 8)), 32.0f, 46.0f);
        for (const Item &it : items)
        {
            if (!pageVisible(it.page))
                continue;
            if (it.section)
            {
                if (*it.section)
                    ui_.text(it.section, 32, y + 8, 11.5f, t.faint, Weight::Semibold);
                else
                    ui_.fill(R(28, y + head * 0.5f, kSidebarW - 56, 1), t.border);
                y += head;
            }
            const Rectangle r = R(12, y, kSidebarW - 24, step - 4);
            const bool sel = page_ == it.page;
            const bool hot = ui_.hovered(r);
            if (sel)
            {
                ui_.round(r, 10, t.panel2);
                ui_.round(R(12, y + (step - 4) * 0.5f - 12, 4, 24), 2, t.accent);
            }
            else if (hot)
                ui_.round(r, 10, ui::alpha(t.panel2, 0.6f));
            ui_.icon(it.icon, R(32, y + (step - 4) * 0.5f - 11, 22, 22), sel ? t.text : t.muted);
            ui_.text(it.label, 66, y + (step - 4) * 0.5f - 10, 15.5f, sel ? t.text : t.muted, sel ? Weight::Semibold : Weight::Regular);
            if (ui_.clicked(r))
            {
                page_ = it.page;
                if (page_ == Page::Cheats)
                    readCheatSave(true);
                if (page_ == Page::Saves)
                {
                    savesView_ = 0;
                    detailsOpen_ = false;
                    savesProgress_ = false;
                    refreshCards();
                }
                if (page_ == Page::Garage)
                    loadGarage();
                if (page_ == Page::About)
                    aboutView_ = 0;
            }
            y += step;
        }
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
        ui_.text(std::string("v") + kVersion + (showUnbuilt_ ? "  \xc2\xb7  test build" : ""), 20, H - 30, 12, t.faint);
    }

    // ------------------------------------------------------------------ Prepare shaders
    void App::startPrepare()
    {
        const ui::Theme &t = ui::theme();
        if (hidden_ || preparing_)
            return;
        if (busy())
        {
            setStatus("Close GT4 first: shaders are prepared with GT4 closed.", t.warn);
            return;
        }
        if (!inst_.exeFound || !inst_.coreFound)
        {
            setStatus("The GT4 program or game\\CORE.elf was not found.", t.bad);
            return;
        }
        // GT4.exe <root>\game\CORE.elf --boot-seconds 120 --headless with GT3_RENDERER=vk and GT3_VKGL_PREPARE=1: Vulkan builds
        // its listed pipelines, saves them and ends before any game code runs; exit 0 done, 1 not done, 2 no Vulkan
        // (analysis\vulkan\README.md 4). No window, no sound device.
        sys::Launch l;
        l.exe = inst_.exe;
        l.cwd = inst_.root;
        l.args = {inst_.coreElf.wstring(), L"--boot-seconds", L"120", L"--headless"};
        l.env = settingsEnvironment(st_, !inst_.package);
        bool haveRenderer = false;
        for (auto &[k, v] : l.env)
            if (k == "GT3_RENDERER")
            {
                v = "vk";
                haveRenderer = true;
            }
        if (!haveRenderer)
            l.env.emplace_back("GT3_RENDERER", "vk");
        l.env.emplace_back("GT3_VKGL_PREPARE", "1");
        l.log = inst_.root / "logs" / "prepare-shaders.log";
        std::string error;
        if (!sys::start(l, prepare_, error))
        {
            prepareStatus_ = "Could not start: " + error;
            setStatus(prepareStatus_, t.bad);
            return;
        }
        preparing_ = true;
        prepareStatus_ = "Preparing shaders (hidden, a few seconds)...";
        setStatus(prepareStatus_, t.muted);
    }

    void App::pollPrepare()
    {
        if (!preparing_)
            return;
        int code = 0;
        if (!sys::poll(prepare_, code))
            return;
        preparing_ = false;
        const ui::Theme &t = ui::theme();
        const char *result = code == 0 ? "ok" : code == 2 ? "novulkan" : "failed";
        st_.full.prepared = exeStamp() + " " + result;
        markDirty();
        prepareStatus_ = code == 0   ? std::string("Shaders ready for this GT4 program.")
                         : code == 2 ? std::string("Vulkan could not start on this PC (no usable graphics device for it).")
                                     : strf("Could not prepare the shaders (exit %d): see logs\\prepare-shaders.log.", code);
        setStatus(prepareStatus_, code == 0 ? t.ok : t.warn);
    }

    // ------------------------------------------------------------------ the car data and the garage
    void App::startCatalogue()
    {
        if (catLoading_ || cat_ || !names_ || !disc_.exists)
            return;
        catLoading_ = true;
        catError_.clear();
        catKey_ = namesKey();
        const fs::path p = pathFrom(st_.disc);
        std::shared_ptr<const GameNames> names = names_;
        catFuture_ = std::async(std::launch::async, [p, names]() {
            CatalogueLoad r;
            const auto t0 = std::chrono::steady_clock::now();
            r.cat = CarCatalogue::load(p, names.get(), r.error);
            r.ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
            return r;
        });
    }

    void App::pollCatalogue(bool wait)
    {
        applyExtraCars();
        if (!catLoading_)
        {
            // The car data needs the names (labels, makers): started when they are there.
            if (!cat_ && names_ && disc_.exists && catError_.empty())
                startCatalogue();
            if (!catLoading_)
                return;
        }
        if (!wait && catFuture_.wait_for(std::chrono::seconds(0)) != std::future_status::ready)
            return;
        CatalogueLoad r = catFuture_.get();
        catLoading_ = false;
        if (catKey_ != namesKey() || !names_)
            return; // read for another disc or extra-cars choice: read again once the names are there
        catMs_ = r.ms;
        catError_ = r.error;
        if (r.cat)
            cat_ = r.cat;
        loadGarage(true);
    }

    fs::path App::garageSource(std::string *title) const
    {
        if (!garageCopy_.empty())
        {
            if (title)
                *title = "Test copy of a save \xc2\xb7 " + shortPath(garageCopy_, inst_.root);
            return garageCopy_;
        }
        const int slot = std::clamp(st_.full.garageCard, 1, 2);
        const bool file = cardIsFile(slot);
        const fs::path source = file ? cardFile(slot) : cardDir(slot);
        if (title)
            *title = strf("Memory card slot %d", slot) + (file ? " \xc2\xb7 card file " + pathText(source.filename()) : std::string(" \xc2\xb7 folder card ") + shortPath(source, inst_.root));
        return source;
    }

    void App::loadGarage(bool force)
    {
        std::string title;
        const fs::path source = garageSource(&title);
        std::error_code ec;
        const auto stamp = [&](const fs::path &p) {
            std::error_code e;
            return fs::exists(p, e) ? static_cast<long long>(fs::last_write_time(p, e).time_since_epoch().count()) : 0LL;
        };
        const std::string key = pathText(source) + strf("|%lld|%lld|%p|%p", stamp(source), stamp(source / gt4save::kSaveFolder / gt4save::kGamedataFile),
                                                         static_cast<const void *>(names_.get()), static_cast<const void *>(cat_.get()));
        if (!force && key == garageKey_)
            return;
        garageKey_ = key;
        garageTitle_ = title;
        const gt4save::SpecDb *spec = names_ ? names_->spec.get() : nullptr;
        gt4save::Save s;
        if (fs::is_regular_file(source, ec))
            s = gt4save::loadCardFile(source, spec);
        else if (fs::is_regular_file(source / gt4save::kGamedataFile, ec))
            s = gt4save::loadSaveFolder(source, spec);
        else
            s = gt4save::loadCardFolder(source, spec);
        garage_ = buildGarage(s, names_.get(), cat_.get());
        progress_ = buildProgress(s, names_.get());
        if (garageCopy_.empty())
            pending_ = pendingChange(backupDir()); // the Garage's buttons wait while a cut-off change is half done
        if (garageSel_ < 0 || !garage_.bySlot(garageSel_))
            garageSel_ = garage_.cars.empty() ? -1 : garage_.cars.front().slot;
        // Compare: the remembered cars that are still in this garage, else the first four.
        std::vector<int> keep;
        for (int c : st_.full.compareSlots)
            if (garage_.bySlot(c) && keep.size() < 4)
                keep.push_back(c);
        if (keep.empty())
            for (const GarageCar &c : garage_.cars)
                if (keep.size() < 4)
                    keep.push_back(c.slot);
        if (keep != st_.full.compareSlots && !garage_.cars.empty())
        {
            st_.full.compareSlots = keep;
            if (!hidden_)
                markDirty();
        }
    }

    const Texture2D *App::carTexture(const std::string &label, int blackGrey)
    {
        if (label.empty() || !cat_)
            return nullptr;
        const std::string key = blackGrey > 0 ? label + "#black" : label;
        auto it = carTex_.find(key);
        if (it == carTex_.end())
        {
            Texture2D tex{};
            int w = 0, h = 0;
            std::vector<uint8_t> rgba;
            // A hidden black car: the launcher's own picture of it (GT4's 3D render in black), else GT4's picture darkened. A prize
            // car GT4 has no dealer picture of: the launcher's own picture (GT4's 3D render, analysis/design/prize-cars).
            if (const int id = blackGrey > 0 ? blackCarPictureResource(label) : prizeCarPictureResource(label))
            {
                const std::vector<uint8_t> png = sys::resource(id);
                Image img{};
                if (!png.empty())
                    img = LoadImageFromMemory(".png", png.data(), static_cast<int>(png.size()));
                if (img.data)
                {
                    ImageFormat(&img, PIXELFORMAT_UNCOMPRESSED_R8G8B8A8);
                    w = img.width;
                    h = img.height;
                    rgba.assign(static_cast<const uint8_t *>(img.data), static_cast<const uint8_t *>(img.data) + static_cast<size_t>(w) * h * 4);
                    blackGrey = 0; // already black
                    UnloadImage(img);
                }
            }
            if (!rgba.empty() || cat_->picture(label, w, h, rgba))
            {
                if (blackGrey > 0)
                    blackenPicture(rgba, blackGrey);
                Image img{};
                img.data = MemAlloc(static_cast<unsigned int>(rgba.size()));
                std::memcpy(img.data, rgba.data(), rgba.size());
                img.width = w;
                img.height = h;
                img.mipmaps = 1;
                img.format = PIXELFORMAT_UNCOMPRESSED_R8G8B8A8;
                tex = LoadTextureFromImage(img);
                UnloadImage(img);
                if (tex.id)
                {
                    GenTextureMipmaps(&tex);
                    SetTextureFilter(tex, TEXTURE_FILTER_TRILINEAR);
                }
            }
            it = carTex_.emplace(key, tex).first;
        }
        return it->second.id ? &it->second : nullptr;
    }

    void App::drawCarPicture(Rectangle r, const std::string &label, bool frame, int blackGrey)
    {
        const ui::Theme &t = ui::theme();
        // A studio backdrop as the mock-ups, GT4's own picture (its usual colours) on it, kept in shape. A hidden black car:
        // GT4's 3D render of it in black (the launcher's own picture), else GT4's picture darkened to its black.
        ui_.round(r, 8, t.panel2);
        ui_.gradientV(R(r.x, r.y, r.width, r.height * 0.7f), ui::alpha(ui::mixColor(t.panel3, t.accent, 0.08f), 0.9f), ui::alpha(t.panel2, 0.0f));
        if (const Texture2D *tex = carTexture(label, blackGrey))
        {
            const float aspect = static_cast<float>(tex->width) / std::max(1, tex->height);
            float w = r.width - 16, h = w / aspect;
            if (h > r.height - 12)
            {
                h = r.height - 12;
                w = h * aspect;
            }
            const Rectangle dst = ui_.px(R(r.x + (r.width - w) * 0.5f, r.y + (r.height - h) * 0.5f, w, h));
            DrawTexturePro(*tex, Rectangle{0, 0, static_cast<float>(tex->width), static_cast<float>(tex->height)}, dst, Vector2{0, 0}, 0, WHITE);
        }
        else
        {
            // No picture under this car's name on the disc: a plain silhouette.
            const float cx = r.x + r.width * 0.5f, cy = r.y + r.height * 0.58f, k = std::min(r.width / 220.0f, r.height / 90.0f);
            ui_.icon(Icon::Garage, R(cx - 60 * k, cy - 30 * k, 120 * k, 60 * k), ui::alpha(t.faint, 0.7f));
        }
        if (frame)
            ui_.roundLine(r, 8, 1, ui::alpha(t.border, 0.9f));
    }

    void App::unloadFull()
    {
        for (auto &[k, tex] : carTex_)
            if (tex.id)
                UnloadTexture(tex);
        carTex_.clear();
        if (previewRaw_.id)
            UnloadRenderTexture(previewRaw_);
        previewRaw_ = RenderTexture2D{};
        if (race16_.id)
            UnloadTexture(race16_);
        race16_ = Texture2D{};
        if (hudClean_.id)
            UnloadTexture(hudClean_);
        if (hudHud_.id)
            UnloadTexture(hudHud_);
        if (hudCut_.id)
            UnloadTexture(hudCut_);
        hudClean_ = hudHud_ = hudCut_ = Texture2D{};
        hudTexKey_.clear();
        if (catLoading_)
            catFuture_.wait();
    }
}
