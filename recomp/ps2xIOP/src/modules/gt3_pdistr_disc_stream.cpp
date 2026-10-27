#include "ps2x/iop/gt3/pdistr_disc_stream.h"
#include "ps2x/iop/gt3/pdistr_startup_image.h"
#include "ps2x/iop/gt3/pdispu2_resident.h"
#include "ps2x/iop/native_library.h"
#include <algorithm>
#include <condition_variable>
#include <map>
#include <mutex>
#include <thread>

namespace ps2x::iop::gt3 {
namespace { thread_local const void* currentProducer=nullptr; }
struct PdistrDiscStream::State {
    IopHost& host; PdicdvdResident& disc; NativeLibraryRegistry* registry;
    mutable std::recursive_mutex mutex,lifecycle;
    std::condition_variable_any changed;
    std::jthread producer;
    std::map<uint16_t,NativeImportBinding> imports;
    uint32_t context=0,buffer=0,descriptor=0,capacity=0;
    uint64_t sourceBytes=0,consumedBytes=0;
    bool opening=false,stopping=false,ready=false,ended=false,failed=false,ring=false;
    State(IopHost& h,PdicdvdResident& d):host(h),disc(d),registry(h.nativeLibraries()) {}
    NativeExportResult call(uint16_t ordinal,std::array<uint32_t,4> args) {
        const auto binding=imports.find(ordinal);
        return binding==imports.end()?NativeExportResult{}:registry->invoke(binding->second,args);
    }
    bool write(uint32_t at,std::span<const uint8_t> bytes) {
        const IopMemoryWrite w{at,bytes}; return host.writeIopMemoryBatch(std::span(&w,1));
    }
    void run(std::stop_token stop,uint32_t lsn,uint32_t sectors) {
        currentProducer=this;
        struct Clear { ~Clear() { currentProducer=nullptr; } } clear;
        try {
            std::unique_lock lock(mutex);
            while (sectors && !failed && !stopping && !stop.stop_requested()) {
                const auto space=call(79,{context,0,0,0});
                if (!space.handled || space.value<0) { failed=true; break; }
                if (space.value<2048) { changed.wait(lock); continue; }
                const auto count=std::min({sectors,uint32_t(space.value)/2048,64u});
                const auto dest=call(75,{context,0,0,0}); if (!dest.handled) { failed=true; break; }
                lock.unlock(); auto data=disc.readStreamSectors(lsn,count); lock.lock();
                if (failed || stopping || stop.stop_requested()) break;
                if (!data || data->size()!=count*2048 || !write(uint32_t(dest.value),*data)) { failed=true; break; }
                std::vector<uint8_t> verify(data->size());
                if (!host.readIopMemory(uint32_t(dest.value),verify.data(),verify.size()) || verify!=*data ||
                    !call(81,{context,count*2048,0,0}).handled) { failed=true; break; }
                lsn+=count; sectors-=count; ready=true; changed.notify_all();
            }
            if (!stopping && !stop.stop_requested()) {
                if (failed) call(67,{context,0,0,0});
                else if (!call(68,{context,0,0,0}).handled) failed=true;
            }
            ended=true; changed.notify_all();
        } catch (...) {
            std::lock_guard lock(mutex); failed=true; ended=true; changed.notify_all();
        }
    }
    bool release() {
        // Called with lifecycle owned and producer joined. A failed/corrupted
        // ring remains owned by LIBPDI until its reset, rather than unsafe free.
        bool ok=true;
        if (ring) {
            const auto cancelled=call(67,{context,0,0,0});
            const auto disposed=call(70,{context,0,0,0});
            ok=cancelled.handled && disposed.handled;
            if (disposed.handled) ring=false;
        }
        for (auto* address:{&descriptor,&buffer,&context}) if (*address && (!ring || address==&descriptor)) {
            if (call(14,{*address,0,0,0}).handled) *address=0; else ok=false;
        }
        if (!context && !buffer && !descriptor) { capacity=0; imports.clear(); }
        return ok;
    }
};
PdistrDiscStream::PdistrDiscStream(IopHost& h,PdicdvdResident& d):state_(std::make_unique<State>(h,d)) {}
PdistrDiscStream::~PdistrDiscStream() { try { close(); } catch (...) {} }
uint32_t PdistrDiscStream::context() const { std::lock_guard lock(state_->mutex); return state_->opening?0:state_->context; }
uint32_t PdistrDiscStream::capacity() const { std::lock_guard lock(state_->mutex); return state_->opening?0:state_->capacity; }
bool PdistrDiscStream::open(uint32_t lsn,uint32_t sectors,uint32_t desired) {
    auto& s=*state_;
    if (currentProducer==&s) return false;
    { std::lock_guard lock(s.mutex); if (s.opening || s.context || s.buffer || s.descriptor) return false; }
    std::lock_guard lifecycle(s.lifecycle);
    { std::lock_guard lock(s.mutex);
      if (s.opening || s.context || s.buffer || s.descriptor || !s.registry) return false;
      s.opening=true; s.stopping=s.ready=s.ended=s.failed=false;
    }
    struct Transaction {
        State& s; bool done=false;
        ~Transaction() {
            if (!done) {
                { std::lock_guard lock(s.mutex); s.stopping=true; }
                s.producer.request_stop(); s.changed.notify_all(); if (s.producer.joinable()) s.producer.join();
                try { s.release(); } catch (...) {}
            }
            std::lock_guard lock(s.mutex); s.opening=false;
        }
    } transaction{s};
    if (!sectors || sectors>INT32_MAX || uint64_t(lsn)+sectors>UINT32_MAX || desired<8192 ||
        desired>0x40000 || desired%8192 || !s.disc.active() || !verifyPdistrModule(s.host)) return false;
    s.sourceBytes=uint64_t(sectors)*2048; s.consumedBytes=0;
    uint64_t owner=0;
    for (uint16_t n:{11,12,13,14,40,65,67,68,70,75,76,79,80,81,82}) {
        auto binding=s.registry->bind("libpdi",0x10a,n);
        if (!binding.registration || (owner && binding.registration!=owner)) return false;
        owner=binding.registration; s.imports[n]=std::move(binding);
    }
    const auto context=s.call(12,{0xcc,0,0,0}); if (!context.handled || context.value<=0) return false;
    s.context=uint32_t(context.value);
    const auto buffer=s.call(13,{8192,8192,desired,s.context+0x88}); if (!buffer.handled || buffer.value<=0) return false;
    s.buffer=uint32_t(buffer.value);
    if (!s.host.readIopMemory(s.context+0x88,&s.capacity,4) || s.capacity<8192 || s.capacity>desired || s.capacity%8192) return false;
    const auto descriptor=s.call(11,{16,0,0,0}); if (!descriptor.handled || descriptor.value<=0) return false;
    s.descriptor=uint32_t(descriptor.value);
    if (!s.call(65,{s.context,s.buffer,s.capacity,0}).handled) return false;
    s.ring=true;
    // Preserve audited source/buffer/job fields. Reader queue ownership and
    // worker completion live in native objects, not fabricated IOP imports.
    std::array<uint32_t,10> job{s.buffer,0,0,0,0,s.context,1,lsn,sectors,0x100};
    if (!s.write(s.context+0x84,std::span(reinterpret_cast<const uint8_t*>(job.data()),sizeof(job)))) return false;
    s.producer=std::jthread([&s,lsn,sectors](std::stop_token stop) { s.run(stop,lsn,sectors); });
    std::unique_lock lock(s.mutex); s.changed.wait(lock,[&] { return s.ready || s.ended; });
    if (s.failed || !s.ready) return false;
    transaction.done=true; return true;
}
bool PdistrDiscStream::consume(uint32_t destination,uint32_t bytes) {
    auto& s=*state_;
    if (currentProducer==&s) return false;
    { std::lock_guard lock(s.mutex); if (s.opening || s.stopping || !s.context) return false; }
    std::lock_guard lifecycle(s.lifecycle); std::unique_lock lock(s.mutex);
    if (s.opening || s.stopping || !s.context || s.failed || bytes>s.sourceBytes-s.consumedBytes) return false;
    const auto chunk=s.capacity/4;
    if (bytes) {
        const bool eeRam=destination<0x2000000 || destination-0x20000000u<0x2000000 ||
            destination-0x80000000u<0x2000000 || destination-0xa0000000u<0x2000000;
        uint32_t physical=0,read=0;
        const uint64_t dma=(uint64_t(bytes)+15)&~uint64_t(15);
        if (!eeRam || !s.host.normalizeGuestAddress(destination,physical) || !physical || (physical&15) ||
            physical>=0x2000000 || dma>0x2000000-physical || !s.host.readIopMemory(s.context+0x1c,&read,4) || read>=s.capacity) return false;
        // Validate all chunk geometry before copying any data. Original203c
        // advances the destination once per outer chunk; its fragmented inner
        // copies have a distinct contract that is deliberately still rejected.
        for (uint32_t left=bytes;left;) {
            const auto take=std::min(left,chunk);
            if (((take+15u)&~15u)>s.capacity-read) return false;
            left-=take; read=(read+take)%s.capacity;
        }
    }
    struct Progress {
        State& s; uint32_t copied=0; bool done=false;
        ~Progress() {
            // Original errors can follow completed DMAs. Keep those bytes, but
            // never replay a partially consumed request from the next cursor.
            if (copied && !done) { s.failed=true; try { s.call(67,{s.context,0,0,0}); } catch (...) {} s.changed.notify_all(); }
        }
    } progress{s};
    do {
        const auto take=std::min(bytes-progress.copied,chunk);
        for (;;) {
            if (s.failed || s.stopping) return false;
            const auto available=s.call(80,{s.context,0,0,0});
            if (!available.handled) return false;
            if (uint32_t(available.value)>=take) break;
            if (s.ended) return false;
            s.changed.wait(lock);
        }
        const std::array<uint32_t,4> args{s.context,destination+progress.copied,take,chunk};
        if (!s.write(s.descriptor,std::span(reinterpret_cast<const uint8_t*>(args.data()),sizeof(args))) ||
            !s.call(40,{s.descriptor,0,0,0}).handled) return false;
        progress.copied+=take; s.consumedBytes+=take;
        s.changed.notify_all();
    } while (progress.copied<bytes);
    progress.done=true; return true;
}
bool PdistrDiscStream::consumeSound(Pdispu2Resident& sound,uint32_t destination,uint32_t bytes) {
    auto& s=*state_;
    if (currentProducer==&s) return false;
    { std::lock_guard lock(s.mutex); if (s.opening || s.stopping || !s.context) return false; }
    std::lock_guard lifecycle(s.lifecycle); std::unique_lock lock(s.mutex);
    const uint64_t rounded=(uint64_t(bytes)+63)&~uint64_t(63);
    if (s.opening || s.stopping || !s.context || s.failed || rounded>s.sourceBytes-s.consumedBytes ||
        (destination&15) || destination>=0x200000 || rounded>0x200000-destination) return false;
    if (!bytes) return true;
    uint32_t cursor=0;
    if (!s.host.readIopMemory(s.context+0x1c,&cursor,4) || cursor>=s.capacity) return false;
    const auto chunk=s.capacity/4;
    // Coalesce wrapped intermediate chunks into owned staging storage. Their
    // original fragment padding is overwritten by later chunks. Keep a strict
    // guard on final padding whose source/length would cross the allocation.
    for (uint32_t left=bytes;left;) {
        const auto take=std::min(left,chunk),dma=(take+63u)&~63u;
        const auto contiguous=s.capacity-cursor;
        if (left==take && ((take<=contiguous && dma>contiguous) || (take>contiguous && contiguous%64))) return false;
        cursor=(cursor+take)%s.capacity;left-=take;
    }
    const auto allocation=s.call(11,{chunk,0,0,0});if (!allocation.handled || allocation.value<=0) return false;
    const uint32_t scratch=uint32_t(allocation.value);
    struct Scratch { State& s;uint32_t address;~Scratch() { try { s.call(14,{address,0,0,0}); } catch (...) {} } } storage{s,scratch};
    struct Progress {
        State& s; bool attempted=false,done=false;
        ~Progress() {
            if (attempted && !done) { s.failed=true; try { s.call(67,{s.context,0,0,0}); } catch (...) {} s.changed.notify_all(); }
        }
    } progress{s};
    for (uint32_t copied=0;copied<bytes;) {
        const auto take=std::min(bytes-copied,chunk),dma=(take+63u)&~63u;
        for (;;) {
            if (s.failed || s.stopping) return false;
            const auto available=s.call(80,{s.context,0,0,0});
            if (!available.handled) return false;
            uint32_t filled=0;
            if (!s.host.readIopMemory(s.context+0x14,&filled,4) || filled>s.capacity) return false;
            if (filled>=dma) break;
            if (s.ended) return false;
            s.changed.wait(lock);
        }
        const auto source=s.call(76,{s.context,0,0,0});if (!source.handled) return false;
        const auto at=uint32_t(source.value);
        if (at<s.buffer || at>=s.buffer+s.capacity) return false;
        const auto first=std::min(dma,s.buffer+s.capacity-at);
        std::vector<uint8_t> staged(dma);
        if (!s.host.readIopMemory(at,staged.data(),first) ||
            (first<dma && !s.host.readIopMemory(s.buffer,staged.data()+first,dma-first)) || !s.write(scratch,staged)) return false;
        std::vector<uint8_t> verify(dma);
        if (!s.host.readIopMemory(scratch,verify.data(),verify.size()) || verify!=staged) return false;
        progress.attempted=true;
        if (!sound.transferIopData(scratch,destination+copied,take) ||
            !s.call(82,{s.context,take,0,0}).handled) return false;
        copied+=take;s.consumedBytes+=take;s.changed.notify_all();
    }
    progress.done=true;return true;
}
bool PdistrDiscStream::close() {
    auto& s=*state_;
    if (currentProducer==&s) return false;
    { std::lock_guard lock(s.mutex); if (s.opening) return false; s.stopping=true; }
    s.changed.notify_all(); std::lock_guard lifecycle(s.lifecycle);
    s.producer.request_stop(); s.changed.notify_all(); if (s.producer.joinable()) s.producer.join();
    std::lock_guard lock(s.mutex); return s.release();
}
}
