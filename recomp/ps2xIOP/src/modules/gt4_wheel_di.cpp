#include "gt4_wheel_di.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>

// DirectInput host wheel for GT4's LGDEV service (header: gt4_wheel_di.h).
// Design: analysis/phase5/specs/p5b-wheel-poweroff-padman.md 2.9.4 / 2.9.5;
// integration notes: analysis/extras/wheel-glue/README.md.

namespace ps2x::iop::gt4
{
    namespace
    {
        constexpr int32_t DiMax = 10000; // DI_FFNOMINALMAX

        std::string lower(std::string_view s)
        {
            std::string out(s);
            for (char &c : out)
                c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            return out;
        }

        std::string trim(std::string_view s)
        {
            const auto b = s.find_first_not_of(" \t\r\n");
            if (b == std::string_view::npos)
                return {};
            const auto e = s.find_last_not_of(" \t\r\n");
            return std::string(s.substr(b, e - b + 1));
        }

        bool parseDouble(const std::string &text, double &value)
        {
            if (text.empty())
                return false;
            char *end = nullptr;
            const double v = std::strtod(text.c_str(), &end);
            if (!end || *end != '\0' || !std::isfinite(v))
                return false;
            value = v;
            return true;
        }

        bool parseInt(const std::string &text, long long &value, int base = 10)
        {
            if (text.empty())
                return false;
            char *end = nullptr;
            const long long v = std::strtoll(text.c_str(), &end, base);
            if (!end || *end != '\0')
                return false;
            value = v;
            return true;
        }

        bool parseBool(const std::string &text, bool &value)
        {
            const std::string t = lower(text);
            if (t == "1" || t == "on" || t == "yes" || t == "true")
                value = true;
            else if (t == "0" || t == "off" || t == "no" || t == "false")
                value = false;
            else
                return false;
            return true;
        }

        int32_t roundClamp(double v, double lo, double hi)
        {
            return static_cast<int32_t>(std::lround(std::clamp(v, lo, hi)));
        }

        struct ButtonName
        {
            const char *name;
            uint32_t bit;
        };
        constexpr ButtonName ButtonNames[] = {
            {"cross", WheelButton::Cross}, {"square", WheelButton::Square}, {"circle", WheelButton::Circle},
            {"triangle", WheelButton::Triangle}, {"r1", WheelButton::R1}, {"l1", WheelButton::L1},
            {"r2", WheelButton::R2}, {"l2", WheelButton::L2}, {"select", WheelButton::Select},
            {"start", WheelButton::Start}, {"b10", WheelButton::B10}, {"b11", WheelButton::B11},
            {"b12", WheelButton::B12}, {"b13", WheelButton::B13}, {"none", 0u},
        };

        const char *modelName(WheelModel model)
        {
            switch (model)
            {
            case WheelModel::DrivingForcePro: return "Driving Force Pro";
            case WheelModel::DrivingForce: return "Driving Force";
            case WheelModel::GtForce: return "GT Force";
            }
            return "?";
        }

        // Vendors whose force-feedback devices are wheels (USB VIDs): Logitech,
        // Fanatec (Endor AG), Thrustmaster (Guillemot), Moza (Gudsen).
        bool wheelVendor(uint16_t vid)
        {
            return vid == 0x046d || vid == 0x0eb7 || vid == 0x044f || vid == 0x346e;
        }

        bool matchesSpec(const DiDeviceInfo &d, const std::string &spec)
        {
            if (spec.empty())
                return false;
            if (spec.front() == '{')
                return lower(spec) == lower(d.instance);
            if (spec.size() == 9 && spec[4] == ':')
            {
                long long vid = 0, pid = 0;
                if (parseInt(spec.substr(0, 4), vid, 16) && parseInt(spec.substr(5), pid, 16))
                    return d.vendor == vid && d.product == pid;
            }
            return lower(d.name).find(lower(spec)) != std::string::npos;
        }

        bool isPedalSet(const DiDeviceInfo &d) { return lower(d.name).find("pedal") != std::string::npos; }

        const DiDeviceInfo *selectWheel(const std::vector<DiDeviceInfo> &list, const std::string &spec)
        {
            if (lower(spec) != "auto")
            {
                for (const auto &d : list)
                    if (matchesSpec(d, spec))
                        return &d;
                return nullptr;
            }
            // auto: a driving device with force feedback, then a force-feedback device of a wheel
            // vendor or named "wheel", then any driving device. Never a gamepad or a pedal set.
            const DiDeviceInfo *best = nullptr;
            int bestRank = 0;
            for (const auto &d : list)
            {
                if (d.type == DiType::Gamepad || isPedalSet(d))
                    continue;
                int rank = 0;
                if (d.type == DiType::Driving && d.forceFeedback)
                    rank = 3;
                else if (d.forceFeedback && (wheelVendor(d.vendor) || lower(d.name).find("wheel") != std::string::npos))
                    rank = 2;
                else if (d.type == DiType::Driving)
                    rank = 1;
                if (rank > bestRank)
                {
                    best = &d;
                    bestRank = rank;
                }
            }
            return best;
        }

        const DiDeviceInfo *selectPedals(const std::vector<DiDeviceInfo> &list, const std::string &spec,
                                         const std::string &wheelInstance)
        {
            const bool automatic = lower(spec) == "auto";
            for (const auto &d : list)
            {
                if (lower(d.instance) == lower(wheelInstance))
                    continue;
                if (automatic ? isPedalSet(d) : matchesSpec(d, spec))
                    return &d;
            }
            return nullptr;
        }

        uint32_t axisOffset(DiAxis axis) { return 4u * static_cast<uint32_t>(axis); }

        // Registry of live adapters for the crash handler.
        std::mutex g_registryMutex;
        std::vector<DirectInputWheel *> g_registry;
    }

    // ------------------------------------------------------------------------ names
    const char *diAxisName(DiAxis axis)
    {
        static const char *names[] = {"x", "y", "z", "rx", "ry", "rz", "slider0", "slider1"};
        const unsigned i = static_cast<unsigned>(axis);
        return i < DiAxisCount ? names[i] : "?";
    }

    bool parseDiAxis(std::string_view text, DiAxis &axis)
    {
        const std::string t = lower(text);
        for (unsigned i = 0; i < DiAxisCount; ++i)
            if (t == diAxisName(static_cast<DiAxis>(i)))
            {
                axis = static_cast<DiAxis>(i);
                return true;
            }
        if (t == "slider")
        {
            axis = DiAxis::Slider0;
            return true;
        }
        return false;
    }

