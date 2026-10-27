#ifndef PS2_GS_FRONTEND_H
#define PS2_GS_FRONTEND_H

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <vector>

#include "runtime/gs/gs_backend.h"

struct GSDebugSnapshot
{
    GSContext ctx[2]{};
    GSPrimReg prim{};
    GSTexaReg texa{};
    GSTexClutReg texclut{};
    uint64_t scanmsk = 0;
    uint64_t dimx = 0;
    uint64_t dthe = 0;
    uint64_t colclamp = 0;
    GSBitBltBuf bitbltbuf{};
    GSTrxPos trxpos{};
    GSTrxReg trxreg{};
    uint32_t trxdir = 0;
    uint32_t transferX = 0;
    uint32_t transferY = 0;
    uint32_t transferTotalPixels = 0;
    uint32_t transferCopiedPixels = 0;
    uint32_t lastDisplayBaseBytes = 0;
    GSFrameReg preferredDisplaySourceFrame{};
    uint32_t preferredDisplayDestFbp = 0;
    bool hasPreferredDisplaySource = false;
    uint32_t hostPresentationWidth = 0;
    uint32_t hostPresentationHeight = 0;
    uint32_t hostPresentationDisplayFbp = 0;
    uint32_t hostPresentationSourceFbp = 0;
    bool hostPresentationUsedPreferred = false;
    bool hasHostPresentationFrame = false;
    size_t localToHostPendingBytes = 0;
};

enum class GSDebugEventKind : uint8_t
{
    GifTag = 0,
    Register = 1,
    Draw = 2,
    Transfer = 3,
    Present = 4,
};

struct GSDebugHistoryEntry
{
    uint64_t seq = 0;
    uint64_t vsyncTick = 0;
    uint32_t frameIndex = 0;
    GSDebugEventKind kind = GSDebugEventKind::Register;

    uint8_t reg = 0;
    uint64_t regValue = 0;

    uint32_t gifSizeBytes = 0;
    uint32_t gifNloop = 0;
    uint8_t gifFlg = 0;
    uint8_t gifNreg = 0;

    GSPrimReg prim{};
    GSFrameReg frame{};
    GSZbufReg zbuf{};
    GSTex0Reg tex0{};
    GSScissorReg scissor{};
    uint64_t test = 0;
    uint64_t alpha = 0;

    uint32_t vertexCount = 0;
    float xMin = 0.0f;
    float xMax = 0.0f;
    float yMin = 0.0f;
    float yMax = 0.0f;
    double zMin = 0.0;
    double zMax = 0.0;
    uint8_t aMin = 0;
    uint8_t aMax = 0;

    GSBitBltBuf bitbltbuf{};
    GSTrxPos trxpos{};
    GSTrxReg trxreg{};
    uint32_t trxdir = 0;
    uint32_t transferPixels = 0;

    uint32_t displayFbp = 0;
    uint32_t sourceFbp = 0;
    uint32_t width = 0;
    uint32_t height = 0;
    bool usedPreferred = false;
};

class GS
{
public:
    GS();
    ~GS() = default;

    void init(uint8_t *vram, uint32_t vramSize, struct GSRegisters *privRegs = nullptr);
    void reset();
    // Wide pictures (GT3_ASPECT, gt3_aspect.cpp): a factor below 1 squeezes
    // the 2D drawn into the displayed frame buffers toward the screen's
    // centre, so the HUD keeps its shape when the frame is shown wider.
    static void setHudSqueeze(float factor);
    // GT3's race view, which decides its HUD layout (0 bumper: gauges in the
    // middle; 1 chase: rev counter at the right; -1 not known), for the wide
    // HUD's anchoring to the screen's sides (gt3_camera.cpp sets it).
    static void setHudView(int view);
    // Triple screens (wider than 2.5:1): the middle screen's share of the
    // picture's width, where the HUD's sides go (0: three 16:9 screens, no
    // bezel correction). Set from GT3_TRIPLE_MONITOR_ASPECT / _BEZEL.
    static void setHudMiddleScreen(float fraction);
    // GT3's race HUD groups hidden (GT3_HUD_HIDE / live key hud_hide; the
    // launcher's HUD page), only while a race is on (setHudRace, from the
    // race camera): the user's own overlays can take their place.
    enum HudHide : uint32_t
    {
        HudHideLeft = 1u,   // position, track map
        HudHideRight = 2u,  // lap times
        HudHideGauges = 4u, // speedometer, rev counter, gear, lights (the bottom band)
        HudHideMirror = 8u  // the rear-view mirror and its frame
    };
    static void setHudHide(uint32_t groups);
    static void setHudRace(bool racing);
    // GT3's messages in the middle of the screen (2D, not the side columns, the
    // bottom band or a results panel) hidden while on, race or not: set by the
    // grid-start lights (gt3_start_lights.cpp) only while GT3's "3 2 1 START"
    // countdown is up, so the lights take its place.
    static void setHudHideMiddle(bool on);
    // An inset 3D view (GT3's rear-view mirror: depth-tested drawing within a
    // scissor narrower than its target, around the middle): once watching is
    // on, the scissor rectangle (x0, y0, x1, y1, as drawn) of the last one
    // and the VSync it was drawn in (gt3_phone_view.cpp crops it).
    static void setInsetWatch(bool on);
    static bool insetView(uint16_t rect[4], uint64_t &tick);
    void setRasterBackend(std::unique_ptr<GSRasterBackend> backend);

