#pragma once
#include "ps2x/iop/gt3/libsd_resident.h"
#include "ps2x/iop/gt3/pdispu2_stream_control.h"
#include "ps2x/iop/gt3/pdistr_mpeg_ring.h"
#include <functional>
#include <memory>
#include <string>

namespace ps2x::iop::gt3 {
// Native PDISPU2 streamed-ADPCM movie audio job: original export55 -> 71c ->
// worker 498, with SPU2 IRQ callback 304, volume callback 424, voice start 68
// and stop 134, and the LIBPDI56-64 two-slot fill counter. Voices are driven
// through LIBSD's typed entry into the native Spu2Voices engine, so the IRQ
// cadence comes from real voice progress (LibsdResident::renderSound).
// See gt3-analysis/iop/SPU2-voices-native.md for the original protocol.
//
// Descriptor (original export52 layout, 0x4c bytes, IOP memory):
//   +14 quarter  +15 restart half (0/2)  +16 range-pause flag
//   +18 volume reference {+0 volume descriptor, +4 callback, +8 argument}
//   +1c timing descriptor (export41)  +20 core  +24 left voice (right = +1)
//   +28 secondary ring  +2c 2048-byte group buffer  +30 pitch
//   +34/+38 left/right sound-RAM base  +3c IRQA selector  +40/+44 voice
//   selectors  +48 key mask   (the job writes +14..+16 and +3c..+48)
//
// Not modelled: the core-claim object at PDISPU2 3840 (bac/bd8; one job at a
// time is enforced instead), the ring's longjmp recovery (ring cancellation is
// the native equivalent), and the separate key worker thread (KOFF/KON are
// issued directly, KOFF of pending keys first, as that worker does).
class Pdispu2MovieAudio {
public:
    struct Environment {
        IopHost* host=nullptr;
        LibsdResident* sound=nullptr;
        std::shared_ptr<Pdispu2StreamControl> timing; // descriptor +1c
        PdistrMpegRing* ring=nullptr;                 // descriptor +28, consumer side
        uint32_t image=0;                             // PDISPU2 model image
        // Original 1ec: patch loop flags of the 2048-byte group at `source` and
        // transfer both channels to base + half*0x400. Must not block on IRQs.
        std::function<bool(uint32_t source,unsigned half)> transfer;
    };
    struct Snapshot {
        bool running=false,finished=false,failed=false;
        int quarter=-1; unsigned count=0;
        uint64_t groups=0,interrupts=0,spurious=0,consumptions=0,starts=0,underruns=0,rangePauses=0;
        std::string error; // first failure reason, empty when none
    };
    explicit Pdispu2MovieAudio(Environment);
    ~Pdispu2MovieAudio();
    Pdispu2MovieAudio(const Pdispu2MovieAudio&)=delete;
    Pdispu2MovieAudio& operator=(const Pdispu2MovieAudio&)=delete;
    bool start(uint32_t descriptor);
    // Cancels the ring and every wait, joins the worker, restores the callback
    // table/volume reference and stops the voices. Idempotent.
    void stop();
    // Original 304, called by the PDISPU2 IRQ dispatcher (9d8) with sound IRQ
    // delivery in progress. The dispatcher acknowledges with ac4 afterwards.
    bool interrupt();
    // Original 424 via the volume reference (export10 write path).
    bool applyVolume();
    uint32_t descriptor() const;
    Snapshot snapshot() const;
private:
    struct State;
    std::shared_ptr<State> state_;
};
}
