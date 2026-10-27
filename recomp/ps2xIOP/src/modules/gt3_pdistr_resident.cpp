#include "ps2x/iop/gt3/pdistr_resident.h"
#include "ps2x/iop/gt3/pdistr_startup_image.h"
#include "ps2x/iop/gt3/pdistr_workers.h"
#include "ps2x/iop/gt3/pdistr_disc_stream.h"
#include "ps2x/iop/gt3/pdistr_mpeg_video_job.h"
#include "ps2x/iop/gt3/pdistr_bgm_job.h"
#include "ps2x/iop/native_library.h"
#include <cstdio>
#include <map>
#include <mutex>
#include <thread>
#include <chrono>

namespace ps2x::iop::gt3 {
namespace {
RpcResult rejected() { RpcResult r; r.serverDispatchPolicy=ServerDispatchPolicy::RejectUnhandled; return r; }
bool eeSpan(uint32_t address,uint32_t size,uint32_t& physical) {
    // Do not accept the host's broader mirroring or modeled IOP heap as EE RAM.
    for (uint32_t base:{0u,0x20000000u,0x80000000u,0xa0000000u}) {
        if (address>=base && uint64_t(address)-base<0x2000000) {
            physical=address-base; return size<=0x2000000-physical;
        }
    }
    return false;
}
uint32_t word(std::span<const uint8_t> bytes,size_t at=0) {
    uint32_t result=0; for (unsigned i=0;i<4;++i) result|=uint32_t(bytes[at+i])<<(8*i); return result;
}
void put(std::span<uint8_t> bytes,size_t at,uint32_t value) {
    for (unsigned i=0;i<4;++i) bytes[at+i]=uint8_t(value>>(8*i));
}
RpcResult nullReply() {
    RpcResult r; r.handled=true; r.serverDispatchPolicy=ServerDispatchPolicy::Suppress; return r;
}
}
struct PdistrResident::State {
    IopHost& host; Pdispu2Resident& sound; PdicdvdResident& discDevice; NativeLibraryRegistry* registry;
    PdistrWorkers workers;
    std::map<uint32_t,std::unique_ptr<PdistrDiscStream>> streams;
    std::shared_ptr<PdistrMpegVideoJob> movie;
    uint64_t movieToken=0;
    std::array<uint8_t,0x38> movieControl{};
    std::array<uint8_t,4> sourceAllocation{},videoAllocation{};
    bool movieFault=false;
    mutable std::recursive_mutex mutex,lifecycle;
    NativeImportBinding libpdi,disc,spu;
    uint32_t image=0; uint64_t registration=0;
    bool busy=false,running=false;
    std::shared_ptr<PdistrBgmJob> bgm;
    State(IopHost& h,Pdispu2Resident& sd,PdicdvdResident& cd):host(h),sound(sd),discDevice(cd),registry(h.nativeLibraries()),workers(h),
        movie(std::make_shared<PdistrMpegVideoJob>(h,cd)),bgm(std::make_shared<PdistrBgmJob>(h,cd)) {
        workers.setBgmHandler([this](uint8_t command,std::span<const uint8_t> parameters) { return bgmCommand(command,parameters); });
    }
    bool setBgmActive(uint8_t value) {
        const std::array<uint8_t,1> bytes{value}; const std::array<IopMemoryWrite,1> writes{{{image+0x2af8,bytes}}};
        uint8_t observed=0xff;
        return host.writeIopMemoryBatch(writes) && host.readIopMemory(image+0x2af8,&observed,1) && observed==value;
    }
    // Runs on the PDISTR command worker (see PdistrWorkers::BgmHandler) while
    // the RPC caller waits holding the lifecycle lock; takes no resident lock.
    bool bgmCommand(uint8_t command,std::span<const uint8_t> p) {
        uint8_t active=0xff;
        if (!host.readIopMemory(image+0x2af8,&active,1)) return false;
        // 49c: only while 2af8 is set: clear it, cancel ring 2c30, export48 on
        // timing 2d90, join job 400 (which releases the timing and storage).
        if (active && (!bgm->stop() || !setBgmActive(0))) return false;
        if (command==4) return true;
        if (command!=1 || p.size()<128) return false;
        // 9d8 -> 4e8(lsn, sectors, loop byte, size, name). Supported: disc
        // source (empty name 2b90), single pass or looping (parser 100 repeats
        // the payload with an export43 period; the reader loops the file).
        const auto lsn=word(p,0),sectors=word(p,4),size=word(p,12)&~0x7ffu; const uint8_t loop=p[8];
        if (p[16]) {
            try { host.log(LogLevel::Warning,"[GT3/PDISTR] BGM start not supported natively: host file name request"); }
            catch (...) {}
            return false;
        }
        if (!setBgmActive(1)) return false;
        PdistrBgmJob::Route route;
        route.sound=&sound; route.registry=registry; route.state=image+0x2cf0; route.rawRing=image+0x2c30;
        route.secondaryRing=image+0x2d08; route.timing=image+0x2d90; route.descriptor=image+0x2dc8;
        route.pcmDescriptor=image+0x2e14; route.volumeReference=image+0x2c14;
        // 2c20 source kind 0 (disc), 2c24 buffer (native-owned; left 0), 2c28 sectors, 2c2c loop.
        std::array<uint8_t,13> fields{}; put(fields,8,sectors); fields[12]=loop;
        const std::array<IopMemoryWrite,1> record{{{image+0x2c20,fields}}};
        if (!host.writeIopMemoryBatch(record) || !bgm->start(route,lsn,sectors,size,loop!=0)) { bgm->stop(); setBgmActive(0); return false; }
        try { host.log(LogLevel::Info,"[GT3/PDISTR] BGM RPC1 accepted: lsn="+std::to_string(lsn)+" sectors="+
            std::to_string(sectors)+" buffer="+std::to_string(size)+(loop?" looping":"")+
            " (native disc source/parser/PDISPU2 job)"); } catch (...) {}
        return true;
    }
    bool initialized() const {
        std::array<uint32_t,7> identity{};
        return running && image && host.readIopMemory(image+0x29f0,identity.data(),sizeof(identity)) &&
            identity[0]==0x106 && identity[4]==image+0x29d0 && identity[5]==0x106 && identity[6]==1;
    }
    bool dependencies() const {
        if (!registry) return false;
        NativeImportBinding expectedPdi,expectedDisc,expectedSpu;
        { std::lock_guard lock(mutex); expectedPdi=libpdi; expectedDisc=disc; expectedSpu=spu; }
        const auto p=registry->bind("libpdi",0x10a,4),d=registry->bind("pdicdvd",0x105,13),s=registry->bind("pdispu2",0x109,4);
        // The registry precedes resident locks. Active sound verifies its own
        // owned LIBSD token, avoiding acceptance after sound-owner replacement.
        return p.registration && p.registration==expectedPdi.registration && d.registration && d.registration==expectedDisc.registration &&
            s.registration && s.registration==expectedSpu.registration && sound.active();
    }
    void release() {
        running=false;
        // No registry lock is held here: producers invoke LIBPDI while joining.
        movie->stop(); bgm->stop();
        { std::lock_guard lock(mutex); movieToken=0; movieFault=false; }
        for (auto& [context,stream]:streams) stream->close();
        streams.clear(); workers.stop();
        if (registration) registry->unregisterLibrary(registration);
        if (image) host.freeIopMemory(image);
        std::lock_guard lock(mutex); image=0; registration=0; libpdi={}; disc={}; spu={};
    }
    NativeExportResult cancelIdle() {
        if (!dependencies()) return {};
        std::lock_guard lock(mutex); if (busy || movieToken || movieFault || !initialized()) return {};
        busy=true; struct Finish { bool& flag; ~Finish() { flag=false; } } finish{busy};
        return workers.cancelIdle()?NativeExportResult{true,0}:NativeExportResult{};
    }
    bool movieValid() const {
        std::array<uint8_t,0x38> control{}; std::array<uint8_t,4> source{},video{};
        return movieToken && !movieFault &&
            host.readIopMemory(image+0x3088,control.data(),control.size()) && control==movieControl &&
            host.readIopMemory(image+0x3144,source.data(),source.size()) && source==sourceAllocation &&
            host.readIopMemory(image+0x345c,video.data(),video.size()) && video==videoAllocation;
    }
    bool stopMovie() {
        if (!movieToken || !movie->stop()) return false;
        constexpr std::array<uint8_t,1> zero{}; uint8_t active=1;
        const std::array<IopMemoryWrite,1> writes{{{image+0x3088,zero}}};
        if (!host.writeIopMemoryBatch(writes) || !host.readIopMemory(image+0x3088,&active,1) || active) {
            movieFault=true; return false;
        }
        std::lock_guard lock(mutex); movieToken=0; movieFault=false; return true;
    }
    RpcResult startMovie(std::span<const uint8_t> arguments) {
        const auto flags=word(arguments),lsn=word(arguments,4),sectors=word(arguments,8);
        // Flags0/2 (audio) use the native PDISTR audio branch and PDISPU2
        // movie-audio job. Reject before retiring an existing movie; an empty
        // name selects the verified disc path.
        if (flags>3 || arguments[12] || !sectors || sectors>0x1fffff ||
            uint64_t(lsn)+sectors>UINT32_MAX || movieFault) return rejected();
        if (movieToken && (!movieValid() || !stopMovie())) return rejected();
        uint8_t active=1;
        if (!host.readIopMemory(image+0x3088,&active,1) || active) return rejected();
        std::array<uint8_t,0x3d8> previous{}; // 3088..345f, including both embedded rings.
        if (!host.readIopMemory(image+0x3088,previous.data(),previous.size())) return rejected();
        struct Rollback {
            State& s; const std::array<uint8_t,0x3d8>& previous; bool committed=false;
            ~Rollback() {
                if (committed) return;
                s.movie->stop();
                const std::array<IopMemoryWrite,1> writes{{{s.image+0x3088,previous}}};
                std::array<uint8_t,0x3d8> observed{};
                bool restored=false;
                try { restored=s.host.writeIopMemoryBatch(writes) &&
                    s.host.readIopMemory(s.image+0x3088,observed.data(),observed.size()) && observed==previous; } catch (...) {}
                std::lock_guard lock(s.mutex); s.movieToken=0; s.movieFault=!restored;
            }
        } transaction{*this,previous};
        // Original f9c/d0c audio objects inside the PDISTR image (state 2ec0):
        // raw ring 3350, secondary ring 3208, timing 3290, ADPCM descriptor
        // 32c8, PCM descriptor 3314; volume reference 2c14 from startup.
        PdistrMpegVideoJob::AudioRoute route;
        route.sound=&sound; route.registry=registry; route.rawRing=image+0x3350; route.secondaryRing=image+0x3208;
        route.timing=image+0x3290; route.descriptor=image+0x32c8; route.pcmDescriptor=image+0x3314;
        route.volumeReference=image+0x2c14;
        const bool withAudio=!(flags&1);
        if (withAudio && !sound.active()) return rejected();
        const auto token=movie->start(flags,lsn,sectors,image+0x30c0,image+0x33d8,withAudio?&route:nullptr);
        if (!token) return rejected();
        movieControl={}; movieControl[0]=1; movieControl[0xc]=uint8_t(flags&1); movieControl[0xd]=uint8_t((flags>>1)&1);
        put(movieControl,0x10,sectors); put(movieControl,0x14,image+0x30c0);
        put(movieControl,0x24,image+0x30c0); put(movieControl,0x28,2);
        put(movieControl,0x2c,lsn); put(movieControl,0x30,sectors); put(movieControl,0x34,flags>>1);
        // Native owners replace the two PS2 job handles; these fields stay zero.
        if (!host.readIopMemory(image+0x30cc,sourceAllocation.data(),4) ||
            !host.readIopMemory(image+0x33e4,videoAllocation.data(),4)) return rejected();
        const std::array<IopMemoryWrite,3> writes{{{image+0x3088,movieControl},
            {image+0x3144,sourceAllocation},{image+0x345c,videoAllocation}}};
        if (!host.writeIopMemoryBatch(writes)) return rejected();
        { std::lock_guard lock(mutex); movieToken=token; }
        if (!movieValid()) return rejected();
        transaction.committed=true;
        try { host.log(LogLevel::Info,"[GT3/PDISTR] MPEG RPC1 accepted: flags="+std::to_string(flags)+
            (withAudio?" (audio: native parser/PDISPU2 movie-audio job)":" (video only)")+
            " lsn="+std::to_string(lsn)+" sectors="+std::to_string(sectors)); } catch (...) {}
        return nullReply();
    }
    RpcResult streamRpc(const RpcRequest& request) {
        const bool opening=request.function==2,consuming=request.function==3,soundCopy=request.function==4;
        const uint32_t minimum=opening?128:(consuming || soundCopy)?12:4;
        uint32_t send=0,receive=0;
        if (!request.send.address || (request.send.address&3) || request.send.size<minimum || request.send.size>128 ||
            !eeSpan(request.send.address,request.send.size,send) || request.receive.size>64 ||
            (request.receive.size && (!request.receive.address || !eeSpan(request.receive.address,request.receive.size,receive))) ||
            (opening && (request.receive.size<4 || (request.receive.address&3)))) return rejected();
        std::array<uint8_t,128> bytes{};
        if (!host.readGuest(send,bytes.data(),request.send.size)) return rejected();
        RpcResult result; result.handled=true; result.serverDispatchPolicy=ServerDispatchPolicy::Suppress;
        if (opening) {
            if (bytes[20]) return rejected(); // Original alternate filename path is unsupported.
            std::array<uint8_t,64> reply{};
            if (!host.readIopMemory(image+0x3540,reply.data(),reply.size())) return rejected();
            auto stream=std::make_unique<PdistrDiscStream>(host,discDevice);
            if (!stream->open(word(bytes),word(bytes,4),word(bytes,8))) return rejected();
            const auto context=stream->context();
            for (unsigned i=0;i<4;++i) reply[i]=uint8_t(context>>(8*i));
            // Establish host ownership before publishing. Erasing on a failed
            // commit closes/joins the otherwise unpublished producer.
            auto [entry,inserted]=streams.emplace(context,std::move(stream));
            if (!inserted) return rejected();
            struct Rollback {
                State& s; uint32_t context; bool committed=false;
                ~Rollback() { if (!committed) s.streams.erase(context); }
            } transaction{*this,context};
            const std::array<IopMemoryWrite,2> writes{{
                {image+0x3540,std::span<const uint8_t>(reply).first(4)},
                {receive,std::span<const uint8_t>(reply).first(request.receive.size)}
            }};
            if (!host.commitIopAndEeMemory(writes)) return rejected();
            transaction.committed=true; result.resultAddress=request.receive.address;
            try { host.log(LogLevel::Info,"[GT3/PDISTR] disc stream opened; context="+std::to_string(context)+
                " lsn="+std::to_string(word(bytes))+" sectors="+std::to_string(word(bytes,4))); } catch (...) {}
            return result;
        }
        const auto found=streams.find(word(bytes));
        if (found==streams.end()) return rejected();
        if (soundCopy) {
            if (!found->second->consumeSound(sound,word(bytes,4),word(bytes,8))) return rejected();
            try { host.log(LogLevel::Info,"[GT3/PDISTR] sound data copied and completed; context="+std::to_string(word(bytes))+
                " sound-address="+std::to_string(word(bytes,4))+" bytes="+std::to_string(word(bytes,8))); } catch (...) {}
        } else if (consuming) {
            const auto size=word(bytes,8); uint32_t destination=0;
            if (size) {
                const uint64_t dma=(uint64_t(size)+15)&~uint64_t(15);
                if (dma>0x2000000 || !eeSpan(word(bytes,4),uint32_t(dma),destination) ||
                    (request.receive.size && uint64_t(destination)<uint64_t(receive)+request.receive.size &&
                        uint64_t(receive)<uint64_t(destination)+dma)) return rejected();
            }
            if (!found->second->consume(word(bytes,4),size)) return rejected();
            try {
                char destinationText[16]; std::snprintf(destinationText,sizeof(destinationText),"0x%x",word(bytes,4));
                host.log(LogLevel::Info,"[GT3/PDISTR] stream data copied; context="+std::to_string(word(bytes))+
                " bytes="+std::to_string(size)+" to="+destinationText); } catch (...) {}
        } else {
            if (!found->second->close()) return rejected();
            streams.erase(found);
            try { host.log(LogLevel::Info,"[GT3/PDISTR] disc stream closed; context="+std::to_string(word(bytes))); } catch (...) {}
        }
        return result; // Original consume/close return null: no receive DMA.
    }
};
PdistrResident::PdistrResident(IopHost& host,Pdispu2Resident& sound,PdicdvdResident& disc):state_(std::make_shared<State>(host,sound,disc)) {}
PdistrResident::~PdistrResident() { reset(); }
bool PdistrResident::active() const {
    auto& s=*state_; if (!s.registry) return false;
    { std::lock_guard lock(s.mutex); if (s.busy || !s.running) return false; }
    const auto own=s.registry->bind("pdistr",0x106,8);
    const bool ready=s.dependencies() && s.workers.active();
    std::lock_guard lock(s.mutex); return !s.busy && own.registration && own.registration==s.registration && ready && s.initialized();
}
bool PdistrResident::start() {
    auto& s=*state_;
    { std::lock_guard lock(s.mutex); if (s.busy) return false; }
    std::lock_guard lifecycle(s.lifecycle);
    { std::lock_guard lock(s.mutex); if (s.busy || s.running || !s.registry) return false; s.busy=true; }
    struct Transaction {
        State& s; bool committed=false;
        ~Transaction() { if (!committed) s.release(); std::lock_guard lock(s.mutex); s.busy=false; }
    } transaction{s};
    const auto libpdi=s.registry->bind("libpdi",0x10a,4),disc=s.registry->bind("pdicdvd",0x105,13),spu=s.registry->bind("pdispu2",0x109,4);
    { std::lock_guard lock(s.mutex); s.libpdi=libpdi; s.disc=disc; s.spu=spu; }
    if (!s.dependencies() || !s.discDevice.active()) return false;
    s.image=s.host.allocateIopMemory(PdistrWorkers::ModelBytes); if (!s.image) return false;
    std::weak_ptr<State> weak=state_;
    s.registration=s.registry->registerLibrary({"pdistr",0x106,{{8,[weak](const auto&) {
        const auto state=weak.lock(); return state?state->cancelIdle():NativeExportResult{};
    }}}});
    if (!s.registration || !initializePdistrImage(s.host,s.image) || !s.workers.start(s.image)) return false;
    constexpr std::array<uint8_t,4> one{1,0,0,0};
    const std::array<IopMemoryWrite,1> flag{{{s.image+0x2a08,one}}};
    if (!s.host.writeIopMemoryBatch(flag)) return false;
    s.running=true;
    if (!s.initialized() || !s.workers.active() || !s.dependencies()) return false;
    transaction.committed=true; return true;
}
bool PdistrResident::reset() {
    auto& s=*state_;
    { std::lock_guard lock(s.mutex); if (s.busy) return false; }
    std::lock_guard lifecycle(s.lifecycle);
    { std::lock_guard lock(s.mutex); if (s.busy) return false; s.busy=true; }
    s.release(); std::lock_guard lock(s.mutex); s.busy=false; return true;
}
RpcResult PdistrResident::handleRpc(const RpcRequest& request) {
    auto& s=*state_;
    // Like the original handlers' semaphore 3084, concurrent RPCs from the EE's
    // servers (e.g. RPC3 ranges while RPC2 reads) wait for the lifecycle lock
    // instead of being rejected. A same-thread reentry still sees busy below.
    std::unique_lock lifecycle(s.lifecycle);
    // The dependency check reads other residents' transient busy windows (e.g.
    // PDISPU2 while the audio parser starts its job, or a sound export in
    // flight). Allow them to settle rather than reject a concurrent RPC.
    auto settledActive=[&] {
        for (unsigned tries=0;tries<200;++tries) {
            if (active()) return true;
            { std::lock_guard lock(s.mutex); if (!s.running) return false; }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        return false;
    };
    const bool mpeg=request.sid==0x4d504731 || request.sid==0x4d504732;
    const bool movieStart=mpeg && request.function==1,movieRead=mpeg && request.function==2;
    const bool known=request.sid==0x5042474d || mpeg || request.sid==0x50444950;
    const bool cancel=request.function==4 && (request.sid==0x4d504731 || request.sid==0x4d504732);
    const bool idleMpegControl=request.function==3 && (request.sid==0x4d504731 || request.sid==0x4d504732);
    const bool bgmVolume=request.function==3 && request.sid==0x5042474d;
    const bool bgmStatus=request.function==6 && request.sid==0x5042474d;
    // a2c/af8 and b08 enqueue BGM commands 2/3 (pause/resume) without reading
    // the send buffer and return a null result.
    const bool bgmPause=(request.function==4 || request.function==5) && request.sid==0x5042474d;
    // ab8 enqueues command 4 (stop) the same way.
    const bool bgmStop=request.function==2 && request.sid==0x5042474d;
    // a70: start; copies the full 128-byte request buffer to 2afc.
    const bool bgmStart=request.function==1 && request.sid==0x5042474d;
    const bool stream=request.sid==0x50444950 && request.function>=1 && request.function<=4;
    if (!known || (request.function!=0 && !cancel && !idleMpegControl && !stream && !bgmVolume && !bgmStatus && !bgmPause && !bgmStop && !bgmStart && !movieStart && !movieRead) || !settledActive()) return rejected();
    std::array<uint8_t,128> movieArguments{};
    if (movieStart || movieRead) {
        uint32_t send=0;
        if (!request.send.address || (request.send.address&3) || request.send.size<(movieStart?16u:8u) ||
            request.send.size>128 || !eeSpan(request.send.address,request.send.size,send) ||
            !s.host.readGuest(send,movieArguments.data(),request.send.size)) return rejected();
    }
    if (movieRead) {
        const auto destination=word(movieArguments),bytes=word(movieArguments,4); uint32_t physical=0;
        if (!s.movieValid() || bytes>0x8000 || bytes%4096 || (destination&15) ||
            !eeSpan(destination,bytes,physical) || !physical) return rejected();
        const auto owner=s.movie; const auto token=s.movieToken;
        // Stop/reset must enter while the reader waits. This retained owner and
        // token are all the reader needs; never touch the resident image again.
        lifecycle.unlock();
        const auto transfer=owner->read(token,destination,bytes);
        using Result=PdistrMpegRing::TransferResult;
        return transfer.result==Result::Complete || transfer.result==Result::End?nullReply():rejected();
    }
    std::array<uint8_t,128> bgmParameters{};
    if (bgmStart) {
        uint32_t send=0;
        if (!request.send.address || (request.send.address&3) || request.send.size!=128 ||
            !eeSpan(request.send.address,request.send.size,send) || !s.host.readGuest(send,bgmParameters.data(),128)) return rejected();
    }
    uint32_t volume=0;
    if (bgmVolume) {
        uint32_t send=0;
        if (!request.send.address || (request.send.address&3) || request.send.size<4 || request.send.size>128 ||
            !eeSpan(request.send.address,request.send.size,send) || !s.host.readGuest(send,&volume,4)) return rejected();
    }
    std::array<uint8_t,16> arguments{};
    if (idleMpegControl) {
        // Original127c loads all four words before helper119c tests activity.
        // An idle null result performs no receive DMA or model-memory write.
        uint32_t send=0;
        if (!request.send.address || (request.send.address&3) || request.send.size<arguments.size() ||
            request.send.size>128 || !eeSpan(request.send.address,request.send.size,send) ||
            !s.host.readGuest(send,arguments.data(),arguments.size())) return rejected();
    }
    if (!stream && request.sid==0x50444950 && (!request.receive.address || request.receive.size<4)) return rejected();
    { std::lock_guard lock(s.mutex); if (s.busy) return rejected(); s.busy=true; }
    struct Finish { State& s; ~Finish() { std::lock_guard lock(s.mutex); s.busy=false; } } finish{s};
    if (bgmStatus) {
        // Original a2c/RPC6 (b18): clear the 24-byte status at 2ea0; only while
        // BGM plays (2e75, set by BGM start and never cleared) fill position/
        // length/finished. The reply is the 64 bytes at 2ea0 (receive DMA of
        // the returned buffer).
        uint8_t playing=1;
        if (!s.host.readIopMemory(s.image+0x2e75,&playing,1) || request.receive.size>64 ||
            (request.receive.size && !request.receive.address)) return rejected();
        std::array<uint8_t,24> status{};
        if (playing) {
            // b58..bcc: from timing 2d90 (+18/+1c position, +10 consumed, +14
            // wraps) and state 2cf0 (+10/+14 total samples): {+18, +1c,
            // +10 sign-extended to 64 bits, +14, finished = consumed >= total}.
            std::array<uint32_t,4> timing{}; std::array<uint32_t,2> total{};
            if (!s.host.readIopMemory(s.image+0x2da0,timing.data(),sizeof(timing)) ||
                !s.host.readIopMemory(s.image+0x2d00,total.data(),sizeof(total))) return rejected();
            const int64_t consumed=int32_t(timing[0]),samples=int64_t((uint64_t(total[1])<<32)|total[0]);
            put(status,0,timing[2]); put(status,4,timing[3]);
            put(status,8,timing[0]); put(status,12,uint32_t(consumed>>32)); put(status,16,timing[1]);
            put(status,20,consumed>=samples?1u:0u);
        }
        const std::array<IopMemoryWrite,1> clear{{{s.image+0x2ea0,status}}};
        if (!s.host.writeIopMemoryBatch(clear)) return rejected();
        RpcResult result; result.handled=true; result.serverDispatchPolicy=ServerDispatchPolicy::Suppress;
        if (!request.receive.size) return result;
        std::vector<uint8_t> reply(request.receive.size);
        if (!s.host.readIopMemory(s.image+0x2ea0,reply.data(),reply.size())) return rejected();
        const std::array<IopMemoryWrite,1> writes{{{request.receive.address,reply}}};
        if (!s.host.commitIopAndEeMemory(writes)) return rejected();
        result.resultAddress=request.receive.address;
        return result;
    }
    if (movieStart) return s.startMovie(movieArguments);
    if (cancel && s.movieToken) return s.stopMovie()?nullReply():rejected();
    if (idleMpegControl && s.movieToken) {
        if (!s.movieValid()) return rejected();
        // 119c: active and audio enabled (3094==0) -> export44 on timing 3290.
        if (!s.movieControl[0xc] && !s.movie->updateAudioRange(s.movieToken,
                {word(arguments),word(arguments,4),word(arguments,8),word(arguments,12)})) return rejected();
        return nullReply();
    }
    if (mpeg && s.movieFault) return rejected();
    if (stream) return s.streamRpc(request);
    const auto reply=cancel?s.workers.queryIdleCancel(request.sid):
        idleMpegControl?s.workers.queryIdleMpegControl(request.sid):
        bgmVolume?s.workers.queryBgmVolume(volume,s.sound):
        bgmPause?s.workers.queryBgmPause(request.function==4):
        bgmStop?s.workers.queryBgmStop():
        bgmStart?s.workers.queryBgmStart(bgmParameters):s.workers.queryInitial(request.sid);
    if (!reply) return rejected();
    RpcResult result; result.handled=true; result.serverDispatchPolicy=ServerDispatchPolicy::Suppress;
    if (reply->hasData) {
        std::array<uint8_t,4> bytes{}; for (unsigned i=0;i<4;++i) bytes[i]=uint8_t(reply->value>>(i*8));
        const std::array<IopMemoryWrite,1> writes{{{request.receive.address,bytes}}};
        if (!s.host.commitIopAndEeMemory(writes)) return rejected();
        result.resultAddress=request.receive.address;
    }
    return result; // Original null-result paths leave the caller's receive bytes intact.
}
}
