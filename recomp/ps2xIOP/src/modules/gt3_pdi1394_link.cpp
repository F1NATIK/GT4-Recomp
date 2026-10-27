#include "ps2x/iop/gt3/pdi1394_link.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <mutex>
#include <optional>
#include <random>

namespace ps2x::iop::gt3 {
namespace {
using Clock=std::chrono::steady_clock;
// GT3's EE libsif state (_sif_rpc_data) in CORE d0188895...: the RPC queue
// list at +0x28 and the reply packet pool that the game's own sceSifInitRpc
// would set up (the runtime replaces that call, so the pool fields stay 0).
constexpr uint32_t SifRpcState=0x4af940,SifPacketTable=0x004ae940,SifBufferTable=0x004af140;
constexpr uint32_t ReplyCommand=0x8000000a;   // SIF_CMD_RPC_END, word 0x20 of the reply packet
constexpr size_t PayloadBytes=0x7c,RingLimit=32,EventLimit=64;
void put32(uint8_t* p,uint32_t v) { for (int i=0;i<4;++i) p[i]=uint8_t(v>>(8*i)); }
uint32_t get32(const uint8_t* p) { uint32_t v=0; for (int i=0;i<4;++i) v|=uint32_t(p[i])<<(8*i); return v; }
std::string hexWord(uint32_t v) { char t[16]; std::snprintf(t,sizeof(t),"%08x",v); return t; }
}

struct Pdi1394Link::State {
    struct Message { uint32_t header=0; std::array<uint8_t,PayloadBytes> payload{}; };
    struct Node {
        LinkGuid guid{}; uint32_t index=0; bool active=false,seen=false; std::deque<Message> ring;
        // Left during a race (not in the original): the game is not told; the
        // car gets its last frame with the controller released, from the frame
        // after its last real one, the same on every console left.
        bool ghost=false,hasLast=false; uint32_t lastSeq=0; std::array<uint8_t,PayloadBytes> lastPayload{};
    };
    // Original table at 0x3f78: flags bit0 = console present, bit1 = active in
    // the session; entries outlive the node so a console keeps its index.
    struct TableEntry { uint32_t flags=0,index=0; LinkGuid guid{}; };
    struct Event { uint32_t function=0; std::vector<uint8_t> payload; };

    IopHost& host;
    explicit State(IopHost& h):host(h) {}
    mutable std::mutex mutex; std::condition_variable changed;
    std::vector<Node> nodes;                 // original list at 0x1b4: self first
    std::array<TableEntry,8> table{};
    LinkGuid self{};
    uint32_t present=0,count=0,frame=0,busNodes=1,busSnapshot=0;   // 0x1b0,0x1ac,0x170,0x1a8,0x1c0
    bool running=false,stopping=false,started=false;              // 0x178
    uint64_t ringEpoch=0;
    std::array<uint8_t,8> controlBuffer{};   // RPC buffer 0x2a28
    std::deque<Event> events;
    std::unique_ptr<LinkTransport> transport;
    // EE delivery, executor only.
    uint32_t replyPacket=0,delivered=0; bool delivering=false,missingLogged=false;
    uint32_t deliverServer=0; Clock::time_point deliverStart{};
    // GT3_LINK_TRACE=1: control calls, exchanges other than plain frames, events.
    const bool trace=[] { const char* v=std::getenv("GT3_LINK_TRACE"); return v && *v && *v!='0'; }();
    uint64_t exchanges=0,controlRepeats=0; std::string lastControl;
    // Racing: an exchange is waiting, or one ended less than 3 s ago.
    // GT3_LINK_KEEP_RACING=0 restores the cable's behaviour (a drop ends the race).
    const bool keepRacing=[] { const char* v=std::getenv("GT3_LINK_KEEP_RACING"); return !(v && *v=='0'); }();
    bool exchanging=false; Clock::time_point lastExchange{};
    std::atomic<int64_t> racingUntilMs{0};    // read by the transport's admission check
    std::vector<LinkGuid> members;            // latest membership (for the deferred reset)
    static int64_t nowMs() { return std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now().time_since_epoch()).count(); }
    bool racing() const { return running && (exchanging || Clock::now()-lastExchange<std::chrono::seconds(3)); }
    bool anyGhost() const { for (const auto& n:nodes) if (n.ghost) return true; return false; }
    // GT3_LINK_DUMP=file: every exchange appended as {function, size, sent bytes, 0x3e0 reply}.
    FILE* dump=[] { const char* v=std::getenv("GT3_LINK_DUMP"); return v && *v?std::fopen(v,"wb"):nullptr; }();
    ~State() { if (dump) std::fclose(dump); }
    static std::string bytesText(const uint8_t* p,size_t n) {
        std::string s; char t[4];
        for (size_t i=0;i<n;++i) { std::snprintf(t,sizeof(t),"%02x",p[i]); s+=t; }
        return s;
    }

