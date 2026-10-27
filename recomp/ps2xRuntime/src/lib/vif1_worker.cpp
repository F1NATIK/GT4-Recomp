#include "runtime/vif1_worker.h"
#include "runtime/ps2_memory.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>

namespace
{
// Short spins before sleeping: GT3 resumes a stalled VIF1 within microseconds
// of the stall when the EE is running, and a condition-variable wake costs
// tens of microseconds on Windows, ~50 times a frame.
constexpr int kWorkerSpin = 4000;
constexpr int kQuiesceSpin = 4000;

inline void cpuRelax()
{
#if defined(_MSC_VER) || defined(__SSE2__) || defined(__x86_64__) || defined(_M_X64)
    _mm_pause();
#endif
}

uint64_t nowNs();
}
// GT3_VIF1_THREAD_STATS: an I-bit interrupt's round trip split up: raised by the worker -> taken by the EE
// (drainCompletedDmacHandlers) -> resumed by the guest's FBRST.STC.
namespace vif1lat
{
    std::atomic<uint64_t> raiseNs{0}, noticeNs{0}, noticeSum{0}, noticeN{0}, noticeMax{0}, handlerSum{0}, handlerN{0}, handlerMax{0};
    std::atomic<uint64_t> waitingSum{0}, waitingN{0};
    std::atomic<int> eeWaiting{0}; // the EE is in EeScheduler::waitForEvent
    std::atomic<bool> raisedWhileWaiting{false};
    std::atomic<int> eePacing{0}, raisedWhilePacing{0}; // the EE in processDueDeadlines' pacing wait
    // What the EE thread does (set by it at a few boundaries): 0 guest code / other, 1 a 32-bit I/O store,
    // 2 processPendingEvents, 3 posting a VSync presentation (GS frame cap), 4 waiting for the VIF1 worker
    // (quiesce), 5 waitForEvent.
    std::atomic<int> eeActivity{0}, raisedActivity{0};
    std::atomic<uint64_t> actSum[6]{}, actN[6]{};
    std::atomic<uint64_t> pacingSum{0}, pacingN{0};
    std::atomic<uint64_t> seenSum{0}, seenN{0}; // raised -> a running EE's checkpoint sees the pending event
    std::atomic<uint64_t> wakeSum{0}, wakeN{0}, drainSum{0}; // raised -> the worker wakes the EE; the GIF drain's part
    std::atomic<uint64_t> path3RunSum{0}, path3RunN{0}, path3RunMax{0}, path3Bytes{0}; // a PATH3 command's run on the worker
    void noteWake()
    {
        const uint64_t r = raiseNs.load(std::memory_order_relaxed);
        if (!r)
            return;
        wakeSum += nowNs() - r;
        ++wakeN;
    }
    void noteCheckpointSeen()
    {
        const uint64_t r = raiseNs.load(std::memory_order_relaxed);
        static thread_local uint64_t lastSeen = 0;
        if (!r || r == lastSeen)
            return;
        lastSeen = r;
        seenSum += nowNs() - r;
        ++seenN;
    }
    void noteRaise()
    {
        raisedWhileWaiting.store(eeWaiting.load(std::memory_order_relaxed) != 0, std::memory_order_relaxed);
        raisedWhilePacing.store(eePacing.load(std::memory_order_relaxed), std::memory_order_relaxed);
        raisedActivity.store(eeWaiting.load(std::memory_order_relaxed) ? 5 : eeActivity.load(std::memory_order_relaxed),
                             std::memory_order_relaxed);
        raiseNs.store(nowNs(), std::memory_order_relaxed);
    }
    std::atomic<int> noticePath{0}; // 1: the scheduler loop (processPendingEvents)
    std::atomic<uint64_t> loopN{0}, loopSum{0};
    void noteNotice()
    {
        const uint64_t r = raiseNs.exchange(0, std::memory_order_relaxed);
        if (!r)
            return;
        const uint64_t now = nowNs(), d = now - r;
        noticeNs.store(now, std::memory_order_relaxed);
        noticeSum += d;
        ++noticeN;
        if (noticePath.load(std::memory_order_relaxed) == 1)
        {
            loopSum += d;
            ++loopN;
        }
        {
            const int a = raisedActivity.load(std::memory_order_relaxed);
            if (a >= 0 && a < 6)
            {
                actSum[a] += d;
                ++actN[a];
            }
        }
        if (raisedWhilePacing.load(std::memory_order_relaxed))
        {
            pacingSum += d;
            ++pacingN;
        }
        if (raisedWhileWaiting.load(std::memory_order_relaxed))
        {
            waitingSum += d;
            ++waitingN;
        }
        if (d > noticeMax.load())
            noticeMax.store(d);
    }
    void noteResume()
    {
        const uint64_t n = noticeNs.exchange(0, std::memory_order_relaxed);
        if (!n)
            return;
        const uint64_t d = nowNs() - n;
        handlerSum += d;
        ++handlerN;
        if (d > handlerMax.load())
            handlerMax.store(d);
    }
}
namespace
{
uint64_t nowNs()
{
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                     std::chrono::steady_clock::now().time_since_epoch())
                                     .count());
}
}

