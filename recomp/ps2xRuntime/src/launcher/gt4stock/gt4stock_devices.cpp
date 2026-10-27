#include "gt4stock_devices.h"

#include "gt4stock_settings.h"
#include "gt4stock_util.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <map>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#define DIRECTINPUT_VERSION 0x0800
#include <windows.h>
#include <objbase.h>
#include <dinput.h>
#if defined(_MSC_VER)
#pragma comment(lib, "dinput8.lib")
#pragma comment(lib, "dxguid.lib")
#pragma comment(lib, "ole32.lib")
#endif
#endif

#if defined(PS2X_INPUT_GLFW)
#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>
#endif

namespace gt4stock
{
    // ------------------------------------------------------------------ pure helpers
    std::string DeviceInfo::idText() const { return strf("%04x:%04x", vid, pid); }

    const char *DeviceInfo::kindText() const
    {
        switch (kind)
        {
        case DeviceKind::Gamepad: return xinput ? "controller (XInput)" : "controller";
        case DeviceKind::Wheel: return "wheel";
        case DeviceKind::Pedals: return "pedals";
        case DeviceKind::Joystick: return "joystick";
        default: return "game device";
        }
    }

    std::string sdlGuidFromVidPid(uint16_t vid, uint16_t pid)
    {
        return strf("03000000%02x%02x0000%02x%02x000000000000", vid & 0xff, vid >> 8, pid & 0xff, pid >> 8);
    }

    bool wheelVendor(uint16_t vid) { return vid == 0x046d || vid == 0x0eb7 || vid == 0x044f || vid == 0x346e; }

    DeviceKind kindFromDiType(uint32_t diType, const std::string &name, bool forceFeedback, uint16_t vid)
    {
        const std::string n = lower(name);
        if (n.find("pedal") != std::string::npos)
            return DeviceKind::Pedals;
        switch (diType & 0xff)
        {
        case 0x16: return DeviceKind::Wheel;    // DI8DEVTYPE_DRIVING
        case 0x15: return DeviceKind::Gamepad;  // DI8DEVTYPE_GAMEPAD
        case 0x14:                              // DI8DEVTYPE_JOYSTICK
        case 0x17:                              // DI8DEVTYPE_FLIGHT
            if (forceFeedback && (wheelVendor(vid) || n.find("wheel") != std::string::npos))
                return DeviceKind::Wheel;
            return DeviceKind::Joystick;
        case 0x1b: return DeviceKind::Other;    // DI8DEVTYPE_SUPPLEMENTAL
        default: break;
        }
        if (forceFeedback && (wheelVendor(vid) || n.find("wheel") != std::string::npos))
            return DeviceKind::Wheel;
        return DeviceKind::Other;
    }

    bool deviceMatches(const DeviceInfo &d, const std::string &spec)
    {
        if (spec.empty())
            return false;
        if (spec.front() == '{')
            return lower(spec) == lower(d.instance);
        if (spec.size() == 9 && spec[4] == ':')
        {
            char *e1 = nullptr, *e2 = nullptr;
            const std::string a = spec.substr(0, 4), b = spec.substr(5);
            const long vid = std::strtol(a.c_str(), &e1, 16), pid = std::strtol(b.c_str(), &e2, 16);
            if (e1 && !*e1 && e2 && !*e2)
                return d.vid == vid && d.pid == pid;
        }
        return lower(d.name).find(lower(spec)) != std::string::npos;
    }

    namespace
    {
        bool isPedalSet(const DeviceInfo &d) { return lower(d.name).find("pedal") != std::string::npos; }
        bool isGamepadType(const DeviceInfo &d) { return (d.diType & 0xff) == 0x15; }
        bool isDrivingType(const DeviceInfo &d) { return (d.diType & 0xff) == 0x16; }
    }

    const DeviceInfo *pickWheel(const std::vector<DeviceInfo> &list, const std::string &spec)
    {
        if (lower(spec) != "auto" && !spec.empty())
        {
            for (const auto &d : list)
                if (deviceMatches(d, spec))
                    return &d;
            return nullptr;
        }
        const DeviceInfo *best = nullptr;
        int bestRank = 0;
        for (const auto &d : list)
        {
            if (isGamepadType(d) || isPedalSet(d))
                continue;
            int rank = 0;
            if (isDrivingType(d) && d.forceFeedback)
                rank = 3;
            else if (d.forceFeedback && (wheelVendor(d.vid) || lower(d.name).find("wheel") != std::string::npos))
                rank = 2;
            else if (isDrivingType(d))
                rank = 1;
            if (rank > bestRank)
            {
                best = &d;
                bestRank = rank;
            }
        }
        return best;
    }

