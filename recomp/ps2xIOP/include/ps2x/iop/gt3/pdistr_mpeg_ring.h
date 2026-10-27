#pragma once
#include "ps2x/iop/iop_host.h"
#include <memory>
#include <optional>
#include <stop_token>

namespace ps2x::iop::gt3 {
// Native owner for an embedded MPEG ring, using the audited LIBPDI65/81/82
// descriptor layout. Not a generic LIBPDI export or a guest longjmp model.
// Caller exclusively retains descriptor/buffer allocations until stop and join.
class PdistrMpegRing {
public:
    struct Snapshot { uint32_t capacity=0,filled=0,producer=0,consumer=0; bool ended=false; };
    explicit PdistrMpegRing(IopHost&);
    ~PdistrMpegRing();
    bool initialize(uint32_t descriptor,uint32_t buffer,uint32_t capacity);
    // Writes only prefix; the remainder of advance retains old ring bytes.
    bool produce(std::span<const uint8_t> prefix,uint32_t advance,std::stop_token={});
    // Wait for contiguous writable space without consuming/publishing bytes.
    // This is not a reservation: the owner must use one producer per ring.
    std::optional<uint32_t> waitWritable(uint32_t minimum,std::stop_token={});
    // Empty successful read means a drained end-of-stream. nullopt is failure
    // or cancellation; no bytes are consumed on a failed atomic commit.
    std::optional<std::vector<uint8_t>> readAvailable(uint32_t maximum,std::stop_token={});
    // One ready contiguous chunk <=4096 bytes,16-byte-aligned EE delivery and
    // descriptor consumption in one batch. Fragmented DMA remains unsupported.
    bool consumeToEe(uint32_t destination,uint32_t bytes,std::stop_token={});
    enum class TransferResult { Complete, End, Stopped, Failed };
    struct Transfer { TransferResult result=TransferResult::Failed; uint32_t bytes=0; };
    // Original MPEG RPC2's <=0x8000-byte request, in complete 4096-byte records.
    // Sole consumer. Each record commits atomically; a later failure reports
    // prior verified bytes. A short EOF leaves the unused EE tail untouched.
    // Failed transfers must be retired by the owner, not blindly retried: a
    // post-commit verification failure may have already changed guest memory.
    Transfer consumeRecordsToEe(uint32_t destination,uint32_t bytes,std::stop_token={});
    bool finish();
    bool cancel();
    void stop();
    std::optional<Snapshot> snapshot() const;
private:
    struct State;
    std::unique_ptr<State> state_;
};
}
