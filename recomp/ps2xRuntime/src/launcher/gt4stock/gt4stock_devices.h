#pragma once

// GT4 Recompilation launcher: game controllers, wheels and pedals.
//
// The list comes from DirectInput's device enumeration (nothing is opened for
// it). In a visible session the devices are also read passively: background,
// non-exclusive DirectInput (no force feedback can be sent that way), so the
// pages can show live bars. The force-feedback test is the only exclusive
// access, started by its button in a visible session with a wheel present.
// The hidden test mode passes allowOpen = false: enumeration only.
//
// Identities match the runtime's: the wheel adapter (gt4_wheel_di.cpp) picks a
// wheel by "vid:pid", "{instance GUID}" or name text with the same rules as
// pickWheel / pickPedals below; the pad mapping picks player 1's controller by
// its GLFW joystick GUID (ps2_pad.cpp, source-guid).

#include "gt4stock_settings.h"
#include "shared/gt4_pad2_rules.h"

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace gt4stock
{
    enum class DeviceKind
    {
        Gamepad,
        Wheel,
        Pedals,
        Joystick,
        Other,
    };

    struct DeviceInfo
    {
        std::string name;     // the instance name Windows shows
        uint16_t vid = 0, pid = 0;
        std::string instance; // "{xxxxxxxx-...}" DirectInput instance GUID
        uint32_t diType = 0;  // DI8DEVTYPE_* (low byte)
        DeviceKind kind = DeviceKind::Other;
        bool forceFeedback = false;
        bool xinput = false;  // an XInput controller (Xbox-compatible)
        bool demo = false;
        std::string glfwGuid; // the GUID GLFW (and the runtime) gives it
        // Live state (passive read or demo): axes x y z rx ry rz slider0 slider1
        // in -32768..32767 (DIPROP_RANGE, as the wheel adapter sets it).
        bool live = false;
        std::array<int32_t, 8> axes{};
        uint32_t axisMask = 0;
        std::array<uint8_t, 128> buttons{};
        int buttonCount = 0;
        int pov = -1; // hundredths of a degree clockwise from up; -1 centred

        std::string idText() const; // "0eb7:0006"
        const char *kindText() const;
    };

    // A GLFW joystick as the runtime sees it (visible sessions only).
    struct GamepadInfo
    {
        int joystick = -1;
        std::string name, guid;
        bool mapped = false; // GLFW has a gamepad mapping (the runtime's "mapped" source)
    };

    enum class FfbTest
    {
        CentreSpring, // a soft centring spring for 2.5 s
        LeftRight,    // a push one way, then the other (0.6 s each)
    };

    struct FfbTestParams
    {
        long magnitude = 0;    // constant force, DirectInput units (10000 = full)
        long coefficient = 0;  // spring
        unsigned long saturation = 0;
        unsigned durationMs = 0, attackMs = 0, fadeMs = 0;
    };
    // The strength a test plays at: the chosen one, never above the 25% cap, whatever
    // aboveCap says (the unlock is for GT4's own forces; the test has no rim-angle guard).
    float ffbTestStrength(float strength, bool aboveCap);
    // The test's forces for a strength (ffbTestStrength); the push itself is half of it.
    FfbTestParams ffbTestParams(FfbTest test, float strength, bool aboveCap);

    class Devices
    {
    public:
        Devices();
        ~Devices();
        Devices(const Devices &) = delete;
        Devices &operator=(const Devices &) = delete;

        // window: the launcher's HWND. allowOpen = false: enumeration only (the
        // hidden test mode). demo: a fixed demo set instead of the real list.
        void init(void *window, bool allowOpen, bool demo);
        void shutdown();
        // Re-enumerates every 2 s, reads the opened devices (or animates the demo).
        void update(double now);
        // While GT4 runs the launcher lets go of every device.
        void setPaused(bool paused);

        const std::vector<DeviceInfo> &list() const { return list_; }
        int openedCount() const { return openedTotal_; } // devices ever opened (0 in test mode)
        bool demo() const { return demo_; }
        const std::string &status() const { return status_; }

        // GLFW joysticks (visible sessions; the hidden test mode never calls GLFW's joystick API).
        std::vector<GamepadInfo> glfwGamepads() const;

        // Force-feedback test on that wheel (visible session, exclusive access while it runs).
        bool startFfbTest(const DeviceInfo &wheel, FfbTest test, float strength, bool aboveCap, std::string &message);
        void stopFfbTest();
        bool ffbTestRunning() const;

    private:
        struct Impl;
        std::unique_ptr<Impl> impl_;
        std::vector<DeviceInfo> list_;
        bool demo_ = false, allowOpen_ = false, paused_ = false;
        int openedTotal_ = 0;
        double lastEnum_ = -100.0;
        std::string status_;
    };

    // ---- pure helpers (unit-tested) ----
    std::string sdlGuidFromVidPid(uint16_t vid, uint16_t pid); // GLFW's "03000000vvvv0000pppp000000000000"
    DeviceKind kindFromDiType(uint32_t diType, const std::string &name, bool forceFeedback, uint16_t vid);
    bool wheelVendor(uint16_t vid);
    bool deviceMatches(const DeviceInfo &d, const std::string &spec); // vid:pid, {guid}, name text
    // The adapter's choice (gt4_wheel_di.cpp selectWheel / selectPedals).
    const DeviceInfo *pickWheel(const std::vector<DeviceInfo> &list, const std::string &spec);
    const DeviceInfo *pickPedals(const std::vector<DeviceInfo> &list, const std::string &spec, const DeviceInfo *wheel);
    // The demo set (the developer's: Fanatec Podium DD1, ClubSport Pedals V3 on USB, an
    // Xbox and a DualSense controller), with fixed live values.
    std::vector<DeviceInfo> demoDevices();

    // ---- player 2 (1.4): the player-2 runtime's own rule (shared/gt4_pad2_rules.h) over GLFW's joysticks
    // A device list (the demo set, or Windows' own list in the hidden test mode, which never calls GLFW's joystick
    // functions) as GLFW numbers it: XInput pads first, then the DirectInput devices (win32_joystick.c); a gamepad
    // is "mapped", a wheel or pedal set is not. A normal session reads GLFW's own list instead.
    gt4pad2rules::Inventory pad2InventoryFromList(const std::vector<DeviceInfo> &list);
    // Player 1's device sources as the runtime reads PS2X_INPUT (ps2_input_profile.cpp readProfile, then the
    // player-2 runtime's usedSlots): the four sources (default: slot k = mapped joystick k; "source" / "source-guid"
    // rows change them) and the slots some binding row uses (the built-in mapping uses slot 0; hand-written rows such
    // as "source 1 mapped 1; button cross 1 0" add others). The rows are the ones the launcher writes, kept rows included.
    struct Player1Source
    {
        bool raw = false;
        int joystick = 0;
        std::string guid; // lower case; empty = by number
    };
    struct Player1Sources
    {
        std::array<Player1Source, 4> sources{{{false, 0, {}}, {false, 1, {}}, {false, 2, {}}, {false, 3, {}}}};
        uint32_t usedSlots = 1; // bits
    };
    Player1Sources player1Sources(const PadSettings &pad);
    // ps2_input_profile.cpp resolveSource: a GUID must match exactly one connected joystick; a mapped source needs a
    // mapped joystick 0..3. -1 when it resolves to nothing.
    int resolvePlayer1Source(const Player1Source &s, const gt4pad2rules::Inventory &inv);
    // The joysticks player 1's profile reads now (every used slot's source resolved), as bits: Gt4Pad2.cpp player1Joysticks.
    uint32_t player1Joysticks(const Player1Sources &p, const gt4pad2rules::Inventory &inv);
    // The wheel adapter's device as the player-2 runtime takes it from PS2X_WHEEL_DEVICE (Gt4Pad2.cpp): only a literal
    // "vvvv:pppp" (hex) counts, whether or not PS2X_WHEEL is on; else 0.
    uint32_t adapterVidPid(const std::string &wheelDevice);
    // Player 2's joystick now (-1: none, `why` says why), for GT4_PAD2 = pad2Value(p), player 1's mapping `player1` and
    // PS2X_WHEEL_DEVICE `wheelDevice`, as the player-2 runtime decides it.
    int pickPlayer2(const Pad2Settings &p, const PadSettings &player1, const gt4pad2rules::Inventory &inv, const std::string &wheelDevice,
                    std::string *why);
}