    const DeviceInfo *pickPedals(const std::vector<DeviceInfo> &list, const std::string &spec, const DeviceInfo *wheel)
    {
        if (lower(spec) == "wheel" || spec.empty())
            return wheel;
        const bool automatic = lower(spec) == "auto";
        for (const auto &d : list)
        {
            if (wheel && lower(d.instance) == lower(wheel->instance))
                continue;
            if (automatic ? isPedalSet(d) : deviceMatches(d, spec))
                return &d;
        }
        return nullptr;
    }

    float ffbTestStrength(float strength, bool /*aboveCap*/)
    {
        // The test never goes past the 25% cap, even with "Allow more than 25%" ticked:
        // that unlock is for GT4's own forces. The test has no rim-angle guard (the
        // wheel-check tool has one), so it stays gentle on a direct-drive base.
        return std::clamp(strength, 0.0f, kFfbCap);
    }

    FfbTestParams ffbTestParams(FfbTest test, float strength, bool aboveCap)
    {
        const float s = ffbTestStrength(strength, aboveCap);
        FfbTestParams p;
        if (test == FfbTest::CentreSpring)
        {
            p.coefficient = std::lround(s * 10000.0f);
            p.saturation = static_cast<unsigned long>(std::lround(s * 10000.0f));
            p.durationMs = 2500;
            p.attackMs = 300;
            p.fadeMs = 300;
        }
        else
        {
            p.magnitude = std::lround(s * 0.5f * 10000.0f);
            p.durationMs = 600;
            p.attackMs = 150;
            p.fadeMs = 150;
        }
        return p;
    }

    std::vector<DeviceInfo> demoDevices()
    {
        std::vector<DeviceInfo> v;
        auto add = [&](const char *name, uint16_t vid, uint16_t pid, uint32_t type, bool ff, bool xinput, const char *instance) -> DeviceInfo & {
            DeviceInfo d;
            d.name = name;
            d.vid = vid;
            d.pid = pid;
            d.diType = type;
            d.forceFeedback = ff;
            d.xinput = xinput;
            d.demo = true;
            d.instance = instance;
            d.kind = kindFromDiType(type, d.name, ff, vid);
            if (xinput)
            {
                d.kind = DeviceKind::Gamepad;
                d.glfwGuid = "78696e70757401000000000000000000";
            }
            else
                d.glfwGuid = sdlGuidFromVidPid(vid, pid);
            d.live = true;
            v.push_back(d);
            return v.back();
        };
        {
            DeviceInfo &w = add("FANATEC Podium Wheel Base DD1", 0x0eb7, 0x0006, 0x16, true, false, "{6f1d2b61-d5a0-11cf-bfc7-444553540000}");
            w.axisMask = 0x7f;
            w.axes = {9830, 32767, 32767, 0, 0, 32767, 0, 0}; // steering 30% right, base pedal inputs unused
            w.buttonCount = 128;
            w.buttons[4] = 1; // right paddle held
        }
        {
            DeviceInfo &p = add("FANATEC ClubSport Pedals V3", 0x0eb7, 0x183b, 0x1b, false, false, "{6f1d2b62-d5a0-11cf-bfc7-444553540000}");
            p.axisMask = 0x07;
            // Each pedal rests at 32767 and reads -32768 fully pressed (demo).
            p.axes = {32767, -7864, 20971, 0, 0, 0, 0, 0}; // clutch released, gas 62%, brake 18%
            p.buttonCount = 0;
        }
        {
            DeviceInfo &x = add("Xbox Wireless Controller", 0x045e, 0x0b13, 0x15, false, true, "{6f1d2b63-d5a0-11cf-bfc7-444553540000}");
            x.axisMask = 0x3f;
            x.buttonCount = 16;
        }
        {
            DeviceInfo &d = add("DualSense Wireless Controller", 0x054c, 0x0ce6, 0x15, false, false, "{6f1d2b64-d5a0-11cf-bfc7-444553540000}");
            d.axisMask = 0x3f;
            d.buttonCount = 14;
        }
        return v;
    }

