#pragma once

// Race pictures for the launcher's HUD layout page (HUD hand-off 7 A1, analysis/design/hud-pages/handoff7; Full edition
// only, never Classic). Once per race, 20 s after the phase first becomes racing (not replays, not while paused), GT4.exe
// saves three files to <GT4.exe folder>\hud-pictures\ (the newest 16 sets kept):
//   <yyyymmdd-hhmmss>-<course id>.jpg      the 3D picture with no HUD at all (the frame's first copy of GT4's 3D buffer,
//                                          TBP 0x1a40, read back as the mirror page does; GT4's mirror taken out with the
//                                          road kept under it), scaled to the game rectangle's shape, at most 1920 wide.
//   <...>-hud.jpg                          the same size: the presented game rectangle with GT4's HUD and none of ours
//                                          (gt4hud::draw skipped for that one frame).
//   <...>.json                             { "course", "view", "w", "h", "parts": { "position": [x, y, w, h], ... } }, in
//                                          fractions of the picture: position, lap, clock, besttimes, map, panel, mirror
//                                          (a part GT4 did not draw in that frame is left out). "members": every RaceDisplay
//                                          member's own box, "pieces": the separate pieces of a part drawn apart.
// How the parts' rectangles are found: on that frame each RaceDisplay member's draw call (ps2_runtime.cpp, around the
// call) is bracketed by a marker written into GT4's own VIF1 packet chain at its cursor (scratchpad +0x21f8, the chain
// 0x49fe28 / 0x4a1950 write): a DMA CNT tag with FLUSH and a DIRECT of one GIF A+D write to GS register 0x7f (a register
// the GS has no use for). The GS backend sees the markers in drawing order and takes the bounding box of the primitives
// drawn into the shown frame between them. The mirror's rectangle is its pass's scissor (gt4_mirror.h).
// Vulkan only (with OpenGL nothing is saved). GT4_HUD_PICTURES=off turns it off (on by default).

#include <atomic>
#include <cstdint>
#include <vector>

namespace gt4hudpic
{
    // On (Full edition, GT4_HUD_PICTURES not off): read once.
    bool enabled();

    // ---- EE thread (ps2_runtime.cpp)
    // Where GT4's scratchpad is (the packet chain's cursor lives there).
    void setScratchpad(uint8_t *spr);
    // Around every guest call while a capture is marking (marking() is a relaxed load: false on almost every frame). The
    // token from callEnter goes to callLeave.
    extern std::atomic<bool> g_markingNow; // (marking()'s flag: read on every guest call, so inline)
    inline bool marking() { return g_markingNow.load(std::memory_order_relaxed); }
    int callEnter(uint8_t *rdram, uint32_t target, uint32_t a0);
    void callLeave(uint8_t *rdram, int token);
    // HUD hand-off 7 C: a bracketed call (callEnter's token) that came back through an EE scheduler checkpoint (yielded) or
    // was left by a blocking wait (threw) has not finished: the guest resumes it later from the scheduler. Its bracket stays
    // open and is closed (callLeave) when the guest reaches its return address ret with the caller's stack pointer sp
    // (atDispatch, from the scheduler's loop and dispatched jumps). Before, the bracket closed at once and the rest of the
    // draw went outside it: GT4's moved parts showed at their own places for a frame now and then (stage B's open problem).
    // GT4_HUD_MOVE_DEFER=0 (tests) gives the old way back.
    void callSuspended(uint8_t *rdram, int token, uint32_t ret, uint32_t sp, bool threw);
    extern std::atomic<int> g_pendingNow; // (pendingReturns()'s count: read on every dispatch, so inline)
    inline bool pendingReturns() { return g_pendingNow.load(std::memory_order_relaxed) > 0; }
    void atDispatch(uint8_t *rdram, uint32_t pc, uint32_t sp);
    // (HUD hand-off 7 B, gt4_hud_move.h: with GT4_HUD_MOVE the same markers bracket RaceDisplay's draw and the moved parts'
    // members on every race frame, with bit 0x100 set in the code; the race pictures' own frames keep theirs without it.)
    // The RaceDisplay member a marker's code names (its low byte), 0 if none.
    uint32_t memberOffset(int index);
    // RaceDisplay's draw is about to run (a0 = the display): arms a capture when it is time.
    void raceDisplayDraw(uint8_t *rdram, uint32_t rd, uint64_t vsync);
    // The race camera's view (gt4cam: 0 bumper, 1 bonnet, 2 roof, 3 close chase, 4 chase, 5 far chase).
    void noteView(int view);
    // GT4 drew its mirror this frame at this rectangle (3D buffer pixels, 640 x 448).
    void noteMirror(int x, int y, int w, int h);
    // Whether the mirror's rectangle must be noted (and its road kept) for a capture.
    bool wantMirror();

    // ---- GS backend thread (gs_gl_backend.cpp, the Vulkan renderer)
    void vulkanSeen();
    // A capture is armed or its clean picture is still on its way: the backend reads the 3D buffer back at the copy.
    bool gsArmed();
    bool gsBusy();
    // The tag for this frame's read of the 3D buffer (the copy); 0: none wanted.
    uint32_t sceneReadTag();
    // At the frame's RaceDisplay draw (the begin marker, after gsMarker): the shown buffer's frame and the tag for reading it
    // (GT4's colours, after the copy, before its HUD); 0: none.
    uint32_t shownReadTag(uint32_t &fbp, float &x0, float &y0, float &x1, float &y1);
    void shownReadDropped(); // (that read could not be started)
    // A finished read of the 3D buffer (rows of w RGBA pixels, top first), by its tag.
    void sceneRead(uint32_t tag, std::vector<uint8_t> &&pixels, int w, int h);
    // A marker from GT4's stream (GS register 0x7f): see gt4_hud_pictures.cpp.
    void gsMarker(uint32_t code);
    // A primitive drawn (GS frame pixels after XYOFFSET, clipped to the scissor) into frame buffer fbp.
    void gsPrimitive(uint32_t fbp, float x0, float y0, float x1, float y1);
    // The frame's copy of the 3D buffer into the shown buffer fbp (its rectangle in that buffer).
    void gsCopy(uint32_t fbp, float x0, float y0, float x1, float y1);

    // ---- presenter thread (ps2_runtime.cpp)
    // The game rectangle shown (window pixels), every frame.
    void noteGameRect(float w, float h);
    // This frame: skip our HUD and hand the window over (true once per capture).
    bool hudShotTurn();
    // The window as presented (RGBA rows, top first) and the game rectangle in it.
    void hudShot(const uint8_t *rgba, int sw, int sh, float x, float y, float w, float h);
} // namespace gt4hudpic
