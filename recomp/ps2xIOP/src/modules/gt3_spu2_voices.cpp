#include "ps2x/iop/gt3/spu2_voices.h"
#include "gt3_spu2_gauss_table.h"
#include <algorithm>
#include <cstdint>
#include <atomic>
#include <mutex>

namespace ps2x::iop::gt3 {
namespace {
constexpr uint32_t AddressMask = 0xfffff; // 2 MiB of sound RAM in halfwords.
constexpr int32_t EnvelopeMax = 0x7fff;
constexpr uint8_t FlagLoopEnd = 1, FlagLoop = 2, FlagLoopStart = 4;
}

struct Spu2Voices::State {
    struct Voice {
        uint32_t start = 0, loopStart = 0, next = 0; // halfwords
        bool loopManual = false, blockValid = false;
        uint8_t loopFlags = 0;
        int32_t prev1 = 0, prev2 = 0;
        int16_t block[28] = {};
        int16_t fifo[32] = {};
        uint32_t readPos = 0, writePos = 0, counter = 0;
        uint16_t pitch = 0, adsr1 = 0, adsr2 = 0, volumeLeftReg = 0, volumeRightReg = 0;
        Phase phase = Phase::Stopped;
        int32_t envelope = 0; uint32_t envelopeCounter = 0;
        int16_t output = 0;
        uint64_t decoded = 0;
        // Block-header cache: valid while the header address and the sound-RAM
        // write generation are unchanged (hardware re-reads it every sample).
        uint32_t headerAddress = UINT32_MAX; uint64_t headerGeneration = 0; uint8_t headerFlags = 0;
    };
    struct Core {
        std::array<Voice, 24> voices{};
        uint32_t endx = 0, irqAddress = 0;
        bool irqEnable = false;
    };
    Spu2Memory& memory;
    mutable std::mutex mutex;
    std::array<Core, 2> cores{};
    uint32_t pending = 0, raisedThisCall = 0, frame = 0, firstIrq = 0;
    DecodeTap tap;
    const std::atomic<uint64_t>* memoryGeneration;
    const int16_t* gauss = &spu2GaussTable[0][0];
    explicit State(Spu2Memory& m): memory(m), memoryGeneration(&m.generationCounter()) {}

    static bool valid(unsigned core, unsigned voice) { return core < 2 && voice < 24; }
    static int16_t fixedVolume(uint16_t reg) { return int16_t(uint16_t(reg << 1)); } // bit15 sweep not modelled

    // Hot path (per voice, per 48 kHz frame). Written with raw arrays and
    // explicit arithmetic so unoptimized (Debug) builds still run well ahead
    // of real time; behaviour is identical to the hardware rules noted below.
    // IRQ state for the current render call, refreshed by refreshIrq().
    bool irqOn[2] = {false, false}; uint32_t irqAt[2] = {0, 0};
    void refreshIrq() {
        for (unsigned c = 0; c < 2; ++c) { irqOn[c] = cores[c].irqEnable; irqAt[c] = cores[c].irqAddress; }
    }
    void raise(unsigned core) {
        const uint32_t bit = 1u << core;
        if (pending & bit) return; // Latched until acknowledged by disabling.
        if (!raisedThisCall) firstIrq = frame;
        raisedThisCall |= bit; pending |= bit;
    }
    void checkIrq(uint32_t address) {
        if (irqOn[0] && irqAt[0] == address) raise(0);
        if (irqOn[1] && irqAt[1] == address) raise(1);
    }
    void incrementNext(Voice& v) { checkIrq(v.next); v.next = (v.next + 1) & AddressMask; }

