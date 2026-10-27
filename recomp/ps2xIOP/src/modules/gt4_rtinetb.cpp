#include "gt4_net.h"

#include <chrono>
#include <deque>
#include <future>
#include <list>
#include <set>

// RTINETB.IRX (GT4, "Libnet 1.20", SID 0x80001201) natively. Request layouts,
// results and the asynchronous ring protocol are ported from GT4's module
// (analysis\iop-sys\disasm\gt4\RTINETB.txt) and checked against the EE side in
// CORE (libnetb_ex 0x56ea50-0x571dd8); addresses below are RTINETB-relative
// unless marked CORE. Every function works in place on the request buffer;
// word 0 of the reply is the result. The event model is GT4's INETCTL as
// RTINETB sees it (handler 0x0, classifier 0x358, waits 0x1ec).

namespace ps2x::iop::gt4::net
{
    namespace
    {
        using Clock = std::chrono::steady_clock;
        using namespace std::chrono_literals;

        constexpr int32_t ErrSemaphore = -0x21c, ErrUnhandled = -0x21d, ErrEvent = -0x220;
        constexpr int32_t ErrInetArgument = -0x200; // INET's code for a bad id / length (INET 0x10418) [C]
        constexpr int32_t InterfaceId = 1;
        // SMAP registers its netdev with flags 0x490 (SMAP 0x1db8) [C]; INETCTL's
        // up / down set and clear bit 0 (INET 0x1090c / 0x1091c) [C]; libnet
        // finds a usable interface by bit 1 (CORE 0x56fd18) and rt_comm waits
        // for bits 0|1 (CORE 0x523348) [C]; 0x20 DHCP [I]; 0x200 PPPoE (RTINETB 0x138) [C bit].
        constexpr uint32_t DevAttached = 0x490u, DevUp = 0x1u, DevRunning = 0x2u, DevDhcp = 0x20u, DevPppoe = 0x200u;
        // INETCTL event types (INETCTL 0x203c forwards INET's 1..10 and adds 6 / 7) [C numbers, I names].
        // Up / Down (inetctl#5 / #6 -> 0x2f5c) run 0x203c with 8 / 9 and forward them too [C].
        constexpr int32_t EvAttach = 1, EvStart = 3, EvStop = 4, EvConf = 6, EvUp = 8, EvDown = 9;
        constexpr int32_t ErrNoInterface = -1; // inetctl#10 for an id it does not list (0x3288) [C]
        // INETCTL interface states (INETCTL 0x1f64) [C numbers, I names].
        constexpr int32_t CtlStarted = 3, CtlIdle = 5;
        constexpr int32_t SliceMs = 50;

        uint32_t roundUp64(uint32_t n) { return (n + 0x3fu) & ~0x3fu; }

        const char *functionName(uint32_t fn)
        {
            switch (fn)
            {
            case 1: return "Create";
            case 2: return "Open";
            case 3: return "Close";
            case 4: return "Recv";
            case 5: return "Send";
            case 6: return "Name2Address";
            case 7: return "Address2String";
            case 8: return "GetInterfaceList";
            case 9: return "InterfaceControl";
            case 10: return "GetRoutingTable";
            case 11: return "GetNameServers";
            case 12: return "ChangeThreadPriority";
            case 13: return "RecvFrom";
            case 14: return "SendTo";
            case 15: return "Abort";
            case 16: return "AbortLog";
            case 17: return "GetLog";
            case 18: return "Address2Name";
            case 19: return "Control";
            case 20: return "Poll";
            case 30: return "libnet init";
            case 31: return "libnet term";
            case 32: return "SetConfiguration";
            case 33: return "wait interface";
            case 34: return "wait address";
            case 35: return "get event";
            case 50: return "Up";
            case 51: return "Down";
            case 52: return "SetAutoMode";
            case 53: return "GetState";
            case 0x64: return "async receive";
            case 0x65: return "async send";
            case 0x66: return "async close";
            case 0x67: return "dump";
            default: return "?";
            }
        }

        // One asynchronous half (RTINETB 0xf04 / 0xff4): a 0x40-byte header
        // copied from the EE descriptor, then the ring / buffer, in IOP memory.
        struct AsyncHandle
        {
            bool receive = false;
            uint32_t address = 0;      // IOP handle (header +0, ring +0x40)
            uint32_t ring = 0;         // +0x1c
            uint32_t ringSize = 0;     // +0x18
            uint32_t eeDescriptor = 0; // +0x20: where the 16-byte status goes
            uint32_t eeRing = 0;       // +0x2c (receive): the EE's ring
            int32_t cid = 0;           // +0x10
            int32_t type = 0;          // +0x34
            int32_t threadId = 0;      // +0x28
            std::atomic<uint32_t> offset{0}; // +0x3c
            std::atomic<bool> stop{false};   // +0x30 close request
            std::atomic<bool> closing{false}; // fn 0x66 in progress
            std::atomic<bool> headWritten{false};
            std::thread thread;
            std::mutex wakeMutex;
            std::condition_variable wake;
            uint64_t wakes = 0;

            bool udpLike() const { return type == 0 || type == 3; }
            bool covers(uint32_t a, uint32_t n) const
            {
                return a < address + 0x40u + ringSize && uint64_t(a) + n > address;
            }
            uint64_t ticket()
            {
                std::lock_guard lock(wakeMutex);
                return wakes;
            }
            void notify()
            {
                {
                    std::lock_guard lock(wakeMutex);
                    ++wakes;
                }
                wake.notify_all();
            }
            // Sleeps until notified after `seen`, stopped, or `timeout` passes.
            template <class Duration> void sleepSince(uint64_t seen, Duration timeout)
            {
                std::unique_lock lock(wakeMutex);
                wake.wait_for(lock, timeout, [&] { return wakes != seen || stop.load(); });
            }
        };

        struct Batch
        {
            std::vector<std::pair<uint32_t, std::vector<uint8_t>>> writes;
            void add(uint32_t address, const uint8_t *data, size_t size)
            {
                if (size)
                    writes.emplace_back(address, std::vector<uint8_t>(data, data + size));
            }
        };

        class Service final : public Rtinetb
        {
        public:
            Service(IopHost &host, Stack &stack, EnvStore &env) : host_(host), stack_(stack), env_(env) {}
            ~Service() override { shutdownAll(); }

            void reset() override
            {
                shutdownAll();
                stopping_ = false;
            }

            RpcResult handleRpc(const RpcRequest &request) override
            {
                auto promise = std::make_shared<std::promise<std::vector<uint8_t>>>();
                auto future = promise->get_future();
                std::vector<uint8_t> data;
                const CallState state = call(request, data, [promise](std::vector<uint8_t> &&reply) {
                    promise->set_value(std::move(reply));
                });
                if (state == CallState::Rejected)
                    return rejected();
                if (state == CallState::Deferred)
                    data = future.get();
                return reply(host_, request, data.data(), data.size());
            }

            CallState call(const RpcRequest &request, std::vector<uint8_t> &out, Completion later) override
            {
                if (request.sid != SidRtinetb)
                    return CallState::Rejected;
                ++calls_;
                const uint32_t fn = request.function;
                const size_t size = std::min<size_t>(
                    std::max<size_t>({request.send.size, request.receive.size, size_t(0x80)}), 0x10000);
                std::vector<uint8_t> buffer(size, 0);
                if (request.send.size &&
                    !host_.readGuest(request.send.address, buffer.data(), std::min<size_t>(request.send.size, size)))
                {
                    host_.log(LogLevel::Warning, "[GT4/NET] RTINETB fn " + hex(fn) + ": send buffer " +
                                                     hex(request.send.address) + " unreadable");
                    return CallState::Rejected;
                }
                noteFirstCall(fn);
                const size_t replySize = request.receive.size;
                if (dispatch(fn, buffer, replySize, later) == CallState::Deferred)
                    return CallState::Deferred;
                buffer.resize(replySize);
                out = std::move(buffer);
                return CallState::Completed;
            }

