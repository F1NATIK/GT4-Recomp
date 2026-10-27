#include "ps2x/iop/gt3/pdistr_mpeg_audio.h"
#include <algorithm>
#include <mutex>
#include <thread>

namespace ps2x::iop::gt3 {
namespace {
thread_local const void* currentAudio=nullptr;
uint32_t word(std::span<const uint8_t> bytes,size_t at) {
    uint32_t value=0; for (unsigned i=0;i<4;++i) value|=uint32_t(bytes[at+i])<<(i*8); return value;
}
}
std::optional<PdistrAudioHeader> parsePdistrAudioHeader(std::span<const uint8_t> bytes) {
    if (bytes.size()<40 || word(bytes,0)!=0x64685353 || word(bytes,4)!=24 || word(bytes,32)!=0x64625353) return {};
    PdistrAudioHeader h{word(bytes,8),word(bytes,12),word(bytes,16),word(bytes,20),word(bytes,36)};
    if (h.format!=1 && h.format!=16) return {};
    if (h.format==16) {
        h.samples=(h.bytes>>5)*28;
        // Preserve original32-bit SLL wrap and unsigned MULTU/MFHI/SRL.
        h.pitch=uint32_t((uint64_t(uint32_t(h.rate<<12))*0x57619f1)>>42);
    } else { h.samples=h.bytes>>2; h.pitch=4096; }
    return h;
}
struct PdistrMpegAudio::State {
    mutable std::mutex mutex;
    std::recursive_mutex lifecycle;
    std::jthread worker;
    PdistrMpegRing* input=nullptr;
    PdistrMpegRing* output=nullptr;
    Configure configure;
    Loop loop;
    Snapshot status;
    void finish(Result result) { std::lock_guard lock(mutex); status.result=result; }
    void cancel() noexcept {
        for (auto* ring:{input,output}) if (ring) {
            try { ring->cancel(); } catch (...) { ring->stop(); }
        }
    }
    bool readExact(uint32_t count,std::vector<uint8_t>& bytes,std::stop_token stop,Result& failure) {
        bytes.clear(); bytes.reserve(count);
        while (bytes.size()<count) {
            const auto chunk=input->readAvailable(count-uint32_t(bytes.size()),stop);
            if (!chunk) { failure=stop.stop_requested()?Result::Cancelled:Result::StorageFailure; return false; }
            if (chunk->empty()) { failure=Result::Truncated; return false; }
            bytes.insert(bytes.end(),chunk->begin(),chunk->end());
        }
        return true;
    }
    void run(std::stop_token stop) {
        currentAudio=this;
        struct Clear { ~Clear() { currentAudio=nullptr; } } clear;
        Result result=Result::StorageFailure;
        try {
            std::vector<uint8_t> bytes;
            if (readExact(40,bytes,stop,result)) {
                const auto header=parsePdistrAudioHeader(bytes);
                if (!header) result=Result::Unsupported;
                else {
                    { std::lock_guard lock(mutex); status.header=header; }
                    if (stop.stop_requested()) result=Result::Cancelled;
                    else if (!configure(*header)) result=Result::Unsupported;
                    else {
                        uint64_t total=0;
                        for (uint32_t passes=1;;++passes) {
                            uint32_t copied=0;
                            while (copied<header->bytes && !stop.stop_requested()) {
                                const auto take=std::min(header->bytes-copied,0x1800u);
                                if (!readExact(take,bytes,stop,result)) break;
                                if (!output->produce(bytes,take,stop)) {
                                    result=stop.stop_requested()?Result::Cancelled:Result::StorageFailure; break;
                                }
                                copied+=take; total+=take;
                                { std::lock_guard lock(mutex); status.copied=total; }
                            }
                            if (copied!=header->bytes || stop.stop_requested()) break;
                            if (loop.onPass && !loop.onPass(passes)) { result=Result::StorageFailure; break; }
                            if (!loop.enabled) {
                                result=output->finish()?Result::Completed:Result::StorageFailure;
                                break;
                            }
                            // 318: skip (state+4 - bytes - 0x28) + 0x28 with 32-bit wrap as subu/addiu.
                            const uint32_t rest=loop.fileBytes?loop.fileBytes-header->bytes-0x28u:0u;
                            uint32_t skip=rest+0x28u;
                            while (skip && !stop.stop_requested()) {
                                const auto take=std::min(skip,0x10000u);
                                if (!readExact(take,bytes,stop,result)) break;
                                skip-=take;
                            }
                            if (skip || stop.stop_requested()) break;
                        }
                    }
                }
            }
            if (stop.stop_requested() && result!=Result::Completed) result=Result::Cancelled;
        } catch (...) { result=Result::StorageFailure; }
        if (result!=Result::Completed) cancel();
        finish(result);
    }
};
PdistrMpegAudio::PdistrMpegAudio():state_(std::make_unique<State>()) {}
PdistrMpegAudio::~PdistrMpegAudio() { stop(); }
bool PdistrMpegAudio::start(PdistrMpegRing& input,PdistrMpegRing& output,Configure configure,Loop loop) {
    auto& s=*state_; if (currentAudio==&s) return false;
    std::lock_guard lifecycle(s.lifecycle);
    if (s.worker.joinable() || &input==&output || !configure) return false;
    const auto source=input.snapshot(),sink=output.snapshot();
    if (!source || !sink || source->ended || sink->ended || sink->filled || sink->capacity<0x1800) return false;
    s.input=&input; s.output=&output; s.configure=std::move(configure); s.loop=std::move(loop);
    { std::lock_guard lock(s.mutex); s.status={Result::Running}; }
    try { s.worker=std::jthread([&s](std::stop_token stop) { s.run(stop); }); }
    catch (...) { s.input=s.output=nullptr; s.configure={}; s.finish(Result::StorageFailure); throw; }
    return true;
}
bool PdistrMpegAudio::stop() {
    auto& s=*state_; if (currentAudio==&s) return false;
    std::lock_guard lifecycle(s.lifecycle);
    s.worker.request_stop(); if (s.worker.joinable()) s.worker.join();
    s.cancel(); s.input=s.output=nullptr; s.configure={}; s.loop={}; return true;
}
PdistrMpegAudio::Snapshot PdistrMpegAudio::snapshot() const {
    auto& s=*state_; std::lock_guard lock(s.mutex); return s.status;
}
}
