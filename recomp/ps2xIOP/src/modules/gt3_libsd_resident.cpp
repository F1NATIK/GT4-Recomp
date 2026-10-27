#include "ps2x/iop/gt3/libsd_resident.h"
#include "ps2x/iop/gt3/libsd_cold_startup.h"
#include <atomic>
#include <mutex>
#include <optional>
#include <algorithm>
#include <thread>
#include <cstdio>
#include <cstdlib>
#include <string>

namespace ps2x::iop::gt3 {
namespace {
// Original table0x40b0, restricted below to configuration latches only.
constexpr std::array<uint16_t,35> registers{
    0,2,4,6,8,0xa,0xc,0xe,0x198,0x760,0x762,0x764,0x766,0x768,0x76a,0x76c,0x76e,
    0x770,0x772,0x180,0x184,0x1a0,0x1a4,0x340,0x188,0x18c,0x190,0x194,
    0x2e0,0x33c,0x1a8,0x19c,0x1c0,0x1c4,0x1c8};
constexpr std::array<uint32_t,6> busAddresses{0xbf801404,0xbf80140c,0xbf8010f0,0xbf801570,0xbf801014,0xbf801414};
// Native sound-bus state owns the sound DMA enable bits only. Other modeled
// IOP devices keep their own control state; these are not host MMIO writes.
constexpr std::array<uint32_t,6> busValues{0xbf900000,0xbf900800,0x80000,8,0x200b31e1,0x200b31e1};
void put(std::span<uint8_t> bytes,size_t offset,uint32_t value) {
    for (unsigned i=0;i<4;++i) bytes[offset+i]=uint8_t(value>>(i*8));
}
uint32_t get(std::span<const uint8_t> bytes,size_t offset,unsigned count=4) {
    uint32_t value=0;
    for (unsigned i=0;i<count;++i) value|=uint32_t(bytes[offset+i])<<(i*8);
    return value;
}
bool verified(IopHost& host, std::vector<uint8_t>& bytes) {
    const auto file=host.openHostFile(host.translateGuestPath("cdrom0:\\LIBSD.IRX;1"));
    if (!file) return false;
    struct Close { IopHost& host; uint64_t file; ~Close() { host.closeHostFile(file); } } close{host,file};
    uint64_t size=0; size_t count=0; std::array<uint8_t,32> digest{};
    if (!host.hostFileSize(file,size) || size<52 || size>1024*1024) return false;
    bytes.resize(static_cast<size_t>(size));
    if (!host.readHostFile(file,0,bytes.data(),bytes.size(),count) || count!=bytes.size() || !host.sha256(bytes,digest)) return false;
    constexpr std::string_view expected="17b208de116e4cf13dc95d624ab6d0b3aac7440b5f863c1a2bf73e1a3924d721";
    constexpr char hex[]="0123456789abcdef"; std::string actual;
    for (auto b:digest) { actual+=hex[b>>4]; actual+=hex[b&15]; }
    return actual==expected;
}
}
struct LibsdResident::State {
    IopHost& host; NativeLibraryRegistry* registry;
    mutable std::recursive_mutex mutex;
    uint32_t image=0;
    uint64_t registration=0,generation=0;
    bool running=false,inFlight=false,deliveringSound=false,transferPoisoned=false;
    std::shared_ptr<LibsdColdStartup> sound;
    std::array<bool,0x800> written{};
    NativeCallbackBinding soundCallback{},soundIrq{};
    uint32_t callbackAddress=0,callbackArgument=0;
    struct StartupCheckpoint {
        uint64_t token=0;
        uint32_t image=0;
        std::vector<uint8_t> bytes;
        std::array<bool,0x800> written{};
        std::shared_ptr<LibsdColdStartup> sound;
        std::thread::id owner;
        bool finished=false;
    };
    std::shared_ptr<StartupCheckpoint> startup;
    // Mirror of startup->token (0 when none), written under `mutex`. Lets
    // ownsSoundStartup answer without the lock: PDISPU2 asks while holding its
    // own lock, and the sound IRQ path holds this lock while entering PDISPU2.
    std::atomic<uint64_t> startupToken{0};
    uint64_t nextStartup=1;
    bool startupAccessible() const {
        return !startup || startup->finished || startup->owner==std::this_thread::get_id();
    }
    explicit State(IopHost& h):host(h),registry(h.nativeLibraries()) {}
    bool configured() const {
        std::array<uint32_t,6> values{};
        if (!running || !registration || !image || !sound || !host.readIopMemory(image+0x4c00,values.data(),sizeof(values))) return false;
        if (!sound->initialized()) return values==busValues;
        uint32_t flag=0;
        return values==sound->busValues() && host.readIopMemory(image+0x48c8,&flag,4) && flag==1 && sound->events().ready();
    }
    NativeExportResult initialize(uint32_t mode) {
        // First cold init only. Reinitialization/warm state preservation is a
        // separate unaudited runtime contract and cannot fall through to success.
        if (mode || sound->initialized()) return {};
        std::array<uint8_t,32> seed{}; uint32_t tableSize=0,flag=0;
        if (!host.readIopMemory(image+0x4a60,seed.data(),seed.size()) ||
            !host.readIopMemory(image+0x41c8,&tableSize,4) || tableSize!=2 ||
            !host.readIopMemory(image+0x48c8,&flag,4) || flag) return {};
        // The muted engine excludes modulation/noise. Original cold reset
        // leaves core0 PMON/NON intact; reject a prior nonzero configuration.
        for (uint32_t offset=0x180;offset<0x188;offset+=2) if (written[offset/2]) {
            uint16_t value=0;
            if (!host.readIopMemory(image+0x5000+offset,&value,2) || value) return {};
        }
        inFlight=true;
        struct Finish { bool& flag; ~Finish() { flag=false; } } finish{inFlight};
        const auto before=generation;
        // Strong local ownership survives a host test callback resetting the
        // resident during commit. Failed startup never publishes its resources.
        auto staged=std::make_shared<LibsdColdStartup>();
        if (!staged->initialize(seed)) return {};
        if (!seedStreamVoices(*staged)) return {};
        std::vector<uint8_t> bytes(0x6000);
        if (!host.readIopMemory(image,bytes.data(),bytes.size()) || generation!=before) return {};
        auto known=written;
        const auto& regs=staged->registers(); const auto& initialized=staged->knownRegisters();
        for (size_t i=0;i<regs.size();++i) if (initialized[i]) {
            bytes[0x5000+i*2]=uint8_t(regs[i]); bytes[0x5001+i*2]=uint8_t(regs[i]>>8); known[i]=true;
        }
        for (size_t i=0;i<6;++i) put(bytes,0x4c00+i*4,staged->busValues()[i]);
        std::fill(bytes.begin()+0x48a4,bytes.begin()+0x48cc,uint8_t(0));
        for (auto offset:{0x48cc,0x48d0,0x48d4,0x48d8,0x48dc,0x48e0,0x48e4,0x48ec,
                         0x490c,0x4910,0x4914,0x4918,0x4a98,0x4a9c,0x4ab0,0x4ab4}) put(bytes,offset,0);
        for (auto offset:{0x48c8,0x48e8,0x491c,0x4920,0x4a90}) put(bytes,offset,1);
        put(bytes,0x4aa0,staged->events().transfer(0)->eventId);
        put(bytes,0x4aa4,staged->events().transfer(1)->eventId);
        put(bytes,0x4aa8,0x1dfff1); put(bytes,0x4aac,0x1ffff1);
        // Native descriptor metadata for the three original intrman bindings.
        // These module-relative handler offsets are not executable EE addresses.
        const std::array<std::array<uint32_t,4>,3> irqs{{
            {36,1,0x2250,image+0x48e0},{40,1,0x2250,image+0x48e8},{9,1,0x11a0,0}}};
        for (size_t i=0;i<3;++i) for (size_t j=0;j<4;++j) put(bytes,0x4c40+i*16+j*4,irqs[i][j]);
        const std::array<IopMemoryWrite,1> writes{{{image,bytes}}};
        if (!host.writeIopMemoryBatch(writes) || generation!=before) return {};
        std::vector<uint8_t> committed(bytes.size());
        if (!host.readIopMemory(image,committed.data(),committed.size()) || committed!=bytes || generation!=before) return {};
        sound=std::move(staged); written=known; transferPoisoned=false;
        return configured()?NativeExportResult{true,0}:NativeExportResult{};
    }
    // Carry the muted cold-init voice state into the runtime engine: the
    // original leaves every voice stopped with its SSA/pitch/ADSR/volume
    // registers set, still stepping its loop block. IRQ stays disabled.
    static bool seedStreamVoices(LibsdColdStartup& staged) {
        const auto& regs=staged.registers(); const auto& known=staged.knownRegisters();
        auto& engine=staged.streamVoices();
        using P=Spu2Voices::Param;
        for (unsigned core=0;core<2;++core) for (unsigned voice=0;voice<24;++voice) {
            const unsigned params=core*0x400+voice*16,start=core*0x400+0x1c0+voice*12;
            const std::array<P,5> order{P::VolumeLeft,P::VolumeRight,P::Pitch,P::Adsr1,P::Adsr2};
            for (unsigned i=0;i<5;++i) if (known[params/2+i] && !engine.setParam(core,voice,order[i],regs[params/2+i])) return false;
            if (known[start/2] && known[start/2+1]) {
                const uint32_t ssa=((uint32_t(regs[start/2])<<16|regs[start/2+1])&0xfffff)*2;
                if (!engine.setAddress(core,voice,Spu2Voices::Addr::Start,ssa)) return false;
            }
            const auto snapshot=staged.voices().snapshot(core,voice);
            if (snapshot && snapshot->configured &&
                !engine.seedStopped(core,voice,snapshot->blockAddress|2,snapshot->loopAddress)) return false;
        }
        return true;
    }
    // Original 900 (SdSetEffectAttr) for reverb modes 1..6 and 9 without the
    // work-area clear flag (0x100) and without the echo/delay adjustments of
    // modes 7/8. Checked against the original instructions for every mode
    // (both cores): EVOL=depth, the mode's 0x44-byte table at 41f0 written by
    // f0 (mask 0 = all fields; address fields x4 split hi/lo), ESA = EEA end -
    // (41c8[mode]*8-2), bookkeeping 48a4/48a8/48aa/48ac/48b0/4a90/4aa8. The
    // ATTR effect-enable bit is cleared and restored around the writes, so its
    // final value is unchanged. The voice engine does not render reverb.
    NativeExportResult effectMode(uint32_t core,const std::array<uint32_t,5>& descriptor) {
        const uint32_t mode=descriptor[1];
        if (core>1 || descriptor[0]!=core || mode==0 || mode==7 || mode==8 || mode>9) return {};
        std::vector<uint8_t> bytes(0x6000);
        if (!host.readIopMemory(image,bytes.data(),bytes.size())) return {};
        if (!written[(0x19a+core*0x400)/2] || !written[(0x33c+core*0x400)/2]) return {};
        inFlight=true;
        struct Finish { bool& flag; ~Finish() { flag=false; } } finish{inFlight};
        const auto before=generation;
        auto known=written;
        auto reg=[&](uint32_t address,uint16_t value) {
            bytes[0x5000+address]=uint8_t(value); bytes[0x5001+address]=uint8_t(value>>8); known[address/2]=true;
        };
        const uint32_t table=0x41f0+mode*0x44,size=get(bytes,0x41c8+mode*4);
        const uint32_t end=(get(bytes,0x533c+core*0x400,2)<<17)|0x1ffff,esa=end-(size*8-2);
        const uint32_t bank=core*0x400,volume=core*40,mask=get(bytes,table);
        const uint16_t depthLeft=uint16_t(descriptor[2]),depthRight=uint16_t(descriptor[2]>>16);
        put(bytes,0x4a90,core); put(bytes,0x48a4+core*20,mode);
        put(bytes,0x48ac+core*20,0); put(bytes,0x48b0+core*20,0);
        put(bytes,0x4aa8+core*4,esa);
        reg(0x764+volume,depthLeft); reg(0x766+volume,depthRight);
        bytes[0x48a8+core*20]=uint8_t(depthLeft); bytes[0x48a9+core*20]=uint8_t(depthLeft>>8);
        bytes[0x48aa+core*20]=uint8_t(depthRight); bytes[0x48ab+core*20]=uint8_t(depthRight>>8);
        // f0 field order: two address registers, eight volumes, twenty address
        // registers, two volumes.
        for (unsigned field=0;field<32;++field) {
            if (mask && !((mask>>field)&1)) continue;
            const uint32_t value=get(bytes,table+4+field*2,2);
            const bool isVolume=(field>=2 && field<10) || field>=30;
            if (isVolume) {
                const uint32_t address=field<10?0x774+(field-2)*2:0x784+(field-30)*2;
                reg(address+volume,uint16_t(value));
            } else {
                const uint32_t address=bank+(field<2?0x2e4+field*4:0x2ec+(field-10)*4),scaled=value*4;
                reg(address,uint16_t(scaled>>16)); reg(address+2,uint16_t(scaled));
            }
        }
        reg(bank+0x2e0,uint16_t(esa>>17)); reg(bank+0x2e2,uint16_t(esa>>1));
        const std::array<IopMemoryWrite,1> writes{{{image,bytes}}};
        if (!host.writeIopMemoryBatch(writes) || generation!=before) return {};
        std::vector<uint8_t> committed(bytes.size());
        if (!host.readIopMemory(image,committed.data(),committed.size()) || committed!=bytes || generation!=before) return {};
        written=known;
        return configured()?NativeExportResult{true,0}:NativeExportResult{};
    }
    NativeExportResult effect(uint16_t ordinal,const std::array<uint32_t,4>& args) {
        const auto core=args[0];
        if (!sound->initialized() || core>1) return {};
        if (ordinal==23 && !(args[1]&3)) {
            std::array<uint32_t,5> requested{};
            if (host.readIopMemory(args[1],requested.data(),sizeof(requested)) && requested[1]!=0)
                return effectMode(core,requested);
        }
        if (ordinal==25 && (args[1]!=0 || args[2]!=4)) return {};
        // The audited startup subset has disabled effect/IRQ processing and
        // stopped voices. No active mixer, sound IRQ or guest callback is skipped.
        auto owned=sound;
        for (unsigned c=0;c<2;++c) for (unsigned v=0;v<24;++v) {
            const auto voice=owned->voices().snapshot(c,v);
            if (!voice || voice->phase!=Spu2StartupVoices::Phase::Stopped) return {};
        }
        std::vector<uint8_t> bytes(0x6000);
        if (!host.readIopMemory(image,bytes.data(),bytes.size())) return {};
        for (unsigned c=0;c<2;++c) {
            if (!written[(0x19a+c*0x400)/2] || (get(bytes,0x519a+c*0x400,2)&0xf0)) return {};
            const auto event=owned->events().transfer(c);
            if (!event || event->busy || get(bytes,0x4aa0+c*4)!=event->eventId) return {};
        }
        if (get(bytes,0x41c8)!=2 || !written[(0x33c+core*0x400)/2] ||
            get(bytes,0x533c+core*0x400,2)!=14+core) return {};
        std::array<uint32_t,5> descriptor{};
        if (ordinal==23) {
            if ((args[1]&3) || !host.readIopMemory(args[1],descriptor.data(),sizeof(descriptor)) ||
                descriptor!=std::array<uint32_t,5>{core,0,0,0,0}) return {};
            // Original mode0 effect table: four unit delay offsets, otherwise0.
            for (unsigned i=0;i<0x44;++i) {
                const auto expected=(i==24 || i==26 || i==40 || i==42)?1:0;
                if (bytes[0x41f0+i]!=expected) return {};
            }
        } else {
            if (get(bytes,0x41d8)!=0xdfc || get(bytes,0x491c)!=1 || get(bytes,0x4914) ||
                get(bytes,0x48d8) || get(bytes,0x4ab0)) return {};
            if (!std::all_of(bytes.begin()+0x44a0,bytes.begin()+0x48a0,[](uint8_t b) { return b==0; })) return {};
        }
        inFlight=true;
        struct Finish { bool& flag; ~Finish() { flag=false; } } finish{inFlight};
        const auto before=generation;
        auto known=written;
        auto reg=[&](uint32_t address,uint16_t value) {
            bytes[0x5000+address]=uint8_t(value); bytes[0x5001+address]=uint8_t(value>>8); known[address/2]=true;
        };
        put(bytes,0x4a90,core);
        if (ordinal==25) {
            // Original600 with(effect core, DMA channel0, mode4): a64-byte
            // prefix overlaps the first1024-byte chunk by32.29 real writes,
            // each followed by its own completion signal, wait and event clear.
            const uint32_t start=0x1d9020+core*0x20000,end=0x1e0000+core*0x20000;
            auto transfer=[&](uint32_t address,uint32_t size) {
                const auto result=owned->events().runTransfer(0,[&] {
                    return owned->memory().clear(address,size) && owned->memory().setTransferAddress(0,(address+size)&0x1fffff);
                });
                return result.accepted && result.complete &&
                    owned->events().waitTransfer(0,result.ticket,std::chrono::milliseconds(0)) && owned->events().clearEvent(0,1);
            };
            if (!transfer(start,64)) return {};
            for (uint32_t address=start+32;address<end;address+=1024)
                if (!transfer(address,std::min(1024u,end-address))) return {};
            const auto tsa=owned->memory().transferAddress(0); if (!tsa || *tsa!=(end&0x1fffff)) return {};
            reg(0x1a8,uint16_t(*tsa>>17)); reg(0x1aa,uint16_t(*tsa>>1)); reg(0x344,0x80);
            put(bytes,0x490c,(end-0x3c0)/2); put(bytes,0x491c,1); put(bytes,0x4a98,1);
            put(bytes,0x48e0,0); put(bytes,0x4924,0); put(bytes,0x4928,image+0x44a0); put(bytes,0x492c,15);
        } else {
            // Original900 mode0 replaces all effect coefficients/addresses and
            // depth. Effect processing is already disabled, so no mixer work.
            const auto bank=core*0x400,volume=core*40;
            reg(0x764+volume,0); reg(0x766+volume,0);
            for (uint32_t offset=0x774;offset<=0x786;offset+=2) reg(offset+volume,0);
            for (uint32_t offset=0x2e4;offset<=0x33a;offset+=2)
                reg(offset+bank,(offset==0x2ee || offset==0x2f2 || offset==0x30e || offset==0x312)?4:0);
            reg(0x2e0+bank,uint16_t(14+core)); reg(0x2e2+bank,0xfff8);
            put(bytes,0x4aa8+core*4,0x1dfff1+core*0x20000);
            for (uint32_t offset=0;offset<16;offset+=4) put(bytes,0x48a4+core*20+offset,0);
        }
        const std::array<IopMemoryWrite,1> writes{{{image,bytes}}};
        // A rejected IOP commit cannot undo sound writes that really completed.
        // Return unhandled on failure; retain the real RAM/event results so a
        // retry is honest. No whole-operation rollback or DMA timing is claimed.
        if (!host.writeIopMemoryBatch(writes) || generation!=before) return {};
        std::vector<uint8_t> committed(bytes.size());
        if (!host.readIopMemory(image,committed.data(),committed.size()) || committed!=bytes || generation!=before) return {};
        written=known;
        return configured()?NativeExportResult{true,0}:NativeExportResult{};
    }
    NativeExportResult setOutputMode(uint32_t value) {
        // Original LIBSD39b4 ->3850, bounded to canonical selector10 and
        // defined mode/rate/copy bits. These are output latches, not playback.
        if (!sound->initialized() || deliveringSound || (value&~0xf83u) || (value&15)>2) return {};
        uint16_t output=0,config=0; uint32_t table=0;
        if (!written[0x19a/2] || !written[0x7c0/2] || !written[0x7c6/2] ||
            !host.readIopMemory(image+0x413c,&table,4) || table!=0xbf90019a ||
            !host.readIopMemory(image+0x57c0,&output,2) || !host.readIopMemory(image+0x57c6,&config,2)) return {};
        switch (value&15) {
        case 0: output=uint16_t((output&0xfef7)|0x20); config&=0xfffd; break;
        case 1: output=uint16_t((output&0xffd7)|0x100); config|=2; break;
        case 2: output&=0xfed7; break;
        }
        config=uint16_t(value&0x80?config|0x8000:config&0x7fff);
        uint16_t media=0x200;
        if ((value&0xf00)==0x800) config|=0x1800;
        else if ((value&0xf00)==0x400) { config&=0xe7ff; media=0; }
        else config=uint16_t((config&0xe7ff)|0x800);
        std::array<uint8_t,4> saved{}; put(saved,0,value);
        const std::array<uint8_t,2> o{uint8_t(output),uint8_t(output>>8)},c{uint8_t(config),uint8_t(config>>8)},m{uint8_t(media),uint8_t(media>>8)};
        const std::array<IopMemoryWrite,4> writes{{{image+0x57c8,m},{image+0x57c0,o},{image+0x57c6,c},{image+0x4a50,saved}}};
        inFlight=true; struct Finish { bool& flag; ~Finish() { flag=false; } } finish{inFlight};
        const auto before=generation;
        if (!host.writeIopMemoryBatch(writes) || generation!=before) return {};
        for (const auto& write:writes) {
            std::array<uint8_t,4> actual{};
            if (!host.readIopMemory(write.address,actual.data(),write.bytes.size()) ||
                !std::equal(write.bytes.begin(),write.bytes.end(),actual.begin())) return {};
        }
        if (generation!=before || !configured()) return {};
        written[0x7c8/2]=true; return {true,0};
    }
    NativeExportResult setIrqEnabled(const std::array<uint32_t,4>& args) {
        // Original 3960 (SdSetCoreAttr generic path): ATTR (selector 0x23,
        // register 0x19a) bit = shift table 4a52[attr]. Attr 4 (IRQ enable,
        // shift 6) drives the engine; attr 2 (effect enable, shift 7) is a
        // register latch only - the engine does not render effects/reverb.
        const auto attr=args[0]&~1u;
        if (!sound->initialized() || (attr!=2 && attr!=4) || args[0]>5 || args[1]>1) return {};
        const auto core=args[0]&1,address=0x19a+core*0x400;
        const uint16_t expectedShift=attr==4?6:7;
        uint16_t previous=0,shift=0; uint32_t table=0;
        if (!written[address/2] || !host.readIopMemory(image+0x5000+address,&previous,2) ||
            !host.readIopMemory(image+0x4a52+attr,&shift,2) || shift!=expectedShift ||
            !host.readIopMemory(image+0x413c,&table,4) || table!=0xbf90019a) return {};
        inFlight=true;
        struct Finish { bool& flag; ~Finish() { flag=false; } } finish{inFlight};
        const auto before=generation;
        const uint16_t value=uint16_t((previous&~(1u<<shift))|(args[1]<<shift));
        const std::array<uint8_t,2> bytes{uint8_t(value),uint8_t(value>>8)};
        const std::array<IopMemoryWrite,1> writes{{{image+0x5000+address,bytes}}};
        if (!host.writeIopMemoryBatch(writes) || generation!=before) return {};
        uint16_t committed=0;
        if (!host.readIopMemory(image+0x5000+address,&committed,2) || committed!=value || generation!=before || !configured()) return {};
        // Clearing the enable acknowledges a latched IRQ, as on the hardware.
        if (attr==4 && !sound->streamVoices().setIrqEnable(core,args[1]!=0)) return {};
        return {true,0}; // Original setter is void; native acknowledgment only.
    }
    NativeExportResult setSoundCallback(const std::array<uint32_t,4>& args) {
        if (!sound->initialized()) return {};
        std::array<uint32_t,3> old{};
        if (!host.readIopMemory(image+0x48cc,old.data(),sizeof(old)) ||
            old!=std::array<uint32_t,3>{0,callbackAddress,callbackArgument}) return {};
        NativeCallbackBinding candidate{};
        if (args[0]) {
            uint32_t instruction=0;
            if ((args[0]&3) || !host.readIopMemory(args[0],&instruction,4)) return {};
            // Export dispatch already holds the registry lock before state.
            candidate=registry->bindCallback(args[0]);
            if (!candidate.callback) return {};
        }
        inFlight=true;
        struct Finish { bool& flag; ~Finish() { flag=false; } } finish{inFlight};
        const auto before=generation;
        const auto previous=callbackAddress;
        auto owned=sound;
        std::array<uint8_t,8> bytes{}; put(bytes,0,args[0]); put(bytes,4,args[1]);
        const std::array<IopMemoryWrite,1> writes{{{image+0x48d0,bytes}}};
        if (!host.writeIopMemoryBatch(writes) || generation!=before) return {};
        std::array<uint8_t,8> committed{};
        if (!host.readIopMemory(image+0x48d0,committed.data(),committed.size()) || committed!=bytes || generation!=before) return {};
        if (candidate.callback) {
            const auto current=registry->bindCallback(args[0]);
            if (current.callback!=candidate.callback || current.registration!=candidate.registration) return {};
        }
        Spu2Events::SoundHandler handler;
        if (candidate.callback) handler=[registry=registry,candidate,argument=args[1]](uint32_t mask) {
            // Original11a0 acknowledges after calling the handler regardless of
            // its numeric result. An unavailable native implementation is different.
            return registry->invokeCallback(candidate,{mask,argument,0,0}).handled;
        };
        if (!owned->events().setSoundHandler(std::move(handler))) return {};
        soundCallback=candidate; callbackAddress=args[0]; callbackArgument=args[1];
        return {true,int32_t(previous)};
    }
    NativeExportResult dispatchSound(uint32_t mask) {
        // Reached through the native registry callback at original11a0, so the
        // lock order stays registry -> resident even for external event sources.
        std::lock_guard lock(mutex);
        if (inFlight || deliveringSound || !startupAccessible() || !configured() || !sound->initialized() || !mask || (mask&~3u)) return {};
        std::array<uint32_t,3> callback{};
        if (!host.readIopMemory(image+0x48cc,callback.data(),sizeof(callback)) ||
            callback!=std::array<uint32_t,3>{0,callbackAddress,callbackArgument}) return {};
        deliveringSound=true;
        struct Finish { bool& flag; ~Finish() { flag=false; } } finish{deliveringSound};
        const auto before=generation; auto owned=sound;
        const bool handled=owned->events().raiseSound(mask);
        return handled && generation==before && configured()?NativeExportResult{true,1}:NativeExportResult{};
    }
    // Reject noncanonical/unsupported selectors rather than aliasing another
    // register through the original unchecked table indexing.
    static std::optional<uint32_t> offset(uint16_t ordinal,uint32_t entry) {
        if (entry>0xffff) return {};
        const auto index=entry>>8,low=entry&0xff,core=entry&1;
        if (index>=registers.size()) return {};
        uint32_t address=registers[index];
        if (ordinal==5 || ordinal==6) {
            if (index<=4) {
                if ((low&0xc0) || ((low>>1)&31)>=24) return {};
                address+=core*0x400+(low&0x3e)*8;
            } else if (index==8) {
                if (low!=core) return {};
                address+=core*0x400;
            } else if (index>=9 && index<=16) {
                if (low!=(0x80|core)) return {};
                address+=core*40; // Original bit0x80 changes core stride400 to28.
            } else return {}; // ENVX/current-volume registers require voice execution.
        } else if (ordinal==7 || ordinal==8) {
            // KON/KOFF (set) and ENDX (get) are served by the native voice engine.
            const bool engine=(ordinal==7 && (index==0x15 || index==0x16)) || (ordinal==8 && index==0x17);
            if (low!=core || !(engine || index==0x13 || index==0x14 || (index>=0x18 && index<=0x1b))) return {};
            address+=core*0x400;
        } else if (ordinal==9 || ordinal==10) {
            if (index>=0x1c && index<=0x1f) {
                if (low!=core) return {};
                address+=core*0x400;
            } else if (index==0x20 || (ordinal==10 && index==0x22)) {
                // SSA set/get; NAX (current address) get from the voice engine.
                // Original 34f8/3558 index with (entry>>6)&0x3fc, so low bit 0x40
                // (part of the SDK voice-address selectors, e.g. 0x2040/0x2240
                // used by PDISPU2) is ignored. 0x80 stays rejected.
                if ((low&0x80) || ((low>>1)&31)>=24) return {};
                address+=core*0x400+(low&0x3e)*6;
            } else return {}; // Loop address remains unsupported.
        } else return {};
        return address<0x7fe?std::optional{address}:std::nullopt;
    }
    NativeExportResult call(uint16_t ordinal,const std::array<uint32_t,4>& args) {
        std::lock_guard lock(mutex);
        // A sound IRQ handler (original PDISPU2 304) reads NAX and writes IRQA,
        // voice params and IRQ enable while the interrupt is being delivered.
        const bool irqSafe=ordinal==5 || ordinal==7 || ordinal==9 || ordinal==10 || ordinal==11;
        if (inFlight || (deliveringSound && !irqSafe) || !startupAccessible() || !configured()) return {};
        if (ordinal==4) return initialize(args[0]);
        if (ordinal==23 || ordinal==25) return effect(ordinal,args);
        if (ordinal==27) return setSoundCallback(args);
        if (ordinal==11) return args[0]==10?setOutputMode(args[1]):setIrqEnabled(args);
        if (ordinal==6 && (args[0]>>8)==5) {
            // ENVX (current envelope level) is read from the voice engine.
            const unsigned low=args[0]&0xff,voice=(low>>1)&31;
            if (args[0]>0xffff || (low&0xc0) || voice>=24 || !sound->initialized()) return {};
            const auto state=sound->streamVoices().voice(low&1,voice);
            return state?NativeExportResult{true,int32_t(uint16_t(state->envelope))}:NativeExportResult{};
        }
        const auto selected=offset(ordinal,args[0]); if (!selected) return {};
        const auto selectorIndex=args[0]>>8; const unsigned core=args[0]&1,voice=(args[0]>>1)&31;
        const bool engineOp=(ordinal==5 && selectorIndex<=4) || (ordinal==7 && (selectorIndex==0x15 || selectorIndex==0x16)) ||
            (ordinal==8 && selectorIndex==0x17) || (ordinal==9 && (selectorIndex==0x1f || selectorIndex==0x20)) ||
            (ordinal==10 && selectorIndex==0x22);
        // Engine-served reads and key switches exist only after cold init; before
        // it, parameter/address writes stay register-bank only (engine is seeded
        // from those registers when init completes).
        const bool engineLive=sound->initialized();
        if (!engineLive && (ordinal==7 || ordinal==8 || ordinal==10) && engineOp) return {};
        auto& engine=sound->streamVoices();
        if (ordinal==8 && selectorIndex==0x17) {
            const auto flags=engine.endFlags(core);
            return flags?NativeExportResult{true,int32_t(*flags)}:NativeExportResult{};
        }
        if (ordinal==10 && selectorIndex==0x22) {
            const auto nax=engine.address(core,voice,Spu2Voices::Addr::Next);
            return nax?NativeExportResult{true,int32_t(*nax)}:NativeExportResult{};
        }
        // Volume sweep (bit15) is not modelled: reject before any register write.
        if (ordinal==5 && selectorIndex<=1 && (args[1]&0x8000)) return {};
        inFlight=true;
        struct Finish { bool& flag; ~Finish() { flag=false; } } finish{inFlight};
        const auto before=generation;
        const auto address=*selected,index=args[0]>>8;
        const bool pair=ordinal>=7 && !(ordinal>=9 && index==0x1d);
        const auto count=pair?4u:2u;
        if ((ordinal&1)==0) {
            if (!written[address/2] || (pair && !written[address/2+1])) return {};
            std::array<uint8_t,4> bytes{};
            if (!host.readIopMemory(image+0x5000+address,bytes.data(),count)) return {};
            const uint32_t low=uint32_t(bytes[0])|(uint32_t(bytes[1])<<8);
            const uint32_t high=uint32_t(bytes[2])|(uint32_t(bytes[3])<<8);
            const uint32_t value=ordinal==6?low:ordinal==8?low|(high<<16):
                (low<<17)|(index==0x1d?0x1ffffu:high<<1);
            return generation==before?NativeExportResult{true,int32_t(value)}:NativeExportResult{};
        }
        const uint32_t value=args[1]; std::array<uint8_t,4> bytes{};
        if (ordinal==5) put(bytes,0,value&0xffff);
        else if (ordinal==7) put(bytes,0,value&0xffffff);
        else {
            // Original0x3530/3548: high=value>>17, low=(value>>1)&fff8.
            // EEA writes only the high half; reads fill low17 bits with ones.
            const uint16_t high=uint16_t(value>>17),low=uint16_t((value>>1)&0xfff8);
            bytes={uint8_t(high),uint8_t(high>>8),uint8_t(low),uint8_t(low>>8)};
        }
        const std::array<IopMemoryWrite,1> writes{{{image+0x5000+address,std::span<const uint8_t>(bytes.data(),count)}}};
        if (!host.writeIopMemoryBatch(writes) || generation!=before || !configured()) return {};
        written[address/2]=true; if (pair) written[address/2+1]=true;
        if (engineOp && engineLive) {
            using P=Spu2Voices::Param;
            constexpr std::array<P,5> params{P::VolumeLeft,P::VolumeRight,P::Pitch,P::Adsr1,P::Adsr2};
            const uint32_t soundAddress=value&0x1ffff0; // Setter keeps high bits and 16-byte-aligned low bits.
            const bool applied=
                ordinal==5?engine.setParam(core,voice,params[selectorIndex],uint16_t(value)):
                ordinal==7?(selectorIndex==0x15?engine.keyOn(core,value&0xffffff):engine.keyOff(core,value&0xffffff)):
                selectorIndex==0x1f?engine.setIrqAddress(core,soundAddress):
                engine.setAddress(core,voice,Spu2Voices::Addr::Start,soundAddress);
            if (!applied) return {};
        }
        return {true,0}; // These original setters return void; native transport acknowledgment only.
    }
};
LibsdResident::LibsdResident(IopHost& host):state_(std::make_unique<State>(host)) {}
LibsdResident::~LibsdResident() { reset(); }
bool LibsdResident::active() const {
    auto& s=*state_;
    const auto binding=s.registry?s.registry->bind("libsd",0x104,6):NativeImportBinding{};
    std::lock_guard lock(s.mutex);
    return binding.registration && binding.registration==s.registration && s.configured();
}
bool LibsdResident::start() {
    auto& s=*state_; uint64_t before=0;
    {
        std::lock_guard lock(s.mutex);
        if (!s.registry || s.running || s.inFlight) return false;
        s.inFlight=true; before=++s.generation;
    }
    // Registry dispatch holds its own mutex before entering a register handler.
    // Never register/unregister/bind while holding this service's state mutex.
    struct Transaction {
        State& s; uint32_t image=0; uint64_t registration=0;
        ~Transaction() {
            if (registration) s.registry->unregisterLibrary(registration);
            if (image) s.host.freeIopMemory(image);
            std::lock_guard lock(s.mutex); s.inFlight=false;
        }
    } transaction{s};
    std::vector<uint8_t> original;
    if (!verified(s.host,original)) return false;
    auto sound=std::make_shared<LibsdColdStartup>();
    transaction.image=s.host.allocateIopMemory(0x6000); if (!transaction.image) return false;
    std::vector<uint8_t> image(0x6000);
    std::copy_n(original.begin()+0x4040,0x200,image.begin()+0x3f90);
    std::copy_n(original.begin()+0x4240,0x8e0,image.begin()+0x4190);
    for (size_t i=0;i<registers.size();++i) put(image,0x40b0+i*4,0xbf900000+registers[i]);
    for (size_t i=0;i<busValues.size();++i) put(image,0x4c00+i*4,busValues[i]);
    // Unwritten hardware register values are unknown: zeroed storage is
    // not exposed by getters until a supported setter establishes a value.
    const std::array<IopMemoryWrite,1> writes{{{transaction.image,image}}};
    if (!s.host.writeIopMemoryBatch(writes)) return false;
    { std::lock_guard lock(s.mutex); if (s.generation!=before) return false; }
    NativeLibrary library{"libsd",0x104,{}};
    for (uint16_t ordinal=4;ordinal<=10;++ordinal)
        library.exports.push_back({ordinal,[&s,ordinal](const auto& args) { return s.call(ordinal,args); }});
    for (uint16_t ordinal:{11,23,25,27})
        library.exports.push_back({ordinal,[&s,ordinal](const auto& args) { return s.call(ordinal,args); }});
    transaction.registration=s.registry->registerLibrary(std::move(library));
    if (!transaction.registration) return false;
    const auto irq=s.registry->registerCallback(transaction.registration,transaction.image+0x11a0,
        [&s](const auto& args) { return s.dispatchSound(args[0]); });
    if (!irq) return false;
    std::lock_guard lock(s.mutex); std::array<uint32_t,6> values{};
    if (s.generation!=before || !s.host.readIopMemory(transaction.image+0x4c00,values.data(),sizeof(values)) || values!=busValues) return false;
    s.image=transaction.image; s.registration=transaction.registration;
    s.soundIrq={transaction.registration,irq};
    s.sound=std::move(sound); s.running=true;
    transaction.image=0; transaction.registration=0; return true;
}
void LibsdResident::reset() {
    auto& s=*state_; uint32_t image=0; uint64_t registration=0;
    {
        std::lock_guard lock(s.mutex); ++s.generation; s.running=false;
        image=s.image; registration=s.registration; s.image=0; s.registration=0; s.written.fill(false);
        s.soundCallback={}; s.soundIrq={}; s.callbackAddress=0; s.callbackArgument=0;
        s.startup.reset(); s.startupToken=0;
        if (s.sound) s.sound->events().reset(); s.sound.reset();
    }
    if (registration) s.registry->unregisterLibrary(registration);
    if (image) s.host.freeIopMemory(image);
}
NativeExportResult LibsdResident::busControl(uint32_t address) const {
    auto& s=*state_;
    const auto binding=s.registry?s.registry->bind("libsd",0x104,6):NativeImportBinding{};
    std::lock_guard lock(s.mutex);
    if (!binding.registration || binding.registration!=s.registration || !s.configured()) return {};
    const auto it=std::find(busAddresses.begin(),busAddresses.end(),address);
    const auto& values=s.sound->initialized()?s.sound->busValues():busValues;
    return it==busAddresses.end()?NativeExportResult{}:NativeExportResult{true,int32_t(values[it-busAddresses.begin()])};
}
bool LibsdResident::soundInitialized() const {
    auto& s=*state_;
    const auto binding=s.registry?s.registry->bind("libsd",0x104,4):NativeImportBinding{};
    std::lock_guard lock(s.mutex);
    return binding.registration && binding.registration==s.registration && s.startupAccessible() && s.configured() && s.sound->initialized();
}
bool LibsdResident::readSoundMemory(uint32_t address,std::span<uint8_t> destination) const {
    auto& s=*state_;
    const auto binding=s.registry?s.registry->bind("libsd",0x104,4):NativeImportBinding{};
    std::lock_guard lock(s.mutex);
    return binding.registration && binding.registration==s.registration && !s.inFlight && s.startupAccessible() && s.configured() &&
        s.sound->initialized() && s.sound->memory().read(address,destination);
}
bool LibsdResident::transferIopToSound(uint64_t token,unsigned channel,uint32_t source,uint32_t destination,uint32_t bytes) {
    auto& s=*state_;
    const auto binding=s.registry?s.registry->bind("libsd",0x104,4):NativeImportBinding{};
    std::lock_guard lock(s.mutex);
    const uint64_t rounded=(uint64_t(bytes)+63)&~uint64_t(63);
    if (!binding.registration || binding.registration!=s.registration || !token || !s.startup || s.startup->token!=token ||
        !s.startup->finished || s.inFlight || s.deliveringSound || s.transferPoisoned || !s.configured() || !s.sound->initialized() ||
        channel>1 || !bytes || bytes>0x40000 || source<0x4000000 || source>=0x4500000 || (source&3) ||
        rounded>0x4500000-source || (destination&15) || destination>=Spu2Memory::Size || rounded>Spu2Memory::Size-destination) return false;
    auto owned=s.sound;
    const auto event=owned->events().transfer(channel); const auto core=channel*0x400;
    std::vector<uint8_t> metadata(0x6000),data(size_t(rounded),0);
    if (!event || event->busy || !s.host.readIopMemory(s.image,metadata.data(),metadata.size()) ||
        get(metadata,0x491c+channel*4)!=1 || get(metadata,0x4914+channel*4) ||
        get(metadata,0x48d8+channel*4) || get(metadata,0x4ab0+channel*4) ||
        get(metadata,0x4aa0+channel*4)!=event->eventId || !s.written[(0x19a+core)/2] ||
        (get(metadata,0x519a+core,2)&0x30) || !s.host.readIopMemory(source,data.data(),data.size())) return false;
    const auto before=s.generation; s.inFlight=true;
    struct Finish { bool& flag; ~Finish() { flag=false; } } finish{s.inFlight};
    auto reg=[&](uint32_t offset,uint16_t value) { metadata[0x5000+offset]=uint8_t(value);metadata[0x5001+offset]=uint8_t(value>>8); };
    auto commit=[&] {
        const IopMemoryWrite write{s.image,metadata};
        if (!s.host.writeIopMemoryBatch(std::span(&write,1)) || s.generation!=before) return false;
        std::vector<uint8_t> actual(metadata.size());
        return s.host.readIopMemory(s.image,actual.data(),actual.size()) && actual==metadata && s.generation==before;
    };
    put(metadata,0x490c+channel*4,destination/2); put(metadata,0x4914+channel*4,0);
    put(metadata,0x491c+channel*4,0); put(metadata,0x48e0+channel*8,channel);
    put(metadata,0x4924,0); put(metadata,0x4928,source); put(metadata,0x492c,uint32_t(rounded/64));
    const auto attr=uint16_t(get(metadata,0x519a+core,2));
    reg(0x19a+core,uint16_t(attr|0x20));reg(0x1a8+core,uint16_t(destination>>17));reg(0x1aa+core,uint16_t(destination>>1));
    bool staged=false;
    const auto result=owned->events().runTransfer(channel,[&] {
        // Completion is published only after source bytes and final metadata
        // really commit. A failure after staging poisons further transfers.
        staged=true;
        if (!commit() || !owned->memory().write(destination,data)) return false;
        const uint32_t end=(destination+uint32_t(rounded))&(Spu2Memory::Size-1);
        if (!owned->memory().setTransferAddress(channel,end)) return false;
        reg(0x1a8+core,uint16_t(end>>17));reg(0x1aa+core,uint16_t(end>>1));reg(0x19a+core,uint16_t(attr&~0x30));reg(0x344+core,0x80);
        put(metadata,0x491c+channel*4,1);put(metadata,0x4a98+channel*4,1);
        return commit() && s.configured();
    });
    if (!result.accepted || !result.complete || s.generation!=before ||
        !owned->events().waitTransfer(channel,result.ticket,std::chrono::milliseconds(0)) || !owned->events().clearEvent(channel,1)) {
        if (staged && s.generation==before) s.transferPoisoned=true;
        return false;
    }
    for (auto offset:{0x19au,0x1a8u,0x1aau,0x344u}) s.written[(offset+core)/2]=true;
    return true;
}
std::optional<Spu2Events::Transfer> LibsdResident::transferState(unsigned channel) const {
    auto& s=*state_;
    const auto binding=s.registry?s.registry->bind("libsd",0x104,4):NativeImportBinding{};
    std::lock_guard lock(s.mutex);
    if (!binding.registration || binding.registration!=s.registration || s.inFlight || !s.startupAccessible() || !s.configured() || !s.sound->initialized()) return {};
    return s.sound->events().transfer(channel);
}
bool LibsdResident::signalSoundInterrupt(uint32_t mask) {
    auto& s=*state_; NativeCallbackBinding binding{};
    { std::lock_guard lock(s.mutex); binding=s.soundIrq; }
    // Never hold state while entering registry. Stale/restarted bindings reject.
    if (!binding.callback || !s.registry) return false;
    const auto result=s.registry->invokeCallback(binding,{mask,0,0,0});
    return result.handled && result.value==1;
}
std::optional<uint32_t> LibsdResident::renderSound(uint32_t frames,std::span<int16_t> stereo) {
    auto& s=*state_; std::shared_ptr<LibsdColdStartup> owned;
    {
        std::lock_guard lock(s.mutex);
        if (!s.running || !s.sound || !s.sound->initialized() || !s.startupAccessible()) return {};
        owned=s.sound;
    }
    // The engine serializes itself; LIBSD register calls from the IRQ handler
    // (or other threads) take this resident's lock only after render returns.
    const auto raised=owned->streamVoices().render(frames,stereo);
    if (!raised) return {};
    // GT3_SPU_VOICE_LOG=1 (diagnostic): once a second, every sounding voice
    // (core:voice, pitch, volume left/right, envelope, start address).
    static const bool voiceLog=std::getenv("GT3_SPU_VOICE_LOG")!=nullptr;
    if (voiceLog) {
        static uint64_t since=0; since+=frames;
        if (since>=48000u) {
            since=0; std::string line="[spu-voices]"; char item[96];
            for (unsigned core=0;core<2;++core) for (unsigned v=0;v<24;++v) {
                const auto st=owned->streamVoices().voice(core,v);
                if (!st || st->phase==Spu2Voices::Phase::Stopped) continue;
                std::snprintf(item,sizeof(item)," %u:%u p%04x l%d r%d e%d a%x",core,v,st->pitch,st->volumeLeft,st->volumeRight,st->envelope,st->startAddress);
                line+=item;
            }
            std::fprintf(stderr,"%s\n",line.c_str());
        }
    }
    if (*raised && !signalSoundInterrupt(*raised)) return {};
    return raised;
}
NativeExportResult LibsdResident::invoke(uint16_t ordinal,const std::array<uint32_t,4>& args) {
    // Only register-level exports; 4/23/25/27 manage resources or bind
    // registry callbacks and stay reachable only through the registry.
    if (ordinal<5 || ordinal>11) return {};
    auto& s=*state_;
    { std::lock_guard lock(s.mutex); if (!s.running) return {}; }
    return s.call(ordinal,args);
}
bool LibsdResident::setSoundDecodeTap(Spu2Voices::DecodeTap tap) {
    auto& s=*state_; std::lock_guard lock(s.mutex);
    if (!s.running || !s.sound || !s.sound->initialized()) return false;
    s.sound->streamVoices().setDecodeTap(std::move(tap)); return true;
}
bool LibsdResident::withSoundInterruptsSuspended(const std::function<bool()>& section) {
    auto& s=*state_; std::lock_guard lock(s.mutex);
    if (!s.running || !s.sound || !s.sound->initialized()) return false;
    return section();
}
std::optional<Spu2Voices::VoiceState> LibsdResident::soundVoice(unsigned core,unsigned voice) const {
    auto& s=*state_; std::shared_ptr<LibsdColdStartup> owned;
    { std::lock_guard lock(s.mutex); if (!s.running || !s.sound || !s.sound->initialized()) return {}; owned=s.sound; }
    return owned->streamVoices().voice(core,voice);
}
uint64_t LibsdResident::beginSoundStartup() {
    auto& s=*state_;
    const auto binding=s.registry?s.registry->bind("libsd",0x104,4):NativeImportBinding{};
    std::lock_guard lock(s.mutex);
    if (!binding.registration || binding.registration!=s.registration || s.inFlight || s.deliveringSound ||
        s.startup || !s.configured() || s.sound->initialized() || !s.nextStartup) return 0;
    auto checkpoint=std::make_shared<State::StartupCheckpoint>();
    checkpoint->bytes.resize(0x6000);
    if (!s.host.readIopMemory(s.image,checkpoint->bytes.data(),checkpoint->bytes.size())) return 0;
    checkpoint->token=s.nextStartup++; checkpoint->image=s.image; checkpoint->sound=s.sound;
    checkpoint->written=s.written; checkpoint->owner=std::this_thread::get_id();
    s.startup=checkpoint; s.startupToken=checkpoint->token; return checkpoint->token;
}
bool LibsdResident::finishSoundStartup(uint64_t token) {
    auto& s=*state_;
    const auto binding=s.registry?s.registry->bind("libsd",0x104,4):NativeImportBinding{};
    std::lock_guard lock(s.mutex);
    if (!binding.registration || binding.registration!=s.registration || s.inFlight || s.deliveringSound ||
        !s.startup || s.startup->token!=token || s.startup->finished ||
        s.startup->owner!=std::this_thread::get_id() || !s.configured() || !s.sound->initialized()) return false;
    s.startup->finished=true; s.startup->owner={}; return true;
}
bool LibsdResident::restoreSoundStartup(uint64_t token) {
    auto& s=*state_;
    const auto binding=s.registry?s.registry->bind("libsd",0x104,4):NativeImportBinding{};
    std::lock_guard lock(s.mutex);
    if (!binding.registration || binding.registration!=s.registration || s.inFlight || s.deliveringSound ||
        !s.running || !s.startup || s.startup->token!=token || s.startup->image!=s.image) return false;
    // Restoration also works after a corrupt/failed prior restore. Do not require
    // configured(): the host may have written the entry image without readback.
    auto checkpoint=s.startup; auto removed=s.sound;
    const auto before=s.generation;
    s.inFlight=true;
    struct Finish { bool& flag; ~Finish() { flag=false; } } finish{s.inFlight};
    const std::array<IopMemoryWrite,1> writes{{{s.image,checkpoint->bytes}}};
    if (!s.host.writeIopMemoryBatch(writes) || s.generation!=before) return false;
    std::vector<uint8_t> committed(checkpoint->bytes.size());
    if (!s.host.readIopMemory(s.image,committed.data(),committed.size()) || committed!=checkpoint->bytes || s.generation!=before) return false;
    if (removed!=checkpoint->sound) removed->events().reset();
    s.sound=checkpoint->sound; s.written=checkpoint->written;
    s.soundCallback={}; s.callbackAddress=0; s.callbackArgument=0; s.startup.reset(); s.startupToken=0;
    return s.configured() && !s.sound->initialized();
}
bool LibsdResident::ownsSoundStartup(uint64_t token) const {
    auto& s=*state_; return token && s.startupToken.load()==token;
}
bool LibsdResident::abandonSoundStartup(uint64_t token) {
    auto& s=*state_; uint32_t image=0; uint64_t registration=0;
    {
        std::lock_guard lock(s.mutex);
        if (!token || !s.startup || s.startup->token!=token || s.inFlight || s.deliveringSound) return false;
        ++s.generation; s.running=false; image=s.image; registration=s.registration;
        s.image=0; s.registration=0; s.written.fill(false); s.startup.reset(); s.startupToken=0;
        s.soundCallback={}; s.soundIrq={}; s.callbackAddress=0; s.callbackArgument=0;
        if (s.sound) s.sound->events().reset(); s.sound.reset();
    }
    if (registration) s.registry->unregisterLibrary(registration);
    if (image) s.host.freeIopMemory(image);
    return true;
}
}