            void onSifTransfer(const SifTransfer &t) override
            {
                if (t.phase != SifTransferPhase::AfterCopy || t.kind != SifTransferKind::SetDma || !t.size)
                    return;
                std::vector<std::shared_ptr<AsyncHandle>> hit;
                {
                    std::lock_guard lock(handlesMutex_);
                    for (const auto *list : {&receivers_, &senders_})
                        for (const auto &h : *list)
                            if (h->covers(t.destinationAddress, t.size))
                                hit.push_back(h);
                }
                for (const auto &h : hit)
                {
                    if (t.destinationAddress < h->address + 0x10u)
                        h->headWritten = true;
                    h->notify();
                }
            }

            void appendMetrics(std::vector<DebugMetric> &metrics) const override
            {
                size_t handles = 0;
                {
                    std::lock_guard lock(handlesMutex_);
                    handles = receivers_.size() + senders_.size();
                }
                metrics.push_back({"rtinetb.calls", calls_.load(), false});
                metrics.push_back({"rtinetb.deferred", deferred_.load(), false});
                metrics.push_back({"rtinetb.async_handles", handles, false});
                metrics.push_back({"rtinetb.ring_records_in", ringRecords_.load(), false});
                metrics.push_back({"rtinetb.ring_records_out", sentRecords_.load(), false});
                metrics.push_back({"rtinetb.ring_full_waits", ringFull_.load(), false});
                metrics.push_back({"rtinetb.ring_wraps_in", ringWraps_.load(), false});
                metrics.push_back({"rtinetb.unhandled", unhandled_.load(), false});
            }

        private:
            // ------------------------------------------------------------ calls