    void log(LogLevel level,const std::string& line) { host.log(level,line); }
    Node* find(const LinkGuid& g) { for (auto& n:nodes) if (n.guid==g) return &n; return nullptr; }
    Node& selfNode() { return nodes.front(); }

    int tableFreeIndex(const LinkGuid& g) const {            // 0x1cd8
        for (const auto& e:table) if (e.guid==g && !(e.flags&1)) return int(e.index);
        return -1;
    }
    bool tableActive(const LinkGuid& g) const {              // 0x1e10
        for (const auto& e:table) if (e.guid==g && (e.flags&2)) return true;
        return false;
    }
    void tableAdd(const LinkGuid& g,uint32_t index) {        // 0x1c04
        for (auto& e:table) if (e.guid==g) { e.flags|=1; return; }
        for (auto& e:table) if (!(e.flags&1)) { e.index=index; e.guid=g; e.flags|=1; return; }
    }
    bool tableAllPresent() const {                           // 0x1bc4
        for (const auto& e:table) if ((e.flags&3)==2) return false;
        return true;
    }
    Node* createNode(const LinkGuid& g) {                    // 0x1a84 / 0x1f4c
        if (auto* n=find(g)) return n;
        if (nodes.size()>=8) return nullptr;
        int index=tableFreeIndex(g);
        if (index<0) {
            index=0;
            for (bool used=true;used;) { used=false; for (const auto& n:nodes) if (n.index==uint32_t(index)) { ++index; used=true; break; } }
        }
        Node n; n.guid=g; n.index=uint32_t(index); n.active=tableActive(g);
        nodes.push_back(std::move(n));
        present|=1u<<index; tableAdd(g,uint32_t(index));
        return &nodes.back();
    }
    void resetRings() {                                      // 0x0ee8
        frame=0; ++ringEpoch;
        for (auto& n:nodes) n.ring.clear();
        changed.notify_all();
    }
    void queueEvent(uint32_t function,std::vector<uint8_t> payload={}) {   // 0x0fd8
        if (function==0xffffffffu && !events.empty() && events.back().function==function) return;
        if (events.size()>=EventLimit) { log(LogLevel::Warning,"[GT3/PDI1394] EE event queue full; dropped "+hexWord(function)); return; }
        events.push_back({function,std::move(payload)});
    }
    // 0x1874: the bus changed; find every console again, keep known ones.
    void enumerate(const std::vector<LinkGuid>& members) {
        busNodes=uint32_t(members.size()); count=0;
        for (auto& n:nodes) n.seen=false;
        for (const auto& g:members) {
            if (count>=8) { queueEvent(0xfffffffdu); break; }
            if (auto* n=createNode(g)) { n->seen=true; ++count; }
        }
        for (size_t i=0;i<nodes.size();) {
            if (nodes[i].seen || nodes[i].guid==self) { ++i; continue; }
            present&=~(1u<<nodes[i].index);                  // 0x20d4
            for (auto& e:table) if (e.guid==nodes[i].guid) e.flags&=~1u;
            nodes.erase(nodes.begin()+ptrdiff_t(i));
        }
    }
    void enqueue(Node& node,const Message& m) {              // 0x0d5c
        const uint32_t seq=m.header&0x07ffffff;
        if (!node.hasLast || int32_t(seq-node.lastSeq)>0) { node.lastSeq=seq; node.lastPayload=m.payload; node.hasLast=true; }
        if (seq<frame) return;                               // older than the frame being collected
        if (node.ring.size()>=RingLimit) { log(LogLevel::Warning,"[GT3/PDI1394] console ring full; frame dropped"); return; }
        node.ring.push_back(m); changed.notify_all();
    }
    // 0x0ae0: class-2 frames go to active consoles and back into our own
    // ring; control classes go to every other console.
    void send(uint32_t header,const uint8_t* data,size_t size) {
        LinkPacket packet{}; put32(packet.data(),header);
        std::memcpy(packet.data()+4,data,std::min(size,PayloadBytes));
        const bool frameData=(header>>30)==2;
        if (frameData) { Message m; m.header=header; std::memcpy(m.payload.data(),packet.data()+4,PayloadBytes); enqueue(selfNode(),m); }
        std::vector<LinkGuid> to;
        for (const auto& n:nodes) if (n.guid!=self && (!frameData || n.active)) to.push_back(n.guid);
        if (transport) transport->send(packet,to);
    }
    // Transport thread: 0x0178 receive worker.
    void received(const LinkGuid& from,const LinkPacket& packet) {
        std::unique_lock lock(mutex);
        Node* node=find(from);
        if (!node) { lock.unlock(); if (transport) transport->requestBusReset(); return; }
        const uint32_t header=get32(packet.data());
        switch (header>>30) {
        case 0: {                                            // 0x11a0: control message -> EE -4
            std::vector<uint8_t> p(12); put32(p.data(),node->index); std::memcpy(p.data()+4,packet.data()+4,8);
            queueEvent(0xfffffffcu,std::move(p)); break;
        }
        case 1: queueEvent(0xfffffffeu); break;              // a console is resetting the link
        case 2: { Message m; m.header=header; std::memcpy(m.payload.data(),packet.data()+4,PayloadBytes); enqueue(*node,m); break; }
        default: break;
        }
    }
    void busChanged(const std::vector<LinkGuid>& list) {
        std::lock_guard lock(mutex);
        members=list;
        // Consoles only left (none arrived) during a race: keep racing.
        bool newcomer=false,left=false;
        for (const auto& g:list) if (!find(g)) newcomer=true;
        for (const auto& n:nodes)
            if (n.guid!=self && !n.ghost && std::find(list.begin(),list.end(),n.guid)==list.end()) left=true;
        if (keepRacing && racing() && left && !newcomer) {
            for (auto& n:nodes) {
                if (n.guid==self || n.ghost || std::find(list.begin(),list.end(),n.guid)!=list.end()) continue;
                n.ghost=true;
                log(LogLevel::Info,"[GT3/PDI1394] console "+std::to_string(n.index)+" left during the race; its car goes on with the controller released"+
                    (n.hasLast?" after frame "+std::to_string(n.lastSeq):""));
            }
            changed.notify_all();
            return;
        }
        fullReset();
    }
    // The bus reset the original reports at once: enumerate, clear rings, EE event -1.
    void fullReset() {
        for (auto& n:nodes) n.ghost=false;
        enumerate(members); resetRings(); queueEvent(0xffffffffu);
        std::string list;
        for (const auto& n:nodes) list+=" "+std::to_string(n.index)+(n.guid==self?"(this)":"")+(n.active?"*":"");
        log(LogLevel::Info,"[GT3/PDI1394] bus reset: "+std::to_string(count)+" console(s), indexes"+list);
    }
    // After the race (the game sends menu messages, starts a new session or
    // stops exchanging), the departed consoles are reported like a real reset.
    void settleGhosts(const char* why) {
        if (!anyGhost()) return;
        log(LogLevel::Info,std::string("[GT3/PDI1394] race over (")+why+"): reporting the consoles that left");
        fullReset();
    }
    // A frame for a console that left: its last one, controller released.
    // Bytes 0x48-0x5b are the DualShock 2 report GT3 sends each frame: buttons
    // (0x4a-0x4b, 1 = up), sticks (0x4c-0x4f, 0x80 = centre), pressures.
    Message ghostFrame(const Node& node,uint32_t seq) {
        Message m; m.header=0x88000000u|(seq&0x07ffffff);
        m.payload=node.hasLast?node.lastPayload:selfNode().lastPayload;
        m.payload[0x4a]=m.payload[0x4b]=0xff;
        for (size_t i=0x4c;i<0x50;++i) m.payload[i]=0x80;
        for (size_t i=0x50;i<0x5c;++i) m.payload[i]=0;
        return m;
    }
    std::vector<uint8_t> nodeTable() const {                 // event -1 payload (0x68c)
        std::vector<uint8_t> t(0x4c,0);
        for (const auto& n:nodes) if (n.index<8) std::memcpy(t.data()+n.index*8,n.guid.data(),8);
        put32(t.data()+0x40,count); put32(t.data()+0x44,present); put32(t.data()+0x48,tableAllPresent()?1:0);
        return t;
    }

