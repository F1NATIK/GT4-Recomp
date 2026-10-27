#include "runtime/vblank_clock.h"

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstdlib>
#include <mutex>
#include <thread>
#include <vector>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif
#endif

#if defined(_MSC_VER) || defined(__SSE2__) || defined(__x86_64__)
#include <immintrin.h>
#define PS2X_VBLANK_PAUSE() _mm_pause()
#else
#define PS2X_VBLANK_PAUSE() ((void)0)
#endif

namespace ps2x::vblank
{
namespace
{
std::atomic<uint64_t> g_wakeSerial{0};
std::atomic<bool> g_waiterBlocked{false};

#if defined(_WIN32)
// The event wake() sets while the EE thread is blocked in its timer wait.
HANDLE wakeEvent()
{
    static HANDLE event = ::CreateEventW(nullptr, FALSE, FALSE, nullptr);
    return event;
}

// One waitable timer per waiting thread (EE thread, host loop): high
// resolution where Windows has it (10 1803+), else a plain one (then woken
// 2 ms before the deadline, as the 1 ms tick allows).
struct ThreadTimer
{
    HANDLE handle = nullptr;
    bool highResolution = false;
    ThreadTimer()
    {
        handle = ::CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
        highResolution = handle != nullptr;
        if (!handle)
            handle = ::CreateWaitableTimerExW(nullptr, nullptr, 0, TIMER_ALL_ACCESS);
    }
    ~ThreadTimer()
    {
        if (handle)
            ::CloseHandle(handle);
    }
    ThreadTimer(const ThreadTimer &) = delete;
    ThreadTimer &operator=(const ThreadTimer &) = delete;
};

ThreadTimer &threadTimer()
{
    thread_local ThreadTimer timer;
    return timer;
}
#endif

Clock::duration spinMargin()
{
#if defined(_WIN32)
    return threadTimer().highResolution ? Clock::duration(std::chrono::microseconds(500)) : Clock::duration(std::chrono::milliseconds(2));
#else
    return std::chrono::milliseconds(2);
#endif
}

// Blocks until `until` (the coarse part of a wait), or the wake event when
// `wakeable`. Returns at once if the timer cannot be used.
void coarseWait(Clock::time_point until, bool wakeable)
{
    const auto now = Clock::now();
    if (until <= now)
        return;
#if defined(_WIN32)
    ThreadTimer &timer = threadTimer();
    if (timer.handle)
    {
        const int64_t ticks = std::chrono::duration_cast<std::chrono::nanoseconds>(until - now).count() / 100;
        LARGE_INTEGER due;
        due.QuadPart = -std::max<int64_t>(1, ticks); // relative, 100 ns units
        if (::SetWaitableTimer(timer.handle, &due, 0, nullptr, nullptr, FALSE))
        {
            const HANDLE event = wakeable ? wakeEvent() : nullptr;
            if (event)
            {
                HANDLE handles[2] = {event, timer.handle};
                ::WaitForMultipleObjects(2, handles, FALSE, INFINITE);
            }
            else
                ::WaitForSingleObject(timer.handle, INFINITE);
            return;
        }
    }
#endif
    (void)wakeable;
    std::this_thread::sleep_until(until);
}

struct Grid
{
    std::mutex mutex;
    bool valid = false;
    uint64_t tick = 0;
    Clock::time_point due{}, nextDue{};
};
Grid g_grid;

bool latenessStatsOn()
{
    static const bool on = [] {
        const char *v = std::getenv("PS2X_FPS_LOG");
        return v && *v && std::atof(v) > 0.0;
    }();
    return on;
}

struct Lateness
{
    std::mutex mutex;
    std::vector<float> ms;
    uint64_t rebases = 0;
};
Lateness g_lateness;

struct Presentations
{
    std::atomic<uint64_t> composing{0};
    std::atomic<uint64_t> ready{0};
    std::atomic<int> waiters{0};
    std::mutex mutex;
    std::condition_variable published;
};
Presentations g_presentations;
} // namespace

bool preciseWaits()
{
    static const bool on = [] {
        const char *v = std::getenv("PS2X_PRECISE_VBLANK");
#if defined(PS2X_GAME_GT3)
        return v && *v == '1';
#else
        return !(v && *v == '0');
#endif
    }();
    return on;
}

uint64_t wakeSerial()
{
    return g_wakeSerial.load(std::memory_order_seq_cst);
}

void waitUntilOrWake(Clock::time_point deadline, uint64_t seen)
{
    const auto margin = spinMargin();
    if (deadline - Clock::now() > margin)
    {
        // Blocked: wake() sets the event (seen through g_waiterBlocked; both
        // sides use sequentially consistent order, so a wake between the
        // caller's check and this wait is never lost).
        g_waiterBlocked.store(true, std::memory_order_seq_cst);
        if (g_wakeSerial.load(std::memory_order_seq_cst) == seen)
            coarseWait(deadline - margin, true);
        g_waiterBlocked.store(false, std::memory_order_seq_cst);
        return;
    }
    while (Clock::now() < deadline)
    {
        if (g_wakeSerial.load(std::memory_order_acquire) != seen)
            return;
        for (int i = 0; i < 16; ++i)
            PS2X_VBLANK_PAUSE();
    }
}

void wake()
{
    g_wakeSerial.fetch_add(1, std::memory_order_seq_cst);
#if defined(_WIN32)
    if (g_waiterBlocked.load(std::memory_order_seq_cst))
        if (const HANDLE event = wakeEvent())
            ::SetEvent(event);
#endif
}

void sleepUntil(Clock::time_point deadline)
{
    const auto margin = spinMargin();
    if (deadline - Clock::now() > margin)
        coarseWait(deadline - margin, false);
    while (Clock::now() < deadline)
        for (int i = 0; i < 16; ++i)
            PS2X_VBLANK_PAUSE();
}

void publishGrid(uint64_t tick, Clock::time_point due, Clock::time_point nextDue)
{
    std::lock_guard<std::mutex> lock(g_grid.mutex);
    g_grid.valid = true;
    g_grid.tick = tick;
    g_grid.due = due;
    g_grid.nextDue = nextDue;
}

bool grid(uint64_t &tick, Clock::time_point &due, Clock::time_point &nextDue)
{
    std::lock_guard<std::mutex> lock(g_grid.mutex);
    if (!g_grid.valid)
        return false;
    tick = g_grid.tick;
    due = g_grid.due;
    nextDue = g_grid.nextDue;
    return true;
}

void beginPresentation(uint64_t tick)
{
    g_presentations.composing.store(tick, std::memory_order_relaxed);
}

uint64_t composingPresentation()
{
    return g_presentations.composing.load(std::memory_order_relaxed);
}

void presentationReady(uint64_t tick)
{
    {
        // Stored under the mutex, so a waiter between its check and its wait
        // cannot miss the notification.
        std::lock_guard<std::mutex> lock(g_presentations.mutex);
        g_presentations.ready.store(tick, std::memory_order_release);
    }
    if (g_presentations.waiters.load(std::memory_order_acquire) != 0)
        g_presentations.published.notify_all();
}

uint64_t readyPresentation()
{
    return g_presentations.ready.load(std::memory_order_acquire);
}

bool waitForPresentation(uint64_t tick, Clock::time_point limit)
{
    std::unique_lock<std::mutex> lock(g_presentations.mutex);
    g_presentations.waiters.fetch_add(1, std::memory_order_acq_rel);
    const bool ready = g_presentations.published.wait_until(
        lock, limit, [tick] { return g_presentations.ready.load(std::memory_order_acquire) >= tick; });
    g_presentations.waiters.fetch_sub(1, std::memory_order_acq_rel);
    return ready;
}

void noteLateness(Clock::duration late, bool rebased)
{
    if (!latenessStatsOn())
        return;
    const float ms = static_cast<float>(std::chrono::duration<double, std::milli>(late).count());
    std::lock_guard<std::mutex> lock(g_lateness.mutex);
    if (g_lateness.ms.size() < 65536u)
        g_lateness.ms.push_back(ms);
    g_lateness.rebases += rebased ? 1u : 0u;
}

LatenessWindow takeLateness()
{
    std::vector<float> ms;
    LatenessWindow out;
    {
        std::lock_guard<std::mutex> lock(g_lateness.mutex);
        ms.swap(g_lateness.ms);
        out.rebases = g_lateness.rebases;
        g_lateness.rebases = 0;
    }
    out.count = ms.size();
    if (ms.empty())
        return out;
    std::sort(ms.begin(), ms.end());
    const auto at = [&](double q) {
        const size_t i = static_cast<size_t>(q * static_cast<double>(ms.size() - 1u) + 0.5);
        return static_cast<double>(ms[std::min(i, ms.size() - 1u)]);
    };
    out.p50Ms = at(0.50);
    out.p99Ms = at(0.99);
    out.maxMs = static_cast<double>(ms.back());
    return out;
}

// ------------------------------------------- VBlankStart on the host clock (GT4)
namespace
{
std::atomic<int64_t> g_nextDueNs{0};      // the next VBlankStart's host deadline (steady ns; 0 none)
std::atomic<int64_t> g_requestedDueNs{0}; // the deadline a request was last made for
// The requester and its context, set and called under g_requesterMutex, so
// setHostVBlankRequester(nullptr) (from ~EeScheduler) returns only once no
// call into the scheduler is in flight, and none starts after it.
std::mutex g_requesterMutex;
HostVBlankRequester g_requester = nullptr;
void *g_requesterContext = nullptr;
std::atomic<bool> g_requesterSet{false};
// The 1b timer thread: started with the first requester, stopped and joined
// when the requester is cleared (it never outlives the scheduler). A pointer,
// so a thread still running at exit has no destructor to terminate the process.
std::mutex g_timerControl;
std::thread *g_timer = nullptr;
std::atomic<bool> g_timerStop{false};
struct HostClockStats
{
    std::mutex mutex;
    uint64_t ipu = 0, timer = 0, requests = 0, multiStep = 0;
    double sumCycles = 0.0, maxCycles = 0.0;
    uint32_t maxSteps = 0;
};
HostClockStats g_hostClock;

int64_t toNs(Clock::time_point t)
{
    return std::chrono::duration_cast<std::chrono::nanoseconds>(t.time_since_epoch()).count();
}

bool envOn(const char *name)
{
    const char *v = std::getenv(name);
    return v && *v && *v != '0';
}

// One request per VBlankStart deadline, whichever trigger comes first.
void request(int trigger)
{
    const int64_t due = g_nextDueNs.load(std::memory_order_acquire);
    if (due == 0)
        return;
    int64_t seen = g_requestedDueNs.load(std::memory_order_relaxed);
    if (seen == due || !g_requestedDueNs.compare_exchange_strong(seen, due, std::memory_order_acq_rel))
        return;
    {
        std::lock_guard<std::mutex> lock(g_requesterMutex);
        if (!g_requester)
            return;
        g_requester(g_requesterContext, trigger);
    }
    if (latenessStatsOn())
    {
        std::lock_guard<std::mutex> lock(g_hostClock.mutex);
        ++g_hostClock.requests;
    }
}

// 1b: sleeps to each published VBlankStart deadline and requests it there.
void timerThread()
{
    while (!g_timerStop.load(std::memory_order_acquire))
    {
        if (!g_requesterSet.load(std::memory_order_acquire))
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
            continue;
        }
        const int64_t due = g_nextDueNs.load(std::memory_order_acquire);
        if (due == 0 || g_requestedDueNs.load(std::memory_order_acquire) == due)
        {
            // Requested (or none yet): wait for the scheduler to publish the next one.
            std::this_thread::sleep_for(std::chrono::microseconds(500));
            continue;
        }
        const auto dueAt = Clock::time_point(std::chrono::duration_cast<Clock::duration>(std::chrono::nanoseconds(due)));
        const auto now = Clock::now();
        if (dueAt - now > std::chrono::milliseconds(3))
        {
            // Coarse until close (re-reading the deadline: a rebase moves it).
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            continue;
        }
        if (now < dueAt)
            sleepUntil(dueAt); // the last stretch precisely
        if (g_nextDueNs.load(std::memory_order_acquire) == due)
            request(2);
    }
}
} // namespace

