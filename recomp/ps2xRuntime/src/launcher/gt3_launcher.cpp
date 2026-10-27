// GT3 launcher (v1): a native window to choose the settings the game supports
// today and start it. The settings reach the game as environment variables,
// exactly as Play-GT3-latest.cmd sets them, and are kept in gt3-launcher.ini
// next to that script. Later features (texture packs, mods, angled triple
// screens) are listed on the Roadmap page only: nothing here claims
// support the runtime does not have.

#include "raylib.h"
#define RAYGUI_IMPLEMENTATION
#include "raygui.h"

#include "gt3_launcher_win32.h"
#include "gt3_qr.h"
#include "../lib/gt3_tower_layout.h"
#if defined(PS2X_INPUT_GLFW)
#include <GLFW/glfw3.h>
#endif

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

namespace
{
    using namespace gt3launcher;

    // Our own HUD's widgets (gt3_hud.h in the game); the layout editor's
    // radar box comes after them (kHudRadar).
    constexpr int kHudCount = 13, kHudRadar = kHudCount;

    struct Settings
    {
        std::string iso, memcard, profile;
        int renderer = 0;    // 0 OpenGL (GPU), 1 software (CPU), 2 Vulkan (GPU, experimental)
        int filter = 0;      // 0 sharp, 1 bilinear, 2 nearest, 3 AMD FSR 1, 4 NVIDIA Image Scaling, 5 TV (CRT)
        int sharpening = 50; // FSR 1 / NIS sharpening, 0..100
        int aspect = 2;      // 0 4:3, 1 square pixels; wider through GT3's 16:9 mode: 2 16:9 (the default: GT3's own widescreen), 3 21:9, 4 triple 16:9 (48:9)
        int window = 0;      // 0 window fitted to the screen, 1 1280x960, 2 borderless full screen, 3 spanning all monitors
        int deinterlace = 0; // 0 adaptive, 1 weave, 2 bob, 3 double, 4 progressive (fields drawn as whole frames)
        int scale = 0;       // internal resolution: 0 native, 1 2x, 2 3x, 3 4x
        bool fpsOverlay = true;
        int frameGen = 0; // frame generation: 0 off, 1 90 fps, 2 120 fps (GT3 itself stays at 60)
        int carDetail = 0; // cars' detail levels: 0 GT3's own, 1 full detail twice as far, 2 four times, 3 always full (GT3_CAR_DETAIL)
        int rimDetail = -1; // wheels' (rims, tyres) detail, as carDetail (GT3_RIM_DETAIL); -1 until read: starts as carDetail
        int wheelBlur = 0;  // wheel spin blur: 0 GT3's own, 1 off (the still rim while turning; GT3_WHEEL_BLUR=0)
        int mirrorDetail = 0; // rear-view mirror: 0 GT3's own (low detail), 1 the main view's detail (GT3_MIRROR_DETAIL=full)
        bool textureDump = false; // save every texture GT3 uses as a PNG (GT3_TEXTURE_DUMP=<root>\textures\dump)
        bool textureReplace = true; // draw pictures from textures\replace instead (GT3_TEXTURE_REPLACE)
        // i.LINK Battle over the network: 0 off (one console), 1 host, 2 join.
        int linkMode = 0;
        std::string linkHost, linkName; // join: "address[:port]", empty = find a host on the LAN
        int linkPort = 13940;
        // HUD units, as GT3 numbers them (US game: mph, HP, ft.lb).
        int unitsSpeed = 1;  // 0 km/h and km, 1 mph and miles
        int unitsPower = 1;  // 0 PS, 1 HP, 2 BHP, 3 CV, 4 kW
        int unitsTorque = 1; // 0 kgf.m, 1 ft.lb, 2 Nm
        bool phoneMirror = false; // the rear-view mirror served to a phone (GT3_PHONE_VIEW, port 8093)
        int hudAnchor = 1; // wide races: HUD 1 at the screen's sides (triple: the middle screen's), 0 in the middle 4:3
        // GT3's own race HUD parts hidden (bits: 1 position and map, 2 lap
        // times, 4 gauges and lights, 8 rear-view mirror; GT3_HUD_HIDE).
        int hudHide = 0;
        // Grid-start lights in place of GT3's "3 2 1 START" (GT3_START_LIGHTS; gt3_start_lights.h):
        // lights come on one a second, then all go out a random time after the last.
        bool startLights = false;
        int startLightsCount = 5; // 3..5
        int startWaitMin = 2;     // lights out: shortest / longest wait after the last light, tenths of a second
        int startWaitMax = 30;
        int startAiReaction = 2;  // AI cars' reaction to the lights going out: 0 instant (GT3's), 1 quick, 2 normal, 3 slow
        // End-of-race report (GT3_RACE_REPORT; gt3_race_report.h): results, lap times, sectors, positions, your race.
        bool raceReport = false;
        bool raceReportSave = true; // each report also saved as a web page in race-reports
        // AI cars 1-5 drive what the Race Lab sent them (GT3_AI_LINES=<root>\racelab-data\ai-lines; gt3_ai_pilot.h)
        bool raceLabAi = false;
        // Drivers' names (GT3's AI cars have none): the player's is linkName (also the LAN name);
        // the AI cars' a comma-separated list (GT3_DRIVER_NAMES; default: the GT Race Lab's roster).
        std::string driverNames = "Kenji Arata, Sofia Lindqvist, Dale Brennan, Amelie Duret, Tomas Varga, Rhys Calder, Haruto Moriyama, Nadia Okafor";
        int reportNames = 0;   // the report starts on: 0 drivers' names, 1 cars'
        int towerNames = 0;    // the timing tower: 0 cars, 1 drivers, 2 switching every 5 s (GT3_HUD_TOWER_NAMES)
        bool towerTimeSwitch = false; // the tower's time column switching between best and last lap (GT3_HUD_TOWER_TIME_SWITCH)
        bool vibration = true; // controller vibration (GT3_VIBRATION=0 off)
        bool pedalsTriggers = true; // the triggers are GT3's analog pedals only (GT3_PEDALS=triggers)
        // The picture behind the HUD layout editor and the radar preview: empty =
        // the latest race's (hud-background.png, saved by the game each race),
        // else a file (a kept race picture in hud-pictures, or the user's own).
        std::string hudPicture;
        // Our own HUD widgets (gt3_hud.h in the game): on, middle (percent of
        // the HUD's width / the height), height (percent), box opacity.
        // Also a style (gt3_hud.h: rev counter, lap times, maps, tower,
        // splits) and the zoomed map's range (metres across).
        struct HudBox
        {
            bool on;
            int x, y, size, alpha;
            int style = 0, range = 300;
            int columns = gt3hud::kTowerDefaultColumns, timeMode = gt3hud::TimeBest; // timing tower (gt3_tower_layout.h)
        };
        std::array<HudBox, kHudCount> hudBoxes{{{false, 88, 84, 11, 60},
                                                {false, 50, 93, 6, 60},
                                                {false, 77, 84, 11, 60},
                                                {false, 50, 7, 4, 50},
                                                {false, 9, 9, 9, 60},
                                                {false, 9, 19, 6, 60},
                                                {false, 86, 12, 14, 60},
                                                {false, 12, 46, 26, 45},
                                                {false, 10, 55, 30, 55},
                                                {false, 50, 17, 7, 60},
                                                {false, 88, 50, 26, 45},
                                                {false, 50, 86, 5, 60},
                                                {false, 50, 97, 4, 60}}};
        // Proximity radar (GT3_RADAR*, gt3_radar.h): 0 off, 1 when cars are
        // near, 2 always; its middle in percent of the picture's middle 4:3
        // (x) and height (y); size percent of the height; range metres;
        // opacities percent; colours 0xRRGGBB.
        int radarMode = 1;
        int radarX = 86, radarY = 52, radarSize = 24, radarRange = 25, radarOpacity = 90, radarBgOpacity = 35;
        int radarPlayer = 0x78DCFF, radarCars = 0xFFE65A, radarNear = 0xFF8C00, radarBg = 0x000000;
        // Race cameras (GT3_CAMERA_*, gt3_camera.h): the views in the R1 cycle
        // (bits: bumper, bonnet, roof, close chase, chase, far chase), the view
        // a race starts in (-1 GT3's own), the right-stick look-around (0 off,
        // 1 left-right only, 2 full 360).
        int cameraViews = 0x3F, cameraStart = -1, cameraLook = 0;
        int padStyle = 0; // the Controls page's names: 0 Xbox-style, 1 PlayStation (DualShock 2 adapter, DualShock 4 / 5)
        // Steering wheel (Controls page, Wheel & pedals): the rotation set on the
        // wheel base (its whole axis, degrees lock to lock), the lock wanted for
        // full steering in GT3, and GT3's steering dead zone made up for.
        int wheelRotation = 900, wheelLock = 360;
        bool wheelDeadzone = true;
        // The phone dashboard (GT3_PHONE_*, gt3_phone_view.h): panels 0 mirror,
        // 1 position, 2 timing, 3 map, 4 radar, in this order, each 0 off,
        // 1 small, 2 medium, 3 large; arrangement 0 auto (by the phone's
        // orientation), 1 one column, 2 two columns; the radar's range, metres.
        std::array<int, 5> phoneOrder{0, 1, 2, 3, 4}, phoneSizes{3, 1, 2, 2, 2};
        int phoneArrange = 0, phoneRadarRange = 30;
        bool phoneMapFallback = true; // no course data found: the map drawn from the cars' paths
        // The launcher's window: its size when not full screen (0: the
        // design size), and full screen (borderless) or not.
        int launcherWidth = 0, launcherHeight = 0;
        bool launcherFullscreen = false;
        // Triple screens (Picture shape "Triple"; the Triple screens page):
        // the screens' shape (0 16:9, 1 16:10, 2 21:9) and size (inches), the
        // gap between two screens' pictures (mm, both bezels; 0 no bezel
        // correction), the field of view (0 GT3's own, 1 matched to the eyes'
        // distance from the middle screen, cm), the side screens (0 flat,
        // 1 angled - not built yet) and their angle (degrees).
        int tripleMonitor = 0, tripleInches = 27, tripleBezelMm = 0, tripleFov = 0, tripleSeatCm = 70, tripleSides = 0, tripleAngle = 45;
        int tripleSideDetail = 0; // angled side screens: 0 full detail, 1 the rear-view mirror's (faster)
    };
    // The launcher's pages are laid out for this size; the window shows them
    // scaled to fit (resizable, F11 full screen).
    constexpr int kDesignW = 1180, kDesignH = 812;

    // The Controls page's PS2 controls (as PCSX2 maps a pad): the 16 buttons,
    // then each stick's directions (left / right, up / down for lx, ly, rx, ry).
    constexpr int kPadTargets = 24;
    const char *const kPadLabel[kPadTargets] = {"D-pad Up", "D-pad Right", "D-pad Down", "D-pad Left", "Triangle", "Circle",
                                                "Cross", "Square", "L1", "R1", "L2", "R2", "Select", "Start", "L3 (left stick press)",
                                                "R3 (right stick press)", "Left stick Left", "Left stick Right", "Left stick Up",
                                                "Left stick Down", "Right stick Left", "Right stick Right", "Right stick Up",
                                                "Right stick Down"};
    // The mapping file's names (gt3-input.profile, ps2_input_profile.cpp).
    const char *const kPadProfileName[16] = {"up", "right", "down", "left", "triangle", "circle", "cross", "square",
                                             "l1", "r1", "l2", "r2", "select", "start", "l3", "r3"};
    const char *const kPadStick[4] = {"lx", "ly", "rx", "ry"};
    // Analog pressures (ps2_input_profile.cpp pressureNames): a pedal's target.
    const char *const kPressureNames[12] = {"right", "left", "up", "down", "triangle", "circle", "cross", "square", "l1", "r1", "l2", "r2"};
    const char *const kPressureLabels[12] = {"Right", "Left", "Up", "Down", "Triangle", "Circle", "Cross", "Square", "L1", "R1", "L2", "R2"};
    // Launcher pages, in sidebar order.
    enum Page
    {
        kPagePlay,
        kPageGraphics,
        kPageHud,
        kPageHudLayout,
        kPageRadar,
        kPageCamera,
        kPagePhone,
        kPageTriple,
        kPageControls,
        kPageMemoryCard,
        kPageLink,
        kPagePaths,
        kPageRoadmap,
        kPageDetail,
        kPageStartLights,
        kPageRaceReport,
        kPageRaceLab,
        kPageCount
    };
    const char *const kPhonePanelNames[] = {"Rear-view mirror", "Position", "Timing", "Track map", "Radar"};
    const char *const kCameraKeys[] = {"bumper", "bonnet", "roof", "close", "chase", "far"};
    const char *const kTripleMonitorValues[] = {"16:9", "16:10", "21:9"};
    // The HUD widgets, as the game names them (gt3_hud.h), their titles and box shapes.
    const char *const kHudKeys[kHudCount] = {"speed", "rpm", "gear", "shift", "position", "lap", "times", "map", "tower", "splits", "zoommap", "lights", "odo"};
    const char *const kHudNames[kHudCount] = {"Speed", "Rev counter", "Gear", "Shift lights", "Position", "Lap", "Lap times", "Track map",
                                              "Timing tower", "Split times", "Zoomed map", "Warning lights (ASM, TCS)", "Odometer"};
    // Styles per widget (as gt3hud::kStyleCount) and their names.
    constexpr int kHudStyleCount[kHudCount] = {1, 5, 1, 1, 1, 1, 3, 2, 6, 2, 2, 1, 1};
    const std::vector<std::vector<const char *>> kHudStyleNames = {
        {}, {"Bar", "Dial", "Arc", "LED strip", "Digital"}, {}, {}, {}, {}, {"Lap, last, best", "Live difference", "One line"},
        {"North up", "Turns with car"}, {"Gap to leader", "Interval", "Last lap", "Table: car names", "Table: Car 1..6", "Custom"}, {"Sector times", "Differences"}, {"Turns with car", "North up"}, {}, {}};
    // A widget's box shape (width / height) in a style (gt3hud::aspect).
    float hudAspect(int widget, int style, int columns = gt3hud::kTowerDefaultColumns)
    {
        static const float rpm[5] = {5.0f, 1.2f, 1.45f, 6.0f, 2.3f}, times[3] = {2.4f, 2.4f, 4.2f};
        static const float fixed[kHudCount] = {1.9f, 0.0f, 0.9f, 6.0f, 1.8f, 2.2f, 0.0f, 1.0f, 1.3f, 2.8f, 1.0f, 3.8f, 3.0f};
        if (widget == 1)
            return rpm[std::clamp(style, 0, 4)];
        if (widget == 6)
            return times[std::clamp(style, 0, 2)];
        if (widget == 8 && style == 5)
            return gt3hud::towerCustomAspect(columns); // the custom tower: its columns
        if (widget == 8 && style >= 3)
            return 2.3f; // the timing tower's tables (as the phone's) are wide
        return fixed[std::clamp(widget, 0, kHudCount - 1)];
    }
    const char *const kCameraLookValues[] = {"off", "sideways", "full"};
    const char *const kLinkModeValues[] = {"", "host", "join"};
    // The AI cars' reaction to the start lights going out (GT3_START_AI_REACTION: shortest,longest seconds).
    const char *const kStartReactionValues[] = {"", "0.08,0.22", "0.15,0.40", "0.25,0.65"};
    // GT3_HUD_TOWER_NAMES: the timing tower's names.
    const char *const kTowerNameValues[] = {"", "drivers", "switch"};
    // GT3_START_WAIT: the lights' shortest and longest wait after the last one comes on, in seconds.
    std::string startWaitValue(const Settings &s)
    {
        const int lo = std::min(s.startWaitMin, s.startWaitMax), hi = std::max(s.startWaitMin, s.startWaitMax);
        char out[32];
        std::snprintf(out, sizeof(out), "%.1f,%.1f", lo / 10.0, hi / 10.0);
        return out;
    }
    const char *const kSpeedValues[] = {"kmh", "mph"};
    const char *const kPowerValues[] = {"ps", "hp", "bhp", "cv", "kw"};
    const char *const kTorqueValues[] = {"kgfm", "ftlb", "nm"};

    const char *const kFilterValues[] = {"sharp", "bilinear", "nearest", "fsr", "nis", "crt"};
    const char *const kDeinterlaceValues[] = {"", "weave", "bob", "double", ""};
    const char *const kAspectValues[] = {"", "", "16:9", "21:9", "48:9"};
    const float kAspectRatios[] = {4.0f / 3.0f, 640.0f / 448.0f, 16.0f / 9.0f, 21.0f / 9.0f, 48.0f / 9.0f};
    const char *const kScaleValues[] = {"", "2", "3", "4", "6", "8"};
    const char *const kFrameGenValues[] = {"", "90", "120"};

    // Colours (dark theme after the launcher concept).
    const Color kBackground{15, 17, 21, 255};
    const Color kPanel{23, 26, 33, 255};
    const Color kPanelRaised{31, 35, 44, 255};
    const Color kBorder{48, 54, 68, 255};
    const Color kText{230, 233, 239, 255};
    const Color kMuted{146, 154, 170, 255};
    const Color kAccent{62, 126, 255, 255};
    const Color kAccentDark{36, 84, 190, 255};
    const Color kGood{86, 200, 128, 255};
    const Color kWarn{240, 180, 60, 255};
    const Color kBad{235, 90, 80, 255};

    std::string trim(const std::string &s)
    {
        const size_t a = s.find_first_not_of(" \t\r\n");
        if (a == std::string::npos)
            return {};
        const size_t b = s.find_last_not_of(" \t\r\n");
        return s.substr(a, b - a + 1);
    }

    std::string parentDirectory(const std::string &path)
    {
        const size_t slash = path.find_last_of("\\/");
        return slash == std::string::npos ? std::string{} : path.substr(0, slash);
    }

    // The project folder: the first parent of the launcher with the game's
    // CORE.elf in windows-diagnostic\game.
    std::string findRoot()
    {
        std::string dir = executableDirectory();
        for (int i = 0; i < 8 && !dir.empty(); ++i)
        {
            if (fileExists(dir + "\\windows-diagnostic\\game\\CORE.elf"))
                return dir;
            dir = parentDirectory(dir);
        }
        return executableDirectory();
    }

    // The files the game reads from the disc's root besides the disc image
    // itself: CORE.elf (GT3's program, unpacked from the disc's CORE.GT3: its
    // data and version check; the code is recompiled into ps2EntryRunner.exe)
    // and the IOP driver modules (*.IRX: never run - the native drivers check
    // and read them). SYSTEM.CNF, SCUS_971.02 and IOPRP224.IMG are not needed
    // (tested 2026-09-30).
    // gameFilesPresent: the game folder has them all.
    bool gameFilesPresent(const std::string &gameDir)
    {
        static const char *kNeeded[] = {"CORE.elf",    "ILINK.IRX",  "LIBPDI.IRX",  "LIBSD.IRX",   "MCMAN.IRX",   "MCSERV.IRX",
                                        "MEM2MB.IRX",  "MEMCLEAR.IRX", "MTAPMAN.IRX", "PADMAN.IRX",  "PDI1394.IRX", "PDICDVD.IRX",
                                        "PDISPU2.IRX", "PDISTR.IRX", "PDIUSB.IRX",  "SIO2MAN.IRX", "USBD.IRX"};
        for (const char *name : kNeeded)
            if (!fileExists(gameDir + "\\" + name))
                return false;
        return true;
    }

    // CORE.elf from the disc's CORE.GT3, as gt3-tools/core_to_elf.py builds it:
    // the container (u16 0x0101, u32 unpacked size, raw deflate) holds two
    // 128-byte RSA fields, then one image segment (entry 0x100008, base
    // 0x100000); the segment's bytes are written unchanged behind an ELF
    // header with the text / data / bss split, GP and section table the
    // script derived for this exact US image. (The script also checks the
    // RSA signature and CORE.GT3's SHA-256; the game verifies CORE.elf at start.)
    bool buildCoreElf(const std::vector<uint8_t> &packed, std::vector<uint8_t> &elf, std::string &error)
    {
        const auto rd16 = [](const uint8_t *p) { return static_cast<uint32_t>(p[0] | (p[1] << 8)); };
        const auto rd32 = [](const uint8_t *p) { return static_cast<uint32_t>(p[0] | (p[1] << 8) | (p[2] << 16) | (static_cast<uint32_t>(p[3]) << 24)); };
        if (packed.size() < 16 || rd16(packed.data()) != 0x0101u)
        {
            error = "CORE.GT3 has an unknown header";
            return false;
        }
        const uint32_t expected = rd32(packed.data() + 2);
        int unpackedSize = 0;
        unsigned char *raw = DecompressData(packed.data() + 6, static_cast<int>(packed.size() - 6), &unpackedSize);
        if (!raw || expected == 0u || static_cast<uint32_t>(unpackedSize) != expected)
        {
            if (raw)
                MemFree(raw);
            error = "CORE.GT3 could not be unpacked";
            return false;
        }
        std::vector<uint8_t> data(raw, raw + unpackedSize);
        MemFree(raw);
        size_t pos = 0;
        for (int k = 0; k < 2; ++k)
        {
            if (pos + 2 > data.size() || rd16(data.data() + pos) != 128u)
            {
                error = "CORE.GT3: unexpected signature fields";
                return false;
            }
            pos += 2 + 128;
        }
        if (pos + 16 > data.size() || rd32(data.data() + pos) != 1u || rd32(data.data() + pos + 4) != 0x100008u)
        {
            error = "CORE.GT3 is not the GT3 USA image";
            return false;
        }
        const uint32_t entry = 0x100008u, base = rd32(data.data() + pos + 8), size = rd32(data.data() + pos + 12);
        pos += 16;
        if (base != 0x100000u || pos + size != data.size() || size < 8u + 26u * 4u)
        {
            error = "CORE.GT3: unexpected segment";
            return false;
        }
        const uint8_t *payload = data.data() + pos;
        // The startup instructions that fix BSS and GP (as the script checks).
        const std::pair<int, uint32_t> ops[] = {{0, 0x3c020035u}, {1, 0x3c03004bu}, {2, 0x24423200u}, {3, 0x24632c0cu}, {11, 0x3c040035u}, {16, 0x248479f0u}};
        for (const auto &[i, op] : ops)
            if (rd32(payload + 8 + i * 4) != op)
            {
                error = "CORE.GT3: unrecognised start-up code";
                return false;
            }
        const uint32_t bssStart = 0x353200u, bssEnd = 0x4b2c0cu, gp = 0x3579f0u;
        if (base + size > bssStart)
        {
            error = "CORE.GT3: image overlaps BSS";
            return false;
        }
        const uint32_t memoryEnd = (bssEnd + 31u) & ~15u, textSize = 0x28e780u - base;
        static const char names[] = "\0.text\0.data\0.bss\0.reginfo\0.shstrtab"; // + the array's own terminating zero
        const uint32_t namesLen = sizeof(names);
        const uint32_t dataOffset = 0x1000u, regOffset = (dataOffset + size + 3u) & ~3u, namesOffset = regOffset + 24u,
                       shOffset = (namesOffset + namesLen + 3u) & ~3u;
        elf.assign(shOffset + 6u * 40u, 0u);
        const auto wr16 = [&](uint32_t at, uint32_t v) { elf[at] = static_cast<uint8_t>(v); elf[at + 1] = static_cast<uint8_t>(v >> 8); };
        const auto wr32 = [&](uint32_t at, uint32_t v) {
            for (int b = 0; b < 4; ++b)
                elf[at + b] = static_cast<uint8_t>(v >> (8 * b));
        };
        const uint8_t ident[7] = {0x7f, 'E', 'L', 'F', 1, 1, 1};
        std::memcpy(elf.data(), ident, sizeof(ident));
        wr16(16, 2);                 // e_type EXEC
        wr16(18, 8);                 // e_machine MIPS
        wr32(20, 1);                 // e_version
        wr32(24, entry);
        wr32(28, 52);                // e_phoff
        wr32(32, shOffset);          // e_shoff
        wr32(36, 0x20924001u);       // e_flags
        wr16(40, 52);                // e_ehsize
        wr16(42, 32);                // e_phentsize
        wr16(44, 2);                 // e_phnum
        wr16(46, 40);                // e_shentsize
        wr16(48, 6);                 // e_shnum
        wr16(50, 5);                 // e_shstrndx
        const uint32_t ph0[8] = {1, dataOffset, base, base, textSize, textSize, 5, 0x1000};
        const uint32_t ph1[8] = {1, dataOffset + textSize, base + textSize, base + textSize, size - textSize, memoryEnd - base - textSize, 6, 0x1000};
        for (int k = 0; k < 8; ++k)
        {
            wr32(52 + k * 4, ph0[k]);
            wr32(84 + k * 4, ph1[k]);
        }
        std::memcpy(elf.data() + dataOffset, payload, size);
        wr32(regOffset + 20, gp); // .reginfo: five zero words, then GP
        std::memcpy(elf.data() + namesOffset, names, namesLen);
        const uint32_t sections[5][10] = {
            {1, 1, 6, base, dataOffset, textSize, 0, 0, 64, 0},
            {7, 1, 3, base + textSize, dataOffset + textSize, size - textSize, 0, 0, 64, 0},
            {13, 8, 3, bssStart, dataOffset + size, memoryEnd - bssStart, 0, 0, 16, 0},
            {18, 0x70000006u, 0, 0, regOffset, 24, 0, 0, 4, 24},
            {27, 3, 0, 0, namesOffset, namesLen, 0, 0, 1, 0},
        };
        for (int s = 0; s < 5; ++s)
            for (int k = 0; k < 10; ++k)
                wr32(shOffset + (s + 1) * 40 + k * 4, sections[s][k]);
        return true;
    }

    // Copies CORE.elf and the *.IRX files out of the disc image's root
    // directory (ISO 9660; .iso with 2048-byte sectors, or a raw .bin with
    // 2352-byte Mode 1 / Mode 2 sectors) into gameDir. Returns how many files
    // were written; error says why when none.
    int extractGameFiles(const std::string &image, const std::string &gameDir, std::string &error)
    {
        std::ifstream in(image, std::ios::binary);
        if (!in)
        {
            error = "the disc image cannot be opened";
            return 0;
        }
        // The sector layout: where sector 16 (the volume descriptor) says CD001.
        struct Layout
        {
            uint32_t sectorSize, dataOffset;
        };
        const Layout layouts[] = {{2048, 0}, {2352, 16}, {2352, 24}};
        Layout layout{0, 0};
        uint8_t pvd[2048];
        for (const Layout &l : layouts)
        {
            in.clear();
            in.seekg(static_cast<std::streamoff>(16) * l.sectorSize + l.dataOffset);
            if (in.read(reinterpret_cast<char *>(pvd), sizeof(pvd)) && pvd[0] == 1 && std::memcmp(pvd + 1, "CD001", 5) == 0)
            {
                layout = l;
                break;
            }
        }
        if (layout.sectorSize == 0)
        {
            error = "not a disc image the launcher can read (no ISO 9660 volume)";
            return 0;
        }
        const auto u32 = [](const uint8_t *p) { return static_cast<uint32_t>(p[0] | (p[1] << 8) | (p[2] << 16) | (static_cast<uint32_t>(p[3]) << 24)); };
        // Reads `bytes` of the file data starting at sector `lba`.
        const auto readExtent = [&](uint32_t lba, uint32_t bytes, std::vector<uint8_t> &out) {
            out.resize(bytes);
            for (uint32_t done = 0, sector = lba; done < bytes; ++sector)
            {
                const uint32_t chunk = std::min<uint32_t>(2048u, bytes - done);
                in.clear();
                in.seekg(static_cast<std::streamoff>(sector) * layout.sectorSize + layout.dataOffset);
                if (!in.read(reinterpret_cast<char *>(out.data() + done), chunk))
                    return false;
                done += chunk;
            }
            return true;
        };
        const uint8_t *root = pvd + 156;
        std::vector<uint8_t> dir;
        if (!readExtent(u32(root + 2), u32(root + 10), dir))
        {
            error = "the disc image's root folder cannot be read";
            return 0;
        }
        std::error_code ec;
        std::filesystem::create_directories(gameDir, ec);
        int written = 0;
        for (size_t pos = 0; pos < dir.size();)
        {
            const uint8_t len = dir[pos];
            if (len == 0)
            {
                pos = (pos / 2048 + 1) * 2048; // the rest of this sector is padding
                continue;
            }
            if (pos + len > dir.size() || len < 34)
                break;
            const uint8_t *rec = dir.data() + pos;
            const bool isDir = (rec[25] & 2u) != 0u;
            std::string name(reinterpret_cast<const char *>(rec + 33), rec[32]);
            if (const size_t semi = name.find(';'); semi != std::string::npos)
                name.resize(semi);
            std::string upper = name;
            for (char &ch : upper)
                ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
            const bool core = upper == "CORE.GT3";
            const bool wanted = !isDir && (core || (upper.size() > 4 && upper.compare(upper.size() - 4, 4, ".IRX") == 0));
            if (wanted)
            {
                std::vector<uint8_t> data;
                if (readExtent(u32(rec + 2), u32(rec + 10), data))
                {
                    std::string outName = upper;
                    if (core)
                    {
                        // GT3's program: unpacked into CORE.elf (see buildCoreElf).
                        std::vector<uint8_t> elf;
                        std::string coreError;
                        if (!buildCoreElf(data, elf, coreError))
                        {
                            error = coreError;
                            pos += len;
                            continue;
                        }
                        data.swap(elf);
                        outName = "CORE.elf";
                    }
                    std::ofstream out(gameDir + "\\" + outName, std::ios::binary | std::ios::trunc);
                    if (out.write(reinterpret_cast<const char *>(data.data()), static_cast<std::streamsize>(data.size())))
                        ++written;
                }
            }
            pos += len;
        }
        if (written == 0 && error.empty())
            error = "CORE.GT3 and the .IRX files were not found in the disc image's root folder";
        return written;
    }

    // First-run defaults: the disc path from Play-GT3-latest.cmd when present.
    std::string discFromPlayScript(const std::string &root)
    {
        std::ifstream in(root + "\\Play-GT3-latest.cmd");
        std::string line;
        const std::string key = "set \"GT3_DISC_IMAGE=";
        while (std::getline(in, line))
        {
            const size_t at = line.find(key);
            if (at == std::string::npos)
                continue;
            std::string value = line.substr(at + key.size());
            const size_t quote = value.rfind('"');
            if (quote != std::string::npos)
                value.resize(quote);
            return value;
        }
        return {};
    }

