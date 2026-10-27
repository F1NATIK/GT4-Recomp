#pragma once
#include "ps2x/iop/iop_host.h"
#include <memory>

namespace ps2x::iop::gt3 {
// Supplied PADMAN residency and bounded basic-digital input. The owner must
// opt into frame delivery before initialization/open RPCs are enabled.
// Lifecycle, frame notifications and requests use the controlling thread.
class PadmanResident {
public:
    explicit PadmanResident(IopHost&,bool frameEvents=false);
    ~PadmanResident();
    bool start();
    bool active() const;
    void reset();
    RpcResult handleRpc(const RpcRequest&);
    void onVBlank(bool start,uint64_t tick);
private:
    struct State;
    std::unique_ptr<State> state_;
};
}