    const char *diStatusName(DiStatus status)
    {
        switch (status)
        {
        case DiStatus::Ok: return "ok";
        case DiStatus::NotAcquired: return "not acquired";
        case DiStatus::InputLost: return "input lost";
        case DiStatus::Gone: return "unplugged";
        case DiStatus::Failed: return "failed";
        }
        return "?";
    }

    const char *diEffectKindName(DiEffectKind kind)
    {
        switch (kind)
        {
        case DiEffectKind::Constant: return "constant";
        case DiEffectKind::Spring: return "spring";
        case DiEffectKind::Damper: return "damper";
        case DiEffectKind::Friction: return "friction";
        }
        return "?";
    }

    std::string describeDiEffect(const DiEffect &e)
    {
        std::ostringstream o;
        o << diEffectKindName(e.kind);
        if (e.kind == DiEffectKind::Constant)
            o << " magnitude " << e.magnitude;
        else
            o << " offset " << e.condition.offset << " k " << e.condition.positiveCoefficient << '/'
              << e.condition.negativeCoefficient << " sat " << e.condition.positiveSaturation << '/'
              << e.condition.negativeSaturation << " deadband " << e.condition.deadBand;
        if (e.durationUs != 0xffffffffu)
            o << " duration " << e.durationUs << "us";
        return o.str();
    }

    std::string DiDeviceInfo::describe() const
    {
        char ids[16];
        std::snprintf(ids, sizeof(ids), "%04x:%04x", vendor, product);
        std::string type_ = type == DiType::Driving        ? "driving"
                            : type == DiType::Gamepad      ? "gamepad"
                            : type == DiType::Joystick     ? "joystick"
                            : type == DiType::Supplemental ? "supplemental"
                            : type == DiType::Flight       ? "flight"
                                                           : "type 0x" + std::to_string(type);
        std::string out = "'" + name + "' " + ids + " " + type_ + (forceFeedback ? ", force feedback" : "");
        if (axes)
        {
            out += ", axes";
            for (unsigned i = 0; i < DiAxisCount; ++i)
                if (axes & (1u << i))
                    out += std::string(" ") + diAxisName(static_cast<DiAxis>(i));
        }
        return out;
    }

    // ------------------------------------------------------------------------ config
    DiWheelConfig::DiWheelConfig()
    {
        // DFP button numbering (spec 2.5): host buttons 0..13 -> DFP buttons 0..13. A G29 /
        // G923 (PlayStation) reports Cross, Square, Circle, Triangle, the right / left paddles,
        // R2, L2, Share, Options, R3, L3 in this order [I].
        const uint32_t dfp[14] = {WheelButton::Cross, WheelButton::Square, WheelButton::Circle, WheelButton::Triangle,
                                  WheelButton::R1,    WheelButton::L1,     WheelButton::R2,     WheelButton::L2,
                                  WheelButton::Select, WheelButton::Start, WheelButton::B10,    WheelButton::B11,
                                  WheelButton::B12,   WheelButton::B13};
        for (unsigned i = 0; i < 14; ++i)
            buttons[i] = dfp[i];
        brake.axis = DiAxis::Rz;
    }

    double DiWheelConfig::effectiveLock() const
    {
        if (lock > 0.0)
            return lock;
        return model == WheelModel::DrivingForcePro ? 900.0 : 200.0;
    }

    std::string DiWheelConfig::describe() const
    {
        std::ostringstream o;
        o << "wheel " << wheel << ", pedals " << pedals << ", steering " << diAxisName(steerAxis)
          << (steerInvert ? " inverted" : "") << " (base " << rotation << " deg, GT4 full lock at " << effectiveLock()
          << " deg)";
        auto pedal = [&](const char *name, const Pedal &p) {
            o << ", " << name << ' ';
            if (!p.enabled)
                o << "none";
            else
            {
                o << diAxisName(p.axis);
                if (p.calibrated)
                    o << " (" << p.released << ".." << p.full << ')';
            }
        };
        pedal("gas", gas);
        pedal("brake", brake);
        o << ", hat " << (pov >= 0 ? "pov" + std::to_string(pov) : std::string("none")) << ", as " << modelName(model)
          << ", force feedback " << (ffb ? "on" : "off");
        if (ffb)
        {
            o << " (master " << std::lround(ffbGain * 100.0) << "%, slew " << ffbSlew << "/s"
              << (ffbInvert ? ", inverted" : "") << (ffbPauseUnfocused ? ", paused when unfocused" : "") << ", effects";
            if (playConstant)
                o << " constant";
            if (playSpring)
                o << " spring";
            if (playDamper)
                o << " damper";
            if (playFriction)
                o << " friction";
            o << ')';
        }
        return o.str();
    }

