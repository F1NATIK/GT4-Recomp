#pragma once

// Software SPU2 for the GT4 build: 2 MiB sound RAM, two cores of 24 voices,
// VAG ADPCM decode with loop flags, 4-tap Gaussian interpolation, ADSR and
// volume sweeps, pitch modulation, noise, ENDX, IRQ address matching, voice
// mixing switches and core volumes. Driven at the libsd register level by the
// GT4 PDISPU2 / PDISTR services and rendered by the host's 48 kHz output clock.
// Core routing as on the PS2 (PCSX2 SPU2 spu2sys.cpp / Mixer.cpp): core 0's output
// (after its MVOL) is core 1's external input, scaled by core 1's AVOL (0x768,
// "core external input volume") and gated by core 1's MMIX bits 3/2; BVOL (0x76c)
// is each core's sound data (memory / ADMA) input volume, gated by MMIX bits 7/6.
// Written for GT4 from the hardware documentation (nocash psx-spx / PCSX2
// behaviour notes); GT3's movie-audio voice engine is not used.
//
// Not modelled yet: the reverb (effect) unit; effect sends are mixed dry-less,
// i.e. only the dry path is heard.

#include <array>
#include <cstdint>
#include <mutex>
#include <span>
#include <vector>
#if !defined(GT4_STOCK_BUILD)
#include <cctype>
#include <string_view>
#endif

namespace ps2x::iop::gt4
{
    class Spu2
    {
    public:
        static constexpr uint32_t RamBytes = 2u * 1024u * 1024u;
        static constexpr unsigned Voices = 24;

        enum class VoiceParam { VolumeLeft, VolumeRight, Pitch, Adsr1, Adsr2, Envelope, VolumeXLeft, VolumeXRight };
        enum class VoiceAddr { Start, LoopStart, Next };
        enum class CoreMask { PitchMod, Noise, DryLeft, DryRight, WetLeft, WetRight, KeyOn, KeyOff, EndFlags };
        // ExtInput = AVOL (0x768/0x76a): core 1's external input (core 0's output) volume.
        // DataInput = BVOL (0x76c/0x76e): the sound data (memory / ADMA) input volume.
        enum class CoreVolume { MasterLeft, MasterRight, EffectLeft, EffectRight, ExtInputLeft, ExtInputRight, DataInputLeft, DataInputRight };

        Spu2();
        void reset();

        void writeRam(uint32_t byteAddress, std::span<const uint8_t> bytes);
        void readRam(uint32_t byteAddress, std::span<uint8_t> bytes) const;

        void setVoiceParam(unsigned core, unsigned voice, VoiceParam param, uint16_t value);
        uint16_t voiceParam(unsigned core, unsigned voice, VoiceParam param) const;
        void setVoiceAddr(unsigned core, unsigned voice, VoiceAddr which, uint32_t byteAddress);
        uint32_t voiceAddr(unsigned core, unsigned voice, VoiceAddr which) const;

        // KeyOn / KeyOff take effect immediately; EndFlags is read-only (cleared by key-on).
        void setCoreMask(unsigned core, CoreMask which, uint32_t mask);
        uint32_t coreMask(unsigned core, CoreMask which) const;
        void setCoreVolume(unsigned core, CoreVolume which, uint16_t value);
        uint16_t coreVolume(unsigned core, CoreVolume which) const;
        void setMix(unsigned core, uint16_t mmix);
        uint16_t mix(unsigned core) const;
        void setAttr(unsigned core, uint16_t attr);
        uint16_t attr(unsigned core) const;
        void setIrqAddress(unsigned core, uint32_t byteAddress);
        uint32_t irqAddress(unsigned core) const;
        void setEffectArea(unsigned core, uint32_t startByte, uint32_t endByte);

