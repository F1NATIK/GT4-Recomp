#include "gt4_spu2.h"
#include "spu2_gauss_table.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>

namespace ps2x::iop::gt4
{
    namespace
    {
        constexpr uint32_t RamHalfwords = Spu2::RamBytes / 2u;
        constexpr uint32_t AddressMask = RamHalfwords - 1u;
        constexpr int32_t FilterPos[5] = {0, 60, 115, 98, 122};
        constexpr int32_t FilterNeg[5] = {0, 0, -52, -55, -60};
#if !defined(GT4_STOCK_BUILD)
        // Engine bass, room for the boost (gt4_spu2.h): the scale's attack (5 ms from 1 to 0) and release (0.5 s from
        // 0 to 1), per 48 kHz sample.
        constexpr double BassAttackStep = 1.0 / 240.0;
        constexpr double BassReleaseStep = 1.0 / 24000.0;
#endif

        int16_t clamp16(int32_t value)
        {
            return static_cast<int16_t>(std::clamp(value, -32768, 32767));
        }

        // One envelope tick (nocash psx-spx "SPU ADSR"): returns the new level.
        int32_t envelopeTick(int32_t level, uint32_t &counter, unsigned rate, bool exponential, bool decrease)
        {
            const int shift = static_cast<int>(rate >> 2);
            int32_t step = decrease ? -8 + static_cast<int32_t>(rate & 3u) : 7 - static_cast<int32_t>(rate & 3u);
            uint64_t cycles = uint64_t(1) << std::max(0, shift - 11);
            step = step * (1 << std::max(0, 11 - shift));
            if (exponential && !decrease && level > 0x6000)
            {
                cycles *= 4u;
            }
            if (exponential && decrease)
            {
                step = (step * level) >> 15;
            }
            if (++counter < cycles)
            {
                return level;
            }
            counter = 0;
            return std::clamp(level + step, 0, 0x7fff);
        }
    }

    Spu2::Spu2() : ram_(RamHalfwords, 0)
    {
        reset();
    }

    void Spu2::reset()
    {
        std::lock_guard lock(mutex_);
        std::fill(ram_.begin(), ram_.end(), 0);
        core_ = {};
        pendingIrq_ = 0;
#if !defined(GT4_STOCK_BUILD)
        // The sound RAM is empty again: no engine samples; the engine bass level stays as set.
        std::fill(engineBlocks_.begin(), engineBlocks_.end(), uint8_t(0));
        resetEngineBassState();
#endif
    }

#if !defined(GT4_STOCK_BUILD)
    void Spu2::resetEngineBassState()
    {
        bassShelf_ = {};
        bassScale_ = bassTarget_ = 1.0;
        bassChunkAt_ = 0;
        bassChunkSlot_ = 0;
        bassPeakRest_ = {};
        bassPeakBoost_ = {};
        bassRest_ = {};
        bassBoost_ = {};
        bassLimiting_ = false;
        bassHold_ = 0;
    }

    void Spu2::setEngineBass(EngineBass level)
    {
        std::lock_guard lock(mutex_);
        bassLevel_ = level;
        resetEngineBassState();
        // RBJ "Audio EQ Cookbook" low shelf, shelf slope S = 1 (alpha = sin(w0) / 2 * sqrt(2)), 48 kHz.
        const double a = std::pow(10.0, engineBassGainDb(level) / 40.0);
        const double w0 = 2.0 * 3.14159265358979323846 * EngineBassCornerHz / 48000.0;
        const double cw = std::cos(w0), alpha = std::sin(w0) / 2.0 * std::sqrt(2.0), sa2 = 2.0 * std::sqrt(a) * alpha;
        const double b0 = a * ((a + 1.0) - (a - 1.0) * cw + sa2);
        const double b1 = 2.0 * a * ((a - 1.0) - (a + 1.0) * cw);
        const double b2 = a * ((a + 1.0) - (a - 1.0) * cw - sa2);
        const double a0 = (a + 1.0) + (a - 1.0) * cw + sa2;
        const double a1 = -2.0 * ((a - 1.0) + (a + 1.0) * cw);
        const double a2 = (a + 1.0) + (a - 1.0) * cw - sa2;
        bassCoef_ = {b0 / a0, b1 / a0, b2 / a0, a1 / a0, a2 / a0};
    }

    Spu2::EngineBass Spu2::engineBass() const
    {
        std::lock_guard lock(mutex_);
        return bassLevel_;
    }

    int16_t Spu2::engineBassLimit(int32_t sample)
    {
        const int64_t s = sample;
        const int64_t a = s < 0 ? -s : s;
        if (a <= EngineBassKnee)
            return static_cast<int16_t>(sample);
        constexpr double range = double(EngineBassCeiling - EngineBassKnee);
        const double y = EngineBassKnee + range * std::tanh(double(a - EngineBassKnee) / range);
        const int32_t m = std::min<int32_t>(static_cast<int32_t>(y + 0.5), EngineBassCeiling - 1);
        return static_cast<int16_t>(s < 0 ? -m : m);
    }

    void Spu2::markEngineSamples(uint32_t byteAddress, uint32_t bytes, bool engine)
    {
        if (bytes == 0)
            return;
        std::lock_guard lock(mutex_);
        constexpr uint32_t blocks = RamBytes / 16u;
        const uint32_t start = byteAddress % RamBytes;
        const uint64_t end = uint64_t(start) + bytes; // exclusive; may pass the end of the RAM (writeRam wraps)
        const uint64_t count = std::min<uint64_t>(blocks, (end + 15u) / 16u - start / 16u);
        for (uint64_t i = 0; i < count; ++i)
            engineBlocks_[(start / 16u + i) % blocks] = engine ? 1u : 0u;
    }