    Settings loadSettings(const std::string &file, const Settings &defaults)
    {
        Settings s = defaults;
        std::ifstream in(file);
        std::string line;
        auto clampInt = [](const std::string &v, int maxValue) { return std::clamp(std::atoi(v.c_str()), 0, maxValue); };
        while (std::getline(in, line))
        {
            if (line.empty() || line[0] == '#' || line[0] == ';')
                continue;
            const size_t eq = line.find('=');
            if (eq == std::string::npos)
                continue;
            const std::string key = trim(line.substr(0, eq)), value = trim(line.substr(eq + 1));
            if (key == "iso")
                s.iso = value;
            else if (key == "memcard1")
                s.memcard = value;
            else if (key == "input_profile")
                s.profile = value;
            else if (key == "renderer")
                s.renderer = clampInt(value, 2);
            else if (key == "display_filter")
                s.filter = clampInt(value, 5);
            else if (key == "upscale_sharpness")
                s.sharpening = clampInt(value, 100);
            else if (key == "display_aspect")
                s.aspect = clampInt(value, 4);
            else if (key == "window")
                s.window = clampInt(value, 3);
            else if (key == "deinterlace")
                s.deinterlace = clampInt(value, 4);
            else if (key == "internal_resolution")
                s.scale = clampInt(value, 5);
            else if (key == "fps_overlay")
                s.fpsOverlay = value != "0";
            else if (key == "frame_generation")
                s.frameGen = clampInt(value, 2);
            else if (key == "car_detail")
                s.carDetail = clampInt(value, 3);
            else if (key == "rim_detail")
                s.rimDetail = clampInt(value, 3);
            else if (key == "wheel_blur")
                s.wheelBlur = value == "0" ? 1 : 0; // the file keeps GT3's sense: 1 blur on
            else if (key == "texture_dump")
                s.textureDump = value == "1";
            else if (key == "texture_replace")
                s.textureReplace = value != "0";
            else if (key == "mirror_detail")
                s.mirrorDetail = clampInt(value, 1);
            else if (key == "link_mode")
                s.linkMode = clampInt(value, 2);
            else if (key == "link_host")
                s.linkHost = value;
            else if (key == "link_port")
                s.linkPort = std::clamp(std::atoi(value.c_str()), 1024, 65535);
            else if (key == "link_name")
                s.linkName = value;
            else if (key == "units_speed")
                s.unitsSpeed = clampInt(value, 1);
            else if (key == "units_power")
                s.unitsPower = clampInt(value, 4);
            else if (key == "units_torque")
                s.unitsTorque = clampInt(value, 2);
            else if (key == "phone_mirror")
                s.phoneMirror = value == "1";
            else if (key == "hud_anchor")
                s.hudAnchor = clampInt(value, 1);
            else if (key == "hud_hide")
                s.hudHide = clampInt(value, 15);
            else if (key == "start_lights")
                s.startLights = value == "1";
            else if (key == "start_lights_count")
                s.startLightsCount = std::max(3, clampInt(value, 5));
            else if (key == "start_wait_min")
                s.startWaitMin = clampInt(value, 100);
            else if (key == "start_wait_max")
                s.startWaitMax = clampInt(value, 100);
            else if (key == "start_ai_reaction")
                s.startAiReaction = clampInt(value, 3);
            else if (key == "race_report")
                s.raceReport = value == "1";
            else if (key == "race_report_save")
                s.raceReportSave = value != "0";
            else if (key == "race_lab_ai")
                s.raceLabAi = value == "1";
            else if (key == "driver_names")
                s.driverNames = value;
            else if (key == "race_report_names")
                s.reportNames = clampInt(value, 1);
            else if (key == "tower_names")
                s.towerNames = clampInt(value, 2);
            else if (key == "tower_time_switch")
                s.towerTimeSwitch = value == "1";
            else if (key == "vibration")
                s.vibration = value != "0";
            else if (key == "pedals_triggers")
                s.pedalsTriggers = value != "0";
            else if (key == "hud_picture")
                s.hudPicture = value;
            else if (key.rfind("hud_", 0) == 0)
            {
                // hud_<widget>_on / _x / _y / _size / _alpha / _style / _range
                for (int w = 0; w < kHudCount; ++w)
                {
                    const std::string prefix = std::string("hud_") + kHudKeys[w] + "_";
                    if (key.rfind(prefix, 0) != 0)
                        continue;
                    const std::string field = key.substr(prefix.size());
                    auto &b = s.hudBoxes[static_cast<size_t>(w)];
                    if (field == "on")
                        b.on = value == "1";
                    else if (field == "x")
                        b.x = clampInt(value, 100);
                    else if (field == "y")
                        b.y = clampInt(value, 100);
                    else if (field == "size")
                        b.size = std::clamp(std::atoi(value.c_str()), 2, 60);
                    else if (field == "alpha")
                        b.alpha = clampInt(value, 100);
                    else if (field == "style")
                        b.style = clampInt(value, kHudStyleCount[w] - 1);
                    else if (field == "range")
                        b.range = std::clamp(std::atoi(value.c_str()), 100, 1500);
                    else if (field == "columns")
                        b.columns = std::clamp(std::atoi(value.c_str()), 0, 255);
                    else if (field == "time")
                        b.timeMode = clampInt(value, gt3hud::kTowerTimeCount - 1);
                }
            }
            else if (key == "radar")
                s.radarMode = clampInt(value, 2);
            else if (key == "radar_x")
                s.radarX = clampInt(value, 100);
            else if (key == "radar_y")
                s.radarY = clampInt(value, 100);
            else if (key == "radar_size")
                s.radarSize = std::clamp(std::atoi(value.c_str()), 8, 60);
            else if (key == "radar_range")
                s.radarRange = std::clamp(std::atoi(value.c_str()), 10, 80);
            else if (key == "radar_opacity")
                s.radarOpacity = std::clamp(std::atoi(value.c_str()), 10, 100);
            else if (key == "radar_bg_opacity")
                s.radarBgOpacity = clampInt(value, 100);
            else if (key == "radar_color_player")
                s.radarPlayer = clampInt(value, 0xFFFFFF);
            else if (key == "radar_color_cars")
                s.radarCars = clampInt(value, 0xFFFFFF);
            else if (key == "radar_color_near")
                s.radarNear = clampInt(value, 0xFFFFFF);
            else if (key == "radar_color_bg")
                s.radarBg = clampInt(value, 0xFFFFFF);
            else if (key == "camera_views")
            {
                s.cameraViews = clampInt(value, 0x3F);
                if (s.cameraViews == 0x3D)
                    s.cameraViews = 0x3F; // the first version's default had Bonnet off (it did not work then)
            }
            else if (key == "camera_start")
                s.cameraStart = std::clamp(std::atoi(value.c_str()), -1, 5);
            else if (key == "camera_look")
                s.cameraLook = clampInt(value, 2);
            else if (key == "wheel_rotation")
                s.wheelRotation = std::clamp(std::atoi(value.c_str()), 90, 2880);
            else if (key == "wheel_lock")
                s.wheelLock = std::clamp(std::atoi(value.c_str()), 45, 2880);
            else if (key == "wheel_gt3_deadzone")
                s.wheelDeadzone = value != "0";
            else if (key == "pad_style")
                s.padStyle = clampInt(value, 1);
            else if (key == "phone_order" || key == "phone_sizes")
            {
                // 5 digits, one per panel (+ 1): the order (each panel once) or the sizes.
                const bool order = key == "phone_order";
                std::array<int, 5> digits{};
                bool ok = value.size() == 5u, seen[5] = {};
                for (int i = 0; ok && i < 5; ++i)
                {
                    digits[i] = value[i] - '1';
                    ok = digits[i] >= 0 && digits[i] <= (order ? 4 : 3) && !(order && seen[digits[i]]);
                    if (ok && order)
                        seen[digits[i]] = true;
                }
                if (ok)
                    (order ? s.phoneOrder : s.phoneSizes) = digits;
            }
            else if (key == "phone_arrange")
                s.phoneArrange = clampInt(value, 2);
            else if (key == "phone_radar_range")
                s.phoneRadarRange = std::clamp(std::atoi(value.c_str()), 10, 100);
            else if (key == "phone_map_fallback")
                s.phoneMapFallback = value != "0";
            else if (key == "launcher_width")
                s.launcherWidth = std::clamp(std::atoi(value.c_str()), 0, 16384);
            else if (key == "launcher_height")
                s.launcherHeight = std::clamp(std::atoi(value.c_str()), 0, 16384);
            else if (key == "launcher_fullscreen")
                s.launcherFullscreen = value == "1";
            else if (key == "triple_monitor")
                s.tripleMonitor = clampInt(value, 2);
            else if (key == "triple_inches")
                s.tripleInches = std::clamp(std::atoi(value.c_str()), 17, 65);
            else if (key == "triple_bezel_mm")
                s.tripleBezelMm = clampInt(value, 80);
            else if (key == "triple_fov")
                s.tripleFov = clampInt(value, 1);
            else if (key == "triple_seat_cm")
                s.tripleSeatCm = std::clamp(std::atoi(value.c_str()), 30, 200);
            else if (key == "triple_sides")
                s.tripleSides = clampInt(value, 1);
            else if (key == "triple_angle")
                s.tripleAngle = clampInt(value, 80);
            else if (key == "triple_side_detail")
                s.tripleSideDetail = clampInt(value, 1);
        }
        // Rim detail came after car detail (which covered the wheels for a
        // while): a file without it keeps the wheels as car detail had them.
        if (s.rimDetail < 0)
            s.rimDetail = s.carDetail;
        return s;
    }

    // GT3_HUD_<NAME>=on,x,y,size,opacity,style,range,columns,time for widget w.
    std::string hudEnv(const Settings &s, int w)
    {
        const auto &b = s.hudBoxes[static_cast<size_t>(w)];
        return std::to_string(b.on ? 1 : 0) + "," + std::to_string(b.x) + "," + std::to_string(b.y) + "," + std::to_string(b.size) + "," +
               std::to_string(b.alpha) + "," + std::to_string(b.style) + "," + std::to_string(b.range) + "," + std::to_string(b.columns) + "," +
               std::to_string(b.timeMode);
    }

    std::string phoneDigits(const std::array<int, 5> &values)
    {
        std::string digits;
        for (const int v : values)
            digits += static_cast<char>('1' + v);
        return digits;
    }

    void saveSettings(const std::string &file, const Settings &s)
    {
        std::ofstream out(file, std::ios::trunc);
        out << "# GT3 launcher settings (gt3Launcher.exe writes this file).\n"
            << "iso=" << s.iso << "\n"
            << "memcard1=" << s.memcard << "\n"
            << "input_profile=" << s.profile << "\n"
            << "# 0 OpenGL (GPU), 1 software (CPU), 2 Vulkan (GPU, experimental)\nrenderer=" << s.renderer << "\n"
            << "# 0 sharp, 1 bilinear, 2 nearest, 3 AMD FSR 1, 4 NVIDIA Image Scaling, 5 TV (CRT)\ndisplay_filter=" << s.filter << "\n"
            << "# FSR 1 / NIS sharpening 0..100\nupscale_sharpness=" << s.sharpening << "\n"
            << "# 0 4:3, 1 square pixels, 2 16:9, 3 21:9, 4 triple 16:9 (48:9)\ndisplay_aspect=" << s.aspect << "\n"
            << "# 0 window fitted to the screen, 1 1280x960, 2 borderless full screen, 3 borderless over all monitors\nwindow=" << s.window << "\n"
            << "# 0 adaptive, 1 weave, 2 bob, 3 double, 4 progressive\ndeinterlace=" << s.deinterlace << "\n"
            << "# 0 native, 1 2x, 2 3x, 3 4x, 4 6x, 5 8x\ninternal_resolution=" << s.scale << "\n"
            << "fps_overlay=" << (s.fpsOverlay ? 1 : 0) << "\n"
            << "# frame generation (experimental): 0 off, 1 90 fps, 2 120 fps\nframe_generation=" << s.frameGen << "\n"
            << "# cars' detail: 0 GT3's own switching, 1 full detail twice as far, 2 four times as far, 3 always full\ncar_detail=" << s.carDetail << "\n"
            << "# wheels' detail (rims, tyres): 0 GT3's own switching, 1 detailed rims twice as far, 2 four times as far, 3 always full\nrim_detail="
            << std::max(0, s.rimDetail) << "\n"
            << "# wheel spin blur: 1 GT3's own (turning rims drawn blurred, a little smaller), 0 off (the still rim, turning)\nwheel_blur="
            << (s.wheelBlur == 1 ? 0 : 1) << "\n"
            << "# rear-view mirror: 0 GT3's own detail, 1 the view ahead's detail\nmirror_detail=" << s.mirrorDetail << "\n"
            << "# 1: every texture the game uses saved as a PNG in textures\\dump (for upscaling)\ntexture_dump=" << (s.textureDump ? 1 : 0) << "\n"
            << "# 1: pictures in textures\\replace drawn instead of the textures they are named after\ntexture_replace=" << (s.textureReplace ? 1 : 0) << "\n"
            << "# i.LINK Battle over the network: 0 off, 1 host, 2 join\nlink_mode=" << s.linkMode << "\n"
            << "# join: host address[:port]; empty = find a host on the local network\nlink_host=" << s.linkHost << "\n"
            << "link_port=" << s.linkPort << "\n"
            << "link_name=" << s.linkName << "\n"
            << "# HUD units: speed 0 km/h, 1 mph; power 0 PS, 1 HP, 2 BHP, 3 CV, 4 kW; torque 0 kgf.m, 1 ft.lb, 2 Nm\n"
            << "units_speed=" << s.unitsSpeed << "\n"
            << "units_power=" << s.unitsPower << "\n"
            << "units_torque=" << s.unitsTorque << "\n"
            << "# rear-view mirror on a phone (http://<this PC>:8093/): 0 off, 1 on\n"
            << "phone_mirror=" << (s.phoneMirror ? 1 : 0) << "\n"
            << "# wide races: HUD 1 at the screen's sides (triple: the middle screen's), 0 in the middle 4:3\n"
            << "hud_anchor=" << s.hudAnchor << "\n"
            << "# GT3's own race HUD parts hidden: bits 1 position and map, 2 lap times, 4 gauges and lights, 8 mirror\n"
            << "hud_hide=" << s.hudHide << "\n"
            << "# grid starts: 1 start lights instead of GT3's 3 2 1 START, 0 GT3's own\n"
            << "start_lights=" << (s.startLights ? 1 : 0) << "\n"
            << "# start lights: how many (3-5)\n"
            << "start_lights_count=" << s.startLightsCount << "\n"
            << "# lights out: shortest and longest wait after the last light, tenths of a second\n"
            << "start_wait_min=" << s.startWaitMin << "\n"
            << "start_wait_max=" << s.startWaitMax << "\n"
            << "# AI cars' reaction to the lights going out: 0 instant (GT3's), 1 quick, 2 normal, 3 slow\n"
            << "start_ai_reaction=" << s.startAiReaction << "\n"
            << "# end-of-race report (results, lap times, sectors, positions; F2 in game): 1 on, 0 off\n"
            << "race_report=" << (s.raceReport ? 1 : 0) << "\n"
            << "# each report also saved as a web page in the race-reports folder: 1 yes, 0 no\n"
            << "race_report_save=" << (s.raceReportSave ? 1 : 0) << "\n"
            << "# AI cars 1-5: 0 GT3's own AI, 1 what the Race Lab sent them (racelab-data\\ai-lines)\n"
            << "race_lab_ai=" << (s.raceLabAi ? 1 : 0) << "\n"
            << "# the AI cars' drivers, comma-separated (the player's name is link_name)\n"
            << "driver_names=" << s.driverNames << "\n"
            << "# the race report starts on: 0 drivers' names, 1 cars'\n"
            << "race_report_names=" << s.reportNames << "\n"
            << "# timing tower names: 0 cars, 1 drivers, 2 switching every 5 s\n"
            << "tower_names=" << s.towerNames << "\n"
            << "# timing tower time column switching between best and last lap every 5 s: 1 on, 0 off\n"
            << "tower_time_switch=" << (s.towerTimeSwitch ? 1 : 0) << "\n"
            << "# controller vibration: 1 on, 0 off\n"
            << "vibration=" << (s.vibration ? 1 : 0) << "\n"
            << "# 1: right trigger = accelerate, left trigger = brake (analog; GT3's Key Configuration set to match); 0: the mapping file alone\n"
            << "pedals_triggers=" << (s.pedalsTriggers ? 1 : 0) << "\n"
            << "# the HUD layout / radar preview picture (empty: the latest race's)\n"
            << "hud_picture=" << s.hudPicture << "\n"
            << "# our own HUD widgets: on, x / y percent, size percent of the height, box opacity, style, range (zoomed map, metres);\n"
            << "# timing tower columns (bits 1 place, 2 colour, 4 car, 8 lap, 16 time, 32 gap, 64 interval, 128 Car 1..6), time 0 best 1 last 2 this lap\n";
        for (int w = 0; w < kHudCount; ++w)
        {
            const auto &b = s.hudBoxes[static_cast<size_t>(w)];
            const std::string k = std::string("hud_") + kHudKeys[w] + "_";
            out << k << "on=" << (b.on ? 1 : 0) << "\n"
                << k << "x=" << b.x << "\n"
                << k << "y=" << b.y << "\n"
                << k << "size=" << b.size << "\n"
                << k << "alpha=" << b.alpha << "\n";
            if (kHudStyleCount[w] > 1)
                out << k << "style=" << b.style << "\n";
            if (w == 10)
                out << k << "range=" << b.range << "\n";
            if (w == 8)
                out << k << "columns=" << b.columns << "\n" << k << "time=" << b.timeMode << "\n";
        }
        out
            << "# proximity radar: 0 off, 1 when cars are near, 2 always; position percent; colours 0xRRGGBB as decimal\n"
            << "radar=" << s.radarMode << "\n"
            << "radar_x=" << s.radarX << "\n"
            << "radar_y=" << s.radarY << "\n"
            << "radar_size=" << s.radarSize << "\n"
            << "radar_range=" << s.radarRange << "\n"
            << "radar_opacity=" << s.radarOpacity << "\n"
            << "radar_bg_opacity=" << s.radarBgOpacity << "\n"
            << "radar_color_player=" << s.radarPlayer << "\n"
            << "radar_color_cars=" << s.radarCars << "\n"
            << "radar_color_near=" << s.radarNear << "\n"
            << "radar_color_bg=" << s.radarBg << "\n"
            << "# race cameras: views in the R1 cycle (bits bumper 1, bonnet 2, roof 4, close chase 8, chase 16, far chase 32);\n"
            << "# start view -1 GT3's own, 0-5 as the bits; look-around 0 off, 1 left-right only, 2 full 360\n"
            << "camera_views=" << s.cameraViews << "\n"
            << "camera_start=" << s.cameraStart << "\n"
            << "camera_look=" << s.cameraLook << "\n"
            << "# Controls page names: 0 Xbox-style controller, 1 PlayStation (DualShock 2 on an adapter, DualShock 4 / 5)\n"
            << "pad_style=" << s.padStyle << "\n"
            << "# steering wheel: rotation set on the base, lock for full steering (degrees lock to lock), GT3's dead zone made up for\n"
            << "wheel_rotation=" << s.wheelRotation << "\nwheel_lock=" << s.wheelLock << "\nwheel_gt3_deadzone=" << (s.wheelDeadzone ? 1 : 0) << "\n"
            << "# phone dashboard: panels 1 mirror, 2 position, 3 timing, 4 map, 5 radar in order; sizes per panel 1 off, 2 small,\n"
            << "# 3 medium, 4 large; arrangement 0 auto, 1 one column, 2 two columns; radar range metres\n"
            << "phone_order=" << phoneDigits(s.phoneOrder) << "\n"
            << "phone_sizes=" << phoneDigits(s.phoneSizes) << "\n"
            << "phone_arrange=" << s.phoneArrange << "\n"
            << "phone_radar_range=" << s.phoneRadarRange << "\n"
            << "# phone map without GT3's course data: 1 drawn from the cars' paths, 0 not drawn\n"
            << "phone_map_fallback=" << (s.phoneMapFallback ? 1 : 0) << "\n"
            << "# this launcher's window: size when not full screen, full screen 1 / 0 (F11)\n"
            << "launcher_width=" << s.launcherWidth << "\n"
            << "launcher_height=" << s.launcherHeight << "\n"
            << "launcher_fullscreen=" << (s.launcherFullscreen ? 1 : 0) << "\n"
            << "# triple screens: shape 0 16:9, 1 16:10, 2 21:9; size inches; picture gap mm (0 off); field of view 0 GT3's own,\n"
            << "# 1 matched to the eyes' distance (cm); side screens 0 flat, 1 angled (not built yet); angle degrees\n"
            << "triple_monitor=" << s.tripleMonitor << "\n"
            << "triple_inches=" << s.tripleInches << "\n"
            << "triple_bezel_mm=" << s.tripleBezelMm << "\n"
            << "triple_fov=" << s.tripleFov << "\n"
            << "triple_seat_cm=" << s.tripleSeatCm << "\n"
            << "triple_sides=" << s.tripleSides << "\n"
            << "triple_angle=" << s.tripleAngle << "\n"
            << "triple_side_detail=" << s.tripleSideDetail << "\n";
    }

    // Triple screens worked out from the settings: one screen's shape and
    // size, the bezel gap in screen widths, the whole picture's shape, the
    // field-of-view zoom (GT3's own vertical field of view is 47.8 degrees:
    // tan 0.4437; matched: the middle screen seen as from the seat), and the
    // resulting fields of view in degrees.
    struct TripleGeometry
    {
        float aspect = 16.0f / 9.0f, widthCm = 0.0f, heightCm = 0.0f, gap = 0.0f, pictureAspect = 48.0f / 9.0f, zoom = 1.0f;
        float middleDeg = 0.0f, totalDeg = 0.0f, verticalDeg = 0.0f;
    };
    TripleGeometry tripleGeometry(const Settings &s)
    {
        static const float aspects[3] = {16.0f / 9.0f, 16.0f / 10.0f, 21.0f / 9.0f};
        TripleGeometry g;
        g.aspect = aspects[std::clamp(s.tripleMonitor, 0, 2)];
        const float diagonal = s.tripleInches * 2.54f;
        g.heightCm = diagonal / std::sqrt(1.0f + g.aspect * g.aspect);
        g.widthCm = g.heightCm * g.aspect;
        g.gap = s.tripleBezelMm * 0.1f / g.widthCm;
        g.pictureAspect = (3.0f + 2.0f * g.gap) * g.aspect;
        const float gt3Tan = 0.4437f; // half the vertical field of view
        float halfTan = gt3Tan;       // the vertical half-angle's tangent seen on the screens
        if (s.tripleFov == 1)
        {
            halfTan = (g.heightCm * 0.5f) / static_cast<float>(s.tripleSeatCm);
            g.zoom = std::clamp(gt3Tan / halfTan, 0.4f, 4.0f);
            halfTan = gt3Tan / g.zoom;
        }
        const float halfWidthTan = halfTan * g.aspect; // the middle screen's half-width over the distance
        g.middleDeg = 2.0f * std::atan(halfWidthTan) * 57.29578f;
        g.totalDeg = 2.0f * std::atan(halfWidthTan * (3.0f + 2.0f * g.gap)) * 57.29578f;
        g.verticalDeg = 2.0f * std::atan(halfTan) * 57.29578f;
        return g;
    }

    std::string cameraViewList(int bits)
    {
        std::string list;
        for (int v = 0; v < 6; ++v)
            if (bits & (1 << v))
                list += (list.empty() ? "" : ",") + std::string(kCameraKeys[v]);
        return list;
    }

    std::string hexColour(int c)
    {
        char text[8];
        std::snprintf(text, sizeof(text), "%06X", static_cast<unsigned>(c) & 0xFFFFFFu);
        return text;
    }

    void applyStyle(Font font)
    {
        GuiSetFont(font);
        GuiSetStyle(DEFAULT, TEXT_SIZE, 20);
        GuiSetStyle(DEFAULT, TEXT_SPACING, 1);
        GuiSetStyle(DEFAULT, BACKGROUND_COLOR, ColorToInt(kBackground));
        GuiSetStyle(DEFAULT, LINE_COLOR, ColorToInt(kBorder));
        GuiSetStyle(DEFAULT, BORDER_WIDTH, 1);
        GuiSetStyle(DEFAULT, BORDER_COLOR_NORMAL, ColorToInt(kBorder));
        GuiSetStyle(DEFAULT, BASE_COLOR_NORMAL, ColorToInt(kPanelRaised));
        GuiSetStyle(DEFAULT, TEXT_COLOR_NORMAL, ColorToInt(kText));
        GuiSetStyle(DEFAULT, BORDER_COLOR_FOCUSED, ColorToInt(kAccent));
        GuiSetStyle(DEFAULT, BASE_COLOR_FOCUSED, ColorToInt(Color{40, 46, 60, 255}));
        GuiSetStyle(DEFAULT, TEXT_COLOR_FOCUSED, ColorToInt(kText));
        GuiSetStyle(DEFAULT, BORDER_COLOR_PRESSED, ColorToInt(kAccent));
        GuiSetStyle(DEFAULT, BASE_COLOR_PRESSED, ColorToInt(kAccentDark));
        GuiSetStyle(DEFAULT, TEXT_COLOR_PRESSED, ColorToInt(WHITE));
        GuiSetStyle(DEFAULT, BORDER_COLOR_DISABLED, ColorToInt(Color{36, 40, 50, 255}));
        GuiSetStyle(DEFAULT, BASE_COLOR_DISABLED, ColorToInt(Color{24, 27, 34, 255}));
        GuiSetStyle(DEFAULT, TEXT_COLOR_DISABLED, ColorToInt(Color{90, 96, 110, 255}));
        GuiSetStyle(TOGGLE, GROUP_PADDING, 6);
        GuiSetStyle(CHECKBOX, TEXT_COLOR_NORMAL, ColorToInt(kAccent));
        GuiSetStyle(CHECKBOX, TEXT_COLOR_FOCUSED, ColorToInt(kAccent));
        GuiSetStyle(CHECKBOX, TEXT_COLOR_PRESSED, ColorToInt(kAccent));
        GuiSetStyle(TEXTBOX, TEXT_PADDING, 8);
        GuiSetStyle(TEXTBOX, TEXT_ALIGNMENT, TEXT_ALIGN_LEFT);
        GuiSetStyle(BUTTON, BORDER_WIDTH, 1);
    }

    struct PathBox
    {
        std::array<char, 1024> text{};
        bool editing = false;
        void set(const std::string &s) { std::snprintf(text.data(), text.size(), "%s", s.c_str()); }
        std::string get() const { return trim(text.data()); }
    };

    class Launcher
    {
    public:
        // Test options: start on a page; save a screenshot after a few frames and quit.
        int startPage = 0;
        std::string shotPath;
        bool playAtStart = false;
        int testWidth = 0, testHeight = 0; // --size WxH (tests: the window at that size)
        bool testFullscreen = false;       // --fullscreen (tests)
        bool testQr = false;               // --qr (tests: the Phone page's QR code shown)
        int testHudSelect = -1;            // --hud-select N (tests: that HUD box selected)
        bool testTowerColumns = false;     // --tower-columns (tests: the custom tower's columns shown)
        bool testWheelTab = false;         // --wheel-tab (tests: the Controls page's Wheel & pedals tab)
        bool testRaceLab = false;          // --race-lab (tests: the Race Lab started as its button does, no browser opened)
        int shotFrames = 5;                // --shot-frames N: the screenshot after N frames (30 a second)
        std::string extractTo;             // --extract-to <folder> (test): the disc image's game files there, then exit

        void run()
        {
            SetConfigFlags(FLAG_MSAA_4X_HINT | FLAG_WINDOW_RESIZABLE);
            InitWindow(kDesignW, kDesignH, "Gran Turismo 3 A-Spec - Native PC Launcher");
            SetWindowMinSize(kDesignW * 3 / 5, kDesignH * 3 / 5);
            SetExitKey(KEY_NULL); // Esc closes the QR code, not the launcher
            SetTargetFPS(30);
            // Large glyphs, so text stays sharp when the window is enlarged.
            m_font = loadFont("C:\\Windows\\Fonts\\segoeui.ttf", 72);
            m_fontBold = loadFont("C:\\Windows\\Fonts\\segoeuib.ttf", 96);
            applyStyle(m_font);

            m_root = findRoot();
            m_settingsFile = m_root + "\\gt3-launcher.ini";
            m_runner = executableDirectory() + "\\ps2EntryRunner.exe";
            if (!fileExists(m_runner))
                m_runner = m_root + "\\out\\gt3-msvc-release\\ps2xRuntime\\ps2EntryRunner.exe";
            Settings defaults;
            defaults.iso = discFromPlayScript(m_root);
            if (defaults.iso.empty())
            {
                // A portable copy: the disc image in its "disc" folder.
                const std::vector<std::string> discs = filesIn(m_root + "\\disc", "*.iso");
                if (!discs.empty())
                    defaults.iso = "disc\\" + discs.front();
            }
            defaults.memcard = "memcards\\Mcd001.ps2";
            defaults.profile = "gt3-input.profile";
            defaults.linkName = computerName();
            m_settings = loadSettings(m_settingsFile, defaults);
            for (int p = 0; p < 5; ++p)
                if (m_settings.phoneSizes[p] > 0)
                    m_phoneLastSize[p] = m_settings.phoneSizes[p];
            m_iso.set(m_settings.iso);
            m_card.set(m_settings.memcard);
            m_profile.set(m_settings.profile);
            m_linkHost.set(m_settings.linkHost);
            m_linkName.set(m_settings.linkName);
            m_driverNames.set(m_settings.driverNames);
            m_linkPort.set(std::to_string(m_settings.linkPort));
            m_linkStatusFile = m_root + "\\gt3-link-status.txt";
            m_addresses = localAddresses();
            m_page = std::clamp(startPage, 0, kPageCount - 1);
            // --extract-to <folder> (test): the disc image's game files into
            // that folder, the result in <folder>\extract-result.txt, then exit.
            if (!extractTo.empty())
            {
                std::string error;
                const int n = extractGameFiles(full(m_iso.get()), extractTo, error);
                std::ofstream(extractTo + "\\extract-result.txt") << n << " files; complete " << (gameFilesPresent(extractTo) ? "yes" : "no") << "; "
                                                                   << error << "\n";
                CloseWindow();
                return;
            }
            // The window as it was left: its size (kept on the screen), full screen.
            if (m_settings.launcherWidth >= kDesignW * 3 / 5 && m_settings.launcherHeight >= kDesignH * 3 / 5 && shotPath.empty())
            {
                const int monitor = GetCurrentMonitor();
                const int w = std::min(m_settings.launcherWidth, GetMonitorWidth(monitor));
                const int h = std::min(m_settings.launcherHeight, GetMonitorHeight(monitor) - 60);
                SetWindowSize(w, h);
                SetWindowPosition(std::max(0, (GetMonitorWidth(monitor) - w) / 2), std::max(30, (GetMonitorHeight(monitor) - h) / 2));
            }
            if (m_settings.launcherFullscreen && shotPath.empty())
                ToggleBorderlessWindowed();
            if (testWidth > 0 && testHeight > 0)
                SetWindowSize(testWidth, testHeight);
            if (testFullscreen)
                ToggleBorderlessWindowed();
            m_qrOpen = testQr;
            m_hudSelected = testHudSelect;
            m_towerColumnsOpen = testTowerColumns;
            m_controlsTab = testWheelTab ? 1 : 0;
            if (playAtStart)
                launch();
            if (testRaceLab)
                openRaceLab();

            int frames = 0;
            while (!WindowShouldClose())
            {
                reloadIfChangedElsewhere();
                if (pollGame(m_game))
                {
                    m_status = m_game.exitCode == 0 ? "The game closed normally." : "The game exited with code " + std::to_string(m_game.exitCode) + ": see the log.";
                    m_statusColor = m_game.exitCode == 0 ? kGood : kWarn;
                    if (IsWindowMinimized())
                        RestoreWindow();
                }
                if (pollGame(m_raceLab))
                {
                    // stopped here, or it couldn't start (its log's last line says why)
                    m_raceLabUrl.clear();
                    if (!m_raceLabStopping)
                        setStatus("The Race Lab stopped: " + lastLine(m_raceLabLog), kWarn);
                    m_raceLabStopping = false;
                }
                if (IsKeyPressed(KEY_F11) || m_toggleFullscreen)
                {
                    toggleFullscreen();
                    m_toggleFullscreen = false;
                }
                // The pages at kDesignW x kDesignH, scaled to fit the window
                // (the rest background); the mouse mapped back the same way.
                const float sw = static_cast<float>(GetScreenWidth()), sh = static_cast<float>(GetScreenHeight());
                m_viewScale = std::max(0.1f, std::min(sw / kDesignW, sh / kDesignH));
                m_viewX = std::floor((sw - kDesignW * m_viewScale) * 0.5f);
                m_viewY = std::floor((sh - kDesignH * m_viewScale) * 0.5f);
                SetMouseOffset(static_cast<int>(-m_viewX), static_cast<int>(-m_viewY));
                SetMouseScale(1.0f / m_viewScale, 1.0f / m_viewScale);
                BeginDrawing();
                ClearBackground(kBackground);
                {
                    // The header, sidebar and status bar reach the window's edges.
                    const float s = m_viewScale, headerBottom = m_viewY + 88.0f * s, statusTop = m_viewY + (kDesignH - 32) * s;
                    DrawRectangleRec(Rectangle{0.0f, 0.0f, sw, headerBottom}, kPanel);
                    DrawRectangleRec(Rectangle{0.0f, headerBottom - s, sw, s}, kBorder);
                    DrawRectangleRec(Rectangle{0.0f, statusTop, sw, sh - statusTop}, kPanel);
                    DrawRectangleRec(Rectangle{0.0f, statusTop, sw, s}, kBorder);
                    DrawRectangleRec(Rectangle{0.0f, headerBottom, m_viewX + 220.0f * s, statusTop - headerBottom}, kPanel);
                }
                Camera2D view{};
                view.offset = Vector2{m_viewX, m_viewY};
                view.zoom = m_viewScale;
                BeginMode2D(view);
                drawHeader();
                drawSidebar();
                switch (m_page)
                {
                case kPagePlay: pagePlay(); break;
                case kPageGraphics: pageGraphics(); break;
                case kPageDetail: pageDetail(); break;
                case kPageHud: pageHud(); break;
                case kPageHudLayout: pageHudLayout(); break;
                case kPageRadar: pageRadar(); break;
                case kPageStartLights: pageStartLights(); break;
                case kPageRaceReport: pageRaceReport(); break;
                case kPageCamera: pageCamera(); break;
                case kPagePhone: pagePhone(); break;
                case kPageTriple: pageTriple(); break;
                case kPageControls: pageControls(); break;
                case kPageMemoryCard: pageMemoryCard(); break;
                case kPageLink: pageLink(); break;
                case kPagePaths: pagePaths(); break;
                case kPageRaceLab: pageRaceLab(); break;
                default: pageRoadmap(); break;
                }
                drawStatusBar();
                EndMode2D();
                EndDrawing();
                if (!shotPath.empty() && ++frames == shotFrames)
                {
                    TakeScreenshot(shotPath.c_str());
                    break;
                }
            }
            syncFromBoxes();
            rememberWindow();
            saveOwn();
            UnloadFont(m_font);
            UnloadFont(m_fontBold);
            CloseWindow();
        }

