#pragma once

#include <string_view>
#include <string>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WINAPI
#define WINAPI __stdcall
#endif

extern "C"
{
    typedef void *HANDLE;
    typedef void *HMODULE;
    typedef const wchar_t *PCWSTR;
    typedef long HRESULT;

    __declspec(dllimport) HMODULE WINAPI GetModuleHandleW(const wchar_t *lpModuleName);
    __declspec(dllimport) void *WINAPI GetProcAddress(HMODULE hModule, const char *lpProcName);
    __declspec(dllimport) HANDLE WINAPI GetCurrentThread(void);
    __declspec(dllimport) HANDLE WINAPI GetCurrentProcess(void);
    __declspec(dllimport) int WINAPI SetThreadPriority(HANDLE hThread, int nPriority);
}
#include <cstdlib>
#elif defined(__APPLE__) || defined(__linux__)
#include <pthread.h>
#endif

namespace ThreadNaming
{
    inline void SetCurrentThreadName(std::string_view name)
    {
#if defined(_WIN32) 
        using SetThreadDescriptionFn = HRESULT(WINAPI*)(HANDLE, PCWSTR);

        HMODULE kernel32 = ::GetModuleHandleW(L"Kernel32.dll");
        auto setThreadDescription =
            reinterpret_cast<SetThreadDescriptionFn>(::GetProcAddress(kernel32, "SetThreadDescription"));

        if (setThreadDescription)
        {
            std::wstring wname(name.begin(), name.end());
            setThreadDescription(::GetCurrentThread(), wname.c_str());
        }

#elif defined(__APPLE__)
        pthread_setname_np(name.data());

#elif defined(__linux__)
        pthread_setname_np(pthread_self(), name.data());
#endif
    }

    // Opts the calling thread out of Windows power throttling (EcoQoS). On
    // hybrid CPUs (performance + efficiency cores) a thread of a window that
    // is not in front can otherwise be moved to an efficiency core; the
    // emulation pipeline threads (EE, VIF1/VU1, GS) call this. No-op elsewhere
    // or where the API is missing.
    inline void PreferPerformance()
    {
#if defined(_WIN32)
        struct PowerThrottlingState
        {
            unsigned long version, controlMask, stateMask;
        };
        using SetThreadInformationFn = int(WINAPI *)(HANDLE, int, void *, unsigned long);
        HMODULE kernel32 = ::GetModuleHandleW(L"Kernel32.dll");
        auto setThreadInformation = reinterpret_cast<SetThreadInformationFn>(::GetProcAddress(kernel32, "SetThreadInformation"));
        if (setThreadInformation)
        {
            // THREAD_POWER_THROTTLING_CURRENT_VERSION 1, EXECUTION_SPEED 1, state 0: never throttle.
            PowerThrottlingState state{1u, 1u, 0u};
            setThreadInformation(::GetCurrentThread(), 3 /* ThreadPowerThrottling */, &state, sizeof(state));
        }
        // GT4_THREAD_PRIORITY = above (GT4 builds' default) | high | normal (GT3 builds' default): the
        // emulation pipeline threads (EE, VIF1/VU1, GS, the GS renderer) above other programs' normal threads,
        // so a busy desktop (sync clients, browsers) does not delay their wake-ups: they hand work to each other
        // ~100 times a frame (GT4's VIF1 interrupts start its texture uploads), and each late wake-up is a late
        // frame on an 8-thread CPU (i7-9700K, Trial Mountain with OneDrive syncing: 26.9 -> 33.0 fps).
        static const int priority = [] {
            const char *v = std::getenv("GT4_THREAD_PRIORITY");
#if defined(PS2X_GAME_GT3)
            const std::string_view s = v ? v : "normal";
#else
            const std::string_view s = v ? v : "above";
#endif
            return s == "high" ? 2 : s == "above" ? 1 : 0;
        }();
        if (priority != 0)
            ::SetThreadPriority(::GetCurrentThread(), priority);
#endif
    }

    // The same for the whole process (ProcessPowerThrottling), and the timer
    // resolution: Windows 11 may ignore the process's 1 ms timer request
    // (raylib's timeBeginPeriod) while its window is hidden, minimized or
    // covered, which paces VBlanks on ~15.6 ms ticks.
    inline void PreferPerformanceProcess()
    {
#if defined(_WIN32)
        struct PowerThrottlingState
        {
            unsigned long version, controlMask, stateMask;
        };
        using SetProcessInformationFn = int(WINAPI *)(HANDLE, int, void *, unsigned long);
        HMODULE kernel32 = ::GetModuleHandleW(L"Kernel32.dll");
        auto setProcessInformation = reinterpret_cast<SetProcessInformationFn>(::GetProcAddress(kernel32, "SetProcessInformation"));
        if (setProcessInformation)
        {
            // EXECUTION_SPEED 1 | IGNORE_TIMER_RESOLUTION 4, state 0: neither
            // throttle. Windows 10 refuses bit 4: then EXECUTION_SPEED only.
            PowerThrottlingState state{1u, 0x1u | 0x4u, 0u};
            if (!setProcessInformation(::GetCurrentProcess(), 4 /* ProcessPowerThrottling */, &state, sizeof(state)))
            {
                state.controlMask = 0x1u;
                setProcessInformation(::GetCurrentProcess(), 4 /* ProcessPowerThrottling */, &state, sizeof(state));
            }
        }
#endif
    }
}