    bool Spu2::engineSampleAt(uint32_t byteAddress) const
    {
        std::lock_guard lock(mutex_);
        return engineBlocks_[(byteAddress % RamBytes) / 16u] != 0;
    }

    bool Spu2::voiceIsEngine(unsigned coreIndex, unsigned voice) const
    {
        std::lock_guard lock(mutex_);
        return core_[coreIndex & 1].voice[voice % Voices].engine;
    }

    int Spu2::engineSlotOf(uint32_t byteAddress)
    {
        if (byteAddress < EngineSlotBase)
            return -1;
        const uint32_t slot = (byteAddress - EngineSlotBase) / EngineSlotBytes;
        return slot < EngineSlots ? static_cast<int>(slot) : -1;
    }

    void Spu2::setMutedEngineSlots(uint32_t mask)
    {
        std::lock_guard lock(mutex_);
        mutedSlots_ = mask & ((1u << EngineSlots) - 1u);
        if (!mutedSlots_)
            mutedHeardRms_ = 0.0;
    }

    uint32_t Spu2::mutedEngineSlots() const
    {
        std::lock_guard lock(mutex_);
        return mutedSlots_;
    }

    double Spu2::mutedHeardRms() const
    {
        std::lock_guard lock(mutex_);
        return mutedHeardRms_;
    }

    int Spu2::voiceEngineSlot(unsigned coreIndex, unsigned voice) const
    {
        std::lock_guard lock(mutex_);
        return core_[coreIndex & 1].voice[voice % Voices].slot;
    }
#endif

    void Spu2::writeRam(uint32_t byteAddress, std::span<const uint8_t> bytes)
    {
        std::lock_guard lock(mutex_);
        auto *raw = reinterpret_cast<uint8_t *>(ram_.data());
        for (size_t i = 0; i < bytes.size(); ++i)
        {
            raw[(byteAddress + i) % RamBytes] = bytes[i];
        }
        checkIrq((byteAddress / 2u) & AddressMask, static_cast<uint32_t>((bytes.size() + 1u) / 2u));
    }

    void Spu2::readRam(uint32_t byteAddress, std::span<uint8_t> bytes) const
    {
        std::lock_guard lock(mutex_);
        const auto *raw = reinterpret_cast<const uint8_t *>(ram_.data());
        for (size_t i = 0; i < bytes.size(); ++i)
        {
            bytes[i] = raw[(byteAddress + i) % RamBytes];
        }
    }

    void Spu2::setVoiceParam(unsigned core, unsigned voice, VoiceParam param, uint16_t value)
    {
        std::lock_guard lock(mutex_);
        auto &v = core_[core & 1].voice[voice % Voices];
        switch (param)
        {
        case VoiceParam::VolumeLeft: v.left.raw = value; v.left.level = fixedVolume(value); v.left.counter = 0; break;
        case VoiceParam::VolumeRight: v.right.raw = value; v.right.level = fixedVolume(value); v.right.counter = 0; break;
        case VoiceParam::Pitch: v.pitch = value; break;
        case VoiceParam::Adsr1: v.adsr1 = value; break;
        case VoiceParam::Adsr2: v.adsr2 = value; break;
        case VoiceParam::Envelope: v.env.level = std::min<int32_t>(value, 0x7fff); break;
        default: break;
        }
    }

    uint16_t Spu2::voiceParam(unsigned core, unsigned voice, VoiceParam param) const
    {
        std::lock_guard lock(mutex_);
        const auto &v = core_[core & 1].voice[voice % Voices];
        switch (param)
        {
        case VoiceParam::VolumeLeft: return v.left.raw;
        case VoiceParam::VolumeRight: return v.right.raw;
        case VoiceParam::Pitch: return v.pitch;
        case VoiceParam::Adsr1: return v.adsr1;
        case VoiceParam::Adsr2: return v.adsr2;
        case VoiceParam::Envelope: return static_cast<uint16_t>(v.env.level);
        case VoiceParam::VolumeXLeft: return static_cast<uint16_t>(v.outLeft);
        case VoiceParam::VolumeXRight: return static_cast<uint16_t>(v.outRight);
        }
        return 0;
    }

    void Spu2::setVoiceAddr(unsigned core, unsigned voice, VoiceAddr which, uint32_t byteAddress)
    {
        std::lock_guard lock(mutex_);
        auto &v = core_[core & 1].voice[voice % Voices];
        const uint32_t halfword = (byteAddress / 2u) & AddressMask;
        switch (which)
        {
        case VoiceAddr::Start: v.ssa = halfword & ~7u; break;
        case VoiceAddr::LoopStart: v.lsa = halfword & ~7u; v.lsaFromBlock = false; break;
        case VoiceAddr::Next: v.nax = halfword & ~7u; v.sampleInBlock = 28; break;
        }
    }

    uint32_t Spu2::voiceAddr(unsigned core, unsigned voice, VoiceAddr which) const
    {
        std::lock_guard lock(mutex_);
        const auto &v = core_[core & 1].voice[voice % Voices];
        switch (which)
        {
        case VoiceAddr::Start: return v.ssa * 2u;
        case VoiceAddr::LoopStart: return v.lsa * 2u;
        case VoiceAddr::Next: return v.nax * 2u;
        }
        return 0;
    }

