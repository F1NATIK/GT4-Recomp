#include "gt4stock_app.h"

#include "gt4stock_resources.h"
#include "gt4stock_util.h"

#include "rlgl.h"

#if defined(PS2X_INPUT_GLFW)
#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>
#endif

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <fstream>
#include <iostream>
#include <iterator>

#ifndef GT4STOCK_COMMIT
#define GT4STOCK_COMMIT "unknown"
#endif

// The guarded raylib of the GT4 build (ps2xRuntime/cmake/RaylibInitGuard.cmake): with a
// hidden window raylib never opens or reads a gamepad, wheel or pedal set. Its flag is
// referenced here so that the launcher cannot link against any other raylib.
extern "C" int ps2x_skip_hidden_swap;

namespace gt4stock
{
#if defined(GT4L_EDITION_FULL)
    // The Full edition (FULL-LAUNCHER-PLAN.md 1.1): the same sources built with GT4L_EDITION_FULL, as GT4RecompilationFull.exe.
    const char *const kVersion = "1.5.0";
    const char *const kProductName = "GT4 Recompilation";
    const char *const kEdition = "Full";
#else
    const char *const kVersion = "1.4.2";
    const char *const kProductName = "GT4 Recompilation";
    const char *const kEdition = "Classic"; // renamed from "Stock" (developer, PLAN.md 2026-10-05)
#endif

    namespace
    {
        constexpr uint64_t kGt4DiscBytes = 5314478080ull;
        std::string g_glRenderer;

        void traceLog(int level, const char *text, va_list args)
        {
            char line[1024];
            std::vsnprintf(line, sizeof(line), text, args);
            const std::string s = line;
            if (s.find("Renderer:") != std::string::npos)
                g_glRenderer = trim(s.substr(s.find("Renderer:") + 9));
            if (level >= LOG_WARNING)
                std::fprintf(stderr, "[raylib] %s\n", line);
        }
    }

    // ------------------------------------------------------------------ setup
    void App::locate()
    {
        Install &in = inst_;
        in.launcherDir = sys::exePath().parent_path();
        std::error_code ec;
        if (!opt_.root.empty())
            in.root = fs::absolute(opt_.root, ec);
        else
        {
            // The launcher's folder (a package), or a folder above it (the
            // development tree: recomp\out\gt4-msvc\ps2xRuntime\...).
            fs::path p = in.launcherDir;
            for (int i = 0; i < 7 && !p.empty(); ++i)
            {
                if (fs::exists(p / "game" / "CORE.elf", ec))
                {
                    in.root = p;
                    break;
                }
                if (p == p.parent_path())
                    break;
                p = p.parent_path();
            }
            if (in.root.empty())
                in.root = in.launcherDir;
        }
        in.gameDir = in.root / "game";
        in.coreElf = in.gameDir / "CORE.elf";
        in.coreFound = fs::is_regular_file(in.coreElf, ec);
        if (in.coreFound)
        {
            std::vector<uint8_t> core;
            if (readFile(in.coreElf, core))
            {
                in.coreBytes = core.size();
                in.coreCrc = crc32(core.data(), core.size());
            }
        }
        in.settingsFile = opt_.settings.empty() ? in.root / "gt4-start.env" : fs::absolute(opt_.settings, ec);
    }

    void App::loadAll()
    {
        std::error_code ec;
        settingsFileExisted_ = fs::exists(inst_.settingsFile, ec);
        if (settingsFileExisted_)
            loadSettings(inst_.settingsFile, st_, warnings_);
        // The GT4 program: the settings' choice, the stock package's, the development build's.
        std::vector<fs::path> candidates;
        if (!opt_.exe.empty())
            candidates.push_back(fs::absolute(opt_.exe, ec));
        if (!st_.exe.empty())
            candidates.push_back(pathFrom(st_.exe));
        candidates.push_back(inst_.root / L"Gran Turismo 4.exe");
        candidates.push_back(inst_.root / "GT4.exe");
        candidates.push_back(inst_.launcherDir / "GT4.exe");
#if defined(GT4L_EDITION_FULL)
        // The Full edition starts the development GT4 (all its options, Vulkan): the development tree's build first.
        candidates.push_back(inst_.root / "recomp" / "out" / "gt4-msvc" / "ps2xRuntime" / "GT4.exe");
        candidates.push_back(inst_.root / "recomp" / "out" / "gt4-msvc-stock" / "ps2xRuntime" / "GT4.exe");
#else
        candidates.push_back(inst_.root / "recomp" / "out" / "gt4-msvc-stock" / "ps2xRuntime" / "GT4.exe");
        candidates.push_back(inst_.root / "recomp" / "out" / "gt4-msvc" / "ps2xRuntime" / "GT4.exe");
#endif
        inst_.exe.clear();
        for (const fs::path &c : candidates)
            if (fs::is_regular_file(c, ec))
            {
                inst_.exe = c;
                break;
            }
        inst_.exeFound = !inst_.exe.empty();
        if (!inst_.exeFound)
            inst_.exe = inst_.root / L"Gran Turismo 4.exe";
        // Next to game\: the stock package (GT4.exe finds game\, disc\ and gt4-start.env itself).
        inst_.package = fs::equivalent(inst_.exe.parent_path(), inst_.root, ec) && !ec;
#if defined(GT4L_EDITION_FULL)
        inst_.layout = inst_.package ? "Package" : "development tree"; // (no edition name: developer 2026-10-20)
#else
        inst_.layout = inst_.package ? "Classic package" : "development tree";
#endif
        // The disc image: the settings', else disc\ or "Game iso\".
        if (st_.disc.empty() || !fs::exists(pathFrom(st_.disc), ec))
        {
            for (const fs::path &dir : {inst_.root / "disc", inst_.root / "Game iso"})
            {
                bool found = false;
                for (const auto &e : fs::directory_iterator(dir, fs::directory_options::skip_permission_denied, ec))
                {
                    const std::string ext = lower(pathText(e.path().extension()));
                    if (e.is_regular_file(ec) && (ext == ".iso" || ext == ".bin"))
                    {
                        if (st_.disc.empty())
                            st_.disc = pathText(e.path());
                        else
                            st_.disc = pathText(e.path());
                        found = true;
                        break;
                    }
                }
                if (found)
                    break;
            }
        }
        checkDisc();
        refreshCards();
    }

    void App::applyDemo()
    {
        WheelSettings &w = st_.wheel;
        w.enabled = true;
        w.device = "0eb7:0006";
        w.deviceName = "FANATEC Podium Wheel Base DD1";
        w.pedals = "0eb7:183b";
        w.pedalsName = "FANATEC ClubSport Pedals V3";
        w.gas.axis = "y";
        w.gas.calibrated = true;
        w.gas.released = 32767;
        w.gas.full = -32768;
        w.brake.axis = "z";
        w.brake.calibrated = false;
        w.ffb = false;
        w.ffbGain = kFfbFirstTest;
        st_.pad.sourceGuid = "78696e70757401000000000000000000";
        st_.eyetoy = EyeToy::Webcam; // the Brio, #2 in the developer's list (resolveCamera)
        st_.eyetoyCamera = "Logitech BRIO";
        st_.lastPlayed = "2026-10-04 18:22";
        st_.lastExit = 0;
        st_.lastMinutes = 35;
    }

    void App::refreshMonitors()
    {
        monitors_.clear();
        if (opt_.demo)
        {
            MonitorInfo a;
            a.index = 0;
            a.name = "DELL S2721DGF";
            a.w = 2560;
            a.h = 1440;
            a.hz = 165;
            a.workW = 2560;
            a.workH = 1392;
            a.primary = true;
            a.demo = true;
            MonitorInfo b = a;
            b.index = 1;
            b.name = "Built-in display";
            b.x = -2560;
            b.y = 0;
            b.w = 2560;
            b.h = 1600;
            b.hz = 240;
            b.workW = 2560;
            b.workH = 1552;
            b.primary = false;
            monitors_ = {a, b};
            return;
        }
        const int n = GetMonitorCount();
#if defined(PS2X_INPUT_GLFW)
        int count = 0;
        GLFWmonitor **list = glfwGetMonitors(&count);
#endif
        for (int i = 0; i < n; ++i)
        {
            MonitorInfo m;
            m.index = i;
            const char *name = GetMonitorName(i);
            m.name = name && *name ? name : strf("Monitor %d", i + 1);
            m.w = GetMonitorWidth(i);
            m.h = GetMonitorHeight(i);
            m.hz = GetMonitorRefreshRate(i);
            const Vector2 pos = GetMonitorPosition(i);
            m.x = static_cast<int>(pos.x);
            m.y = static_cast<int>(pos.y);
            m.workW = m.w;
            m.workH = m.h;
#if defined(PS2X_INPUT_GLFW)
            if (list && i < count)
            {
                int wx = 0, wy = 0, ww = 0, wh = 0;
                glfwGetMonitorWorkarea(list[i], &wx, &wy, &ww, &wh);
                if (ww > 0 && wh > 0)
                {
                    m.workW = ww;
                    m.workH = wh;
                }
            }
#endif
            m.primary = i == 0; // GLFW lists the main display first
            monitors_.push_back(m);
        }
    }

