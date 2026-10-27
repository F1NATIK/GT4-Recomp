#include "gt4_iop.h"

#include <chrono>
#include <ctime>

// PDICDVD 0x111 (analysis/iop-pdi/PDICDVD/gt4-disasm.txt), Polyphony's disc
// reader. Main server PCDV 0x50434456 (handler 0x1040, table 0x1800):
//   fn0 version {0x111}; fn1 clear disc identity; fn2 set 64-byte identity;
//   fn3 read {lsn, bytes, EE dst, flags}: synchronous - the handler reads the
//       sectors through a 0x20000 ring and ring_to_ee()s them to EE dst, then
//       replies word0 = ring error ? 1 : 0;
//   fn4 dual-layer info (64 bytes from 0xd6c): {on_dual, layer1 start, ...}.
// Secondary server Pcdv 0x50636476: fn0 starts a worker that every 100 ms DMAs
// a 64-byte status block to the EE address in buf[0] (0x12fc / 0xf40):
//   {calendar seconds since 2000-01-01, clock status, disk state, seconds since
//   the disk state changed}; fn1 stops it; fn2 cancels a stream read.
// Disk state 0 = ready (EE 0x104180 flags an error when it is non-zero for 3 s).

namespace ps2x::iop::gt4
{
    namespace
    {
        constexpr uint32_t SidMain = 0x50434456u;
        constexpr uint32_t SidStatus = 0x50636476u;
        constexpr uint32_t Version = 0x111u;

        uint32_t calendarSeconds()
        {
            // libpdi#106 day count from 2000-01-01 (two-digit year), local time.
            const std::time_t now = std::time(nullptr);
            std::tm local{};
#if defined(_WIN32)
            localtime_s(&local, &now);
#else
            localtime_r(&now, &local);
#endif
            std::tm epoch{};
            epoch.tm_year = 100;
            epoch.tm_mday = 1;
            epoch.tm_isdst = local.tm_isdst;
            const std::time_t base = std::mktime(&epoch);
            return static_cast<uint32_t>(now - base);
        }

        class Pdicdvd final : public Component
        {
        public:
            Pdicdvd(IopHost &host, Disc &disc) : host_(host), disc_(disc) {}

            void reset() override
            {
                std::lock_guard lock(mutex_);
                identity_.fill(0);
                statusAddress_ = 0;
                reads_ = 0;
                bytes_ = 0;
                logged_ = 0;
                start_ = std::chrono::steady_clock::now();
            }

            RpcResult handleRpc(const RpcRequest &request) override
            {
                std::lock_guard lock(mutex_);
                if (request.sid == SidStatus)
                    return status(request);
                if (request.sid != SidMain)
                    return rejected();
                switch (request.function)
                {
                case 0:
                    return reply(host_, request, &Version, std::min<size_t>(4u, request.receive.size));
                case 1:
                    identity_.fill(0);
                    return reply(host_, request, nullptr, 0);
                case 2:
                    readSend(host_, request, identity_.data(), std::min<size_t>(64u, request.send.size));
                    return reply(host_, request, nullptr, 0);
                case 3:
                    return read(request);
                case 4:
                {
                    std::array<uint32_t, 16> info{};
                    info[0] = 1; // dual layer
                    info[1] = Disc::Layer1Start;
                    return reply(host_, request, info.data(), std::min<size_t>(sizeof(info), request.receive.size));
                }
                default:
                    break;
                }
                host_.log(LogLevel::Warning, "[GT4/PDICDVD] unsupported PCDV fn " + hex(request.function));
                return rejected();
            }

            void onVBlank(bool start, uint64_t tick) override
            {
                if (!start || tick % 6u)
                    return; // 100 ms at 60 Hz
                std::lock_guard lock(mutex_);
                pushStatus();
            }

        private:
            RpcResult read(const RpcRequest &request)
            {
                std::array<uint32_t, 4> r{};
                if (!readSend(host_, request, r.data(), 13))
                    return rejected();
                const uint32_t lsn = r[0], bytes = r[1], destination = r[2];
                const uint32_t sectors = (bytes + 2047u) / 2048u;
                uint32_t error = 0;
                if (bytes)
                {
                    buffer_.resize(size_t(sectors) * 2048u);
                    if (disc_.readSectors(lsn, sectors, buffer_.data()))
                    {
                        // ring_to_ee rounds each fragment up to 16 bytes.
                        const uint32_t dma = std::min<uint32_t>((bytes + 15u) & ~15u, sectors * 2048u);
                        if (!host_.writeGuest(destination, buffer_.data(), dma))
                            error = 1;
                    }
                    else
                    {
                        error = 1;
                    }
                }
                ++reads_;
                bytes_ += bytes;
                if (error || logged_ < 16)
                {
                    ++logged_;
                    host_.log(error ? LogLevel::Error : LogLevel::Info,
                              "[GT4/PDICDVD] read lsn " + std::to_string(lsn) + " bytes " + std::to_string(bytes) + " -> " +
                                  hex(destination) + (error ? " FAILED" : ""));
                }
                return reply(host_, request, &error, std::min<size_t>(4u, request.receive.size));
            }

            RpcResult status(const RpcRequest &request)
            {
                if (request.function == 0)
                {
                    uint32_t address = 0;
                    if (!readSend(host_, request, &address, 4))
                        return rejected();
                    statusAddress_ = address;
                    pushStatus();
                    host_.log(LogLevel::Info, "[GT4/PDICDVD] status push to " + hex(address) + " every 100 ms");
                }
                else if (request.function == 1)
                {
                    statusAddress_ = 0;
                }
                else if (request.function != 2)
                {
                    host_.log(LogLevel::Warning, "[GT4/PDICDVD] unsupported Pcdv fn " + hex(request.function));
                    return rejected();
                }
                return reply(host_, request, nullptr, 0);
            }

            void pushStatus()
            {
                if (!statusAddress_)
                    return;
                std::array<uint32_t, 16> block{};
                block[0] = calendarSeconds();
                block[1] = 0; // sceCdReadClock status OK
                block[2] = 0; // disk state: ready
                block[3] = static_cast<uint32_t>(
                    std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - start_).count());
                host_.writeGuest(statusAddress_, block.data(), sizeof(block));
            }

            IopHost &host_;
            Disc &disc_;
            std::mutex mutex_;
            std::array<uint32_t, 16> identity_{};
            uint32_t statusAddress_ = 0;
            std::vector<uint8_t> buffer_;
            uint64_t reads_ = 0, bytes_ = 0;
            uint32_t logged_ = 0;
            std::chrono::steady_clock::time_point start_ = std::chrono::steady_clock::now();
        };
    }

    std::unique_ptr<Component> createPdicdvd(IopHost &host, Disc &disc)
    {
        return std::make_unique<Pdicdvd>(host, disc);
    }
}
