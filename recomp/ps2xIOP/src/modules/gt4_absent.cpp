#include "gt4_iop.h"

#include <set>

// Services of GT4 modules whose hardware is not present on this machine:
// PDIUSB 'bsuP' (USB keyboard/mouse; handler 0x78 always replies null),
// RTINETB 0x80001201 (network runtime), EYETOY 0x80001400, PDINETCNF 'PNCF'
// (RTINETB and PNCF only while GT4_NET is off; gt4_net.h serves them otherwise).
// (LGDEV 0x046d046d: gt4_lgdev.cpp; USTORAGE 'PUST': gt4_ustorage.cpp.)
// Every call gets a zero-filled reply: no device attached / no network. Each
// sid/function pair is logged once so their real use can be checked later.

namespace ps2x::iop::gt4
{
    namespace
    {
        class Absent final : public Component
        {
        public:
            explicit Absent(IopHost &host) : host_(host) {}

            void reset() override
            {
                std::lock_guard lock(mutex_);
                seen_.clear();
            }

            RpcResult handleRpc(const RpcRequest &request) override
            {
                std::lock_guard lock(mutex_);
                if (seen_.insert((uint64_t(request.sid) << 32) | request.function).second)
                {
                    host_.log(LogLevel::Info, "[GT4/ABSENT] sid=" + hex(request.sid) + " fn=" + hex(request.function) +
                                                  " send=" + std::to_string(request.send.size) + " recv=" +
                                                  std::to_string(request.receive.size) + ": device absent, zero reply");
                }
                std::vector<uint8_t> zero(std::min<uint32_t>(request.receive.size, 0x10000u), 0);
                return reply(host_, request, zero.data(), zero.size());
            }

        private:
            IopHost &host_;
            std::mutex mutex_;
            std::set<uint64_t> seen_;
        };
    }

    std::unique_ptr<Component> createAbsent(IopHost &host)
    {
        return std::make_unique<Absent>(host);
    }
}
