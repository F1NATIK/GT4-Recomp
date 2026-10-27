#include "runtime/gs/gs_worker.h"
#include "ThreadNaming.h"
#if defined(_M_X64) || defined(__x86_64__)
#include <immintrin.h>
#endif

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>

// GT4_VU1_HANDOFF=1 (GT4 builds; off by default): a VSync presentation posted while the GS side is
// at its frame cap goes into the queue at once (its place in the GS stream unchanged) instead of
// blocking the EE thread until a frame drains. The EE scheduler then holds the guest's threads (not
// its interrupt handlers) while more frames are queued than the cap allows (gsHandoffOver), so the
// EE still runs no further ahead than before, but answers VIF1's I-bit interrupts (GT4's texture
// upload hand-off, ~100 a frame) during the wait: with the blocking post each of those raised then
// waited ~10 ms (i7-9700K, Trial Mountain).
namespace gsHandoff
{
    bool enabled()
    {
#if defined(PS2X_GAME_GT3)
        return false;
#else
        static const bool on = [] {
            const char *v = std::getenv("GT4_VU1_HANDOFF");
            return v && *v == '1';
        }();
        return on;
#endif
    }
    std::atomic<bool> over{false};      // more frames queued than the cap allows: the EE holds its guest threads
    std::atomic<uint32_t> capNow{0};    // the cap of the last post
    std::function<void()> wake;         // set by the runtime: wakes the EE scheduler (an event)
}

GsWorker::~GsWorker()
{
    stop();
}

bool GsWorker::start(ProcessFn fn, std::function<void()> threadStart, std::function<void()> threadExit)
{
    stop();
    m_fn = std::move(fn);
    m_threadStart = std::move(threadStart);
    m_threadExit = std::move(threadExit);
    const char *env = std::getenv("GT3_GS_THREAD");
#if defined(GT4_STOCK_BUILD)
    // The Classic edition always runs this thread: its OpenGL renderer needs it (without it GT4 would
    // fall back to the CPU renderer), whatever the environment says.
    env = nullptr;
#endif
    if (env && *env == '0')
        return false;
    m_stop = false;
    m_submitted = m_processed = 0;
    m_queuedBytes = 0;
    {
        const char *a = std::getenv("GT3_GS_GL_STATS");
        const char *b = std::getenv("GT3_GS_WORKER_STATS");
        m_stats = (a && *a && *a != '0') || (b && *b && *b != '0');
    }
    m_statStart = std::chrono::steady_clock::now();
#if !defined(PS2X_GAME_GT3)
    // GT4: up to 6 frames queued (GT3: 2). With 2, a GL thread held up in the
    // driver (new render targets as the race view appears, the B-Spec
    // hitches) stopped the EE in postFrame inside VBlank processing: late
    // VBlanks and 2-4 VSync frames, i.e. game slow motion. With 6 the lag
    // shows as a repeated picture instead (A/B in
    // analysis/phase5/specs/step4-hitches.md 2.5 and 3.5). The 48 MB byte
    // cap still bounds the queue. GT3_GS_MAX_FRAMES=2 restores the old cap.
    m_maxFrames = 6;
    // While the GL side stays behind, back to 2 (postFrame): the 6 is for
    // stalls, not for a steadily slow GPU (input latency). 0: off.
    m_backlogCap = 2;
    if (const char *v = std::getenv("PS2X_GS_BACKLOG_CAP"); v && *v)
        m_backlogCap = static_cast<uint32_t>(std::strtoul(v, nullptr, 10));
    s_guestStats.store(m_stats, std::memory_order_relaxed);
#endif
    if (const char *v = std::getenv("GT3_GS_MAX_FRAMES"); v && *v)
        m_maxFrames = static_cast<uint32_t>(std::strtoul(v, nullptr, 10));
    m_queuedFrames = 0;
    m_backlogRun = 0;
    m_backlogCapped = false;
    m_running = true;
    m_thread = std::thread([this] { run(); });
    m_threadId = m_thread.get_id();
    return true;
}

void GsWorker::stop()
{
    if (!m_running)
        return;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_stop = true;
    }
    m_workCv.notify_all();
    if (m_thread.joinable())
        m_thread.join();
    m_running = false;
    m_queue.clear();
}

