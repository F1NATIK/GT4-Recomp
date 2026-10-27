#ifndef PS2X_VIF1_WORKER_H
#define PS2X_VIF1_WORKER_H

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

class PS2Memory;

// VIF1 and VU1 on their own thread, overlapped with the EE (GT3_VIF1_THREAD=0
// keeps them on the EE thread).
//
// The EE posts VIF1 input (a DMA's bytes, already copied out of RAM) and
// FBRST.STC resumes, and carries on. The worker runs the VIF1 commands, the VU1
// microprograms and their PATH1/PATH2 GIF packets until the input runs out or
// an I-bit stalls VIF1, then parks. An I-bit interrupt or a finished DMA wakes
// the EE, which raises them from its own thread.
//
// Ownership: until it parks, the worker alone touches VIF1 and VU1 state, the
// GIF arbiter and PATH3 masking. The EE calls quiesce() before it touches any
// of them (VIF1 registers, VU1 memory, a GIF DMA, a GS readback); quiesce()
// returns once everything posted so far has run and the worker is parked, and
// the worker stays parked until the EE posts again. GT3 only touches VIF1 from
// the interrupt handlers of its stalls, where the worker is already parked.
class Vif1Worker
{
public:
    using WakeFn = std::function<void()>;

    ~Vif1Worker() { stop(); }

    // threadInit runs first on the worker thread (the guest float mode).
    bool start(PS2Memory &memory, WakeFn wakeEe, std::function<void()> threadInit);
    // Runs on the worker after every command, before it parks or wakes the EE
    // (the GS worker's producer batch is handed over there). Set before start.
    void setAfterCommand(std::function<void()> fn) { m_afterCommand = std::move(fn); }
    void stop();
    bool active() const { return m_active; }
    // EE side: the worker has input posted that it has not finished (it is
    // running VIF1, so an I-bit interrupt or a completion is due shortly).
    bool running() const { return m_active && m_postedSeq != m_doneSeq.load(std::memory_order_acquire); }
    bool onWorkerThread() const { return m_active && std::this_thread::get_id() == m_threadId; }

    // Where the EE quiesces: GT3_VIF1_THREAD_STATS counts the waits (the
    // worker was still busy) and their time per site.
    enum class QuiesceSite : uint8_t
    {
        Other,          // GPU renderer switch, stop
        Vu1Memory,      // VU1 code / data mapped for the EE
        Vif1Read,       // a VIF1 register read (STAT polls)
        Vif1Write,      // a VIF1 register write other than FBRST
        Vif1Fbrst,      // FBRST other than a plain STC
        Vif1DmaEnd,     // a VIF1 DMA completed from a transfer
        GifDmaStart,    // a GIF (PATH3) DMA start
        GifPending,     // pending GIF transfers run
        GifPacketRam,   // a PATH3 packet from RAM
        GifPacket,      // a PATH3 packet from a buffer
        GifImageChain,  // the native GIF image upload chain
        GifPackedChain, // the native GIF packed chain
        Count
    };

    // --- EE side ---
    // bytes are swapped out (the caller gets a spare buffer back).
    void postData(std::vector<uint8_t> &bytes, bool completesDma, uint32_t vu0Fbrst);
    void postResume(uint32_t vu0Fbrst);
    // GT4 async PATH3 (PS2X_ASYNC_PATH3, analysis/phase5/specs/step4-ee-lateness.md
    // L1): a GIF DMA's gathered bytes, submitted as PATH3 after everything posted
    // before it (bytes are swapped out). Its completion is counted for the EE
    // (takePath3Completions), which publishes it (D_STAT, CHCR, DMAC cause 2).
    // A transfer split at the end of RAM is posted in pieces, the last one
    // with completesDma.
    void postPath3(std::vector<uint8_t> &bytes, bool completesDma);
    void quiesce(QuiesceSite site = QuiesceSite::Other);
    // DMA completions (VIF1 DMAs whose input ran without a stall) since the
    // last call; the EE publishes them (D_STAT, CHCR, DMAC handlers).
    uint32_t takeDmaCompletions() { return m_dmaDone.exchange(0u, std::memory_order_acq_rel); }
    // Posted PATH3 transfers the worker has submitted since the last call.
    uint32_t takePath3Completions() { return m_path3Done.exchange(0u, std::memory_order_acq_rel); }
    // EE: a GIF start found an earlier posted PATH3 not yet published and fell
    // back to the synchronous path (GT3_VIF1_THREAD_STATS).
    void notePath3Fallback() { m_path3Fallbacks.fetch_add(1, std::memory_order_relaxed); }
    // An empty buffer with spare capacity, if one came back from the worker.
    bool takeSpareBuffer(std::vector<uint8_t> &out);

    // --- worker side ---
    // COP2 FBRST of the EE thread that posted the running input (the VU1
    // D/T-bit enables).
    uint32_t vu0Fbrst() const { return m_vu0Fbrst; }

    // GT3_VIF1_THREAD_STATS: parks, EE waits in quiesce(), and their time.
    void reportStats();
    // Posted/run sequence numbers, queued commands, unpublished completions.
    std::string describe();

private:
    enum class Kind : uint8_t
    {
        Data,
        Resume,
        Path3,
    };
    struct Command
    {
        Kind kind = Kind::Data;
        bool completesDma = false;
        uint32_t vu0Fbrst = 0;
        uint64_t seq = 0;
        uint64_t postedNs = 0; // Path3 with GT3_VIF1_THREAD_STATS: when the EE posted it
        std::vector<uint8_t> bytes;
    };

    void post(Command &&command);
    void run();
    void execute(Command &command, bool &wakeEe);

    PS2Memory *m_memory = nullptr;
    WakeFn m_wakeEe;
    std::function<void()> m_threadInit;
    std::function<void()> m_afterCommand;
    std::thread m_thread;
    std::thread::id m_threadId{};
    bool m_active = false;

    std::mutex m_mutex;
    std::condition_variable m_workCv;
    std::condition_variable m_doneCv;
    std::deque<Command> m_queue;
    std::vector<std::vector<uint8_t>> m_spare;
    bool m_stop = false;
    // The EE numbers its posts; the worker publishes the last one it has run
    // when it parks with nothing queued.
    uint64_t m_postedSeq = 0; // EE only
    std::atomic<uint64_t> m_queuedSeq{0};
    std::atomic<uint64_t> m_doneSeq{0};
    std::atomic<uint32_t> m_dmaDone{0};
    std::atomic<uint32_t> m_path3Done{0};
    // Async PATH3 statistics (GT3_VIF1_THREAD_STATS): transfers run, their
    // delay from post to submit (sum, max), EE fallbacks to the synchronous path.
    std::atomic<uint64_t> m_path3Runs{0}, m_path3DelayNs{0}, m_path3MaxDelayNs{0}, m_path3Fallbacks{0};
    uint32_t m_vu0Fbrst = 0; // worker only

    bool m_stats = false;
    std::atomic<uint64_t> m_quiesceWaits{0}, m_quiesceNs{0}, m_parks{0}, m_busyNs{0};
    static constexpr size_t kQuiesceSites = static_cast<size_t>(QuiesceSite::Count);
    std::atomic<uint64_t> m_siteWaits[kQuiesceSites]{}, m_siteNs[kQuiesceSites]{};
    std::atomic<uint64_t> m_stalls{0}, m_stallNs{0};
    uint64_t m_stallStart = 0; // worker only: when the last I-bit stall began
    bool m_lastRunRaisedIrq = false;
};

#endif
