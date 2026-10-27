#pragma once

// GT4 Recompilation launcher, Full edition: the settings only the Full edition has (Display tabs, multi-screen,
// network, garage view state), part of Settings (gt4stock_settings.h, `full`). Every value is one line of gt4-start.env:
//   - a GT4.exe key from the Full list (shared/gt4_full_keys.h) when GT4.exe acts on it today or with the 480p update
//     (video mode GT4_RACE_MODE, colour GT4_RACE_COLOUR, shape GT4_ASPECT, the four scaling filters GT4_PICTURE_FILTER),
//   - a GT4L_NB_<NAME> line when GT4.exe has no side for it yet ("Not built yet": stored, never passed on),
//   - a GT4L_ line for the launcher's own state.
// the developer's "not for now" list (no control at all, not even greyed; FULL-LAUNCHER-PLAN.md "Not for now (developer)"): DLSS, DLSS 5
// relighting, AMD FSR 1 and 4, ray tracing, internal resolution 6x and 8x, frame generation, G-Sync / FreeSync, texture
// dump / replacement / pack manager, the Benchmark button, the LAN race test.
// No raylib and no Windows here: the host tests build it.

#include "shared/gt4_hud_boxes.h"

#include <array>
#include <string>
#include <vector>

namespace gt4stock
{
    // Display > Picture > Scaling. The first four are GT4_PICTURE_FILTER (nearest / bilinear / bicubic / lanczos, the 480p
    // update's fixed values); NIS and TV (CRT) are not built yet (GT4L_NB_UPSCALER) and GT4.exe then gets bilinear.
    enum class FullScaling
    {
        Nearest,
        Bilinear,
        Bicubic,
        Lanczos,
        Nis,
        Tv,
    };
    const char *fullScalingKey(FullScaling s);   // nearest / bilinear / bicubic / lanczos / nis / tv
    const char *fullScalingLabel(FullScaling s); // "Nearest Neighbor", "Bilinear Filtering", ...
    bool fullScalingBuilt(FullScaling s);        // the first four
    // What GT4_PICTURE_FILTER carries for this choice (bilinear for the ones not built yet).
    const char *fullScalingFilterValue(FullScaling s);

    // Display > Screen shape > Aspect ratio. 16:9 and 4:3 are GT4_ASPECT (the 480p update); square pixels is GT4_ASPECT=4:3
    // with GT3_DISPLAY_ASPECT=pixels; 21:9, 32:9 and 48:9 (one screen) are GT4_ASPECT=21:9 / 32:9 / 48:9 (full-m2, Wide: races
    // and replays wider, GT4's menus 16:9). A milestone-1 file's GT4L_NB_ASPECT is read once and written as GT4_ASPECT.
    enum class FullAspect
    {
        Wide,   // 16:9 (the default)
        Normal, // 4:3
        SquarePixels,
        Ultra21,
        Ultra32,
        Triple48,
    };
    const char *fullAspectKey(FullAspect a);   // 16:9 / 4:3 / pixels / 21:9 / 32:9 / 48:9
    const char *fullAspectLabel(FullAspect a); // "16:9", "4:3", "Square pixels", ...
    bool fullAspectBuilt(FullAspect a);
    bool fullAspectWider(FullAspect a); // 21:9, 32:9, 48:9
    // The shape of the race picture for this choice (4:3, the 640 x 448 frame's own, 16:9, 21:9, 32:9, 48:9).
    float fullAspectRatioOf(FullAspect a);

