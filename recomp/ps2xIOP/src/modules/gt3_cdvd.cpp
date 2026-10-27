#include "module_factories.h"
#include <algorithm>
#include <array>
#include <mutex>

namespace ps2x::iop::detail {
namespace {
// IOPRP224 CDVDFSV: init handler 0x430, SCMD dispatcher 0x40ac,
// mode handler 0x3a00. This implements control requests, not sector reads.
class Gt3Cdvd final : public IopService {
public:
    explicit Gt3Cdvd(IopHost& host) : host_(host) {}
    std::string_view name() const override { return "gt3-cdvdfsv-control"; }
    std::span<const uint32_t> sids() const override { return ids_; }
    void reset() override {
        std::lock_guard lock(mutex_);
        initialized_ = false; mode_ = 0; requests_ = 0;
    }
    RpcResult handleRpc(const RpcRequest& request) override {
        const bool init = request.sid == ids_[0] && request.function == 0;
        const bool mode = request.sid == ids_[1] && request.function == 0x22;
        if (!init && !mode) return {}; // Preserve unhandled diagnostics.
        std::lock_guard lock(mutex_);
        uint32_t argument = 0;
        if (request.send.size < 4 || !request.send.address ||
            !host_.readGuest(request.send.address, &argument, 4)) return {};
        if (init && argument != 0) return {}; // Only the observed init mode is characterized.
        std::array<uint32_t, 4> reply{1, 0x214, 0x214, 0};
        const size_t bytes = init ? 16 : 4;
        if (request.receive.size < bytes || !request.receive.address ||
            !host_.writeGuest(request.receive.address, reply.data(), bytes)) return {};
        if (init) initialized_ = true;
        else mode_ = argument;
        ++requests_;
        host_.log(LogLevel::Info, init ? "[GT3/CDVD] init control reply (no disc read)"
                                     : "[GT3/CDVD] disc mode updated (no disc read)");
        RpcResult result;
        result.handled = true;
        result.resultAddress = request.receive.address;
        return result;
    }
    void appendDebugMetrics(std::vector<DebugMetric>& metrics) const override {
        std::lock_guard lock(mutex_);
        metrics.push_back({"initialized", initialized_, false});
        metrics.push_back({"disc_mode", mode_, false});
        metrics.push_back({"control_requests", requests_, false});
    }
private:
    inline static constexpr std::array<uint32_t, 2> ids_{0x80000592u, 0x80000593u};
    IopHost& host_;
    mutable std::mutex mutex_;
    bool initialized_ = false;
    uint32_t mode_ = 0;
    uint64_t requests_ = 0;
};
}
std::unique_ptr<IopService> createGt3CdvdService(IopHost& host) {
    return std::make_unique<Gt3Cdvd>(host);
}
}