    // --- EE event delivery through the game's SIF RPC queue ---
    bool readWord(uint32_t address,uint32_t& value) const { return host.readGuest(address,&value,4); }
    bool writeWord(uint32_t address,uint32_t value) { return host.writeGuest(address,&value,4); }
    // Walk the game's queue list for SID 0x45574650 (thread 0x26e4c8's queue).
    bool findServer(uint32_t& server,uint32_t& queue) const {
        uint32_t qd=0; if (!readWord(SifRpcState+0x28,qd)) return false;
        for (int q=0;qd && q<16;++q) {
            uint32_t sd=0; if (!readWord(qd+8,sd)) return false;
            for (int i=0;sd && i<16;++i) {
                uint32_t sid=0; if (!readWord(sd,sid)) return false;
                if (sid==EeSid) { server=sd; queue=qd; return true; }
                if (!readWord(sd+0x38,sd)) return false;
            }
            if (!readWord(qd+0x14,qd)) return false;
        }
        return false;
    }
    bool packetPoolReady() {
        uint32_t count=0; if (!readWord(SifRpcState+0x18,count)) return false;
        if (count) return true;
        // Same values as the game's sceSifInitRpc (cached aliases). Only the
        // RPC reply path uses them; the runtime's own SIF RPC does not.
        return writeWord(SifRpcState+0x14,SifPacketTable) && writeWord(SifRpcState+0x18,0x20) &&
               writeWord(SifRpcState+0x1c,SifBufferTable) && writeWord(SifRpcState+0x20,0x20) && writeWord(SifRpcState+0x24,0);
    }
    bool replyArrived() const {
        uint32_t command=0; return host.readIopMemory(replyPacket+0x20,&command,4) && command==ReplyCommand;
    }
    void clearReply() {
        std::array<uint8_t,0x40> zero{}; const std::array<IopMemoryWrite,1> writes{{{replyPacket,zero}}};
        host.writeIopMemoryBatch(writes);
    }
    // What the EE's SIF command handler does with an IOP RPC request, then
    // iWakeupThread; the game's SifRpcLoop/SifExecRequest run the handler.
    void pump();
    bool post(uint32_t sd,uint32_t qd,const Event& e) {
        uint32_t function=0,buffer=0,thread=0,start=0,end=0;
        if (!readWord(sd+4,function) || !readWord(sd+8,buffer) || !readWord(qd,thread) ||
            !host.hasGuestFunction(function) || !buffer || int32_t(thread)<=0) return false;
        if (!e.payload.empty() && !host.writeGuest(buffer,e.payload.data(),e.payload.size())) return false;
        clearReply();
        const std::array<std::pair<uint32_t,uint32_t>,9> fields{{{0x0c,uint32_t(e.payload.size())},{0x1c,0},{0x20,replyPacket},
            {0x24,e.function},{0x28,0},{0x2c,0},{0x30,0},{0x34,0},{0x3c,0}}};
        for (const auto& [offset,value]:fields) if (!writeWord(sd+offset,value)) return false;
        if (!readWord(qd+0x0c,start) || !readWord(qd+0x10,end)) return false;
        if (start && end) { if (!writeWord(end+0x3c,sd)) return false; }
        else if (!writeWord(qd+0x0c,sd)) return false;
        if (!writeWord(qd+0x10,sd)) return false;
        host.wakeupEeThread(int(thread));
        return true;
    }
};

