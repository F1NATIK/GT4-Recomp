#include "ps2x/iop/gt3/usbd_resident.h"
#include "ps2x/iop/native_library.h"
#include <algorithm>

namespace ps2x::iop::gt3 {
namespace {
uint32_t word(std::span<const uint8_t> b,size_t at=0) { return uint32_t(b[at])|(uint32_t(b[at+1])<<8)|(uint32_t(b[at+2])<<16)|(uint32_t(b[at+3])<<24); }
void put(std::span<uint8_t> b,size_t at,uint32_t v) { for (unsigned i=0;i<4;++i) b[at+i]=uint8_t(v>>(i*8)); }
struct Busy {
    std::recursive_mutex& mutex; bool& flag;
    Busy(std::recursive_mutex& m,bool& f):mutex(m),flag(f) { flag=true; }
    ~Busy() { std::lock_guard lock(mutex); flag=false; }
};
}
struct UsbdResident::State {
    struct Driver { uint32_t address; std::array<uint8_t,48> bytes; std::array<NativeCallbackBinding,3> callbacks; };
    IopHost& host; NativeLibraryRegistry* registry; UsbdWorkers workers;
    mutable std::recursive_mutex mutex,lifecycle;
    uint64_t registration=0; uint32_t image=0,arena=0;
    bool running=false,busy=false,failed=false;
    std::vector<Driver> drivers;
    explicit State(IopHost& h):host(h),registry(h.nativeLibraries()),workers(h) {}
    bool read(uint32_t at,std::span<uint8_t> b) const { return host.readIopMemory(at,b.data(),b.size()); }
    bool emptyBus() const {
        if (!workers.snapshot().active || !image || !arena) return false;
        for (uint32_t i=0;i<32;++i) {
            std::array<uint8_t,0x98> device{};
            if (!read(arena+0x34ac+i*0x98,device) || device[0x18]!=(i<3?1:0) ||
                word(device,0x24)!=(i==1 || i==2?arena+0x34ac:0) || word(device,0xc) || word(device,0x10) || word(device,0x14) ||
                word(device,0x8c)!=arena+0x8bac+i*512 || word(device,0x90)) return false;
            std::array<uint8_t,1> descriptor{};
            if (!read(word(device,0x8c),descriptor) || descriptor[0]) return false;
        }
        return true;
    }
    bool validDrivers() const {
        std::array<uint8_t,8> ends{};
        if (!read(image+0x58b0,ends) || word(ends)!=(drivers.empty()?0:drivers.front().address) ||
            word(ends,4)!=(drivers.empty()?0:drivers.back().address)) return false;
        for (const auto& driver:drivers) {
            std::array<uint8_t,48> bytes{};
            if (!read(driver.address,bytes) || bytes!=driver.bytes) return false;
        }
        return true;
    }
    // Only called from registry dispatch, which already holds registry before
    // state. Do not call this while holding state from lifecycle operations.
    bool callbacksLive() const {
        for (const auto& driver:drivers) for (size_t i=0;i<3;++i) {
            const auto address=word(driver.bytes,0xc+i*4);
            if (!address) continue;
            const auto now=registry->bindCallback(address),saved=driver.callbacks[i];
            if (!now.callback || now.callback!=saved.callback || now.registration!=saved.registration) return false;
        }
        return true;
    }
    NativeExportResult catalog(uint32_t address,bool remove,uint32_t gp=0,
                               const std::array<NativeCallbackBinding,3>* bindings=nullptr) {
        if (!emptyBus() || !validDrivers()) return {};
        auto updated=drivers;
        const auto found=std::find_if(updated.begin(),updated.end(),[&](const auto& d) { return d.address==address; });
        if (remove) {
            if (found==updated.end()) return {true,0x104};
            if (address==image+0x5788) return {}; // Owned hub cannot be removed independently.
            // Original export5/4acc unlinks neighbors, but preserves the
            // removed descriptor's links/GP. The client owns that memory.
            updated.erase(found);
        } else {
            if (found!=updated.end()) return {true,drivers.size()==1?0x121:0x104};
            if ((address&3) || updated.size()>=64 || address>UINT32_MAX-48) return {};
            Driver d{address,{},{}};
            if (!read(address,d.bytes)) return {};
            if (word(d.bytes) || word(d.bytes,4) || !word(d.bytes,8) || word(d.bytes,0x18) || word(d.bytes,0x1c)) return {true,0x104};
            const auto name=word(d.bytes,8); bool terminated=false;
            if (name>UINT32_MAX-128) return {};
            for (uint32_t i=0;i<128;++i) {
                std::array<uint8_t,1> c{}; if (!read(name+i,c)) return {};
                if (!c[0]) { terminated=true; break; }
            }
            if (!terminated) return {};
            for (size_t i=0;i<3;++i) {
                const auto callback=word(d.bytes,0xc+i*4);
                if (!bindings) return {};
                d.callbacks[i]=(*bindings)[i];
                std::array<uint8_t,4> codeAddress{};
                if (callback && (!d.callbacks[i].callback || !d.callbacks[i].registration || !read(callback,codeAddress))) return {};
            }
            put(d.bytes,0x2c,gp); updated.push_back(d);
        }
        for (size_t i=0;i<updated.size();++i) {
            put(updated[i].bytes,0,i+1<updated.size()?updated[i+1].address:0);
            put(updated[i].bytes,4,i?updated[i-1].address:0);
        }
        std::array<uint8_t,8> ends{}; put(ends,0,updated.front().address); put(ends,4,updated.back().address);
        std::vector<IopMemoryWrite> writes;
        for (const auto& d:updated) writes.push_back({d.address,d.bytes});
        writes.push_back({image+0x58b0,ends});
        try {
            if (!host.writeIopMemoryBatch(writes)) return {};
            for (const auto& write:writes) {
                std::vector<uint8_t> actual(write.bytes.size());
                if (!read(write.address,actual) || !std::equal(actual.begin(),actual.end(),write.bytes.begin())) {
                    failed=true; return {};
                }
            }
        } catch (...) { failed=true; throw; }
        drivers=std::move(updated); return {true,0};
    }
    NativeExportResult registerDriver(const std::array<uint32_t,4>& args,NativeCallContext context) {
        std::lock_guard lock(mutex);
        if (!running || busy || failed || !emptyBus() || !validDrivers() || !callbacksLive()) return {};
        Busy operation(mutex,busy);
        const auto address=args[0];
        if ((address&3) || address>UINT32_MAX-48) return {};
        std::array<uint8_t,48> bytes{}; if (!read(address,bytes)) return {};
        std::array<NativeCallbackBinding,3> bindings{};
        for (size_t i=0;i<3;++i) {
            const auto callback=word(bytes,0xc+i*4);
            if (callback) bindings[i]=registry->bindCallback(callback);
        }
        const auto result=catalog(address,false,context.gp,&bindings);
        if (result.handled && result.value==0 &&
            (!callbacksLive() || registry->bind("usbd",0x101,6).registration!=registration)) {
            failed=true; return {};
        }
        return result;
    }
    NativeExportResult call(uint16_t ordinal,const std::array<uint32_t,4>& args) {
        std::lock_guard lock(mutex);
        if (!running || busy || failed || !emptyBus() || !validDrivers()) return {};
        // An empty-bus removal must remain possible after its callback owner
        // unloads. No callback is invoked in this audited disconnected state.
        if (ordinal!=5 && !callbacksLive()) return {};
        Busy operation(mutex,busy);
        switch (ordinal) {
        case 5: return catalog(args[0],true);
        case 6: return {true,0}; // No configured device descriptor (requires state8).
        case 7: {
            if (args[0]!=1 && args[0]!=2) return {true,0x101};
            std::array<uint8_t,4> value{}; put(value,0,args[1]);
            const IopMemoryWrite write{arena+0x34ac+args[0]*0x98+0x2c,value};
            return host.writeIopMemoryBatch(std::span(&write,1))?NativeExportResult{true,0}:NativeExportResult{};
        }
        case 8: {
            if (args[0]!=1 && args[0]!=2) return {true,0};
            std::array<uint8_t,4> value{};
            return read(arena+0x34ac+args[0]*0x98+0x2c,value)?NativeExportResult{true,int32_t(word(value))}:NativeExportResult{};
        }
        case 9: case 12:
            if (args[0]!=1 && args[0]!=2) return {true,-1};
            return args[1]?NativeExportResult{}:NativeExportResult{true,-1}; // No default pipe; allocation unsupported.
        case 10: case 11: {
            // No endpoint record has a device owner in this native bus. A
            // corrupted/externally activated record must not receive an error stub.
            if (args[0]<64) {
                std::array<uint8_t,4> device{};
                if (!read(arena+0x21ac+args[0]*0x4c+0x18,device) || word(device)) return {};
            }
            return {true,0x102};
        }
        case 13: return {true,0x101}; // Location requires configured state8, absent here.
        case 16: return args[0]==30 && args[1]==36?NativeExportResult{true,0}:NativeExportResult{};
        default: return {};
        }
    }
};
UsbdResident::UsbdResident(IopHost& host):state_(std::make_shared<State>(host)) {}
UsbdResident::~UsbdResident() { reset(); }
bool UsbdResident::active() const {
    auto& s=*state_;
    const auto binding=s.registry?s.registry->bind("usbd",0x101,6):NativeImportBinding{};
    // Pure absent-descriptor query validates callback lifetimes under the
    // registry lock; lifecycle never acquires registry while holding state.
    if (!binding.registration || !s.registry->invoke(binding,{}).handled) return false;
    std::lock_guard lock(s.mutex);
    return s.running && !s.busy && !s.failed && binding.registration==s.registration && s.emptyBus() && s.validDrivers();
}
bool UsbdResident::start(bool realtime) {
    auto& s=*state_; std::lock_guard lifecycle(s.lifecycle);
    std::unique_lock lock(s.mutex);
    if (s.running || s.busy || !s.registry) return false;
    bool success=false;
    try {
        { Busy operation(s.mutex,s.busy);
          if (s.workers.start(realtime)) {
            const auto a=s.workers.snapshot().addresses; s.image=a[0]; s.arena=a[1];
            std::array<uint8_t,48> hub{}; const std::array<uint8_t,4> name{'h','u','b',0};
            put(hub,8,s.image+0x56f4); put(hub,0xc,s.image+0xb58); put(hub,0x10,s.image+0x91c); put(hub,0x14,s.image+0xb18);
            const std::array<IopMemoryWrite,2> writes{{{s.image+0x5788,hub},{s.image+0x56f4,name}}};
            if (s.host.writeIopMemoryBatch(writes)) {
                std::weak_ptr<State> weak=state_; NativeLibrary library{"usbd",0x101,{}};
                library.exports.push_back({4,{},[weak](const auto& args,NativeCallContext context) {
                    const auto state=weak.lock(); return state?state->registerDriver(args,context):NativeExportResult{};
                }});
                for (uint16_t ordinal:{5,6,7,8,9,10,11,12,13,16}) library.exports.push_back({ordinal,[weak,ordinal](const auto& args) {
                    const auto state=weak.lock(); return state?state->call(ordinal,args):NativeExportResult{};
                }});
                // Registry dispatch takes its lock before this state lock.
                // Never publish/bind under the opposite lock order. The busy
                // guard rejects calls until the entire entry has committed.
                lock.unlock();
                const auto registration=s.registry->registerLibrary(std::move(library));
                lock.lock(); s.registration=registration; lock.unlock();
                if (s.registration) {
                    bool callbacks=true;
                    for (const auto [offset,kind]:std::array<std::pair<uint32_t,unsigned>,3>{{{0xb58,0},{0x91c,1},{0xb18,2}}})
                        callbacks&=s.registry->registerCallback(s.registration,s.image+offset,[weak,kind](const auto& args) {
                            const auto state=weak.lock(); if (!state) return NativeExportResult{};
                            std::lock_guard guard(state->mutex);
                            if (!state->running || state->busy || state->failed || !state->emptyBus()) return NativeExportResult{};
                            if (kind==0) return NativeExportResult{true,0}; // Empty descriptor scan.
                            if (kind==1) return NativeExportResult{true,-1}; // No configuration to attach.
                            if (args[0]!=1 && args[0]!=2) return NativeExportResult{true,-1};
                            // Original helper0xbc unconditionally writes through
                            // the private hub pointer, even null. There is no
                            // successful no-op detach for these reserved nodes.
                            return NativeExportResult{};
                        })!=0;
                    std::array<NativeCallbackBinding,3> bindings{};
                    for (size_t i=0;i<3;++i) bindings[i]=s.registry->bindCallback(s.image+std::array<uint32_t,3>{0xb58,0x91c,0xb18}[i]);
                    lock.lock();
                    if (callbacks) {
                        const auto registered=s.catalog(s.image+0x5788,false,s.image+0xd8b0,&bindings);
                        success=registered.handled && registered.value==0 && s.workers.snapshot().active;
                    }
                } else lock.lock();
            }
          }
        }
        s.running=success; lock.unlock();
        if (!success) reset();
        return success;
    } catch (...) { if (lock.owns_lock()) lock.unlock(); reset(); throw; }
}
bool UsbdResident::reset() {
    auto& s=*state_; std::lock_guard lifecycle(s.lifecycle);
    uint64_t registration=0;
    { std::lock_guard lock(s.mutex); if (s.busy) return false; s.running=false; registration=s.registration; }
    if (registration) s.registry->unregisterLibrary(registration);
    if (!s.workers.reset()) return false;
    std::lock_guard lock(s.mutex); s.registration=0; s.image=s.arena=0; s.drivers.clear(); s.failed=false; return true;
}
}
