#include "ps2x/iop/gt3/mcman_native_exports.h"
#include "ps2x/iop/gt3/mcman_storage.h"
#include <algorithm>
#include <cstdlib>
#include <mutex>

namespace ps2x::iop::gt3 {
namespace {
void put(std::span<uint8_t> bytes,size_t offset,uint32_t value) {
    for (unsigned i=0;i<4;++i) bytes[offset+i]=uint8_t(value>>(8*i));
}
bool verified(IopHost& host) {
    const auto file=host.openHostFile(host.translateGuestPath("cdrom0:\\MCMAN.IRX;1"));
    if (!file) return false;
    struct Close { IopHost& host; uint64_t file; ~Close() { host.closeHostFile(file); } } close{host,file};
    uint64_t size=0; size_t read=0;
    if (!host.hostFileSize(file,size) || size<52 || size>1024*1024) return false;
    std::vector<uint8_t> bytes(static_cast<size_t>(size)); std::array<uint8_t,32> digest{};
    if (!host.readHostFile(file,0,bytes.data(),bytes.size(),read) || read!=bytes.size() || !host.sha256(bytes,digest)) return false;
    constexpr std::array<uint8_t,32> expected{0x09,0x74,0xd5,0xf7,0xa3,0x19,0xb6,0x8e,0xf6,0xaf,0xf3,0xdb,0x71,0xb6,0xf0,0xfe,
        0x0f,0xa9,0xfa,0x41,0xe3,0x52,0x70,0xff,0xa6,0x97,0x82,0x60,0x1b,0xe0,0xb6,0x3f};
    return digest==expected;
}
}
struct McmanNativeExports::State {
    IopHost& host; NativeLibraryRegistry* registry; McmanStorage storage;
    McmanStartupServices services;
    std::array<McCard,2> cards;
    uint32_t legacySemaphore=0,ioSemaphore=0;
    std::recursive_mutex mutex;
    uint64_t registration=0,generation=0;
    bool active=false,inFlight=false;
    explicit State(IopHost& h):host(h),registry(h.nativeLibraries()),storage(h),services(h) {}

    NativeExportResult sema(uint16_t ordinal,uint32_t handle) {
        return registry->invoke(registry->bind("thsemap",0x101,ordinal),{handle,0,0,0});
    }
    NativeExportResult exchange(uint32_t port,uint32_t slot,uint32_t packet) {
        if (port>=2 || slot || !storage.base()) return {};
        std::array<uint8_t,0x94> descriptor{};
        if (!host.readIopMemory(packet,descriptor.data(),descriptor.size())) return {};
        const auto before=generation,slots=uint64_t(storage.base()+0x25400);
        std::array<uint8_t,32> slotData{};
        for (size_t i=0;i<4;++i) put(slotData,i*4,UINT32_MAX);
        put(slotData,(port+2)*4,slot);
        const std::array<IopMemoryWrite,1> writes{{{uint32_t(slots),slotData}}};
        if (!host.writeIopMemoryBatch(writes)) return {};
        const auto begin=registry->bind("sio2man",0x203,24),select=registry->bind("sio2man",0x203,57),
            transfer=registry->bind("sio2man",0x203,25),end=registry->bind("sio2man",0x203,26);
        if (!registry->invoke(begin,{}).handled) return {};
        struct End { NativeLibraryRegistry& registry; NativeImportBinding binding;
            ~End() { registry.invoke(binding,{}); } } finish{*registry,end};
        const auto selection=registry->invoke(select,{uint32_t(slots),0,0,0}); uint32_t available=0;
        if (!active || generation!=before || !selection.handled || selection.value!=1 ||
            !host.readIopMemory(uint32_t(slots)+16+(port+2)*4,&available,4) || available!=1) return {};
        const auto result=registry->invoke(transfer,{packet,0,0,0});
        return active && generation==before?result:NativeExportResult{};
    }

