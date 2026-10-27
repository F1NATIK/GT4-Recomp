#include "runtime/ee_scheduler.h"
#include "runtime/vblank_clock.h"
#if defined(PS2X_GAME_GT3)
#include "runtime/gt3_movie_guest_output.h"
#include "runtime/gt3_movie_worker.h"
#endif

#include "ps2_log.h"
#include "ps2_runtime_macros.h"
#include "ps2x/iop/iop_subsystem.h"
#include "../ps2_iop_host.h"
#if defined(PS2X_GAME_GT4) && !defined(GT4_STOCK_BUILD)
#include "../gt4_hud_pictures.h" // (HUD hand-off 7 C: a suspended bracketed HUD draw closed at its real return)
#endif

#include <optional>
#include <algorithm>
#include <cstdio>
#include <type_traits>
#include <variant>
#include <unordered_map>
#include <string>
#include <chrono>
#include <cassert>
#include <cstring>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <stdexcept>

// (GuardDiag.cpp: GT3_EE_GUARD's view of the running guest thread)
struct Ps2xGuardRegs
{
    uint32_t pc, ra, sp, a[4], s[4];
};
extern bool (*g_ps2xGuardRegs)(Ps2xGuardRegs &);

namespace vif1lat
{
    extern std::atomic<int> eeWaiting;
    extern std::atomic<int> noticePath;
    extern std::atomic<int> eePacing;
    extern std::atomic<int> eeActivity;
    void noteCheckpointSeen();
}
namespace gsHandoff
{
    bool enabled();
    extern std::atomic<bool> over;
}
static const bool g_vif1LatStats = [] {
    const char *s = std::getenv("GT3_VIF1_THREAD_STATS");
    return s && *s && *s != '0';
}();

// GT3_VIF1_THREAD_STATS: EeDispatcherTransfer throws (the guest stack unwound by a C++ exception), per site, every 10 s.
static void noteDispatcherThrow(int site)
{
    if (!g_vif1LatStats)
        return;
    static uint64_t counts[8] = {};
    static auto windowStart = std::chrono::steady_clock::now();
    ++counts[site & 7];
    const auto now = std::chrono::steady_clock::now();
    if (now - windowStart < std::chrono::seconds(10))
        return;
    windowStart = now;
    std::fprintf(stderr, "[ee-throw] 10 s: dispatcher transfers by site: %llu %llu %llu %llu %llu\n", (unsigned long long)counts[0],
                 (unsigned long long)counts[1], (unsigned long long)counts[2], (unsigned long long)counts[3], (unsigned long long)counts[4]);
    std::fill(std::begin(counts), std::end(counts), 0ull);
}

namespace
{
    constexpr int KE_OK = 0;
    constexpr int KE_ERROR = -1;
    constexpr int KE_ILLEGAL_PRIORITY = -403;
    constexpr int KE_ILLEGAL_THID = -406;
    constexpr int KE_UNKNOWN_THID = -407;
    constexpr int KE_UNKNOWN_SEMID = -408;
    constexpr int KE_UNKNOWN_EVFID = -409;
    constexpr int KE_DORMANT = -413;
    constexpr int KE_NOT_DORMANT = -414;
    constexpr int KE_NOT_SUSPEND = -415;
    constexpr int KE_NOT_WAIT = -416;
    constexpr int KE_RELEASE_WAIT = -418;
    constexpr int KE_SEMA_ZERO = -419;
    constexpr int KE_SEMA_OVF = -420;
    constexpr int KE_EVF_COND = -421;
    constexpr int KE_WAIT_DELETE = -425;

    constexpr uint32_t WEF_OR = 0x01u;
    constexpr uint32_t WEF_CLEAR = 0x10u;
    constexpr uint32_t WEF_CLEAR_ALL = 0x20u;
    constexpr auto kVBlankPeriod = std::chrono::microseconds(16667);
    constexpr auto kVBlankDuration = std::chrono::microseconds(500);
    constexpr auto kMaxVBlankHostLag = 4 * kVBlankPeriod;
    constexpr uint64_t kAlarmTickMicroseconds = 64u;
    constexpr uint32_t kDebugPublishDispatchInterval = 4096u;

#if !defined(PS2X_GAME_GT3)
    // The guest thread that last started running (EE thread only), for
    // PS2X_VBLANK_LATE_LOG when a late VBlank is handled between threads.
    int g_lastRunThreadId = 0;

    // EE timer interrupts in idle waits (analysis/phase5/specs/step6-l1-steploss.md
    // sections 3 and 7). An idle wait that an event ends (the VIF1 worker's
    // I-bit, DMA or PATH3 completion, an IOP reply) charges no EE cycles, so
    // through a frame of such waits the EE cycle clock falls behind wall time.
    // An EE timer interrupt due before the next VBlankStart (in cycles) then
    // fires either with that VBlank's idle fast-forward (events keep ending
    // the waits until its host deadline: the usual case) or, after a quiet gap
    // as long as its remaining cycles, on its own host deadline (now +
    // remaining cycles), mid-period. GT4's main loop sleeps 1 ms on TIMER2
    // after a flip that did not wait (DelayThread 0x5aed18); when that sleep
    // ends mid-period, the next iteration runs across the VBlank's host
    // deadline, the cycle gate holds the VBlank 6-10 ms, and the iteration
    // after it takes two VBlanks for one race step (lost steps at the B-Spec
    // rolling start, 3-4x more often with async PATH3).
    //
    // An idle wait has been ended by an event since the last timeout
    // fast-forward (a deadline or a timer reached). EE thread only.
    bool g_idleEventStretch = false;

    int envInt(const char *name, int fallback)
    {
        const char *v = std::getenv(name);
        return (v && *v) ? std::atoi(v) : fallback;
    }

    // PS2X_IDLE_TIMER_HOLD (fix H; GT4 default 1, 0 off): inside such a
    // stretch, while the cycle clock trails the next VBlankStart, an EE timer
    // interrupt that the VBlank's fast-forward will pass gets no host deadline
    // of its own; it fires with the VBlank (after the VBlank's events, in the
    // same processPendingEvents pass), as it already does whenever events keep
    // coming. 2: whenever the cycle clock trails, also outside an event-ended
    // stretch (the spec's variant).
    int idleTimerHold()
    {
        static const int mode = std::max(0, envInt("PS2X_IDLE_TIMER_HOLD", 1));
        return mode;
    }

    // PS2X_IDLE_CHARGE=1 (alternative F, default off): an idle wait ended by
    // an event charges its wall time as EE cycles, capped one cycle short of
    // the next deadline and the next EE timer interrupt (those still fire on
    // their own timeout): EE timers run at wall rate through idle, as on a PS2.
    bool idleCharge()
    {
        static const bool on = envInt("PS2X_IDLE_CHARGE", 0) != 0;
        return on;
    }

    // PS2X_IDLE_TIMER_PRECISE=1 (default off): an EE timer's host deadline is
    // waited for with the precise VBlank wait (waitable timer + short spin)
    // instead of condition_variable::wait_until, which rounds to whole
    // milliseconds (it woke up to 1.4 ms late, past a VBlank's host deadline).
    // Needs the precise waits (PS2X_PRECISE_VBLANK, on by default), which are
    // what events wake.
    bool idleTimerPrecise()
    {
        static const bool on = envInt("PS2X_IDLE_TIMER_PRECISE", 0) != 0;
        return on;
    }
#endif

    // GT3_BOOT_TRACE, read once: it is checked on every interrupt invocation
    // (getenv showed in the EE thread's profile under queueInvocation).
    bool bootTrace()
    {
        static const bool enabled = std::getenv("GT3_BOOT_TRACE") != nullptr;
        return enabled;
    }

    // GT4: a host-clock VBlank request is pending (requestHostClockVBlank,
    // vblank_clock.h). It ends the EE's host waits as an event does (GT3
    // builds: never).
    inline bool hostVBlankArmed(const std::atomic<int> &trigger)
    {
#if defined(PS2X_GAME_GT3)
        (void)trigger;
        return false;
#else
        return trigger.load(std::memory_order_acquire) != 0;
#endif
    }

    // Waits (lock held on entry and on return) until `deadline` or until
    // `ready` holds, true when it does: a high-resolution timer, then a short
    // spin (runtime/vblank_clock.h). condition_variable::wait_until rounds to
    // whole milliseconds and woke VBlanks 0-2.4 ms late
    // (analysis/phase5/specs/step3-pacing.md 2.4). postEvent / requestStop
    // wake it (ps2x::vblank::wake).
    template <typename Ready>
    bool preciseWaitUntil(std::unique_lock<std::mutex> &lock, std::chrono::steady_clock::time_point deadline, Ready ready)
    {
        for (;;)
        {
            if (ready())
                return true;
            if (std::chrono::steady_clock::now() >= deadline)
                return false;
            const uint64_t seen = ps2x::vblank::wakeSerial();
            lock.unlock();
            ps2x::vblank::waitUntilOrWake(deadline, seen);
            lock.lock();
        }
    }

    constexpr uint64_t microsecondsToEeCycles(uint64_t microseconds)
    {
        return (microseconds * EeScheduler::kEeClockHz + 999999ull) / 1000000ull;
    }

    std::chrono::nanoseconds eeCyclesToHostDuration(uint64_t cycles)
    {
        constexpr uint64_t kNanosecondsPerSecond = 1000000000ull;
        const uint64_t wholeSeconds = cycles / EeScheduler::kEeClockHz;
        const uint64_t remainingCycles = cycles % EeScheduler::kEeClockHz;
        const uint64_t remainingNanoseconds = (remainingCycles * kNanosecondsPerSecond + EeScheduler::kEeClockHz - 1u) / EeScheduler::kEeClockHz;
        return std::chrono::seconds(wholeSeconds) + std::chrono::nanoseconds(remainingNanoseconds);
    }

    // Test runs only: GT3_TURBO=<n> paces VSyncs n times faster on the host
    // clock (menus, movies and loads race ahead) until VSync GT3_TURBO_UNTIL
    // (default: always), then real time. Guest timing is still in VSyncs, so pad
    // replays line up; audio and alarms stay on real time. GT3_TURBO_FROM=<VSync>
    // starts it later (menus at real speed: under turbo a menu frame can span
    // several VSyncs, so replayed menu presses land on other screens).
    std::chrono::nanoseconds vblankHostPeriod(uint64_t tick)
    {
        struct Turbo
        {
            uint32_t factor;
            uint64_t from, until;
        };
        static const Turbo turbo = []
        {
            const char *factor = std::getenv("GT3_TURBO");
            const char *from = std::getenv("GT3_TURBO_FROM");
            const char *until = std::getenv("GT3_TURBO_UNTIL");
            const uint32_t n = factor ? static_cast<uint32_t>(std::strtoul(factor, nullptr, 10)) : 1u;
            const uint64_t begin = from && *from ? std::strtoull(from, nullptr, 10) : 0ull;
            const uint64_t end = until && *until ? std::strtoull(until, nullptr, 10) : ~0ull;
            if (n > 1u)
                std::cerr << "[ee] turbo: VSyncs " << n << "x faster from VSync " << begin << " until VSync " << end << std::endl;
            return Turbo{std::max(n, 1u), begin, end};
        }();
        const std::chrono::nanoseconds period = kVBlankPeriod;
        return tick >= turbo.from && tick < turbo.until ? period / turbo.factor : period;
    }

    constexpr uint64_t kVBlankPeriodCycles = microsecondsToEeCycles(16667u);
    constexpr uint64_t kVBlankDurationCycles = microsecondsToEeCycles(500u);
    constexpr uint64_t kAlarmTickCycles = microsecondsToEeCycles(kAlarmTickMicroseconds);

    bool hasActiveInterrupt(const GuestThread *owner)
    {
        // Generated checkpoints unwind the host call stack while the guest
        // interrupt frame remains active. Also cover callbacks nested in it.
        return owner && std::any_of(owner->invocations.begin(), owner->invocations.end(),
            [](const GuestInvocation &invocation) {
                return invocation.kind == GuestInvocationKind::Interrupt;
            });
    }

    template <typename Map>
    int allocatePositiveId(int &nextId, const Map &objects)
    {
        const int first = std::max(1, nextId);
        int candidate = first;
        do
        {
            if (!objects.contains(candidate))
            {
                nextId = (candidate == std::numeric_limits<int>::max()) ? 1 : candidate + 1;
                return candidate;
            }
            candidate = (candidate == std::numeric_limits<int>::max()) ? 1 : candidate + 1;
        } while (candidate != first);
        return 0;
    }
}

EeScheduler::EeScheduler(PS2Runtime &runtime)
    : m_runtime(runtime)
{
#if !defined(PS2X_GAME_GT3)
    // VBlankStart on the host clock (L2n / 1b, vblank_clock.h), when switched on.
    if (ps2x::vblank::ipuVBlankEnabled() || ps2x::vblank::hostClockVBlankEnabled())
    {
        ps2x::vblank::setHostVBlankRequester(
            [](void *self, int trigger) { static_cast<EeScheduler *>(self)->requestHostClockVBlank(trigger); }, this);
        std::cerr << "[vblank] VBlankStart on the host clock:" << (ps2x::vblank::ipuVBlankEnabled() ? " IPU work (PS2X_IPU_VBLANK)" : "")
                  << (ps2x::vblank::hostClockVBlankEnabled() ? " timer (PS2X_VBLANK_HOST_CLOCK)" : "") << std::endl;
    }
#endif
}

EeScheduler::~EeScheduler()
{
#if !defined(PS2X_GAME_GT3)
    if (ps2x::vblank::ipuVBlankEnabled() || ps2x::vblank::hostClockVBlankEnabled())
        ps2x::vblank::setHostVBlankRequester(nullptr, nullptr);
#endif
    requestStop();
}

void EeScheduler::requestHostClockVBlank(int trigger) noexcept
{
    {
        // Under the event mutex, as postEvent stores: processPendingEvents
        // decides m_checkpointPending under it, so a request from the 1b timer
        // thread cannot fall between its check and its store and be lost.
        std::lock_guard lock(m_eventMutex);
        int none = 0;
        m_hostVBlankTrigger.compare_exchange_strong(none, trigger, std::memory_order_acq_rel);
        m_checkpointPending.store(true, std::memory_order_release);
    }
    // An idle or pacing host wait ends for it (they check hostVBlankArmed).
    m_eventCv.notify_one();
    if (ps2x::vblank::preciseWaits())
        ps2x::vblank::wake();
}

