#include "ps2x/iop/gt3/mcserv_resident.h"
#include "ps2x/iop/native_library.h"
#include "ps2x/iop/gt3/mcman_native_exports.h"
#include <cstring>
#include <condition_variable>
#include <future>
#include <mutex>
#include <thread>
#include <optional>
#include <algorithm>
#include <cstdlib>

namespace ps2x::iop::gt3 {
namespace {
void put(std::span<uint8_t> bytes,size_t offset,uint32_t value) {
    for (unsigned i=0;i<4;++i) bytes[offset+i]=uint8_t(value>>(i*8));
}
uint32_t word(std::span<const uint8_t> bytes,size_t offset) {
    return uint32_t(bytes[offset])|(uint32_t(bytes[offset+1])<<8)|(uint32_t(bytes[offset+2])<<16)|(uint32_t(bytes[offset+3])<<24);
}
bool verified(IopHost& host) {
    const auto file=host.openHostFile(host.translateGuestPath("cdrom0:\\MCSERV.IRX;1")); if (!file) return false;
    struct Close { IopHost& host; uint64_t file; ~Close() { host.closeHostFile(file); } } close{host,file};
    uint64_t size=0; size_t read=0; std::array<uint8_t,32> digest{};
    if (!host.hostFileSize(file,size) || size<52 || size>1024*1024) return false;
    std::vector<uint8_t> bytes(size);
    if (!host.readHostFile(file,0,bytes.data(),bytes.size(),read) || read!=bytes.size() || !host.sha256(bytes,digest)) return false;
    constexpr std::string_view hash="6957f60bd5aa8500bcfa1a7cffb30bcf1b1d8daf03f86f7826e8f8a340834356";
    constexpr char hex[]="0123456789abcdef"; std::string actual;
    for (auto b:digest) { actual+=hex[b>>4]; actual+=hex[b&15]; }
    return actual==hash;
}
}
struct McservResident::State {
    IopHost& host; NativeLibraryRegistry* registry;
    mutable std::recursive_mutex lifecycle;
    std::mutex mutex; std::condition_variable_any changed;
    std::jthread worker;
    uint32_t image=0,stack=0,semaphore=0;
    uint64_t registration=0;
    bool running=false,startupHeld=false;
    unsigned tracedRejections=0;
    NativeImportBinding getSemaphore,signal,wait;
    struct Reply {
        bool handled=false; std::vector<uint8_t> bytes; std::vector<uint8_t> data; uint32_t destination=0;
        std::vector<std::pair<uint32_t,std::vector<uint8_t>>> dma; // Further EE destinations.
    };
    struct Job { uint32_t function; std::vector<uint8_t> input; std::promise<Reply> result; };
    std::optional<Job> pending;
    McmanNativeExports* cards=nullptr;
    explicit State(IopHost& h,McmanNativeExports* c):host(h),registry(h.nativeLibraries()),cards(c) {}
    NativeExportResult call(uint16_t ordinal,const std::array<uint32_t,4>& args={}) {
        return registry->invoke(registry->bind("mcman",0x206,ordinal),args);
    }
    // Card-backed functions (original request layouts, handlers c30..e6c);
    // nullopt: the addressed port has no card. EE descriptors are
    // port*MaxHandles + the card's descriptor.
    std::optional<int32_t> card(uint32_t function,const std::vector<uint8_t>& input,Reply& reply) {
        if (!cards) return std::nullopt;
        auto w=[&](size_t offset) { return offset+4<=input.size()?word(input,offset):0u; };
        auto name=[&] {
            if (input.size()<=0x14) return std::string();
            const auto* p=reinterpret_cast<const char*>(input.data()+0x14);
            return std::string(p,strnlen(p,std::min<size_t>(1024,input.size()-0x14)));
        };
        auto byPort=[&](uint32_t port,uint32_t slot)->McCard* { return port<2 && !slot?cards->card(port):nullptr; };
        int32_t fd=0;
        auto byFd=[&](uint32_t descriptor)->McCard* {
            if (int32_t(descriptor)<0) return nullptr;
            fd=int32_t(descriptor)%McCard::MaxHandles;
            return byPort(descriptor/McCard::MaxHandles,0);
        };
        switch (function) {
        case 1: { // c30: +4 port +8 slot; +c/+10/+14 want format/free/type; 0xc0 bytes to +1c.
            auto* c=byPort(w(4),w(8)); if (!c) return std::nullopt;
            std::vector<uint8_t> info(0xc0);
            const auto detected=c->detect();
            if (w(0x14)) put(info,0,2); // PS2 card
            if (w(0x10)) put(info,4,uint32_t(c->freeClusters()));
            if (w(0xc)) put(info,0x90,c->formatted()>0);
            reply.dma.push_back({w(0x1c),std::move(info)});
            return detected;
        }
        case 2: { // 5cc: port, slot, name, flags (0x40: make directory).
            auto* c=byPort(w(0),w(4)); if (!c) return std::nullopt;
            const auto result=c->open(name(),w(8));
            return result<0 || (w(8)&0x40)?result:int32_t(w(0)*McCard::MaxHandles)+result;
        }
        case 3: { auto* c=byFd(w(0)); if (!c) return std::nullopt; return c->close(fd); }
        case 4: { auto* c=byFd(w(0)); if (!c) return std::nullopt; return c->seek(fd,int32_t(w(0x10)),int32_t(w(0x14))); }
        case 0xa: { auto* c=byFd(w(0)); if (!c) return std::nullopt; return c->flush(fd); }
        case 5: { // 650: aligned middle DMA'd to +18; unaligned head/tail in the 0xc0-byte block at +1c.
            auto* c=byFd(w(0)); if (!c) return std::nullopt;
            const int32_t size=int32_t(w(0xc)); uint32_t destination=w(0x18);
            const int32_t head=size<=0x40?size:int32_t((64-(destination&63))&63);
            int32_t middle=size-head; const int32_t tail=middle&0x3f; middle-=tail;
            std::vector<uint8_t> block(0xc0);
            put(block,0,uint32_t(head)); put(block,4,uint32_t(tail));
            if (tail) put(block,0xc,destination+uint32_t(head+middle));
            int32_t total=0,error=0;
            [&] {
                if (head>0) {
                    std::vector<uint8_t> data(head);
                    const auto r=c->read(fd,data);
                    if (r<0) { put(block,0,0); put(block,4,0); error=r; return; }
                    put(block,8,destination); std::copy_n(data.begin(),r,block.begin()+0x10);
                    total=r; destination+=uint32_t(r);
                    if (r!=head) { put(block,0,uint32_t(r)); put(block,4,0); return; }
                }
                while (middle>0) {
                    const auto chunk=std::min(middle,0x2000);
                    std::vector<uint8_t> data(chunk);
                    const auto r=c->read(fd,data);
                    if (r<0) { put(block,4,0); error=r; return; }
                    if (r!=chunk) {
                        const auto rest=r&0x3f; put(block,4,uint32_t(rest));
                        if (rest) {
                            put(block,0xc,destination+uint32_t(r&~0x3f));
                            std::copy_n(data.begin()+(r&~0x3f),rest,block.begin()+0x50);
                        }
                        if (r!=rest) reply.dma.push_back({destination,std::vector<uint8_t>(data.begin(),data.begin()+(r-rest))});
                        total+=r; return;
                    }
                    reply.dma.push_back({destination,std::move(data)});
                    total+=r; middle-=r; destination+=uint32_t(r);
                }
                if (tail) {
                    std::vector<uint8_t> data(tail);
                    const auto r=c->read(fd,data);
                    if (r<0) { put(block,4,0); error=r; return; }
                    total+=r; put(block,4,uint32_t(r)); std::copy_n(data.begin(),r,block.begin()+0x50);
                }
            }();
            reply.dma.push_back({w(0x1c),std::move(block)});
            return error?error:total;
        }
        case 6: { // 8d4: head bytes inline at +20 (+14 count), then +c bytes from EE +18.
            auto* c=byFd(w(0)); if (!c) return std::nullopt;
            const auto head=int32_t(w(0x14));
            if (head<0 || 0x20+size_t(head)>input.size()) return std::nullopt;
            int32_t total=0;
            if (head) {
                total=c->write(fd,std::span<const uint8_t>(input.data()+0x20,size_t(head)));
                if (total!=head) return total;
            }
            uint32_t size=w(0xc),source=w(0x18);
            while (size) {
                const auto chunk=std::min<uint32_t>(size,0x2000);
                std::vector<uint8_t> data(chunk);
                if (!host.readGuest(source,data.data(),data.size())) return std::nullopt;
                const auto r=c->write(fd,data);
                if (r!=int32_t(chunk)) return r<0?r:r+total;
                total+=r; size-=chunk; source+=chunk;
            }
            return total;
        }
        case 0xc: { // b38: port, slot, name; current directory (0x400 bytes) to +10.
            auto* c=byPort(w(0),w(4)); if (!c) return std::nullopt;
            std::string current; const auto result=c->chDir(name(),current);
            std::vector<uint8_t> path(0x400);
            std::copy_n(current.begin(),std::min<size_t>(current.size(),0x3ff),path.begin());
            reply.dma.push_back({w(0x10),std::move(path)});
            return result;
        }
        case 0xd: { // 9e0: port, slot, flags (0: new listing), max entries, 64-byte table at +10.
            auto* c=byPort(w(0),w(4)); if (!c) return std::nullopt;
            const auto maximum=int32_t(w(0xc));
            if (maximum<0) return c->countDir(name());
            std::vector<uint8_t> table; int32_t count=0,result=0; bool start=(w(8)&0xffff)==0;
            for (;count<maximum;++count) {
                McDirInfo entry;
                result=c->getDir(name(),start,entry); start=false;
                if (result<=0) break;
                const auto bytes=entry.table(); table.insert(table.end(),bytes.begin(),bytes.end());
            }
            if (!table.empty()) reply.dma.push_back({w(0x10),std::move(table)});
            return result<0?result:count;
        }
        case 0xe: { // e08: port, slot, flags, 64-byte table from EE +10, name.
            auto* c=byPort(w(0),w(4)); if (!c) return std::nullopt;
            std::array<uint8_t,64> table{};
            if (!host.readGuest(w(0x10),table.data(),table.size())) return std::nullopt;
            return c->setFileInfo(name(),McDirInfo::fromTable(table),w(8));
        }
        case 0xf: { auto* c=byPort(w(0),w(4)); if (!c) return std::nullopt; return c->remove(name()); }
        case 0x10: { auto* c=byPort(w(4),w(8)); if (!c) return std::nullopt; return c->format(); }
        case 0x11: { auto* c=byPort(w(4),w(8)); if (!c) return std::nullopt; return c->unformat(); }
        case 0x12: { auto* c=byPort(w(0),w(4)); if (!c) return std::nullopt; return c->entSpace(name()); }
        case 0x33: { auto* c=byPort(w(4),w(8)); if (!c) return std::nullopt; return 0; } // Block check: no bad blocks.
        default: return std::nullopt;
        }
    }
    Reply execute(uint32_t function,const std::vector<uint8_t>& input) {
        const auto fail=[&](const char* reason)->Reply {
            if (std::getenv("GT3_BOOT_TRACE") && tracedRejections++<8)
                host.log(LogLevel::Warning,std::string("[GT3/MCSERV] rejected: ")+reason);
            return {};
        };
        // Every job depends on the exact startup semaphore binding; replacement
        // MCMAN instances cannot silently inherit this worker's ownership.
        const auto current=registry->invoke(getSemaphore,{});
        if (!current.handled || uint32_t(current.value)!=semaphore) return fail("startup semaphore changed");
        const std::array<IopMemoryWrite,1> request{{{image+0x3190,input}}};
        if (!host.writeIopMemoryBatch(request)) return fail("request staging failed");
        auto w=[&](size_t offset) { return word(input,offset); };
        Reply reply; NativeExportResult result;
        if (function==0x14) { // 56c: ChangeThreadPriority(0, +14) for 9..123, else -403.
            const auto priority=w(0x14); result={true,priority>=9 && priority<124?0:-403};
        } else if (const auto native=card(function,input,reply)) {
            result={true,*native};
            if (std::getenv("GT3_MC_TRACE"))
            {
                // Words 0..3 and the name field (functions with a path at +14).
                std::string detail;
                for (size_t at=0;at<16 && at+4<=input.size();at+=4) detail+=" "+std::to_string(int32_t(w(at)));
                if (input.size()>0x14 && function!=5 && function!=6 && function!=1)
                    detail+=" name="+std::string(reinterpret_cast<const char*>(input.data()+0x14),
                                                 strnlen(reinterpret_cast<const char*>(input.data()+0x14),input.size()-0x14));
                host.log(LogLevel::Info,"[GT3/MCSERV] card RPC="+std::to_string(function)+detail+" result="+std::to_string(*native));
            }
        } else switch (function) {
        case 0xfe: {
            result=call(37,{w(0),0,0,0});
            const auto identity=call(42); std::array<uint8_t,12> info{};
            if (!result.handled) return fail("MCMAN37 init unavailable");
            if (!identity.handled) return fail("MCMAN42 identity unavailable");
            if (!host.readIopMemory(uint32_t(identity.value),info.data(),info.size())) return fail("MCMAN identity read failed");
            reply.bytes.resize(12); put(reply.bytes,4,0x20d); put(reply.bytes,8,word(info,4)&0xffff); break;
        }
        case 1: {
            result=call(21,{w(4),w(8),0,0});
            if (!result.handled || result.value!=-11) return {}; // This resident supports explicit absence only.
            reply.data.resize(0xc0); reply.destination=w(0x1c);
            if (w(0x14)) {
                const auto type=call(39,{w(4),w(8),0,0}); if (!type.handled) return {};
                put(reply.data,0,uint32_t(type.value));
            }
            // Original0xc88/0xc90 writes free=0 and format=0 after detect<-1.
            // Unspecified stack bytes are initialized to zero in the native model.
            break;
        }
        case 2: result=call(6,{w(0),w(4),image+0x31a4,w(8)}); break;
        case 3: result=call(7,{w(0),0,0,0}); break;
        case 4: result=call(10,{w(0),w(0x10),w(0x14),0}); break;
        case 0xa: result=call(14,{w(0),0,0,0}); break;
        case 0xf: result=call(13,{w(0),w(4),image+0x31a4,w(8)}); break;
        case 0x10: result=call(11,{w(4),w(8),0,0}); break;
        case 0x11: result=call(36,{w(4),w(8),0,0}); break;
        case 0x12: result=call(23,{w(0),w(4),image+0x31a4,0}); break;
        case 0x15: result=call(26,{w(4),0,0,0}); break;
        default: return {}; // Read/write/getdir/chdir/setinfo/block/priority still unsupported.
        }
        if (!result.handled) return {};
        if (reply.bytes.empty()) reply.bytes.resize(4);
        put(reply.bytes,0,uint32_t(result.value));
        const auto replacement=call(24); if (!replacement.handled) return fail("MCMAN24 completion unavailable");
        // Original dispatch tail always signals, ignoring an already-full error.
        registry->invoke(signal,{semaphore,0,0,0}); startupHeld=false;
        reply.handled=true; return reply;
    }
};
McservResident::McservResident(IopHost& h,McmanNativeExports* cards):state_(std::make_unique<State>(h,cards)) {}
McservResident::~McservResident() { reset(); }
bool McservResident::active() const { std::lock_guard lock(state_->lifecycle); return state_->running; }
bool McservResident::start() {
    auto& s=*state_; std::lock_guard lock(s.lifecycle);
    if (!s.registry || s.running || !verified(s.host)) return false;
    s.getSemaphore=s.registry->bind("mcman",0x206,53);
    s.wait=s.registry->bind("thsemap",0x101,8); s.signal=s.registry->bind("thsemap",0x101,6);
    for (uint16_t ordinal:{6,7,10,11,13,14,21,23,24,26,36,37,39,42,53})
        if (!s.registry->bind("mcman",0x206,ordinal).registration) return false;
    if (!s.wait.registration || !s.signal.registration) return false;
    try {
        s.image=s.host.allocateIopMemory(0x3700); s.stack=s.host.allocateIopMemory(0x1000);
        if (!s.image || !s.stack) { reset(); return false; }
        std::vector<uint8_t> image(0x3700),stack(0x1000);
        // Owned native metadata preserves original startup requirements; host
        // priority is not claimed to emulate the IOP's priority104 scheduler.
        put(image,0x3600,0x02000000); put(image,0x3604,104); put(image,0x3608,s.stack); put(image,0x360c,0x1000);
        const std::array<IopMemoryWrite,2> writes{{{s.image,image},{s.stack,stack}}};
        if (!s.host.writeIopMemoryBatch(writes)) { reset(); return false; }
        s.registration=s.registry->registerLibrary({"mcserv",0x201,{{5,[](const auto& args) {
            return args[0]<9 || args[0]>123?NativeExportResult{true,-403}:NativeExportResult{};
        }}}});
        if (!s.registration) { reset(); return false; }
        std::promise<bool> initialized; auto ready=initialized.get_future();
        s.worker=std::jthread([&s,initialized=std::move(initialized)](std::stop_token stop) mutable {
            try {
                const auto semaphore=s.registry->invoke(s.getSemaphore,{});
                if (!semaphore.handled || semaphore.value<=0 || !s.registry->invoke(s.wait,{uint32_t(semaphore.value),0,0,0}).handled) {
                    initialized.set_value(false); return;
                }
                s.semaphore=uint32_t(semaphore.value); s.startupHeld=true; initialized.set_value(true);
                std::unique_lock lock(s.mutex);
                while (s.changed.wait(lock,stop,[&] { return s.pending.has_value(); })) {
                    auto job=std::move(*s.pending); s.pending.reset(); lock.unlock();
                    try { job.result.set_value(s.execute(job.function,job.input)); }
                    catch (...) { job.result.set_exception(std::current_exception()); }
                    lock.lock();
                }
            } catch (...) { try { initialized.set_exception(std::current_exception()); } catch (...) {} }
        });
        if (!ready.get()) { reset(); return false; }
        s.running=true; return true;
    } catch (...) { reset(); throw; }
}
void McservResident::reset() {
    auto& s=*state_; std::lock_guard lock(s.lifecycle);
    s.running=false;
    if (s.worker.joinable()) { s.worker.request_stop(); s.changed.notify_all(); s.worker.join(); }
    if (s.startupHeld) s.registry->invoke(s.signal,{s.semaphore,0,0,0});
    if (s.registration) s.registry->unregisterLibrary(s.registration);
    for (auto address:{s.image,s.stack}) if (address) s.host.freeIopMemory(address);
    s.image=s.stack=s.semaphore=0; s.registration=0; s.startupHeld=false; s.pending.reset();
}
RpcResult McservResident::handleRpc(const RpcRequest& request) {
    auto& s=*state_; std::lock_guard lock(s.lifecycle);
    const auto fail=[&](const char* reason)->RpcResult {
        if (std::getenv("GT3_BOOT_TRACE") && s.tracedRejections++<8)
            s.host.log(LogLevel::Warning,std::string("[GT3/MCSERV] transport rejected: ")+reason);
        return {};
    };
    if (!s.running) return fail("resident inactive");
    if (request.sid!=0x80000400) return fail("wrong SID");
    if (!request.send.address) return fail("missing send address");
    if (!request.receive.address) return fail("missing receive address");
    uint32_t minimum=0;
    switch (request.function) {
    case 0xfe: case 3: case 0xa: minimum=4; break;
    case 0x15: minimum=8; break;
    case 0x10: case 0x11: minimum=12; break;
    case 4: minimum=24; break;
    case 1: minimum=32; break;
    case 2: case 0xf: case 0x12: minimum=1044; break;
    case 5: case 6: minimum=0x20; break;
    case 0xc: case 0xd: case 0xe: minimum=1044; break;
    case 0x14: minimum=0x18; break;
    case 0x33: minimum=0x14; break;
    default: return {};
    }
    if (request.send.size<minimum || request.receive.size<(request.function==0xfe?12:4)) return fail("short buffers");
    // Write carries up to 64 unaligned head bytes inline after its 0x20-byte header.
    const auto length=request.function==6?std::max<uint32_t>(minimum,std::min<uint32_t>(request.send.size,0x60)):minimum;
    State::Job job{request.function,std::vector<uint8_t>(length),{}};
    if (!s.host.readGuest(request.send.address,job.input.data(),job.input.size())) return fail("EE input read failed");
    auto future=job.result.get_future();
    { std::lock_guard lock(s.mutex); s.pending=std::move(job); }
    s.changed.notify_all(); const auto reply=future.get(); if (!reply.handled) return fail("worker rejected");
    std::vector<IopMemoryWrite> writes{{request.receive.address,reply.bytes},{s.image+0x1180,reply.bytes}};
    if (!reply.data.empty()) { if (!reply.destination) return {}; writes.push_back({reply.destination,reply.data}); }
    for (const auto& [destination,data]:reply.dma) { if (!destination) return {}; writes.push_back({destination,data}); }
    if (!s.host.commitIopAndEeMemory(writes)) return fail("combined reply commit failed");
    RpcResult result; result.handled=true; result.resultAddress=request.receive.address; return result;
}
}