    // An own HUD widget (Driving > Race HUD, HUD layout): on, where, size and see-through.
    struct FullHudWidget
    {
        bool on = false;
        int place = 0;       // the old form (HUD 4 to 6): 0 bottom right, 1 bottom left, 2 top right, 3 top left, 4 bottom centre,
                             // 5 top centre, 6 above GT4's dash (bottom centre, its bottom edge at 31 % of the picture's height),
                             // 7 automatic (GT4.exe keeps it clear of GT4's own HUD in every view: gt4_hud.h). 7 stays
                             // Automatic in the new form; 0..6 are converted to xy when the file is written (fullHudPlaceCentre).
        int size = 100;      // percent, 50..200 (the new form: 40..250)
        int seeThrough = 20; // percent, 0..90
        bool xy = false;     // the new form (HUD hand-off 7 A3): the box's centre at x, y (percent of the HUD area, one decimal)
        float x = 50.0f, y = 50.0f;
        int style = 0;       // radar 0 round, 1 square; delta 0 bar, 1 number; g-meter 0 trail, 1 dot (pedals: none); HUD hand-off 7 C:
                             // map 0 locked, 1 rotating; revs 0 bar, 1 lights; lap 0 lap, 1 clock (tower, splits, speed, gear: none)
        bool simple = false; // the map's detail (HUD hand-off 10): false detailed (GT4's art), true simple (the centreline)
        // The timing tower's own (GT4_HUD_TOWER=on,<x>,<y>,<size>,<see>,<name>,<columns>,<rows>): name 0 car, 1 short, 2 driver;
        // columns bits 1 gap, 2 int, 4 best, 8 last (written gap+int+best...); rows 4..16.
        int towerName = 0;
        int towerCols = gt4hudbox::kTowerColsDefault;
        int towerRows = gt4hudbox::kTowerRowsDefault;
    };
    // The widgets not built yet (GT4L_NB_HUD_<KEY>): speed, revs, gear, map, timing tower.
    constexpr int kFullHudWidgets = 5;
    extern const char *const kFullHudWidgetKeys[kFullHudWidgets];   // SPEED REVS GEAR MAP TOWER
    extern const char *const kFullHudWidgetLabels[kFullHudWidgets]; // "Speed", "Revs", ...
    // The widgets GT4.exe draws (GT4_HUD_<KEY>=off | on,<place>,<size>,<see-through>): the proximity radar, the delta to the
    // best lap, the pedals (throttle and brake) and the g-meter. One that is off keeps its place, size and see-through in the
    // launcher's GT4L_HUD_<KEY>=<place>,<size>,<see-through> (GT4_HUD_<KEY>=off carries none).
    // The defaults (HUD 6): the radar and the delta on, placed automatically (GT4.exe: the radar at the bottom right, lifted
    // over the chase views' gear and speed box; the delta above GT4's dash in the bumper and bonnet views, at the top centre
    // in the chase views), the pedals and the g-meter off. HUD 4's were the radar at the bottom centre (on,4,100,20) and the
    // delta at the top centre (on,5,100,20); 4B's the radar at the bottom right (on,0,100,20) and the delta above GT4's
    // dash (on,6,100,20). A file still holding an older default exactly gets HUD 6's when read (HUD 4's: a file without
    // GT4L_HUD_DEFAULTS, written before 4B; 4B's: GT4L_HUD_DEFAULTS=4b); a changed one stays.
    // HUD hand-off 7 C adds GT4.exe's new boxes after them, each off by default at its place (the new form): the timing tower
    // (87,46), the race map (12,44, locked), the split times (50,40), the rev lights (50,92), speed (84,90), gear (78.5,90) and our
    // own lap counter (30,7.5); shared/gt4_hud_boxes.h has their sizes and styles.
    constexpr int kFullHudLiveWidgets = gt4hudbox::kCount;
    constexpr int kFullHudTower = gt4hudbox::Tower, kFullHudMap = gt4hudbox::Map, kFullHudSplits = gt4hudbox::Splits, kFullHudRevs = gt4hudbox::Revs,
                  kFullHudSpeed = gt4hudbox::Speed, kFullHudGear = gt4hudbox::Gear, kFullHudLap = gt4hudbox::Lap;
    extern const char *const kFullHudLiveKeys[kFullHudLiveWidgets];   // RADAR DELTA PEDALS GMETER TOWER MAP SPLITS REVS SPEED GEAR LAP
    extern const char *const kFullHudLiveLabels[kFullHudLiveWidgets]; // "Radar", "Delta", ...
    inline constexpr FullHudWidget kFullHudLiveDefaults[kFullHudLiveWidgets] = {
        {true, 7, 100, 20}, {true, 7, 100, 20}, {false, 0, 100, 20}, {false, 1, 100, 20}, // radar, delta on; pedals, g-meter off
        {false, 0, 100, 20, true, 87.0f, 46.0f},  {false, 0, 100, 20, true, 12.0f, 44.0f}, {false, 0, 100, 20, true, 50.0f, 40.0f},
        {false, 0, 100, 20, true, 50.0f, 92.0f},  {false, 0, 100, 20, true, 84.0f, 90.0f}, {false, 0, 100, 20, true, 78.5f, 90.0f},
        {false, 0, 100, 20, true, 30.0f, 7.5f}};
    constexpr int kFullHudPlaceCount = 8;
    extern const char *const kFullHudPlaces[kFullHudPlaceCount]; // "Bottom right", ... "Above GT4's dash", "Automatic"
    // The widgets' base sizes (fractions of the picture's height, width x height, at 100 %; gt4_hud.cpp kBase) and styles.
    // (the tower's comes from its name, columns and rows: gt4hudbox::baseSize)
    inline constexpr const auto &kFullHudBase = gt4hudbox::kBase;
    inline constexpr const auto &kFullHudStyleKeys = gt4hudbox::kStyles;
    // An old-form place's centre for widget w at its size (percent of the HUD area; aspect = the picture's width / height),
    // as GT4.exe placed it (gt4_hud.cpp, one widget per place). Place 7 (automatic) has none: 50, 50.
    void fullHudPlaceCentre(int w, int place, int size, float aspect, float &x, float &y);
    // A widget's GT4_HUD_<KEY> value in the new form (off, or on,<x>,<y>,<size>,<see-through>[,<style>]; auto,auto for
    // Automatic; an old-form place is converted with the picture's aspect).
    std::string fullHudValue(int w, const FullHudWidget &h, float aspect);
    // The timing tower's columns as written: gap+int+best+last in that order ("none" for none).
    std::string fullHudTowerCols(int bits);
    // The start lights' box (GT4_START_LIGHTS_BOX=<x>,<y>,<size>): 50,7.5,100 is the top centre.
    inline constexpr float kFullLightsX = 50.0f, kFullLightsY = 7.5f;
    // The parts of GT4's own race HUD the player can hide (GT4_HUD_HIDE=<list> in this order; empty or absent: none): position
    // (the race position, top left), lap (the lap counter), times (the lap clock, best and last lap), panel (speedo, tacho,
    // gear and digital speed) and map (the course map). GT4's messages are never hidden. An older file's GT4L_NB_HUD_HIDE_MAP
    // (the map) and GT4L_NB_HUD_HIDE_POSITION (the position and the lap counter) are read once and written as GT4_HUD_HIDE.
    // HUD hand-off 7 A3: "times" split into clock (the race clock, top centre) and besttimes (best and last lap, top right);
    // a file's "times" reads as both, and both hidden are written as "times" (an older GT4.exe reads it).
    inline constexpr int kFullHudHideCount = 6;
    inline constexpr const char *kFullHudHideKeys[kFullHudHideCount] = {"position", "lap", "clock", "besttimes", "panel", "map"};
    inline constexpr const char *kFullHudHideLabels[kFullHudHideCount] = {"Position", "Lap counter", "Race clock", "Best and last lap",
                                                                          "Speedo panel", "Course map"};
    inline constexpr int kFullHudHideClock = 2, kFullHudHideBest = 3, kFullHudHidePanel = 4, kFullHudHideMap = 5;
    // GT4's own HUD parts moved (HUD hand-off 7 B1): GT4_HUD_MOVE=<part>:<x>,<y>,<size>;... (x, y the centre in percent of the HUD
    // area with one decimal, size 40..250 %; empty: every part where GT4 puts it). The parts in this order: the race picture's
    // parts, the chase views' compact panel's four pieces (fuel, warnings, gbar, gearspeed; "panel" is the bumper and bonnet
    // views' dash) and the mirror (gt4_hud_move.h).
    inline constexpr int kFullHudMoveParts = 11;
    inline constexpr const char *kFullHudMoveKeys[kFullHudMoveParts] = {"position", "lap",      "clock", "besttimes", "map",   "panel",
                                                                        "fuel",     "warnings", "gbar",  "gearspeed", "mirror"};
    struct FullHudMove
    {
        bool on = false;  // false: where GT4 puts it
        float x = 50.0f, y = 50.0f;
        int size = 100;
    };
    using FullHudMoves = std::array<FullHudMove, kFullHudMoveParts>;
    std::string fullHudMoveList(const FullHudMoves &moves); // the GT4_HUD_MOVE value ("" for none moved)
    // A value read back (false: an item not understood, dropped; the others are kept).
    bool fullHudMoveParse(const std::string &value, FullHudMoves &moves);
    int fullHudMovePart(const std::string &key); // kFullHudMoveKeys' index, -1 none
    // HUD hand-off 7 D / 9, Driving > Race HUD > Radar: the zoom chips' ranges (GT4_RADAR_ZOOM, smallest first; since the GT3
    // radar settings, 20 Oct 2026, no longer offered: the launcher writes GT4_RADAR_ZOOM=off) and the colours' defaults
    // (GT4_RADAR_COLOURS: you, other, close (written as the other cars' colour: not offered since the GT3 settings), alongside
    // under 6 m and the side bars, background). The GT3 radar settings (gt3_launcher.cpp pageRadar): range 10-80 m (25), car
    // opacity 10-100 % (90), background opacity 0-100 % (35), GT3's eleven colours and its default colours.
    inline constexpr int kFullRadarZoomSteps = 9;
    inline constexpr int kFullRadarZoomM[kFullRadarZoomSteps] = {10, 15, 20, 30, 40, 50, 60, 80, 100};
    inline constexpr int kFullRadarZoomDefault = 0;
    inline constexpr uint32_t kFullRadarColours[5] = {0x78dcff, 0xffe65a, 0xffe65a, 0xff8c00, 0x000000};
    inline constexpr int kFullRadarPalette = 11; // GT3's colours: white, yellow, orange, red, pink, purple, blue, light blue, green, grey, black
    inline constexpr uint32_t kFullRadarPaletteRgb[kFullRadarPalette] = {0xffffff, 0xffe65a, 0xff8c00, 0xff3b30, 0xff4fd8, 0xb070ff,
                                                                         0x3e7eff, 0x78dcff, 0x56c880, 0x9aa0aa, 0x000000};
    inline constexpr int kFullRadarRangeDefault = 25, kFullRadarCarsDefault = 90, kFullRadarBgDefault = 35;
    std::string fullRadarZoomList(int bits);                         // "15,30,60"; "off" for none
    std::string fullRadarColourList(const std::array<uint32_t, 5> &c); // "f2b33d,ffffff,f08a3c,e5484d,000000"
    std::string fullZoomKeyName(int key);                            // GT4_HUD_ZOOM_KEY's value: "z", "5", "none"
    std::string fullHudHideList(int bits); // the hidden parts' names in that order, comma separated ("" for none)
    // A list read back into bits (empty: none); false if a name is not one of them (the others are kept).
    bool fullHudHideBits(const std::string &list, int &bits);

