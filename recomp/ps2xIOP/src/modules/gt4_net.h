#pragma once

// GT4 (SCUS-97328) LAN network services, written from GT4's own modules
// (analysis\phase5\specs\p5b-network.md; disassembly analysis\iop-sys\disasm\gt4):
//   RTINETB.IRX "Libnet 1.20 (libnetb 1.10.0005, RTIME)", SID 0x80001201, reached
//     from CORE through libmrpc (MSIFRPC): 53 INET/INETCTL slots + RTime's
//     asynchronous ring functions 0x64-0x67;
//   PDINETCNF.IRX 'PNCF' (0x504e4346): GT4's interface settings -> IOP env;
//   DEV9 / SMAP presence (FILEIO devctl "dev9x:" 0x4401 / 0x4402).
// INET is mapped onto Winsock. LAN only: every host socket is bound to the one
// configured address (never INADDR_ANY), multicast / off-subnet destinations
// are refused with sceINETE_NO_ROUTE, there is no DNS and no gateway. GT4's
// online services (Medius, DNAS, SVO, UPnP) therefore fail at their first
// lookup or send. GT4_NET=off (default) keeps the "no network adaptor" boot.

#include "gt4_iop.h"

#include <atomic>
#include <condition_variable>
#include <functional>
#include <set>
#include <span>
#include <thread>

namespace ps2x::iop::gt4::net
{
    constexpr uint32_t SidRtinetb = 0x80001201u;
    constexpr uint32_t SidPncf = 0x504e4346u; // 'PNCF'

    enum class Mode : uint32_t
    {
        Off,  // no adaptor (the phase 4 behaviour)
        Loop, // instances on this PC, loopback addresses only (tests)
        Lan,  // the host's private LAN address
    };

    // Configuration hook. Addresses are IPv4 in host byte order.
    struct Config
    {
        Mode mode = Mode::Off;
        uint32_t address = 0;          // bound by every socket and reported to GT4 (code 9)
        uint32_t netmask = 0xffffff00u;
        std::vector<uint32_t> peers;   // loop: other instances (broadcast fan-out targets)
        std::vector<uint32_t> blocked; // refused even inside the subnet (lan: the router)
        std::array<uint8_t, 6> mac{0x02, 0x47, 0x54, 0x34, 0x00, 0x01};
        uint32_t log = 0;              // 0 quiet, 1 sockets and refusals, 2 every datagram
        uint32_t sendDelayMs = 0;      // >0: emulate libnetb's polling (-send_delay=3) for A/B runs

        bool enabled() const { return mode != Mode::Off && address != 0; }
        uint32_t broadcast() const { return (address & netmask) | ~netmask; }
        bool inSubnet(uint32_t a) const { return (a & netmask) == (address & netmask); }
        std::string describe() const;
    };

    // GT4_NET=off|loop|lan; GT4_NET_IP (loop: a 127.x address, default
    // 127.0.0.1 or 127.77.0.(10+GT4_NET_INDEX); lan: a private address, found
    // automatically when unset); GT4_NET_MASK (default 255.255.255.0 in loop);
    // GT4_NET_INDEX 1..16; GT4_NET_PEERS (loop: a count N -> 127.77.0.11..10+N,
    // or a comma list of 127.x addresses); GT4_NET_BLOCK (comma list);
    // GT4_NET_LOG 0..2; GT4_NET_SEND_DELAY_MS 0..100.
    // Unusable settings return Mode::Off and the reason in `error`.
    using Lookup = std::function<const char *(const char *name)>;
    Config configFrom(const Lookup &lookup, std::string &error);
    Config configFromEnvironment(std::string &error);

    bool parseIpv4(std::string_view text, uint32_t &address);
    std::string formatIpv4(uint32_t address);
    inline bool isLoopback(uint32_t a) { return (a >> 24) == 127u; }
    bool isPrivate(uint32_t a); // 10/8, 172.16/12, 192.168/16, 169.254/16