// The VBlankStart whose host deadline has passed (requestHostClockVBlank) is
// made due by the cycle rule too: the EE cycles (and EE timers) advance to its
// deadline cycle. Like an idle wait (waitForEvent), it advances one event at a
// time: to the earliest of the next scheduled deadline, the next EE timer
// interrupt and the VBlank's deadline cycle. Until it reaches the VBlank it
// re-arms itself (the trigger stays set and the checkpoint pending), so the
// deadlines and timer interrupts on the way are queued in cycle order before
// it, in passes of their own. Every deadline at or before the cycle it reaches
// is due on the host clock now too (its host deadline is clamped to now): it
// fires in cycle order in this pass and never holds the EE in
// processDueDeadlines' pacing wait (an alarm set from wall time can have a
// host deadline still ahead although the VBlank after it is due).
void EeScheduler::deliverHostClockVBlank(int trigger)
{
    uint64_t step = 0;
    bool more = false;
    {
        std::lock_guard lock(m_eventMutex);
        const auto now = std::chrono::steady_clock::now();
        bool found = false;
        uint64_t vblankCycle = 0;
        for (const ScheduledEvent &item : m_deadlines)
            if (item.event.type == EeEventType::VBlankStart && item.hostDeadline <= now && (!found || item.deadlineCycle < vblankCycle))
            {
                vblankCycle = item.deadlineCycle;
                found = true;
            }
        if (!found)
        {
            m_hostVBlankFastForward = 0; // not due on the host clock (yet): nothing to do
            m_hostVBlankSteps = 0;
            return;
        }
        uint64_t target = std::max(vblankCycle, m_eeCycle);
        if (vblankCycle > m_eeCycle)
        {
            for (const ScheduledEvent &item : m_deadlines)
                if (item.deadlineCycle > m_eeCycle && item.deadlineCycle < target)
                    target = item.deadlineCycle;
            const uint64_t timerCycles = m_runtime.memory().cyclesUntilNextEeTimerInterrupt(); // >= 1
            if (timerCycles < target - m_eeCycle)
                target = m_eeCycle + timerCycles;
        }
        // (Up to the VBlank: deadlines after it keep their host pacing.)
        const uint64_t dueTo = std::min(target, vblankCycle);
        for (ScheduledEvent &item : m_deadlines)
            if (item.deadlineCycle <= dueTo && item.hostDeadline > now)
                item.hostDeadline = now;
        step = target - m_eeCycle;
        more = target < vblankCycle;
        if (more)
        {
            int none = 0;
            m_hostVBlankTrigger.compare_exchange_strong(none, trigger, std::memory_order_acq_rel);
            m_checkpointPending.store(true, std::memory_order_release);
        }
    }
    for (uint64_t remaining = step; remaining > 0u;)
    {
        const uint32_t part = static_cast<uint32_t>(std::min<uint64_t>(remaining, std::numeric_limits<uint32_t>::max()));
        accountCycles(part);
        remaining -= part;
    }
    m_hostVBlankFastForward += step;
    m_hostVBlankSteps += step != 0u ? 1u : 0u;
    if (more)
        return;
    // The VBlank is due by its cycles now (or already was).
    if (m_hostVBlankFastForward != 0u)
        ps2x::vblank::noteHostClockVBlank(trigger, m_hostVBlankFastForward, m_hostVBlankSteps);
    m_hostVBlankFastForward = 0;
    m_hostVBlankSteps = 0;
}

void EeScheduler::reset(uint8_t *rdram, const R5900Context &mainContext)
{
#if defined(PS2X_GAME_GT3)
    ps2x::video::stopGt3MovieOutputsBeforeEeReset(m_runtime);
    ps2x::video::beginGt3MovieWorker(m_runtime);
#endif
    m_executorThread = std::this_thread::get_id();
    m_rdram = rdram;
    m_readyQueues = {};
    m_threads.clear();
    m_invocationStackTops.clear();
    m_freeInvocationStacks.clear();
    m_runtime.resetAsyncCallbackStacks();
    m_semaphores.clear();
    m_eventFlags.clear();
    m_alarms.clear();
    m_intcHandlers.clear();
    m_dmacHandlers.clear();
    m_nextThreadId = kFirstThreadId;
    m_nextInvocationThreadId = -1;
    m_nextSemaphoreId = 1;
    m_nextEventFlagId = 1;
    m_nextAlarmId = 1;
    m_nextIntcHandlerId = 1;
    m_nextDmacHandlerId = 1;
    m_intcHeadOrder = 0;
    m_intcTailOrder = 1000;
    m_dmacHeadOrder = 0;
    m_dmacTailOrder = 1000;
    m_enabledIntcMask = 0xFFFFFFFFu;
    m_enabledDmacMask = 0xFFFFFFFFu;
    m_currentThreadId = 0;
    m_rescheduleRequested = false;
    m_timeSliceExpired = false;
    m_insideInterrupt = false;
    m_pendingEeTimerInterrupts = 0u;
    m_eeCycle = 0u;
    m_sliceEndCycle = kDefaultTimeSliceCycles;
    m_stopRequested.store(false, std::memory_order_release);
    m_checkpointPending.store(false, std::memory_order_release);
    m_debugPublishCountdown = 0u;
    {
        std::lock_guard lock(m_eventMutex);
        m_events.clear();
        m_deadlines.clear();
        m_pendingInvocations.clear();
        m_hostVBlankTrigger.store(0, std::memory_order_release);
    }
    m_hostVBlankFastForward = 0;
    m_hostVBlankSteps = 0;
    m_eventSequence = 0;
    m_invocationSequence = 0;
    m_vsyncTick = 0;
    m_vsyncFlagAddress = 0;
    m_vsyncTickAddress = 0;
    m_gsVSyncCallback = 0;
    m_gsVSyncCallbackGp = 0;
    m_gsVSyncCallbackSp = 0;
    m_runtime.memory().gs().vsyncTick.store(0u, std::memory_order_release);
    m_runtime.memory().resetEeTimers();

    GuestThread main{};
    main.id = kMainThreadId;
    main.context = mainContext;
    m_vu0LiveContext = nullptr;
    storeSharedVu0(mainContext);
    main.entry = mainContext.pc;
    // $sp is live execution state, not the stable initial stack descriptor
    // returned by ReferThreadStatus. SetupThread records that metadata.
    main.stack = 0u;
    main.gp = getRegU32(&mainContext, 28);
    main.initialPriority = 0;
    main.currentPriority = 0;
    main.status = EeThreadStatus::Ready;
    m_threads.emplace(main.id, std::move(main));
    m_readyQueues[0].push_back(kMainThreadId);
    scheduleEvent(m_eeCycle + kVBlankPeriodCycles,
                  std::chrono::steady_clock::now() + kVBlankPeriod,
                  EeEvent{EeEventType::VBlankStart, 0, 0});
    publishSnapshot();
}

void EeScheduler::run()
{
    assertExecutor();
    m_running.store(true, std::memory_order_release);

    while (!m_stopRequested.load(std::memory_order_acquire))
    {
        processPendingEvents();
        if (m_stopRequested.load(std::memory_order_acquire))
        {
            break;
        }
        // GT4_VU1_HANDOFF (gs_worker.cpp): while the GS side has more VSync frames queued than its cap,
        // the guest's threads wait here, as the blocking post made the whole EE wait; interrupt
        // handlers and other invocations still run (VIF1's I-bit hand-off). At most 100 ms at a time
        // (a safety net: logged, then the thread runs).
        static auto heldSince = std::chrono::steady_clock::time_point{};
        if (gsHandoff::enabled() && !gsHandoff::over.load(std::memory_order_acquire))
            heldSince = std::chrono::steady_clock::time_point{};
        if (gsHandoff::enabled() && gsHandoff::over.load(std::memory_order_acquire))
        {
            GuestThread *holder = m_currentThreadId != 0 ? currentThread() : nullptr;
            // (Queued interrupt handlers and other invocations run: they start from m_pendingInvocations.)
            const bool invocationNext = !m_pendingInvocations.empty() || (holder && !holder->invocations.empty());
            if (!invocationNext)
            {
                const auto now = std::chrono::steady_clock::now();
                if (heldSince == std::chrono::steady_clock::time_point{})
                    heldSince = now;
                if (now - heldSince < std::chrono::milliseconds(100))
                {
                    struct WaitingMark
                    {
                        WaitingMark() { vif1lat::eeWaiting.store(1, std::memory_order_relaxed); }
                        ~WaitingMark() { vif1lat::eeWaiting.store(0, std::memory_order_relaxed); }
                    } waitingMark;
                    // GT3_VIF1_THREAD_STATS: the time the guest's threads are held (every 10 s).
                    static auto statStart = now;
                    static double heldMs = 0.0;
                    const auto waitStart = std::chrono::steady_clock::now();
                    struct HeldTimer
                    {
                        std::chrono::steady_clock::time_point start;
                        ~HeldTimer()
                        {
                            if (!g_vif1LatStats)
                                return;
                            const auto end = std::chrono::steady_clock::now();
                            heldMs += std::chrono::duration<double, std::milli>(end - start).count();
                            if (end - statStart >= std::chrono::seconds(10))
                            {
                                std::fprintf(stderr, "[gs-handoff] 10 s: guest threads held for the GS side %.0f ms\n", heldMs);
                                heldMs = 0.0;
                                statStart = end;
                            }
                        }
                    } heldTimer{waitStart};
                    std::unique_lock lock(m_eventMutex);
                    m_eventCv.wait_for(lock, std::chrono::microseconds(500), [this] {
                        return !m_events.empty() || m_stopRequested.load(std::memory_order_acquire) ||
                               !gsHandoff::over.load(std::memory_order_acquire) || m_checkpointPending.load(std::memory_order_acquire);
                    });
                    continue;
                }
                static uint32_t released = 0;
                if (released++ < 20u)
                    std::cerr << "[gs-handoff] guest threads held 100 ms for the GS side: let run (vsync " << m_vsyncTick << ")" << std::endl;
                heldSince = std::chrono::steady_clock::time_point{};
            }
        }
        // Livelock detector: a million passes in a row with no guest code run
        // (reported once, with the scheduler state that keeps it looping).
        if (++m_loopSpins == 1000000u)
            reportLivelock();

        if (m_currentThreadId == 0)
        {
            GuestThread *next = selectReady();
            if (!next && m_pendingInvocations.empty())
            {
                publishSnapshot();
                // Opt-in diagnostic: GT3_EE_IDLE_LOG=1 attributes the time every
                // guest thread is blocked to what they wait on (every 5 s).
                static const bool idleLog = [] {
                    const char *v = std::getenv("GT3_EE_IDLE_LOG");
                    return v && *v == '1';
                }();
                if (!idleLog)
                {
                    waitForEvent();
                    continue;
                }
                static std::unordered_map<std::string, double> idleByWait;
                static auto windowStart = std::chrono::steady_clock::now();
                static double busySeconds = 0.0;
                static auto lastWake = std::chrono::steady_clock::now();
                std::string key;
                for (const auto &[id, thread] : m_threads)
                {
                    if (thread.status != EeThreadStatus::Waiting && thread.status != EeThreadStatus::WaitingSuspended)
                        continue;
                    char part[96];
                    const int waitId = std::visit([](const auto &payload) -> int {
                        using T = std::decay_t<decltype(payload)>;
                        if constexpr (std::is_same_v<T, EeSemaphoreWait> || std::is_same_v<T, EeEventFlagWait>)
                            return payload.id;
                        else if constexpr (std::is_same_v<T, EeExternalWait>)
                            return static_cast<int>(payload.type);
                        else
                            return -1;
                    }, thread.wait.payload);
                    std::snprintf(part, sizeof(part), " t%d:%d/%d@%x", id, static_cast<int>(thread.wait.reason), waitId,
                                  getRegU32(&thread.context, 31));
                    key += part;
                }
                const auto start = std::chrono::steady_clock::now();
                busySeconds += std::chrono::duration<double>(start - lastWake).count();
                waitForEvent();
                lastWake = std::chrono::steady_clock::now();
                idleByWait[key] += std::chrono::duration<double>(lastWake - start).count();
                const double window = std::chrono::duration<double>(lastWake - windowStart).count();
                if (window >= 5.0)
                {
                    std::vector<std::pair<std::string, double>> rows(idleByWait.begin(), idleByWait.end());
                    std::sort(rows.begin(), rows.end(), [](const auto &a, const auto &b) { return a.second > b.second; });
                    double idle = 0.0;
                    for (const auto &row : rows)
                        idle += row.second;
                    std::cerr << "[ee-idle] tick=" << m_vsyncTick << " idle " << 100.0 * idle / window << "% of " << window << " s (reason codes: 1 sleep 2 sema 3 evflag 4 vsync 5 external 6 mpeg; @ra)" << std::endl;
                    for (size_t i = 0; i < rows.size() && i < 6u; ++i)
                        std::cerr << "[ee-idle]   " << 100.0 * rows[i].second / window << "%" << rows[i].first << std::endl;
                    idleByWait.clear();
                    busySeconds = 0.0;
                    windowStart = lastWake;
                }
                continue;
            }
            if (next)
            {
                makeRunning(*next);
            }
            else
            {
                GuestThread *owner = &acquireInvocationThread();
                GuestInvocation invocation = std::move(m_pendingInvocations.front());
                m_pendingInvocations.pop_front();
                owner->status = EeThreadStatus::Running;
                m_currentThreadId = owner->id;
#if !defined(PS2X_GAME_GT3)
                g_lastRunThreadId = owner->id; // PS2X_VBLANK_LATE_LOG
#endif
                renewTimeSlice();
                if (getRegU32(&invocation.context, 29) == 0u)
                {
                    SET_GPR_U32(&invocation.context, 29, invocationStackTop());
                }
                owner->invocations.push_back(std::move(invocation));
            }
        }

        GuestThread *running = currentThread();
        assert(running != nullptr);
        if (running->resumeCompletion)
        {
            auto completion = std::move(running->resumeCompletion);
            running->resumeCompletion = {};
            try
            {
                completion(running->activeContext());
            }
            catch (const EeDispatcherTransfer &)
            {
            }
            if (m_currentThreadId == 0)
            {
                continue;
            }
        }
        R5900Context &context = running->activeContext();
        if (m_debugPublishCountdown == 0u)
        {
            copyMainContextToRuntime();
            publishSnapshot();
            m_debugPublishCountdown = kDebugPublishDispatchInterval - 1u;
        }
        else
        {
            --m_debugPublishCountdown;
        }

        m_runtime.m_debugPc.store(context.pc, std::memory_order_relaxed);
        m_runtime.m_debugRa.store(getRegU32(&context, 31), std::memory_order_relaxed);
        m_runtime.m_debugSp.store(getRegU32(&context, 29), std::memory_order_relaxed);
        m_runtime.m_debugGp.store(getRegU32(&context, 28), std::memory_order_relaxed);

        if (context.pc == 0u)
        {
            if (!running->invocations.empty())
            {
                GuestInvocation completed = std::move(running->invocations.back());
                running->invocations.pop_back();
                if (completed.onComplete)
                {
                    try
                    {
                        completed.onComplete(completed.context, running->activeContext());
                    }
                    catch (const EeDispatcherTransfer &)
                    {
                    }
                }
                continue;
            }
            makeDormant(*running);
            m_currentThreadId = 0;
            continue;
        }

        if (!m_pendingInvocations.empty())
        {
            GuestInvocation invocation = std::move(m_pendingInvocations.front());
            m_pendingInvocations.pop_front();
            if (getRegU32(&invocation.context, 29) == 0u)
            {
                SET_GPR_U32(&invocation.context, 29, invocationStackTop());
            }
            running->invocations.push_back(std::move(invocation));
            continue;
        }

        if (!m_runtime.hasFunction(context.pc))
        {
            if (!running->invocations.empty())
            {
                context.pc = 0u;
            }
            else
            {
                m_runtime.reportMissingFunction(m_rdram,
                                                &context,
                                                context.pc,
                                                context.pc,
                                                PS2Runtime::GuestBranchKind::DirectJump,
                                                "EE scheduler");
                makeDormant(*running);
                m_currentThreadId = 0;
            }
            continue;
        }
        PS2Runtime::RecompiledFunction function = m_runtime.lookupFunction(context.pc);
        const uint32_t dispatchedPc=context.pc;
#if defined(PS2X_GAME_GT4) && !defined(GT4_STOCK_BUILD)
        // (HUD hand-off 7 C: a bracketed RaceDisplay member draw that yielded returns to its caller here: its bracket closed)
        if (gt4hudpic::pendingReturns())
            gt4hudpic::atDispatch(m_rdram, context.pc, getRegU32(&context, 29));
#endif

        if (checkpointDue(kGuestDispatchCycles))
        {
            continue;
        }
        m_loopSpins = 0;
        {
            DispatchRecord &record = m_dispatchRing[m_dispatchCount++ % m_dispatchRing.size()];
            record.thread = m_currentThreadId;
            record.pc = context.pc;
            record.depth = static_cast<uint16_t>(running->invocations.size());
            record.kind = running->invocations.empty() ? 0xFFu : static_cast<uint8_t>(running->invocations.back().kind);
            record.vsync = static_cast<uint32_t>(m_vsyncTick);
        }

        try
        {
            m_insideInterrupt = !running->invocations.empty() && running->invocations.back().kind == GuestInvocationKind::Interrupt;
            m_guestExecuting.store(true, std::memory_order_release);
            loadSharedVu0(context);
            m_vu0LiveContext = &context;
            // Resuming a call that yielded before its callee ran: a native stub
            // returns with pc unchanged; continue at the fallthrough, as
            // dispatchGuestBranch does for a direct call. Otherwise the stub
            // (GT3's sceSifCheckStatRpc poll, priority 4) re-ran forever and
            // starved every lower-priority thread.
            bool resumedCall = false;
            uint32_t resumedFallthrough = 0;
            const size_t depth = running->invocations.size();
            if (depth < running->yieldCalls.size())
            {
                GuestThread::YieldCall &call = running->yieldCalls[depth];
                resumedCall = call.target != 0u && call.target == context.pc;
                resumedFallthrough = call.fallthrough;
                call = {};
                // Deeper records belonged to invocations that have finished.
                for (size_t k = depth + 1u; k < running->yieldCalls.size(); ++k)
                    running->yieldCalls[k] = {};
            }
            PS2Runtime::exchangeEeYieldTaken(false);
            function(m_rdram, &context, &m_runtime);
            const bool yielded = PS2Runtime::exchangeEeYieldTaken(false);
            if (resumedCall && !yielded && context.pc == dispatchedPc)
                context.pc = resumedFallthrough;
            flushLiveVu0();
            if (context.pc==0 && running->invocations.empty() && bootTrace()) {
                std::cerr<<"[boot-trace:zero-pc] thread="<<running->id<<" dispatched=0x"<<std::hex<<dispatchedPc
                    <<" ra=0x"<<getRegU32(&context,31)<<" sp=0x"<<getRegU32(&context,29)
                    <<" v0=0x"<<getRegU32(&context,2)<<std::dec<<std::endl;
            }
            m_guestExecuting.store(false, std::memory_order_release);
            m_insideInterrupt = false;
        }
        catch (const EeDispatcherTransfer &)
        {
            // A transfer that did not already flush (a wait, a yield) leaves the
            // live VU0 registers in the context that was running.
            flushLiveVu0();
            m_guestExecuting.store(false, std::memory_order_release);
            m_insideInterrupt = false;
        }
        catch (...)
        {
            flushLiveVu0();
            m_guestExecuting.store(false, std::memory_order_release);
            m_running.store(false, std::memory_order_release);
            publishSnapshot();
            throw;
        }

        processPendingEvents();
    }

    m_guestExecuting.store(false, std::memory_order_release);
    m_running.store(false, std::memory_order_release);
    copyMainContextToRuntime();
    publishSnapshot();
    if (bootTrace()) {
        std::cerr<<"[boot-trace:stopped] pending-callbacks="<<m_pendingInvocations.size()<<std::hex
            <<" intc-mask=0x"<<m_enabledIntcMask<<" dmac-mask=0x"<<m_enabledDmacMask<<std::dec<<std::endl;
        std::cerr<<"[boot-trace:vif1] "<<m_runtime.memory().describeVif1()<<std::endl;
        for (uint64_t i = m_dispatchCount > m_dispatchRing.size() ? m_dispatchCount - m_dispatchRing.size() : 0; i < m_dispatchCount; ++i) {
            const DispatchRecord &record = m_dispatchRing[i % m_dispatchRing.size()];
            std::cerr<<"[boot-trace:dispatch] thread="<<record.thread<<" pc=0x"<<std::hex<<record.pc<<std::dec<<" depth="<<record.depth
                <<" kind="<<unsigned(record.kind)<<" vsync="<<record.vsync<<std::endl;
        }
        for (const auto& [id,thread]:m_threads) {
            std::cerr<<"[boot-trace:context] thread="<<id<<" parent-pc=0x"<<std::hex<<thread.context.pc
                <<" parent-ra=0x"<<getRegU32(&thread.context,31)<<" active-pc=0x"<<thread.activeContext().pc
                <<std::dec<<" callbacks="<<thread.invocations.size()
                <<" status="<<static_cast<int>(thread.status)<<" wait="<<static_cast<int>(thread.wait.reason)
                <<" wakeups="<<thread.wakeupCount<<" prio="<<thread.currentPriority
                <<std::hex<<" cop0_status=0x"<<thread.activeContext().cop0_status<<std::dec;
            if (thread.wait.reason==EeWaitReason::Semaphore)
                std::cerr<<" sema="<<std::get<EeSemaphoreWait>(thread.wait.payload).id;
            std::cerr<<std::endl;
            const auto sp=getRegU32(&thread.context,29)&0x1fffffffu;
            if (sp<PS2_RAM_SIZE && 0x80u<=PS2_RAM_SIZE-sp) {
                std::cerr<<"[boot-trace:parent-stack] thread="<<id<<" sp=0x"<<std::hex<<sp<<" words=";
                for (uint32_t offset=0;offset<0x80;offset+=4) {
                    uint32_t word=0;std::memcpy(&word,m_rdram+sp+offset,4);std::cerr<<' '<<word;
                }
                std::cerr<<std::dec<<std::endl;
            }
        }
    }
}