            CallState dispatch(uint32_t fn, std::vector<uint8_t> &b, size_t replySize, Completion &later)
            {
                auto w = [&](size_t i) { return static_cast<int32_t>(rd32(b.data() + 4 * i)); };
                auto set = [&](size_t i, int32_t v) { wr32(b.data() + 4 * i, static_cast<uint32_t>(v)); };
                auto room = [&](size_t offset) { return b.size() > offset ? b.size() - offset : size_t(0); };
                auto count = [&](int32_t requested, size_t offset) {
                    return static_cast<int32_t>(std::clamp<int64_t>(requested, 0, int64_t(room(offset))));
                };

                switch (fn)
                {
                case 1: // inet#6 Create(param @+4) (0x4d4)
                {
                    const uint8_t *p = b.data() + 4;
                    set(0, stack_.create(int32_t(rd32(p)), int32_t(rd32(p + 4)), readInetAddress(p + 8),
                                         int32_t(rd32(p + 0x18))));
                    return CallState::Completed;
                }
                case 2: // inet#7 Open(cid, ms) (0x4e4)
                {
                    const int32_t cid = w(0), ms = w(1);
                    const int32_t r = stack_.open(cid, 0);
                    if (r == inet::Timeout && ms != 0)
                        return defer(b, replySize, later, [this, cid, ms](std::vector<uint8_t> &d) {
                            wr32(d.data(), uint32_t(stack_.open(cid, ms)));
                        });
                    set(0, r);
                    return CallState::Completed;
                }
                case 3: // inet#8 Close(cid, ms) (0x4fc)
                    set(0, stack_.close(w(0), w(1)));
                    return CallState::Completed;
                case 4: // inet#9 Recv(cid, data @+8, count [2], flags @+4, ms [3]) (0x514)
                {
                    const int32_t cid = w(0), n = count(w(2), 8), ms = w(3);
                    int32_t flags = w(1);
                    const int32_t r = stack_.recv(cid, b.data() + 8, n, flags, 0);
                    if (r == inet::Timeout && ms != 0)
                        return defer(b, replySize, later, [this, cid, n, ms](std::vector<uint8_t> &d) {
                            int32_t f = int32_t(rd32(d.data() + 4));
                            wr32(d.data(), uint32_t(stack_.recv(cid, d.data() + 8, n, f, ms)));
                            wr32(d.data() + 4, uint32_t(f));
                        });
                    set(0, r);
                    set(1, flags);
                    return CallState::Completed;
                }
                case 5: // inet#10 Send(cid, data @+0x10, count [2], flags @+4, ms [3]) (0x56c)
                {
                    const int32_t cid = w(0), n = count(w(2), 0x10), ms = w(3);
                    int32_t flags = w(1);
                    if (ms != 0 && !stack_.isUdpLike(cid) && stack_.known(cid))
                        return defer(b, replySize, later, [this, cid, n, ms](std::vector<uint8_t> &d) {
                            int32_t f = int32_t(rd32(d.data() + 4));
                            wr32(d.data(), uint32_t(stack_.send(cid, d.data() + 0x10, n, f, ms)));
                            wr32(d.data() + 4, uint32_t(f));
                        });
                    set(0, stack_.send(cid, b.data() + 0x10, n, flags, ms));
                    set(1, flags);
                    return CallState::Completed;
                }
                case 6: // inet#4 Name2Address(flags, address @+4, name @+0x14 if [4] > 0, ms, nretry) (0x450)
                {
                    const int32_t length = w(4);
                    uint32_t address = 0;
                    int32_t r = inet::Invalid;
                    if (length > 0 && room(0x14))
                    {
                        const char *name = reinterpret_cast<const char *>(b.data() + 0x14);
                        r = stack_.name2Address(std::string(name, strnlen(name, std::min<size_t>(room(0x14), size_t(length)))),
                                                address);
                    }
                    if (r == 0)
                        writeInetAddress(b.data() + 4, address);
                    set(0, r);
                    return CallState::Completed;
                }
                case 7: // inet#5 Address2String(text @+0x14, len [0], address @+4) (0x488)
                {
                    const std::string text = formatIpv4(readInetAddress(b.data() + 4));
                    const int32_t length = w(0);
                    if (length < int32_t(text.size() + 1) || room(0x14) < text.size() + 1)
                        set(0, inet::Invalid);
                    else
                    {
                        std::memcpy(b.data() + 0x14, text.c_str(), text.size() + 1);
                        set(0, 0);
                    }
                    return CallState::Completed;
                }
                case 8: // inet#24 GetInterfaceList(ids @+4, n [0]) (0x608)
                {
                    const int32_t n = w(0);
                    int32_t listed = 0;
                    if (n >= 1 && room(4) >= 4)
                    {
                        wr32(b.data() + 4, InterfaceId);
                        listed = 1;
                    }
                    set(0, listed);
                    return CallState::Completed;
                }
                case 9: // inet#25 InterfaceControl(id, code, data @+0xc or NULL, len) (0x61c)
                {
                    const int32_t length = w(2);
                    if (length < 0 || size_t(length) > room(0xc))
                        set(0, ErrInetArgument);
                    else
                        set(0, interfaceControl(w(0), w(1), length ? b.data() + 0xc : nullptr, length));
                    return CallState::Completed;
                }
                case 10: // inet#27 GetRoutingTable: no routes [I entry layout unknown; no GT4 caller]
                case 11: // inet#30 GetNameServers: none (no DNS, spec 3.4)
                case 12: // inet#36 ChangeThreadPriority
                case 16: // inet#41
                case 17: // inet#38 GetLog: no log
                    set(0, 0);
                    return CallState::Completed;
                case 13: // inet#12 RecvFrom(cid, data @+0x1c, count [2], flags @+4, address @+8, port @+0x18, ms [3]) (0x538)
                {
                    const int32_t cid = w(0), n = count(w(2), 0x1c), ms = w(3);
                    int32_t flags = w(1), port = 0;
                    uint32_t from = 0;
                    const int32_t r = stack_.recvFrom(cid, b.data() + 0x1c, n, flags, from, port, 0);
                    if (r == inet::Timeout && ms != 0)
                        return defer(b, replySize, later, [this, cid, n, ms](std::vector<uint8_t> &d) {
                            int32_t f = int32_t(rd32(d.data() + 4)), p = 0;
                            uint32_t a = 0;
                            const int32_t got = stack_.recvFrom(cid, d.data() + 0x1c, n, f, a, p, ms);
                            if (got >= 0)
                            {
                                writeInetAddress(d.data() + 8, a);
                                wr32(d.data() + 0x18, uint32_t(p));
                            }
                            wr32(d.data(), uint32_t(got));
                            wr32(d.data() + 4, uint32_t(f));
                        });
                    if (r >= 0)
                    {
                        writeInetAddress(b.data() + 8, from);
                        wr32(b.data() + 0x18, uint32_t(port));
                    }
                    set(0, r);
                    set(1, flags);
                    return CallState::Completed;
                }
                case 14: // inet#13 SendTo(cid, data @+0x24, count [2], flags @+4, address @+0x14, port [4], ms [3]) (0x590)
                {
                    const int32_t cid = w(0), n = count(w(2), 0x24), ms = w(3), port = w(4);
                    const uint32_t to = readInetAddress(b.data() + 0x14);
                    int32_t flags = w(1);
                    if (ms != 0 && !stack_.isUdpLike(cid) && stack_.known(cid))
                        return defer(b, replySize, later, [this, cid, n, ms](std::vector<uint8_t> &d) {
                            int32_t f = int32_t(rd32(d.data() + 4));
                            wr32(d.data(), uint32_t(stack_.send(cid, d.data() + 0x24, n, f, ms)));
                            wr32(d.data() + 4, uint32_t(f));
                        });
                    set(0, stack_.sendTo(cid, b.data() + 0x24, n, flags, to, port, ms));
                    set(1, flags);
                    return CallState::Completed;
                }
                case 15: // inet#11 Abort(cid, flags) (0x5c4)
                    set(0, stack_.abort(w(0), w(1)));
                    return CallState::Completed;
                case 18: // inet#14 Address2Name: no reverse lookup (no DNS) [I code]
                    set(0, inet::Timeout);
                    return CallState::Completed;
                case 19: // inet#15 Control(cid, code, data @+0xc or NULL, len) (0x5dc)
                {
                    const int32_t length = w(2);
                    if (length < 0 || size_t(length) > room(0xc))
                        set(0, inet::Invalid);
                    else
                        set(0, stack_.control(w(0), w(1), length ? b.data() + 0xc : nullptr, length));
                    return CallState::Completed;
                }
                case 20: // inet#16 Poll(fds @+8, nfds [0], ms [1]) (0x438)
                {
                    const int32_t n = static_cast<int32_t>(std::clamp<int64_t>(w(0), 0, int64_t(room(8) / 8)));
                    const int32_t ms = w(1);
                    auto run = [this, n](std::vector<uint8_t> &d, int32_t wait) {
                        std::vector<PollEntry> entries(static_cast<size_t>(n));
                        for (int32_t i = 0; i < n; ++i)
                        {
                            const uint8_t *e = d.data() + 8 + 8 * size_t(i);
                            entries[i].cid = int32_t(rd32(e));
                            entries[i].events = int16_t(e[4] | e[5] << 8);
                        }
                        const int32_t r = stack_.poll(entries, wait);
                        for (int32_t i = 0; i < n; ++i)
                        {
                            uint8_t *e = d.data() + 8 + 8 * size_t(i);
                            e[6] = uint8_t(entries[i].revents);
                            e[7] = uint8_t(uint16_t(entries[i].revents) >> 8);
                        }
                        wr32(d.data(), uint32_t(r));
                        return r;
                    };
                    if (run(b, 0) == 0 && ms != 0)
                        return defer(b, replySize, later, [run, ms](std::vector<uint8_t> &d) { run(d, ms); });
                    return CallState::Completed;
                }
                case 30: // libnet init: reset the event ring, first user registers the handler (0x6fc)
                {
                    std::lock_guard lock(ctlMutex_);
                    events_.clear();
                    eventsEver_ = false;
                    ifids_ = {0, 0};
                    if (++refcount_ == 1)
                        handler_ = true;
                    set(0, 0);
                    return CallState::Completed;
                }
                case 31: // libnet term (0x7bc)
                {
                    std::lock_guard lock(ctlMutex_);
                    if (--refcount_ == 0)
                        handler_ = false;
                    set(0, 0);
                    return CallState::Completed;
                }
                case 32: // inetctl#4 SetConfiguration(env) (0x83c)
                    set(0, setConfiguration(uint32_t(w(0))));
                    return CallState::Completed;
                case 33: // wait for class 1 (interface) (0x850)
                case 34: // wait for class 4 (address) (0x8bc)
                {
                    const int32_t wanted = fn == 33 ? 1 : 4;
                    return defer(b, replySize, later, [this, wanted](std::vector<uint8_t> &d) { waitClass(wanted, d); });
                }
                case 35: // next event {result, id, type} (0x928)
                    return defer(b, replySize, later, [this](std::vector<uint8_t> &d) {
                        int32_t id = 0, type = 0;
                        const int32_t r = nextEvent(id, type);
                        wr32(d.data(), uint32_t(r));
                        wr32(d.data() + 4, uint32_t(id));
                        wr32(d.data() + 8, uint32_t(type));
                    });
                case 50: // inetctl#5 Up(id)
                case 51: // inetctl#6 Down(id)
                case 52: // inetctl#7 SetAutoMode(flag)
                    set(0, interfaceCommand(fn, w(0)));
                    return CallState::Completed;
                case 53: // inetctl#10 GetState(id, state @+4): -1 and no state for an unknown id (0x3288) [C]
                {
                    std::lock_guard lock(ctlMutex_);
                    if (w(0) == InterfaceId)
                    {
                        set(0, 0);
                        set(1, ctlState_);
                    }
                    else
                        set(0, ErrNoInterface);
                    return CallState::Completed;
                }
                case 0x64:
                case 0x65:
                    asyncOpen(fn == 0x64, b);
                    return CallState::Completed;
                case 0x66:
                    return asyncClose(b, replySize, later);
                case 0x67:
                    dump();
                    return CallState::Completed;
                default:
                    ++unhandled_;
                    host_.log(LogLevel::Warning, "[GT4/NET] RTINETB fn " + hex(fn) +
                                                     ": UNHANDLED CASE (libnetb -0x21d, RTINETB 0x111c)");
                    set(0, ErrUnhandled);
                    return CallState::Completed;
                }
            }

            CallState defer(std::vector<uint8_t> &buffer, size_t replySize, Completion &later,
                            std::function<void(std::vector<uint8_t> &)> work)
            {
                ++deferred_;
                auto data = std::make_shared<std::vector<uint8_t>>(std::move(buffer));
                spawn([data, replySize, done = std::move(later), work = std::move(work)]() mutable {
                    work(*data);
                    data->resize(replySize);
                    if (done)
                        done(std::move(*data));
                });
                return CallState::Deferred;
            }

            void noteFirstCall(uint32_t fn)
            {
                if (stack_.config().log < 1)
                    return;
                {
                    std::lock_guard lock(logMutex_);
                    if (!seenFunctions_.insert(fn).second)
                        return;
                }
                host_.log(LogLevel::Info, "[GT4/NET] RTINETB fn " + std::to_string(fn) + " (" + functionName(fn) +
                                              ") first call");
            }

            // ------------------------------------------- INETCTL / interface model

            uint32_t interfaceFlagsLocked() const
            {
                uint32_t f = DevAttached;
                if (started_)
                    f |= DevUp | DevRunning | (dhcp_ ? DevDhcp : 0u);
                return f;
            }

            void pushEventLocked(int32_t id, int32_t type)
            {
                // RTINETB's handler (0x0) queues {id, type} in a 16-entry ring [C].
                if (!handler_)
                    return;
                events_.emplace_back(id, type);
                if (events_.size() > 16)
                    events_.pop_front();
                eventsEver_ = true;
                ctlCv_.notify_all();
            }