        // Pending IRQs (bit per core) raised since the last take.
        uint32_t takeIrqs();
        // ENDX and every voice's ENVX as they will read `samples` output samples from now, both
        // at the same instant: the voices run ahead that far exactly as render() runs them (ADPCM
        // decode and loop flags, envelopes, pitch modulation), then everything is put back, so
        // nothing changes what render() plays or the IRQs it raises. 0 = as they read now.
        struct Status
        {
            std::array<uint32_t, 2> endx{};
            std::array<std::array<uint16_t, Voices>, 2> envx{};
        };
        Status statusAfter(unsigned samples);
        // Advance `frames` 48 kHz samples into interleaved stereo (may be empty).
        // Optional per-frame interleaved stereo sources (each 2*frames or empty):
        // extraDry1 is added to core 1's voice dry sum (as streamed voices), and
        // input[c] is core c's sound data input (ADMA) as the native PCM player
        // delivers it, already at the volume its model writes to BVOL; gated by MMIX.
        // outputAdd (development and Full builds only; Custom Sounds): interleaved stereo added to the final output
        // (after both cores' MVOL, before the last clamp or the engine bass limiter); empty: nothing added.
        void render(uint32_t frames, std::span<int16_t> stereo, std::span<const int16_t> extraDry1 = {},
                    std::span<const int16_t> input0 = {}, std::span<const int16_t> input1 = {}, std::span<const int32_t> outputAdd = {});

#if !defined(GT4_STOCK_BUILD)
        // Engine bass (the Full edition's "Engine bass", GT4_ENGINE_BASS in gt4_sound.cpp; compiled out of the
        // Classic build): a low shelf on GT4's engine voices only, then, while engine voices sound, a soft limiter
        // on the output.
        //  - Engine voices: a voice keyed on with its sample start address (SSA) in SPU2 RAM that GT4 last
        //    filled from its car engine / exhaust banks (GT4.VOL sound/engine/*, sound/normal0-3/* and
        //    sound/turbo0-3/*; marked by markEngineSamples from gt4_sound.cpp's STRP transfers; every other write
        //    unmarks). Decided at key-on.
        //  - The shelf: RBJ low shelf (S = 1) at EngineBassCornerHz, up to +3 / +6 / +9 dB (half that at the
        //    corner), run once per core and side on the sum of that core's engine voices (the same as filtering
        //    each voice and adding them up: the filter is linear; no per-voice state, so a key-on cannot make it
        //    click). The voices' dry gates (VMIXL/R) apply as to any voice; the result joins the core's dry sum
        //    before MMIX and MVOL.
        //  - Room for the boost: what the shelf adds is scaled down (and only that: never the other sounds) when
        //    GT4's own output has stayed loud for a while. Per side and per 10 ms (EngineBassRoomChunk frames) the
        //    peaks of the output without the boost and of the boost alone at the output are kept for the last
        //    EngineBassRoomChunks intervals; the room is (EngineBassCeiling - the smallest of the output's peaks) /
        //    the largest of the boost's, so an over that lasts under 20 ms (a bump, a crash) never makes it dip
        //    and one that lasts 40 ms or more always does; the scale follows within 5 ms (down) and 0.5 s (up).
        //    With all six engines at full revs at a race start GT4's own mix is past full scale for seconds, and
        //    the boost waits there.
        //  - While an engine voice sounds (and EngineBassHoldFrames after), the cores mix at full precision (no
        //    16-bit clamp between the stages) and the output goes through engineBassLimit: unchanged up to the
        //    knee, then a smooth curve that never reaches its ceiling, so GT4's own overs in a race are rounded off
        //    instead of cut. Otherwise (menus, music and movies outside a race) the output is GT4's own mix with its
        //    clamps, sample for sample; the switch between the two is made only at a frame where both give the
        //    same samples, so it cannot click. Off (the default, and always without GT4_ENGINE_BASS) mixes exactly
        //    as without this code.
        enum class EngineBass : uint8_t { Off, Low, Medium, High };
        static constexpr double EngineBassCornerHz = 150.0;
        static constexpr double engineBassGainDb(EngineBass level)
        {
            return level == EngineBass::Low ? 3.0 : level == EngineBass::Medium ? 6.0 : level == EngineBass::High ? 9.0 : 0.0;
        }
        // The limiter: |x| <= Knee unchanged; above, Knee + (Ceiling - Knee) * tanh((|x| - Knee) / (Ceiling - Knee)):
        // slope 1 and no bend at the knee, never reaching Ceiling (-0.2 dBFS). The limiter itself has no memory;
        // the room stage above is what can hold the boost back.
        static constexpr int32_t EngineBassKnee = 24576;    // 0.75 of full scale, -2.5 dBFS
        static constexpr int32_t EngineBassCeiling = 32000; // -0.2 dBFS; also the level the room stage keeps under
        static constexpr uint32_t EngineBassRoomChunk = 480;   // 10 ms at 48 kHz
        static constexpr unsigned EngineBassRoomChunks = 4;    // the room looks at the last 40 ms
        static constexpr uint32_t EngineBassHoldFrames = 4800; // the limiter stays 100 ms after the engines go quiet
        static int16_t engineBassLimit(int32_t sample);
        void setEngineBass(EngineBass level);
        EngineBass engineBass() const;
        // [byteAddress, +bytes) now holds GT4's engine / exhaust samples (engine = true) or anything else (false).
        // Kept per 16-byte ADPCM block; a block takes the last write that touched it.
        void markEngineSamples(uint32_t byteAddress, uint32_t bytes, bool engine);
        bool engineSampleAt(uint32_t byteAddress) const;
        bool voiceIsEngine(unsigned core, unsigned voice) const;

