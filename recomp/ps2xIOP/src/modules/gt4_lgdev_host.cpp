#include "gt4_lgdev.h"
#include "gt4_wheel_port.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>

// Host side of the GT4 LGDEV service: LGDEV force maths shared with host
// adapters, the HostWheel factory (adapter point) and VirtualWheel, the
// scripted fake wheel used by the unit tests and hidden runs.

namespace ps2x::iop::gt4
{
    namespace
    {
        // LGDEV quarter-sine table, 64 entries 0..0x3ff (IOP+0x9040, .data).
        constexpr uint16_t SineTable[64] = {
            0x0000, 0x0019, 0x0032, 0x004b, 0x0064, 0x007d, 0x0096, 0x00af, 0x00c7, 0x00e0, 0x00f8, 0x0111, 0x0129,
            0x0141, 0x0158, 0x0170, 0x0187, 0x019e, 0x01b5, 0x01cc, 0x01e2, 0x01f8, 0x020e, 0x0223, 0x0238, 0x024d,
            0x0261, 0x0275, 0x0289, 0x029c, 0x02af, 0x02c2, 0x02d4, 0x02e5, 0x02f6, 0x0307, 0x0317, 0x0327, 0x0336,
            0x0345, 0x0353, 0x0361, 0x036e, 0x037a, 0x0387, 0x0392, 0x039d, 0x03a8, 0x03b2, 0x03bb, 0x03c4, 0x03cc,
            0x03d3, 0x03da, 0x03e1, 0x03e7, 0x03ec, 0x03f0, 0x03f4, 0x03f8, 0x03fb, 0x03fd, 0x03fe, 0x03ff};

        // IOP+0x73d4: (a * b +- c/2) / c, rounding half away from zero on a's sign.
        int32_t scaleRound(int32_t a, int32_t b, int32_t c)
        {
            int32_t half = c / 2;
            if (a < 0)
                half = -half;
            return (a * b + half) / c;
        }

        struct FactoryState
        {
            std::mutex mutex;
            HostWheelFactory factory;
        };
        FactoryState &factoryState()
        {
            static FactoryState state;
            return state;
        }

        std::string trim(std::string_view s)
        {
            const auto b = s.find_first_not_of(" \t\r\n");
            if (b == std::string_view::npos)
                return {};
            const auto e = s.find_last_not_of(" \t\r\n");
            return std::string(s.substr(b, e - b + 1));
        }

        const char *typeName(WheelEffectType type)
        {
            switch (type)
            {
            case WheelEffectType::Constant: return "constant";
            case WheelEffectType::Spring: return "spring";
            case WheelEffectType::Damper: return "damper";
            case WheelEffectType::Friction: return "friction";
            }
            return "?";
        }

        std::string hex32(uint32_t v)
        {
            char text[16];
            std::snprintf(text, sizeof(text), "0x%08x", v);
            return text;
        }
    }

    // ------------------------------------------------------------------------ maths
    double wheelConstantLevel(const WheelEffectParams &params)
    {
        // IOP+0x204..0x2b8: x component of magnitude at the stored direction.
        const uint32_t a = static_cast<uint32_t>(params.direction()) & 0xffu;
        int32_t s;
        if (a < 0x40)
            s = SineTable[a];
        else if (a < 0x80)
            s = SineTable[0x7f - a];
        else if (a < 0xc0)
            s = -SineTable[a - 0x80];
        else
            s = -SineTable[0xff - a];
        const int32_t x = scaleRound(params.magnitude() * s, 0x7f, 0x3fc00); // 255 * 1024
        return static_cast<double>(x) / 127.0;
    }

    int32_t wheelApplyGain(int32_t value, uint32_t gain)
    {
        return gain == 0xffu ? value : scaleRound(value, static_cast<int32_t>(gain), 0xff);
    }

