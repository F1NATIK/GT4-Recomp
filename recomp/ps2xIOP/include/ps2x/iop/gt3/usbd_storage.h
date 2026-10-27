#pragma once
#include "ps2x/iop/iop_host.h"
#include <array>
#include <mutex>

namespace ps2x::iop::gt3 {
// Owned default USBD descriptor pools only. No library publication, OHCI
// controller, root-hub driver registration, events or resident success.
class UsbdStorage {
public:
    explicit UsbdStorage(IopHost& host):host_(host) {}
    ~UsbdStorage() { reset(); }
    bool prepare();
    void reset();
    std::array<uint32_t,3> addresses() const; // Module model, aligned arena, hubs.
    static constexpr uint32_t ImageBytes=0x5900,ArenaBytes=0xcbc8,HubBytes=0x7c0;
private:
    IopHost& host_;
    mutable std::recursive_mutex mutex_;
    std::array<uint32_t,3> allocated_{},addresses_{};
    uint64_t generation_=0;
    bool preparing_=false;
};
}
