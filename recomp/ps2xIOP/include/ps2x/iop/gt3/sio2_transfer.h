#pragma once
#include "ps2x/iop/iop_host.h"
#include "ps2x/iop/native_library.h"
#include <array>
#include <cstdint>
#include <span>
#include <vector>

namespace ps2x::iop::gt3 {
// Layout recovered from the supplied SIO2MAN.IRX, not an EE RPC packet.
struct Sio2Dma {
    uint32_t address{}, blockSize{}, blockCount{};
};
struct Sio2Transfer {
    std::array<uint32_t,4> portControl1{}, portControl2{};
    std::array<uint32_t,16> sendControl{};
    uint32_t outputAddress{}, outputSize{};
    std::vector<uint8_t> input, dmaInput;
    uint32_t dmaOutputSize{};
    Sio2Dma inputDma{}, outputDma{};
};
enum class Sio2Result { Ok, InvalidMemory, SizeLimit, DmaUnsupported, DeviceUnsupported, InvalidDeviceReply, InvalidDma, CommitFailed, InvalidSession };
struct Sio2Completion {
    std::array<uint32_t,3> receiveStatus{};
    std::vector<uint8_t> output, dmaOutput;
};
// A device implementation must supply status words as well as response bytes.
// This boundary deliberately has no default successful/no-device reply.
class Sio2Device {
public:
    virtual ~Sio2Device() = default;
    virtual bool exchange(const Sio2Transfer& request, Sio2Completion& completion) = 0;
};
inline constexpr uint32_t Sio2PacketSize=0x94;
// Implementation resource limit, not a claim about physical SIO2 FIFO capacity.
inline constexpr uint32_t Sio2MaxFifoBytes=4096;
inline constexpr uint32_t Sio2MaxDmaBytes=65536;
// Default export 57 behavior at supplied module 0xa90..0xad8.
// Four requested slot words followed by four availability result words.
NativeExportResult applySio2DefaultSlotChange(IopHost& host,uint32_t address);
Sio2Result decodeSio2Transfer(const IopHost& host, uint32_t address, Sio2Transfer& result);
// Produces staged results only. The eventual worker must commit them and signal
// completion; neither helper changes guest RAM, launches DMA, or signals an IRQ.
Sio2Result runSio2Fifo(const Sio2Transfer& request, Sio2Device* device, Sio2Completion& completion);
}

namespace ps2x::iop::gt3 {
// Single-owner model of the supplied worker's event ordering (0x3b4..0x53c).
// An adapter must drive the actions; this class does not emulate IOP threads.
class Sio2Worker {
public:
    enum class Action { None, StartTransfer, CollectResult };
    enum class Phase { Idle, Ready, WaitingForInterrupt, Committing, Faulted };
    void reset() { events_=0; phase_=Phase::Idle; }
    void signal(uint32_t bits) { events_ |= bits; }
    void clear(uint32_t bits) { events_ &= ~bits; }
    uint32_t events() const { return events_; }
    Phase phase() const { return phase_; }
    Action advance();
    // Never publish completion until the adapter confirms the result was stored.
    bool finishCommit(bool success);
    // A rejected/cancelled native device exchange publishes no completion and
    // returns the owned session to Ready for an explicit retry.
    void cancelTransfer();
private:
    uint32_t events_{};
    Phase phase_{Phase::Idle};
};
}

namespace ps2x::iop::gt3 {
// Synchronous native adapter, not an emulation of IOP interrupt timing.
// The returned events include completion only after a successful memory commit.
Sio2Result executeSio2Transfer(IopHost& host, uint32_t packet, uint32_t sessionRequest,
                              Sio2Device* device, uint32_t& events);
// Persistent session worker must already have acknowledged its begin request.
Sio2Result executeSio2Transfer(IopHost& host,uint32_t packet,Sio2Worker& worker,
                              Sio2Device* device,uint32_t& events);
}