    // sceINETE_* (RTINETB error printer 0x3230, table 0x5a80) [C].
    namespace inet
    {
        constexpr int32_t Timeout = -500, Abort = -501, Busy = -502, LinkDown = -503, Resources = -504,
                          LocalSocket = -505, ForeignSocket = -506, Exists = -507, NotExist = -508,
                          Closing = -509, Reset = -510, Refused = -511, Invalid = -512, InvalidCall = -513,
                          NoRoute = -514;
        // sceInetRecv flag the RTINETB TCP thread reads as "closed remotely" (0x151c) [C bit, I name].
        constexpr int32_t FlagFin = 4;
        // sceInetPoll event bits [I: POSIX values; GT4 has no Poll caller].
        constexpr int16_t PollIn = 0x01, PollOut = 0x04, PollErr = 0x08, PollHup = 0x10, PollNval = 0x20;
    }

    // INET socket types (sceInetParam.type, RTINETB 0x2090-0x20fc) [C].
    enum class SocketType : int32_t
    {
        Udp = 0,
        TcpActive = 1,
        TcpPassive = 2,
        Raw = 3,
    };

    struct PollEntry
    {
        int32_t cid = 0;
        int16_t events = 0;
        int16_t revents = 0;
    };

    struct BoundEndpoint
    {
        int32_t cid = 0;
        uint32_t address = 0;
        uint16_t port = 0;
        bool listening = false;
    };

    // One GT4 instance's INET on Winsock. Blocking calls take INET's ms
    // (-1 forever, 0 no wait) and return early with sceINETE_ABORT when the
    // socket is aborted, closed or the stack shuts down. Thread safe.
    class Stack
    {
    public:
        Stack(IopHost &host, const Config &config);
        ~Stack();
        Stack(const Stack &) = delete;
        Stack &operator=(const Stack &) = delete;

        const Config &config() const { return config_; }
        bool ready() const { return ready_; }

        int32_t create(int32_t type, int32_t localPort, uint32_t remoteAddress, int32_t remotePort);
        int32_t open(int32_t cid, int32_t ms);
        int32_t close(int32_t cid, int32_t ms);
        int32_t recv(int32_t cid, uint8_t *data, int32_t count, int32_t &flags, int32_t ms);
        int32_t recvFrom(int32_t cid, uint8_t *data, int32_t count, int32_t &flags, uint32_t &address,
                         int32_t &port, int32_t ms);
        int32_t send(int32_t cid, const uint8_t *data, int32_t count, int32_t &flags, int32_t ms);
        int32_t sendTo(int32_t cid, const uint8_t *data, int32_t count, int32_t &flags, uint32_t address,
                       int32_t port, int32_t ms);
        int32_t abort(int32_t cid, int32_t flags);
        // code 1 = 0x4c-byte connection info; code 3 = 4-byte option (TCP no-delay [I]).
        int32_t control(int32_t cid, int32_t code, uint8_t *data, int32_t length);
        int32_t poll(std::span<PollEntry> entries, int32_t ms);
        int32_t name2Address(std::string_view name, uint32_t &address);
        // Non-blocking readiness of one connection (async threads, Poll).
        bool known(int32_t cid) const;
        bool isUdpLike(int32_t cid) const;

        void shutdown(); // closes every socket; blocked calls return sceINETE_ABORT
        std::vector<BoundEndpoint> boundEndpoints() const;
        void appendMetrics(std::vector<DebugMetric> &metrics) const;
        void logLine(uint32_t level, const std::string &text) const;

        struct Socket;

    private:
        std::shared_ptr<Socket> find(int32_t cid) const;
        int32_t route(uint32_t address, int32_t port, bool datagram, std::vector<uint32_t> &targets);
        int32_t waitReady(Socket &socket, bool write, int32_t ms, uint32_t abortSeen);
        int32_t ensureConnection(Socket &socket, int32_t ms, uint32_t abortSeen);
        int32_t finishConnect(Socket &socket);
        int32_t tryAccept(Socket &socket);
        void refuse(uint32_t address, const char *why);

        IopHost &host_;
        Config config_;
        bool ready_ = false;
        bool wsa_ = false;
        mutable std::mutex mutex_;
        std::map<int32_t, std::shared_ptr<Socket>> sockets_;
        int32_t nextCid_ = 1;
        std::atomic<bool> stopping_{false};
        std::atomic<uint64_t> generation_{0}; // shutdown() count: ends waits that hold no socket (Poll)
        std::set<uint32_t> refusedLogged_;
        std::set<std::string> refusedNames_;
        std::atomic<uint64_t> datagramsOut_{0}, datagramsIn_{0}, bytesOut_{0}, bytesIn_{0}, refusals_{0},
            inboundDropped_{0};
    };

