#include "ps2x/iop/gt3/pdi1394_link.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <map>
#include <mutex>
#include <random>
#include <set>
#include <thread>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
using SocketHandle=SOCKET;
constexpr SocketHandle NoSocket=INVALID_SOCKET;
static void closeSocket(SocketHandle s) { closesocket(s); }
#else
#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
using SocketHandle=int;
constexpr SocketHandle NoSocket=-1;
static void closeSocket(SocketHandle s) { close(s); }
#endif

// Mesh: every console talks to every other console directly. One console,
// the coordinator (the host at first), keeps the member list: it admits
// joiners and announces every change ("bus reset") in its order. If it
// leaves, the next member in that order takes over. Direct links are opened
// by both sides sending to each other (UDP hole punching through home
// routers); a pair that cannot connect directly has its data relayed by the
// coordinator. Each console's data to another console carries its own
// sequence (per pair), so direct and relayed copies are delivered once and in
// order. When a console leaves, the coordinator gathers the last frames each
// remaining console received from it and hands everyone the same set before
// announcing the departure, so every console's race stays identical.

namespace ps2x::iop::gt3 {
namespace {
using Clock=std::chrono::steady_clock;
constexpr uint32_t Magic=0x4b4c3347;   // "G3LK"
constexpr uint8_t Version=2;
constexpr size_t MaxMembers=8,NameBytes=16,FrameHistory=8;
enum Type : uint8_t { Hello=1,Welcome,Reject,Rel,Ack,Ping,Pong,Bye,Discover,Offer,Link };
enum Kind : uint8_t { BusKind=1,DataKind,RelayKind,ResetKind,LeaveQueryKind,LeaveStateKind,LeaveFinalKind };
constexpr std::chrono::milliseconds PingEvery{250},RacePingEvery{100},PeerTimeout{4000},HelloEvery{300},DiscoverEvery{700},
    PunchEvery{200},DirectGiveUp{5000},LeaveWait{1500};
// During a race frames flow 60 times a second each way, so a console silent
// for this long is gone (GT3_LINK_DROP_MS; raise it for poor internet links).
std::chrono::milliseconds raceDropTime() {
    static const std::chrono::milliseconds value{[] { const char* v=std::getenv("GT3_LINK_DROP_MS"); const int ms=v?std::atoi(v):0; return ms>=100?ms:500; }()};
    return value;
}

struct Writer {
    std::vector<uint8_t> b;
    void u8(uint8_t v) { b.push_back(v); }
    void u16(uint16_t v) { for (int i=0;i<2;++i) b.push_back(uint8_t(v>>(8*i))); }
    void u32(uint32_t v) { for (int i=0;i<4;++i) b.push_back(uint8_t(v>>(8*i))); }
    void u64(uint64_t v) { for (int i=0;i<8;++i) b.push_back(uint8_t(v>>(8*i))); }
    void bytes(const uint8_t* p,size_t n) { b.insert(b.end(),p,p+n); }
    void guid(const LinkGuid& g) { bytes(g.data(),g.size()); }
    void name(const std::string& s) { std::array<uint8_t,NameBytes> n{}; std::memcpy(n.data(),s.data(),std::min(s.size(),NameBytes-1)); bytes(n.data(),n.size()); }
};
struct Reader {
    const uint8_t* p; size_t n,at=0; bool ok=true;
    bool need(size_t k) { if (at+k>n) ok=false; return ok; }
    uint8_t u8() { if (!need(1)) return 0; return p[at++]; }
    uint16_t u16() { uint16_t v=0; if (need(2)) for (int i=0;i<2;++i) v|=uint16_t(p[at++])<<(8*i); return v; }
    uint32_t u32() { uint32_t v=0; if (need(4)) for (int i=0;i<4;++i) v|=uint32_t(p[at++])<<(8*i); return v; }
    uint64_t u64() { uint64_t v=0; if (need(8)) for (int i=0;i<8;++i) v|=uint64_t(p[at++])<<(8*i); return v; }
    void bytes(uint8_t* d,size_t k) { if (need(k)) { std::memcpy(d,p+at,k); at+=k; } }
    LinkGuid guid() { LinkGuid g{}; bytes(g.data(),g.size()); return g; }
    std::string name() { std::array<char,NameBytes> s{}; bytes(reinterpret_cast<uint8_t*>(s.data()),s.size()); s.back()=0; return s.data(); }
};
std::string guidText(const LinkGuid& g) {
    static const char* hex="0123456789abcdef"; std::string s;
    for (auto b:g) { s+=hex[b>>4]; s+=hex[b&15]; }
    return s;
}
std::string addressText(const sockaddr_storage& a) {
    char text[64]{};
    if (a.ss_family==AF_INET) {
        const auto& v4=reinterpret_cast<const sockaddr_in&>(a);
        inet_ntop(AF_INET,&v4.sin_addr,text,sizeof(text));
        return std::string(text)+":"+std::to_string(ntohs(v4.sin_port));
    }
    return "?";
}
bool sameAddress(const sockaddr_storage& a,const sockaddr_storage& b) {
    if (a.ss_family!=AF_INET || b.ss_family!=AF_INET) return false;
    const auto& x=reinterpret_cast<const sockaddr_in&>(a); const auto& y=reinterpret_cast<const sockaddr_in&>(b);
    return x.sin_port==y.sin_port && x.sin_addr.s_addr==y.sin_addr.s_addr;
}
uint64_t nowMicros() { return uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(Clock::now().time_since_epoch()).count()); }
// Lockstep frame number of a packet (PDI1394 class 2), or -1.
int64_t frameOf(const LinkPacket& p) {
    const uint32_t h=uint32_t(p[0])|uint32_t(p[1])<<8|uint32_t(p[2])<<16|uint32_t(p[3])<<24;
    return (h>>30)==2?int64_t(h&0x07ffffff):-1;
}
}