    // ------------------------------------------------------------------ player 2
    gt4pad2rules::Inventory pad2InventoryFromList(const std::vector<DeviceInfo> &list)
    {
        gt4pad2rules::Inventory inv{};
        int j = 0;
        for (const DeviceInfo &d : list)
            if (d.xinput && j < gt4pad2rules::Joysticks)
                inv[static_cast<size_t>(j++)] = {true, true, lower(d.glfwGuid), d.name};
        for (const DeviceInfo &d : list)
            if (!d.xinput && j < gt4pad2rules::Joysticks)
                inv[static_cast<size_t>(j++)] = {true, d.kind == DeviceKind::Gamepad, lower(d.glfwGuid), d.name};
        return inv;
    }

    Player1Sources player1Sources(const PadSettings &pad)
    {
        // readProfile's rows (ps2_input_profile.cpp), only what decides the devices: the source rows, and for every
        // binding row its device and whether it binds anything (a later row for the same control replaces the earlier).
        Player1Sources out;
        std::map<std::string, std::pair<int, bool>> bound; // control -> (device, binds something)
        for (const char *b : kPadButtonKeys)
            bound[std::string("button ") + b] = {0, true}; // InputProfile::defaults(): every button, the sticks, L2 / R2
        for (const char *s : kPadStickKeys)
            bound[std::string("stick ") + s] = {0, true};
        bound["pressure l2"] = {0, true};
        bound["pressure r2"] = {0, true};
        for (const std::string &raw : split(padProfileRows(pad), ';'))
        {
            const std::vector<std::string> w = [&] {
                std::vector<std::string> v;
                for (const std::string &x : split(trim(raw.substr(0, raw.find('#'))), ' '))
                    if (!trim(x).empty())
                        v.push_back(trim(x));
                return v;
            }();
            if (w.empty())
                continue;
            const std::string kind = w[0];
            long long a = 0, b = 0;
            auto num = [](const std::string &t, long long &v) { return parseInt(t, v); };
            if ((kind == "source" || kind == "source-guid") && w.size() == 4 && num(w[1], a) && a >= 0 && a < 4 && (w[2] == "raw" || w[2] == "mapped"))
            {
                Player1Source s;
                s.raw = w[2] == "raw";
                if (kind == "source" && num(w[3], b) && b >= 0 && b < (s.raw ? 16 : 4))
                    s.joystick = static_cast<int>(b);
                else if (kind == "source-guid" && w[3].size() == 32)
                {
                    s.joystick = -1;
                    s.guid = lower(w[3]);
                }
                else
                    continue;
                out.sources[static_cast<size_t>(a)] = s;
                continue;
            }
            // <kind> <name> <device> <button or axis> ...; stick-half <stick> <half> <device> <axis>; wheel-steer <device>
            // <axis>; wheel-pedal <name> <target> <device> <axis>.
            size_t at = 0;
            std::string control;
            if ((kind == "button" || kind == "wheel-button" || kind == "stick" || kind == "pressure") && w.size() >= 4)
            {
                at = 2;
                control = kind + " " + w[1];
            }
            else if (kind == "stick-half" && w.size() >= 5)
            {
                at = 3;
                control = kind + " " + w[1] + " " + w[2];
            }
            else if (kind == "wheel-steer" && w.size() >= 3)
            {
                at = 1;
                control = kind;
            }
            else if (kind == "wheel-pedal" && w.size() >= 5)
            {
                at = 3;
                control = kind + " " + w[1];
            }
            else
                continue;
            if (num(w[at], a) && a >= 0 && a < 4 && num(w[at + 1], b))
                bound[control] = {static_cast<int>(a), b >= 0};
        }
        out.usedSlots = 0;
        for (const auto &[control, v] : bound)
            if (v.second)
                out.usedSlots |= 1u << v.first;
        return out;
    }

    int resolvePlayer1Source(const Player1Source &s, const gt4pad2rules::Inventory &inv)
    {
        int selected = s.joystick;
        if (!s.guid.empty())
        {
            selected = -1;
            for (int i = 0; i < gt4pad2rules::Joysticks; ++i)
                if (inv[static_cast<size_t>(i)].connected && lower(inv[static_cast<size_t>(i)].guid) == s.guid)
                {
                    if (selected >= 0)
                        return -1; // an id that matches two connected joysticks picks neither
                    selected = i;
                }
        }
        if (selected < 0 || selected >= gt4pad2rules::Joysticks || !inv[static_cast<size_t>(selected)].connected ||
            (!s.raw && (selected >= gt4pad2rules::Gamepads || !inv[static_cast<size_t>(selected)].mapped)))
            return -1;
        return selected;
    }