void GsWorker::submit(const uint8_t *data, uint32_t size)
{
    if (!m_running)
    {
        if (m_fn)
            m_fn(data, size);
        return;
    }
    if (std::this_thread::get_id() == m_threadId)
    {
        // A packet produced while processing one (not expected) keeps order.
        m_fn(data, size);
        return;
    }
    std::unique_lock<std::mutex> lock(m_mutex);
    if (m_stats && m_queuedBytes >= kMaxQueuedBytes)
    {
        const auto t = std::chrono::steady_clock::now();
        waitDone(lock, [this] { return m_queuedBytes < kMaxQueuedBytes || m_stop; });
        m_fullNs += sinceNs(t);
    }
    waitDone(lock, [this] { return m_queuedBytes < kMaxQueuedBytes || m_stop; });
    std::vector<uint8_t> packet;
    if (!m_free.empty())
    {
        packet = std::move(m_free.back());
        m_free.pop_back();
    }
    m_queuedBytes += size;
    // The copy runs without the lock (the worker keeps going meanwhile); the
    // packet is queued after it, in submission order (one producer, the EE).
    lock.unlock();
    packet.assign(data, data + size);
    lock.lock();
    m_queue.push_back(Item{std::move(packet), nullptr});
    ++m_submitted;
    m_hasWork.store(true, std::memory_order_release);
    const bool wake = m_workerSleeping;
    lock.unlock();
    if (wake)
        m_workCv.notify_one();
}

struct GsWorker::ProducerBatch
{
    const GsWorker *owner = nullptr;
    std::vector<Item> items;
    std::vector<std::vector<uint8_t>> spares;
    size_t bytes = 0;
};
namespace
{
thread_local GsWorker::ProducerBatch t_batch;
constexpr size_t kBatchItems = 16u;
constexpr size_t kBatchBytes = 256u << 10u;
} // namespace

void GsWorker::beginProducerBatch()
{
    const char *v = std::getenv("GT3_GS_BATCH");
    if (v && *v == '0')
        return;
    t_batch.owner = this;
}

void GsWorker::flushProducerBatch()
{
    if (t_batch.owner != this || t_batch.items.empty())
        return;
    if (!m_running || std::this_thread::get_id() == m_threadId)
    {
        for (Item &item : t_batch.items)
            if (m_fn)
                m_fn(item.packet.data(), static_cast<uint32_t>(item.packet.size()));
        t_batch.items.clear();
        t_batch.bytes = 0;
        return;
    }
    std::unique_lock<std::mutex> lock(m_mutex);
    waitDone(lock, [this] { return m_queuedBytes < kMaxQueuedBytes || m_stop; });
    for (Item &item : t_batch.items)
    {
        m_queuedBytes += item.packet.size();
        m_queue.push_back(std::move(item));
        ++m_submitted;
    }
    while (t_batch.spares.size() < kBatchItems && !m_free.empty())
    {
        t_batch.spares.push_back(std::move(m_free.back()));
        m_free.pop_back();
    }
    m_hasWork.store(true, std::memory_order_release);
    const bool wake = m_workerSleeping;
    lock.unlock();
    if (wake)
        m_workCv.notify_one();
    t_batch.items.clear();
    t_batch.bytes = 0;
}

void GsWorker::submitOwned(std::vector<uint8_t> &packet)
{
    if (!m_running || std::this_thread::get_id() == m_threadId)
    {
        if (m_fn)
            m_fn(packet.data(), static_cast<uint32_t>(packet.size()));
        packet.clear();
        return;
    }
    if (t_batch.owner == this)
    {
        t_batch.bytes += packet.size();
        Item item;
        item.packet.swap(packet);
        if (!t_batch.spares.empty())
        {
            packet = std::move(t_batch.spares.back());
            t_batch.spares.pop_back();
        }
        packet.clear();
        t_batch.items.push_back(std::move(item));
        if (t_batch.items.size() >= kBatchItems || t_batch.bytes >= kBatchBytes)
            flushProducerBatch();
        return;
    }
    const size_t size = packet.size();
    std::unique_lock<std::mutex> lock(m_mutex);
    if (m_stats && m_queuedBytes >= kMaxQueuedBytes)
    {
        const auto t = std::chrono::steady_clock::now();
        waitDone(lock, [this] { return m_queuedBytes < kMaxQueuedBytes || m_stop; });
        m_fullNs += sinceNs(t);
    }
    waitDone(lock, [this] { return m_queuedBytes < kMaxQueuedBytes || m_stop; });
    Item item;
    item.packet.swap(packet);
    if (!m_free.empty())
    {
        packet = std::move(m_free.back());
        m_free.pop_back();
    }
    packet.clear();
    m_queuedBytes += size;
    m_queue.push_back(std::move(item));
    ++m_submitted;
    m_hasWork.store(true, std::memory_order_release);
    const bool wake = m_workerSleeping;
    lock.unlock();
    if (wake)
        m_workCv.notify_one();
}

