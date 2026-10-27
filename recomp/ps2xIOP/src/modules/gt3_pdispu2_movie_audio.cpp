#include "ps2x/iop/gt3/pdispu2_movie_audio.h"
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>

namespace ps2x::iop::gt3 {
namespace {
constexpr uint32_t CallbackTable = 0x3830, IrqCallback = 0x304, VolumeCallback = 0x424;
int32_t mulShift(int32_t a, int32_t b) { return int32_t(uint32_t(int64_t(a) * b)) >> 15; } // mult/mflo/sra 15
int32_t volumeProduct(int32_t a, int32_t b, int32_t c, int32_t d) { // original 1c30
    const int32_t v = mulShift(mulShift(mulShift(a, b), c), d);
    return v < -0x8000 ? -0x8000 : v > 0x7fff ? 0x7fff : v;
}
}

struct Pdispu2MovieAudio::State {
    Environment env;
    // Descriptor-derived configuration (fixed while running).
    uint32_t descriptor = 0, volumeRef = 0, timingAddress = 0, buffer = 0;
    uint32_t core = 0, leftBase = 0, rightBase = 0, pitch = 0;
    uint32_t irqSelector = 0, leftSelector = 0, rightSelector = 0, keyMask = 0;
    // Voice/IRQ state: read and written only with sound IRQ delivery excluded
    // (worker inside withSoundInterruptsSuspended, or the IRQ handler itself).
    int quarter = -1; uint8_t halfToggle = 0; bool rangePause = false;
    // LIBPDI56-64 counter; `mutex` is a leaf lock (never held while calling out).
    mutable std::mutex mutex; std::condition_variable changed;
    int count = 0; bool empty = true, full = false;
    std::atomic<bool> stopping{false}, running{false}, finished{false}, failed{false};
    std::stop_source cancel; std::jthread worker;
    mutable std::mutex errorMutex; std::string error;
    bool fail(const char* why) { std::lock_guard lock(errorMutex); if (error.empty()) error = why; return false; }
    std::atomic<uint64_t> groups{0}, interrupts{0}, spurious{0}, consumptions{0}, starts{0}, underruns{0}, rangePauses{0};