    bool applyDiWheelConfigLine(DiWheelConfig &config, std::string_view rawLine, std::string &error)
    {
        std::string line = trim(rawLine.substr(0, rawLine.find('#')));
        if (line.empty())
            return true;
        std::istringstream in(line);
        std::vector<std::string> t;
        for (std::string w; in >> w;)
            t.push_back(w);
        const std::string key = lower(t[0]);
        auto fail = [&](const std::string &why) {
            error = "'" + line + "': " + why;
            return false;
        };
        auto need = [&](size_t n) { return t.size() >= n; };
        if (key == "wheel" || key == "pedals")
        {
            if (!need(2))
                return fail("needs a device");
            // The rest of the line: a device name may contain spaces.
            const std::string value = trim(line.substr(t[0].size()));
            (key == "wheel" ? config.wheel : config.pedals) = value;
            return true;
        }
        if (key == "steer")
        {
            if (!need(2) || !parseDiAxis(t[1], config.steerAxis))
                return fail("steer <axis> [invert]");
            config.steerInvert = t.size() > 2 && lower(t[2]) == "invert";
            return true;
        }
        if (key == "rotation" || key == "lock")
        {
            double v = 0;
            if (!need(2) || !parseDouble(t[1], v) || v < (key == "lock" ? 0.0 : 90.0) || v > 3600.0)
                return fail("degrees, 90..3600");
            (key == "rotation" ? config.rotation : config.lock) = v;
            return true;
        }
        if (key == "gas" || key == "brake")
        {
            DiWheelConfig::Pedal &p = key == "gas" ? config.gas : config.brake;
            if (!need(2))
                return fail("<axis> [released=<raw>] [full=<raw>] | none");
            if (lower(t[1]) == "none")
            {
                p.enabled = false;
                return true;
            }
            DiWheelConfig::Pedal next = p;
            next.enabled = true;
            next.calibrated = false;
            if (!parseDiAxis(t[1], next.axis))
                return fail("unknown axis " + t[1]);
            bool haveReleased = false, haveFull = false;
            for (size_t i = 2; i < t.size(); ++i)
            {
                const auto eq = t[i].find('=');
                long long v = 0;
                if (eq == std::string::npos || !parseInt(t[i].substr(eq + 1), v) || v < -32768 || v > 32767)
                    return fail("released=<raw> / full=<raw>, -32768..32767");
                const std::string name = lower(t[i].substr(0, eq));
                if (name == "released")
                {
                    next.released = static_cast<int32_t>(v);
                    haveReleased = true;
                }
                else if (name == "full")
                {
                    next.full = static_cast<int32_t>(v);
                    haveFull = true;
                }
                else
                    return fail("unknown " + name);
            }
            if (haveReleased != haveFull)
                return fail("give both released= and full=, or neither (self-calibrating)");
            if (haveReleased && next.released == next.full)
                return fail("released and full must differ");
            next.calibrated = haveReleased;
            p = next;
            return true;
        }
        if (key == "pedal-deadzone")
        {
            double v = 0;
            if (!need(2) || !parseDouble(t[1], v) || v < 0.0 || v > 0.5)
                return fail("0..0.5");
            config.pedalDeadzone = v;
            return true;
        }
        if (key == "button")
        {
            long long index = 0;
            if (!need(3) || !parseInt(t[1], index) || index < 0 || index > 127)
                return fail("button <host index 0..127> <name>");
            const std::string name = lower(t[2]);
            for (const auto &b : ButtonNames)
                if (name == b.name)
                {
                    config.buttons[static_cast<size_t>(index)] = b.bit;
                    return true;
                }
            return fail("unknown button " + t[2]);
        }
        if (key == "hat")
        {
            long long v = 0;
            if (need(2) && lower(t[1]) == "none")
            {
                config.pov = -1;
                return true;
            }
            if (!need(2) || !parseInt(t[1], v) || v < 0 || v > 3)
                return fail("hat <pov 0..3> | none");
            config.pov = static_cast<int>(v);
            return true;
        }
        if (key == "model")
        {
            const std::string m = need(2) ? lower(t[1]) : "";
            if (m == "dfp")
                config.model = WheelModel::DrivingForcePro;
            else if (m == "df")
                config.model = WheelModel::DrivingForce;
            else if (m == "gtforce")
                config.model = WheelModel::GtForce;
            else
                return fail("dfp | df | gtforce");
            return true;
        }
        if (key == "ffb" || key == "ffb-invert" || key == "ffb-pause-unfocused" || key == "log")
        {
            bool v = false;
            if (!need(2) || !parseBool(t[1], v))
                return fail("on | off");
            (key == "ffb" ? config.ffb : key == "ffb-invert" ? config.ffbInvert : key == "log" ? config.verbose : config.ffbPauseUnfocused) = v;
            return true;
        }
        if (key == "ffb-gain")
        {
            double v = 0;
            if (!need(2) || !parseDouble(t[1], v) || v < 0.0 || v > 1.0)
                return fail("0..1");
            config.ffbGain = v;
            return true;
        }
        if (key == "ffb-slew")
        {
            double v = 0;
            if (!need(2) || !parseDouble(t[1], v) || v < 0.5 || v > 100.0)
                return fail("0.5..100 (full scale per second)");
            config.ffbSlew = v;
            return true;
        }
        if (key == "ffb-effects")
        {
            if (!need(2))
                return fail("a comma list of constant, spring, damper, friction (or none)");
            bool c = false, s = false, d = false, f = false;
            std::istringstream list(lower(t[1]));
            for (std::string item; std::getline(list, item, ',');)
            {
                if (item == "constant")
                    c = true;
                else if (item == "spring")
                    s = true;
                else if (item == "damper")
                    d = true;
                else if (item == "friction")
                    f = true;
                else if (item != "none")
                    return fail("unknown effect " + item);
            }
            config.playConstant = c;
            config.playSpring = s;
            config.playDamper = d;
            config.playFriction = f;
            return true;
        }
        return fail("unknown setting");
    }

    bool parseDiWheelConfig(std::string_view text, DiWheelConfig &config, std::string &error)
    {
        std::istringstream lines{std::string(text)};
        std::string line;
        unsigned number = 0;
        while (std::getline(lines, line))
        {
            ++number;
            std::string why;
            if (!applyDiWheelConfigLine(config, line, why))
            {
                error = "line " + std::to_string(number) + ": " + why;
                return false;
            }
        }
        return true;
    }

    bool diWheelConfigFrom(const EnvLookup &lookup, DiWheelConfig &config, std::string &error)
    {
        auto get = [&](const char *name) -> std::string {
            const char *v = lookup ? lookup(name) : nullptr;
            return v ? trim(v) : std::string();
        };
        if (const std::string path = get("PS2X_WHEEL_CONFIG"); !path.empty())
        {
            std::ifstream file(path, std::ios::binary);
            if (!file)
            {
                error = "PS2X_WHEEL_CONFIG: cannot open " + path;
                return false;
            }
            std::ostringstream text;
            text << file.rdbuf();
            std::string why;
            if (!parseDiWheelConfig(text.str(), config, why))
            {
                error = "PS2X_WHEEL_CONFIG " + path + ": " + why;
                return false;
            }
        }
        // PS2X_WHEEL_LINES: config lines separated by ';' (the GT4 Stock Launcher
        // writes them into gt4-start.env), after the file, before the single keys.
        if (const std::string v = get("PS2X_WHEEL_LINES"); !v.empty())
        {
            std::string text = v;
            std::replace(text.begin(), text.end(), ';', '\n');
            std::string why;
            if (!parseDiWheelConfig(text, config, why))
            {
                error = "PS2X_WHEEL_LINES: " + why + " (lines count from 1, split at ';')";
                return false;
            }
        }
        const struct
        {
            const char *env;
            const char *key;
        } single[] = {
            {"PS2X_WHEEL_DEVICE", "wheel"},   {"PS2X_WHEEL_PEDALS", "pedals"}, {"PS2X_WHEEL_ROTATION", "rotation"},
            {"PS2X_WHEEL_LOCK", "lock"},      {"PS2X_WHEEL_MODEL", "model"},   {"PS2X_WHEEL_FFB", "ffb"},
            {"PS2X_WHEEL_FFB_GAIN", "ffb-gain"}, {"PS2X_WHEEL_FFB_INVERT", "ffb-invert"}, {"PS2X_WHEEL_LOG", "log"},
        };
        for (const auto &s : single)
        {
            const std::string v = get(s.env);
            if (v.empty())
                continue;
            std::string why;
            if (!applyDiWheelConfigLine(config, std::string(s.key) + " " + v, why))
            {
                error = std::string(s.env) + "=" + v + ": " + why;
                return false;
            }
        }
        if (const std::string v = get("PS2X_WHEEL_STEER_INVERT"); !v.empty())
        {
            bool b = false;
            if (!parseBool(v, b))
            {
                error = "PS2X_WHEEL_STEER_INVERT=" + v + ": 0 or 1";
                return false;
            }
            config.steerInvert = b;
        }
        config.ffbLogPath = get("PS2X_WHEEL_FFB_LOG");
        return true;
    }

