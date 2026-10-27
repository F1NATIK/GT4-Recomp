#pragma once

// GT4 Recompilation launcher (Stock edition): every setting, and the one
// settings file GT4.exe reads.
//
// The file is the stock package's start file, gt4-start.env (KEY=VALUE, one
// per line, '#' comments; main.cpp portableStart). The launcher writes the
// keys the stock-scanline, wheel-glue and phase 5b work defined, plus four new
// ones (analysis/gt4-stock-launcher/README.md, "The settings file"):
//   GT4_MONITOR        the monitor GT4 opens on: "<x>,<y> <name>"
//   PS2X_INPUT         the controller mapping: input-profile rows separated
//                      by ';' (the PS2X_INPUT_PROFILE file syntax)
//   PS2X_WHEEL_LINES   wheel config lines separated by ';' (the
//                      PS2X_WHEEL_CONFIG file syntax)
//   GT4_MC0 / GT4_MC1  the memory card slots (the ps2-card patch,
//                      analysis/saves/ps2-card): folder / none / a .ps2 file
//   GT4L_*             the launcher's own state; GT4.exe ignores them
// GT4_EYETOY is the EyeToy service's own key (analysis/phase5b/eyetoy):
// absent / shirt / image:<file> / webcam:<n>.
// 1.4: GT4_CHEATS and GT4_CHEAT_CARD / _LIST / _KEY / _PLACE / _FOLLOW (the cheats runtime,
// analysis/cheats/runtime) and GT4_PAD2 (the player-2 runtime, analysis/phase5b/player2), all on
// the stock build's allowlist in those patches; their test-only switches (GT4_CHEAT_CARD_TEST,
// GT4_CHEATS_TEST_NODAY, GT4_PAD2_REPLAY) are never written.
// Keys it does not manage are kept as they were found.

#include <array>
#include <filesystem>
#include <string>
#include <utility>
#include <vector>

#if defined(GT4L_EDITION_FULL)
#include "gt4full_settings.h" // the Full edition's own settings (Display tabs, multi-screen, network, garage)
#include <functional>
#include <iosfwd>
#endif

namespace gt4stock
{
    enum class Fit
    {
        Auto,
        Fill,
        Integer,
    };
    const char *fitKey(Fit f);   // auto / fill / integer
    const char *fitLabel(Fit f); // Auto / Fill / Whole rows

    // How GT4.exe scales the picture (GT4_PICTURE_FILTER), as PCSX2's display filter offers it.
    enum class PictureFilter
    {
        Nearest, // nearest: every PS2 pixel a hard-edged block (pixel-perfect)
        Sharp,   // sharp: bilinear (sharp), crisp pixels with soft edges
        Smooth,  // smooth: bilinear (smooth), the default
    };
    const char *pictureFilterKey(PictureFilter f);   // nearest / sharp / smooth
    const char *pictureFilterLabel(PictureFilter f); // Nearest / Sharp / Smooth

    // How GT4.exe makes one picture of the two interlaced fields (GT4_DEINTERLACE).
    enum class Deinterlace
    {
        Weave,    // weave: both fields' lines together, as they are (no deinterlacing; GT4.exe also takes "off")
        Bob,      // bob: the newest field, the lines between filled in
        Blend,    // blend: woven, each line blended a little with its neighbours (the default)
        Adaptive, // adaptive: woven where the picture is still, bob where it moves
    };
    const char *deinterlaceKey(Deinterlace d);   // weave / bob / blend / adaptive
    const char *deinterlaceLabel(Deinterlace d); // Weave / Bob / Blend / Adaptive

    // The EyeToy camera GT4 sees (GT4_EYETOY). GT4 used the EyeToy for one thing: the
    // Nike One 2022 unlock (GT Mode, USA, the Nike dealer) by recognising the GT shirt design.
    enum class EyeToy
    {
        Off,    // absent: no camera, as a PS2 without one (the default)
        Shirt,  // shirt: the built-in GT shirt picture (read from the disc); no camera needed
        Webcam, // webcam:<n>: a webcam, chosen by name (GT4L_EYETOY_CAMERA)
        Image,  // image:<file>: a still picture (written by hand; kept)
    };
    const char *eyeToyLabel(EyeToy e); // "Off", "Built-in GT shirt picture", "Webcam", "Picture file"

    // DualShock 2 controls in the input profile's order (ps2_input_profile.cpp).
    constexpr int kPadButtons = 16;
    constexpr int kPadPressures = 12;
    constexpr int kPadSticks = 4;
    extern const char *const kPadButtonKeys[kPadButtons];   // select l3 r3 start up right down left l2 r2 l1 r1 triangle circle cross square
    extern const char *const kPadButtonLabels[kPadButtons]; // "Select", "L3", ...
    extern const char *const kPadPressureKeys[kPadPressures];
    extern const char *const kPadStickKeys[kPadSticks]; // rx ry lx ly
    // The pressure index of a button (profile order), or -1 (Select, L3, R3, Start).
    int pressureOfButton(int button);