bool ipuVBlankEnabled()
{
#if defined(PS2X_GAME_GT3)
    return false;
#else
    static const bool on = envOn("PS2X_IPU_VBLANK");
    return on;
#endif
}

bool hostClockVBlankEnabled()
{
#if defined(PS2X_GAME_GT3)
    return false;
#else
    static const bool on = envOn("PS2X_VBLANK_HOST_CLOCK");
    return on;
#endif
}

void publishNextVBlankDue(Clock::time_point due)
{
    g_nextDueNs.store(toNs(due), std::memory_order_release);
}

void setHostVBlankRequester(HostVBlankRequester fn, void *context)
{
    {
        // Waits for a request in flight (request() calls under this mutex).
        std::lock_guard<std::mutex> lock(g_requesterMutex);
        g_requester = fn;
        g_requesterContext = fn ? context : nullptr;
        g_requesterSet.store(fn != nullptr, std::memory_order_release);
    }
    std::lock_guard<std::mutex> control(g_timerControl);
    if (fn && hostClockVBlankEnabled() && !g_timer)
    {
        g_timerStop.store(false, std::memory_order_release);
        g_timer = new std::thread(timerThread);
    }
    else if (!fn && g_timer)
    {
        // Not under g_requesterMutex: the thread may be waiting for it in request().
        g_timerStop.store(true, std::memory_order_release);
        g_timer->join(); // its sleeps are at most ~3 ms
        delete g_timer;
        g_timer = nullptr;
    }
}

