#include "ps2x/iop/gt3/pdistr_mpeg_ring.h"
#include "ps2x/iop/gt3/pdistr_startup_image.h"
#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <mutex>

namespace ps2x::iop::gt3 {
namespace {
std::atomic<uint64_t> handles{0x72000000};
void put(std::span<uint8_t> bytes,size_t offset,uint32_t value) {
    for (unsigned i=0;i<4;++i) bytes[offset+i]=uint8_t(value>>(i*8));
}
uint32_t word(std::span<const uint8_t> bytes,size_t offset) {
    uint32_t value=0; for (unsigned i=0;i<4;++i) value|=uint32_t(bytes[offset+i])<<(i*8); return value;
}
bool eeSpan(uint32_t raw,uint32_t size,uint32_t& physical) {
    for (uint32_t base:{0u,0x20000000u,0x80000000u,0xa0000000u})
        if (raw>=base && uint64_t(raw)-base<0x2000000) {
            physical=raw-base; return physical && !(physical&15) && size<=0x2000000-physical;
        }
    return false;
}
}
struct PdistrMpegRing::State {
    IopHost& host;
    mutable std::recursive_mutex mutex;
    std::condition_variable_any changed;
    uint32_t descriptor=0,buffer=0,size=0;
    std::array<uint8_t,0x84> bytes{};
    bool active=false,busy=false,failed=false;
    explicit State(IopHost& h):host(h) {}
    bool valid() const {
        std::array<uint8_t,0x84> observed{}; uint8_t last=0;
        return active && !failed && host.readIopMemory(descriptor,observed.data(),observed.size()) && observed==bytes &&
            host.readIopMemory(buffer+size-1,&last,1);
    }
    bool committed(const std::array<uint8_t,0x84>& next) {
        std::array<uint8_t,0x84> observed{};
        if (!host.readIopMemory(descriptor,observed.data(),observed.size()) || observed!=next) {
            failed=true; changed.notify_all(); return false;
        }
        bytes=next; changed.notify_all(); return true;
    }
    bool read(uint32_t count,std::vector<uint8_t>& data) const {
        data.resize(count); if (!count) return true;
        const auto cursor=word(bytes,0x1c),first=std::min(count,size-cursor);
        return host.readIopMemory(buffer+cursor,data.data(),first) &&
            (first==count || host.readIopMemory(buffer,data.data()+first,count-first));
    }
    std::array<uint8_t,0x84> consumed(uint32_t count) const {
        auto next=bytes; put(next,0x14,word(bytes,0x14)-count); put(next,0x1c,(word(bytes,0x1c)+count)%size); return next;
    }
};
PdistrMpegRing::PdistrMpegRing(IopHost& host):state_(std::make_unique<State>(host)) {}
PdistrMpegRing::~PdistrMpegRing() { stop(); }
bool PdistrMpegRing::initialize(uint32_t descriptor,uint32_t buffer,uint32_t capacity) {
    auto& s=*state_; std::lock_guard lock(s.mutex);
    if (s.active || s.busy || !descriptor || (descriptor&3) || !buffer || !capacity || capacity>0x40000 ||
        descriptor>UINT32_MAX-0x84 || buffer>UINT32_MAX-capacity ||
        (uint64_t(descriptor)<uint64_t(buffer)+capacity && uint64_t(buffer)<uint64_t(descriptor)+0x84)) return false;
    s.busy=true; struct Finish { bool& b; ~Finish() { b=false; } } finish{s.busy};
    std::array<uint8_t,0x84> old{}; std::vector<uint8_t> data(capacity);
    if (!verifyPdistrModule(s.host) || !s.host.readIopMemory(descriptor,old.data(),old.size()) ||
        !s.host.readIopMemory(buffer,data.data(),data.size())) return false;
    const auto semaphore=handles.fetch_add(1); if (semaphore>=0x73000000) return false;
    std::array<uint8_t,0x84> initial{}; put(initial,0,uint32_t(semaphore)); put(initial,0xc,buffer); put(initial,0x10,capacity);
    const std::array<IopMemoryWrite,1> writes{{{descriptor,initial}}};
    if (!s.host.writeIopMemoryBatch(writes)) return false;
    std::array<uint8_t,0x84> observed{};
    if (!s.host.readIopMemory(descriptor,observed.data(),observed.size()) || observed!=initial) return false;
    s.descriptor=descriptor; s.buffer=buffer; s.size=capacity; s.bytes=initial; s.active=true; s.failed=false; return true;
}
bool PdistrMpegRing::produce(std::span<const uint8_t> prefix,uint32_t advance,std::stop_token stop) {
    auto& s=*state_; std::unique_lock lock(s.mutex);
    if (s.busy || !s.valid() || s.bytes[0x23] || prefix.size()>advance || advance>s.size) return false;
    if (!s.changed.wait(lock,stop,[&] { return !s.active || s.failed || s.bytes[0x23] || s.size-word(s.bytes,0x14)>=advance; }) ||
        !s.valid() || s.bytes[0x23]) return false;
    if (!advance) return true;
    s.busy=true; struct Finish { bool& b; ~Finish() { b=false; } } finish{s.busy};
    const auto cursor=word(s.bytes,0x18),first=std::min<uint32_t>(uint32_t(prefix.size()),s.size-cursor);
    auto next=s.bytes; put(next,0x14,word(s.bytes,0x14)+advance); put(next,0x18,(cursor+advance)%s.size);
    std::vector<IopMemoryWrite> writes{{s.descriptor,next}};
    if (first) writes.push_back({s.buffer+cursor,prefix.first(first)});
    if (first<prefix.size()) writes.push_back({s.buffer,prefix.subspan(first)});
    if (!s.host.writeIopMemoryBatch(writes)) return false;
    std::vector<uint8_t> observed(prefix.size());
    if ((first && !s.host.readIopMemory(s.buffer+cursor,observed.data(),first)) ||
        (first<prefix.size() && !s.host.readIopMemory(s.buffer,observed.data()+first,prefix.size()-first)) ||
        !std::ranges::equal(observed,prefix)) { s.failed=true; s.changed.notify_all(); return false; }
    return s.committed(next);
}
std::optional<uint32_t> PdistrMpegRing::waitWritable(uint32_t minimum,std::stop_token stop) {
    auto& s=*state_; std::unique_lock lock(s.mutex);
    if (s.busy || !minimum || !s.valid() || minimum>s.size-word(s.bytes,0x18)) return {};
    auto space=[&] { return std::min(s.size-word(s.bytes,0x14),s.size-word(s.bytes,0x18)); };
    if (!s.changed.wait(lock,stop,[&] { return !s.active || s.failed || s.bytes[0x23] || space()>=minimum; }) ||
        !s.valid() || s.bytes[0x23]) return {};
    return space();
}
std::optional<std::vector<uint8_t>> PdistrMpegRing::readAvailable(uint32_t maximum,std::stop_token stop) {
    auto& s=*state_; std::unique_lock lock(s.mutex);
    if (s.busy || !maximum || !s.valid()) return {};
    if (!s.changed.wait(lock,stop,[&] { return !s.active || s.failed || s.bytes[0x23] || word(s.bytes,0x14); }) || !s.valid()) return {};
    const auto count=std::min(maximum,word(s.bytes,0x14));
    std::vector<uint8_t> result;
    if (!s.read(count,result)) return {};
    if (!count) return result;
    s.busy=true; struct Finish { bool& b; ~Finish() { b=false; } } finish{s.busy};
    const auto next=s.consumed(count); const std::array<IopMemoryWrite,1> writes{{{s.descriptor,next}}};
    if (!s.host.writeIopMemoryBatch(writes) || !s.committed(next)) return {};
    return result;
}
bool PdistrMpegRing::consumeToEe(uint32_t destination,uint32_t count,std::stop_token stop) {
    auto& s=*state_; std::unique_lock lock(s.mutex);
    uint32_t physical=0;
    if (s.busy || !count || (count&15) || count>s.size || count>4096 ||
        count>s.size-word(s.bytes,0x1c) || !eeSpan(destination,count,physical) || !s.valid()) return false;
    if (!s.changed.wait(lock,stop,[&] { return !s.active || s.failed || s.bytes[0x23] || word(s.bytes,0x14)>=count; }) ||
        !s.valid() || word(s.bytes,0x14)<count) return false;
    s.busy=true; struct Finish { bool& b; ~Finish() { b=false; } } finish{s.busy};
    std::vector<uint8_t> data; if (!s.read(count,data)) return false;
    const auto next=s.consumed(count);
    const std::array<IopMemoryWrite,2> writes{{{s.descriptor,next},{physical,data}}};
    if (!s.host.commitIopAndEeMemory(writes)) return false;
    std::vector<uint8_t> delivered(count);
    if (!s.host.readGuest(physical,delivered.data(),delivered.size()) || delivered!=data) {
        s.failed=true; s.changed.notify_all(); return false;
    }
    return s.committed(next);
}
PdistrMpegRing::Transfer PdistrMpegRing::consumeRecordsToEe(uint32_t destination,uint32_t bytes,std::stop_token stop) {
    auto& s=*state_;
    uint32_t physical=0;
    {
        std::lock_guard lock(s.mutex);
        if (s.busy || bytes>0x8000 || (bytes%4096) || !eeSpan(destination,bytes,physical) ||
            !s.valid() || (s.size%4096) || (word(s.bytes,0x1c)%4096) || (word(s.bytes,0x14)%4096)) return {};
    }
    Transfer transfer{TransferResult::Complete,0};
    while (transfer.bytes<bytes) {
        if (stop.stop_requested()) { transfer.result=TransferResult::Stopped; return transfer; }
        if (!consumeToEe(physical+transfer.bytes,4096,stop)) {
            std::lock_guard lock(s.mutex);
            if (stop.stop_requested() || !s.active) transfer.result=TransferResult::Stopped;
            else if (s.valid() && s.bytes[0x23] && !word(s.bytes,0x14)) transfer.result=TransferResult::End;
            else transfer.result=TransferResult::Failed;
            return transfer;
        }
        transfer.bytes+=4096;
    }
    return transfer;
}
bool PdistrMpegRing::finish() {
    auto& s=*state_; std::lock_guard lock(s.mutex);
    if (s.busy || !s.valid()) return false;
    s.busy=true; struct Finish { bool& b; ~Finish() { b=false; } } finish{s.busy};
    auto next=s.bytes; next[0x23]=1;
    const std::array<IopMemoryWrite,1> writes{{{s.descriptor,next}}};
    return s.host.writeIopMemoryBatch(writes) && s.committed(next);
}
bool PdistrMpegRing::cancel() {
    auto& s=*state_; std::lock_guard lock(s.mutex);
    if (s.busy) return false;
    struct Retire { State& s; ~Retire() { s.active=false; s.changed.notify_all(); } } retire{s};
    bool result=false;
    if (s.valid()) {
        s.busy=true; struct Finish { bool& b; ~Finish() { b=false; } } finish{s.busy};
        auto next=s.bytes; next[0x22]=1;
        const std::array<IopMemoryWrite,1> writes{{{s.descriptor,next}}};
        result=s.host.writeIopMemoryBatch(writes) && s.committed(next);
    }
    return result;
}
void PdistrMpegRing::stop() {
    auto& s=*state_; std::lock_guard lock(s.mutex);
    if (s.busy) return;
    s.active=false; s.changed.notify_all();
}
std::optional<PdistrMpegRing::Snapshot> PdistrMpegRing::snapshot() const {
    auto& s=*state_; std::lock_guard lock(s.mutex); if (s.busy || !s.valid()) return {};
    return Snapshot{s.size,word(s.bytes,0x14),word(s.bytes,0x18),word(s.bytes,0x1c),s.bytes[0x23]!=0};
}
}
