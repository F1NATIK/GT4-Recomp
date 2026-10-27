#include "ps2x/iop/gt3/pdicdvd_resident.h"
#include "ps2x/iop/native_library.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <future>
#include <mutex>
#include <thread>

namespace ps2x::iop::gt3 {
namespace {
constexpr uint32_t ImageBytes=0x1500;
thread_local const void* currentWorker=nullptr;
std::atomic<uint32_t> nextHandle{0x64000000};
uint32_t newHandle() {
    auto n=nextHandle.load();
    while (n<0x65000000) if (nextHandle.compare_exchange_weak(n,n+1)) return n;
    return 0;
}
void put(std::span<uint8_t> b,size_t p,uint32_t v) { for (unsigned i=0;i<4;++i) b[p+i]=uint8_t(v>>(i*8)); }
uint32_t word(std::span<const uint8_t> b,size_t p=0) {
    return uint32_t(b[p])|(uint32_t(b[p+1])<<8)|(uint32_t(b[p+2])<<16)|(uint32_t(b[p+3])<<24);
}
RpcResult rejected() { RpcResult r; r.serverDispatchPolicy=ServerDispatchPolicy::RejectUnhandled; return r; }
std::vector<uint8_t> verifiedImage(IopHost& host) {
    const auto file=host.openHostFile(host.translateGuestPath("cdrom0:\\PDICDVD.IRX;1")); if (!file) return {};
    struct Close { IopHost& host; uint64_t file; ~Close() { host.closeHostFile(file); } } close{host,file};
    uint64_t size=0; size_t n=0; std::array<uint8_t,32> digest{};
    if (!host.hostFileSize(file,size) || size<0x1330 || size>1024*1024) return {};
    std::vector<uint8_t> bytes(static_cast<size_t>(size));
    if (!host.readHostFile(file,0,bytes.data(),bytes.size(),n) || n!=bytes.size() || !host.sha256(bytes,digest)) return {};
    constexpr std::string_view expected="612fd3462759da91814d007c5fb802ccac018fe9697c131da94fa98727543fbf";
    constexpr char hex[]="0123456789abcdef"; std::string actual;
    for (auto b:digest) { actual+=hex[b>>4]; actual+=hex[b&15]; }
    if (actual!=expected) return {};
    std::vector<uint8_t> image(ImageBytes);
    std::copy_n(bytes.begin()+0x12e0,0x50,image.begin()+0x1240); return image;
}
std::optional<PdicdvdResident::Calendar> utcCalendar() {
    using namespace std::chrono;
    const auto now=floor<seconds>(system_clock::now()); const auto day=floor<days>(now);
    const year_month_day ymd{day}; const hh_mm_ss time{now-day};
    if (int(ymd.year())<2000 || int(ymd.year())>2099) return {};
    return PdicdvdResident::Calendar{uint32_t(int(ymd.year())),unsigned(ymd.month()),unsigned(ymd.day()),
        uint32_t(time.hours().count()),uint32_t(time.minutes().count()),uint32_t(time.seconds().count())};
}
}
struct PdicdvdResident::State {
    struct Reply {
        bool handled=false;
        uint32_t destination=0; // Module-relative control/result location, zero for no write.
        std::vector<uint8_t> bytes;
        bool copyToEe=false;
        uint32_t dataDestination=0;
        std::vector<uint8_t> data;
    };
    struct Job { uint32_t function; std::array<uint8_t,16> bytes{}; std::promise<Reply> promise; };
    struct SectorJob { uint32_t lsn,bytes,identityLsn,identityChecksum; std::promise<std::optional<std::vector<uint8_t>>> promise; };
    struct Queue {
        std::mutex mutex; std::condition_variable_any changed;
        std::optional<Job> pending; std::jthread worker;
    };
    // The read worker consumes bounded ISO jobs. Active cancellation/retry and
    // long-running ring-buffer streaming remain outside this subset.
    struct BinarySignal { std::mutex mutex; std::condition_variable_any changed; unsigned count=0; };
    IopHost& host; NativeLibraryRegistry* registry; CalendarSource calendar;
    mutable std::recursive_mutex mutex,lifecycle;
    std::array<Queue,2> queues;
    std::array<BinarySignal,2> signals;
    std::optional<SectorJob> sectorJob; // Protected by signals[1].mutex.
    std::mutex readQueueMutex,streamMutex; // Original count1/max1 synchronization resources.
    std::array<std::jthread,2> idleWorkers;
    std::array<uint32_t,4> stacks{},threads{},semas{};
    uint32_t image=0; uint64_t registration=0;
    uint64_t completedReads=0;
    unsigned tracedRejections=0;
    NativeImportBinding seconds,date;
    bool running=false,busy=false;
    explicit State(IopHost& h,CalendarSource c):host(h),registry(h.nativeLibraries()),calendar(std::move(c)) {
        if (!calendar) calendar=utcCalendar;
    }
    bool initialized() const {
        std::array<uint8_t,0x58> b{}; uint32_t streamSema=0;
        return running && image && host.readIopMemory(image+0x1260,b.data(),b.size()) &&
            word(b)==1 && word(b,4)==1 && word(b,0x10)==0x105 && word(b,0x20)==image+0x1240 &&
            word(b,0x24)==0x105 && word(b,0x28)==1 &&
            word(b,0x30)==semas[0] && word(b,0x34)==semas[1] && word(b,0x38)==semas[2] &&
            !word(b,0x50) && !word(b,0x54) && // Read queue remains empty in this subset.
            host.readIopMemory(image+0x1430,&streamSema,4) && streamSema==semas[3];
    }
    NativeExportResult initializeJob(const std::array<uint32_t,4>& args) {
        std::lock_guard lock(mutex);
        if (busy || !initialized() || args[0]>UINT32_MAX-0x1c ||
            (uint64_t(args[0])<uint64_t(image)+ImageBytes && uint64_t(image)<uint64_t(args[0])+0x1c)) return {};
        struct Busy { bool& v; ~Busy() { v=false; } } operation{busy}; busy=true;
        std::array<uint8_t,0x1c> bytes{}; bytes[0x19]=1;
        const IopMemoryWrite w{args[0],bytes};
        return host.writeIopMemoryBatch(std::span(&w,1))?NativeExportResult{true,1}:NativeExportResult{};
    }
    std::optional<std::vector<uint8_t>> readSectors(const SectorJob& job) {
        const auto path=host.hostPath(HostPathKind::CdImage);
        if (path.empty()) return {};
        const auto file=host.openHostFile(path); if (!file) return {};
        struct Close { IopHost& host; uint64_t file; ~Close() { host.closeHostFile(file); } } close{host,file};
        uint64_t size=0; size_t read=0;
        const uint64_t offset=uint64_t(job.lsn)*0x800u;
        if (!host.hostFileSize(file,size) || size%0x800 || size<17*0x800 ||
            offset>size || job.bytes>size-offset) return {};
        std::array<uint8_t,72> pvd{};
        constexpr std::array<uint8_t,7> signature{1,'C','D','0','0','1',1};
        if (!host.readHostFile(file,16*0x800,pvd.data(),pvd.size(),read) || read!=pvd.size() ||
            !std::equal(signature.begin(),signature.end(),pvd.begin())) return {};
        // Original helper818 checks sector identity after a tray change. A local
        // ISO has no tray: verify configured identity on every opened handle.
        if (job.identityLsn) {
            const uint64_t identityOffset=uint64_t(job.identityLsn)*0x800u;
            std::array<uint8_t,0x800> identity{};
            if (identityOffset>size || identity.size()>size-identityOffset ||
                !host.readHostFile(file,identityOffset,identity.data(),identity.size(),read) || read!=identity.size()) return {};
            uint32_t checksum=0;
            for (uint32_t i=0;i<identity.size();++i) checksum+=(i+1u)*identity[i];
            if (checksum!=job.identityChecksum) return {};
        }
        std::vector<uint8_t> bytes(job.bytes);
        if (!host.readHostFile(file,offset,bytes.data(),bytes.size(),read) || read!=bytes.size()) return {};
        return bytes;
    }
    Reply readRequest(const Job& job) {
        const auto fail=[&](const char* reason)->Reply {
            if (std::getenv("GT3_BOOT_TRACE") && tracedRejections++<16)
                host.log(LogLevel::Warning,std::string("[GT3/PDICDVD] read rejected: ")+reason);
            return {};
        };
        const uint32_t requested=word(job.bytes,4),destination=word(job.bytes,8);
        // Flags0/1 have identical successful reads. Flag1's unbounded physical
        // media retry is not modeled: failed host reads remain negative.
        if (!requested || requested>0x20000 || word(job.bytes,12)>1) return fail("unsupported length/flags");
        uint32_t normalized=0;
        const uint32_t dmaBytes=(requested+15u)&~15u;
        if (!destination || !host.normalizeGuestAddress(destination,normalized) ||
            normalized>=0x2000000 || (normalized&15u) || dmaBytes>0x2000000-normalized) return fail("invalid EE destination");
        std::array<uint32_t,2> config{};
        if (!host.readIopMemory(image+0x129c,config.data(),sizeof(config))) return fail("disc identity snapshot failed");
        const auto allocate=registry->bind("libpdi",0x10a,11),release=registry->bind("libpdi",0x10a,14);
        if (!allocate.registration || allocate.registration!=seconds.registration || release.registration!=allocate.registration) return fail("allocator binding changed");
        const auto block=registry->invoke(allocate,{0x20000,0,0,0});
        if (!block.handled || block.value<=0) return fail("allocation failed");
        struct Owned {
            NativeLibraryRegistry& registry; NativeImportBinding release; uint32_t address;
            ~Owned() { if (address) { try { registry.invoke(release,{address,0,0,0}); } catch (...) {} } }
        } owned{*registry,release,uint32_t(block.value)};
        SectorJob read{word(job.bytes),(requested+0x7ffu)&~0x7ffu,config[0],config[1]};
        auto future=read.promise.get_future();
        {
            auto& signal=signals[1]; std::lock_guard lock(signal.mutex);
            if (sectorJob || signal.count) return fail("read worker busy");
            sectorJob=std::move(read); signal.count=1;
        }
        signals[1].changed.notify_one();
        auto data=future.get(); if (!data) return fail("ISO source read failed");
        const IopMemoryWrite stage{owned.address,*data};
        if (!host.writeIopMemoryBatch(std::span(&stage,1))) return fail("IOP staging failed");
        std::vector<uint8_t> verified(data->size());
        if (!host.readIopMemory(owned.address,verified.data(),verified.size()) || verified!=*data) return fail("IOP readback mismatch");
        // Snapshot the original fixed RPC reply span; only its status word changes.
        Reply reply{true,0x1380,std::vector<uint8_t>(0x40),true,destination,{}};
        if (!host.readIopMemory(image+0x1380,reply.bytes.data(),reply.bytes.size())) return fail("reply snapshot failed");
        put(reply.bytes,0,0);
        verified.resize(dmaBytes); reply.data=std::move(verified);
        if (!registry->invoke(release,{owned.address,0,0,0}).handled) return fail("allocation release failed");
        owned.address=0;
        return reply;
    }
    Reply execute(size_t queue,const Job& job) {
        if (!queue) {
            if (job.function==0) return {true,0x1270,{5,1,0,0},true};
            if (job.function==1) return {true,0x129c,std::vector<uint8_t>(4),false};
            if (job.function==2) return {true,0x129c,{job.bytes.begin(),job.bytes.begin()+8},false};
            if (job.function==3) return readRequest(job);
            return {};
        }
        if (job.function==0) return {true};
        if (job.function==1) {
            std::lock_guard streamLock(streamMutex);
            uint32_t stream=0;
            if (!host.readIopMemory(image+0x1434,&stream,4) || stream) return {};
            return {true}; // Audited idle cancellation, no current stream to cancel.
        }
        if (job.function!=2) return {};
        const auto now=registry->invoke(seconds,{}); if (!now.handled) return {};
        std::array<uint8_t,8> state{};
        if (!host.readIopMemory(image+0x12b8,state.data(),state.size())) return {};
        Reply r{true,0x12d0,std::vector<uint8_t>(8),true};
        put(r.bytes,0,word(state,4)+uint32_t(now.value)); put(r.bytes,4,word(state)); return r;
    }
    void stopWorkers() {
        for (auto& w:idleWorkers) w.request_stop();
        for (auto& signal:signals) signal.changed.notify_all();
        for (auto& q:queues) { q.worker.request_stop(); q.changed.notify_all(); }
        for (auto& w:idleWorkers) if (w.joinable()) w.join();
        for (auto& q:queues) { if (q.worker.joinable()) q.worker.join(); q.pending.reset(); }
    }
};
PdicdvdResident::PdicdvdResident(IopHost& h,CalendarSource c):state_(std::make_shared<State>(h,std::move(c))) {}
PdicdvdResident::~PdicdvdResident() { reset(); }
bool PdicdvdResident::active() const {
    auto& s=*state_;
    if (!s.registry) return false;
    const auto own=s.registry->bind("pdicdvd",0x105,13),dep=s.registry->bind("libpdi",0x10a,96);
    std::lock_guard lock(s.mutex);
    return !s.busy && own.registration && own.registration==s.registration && dep.registration==s.seconds.registration && s.initialized();
}
bool PdicdvdResident::start() {
    auto& s=*state_; if (currentWorker==&s) return false;
    std::lock_guard lifecycle(s.lifecycle);
    { std::lock_guard lock(s.mutex); if (s.running || s.busy || !s.registry) return false; s.busy=true; }
    struct Transaction {
        State& s; bool committed=false;
        ~Transaction() {
            if (!committed) {
                s.stopWorkers();
                if (s.registration) s.registry->unregisterLibrary(s.registration);
                if (s.image) s.host.freeIopMemory(s.image);
                for (auto stack:s.stacks) if (stack) s.host.freeIopMemory(stack);
                s.image=0; s.stacks={}; s.threads={}; s.semas={}; s.registration=0; s.running=false;
            }
            std::lock_guard lock(s.mutex); s.busy=false;
        }
    } transaction{s};
    auto bytes=verifiedImage(s.host); if (bytes.empty()) return false;
    s.seconds=s.registry->bind("libpdi",0x10a,96); s.date=s.registry->bind("libpdi",0x10a,98);
    if (!s.seconds.registration || s.seconds.registration!=s.date.registration) return false;
    const auto calendar=s.calendar(); if (!calendar) return false;
    const auto& c=*calendar; if (c[0]<2000 || c[0]>2099 || c[3]>23 || c[4]>59 || c[5]>59) return false;
    const auto days=s.registry->invoke(s.date,{c[0],c[1],c[2],0}),seconds=s.registry->invoke(s.seconds,{});
    if (!days.handled || !seconds.handled || days.value<0 || days.value>=36525) return false;
    s.image=s.host.allocateIopMemory(ImageBytes); if (!s.image || s.image>UINT32_MAX-ImageBytes) return false;
    for (size_t i=0;i<4;++i) {
        s.stacks[i]=s.host.allocateIopMemory(0x800); s.threads[i]=newHandle(); s.semas[i]=newHandle();
        if (!s.stacks[i] || s.stacks[i]>UINT32_MAX-0x800 || !s.threads[i] || !s.semas[i]) return false;
    }
    put(bytes,0x1264,1); put(bytes,0x1280,s.image+0x1240); put(bytes,0x1288,1);
    for (unsigned i=0;i<3;++i) put(bytes,0x1290+i*4,s.semas[i]);
    put(bytes,0x1430,s.semas[3]);
    put(bytes,0x12bc,uint32_t(days.value)*86400+c[3]*3600+c[4]*60+c[5]-uint32_t(seconds.value));
    put(bytes,0x12c0,uint32_t(seconds.value));
    const std::array<uint32_t,4> entries{0x8c4,0x994,0xdb8,0xeec},priorities{25,78,79,79};
    for (size_t i=0;i<4;++i) {
        // Native metadata beyond original1440 model; not executable IOP code or
        // a claim that Windows enforces original IOP scheduling priorities.
        const size_t at=0x1440+i*0x30;
        put(bytes,at,0x02000000); put(bytes,at+4,entries[i]); put(bytes,at+8,s.stacks[i]);
        put(bytes,at+12,0x800); put(bytes,at+16,priorities[i]); put(bytes,at+20,s.threads[i]);
        if (i>=2) { put(bytes,at+24,i==2?0x50434456:0x50636476); put(bytes,at+28,s.image+(i==2?0x1354:0x1404)); }
    }
    std::vector<uint8_t> stack(0x800); std::array<IopMemoryWrite,5> writes{}; writes[0]={s.image,bytes};
    for (size_t i=0;i<4;++i) writes[i+1]={s.stacks[i],stack};
    if (!s.host.writeIopMemoryBatch(writes)) return false;
    std::weak_ptr<State> weak=state_;
    s.registration=s.registry->registerLibrary({"pdicdvd",0x105,{{13,[weak](const auto& a) {
        const auto s=weak.lock(); return s?s->initializeJob(a):NativeExportResult{};
    }}}});
    if (!s.registration) return false;
    for (size_t i=0;i<2;++i) {
        std::promise<void> ready; auto initialized=ready.get_future();
        s.idleWorkers[i]=std::jthread([&s,i,ready=std::move(ready)](std::stop_token stop) mutable {
            currentWorker=&s; auto& signal=s.signals[i]; std::unique_lock lock(signal.mutex);
            ready.set_value();
            while (signal.changed.wait(lock,stop,[&] { return signal.count!=0; })) {
                signal.count=0;
                if (i!=1 || !s.sectorJob) continue;
                auto job=std::move(*s.sectorJob); s.sectorJob.reset(); lock.unlock();
                try { job.promise.set_value(s.readSectors(job)); }
                catch (...) { job.promise.set_exception(std::current_exception()); }
                lock.lock();
            }
            currentWorker=nullptr;
        }); initialized.get();
    }
    for (size_t i=0;i<2;++i) {
        std::promise<void> ready; auto initialized=ready.get_future(); auto& q=s.queues[i];
        q.worker=std::jthread([&s,&q,i,ready=std::move(ready)](std::stop_token stop) mutable {
            currentWorker=&s; std::unique_lock lock(q.mutex); ready.set_value();
            while (q.changed.wait(lock,stop,[&] { return q.pending.has_value(); })) {
                auto job=std::move(*q.pending); q.pending.reset(); lock.unlock();
                try { job.promise.set_value(s.execute(i,job)); } catch (...) { job.promise.set_exception(std::current_exception()); }
                lock.lock();
            }
            currentWorker=nullptr;
        }); initialized.get();
    }
    std::lock_guard lock(s.mutex); s.running=true;
    if (!s.initialized()) return false;
    transaction.committed=true; return true;
}
bool PdicdvdResident::reset() {
    auto& s=*state_; if (currentWorker==&s) return false;
    { std::lock_guard lock(s.mutex); if (s.busy) return false; }
    std::lock_guard lifecycle(s.lifecycle);
    { std::lock_guard lock(s.mutex); if (s.busy) return false; s.busy=true; s.running=false; }
    s.stopWorkers();
    if (s.registration) s.registry->unregisterLibrary(s.registration);
    if (s.image) s.host.freeIopMemory(s.image);
    for (auto stack:s.stacks) if (stack) s.host.freeIopMemory(stack);
    std::lock_guard lock(s.mutex); s.image=0; s.stacks={}; s.threads={}; s.semas={}; s.registration=0; s.busy=false; return true;
}
std::optional<std::vector<uint8_t>> PdicdvdResident::readStreamSectors(uint32_t lsn,uint32_t sectors) {
    auto& s=*state_;
    if (currentWorker==&s || !sectors || sectors>64 || uint64_t(lsn)+sectors>UINT32_MAX) return {};
    std::lock_guard lifecycle(s.lifecycle);
    if (!active()) return {};
    { std::lock_guard lock(s.mutex); if (s.busy || !s.initialized()) return {}; s.busy=true; }
    struct Busy { State& s; ~Busy() { std::lock_guard lock(s.mutex); s.busy=false; } } operation{s};
    std::array<uint32_t,2> identity{};
    if (!s.host.readIopMemory(s.image+0x129c,identity.data(),sizeof(identity))) return {};
    State::SectorJob job{lsn,sectors*2048,identity[0],identity[1]};
    auto future=job.promise.get_future();
    {
        auto& signal=s.signals[1]; std::lock_guard lock(signal.mutex);
        if (s.sectorJob || signal.count) return {};
        s.sectorJob=std::move(job); signal.count=1;
    }
    s.signals[1].changed.notify_one();
    return future.get();
}
RpcResult PdicdvdResident::handleRpc(const RpcRequest& request) {
    auto& s=*state_; if (currentWorker==&s) return rejected();
    std::lock_guard lifecycle(s.lifecycle);
    if (!active() || (request.sid!=0x50434456 && request.sid!=0x50636476)) return rejected();
    const size_t index=request.sid==0x50636476;
    const bool sectorRead=!index && request.function==3;
    const size_t replyBytes=sectorRead?4:!index && request.function==0?4:index && request.function==2?8:0;
    if (request.function>(sectorRead?3u:2u) || (replyBytes && (!request.receive.address || request.receive.size<replyBytes)) ||
        (!replyBytes && request.receive.size)) return rejected();
    State::Job job{request.function};
    const size_t inputBytes=sectorRead?16:!index && request.function==2?8:0;
    if (inputBytes && (request.send.size<inputBytes || !request.send.address ||
        !s.host.readGuest(request.send.address,job.bytes.data(),inputBytes))) return rejected();
    if (sectorRead) {
        uint32_t dest=0,receive=0;
        const uint32_t bytes=word(job.bytes,4);
        if (request.send.size!=16 || request.receive.size>0x40 || !bytes || bytes>0x20000 ||
            !s.host.normalizeGuestAddress(word(job.bytes,8),dest) || dest>=0x2000000 ||
            !s.host.normalizeGuestAddress(request.receive.address,receive) || receive>=0x2000000 ||
            request.receive.size>0x2000000-receive || ((bytes+15u)&~15u)>0x2000000-dest ||
            (uint64_t(dest)<uint64_t(receive)+request.receive.size && uint64_t(receive)<uint64_t(dest)+((bytes+15u)&~15u))) return rejected();
    }
    { std::lock_guard lock(s.mutex); if (s.busy || !s.initialized()) return rejected(); s.busy=true; }
    struct Busy { State& s; ~Busy() { std::lock_guard lock(s.mutex); s.busy=false; } } operation{s};
    const auto requestWords=job.bytes; // Keep diagnostics independent of the moved job.
    auto future=job.promise.get_future(); auto& q=s.queues[index];
    { std::lock_guard lock(q.mutex); q.pending=std::move(job); } q.changed.notify_all();
    auto reply=future.get(); if (!reply.handled) return rejected();
    // Snapshot inputs before applying any original control writes. The same
    // transaction commits time/version DMA to EE with the modeled IOP reply.
    std::vector<IopMemoryWrite> writes;
    if (reply.destination) writes.push_back({s.image+reply.destination,reply.bytes});
    if (reply.copyToEe) writes.push_back({request.receive.address,std::span<const uint8_t>(reply.bytes).first(sectorRead?request.receive.size:reply.bytes.size())});
    if (!reply.data.empty()) writes.push_back({reply.dataDestination,reply.data});
    if (!writes.empty() && !s.host.commitIopAndEeMemory(writes)) return rejected();
    if (sectorRead) {
        ++s.completedReads;
        if (s.completedReads<=16 || (s.completedReads&(s.completedReads-1))==0) {
            try {
                s.host.log(LogLevel::Info,"[GT3/PDICDVD] native ISO read committed; lsn="+std::to_string(word(requestWords))+
                    " bytes="+std::to_string(word(requestWords,4))+" flags="+std::to_string(word(requestWords,12))+" dma-bytes="+std::to_string(reply.data.size())+
                    " completed="+std::to_string(s.completedReads));
            } catch (...) {} // A diagnostic sink cannot invalidate committed data.
        }
    }
    RpcResult result; result.handled=true; result.resultAddress=reply.copyToEe?request.receive.address:0;
    result.serverDispatchPolicy=ServerDispatchPolicy::Suppress; return result;
}
}