    // ------------------------------------------------------------------------ mappings
    int16_t diSteer(int32_t raw, const DiWheelConfig &config)
    {
        double norm = std::clamp(static_cast<double>(raw) / 32767.0, -1.0, 1.0);
        if (config.steerInvert)
            norm = -norm;
        const double rimDegrees = norm * config.rotation * 0.5;
        const double halfLock = config.effectiveLock() * 0.5;
        return static_cast<int16_t>(roundClamp(rimDegrees / halfLock * 32767.0, -32767.0, 32767.0));
    }

    double diPedalTravel(int32_t raw, int32_t released, int32_t full, double deadzone)
    {
        if (full == released)
            return 0.0;
        double travel = std::clamp((static_cast<double>(raw) - released) / (static_cast<double>(full) - released), 0.0, 1.0);
        if (travel <= deadzone)
            return 0.0;
        return (travel - deadzone) / (1.0 - deadzone);
    }

    int32_t diPedalFarEnd(int32_t rest) { return rest >= 0 ? -32768 : 32767; }

    uint32_t diButtons(const DiState &state, const DiWheelConfig &config)
    {
        uint32_t out = 0;
        for (size_t i = 0; i < state.buttons.size(); ++i)
            if (state.buttons[i] & 0x80u)
                out |= config.buttons[i];
        return out & WheelButton::Mask;
    }

    uint8_t diHat(uint32_t pov)
    {
        if ((pov & 0xffffu) == 0xffffu)
            return 0xff;
        return static_cast<uint8_t>(((pov % 36000u) + 2250u) / 4500u % 8u);
    }

    bool diEffectKindFor(WheelEffectType type, DiEffectKind &kind)
    {
        switch (type)
        {
        case WheelEffectType::Constant: kind = DiEffectKind::Constant; return true;
        case WheelEffectType::Spring: kind = DiEffectKind::Spring; return true;
        case WheelEffectType::Damper: kind = DiEffectKind::Damper; return true;
        case WheelEffectType::Friction: kind = DiEffectKind::Friction; return true;
        }
        return false;
    }

    DiEffect diEffectFor(WheelEffectType type, const WheelEffectParams &params, uint32_t lgdevGain, const DiWheelConfig &config)
    {
        DiEffect e;
        diEffectKindFor(type, e.kind);
        e.axisOffset = axisOffset(config.steerAxis);
        // [I] LGDEV's duration / start delay units (GT4 sends -1 and 0, so they never matter).
        e.durationUs = params.duration() > 0 ? static_cast<uint32_t>(params.duration()) * 1000u : 0xffffffffu;
        e.startDelayUs = params.delay() > 0 ? static_cast<uint32_t>(params.delay()) * 1000u : 0u;
        const double master = std::clamp(config.ffbGain, 0.0, 1.0);
        const double steerSign = config.steerInvert ? -1.0 : 1.0;
        const uint32_t gain = std::min<uint32_t>(lgdevGain, 0xffu);
        if (e.kind == DiEffectKind::Constant)
        {
            // LGDEV's x (-127..127) at the stored direction, LGDEV's gain rounding, then the host's
            // master gain and sign. Positive x pushes towards positive steering [I: test item].
            const int32_t x = static_cast<int32_t>(std::lround(wheelConstantLevel(params) * 127.0));
            const double level = static_cast<double>(wheelApplyGain(x, gain)) / 127.0;
            const double out = level * master * steerSign * (config.ffbInvert ? -1.0 : 1.0);
            e.magnitude = roundClamp(out * DiMax, -DiMax, DiMax);
            return e;
        }
        // Conditions: LGDEV's coefficients and saturations are 0..255 [C clamps IOP+0xbb4]; LGDEV
        // scales its clip by gain/255 [C 0x0d50]; the master gain scales coefficient and
        // saturation alike (the whole force). The offset is in wheel-axis units.
        const auto c = params.condition(0);
        const double scale = master * static_cast<double>(gain) / 255.0;
        DiCondition d;
        d.offset = roundClamp(std::clamp(c.offset / 32767.0, -1.0, 1.0) * DiMax * steerSign, -DiMax, DiMax);
        int32_t kPos = roundClamp(std::clamp(c.coefficientPos / 255.0, -1.0, 1.0) * DiMax * scale, -DiMax, DiMax);
        int32_t kNeg = roundClamp(std::clamp(c.coefficientNeg / 255.0, -1.0, 1.0) * DiMax * scale, -DiMax, DiMax);
        uint32_t sPos = static_cast<uint32_t>(roundClamp(std::clamp(c.saturationPos / 255.0, 0.0, 1.0) * DiMax * scale, 0, DiMax));
        uint32_t sNeg = static_cast<uint32_t>(roundClamp(std::clamp(c.saturationNeg / 255.0, 0.0, 1.0) * DiMax * scale, 0, DiMax));
        if (steerSign < 0)
        {
            std::swap(kPos, kNeg); // LGDEV's positive side is the host axis's negative side
            std::swap(sPos, sNeg);
        }
        d.positiveCoefficient = kPos;
        d.negativeCoefficient = kNeg;
        d.positiveSaturation = sPos;
        d.negativeSaturation = sNeg;
        d.deadBand = roundClamp(std::abs(c.deadBand) / 32767.0 * DiMax, 0, DiMax);
        e.condition = d;
        return e;
    }

