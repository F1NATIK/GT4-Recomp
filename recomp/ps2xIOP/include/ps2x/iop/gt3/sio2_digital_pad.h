#pragma once
#include "ps2x/iop/gt3/sio2_transfer.h"
#include <memory>
namespace ps2x::iop::gt3 {
// Virtual port0 controller fed by host snapshots, with absent memory cards
// and multitap. The basic digital pad answers polls only. The DualShock 2
// follows the public controller protocol (digital 41, analog 73, pressure 79,
// configuration commands 40..4f) and reports its motors to the host. Neither
// is a physical connection claim.
std::shared_ptr<Sio2Device> createSio2DigitalPad(IopHost&);
std::shared_ptr<Sio2Device> createSio2DualShock2(IopHost&);
class Sio2DigitalPadClient {
public:
    explicit Sio2DigitalPadClient(IopHost& host):host_(host) {}
    ~Sio2DigitalPadClient() { reset(); }
    Sio2DigitalPadClient(const Sio2DigitalPadClient&)=delete;
    Sio2DigitalPadClient& operator=(const Sio2DigitalPadClient&)=delete;
    bool poll(std::array<uint8_t,4>& packet);
    void reset();
private:
    IopHost& host_;
    uint32_t storage_=0;
};
}
