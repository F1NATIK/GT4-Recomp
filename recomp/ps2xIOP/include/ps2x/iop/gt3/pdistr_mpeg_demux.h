#pragma once
#include "ps2x/iop/gt3/pdistr_mpeg_ring.h"
#include <memory>

namespace ps2x::iop::gt3 {
// Native parser worker. Caller owns initialized source/video/audio
// rings until stop returns and all external producers/consumers have joined.
// Disc ownership, sound decoding and runtime RPC1 remain separate.
class PdistrMpegDemux {
public:
    enum class Result { Idle, Running, Completed, Cancelled, Unsupported, Truncated, StorageFailure };
    struct Snapshot { Result result=Result::Idle; uint64_t consumed=0,packets=0,videoRecords=0,audioBytes=0,passes=0; };
    PdistrMpegDemux();
    ~PdistrMpegDemux();
    // Nonzero repeatSectors skips each extent tail after its end record, then
    // parses the next pass without setting EOF. Source must supply repeat data.
    bool start(PdistrMpegRing& source,PdistrMpegRing& video,PdistrMpegRing* audio,uint32_t repeatSectors=0);
    // Cancellation owns the pipeline's three rings, wakes blocked endpoints,
    // and joins its parser thread. No guest job is marked complete on failure.
    bool stop();
    Snapshot snapshot() const;
private:
    struct State;
    std::unique_ptr<State> state_;
};
}