    // Sound > Engine bass: GT4_ENGINE_BASS (gt4_sound.cpp: a low shelf on GT4's engine and exhaust voices, up to +3 / +6 / +9 dB
    // well below 150 Hz, half that at 150 Hz, with a soft limiter in races). 0 off, 1 low, 2 medium (the default), 3 high. The
    // settings file's empty or unknown value is read as medium (and written back); GT4.exe itself reads an empty value as off.
    constexpr int kFullEngineBassDefault = 2;
    const char *fullEngineBassKey(int level);   // off / low / medium / high
    const char *fullEngineBassLabel(int level); // "Off", "Low", "Medium", "High"
    int fullEngineBassGainDb(int level);        // 0, 3, 6, 9

    // Sound > Sound packs (Custom Sounds, analysis/design/sound-packs): a player's engine sound pack (a folder with pack.json,
    // made by sound_pack.py import) played by GT4.exe in place of GT4's own engine for the car the player drives.
    //   GT4_SOUND_PACK=<folder> (every car) or off; GT4_SOUND_PACK_MAP=<file> (per car: "<car code> <folder>" lines and an
    //   optional "all <folder>") or off. The launcher keeps its own choices in GT4L_SOUND_ALL (the pack for every car, kept
    //   while another mode is chosen) and GT4L_SOUND_CARS ("<code>=<folder>|..."), and writes the map file itself.
    enum class FullSoundMode
    {
        Off,      // GT4's own engine sound
        EveryCar, // one pack for every car
        PerCar,   // a pack per car (the others: soundPackAll, or GT4's own when it is empty)
    };
    const char *fullSoundModeLabel(FullSoundMode m);
    struct FullSoundCar
    {
        uint32_t code = 0;  // GENERIC_CAR code (59 = ka_01)
        std::string folder; // the pack folder
    };