    uint32_t player1Joysticks(const Player1Sources &p, const gt4pad2rules::Inventory &inv)
    {
        uint32_t mask = 0;
        for (int slot = 0; slot < 4; ++slot)
            if ((p.usedSlots >> slot) & 1u)
                if (const int j = resolvePlayer1Source(p.sources[static_cast<size_t>(slot)], inv); j >= 0 && j < 32)
                    mask |= 1u << j;
        return mask;
    }

    uint32_t adapterVidPid(const std::string &wheel)
    {
        // Gt4Pad2.cpp: wheel.size() == 9 && wheel[4] == ':', both halves hex (strtoul to the end).
        if (wheel.size() != 9 || wheel[4] != ':')
            return 0;
        const std::string vendor = wheel.substr(0, 4), product = wheel.substr(5);
        char *e1 = nullptr, *e2 = nullptr;
        const unsigned long v = std::strtoul(vendor.c_str(), &e1, 16), p = std::strtoul(product.c_str(), &e2, 16);
        if (*e1 || *e2)
            return 0;
        return static_cast<uint32_t>((v << 16) | p);
    }

    int pickPlayer2(const Pad2Settings &p, const PadSettings &player1, const gt4pad2rules::Inventory &inv, const std::string &wheelDevice, std::string *why)
    {
        gt4pad2rules::Choice c;
        std::string err;
        if (!gt4pad2rules::parseChoice(pad2Value(p), c, err))
        {
            if (why)
                *why = err;
            return -1;
        }
        return gt4pad2rules::choose(c, inv, player1Joysticks(player1Sources(player1), inv), -1, "", adapterVidPid(wheelDevice), why);
    }

    // ------------------------------------------------------------------ DirectInput
#if defined(_WIN32)
    namespace
    {
        std::string guidText(const GUID &g)
        {
            return strf("{%08lx-%04x-%04x-%02x%02x-%02x%02x%02x%02x%02x%02x}", g.Data1, g.Data2, g.Data3, g.Data4[0], g.Data4[1], g.Data4[2],
                        g.Data4[3], g.Data4[4], g.Data4[5], g.Data4[6], g.Data4[7]);
        }

        // GLFW's XInput test (win32_joystick.c supportsXInput): a raw-input HID
        // device with this vid/pid whose name holds "IG_". Reads the device
        // list only.
        bool supportsXInput(const GUID &product)
        {
            UINT count = 0;
            if (GetRawInputDeviceList(nullptr, &count, sizeof(RAWINPUTDEVICELIST)) != 0 || count == 0)
                return false;
            std::vector<RAWINPUTDEVICELIST> list(count);
            if (GetRawInputDeviceList(list.data(), &count, sizeof(RAWINPUTDEVICELIST)) == static_cast<UINT>(-1))
                return false;
            for (UINT i = 0; i < count; ++i)
            {
                if (list[i].dwType != RIM_TYPEHID)
                    continue;
                RID_DEVICE_INFO rdi{};
                rdi.cbSize = sizeof(rdi);
                UINT size = sizeof(rdi);
                if (static_cast<INT>(GetRawInputDeviceInfoA(list[i].hDevice, RIDI_DEVICEINFO, &rdi, &size)) == -1)
                    continue;
                if (static_cast<LONG>(MAKELONG(rdi.hid.dwVendorId, rdi.hid.dwProductId)) != static_cast<LONG>(product.Data1))
                    continue;
                char name[256] = {};
                size = sizeof(name);
                if (static_cast<INT>(GetRawInputDeviceInfoA(list[i].hDevice, RIDI_DEVICENAME, name, &size)) == -1)
                    break;
                name[sizeof(name) - 1] = '\0';
                if (std::strstr(name, "IG_"))
                    return true;
            }
            return false;
        }

        struct Opened
        {
            IDirectInputDevice8W *device = nullptr;
            bool acquired = false;
        };
    }

    struct Devices::Impl
    {
        HWND window = nullptr;
        IDirectInput8W *di = nullptr;
        std::map<std::string, Opened> opened; // by instance GUID text
        // Force-feedback test
        IDirectInputDevice8W *ffbDevice = nullptr;
        IDirectInputEffect *ffbEffect = nullptr;
        std::string ffbInstance;
        FfbTest ffbKind = FfbTest::CentreSpring;
        FfbTestParams ffbParams;
        ULONGLONG ffbStart = 0;
        int ffbPhase = 0;

