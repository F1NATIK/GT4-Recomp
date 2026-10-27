#pragma once
#include "ps2x/iop/iop_host.h"
#include <memory>
#include <optional>
#include <stop_token>

namespace ps2x::iop::gt3 {
// Original5908/5ad0/5bc8 storage subset. Caller exclusively owns the unpublished
// allocated module image prepared by initializeIlinkImage, retaining it until
// stop, then discarding/repreparing it. Failed startup/commit or stop may leave
// metadata. Stop payload producers before teardown: lease tokens reject stale
// pool calls but cannot protect direct IOP memory writes through raw addresses.
// No ILINK export4 publication, physical link, packet validity or network I/O.
class IlinkPacketPool {
public:
    static constexpr uint32_t ModelBytes=0x1a500,Capacity=16,PayloadBytes=0x814,RecordBytes=0x2c;
    struct Lease { uint32_t record=0,payload=0; uint64_t token=0; };
    struct Snapshot {
        uint32_t image=0,event=0,eventHandle=0,payloads=0,records=0,freeCount=0,waiters=0;
        uint64_t generation=0,signals=0;
    };
    explicit IlinkPacketPool(IopHost&);
    ~IlinkPacketPool();
    bool start(uint32_t image);
    bool stop();
    bool active() const;
    std::optional<Snapshot> snapshot() const;
    std::optional<Lease> tryAcquire();
    std::optional<Lease> acquire(std::stop_token);
    bool release(Lease);
private:
    struct State;
    std::shared_ptr<State> state_;
};
}
