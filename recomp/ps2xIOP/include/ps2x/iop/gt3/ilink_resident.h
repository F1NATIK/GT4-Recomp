#pragma once
#include "ps2x/iop/iop_host.h"
#include <memory>

namespace ps2x::iop::gt3 {
// Supplied ILINK entry registers its library only. Separate hardware/network
// initialization is deliberately absent; modeled queries retain error -1000.
class IlinkResident {
public:
    explicit IlinkResident(IopHost&);
    ~IlinkResident();
    bool start();
    bool active() const;
    bool reset();
private:
    struct State;
    std::shared_ptr<State> state_;
};
}
