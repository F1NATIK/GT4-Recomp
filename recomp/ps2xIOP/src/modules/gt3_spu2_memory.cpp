#include "ps2x/iop/gt3/spu2_memory.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <mutex>
#include <vector>

namespace ps2x::iop::gt3 {
struct Spu2Memory::State {
    mutable std::mutex mutex;
    std::vector<uint8_t> bytes = std::vector<uint8_t>(Size);
    std::vector<uint8_t> known = std::vector<uint8_t>(Size);
    std::array<std::optional<uint32_t>, 2> tsa{};
    std::atomic<uint64_t> generation{0};
    static bool range(uint32_t address, size_t size) {
        return address <= Size && size <= Size-address;
    }
};
Spu2Memory::Spu2Memory(): state_(std::make_unique<State>()) {}
Spu2Memory::~Spu2Memory() = default;
bool Spu2Memory::read(uint32_t address, std::span<uint8_t> destination) const {
    if (!State::range(address, destination.size())) return false;
    auto& s = *state_; std::lock_guard lock(s.mutex);
    if (!std::all_of(s.known.begin()+address, s.known.begin()+address+destination.size(),
        [](uint8_t known) { return known != 0; })) return false;
    std::copy_n(s.bytes.begin()+address, destination.size(), destination.begin());
    return true;
}
bool Spu2Memory::write(uint32_t address, std::span<const uint8_t> source) {
    if (!State::range(address, source.size())) return false;
    auto& s = *state_; std::lock_guard lock(s.mutex);
    std::copy(source.begin(), source.end(), s.bytes.begin()+address);
    std::fill_n(s.known.begin()+address, source.size(), uint8_t(1));
    s.generation.fetch_add(1, std::memory_order_release);
    return true;
}
bool Spu2Memory::clear(uint32_t address, uint32_t size) {
    if (!State::range(address, size)) return false;
    auto& s = *state_; std::lock_guard lock(s.mutex);
    std::fill_n(s.bytes.begin()+address, size, uint8_t(0));
    std::fill_n(s.known.begin()+address, size, uint8_t(1));
    s.generation.fetch_add(1, std::memory_order_release);
    return true;
}
uint64_t Spu2Memory::generation() const { return state_->generation.load(std::memory_order_acquire); }
const std::atomic<uint64_t>& Spu2Memory::generationCounter() const { return state_->generation; }
bool Spu2Memory::setTransferAddress(unsigned core, uint32_t address) {
    if (core >= 2 || address >= Size || (address & 1)) return false;
    auto& s = *state_; std::lock_guard lock(s.mutex);
    s.tsa[core] = address; return true;
}
std::optional<uint32_t> Spu2Memory::transferAddress(unsigned core) const {
    if (core >= 2) return {};
    auto& s = *state_; std::lock_guard lock(s.mutex);
    return s.tsa[core];
}
bool Spu2Memory::writePio(unsigned core, uint16_t value) {
    if (core >= 2) return false;
    auto& s = *state_; std::lock_guard lock(s.mutex);
    if (!s.tsa[core]) return false;
    const auto address = *s.tsa[core];
    s.bytes[address] = uint8_t(value);
    s.bytes[address+1] = uint8_t(value >> 8);
    s.known[address] = s.known[address+1] = 1;
    s.tsa[core] = (address+2) & (Size-1);
    s.generation.fetch_add(1, std::memory_order_release);
    return true;
}
void Spu2Memory::resetTransferPorts() {
    auto& s = *state_; std::lock_guard lock(s.mutex);
    s.tsa = {};
}
}
