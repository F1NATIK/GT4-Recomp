#include "runtime/ps2_input_profile.h"
#include "runtime/ps2_pad_pressure.h"
#include "ps2_host_backend.h"
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <sstream>

namespace ps2_pad
{
    namespace
    {
        std::atomic<bool> g_rightStickClaimed{false};
        std::atomic<float> g_rightStickX{0.f}, g_rightStickY{0.f};
    }
    void setRightStickClaimed(bool claimed) { g_rightStickClaimed.store(claimed, std::memory_order_relaxed); }
    namespace
    {
        std::atomic<int> g_pedalsOverride{-1}; // tests: -1 = GT3_PEDALS
    }
    void setPedalsOnTriggersForTest(int on) { g_pedalsOverride.store(on, std::memory_order_relaxed); }
    bool pedalsOnTriggers()
    {
#if defined(GT3_STOCK_BUILD)
        return false; // the stock build: GT3's own controls
#else
        if (const int o = g_pedalsOverride.load(std::memory_order_relaxed); o >= 0)
            return o != 0;
        static const bool on = [] {
            const char *v = std::getenv("GT3_PEDALS");
            return v && std::string(v) == "triggers";
        }();
        return on;
#endif
    }
    void rightStick(float &x, float &y)
    {
        x = g_rightStickX.load(std::memory_order_relaxed);
        y = g_rightStickY.load(std::memory_order_relaxed);
    }

    int resolveSource(const DeviceSource &source, const DeviceInventory &inventory)
    {
        int selected = source.joystick;
        if (!source.guid.empty())
        {
            selected = -1;
            for (int i = 0; i < static_cast<int>(inventory.size()); ++i)
                if (inventory[i].connected && inventory[i].guid == source.guid)
                {
                    if (selected >= 0) return -1;
                    selected = i;
                }
        }
        if (selected < 0 || selected >= static_cast<int>(inventory.size()) ||
            !inventory[selected].connected || (!source.raw && (selected >= 4 || !inventory[selected].mapped))) return -1;
        return selected;
    }

    namespace
    {
        constexpr const char *buttonNames[] = {"select", "l3", "r3", "start", "up", "right", "down", "left",
            "l2", "r2", "l1", "r1", "triangle", "circle", "cross", "square"};
        constexpr const char *stickNames[] = {"rx", "ry", "lx", "ly"};
        constexpr const char *pressureNames[] = {"right", "left", "up", "down", "triangle", "circle",
            "cross", "square", "l1", "r1", "l2", "r2"};
        template<std::size_t N> int index(const char *const (&names)[N], const std::string &name)
        {
            for (std::size_t i = 0; i < N; ++i) if (name == names[i]) return static_cast<int>(i);
            return -1;
        }
        bool axisValue(const AxisBinding &b, const Devices &devices, float &value)
        {
            if (b.device < 0 || b.device >= 4 || b.axis < 0 || b.axis >= 8) return false;
            const auto &d = devices[b.device];
            if (!d.connected || b.axis >= d.axisCount || !std::isfinite(d.axes[b.axis])) return false;
            value = d.axes[b.axis];
            return true;
        }
        uint8_t pressure(float value, const AxisBinding &b)
        {
            const float fraction = std::clamp((value - b.minimum) / (b.maximum - b.minimum), 0.f, 1.f);
            return static_cast<uint8_t>(std::lround(std::max(0.f, fraction - b.deadzone) / (1.f - b.deadzone) * 255.f));
        }
    }