    void processGIFPacket(const uint8_t *data, uint32_t sizeBytes);
    bool processNativePackedGIFPacket(const uint8_t *data, uint32_t sizeBytes);
    void uploadImageNative(uint64_t bitbltbuf,
                           uint64_t trxpos,
                           uint64_t trxreg,
                           uint64_t trxdir,
                           const uint8_t *data,
                           uint32_t sizeBytes);
    void writeRegister(uint8_t regAddr, uint64_t value);

    const uint8_t *lockDisplaySnapshot(uint32_t &outSize);
    void unlockDisplaySnapshot();
    uint32_t getLastDisplayBaseBytes() const;
    const GSFrameReg &getContextFrame(int index) const
    {
        return m_ctx[(index != 0) ? 1 : 0].frame;
    }
    GSDebugSnapshot getDebugSnapshot() const;
    std::vector<GSDebugHistoryEntry> getDebugHistory() const;
    void clearDebugHistory();
    bool isDebugHistoryPaused() const;
    void setDebugHistoryPaused(bool paused);
    bool getPreferredDisplaySource(GSFrameReg &outSource, uint32_t &outDestFbp) const;
    void latchHostPresentationFrame();
    // The display registers as the EE had them at a VSync.
    struct DisplaySnapshot
    {
        uint64_t pmode = 0, smode2 = 0, dispfb1 = 0, display1 = 0, dispfb2 = 0, display2 = 0, bgcolor = 0, vsyncTick = 0;
        uint32_t crt = 0; // the video mode SetGsCrt chose (runtime/gs/gs_crt.h)
    };
    // Latches the frame for a VSync from the GS stream (the GS worker runs it
    // in order with the packets, so contents and display registers match),
    // and counts it as published.
    void latchPresentationSnapshot(const DisplaySnapshot &snapshot);
    uint64_t publishedFrameCount() const { return m_publishedFrames.load(std::memory_order_acquire); }

    // Game frame rate from display-buffer flips (a new displayed FBP at a
    // VSync), over the last second: frames per second and the average and
    // longest time between flips (the FPS / frame time overlay).
    struct FrameStats
    {
        double fps = 0.0;
        double avgMs = 0.0;
        double maxMs = 0.0;
    };
    FrameStats frameStats() const;
    // The times between the latest flips, newest first, in ms (GT4's FPS counter graph,
    // gt4_fps_counter.h): at most `max` of them; returns how many were written.
    size_t recentFrameGapsMs(float *out, size_t max) const;
    // Diagnostic (GT3_GS_DRAW_LOG=teal:<n>): log the draws of the next frame.
    void requestDrawLog(uint64_t tick) { m_drawLogRequestTick.store(tick + 1u, std::memory_order_relaxed); }
    bool copyLatchedHostPresentationFrame(std::vector<uint8_t> &outPixels,
                                          uint32_t &outWidth,
                                          uint32_t &outHeight,
                                          uint32_t *outDisplayFbp = nullptr,
                                          uint32_t *outSourceFbp = nullptr,
                                          bool *outUsedPreferred = nullptr,
                                          uint32_t *outCrt = nullptr) const;
    bool clearFramebufferContext(uint32_t contextIndex, uint32_t rgba);
    bool clearActiveFramebuffer(uint32_t rgba);
    uint64_t nativeImageUploadCount() const { return m_nativeImageUploadCount; }
    uint64_t nativePackedGIFPacketCount() const { return m_nativePackedGIFPacketCount; }

    uint32_t consumeLocalToHostBytes(uint8_t *dst, uint32_t maxBytes);
    // Called with the CSR event bits (1 SIGNAL, 2 FINISH) the GS just set;
    // the runtime turns unmasked ones into the INTC GS interrupt.
    void setEventSink(std::function<void(uint32_t)> sink) { m_eventSink = std::move(sink); }

    void refreshDisplaySnapshot();
    // Completes every queued draw (the CPU backend rasterizes in batches).
    void flushRaster();

