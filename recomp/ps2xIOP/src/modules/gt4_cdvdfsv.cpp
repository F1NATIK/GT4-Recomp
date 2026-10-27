#include "gt4_iop.h"

// IOPRP300 CDVDFSV 2.38 (analysis/iop-sys/disasm/gt4/IOPRP300-CDVDFSV.txt).
// SIDs: 0x80000592 init, 0x80000593 S-commands, 0x80000595 N-commands,
// 0x80000597 search file, 0x8000059a / 0x8000059c disk ready (one handler).
// GT4 reads game data through PDICDVD; CDVDFSV only serves libcdvd control.

namespace ps2x::iop::gt4
{
    namespace
    {
        constexpr uint32_t SidInit = 0x80000592u;
        constexpr uint32_t SidSCommand = 0x80000593u;
        constexpr uint32_t SidNCommand = 0x80000595u;
        constexpr uint32_t SidSearch = 0x80000597u;
        constexpr uint32_t SidReady = 0x8000059au;
        constexpr uint32_t SidReady2 = 0x8000059cu;
        constexpr uint32_t CdvdmanVersion = 0x226u; // halfword at CDVDFSV 0x5534

        class Cdvdfsv final : public Component
        {
        public:
            Cdvdfsv(IopHost &host, Disc &disc) : host_(host), disc_(disc) {}

            void reset() override
            {
                std::lock_guard lock(mutex_);
                mode_ = 0;
                logged_ = 0;
            }

            RpcResult handleRpc(const RpcRequest &request) override
            {
                std::lock_guard lock(mutex_);
                if (request.sid == SidInit && request.function == 0)
                {
                    // Handler 0x694: {1, 0x226, cdvdman#50(-9), debug ? 0xfe : 0}.
                    const std::array<uint32_t, 4> words{1u, CdvdmanVersion, CdvdmanVersion, 0u};
                    return reply(host_, request, words.data(), std::min<size_t>(16u, request.receive.size));
                }
                if (request.sid == SidSCommand && request.function == 0x22)
                {
                    // sceCdMmode: 4-byte media mode in, 4-byte result (1) out.
                    uint32_t mode = 0;
                    if (!readSend(host_, request, &mode, 4))
                    {
                        return rejected();
                    }
                    mode_ = mode;
                    const uint32_t ok = 1u;
                    return reply(host_, request, &ok, 4);
                }
                if ((request.sid == SidReady || request.sid == SidReady2) && request.function == 0)
                {
                    // sceCdDiskReady: the image is always ready (2 = SCECdComplete).
                    const uint32_t ready = 2u;
                    return reply(host_, request, &ready, std::min<size_t>(4u, request.receive.size));
                }
                if (logged_ < 64)
                {
                    ++logged_;
                    host_.log(LogLevel::Warning, "[GT4/CDVDFSV] unsupported request sid=" + hex(request.sid) +
                                                     " fn=" + hex(request.function) + " send=" + std::to_string(request.send.size) +
                                                     " recv=" + std::to_string(request.receive.size));
                }
                return rejected();
            }

        private:
            IopHost &host_;
            Disc &disc_;
            std::mutex mutex_;
            uint32_t mode_ = 0;
            uint32_t logged_ = 0;
        };
    }

    std::unique_ptr<Component> createCdvdfsv(IopHost &host, Disc &disc)
    {
        return std::make_unique<Cdvdfsv>(host, disc);
    }
}
