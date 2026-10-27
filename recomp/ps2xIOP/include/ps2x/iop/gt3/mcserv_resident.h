#pragma once
#include "ps2x/iop/iop_host.h"
#include <memory>
namespace ps2x::iop::gt3 {
class McmanNativeExports;
// Supplied MCSERV: scoped IOP image/stack and native RPC worker. Ports with a
// card image (McmanNativeExports::card) run every RPC against the native card
// with the original request/reply layouts; others keep the absent-card
// profile. start/reset/handleRpc are serialized on the controlling thread.
class McservResident {
public:
    explicit McservResident(IopHost&,McmanNativeExports* cards=nullptr);
    ~McservResident();
    bool start();
    bool active() const;
    void reset();
    RpcResult handleRpc(const RpcRequest&);
private:
    struct State;
    std::unique_ptr<State> state_;
};
}