    void Spu2::setCoreMask(unsigned coreIndex, CoreMask which, uint32_t mask)
    {
        std::lock_guard lock(mutex_);
        auto &core = core_[coreIndex & 1];
        mask &= 0xffffffu;
        switch (which)
        {
        case CoreMask::PitchMod: core.pmon = mask & ~1u; break;
        case CoreMask::Noise: core.non = mask; break;
        case CoreMask::DryLeft: core.vmixl = mask; break;
        case CoreMask::DryRight: core.vmixr = mask; break;
        case CoreMask::WetLeft: core.vmixel = mask; break;
        case CoreMask::WetRight: core.vmixer = mask; break;
        case CoreMask::KeyOn:
            for (unsigned i = 0; i < Voices; ++i)
                if (mask & (1u << i))
                    keyOn(core, i);
            break;
        case CoreMask::KeyOff:
            for (unsigned i = 0; i < Voices; ++i)
                if (mask & (1u << i))
                    keyOff(core.voice[i]);
            break;
        case CoreMask::EndFlags: core.endx = 0; break; // a write clears ENDX
        }
    }

    uint32_t Spu2::coreMask(unsigned coreIndex, CoreMask which) const
    {
        std::lock_guard lock(mutex_);
        const auto &core = core_[coreIndex & 1];
        switch (which)
        {
        case CoreMask::PitchMod: return core.pmon;
        case CoreMask::Noise: return core.non;
        case CoreMask::DryLeft: return core.vmixl;
        case CoreMask::DryRight: return core.vmixr;
        case CoreMask::WetLeft: return core.vmixel;
        case CoreMask::WetRight: return core.vmixer;
        case CoreMask::EndFlags: return core.endx;
        default: return 0;
        }
    }

    void Spu2::setCoreVolume(unsigned coreIndex, CoreVolume which, uint16_t value)
    {
        std::lock_guard lock(mutex_);
        auto &core = core_[coreIndex & 1];
        switch (which)
        {
        case CoreVolume::MasterLeft: core.mvolL.raw = value; core.mvolL.level = fixedVolume(value); break;
        case CoreVolume::MasterRight: core.mvolR.raw = value; core.mvolR.level = fixedVolume(value); break;
        case CoreVolume::EffectLeft: core.evolL = value; break;
        case CoreVolume::EffectRight: core.evolR = value; break;
        case CoreVolume::ExtInputLeft: core.avolL = value; break;
        case CoreVolume::ExtInputRight: core.avolR = value; break;
        case CoreVolume::DataInputLeft: core.bvolL = value; break;
        case CoreVolume::DataInputRight: core.bvolR = value; break;
        }
    }

    uint16_t Spu2::coreVolume(unsigned coreIndex, CoreVolume which) const
    {
        std::lock_guard lock(mutex_);
        const auto &core = core_[coreIndex & 1];
        switch (which)
        {
        case CoreVolume::MasterLeft: return core.mvolL.raw;
        case CoreVolume::MasterRight: return core.mvolR.raw;
        case CoreVolume::EffectLeft: return core.evolL;
        case CoreVolume::EffectRight: return core.evolR;
        case CoreVolume::ExtInputLeft: return core.avolL;
        case CoreVolume::ExtInputRight: return core.avolR;
        case CoreVolume::DataInputLeft: return core.bvolL;
        case CoreVolume::DataInputRight: return core.bvolR;
        }
        return 0;
    }

    void Spu2::setMix(unsigned coreIndex, uint16_t mmix)
    {
        std::lock_guard lock(mutex_);
        core_[coreIndex & 1].mmix = mmix;
    }

    uint16_t Spu2::mix(unsigned coreIndex) const
    {
        std::lock_guard lock(mutex_);
        return core_[coreIndex & 1].mmix;
    }

    void Spu2::setAttr(unsigned coreIndex, uint16_t attr)
    {
        std::lock_guard lock(mutex_);
        auto &core = core_[coreIndex & 1];
        if ((core.attr & 0x40u) && !(attr & 0x40u))
        {
            pendingIrq_ &= ~(1u << (coreIndex & 1)); // clearing IRQ enable acknowledges
        }
        core.attr = attr;
    }

    uint16_t Spu2::attr(unsigned coreIndex) const
    {
        std::lock_guard lock(mutex_);
        return core_[coreIndex & 1].attr;
    }

    void Spu2::setIrqAddress(unsigned coreIndex, uint32_t byteAddress)
    {
        std::lock_guard lock(mutex_);
        core_[coreIndex & 1].irqa = (byteAddress / 2u) & AddressMask;
    }

    uint32_t Spu2::irqAddress(unsigned coreIndex) const
    {
        std::lock_guard lock(mutex_);
        return core_[coreIndex & 1].irqa * 2u;
    }

    void Spu2::setEffectArea(unsigned coreIndex, uint32_t startByte, uint32_t endByte)
    {
        std::lock_guard lock(mutex_);
        core_[coreIndex & 1].esa = (startByte / 2u) & AddressMask;
        core_[coreIndex & 1].eea = (endByte / 2u) & AddressMask;
    }

    Spu2::Status Spu2::statusAfter(unsigned samples)
    {
        std::lock_guard lock(mutex_);
        auto read = [this] {
            Status s;
            for (unsigned c = 0; c < 2; ++c)
            {
                s.endx[c] = core_[c].endx;
                for (unsigned i = 0; i < Voices; ++i)
                    s.envx[c][i] = static_cast<uint16_t>(core_[c].voice[i].env.level);
            }
            return s;
        };
        if (samples == 0)
            return read();
        // Run the voices ahead as render() does (same order: core 0 then core 1, voices in order
        // for pitch modulation), read, then put back the cores and the pending IRQs. The sound
        // RAM is only read on this path.
        const std::array<Core, 2> saved = core_;
        const uint32_t savedIrq = pendingIrq_;
        for (unsigned n = 0; n < samples; ++n)
            for (unsigned c = 0; c < 2; ++c)
            {
                stepNoise(core_[c]);
                int32_t previous = 0;
                for (unsigned i = 0; i < Voices; ++i)
                    previous = voiceSample(c, i, previous);
            }
        const Status s = read();
        core_ = saved;
        pendingIrq_ = savedIrq;
        return s;
    }