    // ------------------------------------------------------------------------ adapter
    DirectInputWheel::DirectInputWheel(std::shared_ptr<DiBackend> backend, DiWheelConfig config, Options options, Log log)
        : backend_(std::move(backend)), config_(std::move(config)), options_(options), log_(std::move(log))
    {
        if (!config_.ffbLogPath.empty())
        {
            if (FILE *f = std::fopen(config_.ffbLogPath.c_str(), "w"))
            {
                std::fprintf(f, "# GT4 host wheel (DirectInput): seconds vsync action (DirectInput units, -10000..10000)\n");
                ffbFile_ = f;
            }
            else
                logLine("cannot write the force log " + config_.ffbLogPath);
        }
        {
            std::lock_guard lock(g_registryMutex);
            g_registry.push_back(this);
        }
        logLine("DirectInput wheel: " + config_.describe());
        if (options_.startThread)
            thread_ = std::thread([this] { worker(); });
    }

    DirectInputWheel::~DirectInputWheel()
    {
        {
            std::lock_guard lock(wakeMutex_);
            stop_ = true;
        }
        wake_.notify_all();
        if (thread_.joinable())
            thread_.join();
        {
            std::lock_guard lock(g_registryMutex);
            std::erase(g_registry, this);
        }
        if (!poweredOff_)
            stopEverything("exit");
        if (ffbFile_)
            std::fclose(static_cast<FILE *>(ffbFile_));
    }

    WheelModel DirectInputWheel::model(unsigned) const { return config_.model; }

    bool DirectInputWheel::poll(unsigned device, WheelInput &input)
    {
        if (device != 0)
            return false;
        std::lock_guard lock(mutex_);
        input = snapshot_;
        return true;
    }

    void DirectInputWheel::command(unsigned device, const WheelCommand &c)
    {
        if (device != 0)
            return;
        bool urgent = false;
        int rangeAsked = 0;
        {
            std::lock_guard lock(mutex_);
            Desired &d = desired_;
            DesiredEffect &e = d.effects[c.effect & 63u];
            switch (c.kind)
            {
            case WheelCommandKind::Open: d.open = true; break;
            case WheelCommandKind::Close:
                // LGDEV's close destroys every effect of the device (IOP+0x7eac + 0x78d4).
                d.open = false;
                d.effects = {};
                break;
            case WheelCommandKind::StopAll:
                for (auto &x : d.effects)
                    x.playing = false;
                ++d.stopAll;
                break;
            case WheelCommandKind::AutoCentre: d.autoCentre = c.value ? 1 : 0; break;
            case WheelCommandKind::Range:
                // DFP 0xf8 03 / 02. A host base keeps its own rotation (config "rotation"): the
                // steering is scaled to GT4's lock instead.
                if (c.value != rangeLogged_)
                    rangeAsked = rangeLogged_ = c.value;
                break;
            case WheelCommandKind::Gain: d.gain = static_cast<uint32_t>(std::clamp(c.value, 0, 255)); break;
            case WheelCommandKind::EffectCreate:
                e.exists = true;
                e.type = c.type;
                e.params = c.params;
                e.playing = false;
                break;
            case WheelCommandKind::EffectUpdate:
                if (e.exists)
                    e.params = c.params;
                break;
            case WheelCommandKind::EffectStart:
                if (e.exists)
                {
                    e.playing = true;
                    ++e.startCount;
                }
                break;
            case WheelCommandKind::EffectStop: e.playing = false; break;
            case WheelCommandKind::EffectUnload: e = {}; break;
            case WheelCommandKind::PowerOff:
                d.powerOff = true;
                d.effects = {};
                urgent = true;
                break;
            }
            ++d.version;
        }
        if (rangeAsked)
            logLine("GT4 set the wheel to " + std::to_string(rangeAsked) + " degrees; the base stays at its own " +
                    std::to_string(std::lround(config_.rotation)) + " (config rotation), steering scaled to GT4's " +
                    std::to_string(std::lround(config_.effectiveLock())) + "-degree lock");
        if (urgent)
        {
            // Power-off: the worker stops the forces now, not at its next period.
            {
                std::lock_guard lock(wakeMutex_);
                kick_ = true;
            }
            wake_.notify_all();
        }
    }

    void DirectInputWheel::onVBlank(uint64_t vsync)
    {
        std::lock_guard lock(mutex_);
        vsync_ = vsync;
    }

    DirectInputWheel::Stats DirectInputWheel::stats() const
    {
        std::lock_guard lock(mutex_);
        return stats_;
    }

    double DirectInputWheel::monotonicNow() const
    {
        using namespace std::chrono;
        return duration<double>(steady_clock::now().time_since_epoch()).count();
    }

    void DirectInputWheel::worker()
    {
        for (;;)
        {
            pump(monotonicNow());
            std::unique_lock lock(wakeMutex_);
            wake_.wait_for(lock, std::chrono::milliseconds(options_.periodMs), [this] { return stop_ || kick_; });
            if (stop_)
                break;
            kick_ = false;
        }
    }

    void DirectInputWheel::logLine(const std::string &line)
    {
        if (log_)
            log_(line);
    }

    void DirectInputWheel::ffbLog(const std::string &line)
    {
        if (!ffbFile_)
            return;
        uint64_t vsync;
        {
            std::lock_guard lock(mutex_);
            vsync = vsync_;
        }
        const double t = lastPump_ < 0 ? 0.0 : lastPump_;
        std::fprintf(static_cast<FILE *>(ffbFile_), "%.3f %llu %s\n", t, static_cast<unsigned long long>(vsync), line.c_str());
    }

    void DirectInputWheel::pump(double now)
    {
        // The slew step uses at most 20 ms, so a worker that stalled (a driver blocking in
        // SetParameters) still moves the constant force in small steps afterwards.
        const double dt = lastPump_ < 0 ? 0.0 : std::clamp(now - lastPump_, 0.0, 0.02);
        lastPump_ = now;
        Desired d;
        {
            std::lock_guard lock(mutex_);
            d = desired_;
        }
        if (d.powerOff)
        {
            if (!poweredOff_)
            {
                stopEverything("power-off");
                poweredOff_ = true;
                std::lock_guard lock(mutex_);
                stats_.poweredOff = true;
                stats_.ffbActive = false;
            }
            return;
        }
        if (poweredOff_)
            return;
        if (!backendReady_)
        {
            if (now < nextEnumerate_)
                return;
            nextEnumerate_ = now + 5.0;
            backendReady_ = backend_ && backend_->ready(backendError_);
            if (!backendReady_)
            {
                logLine("DirectInput unavailable (" + (backend_ ? backendError_ : std::string("no backend")) + "); no host wheel");
                return;
            }
            nextEnumerate_ = now;
        }
        ensureDevices(now);
        if (!wheel_)
            return;
        readInput(now);
        if (wheel_ && ffbActive_)
            applyForces(d, now, dt);
        std::lock_guard lock(mutex_);
        stats_.ffbActive = ffbActive_;
        stats_.paused = paused_;
        for (const auto &a : applied_)
            if (a.handle >= 0 && a.effect.kind == DiEffectKind::Constant)
            {
                stats_.constantApplied = a.effect.magnitude;
                break;
            }
    }

