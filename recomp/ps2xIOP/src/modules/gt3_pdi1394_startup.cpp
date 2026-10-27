#include "ps2x/iop/gt3/pdi1394_startup.h"
#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <vector>

namespace ps2x::iop::gt3 {
namespace {
void put(std::span<uint8_t> bytes,size_t offset,uint32_t value) {
    for (unsigned i=0;i<4;++i) bytes[offset+i]=uint8_t(value>>(i*8));
}
std::vector<uint8_t> originalModule(IopHost& host) {
    const auto file=host.openHostFile(host.translateGuestPath("cdrom0:\\PDI1394.IRX;1")); if (!file) return {};
    struct Close { IopHost& host; uint64_t file; ~Close() { host.closeHostFile(file); } } close{host,file};
    uint64_t size=0; size_t count=0; std::array<uint8_t,32> digest{};
    if (!host.hostFileSize(file,size) || size!=13945) return {};
    std::vector<uint8_t> bytes(static_cast<size_t>(size));
    if (!host.readHostFile(file,0,bytes.data(),bytes.size(),count) || count!=bytes.size() || !host.sha256(bytes,digest)) return {};
    constexpr std::string_view expected="72036438c02856ee2d3a6d01fbc4149b9be2fc3a52abb435717792193c421f3f";
    constexpr char hex[]="0123456789abcdef"; std::string actual;
    for (auto byte:digest) { actual+=hex[byte>>4]; actual+=hex[byte&15]; }
    return actual==expected?std::move(bytes):std::vector<uint8_t>{};
}
}
bool initializePdi1394Image(IopHost& host,uint32_t image) {
    if (!image || (image&3) || image>UINT32_MAX-Pdi1394OriginalModelBytes) return false;
    const auto original=originalModule(host); if (original.empty()) return false;
    std::vector<uint8_t> bytes(Pdi1394OriginalModelBytes),check(bytes.size());
    if (!host.readIopMemory(image,check.data(),check.size())) return false;
    std::copy_n(original.begin()+0x2840,0xb0,bytes.begin()+0x27a0);
    std::copy_n(original.begin()+0x28f0,0x10,bytes.begin()+0x2850);
    // The sole non-text relocation is the original module-name pointer.
    put(bytes,0x2850,image+0x27a0);
    put(bytes,0x2860,0xac1dcafe);
    for (unsigned i=0;i<8;++i) put(bytes,0x3ce0+i*0x28,UINT32_MAX);
    const std::array<IopMemoryWrite,1> writes{{{image,bytes}}};
    return host.writeIopMemoryBatch(writes) && host.readIopMemory(image,check.data(),check.size()) && check==bytes;
}
Pdi1394EeWaitResult waitForPdi1394EeService(IopHost& host,std::stop_token stop) {
    Pdi1394EeWaitResult result;
    std::mutex mutex; std::condition_variable_any wake; std::unique_lock lock(mutex);
    while (!stop.stop_requested()) {
        const auto server=host.findEeRpcServer(0x45574650); ++result.attempts;
        if (!server.supported) return result;
        if (server.sid!=0x45574650 || (server.registration && (!server.server || !server.function || (server.server&3) || (server.function&3)))) {
            result.status=Pdi1394EeWaitStatus::InvalidSnapshot; return result;
        }
        // Original performs DelayThread(1000) even after a successful bind.
        wake.wait_for(lock,stop,std::chrono::microseconds(1000),[] { return false; });
        if (stop.stop_requested()) break;
        if (server.registration) {
            const auto current=host.findEeRpcServer(0x45574650);
            if (!current.supported) return result;
            if (current.registration==server.registration && current.server==server.server && current.function==server.function &&
                current.buffer==server.buffer && current.queue==server.queue && current.sid==server.sid) {
                result.status=Pdi1394EeWaitStatus::Available; result.server=server; return result;
            }
        }
    }
    result.status=Pdi1394EeWaitStatus::Cancelled; return result;
}
}