    uint32_t Spu2::takeIrqs()
    {
        std::lock_guard lock(mutex_);
        const uint32_t irqs = pendingIrq_;
        pendingIrq_ = 0;
        return irqs;
    }

    int32_t Spu2::fixedVolume(uint16_t raw)
    {
        if (raw & 0x8000u)
        {
            // Sweep: starts from the current level in stepVolume; begin at 0.
            return 0;
        }
        return static_cast<int16_t>(raw << 1);
    }

    void Spu2::stepVolume(Volume &volume)
    {
        if (!(volume.raw & 0x8000u))
        {
            return;
        }
        // Sweep mode: bit14 exponential, bit13 decrease, bit12 negative phase, bits6..0 rate.
        const bool exponential = (volume.raw & 0x4000u) != 0;
        const bool decrease = (volume.raw & 0x2000u) != 0;
        const bool negative = (volume.raw & 0x1000u) != 0;
        int32_t magnitude = std::abs(volume.level);
        magnitude = envelopeTick(magnitude, volume.counter, volume.raw & 0x7fu, exponential, decrease);
        volume.level = negative ? -magnitude : magnitude;
    }

    void Spu2::checkIrq(uint32_t halfwordAddress, uint32_t halfwords)
    {
        for (unsigned c = 0; c < 2; ++c)
        {
            const auto &core = core_[c];
            if (!(core.attr & 0x40u))
            {
                continue;
            }
            const uint32_t offset = (core.irqa - halfwordAddress) & AddressMask;
            if (offset < halfwords)
            {
                pendingIrq_ |= 1u << c;
            }
        }
    }

    void Spu2::keyOn(Core &core, unsigned index)
    {
        auto &v = core.voice[index];
        v.phase = Phase::Attack;
        v.env = {};
        v.nax = v.ssa;
        v.lsaFromBlock = true;
        v.sampleInBlock = 28;
        v.counter = 0;
        v.s1 = v.s2 = 0;
        v.history = {};
        core.endx &= ~(1u << index);
#if !defined(GT4_STOCK_BUILD)
        // Engine bass: an engine voice when its sample starts in an engine / exhaust bank (8 halfwords per block).
        v.engine = engineBlocks_[(v.ssa & AddressMask) >> 3] != 0;
        // Custom Sounds: the car slot of an engine voice.
        v.slot = static_cast<int8_t>(v.engine ? engineSlotOf((v.ssa & AddressMask) * 2u) : -1);
#endif
    }

    void Spu2::keyOff(Voice &voice)
    {
        if (voice.phase != Phase::Off)
        {
            voice.phase = Phase::Release;
            voice.env.counter = 0;
        }
    }

    void Spu2::startBlock(unsigned coreIndex, Voice &voice)
    {
        const uint16_t head = ram_[voice.nax & AddressMask];
        voice.header = static_cast<uint8_t>(head & 0xffu);
        voice.flags = static_cast<uint8_t>(head >> 8);
        if ((voice.flags & 4u) && voice.lsaFromBlock)
        {
            voice.lsa = voice.nax & AddressMask;
        }
        voice.sampleInBlock = 0;
        checkIrq(voice.nax & AddressMask, 8u);
        (void)coreIndex;
    }

    void Spu2::decodeNext(unsigned coreIndex, Voice &voice)
    {
        if (voice.sampleInBlock >= 28)
        {
            startBlock(coreIndex, voice);
        }
        const unsigned index = voice.sampleInBlock;
        const uint16_t word = ram_[(voice.nax + 1u + index / 4u) & AddressMask];
        const int32_t nibble = (word >> ((index & 3u) * 4u)) & 0xf;
        const int32_t shift = std::min(voice.header & 0xf, 12);
        const unsigned filter = std::min<unsigned>((voice.header >> 4) & 0xf, 4u);
        int32_t sample = static_cast<int16_t>(nibble << 12) >> shift;
        sample += (voice.s1 * FilterPos[filter] + voice.s2 * FilterNeg[filter] + 32) >> 6;
        sample = clamp16(sample);
        voice.s2 = voice.s1;
        voice.s1 = sample;
        voice.history[0] = voice.history[1];
        voice.history[1] = voice.history[2];
        voice.history[2] = voice.history[3];
        voice.history[3] = static_cast<int16_t>(sample);
        if (++voice.sampleInBlock < 28)
        {
            return;
        }
        // Block end: apply the loop flags of this block.
        auto &core = core_[coreIndex];
        const unsigned voiceIndex = static_cast<unsigned>(&voice - core.voice.data());
        if (voice.flags & 1u)
        {
            core.endx |= 1u << voiceIndex;
            voice.nax = voice.lsa;
            if (!(voice.flags & 2u))
            {
                voice.phase = Phase::Off;
                voice.env.level = 0;
            }
        }
        else
        {
            voice.nax = (voice.nax + 8u) & AddressMask;
        }
    }

