#include "gt4stock_settings.h"

#include "gt4stock_util.h"
#include "shared/gt4_cheat_codes.h" // GT4_CHEAT_CARD_LIST ids (the cheats runtime's table)
#include "shared/gt4_pad2_rules.h"  // GT4_PAD2 as the player-2 runtime reads it
#include "shared/gt4_start_keys.h"  // GT4.exe's test and diagnostic switches (and the Classic allowlist)

#include <algorithm>
#include <cmath>
#include <map>
#include <set>
#include <sstream>

namespace gt4stock
{
    const char *fitKey(Fit f) { return f == Fit::Fill ? "fill" : f == Fit::Integer ? "integer" : "auto"; }
    const char *fitLabel(Fit f) { return f == Fit::Fill ? "Fill" : f == Fit::Integer ? "Whole rows" : "Auto"; }
    const char *pictureFilterKey(PictureFilter f) { return f == PictureFilter::Nearest ? "nearest" : f == PictureFilter::Sharp ? "sharp" : "smooth"; }
    const char *pictureFilterLabel(PictureFilter f) { return f == PictureFilter::Nearest ? "Nearest" : f == PictureFilter::Sharp ? "Sharp" : "Smooth"; }
    const char *deinterlaceKey(Deinterlace d)
    {
        return d == Deinterlace::Weave ? "weave" : d == Deinterlace::Bob ? "bob" : d == Deinterlace::Adaptive ? "adaptive" : "blend";
    }
    const char *deinterlaceLabel(Deinterlace d)
    {
        return d == Deinterlace::Weave ? "Weave" : d == Deinterlace::Bob ? "Bob" : d == Deinterlace::Adaptive ? "Adaptive" : "Blend";
    }

    const char *eyeToyLabel(EyeToy e)
    {
        switch (e)
        {
        case EyeToy::Shirt: return "Built-in GT shirt picture";
        case EyeToy::Webcam: return "Webcam";
        case EyeToy::Image: return "Picture file";
        default: return "Off";
        }
    }

    std::string eyeToyValue(const Settings &s)
    {
        switch (s.eyetoy)
        {
        case EyeToy::Shirt: return "shirt";
        case EyeToy::Image: return s.eyetoyImage.empty() ? std::string("absent") : "image:" + s.eyetoyImage;
        case EyeToy::Webcam: return s.eyetoyIndex >= 0 ? strf("webcam:%d", s.eyetoyIndex) : std::string("absent");
        default: return "absent";
        }
    }

    const char *const kPadButtonKeys[kPadButtons] = {"select", "l3", "r3", "start", "up", "right", "down", "left",
                                                     "l2", "r2", "l1", "r1", "triangle", "circle", "cross", "square"};
    const char *const kPadButtonLabels[kPadButtons] = {"Select", "L3 (left stick)", "R3 (right stick)", "Start", "Up", "Right", "Down", "Left",
                                                       "L2", "R2", "L1", "R1", "Triangle", "Circle", "Cross", "Square"};
    const char *const kPadPressureKeys[kPadPressures] = {"right", "left", "up", "down", "triangle", "circle",
                                                         "cross", "square", "l1", "r1", "l2", "r2"};
    const char *const kPadStickKeys[kPadSticks] = {"rx", "ry", "lx", "ly"};

    int pressureOfButton(int button)
    {
        if (button < 0 || button >= kPadButtons)
            return -1;
        for (int p = 0; p < kPadPressures; ++p)
            if (std::string_view(kPadPressureKeys[p]) == kPadButtonKeys[button])
                return p;
        return -1;
    }

    namespace
    {
        // raylib GamepadButton / GamepadAxis values (raylib.h 5.5).
        enum : int
        {
            GpUnknown = 0, GpLeftFaceUp, GpLeftFaceRight, GpLeftFaceDown, GpLeftFaceLeft, GpRightFaceUp, GpRightFaceRight,
            GpRightFaceDown, GpRightFaceLeft, GpLeftTrigger1, GpLeftTrigger2, GpRightTrigger1, GpRightTrigger2, GpMiddleLeft,
            GpMiddle, GpMiddleRight, GpLeftThumb, GpRightThumb
        };
        enum : int
        {
            AxLeftX = 0, AxLeftY, AxRightX, AxRightY, AxLeftTrigger, AxRightTrigger
        };
        constexpr int kDefaultButtons[kPadButtons] = {GpMiddleLeft, GpLeftThumb, GpRightThumb, GpMiddleRight, GpLeftFaceUp,
                                                      GpLeftFaceRight, GpLeftFaceDown, GpLeftFaceLeft, GpLeftTrigger2, GpRightTrigger2,
                                                      GpLeftTrigger1, GpRightTrigger1, GpRightFaceUp, GpRightFaceRight, GpRightFaceDown,
                                                      GpRightFaceLeft};
        constexpr int kPressureL2 = 10, kPressureR2 = 11, kPressureCross = 6, kPressureSquare = 7;
        constexpr int kButtonL2 = 8, kButtonR2 = 9;

        template <size_t N> int indexOf(const char *const (&names)[N], const std::string &name)
        {
            for (size_t i = 0; i < N; ++i)
                if (name == names[i])
                    return static_cast<int>(i);
            return -1;
        }

        bool parseBool(const std::string &text, bool &value)
        {
            const std::string t = lower(trim(text));
            if (t == "1" || t == "on" || t == "yes" || t == "true")
                value = true;
            else if (t == "0" || t == "off" || t == "no" || t == "false")
                value = false;
            else
                return false;
            return true;
        }

        std::vector<std::string> words(const std::string &line)
        {
            std::istringstream in(line);
            std::vector<std::string> w;
            for (std::string t; in >> t;)
                w.push_back(t);
            return w;
        }

        bool isGuid32(std::string &g)
        {
            if (g.size() != 32)
                return false;
            for (char &c : g)
            {
                c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f')))
                    return false;
            }
            return true;
        }
    }

    const char *gamepadButtonName(int button, bool ps)
    {
        static const char *const xbox[kGamepadButtonCount] = {"-", "D-pad Up", "D-pad Right", "D-pad Down", "D-pad Left", "Y", "B", "A", "X",
                                                              "LB", "LT", "RB", "RT", "View", "Guide", "Menu", "Left stick press",
                                                              "Right stick press"};
        static const char *const play[kGamepadButtonCount] = {"-", "D-pad Up", "D-pad Right", "D-pad Down", "D-pad Left", "Triangle", "Circle",
                                                              "Cross", "Square", "L1", "L2", "R1", "R2", "Create", "PS", "Options", "L3",
                                                              "R3"};
        if (button < 0 || button >= kGamepadButtonCount)
            return "-";
        return ps ? play[button] : xbox[button];
    }

    const char *gamepadAxisName(int axis, bool ps)
    {
        static const char *const xbox[kGamepadAxisCount] = {"Left stick X", "Left stick Y", "Right stick X", "Right stick Y", "Left trigger (LT)",
                                                            "Right trigger (RT)"};
        static const char *const play[kGamepadAxisCount] = {"Left stick X", "Left stick Y", "Right stick X", "Right stick Y", "L2 trigger",
                                                            "R2 trigger"};
        if (axis < 0 || axis >= kGamepadAxisCount)
            return "-";
        return ps ? play[axis] : xbox[axis];
    }

    // ------------------------------------------------------------------ pad
    PadSettings::PadSettings() { applyStandard(); }

