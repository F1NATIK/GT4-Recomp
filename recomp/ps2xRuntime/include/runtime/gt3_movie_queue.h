#pragma once
#include "runtime/mpeg_video_decoder.h"
#include <memory>
#include <vector>

namespace ps2x::video {
struct Gt3MoviePicture {
    uint32_t width = 0, height = 0;
    int64_t pts90k = -1;
    int repeatPict = 0;
    std::vector<uint8_t> tiledRgba;
};
using Gt3MoviePictureLease = std::shared_ptr<const Gt3MoviePicture>;
enum class Gt3MoviePublish { Published, Full, Cancelled, Invalid };

// Single-owner-thread component; guest scheduling/waiting belongs to the adapter.
// Leases keep immutable pixels alive across release, cancellation and reconfigure.
// The queue owns at most capacity pictures; consumers must retire old leases.
class Gt3MovieQueue {
public:
    Gt3MovieQueue() = default;
    Gt3MovieQueue(const Gt3MovieQueue&) = delete;
    Gt3MovieQueue& operator=(const Gt3MovieQueue&) = delete;
    // Invalid configuration preserves the existing session. Valid configuration
    // starts a fresh session (including the external original priming latch).
    bool configure(uint32_t width, uint32_t height, uint32_t capacity = 3);
    Gt3MoviePublish publish(const MpegDecodedFrame& frame);
    Gt3MoviePictureLease front() const;
    bool release(const Gt3MoviePictureLease& expected);
    void cancel();
    uint32_t count() const { return count_; }
    uint32_t writeIndex() const { return write_; }
    bool primed() const { return primed_; }
private:
    uint32_t width_ = 0, height_ = 0, write_ = 0, count_ = 0;
    bool cancelled_ = false, primed_ = false;
    std::vector<Gt3MoviePictureLease> slots_;
};
}