    private:
        bool fullscreen() const { return IsWindowState(FLAG_BORDERLESS_WINDOWED_MODE); }

        // The window's size while windowed, and whether it is full screen,
        // for the next start.
        void rememberWindow()
        {
            m_settings.launcherFullscreen = fullscreen();
            if (!fullscreen() && !IsWindowMinimized() && shotPath.empty())
            {
                m_settings.launcherWidth = GetScreenWidth();
                m_settings.launcherHeight = GetScreenHeight();
            }
        }

        // F11 / the header button: borderless full screen on this monitor, or back.
        void toggleFullscreen()
        {
            if (!fullscreen())
                rememberWindow();
            ToggleBorderlessWindowed();
            m_settings.launcherFullscreen = fullscreen();
            saveOwn();
        }

        // A scissor rectangle in page coordinates (the pages are scaled).
        void beginScissor(Rectangle r)
        {
            BeginScissorMode(static_cast<int>(m_viewX + r.x * m_viewScale), static_cast<int>(m_viewY + r.y * m_viewScale),
                             static_cast<int>(std::ceil(r.width * m_viewScale)), static_cast<int>(std::ceil(r.height * m_viewScale)));
        }

        static Font loadFont(const char *path, int size)
        {
            if (!fileExists(path))
                return GetFontDefault();
            Font f = LoadFontEx(path, size, nullptr, 0);
            GenTextureMipmaps(&f.texture);
            SetTextureFilter(f.texture, TEXTURE_FILTER_TRILINEAR);
            return f;
        }

        void text(const char *s, float x, float y, float size, Color c, bool bold = false)
        {
            DrawTextEx(bold ? m_fontBold : m_font, s, Vector2{x, y}, size, 1.0f, c);
        }

        // s shortened in the middle ("C:\Users\...\file.iso") to fit width pixels.
        std::string fit(const std::string &s, float width, float size)
        {
            if (MeasureTextEx(m_font, s.c_str(), size, 1.0f).x <= width)
                return s;
            size_t keep = s.size();
            while (keep > 8)
            {
                --keep;
                const size_t head = keep / 3, tail = keep - head;
                const std::string t = s.substr(0, head) + "..." + s.substr(s.size() - tail);
                if (MeasureTextEx(m_font, t.c_str(), size, 1.0f).x <= width)
                    return t;
            }
            return s.substr(0, 8) + "...";
        }

        // Wraps s into lines of at most width pixels.
        void paragraph(const std::string &s, float x, float y, float width, float size, Color c)
        {
            std::string line, word;
            float dy = 0.0f;
            auto flush = [&]() {
                text(line.c_str(), x, y + dy, size, c);
                dy += size * 1.35f;
                line.clear();
            };
            for (size_t i = 0; i <= s.size(); ++i)
            {
                const char ch = i < s.size() ? s[i] : ' ';
                if (ch == '\n' || ch == ' ')
                {
                    const std::string candidate = line.empty() ? word : line + " " + word;
                    if (!line.empty() && MeasureTextEx(m_font, candidate.c_str(), size, 1.0f).x > width)
                    {
                        flush();
                        line = word;
                    }
                    else
                        line = candidate;
                    word.clear();
                    if (ch == '\n')
                        flush();
                }
                else
                    word.push_back(ch);
            }
            if (!line.empty())
                flush();
        }

        void drawHeader()
        {
            DrawRectangle(0, 0, kDesignW, 88, kPanel);
            DrawRectangle(0, 87, kDesignW, 1, kBorder);
            DrawRectangle(24, 22, 6, 44, kAccent);
            text("GRAN TURISMO 3", 44, 14, 38, kText, true);
            text("A-Spec  |  native Windows recompilation (SCUS-97102)  |  development build", 46, 54, 18, kMuted);
            // Full screen (borderless) or back; F11 too. The window can also
            // be resized by its edges: the pages scale with it.
            if (GuiButton(Rectangle{static_cast<float>(kDesignW) - 204.0f, 24.0f, 184.0f, 40.0f}, fullscreen() ? "Window  (F11)" : "Full screen  (F11)"))
                m_toggleFullscreen = true;
        }

        // The sidebar: seven sections; the open one shows its pages below it
        // (a section opens at the page last used in it).
        struct PageGroup
        {
            const char *name;
            int pages[5];
            int count;
        };
        static constexpr int kGroupCount = 8;
        static constexpr PageGroup kGroups[kGroupCount] = {
            {"Play", {kPagePlay}, 1},
            {"Display", {kPageGraphics, kPageDetail, kPageTriple}, 3},
            {"HUD", {kPageHud, kPageHudLayout, kPageRadar, kPageStartLights, kPageRaceReport}, 5},
            {"Camera", {kPageCamera}, 1},
            {"Controls", {kPageControls}, 1},
            {"Race Lab", {kPageRaceLab}, 1},
            {"Phone & LAN", {kPagePhone, kPageLink}, 2},
            {"More", {kPagePaths, kPageMemoryCard, kPageRoadmap}, 3},
        };
        int m_groupLast[kGroupCount] = {kPagePlay, kPageGraphics, kPageHud, kPageCamera, kPageControls, kPageRaceLab, kPagePhone, kPagePaths};

        static int groupOf(int page)
        {
            for (int g = 0; g < kGroupCount; ++g)
                for (int k = 0; k < kGroups[g].count; ++k)
                    if (kGroups[g].pages[k] == page)
                        return g;
            return 0;
        }

        void openPage(int page)
        {
            syncFromBoxes();
            m_page = page;
            m_qrOpen = false;
        }

        void drawSidebar()
        {
            static const char *pageNames[kPageCount] = {"Play",  "Graphics", "Units & GT3 HUD", "HUD layout", "Radar",       "Camera",     "Phone",
                                                        "Triple screens", "Controls", "Memory card", "LAN / Online", "Game files", "Roadmap",
                                                        "Detail", "Start lights", "Race report", "Race Lab"};
            DrawRectangle(0, 88, 220, kDesignH - 88 - 32, kPanel);
            DrawRectangle(219, 88, 1, kDesignH - 88 - 32, kBorder);
            const int open = groupOf(m_page);
            m_groupLast[open] = m_page;
            float y = 104.0f;
            for (int g = 0; g < kGroupCount; ++g)
            {
                const PageGroup &group = kGroups[g];
                const bool active = g == open, single = group.count == 1;
                const Rectangle r{12, y, 196, 44};
                const bool hot = CheckCollisionPointRec(GetMousePosition(), r);
                if (active && single)
                    DrawRectangleRounded(r, 0.2f, 6, kAccentDark);
                else if (active || hot)
                    DrawRectangleRounded(r, 0.2f, 6, kPanelRaised);
                if (active && !single)
                    DrawRectangle(static_cast<int>(r.x), static_cast<int>(r.y + 10), 4, 24, kAccent);
                text(group.name, r.x + 18, r.y + 11, 22, active && single ? WHITE : kText, active);
                if (!single)
                {
                    // A small arrow: down when open, right when closed.
                    const float cx = r.x + r.width - 22.0f, cy = r.y + r.height * 0.5f;
                    if (active)
                        DrawTriangle(Vector2{cx - 6, cy - 3}, Vector2{cx, cy + 4}, Vector2{cx + 6, cy - 3}, kMuted);
                    else
                        DrawTriangle(Vector2{cx - 3, cy - 6}, Vector2{cx - 3, cy + 6}, Vector2{cx + 4, cy}, kMuted);
                }
                if (hot && IsMouseButtonReleased(MOUSE_BUTTON_LEFT) && !(active && single))
                    openPage(m_groupLast[g]);
                y += 50.0f;
                if (!active || single)
                    continue;
                // The open section's pages.
                for (int k = 0; k < group.count; ++k)
                {
                    const int page = group.pages[k];
                    const Rectangle s{30, y - 2, 178, 36};
                    const bool subHot = CheckCollisionPointRec(GetMousePosition(), s);
                    if (page == m_page)
                        DrawRectangleRounded(s, 0.25f, 6, kAccentDark);
                    else if (subHot)
                        DrawRectangleRounded(s, 0.25f, 6, kPanelRaised);
                    text(pageNames[page], s.x + 16, s.y + 8, 19, page == m_page ? WHITE : kMuted);
                    if (subHot && IsMouseButtonReleased(MOUSE_BUTTON_LEFT) && page != m_page)
                        openPage(page);
                    y += 40.0f;
                }
                y += 8.0f;
            }
        }

        void drawStatusBar()
        {
            const float y = static_cast<float>(kDesignH - 32);
            DrawRectangle(0, static_cast<int>(y), kDesignW, 32, kPanel);
            DrawRectangle(0, static_cast<int>(y), kDesignW, 1, kBorder);
            const std::string running = "Game running (Graphics changes apply live) - " + m_status;
            text(m_game.running ? running.c_str() : m_status.c_str(), 16, y + 6, 18, m_game.running ? kAccent : m_statusColor);
        }

        // A labelled row of choices; returns true when the value changed.
        bool choiceRow(float y, const char *label, const char *options, int count, int &value, const char *help)
        {
            text(label, 250, y + 8, 22, kText);
            const float itemWidth = 600.0f / static_cast<float>(count) - 6.0f;
            const int before = value;
            GuiToggleGroup(Rectangle{470, y, itemWidth, 40}, options, &value);
            if (help)
                text(help, 470, y + 46, 17, kMuted);
            return value != before;
        }

        bool pathRow(float y, const char *label, PathBox &box, const char *buttonText, bool *clicked)
        {
            text(label, 250, y + 8, 22, kText);
            if (GuiTextBox(Rectangle{470, y, 540, 40}, box.text.data(), static_cast<int>(box.text.size()), box.editing))
                box.editing = !box.editing;
            *clicked = GuiButton(Rectangle{1020, y, 130, 40}, buttonText) != 0;
            return true;
        }

        // Paths in the settings may be relative to the project folder (a
        // portable copy: "disc\GT3.iso", "memcards\Mcd001.ps2").
        std::string full(const std::string &path) const
        {
            const bool absolute = path.size() >= 2 && (path[1] == ':' || (path[0] == '\\' && path[1] == '\\'));
            return path.empty() || absolute ? path : m_root + "\\" + path;
        }

        // Game logs: the project's gt3-analysis\local, a portable copy's logs
        // folder, else the project folder.
        std::string logDirectory() const
        {
            for (const char *sub : {"\\gt3-analysis\\local", "\\logs"})
                if (directoryExists(m_root + sub))
                    return m_root + sub;
            return m_root;
        }

        void syncFromBoxes()
        {
            m_settings.iso = m_iso.get();
            m_settings.memcard = m_card.get();
            m_settings.profile = m_profile.get();
            m_settings.linkHost = m_linkHost.get();
            m_settings.linkName = m_linkName.get();
            m_settings.driverNames = m_driverNames.get();
            const int port = std::atoi(m_linkPort.get().c_str());
            if (port >= 1024 && port <= 65535)
                m_settings.linkPort = port;
        }

        std::string windowSize() const
        {
            if (m_settings.window == 1)
                return "1280x960";
            if (m_settings.window != 0)
                return {};
            // Fitted: 85% of the monitor's height (at most 90% of its width),
            // at the picture's shape.
            const int monitor = GetCurrentMonitor();
            const float aspect = m_settings.aspect == 4 ? tripleGeometry(m_settings).pictureAspect
                                 : (m_settings.aspect >= 2 ? kAspectRatios[m_settings.aspect] : 4.0f / 3.0f);
            const int h = std::max(240, static_cast<int>(GetMonitorHeight(monitor) * 0.85f));
            const int w = std::min(static_cast<int>(GetMonitorWidth(monitor) * 0.9f), static_cast<int>(h * aspect)) / 4 * 4;
            return std::to_string(w) + "x" + std::to_string(static_cast<int>(w / aspect) / 2 * 2);
        }

        void launch()
        {
            syncFromBoxes();
            saveOwn();
            const Settings &s = m_settings;
            if (!fileExists(full(s.iso)))
            {
                setStatus("The game disc image was not found: set it on the Game files page.", kBad);
                m_page = kPagePaths;
                return;
            }
            if (!fileExists(m_runner))
            {
                setStatus("The game executable (ps2EntryRunner.exe) was not found: build the project first.", kBad);
                return;
            }
            const std::string logDir = logDirectory();
            rotateLogs(logDir, "play-latest.log", "play-previous-", 3);
            std::remove(m_linkStatusFile.c_str()); // a killed game leaves its last link status behind
            m_linkStatus.clear();
            m_linkPeers.clear();
            const std::vector<std::pair<std::string, std::string>> env = {
                {"GT3_DISC_IMAGE", full(s.iso)},
                {"GT3_RENDERER", s.renderer == 0 ? "gl" : (s.renderer == 2 ? "vk" : "")},
                {"GT3_DISPLAY_FILTER", kFilterValues[s.filter]},
                {"GT3_UPSCALE_SHARPNESS", std::to_string(s.sharpening)},
                {"GT3_DISPLAY_ASPECT", s.aspect == 1 ? "pixels" : ""},
                {"GT3_ASPECT", s.aspect == 4 ? std::to_string(tripleGeometry(s).pictureAspect) : std::string(kAspectValues[s.aspect])},
                {"GT3_TRIPLE_BEZEL", s.aspect == 4 && s.tripleBezelMm > 0 ? std::to_string(tripleGeometry(s).gap) : ""},
                {"GT3_TRIPLE_MONITOR_ASPECT", s.aspect == 4 ? std::string(kTripleMonitorValues[s.tripleMonitor]) : ""},
                {"GT3_TRIPLE_SIDES", s.aspect == 4 && s.tripleSides == 1 ? "angled" : ""},
                {"GT3_TRIPLE_ANGLE", s.aspect == 4 && s.tripleSides == 1 ? std::to_string(s.tripleAngle) : ""},
                {"GT3_SIDE_DETAIL", s.aspect == 4 && s.tripleSides == 1 && s.tripleSideDetail == 1 ? "mirror" : ""},
                {"GT3_FOV_ZOOM", s.aspect == 4 && s.tripleFov == 1 ? std::to_string(tripleGeometry(s).zoom) : ""},
                {"GT3_WINDOW_SIZE", windowSize()},
                {"GT3_FULLSCREEN", s.window == 2 ? "1" : (s.window == 3 ? "span" : "")},
                {"GT3_DEINTERLACE", kDeinterlaceValues[s.deinterlace]},
                {"GT3_GS_GL_SCALE", s.renderer != 1 ? kScaleValues[s.scale] : ""},
                {"GT3_GS_GL_PROGRESSIVE", s.renderer != 1 && s.deinterlace == 4 ? "1" : ""},
                {"GT3_FPS_OVERLAY", s.fpsOverlay ? "" : "0"},
                {"GT3_FRAME_GEN", s.renderer != 1 ? kFrameGenValues[s.frameGen] : ""},
                {"GT3_CAR_DETAIL", s.carDetail ? std::to_string(s.carDetail) : ""},
                {"GT3_RIM_DETAIL", std::to_string(std::max(0, s.rimDetail))},
                {"GT3_WHEEL_BLUR", s.wheelBlur == 1 ? "0" : ""},
                {"GT3_MIRROR_DETAIL", s.mirrorDetail == 1 ? "full" : ""},
                {"GT3_TEXTURE_DUMP", s.textureDump ? m_root + "\\textures\\dump" : ""},
                {"GT3_TEXTURE_REPLACE", s.textureReplace && s.renderer != 1 ? m_root + "\\textures\\replace" : ""},
                {"GT3_UNITS_SPEED", kSpeedValues[s.unitsSpeed]},
                {"GT3_UNITS_POWER", kPowerValues[s.unitsPower]},
                {"GT3_UNITS_TORQUE", kTorqueValues[s.unitsTorque]},
                {"GT3_PHONE_VIEW", s.phoneMirror ? "8093" : ""},
                {"GT3_PHONE_ORDER", phoneDigits(s.phoneOrder)},
                {"GT3_PHONE_SIZES", phoneDigits(s.phoneSizes)},
                {"GT3_PHONE_ARRANGE", std::to_string(s.phoneArrange)},
                {"GT3_PHONE_RADAR_RANGE", std::to_string(s.phoneRadarRange)},
                {"GT3_PHONE_MAP_FALLBACK", s.phoneMapFallback ? "" : "0"},
                {"GT3_HUD_ANCHOR", s.hudAnchor ? "" : "0"},
                {"GT3_HUD_HIDE", s.hudHide ? std::to_string(s.hudHide) : ""},
                {"GT3_START_LIGHTS", s.startLights ? "1" : ""},
                {"GT3_START_LIGHTS_COUNT", s.startLights ? std::to_string(s.startLightsCount) : ""},
                {"GT3_START_WAIT", s.startLights ? startWaitValue(s) : ""},
                {"GT3_START_AI_REACTION", s.startLights ? kStartReactionValues[s.startAiReaction] : ""},
                {"GT3_RACE_REPORT", s.raceReport ? "1" : ""},
                {"GT3_RACE_REPORT_DIR", s.raceReport && s.raceReportSave ? m_root + "\\race-reports" : ""},
                {"GT3_RACE_REPORT_NAMES", s.reportNames == 1 ? "cars" : ""},
                {"GT3_AI_LINES", s.raceLabAi ? m_root + "\\racelab-data\\ai-lines" : ""},
                {"GT3_PLAYER_NAME", s.linkName},
                {"GT3_DRIVER_NAMES", s.driverNames},
                {"GT3_HUD_TOWER_NAMES", kTowerNameValues[s.towerNames]},
                {"GT3_HUD_TOWER_TIME_SWITCH", s.towerTimeSwitch ? "1" : ""},
                {"GT3_VIBRATION", s.vibration ? "" : "0"},
                // (Retired 2026-09-30: the Controls page maps the pad as PCSX2
                // does and GT3's own Key Configuration sets the pedals; the
                // launcher no longer rewrites GT3's controls.)
                {"GT3_PEDALS", ""},
                {"GT3_HUD_SPEED", hudEnv(s, 0)},
                {"GT3_HUD_RPM", hudEnv(s, 1)},
                {"GT3_HUD_GEAR", hudEnv(s, 2)},
                {"GT3_HUD_SHIFT", hudEnv(s, 3)},
                {"GT3_HUD_POSITION", hudEnv(s, 4)},
                {"GT3_HUD_LAP", hudEnv(s, 5)},
                {"GT3_HUD_TIMES", hudEnv(s, 6)},
                {"GT3_HUD_MAP", hudEnv(s, 7)},
                {"GT3_HUD_TOWER", hudEnv(s, 8)},
                {"GT3_HUD_SPLITS", hudEnv(s, 9)},
                {"GT3_HUD_ZOOMMAP", hudEnv(s, 10)},
                {"GT3_HUD_LIGHTS", hudEnv(s, 11)},
                {"GT3_HUD_ODO", hudEnv(s, 12)},
                {"GT3_RADAR", s.radarMode == 0 ? "0" : (s.radarMode == 1 ? "1" : "always")},
                {"GT3_RADAR_X", std::to_string(s.radarX)},
                {"GT3_RADAR_Y", std::to_string(s.radarY)},
                {"GT3_RADAR_SIZE", std::to_string(s.radarSize)},
                {"GT3_RADAR_RANGE", std::to_string(s.radarRange)},
                {"GT3_RADAR_OPACITY", std::to_string(s.radarOpacity)},
                {"GT3_RADAR_BG_OPACITY", std::to_string(s.radarBgOpacity)},
                {"GT3_RADAR_COLOR_PLAYER", hexColour(s.radarPlayer)},
                {"GT3_RADAR_COLOR_CARS", hexColour(s.radarCars)},
                {"GT3_RADAR_COLOR_NEAR", hexColour(s.radarNear)},
                {"GT3_RADAR_COLOR_BG", hexColour(s.radarBg)},
                {"GT3_CAMERA_VIEWS", cameraViewList(s.cameraViews)},
                {"GT3_CAMERA_START", s.cameraStart >= 0 ? kCameraKeys[s.cameraStart] : ""},
                {"GT3_CAMERA_LOOK", kCameraLookValues[s.cameraLook]},
                {"PS2X_INPUT_PROFILE", full(s.profile)},
                {"GT3_MEMCARD1", full(s.memcard)},
                {"GT3_CRASH_DUMP", logDir + "\\play-latest-crash.dmp"},
                // Graphics changes made here while the game runs (see pageGraphics).
                {"GT3_LIVE_SETTINGS", m_settingsFile},
                // i.LINK Battle over the network (LAN / Online page).
                {"GT3_LINK_MODE", kLinkModeValues[s.linkMode]},
                {"GT3_LINK_HOST", s.linkMode == 2 ? s.linkHost : ""},
                {"GT3_LINK_PORT", s.linkMode != 0 ? std::to_string(s.linkPort) : ""},
                {"GT3_LINK_NAME", s.linkMode != 0 ? s.linkName : ""},
                {"GT3_LINK_STATUS", s.linkMode != 0 ? m_linkStatusFile : ""},
            };
            std::string error;
            // CORE.elf and the drivers: taken from the disc image when missing.
            const std::string gameDir = m_root + "\\windows-diagnostic\\game";
            if (!gameFilesPresent(gameDir))
            {
                if (extractGameFiles(full(m_iso.get()), gameDir, error) == 0 || !gameFilesPresent(gameDir))
                {
                    setStatus("Could not take the game files from the disc image: " + (error.empty() ? std::string("some are missing") : error), kBad);
                    return;
                }
            }
            if (!startGame(m_runner, {gameDir + "\\CORE.elf"}, m_root, env, logDir + "\\play-latest.log", m_game, error))
            {
                setStatus("Could not start the game: " + error, kBad);
                return;
            }
            m_logFile = logDir + "\\play-latest.log";
            setStatus("Game started.", kGood);
            MinimizeWindow();
        }

        void setStatus(const std::string &s, Color c)
        {
            m_status = s;
            m_statusColor = c;
        }

        void pagePlay()
        {
            text("Play", 250, 108, 30, kText, true);
            const Rectangle play{250, 160, 360, 96};
            const bool hot = !m_game.running && CheckCollisionPointRec(GetMousePosition(), play);
            const bool down = hot && IsMouseButtonDown(MOUSE_BUTTON_LEFT);
            DrawRectangleRounded(play, 0.12f, 8,
                                 m_game.running ? kPanelRaised : (down ? kAccentDark : (hot ? Color{86, 146, 255, 255} : kAccent)));
            const char *label = m_game.running ? "RUNNING" : "PLAY";
            const Vector2 size = MeasureTextEx(m_fontBold, label, 44, 2.0f);
            DrawTextEx(m_fontBold, label, Vector2{play.x + (play.width - size.x) / 2, play.y + (play.height - size.y) / 2}, 44, 2.0f,
                       m_game.running ? kMuted : WHITE);
            if (hot && IsMouseButtonReleased(MOUSE_BUTTON_LEFT))
                launch();
            if (m_game.running && GuiButton(Rectangle{630, 188, 170, 40}, "Stop game"))
                stopGame(m_game);
            if (!m_logFile.empty() && GuiButton(Rectangle{820, 188, 170, 40}, "Open game log"))
                shellOpen(m_logFile);

            float y = 290;
            auto line = [&](const char *label, const std::string &value, Color c) {
                text(label, 250, y, 20, kMuted);
                text(fit(value, 680, 20).c_str(), 470, y, 20, c);
                y += 34;
            };
            // A short summary: file names (their folders are on the pages that
            // set them), the picture in one line, the screen in another.
            const auto fileName = [](const std::string &path) {
                const size_t slash = path.find_last_of("\\/");
                return slash == std::string::npos ? path : path.substr(slash + 1);
            };
            const std::string iso = m_iso.get();
            const bool isoFound = fileExists(full(iso));
            line("Game disc", iso.empty() ? "not set (More > Game files)" : fileName(iso) + (isoFound ? "" : "  - not found"), isoFound ? kText : kBad);
            static const char *renderers[] = {"OpenGL", "Software (slow)", "Vulkan"};
            static const char *scales[] = {"native (exact)", "2x", "3x", "4x", "6x", "8x"};
            static const char *filters[] = {"sharp pixels", "bilinear", "nearest", "FSR 1", "NIS", "TV (CRT)"};
            line("Picture", std::string(renderers[m_settings.renderer]) + ", " + (m_settings.renderer != 1 ? scales[m_settings.scale] : scales[0]) +
                                (m_settings.renderer != 1 && m_settings.deinterlace == 4 ? " progressive" : "") + ", " + filters[m_settings.filter] +
                                (m_settings.mirrorDetail == 1 ? ", sharp mirror" : ""),
                 kText);
            static const char *shapes[] = {"4:3", "square pixels", "16:9", "21:9", "Triple screens"};
            static const char *windows[] = {"window", "window 1280x960", "full screen", "across all monitors"};
            std::string screen = std::string(shapes[m_settings.aspect]);
            if (m_settings.aspect == 4)
                screen += m_settings.tripleSides == 1 ? " (angled " + std::to_string(m_settings.tripleAngle) + " degrees)" : " (flat)";
            line("Screen", screen + ", " + windows[m_settings.window], kText);
            const std::string card = m_card.get();
            line("Memory card", card.empty() ? "none" : fileName(card) + (fileExists(full(card)) ? "" : "  (created on first use)"), kText);
            line("Controller", fileName(m_profile.get()), fileExists(full(m_profile.get())) ? kText : kWarn);
            line("HUD units", unitsSummary(), kText);
            const std::string host = m_linkHost.get();
            line("LAN / Online",
                 m_settings.linkMode == 1   ? "Hosting i.LINK Battle on UDP port " + m_linkPort.get()
                 : m_settings.linkMode == 2 ? "Joining " + (host.empty() ? std::string("the first host on this network") : host)
                                            : std::string("Off (i.LINK Battle sees one console)"),
                 kText);
            y += 10;
            paragraph("In game: F3 shows or hides the frame rate. Your controller's buttons are mapped on the Controls page; "
                      "what they do is GT3's own Options > Key Configuration. Close the game window to come back here.",
                      250, y, 880, 19, kMuted);
        }

        void pageGraphics()
        {
            text("Graphics", 250, 108, 30, kText, true);
            // While the game runs, the settings it can change live are saved at
            // once; the game watches this file (GT3_LIVE_SETTINGS) and applies
            // them within half a second. Renderer and Window need a restart.
            text(m_game.running ? "The game is running: changes below apply to it live (Renderer, Window, wide shapes at the next start)."
                                : "Changes apply at start; while the game runs they also apply live. In game, F4 cycles the internal resolution.",
                 440, 116, 17, m_game.running ? kGood : kMuted);
            const float step = 61;
            float y = 150;
            bool live = false, restart = false;
            restart |= choiceRow(y, "Renderer", "OpenGL (GPU);Software (CPU);Vulkan", 3, m_settings.renderer,
                                 "Vulkan: the same picture, fastest at 3x-4x (tested on NVIDIA). OpenGL: the older path. Software: for comparison.");
            y += step;
            if (m_settings.renderer == 1)
                GuiDisable();
            live |= choiceRow(y, "Internal resolution", "Native;2x;3x;4x;6x;8x", 6, m_settings.scale,
                              "Native is exact to the PS2. RTX 5060 laptop: 60 fps up to 4x. 6x / 8x: for strong desktop GPUs (untested there).");
            GuiEnable();
            y += step;
            live |= choiceRow(y, "Display filter", "Sharp;Bilinear;Nearest;FSR 1;NIS;TV", 6, m_settings.filter,
                              "Sharp: crisp. Bilinear: soft. Nearest: blocky. FSR 1 / NIS: upscalers. TV: an old TV's look (F1 in game).");
            y += step;
            text("Sharpening", 250, y + 8, 22, kText);
            if (m_settings.filter < 3 || m_settings.filter == 5)
                GuiDisable();
            float sharpening = static_cast<float>(m_settings.sharpening);
            GuiSlider(Rectangle{470, y + 10, 400, 22}, nullptr, (std::to_string(m_settings.sharpening) + "%").c_str(), &sharpening, 0.0f, 100.0f);
            GuiEnable();
            if (static_cast<int>(sharpening + 0.5f) != m_settings.sharpening)
            {
                m_settings.sharpening = static_cast<int>(sharpening + 0.5f);
                live = true;
            }
            text("FSR 1 / NIS only.", 470, y + 38, 17, kMuted);
            {
                // The frame rate overlay shares this row (the page is full).
                const bool overlayBefore = m_settings.fpsOverlay;
                GuiCheckBox(Rectangle{928, y + 8, 26, 26}, "  Frame rate overlay (F3)", &m_settings.fpsOverlay);
                live |= m_settings.fpsOverlay != overlayBefore;
            }
            y += step;
            {
                // 4:3 <-> square pixels is live; the wide shapes are GT3's
                // widescreen mode with a wider camera, set up at start.
                const int before = m_settings.aspect;
                if (choiceRow(y, "Picture shape", "4:3 (TV);Square px;16:9;21:9;Triple", 5, m_settings.aspect,
                              "16:9: GT3's own widescreen, menus and races (its Screen Size option). 21:9 / Triple: wider races, menus 4:3 (next start)."))
                    (before >= 2 || m_settings.aspect >= 2 ? restart : live) = true;
            }
            y += step;
            restart |= choiceRow(y, "Window", "Fit screen;1280x960;Full screen;Span monitors", 4, m_settings.window,
                                 "Full screen: borderless on this monitor. Span: one borderless window over all monitors.");
            y += step;
            live |= choiceRow(y, "Interlacing", "Adaptive;Weave;Bob;Double;Progressive", 5, m_settings.deinterlace,
                              "Progressive (OpenGL): each field drawn as a whole frame, no combing; any resolution, twice the GPU work.");
            y += step;
            if (m_settings.renderer == 1)
                GuiDisable();
            restart |= choiceRow(y, "Frame generation", "Off;90 fps;120 fps", 3, m_settings.frameGen,
                                 "Experimental: pictures between GT3's 60 from its own motion data (HUD kept still). Adds ~17 ms delay.");
            GuiEnable();
            y += step;
            text("Car, rim and mirror detail, wheel spin blur: the Detail page.", 470, y + 8, 17, kMuted);
            if (m_game.running && (live || restart))
            {
                syncFromBoxes();
                saveOwn();
                if (restart)
                    setStatus("Saved: this change applies the next time the game starts.", kWarn);
                else
                    setStatus("Applied to the running game.", kGood);
            }
        }