    void PadSettings::applyStandard()
    {
        for (int i = 0; i < kPadButtons; ++i)
            buttons[static_cast<size_t>(i)] = kDefaultButtons[i];
        pressure.fill(-1);
        pressure[kPressureL2] = AxLeftTrigger;
        pressure[kPressureR2] = AxRightTrigger;
        stickAxis = {AxRightX, AxRightY, AxLeftX, AxLeftY};
        stickInvert.fill(false);
    }

    void PadSettings::applyTriggersOnFaceButtons()
    {
        applyStandard();
        pressure[kPressureL2] = -1;
        pressure[kPressureR2] = -1;
        pressure[kPressureCross] = AxRightTrigger;
        pressure[kPressureSquare] = AxLeftTrigger;
        // The triggers' digital clicks would also press L2 / R2.
        buttons[kButtonL2] = -1;
        buttons[kButtonR2] = -1;
    }

    bool PadSettings::usesTriggersOnFaceButtons() const
    {
        return pressure[kPressureCross] == AxRightTrigger && pressure[kPressureSquare] == AxLeftTrigger;
    }

    std::string padProfileRows(const PadSettings &pad)
    {
        std::vector<std::string> rows;
        rows.push_back(pad.sourceGuid.empty() ? "source 0 mapped 0" : "source-guid 0 mapped " + pad.sourceGuid);
        const PadSettings standard;
        for (int i = 0; i < kPadButtons; ++i)
            if (pad.buttons[static_cast<size_t>(i)] != standard.buttons[static_cast<size_t>(i)])
                rows.push_back(strf("button %s 0 %d", kPadButtonKeys[i], pad.buttons[static_cast<size_t>(i)]));
        const std::string dz = numberText(std::clamp(pad.triggerDeadzone, 0.0f, 0.9f));
        // The end point (developer, 2026-10-09: "start and end points"): the axis value where the pressure is full, -1 + 2 x the
        // share of the travel (1 = the trigger's whole travel).
        const float full = std::clamp(pad.triggerFull, 0.5f, 1.0f);
        const std::string fullText = numberText(-1.0f + 2.0f * full);
        for (int p = 0; p < kPadPressures; ++p)
        {
            const int axis = pad.pressure[static_cast<size_t>(p)];
            const bool changed = axis != standard.pressure[static_cast<size_t>(p)];
            if (axis >= 0 && (changed || pad.triggerDeadzone != 0.0f || full < 1.0f))
                rows.push_back(strf("pressure %s 0 %d -1 %s %s", kPadPressureKeys[p], axis, fullText.c_str(), dz.c_str()));
            else if (axis < 0 && changed)
                rows.push_back(strf("pressure %s 0 -1 -1 1 0", kPadPressureKeys[p]));
        }
        for (int s = 0; s < kPadSticks; ++s)
        {
            const float dzs = s < 2 ? pad.rightDeadzone : pad.leftDeadzone;
            const int axis = pad.stickAxis[static_cast<size_t>(s)];
            const bool inv = pad.stickInvert[static_cast<size_t>(s)];
            if (axis != standard.stickAxis[static_cast<size_t>(s)] || inv || dzs != 0.0f)
                rows.push_back(strf("stick %s 0 %d %s 0 %s %s", kPadStickKeys[s], axis, inv ? "1" : "-1", inv ? "-1" : "1",
                                    numberText(std::clamp(dzs, 0.0f, 0.9f)).c_str()));
        }
        if (!pad.keyboard)
            rows.push_back("keyboard 0");
        if (pad.rumble <= 0.0f)
            rows.push_back("rumble off");
        else if (pad.rumble < 1.0f)
        {
            const std::string r = numberText(std::clamp(pad.rumble, 0.0f, 1.0f), 2);
            rows.push_back("rumble xinput auto " + r + " " + r);
        }
        for (const std::string &extra : pad.extraRows)
            rows.push_back(extra);
        return join(rows, "; ");
    }

    bool parsePadProfileRows(const std::string &text, PadSettings &pad, std::string &error)
    {
        PadSettings next; // rows apply over the built-in mapping, as the runtime does
        next.triggerDeadzone = 0.0f;
        bool sawTriggerDeadzone = false;
        for (const std::string &raw : split(text, ';'))
        {
            const std::string row = trim(raw.substr(0, raw.find('#')));
            if (row.empty())
                continue;
            const std::vector<std::string> w = words(row);
            const std::string kind = lower(w[0]);
            bool ok = false;
            long long a = 0, b = 0;
            double f = 0;
            if (kind == "source" && w.size() == 4 && w[1] == "0" && w[2] == "mapped" && w[3] == "0")
            {
                next.sourceGuid.clear();
                ok = true;
            }
            else if (kind == "source-guid" && w.size() == 4 && w[1] == "0" && w[2] == "mapped")
            {
                std::string g = w[3];
                ok = isGuid32(g);
                if (ok)
                    next.sourceGuid = g;
            }
            else if (kind == "button" && w.size() == 4 && w[2] == "0" && parseInt(w[3], b))
            {
                const int i = indexOf(kPadButtonKeys, lower(w[1]));
                ok = i >= 0 && b >= -1 && b < kGamepadButtonCount;
                if (ok)
                    next.buttons[static_cast<size_t>(i)] = static_cast<int>(b);
            }
            else if (double fullAxis = 1.0; kind == "pressure" && w.size() == 7 && w[2] == "0" && parseInt(w[3], a) && w[4] == "-1" &&
                                             parseFloat(w[5], fullAxis) && fullAxis >= -0.001 && fullAxis <= 1.0 && parseFloat(w[6], f))
            {
                const int p = indexOf(kPadPressureKeys, lower(w[1]));
                ok = p >= 0 && a >= -1 && a < kGamepadAxisCount && f >= 0.0 && f < 1.0;
                if (ok)
                {
                    next.pressure[static_cast<size_t>(p)] = static_cast<int>(a);
                    if (a >= 0)
                    {
                        next.triggerDeadzone = static_cast<float>(f);
                        next.triggerFull = std::clamp(static_cast<float>((fullAxis + 1.0) * 0.5), 0.5f, 1.0f);
                        sawTriggerDeadzone = true;
                    }
                }
            }
            else if (kind == "stick" && w.size() == 8 && w[2] == "0" && parseInt(w[3], a) && w[5] == "0" && parseFloat(w[7], f))
            {
                const int s = indexOf(kPadStickKeys, lower(w[1]));
                const bool inv = w[4] == "1" && w[6] == "-1";
                const bool norm = w[4] == "-1" && w[6] == "1";
                ok = s >= 0 && a >= -1 && a < kGamepadAxisCount && (inv || norm) && f >= 0.0 && f < 1.0;
                if (ok)
                {
                    next.stickAxis[static_cast<size_t>(s)] = static_cast<int>(a);
                    next.stickInvert[static_cast<size_t>(s)] = inv;
                    (s < 2 ? next.rightDeadzone : next.leftDeadzone) = static_cast<float>(f);
                }
            }
            else if (kind == "keyboard" && w.size() == 2 && (w[1] == "0" || w[1] == "1"))
            {
                next.keyboard = w[1] == "1";
                ok = true;
            }
            else if (kind == "rumble" && w.size() == 2 && lower(w[1]) == "off")
            {
                next.rumble = 0.0f;
                ok = true;
            }
            else if (kind == "rumble" && w.size() == 5 && lower(w[1]) == "xinput" && lower(w[2]) == "auto" && parseFloat(w[3], f))
            {
                ok = f >= 0.0 && f <= 1.0;
                if (ok)
                    next.rumble = static_cast<float>(f);
            }
            if (!ok)
            {
                // Not a row this launcher writes (hand-written): kept as it is.
                next.extraRows.push_back(row);
            }
        }
        if (!sawTriggerDeadzone)
            next.triggerDeadzone = 0.0f;
        pad = next;
        error.clear();
        return true;
    }