    struct FullSettings
    {
        // ---- Picture (live): video mode, colour, scaling (the 480p update's keys)
        bool gameVideoModes = false; // GT4_RACE_MODE=game (GT4's own Options: 480i / 1080i); off = 480p everywhere
        int colour = 32;             // GT4_RACE_COLOUR=32 / 16
        FullScaling scaling = FullScaling::Bilinear;
        // ---- Colour (analysis/design/colour-lut: live with a GT4.exe that has the marker, FullFeatures::lut)
        int lut = 0;            // GT4_LUT 0 off (the default), 1 gt3 (the GT3 look), 2 the .cube file in lutFile
        std::string lutFile;    // GT4L_LUT_FILE: the player's .cube file (kept while Off or the GT3 look is chosen)
        int lutStrength = 100;  // GT4_LUT_STRENGTH 0..100
        // ---- Resolution (full-m2, Resolution: live with a GT4.exe that has the marker, FullFeatures::internalRes)
        int internalRes = 1;    // GT3_GS_GL_SCALE 1 (native), 2, 3, 4 (an older file's GT4L_NB_INTERNAL_RES is read once)
        // GT4L_RES_PRESET (developer, 2026-10-19: "yes use the resolution presets, but keep the other options in there as well"):
        // 0 custom (the internal resolution as chosen below it), 1 auto (from the monitor's height), 2 720p, 3 1080p, 4 1440p,
        // 5 2160p. A preset sets internalRes (fullPresetScale); an older file with GT3_GS_GL_SCALE and no preset keeps custom.
        int resPreset = 1;
        bool resPresetSet = false; // (reading only) a GT4L_RES_PRESET line was read
        int drawing = 0;        // GT4L_DRAWING 0 auto (follows the resolution), 1 exact, 2 fast; GT4.exe gets GT3_GS_GL_FAST 0 / 1
        int effectsScale = 0;   // GT4_EFFECTS_SCALE 0 Normal (as the picture), 2, 3, 4: GT4's glow / light / reflection buffers' size
        int presenter = 0;      // GT4_VK_PRESENT 0 auto (Vulkan shows the frames), 1 gl (the older way, through OpenGL)
        // ---- Race Lab (racelab/HANDOFF-1-RECORDER-AND-SERVER.md)
        bool raceLabRecord = false; // GT4_DRIVE_LOG: on = racelab-laps (beside gt4-start.env), off = off; F10 in a race stops and starts it
        bool raceLabDrivers = true; // GT4_AI_DRIVERS: on = racelab-data\ai-lines (the drivers the Race Lab sent), off = the AI cars keep car names
        bool drawingSet = false; // (reading only) a GT4L_DRAWING line was read: it wins over GT3_GS_GL_FAST, which follows it
        // ---- Screen shape
        FullAspect aspect = FullAspect::Wide;
        bool span = false;      // GT3_FULLSCREEN=span (across all monitors; Settings::fullscreen is then on)
        bool exclusive = false; // GT3_FULLSCREEN=exclusive (full screen; Settings::fullscreen is then on; off: borderless)
        // ---- Cameras (full-m2, Cameras: live with a GT4.exe that has the marker, FullFeatures::cameras)
        // The view button's views (GT4_CAM_VIEWS, MoreCams): bits bumper 1, bonnet 2, roof 4, close chase 8, chase 16, far
        // chase 32 (at least one; GT4_CAM_FAR_CHASE follows the far chase's bit for a GT4.exe from the Cameras track).
        int camViews = 0x3F;
        // The views locked to the car's tilt (GT4_CAM_LOCK, MoreCams), bits as camViews: GT4's own is bumper, bonnet, roof.
        int camLock = 0x07;
        bool look360 = true;  // GT4_CAM_LOOK 360 / off: the 360 look on the right stick
        bool lookTakeStick = true; // GT4_CAM_LOOK_STICK take / share: with the 360 look on, it takes the right stick in races
        int triggerRange = 100;    // GT4_TRIGGER_RANGE 30..100: the L2 / R2 pressure at a trigger's full travel (Controllers)
        // ---- Smoothness
        bool vsync = true;      // GT4L_NB_VSYNC (not built yet)
        int fpsCounter = 0;     // GT4_FPS_COUNTER 0 off, 1 simple (fps and frame time), 2 detailed (the stats in fpsStats); F3 in GT4 cycles
        int fpsStats = 0x7FF;   // GT4_FPS_STATS: what detailed shows, bits in kFullFpsStatKeys' order (at least one; all by default)
        // GT4L_RENDERER_PREPARED: "<GT4.exe bytes> <GT4.exe time> <result>" of the last Prepare shaders run (result ok /
        // failed / novulkan); empty: never prepared for this GT4.exe.
        std::string prepared;
        // GT3_RENDERER: 0 auto (Vulkan on an NVIDIA or AMD card, OpenGL on any other; on AMD Vulkan
        // shows its frames through its own swapchain, gs_vk_present.h: AMD's OpenGL driver crashed on the shared picture), 1 vk (Vulkan), 2 gl (OpenGL).
        int renderer = 0;
        // ---- Detail (full-m2, Detail: live with a GT4.exe that has the marker, FullFeatures::detail)
        int carDetail = 0, wheelDetail = 0; // GT4_CAR_DETAIL / GT4_WHEEL_DETAIL game / 2x / 4x / full: 0 GT4's own, 1 twice as far,
                                            // 2 four times, 3 always full (an older file's GT4L_NB_CAR_DETAIL / _WHEEL_DETAIL is read once)
        bool wheelBlur = true;              // GT4_WHEEL_BLUR on / off (an older file's GT4L_NB_WHEEL_BLUR is read once)
        bool texSmooth = false;             // GT4_TEX_SMOOTH on / off (off: GT4's own sampling; only at 2x and above)
        bool texDump = false;               // GT4_TEXTURE_DUMP on / off (on: GT4.exe's folder	exture-dump)
        bool texDumpOpaque = false;         // GT4_TEXTURE_DUMP_OPAQUE on / off (copies without alpha under texture-dump\_opaque)
        bool texReplace = false;            // GT4_TEXTURE_REPLACE on / off (on: GT4.exe's folder	exture-replace)
        bool detailSet = false;             // (reading only) a GT4_CAR_DETAIL / GT4_WHEEL_DETAIL / GT4_WHEEL_BLUR line was read: it wins
        // ---- Detail and textures (not built yet)
        bool mirrorDetail = false;          // GT4L_NB_MIRROR_DETAIL
        bool sharpTextures = false;         // GT4L_NB_TEXTURE_LOD: ps2 / sharp
        bool effectDetail = false;          // GT4L_NB_EFFECT_DETAIL
        // ---- HUD (live): GT4.exe's widgets, the start lights and the parts of GT4's own HUD hidden; units stay GT4's own Options
        std::array<FullHudWidget, kFullHudLiveWidgets> liveWidgets{kFullHudLiveDefaults[0], kFullHudLiveDefaults[1], kFullHudLiveDefaults[2],
                                                                   kFullHudLiveDefaults[3], kFullHudLiveDefaults[4], kFullHudLiveDefaults[5],
                                                                   kFullHudLiveDefaults[6], kFullHudLiveDefaults[7], kFullHudLiveDefaults[8],
                                                                   kFullHudLiveDefaults[9], kFullHudLiveDefaults[10]}; // GT4_HUD_<KEY>, GT4L_HUD_<KEY> (off)
        int liveWidgetsSet = 0;             // (reading only) bits: a GT4_HUD_<KEY>=on,... line was read: it wins over GT4L_HUD_<KEY>
        int liveWidgetsMoved = 0;           // (reading only) bits: a widget read with an older default, given HUD 6's (kFullHudLiveDefaults)
        std::array<FullHudWidget, kFullHudLiveWidgets> liveWidgetsRead{}; // (reading only) a moved widget as it was read
        int hudDefaultsVersion = 0;         // (reading only) GT4L_HUD_DEFAULTS: 0 none (before 4B), 1 "4b", 2 "6" (no widget is moved)
        bool startLights = false;           // GT4_START_LIGHTS 1 / 0: start lights over GT4's own countdown (they follow it)
        int startCount = 5;                 // GT4_START_COUNT 3..5
        float lightsX = kFullLightsX, lightsY = kFullLightsY; // GT4_START_LIGHTS_BOX=<x>,<y>,<size>: the lights' centre (percent
        int lightsSize = 100;                                  // of the HUD area) and size (40..250 %)
        int startLook = 0;                  // GT4_START_LOOK 0 row, 1 two (two rows, as GT3 has them)
        // HUD hand-off 7 E, Driving > Race HUD > Start lights: holding the start (GT4.exe gt4_start_hold.h; off by default)
        bool startHold = false;             // GT4_START_HOLD on / off: the switch for the four below
        float startDelayMin = 0.2f, startDelayMax = 3.0f; // GT4_START_DELAY=<min>,<max>: lights out 0-5 s after the last light
        int startAi = 2;                    // GT4_START_AI 0 instant, 1 quick, 2 normal, 3 slow: each AI car's reaction
        bool startFree = false;             // GT4_START_CAR held / free: your car yours from the first light
        int startJump = 0;                  // GT4_START_JUMP 0 h5, 1 h10: a jump start holds your car 5 / 10 s after the lights
        std::string hudPicture;             // GT4L_HUD_PICTURE: the player's own picture for the HUD layout page (empty: GT4's)
        int hudHide = 0;                    // GT4_HUD_HIDE: the parts of GT4's own HUD hidden, bits in kFullHudHideKeys' order (0: none)
        bool hudHideSet = false;            // (reading only) a GT4_HUD_HIDE line was read: it wins over GT4L_NB_HUD_HIDE_*
        FullHudMoves hudMoves{};            // GT4_HUD_MOVE: GT4's own parts moved and resized (none: where GT4 puts them)
        // HUD hand-off 7 D / 9, Driving > Race HUD > Radar (the radar's on / off, place, size, see-through and shape are
        // GT4_HUD_RADAR's, liveWidgets[0])
        int radarShow = 0;                  // GT4_RADAR_SHOW 0 always, 1 near (when cars are near)
        int radarRange = kFullRadarRangeDefault; // GT4_RADAR_RANGE 10..80 m (1 m steps; an older file's 5..100 clamped)
        int radarZoom = kFullRadarZoomDefault; // GT4_RADAR_ZOOM: bits of kFullRadarZoomM, read but always written as off (not offered)
        bool radarLabels = true;            // GT4_RADAR_LABELS on / off: the metres on the rings
        int radarCars = kFullRadarCarsDefault; // GT4_RADAR_CARS 10..100 %: the cars' opacity (yours 10 % more, as GT3)
        int radarBg = kFullRadarBgDefault;  // GT4_RADAR_BG 0..100 %: the background's opacity (also GT4_HUD_RADAR's see-through,
                                            // 100 - it, up to 90)
        bool radarBgRead = false;           // (reading only) a GT4_RADAR_BG line was read (else an older file's see-through gives it)
        bool radarLineRead = false;         // (reading only) a GT4_HUD_RADAR / GT4L_HUD_RADAR line was read
        std::array<uint32_t, 5> radarColours{kFullRadarColours[0], kFullRadarColours[1], kFullRadarColours[2], kFullRadarColours[3],
                                             kFullRadarColours[4]}; // GT4_RADAR_COLOURS you, other, close (= other), alongside, background
        int hudZoomKey = 'Z';               // GT4_HUD_ZOOM_KEY: the Race HUD zoom key (raylib KEY_A..KEY_Z, KEY_ZERO..KEY_NINE: their
                                            // ASCII capitals and digits), 0 none. Read only: not offered or written since the GT3
                                            // radar settings (the zoom is off, so GT4.exe takes no key: Z stays GT4's Square)
        int hudZoomPad = -1;                // GT4_HUD_ZOOM_PAD: its controller button (raylib GamepadButton 1..17), -1 none (read only)
        // ---- HUD (not built yet)
        bool hudKeepMiddle = false;         // GT4L_NB_HUD_WIDE: edges / middle
        std::array<FullHudWidget, kFullHudWidgets> widgets{};      // GT4L_NB_HUD_<WIDGET>=off | on,<place>,<size>,<see-through>
        bool phoneMirror = false;           // GT4L_NB_PHONE_MIRROR
        // ---- Multi-screen (the developer's rig are the defaults, MULTI-MONITOR-NOTES.md). Live (full-m2, Multi; with a GT4.exe that has the
        // marker, FullFeatures::screens): mode 4, three screens side by side with one wide picture, GT4_SCREENS=3 and
        // GT4_SCREENS_GEOMETRY=<width>,<height>,<gap> (the gap 0 with Panorama on). The rest is not built yet (GT4L_NB_MM_).
        // ---- Telemetry screen (HUD: hud/pages/FEED.md): GT4's timing / map page on another monitor, phone pages
        int telemetryScreen = 0;      // GT4L_TELEMETRY_SCREEN 0 off, else the launcher's monitor n (1-based)
        std::string telemetryRect;    // GT4_TELEMETRY_MONITOR: that monitor's x,y,w,h when it was chosen
        int telemetryLayout = 0;      // GT4_TELEMETRY_LAYOUT 0 timing-map, 1 timing, 2 map, 3 dash, 4 mirror
        bool phonePages = false;      // GT4_PAGES=lan: the pages on the home network too (off: this PC only)
        int mirrorMode = 0;           // GT4_MIRROR 0 screen, 1 both (screen + phone), 2 page (phone only); 1 and 2 need GT4_PAGES=lan
        // (HUD hand-off 8, Driving > Telemetry screen: GT4_TELEMETRY_MONITOR also takes <DISPLAYn>,<name>,<W>x<H>,<x>,<y>, kept in
        // telemetryRect; the layouts are telemetry-layout.json, GT4_TELEMETRY_LAYOUT is retired)
        bool telemetryMirror = false; // GT4L_TELE_MIRROR: a telemetry screen in use shows the mirror (GT4_MIRROR page, or both)
        bool teleBlack = false;       // GT4_TELE_BG black | dark: the telemetry window's background (the layouts' "bg")
        int mmMode = 0;                     // 0 one screen (GT4_SCREENS=1), 4 three screens, one wide picture (GT4_SCREENS=3);
                                            // GT4L_NB_MM_MODE 1 GT4's Multi Monitor Mode, 2 side views in every mode, 3 one GT4 draws every view
        int mmLayout = 3;                   // GT4L_NB_MM_LAYOUT 3 (three across) / 6 (two rows of three)
        bool mmAngled = true;               // GT4L_NB_MM_ANGLE auto | flat | <degrees>
        bool mmAutoAngle = true;
        int mmAngleDeg = 54;
        double mmScreenW = 698.1, mmScreenH = 392.7, mmBezel = 18.1; // mm (MSI G321Q, its data sheet)
        int mmDistanceCm = 70;              // GT4L_NB_MM_GEOMETRY=<w>,<h>,<bezel>,<distance cm>
        bool mmPanorama = false;            // GT4L_NB_MM_PANORAMA
        bool mmHudEvery = false;            // GT4L_NB_MM_HUD middle / every
        bool mmLive = false;                // GT4L_NB_MM_LIVE
        int mmTop = 0;                      // GT4L_NB_MM_TOP (GT4's own Multi Monitor Mode): off / top (the screen above the middle
                                            // one shows GT4's Top Center view) / live (it shows GT4's Live view, the replay cameras)
        // ---- Network (live): GT4_NET, GT4_NET_IP, GT4_NET_INDEX, GT4_NET_PEERS
        int netMode = 0;                    // 0 off, 1 lan (other PCs), 2 loop (GT4 windows on this PC)
        std::string netIp;                  // lan: this PC's address (empty: automatic)
        int netIndex = 1;                   // loop: this window's number 1..16
        int netPeers = 6;                   // loop: GT4 windows on this PC 1..16 (6: GT4.exe's own default)
        // ---- Sound (live): GT4_ENGINE_BASS
        int engineBass = kFullEngineBassDefault; // 0 off, 1 low, 2 medium (the default), 3 high
        // ---- Sound > Sound packs (live with a GT4.exe that reads GT4_SOUND_PACK, FullFeatures::soundPacks)
        FullSoundMode soundMode = FullSoundMode::Off; // GT4L_SOUND_MODE off / all / cars
        bool soundModeSet = false;          // (reading only) a GT4L_SOUND_MODE line was read: it wins
        std::string soundPackAll;           // GT4L_SOUND_ALL: the pack for every car (or for the other cars, per car)
        std::vector<FullSoundCar> soundCars; // GT4L_SOUND_CARS
        std::string soundMapFile;           // where the map file goes (set by the launcher: <GT4.exe folder>\sound-packs\cars.txt)
        int soundTab = 0;                   // GT4L_SOUND_TAB: 0 Engine bass, 1 Sound packs
        // ---- the launcher's own state (GT4L_)
        int displayTab = 0;                 // GT4L_DISPLAY_TAB
        bool otherModesOpen = false;        // GT4L_OTHER_MODES_OPEN
        int previewZoom = 4;                // GT4L_PREVIEW_ZOOM 1..8
        std::vector<int> compareSlots;      // GT4L_COMPARE_CARS: garage slots, up to 4
        std::string compareStats;           // GT4L_COMPARE_STATS: the ticked stats (letters), empty = the default set
        int powerUnit = 1;                  // GT4L_COMPARE_UNITS: power 0 PS, 1 hp, 2 kW
        int torqueUnit = 0;                 //                     torque 0 Nm, 1 kgfm, 2 lb-ft
        bool dealerPay = true;              // GT4L_DEALER_PAY (Car Dealers: pay with my credits)
        int garageCard = 1;                 // GT4L_GARAGE_CARD: the memory card slot the Garage and Game progress read (1 / 2)
    };

