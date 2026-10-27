#pragma once
#include "ps2x/iop/gt3/pdistr_mpeg_ring.h"
#include <functional>

namespace ps2x::iop::gt3 {
struct PdistrAudioHeader {
    uint32_t format=0,rate=0,channels=0,interleave=0,bytes=0,samples=0,pitch=0;
};
// Known stereo-stream container subset. The original100 ignores magic fields;
// malformed containers and formats beyond1/16 are intentionally unsupported.
std::optional<PdistrAudioHeader> parsePdistrAudioHeader(std::span<const uint8_t>);

// Original100's header/payload routing, with an explicit sound-job
// configurator. No decoder, voice clock or sound completion is supplied here.
// Single pass by default. With Loop (state+c set, BGM only) it repeats as 100
// does at 318: after each payload it skips the rest of the file plus the next
// pass's 0x28-byte header (state+4 - bytes - 0x28, +0x28) and copies the
// payload again until cancelled; state+8 counts passes.
class PdistrMpegAudio {
public:
    enum class Result { Idle,Running,Completed,Cancelled,Unsupported,Truncated,StorageFailure };
    struct Snapshot { Result result=Result::Idle; std::optional<PdistrAudioHeader> header; uint64_t copied=0; };
    // Must create/configure the real downstream worker, or return false. Never
    // report success for an unsupported codec/job. Called outside registry locks.
    using Configure=std::function<bool(const PdistrAudioHeader&)>;
    struct Loop {
        bool enabled=false;
        uint32_t fileBytes=0;                       // state+4 (sectors << 11), 0 if unknown
        std::function<bool(uint32_t passes)> onPass; // state+8 after each payload; false stops
    };
    PdistrMpegAudio();
    ~PdistrMpegAudio();
    bool start(PdistrMpegRing& input,PdistrMpegRing& output,Configure,Loop loop={});
    bool stop();
    Snapshot snapshot() const;
private:
    struct State;
    std::unique_ptr<State> state_;
};
}