struct LinkTransport::Impl {
    struct Pending { uint32_t seq; std::vector<uint8_t> body; Clock::time_point sent; };
    struct Peer {
        LinkGuid guid{}; std::string name;
        sockaddr_storage address{}; bool haveAddress=false;
        bool linked=false,relayed=false;             // direct channel up / given up on
        Clock::time_point since=Clock::now(),punched{},heard=Clock::now(),pinged{};
        uint32_t nextSeq=1,received=0;               // direct reliable channel
        std::map<uint32_t,std::vector<uint8_t>> early;
        std::deque<Pending> unacked;
        double rttMs=20; bool ackDue=false;
        uint32_t dataOut=1,dataIn=1;                 // end to end, direct or relayed
        std::map<uint32_t,LinkPacket> dataEarly;
        std::deque<LinkPacket> frames;               // its last frames, for a departure
    };
    struct Leave { LinkGuid who{}; uint32_t round=0; std::vector<LinkGuid> waiting; std::vector<LinkPacket> frames; Clock::time_point started=Clock::now(); };
    struct Deliver { enum { BusChange,Packet,Log } what; std::vector<LinkGuid> members; LinkGuid from{}; LinkPacket packet{}; std::string line; };

    Config config; LinkGuid self; Callbacks callbacks;
    mutable std::mutex mutex;
    SocketHandle socket=NoSocket;
    std::thread worker; std::atomic<bool> stopping{false};
    uint64_t session=0;
    std::map<LinkGuid,Peer> peers;                   // every other console we know of
    std::vector<LinkGuid> members;                   // the session, coordinator's order
    std::map<LinkGuid,std::string> names;
    LinkGuid coordinator{};
    uint32_t generation=0;
    bool joined=false;                               // in a session (a host always is)
    bool together=false;                             // Join: the session had others in it
    sockaddr_storage hostAddress{}; bool hostKnown=false;
    Clock::time_point lastHello{},lastDiscover{};
    std::set<LinkGuid> departed;                     // left this session: ignored
    std::set<LinkGuid> lost;                         // declared gone, departure pending: no new links
    std::deque<Leave> leaves; uint32_t leaveRound=0;
    std::map<LinkGuid,std::vector<LinkPacket>> held; // data from consoles not yet announced
    std::vector<Deliver> pendingDelivery;
    std::string statusFile; Clock::time_point statusWritten{}; bool rejected=false,hostRacing=false;
#if defined(_WIN32)
    bool wsa=false;
#endif

    bool leaving=false;                              // said goodbye: silent from now on
    static std::mutex& registryMutex() { static std::mutex m; return m; }
    static std::set<Impl*>& registry() { static std::set<Impl*> r; return r; }
    // Leaving on purpose: tell every console at once (three copies, UDP),
    // so nobody waits for a timeout.
    void goodbye() {
        leaving=true;
        for (int i=0;i<3;++i) for (auto& [g,p]:peers) if (p.linked) sendControl(p.address,Bye,{},p.received);
    }
    bool racingNow() const { return callbacks.admit && !callbacks.admit(); }
    std::chrono::milliseconds silenceLimit() const { return racingNow()?raceDropTime():PeerTimeout; }
    bool isCoordinator() const { return joined && coordinator==self; }
    bool isMember(const LinkGuid& g) const { return std::find(members.begin(),members.end(),g)!=members.end(); }
    std::string nameOf(const LinkGuid& g) const { const auto it=names.find(g); return it!=names.end() && !it->second.empty()?it->second:guidText(g); }
    void log(std::string line) { pendingDelivery.push_back({Deliver::Log,{},{},{},std::move(line)}); }
    void header(Writer& w,Type type,uint32_t ack) const {
        w.u32(Magic); w.u8(Version); w.u8(type); w.u16(0); w.u64(session); w.u32(ack);
    }
    void sendRaw(const sockaddr_storage& to,const std::vector<uint8_t>& bytes) const {
        sendto(socket,reinterpret_cast<const char*>(bytes.data()),int(bytes.size()),0,reinterpret_cast<const sockaddr*>(&to),sizeof(sockaddr_in));
    }
    void sendControl(const sockaddr_storage& to,Type type,const std::vector<uint8_t>& body={},uint32_t ack=0) const {
        Writer w; header(w,type,ack); w.bytes(body.data(),body.size()); sendRaw(to,w.b);
    }
    void transmit(Peer& peer,const Pending& p) {
        Writer w; header(w,Rel,peer.received); w.u32(p.seq); w.bytes(p.body.data(),p.body.size());
        sendRaw(peer.address,w.b); peer.ackDue=false;
    }
    void sendReliable(Peer& peer,std::vector<uint8_t> body) {
        peer.unacked.push_back({peer.nextSeq++,std::move(body),Clock::now()});
        transmit(peer,peer.unacked.back());
    }
    Peer& peerFor(const LinkGuid& g) { auto& p=peers[g]; p.guid=g; return p; }
    Peer* peerAt(const sockaddr_storage& address) {
        for (auto& [g,p]:peers) if (p.haveAddress && sameAddress(p.address,address)) return &p;
        return nullptr;
    }
    Peer* coordinatorPeer() { const auto it=peers.find(coordinator); return it!=peers.end() && it->second.linked?&it->second:nullptr; }
    void sendLink(Peer& p,bool wantReply) {
        Writer w; w.guid(self); w.name(config.name); w.u8(wantReply?1:0); sendControl(p.address,Link,w.b);
    }

