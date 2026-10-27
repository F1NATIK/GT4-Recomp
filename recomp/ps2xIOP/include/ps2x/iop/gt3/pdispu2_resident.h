#pragma once
#include "ps2x/iop/gt3/libsd_resident.h"
#include "ps2x/iop/gt3/pdispu2_movie_audio.h"
#include "ps2x/iop/gt3/pdistr_mpeg_ring.h"
#include <stop_token>

namespace ps2x::iop::gt3 {
// Native startup, initial sound callback, volume4/10, stream descriptors52/58,
// version RPC0/configuration5
// and owned synchronous IOP-to-sound transfers. No audible playback, secondary
// callback, generic transfer export or command RPC4 support.
// Keep the LIBSD owner alive longer than this resident. Lifecycle calls are made
// outside native registry callbacks; exports reject reentrant lifecycle changes.
class Pdispu2Resident {
public:
    Pdispu2Resident(IopHost&,LibsdResident&);
    ~Pdispu2Resident();
    bool start();
    bool reset();
    bool active() const;
    RpcResult handleRpc(const RpcRequest&);
    // Serialized native stream write and actual completion; source is IOP RAM,
    // destination is sound RAM. Generic export35/reverse transfers stay absent.
    bool transferIopData(uint32_t source,uint32_t soundDestination,uint32_t bytes);
    // Original1ec/1b0 subset: patch loop flags in one2048-byte stereo block,
    // then complete two1024-byte transfers to default1d8000/1f8000 buffers.
    // half0/1 only. Does not configure voices, signal playback or accept RPC1.
    // Failed transfers can leave the left channel committed; caller must stop.
    bool transferAdpcmChunk(uint32_t source,unsigned half);
    // Export10: write low16 bits through a volume reference with no callback.
    // Callback-bearing references remain unsupported, with no partial write.
    // Direct entry avoids taking the native registry on a dependent worker.
    bool setVolume(uint32_t reference,uint32_t value);
    // Original export44 carries two words on the IOP stack, beyond the generic
    // four-register native import ABI. Use this typed entry for owned controls.
    bool updateStreamRange(uint32_t descriptor,const std::array<uint32_t,4>& words);
    // Typed blocking46 entry, outside registry dispatch. Reset cancels the
    // wait; another thread can still supply44 ranges while it is blocked.
    bool waitForStreamRange(uint32_t descriptor,std::stop_token);
    // Typed export50/51 (original 26e0): resume, reset range, drop the timing
    // control. Refused while an unfinished movie-audio job uses it.
    bool releaseStreamControl(uint32_t descriptor);
    // Typed export55 (original 71c -> worker 498): start the native streamed
    // ADPCM movie-audio job on an export52 descriptor. The descriptor's +1c must
    // be a timing descriptor owned here (export41). `ring` is the descriptor's
    // secondary ring; its owner keeps it alive until stopMovieAudio returns.
    // One job at a time. Its IRQ callback 304 and volume callback 424 are served
    // natively through the original callback table and volume reference.
    bool startMovieAudio(uint32_t descriptor,PdistrMpegRing& ring);
    bool stopMovieAudio();
    std::optional<Pdispu2MovieAudio::Snapshot> movieAudio() const;
private:
    struct State;
    std::shared_ptr<State> state_;
};
}
