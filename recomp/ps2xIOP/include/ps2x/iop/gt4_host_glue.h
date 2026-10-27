#pragma once

// GT4 (SCUS-97328) host glue between the runtime and the GT4-only IOP
// services, so ps2xRuntime needs no ps2xIOP/src/modules header. Standard
// headers only. Implemented in ps2xIOP/src/modules/gt4_host_glue.cpp (GT4
// builds; the runtime calls it only under PS2X_GAME_GT4).
//
//   power button  POWOFF (gt4_powoff.h): the runtime's quit handler and the
//                 window close mapped to the console power button
//   host wheel    LGDEV (gt4_lgdev.h): the DirectInput 8 adapter
//                 (gt4_wheel_di.h) registered for visible play
//   libmrpc       CORE's sceSifMBindRpc / sceSifMUnBindRpc / sceSifMCallRpc
//                 (EE 0x590cc8 / 0x590e98 / 0x590fd8) answered natively by
//                 RTINETB (gt4_net.h), with deferred calls completed through
//                 the EE scheduler (the runtime does the waiting)

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>

namespace ps2x::iop::gt4::glue
{
    using LogLine = std::function<void(std::string_view)>;

    // ---------------------------------------------------------------- power button (POWOFF)
    // The host quit after GT4's power-off sequence. Runs on the EE scheduler
    // thread inside the VBlank event: request the stop, never wait for guest
    // progress. An empty handler unregisters (a press is then logged and ignored).
    using QuitHandler = std::function<void(uint64_t vsync, std::string_view source)>;
    void setPowerOffQuit(QuitHandler quit);
    // PS2X_POWER_BUTTON=0 turns the window close back into an immediate stop.
    bool windowCloseIsPowerButton();
    void pressPowerButtonFromWindow(); // window close button / Alt+F4; thread-safe, later presses ignored

    // ---------------------------------------------------------------- host wheel (LGDEV)
    // Call before the GT4 IOP profile is created (LGDEV asks for its wheel then).
    // PS2X_WHEEL=dinput (alias host): the DirectInput adapter, visible play only
    // (refused with --headless or without a window: no device is ever opened in
    // hidden runs). Any other PS2X_WHEEL value is left to LGDEV's own handling
    // (virtual, virtual2, none). Returns one line saying what was decided.
    std::string installHostWheel(void *nativeWindow, bool headless, LogLine log);
    void removeHostWheel();
    // Crash handler, any thread, no blocking lock: stop the forces of every open wheel.
    void hostWheelEmergencyStop() noexcept;

    // ---------------------------------------------------------------- libmrpc (RTINETB over MSIFRPC)
    constexpr uint32_t MrpcBindAddress = 0x590cc8u;   // sceSifMBindRpc
    constexpr uint32_t MrpcUnbindAddress = 0x590e98u; // sceSifMUnBindRpc
    constexpr uint32_t MrpcCallAddress = 0x590fd8u;   // sceSifMCallRpc

    enum class MrpcMode : uint8_t
    {
        Off,  // GT4_NET_MRPC=off: CORE's translated libmrpc (hangs once LAN code runs, as before)
        Auto, // default: native when GT4_NET is set and not "off"
        On,   // always native (with GT4_NET off a bind fails: libnet reports -0x21c instead of hanging)
    };
    MrpcMode mrpcMode();
    bool mrpcInstallWanted();
    const char *mrpcModeName(MrpcMode mode);

    // EE memory as the runtime sees it (little-endian guest addresses).
    struct GuestMemory
    {
        std::function<bool(uint32_t address, void *destination, size_t size)> read;
        std::function<bool(uint32_t address, const void *source, size_t size)> write;
    };

    // a0 client, a1 sid, a2 mode, a3 buffer size, t0, t1 (CORE 0x590cc8). Returns v0.
    int32_t mrpcBind(const GuestMemory &memory, uint32_t client, uint32_t sid, uint32_t mode, uint32_t bufferSize,
                     uint32_t arg4, uint32_t arg5, const LogLine &log);
    // a0 client, a1 mode (CORE 0x590e98). Returns v0 (1 = unbound).
    int32_t mrpcUnbind(const GuestMemory &memory, uint32_t client, uint32_t mode, const LogLine &log);

    struct MrpcCall
    {
        uint32_t client = 0;      // a0
        uint32_t function = 0;    // a1
        uint32_t mode = 0;        // a2
        uint32_t send = 0;        // a3
        uint32_t sendSize = 0;    // t0
        uint32_t receive = 0;     // t1
        uint32_t receiveSize = 0; // t2
        uint32_t endFunction = 0; // t3
        uint32_t endParam = 0;    // stack +0
    };
    struct MrpcStart
    {
        bool wait = false; // the call is running: block the EE thread until wake(token), then mrpcFinish
        int32_t v0 = -1;   // when !wait: the return value (the reply is already in EE memory)
        uint64_t token = 0;
    };
    // Called on any thread when a deferred call has its reply; may run before mrpcCall returns.
    using MrpcWake = std::function<void(uint64_t token)>;
    // CORE 0x590fd8. Hold the IOP call scope around it, never around the wait.
    MrpcStart mrpcCall(const GuestMemory &memory, const MrpcCall &call, MrpcWake wake, const LogLine &log);
    // After the wake: copies the reply to the receive buffer; v0 = 0. False (v0 = -2) if the call
    // has not finished (the thread was released by something else) or the token is unknown.
    bool mrpcFinish(const GuestMemory &memory, uint64_t token, int32_t &v0);
    // Forget a call whose waiter gave up; its late reply is dropped.
    void mrpcAbandon(uint64_t token);
    // A caller that cannot block its EE thread (an interrupt handler): wait on the host,
    // at most `timeoutMs`, then finish. Returns v0.
    int32_t mrpcWaitHost(const GuestMemory &memory, uint64_t token, uint32_t timeoutMs, const LogLine &log);
    size_t mrpcPending();
}