    // ------------------------------------------------------------------ wheel
    const char *const kWheelAxisKeys[kWheelAxes] = {"x", "y", "z", "rx", "ry", "rz", "slider0", "slider1"};
    // The names Windows' "Set up USB game controllers" panel uses for the same axes.
    const char *const kWheelAxisLabels[kWheelAxes] = {"X axis", "Y axis", "Z axis", "X rotation", "Y rotation", "Z rotation", "Slider 1", "Slider 2"};
    const char *const kWheelButtonKeys[kWheelButtonNames] = {"none", "cross", "square", "circle", "triangle", "r1", "l1", "r2",
                                                             "l2", "select", "start", "b10", "b11", "b12", "b13"};
    // DFP buttons 12 and 13 reach LGDEV but GT4 drops them (gt4_lgdev.h WheelButton).
    const char *const kWheelButtonLabels[kWheelButtonNames] = {"Not used", "Cross", "Square", "Circle", "Triangle", "R1", "L1", "R2",
                                                               "L2", "Select", "Start", "Button 10", "Button 11", "Button 12 (unused)", "Button 13 (unused)"};

    const char *wheelAxisLabel(const std::string &key)
    {
        if (key == "none" || key.empty())
            return "None";
        for (int a = 0; a < kWheelAxes; ++a)
            if (key == kWheelAxisKeys[a])
                return kWheelAxisLabels[a];
        return "?";
    }

    int wheelButtonIndex(const std::string &key)
    {
        const std::string k = lower(key);
        for (int i = 0; i < kWheelButtonNames; ++i)
            if (k == kWheelButtonKeys[i])
                return i;
        return -1;
    }

    WheelSettings::WheelSettings()
    {
        gas.axis = "y";
        brake.axis = "rz";
        buttons.fill(0);
        for (int i = 0; i < 14; ++i)
            buttons[static_cast<size_t>(i)] = i + 1; // host 0..13 -> cross .. b13 (the adapter's DFP layout)
    }

    namespace
    {
        bool knownAxis(const std::string &a)
        {
            for (const char *k : kWheelAxisKeys)
                if (a == k)
                    return true;
            return false;
        }

        std::string pedalLine(const char *name, const PedalSettings &p)
        {
            if (p.axis == "none" || p.axis.empty())
                return std::string(name) + " none";
            std::string l = std::string(name) + " " + p.axis;
            if (p.calibrated && p.released != p.full)
                l += strf(" released=%d full=%d", p.released, p.full);
            return l;
        }
    }

    std::string wheelConfigLines(const WheelSettings &w)
    {
        std::vector<std::string> lines;
        lines.push_back("steer " + w.steerAxis);
        lines.push_back(pedalLine("gas", w.gas));
        lines.push_back(pedalLine("brake", w.brake));
        lines.push_back("pedal-deadzone " + numberText(std::clamp(w.pedalDeadzone, 0.0f, 0.5f)));
        lines.push_back(w.hat >= 0 ? strf("hat %d", w.hat) : std::string("hat none"));
        const WheelSettings standard;
        for (size_t i = 0; i < w.buttons.size(); ++i)
            if (w.buttons[i] != standard.buttons[i])
            {
                const int b = std::clamp(w.buttons[i], 0, kWheelButtonNames - 1);
                lines.push_back(strf("button %zu %s", i, kWheelButtonKeys[b]));
            }
        static const char *const effectNames[4] = {"constant", "spring", "damper", "friction"};
        std::vector<std::string> effects;
        for (int e = 0; e < 4; ++e)
            if (w.effects[static_cast<size_t>(e)])
                effects.push_back(effectNames[e]);
        lines.push_back("ffb-effects " + (effects.empty() ? std::string("none") : join(effects, ",")));
        lines.push_back(std::string("ffb-pause-unfocused ") + (w.ffbPauseUnfocused ? "1" : "0"));
        lines.push_back("ffb-slew " + numberText(std::clamp(w.ffbSlew, 0.5f, 100.0f), 2));
        for (const std::string &extra : w.extraLines)
            lines.push_back(extra);
        return join(lines, "; ");
    }

    bool parseWheelConfigLines(const std::string &text, WheelSettings &wheel, std::string &error)
    {
        WheelSettings next = wheel;
        next.extraLines.clear();
        for (const std::string &raw : split(text, ';'))
        {
            const std::string line = trim(raw.substr(0, raw.find('#')));
            if (line.empty())
                continue;
            const std::vector<std::string> t = words(line);
            const std::string key = lower(t[0]);
            bool ok = false;
            long long n = 0;
            double f = 0;
            if (key == "steer" && t.size() >= 2 && knownAxis(lower(t[1])))
            {
                next.steerAxis = lower(t[1]);
                next.steerInvert = t.size() > 2 && lower(t[2]) == "invert";
                ok = t.size() <= 3;
            }
            else if ((key == "gas" || key == "brake") && t.size() >= 2)
            {
                PedalSettings p = key == "gas" ? next.gas : next.brake;
                p.calibrated = false;
                const std::string axis = lower(t[1]);
                ok = axis == "none" || knownAxis(axis);
                p.axis = axis;
                bool haveReleased = false, haveFull = false;
                for (size_t i = 2; ok && i < t.size(); ++i)
                {
                    const size_t eq = t[i].find('=');
                    long long v = 0;
                    if (eq == std::string::npos || !parseInt(t[i].substr(eq + 1), v) || v < -32768 || v > 32767)
                    {
                        ok = false;
                        break;
                    }
                    const std::string name = lower(t[i].substr(0, eq));
                    if (name == "released")
                    {
                        p.released = static_cast<int>(v);
                        haveReleased = true;
                    }
                    else if (name == "full")
                    {
                        p.full = static_cast<int>(v);
                        haveFull = true;
                    }
                    else
                        ok = false;
                }
                ok = ok && haveReleased == haveFull && (!haveReleased || p.released != p.full);
                p.calibrated = ok && haveReleased;
                if (ok)
                    (key == "gas" ? next.gas : next.brake) = p;
            }
            else if (key == "pedal-deadzone" && t.size() == 2 && parseFloat(t[1], f) && f >= 0.0 && f <= 0.5)
            {
                next.pedalDeadzone = static_cast<float>(f);
                ok = true;
            }
            else if (key == "hat" && t.size() == 2)
            {
                if (lower(t[1]) == "none")
                {
                    next.hat = -1;
                    ok = true;
                }
                else if (parseInt(t[1], n) && n >= 0 && n <= 3)
                {
                    next.hat = static_cast<int>(n);
                    ok = true;
                }
            }
            else if (key == "button" && t.size() == 3 && parseInt(t[1], n) && n >= 0 && n <= 127)
            {
                const int b = wheelButtonIndex(t[2]);
                ok = b >= 0;
                if (ok)
                    next.buttons[static_cast<size_t>(n)] = b;
            }
            else if (key == "ffb-effects" && t.size() == 2)
            {
                std::array<bool, 4> e{false, false, false, false};
                ok = true;
                for (const std::string &item : split(lower(t[1]), ','))
                {
                    if (item == "constant")
                        e[0] = true;
                    else if (item == "spring")
                        e[1] = true;
                    else if (item == "damper")
                        e[2] = true;
                    else if (item == "friction")
                        e[3] = true;
                    else if (item != "none")
                        ok = false;
                }
                if (ok)
                    next.effects = e;
            }
            else if (key == "ffb-pause-unfocused" && t.size() == 2)
            {
                bool v = true;
                ok = parseBool(t[1], v);
                if (ok)
                    next.ffbPauseUnfocused = v;
            }
            else if (key == "ffb-slew" && t.size() == 2 && parseFloat(t[1], f) && f >= 0.5 && f <= 100.0)
            {
                next.ffbSlew = static_cast<float>(f);
                ok = true;
            }
            if (!ok)
                next.extraLines.push_back(line); // kept for GT4.exe, which reports a bad one
        }
        wheel = next;
        error.clear();
        return true;
    }

