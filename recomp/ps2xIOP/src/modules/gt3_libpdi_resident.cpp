#include "ps2x/iop/gt3/libpdi_resident.h"
#include "ps2x/iop/native_library.h"
#include <algorithm>
#include <mutex>
#include <chrono>
#include <map>
#include <atomic>

namespace ps2x::iop::gt3 {
namespace {
constexpr uint32_t ImageBytes=0x2740;
std::atomic<uint64_t> nextStreamSemaphore{0x6f000000};
std::vector<uint8_t> verifiedImage(IopHost& host) {
    const auto file=host.openHostFile(host.translateGuestPath("cdrom0:\\LIBPDI.IRX;1")); if (!file) return {};
    struct Close { IopHost& host; uint64_t file; ~Close() { host.closeHostFile(file); } } close{host,file};
    uint64_t size=0; size_t count=0; std::array<uint8_t,32> digest{};
    if (!host.hostFileSize(file,size) || size<0x27e0 || size>1024*1024) return {};
    std::vector<uint8_t> bytes(static_cast<size_t>(size));
    if (!host.readHostFile(file,0,bytes.data(),bytes.size(),count) || count!=bytes.size() || !host.sha256(bytes,digest)) return {};
    constexpr std::string_view expected="6797023b2dc5d59562254d764e1f1b86391518ff9c8f5c999110a06262b3ad6c";
    constexpr char hex[]="0123456789abcdef"; std::string actual;
    for (auto b:digest) { actual+=hex[b>>4]; actual+=hex[b&15]; }
    if (actual!=expected) return {};
    // Verified rodata/data sections only. Native handlers never execute copied
    // IOP instructions; original0x2700 name pointer is relocated at startup.
    std::vector<uint8_t> image(ImageBytes);
    std::copy_n(bytes.begin()+0x2780,0x60,image.begin()+0x26e0); return image;
}
void put(std::span<uint8_t> bytes,size_t at,uint32_t value) {
    for (unsigned i=0;i<4;++i) bytes[at+i]=uint8_t(value>>(8*i));
}
uint32_t word(std::span<const uint8_t> bytes,size_t at) {
    uint32_t value=0; for (unsigned i=0;i<4;++i) value|=uint32_t(bytes[at+i])<<(8*i); return value;
}
}
struct LibpdiResident::State {
    IopHost& host; NativeLibraryRegistry* registry;
    mutable std::recursive_mutex mutex,lifecycle;
    uint32_t image=0; uint64_t registration=0;
    bool running=false,busy=false;
    // Only export11/12/13 allocations belong to this owner. Never free another
    // resident's image/stack, EE RAM, an interior pointer or a stale binding.
    std::map<uint32_t,uint32_t> allocations;
    // A live context/buffer pair is pinned until export70 or resident reset.
    // The resident mutex backs each count1/max1 ring semaphore. Only immediate
    // waits are implemented; no guest waiter or longjmp environment is invented.
    struct Ring { uint32_t buffer; std::array<uint8_t,0x84> bytes; };
    std::map<uint32_t,Ring> rings;
    SecondsSource seconds;
    explicit State(IopHost& h,SecondsSource source):host(h),registry(h.nativeLibraries()),seconds(std::move(source)) {
        if (!seconds) {
            // A monotonic native IOP epoch, retained across this library's reset.
            // PDICDVD calibrates its calendar offset against the same source.
            const auto epoch=std::chrono::steady_clock::now();
            seconds=[epoch] { return uint64_t(std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now()-epoch).count()); };
        }
    }
    bool initialized() const {
        std::array<uint32_t,3> state{};
        return running && !busy && image && host.readIopMemory(image+0x2700,state.data(),sizeof(state)) &&
               state==std::array<uint32_t,3>{image+0x26e0,0x10a,1};
    }
    NativeExportResult ringCall(uint16_t ordinal,const std::array<uint32_t,4>& args) {
        struct Busy { bool& flag; ~Busy() { flag=false; } } operation{busy}; busy=true;
        std::array<uint32_t,4> descriptor{};
        if (ordinal==40 && ((args[0]&3) || !host.readIopMemory(args[0],descriptor.data(),sizeof(descriptor)))) return {};
        const auto context=ordinal==40?descriptor[0]:args[0];
        if (ordinal==65) {
            const auto owner=allocations.find(context),buffer=allocations.find(args[1]);
            if (owner==allocations.end() || owner->second<0x84 || buffer==allocations.end() ||
                !args[2] || args[2]>buffer->second || context==args[1] || rings.contains(context) ||
                std::any_of(rings.begin(),rings.end(),[&](const auto& r) {
                    return r.first==args[1] || r.second.buffer==context || r.second.buffer==args[1];
                })) return {};
            std::vector<uint8_t> data(args[2]); std::array<uint8_t,0x84> old{};
            if (!host.readIopMemory(context,old.data(),old.size()) || !host.readIopMemory(args[1],data.data(),data.size())) return {};
            const auto semaphore=nextStreamSemaphore.fetch_add(1);
            if (semaphore>=0x70000000) return {};
            Ring ring{args[1],{}};
            put(ring.bytes,0,uint32_t(semaphore)); put(ring.bytes,0xc,args[1]); put(ring.bytes,0x10,args[2]);
            auto [it,inserted]=rings.emplace(context,ring);
            struct Pending { std::map<uint32_t,Ring>& rings; uint32_t context; ~Pending() { if (context) rings.erase(context); } } pending{rings,context};
            const IopMemoryWrite write{context,it->second.bytes};
            if (!host.writeIopMemoryBatch(std::span(&write,1))) return {};
            pending.context=0; return {true,int32_t(semaphore)};
        }
        const auto found=rings.find(context); if (found==rings.end()) return {};
        auto& ring=found->second;
        std::array<uint8_t,0x84> actual{}; uint8_t last=0;
        const auto size=word(ring.bytes,0x10);
        if (!host.readIopMemory(context,actual.data(),actual.size()) || actual!=ring.bytes ||
            !host.readIopMemory(ring.buffer+size-1,&last,1)) return {};
        const auto filled=word(actual,0x14),producer=word(actual,0x18),consumer=word(actual,0x1c);
        if (ordinal==40) {
            // Original203c consumes descriptor {ring, EE destination, bytes,
            // chunk}. Complete only one ready contiguous chunk. Blocking,
            // fragmented/multiple chunks and saved guest longjmps stay absent.
            const auto count=descriptor[2],chunk=descriptor[3];
            if (actual[0x22] || count>INT32_MAX || chunk>INT32_MAX) return {};
            if (!count) return {true,0};
            const auto dma=(count+15u)&~15u;
            const auto raw=descriptor[1];
            // The general host resolver mirrors wider ranges for legacy code.
            // DMA accepts only the four explicit32MiB EE RAM windows here.
            const bool eeRam=raw<0x2000000 || raw-0x20000000u<0x2000000 ||
                raw-0x80000000u<0x2000000 || raw-0xa0000000u<0x2000000;
            uint32_t destination=0;
            if (!eeRam || !chunk || count>chunk || count>filled || count>size-consumer || dma>size-consumer ||
                !host.normalizeGuestAddress(descriptor[1],destination) || !destination || (destination&15) ||
                destination>=0x2000000 || dma>0x2000000-destination) return {};
            std::vector<uint8_t> data(dma);
            if (!host.readIopMemory(ring.buffer+consumer,data.data(),data.size())) return {};
            put(actual,0x1c,(consumer+count)%size); put(actual,0x14,filled-count);
            // Commit the validated physical EE span, including uncached RAM
            // aliases accepted by the host's address resolver.
            const std::array<IopMemoryWrite,2> writes{{{context,actual},{destination,data}}};
            if (!host.commitIopAndEeMemory(writes)) return {};
            ring.bytes=actual; return {true,0};
        }
        if (ordinal==75) return {true,int32_t(ring.buffer+producer)};
        if (ordinal==76) return {true,int32_t(ring.buffer+consumer)};
        if (ordinal==79) return {true,int32_t(std::min(size-producer,size-filled))};
        if (ordinal==80) return {true,int32_t(std::min(size-consumer,filled))};
        if (ordinal==73 || ordinal==74) {
            if (args[1]>INT32_MAX) return {};
            if (actual[0x22]) return {true,0};
            if (actual[0x23] || (ordinal==73?size-filled:filled)>=args[1]) return {true,1};
            return {}; // Original would block; never complete an unsatisfied wait.
        }
        if (ordinal==66) {
            for (size_t at:{4,8,0x14,0x18,0x1c}) put(actual,at,0);
        } else if (ordinal==67) actual[0x22]=1;
        else if (ordinal==68) actual[0x23]=1;
        else if (ordinal==70) { put(actual,0,0); actual[0x22]=actual[0x23]=1; }
        else if (ordinal==81 || ordinal==82) {
            if (args[1]>INT32_MAX) return {};
            const auto take=std::min(args[1],ordinal==81?size-filled:filled);
            if (ordinal==81) { put(actual,0x18,(producer+take)%size); put(actual,0x14,filled+take); }
            else { put(actual,0x1c,(consumer+take)%size); put(actual,0x14,filled-take); }
        } else return {};
        const IopMemoryWrite write{context,actual};
        if (!host.writeIopMemoryBatch(std::span(&write,1))) return {};
        if (ordinal==70) rings.erase(found); else ring.bytes=actual;
        return {true,0};
    }
    NativeExportResult call(uint16_t ordinal,const std::array<uint32_t,4>& args) {
        std::lock_guard lock(mutex);
        if (!initialized()) return {};
        if (ordinal>=11 && ordinal<=14) {
            struct Busy { bool& flag; ~Busy() { flag=false; } } operation{busy}; busy=true;
            if (ordinal!=14) {
                auto size=args[0];
                if (ordinal==13) {
                    // Original448 queries sysmem7 and only rounds down when
                    // the desired size does not fit. The minimum is likewise
                    // checked only on that fallback path. args[3] is mandatory.
                    uint32_t previous=0;
                    if ((args[3]&3) || args[3]<0x04000000 || args[3]>0x04500000-4 ||
                        (uint64_t(args[3])<uint64_t(image)+ImageBytes && uint64_t(image)<uint64_t(args[3])+4) ||
                        !host.readIopMemory(args[3],&previous,4)) return {};
                    size=args[2];
                    if (!size || size>0x500000) return {};
                    const auto available=host.maxFreeIopMemory();
                    if (available<size) {
                        if (!args[1]) return {}; // Original reaches its divide-by-zero trap.
                        size=available-available%args[1];
                        if (size<args[0]) return {};
                    }
                }
                if (!size || size>0x500000) return {};
                const auto address=ordinal==12?host.allocateIopMemoryLast(size):host.allocateIopMemory(size);
                // Original340/364/448 retries failed allocation indefinitely after
                // DelayThread(1000). This synchronous subset cannot claim a
                // completed call on exhaustion, and never returns handled/null.
                if (!address) return {};
                const auto overlaps=[&](uint32_t at,uint32_t bytes) {
                    return uint64_t(address)<uint64_t(at)+bytes && uint64_t(at)<uint64_t(address)+size;
                };
                if (overlaps(image,ImageBytes) || (ordinal==13 && overlaps(args[3],4)) || std::any_of(allocations.begin(),allocations.end(),
                    [&](const auto& a) { return overlaps(a.first,a.second); })) return {};
                struct Pending {
                    IopHost& host; uint32_t address;
                    ~Pending() { if (address) host.freeIopMemory(address); }
                } pending{host,address};
                if ((address&3) || address<0x04000000 || uint64_t(address)+size>0x04500000) return {};
                // Read the whole requested extent, without modifying its data
                // or claiming that original sysmem returns zero-filled memory.
                std::vector<uint8_t> bytes(size);
                if (!host.readIopMemory(address,bytes.data(),bytes.size())) return {};
                // call() is entered with the recursive registry lock already
                // held; this recheck cannot invert registry -> state ordering.
                if (registry->bind("libpdi",0x10a,ordinal).registration!=registration) return {};
                allocations.emplace(address,size);
                // Own the allocation before committing the output. On failure
                // (including exceptions) roll back ownership and release it.
                struct Ownership {
                    std::map<uint32_t,uint32_t>& entries; uint32_t address;
                    ~Ownership() { if (address) entries.erase(address); }
                } ownership{allocations,address};
                if (ordinal==13) {
                    std::array<uint8_t,4> actual{}; put(actual,0,size);
                    const IopMemoryWrite write{args[3],actual};
                    if (!host.writeIopMemoryBatch(std::span(&write,1))) return {};
                }
                ownership.address=0; pending.address=0;
                return {true,int32_t(address)};
            }
            const auto found=allocations.find(args[0]);
            if (found==allocations.end()) return {};
            if (rings.contains(args[0]) || std::any_of(rings.begin(),rings.end(),[&](const auto& r) { return r.second.buffer==args[0]; })) return {};
            std::vector<uint8_t> bytes(found->second);
            if (!host.readIopMemory(found->first,bytes.data(),bytes.size())) return {};
            uint8_t remaining=0;
            try { host.freeIopMemory(found->first); }
            catch (...) {
                if (!host.readIopMemory(found->first,&remaining,1)) allocations.erase(found);
                throw;
            }
            if (host.readIopMemory(found->first,&remaining,1)) return {};
            allocations.erase(found);
            if (registry->bind("libpdi",0x10a,14).registration!=registration) return {};
            return {true,0}; // Original388 returns the successful sysmem free result.
        }
        if (ordinal==40 || (ordinal>=65 && ordinal<=82)) return ringCall(ordinal,args);
        if (ordinal==99) return {true,int32_t(image+0x26f0)};
        if (ordinal==96) {
            struct Busy { bool& flag; ~Busy() { flag=false; } } operation{busy}; busy=true;
            return {true,int32_t(uint32_t(seconds()))}; // Original discards microseconds/high seconds.
        }
        if (ordinal==98) {
            constexpr std::array<uint16_t,24> offsets{0,31,59,90,120,151,181,212,243,273,304,334,
                                                     0,31,60,91,121,152,182,213,244,274,305,335};
            std::array<uint16_t,24> actual{};
            if (!host.readIopMemory(image+0x2710,actual.data(),sizeof(actual)) || actual!=offsets) return {};
            const uint32_t year=args[0]%100,month=args[1],day=args[2];
            if (month<1 || month>12 || !day) return {};
            const auto base=(year%4)?0u:12u;
            const auto days=month==12?31u:uint32_t(offsets[base+month]-offsets[base+month-1]);
            if (day>days) return {};
            return {true,int32_t(year*365+(year+3)/4+offsets[base+month-1]+day-1)};
        }
        if (ordinal!=4) return {};
        const auto dest=args[0],source=args[1],size=args[2];
        if (!size) return {true,int32_t(dest)}; // Original zero-size copy never accesses memory.
        if (size>1024*1024 || uint64_t(dest)+size>UINT32_MAX || uint64_t(source)+size>UINT32_MAX ||
            (dest!=source && uint64_t(dest)<uint64_t(source)+size && uint64_t(source)<uint64_t(dest)+size) ||
            (uint64_t(dest)<uint64_t(image)+ImageBytes && uint64_t(image)<uint64_t(dest)+size)) return {};
        struct Busy { bool& flag; explicit Busy(bool& f):flag(f) { flag=true; } ~Busy() { flag=false; } } operation(busy);
        std::vector<uint8_t> bytes(size);
        if (!host.readIopMemory(source,bytes.data(),bytes.size())) return {};
        const IopMemoryWrite write{dest,bytes};
        if (!host.writeIopMemoryBatch(std::span(&write,1))) return {};
        return {true,int32_t(dest)};
    }
};
LibpdiResident::LibpdiResident(IopHost& host,SecondsSource seconds):state_(std::make_shared<State>(host,std::move(seconds))) {}
LibpdiResident::~LibpdiResident() { reset(); }
bool LibpdiResident::active() const {
    auto& s=*state_; const auto binding=s.registry?s.registry->bind("libpdi",0x10a,99):NativeImportBinding{};
    std::lock_guard lock(s.mutex);
    return binding.registration && binding.registration==s.registration && s.initialized();
}
bool LibpdiResident::start() {
    auto& s=*state_;
    { std::lock_guard lock(s.mutex); if (s.running || s.busy) return false; }
    std::lock_guard lifecycle(s.lifecycle);
    { std::lock_guard lock(s.mutex); if (s.running || s.busy || !s.registry) return false; s.busy=true; }
    struct Transaction {
        State& s; uint32_t image=0; uint64_t registration=0;
        ~Transaction() {
            if (registration) s.registry->unregisterLibrary(registration);
            if (image) s.host.freeIopMemory(image);
            std::lock_guard lock(s.mutex); s.busy=false;
        }
    } transaction{s};
    auto bytes=verifiedImage(s.host); if (bytes.empty()) return false;
    transaction.image=s.host.allocateIopMemory(ImageBytes);
    if (!transaction.image || transaction.image>UINT32_MAX-ImageBytes) return false;
    put(bytes,0x2700,transaction.image+0x26e0); put(bytes,0x2708,1);
    const IopMemoryWrite write{transaction.image,bytes};
    if (!s.host.writeIopMemoryBatch(std::span(&write,1))) return false;
    std::weak_ptr<State> weak=state_; NativeLibrary library{"libpdi",0x10a,{}};
    for (uint16_t ordinal:{4,11,12,13,14,40,65,66,67,68,70,73,74,75,76,79,80,81,82,96,98,99}) library.exports.push_back({ordinal,[weak,ordinal](const auto& args) {
        const auto state=weak.lock(); return state?state->call(ordinal,args):NativeExportResult{};
    }});
    transaction.registration=s.registry->registerLibrary(std::move(library));
    if (!transaction.registration) return false;
    std::lock_guard lock(s.mutex);
    std::array<uint32_t,3> values{};
    if (!s.host.readIopMemory(transaction.image+0x2700,values.data(),sizeof(values)) ||
        values!=std::array<uint32_t,3>{transaction.image+0x26e0,0x10a,1}) return false;
    s.image=transaction.image; s.registration=transaction.registration; s.running=true;
    transaction.image=0; transaction.registration=0; return true;
}
bool LibpdiResident::reset() {
    auto& s=*state_;
    { std::lock_guard lock(s.mutex); if (s.busy) return false; }
    std::lock_guard lifecycle(s.lifecycle);
    uint32_t image=0; uint64_t registration=0;
    std::map<uint32_t,uint32_t> allocations;
    { std::lock_guard lock(s.mutex);
      if (s.busy) return false;
      s.running=false; s.busy=true; image=s.image; registration=s.registration; s.image=0; s.registration=0;
      s.rings.clear(); allocations=std::move(s.allocations); s.allocations.clear();
    }
    struct Finish { State& s; ~Finish() { std::lock_guard lock(s.mutex); s.busy=false; } } finish{s};
    if (registration) s.registry->unregisterLibrary(registration);
    for (const auto& [address,size]:allocations) s.host.freeIopMemory(address);
    if (image) s.host.freeIopMemory(image);
    return true;
}
}