    // --- membership (the coordinator decides) ---
    void publishBus() {
        ++generation;
        Writer w; w.u8(BusKind); w.u32(generation); w.guid(coordinator); w.u8(uint8_t(members.size()));
        for (const auto& g:members) {
            w.guid(g); w.name(nameOf(g));
            const auto it=peers.find(g);
            // The address this console sees the member at (0 = "the one you already use").
            if (g!=self && it!=peers.end() && it->second.haveAddress) {
                const auto& v4=reinterpret_cast<const sockaddr_in&>(it->second.address);
                w.u32(v4.sin_addr.s_addr); w.u16(v4.sin_port);
            } else { w.u32(0); w.u16(0); }
        }
        for (const auto& g:members) if (g!=self) if (auto it=peers.find(g);it!=peers.end() && it->second.linked) sendReliable(it->second,w.b);
        announce();
    }
    void announce() {
        pendingDelivery.push_back({Deliver::BusChange,members});
        for (auto it=held.begin();it!=held.end();) {
            if (!isMember(it->first)) { ++it; continue; }
            for (const auto& p:it->second) pendingDelivery.push_back({Deliver::Packet,{},it->first,p,{}});
            it=held.erase(it);
        }
    }
    void localBusAlone() { members.assign(1,self); announce(); }
    void takeBus(Reader& r) {
        const auto gen=r.u32(); const auto coord=r.guid(); const auto count=r.u8();
        std::vector<LinkGuid> list;
        std::vector<std::pair<LinkGuid,sockaddr_storage>> addresses;
        for (unsigned i=0;i<count && r.ok;++i) {
            const auto g=r.guid(); const auto name=r.name(); const auto ip=r.u32(); const auto port=r.u16();
            list.push_back(g); names[g]=name;
            if (ip) { sockaddr_storage a{}; auto& v4=reinterpret_cast<sockaddr_in&>(a); v4.sin_family=AF_INET; v4.sin_addr.s_addr=ip; v4.sin_port=port; addresses.push_back({g,a}); }
        }
        if (!r.ok || int32_t(gen-generation)<=0) return;
        generation=gen; coordinator=coord;
        for (const auto& g:list) if (g!=self) {
            const bool known=peers.count(g)!=0;
            auto& p=peerFor(g);
            if (!known) p.since=Clock::now();
            p.name=names[g];
        }
        for (const auto& [g,a]:addresses) if (g!=self) { auto& p=peerFor(g); if (!p.haveAddress) { p.address=a; p.haveAddress=true; } }
        for (auto it=peers.begin();it!=peers.end();) {
            if (std::find(list.begin(),list.end(),it->first)==list.end() && isMember(it->first)) { departed.insert(it->first); it=peers.erase(it); }
            else ++it;
        }
        members=list;
        if (members.size()>1) together=true;
        announce();
    }

    // --- data, end to end ---
    void sendData(const LinkGuid& to,const LinkPacket& packet) {
        auto& p=peerFor(to);
        Writer w; w.u8(DataKind); w.guid(self); w.guid(to); w.u32(p.dataOut++); w.bytes(packet.data(),packet.size());
        if (p.linked && !p.relayed) { sendReliable(p,w.b); return; }
        if (Peer* c=coordinatorPeer();c && coordinator!=to) { w.b[0]=RelayKind; sendReliable(*c,w.b); }
    }
    void deliverData(const LinkGuid& from,const LinkPacket& packet) {
        if (frameOf(packet)>=0) {
            auto& frames=peerFor(from).frames; frames.push_back(packet);
            while (frames.size()>FrameHistory) frames.pop_front();
        }
        if (isMember(from)) pendingDelivery.push_back({Deliver::Packet,{},from,packet,{}});
        else held[from].push_back(packet);
    }
    void takeData(const LinkGuid& from,uint32_t seq,const LinkPacket& packet) {
        if (from==self || departed.count(from)) return;
        auto& p=peerFor(from);
        if (int32_t(seq-p.dataIn)<0) return;                             // copy already delivered
        if (seq!=p.dataIn) { if (p.dataEarly.size()<512) p.dataEarly.emplace(seq,packet); return; }
        deliverData(from,packet); ++p.dataIn;
        for (auto& q=peerFor(from);!q.dataEarly.empty() && q.dataEarly.begin()->first==q.dataIn;) {
            const auto next=q.dataEarly.begin()->second; q.dataEarly.erase(q.dataEarly.begin());
            deliverData(from,next); ++q.dataIn;
        }
    }

