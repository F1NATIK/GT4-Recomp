#include "runtime/ps2_pad_rumble.h"

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <Xinput.h>
#include <initializer_list>

namespace
{
    // XInput is loaded at run time (xinput1_4 on Windows 8+, else 9.1.0) so
    // the runner starts without it; rumble is then simply unavailable.
    struct XInputApi
    {
        decltype(&XInputGetState) getState = nullptr;
        decltype(&XInputSetState) setState = nullptr;
        XInputApi()
        {
            for (const char *name : {"xinput1_4.dll", "xinput9_1_0.dll", "xinput1_3.dll"})
                if (HMODULE module = LoadLibraryA(name))
                {
                    getState = reinterpret_cast<decltype(getState)>(GetProcAddress(module, "XInputGetState"));
                    setState = reinterpret_cast<decltype(setState)>(GetProcAddress(module, "XInputSetState"));
                    if (getState && setState) return;
                    getState = nullptr; setState = nullptr;
                }
        }
    };
    const XInputApi &api()
    {
        static const XInputApi instance;
        return instance;
    }
}

namespace ps2_pad
{
    int firstXInputPad()
    {
        if (!api().getState) return -1;
        for (DWORD index = 0; index < XUSER_MAX_COUNT; ++index)
        {
            XINPUT_STATE state{};
            if (api().getState(index, &state) == ERROR_SUCCESS) return static_cast<int>(index);
        }
        return -1;
    }

    unsigned readXInputPads(unsigned mask, XInputPadState *pads)
    {
        unsigned connected = 0;
        if (!api().getState) return 0;
        for (DWORD index = 0; index < XUSER_MAX_COUNT; ++index)
        {
            if (!((mask >> index) & 1u)) continue;
            XINPUT_STATE state{};
            if (api().getState(index, &state) != ERROR_SUCCESS) continue;
            connected |= 1u << index;
            if (pads)
            {
                const XINPUT_GAMEPAD &g = state.Gamepad;
                pads[index] = {g.wButtons, g.sThumbLX, g.sThumbLY, g.sThumbRX, g.sThumbRY, g.bLeftTrigger, g.bRightTrigger};
            }
        }
        return connected;
    }

    unsigned connectedXInputPads() { return readXInputPads(0xfu, nullptr); }

    bool setXInputRumble(int index, uint16_t lowFrequency, uint16_t highFrequency)
    {
        if (!api().setState || index < 0 || index >= XUSER_MAX_COUNT) return false;
        XINPUT_VIBRATION vibration{lowFrequency, highFrequency};
        return api().setState(static_cast<DWORD>(index), &vibration) == ERROR_SUCCESS;
    }
}
#else
namespace ps2_pad
{
    int firstXInputPad() { return -1; }
    unsigned readXInputPads(unsigned, XInputPadState *) { return 0; }
    unsigned connectedXInputPads() { return 0; }
    bool setXInputRumble(int, uint16_t, uint16_t) { return false; }
}
#endif