    void App::refreshCards()
    {
        cards_[0] = scanCard(cardDir(1), 1);
        cards_[1] = scanCard(cardDir(2), 2);
        for (int slot = 1; slot <= 2; ++slot)
            cardFiles_[slot - 1] = cardIsFile(slot) ? scanCardFile(cardFile(slot)) : CardFileInfo{};
        backups_ = listBackups(backupDir());
        photos_ = scanPhotos(usbRoot());
#if defined(GT4L_EDITION_FULL)
        scanExtraCarsInSaves(); // Saves warns about a save holding the ESSO (analysis/jp-cars)
#endif
        if (opt_.demo && backups_.empty())
        {
            // Demo rows for the screenshots (no file is made). Each row's date is the one in
            // its name (local time), so the pictures agree with themselves on every run.
            const struct
            {
                const char *name;
                uint64_t bytes;
            } demo[] = {
#if defined(GT4L_EDITION_FULL)
                {"GT4-mc0-2026-10-04_1912-48-before-edit.zip", 1153519}, // the Full edition's Garage backs up before a change
#endif
                {"GT4-mc0-2026-10-04_1830-12.zip", 1153433}, {"GT4-mc1-2026-10-04_1702-33.ps2", 8650752},
                {"GT4-mc0-2026-10-03_2215-40.zip", 1149021}, {"GT4-mc0-2026-10-03_1544-03-before-restore.zip", 1101004}};
            for (const auto &d : demo)
            {
                BackupInfo b;
                b.file = backupDir() / d.name;
                b.slot = d.name[6] - '0' + 1;
                b.cardFile = endsWith(d.name, ".ps2");
                b.bytes = d.bytes;
                const std::string n = d.name; // GT4-mc0-YYYY-MM-DD_HHMM-SS...
                std::tm tm{};
                tm.tm_year = std::stoi(n.substr(8, 4)) - 1900;
                tm.tm_mon = std::stoi(n.substr(13, 2)) - 1;
                tm.tm_mday = std::stoi(n.substr(16, 2));
                tm.tm_hour = std::stoi(n.substr(19, 2));
                tm.tm_min = std::stoi(n.substr(21, 2));
                tm.tm_sec = std::stoi(n.substr(24, 2));
                tm.tm_isdst = -1;
                b.time = std::chrono::clock_cast<fs::file_time_type::clock>(std::chrono::system_clock::from_time_t(std::mktime(&tm)));
                b.beforeRestore = std::string(d.name).find("before-restore") != std::string::npos;
                backups_.push_back(b);
            }
        }
    }

    void App::checkDisc()
    {
        DiscCheck d;
        d.path = st_.disc;
        std::error_code ec;
        const fs::path p = pathFrom(st_.disc);
        d.exists = !st_.disc.empty() && fs::is_regular_file(p, ec);
        if (d.exists)
        {
            d.bytes = fs::file_size(p, ec);
            d.sizeMatches = d.bytes == kGt4DiscBytes;
            std::ifstream f(p, std::ios::binary);
            char pvd[2048] = {};
            f.seekg(0x8000);
            if (f.read(pvd, sizeof(pvd)))
            {
                d.iso9660 = pvd[0] == 1 && std::memcmp(pvd + 1, "CD001", 5) == 0;
                d.volume = trim(std::string(pvd + 40, 32));
                d.volumeMatches = d.iso9660 && d.volume == "GRANTURISMO4";
            }
        }
        disc_ = d;
    }

    void App::loadPictures()
    {
        const int ids[3] = {GT4STOCK_IDR_RACE, GT4STOCK_IDR_MENU, GT4STOCK_IDR_TITLE};
        for (int i = 0; i < 3; ++i)
        {
            std::vector<uint8_t> png = sys::resource(ids[i]);
            Image img{};
            if (!png.empty())
                img = LoadImageFromMemory(".png", png.data(), static_cast<int>(png.size()));
            if (!img.data)
            {
                // No resource (a test build): a test card, 640x448 with one-line stripes.
                img = GenImageColor(640, 448, Color{30, 34, 44, 255});
                for (int y = 0; y < 448; ++y)
                    for (int x = 0; x < 640; ++x)
                    {
                        const bool stripe = x > 400 && (y & 1);
                        const unsigned char g = static_cast<unsigned char>(stripe ? 230 : 40 + (x * 180) / 640);
                        ImageDrawPixel(&img, x, y, Color{g, static_cast<unsigned char>(stripe ? 230 : g / 2), static_cast<unsigned char>(stripe ? 230 : 255 - g), 255});
                    }
            }
            ImageFormat(&img, PIXELFORMAT_UNCOMPRESSED_R8G8B8A8);
            pictureW_[i] = img.width;
            pictureH_[i] = img.height;
            pictures_[i] = LoadTextureFromImage(img);
            SetTextureFilter(pictures_[i], TEXTURE_FILTER_BILINEAR); // GT4.exe's picture with the lines off
            {
                // Bob on a still frame: the even field, the odd lines filled in from the lines above and below.
                Image bob = ImageCopy(img);
                auto *px = static_cast<unsigned char *>(bob.data);
                const auto *in = static_cast<const unsigned char *>(img.data);
                for (int y = 1; y < img.height; y += 2)
                    for (int x = 0; x < img.width; ++x)
                        for (int c = 0; c < 4; ++c)
                        {
                            const int above = in[((y - 1) * img.width + x) * 4 + c];
                            const int below = y + 1 < img.height ? in[((y + 1) * img.width + x) * 4 + c] : above;
                            px[(y * img.width + x) * 4 + c] = static_cast<unsigned char>((above + below + 1) / 2);
                        }
                picturesBob_[i] = LoadTextureFromImage(bob);
                UnloadImage(bob);
            }
            UnloadImage(img);
        }
    }

    void App::loadShirtDesign()
    {
        if (shirtTex_.id)
            UnloadTexture(shirtTex_);
        shirtTex_ = Texture2D{};
        shirt_ = ShirtDesign{};
        if (!disc_.exists)
        {
            shirtError_ = "No disc image: the design is read from it.";
            return;
        }
        if (!readShirtDesign(pathFrom(st_.disc), shirt_, shirtError_))
            return;
        // As printed: the pattern (2 x its 224 samples, nearest) on black with the print's border.
        int w = 0, h = 0;
        const std::vector<uint8_t> rgb = shirtPrintSheet(shirt_, shirt_.width * 2, w, h);
        Image img{};
        img.data = MemAlloc(static_cast<unsigned int>(rgb.size()));
        std::memcpy(img.data, rgb.data(), rgb.size());
        img.width = w;
        img.height = h;
        img.mipmaps = 1;
        img.format = PIXELFORMAT_UNCOMPRESSED_R8G8B8;
        shirtTex_ = LoadTextureFromImage(img);
        UnloadImage(img);
        if (shirtTex_.id)
        {
            GenTextureMipmaps(&shirtTex_);
            SetTextureFilter(shirtTex_, TEXTURE_FILTER_TRILINEAR);
        }
    }

    void App::saveShirtPrint()
    {
        if (hidden_ || shirt_.samples.empty())
            return;
        static const wchar_t filter[] = L"PNG picture (*.png)\0*.png\0"; // the array ends in the second \0
        const fs::path p = sys::browseForSaveFile(GetWindowHandle(), L"Save the GT shirt design to print", std::wstring(filter, std::size(filter)),
                                                  inst_.root / L"GT4 shirt design (print).png", L"png");
        if (p.empty())
            return;
        int w = 0, h = 0;
        const std::vector<uint8_t> rgb = shirtPrintSheet(shirt_, 1600, w, h); // gtshirt_target.py --print's size
        Image img{};
        img.data = MemAlloc(static_cast<unsigned int>(rgb.size()));
        std::memcpy(img.data, rgb.data(), rgb.size());
        img.width = w;
        img.height = h;
        img.mipmaps = 1;
        img.format = PIXELFORMAT_UNCOMPRESSED_R8G8B8;
        const bool ok = exportPng(img, p);
        UnloadImage(img);
        if (ok)
            setStatus("Saved " + pathText(p.filename()) + ": print it (keep the black border) or show it full screen on a phone.", ui::theme().ok);
        else
            setStatus("Could not save " + pathText(p) + ".", ui::theme().bad);
    }

    void App::markDirty()
    {
        dirty_ = true;
        dirtyAt_ = GetTime();
    }

    bool App::saveNow()
    {
        dirty_ = false;
        if (hidden_ && !opt_.saveSettings)
            return true; // the hidden test mode writes nothing but its pictures
        std::string error;
        if (saveSettings(inst_.settingsFile, st_, kVersion, error))
        {
            settingsFileExisted_ = true;
#if defined(GT4L_EDITION_FULL)
            writeSoundMap(); // Sound packs: the per-car map file GT4_SOUND_PACK_MAP names
            teleWrite();     // Telemetry screen: telemetry-layout.json beside the settings
#endif
            if (opt_.saveSettings)
                report("saved " + pathText(inst_.settingsFile));
            return true;
        }
        setStatus("Could not save the settings: " + error, ui::theme().bad);
        return false;
    }

    fs::path App::cardDir(int slot) const { return inst_.gameDir / (slot == 1 ? "mc0" : "mc1"); }
    fs::path App::backupDir() const { return inst_.gameDir / "backups"; }
    bool App::cardIsFile(int slot) const
    {
        const Settings::CardSlot &c = st_.cards[static_cast<size_t>(std::clamp(slot, 1, 2) - 1)];
        return c.file && !c.path.empty();
    }
    fs::path App::cardFile(int slot) const
    {
        const fs::path p = pathFrom(st_.cards[static_cast<size_t>(std::clamp(slot, 1, 2) - 1)].path);
        return p.is_absolute() ? p : inst_.gameDir / p; // as GT4.exe (ps2-card): relative to the CORE.elf folder
    }
    fs::path App::usbRoot() const
    {
        if (st_.usbRoot.empty())
            return inst_.gameDir / "usb0";
        // A relative GT4_USB_ROOT is taken from GT4.exe's folder, where GT4.exe runs (the package sets its working folder
        // there; the development layout starts it there), not from wherever the launcher was started.
        const fs::path p = pathFrom(st_.usbRoot);
        return p.is_absolute() ? p : inst_.root / p;
    }
    fs::path App::logFile() const { return inst_.root / "logs" / (inst_.package ? "gt4.log" : "launcher-play.log"); }

