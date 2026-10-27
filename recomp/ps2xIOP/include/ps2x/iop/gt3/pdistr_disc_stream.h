#pragma once
#include "ps2x/iop/gt3/pdicdvd_resident.h"
#include <memory>

namespace ps2x::iop::gt3 {
class Pdispu2Resident;
// Native disc-source producer. The caller keeps LIBPDI/PDICDVD alive until close.
// No file-source, looping stream, reverse transfer or physical retry behavior.
class PdistrDiscStream {
public:
    PdistrDiscStream(IopHost&,PdicdvdResident&);
    ~PdistrDiscStream();
    bool open(uint32_t lsn,uint32_t sectors,uint32_t desiredBytes);
    uint32_t context() const;
    uint32_t capacity() const;
    // Audited capacity/4 chunks. Each chunk must be contiguous; fragmented
    // inner copies remain unsupported. A failure after progress poisons retry.
    bool consume(uint32_t eeDestination,uint32_t bytes);
    bool consumeSound(Pdispu2Resident&,uint32_t soundDestination,uint32_t bytes);
    bool close();
private:
    struct State;
    std::unique_ptr<State> state_;
};
}