bool Vif1Worker::start(PS2Memory &memory, WakeFn wakeEe, std::function<void()> threadInit)
{
    stop();
    m_memory = &memory;
    m_wakeEe = std::move(wakeEe);
    m_threadInit = std::move(threadInit);
    {
        const char *s = std::getenv("GT3_VIF1_THREAD_STATS");
        m_stats = s && *s && *s != '0';
    }
    m_stop = false;
    m_postedSeq = 0;
    m_queuedSeq.store(0);
    m_doneSeq.store(0);
    m_dmaDone.store(0);
    m_path3Done.store(0);
    m_active = true;
    m_thread = std::thread([this] { run(); });
    m_threadId = m_thread.get_id();
    return true;
}

void Vif1Worker::stop()
{
    if (!m_active)
        return;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_stop = true;
    }
    m_workCv.notify_all();
    if (m_thread.joinable())
        m_thread.join();
    m_active = false;
    m_queue.clear();
    m_spare.clear();
}

void Vif1Worker::post(Command &&command)
{
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        command.seq = ++m_postedSeq;
        m_queue.push_back(std::move(command));
        m_queuedSeq.store(m_postedSeq, std::memory_order_release);
    }
    m_workCv.notify_one();
}

void Vif1Worker::postData(std::vector<uint8_t> &bytes, bool completesDma, uint32_t vu0Fbrst)
{
    Command command;
    command.kind = Kind::Data;
    command.completesDma = completesDma;
    command.vu0Fbrst = vu0Fbrst;
    command.bytes.swap(bytes);
    post(std::move(command));
}

void Vif1Worker::postResume(uint32_t vu0Fbrst)
{
    Command command;
    command.kind = Kind::Resume;
    command.vu0Fbrst = vu0Fbrst;
    post(std::move(command));
}

void Vif1Worker::postPath3(std::vector<uint8_t> &bytes, bool completesDma)
{
    Command command;
    command.kind = Kind::Path3;
    command.completesDma = completesDma;
    command.postedNs = m_stats ? nowNs() : 0u;
    command.bytes.swap(bytes);
    post(std::move(command));
}

void Vif1Worker::quiesce(QuiesceSite site)
{
    if (!m_active || onWorkerThread())
        return;
    struct Mark
    {
        int before = vif1lat::eeActivity.exchange(4, std::memory_order_relaxed);
        ~Mark() { vif1lat::eeActivity.store(before, std::memory_order_relaxed); }
    } mark;
    const uint64_t target = m_postedSeq;
    if (m_doneSeq.load(std::memory_order_acquire) == target)
        return;
    const uint64_t start = m_stats ? nowNs() : 0u;
    bool done = false;
    for (int i = 0; i < kQuiesceSpin; ++i)
    {
        if (m_doneSeq.load(std::memory_order_acquire) == target)
        {
            done = true;
            break;
        }
        cpuRelax();
    }
    if (!done)
    {
        std::unique_lock<std::mutex> lock(m_mutex);
        m_doneCv.wait(lock, [&] { return m_doneSeq.load(std::memory_order_acquire) == target; });
    }
    if (m_stats)
    {
        const uint64_t waited = nowNs() - start;
        m_quiesceWaits.fetch_add(1, std::memory_order_relaxed);
        m_quiesceNs.fetch_add(waited, std::memory_order_relaxed);
        m_siteWaits[static_cast<size_t>(site)].fetch_add(1, std::memory_order_relaxed);
        m_siteNs[static_cast<size_t>(site)].fetch_add(waited, std::memory_order_relaxed);
    }
}

bool Vif1Worker::takeSpareBuffer(std::vector<uint8_t> &out)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_spare.empty())
        return false;
    out = std::move(m_spare.back());
    m_spare.pop_back();
    return true;
}

