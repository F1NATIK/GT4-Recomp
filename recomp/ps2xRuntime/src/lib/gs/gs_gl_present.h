#pragma once

#include <cstdint>

// Frames composed on the GPU by the OpenGL renderer (GS worker thread) and
// shown by the host window (main thread) without a copy through the CPU.
// Three slots: the worker composes into one the host is not reading, fences
// it and publishes it; the host waits for that fence on its own context,
// copies the texture into its display texture and fences the copy, which the
// worker waits for before composing into that slot again. Both contexts share
// objects (gs_gl_context).
namespace gsgl
{
    // Internal resolution: each GS pixel drawn as renderScale() x renderScale()
    // samples (GT3_GS_GL_SCALE=1..4; 1, the default, is the exact native
    // renderer). GT3_GS_GL_PROGRESSIVE=1 doubles the sample rows again, so
    // each interlaced field is drawn as a whole frame (progressive output).
    // These are the requested values: they start from the environment and can
    // change while the game runs (requestRenderScale, from the host's live
    // settings); the renderer switches at its next presentation.
    int renderScale();
    bool progressiveScan();
    void requestRenderScale(int scale, bool progressive);
    // How interlaced fields are shown: 0 adaptive (the clamped weave; GT4:
    // "clamp"), 1 weave, 2 bob, 3 double, 4 blend (woven; the host's picture
    // pass blends the lines), 5 motion adaptive (GT4: "adaptive")
    // (GT3_DEINTERLACE, GT4 builds GT4_DEINTERLACE; live like the scale).
    int deinterlaceMode();
    void requestDeinterlace(int mode);

    // Wide pictures (GT3_ASPECT from about 2:1): a requested scale split into
    // samples across and rows per display line with about as many samples
    // per GS pixel, more of them across (4 at 48:9: 8 x 2; native stays 1 x
    // 1). Otherwise scale x scale. GT3_GS_GL_WIDE_SPLIT=0 turns it off.
    void wideScaleSplit(int scale, int &scaleX, int &scaleY);

    // The scale the renderer composes frames at now, across and down (the
    // worker sets it when it switches); before the first frame, the split of
    // renderScale().
    int presentScale();
    int presentScaleY();
    void setPresentScale(int scaleX, int scaleY);

    // Worker: the texture (640x512 RGBA8 times presentScale() x presentScaleY(),
    // taller when presentRows() asks for more rows) to compose the next frame into.
    unsigned beginPresentSlot();
    // Worker: display rows (native) the next frames need, before
    // beginPresentSlot: 512 or less keeps the 640x512 slots; 1080i's 960-row
    // picture asks for 1024 (slots grow and stay grown, so 480i frames after a
    // 1080i race reuse them). The Vulkan presentation image follows it too.
    void setPresentRows(uint32_t rows);
    uint32_t presentRows();
    // Worker: the frame in that slot is complete (width x height of it valid).
    void endPresentSlot(uint32_t width, uint32_t height);
    // Host: copies the newest published frame into dstTexture (dstWidth x
    // dstHeight) if there is one it has not copied yet: 1 copied (width x
    // height of it valid), 0 no new frame, 2 the frame does not fit
    // (frameScale / frameScaleY: the scale it was composed at; the host resizes
    // its texture to 640x512 times that and asks again).
    // maxTick (the host loop's grid pacing): the newest such frame whose
    // latch tick is <= maxTick, so a frame published early stays for the next
    // present; when there is none, the newest.
    int hostCopyLatestFrame(unsigned dstTexture, uint32_t dstWidth, uint32_t dstHeight, uint32_t &width, uint32_t &height, int &frameScale,
                            int &frameScaleY, uint64_t maxTick = ~0ull);
    // Host: a GPU frame was published since startup (the renderer is active).
    bool gpuPresentationActive();

    // Worker (the Vulkan renderer's own presenter, gs_vk_present.h): what
    // notePresentTick / notePresentField noted for the frame being composed.
    uint64_t composingTick();

    // Worker: the latch tick of the frame being composed
    // (ps2x::vblank::composingPresentation; called before endPresentSlot, the
    // slot keeps it).
    void notePresentTick(uint64_t tick);

    // How the frame being composed holds its fields (the host's scanline
    // pass: field emphasis). parity: the display-line parity of the newest
    // field's rows (0 even, 1 odd) for an interlaced field picture, -1 for
    // a frame without fields. mode: the presentation mode (0 whole frame,
    // 1 field line-doubled, 2 weave, 3 bob, 4 adaptive, 5 progressive, 6 motion
    // adaptive).
    // crt: the frame's video mode (runtime/gs/gs_crt.h; 0 unknown); lines:
    // its display lines (448 for 480i / 480p, 960 for 1080i; 0 unknown).
    struct FieldInfo
    {
        int parity = -1;
        int mode = 0;
        uint32_t crt = 0;
        int lines = 0;
    };
    // Worker: before endPresentSlot, like notePresentTick (the slot keeps it).
    void notePresentField(int parity, int mode, uint32_t crt = 0, int lines = 0);
    // Host: the field information of the frame hostCopyLatestFrame copied
    // last ({-1, 0} before the first copy).
    FieldInfo copiedFrameField();
    // Worker: the field information noted for the frame being composed (composingTick).
    FieldInfo composingField();
}