        // Custom Sounds (GT4_SOUND_PACK / GT4_SOUND_PACK_MAP in gt4_sound.cpp; analysis/design/sound-packs/README.md):
        // GT4 streams each race car's engine banks into its own 0x25800-byte slot of SPU2 RAM, car i (the EE's race car
        // order, D + 0x6698 + i * 0x10a0) at 0xac000 + i * 0x25800 (measured: every car's revs follow the pitch of the
        // engine voices in its own slot only). setMutedEngineSlots: the engine voices (engine bass's rule) keyed on with
        // their sample start in a slot whose bit is set are left out of the mix (they still run, so ENDX / ENVX and
        // everything GT4 reads stay exactly as without this); 0 (the default) mixes exactly as without this code.
        static constexpr uint32_t EngineSlotBase = 0xac000u, EngineSlotBytes = 0x25800u;
        static constexpr unsigned EngineSlots = 8;
        static int engineSlotOf(uint32_t byteAddress); // -1 outside the slots
        void setMutedEngineSlots(uint32_t mask);
        uint32_t mutedEngineSlots() const;
        // The muted voices as they would have been heard in the last render call: RMS over both sides (16-bit units,
        // through VMIXL/R, MMIX, MVOL and core 0 -> core 1's AVOL, as the engine bass's room stage measures the boost).
        double mutedHeardRms() const;
        int voiceEngineSlot(unsigned core, unsigned voice) const; // -1: not an engine voice in a slot