        // Detail: how far GT3 keeps its detailed models (cars, wheels) and
        // the rear-view mirror's detail.
        void pageDetail()
        {
            text("Detail", 250, 108, 30, kText, true);
            text(m_game.running ? "The game is running: car, rim and wheel blur apply to it live."
                                : "Changes apply at start; while the game runs, car, rim and wheel blur apply live.",
                 440, 116, 17, m_game.running ? kGood : kMuted);
            const float step = 92;
            float y = 156;
            bool live = false, restart = false;
            live |= choiceRow(y, "Car detail", "GT3's own;2x farther;4x farther;Always full", 4, m_settings.carDetail,
                              "GT3 swaps car bodies to simpler models at a distance (10-20 m for the first step: replays, pre-race views).");
            text("In races GT3 always draws the bodies in full; there the wheels switch (Rim detail).", 470, y + 66, 17, kMuted);
            y += step;
            live |= choiceRow(y, "Rim detail", "GT3's own;2x farther;4x farther;Always full", 4, m_settings.rimDetail,
                              "GT3 swaps wheels to simpler rims and tyres as they shrink on screen: the pop-in on cars in races.");
            text("2x / 4x farther: each rim kept to twice / four times the distance. Always full: no plain wheels.", 470, y + 66, 17, kMuted);
            y += step;
            live |= choiceRow(y, "Wheel spin blur", "GT3's own;Off", 2, m_settings.wheelBlur,
                              "GT3 draws turning rims blurred and a little smaller inside the tyre (a PS2 does too).");
            text("Off: the sharp rim, turning, at every speed (fast wheels may look strobed).", 470, y + 66, 17, kMuted);
            y += step;
            restart |= choiceRow(y, "Mirror detail", "GT3's own;As the view ahead", 2, m_settings.mirrorDetail,
                                 "Sharp rear-view mirror (full models, textures); 60 fps at 4x on the laptop too. Next start.");
            if (m_game.running && (live || restart))
            {
                syncFromBoxes();
                saveOwn();
                if (restart)
                    setStatus("Saved: this change applies the next time the game starts.", kWarn);
                else
                    setStatus("Applied to the running game.", kGood);
            }
        }

        std::string unitsSummary() const
        {
            static const char *speed[] = {"km/h and km", "mph and miles"};
            static const char *power[] = {"PS", "HP", "BHP", "CV", "kW"};
            static const char *torque[] = {"kgf.m", "ft.lb", "Nm"};
            return std::string(speed[m_settings.unitsSpeed]) + ", " + power[m_settings.unitsPower] + ", " + torque[m_settings.unitsTorque];
        }

        // HUD units: written into GT3's own units setting by the game
        // executable (GT3_UNITS_*; live through GT3_LIVE_SETTINGS).
        void pageHud()
        {
            text("HUD", 250, 108, 30, kText, true);
            text(m_game.running ? "The game is running: changes below apply to it live." : "Units GT3 shows in races and on the car screens.",
                 440, 116, 17, m_game.running ? kGood : kMuted);
            const float step = 76;
            float y = 156;
            bool changed = false;
            changed |= choiceRow(y, "Speed and distance", "km/h, km;mph, miles", 2, m_settings.unitsSpeed,
                                 "Speedometer, top speed, course lengths and odometer.");
            y += step;
            changed |= choiceRow(y, "Power", "PS;HP;BHP;CV;kW", 5, m_settings.unitsPower,
                                 "HP, BHP and CV use the same number (GT3 converts from PS); kW is metric.");
            y += step;
            changed |= choiceRow(y, "Torque", "kgf.m;ft.lb;Nm", 3, m_settings.unitsTorque, "kgf.m is GT3's own (Japanese) unit.");
            y += step;
            text("Presets", 250, y + 8, 22, kText);
            // USA, Japan and Europe are sets from GT3's own region table
            // (sub_245820); UK is the usual British spec sheet.
            struct Preset
            {
                const char *name;
                int speed, power, torque;
            };
            static const Preset presets[] = {{"USA (game)", 1, 1, 1}, {"Japan", 0, 0, 0}, {"Europe", 0, 4, 2}, {"UK", 1, 2, 1}};
            float x = 470;
            for (const Preset &p : presets)
            {
                if (GuiButton(Rectangle{x, y, 140, 40}, p.name))
                {
                    m_settings.unitsSpeed = p.speed;
                    m_settings.unitsPower = p.power;
                    m_settings.unitsTorque = p.torque;
                    changed = true;
                }
                x += 150;
            }
            y += 64;
            text(("Now: " + unitsSummary()).c_str(), 470, y, 19, kText);
            y += 40;
            paragraph("GT3 keeps speeds, power and torque in km/h, PS and kgf.m and converts them when it draws them, so the numbers "
                      "change with the labels. The game itself picks them from its language at start (the US game: mph, HP, "
                      "ft.lb); this setting replaces that choice. A change made during a race shows in the numbers at once; the "
                      "speedometer dial (mph 0-200 or km/h 0-320) follows from the next race.",
                      250, y, 880, 18, kMuted);
            y += 104;
            int wideHud = m_settings.hudAnchor ? 0 : 1;
            changed |= choiceRow(y, "Wide HUD", "At the sides;In the middle (4:3)", 2, wideHud,
                                 "16:9 and wider races: position and map at the left, lap times at the right (triple screens: the middle screen's).");
            m_settings.hudAnchor = wideHud == 0 ? 1 : 0;
            y += 80;
            text("GT3's own HUD", 250, y + 4, 22, kText);
            {
                static const char *parts[4] = {"Position & map", "Lap times", "Gauges & lights", "Mirror"};
                static const float xs[4] = {470, 660, 810, 1000};
                for (int i = 0; i < 4; ++i)
                {
                    bool shown = (m_settings.hudHide & (1 << i)) == 0;
                    const bool before = shown;
                    GuiCheckBox(Rectangle{xs[i], y + 6, 24, 24}, (std::string("  ") + parts[i]).c_str(), &shown);
                    if (shown != before)
                    {
                        m_settings.hudHide ^= 1 << i;
                        changed = true;
                    }
                }
                const bool phoneMirror = m_settings.phoneMirror && m_settings.phoneSizes[0] > 0;
                text(phoneMirror && (m_settings.hudHide & 8)
                         ? "Untick a part to hide it in races (live). The mirror stays while the phone shows it (Phone page: Mirror panel)."
                         : "Untick a part to hide it in races (live), e.g. for your own overlays. Countdown and pause messages always stay.",
                     470, y + 38, 16, phoneMirror && (m_settings.hudHide & 8) ? kWarn : kMuted);
            }
            if (changed)
            {
                syncFromBoxes();
                saveOwn();
                if (m_game.running)
                    setStatus("Applied to the running game.", kGood);
            }
        }

        // HUD > Start lights (user 2026-10-02: start lights instead of GT3's "3 2 1 START", lights out at a
        // random moment, "options for everything"): read by the game when it starts (gt3_start_lights.h).
        void pageStartLights()
        {
            text("Start lights", 250, 108, 30, kText, true);
            text(m_game.running ? "Changes here apply from the next time the game starts." : "Grid starts: lights in place of GT3's 3 2 1 START.",
                 470, 116, 17, m_game.running ? kWarn : kMuted);
            bool changed = false;
            float y = 156;
            int mode = m_settings.startLights ? 1 : 0;
            changed |= choiceRow(y, "Grid starts", "GT3's countdown;Start lights", 2, mode,
                                 "Start lights: they come on one a second, then all go out at a random moment - go!");
            m_settings.startLights = mode == 1;
            y += 80;
            int lights = std::clamp(m_settings.startLightsCount, 3, 5) - 3;
            changed |= choiceRow(y, "Lights", "3;4;5", 3, lights, "How many lights come on before they go out.");
            m_settings.startLightsCount = lights + 3;
            y += 80;
            // The random wait after the last light: shortest and longest, in tenths of a second.
            const auto tenthsRow = [&](float row, const char *label, int &value) {
                text(label, 250, row + 2, 20, kText);
                float v = static_cast<float>(value);
                char shown[24];
                std::snprintf(shown, sizeof(shown), "%.1f s", value / 10.0);
                GuiSlider(Rectangle{470, row + 4, 330, 22}, nullptr, shown, &v, 0.0f, 60.0f);
                const int now = static_cast<int>(v + 0.5f);
                if (now == value)
                    return false;
                value = now;
                return true;
            };
            text("Lights out after the last one", 250, y + 2, 22, kText);
            y += 38;
            changed |= tenthsRow(y, "   shortest", m_settings.startWaitMin);
            y += 36;
            changed |= tenthsRow(y, "   longest", m_settings.startWaitMax);
            if (m_settings.startWaitMax < m_settings.startWaitMin)
                m_settings.startWaitMax = m_settings.startWaitMin;
            text("Somewhere in between each start, so you can't time it (F1: 0.2 - 3 s).", 470, y + 34, 17, kMuted);
            y += 76;
            changed |= choiceRow(y, "AI reaction", "Instant;Quick;Normal;Slow", 4, m_settings.startAiReaction,
                                 "Each AI car waits its own random time after the lights go out (Instant: GT3's own, all at once).");
            y += 84;
            // A preview of the panel: the lights coming on and going out, over and over.
            {
                const int count = m_settings.startLightsCount;
                const double t = std::fmod(GetTime(), static_cast<double>(count) + 2.5);
                const int lit = t < count ? static_cast<int>(t) + 1 : (t < count + 1.2 ? count : 0);
                const float cell = 46.0f, r = 15.0f;
                const Rectangle box{470.0f, y, cell * (count + 0.5f), cell * 2.4f};
                DrawRectangleRounded(box, 0.22f, 8, Color{12, 12, 14, 255});
                DrawRectangleRounded(Rectangle{box.x + 4, box.y + 4, box.width - 8, box.height - 8}, 0.2f, 8, Color{26, 26, 30, 255});
                for (int i = 0; i < count; ++i)
                    for (int row = 0; row < 2; ++row)
                    {
                        const bool on = m_settings.startLights && i < lit;
                        const Vector2 c{box.x + cell * 0.25f + cell * (i + 0.5f), box.y + cell * 0.2f + cell * (row + 0.5f)};
                        if (on)
                            DrawCircleV(c, r * 1.55f, Color{255, 40, 20, 70});
                        DrawCircleV(c, r, on ? Color{255, 36, 22, 255} : Color{58, 18, 18, 255});
                        if (on)
                            DrawCircleV(Vector2{c.x - r * 0.3f, c.y - r * 0.3f}, r * 0.28f, Color{255, 170, 150, 200});
                    }
                paragraph(m_settings.startLights
                              ? "Shown where GT3's countdown was. The race clock waits for the lights; the cars and the AI are held on "
                                "the grid until they go out. Rolling starts are unchanged. LAN / Online races: no random wait (every "
                                "PC must start together); replays: the lights go out at GT3's moment."
                              : "Off: GT3's own 3 2 1 START.",
                          box.x + box.width + 30, y, 1150 - (box.x + box.width + 30), 17, kMuted);
            }
            if (changed)
            {
                syncFromBoxes();
                saveOwn();
            }
        }

        // HUD > Race report (user 2026-10-03: "a more advanced end of race screen, with more in depth data,
        // fastest laps for each driver, split times"): read by the game when it starts (gt3_race_report.h).
        void pageRaceReport()
        {
            text("Race report", 250, 108, 30, kText, true);
            text(m_game.running ? "Changes here apply from the next time the game starts." : "An end-of-race screen with the race in detail.",
                 470, 116, 17, m_game.running ? kWarn : kMuted);
            bool changed = false;
            float y = 156;
            int mode = m_settings.raceReport ? 1 : 0;
            changed |= choiceRow(y, "End of race", "GT3's results only;Race report", 2, mode,
                                 "Race report: shown over the picture a few seconds after you finish (F2 in game shows it any time).");
            m_settings.raceReport = mode == 1;
            y += 80;
            int keep = m_settings.raceReportSave ? 1 : 0;
            changed |= choiceRow(y, "Save each report", "No;As a web page", 2, keep, "");
            m_settings.raceReportSave = keep == 1;
            text(("In " + m_root + "\\race-reports (open in any browser).").c_str(), 470, y + 46, 17, kMuted);
            if (m_settings.raceReport && m_settings.raceReportSave && GuiButton(Rectangle{1020, y + 40, 130, 32}, "Open folder"))
            {
                const std::string dir = m_root + "\\race-reports";
                std::error_code ec;
                std::filesystem::create_directories(dir, ec);
                OpenURL(("file:///" + dir).c_str());
            }
            y += 92;
            paragraph("Pages: Results (order, times, gaps, best laps, top speeds, grid places gained, laps led), Lap times, Sectors (the lap "
                      "in thirds: best sectors, ideal lap), Positions (lap by lap) and Your race. In game: F2 shows it any time and turns "
                      "the page, Shift+F2 closes it, Ctrl+F2 switches drivers / cars; the tabs can be clicked.",
                      250, y - 4, 900, 17, kMuted);
            y += 74;
            // Names: the player's (shared with the LAN page) and the AI cars' drivers (GT3 has none).
            text("Names", 250, y, 24, kText, true);
            y += 40;
            auto box = [&](const char *label, PathBox &b, float width, int maxLength) {
                text(label, 250, y + 8, 22, kText);
                if (GuiTextBox(Rectangle{470, y, width, 40}, b.text.data(), maxLength, b.editing))
                {
                    b.editing = !b.editing;
                    if (!b.editing)
                        changed = true;
                }
            };
            box("Your name", m_linkName, 300, 16);
            text("Also your name in LAN / online races.", 790, y + 10, 17, kMuted);
            y += 56;
            box("AI drivers", m_driverNames, 680, 400);
            text("Made-up names, comma-separated, in the AI cars' order (GT3's AI cars have no drivers).", 470, y + 46, 17, kMuted);
            y += 78;
            changed |= choiceRow(y, "Report shows", "Drivers' names;Cars' names", 2, m_settings.reportNames, nullptr);
            y += 56;
            changed |= choiceRow(y, "Timing tower names", "Cars;Drivers;Switching", 3, m_settings.towerNames,
                                 "Switching: drivers' and cars' names in turn, every 5 seconds (Ctrl+F5 in game).");
            y += 80;
            int towerTimes = m_settings.towerTimeSwitch ? 1 : 0;
            changed |= choiceRow(y, "Timing tower times", "As set (F5);Best / last lap in turn", 2, towerTimes,
                                 "In turn: best lap and last lap, every 5 seconds (Shift+F5 in game).");
            m_settings.towerTimeSwitch = towerTimes == 1;
            if (changed)
            {
                syncFromBoxes();
                saveOwn();
            }
        }

        void saveOwn()
        {
            saveSettings(m_settingsFile, m_settings);
            std::error_code ec;
            m_savedStamp = std::filesystem::last_write_time(m_settingsFile, ec);
        }

        // The game writes the radar's settings into this file after a change
        // in game (F8, Shift+F8): taken up here so the launcher's next save
        // keeps them.
        void reloadIfChangedElsewhere()
        {
            if (GetTime() < m_nextReloadCheck)
                return;
            m_nextReloadCheck = GetTime() + 0.5;
            std::error_code ec;
            const auto stamp = std::filesystem::last_write_time(m_settingsFile, ec);
            if (ec || stamp == m_savedStamp)
                return;
            m_savedStamp = stamp;
            const Settings loaded = loadSettings(m_settingsFile, m_settings);
            Settings &s = m_settings;
            s.radarMode = loaded.radarMode;
            s.radarX = loaded.radarX;
            s.radarY = loaded.radarY;
            s.radarSize = loaded.radarSize;
            s.radarRange = loaded.radarRange;
            s.radarOpacity = loaded.radarOpacity;
            s.radarBgOpacity = loaded.radarBgOpacity;
            s.radarPlayer = loaded.radarPlayer;
            s.radarCars = loaded.radarCars;
            s.radarNear = loaded.radarNear;
            s.radarBg = loaded.radarBg;
        }

        static Color rgb(int c, float alpha = 1.0f)
        {
            return Color{static_cast<unsigned char>(c >> 16), static_cast<unsigned char>(c >> 8), static_cast<unsigned char>(c),
                         static_cast<unsigned char>(std::clamp(alpha, 0.0f, 1.0f) * 255.0f)};
        }

        bool sliderRow(float y, const char *label, int &value, int lo, int hi, const char *unit)
        {
            text(label, 250, y + 2, 20, kText);
            float v = static_cast<float>(value);
            GuiSlider(Rectangle{470, y + 4, 330, 22}, nullptr, (std::to_string(value) + unit).c_str(), &v, static_cast<float>(lo),
                      static_cast<float>(hi));
            const int now = static_cast<int>(v + 0.5f);
            if (now == value)
                return false;
            value = now;
            return true;
        }

        bool colourRow(float y, const char *label, int &value)
        {
            static const int palette[] = {0xFFFFFF, 0xFFE65A, 0xFF8C00, 0xFF3B30, 0xFF4FD8, 0xB070FF,
                                          0x3E7EFF, 0x78DCFF, 0x56C880, 0x9AA0AA, 0x000000};
            text(label, 250, y + 4, 20, kText);
            bool changed = false;
            for (int i = 0; i < static_cast<int>(sizeof(palette) / sizeof(palette[0])); ++i)
            {
                const Rectangle r{470 + i * 36.0f, y, 30, 30};
                const bool selected = palette[i] == value;
                const bool hot = CheckCollisionPointRec(GetMousePosition(), r);
                DrawRectangleRounded(r, 0.2f, 4, rgb(palette[i]));
                if (selected || hot)
                    DrawRectangleRoundedLinesEx(r, 0.2f, 4, selected ? 3.0f : 1.0f, selected ? WHITE : kMuted);
                else
                    DrawRectangleRoundedLinesEx(r, 0.2f, 4, 1.0f, kBorder);
                if (hot && !selected && IsMouseButtonReleased(MOUSE_BUTTON_LEFT))
                {
                    value = palette[i];
                    changed = true;
                }
            }
            return changed;
        }

        // The radar as the game draws it (gt3_radar.cpp), over a sketch of a
        // 4:3 race picture, with example cars.
        void radarPreview(Rectangle picture)
        {
            const Settings &s = m_settings;
            // The HUD layout's picture (a race of the user's), else a drawn road.
            if (!drawHudPicture(picture))
            {
                DrawRectangleGradientV(static_cast<int>(picture.x), static_cast<int>(picture.y), static_cast<int>(picture.width),
                                       static_cast<int>(picture.height * 0.45f), Color{118, 160, 205, 255}, Color{190, 205, 215, 255});
                DrawRectangleGradientV(static_cast<int>(picture.x), static_cast<int>(picture.y + picture.height * 0.45f), static_cast<int>(picture.width),
                                       static_cast<int>(picture.height * 0.55f + 1), Color{88, 92, 96, 255}, Color{52, 54, 58, 255});
                DrawTriangle(Vector2{picture.x + picture.width * 0.47f, picture.y + picture.height * 0.45f},
                             Vector2{picture.x + picture.width * 0.2f, picture.y + picture.height},
                             Vector2{picture.x + picture.width * 0.8f, picture.y + picture.height}, Color{70, 72, 76, 255});
            }
            DrawRectangleLinesEx(picture, 1.0f, kBorder);
            if (s.radarMode == 0)
            {
                text("Radar off", picture.x + 12, picture.y + 10, 17, kText);
                return;
            }
            const float size = picture.height * s.radarSize / 100.0f, half = size * 0.5f;
            const float cx = std::clamp(picture.x + picture.width * s.radarX / 100.0f, picture.x + half, picture.x + picture.width - half);
            const float cy = std::clamp(picture.y + picture.height * s.radarY / 100.0f, picture.y + half, picture.y + picture.height - half);
            const float range = static_cast<float>(s.radarRange), k = half / range;
            const float carAlpha = s.radarOpacity / 100.0f, bgAlpha = s.radarBgOpacity / 100.0f;
            const Rectangle box{cx - half, cy - half, size, size};
            DrawRectangleRounded(box, 0.12f, 6, rgb(s.radarBg, bgAlpha));
            DrawRectangleRoundedLines(box, 0.12f, 6, Fade(WHITE, 0.35f * std::max(bgAlpha, carAlpha * 0.5f)));
            const Color guide = Fade(WHITE, 0.18f * std::max(bgAlpha, carAlpha * 0.4f));
            if (range > 10.0f)
                DrawCircleLines(static_cast<int>(cx), static_cast<int>(cy), 10.0f * k, guide);
            DrawLineV(Vector2{cx, box.y + 3}, Vector2{cx, box.y + size - 3}, guide);
            DrawLineV(Vector2{box.x + 3, cy}, Vector2{box.x + size - 3, cy}, guide);
            const float carW = std::max(2.0f, 1.8f * k), carL = std::max(4.0f, 4.4f * k);
            struct Example
            {
                float along, side, heading;
                bool near;
            };
            const Example cars[] = {{range * 0.45f, -range * 0.12f, 0.1f, false}, {1.0f, range * 0.14f, 0.0f, true}, {-range * 0.6f, range * 0.05f, -0.2f, false}};
            beginScissor(box);
            for (const Example &c : cars)
                DrawRectanglePro(Rectangle{cx + c.side * k, cy - c.along * k, carW, carL}, Vector2{carW * 0.5f, carL * 0.5f}, c.heading * 57.29578f,
                                 rgb(c.near ? s.radarNear : s.radarCars, carAlpha));
            DrawRectanglePro(Rectangle{cx, cy, carW, carL}, Vector2{carW * 0.5f, carL * 0.5f}, 0.0f, rgb(s.radarPlayer, std::min(1.0f, carAlpha + 0.1f)));
            EndScissorMode();
        }

        // Proximity radar (gt3_radar.h in the game): GT3's own car positions,
        // drawn over the race picture; live while the game runs.
        void pageRadar()
        {
            text("Radar", 250, 108, 30, kText, true);
            text(m_game.running ? "The game is running: changes below apply to it live." : "The cars around you, drawn over races.", 440, 116,
                 17, m_game.running ? kGood : kMuted);
            Settings &s = m_settings;
            bool changed = false;
            float y = 156;
            changed |= choiceRow(y, "Show", "Off;When cars are near;Always", 3, s.radarMode,
                                 "In game: F8 cycles these. Shift+F8: drag it with the mouse, wheel = size, [ ] = opacity; Shift+F8 again saves.");
            y += 80;
            text("Position", 250, y + 8, 22, kText);
            struct Spot
            {
                const char *name;
                int x, y;
            };
            static const Spot spots[] = {{"Right", 86, 52}, {"Left", 14, 52}, {"Top", 50, 20},
                                         {"Bottom", 50, 74}, {"Top right", 86, 24}, {"Bottom left", 14, 76}};
            for (int i = 0; i < 6; ++i)
            {
                const Rectangle r{470 + (i % 3) * 112.0f, y + (i / 3) * 44.0f, 106, 38};
                if (GuiButton(r, spots[i].name))
                {
                    s.radarX = spots[i].x;
                    s.radarY = spots[i].y;
                    changed = true;
                }
            }
            y += 96;
            changed |= sliderRow(y, "Across", s.radarX, 0, 100, "%");
            y += 38;
            changed |= sliderRow(y, "Down", s.radarY, 0, 100, "%");
            y += 38;
            changed |= sliderRow(y, "Size", s.radarSize, 8, 60, "% of height");
            y += 38;
            changed |= sliderRow(y, "Range", s.radarRange, 10, 80, " m");
            y += 38;
            changed |= sliderRow(y, "Car opacity", s.radarOpacity, 10, 100, "%");
            y += 38;
            changed |= sliderRow(y, "Background", s.radarBgOpacity, 0, 100, "% opacity");
            y += 46;
            changed |= colourRow(y, "Your car", s.radarPlayer);
            y += 40;
            changed |= colourRow(y, "Other cars", s.radarCars);
            y += 40;
            changed |= colourRow(y, "Alongside", s.radarNear);
            y += 40;
            changed |= colourRow(y, "Background", s.radarBg);

            text("Preview", 912, 236, 18, kMuted);
            radarPreview(Rectangle{912, 262, 252, 141.75f});
            if (GuiButton(Rectangle{912, 420, 252, 36}, "Reset radar to defaults"))
            {
                const Settings defaults;
                s.radarMode = defaults.radarMode;
                s.radarX = defaults.radarX;
                s.radarY = defaults.radarY;
                s.radarSize = defaults.radarSize;
                s.radarRange = defaults.radarRange;
                s.radarOpacity = defaults.radarOpacity;
                s.radarBgOpacity = defaults.radarBgOpacity;
                s.radarPlayer = defaults.radarPlayer;
                s.radarCars = defaults.radarCars;
                s.radarNear = defaults.radarNear;
                s.radarBg = defaults.radarBg;
                changed = true;
            }
            paragraph("From GT3's own car positions (races with the timing HUD; not replays). Wide races: placed across the "
                      "whole picture like the HUD; triple screens: on the middle screen.",
                      912, 472, 252, 16, kMuted);
            if (changed)
            {
                syncFromBoxes();
                saveOwn();
                if (m_game.running)
                    setStatus("Applied to the running game.", kGood);
            }
        }

        // Race cameras (gt3_camera.h in the game): which views R1 cycles
        // through, the view a race starts in, the right-stick look-around.
        void pageCamera()
        {
            text("Camera", 250, 108, 30, kText, true);
            text(m_game.running ? "The game is running: changes below apply to it live." : "Race views and looking around.", 440, 116, 17,
                 m_game.running ? kGood : kMuted);
            Settings &s = m_settings;
            bool changed = false;
            float y = 160;
            text("Views", 250, y + 4, 22, kText);
            text("GT3's view button (R1; keyboard E) or F9 / Shift+F9 cycles through the ticked views:", 470, y + 6, 17, kMuted);
            y += 36;
            static const char *names[6] = {"Bumper (GT3's)", "Bonnet", "Roof", "Close chase", "Chase (GT3's)", "Far chase"};
            static const char *notes[6] = {"From the front of the car, as in GT3.",
                                           "From the windscreen: the whole bonnet in view.",
                                           "From the top of the windscreen, over the bonnet.",
                                           "Nearer and higher than GT3's chase view.",
                                           "GT3's own chase view.",
                                           "Half as far again as GT3's chase view."};
            for (int v = 0; v < 6; ++v)
            {
                bool on = (s.cameraViews >> v) & 1;
                const bool before = on;
                GuiCheckBox(Rectangle{470, y + 4, 24, 24}, (std::string("  ") + names[v]).c_str(), &on);
                text(notes[v], 720, y + 7, 16, kMuted);
                if (on != before)
                {
                    s.cameraViews ^= 1 << v;
                    if ((s.cameraViews & 0x3F) == 0)
                        s.cameraViews = 1 << 4; // at least one
                    changed = true;
                }
                y += 34;
            }
            y += 14;
            {
                // -1 GT3's own, then the views.
                static const int startValues[7] = {-1, 0, 1, 2, 3, 4, 5};
                int index = 0;
                for (int i = 0; i < 7; ++i)
                    if (startValues[i] == s.cameraStart)
                        index = i;
                const int before = index;
                changed |= choiceRow(y, "Race starts in", "GT3's own;Bumper;Bonnet;Roof;Close;Chase;Far", 7, index,
                                     "GT3's own: the view GT3 itself starts the race in.");
                if (index != before)
                    s.cameraStart = startValues[index];
            }
            y += 84;
            changed |= choiceRow(y, "Look around", "Off;Left-right only;Full 360", 3, s.cameraLook, nullptr);
            y += 50;
            const char *lookHelp[3] = {
                "The right stick is left to the game.",
                "Push the right stick left or right to look that way (up to about 150 degrees); let go to look ahead again. The game "
                "still gets the right stick too.",
                "The right stick points where you look: up ahead, right to the right, down behind, any angle between; let go to look ahead. "
                "In races the right stick no longer reaches the game (pedals on the triggers: Controls page)."};
            paragraph(lookHelp[s.cameraLook], 470, y, 680, 17, s.cameraLook == 2 ? kWarn : kMuted);
            if (changed)
            {
                syncFromBoxes();
                saveOwn();
                if (m_game.running)
                    setStatus("Applied to the running game.", kGood);
            }
        }

        // The HUD layout editor's canvas shape: the race picture's (4:3; 16:9
        // or 21:9 when races are that wide; the middle screen on triples).
        float hudCanvasAspect() const
        {
            if (m_settings.aspect == 2 || m_settings.aspect == 4)
                return 16.0f / 9.0f;
            if (m_settings.aspect == 3)
                return 21.0f / 9.0f;
            return 4.0f / 3.0f;
        }

        // A widget's box on the editor canvas (as gt3_hud.cpp places it).
        Rectangle hudBoxRect(Rectangle canvas, int w) const
        {
            const auto &b = m_settings.hudBoxes[static_cast<size_t>(w)];
            const float bh = canvas.height * b.size / 100.0f, bw = bh * hudAspect(w, b.style, b.columns);
            const float cx = std::clamp(canvas.x + canvas.width * b.x / 100.0f, canvas.x + bw * 0.5f, canvas.x + canvas.width - bw * 0.5f);
            const float cy = std::clamp(canvas.y + canvas.height * b.y / 100.0f, canvas.y + bh * 0.5f, canvas.y + canvas.height - bh * 0.5f);
            return Rectangle{cx - bw * 0.5f, cy - bh * 0.5f, bw, bh};
        }
        // The radar's box (the Radar page's settings: middle and size).
        Rectangle radarBoxRect(Rectangle canvas) const
        {
            const float size = canvas.height * m_settings.radarSize / 100.0f;
            const float cx = std::clamp(canvas.x + canvas.width * m_settings.radarX / 100.0f, canvas.x + size * 0.5f, canvas.x + canvas.width - size * 0.5f);
            const float cy = std::clamp(canvas.y + canvas.height * m_settings.radarY / 100.0f, canvas.y + size * 0.5f, canvas.y + canvas.height - size * 0.5f);
            return Rectangle{cx - size * 0.5f, cy - size * 0.5f, size, size};
        }

        void centredText(const char *s, float cx, float cy, float size, Color c, bool bold = true)
        {
            const Vector2 m = MeasureTextEx(bold ? m_fontBold : m_font, s, size, 1.0f);
            DrawTextEx(bold ? m_fontBold : m_font, s, Vector2{cx - m.x * 0.5f, cy - m.y * 0.5f}, size, 1.0f, c);
        }

