#pragma once
#include "ps2x/iop/iop_host.h"
#include <memory>
#include <functional>

namespace ps2x::iop::gt3 {
class LibpdiResident {
public:
    // Optional native IOP uptime source for deterministic fixtures. Must return
    // promptly and cannot reenter registry/lifecycle operations.
    using SecondsSource=std::function<uint64_t()>;
    explicit LibpdiResident(IopHost&,SecondsSource={});
    ~LibpdiResident();
    bool start();
    bool active() const;
    bool reset();
private:
    struct State;
    std::shared_ptr<State> state_;
};
}
