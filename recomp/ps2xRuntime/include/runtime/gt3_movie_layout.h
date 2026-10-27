#pragma once
#include <cstddef>
#include <cstdint>
#include <vector>

namespace ps2x::video {
struct Gt3MovieTile {
    uint32_t x = 0, y = 0;
    size_t rgbaOffset = 0; // Original column-major 16x16 RGBA macroblocks.
    bool operator==(const Gt3MovieTile&) const = default;
};
struct Gt3MovieLayout {
    uint32_t width = 0, height = 0, sourceY = 0, bufferWidth = 0;
    std::vector<Gt3MovieTile> tiles;
};
// CORE 1cd168 -> 1ccec0/1cd048. Mode 1 selects a half-picture;
// display state 3 selects the lower half, all other states the upper half.
// State names deliberately do not assert physical odd/even field parity.
// Only complete 16x16 tiles are supported; failure clears the output.
bool makeGt3MovieLayout(uint32_t width, uint32_t codedHeight,
                       uint32_t mode, uint32_t displayState, Gt3MovieLayout& out);
}