        // A widget with example content, as the game draws it (gt3_hud.cpp),
        // in its style.
        void hudSample(int w, Rectangle r, int alpha, bool selected)
        {
            const Color accent{255, 184, 48, 255}, green{70, 220, 110, 255}, yellow{255, 210, 60, 255}, red{255, 70, 60, 255};
            const Color player{120, 220, 255, 255};
            const int style = w < kHudCount ? m_settings.hudBoxes[static_cast<size_t>(w)].style : 0;
            const bool dial = w == 1 && style == 1;
            DrawRectangleRounded(r, dial ? 0.5f : 0.18f, 6, Fade(BLACK, alpha / 100.0f));
            const float cx = r.x + r.width * 0.5f, cy = r.y + r.height * 0.5f, h = r.height, bw = r.width;
            auto leftText = [&](const char *t, float x, float y, float size, Color c, bool bold) {
                const Vector2 m = MeasureTextEx(bold ? m_fontBold : m_font, t, size, 1.0f);
                DrawTextEx(bold ? m_fontBold : m_font, t, Vector2{x, y - m.y * 0.5f}, size, 1.0f, c);
            };
            auto rightText = [&](const char *t, float x, float y, float size, Color c, bool bold) {
                const Vector2 m = MeasureTextEx(bold ? m_fontBold : m_font, t, size, 1.0f);
                DrawTextEx(bold ? m_fontBold : m_font, t, Vector2{x - m.x, y - m.y * 0.5f}, size, 1.0f, c);
            };
            // A small course outline (the maps), turned by `turn` radians.
            auto course = [&](Vector2 c, float scale, float turn, bool arrow) {
                static const float pts[][2] = {{-0.8f, 0.5f}, {-0.6f, -0.2f}, {-0.2f, -0.6f}, {0.5f, -0.7f}, {0.8f, -0.3f},
                                               {0.4f, 0.0f},  {0.7f, 0.4f},  {0.3f, 0.7f},   {-0.3f, 0.6f}};
                const int n = 9;
                const float cs = std::cos(turn), sn = std::sin(turn);
                auto at = [&](int i) {
                    const float x = pts[i % n][0], y = pts[i % n][1];
                    return Vector2{c.x + (x * cs - y * sn) * scale, c.y + (x * sn + y * cs) * scale};
                };
                for (int i = 0; i < n; ++i)
                    DrawLineEx(at(i), at(i + 1), std::max(2.0f, scale * 0.09f), Fade(WHITE, 0.35f));
                const Vector2 me = at(4), other = at(1);
                // The other cars: their tower colours, their places in the dots.
                const float rad = std::max(4.0f, h * 0.05f);
                DrawCircleV(other, rad, Color{255, 106, 106, 255});
                DrawCircleLinesV(other, rad, Fade(BLACK, 0.55f));
                if (rad >= 5.0f)
                    centredText("1", other.x, other.y, rad * 1.4f, Color{20, 22, 26, 255});
                if (arrow)
                    DrawTriangle(Vector2{me.x, me.y - h * 0.06f}, Vector2{me.x - h * 0.04f, me.y + h * 0.04f}, Vector2{me.x + h * 0.04f, me.y + h * 0.04f},
                                 player);
                else
                    DrawCircleV(me, std::max(2.0f, h * 0.045f), player);
            };
            switch (w)
            {
            case 0:
                centredText("88", cx, r.y + h * 0.42f, h * 0.62f, WHITE);
                centredText(m_settings.unitsSpeed ? "mph" : "km/h", cx, r.y + h * 0.82f, h * 0.2f, accent);
                break;
            case 1:
                if (style == 1)
                {
                    // Dial.
                    const float R = std::min(bw, h) * 0.46f;
                    const Vector2 c{cx, cy + h * 0.03f};
                    auto ang = [](float f) { return 135.0f + 270.0f * f; };
                    DrawRing(c, R * 0.93f, R, ang(0.0f), ang(1.0f), 40, Fade(WHITE, 0.25f));
                    DrawRing(c, R * 0.8f, R, ang(0.8f), ang(1.0f), 16, Fade(red, 0.75f));
                    for (int k = 0; k <= 10; ++k)
                    {
                        const float a = ang(k / 10.0f) * DEG2RAD;
                        DrawLineEx(Vector2{c.x + std::cos(a) * R * 0.78f, c.y + std::sin(a) * R * 0.78f},
                                   Vector2{c.x + std::cos(a) * R * 0.97f, c.y + std::sin(a) * R * 0.97f}, 1.5f, k >= 8 ? red : WHITE);
                    }
                    const float a = ang(0.55f) * DEG2RAD;
                    DrawLineEx(c, Vector2{c.x + std::cos(a) * R * 0.9f, c.y + std::sin(a) * R * 0.9f}, std::max(2.0f, h * 0.025f), Color{255, 90, 50, 255});
                    DrawCircleV(c, std::max(3.0f, h * 0.04f), Color{40, 42, 48, 255});
                    centredText("5500", c.x, c.y + R * 0.52f, h * 0.1f, WHITE);
                }
                else if (style == 2)
                {
                    // Arc.
                    const float R = std::min(bw * 0.5f, h * 0.62f) * 0.95f;
                    const Vector2 c{cx, r.y + h * 0.58f};
                    auto ang = [](float f) { return 150.0f + 240.0f * f; };
                    DrawRing(c, R * 0.74f, R, ang(0.0f), ang(1.0f), 48, Fade(WHITE, 0.12f));
                    DrawRing(c, R * 0.74f, R, ang(0.0f), ang(0.42f), 24, green);
                    DrawRing(c, R * 0.74f, R, ang(0.42f), ang(0.55f), 12, yellow);
                    DrawRing(c, R * 1.02f, R * 1.08f, ang(0.87f), ang(1.0f), 12, red);
                    centredText("5500", c.x, c.y - R * 0.12f, h * 0.26f, WHITE);
                    centredText("rpm", c.x, c.y + R * 0.2f, h * 0.11f, accent);
                }
                else if (style == 3)
                {
                    // LED strip.
                    const int n = 15;
                    const float gap = bw * 0.012f, segW = (bw * 0.94f - gap * (n - 1)) / n;
                    for (int i = 0; i < n; ++i)
                    {
                        const Color on = i < 6 ? green : (i < 11 ? yellow : red);
                        DrawRectangleRounded(Rectangle{r.x + bw * 0.03f + i * (segW + gap), r.y + h * 0.2f, segW, h * 0.6f}, 0.3f, 4,
                                             i < 9 ? on : Fade(on, 0.14f));
                    }
                }
                else if (style == 4)
                {
                    // Digital.
                    rightText("5500", r.x + bw * 0.62f, r.y + h * 0.4f, h * 0.56f, WHITE, true);
                    leftText("rpm", r.x + bw * 0.66f, r.y + h * 0.48f, h * 0.2f, accent, false);
                    const Rectangle bar{r.x + bw * 0.06f, r.y + h * 0.76f, bw * 0.88f, h * 0.1f};
                    DrawRectangleRec(bar, Fade(WHITE, 0.12f));
                    DrawRectangleRec(Rectangle{bar.x + bar.width * 0.84f, bar.y, bar.width * 0.16f, bar.height}, Fade(red, 0.45f));
                    DrawRectangleRec(Rectangle{bar.x, bar.y, bar.width * 0.6f, bar.height}, yellow);
                }
                else
                {
                    const Rectangle bar{r.x + bw * 0.04f, r.y + h * 0.2f, bw * 0.72f, h * 0.6f};
                    DrawRectangleRec(bar, Fade(WHITE, 0.12f));
                    DrawRectangleRec(Rectangle{bar.x + bar.width * 0.84f, bar.y, bar.width * 0.16f, bar.height}, Fade(RED, 0.45f));
                    DrawRectangleRec(Rectangle{bar.x, bar.y, bar.width * 0.6f, bar.height}, WHITE);
                    centredText("5500", r.x + bw * 0.88f, cy - h * 0.1f, h * 0.42f, WHITE);
                }
                break;
            case 2:
                centredText("3", cx, cy, h * 0.85f, accent);
                break;
            case 3:
                for (int i = 0; i < 10; ++i)
                {
                    const Color on = i < 4 ? Color{60, 220, 90, 255} : (i < 7 ? Color{255, 210, 50, 255} : Color{255, 60, 50, 255});
                    DrawCircleV(Vector2{r.x + bw * (0.5f + (i - 4.5f) / 10.8f), cy}, std::min(h * 0.7f, bw / 11.5f) * 0.42f,
                                i < 6 ? on : Fade(on, 0.15f));
                }
                break;
            case 4:
                centredText("3rd", r.x + bw * 0.42f, cy, h * 0.7f, accent);
                centredText("/6", r.x + bw * 0.86f, cy + h * 0.12f, h * 0.34f, kMuted, false);
                break;
            case 5:
                centredText("LAP", r.x + bw * 0.22f, cy, h * 0.36f, kMuted, false);
                rightText("2", r.x + bw * 0.62f, cy, h * 0.7f, WHITE, true);
                leftText("/3", r.x + bw * 0.63f, cy + h * 0.1f, h * 0.4f, kMuted, false);
                break;
            case 6:
                if (style == 2)
                {
                    leftText("LAP", r.x + bw * 0.05f, r.y + h * 0.27f, h * 0.22f, kMuted, false);
                    leftText("0'41.203", r.x + bw * 0.05f, r.y + h * 0.64f, h * 0.4f, WHITE, true);
                    leftText("BEST", r.x + bw * 0.53f, r.y + h * 0.27f, h * 0.22f, kMuted, false);
                    leftText("1'23.905", r.x + bw * 0.53f, r.y + h * 0.64f, h * 0.4f, accent, true);
                }
                else if (style == 1)
                {
                    leftText("LAP", r.x + bw * 0.06f, r.y + h * 0.2f, h * 0.19f, kMuted, false);
                    rightText("0'41.203", r.x + bw * 0.94f, r.y + h * 0.2f, h * 0.24f, WHITE, true);
                    centredText("-0.348", cx, r.y + h * 0.5f, h * 0.3f, green);
                    leftText("BEST", r.x + bw * 0.06f, r.y + h * 0.8f, h * 0.19f, kMuted, false);
                    rightText("1'23.905", r.x + bw * 0.94f, r.y + h * 0.8f, h * 0.24f, accent, true);
                }
                else
                {
                    const char *labels[3] = {"LAP", "LAST", "BEST"};
                    const char *times[3] = {"0'41.203", "1'24.771", "1'23.905"};
                    for (int i = 0; i < 3; ++i)
                    {
                        const float ly = r.y + h * (0.2f + 0.3f * i);
                        leftText(labels[i], r.x + bw * 0.06f, ly, h * 0.19f, kMuted, false);
                        rightText(times[i], r.x + bw * 0.94f, ly, h * 0.24f, i == 2 ? accent : WHITE, true);
                    }
                }
                break;
            case 7:
                course(Vector2{cx, cy}, std::min(bw, h) * 0.4f, style == 1 ? 0.6f : 0.0f, style == 1);
                break;
            case 8:
            {
                // Timing tower.
                const char *names[6] = {"CELICA SS-II", "ALTEZZA RS200", "328ci", "CR-X Del-Sol", "EUNOS ROADSTER", "IMPREZA"};
                if (style >= 3)
                {
                    // The tables (as gt3_hud.cpp draws them): the custom style's
                    // columns, or place, colour, car, lap, time, gap; widths from
                    // gt3_tower_layout.h.
                    const auto &tb = m_settings.hudBoxes[8];
                    const int columns = style == 5 ? tb.columns : (gt3hud::kTowerDefaultColumns | (style == 4 ? gt3hud::ColCarNumber : 0));
                    const bool numbers = (columns & gt3hud::ColCarNumber) != 0;
                    const int timeMode = std::clamp(tb.timeMode, 0, gt3hud::kTowerTimeCount - 1);
                    const Color colours[6] = {{255, 106, 106, 255}, WHITE, {74, 210, 255, 255}, {125, 255, 122, 255}, {255, 162, 74, 255},
                                              {209, 140, 255, 255}};
                    const char *carNumbers[6] = {"Car 3", "Car 1", "Car 2", "Car 4", "Car 6", "Car 5"};
                    const char *gapsT[6] = {"LEAD", "+0.803", "+1.520", "+2.244", "+3.910", "+5.071"};
                    const char *intsT[6] = {"LEAD", "+0.803", "+0.717", "+0.724", "+1.666", "+1.161"};
                    const char *times[3][6] = {{"1'24.771", "1'25.102", "1'25.330", "1'25.918", "1'26.004", "1'26.410"},
                                               {"1'25.013", "1'25.340", "1'25.330", "1'26.102", "1'26.250", "1'26.911"},
                                               {"0'41.310", "0'40.517", "0'40.527", "0'40.537", "0'40.547", "0'40.557"}};
                    static const char *timeHeads[3] = {"BEST LAP", "LAST LAP", "LAP TIME"};
                    float total = gt3hud::kTowerMargin;
                    for (int k = 0; k < gt3hud::kTowerColumnCount; ++k)
                        if (columns & (1 << k))
                            total += gt3hud::towerColumnWidth(k, numbers);
                    const float unit = bw / total, rowH = h / 7.25f, size = rowH * 0.5f, pad = unit * 0.25f;
                    float x0[gt3hud::kTowerColumnCount] = {}, x1[gt3hud::kTowerColumnCount] = {};
                    {
                        float x = r.x + unit * gt3hud::kTowerMargin * 0.5f;
                        for (int k = 0; k < gt3hud::kTowerColumnCount; ++k)
                            if (columns & (1 << k))
                            {
                                x0[k] = x;
                                x += gt3hud::towerColumnWidth(k, numbers) * unit;
                                x1[k] = x;
                            }
                    }
                    auto on = [&](int k) { return (columns & (1 << k)) != 0; };
                    auto mid = [&](int k) { return (x0[k] + x1[k]) * 0.5f; };
                    const float headY = r.y + rowH * 0.6f, headSize = rowH * 0.4f;
                    if (on(0))
                        centredText("POS", mid(0), headY, headSize, kMuted);
                    if (on(2))
                        leftText("CAR", x0[2] + pad, headY, headSize, kMuted, false);
                    if (on(3))
                        centredText("LAP", mid(3), headY, headSize, kMuted);
                    if (on(4))
                        rightText(timeHeads[timeMode], x1[4] - pad, headY, headSize, kMuted, false);
                    if (on(5))
                        rightText("GAP", x1[5] - pad, headY, headSize, kMuted, false);
                    if (on(6))
                        rightText("INT", x1[6] - pad, headY, headSize, kMuted, false);
                    for (int i = 0; i < 6; ++i)
                    {
                        const float ry = r.y + rowH * (1.15f + i), rowMid = ry + rowH * 0.5f;
                        if (i == 1)
                            DrawRectangleRounded(Rectangle{r.x + bw * 0.012f, ry + rowH * 0.05f, bw * 0.976f, rowH * 0.9f}, 0.3f, 4, Fade(player, 0.25f));
                        if (on(0))
                        {
                            const float badgeW = std::min(rowH * 0.95f, (x1[0] - x0[0]) * 0.8f);
                            DrawRectangleRounded(Rectangle{mid(0) - badgeW * 0.5f, ry + rowH * 0.14f, badgeW, rowH * 0.72f}, 0.25f, 4,
                                                 i == 0 ? accent : Color{230, 232, 238, 255});
                            centredText(std::to_string(i + 1).c_str(), mid(0), rowMid, rowH * 0.56f, Color{20, 22, 26, 255});
                        }
                        if (on(1))
                            DrawCircleV(Vector2{mid(1), rowMid}, rowH * 0.2f, colours[i]);
                        if (on(2))
                            leftText(numbers ? carNumbers[i] : names[i], x0[2] + pad, rowMid, size, WHITE, true);
                        if (on(3))
                            centredText("2", mid(3), rowMid, size, WHITE);
                        if (on(4))
                            rightText(times[timeMode][i], x1[4] - pad, rowMid, size, timeMode == 0 && i == 0 ? Color{200, 130, 255, 255} : WHITE, true);
                        if (on(5))
                            rightText(gapsT[i], x1[5] - pad, rowMid, size, i == 0 ? accent : WHITE, true);
                        if (on(6))
                            rightText(intsT[i], x1[6] - pad, rowMid, size, i == 0 ? accent : WHITE, true);
                    }
                    break;
                }
                const char *gaps[3][6] = {{"LEAD", "+0.803", "+1.520", "+2.244", "+3.910", "+5.071"},
                                          {"LEAD", "+0.803", "+0.717", "+0.724", "+1.666", "+1.161"},
                                          {"1'24.771", "1'25.102", "1'25.330", "1'25.918", "1'26.004", "1'26.410"}};
                const float rowH = h / 7.3f;
                leftText("POS", r.x + bw * 0.05f, r.y + rowH * 0.6f, rowH * 0.42f, kMuted, false);
                static const char *heads[3] = {"GAP", "INT", "LAST"};
                rightText(heads[std::clamp(style, 0, 2)], r.x + bw * 0.95f, r.y + rowH * 0.6f, rowH * 0.42f, kMuted, false);
                for (int i = 0; i < 6; ++i)
                {
                    const float ry = r.y + rowH * (1.2f + i), mid = ry + rowH * 0.5f;
                    if (i == 2)
                        DrawRectangleRounded(Rectangle{r.x + bw * 0.02f, ry + rowH * 0.06f, bw * 0.96f, rowH * 0.88f}, 0.3f, 4, Fade(player, 0.22f));
                    const Rectangle badge{r.x + bw * 0.04f, ry + rowH * 0.14f, rowH * 0.95f, rowH * 0.72f};
                    DrawRectangleRounded(badge, 0.25f, 4, i == 0 ? accent : Color{230, 232, 238, 255});
                    centredText(std::to_string(i + 1).c_str(), badge.x + badge.width * 0.5f, mid, rowH * 0.56f, Color{20, 22, 26, 255});
                    // The car's colour (as its map dot), then its name.
                    static const Color dots[6] = {{255, 106, 106, 255}, {74, 210, 255, 255}, WHITE, {125, 255, 122, 255}, {255, 162, 74, 255},
                                                  {209, 140, 255, 255}};
                    const Vector2 dot{badge.x + badge.width + rowH * 0.42f, mid};
                    DrawCircleV(dot, rowH * 0.2f, dots[i]);
                    leftText(names[i], dot.x + rowH * 0.42f, mid, rowH * 0.5f, i == 2 ? player : WHITE, true);
                    rightText(gaps[std::clamp(style, 0, 2)][i], r.x + bw * 0.95f, mid, rowH * 0.5f, i == 0 && style != 2 ? accent : WHITE, true);
                }
                break;
            }
            case 9:
            {
                // Split times.
                const char *values[2][3] = {{"28.412", "31.090", "12.605"}, {"-0.204", "+0.118", "..."}};
                const Color colours[3] = {green, yellow, WHITE};
                for (int k = 0; k < 3; ++k)
                {
                    const float colX = r.x + bw * (0.035f + k * 0.31f), colW = bw * 0.3f, mx = colX + colW * 0.5f;
                    const char label[3] = {'S', static_cast<char>('1' + k), 0};
                    centredText(label, mx, r.y + h * 0.2f, h * 0.2f, kMuted, false);
                    const char *value = values[style == 1 ? 1 : 0][k];
                    float size = h * 0.3f;
                    const float tw = MeasureTextEx(m_fontBold, value, size, 1.0f).x;
                    if (tw > colW * 0.9f)
                        size *= colW * 0.9f / tw;
                    centredText(value, mx, r.y + h * 0.55f, size, colours[k]);
                    DrawRectangleRounded(Rectangle{colX + colW * 0.08f, r.y + h * 0.8f, colW * 0.84f, h * 0.08f}, 0.5f, 4,
                                         k < 2 ? colours[k] : Fade(WHITE, 0.5f));
                }
                break;
            }
            case 11:
            {
                // Lamps: ASM and TCS lit, brake and oil off.
                static const char *names[4] = {"ASM", "TCS", "P", "OIL"};
                const Color lit[4] = {green, yellow, red, Color{255, 150, 40, 255}};
                for (int k = 0; k < 4; ++k)
                {
                    const float mx = r.x + bw * (0.14f + 0.24f * k);
                    const Color c = k < 2 ? lit[k] : Fade(kMuted, 0.3f);
                    if (k == 2)
                        DrawRing(Vector2{mx, cy}, h * 0.26f, h * 0.32f, 0.0f, 360.0f, 32, c);
                    centredText(names[k], mx, cy, k == 2 ? h * 0.4f : h * 0.44f, c);
                }
                break;
            }
            case 12:
            {
                // Odometer: this race's distance.
                centredText("ODO", r.x + bw * 0.16f, cy, h * 0.42f, kMuted);
                centredText("3.1", r.x + bw * 0.58f, cy, h * 0.62f, WHITE);
                centredText("mi", r.x + bw * 0.88f, cy, h * 0.4f, kAccent);
                break;
            }
            case 10:
            {
                // Zoomed map: a stretch of road around the car.
                const float turn = style == 0 ? 0.0f : 0.5f;
                const Vector2 me{cx, style == 0 ? r.y + h * 0.62f : cy};
                const float cs = std::cos(turn), sn = std::sin(turn);
                auto at = [&](float x, float y) { return Vector2{me.x + (x * cs - y * sn) * h, me.y + (x * sn + y * cs) * h}; };
                for (int i = 0; i < 9; ++i)
                {
                    const float y0 = 0.35f - i * 0.1f, y1 = y0 - 0.1f;
                    const float x0 = 0.25f * (y0 < 0 ? y0 * y0 * 2.0f : 0.0f), x1 = 0.25f * (y1 < 0 ? y1 * y1 * 2.0f : 0.0f);
                    const Vector2 a = at(x0 - 0.06f, y0), b = at(x0 + 0.06f, y0), c = at(x1 - 0.06f, y1), d = at(x1 + 0.06f, y1);
                    DrawTriangle(a, b, d, Fade(WHITE, 0.22f));
                    DrawTriangle(a, d, c, Fade(WHITE, 0.22f));
                    DrawTriangle(a, d, b, Fade(WHITE, 0.22f));
                    DrawTriangle(a, c, d, Fade(WHITE, 0.22f));
                    DrawLineEx(a, c, 1.5f, Fade(WHITE, 0.6f));
                    DrawLineEx(b, d, 1.5f, Fade(WHITE, 0.6f));
                }
                const Vector2 tip = at(0.0f, -0.07f), l = at(-0.04f, 0.04f), rr = at(0.04f, 0.04f);
                DrawTriangle(tip, l, rr, player);
                DrawTriangle(tip, rr, l, player);
                break;
            }
            default:
                break;
            }
            if (selected)
                DrawRectangleRoundedLinesEx(r, dial ? 0.5f : 0.18f, 6, 2.0f, kAccent);
        }

        // Our own HUD's layout: drag the boxes over a race picture from the
        // user's game (hud-background.png, saved by the game), the mouse wheel
        // resizes; on / off, size and opacity at the right. Live while the
        // game runs.
        // The pictures for the HUD layout / radar previews: the latest race's
        // ("" in the list), the kept race pictures (hud-pictures, newest
        // first), the user's own if one was chosen.
        std::vector<std::string> hudPictureList() const
        {
            std::vector<std::string> list{""};
            std::vector<std::string> names = filesIn(m_root + "\\hud-pictures", "*.png");
            std::sort(names.rbegin(), names.rend());
            for (const std::string &n : names)
                list.push_back(m_root + "\\hud-pictures\\" + n);
            if (!m_settings.hudPicture.empty() && std::find(list.begin(), list.end(), m_settings.hudPicture) == list.end())
                list.push_back(m_settings.hudPicture);
            return list;
        }
        std::string hudPicturePath() const { return m_settings.hudPicture.empty() ? m_root + "\\hud-background.png" : m_settings.hudPicture; }
        // Loads the chosen picture (again when the file changes).
        void ensureHudPicture()
        {
            const std::string path = hudPicturePath();
            std::error_code ec;
            const auto stamp = std::filesystem::last_write_time(path, ec);
            if (ec)
            {
                if (m_hudBg.id)
                    UnloadTexture(m_hudBg);
                m_hudBg = Texture2D{};
                m_hudBgPath = path;
                return;
            }
            if (path == m_hudBgPath && stamp == m_hudBgStamp)
                return;
            if (m_hudBg.id)
                UnloadTexture(m_hudBg);
            m_hudBg = LoadTexture(path.c_str());
            if (m_hudBg.id)
                SetTextureFilter(m_hudBg, TEXTURE_FILTER_BILINEAR);
            m_hudBgPath = path;
            m_hudBgStamp = stamp;
        }
        // The picture filling r, cropped to its shape; false if there is none.
        bool drawHudPicture(Rectangle r)
        {
            ensureHudPicture();
            if (!m_hudBg.id)
                return false;
            const float aspect = r.width / std::max(1.0f, r.height);
            const float sw = static_cast<float>(m_hudBg.width), sh = static_cast<float>(m_hudBg.height);
            Rectangle src{0, 0, sw, sh};
            if (sw / sh > aspect)
            {
                src.width = sh * aspect;
                src.x = (sw - src.width) * 0.5f;
            }
            else
            {
                src.height = sw / aspect;
                src.y = (sh - src.height) * 0.5f;
            }
            DrawTexturePro(m_hudBg, src, r, Vector2{0, 0}, 0.0f, WHITE);
            return true;
        }
        // Below a preview: < > through the pictures, a file of one's own, back to the latest race.
        bool hudPictureControls(float x, float y, float width)
        {
            Settings &s = m_settings;
            bool changed = false;
            const std::vector<std::string> list = hudPictureList();
            size_t index = 0;
            for (size_t i = 0; i < list.size(); ++i)
                if (list[i] == s.hudPicture)
                    index = i;
            if (GuiButton(Rectangle{x, y, 32, 28}, "<"))
            {
                s.hudPicture = list[(index + list.size() - 1) % list.size()];
                changed = true;
            }
            if (GuiButton(Rectangle{x + 36, y, 32, 28}, ">"))
            {
                s.hudPicture = list[(index + 1) % list.size()];
                changed = true;
            }
            if (GuiButton(Rectangle{x + 72, y, 120, 28}, "Own picture..."))
            {
                std::string path;
                if (browseOpenFile(GetWindowHandle(), "Picture for the HUD layout", "Pictures (PNG)|*.png|Other pictures|*.jpg;*.jpeg;*.bmp|All files|*.*", path))
                {
                    s.hudPicture = path;
                    changed = true;
                }
            }
            if (GuiButton(Rectangle{x + 196, y, 110, 28}, "Latest race"))
            {
                changed |= !s.hudPicture.empty();
                s.hudPicture.clear();
            }
            std::string label = s.hudPicture.empty() ? "The latest race (new each race)"
                                                     : std::filesystem::path(s.hudPicture).filename().string();
            if (list.size() > 1)
                label += "   (" + std::to_string(index + 1) + " of " + std::to_string(list.size()) + ")";
            if (!m_hudBg.id && fileExists(hudPicturePath()))
                label = "Could not read this picture (PNG works)";
            float size = 15.0f;
            const float room = width - 316.0f;
            while (size > 10.0f && MeasureTextEx(m_font, label.c_str(), size, 1.0f).x > room)
                size -= 1.0f;
            text(label.c_str(), x + 316, y + 6, size, kMuted);
            return changed;
        }

        void pageHudLayout()
        {
            text("HUD layout", 250, 108, 30, kText, true);
            text(m_game.running ? "Drag boxes to move them, mouse wheel to resize. Applies live." : "Drag boxes to move them, mouse wheel to resize.",
                 470, 116, 17, m_game.running ? kGood : kMuted);
            Settings &s = m_settings;
            const float aspect = hudCanvasAspect();
            const float cw = std::min(640.0f, 480.0f * aspect), ch = cw / aspect;
            const Rectangle canvas{250, 156 + (480 - ch) * 0.5f, cw, ch};
            // The chosen picture (the latest race's by default).
            if (!drawHudPicture(canvas))
            {
                DrawRectangleGradientV(static_cast<int>(canvas.x), static_cast<int>(canvas.y), static_cast<int>(canvas.width),
                                       static_cast<int>(canvas.height * 0.45f), Color{118, 160, 205, 255}, Color{190, 205, 215, 255});
                DrawRectangleGradientV(static_cast<int>(canvas.x), static_cast<int>(canvas.y + canvas.height * 0.45f), static_cast<int>(canvas.width),
                                       static_cast<int>(canvas.height * 0.55f + 1), Color{88, 92, 96, 255}, Color{52, 54, 58, 255});
                text("Race once and the game saves a picture of it here.", canvas.x + 12, canvas.y + 10, 16, Color{30, 34, 40, 255});
            }
            DrawRectangleLinesEx(canvas, 1.0f, kBorder);

            // Boxes: the radar (index 8) and the widgets, the selected one on top.
            const Vector2 mouse = GetMousePosition();
            const bool radarOn = s.radarMode != 0;
            auto rectOf = [&](int w) { return w == kHudRadar ? radarBoxRect(canvas) : hudBoxRect(canvas, w); };
            auto isOn = [&](int w) { return w == kHudRadar ? radarOn : s.hudBoxes[static_cast<size_t>(w)].on; };
            for (int w = 0; w <= kHudRadar; ++w)
            {
                if (!isOn(w) || w == m_hudSelected)
                    continue;
                if (w == kHudRadar)
                    radarPreviewBox(rectOf(kHudRadar), false);
                else
                    hudSample(w, rectOf(w), s.hudBoxes[static_cast<size_t>(w)].alpha, false);
            }
            if (m_hudSelected >= 0 && isOn(m_hudSelected))
            {
                if (m_hudSelected == kHudRadar)
                    radarPreviewBox(rectOf(kHudRadar), true);
                else
                    hudSample(m_hudSelected, rectOf(m_hudSelected), s.hudBoxes[static_cast<size_t>(m_hudSelected)].alpha, true);
            }
            bool changed = false, commit = false;
            if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT) && CheckCollisionPointRec(mouse, canvas))
            {
                // The topmost box under the mouse: the selected one first.
                int hit = -1;
                if (m_hudSelected >= 0 && isOn(m_hudSelected) && CheckCollisionPointRec(mouse, rectOf(m_hudSelected)))
                    hit = m_hudSelected;
                for (int w = kHudRadar; w >= 0 && hit < 0; --w)
                    if (isOn(w) && CheckCollisionPointRec(mouse, rectOf(w)))
                        hit = w;
                m_hudSelected = hit;
                m_hudDragging = hit >= 0;
                if (hit >= 0)
                {
                    const Rectangle r = rectOf(hit);
                    m_hudGrab = Vector2{mouse.x - (r.x + r.width * 0.5f), mouse.y - (r.y + r.height * 0.5f)};
                }
            }
            if (m_hudDragging && IsMouseButtonDown(MOUSE_BUTTON_LEFT) && m_hudSelected >= 0)
            {
                const int px = std::clamp(static_cast<int>(std::lround((mouse.x - m_hudGrab.x - canvas.x) / canvas.width * 100.0f)), 0, 100);
                const int py = std::clamp(static_cast<int>(std::lround((mouse.y - m_hudGrab.y - canvas.y) / canvas.height * 100.0f)), 0, 100);
                int &bx = m_hudSelected == kHudRadar ? s.radarX : s.hudBoxes[static_cast<size_t>(m_hudSelected)].x;
                int &by = m_hudSelected == kHudRadar ? s.radarY : s.hudBoxes[static_cast<size_t>(m_hudSelected)].y;
                changed |= bx != px || by != py;
                bx = px;
                by = py;
            }
            if (m_hudDragging && IsMouseButtonReleased(MOUSE_BUTTON_LEFT))
            {
                m_hudDragging = false;
                commit = true;
            }
            if (const float wheel = GetMouseWheelMove(); wheel != 0.0f && CheckCollisionPointRec(mouse, canvas))
            {
                for (int w = kHudRadar; w >= 0; --w)
                    if (isOn(w) && CheckCollisionPointRec(mouse, rectOf(w)))
                    {
                        int &size = w == kHudRadar ? s.radarSize : s.hudBoxes[static_cast<size_t>(w)].size;
                        size = std::clamp(size + (wheel > 0 ? 1 : -1), w == kHudRadar ? 8 : 2, 60);
                        m_hudSelected = w;
                        commit = true;
                        break;
                    }
            }