    // Reconstruct original packet fields. PS2 commands use one 0x24-word
    // DMA block each; legacy detection uses three FIFO bytes (no DMA).
    bool absentExchange(uint32_t port,uint32_t slot,uint8_t opcode,bool legacy) {
        const auto base=storage.base(); if (!base) return false;
        const auto before=generation;
        const auto packet=base+(legacy?0x129f8:0x25020);
        const auto input=base+(legacy?0x12a90:0x10d40);
        const auto output=base+(legacy?0x12b20:0x11370);
        const uint32_t length=legacy?3:opcode==0x11?4:5;
        std::array<uint8_t,0x94> descriptor{}; std::array<uint8_t,144> data{};
        put(descriptor,4+(port+2)*4,legacy?0xffc00505:0xff020405);
        put(descriptor,0x14+(port+2)*4,legacy?0x201f4:0x5ffff);
        put(descriptor,0x28,(length<<18)|(length<<8)|(legacy?0x40:0x70)|(port+2));
        data[0]=0x81; data[1]=opcode;
        if (legacy) {
            put(descriptor,0x6c,length); put(descriptor,0x70,length);
            put(descriptor,0x74,input); put(descriptor,0x78,output);
        } else {
            put(descriptor,0x7c,input); put(descriptor,0x80,0x24); put(descriptor,0x84,1);
            put(descriptor,0x88,output); put(descriptor,0x8c,0x24); put(descriptor,0x90,1);
        }
        if (legacy && !sema(8,legacySemaphore).handled) return false;
        struct Release { State& state; uint32_t handle; ~Release() { if (handle) state.sema(6,handle); } }
            release{*this,legacy?legacySemaphore:0};
        const std::array<IopMemoryWrite,2> writes{{{packet,descriptor},{input,data}}};
        if (!host.writeIopMemoryBatch(writes)) return false;
        const auto result=exchange(port,slot,packet);
        if (!active || generation!=before || !result.handled || result.value!=1) return false;
        uint32_t status=0; std::array<uint8_t,5> response{};
        if (!host.readIopMemory(packet,&status,4) || !host.readIopMemory(output,response.data(),length)) return false;
        // Only explicit absence is implemented. A connected or indeterminate
        // reply must not be converted to a missing-card result.
        return status==0x1d100 &&
            std::all_of(response.begin(),response.begin()+length,[](uint8_t byte) { return byte==0xff; });
    }
    NativeExportResult detect(uint32_t port,uint32_t slot,bool extended) {
        if (port>=2 || slot!=0) return {}; // Attached-multitap slots unsupported.
        if (extended) {
            // 0x3590: reset0x11 retries five; command-index17 -> wire f3.
            for (unsigned i=0;i<5;++i) if (!absentExchange(port,slot,0x11,false)) return {};
            if (!absentExchange(port,slot,0xf3,false)) return {};
        } else {
            // 0x6c8 -> 0x3444 selects index9 -> wire28, five attempts.
            for (unsigned i=0;i<5;++i) if (!absentExchange(port,slot,0x28,false)) return {};
        }
        // 0xa358 vs 0xa194: same legacy packet, distinct final error codes.
        for (unsigned i=0;i<5;++i) if (!absentExchange(port,slot,0x52,true)) return {};
        return {true,extended?-11:-12};
    }
    NativeExportResult bootPath(const std::array<uint32_t,4>& args) {
        std::string path; if (!args[0] || args[0]>UINT32_MAX-1024) return {};
        for (uint32_t i=0;i<1024;++i) {
            char c=0; if (!host.readIopMemory(args[0]+i,&c,1)) return {};
            if (!c) break; path+=c; if (i==1023) return {};
        }
        const auto first=path.find_first_not_of(' ');
        if (first==std::string::npos) return {true,0}; path.erase(0,first);
        if (path.size()<2 || (path[0]|0x20)!='m' || (path[1]|0x20)!='c') return {true,0};
        // Original 0xf994: final decimal digit before ':' is the port (+2);
        // preceding decimal digits form the slot, consumed only when port output exists.
        auto end=path.find(':'); if (end==std::string::npos) end=path.size();
        if (!end) return {};
        size_t cursor=end; uint32_t port=2,slot=0,multiplier=1;
        auto digit=[&](size_t p) { return path[p]>='0' && path[p]<='9'; };
        if (args[1] && cursor && digit(cursor-1)) port+=path[--cursor]-'0';
        if (args[2]) while (cursor && digit(cursor-1)) {
            slot+=uint32_t(path[--cursor]-'0')*multiplier; multiplier*=10;
        }
        std::array<uint8_t,4> portBytes{},slotBytes{}; put(portBytes,0,port); put(slotBytes,0,slot);
        std::vector<IopMemoryWrite> writes;
        if (args[1]) writes.push_back({args[1],portBytes});
        if (args[2]) writes.push_back({args[2],slotBytes});
        if (!writes.empty() && !host.writeIopMemoryBatch(writes)) return {};
        return {true,1};
    }
    NativeExportResult device(uint16_t operation,const std::array<uint32_t,4>& args) {
        if (operation==0) return {true,0};
        if (operation==1) {
            const auto result=sema(5,ioSemaphore); if (!result.handled) return {};
            ioSemaphore=0; return {true,0}; // All descriptors already closed.
        }
        // Original ioctl 0x107a4 acquires the lock and deliberately leaves it
        // held for the MCSERV dispatch tail to signal via mcman/53.
        if (operation==8) return sema(8,ioSemaphore);
        std::array<uint32_t,4> file{};
        if (!host.readIopMemory(args[0],file.data(),sizeof(file)) || !sema(8,ioSemaphore).handled) return {};
        const auto before=generation;
        struct Release { State& state; uint32_t handle; ~Release() { state.sema(6,handle); } } release{*this,ioSemaphore};
        if (operation==4 || operation==5 || operation==6 || operation==7 || operation==13)
            return {true,-13}; // Closed descriptor -5 -> original device error mapping0xf920.
        if (operation==14) return {}; // Directory descriptor cursor not implemented.
        // The absent profile supports plain units0/1 only, never multitap slots.
        if (file[1]>=2) return {};
        const auto result=detect(file[1],0,true);
        if (!active || generation!=before || !result.handled) return {};
        return result.value==-11?NativeExportResult{true,-6}:NativeExportResult{};
    }
    NativeExportResult call(uint32_t ordinal,const std::array<uint32_t,4>& args) {
        std::lock_guard lock(mutex);
        if (!active || inFlight) return {};
        const auto before=generation;
        inFlight=true;
        struct Finish { State& state; uint64_t generation;
            ~Finish() { if (state.generation==generation) state.inFlight=false; } } finish{*this,before};
        if (ordinal>=0x10000 && ordinal<0x10011) return device(uint16_t(ordinal-0x10000),args);
        if (ordinal==0x20000) return exchange(args[0]&1,args[1],args[2]);
        if (ordinal==0x20001) return {true,int32_t(((args[0]&1)<<3)+args[1])};
        if (ordinal==0x20002) return bootPath(args);
        switch (ordinal) {
        case 5: return detect(args[0],args[1],false);
        case 21: return detect(args[0],args[1],true);
        // All descriptors stay closed in this absent-only provider. Original
        // close/read/write/seek/flush check descriptor state before pointers.
        case 7: case 8: case 9: case 10: case 14: return {true,-5};
        // Original functions first call 0x6c8; absent (-12) exits before any
        // filename/output/fifth or sixth argument is accessed.
        case 6: case 11: case 12: case 13: case 15: case 16: case 23: case 36:
            return detect(args[0],args[1],false);
        case 22: case 38: case 39:
            if (args[0]>=2 || args[1]!=0) return {};
            return {true,0}; // Initialized type/format zero; no free clusters.
        case 24: case 25: case 37: return {true,0}; // No pending replacement/open descriptors; ret-only is zero.
        case 42: return {true,static_cast<int32_t>(storage.base()+0x10b44)};
        case 53: return ioSemaphore?NativeExportResult{true,int32_t(ioSemaphore)}:NativeExportResult{};
        case 26: {
            if (args[0]>=2) return {};
            const auto result=registry->invoke(registry->bind("sio2man",0x203,59),{args[0]+2,0,0,0});
            return active && generation==before ? result : NativeExportResult{};
        }
        default: return {};
        }
    }
};
McmanNativeExports::McmanNativeExports(IopHost& host):state_(std::make_shared<State>(host)) {}
McmanNativeExports::~McmanNativeExports() { reset(); }
bool McmanNativeExports::publish() {
    auto& s=*state_;
    if (!s.registry || s.active || s.registration || !verified(s.host)) return false;
    for (uint16_t ordinal:{24,25,26,57,59}) if (!s.registry->bind("sio2man",0x203,ordinal).registration) return false;
    try {
        if (!s.storage.prepare()) return false;
        // Original IRX identity at0x10b44: name pointer, version0x217.
        // Its exported library version0x206 is a separate field.
        std::array<uint8_t,12> info{}; put(info,0,s.storage.base()+0x10a00); put(info,4,0x217);
        constexpr std::array<uint8_t,11> name{'m','c','m','a','n','_','t','o','o','l',0};
        const std::array<IopMemoryWrite,2> identity{{{s.storage.base()+0x10b44,info},{s.storage.base()+0x10a00,name}}};
        if (!s.host.writeIopMemoryBatch(identity)) { reset(); return false; }
        std::weak_ptr<State> weak=state_; NativeLibrary library{"mcman",0x206,{}};
        for (uint16_t ordinal:{5,6,7,8,9,10,11,12,13,14,15,16,21,22,23,24,25,26,36,37,38,39,42,53})
            library.exports.push_back({ordinal,[weak,ordinal](const auto& args) {
                const auto state=weak.lock(); return state?state->call(ordinal,args):NativeExportResult{};
            }});
        s.registration=s.registry->registerLibrary(std::move(library));
        if (!s.registration) { reset(); return false; }
        s.active=true; ++s.generation;
        if (!s.services.start()) { reset(); return false; }
        // Card images: GT3_MEMCARD1 (port 0) and GT3_MEMCARD2 (port 1); unset
        // or "none": no card.
        for (uint32_t port=0;port<2;++port) {
            const char* path=std::getenv(port?"GT3_MEMCARD2":"GT3_MEMCARD1");
            if (!path || !*path || std::string_view(path)=="none") continue;
            std::string error;
            if (s.cards[port].attach(path,error))
                s.host.log(LogLevel::Info,"[GT3/MCMAN] memory card "+std::to_string(port+1)+": "+path+
                    (s.cards[port].formatted()?" (formatted, ":" (unformatted, ")+
                    std::to_string(s.cards[port].freeClusters())+" KB free)");
            else s.host.log(LogLevel::Warning,"[GT3/MCMAN] memory card "+std::to_string(port+1)+" not inserted: "+error);
        }
        const auto base=s.storage.base();
        constexpr std::array<uint32_t,17> operations{0xf900,0xfc3c,0xfec4,0xfc6c,0xfd2c,0xfdec,0xfe58,
            0xfd80,0x107a4,0xff64,0x10010,0x100bc,0x10168,0x1021c,0x10270,0x102cc,0x10384};
        auto callback=[&](uint32_t offset,uint32_t operation) {
            return s.registry->registerCallback(s.registration,base+offset,[weak,operation](const auto& args) {
                const auto state=weak.lock(); return state?state->call(operation,args):NativeExportResult{};
            });
        };
        for (uint32_t i=0;i<operations.size();++i) if (!callback(operations[i],0x10000+i)) { reset(); return false; }
        if (!callback(0x3a00,0x20000) || !callback(0x3a38,0x20001) || !callback(0xf994,0x20002)) { reset(); return false; }
        std::array<uint8_t,68> deviceOps{}; for (size_t i=0;i<operations.size();++i) put(deviceOps,i*4,base+operations[i]);
        std::array<uint8_t,20> device{};
        put(device,0,base+0x10b0c); put(device,4,0x10000010); put(device,8,1);
        put(device,12,base+0x10b00); put(device,16,base+0x10cc0);
        constexpr std::array<uint8_t,15> names{'M','e','m','o','r','y',' ','C','a','r','d',0,'m','c',0};
        std::array<uint8_t,16> semaParameters{}; put(semaParameters,0,1); put(semaParameters,8,1); put(semaParameters,12,1);
        std::array<uint8_t,0x94> ps2{},legacy{};
        for (uint32_t port=0;port<4;++port) {
            put(legacy,4+port*4,0xffc00505); put(legacy,0x14+port*4,0x201f4);
            if (port>=2) { put(ps2,4+port*4,0xff020405); put(ps2,0x14+port*4,0x5ffff); }
        }
        put(ps2,0x7c,base+0x10d40); put(ps2,0x80,0x24);
        put(ps2,0x88,base+0x11370); put(ps2,0x8c,0x24);
        put(legacy,0x74,base+0x12a90); put(legacy,0x78,base+0x12b20);
        const std::array<IopMemoryWrite,6> startup{{{base+0x10cc0,deviceOps},{base+0x10d2c,device},
            {base+0x10b00,names},{base+0x25500,semaParameters},{base+0x25020,ps2},{base+0x129f8,legacy}}};
        if (!s.host.writeIopMemoryBatch(startup)) { reset(); return false; }
        auto invoke=[&](std::string_view library,uint16_t version,uint16_t ordinal,uint32_t argument) {
            return s.registry->invoke(s.registry->bind(library,version,ordinal),{argument,0,0,0});
        };
        if (!invoke("secrman",0x103,4,base+0x3a00).handled || !invoke("secrman",0x103,5,base+0x3a38).handled ||
            !invoke("modload",0x101,13,base+0xf994).handled) { reset(); return false; }
        const auto legacyLock=s.sema(4,base+0x25500);
        if (!legacyLock.handled || legacyLock.value<=0) { reset(); return false; }
        s.legacySemaphore=uint32_t(legacyLock.value);
        const auto added=invoke("ioman",0x104,20,base+0x10d2c);
        if (!added.handled || added.value) { reset(); return false; }
        const auto ioLock=s.sema(4,base+0x25500);
        if (!ioLock.handled || ioLock.value<=0) { reset(); return false; }
        s.ioSemaphore=uint32_t(ioLock.value);
        std::array<uint8_t,4> legacyId{},ioId{}; put(legacyId,0,s.legacySemaphore); put(ioId,0,s.ioSemaphore);
        const std::array<IopMemoryWrite,2> locks{{{base+0x12c30,legacyId},{base+0x23f48,ioId}}};
        if (!s.host.writeIopMemoryBatch(locks)) { reset(); return false; }
        return true;
    } catch (...) { reset(); throw; }
}
void McmanNativeExports::reset() {
    auto& s=*state_;
    if (s.registry && s.registration) s.registry->unregisterLibrary(s.registration);
    std::lock_guard lock(s.mutex);
    s.registration=0; s.active=false; s.inFlight=false; ++s.generation;
    s.services.reset(); s.legacySemaphore=0; s.ioSemaphore=0;
    s.storage.reset();
    for (auto& card:s.cards) card.detach();
}
bool McmanNativeExports::active() const { std::lock_guard lock(state_->mutex); return state_->active && state_->ioSemaphore; }
McmanStartupServices& McmanNativeExports::services() { return state_->services; }
McCard* McmanNativeExports::card(uint32_t port) {
    return port<2 && state_->cards[port].present()?&state_->cards[port]:nullptr;
}
}
