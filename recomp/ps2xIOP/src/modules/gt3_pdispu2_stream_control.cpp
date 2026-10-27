#include "ps2x/iop/gt3/pdispu2_stream_control.h"
#include "ps2x/iop/gt3/pdispu2_sound_setup.h"
#include <atomic>
#include <algorithm>
#include <bit>
#include <chrono>
#include <condition_variable>
#include <mutex>

namespace ps2x::iop::gt3 {
namespace {
std::atomic<uint64_t> handles{0x71000000};
void put(std::span<uint8_t> bytes,size_t offset,uint32_t value) {
    for (unsigned i=0;i<4;++i) bytes[offset+i]=uint8_t(value>>(i*8));
}
uint32_t word(std::span<const uint8_t> bytes,size_t offset) {
    uint32_t value=0; for (unsigned i=0;i<4;++i) value|=uint32_t(bytes[offset+i])<<(i*8); return value;
}
}
struct Pdispu2StreamControl::State {
    IopHost& host;
    mutable std::recursive_mutex mutex;
    std::condition_variable_any changed;
    uint32_t descriptor=0,semaphore=0;
    unsigned count=0;
    bool active=false,busy=false,failed=false;
    explicit State(IopHost& h):host(h) {}
    bool valid() const {
        uint32_t actual=0;
        return active && !failed && host.readIopMemory(descriptor,&actual,4) && actual==semaphore;
    }
    bool commit(uint32_t offset,std::span<const uint8_t> bytes) {
        const std::array<IopMemoryWrite,1> writes{{{descriptor+offset,bytes}}};
        if (!host.writeIopMemoryBatch(writes)) return false;
        std::vector<uint8_t> observed(bytes.size());
        if (!host.readIopMemory(descriptor+offset,observed.data(),observed.size()) ||
            !std::ranges::equal(observed,bytes) || !valid()) {
            failed=true; changed.notify_all(); return false;
        }
        return true;
    }
};
Pdispu2StreamControl::Pdispu2StreamControl(IopHost& host):state_(std::make_unique<State>(host)) {}
Pdispu2StreamControl::~Pdispu2StreamControl() { stop(); }
bool Pdispu2StreamControl::initialize(uint32_t descriptor) {
    auto& s=*state_; std::lock_guard lock(s.mutex);
    if (s.active || s.busy || !descriptor || (descriptor&3) || descriptor>UINT32_MAX-0x38) return false;
    s.busy=true; struct Finish { bool& b; ~Finish() { b=false; } } finish{s.busy};
    std::array<uint8_t,0x38> bytes{};
    if (!verifyPdispu2Module(s.host) || !s.host.readIopMemory(descriptor,bytes.data(),bytes.size())) return false;
    const auto handle=handles.fetch_add(1); if (handle>=0x72000000) return false;
    // Original constructor writes through byte34 only; preserve three trailing
    // bytes that belong to its caller's existing object storage.
    std::fill(bytes.begin(),bytes.begin()+0x35,0);
    put(bytes,0,uint32_t(handle)); put(bytes,4,48000); put(bytes,8,0x7fffffff);
    for (size_t at=0x20;at<=0x2c;at+=4) put(bytes,at,0xffffffff);
    const std::array<IopMemoryWrite,1> writes{{{descriptor,bytes}}};
    if (!s.host.writeIopMemoryBatch(writes)) return false;
    std::array<uint8_t,0x38> observed{};
    if (!s.host.readIopMemory(descriptor,observed.data(),observed.size()) || observed!=bytes) return false;
    s.descriptor=descriptor; s.semaphore=uint32_t(handle); s.count=0; s.active=true; s.failed=false; return true;
}
bool Pdispu2StreamControl::setRange(const std::array<uint32_t,4>& words) {
    auto& s=*state_; std::lock_guard lock(s.mutex);
    if (s.busy || !s.valid()) return false;
    s.busy=true; struct Finish { bool& b; ~Finish() { b=false; } } finish{s.busy};
    std::array<uint8_t,16> bytes{}; for (size_t i=0;i<4;++i) put(bytes,i*4,words[i]);
    const std::array<IopMemoryWrite,1> writes{{{s.descriptor+0x20,bytes}}};
    if (!s.host.writeIopMemoryBatch(writes)) return false;
    std::array<uint8_t,16> observed{};
    if (!s.host.readIopMemory(s.descriptor+0x20,observed.data(),observed.size()) || observed!=bytes || !s.valid()) return false;
    // Original23c8 ignores an already-full SignalSema result. Preserve binary
    // coalescing; repeated updates replace the range without accumulating wakes.
    s.count=1; s.changed.notify_all(); return true;
}
bool Pdispu2StreamControl::wait(std::stop_token stop) {
    auto& s=*state_; std::unique_lock lock(s.mutex);
    if (s.busy || !s.valid()) return false;
    if (!s.changed.wait(lock,stop,[&] { return !s.active || s.failed || s.count; }) || !s.valid()) return false;
    --s.count; return true;
}
bool Pdispu2StreamControl::configure(uint32_t step,uint32_t rate) {
    auto& s=*state_; std::lock_guard lock(s.mutex);
    if (s.busy || !s.valid() || !rate || rate>INT32_MAX) return false;
    s.busy=true; struct Finish { bool& b; ~Finish() { b=false; } } finish{s.busy};
    std::array<uint8_t,12> bytes{};
    if (!s.host.readIopMemory(s.descriptor+4,bytes.data(),bytes.size())) return false;
    put(bytes,0,rate); put(bytes,8,step); return s.commit(4,bytes);
}
bool Pdispu2StreamControl::waitForRange(std::stop_token stop) {
    auto& s=*state_; std::unique_lock lock(s.mutex);
    if (s.busy || !s.valid()) return false;
    for (;;) {
        if (!s.changed.wait(lock,stop,[&] { return !s.active || s.failed || s.count; }) || !s.valid()) return false;
        --s.count;
        std::array<uint8_t,24> bytes{};
        if (!s.host.readIopMemory(s.descriptor+0x18,bytes.data(),bytes.size())) {
            s.failed=true; s.changed.notify_all(); return false;
        }
        const auto time=std::bit_cast<int64_t>(uint64_t(word(bytes,0))|(uint64_t(word(bytes,4))<<32));
        const auto end=std::bit_cast<int64_t>(uint64_t(word(bytes,16))|(uint64_t(word(bytes,20))<<32));
        if (time>=end) continue;
        const auto delay=std::bit_cast<int32_t>(word(bytes,8)-word(bytes,0));
        if (delay>0) {
            // Original DelayThread is not shortened by another range signal.
            // Native stop/failure still interrupts the delay for safe teardown.
            s.changed.wait_for(lock,stop,std::chrono::microseconds(delay),[&] { return !s.active || s.failed; });
        }
        return !stop.stop_requested() && s.valid();
    }
}
bool Pdispu2StreamControl::setPeriod(uint32_t samples) {
    auto& s=*state_; std::lock_guard lock(s.mutex);
    if (s.busy || !s.valid() || samples>INT32_MAX) return false;
    s.busy=true; struct Finish { bool& b; ~Finish() { b=false; } } finish{s.busy};
    std::array<uint8_t,4> bytes{}; put(bytes,0,samples); return s.commit(8,bytes);
}
std::optional<bool> Pdispu2StreamControl::advance() {
    auto& s=*state_; std::lock_guard lock(s.mutex);
    if (s.busy || !s.valid()) return {};
    s.busy=true; struct Finish { bool& b; ~Finish() { b=false; } } finish{s.busy};
    std::array<uint8_t,0x38> bytes{};
    if (!s.host.readIopMemory(s.descriptor,bytes.data(),bytes.size())) return {};
    const auto rate=word(bytes,4),period=word(bytes,8);
    if (!rate || rate>INT32_MAX || period>INT32_MAX) return {};
    uint32_t position=word(bytes,16)+word(bytes,12),cycles=word(bytes,20);
    if (std::bit_cast<int32_t>(position)>int32_t(period)) { position-=period; ++cycles; }
    const int64_t time=int64_t(std::bit_cast<int32_t>(position))*1000000/int64_t(rate);
    const auto end=std::bit_cast<int64_t>(uint64_t(word(bytes,40))|(uint64_t(word(bytes,44))<<32));
    std::array<uint8_t,16> next{}; put(next,0,position); put(next,4,cycles);
    put(next,8,uint32_t(time)); put(next,12,uint32_t(uint64_t(time)>>32));
    if (!s.commit(16,next)) return {};
    return time>end;
}
void Pdispu2StreamControl::stop() {
    auto& s=*state_; std::lock_guard lock(s.mutex);
    if (s.busy) return; // Reentrant host instrumentation cannot tear down a commit.
    s.active=false; s.count=0; s.changed.notify_all();
}
std::optional<Pdispu2StreamControl::Snapshot> Pdispu2StreamControl::snapshot() const {
    auto& s=*state_; std::lock_guard lock(s.mutex);
    if (s.busy || !s.valid()) return {};
    Snapshot result{s.semaphore,s.count,{}};
    if (!s.host.readIopMemory(s.descriptor+0x20,result.range.data(),sizeof(result.range))) return {};
    return result;
}
}