    const char *wheelCommandName(WheelCommandKind kind)
    {
        switch (kind)
        {
        case WheelCommandKind::Open: return "Open";
        case WheelCommandKind::Close: return "Close";
        case WheelCommandKind::StopAll: return "StopAll";
        case WheelCommandKind::AutoCentre: return "AutoCentre";
        case WheelCommandKind::Range: return "Range";
        case WheelCommandKind::Gain: return "Gain";
        case WheelCommandKind::EffectCreate: return "EffectCreate";
        case WheelCommandKind::EffectUpdate: return "EffectUpdate";
        case WheelCommandKind::EffectStart: return "EffectStart";
        case WheelCommandKind::EffectStop: return "EffectStop";
        case WheelCommandKind::EffectUnload: return "EffectUnload";
        case WheelCommandKind::PowerOff: return "PowerOff";
        }
        return "?";
    }

    std::string describeWheelCommand(const WheelCommand &c)
    {
        std::ostringstream o;
        o << wheelCommandName(c.kind);
        switch (c.kind)
        {
        case WheelCommandKind::AutoCentre: o << (c.value ? " on" : " off"); break;
        case WheelCommandKind::Range: o << ' ' << c.value << " deg"; break;
        case WheelCommandKind::Gain: o << ' ' << c.value; break;
        case WheelCommandKind::EffectCreate:
        case WheelCommandKind::EffectUpdate:
        {
            o << " slot " << unsigned(c.effect) << " id " << hex32(c.effectId) << ' ' << typeName(c.type);
            if (c.kind == WheelCommandKind::EffectUpdate)
                o << " changed " << c.value;
            const WheelEffectParams &p = c.params;
            o << " dur " << p.duration() << " delay " << p.delay();
            if (c.type == WheelEffectType::Constant)
            {
                char level[32];
                std::snprintf(level, sizeof(level), "%+.3f", wheelConstantLevel(p));
                o << " mag " << p.magnitude() << " dir " << p.direction() << " level " << level;
            }
            else
            {
                const auto a = p.condition(0);
                o << " offset " << a.offset << " deadband " << a.deadBand << " sat " << a.saturationPos << '/'
                  << a.saturationNeg << " k " << a.coefficientPos << '/' << a.coefficientNeg;
            }
            break;
        }
        case WheelCommandKind::EffectStart:
            o << " slot " << unsigned(c.effect) << " id " << hex32(c.effectId) << ' ' << typeName(c.type) << (c.value ? " again" : "");
            break;
        case WheelCommandKind::EffectStop:
        case WheelCommandKind::EffectUnload:
            o << " slot " << unsigned(c.effect) << " id " << hex32(c.effectId) << ' ' << typeName(c.type);
            break;
        default: break;
        }
        return o.str();
    }

    // ------------------------------------------------------------------------ factory
    void setHostWheelFactory(HostWheelFactory factory)
    {
        auto &state = factoryState();
        std::lock_guard lock(state.mutex);
        state.factory = std::move(factory);
    }