    // --- a console leaves: same last frames everywhere, then the bus change ---
    void startLeave(const LinkGuid& who) {
        if (who==self || !isMember(who)) { peers.erase(who); return; }
        for (const auto& l:leaves) if (l.who==who) return;
        Leave l; l.who=who; l.round=++leaveRound;
        if (auto it=peers.find(who);it!=peers.end()) { l.frames.assign(it->second.frames.begin(),it->second.frames.end()); it->second.linked=false; }
        Writer w; w.u8(LeaveQueryKind); w.guid(who); w.u32(l.round);
        for (const auto& g:members) if (g!=self && g!=who)
            if (auto it=peers.find(g);it!=peers.end() && it->second.linked) { l.waiting.push_back(g); sendReliable(it->second,w.b); }
        log("[GT3/link] "+nameOf(who)+" left; collecting its last frames from the others");
        leaves.push_back(std::move(l));
        if (leaves.back().waiting.empty()) finishLeave(leaves.size()-1);
    }
    static void mergeFrames(std::vector<LinkPacket>& into,const LinkPacket& f) {
        for (const auto& e:into) if (frameOf(e)==frameOf(f)) return;
        into.push_back(f);
    }
    void finishLeave(size_t index) {
        Leave l=std::move(leaves[index]); leaves.erase(leaves.begin()+ptrdiff_t(index));
        std::sort(l.frames.begin(),l.frames.end(),[](const LinkPacket& a,const LinkPacket& b) { return frameOf(a)<frameOf(b); });
        Writer w; w.u8(LeaveFinalKind); w.guid(l.who); w.u32(l.round); w.u8(uint8_t(l.frames.size()));
        for (const auto& f:l.frames) w.bytes(f.data(),f.size());
        for (const auto& g:members) if (g!=self && g!=l.who)
            if (auto it=peers.find(g);it!=peers.end() && it->second.linked) sendReliable(it->second,w.b);
        applyLastFrames(l.who,l.frames);
        members.erase(std::remove(members.begin(),members.end(),l.who),members.end());
        peers.erase(l.who); departed.insert(l.who); lost.erase(l.who);
        publishBus();
    }
    // Frames of a departed console this console has not had yet, in order.
    void applyLastFrames(const LinkGuid& who,const std::vector<LinkPacket>& frames) {
        int64_t last=-1;
        if (auto it=peers.find(who);it!=peers.end() && !it->second.frames.empty()) last=frameOf(it->second.frames.back());
        for (const auto& f:frames) if (frameOf(f)>last) { deliverData(who,f); last=frameOf(f); }
    }
    void coordinatorLost() {
        const auto old=coordinator;
        log("[GT3/link] "+nameOf(old)+" (keeping the member list) stopped answering");
        if (auto it=peers.find(old);it!=peers.end()) it->second.linked=false;
        lost.insert(old);
        const auto now=Clock::now();
        LinkGuid next=self;
        for (const auto& g:members) {
            if (g==old) continue;
            if (g==self) { next=self; break; }
            if (auto it=peers.find(g);it!=peers.end() && it->second.linked && now-it->second.heard<silenceLimit()) { next=g; break; }
        }
        coordinator=next;
        if (next==self) { log("[GT3/link] this console now keeps the member list"); startLeave(old); }
    }
    void peerLost(const LinkGuid& g,const char* why) {
        if (!isMember(g)) { peers.erase(g); return; }
        auto& p=peerFor(g);
        if (g==coordinator && !isCoordinator()) { log("[GT3/link] "+nameOf(g)+" "+why); coordinatorLost(); return; }
        if (isCoordinator()) { log("[GT3/link] "+nameOf(g)+" "+why); lost.insert(g); startLeave(g); return; }
        // Only our direct link failed: its data goes through the coordinator;
        // whether it left is the coordinator's call.
        p.linked=false; p.relayed=true;
        if (Peer* c=coordinatorPeer()) for (auto& pending:p.unacked) if (!pending.body.empty() && pending.body[0]==DataKind) {
            pending.body[0]=RelayKind; sendReliable(*c,pending.body);
        }
        p.unacked.clear();
        log("[GT3/link] lost the direct link to "+nameOf(g)+"; relaying through "+nameOf(coordinator));
    }