// While the VIF1 worker runs a stretch of VIF1 input, its next I-bit interrupt
// or DMA completion is due within microseconds, and the EE's handlers resume
// it: a condition-variable sleep and wake (tens of microseconds on Windows,
// about 50 times a GT3 frame) sat on that path. Spin up to 50 us (or until
// limit) for an event first (300 us before 2026-09-29: the same frame rates at
// 2x progressive and 4x, and a quarter of a core less on this thread).
// Called with lock held; true when an event (or a stop) arrived.
bool EeScheduler::spinForEvent(std::unique_lock<std::mutex> &lock, std::chrono::steady_clock::time_point limit)
{
    if (!m_runtime.memory().vif1WorkerRunning())
        return false;
    // GT3_EE_SPIN_US: the spin window (default 50 us).
    static const auto spinWindow = std::chrono::microseconds([] {
        const char *v = std::getenv("GT3_EE_SPIN_US");
        return v && *v ? std::strtol(v, nullptr, 10) : 50L;
    }());
    lock.unlock();
    const auto start = std::chrono::steady_clock::now();
    const auto until = std::min(limit, start + spinWindow);
    for (uint32_t i = 1;; ++i)
    {
        if (m_checkpointPending.load(std::memory_order_acquire) || m_stopRequested.load(std::memory_order_acquire))
            break;
        if ((i & 63u) == 0u && std::chrono::steady_clock::now() >= until)
            break;
#if defined(_MSC_VER) || defined(__SSE2__) || defined(__x86_64__)
        _mm_pause();
#endif
    }
    lock.lock();
    return !m_events.empty() || m_stopRequested.load(std::memory_order_acquire);
}

void EeScheduler::noteYieldAtCall(uint32_t target, uint32_t fallthrough)
{
    GuestThread *thread = currentThread();
    if (!thread)
        return;
    const size_t depth = thread->invocations.size();
    if (depth < thread->yieldCalls.size())
        thread->yieldCalls[depth] = {target, fallthrough};
}

void EeScheduler::reportLivelock()
{
    std::lock_guard lock(m_eventMutex);
    const auto now = std::chrono::steady_clock::now();
    std::cerr << "[ee-livelock] current thread " << m_currentThreadId << " events " << m_events.size() << " pending-invocations "
              << m_pendingInvocations.size() << " ee-cycle " << m_eeCycle << " next-deadline " << m_nextDeadlineCycle.load()
              << " slice-end " << m_sliceEndCycle << " timers 0x" << std::hex << m_pendingEeTimerInterrupts << std::dec
              << " reschedule " << m_rescheduleRequested << " checkpoint " << m_checkpointPending.load() << std::endl;
    for (const ScheduledEvent &item : m_deadlines)
        std::cerr << "[ee-livelock]   deadline type " << static_cast<int>(item.event.type) << " id " << item.event.id << " cycle "
                  << item.deadlineCycle << " host-in " << std::chrono::duration<double, std::milli>(item.hostDeadline - now).count()
                  << " ms" << std::endl;
    for (const EeEvent &event : m_events)
        std::cerr << "[ee-livelock]   event type " << static_cast<int>(event.type) << " id " << event.id << std::endl;
    for (int p = 0; p < kPriorityCount; ++p)
        for (int id : m_readyQueues[static_cast<size_t>(p)])
            std::cerr << "[ee-livelock]   ready prio " << p << " thread " << id << std::endl;
    for (const auto &[id, thread] : m_threads)
        std::cerr << "[ee-livelock]   thread " << id << " status " << static_cast<int>(thread.status) << " wait "
                  << static_cast<int>(thread.wait.reason) << " prio " << thread.currentPriority << " invocations "
                  << thread.invocations.size() << " pc 0x" << std::hex << thread.activeContext().pc << std::dec
                  << (thread.resumeCompletion ? " resume-completion" : "") << std::endl;
}

void EeScheduler::requestStop()
{
    m_stopRequested.store(true, std::memory_order_release);
    m_checkpointPending.store(true, std::memory_order_release);
    m_eventCv.notify_all();
    if (ps2x::vblank::preciseWaits())
        ps2x::vblank::wake();
}

void EeScheduler::postEvent(EeEvent event)
{
    if (event.type == EeEventType::Stop)
    {
        requestStop();
        return;
    }

    {
        std::lock_guard lock(m_eventMutex);
        m_events.push_back(event);
        m_checkpointPending.store(true, std::memory_order_release);
    }
    m_eventCv.notify_one();
    if (ps2x::vblank::preciseWaits())
        ps2x::vblank::wake();
}

bool EeScheduler::checkpointDue(uint32_t cycles) noexcept
{
    accountCycles(cycles);

    if (m_checkpointPending.load(std::memory_order_acquire) ||
        m_stopRequested.load(std::memory_order_acquire))
    {
        if (g_vif1LatStats)
            vif1lat::noteCheckpointSeen();
        return true;
    }

    const uint64_t nextEventCycle = m_nextDeadlineCycle.load(std::memory_order_acquire);
    if (nextEventCycle != 0u && m_eeCycle >= nextEventCycle)
    {
        m_checkpointPending.store(true, std::memory_order_release);
        return true;
    }

    if (m_eeCycle < m_sliceEndCycle)
    {
        return false;
    }

    const GuestThread *running = currentThread();
    // Event/stop checkpoints above still run. A time slice must not repeatedly
    // yield the unfinished interrupt or select a newly awakened base thread.
    if (hasActiveInterrupt(running))
    {
        return false;
    }
    if (running != nullptr && hasReadyAtOrAbovePriority(running->currentPriority))
    {
        m_rescheduleRequested = true;
        m_timeSliceExpired = true;
        return true;
    }

    renewTimeSlice();
    return false;
}

void EeScheduler::accountCycles(uint32_t cycles) noexcept
{
    const uint64_t elapsed = std::max<uint64_t>(1u, cycles);
    m_eeCycle += elapsed;
    m_pendingEeTimerInterrupts |= m_runtime.memory().advanceEeTimers(elapsed);
    if (m_pendingEeTimerInterrupts != 0u)
    {
        m_checkpointPending.store(true, std::memory_order_release);
    }
}

bool EeScheduler::isExecutingGuest() const noexcept
{
    return m_guestExecuting.load(std::memory_order_acquire);
}

void EeScheduler::setupCurrentThread(uint32_t stack, uint32_t stackSize, uint32_t gp)
{
    assertExecutor();
    GuestThread *target = currentThread();
    if (!target)
    {
        return;
    }

    target->stack = stack;
    target->stackSize = stackSize;
    target->gp = gp;
    publishSnapshot();
}

int EeScheduler::createThread(const EeThreadCreateParams &params)
{
    assertExecutor();
    if (params.priority < 1 || params.priority >= kPriorityCount)
    {
        return KE_ILLEGAL_PRIORITY;
    }

    const int id = allocateThreadId();
    if (id == 0)
    {
        return KE_ERROR;
    }

    GuestThread thread{};
    thread.id = id;
    thread.entry = params.entry;
    thread.stack = params.stack;
    thread.stackSize = params.stackSize;
    thread.gp = params.gp;
    thread.attr = params.attr;
    thread.option = params.option;
    thread.initialPriority = params.priority;
    thread.currentPriority = params.priority;
    thread.status = EeThreadStatus::Dormant;
    m_threads.emplace(id, std::move(thread));
    publishSnapshot();
    return id;
}