    // raylib gamepad numbering (the runtime reads mapped controllers through it).
    constexpr int kGamepadButtonCount = 18; // 0 unknown, 1..17
    constexpr int kGamepadAxisCount = 6;    // left x, left y, right x, right y, left trigger, right trigger
    const char *gamepadButtonName(int button, bool playstation); // "A", "Cross", "LB", ...
    const char *gamepadAxisName(int axis, bool playstation);     // "Right trigger", "R2 trigger", ...

    struct PadSettings
    {
        std::string sourceGuid; // GLFW joystick GUID of player 1's controller; empty = the first controller
        std::array<int, kPadButtons> buttons{};     // raylib gamepad button, -1 none
        std::array<int, kPadPressures> pressure{};  // raylib gamepad axis feeding the pressure, -1 none
        float triggerDeadzone = 0.02f;              // pressure sources (triggers): the start point (travel before any pressure)
        float triggerFull = 1.0f;                   // the end point: the share of the travel where the pressure is full (0.5..1)
        std::array<int, kPadSticks> stickAxis{};    // raylib axis per rx ry lx ly
        std::array<bool, kPadSticks> stickInvert{};
        float leftDeadzone = 0.0f, rightDeadzone = 0.0f;
        bool keyboard = true;
        float rumble = 1.0f; // both motors' strength 0..1
        std::vector<std::string> extraRows; // rows the launcher does not edit, kept

        PadSettings(); // the runtime's built-in mapping (InputProfile::defaults)
        void applyStandard();          // DualShock layout by position, triggers = L2 / R2 (analog)
        void applyTriggersOnFaceButtons(); // right trigger = Cross, left = Square (analog), L2 / R2 unmapped
        bool usesTriggersOnFaceButtons() const;
    };
    std::string padProfileRows(const PadSettings &pad);  // "source 0 mapped 0; ..."
    bool parsePadProfileRows(const std::string &rows, PadSettings &pad, std::string &error);

    // Wheel config axis names (gt4_wheel_di.cpp parseDiAxis).
    constexpr int kWheelAxes = 8;
    extern const char *const kWheelAxisKeys[kWheelAxes];   // x y z rx ry rz slider0 slider1
    extern const char *const kWheelAxisLabels[kWheelAxes]; // "X axis" ... "Slider 2" (as Windows' game controller panel names them)
    const char *wheelAxisLabel(const std::string &key);     // "None" for none
    // DFP button names a host button can give (gt4_wheel_di.cpp ButtonNames).
    constexpr int kWheelButtonNames = 15;
    extern const char *const kWheelButtonKeys[kWheelButtonNames];   // none cross square circle triangle r1 l1 r2 l2 select start b10 b11 b12 b13
    extern const char *const kWheelButtonLabels[kWheelButtonNames]; // "Not used", "Cross", ...
    int wheelButtonIndex(const std::string &key);

    struct PedalSettings
    {
        std::string axis = "y"; // a kWheelAxisKeys name, or "none"
        bool calibrated = false;
        int released = 0, full = 0; // raw -32768..32767 when calibrated
    };

    struct WheelSettings
    {
        bool enabled = false;          // PS2X_WHEEL=dinput
        std::string device = "auto";   // auto | vid:pid | {guid} | name text
        std::string deviceName;        // shown while it is unplugged (GT4L_WHEEL_NAME)
        std::string pedals = "wheel";  // wheel | auto | vid:pid ...
        std::string pedalsName;        // GT4L_PEDALS_NAME
        std::string model = "dfp";     // dfp | df | gtforce
        int rotation = 900;            // the base's own lock to lock (degrees)
        std::string steerAxis = "x";
        bool steerInvert = false;
        PedalSettings gas, brake;
        float pedalDeadzone = 0.02f;
        std::array<int, 128> buttons{}; // host button -> kWheelButtonKeys index (0 none)
        int hat = 0;                    // POV 0..3, -1 none
        bool ffb = false;
        float ffbGain = 0.10f;          // 0..1 master strength (10%: the suggested first test)
        bool ffbAboveCap = false;       // GT4L_FFB_ABOVE_CAP: the slider may go past 25%
        bool ffbInvert = false;
        float ffbSlew = 4.0f;
        bool ffbPauseUnfocused = true;
        std::array<bool, 4> effects{true, true, true, true}; // constant spring damper friction
        std::vector<std::string> extraLines;