void GsWorker::sync()
{
    if (!m_running || std::this_thread::get_id() == m_threadId)
        return;
    std::unique_lock<std::mutex> lock(m_mutex);
    const uint64_t ticket = m_submitted;
    const auto t = std::chrono::steady_clock::now();
    waitDone(lock, [this, ticket] { return m_processed >= ticket || m_stop; });
    if (m_stats)
    {
        m_syncNs += sinceNs(t);
        ++m_syncCount;
    }
}

void GsWorker::runOnWorker(const std::function<void()> &task, bool next)
{
    if (!m_running || std::this_thread::get_id() == m_threadId)
    {
        task();
        return;
    }
    std::unique_lock<std::mutex> lock(m_mutex);
    bool done = false;
    const std::function<void()> wrapped = [&task, &done] {
        task();
        done = true;
    };
    if (next)
        m_queue.push_front(Item{{}, &wrapped});
    else
        m_queue.push_back(Item{{}, &wrapped});
    ++m_submitted;
    m_hasWork.store(true, std::memory_order_release);
    const bool wake = m_workerSleeping;
    lock.unlock();
    if (wake)
        m_workCv.notify_one();
    lock.lock();
    // Not "|| m_stop": the worker drains the queue before it exits, and the
    // task lives on this caller's stack until it has run. `done` is written
    // on the worker before it takes the lock to count the item processed.
    const auto t = std::chrono::steady_clock::now();
    waitDone(lock, [&done] { return done; });
    if (m_stats)
    {
        m_syncNs += sinceNs(t);
        ++m_syncCount;
    }
}

void GsWorker::post(std::function<void()> task)
{
    if (!m_running || std::this_thread::get_id() == m_threadId)
    {
        task();
        return;
    }
    std::unique_lock<std::mutex> lock(m_mutex);
    Item item;
    item.owned = std::move(task);
    m_queue.push_back(std::move(item));
    ++m_submitted;
    m_hasWork.store(true, std::memory_order_release);
    const bool wake = m_workerSleeping;
    lock.unlock();
    if (wake)
        m_workCv.notify_one();
}

