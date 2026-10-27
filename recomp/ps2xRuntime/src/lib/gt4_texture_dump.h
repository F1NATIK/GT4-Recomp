#pragma once

#include <cstdint>
#include <string>
#include <vector>

// GT4 texture dump (GT4_TEXTURE_DUMP=<folder>; development and Full GT4.exe only, the Classic build has none):
// every texture GT4 draws with from GS memory (not a picture it drew itself: render targets, the mirror, post
// effects), decoded with the palette bound at the draw, and saved once as a PNG. Dumper only, nothing is replaced.
// The GS renderer decodes and hashes (gs_gl_backend.cpp textureIdentity / dumpTexture, shared with GT3's dumper);
// this keeps each new texture in memory while GT4 draws with it, widening the texel rectangle its draws actually
// sample (touch), and writes it once unused for a while (about 3 s), at a scene change or at exit: only that
// rectangle, everything outside it transparent (GT4 declares power-of-two sizes and samples part of them; the rest
// of GS memory there holds other textures, palettes or old data). Folders by the car or course the texture's bytes
// came from (gt4_texture_owner.h: GT4's disc reads and its uploads into GS memory, matched by content):
//   cars\<car name>\race\       a car's textures first drawn in a race session (applyRaceDisplayMode 0x436640 ..
//                               applyMenuDisplayMode 0x436740): its body, livery, lights, cockpit
//   cars\<car name>\showroom\   the same car's textures first drawn outside races (car select, garage, dealers)
//   cars\<car name>\contact-sheet.png   all of that car's pictures on one sheet (made at exit)
//   tracks\<course name>\       a course's textures (its scenery, road, signage)
//   wheels\                     wheels and tyres (GT4.VOL wheel/, tire/: shared by cars, read after all of a race's)
//   other\                      the rest (HUD, menus, effects, and anything not tied to a car or course)
// <car name>: GT4's own full name ("Toyota Castrol Tom's Supra (JGTC) '00"), else the car's label or model code;
// <course name>: GT4's course name ("Trial Mountain Circuit"), else its label.
// Names: gt4-<hash of the sampled rectangle's texels, 16 hex>-<width>x<height>[-r<u0>_<v0>_<u1>_<v1>].png (-r: the
// sampled rectangle when not the whole texture), the same picture saved once
// across runs and folders (in the folder it was first seen in). index.csv in the folder: path, size, the sampled
// rectangle, format, GS address, first frame, "noisy" when much of the rectangle looks like random data, and the
// owner ("car:castrol_supra_00", "track:trialmt", "wheels" or empty).
// Alpha: the PS2's 0..0x80 (0x80 opaque) saved as 0..255. GT4_TEXTURE_DUMP_OPAQUE=1: also a copy without alpha
// in an _opaque\ folder next to each picture (for viewing; replacements need the alpha of the normal file, and
// texture replacement skips _opaque folders).
namespace gt4texdump
{
    bool enabled();
    // The hash a dumped picture is named by: width, height and the texels (PS2 alpha) with those outside u0..u1,
    // v0..v1 taken as 0. Texture replacement finds pictures by it (gt4_texture_replace.h).
    uint64_t maskedHash(const uint32_t *texels, int width, int height, int u0, int v0, int u1, int v1);
    // Seen this run (kept, written or dropped).
    bool known(uint64_t hash);
    // A decoded texture (RGBA8 in memory order R, G, B, A; PS2 alpha) by the hash of all its texels.
    void submit(uint64_t hash, int width, int height, std::vector<uint32_t> &&texels, const std::string &info);
    // A draw samples texels u0..u1, v0..v1 (inclusive) of a texture; frame: the renderer's frame counter.
    void touch(uint64_t hash, int u0, int v0, int u1, int v1, uint64_t frame);
    // The EE thread: a race session starts (true) or GT4 is back in its menus (false).
    void setRace(bool race);
    uint64_t written();
    void shutdown();
}