        bool ensureDi()
        {
            if (di)
                return true;
            const HRESULT hr = DirectInput8Create(GetModuleHandleW(nullptr), DIRECTINPUT_VERSION, IID_IDirectInput8W,
                                                  reinterpret_cast<void **>(&di), nullptr);
            if (FAILED(hr))
                di = nullptr;
            return di != nullptr;
        }

        void closeAll()
        {
            for (auto &[id, o] : opened)
                if (o.device)
                {
                    o.device->Unacquire();
                    o.device->Release();
                }
            opened.clear();
        }

        void stopFfb()
        {
            if (ffbEffect)
            {
                ffbEffect->Stop();
                ffbEffect->Unload();
                ffbEffect->Release();
                ffbEffect = nullptr;
            }
            if (ffbDevice)
            {
                ffbDevice->SendForceFeedbackCommand(DISFFC_STOPALL);
                ffbDevice->Unacquire();
                ffbDevice->Release();
                ffbDevice = nullptr;
            }
            ffbInstance.clear();
            ffbPhase = 0;
        }

        ~Impl()
        {
            stopFfb();
            closeAll();
            if (di)
                di->Release();
        }
    };

    namespace
    {
        struct EnumContext
        {
            std::vector<DeviceInfo> *list;
            bool forceFeedbackPass;
        };

        BOOL CALLBACK enumDevice(const DIDEVICEINSTANCEW *inst, void *user)
        {
            auto *ctx = static_cast<EnumContext *>(user);
            const std::string id = guidText(inst->guidInstance);
            if (ctx->forceFeedbackPass)
            {
                for (auto &d : *ctx->list)
                    if (d.instance == id)
                        d.forceFeedback = true;
                return DIENUM_CONTINUE;
            }
            DeviceInfo d;
            d.instance = id;
            d.name = utf8(inst->tszInstanceName);
            if (d.name.empty())
                d.name = utf8(inst->tszProductName);
            // guidProduct.Data1 = MAKELONG(vid, pid) for HID devices ("PIDVID").
            const bool pidvid = std::memcmp(&inst->guidProduct.Data4[2], "PIDVID", 6) == 0;
            if (pidvid)
            {
                d.vid = static_cast<uint16_t>(inst->guidProduct.Data1 & 0xffff);
                d.pid = static_cast<uint16_t>(inst->guidProduct.Data1 >> 16);
            }
            d.diType = inst->dwDevType & 0xff;
            d.xinput = supportsXInput(inst->guidProduct);
            d.glfwGuid = d.xinput ? std::string("78696e70757401000000000000000000") : pidvid ? sdlGuidFromVidPid(d.vid, d.pid) : std::string();
            ctx->list->push_back(d);
            return DIENUM_CONTINUE;
        }

        // DIJOYSTATE2 axes in wheel-adapter order: x y z rx ry rz slider0 slider1.
        const DWORD kAxisOffsets[8] = {DIJOFS_X, DIJOFS_Y, DIJOFS_Z, DIJOFS_RX, DIJOFS_RY, DIJOFS_RZ, DIJOFS_SLIDER(0), DIJOFS_SLIDER(1)};
    }

    Devices::Devices() : impl_(std::make_unique<Impl>()) {}
    Devices::~Devices() { shutdown(); }

    void Devices::init(void *window, bool allowOpen, bool demo)
    {
        impl_->stopFfb();
        impl_->closeAll();
        impl_->window = static_cast<HWND>(window);
        allowOpen_ = allowOpen;
        demo_ = demo;
        lastEnum_ = -100.0;
        list_.clear();
    }

    void Devices::shutdown()
    {
        if (impl_)
        {
            impl_->stopFfb();
            impl_->closeAll();
        }
    }

    void Devices::setPaused(bool paused)
    {
        if (paused == paused_)
            return;
        paused_ = paused;
        if (paused)
        {
            impl_->stopFfb();
            impl_->closeAll();
            for (auto &d : list_)
                d.live = d.demo;
        }
    }