void Vif1Worker::execute(Command &command, bool &wakeEe)
{
    // GT3_VIF1_JITTER=<max us>: a random delay before every command, so the
    // interrupts and completions land at many different EE points (a test for
    // races between the two threads).
    static const uint32_t jitterUs = []
    {
        const char *v = std::getenv("GT3_VIF1_JITTER");
        return v ? static_cast<uint32_t>(std::strtoul(v, nullptr, 10)) : 0u;
    }();
    if (jitterUs != 0u)
    {
        static thread_local uint32_t seed = 0x9E3779B9u;
        seed = seed * 1664525u + 1013904223u;
        const uint64_t until = nowNs() + static_cast<uint64_t>((seed >> 8) % jitterUs) * 1000u;
        while (nowNs() < until)
            cpuRelax();
    }
    if (command.kind == Kind::Path3)
    {
        // A GIF DMA posted behind the VIF1 work before it (async PATH3): its
        // packets go to the GIF arbiter in post order, then the EE publishes
        // the DMA's completion. VIF1 itself is left as it is (stalled or out
        // of input), and so are the I-bit stall statistics.
        if (m_stats && command.postedNs != 0u)
        {
            const uint64_t delay = nowNs() - command.postedNs;
            m_path3DelayNs.fetch_add(delay, std::memory_order_relaxed);
            uint64_t seen = m_path3MaxDelayNs.load(std::memory_order_relaxed);
            while (delay > seen && !m_path3MaxDelayNs.compare_exchange_weak(seen, delay, std::memory_order_relaxed))
            {
            }
        }
        const uint64_t runStart = m_stats ? nowNs() : 0u;
        m_memory->vif1WorkerPath3(command.bytes);
        if (m_stats)
        {
            const uint64_t d = nowNs() - runStart;
            vif1lat::path3RunSum += d;
            ++vif1lat::path3RunN;
            vif1lat::path3Bytes += command.bytes.size();
            if (d > vif1lat::path3RunMax.load())
                vif1lat::path3RunMax.store(d);
        }
        if (command.completesDma)
        {
            m_path3Runs.fetch_add(1, std::memory_order_relaxed);
            m_path3Done.fetch_add(1u, std::memory_order_acq_rel);
            wakeEe = true;
        }
        return;
    }
    m_vu0Fbrst = command.vu0Fbrst;
    if (command.kind == Kind::Data)
        m_memory->vif1WorkerIngest(command.bytes, command.completesDma);
    else
        m_memory->vif1WorkerResume();
    const uint32_t result = m_memory->vif1WorkerRun();
    if ((result & 2u) != 0u)
        m_dmaDone.fetch_add(1u, std::memory_order_acq_rel);
    m_lastRunRaisedIrq = (result & 1u) != 0u;
    if (result != 0u)
        wakeEe = true;
}

void Vif1Worker::run()
{
    if (m_threadInit)
        m_threadInit();
    uint64_t windowStart = nowNs();
    std::unique_lock<std::mutex> lock(m_mutex);
    for (;;)
    {
        if (m_queue.empty())
        {
            if (m_stop)
                return;
            lock.unlock();
            const uint64_t done = m_doneSeq.load(std::memory_order_relaxed);
            for (int i = 0; i < kWorkerSpin && m_queuedSeq.load(std::memory_order_acquire) == done; ++i)
                cpuRelax();
            lock.lock();
            m_workCv.wait(lock, [&] { return !m_queue.empty() || m_stop; });
            continue;
        }
        Command command = std::move(m_queue.front());
        m_queue.pop_front();
        lock.unlock();

        const uint64_t start = m_stats ? nowNs() : 0u;
        // Time from raising an I-bit interrupt to the EE's resume (FBRST.STC).
        // (A posted PATH3 leaves the stall being timed as it is.)
        const bool path3 = command.kind == Kind::Path3;
        if (m_stats && command.kind == Kind::Resume && m_stallStart != 0u)
        {
            m_stallNs.fetch_add(start - m_stallStart, std::memory_order_relaxed);
            m_stalls.fetch_add(1, std::memory_order_relaxed);
        }
        if (!path3)
            m_stallStart = 0u;
        bool wakeEe = false;
        execute(command, wakeEe);
        if (m_afterCommand)
            m_afterCommand();
        if (m_stats)
        {
            const uint64_t end = nowNs();
            if (m_lastRunRaisedIrq && !path3)
                m_stallStart = end;
            m_busyNs.fetch_add(end - start, std::memory_order_relaxed);
            if (end - windowStart >= 10'000'000'000ull)
            {
                reportStats();
                windowStart = end;
            }
        }

        lock.lock();
        // Hand the input buffer back for the next chain (no regrowth per frame).
        if (command.bytes.capacity() >= (64u << 10) && m_spare.size() < 4u)
        {
            command.bytes.clear();
            m_spare.push_back(std::move(command.bytes));
        }
        if (m_queue.empty())
        {
            m_doneSeq.store(command.seq, std::memory_order_release);
            m_parks.fetch_add(1, std::memory_order_relaxed);
            m_doneCv.notify_all();
        }
        if (wakeEe && m_wakeEe)
        {
            if (m_stats)
                vif1lat::noteWake();
            lock.unlock();
            m_wakeEe();
            lock.lock();
        }
    }
}

std::string Vif1Worker::describe()
{
    if (!m_active)
        return "worker off";
    size_t queued = 0;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        queued = m_queue.size();
    }
    char line[160];
    std::snprintf(line, sizeof(line), "worker posted %llu done %llu queued %zu dma-done %u path3-done %u",
                  static_cast<unsigned long long>(m_postedSeq), static_cast<unsigned long long>(m_doneSeq.load()), queued,
                  m_dmaDone.load(), m_path3Done.load());
    return line;
}

