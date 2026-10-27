#include "ps2x/iop/gt3/pdistr_startup_image.h"
#include "ps2x/iop/native_library.h"
#include <algorithm>
#include <vector>

namespace ps2x::iop::gt3 {
namespace {
uint32_t word(std::span<const uint8_t> bytes,size_t offset) {
    uint32_t value=0; for (unsigned i=0;i<4;++i) value|=uint32_t(bytes[offset+i])<<(i*8); return value;
}
void put(std::span<uint8_t> bytes,size_t offset,uint32_t value) {
    for (unsigned i=0;i<4;++i) bytes[offset+i]=uint8_t(value>>(i*8));
}
std::vector<uint8_t> originalModule(IopHost& host) {
    const auto file=host.openHostFile(host.translateGuestPath("cdrom0:\\PDISTR.IRX;1")); if (!file) return {};
    struct Close { IopHost& host; uint64_t file; ~Close() { host.closeHostFile(file); } } close{host,file};
    uint64_t size=0; size_t count=0; std::array<uint8_t,32> digest{};
    if (!host.hostFileSize(file,size) || size!=15973) return {};
    std::vector<uint8_t> bytes(static_cast<size_t>(size));
    if (!host.readHostFile(file,0,bytes.data(),bytes.size(),count) || count!=bytes.size() || !host.sha256(bytes,digest)) return {};
    constexpr std::string_view expected="5fbc910a9a5e266ac348182c4583207b8c2c771d04fbf87cb0abffee3ef2d191";
    constexpr char hex[]="0123456789abcdef"; std::string actual;
    for (auto byte:digest) { actual+=hex[byte>>4]; actual+=hex[byte&15]; }
    return actual==expected?std::move(bytes):std::vector<uint8_t>{};
}
}
bool verifyPdistrModule(IopHost& host) { return !originalModule(host).empty(); }
bool initializePdistrImage(IopHost& host,uint32_t image) {
    auto* registry=host.nativeLibraries();
    if (!registry || !image || (image&3) || image>UINT32_MAX-PdistrOriginalModelBytes) return false;
    const auto original=originalModule(host); if (original.empty()) return false;
    const auto volume=registry->bind("pdispu2",0x109,4); if (!volume.registration) return false;
    std::vector<uint8_t> bytes(PdistrOriginalModelBytes),check(bytes.size());
    if (!host.readIopMemory(image,check.data(),check.size())) return false;
    std::copy_n(original.begin()+0x28d0,0x1d0,bytes.begin()+0x2820);
    std::copy_n(original.begin()+0x2aa0,0x20,bytes.begin()+0x29f0);
    // Full-file hash fixes both relocation sections. Text pointers name future
    // explicit native functions; original text is never copied or executed.
    for (size_t at=0x3b74;at<0x3e54;at+=8) {
        const auto offset=word(original,at),type=word(original,at+4)&255;
        if (type!=2 || offset<0x2820 || offset>0x2a0c || (offset&3)) return false;
        put(bytes,offset,word(bytes,offset)+image);
    }
    const std::array<IopMemoryWrite,1> initial{{{image,bytes}}};
    if (!host.writeIopMemoryBatch(initial) || !host.readIopMemory(image,check.data(),check.size()) || check!=bytes) return false;
    const auto result=registry->invoke(volume,{image+0x2c00,0,0,0});
    if (!result.handled || registry->bind("pdispu2",0x109,4).registration!=volume.registration) return false;
    for (size_t offset=0x2c00;offset<0x2c10;offset+=4) put(bytes,offset,0x8000);
    if (!host.readIopMemory(image,check.data(),check.size()) || check!=bytes) return false;
    put(bytes,0x2c14,image+0x2c00);
    const std::array<IopMemoryWrite,1> pointer{{{image+0x2c14,std::span<const uint8_t>(bytes).subspan(0x2c14,4)}}};
    return host.writeIopMemoryBatch(pointer) && host.readIopMemory(image,check.data(),check.size()) && check==bytes;
}
}
