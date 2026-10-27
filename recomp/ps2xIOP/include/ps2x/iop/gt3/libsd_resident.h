#pragma once
#include "ps2x/iop/iop_host.h"
#include "ps2x/iop/native_library.h"
#include "ps2x/iop/gt3/spu2_events.h"
#include "ps2x/iop/gt3/spu2_voices.h"
#include <functional>
#include <memory>

namespace ps2x::iop::gt3 {
// Native supplied LIBSD entry, configuration bank and first cold SdInit(0).
// Startup runs the muted voice subset; warm/repeated init and guest DMA/voice
// playback exports remain unsupported. This is not an audible sound renderer.
// Lifecycle is controlled outside registry callbacks; imports are synchronous.
class LibsdResident {
public:
    explicit LibsdResident(IopHost&);
    ~LibsdResident();
    bool start();
    bool active() const;
    void reset();
    NativeExportResult busControl(uint32_t address) const;
    bool soundInitialized() const;
    bool readSoundMemory(uint32_t address, std::span<uint8_t> destination) const;
    // Native equivalent of bounded LIBSD17 write +18 completion, owned by the
    // startup token. Copies real IOP bytes; does not register generic DMA exports.
    bool transferIopToSound(uint64_t token,unsigned channel,uint32_t source,uint32_t destination,uint32_t bytes);
    std::optional<Spu2Events::Transfer> transferState(unsigned channel) const;
    // Explicit native IRQ source; called by sound services after detecting an
    // event. This does not itself emulate audible output or a physical IRQ.
    bool signalSoundInterrupt(uint32_t coreMask);
    // Output clock entry: advance the native SPU2 voice engine by `frames`
    // 48 kHz samples (stereo empty, or 2*frames interleaved int16), then deliver
    // newly raised SPU2 IRQs through signalSoundInterrupt, outside this
    // resident's lock. Returns the raised core mask; nullopt if sound is not
    // initialized, rendering failed, or a raised IRQ could not be delivered.
    std::optional<uint32_t> renderSound(uint32_t frames,std::span<int16_t> stereo);
    std::optional<Spu2Voices::VoiceState> soundVoice(unsigned core,unsigned voice) const;
    // Direct typed entry to the LIBSD exports for native sound modules, without
    // the registry: lets a native driver program voices from its own thread or
    // from inside a sound IRQ handler without registry/resident lock inversion.
    NativeExportResult invoke(uint16_t ordinal,const std::array<uint32_t,4>& args);
    // Diagnostic only: observe every ADPCM sample the voice engine decodes.
    bool setSoundDecodeTap(Spu2Voices::DecodeTap);
    // Run `section` with sound IRQ delivery excluded (the equivalent of the
    // original drivers' CpuSuspendIntr around voice setup). `section` may use
    // invoke(); it must not enter the native registry.
    bool withSoundInterruptsSuspended(const std::function<bool()>& section);
    // One native module may own first sound startup and restore the entry state
    // if its later startup fails. Tokens never grant ownership of another init.
    // Failed restore keeps the checkpoint available for a later retry.
    uint64_t beginSoundStartup();
    bool finishSoundStartup(uint64_t token);
    bool restoreSoundStartup(uint64_t token);
    bool ownsSoundStartup(uint64_t token) const;
    // Destruction fallback if restoring the owned startup cannot commit. This
    // unloads only that token's LIBSD instance; another owner's state is retained.
    bool abandonSoundStartup(uint64_t token);
private:
    struct State;
    std::unique_ptr<State> state_;
};
}
