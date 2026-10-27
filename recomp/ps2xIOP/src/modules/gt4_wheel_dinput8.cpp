#include "gt4_wheel_di.h"

// Real DirectInput 8 backend for the GT4 host wheel (gt4_wheel_di.h).
// dinput8.dll is loaded at run time from System32 (no import library, no
// build change); the GUIDs come from dinput.h through initguid.h (selectany
// definitions, no dxguid.lib) and the joystick data format is built here
// (DIJOYSTATE2's axes, POVs and 128 buttons) instead of dinput8.lib's
// c_dfDIJoystick2. Nothing here runs until the runtime registers the adapter
// for visible play (PS2X_WHEEL=dinput); unit tests use a fake backend.

#if defined(_WIN32)

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef DIRECTINPUT_VERSION
#define DIRECTINPUT_VERSION 0x0800
#endif
#include <windows.h>
#include <initguid.h>
#include <dinput.h>

// The "object may be missing" flag of DirectInput's own joystick formats (dinputd.h; not in
// the public dinput.h): a wheel without, say, an Rz axis still accepts the format.
#ifndef DIDFT_OPTIONAL
#define DIDFT_OPTIONAL 0x80000000
#endif

#include <cstddef>
#include <cstdio>
#include <map>
#include <mutex>

namespace ps2x::iop::gt4
{
    namespace
    {
        using DirectInput8CreateFn = HRESULT(WINAPI *)(HINSTANCE, DWORD, REFIID, LPVOID *, LPUNKNOWN);

        struct HostJoyState
        {
            LONG axis[8];       // X Y Z Rx Ry Rz Slider0 Slider1 (offset 4 * DiAxis)
            DWORD pov[4];
            BYTE buttons[128];
        };
        static_assert(sizeof(HostJoyState) % 4 == 0, "DirectInput needs a DWORD multiple");
        static_assert(offsetof(HostJoyState, axis) == 0, "X must sit at DIJOFS_X for the effect axis offsets");

        const DIDATAFORMAT &joyFormat()
        {
            static DIOBJECTDATAFORMAT objects[8 + 4 + 128];
            static DIDATAFORMAT format{};
            static std::once_flag once;
            std::call_once(once, [] {
                const GUID *axisGuids[8] = {&GUID_XAxis, &GUID_YAxis, &GUID_ZAxis, &GUID_RxAxis,
                                            &GUID_RyAxis, &GUID_RzAxis, &GUID_Slider, &GUID_Slider};
                DWORD n = 0;
                for (DWORD i = 0; i < 8; ++i)
                    objects[n++] = {axisGuids[i], static_cast<DWORD>(offsetof(HostJoyState, axis) + 4 * i),
                                    DIDFT_AXIS | DIDFT_OPTIONAL | DIDFT_ANYINSTANCE, DIDOI_ASPECTPOSITION};
                for (DWORD i = 0; i < 4; ++i)
                    objects[n++] = {&GUID_POV, static_cast<DWORD>(offsetof(HostJoyState, pov) + 4 * i),
                                    DIDFT_POV | DIDFT_OPTIONAL | DIDFT_ANYINSTANCE, 0};
                for (DWORD i = 0; i < 128; ++i)
                    objects[n++] = {nullptr, static_cast<DWORD>(offsetof(HostJoyState, buttons) + i),
                                    DIDFT_BUTTON | DIDFT_OPTIONAL | DIDFT_ANYINSTANCE, 0};
                format = {sizeof(DIDATAFORMAT), sizeof(DIOBJECTDATAFORMAT), DIDF_ABSAXIS, sizeof(HostJoyState), n, objects};
            });
            return format;
        }

        std::string guidText(const GUID &g)
        {
            char text[40];
            std::snprintf(text, sizeof(text), "{%08lX-%04hX-%04hX-%02X%02X-%02X%02X%02X%02X%02X%02X}", g.Data1, g.Data2,
                          g.Data3, g.Data4[0], g.Data4[1], g.Data4[2], g.Data4[3], g.Data4[4], g.Data4[5], g.Data4[6],
                          g.Data4[7]);
            return text;
        }