    InputProfile InputProfile::defaults()
    {
        InputProfile p;
        const int buttons[] = {GAMEPAD_BUTTON_MIDDLE_LEFT, GAMEPAD_BUTTON_LEFT_THUMB, GAMEPAD_BUTTON_RIGHT_THUMB,
            GAMEPAD_BUTTON_MIDDLE_RIGHT, GAMEPAD_BUTTON_LEFT_FACE_UP, GAMEPAD_BUTTON_LEFT_FACE_RIGHT,
            GAMEPAD_BUTTON_LEFT_FACE_DOWN, GAMEPAD_BUTTON_LEFT_FACE_LEFT, GAMEPAD_BUTTON_LEFT_TRIGGER_2,
            GAMEPAD_BUTTON_RIGHT_TRIGGER_2, GAMEPAD_BUTTON_LEFT_TRIGGER_1, GAMEPAD_BUTTON_RIGHT_TRIGGER_1,
            GAMEPAD_BUTTON_RIGHT_FACE_UP, GAMEPAD_BUTTON_RIGHT_FACE_RIGHT, GAMEPAD_BUTTON_RIGHT_FACE_DOWN,
            GAMEPAD_BUTTON_RIGHT_FACE_LEFT};
        for (int i = 0; i < 16; ++i) p.buttons[i].button = buttons[i];
        p.sticks[0].axis = GAMEPAD_AXIS_RIGHT_X; p.sticks[1].axis = GAMEPAD_AXIS_RIGHT_Y;
        p.sticks[2].axis = GAMEPAD_AXIS_LEFT_X; p.sticks[3].axis = GAMEPAD_AXIS_LEFT_Y;
        p.pressures[10].axis = GAMEPAD_AXIS_LEFT_TRIGGER;
        p.pressures[11].axis = GAMEPAD_AXIS_RIGHT_TRIGGER;
        return p;
    }

