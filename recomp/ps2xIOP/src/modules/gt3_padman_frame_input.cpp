#include "ps2x/iop/gt3/padman_frame_input.h"
#include "ps2x/iop/native_library.h"
#include <algorithm>

namespace ps2x::iop::gt3 {
namespace {
void put(auto& bytes,size_t offset,uint32_t value) {
    for (unsigned i=0;i<4;++i) bytes[offset+i]=uint8_t(value>>(i*8));
}
uint32_t word(const auto& bytes,size_t offset) {
    return uint32_t(bytes[offset])|(uint32_t(bytes[offset+1])<<8)|
        (uint32_t(bytes[offset+2])<<16)|(uint32_t(bytes[offset+3])<<24);
}
bool eeBuffer(IopHost& host,uint32_t address) {
    uint32_t begin=0,end=0;
    return address && !(address&63u) && address<=0xffffff00u &&
        host.normalizeGuestAddress(address,begin) && host.normalizeGuestAddress(address+255u,end) &&
        begin<=0x02000000u-256u && end==begin+255u;
}
constexpr uint32_t TransferBytes=0xd4,SendAt=0x94,ReceiveAt=0xb4;
bool commit(IopHost& host,std::vector<IopMemoryWrite> writes,std::span<const IopMemoryWrite> replies) {
    writes.insert(writes.end(),replies.begin(),replies.end());
    return host.commitIopAndEeMemory(writes);
}
}
struct PadmanFrameInput::State {
    IopHost& host;
    uint32_t image=0,transfer=0;
    bool open=false,seenStart=false;
    uint64_t lastTick=0;
    PadmanPortState port;
    explicit State(IopHost& h):host(h) {}
    // The port structure without the EE's actuator slots (+0x80..+0xbf), which
    // the EE writes by DMA at any time.
    std::array<IopMemoryWrite,2> portWrites(const PadmanPortState& state) const {
        const auto& bytes=state.portState();
        return {{{image+0x7d60,std::span<const uint8_t>(bytes.data(),PadmanActSlotsOffset)},
                 {image+0x7d60+PadmanActSlotsOffset+PadmanActSlotsBytes,
                  std::span<const uint8_t>(bytes.data()+PadmanActSlotsOffset+PadmanActSlotsBytes,
                                           PadmanPortBytes-PadmanActSlotsOffset-PadmanActSlotsBytes)}}};
    }
};
PadmanFrameInput::PadmanFrameInput(IopHost& host):state_(std::make_unique<State>(host)) {}
PadmanFrameInput::~PadmanFrameInput() { reset(); }
bool PadmanFrameInput::initialized() const { return state_->image!=0; }
bool PadmanFrameInput::opened() const { return state_->open; }
uint32_t PadmanFrameInput::portAddress() const { return state_->image?state_->image+0x7d60:0; }
void PadmanFrameInput::reset() {
    auto& s=*state_;
    if (s.transfer) s.host.freeIopMemory(s.transfer);
    s.transfer=s.image=0; s.open=s.seenStart=false; s.lastTick=0; s.port.reset();
}
bool PadmanFrameInput::initialize(uint32_t image,std::span<const IopMemoryWrite> replies) {
    auto& s=*state_;
    if (s.image || !image) return false;
    std::array<uint8_t,0x9a00> previous{};
    if (!s.host.readIopMemory(image,previous.data(),previous.size())) return false;
    auto* libraries=s.host.nativeLibraries();
    if (!libraries) return false;
    for (uint16_t ordinal:{23,25,52}) if (!libraries->bind("sio2man",0x203,ordinal).registration) return false;
    // 0x94-byte SIO2 packet, then 32-byte send and receive buffers.
    const auto transfer=s.host.allocateIopMemory(TransferBytes);
    if (!transfer) return false;
    struct Rollback { IopHost& host; uint32_t address; ~Rollback() { if (address) host.freeIopMemory(address); } } rollback{s.host,transfer};
    std::array<uint8_t,TransferBytes> packet{};
    if (!s.host.readIopMemory(transfer,packet.data(),packet.size())) return false;
    packet.fill(0);
    std::array<uint8_t,0x2c> globals{}; //8ba0..8bcb: no open slots, native frame pipeline ready.
    put(globals,0x10,1); put(globals,0x24,1);
    std::array<uint8_t,4> diagnostic{}; put(diagnostic,0,1);
    if (!commit(s.host,{{transfer,packet},{image+0x8ba0,globals},{image+0x7d38,diagnostic}},replies)) return false;
    rollback.address=0; s.image=image; s.transfer=transfer;
    return true;
}
bool PadmanFrameInput::open(uint32_t address,std::span<const IopMemoryWrite> replies) {
    auto& s=*state_;
    if (!s.image || s.open || !eeBuffer(s.host,address)) return false;
    PadmanPortState next;
    if (!next.open(address)) return false;
    std::array<uint8_t,4> slots{}; put(slots,0,1);
    if (!commit(s.host,{{s.image+0x7d60,next.portState()},{s.image+0x8ba0,slots}},replies)) return false;
    s.port=next; s.open=true; return true;
}
bool PadmanFrameInput::close(std::span<const IopMemoryWrite> replies) {
    auto& s=*state_;
    if (!s.image || !s.open) return false;
    std::array<uint8_t,4> zero{};
    if (!commit(s.host,{{s.image+0x8ba0,zero}},replies)) return false;
    s.open=false; s.port.reset(); return true;
}
bool PadmanFrameInput::end(std::span<const IopMemoryWrite> replies) {
    auto& s=*state_;
    if (!s.image) return false;
    std::array<uint8_t,0x2c> globals{};
    if (!commit(s.host,{{s.image+0x8ba0,globals}},replies)) return false;
    reset(); return true;
}
bool PadmanFrameInput::onVBlank(bool start,uint64_t tick) {
    auto& s=*state_;
    if (!s.image) return false;
    if (!start) {
        if (!s.seenStart || tick!=s.lastTick) return false;
        std::array<uint8_t,4> end{}; put(end,0,1);
        const std::array<IopMemoryWrite,1> writes{{{s.image+0x7d3c,end}}};
        return s.host.writeIopMemoryBatch(writes);
    }
    if (s.seenStart && tick<=s.lastTick) return false;
    // Deduplicate actual frame events even if a device/publication fails.
    s.seenStart=true; s.lastTick=tick;
    std::array<uint8_t,8> frameGlobals{};
    if (!s.host.readIopMemory(s.image+0x7d3c,frameGlobals.data(),frameGlobals.size())) return false;
    put(frameGlobals,0,0); put(frameGlobals,4,word(frameGlobals,4)+1u);
    if (!s.open) return commit(s.host,{{s.image+0x7d3c,frameGlobals}},{});
    auto* registry=s.host.nativeLibraries();
    if (!registry) return false;
    const auto begin=registry->bind("sio2man",0x203,23),exchange=registry->bind("sio2man",0x203,25),end=registry->bind("sio2man",0x203,52);
    if (!begin.registration || !exchange.registration || !end.registration ||
        begin.registration!=exchange.registration || begin.registration!=end.registration) return false;
    PadmanRequest request;
    if (!s.port.request(request)) return false;
    std::array<uint8_t,TransferBytes> packet{};
    put(packet,4,request.portControl1); put(packet,0x14,request.portControl2); put(packet,0x28,request.sendControl());
    put(packet,0x6c,request.size); put(packet,0x70,request.size);
    put(packet,0x74,s.transfer+SendAt); put(packet,0x78,s.transfer+ReceiveAt);
    std::copy_n(request.bytes.begin(),request.size,packet.begin()+SendAt);
    const std::array<IopMemoryWrite,1> transferWrite{{{s.transfer,packet}}};
    if (!s.host.writeIopMemoryBatch(transferWrite) || !registry->invoke(begin,{}).handled) return false;
    bool completed=false;
    {
        struct Finish { NativeLibraryRegistry& registry; NativeImportBinding binding; ~Finish() { registry.invoke(binding,{}); } } finish{*registry,end};
        const auto result=registry->invoke(exchange,{s.transfer,0,0,0});
        completed=result.handled && result.value==1;
    }
    std::array<uint8_t,PadmanActSlotsBytes> slots{};
    if (!completed || !s.host.readIopMemory(s.transfer,packet.data(),packet.size()) ||
        !s.host.readIopMemory(s.image+0x7d60+PadmanActSlotsOffset,slots.data(),slots.size())) return false;
    auto next=s.port; PadmanPublication publication; int consumed=-1;
    if (!next.complete(word(packet,0),std::span<const uint8_t>(packet.data()+ReceiveAt,request.size),publication,slots,&consumed))
        return false;
    const auto ports=s.portWrites(next);
    const std::array<uint8_t,4> taken{};
    std::vector<IopMemoryWrite> writes{ports[0],ports[1],
        {publication.eeTarget,std::span<const uint8_t>(publication.port.data(),PadmanFrameBytes)},
        {s.image+0x7d3c,frameGlobals}};
    // The consumed EE actuator slot's pending word (0x14 clears it).
    if (consumed>=0) writes.push_back({s.image+0x7d60+PadmanActSlotsOffset+uint32_t(consumed)*0x20+4,taken});
    if (!commit(s.host,std::move(writes),{})) return false;
    const auto before=s.port.portState()[0xc1],after=next.portState()[0xc1];
    if (after && after!=before) {
        constexpr char hex[]="0123456789abcdef";
        s.host.log(LogLevel::Info,std::string("[GT3/PADMAN] port0 controller identity ")+hex[after>>4]+hex[after&15]+
            (after==0x41?" (digital)":after==0x73?" (analog)":after==0x79?" (analog, pressure buttons)":""));
    }
    s.port=next; return true;
}
bool PadmanFrameInput::command(std::array<uint8_t,0x80>& packet,std::span<const uint32_t> replyAddresses) {
    auto& s=*state_;
    if (!s.image) return false;
    const auto op=word(packet,0),port=word(packet,4),slot=word(packet,8);
    if (op<3 || op>11 || port>1 || slot>3) return false;
    // Unopened ports have worker mode 0: every original function answers 0.
    const bool live=s.open && !port && !slot;
    auto next=s.port;
    const auto arg=[&](size_t offset) { return word(packet,offset); };
    const auto* data=packet.data()+0xc;
    int32_t value=0;
    if (live) switch (op) {
    case 3: value=next.infoAct(int32_t(arg(0xc)),int32_t(arg(0x10))); break;
    case 4: value=next.infoComb(int32_t(arg(0xc)),int32_t(arg(0x10))); break;
    case 5: value=next.infoMode(int32_t(arg(0xc)),int32_t(arg(0x10))); break;
    case 6: value=next.setMainMode(arg(0xc),arg(0x10)); break;
    case 7: value=next.setActDirect(std::span<const uint8_t,6>(data,6)); break;
    case 8: value=next.setActAlign(std::span<const uint8_t,6>(data,6)); break;
    case 9: value=int32_t(next.buttonMask()); break;
    case 10: value=next.setButtonInfo(arg(0xc)); break;
    case 11: value=next.setVrefParam(std::span<const uint8_t,12>(data,12)); break;
    }
    // Handlers 7198..73a0 store the result at +0x14, except RPC9 (+0xc),
    // RPC10 (+0x10) and RPC11 (+0x1c).
    put(packet,op==9?0xc:op==10?0x10:op==11?0x1c:0x14,uint32_t(value));
    std::vector<IopMemoryWrite> writes;
    for (auto address:replyAddresses) writes.push_back({address,packet});
    if (live) { const auto ports=s.portWrites(next); writes.insert(writes.end(),ports.begin(),ports.end()); }
    if (!s.host.commitIopAndEeMemory(writes)) return false;
    if (live) s.port=next;
    return true;
}
}