    sys::Launch App::playLaunch() const
    {
        sys::Launch l;
        l.exe = inst_.exe;
        l.cwd = inst_.root;
        if (!inst_.package)
        {
            // The development build: CORE.elf as its argument, the settings as variables.
            l.args.push_back(inst_.coreElf.wstring());
            l.log = logFile();
        }
        l.env = settingsEnvironment(st_, !inst_.package);
        // (1.4.2) A GT4 program that is not a Classic build draws on the CPU unless GT3_RENDERER names a GPU renderer
        // (STACK-REVIEW F2): OpenGL, as the Classic build always draws, unless the settings name one. In the package
        // layout the variable wins over gt4-start.env, which then has no GT3_RENDERER line (launchRenderer).
        if (const std::string r = launchRenderer(gt4Real_.read, gt4Real_.classic, st_); !r.empty())
            l.env.emplace_back("GT3_RENDERER", r);
#if defined(GT4L_EDITION_FULL)
        adjustFullLaunch(l); // the values this GT4 program understands (gt4full_app.cpp)
#endif
        return l;
    }

    void App::setStatus(const std::string &text, Color c)
    {
        status_ = text;
        statusColor_ = c;
        statusAt_ = GetTime();
    }

    void App::report(const std::string &line) { reportLines_.push_back(line); }

    // ------------------------------------------------------------------ helpers
    int App::defaultMonitor() const
    {
        // (1.4.2) The main display, as GT4.exe without GT4_MONITOR (no preference for one size).
        for (const MonitorInfo &m : monitors_)
            if (m.primary)
                return m.index;
        return monitors_.empty() ? -1 : monitors_.front().index;
    }

    const MonitorInfo *App::selectedMonitor() const
    {
        if (monitors_.empty())
            return nullptr;
        if (st_.monitorSet)
        {
            for (const MonitorInfo &m : monitors_)
                if (m.x == st_.monitorX && m.y == st_.monitorY && (st_.monitorName.empty() || m.name == st_.monitorName))
                    return &m;
            for (const MonitorInfo &m : monitors_)
                if (!st_.monitorName.empty() && m.name == st_.monitorName)
                    return &m;
            for (const MonitorInfo &m : monitors_)
                if (m.x == st_.monitorX && m.y == st_.monitorY)
                    return &m;
        }
        const int d = defaultMonitor();
        for (const MonitorInfo &m : monitors_)
            if (m.index == d)
                return &m;
        return &monitors_.front();
    }

    void App::pickMonitor(int index)
    {
        for (const MonitorInfo &m : monitors_)
            if (m.index == index)
            {
                st_.monitorSet = true;
                st_.monitorX = m.x;
                st_.monitorY = m.y;
                st_.monitorName = m.name;
                markDirty();
            }
    }

    scan::Placement App::gamePlacement(const MonitorInfo *m, int *winW, int *winH) const
    {
        int w = st_.windowW, h = st_.windowH;
        if (st_.fullscreen && m)
        {
            w = m->w;
            h = m->h;
        }
        if (winW)
            *winW = w;
        if (winH)
            *winH = h;
        return scan::fitPicture(static_cast<float>(w), static_cast<float>(h), scan::kLines, 4.0f / 3.0f, st_.fit);
    }

    std::vector<std::pair<int, int>> App::windowSizes(const MonitorInfo *m) const
    {
        std::vector<std::pair<int, int>> sizes = {{1195, 896}, {1280, 960}, {1792, 1344}, {1920, 1440}, {2389, 1792}, {2560, 1920}};
        std::vector<std::pair<int, int>> out;
        for (const auto &s : sizes)
            if (!m || (s.first <= m->workW && s.second + 40 <= m->workH))
                out.push_back(s);
        if (std::find(out.begin(), out.end(), std::make_pair(st_.windowW, st_.windowH)) == out.end())
            out.push_back({st_.windowW, st_.windowH});
        std::sort(out.begin(), out.end());
        return out;
    }

    const DeviceInfo *App::wheelDevice() const { return pickWheel(devices_.list(), st_.wheel.device); }

    const DeviceInfo *App::pedalDevice() const { return pickPedals(devices_.list(), st_.wheel.pedals, wheelDevice()); }

    void App::resolveCamera()
    {
        if (st_.eyetoy != EyeToy::Webcam)
            return;
        const CameraList &cl = cameras_.list();
        if (!cl.listed && cl.error.empty())
            return; // not read yet
        const std::vector<CameraInfo> &list = cl.devices;
        if (st_.eyetoyCamera.empty())
        {
            // A new choice, or a hand-written webcam:<n>: name the camera from the list (the
            // Logitech BRIO first; never an infrared camera).
            const int n = st_.eyetoyIndex;
            const int pick = n >= 0 && n < static_cast<int>(list.size()) && !list[static_cast<size_t>(n)].infrared ? n : defaultCamera(list);
            if (pick >= 0)
            {
                st_.eyetoyCamera = list[static_cast<size_t>(pick)].name;
                markDirty();
            }
        }
        const int found = findCamera(list, st_.eyetoyCamera);
        if (found != st_.eyetoyIndex)
        {
            st_.eyetoyIndex = found;
            markDirty();
        }
    }

    const CameraInfo *App::chosenCamera() const
    {
        const std::vector<CameraInfo> &list = cameras_.list().devices;
        if (st_.eyetoy != EyeToy::Webcam || st_.eyetoyIndex < 0 || st_.eyetoyIndex >= static_cast<int>(list.size()))
            return nullptr;
        return &list[static_cast<size_t>(st_.eyetoyIndex)];
    }

    std::string App::cameraSummary() const
    {
        switch (st_.eyetoy)
        {
        case EyeToy::Shirt: return "Built-in GT shirt picture (no camera needed)";
        case EyeToy::Image: return "Picture file: " + pathText(pathFrom(st_.eyetoyImage).filename());
        case EyeToy::Webcam:
        {
            const std::string name = st_.eyetoyCamera.empty() ? std::string("no camera chosen") : st_.eyetoyCamera;
            const CameraList &cl = cameras_.list();
            if (!cl.listed && cl.error.empty())
                return "Webcam: " + name;
            const CameraInfo *cam = chosenCamera();
            return "Webcam: " + name + (cam ? strf(" \xc2\xb7 Windows camera #%d", cam->index) : std::string(" (not connected: no camera)"));
        }
        default: return "Off (no camera, as a PS2 without one)";
        }
    }

    int App::padJoystick() const
    {
        // The joystick GT4.exe reads player 1's mapping from: slot 0's source resolved as the runtime does it (Automatic is
        // GLFW's joystick 0 when it is a mapped pad, never the first mapped one further down; an id shared by two connected
        // pads picks neither). The same rule as the Player 2 section (1.4 review), so Practise, that section and GT4 agree.
        if (hidden_ || devices_.demo())
            return -1;
        return resolvePlayer1Source(player1Sources(st_.pad).sources[0], pad2Inventory());
    }

#if defined(GT4L_EDITION_FULL)
    // The hidden Prepare shaders run is a GT4.exe too (the same Vulkan cache): nothing else starts or changes a save meanwhile.
    bool App::busy() const { return gameRunning_ || otherGameRunning_ || preparing_; }
#else
    bool App::busy() const { return gameRunning_ || otherGameRunning_; }
#endif

    // ------------------------------------------------------------------ actions
    void App::play()
    {
        const ui::Theme &t = ui::theme();
        if (hidden_)
            return;
        if (busy())
        {
#if defined(GT4L_EDITION_FULL)
            if (preparing_)
            {
                setStatus("Preparing shaders: wait a moment (GT4 runs hidden for a few seconds), then press Play.", t.warn);
                return;
            }
#endif
            setStatus(gameRunning_ ? "GT4 is already running." : "Another GT4 is running: close it first.", t.warn);
            return;
        }
        std::error_code ec;
        if (!fs::is_regular_file(inst_.exe, ec))
        {
            setStatus("The GT4 program was not found (" + pathText(inst_.exe) + ").", t.bad);
            return;
        }
        if (!inst_.coreFound)
        {
            setStatus("game\\CORE.elf was not found next to the GT4 program.", t.bad);
            return;
        }
        if (!disc_.exists)
        {
            setStatus("Choose the Gran Turismo 4 disc image first.", t.bad);
            return;
        }
        // The webcam's number now (webcam:<n> is a place in Windows' list, which changes
        // when a camera comes or goes): listed again, never opened.
        if (st_.eyetoy == EyeToy::Webcam)
        {
            cameras_.refreshNow();
            resolveCamera();
        }
        if (!saveNow() && inst_.package)
        {
            // The Classic package's GT4.exe reads gt4-start.env itself: an old file would start it with old settings (a
            // switch turned off could still be on there). The development layout passes every setting as a variable.
            setStatus("GT4 was not started: the settings could not be saved to " + shortPath(inst_.settingsFile, inst_.root) +
                          ", and GT4 reads them from there. Check that the file is not read-only or open in another program.",
                      t.bad);
            return;
        }
        const sys::Launch l = playLaunch();
        std::string error;
        if (!sys::start(l, game_, error))
        {
            setStatus(error, t.bad);
            return;
        }
        gameRunning_ = true;
#if defined(GT4L_EDITION_FULL)
        startMmSides(l); // GT4's own Multi Monitor Mode: the side screens' GT4 copies (none otherwise)
        const bool mmStarted = !mmSides_.empty();
#else
        const bool mmStarted = false;
#endif
        st_.lastPlayed = nowText();
        st_.lastMinutes = -1;
        devices_.setPaused(true);
        if (mmStarted)
        {
            // startMmSides said what to do
        }
        else if (st_.eyetoy == EyeToy::Webcam && !chosenCamera())
            setStatus("GT4 started without a camera: the webcam " + (st_.eyetoyCamera.empty() ? std::string("chosen") : st_.eyetoyCamera) +
                          " is not connected.",
                      t.warn);
        else
            setStatus("GT4 started. Closing its window is the PS2's power button (a clean quit).", t.ok);
        if (!hidden_)
            MinimizeWindow();
    }