    void DirectInputWheel::ensureDevices(double now)
    {
        const bool separatePedals = lower(config_.pedals) != "wheel";
        if (wheel_ && (!separatePedals || pedals_))
            return;
        if (now < nextEnumerate_)
            return;
        nextEnumerate_ = now + 2.0;
        const std::vector<DiDeviceInfo> list = backend_->enumerate();
        {
            std::lock_guard lock(mutex_);
            ++stats_.enumerations;
        }
        std::string text;
        for (const auto &d : list)
            text += "\n  " + d.describe() + " " + d.instance;
        if (text != lastDeviceList_)
        {
            lastDeviceList_ = text;
            if (config_.verbose || !wheel_)
                logLine("game controllers:" + (text.empty() ? std::string(" none") : text));
        }
        if (!wheel_)
        {
            const DiDeviceInfo *pick = selectWheel(list, config_.wheel);
            if (!pick)
            {
                if (!loggedNoWheel_)
                    logLine("no wheel matches '" + config_.wheel + "'; looking again every 2 s");
                loggedNoWheel_ = true;
                return;
            }
            openWheel(*pick, now);
        }
        if (wheel_ && separatePedals && !pedals_)
            openPedals(list);
    }

    void DirectInputWheel::openWheel(const DiDeviceInfo &info, double now)
    {
        if (!options_.window)
        {
            if (!loggedNoWindow_)
                logLine("no game window: a DirectInput wheel needs the visible game window; no host wheel");
            loggedNoWindow_ = true;
            return;
        }
        std::string error;
        std::unique_ptr<DiDevice> dev = backend_->open(info, options_.window, error);
        if (!dev)
        {
            logLine("cannot open " + info.describe() + ": " + error);
            return;
        }
        const bool anyEffect = config_.playConstant || config_.playSpring || config_.playDamper || config_.playFriction;
        bool exclusive = config_.ffb && info.forceFeedback && anyEffect;
        DiStatus st = dev->acquire(exclusive);
        if (st != DiStatus::Ok && exclusive)
        {
            logLine("exclusive access to " + info.name + " refused (" + diStatusName(st) +
                    "; another program may hold its force feedback): steering and pedals only");
            exclusive = false;
            st = dev->acquire(false);
        }
        if (st != DiStatus::Ok)
        {
            logLine("cannot acquire " + info.describe() + ": " + diStatusName(st));
            return;
        }
        exclusive_ = exclusive;
        ffbActive_ = exclusive;
        if (ffbActive_)
        {
            dev->setDeviceGain(DiMax); // strength comes from the effects (master gain), not the device gain
            dev->stopAll();
        }
        const DiDeviceInfo opened = dev->info();
        {
            std::lock_guard lock(deviceMutex_);
            wheel_ = std::move(dev);
            live_.store(wheel_.get());
        }
        applied_ = {};
        appliedAutoCentre_ = -1;
        {
            std::lock_guard lock(mutex_);
            appliedStopAll_ = desired_.stopAll;
            ++stats_.opens;
            stats_.device = opened.describe();
        }
        gasState_ = {};
        brakeState_ = {};
        failures_ = 0;
        loggedUnsupported_ = {};
        loggedNoWheel_ = false;
        if (everOpened_)
            ++plug_; // LGDEV sees a re-plug even if it never polled the gap
        everOpened_ = true;
        std::string forces = "off";
        if (ffbActive_)
            forces = "on, master " + std::to_string(std::lround(config_.ffbGain * 100.0)) + "%";
        else if (config_.ffb && !opened.forceFeedback)
            forces = "off (the device has none)";
        else if (config_.ffb)
            forces = "off (no exclusive access)";
        logLine("host wheel " + opened.describe() + " -> GT4 " + modelName(config_.model) + "; steering " +
                diAxisName(config_.steerAxis) + (config_.steerInvert ? " inverted" : "") + ", base " +
                std::to_string(std::lround(config_.rotation)) + " deg, full lock at " +
                std::to_string(std::lround(config_.effectiveLock())) + " deg; force feedback " + forces);
        ffbLog("open " + opened.describe() + (ffbActive_ ? " exclusive" : " shared"));
        (void)now;
    }

    void DirectInputWheel::openPedals(const std::vector<DiDeviceInfo> &list)
    {
        const DiDeviceInfo *pick = selectPedals(list, config_.pedals, wheel_ ? wheel_->info().instance : std::string());
        if (!pick)
            return; // the wheel's own axes serve until a pedal set appears
        std::string error;
        std::unique_ptr<DiDevice> dev = backend_->open(*pick, options_.window, error);
        if (!dev)
        {
            logLine("cannot open the pedals " + pick->describe() + ": " + error);
            return;
        }
        const DiStatus st = dev->acquire(false);
        if (st != DiStatus::Ok)
        {
            logLine("cannot acquire the pedals " + pick->describe() + ": " + diStatusName(st));
            return;
        }
        pedals_ = std::move(dev);
        gasState_ = {};
        brakeState_ = {};
        logLine("pedals " + pedals_->info().describe());
        std::lock_guard lock(mutex_);
        stats_.pedals = pedals_->info().describe();
    }

    void DirectInputWheel::releaseEffect(unsigned slot, const char *why)
    {
        AppliedEffect &a = applied_[slot];
        if (a.handle < 0 || !wheel_)
        {
            a = {};
            return;
        }
        if (a.playing)
            wheel_->stopEffect(a.handle);
        wheel_->releaseEffect(a.handle);
        ffbLog("release slot " + std::to_string(slot) + " " + diEffectKindName(a.effect.kind) + " (" + why + ")");
        a = {};
        std::lock_guard lock(mutex_);
        ++stats_.releases;
    }