int EeScheduler::deleteThread(int id, uint32_t &ownedStack)
{
    assertExecutor();
    ownedStack = 0;
    if (id <= kMainThreadId)
    {
        return KE_ILLEGAL_THID;
    }
    auto it = m_threads.find(id);
    if (it == m_threads.end())
    {
        return KE_UNKNOWN_THID;
    }
    if (it->second.status != EeThreadStatus::Dormant)
    {
        return KE_NOT_DORMANT;
    }
    if (it->second.ownsStack)
    {
        ownedStack = it->second.stack;
    }
    m_threads.erase(it);
    releaseInvocationStacks(id);
    publishSnapshot();
    return KE_OK;
}

int EeScheduler::startThread(int id, uint32_t arg, const R5900Context &caller, bool interruptSafe)
{
    assertExecutor();
    GuestThread *target = thread(id);
    if (!target)
    {
        return KE_UNKNOWN_THID;
    }
    if (target->status != EeThreadStatus::Dormant)
    {
        return KE_NOT_DORMANT;
    }

    target->context = R5900Context{};
    target->context.pc = target->entry;
    target->arg = arg;
    target->suspendCount = 0;
    target->wakeupCount = 0;
    target->wait = {};
    SET_GPR_U32(&target->context, 4, arg);
    SET_GPR_U32(&target->context, 28, target->gp != 0u ? target->gp : getRegU32(&caller, 28));
    const uint32_t stackTop = target->stack != 0u
                                  ? (target->stack + target->stackSize) & ~0xFu
                                  : getRegU32(&caller, 29);
    SET_GPR_U32(&target->context, 29, stackTop);
    SET_GPR_U32(&target->context, 31, 0u);
    enqueueReady(*target);
    requestPreemptionIfHigher(*target, interruptSafe);
    publishSnapshot();
    return KE_OK;
}

[[noreturn]] void EeScheduler::exitCurrent(bool deleteThreadRecord)
{
    assertExecutor();
    flushLiveVu0();
    GuestThread *exiting = currentThread();
    assert(exiting != nullptr);
    const int id = exiting->id;
    const uint32_t ownedStack = deleteThreadRecord && exiting->ownsStack ? exiting->stack : 0u;
    makeDormant(*exiting);
    m_currentThreadId = 0;
    if (deleteThreadRecord && id != kMainThreadId)
    {
        m_threads.erase(id);
        releaseInvocationStacks(id);
    }
    if (ownedStack != 0u)
    {
        m_runtime.guestFree(ownedStack);
    }
    publishSnapshot();
    noteDispatcherThrow(0);
    throw EeDispatcherTransfer{};
}

int EeScheduler::terminateThread(int id, uint32_t &ownedStack, bool interruptSafe)
{
    assertExecutor();
    ownedStack = 0;
    if (id == 0 || id == m_currentThreadId)
    {
        return KE_ILLEGAL_THID;
    }
    GuestThread *target = thread(id);
    if (!target)
    {
        return KE_UNKNOWN_THID;
    }
    if (target->status == EeThreadStatus::Dormant)
    {
        return KE_DORMANT;
    }
    if (target->ownsStack)
    {
        ownedStack = target->stack;
        target->ownsStack = false;
    }
    makeDormant(*target);
    (void)interruptSafe;
    publishSnapshot();
    return KE_OK;
}

int EeScheduler::suspendThread(int id, bool interruptSafe)
{
    assertExecutor();
    if (id == 0)
    {
        id = m_currentThreadId;
    }
    GuestThread *target = thread(id);
    if (!target)
    {
        return KE_UNKNOWN_THID;
    }
    if (target->status == EeThreadStatus::Dormant)
    {
        return KE_DORMANT;
    }

    ++target->suspendCount;
    switch (target->status)
    {
    case EeThreadStatus::Running:
        target->status = EeThreadStatus::Suspended;
        m_currentThreadId = 0;
        m_rescheduleRequested = true;
        break;
    case EeThreadStatus::Ready:
        removeReady(*target);
        target->status = EeThreadStatus::Suspended;
        break;
    case EeThreadStatus::Waiting:
        target->status = EeThreadStatus::WaitingSuspended;
        break;
    case EeThreadStatus::WaitingSuspended:
    case EeThreadStatus::Suspended:
        break;
    case EeThreadStatus::Dormant:
        break;
    }
    if (interruptSafe && m_insideInterrupt)
    {
        m_rescheduleRequested = true;
    }
    publishSnapshot();
    return KE_OK;
}

int EeScheduler::resumeThread(int id, bool interruptSafe)
{
    assertExecutor();
    if (id == 0)
    {
        return KE_ILLEGAL_THID;
    }
    GuestThread *target = thread(id);
    if (!target)
    {
        return KE_UNKNOWN_THID;
    }
    if (target->suspendCount == 0)
    {
        return KE_NOT_SUSPEND;
    }
    --target->suspendCount;
    if (target->suspendCount != 0)
    {
        return KE_OK;
    }
    if (target->status == EeThreadStatus::WaitingSuspended)
    {
        target->status = EeThreadStatus::Waiting;
    }
    else if (target->status == EeThreadStatus::Suspended)
    {
        enqueueReady(*target);
        requestPreemptionIfHigher(*target, interruptSafe);
    }
    publishSnapshot();
    return KE_OK;
}

void EeScheduler::sleepCurrent()
{
    assertExecutor();
    GuestThread *self = currentThread();
    assert(self != nullptr);
    if (self->wakeupCount != 0u)
    {
        --self->wakeupCount;
        setReturnS32(&self->activeContext(), KE_OK);
        return;
    }
    blockCurrent(EeWaitState{EeWaitReason::Sleep, std::monostate{}});
}

int EeScheduler::wakeupThread(int id, bool interruptSafe)
{
    assertExecutor();
    if (id == 0 || id == m_currentThreadId)
    {
        return KE_ILLEGAL_THID;
    }
    GuestThread *target = thread(id);
    if (!target)
    {
        return KE_UNKNOWN_THID;
    }
    if (target->status == EeThreadStatus::Dormant)
    {
        return KE_DORMANT;
    }
    if ((target->status == EeThreadStatus::Waiting || target->status == EeThreadStatus::WaitingSuspended) &&
        target->wait.reason == EeWaitReason::Sleep)
    {
        makeReady(*target, KE_OK, interruptSafe);
    }
    else
    {
        ++target->wakeupCount;
    }
    publishSnapshot();
    return KE_OK;
}

int EeScheduler::cancelWakeup(int id)
{
    assertExecutor();
    if (id == 0)
    {
        id = m_currentThreadId;
    }
    GuestThread *target = thread(id);
    if (!target)
    {
        return KE_UNKNOWN_THID;
    }
    const int old = static_cast<int>(target->wakeupCount);
    target->wakeupCount = 0;
    publishSnapshot();
    return old;
}

int EeScheduler::changePriority(int id, int priority, bool interruptSafe, int &oldPriority)
{
    assertExecutor();
    if (priority < 1 || priority >= kPriorityCount)
    {
        return KE_ILLEGAL_PRIORITY;
    }
    if (id == 0)
    {
        id = m_currentThreadId;
    }
    GuestThread *target = thread(id);
    if (!target)
    {
        return KE_UNKNOWN_THID;
    }
    oldPriority = target->currentPriority;
    if (target->status == EeThreadStatus::Ready)
    {
        removeReady(*target);
        target->currentPriority = priority;
        enqueueReady(*target);
        requestPreemptionIfHigher(*target, interruptSafe);
    }
    else
    {
        target->currentPriority = priority;
        if (target->status == EeThreadStatus::Running)
        {
            for (int p = 0; p < target->currentPriority; ++p)
            {
                if (!m_readyQueues[p].empty())
                {
                    m_rescheduleRequested = true;
                    break;
                }
            }
        }
    }
    publishSnapshot();
    return KE_OK;
}

int EeScheduler::rotateReadyQueue(int priority, bool interruptSafe)
{
    assertExecutor();
    if (priority == 0)
    {
        const GuestThread *self = currentThread();
        priority = self ? self->currentPriority : 0;
    }
    if (priority < 0 || priority >= kPriorityCount)
    {
        return KE_ILLEGAL_PRIORITY;
    }

    GuestThread *self = currentThread();
    if (self && self->currentPriority == priority)
    {
        enqueueReady(*self);
        m_currentThreadId = 0;
        m_rescheduleRequested = true;
    }
    else
    {
        auto &queue = m_readyQueues[priority];
        if (queue.size() > 1u)
        {
            const int head = queue.front();
            queue.pop_front();
            queue.push_back(head);
        }
    }
    (void)interruptSafe;
    publishSnapshot();
    return KE_OK;
}

int EeScheduler::releaseWait(int id, bool interruptSafe)
{
    assertExecutor();
    if (id == 0)
    {
        return KE_ILLEGAL_THID;
    }
    GuestThread *target = thread(id);
    if (!target)
    {
        return KE_UNKNOWN_THID;
    }
    if (target->status != EeThreadStatus::Waiting && target->status != EeThreadStatus::WaitingSuspended)
    {
        return KE_NOT_WAIT;
    }
    removeFromWaitObject(*target);
    makeReady(*target, KE_RELEASE_WAIT, interruptSafe);
    publishSnapshot();
    return KE_OK;
}

void EeScheduler::transferIfRequested(bool interruptSafe)
{
    assertExecutor();
    if (interruptSafe || m_insideInterrupt || hasActiveInterrupt(currentThread()) || !m_rescheduleRequested)
    {
        return;
    }
    if (m_currentThreadId != 0)
    {
        GuestThread *self = currentThread();
        assert(self != nullptr);
        enqueueReady(*self, true);
        m_currentThreadId = 0;
    }
    m_rescheduleRequested = false;
    m_timeSliceExpired = false;
    publishSnapshot();
    noteDispatcherThrow(1);
    throw EeDispatcherTransfer{};
}

int EeScheduler::createSemaphore(int initCount, int maxCount, uint32_t attr, uint32_t option)
{
    assertExecutor();
    if (maxCount <= 0 || initCount < 0 || initCount > maxCount)
    {
        return KE_ERROR;
    }
    const int id = allocatePositiveId(m_nextSemaphoreId, m_semaphores);
    if (id == 0)
    {
        return KE_ERROR;
    }
    EeSemaphore semaphore{};
    semaphore.id = id;
    semaphore.count = initCount;
    semaphore.maxCount = maxCount;
    semaphore.initCount = initCount;
    semaphore.attr = attr;
    semaphore.option = option;
    m_semaphores.emplace(id, std::move(semaphore));
    publishSnapshot();
    return id;
}

int EeScheduler::deleteSemaphore(int id, bool interruptSafe)
{
    assertExecutor();
    auto it = m_semaphores.find(id);
    if (it == m_semaphores.end())
    {
        return KE_UNKNOWN_SEMID;
    }
    std::deque<int> waiters = std::move(it->second.waiters);
    m_semaphores.erase(it);
    for (const int threadId : waiters)
    {
        if (GuestThread *waiter = thread(threadId))
        {
            makeReady(*waiter, KE_WAIT_DELETE, interruptSafe);
        }
    }
    publishSnapshot();
    return id;
}

int EeScheduler::signalSemaphore(int id, bool interruptSafe)
{
    assertExecutor();
    EeSemaphore *object = semaphore(id);
    if (!object)
    {
        return KE_UNKNOWN_SEMID;
    }
    if (!object->waiters.empty())
    {
        const int waiterId = object->waiters.front();
        object->waiters.pop_front();
        GuestThread *waiter = thread(waiterId);
        assert(waiter != nullptr);
        makeReady(*waiter, id, interruptSafe);
        publishSnapshot();
        return id;
    }
    if (object->count == object->maxCount)
    {
        return KE_SEMA_OVF;
    }
    ++object->count;
    publishSnapshot();
    return id;
}

int EeScheduler::pollSemaphore(int id)
{
    assertExecutor();
    EeSemaphore *object = semaphore(id);
    if (!object)
    {
        return KE_UNKNOWN_SEMID;
    }
    if (object->count == 0)
    {
        return KE_SEMA_ZERO;
    }
    --object->count;
    publishSnapshot();
    return id;
}

void EeScheduler::waitSemaphore(int id)
{
    assertExecutor();
    EeSemaphore *object = semaphore(id);
    if (!object)
    {
        GuestThread *self = currentThread();
        assert(self != nullptr);
        setReturnS32(&self->activeContext(), KE_UNKNOWN_SEMID);
        return;
    }
    if (object->count != 0)
    {
        --object->count;
        GuestThread *self = currentThread();
        assert(self != nullptr);
        setReturnS32(&self->activeContext(), id);
        publishSnapshot();
        return;
    }
    GuestThread *self = currentThread();
    assert(self != nullptr);
    object->waiters.push_back(self->id);
    blockCurrent(EeWaitState{EeWaitReason::Semaphore, EeSemaphoreWait{id}});
}

int EeScheduler::createEventFlag(uint32_t initialBits, uint32_t attr, uint32_t option)
{
    assertExecutor();
    const int id = allocatePositiveId(m_nextEventFlagId, m_eventFlags);
    if (id == 0)
    {
        return KE_ERROR;
    }
    EeEventFlag flag{};
    flag.id = id;
    flag.attr = attr;
    flag.option = option;
    flag.initBits = initialBits;
    flag.bits = initialBits;
    m_eventFlags.emplace(id, std::move(flag));
    publishSnapshot();
    return id;
}

int EeScheduler::deleteEventFlag(int id, bool interruptSafe)
{
    assertExecutor();
    auto it = m_eventFlags.find(id);
    if (it == m_eventFlags.end())
    {
        return KE_UNKNOWN_EVFID;
    }
    std::deque<int> waiters = std::move(it->second.waiters);
    m_eventFlags.erase(it);
    for (const int threadId : waiters)
    {
        if (GuestThread *waiter = thread(threadId))
        {
            makeReady(*waiter, KE_WAIT_DELETE, interruptSafe);
        }
    }
    publishSnapshot();
    return KE_OK;
}

int EeScheduler::setEventFlag(int id, uint32_t bits, bool interruptSafe)
{
    assertExecutor();
    EeEventFlag *flag = eventFlag(id);
    if (!flag)
    {
        return KE_UNKNOWN_EVFID;
    }
    flag->bits |= bits;
    finishEventWaiters(*flag, interruptSafe);
    publishSnapshot();
    return KE_OK;
}

int EeScheduler::clearEventFlag(int id, uint32_t mask)
{
    assertExecutor();
    EeEventFlag *flag = eventFlag(id);
    if (!flag)
    {
        return KE_UNKNOWN_EVFID;
    }
    flag->bits &= mask;
    publishSnapshot();
    return KE_OK;
}

