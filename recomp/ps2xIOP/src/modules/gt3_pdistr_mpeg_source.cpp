#include "ps2x/iop/gt3/pdistr_mpeg_source.h"
#include <algorithm>
#include <mutex>
#include <thread>

namespace ps2x::iop::gt3 {
namespace { thread_local const void* currentSource=nullptr; }
struct PdistrMpegSource::State {
    PdicdvdResident& disc;
    PdistrMpegRing* ring=nullptr;
    mutable std::mutex mutex;
    std::recursive_mutex lifecycle;
    std::jthread worker;
    Snapshot status;
    explicit State(PdicdvdResident& d):disc(d) {}
    void finish(Result result) { std::lock_guard lock(mutex); status.result=result; }
    void cancel() noexcept {
        if (ring) { try { ring->cancel(); } catch (...) { ring->stop(); } }
    }
    Result endpointResult(std::stop_token stop) const {
        if (stop.stop_requested()) return Result::Cancelled;
        const auto state=ring->snapshot();
        return state && state->ended?Result::ConsumerEnded:Result::StorageFailure;
    }
    void run(std::stop_token stop,uint32_t first,uint32_t count,bool loop) {
        currentSource=this;
        struct Clear { ~Clear() { currentSource=nullptr; } } clear;
        Result result=Result::StorageFailure;
        try {
            uint32_t lsn=first,left=count;
            for (;;) {
                if (stop.stop_requested()) { result=Result::Cancelled; break; }
                if (!left) {
                    if (count) { std::lock_guard lock(mutex); ++status.passes; }
                    // Original57c disables repeat for an empty extent.
                    if (!loop || !count) {
                        result=ring->finish()?Result::Completed:Result::StorageFailure; break;
                    }
                    lsn=first; left=count;
                }
                const auto space=ring->waitWritable(2048,stop);
                if (!space) { result=endpointResult(stop); break; }
                // The native backend accepts at most64 sectors. Splitting an
                // original larger read preserves the ordered source bytes.
                const auto take=std::min({left,*space/2048,64u});
                std::optional<std::vector<uint8_t>> data;
                try { data=disc.readStreamSectors(lsn,take); }
                catch (...) { result=Result::DiscFailure; break; }
                if (stop.stop_requested()) { result=Result::Cancelled; break; }
                if (!data || data->size()!=size_t(take)*2048) { result=Result::DiscFailure; break; }
                if (!ring->produce(*data,take*2048,stop)) { result=endpointResult(stop); break; }
                lsn+=take; left-=take;
                { std::lock_guard lock(mutex); status.sectors+=take; }
            }
        } catch (...) { result=Result::StorageFailure; }
        if (result!=Result::Completed && result!=Result::ConsumerEnded) cancel();
        finish(result);
    }
};
PdistrMpegSource::PdistrMpegSource(PdicdvdResident& disc):state_(std::make_unique<State>(disc)) {}
PdistrMpegSource::~PdistrMpegSource() { stop(); }
bool PdistrMpegSource::start(PdistrMpegRing& ring,uint32_t lsn,uint32_t sectors,bool loop) {
    auto& s=*state_; if (currentSource==&s) return false;
    std::lock_guard lifecycle(s.lifecycle);
    if (s.worker.joinable() || sectors>INT32_MAX || uint64_t(lsn)+sectors>UINT32_MAX || !s.disc.active()) return false;
    const auto state=ring.snapshot();
    if (!state || state->ended || state->filled || state->capacity<2048 ||
        state->capacity%2048 || state->producer%2048 || state->consumer!=state->producer) return false;
    s.ring=&ring;
    { std::lock_guard lock(s.mutex); s.status={Result::Running}; }
    try { s.worker=std::jthread([&s,lsn,sectors,loop](std::stop_token stop) { s.run(stop,lsn,sectors,loop); }); }
    catch (...) { s.ring=nullptr; s.finish(Result::StorageFailure); throw; }
    return true;
}
bool PdistrMpegSource::stop() {
    auto& s=*state_; if (currentSource==&s) return false;
    std::lock_guard lifecycle(s.lifecycle);
    s.worker.request_stop(); if (s.worker.joinable()) s.worker.join();
    s.cancel(); s.ring=nullptr; return true;
}
PdistrMpegSource::Snapshot PdistrMpegSource::snapshot() const {
    auto& s=*state_; std::lock_guard lock(s.mutex); return s.status;
}
}
