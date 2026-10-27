#pragma once

#include "runtime/gs/gs_backend.h"
#include "runtime/gs/gs_cpu_backend.h"

#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

// GPU translation renderer (OpenGL; see gt3-analysis/GPU-TRANSLATION-RENDERER.md).
//
// GS draws become OpenGL draws into GPU render targets (one GL texture per GS
// frame/Z buffer). The GS local memory copy (the CPU backend's 4 MB) stays the
// hand-off format: each 8 KiB page is owned either by that copy or by the GPU
// target that last drew into it, and is copied across when the other side
// needs it (texture uploads, readbacks, CLUT loads, presentation, and draws
// the GPU side does not handle yet, which the CPU backend then draws).
//
// OpenGL work happens only on the GL thread (the GS worker, which has the
// shared context current); calls from other threads run there through
// runOnGlThread.
class GSGlBackend final : public GSRasterBackend
{
public:
    using RunOnGlThread = std::function<void(const std::function<void()> &)>;

    // runOnGlThreadNext: the same, but before GS packets still queued (the
    // presentation snapshot must match the state the frontend chose it from).
    GSGlBackend(RunOnGlThread runOnGlThread, RunOnGlThread runOnGlThreadNext);
    ~GSGlBackend() override;

    void Initialize(uint8_t *vram, uint32_t vramSize) override;
    void Reset() override;

    void Submit(const GSPrimitiveBatch &batch) override;
    // Reuses the previous primitive's resolved draw state while nothing it
    // depends on changed (step 7 T1, PS2X_GS_GL_STATE_RUNS).
    void SubmitSameState(const GSPrimitiveBatch &batch) override;
    void LoadClut(const GSTex0Reg &tex, const GSTexClutReg &texclut) override;
    void BeginTransfer(const GSTransferCommand &command) override;
    void UploadImage(const uint8_t *data, uint32_t sizeBytes) override;

    void Flush() override;
    void TextureFlush() override;
    void Sync(GSSyncReason reason) override;
    void Marker(uint32_t code) override;
    PresentationFrame Present(const GSPresentationRequest &request) override;

    bool ClearFramebuffer(const GSContext &context, uint32_t rgba) override;
    uint32_t ConsumeLocalToHostBytes(uint8_t *dst, uint32_t maxBytes) override;

    uint32_t ReadVram(uint32_t psm, uint32_t base, uint32_t bw, uint32_t x, uint32_t y) const override;
    void WriteVram(uint32_t psm, uint32_t base, uint32_t bw, uint32_t x, uint32_t y, uint32_t value) override;
    void SnapshotVram(std::vector<uint8_t> &out) const override;
    GSTransferSnapshot GetTransferSnapshot() const override;
    // GL thread idle time (GSAsyncBackend): spare target textures (PS2X_GS_GL_PREWARM).
    bool IdleWork() override;

    bool glReady() const { return m_glReady; }

    // Copies the GPU-held pages of a buffer rectangle into the local memory
    // copy without handing them over (as a presentation peek): for comparing
    // the GL renderer against the CPU one (GSCompareBackend).
    void PeekToLocalMemory(uint32_t fbp, uint32_t fbw, uint32_t psm, int x0, int y0, int x1, int y1);
    // A page's ownership state, for the compare diagnostics.
    std::string DescribePage(uint32_t page);
    // The texel (x, y) of the colour target for (fbp, fbw, layout of psm),
    // read straight from its texture, and that target's id (-1: none).
    uint32_t ProbeTarget(uint32_t fbp, uint32_t fbw, uint32_t psm, int x, int y, int &id);
    // The CLUT entry a lookup of `index` with this TEX0 reads in the GL
    // renderer (its GPU CLUT or its CPU buffer), described, for the compare.
    std::string DescribeClutEntry(const GSTex0Reg &tex, uint32_t index);
    const std::string &glDescription() const { return m_glDescription; }
    // The CPU copy (GS memory as the CPU renderer sees it after page downloads, and its full-colour plane:
    // GT4's race colour, runtime/gs/gs_fullcolour.h), for the compare and tests.
    GSCpuBackend &cpuCopy() const { return m_cpu; }

    struct Impl;

private:
    void initGl(); // on the GL thread
    void onGl(const std::function<void()> &task) const;
    void submitPrimitive(const GSPrimitiveBatch &batch, bool sameState);