    bool readProfile(std::istream &stream, InputProfile &profile, std::string &error)
    {
        auto candidate = InputProfile::defaults();
        std::string line; unsigned number = 0;
        while (std::getline(stream, line))
        {
            ++number;
            line = line.substr(0, line.find('#'));
            std::istringstream row(line);
            std::string kind, name, extra;
            if (!(row >> kind)) continue;
            bool ok = false;
            if (kind == "keyboard")
            {
                int enabled = -1;
                ok = bool(row >> enabled) && (enabled == 0 || enabled == 1);
                candidate.keyboardFallback = enabled == 1;
            }
            else if (kind == "stick-half" && (row >> name >> extra))
            {
                // stick-half <rx|ry|lx|ly> <up|down|left|right> device axis min max deadzone
                AxisBinding b;
                const int i = index(stickNames, name);
                const bool vertical = i == 1 || i == 3;
                const int half = extra == (vertical ? "up" : "left") ? 0 : extra == (vertical ? "down" : "right") ? 1 : -1;
                ok = i >= 0 && half >= 0 && bool(row >> b.device >> b.axis >> b.minimum >> b.maximum >> b.deadzone) &&
                    b.device >= 0 && b.device < 4 && b.axis >= -1 && b.axis < 8 && std::isfinite(b.minimum) &&
                    std::isfinite(b.maximum) && std::isfinite(b.deadzone) && b.deadzone >= 0 && b.deadzone < 1 &&
                    std::abs(b.maximum - b.minimum) > 0.000001f;
                if (ok) candidate.stickHalves[i][half] = b;
                extra.clear();
            }
            else if (kind == "stick-dpad")
            {
                float threshold = -1;
                ok = bool(row >> threshold) && threshold >= 0 && threshold <= 1;
                if (ok) candidate.stickDpad = threshold;
            }
            else if (kind == "rumble" && (row >> name))
            {
                // rumble off | rumble xinput <auto|0..3> <small 0..1> <large 0..1>
                std::string index;
                if (name == "off") { ok = true; candidate.rumble = false; }
                else if (name == "xinput" && (row >> index >> candidate.rumbleSmall >> candidate.rumbleLarge))
                {
                    const bool automatic = index == "auto";
                    const int user = automatic ? -1 : std::atoi(index.c_str());
                    ok = (automatic || (index.size() == 1 && user >= 0 && user < 4)) &&
                        candidate.rumbleSmall >= 0 && candidate.rumbleSmall <= 1 &&
                        candidate.rumbleLarge >= 0 && candidate.rumbleLarge <= 1;
                    candidate.rumble = true; candidate.rumbleXInput = user;
                }
            }
            else if (kind == "source-guid")
            {
                int slot = -1;
                std::string guid;
                ok = bool(row >> slot >> name >> guid) && slot >= 0 && slot < 4 &&
                    (name == "raw" || name == "mapped") && guid.size() == 32;
                for (char &c : guid)
                {
                    if (c >= 'A' && c <= 'F') c = char(c - 'A' + 'a');
                    ok = ok && ((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'));
                }
                if (ok) candidate.sources[slot] = {name == "raw", -1, guid};
            }
            else if (kind == "source")
            {
                int slot = -1, joystick = -1;
                ok = bool(row >> slot >> name >> joystick) && slot >= 0 && slot < 4 &&
                    (name == "raw" || name == "mapped") && joystick >= 0 &&
                    joystick < (name == "raw" ? 16 : 4);
                if (ok) candidate.sources[slot] = {name == "raw", joystick};
            }
            else if (kind == "button" && (row >> name))
            {
                ButtonBinding b;
                int i = index(buttonNames, name);
                ok = i >= 0 && bool(row >> b.device >> b.button) && b.device >= 0 && b.device < 4 && b.button >= -1 && b.button < 128;
                if (ok) candidate.buttons[i] = b;
            }
            else if (kind == "wheel-steer")
            {
                // wheel-steer device axis min center max lock gt3-deadzone
                AxisBinding b;
                float lock = 1.f, gt3Deadzone = 0.f;
                ok = bool(row >> b.device >> b.axis >> b.minimum >> b.center >> b.maximum >> lock >> gt3Deadzone) && b.device >= 0 &&
                    b.device < 4 && b.axis >= -1 && b.axis < 8 && std::isfinite(b.minimum) && std::isfinite(b.center) &&
                    std::isfinite(b.maximum) && b.center > std::min(b.minimum, b.maximum) && b.center < std::max(b.minimum, b.maximum) &&
                    std::isfinite(lock) && lock > 0.01f && lock <= 1.f && std::isfinite(gt3Deadzone) && gt3Deadzone >= 0.f && gt3Deadzone < 0.9f;
                if (ok)
                {
                    candidate.wheelSteer = b;
                    candidate.wheelLock = lock;
                    candidate.wheelGt3Deadzone = gt3Deadzone;
                }
            }
            else if (kind == "wheel-pedal" && (row >> name >> extra))
            {
                // wheel-pedal <throttle|brake|clutch> <pressure target|none> device axis released full deadzone
                static const char *pedalNames[] = {"throttle", "brake", "clutch"};
                const int slot = index(pedalNames, name);
                const int target = extra == "none" ? -1 : index(pressureNames, extra);
                AxisBinding b;
                ok = slot >= 0 && (target >= 0 || extra == "none") && bool(row >> b.device >> b.axis >> b.minimum >> b.maximum >> b.deadzone) &&
                    b.device >= 0 && b.device < 4 && b.axis >= -1 && b.axis < 8 && std::isfinite(b.minimum) && std::isfinite(b.maximum) &&
                    std::isfinite(b.deadzone) && b.deadzone >= 0 && b.deadzone < 1 && std::abs(b.maximum - b.minimum) > 0.000001f;
                if (ok)
                    candidate.pedals[slot] = InputProfile::Pedal{target, b};
                extra.clear();
            }
            else if (kind == "wheel-button" && (row >> name))
            {
                ButtonBinding b;
                const int i = index(buttonNames, name);
                ok = i >= 0 && bool(row >> b.device >> b.button) && b.device >= 0 && b.device < 4 && b.button >= -1 && b.button < 128;
                if (ok) candidate.wheelButtons[i] = b;
            }
            else if ((kind == "stick" || kind == "pressure") && (row >> name))
            {
                AxisBinding b;
                const bool stick = kind == "stick";
                int i = stick ? index(stickNames, name) : index(pressureNames, name);
                ok = i >= 0 && bool(row >> b.device >> b.axis >> b.minimum);
                if (stick) ok = ok && bool(row >> b.center);
                ok = ok && bool(row >> b.maximum >> b.deadzone);
                ok = ok && b.device >= 0 && b.device < 4 && b.axis >= -1 && b.axis < 8 &&
                    std::isfinite(b.minimum) && std::isfinite(b.maximum) && std::isfinite(b.center) &&
                    std::isfinite(b.deadzone) && b.deadzone >= 0 && b.deadzone < 1 &&
                    std::abs(b.maximum - b.minimum) > 0.000001f;
                if (stick) ok = ok && b.center > std::min(b.minimum, b.maximum) && b.center < std::max(b.minimum, b.maximum);
                if (ok) { if (stick) candidate.sticks[i] = b; else candidate.pressures[i] = b; }
            }
            if (!ok || (row >> extra))
            {
                error = "Invalid input profile line " + std::to_string(number);
                return false;
            }
        }
        if (stream.bad()) { error = "Input profile read failed"; return false; }
        profile = candidate;
        error.clear();
        return true;
    }

    bool hasBoundDevice(const InputProfile &p, const Devices &devices)
    {
        for (auto b : p.buttons) if (b.button >= 0 && devices[b.device].connected) return true;
        for (auto b : p.sticks) if (b.axis >= 0 && devices[b.device].connected) return true;
        for (auto b : p.pressures) if (b.axis >= 0 && devices[b.device].connected) return true;
        for (const auto &halves : p.stickHalves)
            for (auto b : halves) if (b.axis >= 0 && devices[b.device].connected) return true;
        if (p.wheelSteer.axis >= 0 && devices[p.wheelSteer.device].connected) return true;
        for (const auto &pedal : p.pedals) if (pedal.axis.axis >= 0 && devices[pedal.axis.device].connected) return true;
        for (auto b : p.wheelButtons) if (b.button >= 0 && devices[b.device].connected) return true;
        return false;
    }

    std::array<uint8_t, 32> mapDevices(const InputProfile &p, const Devices &devices)
    {
        static MapState state; // player 1's, as the function-level hysteresis was
        return mapDevices(p, devices, state);
    }

    std::array<uint8_t, 32> mapDevices(const InputProfile &p, const Devices &devices, MapState &state)
    {
        std::array<uint8_t, 32> data{};
        data[0] = 1; data[1] = 0x79;
        uint16_t buttons = 0xffff;
        const bool pedals = pedalsOnTriggers();
        for (int i = 0; i < 16; ++i)
            for (const ButtonBinding b : {p.buttons[i], p.wheelButtons[i]})
                if (b.button >= 0 && b.button < 128 && devices[b.device].connected && devices[b.device].buttons[b.button])
                    buttons &= uint16_t(~(1u << i));
        for (int i = 0; i < 4; ++i)
        {
            float v = 0, normalized = 0;
            const auto &b = p.sticks[i];
            if (axisValue(b, devices, v))
            {
                const float direction = b.maximum > b.minimum ? 1.f : -1.f;
                const float delta = (v - b.center) * direction;
                normalized = delta >= 0 ? delta / std::abs(b.maximum - b.center) : delta / std::abs(b.minimum - b.center);
                normalized = std::clamp(normalized, -1.f, 1.f);
                normalized = std::copysign(std::max(0.f, std::abs(normalized) - b.deadzone) / (1.f - b.deadzone), normalized);
            }
            if (i < 2 && state.publishRightStick) // rx, ry: the physical right stick (the camera's look-around)
            {
                (i == 0 ? g_rightStickX : g_rightStickY).store(normalized, std::memory_order_relaxed);
                if (g_rightStickClaimed.load(std::memory_order_relaxed))
                    normalized = 0.f;
            }
            for (int half = 0; half < 2; ++half)
            {
                AxisBinding hb = p.stickHalves[i][half];
                // Pedals on the triggers: ry up = right trigger, down = left
                // trigger unless the profile gives the halves itself.
                if (i == 1 && pedals && hb.axis < 0 && p.stickHalves[1][0].axis < 0 && p.stickHalves[1][1].axis < 0)
                {
                    hb.device = 0;
                    hb.axis = half ? 4 : 5;
                    hb.minimum = -1.f;
                    hb.maximum = 1.f;
                    hb.deadzone = 0.02f;
                }
                if (axisValue(hb, devices, v))
                {
                    float f = pressure(v, hb) / 255.f;
                    // Pedals on the triggers: GT3's analog pedals give nothing
                    // for the first 28 / 128 of right-stick travel and are full
                    // from ~113 (throttle car + 0x13ac against held stick levels,
                    // RAM dumps `ped-*` 2026-09-30: travel 32 -> 0, 40 -> 0.14,
                    // 64 -> 0.42, 96 -> 0.80, 128 -> 1.0; the brake half, read
                    // through the same stick axis, slows the car progressively
                    // too, `brk4`). The trigger's whole travel is spread over
                    // that live range.
                    // (Also without GT3_PEDALS: whenever a trigger - axis 4 / 5 -
                    // feeds the right stick's up / down, as the launcher's
                    // Controls page "Pedals on the triggers" maps it.)
#if !defined(PS2X_GAME_GT4)
                    if (i == 1 && (pedals || hb.axis == 4 || hb.axis == 5) && f > 0.f)
                        f = (28.f + f * (113.f - 28.f)) / 128.f;
#endif
                    normalized += (half ? 1.f : -1.f) * f;
                }
            }
            normalized = std::clamp(normalized, -1.f, 1.f);
            // lx: a steering wheel (wheel-steer), its lock and GT3's dead
            // zone applied, when turned further than the stick is pushed.
            if (i == 2 && axisValue(p.wheelSteer, devices, v))
            {
                const auto &w = p.wheelSteer;
                const float direction = w.maximum > w.minimum ? 1.f : -1.f;
                const float delta = (v - w.center) * direction;
                float s = delta >= 0 ? delta / std::abs(w.maximum - w.center) : delta / std::abs(w.minimum - w.center);
                s = std::clamp(s / p.wheelLock, -1.f, 1.f);
                const float magnitude = std::abs(s) < 0.002f ? 0.f : p.wheelGt3Deadzone + (1.f - p.wheelGt3Deadzone) * std::abs(s);
                const float steer = std::copysign(magnitude, s);
                if (std::abs(steer) >= std::abs(normalized))
                    normalized = steer;
            }
            data[4 + i] = static_cast<uint8_t>(std::lround(128.f + normalized * (normalized < 0 ? 128.f : 127.f)));
        }
        {
            // Optional (profile stick-dpad, or GT3_STICK_DPAD): past this
            // fraction of full deflection the left stick also presses the
            // D-pad. Off by default: the native DualShock 2 reports the sticks
            // themselves. Before the pressure bytes, which follow the digital bits.
            static const char *override = std::getenv("GT3_STICK_DPAD");
            const float threshold = override ? std::clamp(std::strtof(override, nullptr), 0.f, 1.f) : p.stickDpad;
            if (threshold > 0.f)
            {
                const int limit = static_cast<int>(std::lround(threshold * 127.f));
                const int lx = data[6] - 128, ly = data[7] - 128;
                if (lx <= -limit) buttons &= uint16_t(~0x0080u); // left
                if (lx >= limit) buttons &= uint16_t(~0x0020u);  // right
                if (ly <= -limit) buttons &= uint16_t(~0x0010u); // up
                if (ly >= limit) buttons &= uint16_t(~0x0040u);  // down
            }
        }
        // An analog source (a trigger) also presses its digital button, but
        // only past a clear point and with hysteresis: pressed from 30% of its
        // travel, released below 20%. Any pressure used to press it, so a
        // finger resting on a trigger flickered the button on and off - each
        // flicker a new press (2026-09-30: after AUTOMATIC / MANUAL, GT3's
        // RACING / DRIFT got chosen by itself with Cross on the right
        // trigger). The pressure byte itself stays fully analog.
        constexpr uint8_t kAnalogPress = 77, kAnalogRelease = 51;
        auto &analogHeld = state.analogHeld;
        for (int i = 0; i < 12; ++i)
        {
            const auto &b = p.pressures[i]; float v = 0;
            data[8 + i] = (buttons & pressureMasks[i]) ? 0 : 255;
            // Pedals on the triggers: a trigger presses no button (X stays X).
            const bool triggerSource = pedals && (b.axis == 4 || b.axis == 5);
            // A pedal (wheel-pedal) on this button: its travel spread over the
            // pressures GT3 uses for its job (throttle 1..45, brake 1..83, as
            // L2 / R2 below; a clutch plain), whatever button it is on.
            int pedalAnalog = -1, pedalSent = -1;
            for (int k = 0; k < 3; ++k)
                if (p.pedals[k].target == i && axisValue(p.pedals[k].axis, devices, v))
                {
                    const int a = pressure(v, p.pedals[k].axis);
                    pedalAnalog = std::max(pedalAnalog, a);
#if defined(PS2X_GAME_GT4)
                    pedalSent = std::max(pedalSent, a); // GT4: the full 0..255 (see below)
#else
                    const int top = k == 0 ? 45 : 83;
                    pedalSent = std::max(pedalSent, k == 2 ? a : (a ? 1 + (a * (top - 1) + 127) / 255 : 0));
#endif
                }
            // A digital binding for the same button still works (full pressure).
            const bool fromAxis = b.axis >= 0 && !triggerSource && axisValue(b, devices, v);
            if (fromAxis || pedalAnalog >= 0)
            {
                const uint8_t analog = static_cast<uint8_t>(std::max<int>(fromAxis ? pressure(v, b) : 0, pedalAnalog));
                // L2 / R2 from an analog source (a controller's triggers) are
                // GT3's analog pedals once its Key Configuration puts
                // Acceleration on R2 and Brakes on L2. GT3's throttle reads a
                // pressure of 1..~45 as 10..100% (measured 2026-09-30 on X and on
                // R2: 8 -> 0.26, 16 -> 0.42, 20 -> 0.50, 24 -> 0.58, 32 -> 0.74,
                // 40 -> 0.90, 100 -> 1.0), its brake 1..~83 (L2: 20 -> 0.31, 40 ->
                // 0.53, 60 -> 0.75), and a pressure counts only while its button
                // is down (PADMAN 390c): R2's travel is spread over 1..45, L2's
                // over 1..83, and the button is down from 4% of the travel.
#if defined(PS2X_GAME_GT4)
                // GT4 is not GT3: its DS2U_D passes a pressure only while the
                // button's bit is down (analysis/phase4/specs/pads.md 3.2) and the
                // game reads the whole 0..255. So no throttle / brake squeeze on
                // L2 / R2, and an analog source (a trigger on any button) presses
                // its button from 4% of the travel, or GT4 would see nothing for
                // the first 30% (analysis/gt4-stock-launcher/README.md).
                const bool pedal = false;
#else
                const bool pedal = i == 10 || i == 11;
#endif
                const int top = i == 10 ? 83 : 45;
#if defined(PS2X_GAME_GT4)
                // GT4_TRIGGER_RANGE=<30..100> (default 100): the pressure L2 / R2 reach at a trigger's full travel, in percent
                // of 255 (developer, 2026-10-09: GT4's throttle and brake felt full at under half the trigger's travel; a lower
                // range spreads the part GT4 uses over the whole travel). Not measured in GT4's own throttle yet.
                static const float triggerRange = [] {
                    const char *e = std::getenv("GT4_TRIGGER_RANGE");
                    const long n = e && *e ? std::strtol(e, nullptr, 10) : 100;
                    return static_cast<float>(std::clamp(n, 30L, 100L)) / 100.f;
                }();
                const uint8_t fromPad = fromAxis ? static_cast<uint8_t>(std::lround(pressure(v, b) * ((i == 10 || i == 11) ? triggerRange : 1.f))) : 0;
#else
                const uint8_t fromPad = fromAxis ? pressure(v, b) : 0;
#endif
                const uint8_t sent = static_cast<uint8_t>(std::max<int>(
                    pedal ? (fromPad ? 1 + (fromPad * (top - 1) + 127) / 255 : 0) : fromPad, pedalSent));
                data[8 + i] = std::max(data[8 + i], sent);
#if defined(PS2X_GAME_GT4)
                const bool light = true; // every analog source, as a DualShock 2's pressure button
#else
                const bool light = pedal || pedalAnalog >= 0; // pedals press their button from 4% of travel
#endif
                const uint8_t pressAt = light ? 10 : kAnalogPress, releaseAt = light ? 5 : kAnalogRelease;
                analogHeld[i] = analogHeld[i] ? analog >= releaseAt : analog >= pressAt;
                if (analogHeld[i]) buttons &= uint16_t(~pressureMasks[i]);
            }
            else
                analogHeld[i] = false;
        }
        data[2] = uint8_t(buttons); data[3] = uint8_t(buttons >> 8);
        return data;
    }
}