    // ------------------------------------------------------------------ player 2, cheats
    std::string pad2Value(const Pad2Settings &p)
    {
        switch (p.mode)
        {
        case Pad2Settings::Mode::Off: return "off";
        case Pad2Settings::Mode::Chosen:
            if (!p.guid.empty())
                return "guid:" + p.guid;
            if (p.joystick >= 0 && p.joystick <= 3)
                return strf("joystick:%d", p.joystick);
            return "auto";
        default: return "auto";
        }
    }

    const std::vector<std::string> &launcherManagedKeys()
    {
        static const std::vector<std::string> keys = {
            "GT4_DISC_IMAGE", "GT3_DISPLAY_FILTER", "GT3_CRT_SCANLINES", "GT4_PICTURE_FIT", "GT4_SCANLINE_FIELD",
            "GT4_PICTURE_FILTER", "GT4_DEINTERLACE",
            "GT3_FULLSCREEN", "GT3_WINDOW_SIZE", "GT4_MONITOR", "GT3_VIBRATION", "PS2X_INPUT", "PS2X_WHEEL",
            "PS2X_WHEEL_DEVICE", "PS2X_WHEEL_PEDALS", "PS2X_WHEEL_MODEL", "PS2X_WHEEL_ROTATION", "PS2X_WHEEL_STEER_INVERT",
            "PS2X_WHEEL_FFB", "PS2X_WHEEL_FFB_GAIN", "PS2X_WHEEL_FFB_INVERT", "PS2X_WHEEL_LINES", "GT4_MC0", "GT4_MC1", "GT4_USB", "GT4_USB_ROOT",
            "GT4_EYETOY", "GT4L_PREVIEW", "GT4L_EXE", "GT4L_LAST_PLAYED", "GT4L_LAST_EXIT", "GT4L_LAST_MINUTES", "GT4L_FFB_ABOVE_CAP",
            "GT4L_WHEEL_NAME", "GT4L_PEDALS_NAME", "GT4L_EYETOY", "GT4L_EYETOY_CAMERA",
            // 1.4: player 2 (the player-2 runtime) and GT4's button codes (the cheats runtime). The GT4.exe keys
            // are the ones those patches put on the stock build's allowlist (main.cpp kAllowed).
            "GT4_PAD2", "GT4L_PAD2_NAME", "GT4_CHEATS", "GT4_CHEAT_CARD", "GT4_CHEAT_CARD_LIST", "GT4_CHEAT_CARD_KEY",
            "GT4_CHEAT_CARD_PLACE", "GT4_CHEAT_CARD_FOLLOW", "GT4L_CHEATS_TICKED", "GT4L_CHEATS_PAPER"};
#if defined(GT4L_EDITION_FULL)
        // The Full edition also owns its picture, renderer, network and launcher-state keys (gt4full_settings.cpp).
        static const std::vector<std::string> full = [] {
            std::vector<std::string> k = keys;
            for (const std::string &f : fullManagedKeys())
                if (std::find(k.begin(), k.end(), f) == k.end())
                    k.push_back(f);
            return k;
        }();
        return full;
#else
        return keys;
#endif
    }

    bool isTestOnlyKey(const std::string &key)
    {
        // (1.4.2) GT4.exe's own list (shared/gt4_start_keys.h, the runtime's runtime/gt4_start_keys.h byte for byte): the
        // cheats and player-2 runtimes' test switches (GT4_CHEAT_CARD_TEST, GT4_CHEATS_TEST_NODAY, GT4_PAD2_REPLAY), the
        // race video-mode overrides that GT4 would save (GT4_RACE_VIDEO_MODE / _BLEND, GT4_SCREEN_ASPECT), the replays and
        // the test pilot (GT3_PAD_REPLAY, PS2X_WHEEL_REPLAY, GT4_AI_PILOT), shots, dumps, traces and the compare.
        // Any spelling: Windows' environment ignores case, so gt4_cheat_card_test would reach a development GT4.exe too.
        return gt4startkeys::testOnlyKey(key);
    }

    // ------------------------------------------------------------------ file
    namespace
    {
        // Every key the launcher writes (and therefore owns).
        const std::set<std::string> &managedKeys()
        {
            static const std::set<std::string> keys(launcherManagedKeys().begin(), launcherManagedKeys().end());
            return keys;
        }

        bool percentValue(const std::string &v, int &out)
        {
            long long n = 0;
            if (!parseInt(v, n))
                return false;
            out = static_cast<int>(std::clamp<long long>(n, 0, 100));
            return true;
        }
    }

