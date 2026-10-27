#include "ps2x/iop/gt4_host_glue.h"

#include "gt4_lgdev.h"
#include "gt4_net.h"
#include "gt4_powoff.h"
#include "gt4_wheel_di.h"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <set>
#include <vector>

// Runtime <-> GT4 IOP glue (header: include/ps2x/iop/gt4_host_glue.h).
// Integration notes and tests: analysis/extras/wheel-glue/README.md.

namespace ps2x::iop::gt4::glue
{
    namespace
    {
        std::string lowerEnv(const char *name)
        {
            const char *v = std::getenv(name);
            std::string out = v ? v : "";
            for (char &c : out)
                c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            return out;
        }

        std::string hex(uint32_t v)
        {
            char text[16];
            std::snprintf(text, sizeof(text), "0x%x", v);
            return text;
        }

        // ------------------------------------------------------------ libmrpc state
        struct Pending
        {
            uint32_t receive = 0;
            uint32_t receiveSize = 0;
            bool done = false;
            std::vector<uint8_t> reply;
        };

        struct MrpcState
        {
            std::mutex mutex;
            std::condition_variable done;
            std::map<uint64_t, Pending> pending;
            uint64_t nextToken = 0;
            std::set<std::string> logged;
        };

        MrpcState &mrpcState()
        {
            static MrpcState state;
            return state;
        }

        void logOnce(const LogLine &log, const std::string &key, const std::string &line)
        {
            if (!log)
                return;
            {
                auto &s = mrpcState();
                std::lock_guard lock(s.mutex);
                if (!s.logged.insert(key).second)
                    return;
            }
            log(line);
        }

        bool read32(const GuestMemory &memory, uint32_t address, uint32_t &value)
        {
            return memory.read && memory.read(address, &value, 4);
        }

        bool write32(const GuestMemory &memory, uint32_t address, uint32_t value)
        {
            return memory.write && memory.write(address, &value, 4);
        }

        // libmrpc client (sceSifMClientData) words used here [C CORE 0x590cc8 / 0x590e98 / 0x590fd8]:
        // +0x10 SID, +0x24 server (non-zero once bound; libnet loops while 0, 0x56fa64),
        // +0x2c unbound flag (sceSifMUnBindRpc returns it; sceLibnetTerminate loops until 1,
        // 0x56fae8), +0x30 buffer size (libnetb's callrpc checks it, 0x56ea68), +0x34 / +0x38
        // the bind's t0 / t1.
        constexpr uint32_t ClientSid = 0x10, ClientServer = 0x24, ClientUnbound = 0x2c, ClientBufferSize = 0x30,
                           ClientArg4 = 0x34, ClientArg5 = 0x38;
        // Non-zero "server" word; on the console it is the IOP address of the RTINETB server
        // record. Nothing on the EE side dereferences it.
        constexpr uint32_t NativeServer = 0x0001f000u;
    }

    // ---------------------------------------------------------------- power button
    void setPowerOffQuit(QuitHandler quit)
    {
        if (!quit)
        {
            setPowerOffHandler({});
            return;
        }
        setPowerOffHandler([quit = std::move(quit)](const PowerOffReport &report) {
            quit(report.vsync, powerButtonSourceName(report.source));
        });
    }

    bool windowCloseIsPowerButton()
    {
        const std::string v = lowerEnv("PS2X_POWER_BUTTON");
        return !(v == "0" || v == "off");
    }

    void pressPowerButtonFromWindow() { pressPowerButton(PowerButtonSource::WindowClose); }

    // ---------------------------------------------------------------- host wheel
    std::string installHostWheel(void *nativeWindow, bool headless, LogLine log)
    {
        const std::string mode = lowerEnv("PS2X_WHEEL");
        if (mode != "dinput" && mode != "host")
        {
            setHostWheelFactory({}); // LGDEV's own handling: virtual / virtual2 / none
            return mode.empty() ? std::string("no host wheel (PS2X_WHEEL unset)")
                                : "PS2X_WHEEL=" + mode + ": handled by LGDEV (no DirectInput)";
        }
        if (headless)
        {
            setHostWheelFactory({});
            return "PS2X_WHEEL=" + mode + " refused in a --headless run (no device is opened in hidden runs); no wheel";
        }
        if (!nativeWindow)
        {
            setHostWheelFactory({});
            return "PS2X_WHEEL=" + mode + " needs the game window (DirectInput cooperative level); no wheel";
        }
        DiWheelConfig config;
        std::string error;
        if (!diWheelConfigFrom([](const char *name) { return std::getenv(name); }, config, error))
        {
            setHostWheelFactory({});
            return "PS2X_WHEEL=" + mode + ": " + error + "; no wheel";
        }
        std::shared_ptr<DiBackend> backend = createDirectInput8Backend();
        if (!backend)
        {
            setHostWheelFactory({});
            return "PS2X_WHEEL=" + mode + ": DirectInput is not available on this platform; no wheel";
        }
        setHostWheelFactory([backend, config, nativeWindow, log]() -> std::shared_ptr<HostWheel> {
            DirectInputWheel::Options options;
            options.window = nativeWindow;
            return std::make_shared<DirectInputWheel>(backend, config, options, [log](std::string_view line) {
                if (log)
                    log(line);
            });
        });
        return "PS2X_WHEEL=" + mode + ": DirectInput wheel adapter registered for LGDEV";
    }