            void startLocked()
            {
                if (started_)
                    return;
                started_ = true;
                ctlState_ = CtlStarted;
                // INETCTL's thread forwards Start (3) once INET reports the
                // interface running (flag bit 1 rises, INETCTL 0x2824-0x2848)
                // [C number, I timing: at once, no DHCP wait].
                pushEventLocked(InterfaceId, EvStart);
                const Config &c = stack_.config();
                std::string asked = settings_.address.empty() ? "" : "; GT4's static " + settings_.address + " not used";
                host_.log(LogLevel::Info, "[GT4/NET] interface up: " + formatIpv4(c.address) + "/" +
                                              formatIpv4(c.netmask) + " (GT4 asked " +
                                              (configured_ ? settings_.describe() : std::string("no configuration")) +
                                              asked + ")");
            }

            int32_t setConfiguration(uint32_t env)
            {
                InterfaceSettings s;
                const bool known = env_.get(env, s);
                std::lock_guard lock(ctlMutex_);
                configured_ = true;
                settings_ = known ? s : InterfaceSettings{};
                dhcp_ = !known || s.dhcp != 0;
                if (!known)
                    host_.log(LogLevel::Info, "[GT4/NET] SetConfiguration env " + hex(env) +
                                                  " is not a PDINETCNF env; taken as DHCP");
                // inetctl#4 drops INETCTL's interface list (0x26c4) and its thread
                // attaches every interface again: 0x203c with Attach (1) forwards
                // Conf (6) then Attach (1) for a configured interface (0x21dc /
                // 0x23fc), whatever the auto mode; auto mode also starts it (0x21bc) [C].
                pushEventLocked(InterfaceId, EvConf);
                pushEventLocked(InterfaceId, EvAttach);
                if (autoMode_)
                    startLocked();
                return 0;
            }

            int32_t interfaceCommand(uint32_t fn, int32_t argument)
            {
                std::lock_guard lock(ctlMutex_);
                if (fn == 52)
                {
                    autoMode_ = argument != 0;
                    if (autoMode_ && configured_)
                        startLocked();
                    return 0;
                }
                // inetctl#5 / #6 -> 0x2f5c: id 0 means every interface; an id that
                // matches none does nothing and still returns 0; Up without a
                // configuration returns 0 at once (0x2f7c-0x2f98) [C].
                if (argument != 0 && argument != InterfaceId)
                    return 0;
                if (fn == 50)
                {
                    if (!configured_)
                        return 0;
                    pushEventLocked(InterfaceId, EvUp); // 0x22c4 starts, 0x23fc forwards 8
                    startLocked();
                    return 0;
                }
                pushEventLocked(InterfaceId, EvDown); // 0x2294 / 0x23fc forward 9
                if (started_)
                {
                    started_ = false;
                    ctlState_ = CtlIdle;
                    pushEventLocked(InterfaceId, EvStop); // bit 1 falls: 0x2844 -> Stop (4)
                    host_.log(LogLevel::Info, "[GT4/NET] interface down");
                }
                return 0;
            }

            // Port of 0x1ec: before any event, up to 5 x 300 ms then -0x220;
            // after the first one the event-count semaphore is waited forever [C].
            int32_t nextEvent(int32_t &id, int32_t &type)
            {
                id = 0;
                type = 0;
                std::unique_lock lock(ctlMutex_);
                if (!eventsEver_ && !ctlCv_.wait_for(lock, 1500ms, [&] { return eventsEver_ || stopping_.load(); }))
                    return ErrEvent;
                ctlCv_.wait(lock, [&] { return !events_.empty() || stopping_.load(); });
                if (events_.empty())
                    return ErrEvent;
                id = events_.front().first;
                type = events_.front().second;
                events_.pop_front();
                return 0;
            }

            // Port of the classifier 0x358 (table 0x3bb0) with 0x110 / 0x194 [C].
            int32_t classifyLocked(int32_t id, int32_t type)
            {
                const bool pppoe = (interfaceFlagsLocked() & DevPppoe) != 0;
                switch (type)
                {
                case 1:
                    return !pppoe && ifids_[0] == id ? 1 : -1;
                case 2:
                case 5:
                    return ifids_[0] == id ? 0x100 : -1;
                case 3:
                    return 4;
                case 6:
                    if (pppoe)
                    {
                        if (ifids_[1] == 0)
                            ifids_[1] = id;
                    }
                    else
                        ifids_[0] = id;
                    return -1;
                case 7:
                    return ifids_[0] == 0 ? 0x100 : -1;
                case 9:
                    return 0x100;
                default:
                    return -1;
                }
            }

            // fn 33 / 34 (0x850 / 0x8bc): {class, ids} / {-0x220, type, id} / {error, id}.
            void waitClass(int32_t wanted, std::vector<uint8_t> &d)
            {
                for (;;)
                {
                    int32_t id = 0, type = 0;
                    const int32_t r = nextEvent(id, type);
                    if (r < 0)
                    {
                        wr32(d.data(), uint32_t(r));
                        wr32(d.data() + 4, uint32_t(id));
                        return;
                    }
                    std::lock_guard lock(ctlMutex_);
                    const int32_t c = classifyLocked(id, type);
                    if (c == 0x100)
                    {
                        wr32(d.data(), uint32_t(ErrEvent));
                        wr32(d.data() + 4, uint32_t(type));
                        wr32(d.data() + 8, uint32_t(id));
                        return;
                    }
                    if (c == wanted)
                    {
                        wr32(d.data(), uint32_t(wanted));
                        wr32(d.data() + 4, uint32_t(ifids_[0]));
                        wr32(d.data() + 8, uint32_t(ifids_[1]));
                        return;
                    }
                }
            }

            // inet#25 codes as INET 0x10418 implements them for SMAP's netdev [C
            // lengths and fields; values from SMAP 0x1d70 [C] or marked I].
            int32_t interfaceControl(int32_t id, int32_t code, uint8_t *data, int32_t length)
            {
                if (id != InterfaceId)
                    return ErrInetArgument;
                std::lock_guard lock(ctlMutex_);
                const Config &c = stack_.config();
                auto text = [&](const char *s) {
                    const size_t n = std::strlen(s) + 1;
                    if (!data || size_t(length) < n)
                        return ErrInetArgument;
                    std::memcpy(data, s, n);
                    return 0;
                };
                auto fixed = [&](int32_t need) {
                    if (!data || length != need)
                        return false;
                    std::memset(data, 0, size_t(need));
                    return true;
                };
                switch (code)
                {
                case 0: // 9-byte interface name [I value]
                    if (!fixed(9))
                        return ErrInetArgument;
                    std::memcpy(data, "smap0", 5);
                    return 0;
                case 1: return text("smap");
                case 2: return text("SCE");
                case 3: return text("Ethernet (Network Adaptor)");
                case 4: // netdev +0x20 = 5
                    if (!fixed(1))
                        return ErrInetArgument;
                    data[0] = 5;
                    return 0;
                case 5: // 31-byte string at +0x21 (unset)
                    return fixed(0x1f) ? 0 : ErrInetArgument;
                case 6: // +0x40 = 2
                    if (!fixed(2))
                        return ErrInetArgument;
                    data[0] = 2;
                    return 0;
                case 7:
                    return fixed(2) ? 0 : ErrInetArgument;
                case 8: // flags
                    if (!fixed(4))
                        return ErrInetArgument;
                    wr32(data, interfaceFlagsLocked());
                    return 0;
                case 9:  // address (+0x70)
                case 10: // netmask (+0x74) [I name]
                case 11: // broadcast (+0x78) [I name]
                {
                    if (!fixed(16))
                        return ErrInetArgument;
                    const uint32_t value = code == 9 ? c.address : code == 10 ? c.netmask : c.broadcast();
                    writeInetAddress(data, started_ ? value : 0u);
                    return 0;
                }
                case 12: // +0x84 [I: MTU]
                    if (!fixed(4))
                        return ErrInetArgument;
                    wr32(data, 1500u);
                    return 0;
                case 13: // 16 bytes at +0x88 [I: hardware address]
                    if (!fixed(16))
                        return ErrInetArgument;
                    std::memcpy(data, c.mac.data(), c.mac.size());
                    return 0;
                case 14: // stored block +0x98, length +0x198 (none) -> 0 bytes
                    return length <= 0x100 ? 0 : ErrInetArgument;
                case 15:
                    return fixed(4) ? 0 : ErrInetArgument;
                case 0x10000: // INETCTL up / down (INET 0x1090c / 0x1091c)
                case 0x10001:
                    return 0;
                default:
                    if (loggedCodes_.insert(code).second)
                        host_.log(LogLevel::Info, "[GT4/NET] InterfaceControl code " + hex(uint32_t(code)) +
                                                      " length " + std::to_string(length) + " not provided");
                    return ErrInetArgument;
                }
            }

