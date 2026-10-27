#include "ps2x/iop/gt3/pdispu2_workers.h"
#include "ps2x/iop/gt3/pdispu2_sound_setup.h"
#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <future>
#include <mutex>

namespace ps2x::iop::gt3 {
namespace {
std::atomic<uint64_t> handles{0x69000000};
thread_local const void* currentPdispu2Worker=nullptr;
uint32_t handle() { const auto value=handles.fetch_add(1); return value<0x6a000000?uint32_t(value):0; }
void put(std::span<uint8_t> bytes,size_t offset,uint32_t value) {
    for (unsigned i=0;i<4;++i) bytes[offset+i]=uint8_t(value>>(i*8));
}
constexpr std::array<uint32_t,8> initialCounts{0,1,0,1,0,0,1,0};
constexpr std::array<uint32_t,7> semaOffsets{0x3820,0x3824,0x3840,0x3844,0x3850,0x4030,0x4034};
}
struct Pdispu2Workers::State {
    struct Semaphore { std::mutex mutex; std::condition_variable_any changed; unsigned count=0; };
    struct Queue {
        std::mutex mutex; std::condition_variable_any changed;
        std::optional<std::promise<std::optional<uint32_t>>> request;
        std::atomic<uint64_t> queries{0};
    };
    IopHost& host;
    mutable std::recursive_mutex mutex,lifecycle;
    std::array<Semaphore,8> semaphores;
    std::array<Queue,2> queues;
    std::array<std::jthread,4> workers;
    Snapshot resources;
    uint32_t image=0;
    bool running=false,busy=false;
    explicit State(IopHost& h):host(h) {}
    bool valid() const {
        std::array<uint32_t,3> identity{};
        if (!running || !image || !host.readIopMemory(image+0x3790,identity.data(),sizeof(identity)) ||
            identity!=std::array<uint32_t,3>{0x109,0x53505550,0x53505556}) return false;
        for (size_t i=0;i<semaOffsets.size();++i) {
            uint32_t value=0;
            if (!host.readIopMemory(image+semaOffsets[i],&value,4) || value!=resources.semaphores[i]) return false;
        }
        return true;
    }
    void stopThreads() {
        for (auto& worker:workers) worker.request_stop();
        for (auto& sema:semaphores) sema.changed.notify_all();
        for (auto& queue:queues) queue.changed.notify_all();
        for (auto& worker:workers) if (worker.joinable()) worker.join();
        for (auto& queue:queues) { queue.request.reset(); queue.queries=0; }
    }
    void release() {
        stopThreads();
        for (auto stack:resources.stacks) if (stack) host.freeIopMemory(stack);
        resources={}; image=0; running=false;
    }
};
Pdispu2Workers::Pdispu2Workers(IopHost& host):state_(std::make_shared<State>(host)) {}
Pdispu2Workers::~Pdispu2Workers() { stop(); }
bool Pdispu2Workers::start(uint32_t image) {
    auto& s=*state_; if (currentPdispu2Worker==&s) return false;
    std::lock_guard lifecycle(s.lifecycle);
    { std::lock_guard lock(s.mutex); if (s.busy || s.running) return false; s.busy=true; }
    struct Transaction {
        State& state; bool committed=false;
        ~Transaction() { if (!committed) state.release(); std::lock_guard lock(state.mutex); state.busy=false; }
    } transaction{s};
    if (!image || (image&3) || image>UINT32_MAX-ModelBytes || !verifyPdispu2Module(s.host)) return false;
    std::vector<uint8_t> bytes(ModelBytes);
    if (!s.host.readIopMemory(image,bytes.data(),bytes.size())) return false;
    s.image=image; s.running=true;
    std::array<uint32_t,3> identity{};
    if (!s.host.readIopMemory(image+0x3790,identity.data(),sizeof(identity)) ||
        identity!=std::array<uint32_t,3>{0x109,0x53505550,0x53505556} ||
        !std::all_of(bytes.begin()+0x3820,bytes.begin()+0x4040,[](uint8_t b) { return b==0; })) return false;
    for (size_t i=0;i<8;++i) {
        s.resources.semaphores[i]=handle(); if (!s.resources.semaphores[i]) return false;
        s.semaphores[i].count=initialCounts[i];
        if (i<semaOffsets.size()) put(bytes,semaOffsets[i],s.resources.semaphores[i]);
        const auto meta=0x4100+i*16;
        put(bytes,meta,s.resources.semaphores[i]); put(bytes,meta+4,initialCounts[i]); put(bytes,meta+8,1);
    }
    const std::array<uint32_t,4> entries{0xe24,0x295c,0x1b94,0x1b94},priorities{9,22,21,77};
    std::vector<uint8_t> stack(0x800);
    std::array<IopMemoryWrite,5> writes{}; writes[0]={image,bytes};
    for (size_t i=0;i<4;++i) {
        s.resources.stacks[i]=s.host.allocateIopMemory(0x800); s.resources.threads[i]=handle();
        if (!s.resources.stacks[i] || !s.resources.threads[i]) return false;
        writes[i+1]={s.resources.stacks[i],stack};
        const auto meta=0x4040+i*0x30;
        put(bytes,meta,0x02000000); put(bytes,meta+4,entries[i]); put(bytes,meta+8,s.resources.stacks[i]);
        put(bytes,meta+12,0x800); put(bytes,meta+16,priorities[i]); put(bytes,meta+20,s.resources.threads[i]);
        put(bytes,meta+24,i==3?1:0);
        if (i>=2) {
            const auto offset=uint32_t(i-2)*0x3b0;
            put(bytes,meta+28,i==2?0x53505550:0x53505556); put(bytes,meta+32,image+0x38d0+offset);
            put(bytes,meta+36,image+0x3914+offset); put(bytes,meta+40,image+0x3930+offset);
        }
    }
    // Create actual waiting workers before publishing any of their IOP handles.
    for (size_t i=0;i<4;++i) {
        std::promise<void> ready; auto initialized=ready.get_future();
        s.workers[i]=std::jthread([&s,i,ready=std::move(ready)](std::stop_token stop) mutable {
            currentPdispu2Worker=&s; s.resources.workerIds[i]=std::this_thread::get_id();
            if (i<2) {
                auto& sema=s.semaphores[i+4]; std::unique_lock lock(sema.mutex);
                ready.set_value(); sema.changed.wait(lock,stop,[&] { return sema.count!=0; });
            } else {
                auto& queue=s.queues[i-2]; std::unique_lock lock(queue.mutex); ready.set_value();
                while (queue.changed.wait(lock,stop,[&] { return queue.request.has_value(); })) {
                    auto promise=std::move(*queue.request); queue.request.reset(); lock.unlock();
                    try {
                        uint32_t version=0;
                        const bool read=s.host.readIopMemory(s.image+0x3790,&version,4) && version==0x109;
                        ++queue.queries; promise.set_value(read?std::optional{version}:std::nullopt);
                    } catch (...) { promise.set_exception(std::current_exception()); }
                    lock.lock();
                }
            }
            currentPdispu2Worker=nullptr;
        }); initialized.get();
    }
    if (!s.host.writeIopMemoryBatch(writes)) return false;
    std::vector<uint8_t> committed(bytes.size());
    if (!s.host.readIopMemory(image,committed.data(),committed.size()) || committed!=bytes || !s.valid()) return false;
    transaction.committed=true; return true;
}
bool Pdispu2Workers::stop() {
    auto& s=*state_; if (currentPdispu2Worker==&s) return false;
    { std::lock_guard lock(s.mutex); if (s.busy) return false; }
    std::lock_guard lifecycle(s.lifecycle);
    { std::lock_guard lock(s.mutex); if (s.busy) return false; s.busy=true; s.running=false; }
    s.release();
    std::lock_guard lock(s.mutex); s.busy=false; return true;
}
bool Pdispu2Workers::active() const { auto& s=*state_; std::lock_guard lock(s.mutex); return !s.busy && s.valid(); }
std::optional<Pdispu2Workers::Snapshot> Pdispu2Workers::snapshot() const {
    auto& s=*state_; std::lock_guard lock(s.mutex); if (s.busy || !s.valid()) return {};
    auto snapshot=s.resources;
    for (size_t i=0;i<2;++i) snapshot.versionQueries[i]=s.queues[i].queries.load();
    return snapshot;
}
std::optional<uint32_t> Pdispu2Workers::queryVersion(uint32_t sid) {
    auto& s=*state_; if (currentPdispu2Worker==&s || (sid!=0x53505550 && sid!=0x53505556)) return {};
    std::lock_guard lifecycle(s.lifecycle);
    { std::lock_guard lock(s.mutex); if (s.busy || !s.valid()) return {}; s.busy=true; }
    struct Finish { State& s; ~Finish() { std::lock_guard lock(s.mutex); s.busy=false; } } finish{s};
    std::promise<std::optional<uint32_t>> request; auto result=request.get_future(); auto& queue=s.queues[sid==0x53505556];
    { std::lock_guard lock(queue.mutex); queue.request=std::move(request); } queue.changed.notify_all();
    return result.get();
}
}
