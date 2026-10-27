#pragma once
#include "ps2x/iop/iop_host.h"
#include "ps2x/iop/native_library.h"

namespace ps2x::iop::gt3 {
// Scoped native services used by MCMAN's reconstructed startup. Not a general
// IOP kernel: contended WaitSema is unhandled (never reported as acquired).
// No security authentication/decryption or host memory-card file writes.
class McmanStartupServices {
public:
    enum class Hook { CardCommand, CardDeviceId, CheckBootPath };
    explicit McmanStartupServices(IopHost&);
    ~McmanStartupServices();
    bool start();
    void reset();
    NativeExportResult callHook(Hook,const std::array<uint32_t,4>&);
    NativeExportResult callDevice(std::string_view,uint16_t,const std::array<uint32_t,4>&);
private:
    struct State;
    std::shared_ptr<State> state_;
};
}
