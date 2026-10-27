#include "ps2x/iop/gt3/sio2_native_exports.h"
#include <mutex>
#include <thread>

namespace ps2x::iop::gt3 {
struct Sio2NativeExports::State {
    IopHost& host;
    std::shared_ptr<Sio2Device> device;
    std::recursive_mutex mutex;
    bool published=false;
    bool inFlight=false;
    uint32_t session=0;
    uint64_t generation=0;
    std::thread::id owner;
    std::array<uint32_t,3> status{};
    Sio2Worker worker;
    std::array<NativeCallbackBinding,4> callbacks{};
    std::array<bool,4> inCallback{};
    State(IopHost& h,std::shared_ptr<Sio2Device> d):host(h),device(std::move(d)) {}
    NativeExportResult call(uint16_t ordinal,const std::array<uint32_t,4>& args) {
        std::lock_guard lock(mutex);
        if (!published) return {};
        uint32_t begin=0;
        switch (ordinal) {
        case 23: begin=1; break;
        case 24: begin=4; break;
        case 48: begin=0x10; break;
        case 49: begin=0x40; break;
        case 50: begin=0x100; break;
        }
        if (begin) {
            // Host-thread ownership is a bounded native policy, not guest IOP scheduling.
            // Original blocking contention is unsupported and stays negative.
            if (session) return {};
            worker.reset(); worker.signal(begin); worker.advance(); worker.clear(begin<<1);
            if (worker.phase()!=Sio2Worker::Phase::Ready) return {};
            session=begin; owner=std::this_thread::get_id(); ++generation;
            return {true,0}; // Original export is void; no success packet fabricated.
        }
        if (ordinal==26 || ordinal==52) {
            if (session && owner!=std::this_thread::get_id()) return {};
            worker.reset();
            session=0; inFlight=false; owner={}; ++generation;
            return {true,0};
        }
        if (ordinal==25 || ordinal==51) {
            if (!session || inFlight || owner!=std::this_thread::get_id() || !device) return {};
            struct Flight {
                State& state; uint64_t generation;
                explicit Flight(State& s):state(s),generation(s.generation) { state.inFlight=true; }
                ~Flight() { if (state.generation==generation) state.inFlight=false; }
            } flight(*this);
            struct GuardedDevice final : Sio2Device {
                State& state; uint64_t generation; std::array<uint32_t,3> status{};
                GuardedDevice(State& s):state(s),generation(s.generation) {}
                bool exchange(const Sio2Transfer& request,Sio2Completion& reply) override {
                    if (!state.device->exchange(request,reply)) return false;
                    // Reentrant reset/end cancels before executeSio2Transfer commits.
                    if (!state.published || state.generation!=generation) return false;
                    status=reply.receiveStatus; return true;
                }
            } guarded(*this);
            uint32_t events=0;
            const auto result=executeSio2Transfer(host,args[0],worker,&guarded,events);
            if (result!=Sio2Result::Ok || !(events&0x800)) return {};
            status=guarded.status;
            return {true,1};
        }
        if (ordinal==11) return {true,static_cast<int32_t>(status[1])};
        if (ordinal>=53 && ordinal<=56) {
            NativeCallbackBinding binding{};
            if (args[0]) {
                const auto registry=host.nativeLibraries();
                if (!registry) return {};
                binding=registry->bindCallback(args[0]);
                if (!binding.callback) return {}; // Invalid replacement preserves old binding.
            }
            callbacks[ordinal-53]=binding;
            return {true,0};
        }
        if (ordinal>=57 && ordinal<=60 && callbacks[ordinal-57].callback) {
            const auto index=ordinal-57;
            const auto registry=host.nativeLibraries();
            if (!registry || inCallback[index]) return {};
            const auto before=generation;
            struct CallbackFlight {
                State& state; size_t index;
                CallbackFlight(State& s,size_t i):state(s),index(i) { state.inCallback[index]=true; }
                ~CallbackFlight() { state.inCallback[index]=false; }
            } flight(*this,index);
            const auto result=registry->invokeCallback(callbacks[index],args);
            return published && generation==before ? result : NativeExportResult{};
        }
        if (ordinal==57) return applySio2DefaultSlotChange(host,args[0]);
        if (ordinal==58 || ordinal==59) return {true,1}; // Original defaults ignore port.
        if (ordinal==60) return {true,0}; // Original default is void/no operation.
        return {};
    }
};
Sio2NativeExports::Sio2NativeExports(IopHost& host,std::shared_ptr<Sio2Device> device)
    :state_(std::make_shared<State>(host,std::move(device))),registry_(host.nativeLibraries()) {}
Sio2NativeExports::~Sio2NativeExports() { reset(); }
bool Sio2NativeExports::publish() {
    if (!registry_ || registration_) return false;
    NativeLibrary library{"sio2man",0x203,{}};
    std::weak_ptr<State> weak=state_;
    for (const uint16_t ordinal : {11,23,24,25,26,48,49,50,51,52,53,54,55,56,57,58,59,60}) {
        library.exports.push_back({ordinal,[weak,ordinal](const std::array<uint32_t,4>& args) {
            const auto state=weak.lock(); return state ? state->call(ordinal,args) : NativeExportResult{};
        }});
    }
    // Registry lock precedes state lock for invocation/reset. No owner-thread
    // publication race: caller must publish/reset on its controlling host thread.
    registration_=registry_->registerLibrary(std::move(library));
    if (!registration_) return false;
    std::lock_guard lock(state_->mutex); state_->published=true;
    return true;
}
void Sio2NativeExports::reset() {
    if (registry_ && registration_) registry_->unregisterLibrary(registration_);
    registration_=0;
    std::lock_guard lock(state_->mutex);
    state_->published=false; state_->inFlight=false; state_->session=0; state_->owner={};
    state_->status={}; state_->callbacks={}; state_->inCallback={}; ++state_->generation;
    state_->worker.reset();
}
}
