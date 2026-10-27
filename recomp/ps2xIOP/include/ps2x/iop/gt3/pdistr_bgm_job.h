#pragma once
#include "ps2x/iop/gt3/pdistr_mpeg_audio.h"
#include "ps2x/iop/gt3/pdistr_mpeg_source.h"
#include "ps2x/iop/gt3/pdispu2_movie_audio.h"

namespace ps2x::iop { class NativeLibraryRegistry; }
namespace ps2x::iop::gt3 {
class Pdispu2Resident;
// Native PDISTR BGM playback (worker command 1: 9d8 -> 4e8, job 400). The
// original allocates size+0x6000 bytes, runs the disc reader 2670 into the raw
// ring 2c30 (buffer+0x6000, capacity size), builds the audio objects with
// constructor 0 on state 2cf0 (secondary ring 2d08 = buffer+0x3000/0x3000,
// timing 2d90 with range [0,max), ADPCM descriptor 2dc8, PCM descriptor 2e14),
// then job 400 runs parser 100 and the PDISPU2 streamed-ADPCM job. Unlike the
// movie branch there is no (-1,-1) range: BGM plays without EE timing ranges.
// Supported subset: disc source (empty file name), single pass or looping
// (loop byte: state+c set, export43 period = samples on the timing, reader and
// parser 100 repeat until stopped), ADPCM stereo with 1024-byte interleave.
// Everything else is refused.
class PdistrBgmJob {
public:
    struct Route {
        Pdispu2Resident* sound=nullptr;
        NativeLibraryRegistry* registry=nullptr;
        uint32_t state=0,rawRing=0,secondaryRing=0,timing=0,descriptor=0,pcmDescriptor=0,volumeReference=0;
    };
    struct Snapshot {
        PdistrMpegAudio::Snapshot parser;
        bool jobStarted=false; std::optional<Pdispu2MovieAudio::Snapshot> job;
    };
    PdistrBgmJob(IopHost&,PdicdvdResident&);
    ~PdistrBgmJob();
    // bufferBytes is the request's word 3 already masked with ~0x7ff.
    bool start(const Route&,uint32_t lsn,uint32_t sectors,uint32_t bufferBytes,bool loop=false);
    // Original 49c teardown: cancel the rings, stop the sound job, join the
    // source and parser, release the timing object (export51), free storage.
    bool stop();
    bool active() const;
    std::optional<Snapshot> snapshot() const;
private:
    struct State;
    std::unique_ptr<State> state_;
};
}
