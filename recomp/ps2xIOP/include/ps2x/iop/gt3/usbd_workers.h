#pragma once
#include "ps2x/iop/gt3/usbd_ohci.h"
#include <memory>

namespace ps2x::iop::gt3 {
// Owned native USB execution substrate, not a published/resident USB driver.
// Lifecycle must run outside NativeLibraryRegistry handlers. Callbacks may
// enqueue work but must terminate; reset/start from a worker are rejected.
class UsbdWorkers {
public:
    struct Snapshot {
        bool active=false,failed=false;
        uint64_t frames=0,interrupts=0,callbacks=0,callbackFailures=0;
        size_t queued=0;
        std::array<uint32_t,3> addresses{};
        std::array<uint32_t,5> handles{}; // sema, IRQ thread/event, callback thread/event
    };
    explicit UsbdWorkers(IopHost&);
    ~UsbdWorkers();
    bool start(bool realtime=true);
    bool reset();
    Snapshot snapshot() const;
    bool stepFrame(); // Manual-clock mode only.
    std::optional<uint32_t> read(uint32_t address) const;
    bool write(uint32_t address,uint32_t value);
    // Internal completion sink. Does not submit/execute a USB transfer or expose
    // a guest export. Callback gets status, transferred bytes, user argument.
    bool postCompletion(uint32_t callbackAddress,int32_t status,uint32_t transferred,
                        uint32_t argument,uint32_t originalGp=0);
private:
    struct State;
    std::unique_ptr<State> state_;
};
}