            // ------------------------------------------------ IOP / EE memory

            bool readIop(uint32_t address, void *data, size_t size) const
            {
                return size == 0 || host_.readIopMemory(address, data, size) || host_.readGuest(address, data, size);
            }

            std::array<uint8_t, 0x40> header(const AsyncHandle &h) const
            {
                std::array<uint8_t, 0x40> x{};
                readIop(h.address, x.data(), x.size());
                return x;
            }

            bool commit(const Batch &batch)
            {
                if (batch.writes.empty())
                    return true;
                std::vector<IopMemoryWrite> list;
                list.reserve(batch.writes.size());
                for (const auto &[address, bytes] : batch.writes)
                    list.push_back({address, std::span<const uint8_t>(bytes)});
                if (host_.commitIopAndEeMemory(list))
                    return true;
                bool ok = true;
                for (const auto &[address, bytes] : batch.writes)
                    ok = host_.writeGuest(address, bytes.data(), bytes.size()) && ok;
                if (!commitFallbackLogged_.exchange(true))
                    host_.log(LogLevel::Warning, "[GT4/NET] host has no atomic IOP/EE commit; writing in order");
                return ok;
            }

            // Posts the handle's 16-byte status to the EE descriptor (0x126c(handle, +0x20, 0x10)).
            void postStatus(AsyncHandle &h, std::array<uint8_t, 0x40> &hd, Batch &batch)
            {
                batch.add(h.address, hd.data(), 0x10);
                batch.add(h.eeDescriptor, hd.data(), 0x10);
            }

            // ------------------------------------------------ async handles

            void asyncOpen(bool receive, std::vector<uint8_t> &b)
            {
                uint8_t *d = b.data();
                const uint32_t ringSize = rd32(d + 0x18);
                const uint32_t block = ringSize && ringSize <= 0x100000u ? host_.allocateIopMemory(ringSize + 0x40u) : 0u;
                if (!block)
                {
                    host_.log(LogLevel::Error, "[GT4/NET] ERROR: libnetb - Couldn't allocate Async Inet handler! (ring " +
                                                   hex(ringSize) + ")");
                    wr32(d + 0x20, 0);
                    return;
                }
                auto h = std::make_shared<AsyncHandle>();
                h->receive = receive;
                h->address = block;
                h->ring = block + 0x40u;
                h->ringSize = ringSize;
                h->cid = int32_t(rd32(d + 0x10));
                h->type = int32_t(rd32(d + 0x34));
                h->eeDescriptor = rd32(d + 0x20);
                h->eeRing = receive ? rd32(d + 0x1c) : 0u;
                std::array<uint8_t, 0x40> hd{};
                std::memcpy(hd.data(), d, 0x40);
                wr32(hd.data() + 0x28, 0);
                wr32(hd.data() + 0x24, 0);
                wr32(hd.data() + 0x1c, h->ring);
                if (receive)
                {
                    wr32(hd.data() + 0x30, 0);
                    wr32(hd.data() + 0x2c, h->eeRing);
                }
                bool ok = true;
                Batch batch;
                std::array<uint8_t, 0x40> first{};
                if (h->udpLike())
                {
                    // 0x19a8 / 0x1a58: the ring must hold one largest datagram and
                    // 0x342c lays one free record over all of it (ring >= 0x80) [C].
                    const uint32_t maxSize = rd32(d + 8);
                    ok = ringSize >= uint64_t(maxSize) + 0x40u && ringSize >= 0x80u;
                    wr32(first.data() + 8, ringSize - 0x40u);
                    wr32(first.data() + 0xc, ringSize - 0x40u);
                }
                if (ok)
                {
                    h->threadId = nextThreadId_++;
                    wr32(hd.data() + 0x28, uint32_t(h->threadId));
                    batch.add(h->address, hd.data(), hd.size());
                    if (h->udpLike())
                        batch.add(h->ring, first.data(), first.size());
                    ok = commit(batch);
                }
                if (!ok)
                {
                    host_.freeIopMemory(block);
                    host_.log(LogLevel::Error,
                              "[GT4/NET] ERROR: libnetb - Couldn't create Async Inet listener thread! (cid " +
                                  hex(uint32_t(h->cid)) + " ring " + hex(ringSize) + " max " + hex(rd32(d + 8)) + ")");
                    wr32(d + 0x20, 0);
                    return;
                }
                if (receive)
                    h->thread = std::thread([this, h] { h->udpLike() ? udpReceive(h) : tcpReceive(h); });
                else
                    h->thread = std::thread([this, h] { h->udpLike() ? udpSend(h) : tcpSend(h); });
                {
                    // Published after the thread object exists (close / reset join it).
                    std::lock_guard lock(handlesMutex_);
                    (receive ? receivers_ : senders_).push_back(h);
                }
                wr32(d + 0x20, block);
                if (!receive)
                {
                    wr32(d + 0x2c, h->ring);
                    wr32(d + 0x28, uint32_t(h->threadId));
                }
                stack_.logLine(1, std::string("async ") + (receive ? "receive" : "send") + " cid " +
                                      hex(uint32_t(h->cid)) + " type " + std::to_string(h->type) + " handle " +
                                      hex(block) + " ring " + hex(ringSize) +
                                      (receive ? " EE ring " + hex(h->eeRing) : std::string()));
            }

            // Port of 0x3360 with a2 = 0 (0x3718 walks wrap markers) for UDP, and
            // the TCP "bytes waiting" test.
            bool sendPending(const AsyncHandle &h)
            {
                if (!h.udpLike())
                    return int32_t(rd32(header(h).data())) > 0;
                std::vector<uint8_t> ring(h.ringSize);
                if (!readIop(h.ring, ring.data(), ring.size()))
                    return false;
                const uint32_t start = h.offset.load();
                uint32_t at = start;
                for (uint32_t guard = 0; guard < h.ringSize / 0x40u + 1; ++guard)
                {
                    if (at + 0x40u > ring.size())
                        return false;
                    const uint8_t *r = ring.data() + at;
                    if (rd32(r + 4) != 1)
                        return rd32(r) == 1 && rd32(r + 0x3c) == 1;
                    at += 0x40u + rd32(r + 8);
                    if (at >= h.ringSize)
                        at = 0;
                    if (at == start)
                        return false;
                }
                return false;
            }

            std::shared_ptr<AsyncHandle> findHandle(std::vector<std::shared_ptr<AsyncHandle>> &list, int32_t cid)
            {
                for (const auto &h : list)
                    if (h->cid == cid && !h->closing)
                        return h;
                return nullptr;
            }

