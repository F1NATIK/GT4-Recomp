#pragma once
#include "ps2x/iop/gt3/sio2_resident.h"
namespace ps2x::iop::gt3 {
// Native SIO2 resource model, not an executing IOP kernel/MMIO controller.
// Owns one backend per runtime registry, a queued exchange worker, DMA staging
// and interrupt-completion notification. Guest result writes stay with exports.
// Lifecycle operations use the controlling thread; finite device exchanges and
// host/registry lifetime beyond this backend are required.
class Sio2NativeBackend final : public Sio2ResidentBackend,public Sio2Device {
public:
    Sio2NativeBackend(IopHost&,std::shared_ptr<Sio2Device> source);
    ~Sio2NativeBackend();
    bool acquire() override;
    bool configure(const Sio2StartupConfig&) override;
    bool startWorker() override;
    void stopWorker() noexcept override;
    void release() noexcept override;
    bool exchange(const Sio2Transfer&,Sio2Completion&) override;
private:
    struct State;
    std::shared_ptr<State> state_;
};
}
