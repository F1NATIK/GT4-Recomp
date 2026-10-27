#pragma once
#include "ps2x/iop/gt3/sio2_transfer.h"
#include <memory>

namespace ps2x::iop::gt3 {
// Native session/transfer exports. Publishing these is not resident IRX startup:
// Only explicitly mapped native IOP callbacks run; original IOP instruction
// execution and module interrupt/thread lifecycle remain unmodeled.
class Sio2NativeExports {
public:
    Sio2NativeExports(IopHost& host,std::shared_ptr<Sio2Device> device={});
    ~Sio2NativeExports();
    Sio2NativeExports(const Sio2NativeExports&)=delete;
    Sio2NativeExports& operator=(const Sio2NativeExports&)=delete;
    bool publish();
    void reset();
private:
    struct State;
    std::shared_ptr<State> state_;
    NativeLibraryRegistry* registry_;
    uint64_t registration_=0;
};
}