            // fn 0x66 (0xdc4): wait (up to ms) for the send queue, close, end both
            // threads. The module deletes both threads before it replies
            // (0x1b08 / 0x1bfc), after which CORE frees the descriptors and rings
            // (0x570468-0x570478): so the reply waits here until neither thread
            // can write EE memory again. That wait runs on a worker, never on
            // the caller's thread (a ring thread may be waiting for the host's
            // call lock to commit).
            CallState asyncClose(std::vector<uint8_t> &b, size_t replySize, Completion &later)
            {
                const int32_t cid = int32_t(rd32(b.data())), ms = int32_t(rd32(b.data() + 4));
                std::shared_ptr<AsyncHandle> receiver, sender;
                {
                    std::lock_guard lock(handlesMutex_);
                    receiver = findHandle(receivers_, cid);
                    sender = findHandle(senders_, cid);
                    if (receiver && sender)
                    {
                        // Still listed (SIF DMA keeps waking the draining sender),
                        // but never found or closed again.
                        receiver->closing = true;
                        sender->closing = true;
                    }
                }
                if (!receiver)
                {
                    wr32(b.data(), uint32_t(stack_.close(cid, ms)));
                    return CallState::Completed;
                }
                if (!sender)
                    return CallState::Completed; // 0xe04: reply left as it came [C]
                return defer(b, replySize, later, [this, cid, ms, receiver, sender](std::vector<uint8_t> &d) {
                    // 0xe54-0xe8c: DelayThread(100) steps while ms * 1000 us last
                    // (ms < 0: no limit). A wall-clock deadline here: a 100 us host
                    // sleep lasts 1-16 ms on Windows, so counting steps would stretch
                    // rt_comm's 500 ms close (CORE 0x522acc) to seconds.
                    const auto deadline = Clock::now() + std::chrono::milliseconds(ms > 0 ? ms : 0);
                    while (ms != 0 && !stopping_ && sendPending(*sender) && (ms < 0 || Clock::now() < deadline))
                        sender->sleepSince(sender->ticket(), 1ms);
                    receiver->stop = true;
                    sender->stop = true;
                    receiver->notify();
                    sender->notify();
                    const int32_t r = stack_.close(cid, ms);
                    for (const auto &h : {receiver, sender})
                    {
                        if (h->thread.joinable())
                            h->thread.join();
                        host_.freeIopMemory(h->address);
                    }
                    {
                        std::lock_guard lock(handlesMutex_);
                        std::erase(receivers_, receiver);
                        std::erase(senders_, sender);
                    }
                    stack_.logLine(1, "async close cid " + hex(uint32_t(cid)) + " -> " + std::to_string(r));
                    wr32(d.data(), uint32_t(r));
                });
            }

            void dump()
            {
                std::lock_guard lock(handlesMutex_);
                host_.log(LogLevel::Info, "[GT4/NET] Displaying LIBNETB's State: " + std::to_string(receivers_.size()) +
                                              " receive / " + std::to_string(senders_.size()) + " send handles");
                for (const auto *list : {&receivers_, &senders_})
                    for (const auto &h : *list)
                        host_.log(LogLevel::Info, std::string("[GT4/NET]   ") + (h->receive ? "recv" : "send") +
                                                      " cid " + hex(uint32_t(h->cid)) + " type " +
                                                      std::to_string(h->type) + " handle " + hex(h->address) +
                                                      " offset " + hex(h->offset.load()));
            }

            // ---------------------------------------- UDP receive (0x156c, 0x28f4)

            // Sum of consecutive free records from `at` (port of 0x3474) [C], kept
            // inside the ring.
            static uint32_t freeRun(const std::vector<uint8_t> &ring, uint32_t at, uint32_t remaining)
            {
                uint32_t total = 0;
                while (remaining >= 0x40u && at + 0x40u <= ring.size())
                {
                    const uint8_t *r = ring.data() + at;
                    if (rd32(r) != 0 || rd32(r + 0x3c) != 0)
                        break;
                    const uint32_t step = rd32(r + 8) + 0x40u;
                    total += step;
                    if (step > remaining)
                        break;
                    remaining -= step;
                    at += step;
                }
                return total;
            }

            // Port of 0x34dc: room for one datagram of up to maxSize at the write
            // offset, writing a wrap marker / split records into both rings.
            // Returns false when the offset is out of range.
            bool reserveSlot(AsyncHandle &h, uint32_t maxSize, bool &ok)
            {
                uint32_t needed = maxSize + 0x40u;
                if (needed & 0x3fu)
                    needed = maxSize + 0x80u - (needed & 0x3fu);
                for (int pass = 0; pass < 2; ++pass)
                {
                    const uint32_t at = h.offset.load();
                    if (at >= h.ringSize)
                        return false;
                    std::vector<uint8_t> ring(h.ringSize);
                    if (!readIop(h.ring, ring.data(), ring.size()))
                        return false;
                    uint8_t *rec = ring.data() + at;
                    if (rd32(rec) != 0 || rd32(rec + 0x3c) != 0)
                    {
                        ok = false;
                        return true;
                    }
                    const uint32_t remaining = h.ringSize - at;
                    const uint32_t free = freeRun(ring, at, remaining);
                    if (free < needed)
                    {
                        if (remaining - free < 0x40u && at != 0)
                        {
                            wr32(rec, 1);
                            wr32(rec + 4, 1);
                            wr32(rec + 8, remaining - 0x40u);
                            wr32(rec + 0xc, remaining - 0x40u);
                            wr32(rec + 0x2c, h.ring + at);
                            wr32(rec + 0x3c, 1);
                            Batch batch;
                            batch.add(h.ring + at, rec, 0x40);
                            batch.add(h.eeRing + at, rec, 0x40);
                            commit(batch);
                            ++ringWraps_;
                            h.offset = 0;
                            continue;
                        }
                        ok = false;
                        return true;
                    }
                    Batch batch;
                    if (needed < free)
                    {
                        uint8_t *tail = rec + needed;
                        wr32(tail, 0);
                        wr32(tail + 4, 0);
                        wr32(tail + 8, free - needed - 0x40u);
                        wr32(tail + 0xc, free - needed - 0x40u);
                        wr32(tail + 0x3c, 0);
                        batch.add(h.ring + at + needed, tail, 0x40);
                        batch.add(h.eeRing + at + needed, tail, 0x40);
                    }
                    wr32(rec, 0);
                    wr32(rec + 4, 0);
                    wr32(rec + 8, needed - 0x40u);
                    wr32(rec + 0xc, needed - 0x40u);
                    wr32(rec + 0x3c, 0);
                    batch.add(h.ring + at, rec, 0x40);
                    batch.add(h.eeRing + at, rec, 0x40);
                    commit(batch);
                    ok = true;
                    return true;
                }
                ok = false;
                return true;
            }

            // Second half of 0x28f4: one record (payload first, header last).
            void writeRecord(AsyncHandle &h, const std::vector<uint8_t> &payload, int32_t n, int32_t flags,
                             uint32_t from, int32_t port)
            {
                const uint32_t at = h.offset.load();
                uint8_t slot[0x40];
                readIop(h.ring + at, slot, sizeof(slot));
                const uint32_t capacity = rd32(slot + 8);
                const uint32_t bytes = n > 0 ? uint32_t(n) : 0u;
                const uint32_t rounded = roundUp64(bytes);
                if (rounded > capacity || uint64_t(at) + 0x40u + capacity > h.ringSize)
                {
                    host_.log(LogLevel::Error, "[GT4/NET] receive ring of cid " + hex(uint32_t(h.cid)) +
                                                   " inconsistent at " + hex(at) + "; datagram dropped");
                    return;
                }
                const uint32_t leftover = capacity - rounded;
                const uint32_t dma = rounded + (leftover ? 0x40u : 0u);
                std::vector<uint8_t> area(0x40u + dma);
                readIop(h.ring + at, area.data(), area.size()); // stale mirror bytes travel too, as on the IOP
                uint8_t *rec = area.data();
                wr32(rec, 1);
                wr32(rec + 4, 0);
                wr32(rec + 8, rounded);
                wr32(rec + 0xc, bytes);
                writeInetAddress(rec + 0x10, from);
                wr32(rec + 0x20, uint32_t(port));
                wr32(rec + 0x24, uint32_t(flags));
                wr32(rec + 0x2c, h.ring + at);
                wr32(rec + 0x3c, 1);
                if (bytes)
                    std::memcpy(rec + 0x40, payload.data(), bytes);
                if (leftover)
                {
                    uint8_t *tail = rec + 0x40 + rounded;
                    wr32(tail, 0);
                    wr32(tail + 4, 0);
                    wr32(tail + 8, leftover - 0x40u);
                    wr32(tail + 0xc, leftover - 0x40u);
                    wr32(tail + 0x3c, 0);
                }
                Batch batch;
                batch.add(h.ring + at + 0x40u, rec + 0x40, dma);
                batch.add(h.ring + at, rec, 0x40);
                batch.add(h.eeRing + at + 0x40u, rec + 0x40, dma);
                batch.add(h.eeRing + at, rec, 0x40);
                commit(batch);
                uint32_t next = at + 0x40u + rounded;
                if (next >= h.ringSize)
                    next = 0;
                h.offset = next;
                ++ringRecords_;
            }

