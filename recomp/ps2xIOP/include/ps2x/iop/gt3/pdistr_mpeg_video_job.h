#pragma once
#include "ps2x/iop/gt3/pdistr_mpeg_audio.h"
#include "ps2x/iop/gt3/pdistr_mpeg_source.h"
#include "ps2x/iop/gt3/pdispu2_movie_audio.h"

namespace ps2x::iop { class NativeLibraryRegistry; }
namespace ps2x::iop::gt3 {
class Pdispu2Resident;
// Owns a disc/demux/EE-read MPEG pipeline and, for audio-enabled requests
// (flags0/2), the original PDISTR audio branch: raw audio ring 3350 -> parser
// 100 -> secondary ring 3208 -> PDISPU2 export55 movie-audio job, with the
// timing object 3290 (export41/44) that RPC3 ranges drive. The host, disc and
// sound residents must outlive this object. Lifecycle calls run outside the
// native-library registry and host/worker callbacks; the owner joins callers
// before destroying this API.
class PdistrMpegVideoJob {
public:
    // Original PDISTR-image addresses of the audio objects (see
    // gt3-analysis/iop/SPU2-voices-native.md section 8). All must stay
    // allocated until stop returns.
    struct AudioRoute {
        Pdispu2Resident* sound=nullptr;
        NativeLibraryRegistry* registry=nullptr;
        uint32_t rawRing=0,secondaryRing=0,timing=0,descriptor=0,pcmDescriptor=0,volumeReference=0;
    };
    struct AudioSnapshot {
        PdistrMpegAudio::Snapshot parser;
        bool jobStarted=false; std::optional<Pdispu2MovieAudio::Snapshot> job;
    };
    PdistrMpegVideoJob(IopHost&,PdicdvdResident&);
    ~PdistrMpegVideoJob();
    // Flags1(video only), 3(video only/repeat), and with an audio route also
    // 0(audio) and 2(audio/repeat). Returns fresh token, or0.
    // Optional embedded descriptors are borrowed from the resident. Both must
    // remain allocated until stop returns; only the ring buffers are then owned.
    // A failed start may have initialized them; the resident restores its model
    // snapshot after the failed attempt has joined all unpublished workers.
    uint64_t start(uint32_t flags,uint32_t lsn,uint32_t sectors,
                   uint32_t sourceDescriptor=0,uint32_t videoDescriptor=0,
                   const AudioRoute* audio=nullptr);
    // One reader at a time, waits without retaining the lifecycle lock.
    PdistrMpegRing::Transfer read(uint64_t token,uint32_t destination,uint32_t bytes);
    // Original RPC3 -> 119c: while an audio movie is active, export44 on the
    // timing object with the EE's {start lo, start hi, end lo, end hi} (us).
    // False when no audio movie with this token is active.
    bool updateAudioRange(uint64_t token,const std::array<uint32_t,4>& range);
    std::optional<AudioSnapshot> audio() const;
    // Stops the movie-audio job, wakes/cancels a pending read, joins producers
    // and reader, releases the timing object, then frees IOP storage. All old
    // tokens stay invalid even if guest addresses are reused.
    bool stop();
private:
    struct State;
    std::unique_ptr<State> state_;
};
}