            // The list: on / off per box; the selected box's size, opacity,
            // style and (zoomed map) range.
            const float lx = 912;
            // The custom timing tower's columns, in the list's place while open.
            const bool towerColumns = m_towerColumnsOpen && m_hudSelected == 8 && s.hudBoxes[8].style == 5;
            if (!towerColumns)
                m_towerColumnsOpen = false;
            else
            {
                auto &b = s.hudBoxes[8];
                text("Timing tower columns", lx, 146, 20, kText);
                static const char *names[8] = {"Place", "Colour dot (as on the map)", "Car", "Lap", "Time (best / last / this lap)", "Gap to leader",
                                               "Interval to car ahead", "Car 1..6 instead of names"};
                static const int bits[8] = {gt3hud::ColPlace, gt3hud::ColColour, gt3hud::ColCar, gt3hud::ColLap, gt3hud::ColTime, gt3hud::ColGap,
                                            gt3hud::ColInterval, gt3hud::ColCarNumber};
                for (int k = 0; k < 8; ++k)
                {
                    const float ly = 180 + k * 30.0f;
                    bool on = (b.columns & bits[k]) != 0;
                    const bool before = on;
                    GuiCheckBox(Rectangle{lx, ly, 20, 20}, "", &on);
                    text(names[k], lx + 30, ly, 16, on ? kText : kMuted);
                    if (on != before)
                    {
                        b.columns = on ? (b.columns | bits[k]) : (b.columns & ~bits[k]);
                        commit = true;
                    }
                }
                text("In a race, F5 switches the time", lx, 180 + 8 * 30.0f + 4, 15, kMuted);
                text("between best, last and this lap.", lx, 180 + 8 * 30.0f + 22, 15, kMuted);
                if (GuiButton(Rectangle{lx, 180 + 8 * 30.0f + 50, 240, 28}, "Done"))
                    m_towerColumnsOpen = false;
            }
            if (!towerColumns)
                text("Boxes", lx, 146, 20, kText);
            for (int w = 0; w <= kHudRadar && !towerColumns; ++w)
            {
                const float ly = 174 + w * 24.0f;
                bool on = isOn(w);
                const bool before = on;
                GuiCheckBox(Rectangle{lx, ly, 20, 20}, "", &on);
                const Rectangle label{lx + 28, ly - 2, 212, 24};
                const bool sel = w == m_hudSelected;
                if (sel)
                    DrawRectangleRounded(label, 0.2f, 4, kPanelRaised);
                text(w == kHudRadar ? "Radar" : kHudNames[w], lx + 34, ly, 17, on ? kText : kMuted);
                if (CheckCollisionPointRec(mouse, label) && IsMouseButtonReleased(MOUSE_BUTTON_LEFT))
                    m_hudSelected = w;
                if (on != before)
                {
                    if (w == kHudRadar)
                        s.radarMode = on ? 2 : 0;
                    else
                        s.hudBoxes[static_cast<size_t>(w)].on = on;
                    if (on)
                        m_hudSelected = w;
                    commit = true;
                }
            }
            float y = 174 + (kHudRadar + 1) * 24.0f + 8;
            if (m_hudSelected >= 0)
            {
                text(m_hudSelected == kHudRadar ? "Radar" : kHudNames[m_hudSelected], lx, y, 18, kAccent);
                y += 26;
                int &size = m_hudSelected == kHudRadar ? s.radarSize : s.hudBoxes[static_cast<size_t>(m_hudSelected)].size;
                int &alpha = m_hudSelected == kHudRadar ? s.radarBgOpacity : s.hudBoxes[static_cast<size_t>(m_hudSelected)].alpha;
                auto slider = [&](const char *label, int &value, int lo, int hi) {
                    text(label, lx, y, 16, kMuted);
                    float v = static_cast<float>(value);
                    GuiSlider(Rectangle{lx + 70, y, 140, 18}, nullptr, nullptr, &v, static_cast<float>(lo), static_cast<float>(hi));
                    text(std::to_string(value).c_str(), lx + 218, y, 16, kText);
                    const int now = static_cast<int>(v + 0.5f);
                    if (now != value)
                    {
                        value = now;
                        changed = true;
                        m_hudSliderMoved = true;
                    }
                    y += 26;
                };
                slider("Size", size, m_hudSelected == kHudRadar ? 8 : 2, 60);
                slider("Box", alpha, 0, 100);
                if (m_hudSelected < kHudCount && kHudStyleCount[m_hudSelected] > 1)
                {
                    // Style: < name > (click the arrows or the name).
                    auto &b = s.hudBoxes[static_cast<size_t>(m_hudSelected)];
                    const int n = kHudStyleCount[m_hudSelected];
                    text("Style", lx, y + 3, 16, kMuted);
                    int step = 0;
                    if (GuiButton(Rectangle{lx + 70, y, 26, 24}, "<"))
                        step = -1;
                    if (GuiButton(Rectangle{lx + 240, y, 26, 24}, ">"))
                        step = 1;
                    const Rectangle name{lx + 100, y, 136, 24};
                    DrawRectangleRounded(name, 0.2f, 4, kPanelRaised);
                    centredText(kHudStyleNames[static_cast<size_t>(m_hudSelected)][static_cast<size_t>(b.style)], name.x + name.width * 0.5f,
                                name.y + name.height * 0.5f, 15, kText, false);
                    if (CheckCollisionPointRec(mouse, name) && IsMouseButtonReleased(MOUSE_BUTTON_LEFT))
                        step = 1;
                    if (step != 0)
                    {
                        b.style = (b.style + step + n) % n;
                        commit = true;
                    }
                    y += 30;
                }
                if (m_hudSelected == 10)
                    slider("Range", s.hudBoxes[10].range, 100, 1500);
                if (m_hudSelected == 8 && s.hudBoxes[8].style >= 3)
                {
                    // The tables' time column: < Best lap > (F5 in a race too).
                    auto &b = s.hudBoxes[8];
                    static const char *timeNames[gt3hud::kTowerTimeCount] = {"Best lap", "Last lap", "This lap"};
                    text("Time", lx, y + 3, 16, kMuted);
                    int step = 0;
                    if (GuiButton(Rectangle{lx + 70, y, 26, 24}, "<"))
                        step = -1;
                    if (GuiButton(Rectangle{lx + 240, y, 26, 24}, ">"))
                        step = 1;
                    const Rectangle name{lx + 100, y, 136, 24};
                    DrawRectangleRounded(name, 0.2f, 4, kPanelRaised);
                    centredText(timeNames[std::clamp(b.timeMode, 0, gt3hud::kTowerTimeCount - 1)], name.x + name.width * 0.5f,
                                name.y + name.height * 0.5f, 15, kText, false);
                    if (CheckCollisionPointRec(mouse, name) && IsMouseButtonReleased(MOUSE_BUTTON_LEFT))
                        step = 1;
                    if (step != 0)
                    {
                        b.timeMode = (b.timeMode + step + gt3hud::kTowerTimeCount) % gt3hud::kTowerTimeCount;
                        commit = true;
                    }
                    y += 30;
                    if (b.style == 5)
                    {
                        if (GuiButton(Rectangle{lx, y, 266, 26}, m_towerColumnsOpen ? "Columns (open above)" : "Columns..."))
                            m_towerColumnsOpen = !m_towerColumnsOpen;
                        y += 32;
                    }
                }
            }
            else
            {
                text("Click a box to select it.", lx, y, 16, kMuted);
                y += 28;
            }
            if (m_hudSliderMoved && !IsMouseButtonDown(MOUSE_BUTTON_LEFT))
            {
                m_hudSliderMoved = false;
                commit = true;
            }
            y += 6;
            {
                const bool hidden = s.hudHide == 15;
                if (GuiButton(Rectangle{lx, y, 240, 30}, hidden ? "Show GT3's own HUD" : "Hide GT3's own HUD"))
                {
                    s.hudHide = hidden ? 0 : 15;
                    commit = true;
                }
                y += 36;
                if (GuiButton(Rectangle{lx, y, 240, 30}, "Reset layout"))
                {
                    const Settings defaults;
                    s.hudBoxes = defaults.hudBoxes;
                    commit = true;
                }
            }
            const float pictureY = canvas.y + canvas.height + 10;
            commit |= hudPictureControls(250, pictureY, 640);
            paragraph("GT3's own HUD parts can also be hidden one by one on the HUD page. The radar's colours are on the Radar page. "
                      "The game keeps a picture from each race (hud-pictures folder, the last 16).",
                      250, pictureY + 40, 640, 16, kMuted);
            (void)changed;
            if (commit)
            {
                syncFromBoxes();
                saveOwn();
                if (m_game.running)
                    setStatus("Applied to the running game.", kGood);
            }
        }

        // The radar on the layout canvas: its box and example cars.
        void radarPreviewBox(Rectangle r, bool selected)
        {
            const Settings &s = m_settings;
            DrawRectangleRounded(r, 0.12f, 6, rgb(s.radarBg, s.radarBgOpacity / 100.0f));
            const float cx = r.x + r.width * 0.5f, cy = r.y + r.height * 0.5f, k = r.width / 60.0f;
            DrawLineV(Vector2{cx, r.y + 3}, Vector2{cx, r.y + r.height - 3}, Fade(WHITE, 0.15f));
            DrawLineV(Vector2{r.x + 3, cy}, Vector2{r.x + r.width - 3, cy}, Fade(WHITE, 0.15f));
            const float cwid = std::max(2.0f, 1.8f * k), clen = std::max(4.0f, 4.4f * k);
            DrawRectangleRec(Rectangle{cx + 6 * k - cwid / 2, cy - 12 * k - clen / 2, cwid, clen}, rgb(s.radarCars, s.radarOpacity / 100.0f));
            DrawRectangleRec(Rectangle{cx - cwid / 2, cy - clen / 2, cwid, clen}, rgb(s.radarPlayer, 1.0f));
            if (selected)
                DrawRectangleRoundedLinesEx(r, 0.12f, 6, 2.0f, kAccent);
        }

        // The phone dashboard's panels laid out as its page does
        // (gt3_phone_view.cpp, build()): in order, one column or two (split
        // where half the size is reached); heights by size within a column.
        struct PhonePanel
        {
            int panel;
            Rectangle r;
        };
        std::vector<PhonePanel> phoneLayout(Rectangle screen, bool sideways) const
        {
            const Settings &s = m_settings;
            std::vector<int> list;
            int total = 0;
            for (const int p : s.phoneOrder)
                if (s.phoneSizes[p] > 0)
                {
                    list.push_back(p);
                    total += s.phoneSizes[p];
                }
            int cols = s.phoneArrange == 1 ? 1 : (s.phoneArrange == 2 ? 2 : (sideways ? 2 : 1));
            if (list.size() < 2u)
                cols = 1;
            std::vector<int> column[2];
            int done = 0;
            for (const int p : list)
            {
                column[cols == 2 && done * 2 >= total ? 1 : 0].push_back(p);
                done += s.phoneSizes[p];
            }
            if (cols == 2 && column[1].empty() && column[0].size() > 1u)
            {
                column[1].push_back(column[0].back());
                column[0].pop_back();
            }
            const float gap = 3.0f, width = (screen.width - gap * (cols + 1)) / cols;
            std::vector<PhonePanel> out;
            // The mirror keeps its picture's shape (about 3.6:1) at 60, 80 or
            // 100% of the column's width; the other panels share the rest.
            auto mirrorWidth = [&](int p) { return width * (p == 0 ? 0.4f + 0.2f * s.phoneSizes[0] : 1.0f); };
            for (int c = 0; c < cols; ++c)
            {
                int weight = 0;
                float room = screen.height - gap * (column[c].size() + 1);
                for (const int p : column[c])
                    if (p == 0)
                        room -= mirrorWidth(p) / 3.6f;
                    else
                        weight += s.phoneSizes[p];
                float y = screen.y + gap;
                const float x = screen.x + gap + c * (width + gap);
                for (const int p : column[c])
                {
                    const float w = mirrorWidth(p), h = p == 0 ? w / 3.6f : std::max(0.0f, room) * s.phoneSizes[p] / std::max(1, weight);
                    out.push_back(PhonePanel{p, Rectangle{x + (width - w) / 2, y, w, h}});
                    y += h + gap;
                }
            }
            return out;
        }

        // A phone (upright or sideways) with the dashboard's panels sketched.
        void phonePreview(Rectangle body, bool sideways)
        {
            DrawRectangleRounded(body, 0.12f, 8, Color{8, 9, 12, 255});
            DrawRectangleRoundedLinesEx(body, 0.12f, 8, 2.0f, kBorder);
            const float bezel = 7.0f;
            const Rectangle screen{body.x + bezel, body.y + bezel, body.width - 2 * bezel, body.height - 2 * bezel};
            DrawRectangleRec(screen, Color{11, 13, 16, 255});
            static const Color tints[5] = {{52, 78, 112, 255}, {112, 84, 30, 255}, {40, 58, 78, 255}, {44, 84, 56, 255}, {82, 56, 110, 255}};
            const std::vector<PhonePanel> panels = phoneLayout(screen, sideways);
            if (panels.empty())
                text("No panels", screen.x + 6, screen.y + 6, 14, kMuted);
            for (const PhonePanel &pp : panels)
            {
                const Rectangle r = pp.r;
                DrawRectangleRounded(r, 0.08f, 4, tints[pp.panel]);
                const float cx = r.x + r.width / 2, cy = r.y + r.height / 2, small = std::min(r.width, r.height);
                switch (pp.panel)
                {
                case 0: // mirror: sky over road
                    DrawRectangleGradientV(static_cast<int>(r.x + 3), static_cast<int>(r.y + 3), static_cast<int>(r.width - 6),
                                           static_cast<int>((r.height - 6) * 0.5f), Color{118, 160, 205, 255}, Color{175, 195, 210, 255});
                    DrawRectangle(static_cast<int>(r.x + 3), static_cast<int>(r.y + 3 + (r.height - 6) * 0.5f), static_cast<int>(r.width - 6),
                                  static_cast<int>((r.height - 6) * 0.5f), Color{70, 72, 76, 255});
                    break;
                case 1: // position
                {
                    const float size = std::clamp(small * 0.45f, 9.0f, 34.0f);
                    const Vector2 m = MeasureTextEx(m_fontBold, "3rd", size, 1.0f);
                    DrawTextEx(m_fontBold, "3rd", Vector2{cx - m.x / 2, cy - m.y / 2}, size, 1.0f, Color{255, 184, 48, 255});
                    break;
                }
                case 2: // timing rows
                    for (float ly = r.y + (r.width > 46 && r.height > 22 ? 18 : 6); ly + 3 < r.y + r.height - 3; ly += 7)
                        DrawRectangle(static_cast<int>(r.x + 5), static_cast<int>(ly), static_cast<int>(r.width - 10), 3, Fade(kText, 0.35f));
                    break;
                case 3: // map
                    DrawEllipseLines(static_cast<int>(cx), static_cast<int>(cy), r.width * 0.36f, r.height * 0.32f, Fade(kText, 0.6f));
                    DrawCircleV(Vector2{cx + r.width * 0.36f, cy}, std::max(2.0f, small * 0.06f), WHITE);
                    break;
                default: // radar
                    DrawCircleLines(static_cast<int>(cx), static_cast<int>(cy), small * 0.38f, Fade(kText, 0.35f));
                    DrawRectangleRec(Rectangle{cx - small * 0.04f, cy - small * 0.09f, small * 0.08f, small * 0.18f}, Color{120, 220, 255, 255});
                    DrawRectangleRec(Rectangle{cx + small * 0.12f, cy - small * 0.3f, small * 0.08f, small * 0.18f}, Color{255, 230, 90, 255});
                    break;
                }
                if (r.width > 46 && r.height > 22 && pp.panel != 1)
                    text(kPhonePanelNames[pp.panel], r.x + 4, r.y + 2, 12, WHITE);
            }
        }

        // The phone dashboard (gt3_phone_view.h in the game): the panels a
        // phone or tablet shows, their sizes and order, the arrangement.
        // Layout changes reach a running game's phone page within 2 seconds.
        // "http://<address>:8093/" for m_addresses[i] ("Wi-Fi: 192.168.0.5").
        std::string phoneUrl(size_t i) const
        {
            if (i >= m_addresses.size())
                return "http://<this PC's address>:8093/";
            const size_t space = m_addresses[i].rfind(' ');
            return "http://" + (space == std::string::npos ? m_addresses[i] : m_addresses[i].substr(space + 1)) + ":8093/";
        }

        // The Phone page's QR code: point the phone's camera at it and the
        // dashboard opens (gt3_qr.h). Esc, Close or a click outside closes it.
        void qrOverlay()
        {
            const Rectangle panel{330, 118, 520, 600};
            DrawRectangle(0, 0, kDesignW, kDesignH, Fade(BLACK, 0.6f));
            DrawRectangleRounded(panel, 0.04f, 8, kPanel);
            DrawRectangleRoundedLinesEx(panel, 0.04f, 8, 1.0f, kBorder);
            text("Scan with your phone's camera", panel.x + 24, panel.y + 18, 24, kText, true);
            const std::string url = phoneUrl(m_qrIndex);
            if (url != m_qrText)
            {
                m_qrText = url;
                m_qr = gt3qr::encode(url);
            }
            const float box = 340.0f, bx = panel.x + (panel.width - box) * 0.5f, by = panel.y + 64;
            DrawRectangleRec(Rectangle{bx, by, box, box}, WHITE);
            if (m_qr.size > 0 && !m_addresses.empty())
            {
                const int modules = m_qr.size + 8; // with the 4-module quiet zone
                const float cell = std::floor(box / modules);
                const float ox = bx + std::floor((box - cell * m_qr.size) * 0.5f), oy = by + std::floor((box - cell * m_qr.size) * 0.5f);
                for (int y = 0; y < m_qr.size; ++y)
                    for (int x = 0; x < m_qr.size; ++x)
                        if (m_qr.dark(x, y))
                            DrawRectangleRec(Rectangle{ox + x * cell, oy + y * cell, cell, cell}, BLACK);
            }
            else
                text("No network address found", bx + 60, by + box * 0.5f - 10, 18, kBad);
            const Vector2 size = MeasureTextEx(m_font, url.c_str(), 20, 1.0f);
            text(url.c_str(), panel.x + (panel.width - size.x) * 0.5f, by + box + 14, 20, kText);
            if (m_addresses.size() > 1)
            {
                const std::string which = m_addresses[m_qrIndex].substr(0, m_addresses[m_qrIndex].rfind(' '));
                text(fit(which, 300, 16).c_str(), panel.x + 24, by + box + 50, 16, kMuted);
                if (GuiButton(Rectangle{panel.x + panel.width - 184, by + box + 44, 160, 30}, "Other address"))
                    m_qrIndex = (m_qrIndex + 1) % m_addresses.size();
            }
            paragraph(m_settings.phoneMirror ? "Phone and PC on the same Wi-Fi / network. The page shows the race once you're racing."
                                             : "Turn \"Phone screen\" On first; it applies when the game next starts.",
                      panel.x + 24, by + box + 84, panel.width - 48, 16, m_settings.phoneMirror ? kMuted : kWarn);
            // A click outside closes it - not the release of the click that opened it.
            const bool outside = !m_qrOpenedFrame && IsMouseButtonReleased(MOUSE_BUTTON_LEFT) && !CheckCollisionPointRec(GetMousePosition(), panel);
            m_qrOpenedFrame = false;
            if (GuiButton(Rectangle{panel.x + panel.width - 144, panel.y + panel.height - 56, 120, 38}, "Close") || IsKeyPressed(KEY_ESCAPE) ||
                outside)
                m_qrOpen = false;
        }

        void pagePhone()
        {
            if (m_qrOpen)
                GuiLock();
            text("Phone", 250, 108, 30, kText, true);
            text(m_game.running ? "The game is running: layout changes show on the phone within 2 seconds."
                                : "A second screen on a phone or tablet: choose what it shows.",
                 440, 116, 17, m_game.running ? kGood : kMuted);
            Settings &s = m_settings;
            bool changed = false;
            float y = 156;
            text("Phone screen", 250, y + 2, 22, kText);
            const bool phoneBefore = s.phoneMirror;
            GuiCheckBox(Rectangle{470, y + 4, 26, 26}, "  On (next start)", &s.phoneMirror);
            const bool switched = s.phoneMirror != phoneBefore;
            y += 40;
            {
                std::string where = "Same Wi-Fi / network, in its browser:";
                for (size_t i = 0; i < m_addresses.size() && i < 2; ++i)
                {
                    // "Wi-Fi: 192.168.0.5" -> the address alone.
                    const size_t space = m_addresses[i].rfind(' ');
                    where += "  http://" + (space == std::string::npos ? m_addresses[i] : m_addresses[i].substr(space + 1)) + ":8093/";
                }
                if (m_addresses.empty())
                    where += "  http://<this PC's address>:8093/";
                text(fit(where, 390, 17).c_str(), 470, y, 17, s.phoneMirror ? kText : kMuted);
                if (GuiButton(Rectangle{870, y - 7, 120, 32}, "QR code"))
                {
                    m_qrOpen = true;
                    m_qrIndex = 0;
                    m_qrOpenedFrame = true; // this click's release is not a click outside
                }
            }
            y += 38;
            text("Panels", 250, y + 8, 22, kText);
            text("In order, top to bottom", 250, y + 36, 15, kMuted);
            text("(two columns: left first)", 250, y + 54, 15, kMuted);
            for (int i = 0; i < 5; ++i)
            {
                const int p = s.phoneOrder[i];
                bool on = s.phoneSizes[p] > 0;
                const bool before = on;
                GuiCheckBox(Rectangle{470, y + 8, 24, 24}, (std::string("  ") + kPhonePanelNames[p]).c_str(), &on);
                if (on != before)
                {
                    s.phoneSizes[p] = on ? std::max(1, m_phoneLastSize[p]) : 0;
                    changed = true;
                }
                if (!on)
                    GuiDisable();
                int size = std::max(1, on ? s.phoneSizes[p] : m_phoneLastSize[p]) - 1;
                const int sizeBefore = size;
                GuiToggleGroup(Rectangle{660, y + 2, 78, 36}, "Small;Medium;Large", &size);
                GuiEnable();
                if (on && size != sizeBefore)
                {
                    s.phoneSizes[p] = size + 1;
                    changed = true;
                }
                if (on)
                    m_phoneLastSize[p] = s.phoneSizes[p];
                if (i == 0)
                    GuiDisable();
                if (GuiButton(Rectangle{914, y + 2, 36, 36}, GuiIconText(ICON_ARROW_UP_FILL, nullptr)) && i > 0)
                {
                    std::swap(s.phoneOrder[i], s.phoneOrder[i - 1]);
                    changed = true;
                }
                GuiEnable();
                if (i == 4)
                    GuiDisable();
                if (GuiButton(Rectangle{954, y + 2, 36, 36}, GuiIconText(ICON_ARROW_DOWN_FILL, nullptr)) && i < 4)
                {
                    std::swap(s.phoneOrder[i], s.phoneOrder[i + 1]);
                    changed = true;
                }
                GuiEnable();
                y += 44;
            }
            y += 16;
            text("Arrangement", 250, y + 8, 22, kText);
            {
                const int before = s.phoneArrange;
                GuiToggleGroup(Rectangle{470, y, 140, 40}, "Auto;One column;Two columns", &s.phoneArrange);
                changed |= s.phoneArrange != before;
                static const char *help[3] = {"Auto: one column upright, two when the phone is turned sideways.",
                                              "One column, upright or sideways.", "Two columns, upright or sideways (tablets)."};
                text(help[s.phoneArrange], 470, y + 46, 17, kMuted);
            }
            y += 84;
            changed |= sliderRow(y, "Radar range", s.phoneRadarRange, 10, 100, " m");
            y += 40;
            text("Map", 250, y + 2, 20, kText);
            {
                const bool before = s.phoneMapFallback;
                GuiCheckBox(Rectangle{470, y + 2, 24, 24}, "  If GT3's course is not found, draw it as cars drive", &s.phoneMapFallback);
                changed |= s.phoneMapFallback != before;
            }
            y += 42;
            if (GuiButton(Rectangle{470, y, 200, 40}, "Reset layout"))
            {
                const Settings defaults;
                s.phoneOrder = defaults.phoneOrder;
                s.phoneSizes = defaults.phoneSizes;
                s.phoneArrange = defaults.phoneArrange;
                s.phoneRadarRange = defaults.phoneRadarRange;
                for (int p = 0; p < 5; ++p)
                    m_phoneLastSize[p] = defaults.phoneSizes[p];
                changed = true;
            }
            if (!m_game.running || !s.phoneMirror)
                GuiDisable();
            if (GuiButton(Rectangle{680, y, 220, 40}, "Open on this PC"))
                shellOpen("http://localhost:8093/");
            GuiEnable();
            y += 52;
            paragraph("Mirror: the race's rear-view mirror (bumper and bonnet views). Position: place, lap, lap times. Timing: every "
                      "car's times, gap and interval. Track map: the whole course from GT3's data. Radar: the cars around you. Races only.",
                      470, y, 520, 16, kMuted);

            text("Upright", 1010, 150, 16, kMuted);
            phonePreview(Rectangle{1010, 172, 130, 262}, false);
            text("Sideways", 920, 476, 16, kMuted);
            phonePreview(Rectangle{920, 498, 240, 124}, true);
            if (m_qrOpen)
            {
                GuiUnlock();
                qrOverlay();
            }
            if (changed || switched)
            {
                syncFromBoxes();
                saveOwn();
                if (m_game.running)
                    setStatus(switched ? "Saved: the phone screen switch applies the next time the game starts."
                                       : "Applied: the phone picks the new layout up within 2 seconds.",
                              switched ? kWarn : kGood);
            }
        }

        // A labelled row of narrower choices (the Triple page has a preview at the right).
        bool narrowRow(float y, const char *label, const char *options, int count, int &value, const char *help)
        {
            text(label, 250, y + 8, 22, kText);
            const int before = value;
            GuiToggleGroup(Rectangle{470, y, 420.0f / count - 6.0f, 38}, options, &value);
            if (help)
                text(help, 470, y + 43, 16, kMuted);
            return value != before;
        }

        // Three screens from above (the eyes at the bottom) and from the
        // front (each screen's part of the picture, the gaps hidden).
        void triplePreview(Rectangle area, const TripleGeometry &g)
        {
            const Settings &s = m_settings;
            DrawRectangleRounded(area, 0.04f, 6, kPanel);
            DrawRectangleRoundedLinesEx(area, 0.04f, 6, 1.0f, kBorder);
            text("From above", area.x + 10, area.y + 6, 15, kMuted);
            // Units: centimetres, scaled to fit the area's upper part.
            const float gapCm = s.tripleBezelMm * 0.1f, seat = s.tripleFov == 1 ? static_cast<float>(s.tripleSeatCm) : g.widthCm * 0.5f / std::tan(g.middleDeg * 0.5f / 57.29578f);
            const float angle = (s.tripleSides == 1 ? s.tripleAngle : 0) / 57.29578f;
            const float halfSpan = g.widthCm * 0.5f + gapCm + g.widthCm * std::cos(angle);
            const float depth = seat + 10.0f;
            const Rectangle top{area.x + 12, area.y + 28, area.width - 24, area.height * 0.55f - 34};
            const float k = std::min(top.width / (2.0f * halfSpan), top.height / depth);
            const Vector2 eye{top.x + top.width * 0.5f, top.y + top.height - 4};
            auto at = [&](float xCm, float zCm) { return Vector2{eye.x + xCm * k, eye.y - zCm * k}; };
            // Field of view lines to the outer picture edges.
            const float halfTotal = std::tan(g.totalDeg * 0.5f / 57.29578f);
            beginScissor(top);
            DrawLineEx(eye, at(-halfTotal * seat, seat), 1.0f, Fade(kAccent, 0.6f));
            DrawLineEx(eye, at(halfTotal * seat, seat), 1.0f, Fade(kAccent, 0.6f));
            EndScissorMode();
            // Screens: the middle one, then the sides turned by the angle about their inner edge.
            const Color screen{200, 206, 218, 255};
            DrawLineEx(at(-g.widthCm * 0.5f, seat), at(g.widthCm * 0.5f, seat), 4.0f, screen);
            for (int side = -1; side <= 1; side += 2)
            {
                const float x0 = side * (g.widthCm * 0.5f + gapCm);
                const float x1 = x0 + side * g.widthCm * std::cos(angle), z1 = seat - g.widthCm * std::sin(angle);
                DrawLineEx(at(x0, seat), at(x1, z1), 4.0f, screen);
            }
            DrawCircleV(eye, 4.0f, kWarn);
            // Front view: the picture across three screens, the gaps hidden.
            const Rectangle front{area.x + 12, area.y + area.height * 0.55f + 24, area.width - 24, area.height * 0.45f - 60};
            text("From the front", area.x + 10, front.y - 20, 15, kMuted);
            const float units = 3.0f + 2.0f * g.gap;
            const float w = std::min(front.width / units, front.height * g.aspect), h = w / g.aspect;
            const float fx = front.x + (front.width - w * units) * 0.5f, fy = front.y + (front.height - h) * 0.5f;
            DrawRectangleGradientV(static_cast<int>(fx), static_cast<int>(fy), static_cast<int>(w * units), static_cast<int>(h * 0.5f),
                                   Color{118, 160, 205, 255}, Color{190, 205, 215, 255});
            DrawRectangle(static_cast<int>(fx), static_cast<int>(fy + h * 0.5f), static_cast<int>(w * units), static_cast<int>(h * 0.5f + 1),
                          Color{70, 72, 76, 255});
            DrawLineEx(Vector2{fx, fy + h}, Vector2{fx + w * units * 0.5f, fy + h * 0.5f}, 2.0f, WHITE);
            DrawLineEx(Vector2{fx + w * units, fy + h}, Vector2{fx + w * units * 0.5f, fy + h * 0.5f}, 2.0f, WHITE);
            for (int i = 0; i < 3; ++i)
                DrawRectangleLinesEx(Rectangle{fx + i * w * (1.0f + g.gap), fy, w, h}, 1.5f, screen);
            for (int i = 0; i < 2; ++i)
                if (g.gap > 0.0f)
                    DrawRectangleRec(Rectangle{fx + (i + 1) * w + i * w * g.gap, fy - 3, w * g.gap, h + 6}, Color{8, 9, 12, 255});
        }

        // Triple screens: one picture over three monitors side by side (the
        // window spans them), bezel correction, a field of view matched to
        // the seat. Angled side screens (one camera per screen) are the next
        // step and not built yet.
        void pageTriple()
        {
            text("Triple screens", 250, 108, 30, kText, true);
            text("Races over three monitors. Changes apply at the next start.", 470, 116, 17, kMuted);
            Settings &s = m_settings;
            bool changed = false;
            float y = 156;
            {
                int on = s.aspect == 4 ? 1 : 0;
                if (narrowRow(y, "Triple screens", "Off;On", 2, on, "On: the race picture across three monitors (the window spans them)."))
                {
                    s.aspect = on ? 4 : 0;
                    if (on)
                        s.window = 3;
                    else if (s.window == 3)
                        s.window = 0;
                    changed = true;
                }
            }
            y += 68;
            if (s.aspect != 4)
                GuiDisable();
            changed |= narrowRow(y, "Screens", "16:9;16:10;21:9", 3, s.tripleMonitor, nullptr);
            y += 48;
            changed |= sliderRow(y, "Screen size", s.tripleInches, 17, 65, " inch");
            y += 40;
            changed |= sliderRow(y, "Bezel correction", s.tripleBezelMm, 0, 80, " mm");
            text("Gap between two screens' pictures (both bezels); 0 = off.", 470, y + 28, 15, kMuted);
            y += 60;
            changed |= narrowRow(y, "Field of view", "GT3's own;Matched to me", 2, s.tripleFov, nullptr);
            y += 48;
            if (s.tripleFov != 1)
                GuiDisable();
            changed |= sliderRow(y, "Eyes to screen", s.tripleSeatCm, 30, 200, " cm");
            text("Your eyes to the middle screen: real sizes, angles line up.", 470, y + 28, 15, kMuted);
            if (s.aspect == 4)
                GuiEnable();
            y += 60;
            changed |= narrowRow(y, "Side screens", "Flat;Angled", 2, s.tripleSides, nullptr);
            y += 48;
            if (s.tripleSides != 1)
                GuiDisable();
            changed |= sliderRow(y, "Side angle", s.tripleAngle, 0, 80, " degrees");
            {
                // Side views join the middle one without a seam or overlap when
                // turned by one screen's field of view (screens facing the eyes).
                const TripleGeometry seamless = tripleGeometry(s);
                const int seamlessAngle = std::clamp(static_cast<int>(std::lround(seamless.middleDeg)), 0, 80);
                char label[48];
                std::snprintf(label, sizeof(label), "Seamless: %d", seamlessAngle);
                y += 34;
                text("Turned by one screen's field of view the views join:", 250, y + 5, 15, kMuted);
                if (GuiButton(Rectangle{640, y, 150, 26}, label) && s.tripleAngle != seamlessAngle)
                {
                    s.tripleAngle = seamlessAngle;
                    changed = true;
                }
            }
            y += 40;
            changed |= narrowRow(y, "Side detail", "Full;Mirror (faster)", 2, s.tripleSideDetail, nullptr);
            GuiEnable();
            y += 52;
            const TripleGeometry g = tripleGeometry(s);
            char info[200];
            std::snprintf(info, sizeof(info), "Field of view: middle screen %.0f, all three %.0f degrees across, %.0f up and down%s.", g.middleDeg,
                          g.totalDeg, g.verticalDeg, s.tripleFov == 1 ? "" : " (GT3's own)");
            text(info, 250, y, 17, s.aspect == 4 ? kText : kMuted);
            y += 30;
            paragraph(s.tripleSides == 1
                          ? "Angled: each side screen gets its own view, turned by your screens' angle, so the picture wraps around you. "
                            "GT3 draws the scene three times a frame: test laptop 58-60 fps racing, 51-56 in the busiest start. Mirror "
                            "detail (GT3's rear-view mirror's) is blurrier but about 3 fps faster at 4x."
                          : "Flat: one wide picture, as if the three screens were a single flat panel. The HUD, mirror and radar stay on the "
                            "middle screen.",
                      250, y, 640, 16, kMuted);
            triplePreview(Rectangle{920, 150, 240, 420}, g);
            if (changed)
            {
                syncFromBoxes();
                saveOwn();
                if (m_game.running)
                    setStatus("Saved: triple-screen changes apply the next time the game starts.", kWarn);
            }
        }

        // ---- The controller mapping (Controls page), as PCSX2 maps a pad: each
        // PS2 control gets one of your controller's buttons, triggers or stick
        // directions; GT3's own Options > Key Configuration then decides what
        // the PS2 controls do. Read from and written to the mapping file (the
        // game reads the same file, ps2_input_profile.cpp).
        struct PadBind
        {
            int kind = 0;                 // 0 none, 1 button, 2 axis
            int button = -1;              // raylib gamepad button (1 D-pad up ... 17 right stick press)
            int axis = -1;                // raylib gamepad axis: 0 LX, 1 LY, 2 RX, 3 RY, 4 LT, 5 RT
            float from = 0.0f, to = 1.0f; // the axis at rest and fully on
        };
        PadBind m_pad[kPadTargets];
        // The wheel and pedals (profile lines wheel-steer / wheel-pedal /
        // wheel-button; ps2_input_profile.h). Devices by GLFW GUID: the wheel
        // is the profile's device 1, a separate pedal box device 2.
        struct WheelAxis
        {
            int slot = -1, axis = -1;             // profile device (1 wheel, 2 pedals), axis
            float released = -1.0f, full = 1.0f;  // a pedal's ends (steering: released = the centre)
        };
        struct WheelBinds
        {
            std::string wheelGuid, pedalGuid;
            WheelAxis steer;
            WheelAxis pedal[3];                    // throttle, brake, clutch
            int pedalTarget[3] = {11, 10, -1};     // pressure index (kPressureNames): R2, L2, none
            int buttonSlot[16], button[16];
            WheelBinds()
            {
                for (int i = 0; i < 16; ++i)
                    buttonSlot[i] = button[i] = -1;
            }
        };
        WheelBinds m_wheel;
        int m_controlsTab = 0;          // 0 the controller, 1 wheel & pedals
        int m_wheelCapture = -1;        // 0 steering, 1-3 pedals, 10 + i PS2 button i
        float m_wheelBase[3][8] = {};   // [slot] axes when the capture started
        bool m_wheelBaseButtons[3][128] = {};
        std::vector<std::string> m_padOther; // lines kept as they were (keyboard, rumble, source, stick-dpad, ...)
        std::string m_padLoaded;             // the file the bindings came from
        int m_padCapture = -1;               // the PS2 control waiting for an input
        float m_padBase[8] = {};             // the axes when the capture started
        double m_padCaptureAt = 0.0;
        std::string m_padNote;
        Color m_padNoteColor = kMuted;

        static PadBind padButton(int button)
        {
            PadBind b;
            b.kind = 1;
            b.button = button;
            return b;
        }
        static PadBind padAxis(int axis, float from, float to)
        {
            PadBind b;
            b.kind = 2;
            b.axis = axis;
            b.from = from;
            b.to = to;
            return b;
        }

        // A PS2-pad-like default for an Xbox-style controller: face buttons by
        // position, LB / RB = L1 / R1, the triggers = L2 / R2, sticks as sticks.
        void padDefaults()
        {
            const int buttons[16] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 11, -1, -1, 13, 15, 16, 17};
            for (int t = 0; t < 16; ++t)
                m_pad[t] = buttons[t] >= 0 ? padButton(buttons[t]) : PadBind{};
            m_pad[10] = padAxis(4, -1.0f, 1.0f);
            m_pad[11] = padAxis(5, -1.0f, 1.0f);
            for (int k = 0; k < 4; ++k)
            {
                m_pad[16 + 2 * k] = padAxis(k, 0.0f, -1.0f);
                m_pad[17 + 2 * k] = padAxis(k, 0.0f, 1.0f);
            }
            if (m_padOther.empty())
                m_padOther = {"keyboard 1", "rumble xinput auto 1 1"};
        }

        // The triggers as analog L2 / R2 (GT3's analog pedals once its Key
        // Configuration has Acceleration = R2, Brakes = L2: the game reads their
        // pressure, spread over its range by the game side), the right stick as
        // a stick, the stick presses as L3 / R3.
        void padPedals()
        {
            m_pad[10] = padAxis(4, -1.0f, 1.0f);
            m_pad[11] = padAxis(5, -1.0f, 1.0f);
            m_pad[14] = padButton(16);
            m_pad[15] = padButton(17);
            for (int k = 2; k < 4; ++k)
            {
                m_pad[16 + 2 * k] = padAxis(k, 0.0f, -1.0f);
                m_pad[17 + 2 * k] = padAxis(k, 0.0f, 1.0f);
            }
        }

        static int padIndex(const char *const *names, int count, const std::string &name)
        {
            for (int i = 0; i < count; ++i)
                if (name == names[i])
                    return i;
            return -1;
        }

        void padLoad()
        {
            const std::string path = full(m_profile.get());
            if (path == m_padLoaded)
                return;
            m_padLoaded = path;
            for (PadBind &b : m_pad)
                b = PadBind{};
            m_wheel = WheelBinds{};
            m_padOther.clear();
            std::ifstream in(path);
            if (!in)
            {
                padDefaults();
                return;
            }
            std::string line;
            while (std::getline(in, line))
            {
                std::istringstream row(line.substr(0, line.find('#')));
                std::string kind, name, dir;
                if (!(row >> kind))
                    continue;
                int device = 0, index = -1;
                float a = 0.0f, b = 0.0f, c = 0.0f, dz = 0.0f;
                if (kind == "button")
                {
                    if (row >> name >> device >> index)
                        if (const int t = padIndex(kPadProfileName, 16, name); t >= 0 && index >= 0 && m_pad[t].kind != 2)
                            m_pad[t] = padButton(index);
                }
                else if (kind == "pressure")
                {
                    if (row >> name >> device >> index >> a >> b >> dz)
                        if (const int t = padIndex(kPadProfileName, 12, name); t >= 0 && index >= 0)
                            m_pad[t] = padAxis(index, a, b); // an analog source wins over a button
                }
                else if (kind == "stick")
                {
                    if (row >> name >> device >> index >> a >> c >> b >> dz)
                        if (const int k = padIndex(kPadStick, 4, name); k >= 0 && index >= 0)
                        {
                            m_pad[16 + 2 * k] = padAxis(index, c, a);
                            m_pad[17 + 2 * k] = padAxis(index, c, b);
                        }
                }
                else if (kind == "stick-half")
                {
                    if (row >> name >> dir >> device >> index >> a >> b >> dz)
                    {
                        const int k = padIndex(kPadStick, 4, name);
                        const int half = (dir == "left" || dir == "up") ? 0 : (dir == "right" || dir == "down") ? 1 : -1;
                        if (k >= 0 && half >= 0 && index >= 0)
                            m_pad[16 + 2 * k + half] = padAxis(index, a, b);
                    }
                }
                else if (kind == "wheel-steer")
                {
                    int slot = -1, axis = -1;
                    float lo = -1.0f, centre = 0.0f, hi = 1.0f, lock = 1.0f, gt3Dz = 0.0f;
                    if (row >> slot >> axis >> lo >> centre >> hi >> lock >> gt3Dz)
                        m_wheel.steer = WheelAxis{slot, axis, centre, hi};
                }
                else if (kind == "wheel-pedal")
                {
                    std::string target;
                    int slot = -1, axis = -1;
                    float released = -1.0f, fullAt = 1.0f;
                    if (row >> name >> target >> slot >> axis >> released >> fullAt >> dz)
                    {
                        static const char *pedals[3] = {"throttle", "brake", "clutch"};
                        for (int k = 0; k < 3; ++k)
                            if (name == pedals[k])
                            {
                                m_wheel.pedal[k] = WheelAxis{slot, axis, released, fullAt};
                                m_wheel.pedalTarget[k] = target == "none" ? -1 : padIndex(kPressureNames, 12, target);
                            }
                    }
                }
                else if (kind == "wheel-button")
                {
                    int slot = -1, button = -1;
                    if (row >> name >> slot >> button)
                        if (const int t = padIndex(kPadProfileName, 16, name); t >= 0)
                        {
                            m_wheel.buttonSlot[t] = slot;
                            m_wheel.button[t] = button;
                        }
                }
                else if (kind == "source-guid" && (row >> device >> name >> dir) && (device == 1 || device == 2) && name == "raw")
                    (device == 1 ? m_wheel.wheelGuid : m_wheel.pedalGuid) = dir;
                else
                    m_padOther.push_back(line); // keyboard, rumble, other sources, stick-dpad, ... kept as they were
            }
        }

        void padSave()
        {
            const std::string path = full(m_profile.get());
            if (path.empty())
            {
                m_padNote = "No mapping file set (Paths page).";
                m_padNoteColor = kBad;
                return;
            }
            std::ostringstream out;
            out << "# GT3 controller mapping - written by the launcher's Controls page; the game reads it at start.\n"
                << "# Each PS2 control from your controller (raylib gamepad numbering: buttons 1 D-pad up .. 17 right stick\n"
                << "# press, axes 0 left X, 1 left Y, 2 right X, 3 right Y, 4 left trigger, 5 right trigger). What the PS2\n"
                << "# controls do in the game is GT3's own Options > Key Configuration.\n";
            for (int t = 0; t < 16; ++t)
            {
                const PadBind &b = m_pad[t];
                out << "button " << kPadProfileName[t] << " 0 " << (b.kind == 1 ? b.button : -1) << "\n";
                if (t < 12)
                {
                    if (b.kind == 2)
                        out << "pressure " << kPadProfileName[t] << " 0 " << b.axis << " " << b.from << " " << b.to << " 0.02\n";
                    else
                        out << "pressure " << kPadProfileName[t] << " 0 -1 -1 1 0\n";
                }
            }
            for (int k = 0; k < 4; ++k)
            {
                const PadBind &n = m_pad[16 + 2 * k], &p = m_pad[17 + 2 * k];
                const bool whole = n.kind == 2 && p.kind == 2 && n.axis == p.axis && n.axis < 4 && n.from == p.from &&
                                   (n.to - n.from) * (p.to - p.from) < 0.0f;
                if (whole)
                {
                    out << "stick " << kPadStick[k] << " 0 " << n.axis << " " << n.to << " " << n.from << " " << p.to << " 0.05\n";
                    continue;
                }
                // The directions one by one; the physical right stick stays the
                // 360 camera's (and the game's outside races) when the PS2
                // right stick comes from elsewhere (the triggers as pedals).
                if (k >= 2)
                    out << "stick " << kPadStick[k] << " 0 " << k << " -1 0 1 0.05   # the physical right stick (the 360 camera's)\n";
                else
                    out << "stick " << kPadStick[k] << " 0 -1 -1 0 1 0.05\n";
                const char *dirs[2] = {k % 2 ? "up" : "left", k % 2 ? "down" : "right"};
                for (int h = 0; h < 2; ++h)
                {
                    const PadBind &b = h ? p : n;
                    if (b.kind == 2)
                        out << "stick-half " << kPadStick[k] << " " << dirs[h] << " 0 " << b.axis << " " << b.from << " " << b.to << " 0.02\n";
                }
            }
            if (!m_wheel.wheelGuid.empty() || !m_wheel.pedalGuid.empty())
            {
                out << "# Steering wheel and pedals (Controls page, Wheel & pedals): device 1 the wheel, 2 a separate pedal box;\n"
                    << "# lock = the wheel's rotation / the lock wanted; 0.2913 = GT3's steering dead zone made up for.\n";
                if (!m_wheel.wheelGuid.empty())
                    out << "source-guid 1 raw " << m_wheel.wheelGuid << "\n";
                if (!m_wheel.pedalGuid.empty() && m_wheel.pedalGuid != m_wheel.wheelGuid)
                    out << "source-guid 2 raw " << m_wheel.pedalGuid << "\n";
                const WheelAxis &st = m_wheel.steer;
                if (st.axis >= 0)
                {
                    const float lock = std::clamp(static_cast<float>(m_settings.wheelLock) / static_cast<float>(std::max(1, m_settings.wheelRotation)),
                                                  0.02f, 1.0f);
                    out << "wheel-steer " << st.slot << " " << st.axis << " -1 " << std::clamp(st.released, -0.9f, 0.9f) << " 1 " << lock << " "
                        << (m_settings.wheelDeadzone ? "0.2913" : "0") << "\n";
                }
                static const char *pedals[3] = {"throttle", "brake", "clutch"};
                for (int k = 0; k < 3; ++k)
                {
                    const WheelAxis &a = m_wheel.pedal[k];
                    if (a.axis >= 0 && a.released != a.full)
                        out << "wheel-pedal " << pedals[k] << " " << (m_wheel.pedalTarget[k] >= 0 ? kPressureNames[m_wheel.pedalTarget[k]] : "none")
                            << " " << a.slot << " " << a.axis << " " << a.released << " " << a.full << " 0.02\n";
                }
                for (int t = 0; t < 16; ++t)
                    if (m_wheel.button[t] >= 0)
                        out << "wheel-button " << kPadProfileName[t] << " " << m_wheel.buttonSlot[t] << " " << m_wheel.button[t] << "\n";
            }
            for (const std::string &line : m_padOther)
                out << line << "\n";
            std::ofstream file(path, std::ios::trunc);
            if (!(file << out.str()))
            {
                m_padNote = "Could not write " + path;
                m_padNoteColor = kBad;
                return;
            }
            m_padLoaded = path;
            if (m_game.running)
                setStatus("Mapping saved: it applies the next time the game starts.", kWarn);
        }

        std::string padBindName(const PadBind &b) const
        {
            // Your controller's names: Xbox-style (Xbox, GuliKit, 8BitDo) or
            // PlayStation (DualShock 2 on a USB adapter, DualShock 4 / 5).
            static const char *const xbox[18] = {"?", "D-pad Up", "D-pad Right", "D-pad Down", "D-pad Left", "Y (top)", "B (right)",
                                                 "A (bottom)", "X (left)", "LB", "LT", "RB", "RT", "View / Back", "Guide",
                                                 "Menu / Start", "Left stick press", "Right stick press"};
            static const char *const playstation[18] = {"?", "D-pad Up", "D-pad Right", "D-pad Down", "D-pad Left", "Triangle", "Circle",
                                                        "Cross", "Square", "L1", "L2", "R1", "R2", "Select / Share", "PS",
                                                        "Start / Options", "L3", "R3"};
            const char *const *buttons = m_settings.padStyle == 1 ? playstation : xbox;
            if (b.kind == 1)
                return b.button >= 0 && b.button < 18 ? buttons[b.button] : "Button " + std::to_string(b.button);
            if (b.kind == 2 && m_settings.padStyle == 1 && (b.axis == 4 || b.axis == 5))
                return b.axis == 4 ? "L2 (analog)" : "R2 (analog)";
            if (b.kind == 2)
            {
                const bool negative = b.to < b.from;
                switch (b.axis)
                {
                case 0: return negative ? "Left stick Left" : "Left stick Right";
                case 1: return negative ? "Left stick Up" : "Left stick Down";
                case 2: return negative ? "Right stick Left" : "Right stick Right";
                case 3: return negative ? "Right stick Up" : "Right stick Down";
                case 4: return "Left trigger (LT)";
                case 5: return "Right trigger (RT)";
                default: return "Axis " + std::to_string(b.axis);
                }
            }
            return "-";
        }

        // How far a binding is pressed now (0..1), from controller 0.
        static float padValue(const PadBind &b)
        {
            if (!IsGamepadAvailable(0))
                return 0.0f;
            if (b.kind == 1)
                return IsGamepadButtonDown(0, b.button) ? 1.0f : 0.0f;
            if (b.kind == 2 && b.axis < GetGamepadAxisCount(0) && std::fabs(b.to - b.from) > 0.001f)
                return std::clamp((GetGamepadAxisMovement(0, b.axis) - b.from) / (b.to - b.from), 0.0f, 1.0f);
            return 0.0f;
        }

        void padStartCapture(int target)
        {
            m_padCapture = target;
            m_padCaptureAt = GetTime();
            for (int a = 0; a < 8; ++a)
                m_padBase[a] = IsGamepadAvailable(0) && a < GetGamepadAxisCount(0) ? GetGamepadAxisMovement(0, a) : 0.0f;
            m_padNote = std::string("Press a button, or push a stick or trigger, for PS2 ") + kPadLabel[target] +
                        ".  Esc cancels, Delete clears.";
            m_padNoteColor = kWarn;
        }

        void padFinish(const PadBind &b)
        {
            m_pad[m_padCapture] = b;
            m_padNote = std::string("PS2 ") + kPadLabel[m_padCapture] + " = " + padBindName(b) + ".";
            m_padNoteColor = kGood;
            m_padCapture = -1;
            padSave();
        }

        void padCaptureStep()
        {
            if (m_padCapture < 0)
                return;
            if (IsKeyPressed(KEY_ESCAPE))
            {
                m_padCapture = -1;
                m_padNote = "Cancelled.";
                m_padNoteColor = kMuted;
                return;
            }
            if (IsKeyPressed(KEY_DELETE) || IsKeyPressed(KEY_BACKSPACE))
            {
                const int t = m_padCapture;
                padFinish(PadBind{});
                m_padNote = std::string("PS2 ") + kPadLabel[t] + " cleared.";
                return;
            }
            if (!IsGamepadAvailable(0))
            {
                m_padNote = "No controller found: connect it (the first one is used), then try again.  Esc cancels.";
                m_padNoteColor = kBad;
                return;
            }
            if (GetTime() - m_padCaptureAt < 0.2)
                return;
            const bool stickTarget = m_padCapture >= 16, buttonOnly = m_padCapture >= 12 && m_padCapture < 16;
            // Axes first: the triggers are also reported as buttons (LT / RT).
            for (int a = 0; a < std::min(GetGamepadAxisCount(0), 8); ++a)
            {
                const float v = GetGamepadAxisMovement(0, a);
                if (std::fabs(v - m_padBase[a]) > 0.6f)
                {
                    if (buttonOnly)
                    {
                        m_padNote = std::string("PS2 ") + kPadLabel[m_padCapture] + " takes a button, not a stick or trigger.";
                        m_padNoteColor = kBad;
                        m_padBase[a] = v;
                        return;
                    }
                    const bool trigger = m_padBase[a] < -0.5f; // rests at one end
                    padFinish(padAxis(a, trigger ? -1.0f : 0.0f, trigger ? 1.0f : (v > m_padBase[a] ? 1.0f : -1.0f)));
                    return;
                }
            }
            for (int b = 1; b < 18; ++b)
            {
                if (b == 10 || b == 12 || b == 14 || !IsGamepadButtonPressed(0, b))
                    continue;
                if (stickTarget)
                {
                    m_padNote = "A stick direction takes a stick or a trigger (analog), not a button.";
                    m_padNoteColor = kBad;
                    return;
                }
                padFinish(padButton(b));
                return;
            }
        }

        // One PS2 control on the picture: lit as its binding is pressed, amber
        // while it waits for an input; a click starts the capture.
        void padControl(int t, Rectangle r, bool round, const char *label)
        {
            const float v = padValue(m_pad[t]);
            const bool waiting = m_padCapture == t;
            const bool hot = CheckCollisionPointRec(GetMousePosition(), r);
            Color fill = kPanel;
            fill.r = static_cast<unsigned char>(fill.r + (kAccent.r - fill.r) * v);
            fill.g = static_cast<unsigned char>(fill.g + (kAccent.g - fill.g) * v);
            fill.b = static_cast<unsigned char>(fill.b + (kAccent.b - fill.b) * v);
            const Color edge = waiting ? kWarn : hot ? kText : m_pad[t].kind ? kMuted : kBorder;
            if (round)
            {
                const Vector2 c{r.x + r.width * 0.5f, r.y + r.height * 0.5f};
                DrawCircleV(c, r.width * 0.5f, fill);
                DrawCircleLinesV(c, r.width * 0.5f, edge);
            }
            else
            {
                DrawRectangleRounded(r, 0.35f, 6, fill);
                DrawRectangleRoundedLinesEx(r, 0.35f, 6, waiting ? 2.0f : 1.0f, edge);
            }
            if (label && *label)
            {
                const Vector2 size = MeasureTextEx(m_font, label, 14, 1.0f);
                text(label, r.x + (r.width - size.x) * 0.5f, r.y + (r.height - size.y) * 0.5f, 14, kText);
            }
            if (hot && IsMouseButtonPressed(MOUSE_BUTTON_LEFT))
                padStartCapture(t);
        }

        void pageControls()
        {
            text("Controls", 250, 108, 30, kText, true);
            padLoad();
            padCaptureStep();
            {
                // Your controller's kind (names only; any controller maps the same way).
                text("Your controller", 470, 116, 16, kMuted);
                const char *kinds[2] = {"Xbox-style pad", "PlayStation pad"};
                for (int k = 0; k < 2; ++k)
                {
                    const Rectangle r{590.0f + 210.0f * k, 110, 200, 30};
                    if (GuiButton(r, kinds[k]))
                    {
                        m_controlsTab = 0;
                        if (m_settings.padStyle != k)
                        {
                            m_settings.padStyle = k;
                            syncFromBoxes();
                            saveOwn();
                        }
                    }
                    if (m_controlsTab == 0 && m_settings.padStyle == k) // the chosen one: underlined
                        DrawRectangleRec(Rectangle{r.x, r.y + r.height - 3, r.width, 3}, kAccent);
                }
                const Rectangle wheelTab{1010, 110, 150, 30};
                if (GuiButton(wheelTab, "Wheel & pedals"))
                    m_controlsTab = 1;
                if (m_controlsTab == 1)
                    DrawRectangleRec(Rectangle{wheelTab.x, wheelTab.y + wheelTab.height - 3, wheelTab.width, 3}, kAccent);
            }
            if (m_controlsTab == 1)
            {
                pageWheel();
                return;
            }

            // The DualShock 2.
            const Color body = kPanelRaised;
            DrawCircle(345, 372, 62, body);
            DrawCircle(675, 372, 62, body);
            DrawRectangleRounded(Rectangle{290, 222, 440, 150}, 0.45f, 12, body);
            padControl(10, Rectangle{300, 156, 110, 24}, false, "L2");
            padControl(8, Rectangle{300, 186, 110, 24}, false, "L1");
            padControl(11, Rectangle{610, 156, 110, 24}, false, "R2");
            padControl(9, Rectangle{610, 186, 110, 24}, false, "R1");
            padControl(0, Rectangle{336, 236, 26, 28}, false, "");
            padControl(2, Rectangle{336, 290, 26, 28}, false, "");
            padControl(3, Rectangle{308, 264, 28, 26}, false, "");
            padControl(1, Rectangle{362, 264, 28, 26}, false, "");
            padControl(4, Rectangle{664, 234, 30, 30}, true, "");
            padControl(5, Rectangle{692, 262, 30, 30}, true, "");
            padControl(6, Rectangle{664, 290, 30, 30}, true, "");
            padControl(7, Rectangle{636, 262, 30, 30}, true, "");
            {
                // The face symbols in their PS2 colours.
                DrawTriangleLines(Vector2{679, 241}, Vector2{671, 255}, Vector2{687, 255}, Color{80, 200, 160, 255});
                DrawCircleLines(707, 277, 7, Color{235, 90, 90, 255});
                DrawLineEx(Vector2{673, 298}, Vector2{685, 312}, 2.0f, Color{120, 160, 240, 255});
                DrawLineEx(Vector2{685, 298}, Vector2{673, 312}, 2.0f, Color{120, 160, 240, 255});
                DrawRectangleLines(645, 271, 12, 12, Color{230, 130, 190, 255});
            }
            padControl(12, Rectangle{452, 266, 46, 18}, false, "SEL");
            padControl(13, Rectangle{522, 266, 46, 18}, false, "START");
            const float sticks[2][2] = {{445, 350}, {575, 350}};
            for (int s = 0; s < 2; ++s)
            {
                const float cx = sticks[s][0], cy = sticks[s][1];
                DrawCircle(static_cast<int>(cx), static_cast<int>(cy), 44, kPanel);
                const int base = 16 + 4 * s; // left / right, up / down of this stick
                padControl(base + 0, Rectangle{cx - 42, cy - 10, 22, 20}, false, "<");
                padControl(base + 1, Rectangle{cx + 20, cy - 10, 22, 20}, false, ">");
                padControl(base + 2, Rectangle{cx - 11, cy - 42, 22, 20}, false, "^");
                padControl(base + 3, Rectangle{cx - 11, cy + 22, 22, 20}, false, "v");
                padControl(14 + s, Rectangle{cx - 15, cy - 15, 30, 30}, true, s ? "R3" : "L3");
            }

            // The list: every PS2 control and what drives it; a click maps it.
            const float lx = 790, ly = 148, rowH = 20.0f;
            for (int t = 0; t < kPadTargets; ++t)
            {
                const Rectangle row{lx, ly + t * rowH, 370, rowH - 2};
                const bool hot = CheckCollisionPointRec(GetMousePosition(), row);
                const float v = padValue(m_pad[t]);
                DrawRectangleRec(row, m_padCapture == t ? Color{80, 62, 20, 255} : hot ? kPanelRaised : kPanel);
                if (v > 0.0f)
                    DrawRectangleRec(Rectangle{row.x, row.y + row.height - 3, row.width * v, 3}, kAccent);
                text(kPadLabel[t], row.x + 6, row.y + 2, 15, kText);
                text(m_padCapture == t ? "press..." : padBindName(m_pad[t]).c_str(), row.x + 190, row.y + 2, 15,
                     m_padCapture == t ? kWarn : m_pad[t].kind ? kMuted : kBorder);
                if (hot && IsMouseButtonPressed(MOUSE_BUTTON_LEFT))
                    padStartCapture(t);
            }

            // What the controller sends right now (a trigger or button that
            // never shows here is not reaching the PC).
            float y = 452;
            if (IsGamepadAvailable(0))
            {
                text((std::string("Controller: ") + GetGamepadName(0)).c_str(), 250, y, 16, kText);
                std::string held;
                for (int b = 1; b < 18; ++b)
                    if (b != 10 && b != 12 && IsGamepadButtonDown(0, b))
                        held += (held.empty() ? "" : ", ") + padBindName(padButton(b));
                char axes[160];
                const auto ax = [](int a) { return a < GetGamepadAxisCount(0) ? GetGamepadAxisMovement(0, a) : 0.0f; };
                std::snprintf(axes, sizeof(axes), "Sticks  L %+.2f %+.2f   R %+.2f %+.2f      Triggers  LT %+.2f   RT %+.2f", ax(0), ax(1), ax(2),
                              ax(3), ax(4), ax(5));
                text(axes, 250, y + 20, 15, kMuted);
                text((std::string("Buttons  ") + (held.empty() ? "-" : held)).c_str(), 250, y + 38, 15, kMuted);
            }
            else
                text("No controller found (the first one connected is used).", 250, y, 16, kWarn);

            y = 520;
            {
                bool camera = m_settings.cameraLook == 2;
                const bool before = camera;
                GuiCheckBox(Rectangle{250, y, 24, 24}, "  Right stick = 360 camera in races (not sent to the game)", &camera);
                if (camera != before)
                {
                    m_settings.cameraLook = camera ? 2 : 0;
                    syncFromBoxes();
                    saveOwn();
                    setStatus(m_game.running ? "Saved: applies the next time the game starts." : "Saved.", m_game.running ? kWarn : kGood);
                }
            }
            {
                const bool before = m_settings.vibration;
                GuiCheckBox(Rectangle{250, y + 32, 24, 24}, "  Vibration (applies live)", &m_settings.vibration);
                if (m_settings.vibration != before)
                {
                    syncFromBoxes();
                    saveOwn();
                    if (m_game.running)
                        setStatus(m_settings.vibration ? "Vibration on." : "Vibration off.", kGood);
                }
            }
            y = 594;
            if (GuiButton(Rectangle{250, y, 190, 36}, "Triggers = L2 / R2"))
            {
                padPedals();
                padSave();
                m_padNote = "Left trigger = L2, right trigger = R2, analog; the right stick is the right stick again.";
                m_padNoteColor = kGood;
            }
            if (GuiButton(Rectangle{450, y, 150, 36}, "PS2 defaults"))
            {
                padDefaults();
                padSave();
                m_padNote = "Default mapping: face buttons by position, LB / RB = L1 / R1, the triggers = L2 / R2.";
                m_padNoteColor = kGood;
            }
            if (GuiButton(Rectangle{610, y, 160, 36}, "Mapping file..."))
            {
                std::string path = m_profile.get();
                if (browseOpenFile(GetWindowHandle(), "Controller mapping", "Input profiles|*.profile;*.txt|All files|*.*", path))
                {
                    m_profile.set(path);
                    m_padLoaded.clear(); // read the chosen file
                }
            }
            if (!m_padNote.empty())
                paragraph(m_padNote, 250, 640, 900, 16, m_padNoteColor);
            paragraph(std::string("Click a PS2 control (picture or list), then press the button or move the stick / trigger you want "
                                  "for it. What each PS2 control does in the game is set in GT3 itself: Options > Key Configuration "
                                  "(saved on your memory card). Analog pedals: your triggers are L2 and R2 over their whole travel - "
                                  "in Key Configuration set Acceleration = R2 and Brakes = L2.") +
                          (m_settings.padStyle == 1 ? " A DualShock 2 on a USB adapter: most adapters send L2 / R2 (and the other "
                                                      "buttons) as on / off only, so its pedals are on / off."
                                                    : ""),
                      250, 668, 900, 16, kMuted);
        }

        // ---- Wheel & pedals (raw joysticks through GLFW: DirectInput wheels,
        // pedal boxes; they need no controller mapping).
        struct Joy
        {
            bool present = false, gamepad = false;
            std::string name, guid;
            int axes = 0, buttons = 0;
        };
        static Joy joyInfo(int j)
        {
            Joy info;
#if defined(PS2X_INPUT_GLFW)
            if (!glfwJoystickPresent(j))
                return info;
            info.present = true;
            const char *n = glfwGetJoystickName(j);
            const char *g = glfwGetJoystickGUID(j);
            info.name = n ? n : "unknown";
            info.guid = g ? g : "";
            info.gamepad = glfwJoystickIsGamepad(j) == GLFW_TRUE;
            glfwGetJoystickAxes(j, &info.axes);
            glfwGetJoystickButtons(j, &info.buttons);
#else
            (void)j;
#endif
            return info;
        }
        // The one connected joystick with this GUID (-1: none, or two alike).
        static int joyByGuid(const std::string &guid)
        {
            if (guid.empty())
                return -1;
            int found = -1;
            for (int j = 0; j < 16; ++j)
                if (const Joy info = joyInfo(j); info.present && info.guid == guid)
                {
                    if (found >= 0)
                        return -1;
                    found = j;
                }
            return found;
        }
        static float joyAxis(int j, int a)
        {
#if defined(PS2X_INPUT_GLFW)
            int count = 0;
            const float *axes = j >= 0 ? glfwGetJoystickAxes(j, &count) : nullptr;
            return axes && a >= 0 && a < count ? axes[a] : 0.0f;
#else
            (void)j;
            (void)a;
            return 0.0f;
#endif
        }
        static bool joyButton(int j, int b)
        {
#if defined(PS2X_INPUT_GLFW)
            int count = 0;
            const unsigned char *buttons = j >= 0 ? glfwGetJoystickButtons(j, &count) : nullptr;
            return buttons && b >= 0 && b < count && buttons[b] == GLFW_PRESS;
#else
            (void)j;
            (void)b;
            return false;
#endif
        }
        // The joystick behind a profile device (1 the wheel, 2 the pedal box).
        int wheelJoy(int slot) const
        {
            if (slot == 2 && !m_wheel.pedalGuid.empty())
                return joyByGuid(m_wheel.pedalGuid);
            return joyByGuid(m_wheel.wheelGuid);
        }
        void wheelStartCapture(int target)
        {
            m_wheelCapture = target;
            for (int slot = 1; slot <= 2; ++slot)
            {
                const int j = wheelJoy(slot);
                for (int a = 0; a < 8; ++a)
                    m_wheelBase[slot][a] = joyAxis(j, a);
                for (int b = 0; b < 128; ++b)
                    m_wheelBaseButtons[slot][b] = joyButton(j, b);
            }
        }
        void wheelCaptureStep()
        {
            if (m_wheelCapture < 0)
                return;
            if (IsKeyPressed(KEY_ESCAPE))
            {
                m_wheelCapture = -1;
                return;
            }
            const int slots = (!m_wheel.pedalGuid.empty() && m_wheel.pedalGuid != m_wheel.wheelGuid) ? 2 : 1;
            for (int slot = 1; slot <= slots; ++slot)
            {
                const int j = wheelJoy(slot);
                if (j < 0)
                    continue;
                if (m_wheelCapture >= 10)
                {
                    for (int b = 0; b < 128; ++b)
                        if (joyButton(j, b) && !m_wheelBaseButtons[slot][b])
                        {
                            m_wheel.buttonSlot[m_wheelCapture - 10] = slot;
                            m_wheel.button[m_wheelCapture - 10] = b;
                            m_wheelCapture = -1;
                            padSave();
                            return;
                        }
                    continue;
                }
                for (int a = 0; a < 8; ++a)
                {
                    const float base = m_wheelBase[slot][a], now = joyAxis(j, a);
                    if (std::abs(now - base) < 0.5f)
                        continue;
                    WheelAxis &target = m_wheelCapture == 0 ? m_wheel.steer : m_wheel.pedal[m_wheelCapture - 1];
                    target.slot = slot;
                    target.axis = a;
                    if (m_wheelCapture == 0)
                    {
                        target.released = base; // the wheel straight when the capture started
                        target.full = 1.0f;
                    }
                    else
                    {
                        // A pedal: at rest where it was, fully pressed at the
                        // axis's other end (the way it moved).
                        target.released = std::abs(base) > 0.9f ? std::copysign(1.0f, base) : base;
                        target.full = now > base ? 1.0f : -1.0f;
                    }
                    m_wheelCapture = -1;
                    padSave();
                    return;
                }
            }
        }
        // A pedal's or the wheel's position 0..1 (steering -1..1) now.
        float wheelValue(const WheelAxis &a, bool steering) const
        {
            if (a.axis < 0)
                return 0.0f;
            const float v = joyAxis(wheelJoy(a.slot), a.axis);
            if (steering)
                return std::clamp(v - a.released, -1.0f, 1.0f);
            return a.full != a.released ? std::clamp((v - a.released) / (a.full - a.released), 0.0f, 1.0f) : 0.0f;
        }

        void pageWheel()
        {
            padLoad();
            wheelCaptureStep();
#if !defined(PS2X_INPUT_GLFW)
            text("Wheels are read through GLFW, which this launcher was built without.", 250, 170, 18, kWarn);
            return;
#endif
            // The devices: pick the wheel and the pedals (the same device when
            // the pedals plug into the wheel base).
            text("Devices", 250, 156, 20, kText);
            float y = 184;
            int shown = 0;
            for (int j = 0; j < 16 && shown < 4; ++j)
            {
                const Joy info = joyInfo(j);
                if (!info.present)
                    continue;
                ++shown;
                char line[200];
                std::snprintf(line, sizeof(line), "%s  (%d axes, %d buttons%s)", info.name.c_str(), info.axes, info.buttons,
                              info.gamepad ? ", a controller" : "");
                text(fit(line, 560, 16).c_str(), 250, y + 4, 16, kText);
                const bool isWheel = info.guid == m_wheel.wheelGuid, isPedals = info.guid == m_wheel.pedalGuid ||
                                                                                (m_wheel.pedalGuid.empty() && isWheel);
                if (GuiButton(Rectangle{830, y, 150, 26}, isWheel ? "The wheel" : "Use as the wheel"))
                {
                    m_wheel.wheelGuid = info.guid;
                    padSave();
                }
                if (GuiButton(Rectangle{990, y, 170, 26}, isPedals && !m_wheel.wheelGuid.empty() ? "The pedals" : "Use for the pedals"))
                {
                    m_wheel.pedalGuid = info.guid == m_wheel.wheelGuid ? std::string() : info.guid;
                    padSave();
                }
                if (isWheel)
                    DrawRectangleRec(Rectangle{830, y + 23, 150, 3}, kAccent);
                if (isPedals && !m_wheel.wheelGuid.empty())
                    DrawRectangleRec(Rectangle{990, y + 23, 170, 3}, kAccent);
                y += 30;
            }
            if (shown == 0)
            {
                text("No wheel, pedals or controller found. Plug the wheel base in (and switch it on), then come back here.", 250, y + 4, 16, kWarn);
                y += 30;
            }
            y = std::max(y, 246.0f) + 8;

            // Steering.
            auto bar = [&](float x, float yy, float w, float v, bool centred) {
                DrawRectangleRec(Rectangle{x, yy, w, 8}, kPanel);
                if (centred)
                {
                    const float mid = x + w * 0.5f;
                    DrawRectangleRec(Rectangle{std::min(mid, mid + v * w * 0.5f), yy, std::abs(v) * w * 0.5f, 8}, kAccent);
                    DrawRectangleRec(Rectangle{mid - 1, yy - 2, 2, 12}, kMuted);
                }
                else
                    DrawRectangleRec(Rectangle{x, yy, w * v, 8}, kAccent);
            };
            auto axisName = [&](const WheelAxis &a) -> std::string {
                if (a.axis < 0)
                    return "not set";
                return std::string(a.slot == 2 ? "pedals" : "wheel") + " axis " + std::to_string(a.axis);
            };
            auto assignRow = [&](float yy, const char *label, const WheelAxis &a, int target, bool steering) {
                text(label, 250, yy + 4, 18, kText);
                text(m_wheelCapture == target ? (steering ? "turn the wheel..." : "press the pedal...") : axisName(a).c_str(), 370, yy + 5, 16,
                     m_wheelCapture == target ? kWarn : (a.axis >= 0 ? kMuted : kBorder));
                if (GuiButton(Rectangle{540, yy, 90, 26}, "Assign"))
                    wheelStartCapture(target);
                bar(645, yy + 9, 200, wheelValue(a, steering), steering);
            };
            text("Steering", 250, y, 20, kText);
            y += 28;
            assignRow(y, "Wheel", m_wheel.steer, 0, true);
            {
                bool dz = m_settings.wheelDeadzone;
                GuiCheckBox(Rectangle{870, y + 2, 22, 22}, "  Make up for GT3's dead zone", &dz);
                if (dz != m_settings.wheelDeadzone)
                {
                    m_settings.wheelDeadzone = dz;
                    saveOwn();
                    padSave();
                }
            }
            y += 34;
            {
                auto degrees = [&](float x, float yy, const char *label, int &value, int lo, int hi) {
                    text(label, x, yy + 3, 16, kMuted);
                    float v = static_cast<float>(value);
                    GuiSlider(Rectangle{x + 250, yy + 2, 170, 20}, nullptr, (std::to_string(value) + " deg").c_str(), &v, static_cast<float>(lo),
                              static_cast<float>(hi));
                    const int now = (static_cast<int>(v + 0.5f) + 5) / 10 * 10;
                    if (now != value)
                    {
                        value = now;
                        m_hudSliderMoved = true;
                    }
                };
                degrees(250, y, "Rotation set on your wheel base", m_settings.wheelRotation, 90, 2880);
                degrees(250, y + 28, "Full steering at (lock to lock)", m_settings.wheelLock, 90, 1080);
                if (m_hudSliderMoved && !IsMouseButtonDown(MOUSE_BUTTON_LEFT))
                {
                    m_hudSliderMoved = false;
                    saveOwn();
                    padSave();
                }
            }
            y += 64;

            // Pedals.
            text("Pedals", 250, y, 20, kText);
            y += 28;
            static const char *pedalLabels[3] = {"Throttle", "Brake", "Clutch"};
            for (int k = 0; k < 3; ++k)
            {
                assignRow(y, pedalLabels[k], m_wheel.pedal[k], 1 + k, false);
                // Its PS2 control: < R2 > (pressure buttons; none).
                int &t = m_wheel.pedalTarget[k];
                int step = 0;
                if (GuiButton(Rectangle{870, y, 26, 26}, "<"))
                    step = -1;
                if (GuiButton(Rectangle{1004, y, 26, 26}, ">"))
                    step = 1;
                const Rectangle name{900, y, 100, 26};
                DrawRectangleRounded(name, 0.2f, 4, kPanelRaised);
                centredText(t >= 0 ? kPressureLabels[t] : "none", name.x + name.width * 0.5f, name.y + name.height * 0.5f, 15, kText, false);
                if (step)
                {
                    t = (t + 1 + step + 13) % 13 - 1; // -1 (none), 0..11
                    padSave();
                }
                y += 32;
            }
            y += 6;

            // Buttons: the wheel's (and pedal box's) buttons for PS2 buttons.
            text("Buttons", 250, y, 20, kText);
            text("click one, then press the wheel button for it (Esc cancels)", 350, y + 4, 15, kMuted);
            y += 28;
            for (int t = 0; t < 16; ++t)
            {
                const int col = t % 4, rowN = t / 4;
                const Rectangle cell{250.0f + col * 228.0f, y + rowN * 28.0f, 220, 24};
                const bool hot = CheckCollisionPointRec(GetMousePosition(), cell);
                const bool down = m_wheel.button[t] >= 0 && joyButton(wheelJoy(m_wheel.buttonSlot[t]), m_wheel.button[t]);
                DrawRectangleRec(cell, m_wheelCapture == 10 + t ? Color{80, 62, 20, 255} : down ? Color{40, 80, 50, 255} : hot ? kPanelRaised : kPanel);
                text(kPadLabel[t], cell.x + 6, cell.y + 3, 15, kText);
                const std::string what = m_wheelCapture == 10 + t ? "press..." : (m_wheel.button[t] >= 0 ? "button " + std::to_string(m_wheel.button[t]) : "-");
                text(what.c_str(), cell.x + 158, cell.y + 3, 15, m_wheelCapture == 10 + t ? kWarn : kMuted);
                if (hot && IsMouseButtonPressed(MOUSE_BUTTON_LEFT))
                    wheelStartCapture(10 + t);
                if (hot && IsMouseButtonPressed(MOUSE_BUTTON_RIGHT) && m_wheel.button[t] >= 0)
                {
                    m_wheel.button[t] = -1;
                    padSave();
                }
            }
            y += 4 * 28 + 8;
            paragraph("In GT3: Options > Key Configuration, set Acceleration and Brakes to the pedals' PS2 controls (R2 and L2 here). The "
                      "controller keeps working next to the wheel. Right-click a button to clear it. Force feedback is not done yet.",
                      250, y, 910, 16, kMuted);
        }

        void pageMemoryCard()
        {
            text("Memory card", 250, 108, 30, kText, true);
            bool browse = false;
            pathRow(160, "Slot 1", m_card, "Choose...", &browse);
            if (browse)
            {
                std::string path = m_card.get();
                if (browseSaveFile(GetWindowHandle(), "Memory card image (.ps2)", "PS2 memory cards|*.ps2|All files|*.*", path))
                    m_card.set(path);
            }
            if (GuiButton(Rectangle{470, 214, 250, 40}, "Open card folder"))
                shellOpen(parentDirectory(full(m_card.get())));
            const std::string card = m_card.get();
            text(card.empty() ? "No card: the game runs without saving." : (fileExists(full(card)) ? "Card found." : "A new, formatted card is created on first use."),
                 470, 266, 19, card.empty() ? kWarn : kMuted);
            paragraph("Cards use PCSX2's .ps2 format (8 MB). To continue a PCSX2 career, copy your PCSX2 card over this file while "
                      "the game is closed. Clear the path to play without a card.",
                      250, 310, 880, 19, kMuted);
        }

        // Reads the game's GT3_LINK_STATUS file (key=value lines, "peer=name|ping").
        void readLinkStatus()
        {
            if (GetTime() - m_linkStatusRead < 0.5)
                return;
            m_linkStatusRead = GetTime();
            m_linkStatus.clear();
            m_linkPeers.clear();
            std::ifstream in(m_linkStatusFile);
            std::string line;
            while (std::getline(in, line))
            {
                const size_t eq = line.find('=');
                if (eq == std::string::npos)
                    continue;
                const std::string key = line.substr(0, eq), value = trim(line.substr(eq + 1));
                if (key == "peer")
                    m_linkPeers.push_back(value);
                else
                    m_linkStatus[key] = value;
            }
        }

        void pageLink()
        {
            text("LAN / Online", 250, 108, 30, kText, true);
            text(m_game.running ? "Changes here apply the next time the game starts." : "GT3's i.LINK Battle over a network instead of the PS2's i.LINK cable.",
                 470, 116, 17, m_game.running ? kWarn : kMuted);
            float y = 156;
            choiceRow(y, "This console", "Off (one console);Host a game;Join a game", 3, m_settings.linkMode,
                      m_settings.linkMode == 0   ? "i.LINK Battle then sees only this console."
                      : m_settings.linkMode == 1 ? "The other players join this PC. Start the game here first."
                                                 : "Connect to the player who hosts.");
            y += 76;
            auto box = [&](const char *label, PathBox &b, float width, int maxLength) {
                text(label, 250, y + 8, 22, kText);
                if (GuiTextBox(Rectangle{470, y, width, 40}, b.text.data(), maxLength, b.editing))
                    b.editing = !b.editing;
            };
            if (m_settings.linkMode == 0)
                GuiDisable();
            box("Player name", m_linkName, 300, 16);
            y += 56;
            if (m_settings.linkMode == 2)
            {
                box("Host address", m_linkHost, 300, 64);
                text("Empty: find the host on this network. Or its address: 192.168.1.20, or 203.0.113.5:13940 over the internet.", 470,
                     y + 46, 17, kMuted);
                y += 76;
            }
            box("UDP port", m_linkPort, 120, 6);
            text(m_settings.linkMode == 2 ? "The host's port (13940 unless the host changed it)." : "For internet play, forward this UDP port to this PC on the router.",
                 610, y + 10, 17, kMuted);
            y += 60;
            GuiEnable();
            if (m_settings.linkMode == 1)
            {
                text("This PC's addresses (for the players who join):", 250, y, 19, kMuted);
                y += 28;
                for (size_t i = 0; i < m_addresses.size() && i < 4; ++i, y += 26)
                    text(m_addresses[i].c_str(), 470, y, 19, kText);
                if (m_addresses.empty())
                {
                    text("none found: is the network connected?", 470, y, 19, kWarn);
                    y += 26;
                }
                y += 8;
            }
            if (m_game.running && m_settings.linkMode != 0)
            {
                readLinkStatus();
                const std::string state = m_linkStatus.count("state") ? m_linkStatus["state"] : "starting";
                std::string line = state == "hosting"      ? "Hosting on port " + m_linkStatus["port"]
                                   : state == "connected"  ? "Connected to " + m_linkStatus["host"]
                                   : state == "connecting" ? "Connecting to " + m_linkStatus["host"] + "..."
                                   : state == "searching"  ? "Looking for a host on this network..."
                                   : state == "waiting"    ? "The host is in a race: joining when it ends..."
                                   : state == "refused"    ? "The host refused this console (full, or a different game version)."
                                                           : "Starting...";
                if (m_linkStatus.count("consoles"))
                    line += "  -  consoles on the link: " + m_linkStatus["consoles"];
                text(line.c_str(), 250, y, 20, state == "refused" ? kBad : (state == "connected" || state == "hosting") ? kGood : kWarn);
                y += 30;
                for (const auto &peer : m_linkPeers)
                {
                    // "name|ping ms" (direct link) or "name||relayed" (through the host).
                    const size_t bar = peer.find('|');
                    const std::string name = peer.substr(0, bar), rest = bar == std::string::npos ? "" : peer.substr(bar + 1);
                    const size_t bar2 = rest.find('|');
                    const std::string ping = rest.substr(0, bar2);
                    const bool relayed = bar2 != std::string::npos;
                    text((name + (relayed ? "   (through the host: no direct connection)" : ping.empty() ? "" : "   ping " + ping + " ms")).c_str(), 270, y,
                         19, relayed ? kWarn : kText);
                    y += 26;
                }
                y += 8;
            }
            paragraph("Start the game on every PC and let them connect (status above) before anyone opens i.LINK Battle: a "
                      "console arriving while the others are already in that menu can leave the menu stuck, as with a real "
                      "cable. In the game: Arcade Mode > i.LINK Battle (press Up from Single Race). Every PC picks Compete to "
                      "race or Broadcast to watch; console 1 chooses the course, then everyone chooses a car. Tested so far "
                      "with two and three consoles on one PC; the original game links up to six. If a player leaves during "
                      "a race, the others keep racing (that car rolls to a stop): at once when the game is closed, after "
                      "half a second if the PC or its connection dies. The menus notice after the race.\n"
                      "Like the i.LINK cable, the game waits for every console's input each frame, so all PCs run at the speed "
                      "of the slowest one: a LAN is ideal; over the internet you need a ping under about 30 ms. The first "
                      "time, Windows asks whether the game may use the network: allow it for private networks.",
                      250, y, 880, 18, kMuted);
        }

        void pagePaths()
        {
            text("Game files", 250, 108, 30, kText, true);
            bool browse = false;
            pathRow(160, "Disc image", m_iso, "Browse...", &browse);
            if (browse)
            {
                std::string path = m_iso.get();
                if (browseOpenFile(GetWindowHandle(), "Gran Turismo 3 A-Spec (USA) disc image", "Disc images|*.iso;*.bin|All files|*.*", path))
                    m_iso.set(path);
            }
            text(fileExists(full(m_iso.get())) ? "Disc image found." : "Your own copy of Gran Turismo 3 A-Spec (USA, SCUS-97102) as an .iso.", 470, 212,
                 19, fileExists(full(m_iso.get())) ? kGood : kWarn);
            float y = 260;
            {
                // CORE.elf and the 16 driver files from the disc's root: the
                // launcher copies them out of the disc image when needed.
                const std::string gameDir = m_root + "\\windows-diagnostic\\game";
                const bool present = gameFilesPresent(gameDir);
                text("Files from the disc", 250, y, 20, kMuted);
                text(present ? "CORE.elf and the 16 driver files (.IRX) are ready." : "Not taken from the disc image yet: PLAY does it, or this button.",
                     470, y, 18, present ? kGood : kWarn);
                y += 30;
                if (GuiButton(Rectangle{470, y, 300, 36}, present ? "Take them again from the disc image" : "Take them from the disc image"))
                {
                    std::string error;
                    const int n = extractGameFiles(full(m_iso.get()), gameDir, error);
                    if (n > 0 && gameFilesPresent(gameDir))
                        setStatus("Took " + std::to_string(n) + " files from the disc image.", kGood);
                    else
                        setStatus("Could not take the game files from the disc image: " + (error.empty() ? std::string("some are missing") : error), kBad);
                }
                y += 48;
                paragraph("The game needs only your disc image. Everything else it reads from the disc (GT3's program, CORE.GT3, unpacked "
                          "into CORE.elf, and its 16 driver files, which are checked but never run) is taken out of it. No PS2 BIOS is needed.",
                          250, y, 880, 17, kMuted);
                y += 56;
            }
            {
                // Texture dump (gt3_texture_dump.h): every texture the game
                // draws with, once, as a PNG - the first step to upscaled ones.
                text("Textures", 250, y, 20, kMuted);
                bool dump = m_settings.textureDump;
                GuiCheckBox(Rectangle{470, y - 2, 26, 26}, "  Save every texture the game uses (PNG, for upscaling)", &dump);
                if (dump != m_settings.textureDump)
                {
                    m_settings.textureDump = dump;
                    saveOwn();
                    setStatus(m_game.running ? "Saved: texture saving starts the next time the game starts." : "Saved.", m_game.running ? kWarn : kGood);
                }
                y += 34;
                paragraph("Into textures\\dump: each texture once (named by its picture, so the same one is never saved twice, also across "
                          "sessions), with index.csv. Play the menus and races you want; costs some speed while new textures appear. "
                          "Alpha: PS2 half (0x80) saved as opaque.",
                          470, y, 660, 16, kMuted);
                y += 70;
                if (GuiButton(Rectangle{470, y, 220, 36}, "Open texture folder"))
                {
                    const std::string dir = m_root + "\\textures\\dump";
                    std::error_code ec;
                    std::filesystem::create_directories(dir, ec);
                    shellOpen(dir);
                }
                y += 50;
                bool replace = m_settings.textureReplace;
                GuiCheckBox(Rectangle{470, y - 2, 26, 26}, "  Use my textures from textures\\replace (GPU renderers)", &replace);
                if (replace != m_settings.textureReplace)
                {
                    m_settings.textureReplace = replace;
                    saveOwn();
                    setStatus(m_game.running ? "Saved: applies the next time the game starts." : "Saved.", m_game.running ? kWarn : kGood);
                }
                y += 34;
                paragraph("Put an edited or upscaled copy of a saved texture there under the same name (any size; subfolders are fine): "
                          "the game draws it instead.",
                          470, y, 660, 16, kMuted);
                y += 30;
                if (GuiButton(Rectangle{470, y, 220, 36}, "Open replace folder"))
                {
                    const std::string dir = m_root + "\\textures\\replace";
                    std::error_code ec;
                    std::filesystem::create_directories(dir, ec);
                    shellOpen(dir);
                }
                y += 52;
            }
            text("Game executable", 250, y, 20, kMuted);
            text(fit(m_runner, 680, 18).c_str(), 470, y, 18, fileExists(m_runner) ? kText : kBad);
            y += 34;
            text("Project folder", 250, y, 20, kMuted);
            text(fit(m_root, 680, 18).c_str(), 470, y, 18, kText);
            y += 50;
            if (GuiButton(Rectangle{470, y, 220, 40}, "Open project folder"))
                shellOpen(m_root);
            if (GuiButton(Rectangle{700, y, 220, 40}, "Open logs folder"))
                shellOpen(logDirectory());
        }

        // ------------------------------------------------------------ GT Race Lab on this PC
        // gt3RaceLab.exe (development builds, next to the launcher) serves the GT Race Lab page on 127.0.0.1 and
        // keeps what it saves as plain files in <game folder>\racelab-data; it closes when the launcher does.
        std::string raceLabExe() const
        {
            const std::string here = executableDirectory() + "\\gt3RaceLab.exe";
            return fileExists(here) ? here : m_root + "\\out\\gt3-msvc-release\\ps2xRuntime\\gt3RaceLab.exe";
        }
        static std::string lastLine(const std::string &path)
        {
            std::ifstream in(path);
            std::string line, last;
            while (std::getline(in, line))
                if (!line.empty())
                    last = line;
            return last.empty() ? "no message" : last;
        }
        void openRaceLab()
        {
            if (m_raceLab.running)
            {
                if (!m_raceLabUrl.empty())
                    shellOpen(m_raceLabUrl);
                return;
            }
            std::string error;
            m_raceLabLog = logDirectory() + "\\racelab.log";
            m_raceLabUrl.clear();
            m_raceLabStopping = false;
            std::vector<std::string> args{"--root", m_root, "--parent", std::to_string(currentProcessId())};
            if (testRaceLab)
                args.push_back("--no-browser");
            if (startGame(raceLabExe(), args, m_root, {}, m_raceLabLog, m_raceLab, error))
                setStatus("Race Lab started: it opens in your browser.", kGood);
            else
                setStatus("Could not start the Race Lab: " + error, kBad);
        }
        void pageRaceLab()
        {
            text("Race Lab", 250, 108, 30, kText, true);
            paragraph("Your racing lines, the laps you record (F10 in a race), the AI trainer, braking zones and the AI cars' drivers, in "
                      "your browser on this PC. What it saves stays in the game folder as plain files (racelab-data), where Claude can "
                      "pick it up for the AI driving.",
                      250, 156, 880, 18, kMuted);
            // the address it listens on: its log's first line
            if (m_raceLab.running && m_raceLabUrl.empty())
            {
                std::ifstream in(m_raceLabLog);
                std::string line;
                if (std::getline(in, line) && line.find("http://") != std::string::npos)
                {
                    m_raceLabUrl = line.substr(line.find("http://"));
                    while (!m_raceLabUrl.empty() && (m_raceLabUrl.back() == '\r' || m_raceLabUrl.back() == ' '))
                        m_raceLabUrl.pop_back();
                }
            }
            float y = 228;
            const bool built = fileExists(raceLabExe());
            if (!built)
                GuiDisable();
            if (GuiButton(Rectangle{250, y, 280, 44}, m_raceLab.running ? "Open the Race Lab again" : "Open the Race Lab"))
                openRaceLab();
            GuiEnable();
            if (m_raceLab.running && GuiButton(Rectangle{550, y, 200, 44}, "Stop the Race Lab"))
            {
                m_raceLabStopping = true;
                stopGame(m_raceLab);
                setStatus("Race Lab stopped.", kMuted);
            }
            y += 60;
            if (!built)
                text("The Race Lab comes with development builds (gt3RaceLab.exe isn't here).", 250, y, 18, kWarn);
            else if (m_raceLab.running)
                text(m_raceLabUrl.empty() ? "Starting..." : ("Running at " + m_raceLabUrl + " (this PC only)").c_str(), 250, y, 18, kGood);
            else
                text("Not running: Open the Race Lab starts it.", 250, y, 18, kMuted);
            y += 50;
            text("Saved files", 250, y, 20, kMuted);
            text(fit(m_root + "\\racelab-data", 520, 18).c_str(), 400, y, 18, kText);
            if (GuiButton(Rectangle{940, y - 6, 180, 32}, "Open folder"))
            {
                std::error_code ec;
                std::filesystem::create_directories(m_root + "\\racelab-data", ec);
                shellOpen(m_root + "\\racelab-data");
            }
            y += 44;
            text("Recordings", 250, y, 20, kMuted);
            text(fit(m_root + "\\recordings", 520, 18).c_str(), 400, y, 18, kText);
            if (GuiButton(Rectangle{940, y - 6, 180, 32}, "Open folder"))
            {
                std::error_code ec;
                std::filesystem::create_directories(m_root + "\\recordings", ec);
                shellOpen(m_root + "\\recordings");
            }
            y += 50;
            int ai = m_settings.raceLabAi ? 1 : 0;
            if (choiceRow(y, "AI cars 1-5", "GT3's own AI;The Race Lab's", 2, ai,
                          m_game.running ? "Changes here apply from the next time the game starts."
                                         : "The Race Lab's: what its Send to AI cars wrote (racelab-data\\ai-lines), from each race's start."))
            {
                m_settings.raceLabAi = ai == 1;
                syncFromBoxes();
                saveOwn();
            }
            y += 80;
            paragraph("Press F10 in a race to record your laps (only while you drive the car: not the grid wait or GT3's own driving). "
                      "In the Race Lab, Laps > Add laps from the game picks up the new ones. The claude.ai copy of the Race Lab keeps its "
                      "own saved data: ask Claude to copy between them.",
                      250, y, 880, 17, kMuted);
        }

        void pageRoadmap()
        {
            text("Roadmap", 250, 108, 30, kText, true);
            paragraph("Working now: native game code, OpenGL and Vulkan renderers at 60 fps up to 4x progressive in the tested "
                      "races, DualShock 2 (analog steering, analog pedals, vibration path), memory card saves, adaptive "
                      "deinterlacing, graphics changes applied live, i.LINK Battle over LAN / internet (tested: up to three "
                      "consoles on one PC), AMD FSR 1 and NVIDIA Image Scaling, HUD units, experimental frame generation to "
                      "90 / 120 fps, widescreen and triple-screen races (flat or angled side screens, bezel correction and a "
                      "field of view matched to your seat; untested on real triple monitors), six race cameras and a "
                      "right-stick look-around, a proximity radar, your own HUD layout, a sharp rear-view mirror option, and a "
                      "phone dashboard (mirror, position, timing, the whole track map, radar). The launcher takes everything "
                      "it needs from your disc image.\n\n"
                      "Next: i.LINK Battle between two PCs (to test), the triple screens on real monitors (to test), FSR 2 / "
                      "DLSS from the same motion data.\n\n"
                      "Later: texture filtering and replacement packs, FSR 3/4 and DLSS on Vulkan, more than 60 fps (needs "
                      "GT3's physics timing), wheels and force feedback, sector splits on the phone, mods.\n\n"
                      "These are plans, not features: this launcher only offers settings the game supports today.",
                      250, 160, 880, 20, kMuted);
        }

        Font m_font{}, m_fontBold{};
        std::string m_root, m_settingsFile, m_runner, m_logFile;
        Settings m_settings;
        PathBox m_iso, m_card, m_profile, m_linkHost, m_linkName, m_linkPort, m_driverNames;
        std::string m_linkStatusFile;
        std::vector<std::string> m_addresses, m_linkPeers;
        std::map<std::string, std::string> m_linkStatus;
        double m_linkStatusRead = -1.0;
        GameProcess m_game;
        GameProcess m_raceLab;                       // gt3RaceLab.exe (the Race Lab page)
        std::string m_raceLabUrl, m_raceLabLog;      // where it listens (its log's first line), its log
        bool m_raceLabStopping = false;              // stopped from the Race Lab page (not an error)
        std::filesystem::file_time_type m_savedStamp{}; // the settings file's time after the launcher's own last save
        double m_nextReloadCheck = 0.0;
        int m_phoneLastSize[5] = {3, 1, 2, 2, 2}; // a panel's size before it was turned off
        float m_viewScale = 1.0f, m_viewX = 0.0f, m_viewY = 0.0f; // the pages' scale and offset in the window
        // The HUD layout page: the race picture, the selected box (8 radar), dragging.
        Texture2D m_hudBg{};
        std::filesystem::file_time_type m_hudBgStamp{};
        std::string m_hudBgPath; // the file m_hudBg was loaded from (or tried)
        int m_hudSelected = -1;
        bool m_hudDragging = false, m_hudSliderMoved = false;
        bool m_towerColumnsOpen = false; // the custom tower's columns shown in place of the box list
        Vector2 m_hudGrab{};
        bool m_qrOpen = false;   // the Phone page's QR code is shown
        bool m_qrOpenedFrame = false; // opened this frame (by a click being released)
        size_t m_qrIndex = 0;    // which of m_addresses it encodes
        std::string m_qrText;    // what m_qr encodes
        gt3qr::Code m_qr;
        bool m_toggleFullscreen = false;                          // the header button was pressed
        int m_page = 0;
        std::string m_status = "Ready.";
        Color m_statusColor = kMuted;
    };
} // namespace

