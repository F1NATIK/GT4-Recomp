#include "ps2x/iop/gt3/ilink_startup_image.h"
#include <algorithm>
#include <vector>

namespace ps2x::iop::gt3 {
namespace {
uint32_t word(std::span<const uint8_t> bytes,size_t at) {
    uint32_t value=0; for (unsigned i=0;i<4;++i) value|=uint32_t(bytes[at+i])<<(8*i); return value;
}
void put(std::span<uint8_t> bytes,size_t at,uint32_t value) {
    for (unsigned i=0;i<4;++i) bytes[at+i]=uint8_t(value>>(8*i));
}
std::vector<uint8_t> original(IopHost& host) {
    const auto file=host.openHostFile(host.translateGuestPath("cdrom0:\\ILINK.IRX;1")); if (!file) return {};
    struct Close { IopHost& host; uint64_t file; ~Close() { host.closeHostFile(file); } } close{host,file};
    uint64_t size=0; size_t count=0; std::array<uint8_t,32> digest{};
    if (!host.hostFileSize(file,size) || size!=141493) return {};
    std::vector<uint8_t> bytes(static_cast<size_t>(size));
    if (!host.readHostFile(file,0,bytes.data(),bytes.size(),count) || count!=bytes.size() || !host.sha256(bytes,digest)) return {};
    constexpr std::string_view expected="d4e054ecfc8c5ea0b6647f2674455ec0592b65456a3b70d25baef4db084d5cd4";
    constexpr char hex[]="0123456789abcdef"; std::string actual;
    for (auto value:digest) { actual+=hex[value>>4]; actual+=hex[value&15]; }
    return actual==expected?std::move(bytes):std::vector<uint8_t>{};
}
}
bool verifyIlinkModule(IopHost& host) { return !original(host).empty(); }
bool initializeIlinkImage(IopHost& host,uint32_t image) {
    if (!image || (image&3) || image>UINT32_MAX-IlinkOriginalModelBytes) return false;
    const auto source=original(host); if (source.empty()) return false;
    std::vector<uint8_t> bytes(IlinkOriginalModelBytes),check(bytes.size());
    if (!host.readIopMemory(image,check.data(),check.size())) return false;
    std::copy_n(source.begin()+0x14750,0x2400,bytes.begin()+0x146b0);
    std::copy_n(source.begin()+0x16b50,0x7a0,bytes.begin()+0x16ab0);
    for (size_t at=0x21c14;at<0x228a4;at+=8) {
        const auto offset=word(source,at),info=word(source,at+4);
        if (info!=2 || (offset&3) || offset<0x146b0 || offset>0x17250-4) return false;
        const auto pointer=word(bytes,offset); if (pointer>=IlinkOriginalModelBytes) return false;
        put(bytes,offset,image+pointer);
    }
    const IopMemoryWrite write{image,bytes};
    return host.writeIopMemoryBatch(std::span(&write,1)) && host.readIopMemory(image,check.data(),check.size()) && check==bytes;
}
}