    RunOnGlThread m_runOnGlThread;
    RunOnGlThread m_runOnGlThreadNext;
    mutable GSCpuBackend m_cpu;
    bool m_glReady = false;
    std::string m_glDescription;
    std::thread::id m_glThread{};
    std::unique_ptr<Impl> m_impl;
};

// GT3_GS_COMPARE=1 (with GT3_RENDERER=gl): every GS call goes to the OpenGL
// renderer and to a CPU renderer with its own local memory; at each
// presentation the displayed rectangle of both is compared word by word and
// the differences are logged ([gs-compare]). The first differing frames are
// written as <GT3_GS_COMPARE_DUMP prefix>-<tick>-{gl,cpu}.ppm.
class GSCompareBackend final : public GSRasterBackend
{
public:
    explicit GSCompareBackend(std::unique_ptr<GSGlBackend> gl);
    ~GSCompareBackend() override;

    void Initialize(uint8_t *vram, uint32_t vramSize) override;
    void Reset() override;
    void Submit(const GSPrimitiveBatch &batch) override;
    // The GL renderer gets SubmitSameState (so compare runs check the
    // draw-state runs), the CPU one Submit.
    void SubmitSameState(const GSPrimitiveBatch &batch) override;
    void LoadClut(const GSTex0Reg &tex, const GSTexClutReg &texclut) override;
    void BeginTransfer(const GSTransferCommand &command) override;
    void UploadImage(const uint8_t *data, uint32_t sizeBytes) override;
    void Flush() override;
    void TextureFlush() override;
    void Sync(GSSyncReason reason) override;
    PresentationFrame Present(const GSPresentationRequest &request) override;
    bool ClearFramebuffer(const GSContext &context, uint32_t rgba) override;
    uint32_t ConsumeLocalToHostBytes(uint8_t *dst, uint32_t maxBytes) override;
    uint32_t ReadVram(uint32_t psm, uint32_t base, uint32_t bw, uint32_t x, uint32_t y) const override;
    void WriteVram(uint32_t psm, uint32_t base, uint32_t bw, uint32_t x, uint32_t y, uint32_t value) override;
    void SnapshotVram(std::vector<uint8_t> &out) const override;
    GSTransferSnapshot GetTransferSnapshot() const override;
    bool IdleWork() override;

    GSGlBackend &gl() { return *m_gl; }

private:
    void compare(const GSPresentationRequest &request);
    void submitBoth(const GSPrimitiveBatch &batch, bool sameState);

    std::unique_ptr<GSGlBackend> m_gl;
    GSCpuBackend m_shadow;
    std::vector<uint8_t> m_shadowVram;
    uint8_t *m_vram = nullptr;
    uint64_t m_frames = 0, m_differingFrames = 0, m_dumps = 0, m_drawReports = 0;
    uint64_t m_fcFrames = 0, m_fcOnlyFrames = 0, m_fcPartialFrames = 0; // GT4's race colour (compare)
    // This frame's draws (since the last presentation), and how many
    // transfers/CLUT loads came before each.
    std::vector<GSPrimitiveBatch> m_frameDraws, m_prevDraws;
    std::vector<uint32_t> m_frameDrawEvents;
    uint32_t m_events = 0;
    // GT3_GS_COMPARE_WATCH=x,y[;x,y...]: frame pixels checked around every
    // draw that covers them; the first draw after which GL and CPU differ is
    // logged with the values before and after.
    std::vector<std::pair<int, int>> m_watch;
    uint64_t m_watchReports = 0;
    void watchDraw(const GSPrimitiveBatch &batch);
    // GT3_GS_COMPARE_MEMWATCH=fbp,fbw,x,y[;...] (fbp in hex): 32-bit words of
    // local memory checked after every draw, transfer and upload that touches
    // their page; the event after which GL and CPU differ is logged.
    struct MemWatch
    {
        uint32_t word = 0;
        bool differs = false;
        uint32_t lastGl = 0, lastCpu = 0;
        std::string name;
    };
    std::vector<MemWatch> m_memWatch;
    GSTransferCommand m_lastTransfer{};
    void checkMemWatch(const char *event, const std::vector<uint32_t> &pages);
    std::string m_memWatchBefore;
    uint64_t m_lastTick = 0, m_traceFrom = ~0ull, m_traceLines = 0;
    std::vector<std::string> m_ring; // the last events that touched a watched page (dumped at a divergence)
    size_t m_ringNext = 0;
    void ringNote(const std::string &line)
    {
        if (m_ring.size() < 48u)
            m_ring.push_back(line);
        else
            m_ring[m_ringNext++ % 48u] = line;
    }
};