    bool parseSettingsText(const std::string &text, Settings &s, std::vector<std::string> &warnings)
    {
        std::istringstream in(text);
        std::string line;
        unsigned number = 0;
        bool sawWheel = false;
        // PS2X_WHEEL_STEER_INVERT, wherever it is in the file, wins over a "steer <axis>
        // invert" line (the adapter applies the single keys after the lines).
        bool sawSteerInvert = false, steerInvertKey = false;
        // GT4L_EYETOY=webcam: a webcam is chosen but was not connected when the file was
        // written (GT4_EYETOY=absent then).
        bool webcamChosen = false;
        // 1.4: the ticked cards are GT4L_CHEATS_TICKED; without it (a file by hand), GT4_CHEAT_CARD_LIST.
        bool sawTicked = false, sawCardList = false;
        std::vector<std::string> cardList;
        while (std::getline(in, line))
        {
            ++number;
            if (!line.empty() && line.back() == '\r')
                line.pop_back();
            const size_t eq = line.find('=');
            // GT4.exe's reader: '#' first, or no '=', or '=' first -> skipped.
            if (line.empty() || line[0] == '#' || eq == std::string::npos || eq == 0)
                continue;
            const std::string key = line.substr(0, eq);
            const std::string value = line.substr(eq + 1);
            auto bad = [&](const char *what) { warnings.push_back(strf("line %u: %s=%s: %s", number, key.c_str(), value.c_str(), what)); };
            double f = 0;
            long long n = 0;
#if defined(GT4L_EDITION_FULL)
            // The Full edition's keys first: its own, and the ones it reads differently (GT4_PICTURE_FILTER's four
            // filters, GT3_FULLSCREEN=span, the network keys).
            if (parseFullSetting(key, value, s, bad))
            {
            }
            else
#endif
            if (key == "GT4_DISC_IMAGE")
                s.disc = value;
            else if (key == "GT3_DISPLAY_FILTER")
                s.linesOn = iequals(trim(value), "crt") || trim(value).empty();
            else if (key == "GT3_CRT_SCANLINES")
            {
                if (!percentValue(value, s.strength))
                    bad("a number 0-100");
            }
            else if (key == "GT4_SCANLINE_FIELD")
            {
                if (!percentValue(value, s.field))
                    bad("a number 0-100");
            }
            else if (key == "GT4_PICTURE_FIT")
            {
                const std::string v = lower(trim(value));
                if (v == "auto")
                    s.fit = Fit::Auto;
                else if (v == "fill")
                    s.fit = Fit::Fill;
                else if (v == "integer")
                    s.fit = Fit::Integer;
                else
                    bad("auto, fill or integer");
            }
            else if (key == "GT4_PICTURE_FILTER")
            {
                // GT4.exe also takes "bilinear" for smooth (present_scanline_math.h parseFilter).
                const std::string v = lower(trim(value));
                if (v == "nearest")
                    s.filter = PictureFilter::Nearest;
                else if (v == "sharp")
                    s.filter = PictureFilter::Sharp;
                else if (v == "smooth" || v == "bilinear")
                    s.filter = PictureFilter::Smooth;
                else
                    bad("nearest, sharp or smooth");
            }
            else if (key == "GT4_DEINTERLACE")
            {
                // GT4.exe also takes "off" for weave (present_scanline_math.h parseDeinterlace).
                const std::string v = lower(trim(value));
                if (v == "weave" || v == "off")
                    s.deinterlace = Deinterlace::Weave;
                else if (v == "bob")
                    s.deinterlace = Deinterlace::Bob;
                else if (v == "blend")
                    s.deinterlace = Deinterlace::Blend;
                else if (v == "adaptive")
                    s.deinterlace = Deinterlace::Adaptive;
                else
                    bad("weave, bob, blend or adaptive");
            }
            else if (key == "GT3_FULLSCREEN")
                s.fullscreen = trim(value) == "1";
            else if (key == "GT3_WINDOW_SIZE")
            {
                int w = 0, h = 0;
                const std::vector<std::string> wh = split(lower(trim(value)), 'x');
                long long a = 0, b = 0;
                if (wh.size() == 2 && parseInt(wh[0], a) && parseInt(wh[1], b) && a >= 320 && b >= 224 && a <= 16384 && b <= 16384)
                {
                    w = static_cast<int>(a);
                    h = static_cast<int>(b);
                    s.windowW = w;
                    s.windowH = h;
                }
                else
                    bad("<width>x<height>");
            }
            else if (key == "GT4_MONITOR")
            {
                const std::string v = trim(value);
                const size_t space = v.find(' ');
                const std::vector<std::string> xy = split(v.substr(0, space), ',');
                long long x = 0, y = 0;
                if (xy.size() == 2 && parseInt(xy[0], x) && parseInt(xy[1], y))
                {
                    s.monitorSet = true;
                    s.monitorX = static_cast<int>(x);
                    s.monitorY = static_cast<int>(y);
                    s.monitorName = space == std::string::npos ? std::string() : trim(v.substr(space + 1));
                }
                else if (!v.empty())
                    bad("<x>,<y> <name>");
            }
            else if (key == "GT3_VIBRATION")
                s.vibration = trim(value) != "0";
            else if (key == "PS2X_INPUT")
            {
                std::string why;
                parsePadProfileRows(value, s.pad, why);
            }
            else if (key == "PS2X_WHEEL")
            {
                const std::string v = lower(trim(value));
                s.wheel.enabled = v == "dinput" || v == "host";
                sawWheel = true;
                if (!s.wheel.enabled && !v.empty())
                    s.other.emplace_back(key, value); // virtual / virtual2: a test setting, kept
            }
            else if (key == "PS2X_WHEEL_DEVICE")
                s.wheel.device = trim(value).empty() ? "auto" : trim(value);
            else if (key == "PS2X_WHEEL_PEDALS")
                s.wheel.pedals = trim(value).empty() ? "wheel" : trim(value);
            else if (key == "PS2X_WHEEL_MODEL")
            {
                const std::string v = lower(trim(value));
                if (v == "dfp" || v == "df" || v == "gtforce")
                    s.wheel.model = v;
                else
                    bad("dfp, df or gtforce");
            }
            else if (key == "PS2X_WHEEL_ROTATION")
            {
                if (parseFloat(value, f) && f >= 90.0 && f <= 3600.0)
                    s.wheel.rotation = static_cast<int>(std::lround(f));
                else
                    bad("degrees 90-3600");
            }
            else if (key == "PS2X_WHEEL_STEER_INVERT")
            {
                if (parseBool(value, steerInvertKey))
                    sawSteerInvert = true;
                else
                    bad("0 or 1");
            }
            else if (key == "PS2X_WHEEL_FFB")
            {
                if (!parseBool(value, s.wheel.ffb))
                    bad("0 or 1");
            }
            else if (key == "PS2X_WHEEL_FFB_GAIN")
            {
                if (parseFloat(value, f) && f >= 0.0 && f <= 1.0)
                    s.wheel.ffbGain = static_cast<float>(f);
                else
                    bad("0..1");
            }
            else if (key == "PS2X_WHEEL_FFB_INVERT")
            {
                if (!parseBool(value, s.wheel.ffbInvert))
                    bad("0 or 1");
            }
            else if (key == "PS2X_WHEEL_LINES")
            {
                // A "steer <axis> [invert]" line sets the invert flag; the single
                // key, read before or after, wins (applied after the loop).
                std::string why;
                parseWheelConfigLines(value, s.wheel, why);
            }
            else if (key == "GT4_MC0" || key == "GT4_MC1")
            {
                Settings::CardSlot &slot = s.cards[key == "GT4_MC0" ? 0 : 1];
                const std::string v = trim(value);
                if (v.empty() || iequals(v, "folder") || iequals(v, "none"))
                {
                    slot.file = false;
                    if (key == "GT4_MC1" && iequals(v, "folder"))
                        bad("slot 2 has no folder card: none or a .ps2 file");
                }
                else
                {
                    slot.file = true;
                    slot.path = v;
                }
            }
            else if (key == "GT4_USB")
                s.usb = trim(value) != "0";
            else if (key == "GT4_USB_ROOT")
                s.usbRoot = trim(value);
            else if (key == "GT4_EYETOY")
            {
                // The EyeToy service's reading (gt4_eyetoy.cpp EyeToy::EyeToy, ensureSource).
                const std::string v = trim(value);
                const std::string lv = lower(v);
                if (lv.empty() || lv == "absent" || lv == "0" || lv == "off")
                    s.eyetoy = EyeToy::Off;
                else if (lv == "shirt" || lv == "1" || lv == "on")
                    s.eyetoy = EyeToy::Shirt;
                else if (startsWith(lv, "image:") && v.size() > 6)
                {
                    s.eyetoy = EyeToy::Image;
                    s.eyetoyImage = v.substr(6);
                }
                else if (lv == "webcam")
                {
                    s.eyetoy = EyeToy::Webcam;
                    s.eyetoyIndex = 0;
                }
                else if (startsWith(lv, "webcam:"))
                {
                    s.eyetoy = EyeToy::Webcam;
                    if (parseInt(v.substr(7), n) && n >= 0 && n < 64)
                        s.eyetoyIndex = static_cast<int>(n);
                    else
                    {
                        // GT4.exe takes a number here (atoi: a name would open camera 0). Taken
                        // as the camera's name; the launcher writes its number back.
                        s.eyetoyIndex = -1;
                        s.eyetoyCamera = trim(v.substr(7));
                        bad("webcam:<n>, n the camera's number (GT4.exe would open camera 0): taken as a camera name");
                    }
                }
                else
                {
                    s.eyetoy = EyeToy::Off;
                    bad("absent, shirt, image:<file> or webcam:<n>");
                }
            }
            else if (key == "GT4L_EYETOY")
                webcamChosen = iequals(trim(value), "webcam");
            else if (key == "GT4L_EYETOY_CAMERA")
            {
                if (!trim(value).empty())
                    s.eyetoyCamera = trim(value);
            }
            else if (key == "GT4L_PREVIEW")
                s.preview = lower(trim(value));
            else if (key == "GT4L_EXE")
                s.exe = trim(value);
            else if (key == "GT4L_LAST_PLAYED")
                s.lastPlayed = trim(value);
            else if (key == "GT4L_LAST_EXIT")
            {
                if (parseInt(value, n))
                    s.lastExit = static_cast<int>(n);
            }
            else if (key == "GT4L_LAST_MINUTES")
            {
                if (parseInt(value, n))
                    s.lastMinutes = static_cast<int>(n);
            }
            else if (key == "GT4L_FFB_ABOVE_CAP")
                parseBool(value, s.wheel.ffbAboveCap);
            else if (key == "GT4L_WHEEL_NAME")
                s.wheel.deviceName = trim(value);
            else if (key == "GT4L_PEDALS_NAME")
                s.wheel.pedalsName = trim(value);
            // ---- 1.4: player 2 (the player-2 runtime's GT4_PAD2, read with its own rules header)
            else if (key == "GT4_PAD2")
            {
                gt4pad2rules::Choice c;
                std::string why;
                if (gt4pad2rules::parseChoice(value, c, why))
                {
                    s.pad2.explicitChoice = true;
                    s.pad2.mode = c.mode == gt4pad2rules::Mode::Off         ? Pad2Settings::Mode::Off
                                  : c.mode == gt4pad2rules::Mode::Automatic ? Pad2Settings::Mode::Automatic
                                                                            : Pad2Settings::Mode::Chosen;
                    s.pad2.guid = c.mode == gt4pad2rules::Mode::Guid ? c.guid : std::string();
                    s.pad2.joystick = c.mode == gt4pad2rules::Mode::Joystick ? c.joystick : -1;
                }
                else
                {
                    s.pad2 = Pad2Settings{};
                    s.pad2.explicitChoice = true; // written back as off (GT4.exe also takes a bad value as off)
                    bad("auto, off, joystick:<0..3> or guid:<32 hex>: taken as off");
                }
            }
            else if (key == "GT4L_PAD2_NAME")
                s.pad2.name = trim(value);
            // ---- 1.4: GT4's button codes (the cheats runtime's keys, read as it reads them)
            else if (key == "GT4_CHEATS")
            {
                CheatSettings &c = s.cheats;
                c.noday = c.secret = c.missionfix = false;
                c.otherWords.clear();
                std::string w;
                auto take = [&] {
                    w = lower(trim(w));
                    if (w == "noday")
                        c.noday = true;
                    else if (w == "secret")
                        c.secret = true;
                    else if (w == "missionfix")
                        c.missionfix = true;
                    else if (w == "off" || w == "none")
                    {
                        // the runtime's words for nothing on (gt4_cheat_overlay.cpp parseSwitches)
                    }
                    else if (!w.empty() && std::find(c.otherWords.begin(), c.otherWords.end(), w) == c.otherWords.end())
                    {
                        c.otherWords.push_back(w);
                        bad(("'" + w + "' is not a switch on the Cheats page (noday, secret, missionfix): kept as written").c_str());
                    }
                    w.clear();
                };
                for (char ch : value)
                {
                    if (ch == ',' || ch == ' ' || ch == ';' || ch == '\t')
                        take();
                    else
                        w.push_back(ch);
                }
                take();
            }
            else if (key == "GT4_CHEAT_CARD")
            {
                const std::string v = lower(trim(value));
                s.cheats.card = v == "key" || v == "on" || v == "1" || v == "auto";
                s.cheats.cardAuto = v == "auto";
                if (!s.cheats.card && !v.empty() && v != "off" && v != "0")
                    bad("off or key: the card stays off");
            }
            else if (key == "GT4_CHEAT_CARD_LIST")
            {
                std::vector<std::string> unknown;
                cardList.clear();
                for (const gt4cheats::Code *c : gt4cheats::parseList(value, &unknown))
                    cardList.emplace_back(c->id);
                sawCardList = true;
                for (const std::string &u : unknown)
                    bad(("'" + u + "' is not a card (credits, licence, test, event, mission, options)").c_str());
            }
            else if (key == "GT4_CHEAT_CARD_KEY")
            {
                // gt4_cheat_card.cpp: F2..F12 (F1 is the scanlines).
                const std::string v = lower(trim(value));
                long long fn = 0;
                if (v.size() >= 2 && v[0] == 'f' && parseInt(v.substr(1), fn) && fn >= 2 && fn <= 12)
                    s.cheats.key = strf("F%lld", fn);
                else
                {
                    s.cheats.key = "F2";
                    bad("F2..F12: F2");
                }
            }
            else if (key == "GT4_CHEAT_CARD_PLACE")
            {
                // gt4_cheat_card.cpp: auto, left, right, top-right (also "topright" and "corner"); anything else is auto
                // there. Kept as written (the runtime's other spellings of top-right written as top-right).
                const std::string v = lower(trim(value));
                if (v == "topright" || v == "corner")
                    s.cheats.place = "top-right";
                else if (v == "auto" || v.empty())
                    s.cheats.place.clear();
                else
                {
                    s.cheats.place = v;
                    if (v != "left" && v != "right" && v != "top-right")
                        bad("auto, left, right or top-right (GT4.exe reads this one as auto): kept as written");
                }
            }
            else if (key == "GT4_CHEAT_CARD_FOLLOW")
            {
                const std::string v = trim(value);
                if (v == "0" || v == "1")
                    s.cheats.follow = v == "1" ? std::string() : v;
                else
                    bad("0 or 1: 1");
            }
            else if (key == "GT4L_CHEATS_TICKED")
            {
                s.cheats.ticked.clear();
                for (const gt4cheats::Code *c : gt4cheats::parseList(value))
                    s.cheats.ticked.emplace_back(c->id);
                sawTicked = true;
            }
            else if (key == "GT4L_CHEATS_PAPER")
                s.cheats.letter = iequals(trim(value), "letter");
            else if (isTestOnlyKey(key))
                bad("a test switch of GT4.exe: not kept, and never passed on by the launcher");
            else
                s.other.emplace_back(key, value);
        }
        // The ticked cards keep the order they were written in (F2 shows them so; gt4cheats::parseList already takes each
        // card once). A file by hand without GT4L_CHEATS_TICKED: the card's list is the ticked set ("all" ticks all six).
        if (!sawTicked && sawCardList)
            s.cheats.ticked = cardList;
        if (!sawWheel)
            s.wheel.enabled = false;
        if (sawSteerInvert)
            s.wheel.steerInvert = steerInvertKey;
        if (webcamChosen && s.eyetoy == EyeToy::Off)
        {
            s.eyetoy = EyeToy::Webcam;
            s.eyetoyIndex = -1; // not connected when the file was written
        }
#if defined(GT4L_EDITION_FULL)
        // The radar's background opacity (GT4_RADAR_BG, the GT3 radar settings): a file from before it keeps the background it
        // had, 100 - the radar's see-through.
        if (!s.full.radarBgRead && s.full.radarLineRead)
            s.full.radarBg = 100 - std::clamp(s.full.liveWidgets[0].seeThrough, 0, 90);
#endif
        if (!s.wheel.ffbAboveCap && s.wheel.ffbGain > kFfbCap)
        {
            warnings.push_back("PS2X_WHEEL_FFB_GAIN above 25% without GT4L_FFB_ABOVE_CAP=1: shown as set, the slider stops at 25%");
        }
        return true;
    }