Pdi1394Link::Pdi1394Link(IopHost& host):s_(std::make_unique<State>(host)) {}
Pdi1394Link::~Pdi1394Link() { stop(); }

bool Pdi1394Link::start(uint32_t replyPacketIop) {
    auto& s=*s_;
    if (s.started || !replyPacketIop) return false;
    std::mt19937_64 random{std::random_device{}()^uint64_t(Clock::now().time_since_epoch().count())};
    const auto bits=random();
    // EUI-64 style: Sony's OUI 08-00-46, then a random device part.
    s.self={0x08,0x00,0x46,uint8_t(bits),uint8_t(bits>>8),uint8_t(bits>>16),uint8_t(bits>>24),uint8_t(bits>>32)};
    {
        std::lock_guard lock(s.mutex);
        s.replyPacket=replyPacketIop; s.stopping=false; s.nodes.clear(); s.table={}; s.present=0; s.count=0;
        s.frame=0; s.running=false; s.events.clear(); s.delivering=false;
        s.createNode(s.self);                               // 0x1824: this console, index 0
    }
    LinkTransport::Callbacks callbacks;
    callbacks.bus=[&s](const std::vector<LinkGuid>& members) { s.busChanged(members); };
    callbacks.packet=[&s](const LinkGuid& from,const LinkPacket& packet) { s.received(from,packet); };
    callbacks.log=[&s](const std::string& line) { s.log(LogLevel::Info,line); };
    // New players wait while a race runs (joining is a bus reset, which ends it).
    callbacks.admit=[&s] { return State::nowMs()>s.racingUntilMs.load(); };
    s.transport=std::make_unique<LinkTransport>(LinkTransport::configFromEnvironment(),s.self,std::move(callbacks));
    if (!s.transport->start()) {
        s.log(LogLevel::Warning,"[GT3/PDI1394] network link unavailable; i.LINK runs as a lone console");
        s.transport=std::make_unique<LinkTransport>(LinkTransport::Config{},s.self,LinkTransport::Callbacks{
            [&s](const std::vector<LinkGuid>& members) { s.busChanged(members); },{},{},{}});
        s.transport->start();
    }
    s.started=true;
    return true;
}

