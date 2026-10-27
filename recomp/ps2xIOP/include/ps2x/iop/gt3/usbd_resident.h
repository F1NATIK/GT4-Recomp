#pragma once
#include "ps2x/iop/gt3/usbd_workers.h"

namespace ps2x::iop::gt3 {
// Supplied module entry for an explicitly disconnected native USB bus.
// Lifecycle is controlled outside native callbacks/registry dispatch.
class UsbdResident {
public:
    explicit UsbdResident(IopHost&);
    ~UsbdResident();
    bool start(bool realtime=true);
    bool active() const;
    bool reset();
private:
    struct State;
    std::shared_ptr<State> state_;
};
}