        std::string utf8(const wchar_t *text)
        {
            if (!text || !*text)
                return {};
            const int bytes = WideCharToMultiByte(CP_UTF8, 0, text, -1, nullptr, 0, nullptr, nullptr);
            if (bytes <= 1)
                return {};
            std::string out(static_cast<size_t>(bytes - 1), '\0');
            WideCharToMultiByte(CP_UTF8, 0, text, -1, out.data(), bytes, nullptr, nullptr);
            return out;
        }

        std::string hresultText(HRESULT hr)
        {
            char text[24];
            std::snprintf(text, sizeof(text), "HRESULT 0x%08lx", static_cast<unsigned long>(hr));
            return text;
        }

        DiStatus statusOf(HRESULT hr)
        {
            if (SUCCEEDED(hr))
                return DiStatus::Ok; // includes DI_NOEFFECT / S_FALSE (already acquired, nothing to poll)
            switch (hr)
            {
            case static_cast<HRESULT>(DIERR_NOTACQUIRED): return DiStatus::NotAcquired;
            case static_cast<HRESULT>(DIERR_INPUTLOST): return DiStatus::InputLost;
            case static_cast<HRESULT>(DIERR_UNPLUGGED): return DiStatus::Gone;
            default: return DiStatus::Failed;
            }
        }

        // One-axis effect, cartesian with direction 0 and object offsets: the sign of the
        // magnitude / coefficients is the direction along the axis.
        struct EffectBlock
        {
            DIEFFECT effect{};
            DWORD axis = 0;
            LONG direction = 0;
            DICONSTANTFORCE constant{};
            DICONDITION condition{};

            explicit EffectBlock(const DiEffect &e)
            {
                axis = e.axisOffset;
                effect.dwSize = sizeof(DIEFFECT);
                effect.dwFlags = DIEFF_CARTESIAN | DIEFF_OBJECTOFFSETS;
                effect.dwDuration = e.durationUs; // INFINITE = 0xffffffff
                effect.dwSamplePeriod = 0;
                effect.dwGain = DI_FFNOMINALMAX;
                effect.dwTriggerButton = DIEB_NOTRIGGER;
                effect.dwTriggerRepeatInterval = 0;
                effect.cAxes = 1;
                effect.rgdwAxes = &axis;
                effect.rglDirection = &direction;
                effect.lpEnvelope = nullptr;
                effect.dwStartDelay = e.startDelayUs;
                if (e.kind == DiEffectKind::Constant)
                {
                    constant.lMagnitude = e.magnitude;
                    effect.cbTypeSpecificParams = sizeof(DICONSTANTFORCE);
                    effect.lpvTypeSpecificParams = &constant;
                }
                else
                {
                    condition.lOffset = e.condition.offset;
                    condition.lPositiveCoefficient = e.condition.positiveCoefficient;
                    condition.lNegativeCoefficient = e.condition.negativeCoefficient;
                    condition.dwPositiveSaturation = e.condition.positiveSaturation;
                    condition.dwNegativeSaturation = e.condition.negativeSaturation;
                    condition.lDeadBand = e.condition.deadBand;
                    effect.cbTypeSpecificParams = sizeof(DICONDITION);
                    effect.lpvTypeSpecificParams = &condition;
                }
            }
        };

        const GUID &effectGuid(DiEffectKind kind)
        {
            switch (kind)
            {
            case DiEffectKind::Constant: return GUID_ConstantForce;
            case DiEffectKind::Spring: return GUID_Spring;
            case DiEffectKind::Damper: return GUID_Damper;
            case DiEffectKind::Friction: return GUID_Friction;
            }
            return GUID_ConstantForce;
        }

