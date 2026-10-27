#include "ps2x/iop/gt3/sio2_resident.h"

namespace ps2x::iop::gt3 {
Sio2Resident::Sio2Resident(IopHost& host,Sio2ResidentBackend& backend,std::shared_ptr<Sio2Device> device)
    :host_(host),backend_(backend),exports_(host,std::move(device)) {}
Sio2Resident::~Sio2Resident() { reset(); }
bool Sio2Resident::start(uint32_t argumentBytes) {
    // Unsupported optional arguments and duplicate initialization remain failure.
    if (argumentBytes || active_ || acquired_) return false;
    try {
        if (!backend_.acquire()) return false;
        acquired_=true;
        stack_=host_.allocateIopMemory(0x2000);
        if (!stack_) { reset(); return false; }
        // Native stacks belong to modeled IOP heap, never EE guestMalloc.
        std::vector<uint8_t> zeroes(0x2000);
        const std::array<IopMemoryWrite,1> writes{{{stack_,zeroes}}};
        if (!host_.writeIopMemoryBatch(writes)) { reset(); return false; }
        Sio2StartupConfig config; config.stackAddress=stack_;
        if (!backend_.configure(config)) { reset(); return false; }
        if (!exports_.publish()) { reset(); return false; }
        if (!backend_.startWorker()) { reset(); return false; }
        workerStarted_=true; active_=true;
        return true;
    } catch (...) {
        reset(); throw; // Preserve actual backend errors after releasing owned state.
    }
}
void Sio2Resident::reset() {
    // Exports disappear before stopping the worker/freeing its storage.
    exports_.reset();
    if (workerStarted_) backend_.stopWorker();
    workerStarted_=false; active_=false;
    if (acquired_) backend_.release();
    acquired_=false;
    if (stack_) host_.freeIopMemory(stack_);
    stack_=0;
}
}
