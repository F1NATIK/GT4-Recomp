#pragma once

// A raster backend run on its own thread behind a command ring. The GS
// frontend (GIF parsing, register writes, vertex kicks: the GS worker thread)
// queues draws, CLUT loads, transfers and uploads and carries on, while this
// thread runs the backend (the OpenGL renderer, whose context is current
// here). Calls that return something or read memory the backend writes
// (presentation, read backs, VRAM access) first wait until everything queued
// before them has run, so the frontend sees the same results in the same
// order as with the backend on its own thread.
//
// In GT3's race the frontend was ~12% of the GS thread; on one thread with the
// OpenGL work the GS thread was the busiest pipeline stage.

#include "runtime/gs/gs_backend.h"

#include <array>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

class GSAsyncBackend final : public GSRasterBackend
{
public:
    // Runs a task on the backend thread and waits for it (inline there).
    using RunSync = std::function<void(const std::function<void()> &)>;
    // Makes the backend; its thread calls go through runOnThread.
    using Factory = std::function<std::unique_ptr<GSRasterBackend>(RunSync runOnThread)>;

    // threadStart/threadExit run on the backend thread (bind/release a context).
    GSAsyncBackend(const Factory &factory, std::function<void()> threadStart, std::function<void()> threadExit);
    ~GSAsyncBackend() override;
    GSAsyncBackend(const GSAsyncBackend &) = delete;
    GSAsyncBackend &operator=(const GSAsyncBackend &) = delete;

    void Initialize(uint8_t *vram, uint32_t vramSize) override;
    void Reset() override;
    void Submit(const GSPrimitiveBatch &batch) override;
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

    bool onBackendThread() const { return std::this_thread::get_id() == m_threadId; }

private:
    enum class Kind : uint32_t
    {
        Wrap = 0, // the rest of the ring is unused: continue at 0
        Submit,
        LoadClut,
        BeginTransfer,
        UploadImage,
        Flush,
        TextureFlush,
        Sync,
        Task, // a waiting caller's task (payload: TaskRecord)
        // A primitive with the draw state of the previous Submit record
        // (payload: SameRecord; step 7 draw-state runs, PS2X_GS_GL_STATE_RUNS).
        SubmitSame,
        Marker, // GSRasterBackend::Marker (payload: the code)
    };
    struct SameRecord
    {
        std::array<GSVertex, 3> vertices;
        uint8_t vertexCount;
    };
    struct Header
    {
        Kind kind;
        uint32_t bytes; // payload bytes
    };
    struct TaskRecord
    {
        const std::function<void()> *task;
    };

    static constexpr size_t kRingBytes = size_t{32} << 20u; // power of two
    static constexpr size_t kAlign = 16u;

    void run(std::function<void()> threadStart, std::function<void()> threadExit);
    // Queues a primitive: a SubmitSame record when its draw state equals the
    // previous Submit record's, else a Submit record.
    void pushSubmit(const GSPrimitiveBatch &batch);
    // Queues a record; payload may be null (bytes 0).
    void push(Kind kind, const void *payload, uint32_t bytes);
    void push2(Kind kind, const void *a, uint32_t aBytes, const void *b, uint32_t bBytes);
    // Runs task on the backend thread after everything queued, and waits.
    void runSync(const std::function<void()> &task) const;
    void execute(const Header &header, const uint8_t *payload);
    uint8_t *reserve(size_t recordBytes); // producer lock held
    void publish(size_t newHead);         // producer lock held

    std::unique_ptr<GSRasterBackend> m_inner;
    std::thread m_thread;
    std::thread::id m_threadId{};

    // Draw-state runs (PS2X_GS_GL_STATE_RUNS; step 7 T1). Producer side,
    // under m_produce: the state of the last Submit record queued.
    bool m_stateRuns = false;
    bool m_lastStateValid = false;
    GSDrawState m_lastState{};
    // Backend thread: the primitive the records describe (a SubmitSame record
    // replaces only its vertices), and whether a Submit reached the backend
    // directly (on this thread) since the last record, so the next SubmitSame
    // record goes to it as a Submit (its previous call had another state).
    GSPrimitiveBatch m_current{};
    bool m_currentValid = false;
    bool m_directSubmit = false;

    std::vector<uint8_t> m_ring;
    // Producer side (any thread; one at a time under m_produce).
    mutable std::atomic_flag m_produce = ATOMIC_FLAG_INIT;
    size_t m_head = 0; // bytes written (monotonic), under m_produce
    alignas(64) std::atomic<size_t> m_published{0};
    alignas(64) std::atomic<size_t> m_consumed{0};
    alignas(64) std::atomic<uint64_t> m_tasksDone{0};
    mutable uint64_t m_tasksQueued = 0; // under m_produce

    mutable std::mutex m_mutex;
    mutable std::condition_variable m_wake;     // the backend thread sleeps on it
    mutable std::condition_variable m_progress; // producers wait for room or a task
    std::atomic<bool> m_sleeping{false};
    std::atomic<uint32_t> m_roomWaiters{0}; // producers waiting for ring space
    std::atomic<uint32_t> m_taskWaiters{0}; // callers waiting for their task
    std::atomic<bool> m_stop{false};
};