void GsWorker::reportStats()
{
    const uint64_t elapsed = sinceNs(m_statStart);
    if (elapsed < 10000000000ull)
        return;
    std::fprintf(stderr,
                 "[gs-worker] %.1f s: GS thread idle %.0f%%, producers waited: frame cap %.0f ms, queue full %.0f ms, sync/runOnWorker %.0f ms (%llu)\n",
                 elapsed / 1e9, 100.0 * static_cast<double>(m_idleNs) / static_cast<double>(elapsed), m_frameWaitNs.load() / 1e6,
                 m_fullNs.load() / 1e6, m_syncNs.load() / 1e6, static_cast<unsigned long long>(m_syncCount.load()));
#if !defined(PS2X_GAME_GT3)
    {
        // GT4: the queue depth the EE met at each frame post (2+ = it would
        // have waited under the old cap 2) and what the guest saw of the GS
        // early (step4-hitches.md P5).
        uint64_t posts = 0;
        for (uint64_t n : m_postDepth)
            posts += n;
        const uint64_t events = m_evSignal.load() + m_evFinish.load();
        std::fprintf(stderr,
                     "[gs-worker] frames queued at postFrame: mean %.2f, max %u, 0/1/2/3/4/5/6/7+: %llu/%llu/%llu/%llu/%llu/%llu/%llu/%llu; "
                     "posts under the backlog cap %u: %llu; GS events: SIGNAL %llu, FINISH %llu, GS interrupts raised %llu, "
                     "frames queued behind them mean %.2f max %llu; guest reads: CSR %llu (%llu with SIGNAL/FINISH set), SIGLBLID %llu\n",
                     posts ? static_cast<double>(m_postDepthSum) / static_cast<double>(posts) : 0.0, m_postDepthMax,
                     static_cast<unsigned long long>(m_postDepth[0]), static_cast<unsigned long long>(m_postDepth[1]),
                     static_cast<unsigned long long>(m_postDepth[2]), static_cast<unsigned long long>(m_postDepth[3]),
                     static_cast<unsigned long long>(m_postDepth[4]), static_cast<unsigned long long>(m_postDepth[5]),
                     static_cast<unsigned long long>(m_postDepth[6]), static_cast<unsigned long long>(m_postDepth[7]), m_backlogCap,
                     static_cast<unsigned long long>(m_postsReduced), static_cast<unsigned long long>(m_evSignal.load()),
                     static_cast<unsigned long long>(m_evFinish.load()), static_cast<unsigned long long>(m_evDelivered.load()),
                     events ? static_cast<double>(m_evLagSum.load()) / static_cast<double>(events) : 0.0,
                     static_cast<unsigned long long>(m_evLagMax.load()), static_cast<unsigned long long>(s_csrReads.exchange(0)),
                     static_cast<unsigned long long>(s_csrReadsSet.exchange(0)), static_cast<unsigned long long>(s_sigLblIdReads.exchange(0)));
        for (uint64_t &n : m_postDepth)
            n = 0;
        m_postDepthSum = m_postsReduced = 0;
        m_postDepthMax = 0;
        m_evSignal = m_evFinish = m_evDelivered = m_evLagSum = m_evLagMax = 0;
    }
#endif
    m_frameWaitNs = 0;
    m_idleNs = 0;
    m_fullNs = 0;
    m_syncNs = 0;
    m_syncCount = 0;
    m_statStart = std::chrono::steady_clock::now();
}

std::atomic<bool> GsWorker::s_guestStats{false};
std::atomic<uint64_t> GsWorker::s_csrReads{0}, GsWorker::s_csrReadsSet{0}, GsWorker::s_sigLblIdReads{0};

void GsWorker::noteGsEvent(uint32_t csrBits, bool delivered)
{
    if (!m_stats)
        return;
    uint32_t behind = 0;
    if (m_running)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        behind = m_queuedFrames;
    }
    if (csrBits & 0x1u)
        ++m_evSignal;
    if (csrBits & 0x2u)
        ++m_evFinish;
    if (delivered)
        ++m_evDelivered;
    m_evLagSum += behind;
    if (behind > m_evLagMax.load(std::memory_order_relaxed))
        m_evLagMax.store(behind, std::memory_order_relaxed);
}

void GsWorker::noteGuestRead(uint32_t regOff, uint64_t value)
{
    if (!s_guestStats.load(std::memory_order_relaxed))
        return;
    if (regOff == 0x1000u)
    {
        s_csrReads.fetch_add(1, std::memory_order_relaxed);
        if ((value & 0x3u) != 0u)
            s_csrReadsSet.fetch_add(1, std::memory_order_relaxed);
    }
    else if (regOff == 0x1080u)
        s_sigLblIdReads.fetch_add(1, std::memory_order_relaxed);
}

