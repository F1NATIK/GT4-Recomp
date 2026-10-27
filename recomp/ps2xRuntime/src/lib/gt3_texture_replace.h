#pragma once

#include <cstdint>
#include <vector>

// Texture replacement (GT3_TEXTURE_REPLACE=<folder>): pictures named like the
// dump's files (gt3_texture_dump.h: gt3-<16 hex>-<w>x<h>.png, any size, any
// subfolder; other image types raylib reads work too) are drawn instead of
// the texture with that hash. The GS renderer identifies textures
// (gs_gl_backend.cpp textureIdentity) and asks here; the pictures are loaded
// on a thread of their own the first time one is needed (the original is
// drawn until then). Alpha: 0..255 in the file, the PS2's 0..0x80 in use.
namespace gt3texreplace
{
    // The folder is set and holds at least one replacement.
    bool enabled();
    // A file exists for this hash.
    bool has(uint64_t hash);
    // The picture, once loaded: RGBA8 (R, G, B, A in memory order; PS2 alpha).
    // True once per hash (the caller keeps it); false while loading, for no
    // file, or a file that could not be read. The first call starts the load.
    bool take(uint64_t hash, int &width, int &height, std::vector<uint32_t> &rgba);
    // Replacements found in the folder.
    size_t count();
    void shutdown();
}
