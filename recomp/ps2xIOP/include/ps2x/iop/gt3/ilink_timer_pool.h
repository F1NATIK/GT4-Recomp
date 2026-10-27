#pragma once
#include "ps2x/iop/iop_host.h"
#include <array>
#include <memory>
#include <optional>
#include <stop_token>

namespace ps2x::iop::gt3 {
// Original64fc/65c8/6730 cold timer storage and general-slot allocation.
// Caller exclusively owns a prepared, unpublished ILINK image. Retain it until
// stop, then discard/reprepare it: failed commits and stop can leave metadata.
// The two extra contexts are reserved from general allocation. Timers stay
// unarmed; alarm scheduling, callbacks, export4 and networking are unsupported.
// Never mutate context memory through a lease; future arming needs ownership
// and callback cancellation before this pool can safely free its resources.
class IlinkTimerPool {
public:
    static constexpr uint32_t ContextCount=64,Capacity=62,ContextBytes=12;
    struct Lease { uint32_t index=0,context=0; uint64_t token=0; };
    struct Snapshot {
        uint32_t image=0,event=0,eventHandle=0,freeCount=0,waiters=0;
        std::array<uint32_t,ContextCount> contexts{};
        int32_t cursor=0;
        uint64_t generation=0,signals=0;
    };
    explicit IlinkTimerPool(IopHost&);
    ~IlinkTimerPool();
    IlinkTimerPool(const IlinkTimerPool&)=delete;
    IlinkTimerPool& operator=(const IlinkTimerPool&)=delete;
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