    bool loadSettings(const std::filesystem::path &file, Settings &settings, std::vector<std::string> &warnings)
    {
        std::string text;
        if (!readText(file, text))
            return false;
        return parseSettingsText(text, settings, warnings);
    }

    std::string renderSettings(const Settings &s, const std::string &version)
    {
        std::ostringstream o;
        const char *nl = "\r\n";
        auto line = [&](const std::string &t) { o << t << nl; };
        auto kv = [&](const char *k, const std::string &v) { o << k << '=' << v << nl; };
#if defined(GT4L_EDITION_FULL)
        line("# Gran Turismo 4 settings: GT4 Recompilation."); // (no edition name: developer 2026-10-20)
#else
        line("# Gran Turismo 4 settings: GT4 Recompilation, Classic edition.");
#endif
        line("# Written by the GT4 Recompilation launcher " + version + " on " + nowText() + ". GT4.exe reads this file");
        line("# when it is started without arguments (the launcher's Play does that, or a");
        line("# double-click). KEY=VALUE, one per line; '#' starts a comment. A setting that");
        line("# is already in the environment wins over this file.");
        line("");
        line("# ---- Disc ----");
        line("# The Gran Turismo 4 (USA) disc image (SCUS-97328, 5,314,478,080 bytes).");
        kv("GT4_DISC_IMAGE", s.disc);
        line("");
#if defined(GT4L_EDITION_FULL)
        // The Full edition's picture, screen shape and window (gt4full_settings.cpp).
        renderFullPicture(o, s);
#else
        line("# ---- Picture: TV scanlines (F1 turns them on and off while you play) ----");
        line("# crt = lines on at start, off = lines off at start");
        kv("GT3_DISPLAY_FILTER", s.linesOn ? "crt" : "off");
        line("# strength 0-100: 0 none, 50 a good TV of the time (default), 100 darker gaps");
        kv("GT3_CRT_SCANLINES", std::to_string(std::clamp(s.strength, 0, 100)));
        line("# auto (default), fill, integer (a whole number of screen rows per TV line)");
        kv("GT4_PICTURE_FIT", fitKey(s.fit));
        line("# interlace flicker 0-100 (0 = off, the default; it flickers on an LCD)");
        kv("GT4_SCANLINE_FIELD", std::to_string(std::clamp(s.field, 0, 100)));
        line("");
        line("# ---- Picture: scaling and interlacing ----");
        line("# How the picture is scaled to the screen: smooth (bilinear, the default),");
        line("# sharp (crisp pixels with soft edges), nearest (hard-edged pixels)");
        kv("GT4_PICTURE_FILTER", pictureFilterKey(s.filter));
        line("# How the two interlaced fields (two half pictures a 480i TV showed in turn)");
        line("# make one picture: blend (the default: both together, blended a little, as a");
        line("# TV smoothed them), adaptive (both together where the picture is still, one");
        line("# at a time where it moves), bob (one at a time), weave (both together, as they are)");
        kv("GT4_DEINTERLACE", deinterlaceKey(s.deinterlace));
        line("");
        line("# ---- Window ----");
        line("# 1 = borderless full screen on the monitor below; 0 = a window of the size below");
        kv("GT3_FULLSCREEN", s.fullscreen ? "1" : "0");
        kv("GT3_WINDOW_SIZE", strf("%dx%d", s.windowW, s.windowH));
        line("# The monitor: its desktop position x,y, then its name (GT4_MONITOR needs the");
        line("# runtime-settings patch; without it GT4 opens on the main display).");
        if (s.monitorSet)
            kv("GT4_MONITOR", strf("%d,%d", s.monitorX, s.monitorY) + (s.monitorName.empty() ? std::string() : " " + s.monitorName));
        else
            line("# GT4_MONITOR=0,0");
        line("");
#endif
        line("# ---- Controller: the DualShock 2 in port 1 ----");
        kv("GT3_VIBRATION", s.vibration ? "1" : "0");
        line("# The mapping: input-profile rows separated by ';' (the PS2X_INPUT_PROFILE file");
        line("# syntax; PS2X_INPUT needs the runtime-settings patch).");
        kv("PS2X_INPUT", padProfileRows(s.pad));
        line("");
        line("# ---- Player 2: the DualShock 2 in port 2 ----");
        line("# auto = the second controller, as when a pad is plugged into port 2 (a lone controller always");
        line("# stays player 1's; a wheel or pedal set is never player 2's); off = nothing in port 2;");
        line("# guid:<id> = that controller (the launcher keeps its name on GT4L_PAD2_NAME below). Without the line");
        line("# GT4.exe's own default applies (off: nothing in port 2).");
        if (s.pad2.mode != Pad2Settings::Mode::Off || s.pad2.explicitChoice)
            kv("GT4_PAD2", pad2Value(s.pad2));
        else
            line("# GT4_PAD2=auto");
        line("");
        line("# ---- Wheel and pedals: GT4 sees a Logitech Driving Force Pro on USB port 1 ----");
        line("# PS2X_WHEEL=dinput uses the wheel below (DirectInput); without it, no wheel.");
        if (s.wheel.enabled)
            kv("PS2X_WHEEL", "dinput");
        else
            line("# PS2X_WHEEL=dinput");
        kv("PS2X_WHEEL_DEVICE", s.wheel.device.empty() ? "auto" : s.wheel.device);
        kv("PS2X_WHEEL_PEDALS", s.wheel.pedals.empty() ? "wheel" : s.wheel.pedals);
        kv("PS2X_WHEEL_MODEL", s.wheel.model);
        kv("PS2X_WHEEL_ROTATION", std::to_string(s.wheel.rotation));
        kv("PS2X_WHEEL_STEER_INVERT", s.wheel.steerInvert ? "1" : "0");
        line("# Force feedback starts off; strength 0..1 (0.25 = 25%; 10% for a first test).");
        kv("PS2X_WHEEL_FFB", s.wheel.ffb ? "1" : "0");
        kv("PS2X_WHEEL_FFB_GAIN", numberText(std::clamp(s.wheel.ffbGain, 0.0f, 1.0f), 2));
        kv("PS2X_WHEEL_FFB_INVERT", s.wheel.ffbInvert ? "1" : "0");
        line("# Axes, pedal calibration, buttons, effects: wheel config lines separated by ';'");
        line("# (the PS2X_WHEEL_CONFIG file syntax; PS2X_WHEEL_LINES needs the runtime-settings patch).");
        kv("PS2X_WHEEL_LINES", wheelConfigLines(s.wheel));
        line("");
        line("# ---- Memory cards ----");
        line("# Slot 1: folder = the folder card game\\mc0 (as today), or a PCSX2-style .ps2 card file");
        line("# (a relative path is taken from game\\). Slot 2: none = empty, or a .ps2 card file.");
        line("# GT4_MC0 / GT4_MC1 need the ps2-card patch (analysis\\saves\\ps2-card); without it slot 1");
        line("# is the folder card and slot 2 is empty.");
        kv("GT4_MC0", s.cards[0].file && !s.cards[0].path.empty() ? s.cards[0].path : std::string("folder"));
        kv("GT4_MC1", s.cards[1].file && !s.cards[1].path.empty() ? s.cards[1].path : std::string("none"));
        line("");
        line("# ---- USB stick: GT4's photo export (game\\usb0\\GT4 unless GT4_USB_ROOT is set) ----");
        kv("GT4_USB", s.usb ? "1" : "0");
        if (!s.usbRoot.empty())
            kv("GT4_USB_ROOT", s.usbRoot);
        line("");
        line("# ---- EyeToy camera: GT4 used it for one thing, the Nike One 2022 (GT Mode, USA, Nike dealer) ----");
        line("# absent = no camera (default); shirt = the built-in GT shirt picture (read from the disc,");
        line("# no camera needed); webcam:<n> = Windows video capture device n (Media Foundation's order;");
        line("# the launcher keeps n on GT4L_EYETOY_CAMERA below); image:<file> = a still picture.");
        line("# GT4_EYETOY needs the EyeToy service (analysis\\phase5b\\eyetoy, in wheel-glue's all-in-one patch).");
        if (s.eyetoy == EyeToy::Webcam && s.eyetoyIndex < 0)
            line("# The chosen webcam was not connected when this was written: no camera until it is.");
        kv("GT4_EYETOY", eyeToyValue(s));
        line("");
        {
            const CheatSettings &c = s.cheats;
            line("# ---- Cheats: GT4's own button codes (the launcher's Cheats page; everything off = GT4 as shipped) ----");
            line("# GT4_CHEATS, a comma list, changes GT4's menu scripts in memory as they are read from the disc (the");
            line("# disc and your save are not changed by it): noday = the five codes work before Day 367; secret =");
            line("# controller 1 may type the hidden Options code; missionfix = the Mission Hall code passes the");
            line("# highlighted mission instead of giving 10,000,000 credits. What a code does is saved when GT4 saves:");
            line("# back up your save first (Saves page).");
            std::vector<std::string> words;
            if (c.noday)
                words.push_back("noday");
            if (c.secret)
                words.push_back("secret");
            if (c.missionfix)
                words.push_back("missionfix");
            for (const std::string &w : c.otherWords)
                words.push_back(w);
            if (words.empty())
                line("# GT4_CHEATS=noday");
            else
                kv("GT4_CHEATS", join(words, ","));
            line("# The in-game cheat card: key = " + c.key + " in GT4 shows the cards in GT4_CHEAT_CARD_LIST, in that order (" + c.key +
                 " again: the next card, then hidden); off without the line.");
            if (c.card)
                kv("GT4_CHEAT_CARD", c.cardAuto ? "auto" : "key");
            else
                line("# GT4_CHEAT_CARD=key");
            if (c.card)
                kv("GT4_CHEAT_CARD_LIST", c.ticked.empty() ? std::string("all") : join(c.ticked, ","));
            if (c.card || c.key != "F2")
                kv("GT4_CHEAT_CARD_KEY", c.key);
            if (!c.place.empty())
                kv("GT4_CHEAT_CARD_PLACE", c.place);
            if (!c.follow.empty())
                kv("GT4_CHEAT_CARD_FOLLOW", c.follow);
            line("");
        }
#if defined(GT4L_EDITION_FULL)
        // The renderer, the network, and the choices for what GT4.exe does not have yet (gt4full_settings.cpp).
        renderFullMore(o, s);
        if (!s.other.empty())
        {
            line("# ---- Other settings (kept as they were) ----");
            for (const auto &[k, v] : s.other)
                kv(k.c_str(), v);
            line("");
        }
#else
        if (!s.other.empty())
        {
            line("# ---- Other settings (kept as they were; the Classic edition's GT4.exe ignores keys that are not its own) ----");
            for (const auto &[k, v] : s.other)
                kv(k.c_str(), v);
            line("");
        }
#endif
        line("# ---- The launcher's own (GT4.exe ignores GT4L_ keys) ----");
        kv("GT4L_PREVIEW", s.preview);
        kv("GT4L_FFB_ABOVE_CAP", s.wheel.ffbAboveCap ? "1" : "0");
        if (!s.wheel.deviceName.empty())
            kv("GT4L_WHEEL_NAME", s.wheel.deviceName);
        if (!s.wheel.pedalsName.empty())
            kv("GT4L_PEDALS_NAME", s.wheel.pedalsName);
        if (s.eyetoy == EyeToy::Webcam && s.eyetoyIndex < 0)
            kv("GT4L_EYETOY", "webcam");
        if (s.eyetoy == EyeToy::Webcam && !s.eyetoyCamera.empty())
            kv("GT4L_EYETOY_CAMERA", s.eyetoyCamera);
        if (s.pad2.mode == Pad2Settings::Mode::Chosen && !s.pad2.name.empty())
            kv("GT4L_PAD2_NAME", s.pad2.name);
        kv("GT4L_CHEATS_TICKED", join(s.cheats.ticked, ","));
        kv("GT4L_CHEATS_PAPER", s.cheats.letter ? "letter" : "a4");
        if (!s.exe.empty())
            kv("GT4L_EXE", s.exe);
        if (!s.lastPlayed.empty())
        {
            kv("GT4L_LAST_PLAYED", s.lastPlayed);
            kv("GT4L_LAST_EXIT", std::to_string(s.lastExit));
            if (s.lastMinutes >= 0)
                kv("GT4L_LAST_MINUTES", std::to_string(s.lastMinutes));
        }
#if defined(GT4L_EDITION_FULL)
        renderFullLauncher(o, s);
#endif
        return o.str();
    }