    // libmrpc call outcome for the runtime's sceSifMCallRpc stub (spec 3.5).
    enum class CallState : uint32_t
    {
        Completed, // `reply` holds receive.size bytes for the EE receive buffer
        Deferred,  // `later(reply)` runs once, on another thread; block the calling EE thread
        Rejected,  // not a call this service answers
    };
    using Completion = std::function<void(std::vector<uint8_t> &&reply)>;

    // RTINETB (SID 0x80001201). handleRpc() is the synchronous path (waits for
    // deferred calls on the calling thread); call() is the libmrpc path.
    class Rtinetb : public Component
    {
    public:
        virtual CallState call(const RpcRequest &request, std::vector<uint8_t> &reply, Completion later) = 0;
        virtual void onSifTransfer(const SifTransfer &transfer) = 0;
        virtual void appendMetrics(std::vector<DebugMetric> &metrics) const = 0;
    };

    // GT4's interface settings as PDINETCNF fn 3 received them (sceNetcnfifData,
    // 0x1340 bytes; offsets checked against CORE 0x4ee7c8-0x4ee9f0 [C]).
    struct InterfaceSettings
    {
        std::string attachIfc, attachDev, dhcpHostName, address, netmask, gateway, dns1, dns2, vendor, product;
        int32_t ifcType = -1, mtu = -1, devType = -1, phyConfig = -1;
        uint8_t dhcp = 0xff;
        std::string describe() const;
    };

    // The IOP env handles PDINETCNF hands out and INETCTL (RTINETB fn 32) uses.
    class EnvStore
    {
    public:
        uint32_t put(IopHost &host, const InterfaceSettings &settings);
        bool get(uint32_t env, InterfaceSettings &settings) const;
        void reset(IopHost &host);

    private:
        mutable std::mutex mutex_;
        uint32_t address_ = 0; // PDINETCNF keeps one static env (0x2370) [C]
        bool synthetic_ = false;
        bool valid_ = false;
        InterfaceSettings settings_;
    };

    // Everything GT4_NET != off adds to the GT4 profile.
    class Network
    {
    public:
        Network(IopHost &host, const Config &config);
        ~Network();
        const Config &config() const { return stack_->config(); }
        Stack &stack() { return *stack_; }
        Rtinetb &rtinetb() { return *rtinetb_; }
        Component &pdinetcnf() { return *pdinetcnf_; }
        void reset();
        void onSifTransfer(const SifTransfer &transfer);
        void appendMetrics(std::vector<DebugMetric> &metrics) const;

    private:
        IopHost &host_;
        EnvStore env_;
        std::unique_ptr<Stack> stack_;
        std::unique_ptr<Rtinetb> rtinetb_;
        std::unique_ptr<Component> pdinetcnf_;
    };

    std::unique_ptr<Rtinetb> createRtinetb(IopHost &host, Stack &stack, EnvStore &env);
    std::unique_ptr<Component> createPdinetcnf(IopHost &host, Stack &stack, EnvStore &env);

    // Process-wide entry for the runtime's libmrpc stubs: the RTINETB of the
    // live GT4 profile when GT4_NET != off, else nullptr. The bind stub
    // succeeds only for SidRtinetb while this is non-null.
    Rtinetb *mrpcService(uint32_t sid);
    void registerMrpcService(Rtinetb *service); // nullptr unregisters

    // Little helpers shared by the network files.
    inline uint32_t rd32(const uint8_t *p)
    {
        return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24;
    }
    inline void wr32(uint8_t *p, uint32_t v)
    {
        p[0] = uint8_t(v);
        p[1] = uint8_t(v >> 8);
        p[2] = uint8_t(v >> 16);
        p[3] = uint8_t(v >> 24);
    }
    // sceInetAddress: 16 bytes, word 0 reserved, IPv4 in bytes 4..7 (network order) [C].
    inline uint32_t readInetAddress(const uint8_t *p)
    {
        return uint32_t(p[4]) << 24 | uint32_t(p[5]) << 16 | uint32_t(p[6]) << 8 | uint32_t(p[7]);
    }
    inline void writeInetAddress(uint8_t *p, uint32_t address)
    {
        std::memset(p, 0, 16);
        p[4] = uint8_t(address >> 24);
        p[5] = uint8_t(address >> 16);
        p[6] = uint8_t(address >> 8);
        p[7] = uint8_t(address);
    }
}
