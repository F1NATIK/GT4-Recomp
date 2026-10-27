#include "ps2x/iop/gt3/ilink_resident.h"
#include "ps2x/iop/gt3/ilink_startup_image.h"
#include "ps2x/iop/native_library.h"
#include <mutex>

namespace ps2x::iop::gt3 {
namespace {
constexpr uint32_t ImageBytes=0x1a500,InitializationCount=0x175a8;

}
struct IlinkResident::State {
    IopHost& host; NativeLibraryRegistry* registry;
    mutable std::recursive_mutex mutex,lifecycle;
    uint32_t image=0; uint64_t registration=0;
    bool running=false,busy=false;
    explicit State(IopHost& h):host(h),registry(h.nativeLibraries()) {}
    bool uninitialized() const {
        uint32_t count=UINT32_MAX;
        return running && !busy && image && host.readIopMemory(image+InitializationCount,&count,4) && count==0;
    }
    NativeExportResult call() {
        std::lock_guard lock(mutex);
        return uninitialized()?NativeExportResult{true,-1000}:NativeExportResult{};
    }
};
IlinkResident::IlinkResident(IopHost& host):state_(std::make_shared<State>(host)) {}
IlinkResident::~IlinkResident() { reset(); }
bool IlinkResident::active() const {
    auto& s=*state_;
    const auto binding=s.registry?s.registry->bind("ilink",0x202,5):NativeImportBinding{};
    std::lock_guard lock(s.mutex);
    return binding.registration && binding.registration==s.registration && s.uninitialized();
}
bool IlinkResident::start() {
    auto& s=*state_; std::lock_guard lifecycle(s.lifecycle);
    { std::lock_guard lock(s.mutex); if (s.running || s.busy || !s.registry) return false; s.busy=true; }
    struct Transaction {
        State& s; uint32_t image=0; uint64_t registration=0;
        ~Transaction() {
            if (registration) s.registry->unregisterLibrary(registration);
            if (image) s.host.freeIopMemory(image);
            std::lock_guard lock(s.mutex); s.busy=false;
        }
    } transaction{s};
    if (!verifyIlinkModule(s.host)) return false;
    transaction.image=s.host.allocateIopMemory(ImageBytes);
    if (!transaction.image || transaction.image>UINT32_MAX-ImageBytes) return false;
    // Preserve original static data before later initialization is implemented.
    // Text is not executable; native exports still expose uninitialized errors.
    if (!initializeIlinkImage(s.host,transaction.image)) return false;
    std::weak_ptr<State> weak=state_; NativeLibrary library{"ilink",0x202,{}};
    for (uint16_t ordinal:{5,7,8,24,25,26,27,28,31,32}) library.exports.push_back({ordinal,[weak](const auto&) {
        const auto state=weak.lock(); return state?state->call():NativeExportResult{};
    }});
    transaction.registration=s.registry->registerLibrary(std::move(library));
    if (!transaction.registration) return false;
    std::lock_guard lock(s.mutex);
    uint32_t count=UINT32_MAX;
    if (!s.host.readIopMemory(transaction.image+InitializationCount,&count,4) || count) return false;
    s.image=transaction.image; s.registration=transaction.registration; s.running=true;
    transaction.image=0; transaction.registration=0; return true;
}
bool IlinkResident::reset() {
    auto& s=*state_; std::lock_guard lifecycle(s.lifecycle);
    uint32_t image=0; uint64_t registration=0;
    { std::lock_guard lock(s.mutex);
      if (s.busy) return false;
      s.running=false; image=s.image; registration=s.registration; s.image=0; s.registration=0;
    }
    if (registration) s.registry->unregisterLibrary(registration);
    if (image) s.host.freeIopMemory(image);
    return true;
}
}