int main(int argc, char **argv)
{
    Launcher launcher;
    for (int i = 1; i + 1 < argc; ++i)
    {
        if (std::strcmp(argv[i], "--page") == 0)
            launcher.startPage = std::atoi(argv[++i]);
        else if (std::strcmp(argv[i], "--shot") == 0)
            launcher.shotPath = argv[++i];
        else if (std::strcmp(argv[i], "--shot-frames") == 0)
            launcher.shotFrames = std::max(1, std::atoi(argv[++i]));
        else if (std::strcmp(argv[i], "--extract-to") == 0)
            launcher.extractTo = argv[++i];
        else if (std::strcmp(argv[i], "--hud-select") == 0)
            launcher.testHudSelect = std::atoi(argv[++i]);
        else if (std::strcmp(argv[i], "--size") == 0)
            std::sscanf(argv[++i], "%dx%d", &launcher.testWidth, &launcher.testHeight);
    }
    for (int i = 1; i < argc; ++i)
        if (std::strcmp(argv[i], "--play") == 0)
            launcher.playAtStart = true;
        else if (std::strcmp(argv[i], "--fullscreen") == 0)
            launcher.testFullscreen = true;
        else if (std::strcmp(argv[i], "--qr") == 0)
            launcher.testQr = true;
        else if (std::strcmp(argv[i], "--tower-columns") == 0)
            launcher.testTowerColumns = true;
        else if (std::strcmp(argv[i], "--wheel-tab") == 0)
            launcher.testWheelTab = true;
        else if (std::strcmp(argv[i], "--race-lab") == 0)
            launcher.testRaceLab = true;
    launcher.run();
    return 0;
}