    std::shared_ptr<HostWheel> createHostWheel(const std::function<void(std::string_view)> &log)
    {
        {
            auto &state = factoryState();
            std::lock_guard lock(state.mutex);
            if (state.factory)
            {
                auto wheel = state.factory();
                if (log)
                    log(wheel ? "host wheel from the registered adapter (" + std::to_string(wheel->deviceCount()) + " device(s))"
                              : std::string("registered wheel adapter has no wheel"));
                return applyWheelPort(std::move(wheel), log); // PS2X_WHEEL_PORT (gt4_wheel_port.h)
            }
        }
        const char *mode = std::getenv("PS2X_WHEEL");
        const std::string m = mode ? mode : "";
        if (m.empty() || m == "none" || m == "0")
            return nullptr; // no wheel: keep the log exactly as without this service
        VirtualWheel::Config config;
        if (m == "virtual" || m == "virtual1")
            config.devices = 1;
        else if (m == "virtual2")
            config.devices = 2;
        else
        {
            if (log)
                log("PS2X_WHEEL=" + m + " not understood (virtual, virtual2, none); no wheel");
            return nullptr;
        }
        if (const char *model = std::getenv("PS2X_WHEEL_MODEL"); model && *model)
        {
            const std::string s = model;
            if (s == "df")
                config.model = WheelModel::DrivingForce;
            else if (s == "gtforce")
                config.model = WheelModel::GtForce;
            else if (s != "dfp" && log)
                log("PS2X_WHEEL_MODEL=" + s + " not understood (dfp, df, gtforce); using dfp");
        }
        if (const char *ffb = std::getenv("PS2X_WHEEL_FFB_LOG"); ffb && *ffb)
            config.ffbLogPath = ffb;
        auto wheel = std::make_shared<VirtualWheel>(config);
        std::string replay = "no replay (neutral input)";
        if (const char *path = std::getenv("PS2X_WHEEL_REPLAY"); path && *path)
        {
            std::string error;
            replay = wheel->loadReplayFile(path, error) ? std::string("replay ") + path : "replay " + std::string(path) + " NOT loaded: " + error;
        }
        if (log)
            log("virtual wheel x" + std::to_string(config.devices) + " (" +
                (config.model == WheelModel::DrivingForcePro ? "dfp" : config.model == WheelModel::DrivingForce ? "df" : "gtforce") +
                "), " + replay + (config.ffbLogPath.empty() ? "" : ", force log " + config.ffbLogPath));
        return applyWheelPort(std::move(wheel), log); // PS2X_WHEEL_PORT (gt4_wheel_port.h)
    }

    // ------------------------------------------------------------------------ VirtualWheel
    VirtualWheel::VirtualWheel(Config config) : config_(std::move(config))
    {
        config_.devices = std::min(std::max(config_.devices, 1u), MaxDevices);
        for (unsigned i = 0; i < config_.devices; ++i)
            input_[i].connected = true;
        if (!config_.ffbLogPath.empty())
        {
            FILE *f = std::fopen(config_.ffbLogPath.c_str(), "w");
            if (f)
            {
                std::fprintf(f, "# GT4 LGDEV force log: vsync device command (LGDEV units; level = constant x/127 before gain)\n");
                file_ = f;
            }
        }
    }

    VirtualWheel::~VirtualWheel()
    {
        if (file_)
            std::fclose(static_cast<FILE *>(file_));
    }

    bool VirtualWheel::poll(unsigned device, WheelInput &input)
    {
        std::lock_guard lock(mutex_);
        if (device >= config_.devices)
            return false;
        input = input_[device];
        return true;
    }

    void VirtualWheel::command(unsigned device, const WheelCommand &command)
    {
        std::lock_guard lock(mutex_);
        log_.push_back({vsync_, device, command});
        if (file_)
        {
            const std::string line = describeWheelCommand(command);
            std::fprintf(static_cast<FILE *>(file_), "%llu %u %s\n", static_cast<unsigned long long>(vsync_), device, line.c_str());
            if (command.kind == WheelCommandKind::PowerOff || command.kind == WheelCommandKind::Close)
                std::fflush(static_cast<FILE *>(file_));
        }
    }

    void VirtualWheel::onVBlank(uint64_t vsync)
    {
        std::lock_guard lock(mutex_);
        vsync_ = vsync;
        if (replay_.empty())
            return;
        for (unsigned d = 0; d < config_.devices; ++d)
        {
            const Entry *hit = nullptr;
            for (const auto &e : replay_)
                if (e.device == d && vsync >= e.begin && vsync < e.end)
                {
                    hit = &e;
                    break;
                }
            WheelInput next{};
            next.connected = true;
            next.plug = input_[d].plug;
            if (hit)
                next = fromEntry(*hit, input_[d]);
            input_[d] = next;
        }
    }

