#pragma once
#include "runtime/gt3_movie_queue.h"
#include <array>
class PS2Runtime;
namespace ps2x::video {
void stopGt3MovieOutputsBeforeEeReset(PS2Runtime& runtime);
// Called by verified original queue reset/cancel/destruction paths before any
// guest mutation or yielding syscall. Matching runtime and descriptor only.
void stopGt3MovieOutputsBeforePoolChange(PS2Runtime& runtime, uint32_t descriptor);
enum class Gt3GuestPictureResult { Published, WouldBlock, Stopped, Invalid };
enum class Gt3GuestDrainResult { Empty, Pending, Busy, Stopped, Invalid };
// EE-executor-only producer for an already allocated original 2602e8 queue.
// Sole producer: the original IPU producer must not run alongside this adapter.
// EE reset and verified original pool lifecycle paths invalidate registered
// outputs automatically. Other owners must stop before destroying their pool.
// The hash-gated Gt3MovieWorker is the native producer in the game's decoder worker.
class Gt3MovieGuestOutput {
public:
    explicit Gt3MovieGuestOutput(PS2Runtime& runtime);
    ~Gt3MovieGuestOutput();
    Gt3MovieGuestOutput(const Gt3MovieGuestOutput&) = delete;
    Gt3MovieGuestOutput& operator=(const Gt3MovieGuestOutput&) = delete;
    // Returns a fresh nonzero session token, or zero. Failed bind stops any old
    // session. primingAddress is the verified game's external queue-ready word.
    uint64_t bind(uint32_t descriptor, uint32_t primingAddress, uint32_t width, uint32_t height);
    Gt3GuestPictureResult publish(uint64_t session, const Gt3MoviePicture& picture);
    // Checks original queue ownership and permit balance before reporting that
    // all published pictures were released. Does not clear buffers or permits.
    Gt3GuestDrainResult drainState(uint64_t session);
    // At end of input a short stream may never fill the initial queue. Expose
    // its remaining pictures only after validating the same lifetime/permits.
    Gt3GuestDrainResult primeAtEnd(uint64_t session);
    void stop() { active_ = false; }
    bool isActive(uint64_t session) const { return active_ && session == session_; }
private:
    friend void stopGt3MovieOutputsBeforePoolChange(PS2Runtime&, uint32_t);
    bool readDescriptor(std::array<uint32_t, 10>& words) const;
    PS2Runtime& runtime_;
    uint32_t descriptor_ = 0, priming_ = 0, width_ = 0, height_ = 0;
    std::array<uint32_t, 10> bound_{};
    uint64_t session_ = 0;
    bool active_ = false;
};
}