int EeScheduler::pollEventFlag(int id, uint32_t bits, uint32_t mode, uint32_t &observedBits)
{
    assertExecutor();
    EeEventFlag *flag = eventFlag(id);
    if (!flag)
    {
        return KE_UNKNOWN_EVFID;
    }
    if (!eventCondition(flag->bits, bits, mode))
    {
        return KE_EVF_COND;
    }
    observedBits = flag->bits;
    if ((mode & WEF_CLEAR_ALL) != 0u)
    {
        flag->bits = 0;
    }
    else if ((mode & WEF_CLEAR) != 0u)
    {
        flag->bits &= ~bits;
    }
    publishSnapshot();
    return KE_OK;
}

void EeScheduler::waitEventFlag(int id, uint32_t bits, uint32_t mode, uint32_t resultAddress)
{
    assertExecutor();
    EeEventFlag *flag = eventFlag(id);
    GuestThread *self = currentThread();
    assert(self != nullptr);
    if (!flag)
    {
        setReturnS32(&self->activeContext(), KE_UNKNOWN_EVFID);
        return;
    }
    if (eventCondition(flag->bits, bits, mode))
    {
        const uint32_t observed = flag->bits;
        writeGuestU32(resultAddress, observed);
        if ((mode & WEF_CLEAR_ALL) != 0u)
        {
            flag->bits = 0;
        }
        else if ((mode & WEF_CLEAR) != 0u)
        {
            flag->bits &= ~bits;
        }
        setReturnS32(&self->activeContext(), KE_OK);
        publishSnapshot();
        return;
    }
    flag->waiters.push_back(self->id);
    blockCurrent(EeWaitState{EeWaitReason::EventFlag,
                             EeEventFlagWait{id, bits, mode, resultAddress}});
}

int EeScheduler::setAlarm(uint16_t ticks,
                          uint32_t handler,
                          uint32_t argument,
                          uint32_t gp,
                          uint32_t sp)
{
    assertExecutor();
    if (handler == 0u || !m_runtime.hasFunction(handler))
    {
        return KE_ERROR;
    }
    const int id = allocatePositiveId(m_nextAlarmId, m_alarms);
    if (id == 0)
    {
        return KE_ERROR;
    }
    m_alarms.emplace(id, EeAlarm{id, ticks, handler, argument, gp, sp});
    const uint64_t tickCount = ticks == 0u ? 1u : static_cast<uint64_t>(ticks);
    scheduleEvent(m_eeCycle + tickCount * kAlarmTickCycles,
                  std::chrono::steady_clock::now() + std::chrono::microseconds(tickCount * kAlarmTickMicroseconds),
                  EeEvent{EeEventType::Alarm, static_cast<uint32_t>(id), 0});
    return id;
}

int EeScheduler::cancelAlarm(int id)
{
    assertExecutor();
    if (m_alarms.erase(id) == 0u)
    {
        return KE_ERROR;
    }
    {
        std::lock_guard lock(m_eventMutex);
        std::erase_if(m_deadlines, [id](const ScheduledEvent &scheduled)
                      { return scheduled.event.type == EeEventType::Alarm &&
                               scheduled.event.id == static_cast<uint32_t>(id); });
        updateNextDeadline();
    }
    return KE_OK;
}

void EeScheduler::queueInvocation(GuestInvocation invocation)
{
    static unsigned traced=0;
    if (bootTrace() && traced++<12) {
        const auto* owner=currentThread();
        std::cerr<<"[boot-trace:queue-callback] kind="<<unsigned(invocation.kind)
            <<" pc=0x"<<std::hex<<invocation.context.pc
            <<" sp=0x"<<getRegU32(&invocation.context,29)
            <<" live-sp=0x"<<(owner?getRegU32(&owner->activeContext(),29):0u)
            <<std::dec<<std::endl;
    }
    assertExecutor();
    invocation.sequence = ++m_invocationSequence;
    m_pendingInvocations.push_back(std::move(invocation));
    m_checkpointPending.store(true, std::memory_order_release);
}

[[noreturn]] void EeScheduler::invokeCurrent(GuestInvocation invocation)
{
    assertExecutor();
    flushLiveVu0();
    GuestThread *owner = currentThread();
    assert(owner != nullptr);
    if (getRegU32(&invocation.context, 29) == 0u)
    {
        SET_GPR_U32(&invocation.context, 29, invocationStackTop());
    }
    invocation.sequence = ++m_invocationSequence;
    owner->invocations.push_back(std::move(invocation));
    publishSnapshot();
    noteDispatcherThrow(2);
    throw EeDispatcherTransfer{};
}

[[noreturn]] void EeScheduler::invokeCurrentSequence(std::vector<GuestInvocation> invocations)
{
    assertExecutor();
    flushLiveVu0();
    GuestThread *owner = currentThread();
    assert(owner != nullptr);
    assert(!invocations.empty());
    for (auto it = invocations.rbegin(); it != invocations.rend(); ++it)
    {
        if (getRegU32(&it->context, 29) == 0u)
        {
            SET_GPR_U32(&it->context, 29, invocationStackTop());
        }
        it->sequence = ++m_invocationSequence;
        owner->invocations.push_back(std::move(*it));
    }
    publishSnapshot();
    noteDispatcherThrow(3);
    throw EeDispatcherTransfer{};
}

bool EeScheduler::hasInvocation(GuestInvocationKind kind, uint64_t tag) const
{
    const GuestThread *owner = currentThread();
    if (!owner)
    {
        return false;
    }
    return std::any_of(owner->invocations.begin(), owner->invocations.end(),
                       [kind, tag](const GuestInvocation &invocation)
                       {
                           return invocation.kind == kind && invocation.tag == tag;
                       });
}

uint32_t EeScheduler::invocationStackTop()
{
    assertExecutor();
    const GuestThread *owner = currentThread();
    if (!owner)
    {
        throw std::logic_error("EE invocation stack requested without a current guest context");
    }
    const size_t depth = owner ? owner->invocations.size() : 0u;
    const uint64_t key = (static_cast<uint64_t>(static_cast<uint32_t>(owner->id)) << 32u) |
                         static_cast<uint32_t>(depth);
    const auto existing = m_invocationStackTops.find(key);
    if (existing != m_invocationStackTops.end())
    {
        return existing->second;
    }
#if defined(PS2X_GAME_GT4)
    // GT4 nests callbacks up to 6 deep on several threads at once (boot trace:
    // threads 1, 11, 15 and interrupt context), which the 32-stack arena of
    // 16 KiB stacks cannot hold. GT4 runs most of its threads on 8 KiB stacks,
    // so 8 KiB per invocation level is enough and doubles the count to 64.
    constexpr uint32_t kInvocationStackSize = 0x2000u;
#else
    constexpr uint32_t kInvocationStackSize = 0x4000u;
#endif
    uint32_t top = 0u;
#if defined(PS2X_GAME_GT4)
    // GT4 runs ~20 threads that each nest invocations up to 6 deep. Before
    // taking new arena space, return stacks whose level is no longer live on
    // their thread (level >= that thread's current invocation count): a
    // finished invocation's stack holds nothing. Live levels keep theirs.
    if (m_freeInvocationStacks.empty())
    {
        for (auto it = m_invocationStackTops.begin(); it != m_invocationStackTops.end();)
        {
            const int tid = static_cast<int>(static_cast<uint32_t>(it->first >> 32u));
            const size_t level = static_cast<size_t>(static_cast<uint32_t>(it->first));
            const GuestThread *t = thread(tid);
            if (!t || t->invocations.size() <= level)
            {
                m_freeInvocationStacks.push_back(it->second);
                it = m_invocationStackTops.erase(it);
            }
            else
            {
                ++it;
            }
        }
    }
#endif
    if (!m_freeInvocationStacks.empty())
    {
        top = m_freeInvocationStacks.back();
        m_freeInvocationStacks.pop_back();
    }
    else
    {
        top = m_runtime.reserveAsyncCallbackStack(kInvocationStackSize, 16u);
    }
    if (bootTrace()) {
        std::cerr<<"[boot-trace:callback-stack] thread="<<owner->id<<" depth="<<depth
            <<" callback-top=0x"<<std::hex<<top<<" live-sp=0x"<<getRegU32(&owner->activeContext(),29)
            <<" guest-stack=0x"<<owner->stack<<" bytes=0x"<<owner->stackSize<<std::dec<<std::endl;
    }
    if (top == 0u)
    {
        throw std::runtime_error("EE invocation stack space exhausted");
    }
    m_invocationStackTops.emplace(key, top);
    return top;
}

void EeScheduler::releaseInvocationStacks(int threadId)
{
    // A deleted thread runs nothing, so its stacks can serve other threads. GT3
    // starts fresh movie decoder threads for every menu movie; without reuse the
    // 32-stack arena ran out a few screens into Arcade mode.
    for (auto it = m_invocationStackTops.begin(); it != m_invocationStackTops.end();)
    {
        if (static_cast<int>(static_cast<uint32_t>(it->first >> 32u)) == threadId)
        {
            m_freeInvocationStacks.push_back(it->second);
            it = m_invocationStackTops.erase(it);
        }
        else
        {
            ++it;
        }
    }
}

int EeScheduler::addIrqHandler(bool dmac,
                               uint32_t cause,
                               uint32_t handler,
                               bool append,
                               uint32_t argument,
                               uint32_t gp,
                               uint32_t sp)
{
    assertExecutor();
    auto &handlers = dmac ? m_dmacHandlers : m_intcHandlers;
    int &nextId = dmac ? m_nextDmacHandlerId : m_nextIntcHandlerId;
    const int id = allocatePositiveId(nextId, handlers);
    if (id == 0)
    {
        return KE_ERROR;
    }
    int &head = dmac ? m_dmacHeadOrder : m_intcHeadOrder;
    int &tail = dmac ? m_dmacTailOrder : m_intcTailOrder;
    handlers.emplace(id,
                     EeIrqHandler{id,
                                  cause,
                                  handler,
                                  argument,
                                  gp,
                                  sp,
                                  true,
                                  append ? ++tail : --head});
    return id;
}

int EeScheduler::removeIrqHandler(bool dmac, uint32_t cause, int id)
{
    assertExecutor();
    auto &handlers = dmac ? m_dmacHandlers : m_intcHandlers;
    auto it = handlers.find(id);
    if (it != handlers.end() && it->second.cause == cause)
    {
        handlers.erase(it);
    }
    return KE_OK;
}

int EeScheduler::setIrqHandlerEnabled(bool dmac, int id, bool enabled)
{
    assertExecutor();
    auto &handlers = dmac ? m_dmacHandlers : m_intcHandlers;
    auto it = handlers.find(id);
    if (it != handlers.end())
    {
        it->second.enabled = enabled;
    }
    return KE_OK;
}

int EeScheduler::setIrqCauseEnabled(bool dmac, uint32_t cause, bool enabled)
{
    assertExecutor();
    if (cause < 32u)
    {
        uint32_t &mask = dmac ? m_enabledDmacMask : m_enabledIntcMask;
        if (enabled)
        {
            mask |= 1u << cause;
        }
        else
        {
            mask &= ~(1u << cause);
        }
    }
    return KE_OK;
}

void EeScheduler::dispatchIrq(bool dmac, uint32_t cause)
{
    assertExecutor();
    const uint32_t mask = dmac ? m_enabledDmacMask : m_enabledIntcMask;
    if (cause < 32u && (mask & (1u << cause)) == 0u)
    {
        static uint32_t reported = 0;
        if (reported < 40u)
        {
            ++reported;
            std::cerr << "[ee-irq] " << (dmac ? "DMAC" : "INTC") << " cause " << cause << " dropped: masked (vsync "
                      << m_vsyncTick << ")" << std::endl;
        }
        return;
    }
    const auto &handlers = dmac ? m_dmacHandlers : m_intcHandlers;
    std::vector<EeIrqHandler> matching;
    for (const auto &[id, handler] : handlers)
    {
        (void)id;
        if (handler.enabled && handler.cause == cause && handler.handler != 0u &&
            m_runtime.hasFunction(handler.handler))
        {
            matching.push_back(handler);
        }
    }
    std::sort(matching.begin(), matching.end(), [](const EeIrqHandler &left, const EeIrqHandler &right)
              { return left.order < right.order; });
    for (const EeIrqHandler &handler : matching)
    {
        GuestInvocation invocation{};
        invocation.kind = GuestInvocationKind::Interrupt;
        invocation.context.pc = handler.handler;
        SET_GPR_U32(&invocation.context, 4, cause);
        SET_GPR_U32(&invocation.context, 5, handler.argument);
        SET_GPR_U32(&invocation.context, 28, handler.gp);
        // Registration captures GP, not an enduring stack frame. Leave SP zero
        // so dispatch assigns the owner's isolated callback stack at its depth.
        SET_GPR_U32(&invocation.context, 31, 0u);
        queueInvocation(std::move(invocation));
    }
}

void EeScheduler::setVSyncFlag(uint32_t flagAddress, uint32_t tickAddress)
{
    assertExecutor();
    m_vsyncFlagAddress = flagAddress;
    m_vsyncTickAddress = tickAddress;
    writeGuestU32(flagAddress, 0u);
    if (tickAddress != 0u)
    {
        const uint32_t physical = tickAddress & 0x1FFFFFFFu;
        if (m_rdram && physical <= PS2_RAM_SIZE - sizeof(uint64_t))
        {
            const uint64_t zero = 0u;
            std::memcpy(m_rdram + physical, &zero, sizeof(zero));
        }
    }
}

uint64_t EeScheduler::currentVSyncTick() const noexcept
{
    return m_vsyncTick;
}

uint32_t EeScheduler::setGsVSyncCallback(uint32_t callback, uint32_t gp, uint32_t sp)
{
    assertExecutor();
    (void)sp;
    const uint32_t previous = m_gsVSyncCallback;
    m_gsVSyncCallback = callback;
    m_gsVSyncCallbackGp = gp;
    m_gsVSyncCallbackSp = 0u;
    return previous;
}

[[noreturn]] void EeScheduler::waitVSync(uint64_t afterTick, int fixedResult, std::function<void(R5900Context &)> completion)
{
    blockCurrent(EeWaitState{
        EeWaitReason::VSync,
        EeVSyncWait{afterTick, fixedResult},
        std::move(completion)});
}

void EeScheduler::completeVSync(uint64_t tick)
{
    assertExecutor();
    std::vector<int> completed;
    for (const auto &[id, candidate] : m_threads)
    {
        if ((candidate.status == EeThreadStatus::Waiting || candidate.status == EeThreadStatus::WaitingSuspended) &&
            candidate.wait.reason == EeWaitReason::VSync &&
            std::get<EeVSyncWait>(candidate.wait.payload).afterTick < tick)
        {
            completed.push_back(id);
        }
    }
    std::sort(completed.begin(), completed.end());
    for (const int id : completed)
    {
        GuestThread *waiter = thread(id);
        assert(waiter != nullptr);
        const EeVSyncWait wait = std::get<EeVSyncWait>(waiter->wait.payload);
        const int result = wait.fixedResult >= 0
                               ? wait.fixedResult
                               : static_cast<int>((tick - 1u) & 1u);
        makeReady(*waiter, result, false);
    }
    publishSnapshot();
}

