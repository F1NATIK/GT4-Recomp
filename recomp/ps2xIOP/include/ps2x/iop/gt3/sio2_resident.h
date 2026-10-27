#pragma once
#include "ps2x/iop/gt3/sio2_native_exports.h"

namespace ps2x::iop::gt3 {
struct Sio2StartupConfig {
    uint32_t control=0x3bc;
    uint32_t eventAttributes=2;
    uint32_t workerAttributes=0x02000000;
    uint32_t stackAddress=0;
    uint32_t stackSize=0x2000;
    uint32_t priority=16;
    uint32_t interrupt=0x11;
    uint32_t interruptMode=1;
    uint32_t dmaInput=11,dmaOutput=12,dmaPriority=3;
};
// Exclusive native backend contract. A successful acquisition/configuration
// must be undone by release; failed steps must leave no unowned side effects.
// Implementations must drive the worker/interrupt/DMA, not merely store IDs.
class Sio2ResidentBackend {
public:
    virtual ~Sio2ResidentBackend()=default;
    virtual bool acquire() { return false; }
    virtual bool configure(const Sio2StartupConfig&) { return false; }
    virtual bool startWorker() { return false; }
    virtual void stopWorker() noexcept {}
    virtual void release() noexcept {}
};
// Transaction orchestration only; no backend is activated by LOADFILE yet.
class Sio2Resident {
public:
    Sio2Resident(IopHost& host,Sio2ResidentBackend& backend,std::shared_ptr<Sio2Device> device={});
    ~Sio2Resident();
    Sio2Resident(const Sio2Resident&)=delete;
    Sio2Resident& operator=(const Sio2Resident&)=delete;
    bool start(uint32_t argumentBytes=0);
    void reset();
    bool active() const { return active_; }
    uint32_t stackAddress() const { return stack_; }
private:
    IopHost& host_;
    Sio2ResidentBackend& backend_;
    Sio2NativeExports exports_;
    uint32_t stack_=0;
    bool acquired_=false,workerStarted_=false,active_=false;
};
}