    // Display > Resolution. The internal resolutions offered (GT3_GS_GL_SCALE 1..4; 6x and 8x are not for now) and the
    // drawing GT4.exe gets for a choice (GT3_GS_GL_FAST: "follow" is exact at native and 2x, fast at 3x and 4x, GT4.exe's
    // own rule; native is always exact). fullScaleBuilt: the scale passed this launcher's checks with GT4 (the full-m2
    // Resolution track's runs); fullScaleWhy: the reason a scale is greyed (empty when it is offered).
    constexpr int kFullScales = 4;
    const char *fullScaleLabel(int scale); // "Native", "2×", ...
    const char *fullScaleSize(int scale);  // "640 × 448", "1280 × 896", ...
    bool fullScaleBuilt(int scale);
    // The resolution presets (GT4L_RES_PRESET): the internal resolution a preset gives on a monitor monitorH pixels tall
    // (720p 2x, 1080p 3x, 1440p and 2160p 4x; auto: up to 800 rows 2x, up to 1200 3x, taller 4x), the biggest offered
    // scale at most; -1 for custom. Labels and keys in the presets' order (0 custom ... 5 2160p).
    constexpr int kFullResPresets = 6;
    int fullPresetScale(int preset, int monitorH);
    const char *fullPresetKey(int preset);
    const char *fullPresetLabel(int preset);
    const char *fullScaleWhy(int scale);
    // New York (GT4's busiest course) on the developer's RTX 5060 laptop with the drawing "follow" picks (the full-m2 runs): a short
    // note for the Resolution tab ("" at native), and the same with exact drawing at 3x and 4x (too slow there).
    const char *fullScaleSpeed(int scale);
    const char *fullScaleExactSpeed(int scale);
    bool fullDrawingFast(int scale, int drawing); // what GT3_GS_GL_FAST carries (false at native)