    // One in-order reliable message from `from`.
    void handleMessage(const LinkGuid& from,const std::vector<uint8_t>& body) {
        Reader r{body.data(),body.size()};
        const auto kind=r.u8();
        if (kind==BusKind) { takeBus(r); return; }
        if (kind==DataKind || kind==RelayKind) {
            const auto src=r.guid(); const auto dst=r.guid(); const auto seq=r.u32();
            LinkPacket packet{}; r.bytes(packet.data(),packet.size());
            if (!r.ok) return;
            if (kind==DataKind) { if (dst==self) takeData(src,seq,packet); return; }
            if (!isCoordinator() || src!=from) return;
            if (dst==self) { takeData(src,seq,packet); return; }
            if (auto it=peers.find(dst);it!=peers.end() && it->second.linked) {
                Writer w; w.u8(DataKind); w.guid(src); w.guid(dst); w.u32(seq); w.bytes(packet.data(),packet.size());
                sendReliable(it->second,w.b);
            }
            return;
        }
        if (kind==ResetKind && isCoordinator()) { log("[GT3/link] bus reset requested by "+nameOf(from)); publishBus(); return; }
        if (kind==LeaveQueryKind) {
            const auto who=r.guid(); const auto round=r.u32();
            if (!r.ok) return;
            Writer w; w.u8(LeaveStateKind); w.guid(who); w.u32(round);
            const auto it=peers.find(who);
            const auto count=it!=peers.end()?it->second.frames.size():0;
            w.u8(uint8_t(count));
            if (count) for (const auto& f:it->second.frames) w.bytes(f.data(),f.size());
            if (auto sender=peers.find(from);sender!=peers.end() && sender->second.linked) sendReliable(sender->second,w.b);
            return;
        }
        if (kind==LeaveStateKind && isCoordinator()) {
            const auto who=r.guid(); const auto round=r.u32(); const auto count=r.u8();
            for (size_t i=0;i<leaves.size();++i) {
                auto& l=leaves[i];
                if (l.who!=who || l.round!=round) continue;
                for (unsigned k=0;k<count && r.ok;++k) { LinkPacket f{}; r.bytes(f.data(),f.size()); if (r.ok) mergeFrames(l.frames,f); }
                l.waiting.erase(std::remove(l.waiting.begin(),l.waiting.end(),from),l.waiting.end());
                if (l.waiting.empty()) finishLeave(i);
                return;
            }
            return;
        }
        if (kind==LeaveFinalKind) {
            const auto who=r.guid(); r.u32(); const auto count=r.u8();
            std::vector<LinkPacket> frames;
            for (unsigned k=0;k<count && r.ok;++k) { LinkPacket f{}; r.bytes(f.data(),f.size()); if (r.ok) frames.push_back(f); }
            if (r.ok) applyLastFrames(who,frames);
        }
    }
    void acknowledge(Peer& peer,uint32_t ack) {
        while (!peer.unacked.empty() && int32_t(ack-peer.unacked.front().seq)>=0) peer.unacked.pop_front();
    }
    void admit(const sockaddr_storage& from,const LinkGuid& guid,const std::string& name) {
        auto sendWelcome=[&] { Writer w; w.guid(self); w.name(config.name); sendControl(from,Welcome,w.b); };
        if (isMember(guid)) { sendWelcome(); peerFor(guid).heard=Clock::now(); return; }
        // Same address, new identity: that game restarted.
        for (auto& [g,p]:peers) if (g!=guid && p.haveAddress && sameAddress(p.address,from) && isMember(g)) { startLeave(g); break; }
        if (members.size()>=MaxMembers || guid==self || departed.count(guid)) { Writer w; w.u32(1); sendControl(from,Reject,w.b); return; }
        if (callbacks.admit && !callbacks.admit()) { Writer w; w.u32(2); sendControl(from,Reject,w.b); return; }   // racing: wait
        auto& p=peerFor(guid); p.name=name.empty()?addressText(from):name; names[guid]=p.name;
        p.address=from; p.haveAddress=true; p.linked=true; p.heard=p.since=Clock::now();
        members.push_back(guid);
        log("[GT3/link] "+p.name+" joined from "+addressText(from)+" ("+guidText(guid)+")");
        sendWelcome();
        publishBus();
    }
    void receive(const uint8_t* data,size_t size,const sockaddr_storage& from) {
        if (leaving) return;
        Reader r{data,size};
        if (r.u32()!=Magic || r.u8()!=Version) return;
        const auto type=Type(r.u8()); r.u16();
        const auto token=r.u64(); const auto ack=r.u32();
        if (!r.ok) return;
        if (type==Discover) {
            if (config.mode==Mode::Host && isCoordinator()) {
                Writer w; w.name(config.name); w.u16(config.port); w.u8(uint8_t(members.size()));
                sendControl(from,Offer,w.b);
            }
            return;
        }
        if (type==Hello) {
            const auto guid=r.guid(); const auto name=r.name();
            if (r.ok && isCoordinator()) admit(from,guid,name);
            return;
        }
        if (config.mode==Mode::Join && !joined) {
            if (type==Offer && !hostKnown) {
                const auto name=r.name(); const auto port=r.u16();
                if (!r.ok) return;
                hostAddress=from; reinterpret_cast<sockaddr_in&>(hostAddress).sin_port=htons(port);
                hostKnown=true; log("[GT3/link] found host "+name+" at "+addressText(hostAddress));
            } else if (type==Welcome && hostKnown && sameAddress(from,hostAddress)) {
                const auto guid=r.guid(); const auto name=r.name();
                if (!r.ok) return;
                joined=true; session=token; coordinator=guid; hostRacing=rejected=false; names[guid]=name;
                auto& p=peerFor(guid); p.name=name; p.address=from; p.haveAddress=true; p.linked=true; p.heard=p.since=Clock::now();
                log("[GT3/link] joined "+name+" at "+addressText(from));
            } else if (type==Reject && hostKnown && sameAddress(from,hostAddress)) {
                const bool busy=r.u32()==2;
                if (busy && !hostRacing) log("[GT3/link] the host is in a race; this console joins when it ends");
                if (!busy && !rejected) log("[GT3/link] the host refused to add this console (full or a different version)");
                (busy?hostRacing:rejected)=true;
            }
            return;
        }
        if (!joined || token!=session) return;
        if (type==Link) {
            const auto guid=r.guid(); const auto name=r.name(); const bool wantReply=r.u8()!=0;
            if (!r.ok || guid==self || departed.count(guid) || lost.count(guid)) return;
            auto& p=peerFor(guid); if (!name.empty()) { p.name=name; names[guid]=name; }
            p.address=from; p.haveAddress=true; p.heard=Clock::now();
            if (!p.linked) { p.linked=true; p.relayed=false; p.pinged={}; log("[GT3/link] direct link with "+nameOf(guid)+" at "+addressText(from)); }
            if (wantReply) sendLink(p,false);
            return;
        }
        Peer* peer=peerAt(from);
        if (!peer || !peer->linked) return;
        peer->heard=Clock::now();
        acknowledge(*peer,ack);
        if (type==Ping) { Writer w; w.u64(r.u64()); if (r.ok) sendControl(from,Pong,w.b,peer->received); return; }
        if (type==Pong) {
            const auto sent=r.u64();
            if (r.ok) { const double ms=double(nowMicros()-sent)/1000.0; peer->rttMs=peer->rttMs*0.8+ms*0.2; }
            return;
        }
        if (type==Bye) { peerLost(peer->guid,"left"); return; }
        if (type!=Rel) return;
        const auto seq=r.u32();
        if (!r.ok) return;
        std::vector<uint8_t> body(data+r.at,data+size);
        peer->ackDue=true;
        if (int32_t(seq-peer->received)<=0) return;                 // duplicate
        if (seq!=peer->received+1) { if (peer->early.size()<256) peer->early.emplace(seq,std::move(body)); return; }
        const auto guid=peer->guid;
        peer->received=seq; handleMessage(guid,body);
        for (auto it=peers.find(guid);it!=peers.end() && !it->second.early.empty() && it->second.early.begin()->first==it->second.received+1;it=peers.find(guid)) {
            auto next=std::move(it->second.early.begin()->second); it->second.early.erase(it->second.early.begin());
            ++it->second.received; handleMessage(guid,next);
        }
    }
    void tick() {
        if (leaving) return;
        const auto now=Clock::now();
        const bool mayJoin=!callbacks.admit || callbacks.admit();   // not in a race
        const auto silence=mayJoin?PeerTimeout:raceDropTime();
        const auto pingEvery=mayJoin?PingEvery:RacePingEvery;
        if (config.mode==Mode::Join) {
            // Everyone else gone: look for a host again (after any race).
            if (joined && together && members.size()==1 && mayJoin && leaves.empty()) {
                joined=false; together=false; session=0; coordinator={}; generation=0; peers.clear(); departed.clear(); lost.clear(); held.clear();
                hostKnown=!config.host.empty() && resolveHost();
                log("[GT3/link] alone; looking for a host again");
            }
            if (!joined && mayJoin) {
                if (!hostKnown && now-lastDiscover>=DiscoverEvery) {
                    sockaddr_storage all{}; auto& v4=reinterpret_cast<sockaddr_in&>(all);
                    v4.sin_family=AF_INET; v4.sin_port=htons(config.port); v4.sin_addr.s_addr=htonl(INADDR_BROADCAST);
                    sendControl(all,Discover); lastDiscover=now;
                }
                if (hostKnown && now-lastHello>=HelloEvery) {
                    Writer w; w.guid(self); w.name(config.name);
                    sendControl(hostAddress,Hello,w.b); lastHello=now;
                }
            }
        }
        std::vector<std::pair<LinkGuid,const char*>> silent;
        for (auto& [g,p]:peers) {
            if (!p.linked) {
                if (!p.haveAddress || p.relayed || !isMember(g) || lost.count(g)) continue;
                if (now-p.punched>=PunchEvery) { sendLink(p,true); p.punched=now; }
                if (now-p.since>=DirectGiveUp) {
                    p.relayed=true;
                    log("[GT3/link] no direct link to "+nameOf(g)+(isCoordinator()?std::string():"; relaying through "+nameOf(coordinator)));
                }
                continue;
            }
            if (now-p.heard>silence) { silent.push_back({g,"stopped answering"}); continue; }
            const auto rto=std::chrono::microseconds(int64_t(std::clamp(p.rttMs*1.5+5.0,15.0,500.0)*1000.0));
            for (auto& pending:p.unacked) if (now-pending.sent>=rto) { transmit(p,pending); pending.sent=now; }
            if (now-p.pinged>=pingEvery) { Writer w; w.u64(nowMicros()); sendControl(p.address,Ping,w.b,p.received); p.pinged=now; p.ackDue=false; }
            if (p.ackDue) { sendControl(p.address,Ack,{},p.received); p.ackDue=false; }
        }
        for (const auto& [g,why]:silent) if (peers.count(g)) peerLost(g,why);
        for (size_t i=0;i<leaves.size();++i) if (now-leaves[i].started>=LeaveWait) { finishLeave(i); break; }
    }
    void writeStatus() {
        if (statusFile.empty()) return;
        std::string state=config.mode==Mode::Off?"off":config.mode==Mode::Host && isCoordinator()?"hosting":
            rejected?"refused":joined?"connected":hostRacing?"waiting":hostKnown?"connecting":"searching";
        std::string text="mode="+std::string(config.mode==Mode::Host?"host":config.mode==Mode::Join?"join":"off")+
            "\nstate="+state+"\nport="+std::to_string(config.port)+"\nname="+config.name+
            "\nconsoles="+std::to_string(members.size())+"\n";
        if (joined && coordinator!=self) text+="host="+nameOf(coordinator)+"\n";
        else if (config.mode==Mode::Join && hostKnown) text+="host="+addressText(hostAddress)+"\n";
        for (const auto& g:members) if (g!=self) {
            const auto it=peers.find(g);
            const bool direct=it!=peers.end() && it->second.linked;
            text+="peer="+nameOf(g)+"|"+(direct?std::to_string(int(it->second.rttMs+0.5)):std::string())+(direct?"":"|relayed")+"\n";
        }
        const auto temp=statusFile+".tmp";
        if (FILE* f=std::fopen(temp.c_str(),"wb")) {
            std::fwrite(text.data(),1,text.size(),f); std::fclose(f);
#if defined(_WIN32)
            MoveFileExA(temp.c_str(),statusFile.c_str(),MOVEFILE_REPLACE_EXISTING);
#else
            std::rename(temp.c_str(),statusFile.c_str());
#endif
        }
    }
    void flush() {
        std::vector<Deliver> work; { std::lock_guard lock(mutex); work.swap(pendingDelivery); }
        for (auto& d:work) {
            if (d.what==Deliver::BusChange && callbacks.bus) callbacks.bus(d.members);
            else if (d.what==Deliver::Packet && callbacks.packet) callbacks.packet(d.from,d.packet);
            else if (d.what==Deliver::Log && callbacks.log) callbacks.log(d.line);
        }
    }
    void run() {
        std::array<uint8_t,2048> buffer{};
        while (!stopping) {
            fd_set read; FD_ZERO(&read); FD_SET(socket,&read);
            timeval wait{0,3000};
            const int ready=select(int(socket)+1,&read,nullptr,nullptr,&wait);
            {
                std::lock_guard lock(mutex);
                if (ready>0) for (int i=0;i<64;++i) {
                    sockaddr_storage from{}; socklen_t length=sizeof(from);
#if defined(_WIN32)
                    u_long available=0; ioctlsocket(socket,FIONREAD,&available); if (!available && i) break;
#endif
                    const auto n=recvfrom(socket,reinterpret_cast<char*>(buffer.data()),int(buffer.size()),0,reinterpret_cast<sockaddr*>(&from),&length);
                    if (n<=0) break;
                    receive(buffer.data(),size_t(n),from);
#if !defined(_WIN32)
                    break;
#endif
                }
                tick();
                if (Clock::now()-statusWritten>=std::chrono::milliseconds(500)) { writeStatus(); statusWritten=Clock::now(); }
            }
            flush();
        }
    }
    bool resolveHost() {
        auto text=config.host; uint16_t port=config.port;
        if (const auto colon=text.rfind(':');colon!=std::string::npos) { port=uint16_t(std::atoi(text.c_str()+colon+1)); text.resize(colon); }
        addrinfo hints{}; hints.ai_family=AF_INET; hints.ai_socktype=SOCK_DGRAM; addrinfo* found=nullptr;
        if (getaddrinfo(text.c_str(),nullptr,&hints,&found)!=0 || !found) return false;
        std::memcpy(&hostAddress,found->ai_addr,sizeof(sockaddr_in)); freeaddrinfo(found);
        reinterpret_cast<sockaddr_in&>(hostAddress).sin_port=htons(port);
        hostKnown=true; return true;
    }
};

