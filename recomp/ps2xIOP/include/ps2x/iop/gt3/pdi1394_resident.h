#pragma once
#include "ps2x/iop/iop_host.h"
#include "ps2x/iop/gt3/pdi1394_link.h"
#include <memory>
#include <optional>

namespace ps2x::iop::gt3 {
// PDI1394 (i.LINK battle) resident: the hash-verified original module image
// and library registration, with the protocol implemented natively by
// Pdi1394Link over the network (LAN/online) instead of an IEEE 1394 bus.
class Pdi1394Resident {
public:
    static constexpr uint32_t ModelBytes=0x5000,ReplyBytes=Pdi1394Link::ReplyBytes;
    enum class Phase { Stopped,Ready };
    struct Snapshot {
        uint32_t image=0,replyPacket=0;
        Phase phase=Phase::Stopped;
        Pdi1394Link::Status link;
    };
    explicit Pdi1394Resident(IopHost&);
    ~Pdi1394Resident();
    Pdi1394Resident(const Pdi1394Resident&)=delete;
    Pdi1394Resident& operator=(const Pdi1394Resident&)=delete;
    bool start();
    bool reset();
    bool active() const;
    std::optional<Snapshot> snapshot() const;
    RpcResult handleRpc(const RpcRequest&);
    void onVBlank(bool start,uint64_t tick);
    void onSifTransfer(const SifTransfer&);
private:
    struct State;
    std::shared_ptr<State> state_;
};
}