    void App::stopGame()
    {
        if (!gameRunning_)
            return;
#if defined(GT4L_EDITION_FULL)
        closeMmSides(game_.closeSent); // GT4's own Multi Monitor Mode: the side screens' copies with it
#endif
        if (!game_.closeSent)
        {
            sys::requestClose(game_);
            setStatus("Asked GT4 to close (the PS2 power button)...", ui::theme().muted);
        }
        else
        {
            sys::terminate(game_);
            setStatus("GT4 was stopped.", ui::theme().warn);
        }
    }

    void App::pollGame()
    {
#if defined(GT4L_EDITION_FULL)
        // GT4's own Multi Monitor Mode: the side screens' copies end with the middle one (closed, then stopped if they hang on).
        if (!gameRunning_ && !mmSides_.empty())
            closeMmSides(sys::tickMs() - game_.startMs > 0 && std::any_of(mmSides_.begin(), mmSides_.end(), [](const sys::Process &p) {
                             return p.closeSent && sys::tickMs() - p.closeSentMs > 8000u;
                         }));
#endif
        if (!gameRunning_)
            return;
        int code = 0;
        if (sys::poll(game_, code))
        {
            gameRunning_ = false;
#if defined(GT4L_EDITION_FULL)
            closeMmSides(false);
#endif
            st_.lastExit = code;
            st_.lastMinutes = static_cast<int>((sys::tickMs() - game_.startMs) / 60000u);
            markDirty();
            devices_.setPaused(false);
            refreshCards();
            readCheatSave(true); // the Cheats page's day: GT4 may have saved
            if (detailsOpen_)
                reloadDetails(); // GT4 may have saved
#if defined(GT4L_EDITION_FULL)
            loadGarage(true); // the Garage and Game progress: GT4 may have saved
#endif
            setStatus(code == 0 ? "GT4 closed normally." : strf("GT4 ended with exit code %d: see the log.", code),
                      code == 0 ? ui::theme().ok : ui::theme().warn);
            if (!hidden_ && IsWindowMinimized())
                RestoreWindow();
        }
    }

    void App::backup(int slot)
    {
        if (hidden_)
            return;
        if (busy())
        {
            setStatus("Close GT4 before backing up its memory card.", ui::theme().warn);
            return;
        }
        fs::path zip;
        std::string error;
        if (cardIsFile(slot))
        {
            if (backupCardFile(cardFile(slot), slot, backupDir(), "", zip, error))
                setStatus("Backed up slot " + std::to_string(slot) + "'s card file to " + pathText(zip.filename()) + ".", ui::theme().ok);
            else
                setStatus("Backup failed: " + error, ui::theme().bad);
            refreshCards();
            return;
        }
        if (backupCard(cardDir(slot), slot, backupDir(), kVersion, "", zip, error))
            setStatus("Backed up slot " + std::to_string(slot) + " to " + pathText(zip.filename()) + ".", ui::theme().ok);
        else
            setStatus("Backup failed: " + error, ui::theme().bad);
        refreshCards();
    }

    void App::restore(const fs::path &zip, int slot)
    {
        if (hidden_)
            return;
        if (busy())
        {
            setStatus("Close GT4 before restoring a memory card.", ui::theme().warn);
            return;
        }
        std::string reportText, error;
        const bool fileBackup = lower(pathText(zip.extension())) == ".ps2";
        if (fileBackup != cardIsFile(slot))
        {
            setStatus(fileBackup ? strf("That backup is a card file: set slot %d to a card file first.", slot)
                                 : strf("That backup is a folder card: set slot %d to the folder card first.", slot),
                      ui::theme().warn);
            return;
        }
        if (fileBackup)
        {
            if (restoreCardFile(zip, cardFile(slot), slot, backupDir(), reportText, error))
                setStatus(reportText, ui::theme().ok);
            else
                setStatus("Restore failed: " + error, ui::theme().bad);
            refreshCards();
            return;
        }
        if (restoreCard(zip, cardDir(slot), slot, backupDir(), kVersion, reportText, error))
            setStatus(reportText, ui::theme().ok);
        else
            setStatus("Restore failed: " + error, ui::theme().bad);
        refreshCards();
    }

