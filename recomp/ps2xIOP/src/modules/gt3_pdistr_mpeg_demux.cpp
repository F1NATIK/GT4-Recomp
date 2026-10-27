#include "ps2x/iop/gt3/pdistr_mpeg_demux.h"
#include "ps2x/iop/gt3/pdistr_mpeg_packet.h"
#include <algorithm>
#include <mutex>
#include <thread>

namespace ps2x::iop::gt3 {
namespace { thread_local const void* currentDemux=nullptr; }
struct PdistrMpegDemux::State {
    mutable std::mutex mutex;
    std::recursive_mutex lifecycle;
    std::jthread worker;
    PdistrMpegRing* source=nullptr;
    PdistrMpegRing* video=nullptr;
    PdistrMpegRing* audio=nullptr;
    Snapshot status;
    void finish(Result result) {
        std::lock_guard lock(mutex); status.result=result;
    }
    void cancelRings() noexcept {
        for (auto* ring:{source,video,audio}) if (ring) {
            try { ring->cancel(); } catch (...) { ring->stop(); }
        }
    }
    void run(std::stop_token stop,uint32_t repeatSectors) {
        currentDemux=this;
        struct Clear { ~Clear() { currentDemux=nullptr; } } clear;
        Result result=Result::StorageFailure;
        try {
            std::vector<uint8_t> pending;
            const uint64_t extent=uint64_t(repeatSectors)*2048;
            uint64_t passBytes=0,skip=0;
            bool ended=false;
            while (!stop.stop_requested() && !ended) {
                const auto chunk=source->readAvailable(4096,stop);
                if (!chunk) { result=stop.stop_requested()?Result::Cancelled:Result::StorageFailure; break; }
                if (chunk->empty()) { result=Result::Truncated; break; }
                pending.insert(pending.end(),chunk->begin(),chunk->end());
                size_t used=0;
                while (used<pending.size() && !stop.stop_requested()) {
                    if (skip) {
                        const auto take=std::min<uint64_t>(skip,pending.size()-used);
                        used+=size_t(take); skip-=take;
                        { std::lock_guard lock(mutex); status.consumed+=take; }
                        if (used==pending.size()) break;
                    }
                    const auto packet=parsePdistrMpegPacket(std::span(pending).subspan(used),audio!=nullptr);
                    if (packet.status==MpegPacketStatus::NeedInput) {
                        if (extent && pending.size()-used>=extent-passBytes) { result=Result::Truncated; ended=true; }
                        break;
                    }
                    if (packet.status==MpegPacketStatus::Unsupported) { result=Result::Unsupported; ended=true; break; }
                    if (extent && packet.consumed>extent-passBytes) { result=Result::Truncated; ended=true; break; }
                    bool written=true;
                    if (packet.kind==MpegPacketKind::Video || packet.kind==MpegPacketKind::End)
                        written=video->produce(packet.output,packet.advance,stop);
                    else if (packet.kind==MpegPacketKind::Audio)
                        written=audio->produce(packet.output,packet.advance,stop);
                    if (!written) { result=stop.stop_requested()?Result::Cancelled:Result::StorageFailure; ended=true; break; }
                    used+=packet.consumed;
                    passBytes+=packet.consumed;
                    {
                        std::lock_guard lock(mutex);
                        status.consumed+=packet.consumed; ++status.packets;
                        if (packet.kind==MpegPacketKind::Video || packet.kind==MpegPacketKind::End) ++status.videoRecords;
                        else if (packet.kind==MpegPacketKind::Audio) status.audioBytes+=packet.output.size();
                        if (packet.kind==MpegPacketKind::End) ++status.passes;
                    }
                    if (packet.kind==MpegPacketKind::End) {
                        if (extent) { skip=extent-passBytes; passBytes=0; continue; }
                        // Original non-loop1750 marks input and outputs ended.
                        // The coordinator still owns source-reader cancellation/join.
                        const bool complete=source->finish() && (!audio || audio->finish()) && video->finish();
                        result=complete?Result::Completed:Result::StorageFailure; ended=true; break;
                    }
                }
                pending.erase(pending.begin(),pending.begin()+used);
                if (pending.size()>65541) { result=Result::Unsupported; break; }
            }
            if (stop.stop_requested() && result!=Result::Completed) result=Result::Cancelled;
        } catch (...) { result=Result::StorageFailure; }
        if (result!=Result::Completed) cancelRings();
        finish(result);
    }
};
PdistrMpegDemux::PdistrMpegDemux():state_(std::make_unique<State>()) {}
PdistrMpegDemux::~PdistrMpegDemux() { stop(); }
bool PdistrMpegDemux::start(PdistrMpegRing& source,PdistrMpegRing& video,PdistrMpegRing* audio,uint32_t repeatSectors) {
    auto& s=*state_; if (currentDemux==&s) return false;
    std::lock_guard lifecycle(s.lifecycle);
    if (s.worker.joinable() || &source==&video || audio==&source || audio==&video || repeatSectors>0x1fffff) return false;
    const auto input=source.snapshot(),output=video.snapshot(),sound=audio?audio->snapshot():std::optional<PdistrMpegRing::Snapshot>{};
    if (!input || !output || input->ended || output->ended || output->filled || output->capacity<4096 ||
        output->capacity%4096 || (audio && (!sound || sound->ended || sound->filled))) return false;
    s.source=&source; s.video=&video; s.audio=audio;
    { std::lock_guard lock(s.mutex); s.status={Result::Running}; }
    try { s.worker=std::jthread([&s,repeatSectors](std::stop_token stop) { s.run(stop,repeatSectors); }); }
    catch (...) { s.source=s.video=s.audio=nullptr; s.finish(Result::StorageFailure); throw; }
    return true;
}
bool PdistrMpegDemux::stop() {
    auto& s=*state_; if (currentDemux==&s) return false;
    std::lock_guard lifecycle(s.lifecycle);
    s.worker.request_stop();
    if (s.worker.joinable()) s.worker.join();
    // Also release any external endpoint blocked after a normal program end.
    // The caller joins those endpoints before freeing their ring storage.
    s.cancelRings(); s.source=s.video=s.audio=nullptr; return true;
}
PdistrMpegDemux::Snapshot PdistrMpegDemux::snapshot() const {
    auto& s=*state_; std::lock_guard lock(s.mutex); return s.status;
}
}
