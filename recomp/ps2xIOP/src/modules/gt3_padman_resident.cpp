#include "ps2x/iop/gt3/padman_resident.h"
#include "ps2x/iop/gt3/padman_frame_input.h"
#include "ps2x/iop/native_library.h"
#include <array>
#include <atomic>
#include <condition_variable>
#include <future>
#include <mutex>
#include <optional>
#include <thread>

namespace ps2x::iop::gt3 {
namespace {
void put(std::span<uint8_t> bytes,size_t offset,uint32_t value) {
    for (unsigned i=0;i<4;++i) bytes[offset+i]=uint8_t(value>>(i*8));
}
uint32_t word(std::span<const uint8_t> bytes,size_t offset) {
    return uint32_t(bytes[offset])|(uint32_t(bytes[offset+1])<<8)|
        (uint32_t(bytes[offset+2])<<16)|(uint32_t(bytes[offset+3])<<24);
}
bool verified(IopHost& host) {
    const auto file=host.openHostFile(host.translateGuestPath("cdrom0:\\PADMAN.IRX;1"));
    if (!file) return false;
    struct Close { IopHost& host; uint64_t file; ~Close() { host.closeHostFile(file); } } close{host,file};
    uint64_t size=0; size_t read=0; std::array<uint8_t,32> digest{};
    if (!host.hostFileSize(file,size) || size<52 || size>1024*1024) return false;
    std::vector<uint8_t> bytes(static_cast<size_t>(size));
    if (!host.readHostFile(file,0,bytes.data(),bytes.size(),read) || read!=bytes.size() || !host.sha256(bytes,digest)) return false;
    constexpr std::string_view expected="10120230a66017c200c5ce8e4b9f11c42ffb240bc2df9d194e5d2624625df5d7";
    constexpr char hex[]="0123456789abcdef"; std::string actual;
    for (auto b:digest) { actual+=hex[b>>4]; actual+=hex[b&15]; }
    return actual==expected;
}
RpcResult rejected() {
    RpcResult result; result.serverDispatchPolicy=ServerDispatchPolicy::RejectUnhandled; return result;
}
}
struct PadmanResident::State {
    struct Reply { bool handled=false; std::array<uint8_t,0x80> bytes{}; };
    struct Job { std::array<uint8_t,0x80> bytes; std::promise<Reply> result; };
    struct Queue {
        std::mutex mutex;
        std::condition_variable_any changed;
        std::optional<Job> pending;
        std::jthread worker;
    };
    IopHost& host; NativeLibraryRegistry* registry;
    mutable std::recursive_mutex lifecycle;
    std::array<Queue,2> queues;
    std::array<uint32_t,2> stacks{};
    uint32_t image=0;
    uint64_t registration=0,rejections=0;
    bool running=false;
    PadmanFrameInput input;
    bool frameEvents=false;
    std::atomic<bool> polling{false};
    uint64_t frameFailures=0;
    uint32_t commandsSeen=0;
    explicit State(IopHost& h,bool frames):host(h),registry(h.nativeLibraries()),input(h),frameEvents(frames) {}
    bool active() const {
        return running && registry && registry->bind("padman",0x203,5).registration==registration;
    }
    static Reply execute(size_t queue,const std::array<uint8_t,0x80>& input) {
        Reply reply{false,input};
        if (queue==1) { // Original0x7624 logs and returns the input buffer unchanged.
            reply.handled=true; return reply;
        }
        switch (word(input,0)) {
        case 18: put(reply.bytes,0xc,0x402); break; // 0x70f4 -> 0x44d0.
        case 12: put(reply.bytes,0xc,2); break; // 0x73b4 -> 0x44a8.
        case 15: put(reply.bytes,0xc,1); break; // padEnd while uninitialized,0x34a4 ->0x3574.
        case 20: put(reply.bytes,8,1); break; // 0x70c0 -> 0x44e0: diagnostic flag; commit below.
        default: return reply; // In particular, padInit16 and port open1 are not ready.
        }
        reply.handled=true; return reply;
    }
};
PadmanResident::PadmanResident(IopHost& host,bool frames):state_(std::make_unique<State>(host,frames)) {}
PadmanResident::~PadmanResident() { reset(); }
bool PadmanResident::active() const { std::lock_guard lock(state_->lifecycle); return state_->active(); }
bool PadmanResident::start() {
    auto& s=*state_; std::lock_guard lock(s.lifecycle);
    if (!s.registry || s.running || !verified(s.host)) return false;
    try {
        s.image=s.host.allocateIopMemory(0x9a00);
        for (auto& stack:s.stacks) stack=s.host.allocateIopMemory(0x800);
        if (!s.image || !s.stacks[0] || !s.stacks[1]) { reset(); return false; }
        std::vector<uint8_t> image(0x9a00),stack(0x800);
        put(image,0x7d34,0x402); put(image,0x8bcc,20); put(image,0x8bd0,46);
        // Native metadata, not executable IOP thread structures or host priorities.
        for (size_t i=0;i<2;++i) {
            const size_t offset=0x9940+i*0x20;
            put(image,offset,0x02000000); put(image,offset+4,i?0x7654:0x7590);
            put(image,offset+8,s.stacks[i]); put(image,offset+12,0x800);
            put(image,offset+16,46); put(image,offset+20,0x80000100+uint32_t(i));
        }
        const std::array<IopMemoryWrite,3> writes{{{s.image,image},{s.stacks[0],stack},{s.stacks[1],stack}}};
        if (!s.host.writeIopMemoryBatch(writes)) { reset(); return false; }
        // The only published export is the verified, already-uninitialized
        // padEnd path. Other original library operations remain unbound.
        s.registration=s.registry->registerLibrary({"padman",0x203,{{5,[state=&s](const auto&) {
            // This scoped export still supports only the uninitialized path.
            // Full initialized teardown is owned by the controlling RPC/frame
            // thread; never claim it occurred from this registry callback.
            return state->polling.load()?NativeExportResult{}:NativeExportResult{true,1};
        }}}});
        if (!s.registration) { reset(); return false; }
        for (size_t i=0;i<s.queues.size();++i) {
            auto& queue=s.queues[i];
            std::promise<void> initialized; auto ready=initialized.get_future();
            queue.worker=std::jthread([&queue,i,initialized=std::move(initialized)](std::stop_token stop) mutable {
                std::unique_lock lock(queue.mutex);
                initialized.set_value();
                while (queue.changed.wait(lock,stop,[&] { return queue.pending.has_value(); })) {
                    auto job=std::move(*queue.pending); queue.pending.reset(); lock.unlock();
                    try { job.result.set_value(State::execute(i,job.bytes)); }
                    catch (...) { job.result.set_exception(std::current_exception()); }
                    lock.lock();
                }
            });
            ready.get();
        }
        s.running=true; return true;
    } catch (...) { reset(); throw; }
}
void PadmanResident::reset() {
    auto& s=*state_; std::lock_guard lock(s.lifecycle); s.running=false;
    s.polling=false; s.input.reset(); s.frameFailures=0;
    for (auto& queue:s.queues) {
        if (queue.worker.joinable()) { queue.worker.request_stop(); queue.changed.notify_all(); queue.worker.join(); }
        queue.pending.reset();
    }
    if (s.registration) s.registry->unregisterLibrary(s.registration);
    if (s.image) s.host.freeIopMemory(s.image);
    for (auto& stack:s.stacks) { if (stack) s.host.freeIopMemory(stack); stack=0; }
    s.image=0; s.registration=s.rejections=0;
}
RpcResult PadmanResident::handleRpc(const RpcRequest& request) {
    auto& s=*state_; std::lock_guard lock(s.lifecycle);
    if (!s.active() || (request.sid!=0x80000100 && request.sid!=0x80000101) || request.function!=1 ||
        !request.send.address || request.send.size<0x80 || !request.receive.address || request.receive.size<0x80) return rejected();
    State::Job job{};
    if (!s.host.readGuest(request.send.address,job.bytes.data(),job.bytes.size())) return rejected();
    const auto command=word(job.bytes,0);
    const size_t index=request.sid-0x80000100;
    if (index==0 && s.frameEvents && (command==16 || command==1 || command==14 || (command==15 && s.input.initialized()))) {
        auto reply=job.bytes;
        put(reply,0xc,1);
        if (command==1) put(reply,0x14,s.image+0x7de0);
        const std::array<IopMemoryWrite,2> replies{{{s.image+0x97d0,reply},{request.receive.address,reply}}};
        bool handled=false;
        if (command==16 && word(job.bytes,0x10)==0)
            handled=s.input.initialize(s.image,replies);
        else if (command==1 && word(job.bytes,4)==0 && word(job.bytes,8)==0)
            handled=s.input.open(word(job.bytes,0x10),replies);
        else if (command==14 && word(job.bytes,4)==0 && word(job.bytes,8)==0 && word(job.bytes,0x10)==1)
            handled=s.input.close(replies);
        else if (command==15)
            handled=s.input.end(replies);
        if (!handled) return rejected();
        s.polling=s.input.initialized();
        s.host.log(LogLevel::Info,"[GT3/PADMAN] native frame-driven RPC="+std::to_string(command)+
            "; initialized="+std::to_string(s.input.initialized())+"; port0-open="+std::to_string(s.input.opened()));
        RpcResult result; result.handled=true; result.resultAddress=request.receive.address;
        result.serverDispatchPolicy=ServerDispatchPolicy::Suppress; return result;
    }
    if (index==0 && s.frameEvents && command>=3 && command<=11 && s.input.initialized()) {
        // Port queries, main mode, actuators, pressure mode and VREF run the
        // original functions against the native port state (padman_port_state.h).
        auto reply=job.bytes;
        const std::array<uint32_t,2> addresses{s.image+0x97d0,request.receive.address};
        if (!s.input.command(reply,addresses)) return rejected();
        if (!(s.commandsSeen&(1u<<command))) {
            s.commandsSeen|=1u<<command;
            s.host.log(LogLevel::Info,"[GT3/PADMAN] native port RPC="+std::to_string(command)+" first use; port="+
                std::to_string(word(job.bytes,4))+" slot="+std::to_string(word(job.bytes,8)));
        }
        RpcResult result; result.handled=true; result.resultAddress=request.receive.address;
        result.serverDispatchPolicy=ServerDispatchPolicy::Suppress; return result;
    }
    auto future=job.result.get_future(); auto& queue=s.queues[index];
    { std::lock_guard lock(queue.mutex); queue.pending=std::move(job); }
    queue.changed.notify_all(); const auto reply=future.get();
    if (!reply.handled) {
        if (s.rejections!=UINT64_MAX) ++s.rejections;
        if (s.rejections==1 || (s.rejections&(s.rejections-1))==0)
            s.host.log(LogLevel::Warning,"[GT3/PADMAN] unsupported RPC command="+std::to_string(command)+
                "; rejected="+std::to_string(s.rejections)+"; unsupported input mode/operation; reply preserved");
        return rejected();
    }
    // Original request20 stores the full argument word at module+0x7d38,
    // returns 1 at packet+8 (not +12), and does not initialize input. Stage
    // its state change with both packet copies so bad outputs cannot apply it.
    const bool setDiagnostic=index==0 && command==20;
    std::array<uint8_t,4> diagnostic{};
    if (setDiagnostic) put(diagnostic,0,word(reply.bytes,4));
    const std::array<IopMemoryWrite,3> pending{{
        {s.image+(index?0x98b0u:0x97d0u),reply.bytes},
        {request.receive.address,reply.bytes}, {s.image+0x7d38,diagnostic}}};
    const auto writes=std::span<const IopMemoryWrite>(pending.data(),setDiagnostic?3:2);
    if (!s.host.commitIopAndEeMemory(writes)) return rejected();
    if (index) s.host.log(LogLevel::Info,"[GT3/PADMAN] supplied secondary RPC diagnostic acknowledged; packet unchanged");
    RpcResult result; result.handled=true; result.resultAddress=request.receive.address;
    result.serverDispatchPolicy=ServerDispatchPolicy::Suppress; return result;
}
void PadmanResident::onVBlank(bool start,uint64_t tick) {
    auto& s=*state_; std::lock_guard lock(s.lifecycle);
    if (!s.frameEvents || !s.active() || !s.input.initialized()) return;
    if (!s.input.onVBlank(start,tick) && start) {
        if (s.frameFailures!=UINT64_MAX) ++s.frameFailures;
        if (s.frameFailures==1 || (s.frameFailures&(s.frameFailures-1))==0)
            s.host.log(LogLevel::Warning,"[GT3/PADMAN] frame input publication failed; count="+std::to_string(s.frameFailures)+"; previous frame preserved");
    }
}
}