    void WriteVram(uint32_t psm, uint32_t base, uint32_t bw, uint32_t x, uint32_t y, uint32_t value);
    uint32_t ReadVram(uint32_t psm, uint32_t base, uint32_t bw, uint32_t x, uint32_t y) const;

private:
    void snapshotVRAM();
    void writeRegisterUnlocked(uint8_t regAddr, uint64_t value);
    void writeRegisterPacked(uint8_t regDesc, uint64_t lo, uint64_t hi);
    void uploadImageNativeUnlocked(uint64_t bitbltbuf,
                                   uint64_t trxpos,
                                   uint64_t trxreg,
                                   uint64_t trxdir,
                                   const uint8_t *data,
                                   uint32_t sizeBytes);
    void vertexKick(bool drawing);

    void recordDebugEventUnlocked(GSDebugHistoryEntry entry);
    GSDebugHistoryEntry makeDebugEventUnlocked(GSDebugEventKind kind) const;
    void recordGifTagDebugEventUnlocked(uint32_t sizeBytes, uint32_t nloop, uint8_t flg, uint32_t nreg);
    void recordRegisterDebugEventUnlocked(uint8_t regAddr, uint64_t value);
    void recordDrawDebugEventUnlocked(int vertexCount);
    void recordTransferDebugEventUnlocked();
    void recordPresentDebugEventUnlocked(uint32_t displayFbp, uint32_t sourceFbp, uint32_t width, uint32_t height, bool usedPreferred);

    void processImageData(const uint8_t *data, uint32_t sizeBytes);
    bool tryProcessNativeImageUploadPacket(const uint8_t *data, uint32_t sizeBytes);
    GSPrimitiveBatch buildDrawBatch(int vertexCount) const;
    // GT4's race colour (gs_fullcolour.h): the video mode the last draw saw and how many times it changed
    // (GSDrawState::fcEpoch; buildDrawBatch keeps them).
    mutable uint32_t m_fcCrt = 0u;
    mutable uint16_t m_fcEpoch = 0u;
    void updatePreferredDisplaySourceForDraw(const GSPrimitiveBatch &batch);
    GSPresentationRequest buildPresentationRequestUnlocked() const;


    GSContext &activeContext();

    uint8_t *m_localMemoryStorage = nullptr;
    uint32_t m_localMemorySize = 0u;
    struct GSRegisters *m_privRegs = nullptr;
    std::function<void(uint32_t)> m_eventSink;
    mutable std::recursive_mutex m_stateMutex;
    mutable std::mutex m_backendLifetimeMutex;
    mutable std::mutex m_presentationMutex;

    GSContext m_ctx[2];
    GSPrimReg m_prim{};
    GSPrimReg m_primRegister{};
    GSPrimReg m_prmodeRegister{};

    uint8_t m_curR = 0x80, m_curG = 0x80, m_curB = 0x80, m_curA = 0x80;
    float m_curQ = 1.0f;
    // GT4's VU1 tap: the current GIF tag's tap slot + 1 (tag bits 16-31, read only with the tap on) and the next
    // vertex's place in that packet.
    uint16_t m_tapSlot = 0;
    uint8_t m_tapK = 0;
    // GIF tag in progress. A GIF packet (tag + NLOOP loops) may span several
    // submissions: e.g. a VIF DIRECT/DMA transfer carrying the BITBLT registers
    // and an IMAGE tag, followed by separate transfers of the image data. The
    // GIF keeps its tag state across them, so the parser must too.
    struct GifTagProgress
    {
        bool active = false;
        uint8_t flg = 0;
        uint32_t nloopLeft = 0, nreg = 0, regIndex = 0;
        bool regListOdd = false; // REGLIST: pad the final odd 64-bit word
        uint8_t regs[16] = {};
    } m_gifTag;
    float m_curS = 0.0f, m_curT = 0.0f;
    uint16_t m_curU = 0, m_curV = 0;
    uint8_t m_curFog = 0;
    uint8_t m_fogR = 0, m_fogG = 0, m_fogB = 0;

    bool m_prmodecont = true;
    bool m_pabe = false;
    uint64_t m_scanmsk = 0;
    uint64_t m_dimx = 0;
    uint64_t m_dthe = 0;
    uint64_t m_colclamp = 0;
    GSTexaReg m_texa{0u, false, 0u};
    GSTexClutReg m_texclut{0u, 0u, 0u};
    // CLUT load bookkeeping for TEX0/TEX2 CLD 2..5 (CBP0/CBP1 registers).
    uint32_t m_clutCbp0 = 0u;
    uint32_t m_clutCbp1 = 0u;
    void loadClutForCld(const GSTex0Reg &tex);

    GSBitBltBuf m_bitbltbuf{};
    GSTrxPos m_trxpos{};
    GSTrxReg m_trxreg{};
    uint32_t m_trxdir = 3;


    static constexpr int kMaxVerts = 6;
    GSVertex m_vtxQueue[kMaxVerts];
    int m_vtxCount = 0;
    int m_vtxIndex = 0;