    bool saveSettings(const std::filesystem::path &file, const Settings &settings, const std::string &version, std::string &error)
    {
        return writeFileAtomic(file, renderSettings(settings, version), error);
    }

    std::vector<std::pair<std::string, std::string>> settingsEnvironment(const Settings &s, bool withOther)
    {
        std::vector<std::pair<std::string, std::string>> env;
        std::vector<std::string> warnings;
        Settings copy;
        // Same values the file gives: render, then read the KEY=VALUE lines back.
        std::istringstream in(renderSettings(s, "env"));
        std::string line;
        while (std::getline(in, line))
        {
            if (!line.empty() && line.back() == '\r')
                line.pop_back();
            const size_t eq = line.find('=');
            if (line.empty() || line[0] == '#' || eq == std::string::npos || eq == 0)
                continue;
            const std::string key = line.substr(0, eq);
            if (startsWith(key, "GT4L_"))
                continue;
            if (!managedKeys().count(key) && !withOther)
                continue;
            env.emplace_back(key, line.substr(eq + 1));
        }
        // (1.4 review) A cheat switch that is off is passed as off, never left out: GT4.exe started without arguments (the
        // Classic package) fills an absent key from gt4-start.env, which could still hold an older "on" if the file could
        // not be written. "off" is not empty, so the file's value never replaces it (main.cpp setDefault), and the cheats
        // runtime reads it as nothing on.
        auto has = [&](const char *k) { return std::any_of(env.begin(), env.end(), [&](const auto &e) { return e.first == k; }); };
        if (!has("GT4_CHEATS"))
            env.emplace_back("GT4_CHEATS", "off");
        if (!has("GT4_CHEAT_CARD"))
            env.emplace_back("GT4_CHEAT_CARD", "off");
        return env;
    }

    std::string launchRenderer(bool exeRead, bool exeClassic, const Settings &s)
    {
#if defined(GT4L_EDITION_FULL)
        // The Full edition writes GT3_RENDERER=vk itself (gt4full_settings.cpp): nothing to add.
        (void)exeRead;
        (void)exeClassic;
        (void)s;
        return {};
#else
        if (!exeRead || exeClassic)
            return {};
        for (const auto &[k, v] : s.other)
            if (upper(trim(k)) == "GT3_RENDERER")
                return {};
        return "gl";
#endif
    }
}