    void Spu2::stepEnvelope(Voice &v)
    {
        switch (v.phase)
        {
        case Phase::Off:
            v.env.level = 0;
            break;
        case Phase::Attack:
            v.env.level = envelopeTick(v.env.level, v.env.counter, (v.adsr1 >> 8) & 0x7fu, (v.adsr1 & 0x8000u) != 0, false);
            if (v.env.level >= 0x7fff)
            {
                v.phase = Phase::Decay;
                v.env.counter = 0;
            }
            break;
        case Phase::Decay:
        {
            v.env.level = envelopeTick(v.env.level, v.env.counter, ((v.adsr1 >> 4) & 0xfu) << 2, true, true);
            const int32_t sustainLevel = ((v.adsr1 & 0xf) + 1) * 0x800;
            if (v.env.level <= sustainLevel)
            {
                v.phase = Phase::Sustain;
                v.env.counter = 0;
            }
            break;
        }
        case Phase::Sustain:
            v.env.level = envelopeTick(v.env.level, v.env.counter, (v.adsr2 >> 6) & 0x7fu, (v.adsr2 & 0x8000u) != 0,
                                       (v.adsr2 & 0x4000u) != 0);
            break;
        case Phase::Release:
            v.env.level = envelopeTick(v.env.level, v.env.counter, (v.adsr2 & 0x1fu) << 2, (v.adsr2 & 0x20u) != 0, true);
            if (v.env.level <= 0)
            {
                v.phase = Phase::Off;
            }
            break;
        }
    }

    void Spu2::stepNoise(Core &core)
    {
        // Noise clock from ATTR bits 13..8 (shift 4 bits, step 2 bits).
        const unsigned clock = (core.attr >> 8) & 0x3fu;
        const unsigned shift = clock >> 2;
        const int32_t step = 4 + static_cast<int32_t>(clock & 3u);
        core.noiseCount -= step;
        if (core.noiseCount >= 0)
        {
            return;
        }
        core.noiseCount += 0x20000 >> shift;
        const uint32_t level = static_cast<uint16_t>(core.noiseLevel);
        const uint32_t feedback = ((level >> 15) ^ (level >> 12) ^ (level >> 11) ^ (level >> 10) ^ 1u) & 1u;
        core.noiseLevel = static_cast<int16_t>((level << 1) | feedback);
    }

    int32_t Spu2::voiceSample(unsigned coreIndex, unsigned index, int32_t previousOutput)
    {
        auto &core = core_[coreIndex];
        auto &v = core.voice[index];
        if (v.phase == Phase::Off && v.env.level == 0)
        {
            v.output = 0;
            v.outLeft = v.outRight = 0;
            return 0;
        }
        uint32_t step = v.pitch;
        if ((core.pmon >> index) & 1u)
        {
            step = static_cast<uint32_t>((static_cast<int64_t>(step) * (32768 + previousOutput)) >> 15);
        }
        step = std::min<uint32_t>(step, 0x3fffu);
        const auto &w = spu2GaussTable[(v.counter >> 4) & 0xffu];
        int32_t sample = (w[0] * v.history[0] + w[1] * v.history[1] + w[2] * v.history[2] + w[3] * v.history[3]) >> 15;
        if ((core.non >> index) & 1u)
        {
            sample = core.noiseLevel;
        }
        v.counter += step;
        while (v.counter >= 0x1000u)
        {
            v.counter -= 0x1000u;
            decodeNext(coreIndex, v);
        }
        stepEnvelope(v);
        const int32_t enveloped = (sample * v.env.level) >> 15;
        v.output = clamp16(enveloped);
        stepVolume(v.left);
        stepVolume(v.right);
        v.outLeft = clamp16((enveloped * v.left.level) >> 15);
        v.outRight = clamp16((enveloped * v.right.level) >> 15);
        return v.output;
    }

