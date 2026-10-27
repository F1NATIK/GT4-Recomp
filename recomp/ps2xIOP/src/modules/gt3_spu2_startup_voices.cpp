#include "ps2x/iop/gt3/spu2_startup_voices.h"
#include <algorithm>
#include <mutex>

namespace ps2x::iop::gt3 {
struct Spu2StartupVoices::State {
    struct Voice {
        Snapshot state;
        std::array<int16_t, 28> samples{};
        uint8_t position = 0, flags = 0;
    };
    Spu2Memory& memory;
    mutable std::mutex mutex;
    std::array<std::array<Voice, 24>, 2> voices{};
    std::array<uint32_t, 2> ended{};
    explicit State(Spu2Memory& m): memory(m) {}
    static bool selector(unsigned core, uint32_t mask) { return core < 2 && !(mask & 0xff000000); }
    bool decode(Voice& voice) {
        auto& s = voice.state;
        std::array<uint8_t, 16> block{};
        if (!memory.read(s.blockAddress, block) || block[0] > 12 || (block[1] & ~7)) return false;
        const int scale = 1 << (12-block[0]); // Filter zero: no previous-block predictor.
        for (unsigned i=0; i<28; ++i) {
            int nibble = (block[2+i/2] >> ((i&1)*4)) & 15;
            if (nibble >= 8) nibble -= 16;
            voice.samples[i] = int16_t(nibble * scale); // No left-shift of negative signed values.
        }
        voice.flags = block[1]; voice.position = 0; ++s.decodedBlocks;
        if (voice.flags & 4) s.loopAddress = s.blockAddress;
        return true;
    }
    static void envelope(Snapshot& s) {
        // ADSR1=ADSR2=0: linear attack/sustain +7*2048, exponential
        // decay -8*2048 scaled by current level, linear release -8*2048.
        switch (s.phase) {
        case Phase::Attack:
            s.envelope = uint16_t(std::min<unsigned>(32767, s.envelope+14336));
            if (s.envelope == 32767) s.phase = Phase::Decay;
            break;
        case Phase::Decay:
            s.envelope = uint16_t(s.envelope/2); // level + floor(-level/2).
            if (s.envelope <= 2048) s.phase = Phase::Sustain;
            break;
        case Phase::Sustain:
            s.envelope = uint16_t(std::min<unsigned>(32767, s.envelope+14336));
            break;
        case Phase::Release:
            s.envelope = uint16_t(std::max(0, int(s.envelope)-16384));
            if (!s.envelope) s.phase = Phase::Stopped;
            break;
        case Phase::Stopped: break;
        }
    }
    bool tick(Voice& voice, uint32_t bit, uint32_t& ended) {
        auto& s = voice.state;
        if (s.phase == Phase::Stopped) return true;
        envelope(s);
        if (s.phase == Phase::Stopped) return true;
        uint32_t fraction = s.fraction+s.pitch;
        while (fraction >= 0x1000) {
            fraction -= 0x1000;
            if (voice.position == 28 && !decode(voice)) return false;
            s.lastSample = voice.samples[voice.position++]; ++s.consumedSamples;
            if (voice.position == 28) {
                if (voice.flags & 1) {
                    ended |= bit;
                    s.blockAddress = s.loopAddress;
                    if (!(voice.flags & 2)) {
                        s.envelope = 0; s.phase = Phase::Stopped;
                        break;
                    }
                } else s.blockAddress = (s.blockAddress+16) & (Spu2Memory::Size-1);
            }
        }
        s.fraction = uint16_t(fraction & 0xfff);
        return true;
    }
};
Spu2StartupVoices::Spu2StartupVoices(Spu2Memory& memory): state_(std::make_unique<State>(memory)) {}
Spu2StartupVoices::~Spu2StartupVoices() = default;
bool Spu2StartupVoices::configure(unsigned core, unsigned voice, uint32_t address,
    uint16_t pitch, uint16_t left, uint16_t right, uint16_t adsr1, uint16_t adsr2) {
    if (core >= 2 || voice >= 24 || address >= Spu2Memory::Size || (address & 15) ||
        pitch > 0x3fff || left || right || adsr1 || adsr2) return false;
    auto& s = *state_; std::lock_guard lock(s.mutex);
    auto& v = s.voices[core][voice];
    if (v.state.phase != Phase::Stopped) return false;
    v = {}; v.state.configured = true; v.state.startAddress = address; v.state.pitch = pitch;
    return true;
}
bool Spu2StartupVoices::keyOn(unsigned core, uint32_t mask) {
    if (!State::selector(core, mask)) return false;
    auto& s = *state_; std::lock_guard lock(s.mutex);
    auto staged = s.voices[core];
    for (unsigned i=0; i<24; ++i) if (mask & (1u<<i)) {
        auto& v = staged[i];
        if (!v.state.configured) return false;
        const auto address = v.state.startAddress; const auto pitch = v.state.pitch;
        v = {}; v.state.configured = true; v.state.startAddress = address; v.state.pitch = pitch;
        v.state.blockAddress = v.state.loopAddress = address; v.state.phase = Phase::Attack;
        if (!s.decode(v)) return false;
    }
    s.voices[core] = staged; s.ended[core] &= ~mask; return true;
}
bool Spu2StartupVoices::keyOff(unsigned core, uint32_t mask) {
    if (!State::selector(core, mask)) return false;
    auto& s = *state_; std::lock_guard lock(s.mutex);
    for (unsigned i=0; i<24; ++i) if (mask & (1u<<i)) {
        auto& v = s.voices[core][i].state;
        if (v.phase != Phase::Stopped) v.phase = Phase::Release;
    }
    return true;
}
bool Spu2StartupVoices::advance(uint32_t ticks) {
    if (ticks > 48000) return false;
    auto& s = *state_; std::lock_guard lock(s.mutex);
    auto staged = s.voices; auto ended = s.ended;
    for (uint32_t tick=0; tick<ticks; ++tick)
        for (unsigned core=0; core<2; ++core)
            for (unsigned voice=0; voice<24; ++voice)
                if (!s.tick(staged[core][voice], 1u<<voice, ended[core])) return false;
    s.voices = staged; s.ended = ended; return true;
}
bool Spu2StartupVoices::clearEnd(unsigned core, uint32_t mask) {
    if (!State::selector(core, mask)) return false;
    auto& s = *state_; std::lock_guard lock(s.mutex);
    s.ended[core] &= ~mask; return true;
}
std::optional<uint32_t> Spu2StartupVoices::endFlags(unsigned core) const {
    if (core >= 2) return {};
    auto& s = *state_; std::lock_guard lock(s.mutex); return s.ended[core];
}
std::optional<Spu2StartupVoices::Snapshot> Spu2StartupVoices::snapshot(unsigned core, unsigned voice) const {
    if (core >= 2 || voice >= 24) return {};
    auto& s = *state_; std::lock_guard lock(s.mutex); return s.voices[core][voice].state;
}
void Spu2StartupVoices::reset() {
    auto& s = *state_; std::lock_guard lock(s.mutex); s.voices = {}; s.ended = {};
}
}
