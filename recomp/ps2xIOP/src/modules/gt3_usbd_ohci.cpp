#include "ps2x/iop/gt3/usbd_ohci.h"
#include <algorithm>

namespace ps2x::iop::gt3 {
namespace {
uint32_t word(const uint8_t* b) { return uint32_t(b[0])|(uint32_t(b[1])<<8)|(uint32_t(b[2])<<16)|(uint32_t(b[3])<<24); }
struct Busy { bool& flag; explicit Busy(bool& f):flag(f) { flag=true; } ~Busy() { flag=false; } };
constexpr uint32_t InterruptMask=0xc000007f;
}
void UsbdOhci::softwareReset(State& state) {
    // HCR is synchronous in this native model. Clear operational state before
    // acknowledging; retain root-hub state, and never clear guest-owned RAM.
    std::fill_n(state.reg.begin(),0x48/4,0);
    state.reg[0]=0x10; state.reg[4/4]=0xc0;
    state.reg[0x34/4]=0x2edf; state.reg[0x44/4]=0x628;
}
bool UsbdOhci::validStorage() const {
    const auto a=storage_.addresses(); if (!a[0] || !a[1] || !a[2]) return false;
    std::array<uint8_t,4> b{};
    return host_.readIopMemory(a[0]+0x57c0,b.data(),b.size()) && word(b.data())==a[1]+0x2120 &&
        host_.readIopMemory(a[1]+0xfc,b.data(),b.size()) && word(b.data())==a[0]+0x5880 &&
        host_.readIopMemory(a[1]+UsbdStorage::ArenaBytes-4,b.data(),b.size()) &&
        host_.readIopMemory(a[2]+UsbdStorage::HubBytes-4,b.data(),b.size());
}
bool UsbdOhci::skippedList(uint32_t address) const {
    const auto arena=storage_.addresses()[1];
    // Restrict DMA descriptors to the owned hardware ED region. Never follow an
    // EE pointer, uncached alias, software record, or unbounded guest cycle.
    std::array<uint32_t,130> visited{}; size_t count=0;
    while (address) {
        if ((address&15) || address<arena+0x1900 || address>=arena+0x2120 || count==visited.size() ||
            std::find(visited.begin(),visited.begin()+count,address)!=visited.begin()+count) return false;
        visited[count++]=address;
        std::array<uint8_t,16> ed{};
        if (!host_.readIopMemory(address,ed.data(),ed.size()) || !(word(ed.data())&0x4000)) return false;
        address=word(ed.data()+12);
    }
    return true;
}
bool UsbdOhci::applyWrite(State& s,uint32_t offset,uint32_t value) const {
    auto& r=s.reg;
    switch (offset) {
    case 0x04:
        // Interrupt routing, remote wake and timed resume are not modeled.
        if ((value&~0xffu) || (value&0xc0)==0x40) return false;
        r[1]=value; return true;
    case 0x08:
        if (value&~1u) return false; // CLF/BLF/ownership execution unsupported.
        if (value&1) softwareReset(s);
        return true;
    case 0x0c: r[3]&=~(value&0x4000007f); return true; // W1C, no software IRQ injection.
    case 0x10: r[4]|=value&InterruptMask; return true;
    case 0x14: r[4]&=~(value&InterruptMask); return true;
    case 0x18:
        if (value && value!=storage_.addresses()[1]) return false;
        r[6]=value; return true;
    case 0x20: case 0x28:
        if (!skippedList(value)) return false;
        r[offset/4]=value; return true;
    case 0x34:
        if (value&0xc000) return false;
        r[offset/4]=value; return true;
    case 0x40:
        if (value&~0x3fffu) return false;
        r[offset/4]=value; return true;
    case 0x44:
        if (value&~0xfffu) return false;
        r[offset/4]=value; return true;
    case 0x50:
        if (value&~0x80038001u) return false;
        // Fixed always-powered profile: global power and overcurrent ack no-op.
        if (value&0x8000) r[offset/4]|=0x8000;
        if (value&0x80000000) r[offset/4]&=~0x8000u;
        return true;
    case 0x54: case 0x58: {
        if (value&~0x1f031fu) return false;
        auto& port=r[offset/4]; const auto previous=port;
        port&=~(value&0x1f0000); // Clear change latches only.
        // Enabling, suspending or resetting a disconnected port sets CSC. It
        // must never create a connection, port enable or successful reset.
        if (value&0x16) port|=0x10000;
        if ((port&~previous)&0x1f0000) r[3]|=0x40;
        return true;
    }
    default: return false;
    }
}
bool UsbdOhci::start() {
    std::lock_guard lock(mutex_);
    if (inFlight_) return false;
    if (active_) return validStorage();
    Busy busy(inFlight_); const auto before=generation_;
    struct Rollback { UsbdOhci& self; bool done=false; ~Rollback() { if (!done) self.reset(); } } rollback{*this};
    if (!storage_.prepare() || generation_!=before || !validStorage()) return false;
    State staged;
    // Native profile choice, not a claim about physical PS2 power-on values:
    // two removable, always-powered ports with no overcurrent source.
    staged.reg[0x48/4]=0x1202; staged.reg[0x54/4]=staged.reg[0x58/4]=0x100;
    const auto arena=storage_.addresses()[1];
    const std::array<std::pair<uint32_t,uint32_t>,10> setup{{
        {0x14,UINT32_MAX},{0x08,1},{0x04,0},
        {0x20,arena+0x20f0},{0x28,arena+0x2100},{0x18,arena},
        {0x34,0x27782edf},{0x40,0x2a2f},{0x10,0x80000073},{0x04,0xbf}}};
    for (auto [offset,value]:setup) if (!applyWrite(staged,offset,value)) return false;
    // Validate every periodic entry before exposing configured state.
    std::array<uint8_t,128> hcca{};
    if (!host_.readIopMemory(arena,hcca.data(),hcca.size())) return false;
    for (size_t i=0;i<32;++i) if (!skippedList(word(hcca.data()+i*4))) return false;
    if (generation_!=before) return false;
    state_=staged; active_=true; rollback.done=true; return true;
}
void UsbdOhci::reset() {
    std::lock_guard lock(mutex_); ++generation_;
    active_=false; state_={}; storage_.reset();
}
bool UsbdOhci::active() const { std::lock_guard lock(mutex_); return active_ && !inFlight_ && validStorage(); }
std::array<uint32_t,3> UsbdOhci::addresses() const {
    std::lock_guard lock(mutex_); return active_ && !inFlight_ && validStorage()?storage_.addresses():std::array<uint32_t,3>{};
}
std::optional<uint32_t> UsbdOhci::read(uint32_t address) const {
    std::lock_guard lock(mutex_);
    if (!active_ || inFlight_ || !validStorage()) return {};
    // These expose owned USB enable bits only, not a complete shared DMA bank.
    if (address==0xbf801570) return 0x08000000;
    if (address==0xbf801680) return 1;
    if (address<Base || address>=Base+0x5c || (address&3)) return {};
    const auto offset=address-Base;
    if (offset==0x38) return {}; // Sub-frame countdown not implemented.
    return state_.reg[offset==0x14?0x10/4:offset/4];
}
bool UsbdOhci::write(uint32_t address,uint32_t value) {
    std::lock_guard lock(mutex_);
    if (!active_ || inFlight_ || !validStorage() || address<Base || address>=Base+0x5c || (address&3)) return false;
    return applyWrite(state_,address-Base,value);
}
bool UsbdOhci::irqPending() const {
    std::lock_guard lock(mutex_);
    return active_ && !inFlight_ && validStorage() && (state_.reg[4]&0x80000000) && (state_.reg[3]&state_.reg[4]&0x4000007f);
}
bool UsbdOhci::advanceFrame() {
    std::lock_guard lock(mutex_);
    if (!active_ || inFlight_ || !validStorage() || (state_.reg[1]&0xc0)!=0x80) return false;
    Busy busy(inFlight_); const auto before=generation_;
    const auto arena=storage_.addresses()[1]; auto staged=state_;
    if (staged.reg[6]!=arena) return false;
    const auto previous=staged.reg[0x3c/4],next=(previous+1)&0xffff;
    if (staged.reg[1]&4) {
        std::array<uint8_t,4> entry{};
        if (!host_.readIopMemory(arena+(next&31)*4,entry.data(),entry.size()) || !skippedList(word(entry.data()))) return false;
    }
    if ((staged.reg[1]&0x10) && !skippedList(staged.reg[0x20/4])) return false;
    if ((staged.reg[1]&0x20) && !skippedList(staged.reg[0x28/4])) return false;
    // HCCA frame number is a 16-bit field; pad at +0x82 and done-head +0x84
    // belong to distinct fields and must not be overwritten by this commit.
    const std::array<uint8_t,2> number{uint8_t(next),uint8_t(next>>8)};
    const IopMemoryWrite write{arena+0x80,number};
    if (!host_.writeIopMemoryBatch(std::span(&write,1)) || generation_!=before) return false;
    staged.reg[0x3c/4]=next; staged.reg[3]|=4;
    if ((previous^next)&0x8000) staged.reg[3]|=0x20;
    state_=staged; return true;
}
}
