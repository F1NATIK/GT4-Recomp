#pragma once
#include "ps2x/iop/gt3/padman_port_state.h"
#include <memory>

namespace ps2x::iop::gt3 {
// Bounded native port0/slot0 service. Owner must supply a hash-verified PADMAN
// model image and real frame notifications; this does not publish RPCs itself.
// All calls are serialized by that owner on the controlling runtime thread.
class PadmanFrameInput {
public:
    explicit PadmanFrameInput(IopHost&);
    ~PadmanFrameInput();
    bool initialize(uint32_t image,std::span<const IopMemoryWrite> replies={});
    bool open(uint32_t eeDoubleBuffer,std::span<const IopMemoryWrite> replies={});
    bool close(std::span<const IopMemoryWrite> replies={});
    bool end(std::span<const IopMemoryWrite> replies={});
    // Original RPC3..11 (port queries, main mode, actuators, pressure mode,
    // VREF) against the port state. packet is the request, rewritten with the
    // result at the original offset and committed with the state to each
    // address in replyAddresses. Ports other than an open port0/slot0 answer 0.
    bool command(std::array<uint8_t,0x80>& packet,std::span<const uint32_t> replyAddresses);
    void reset();
    bool onVBlank(bool start,uint64_t tick);
    bool initialized() const;
    bool opened() const;
    uint32_t portAddress() const;
private:
    struct State;
    std::unique_ptr<State> state_;
};
}