    // ------------------------------------------------------------------ run
    int App::run(const Options &o)
    {
        opt_ = o;
        hidden_ = !opt_.shots.empty() || !opt_.scanlineShot.empty() || opt_.dryRun || opt_.saveSettings;
#if defined(GT4L_EDITION_FULL)
        if (!opt_.editTest.empty() || opt_.buildTest)
            hidden_ = true; // --edit-test, --build-test: no window at all
#endif
        // Never a program, an Explorer window or a dialog from a test mode; --build-test's one program is gt4setup.exe,
        // a console tool started with no window (it starts nothing else).
        sys::setNoShell(hidden_ && !opt_.buildTest);
        locate();
        loadAll();
        if (opt_.demo)
            applyDemo();
        // What the GT4 program reads (its strings; read only, nothing is started). The demo pictures show a GT4.exe with
        // the cheats and player-2 runtimes; the test report also gives the real program's answer.
        gt4Real_ = inst_.exeFound ? exeFeatures(inst_.exe) : ExeFeatures{};
        if (!inst_.exeFound)
            gt4Real_.error = "no GT4 program";
        gt4_ = gt4Real_;
        if (opt_.demo)
        {
            gt4_ = ExeFeatures{};
            gt4_.read = gt4_.cheats = gt4_.card = gt4_.missionfix = gt4_.pad2 = true;
        }
#if defined(GT4L_EDITION_FULL)
        fullSetup(); // what the GT4 program reads for the Full pages, the release look (gt4full_app.cpp)
        if (opt_.page == "multiscreen")
            page_ = Page::Multiscreen;
        else if (opt_.page == "garage")
            page_ = Page::Garage;
        else if (opt_.page == "network")
            page_ = Page::Network;
        else if (opt_.page == "sound")
            page_ = Page::Sound;
        else if (opt_.page == "racehud")
            page_ = Page::RaceHud;
        else if (opt_.page == "telemetry")
            page_ = Page::Telemetry;
        else if (opt_.page == "progress")
        {
            page_ = Page::Saves;
            savesProgress_ = true;
        }
#endif
        if (opt_.page == "display")
            page_ = Page::Display;
        else if (opt_.page == "controllers")
            page_ = Page::Controllers;
        else if (opt_.page == "saves")
            page_ = Page::Saves;
        else if (opt_.page == "about")
            page_ = Page::About;
        else if (opt_.page == "cheats")
            page_ = Page::Cheats;
        else if (opt_.page == "how")
        {
            page_ = Page::About;
            aboutView_ = 1;
        }
        else if (opt_.page == "eyetoy" || opt_.page == "camera")
        {
            page_ = Page::Controllers;
            controllersTab_ = 2;
        }

        auto flushReport = [&](int rc) {
            for (const std::string &l : reportLines_)
                std::cout << l << "\n";
            std::cout.flush();
            if (!opt_.report.empty())
            {
                std::ofstream out(opt_.report, std::ios::binary | std::ios::trunc);
                for (const std::string &l : reportLines_)
                    out << l << "\r\n";
            }
            return rc;
        };
        if (opt_.dryRun)
            return flushReport(runDryRun());
#if defined(GT4L_EDITION_FULL)
        if (!opt_.editTest.empty())
            return flushReport(runEditTest()); // one Garage change on a test copy (gt4full_garageedit.cpp), no window
        if (opt_.buildTest)
            return flushReport(runBuildTest()); // Build GT4 into --root's build-kit (gt4full_build.cpp), no window
#endif
        SetTraceLogCallback(traceLog);
        SetTraceLogLevel(LOG_INFO);
        if (hidden_)
        {
            // Never shown, never focused: GLFW creates the window invisible and
            // the guarded raylib build then opens no gamepad (RaylibInitGuard.cmake).
            SetConfigFlags(FLAG_WINDOW_HIDDEN | FLAG_WINDOW_UNFOCUSED | FLAG_WINDOW_ALWAYS_RUN);
            InitWindow(640, 400, strf("%s (%s, test)", kProductName, kEdition).c_str());
        }
        else
        {
            SetConfigFlags(FLAG_WINDOW_RESIZABLE | FLAG_MSAA_4X_HINT | FLAG_VSYNC_HINT);
#if defined(GT4L_EDITION_FULL)
            InitWindow(1280, 800, kProductName); // "GT4 Recompilation" (no edition name: developer 2026-10-20)
#else
            InitWindow(1280, 800, strf("%s (%s)", kProductName, kEdition).c_str()); // "GT4 Recompilation (Classic)"
#endif
        }
        if (!IsWindowReady())
        {
            report("FAIL: no window / OpenGL");
            return flushReport(3);
        }
        if (hidden_ && sys::anyVisibleWindow())
        {
            CloseWindow();
            report("FAIL: the hidden window is visible");
            return flushReport(4);
        }
        glRenderer_ = g_glRenderer;
        SetTraceLogLevel(LOG_WARNING);
        SetExitKey(KEY_NULL);
        // The About page's "How it works" text (built in) and its "Is it a full recompilation?"
        // card, the graphics adapter GT4.exe runs on (About's Graphics row; read only, nothing is
        // started), and Save details' names from the disc, read once on a worker thread (none of
        // them needed for the scanline renders).
        {
            const std::vector<uint8_t> md = sys::resource(GT4STOCK_IDR_HOWTO);
#if defined(GT4L_EDITION_FULL)
            howDoc_ = parseDoc(fullHowText(std::string(md.begin(), md.end()))); // the edition's words switched (gt4full_app.cpp)
#else
            howDoc_ = parseDoc(std::string(md.begin(), md.end()));
#endif
            recomp_ = recompAnswer(howDoc_);
        }
        if (opt_.scanlineShot.empty() && !opt_.saveSettings)
            gpu_ = sys::gameGpu(inst_.exeFound ? inst_.exe : fs::path());
        if (opt_.scanlineShot.empty() && !opt_.saveSettings)
            startNames();
#if defined(GT4L_EDITION_FULL)
        if (opt_.scanlineShot.empty() && !opt_.saveSettings)
            startCatalogue(); // GT4's car data for the Garage (a worker thread, after the names)
#endif
        ui_.loadFonts();
        const bool shaderOk = preview_.init();
        loadPictures();
        if (opt_.scanlineShot.empty())
            loadShirtDesign();
        refreshMonitors();
        devices_.init(GetWindowHandle(), !hidden_, opt_.demo);
        devices_.update(0.0);
        // The webcams for the EyeToy option: listed (in the background in a normal session),
        // never opened. Not needed for the scanline renders.
        if (opt_.scanlineShot.empty())
        {
            cameras_.init(opt_.demo, !hidden_);
            resolveCamera();
        }
        // (1.4.2) No monitor is chosen for the player: until one is picked on the Display page, GT4_MONITOR stays unwritten
        // and GT4.exe opens on the main display (the pages show that one, selectedMonitor).
        report(strf("%s launcher %s, %s edition (%s); %s at %s; settings %s (%s)", kProductName, kVersion, kEdition, GT4STOCK_COMMIT,
                    inst_.layout.c_str(), pathText(inst_.root).c_str(), pathText(inst_.settingsFile).c_str(), settingsFileExisted_ ? "read" : "new"));
        report("OpenGL: " + (glRenderer_.empty() ? std::string("?") : glRenderer_) + "; scanline shader " + (shaderOk ? "compiled" : "FAILED"));
        if (opt_.scanlineShot.empty() && !opt_.saveSettings)
        {
            report(strf("graphics for GT4.exe: %s (%s)", gpu_.label().c_str(), gpu_.why.c_str()));
            report(strf("  %s: %s%s%s; Windows' graphics setting for it: %s", shortPath(inst_.exe, inst_.root).c_str(),
                        gpu_.exeRead ? "exports" : "not read", gpu_.nvExport ? " NvOptimusEnablement" : "",
                        gpu_.amdExport ? " AmdPowerXpressRequestHighPerformance" : (gpu_.nvExport || !gpu_.exeRead ? "" : " no GPU request"),
                        gpu_.windowsSetting < 0 ? "none" : gpu_.windowsSetting == 2 ? "high performance" : gpu_.windowsSetting == 1 ? "power saving" : "let Windows decide"));
            report("  adapters (DXGI, listed only): " + (gpu_.adapters.empty() ? std::string("none") : join(gpu_.adapters, "; ")) + "; high performance: " +
                   (gpu_.highPerformance.empty() ? std::string("-") : gpu_.highPerformance) + "; minimum power: " +
                   (gpu_.minimumPower.empty() ? std::string("-") : gpu_.minimumPower));
            report(strf("how it works: %zu sections; recompilation card: %s (%s = %s + %s), %zu points", howDoc_.sections.size(),
                        recomp_.found ? recomp_.headline.c_str() : "NOT FOUND", recomp_.total.c_str(), recomp_.translated.c_str(), recomp_.replaced.c_str(),
                        recomp_.points.size()));
        }
        for (const std::string &w : warnings_)
            report("settings: " + w);
        report("GT4 program reads (its strings, read only): " + gt4Real_.text() +
               (opt_.demo ? "; the demo pictures show a GT4.exe with the cheats and player-2 runtimes (7h shows one without)" : std::string()));

        int rc = 0;
        if (!opt_.saveSettings && !opt_.scanlineShot.empty())
            rc = runScanlineShot();
        else if (opt_.saveSettings)
            rc = saveNow() ? 0 : 1;
        else if (hidden_)
            rc = runTestShots();
        else
        {
            // Sized for the screen it opens on: 1280 x 800 dp at the monitor's
            // scale (at least the Windows display scale), centred.
            const int mon = GetCurrentMonitor();
            const Vector2 dpi = GetWindowScaleDPI();
            const Vector2 mpos = GetMonitorPosition(mon);
            const int mw = GetMonitorWidth(mon), mh = GetMonitorHeight(mon);
            const float scale = std::max(dpi.x, std::min(mw * 0.62f / 1280.0f, mh * 0.70f / 800.0f));
            const int w = std::min(mw - 40, static_cast<int>(std::lround(1280.0f * scale)));
            const int h = std::min(mh - 100, static_cast<int>(std::lround(800.0f * scale)));
            SetWindowMinSize(960, 600);
            SetWindowSize(w, h);
            SetWindowPosition(static_cast<int>(mpos.x) + (mw - w) / 2, static_cast<int>(mpos.y) + std::max(30, (mh - h) / 2 - 20));
            SetTargetFPS(60);
            while (!WindowShouldClose())
            {
                const double now = GetTime();
                pollGame();
                pollNames();
#if defined(GT4L_EDITION_FULL)
                pollCatalogue();
                pollPrepare();
                pollBuild();
#endif
                devices_.update(now);
                // The webcam list: every 4 s on the EyeToy tab, every 10 s on Play with a
                // webcam chosen (its "not connected" note); never while GT4 runs.
                cameras_.update();
                resolveCamera();
                const bool cameraTab = page_ == Page::Controllers && controllersTab_ == 2;
                const double every = cameraTab ? 4.0 : (page_ == Page::Play && st_.eyetoy == EyeToy::Webcam) ? 10.0 : 0.0;
                if (every > 0.0 && !busy() && now - lastCameraList_ > every)
                {
                    lastCameraList_ = now;
                    cameras_.refresh();
                }
                if (now - lastProcessCheck_ > 2.0)
                {
                    lastProcessCheck_ = now;
#if defined(GT4L_EDITION_FULL)
                    // The launcher's own hidden Prepare shaders run is not "another GT4" (busy() covers it).
                    otherGameRunning_ = !gameRunning_ && !preparing_ && sys::gameRunning({L"GT4.exe", L"Gran Turismo 4.exe"});
#else
                    otherGameRunning_ = !gameRunning_ && sys::gameRunning({L"GT4.exe", L"Gran Turismo 4.exe"});
#endif
                }
                if (now - lastRefresh_ > 5.0 && page_ == Page::Saves)
                {
                    lastRefresh_ = now;
                    refreshCards();
                }
                if (dirty_ && now - dirtyAt_ > 0.4)
                    saveNow();
                ui::Input in;
                in.enabled = true;
                in.mouse = GetMousePosition();
                in.pressed = IsMouseButtonPressed(MOUSE_BUTTON_LEFT);
                in.down = IsMouseButtonDown(MOUSE_BUTTON_LEFT);
                in.released = IsMouseButtonReleased(MOUSE_BUTTON_LEFT);
                in.rightPressed = IsMouseButtonPressed(MOUSE_BUTTON_RIGHT);
                in.wheel = GetMouseWheelMove();
                const int sw = GetScreenWidth(), sh = GetScreenHeight();
#if defined(GT4L_EDITION_FULL)
                prepareFullPreview(); // the Full Display page's preview targets (gt4full_display.cpp)
#else
                preparePreview();
#endif
                prepareCheats(sw, sh); // the sheet's preview, Save card / Save PDF / Print (outside drawing)
                BeginDrawing();
                ClearBackground(ui::theme().bg);
                frame(sw, sh, in);
                EndDrawing();
            }
            if (dirty_)
                saveNow();
            devices_.stopFfbTest();
        }
        devices_.shutdown();
        if (previewOn_.id)
            UnloadRenderTexture(previewOn_);
        if (previewOff_.id)
            UnloadRenderTexture(previewOff_);
        for (Texture2D &t : pictures_)
            if (t.id)
                UnloadTexture(t);
        for (Texture2D &t : picturesBob_)
            if (t.id)
                UnloadTexture(t);
        if (shirtTex_.id)
            UnloadTexture(shirtTex_);
        if (cheatThumb_.id)
            UnloadTexture(cheatThumb_);
#if defined(GT4L_EDITION_FULL)
        unloadFull();
#endif
        preview_.shutdown();
        ui_.unloadFonts();
        CloseWindow();
        return flushReport(rc);
    }

