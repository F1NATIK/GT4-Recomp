#include "ps2x/iop/gt3/spu2_events.h"
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <utility>

namespace ps2x::iop::gt3 {
namespace {
std::atomic<uint64_t> eventIds{0x67000000};
std::atomic<uint64_t> transferTickets{1};
}
struct Spu2Events::State {
    mutable std::mutex mutex;
    mutable std::condition_variable changed;
    uint64_t generation = 0, soundCount = 0;
    bool active = false, soundBusy = false;
    unsigned transfersInFlight = 0;
    std::array<Transfer, 2> cores{};
    SoundHandler sound;
};
Spu2Events::Spu2Events(): state_(std::make_shared<State>()) {}
Spu2Events::~Spu2Events() { reset(); }
bool Spu2Events::initialize() {
    auto s = state_; std::lock_guard lock(s->mutex);
    if (s->active || s->soundBusy || s->transfersInFlight) return false;
    const auto id = eventIds.fetch_add(2);
    if (id > 0x67fffffe) return false;
    s->cores = {}; s->soundCount = 0;
    s->cores[0].eventId = uint32_t(id); s->cores[1].eventId = uint32_t(id+1);
    s->active = true; ++s->generation; return true;
}
void Spu2Events::reset() {
    auto s = state_; SoundHandler removed;
    {
        std::lock_guard lock(s->mutex);
        s->active = false; ++s->generation;
        s->cores = {}; s->soundCount = 0;
        removed = std::move(s->sound);
        // Keep soundBusy until an existing handler exits. Its strong local
        // State/handler copies remain alive; a reset never joins that caller.
    }
    s->changed.notify_all();
}
bool Spu2Events::ready() const {
    auto s = state_; std::lock_guard lock(s->mutex); return s->active;
}
std::optional<Spu2Events::Transfer> Spu2Events::transfer(unsigned core) const {
    if (core >= 2) return {};
    auto s = state_; std::lock_guard lock(s->mutex);
    if (!s->active) return {};
    return s->cores[core];
}
Spu2Events::Result Spu2Events::runTransfer(unsigned core, const Operation& operation) {
    if (core >= 2 || !operation) return {};
    auto s = state_; uint64_t generation = 0, ticket = 0;
    {
        std::lock_guard lock(s->mutex);
        if (!s->active || s->cores[core].busy) return {};
        auto& c = s->cores[core];
        ticket = transferTickets.fetch_add(1); if (!ticket) return {};
        generation = s->generation;
        ++s->transfersInFlight;
        c.ticket = ticket; c.busy = true; c.complete = false; c.eventBits &= ~1u;
    }
    bool success = false;
    try { success = operation(); } catch (...) { success = false; }
    bool published = false;
    {
        std::lock_guard lock(s->mutex);
        --s->transfersInFlight;
        if (s->active && s->generation == generation && s->cores[core].ticket == ticket) {
            auto& c = s->cores[core]; c.busy = false; c.complete = success;
            if (success) {
                // Native counterpart of the initial callback-free IRQ2250
                // path: signal that core's event bit1 after transfer complete.
                ++c.irqCount; c.eventBits |= 1; published = true;
            }
        }
    }
    s->changed.notify_all();
    return {true, published, ticket};
}
bool Spu2Events::waitTransfer(unsigned core, uint64_t ticket, std::chrono::milliseconds timeout) const {
    if (core >= 2 || !ticket || timeout < std::chrono::milliseconds::zero() || timeout > std::chrono::seconds(10)) return false;
    auto s = state_; std::unique_lock lock(s->mutex);
    if (!s->active || s->cores[core].ticket != ticket) return false;
    const auto generation = s->generation;
    if (!s->changed.wait_for(lock, timeout, [&] {
        return !s->active || s->generation != generation || s->cores[core].ticket != ticket || !s->cores[core].busy;
    })) return false;
    return s->active && s->generation == generation && s->cores[core].ticket == ticket && s->cores[core].complete;
}
bool Spu2Events::clearEvent(unsigned core, uint32_t bits) {
    if (core >= 2 || (bits & ~1u)) return false;
    auto s = state_; std::lock_guard lock(s->mutex);
    if (!s->active || s->cores[core].busy) return false;
    s->cores[core].eventBits &= ~bits; return true;
}
bool Spu2Events::setSoundHandler(SoundHandler handler) {
    auto s = state_; SoundHandler removed;
    {
        std::lock_guard lock(s->mutex);
        if (!s->active || s->soundBusy) return false;
        removed = std::move(s->sound); s->sound = std::move(handler);
    }
    return true;
}
bool Spu2Events::raiseSound(uint32_t mask) {
    if (!mask || (mask & ~3u)) return false;
    auto s = state_; SoundHandler handler; uint64_t generation = 0;
    {
        std::lock_guard lock(s->mutex);
        if (!s->active || s->soundBusy) return false;
        handler = s->sound; generation = s->generation; s->soundBusy = true;
    }
    bool success = false;
    try { success = !handler || handler(mask); } catch (...) { success = false; }
    std::lock_guard lock(s->mutex); s->soundBusy = false;
    if (!s->active || s->generation != generation || !success) return false;
    ++s->soundCount; return true; // Original11a0 returns1 with no installed callback.
}
uint64_t Spu2Events::soundIrqCount() const {
    auto s = state_; std::lock_guard lock(s->mutex); return s->soundCount;
}
}
