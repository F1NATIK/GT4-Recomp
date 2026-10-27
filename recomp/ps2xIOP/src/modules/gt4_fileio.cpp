#include "gt4_iop.h"

// IOPRP300 FILEIO 2.18 (SDK 3.0), SID 0x80000001. Unlike SDK 2.x, results do
// not come back in the RPC reply: the IOP worker sends SIF command 0x80000011
// with a result packet into one of two EE buffers (registered by fn 0xff), and
// GT4's handler 0x5b23b8 copies the result word to the caller's result pointer,
// copies any output data and iSignalSema()s the caller's semaphore.
//
// Every request starts {sema id, EE result pointer, result bytes, ...}. This
// component performs the same effects natively: result word, output data,
// semaphore signal. Commands not written yet complete with -ErrIo so the caller
// is never left waiting, and are logged.

namespace ps2x::iop::gt4
{
    namespace
    {
        constexpr uint32_t Sid = 0x80000001u;
        constexpr int32_t ErrNoDevice = -19;
        constexpr int32_t ErrIo = -5;

        class Fileio final : public Component
        {
        public:
            Fileio(IopHost &host, bool networkAdaptor) : host_(host), networkAdaptor_(networkAdaptor) {}

            void reset() override
            {
                std::lock_guard lock(mutex_);
                logged_ = 0;
            }

            RpcResult handleRpc(const RpcRequest &request) override
            {
                std::lock_guard lock(mutex_);
                if (request.sid != Sid)
                    return rejected();
                if (request.function == 0xff)
                {
                    // Init: {EE result buffer 0, buffer 1}; reply {.., 2} (handler 0x1e68).
                    const std::array<uint32_t, 2> words{0u, 2u};
                    return reply(host_, request, words.data(), std::min<size_t>(8u, request.receive.size));
                }
                std::array<uint32_t, 3> head{};
                if (!readSend(host_, request, head.data(), sizeof(head)))
                    return rejected();
                int32_t result = ErrIo;
                std::string detail;
                if (request.function == 0x17)
                {
                    // devctl {sema, res*, 4, name @0xc, args @0x40c, cmd @0x80c, arglen, out*, outlen}
                    std::array<char, 64> name{};
                    uint32_t cmd = 0;
                    host_.readGuest(request.send.address + 0xc, name.data(), name.size() - 1);
                    host_.readGuest(request.send.address + 0x80c, &cmd, 4);
                    detail = std::string(" devctl '") + name.data() + "' " + hex(cmd);
                    // No network adapter or other devctl-capable device is present,
                    // unless GT4_NET is on: DEV9 0x288-0x2d0 answers 0x4401 with the
                    // adaptor type (1 = expansion bay, CXD9611) and 0x4402 (shut the
                    // adaptor down, CORE 0x1027d0) with 0 [C]; CORE only tests >= 0.
                    result = ErrNoDevice;
                    if (networkAdaptor_ && std::string_view(name.data()).rfind("dev9x", 0) == 0)
                    {
                        if (cmd == 0x4401u)
                            result = 1;
                        else if (cmd == 0x4402u)
                            result = 0;
                    }
                }
                complete(head, result);
                if (logged_ < 128)
                {
                    ++logged_;
                    host_.log(result == ErrIo ? LogLevel::Warning : LogLevel::Info,
                              "[GT4/FILEIO] fn " + hex(request.function) + detail + " -> " + std::to_string(result) +
                                  (result == ErrIo ? " (not written yet)" : ""));
                }
                // The reply word: CORE's devctl (0x5b5698) waits for the result only when it is non-zero, else it
                // returns -11 at once (0x5b58e0-0x5b58f8) [C]; with GT4_NET on the adaptor check (0x4ed008 ->
                // 0x645430, isNetworkAdaptorAvailable) needs the result. GT4_NET off: 0 as before (the result is
                // negative either way).
                const uint32_t word = request.function == 0x17 && networkAdaptor_ ? 1u : 0u;
                return reply(host_, request, &word, std::min<size_t>(4u, request.receive.size));
            }

        private:
            void complete(const std::array<uint32_t, 3> &head, int32_t result)
            {
                if (head[1] && head[2])
                    host_.writeGuest(head[1], &result, std::min<uint32_t>(head[2], 4u));
                if (static_cast<int32_t>(head[0]) >= 0)
                    host_.signalEeSemaphore(static_cast<int>(head[0]));
            }

            IopHost &host_;
            bool networkAdaptor_ = false;
            std::mutex mutex_;
            uint32_t logged_ = 0;
        };
    }

    std::unique_ptr<Component> createFileio(IopHost &host, bool networkAdaptor)
    {
        return std::make_unique<Fileio>(host, networkAdaptor);
    }
}
