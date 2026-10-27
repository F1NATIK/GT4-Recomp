#pragma once
#include "ps2x/iop/iop_host.h"
#include <functional>
#include <memory>
#include <optional>

namespace ps2x::iop::gt3 {
// Native startup/control and bounded flags0/1 ISO reads (1..128KiB).
// Active cancellation, retries and general stream exports remain unsupported.
// Lifecycle/RPC calls must run outside NativeLibraryRegistry dispatch because
// the time RPC worker imports LIBPDI. Never join it while holding that registry.
class PdicdvdResident {
public:
    using Calendar=std::array<uint32_t,6>; // year,month,day,hour,minute,second
    using CalendarSource=std::function<std::optional<Calendar>()>;
    explicit PdicdvdResident(IopHost&,CalendarSource={});
    ~PdicdvdResident();
    bool start();
    bool reset();
    bool active() const;
    RpcResult handleRpc(const RpcRequest&);
    // Native producer backend; same owned read worker and disc identity checks
    // as RPC3. One bounded batch,1..64 sectors. Call outside registry dispatch.
    // This is not acceptance of the original streaming exports or RPCs.
    std::optional<std::vector<uint8_t>> readStreamSectors(uint32_t lsn,uint32_t sectors);
private:
    struct State;
    std::shared_ptr<State> state_;
};
}