void GsWorker::postFrame(std::function<void()> task)
{
    if (!m_running || std::this_thread::get_id() == m_threadId)
    {
        task();
        return;
    }
    std::unique_lock<std::mutex> lock(m_mutex);
    uint32_t cap = m_maxFrames;
#if !defined(PS2X_GAME_GT3)
    // GT4: after more than kBacklogPosts frames in a row that found the queue
    // full (the GL side is slower than the EE; a stall of a few frames, or a
    // GL side that is merely busy and drains a backlog slowly, never fills it
    // that long), the cap is m_backlogCap until a frame is posted with fewer
    // than that queued. The EE waits here once for the queue to drain to it.
    if (m_backlogCap != 0u && m_backlogCap < m_maxFrames)
    {
        if (m_backlogCapped)
        {
            if (m_queuedFrames < m_backlogCap)
            {
                m_backlogCapped = false;
                m_backlogRun = 0;
            }
        }
        else
        {
            m_backlogRun = m_queuedFrames >= m_maxFrames ? m_backlogRun + 1u : 0u;
            m_backlogCapped = m_backlogRun > kBacklogPosts;
        }
        if (m_backlogCapped)
            cap = m_backlogCap;
    }
    if (m_stats)
    {
        ++m_postDepth[std::min(m_queuedFrames, 7u)];
        m_postDepthSum += m_queuedFrames;
        m_postDepthMax = std::max(m_postDepthMax, m_queuedFrames);
        if (cap != m_maxFrames)
            ++m_postsReduced;
    }
#endif
    const bool handoff = gsHandoff::enabled();
    if (!handoff && cap != 0u && m_queuedFrames >= cap)
    {
        const auto t = std::chrono::steady_clock::now();
        waitDone(lock, [this, cap] { return m_queuedFrames < cap || m_stop; });
        if (m_stats)
            m_frameWaitNs += sinceNs(t);
    }
    Item item;
    item.owned = std::move(task);
    item.frame = true;
    m_queue.push_back(std::move(item));
    ++m_queuedFrames;
    if (handoff)
    {
        // (The blocking post would have waited until fewer than cap were queued, then queued this one.)
        gsHandoff::capNow.store(cap, std::memory_order_relaxed);
        gsHandoff::over.store(cap != 0u && m_queuedFrames > cap, std::memory_order_release);
    }
    ++m_submitted;
    m_hasWork.store(true, std::memory_order_release);
    const bool wake = m_workerSleeping;
    lock.unlock();
    if (wake)
        m_workCv.notify_one();
}

void GsWorker::run()
{
    ThreadNaming::SetCurrentThreadName("GS");
    ThreadNaming::PreferPerformance();
    if (m_threadStart)
        m_threadStart();
    std::unique_lock<std::mutex> lock(m_mutex);
    for (;;)
    {
        if (m_queue.empty() && !m_stop)
        {
            // Spin briefly before sleeping: packets come every few
            // microseconds while a frame is drawn, and waking this thread from
            // the condition variable cost the producer (the VIF1 worker) a
            // kernel call per packet once the renderer ran on its own thread.
            // GT3_GS_SPIN_US: the spin window (default 100 us).
            static const std::chrono::microseconds spinWindow([] {
                const char *v = std::getenv("GT3_GS_SPIN_US");
                return v && *v ? std::strtol(v, nullptr, 10) : 100L;
            }());
            const auto t = std::chrono::steady_clock::now();
            lock.unlock();
            for (uint32_t i = 1; !m_hasWork.load(std::memory_order_acquire); ++i)
            {
                if ((i & 63u) == 0u && std::chrono::steady_clock::now() - t > spinWindow)
                    break;
#if defined(_M_X64) || defined(__x86_64__)
                _mm_pause();
#endif
            }
            lock.lock();
            if (m_queue.empty() && !m_stop)
            {
                m_workerSleeping = true;
                m_workCv.wait(lock, [this] { return !m_queue.empty() || m_stop; });
                m_workerSleeping = false;
            }
            if (m_stats)
                m_idleNs += sinceNs(t);
        }
        if (m_queue.empty() && m_stop)
            break;
        if (m_stats)
            reportStats();
        Item item = std::move(m_queue.front());
        m_queue.pop_front();
        if (m_queue.empty())
            m_hasWork.store(false, std::memory_order_relaxed);
        lock.unlock();
        if (item.task)
            (*item.task)();
        else if (item.owned)
            item.owned();
        else
            m_fn(item.packet.data(), static_cast<uint32_t>(item.packet.size()));
        lock.lock();
        m_queuedBytes -= item.packet.size();
        if (item.frame && m_queuedFrames > 0u)
            --m_queuedFrames;
        if (item.frame && gsHandoff::over.load(std::memory_order_acquire))
        {
            const uint32_t cap = gsHandoff::capNow.load(std::memory_order_relaxed);
            if (!(cap != 0u && m_queuedFrames > cap))
            {
                gsHandoff::over.store(false, std::memory_order_release);
                if (gsHandoff::wake)
                {
                    lock.unlock();
                    gsHandoff::wake();
                    lock.lock();
                }
            }
        }
        ++m_processed;
        if (!item.task && !item.owned && m_free.size() < 64u)
            m_free.push_back(std::move(item.packet));
        if (m_doneWaiters != 0u)
            m_doneCv.notify_all();
    }
    lock.unlock();
    if (m_threadExit)
        m_threadExit();
}
