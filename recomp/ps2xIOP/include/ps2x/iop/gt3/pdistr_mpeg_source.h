#pragma once
#include "ps2x/iop/gt3/pdicdvd_resident.h"
#include "ps2x/iop/gt3/pdistr_mpeg_ring.h"

namespace ps2x::iop::gt3 {
// Sole producer for a borrowed embedded source ring. Uses PDICDVD's owned
// ISO worker; retains dependencies and storage until stop and endpoint joins.
// Lifecycle calls must run outside NativeLibraryRegistry dispatch.
class PdistrMpegSource {
public:
    enum class Result { Idle,Running,Completed,ConsumerEnded,Cancelled,DiscFailure,StorageFailure };
    struct Snapshot { Result result=Result::Idle; uint64_t sectors=0,passes=0; };
    explicit PdistrMpegSource(PdicdvdResident&);
    ~PdistrMpegSource();
    bool start(PdistrMpegRing&,uint32_t lsn,uint32_t sectors,bool loop);
    // A pending bounded host-file read is joined; it is not forcibly interrupted.
    bool stop();
    Snapshot snapshot() const;
private:
    struct State;
    std::unique_ptr<State> state_;
};
}