    void Devices::update(double now)
    {
        if (demo_)
        {
            if (list_.empty())
            {
                list_ = demoDevices();
                status_ = "Demo devices (test mode)";
            }
            return;
        }
        if (now - lastEnum_ >= 2.0)
        {
            lastEnum_ = now;
            std::vector<DeviceInfo> fresh;
            if (impl_->ensureDi())
            {
                EnumContext all{&fresh, false};
                impl_->di->EnumDevices(DI8DEVCLASS_GAMECTRL, enumDevice, &all, DIEDFL_ATTACHEDONLY);
                EnumContext ff{&fresh, true};
                impl_->di->EnumDevices(DI8DEVCLASS_GAMECTRL, enumDevice, &ff, DIEDFL_ATTACHEDONLY | DIEDFL_FORCEFEEDBACK);
                for (auto &d : fresh)
                {
                    d.kind = d.xinput ? DeviceKind::Gamepad : kindFromDiType(d.diType, d.name, d.forceFeedback, d.vid);
                    for (const auto &old : list_)
                        if (old.instance == d.instance)
                        {
                            d.live = old.live;
                            d.axes = old.axes;
                            d.axisMask = old.axisMask;
                            d.buttons = old.buttons;
                            d.buttonCount = old.buttonCount;
                            d.pov = old.pov;
                        }
                }
                status_ = fresh.empty() ? std::string("none found") : strf("%zu found", fresh.size());
            }
            else
                status_ = "Windows' controller list is not available";
            // Devices that went away are let go.
            for (auto it = impl_->opened.begin(); it != impl_->opened.end();)
            {
                const bool present = std::any_of(fresh.begin(), fresh.end(), [&](const DeviceInfo &d) { return d.instance == it->first; });
                if (!present)
                {
                    if (it->second.device)
                    {
                        it->second.device->Unacquire();
                        it->second.device->Release();
                    }
                    it = impl_->opened.erase(it);
                }
                else
                    ++it;
            }
            list_ = std::move(fresh);
        }
        if (!allowOpen_ || paused_ || !impl_->di)
            return;
        // Passive reading: background, non-exclusive (no force feedback possible).
        for (auto &d : list_)
        {
            if (impl_->ffbDevice && d.instance == impl_->ffbInstance)
                continue; // held exclusively by the force-feedback test
            Opened &o = impl_->opened[d.instance];
            if (!o.device)
            {
                GUID guid{};
                std::wstring w = wide(d.instance);
                if (FAILED(CLSIDFromString(w.c_str(), &guid)))
                    continue;
                if (FAILED(impl_->di->CreateDevice(guid, &o.device, nullptr)) || !o.device)
                {
                    o.device = nullptr;
                    continue;
                }
                ++openedTotal_;
                o.device->SetDataFormat(&c_dfDIJoystick2);
                o.device->SetCooperativeLevel(impl_->window, DISCL_BACKGROUND | DISCL_NONEXCLUSIVE);
                d.axisMask = 0;
                for (int a = 0; a < 8; ++a)
                {
                    DIPROPRANGE range{};
                    range.diph.dwSize = sizeof(range);
                    range.diph.dwHeaderSize = sizeof(DIPROPHEADER);
                    range.diph.dwHow = DIPH_BYOFFSET;
                    range.diph.dwObj = kAxisOffsets[a];
                    range.lMin = -32768;
                    range.lMax = 32767;
                    if (SUCCEEDED(o.device->SetProperty(DIPROP_RANGE, &range.diph)))
                        d.axisMask |= 1u << a;
                }
                DIDEVCAPS caps{};
                caps.dwSize = sizeof(caps);
                if (SUCCEEDED(o.device->GetCapabilities(&caps)))
                    d.buttonCount = static_cast<int>(std::min<DWORD>(caps.dwButtons, 128));
            }
            if (!o.acquired)
                o.acquired = SUCCEEDED(o.device->Acquire());
            if (!o.acquired)
                continue;
            o.device->Poll();
            DIJOYSTATE2 st{};
            const HRESULT hr = o.device->GetDeviceState(sizeof(st), &st);
            if (hr == DIERR_INPUTLOST || hr == DIERR_NOTACQUIRED)
            {
                o.acquired = false;
                d.live = false;
                continue;
            }
            if (FAILED(hr))
                continue;
            const LONG *axes[8] = {&st.lX, &st.lY, &st.lZ, &st.lRx, &st.lRy, &st.lRz, &st.rglSlider[0], &st.rglSlider[1]};
            for (int a = 0; a < 8; ++a)
                d.axes[static_cast<size_t>(a)] = static_cast<int32_t>(*axes[a]);
            for (int b = 0; b < 128; ++b)
                d.buttons[static_cast<size_t>(b)] = (st.rgbButtons[b] & 0x80) ? 1 : 0;
            d.pov = LOWORD(st.rgdwPOV[0]) == 0xffff ? -1 : static_cast<int>(st.rgdwPOV[0]);
            d.live = true;
        }
        // The force-feedback test's second half, and its end.
        if (impl_->ffbEffect)
        {
            const ULONGLONG ms = GetTickCount64() - impl_->ffbStart;
            if (impl_->ffbKind == FfbTest::LeftRight && impl_->ffbPhase == 1 && ms >= impl_->ffbParams.durationMs)
            {
                DICONSTANTFORCE cf{-impl_->ffbParams.magnitude};
                DIEFFECT eff{};
                eff.dwSize = sizeof(eff);
                eff.cbTypeSpecificParams = sizeof(cf);
                eff.lpvTypeSpecificParams = &cf;
                impl_->ffbEffect->SetParameters(&eff, DIEP_TYPESPECIFICPARAMS | DIEP_START);
                impl_->ffbPhase = 2;
            }
            const ULONGLONG total = impl_->ffbParams.durationMs * (impl_->ffbKind == FfbTest::LeftRight ? 2u : 1u) + 100u;
            if (ms >= total || GetForegroundWindow() != impl_->window)
                impl_->stopFfb();
        }
    }