        BOOL CALLBACK collectAxis(LPCDIDEVICEOBJECTINSTANCEW object, LPVOID ref)
        {
            auto *axes = static_cast<uint8_t *>(ref);
            const GUID &g = object->guidType;
            if (g == GUID_XAxis)
                *axes |= 1u << 0;
            else if (g == GUID_YAxis)
                *axes |= 1u << 1;
            else if (g == GUID_ZAxis)
                *axes |= 1u << 2;
            else if (g == GUID_RxAxis)
                *axes |= 1u << 3;
            else if (g == GUID_RyAxis)
                *axes |= 1u << 4;
            else if (g == GUID_RzAxis)
                *axes |= 1u << 5;
            else if (g == GUID_Slider)
                *axes |= (*axes & (1u << 6)) ? (1u << 7) : (1u << 6);
            return DIENUM_CONTINUE;
        }

        struct EnumContext
        {
            std::vector<DiDeviceInfo> *list;
            std::map<std::string, GUID> *guids;
            bool forceFeedbackPass; // second pass: DIEDFL_FORCEFEEDBACK marks the force-feedback devices
        };

        BOOL CALLBACK enumDevice(LPCDIDEVICEINSTANCEW instance, LPVOID ref)
        {
            auto *ctx = static_cast<EnumContext *>(ref);
            const std::string id = guidText(instance->guidInstance);
            if (ctx->forceFeedbackPass)
            {
                for (auto &d : *ctx->list)
                    if (d.instance == id)
                        d.forceFeedback = true;
                return DIENUM_CONTINUE;
            }
            DiDeviceInfo d;
            d.instance = id;
            d.name = utf8(instance->tszProductName);
            d.vendor = LOWORD(instance->guidProduct.Data1);  // guidProduct.Data1 = MAKELONG(VID, PID)
            d.product = HIWORD(instance->guidProduct.Data1);
            d.type = LOBYTE(instance->dwDevType);
            d.subType = HIBYTE(LOWORD(instance->dwDevType));
            ctx->list->push_back(d);
            (*ctx->guids)[id] = instance->guidInstance;
            return DIENUM_CONTINUE;
        }

        class Dinput8Device final : public DiDevice
        {
        public:
            Dinput8Device(IDirectInputDevice8W *device, DiDeviceInfo info, HWND window)
                : device_(device), info_(std::move(info)), window_(window)
            {
            }

            ~Dinput8Device() override
            {
                for (IDirectInputEffect *e : effects_)
                    if (e)
                    {
                        e->Stop();
                        e->Unload();
                        e->Release();
                    }
                device_->Unacquire();
                device_->Release();
            }

            const DiDeviceInfo &info() const override { return info_; }

            DiStatus acquire(bool exclusive) override
            {
                const int level = exclusive ? 1 : 0;
                if (level != level_)
                {
                    device_->Unacquire();
                    const HRESULT hr = device_->SetCooperativeLevel(
                        window_, (exclusive ? DISCL_EXCLUSIVE : DISCL_NONEXCLUSIVE) | DISCL_BACKGROUND);
                    if (FAILED(hr))
                        return DiStatus::Failed;
                    level_ = level;
                }
                return statusOf(device_->Acquire());
            }

            void unacquire() override { device_->Unacquire(); }

            DiStatus poll(DiState &state) override
            {
                HRESULT hr = device_->Poll();
                if (FAILED(hr))
                    return statusOf(hr);
                HostJoyState js{};
                hr = device_->GetDeviceState(sizeof(js), &js);
                if (FAILED(hr))
                    return statusOf(hr);
                for (unsigned i = 0; i < 8; ++i)
                    state.axis[i] = js.axis[i];
                for (unsigned i = 0; i < 4; ++i)
                    state.pov[i] = js.pov[i];
                for (unsigned i = 0; i < 128; ++i)
                    state.buttons[i] = js.buttons[i];
                return DiStatus::Ok;
            }