    void removeHostWheel() { setHostWheelFactory({}); }

    void hostWheelEmergencyStop() noexcept { hostWheelEmergencyStopAll(); }

    // ---------------------------------------------------------------- libmrpc
    MrpcMode mrpcMode()
    {
        const std::string v = lowerEnv("GT4_NET_MRPC");
        if (v == "off" || v == "0")
            return MrpcMode::Off;
        if (v == "on" || v == "1")
            return MrpcMode::On;
        return MrpcMode::Auto;
    }

    const char *mrpcModeName(MrpcMode mode)
    {
        switch (mode)
        {
        case MrpcMode::Off: return "off";
        case MrpcMode::Auto: return "auto";
        case MrpcMode::On: return "on";
        }
        return "?";
    }

    bool mrpcInstallWanted()
    {
        switch (mrpcMode())
        {
        case MrpcMode::Off: return false;
        case MrpcMode::On: return true;
        case MrpcMode::Auto:
        {
            const std::string net = lowerEnv("GT4_NET");
            return !net.empty() && net != "off" && net != "0";
        }
        }
        return false;
    }

    int32_t mrpcBind(const GuestMemory &memory, uint32_t client, uint32_t sid, uint32_t mode, uint32_t bufferSize,
                     uint32_t arg4, uint32_t arg5, const LogLine &log)
    {
        if (mode != 0)
        {
            // libmrpc prints and returns -1 for any non-zero mode (CORE 0x590cfc) [C].
            logOnce(log, "bind-mode", "sceSifMBindRpc mode " + std::to_string(mode) + " -> -1 (libmrpc binds synchronously only)");
            return -1;
        }
        if (!net::mrpcService(sid))
        {
            // -1: sceLibnetInitialize returns -0x21c instead of looping (CORE 0x56fa5c) [C].
            logOnce(log, "bind-sid-" + hex(sid), "sceSifMBindRpc SID " + hex(sid) + ": no native service (GT4_NET off?) -> -1");
            return -1;
        }
        bool ok = write32(memory, client + ClientSid, sid);
        ok &= write32(memory, client + ClientServer, NativeServer);
        ok &= write32(memory, client + ClientUnbound, 0);
        ok &= write32(memory, client + ClientBufferSize, bufferSize);
        ok &= write32(memory, client + ClientArg4, arg4);
        ok &= write32(memory, client + ClientArg5, arg5);
        if (!ok)
        {
            logOnce(log, "bind-client", "sceSifMBindRpc: client " + hex(client) + " not writable -> -1");
            return -1;
        }
        logOnce(log, "bind-ok-" + hex(sid), "sceSifMBindRpc SID " + hex(sid) + " client " + hex(client) + " buffer " +
                                                hex(bufferSize) + ": bound to the native RTINETB");
        return 0;
    }

    int32_t mrpcUnbind(const GuestMemory &memory, uint32_t client, uint32_t mode, const LogLine &log)
    {
        if (mode != 0)
        {
            logOnce(log, "unbind-mode", "sceSifMUnBindRpc mode " + std::to_string(mode) + " -> -1");
            return -1;
        }
        if (!write32(memory, client + ClientServer, 0) || !write32(memory, client + ClientUnbound, 1))
            return -1;
        logOnce(log, "unbind", "sceSifMUnBindRpc client " + hex(client) + ": unbound");
        return 1; // the reply handler's "unbound" flag, which libmrpc returns (CORE 0x590fb4) [C]
    }