void Pdi1394Link::stop() {
    auto& s=*s_;
    { std::lock_guard lock(s.mutex); s.stopping=true; s.changed.notify_all(); }
    if (s.transport) { s.transport->stop(); s.transport.reset(); }
    std::lock_guard lock(s.mutex); s.started=false;
}

RpcResult Pdi1394Link::handleRpc(const RpcRequest& request) {
    auto& s=*s_;
    RpcResult result; result.serverDispatchPolicy=ServerDispatchPolicy::RejectUnhandled;
    if (!s.started || (request.sid!=ControlSid && request.sid!=ExchangeSid)) return result;
    std::vector<uint8_t> data(std::min<uint32_t>(request.send.size,0x80));
    if (!data.empty() && (!request.send.address || !s.host.readGuest(request.send.address,data.data(),data.size()))) return result;
    const uint32_t function=request.function,group=function>>30,sub=(function>>27)&7,value=function&0x07ffffff;
    result.handled=true; result.serverDispatchPolicy=ServerDispatchPolicy::Suppress;

    std::unique_lock lock(s.mutex);
    if (request.sid==ControlSid) {                           // handler 0x588
        if (group!=2) s.settleGhosts("menu message");
        if (!data.empty()) std::memcpy(s.controlBuffer.data(),data.data(),std::min<size_t>(data.size(),8));
        if (s.trace) {
            const auto text=hexWord(function)+" data "+s.bytesText(data.data(),data.size());
            if (text==s.lastControl) ++s.controlRepeats;
            else {
                s.log(LogLevel::Info,"[GT3/PDI1394] control "+text+(s.controlRepeats?" (last one repeated "+std::to_string(s.controlRepeats)+" more times)":""));
                s.lastControl=text; s.controlRepeats=0;
            }
        }
        if (group==0) s.send(0,s.controlBuffer.data(),8);
        else if (group==1) {
            s.send(0x40000000,s.controlBuffer.data(),8);
            lock.unlock(); if (s.transport) s.transport->requestBusReset(); lock.lock();
        } else if (group==3) {
            if (s.running) {
                s.running=false;
                for (auto& e:s.table) e.flags&=~2u;           // 0x1d6c
                s.resetRings();
                for (auto& n:s.nodes) n.active=false;
            }
            bool found=false;
            for (auto& n:s.nodes) if (n.index==sub) {         // 0x1ae0
                for (auto& e:s.table) if (e.guid==n.guid) e.flags|=2;
                n.active=true; found=true;
            }
            if (!found) s.log(LogLevel::Warning,"[GT3/PDI1394] activate: no console with index "+std::to_string(sub));
        }
        return result;                                       // original returns a null reply
    }

    // handler 0x388: the per-frame lockstep exchange.
    std::array<uint8_t,ReplyBytes> reply{};
    auto finish=[&](std::optional<uint32_t> marker) {
        if (marker) put32(reply.data(),*marker);
        ++s.exchanges;
        s.exchanging=false; s.lastExchange=Clock::now(); s.racingUntilMs=State::nowMs()+3000;
        if (s.dump) {
            std::array<uint8_t,8+0x80> head{}; put32(head.data(),function); put32(head.data()+4,uint32_t(data.size()));
            std::memcpy(head.data()+8,data.data(),std::min<size_t>(data.size(),0x80));
            std::fwrite(head.data(),1,head.size(),s.dump); std::fwrite(reply.data(),1,reply.size(),s.dump);
        }
        if (s.trace && (marker || sub!=1 || group!=2 || s.exchanges%600==1))
            s.log(LogLevel::Info,"[GT3/PDI1394] exchange "+hexWord(function)+(marker?" -> "+hexWord(*marker):std::string())+
                " size "+std::to_string(data.size())+" [#"+std::to_string(s.exchanges)+"] send "+s.bytesText(data.data(),std::min<size_t>(data.size(),16))+
                " slot1 "+s.bytesText(reply.data()+PayloadBytes,16));
        lock.unlock();
        if (request.receive.address && request.receive.size) {
            const auto size=std::min<uint32_t>(request.receive.size,ReplyBytes);
            const std::array<IopMemoryWrite,1> writes{{{request.receive.address,std::span(reply).first(size)}}};
            if (!s.host.commitIopAndEeMemory(writes)) { result.handled=false; result.serverDispatchPolicy=ServerDispatchPolicy::RejectUnhandled; return result; }
            result.resultAddress=request.receive.address;
        }
        return result;
    };
    auto sendFrame=[&] { s.frame=value; s.send(function,data.data(),data.size()); return finish({}); };
    s.exchanging=true; s.racingUntilMs=State::nowMs()+3000;
    if (group!=2) { s.frame=value; return finish({}); }
    if (sub==0) { s.settleGhosts("new race"); s.running=true; s.busSnapshot=s.busNodes; return sendFrame(); }
    if (sub==3) return sendFrame();
    const uint64_t epoch=s.ringEpoch;
    bool ahead=false;
    for (size_t i=0;i<s.nodes.size();++i) {
        if (!s.nodes[i].active) continue;
        const uint32_t index=s.nodes[i].index;
        const auto waitStart=Clock::now(); auto nextReport=waitStart+std::chrono::seconds(5);
        for (;;) {
            while (!s.stopping && s.ringEpoch==epoch && s.nodes[i].ring.empty() && !s.nodes[i].ghost) {
                s.changed.wait_for(lock,std::chrono::milliseconds(100));
                if (Clock::now()>=nextReport && s.ringEpoch==epoch && !s.stopping) {
                    s.log(LogLevel::Info,"[GT3/PDI1394] waiting for console "+std::to_string(index)+" (frame "+std::to_string(value-1)+")");
                    nextReport+=std::chrono::seconds(5);
                }
            }
            if (s.stopping || s.ringEpoch!=epoch) return finish(0xdeadbeafu);
            auto& node=s.nodes[i];
            // A console that left has every real frame in its ring already;
            // past those it gets released-controller frames.
            if (node.ring.empty()) node.ring.push_back(s.ghostFrame(node,value-1));
            const auto message=node.ring.front(); node.ring.pop_front();
            std::memcpy(reply.data()+index*PayloadBytes,message.payload.data(),PayloadBytes);
            const uint32_t seq=message.header&0x07ffffff;
            if (int32_t(value-1)<int32_t(seq)) { node.ring.push_back(message); ahead=true; break; }   // 0x0ddc
            if (seq==value-1) break;
        }
    }
    if (ahead) return finish(0xac1dcafeu);
    return sendFrame();
}

