#pragma once
#include "ps2x/iop/iop_host.h"
#include <array>
#include <memory>
#include <optional>
#include <thread>

namespace ps2x::iop::gt3 {
// Hash-verified original startup and disconnected-device RPC subset only.
class PdiusbResident {
public:
    static constexpr uint32_t ModelBytes=0x3800,ReplyBytes=0x80,Sid=0x594f4a50;
    enum class Phase { Stopped,Ready,MemoryFailure,UnsupportedDevice };
    struct Snapshot {
        uint32_t image=0,context=0,request=0,reply=0;
        std::array<uint32_t,2> stacks{},threads{},semaphores{};
        std::array<std::thread::id,2> workerIds{};
        uint64_t callbackOwner=0,queries=0;
        Phase phase=Phase::Stopped;
    };
    explicit PdiusbResident(IopHost&);
    ~PdiusbResident();
    PdiusbResident(const PdiusbResident&)=delete;
    PdiusbResident& operator=(const PdiusbResident&)=delete;
    bool start();
    bool reset();
    bool active() const;
    std::optional<Snapshot> snapshot() const;
    RpcResult handleRpc(const RpcRequest&);
private:
    struct State;
    std::shared_ptr<State> state_;
};
}
