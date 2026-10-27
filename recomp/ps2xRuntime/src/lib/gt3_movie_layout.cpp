#include "runtime/gt3_movie_layout.h"

namespace ps2x::video {
bool makeGt3MovieLayout(uint32_t width, uint32_t codedHeight,
                       uint32_t mode, uint32_t displayState, Gt3MovieLayout& out) {
    out = {};
    if (!width || !codedHeight || width > 4096 || codedHeight > 4096 ||
        width % 16 || codedHeight % 16 || (mode == 1 && codedHeight % 32)) return false;
    out.width = width;
    out.height = mode == 1 ? codedHeight / 2 : codedHeight;
    out.sourceY = mode == 1 && displayState == 3 ? out.height : 0;
    out.bufferWidth = (width + 63) & ~63u;
    out.tiles.reserve(size_t(width / 16) * (out.height / 16));
    for (uint32_t x = 0; x < width; x += 16)
        for (uint32_t y = out.sourceY; y < out.sourceY + out.height; y += 16)
            out.tiles.push_back({x, y - out.sourceY,
                (size_t(x / 16) * (codedHeight / 16) + y / 16) * 1024});
    return true;
}
}