LinkTransport::LinkTransport(Config config,LinkGuid self,Callbacks callbacks):impl_(std::make_unique<Impl>()) {
    impl_->config=std::move(config); impl_->self=self; impl_->callbacks=std::move(callbacks);
    if (impl_->config.name.empty()) impl_->config.name="GT3";
    impl_->names[self]=impl_->config.name;
    if (const char* status=std::getenv("GT3_LINK_STATUS")) impl_->statusFile=status;
}
LinkTransport::~LinkTransport() { stop(); }

bool LinkTransport::start() {
    auto& s=*impl_;
    if (s.config.mode==Mode::Off) { { std::lock_guard lock(s.mutex); s.localBusAlone(); } s.flush(); return true; }
#if defined(_WIN32)
    WSADATA data{}; if (WSAStartup(MAKEWORD(2,2),&data)!=0) return false; s.wsa=true;
#endif
    s.socket=::socket(AF_INET,SOCK_DGRAM,IPPROTO_UDP);
    if (s.socket==NoSocket) return false;
    int yes=1;
    setsockopt(s.socket,SOL_SOCKET,SO_BROADCAST,reinterpret_cast<const char*>(&yes),sizeof(yes));
#if defined(_WIN32)
    // A datagram to a closed port would otherwise fail the next recvfrom.
#ifndef SIO_UDP_CONNRESET
#define SIO_UDP_CONNRESET _WSAIOW(IOC_VENDOR,12)
#endif
    BOOL report=FALSE; DWORD returned=0;
    WSAIoctl(s.socket,SIO_UDP_CONNRESET,&report,sizeof(report),nullptr,0,&returned,nullptr,nullptr);
#endif
    sockaddr_in local{}; local.sin_family=AF_INET; local.sin_addr.s_addr=htonl(INADDR_ANY);
    local.sin_port=htons(s.config.mode==Mode::Host?s.config.port:0);
    if (bind(s.socket,reinterpret_cast<const sockaddr*>(&local),sizeof(local))!=0) {
        if (s.callbacks.log) s.callbacks.log("[GT3/link] cannot use UDP port "+std::to_string(s.config.port)+" (another program or game instance has it)");
        closeSocket(s.socket); s.socket=NoSocket; return false;
    }
    std::mt19937_64 random{std::random_device{}()^uint64_t(nowMicros())};
    if (s.config.mode==Mode::Join && !s.config.host.empty() && !s.resolveHost()) {
        if (s.callbacks.log) s.callbacks.log("[GT3/link] cannot resolve host '"+s.config.host+"'");
    }
    {
        std::lock_guard lock(s.mutex);
        if (s.config.mode==Mode::Host) {
            s.session=random()|1; s.joined=true; s.coordinator=s.self; s.members.assign(1,s.self);
            s.log("[GT3/link] hosting on UDP port "+std::to_string(s.config.port)+" as "+s.config.name);
            s.publishBus();
        } else {
            s.log(std::string("[GT3/link] joining ")+(s.config.host.empty()?"the first host found on the LAN":s.config.host)+" as "+s.config.name);
            s.localBusAlone();
        }
    }
    s.flush();
    s.stopping=false;
    s.worker=std::thread([&s] { s.run(); });
    { std::lock_guard lock(Impl::registryMutex()); Impl::registry().insert(&s); }
    return true;
}

