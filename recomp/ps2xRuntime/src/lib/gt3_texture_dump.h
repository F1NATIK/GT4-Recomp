#pragma once

#include <cstdint>
#include <string>
#include <vector>

// Texture dump (GT3_TEXTURE_DUMP=<folder>): every texture the game draws with
// (from GS memory, not from a picture it drew itself: reflections, the
// mirror, post effects), decoded (palette applied) and saved once as a PNG,
// for upscaling and, later, replacement. The GS renderer decodes and hashes
// (gs_gl_backend.cpp dumpTexture); this writes the files on a thread of its
// own. Names: gt3-<hash of the decoded texels, 16 hex>-<width>x<height>.png;
// index.csv in the folder: hash, size, format, GS address, first frame.
// Alpha: the PS2's 0..0x80 (0x80 opaque) saved as 0..255 (above 0x80: 255).
namespace gt3texdump
{
    bool enabled();
    // Written (in this or an earlier run) or waiting to be.
    bool known(uint64_t hash);
    // A decoded texture (RGBA8 in memory order R, G, B, A; PS2 alpha).
    // `info` goes to index.csv. Dropped (not marked known) if too many wait.
    void submit(uint64_t hash, int width, int height, std::vector<uint32_t> &&texels, const std::string &info);
    // Files written this run (for the status line / logs).
    uint64_t written();
    void shutdown();
}
