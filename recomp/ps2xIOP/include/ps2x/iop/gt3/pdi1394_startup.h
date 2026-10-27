#pragma once
#include "ps2x/iop/iop_host.h"
#include <stop_token>

namespace ps2x::iop::gt3 {
inline constexpr uint32_t Pdi1394OriginalModelBytes=0x4d80;
// Caller owns this unpublished image. No worker/semaphore handles, library
// registration, SIF binding or ILINK initialization are supplied by this helper.
bool initializePdi1394Image(IopHost& host,uint32_t image);
enum class Pdi1394EeWaitStatus { Available,Cancelled,UnsupportedHost,InvalidSnapshot };
struct Pdi1394EeWaitResult {
    Pdi1394EeWaitStatus status=Pdi1394EeWaitStatus::UnsupportedHost;
    EeRpcServerSnapshot server;
    uint64_t attempts=0;
};
// Original8dc prefix: real discovery and a cancellable1ms delay. Returns a
// snapshot, not an executed SifBindRpc/ILINK4 or a persistent live binding.
Pdi1394EeWaitResult waitForPdi1394EeService(IopHost& host,std::stop_token stop);
}
