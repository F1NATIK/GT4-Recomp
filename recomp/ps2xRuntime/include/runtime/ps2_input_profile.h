#pragma once
#include <array>
#include <cstdint>
#include <istream>
#include <string>

namespace ps2_pad
{
    struct DeviceState
    {
        bool connected = false;
        int axisCount = 0;
        std::array<float, 8> axes{};
        std::array<bool, 128> buttons{}; // raw devices (wheels) have up to 128
    };
    using Devices = std::array<DeviceState, 4>;
    struct DeviceSource { bool raw = false; int joystick = 0; std::string guid; };
    struct DeviceIdentity { bool connected = false; bool mapped = false; std::string guid; };
    using DeviceInventory = std::array<DeviceIdentity, 16>;
    // GUID selectors require exactly one connected match. Never choose among duplicates.
    int resolveSource(const DeviceSource &source, const DeviceInventory &inventory);
    struct ButtonBinding { int device = 0; int button = -1; };
    struct AxisBinding
    {
        int device = 0;
        int axis = -1;
        float minimum = -1, center = 0, maximum = 1, deadzone = 0;
    };
    struct InputProfile
    {
        // Button bit order; stick packet order RX, RY, LX, LY; pressure byte order.
        std::array<ButtonBinding, 16> buttons;
        std::array<AxisBinding, 4> sticks;
        std::array<AxisBinding, 12> pressures;
        // Half-stick sources (e.g. a trigger): [stick][0] pushes it toward
        // 0 (up/left), [stick][1] toward 255 (down/right), by the pressed
        // fraction, added to the stick's own axis.
        std::array<std::array<AxisBinding, 2>, 4> stickHalves;
        bool keyboardFallback = true;
        // Left stick past this fraction also presses the D-pad (0 disables).
        float stickDpad = 0.f;
        // Controller motors: XInput user index (-1: first connected), gains.
        bool rumble = true;
        int rumbleXInput = -1;
        float rumbleSmall = 1.f, rumbleLarge = 1.f;
        std::array<DeviceSource, 4> sources{{{false, 0}, {false, 1}, {false, 2}, {false, 3}}};
        // A steering wheel and pedals (the launcher's Controls page, "Wheel &
        // pedals"), besides the controller: whichever of the two is further
        // from rest drives each PS2 control.
        // wheel-steer <device> <axis> <min> <center> <max> <lock> <gt3 deadzone>:
        // the wheel as the left stick's X; `lock` the fraction of the half
        // travel that gives full lock (wheel rotation set on the base vs the
        // lock wanted); GT3 ignores the first 37 / 127 of analog steering
        // (measured, gt3_autopilot.cpp), so the wheel's travel starts past it
        // (0: off) and the car turns from the first degree.
        AxisBinding wheelSteer;
        float wheelLock = 1.f, wheelGt3Deadzone = 37.f / 127.f;
        // wheel-pedal <name> <pressure target> <device> <axis> <released> <full> <deadzone>:
        // a pedal as a PS2 button's analog pressure (throttle -> R2, brake ->
        // L2 by default: GT3's Key Configuration Acceleration = R2, Brakes =
        // L2). Up to 3 (throttle, brake, clutch).
        struct Pedal
        {
            int target = -1; // pressure index (pressureNames), -1 none
            AxisBinding axis;
        };
        std::array<Pedal, 3> pedals;
        // wheel-button <ps2 button> <device> <button>: a wheel / pedal-box
        // button, besides the controller's.
        std::array<ButtonBinding, 16> wheelButtons{};
        static InputProfile defaults();
    };
    // Parses an entire profile transactionally, starting from defaults.
    bool readProfile(std::istream &stream, InputProfile &profile, std::string &error);
    std::array<uint8_t, 32> mapDevices(const InputProfile &profile, const Devices &devices);
    // What mapDevices keeps from one call to the next. The two-argument form uses its
    // own (player 1's); a second controller (GT4's player 2, Kernel/Stubs/Gt4Pad2.h)
    // keeps its own and does not publish the right stick (rightStick below).
    struct MapState
    {
        std::array<bool, 12> analogHeld{}; // analog press hysteresis per pressure button
        bool publishRightStick = true;
    };
    std::array<uint8_t, 32> mapDevices(const InputProfile &profile, const Devices &devices, MapState &state);
    // The physical right stick for the race camera's look-around
    // (gt3_camera.h): its position (the profile's rx / ry stick entries,
    // -1..1) is kept on every mapping; while claimed those entries no longer
    // reach the game (stick-half entries such as trigger pedals still do).
    void setRightStickClaimed(bool claimed);
    void rightStick(float &x, float &y);
    // GT3_PEDALS=triggers (the launcher's Controls page, default): the
    // triggers are GT3's analog pedals and nothing else. They drive the
    // right stick's up / down halves only (right trigger up = Acceleration,
    // left trigger down = Brakes; the profile's own ry halves, else axes 5 / 4
    // of controller 0); pressure entries fed by a trigger (axes 4 / 5: Cross,
    // Square, L2, R2 ...) are ignored, so X is only X. The runtime sets GT3's
    // Key Configuration to match (Acceleration = right stick up, Brakes =
    // right stick down) and the camera keeps the physical right stick in races.
    bool pedalsOnTriggers();
    void setPedalsOnTriggersForTest(int on); // -1: back to GT3_PEDALS
    bool hasBoundDevice(const InputProfile &profile, const Devices &devices);
}
