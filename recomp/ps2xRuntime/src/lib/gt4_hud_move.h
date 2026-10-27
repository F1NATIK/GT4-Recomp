#pragma once

// GT4's own race HUD parts moved and resized (HUD hand-off 7 B, analysis/design/hud-pages/handoff7; Full edition only, never
// Classic). Off by default: with GT4_HUD_MOVE empty or absent nothing here runs and GT4's stream is not touched.
//   GT4_HUD_MOVE=<part>:<x>,<y>,<size %>;...   x, y: the part's centre in % of the HUD area (GT4's frame, 640 x 448); size
//   40..250 %, scaled about the centre. A part not listed stays where GT4 puts it. Parts:
//     position, lap, clock, besttimes, map      (labels move with their part; the map's car dots are drawn inside it)
//     panel                                     the bumper / bonnet views' dash (RaceDisplay +0x108)
//     fuel, warnings, gbar, gearspeed           the chase views' compact panel (+0xd58), one member drawing four pieces:
//                                               split by where each primitive draws (the pieces learned from GT4's own frames)
//     mirror                                    the rear-view mirror (Vulkan renderer): the road put back under GT4's mirror,
//                                               the mirror's picture drawn at the new place (same frame) before GT4's HUD
// How: on every race frame RaceDisplay's draw and the draws of the moved parts' members are bracketed with markers in GT4's
// packet chain (gt4_hud_pictures.cpp, the race pictures' markers with bit 0x100 set). The GS frontend (gs_frontend.cpp)
// hands each marker and each primitive to this file: inside a moved part's bracket every primitive's XY (and its scissor) is
// moved and scaled in GS frame pixels (after XYOFFSET) before it goes to the renderer. A part's place in GT4's layout (the
// centre the move starts from) is learned from GT4's own frames: each member's box, kept once it is the same (within 2
// pixels) for 20 frames running (or after 300 frames, the boxes so far), per frame shape. Until then: in GT4's 16:9 frame
// (GT4_ASPECT 16:9 or unset, one screen) the places measured on Autumn Ring Mini; in other shapes the part stays where GT4
// puts it. GT4_HUD_MOVE_LOG=1 (tests) prints what was learned and where the markers went.

#include <cstdint>
#include <string>
#include <vector>

struct GSPrimitiveBatch;

namespace gt4hudmove
{
    // GT4_HUD_MOVE lists at least one part (read once; false in the Classic build).
    bool on();

    // ---- EE thread (gt4_hud_pictures.cpp): whether the RaceDisplay member at this offset is bracketed on every frame.
    bool wantsMember(uint32_t off);

    // ---- GS frontend (gs_frontend.cpp; under its lock, in stream order)
    // A marker from GT4's stream (the low word: bit 0x100 a move-only frame; the low byte as gt4_hud_pictures.cpp).
    void gsMarker(uint32_t code);
    // A primitive about to go to the renderer: moved when it belongs to a moved part.
    void apply(GSPrimitiveBatch &batch);

    // ---- GS backend thread (gs_gl_backend.cpp): the mirror moved (not with GT4_MIRROR=page).
    bool mirrorMoved();
    // Where: its centre (frame pixels, inside GT4's frame), its scale, and the frame's size.
    bool mirrorPlace(float &cx, float &cy, float &scale, float &frameW, float &frameH);

    // ---- the race pictures (gt4_hud_pictures.cpp, the writer thread): GT4's place of each part the frontend has learned
    // (frame pixels) and, for a moved one, where it is drawn now.
    struct PartPlace
    {
        std::string part;
        float x0 = 0, y0 = 0, x1 = 0, y1 = 0; // GT4's place
        bool moved = false;
        float mx0 = 0, my0 = 0, mx1 = 0, my1 = 0; // where it is drawn (moved)
    };
    // The places on the last race picture's frame (empty: none learned / moves off). pieces: the compact panel's four pieces were split.
    void places(std::vector<PartPlace> &out, bool &pieces);
    // A rectangle (frame pixels) moved as the part would be (false: that part is not moved).
    bool movedRect(const char *part, float &x0, float &y0, float &x1, float &y1);
} // namespace gt4hudmove