    WheelInput VirtualWheel::fromEntry(const Entry &e, const WheelInput &previous) const
    {
        WheelInput in{};
        in.plug = previous.plug;
        if (e.unplug)
            return in; // connected = false
        in.connected = true;
        const double half = config_.model == WheelModel::DrivingForcePro ? 450.0 : 100.0;
        const double steer = std::clamp(std::round(e.steerDegrees / half * 32767.0), -32767.0, 32767.0);
        in.steer = static_cast<int16_t>(steer);
        auto pedal = [](double pct) { return static_cast<uint16_t>(std::clamp(std::round(pct / 100.0 * 65535.0), 0.0, 65535.0)); };
        in.pedalA = pedal(e.gasPercent);
        in.pedalB = pedal(e.brakePercent);
        in.buttons = e.buttons & WheelButton::Mask;
        in.hat = e.hat;
        return in;
    }

    void VirtualWheel::set(unsigned device, const WheelInput &input)
    {
        std::lock_guard lock(mutex_);
        if (device < config_.devices)
            input_[device] = input;
    }

    bool VirtualWheel::loadReplay(std::string_view text, std::string &error)
    {
        std::vector<Entry> entries;
        std::istringstream lines{std::string(text)};
        std::string raw;
        unsigned number = 0;
        while (std::getline(lines, raw))
        {
            ++number;
            std::string line = raw.substr(0, raw.find('#'));
            line = trim(line);
            if (line.empty())
                continue;
            std::istringstream t(line);
            std::vector<std::string> tok;
            for (std::string w; t >> w;)
                tok.push_back(w);
            Entry e{};
            size_t i = 0;
            if (tok[0] == "p1" || tok[0] == "p2")
            {
                e.device = tok[0] == "p2" ? 1u : 0u;
                ++i;
            }
            auto fail = [&](const std::string &why) {
                error = "line " + std::to_string(number) + ": " + why;
                return false;
            };
            try
            {
                if (tok.size() < i + 3)
                    return fail("need begin end and steer-deg gas% brake% buttons [hat], or unplug");
                e.begin = std::stoull(tok[i]);
                e.end = std::stoull(tok[i + 1]);
                if (e.end <= e.begin)
                    return fail("end must be after begin (end is exclusive)");
                if (tok[i + 2] == "unplug")
                {
                    if (tok.size() != i + 3)
                        return fail("nothing may follow unplug");
                    e.unplug = true;
                }
                else
                {
                    if (tok.size() < i + 6 || tok.size() > i + 7)
                        return fail("expected steer-deg gas% brake% buttons-hex [hat]");
                    e.steerDegrees = std::stod(tok[i + 2]);
                    e.gasPercent = std::stod(tok[i + 3]);
                    e.brakePercent = std::stod(tok[i + 4]);
                    e.buttons = static_cast<uint32_t>(std::stoul(tok[i + 5], nullptr, 16));
                    e.hat = 0xff;
                    if (tok.size() == i + 7)
                    {
                        const unsigned long hat = std::stoul(tok[i + 6], nullptr, 0);
                        if (hat > 7 && hat != 0xff)
                            return fail("hat is 0..7 or 0xff");
                        e.hat = static_cast<uint8_t>(hat);
                    }
                }
            }
            catch (const std::exception &)
            {
                return fail("bad number");
            }
            if (e.device >= config_.devices)
                return fail("p2 needs PS2X_WHEEL=virtual2");
            entries.push_back(e);
        }
        std::lock_guard lock(mutex_);
        replay_ = std::move(entries);
        return true;
    }

    bool VirtualWheel::loadReplayFile(const std::string &path, std::string &error)
    {
        std::ifstream f(path, std::ios::binary);
        if (!f)
        {
            error = "cannot open " + path;
            return false;
        }
        std::ostringstream text;
        text << f.rdbuf();
        return loadReplay(text.str(), error);
    }

    std::vector<VirtualWheel::LoggedCommand> VirtualWheel::commands() const
    {
        std::lock_guard lock(mutex_);
        return log_;
    }

    void VirtualWheel::clearCommands()
    {
        std::lock_guard lock(mutex_);
        log_.clear();
    }

    uint64_t VirtualWheel::vsync() const
    {
        std::lock_guard lock(mutex_);
        return vsync_;
    }
}
