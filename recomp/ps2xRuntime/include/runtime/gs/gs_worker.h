#ifndef PS2_GS_WORKER_H
#define PS2_GS_WORKER_H

// GS packet worker thread: the EE side (GIF arbiter) queues GIF packets in
// order and carries on, while this thread feeds them to the GS frontend and
// rasterizer, as the real GS runs beside the EE. Anything that reads GS state
// or VRAM back on the EE side calls sync() first, which waits until every
// packet queued before the call has been processed.
//
// GT3_GS_THREAD=0 turns it off (packets are processed on the caller).

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

class GsWorker
{
public:
    using ProcessFn = std::function<void(const uint8_t *, uint32_t)>;

    GsWorker() = default;
    ~GsWorker();
    GsWorker(const GsWorker &) = delete;
    GsWorker &operator=(const GsWorker &) = delete;

    // Starts the thread unless GT3_GS_THREAD=0; returns whether it runs.
    // threadStart/threadExit run on the worker thread itself (the GPU
    // renderer binds and releases its OpenGL context there).
    bool start(ProcessFn fn, std::function<void()> threadStart = {}, std::function<void()> threadExit = {});
    void stop();
    bool running() const { return m_running; }
    bool onWorkerThread() const { return m_running && std::this_thread::get_id() == m_threadId; }

    // Runs task on the worker thread and waits for it (inline when not running
    // or already on it): in order after everything submitted so far, or with
    // `next`, before the packets still queued (at the next packet boundary).
    void runOnWorker(const std::function<void()> &task, bool next = false);
    // Queues task to run on the worker in order with the packets, without
    // waiting (inline when the worker is not running).
    void post(std::function<void()> task);
    // post() for the end of a frame (the VSync presentation snapshot): waits
    // first while GT3_GS_MAX_FRAMES (default 2, GT4 6; 0: no limit) such
    // tasks are still queued, so the EE runs at most that many frames ahead of
    // what the GS has drawn (input latency; the byte limit alone allowed ~20
    // frames). GT4: the 6 is headroom for a GL stall of a few frames. When
    // more than kBacklogPosts frames in a row find the queue full (the GL
    // side is slower than the EE, not just held up once), the cap becomes
    // PS2X_GS_BACKLOG_CAP (default 2; 0: off) until a frame is posted with
    // fewer than that queued, so a steadily slow GL side costs 2 frames of
    // input latency, not 6.
    void postFrame(std::function<void()> task);

    // GT4 diagnostics (GT3_GS_WORKER_STATS, analysis/phase5/specs/step4-hitches.md
    // P5): what the guest sees of the GS before the worker has caught up.
    // noteGsEvent: a SIGNAL (bit 0) / FINISH (bit 1) the worker just raised;
    // delivered = it raised the INTC GS interrupt (IMR unmasked). The frames
    // still queued behind it are how many VBlanks after its packet the guest
    // sees it. noteGuestRead: an EE read of CSR (0x1000) or SIGLBLID (0x1080),
    // which return the registers as the worker has left them (no sync). Both
    // count only while the stats are on; reportStats prints them.
    void noteGsEvent(uint32_t csrBits, bool delivered);
    static void noteGuestRead(uint32_t regOff, uint64_t value);

    // Queues a copy of the packet (or processes it now when not running).
    void submit(const uint8_t *data, uint32_t size);
    // Queues the packet in `packet` without copying it: its bytes are swapped
    // into the queue and `packet` gets a spare (empty) buffer back.
    void submitOwned(std::vector<uint8_t> &packet);
    // Waits until everything submitted before this call has been processed.
    // A no-op on the worker thread itself.
    void sync();

    // Producer batching for the calling thread (the VIF1/VU1 worker, which
    // submits a GIF packet per XGKICK): its submitOwned() packets are handed
    // over up to 16 (or 256 KB) at a time, under one lock and at most one
    // wake of the worker. flushProducerBatch() hands over what is pending; the
    // producer calls it before anything else may depend on the packets (the
    // VIF1 worker: after every command, before it parks or raises an
    // interrupt). GT3_GS_BATCH=0: off.
    void beginProducerBatch();
    void flushProducerBatch();
    struct ProducerBatch;

private:
    void run();

    struct Item
    {
        std::vector<uint8_t> packet;
        const std::function<void()> *task = nullptr; // owned by the waiting caller
        std::function<void()> owned;                  // a posted task
        bool frame = false;                           // posted by postFrame
    };

    ProcessFn m_fn;
    std::function<void()> m_threadStart;
    std::function<void()> m_threadExit;
    std::thread m_thread;
    std::thread::id m_threadId{};
    bool m_running = false;

    std::mutex m_mutex;
    std::condition_variable m_workCv;
    std::condition_variable m_doneCv;
    std::deque<Item> m_queue;
    std::vector<std::vector<uint8_t>> m_free;
    uint64_t m_submitted = 0;
    uint64_t m_processed = 0;
    size_t m_queuedBytes = 0;
    bool m_stop = false;
    // The worker is waiting on m_workCv (producers notify only then) / the
    // queue has items (it spins on this before sleeping). Under m_mutex /
    // written under it.
    bool m_workerSleeping = false;
    std::atomic<bool> m_hasWork{false};
    // Callers waiting on m_doneCv (the worker notifies only then; under m_mutex).
    uint32_t m_doneWaiters = 0;
    template <class Pred>
    void waitDone(std::unique_lock<std::mutex> &lock, Pred pred)
    {
        if (pred())
            return;
        ++m_doneWaiters;
        m_doneCv.wait(lock, pred);
        --m_doneWaiters;
    }
    // Back-pressure: the EE waits when this much packet data is waiting.
    static constexpr size_t kMaxQueuedBytes = 48u << 20u;

    // GT3_GS_GL_STATS / GT3_GS_WORKER_STATS: every 10 s, how long the worker
    // waited for work and how long producers waited for it (queue full, sync,
    // runOnWorker): which side of the EE/GS pipeline is the bottleneck.
    bool m_stats = false;
    uint64_t m_idleNs = 0;
    std::atomic<uint64_t> m_fullNs{0}, m_syncNs{0}, m_syncCount{0}, m_frameWaitNs{0};
    uint32_t m_queuedFrames = 0;
    uint32_t m_maxFrames = 2;
    // GT4 sustained-backlog cap (see postFrame; under m_mutex): the cap after
    // a sustained backlog (0: off), the frames posted in a row that found the
    // queue full, and whether the backlog cap is in force.
    static constexpr uint32_t kBacklogPosts = 30u;
    uint32_t m_backlogCap = 0;
    uint32_t m_backlogRun = 0;
    bool m_backlogCapped = false;
    // Stats (under m_mutex): frames queued when each frame was posted
    // (histogram 0..7+, sum, max) and the posts made under the backlog cap.
    uint64_t m_postDepth[8] = {};
    uint64_t m_postDepthSum = 0, m_postsReduced = 0;
    uint32_t m_postDepthMax = 0;
    // Guest-visible GS events and reads (noteGsEvent / noteGuestRead).
    std::atomic<uint64_t> m_evSignal{0}, m_evFinish{0}, m_evDelivered{0}, m_evLagSum{0}, m_evLagMax{0};
    static std::atomic<bool> s_guestStats;
    static std::atomic<uint64_t> s_csrReads, s_csrReadsSet, s_sigLblIdReads;
    std::chrono::steady_clock::time_point m_statStart{};
    static uint64_t sinceNs(std::chrono::steady_clock::time_point t)
    {
        return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t).count());
    }
    void reportStats();
};

#endif