void EeScheduler::completeExternalWait(uint32_t type, uint64_t token, int result)
{
    assertExecutor();
    std::vector<int> completed;
    for (const auto &[id, candidate] : m_threads)
    {
        if ((candidate.status != EeThreadStatus::Waiting && candidate.status != EeThreadStatus::WaitingSuspended) ||
            (candidate.wait.reason != EeWaitReason::External &&
             candidate.wait.reason != EeWaitReason::Mpeg))
        {
            continue;
        }
        const auto &external = std::get<EeExternalWait>(candidate.wait.payload);
        if (external.type == type && external.token == token)
        {
            completed.push_back(id);
        }
    }
    std::sort(completed.begin(), completed.end());
    for (const int id : completed)
    {
        GuestThread *waiter = thread(id);
        assert(waiter != nullptr);
        makeReady(*waiter, result, false);
    }
    publishSnapshot();
}

[[noreturn]] void EeScheduler::waitExternal(EeWaitReason reason,
                                            uint32_t type,
                                            uint64_t token,
                                            std::function<void(R5900Context &)> completion)
{
    EeWaitState wait{reason, EeExternalWait{type, token}, std::move(completion)};
    blockCurrent(std::move(wait));
}

GuestThread *EeScheduler::thread(int id)
{
    auto it = m_threads.find(id);
    return it == m_threads.end() ? nullptr : &it->second;
}

const GuestThread *EeScheduler::thread(int id) const
{
    auto it = m_threads.find(id);
    return it == m_threads.end() ? nullptr : &it->second;
}

EeSemaphore *EeScheduler::semaphore(int id)
{
    auto it = m_semaphores.find(id);
    return it == m_semaphores.end() ? nullptr : &it->second;
}

const EeSemaphore *EeScheduler::semaphore(int id) const
{
    auto it = m_semaphores.find(id);
    return it == m_semaphores.end() ? nullptr : &it->second;
}

EeEventFlag *EeScheduler::eventFlag(int id)
{
    auto it = m_eventFlags.find(id);
    return it == m_eventFlags.end() ? nullptr : &it->second;
}

const EeEventFlag *EeScheduler::eventFlag(int id) const
{
    auto it = m_eventFlags.find(id);
    return it == m_eventFlags.end() ? nullptr : &it->second;
}

GuestThread *EeScheduler::currentThread()
{
    return thread(m_currentThreadId);
}

const GuestThread *EeScheduler::currentThread() const
{
    return thread(m_currentThreadId);
}

int EeScheduler::currentThreadId() const noexcept
{
    return m_currentThreadId;
}

void EeScheduler::loadSharedVu0(R5900Context &context) const
{
    std::memcpy(reinterpret_cast<uint8_t *>(&context) + kVu0StateBegin, m_sharedVu0.data(), m_sharedVu0.size());
}

void EeScheduler::storeSharedVu0(const R5900Context &context)
{
    std::memcpy(m_sharedVu0.data(), reinterpret_cast<const uint8_t *>(&context) + kVu0StateBegin, m_sharedVu0.size());
}

void EeScheduler::flushLiveVu0()
{
    // Must run before anything that can move or destroy the dispatched context
    // (pushing onto its thread's invocation stack, deleting the thread).
    if (m_vu0LiveContext)
    {
        storeSharedVu0(*m_vu0LiveContext);
        m_vu0LiveContext = nullptr;
    }
}

R5900Context *EeScheduler::currentContext()
{
    GuestThread *self = currentThread();
    return self ? &self->activeContext() : nullptr;
}

uint8_t *EeScheduler::rdram() const noexcept
{
    return m_rdram;
}

void EeScheduler::bindMainContextForSyscall(R5900Context &ctx, uint8_t *rdram)
{
    if (m_executorThread == std::thread::id{})
    {
        reset(rdram, ctx);
        GuestThread *main = selectReady();
        assert(main != nullptr);
        makeRunning(*main);
        return;
    }
    assertExecutor();
    m_rdram = rdram;
    if (m_currentThreadId == 0)
    {
        GuestThread *main = thread(kMainThreadId);
        assert(main != nullptr);
        assert(main->status == EeThreadStatus::Ready);
        removeReady(*main);
        makeRunning(*main);
    }
}

EeKernelSnapshot EeScheduler::snapshot() const
{
    std::lock_guard lock(m_snapshotMutex);
    return m_snapshot;
}

void EeScheduler::publishSnapshot()
{
    EeKernelSnapshot next{};
    next.sequence = ++m_snapshotSequence;
    next.eeCycle = m_eeCycle;
    next.sliceEndCycle = m_sliceEndCycle;
    next.nextEventCycle = m_nextDeadlineCycle.load(std::memory_order_acquire);
    next.runningThreadId = m_currentThreadId;
    next.threads.reserve(m_threads.size());
    for (const auto &[id, item] : m_threads)
    {
        if (id < 0)
        {
            continue;
        }
        EeThreadSnapshot snapshot{};
        snapshot.id = id;
        snapshot.pc = item.activeContext().pc;
        snapshot.entry = item.entry;
        snapshot.stack = item.stack;
        snapshot.stackSize = item.stackSize;
        snapshot.gp = item.gp;
        snapshot.initialPriority = item.initialPriority;
        snapshot.currentPriority = item.currentPriority;
        snapshot.status = item.status;
        snapshot.waitReason = item.wait.reason;
        snapshot.waitId = waitObjectId(item.wait);
        snapshot.suspendCount = item.suspendCount;
        snapshot.wakeupCount = item.wakeupCount;
        next.threads.push_back(snapshot);
    }
    std::sort(next.threads.begin(), next.threads.end(), [](const auto &left, const auto &right)
              { return left.id < right.id; });
    next.semaphores.reserve(m_semaphores.size());
    for (const auto &[id, item] : m_semaphores)
    {
        next.semaphores.push_back(EeSemaphoreSnapshot{id,
                                                      item.count,
                                                      item.maxCount,
                                                      static_cast<uint32_t>(item.waiters.size())});
    }
    std::sort(next.semaphores.begin(), next.semaphores.end(), [](const auto &left, const auto &right)
              { return left.id < right.id; });
    next.eventFlags.reserve(m_eventFlags.size());
    for (const auto &[id, item] : m_eventFlags)
    {
        next.eventFlags.push_back(EeEventFlagSnapshot{id,
                                                      item.bits,
                                                      item.initBits,
                                                      item.attr,
                                                      static_cast<uint32_t>(item.waiters.size())});
    }
    std::sort(next.eventFlags.begin(), next.eventFlags.end(), [](const auto &left, const auto &right)
              { return left.id < right.id; });
    {
        std::lock_guard lock(m_snapshotMutex);
        m_snapshot = std::move(next);
    }
}

void EeScheduler::assertExecutor() const
{
    assert(m_executorThread == std::this_thread::get_id());
}

int EeScheduler::allocateThreadId()
{
    for (int attempts = 0; attempts <= kLastThreadId - kFirstThreadId; ++attempts)
    {
        const int candidate = m_nextThreadId;
        m_nextThreadId = candidate == kLastThreadId ? kFirstThreadId : candidate + 1;
        if (!m_threads.contains(candidate))
        {
            return candidate;
        }
    }
    return 0;
}

GuestThread &EeScheduler::acquireInvocationThread()
{
    for (auto &[id, candidate] : m_threads)
    {
        if (id < 0 && candidate.status == EeThreadStatus::Dormant && candidate.invocations.empty())
        {
            return candidate;
        }
    }

    GuestThread dispatcher{};
    dispatcher.id = m_nextInvocationThreadId--;
    dispatcher.initialPriority = 0;
    dispatcher.currentPriority = 0;
    dispatcher.status = EeThreadStatus::Dormant;
    return m_threads.emplace(dispatcher.id, std::move(dispatcher)).first->second;
}

void EeScheduler::enqueueReady(GuestThread &item, bool front)
{
    assert(item.currentPriority >= 0 && item.currentPriority < kPriorityCount);
    item.status = EeThreadStatus::Ready;
    auto &queue = m_readyQueues[item.currentPriority];
    if (front)
    {
        queue.push_front(item.id);
    }
    else
    {
        queue.push_back(item.id);
    }
}

void EeScheduler::removeReady(GuestThread &item)
{
    if (item.status != EeThreadStatus::Ready)
    {
        return;
    }
    auto &queue = m_readyQueues[item.currentPriority];
    auto it = std::find(queue.begin(), queue.end(), item.id);
    assert(it != queue.end());
    queue.erase(it);
}

GuestThread *EeScheduler::selectReady()
{
    for (auto &queue : m_readyQueues)
    {
        if (queue.empty())
        {
            continue;
        }
        const int id = queue.front();
        queue.pop_front();
        GuestThread *selected = thread(id);
        assert(selected != nullptr);
        assert(selected->status == EeThreadStatus::Ready);
        return selected;
    }
    return nullptr;
}

void EeScheduler::makeRunning(GuestThread &item)
{
    assert(m_currentThreadId == 0);
    assert(item.status == EeThreadStatus::Ready);
    item.status = EeThreadStatus::Running;
    m_currentThreadId = item.id;
#if !defined(PS2X_GAME_GT3)
    g_lastRunThreadId = item.id; // PS2X_VBLANK_LATE_LOG
#endif
    renewTimeSlice();
}

void EeScheduler::makeDormant(GuestThread &item)
{
    removeReady(item);
    removeFromWaitObject(item);
    item.status = EeThreadStatus::Dormant;
    item.wait = {};
    item.resumeCompletion = {};
    item.suspendCount = 0;
    item.wakeupCount = 0;
    item.invocations.clear();
}

void EeScheduler::removeFromWaitObject(GuestThread &item)
{
    const int id = item.id;
    if (item.wait.reason == EeWaitReason::Semaphore)
    {
        const int objectId = std::get<EeSemaphoreWait>(item.wait.payload).id;
        if (EeSemaphore *object = semaphore(objectId))
        {
            auto it = std::find(object->waiters.begin(), object->waiters.end(), id);
            if (it != object->waiters.end())
            {
                object->waiters.erase(it);
            }
        }
    }
    else if (item.wait.reason == EeWaitReason::EventFlag)
    {
        const int objectId = std::get<EeEventFlagWait>(item.wait.payload).id;
        if (EeEventFlag *object = eventFlag(objectId))
        {
            auto it = std::find(object->waiters.begin(), object->waiters.end(), id);
            if (it != object->waiters.end())
            {
                object->waiters.erase(it);
            }
        }
    }
    item.wait = {};
}

void EeScheduler::blockCurrent(EeWaitState wait)
{
    GuestThread *self = currentThread();
    assert(self != nullptr);
    self->wait = std::move(wait);
    self->status = self->suspendCount == 0 ? EeThreadStatus::Waiting : EeThreadStatus::WaitingSuspended;
    m_currentThreadId = 0;
    publishSnapshot();
    noteDispatcherThrow(4);
    throw EeDispatcherTransfer{};
}

void EeScheduler::makeReady(GuestThread &item, int result, bool interruptSafe)
{
    auto completion = std::move(item.wait.completion);
    item.wait = {};
    setReturnS32(&item.activeContext(), result);
    item.resumeCompletion = std::move(completion);
    if (item.suspendCount != 0)
    {
        item.status = EeThreadStatus::Suspended;
        return;
    }
    enqueueReady(item);
    requestPreemptionIfHigher(item, interruptSafe);
}

void EeScheduler::requestPreemptionIfHigher(const GuestThread &readyThread, bool interruptSafe)
{
    const GuestThread *running = currentThread();
    if (!running || readyThread.currentPriority >= running->currentPriority)
    {
        return;
    }
    m_rescheduleRequested = true;
    if (interruptSafe || m_insideInterrupt)
    {
        m_checkpointPending.store(true, std::memory_order_release);
    }
}

void EeScheduler::applyPendingPreemption()
{
    if (!m_rescheduleRequested || hasActiveInterrupt(currentThread()))
    {
        return;
    }
    if (m_currentThreadId == 0)
    {
        m_rescheduleRequested = false;
        m_timeSliceExpired = false;
        return;
    }
    GuestThread *self = currentThread();
    assert(self != nullptr);
    enqueueReady(*self, !m_timeSliceExpired);
    m_currentThreadId = 0;
    m_rescheduleRequested = false;
    m_timeSliceExpired = false;
}

void EeScheduler::processPendingEvents()
{
    assertExecutor();
    processDueDeadlines();
    // VIF and GS (SIGNAL/FINISH) interrupts and finished DMAs, including those
    // of the VIF1 worker thread, which only wakes the scheduler.
    vif1lat::noticePath.store(1, std::memory_order_relaxed);
    vif1lat::eeActivity.store(2, std::memory_order_relaxed);
    m_runtime.drainCompletedDmacHandlers(m_rdram);
    vif1lat::noticePath.store(0, std::memory_order_relaxed);
    struct ActivityEnd
    {
        ~ActivityEnd() { vif1lat::eeActivity.store(0, std::memory_order_relaxed); }
    } activityEnd;
    const uint32_t timerInterrupts = m_pendingEeTimerInterrupts;
    m_pendingEeTimerInterrupts = 0u;
    for (uint32_t timer = 0u; timer < 4u; ++timer)
    {
        if ((timerInterrupts & (1u << timer)) != 0u)
        {
            dispatchIrq(false, 9u + timer);
        }
    }
    std::deque<EeEvent> pending;
    {
        std::lock_guard lock(m_eventMutex);
        pending.swap(m_events);
    }
    for (const EeEvent &event : pending)
    {
        processEvent(event);
    }

    {
        std::lock_guard lock(m_eventMutex);
        const uint64_t nextEventCycle = m_nextDeadlineCycle.load(std::memory_order_acquire);
        const bool cycleEventDue = nextEventCycle != 0u && m_eeCycle >= nextEventCycle;
        const bool pendingWork = !m_events.empty() || cycleEventDue || m_stopRequested.load(std::memory_order_acquire) ||
                                 m_hostVBlankTrigger.load(std::memory_order_acquire) != 0; // (a request that came meanwhile)
        m_checkpointPending.store(pendingWork, std::memory_order_release);
    }
    applyPendingPreemption();
}

