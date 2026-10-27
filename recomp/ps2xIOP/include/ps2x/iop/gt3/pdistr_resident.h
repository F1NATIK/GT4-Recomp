#pragma once
#include "ps2x/iop/iop_host.h"
#include "ps2x/iop/gt3/pdispu2_resident.h"
#include "ps2x/iop/gt3/pdicdvd_resident.h"
#include <memory>

namespace ps2x::iop::gt3 {
class PdistrResident {
public:
    PdistrResident(IopHost&,Pdispu2Resident&,PdicdvdResident&);
    ~PdistrResident();
    bool start();
    bool reset();
    bool active() const;
    RpcResult handleRpc(const RpcRequest&);
private:
    struct State;
    std::shared_ptr<State> state_;
};
}