        // Test-only probe (GT4_SPU_TRACE in gt4_sound.cpp; compiled out of the Classic build): what
        // each voice contributes to the final output, through the same routing the mixer uses
        // (VMIXL/R, MMIX, MVOL, and core 0 -> core 1's external input).
        struct ProbeVoice
        {
            double ownSq = 0.0;   // sum of the voice's own output (after its VOLL/VOLR), (L^2 + R^2) / 2
            double heardSq = 0.0; // the same after the core routing, as it reaches the output
            double heardPeak = 0.0;
            uint32_t activeFrames = 0; // frames not Off
        };
        struct Probe
        {
            uint32_t frames = 0;
            std::array<std::array<ProbeVoice, Voices>, 2> voice{};
            double voicesSq = 0.0;                // all voices as heard (stereo mean square sum)
            std::array<std::array<double, 2>, 2> path{}; // last per-core L/R gain from the voice dry sum to the output
            // The engine voices as heard, after the engine bass shelf (stereo mean square sum); and the same
            // measures with the engine bass on or off, against the mix at full precision (no 16-bit clamp anywhere,
            // the "pre" output): the output samples that differ from it (changed by GT4's clamps or the limiter),
            // those of it past 16 bits, and its largest sample.
            double engineSq = 0.0;
            uint32_t limited = 0, overFull = 0;
            int32_t prePeak = 0;          // largest |pre| (the cores' clamps or the limiter not applied)
            uint32_t limiterFrames = 0;   // frames the output went through the limiter (engine bass on, engines sounding)
            double bassScaleMin = 1.0;    // the smallest scale of the boost (1 = the full shelf)
            double bassScaleSum = 0.0;    // the scale summed over the frames (engine bass on)
            uint32_t bassBackedOff = 0;   // frames with the boost scaled below 0.99
        };
        struct VoiceState
        {
            uint8_t phase = 0;
            int32_t env = 0;
            uint16_t volL = 0, volR = 0, pitch = 0, adsr1 = 0, adsr2 = 0;
            uint32_t ssa = 0, lsa = 0, nax = 0; // byte addresses
            bool engine = false;                // an engine voice (engine bass)
        };
        // stems: also record each voice's heard output per frame (stereo: 96 channels, voice k of the 48
        // (core 0 voices 0-23, then core 1) left in channel 2k, right in 2k+1).
        void setProbe(bool on, bool stems);
        // Voices-only heard stereo output and the per-voice stems of the last render call (each voice before the
        // engine bass shelf), and the engine voices as heard after the shelf and before it (stereo).
        const std::vector<float> &probeVoicesOnly() const { return probeVoicesOnly_; }
        const std::vector<float> &probeStems() const { return probeStems_; }
        const std::vector<float> &probeEngine() const { return probeEngine_; }
        const std::vector<float> &probeEngineDry() const { return probeEngineDry_; }
        // The output at full precision before GT4's clamps or the limiter (stereo; "pre" above).
        const std::vector<float> &probePre() const { return probePre_; }
        // Custom Sounds: the muted voices as they would have been heard (stereo; zeros when no slot is muted).
        const std::vector<float> &probeMuted() const { return probeMuted_; }
        // Moves the accumulated sums out (and clears them); fills the current voice registers.
        void takeProbe(Probe &out, std::array<std::array<VoiceState, Voices>, 2> &state);
#endif

    private:
        enum class Phase : uint8_t { Off, Attack, Decay, Sustain, Release };
        struct Envelope
        {
            int32_t level = 0;
            uint32_t counter = 0;
        };
        struct Volume
        {
            uint16_t raw = 0;
            int32_t level = 0;
            uint32_t counter = 0;
        };
        struct Voice
        {
            Volume left, right;
            uint16_t pitch = 0, adsr1 = 0, adsr2 = 0;
            uint32_t ssa = 0, lsa = 0, nax = 0; // halfword addresses
            bool lsaFromBlock = true;
            Phase phase = Phase::Off;
            Envelope env;
            uint32_t counter = 0;
            uint8_t header = 0, flags = 0;
            unsigned sampleInBlock = 28;
            int32_t s1 = 0, s2 = 0;
            std::array<int16_t, 4> history{};
            int16_t output = 0;
            int16_t outLeft = 0, outRight = 0;
#if !defined(GT4_STOCK_BUILD)
            bool engine = false; // engine bass: keyed on with its SSA in an engine / exhaust bank
            int8_t slot = -1;    // Custom Sounds: an engine voice's car slot (engineSlotOf its SSA at key-on)
#endif
        };
        struct Core
        {
            std::array<Voice, Voices> voice{};
            uint32_t pmon = 0, non = 0, vmixl = 0, vmixr = 0, vmixel = 0, vmixer = 0, endx = 0;
            Volume mvolL, mvolR;
            uint16_t evolL = 0, evolR = 0, avolL = 0, avolR = 0, bvolL = 0, bvolR = 0;
            uint16_t mmix = 0, attr = 0;
            uint32_t irqa = 0; // halfwords
            uint32_t esa = 0, eea = 0;
            int32_t noiseLevel = 1, noiseCount = 0;
        };