void LinkTransport::goodbyeAll(bool fromCrash) {
    std::unique_lock registry(Impl::registryMutex(),std::defer_lock);
    if (!fromCrash) registry.lock();
    else if (!registry.try_lock()) return;
    for (Impl* s:Impl::registry()) {
        std::unique_lock lock(s->mutex,std::defer_lock);
        if (!fromCrash) lock.lock();
        else if (!lock.try_lock()) continue;
        if (!s->leaving) s->goodbye();
    }
}

void LinkTransport::stop() {
    auto& s=*impl_;
    { std::lock_guard lock(Impl::registryMutex()); Impl::registry().erase(&s); }
    if (s.worker.joinable()) {
        {
            std::lock_guard lock(s.mutex);
            if (!s.leaving) s.goodbye();
        }
        s.stopping=true; s.worker.join();
    }
    if (s.socket!=NoSocket) { closeSocket(s.socket); s.socket=NoSocket; }
#if defined(_WIN32)
    if (s.wsa) { WSACleanup(); s.wsa=false; }
#endif
    if (!s.statusFile.empty()) std::remove(s.statusFile.c_str());
    std::lock_guard lock(s.mutex); s.peers.clear(); s.joined=false; s.leaving=false; s.pendingDelivery.clear();
}

void LinkTransport::send(const LinkPacket& packet,const std::vector<LinkGuid>& to) {
    auto& s=*impl_;
    if (to.empty() || s.config.mode==Mode::Off) return;
    std::lock_guard lock(s.mutex);
    if (!s.joined) return;
    for (const auto& g:to) if (g!=s.self && s.isMember(g) && !s.departed.count(g)) s.sendData(g,packet);
}