void EeScheduler::processDueDeadlines()
{
#if !defined(PS2X_GAME_GT3)
    if (m_hostVBlankTrigger.load(std::memory_order_relaxed) != 0)
        if (const int trigger = m_hostVBlankTrigger.exchange(0, std::memory_order_acq_rel))
            deliverHostClockVBlank(trigger);
#endif
    for (;;)
    {
        std::vector<ScheduledEvent> due;
        std::chrono::steady_clock::time_point pacingDeadline{};
        bool pacingVBlank = false; // the deadline waited for is a VBlankStart's
        {
            std::unique_lock lock(m_eventMutex);
            const auto now = std::chrono::steady_clock::now();
            for (const ScheduledEvent &item : m_deadlines)
            {
                if (item.deadlineCycle <= m_eeCycle &&
                    (pacingDeadline == std::chrono::steady_clock::time_point{} ||
                     item.hostDeadline < pacingDeadline))
                {
                    pacingDeadline = item.hostDeadline;
                    pacingVBlank = item.event.type == EeEventType::VBlankStart;
                }
            }

            if (pacingDeadline == std::chrono::steady_clock::time_point{})
            {
                updateNextDeadline();
                return;
            }

            struct PacingMark
            {
                PacingMark() { vif1lat::eePacing.store(1, std::memory_order_relaxed); }
                ~PacingMark() { vif1lat::eePacing.store(0, std::memory_order_relaxed); }
            };
            std::optional<PacingMark> pacingMark;
            if (now < pacingDeadline)
                pacingMark.emplace();
            if (now < pacingDeadline && !spinForEvent(lock, pacingDeadline))
            {
                // (A host-clock VBlank request also ends the wait: the next
                // pass delivers it, with the deadline waited for.)
                const auto eventOrStop = [this]()
                { return !m_events.empty() ||
                         m_stopRequested.load(std::memory_order_acquire) || hostVBlankArmed(m_hostVBlankTrigger); };
                // GT4: VBlankStart on time (PS2X_PRECISE_VBLANK, vblank_clock.h).
                if (pacingVBlank && ps2x::vblank::preciseWaits())
                    preciseWaitUntil(lock, pacingDeadline, eventOrStop);
                else
                    m_eventCv.wait_until(lock, pacingDeadline, eventOrStop);
                if (!m_events.empty() || m_stopRequested.load(std::memory_order_acquire) || hostVBlankArmed(m_hostVBlankTrigger))
                {
                    updateNextDeadline();
                    return;
                }
            }

            const auto pacedNow = std::chrono::steady_clock::now();
            auto firstFuture = std::partition(m_deadlines.begin(), m_deadlines.end(),
                                              [this, pacedNow](const ScheduledEvent &item)
                                              { return item.deadlineCycle <= m_eeCycle &&
                                                       item.hostDeadline <= pacedNow; });
            due.insert(due.end(),
                       std::make_move_iterator(m_deadlines.begin()),
                       std::make_move_iterator(firstFuture));
            m_deadlines.erase(m_deadlines.begin(), firstFuture);
            updateNextDeadline();
        }

        std::sort(due.begin(), due.end(), [](const ScheduledEvent &left, const ScheduledEvent &right)
                  {
                      if (left.deadlineCycle != right.deadlineCycle)
                      {
                          return left.deadlineCycle < right.deadlineCycle;
                      }
                      if (left.event.type != right.event.type)
                      {
                          return left.event.type < right.event.type;
                      }
                      if (left.event.id != right.event.id)
                      {
                          return left.event.id < right.event.id;
                      }
                      return left.sequence < right.sequence; });

        if (due.empty())
        {
            return;
        }

        for (ScheduledEvent &scheduled : due)
        {
            if (scheduled.event.type == EeEventType::VBlankStart)
            {
                // Pace the next field from this one's deadline (a steady 60 Hz),
                // but never let the host clock trail wall time by more than a few
                // fields. While the EE runs slower than real time the lag would
                // otherwise grow for as long as the game stays CPU-bound, and the
                // next idle wait would replay all of it as a burst of VSyncs
                // (GT3's demo race jumped ~1300 VSyncs in one short wait and ended early).
                const auto firedAt = std::chrono::steady_clock::now();
                const auto hostBase = std::max(scheduled.hostDeadline,
                                               firedAt - kMaxVBlankHostLag);
                const auto hostPeriod = vblankHostPeriod(m_vsyncTick);
                scheduleEvent(scheduled.deadlineCycle + kVBlankDurationCycles,
                              hostBase + std::chrono::nanoseconds(kVBlankDuration) * hostPeriod.count() /
                                             std::chrono::nanoseconds(kVBlankPeriod).count(),
                              EeEvent{EeEventType::VBlankEnd, 0, m_vsyncTick + 1u});
                scheduleEvent(scheduled.deadlineCycle + kVBlankPeriodCycles,
                              hostBase + hostPeriod,
                              EeEvent{EeEventType::VBlankStart, 0, 0});
                // The grid for the host loop's presentation, and lateness
                // (PS2X_FPS_LOG "[vblank]"): this VBlank becomes tick m_vsyncTick + 1.
                ps2x::vblank::noteLateness(firedAt - scheduled.hostDeadline, hostBase != scheduled.hostDeadline);
                ps2x::vblank::publishGrid(m_vsyncTick + 1u, scheduled.hostDeadline, hostBase + hostPeriod);
#if !defined(PS2X_GAME_GT3)
                // GT4_MOVIE_TRACE=1 (diagnostics, analysis/design/movie-stutter): every VBlankStart, host time and lateness.
                static const bool movieTrace = std::getenv("GT4_MOVIE_TRACE") != nullptr;
                if (movieTrace)
                {
                    // The low-priority threads (GT4's movie reader and decoder: priority 100+): id/priority/status/wait/pc/ra.
                    std::string threads;
                    for (const auto &[id, t] : m_threads)
                        if (t.currentPriority >= 100 && t.status != EeThreadStatus::Dormant)
                        {
                            char one[96];
                            std::snprintf(one, sizeof(one), " t%d/%d/%d/%d/%x/%x", id, t.currentPriority, static_cast<int>(t.status),
                                          static_cast<int>(t.wait.reason), static_cast<unsigned>(t.activeContext().pc),
                                          getRegU32(&t.activeContext(), 31));
                            threads += one;
                        }
                    std::fprintf(stderr, "[mvt] vb t=%lld tick=%llu late=%.2f%s\n",
                                 static_cast<long long>(std::chrono::duration_cast<std::chrono::microseconds>(firedAt.time_since_epoch()).count()),
                                 static_cast<unsigned long long>(m_vsyncTick + 1u),
                                 std::chrono::duration<double, std::milli>(firedAt - scheduled.hostDeadline).count(), threads.c_str());
                }
                // PS2X_VBLANK_LATE_LOG=<ms> (diagnostics, off by default): each
                // VBlank that fires more than <ms> late names the guest thread
                // that was running (id, priority, pc, ra, interrupt depth; none:
                // the EE was idle or in a host wait) and thread 2's state (GT4's
                // main game thread, WaitVSync 0x4a21f0), to tell whether a late
                // VBlank holds back the main thread's frame
                // (analysis/phase5/specs/step4-hitches.md D2).
                static const double lateLogMs = [] {
                    const char *v = std::getenv("PS2X_VBLANK_LATE_LOG");
                    return (v && *v) ? std::strtod(v, nullptr) : 0.0;
                }();
                if (lateLogMs > 0.0)
                {
                    const double lateMs = std::chrono::duration<double, std::milli>(firedAt - scheduled.hostDeadline).count();
                    if (lateMs > lateLogMs)
                    {
                        char running[160];
                        if (const GuestThread *t = currentThread())
                        {
                            const R5900Context &c = t->activeContext();
                            std::snprintf(running, sizeof(running), "t%d prio %d pc %x ra %x depth %zu", t->id, t->currentPriority,
                                          static_cast<unsigned>(c.pc), getRegU32(&c, 31), t->invocations.size());
                        }
                        else if (auto last = m_threads.find(g_lastRunThreadId); last != m_threads.end())
                        {
                            // Handled between threads: the one that ran last, as it is now.
                            const GuestThread &t = last->second;
                            std::snprintf(running, sizeof(running), "none (last ran: t%d prio %d status %d wait %d ra %x)", t.id,
                                          t.currentPriority, static_cast<int>(t.status), static_cast<int>(t.wait.reason),
                                          getRegU32(&t.activeContext(), 31));
                        }
                        else
                            std::snprintf(running, sizeof(running), "none");
                        char mainState[96] = "absent";
                        if (auto it = m_threads.find(2); it != m_threads.end())
                            std::snprintf(mainState, sizeof(mainState), "status %d wait %d ra %x", static_cast<int>(it->second.status),
                                          static_cast<int>(it->second.wait.reason), getRegU32(&it->second.context, 31));
                        std::fprintf(stderr, "[vblank-late] vsync %llu late %.2f ms: running %s; thread 2 %s\n",
                                     static_cast<unsigned long long>(m_vsyncTick + 1u), lateMs, running, mainState);
                    }
                }
#endif
            }
            processEvent(scheduled.event);
        }
    }
}

void EeScheduler::processEvent(const EeEvent &event)
{
    switch (event.type)
    {
    case EeEventType::Stop:
        requestStop();
        break;
    case EeEventType::VBlankStart:
        ++m_vsyncTick;
        m_runtime.memory().gs().vsyncTick.store(m_vsyncTick, std::memory_order_release);
        // Diagnostics, GT3_EE_THREAD_DUMP=<vsync>[,<vsync>...]: every guest thread's state at those VSyncs, with the
        // return addresses found on its stack (words just after a jal / jalr in the program), innermost first.
        {
            static std::vector<uint64_t> dumpAt = [] {
                std::vector<uint64_t> out;
                if (const char *v = std::getenv("GT3_EE_THREAD_DUMP"))
                    for (const char *p = v; *p;)
                    {
                        char *end = nullptr;
                        const unsigned long long t = std::strtoull(p, &end, 10);
                        if (end == p)
                            break;
                        out.push_back(t);
                        p = *end == ',' ? end + 1 : end;
                    }
                return out;
            }();
            // (GT3_EE_WORD_WATCH=<hex address>: that word checked at every VSync, each change logged; it sees writes the
            // guest write watch GT3_WATCH_ADDR cannot, such as DMA and host code)
            static const uint32_t wordWatch = [] {
                const char *v = std::getenv("GT3_EE_WORD_WATCH");
                return v && *v ? static_cast<uint32_t>(std::strtoul(v, nullptr, 16)) & 0x01fffffcu : 0u;
            }();
            // (GT3_EE_GUARD=<hex address>,<vsync>: GuardDiag.cpp, armed at that VSync)
            static const std::pair<uint32_t, uint64_t> guard = [] {
                const char *v = std::getenv("GT3_EE_GUARD");
                if (!v || !*v)
                    return std::pair<uint32_t, uint64_t>{0u, 0ull};
                char *end = nullptr;
                const uint32_t a = static_cast<uint32_t>(std::strtoul(v, &end, 16));
                return std::pair<uint32_t, uint64_t>{a, end && *end == ',' ? std::strtoull(end + 1, nullptr, 10) : 0ull};
            }();
            if (guard.first && m_vsyncTick >= guard.second && m_rdram)
            {
                static EeScheduler *self = this;
                g_ps2xGuardRegs = [](Ps2xGuardRegs &r) {
                    GuestThread *t = self->currentThread();
                    if (!t)
                        return false;
                    const R5900Context &c = t->activeContext();
                    r.pc = c.pc, r.ra = getRegU32(&c, 31), r.sp = getRegU32(&c, 29);
                    for (int k = 0; k < 4; ++k)
                        r.a[k] = getRegU32(&c, 4 + k), r.s[k] = getRegU32(&c, 16 + k);
                    return true;
                };
                void ps2xGuardDiagArm(uint8_t *, uint32_t);
                ps2xGuardDiagArm(m_rdram, guard.first);
            }
            if (wordWatch && m_rdram)
            {
                static uint32_t last = 0;
                static bool first = true;
                uint32_t now = 0;
                std::memcpy(&now, m_rdram + wordWatch, 4);
                if (first || now != last)
                    std::fprintf(stderr, "[word-watch] vsync %llu: %08x = %08x (was %08x)\n", static_cast<unsigned long long>(m_vsyncTick), wordWatch, now, last);
                first = false, last = now;
            }
            if (std::find(dumpAt.begin(), dumpAt.end(), m_vsyncTick) != dumpAt.end() && m_rdram)
            {
                const auto word = [this](uint32_t a) {
                    uint32_t v = 0;
                    std::memcpy(&v, m_rdram + (a & 0x01fffffcu), 4);
                    return v;
                };
                for (const auto &[id, thread] : m_threads)
                {
                    const uint32_t sp = getRegU32(&thread.context, 29), ra = getRegU32(&thread.context, 31);
                    std::string chain;
                    for (uint32_t a = sp & ~3u, n = 0; a < 0x02000000u && a < (sp & ~3u) + 0x4000u && n < 24; a += 4)
                    {
                        const uint32_t v = word(a);
                        if (v < 0x100008u || v >= 0x600000u || (v & 3u))
                            continue;
                        const uint32_t call = word(v - 8u);
                        if ((call >> 26) == 3u || ((call & 0xfc00003fu) == 9u))
                        {
                            char part[24];
                            std::snprintf(part, sizeof(part), " %x", v);
                            chain += part, ++n;
                        }
                    }
                    std::fprintf(stderr, "[ee-threads] vsync %llu t%d status %d wait %d pc %x ra %x sp %x prio %d:%s\n",
                                 static_cast<unsigned long long>(m_vsyncTick), id, static_cast<int>(thread.status),
                                 static_cast<int>(thread.wait.reason), thread.context.pc, ra, sp, thread.currentPriority, chain.c_str());
                }
            }
        }
        m_runtime.queueVSyncPresentation(m_vsyncTick);
        if (m_runtime.m_iopSubsystem && m_runtime.m_iopHost)
        {
            // Native IOP services run at the real scheduler frame boundary,
            // before EE waiters/callbacks consume the new input record.
            auto scope=m_runtime.m_iopHost->enterCall(nullptr,m_rdram);
            m_runtime.m_iopSubsystem->onVBlank(true,m_vsyncTick);
        }
        if ((m_vsyncTick & 1u) != 0u)
        {
            m_runtime.memory().gs().csr.fetch_or(0x2000ull, std::memory_order_acq_rel);
        }
        else
        {
            m_runtime.memory().gs().csr.fetch_and(~0x2000ull, std::memory_order_acq_rel);
        }
        writeGuestU32(m_vsyncFlagAddress, 1u);
        if (m_vsyncTickAddress != 0u)
        {
            const uint32_t physical = m_vsyncTickAddress & 0x1FFFFFFFu;
            if (m_rdram && physical <= PS2_RAM_SIZE - sizeof(uint64_t))
            {
                std::memcpy(m_rdram + physical, &m_vsyncTick, sizeof(m_vsyncTick));
            }
        }
        m_vsyncFlagAddress = 0u;
        m_vsyncTickAddress = 0u;
        completeVSync(m_vsyncTick);
        if (m_gsVSyncCallback != 0u && m_runtime.hasFunction(m_gsVSyncCallback))
        {
            GuestInvocation invocation{};
            invocation.kind = GuestInvocationKind::GsCallback;
            invocation.context.pc = m_gsVSyncCallback;
            SET_GPR_U32(&invocation.context, 4, static_cast<uint32_t>(m_vsyncTick));
            SET_GPR_U32(&invocation.context, 28, m_gsVSyncCallbackGp);
            // Dispatch assigns an isolated callback stack, not registration SP.
            SET_GPR_U32(&invocation.context, 31, 0u);
            queueInvocation(std::move(invocation));
        }
        dispatchIrq(false, 2u);
        break;
    case EeEventType::ExternalWake:
        completeExternalWait(event.id, event.value, KE_OK);
        break;
    case EeEventType::Wake:
        break;
    case EeEventType::VBlankEnd:
        if (m_runtime.m_iopSubsystem && m_runtime.m_iopHost)
        {
            auto scope=m_runtime.m_iopHost->enterCall(nullptr,m_rdram);
            m_runtime.m_iopSubsystem->onVBlank(false,event.value);
        }
        dispatchIrq(false, 3u);
        break;
    case EeEventType::Dmac:
        break;
    case EeEventType::Alarm:
    {
        auto it = m_alarms.find(static_cast<int>(event.id));
        if (it == m_alarms.end())
        {
            break;
        }
        const EeAlarm alarm = it->second;
        m_alarms.erase(it);
        GuestInvocation invocation{};
        invocation.kind = GuestInvocationKind::Alarm;
        invocation.context.pc = alarm.handler;
        SET_GPR_U32(&invocation.context, 4, static_cast<uint32_t>(alarm.id));
        SET_GPR_U32(&invocation.context, 5, static_cast<uint32_t>(alarm.ticks));
        SET_GPR_U32(&invocation.context, 6, alarm.argument);
        SET_GPR_U32(&invocation.context, 28, alarm.gp);
        // Dispatch assigns an isolated callback stack, not registration SP.
        SET_GPR_U32(&invocation.context, 31, 0u);
        queueInvocation(std::move(invocation));
        break;
    }
    }
}