    std::vector<GamepadInfo> Devices::glfwGamepads() const
    {
        std::vector<GamepadInfo> out;
#if defined(PS2X_INPUT_GLFW)
        // Never in the hidden test mode: GLFW would set its joysticks up.
        if (!allowOpen_ || demo_)
            return out;
        for (int j = GLFW_JOYSTICK_1; j <= GLFW_JOYSTICK_LAST; ++j)
        {
            if (!glfwJoystickPresent(j))
                continue;
            GamepadInfo g;
            g.joystick = j;
            const char *name = glfwGetJoystickName(j);
            const char *guid = glfwGetJoystickGUID(j);
            g.name = name ? name : "?";
            g.guid = guid ? guid : "";
            g.mapped = glfwJoystickIsGamepad(j) == GLFW_TRUE;
            out.push_back(g);
        }
#endif
        return out;
    }

    bool Devices::startFfbTest(const DeviceInfo &wheel, FfbTest test, float strength, bool aboveCap, std::string &message)
    {
        stopFfbTest();
        if (!allowOpen_ || demo_ || wheel.demo)
        {
            message = "The force-feedback test runs only in a normal (visible) session with a wheel plugged in.";
            return false;
        }
        if (!wheel.forceFeedback)
        {
            message = "This device has no force feedback.";
            return false;
        }
        if (!impl_->ensureDi() || GetForegroundWindow() != impl_->window)
        {
            message = "The launcher window must be in front for the test.";
            return false;
        }
        GUID guid{};
        if (FAILED(CLSIDFromString(wide(wheel.instance).c_str(), &guid)))
        {
            message = "Bad device id.";
            return false;
        }
        // The passive reader of this device lets go first (exclusive access below).
        if (auto it = impl_->opened.find(wheel.instance); it != impl_->opened.end())
        {
            if (it->second.device)
            {
                it->second.device->Unacquire();
                it->second.device->Release();
            }
            impl_->opened.erase(it);
        }
        IDirectInputDevice8W *dev = nullptr;
        if (FAILED(impl_->di->CreateDevice(guid, &dev, nullptr)) || !dev)
        {
            message = "The wheel could not be opened.";
            return false;
        }
        ++openedTotal_;
        dev->SetDataFormat(&c_dfDIJoystick2);
        // The driver's own centring spring (DIPROP_AUTOCENTER) plays at the base's strength,
        // not the test's: switched off before and after acquiring (some drivers take it only
        // while unacquired, others only after an exclusive acquire), as the wheel adapter does.
        DIPROPDWORD autoCentre{};
        autoCentre.diph.dwSize = sizeof(autoCentre);
        autoCentre.diph.dwHeaderSize = sizeof(DIPROPHEADER);
        autoCentre.diph.dwObj = 0;
        autoCentre.diph.dwHow = DIPH_DEVICE;
        autoCentre.dwData = DIPROPAUTOCENTER_OFF;
        if (FAILED(dev->SetCooperativeLevel(impl_->window, DISCL_EXCLUSIVE | DISCL_FOREGROUND)))
        {
            dev->Release();
            message = "Another program holds the wheel (FanaLab, a game?): close it and try again.";
            return false;
        }
        dev->SetProperty(DIPROP_AUTOCENTER, &autoCentre.diph);
        if (FAILED(dev->Acquire()))
        {
            dev->Release();
            message = "Another program holds the wheel (FanaLab, a game?): close it and try again.";
            return false;
        }
        dev->SendForceFeedbackCommand(DISFFC_STOPALL);
        if (dev->SetProperty(DIPROP_AUTOCENTER, &autoCentre.diph) == DIERR_ACQUIRED)
        {
            dev->Unacquire();
            dev->SetProperty(DIPROP_AUTOCENTER, &autoCentre.diph);
            if (FAILED(dev->Acquire()))
            {
                dev->Release();
                message = "The wheel could not be opened for the test.";
                return false;
            }
        }
        const float played = ffbTestStrength(strength, aboveCap);
        const FfbTestParams p = ffbTestParams(test, strength, aboveCap);
        DWORD axes[1] = {DIJOFS_X};
        LONG direction[1] = {0};
        DIENVELOPE env{};
        env.dwSize = sizeof(env);
        env.dwAttackLevel = 0;
        env.dwAttackTime = p.attackMs * 1000u;
        env.dwFadeLevel = 0;
        env.dwFadeTime = p.fadeMs * 1000u;
        DIEFFECT eff{};
        eff.dwSize = sizeof(eff);
        eff.dwFlags = DIEFF_CARTESIAN | DIEFF_OBJECTOFFSETS;
        eff.dwDuration = p.durationMs * 1000u;
        eff.dwSamplePeriod = 0;
        eff.dwGain = DI_FFNOMINALMAX;
        eff.dwTriggerButton = DIEB_NOTRIGGER;
        eff.cAxes = 1;
        eff.rgdwAxes = axes;
        eff.rglDirection = direction;
        DICONSTANTFORCE cf{p.magnitude};
        DICONDITION cond{};
        cond.lOffset = 0;
        cond.lPositiveCoefficient = p.coefficient;
        cond.lNegativeCoefficient = p.coefficient;
        cond.dwPositiveSaturation = p.saturation;
        cond.dwNegativeSaturation = p.saturation;
        cond.lDeadBand = 0;
        if (test == FfbTest::LeftRight)
        {
            eff.lpEnvelope = &env;
            eff.cbTypeSpecificParams = sizeof(cf);
            eff.lpvTypeSpecificParams = &cf;
        }
        else
        {
            eff.lpEnvelope = nullptr; // conditions take no envelope
            eff.cbTypeSpecificParams = sizeof(cond);
            eff.lpvTypeSpecificParams = &cond;
        }
        IDirectInputEffect *effect = nullptr;
        const GUID &kind = test == FfbTest::LeftRight ? GUID_ConstantForce : GUID_Spring;
        if (FAILED(dev->CreateEffect(kind, &eff, &effect, nullptr)) || !effect)
        {
            dev->Unacquire();
            dev->Release();
            message = "The wheel refused the test effect.";
            return false;
        }
        impl_->ffbDevice = dev;
        impl_->ffbEffect = effect;
        impl_->ffbInstance = wheel.instance;
        impl_->ffbKind = test;
        impl_->ffbParams = p;
        impl_->ffbStart = GetTickCount64();
        impl_->ffbPhase = 1;
        effect->Start(1, 0);
        const int shown = static_cast<int>(std::lround(played * 100.0f));
        message = test == FfbTest::LeftRight ? strf("Push test at %d%% (half strength): one way, then the other.", shown)
                                             : strf("Centring spring at %d%% for 2.5 s.", shown);
        if (strength > played + 0.001f)
            message += " The tests stop at 25%.";
        return true;
    }

    void Devices::stopFfbTest() { impl_->stopFfb(); }
    bool Devices::ffbTestRunning() const { return impl_->ffbEffect != nullptr; }
#else
    struct Devices::Impl
    {
    };
    Devices::Devices() : impl_(std::make_unique<Impl>()) {}
    Devices::~Devices() = default;
    void Devices::init(void *, bool allowOpen, bool demo)
    {
        allowOpen_ = allowOpen;
        demo_ = demo;
    }
    void Devices::shutdown() {}
    void Devices::setPaused(bool paused) { paused_ = paused; }
    void Devices::update(double)
    {
        if (demo_ && list_.empty())
            list_ = demoDevices();
    }
    std::vector<GamepadInfo> Devices::glfwGamepads() const { return {}; }
    bool Devices::startFfbTest(const DeviceInfo &, FfbTest, float, bool, std::string &message)
    {
        message = "Windows only.";
        return false;
    }
    void Devices::stopFfbTest() {}
    bool Devices::ffbTestRunning() const { return false; }
#endif
}
