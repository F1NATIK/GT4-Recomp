#include "ps2x/iop/gt3/pdiusb_resident.h"
#include "ps2x/iop/native_library.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <future>
#include <mutex>
#include <semaphore>
#include <vector>

namespace ps2x::iop::gt3 {
namespace {
std::atomic<uint64_t> handles{0x6e000000};
thread_local const void* currentPdiusb=nullptr;
struct Operation {
    const void* previous;
    explicit Operation(const void* state):previous(currentPdiusb) { currentPdiusb=state; }
    ~Operation() { currentPdiusb=previous; }
};
uint32_t handle() { const auto h=handles.fetch_add(1); return h<0x6f000000?uint32_t(h):0; }
uint32_t word(std::span<const uint8_t> b,size_t at) {
    uint32_t v=0; for (unsigned i=0;i<4;++i) v|=uint32_t(b[at+i])<<(8*i); return v;
}
void put(std::span<uint8_t> b,size_t at,uint32_t v) {
    for (unsigned i=0;i<4;++i) b[at+i]=uint8_t(v>>(8*i));
}
constexpr std::array<uint32_t,3> callbacks{0x1320,0x13bc,0x171c};
RpcResult rejected() { RpcResult r; r.serverDispatchPolicy=ServerDispatchPolicy::RejectUnhandled; return r; }
std::vector<uint8_t> originalModule(IopHost& host) {
    const auto file=host.openHostFile(host.translateGuestPath("cdrom0:\\PDIUSB.IRX;1")); if (!file) return {};
    struct Close { IopHost& host; uint64_t file; ~Close() { host.closeHostFile(file); } } close{host,file};
    uint64_t size=0; size_t count=0; std::array<uint8_t,32> digest{};
    if (!host.hostFileSize(file,size) || size!=18373) return {};
    std::vector<uint8_t> bytes(static_cast<size_t>(size));
    if (!host.readHostFile(file,0,bytes.data(),bytes.size(),count) || count!=bytes.size() || !host.sha256(bytes,digest)) return {};
    constexpr std::string_view expected="19cd22589d3794f3597f3605d2d86c7eaf486ae8cb20ac4ed0943d21363ae6d6";
    constexpr char hex[]="0123456789abcdef"; std::string actual;
    for (auto b:digest) { actual+=hex[b>>4]; actual+=hex[b&15]; }
    return actual==expected?std::move(bytes):std::vector<uint8_t>{};
}
}

struct PdiusbResident::State {
    using Reply=std::array<uint8_t,ReplyBytes>;
    struct Binary { std::binary_semaphore value{0}; };
    struct Job { uint32_t function=0; std::vector<uint8_t> input; std::promise<std::optional<Reply>> promise; };
    IopHost& host; NativeLibraryRegistry* registry;
    mutable std::recursive_mutex lifecycle;
    std::mutex gateMutex,queueMutex;
    std::condition_variable_any gateChanged,queueChanged;
    bool gate=false,running=false,driverAttempted=false;
    std::array<std::jthread,2> workers;
    std::array<std::unique_ptr<Binary>,2> semaphores;
    std::optional<Job> job;
    Snapshot resources;
    std::array<uint8_t,ModelBytes> imageBytes{};
    std::array<uint8_t,0x98> contextBytes{};
    NativeImportBinding allocate,free,registerDriver,removeDriver,emptyDevice;
    std::array<NativeCallbackBinding,3> bindings{};
    std::atomic<Phase> phase{Phase::Stopped};
    std::atomic<uint64_t> queries{0};
    explicit State(IopHost& h):host(h),registry(h.nativeLibraries()) {}
    bool importsLive() const {
        if (!registry || !allocate.registration || !registerDriver.registration) return false;
        for (auto b:{allocate,free}) if (registry->bind("libpdi",0x10a,b.ordinal).registration!=b.registration) return false;
        for (auto b:{registerDriver,removeDriver,emptyDevice}) if (registry->bind("usbd",0x101,b.ordinal).registration!=b.registration) return false;
        for (size_t i=0;i<bindings.size();++i) {
            const auto found=registry->bindCallback(resources.image+callbacks[i]);
            if (!found.callback || found.callback!=bindings[i].callback || found.registration!=resources.callbackOwner) return false;
        }
        const auto absent=registry->invoke(emptyDevice,{});
        return absent.handled && absent.value==0;
    }
    // Workers never touch the library registry. The owning runtime serializes
    // dependency teardown; public requests pin/check the exact import tokens.
    bool memoryValid() const {
        if (!resources.image || !resources.context || !resources.request || !resources.reply) return false;
        std::array<uint8_t,ModelBytes> actual{};
        if (!host.readIopMemory(resources.image,actual.data(),actual.size())) return false;
        // USBD legitimately changes this driver's linked-list neighbours.
        std::copy_n(imageBytes.begin()+0x34b0,8,actual.begin()+0x34b0);
        if (actual!=imageBytes) return false;
        std::array<uint8_t,0x98> context{};
        std::array<uint8_t,0x30> request{};
        if (!host.readIopMemory(resources.context,context.data(),context.size()) || context!=contextBytes ||
            !host.readIopMemory(resources.request,request.data(),request.size()) ||
            word(request,0x20)!=resources.context || word(request,0x24)!=resources.reply ||
            word(request,0x28) || word(request,0x2c)) return false;
        uint8_t end=0;
        for (auto address:resources.stacks) if (!address || !host.readIopMemory(address+0x7ff,&end,1)) return false;
        return host.readIopMemory(resources.reply+ReplyBytes-1,&end,1);
    }
    std::optional<Reply> execute(const Job& request) {
        if (phase!=Phase::Ready || !memoryValid()) return {};
        Reply reply{}; if (!host.readIopMemory(resources.reply,reply.data(),reply.size())) return {};
        put(reply,0,request.function==1?0:3);
        if (request.function==1) std::fill(reply.begin()+8,reply.begin()+24,0);
        std::array<IopMemoryWrite,2> writes{{{resources.reply,reply},{resources.request,request.input}}};
        struct Mutation { State& s; bool done=false; ~Mutation() { if (!done) s.phase=Phase::MemoryFailure; } } mutation{*this};
        const auto count=request.input.empty()?1u:2u;
        if (!host.writeIopMemoryBatch(std::span(writes).first(count))) return {};
        Reply found{}; std::vector<uint8_t> input(request.input.size());
        if (!host.readIopMemory(resources.reply,found.data(),found.size()) || found!=reply ||
            (!input.empty() && (!host.readIopMemory(resources.request,input.data(),input.size()) || input!=request.input)) || !memoryValid()) return {};
        mutation.done=true; ++queries; return reply;
    }
    bool release() {
        running=false; phase=Phase::Stopped;
        for (auto& worker:workers) worker.request_stop();
        gateChanged.notify_all(); queueChanged.notify_all();
        for (auto& worker:workers) if (worker.joinable()) worker.join();
        job.reset(); for (auto& sema:semaphores) sema.reset(); gate=false;
        if (driverAttempted) {
            if (registry->bind("usbd",0x101,5).registration==removeDriver.registration) {
                const auto removed=registry->invoke(removeDriver,{resources.image+0x34b0,0,0,0});
                // A poisoned live USB catalog may still own this descriptor.
                // Retain it until USBD is reset; never free a referenced image.
                if (!removed.handled || (removed.value!=0 && removed.value!=0x104)) return false;
            }
            driverAttempted=false;
        }
        if (resources.callbackOwner) { registry->unregisterCallbackOwner(resources.callbackOwner); resources.callbackOwner=0; }
        for (auto* address:{&resources.reply,&resources.request,&resources.context}) if (*address) {
            if (registry->bind("libpdi",0x10a,14).registration==free.registration) {
                const auto released=registry->invoke(free,{*address,0,0,0});
                if (!released.handled || released.value) return false;
            }
            // If the provider disappeared, its allocation map still owns the
            // block (or freed it at reset). Never raw-free a borrowed address.
            *address=0;
        }
        for (auto address:resources.stacks) if (address) host.freeIopMemory(address);
        if (resources.image) host.freeIopMemory(resources.image);
        resources={}; imageBytes={}; contextBytes={}; bindings={}; queries=0;
        allocate={}; free={}; registerDriver={}; removeDriver={}; emptyDevice={}; return true;
    }
};

PdiusbResident::PdiusbResident(IopHost& host):state_(std::make_shared<State>(host)) {}
PdiusbResident::~PdiusbResident() { try { reset(); } catch (...) {} }
bool PdiusbResident::start() {
    auto& s=*state_; if (currentPdiusb==&s) return false;
    std::lock_guard lifecycle(s.lifecycle); Operation operation(&s);
    if (s.running || s.resources.image || !s.registry) return false;
    struct Transaction { State& s; bool done=false; ~Transaction() { if (!done) { try { s.release(); } catch (...) {} } } } transaction{s};
    const auto original=originalModule(s.host); if (original.empty()) return false;
    s.allocate=s.registry->bind("libpdi",0x10a,11); s.free=s.registry->bind("libpdi",0x10a,14);
    s.registerDriver=s.registry->bind("usbd",0x101,4); s.removeDriver=s.registry->bind("usbd",0x101,5); s.emptyDevice=s.registry->bind("usbd",0x101,6);
    if (!s.allocate.registration || s.allocate.registration!=s.free.registration || !s.registerDriver.registration ||
        s.registerDriver.registration!=s.removeDriver.registration || s.registerDriver.registration!=s.emptyDevice.registration) return false;
    const auto absent=s.registry->invoke(s.emptyDevice,{}); if (!absent.handled || absent.value) return false;
    s.resources.image=s.host.allocateIopMemory(ModelBytes);
    if (!s.resources.image || (s.resources.image&3) || s.resources.image>UINT32_MAX-0xb530 ||
        !s.host.readIopMemory(s.resources.image,s.imageBytes.data(),s.imageBytes.size())) return false;
    s.imageBytes={};
    std::copy_n(original.begin()+0x33c0,0x170,s.imageBytes.begin()+0x3320);
    std::copy_n(original.begin()+0x3530,0xb0,s.imageBytes.begin()+0x3490);
    for (size_t at=0x4604;at<0x47b4;at+=8) {
        const auto offset=word(original,at);
        if (word(original,at+4)!=2 || offset<0x3320 || offset>0x353c || (offset&3)) return false;
        put(s.imageBytes,offset,s.resources.image+word(s.imageBytes,offset));
    }
    std::array<uint32_t*,3> allocated{&s.resources.context,&s.resources.request,&s.resources.reply};
    constexpr std::array<uint32_t,3> sizes{0x98,0x30,0x80};
    for (size_t i=0;i<3;++i) {
        const auto result=s.registry->invoke(s.allocate,{sizes[i],0,0,0});
        if (!result.handled || !result.value) return false;
        *allocated[i]=uint32_t(result.value);
        std::vector<uint8_t> check(sizes[i]);
        if ((*allocated[i]&3) || !s.host.readIopMemory(*allocated[i],check.data(),check.size())) return false;
    }
    s.resources.callbackOwner=s.registry->registerCallbackOwner(); if (!s.resources.callbackOwner) return false;
    for (size_t i=0;i<callbacks.size();++i) {
        std::weak_ptr<State> weak=state_;
        if (!s.registry->registerCallback(s.resources.callbackOwner,s.resources.image+callbacks[i],[weak](const auto&) {
            if (auto state=weak.lock()) state->phase=Phase::UnsupportedDevice;
            return NativeExportResult{}; // Attached-device callbacks are explicit boundaries.
        })) return false;
        s.bindings[i]=s.registry->bindCallback(s.resources.image+callbacks[i]);
    }
    std::array<uint8_t,0x30> request{}; std::array<uint8_t,0x800> stack{};
    put(request,0x20,s.resources.context); put(request,0x24,s.resources.reply);
    put(s.imageBytes,0x349c,s.resources.context);
    auto& context=s.contextBytes; context={};
    put(context,0,Sid); put(context,4,s.resources.image+0x1ac8); put(context,8,s.resources.request);
    put(context,0xc,0xe8); put(context,0x10,s.resources.image+0x33d0); put(context,0x14,2);
    std::array<IopMemoryWrite,5> writes{{{s.resources.image,s.imageBytes},{s.resources.context,context},{s.resources.request,request}}};
    for (size_t i=0;i<2;++i) {
        s.resources.stacks[i]=s.host.allocateIopMemory(0x800); s.resources.threads[i]=handle(); s.resources.semaphores[i]=handle();
        if (!s.resources.stacks[i] || !s.resources.threads[i] || !s.resources.semaphores[i]) return false;
        s.semaphores[i]=std::make_unique<State::Binary>();
        put(context,0x20+i*4,s.resources.threads[i]);
        put(context,i==0?0x1c:0x18,s.resources.semaphores[i]);
        const auto meta=0x3600+i*0x40;
        put(s.imageBytes,meta,0x02000000); put(s.imageBytes,meta+4,s.resources.image+(i==0?0xe68:0x1d8c));
        put(s.imageBytes,meta+8,s.resources.stacks[i]); put(s.imageBytes,meta+12,0x800); put(s.imageBytes,meta+16,32);
        put(s.imageBytes,meta+20,s.resources.threads[i]); put(s.imageBytes,meta+24,s.resources.context);
        put(s.imageBytes,0x3680+i*16,s.resources.semaphores[i]); put(s.imageBytes,0x3688+i*16,1);
        writes[i+3]={s.resources.stacks[i],stack};
    }
    put(s.imageBytes,0x36b0,Sid); put(s.imageBytes,0x36b4,s.resources.context+0x40); put(s.imageBytes,0x36b8,s.resources.context+0x28);
    put(s.imageBytes,0x36bc,s.resources.image+0x1ac8); put(s.imageBytes,0x36c0,s.resources.request);
    // Preserve the original reply allocation's contents; only context/request
    // were memset by startup. Original text and opaque kernel structs stay zero.
    if (!s.host.writeIopMemoryBatch(writes) || !s.memoryValid()) return false;
    for (auto address:s.resources.stacks) { std::array<uint8_t,0x800> found{}; if (!s.host.readIopMemory(address,found.data(),found.size()) || found!=stack) return false; }
    s.driverAttempted=true;
    const auto registered=s.registry->invoke(s.registerDriver,{s.resources.image+0x34b0,0,0,0},NativeCallContext{s.resources.image+0xb530});
    if (!registered.handled || registered.value) return false;
    put(s.imageBytes,0x34dc,s.resources.image+0xb530);
    if (!s.importsLive() || !s.memoryValid()) return false;
    std::promise<bool> rpcReady; auto ready=rpcReady.get_future();
    for (unsigned i=0;i<2;++i) {
        std::promise<void> created; auto initialized=created.get_future();
        s.workers[i]=std::jthread([&s,i,created=std::move(created),started=i==0?std::move(rpcReady):std::promise<bool>{}](std::stop_token stop) mutable {
            Operation worker(&s); s.resources.workerIds[i]=std::this_thread::get_id();
            { std::unique_lock lock(s.gateMutex); created.set_value();
              if (!s.gateChanged.wait(lock,stop,[&] { return s.gate; }) || stop.stop_requested()) return; }
            if (i==1) {
                s.semaphores[0]->value.release(); // Registration completed before this handoff.
                std::unique_lock lock(s.gateMutex); s.gateChanged.wait(lock,stop,[] { return false; }); return;
            }
            while (!stop.stop_requested() && !s.semaphores[0]->value.try_acquire_for(std::chrono::milliseconds(1))) {}
            if (stop.stop_requested()) { started.set_value(false); return; }
            started.set_value(true);
            std::unique_lock lock(s.queueMutex);
            while (s.queueChanged.wait(lock,stop,[&] { return s.job.has_value(); }) && !stop.stop_requested()) {
                auto request=std::move(*s.job); s.job.reset(); lock.unlock();
                try { request.promise.set_value(s.execute(request)); }
                catch (...) { request.promise.set_exception(std::current_exception()); }
                lock.lock();
            }
        }); initialized.get();
    }
    s.phase=Phase::Ready;
    { std::lock_guard lock(s.gateMutex); s.gate=true; } s.gateChanged.notify_all();
    if (!ready.get() || !s.importsLive() || !s.memoryValid()) return false;
    s.running=true; transaction.done=true; return true;
}
bool PdiusbResident::reset() {
    auto& s=*state_; if (currentPdiusb==&s) return false;
    std::lock_guard lifecycle(s.lifecycle); Operation operation(&s); return s.release();
}
bool PdiusbResident::active() const { const auto found=snapshot(); return found && found->phase==Phase::Ready; }
std::optional<PdiusbResident::Snapshot> PdiusbResident::snapshot() const {
    auto& s=*state_; if (currentPdiusb==&s) return {};
    std::unique_lock lifecycle(s.lifecycle,std::try_to_lock); if (!lifecycle || !s.running) return {};
    Operation operation(&s); if (!s.importsLive() || !s.memoryValid()) return {};
    auto result=s.resources; result.phase=s.phase.load(); result.queries=s.queries.load(); return result;
}
RpcResult PdiusbResident::handleRpc(const RpcRequest& request) {
    auto& s=*state_; if (currentPdiusb==&s || request.sid!=Sid || request.send.size>0x20 || request.receive.size>ReplyBytes ||
        (request.send.size && !request.send.address) || (request.receive.size && !request.receive.address)) return rejected();
    std::lock_guard lifecycle(s.lifecycle); Operation operation(&s);
    if (!s.running || s.phase!=Phase::Ready || !s.importsLive() || !s.memoryValid()) return rejected();
    State::Job job; job.function=request.function; job.input.resize(request.send.size);
    if (!job.input.empty() && !s.host.readGuest(request.send.address,job.input.data(),job.input.size())) return rejected();
    // Parse before writing: the EE may alias its send and receive buffers.
    auto answer=job.promise.get_future();
    { std::lock_guard lock(s.queueMutex); s.job=std::move(job); } s.queueChanged.notify_all();
    const auto reply=answer.get();
    if (!reply || s.phase!=Phase::Ready || !s.importsLive() || !s.memoryValid()) return rejected();
    RpcResult result; result.handled=true; result.serverDispatchPolicy=ServerDispatchPolicy::Suppress;
    if (request.receive.size) {
        const std::array<IopMemoryWrite,1> writes{{{request.receive.address,std::span(*reply).first(request.receive.size)}}};
        if (!s.host.commitIopAndEeMemory(writes)) return rejected();
        result.resultAddress=request.receive.address;
    }
    return result;
}
}
