#include "runtime/gt3_movie_guest_output.h"
#include "ps2_runtime.h"
#include "runtime/ee_scheduler.h"
#include <cstring>
#include <limits>
#include <algorithm>
#include <mutex>

namespace ps2x::video {
namespace {
struct OutputRegistry {
    std::mutex mutex;
    std::vector<std::pair<PS2Runtime*, Gt3MovieGuestOutput*>> outputs;
};
OutputRegistry& registry() { static OutputRegistry value; return value; }
bool ramRange(uint32_t address, uint64_t size) {
    return address >= 0x10000 && uint64_t(address) + size <= PS2_RAM_SIZE;
}
bool overlaps(uint32_t a, uint64_t as, uint32_t b, uint64_t bs) {
    return uint64_t(a) < uint64_t(b) + bs && uint64_t(b) < uint64_t(a) + as;
}
}
Gt3MovieGuestOutput::Gt3MovieGuestOutput(PS2Runtime& runtime) : runtime_(runtime) {
    auto& entries = registry(); std::lock_guard lock(entries.mutex);
    entries.outputs.emplace_back(&runtime, this);
}
Gt3MovieGuestOutput::~Gt3MovieGuestOutput() {
    auto& entries = registry(); std::lock_guard lock(entries.mutex);
    std::erase_if(entries.outputs, [this](const auto& item) { return item.second == this; });
}
void stopGt3MovieOutputsBeforeEeReset(PS2Runtime& runtime) {
    auto& entries = registry(); std::lock_guard lock(entries.mutex);
    for (const auto& [owner, output] : entries.outputs) if (owner == &runtime) output->stop();
}
void stopGt3MovieOutputsBeforePoolChange(PS2Runtime& runtime, uint32_t descriptor) {
    auto& entries = registry(); std::lock_guard lock(entries.mutex);
    for (const auto& [owner, output] : entries.outputs)
        if (owner == &runtime && output->descriptor_ == descriptor) output->stop();
}
bool Gt3MovieGuestOutput::readDescriptor(std::array<uint32_t, 10>& words) const {
    auto* ram = runtime_.memory().getRDRAM();
    if (!ram || !ramRange(descriptor_, 40) || descriptor_ % 4 || !ramRange(priming_, 4) || priming_ % 4)
        return false;
    std::memcpy(words.data(), ram + descriptor_, 40);
    const uint64_t pictureBytes = uint64_t(width_) * height_ * 4;
    if (!width_ || !height_ || width_ > 4096 || height_ > 4096 || width_ % 16 || height_ % 16 ||
        words[2] != pictureBytes || !words[3] || words[3] > 8 ||
        words[1] != pictureBytes * words[3] || words[0] % 16 || !ramRange(words[0], words[1]) ||
        words[4] >= words[3] || words[5] > words[3] ||
        overlaps(words[0], words[1], descriptor_, 40) || overlaps(words[0], words[1], priming_, 4) ||
        overlaps(descriptor_, 40, priming_, 4)) return false;
    return true;
}
uint64_t Gt3MovieGuestOutput::bind(uint32_t descriptor, uint32_t primingAddress, uint32_t width, uint32_t height) {
    stop(); descriptor_ = descriptor; priming_ = primingAddress; width_ = width; height_ = height;
    if (session_ == std::numeric_limits<uint64_t>::max() || !readDescriptor(bound_)) return 0;
    auto& ee = runtime_.eeScheduler();
    const auto* free = ee.semaphore(int(bound_[6])); const auto* mutex = ee.semaphore(int(bound_[7]));
    if (bound_[6] == bound_[7] || !free || !mutex || free->maxCount != int(bound_[3]) || mutex->maxCount != 1)
        return 0;
    active_ = true; return ++session_;
}
Gt3GuestPictureResult Gt3MovieGuestOutput::publish(uint64_t session, const Gt3MoviePicture& picture) {
    if (!active_ || session != session_) return Gt3GuestPictureResult::Stopped;
    std::array<uint32_t, 10> words{};
    if (!readDescriptor(words)) { stop(); return Gt3GuestPictureResult::Invalid; }
    for (size_t index : {0u, 1u, 2u, 3u, 6u, 7u})
        if (words[index] != bound_[index]) { stop(); return Gt3GuestPictureResult::Stopped; }
    if (picture.width != width_ || picture.height != height_ || picture.tiledRgba.size() != words[2])
        return Gt3GuestPictureResult::Invalid;
    auto& ee = runtime_.eeScheduler();
    const int freeId = int(words[6]), mutexId = int(words[7]);
    const auto* free = ee.semaphore(freeId); const auto* mutex = ee.semaphore(mutexId);
    if (!free || !mutex) { stop(); return Gt3GuestPictureResult::Stopped; }
    if (free->maxCount != int(words[3]) || mutex->maxCount != 1 || !free->waiters.empty()) {
        stop(); return Gt3GuestPictureResult::Invalid;
    }
    if (!mutex->count) return Gt3GuestPictureResult::WouldBlock;
    // Original 2606f0 unlocks the queue before signalling the free-slot permit.
    // It can yield in between. A shortage is transient: retry without writing.
    // Excess permits still indicate corruption or an unsupported producer.
    const int available = int(words[3] - words[5]);
    if (free->count < 0 || free->count > available) { stop(); return Gt3GuestPictureResult::Invalid; }
    if (free->count < available) return Gt3GuestPictureResult::WouldBlock;
    if (words[5] == words[3]) return Gt3GuestPictureResult::WouldBlock;
    if (ee.pollSemaphore(mutexId) != mutexId) return Gt3GuestPictureResult::WouldBlock;
    if (ee.pollSemaphore(freeId) != freeId) {
        ee.signalSemaphore(mutexId, false); return Gt3GuestPictureResult::WouldBlock;
    }
    auto* ram = runtime_.memory().getRDRAM();
    const auto destination = words[0] + words[4] * words[2];
    std::memcpy(ram + destination, picture.tiledRgba.data(), words[2]);
    const uint32_t count = words[5] + 1, next = (words[4] + 1) % words[3];
    std::memcpy(ram + descriptor_ + 0x14, &count, 4);
    std::memcpy(ram + descriptor_ + 0x10, &next, 4);
    uint32_t primed; std::memcpy(&primed, ram + priming_, 4);
    if (!primed && count == words[3]) {
        primed = 1; std::memcpy(ram + priming_, &primed, 4);
    }
    ee.signalSemaphore(mutexId, false);
    return Gt3GuestPictureResult::Published;
}
Gt3GuestDrainResult Gt3MovieGuestOutput::primeAtEnd(uint64_t session) {
    const auto state = drainState(session);
    if (state == Gt3GuestDrainResult::Pending) {
        // EE executor only; no guest dispatch or yield between validation and
        // this latch write. Queue bytes/counts/permits remain untouched.
        runtime_.memory().write32(priming_, 1);
    }
    return state;
}
Gt3GuestDrainResult Gt3MovieGuestOutput::drainState(uint64_t session) {
    if (!active_ || session != session_) return Gt3GuestDrainResult::Stopped;
    std::array<uint32_t, 10> words{};
    if (!readDescriptor(words)) { stop(); return Gt3GuestDrainResult::Invalid; }
    for (size_t index : {0u, 1u, 2u, 3u, 6u, 7u})
        if (words[index] != bound_[index]) { stop(); return Gt3GuestDrainResult::Stopped; }
    auto& ee = runtime_.eeScheduler();
    const auto* free = ee.semaphore(int(words[6]));
    const auto* mutex = ee.semaphore(int(words[7]));
    if (!free || !mutex) { stop(); return Gt3GuestDrainResult::Stopped; }
    if (free->maxCount != int(words[3]) || mutex->maxCount != 1 || !free->waiters.empty()) {
        stop(); return Gt3GuestDrainResult::Invalid;
    }
    // A consumer may yield with the mutex held or after unlocking but before
    // signalling its free slot. Neither state is safe for teardown.
    if (!mutex->count) return Gt3GuestDrainResult::Busy;
    const int available = int(words[3] - words[5]);
    if (free->count < 0 || free->count > available) { stop(); return Gt3GuestDrainResult::Invalid; }
    if (free->count < available) return Gt3GuestDrainResult::Busy;
    return words[5] ? Gt3GuestDrainResult::Pending : Gt3GuestDrainResult::Empty;
}
}