    void DirectInputWheel::forcesLost()
    {
        // After a re-acquire the device stopped every effect (and may have reset its centring).
        for (auto &a : applied_)
            a.playing = false;
        appliedAutoCentre_ = -1;
    }

    void DirectInputWheel::dropWheel(const char *why, double now)
    {
        logLine(std::string("host wheel lost (") + why + "); looking again");
        if (wheel_)
        {
            for (unsigned slot = 0; slot < applied_.size(); ++slot)
                if (applied_[slot].handle >= 0)
                    wheel_->releaseEffect(applied_[slot].handle);
            wheel_->unacquire();
        }
        applied_ = {};
        {
            std::lock_guard lock(deviceMutex_);
            live_.store(nullptr);
            wheel_.reset();
        }
        dropPedals("wheel lost");
        exclusive_ = ffbActive_ = false;
        nextEnumerate_ = now + 1.0;
        std::lock_guard lock(mutex_);
        snapshot_.connected = false;
        stats_.connected = false;
        ++stats_.losses;
    }

    void DirectInputWheel::dropPedals(const char *why)
    {
        if (!pedals_)
            return;
        logLine(std::string("pedals lost (") + why + ")");
        pedals_->unacquire();
        pedals_.reset();
        gasState_ = {};
        brakeState_ = {};
        std::lock_guard lock(mutex_);
        stats_.pedals.clear();
    }

    uint16_t DirectInputWheel::pedal(const DiWheelConfig::Pedal &p, PedalState &st, int32_t raw)
    {
        if (!p.enabled)
            return 0;
        int32_t released = p.released, full = p.full;
        if (!p.calibrated)
        {
            if (!st.haveRest)
            {
                st.haveRest = true;
                st.rest = raw;
            }
            full = diPedalFarEnd(st.rest);
            // The released end is the furthest the pedal has been from the far end (a pedal pressed a
            // little at the first sample, or a device that reports 0 before its first report).
            if (std::abs(static_cast<int64_t>(raw) - full) > std::abs(static_cast<int64_t>(st.rest) - full))
                st.rest = raw;
            released = st.rest;
        }
        const double travel = diPedalTravel(raw, released, full, config_.pedalDeadzone);
        return static_cast<uint16_t>(std::lround(travel * 65535.0));
    }

    void DirectInputWheel::readInput(double now)
    {
        DiState s;
        DiStatus st = wheel_->poll(s);
        if (st == DiStatus::NotAcquired || st == DiStatus::InputLost)
        {
            {
                std::lock_guard lock(mutex_);
                ++stats_.reacquires;
            }
            const DiStatus a = wheel_->acquire(exclusive_);
            if (a == DiStatus::Ok)
            {
                st = wheel_->poll(s);
                if (exclusive_)
                    forcesLost();
            }
            else if (a == DiStatus::Gone)
                st = DiStatus::Gone;
        }
        if (st == DiStatus::Gone)
        {
            dropWheel("unplugged", now);
            return;
        }
        if (st != DiStatus::Ok)
        {
            if (++failures_ >= 250) // about a second of failed reads
                dropWheel("not responding", now);
            return;
        }
        failures_ = 0;
        DiState ps = s;
        bool pedalsFresh = true;
        if (pedals_)
        {
            DiStatus p = pedals_->poll(ps);
            if (p == DiStatus::NotAcquired || p == DiStatus::InputLost)
                if (pedals_->acquire(false) == DiStatus::Ok)
                    p = pedals_->poll(ps);
            if (p == DiStatus::Gone)
            {
                dropPedals("unplugged");
                nextEnumerate_ = now + 1.0;
                ps = s;
            }
            else if (p != DiStatus::Ok)
                pedalsFresh = false;
        }
        WheelInput in;
        {
            std::lock_guard lock(mutex_);
            in = snapshot_;
        }
        in.connected = true;
        in.plug = plug_;
        in.steer = diSteer(s.axis[static_cast<unsigned>(config_.steerAxis)], config_);
        if (pedalsFresh)
        {
            in.pedalA = pedal(config_.gas, gasState_, ps.axis[static_cast<unsigned>(config_.gas.axis)]);
            in.pedalB = pedal(config_.brake, brakeState_, ps.axis[static_cast<unsigned>(config_.brake.axis)]);
        }
        in.buttons = diButtons(s, config_);
        in.hat = config_.pov >= 0 ? diHat(s.pov[static_cast<unsigned>(config_.pov)]) : 0xff;
        {
            std::lock_guard lock(mutex_);
            snapshot_ = in;
            stats_.connected = true;
        }
        lastState_ = s;
        // Axis monitor (log 1): what moved, once a second, to find the pedal axes on a new wheel.
        if (config_.verbose && now >= nextMonitor_)
        {
            nextMonitor_ = now + 1.0;
            std::string moved;
            auto scan = [&moved](const DiState &current, DiState &previous, const char *prefix) {
                for (unsigned i = 0; i < DiAxisCount; ++i)
                    if (std::abs(current.axis[i] - previous.axis[i]) > 1000)
                        moved += std::string(" ") + prefix + diAxisName(static_cast<DiAxis>(i)) + "=" + std::to_string(current.axis[i]);
                previous = current;
            };
            scan(s, monitorState_, "");
            if (pedals_)
                scan(ps, monitorPedals_, "pedals.");
            for (unsigned i = 0; i < s.buttons.size(); ++i)
                if (s.buttons[i] & 0x80u)
                    moved += " button" + std::to_string(i);
            if (!moved.empty())
            {
                char tail[96];
                std::snprintf(tail, sizeof(tail), " -> steer %d gas %u brake %u buttons 0x%08x hat %u", in.steer,
                              static_cast<unsigned>(in.pedalA), static_cast<unsigned>(in.pedalB), in.buttons,
                              static_cast<unsigned>(in.hat));
                logLine("input:" + moved + tail);
            }
        }
    }