    // Multi-screen: the mode that is built (three screens, one wide picture: GT4_SCREENS=3) and what GT4_SCREENS_GEOMETRY
    // carries for the settings ("<width>,<height>,<gap>" in mm, the gap 0 with Panorama on).
    constexpr int kMmThreeWide = 4;
    std::string fullScreensGeometry(const FullSettings &f);

    // GT4's own Multi Monitor Mode (mode 1, analysis/design/multi-monitor; with a GT4.exe that has the marker
    // kMultiMonitorMarker): one GT4 copy per screen on this PC, linked by the LAN service on this PC (GT4_NET=loop), each
    // given its screen by GT4_MM_* (GT4.exe's gt4_multimon.h): the middle copy (the one you drive) hosts an Arcade
    // Multi-LAN Race as Play / Center, the side copies join as Monitor / Left and Right (and a top screen as Top Center
    // or Live). The copies in order: centre first, then left, right and top (when chosen).
    constexpr int kMmGt4Own = 1;
    struct FullMmCopy
    {
        std::string role;     // center, left, right, top
        std::string name;     // GT4's Console Name (CENTER, LEFT, ...)
        std::string position; // GT4_MM_POSITION
        std::string style;    // GT4_MM_STYLE: play, monitor or live
        int index = 1;        // GT4_NET_INDEX (127.77.0.(10 + index))
    };
    std::vector<FullMmCopy> fullMmCopies(const FullSettings &f);
    // GT4's Detail Monitor Settings for the rig (GT4_MM_GEOMETRY: flag, zoom, distance, width, height, aspect, scale, borders
    // left, right, top, bottom in mm): flag 1 for angled side screens (GT4 turns each to face you at its own angle for the
    // distance, fullMmGt4Angle), 0 for flat ones; with Panorama the frames are not allowed for (borders 0).
    std::string fullMmGeometry(const FullSettings &f);
    double fullMmGt4Angle(const FullSettings &f); // the side screens' angle GT4 uses, degrees
    // Every key one copy gets on top of the player's settings (the side copies: no sound device, no wheel, no player 2, no
    // EyeToy, no vibration, controller 1 following the middle copy until their network is up).
    std::vector<std::pair<std::string, std::string>> fullMmKeys(const FullSettings &f, const FullMmCopy &c, size_t copies, const std::string &group);

