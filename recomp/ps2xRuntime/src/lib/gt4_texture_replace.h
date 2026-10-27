#pragma once

#include <cstdint>
#include <vector>

// GT4 texture replacement (GT4_TEXTURE_REPLACE=<folder>; development and Full GT4.exe only, the Classic build has
// none): pictures named like the dump's files (gt4_texture_dump.h: gt4-<16 hex>-<w>x<h>[-r<u0>_<v0>_<u1>_<v1>].png,
// any subfolder, any size: a 2x or 4x upscale covers the same texture) are drawn instead of the texture whose sampled
// area holds the same texels. The hash in the name is of the dumped picture (the texels outside the sampled area
// cleared); the -r part names that area (none: the whole texture). The GS renderer decodes a texture the first time
// its contents are seen and asks here (rects / has); pictures are loaded on a thread of their own the first time one
// is needed (the original is drawn until then). Alpha: 0..255 in the file, the PS2's 0..0x80 in use; colours as
// dumped (raw texel values).
namespace gt4texreplace
{
    struct Rect
    {
        int u0, v0, u1, v1;
    };
    // The folder is set and holds at least one replacement.
    bool enabled();
    // The sampled areas named by replacements of a width x height texture (the whole texture included).
    const std::vector<Rect> &rects(int width, int height);
    // A file exists for this (dumped picture) hash.
    bool has(uint64_t hash);
    // The picture, once loaded: RGBA8 (R, G, B, A in memory order; PS2 alpha). True once per hash; false while
    // loading, for no file, or a file that could not be read. The first call starts the load.
    bool take(uint64_t hash, int &width, int &height, std::vector<uint32_t> &rgba);
    void shutdown();
}
