#include "ps2x/iop/gt3/ilink_timer_pool.h"
#include "ps2x/iop/gt3/ilink_startup_image.h"
#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <vector>

namespace ps2x::iop::gt3 {
namespace {
std::atomic<uint64_t> eventHandles{0x6c000000},tokens{1};
uint64_t nextToken() {
    auto value=tokens.load();
    while (value!=UINT64_MAX) if (tokens.compare_exchange_weak(value,value+1)) return value;
    return 0;
}
void put(std::span<uint8_t> bytes,size_t at,uint32_t value) {
    for (unsigned i=0;i<4;++i) bytes[at+i]=uint8_t(value>>(8*i));
}
std::span<const uint8_t> bytesOf(const uint32_t& value) {
    return {reinterpret_cast<const uint8_t*>(&value),4};
}
}

struct IlinkTimerPool::State {
    IopHost& host;
    mutable std::recursive_mutex mutex;
    std::condition_variable_any changed;
    Snapshot data;
    std::array<uint64_t,Capacity> owners{};
    bool running=false,busy=false;
    explicit State(IopHost& h):host(h) {}
    struct Busy {
        State& s;
        explicit Busy(State& state):s(state) { s.busy=true; }
        ~Busy() { s.busy=false; }
    };
    void invalidate() { running=false; changed.notify_all(); }
    struct Mutation {
        State& s; bool done=false;
        ~Mutation() { if (!done) s.invalidate(); }
    };
    bool wordIs(uint32_t address,uint32_t value) const {
        uint32_t found=0; return host.readIopMemory(address,&found,4) && found==value;
    }
    std::array<uint8_t,0x281> control() const {
        //172c0 associated pointers,173c0 contexts,174c0 armed flags,
        //17500 slot ownership flags,17540 signed allocation cursor.
        std::array<uint8_t,0x281> bytes{};
        for (unsigned i=0;i<ContextCount;++i) put(bytes,0x100+i*4,data.contexts[i]);
        for (unsigned i=0;i<Capacity;++i) bytes[0x240+i]=owners[i]?1:0;
        bytes[0x280]=uint8_t(data.cursor); return bytes;
    }
    bool valid() const {
        if (!running || !wordIs(data.image+0x172a0,data.event)) return false;
        const std::array<uint32_t,4> expected{0,data.eventHandle,0,data.image+0x11f68};
        std::array<uint32_t,4> found{};
        if (!host.readIopMemory(data.event,found.data(),sizeof(found)) || found!=expected) return false;
        std::array<uint8_t,0x281> bytes{};
        if (!host.readIopMemory(data.image+0x172c0,bytes.data(),bytes.size()) || bytes!=control()) return false;
        const std::array<uint32_t,3> context{data.image+0x12f78,0,0};
        for (auto address:data.contexts) {
            std::array<uint32_t,3> actual{};
            if (!host.readIopMemory(address,actual.data(),sizeof(actual)) || actual!=context) return false;
        }
        return true;
    }
    void dispose() {
        invalidate();
        if (data.event) host.freeIopMemory(data.event);
        for (auto address:data.contexts) if (address) host.freeIopMemory(address);
        const auto waiters=data.waiters; data={}; data.waiters=waiters; owners={};
    }
    bool commitCursor(int32_t cursor,std::optional<std::pair<unsigned,uint8_t>> slot={}) {
        const uint8_t byte=uint8_t(cursor);
        std::array<IopMemoryWrite,2> writes{{{data.image+0x17540,{&byte,1}}}};
        if (slot) writes[1]={data.image+0x17500+slot->first,{&slot->second,1}};
        Mutation mutation{*this};
        const auto count=slot?2u:1u;
        if (!host.writeIopMemoryBatch(std::span(writes).first(count))) return false;
        for (unsigned i=0;i<count;++i) {
            uint8_t actual=0;
            if (!host.readIopMemory(writes[i].address,&actual,1) || actual!=writes[i].bytes[0]) return false;
        }
        mutation.done=true; return true;
    }
    std::optional<Lease> take() {
        if (busy || !running) return {};
        Busy guard(*this);
        if (!valid()) { invalidate(); return {}; }
        const auto first=data.cursor<0?0u:uint32_t(data.cursor);
        for (unsigned offset=0;offset<Capacity;++offset) {
            const auto index=(first+offset)%Capacity;
            if (owners[index]) continue;
            const auto token=nextToken(); if (!token) { invalidate(); return {}; }
            const auto next=(index+1)%Capacity;
            if (!commitCursor(next,std::pair<unsigned,uint8_t>{index,1})) return {};
            data.cursor=next; owners[index]=token;
            return Lease{index,data.contexts[index],token};
        }
        if (data.cursor!=-1) {
            if (!commitCursor(-1)) return {};
            data.cursor=-1;
        }
        return {};
    }
};

IlinkTimerPool::IlinkTimerPool(IopHost& host):state_(std::make_shared<State>(host)) {}
IlinkTimerPool::~IlinkTimerPool() { stop(); }
bool IlinkTimerPool::start(uint32_t image) {
    auto& s=*state_; std::lock_guard lock(s.mutex);
    if (s.busy || s.running || s.data.image) return false;
    State::Busy guard(s);
    struct Transaction { State& s; bool done=false; ~Transaction() { if (!done) s.dispose(); } } transaction{s};
    if (!image || (image&3) || image>UINT32_MAX-IlinkOriginalModelBytes || !verifyIlinkModule(s.host)) return false;
    std::vector<uint8_t> model(IlinkOriginalModelBytes);
    if (!s.host.readIopMemory(image,model.data(),model.size())) return false;
    if (!s.wordIs(image+0x16af0,Capacity) || !s.wordIs(image+0x16d5c,16) || !s.wordIs(image+0x16d60,64) ||
        !s.wordIs(image+0x16ba0,UINT32_MAX) || !s.wordIs(image+0x175a8,0) || !s.wordIs(image+0x172a0,0)) return false;
    if (!std::all_of(model.begin()+0x172c0,model.begin()+0x17541,[](uint8_t b) { return b==0; })) return false;
    s.data.image=image; s.data.event=s.host.allocateIopMemory(16);
    if (!s.data.event) return false;
    for (auto& address:s.data.contexts) {
        address=s.host.allocateIopMemory(ContextBytes); if (!address) return false;
    }
    const auto handle=eventHandles.fetch_add(1),generation=nextToken();
    if (handle>=0x6d000000 || !generation) return false;
    s.data.eventHandle=uint32_t(handle); s.data.generation=generation;
    std::array<uint8_t,16> event{}; put(event,4,s.data.eventHandle); put(event,12,image+0x11f68);
    std::array<uint8_t,ContextBytes> context{}; put(context,0,image+0x12f78);
    const auto control=s.control();
    std::array<IopMemoryWrite,ContextCount+3> writes{{{s.data.event,event},{image+0x172a0,bytesOf(s.data.event)},
                                                   {image+0x172c0,control}}};
    for (unsigned i=0;i<ContextCount;++i) writes[i+3]={s.data.contexts[i],context};
    if (!s.host.writeIopMemoryBatch(writes)) return false;
    s.running=true; if (!s.valid()) return false;
    transaction.done=true; return true;
}
bool IlinkTimerPool::stop() {
    auto& s=*state_; std::lock_guard lock(s.mutex); if (s.busy) return false;
    State::Busy guard(s); s.dispose(); return true;
}
bool IlinkTimerPool::active() const { return snapshot().has_value(); }
std::optional<IlinkTimerPool::Snapshot> IlinkTimerPool::snapshot() const {
    auto& s=*state_; std::lock_guard lock(s.mutex); if (s.busy) return {};
    State::Busy guard(s);
    if (!s.valid()) { s.invalidate(); return {}; }
    auto result=s.data; result.freeCount=uint32_t(std::count(s.owners.begin(),s.owners.end(),uint64_t{0}));
    return result;
}
std::optional<IlinkTimerPool::Lease> IlinkTimerPool::tryAcquire() {
    auto& s=*state_; std::lock_guard lock(s.mutex); return s.take();
}
std::optional<IlinkTimerPool::Lease> IlinkTimerPool::acquire(std::stop_token stop) {
    const auto state=state_; auto& s=*state; std::unique_lock lock(s.mutex);
    if (s.busy || !s.running || stop.stop_requested()) return {};
    const auto generation=s.data.generation;
    while (!stop.stop_requested() && s.running && s.data.generation==generation) {
        if (auto lease=s.take()) return lease;
        if (!s.running) return {};
        ++s.data.waiters;
        struct Waiting { State& s; ~Waiting() { --s.data.waiters; } } waiting{s};
        s.changed.wait(lock,stop,[&] { return !s.running || s.data.generation!=generation || s.data.cursor>=0; });
    }
    return {};
}
bool IlinkTimerPool::release(Lease lease) {
    auto& s=*state_; std::lock_guard lock(s.mutex); if (s.busy || !s.running) return false;
    State::Busy guard(s);
    if (!s.valid()) { s.invalidate(); return false; }
    if (lease.index>=Capacity || !lease.token || s.owners[lease.index]!=lease.token ||
        lease.context!=s.data.contexts[lease.index]) return false;
    const bool signal=s.data.cursor<0;
    const int32_t cursor=signal?int32_t(lease.index):s.data.cursor;
    if (!s.commitCursor(cursor,std::pair<unsigned,uint8_t>{lease.index,0})) return false;
    s.data.cursor=cursor; s.owners[lease.index]=0;
    if (signal) { ++s.data.signals; s.changed.notify_all(); }
    return true;
}
}
