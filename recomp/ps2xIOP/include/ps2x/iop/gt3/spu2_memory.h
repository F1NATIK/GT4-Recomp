#pragma once
#include <atomic>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>

namespace ps2x::iop::gt3 {
// Owned SPU2 RAM, distinct from both EE RAM and the synthetic IOP heap. All
// addresses here are sound-RAM byte offsets. These are memory primitives, not
// guest DMA exports: they do not imply voice execution, timing or interrupts.
// Unknown power-on contents are not exposed until explicitly written/cleared.
class Spu2Memory {
public:
    static constexpr uint32_t Size = 0x200000;
    Spu2Memory();
    ~Spu2Memory();
    Spu2Memory(const Spu2Memory&) = delete;
    Spu2Memory& operator=(const Spu2Memory&) = delete;
    bool read(uint32_t address, std::span<uint8_t> destination) const;
    bool write(uint32_t address, std::span<const uint8_t> source);
    bool clear(uint32_t address, uint32_t size);
    // Increments on every successful write/clear/PIO store. Readers may cache
    // bytes and re-read only when this changes (voice block-header cache).
    uint64_t generation() const;
    const std::atomic<uint64_t>& generationCounter() const;
    // TSA is word addressed in the hardware. This interface takes an even byte
    // offset; the LIBSD selector's additional alignment is handled by LIBSD.
    bool setTransferAddress(unsigned core, uint32_t address);
    std::optional<uint32_t> transferAddress(unsigned core) const;
    bool writePio(unsigned core, uint16_t value);
    // A port reset invalidates transfer addresses; it does not erase sound RAM.
    void resetTransferPorts();
private:
    struct State;
    std::unique_ptr<State> state_;
};
}
