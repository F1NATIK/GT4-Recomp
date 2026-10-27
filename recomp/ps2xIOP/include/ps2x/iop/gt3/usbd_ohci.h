#pragma once
#include "ps2x/iop/gt3/usbd_storage.h"
#include <optional>

namespace ps2x::iop::gt3 {
// Native two-port, disconnected OHCI subset. No host USB access, resident
// publication, IRQ worker, transfer execution or autonomous clock.
class UsbdOhci {
public:
    explicit UsbdOhci(IopHost& host):host_(host),storage_(host) {}
    ~UsbdOhci() { reset(); }
    bool start();
    void reset();
    bool active() const;
    std::array<uint32_t,3> addresses() const;
    std::optional<uint32_t> read(uint32_t address) const;
    bool write(uint32_t address,uint32_t value);
    bool irqPending() const;
    // One externally scheduled frame boundary. Only skipped lists are supported.
    // Failed validation/commit leaves the frame number, HCCA and IRQ unchanged.
    bool advanceFrame();
    static constexpr uint32_t Base=0xbf801600;
private:
    struct State { std::array<uint32_t,0x5c/4> reg{}; } state_;
    bool validStorage() const;
    bool skippedList(uint32_t address) const;
    bool applyWrite(State& state,uint32_t offset,uint32_t value) const;
    static void softwareReset(State& state);
    IopHost& host_;
    UsbdStorage storage_;
    mutable std::recursive_mutex mutex_;
    bool active_=false,inFlight_=false;
    uint64_t generation_=0;
};
}