    // Race cameras (MoreCams): the six views in the launcher's order (GT4.exe's names for GT4_CAM_VIEWS / GT4_CAM_LOCK) and
    // the lists those keys carry for view bits ("none" for no view).
    inline constexpr const char *kFullCamViewKeys[6] = {"bumper", "bonnet", "roof", "close", "chase", "far"};
    std::string fullCamViewList(int bits);
    // A list read back into bits; false (and the bits read so far) if a name is not one of the six.
    bool fullCamViewBits(const std::string &list, int &bits);

    // The detailed FPS counter's stats (GT4_FPS_STATS names, GT4.exe's gt4_fps_counter.h), in the launcher's order, and their
    // labels on Display > Smoothness; the list GT4_FPS_STATS carries for stat bits ("all" for every one).
    inline constexpr int kFullFpsStatCount = 11;
    inline constexpr const char *kFullFpsStatKeys[kFullFpsStatCount] = {"fps", "frametime", "worst", "low1", "graph", "cpu",
                                                                        "gpu", "ram", "vram", "resolution", "renderer"};
    inline constexpr const char *kFullFpsStatLabels[kFullFpsStatCount] = {"Frames per second", "Frame time", "Worst frame (last second)",
                                                                          "1% low fps", "Frame-time graph", "CPU load",
                                                                          "GPU load", "RAM use", "VRAM use",
                                                                          "Internal resolution", "Renderer and GPU"};
    std::string fullFpsStatList(int bits);
    // A list read back into bits ("all" or empty: every stat); false if a name is not one of them.
    bool fullFpsStatBits(const std::string &list, int &bits);
    const char *fullFpsCounterKey(int mode); // off / simple / detailed

