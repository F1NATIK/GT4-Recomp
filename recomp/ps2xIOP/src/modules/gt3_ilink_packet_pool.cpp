#include "ps2x/iop/gt3/ilink_packet_pool.h"
#include "ps2x/iop/gt3/ilink_startup_image.h"
#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <vector>

namespace ps2x::iop::gt3 {
namespace {
std::atomic<uint64_t> eventHandles{0x6b000000},tokens{1};
uint64_t nextToken() {
    auto value=tokens.load();
    while (value!=UINT64_MAX) if (tokens.compare_exchange_weak(value,value+1)) return value;
    return 0;
}
constexpr std::array<uint32_t,5> fields{0x17294,0x172a4,0x172a8,0x172ac,0x172b0};
void put(std::span<uint8_t> bytes,size_t at,uint32_t value) {
    for (unsigned i=0;i<4;++i) bytes[at+i]=uint8_t(value>>(8*i));
}
std::span<const uint8_t> bytesOf(const uint32_t& value) {
    return {reinterpret_cast<const uint8_t*>(&value),4};
}

}
struct IlinkPacketPool::State {
    IopHost& host;
    mutable std::recursive_mutex mutex;
    std::condition_variable_any changed;
    Snapshot data;
    std::vector<unsigned> free;
    std::array<uint64_t,Capacity> owners{};
    bool running=false,busy=false;
    explicit State(IopHost& h):host(h) {}
    struct Busy {
        State& s;
        explicit Busy(State& state):s(state) { s.busy=true; }
        ~Busy() { s.busy=false; }
    };
    uint32_t record(unsigned index) const { return data.records+index*RecordBytes; }
    std::array<uint32_t,5> metadata() const {
        return {data.event,free.empty()?0:record(free.back()),uint32_t(free.size()),data.payloads,data.records};
    }
    bool wordIs(uint32_t address,uint32_t value) const {
        uint32_t found=0; return host.readIopMemory(address,&found,4) && found==value;
    }
    bool valid() const {
        if (!running || !data.image) return false;
        const auto values=metadata();
        for (size_t i=0;i<fields.size();++i) if (!wordIs(data.image+fields[i],values[i])) return false;
        const std::array<uint32_t,4> event{0,data.eventHandle,0,data.image+0x11f68};
        std::array<uint32_t,4> found{};
        if (!host.readIopMemory(data.event,found.data(),sizeof(found)) || found!=event) return false;
        std::array<uint32_t,Capacity> next{};
        for (size_t i=1;i<free.size();++i) next[free[i]]=record(free[i-1]);
        uint8_t end=0;
        if (!host.readIopMemory(data.payloads+Capacity*PayloadBytes-1,&end,1)) return false;
        for (unsigned i=0;i<Capacity;++i)
            if (!wordIs(record(i),data.payloads+i*PayloadBytes) || !wordIs(record(i)+0x28,next[i])) return false;
        return true;
    }
    void invalidate() { running=false; changed.notify_all(); }
    void dispose() {
        invalidate();
        for (auto address:{data.event,data.payloads,data.records}) if (address) host.freeIopMemory(address);
        // Waiters use generation, not the cleared counter, to detect restart.
        const auto waiters=data.waiters; data={}; data.waiters=waiters; free.clear(); owners={};
    }
    std::optional<Lease> take() {
        if (busy || !running) return {};
        Busy guard(*this);
        if (!valid()) { invalidate(); return {}; }
        if (free.empty()) return {};
        const auto token=nextToken(); if (!token) { invalidate(); return {}; }
        const auto index=free.back();
        const std::array<uint32_t,3> values{free.size()>1?record(free[free.size()-2]):0,uint32_t(free.size()-1),0};
        const std::array<uint32_t,3> addresses{data.image+0x172a4,data.image+0x172a8,record(index)+0x28};
        std::array<IopMemoryWrite,3> writes{};
        for (size_t i=0;i<3;++i) writes[i]={addresses[i],bytesOf(values[i])};
        struct Failed { State& s; bool done=false; ~Failed() { if (!done) s.invalidate(); } } result{*this};
        if (!host.writeIopMemoryBatch(writes)) return {};
        for (size_t i=0;i<3;++i) if (!wordIs(addresses[i],values[i])) return {};
        free.pop_back(); owners[index]=token;
        result.done=true; return Lease{record(index),data.payloads+index*PayloadBytes,token};
    }
};
IlinkPacketPool::IlinkPacketPool(IopHost& host):state_(std::make_shared<State>(host)) {}
IlinkPacketPool::~IlinkPacketPool() { stop(); }
bool IlinkPacketPool::start(uint32_t image) {
    auto& s=*state_; std::lock_guard lock(s.mutex);
    if (s.busy || s.running || s.data.image) return false;
    State::Busy guard(s);
    struct Transaction { State& s; bool done=false; ~Transaction() { if (!done) s.dispose(); } } transaction{s};
    if (!image || (image&3) || image>UINT32_MAX-ModelBytes || !verifyIlinkModule(s.host)) return false;
    std::vector<uint8_t> model(ModelBytes);
    if (!s.host.readIopMemory(image,model.data(),model.size())) return false;
    if (!s.wordIs(image+0x16d5c,Capacity) || !s.wordIs(image+0x16d60,64) ||
        !s.wordIs(image+0x16ba0,UINT32_MAX) || !s.wordIs(image+0x175a8,0)) return false;
    for (auto offset:fields) if (!s.wordIs(image+offset,0)) return false;
    s.data.image=image;
    s.data.event=s.host.allocateIopMemory(16);
    s.data.payloads=s.host.allocateIopMemory(Capacity*PayloadBytes);
    s.data.records=s.host.allocateIopMemory(Capacity*RecordBytes);
    const auto handle=eventHandles.fetch_add(1),generation=nextToken();
    if (!s.data.event || !s.data.payloads || !s.data.records || handle>=0x6c000000 || !generation) return false;
    s.data.eventHandle=uint32_t(handle); s.data.generation=generation;
    std::array<uint8_t,16> event{}; put(event,4,s.data.eventHandle); put(event,12,image+0x11f68);
    std::vector<uint8_t> payloads(Capacity*PayloadBytes),records(Capacity*RecordBytes);
    for (unsigned i=0;i<Capacity;++i) {
        put(records,i*RecordBytes,s.data.payloads+i*PayloadBytes);
        put(records,i*RecordBytes+0x28,i+1<Capacity?s.record(i+1):0);
        s.free.push_back(Capacity-1-i);
    }
    const auto metadata=s.metadata();
    std::array<IopMemoryWrite,8> writes{{{s.data.event,event},{s.data.payloads,payloads},{s.data.records,records}}};
    for (size_t i=0;i<5;++i) writes[i+3]={image+fields[i],bytesOf(metadata[i])};
    if (!s.host.writeIopMemoryBatch(writes)) return false;
    std::vector<uint8_t> check(records.size());
    if (!s.host.readIopMemory(s.data.records,check.data(),check.size()) || check!=records) return false;
    check.resize(payloads.size());
    if (!s.host.readIopMemory(s.data.payloads,check.data(),check.size()) || check!=payloads) return false;
    s.running=true;
    if (!s.valid()) return false;
    transaction.done=true; return true;
}
bool IlinkPacketPool::stop() {
    auto& s=*state_; std::lock_guard lock(s.mutex); if (s.busy) return false;
    State::Busy guard(s); s.dispose(); return true;
}
bool IlinkPacketPool::active() const { return snapshot().has_value(); }
std::optional<IlinkPacketPool::Snapshot> IlinkPacketPool::snapshot() const {
    auto& s=*state_; std::lock_guard lock(s.mutex); if (s.busy) return {};
    State::Busy guard(s); if (!s.valid()) return {};
    auto result=s.data; result.freeCount=uint32_t(s.free.size()); return result;
}
std::optional<IlinkPacketPool::Lease> IlinkPacketPool::tryAcquire() {
    auto& s=*state_; std::lock_guard lock(s.mutex); return s.take();
}
std::optional<IlinkPacketPool::Lease> IlinkPacketPool::acquire(std::stop_token stop) {
    const auto state=state_; auto& s=*state; std::unique_lock lock(s.mutex);
    if (s.busy || !s.running || stop.stop_requested()) return {};
    const auto generation=s.data.generation;
    if (auto lease=s.take()) return lease;
    if (!s.running) return {};
    ++s.data.waiters;
    struct Waiting { State& s; ~Waiting() { --s.data.waiters; } } waiting{s};
    s.changed.wait(lock,stop,[&] { return !s.running || s.data.generation!=generation || !s.free.empty(); });
    if (stop.stop_requested() || !s.running || s.data.generation!=generation) return {};
    return s.take();
}
bool IlinkPacketPool::release(Lease lease) {
    auto& s=*state_; std::lock_guard lock(s.mutex); if (s.busy || !s.running) return false;
    State::Busy guard(s);
    if (!s.valid()) { s.invalidate(); return false; }
    if (lease.record<s.data.records || lease.record>=s.data.records+Capacity*RecordBytes ||
        (lease.record-s.data.records)%RecordBytes) return false;
    const auto index=(lease.record-s.data.records)/RecordBytes;
    if (!lease.token || lease.token!=s.owners[index] || lease.payload!=s.data.payloads+index*PayloadBytes) return false;
    const std::array<uint32_t,3> values{lease.record,uint32_t(s.free.size()+1),s.free.empty()?0:s.record(s.free.back())};
    const std::array<uint32_t,3> addresses{s.data.image+0x172a4,s.data.image+0x172a8,lease.record+0x28};
    std::array<IopMemoryWrite,3> writes{};
    for (size_t i=0;i<3;++i) writes[i]={addresses[i],bytesOf(values[i])};
    struct Failed { State& s; bool done=false; ~Failed() { if (!done) s.invalidate(); } } result{s};
    if (!s.host.writeIopMemoryBatch(writes)) return false;
    for (size_t i=0;i<3;++i) if (!s.wordIs(addresses[i],values[i])) return false;
    const bool signal=s.free.empty(); s.free.push_back(index); s.owners[index]=0;
    if (signal) ++s.data.signals;
    result.done=true; s.changed.notify_all(); return true;
}
}
