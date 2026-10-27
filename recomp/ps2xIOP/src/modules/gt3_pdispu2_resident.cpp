#include "ps2x/iop/gt3/pdispu2_resident.h"
#include "ps2x/iop/gt3/pdispu2_sound_setup.h"
#include "ps2x/iop/gt3/pdispu2_workers.h"
#include "ps2x/iop/gt3/pdispu2_stream_control.h"
#include <algorithm>
#include <cstdio>
#include <map>
#include <mutex>
#include <thread>
#include <chrono>

namespace ps2x::iop::gt3 {
namespace {
void put(std::span<uint8_t> bytes,size_t offset,uint32_t value) {
    for (unsigned i=0;i<4;++i) bytes[offset+i]=uint8_t(value>>(i*8));
}
uint32_t word(std::span<const uint8_t> bytes,size_t offset=0) {
    uint32_t value=0; for (unsigned i=0;i<4;++i) value|=uint32_t(bytes[offset+i])<<(i*8); return value;
}
RpcResult rejected() { RpcResult result; result.serverDispatchPolicy=ServerDispatchPolicy::RejectUnhandled; return result; }
}
struct Pdispu2Resident::State {
    IopHost& host; LibsdResident& sound; NativeLibraryRegistry* registry;
    Pdispu2Workers workers;
    std::map<uint32_t,std::shared_ptr<Pdispu2StreamControl>> controls;
    mutable std::recursive_mutex mutex,lifecycle;
    uint32_t image=0;
    uint64_t registration=0,soundToken=0;
    NativeImportBinding libpdi;
    std::shared_ptr<Pdispu2MovieAudio> movie;
    bool running=false,busy=false;
    std::array<bool,2> loggedVoiceCommands{};
    bool loggedEffect=false;
    State(IopHost& h,LibsdResident& sd):host(h),sound(sd),registry(h.nativeLibraries()),workers(h) {}
    bool valid() const {
        std::array<uint8_t,0x3c> bytes{};
        return running && image && host.readIopMemory(image+0x3790,bytes.data(),bytes.size()) &&
            word(bytes)==0x109 && word(bytes,4)==0x53505550 && word(bytes,8)==0x53505556 &&
            word(bytes,0x20)==1 && word(bytes,0x30)==image+0x3670 && word(bytes,0x34)==0x109 && word(bytes,0x38)==1;
    }
    bool release(bool force=false) {
        running=false;
        if (movie) { movie->stop(); movie.reset(); } // Joins its worker before controls stop.
        for (auto& [address,control]:controls) control->stop();
        if (!workers.stop()) return false;
        controls.clear();
        if (soundToken && sound.ownsSoundStartup(soundToken)) {
            bool restored=false;
            try { restored=sound.restoreSoundStartup(soundToken); } catch (...) {}
            if (!restored && (!force || !sound.abandonSoundStartup(soundToken))) return false;
        }
        soundToken=0;
        if (registration) registry->unregisterLibrary(registration);
        if (image) host.freeIopMemory(image);
        registration=0; image=0; libpdi={}; return true;
    }
    NativeExportResult initializeVolume(const std::array<uint32_t,4>& args) {
        std::lock_guard lock(mutex);
        if (busy || !valid() || (args[0]&3) || args[0]>UINT32_MAX-20 ||
            (uint64_t(args[0])<uint64_t(image)+Pdispu2Workers::ModelBytes && uint64_t(image)<uint64_t(args[0])+20)) return {};
        busy=true; struct Finish { bool& flag; ~Finish() { flag=false; } } finish{busy};
        std::array<uint8_t,20> bytes{}; for (unsigned offset=0;offset<16;offset+=4) put(bytes,offset,0x8000);
        const std::array<IopMemoryWrite,1> writes{{{args[0],bytes}}};
        return host.writeIopMemoryBatch(writes)?NativeExportResult{true,0}:NativeExportResult{};
    }
    NativeExportResult interrupt(const std::array<uint32_t,4>& args) {
        // Original 9d8: per core in the IRQ mask, call the table callback, then
        // ac4 (IRQ enable 0 -> 1). Runs on the sound output thread with LIBSD
        // delivery in progress; no resident lock is held while calling out.
        std::shared_ptr<Pdispu2MovieAudio> job; std::array<uint32_t,4> table{};
        {
            std::lock_guard lock(mutex);
            if (!valid() || !sound.ownsSoundStartup(soundToken) || args[1]!=image+0x3830 || !args[0] || (args[0]&~3u) ||
                !host.readIopMemory(image+0x3830,table.data(),sizeof(table))) return {};
            job=movie;
            if (table==std::array<uint32_t,4>{}) {
                if (busy) return {};
                busy=true; struct Finish { bool& flag; ~Finish() { flag=false; } } finish{busy};
                return pdispu2InitialSoundInterrupt(host,image,args);
            }
        }
        for (unsigned core=0;core<2;++core) if (args[0]&(1u<<core)) {
            const auto callback=table[core*2],argument=table[core*2+1];
            if (callback) {
                if (callback!=image+0x304 || !job || argument!=job->descriptor() || !job->interrupt()) return {};
            }
            if (!sound.invoke(11,{4|core,0,0,0}).handled || !sound.invoke(11,{4|core,1,0,0}).handled) return {};
        }
        return {true,0};
    }
    NativeExportResult setVolume(const std::array<uint32_t,4>& args) {
        // Movie-audio volume reference {volume, 424, job}: write then run 424,
        // with the resident lock released before LIBSD is entered.
        std::shared_ptr<Pdispu2MovieAudio> job;
        {
            std::lock_guard lock(mutex);
            std::array<uint32_t,3> reference{};
            if (movie && args[0] && !(args[0]&3) && host.readIopMemory(args[0],reference.data(),sizeof(reference)) &&
                reference[1]==image+0x424 && reference[2]==movie->descriptor()) {
                if (busy || !valid() || !reference[0] || (reference[0]&1)) return {};
                const std::array<uint8_t,2> value{uint8_t(args[1]),uint8_t(args[1]>>8)};
                const std::array<IopMemoryWrite,1> writes{{{reference[0],value}}};
                if (!host.writeIopMemoryBatch(writes)) return {};
                job=movie;
            }
        }
        if (job) return job->applyVolume()?NativeExportResult{true,0}:NativeExportResult{};
        std::lock_guard lock(mutex);
        if (busy || !valid() || !sound.ownsSoundStartup(soundToken)) return {};
        busy=true; struct Finish { bool& flag; ~Finish() { flag=false; } } finish{busy};
        if (!args[0]) return {true,0};
        auto overlaps=[](uint32_t a,uint32_t n,uint32_t b,uint32_t m) {
            return uint64_t(a)<uint64_t(b)+m && uint64_t(b)<uint64_t(a)+n;
        };
        std::array<uint8_t,12> reference{};
        if ((args[0]&3) || args[0]>UINT32_MAX-12 ||
            overlaps(args[0],12,image,Pdispu2Workers::ModelBytes) ||
            !host.readIopMemory(args[0],reference.data(),4)) return {};
        const auto target=word(reference);
        if (!target) return {true,0};
        if ((target&1) || target>UINT32_MAX-2 || overlaps(target,2,args[0],12) ||
            overlaps(target,2,image,Pdispu2Workers::ModelBytes) ||
            !host.readIopMemory(args[0],reference.data(),reference.size()) || word(reference,4)!=0) return {};
        const std::array<uint8_t,2> value{uint8_t(args[1]),uint8_t(args[1]>>8)};
        const std::array<IopMemoryWrite,1> writes{{{target,value}}};
        if (!host.writeIopMemoryBatch(writes)) return {};
        std::array<uint8_t,2> observed{};
        return host.readIopMemory(target,observed.data(),observed.size()) && observed==value?
            NativeExportResult{true,0}:NativeExportResult{};
    }
    NativeExportResult initializeStreamDescriptor(uint16_t ordinal,const std::array<uint32_t,4>& args) {
        std::lock_guard lock(mutex);
        if (busy || !valid() || !sound.ownsSoundStartup(soundToken)) return {};
        busy=true; struct Finish { bool& flag; ~Finish() { flag=false; } } finish{busy};
        const uint32_t size=ordinal==52?0x4c:0x30;
        if (!args[0] || (args[0]&3) || args[0]>UINT32_MAX-size ||
            (uint64_t(args[0])<uint64_t(image)+Pdispu2Workers::ModelBytes && uint64_t(image)<uint64_t(args[0])+size)) return {};
        std::vector<uint8_t> bytes(size);
        put(bytes,0x20,ordinal==52?1:3);
        if (ordinal==52) {
            put(bytes,0x24,22); put(bytes,0x30,4096); put(bytes,0x34,0x1d8000); put(bytes,0x38,0x1f8000);
        }
        const std::array<IopMemoryWrite,1> writes{{{args[0],bytes}}};
        if (!host.writeIopMemoryBatch(writes)) return {};
        std::vector<uint8_t> observed(size);
        return host.readIopMemory(args[0],observed.data(),observed.size()) && observed==bytes?
            NativeExportResult{true,0}:NativeExportResult{};
    }
    NativeExportResult initializeStreamControl(const std::array<uint32_t,4>& args) {
        std::lock_guard lock(mutex);
        const uint32_t address=args[0];
        if (busy || !valid() || !sound.ownsSoundStartup(soundToken) ||
            !address || (address&3) || address>UINT32_MAX-0x38 ||
            (uint64_t(address)<uint64_t(image)+Pdispu2Workers::ModelBytes && uint64_t(image)<uint64_t(address)+0x38)) return {};
        for (const auto& [existing,control]:controls)
            if (uint64_t(address)<uint64_t(existing)+0x38 && uint64_t(existing)<uint64_t(address)+0x38) return {};
        busy=true; struct Finish { bool& b; ~Finish() { b=false; } } finish{busy};
        // Establish ownership before publishing the external descriptor, so a
        // host allocation exception cannot leave an unowned successful resource.
        auto [entry,inserted]=controls.emplace(address,std::make_shared<Pdispu2StreamControl>(host));
        if (!inserted) return {};
        struct Rollback {
            State& s; uint32_t address; bool done=false;
            ~Rollback() { if (!done) s.controls.erase(address); }
        } transaction{*this,address};
        if (!entry->second->initialize(address)) return {};
        transaction.done=true; return {true,0};
    }
    // Original 26e0 (export50; export51 wraps it): 2604 resume (clear the
    // pause byte +34 and waiter +30), range words +20..+2c = -1, handle +0 = 0,
    // then delete the semaphore. Natively the control is stopped (waking any
    // waiter) and dropped. Refused while the movie-audio job still uses it.
    bool releaseStreamControl(uint32_t address) {
        std::lock_guard lock(mutex);
        if (busy || !valid() || !sound.ownsSoundStartup(soundToken)) return false;
        const auto found=controls.find(address); if (found==controls.end()) return false;
        if (movie && !movie->snapshot().finished) {
            uint32_t timing=0;
            if (!host.readIopMemory(movie->descriptor()+0x1c,&timing,4) || timing==address) return false;
        }
        busy=true; struct Finish { bool& b; ~Finish() { b=false; } } finish{busy};
        found->second->stop(); controls.erase(found);
        std::array<uint8_t,0x35> bytes{};
        if (!host.readIopMemory(address,bytes.data(),bytes.size())) return false;
        put(bytes,0,0); for (size_t at=0x20;at<=0x2c;at+=4) put(bytes,at,0xffffffff);
        put(bytes,0x30,0); bytes[0x34]=0;
        const std::array<IopMemoryWrite,1> writes{{{address,bytes}}};
        return host.writeIopMemoryBatch(writes);
    }
    // Original PDISPU2 RPC4 (handler 10a4) and RPC3 (f6c): the game's
    // per-voice sound-effect command batch and voice-status query.
    //
    // The 848-byte request is the server buffer (0x3930 for SID 53505550,
    // 0x3ce0 for 53505556): 48 voice records of 16 bytes (core0 voices 0..23,
    // then core1), two 0x24-byte core records at +0x300 and an effect flag at
    // +0x34c. Voice record: +0 flags, +2 pitch, +4/+6 volume L/R, +8 start
    // address, +0xc/+0xe ADSR1/2. Flags: bit0 key off, bit1 key on, bits2..7
    // PMON/NON/VMIXL/VMIXEL/VMIXR/VMIXER, bit8 release-at-ENDX, 0x200 pitch,
    // 0x400 volume, 0x800 start address+ADSR.
    // Per-core state at 0x3870 (+0x2c per core): the eight bit masks, then
    // +0x20 active, +0x24 releasing, +0x28 end-watch. Key switches are queued
    // in the 3850 block for the e24 worker; natively the worker step runs
    // synchronously where the original yields to it (DelayThread 100 us).
    // The reply is 64 bytes from 0x38c8: the 48-bit active-voice mask
    // (bits 0..23 core0, 24..47 core1) followed by server data.
    RpcResult voiceCommands(const RpcRequest& request,bool apply) {
        const uint32_t buffer=request.sid==0x53505550?0x3930:0x3ce0;
        auto invoke=[&](uint16_t ordinal,uint32_t selector,uint32_t value=0) -> std::optional<int32_t> {
            // LIBSD refuses reads (ENVX/ENDX) while it delivers a sound IRQ
            // to the movie-audio handler; that window is short, so retry.
            for (unsigned tries=0;;++tries) {
                const auto result=sound.invoke(ordinal,{selector,value,0,0});
                if (result.handled) return result.value;
                if ((ordinal!=6 && ordinal!=8) || tries>=20) return std::nullopt;
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        };
        std::array<uint8_t,0x58> voices{};
        if (!host.readIopMemory(image+0x3870,voices.data(),voices.size())) return rejected();
        auto field=[&](unsigned core,unsigned offset) { return word(voices,core*0x2c+offset); };
        auto setField=[&](unsigned core,unsigned offset,uint32_t value) { put(voices,core*0x2c+offset,value); };
        std::array<uint8_t,0x14> pending{}; // 3850: worker semaphore, KON core0/1, KOFF core0/1
        if (!host.readIopMemory(image+0x3850,pending.data(),pending.size())) return rejected();
        // e24 worker body: KOFF(pending off & ~pending on), then KON.
        auto runKeyWorker=[&]() -> bool {
            const uint32_t on0=word(pending,4),on1=word(pending,8);
            const uint32_t off0=word(pending,0xc)&~on0,off1=word(pending,0x10)&~on1;
            for (unsigned at=4;at<0x10+4;at+=4) put(pending,at,0);
            if (off0 && !invoke(7,0x1600,off0)) return false;
            if (off1 && !invoke(7,0x1601,off1)) return false;
            if (on0 && !invoke(7,0x1500,on0)) return false;
            if (on1 && !invoke(7,0x1501,on1)) return false;
            return true;
        };
        std::vector<uint8_t> commands;
        if (apply) {
            uint32_t send=0;
            if (!request.send.address || (request.send.address&3) || request.send.size!=848 ||
                !host.normalizeGuestAddress(request.send.address,send) || !send || send>=0x2000000 ||
                848>0x2000000-send) return rejected();
            commands.resize(848);
            if (!host.readGuest(send,commands.data(),commands.size())) return rejected();
            auto half=[&](size_t at) { return uint16_t(commands[at]|(commands[at+1]<<8)); };
            // The request lands in the server buffer first (SIF receive); the
            // effect attributes below are passed to LIBSD by address.
            {
                const std::array<IopMemoryWrite,1> staged{{{image+buffer,commands}}};
                if (!host.writeIopMemoryBatch(staged)) return rejected();
            }
            // Pass 1 (10e4): bit masks per core.
            for (unsigned core=0;core<2;++core) {
                std::array<uint32_t,9> mask{};
                for (unsigned voice=0;voice<24;++voice) {
                    const uint16_t flags=half((core*24+voice)*16);
                    for (unsigned bit=0;bit<8;++bit) if (flags&(1u<<bit)) mask[bit]|=1u<<voice;
                    if (flags&0x100) mask[8]|=1u<<voice;
                }
                for (unsigned bit=0;bit<8;++bit) setField(core,bit*4,mask[bit]);
                setField(core,0x20,field(core,0x20)|mask[1]);
                setField(core,0x24,(field(core,0x24)|mask[0])&~mask[1]);
                setField(core,0x28,mask[8]);
            }
            // Pass 2 (11f4): key-on voices start silent; their volume follows KON.
            for (unsigned core=0;core<2;++core) for (unsigned voice=0;voice<24;++voice) {
                const size_t at=(core*24+voice)*16; const uint16_t flags=half(at);
                if (!(flags&2)) continue;
                const uint16_t cleared=uint16_t(flags&~0x400u);
                commands[at]=uint8_t(cleared); commands[at+1]=uint8_t(cleared>>8);
                const uint32_t v=core|(voice<<1);
                if (!invoke(5,v,0) || !invoke(5,v|0x100,0)) return rejected();
            }
            // d8c + e04: queue key-offs and let the worker run.
            put(pending,0xc,word(pending,0xc)|field(0,0)); put(pending,0x10,word(pending,0x10)|field(1,0));
            if (!runKeyWorker()) return rejected();
            // 1280: effect attributes per core (SdSetEffectAttr), record +0x310
            // with its core word stored first.
            if (word(commands,0x34c)) {
                const auto setter=registry?registry->bind("libsd",0x104,23):NativeImportBinding{};
                for (unsigned core=0;core<2;++core) {
                    const uint32_t at=0x310+core*0x24;
                    put(commands,at,core);
                    const std::array<uint8_t,4> coreWord{uint8_t(core),0,0,0};
                    const std::array<IopMemoryWrite,1> stage{{{image+buffer+at,coreWord}}};
                    if (!setter.registration || !host.writeIopMemoryBatch(stage) ||
                        !registry->invoke(setter,{core,image+buffer+at,0,0}).handled) {
                        if (!loggedEffect) {
                            loggedEffect=true;
                            try { host.log(LogLevel::Info,"[GT3/PDISPU2] RPC4 effect attributes refused by LIBSD: core="+std::to_string(core)+
                                " mode="+std::to_string(word(commands,at+4))); } catch (...) {}
                        }
                        return rejected();
                    }
                }
            }
            // Core records (12d8): effect enable, MMIX, effect volume L/R.
            for (unsigned core=0;core<2;++core) {
                const size_t at=0x300+core*0x24;
                if (!invoke(11,core|2,commands[at+8]) || !invoke(5,0x800|core,half(at+0xc)) ||
                    !invoke(5,0xb80|core,half(at+4)) || !invoke(5,0xc80|core,half(at+6))) return rejected();
            }
            // Pass 3 (1334): per-voice parameters, then the core switch masks.
            for (unsigned core=0;core<2;++core) {
                for (unsigned voice=0;voice<24;++voice) {
                    const size_t at=(core*24+voice)*16; const uint16_t flags=half(at);
                    const uint32_t v=core|(voice<<1);
                    if ((flags&0x200) && !invoke(5,v|0x200,half(at+2))) return rejected();
                    if ((flags&0x400) && (!invoke(5,v,half(at+4)) || !invoke(5,v|0x100,half(at+6)))) return rejected();
                    if ((flags&0x800) && (!invoke(9,v|0x2040,word(commands,at+8)) ||
                        !invoke(5,v|0x300,half(at+0xc)) || !invoke(5,v|0x400,half(at+0xe)))) return rejected();
                }
                constexpr std::array<uint32_t,6> switches{0x1300,0x1400,0x1800,0x1900,0x1a00,0x1b00};
                for (unsigned i=0;i<switches.size();++i)
                    if (!invoke(7,switches[i]|core,field(core,8+i*4))) return rejected();
            }
            // d14 + e04: queue key-ons and let the worker run.
            put(pending,4,word(pending,4)|field(0,4)); put(pending,8,word(pending,8)|field(1,4));
            if (!runKeyWorker()) return rejected();
            // Pass 4 (147c): volume of the voices just keyed on.
            for (unsigned core=0;core<2;++core) for (unsigned voice=0;voice<24;++voice) {
                const size_t at=(core*24+voice)*16;
                if (!(half(at)&2)) continue;
                const uint32_t v=core|(voice<<1);
                if (!invoke(5,v,half(at+4)) || !invoke(5,v|0x100,half(at+6))) return rejected();
            }
        }
        // f6c: retire voices that reached ENDX (end-watch) or whose release
        // envelope reached zero, then publish the active mask.
        for (unsigned core=0;core<2;++core) {
            const auto endx=invoke(8,0x1700|core); if (!endx) return rejected();
            uint32_t done=uint32_t(*endx)&field(core,0x28);
            const uint32_t releasing=field(core,0x24);
            for (unsigned voice=0;voice<24;++voice) if (releasing&(1u<<voice)) {
                const auto envx=invoke(6,0x500|core|(voice<<1)); if (!envx) return rejected();
                if (!(uint32_t(*envx)&0xffff)) done|=1u<<voice;
            }
            setField(core,0x20,field(core,0x20)&~done);
            setField(core,0x24,releasing&~done);
        }
        const uint32_t active0=field(0,0x20),active1=field(1,0x20);
        std::array<uint8_t,8> status{};
        put(status,0,(active1<<24)|active0);
        put(status,4,uint32_t(int32_t(active1)>>31)<<24|(active1>>8)|uint32_t(int32_t(active0)>>31));
        std::vector<IopMemoryWrite> writes{{image+0x3870,voices},{image+0x3854,std::span<const uint8_t>(pending).subspan(4)},
            {image+0x38c8,status}};
        if (apply) writes.push_back({image+buffer,commands});
        if (!host.writeIopMemoryBatch(writes)) return rejected();
        if (!loggedVoiceCommands[apply]) {
            loggedVoiceCommands[apply]=true;
            try { host.log(LogLevel::Info,std::string("[GT3/PDISPU2] RPC")+(apply?"4 voice commands":"3 voice status")+
                " handled natively (first call); active voices core0="+std::to_string(active0)+" core1="+std::to_string(active1)); } catch (...) {}
        }
        if (!request.receive.size) { RpcResult result; result.handled=true; result.serverDispatchPolicy=ServerDispatchPolicy::Suppress; return result; }
        if (!request.receive.address || request.receive.size>64) return rejected();
        std::vector<uint8_t> reply(request.receive.size);
        if (!host.readIopMemory(image+0x38c8,reply.data(),reply.size())) return rejected();
        const std::array<IopMemoryWrite,1> replyWrite{{{request.receive.address,reply}}};
        if (!host.commitIopAndEeMemory(replyWrite)) return rejected();
        RpcResult result; result.handled=true; result.resultAddress=request.receive.address;
        result.serverDispatchPolicy=ServerDispatchPolicy::Suppress; return result;
    }
    NativeExportResult updateStreamClock(uint16_t ordinal,const std::array<uint32_t,4>& args) {
        std::lock_guard lock(mutex);
        if (busy || !valid() || !sound.ownsSoundStartup(soundToken)) return {};
        const auto found=controls.find(args[0]); if (found==controls.end()) return {};
        busy=true; struct Finish { bool& b; ~Finish() { b=false; } } finish{busy};
        if (ordinal==42) return found->second->configure(args[1],args[2])?NativeExportResult{true,0}:NativeExportResult{};
        if (ordinal==43) return found->second->setPeriod(args[1])?NativeExportResult{true,0}:NativeExportResult{};
        if (ordinal==45) {
            const auto result=found->second->advance();
            return result?NativeExportResult{true,int32_t(*result)}:NativeExportResult{};
        }
        return {};
    }
};
Pdispu2Resident::Pdispu2Resident(IopHost& host,LibsdResident& sound):state_(std::make_shared<State>(host,sound)) {}
Pdispu2Resident::~Pdispu2Resident() {
    if (!reset()) {
        auto& s=*state_; std::lock_guard lifecycle(s.lifecycle);
        // A failed host restore cannot keep this dying module's callback/image.
        // Token-checked fallback unloads only the sound startup owned here.
        s.release(true);
    }
}
bool Pdispu2Resident::active() const {
    auto& s=*state_; if (!s.registry) return false;
    uint64_t token=0;
    { std::lock_guard lock(s.mutex); if (s.busy || !s.running) return false; token=s.soundToken; }
    const auto own=s.registry->bind("pdispu2",0x109,4),dependency=s.registry->bind("libpdi",0x10a,4);
    const bool soundReady=s.sound.soundInitialized() && s.sound.ownsSoundStartup(token),workersReady=s.workers.active();
    std::lock_guard lock(s.mutex);
    return !s.busy && s.soundToken==token && own.registration && own.registration==s.registration && dependency.registration==s.libpdi.registration &&
        soundReady && workersReady && s.valid();
}
bool Pdispu2Resident::start() {
    auto& s=*state_;
    { std::lock_guard lock(s.mutex); if (s.busy) return false; }
    std::lock_guard lifecycle(s.lifecycle);
    { std::lock_guard lock(s.mutex); if (s.busy || s.running || !s.registry) return false; s.busy=true; }
    struct Transaction {
        State& s; bool committed=false;
        ~Transaction() { if (!committed) s.release(); std::lock_guard lock(s.mutex); s.busy=false; }
    } transaction{s};
    if (!s.release()) return false; // Retry pending sound restoration before allocating another attempt.
    const auto original=readPdispu2Module(s.host); if (original.empty()) return false;
    s.libpdi=s.registry->bind("libpdi",0x10a,4);
    if (!s.libpdi.registration || !s.sound.active() || s.sound.soundInitialized()) return false;
    s.image=s.host.allocateIopMemory(Pdispu2Workers::ModelBytes); if (!s.image) return false;
    std::vector<uint8_t> bytes(Pdispu2Workers::ModelBytes);
    std::copy_n(original.begin()+0x36f0,0x140,bytes.begin()+0x3650);
    std::copy_n(original.begin()+0x3830,0x90,bytes.begin()+0x3790);
    // All16 original R_MIPS_32 rodata/data relocations. Text is represented by
    // explicit native functions only; these pointers are never EE entrypoints.
    for (uint32_t offset:{0x3650,0x3654,0x3658,0x365c,0x3660,0x3664,0x37c0,
                         0x37d0,0x37d4,0x37d8,0x37dc,0x37e0,0x37e4,0x37e8,0x37ec,0x380c})
        put(bytes,offset,word(bytes,offset)+s.image);
    const std::array<IopMemoryWrite,1> writes{{{s.image,bytes}}};
    if (!s.host.writeIopMemoryBatch(writes)) return false;
    std::weak_ptr<State> weak=state_;
    s.registration=s.registry->registerLibrary({"pdispu2",0x109,{{4,[weak](const auto& args) {
        const auto state=weak.lock(); return state?state->initializeVolume(args):NativeExportResult{};
    }},{10,[weak](const auto& args) {
        const auto state=weak.lock(); return state?state->setVolume(args):NativeExportResult{};
    }},{41,[weak](const auto& args) {
        const auto state=weak.lock(); return state?state->initializeStreamControl(args):NativeExportResult{};
    }},{42,[weak](const auto& args) {
        const auto state=weak.lock(); return state?state->updateStreamClock(42,args):NativeExportResult{};
    }},{43,[weak](const auto& args) {
        const auto state=weak.lock(); return state?state->updateStreamClock(43,args):NativeExportResult{};
    }},{45,[weak](const auto& args) {
        const auto state=weak.lock(); return state?state->updateStreamClock(45,args):NativeExportResult{};
    }},{50,[weak](const auto& args) {
        const auto state=weak.lock(); return state && state->releaseStreamControl(args[0])?NativeExportResult{true,0}:NativeExportResult{};
    }},{51,[weak](const auto& args) {
        const auto state=weak.lock(); return state && state->releaseStreamControl(args[0])?NativeExportResult{true,0}:NativeExportResult{};
    }},{52,[weak](const auto& args) {
        const auto state=weak.lock(); return state?state->initializeStreamDescriptor(52,args):NativeExportResult{};
    }},{58,[weak](const auto& args) {
        const auto state=weak.lock(); return state?state->initializeStreamDescriptor(58,args):NativeExportResult{};
    }}}});
    if (!s.registration || !s.registry->registerCallback(s.registration,s.image+0x9d8,[weak](const auto& args) {
        const auto state=weak.lock(); return state?state->interrupt(args):NativeExportResult{};
    })) return false;
    if (!s.workers.start(s.image) || !initializePdispu2Sound(s.host,s.sound,s.image,s.soundToken)) return false;
    std::array<uint8_t,4> one{1,0,0,0};
    const std::array<IopMemoryWrite,2> flags{{{s.image+0x37b0,one},{s.image+0x37c8,one}}};
    if (!s.host.writeIopMemoryBatch(flags)) return false;
    s.running=true;
    if (!s.valid() || !s.workers.active() || !s.sound.finishSoundStartup(s.soundToken)) return false;
    transaction.committed=true; return true;
}
bool Pdispu2Resident::reset() {
    auto& s=*state_;
    { std::lock_guard lock(s.mutex); if (s.busy) return false; }
    std::lock_guard lifecycle(s.lifecycle);
    { std::lock_guard lock(s.mutex); if (s.busy) return false; s.busy=true; }
    const bool released=s.release();
    std::lock_guard lock(s.mutex); s.busy=false; return released;
}
RpcResult Pdispu2Resident::handleRpc(const RpcRequest& request) {
    auto& s=*state_;
    std::lock_guard lifecycle(s.lifecycle);
    // With the movie-audio job running, short export/IRQ-side busy windows
    // are normal; let them pass (bounded) instead of rejecting the EE's RPC.
    for (unsigned tries=0;;++tries) {
        { std::lock_guard lock(s.mutex); if (!s.busy) break; if (tries>=200 || !s.running) return rejected(); }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    if ((request.sid==0x53505550 || request.sid==0x53505556) && request.function==5) {
        const auto address=request.send.address;
        const bool ee=address<0x2000000 || address-0x20000000u<0x2000000 ||
            address-0x80000000u<0x2000000 || address-0xa0000000u<0x2000000;
        uint32_t physical=0;
        if (!ee || !address || (address&3) || request.send.size<12 || request.send.size>848 || request.receive.size ||
            !s.host.normalizeGuestAddress(address,physical) || !physical || physical>=0x2000000 || request.send.size>0x2000000-physical || !active()) return rejected();
        { std::lock_guard lock(s.mutex); if (s.busy) return rejected(); s.busy=true; }
        struct Finish { State& s; ~Finish() { std::lock_guard lock(s.mutex); s.busy=false; } } finish{s};
        std::array<uint8_t,12> modes{}; uint32_t persistent=0;
        if (!s.host.readGuest(physical,modes.data(),modes.size()) || !s.host.readIopMemory(s.image+0x37ac,&persistent,4)) return rejected();
        const uint32_t value=(word(modes,4)==1?0x800u:0)|(word(modes,8)==1?0x80u:0)|
            (word(modes)==0?2u:uint32_t(persistent==1));
        const auto setter=s.registry->bind("libsd",0x104,11); if (!setter.registration) return rejected();
        // Original stores the3 configuration words before calling LIBSD. If
        // that call fails, retain the real state and reject; retry is safe.
        const IopMemoryWrite write{s.image+0x37a0,modes};
        std::array<uint8_t,12> committed{};
        if (!s.host.writeIopMemoryBatch(std::span(&write,1)) ||
            !s.host.readIopMemory(s.image+0x37a0,committed.data(),committed.size()) || committed!=modes ||
            !s.registry->invoke(setter,{10,value,0,0}).handled) return rejected();
        RpcResult result; result.handled=true; result.serverDispatchPolicy=ServerDispatchPolicy::Suppress;
        return result; // Original null reply: no receive DMA.
    }
    if ((request.sid==0x53505550 || request.sid==0x53505556) && (request.function==3 || request.function==4)) {
        if (!active()) return rejected();
        { std::lock_guard lock(s.mutex); if (s.busy) return rejected(); s.busy=true; }
        struct Finish { State& s; ~Finish() { std::lock_guard lock(s.mutex); s.busy=false; } } finish{s};
        return s.voiceCommands(request,request.function==4);
    }
    if ((request.sid!=0x53505550 && request.sid!=0x53505556) || request.function!=0 ||
        !request.receive.address || request.receive.size<4 || !active()) return rejected();
    { std::lock_guard lock(s.mutex); if (s.busy) return rejected(); s.busy=true; }
    struct Finish { State& s; ~Finish() { std::lock_guard lock(s.mutex); s.busy=false; } } finish{s};
    const auto version=s.workers.queryVersion(request.sid); if (!version) return rejected();
    std::array<uint8_t,4> bytes{}; put(bytes,0,*version);
    const std::array<IopMemoryWrite,1> writes{{{request.receive.address,bytes}}};
    if (!s.host.commitIopAndEeMemory(writes)) return rejected();
    RpcResult result; result.handled=true; result.resultAddress=request.receive.address;
    result.serverDispatchPolicy=ServerDispatchPolicy::Suppress; return result;
}
bool Pdispu2Resident::transferIopData(uint32_t source,uint32_t destination,uint32_t bytes) {
    auto& s=*state_;
    { std::lock_guard lock(s.mutex); if (s.busy) return false; }
    std::lock_guard lifecycle(s.lifecycle);
    if (!active()) return false;
    { std::lock_guard lock(s.mutex); if (s.busy) return false; s.busy=true; }
    struct Finish { State& s; ~Finish() { std::lock_guard lock(s.mutex); s.busy=false; } } finish{s};
    // All native stream writes are synchronous and serialized; channel0 is
    // released only after its real event completes. No guest IRQ is invented.
    return s.sound.transferIopToSound(s.soundToken,0,source,destination,bytes);
}
bool Pdispu2Resident::setVolume(uint32_t reference,uint32_t value) {
    return state_->setVolume({reference,value,0,0}).handled;
}
bool Pdispu2Resident::transferAdpcmChunk(uint32_t source,unsigned half) {
    auto& s=*state_;
    { std::lock_guard lock(s.mutex); if (s.busy) return false; }
    std::lock_guard lifecycle(s.lifecycle);
    if (!active() || half>1 || !source || (source&15) || source>UINT32_MAX-2048 ||
        (uint64_t(source)<uint64_t(s.image)+Pdispu2Workers::ModelBytes && uint64_t(s.image)<uint64_t(source)+2048)) return false;
    { std::lock_guard lock(s.mutex); if (s.busy) return false; s.busy=true; }
    struct Finish { State& s; ~Finish() { std::lock_guard lock(s.mutex); s.busy=false; } } finish{s};
    std::array<uint8_t,2048> bytes{},observed{};
    if (!s.host.readIopMemory(source,bytes.data(),bytes.size())) return false;
    for (size_t base:{size_t(0),size_t(1024)}) {
        bytes[base+1]=half?uint8_t(bytes[base+1]&~4u):uint8_t(bytes[base+1]|4u);
        bytes[base+0x3f1]=half?uint8_t(bytes[base+0x3f1]|1u):uint8_t(bytes[base+0x3f1]&~1u);
    }
    const std::array<IopMemoryWrite,1> writes{{{source,bytes}}};
    if (!s.host.writeIopMemoryBatch(writes) || !s.host.readIopMemory(source,observed.data(),observed.size()) || observed!=bytes) return false;
    // Each transfer owns a real completion event. Preserve original sequential
    // ordering; a right-channel failure does not roll back a completed left.
    return s.sound.transferIopToSound(s.soundToken,0,source,0x1d8000+half*1024,1024) &&
           s.sound.transferIopToSound(s.soundToken,0,source+1024,0x1f8000+half*1024,1024);
}
bool Pdispu2Resident::startMovieAudio(uint32_t descriptor,PdistrMpegRing& ring) {
    auto& s=*state_; std::shared_ptr<Pdispu2StreamControl> timing; uint32_t image=0; uint64_t token=0;
    {
        std::lock_guard lock(s.mutex);
        if (s.busy || s.movie || !s.valid() || !s.sound.ownsSoundStartup(s.soundToken) || !descriptor || (descriptor&3) ||
            (uint64_t(descriptor)<uint64_t(s.image)+Pdispu2Workers::ModelBytes && uint64_t(s.image)<uint64_t(descriptor)+0x4c)) return false;
        uint32_t timingAddress=0;
        if (!s.host.readIopMemory(descriptor+0x1c,&timingAddress,4)) return false;
        const auto found=s.controls.find(timingAddress); if (found==s.controls.end()) return false;
        timing=found->second; image=s.image; token=s.soundToken; s.busy=true;
    }
    struct Finish { State& s; ~Finish() { std::lock_guard lock(s.mutex); s.busy=false; } } finish{s};
    Pdispu2MovieAudio::Environment env;
    env.host=&s.host; env.sound=&s.sound; env.timing=timing; env.ring=&ring; env.image=image;
    // Original 1ec without the resident busy/lifecycle guards: the worker must
    // not wait on them (reset joins it) and IRQs may arrive during a transfer.
    env.transfer=[&host=s.host,&sound=s.sound,image,token](uint32_t source,unsigned half) {
        if (half>1 || !source || (source&15) || source>UINT32_MAX-2048 ||
            (uint64_t(source)<uint64_t(image)+Pdispu2Workers::ModelBytes && uint64_t(image)<uint64_t(source)+2048)) return false;
        std::array<uint8_t,2048> bytes{},observed{};
        if (!host.readIopMemory(source,bytes.data(),bytes.size())) return false;
        for (size_t base:{size_t(0),size_t(1024)}) {
            bytes[base+1]=half?uint8_t(bytes[base+1]&~4u):uint8_t(bytes[base+1]|4u);
            bytes[base+0x3f1]=half?uint8_t(bytes[base+0x3f1]|1u):uint8_t(bytes[base+0x3f1]&~1u);
        }
        const std::array<IopMemoryWrite,1> writes{{{source,bytes}}};
        if (!host.writeIopMemoryBatch(writes) || !host.readIopMemory(source,observed.data(),observed.size()) || observed!=bytes) return false;
        return sound.transferIopToSound(token,0,source,0x1d8000+half*1024,1024) &&
               sound.transferIopToSound(token,0,source+1024,0x1f8000+half*1024,1024);
    };
    auto job=std::make_shared<Pdispu2MovieAudio>(std::move(env));
    { std::lock_guard lock(s.mutex); s.movie=job; } // Visible to 9d8 before its callback entry exists.
    if (!job->start(descriptor)) { std::lock_guard lock(s.mutex); s.movie.reset(); return false; }
    return true;
}
bool Pdispu2Resident::stopMovieAudio() {
    auto& s=*state_; std::shared_ptr<Pdispu2MovieAudio> job;
    { std::lock_guard lock(s.mutex); job=s.movie; }
    if (!job) return true;
    job->stop();
    std::lock_guard lock(s.mutex); if (s.movie==job) s.movie.reset();
    return true;
}
std::optional<Pdispu2MovieAudio::Snapshot> Pdispu2Resident::movieAudio() const {
    auto& s=*state_; std::lock_guard lock(s.mutex);
    if (!s.movie) return {};
    return s.movie->snapshot();
}
bool Pdispu2Resident::updateStreamRange(uint32_t descriptor,const std::array<uint32_t,4>& words) {
    auto& s=*state_; std::lock_guard lock(s.mutex);
    if (s.busy || !s.valid() || !s.sound.ownsSoundStartup(s.soundToken)) return false;
    const auto found=s.controls.find(descriptor); if (found==s.controls.end()) return false;
    s.busy=true; struct Finish { bool& b; ~Finish() { b=false; } } finish{s.busy};
    return found->second->setRange(words);
}
bool Pdispu2Resident::waitForStreamRange(uint32_t descriptor,std::stop_token stop) {
    auto& s=*state_; std::shared_ptr<Pdispu2StreamControl> control;
    {
        std::lock_guard lock(s.mutex);
        if (s.busy || !s.valid() || !s.sound.ownsSoundStartup(s.soundToken)) return false;
        const auto found=s.controls.find(descriptor); if (found==s.controls.end()) return false;
        control=found->second;
    }
    // A shared resource survives concurrent reset until this wait returns;
    // reset stops it before dropping ownership or releasing resident storage.
    return control->waitForRange(stop);
}
}
namespace ps2x::iop::gt3 {
bool Pdispu2Resident::releaseStreamControl(uint32_t descriptor) { return state_->releaseStreamControl(descriptor); }
}