            void udpReceive(std::shared_ptr<AsyncHandle> h)
            {
                // The EE lays its ring out (CORE 0x571850) after the 0x64 reply and
                // DMAs the descriptor head (CORE 0x5703b0) after opening: records
                // wait for that head so none lands before the EE's ring is ready [I
                // ordering guard; the IOP module does not wait].
                const auto gate = Clock::now();
                while (!h->stop && !h->headWritten && Clock::now() - gate < 250ms)
                    h->sleepSince(h->ticket(), 5ms);
                std::vector<uint8_t> payload;
                bool loggedError = false;
                while (!h->stop)
                {
                    const uint64_t ticket = h->ticket();
                    const uint32_t maxSize = rd32(header(*h).data() + 8);
                    bool ok = false;
                    if (!maxSize || uint64_t(maxSize) + 0x40u > h->ringSize || !reserveSlot(*h, maxSize, ok))
                    {
                        if (!loggedError)
                            host_.log(LogLevel::Error, "[GT4/NET] RECV/UDP: Error while receiving data (cid " +
                                                           hex(uint32_t(h->cid)) + ")");
                        loggedError = true;
                        h->sleepSince(ticket, 5ms);
                        continue;
                    }
                    if (!ok)
                    {
                        ++ringFull_; // 0x2978: DelayThread(500), datagrams stay in INET
                        h->sleepSince(ticket, 1ms);
                        continue;
                    }
                    payload.resize(maxSize);
                    int32_t n = 0, flags = 0, port = 0;
                    uint32_t from = 0;
                    do
                        n = stack_.recvFrom(h->cid, payload.data(), int32_t(maxSize), flags, from, port, SliceMs);
                    while (n == inet::Timeout && !h->stop);
                    if (h->stop)
                        return;
                    if (n == 0)
                        continue;
                    if (n == inet::Invalid || n == inet::NotExist)
                    {
                        // The cid was closed under the async receiver (not through
                        // fn 0x66): park instead of filling the ring with empty
                        // error records [I].
                        host_.log(LogLevel::Warning, "[GT4/NET] RECV/UDP: cid " + hex(uint32_t(h->cid)) +
                                                         " gone; async receiver idle");
                        while (!h->stop)
                            h->sleepSince(h->ticket(), 50ms);
                        return;
                    }
                    if (n < 0)
                        host_.log(LogLevel::Warning, "[GT4/NET] RECV/UDP: sceInetRecvFrom() failed, error " +
                                                         std::to_string(n));
                    writeRecord(*h, payload, n, flags, n < 0 ? 0u : from, n < 0 ? 0 : port);
                }
            }

            // ------------------------------------------- UDP send (0x1818, 0x26b8)

            bool recordReady(AsyncHandle &h)
            {
                uint8_t r[0x40];
                return readIop(h.ring + h.offset.load(), r, sizeof(r)) && rd32(r) == 1 && rd32(r + 0x3c) == 1;
            }

            // Port of 0x26b8 (UDP): false when the record stays (-504 or nothing sent).
            bool sendRecord(AsyncHandle &h)
            {
                const uint32_t at = h.offset.load();
                uint8_t rec[0x40];
                if (uint64_t(at) + 0x40u > h.ringSize || !readIop(h.ring + at, rec, sizeof(rec)))
                    return false;
                int32_t result = 0, flagsOut = 0;
                if (rd32(rec + 4) == 0)
                {
                    const uint32_t room = h.ringSize - at - 0x40u;
                    const uint32_t count = std::min(rd32(rec + 0xc), room);
                    std::vector<uint8_t> payload(count);
                    readIop(h.ring + at + 0x40u, payload.data(), count);
                    int32_t flags = 0;
                    const int32_t r = stack_.sendTo(h.cid, payload.data(), int32_t(count), flags,
                                                    readInetAddress(rec + 0x10), int32_t(rd32(rec + 0x20)), 500);
                    if (r == inet::Resources)
                        return false; // retried silently (0x275c)
                    if (r < 0)
                        host_.log(LogLevel::Warning, "[GT4/NET] SEND/UDP: sceInetSendTo() failed, error " +
                                                         std::to_string(r) + " (to " + formatIpv4(readInetAddress(rec + 0x10)) +
                                                         ":" + std::to_string(rd32(rec + 0x20)) + ")");
                    if (r == 0)
                        return false;
                    result = r;
                    flagsOut = flags;
                }
                wr32(rec + 0x28, uint32_t(result));
                wr32(rec + 0x24, uint32_t(flagsOut));
                wr32(rec, 0);
                wr32(rec + 0x3c, 0);
                wr32(rec + 4, 0);
                Batch batch;
                batch.add(h.ring + at, rec, 0x40);
                if (result < 0 || flagsOut != 0)
                {
                    auto hd = header(h);
                    if (int32_t(rd32(hd.data())) >= 0 && result < 0)
                        wr32(hd.data(), uint32_t(result));
                    if (rd32(hd.data() + 4) == 0)
                        wr32(hd.data() + 4, uint32_t(flagsOut));
                    postStatus(h, hd, batch);
                }
                uint32_t next = at + 0x40u + rd32(rec + 8);
                if (next >= h.ringSize)
                    next = 0;
                h.offset = next;
                batch.add(rd32(rec + 0x2c), rec, 0x40); // header back to the EE's record (0x28b8)
                commit(batch);
                ++sentRecords_;
                return true;
            }

            void udpSend(std::shared_ptr<AsyncHandle> h)
            {
                const uint32_t delay = stack_.config().sendDelayMs;
                const auto poll = std::chrono::milliseconds(delay ? delay : 2);
                while (!h->stop)
                {
                    const uint64_t ticket = h->ticket();
                    if (!recordReady(*h))
                    {
                        h->sleepSince(ticket, poll);
                        continue;
                    }
                    if (delay)
                        std::this_thread::sleep_for(std::chrono::milliseconds(delay));
                    while (!h->stop && recordReady(*h))
                    {
                        const uint64_t t = h->ticket();
                        if (!sendRecord(*h))
                            h->sleepSince(t, 1ms);
                        else if (delay)
                            std::this_thread::sleep_for(1ms);
                    }
                }
            }

            // -------------------------------------------- TCP (0x133c, 0x1648)

            // Waits until the EE has taken the buffer (head +0 == 0) or stop.
            bool waitConsumed(AsyncHandle &h)
            {
                while (!h.stop)
                {
                    const uint64_t ticket = h.ticket();
                    if (rd32(header(h).data()) == 0)
                        return true;
                    h.sleepSince(ticket, 1ms);
                }
                return false;
            }