        void keyOn(Core &core, unsigned index);
        void keyOff(Voice &voice);
        void startBlock(unsigned coreIndex, Voice &voice);
        void decodeNext(unsigned coreIndex, Voice &voice);
        int32_t voiceSample(unsigned coreIndex, unsigned index, int32_t previousOutput);
        static void stepEnvelope(Voice &voice);
        void checkIrq(uint32_t halfwordAddress, uint32_t halfwords);
        static void stepVolume(Volume &volume);
        static int32_t fixedVolume(uint16_t raw);
        void stepNoise(Core &core);

        mutable std::mutex mutex_;
        std::vector<uint16_t> ram_;
        std::array<Core, 2> core_{};
        uint32_t pendingIrq_ = 0;
#if !defined(GT4_STOCK_BUILD)
        // Engine bass: the level, the shelf's coefficients (b0 b1 b2 a1 a2, a0 = 1) and its state per core and
        // side (transposed direct form II), and which 16-byte blocks of SPU2 RAM hold engine / exhaust samples.
        struct Shelf
        {
            double z1 = 0.0, z2 = 0.0;
        };
        EngineBass bassLevel_ = EngineBass::Off;
        std::array<double, 5> bassCoef_{1.0, 0.0, 0.0, 0.0, 0.0};
        std::array<std::array<Shelf, 2>, 2> bassShelf_{};
        // Room for the boost: its scale and target, and per side the peaks of the output without the boost and of
        // the boost alone at the output in this 10 ms interval and in the last EngineBassRoomChunks ones.
        double bassScale_ = 1.0, bassTarget_ = 1.0;
        uint32_t bassChunkAt_ = 0;
        unsigned bassChunkSlot_ = 0;
        std::array<double, 2> bassPeakRest_{}, bassPeakBoost_{};
        std::array<std::array<double, EngineBassRoomChunks>, 2> bassRest_{}, bassBoost_{};
        // The limiter in use (else GT4's clamps), and the frames it stays after the engines went quiet.
        bool bassLimiting_ = false;
        uint32_t bassHold_ = 0;
        void resetEngineBassState(); // the shelf, the room and the limiter's state (under mutex_)
        std::vector<uint8_t> engineBlocks_ = std::vector<uint8_t>(RamBytes / 16u, 0);
        uint32_t mutedSlots_ = 0;    // Custom Sounds: the car slots whose engine voices are left out of the mix
        double mutedHeardRms_ = 0.0; // and those voices as they would have been heard in the last render call
        bool probeOn_ = false, probeStemsOn_ = false;
        Probe probe_;
        std::vector<float> probeVoicesOnly_, probeStems_, probeEngine_, probeEngineDry_, probePre_, probeMuted_;
        int32_t probeFrame_[2][Voices][2] = {}; // one frame's voice outputs (all written each probed frame)
#endif
    };