            DiStatus setAutoCentre(bool on) override
            {
                DIPROPDWORD p{};
                p.diph.dwSize = sizeof(DIPROPDWORD);
                p.diph.dwHeaderSize = sizeof(DIPROPHEADER);
                p.diph.dwObj = 0;
                p.diph.dwHow = DIPH_DEVICE;
                p.dwData = on ? DIPROPAUTOCENTER_ON : DIPROPAUTOCENTER_OFF;
                HRESULT hr = device_->SetProperty(DIPROP_AUTOCENTER, &p.diph);
                if (hr == DIERR_ACQUIRED)
                {
                    // Some drivers take it only while unacquired; re-acquiring stops running effects.
                    device_->Unacquire();
                    hr = device_->SetProperty(DIPROP_AUTOCENTER, &p.diph);
                    device_->Acquire();
                    return SUCCEEDED(hr) ? DiStatus::NotAcquired : DiStatus::Failed;
                }
                return SUCCEEDED(hr) ? DiStatus::Ok : DiStatus::Failed;
            }

            bool setDeviceGain(uint32_t gain) override
            {
                DIPROPDWORD p{};
                p.diph.dwSize = sizeof(DIPROPDWORD);
                p.diph.dwHeaderSize = sizeof(DIPROPHEADER);
                p.diph.dwObj = 0;
                p.diph.dwHow = DIPH_DEVICE;
                p.dwData = gain > DI_FFNOMINALMAX ? DI_FFNOMINALMAX : gain;
                return SUCCEEDED(device_->SetProperty(DIPROP_FFGAIN, &p.diph));
            }

            bool stopAll() override { return SUCCEEDED(device_->SendForceFeedbackCommand(DISFFC_STOPALL)); }

            int createEffect(const DiEffect &e) override
            {
                EffectBlock block(e);
                IDirectInputEffect *effect = nullptr;
                if (FAILED(device_->CreateEffect(effectGuid(e.kind), &block.effect, &effect, nullptr)) || !effect)
                    return -1;
                for (size_t i = 0; i < effects_.size(); ++i)
                    if (!effects_[i])
                    {
                        effects_[i] = effect;
                        return static_cast<int>(i);
                    }
                effects_.push_back(effect);
                return static_cast<int>(effects_.size() - 1);
            }

            bool updateEffect(int handle, const DiEffect &e) override
            {
                IDirectInputEffect *effect = at(handle);
                if (!effect)
                    return false;
                EffectBlock block(e);
                // Type-specific parameters only: duration and start delay are set at creation (GT4 sends
                // -1 and 0 and never changes them), so a per-frame update touches nothing else.
                return SUCCEEDED(effect->SetParameters(&block.effect, DIEP_TYPESPECIFICPARAMS));
            }

            bool startEffect(int handle) override
            {
                IDirectInputEffect *effect = at(handle);
                return effect && SUCCEEDED(effect->Start(1, 0));
            }

            bool stopEffect(int handle) override
            {
                IDirectInputEffect *effect = at(handle);
                return effect && SUCCEEDED(effect->Stop());
            }

            void releaseEffect(int handle) override
            {
                IDirectInputEffect *effect = at(handle);
                if (!effect)
                    return;
                effect->Stop();
                effect->Unload();
                effect->Release();
                effects_[static_cast<size_t>(handle)] = nullptr;
            }

            void emergencyStop() noexcept override { device_->SendForceFeedbackCommand(DISFFC_STOPALL); }

        private:
            IDirectInputEffect *at(int handle) const
            {
                return handle >= 0 && static_cast<size_t>(handle) < effects_.size() ? effects_[static_cast<size_t>(handle)] : nullptr;
            }

            IDirectInputDevice8W *device_;
            DiDeviceInfo info_;
            HWND window_;
            int level_ = -1;
            std::vector<IDirectInputEffect *> effects_;
        };

        class Dinput8Backend final : public DiBackend
        {
        public:
            ~Dinput8Backend() override
            {
                if (input_)
                    input_->Release(); // the DLL itself stays loaded (other users in the process)
            }

