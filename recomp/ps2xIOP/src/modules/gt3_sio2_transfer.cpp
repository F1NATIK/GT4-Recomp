#include "ps2x/iop/gt3/sio2_transfer.h"
#include <limits>
#include <utility>

namespace ps2x::iop::gt3 {
NativeExportResult applySio2DefaultSlotChange(IopHost& host,uint32_t address) {
    if (!address || (address&3u) || address>std::numeric_limits<uint32_t>::max()-31u) return {};
    std::array<uint8_t,32> input{};
    if (!host.readGuest(address,input.data(),input.size())) return {};
    std::array<uint8_t,16> available{};
    bool all=true;
    for (size_t i=0;i<4;++i) {
        const size_t p=i*4;
        const uint32_t slot=uint32_t(input[p])|(uint32_t(input[p+1])<<8)|
            (uint32_t(input[p+2])<<16)|(uint32_t(input[p+3])<<24);
        // Original ADDIU then SLTIU: unsigned wrapping addition, accepting only -1/0.
        const bool accepted=uint32_t(slot+1u)<2u;
        available[p]=static_cast<uint8_t>(accepted);
        all=all && accepted;
    }
    const std::array<IopMemoryWrite,1> writes{{{address+16,available}}};
    // IOP-only atomic write capability rejects EE pointers and invalid allocations.
    if (!host.writeIopMemoryBatch(writes)) return {};
    return {true,all ? 1 : 0};
}
namespace {
bool rangeFits(uint32_t address, uint32_t size) {
    return size == 0 || (address != 0 && size - 1 <= std::numeric_limits<uint32_t>::max() - address);
}
}
Sio2Result decodeSio2Transfer(const IopHost& host, uint32_t address, Sio2Transfer& result) {
    std::array<uint8_t,Sio2PacketSize> bytes{};
    if (!rangeFits(address,Sio2PacketSize) || !host.readGuest(address,bytes.data(),bytes.size()))
        return Sio2Result::InvalidMemory;
    auto word=[&](size_t offset) {
        return uint32_t(bytes[offset]) | (uint32_t(bytes[offset+1])<<8) |
               (uint32_t(bytes[offset+2])<<16) | (uint32_t(bytes[offset+3])<<24);
    };
    Sio2Transfer decoded;
    for (size_t i=0;i<4;++i) {
        decoded.portControl1[i]=word(4+i*4);
        decoded.portControl2[i]=word(0x14+i*4);
    }
    for (size_t i=0;i<16;++i) decoded.sendControl[i]=word(0x28+i*4);
    const auto inputSize=word(0x6c), inputAddress=word(0x74);
    decoded.outputSize=word(0x70); decoded.outputAddress=word(0x78);
    decoded.inputDma={word(0x7c),word(0x80),word(0x84)};
    decoded.outputDma={word(0x88),word(0x8c),word(0x90)};
    if (inputSize>Sio2MaxFifoBytes || decoded.outputSize>Sio2MaxFifoBytes) return Sio2Result::SizeLimit;
    if (!rangeFits(inputAddress,inputSize) || !rangeFits(decoded.outputAddress,decoded.outputSize))
        return Sio2Result::InvalidMemory;
    decoded.input.resize(inputSize);
    if (inputSize && !host.readGuest(inputAddress,decoded.input.data(),inputSize)) return Sio2Result::InvalidMemory;
    // Verify that the receive range exists without modifying it. The eventual
    // worker must also validate write access when committing a completion.
    std::vector<uint8_t> previousOutput(decoded.outputSize);
    if (decoded.outputSize && !host.readGuest(decoded.outputAddress,previousOutput.data(),previousOutput.size()))
        return Sio2Result::InvalidMemory;
    auto dmaSize=[](const Sio2Dma& dma, uint32_t& size) {
        size=0;
        if (!dma.address) return Sio2Result::Ok;
        if (dma.address & 3u) return Sio2Result::InvalidDma;
        // sceSetSliceDMA packs the low 16 bits of size/count into BCR.
        const uint64_t bytes=uint64_t(dma.blockSize & 0xffffu)*(dma.blockCount & 0xffffu)*4u;
        if (bytes>Sio2MaxDmaBytes) return Sio2Result::SizeLimit;
        size=static_cast<uint32_t>(bytes);
        return rangeFits(dma.address,size) ? Sio2Result::Ok : Sio2Result::InvalidMemory;
    };
    uint32_t dmaInputSize=0;
    auto dmaResult=dmaSize(decoded.inputDma,dmaInputSize);
    if (dmaResult!=Sio2Result::Ok) return dmaResult;
    dmaResult=dmaSize(decoded.outputDma,decoded.dmaOutputSize);
    if (dmaResult!=Sio2Result::Ok) return dmaResult;
    decoded.dmaInput.resize(dmaInputSize);
    if (dmaInputSize && !host.readGuest(decoded.inputDma.address,decoded.dmaInput.data(),dmaInputSize))
        return Sio2Result::InvalidMemory;
    std::vector<uint8_t> dmaPrevious(decoded.dmaOutputSize);
    if (decoded.dmaOutputSize && !host.readGuest(decoded.outputDma.address,dmaPrevious.data(),dmaPrevious.size()))
        return Sio2Result::InvalidMemory;
    result=std::move(decoded);
    return Sio2Result::Ok;
}
Sio2Result runSio2Fifo(const Sio2Transfer& request, Sio2Device* device, Sio2Completion& completion) {
    if (request.input.size()>Sio2MaxFifoBytes || request.outputSize>Sio2MaxFifoBytes) return Sio2Result::SizeLimit;
    // SIO2MAN tests the DMA address, not the other two descriptor words.
    if (request.inputDma.address || request.outputDma.address) return Sio2Result::DmaUnsupported;
    if (!device) return Sio2Result::DeviceUnsupported;
    Sio2Completion staged;
    if (!device->exchange(request,staged)) return Sio2Result::DeviceUnsupported;
    if (staged.output.size()!=request.outputSize) return Sio2Result::InvalidDeviceReply;
    completion=std::move(staged);
    return Sio2Result::Ok;
}
}