void EeScheduler::finishEventWaiters(EeEventFlag &flag, bool interruptSafe)
{
    for (auto it = flag.waiters.begin(); it != flag.waiters.end();)
    {
        GuestThread *waiter = thread(*it);
        assert(waiter != nullptr);
        const EeEventFlagWait wait = std::get<EeEventFlagWait>(waiter->wait.payload);
        if (!eventCondition(flag.bits, wait.bits, wait.mode))
        {
            ++it;
            continue;
        }
        const uint32_t observed = flag.bits;
        writeGuestU32(wait.resultAddress, observed);
        if ((wait.mode & WEF_CLEAR_ALL) != 0u)
        {
            flag.bits = 0;
        }
        else if ((wait.mode & WEF_CLEAR) != 0u)
        {
            flag.bits &= ~wait.bits;
        }
        it = flag.waiters.erase(it);
        makeReady(*waiter, KE_OK, interruptSafe);
    }
}

bool EeScheduler::eventCondition(uint32_t current, uint32_t requested, uint32_t mode)
{
    return (mode & WEF_OR) != 0u ? (current & requested) != 0u
                                 : (current & requested) == requested;
}

int EeScheduler::waitObjectId(const EeWaitState &wait)
{
    switch (wait.reason)
    {
    case EeWaitReason::Semaphore:
        return std::get<EeSemaphoreWait>(wait.payload).id;
    case EeWaitReason::EventFlag:
        return std::get<EeEventFlagWait>(wait.payload).id;
    default:
        return 0;
    }
}

void EeScheduler::writeGuestU32(uint32_t address, uint32_t value)
{
    if (address == 0u)
    {
        return;
    }
    const uint32_t physical = address & 0x1FFFFFFFu;
    if (!m_rdram || physical > PS2_RAM_SIZE - sizeof(value))
    {
        return;
    }
    std::memcpy(m_rdram + physical, &value, sizeof(value));
}

#if !defined(PS2X_GAME_GT3)
// An idle wait ended because of an event (lock held on entry, released on
// return): the stretch flag, and with PS2X_IDLE_CHARGE its wall time charged
// as EE cycles, capped one cycle short of the next deadline and the next EE
// timer interrupt.
void EeScheduler::idleWaitEndedByEvent(std::unique_lock<std::mutex> &lock, std::chrono::steady_clock::time_point waitStart)
{
    g_idleEventStretch = true;
    if (!idleCharge())
        return;
    uint64_t nextCycle = std::numeric_limits<uint64_t>::max();
    for (const ScheduledEvent &item : m_deadlines)
        nextCycle = std::min(nextCycle, item.deadlineCycle);
    const uint64_t timerCycles = m_runtime.memory().cyclesUntilNextEeTimerInterrupt();
    if (timerCycles != std::numeric_limits<uint64_t>::max())
        nextCycle = std::min(nextCycle, m_eeCycle + timerCycles);
    lock.unlock();
    if (nextCycle <= m_eeCycle + 1u)
        return;
    const auto waited = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - waitStart).count();
    if (waited <= 0)
        return;
    const uint64_t cycles = static_cast<uint64_t>(waited) / 1000u * kEeClockHz / 1000000u;
    uint64_t remaining = std::min<uint64_t>(cycles, nextCycle - m_eeCycle - 1u);
    if (remaining == 0u)
        return;
    ps2x::vblank::noteIdleCharge(remaining);
    while (remaining > 0u)
    {
        const uint32_t step = static_cast<uint32_t>(std::min<uint64_t>(remaining, std::numeric_limits<uint32_t>::max()));
        accountCycles(step);
        remaining -= step;
    }
}

// The EE timer interrupt timerCycles ahead is covered by the next VBlankStart
// while the cycle clock trails the VBlank grid (lock held): the earliest
// VBlankStart deadline (Cv, Hv) has m_eeCycle + timerCycles <= Cv, and Cv is
// further ahead in EE cycles than Hv is in wall time, by more than 0.25 ms.
// Its fast-forward then passes the timer.
bool EeScheduler::timerCoveredByTrailingVBlank(uint64_t timerCycles, std::chrono::steady_clock::time_point now) const
{
    const ScheduledEvent *vblank = nullptr;
    for (const ScheduledEvent &item : m_deadlines)
        if (item.event.type == EeEventType::VBlankStart && (!vblank || item.deadlineCycle < vblank->deadlineCycle))
            vblank = &item;
    if (!vblank || vblank->deadlineCycle <= m_eeCycle || timerCycles > vblank->deadlineCycle - m_eeCycle)
        return false;
    constexpr uint64_t kTrailMarginCycles = microsecondsToEeCycles(250u);
    const auto wallAhead = std::chrono::duration_cast<std::chrono::microseconds>(vblank->hostDeadline - now).count();
    const uint64_t wallCycles = wallAhead > 0 ? microsecondsToEeCycles(static_cast<uint64_t>(wallAhead)) : 0u;
    return vblank->deadlineCycle - m_eeCycle > wallCycles + kTrailMarginCycles;
}
#endif

void EeScheduler::waitForEvent()
{
    struct WaitingMark
    {
        WaitingMark() { vif1lat::eeWaiting.store(1, std::memory_order_relaxed); }
        ~WaitingMark() { vif1lat::eeWaiting.store(0, std::memory_order_relaxed); }
    } waitingMark;
    std::unique_lock lock(m_eventMutex);
    // (A pending host-clock VBlank request is handled by the next pass, not waited for.)
    if (!m_events.empty() || m_stopRequested.load(std::memory_order_acquire) || hostVBlankArmed(m_hostVBlankTrigger))
    {
#if !defined(PS2X_GAME_GT3)
        g_idleEventStretch = true; // (no time waited: nothing to charge)
#endif
        return;
    }
#if !defined(PS2X_GAME_GT3)
    const auto waitStart = std::chrono::steady_clock::now();
#endif
    const uint64_t timerCycles = m_runtime.memory().cyclesUntilNextEeTimerInterrupt();
    const bool hasTimerDeadline = timerCycles != std::numeric_limits<uint64_t>::max();
    if (m_deadlines.empty() && !hasTimerDeadline)
    {
        if (spinForEvent(lock, std::chrono::steady_clock::time_point::max()))
        {
#if !defined(PS2X_GAME_GT3)
            idleWaitEndedByEvent(lock, waitStart);
#endif
            return;
        }
        m_eventCv.wait(lock, [this]()
                       { return !m_events.empty() || m_stopRequested.load(std::memory_order_acquire); });
#if !defined(PS2X_GAME_GT3)
        idleWaitEndedByEvent(lock, waitStart);
#endif
        return;
    }

    uint64_t deadlineCycle = 0u;
    auto hostDeadline = std::chrono::steady_clock::time_point::max();
    bool waitVBlank = false; // the deadline waited for is a VBlankStart's
    if (!m_deadlines.empty())
    {
        const auto next = std::min_element(m_deadlines.begin(), m_deadlines.end(),
                                           [](const ScheduledEvent &left, const ScheduledEvent &right)
                                           {
                                               if (left.deadlineCycle != right.deadlineCycle)
                                               {
                                                   return left.deadlineCycle < right.deadlineCycle;
                                               }
                                               return left.sequence < right.sequence;
                                           });
        deadlineCycle = next->deadlineCycle;
        hostDeadline = next->hostDeadline;
        waitVBlank = next->event.type == EeEventType::VBlankStart;
    }
    bool waitTimer = false; // the host deadline waited for is an EE timer interrupt's
#if !defined(PS2X_GAME_GT3)
    bool timerTrailing = false; // ... covered by the next VBlankStart while the cycle clock trails
#endif
    if (hasTimerDeadline)
    {
        const auto now = std::chrono::steady_clock::now();
        const auto timerHostDeadline = now + eeCyclesToHostDuration(timerCycles);
        if (timerHostDeadline < hostDeadline)
        {
#if !defined(PS2X_GAME_GT3)
            // Fix H (PS2X_IDLE_TIMER_HOLD): keep the deadline wait; its
            // timeout's fast-forward passes the timer.
            timerTrailing = timerCoveredByTrailingVBlank(timerCycles, now);
            const int hold = idleTimerHold();
            if (timerTrailing && hold != 0 && (g_idleEventStretch || hold >= 2))
                ps2x::vblank::noteIdleTimer(ps2x::vblank::IdleTimerNote::Held);
            else
#endif
            {
                deadlineCycle = m_eeCycle + timerCycles;
                hostDeadline = timerHostDeadline;
                waitVBlank = false;
                waitTimer = true;
            }
        }
    }

    if (spinForEvent(lock, hostDeadline))
    {
#if !defined(PS2X_GAME_GT3)
        idleWaitEndedByEvent(lock, waitStart);
#endif
        return;
    }
    const auto eventOrStop = [this]()
    { return !m_events.empty() ||
             m_stopRequested.load(std::memory_order_acquire) || hostVBlankArmed(m_hostVBlankTrigger); };
    // GT4: VBlankStart on time (PS2X_PRECISE_VBLANK, vblank_clock.h); EE
    // timers too with PS2X_IDLE_TIMER_PRECISE.
    bool precise = waitVBlank && ps2x::vblank::preciseWaits();
#if !defined(PS2X_GAME_GT3)
    precise = precise || (waitTimer && idleTimerPrecise() && ps2x::vblank::preciseWaits());
#endif
    const bool signaled = precise ? preciseWaitUntil(lock, hostDeadline, eventOrStop)
                                  : m_eventCv.wait_until(lock, hostDeadline, eventOrStop);
    if (!signaled)
    {
        const uint64_t elapsed = deadlineCycle > m_eeCycle ? deadlineCycle - m_eeCycle : 0u;
        lock.unlock();
        uint64_t remaining = elapsed;
        while (remaining > 0u)
        {
            const uint32_t step = static_cast<uint32_t>(std::min<uint64_t>(remaining, std::numeric_limits<uint32_t>::max()));
            accountCycles(step);
            remaining -= step;
        }
        m_checkpointPending.store(true, std::memory_order_release);
#if !defined(PS2X_GAME_GT3)
        // A deadline or a timer reached: the event-ended stretch is over.
        if (waitTimer)
        {
            ps2x::vblank::noteIdleTimer(ps2x::vblank::IdleTimerNote::Expired);
            if (timerTrailing)
                ps2x::vblank::noteIdleTimer(ps2x::vblank::IdleTimerNote::ExpiredTrailing);
        }
        g_idleEventStretch = false;
#endif
    }
#if !defined(PS2X_GAME_GT3)
    else
        idleWaitEndedByEvent(lock, waitStart);
#endif
}

void EeScheduler::scheduleEvent(uint64_t deadlineCycle,
                                std::chrono::steady_clock::time_point hostDeadline,
                                EeEvent event)
{
    {
        std::lock_guard lock(m_eventMutex);
        m_deadlines.push_back(ScheduledEvent{deadlineCycle, hostDeadline, event, ++m_eventSequence});
        updateNextDeadline();
    }
#if !defined(PS2X_GAME_GT3)
    if (event.type == EeEventType::VBlankStart)
        ps2x::vblank::publishNextVBlankDue(hostDeadline); // host-clock VBlank requests (vblank_clock.h)
#endif
    m_eventCv.notify_one();
}

void EeScheduler::updateNextDeadline()
{
    if (m_deadlines.empty())
    {
        m_nextDeadlineCycle.store(0u, std::memory_order_release);
        return;
    }
    const auto it = std::min_element(m_deadlines.begin(), m_deadlines.end(),
                                     [](const ScheduledEvent &left, const ScheduledEvent &right)
                                     {
                                         if (left.deadlineCycle != right.deadlineCycle)
                                         {
                                             return left.deadlineCycle < right.deadlineCycle;
                                         }
                                         return left.sequence < right.sequence;
                                     });
    m_nextDeadlineCycle.store(it->deadlineCycle, std::memory_order_release);
}

bool EeScheduler::hasReadyAtOrAbovePriority(int priority) const
{
    const int last = std::clamp(priority, 0, kPriorityCount - 1);
    for (int p = 0; p <= last; ++p)
    {
        if (!m_readyQueues[static_cast<size_t>(p)].empty())
        {
            return true;
        }
    }
    return false;
}

void EeScheduler::renewTimeSlice()
{
    m_sliceEndCycle = m_eeCycle + kDefaultTimeSliceCycles;
    m_timeSliceExpired = false;
}

void EeScheduler::copyMainContextToRuntime()
{
    const GuestThread *main = thread(kMainThreadId);
    if (main)
    {
        m_runtime.m_cpuContext = main->context;
    }
}