            void tcpReceive(std::shared_ptr<AsyncHandle> h)
            {
                std::vector<uint8_t> buffer(h->ringSize);
                for (;;)
                {
                    while (!h->stop)
                    {
                        const uint64_t ticket = h->ticket();
                        if (rd32(header(*h).data() + 0xc) == 0) // opened (0x139c)
                            break;
                        h->sleepSince(ticket, 1ms);
                    }
                    if (h->stop)
                        return;
                    int32_t flags = int32_t(rd32(header(*h).data() + 0x14));
                    int32_t n = 0;
                    do
                        n = stack_.recv(h->cid, buffer.data(), int32_t(h->ringSize), flags, SliceMs);
                    while (n == inet::Timeout && !h->stop);
                    if (h->stop)
                        return;
                    if (n < 0)
                    {
                        host_.log(LogLevel::Info, "[GT4/NET] RECV/TCP: ASync listen thread exiting due to error " +
                                                      std::to_string(n) + " (cid " + hex(uint32_t(h->cid)) + ")");
                        if (!waitConsumed(*h))
                            return;
                        auto hd = header(*h);
                        wr32(hd.data(), uint32_t(n));
                        wr32(hd.data() + 4, uint32_t(flags));
                        Batch batch;
                        postStatus(*h, hd, batch);
                        commit(batch);
                        return;
                    }
                    if (n == 0)
                    {
                        h->sleepSince(h->ticket(), 2ms); // 0x1544
                        continue;
                    }
                    if (!waitConsumed(*h))
                        return;
                    auto hd = header(*h);
                    wr32(hd.data(), uint32_t(n));
                    wr32(hd.data() + 4, uint32_t(flags));
                    Batch batch;
                    batch.add(h->ring, buffer.data(), size_t(n));
                    batch.add(rd32(hd.data() + 0x2c), buffer.data(), size_t(n)); // 0x14f0: to the EE buffer
                    postStatus(*h, hd, batch);
                    commit(batch);
                    ++ringRecords_;
                    if (flags & inet::FlagFin)
                    {
                        stack_.logLine(1, "RECV/TCP: Socket closed remotely, cid " + hex(uint32_t(h->cid)));
                        return;
                    }
                }
            }

            void tcpSend(std::shared_ptr<AsyncHandle> h)
            {
                const uint32_t delay = stack_.config().sendDelayMs;
                const auto poll = std::chrono::milliseconds(delay ? delay : 2);
                std::vector<uint8_t> buffer;
                while (!h->stop)
                {
                    auto hd = header(*h);
                    if (rd32(hd.data() + 0xc) == 2)
                    {
                        // The EE left the connect to the IOP (ms 0, CORE 0x570350):
                        // open with 10 s and post the state (0x16b4) [C].
                        const int32_t r = stack_.open(h->cid, 10000);
                        if (h->stop)
                            return;
                        hd = header(*h);
                        wr32(hd.data() + 0xc, r < 0 ? 3u : 0u);
                        wr32(hd.data(), uint32_t(r < 0 ? r : 0));
                        if (r < 0)
                            host_.log(LogLevel::Info, "[GT4/NET] SEND/TCP: Open failure (ret = " + std::to_string(r) + ")");
                        Batch batch;
                        postStatus(*h, hd, batch);
                        commit(batch);
                    }
                    const uint64_t ticket = h->ticket();
                    hd = header(*h);
                    const int32_t count = int32_t(rd32(hd.data()));
                    if (count <= 0 || rd32(hd.data() + 0xc) != 0)
                    {
                        h->sleepSince(ticket, poll);
                        continue;
                    }
                    if (delay)
                        std::this_thread::sleep_for(std::chrono::milliseconds(delay));
                    const uint32_t bytes = std::min<uint32_t>(uint32_t(count), h->ringSize);
                    buffer.resize(bytes);
                    readIop(h->ring, buffer.data(), bytes);
                    int32_t flags = int32_t(rd32(hd.data() + 0x14)), r = 0;
                    uint32_t sent = 0;
                    while (sent < bytes && !h->stop)
                    {
                        r = stack_.send(h->cid, buffer.data() + sent, int32_t(bytes - sent), flags, 500);
                        if (r < 0)
                            break;
                        sent += uint32_t(r);
                    }
                    if (h->stop)
                        return;
                    if (r < 0)
                        host_.log(LogLevel::Info, "[GT4/NET] SEND/TCP: sceInetSend() failed, error " + std::to_string(r));
                    hd = header(*h);
                    wr32(hd.data(), uint32_t(r < 0 ? r : 0));
                    Batch batch;
                    postStatus(*h, hd, batch);
                    commit(batch);
                    ++sentRecords_;
                }
            }

            // ------------------------------------------------------ lifetime

            struct Worker
            {
                std::thread thread;
                std::shared_ptr<std::atomic<bool>> done;
            };

            void spawn(std::function<void()> job)
            {
                auto done = std::make_shared<std::atomic<bool>>(false);
                std::list<Worker> finished;
                {
                    std::lock_guard lock(workersMutex_);
                    for (auto it = workers_.begin(); it != workers_.end();)
                    {
                        if (it->done->load())
                        {
                            finished.push_back(std::move(*it));
                            it = workers_.erase(it);
                        }
                        else
                            ++it;
                    }
                    workers_.push_back({std::thread([job = std::move(job), done] {
                                            job();
                                            done->store(true);
                                        }),
                                        done});
                }
                for (auto &w : finished)
                    w.thread.join();
            }

            void shutdownAll()
            {
                stopping_ = true;
                {
                    std::lock_guard lock(ctlMutex_);
                    ctlCv_.notify_all();
                }
                auto listed = [this] {
                    std::vector<std::shared_ptr<AsyncHandle>> all;
                    std::lock_guard lock(handlesMutex_);
                    all.insert(all.end(), receivers_.begin(), receivers_.end());
                    all.insert(all.end(), senders_.begin(), senders_.end());
                    return all;
                };
                for (const auto &h : listed())
                {
                    h->stop = true;
                    h->notify();
                }
                stack_.shutdown(); // blocked INET calls return sceINETE_ABORT
                // Deferred calls first: a closing fn 0x66 joins and frees its own handles.
                for (;;)
                {
                    std::list<Worker> pending;
                    {
                        std::lock_guard lock(workersMutex_);
                        pending.swap(workers_);
                    }
                    if (pending.empty())
                        break;
                    for (auto &w : pending)
                        w.thread.join();
                }
                const auto remaining = listed();
                for (const auto &h : remaining)
                {
                    if (h->thread.joinable())
                        h->thread.join();
                    host_.freeIopMemory(h->address);
                }
                {
                    std::lock_guard lock(handlesMutex_);
                    receivers_.clear();
                    senders_.clear();
                }
                std::lock_guard lock(ctlMutex_);
                events_.clear();
                eventsEver_ = false;
                ifids_ = {0, 0};
                refcount_ = 0;
                handler_ = false;
                autoMode_ = configured_ = started_ = false;
                dhcp_ = true;
                ctlState_ = CtlIdle;
                settings_ = {};
            }

            IopHost &host_;
            Stack &stack_;
            EnvStore &env_;
            std::atomic<bool> stopping_{false};

            mutable std::mutex ctlMutex_;
            std::condition_variable ctlCv_;
            std::deque<std::pair<int32_t, int32_t>> events_; // {id, type}
            bool eventsEver_ = false;
            int32_t refcount_ = 0;
            bool handler_ = false;
            std::array<int32_t, 2> ifids_{0, 0};
            bool autoMode_ = false, configured_ = false, started_ = false, dhcp_ = true;
            int32_t ctlState_ = CtlIdle;
            InterfaceSettings settings_;
            std::set<int32_t> loggedCodes_;

            mutable std::mutex handlesMutex_;
            std::vector<std::shared_ptr<AsyncHandle>> receivers_, senders_;
            std::atomic<int32_t> nextThreadId_{0x40};

            std::mutex workersMutex_;
            std::list<Worker> workers_;

            std::mutex logMutex_;
            std::set<uint32_t> seenFunctions_;
            std::atomic<bool> commitFallbackLogged_{false};
            std::atomic<uint64_t> calls_{0}, deferred_{0}, ringRecords_{0}, sentRecords_{0}, ringFull_{0},
                ringWraps_{0}, unhandled_{0};
        };
    }

    std::unique_ptr<Rtinetb> createRtinetb(IopHost &host, Stack &stack, EnvStore &env)
    {
        return std::make_unique<Service>(host, stack, env);
    }
}
