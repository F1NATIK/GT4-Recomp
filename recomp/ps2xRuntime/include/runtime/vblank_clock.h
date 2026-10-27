#pragma once

#include <chrono>
#include <cstdint>

// The EE scheduler's VBlank clock, shared with the host loop
// (analysis/phase5/specs/step3-pacing.md):
// - precise waits: MSVC's condition_variable::wait_until rounds the wait up to
//   whole milliseconds and wakes 0-2.4 ms late on the 1 ms system tick, which
//   made every VBlank that late (race p99 2.5 ms). A high-resolution waitable
//   timer to 0.5 ms before the deadline, then a short spin, lands within ~0.3 ms
//   at ~0.2 ms of spin per frame;
// - the VBlankStart grid (each VBlank's host due time), for the host loop to
//   present each game frame on it;
// - VBlank lateness statistics for PS2X_FPS_LOG ("[vblank]" lines).
namespace ps2x::vblank
{
using Clock = std::chrono::steady_clock;

// Precise VBlankStart waits in the EE scheduler: on in GT4 builds
// (PS2X_PRECISE_VBLANK=0 turns them off), off in GT3 builds unless
// PS2X_PRECISE_VBLANK=1.
bool preciseWaits();

// The EE thread: sleeps until `deadline`, or until wake() is called after
// `seen` was read from wakeSerial(). May return early (the caller checks its
// condition again and calls again).
uint64_t wakeSerial();
void waitUntilOrWake(Clock::time_point deadline, uint64_t seen);
// Any thread: an event was posted for the EE thread.
void wake();

// Any thread: sleeps until `deadline` with the same precision (no wake).
void sleepUntil(Clock::time_point deadline);

// The EE scheduler at each VBlankStart: VBlank `tick` was due at `due`, the
// next one at `nextDue` (its grid, after any lag-cap rebase).
void publishGrid(uint64_t tick, Clock::time_point due, Clock::time_point nextDue);
// The newest published VBlank (false before the first).
bool grid(uint64_t &tick, Clock::time_point &due, Clock::time_point &nextDue);

// Presentations, whichever renderer composed them (CPU or GPU). Each is
// labelled with the VSync tick it was latched at (the GS stream's display
// snapshot), not the tick of the buffer it shows.
// - The GS, before its backend composes a latch: beginPresentation(tick). The
//   GL presenter tags its GPU frame with composingPresentation().
// - The GS worker, once that frame can be copied by the host (CPU frame
//   published, GPU slot published, frame counted): presentationReady(tick).
// - The host loop (grid pacing): readyPresentation() and waitForPresentation,
//   which returns true once the presentation of `tick` or a later one is
//   ready, false when `limit` passes first.
void beginPresentation(uint64_t tick);
uint64_t composingPresentation();
void presentationReady(uint64_t tick);
uint64_t readyPresentation();
bool waitForPresentation(uint64_t tick, Clock::time_point limit);

// PS2X_FPS_LOG: VBlankStart lateness (when it was processed minus when it was
// due) and lag-cap rebases (a VBlank more than 4 periods late: the grid moves).
void noteLateness(Clock::duration late, bool rebased);
struct LatenessWindow
{
    uint64_t count = 0, rebases = 0;
    double p50Ms = 0.0, p99Ms = 0.0, maxMs = 0.0;
};
// The VBlanks noted since the last call.
LatenessWindow takeLateness();

// GT4: VBlankStart on the host clock (analysis/phase5/specs/step4-ee-lateness.md
// L2n and 1b; GT3 builds: both off). Normally a VBlank also waits for the EE's
// cycle count to reach its deadline cycle; uncharged host work (the IPU on the
// EE thread) can hold it back while the game's main thread already sleeps in
// WaitVSync. Delivered on the host clock, the VBlank fires at the EE's next
// checkpoint once its host deadline has passed, with the EE cycles
// fast-forwarded to its deadline cycle first, one event at a time like an idle
// wait: the deadlines and EE timer interrupts before it are handled first, in
// cycle order, and are due on the host clock with it (EeScheduler.cpp
// deliverHostClockVBlank).
// - PS2X_IPU_VBLANK=1 (L2n, default off): requested only from the IPU entry
//   points on the EE thread (noteIpuWork), so it acts only while the IPU works.
// - PS2X_VBLANK_HOST_CLOCK=1 (1b, default off): requested by a timer thread at
//   every VBlankStart's host deadline.
bool ipuVBlankEnabled();
bool hostClockVBlankEnabled();
// The EE scheduler: the next VBlankStart's host deadline, published whenever
// one is scheduled, and the request it honours at its next checkpoint (trigger
// 1 IPU, 2 timer; any thread). Setting a requester starts the 1b timer thread
// when that is on; a null requester stops the requests, waits for one in
// flight and stops (joins) the timer thread.
void publishNextVBlankDue(Clock::time_point due);
using HostVBlankRequester = void (*)(void *context, int trigger);
void setHostVBlankRequester(HostVBlankRequester fn, void *context);
// EE thread, after each IPU call (L2n): cheap when off or not yet due.
void noteIpuWork();
// The EE scheduler, per VBlank it delivered on the host clock: the trigger,
// the EE cycles it fast-forwarded and in how many steps (more than one: it
// passed earlier deadlines or EE timer interrupts on the way). PS2X_FPS_LOG
// prints them per window.
void noteHostClockVBlank(int trigger, uint64_t fastForwardCycles, uint32_t steps);
struct HostClockWindow
{
    uint64_t ipu = 0, timer = 0;       // VBlanks delivered on the host clock, by trigger
    uint64_t requests = 0;              // requests made (a request can find the VBlank already processed)
    double avgCycles = 0.0, maxCycles = 0.0; // EE cycles fast-forwarded per delivered VBlank
    uint64_t multiStep = 0;             // deliveries that passed earlier events on the way
    uint32_t maxSteps = 0;
};
HostClockWindow takeHostClock();

// GT4: EE timer interrupts in the EE scheduler's idle waits
// (analysis/phase5/specs/step6-l1-steploss.md 7.2, EeScheduler.cpp
// waitForEvent; GT3 builds: never noted). Per PS2X_FPS_LOG window:
// - held: idle waits in which an EE timer due before the next VBlankStart was
//   left to that VBlank's fast-forward (PS2X_IDLE_TIMER_HOLD);
// - expired: EE timer waits that ended on the timer's own host deadline, and
//   of them those that did so while the EE cycle clock trailed the VBlank grid
//   (the race step-loss trigger);
// - charged: idle waits ended by an event that were charged as EE cycles
//   (PS2X_IDLE_CHARGE), and the cycles charged.
enum class IdleTimerNote
{
    Held,
    Expired,
    ExpiredTrailing,
};
void noteIdleTimer(IdleTimerNote what);
void noteIdleCharge(uint64_t cycles);
struct IdleTimerWindow
{
    uint64_t held = 0, expired = 0, expiredTrailing = 0;
    uint64_t charges = 0, chargedCycles = 0;
};
IdleTimerWindow takeIdleTimers();
}
