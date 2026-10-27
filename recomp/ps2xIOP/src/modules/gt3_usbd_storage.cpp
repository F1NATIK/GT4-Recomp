#include "ps2x/iop/gt3/usbd_storage.h"
#include <vector>

namespace ps2x::iop::gt3 {
namespace {
bool verified(IopHost& host) {
    const auto file=host.openHostFile(host.translateGuestPath("cdrom0:\\USBD.IRX;1")); if (!file) return false;
    struct Close { IopHost& host; uint64_t file; ~Close() { host.closeHostFile(file); } } close{host,file};
    uint64_t size=0; size_t count=0; std::array<uint8_t,32> digest{};
    if (!host.hostFileSize(file,size) || size<52 || size>1024*1024) return false;
    std::vector<uint8_t> bytes(static_cast<size_t>(size));
    if (!host.readHostFile(file,0,bytes.data(),bytes.size(),count) || count!=bytes.size() || !host.sha256(bytes,digest)) return false;
    constexpr std::string_view expected="f3efe8154f3b785bb974f4993a3dc9c9242441ed6d2eed30a64143a9b6830265";
    constexpr char hex[]="0123456789abcdef"; std::string actual;
    for (auto b:digest) { actual+=hex[b>>4]; actual+=hex[b&15]; }
    return actual==expected;
}
void put(std::vector<uint8_t>& bytes,uint32_t offset,uint32_t value) {
    for (unsigned i=0;i<4;++i) bytes.at(offset+i)=uint8_t(value>>(i*8));
}
}
bool UsbdStorage::prepare() {
    std::lock_guard lock(mutex_);
    if (addresses_[0]) return true;
    if (preparing_ || !verified(host_)) return false;
    preparing_=true;
    struct Finish { bool& flag; ~Finish() { flag=false; } } finish{preparing_};
    const auto before=generation_;
    struct Rollback {
        IopHost& host; std::array<uint32_t,3> owned{};
        ~Rollback() { for (auto a:owned) if (a) host.freeIopMemory(a); }
    } rollback{host_};
    auto& owned=rollback.owned;
    owned[0]=host_.allocateIopMemory(ImageBytes);
    owned[1]=host_.allocateIopMemory(ArenaBytes+0xff); // Own slack for explicit HCCA alignment.
    owned[2]=host_.allocateIopMemory(HubBytes);
    if (!owned[0] || !owned[1] || !owned[2] || owned[0]>UINT32_MAX-ImageBytes ||
        owned[1]>UINT32_MAX-(ArenaBytes+0xff) || owned[2]>UINT32_MAX-HubBytes) return false;
    const auto imageBase=owned[0],arenaBase=(owned[1]+0xff)&~0xffu,hubBase=owned[2];
    std::vector<uint8_t> image(ImageBytes),arena(ArenaBytes),hubs(HubBytes);
    constexpr std::array<uint32_t,12> defaults{32,64,128,128,256,512,8,8,0,30,36,0};
    for (uint32_t i=0;i<defaults.size();++i) put(image,0x5880+i*4,defaults[i]);
    for (uint32_t a=0x57e8;a<=0x57f8;a+=4) put(image,a,UINT32_MAX); // Not live kernel handles.
    // Layout from original0x32b4..0x344c, for verified zero-argument defaults.
    constexpr uint32_t itd=0x100,td=0x1100,ed=0x1900,periodic=0x1d00,control=0x2120,
        endpoints=0x21ac,devices=0x34ac,requests=0x47ac,itdLookup=0x87ac,tdLookup=0x89ac,descriptors=0x8bac;
    auto p=[&](uint32_t field,uint32_t offset) { put(arena,control+field,arenaBase+offset); };
    put(image,0x57c0,arenaBase+control); put(image,0x58ac,arenaBase+control);
    put(image,0x5780,hubBase); put(image,0x58a0,ArenaBytes+HubBytes);
    put(arena,itd-4,imageBase+0x5880);
    put(arena,control,0xbf801600); // Symbolic original OHCI address, never dereferenced on host.
    put(arena,control+4,(arenaBase&0x1fffffff)|0xa0000000); // Original uncached alias metadata.
    p(8,periodic); p(0x24,tdLookup); p(0x28,itdLookup); p(0x44,endpoints);
    p(0x38,devices); p(0x2c,requests);
    // Endpoint software records and associated hardware EDs; doubly linked free queue.
    p(0x48,endpoints); p(0x4c,endpoints+63*0x4c);
    for (uint32_t i=0;i<64;++i) {
        const auto at=endpoints+i*0x4c;
        put(arena,at,i); put(arena,at+0x24,arenaBase+ed+i*16);
        put(arena,at+8,i<63?arenaBase+at+0x4c:0);
        put(arena,at+12,i?arenaBase+at-0x4c:0);
    }
    // Device records have per-device descriptor storage. Reserve root0 and its
    // two port nodes exactly as the three original0xd70 calls do; no attachment.
    p(0x3c,devices+3*0x98); p(0x40,devices+31*0x98); p(0x88,devices);
    for (uint32_t i=0;i<32;++i) {
        const auto at=devices+i*0x98;
        put(arena,at,i); arena.at(at+0x19)=uint8_t(i);
        put(arena,at+0x8c,arenaBase+descriptors+i*512);
        put(arena,at+4,i<31?arenaBase+at+0x98:0);
        put(arena,at+8,i?arenaBase+at-0x98:0);
        if (i<3) {
            arena.at(at+0x18)=1; put(arena,at+0x28,i);
            if (i) put(arena,at+0x24,arenaBase+devices);
        }
    }
    put(arena,devices+0x1c,arenaBase+devices+0x98);
    put(arena,devices+0x20,arenaBase+devices+2*0x98);
    put(arena,devices+0x98+8,0); put(arena,devices+2*0x98+4,0); put(arena,devices+3*0x98+8,0);
    p(0x30,requests); p(0x34,requests+255*0x40);
    for (uint32_t i=0;i<256;++i) {
        const auto at=requests+i*0x40; put(arena,at,i);
        put(arena,at+8,i<255?arenaBase+at+0x40:0); put(arena,at+12,i?arenaBase+at-0x40:0);
    }
    p(0xc,td); p(0x10,td); p(0x14,td+128*16);
    p(0x18,itd); p(0x1c,itd); p(0x20,itd+128*32);
    for (uint32_t i=0;i<127;++i) {
        put(arena,td+i*16+8,arenaBase+td+(i+1)*16);
        put(arena,itd+i*32+8,arenaBase+itd+(i+1)*32);
    }
    // 63 skipped periodic EDs, bit-reversed HCCA leaf table, and three skipped
    // sentinel EDs. The root's next pointer is the third sentinel (original377c).
    for (uint32_t i=0;i<66;++i) {
        const auto at=periodic+i*16; put(arena,at,0x4000);
        if (i && i<63) put(arena,at+12,arenaBase+periodic+((i-1)/2)*16);
        if (i>=31 && i<63) {
            const auto leaf=i-31;
            const auto reversed=((leaf&1)<<4)|((leaf&2)<<2)|(leaf&4)|((leaf&8)>>2)|((leaf&16)>>4);
            put(arena,reversed*4,arenaBase+at);
        }
    }
    put(arena,periodic+12,arenaBase+periodic+65*16);
    for (uint32_t i=0;i<8;++i) put(hubs,i*0xf8,i<7?hubBase+(i+1)*0xf8:0);
    const std::array<IopMemoryWrite,3> writes{{{imageBase,image},{arenaBase,arena},{hubBase,hubs}}};
    if (!host_.writeIopMemoryBatch(writes) || generation_!=before) return false;
    allocated_=owned; addresses_={imageBase,arenaBase,hubBase}; owned={}; return true;
}
void UsbdStorage::reset() {
    std::lock_guard lock(mutex_); ++generation_;
    for (auto a:allocated_) if (a) host_.freeIopMemory(a);
    allocated_={}; addresses_={};
}
std::array<uint32_t,3> UsbdStorage::addresses() const { std::lock_guard lock(mutex_); return addresses_; }
}