            bool ready(std::string &error) override
            {
                if (input_)
                    return true;
                HMODULE dll = LoadLibraryExW(L"dinput8.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
                if (!dll)
                {
                    error = "dinput8.dll not found";
                    return false;
                }
                auto create = reinterpret_cast<DirectInput8CreateFn>(GetProcAddress(dll, "DirectInput8Create"));
                if (!create)
                {
                    error = "dinput8.dll has no DirectInput8Create";
                    return false;
                }
                void *out = nullptr;
                const HRESULT hr = create(GetModuleHandleW(nullptr), DIRECTINPUT_VERSION, IID_IDirectInput8W, &out, nullptr);
                if (FAILED(hr) || !out)
                {
                    error = "DirectInput8Create: " + hresultText(hr);
                    return false;
                }
                input_ = static_cast<IDirectInput8W *>(out);
                return true;
            }

            std::vector<DiDeviceInfo> enumerate() override
            {
                std::vector<DiDeviceInfo> list;
                if (!input_)
                    return list;
                std::lock_guard lock(mutex_);
                EnumContext all{&list, &guids_, false};
                input_->EnumDevices(DI8DEVCLASS_GAMECTRL, enumDevice, &all, DIEDFL_ATTACHEDONLY);
                EnumContext ff{&list, &guids_, true};
                input_->EnumDevices(DI8DEVCLASS_GAMECTRL, enumDevice, &ff, DIEDFL_ATTACHEDONLY | DIEDFL_FORCEFEEDBACK);
                return list;
            }

            std::unique_ptr<DiDevice> open(const DiDeviceInfo &info, void *window, std::string &error) override
            {
                if (!input_)
                {
                    error = "DirectInput not ready";
                    return nullptr;
                }
                if (!window)
                {
                    error = "no window";
                    return nullptr;
                }
                GUID guid{};
                {
                    std::lock_guard lock(mutex_);
                    const auto it = guids_.find(info.instance);
                    if (it == guids_.end())
                    {
                        error = "not enumerated";
                        return nullptr;
                    }
                    guid = it->second;
                }
                IDirectInputDevice8W *device = nullptr;
                HRESULT hr = input_->CreateDevice(guid, &device, nullptr);
                if (FAILED(hr) || !device)
                {
                    error = "CreateDevice: " + hresultText(hr);
                    return nullptr;
                }
                hr = device->SetDataFormat(&joyFormat());
                if (FAILED(hr))
                {
                    device->Release();
                    error = "SetDataFormat: " + hresultText(hr);
                    return nullptr;
                }
                DiDeviceInfo opened = info;
                device->EnumObjects(collectAxis, &opened.axes, DIDFT_AXIS);
                // Every axis -32768..32767, no driver dead zone (the adapter maps the ranges itself).
                DIPROPRANGE range{};
                range.diph.dwSize = sizeof(DIPROPRANGE);
                range.diph.dwHeaderSize = sizeof(DIPROPHEADER);
                range.diph.dwObj = 0;
                range.diph.dwHow = DIPH_DEVICE;
                range.lMin = -32768;
                range.lMax = 32767;
                device->SetProperty(DIPROP_RANGE, &range.diph);
                DIPROPDWORD deadZone{};
                deadZone.diph.dwSize = sizeof(DIPROPDWORD);
                deadZone.diph.dwHeaderSize = sizeof(DIPROPHEADER);
                deadZone.diph.dwObj = 0;
                deadZone.diph.dwHow = DIPH_DEVICE;
                deadZone.dwData = 0;
                device->SetProperty(DIPROP_DEADZONE, &deadZone.diph);
                return std::make_unique<Dinput8Device>(device, std::move(opened), static_cast<HWND>(window));
            }

            bool windowFocused(void *window) const override
            {
                return window && GetForegroundWindow() == static_cast<HWND>(window);
            }

        private:
            IDirectInput8W *input_ = nullptr;
            std::mutex mutex_;
            std::map<std::string, GUID> guids_;
        };
    }

    std::shared_ptr<DiBackend> createDirectInput8Backend() { return std::make_shared<Dinput8Backend>(); }
}

#endif // _WIN32
