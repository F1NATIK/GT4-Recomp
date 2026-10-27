#pragma once
#include "ps2x/iop/iop_host.h"
#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace ps2x::iop::gt3 {
using LinkGuid=std::array<uint8_t,8>;
using LinkPacket=std::array<uint8_t,0x80>;

// Stand-in for the IEEE 1394 bus under GT3's PDI1394: UDP between game
// instances. One instance hosts (a fixed port), the others join it by address
// or by LAN broadcast; the host relays packets, so only it needs a reachable
// port for internet play. Every hop is reliable and ordered (acks and
// resends), like the acknowledged 1394 asynchronous writes PDI1394 uses.
// Membership changes are "bus resets" carrying the member list in the host's
// order, which is the same on every instance.
class LinkTransport {
public:
    enum class Mode { Off,Host,Join };
    struct Config {
        Mode mode=Mode::Off;
        std::string host;      // Join: "address[:port]", or empty for LAN discovery
        uint16_t port=13940;   // Host: listen port; Join: default host port
        std::string name;      // shown to the other players (up to 15 characters)
    };
    struct Callbacks {
        std::function<void(const std::vector<LinkGuid>& members)> bus;
        std::function<void(const LinkGuid& from,const LinkPacket& packet)> packet;
        std::function<void(const std::string& line)> log;
        // Host: may a new console join now? (False while a race runs; the
        // joiner is told to wait and keeps asking.) Must not take locks.
        std::function<bool()> admit;
    };
    struct PeerStatus { LinkGuid guid{}; std::string name; double rttMs=0; };
    LinkTransport(Config,LinkGuid self,Callbacks);
    ~LinkTransport();
    LinkTransport(const LinkTransport&)=delete;
    LinkTransport& operator=(const LinkTransport&)=delete;
    bool start();
    void stop();
    // Reliable, ordered per destination. Callbacks never run inside send().
    void send(const LinkPacket& packet,const std::vector<LinkGuid>& to);
    // A 1394 bus reset requested by this node (PDI1394 class-1 control).
    void requestBusReset();
    std::vector<PeerStatus> peers() const;
    static Config configFromEnvironment();
    // The game is closing (window closed, crash, launcher Stop): every live
    // link tells the other consoles at once, so their race goes on with no
    // wait. fromCrash: never blocks on a lock.
    static void goodbyeAll(bool fromCrash=false);
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// Native PDI1394: the two IOP RPC services GT3's EE i.LINK client calls, and
// the events PDI1394 sends back to EE RPC SID 0x45574650. Mirrors the original
// module (gt3-analysis/iop/PDI1394-protocol.md): node table and indexes, the
// three-deep per-console frame rings, the lockstep frame exchange (reply
// 0x3e0 = 8 slots of 0x7c bytes; 0xac1dcafe = another console is ahead,
// 0xdeadbeaf = rings reset), control messages and activation.
class Pdi1394Link {
public:
    static constexpr uint32_t ControlSid=0x4d494650,ExchangeSid=0x53494650,EeSid=0x45574650,ReplyBytes=0x3e0;
    explicit Pdi1394Link(IopHost&);
    ~Pdi1394Link();
    Pdi1394Link(const Pdi1394Link&)=delete;
    Pdi1394Link& operator=(const Pdi1394Link&)=delete;
    bool start(uint32_t replyPacketIop);   // 0x40 bytes of IOP memory for EE reply packets
    void stop();
    // EE executor. The exchange blocks until every active console's frame is
    // in (as the original's WaitSema on the rings), a bus reset, or stop().
    RpcResult handleRpc(const RpcRequest&);
    // EE executor, each vblank: one EE event at a time through the game's
    // own SIF RPC queue for SID 0x45574650.
    void onVBlank();
    // EE executor: the game's SifExecRequest DMAs its reply packet to us
    // when the handler finishes; queue the next event right away.
    void onSifTransfer(const SifTransfer&);
    struct Status { uint32_t consoles=1,activeMask=0,frame=0,eventsDelivered=0; bool running=false; };
    Status status() const;
private:
    struct State;
    std::unique_ptr<State> s_;
};
}
