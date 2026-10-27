#include "ps2x/iop/gt3/sio2_native_backend.h"
#include "ps2x/iop/gt3/sio2_queued_device.h"
#include <mutex>
#include <unordered_set>

namespace ps2x::iop::gt3 {
namespace {
std::mutex ownersMutex;
std::unordered_set<NativeLibraryRegistry*> owners;
bool validDma(const Sio2Dma& dma,size_t bytes) {
    if (!dma.address) return bytes==0;
    return !(dma.address&3u) && bytes==uint64_t(dma.blockSize&0xffffu)*(dma.blockCount&0xffffu)*4u &&
        bytes<=Sio2MaxDmaBytes && (!bytes || uint64_t(dma.address)+bytes<=uint64_t(UINT32_MAX)+1u);
}
}
struct Sio2NativeBackend::State {
    IopHost& host;
    NativeLibraryRegistry* registry;
    std::shared_ptr<Sio2Device> source;
    std::shared_ptr<Sio2QueuedDevice> queue;
    std::mutex mutex;
    bool acquired=false,configured=false,enabled=false,busy=false;
    uint64_t generation=0;
    uint32_t events=0;
    bool dmaInputActive=false,dmaOutputActive=false;
    State(IopHost& h,std::shared_ptr<Sio2Device> s):host(h),registry(h.nativeLibraries()),source(std::move(s)) {}
    struct InterruptDevice final : Sio2Device {
        std::weak_ptr<State> weak;
        explicit InterruptDevice(std::shared_ptr<State> state):weak(state) {}
        bool exchange(const Sio2Transfer& request,Sio2Completion& reply) override {
            const auto state=weak.lock(); if (!state) return false;
            uint64_t generation;
            {
                std::lock_guard lock(state->mutex);
                if (!state->enabled || !state->busy) return false;
                generation=state->generation;
            }
            Sio2Completion staged;
            if (!state->source->exchange(request,staged) || staged.output.size()!=request.outputSize ||
                staged.dmaOutput.size()!=request.dmaOutputSize) return false;
            std::lock_guard lock(state->mutex);
            if (!state->enabled || generation!=state->generation) return false;
            // Modeled DMA input is the request snapshot; output is staged here.
            // IRQ 0x11's completion handler signals the original 0x2000 event.
            // No EE/IOP memory is written and no 0x800 commit event is fabricated.
            state->dmaInputActive=false; state->dmaOutputActive=false;
            state->events|=0x2000;
            reply=std::move(staged);
            return true;
        }
    };
};
Sio2NativeBackend::Sio2NativeBackend(IopHost& host,std::shared_ptr<Sio2Device> source)
    :state_(std::make_shared<State>(host,std::move(source))) {}
Sio2NativeBackend::~Sio2NativeBackend() { release(); }
bool Sio2NativeBackend::acquire() {
    auto& state=*state_; std::lock_guard lock(state.mutex);
    if (state.acquired || !state.registry || !state.source) return false;
    std::lock_guard ownersLock(ownersMutex);
    if (!owners.insert(state.registry).second) return false;
    state.acquired=true; return true;
}
bool Sio2NativeBackend::configure(const Sio2StartupConfig& config) {
    auto& state=*state_; std::lock_guard lock(state.mutex);
    if (!state.acquired || state.configured || config.control!=0x3bc || config.eventAttributes!=2 ||
        config.workerAttributes!=0x02000000 || !config.stackAddress || config.stackSize!=0x2000 ||
        config.priority!=16 || config.interrupt!=0x11 || config.interruptMode!=1 ||
        config.dmaInput!=11 || config.dmaOutput!=12 || config.dmaPriority!=3) return false;
    // Verify the entire stack through the IOP-only write capability. This also
    // clears it and rejects EE/unallocated storage rather than accepting IDs.
    std::vector<uint8_t> stack(config.stackSize);
    const std::array<IopMemoryWrite,1> writes{{{config.stackAddress,stack}}};
    if (!state.host.writeIopMemoryBatch(writes)) return false;
    state.configured=true; state.events=0; return true;
}
bool Sio2NativeBackend::startWorker() {
    auto& state=*state_; std::lock_guard lock(state.mutex);
    if (!state.configured || state.enabled || state.queue) return false;
    state.queue=std::make_shared<Sio2QueuedDevice>(std::make_shared<State::InterruptDevice>(state_));
    state.enabled=true; ++state.generation; return true;
}
void Sio2NativeBackend::stopWorker() noexcept {
    auto& state=*state_; std::shared_ptr<Sio2QueuedDevice> queue;
    {
        std::lock_guard lock(state.mutex);
        state.enabled=false; ++state.generation; state.events=0;
        state.dmaInputActive=false; state.dmaOutputActive=false;
        queue=std::move(state.queue);
    }
    if (queue) queue->stop(); // Control-thread contract; joins finite callbacks.
}
void Sio2NativeBackend::release() noexcept {
    stopWorker();
    auto& state=*state_; std::lock_guard lock(state.mutex);
    if (state.acquired) {
        std::lock_guard ownersLock(ownersMutex); owners.erase(state.registry);
    }
    state.acquired=false; state.configured=false;
}
bool Sio2NativeBackend::exchange(const Sio2Transfer& request,Sio2Completion& reply) {
    auto& state=*state_; std::shared_ptr<Sio2QueuedDevice> queue; uint64_t generation;
    {
        std::lock_guard lock(state.mutex);
        if (!state.enabled || state.busy || !validDma(request.inputDma,request.dmaInput.size()) ||
            !validDma(request.outputDma,request.dmaOutputSize)) return false;
        state.busy=true; state.events=0; generation=state.generation; queue=state.queue;
        state.dmaInputActive=request.inputDma.address!=0; state.dmaOutputActive=request.outputDma.address!=0;
    }
    struct Cleanup {
        State& state;
        ~Cleanup() { std::lock_guard lock(state.mutex); state.busy=false; state.events=0;
            state.dmaInputActive=false; state.dmaOutputActive=false; }
    } cleanup{state};
    Sio2Completion staged;
    if (!queue->exchange(request,staged)) return false;
    std::lock_guard lock(state.mutex);
    if (!state.enabled || state.generation!=generation || !(state.events&0x2000)) return false;
    state.events&=~0x2000u; reply=std::move(staged); return true;
}
}