void noteIpuWork()
{
    if (!ipuVBlankEnabled())
        return;
    const int64_t due = g_nextDueNs.load(std::memory_order_relaxed);
    if (due == 0 || g_requestedDueNs.load(std::memory_order_relaxed) == due || toNs(Clock::now()) < due)
        return;
    request(1);
}

void noteHostClockVBlank(int trigger, uint64_t fastForwardCycles, uint32_t steps)
{
    if (!latenessStatsOn())
        return;
    std::lock_guard<std::mutex> lock(g_hostClock.mutex);
    (trigger == 1 ? g_hostClock.ipu : g_hostClock.timer) += 1u;
    const double cycles = static_cast<double>(fastForwardCycles);
    g_hostClock.sumCycles += cycles;
    g_hostClock.maxCycles = std::max(g_hostClock.maxCycles, cycles);
    g_hostClock.multiStep += steps > 1u ? 1u : 0u;
    g_hostClock.maxSteps = std::max(g_hostClock.maxSteps, steps);
}

HostClockWindow takeHostClock()
{
    std::lock_guard<std::mutex> lock(g_hostClock.mutex);
    HostClockWindow out;
    out.ipu = g_hostClock.ipu;
    out.timer = g_hostClock.timer;
    out.requests = g_hostClock.requests;
    const uint64_t n = out.ipu + out.timer;
    out.avgCycles = n ? g_hostClock.sumCycles / static_cast<double>(n) : 0.0;
    out.maxCycles = g_hostClock.maxCycles;
    out.multiStep = g_hostClock.multiStep;
    out.maxSteps = g_hostClock.maxSteps;
    g_hostClock.ipu = g_hostClock.timer = g_hostClock.requests = g_hostClock.multiStep = 0;
    g_hostClock.sumCycles = g_hostClock.maxCycles = 0.0;
    g_hostClock.maxSteps = 0;
    return out;
}

