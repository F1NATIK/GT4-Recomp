#pragma once

// GT4's rear-view mirror on a page (HUD hand-off 6 B, hud/HANDOFF-6-LEFTOVERS-AND-MIRROR-PAGE.md; Full edition only):
// the pages service (gt4_pages.h) serves GET /mirror.mjpg (an MJPEG stream) and /mirror.jpg (one picture) of the mirror
// as GT4 draws it, for telemetry.html#mirror on a phone or another monitor. Read-only on the game. Nothing is captured
// while nobody watches.
//   - GT4 draws its mirror into its 3D buffer (FBP 0x0d2, 640 x 448) over the road, at the rectangle its race render
//     keeps at render object +0xe2e0 (190, 20, 260 x 58 in 16:9; narrower in the wider shapes): noted on the EE thread
//     at the race view's draw call (0x33f0a4 / 0x33f080), which GT4 makes only in a frame with the mirror.
//   - The GS backend, at the frame's first copy of the 3D buffer (TBP 0x1a40) into another buffer, reads that rectangle
//     of the 3D buffer's texture (at the internal scale) back without waiting for the GPU (vkgl::readStart /
//     readTake: Vulkan renderer only); the pictures are JPEG'd on a thread of their own, up to 30 a second.

//   GT4_MIRROR=screen (default) | both | page (HUD hand-off 6 B3; 21 Oct, developer): where GT4's mirror shows on the game
//   screen. screen: as GT4 has it, the bumper view only. both: every view of the player's camera. page: none (the road
//   under it). The page (/mirror.mjpg) gets the rear view in every view whichever it is, while a viewer watches: GT4 is
//   made to draw its mirror in the other views too (camera +0x26d), and with screen those page-only mirrors are drawn one
//   row shorter so the GS thread can tell them from GT4's own in the stream (scissorKind) and put the road back over them.

#include <cstdint>
#include <memory>
#include <vector>

namespace gt4mirror
{
    // GT4_MIRROR: 0 screen, 1 both, 2 page.
    int mode();
    // The rectangle of the last mirror GT4 drew (3D buffer pixels; false: none yet), without taking it.
    bool lastRect(int &x, int &y, int &w, int &h);
    // The pages service: a viewer of /mirror.mjpg came / went, or /mirror.jpg was asked for (the page's still-picture mode:
    // capturing for 2 s after). Captures run while one or more watch.
    void viewerJoin();
    void viewerLeave();
    void stillAsked();
    bool wanted();
    // EE thread: GT4 draws its mirror this frame at the rectangle at `rect` in EE RAM (render object +0xe2e0: x, y, w, h
    // floats in the 640 x 448 3D buffer).
    void noteDrawn(uint8_t *rdram, uint32_t rect, uint64_t vsync);
    // EE thread, at the player's camera update: this frame's mirror is drawn only for the page (a view GT4 draws none in;
    // GT4_MIRROR=screen: drawn one row shorter, see scissorKind). forcing(): put GT4's mirror in every view now (both, page,
    // or screen while a page watches).
    void setCamera(uint32_t cam); // (the player's on-board camera; 0 none)
    bool forcing();
    // GS thread: a draw's scissor against the mirror's rectangle: 0 not the mirror, 1 GT4's own mirror, 2 one drawn only for
    // the page (GT4_MIRROR=screen: kept off the game screen).
    int scissorKind(int x0, int y0, int x1, int y1);
    // GS thread: the mirror's rectangle (3D buffer pixels) when GT4 drew it since the last call that returned true (one
    // capture per GT4 frame); false otherwise.
    bool takeRect(int &x, int &y, int &w, int &h);
    // GS thread: a read-back picture (4 bytes a pixel as the 3D buffer holds it, rows top first) for the encoder.
    void submit(std::vector<uint8_t> &&pixels, int w, int h);
    // The pages service: the newest JPEG after `serial` (updated), waiting up to `ms`; null when none came.
    std::shared_ptr<const std::vector<uint8_t>> nextJpeg(uint64_t &serial, int ms);
    // The newest JPEG (null: none yet).
    std::shared_ptr<const std::vector<uint8_t>> lastJpeg();
    // Milliseconds since the newest JPEG was made (-1: none yet): the feed's "mirror_ms", for the page's "the mirror
    // shows in the bumper view" card.
    int64_t lastAgeMs();
} // namespace gt4mirror
