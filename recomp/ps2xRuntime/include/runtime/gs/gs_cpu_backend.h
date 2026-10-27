#pragma once

#include "runtime/gs/gs_backend.h"

#include <array>
#include <functional>
#include <mutex>
#include <vector>

class GSCpuBackend final : public GSRasterBackend
{
public:
    GSCpuBackend();

    // Batched (multithreaded) rasterization is on unless GT3_GS_BATCH=0;
    // 1/0 force it on/off for the process (unit tests), -1 restores the default.
    static void SetBatchingOverride(int enabled);

    // The CLUT buffer as last loaded (the GPU renderer decodes paletted
    // textures with it). CT32 entry k = [k] | [k + 256] << 16; CT16 entry k = [k].
    const std::array<uint16_t, 512> &clutBuffer() const { return m_clutBuffer; }
    // Pixels of the current host -> local transfer written so far (raster
    // order from TRXPOS; the whole rectangle once it is complete).
    uint32_t uploadedPixels() const
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_transferState.copiedPixels;
    }
    // GT4's race colour (runtime/gs/gs_fullcolour.h): the full-colour plane, one RGBA8 word per 16-bit
    // halfword of GS memory (8 MiB), made at the first full-colour draw (null before). A pixel's full colour
    // is its word while that reduces to the halfword in memory, else the halfword as v << 3. The GL renderer's
    // CPU copy fills it from its 16-bit targets at page downloads and uses it at uploads; the GL-vs-CPU
    // compare reads both renderers' planes. EnsureFullColourPlane makes it (zero: every pixel 16-bit).
    uint32_t *FullColourPlane() { return fcPlaneOrNull(); }
    const uint32_t *FullColourPlane() const { return m_fcPlane.empty() ? nullptr : m_fcPlane.data(); }
    uint32_t *EnsureFullColourPlane()
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_fcPlane.empty())
            m_fcPlane.assign(size_t(2u) * 1024u * 1024u, 0u);
        return m_fcPlane.data();
    }
    // A new video mode (GSDrawState::fcEpoch): every pixel's full colour is forgotten (the plane cleared: each
    // pixel its 16-bit value, v << 3). Submit does it itself at the first draw of a new epoch; the GL renderer
    // calls it for its CPU copy when it forgets its own (so the copy's next draw does not do it again).
    void ForgetFullColour(uint16_t epoch);
    // Replaces the CLUT buffer (the GPU renderer's copy of a load it did itself).
    void SetClutBuffer(const std::array<uint16_t, 512> &clut)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        flushPendingUnlocked();
        m_clutBuffer = clut;
        m_clutChanged = true;
    }

    void Initialize(uint8_t *vram, uint32_t vramSize) override;
    void Reset() override;

    void Submit(const GSPrimitiveBatch &batch) override;
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