void LinkTransport::requestBusReset() {
    auto& s=*impl_;
    {
        std::lock_guard lock(s.mutex);
        if (s.isCoordinator()) s.publishBus();
        else if (s.joined) { if (auto* c=s.coordinatorPeer()) { Writer w; w.u8(ResetKind); s.sendReliable(*c,w.b); } }
        else s.localBusAlone();
    }
    if (!s.worker.joinable()) s.flush();
}

std::vector<LinkTransport::PeerStatus> LinkTransport::peers() const {
    std::lock_guard lock(impl_->mutex); std::vector<PeerStatus> list;
    for (const auto& [g,p]:impl_->peers) if (impl_->isMember(g)) list.push_back({g,impl_->nameOf(g),p.rttMs});
    return list;
}

LinkTransport::Config LinkTransport::configFromEnvironment() {
    Config c;
    auto env=[](const char* name) { const char* v=std::getenv(name); return std::string(v?v:""); };
    const auto mode=env("GT3_LINK_MODE");
    c.mode=mode=="host"?Mode::Host:mode=="join"?Mode::Join:Mode::Off;
    c.host=env("GT3_LINK_HOST"); if (c.host=="auto") c.host.clear();
    if (const auto port=std::atoi(env("GT3_LINK_PORT").c_str());port>0 && port<65536) c.port=uint16_t(port);
    c.name=env("GT3_LINK_NAME"); if (c.name.size()>NameBytes-1) c.name.resize(NameBytes-1);
    return c;
}
}
