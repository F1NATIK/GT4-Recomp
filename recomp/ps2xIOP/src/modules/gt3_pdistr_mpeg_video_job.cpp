#include "ps2x/iop/gt3/pdistr_mpeg_video_job.h"
#include "ps2x/iop/gt3/pdistr_mpeg_demux.h"
#include "ps2x/iop/gt3/pdispu2_resident.h"
#include "ps2x/iop/native_library.h"
#include <condition_variable>
#include <limits>
#include <mutex>
#include <thread>
#include <atomic>
#include <chrono>

namespace ps2x::iop::gt3 {
namespace {
// PDISPU2's typed entries refuse (rather than block) while another of its
// operations is committing; e.g. the parser thread starting the movie-audio
// job. Retry such transient refusals for a bounded time.
template<class F> bool retryBusy(F&& attempt) {
    for (unsigned tries=0;tries<200;++tries) {
        if (attempt()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return false;
}
bool putWords(IopHost& host,uint32_t address,std::initializer_list<std::pair<uint32_t,uint32_t>> fields) {
    std::vector<std::array<uint8_t,4>> storage; storage.reserve(fields.size());
    std::vector<IopMemoryWrite> writes; writes.reserve(fields.size());
    for (const auto& [offset,value]:fields) {
        storage.push_back({uint8_t(value),uint8_t(value>>8),uint8_t(value>>16),uint8_t(value>>24)});
        writes.push_back({address+offset,storage.back()});
    }
    if (!host.writeIopMemoryBatch(writes)) return false;
    for (const auto& [offset,value]:fields) {
        uint32_t observed=0;
        if (!host.readIopMemory(address+offset,&observed,4) || observed!=value) return false;
    }
    return true;
}
}
struct PdistrMpegVideoJob::State {
    struct Job {
        IopHost& host;
        PdistrMpegRing sourceRing,videoRing,rawAudioRing,audioRing;
        PdistrMpegSource source;
        PdistrMpegDemux demux;
        PdistrMpegAudio parser;
        std::stop_source cancel;
        AudioRoute route;
        uint32_t descriptors=0,sourceBytes=0,videoBytes=0,audioBytes=0;
        uint64_t token=0;
        bool reading=false,closing=false,failed=false,withAudio=false,timingOwned=false;
        std::atomic<bool> audioJobStarted{false};
        Job(IopHost& h,PdicdvdResident& disc):host(h),sourceRing(h),videoRing(h),rawAudioRing(h),audioRing(h),source(disc) {}
        void cancelEndpoints() noexcept {
            cancel.request_stop();
            for (auto* ring:{&sourceRing,&videoRing,&rawAudioRing,&audioRing}) {
                try { ring->cancel(); } catch (...) { ring->stop(); }
            }
        }
        // Original d98 exit order: the sound job (3a4 joins it) before the
        // rings it reads are torn down. Stopping it first also keeps a
        // cancelled ring from being reported as a job failure.
        bool stopAudioJob() {
            if (!withAudio || !audioJobStarted) return true;
            return route.sound->stopMovieAudio();
        }
        bool joinWorkers() { const bool a=source.stop(),b=demux.stop(),c=parser.stop(); return a && b && c; }
        void releaseTiming() {
            // 3a4 -> export51 on the timing object after the job has joined.
            if (timingOwned && retryBusy([&] { return route.sound->releaseStreamControl(route.timing); })) timingOwned=false;
        }
        void freeStorage() {
            sourceRing.stop(); videoRing.stop(); rawAudioRing.stop(); audioRing.stop();
            if (audioBytes) host.freeIopMemory(audioBytes);
            if (videoBytes) host.freeIopMemory(videoBytes);
            if (sourceBytes) host.freeIopMemory(sourceBytes);
            if (descriptors) host.freeIopMemory(descriptors);
            audioBytes=videoBytes=sourceBytes=descriptors=0;
        }
        ~Job() {
            // Failed unpublished startup has no reader. Successful stop already
            // zeroed allocation ownership before releasing its retained state.
            if (descriptors || sourceBytes || videoBytes || audioBytes || timingOwned || audioJobStarted) {
                stopAudioJob(); cancelEndpoints(); joinWorkers(); releaseTiming(); freeStorage();
            }
        }
        // Original d0c + constructor 0 (PDISTR 31f0): audio buffer 0xa000,
        // raw ring 3350 (buf+6000/cap 4000), secondary ring 3208 (buf+3000/
        // cap 3000), timing 3290 (export41 + range [0,max)), ADPCM descriptor
        // 32c8 (export52) and PCM descriptor 3314 (export58), then range
        // (-1,-1): audio waits for the EE's first RPC3 range.
        bool prepareAudio() {
            auto& r=route; auto* registry=r.registry;
            auto spu=[&](uint16_t ordinal,uint32_t argument) {
                const auto binding=registry->bind("pdispu2",0x109,ordinal);
                return binding.registration && registry->invoke(binding,{argument,0,0,0}).handled;
            };
            audioBytes=host.allocateIopMemory(0xa000);
            if (!audioBytes || (audioBytes&15) ||
                !rawAudioRing.initialize(r.rawRing,audioBytes+0x6000,0x4000) ||
                !audioRing.initialize(r.secondaryRing,audioBytes+0x3000,0x3000)) return false;
            if (!spu(41,r.timing)) return false;
            timingOwned=true;
            if (!r.sound->updateStreamRange(r.timing,{0,0,0xffffffffu,0x7fffffffu}) ||
                !spu(52,r.descriptor) ||
                !putWords(host,r.descriptor,{{0x18,r.volumeReference},{0x1c,r.timing},{0x28,r.secondaryRing},{0x2c,audioBytes}}) ||
                !spu(58,r.pcmDescriptor) ||
                !putWords(host,r.pcmDescriptor,{{0x18,r.volumeReference},{0x1c,r.timing},{0x24,r.secondaryRing},
                                                {0x28,audioBytes},{0x2c,0x3000}}) ||
                !r.sound->updateStreamRange(r.timing,{0xffffffffu,0xffffffffu,0xffffffffu,0xffffffffu})) return false;
            return true;
        }
        // Parser 100's sound-job launch for one header. Format16 (ADPCM):
        // pitch -> descriptor+30, export53/55 job on 32c8. The +c period flag
        // (export43) is 0 for MPEG. PCM (format1, job 26fc on 3314) and
        // non-stereo/non-1024 interleave are not implemented: refuse.
        bool configureAudio(const PdistrAudioHeader& header) {
            if (header.format!=16 || header.channels!=2 || header.interleave!=1024 || !header.pitch || header.pitch>0x3fff) {
                try { host.log(LogLevel::Warning,"[GT3/PDISTR] MPEG audio header not supported natively (format="+
                    std::to_string(header.format)+" channels="+std::to_string(header.channels)+
                    " interleave="+std::to_string(header.interleave)+"); audio branch cancelled"); } catch (...) {}
                return false;
            }
            if (!putWords(host,route.descriptor,{{0x30,header.pitch}})) return false;
            if (!route.sound->startMovieAudio(route.descriptor,audioRing)) return false;
            audioJobStarted=true;
            try { host.log(LogLevel::Info,"[GT3/PDISTR] MPEG audio job started: ADPCM stereo rate="+std::to_string(header.rate)+
                " pitch="+std::to_string(header.pitch)+" payload="+std::to_string(header.bytes)+
                "; waits for EE RPC3 timing range"); } catch (...) {}
            return true;
        }
    };
    IopHost& host; PdicdvdResident& disc;
    std::mutex lifecycle; mutable std::mutex mutex;
    std::condition_variable changed;
    std::shared_ptr<Job> job;
    uint64_t nextToken=0;
    State(IopHost& h,PdicdvdResident& d):host(h),disc(d) {}
};
PdistrMpegVideoJob::PdistrMpegVideoJob(IopHost& host,PdicdvdResident& disc):state_(std::make_unique<State>(host,disc)) {}
PdistrMpegVideoJob::~PdistrMpegVideoJob() { stop(); }
uint64_t PdistrMpegVideoJob::start(uint32_t flags,uint32_t lsn,uint32_t sectors,
                                  uint32_t sourceDescriptor,uint32_t videoDescriptor,const AudioRoute* audio) {
    auto& s=*state_; std::lock_guard lifecycle(s.lifecycle);
    const bool withAudio=!(flags&1),repeat=(flags&2)!=0;
    if (flags>3 || !sectors || sectors>0x1fffff || uint64_t(lsn)+sectors>UINT32_MAX) return 0;
    if (withAudio && (!audio || !audio->sound || !audio->registry || !audio->rawRing || !audio->secondaryRing ||
        !audio->timing || !audio->descriptor || !audio->pcmDescriptor || !audio->volumeReference)) return 0;
    if (sourceDescriptor || videoDescriptor) {
        std::array<uint8_t,0x84> bytes{};
        if (!sourceDescriptor || !videoDescriptor || (sourceDescriptor&3) || (videoDescriptor&3) ||
            sourceDescriptor>UINT32_MAX-bytes.size() || videoDescriptor>UINT32_MAX-bytes.size() ||
            (uint64_t(sourceDescriptor)<uint64_t(videoDescriptor)+bytes.size() &&
             uint64_t(videoDescriptor)<uint64_t(sourceDescriptor)+bytes.size()) ||
            !s.host.readIopMemory(sourceDescriptor,bytes.data(),bytes.size()) ||
            !s.host.readIopMemory(videoDescriptor,bytes.data(),bytes.size())) return 0;
    }
    {
        std::lock_guard lock(s.mutex);
        if (s.job || s.nextToken==std::numeric_limits<uint64_t>::max()) return 0;
    }
    auto job=std::make_shared<State::Job>(s.host,s.disc);
    if (withAudio) { job->withAudio=true; job->route=*audio; }
    if (!sourceDescriptor) {
        job->descriptors=s.host.allocateIopMemory(0x200);
        if (!job->descriptors) return 0;
        sourceDescriptor=job->descriptors; videoDescriptor=job->descriptors+0x100;
    }
    job->sourceBytes=s.host.allocateIopMemory(0x40000);
    job->videoBytes=s.host.allocateIopMemory(0x8000);
    if (!job->sourceBytes || !job->videoBytes ||
        !job->sourceRing.initialize(sourceDescriptor,job->sourceBytes,0x40000) ||
        !job->videoRing.initialize(videoDescriptor,job->videoBytes,0x8000)) return 0;
    if (withAudio) {
        if (!job->prepareAudio()) return 0;
        // d98: the parser job (378) starts before demux 1750.
        auto* raw=job.get();
        if (!job->parser.start(job->rawAudioRing,job->audioRing,[raw](const PdistrAudioHeader& header) {
            return raw->configureAudio(header);
        })) return 0;
    }
    if (!job->demux.start(job->sourceRing,job->videoRing,withAudio?&job->rawAudioRing:nullptr,repeat?sectors:0) ||
        !job->source.start(job->sourceRing,lsn,sectors,repeat)) return 0;
    std::lock_guard lock(s.mutex);
    job->token=++s.nextToken; s.job=std::move(job); return s.job->token;
}
PdistrMpegRing::Transfer PdistrMpegVideoJob::read(uint64_t token,uint32_t destination,uint32_t bytes) {
    auto& s=*state_; std::shared_ptr<State::Job> job;
    {
        std::lock_guard lock(s.mutex); job=s.job;
        if (!job || !token || token!=job->token || job->closing)
            return {PdistrMpegRing::TransferResult::Stopped,0};
        if (job->reading || job->failed) return {};
        job->reading=true;
    }
    struct Complete {
        State& s; State::Job& job;
        ~Complete() { std::lock_guard lock(s.mutex); job.reading=false; s.changed.notify_all(); }
    } complete{s,*job};
    auto result=job->videoRing.consumeRecordsToEe(destination,bytes,job->cancel.get_token());
    // A failed source/demux cancels the ring too; distinguish it from an owner
    // cancellation so an invalid stream cannot masquerade as normal shutdown.
    if (result.result==PdistrMpegRing::TransferResult::Stopped && !job->cancel.stop_requested())
        result.result=PdistrMpegRing::TransferResult::Failed;
    if (result.result==PdistrMpegRing::TransferResult::Failed) {
        { std::lock_guard lock(s.mutex); job->failed=true; }
        job->stopAudioJob();
        job->cancelEndpoints();
    }
    return result;
}
bool PdistrMpegVideoJob::updateAudioRange(uint64_t token,const std::array<uint32_t,4>& range) {
    auto& s=*state_; std::shared_ptr<State::Job> job;
    { std::lock_guard lock(s.mutex); job=s.job; }
    if (!job || !token || token!=job->token || job->closing || !job->withAudio || !job->timingOwned) return false;
    return retryBusy([&] { return job->route.sound->updateStreamRange(job->route.timing,range); });
}
std::optional<PdistrMpegVideoJob::AudioSnapshot> PdistrMpegVideoJob::audio() const {
    auto& s=*state_; std::shared_ptr<State::Job> job;
    { std::lock_guard lock(s.mutex); job=s.job; }
    if (!job || !job->withAudio) return {};
    AudioSnapshot result; result.parser=job->parser.snapshot(); result.jobStarted=job->audioJobStarted;
    if (result.jobStarted) result.job=job->route.sound->movieAudio();
    return result;
}
bool PdistrMpegVideoJob::stop() {
    auto& s=*state_; std::lock_guard lifecycle(s.lifecycle);
    std::shared_ptr<State::Job> job;
    {
        std::lock_guard lock(s.mutex); job=s.job;
        if (!job) return true;
        job->closing=true;
    }
    job->stopAudioJob();
    job->cancelEndpoints(); const bool joined=job->joinWorkers();
    if (!joined) return false; // Keep storage retained if a worker cannot join itself.
    // A parser that launched the job after stopAudioJob above has joined now.
    job->stopAudioJob(); job->audioJobStarted=false;
    job->releaseTiming();
    {
        std::unique_lock lock(s.mutex); s.changed.wait(lock,[&] { return !job->reading; });
        // The reader no longer touches host/worker/ring state after publishing
        // completion. Free now, not in a delayed last shared_ptr destructor.
        job->freeStorage(); s.job.reset();
    }
    return joined;
}
}
