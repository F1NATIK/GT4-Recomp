#include "ps2x/iop/gt3/pdispu2_sound_setup.h"
#include "gt3_pdispu2_sound_sequence.h"
#include <algorithm>
#include <chrono>
#include <thread>

namespace ps2x::iop::gt3 {
std::vector<uint8_t> readPdispu2Module(IopHost& host) {
    const auto file=host.openHostFile(host.translateGuestPath("cdrom0:\\PDISPU2.IRX;1")); if (!file) return {};
    struct Close { IopHost& host; uint64_t file; ~Close() { host.closeHostFile(file); } } close{host,file};
    uint64_t size=0; size_t count=0; std::array<uint8_t,32> digest{};
    if (!host.hostFileSize(file,size) || size<52 || size>1024*1024) return {};
    std::vector<uint8_t> bytes(static_cast<size_t>(size));
    if (!host.readHostFile(file,0,bytes.data(),bytes.size(),count) || count!=bytes.size() || !host.sha256(bytes,digest)) return {};
    constexpr std::string_view expected="64dbe3eccc32f18bdcd53a7c621bf5d0c962659a85d0141d82c2ddeb502139f1";
    constexpr char hex[]="0123456789abcdef"; std::string actual;
    for (auto byte:digest) { actual+=hex[byte>>4]; actual+=hex[byte&15]; }
    return actual==expected?std::move(bytes):std::vector<uint8_t>{};
}
bool verifyPdispu2Module(IopHost& host) { return !readPdispu2Module(host).empty(); }
bool initializePdispu2Sound(IopHost& host,LibsdResident& sound,uint32_t image,uint64_t& token) {
    auto* registry=host.nativeLibraries();
    if (!registry || token || !image || image>UINT32_MAX-0x4040 || (image&3) || !verifyPdispu2Module(host)) return false;
    std::vector<uint8_t> module(0x4040);
    if (!host.readIopMemory(image,module.data(),module.size()) ||
        !std::all_of(module.begin()+0x3830,module.begin()+0x3840,[](uint8_t b) { return b==0; })) return false;
    const auto callback=registry->bindCallback(image+0x9d8); if (!callback.callback) return false;
    std::array<NativeImportBinding,28> imports{};
    for (unsigned ordinal:{4,5,7,9,23,25,27}) {
        imports[ordinal]=registry->bind("libsd",0x104,uint16_t(ordinal));
        if (!imports[ordinal].registration || imports[ordinal].registration!=imports[4].registration) return false;
    }
    const auto descriptor=host.allocateIopMemory(40); if (!descriptor) return false;
    struct Release { IopHost& host; uint32_t address; ~Release() { host.freeIopMemory(address); } } release{host,descriptor};
    std::array<uint8_t,40> descriptors{}; descriptors[20]=1;
    const std::array<IopMemoryWrite,1> writes{{{descriptor,descriptors}}};
    if (!host.writeIopMemoryBatch(writes)) return false;
    token=sound.beginSoundStartup(); if (!token) return false;
    for (const auto& command:pdispu2SoundCommands) {
        if (!command.ordinal) {
            std::this_thread::sleep_for(std::chrono::microseconds(command.a)); continue;
        }
        std::array<uint32_t,4> args{command.a,command.b,command.c,0};
        if (command.ordinal==23) args[1]=descriptor+command.a*20;
        else if (command.ordinal==27) {
            const auto current=registry->bindCallback(image+0x9d8);
            if (current.callback!=callback.callback || current.registration!=callback.registration) return false;
            args[0]=image+0x9d8; args[1]=image+0x3830;
        }
        const auto result=registry->invoke(imports[command.ordinal],args);
        if (!result.handled || ((command.ordinal==4 || command.ordinal==23 || command.ordinal==25 || command.ordinal==27) && result.value!=0)) return false;
    }
    return sound.soundInitialized();
}
NativeExportResult pdispu2InitialSoundInterrupt(IopHost& host,uint32_t image,const std::array<uint32_t,4>& args) {
    auto* registry=host.nativeLibraries();
    if (!registry || !image || image>UINT32_MAX-0x3840 || (image&3) || args[1]!=image+0x3830 || !args[0] || (args[0]&~3u)) return {};
    std::array<uint32_t,4> callbacks{};
    if (!host.readIopMemory(image+0x3830,callbacks.data(),sizeof(callbacks)) || callbacks!=std::array<uint32_t,4>{}) return {};
    const auto enable=registry->bind("libsd",0x104,11); if (!enable.registration) return {};
    for (unsigned core=0;core<2;++core) if (args[0]&(1u<<core)) {
        if (!registry->invoke(enable,{4|core,0,0,0}).handled || !registry->invoke(enable,{4|core,1,0,0}).handled) return {};
    }
    return {true,0};
}
}
