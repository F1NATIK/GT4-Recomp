#include "Gt4HostGlue.h"

#if defined(PS2X_GAME_GT4)

#include "Common.h"
#include "runtime/ee_scheduler.h"
#include "ps2x/iop/gt4_host_glue.h"

#include <cstdlib>
#include <cstring>
#include <iostream>
#include <mutex>
#include <string>

namespace glue = ps2x::iop::gt4::glue;

// GT4's pointer-only CORE functions that the main generated set lacks (src/gt4_late/gt4_late_functions.cpp, compiled
// into GT4.exe, which sets this pointer): entered in the dispatch table when GT4_NET is on (the LAN path reaches them,
// e.g. RTIME's 0x523160; without them the network page waits for ever at "Connecting...").
int (*g_gt4LateFunctionsInstaller)(PS2Runtime &) = nullptr;

namespace
{
    // GT4's CORE.elf (profile gt4-us-core): the libmrpc addresses below are its own.
    constexpr uint32_t Gt4Entry = 0x00100008u;
    constexpr uint32_t Gt4CoreCrc32 = 0x19257b64u;
    // EeExternalWait type of a deferred sceSifMCallRpc ('MRPC'; MPEG uses 1).
    constexpr uint32_t MrpcWaitType = 0x4d525043u;
    // An interrupt handler that calls libnetb cannot block its EE thread (GT4 never does [I]).
    constexpr uint32_t MrpcHostWaitMs = 30000u;

    void mrpcLog(std::string_view line) { std::cerr << "[GT4/MRPC] " << line << std::endl; }
    void wheelLog(std::string_view line) { std::cerr << "[GT4/WHEEL] " << line << std::endl; }

    glue::GuestMemory guestMemory(uint8_t *rdram)
    {
        auto range = [rdram](uint32_t address, size_t size) -> uint8_t * {
            if (size == 0 || size - 1 > 0xffffffffu - address)
                return nullptr;
            uint8_t *first = getMemPtr(rdram, address);
            uint8_t *last = getMemPtr(rdram, address + static_cast<uint32_t>(size - 1));
            if (!first || !last || last < first || static_cast<size_t>(last - first) != size - 1)
                return nullptr;
            return first;
        };
        glue::GuestMemory memory;
        memory.read = [range](uint32_t address, void *destination, size_t size) {
            uint8_t *p = range(address, size);
            if (!p)
                return false;
            std::memcpy(destination, p, size);
            return true;
        };
        memory.write = [range](uint32_t address, const void *source, size_t size) {
            uint8_t *p = range(address, size);
            if (!p)
                return false;
            std::memcpy(p, source, size);
            return true;
        };
        return memory;
    }

    // ---------------------------------------------------------------- libmrpc (CORE 0x590cc8 / 0x590e98 / 0x590fd8)
    // Native replacements in the generated function table: every JAL / J reaches them through
    // dispatchGuestBranch -> lookupFunction (no generated code calls them directly), so CORE
    // needs no regeneration. Like the generated stubs they return to $ra.
    void nativeMBindRpc(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        (void)runtime;
        const uint32_t ra = getRegU32(ctx, 31);
        const int32_t v0 = glue::mrpcBind(guestMemory(rdram), getRegU32(ctx, 4), getRegU32(ctx, 5), getRegU32(ctx, 6),
                                          getRegU32(ctx, 7), getRegU32(ctx, 8), getRegU32(ctx, 9), mrpcLog);
        setReturnS32(ctx, v0);
        ctx->pc = ra;
    }

    void nativeMUnBindRpc(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        (void)runtime;
        const uint32_t ra = getRegU32(ctx, 31);
        const int32_t v0 = glue::mrpcUnbind(guestMemory(rdram), getRegU32(ctx, 4), getRegU32(ctx, 5), mrpcLog);
        setReturnS32(ctx, v0);
        ctx->pc = ra;
    }

