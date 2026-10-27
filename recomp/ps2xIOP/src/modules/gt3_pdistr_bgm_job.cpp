#include "ps2x/iop/gt3/pdistr_bgm_job.h"
#include "ps2x/iop/gt3/pdispu2_resident.h"
#include "ps2x/iop/native_library.h"
#include <atomic>
#include <chrono>
#include <mutex>
#include <thread>

namespace ps2x::iop::gt3 {
namespace {
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
struct PdistrBgmJob::State {
    IopHost& host; PdicdvdResident& disc;
    mutable std::mutex mutex;
    std::recursive_mutex lifecycle;
    PdistrMpegRing rawRing,audioRing;
    PdistrMpegSource source;
    PdistrMpegAudio parser;
    Route route;
    uint32_t buffer=0;
    bool loop=false;
    bool timingOwned=false,running=false;
    std::atomic<bool> jobStarted{false};
    State(IopHost& h,PdicdvdResident& d):host(h),disc(d),rawRing(h),audioRing(h),source(d) {}
    bool spu(uint16_t ordinal,uint32_t argument) {
        const auto binding=route.registry->bind("pdispu2",0x109,ordinal);
        return binding.registration && route.registry->invoke(binding,{argument,0,0,0}).handled;
    }
    // Parser 100, format 16: state+0 = 0, samples -> state+10/14, pitch ->
    // descriptor+30 (= state+108); if state+c (loop) is set, export43 on the
    // timing (state+a0 = 2d90) with the period = samples; then the export55 job
    // on descriptor 2dc8.
    bool configure(const PdistrAudioHeader& header) {
        if (header.format!=16 || header.channels!=2 || header.interleave!=1024 || !header.pitch || header.pitch>0x3fff) {
            try { host.log(LogLevel::Warning,"[GT3/PDISTR] BGM header not supported natively (format="+
                std::to_string(header.format)+" channels="+std::to_string(header.channels)+
                " interleave="+std::to_string(header.interleave)+"); BGM cancelled"); } catch (...) {}
            return false;
        }
        if (!putWords(host,route.state,{{0,0},{0x10,header.samples},{0x14,0}}) ||
            !putWords(host,route.descriptor,{{0x30,header.pitch}})) return false;
        if (loop) {
            const auto binding=route.registry->bind("pdispu2",0x109,43);
            if (!binding.registration || !route.registry->invoke(binding,{route.timing,header.samples,0,0}).handled)
                return false;
        }
        if (!route.sound->startMovieAudio(route.descriptor,audioRing)) return false;
        jobStarted=true;
        try { host.log(LogLevel::Info,"[GT3/PDISTR] BGM job started: ADPCM stereo rate="+std::to_string(header.rate)+
            " pitch="+std::to_string(header.pitch)+" payload="+std::to_string(header.bytes)); } catch (...) {}
        return true;
    }
    void teardown() {
        if (jobStarted) route.sound->stopMovieAudio();
        for (auto* ring:{&rawRing,&audioRing}) { try { ring->cancel(); } catch (...) { ring->stop(); } }
        source.stop(); parser.stop();
        // A parser that launched the job after the first stop has joined now.
        if (jobStarted) { route.sound->stopMovieAudio(); jobStarted=false; }
        if (timingOwned && retryBusy([&] { return route.sound->releaseStreamControl(route.timing); })) timingOwned=false;
        rawRing.stop(); audioRing.stop();
        if (buffer) host.freeIopMemory(buffer);
        buffer=0;
        std::lock_guard lock(mutex); running=false;
    }
};
PdistrBgmJob::PdistrBgmJob(IopHost& host,PdicdvdResident& disc):state_(std::make_unique<State>(host,disc)) {}
PdistrBgmJob::~PdistrBgmJob() { stop(); }
bool PdistrBgmJob::start(const Route& route,uint32_t lsn,uint32_t sectors,uint32_t bufferBytes,bool loop) {
    auto& s=*state_; std::lock_guard lifecycle(s.lifecycle);
    { std::lock_guard lock(s.mutex); if (s.running) return false; }
    if (!route.sound || !route.registry || !route.state || !route.rawRing || !route.secondaryRing || !route.timing ||
        !route.descriptor || !route.pcmDescriptor || !route.volumeReference ||
        !sectors || sectors>0x1fffff || uint64_t(lsn)+sectors>UINT32_MAX ||
        bufferBytes<0x1800 || (bufferBytes&0x7ff) || bufferBytes>0x100000) return false;
    s.route=route; s.loop=loop;
    { std::lock_guard lock(s.mutex); s.running=true; }
    struct Rollback { State& s; bool committed=false; ~Rollback() { if (!committed) s.teardown(); } } rollback{s};
    // 4e8: allocation size+0x6000, raw ring 2c30 over buffer+0x6000.
    s.buffer=s.host.allocateIopMemory(bufferBytes+0x6000);
    if (!s.buffer || (s.buffer&15) ||
        !s.rawRing.initialize(route.rawRing,s.buffer+0x6000,bufferBytes) ||
        !s.audioRing.initialize(route.secondaryRing,s.buffer+0x3000,0x3000)) return false;
    // Constructor 0 (PDISTR 0x0) on state 2cf0.
    if (!putWords(s.host,route.state,{{0,0xffffffffu},{4,sectors<<11},{8,0},{0xc,loop?1u:0u},{0x10,0xffffffffu},
                                      {0x14,0x7fffffffu},{0x9c,route.rawRing}})) return false;
    if (!s.spu(41,route.timing)) return false;
    s.timingOwned=true;
    if (!route.sound->updateStreamRange(route.timing,{0,0,0xffffffffu,0x7fffffffu}) ||
        !s.spu(52,route.descriptor) ||
        !putWords(s.host,route.descriptor,{{0x18,route.volumeReference},{0x1c,route.timing},
                                           {0x28,route.secondaryRing},{0x2c,s.buffer}}) ||
        !s.spu(58,route.pcmDescriptor) ||
        !putWords(s.host,route.pcmDescriptor,{{0x18,route.volumeReference},{0x1c,route.timing},
                                              {0x24,route.secondaryRing},{0x28,s.buffer},{0x2c,0x3000}})) return false;
    auto* raw=&s;
    // 100 counts passes in state+8; with the loop byte it repeats the payload
    // while the reader 2670 loops the file.
    PdistrMpegAudio::Loop repeat;
    repeat.enabled=loop; repeat.fileBytes=sectors<<11;
    repeat.onPass=[raw](uint32_t passes) { return putWords(raw->host,raw->route.state,{{8,passes}}); };
    if (!s.parser.start(s.rawRing,s.audioRing,[raw](const PdistrAudioHeader& h) { return raw->configure(h); },
                        std::move(repeat)) ||
        !s.source.start(s.rawRing,lsn,sectors,loop)) return false;
    rollback.committed=true;
    return true;
}
bool PdistrBgmJob::stop() {
    auto& s=*state_; std::lock_guard lifecycle(s.lifecycle);
    { std::lock_guard lock(s.mutex); if (!s.running) return true; }
    s.teardown(); return true;
}
bool PdistrBgmJob::active() const { auto& s=*state_; std::lock_guard lock(s.mutex); return s.running; }
std::optional<PdistrBgmJob::Snapshot> PdistrBgmJob::snapshot() const {
    auto& s=*state_;
    { std::lock_guard lock(s.mutex); if (!s.running) return {}; }
    Snapshot result; result.parser=s.parser.snapshot(); result.jobStarted=s.jobStarted;
    if (result.jobStarted) result.job=s.route.sound->movieAudio();
    return result;
}
}