    // PDISPU2 (GT4's IOP sound driver) status timing, for gt4_sound.cpp's SPU fn3 / fn4 replies:
    // how many 48 kHz samples ahead the ENDX / ENVX status it answers is read (Spu2::statusAfter).
    //  - fn4 (voice command) with a key-on or key-off (core flags 0x800 / 0x1000 in either core's
    //    block): PDISPU2 0x1188 queues the switches for its KON/KOFF applier thread (priority 9) and
    //    calls DelayThread(100) (steps 1 and 6), sets the key-on volumes (step 7) and only then
    //    reads the status (step 8; analysis/phase4/specs/sound-stream.md 3.2). 100 us is 4.8 samples
    //    of the 48 kHz clock, plus the thread switches, step 7 and the 50 status reads: at least
    //    5 samples (the exact delay on a PS2 is inferred, not measured).
    //  - any other fn4, and fn3 (status only): read at once.
    inline constexpr unsigned PdiSpu2StatusDelaySamples = 5;
    constexpr unsigned pdiSpu2StatusAhead(unsigned function, uint32_t core0Flags, uint32_t core1Flags)
    {
        return function == 4 && ((core0Flags | core1Flags) & 0x1800u) != 0 ? PdiSpu2StatusDelaySamples : 0u;
    }

#if !defined(GT4_STOCK_BUILD)
    // GT4_ENGINE_BASS (gt4_sound.cpp): off, low, medium or high, in any case, spaces around ignored. Absent or empty:
    // off. Anything else: off, with `known` false (GT4.exe logs it).
    inline Spu2::EngineBass parseEngineBass(const char *value, bool &known)
    {
        known = true;
        std::string_view v = value ? std::string_view(value) : std::string_view();
        while (!v.empty() && (v.front() == ' ' || v.front() == '\t'))
            v.remove_prefix(1);
        while (!v.empty() && (v.back() == ' ' || v.back() == '\t' || v.back() == '\r' || v.back() == '\n'))
            v.remove_suffix(1);
        auto is = [&](std::string_view word) {
            if (v.size() != word.size())
                return false;
            for (size_t i = 0; i < v.size(); ++i)
                if (static_cast<char>(std::tolower(static_cast<unsigned char>(v[i]))) != word[i])
                    return false;
            return true;
        };
        if (v.empty() || is("off"))
            return Spu2::EngineBass::Off;
        if (is("low"))
            return Spu2::EngineBass::Low;
        if (is("medium"))
            return Spu2::EngineBass::Medium;
        if (is("high"))
            return Spu2::EngineBass::High;
        known = false;
        return Spu2::EngineBass::Off;
    }
    inline const char *engineBassName(Spu2::EngineBass level)
    {
        return level == Spu2::EngineBass::Low ? "low" : level == Spu2::EngineBass::Medium ? "medium" : level == Spu2::EngineBass::High ? "high" : "off";
    }

    // GT4's car engine and exhaust sample banks (gt4_sound.cpp marks what STRP fn5 streams from them). Per car GT4 loads
    // sound/engine/<id> and a second bank sound/<normal|turbo><k>/<id> (CORE.elf's table "normal0" .. "normal3", "turbo0"
    // .. "turbo3" next to "sound/engine/%05d" and "sound/%s/%05d"). On the USA disc (SCUS-97328,
    // analysis/disc/vol-files.csv) they are two runs of whole sectors with no other file inside: image LSN 565,192 up to
    // 643,224 (sound/engine/*, 940 files, then from 597,080 sound/normal0-3/*, 1,559 files) and 643,225 up to 674,112
    // (sound/turbo0-3/*, 1,075 files); only sound/sdvol.dat (LSN 643,224) lies between them. A transfer of `bytes`
    // from image byte `first` belongs to a bank when it lies wholly inside one of the two runs.
    enum class EngineBank : uint8_t { None, Engine, Normal, Turbo };
    inline constexpr uint64_t EngineBankFirstByte = 565192ull * 2048u, NormalBankFirstByte = 597080ull * 2048u,
                              NormalBankEndByte = 643224ull * 2048u, TurboBankFirstByte = 643225ull * 2048u,
                              TurboBankEndByte = 674112ull * 2048u;
    inline EngineBank engineBankOf(uint64_t first, uint64_t bytes)
    {
        if (bytes == 0)
            return EngineBank::None;
        if (first >= EngineBankFirstByte && first + bytes <= NormalBankEndByte)
            return first < NormalBankFirstByte ? EngineBank::Engine : EngineBank::Normal;
        if (first >= TurboBankFirstByte && first + bytes <= TurboBankEndByte)
            return EngineBank::Turbo;
        return EngineBank::None;
    }
#endif
}