void Pdi1394Link::onVBlank() {
    auto& s=*s_;
    std::lock_guard lock(s.mutex);
    if (!s.started) return;
    if (!s.exchanging && s.anyGhost() && Clock::now()-s.lastExchange>std::chrono::seconds(3)) s.settleGhosts("no frames for 3 s");
    s.pump();
}

void Pdi1394Link::onSifTransfer(const SifTransfer& transfer) {
    auto& s=*s_;
    if (transfer.phase!=SifTransferPhase::AfterCopy || transfer.kind!=SifTransferKind::SetDma) return;
    std::lock_guard lock(s.mutex);
    // The game's SifExecRequest just sent our reply packet: the handler is
    // done, so the next event can go straight onto the queue it returns to.
    if (s.started && s.delivering && transfer.destinationAddress==s.replyPacket) s.pump();
}

void Pdi1394Link::State::pump() {
    auto& s=*this;
    if (s.delivering) {
        if (s.replyArrived()) {
            if (s.trace && !s.events.empty() && s.events.front().function!=0xfffffffcu)
                s.log(LogLevel::Info,"[GT3/PDI1394] EE took event "+hexWord(s.events.front().function)+" "+
                    s.bytesText(s.events.front().payload.data(),s.events.front().payload.size()));
            s.delivering=false; ++s.delivered; s.clearReply();
            if (!s.events.empty()) s.events.pop_front();
        } else if (Clock::now()-s.deliverStart>std::chrono::seconds(10)) {
            s.log(LogLevel::Warning,"[GT3/PDI1394] EE did not answer event "+hexWord(s.events.empty()?0:s.events.front().function)+"; dropped");
            s.delivering=false; if (!s.events.empty()) s.events.pop_front();
        } else {
            // Still queued: the RPC thread was running when woken; wake it again.
            uint32_t qd=0,sd=0,start=0,thread=0;
            if (s.findServer(sd,qd) && s.readWord(qd+0x0c,start) && start==sd && s.readWord(qd,thread)) s.host.wakeupEeThread(int(thread));
            return;
        }
    }
    if (s.events.empty()) return;
    uint32_t sd=0,qd=0;
    if (!s.findServer(sd,qd)) {
        if (!s.missingLogged) { s.log(LogLevel::Info,"[GT3/PDI1394] EE i.LINK callback service not registered yet; events wait"); s.missingLogged=true; }
        return;
    }
    if (!s.packetPoolReady()) return;
    auto& event=s.events.front();
    if (event.function==0xffffffffu) { s.resetRings(); event.payload=s.nodeTable(); }   // 0x068c
    if (!s.post(sd,qd,event)) {
        s.log(LogLevel::Warning,"[GT3/PDI1394] could not queue event "+hexWord(event.function)+" for the EE; dropped");
        s.events.pop_front(); return;
    }
    s.delivering=true; s.deliverStart=Clock::now();
}

Pdi1394Link::Status Pdi1394Link::status() const {
    auto& s=*s_; std::lock_guard lock(s.mutex); Status st;
    st.consoles=s.count?s.count:uint32_t(s.nodes.size()); st.frame=s.frame; st.running=s.running; st.eventsDelivered=s.delivered;
    for (const auto& n:s.nodes) if (n.active) st.activeMask|=1u<<n.index;
    return st;
}
}