    void nativeMCallRpc(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t ra = getRegU32(ctx, 31);
        const glue::GuestMemory memory = guestMemory(rdram);
        glue::MrpcCall call;
        call.client = getRegU32(ctx, 4);
        call.function = getRegU32(ctx, 5);
        call.mode = getRegU32(ctx, 6);
        call.send = getRegU32(ctx, 7);
        call.sendSize = getRegU32(ctx, 8);
        call.receive = getRegU32(ctx, 9);
        call.receiveSize = getRegU32(ctx, 10);
        call.endFunction = getRegU32(ctx, 11);
        uint32_t endParam = 0; // ninth argument at the caller's sp+0 (CORE 0x5910a8)
        memory.read(getRegU32(ctx, 29), &endParam, 4);
        call.endParam = endParam;
        ctx->pc = ra;

        glue::MrpcStart start;
        // The IOP call scope is held for the request only (RTINETB reads the send buffer and
        // may start a worker); never while waiting, because RTINETB's ring threads take the
        // same lock to commit EE memory.
        runtime->runInIopCallScope(rdram, ctx, [&] {
            start = glue::mrpcCall(memory, call, [runtime](uint64_t token) {
                runtime->eeScheduler().postEvent({EeEventType::ExternalWake, MrpcWaitType, token});
            }, mrpcLog);
        });
        if (!start.wait)
        {
            setReturnS32(ctx, start.v0);
            return;
        }
        EeScheduler &scheduler = runtime->eeScheduler();
        const GuestThread *self = scheduler.currentThread();
        if (!self || !self->invocations.empty())
        {
            // An interrupt handler or callback has no thread of its own to block.
            static std::once_flag once;
            std::call_once(once, [] { mrpcLog("sceSifMCallRpc from an interrupt handler or callback: waiting on the host"); });
            setReturnS32(ctx, glue::mrpcWaitHost(memory, start.token, MrpcHostWaitMs, mrpcLog));
            return;
        }
        // Block this EE thread; the wake is an ExternalWake event posted by the completion (any
        // thread). postEvent only queues it and the executor processes events after this
        // thread is registered as waiting (blockCurrent runs before any checkpoint), so a
        // completion that came first is not lost; the reply is kept by token until then.
        const uint64_t token = start.token;
        scheduler.waitExternal(EeWaitReason::External, MrpcWaitType, token, [rdram, token](R5900Context &resume) {
            int32_t v0 = -2;
            if (!glue::mrpcFinish(guestMemory(rdram), token, v0))
            {
                // Released some other way (ReleaseWaitThread): libmrpc's "send failed" value.
                glue::mrpcAbandon(token);
                v0 = -2;
            }
            setReturnS32(&resume, v0);
        });
    }
}

namespace ps2_gt4_glue
{
    void beforeIopProfile(PS2Runtime &runtime)
    {
        // POWOFF's host quit: request the stop (flags only; safe inside the VBlank event).
        glue::setPowerOffQuit([&runtime](uint64_t vsync, std::string_view source) {
            std::cerr << "[power] GT4 power-off (" << source << ") at vsync " << vsync << ": stopping" << std::endl;
            runtime.requestStop();
        });
        void *window = nullptr;
        if (!runtime.headless() && IsWindowReady())
            window = GetWindowHandle();
        const std::string decision = glue::installHostWheel(window, runtime.headless(), wheelLog);
        if (const char *wheel = std::getenv("PS2X_WHEEL"); wheel && *wheel)
            wheelLog(decision);
    }

    void afterLoad(PS2Runtime &runtime, uint32_t entry, uint32_t crc32)
    {
        if (const char *net = std::getenv("GT4_NET"); net && *net && std::string(net) != "off" && std::string(net) != "0" &&
                                                      entry == Gt4Entry && crc32 == Gt4CoreCrc32)
        {
            if (g_gt4LateFunctionsInstaller)
                std::cerr << "[GT4/NET] " << g_gt4LateFunctionsInstaller(runtime)
                          << " pointer-only CORE functions entered in the dispatch table (gt4_late_functions.cpp)" << std::endl;
            else
                std::cerr << "[GT4/NET] this GT4.exe has no late CORE functions (gt4_late_functions.cpp)" << std::endl;
        }
        if (!glue::mrpcInstallWanted())
            return;
        if (entry != Gt4Entry || crc32 != Gt4CoreCrc32)
        {
            mrpcLog("not GT4's CORE.elf (entry/CRC differ): libmrpc left as translated");
            return;
        }
        if (!runtime.hasFunction(glue::MrpcBindAddress) || !runtime.hasFunction(glue::MrpcUnbindAddress) ||
            !runtime.hasFunction(glue::MrpcCallAddress))
        {
            mrpcLog("libmrpc functions missing from the generated table: left as translated");
            return;
        }
        const bool ok = runtime.replaceFunction(glue::MrpcBindAddress, &nativeMBindRpc) &&
                        runtime.replaceFunction(glue::MrpcUnbindAddress, &nativeMUnBindRpc) &&
                        runtime.replaceFunction(glue::MrpcCallAddress, &nativeMCallRpc);
        mrpcLog(std::string(ok ? "native" : "FAILED to install native") +
                " sceSifMBindRpc 0x590cc8 / sceSifMUnBindRpc 0x590e98 / sceSifMCallRpc 0x590fd8 (GT4_NET_MRPC=" +
                glue::mrpcModeName(glue::mrpcMode()) + ")");
    }

    void shutdown()
    {
        glue::setPowerOffQuit({});
        glue::removeHostWheel();
    }

    bool windowCloseToPowerButton()
    {
        if (!glue::windowCloseIsPowerButton())
            return false;
        glue::pressPowerButtonFromWindow();
        return true;
    }
}

#else // GT3 and other builds: nothing to glue.

namespace ps2_gt4_glue
{
    void beforeIopProfile(PS2Runtime &) {}
    void afterLoad(PS2Runtime &, uint32_t, uint32_t) {}
    void shutdown() {}
    bool windowCloseToPowerButton() { return false; }
}

#endif