    bool decodeBlock(unsigned core, unsigned index, Voice& v) {
        std::array<uint8_t, 16> raw{};
        if (!memory.read((v.next & ~7u) * 2, raw)) return false;
        const uint8_t* r = raw.data();
        const int shift = (r[0] & 15) + 16;
        const unsigned filter = (r[0] >> 4) < 4 ? unsigned(r[0] >> 4) : 4u;
        static const int32_t coefficients[5][2] = {{0, 0}, {60, 0}, {115, -52}, {98, -55}, {122, -60}};
        const int32_t p0 = coefficients[filter][0], p1 = coefficients[filter][1];
        int32_t prev1 = v.prev1, prev2 = v.prev2;
        for (unsigned i = 0; i < 28; ++i) {
            const int32_t nibble = int32_t(uint32_t(r[2 + i / 2] >> ((i & 1) * 4)) << 28);
            int32_t sample = (nibble >> shift) + ((p0 * prev1 + p1 * prev2 + 32) >> 6);
            if (sample > 0x7fff) sample = 0x7fff; else if (sample < -0x8000) sample = -0x8000;
            prev2 = prev1; prev1 = sample; v.block[i] = int16_t(sample);
        }
        v.prev1 = prev1; v.prev2 = prev2;
        if (tap) for (unsigned i = 0; i < 28; ++i) tap(core, index, v.block[i]);
        v.decoded += 28; v.blockValid = true;
        return true;
    }
    bool decode(unsigned core, unsigned index, Voice& v) {
        // Block header is re-examined every output sample (cached while the
        // header address and sound-RAM write generation are unchanged).
        const uint32_t headerAddress = v.next & ~7u;
        checkIrq(headerAddress);
        const uint64_t generation = memoryGeneration->load(std::memory_order_acquire);
        if (v.headerAddress != headerAddress || v.headerGeneration != generation) {
            std::array<uint8_t, 2> header{};
            if (!memory.read(headerAddress * 2, header)) return false;
            v.headerAddress = headerAddress; v.headerGeneration = generation; v.headerFlags = header[1];
        }
        v.loopFlags = v.headerFlags;
        if ((v.loopFlags & FlagLoopStart) && !v.loopManual) v.loopStart = headerAddress;
        if (int32_t(v.writePos - v.readPos) > 12) return true;
        if (v.phase != Phase::Stopped) {
            if (!v.blockValid && !decodeBlock(core, index, v)) return false;
            const int16_t* from = v.block + ((v.next & 7) - 1) * 4;
            for (unsigned i = 0; i < 4; ++i) v.fifo[(v.writePos + i) & 31] = from[i];
        }
        v.writePos += 4;
        incrementNext(v);
        if ((v.next & 7) == 0) {
            if (v.loopFlags & FlagLoopEnd) {
                cores[core].endx |= 1u << index;
                v.next = v.loopStart;
                if (!(v.loopFlags & FlagLoop)) { v.envelope = 0; v.phase = Phase::Stopped; }
            }
            incrementNext(v);
            v.blockValid = false;
        }
        return true;
    }
    // ADSR per the hardware rate/step rules (PCSX2 V_ADSR::Calculate).
    static void envelope(Voice& v) {
        if (v.phase == Phase::Stopped) { v.envelope = 0; return; }
        const uint16_t a1 = v.adsr1, a2 = v.adsr2;
        bool decrement = false, exponential = false; int shift = 0, step = 0, target = 0;
        switch (v.phase) {
        case Phase::Attack:
            exponential = a1 >> 15; shift = (a1 >> 10) & 31; step = 7 - ((a1 >> 8) & 3); target = EnvelopeMax; break;
        case Phase::Decay:
            decrement = exponential = true; shift = (a1 >> 4) & 15; step = -8; target = ((a1 & 15) + 1) << 11; break;
        case Phase::Sustain:
            decrement = (a2 >> 14) & 1; exponential = a2 >> 15; shift = (a2 >> 8) & 31; step = 7 - ((a2 >> 6) & 3);
            if (decrement) step = ~step;
            break;
        case Phase::Release:
            decrement = true; exponential = (a2 >> 5) & 1; shift = a2 & 31; step = -8; break;
        case Phase::Stopped: break;
        }
        uint32_t counterIncrement = 0x8000u >> (shift > 11 ? shift - 11 : 0);
        int32_t levelIncrement = step * (1 << (shift < 11 ? 11 - shift : 0));
        if (exponential) {
            if (!decrement && v.envelope > 0x6000) counterIncrement >>= 2;
            if (decrement) levelIncrement = int16_t((levelIncrement * v.envelope) >> 15);
        }
        v.envelopeCounter += counterIncrement ? counterIncrement : 1u;
        if (v.envelopeCounter >= 0x8000) {
            v.envelopeCounter = 0;
            int32_t level = v.envelope + levelIncrement;
            v.envelope = level < 0 ? 0 : level > EnvelopeMax ? EnvelopeMax : level;
        }
        if (v.phase == Phase::Sustain) { if (!v.envelope) v.phase = Phase::Stopped; return; }
        if ((!decrement && v.envelope >= target) || (decrement && v.envelope <= target)) {
            v.phase = v.phase == Phase::Attack ? Phase::Decay : v.phase == Phase::Decay ? Phase::Sustain : Phase::Stopped;
            if (v.phase == Phase::Stopped) v.envelope = 0;
        }
    }
    static bool idle(const Voice& v) { return v.phase == Phase::Stopped && !v.pitch && int32_t(v.writePos - v.readPos) > 12; }
    bool mix(unsigned core, unsigned index, Voice& v, int32_t& left, int32_t& right) {
        // Idle fast path: a stopped voice at zero pitch with a full FIFO only
        // repeats its block-header IRQ comparison. (Its loop-start latch from
        // that unchanged header is not re-evaluated; documented deviation.)
        if (idle(v)) { checkIrq(v.next & ~7u); v.output = 0; return true; }
        if (!decode(core, index, v)) return false;
        if (v.phase != Phase::Stopped) {
            const int16_t* w = gauss + ((v.counter & 0x0ff0) >> 4) * 4;
            const int16_t* f = v.fifo;
            const uint32_t r = v.readPos;
            int32_t value = ((w[0] * f[r & 31]) >> 15) + ((w[1] * f[(r + 1) & 31]) >> 15) +
                            ((w[2] * f[(r + 2) & 31]) >> 15) + ((w[3] * f[(r + 3) & 31]) >> 15);
            envelope(v);
            value = (value * v.envelope) >> 15;
            v.output = int16_t(value);
            left += (value * fixedVolume(v.volumeLeftReg)) >> 15;
            right += (value * fixedVolume(v.volumeRightReg)) >> 15;
        } else v.output = 0;
        v.counter += v.pitch < 0x3fff ? v.pitch : 0x3fff;
        v.readPos += v.counter >> 12; v.counter &= 0xfff;
        return true;
    }
};

Spu2Voices::Spu2Voices(Spu2Memory& memory): state_(std::make_unique<State>(memory)) {}
Spu2Voices::~Spu2Voices() = default;

bool Spu2Voices::setParam(unsigned core, unsigned index, Param p, uint16_t value) {
    if (!State::valid(core, index)) return false;
    auto& s = *state_; std::lock_guard lock(s.mutex); auto& v = s.cores[core].voices[index];
    switch (p) {
    case Param::VolumeLeft: if (value & 0x8000) return false; v.volumeLeftReg = value; return true; // sweep unsupported
    case Param::VolumeRight: if (value & 0x8000) return false; v.volumeRightReg = value; return true;
    case Param::Pitch: v.pitch = value; return true;
    case Param::Adsr1: v.adsr1 = value; return true;
    case Param::Adsr2: v.adsr2 = value; return true;
    }
    return false;
}
std::optional<uint16_t> Spu2Voices::param(unsigned core, unsigned index, Param p) const {
    if (!State::valid(core, index)) return {};
    auto& s = *state_; std::lock_guard lock(s.mutex); const auto& v = s.cores[core].voices[index];
    switch (p) {
    case Param::VolumeLeft: return v.volumeLeftReg;
    case Param::VolumeRight: return v.volumeRightReg;
    case Param::Pitch: return v.pitch;
    case Param::Adsr1: return v.adsr1;
    case Param::Adsr2: return v.adsr2;
    }
    return {};
}
bool Spu2Voices::setAddress(unsigned core, unsigned index, Addr a, uint32_t bytes) {
    if (!State::valid(core, index) || bytes >= Spu2Memory::Size || (bytes & 1) || a == Addr::Next) return false;
    auto& s = *state_; std::lock_guard lock(s.mutex); auto& v = s.cores[core].voices[index];
    if (a == Addr::Start) v.start = bytes / 2;
    else { v.loopStart = bytes / 2; v.loopManual = true; }
    return true;
}
std::optional<uint32_t> Spu2Voices::address(unsigned core, unsigned index, Addr a) const {
    if (!State::valid(core, index)) return {};
    auto& s = *state_; std::lock_guard lock(s.mutex); const auto& v = s.cores[core].voices[index];
    return 2 * (a == Addr::Start ? v.start : a == Addr::LoopStart ? v.loopStart : v.next);
}
bool Spu2Voices::seedStopped(unsigned core, unsigned index, uint32_t next, uint32_t loop) {
    if (!State::valid(core, index) || next >= Spu2Memory::Size || loop >= Spu2Memory::Size || (next & 1) || (loop & 15)) return false;
    auto& s = *state_; std::lock_guard lock(s.mutex); auto& v = s.cores[core].voices[index];
    if (v.phase != Phase::Stopped) return false;
    v.next = next / 2; v.loopStart = loop / 2; v.loopManual = false; v.blockValid = false;
    v.readPos = v.writePos = 0; v.counter = 0; v.envelope = 0;
    return true;
}
bool Spu2Voices::keyOn(unsigned core, uint32_t mask) {
    if (core >= 2 || (mask & 0xff000000)) return false;
    auto& s = *state_; std::lock_guard lock(s.mutex); auto& c = s.cores[core];
    for (unsigned i = 0; i < 24; ++i) if (mask & (1u << i)) {
        auto& v = c.voices[i];
        v.start &= ~7u; // Misaligned starts are rounded down to their block.
        v.phase = Phase::Attack; v.envelope = 0; v.envelopeCounter = 0;
        v.loopManual = false; v.counter = 0; v.loopFlags = 0; v.next = v.start | 1;
        v.prev1 = v.prev2 = 0; v.blockValid = false; v.readPos = v.writePos = 0;
    }
    c.endx &= ~mask;
    return true;
}
bool Spu2Voices::keyOff(unsigned core, uint32_t mask) {
    if (core >= 2 || (mask & 0xff000000)) return false;
    auto& s = *state_; std::lock_guard lock(s.mutex);
    for (unsigned i = 0; i < 24; ++i) if (mask & (1u << i)) {
        auto& v = s.cores[core].voices[i];
        if (v.phase != Phase::Stopped) { v.phase = Phase::Release; v.envelopeCounter = 0; }
    }
    return true;
}
std::optional<uint32_t> Spu2Voices::endFlags(unsigned core) const {
    if (core >= 2) return {};
    auto& s = *state_; std::lock_guard lock(s.mutex); return s.cores[core].endx;
}
bool Spu2Voices::setIrqAddress(unsigned core, uint32_t bytes) {
    if (core >= 2 || bytes >= Spu2Memory::Size || (bytes & 1)) return false;
    auto& s = *state_; std::lock_guard lock(s.mutex); s.cores[core].irqAddress = bytes / 2; return true;
}
std::optional<uint32_t> Spu2Voices::irqAddress(unsigned core) const {
    if (core >= 2) return {};
    auto& s = *state_; std::lock_guard lock(s.mutex); return s.cores[core].irqAddress * 2;
}
bool Spu2Voices::setIrqEnable(unsigned core, bool enable) {
    if (core >= 2) return false;
    auto& s = *state_; std::lock_guard lock(s.mutex);
    s.cores[core].irqEnable = enable;
    if (!enable) s.pending &= ~(1u << core); // Disabling acknowledges the IRQ.
    return true;
}
std::optional<bool> Spu2Voices::irqEnabled(unsigned core) const {
    if (core >= 2) return {};
    auto& s = *state_; std::lock_guard lock(s.mutex); return s.cores[core].irqEnable;
}
uint32_t Spu2Voices::pendingIrqs() const { auto& s = *state_; std::lock_guard lock(s.mutex); return s.pending; }
void Spu2Voices::setDecodeTap(DecodeTap tap) { auto& s = *state_; std::lock_guard lock(s.mutex); s.tap = std::move(tap); }

std::optional<uint32_t> Spu2Voices::render(uint32_t frames, std::span<int16_t> stereo, uint32_t* firstIrqFrame) {
    if (!stereo.empty() && stereo.size() != size_t(frames) * 2) return {};
    auto& s = *state_; std::lock_guard lock(s.mutex);
    s.raisedThisCall = 0; s.firstIrq = frames;
    // An idle voice (see mix) never changes during a render call and only
    // repeats one IRQ comparison, which latches on the first frame. Check it
    // once here and mix only the other voices per frame. Output is identical.
    std::array<std::pair<uint8_t, uint8_t>, 48> active{}; size_t activeCount = 0;
    s.frame = 0; s.refreshIrq();
    int16_t* const out = stereo.empty() ? nullptr : stereo.data();
    for (unsigned c = 0; c < 2; ++c)
        for (unsigned i = 0; i < 24; ++i) {
            auto& v = s.cores[c].voices[i];
            if (!State::idle(v)) { active[activeCount++] = {uint8_t(c), uint8_t(i)}; continue; }
            v.output = 0;
            if (frames) s.checkIrq(v.next & ~7u);
        }
    for (s.frame = 0; s.frame < frames; ++s.frame) {
        int32_t left = 0, right = 0;
        for (size_t k = 0; k < activeCount; ++k) {
            const auto [c, i] = active[k];
            if (!s.mix(c, i, s.cores[c].voices[i], left, right)) return {};
        }
        if (out) {
            out[s.frame * 2] = int16_t(left > 0x7fff ? 0x7fff : left < -0x8000 ? -0x8000 : left);
            out[s.frame * 2 + 1] = int16_t(right > 0x7fff ? 0x7fff : right < -0x8000 ? -0x8000 : right);
        }
    }
    if (firstIrqFrame) *firstIrqFrame = s.firstIrq;
    return s.raisedThisCall;
}
std::optional<Spu2Voices::VoiceState> Spu2Voices::voice(unsigned core, unsigned index) const {
    if (!State::valid(core, index)) return {};
    auto& s = *state_; std::lock_guard lock(s.mutex); const auto& v = s.cores[core].voices[index];
    VoiceState r;
    r.phase = v.phase; r.startAddress = v.start * 2; r.loopAddress = v.loopStart * 2; r.nextAddress = v.next * 2;
    r.pitch = v.pitch; r.adsr1 = v.adsr1; r.adsr2 = v.adsr2;
    r.volumeLeft = State::fixedVolume(v.volumeLeftReg); r.volumeRight = State::fixedVolume(v.volumeRightReg);
    r.envelope = int16_t(v.envelope); r.output = v.output; r.decodedSamples = v.decoded;
    return r;
}
void Spu2Voices::reset() {
    auto& s = *state_; std::lock_guard lock(s.mutex);
    s.cores = {}; s.pending = 0;
}
}
