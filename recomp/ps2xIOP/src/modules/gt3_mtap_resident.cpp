#include "ps2x/iop/gt3/mtap_resident.h"
#include <condition_variable>
#include <mutex>
#include <thread>

namespace ps2x::iop::gt3 {
struct MtapResident::State {
    IopHost& host; NativeLibraryRegistry* registry;
    std::shared_ptr<Sio2Device> bus;
    std::recursive_mutex mutex; std::condition_variable_any changed;
    std::jthread worker;
    bool active=false,pending=false;
    std::array<bool,4> opened{};
    std::array<bool,4> installed{},validatedMissing{};
    uint32_t image=0,stack=0,packet=0;
    uint64_t registration=0;
    State(IopHost& h,std::shared_ptr<Sio2Device> b):host(h),registry(h.nativeLibraries()),bus(std::move(b)) {}
    static Sio2Transfer probe(uint32_t port) {
        Sio2Transfer request; request.input={0x21,uint8_t(port<2?0x12:0x13),0,0,0,0};
        request.outputSize=6; request.sendControl[0]=0x180640|(port&1); return request;
    }
    bool probeThroughImports(uint32_t port) {
        const auto begin=registry->bind("sio2man",0x203,48),transfer=registry->bind("sio2man",0x203,51),end=registry->bind("sio2man",0x203,52);
        if (!begin.registration || !transfer.registration || !end.registration) return false;
        auto request=probe(port); std::vector<uint8_t> bytes(0xa8);
        auto put=[&](size_t offset,uint32_t value) { for (unsigned i=0;i<4;++i) bytes[offset+i]=uint8_t(value>>(i*8)); };
        put(4+(port&1)*4,0x505); put(0x14+(port&1)*4,0); put(0x28,request.sendControl[0]);
        put(0x6c,6); put(0x70,6); put(0x74,packet+0x94); put(0x78,packet+0xa0);
        std::copy(request.input.begin(),request.input.end(),bytes.begin()+0x94);
        const std::array<IopMemoryWrite,1> writes{{{packet,bytes}}};
        if (!host.writeIopMemoryBatch(writes) || !registry->invoke(begin,{}).handled) return false;
        struct End { NativeLibraryRegistry& registry; NativeImportBinding binding; ~End() { registry.invoke(binding,{}); } } finish{*registry,end};
        const auto result=registry->invoke(transfer,{packet,0,0,0}); uint32_t status=0;
        return result.handled && result.value==1 && host.readGuest(packet,&status,4) && (status&0x10000);
    }
    NativeExportResult call(uint16_t ordinal,const std::array<uint32_t,4>& args) {
        std::lock_guard lock(mutex); if (!active) return {};
        const auto port=args[0]; if (port>=4) return {true,0};
        if (ordinal==4) {
            // c60 returns 1 even when the probe reports no attached multitap.
            if (!probeThroughImports(port)) return {};
            opened[port]=true; validatedMissing[port]=true; return {true,1};
        }
        if (ordinal==5) { opened[port]=false; return {true,1}; }
        if (ordinal==6) return opened[port] && !validatedMissing[port] ? NativeExportResult{}:NativeExportResult{true,0};
        return {};
    }
    NativeExportResult callback(unsigned index,const std::array<uint32_t,4>& args) {
        std::lock_guard lock(mutex); if (!active) return {};
        if (index==1 || index==2) return args[0]<4 ? NativeExportResult{true,1}:NativeExportResult{};
        if (index==3) { pending=true; changed.notify_all(); return {true,0}; }
        std::array<uint8_t,32> bytes{};
        if (!args[0] || (args[0]&3u) || args[0]>UINT32_MAX-31u || !host.readGuest(args[0],bytes.data(),32)) return {};
        std::array<uint8_t,16> availability{}; bool all=true;
        for (unsigned i=0;i<4;++i) {
            uint32_t slot=0; for (unsigned j=0;j<4;++j) slot|=uint32_t(bytes[i*4+j])<<(j*8);
            // Original 0x680: -1 -> availability 0; slot 0 -> 1; others -> -1.
            const uint32_t result=slot==UINT32_MAX ? 0 : slot==0 ? 1 : UINT32_MAX;
            all &= result!=UINT32_MAX;
            for (unsigned j=0;j<4;++j) availability[i*4+j]=uint8_t(result>>(j*8));
        }
        const std::array<IopMemoryWrite,1> writes{{{args[0]+16,availability}}};
        if (!host.writeIopMemoryBatch(writes)) return {};
        return {true,all?1:0};
    }
};
MtapResident::MtapResident(IopHost& host,std::shared_ptr<Sio2Device> bus):state_(std::make_shared<State>(host,std::move(bus))) {}
MtapResident::~MtapResident() { reset(); }
bool MtapResident::active() const { std::lock_guard lock(state_->mutex); return state_->active; }
bool MtapResident::start() {
    auto& state=*state_;
    if (!state.registry || !state.bus || active() || state.registration) return false;
    for (uint16_t ordinal:{48,51,52,53,54,55,56}) if (!state.registry->bind("sio2man",0x203,ordinal).registration) return false;
    try {
        state.image=state.host.allocateIopMemory(0x2000); state.stack=state.host.allocateIopMemory(0x800); state.packet=state.host.allocateIopMemory(0xa8);
        if (!state.image || !state.stack || !state.packet) { reset(); return false; }
        std::vector<uint8_t> image(0x2000),stack(0x800);
        const std::array<IopMemoryWrite,2> writes{{{state.image,image},{state.stack,stack}}};
        if (!state.host.writeIopMemoryBatch(writes)) { reset(); return false; }
        std::weak_ptr<State> weak=state_; NativeLibrary library{"mtapman",0x204,{}};
        for (uint16_t ordinal:{4,5,6}) library.exports.push_back({ordinal,[weak,ordinal](const auto& args) { const auto state=weak.lock(); return state?state->call(ordinal,args):NativeExportResult{}; }});
        state.registration=state.registry->registerLibrary(std::move(library));
        if (!state.registration) { reset(); return false; }
        const std::array<uint32_t,4> offsets{0x680,0x8c0,0x8d8,0x8f0};
        for (unsigned i=0;i<4;++i) {
            if (!state.registry->registerCallback(state.registration,state.image+offsets[i],[weak,i](const auto& args) { const auto state=weak.lock(); return state?state->callback(i,args):NativeExportResult{}; })) { reset(); return false; }
        }
        state.active=true;
        // Validate an actual import-driven absent-device transaction before publishing callbacks.
        if (!state.probeThroughImports(0)) { reset(); return false; }
        for (unsigned i=0;i<4;++i) {
            if (!state.registry->invoke(state.registry->bind("sio2man",0x203,uint16_t(53+i)),{state.image+offsets[i],0,0,0}).handled) { reset(); return false; }
            state.installed[i]=true;
        }
        state.worker=std::jthread([weak](std::stop_token stop) {
            const auto state=weak.lock(); if (!state) return;
            std::unique_lock lock(state->mutex);
            while (state->changed.wait(lock,stop,[&] {return state->pending;})) {
                state->pending=false; const auto opened=state->opened; lock.unlock();
                // Rescan copied commands on the native bus. Never acquire EE
                // call-scope/registry locks from this worker (reset joins it).
                for (unsigned port=0;port<4;++port) if (opened[port]) {
                    Sio2Completion reply; bool missing=false;
                    try { missing=state->bus->exchange(State::probe(port),reply) && (reply.receiveStatus[0]&0x10000); }
                    catch (...) { missing=false; }
                    std::lock_guard update(state->mutex); state->validatedMissing[port]=missing;
                }
                lock.lock();
            }
        });
        return true;
    } catch (...) { reset(); throw; }
}
void MtapResident::reset() {
    auto& state=*state_;
    if (state.registry && state.registration) {
        for (unsigned i=0;i<4;++i) if (state.installed[i]) state.registry->invoke(state.registry->bind("sio2man",0x203,uint16_t(53+i)),{});
        state.registry->unregisterLibrary(state.registration);
    }
    state.registration=0;
    { std::lock_guard lock(state.mutex); state.active=false; state.pending=false; state.opened={}; state.installed={}; state.validatedMissing={}; }
    if (state.worker.joinable()) { state.worker.request_stop(); state.changed.notify_all(); state.worker.join(); }
    for (auto address:{state.packet,state.stack,state.image}) if (address) state.host.freeIopMemory(address);
    state.packet=state.stack=state.image=0;
}
RpcResult MtapResident::handleRpc(const RpcRequest& request) {
    if (!active() || request.sid<0x80000901 || request.sid>0x80000905 || request.sid==0x80000904) return {};
    const size_t size=request.sid==0x80000905?4:8;
    if (request.receive.size<size || !request.receive.address ||
        (size==8 && (request.send.size<4 || !request.send.address))) return {};
    std::array<uint32_t,2> reply{};
    if (size==8 && !state_->host.readGuest(request.send.address,reply.data(),4)) return {};
    if (size==4) reply[0]=0x301; // RPC version halfword at original 0x19b4.
    else {
        const auto ordinal=uint16_t(request.sid-0x80000901+4);
        const auto result=state_->call(ordinal,{reply[0],0,0,0}); if (!result.handled) return {};
        reply[1]=static_cast<uint32_t>(result.value);
    }
    if (!state_->host.writeGuest(request.receive.address,reply.data(),size)) return {};
    RpcResult result; result.handled=true; result.resultAddress=request.receive.address; return result;
}
}