        WheelSettings(); // the adapter's defaults (DFP buttons 0..13, gas y, brake rz)
    };
    constexpr float kFfbCap = 0.25f;       // default strength cap
    constexpr float kFfbFirstTest = 0.10f; // suggested first test
    std::string wheelConfigLines(const WheelSettings &wheel);
    bool parseWheelConfigLines(const std::string &lines, WheelSettings &wheel, std::string &error);

    // GT4's button codes (1.4, the Cheats page; analysis/cheats/CHEATS-TAB-DESIGN.md 8 and the cheats
    // runtime's readers, gt4_cheat_overlay.cpp / gt4_cheat_card.cpp). Every switch is off by default:
    // with all of them off GT4.exe reads the disc unchanged and draws nothing extra.
    struct CheatSettings
    {
        // GT4_CHEATS, a comma list (absent = off): GT4's menu scripts changed in memory as they are read.
        bool noday = false;      // noday: the 365-day rule of the five codes skipped
        bool secret = false;     // secret: controller 1 may type the hidden Options code
        bool missionfix = false; // missionfix: the Mission Hall code passes the highlighted mission (MISSION-FIX.md)
        std::vector<std::string> otherWords; // GT4_CHEATS words this launcher does not show (kept)
        // GT4_CHEAT_CARD=off | key (F2 shows the ticked cards in GT4). "auto" (written by hand) is kept.
        bool card = false;
        bool cardAuto = false;
        std::string key = "F2";   // GT4_CHEAT_CARD_KEY: F2..F12 (developer: F2); written whenever it is not F2
        std::string place;        // GT4_CHEAT_CARD_PLACE (no control on the page; a hand-written value is kept;
                                  // the runtime's "topright" and "corner" are written as top-right)
        std::string follow;       // GT4_CHEAT_CARD_FOLLOW (the same)
        // The ticked cards (GT4L_CHEATS_TICKED; GT4_CHEAT_CARD_LIST while the card is on), in the order F2 shows them:
        // a hand-written order is kept, a card ticked on the page goes last.
        std::vector<std::string> ticked;
        bool letter = false;      // GT4L_CHEATS_PAPER=letter (default a4)
        bool any() const { return noday || secret || missionfix || card || !otherWords.empty(); }
    };

    // Player 2: the DualShock 2 in PS2 controller port 2 (1.4; GT4_PAD2, the player-2 runtime's key,
    // analysis/phase5b/player2/PLAYER2-DESIGN.md 4: auto / off / joystick:<0..3> / guid:<32 hex>).
    struct Pad2Settings
    {
        enum class Mode
        {
            Automatic, // auto: the second controller, by the shared rule
            Off,       // off (the default; also an absent key, as GT4.exe reads it): port 2 empty
            Chosen,    // guid:<id> (a controller picked by name) or joystick:<n> (written by hand, kept)
        } mode = Mode::Off;
        std::string guid;  // lower case, 32 hex
        int joystick = -1; // joystick:<n>
        std::string name;  // GT4L_PAD2_NAME: the chosen controller's name, shown while it is unplugged
        // (1.4 review) GT4_PAD2 is written only when it was in the file or picked on the page: an untouched
        // default leaves the key out, so the player-2 runtime's own default decides. (1.4.1) That default is
        // off (absent = off, gt4_pad2_rules.h), so an untouched Player 2 is shown as Off.
        bool explicitChoice = false;
    };
    std::string pad2Value(const Pad2Settings &p); // the GT4_PAD2 value written

