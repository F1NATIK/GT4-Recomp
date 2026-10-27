#pragma once
#include "runtime/gt3_movie_guest_output.h"
#include <span>
namespace ps2x::video {
enum class Gt3MovieStep { Ready, Accepted, NeedOutput, InputDrained, Complete, Boundary, Stopped, Failed };
// EE-thread component connecting exact video PES -> native decoder -> original
// guest picture queue. Gt3MovieWorker joins the verified CORE entry points.
// Verified original pool lifecycle paths and scheduler reset stop output
// automatically; other owners must stop before destroying guest storage.
class Gt3MovieSession {
public:
    explicit Gt3MovieSession(PS2Runtime& runtime) : output_(runtime) {}
    uint64_t start(uint32_t descriptor, uint32_t priming, uint32_t width, uint32_t height);
    // Accepted consumes this packet exactly once, even if output subsequently
    // fills. NeedOutput consumes none: drain guest output, then retry the packet.
    Gt3MovieStep feedPes(uint64_t session, std::span<const uint8_t> packet);
    // Original 4096-byte input framing. Boundary consumes no codec input and
    // does not flush/reset it; the stream owner chooses finish/repeat policy.
    // Video records preserve feedPes's Accepted/NeedOutput retry contract.
    Gt3MovieStep feedRecord(uint64_t session, std::span<const uint8_t> record);
    Gt3MovieStep pump(uint64_t session);
    // Repeat after NeedOutput; flushes the decoder once. InputDrained means all
    // decoded pictures were published, NOT that guest consumers released them.
    Gt3MovieStep finish(uint64_t session);
    // Flushes and waits for original consumers to release every guest slot.
    // Complete permits owner teardown; NeedOutput requires consumer progress.
    Gt3MovieStep drain(uint64_t session);
    // After finish reports InputDrained, reset only the codec for a new pass.
    // Already published guest pictures remain queued and ordered before it.
    Gt3MovieStep repeat(uint64_t session);
    void stop();
    const std::string& error() const { return error_; }
    uint64_t picturesPublished() const { return pictures_; }
private:
    Gt3MovieStep fail(const std::string& message);
    Gt3MovieGuestOutput output_;
    MpegVideoDecoder decoder_;
    Gt3MovieQueue packed_;
    std::deque<MpegDecodedFrame> pending_;
    uint64_t session_ = 0, pictures_ = 0;
    bool active_ = false, ending_ = false;
    std::string error_;
};
}
