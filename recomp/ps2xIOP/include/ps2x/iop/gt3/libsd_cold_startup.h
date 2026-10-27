#pragma once
#include "ps2x/iop/gt3/spu2_memory.h"
#include "ps2x/iop/gt3/spu2_startup_voices.h"
#include "ps2x/iop/gt3/spu2_voices.h"
#include "ps2x/iop/gt3/spu2_events.h"
#include <array>
#include <memory>
#include <span>

namespace ps2x::iop::gt3 {
// One private staging object for native SdInit(0). A failed object is discarded;
// callers publish its register/resource snapshot only after initialize succeeds.
// Access is serialized by the owning LIBSD resident. Snapshot registers are the
// initial state only; later configuration writes live in the resident's IOP bank.
class LibsdColdStartup {
public:
    static constexpr uint32_t RegisterBytes = 0x1000;
    LibsdColdStartup();
    ~LibsdColdStartup();
    bool initialize(std::span<const uint8_t> originalSeed);
    bool initialized() const;
    uint32_t writeCount() const;
    const std::array<uint16_t, RegisterBytes/2>& registers() const;
    const std::array<bool, RegisterBytes/2>& knownRegisters() const;
    const std::array<uint32_t, 6>& busValues() const;
    Spu2Memory& memory();
    Spu2StartupVoices& voices();
    // Runtime voice engine on the same sound RAM; driven through LIBSD registers
    // after cold startup. Starts with every voice stopped at zero pitch.
    Spu2Voices& streamVoices();
    Spu2Events& events();
private:
    struct State;
    std::unique_ptr<State> state_;
};
}