    struct Settings
    {
        // Disc
        std::string disc; // GT4_DISC_IMAGE (UTF-8 path)
        // Picture
        bool linesOn = true; // GT3_DISPLAY_FILTER crt / off
        int strength = 50;   // GT3_CRT_SCANLINES
        int field = 0;       // GT4_SCANLINE_FIELD
        Fit fit = Fit::Auto; // GT4_PICTURE_FIT
        PictureFilter filter = PictureFilter::Smooth;  // GT4_PICTURE_FILTER
        Deinterlace deinterlace = Deinterlace::Blend; // GT4_DEINTERLACE
        // Window
        bool fullscreen = true;          // GT3_FULLSCREEN
        int windowW = 1280, windowH = 960; // GT3_WINDOW_SIZE
        bool monitorSet = false;         // GT4_MONITOR
        int monitorX = 0, monitorY = 0;
        std::string monitorName;
        // Controls
        bool vibration = true; // GT3_VIBRATION
        PadSettings pad;       // PS2X_INPUT
        Pad2Settings pad2;     // GT4_PAD2 (1.4)
        WheelSettings wheel;   // PS2X_WHEEL*
        // GT4's button codes (1.4)
        CheatSettings cheats;  // GT4_CHEATS, GT4_CHEAT_CARD*, GT4L_CHEATS_*
        // Memory cards (GT4_MC0 / GT4_MC1, read by GT4.exe with the ps2-card patch):
        // slot 1 the folder card game\mc0 or a .ps2 card file; slot 2 empty or a file.
        // A relative path is taken from the game folder (next to CORE.elf), as GT4.exe does.
        struct CardSlot
        {
            bool file = false;
            std::string path; // the .ps2 file (UTF-8)
        };
        std::array<CardSlot, 2> cards{};
        // Devices
        bool usb = true;     // GT4_USB
        std::string usbRoot; // GT4_USB_ROOT (empty: game\usb0); the photos are in <root>\GT4
        // The EyeToy camera (GT4_EYETOY, read by GT4.exe with the EyeToy service).
        EyeToy eyetoy = EyeToy::Off;
        std::string eyetoyCamera; // GT4L_EYETOY_CAMERA: the webcam's name (Webcam)
        int eyetoyIndex = -1;     // its place in Media Foundation's list (webcam:<n>); -1 not connected
        std::string eyetoyImage;  // the picture file (Image)
        // Launcher
        std::string preview = "race"; // GT4L_PREVIEW
        std::string exe;              // GT4L_EXE (empty: found by the launcher)
        std::string lastPlayed;       // GT4L_LAST_PLAYED
        int lastExit = 0;             // GT4L_LAST_EXIT
        int lastMinutes = -1;         // GT4L_LAST_MINUTES
        // Keys found in the file that the launcher does not manage, kept in order.
        std::vector<std::pair<std::string, std::string>> other;
#if defined(GT4L_EDITION_FULL)
        FullSettings full; // the Full edition's own (gt4full_settings.h)
#endif
    };

#if defined(GT4L_EDITION_FULL)
    // The Full edition's part of the settings file (gt4full_settings.cpp). parseFullSetting: true when the key is the Full
    // edition's (read into s; bad values go to bad()); the Classic reader handles the rest. The render functions write the
    // Full edition's sections in place of (picture, window) or after (renderer, network, not built yet, launcher state)
    // the Classic ones.
    bool parseFullSetting(const std::string &key, const std::string &value, Settings &s, const std::function<void(const char *)> &bad);
    void renderFullPicture(std::ostream &o, const Settings &s);
    void renderFullMore(std::ostream &o, const Settings &s);
    void renderFullLauncher(std::ostream &o, const Settings &s);
#endif

    // Reads the file over the defaults; problems (bad values) go to warnings,
    // the rest of the file still applies. A missing file is not an error.
    bool loadSettings(const std::filesystem::path &file, Settings &settings, std::vector<std::string> &warnings);
    bool parseSettingsText(const std::string &text, Settings &settings, std::vector<std::string> &warnings);
    // The whole file, commented, as the launcher writes it.
    std::string renderSettings(const Settings &settings, const std::string &version);
    bool saveSettings(const std::filesystem::path &file, const Settings &settings, const std::string &version, std::string &error);
    // KEY=VALUE pairs GT4.exe gets (the managed keys GT4.exe reads; no GT4L_ keys), plus GT4_CHEATS=off and
    // GT4_CHEAT_CARD=off when they are off (never left out). withOther: also the kept keys the launcher does not manage.
    std::vector<std::pair<std::string, std::string>> settingsEnvironment(const Settings &settings, bool withOther);
    // (1.4.2) The renderer PLAY gives GT4.exe as GT3_RENDERER: "gl" for a GT4 program the launcher read that is not a
    // Classic build (a development build draws on the CPU without it: races at 18-33 fps), unless the settings already
    // name one (a kept GT3_RENDERER line, any spelling, e.g. vk). Empty: nothing to add (a Classic build always draws
    // with OpenGL and ignores the key).
    std::string launchRenderer(bool exeRead, bool exeClassic, const Settings &settings);
    // Every key the launcher writes (and therefore owns), GT4L_ keys included.
    const std::vector<std::string> &launcherManagedKeys();
    // Test and diagnostic switches of GT4.exe (shared/gt4_start_keys.h: replays, the test pilot, video-mode overrides,
    // shots, dumps, the compare), in any spelling (Windows' environment ignores case). The launcher never writes them
    // and never passes them on: a hand-written one is dropped, with a warning.
    bool isTestOnlyKey(const std::string &key);
    // The GT4_EYETOY value written: absent, shirt, image:<file>, or webcam:<n> (absent while
    // the chosen webcam is not connected, so GT4 never opens another camera in its place).
    std::string eyeToyValue(const Settings &settings);
}
