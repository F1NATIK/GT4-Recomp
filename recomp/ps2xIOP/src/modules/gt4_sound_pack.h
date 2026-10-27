#pragma once

// Custom Sounds (the Full edition's Sound page > Sound packs; analysis/design/sound-packs/README.md): a user's engine sound
// pack (analysis/design/sound-packs/inputs/CUSTOM-SOUNDS-PLAN.md, pack.json format 1, made by sound_pack.py import) mixed
// into GT4's native SPU2 output in place of GT4's own engine voices for the cars that have one. The mixer follows
// sound_pack.py's cmd_preview (the reference): per sample it smooths the revs and the throttle, crossfades the two rpm
// layers either side of the revs (equal power, each played at rpm / layer rpm), blends the on- and off-throttle layers
// (on * throttle + off * (1 - throttle); a pack with no off layers uses the on layers at 0.35), fires random pops on a
// lift-off and on a downshift blip, then adds a 120 Hz low-pass times (10^(bass_db / 20) - 1) and soft-limits with tanh.
// Compiled out of the Classic build; nothing here runs unless GT4_SOUND_PACK or GT4_SOUND_PACK_MAP is set.
#if !defined(GT4_STOCK_BUILD)

#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <random>
#include <string>
#include <vector>

namespace ps2x::iop::gt4::soundpack
{
    namespace fs = std::filesystem;

    struct Loop
    {
        std::vector<float> data; // mono, the file's own rate
        double rate = 44100.0;   // the file's sample rate
        double rpm = 0.0;
    };

    struct Pack
    {
        fs::path folder;
        std::string name;
        int cylinders = 0;
        std::vector<Loop> on, off; // sorted by rpm
        std::vector<Loop> pops;    // rpm unused
        double volume = 1.0, bassDb = 0.0, popChance = 0.6, popVolume = 0.8;
    };

    // pack.json (format 1) and its WAV files (PCM 8/16/24/32-bit or float, any rate and channel count: mixed to mono).
    // False with a player-readable reason when the folder holds no usable pack (no pack.json, a bad format, no
    // on-throttle loop, a missing or unreadable file).
    bool loadPack(const fs::path &folder, Pack &out, std::string &error);

    // A WAV file as mono float samples at its own rate (the reader the pack uses; exposed for the host test).
    bool readWav(const fs::path &file, std::vector<float> &samples, double &rate, std::string &error);

    // GT4_SOUND_PACK_MAP's file: one line per car, "<GENERIC_CAR code> <pack folder>" (the folder may hold spaces; it runs
    // to the end of the line), or "all <pack folder>"; # starts a comment; blank lines are skipped. Unknown lines are
    // reported in `warnings` and skipped.
    struct PackMap
    {
        std::string all;                    // the pack for every car (empty: none)
        std::map<uint32_t, std::string> car; // car code -> pack folder
    };
    bool parsePackMap(const std::string &text, PackMap &out, std::vector<std::string> &warnings);

    // A pack folder named in a key (UTF-8): an absolute path as it is; otherwise relative to `base` (GT4.exe's
    // folder\sound-packs).
    fs::path resolvePackFolder(const std::string &value, const fs::path &base);
    std::string pathUtf8(const fs::path &p);
    // GT4.exe's own folder (gt4_sound_pack.cpp; the current folder where it cannot be told).
    fs::path exeFolder();

    // One car's engine from its pack, at 48 kHz.
    class Mixer
    {
    public:
        static constexpr double OutRate = 48000.0;
        // outRate: GT4's 48 kHz (the host test also renders at cmd_preview's 44.1 kHz to compare sample for sample).
        Mixer(std::shared_ptr<const Pack> pack, uint32_t seed, double outRate = OutRate);
        const Pack &pack() const { return *pack_; }

        // A new race frame's state (EE thread values, given on the sound thread): the revs, the engine's throttle 0..1 (the
        // on / off blend), the gear and the driver's pedal 0..1 (lift-offs; the engine's throttle also drops at every
        // upshift while the pedal stays down). A lift-off (the pedal above 0.6 in the last 0.1 s, now below 0.2, the
        // revs above 4000) or a downshift blip (a lower forward gear with the revs above 3000, or the pedal from below 0.2
        // to 0.5..0.9) fires pops with the pack's pop_chance: 1 to 5 (lift) or 1 to 3 (blip), 0.05 to 0.2 s later, 0.03
        // to 0.15 s apart, at 0.4 to 1.0 x pop_volume.
        void control(double rpm, double throttle, int gear, double pedal);
        // The engine alone (before any gain), frames mono samples in -0.85..0.85 (tanh(1.2 v) x 0.85, as cmd_preview).
        void render(float *mono, uint32_t frames);
        // Restart the smoothing from these values (a new race, or the car's first frame).
        void reset(double rpm, double throttle);
        uint64_t popsFired() const { return popsFired_; }
        uint64_t popTriggers() const { return popTriggers_; }

    private:
        struct Voice
        {
            const Loop *loop = nullptr;
            double pos = 0.0;
            double next(double ratio); // ratio = output-rate speed (rpm / layer rpm x file rate / 48 kHz)
            void skip(double ratio);
        };
        double blend(std::vector<Voice> &voices, double rpm);
        void firePops(bool lift);

        std::shared_ptr<const Pack> pack_;
        std::vector<Voice> on_, off_;
        std::mt19937 rng_;
        double rpmTarget_ = 1000.0, thrTarget_ = 0.0, rpmS_ = 1000.0, thrS_ = 0.0, pedal_ = 0.0;
        double rpmCoef_ = 0.002, thrCoef_ = 0.004; // per 48 kHz sample (cmd_preview's 0.002 / 0.004 per 44.1 kHz sample)
        double rate_ = OutRate, lp_ = 0.0, lpCoef_ = 0.0, bassGain_ = 0.0;
        int gear_ = 0;
        bool started_ = false;
        double thrHistory_[6] = {}; // the last race frames' throttle (0.1 s at 60 frames a second)
        unsigned thrAt_ = 0;
        uint64_t sample_ = 0;
        struct Pop
        {
            uint64_t start = 0;
            const Loop *loop = nullptr;
            float gain = 0.0f;
        };
        std::vector<Pop> active_;
        uint64_t popsFired_ = 0, popTriggers_ = 0;
        std::vector<float> builtinPop_; // when the pack has no pops (cmd_preview's built-in pop)
        Loop builtinLoop_;
    };
}
#endif
