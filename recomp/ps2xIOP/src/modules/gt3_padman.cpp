#include "module_factories.h"
#include "ps2x/iop/gt3/sio2_digital_pad.h"
#include <array>
#include <algorithm>
#include <cstring>
#include <mutex>

namespace ps2x::iop::detail {
namespace {
class Gt3Padman final : public IopService {
public:
    explicit Gt3Padman(IopHost &host) : host_(host),client_(host) {}
    ~Gt3Padman() override { reset(); }
    std::string_view name() const override { return "gt3-padman-native"; }
    std::span<const uint32_t> sids() const override { return ids_; }
    void reset() override { std::lock_guard lock(mutex_); client_.reset(); if (storage_) host_.freeIopMemory(storage_); storage_=dma_=frame_=0; }
    void onGuestCall(uint32_t address) override {
        if (address != 0x272090u) return;
        std::lock_guard lock(mutex_);
        if (!storage_) return;
        if (frame_ == 0xffffffffu) return; // Stop rather than publish a wrapped, older counter.
        std::array<uint8_t,4> packet{};
        if (!client_.poll(packet)) return;
        std::array<uint8_t,0x80> record{};
        std::copy(packet.begin(),packet.end(),record.begin());
        const uint32_t next = frame_+1u;
        put(record.data()+0x58,next); put(record.data()+0x60,4);
        record[0x70]=6; record[0x71]=2;
        const std::array<IopMemoryWrite,2> writes{{{storage_,record},{dma_+((next&1u)*0x80u),record}}};
        if (host_.commitPadMemory(writes)) frame_=next;
    }
    RpcResult handleRpc(const RpcRequest &request) override {
        std::lock_guard lock(mutex_);
        if (request.sid != ids_[0] || request.function != 1 ||
            !request.send.address || request.send.size < 0x80 ||
            !request.receive.address || request.receive.size < 0x80) return {};
        // Snapshot first: send and receive may alias. Command is packet word 0.
        std::array<uint8_t, 0x80> reply{};
        if (!host_.readGuest(request.send.address, reply.data(), reply.size())) return {};
        uint32_t command = 0;
        std::memcpy(&command, reply.data(), sizeof(command));
        uint32_t result = 0;
        if (command == 18) {
            // Supplied PADMAN 0x70f4 -> 0x44d0 reads halfword at 0x7d34.
            result = 0x0402;
        } else if (command == 12) {
            // Supplied PADMAN 0x73b4 -> 0x44a8 returns two physical ports.
            result = 2;
        } else if (command == 1 || command == 14) {
            uint32_t port=0,slot=0,argument=0;
            std::memcpy(&port,reply.data()+4,4); std::memcpy(&slot,reply.data()+8,4);
            std::memcpy(&argument,reply.data()+0x10,4);
            put(reply.data()+0x0c,0); put(reply.data()+0x14,0);
            if (port || slot || (command==1 && (storage_ || !argument || (argument&63)))) {
                if (!host_.writeGuest(request.receive.address,reply.data(),reply.size())) return {};
            } else if (command == 1) {
                const uint32_t storage = host_.allocateIopMemory(0x1c8);
                if (!storage) return {};
                std::array<uint8_t,0x1c8> state{};
                std::array<uint8_t,0x100> dma{};
                for (int i=0;i<2;++i) { std::fill_n(dma.data()+i*0x80,32,uint8_t(0xff)); dma[i*0x80+0x70]=5; dma[i*0x80+0x71]=2; }
                put(reply.data()+0x0c,1); put(reply.data()+0x14,storage+0x80);
                const std::array<IopMemoryWrite,3> writes{{{storage,state},{argument,dma},{request.receive.address,reply}}};
                if (!host_.commitPadMemory(writes)) { host_.freeIopMemory(storage); return {}; }
                storage_=storage; dma_=argument; frame_=0;
            } else {
                put(reply.data()+0x0c,storage_ ? 1 : 0);
                if (!host_.writeGuest(request.receive.address,reply.data(),reply.size())) return {};
                if (storage_) host_.freeIopMemory(storage_);
                client_.reset();
                storage_=dma_=frame_=0;
            }
            RpcResult handled; handled.handled=true; handled.resultAddress=request.receive.address; return handled;
        } else return {}; // Configuration, pressure negotiation and resident loads remain unsupported.
        std::memcpy(reply.data() + 0x0c, &result, sizeof(result));
        if (!host_.writeGuest(request.receive.address, reply.data(), reply.size())) return {};
        RpcResult handled; handled.handled = true; handled.resultAddress = request.receive.address;
        return handled;
    }
private:
    inline static constexpr std::array<uint32_t, 1> ids_{0x80000100u};
    IopHost &host_;
    gt3::Sio2DigitalPadClient client_;
    static void put(uint8_t *where,uint32_t value) { std::memcpy(where,&value,4); }
    std::mutex mutex_;
    uint32_t storage_=0,dma_=0,frame_=0;
};
}
std::unique_ptr<IopService> createGt3PadmanService(IopHost &host) { return std::make_unique<Gt3Padman>(host); }
}