    MrpcStart mrpcCall(const GuestMemory &memory, const MrpcCall &call, MrpcWake wake, const LogLine &log)
    {
        MrpcStart start;
        if (call.mode != 0)
        {
            logOnce(log, "call-mode", "sceSifMCallRpc mode " + std::to_string(call.mode) + " -> -1 (libmrpc: synchronous only)");
            return start;
        }
        // libmrpc refuses buffers not on a 64-byte boundary (CORE 0x591044..0x59108c) [C].
        if ((call.send & 0x3fu) || (call.receive & 0x3fu))
        {
            logOnce(log, "call-align", "sceSifMCallRpc fn " + std::to_string(call.function) + ": buffer " +
                                           hex(call.send & 0x3fu ? call.send : call.receive) + " not 64-byte aligned -> -1");
            return start;
        }
        if (call.endFunction)
            logOnce(log, "call-end", "sceSifMCallRpc end function " + hex(call.endFunction) + " ignored (GT4 passes 0)");
        uint32_t sid = 0, server = 0;
        if (!read32(memory, call.client + ClientSid, sid) || !read32(memory, call.client + ClientServer, server) || !server)
        {
            logOnce(log, "call-unbound", "sceSifMCallRpc on an unbound client " + hex(call.client) + " -> -1");
            return start;
        }
        net::Rtinetb *service = net::mrpcService(sid);
        if (!service)
        {
            logOnce(log, "call-sid-" + hex(sid), "sceSifMCallRpc SID " + hex(sid) + ": no native service -> -1");
            return start;
        }
        RpcRequest request;
        request.clientAddress = call.client;
        request.sid = sid;
        request.function = call.function;
        request.mode = 0;
        request.send = {call.send, call.sendSize};
        request.receive = {call.receive, call.receiveSize};
        request.endFunction = call.endFunction;
        request.endParameter = call.endParam;

        auto &s = mrpcState();
        uint64_t token;
        {
            std::lock_guard lock(s.mutex);
            token = ++s.nextToken;
            s.pending[token] = Pending{call.receive, call.receiveSize, false, {}};
        }
        // Stored before the wake is posted, so a completion that arrives before the EE thread
        // blocks (even before call() returns) is not lost: the scheduler handles the wake after
        // the waiter is registered.
        auto later = [token, wake](std::vector<uint8_t> &&reply) {
            auto &st = mrpcState();
            {
                std::lock_guard lock(st.mutex);
                if (auto it = st.pending.find(token); it != st.pending.end())
                {
                    it->second.reply = std::move(reply);
                    it->second.done = true;
                }
            }
            st.done.notify_all();
            if (wake)
                wake(token);
        };
        std::vector<uint8_t> reply;
        const net::CallState state = service->call(request, reply, later);
        if (state == net::CallState::Deferred)
        {
            start.wait = true;
            start.v0 = 0;
            start.token = token;
            return start;
        }
        {
            std::lock_guard lock(s.mutex);
            s.pending.erase(token);
        }
        if (state == net::CallState::Rejected)
        {
            logOnce(log, "call-rejected-" + std::to_string(call.function),
                    "sceSifMCallRpc fn " + std::to_string(call.function) + " rejected by RTINETB -> -1");
            return start;
        }
        const size_t bytes = std::min<size_t>(reply.size(), call.receiveSize);
        if (bytes && !(memory.write && memory.write(call.receive, reply.data(), bytes)))
        {
            logOnce(log, "call-receive", "sceSifMCallRpc fn " + std::to_string(call.function) + ": receive buffer " +
                                             hex(call.receive) + " not writable -> -2");
            start.v0 = -2;
            return start;
        }
        start.v0 = 0;
        return start;
    }

    bool mrpcFinish(const GuestMemory &memory, uint64_t token, int32_t &v0)
    {
        Pending done;
        {
            auto &s = mrpcState();
            std::lock_guard lock(s.mutex);
            const auto it = s.pending.find(token);
            if (it == s.pending.end() || !it->second.done)
            {
                v0 = -2;
                return false;
            }
            done = std::move(it->second);
            s.pending.erase(it);
        }
        const size_t bytes = std::min<size_t>(done.reply.size(), done.receiveSize);
        if (bytes && !(memory.write && memory.write(done.receive, done.reply.data(), bytes)))
        {
            v0 = -2;
            return false;
        }
        v0 = 0;
        return true;
    }

    void mrpcAbandon(uint64_t token)
    {
        auto &s = mrpcState();
        std::lock_guard lock(s.mutex);
        s.pending.erase(token);
    }

    int32_t mrpcWaitHost(const GuestMemory &memory, uint64_t token, uint32_t timeoutMs, const LogLine &log)
    {
        auto &s = mrpcState();
        {
            std::unique_lock lock(s.mutex);
            const bool finished = s.done.wait_for(lock, std::chrono::milliseconds(timeoutMs), [&] {
                const auto it = s.pending.find(token);
                return it == s.pending.end() || it->second.done;
            });
            if (!finished)
            {
                s.pending.erase(token);
                lock.unlock();
                logOnce(log, "host-wait-timeout", "sceSifMCallRpc from an interrupt handler: no reply within " +
                                                      std::to_string(timeoutMs) + " ms -> -2");
                return -2;
            }
        }
        int32_t v0 = -2;
        if (!mrpcFinish(memory, token, v0))
            mrpcAbandon(token);
        return v0;
    }

    size_t mrpcPending()
    {
        auto &s = mrpcState();
        std::lock_guard lock(s.mutex);
        return s.pending.size();
    }
}
