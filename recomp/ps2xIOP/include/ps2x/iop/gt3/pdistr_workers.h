#pragma once
#include "ps2x/iop/iop_host.h"
#include <array>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <thread>

namespace ps2x::iop::gt3 {
class Pdispu2Resident;
// Caller exclusively owns the prepared unpublished image and retains it until
// stop completes. Failed startup may leave metadata; discard/reprepare image.
class PdistrWorkers {
public:
    static constexpr uint32_t ModelBytes=0x3800;
    struct Reply { bool hasData=false; uint32_t value=0; };
    struct Snapshot {
        std::array<uint32_t,5> stacks{},threads{};
        std::array<uint32_t,4> semaphores{};
        std::array<std::thread::id,5> workerIds{};
        std::array<uint64_t,4> initialQueries{};
    };
    explicit PdistrWorkers(IopHost&);
    ~PdistrWorkers();
    bool start(uint32_t image);
    bool stop();
    bool active() const;
    std::optional<Snapshot> snapshot() const;
    // Audited RPC function0 only. The first three queues return a null result;
    // only SID50444950 returns module version106. No stream jobs implemented.
    std::optional<Reply> queryInitial(uint32_t sid);
    std::optional<Reply> queryIdleCancel(uint32_t sid);
    // RPC3's original 0x119c path is a null result while active3088 is zero.
    // Active transfers still require the unimplemented dependency at0x26bc.
    std::optional<Reply> queryIdleMpegControl(uint32_t sid);
    // Serial BGM command0, through both original worker roles and semaphores.
    // The sound owner must outlive this synchronous call.
    std::optional<Reply> queryBgmVolume(uint32_t value,Pdispu2Resident& sound);
    // BGM RPC4/RPC5 = commands 2/3: PDISPU2 export47/48 pause/resume on the
    // BGM timing object 2d90. Resume with a recorded waiter is refused.
    std::optional<Reply> queryBgmPause(bool pause);
    // BGM RPC2 = command 4 (stop, 49c): a no-op while BGM is idle (2af8 == 0);
    // refused while it plays, since native BGM playback does not exist yet.
    std::optional<Reply> queryBgmStop();
    // BGM RPC1 (a70): the 128 request bytes go to 2afc, then command 1. The
    // worker's 8f4 snapshot (2b80) is handed to the owner's handler, which
    // performs 9d8/4e8; on success the worker sets the playing byte 2e75.
    // Command 4 (stop) also goes to the handler when one is set. The handler
    // runs on the command worker while the RPC caller waits; it must not
    // re-enter this object. Set it while no BGM command is in flight.
    using BgmHandler=std::function<bool(uint8_t command,std::span<const uint8_t> parameters)>;
    void setBgmHandler(BgmHandler);
    std::optional<Reply> queryBgmStart(const std::array<uint8_t,128>& parameters);
    bool cancelIdle();
private:
    struct State;
    std::shared_ptr<State> state_;
    std::optional<Reply> query(uint32_t sid,uint32_t function,uint32_t value=0,Pdispu2Resident* sound=nullptr);
};
}