void Vif1Worker::reportStats()
{
    static uint64_t lastBusy = 0, lastWaits = 0, lastWaitNs = 0, lastParks = 0, lastStalls = 0, lastStallNs = 0;
    const uint64_t busy = m_busyNs.load(), waits = m_quiesceWaits.load(), waitNs = m_quiesceNs.load(), parks = m_parks.load();
    const uint64_t stalls = m_stalls.load(), stallNs = m_stallNs.load();
    const double stallMs = static_cast<double>(stallNs - lastStallNs) / 1e6;
    std::fprintf(stderr,
                 "[vif1-worker] 10 s: busy %.1f%%, parks %llu, EE quiesce waits %llu (%.1f ms), I-bit stalls %llu waited %.1f ms (%.1f us each)\n",
                 static_cast<double>(busy - lastBusy) / 1e8, static_cast<unsigned long long>(parks - lastParks),
                 static_cast<unsigned long long>(waits - lastWaits), static_cast<double>(waitNs - lastWaitNs) / 1e6,
                 static_cast<unsigned long long>(stalls - lastStalls), stallMs,
                 stalls > lastStalls ? stallMs * 1000.0 / static_cast<double>(stalls - lastStalls) : 0.0);
    {
        using namespace vif1lat;
        const uint64_t nn = noticeN.exchange(0), ns = noticeSum.exchange(0), hn = handlerN.exchange(0), hs = handlerSum.exchange(0);
        const uint64_t wn = waitingN.exchange(0), ws = waitingSum.exchange(0);
        const uint64_t sn = seenN.exchange(0), ss = seenSum.exchange(0);
        const uint64_t kn = wakeN.exchange(0), ks = wakeSum.exchange(0), ds = drainSum.exchange(0);
        const uint64_t ln = loopN.exchange(0), ls = loopSum.exchange(0);
        const uint64_t rn = path3RunN.exchange(0), rs = path3RunSum.exchange(0), rb = path3Bytes.exchange(0);
        std::fprintf(stderr, "[vif1-worker] 10 s: PATH3 runs on the worker: %llu, avg %.1f us, max %.1f us, avg %.1f KB\n", (unsigned long long)rn,
                     rn ? rs / 1e3 / rn : 0.0, path3RunMax.exchange(0) / 1e3, rn ? rb / 1024.0 / rn : 0.0);
        const uint64_t pn = pacingN.exchange(0), ps = pacingSum.exchange(0);
        {
            static const char *const names[6] = {"guest code", "I/O store", "pending events", "VSync post", "VIF1 quiesce", "waiting"};
            std::string line = "[vif1-worker] 10 s: raised while the EE was in:";
            for (int a = 0; a < 6; ++a)
            {
                const uint64_t n = actN[a].exchange(0), sum = actSum[a].exchange(0);
                char part[96];
                std::snprintf(part, sizeof(part), " %s %llu (avg %.1f us, %.0f ms);", names[a], (unsigned long long)n, n ? sum / 1e3 / n : 0.0, sum / 1e6);
                line += part;
            }
            std::fprintf(stderr, "%s\n", line.c_str());
        }
        std::fprintf(stderr, "[vif1-worker] 10 s: raised during the EE's VBlank pacing wait: %llu, avg %.1f us\n", (unsigned long long)pn, pn ? ps / 1e3 / pn : 0.0);
        std::fprintf(stderr, "[vif1-worker] 10 s: taken in the scheduler loop: %llu, avg %.1f us; elsewhere (I/O stores, GIF chains): %llu, avg %.1f us\n",
                     (unsigned long long)ln, ln ? ls / 1e3 / ln : 0.0, (unsigned long long)(nn - ln), nn > ln ? (ns - ls) / 1e3 / (nn - ln) : 0.0);
        std::fprintf(stderr, "[vif1-worker] 10 s: raised -> worker wakes the EE: %llu, avg %.1f us (GIF drains after raising: %.1f ms in all)\n",
                     (unsigned long long)kn, kn ? ks / 1e3 / kn : 0.0, ds / 1e6);
        std::fprintf(stderr, "[vif1-worker] 10 s: raised -> first EE checkpoint seeing it: %llu, avg %.1f us\n", (unsigned long long)sn, sn ? ss / 1e3 / sn : 0.0);
        std::fprintf(stderr, "[vif1-worker] 10 s: of those, raised while the EE waited for events: %llu, avg %.1f us; while it ran: %llu, avg %.1f us\n",
                     (unsigned long long)wn, wn ? ws / 1e3 / wn : 0.0, (unsigned long long)(nn - wn), nn > wn ? (ns - ws) / 1e3 / (nn - wn) : 0.0);
        std::fprintf(stderr, "[vif1-worker] 10 s: I-bit raised -> EE takes it: %llu, avg %.1f us, max %.1f us; EE takes it -> FBRST.STC: %llu, avg %.1f us, max %.1f us\n",
                     (unsigned long long)nn, nn ? ns / 1e3 / nn : 0.0, noticeMax.exchange(0) / 1e3, (unsigned long long)hn, hn ? hs / 1e3 / hn : 0.0,
                     handlerMax.exchange(0) / 1e3);
    }
    lastStalls = stalls;
    lastStallNs = stallNs;
    lastBusy = busy;
    lastWaits = waits;
    lastWaitNs = waitNs;
    lastParks = parks;
    // The EE's waits by site (QuiesceSite), the sites that waited only.
    static const char *const kSiteNames[] = {"other",         "vu1-memory",  "vif1-read",      "vif1-write",
                                             "vif1-fbrst",    "vif1-dma-end", "gif-dma-start", "gif-pending",
                                             "gif-packet-ram", "gif-packet",  "gif-image-chain", "gif-packed-chain"};
    static_assert(sizeof(kSiteNames) / sizeof(kSiteNames[0]) == kQuiesceSites);
    static uint64_t lastSiteWaits[kQuiesceSites]{}, lastSiteNs[kQuiesceSites]{};
    char line[640];
    int used = std::snprintf(line, sizeof(line), "[vif1-worker] 10 s: EE quiesce waits by site:");
    for (size_t i = 0; i < kQuiesceSites; ++i)
    {
        const uint64_t siteWaits = m_siteWaits[i].load(), siteNs = m_siteNs[i].load();
        if (siteWaits != lastSiteWaits[i] && used > 0 && used < static_cast<int>(sizeof(line)))
            used += std::snprintf(line + used, sizeof(line) - used, " %s %llu (%.1f ms)", kSiteNames[i],
                                  static_cast<unsigned long long>(siteWaits - lastSiteWaits[i]),
                                  static_cast<double>(siteNs - lastSiteNs[i]) / 1e6);
        lastSiteWaits[i] = siteWaits;
        lastSiteNs[i] = siteNs;
    }
    std::fprintf(stderr, "%s\n", line);
    // Async PATH3 (PS2X_ASYNC_PATH3): GIF DMAs run behind the posted VIF1 work.
    static uint64_t lastRuns = 0, lastDelayNs = 0, lastFallbacks = 0;
    const uint64_t runs = m_path3Runs.load(), delayNs = m_path3DelayNs.load(), fallbacks = m_path3Fallbacks.load();
    if (runs != lastRuns || fallbacks != lastFallbacks)
    {
        const uint64_t n = runs - lastRuns;
        std::fprintf(stderr, "[vif1-worker] 10 s: PATH3 posted %llu, post-to-submit avg %.3f ms max %.3f ms; EE fallbacks to the synchronous start %llu\n",
                     static_cast<unsigned long long>(n), n ? static_cast<double>(delayNs - lastDelayNs) / 1e6 / static_cast<double>(n) : 0.0,
                     static_cast<double>(m_path3MaxDelayNs.exchange(0u)) / 1e6, static_cast<unsigned long long>(fallbacks - lastFallbacks));
    }
    lastRuns = runs;
    lastDelayNs = delayNs;
    lastFallbacks = fallbacks;
}

void vif1latNoteNotice() { vif1lat::noteNotice(); }
