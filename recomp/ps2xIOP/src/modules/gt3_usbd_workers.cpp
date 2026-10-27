#include "ps2x/iop/gt3/usbd_workers.h"
#include "ps2x/iop/native_library.h"
#include <atomic>
#include <condition_variable>
#include <deque>
#include <future>
#include <thread>

namespace ps2x::iop::gt3 {
namespace {
thread_local const void* currentWorker=nullptr;
std::atomic<uint32_t> nextHandle{0x60000000};
uint32_t handle() {
    auto n=nextHandle.load();
    while (n<INT32_MAX) if (nextHandle.compare_exchange_weak(n,n+1)) return n;
    return 0;
}
uint32_t word(std::span<const uint8_t> b,size_t at=0) {
    return uint32_t(b[at])|(uint32_t(b[at+1])<<8)|(uint32_t(b[at+2])<<16)|(uint32_t(b[at+3])<<24);
}
void put(std::span<uint8_t> b,size_t at,uint32_t value) { for (unsigned i=0;i<4;++i) b[at+i]=uint8_t(value>>(i*8)); }
struct Flag { bool& flag; explicit Flag(bool& f):flag(f) { flag=true; } ~Flag() { flag=false; } };
}
struct UsbdWorkers::State {
    struct Event {
        std::mutex mutex; std::condition_variable_any changed; uint32_t bits=0;
        void signal() { { std::lock_guard lock(mutex); bits|=1; } changed.notify_all(); }
        bool wait(std::stop_token stop) {
            std::unique_lock lock(mutex);
            if (!changed.wait(lock,stop,[&] { return bits&1; })) return false;
            bits=0; return true; // Original OR/clear mode0x11.
        }
    } irqEvent,callbackEvent;
    struct Completion {
        uint32_t request; NativeCallbackBinding binding;
        std::array<uint8_t,0x40> bytes;
    };
    IopHost& host; NativeLibraryRegistry* registry; UsbdOhci controller;
    mutable std::recursive_mutex lifecycle,io;
    std::jthread irqWorker,callbackWorker,clock;
    std::mutex clockMutex; std::condition_variable_any clockChanged;
    std::atomic<bool> running{false},failed{false};
    bool starting=false,realtime=false,irqEnabled=false,committing=false;
    uint64_t generation=0,frames=0,interrupts=0,callbacks=0,callbackFailures=0;
    std::array<uint32_t,2> stacks{}; std::array<uint32_t,5> handles{};
    std::deque<Completion> completions;
    explicit State(IopHost& h):host(h),registry(h.nativeLibraries()),controller(h) {}
    bool live() const { return running && !failed && controller.active(); }
    void fail() { failed=true; irqEvent.signal(); callbackEvent.signal(); clockChanged.notify_all(); }
    bool read(uint32_t address,std::span<uint8_t> out) const { return host.readIopMemory(address,out.data(),out.size()); }
    bool commit(std::span<const IopMemoryWrite> writes) {
        if (committing) return false;
        Flag busy(committing); const auto before=generation;
        return host.writeIopMemoryBatch(writes) && generation==before;
    }
    void deliver() { if (irqEnabled && controller.irqPending()) { irqEnabled=false; irqEvent.signal(); } }
    bool frame() {
        std::lock_guard lock(io);
        if (!live() || committing) return false;
        { Flag busy(committing); if (!controller.advanceFrame()) { fail(); return false; } }
        ++frames; deliver(); return true;
    }
    bool serviceInterrupt() {
        const auto a=controller.addresses(); if (!a[0]) return false;
        const auto status=controller.read(UsbdOhci::Base+0xc),enabled=controller.read(UsbdOhci::Base+0x10);
        if (!status || !enabled) return false;
        const auto pending=*status&*enabled&0x7fffffff;
        // Empty-device subset: frame/overflow and disconnected root changes.
        if (pending&~0x64u) return false;
        std::array<uint8_t,4> done{},timers{};
        if (!read(a[1]+0x84,done) || word(done) || !read(a[1]+0x21a0,timers) || word(timers)) return false;
        if (pending&0x40) for (unsigned port=0;port<2;++port) {
            const auto value=controller.read(UsbdOhci::Base+0x54+port*4);
            std::array<uint8_t,1> state{};
            if (!value || (*value&1) || !read(a[1]+0x34ac+(port+1)*0x98+0x18,state) || state[0]!=1) return false;
        }
        std::array<uint8_t,36> counters{};
        if (!read(a[1]+0x217c,counters)) return false;
        put(counters,0,word(counters)+1);
        if (pending&4) put(counters,0xc,word(counters,0xc)+1);
        if (pending&0x20) put(counters,0x18,word(counters,0x18)+1);
        if (pending&0x40) put(counters,0x1c,word(counters,0x1c)+1);
        const IopMemoryWrite write{a[1]+0x217c,counters};
        if (!commit(std::span(&write,1))) return false;
        if (pending&0x40) for (unsigned port=0;port<2;++port)
            if (!controller.write(UsbdOhci::Base+0x54+port*4,0x1f0000)) return false;
        // Original handlers read ISR then OR their bit into a W1C write. Thus
        // even currently masked latched bits are acknowledged by that write.
        if (!controller.write(UsbdOhci::Base+0xc,*status) ||
            !controller.write(UsbdOhci::Base+0x14,0x80000000) ||
            !controller.write(UsbdOhci::Base+0x10,0x80000000)) return false;
        ++interrupts; irqEnabled=true; deliver(); return true;
    }
    struct Queues {
        std::array<uint8_t,0x4000> requests{};
        std::array<uint8_t,8> free{},pending{};
    };
    bool requestIndex(uint32_t address,uint32_t arena,size_t& offset) const {
        if (address<arena+0x47ac || address>=arena+0x87ac || (address-arena-0x47ac)%0x40) return false;
        offset=address-arena-0x47ac; return true;
    }
    bool loadQueues(const std::array<uint32_t,3>& a,Queues& q) const {
        if (!read(a[1]+0x47ac,q.requests) || !read(a[1]+0x2150,q.free) || !read(a[0]+0x57fc,q.pending)) return false;
        std::array<bool,256> visited{}; size_t count=0,pendingCount=0;
        for (unsigned which=0;which<2;++which) {
            const auto& endpoints=which?q.pending:q.free;
            uint32_t node=word(endpoints),previous=0;
            while (node) {
                size_t at=0;
                if (!requestIndex(node,a[1],at) || visited[at/64] || word(q.requests,at)!=at/64 || word(q.requests,at+12)!=previous) return false;
                if (which) {
                    if (word(q.requests,at+4)!=1 || pendingCount>=completions.size() || completions[pendingCount++].request!=node) return false;
                } else if (word(q.requests,at+4)) return false;
                visited[at/64]=true; ++count; previous=node; node=word(q.requests,at+8);
            }
            if (previous!=word(endpoints,4)) return false;
        }
        return count==256 && pendingCount==completions.size();
    }
    bool commitQueues(const std::array<uint32_t,3>& a,const Queues& q) {
        const std::array<IopMemoryWrite,3> writes{{{a[1]+0x47ac,q.requests},{a[1]+0x2150,q.free},{a[0]+0x57fc,q.pending}}};
        return commit(writes);
    }
    bool pop(Completion& result) {
        const auto a=controller.addresses(); Queues q;
        if (!a[0] || completions.empty() || !loadQueues(a,q)) return false;
        result=completions.front(); size_t at=0; if (!requestIndex(result.request,a[1],at)) return false;
        // Queue links may change; callback payload must match the bound snapshot.
        for (size_t n=0x10;n<0x40;++n) if (q.requests[at+n]!=result.bytes[n]) return false;
        const auto next=word(q.requests,at+8),tail=word(q.free,4);
        put(q.pending,0,next);
        size_t index=0;
        if (next) { if (!requestIndex(next,a[1],index)) return false; put(q.requests,index+12,0); }
        else put(q.pending,4,0);
        if (tail) { if (!requestIndex(tail,a[1],index)) return false; put(q.requests,index+8,result.request); }
        else put(q.free,0,result.request);
        put(q.free,4,result.request); put(q.requests,at+4,0); put(q.requests,at+8,0); put(q.requests,at+12,tail);
        if (!commitQueues(a,q)) return false;
        completions.pop_front(); return true;
    }
};
UsbdWorkers::UsbdWorkers(IopHost& h):state_(std::make_unique<State>(h)) {}
UsbdWorkers::~UsbdWorkers() { reset(); }
bool UsbdWorkers::start(bool realtime) {
    auto& s=*state_; if (currentWorker==&s) return false;
    std::lock_guard lock(s.lifecycle);
    std::lock_guard io(s.io);
    if (s.starting) return false;
    if (s.running) return s.live() && s.realtime==realtime;
    Flag starting(s.starting); const auto before=s.generation;
    try {
        if (!s.registry || !s.controller.start()) { reset(); return false; }
        for (auto& stack:s.stacks) stack=s.host.allocateIopMemory(0x4000);
        for (auto& id:s.handles) id=handle();
        if (!s.stacks[0] || !s.stacks[1] || !s.handles.back()) { reset(); return false; }
        const auto a=s.controller.addresses(); if (!a[0]) { reset(); return false; }
        std::array<uint8_t,20> handles{}; std::array<uint8_t,64> metadata{};
        std::vector<uint8_t> stack(0x4000);
        for (size_t i=0;i<5;++i) put(handles,i*4,s.handles[i]);
        for (size_t i=0;i<2;++i) {
            const auto at=i*32; put(metadata,at,0x02000000); put(metadata,at+4,i?0x4254:0x4204);
            put(metadata,at+8,s.stacks[i]); put(metadata,at+12,0x4000); put(metadata,at+16,i?36:30);
        }
        const std::array<IopMemoryWrite,4> writes{{{a[0]+0x57e8,handles},{a[0]+0x58c0,metadata},{s.stacks[0],stack},{s.stacks[1],stack}}};
        if (!s.commit(writes) || s.generation!=before) { reset(); return false; }
        s.failed=false; s.realtime=realtime; s.irqEnabled=true;
        std::promise<void> irqReady,callbackReady;
        auto irqFuture=irqReady.get_future(),callbackFuture=callbackReady.get_future();
        s.irqWorker=std::jthread([&s,ready=std::move(irqReady)](std::stop_token stop) mutable {
            currentWorker=&s; ready.set_value();
            while (!s.failed && s.irqEvent.wait(stop)) {
                try { std::lock_guard lock(s.io); if (s.running && !s.failed && !s.serviceInterrupt()) s.fail(); }
                catch (...) { s.fail(); }
            }
            currentWorker=nullptr;
        });
        s.callbackWorker=std::jthread([&s,ready=std::move(callbackReady)](std::stop_token stop) mutable {
            currentWorker=&s; ready.set_value();
            while (!s.failed && s.callbackEvent.wait(stop)) {
                while (!stop.stop_requested() && !s.failed) {
                    State::Completion job;
                    try {
                        { std::lock_guard lock(s.io);
                          if (!s.running || s.completions.empty()) break;
                          if (!s.pop(job)) { s.fail(); break; } }
                        // Free request and release the semaphore before callback.
                        // No lifecycle/IO lock spans native registry invocation.
                        const auto result=job.binding.callback?s.registry->invokeCallback(job.binding,
                            {word(job.bytes,0x28),word(job.bytes,0x2c),word(job.bytes,0x34),0}):NativeExportResult{true,0};
                        std::lock_guard lock(s.io); ++s.callbacks;
                        if (!result.handled) ++s.callbackFailures;
                    } catch (...) { s.fail(); break; }
                }
            }
            currentWorker=nullptr;
        });
        irqFuture.get(); callbackFuture.get();
        if (s.generation!=before) { reset(); return false; }
        s.running=true;
        if (realtime) s.clock=std::jthread([&s](std::stop_token stop) {
            currentWorker=&s; std::unique_lock lock(s.clockMutex);
            auto deadline=std::chrono::steady_clock::now()+std::chrono::milliseconds(1);
            while (!stop.stop_requested() && !s.failed) {
                s.clockChanged.wait_until(lock,stop,deadline,[&] { return s.failed.load(); });
                if (stop.stop_requested() || s.failed) break;
                lock.unlock();
                try { s.frame(); } catch (...) { s.fail(); }
                lock.lock();
                // No unlimited catch-up burst after host scheduling delays.
                deadline=std::chrono::steady_clock::now()+std::chrono::milliseconds(1);
            }
            currentWorker=nullptr;
        });
        return true;
    } catch (...) { reset(); throw; }
}
bool UsbdWorkers::reset() {
    auto& s=*state_; if (currentWorker==&s) return false;
    std::lock_guard lock(s.lifecycle);
    { std::lock_guard io(s.io); if (s.committing && s.running) return false; }
    s.running=false;
    for (auto* thread:{&s.clock,&s.irqWorker,&s.callbackWorker}) thread->request_stop();
    s.clockChanged.notify_all(); s.irqEvent.changed.notify_all(); s.callbackEvent.changed.notify_all();
    for (auto* thread:{&s.clock,&s.irqWorker,&s.callbackWorker}) if (thread->joinable()) thread->join();
    std::lock_guard io(s.io); ++s.generation;
    s.irqEvent.bits=s.callbackEvent.bits=0; s.completions.clear();
    for (auto& stack:s.stacks) { if (stack) s.host.freeIopMemory(stack); stack=0; }
    s.controller.reset(); s.handles={}; s.failed=false; s.irqEnabled=false;
    s.frames=s.interrupts=s.callbacks=s.callbackFailures=0; return true;
}
UsbdWorkers::Snapshot UsbdWorkers::snapshot() const {
    auto& s=*state_; std::lock_guard lock(s.io);
    return {s.live(),s.failed.load(),s.frames,s.interrupts,s.callbacks,s.callbackFailures,s.completions.size(),s.controller.addresses(),s.handles};
}
bool UsbdWorkers::stepFrame() { auto& s=*state_; std::lock_guard lock(s.io); if (s.realtime) return false; return s.frame(); }
std::optional<uint32_t> UsbdWorkers::read(uint32_t address) const {
    auto& s=*state_; std::lock_guard lock(s.io); return s.live()?s.controller.read(address):std::optional<uint32_t>{};
}
bool UsbdWorkers::write(uint32_t address,uint32_t value) {
    auto& s=*state_; std::lock_guard lock(s.io);
    if (!s.live() || s.committing || !s.controller.write(address,value)) return false;
    s.deliver(); return true;
}
bool UsbdWorkers::postCompletion(uint32_t callback,int32_t status,uint32_t transferred,uint32_t argument,uint32_t gp) {
    auto& s=*state_;
    if (!s.registry) return false;
    const auto binding=callback?s.registry->bindCallback(callback):NativeCallbackBinding{};
    if (callback && !binding.callback) return false;
    std::lock_guard lock(s.io);
    if (!s.live() || s.committing) return false;
    const auto a=s.controller.addresses(); State::Queues q;
    if (!s.loadQueues(a,q)) return false;
    const auto head=word(q.free),tail=word(q.pending,4); size_t at=0,index=0;
    if (!head || !s.requestIndex(head,a[1],at)) return false;
    const auto next=word(q.requests,at+8);
    State::Completion job{head,binding,{}};
    put(job.bytes,0,uint32_t(at/64)); put(job.bytes,4,1); put(job.bytes,12,tail);
    put(job.bytes,0x28,uint32_t(status)); put(job.bytes,0x2c,transferred); put(job.bytes,0x34,argument);
    put(job.bytes,0x38,callback); put(job.bytes,0x3c,gp);
    put(q.free,0,next);
    if (next) { if (!s.requestIndex(next,a[1],index)) return false; put(q.requests,index+12,0); }
    else put(q.free,4,0);
    if (tail) { if (!s.requestIndex(tail,a[1],index)) return false; put(q.requests,index+8,head); }
    else put(q.pending,0,head);
    put(q.pending,4,head); std::copy(job.bytes.begin(),job.bytes.end(),q.requests.begin()+at);
    // Allocate host queue capacity before mutating IOP state.
    s.completions.push_back(job);
    try { if (!s.commitQueues(a,q)) { s.completions.pop_back(); return false; } }
    catch (...) { s.completions.pop_back(); throw; }
    s.callbackEvent.signal(); return true;
}
}