    // GT4.exe's network keys the Full edition manages (the Classic edition keeps them as written).
    std::string fullNetModeKey(int mode); // off / lan / loop

    // The Full edition's managed keys (GT4.exe keys and GT4L_ lines), added to launcherManagedKeys().
    const std::vector<std::string> &fullManagedKeys();

    // Sound packs: what GT4_SOUND_PACK / GT4_SOUND_PACK_MAP carry for the settings ("off" when not used), and the map file's
    // text (per car: one "<code> <folder>" line per car, "all <folder>" for the other cars when chosen).
    std::string fullSoundPackValue(const FullSettings &f);
    std::string fullSoundMapValue(const FullSettings &f);
    std::string fullSoundMapText(const FullSettings &f);
    // GT4L_SOUND_CARS: "<code>=<folder>|..." both ways (a bad entry is skipped; false when one was).
    std::string fullSoundCarsList(const std::vector<FullSoundCar> &cars);
    bool fullSoundCarsParse(const std::string &v, std::vector<FullSoundCar> &out);
    // The pack chosen for a car in per-car mode ("" none).
    std::string fullSoundPackFor(const FullSettings &f, uint32_t code);

    // A pack folder's pack.json as the Sound packs tab lists it (read only; a small reader, not a full JSON parser).
    struct SoundPackInfo
    {
        std::string folder;  // full path
        std::string name;    // "name" (else the folder's name)
        int on = 0, off = 0, pops = 0;
        double bassDb = 0.0;
        bool ok = false;     // pack.json found with format 1 and at least one on-throttle layer
        std::string problem; // why not
    };
    SoundPackInfo readSoundPackInfo(const std::string &folderUtf8);
    // The packs in a folder (its sub-folders holding pack.json), by name.
    std::vector<SoundPackInfo> listSoundPacks(const std::string &packsFolderUtf8);
}
