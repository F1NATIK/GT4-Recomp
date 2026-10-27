#pragma once
#include "ps2x/iop/gt3/spu2_memory.h"
#include <array>
#include <memory>
#include <optional>

namespace ps2x::iop::gt3 {
// Deliberately bounded voice engine for LIBSD's muted startup. Only zero-volume,
// zero-ADSR parameters and filter-zero ADPCM are accepted. No noise, modulation,
// interpolation, effects or audible output path is implied. Native sample ticks
// are explicit, not a claim of cycle-accurate PS2 register/key latency.
class Spu2StartupVoices {
public:
    enum class Phase { Stopped, Attack, Decay, Sustain, Release };
    struct Snapshot {
        bool configured = false;
        uint32_t startAddress = 0, blockAddress = 0, loopAddress = 0;
        uint16_t pitch = 0, envelope = 0, fraction = 0;
        Phase phase = Phase::Stopped;
        uint64_t consumedSamples = 0, decodedBlocks = 0;
        int16_t lastSample = 0;
        bool operator==(const Snapshot&) const = default;
    };
    explicit Spu2StartupVoices(Spu2Memory&);
    ~Spu2StartupVoices();
    bool configure(unsigned core, unsigned voice, uint32_t startAddress,
                   uint16_t pitch, uint16_t leftVolume, uint16_t rightVolume,
                   uint16_t adsr1, uint16_t adsr2);
    bool keyOn(unsigned core, uint32_t mask);
    bool keyOff(unsigned core, uint32_t mask);
    bool advance(uint32_t sampleTicks);
    bool clearEnd(unsigned core, uint32_t mask);
    std::optional<uint32_t> endFlags(unsigned core) const;
    std::optional<Snapshot> snapshot(unsigned core, unsigned voice) const;
    void reset();
private:
    struct State;
    std::unique_ptr<State> state_;
};
}