namespace ps2x::iop::gt3 {
Sio2Worker::Action Sio2Worker::advance() {
    if (phase_==Phase::Idle) {
        // Instruction order gives priority 1, 0x40, 0x100, 4, 0x10.
        for (const uint32_t request : {1u,0x40u,0x100u,4u,0x10u}) {
            if (events_ & request) {
                events_=(events_ & ~request) | (request<<1);
                phase_=Phase::Ready;
                break;
            }
        }
    }
    if (phase_==Phase::Ready) {
        if (events_ & 0x1000u) {
            events_ &= ~0x1000u;
            phase_=Phase::Idle;
        } else if (events_ & 0x400u) {
            events_ &= ~0x400u;
            phase_=Phase::WaitingForInterrupt;
            return Action::StartTransfer;
        }
    } else if (phase_==Phase::WaitingForInterrupt && (events_ & 0x2000u)) {
        events_ &= ~0x2000u;
        phase_=Phase::Committing;
        return Action::CollectResult;
    }
    return Action::None;
}
bool Sio2Worker::finishCommit(bool success) {
    if (phase_!=Phase::Committing) return false;
    if (!success) { phase_=Phase::Faulted; return false; }
    events_ |= 0x800u;
    phase_=Phase::Ready;
    return true;
}
void Sio2Worker::cancelTransfer() {
    if (phase_==Phase::WaitingForInterrupt || phase_==Phase::Committing) {
        events_ &= ~(0x400u|0x800u|0x2000u);
        phase_=Phase::Ready;
    }
}
}

namespace ps2x::iop::gt3 {
Sio2Result executeSio2Transfer(IopHost& host, uint32_t packet, uint32_t sessionRequest,
                              Sio2Device* device, uint32_t& events) {
    events=0;
    if (sessionRequest!=1 && sessionRequest!=4 && sessionRequest!=0x10 &&
        sessionRequest!=0x40 && sessionRequest!=0x100) return Sio2Result::InvalidSession;
    Sio2Worker worker;
    worker.signal(sessionRequest); worker.advance(); worker.clear(sessionRequest<<1);
    return executeSio2Transfer(host,packet,worker,device,events);
}
Sio2Result executeSio2Transfer(IopHost& host,uint32_t packet,Sio2Worker& worker,
                              Sio2Device* device,uint32_t& events) {
    events=0;
    if (worker.phase()!=Sio2Worker::Phase::Ready) return Sio2Result::InvalidSession;
    Sio2Transfer request;
    auto result=decodeSio2Transfer(host,packet,request);
    if (result!=Sio2Result::Ok) return result;
    // Packet/output overlap would modify the original module's readback pointers.
    // Reject this unsupported layout rather than guessing its MMIO ordering.
    auto overlapsPacket=[&](uint32_t address,uint32_t size) {
        return size && uint64_t(address)<uint64_t(packet)+Sio2PacketSize && uint64_t(packet)<uint64_t(address)+size;
    };
    if (overlapsPacket(request.outputAddress,request.outputSize) ||
        overlapsPacket(request.outputDma.address,request.dmaOutputSize)) return Sio2Result::InvalidMemory;
    if (!device) return Sio2Result::DeviceUnsupported;
    // Completion from an earlier exchange cannot acknowledge this request.
    worker.clear(0x800u|0x2000u);
    worker.signal(0x400);
    if (worker.advance()!=Sio2Worker::Action::StartTransfer) return Sio2Result::InvalidSession;
    struct CancelOnFailure {
        Sio2Worker& worker;
        ~CancelOnFailure() { worker.cancelTransfer(); }
    } cancel{worker};
    Sio2Completion completion;
    if (!device->exchange(request,completion)) return Sio2Result::DeviceUnsupported;
    if (completion.output.size()!=request.outputSize || completion.dmaOutput.size()!=request.dmaOutputSize)
        return Sio2Result::InvalidDeviceReply;
    if (worker.phase()!=Sio2Worker::Phase::WaitingForInterrupt) return Sio2Result::InvalidSession;
    worker.signal(0x2000);
    if (worker.advance()!=Sio2Worker::Action::CollectResult) return Sio2Result::InvalidSession;
    std::array<std::array<uint8_t,4>,3> status{};
    for (size_t i=0;i<3;++i) for (size_t j=0;j<4;++j)
        status[i][j]=static_cast<uint8_t>(completion.receiveStatus[i]>>(8*j));
    // DMA12 drains output first. The module subsequently stores RECV1/2/3
    // (0,0x24,0x68), then copies the remaining FIFO output (0x33c..0x39c).
    const std::array<IopMemoryWrite,5> writes{{
        {request.outputDma.address,completion.dmaOutput},
        {packet,status[0]}, {packet+0x24,status[1]}, {packet+0x68,status[2]},
        {request.outputAddress,completion.output}
    }};
    const bool committed=host.writeIopMemoryBatch(writes);
    worker.finishCommit(committed);
    events=worker.events();
    return committed ? Sio2Result::Ok : Sio2Result::CommitFailed;
}
}