    void Spu2::render(uint32_t frames, std::span<int16_t> stereo, std::span<const int16_t> extraDry1,
                      std::span<const int16_t> input0, std::span<const int16_t> input1, std::span<const int32_t> outputAdd)
    {
        std::lock_guard lock(mutex_);
        const bool write = stereo.size() >= size_t(frames) * 2u;
        const std::span<const int16_t> inputs[2] = {input0, input1};
#if !defined(GT4_STOCK_BUILD)
        // Test-only probe: decided once per call; off, it costs one predictable branch per voice.
        const bool probe = probeOn_;
        if (probe)
        {
            probeVoicesOnly_.assign(size_t(frames) * 2u, 0.0f);
            probeStems_.assign(probeStemsOn_ ? size_t(frames) * 4u * Voices : 0u, 0.0f);
            probeEngine_.assign(size_t(frames) * 2u, 0.0f);
            probeEngineDry_.assign(size_t(frames) * 2u, 0.0f);
            probePre_.assign(size_t(frames) * 2u, 0.0f);
            probeMuted_.assign(size_t(frames) * 2u, 0.0f);
        }
        // Engine bass: decided once per call. Off, the mix below is exactly the one without this code (the
        // engine voices' sum and the mix at full precision are only kept apart for the probe).
        const bool bass = bassLevel_ != EngineBass::Off;
        const bool split = bass || probe;
        const auto &k = bassCoef_;
        // Custom Sounds: muted car slots and the pack's output, decided once per call. With no slot muted and nothing to
        // add, the mix below is exactly the one without this code.
        const uint32_t muteMask = mutedSlots_;
        const bool mute = muteMask != 0;
        const bool adding = outputAdd.size() >= size_t(frames) * 2u;
        double mutedSq = 0.0;
#else
        (void)outputAdd;
#endif
        for (uint32_t frame = 0; frame < frames; ++frame)
        {
            int32_t ext[2] = {};
            int32_t out[2] = {};
            int32_t extGain[2] = {};
#if !defined(GT4_STOCK_BUILD)
            int64_t extFull[2] = {};      // split: core 0's output at full precision (no clamp)
            int64_t outFull[2] = {};      // split: the output at full precision (no clamp in either core)
            int32_t engineBus[2][2] = {}; // per core: the engine voices' dry sum (after the shelf when it is on)
            int32_t engineDry[2][2] = {}; // probe: the same before the shelf
            double boost[2][2] = {};      // per core: what the shelf adds (before the room scale)
            bool engines = false;         // split: an engine voice sounds in this frame
            int32_t mutedDry[2][2] = {};  // Custom Sounds: per core, the muted voices' dry sum (not mixed)
#endif
            for (unsigned c = 0; c < 2; ++c)
            {
                auto &core = core_[c];
                stepNoise(core);
                int32_t dryL = 0, dryR = 0, previous = 0;
#if !defined(GT4_STOCK_BUILD)
                int32_t engL = 0, engR = 0;
#endif
                for (unsigned i = 0; i < Voices; ++i)
                {
                    previous = voiceSample(c, i, previous);
                    const auto &v = core.voice[i];
#if !defined(GT4_STOCK_BUILD)
                    if (mute && v.slot >= 0 && ((muteMask >> v.slot) & 1u))
                    {
                        // A sound pack plays this car's engine: the voice runs on (its pitch modulation output above
                        // too, as GT4 set it up) but is left out of every mix.
                        mutedDry[c][0] += ((core.vmixl >> i) & 1u) ? v.outLeft : 0;
                        mutedDry[c][1] += ((core.vmixr >> i) & 1u) ? v.outRight : 0;
                        if (probe)
                            probeFrame_[c][i][0] = probeFrame_[c][i][1] = 0;
                        continue;
                    }
#endif
                    if ((core.vmixl >> i) & 1u)
                        dryL += v.outLeft;
                    if ((core.vmixr >> i) & 1u)
                        dryR += v.outRight;
#if !defined(GT4_STOCK_BUILD)
                    if (probe)
                    {
                        probeFrame_[c][i][0] = ((core.vmixl >> i) & 1u) ? v.outLeft : 0;
                        probeFrame_[c][i][1] = ((core.vmixr >> i) & 1u) ? v.outRight : 0;
                    }
                    if (split && v.engine)
                    {
                        engL += ((core.vmixl >> i) & 1u) ? v.outLeft : 0;
                        engR += ((core.vmixr >> i) & 1u) ? v.outRight : 0;
                    }
#endif
                }
#if !defined(GT4_STOCK_BUILD)
                engines = engines || engL != 0 || engR != 0;
                if (bass)
                {
                    // The engine voices' sum through the shelf (transposed direct form II): the dry sum becomes the
                    // other voices plus the engine voices with what the shelf adds, at the room scale.
                    const int32_t in[2] = {engL, engR};
                    for (unsigned s = 0; s < 2; ++s)
                    {
                        Shelf &f = bassShelf_[c][s];
                        const double x = in[s];
                        const double y = k[0] * x + f.z1;
                        f.z1 = k[1] * x - k[3] * y + f.z2;
                        f.z2 = k[2] * x - k[4] * y;
                        boost[c][s] = y - x;
                        const double w = x + bassScale_ * boost[c][s];
                        engineBus[c][s] = static_cast<int32_t>(w >= 0.0 ? w + 0.5 : w - 0.5);
                    }
                    dryL += engineBus[c][0] - engL;
                    dryR += engineBus[c][1] - engR;
                }
                else if (probe)
                {
                    engineBus[c][0] = engL;
                    engineBus[c][1] = engR;
                }
                if (probe)
                {
                    engineDry[c][0] = engL;
                    engineDry[c][1] = engR;
                }
#endif
                if (c == 1 && extraDry1.size() >= size_t(frames) * 2u)
                {
                    dryL += extraDry1[frame * 2u];
                    dryR += extraDry1[frame * 2u + 1u];
                }
                // MMIX dry gates (PCSX2 Mix.cpp): bit11/10 voices, bit7/6 memory input, bit3/2 external.
                int32_t mixL = (core.mmix & 0x800u) ? dryL : 0;
                int32_t mixR = (core.mmix & 0x400u) ? dryR : 0;
                if (inputs[c].size() >= size_t(frames) * 2u)
                {
                    // Sound data input: on the PS2 scaled by BVOL, which the IOP's PCM player
                    // writes from its volume model; the native player applies that gain itself.
                    if (core.mmix & 0x80u)
                        mixL += inputs[c][frame * 2u];
                    if (core.mmix & 0x40u)
                        mixR += inputs[c][frame * 2u + 1u];
                }
#if !defined(GT4_STOCK_BUILD)
                // Split: the same mix at full precision, core 0's output into core 1 without its clamp.
                int64_t mixFullL = mixL, mixFullR = mixR;
#endif
                if (c == 1)
                {
                    // External input = core 0's output, scaled by core 1's AVOL (GT4: 0x7fff from
                    // PDISPU2's boot; the EE never changes it). BVOL here silenced all of core 0.
                    extGain[0] = (core.mmix & 0x8u) ? static_cast<int16_t>(core.avolL) : 0;
                    extGain[1] = (core.mmix & 0x4u) ? static_cast<int16_t>(core.avolR) : 0;
#if !defined(GT4_STOCK_BUILD)
                    if (split)
                    {
                        mixFullL += (extFull[0] * extGain[0]) >> 15;
                        mixFullR += (extFull[1] * extGain[1]) >> 15;
                    }
#endif
                    mixL += (ext[0] * extGain[0]) >> 15;
                    mixR += (ext[1] * extGain[1]) >> 15;
                }
                stepVolume(core.mvolL);
                stepVolume(core.mvolR);
                out[0] = clamp16((clamp16(mixL) * core.mvolL.level) >> 15);
                out[1] = clamp16((clamp16(mixR) * core.mvolR.level) >> 15);
#if !defined(GT4_STOCK_BUILD)
                if (split)
                {
                    // Full precision through the cores: the limiter's input (engine bass on) and the probe's "pre".
                    outFull[0] = (mixFullL * core.mvolL.level) >> 15;
                    outFull[1] = (mixFullR * core.mvolR.level) >> 15;
                }
#endif
                if (c == 0)
                {
                    ext[0] = out[0]; // core 0 output is core 1's external input
                    ext[1] = out[1];
#if !defined(GT4_STOCK_BUILD)
                    extFull[0] = outFull[0];
                    extFull[1] = outFull[1];
#endif
                }
            }
#if !defined(GT4_STOCK_BUILD)
            // Custom Sounds: the muted voices as they would have been heard (the same paths as the boost below), and the
            // pack's output added after the cores (clamped with GT4's own, or through the engine bass limiter).
            if (mute)
            {
                const auto &c0 = core_[0];
                const auto &c1 = core_[1];
                for (unsigned s = 0; s < 2; ++s)
                {
                    const uint16_t snd = s ? 0x400u : 0x800u;
                    const double m0 = (s ? c0.mvolR.level : c0.mvolL.level) / 32768.0;
                    const double m1 = (s ? c1.mvolR.level : c1.mvolL.level) / 32768.0;
                    const double g1 = (c1.mmix & snd) ? m1 : 0.0;
                    const double g0 = (c0.mmix & snd) ? m0 * (extGain[s] / 32768.0) * m1 : 0.0;
                    const double heard = mutedDry[0][s] * g0 + mutedDry[1][s] * g1;
                    mutedSq += heard * heard;
                    if (probe)
                        probeMuted_[frame * 2u + s] = float(heard);
                }
            }
            if (adding)
            {
                const int32_t a0 = outputAdd[frame * 2u], a1 = outputAdd[frame * 2u + 1u];
                out[0] += a0;
                out[1] += a1;
                outFull[0] += a0;
                outFull[1] += a1;
                engines = engines || a0 != 0 || a1 != 0;
            }
            // GT4's own mix with its clamps (Off: always).
            int16_t mixOut[2] = {clamp16(out[0]), clamp16(out[1])};
            if (bass)
            {
                // Room for the boost: the boost reaches the output along the engine voices' path (each core's dry sum
                // through MMIX, MVOL and core 0 -> core 1's AVOL; the probe's path gains). Every 10 ms the target scale
                // is the room under the ceiling that the output without the boost has left in each of the last 40 ms
                // (the smallest of its peaks), over the boost's largest peak in them; the scale follows it.
                const auto &c0 = core_[0];
                const auto &c1 = core_[1];
                for (unsigned s = 0; s < 2; ++s)
                {
                    const uint16_t snd = s ? 0x400u : 0x800u;
                    const double m0 = (s ? c0.mvolR.level : c0.mvolL.level) / 32768.0;
                    const double m1 = (s ? c1.mvolR.level : c1.mvolL.level) / 32768.0;
                    const double g1 = (c1.mmix & snd) ? m1 : 0.0;
                    const double g0 = (c0.mmix & snd) ? m0 * (extGain[s] / 32768.0) * m1 : 0.0;
                    const double full = boost[0][s] * g0 + boost[1][s] * g1;
                    const double rest = double(outFull[s]) - bassScale_ * full;
                    bassPeakRest_[s] = std::max(bassPeakRest_[s], std::abs(rest));
                    bassPeakBoost_[s] = std::max(bassPeakBoost_[s], std::abs(full));
                }
                if (++bassChunkAt_ >= EngineBassRoomChunk)
                {
                    bassChunkAt_ = 0;
                    double target = 1.0;
                    for (unsigned s = 0; s < 2; ++s)
                    {
                        bassRest_[s][bassChunkSlot_] = bassPeakRest_[s];
                        bassBoost_[s][bassChunkSlot_] = bassPeakBoost_[s];
                        bassPeakRest_[s] = bassPeakBoost_[s] = 0.0;
                        const double held = *std::min_element(bassRest_[s].begin(), bassRest_[s].end());
                        const double peak = *std::max_element(bassBoost_[s].begin(), bassBoost_[s].end());
                        if (peak > 1.0)
                            target = std::min(target, std::clamp((EngineBassCeiling - held) / peak, 0.0, 1.0));
                    }
                    bassChunkSlot_ = (bassChunkSlot_ + 1u) % EngineBassRoomChunks;
                    bassTarget_ = target;
                }
                bassScale_ = bassTarget_ < bassScale_ ? std::max(bassTarget_, bassScale_ - BassAttackStep) : std::min(bassTarget_, bassScale_ + BassReleaseStep);
                // The limiter while engine voices sound (and EngineBassHoldFrames after), GT4's own clamped mix
                // otherwise; it changes over only at a frame where both give the same samples.
                if (engines)
                    bassHold_ = EngineBassHoldFrames;
                else if (bassHold_)
                    --bassHold_;
                const int16_t soft[2] = {engineBassLimit(static_cast<int32_t>(std::clamp<int64_t>(outFull[0], -INT32_MAX, INT32_MAX))),
                                         engineBassLimit(static_cast<int32_t>(std::clamp<int64_t>(outFull[1], -INT32_MAX, INT32_MAX)))};
                if (bassLimiting_ != (bassHold_ > 0) && soft[0] == mixOut[0] && soft[1] == mixOut[1])
                    bassLimiting_ = bassHold_ > 0;
                if (bassLimiting_)
                {
                    mixOut[0] = soft[0];
                    mixOut[1] = soft[1];
                }
            }
#endif
            if (write)
            {
#if !defined(GT4_STOCK_BUILD)
                stereo[frame * 2u] = mixOut[0];
                stereo[frame * 2u + 1u] = mixOut[1];
#else
                stereo[frame * 2u] = clamp16(out[0]);
                stereo[frame * 2u + 1u] = clamp16(out[1]);
#endif
            }
#if !defined(GT4_STOCK_BUILD)
            if (probe)
            {
                for (unsigned s = 0; s < 2; ++s)
                {
                    const int64_t a = outFull[s] < 0 ? -outFull[s] : outFull[s];
                    probe_.prePeak = std::max<int32_t>(probe_.prePeak, static_cast<int32_t>(std::min<int64_t>(a, INT32_MAX)));
                    probe_.overFull += a > 32767;
                    probe_.limited += int64_t(mixOut[s]) != outFull[s];
                    probePre_[frame * 2u + s] = float(outFull[s]);
                }
                if (bass)
                {
                    probe_.limiterFrames += bassLimiting_ ? 1u : 0u;
                    probe_.bassScaleMin = std::min(probe_.bassScaleMin, bassScale_);
                    probe_.bassScaleSum += bassScale_;
                    probe_.bassBackedOff += bassScale_ < 0.99;
                }
                // Linear gain from each core's voice dry sum to the output (clamps ignored).
                const auto &c0 = core_[0];
                const auto &c1 = core_[1];
                double gain[2][2];
                for (unsigned s = 0; s < 2; ++s)
                {
                    const uint16_t snd = s ? 0x400u : 0x800u;
                    const double m0 = (s ? c0.mvolR.level : c0.mvolL.level) / 32768.0;
                    const double m1 = (s ? c1.mvolR.level : c1.mvolL.level) / 32768.0;
                    gain[1][s] = (c1.mmix & snd) ? m1 : 0.0;
                    gain[0][s] = (c0.mmix & snd) ? m0 * (extGain[s] / 32768.0) * m1 : 0.0;
                }
                double sumL = 0.0, sumR = 0.0;
                for (unsigned c = 0; c < 2; ++c)
                    for (unsigned i = 0; i < Voices; ++i)
                    {
                        auto &pv = probe_.voice[c][i];
                        const int32_t ol = probeFrame_[c][i][0], orr = probeFrame_[c][i][1];
                        if (core_[c].voice[i].phase != Phase::Off)
                            ++pv.activeFrames;
                        if (!ol && !orr)
                            continue;
                        const double hl = ol * gain[c][0], hr = orr * gain[c][1];
                        pv.ownSq += (double(ol) * ol + double(orr) * orr) * 0.5;
                        pv.heardSq += (hl * hl + hr * hr) * 0.5;
                        pv.heardPeak = std::max({pv.heardPeak, std::abs(hl), std::abs(hr)});
                        sumL += hl;
                        sumR += hr;
                        if (probeStemsOn_)
                        {
                            const size_t at = ((size_t(frame) * 2u + c) * Voices + i) * 2u;
                            probeStems_[at] = float(hl);
                            probeStems_[at + 1u] = float(hr);
                        }
                    }
                probeVoicesOnly_[frame * 2u] = float(sumL);
                probeVoicesOnly_[frame * 2u + 1u] = float(sumR);
                probe_.voicesSq += (sumL * sumL + sumR * sumR) * 0.5;
                // The engine voices as heard: each core's engine sum (after the shelf when it is on) along the same paths.
                const double el = engineBus[0][0] * gain[0][0] + engineBus[1][0] * gain[1][0];
                const double er = engineBus[0][1] * gain[0][1] + engineBus[1][1] * gain[1][1];
                probeEngine_[frame * 2u] = float(el);
                probeEngine_[frame * 2u + 1u] = float(er);
                probeEngineDry_[frame * 2u] = float(engineDry[0][0] * gain[0][0] + engineDry[1][0] * gain[1][0]);
                probeEngineDry_[frame * 2u + 1u] = float(engineDry[0][1] * gain[0][1] + engineDry[1][1] * gain[1][1]);
                probe_.engineSq += (el * el + er * er) * 0.5;
                probe_.path = {{{gain[0][0], gain[0][1]}, {gain[1][0], gain[1][1]}}};
                ++probe_.frames;
            }
#endif
        }
#if !defined(GT4_STOCK_BUILD)
        if (mute)
            mutedHeardRms_ = frames ? std::sqrt(mutedSq / (2.0 * frames)) : 0.0;
        // A shelf left with nothing to play decays towards denormal numbers, which are slow: flush what is far below
        // one sample step.
        if (bass)
            for (auto &core : bassShelf_)
                for (Shelf &f : core)
                {
                    if (std::abs(f.z1) < 1e-9)
                        f.z1 = 0.0;
                    if (std::abs(f.z2) < 1e-9)
                        f.z2 = 0.0;
                }
#endif
    }

#if !defined(GT4_STOCK_BUILD)
    void Spu2::setProbe(bool on, bool stems)
    {
        std::lock_guard lock(mutex_);
        probeOn_ = on;
        probeStemsOn_ = on && stems;
        probe_ = {};
    }

    void Spu2::takeProbe(Probe &out, std::array<std::array<VoiceState, Voices>, 2> &state)
    {
        std::lock_guard lock(mutex_);
        out = probe_;
        probe_ = {};
        for (unsigned c = 0; c < 2; ++c)
            for (unsigned i = 0; i < Voices; ++i)
            {
                const auto &v = core_[c].voice[i];
                auto &s = state[c][i];
                s.phase = static_cast<uint8_t>(v.phase);
                s.env = v.env.level;
                s.volL = v.left.raw;
                s.volR = v.right.raw;
                s.pitch = v.pitch;
                s.adsr1 = v.adsr1;
                s.adsr2 = v.adsr2;
                s.ssa = v.ssa * 2u;
                s.lsa = v.lsa * 2u;
                s.nax = v.nax * 2u;
                s.engine = v.engine;
            }
    }
#endif
}
