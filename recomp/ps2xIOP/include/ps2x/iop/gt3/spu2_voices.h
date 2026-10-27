#pragma once
#include "ps2x/iop/gt3/spu2_memory.h"
#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>

namespace ps2x::iop::gt3 {
// Native SPU2 voice engine for streamed ADPCM playback (GT3 movie audio and
// later BGM). One call to render() advances every voice of both cores by whole
// 48 kHz output samples, following the hardware sequence documented by PCSX2
// (GPL-3.0+, used as the permitted reference): per-voice decode FIFO refilled
// four samples per NAX halfword step, NAX starting at SSA|1, loop start/end/
// repeat flags, ENDX, IRQA matching on both cores for every voice access,
// 4-tap Gaussian interpolation, ADSR and fixed voice volume.
//
// Addresses in this interface are sound-RAM BYTE offsets, as returned by LIBSD
// Get/SetAddr; internally the SPU2 works in 16-bit halfwords.
//
// Not modelled (explicitly): pitch modulation, noise, volume slides (sweep
// mode), reverb/effects, core-to-core MIX routing, master/external volumes,
// key-on/off latency and DMA IRQ side effects. The stereo mix is the plain sum
// of voice outputs of both cores, saturated to 16 bits.
class Spu2Voices {
public:
    enum class Param { VolumeLeft, VolumeRight, Pitch, Adsr1, Adsr2 };
    enum class Addr { Start, LoopStart, Next };
    enum class Phase { Stopped, Attack, Decay, Sustain, Release };
    struct VoiceState {
        Phase phase = Phase::Stopped;
        uint32_t startAddress = 0, loopAddress = 0, nextAddress = 0; // bytes
        uint16_t pitch = 0, adsr1 = 0, adsr2 = 0;
        int16_t volumeLeft = 0, volumeRight = 0, envelope = 0, output = 0;
        uint64_t decodedSamples = 0;
    };
    // Optional test/diagnostic tap: every decoded ADPCM sample, in decode order.
    using DecodeTap = std::function<void(unsigned core, unsigned voice, int16_t sample)>;

    explicit Spu2Voices(Spu2Memory&);
    ~Spu2Voices();
    Spu2Voices(const Spu2Voices&) = delete;
    Spu2Voices& operator=(const Spu2Voices&) = delete;

    bool setParam(unsigned core, unsigned voice, Param, uint16_t value);
    std::optional<uint16_t> param(unsigned core, unsigned voice, Param) const;
    bool setAddress(unsigned core, unsigned voice, Addr, uint32_t byteAddress);
    std::optional<uint32_t> address(unsigned core, unsigned voice, Addr) const;
    // Establish a stopped voice's read position without key-on (used to carry
    // over the state left by LIBSD's muted cold-init voices). Byte addresses.
    bool seedStopped(unsigned core, unsigned voice, uint32_t nextAddress, uint32_t loopAddress);
    bool keyOn(unsigned core, uint32_t mask);
    bool keyOff(unsigned core, uint32_t mask);
    std::optional<uint32_t> endFlags(unsigned core) const;
    // IRQ address (bytes, even) and enable. A raised IRQ stays pending until
    // acknowledged; the hardware acknowledges by clearing the enable bit.
    bool setIrqAddress(unsigned core, uint32_t byteAddress);
    std::optional<uint32_t> irqAddress(unsigned core) const;
    bool setIrqEnable(unsigned core, bool enable);
    std::optional<bool> irqEnabled(unsigned core) const;
    uint32_t pendingIrqs() const;           // bit0 core0, bit1 core1
    void setDecodeTap(DecodeTap);

    // Advance `frames` output samples. `stereo` may be empty (silent run) or
    // must hold 2*frames interleaved int16. Returns the IRQ cores newly raised
    // during this call (bit per core); `firstIrqFrame` receives the frame index
    // at which the first new IRQ was raised, or frames if none.
    std::optional<uint32_t> render(uint32_t frames, std::span<int16_t> stereo, uint32_t* firstIrqFrame = nullptr);
    std::optional<VoiceState> voice(unsigned core, unsigned voice) const;
    void reset();
private:
    struct State;
    std::unique_ptr<State> state_;
};
}
