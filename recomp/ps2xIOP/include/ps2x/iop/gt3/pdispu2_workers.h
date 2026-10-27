#pragma once
#include "ps2x/iop/iop_host.h"
#include <memory>
#include <optional>
#include <thread>

namespace ps2x::iop::gt3 {
// Startup resources and RPC0 workers only, not PDISPU2 resident activation.
// Caller owns the verified IOP image and keeps it allocated until stop finishes.
// A failed start leaves the caller's unpublished image for it to discard.
class Pdispu2Workers {
public:
    static constexpr uint32_t ModelBytes=0x4200;
    struct Snapshot {
        std::array<uint32_t,4> stacks{},threads{};
        std::array<uint32_t,8> semaphores{};
        std::array<std::thread::id,4> workerIds{};
        std::array<uint64_t,2> versionQueries{};
    };
    explicit Pdispu2Workers(IopHost&);
    ~Pdispu2Workers();
    bool start(uint32_t image);
    bool stop();
    bool active() const;
    std::optional<Snapshot> snapshot() const;
    std::optional<uint32_t> queryVersion(uint32_t sid);
private:
    struct State;
    std::shared_ptr<State> state_;
};
}