    // ------------------------------------------------------------------ test modes
    int App::runDryRun()
    {
        // As Play: a chosen webcam's number from Windows' list now (listed, never opened).
        if (st_.eyetoy == EyeToy::Webcam)
        {
            cameras_.init(opt_.demo, false);
            resolveCamera();
        }
        const sys::Launch l = playLaunch();
        report(strf("%s launcher %s (%s edition) dry run (%s)", kProductName, kVersion, kEdition, inst_.layout.c_str()));
        if (st_.eyetoy == EyeToy::Webcam)
        {
            const CameraInfo *cam = chosenCamera();
            report("EyeToy: webcam " + (st_.eyetoyCamera.empty() ? std::string("(none chosen)") : "'" + st_.eyetoyCamera + "'") +
                   (cam ? strf(" is #%d in Windows' list (%s)", cam->index, cam->idText().c_str()) : std::string(" is not connected: no camera")) +
                   (cameras_.list().demo ? " [demo list]" : ""));
        }
        else
            report(std::string("EyeToy: ") + eyeToyLabel(st_.eyetoy));
        report("settings file: " + pathText(inst_.settingsFile) + (settingsFileExisted_ ? "" : " (not written yet: built-in defaults)"));
        report("program: " + pathText(l.exe) + (inst_.exeFound ? "" : "  [NOT FOUND]"));
        report("command line: " + utf8(sys::commandLine(l)));
        report("working folder: " + pathText(l.cwd));
        report("log: " + (l.log.empty() ? std::string("written by GT4.exe itself (logs\\gt4.log)") : pathText(l.log)));
        report(std::string("disc: ") + st_.disc + (disc_.exists ? (disc_.sizeMatches && disc_.volumeMatches ? "  [GT4 USA, size and volume match]" : "  [found, not the expected image]") : "  [NOT FOUND]"));
        {
            // 1.4: a run that is not GT4 as shipped is never silent.
            const std::vector<std::string> on = cheatSwitchesOn(cheatsInEffect(st_.cheats, gt4_));
            const std::vector<std::string> set = cheatSwitchesOn(st_.cheats);
            report(on.empty() ? std::string("cheats: all off (GT4 as shipped)") : "cheats ON: " + join(on, ", ") + " (the Cheats page)");
            if (set.size() != on.size())
                report("cheats set on the page but not read by this GT4 program: " + std::to_string(set.size() - on.size()));
            report("player 2: " + (st_.pad2.mode != Pad2Settings::Mode::Off || st_.pad2.explicitChoice ? "GT4_PAD2=" + pad2Value(st_.pad2)
                                                                                                         : std::string("GT4_PAD2 not set (GT4.exe's default: off)")) +
                   (st_.pad2.mode == Pad2Settings::Mode::Chosen && !st_.pad2.name.empty() ? " (" + st_.pad2.name + ")" : std::string()) +
                   (gt4_.pad2 ? std::string() : std::string("; this GT4 program has no player 2")));
            report("GT4 program reads: " + gt4_.text());
            const std::string r = launchRenderer(gt4Real_.read, gt4Real_.classic, st_);
            report(gt4Real_.classic ? std::string("renderer: the Classic GT4 program draws with OpenGL itself")
                   : !r.empty()     ? "renderer: GT3_RENDERER=" + r + " added (a development GT4 program draws on the CPU without it)"
                   : gt4Real_.read  ? std::string("renderer: the settings name one (GT3_RENDERER kept as written)")
                                    : std::string("renderer: the GT4 program was not read: none added"));
        }
        report("removed from the inherited environment: every GT3_*, PS2X_*, GT4_*, TT_* variable");
        report("set for GT4.exe:");
        for (const auto &[k, v] : l.env)
            report("  " + k + "=" + v);
        for (const std::string &w : warnings_)
            report("settings warning: " + w);
#if defined(GT4L_EDITION_FULL)
        reportFullDryRun(l); // what this GT4 program reads, and the stored choices it never gets (gt4full_shots.cpp)
#endif
        return 0;
    }

    int App::runScanlineShot()
    {
        // For analysis/extras/stock-scanline/compare_shot.py: <prefix>-<picture>-src.png
        // and <prefix>-<picture>-<W>x<H>-s<SSS>-f000-p-1.png (the whole window GT4 would show).
        static const char *const names[3] = {"race", "menu", "title"};
        const int sizes[][2] = {{1280, 960}, {1920, 1440}, {2560, 1440}, {2560, 1600}};
        const int strengths[] = {0, 25, 50, 75, 100};
        int written = 0, failed = 0;
        for (int p = 0; p < 2; ++p)
        {
            const std::string stem = opt_.scanlineShot + "-" + names[p];
            Image src = LoadImageFromTexture(pictures_[p]);
            ImageFormat(&src, PIXELFORMAT_UNCOMPRESSED_R8G8B8);
            ExportImage(src, (stem + "-src.png").c_str()) ? ++written : ++failed;
            UnloadImage(src);
            for (const auto &sz : sizes)
                for (int s : strengths)
                {
                    const std::string path = strf("%s-%dx%d-s%03d-f000-p-1.png", stem.c_str(), sz[0], sz[1], s);
                    if (preview_.renderWindowToFile(pictures_[p], pictureW_[p], pictureH_[p], sz[0], sz[1], st_.fit, s / 100.0f, path.c_str()))
                        ++written;
                    else
                        ++failed;
                    if (sys::anyVisibleWindow())
                    {
                        report("FAIL: a window became visible");
                        return 4;
                    }
                }
        }
        report(strf("scanline shots: %d written, %d failed, fit %s", written, failed, fitKey(st_.fit)));
        return failed ? 1 : 0;
    }

