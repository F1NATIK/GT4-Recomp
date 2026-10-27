#pragma once
#include "ps2x/iop/gt3/sio2_transfer.h"
#include <memory>
namespace ps2x::iop::gt3 {
// Native absent-multitap profile only. Caller verifies the supplied IRX hash
// before activation. Native callback addresses are reserved IOP model storage,
// not executable original IRX code. Host/registry/device must outlive this object.
class MtapResident {
public:
    MtapResident(IopHost&,std::shared_ptr<Sio2Device> bus);
    ~MtapResident();
    MtapResident(const MtapResident&)=delete;
    MtapResident& operator=(const MtapResident&)=delete;
    bool start();
    void reset();
    bool active() const;
    RpcResult handleRpc(const RpcRequest&);
private:
    struct State;
    std::shared_ptr<State> state_;
};
}