namespace
{
// EE thread writes, the PS2X_FPS_LOG reader takes (relaxed: counts only).
struct IdleTimers
{
    std::atomic<uint64_t> held{0}, expired{0}, expiredTrailing{0}, charges{0}, chargedCycles{0};
};
IdleTimers g_idleTimers;
} // namespace

void noteIdleTimer(IdleTimerNote what)
{
    if (!latenessStatsOn())
        return;
    switch (what)
    {
    case IdleTimerNote::Held:
        g_idleTimers.held.fetch_add(1u, std::memory_order_relaxed);
        break;
    case IdleTimerNote::Expired:
        g_idleTimers.expired.fetch_add(1u, std::memory_order_relaxed);
        break;
    case IdleTimerNote::ExpiredTrailing:
        g_idleTimers.expiredTrailing.fetch_add(1u, std::memory_order_relaxed);
        break;
    }
}

void noteIdleCharge(uint64_t cycles)
{
    if (!latenessStatsOn())
        return;
    g_idleTimers.charges.fetch_add(1u, std::memory_order_relaxed);
    g_idleTimers.chargedCycles.fetch_add(cycles, std::memory_order_relaxed);
}

IdleTimerWindow takeIdleTimers()
{
    IdleTimerWindow out;
    out.held = g_idleTimers.held.exchange(0u, std::memory_order_relaxed);
    out.expired = g_idleTimers.expired.exchange(0u, std::memory_order_relaxed);
    out.expiredTrailing = g_idleTimers.expiredTrailing.exchange(0u, std::memory_order_relaxed);
    out.charges = g_idleTimers.charges.exchange(0u, std::memory_order_relaxed);
    out.chargedCycles = g_idleTimers.chargedCycles.exchange(0u, std::memory_order_relaxed);
    return out;
}
}
