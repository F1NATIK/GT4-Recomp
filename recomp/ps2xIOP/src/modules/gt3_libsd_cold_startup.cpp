#include "ps2x/iop/gt3/libsd_cold_startup.h"
#include "gt3_libsd_init_sequence.h"
#include <algorithm>
#include <chrono>
#include <thread>

namespace ps2x::iop::gt3 {
struct LibsdColdStartup::State {
    Spu2Memory memory;
    Spu2StartupVoices voices{memory};
    Spu2Voices stream{memory};
    Spu2Events events;
    std::array<uint16_t, RegisterBytes/2> registers{};
    std::array<bool, RegisterBytes/2> known{};
    std::array<uint32_t, 6> bus{};
    std::array<uint8_t, 32> seed{};
    unsigned seedPosition = 0;
    uint32_t count = 0;
    bool attempted = false, initialized = false;
    static constexpr std::array<uint32_t, 6> busAddresses{0xbf801404,0xbf80140c,0xbf8010f0,0xbf801570,0xbf801014,0xbf801414};
    void store(unsigned offset, uint16_t value) {
        registers[offset/2] = value; known[offset/2] = true;
    }
    uint32_t address(unsigned offset) const {
        return (((uint32_t(registers[offset/2]) << 16) | registers[offset/2+1]) & 0xfffff) * 2;
    }
    bool configure(unsigned core, unsigned voice) {
        const auto params = core*0x400+voice*16, start = core*0x400+0x1c0+voice*12;
        for (auto o : {params, params+2, params+4, params+6, params+8, start, start+2})
            if (!known[o/2]) return false;
        return voices.configure(core, voice, address(start), registers[(params+4)/2],
            registers[params/2], registers[(params+2)/2], registers[(params+6)/2], registers[(params+8)/2]);
    }
    void voiceStatus() {
        for (unsigned core=0; core<2; ++core) {
            const auto flags = *voices.endFlags(core);
            store(core*0x400+0x340, uint16_t(flags)); store(core*0x400+0x342, uint16_t(flags>>16));
        }
    }
    bool write(const detail::LibsdInitWrite& step) {
        if (step.bytes == 4) {
            const auto it = std::find(busAddresses.begin(), busAddresses.end(), step.address);
            if (it == busAddresses.end()) return false;
            bus[it-busAddresses.begin()] = step.value; ++count; return true;
        }
        if (step.bytes != 2 || step.address < 0xbf900000 || step.address >= 0xbf901000 || (step.address & 1)) return false;
        const auto offset = unsigned(step.address-0xbf900000), core = offset/0x400, local = offset%0x400;
        const auto value = uint16_t(step.value);
        if (core < 2 && (local == 0x1a0 || local == 0x1a2)) {
            const auto mask = (uint32_t(value) << (local == 0x1a2 ? 16 : 0)) & 0xffffff;
            for (unsigned voice=0; voice<24; ++voice)
                if ((mask & (1u<<voice)) && !configure(core, voice)) return false;
            if (!voices.keyOn(core, mask)) return false;
            voiceStatus();
        } else if (core < 2 && (local == 0x1a4 || local == 0x1a6)) {
            const auto mask = (uint32_t(value) << (local == 0x1a6 ? 16 : 0)) & 0xffffff;
            if (!voices.keyOff(core, mask)) return false;
        } else if (core < 2 && (local == 0x340 || local == 0x342)) {
            if (value || !voices.clearEnd(core, local == 0x340 ? 0xffff : 0xff0000)) return false;
            voiceStatus();
        } else if (core < 2 && local == 0x1ac) {
            if (core || seedPosition >= 16) return false;
            const auto expected = uint16_t(seed[seedPosition*2]) | uint16_t(seed[seedPosition*2+1]) << 8;
            if (value != expected || !memory.writePio(core, value)) return false;
            ++seedPosition;
            const auto word = *memory.transferAddress(core)/2;
            store(core*0x400+0x1a8, uint16_t(word>>16)); store(core*0x400+0x1aa, uint16_t(word));
        }
        store(offset, value); ++count;
        if (core < 2 && (local == 0x1a8 || local == 0x1aa)) {
            const auto tsa = core*0x400+0x1a8;
            if (known[tsa/2] && known[tsa/2+1] && !memory.setTransferAddress(core, address(tsa))) return false;
        }
        if (core < 2 && local == 0x19a) {
            // PIO writes above synchronously committed their bytes. No pending
            // transfer remains; mode bits expose the native ready state.
            store(core*0x400+0x344, (value & 0x30) ? 0x80 : 0);
        }
        return true;
    }
};
LibsdColdStartup::LibsdColdStartup(): state_(std::make_unique<State>()) {}
LibsdColdStartup::~LibsdColdStartup() = default;
bool LibsdColdStartup::initialize(std::span<const uint8_t> originalSeed) {
    auto& s = *state_;
    if (s.attempted || originalSeed.size() != 32) return false;
    s.attempted = true;
    for (unsigned i=0; i<32; ++i) if (originalSeed[i] != (i<16 ? 7 : 0)) return false;
    std::copy(originalSeed.begin(), originalSeed.end(), s.seed.begin());
    // Explicit native cold profile for the initial EEA reads, cross-checked
    // against SPU2 reset defaults. These are word-address high halves.
    s.store(0x33c, 14); s.store(0x73c, 15);
    for (const auto& step : detail::libsdInitWrites) {
        // Original1210 allocates its events before writing the default ESA.
        // Acquire both atomically in this private staging object.
        if (step.pc == 0x1cb8 && !s.events.ready() && !s.events.initialize()) return false;
        if (!s.write(step)) return false;
        if (step.pc == 0x1a20 || step.pc == 0x1a34 || step.pc == 0x1ad8)
            std::this_thread::sleep_for(std::chrono::microseconds(step.pc == 0x1a20 ? 42 : 21));
        if (step.pc == 0x3d94 || step.pc == 0x3dfc) {
            // Execute one millisecond of native 48kHz muted voice progression
            // for each original busy delay. Hardware instruction latency is
            // not reproduced; required loop and release work actually occurs.
            if (!s.voices.advance(48)) return false;
            s.voiceStatus();
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }
    if (!s.events.ready() || s.seedPosition != 16 || s.count != 447) return false;
    for (unsigned core=0; core<2; ++core) for (unsigned voice=0; voice<24; ++voice) {
        const auto v = *s.voices.snapshot(core, voice);
        if (!v.configured || v.phase != Spu2StartupVoices::Phase::Stopped || v.envelope || !v.consumedSamples) return false;
    }
    s.initialized = true; return true;
}
bool LibsdColdStartup::initialized() const { return state_->initialized; }
uint32_t LibsdColdStartup::writeCount() const { return state_->count; }
const std::array<uint16_t, LibsdColdStartup::RegisterBytes/2>& LibsdColdStartup::registers() const { return state_->registers; }
const std::array<bool, LibsdColdStartup::RegisterBytes/2>& LibsdColdStartup::knownRegisters() const { return state_->known; }
const std::array<uint32_t, 6>& LibsdColdStartup::busValues() const { return state_->bus; }
Spu2Memory& LibsdColdStartup::memory() { return state_->memory; }
Spu2StartupVoices& LibsdColdStartup::voices() { return state_->voices; }
Spu2Voices& LibsdColdStartup::streamVoices() { return state_->stream; }
Spu2Events& LibsdColdStartup::events() { return state_->events; }
}