    std::vector<uint8_t> m_displaySnapshot;
    std::mutex m_snapshotMutex;
    uint32_t m_lastDisplayBaseBytes = 0;
    GSFrameReg m_preferredDisplaySourceFrame{};
    uint32_t m_preferredDisplayDestFbp = 0;
    bool m_hasPreferredDisplaySource = false;
    std::vector<uint8_t> m_hostPresentationFrame;
    uint32_t m_hostPresentationWidth = 0;
    uint32_t m_hostPresentationHeight = 0;
    uint32_t m_hostPresentationDisplayFbp = 0;
    uint32_t m_hostPresentationSourceFbp = 0;
    bool m_hostPresentationUsedPreferred = false;
    uint32_t m_hostPresentationCrt = 0; // the frame's video mode (gs_crt.h)
    bool m_hasHostPresentationFrame = false;

    // Present only finished frames (see latchHostPresentationFrame). A game that
    // flips its display buffers every VSync assumes each frame is drawn within
    // one field; when emulation is slower, the flip exposes the buffer still being
    // drawn (for GT3 its Z buffer overlaps it). The frontend records which 8 KiB
    // pages each draw/transfer wrote and never shows the flipped buffer that was
    // written most recently.
    static constexpr uint32_t kGsPageCount = 512u; // 4 MiB / 8 KiB pages
    std::array<uint64_t, kGsPageCount> m_pageWriteSeq{};
    // Per frame buffer (FBP): draw area by fractional OFY since it was last
    // shown, and the dominant phase of the last complete field.
    std::array<std::array<uint32_t, 16>, kGsPageCount> m_fbpPhaseArea{};
    std::array<uint8_t, kGsPageCount> m_fbpPhase{};
    uint64_t m_writeSeq = 0;
    std::atomic<uint64_t> m_drawLogRequestTick{0u};
    struct DisplayRegion
    {
        uint64_t dispfb1 = 0, dispfb2 = 0;
        uint64_t lastLatch = 0;
        uint64_t lastDisplayTick = 0;
        std::vector<uint32_t> pages;
    };
    std::vector<DisplayRegion> m_recentDisplayRegions; // the buffers the game has been flipping between
    uint64_t m_latchCount = 0;
    std::atomic<uint64_t> m_publishedFrames{0};
    mutable std::mutex m_flipMutex;
    std::array<int64_t, 1024> m_flipTimesNs{}; // steady_clock of each flip (ring; GT4's FPS counter's 1% low reads ~10 s)
    size_t m_flipCount = 0;
    uint32_t m_lastFlipFbp = ~0u;
    const DisplaySnapshot *m_snapshot = nullptr; // set while latching from a snapshot
    void latchPresentation();
    uint64_t m_lastPresentedTick = ~0ull;
    // The display setup the last latch showed (video mode, SMODE2, the
    // displayed circuit's size and format): a change (a video mode switch)
    // drops the remembered display buffers and field phases, which belong to
    // the old mode (analysis/design/video-modes/VIDEO-MODES.md 3.5).
    uint64_t m_displaySignature = ~0ull;
    void noteWrittenRect(uint32_t firstPage, uint32_t bufferWidth, uint8_t psm, int x0, int y0, int x1, int y1);
    void noteDrawWrites(const GSPrimitiveBatch &batch);
    void logFrameDraw(const GSPrimitiveBatch &batch);
    bool squeezeHud(GSPrimitiveBatch &batch); // false: a hidden HUD element (not drawn)
    void noteInsetView(const GSPrimitiveBatch &batch);
    // squeezeHud: the last displayed frame buffers (double buffering) and
    // the buffers drawn with a depth test (3D targets, whose copies to the
    // screen are the picture, not the HUD).
    std::array<uint32_t, 4> m_hudFbps{0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu};
    std::array<uint64_t, 8> m_depthTargets{};
    std::vector<uint32_t> displayPagesUnlocked(const GSPresentationRequest &request) const;
    uint64_t lastWriteSeqUnlocked(const std::vector<uint32_t> &pages) const;
    void publishPresentationFrame(PresentationFrame frame);

    uint64_t m_nativeImageUploadCount = 0;
    uint64_t m_nativePackedGIFPacketCount = 0;

    static constexpr size_t kDebugHistoryCapacity = 512;
    std::array<GSDebugHistoryEntry, kDebugHistoryCapacity> m_debugHistory{};
    size_t m_debugHistoryWrite = 0;
    size_t m_debugHistoryCount = 0;
    uint64_t m_debugNextSeq = 1;
    uint32_t m_debugFrameIndex = 0;
    uint64_t m_debugLastVsyncTick = UINT64_MAX;
    bool m_debugHistoryPaused = true;

    std::unique_ptr<GSRasterBackend> m_backend;
};

#endif