private:
    // Draws are queued and rasterized in batches: each worker thread takes a band
    // of window rows and runs every queued draw clipped to it, so per-pixel order
    // is the submission order. A batch is flushed before anything that could
    // observe or change what its draws read or write (see Submit).
    struct RasterJob
    {
        int y0 = 0;
        int y1 = 2047;
        const uint16_t *clut = nullptr; // CLUT buffer as loaded when the draw was submitted
    };
    struct PageSet
    {
        uint64_t words[8]{};
        void set(uint32_t page) { words[(page >> 6u) & 7u] |= 1ull << (page & 63u); }
        bool overlaps(const PageSet &other) const
        {
            for (int i = 0; i < 8; ++i)
                if (words[i] & other.words[i])
                    return true;
            return false;
        }
        void merge(const PageSet &other)
        {
            for (int i = 0; i < 8; ++i)
                words[i] |= other.words[i];
        }
        bool empty() const
        {
            for (int i = 0; i < 8; ++i)
                if (words[i])
                    return false;
            return true;
        }
    };
    struct PendingDraw
    {
        GSPrimitiveBatch batch;
        uint32_t clutVersion = 0;
        int y0 = 0, y1 = -1;
    };
    // A page's low plane is bits 0-23 of every 32-bit word, its high plane
    // bits 24-31: CT24/Z24 writes never touch the high plane and PSMT8H/T4HL/
    // T4HH reads touch only it (GT3 keeps 8-bit images in the alpha planes of
    // its 24-bit buffers).
    struct PlaneSets
    {
        PageSet low, high;
        bool overlaps(const PlaneSets &other) const { return low.overlaps(other.low) || high.overlaps(other.high); }
        void merge(const PlaneSets &other)
        {
            low.merge(other.low);
            high.merge(other.high);
        }
    };
    struct PendingTarget
    {
        uint64_t key = 0;
        PageSet pages;
    };

    void ResetUnlocked();
    void flushPendingUnlocked();
    void logDrawDiagnostics(const GSPrimitiveBatch &batch);
    uint32_t ReadVramUnlocked(uint32_t psm, uint32_t base, uint32_t bw, uint32_t x, uint32_t y) const;
    void WriteVramUnlocked(uint32_t psm, uint32_t base, uint32_t bw, uint32_t x, uint32_t y, uint32_t value);

    void DrawPrimitive(const GSPrimitiveBatch &batch, const RasterJob &job);
    void DrawSprite(const GSPrimitiveBatch &batch, const RasterJob &job);
    void DrawTriangle(const GSPrimitiveBatch &batch, const RasterJob &job);
    void DrawLine(const GSPrimitiveBatch &batch, const RasterJob &job);
    void WritePixel(const GSDrawState &state, int x, int y, int z, uint8_t r, uint8_t g, uint8_t b, uint8_t a, uint8_t fog);
    uint32_t SampleTexture(const GSDrawState &state, float s, float t, float q, uint16_t u, uint16_t v, const RasterJob &job);
    uint32_t LookupCLUT(const GSDrawState &state, uint8_t index, uint8_t cpsm, uint8_t csa, uint8_t sourcePsm, const uint16_t *clut) const;

    void PerformLocalToLocalTransfer();
    void PerformLocalToHostTransfer();
    PresentationFrame PresentFromLocalMemory(const GSPresentationRequest &request);
    bool CopyFrameToHostRgba(const GSFrameReg &frame,
                             uint32_t width,
                             uint32_t height,
                             std::vector<uint8_t> &outPixels,
                             bool preserveAlpha,
                             bool useLocalMemoryLayout,
                             bool frameBaseIsPages,
                             uint32_t sourceOriginX,
                             uint32_t sourceOriginY,
                             bool fullColour = false) const;
    uint32_t *fcPlaneOrNull() { return m_fcPlane.empty() ? nullptr : m_fcPlane.data(); }

    // Plain function pointers: these run per texel and per pixel.
    using WriteVramFunc = void (*)(uint8_t *, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t);
    using ReadVramFunc = uint32_t (*)(uint8_t *, uint32_t, uint32_t, uint32_t, uint32_t);

    static constexpr size_t kPsmHandlerCount = 1u << 6u;
    mutable std::mutex m_mutex;
    // GS CLUT buffer (1 KB): CT32 entry k = low half [k], high half [k + 256];
    // CT16 entry k = [k]. Filled by LoadClut, read by LookupCLUT.
    std::array<uint16_t, 512> m_clutBuffer{};
    uint8_t *m_vram = nullptr;
    uint32_t m_vramSize = 0;
    std::array<ReadVramFunc, kPsmHandlerCount> m_readVramFuncs{};
    std::array<WriteVramFunc, kPsmHandlerCount> m_writeVramFuncs{};

    std::vector<PendingDraw> m_pending;
    std::vector<std::array<uint16_t, 512>> m_clutVersions;
    bool m_clutChanged = true;
    std::vector<PendingTarget> m_pendingTargets;
    PlaneSets m_pendingWrites, m_pendingReads;
    uint64_t m_pendingArea = 0;

    // GT4's race colour (FullColourPlane); m_fcView: Present's snapshot backend reads the displayed pages'
    // plane words from Present's copy instead.
    std::vector<uint32_t> m_fcPlane;
    const uint32_t *m_fcView = nullptr;
    uint16_t m_fcEpoch = 0; // the last GSDrawState::fcEpoch seen (ForgetFullColour)
    void forgetFullColourUnlocked(uint16_t epoch);

    GSTransferCommand m_transfer{};
    GSTransferSnapshot m_transferState{};
    std::vector<uint8_t> m_localToHostBuffer;
    size_t m_localToHostReadPos = 0;
};