    explicit State(Environment e): env(std::move(e)) {}
    IopHost& host() { return *env.host; }
    LibsdResident& sound() { return *env.sound; }
    bool readWord(uint32_t address, uint32_t& value) { return host().readIopMemory(address, &value, 4); }
    bool writeBytes(uint32_t address, std::span<const uint8_t> bytes) {
        const IopMemoryWrite w{address, bytes}; return host().writeIopMemoryBatch(std::span(&w, 1));
    }
    bool writeWord(uint32_t address, uint32_t value) {
        std::array<uint8_t, 4> b{uint8_t(value), uint8_t(value >> 8), uint8_t(value >> 16), uint8_t(value >> 24)};
        return writeBytes(address, b);
    }
    bool writeByte(uint32_t address, uint8_t value) { return writeBytes(address, std::span(&value, 1)); }
    bool libsd(uint16_t ordinal, uint32_t a, uint32_t b = 0) { return sound().invoke(ordinal, {a, b, 0, 0}).handled; }
    std::optional<uint32_t> libsdValue(uint16_t ordinal, uint32_t a) {
        const auto r = sound().invoke(ordinal, {a, 0, 0, 0});
        return r.handled ? std::optional<uint32_t>(uint32_t(r.value)) : std::nullopt;
    }
    void publishVoiceState() { // descriptor +14/+15/+16, as the original stores them
        writeByte(descriptor + 0x14, uint8_t(int8_t(quarter)));
        writeByte(descriptor + 0x15, halfToggle);
        writeByte(descriptor + 0x16, rangePause ? 1 : 0);
    }
    // Original 424: VOLL(left) and VOLR(right) from the volume descriptor.
    bool applyVolume() {
        uint32_t volume = 0; std::array<int32_t, 5> w{};
        if (!readWord(volumeRef, volume) || !volume || !host().readIopMemory(volume, w.data(), sizeof(w))) return false;
        const int32_t left = volumeProduct(w[0], w[1], w[3], w[4]), right = volumeProduct(w[0], w[2], w[3], w[4]);
        return libsd(5, leftSelector, uint32_t(left >> 1) & 0x3fff) &&
               libsd(5, rightSelector | 0x100, uint32_t(right >> 1) & 0x3fff);
    }
    // Original 1e04 -> 1d50(+0x10) -> 1d10: store the level, run the reference callback.
    bool setVolumeLevel(uint16_t value) {
        uint32_t volume = 0, callback = 0, argument = 0;
        if (!readWord(volumeRef, volume) || !readWord(volumeRef + 4, callback) || !readWord(volumeRef + 8, argument)) return false;
        if (!volume) return true;
        const std::array<uint8_t, 2> b{uint8_t(value), uint8_t(value >> 8)};
        if (!writeBytes(volume + 0x10, b)) return false;
        if (!callback) return true;
        return callback == env.image + VolumeCallback && argument == descriptor && applyVolume();
    }
    // Original function 0: SSA, ADSR1=000f, ADSR2=0, pitch.
    bool setupVoice(uint32_t selector, uint32_t address, uint32_t voicePitch) {
        return libsd(9, selector | 0x2040, address) && libsd(5, selector | 0x300, 0xf) &&
               libsd(5, selector | 0x400, 0) && libsd(5, selector | 0x200, voicePitch & 0xffff);
    }
    bool acknowledgeIrq() { return libsd(11, 4 | core, 0) && libsd(11, 4 | core, 1); } // original ac4
    // Original 68 (interrupts suspended by the caller).
    bool startVoices() {
        const uint32_t half = halfToggle ? 0x400 : 0;
        if (!setupVoice(leftSelector, leftBase + half, pitch) || !setupVoice(rightSelector, rightBase + half, pitch)) return false;
        quarter = halfToggle; publishVoiceState();
        if (!setVolumeLevel(0x8000) || !libsd(9, irqSelector, leftBase + half + 0x200) || !acknowledgeIrq()) return false;
        ++starts;
        return libsd(7, 0x1500 | core, keyMask); // c34 key-on (key worker issues it)
    }
    // Original 134 (interrupts excluded by the caller).
    bool stopVoices() {
        quarter = -1; publishVoiceState();
        return libsd(9, irqSelector, leftBase + 0x800) && libsd(5, leftSelector | 0x200, 0) &&
               libsd(5, rightSelector | 0x200, 0) && setVolumeLevel(0) &&
               libsd(7, 0x1600 | core, keyMask); // ca4 key-off
    }
    // LIBPDI58 (consumer, no wait): returns nothing; wakes the producer.
    void consume() {
        { std::lock_guard lock(mutex); count = count > 0 ? count - 1 : 0; empty = count == 0; }
        changed.notify_all();
    }
    // Original 304.
    bool interrupt() {
        ++interrupts;
        if (!running || quarter < 0) return true;
        const int next = (quarter + 1) & 3;
        auto nax = libsdValue(10, leftSelector | 0x2240); // 1ee4 reads until stable; one read is stable here
        if (!nax) return false;
        const uint32_t lo = leftBase + uint32_t(next) * 0x200;
        if (*nax < lo || *nax >= lo + 0x200) { ++spurious; return true; }
        quarter = next;
        if (!libsd(9, irqSelector, leftBase + uint32_t((next + 1) & 3) * 0x200)) return false;
        bool consumeHalf = (next & 1) == 0, stop = false;
        if (!consumeHalf) { // LIBPDI64: producer must have refilled
            bool wasFull = false;
            { std::lock_guard lock(mutex); wasFull = full; full = false; }
            if (!wasFull) { consumeHalf = true; stop = true; ++underruns; }
        }
        if (consumeHalf) {
            halfToggle ^= 2; consume(); ++consumptions;
            const auto beyond = env.timing->advance(); // 2438
            if (!beyond) return false;
            if (*beyond) { rangePause = true; stop = true; ++rangePauses; }
        }
        publishVoiceState();
        return stop ? stopVoices() : true;
    }
    // Blocking read of one 2048-byte group (LIBPDI84). false: cancelled/failed;
    // `ended` set when the stream drained (EOF), possibly with a partial group.
    bool readGroup(std::array<uint8_t, 2048>& group, bool& ended) {
        size_t have = 0; ended = false;
        while (have < group.size()) {
            auto chunk = env.ring->readAvailable(uint32_t(group.size() - have), cancel.get_token());
            if (!chunk) return false;
            if (chunk->empty()) { ended = true; return true; }
            std::copy(chunk->begin(), chunk->end(), group.begin() + have); have += chunk->size();
        }
        return true;
    }
    bool pausedWait() { // original 2670: sleep while timing +34 requests a pause
        for (;;) {
            uint8_t paused = 0;
            if (!host().readIopMemory(timingAddress + 0x34, &paused, 1)) return false;
            if (!paused) return true;
            if (stopping) return false;
            std::this_thread::sleep_for(std::chrono::milliseconds(1)); // woken by export48 in the original
        }
    }
    void run() {
        unsigned half = 0; bool ok = true, drained = false;
        std::array<uint8_t, 2048> group{};
        while (ok && !stopping) {
            if (auto snap = env.ring->snapshot(); !snap || (snap->ended && !snap->filled)) { drained = true; break; } // LIBPDI92
            if (!pausedWait()) { ok = fail("timing pause wait"); break; }
            bool ended = false;
            if (!readGroup(group, ended)) { ok = fail("ring read"); break; }
            if (ended) { drained = true; break; } // partial trailing group is not played
            if (!writeBytes(buffer, group) || !env.transfer(buffer, half)) { ok = fail("group transfer (1ec)"); break; }
            ++groups;
            { // LIBPDI59 with wait: block while both halves are full
                std::unique_lock lock(mutex);
                count = count < 2 ? count + 1 : 2; full = count == 2;
                changed.wait(lock, [&] { return count < 2 || stopping.load(); });
            }
            if (stopping) break;
            bool restart = false, pause = false;
            ok = sound().withSoundInterruptsSuspended([&] { pause = rangePause; rangePause = false; publishVoiceState(); return true; });
            if (!ok) { fail("suspended section"); break; }
            if (pause) { // 2518: wait for the EE's next timing range
                if (!env.timing->waitForRange(cancel.get_token())) { ok = fail("range wait (2518)"); break; }
                restart = true;
            }
            { std::lock_guard lock(mutex); if (empty) { empty = false; restart = true; } } // LIBPDI63
            if (restart && !sound().withSoundInterruptsSuspended([&] { return startVoices(); })) { ok = fail("voice start (68)"); break; }
            half ^= 1;
        }
        if (ok && drained) { // LIBPDI62: wait until the IRQ side consumed everything
            std::unique_lock lock(mutex);
            changed.wait(lock, [&] { return count == 0 || stopping.load(); });
        }
        teardown();
        failed = !ok && !stopping; finished = true;
    }
    void teardown() { // 6b0 path: unregister 304, stop voices, clear volume callback
        running = false;
        const std::array<uint8_t, 8> none{};
        writeBytes(env.image + CallbackTable + core * 8, none);
        sound().withSoundInterruptsSuspended([&] { return stopVoices(); });
        writeWord(volumeRef + 4, 0); writeWord(volumeRef + 8, 0);
    }
};

Pdispu2MovieAudio::Pdispu2MovieAudio(Environment env): state_(std::make_shared<State>(std::move(env))) {}
Pdispu2MovieAudio::~Pdispu2MovieAudio() { stop(); }

bool Pdispu2MovieAudio::start(uint32_t descriptor) {
    auto& s = *state_;
    if (s.worker.joinable() || !s.env.host || !s.env.sound || !s.env.timing || !s.env.ring || !s.env.transfer ||
        !s.env.image || !descriptor || (descriptor & 3)) return false;
    std::array<uint32_t, 0x4c / 4> d{};
    if (!s.host().readIopMemory(descriptor, d.data(), sizeof(d))) return false;
    s.descriptor = descriptor; s.volumeRef = d[0x18 / 4]; s.timingAddress = d[0x1c / 4];
    s.core = d[0x20 / 4]; const uint32_t voice = d[0x24 / 4]; s.buffer = d[0x2c / 4];
    s.pitch = d[0x30 / 4]; s.leftBase = d[0x34 / 4]; s.rightBase = d[0x38 / 4];
    // Subset: one core, a left/right voice pair inside 24, 16-byte aligned
    // buffers with room for two 0x400-byte halves, a real volume reference.
    if (s.core > 1 || voice > 22 || !s.volumeRef || !s.timingAddress || !s.buffer || (s.buffer & 15) ||
        !s.pitch || s.pitch > 0x3fff || (s.leftBase & 15) || (s.rightBase & 15) ||
        s.leftBase > 0x200000 - 0x800 || s.rightBase > 0x200000 - 0x800) return false;
    std::array<uint32_t, 2> table{};
    if (!s.host().readIopMemory(s.env.image + CallbackTable + s.core * 8, table.data(), sizeof(table)) || table[0] || table[1])
        return false; // Another job owns this core's IRQ callback.
    s.irqSelector = s.core | 0x1f00; s.leftSelector = s.core | (voice << 1); s.rightSelector = s.core | ((voice + 1) << 1);
    s.keyMask = (1u << voice) | (1u << (voice + 1));
    // 498 prologue: descriptor fields, counter init (56), volume callback, stop.
    // 498 writes +16=1: the first loop iteration waits for a timing range
    // (2518). PDISTR's d0c publishes range (-1,-1) after its constructor's
    // [0, max), so the job waits for the EE's first RPC3 range; a caller that
    // leaves [0, max) in place (the probes) starts at once.
    s.quarter = -1; s.halfToggle = 0; s.rangePause = true;
    s.count = 0; s.empty = true; s.full = false;
    if (!s.writeWord(descriptor + 0x3c, s.irqSelector) || !s.writeWord(descriptor + 0x40, s.leftSelector) ||
        !s.writeWord(descriptor + 0x44, s.rightSelector) || !s.writeWord(descriptor + 0x48, s.keyMask)) return false;
    s.publishVoiceState();
    if (!s.writeWord(s.volumeRef + 4, s.env.image + VolumeCallback) || !s.writeWord(s.volumeRef + 8, descriptor)) return false;
    s.running = true;
    const std::array<uint32_t, 2> entry{s.env.image + IrqCallback, descriptor};
    if (!s.sound().withSoundInterruptsSuspended([&] { return s.stopVoices(); }) ||
        !s.writeBytes(s.env.image + CallbackTable + s.core * 8,
                      std::span(reinterpret_cast<const uint8_t*>(entry.data()), sizeof(entry)))) {
        s.running = false; s.writeWord(s.volumeRef + 4, 0); s.writeWord(s.volumeRef + 8, 0); return false;
    }
    // 239c: rate = pitch*48000/4096, 1792 samples per tick (one half).
    const uint32_t rate = uint32_t((uint64_t(s.pitch) * 48000) >> 12);
    if (!s.env.timing->configure(0x700, rate)) { s.teardown(); return false; }
    s.stopping = false; s.finished = false; s.failed = false;
    s.worker = std::jthread([state = state_] { state->run(); });
    return true;
}
void Pdispu2MovieAudio::stop() {
    auto& s = *state_;
    if (!s.worker.joinable()) return;
    s.stopping = true; s.cancel.request_stop();
    s.changed.notify_all();
    s.worker.join();
}
bool Pdispu2MovieAudio::interrupt() { return state_->interrupt(); }
bool Pdispu2MovieAudio::applyVolume() { return state_->running && state_->applyVolume(); }
uint32_t Pdispu2MovieAudio::descriptor() const { return state_->descriptor; }
Pdispu2MovieAudio::Snapshot Pdispu2MovieAudio::snapshot() const {
    auto& s = *state_; Snapshot r;
    r.running = s.running; r.finished = s.finished; r.failed = s.failed; r.quarter = s.quarter;
    { std::lock_guard lock(s.mutex); r.count = unsigned(s.count); }
    r.groups = s.groups; r.interrupts = s.interrupts; r.spurious = s.spurious; r.consumptions = s.consumptions;
    r.starts = s.starts; r.underruns = s.underruns; r.rangePauses = s.rangePauses;
    { std::lock_guard lock(s.errorMutex); r.error = s.error; }
    return r;
}
}
