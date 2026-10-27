#include "ps2x/iop/gt3/pdistr_workers.h"
#include "ps2x/iop/gt3/pdistr_startup_image.h"
#include "ps2x/iop/gt3/pdispu2_resident.h"
#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <future>
#include <mutex>
#include <vector>

namespace ps2x::iop::gt3 {
namespace {
std::atomic<uint64_t> handles{0x6a000000};
thread_local const void* currentPdistrWorker=nullptr;
uint32_t handle() { const auto h=handles.fetch_add(1); return h<0x6b000000?uint32_t(h):0; }
void put(std::span<uint8_t> bytes,size_t offset,uint32_t value) {
    for (unsigned i=0;i<4;++i) bytes[offset+i]=uint8_t(value>>(i*8));
}
uint32_t word(std::span<const uint8_t> bytes,size_t offset=0) {
    uint32_t value=0; for (unsigned i=0;i<4;++i) value|=uint32_t(bytes[offset+i])<<(i*8); return value;
}
constexpr std::array<uint32_t,4> counts{0,1,0,1},semaOffsets{0x3080,0x3084,0x2af0,0x2af4};
constexpr std::array<uint32_t,4> sids{0x5042474d,0x4d504731,0x4d504732,0x50444950};
constexpr std::array<uint32_t,4> servers{0x2a10,0x2ec0,0x2f04,0x3460};
constexpr std::array<uint32_t,4> queues{0x2a54,0x2f48,0x2f60,0x34a4};
constexpr std::array<uint32_t,4> buffers{0x2a70,0x2f80,0x3000,0x34c0};
}
struct PdistrWorkers::State {
    struct Semaphore { std::mutex mutex; std::condition_variable_any changed; unsigned count=0; };
    struct Queue {
        std::mutex mutex; std::condition_variable_any changed;
        std::optional<std::promise<std::optional<Reply>>> request;
        std::atomic<uint64_t> queries{0};
        uint32_t function=0;
        uint32_t value=0;
        Pdispu2Resident* sound=nullptr;
        std::array<uint8_t,128> parameters{};
    };
    BgmHandler bgmHandler; // Set while idle; read by the command worker.
    struct BgmRequest { uint8_t command; Pdispu2Resident* sound; std::promise<bool> completion; };
    IopHost& host;
    mutable std::recursive_mutex mutex,lifecycle;
    std::array<Semaphore,4> semaphores;
    std::array<Queue,4> rpcQueues;
    std::array<std::jthread,5> workers;
    std::optional<BgmRequest> bgmRequest; // Guarded by semaphore2.mutex.
    Snapshot resources;
    uint32_t image=0;
    bool running=false,busy=false;
    explicit State(IopHost& h):host(h) {}
    bool cancelIdle() {
        auto& sema=semaphores[1]; std::lock_guard lock(sema.mutex);
        if (sema.count!=1) return false;
        sema.count=0;
        struct Signal { Semaphore& sema; ~Signal() { sema.count=1; sema.changed.notify_all(); } } signal{sema};
        uint8_t active=0xff;
        return host.readIopMemory(image+0x3088,&active,1) && active==0;
    }
    // BGM command c has the fixed queue node 2e60+c*12 {prev, next, linked,
    // pad, command}; head/tail at 2e58/2e5c. Supported: 0 volume (RPC3),
    // 2 pause (RPC4), 3 resume (RPC5) and 4 stop (RPC2) while idle. Start (1)
    // needs native BGM playback and stays unsupported.
    bool enqueueBgm(uint8_t command,uint32_t value,Pdispu2Resident* sound,const std::array<uint8_t,128>* parameters=nullptr) {
        if ((command==0 && !sound) || (command==1 && (!parameters || !bgmHandler)) || command>4) return false;
        const size_t node=8+command*12;
        auto& sema=semaphores[2];
        std::future<bool> complete;
        {
            std::lock_guard lock(sema.mutex);
            std::array<uint8_t,68> queue{};
            std::array<uint8_t,12> reference{};
            if (sema.count || bgmRequest || !host.readIopMemory(image+0x2e58,queue.data(),queue.size()) ||
                word(queue)!=0 || word(queue,4)!=0 || word(queue,node)!=0 || word(queue,node+4)!=0 ||
                !host.readIopMemory(image+0x2c14,reference.data(),reference.size()) ||
                word(reference)!=image+0x2c00) return false;
            // Reference +4/+8 hold a running streamed-audio job's 424 callback
            // (BGM or movie); PDISPU2's setVolume validates that callback.
            for (size_t i=0;i<5;++i) if (queue[16+i*12]) return false;
            // Original a2c stores the RPC3 volume word, then 7d4 links the
            // command's node at the tail and signals 2af0, under interrupt
            // exclusion. We serialize RPCs; coalescing is unsupported.
            put(queue,0,image+0x2e58+node); put(queue,4,image+0x2e58+node);
            queue[node+8]=1; queue[node+9]=0; queue[node+10]=command;
            std::array<uint8_t,4> bytes{}; put(bytes,0,value);
            std::vector<IopMemoryWrite> writes{{image+0x2e58,queue}};
            if (command==0) writes.push_back({image+0x2b7c,bytes});
            // a70 copies all 128 request bytes to 2afc before 7d4(1).
            if (command==1) writes.push_back({image+0x2afc,*parameters});
            if (!host.writeIopMemoryBatch(writes)) return false;
            std::array<uint8_t,68> observed{};
            std::array<uint8_t,128> copied{};
            uint32_t saved=0;
            if (!host.readIopMemory(image+0x2e58,observed.data(),observed.size()) || observed!=queue ||
                (command==0 && (!host.readIopMemory(image+0x2b7c,&saved,4) || saved!=value)) ||
                (command==1 && (!host.readIopMemory(image+0x2afc,copied.data(),copied.size()) || copied!=*parameters))) return false;
            BgmRequest request{command,sound,{}}; complete=request.completion.get_future();
            bgmRequest=std::move(request); sema.count=1;
        }
        sema.changed.notify_all();
        return complete.get();
    }
    bool processBgm(uint8_t command,Pdispu2Resident* sound) {
        const size_t node=8+command*12;
        std::array<uint8_t,128> parameters{};
        std::array<uint8_t,68> queue{};
        uint32_t value=0;
        if (!host.readIopMemory(image+0x2afc,parameters.data(),parameters.size()) ||
            !host.readIopMemory(image+0x2b7c,&value,4) ||
            !host.readIopMemory(image+0x2e58,queue.data(),queue.size()) ||
            word(queue)!=image+0x2e58+node || word(queue,4)!=image+0x2e58+node ||
            word(queue,node)!=0 || word(queue,node+4)!=0 || queue[node+8]!=1 || queue[node+9]!=0 ||
            queue[node+10]!=command) return false;
        // Original 8f4 snapshots parameters on each queue pass; 878 unlinks the
        // node (prev, next, linked byte only; pad and command byte are kept).
        std::fill(queue.begin(),queue.begin()+8,0);
        std::fill(queue.begin()+node,queue.begin()+node+9,0);
        const std::array<IopMemoryWrite,2> writes{{{image+0x2b80,parameters},{image+0x2e58,queue}}};
        if (!host.writeIopMemoryBatch(writes)) return false;
        std::array<uint8_t,128> copied{};
        std::array<uint8_t,68> unlinked{};
        if (!host.readIopMemory(image+0x2b80,copied.data(),copied.size()) || copied!=parameters ||
            !host.readIopMemory(image+0x2e58,unlinked.data(),unlinked.size()) || unlinked!=queue) return false;
        auto& control=semaphores[3]; std::lock_guard lock(control.mutex);
        if (control.count!=1) return false;
        control.count=0;
        struct Signal { Semaphore& s; ~Signal() { s.count=1; s.changed.notify_all(); } } signal{control};
        if (command==0) return sound && sound->setVolume(image+0x2c14,value);
        if (command==1) {
            // 9d8: 4e8(2b80..2b8c, name 2b90), then the playing byte 2e75 = 1.
            if (!bgmHandler || !bgmHandler(1,copied)) return false;
            const std::array<uint8_t,1> playing{1};
            const std::array<IopMemoryWrite,1> mark{{{image+0x2e75,playing}}};
            uint8_t observed=0;
            return host.writeIopMemoryBatch(mark) && host.readIopMemory(image+0x2e75,&observed,1) && observed==1;
        }
        if (command==4 && bgmHandler) return bgmHandler(4,copied);
        if (command==4) {
            // 6b4 -> 49c: only when the BGM active byte 2af8 is set does it
            // clear it and tear down (25ec, export48, 259c). Idle stop is a
            // no-op; an active one would need native BGM playback.
            uint8_t playing=0xff;
            return host.readIopMemory(image+0x2af8,&playing,1) && playing==0;
        }
        // 74c/790 call PDISPU2 export47/48 on the BGM timing object 2d90.
        // export47 (25f0): +34 = 1. export48 (2604): if +34 is set, take the
        // waiting thread at +30, clear both and wake it. Only native BGM
        // playback could leave a waiter, so a recorded waiter is refused.
        std::array<uint8_t,8> timing{};
        if (!host.readIopMemory(image+0x2dc0,timing.data(),timing.size())) return false;
        if (command==2) timing[4]=1;
        else if (timing[4]) {
            if (word(timing)) return false;
            timing[4]=0;
        } else return true;
        const std::array<IopMemoryWrite,1> update{{{image+0x2dc0,timing}}};
        std::array<uint8_t,8> written{};
        return host.writeIopMemoryBatch(update) && host.readIopMemory(image+0x2dc0,written.data(),written.size()) &&
            written==timing;
    }
    bool valid() const {
        uint32_t version=0,pointer=0;
        if (!running || !image || !host.readIopMemory(image+0x29f0,&version,4) || version!=0x106 ||
            !host.readIopMemory(image+0x2c14,&pointer,4) || pointer!=image+0x2c00) return false;
        for (size_t i=0;i<4;++i) {
            uint32_t value=0;
            if (!host.readIopMemory(image+semaOffsets[i],&value,4) || value!=resources.semaphores[i]) return false;
        }
        return true;
    }
    void release() {
        for (auto& worker:workers) worker.request_stop();
        for (auto& sema:semaphores) sema.changed.notify_all();
        for (auto& queue:rpcQueues) queue.changed.notify_all();
        for (auto& worker:workers) if (worker.joinable()) worker.join();
        for (auto& queue:rpcQueues) { queue.request.reset(); queue.queries=0; }
        bgmRequest.reset();
        for (auto stack:resources.stacks) if (stack) host.freeIopMemory(stack);
        resources={}; image=0; running=false;
    }
};
PdistrWorkers::PdistrWorkers(IopHost& host):state_(std::make_shared<State>(host)) {}
PdistrWorkers::~PdistrWorkers() { stop(); }
bool PdistrWorkers::start(uint32_t image) {
    auto& s=*state_; if (currentPdistrWorker==&s) return false;
    std::lock_guard lifecycle(s.lifecycle);
    { std::lock_guard lock(s.mutex); if (s.busy || s.running) return false; s.busy=true; }
    struct Transaction {
        State& s; bool committed=false;
        ~Transaction() { if (!committed) s.release(); std::lock_guard lock(s.mutex); s.busy=false; }
    } transaction{s};
    if (!image || (image&3) || image>UINT32_MAX-ModelBytes || !verifyPdistrModule(s.host)) return false;
    std::vector<uint8_t> bytes(ModelBytes);
    if (!s.host.readIopMemory(image,bytes.data(),bytes.size())) return false;
    auto word=[&](size_t at) { uint32_t v=0; for (unsigned i=0;i<4;++i) v|=uint32_t(bytes[at+i])<<(i*8); return v; };
    if (word(0x29f0)!=0x106 || word(0x2a00)!=image+0x29d0 || word(0x2a08)!=0 || word(0x2c14)!=image+0x2c00) return false;
    for (size_t at=0x2a10;at<0x3580;++at)
        if ((at<0x2c00 || at>=0x2c18) && bytes[at]!=0) return false;
    for (size_t at=0x2c00;at<0x2c10;at+=4) if (word(at)!=0x8000) return false;
    if (word(0x2c10)!=0) return false;
    s.image=image; s.running=true;
    for (size_t i=0;i<4;++i) {
        s.resources.semaphores[i]=handle(); if (!s.resources.semaphores[i]) return false;
        s.semaphores[i].count=counts[i]; put(bytes,semaOffsets[i],s.resources.semaphores[i]);
        const auto meta=0x3700+i*16;
        put(bytes,meta,s.resources.semaphores[i]); put(bytes,meta+4,counts[i]); put(bytes,meta+8,1);
    }
    const std::array<uint32_t,5> entries{0xc04,0x1378,0x1378,0x23bc,0x8f4},priorities{24,76,24,95,75};
    const std::array<uint32_t,5> args{0,0,1,0,image+0x2a10};
    std::vector<uint8_t> stack(0x800);
    std::array<IopMemoryWrite,6> writes{}; writes[0]={image,bytes};
    for (size_t i=0;i<5;++i) {
        s.resources.stacks[i]=s.host.allocateIopMemory(0x800); s.resources.threads[i]=handle();
        if (!s.resources.stacks[i] || !s.resources.threads[i]) return false;
        writes[i+1]={s.resources.stacks[i],stack};
        const auto meta=0x3580+i*0x30;
        put(bytes,meta,0x02000000); put(bytes,meta+4,entries[i]); put(bytes,meta+8,s.resources.stacks[i]);
        put(bytes,meta+12,0x800); put(bytes,meta+16,priorities[i]); put(bytes,meta+20,s.resources.threads[i]);
        put(bytes,meta+24,args[i]);
        if (i<4) {
            put(bytes,meta+28,sids[i]); put(bytes,meta+32,image+servers[i]);
            put(bytes,meta+36,image+queues[i]); put(bytes,meta+40,image+buffers[i]);
        }
    }
    // Workers wait on actual queues/resources before any handles are published.
    for (size_t i=0;i<5;++i) {
        std::promise<void> ready; auto initialized=ready.get_future();
        s.workers[i]=std::jthread([&s,i,ready=std::move(ready)](std::stop_token stop) mutable {
            currentPdistrWorker=&s; s.resources.workerIds[i]=std::this_thread::get_id();
            if (i==4) {
                auto& sema=s.semaphores[2]; std::unique_lock lock(sema.mutex);
                ready.set_value();
                while (sema.changed.wait(lock,stop,[&] { return sema.count!=0; })) {
                    sema.count=0;
                    auto request=std::move(*s.bgmRequest); s.bgmRequest.reset(); lock.unlock();
                    try { request.completion.set_value(s.processBgm(request.command,request.sound)); }
                    catch (...) { request.completion.set_exception(std::current_exception()); }
                    lock.lock();
                }
            } else {
                auto& queue=s.rpcQueues[i]; std::unique_lock lock(queue.mutex); ready.set_value();
                while (queue.changed.wait(lock,stop,[&] { return queue.request.has_value(); })) {
                    auto promise=std::move(*queue.request); const auto function=queue.function,value=queue.value;
                    auto* sound=queue.sound; const auto parameters=queue.parameters; queue.request.reset(); lock.unlock();
                    try {
                        uint32_t version=0;
                        const bool valid=s.host.readIopMemory(s.image+0x29f0,&version,4) && version==0x106 &&
                            (function==0 || ((function==3 || function==4) && (i==1 || i==2) && s.cancelIdle()) ||
                             (i==0 && function==3 && s.enqueueBgm(0,value,sound)) ||
                             (i==0 && (function==4 || function==5) && s.enqueueBgm(uint8_t(function-2),0,nullptr)) ||
                             (i==0 && function==2 && s.enqueueBgm(4,0,nullptr)) ||
                             (i==0 && function==1 && s.enqueueBgm(1,0,nullptr,&parameters)));
                        if (!function) ++queue.queries;
                        promise.set_value(valid?std::optional<Reply>{{i==3,i==3?version:0}}:std::nullopt);
                    } catch (...) { promise.set_exception(std::current_exception()); }
                    lock.lock();
                }
            }
            currentPdistrWorker=nullptr;
        }); initialized.get();
    }
    if (!s.host.writeIopMemoryBatch(writes)) return false;
    std::vector<uint8_t> committed(bytes.size());
    if (!s.host.readIopMemory(image,committed.data(),committed.size()) || committed!=bytes || !s.valid()) return false;
    transaction.committed=true; return true;
}
bool PdistrWorkers::stop() {
    auto& s=*state_; if (currentPdistrWorker==&s) return false;
    { std::lock_guard lock(s.mutex); if (s.busy) return false; }
    std::lock_guard lifecycle(s.lifecycle);
    { std::lock_guard lock(s.mutex); if (s.busy) return false; s.busy=true; s.running=false; }
    s.release(); std::lock_guard lock(s.mutex); s.busy=false; return true;
}
bool PdistrWorkers::active() const { auto& s=*state_; std::lock_guard lock(s.mutex); return !s.busy && s.valid(); }
std::optional<PdistrWorkers::Snapshot> PdistrWorkers::snapshot() const {
    auto& s=*state_; std::lock_guard lock(s.mutex); if (s.busy || !s.valid()) return {};
    auto result=s.resources; for (size_t i=0;i<4;++i) result.initialQueries[i]=s.rpcQueues[i].queries.load(); return result;
}
std::optional<PdistrWorkers::Reply> PdistrWorkers::queryInitial(uint32_t sid) {
    return query(sid,0);
}
std::optional<PdistrWorkers::Reply> PdistrWorkers::queryIdleCancel(uint32_t sid) {
    if (sid!=0x4d504731 && sid!=0x4d504732) return {};
    return query(sid,4);
}
std::optional<PdistrWorkers::Reply> PdistrWorkers::queryIdleMpegControl(uint32_t sid) {
    if (sid!=0x4d504731 && sid!=0x4d504732) return {};
    return query(sid,3);
}
std::optional<PdistrWorkers::Reply> PdistrWorkers::queryBgmVolume(uint32_t value,Pdispu2Resident& sound) {
    return query(0x5042474d,3,value,&sound);
}
std::optional<PdistrWorkers::Reply> PdistrWorkers::queryBgmPause(bool pause) {
    return query(0x5042474d,pause?4:5);
}
std::optional<PdistrWorkers::Reply> PdistrWorkers::queryBgmStop() {
    return query(0x5042474d,2);
}
void PdistrWorkers::setBgmHandler(BgmHandler handler) {
    auto& s=*state_; std::lock_guard lifecycle(s.lifecycle); s.bgmHandler=std::move(handler);
}
std::optional<PdistrWorkers::Reply> PdistrWorkers::queryBgmStart(const std::array<uint8_t,128>& parameters) {
    auto& s=*state_;
    { std::lock_guard lock(s.rpcQueues[0].mutex); s.rpcQueues[0].parameters=parameters; }
    return query(0x5042474d,1);
}
bool PdistrWorkers::cancelIdle() {
    auto& s=*state_; if (currentPdistrWorker==&s) return false;
    std::lock_guard lifecycle(s.lifecycle);
    { std::lock_guard lock(s.mutex); if (s.busy || !s.valid()) return false; s.busy=true; }
    struct Finish { State& s; ~Finish() { std::lock_guard lock(s.mutex); s.busy=false; } } finish{s};
    return s.cancelIdle();
}
std::optional<PdistrWorkers::Reply> PdistrWorkers::query(uint32_t sid,uint32_t function,uint32_t value,Pdispu2Resident* sound) {
    auto& s=*state_; if (currentPdistrWorker==&s) return {};
    const auto found=std::find(sids.begin(),sids.end(),sid); if (found==sids.end()) return {};
    std::lock_guard lifecycle(s.lifecycle);
    { std::lock_guard lock(s.mutex); if (s.busy || !s.valid()) return {}; s.busy=true; }
    struct Finish { State& s; ~Finish() { std::lock_guard lock(s.mutex); s.busy=false; } } finish{s};
    std::promise<std::optional<Reply>> request; auto result=request.get_future(); auto& queue=s.rpcQueues[found-sids.begin()];
    { std::lock_guard lock(queue.mutex); queue.function=function; queue.value=value; queue.sound=sound; queue.request=std::move(request); }
    queue.changed.notify_all(); return result.get();
}
}
