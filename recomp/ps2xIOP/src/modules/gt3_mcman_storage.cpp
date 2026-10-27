#include "ps2x/iop/gt3/mcman_storage.h"
#include <array>
#include <vector>
namespace ps2x::iop::gt3 {
bool McmanStorage::prepare() {
    std::lock_guard lock(mutex_);
    if (base_) return true;
    const auto allocated=host_.allocateIopMemory(Size);
    if (!allocated) return false;
    struct Rollback { IopHost& host; uint32_t address;
        ~Rollback() { if (address) host.freeIopMemory(address); } } rollback{host_,allocated};
    if (allocated>UINT32_MAX-Size) return false;
    std::vector<uint8_t> bytes(Size);
    auto put=[&](uint32_t offset,uint32_t value) {
        for (unsigned i=0;i<4;++i) bytes[offset+i]=uint8_t(value>>(8*i));
    };
    // Original 0xce70: 36 cache entries/data pointers and reversed free list.
    for (uint32_t index=0;index<36;++index) {
        put(0x1bc40+index*16,UINT32_MAX);
        put(0x1bc44+index*16,allocated+0x12c40+index*0x400);
        put(0x1be80+index*4,allocated+0x1bc40+(35-index)*16);
    }
    put(0x23f34,allocated+0x1bc40); put(0x23f38,allocated+0x1be80);
    for (uint32_t index=0;index<4;++index) {
        for (auto field:{0x24574u,0x24578u,0x2457cu,0x24580u,0x24b84u,0x24b88u,0x24b8cu,0x24b90u})
            put(field+index*0x184,UINT32_MAX);
    }
    // Directory-cache invalid entries, with the original per-port count zero.
    for (uint32_t offset=0x1ff10;offset<0x23f30;++offset) bytes[offset]=0xff;
    for (uint32_t index=0;index<4;++index) {
        put(0x1ff10+index*0x804,0); put(0x21f20+index*0x804,0);
    }
    // One atomic IOP-only commit; a bad allocation cannot initialize EE RAM.
    const std::array<IopMemoryWrite,1> writes{{{allocated,bytes}}};
    if (!host_.writeIopMemoryBatch(writes)) return false;
    base_=allocated; rollback.address=0; return true;
}
void McmanStorage::reset() {
    std::lock_guard lock(mutex_);
    if (base_) host_.freeIopMemory(base_);
    base_=0;
}
uint32_t McmanStorage::base() const { std::lock_guard lock(mutex_); return base_; }
}
