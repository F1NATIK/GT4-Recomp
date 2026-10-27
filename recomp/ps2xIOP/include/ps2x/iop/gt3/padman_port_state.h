#pragma once
#include "ps2x/iop/gt3/padman_publication.h"
#include <array>

namespace ps2x::iop::gt3 {
struct PadmanRequest {
    std::array<uint8_t,21> bytes{};
    uint32_t size=0;
    uint32_t portControl1=0xffc00505,portControl2=0;
    // SIO2 SEND3 of this port0 exchange (send and receive length).
    uint32_t sendControl() const { return (size<<18)|(size<<8)|0x40u; }
};
// The EE fills two actuator request slots by DMA at port+0x80/+0xa0 (open
// replies with their IOP address): word0 sequence, word1 pending, +0xc values.
inline constexpr size_t PadmanActSlotsOffset=0x80,PadmanActSlotsBytes=0x40;
// Native port0/slot0 model of the supplied PADMAN workers: read 23b4,
// discovery 2544, main mode 2a94, actuator alignment 2bcc, pressure mode 2d04
// and VREF 2ea8, in the main thread's frame order: the worker that built the
// exchange handles its reply, the frame is published, a requested worker
// starts, then the active worker builds the next exchange. Checked frame by
// frame against the original (audit_padman_ds2.py: a protocol-level virtual
// DualShock 2; audit_padman_recovery.py: the basic digital pad). The caller
// owns the bus, commits, lifecycle and frame events.
class PadmanPortState {
public:
    bool open(uint32_t eeDoubleBuffer);
    void reset();
    bool request(PadmanRequest&) const;
    // actSlots: port+0x80..+0xbf as the EE left them (empty: none). On success
    // *consumedSlot is the slot (0/1) whose pending word the caller must clear,
    // else -1.
    bool complete(uint32_t receiveStatus,std::span<const uint8_t> response,PadmanPublication&,
                  std::span<const uint8_t> actSlots={},int* consumedSlot=nullptr);
    // Original RPC3..11 functions for port0/slot0:
    // 3d1c 3e1c 3f08 40f4 4040 41a4 4404 4320 4258.
    int32_t infoAct(int32_t actuator,int32_t term) const;
    int32_t infoComb(int32_t list,int32_t offset) const;
    int32_t infoMode(int32_t term,int32_t offset) const;
    int32_t setMainMode(uint32_t mode,uint32_t lock);
    int32_t setActDirect(std::span<const uint8_t,6> values);
    int32_t setActAlign(std::span<const uint8_t,6> align);
    uint32_t buttonMask() const;
    int32_t setButtonInfo(uint32_t mask);
    int32_t setVrefParam(std::span<const uint8_t,12> values);
    bool opened() const { return worker_!=Worker::Closed; }
    const std::array<uint8_t,PadmanPortBytes>& portState() const { return port_; }
private:
    enum class Worker : uint8_t { Closed,Read,Detect,MainMode,ActAlign,PressMode,Vref };
    enum class Op : uint8_t { Poll,Probe,EnterConfig,Model,ActInfo,CombInfo,ModeInfo,Align,MaskQuery,
        ExitConfig,SetMainMode,SetMask,SetVref,Verify };
    Worker worker_=Worker::Closed;
    Op op_=Op::Poll;
    uint8_t index_=0,attempts_=0,id_=0;
    uint32_t readErrors_=0;
    bool startDetection_=false;
    std::array<uint8_t,PadmanPortBytes> port_{};
    PadmanRequest pending_;
    bool handle(bool ok,std::span<const uint8_t> response);
    void handleDetect(bool ok,std::span<const uint8_t> response);
    void handleWorker(bool ok,std::span<const uint8_t> response);
    bool retry(bool succeeded);
    void exitWorker(size_t failField=0);
    void afterDetectModel();
    void nextDetectTable();
    void beginDetection();
    void beginWorker(Worker);
    void build(std::span<const uint8_t> actSlots,int* consumedSlot);
};
}