    int App::runTestShots()
    {
#if defined(GT4L_EDITION_FULL)
        return runFullTestShots(); // the Full edition's pages and states (gt4full_shots.cpp)
#else
        struct Scene
        {
            const char *name;
            Page page;
            int tab;
            Modal modal;
            bool realDevices;
            bool running;
        };
        const Scene scenes[] = {
            {"1-play", Page::Play, 0, Modal::None, false, false},
            {"1b-play-running", Page::Play, 0, Modal::None, false, true},
            {"2-display", Page::Display, 0, Modal::None, false, false},
            {"2b-display-this-pc", Page::Display, 0, Modal::None, true, false},
            {"3-controllers", Page::Controllers, 0, Modal::None, false, false},
            {"3b-controllers-this-pc", Page::Controllers, 0, Modal::None, true, false},
            {"3c-controllers-player2", Page::Controllers, 0, Modal::None, false, false},
            {"3d-controllers-player2-list", Page::Controllers, 0, Modal::None, false, false},
            {"4-wheel", Page::Controllers, 1, Modal::None, false, false},
            {"4b-wheel-calibrate", Page::Controllers, 1, Modal::Calibrate, false, false},
            {"4c-wheel-buttons", Page::Controllers, 1, Modal::WheelButtons, false, false},
            {"4d-eyetoy-camera", Page::Controllers, 2, Modal::None, false, false},
            {"4e-eyetoy-camera-this-pc", Page::Controllers, 2, Modal::None, true, false},
            {"5-saves", Page::Saves, 0, Modal::None, false, false},
            {"5b-saves-restore", Page::Saves, 0, Modal::Restore, false, false},
            {"5c-saves-card-file", Page::Saves, 0, Modal::None, false, false},
            {"5d-save-details", Page::Saves, 0, Modal::None, false, false},
            {"5e-save-details-garage", Page::Saves, 0, Modal::None, false, false},
            {"5f-save-details-note", Page::Saves, 0, Modal::None, false, false},
            {"6-about", Page::About, 0, Modal::None, false, false},
            {"6b-how-it-works", Page::About, 0, Modal::None, false, false},
            {"6c-how-it-works-ps2", Page::About, 0, Modal::None, false, false},
            {"6d-how-it-works-code", Page::About, 0, Modal::None, false, false},
            {"6e-how-it-works-table", Page::About, 0, Modal::None, false, false},
            {"6f-how-it-works-end", Page::About, 0, Modal::None, false, false},
            // 1.4: the Cheats page (after About, so the earlier pictures keep their names)
            {"7-cheats", Page::Cheats, 0, Modal::None, false, false},
            {"7b-cheats-defaults", Page::Cheats, 0, Modal::None, false, false},
            {"7c-cheats-show-big-practise", Page::Cheats, 0, Modal::CheatBig, false, false},
            {"7d-cheats-hidden-menu", Page::Cheats, 0, Modal::None, false, false},
            {"7e-cheats-save-print", Page::Cheats, 0, Modal::CheatSave, false, false},
            {"7f-cheats-mission-fix", Page::Cheats, 0, Modal::None, false, false},
            {"7g-play-cheats-on", Page::Play, 0, Modal::None, false, false}, // the Play page's chip while a cheat switch is on
            // 1.4 review: a GT4 program without the cheats and player-2 runtimes (today's recomp): the switches greyed
            {"7h-cheats-older-gt4", Page::Cheats, 0, Modal::None, false, false},
            // The picture's scaling and interlacing (Display page): the menu picture with the defaults (smooth, blend),
            // the race picture with nearest and bob and the Interlacing list open, and lines off with sharp and off.
            {"8-display-picture-menu", Page::Display, 0, Modal::None, false, false},
            {"8b-display-picture-list", Page::Display, 0, Modal::None, false, false},
            {"8c-display-picture-lines-off", Page::Display, 0, Modal::None, false, false},
        };
        // Save details: the names from the disc first (the worker thread started at launch), then
        // the save copies (--save-copy, read only): the first as slot 2's card file, the second as
        // a copy of its own.
        pollNames(true);
        report(names_ ? strf("names from the disc: %zu cars, %zu colour groups, %zu strings, %zu text groups, %zu parts, read in %.0f ms on a worker thread",
                             names_->carNameCount(), names_->colourGroupCount(), names_->stringCount(), names_->textCategoryCount(), names_->partCount(), namesMs_)
                      : "names from the disc: none (" + namesError_ + ")");
        std::error_code ec;
        fs::create_directories(opt_.shots, ec);
        int written = 0, failed = 0, visibleChecks = 0;
        bool cheatFilesDone = false;
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
                if (sc.realDevices && !opt_.demo)
                    continue; // the plain run already shows this PC's devices
                page_ = sc.page;
                controllersTab_ = sc.tab;
                modal_ = sc.modal;
                gameRunning_ = sc.running;
                game_.startMs = sys::tickMs() - 12u * 60000u;
                const Settings::CardSlot keepSlot = st_.cards[1];
                if (std::string(sc.name) == "5c-saves-card-file")
                {
                    // Slot 2 as a .ps2 card file: the ps2-card work's card with the developer's save
                    // imported (recomp\out\extras\ps2-card\cards, only read), else a new one.
                    const fs::path made = inst_.root / "recomp" / "out" / "extras" / "ps2-card" / "cards" / "Mcd001-gt4.ps2";
                    std::error_code ec2;
                    st_.cards[1].file = true;
                    st_.cards[1].path = fs::exists(made, ec2) ? pathText(made) : std::string("Mcd002.ps2");
                    refreshCards();
                }
                const std::string sn = sc.name;
                const bool detailsScene = startsWith(sn, "5d-") || startsWith(sn, "5e-") || startsWith(sn, "5f-");
                if (detailsScene)
                {
                    // Save details of a save copy (read only): 5d / 5e the first copy (the 40-day
                    // save) as slot 2's card file, 5f the second (the recomp save) as a copy.
                    const size_t which = startsWith(sn, "5f-") ? 1 : 0;
                    if (which >= opt_.saveCopies.size())
                    {
                        report("skipped " + sn + ": no --save-copy");
                        continue;
                    }
                    const fs::path copy = fs::absolute(opt_.saveCopies[which], ec);
                    if (which == 0 && fs::is_regular_file(copy, ec))
                    {
                        st_.cards[1].file = true;
                        st_.cards[1].path = pathText(copy);
                        refreshCards();
                        openDetails(2);
                    }
                    else
                        openDetailsPath(copy, "Test copy of a save \xc2\xb7 " + shortPath(copy, inst_.root));
                    if (startsWith(sn, "5d-"))
                    {
                        for (const DetailCar &c : details_.cars)
                            if (c.riding)
                                expandedCar_ = c.slot;
                    }
                    else if (startsWith(sn, "5e-"))
                    {
                        // Another car with its setup, scrolled into view, and the technical notes.
                        showTechnical_ = true;
                        for (size_t i = 0; i < details_.cars.size(); ++i)
                            if (details_.cars[i].fullRecord && !details_.cars[i].riding && details_.cars[i].slot >= 10)
                            {
                                expandedCar_ = details_.cars[i].slot;
                                ui_.scrollTo("details-garage", 58.0f * static_cast<float>(i) - 70.0f);
                                break;
                            }
                    }
                }
                const bool howScene = startsWith(sn, "6b-") || startsWith(sn, "6c-") || startsWith(sn, "6d-") || startsWith(sn, "6e-") || startsWith(sn, "6f-");
                if (howScene)
                {
                    // The page at its top, at the PS2 diagram (its section), at the code illustration
                    // (section 2), at the first table (the modules, with its heading) and at its end.
                    aboutView_ = 1;
                    howJump_ = -1;
                    ui_.scrollTo("how", startsWith(sn, "6f-") ? 1e6f : 0.0f);
                    howAnchor_ = startsWith(sn, "6c-")   ? "section:1"
                                 : startsWith(sn, "6d-") ? "section:2"
                                 : startsWith(sn, "6e-") ? "table:1"
                                                         : "";
                }
                if (sc.modal == Modal::Restore)
                {
                    restoreSlot_ = 1;
                    restoreZip_ = backups_.empty() ? backupDir() / "GT4-mc0-2026-10-04_1830-12.zip" : backups_.front().file;
                }
                if (sc.modal == Modal::Calibrate)
                {
                    calibratePedal_ = 1;
                    calibrateHaveRest_ = true;
                    calibrateRest_ = 32767;
                    calibrateFar_ = -30140;
                }
                // 1.4: the Cheats page's states (mock-ups 1, 1b, 2, 3, 4) and the Mission Hall fix; player 2 chosen.
                const CheatSettings keepCheats = st_.cheats;
                const Pad2Settings keepPad2 = st_.pad2;
                const int keepSel = cheatSel_;
                const ExeFeatures keepGt4 = gt4_;
                const bool cheatScene = sc.page == Page::Cheats || sn == "7g-play-cheats-on";
                if (cheatScene)
                {
                    CheatSettings &c = st_.cheats;
                    c = CheatSettings{};
                    readCheatSave(true);
                    const std::vector<std::string> ticked{"credits", "licence", "event", "options"};
                    if (sn != "7b-cheats-defaults")
                    {
                        c.noday = true;
                        c.card = true;
                        c.ticked = ticked;
                    }
                    cheatSel_ = sn == "7b-cheats-defaults" ? 1 : sn == "7d-cheats-hidden-menu" ? 5 : (sn == "7f-cheats-mission-fix" || sn == "7h-cheats-older-gt4") ? 4 : 0;
                    c.secret = sn == "7d-cheats-hidden-menu";
                    c.missionfix = sn == "7f-cheats-mission-fix" || sn == "7h-cheats-older-gt4";
                    if (sn == "7h-cheats-older-gt4")
                    {
                        // The same switches set, with a GT4 program that reads none of them (its strings have no GT4_CHEATS,
                        // GT4_CHEAT_CARD, missionfix or GT4_PAD2): greyed, with a plain line each.
                        gt4_ = ExeFeatures{};
                        gt4_.read = true;
                    }
                    cheatPractise_ = sn == "7c-cheats-show-big-practise";
                    cheatTestProgress_ = 6;
                    cheatThumbKey_.clear();
                    ui_.resetScroll("cheat-body");
                }
                const std::string keepPreview = st_.preview;
                const PictureFilter keepFilter = st_.filter;
                const Deinterlace keepDeinterlace = st_.deinterlace;
                const bool keepLines = st_.linesOn;
                const bool pictureScene = startsWith(sn, "8");
                if (pictureScene)
                {
                    st_.preview = sn == "8-display-picture-menu" ? "menu" : "race";
                    st_.filter = sn == "8b-display-picture-list" ? PictureFilter::Nearest : sn == "8c-display-picture-lines-off" ? PictureFilter::Sharp : PictureFilter::Smooth;
                    st_.deinterlace = sn == "8b-display-picture-list" ? Deinterlace::Bob : sn == "8c-display-picture-lines-off" ? Deinterlace::Weave : Deinterlace::Blend;
                    st_.linesOn = sn != "8c-display-picture-lines-off";
                    if (sn == "8b-display-picture-list")
                        ui_.openPopup("deinterlace");
                }
                if (sn == "3c-controllers-player2" || sn == "3d-controllers-player2-list")
                {
                    // Player 2 chosen by name (the demo DualSense); 3d with its list open.
                    st_.pad2.mode = Pad2Settings::Mode::Chosen;
                    st_.pad2.guid = sdlGuidFromVidPid(0x054c, 0x0ce6);
                    st_.pad2.name = "DualSense Wireless Controller";
                    if (sn == "3d-controllers-player2-list")
                        ui_.openPopup("p2-pad");
                }
                const bool cameraScene = sc.page == Page::Controllers && sc.tab == 2;
                const bool dirtyBefore = dirty_; // this PC's lists may move the webcam's number: not a change of the demo settings
                if (sc.realDevices)
                {
                    // This PC as it is: its game controllers (enumeration only) and monitors,
                    // and on the EyeToy tab its webcams (listed, none opened).
                    devices_.init(GetWindowHandle(), false, false);
                    devices_.update(1000.0);
                    opt_.demo = false;
                    refreshMonitors();
                    opt_.demo = true;
                    if (cameraScene)
                    {
                        cameras_.init(false, false);
                        resolveCamera();
                        ui_.openPopup("eyetoy-cam"); // the picture shows this PC's webcam list, open
                    }
                    // Before the picture too: this PC's lists can re-mark the settings (a webcam missing
                    // from the list gives GT4_EYETOY=absent), which a real session saves a moment later,
                    // so the sidebar shows "Settings saved", not "Saving settings...".
                    dirty_ = dirtyBefore;
                }
                ui::Input none;
                for (int pass = 0; pass < 2; ++pass)
                {
                    preparePreview();
                    prepareCheats(w, h);
                    BeginTextureMode(rt);
                    ClearBackground(ui::theme().bg);
                    frame(w, h, none);
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
                if (pictureScene)
                {
                    report(strf("%s: preview %s, GT4_PICTURE_FILTER=%s, GT4_DEINTERLACE=%s, lines %s", sn.c_str(), st_.preview.c_str(), pictureFilterKey(st_.filter),
                                deinterlaceKey(st_.deinterlace), st_.linesOn ? "on" : "off"));
                    st_.preview = keepPreview;
                    st_.filter = keepFilter;
                    st_.deinterlace = keepDeinterlace;
                    st_.linesOn = keepLines;
                }
                if (sn == "3c-controllers-player2" || sn == "3-controllers" || sn == "3b-controllers-this-pc" || sn == "7b-cheats-defaults")
                {
                    std::string why;
                    const int p2 = pad2Pick(&why);
                    report(strf("%s: GT4_PAD2=%s; player 2 now (the player-2 rule): %s", sn.c_str(), pad2Value(st_.pad2).c_str(),
                                p2 >= 0 ? pad2Inventory()[static_cast<size_t>(p2)].name.c_str() : ("none (" + why + ")").c_str()));
                }
                if (cheatScene)
                {
                    const gt4cheats::Code &code = gt4cheats::kCodes[static_cast<size_t>(cheatSel_)];
                    std::string statuses;
                    for (const gt4cheats::Code &c : gt4cheats::kCodes)
                        statuses += strf("%s%s: %s", statuses.empty() ? "" : "; ", std::string(c.id).c_str(),
                                         cheatStatusText(cheatStatus(c, cheatsInEffect(st_.cheats, gt4_), cheatSaveState_ == 1 ? cheatElapsed_ : -1,
                                                                     player2Ready(nullptr))));
                    const std::vector<std::string> on = cheatSwitchesOn(st_.cheats);
                    const std::string saveText = cheatSaveState_ == 1   ? strf("Day %lld (elapsed %lld)", gameDay(cheatElapsed_), cheatElapsed_)
                                                 : cheatSaveState_ == 2 ? std::string("not readable")
                                                                        : std::string("none (a new game starts at Day 1)");
                    const std::vector<std::string> acted = cheatSwitchesOn(cheatsInEffect(st_.cheats, gt4_));
                    report(strf("%s: slot-1 save %s; card '%s'; switches: %s (GT4 acts on: %s); ticked: %s", sn.c_str(), saveText.c_str(),
                                std::string(code.name).c_str(), on.empty() ? "all off" : join(on, ", ").c_str(), acted.empty() ? "none" : join(acted, ", ").c_str(),
                                st_.cheats.ticked.empty() ? "none" : join(st_.cheats.ticked, ",").c_str()));
                    if (sc.page == Page::Cheats)
                        report("  the card at this size: " + cheatLevelText_);
                    report("  statuses: " + statuses);
                    if (sn == "7e-cheats-save-print")
                    {
                        report(strf("  sheet preview: %d page(s), %dx%d", cheatThumbPages_, cheatThumb_.width, cheatThumb_.height));
                        if (!cheatFilesDone && !opt_.cheatFiles.empty())
                        {
                            // T9: Save pictures and Save PDF (A4, then Letter), as the buttons do, into --cheat-files
                            // (never the GT4 folder); the print window is never opened in this mode (sys::setNoShell).
                            cheatFilesDone = true;
                            lastCheatFiles_.clear();
                            cheatJob_ = CheatJob::Pictures;
                            prepareCheats(w, h);
                            cheatJob_ = CheatJob::Pdf;
                            prepareCheats(w, h);
                            st_.cheats.letter = true;
                            cheatJob_ = CheatJob::Pdf;
                            prepareCheats(w, h);
                            st_.cheats.letter = false;
                            for (int pi = 0; pi < 2; ++pi)
                            {
                                // The sheet's first page as a picture too (white paper, and the ink saver off), for the README.
                                std::vector<Image> pages = renderCheatSheet(tickedCodes(st_.cheats), false, pi == 0);
                                if (!pages.empty() && pages[0].data)
                                {
                                    const fs::path pp = opt_.cheatFiles / (pi == 0 ? "sheet-a4-page1.png" : "sheet-a4-dark-page1.png");
                                    if (exportPng(pages[0], pp))
                                        report("cheats: sheet picture " + pathText(pp) + strf(" (%dx%d)", pages[0].width, pages[0].height));
                                }
                                for (Image &pg : pages)
                                    UnloadImage(pg);
                            }
                            // 1.4 review: all six cards, the Mission Hall fix off (A4) and on (Letter): more than one page, the
                            // "Does:" line of each block, and the fixed Mission Hall card's own file name.
                            const CheatSettings keepSix = st_.cheats;
                            st_.cheats.ticked = {"credits", "licence", "test", "event", "mission", "options"};
                            for (int fix = 0; fix < 2; ++fix)
                            {
                                st_.cheats.missionfix = fix == 1;
                                st_.cheats.letter = fix == 1;
                                cheatJob_ = CheatJob::Pdf;
                                prepareCheats(w, h);
                                const CheatSettings ce = cheatsInEffect(st_.cheats, gt4_);
                                for (const gt4cheats::Code *c : tickedCodes(st_.cheats))
                                    report(strf("cheats: six-card sheet (%s, fix %s): %s: %s", fix ? "Letter" : "A4", fix ? "on" : "off",
                                                std::string(c->id).c_str(), cheatSheetDoes(*c, ce).c_str()));
                                std::vector<Image> pages = renderCheatSheet(tickedCodes(st_.cheats), st_.cheats.letter, true);
                                for (size_t pi = 0; pi < pages.size(); ++pi)
                                {
                                    const fs::path pp = opt_.cheatFiles / strf("sheet-six-%s-fix%s-page%zu.png", fix ? "letter" : "a4", fix ? "on" : "off", pi + 1);
                                    if (exportPng(pages[pi], pp))
                                        report("cheats: sheet picture " + pathText(pp) + strf(" (%dx%d)", pages[pi].width, pages[pi].height));
                                }
                                report(strf("cheats: six-card sheet (%s): %zu page(s)", fix ? "Letter" : "A4", pages.size()));
                                for (Image &pg : pages)
                                    UnloadImage(pg);
                            }
                            cheatSel_ = 4; // the Mission Hall card, fixed: its own file name
                            cheatJob_ = CheatJob::Card;
                            prepareCheats(w, h);
                            // Print..., as the button does it (the real sheet's pages): the test mode refuses before Windows' print
                            // window could open (sys::setNoShell).
                            status_.clear();
                            cheatJob_ = CheatJob::Print;
                            prepareCheats(w, h);
                            const bool refused = status_ == "Printing is off in the test mode.";
                            report(strf("cheats: print (the real sheet, %zu card(s)): %s (\"%s\")", tickedCodes(st_.cheats).size(),
                                        refused ? "not opened" : "UNEXPECTED", status_.c_str()));
                            if (!refused)
                                ++failed;
                            st_.cheats = keepSix;
                            cheatSel_ = 0;
                            status_.clear(); // the next pictures show the settings line, as before
                            for (const fs::path &f : lastCheatFiles_)
                            {
                                std::error_code e2;
                                report(strf("cheats: file %s, %s bytes", pathText(f.filename()).c_str(), groupedNumber(fs::file_size(f, e2)).c_str()));
                            }
                            if (sys::anyVisibleWindow())
                            {
                                report("FAIL: a launcher window became visible");
                                UnloadRenderTexture(rt);
                                return 4;
                            }
                        }
                    }
                    st_.cheats = keepCheats;
                    cheatSel_ = keepSel;
                    cheatPractise_ = false;
                    modal_ = Modal::None;
                    gt4_ = keepGt4;
                }
                st_.pad2 = keepPad2;
                if (detailsScene)
                {
                    const SaveDetails &d = details_;
                    report(strf("%s: %s, %s; player '%s', Day %lld on GT4's counter (elapsed %lld), %s Cr., %zu cars, %zu problem(s), %zu technical note(s); "
                                "read in %.0f ms, names %s",
                                sn.c_str(), detailsTitle_.c_str(), loadStateText(d.loadState).c_str(), d.player.c_str(), static_cast<long long>(d.gameDay),
                                static_cast<long long>(d.days),
                                groupedNumber(d.credits).c_str(), d.cars.size(), d.problems.size(), d.technical.size(), detailsMs_,
                                detailsNames_ ? "from the disc" : "none"));
                    for (const DetailProblem &p : d.problems)
                        report("  note: " + p.title + ": " + p.text);
                    st_.cards[1] = keepSlot;
                    refreshCards();
                    savesView_ = 0;
                    detailsOpen_ = false;
                    expandedCar_ = -1;
                    showTechnical_ = false;
                }
                if (howScene)
                {
                    const DocLayout &L = howLayout_;
                    size_t texts = 0, diagramLabels = 0;
                    for (const DocItem &it : L.items)
                        if (it.kind == DocItem::Kind::Text)
                            (it.diagram ? diagramLabels : texts) += 1;
                    report(strf("%s: %zu sections, %.0f dp of text in a %.0f dp column (%zu text runs, %zu diagram labels, diagram at %.0f dp), "
                                "scrolled to %.0f dp",
                                sn.c_str(), howDoc_.sections.size(), L.height, L.width, texts, diagramLabels, L.anchor("diagram"), ui_.scrollOffset("how")));
                    aboutView_ = 0;
                }
                if (sn == "6-about")
                    report(strf("6-about: Graphics row '%s'", gpu_.label().c_str()));
                if (std::string(sc.name) == "5c-saves-card-file")
                {
                    report(strf("slot 2 card file %s: %s, %d KB free, GT4 save %s", st_.cards[1].path.c_str(),
                                cardFiles_[1].card ? "a PS2 card" : (cardFiles_[1].exists ? cardFiles_[1].problem.c_str() : "not made yet"),
                                cardFiles_[1].freeKB, cardFiles_[1].gt4Save ? ("'" + cardFiles_[1].gt4.title + "'").c_str() : "none"));
                    st_.cards[1] = keepSlot;
                    refreshCards();
                }
                if (cameraScene)
                {
                    const CameraList &cl = cameras_.list();
                    const CameraInfo *cam = chosenCamera();
                    report(strf("%s: webcams%s: %zu%s; chosen '%s' -> GT4_EYETOY=%s", sc.name, cl.demo ? " (demo)" : " (this PC, listed, none opened)",
                                cl.devices.size(), cl.error.empty() ? "" : (" (" + cl.error + ")").c_str(), st_.eyetoyCamera.c_str(),
                                eyeToyValue(st_).c_str()));
                    for (const CameraInfo &c : cl.devices)
                        report(strf("  #%d  %s  %s%s%s", c.index, c.name.c_str(), c.idText().c_str(), c.infrared ? "  (infrared: never offered)" : "",
                                    cam == &c ? "  <- chosen" : ""));
                    const CameraPrivacy &p = cameras_.privacy();
                    report(strf("  camera privacy (registry, read only): device %s, user %s, desktop apps %s", p.device.empty() ? "-" : p.device.c_str(),
                                p.user.empty() ? "-" : p.user.c_str(), p.desktopApps.empty() ? "-" : p.desktopApps.c_str()));
                }
                if (sc.realDevices)
                {
                    refreshMonitors();
                    report(strf("this PC's game controllers (enumeration only): %zu", devices_.list().size()));
                    for (const DeviceInfo &d : devices_.list())
                        report(strf("  %s  %s  %s%s", d.idText().c_str(), d.name.c_str(), d.kindText(), d.forceFeedback ? ", force feedback" : ""));
                    devices_.init(GetWindowHandle(), false, opt_.demo);
                    devices_.update(2000.0);
                    if (cameraScene)
                    {
                        cameras_.init(opt_.demo, false);
                        resolveCamera();
                    }
                    dirty_ = dirtyBefore;
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
        gameRunning_ = false;
        modal_ = Modal::None;
        report(strf("monitors: %zu%s", monitors_.size(), opt_.demo ? " (demo)" : ""));
        for (const MonitorInfo &m : monitors_)
            report(strf("  %d  %s  %dx%d @ %d Hz at %d,%d%s", m.index + 1, m.name.c_str(), m.w, m.h, m.hz, m.x, m.y, m.primary ? "  (main)" : ""));
        report(strf("devices opened by the launcher: %d (enumeration only in this mode); cameras: listed %d time(s), never opened (the launcher has no "
                    "camera-opening code)",
                    devices_.openedCount(), cameras_.listings()));
        report(strf("raylib: the guarded build (flag ps2x_skip_hidden_swap at %p): a hidden window never sets GLFW's joysticks up, "
                    "and the launcher calls no joystick function in this mode", static_cast<void *>(&ps2x_skip_hidden_swap)));
        report(strf("visible-window checks: %d, none visible", visibleChecks));
        report(strf("test shots: %d written, %d failed", written, failed));
        return failed ? 1 : 0;
#endif
    }
}