    void DirectInputWheel::applyForces(const Desired &d, double now, double dt)
    {
        // Pause while the game window is not in front (default): a direct-drive base must not
        // keep pushing while the player is at the desktop.
        const bool unfocused = config_.ffbPauseUnfocused && !backend_->windowFocused(options_.window);
        if (unfocused)
        {
            if (unfocusedSince_ < 0)
                unfocusedSince_ = now;
        }
        else
            unfocusedSince_ = -1.0;
        const bool pause = unfocused && now - unfocusedSince_ >= 0.25;
        if (pause != paused_)
        {
            paused_ = pause;
            logLine(pause ? "forces paused (the game window is not in front)" : "forces resumed");
            ffbLog(pause ? "pause" : "resume");
        }
        if (d.stopAll != appliedStopAll_)
        {
            appliedStopAll_ = d.stopAll;
            wheel_->stopAll();
            for (auto &a : applied_)
                a.playing = false;
            ffbLog("stopall");
            std::lock_guard lock(mutex_);
            ++stats_.stopAlls;
        }
        if (d.autoCentre >= 0 && d.autoCentre != appliedAutoCentre_)
        {
            appliedAutoCentre_ = d.autoCentre;
            const DiStatus st = wheel_->setAutoCentre(d.autoCentre != 0);
            ffbLog(std::string("autocentre ") + (d.autoCentre ? "on" : "off") + (st == DiStatus::Ok ? "" : std::string(" (") + diStatusName(st) + ")"));
            if (st == DiStatus::NotAcquired)
            {
                forcesLost();
                appliedAutoCentre_ = d.autoCentre;
            }
        }
        for (unsigned slot = 0; slot < applied_.size(); ++slot)
        {
            const DesiredEffect &w = d.effects[slot];
            AppliedEffect &a = applied_[slot];
            DiEffectKind kind{};
            const bool known = w.exists && diEffectKindFor(w.type, kind);
            const bool enabled = known && (kind == DiEffectKind::Constant   ? config_.playConstant
                                           : kind == DiEffectKind::Spring   ? config_.playSpring
                                           : kind == DiEffectKind::Damper   ? config_.playDamper
                                                                            : config_.playFriction);
            if (!(known && enabled && d.open))
            {
                if (a.handle >= 0)
                    releaseEffect(slot, "unloaded");
                a = {};
                continue;
            }
            if (a.handle >= 0 && a.type != w.type)
                releaseEffect(slot, "type changed");
            if (a.failed && a.type == w.type)
                continue;
            const DiEffect target = diEffectFor(w.type, w.params, d.gain, config_);
            if (a.handle < 0)
            {
                DiEffect initial = target;
                if (kind == DiEffectKind::Constant)
                    initial.magnitude = 0; // ramps up from zero (slew limit)
                const int h = wheel_->createEffect(initial);
                if (h < 0)
                {
                    a = {};
                    a.failed = true;
                    a.type = w.type;
                    if (!loggedUnsupported_[static_cast<unsigned>(kind)])
                        logLine(std::string("the wheel refused a ") + diEffectKindName(kind) + " effect; GT4's " +
                                diEffectKindName(kind) + " is not played");
                    loggedUnsupported_[static_cast<unsigned>(kind)] = true;
                    continue;
                }
                a.handle = h;
                a.type = w.type;
                a.effect = initial;
                a.playing = false;
                a.failed = false;
                a.startCount = 0;
                ffbLog("create slot " + std::to_string(slot) + " " + describeDiEffect(initial));
                std::lock_guard lock(mutex_);
                ++stats_.creates;
            }
            const bool play = w.playing && !pause;
            DiEffect next = target;
            if (kind == DiEffectKind::Constant)
            {
                // Slew limit on the constant force (the one GT4 changes every frame): a sign error
                // or a sudden full-scale value reaches the rim over ~1/slew seconds.
                const int32_t goal = play ? target.magnitude : 0;
                const int32_t step = std::max<int32_t>(1, static_cast<int32_t>(std::lround(config_.ffbSlew * DiMax * dt)));
                const int32_t current = a.effect.magnitude;
                next.magnitude = goal > current ? std::min(goal, current + step) : std::max(goal, current - step);
            }
            if (!(next == a.effect))
            {
                if (wheel_->updateEffect(a.handle, next))
                {
                    a.effect = next;
                    ffbLog("update slot " + std::to_string(slot) + " " + describeDiEffect(next));
                    std::lock_guard lock(mutex_);
                    ++stats_.updates;
                }
            }
            const bool finite = target.durationUs != 0xffffffffu;
            if (play && (!a.playing || (finite && w.startCount != a.startCount)))
            {
                if (wheel_->startEffect(a.handle))
                {
                    a.playing = true;
                    ffbLog("start slot " + std::to_string(slot) + " " + diEffectKindName(kind));
                    std::lock_guard lock(mutex_);
                    ++stats_.starts;
                }
                a.startCount = w.startCount;
            }
            else if (!play && a.playing)
            {
                wheel_->stopEffect(a.handle);
                a.playing = false;
                ffbLog("stop slot " + std::to_string(slot) + " " + diEffectKindName(kind));
                std::lock_guard lock(mutex_);
                ++stats_.stops;
            }
        }
    }

    void DirectInputWheel::stopEverything(const char *why)
    {
        const bool hadWheel = static_cast<bool>(wheel_);
        if (wheel_)
        {
            if (ffbActive_)
            {
                wheel_->stopAll();
                for (auto &a : applied_)
                    if (a.handle >= 0)
                        wheel_->releaseEffect(a.handle);
                wheel_->setAutoCentre(false);
                ffbLog(std::string("stopall, effects released, autocentre off (") + why + ")");
            }
            wheel_->unacquire();
        }
        applied_ = {};
        {
            std::lock_guard lock(deviceMutex_);
            live_.store(nullptr);
            wheel_.reset();
        }
        if (pedals_)
        {
            pedals_->unacquire();
            pedals_.reset();
        }
        const bool hadForces = ffbActive_;
        exclusive_ = ffbActive_ = false;
        if (hadWheel)
            logLine(std::string(why) + ": " + (hadForces ? "forces stopped, auto-centre off, " : "") + "host wheel released");
        if (ffbFile_)
            std::fflush(static_cast<FILE *>(ffbFile_));
        std::lock_guard lock(mutex_);
        stats_.connected = false;
    }

    void DirectInputWheel::emergencyStop() noexcept
    {
        std::unique_lock lock(deviceMutex_, std::try_to_lock);
        if (!lock.owns_lock())
            return; // the worker is opening or releasing the device right now
        if (DiDevice *d = live_.load())
            d->emergencyStop();
    }

    void hostWheelEmergencyStopAll() noexcept
    {
        std::unique_lock lock(g_registryMutex, std::try_to_lock);
        if (!lock.owns_lock())
            return;
        for (DirectInputWheel *w : g_registry)
            w->emergencyStop();
    }

#if !defined(_WIN32)
    std::shared_ptr<DiBackend> createDirectInput8Backend() { return nullptr; }
#endif
}
