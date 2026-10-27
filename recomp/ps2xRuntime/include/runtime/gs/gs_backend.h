#pragma once

#include "runtime/gs/gs_types.h"

#include <atomic>
#include <cstdint>
#include <vector>

// Diagnostics: set once a GT3_GS_SNAPSHOT_TEX0 snapshot has been taken, so
// GT3_GS_DRAWLOG_ARMED=1 can start the draw log at that point.
inline std::atomic<bool> g_gsDiagSnapshotTaken{false};

class GSRasterBackend
{
public:
    virtual ~GSRasterBackend() = default;

    virtual void Initialize(uint8_t *vram, uint32_t vramSize) = 0;
    virtual void Reset() = 0;

    virtual void Submit(const GSPrimitiveBatch &batch) = 0;
    // A primitive whose draw state equals (gsSameDrawState) the state of the
    // previous Submit or SubmitSameState call on this backend: a backend may
    // reuse what it resolved for that state (step 7 draw-state runs,
    // analysis/design/ny-throughput/STEP7-THROUGHPUT.md T1). The caller
    // guarantees the equality (GSAsyncBackend's ring records); the result
    // must be the same as Submit's. Default: Submit.
    virtual void SubmitSameState(const GSPrimitiveBatch &batch) { Submit(batch); }

    // CLUT buffer load, issued when a TEX0/TEX2 write's CLD requests it
    // (in order with transfers and primitives).
    virtual void LoadClut(const GSTex0Reg &tex, const GSTexClutReg &texclut) { (void)tex; (void)texclut; }

    virtual void BeginTransfer(const GSTransferCommand &command) = 0;
    virtual void UploadImage(const uint8_t *data, uint32_t sizeBytes) = 0;

    virtual void Flush() = 0;
    virtual void TextureFlush() = 0;
    virtual void Sync(GSSyncReason reason) = 0;
    virtual PresentationFrame Present(const GSPresentationRequest &request) = 0;

    virtual bool ClearFramebuffer(const GSContext &context, uint32_t rgba) = 0;
    virtual uint32_t ConsumeLocalToHostBytes(uint8_t *dst, uint32_t maxBytes) = 0;

    virtual uint32_t ReadVram(uint32_t psm, uint32_t base, uint32_t bw, uint32_t x, uint32_t y) const = 0;
    virtual void WriteVram(uint32_t psm, uint32_t base, uint32_t bw, uint32_t x, uint32_t y, uint32_t value) = 0;
    virtual void SnapshotVram(std::vector<uint8_t> &out) const = 0;
    virtual GSTransferSnapshot GetTransferSnapshot() const = 0;

    // Optional work for idle time, on the backend's own thread: GSAsyncBackend
    // calls it when its command ring has stayed empty for its spin window,
    // before it sleeps, and checks the ring again after each call. One small
    // step per call; true when there is more to do (call again), false when
    // there is nothing (default).
    virtual bool IdleWork() { return false; }

    // A marker in the GS stream, in order with primitives (GT4's race pictures, gt4_hud_pictures.h: an A+D write to GS
    // register 0x7f carrying 'HUDP' in its high word; code = its low word). Default: nothing.
    virtual void Marker(uint32_t code) { (void)code; }
};
