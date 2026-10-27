#pragma once

// GT4 player 2: the host side of PS2 controller port 2 (analysis/phase5b/player2).
// GT4 opens DBCMAN sockets for port 1 and port 2 at boot and polls both every frame;
// the IOP's DBCMAN (ps2xIOP gt4_dbcman.cpp) asks IopHost::readPortPadSnapshot(1)
// once per VBlank, and PS2IopHostAdapter answers from here. GT4 builds only.
//
// Settings (gt4-start.env or the environment):
//   GT4_PAD2 = off | auto | joystick:<0..3> | guid:<32 hex>   (default off: port 2 empty)
//     auto: the lowest-numbered mapped controller that is not player 1's and not a
//     wheel or pedals, while player 1 has a controller (runtime/gt4_pad2_rules.h). It
//     joins with a button press of its own: one made while player 1's controllers show
//     no input change, so a controller that only mirrors player 1's (one pad seen
//     twice through DS4Windows, Steam Input or a dual-mode pad) never joins. It is then
//     kept until it is unplugged, also while player 1's is out.
//     joystick / guid: that controller, at once.
//   PS2X_INPUT2 = player 2's input-profile rows (PS2X_INPUT's syntax, ';'-separated),
//     on top of the defaults. Device slot 0 is always player 2's controller; slots 1..3
//     are read only when they are not one of player 1's joysticks; a `keyboard` row is
//     ignored (the keyboard stays player 1's fallback).
//   GT4_PAD2_REPLAY = <file> (tests only; compiled out of GT4_STOCK_BUILD, which logs it
//     as ignored, and not on its allowlist): GT3_PAD_REPLAY's lines (begin end
//     buttons-hex [rx ry lx ly]) plus "begin end unplug"; port 2 is then plugged in
//     (neutral between lines) except inside unplug lines, whatever GT4_PAD2 says, and
//     host controllers are not used for port 2.
// Vibration: player 2's motors go to player 2's controller only (XInput): PS2X_INPUT2's
// "rumble xinput <n>"; else, with one Xbox-type controller connected, that one; with more,
// the one XInput pad whose input (buttons, sticks, triggers) equals player 2's joystick's
// (none while two match, e.g. two idle virtual pads). Player 1's automatic choice skips
// that pad (player1XInputPad). GT3_VIBRATION=0 silences both.
// Threads: poll() reads devices and calls XInput on the window thread with no lock held;
// readState() (DBCMAN's VBlank, the EE scheduler thread) only copies 32 published bytes
// under a short lock, so a slow controller never holds up a VBlank.
// Hidden runs (--headless / --cpu-only) never poll host controllers, so port 2 is
// empty there unless GT4_PAD2_REPLAY is set.

#include "runtime/ps2_input_profile.h"
#include "runtime/ps2_pad_rumble.h"

#include <array>
#include <cstdint>
#include <functional>
#include <string>

namespace gt4pad2
{
    // Reads the settings from the environment once and logs them ("[GT4/PAD2] ...");
    // later calls return the same text. The other calls configure on first use.
    std::string configure();

    // Window thread (PSPadBackend::poll), with GLFW's joysticks and player 1's
    // profile: picks / keeps player 2's controller, reads it, drives its motors.
    void poll(const ps2_pad::DeviceInventory &inventory, const ps2_pad::InputProfile &player1);

    // Any thread (DBCMAN's VBlank): player 2's 32-byte snapshot (PSPadBackend::readState's
    // layout). false = nothing plugged into port 2.
    bool readState(uint64_t vsync, std::array<uint8_t, 32> &packet);

    // Any thread (DBCMAN): player 2's DualShock 2 motors as GT4 drives them. GT3_RUMBLE_LOG
    // prints every call ("[input] DualShock 2 port 2 motors: ..."), else the first non-zero one.
    void setActuators(bool smallMotor, uint8_t large, uint64_t vsync);

    // Player 2's XInput index while it holds one, else -1.
    int claimedXInput();
    // Player 1's automatic vibration pad: ps2_pad::firstXInputPad() while player 2
    // holds no XInput pad, else the first connected one that is not player 2's.
    int player1XInputPad();

    // Window thread, when the game has stopped (exit skips destructors): player 2's
    // motors off if this process set them.
    void stopRumble();

    // The seams the tests fake: the environment and XInput (ps2_pad_rumble.h in GT4.exe).
    using EnvLookup = std::function<const char *(const char *)>;
    struct XInputLayer
    {
        std::function<unsigned(unsigned, ps2_pad::XInputPadState *)> pads; // ps2_pad::readXInputPads
        std::function<bool(int, uint16_t, uint16_t)> rumble;               // index, low (large motor), high (small motor)
        std::function<double()> now;                                       // seconds
    };

#if !defined(GT4_STOCK_BUILD)
    // ---- tests (analysis/phase5b/player2/tests); never used by the runtime; not in the stock build ----
    struct TestJoystick
    {
        bool connected = false, mapped = false;
        std::string guid, name;
        ps2_pad::DeviceState state; // what reading it returns (connected follows the fields above)
        bool hasRaw = false;        // raw reads (GLFW button order) return `raw` instead of `state`
        ps2_pad::DeviceState raw;
    };
    using TestInventory = std::array<TestJoystick, 16>;
    // Starts over with these settings (stockBuild: as GT4_STOCK_BUILD); returns the log text.
    std::string configureForTest(const EnvLookup &env, bool stockBuild);
    void pollForTest(const TestInventory &joysticks, const ps2_pad::InputProfile &player1, const XInputLayer &xinput);
    // Every "[GT4/PAD2]" line also goes here (empty function: stderr only).
    void setLogSinkForTest(std::function<void(const std::string &)> sink);
    // Called on every device read pollForTest makes (empty function: none).
    void setDeviceReadHookForTest(std::function<void()> hook);
    void stopRumbleForTest(const XInputLayer &xinput);
    int joystickForTest(); // player 2's joystick, -1 none
#endif
}
