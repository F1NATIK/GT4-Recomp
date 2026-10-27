#pragma once
#include "ps2x/iop/gt3/sio2_transfer.h"
#include <memory>

namespace ps2x::iop::gt3 {
// Bounded single-job host worker. Stages device responses on a dedicated thread;
// the caller still validates/commits IOP memory. This supplies no IRQ/DMA backend.
// stop/destruction belong to the controlling thread, never the device callback.
// Device exchange must terminate; stop joins it before releasing its storage.
class Sio2QueuedDevice final : public Sio2Device {
public:
    explicit Sio2QueuedDevice(std::shared_ptr<Sio2Device> device);
    ~Sio2QueuedDevice();
    Sio2QueuedDevice(const Sio2QueuedDevice&)=delete;
    Sio2QueuedDevice& operator=(const Sio2QueuedDevice&)=delete;
    bool exchange(const Sio2Transfer&,Sio2Completion&) override;
    void stop();
private:
    struct State;
    std::unique_ptr<State> state_;
};
}
