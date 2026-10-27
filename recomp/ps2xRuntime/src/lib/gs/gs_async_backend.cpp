#include "runtime/gs/gs_async_backend.h"

#include "ThreadNaming.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <type_traits>
#if defined(_M_X64) || defined(__x86_64__)
#include <immintrin.h>
#endif

namespace
{
inline void cpuRelax()
{
#if defined(_M_X64) || defined(__x86_64__)
    _mm_pause();
#endif
}

static_assert(std::is_trivially_copyable_v<GSPrimitiveBatch>, "batches are copied through the ring");
static_assert(std::is_trivially_copyable_v<GSTransferCommand>, "transfers are copied through the ring");
static_assert(std::is_trivially_copyable_v<GSTex0Reg> && std::is_trivially_copyable_v<GSTexClutReg>, "CLUT loads are copied through the ring");

constexpr size_t kHeaderBytes = 16u; // Header padded: payloads start 16-byte aligned

size_t recordBytes(size_t payload)
{
    return (kHeaderBytes + payload + 15u) & ~size_t{15u};
}

// The backend thread spins this long for more work before it sleeps: GIF
// packets arrive every few microseconds while a frame is drawn
// (GT3_GL_SPIN_US, default 200).
const std::chrono::microseconds kIdleSpin([] {
    const char *v = std::getenv("GT3_GL_SPIN_US");
    return v && *v ? std::strtol(v, nullptr, 10) : 200L;
}());

// GT3_GS_GL_STATS=1 (step 7 T0, analysis/design/ny-throughput/STEP7-THROUGHPUT.md):
// every 10 s a [gs-ring] line with the backend thread's busy share and the
// records it ran. Timed by its idle periods only: one clock read when it runs
// out of work (the one the spin window takes anyway) and one when work
// arrives; time in IdleWork counts as idle.
const bool kRingStats = [] {
    const char *v = std::getenv("GT3_GS_GL_STATS");
    return v && *v && *v != '0';
}();

// Draw-state runs (step 7 T1, analysis/design/ny-throughput/STEP7-THROUGHPUT.md):
// a primitive whose draw state equals the previous Submit record's
// (gsSameDrawState, field by field) is queued as a SubmitSame record with
// only its vertices (144 bytes instead of 336) and reaches the backend as
// SubmitSameState, which may reuse what it resolved for that state.
// PS2X_GS_GL_STATE_RUNS=0: full records and Submit only (as before; the GT3
// default). The GL backend reads the same switch.
bool stateRunsOn()
{
    const char *v = std::getenv("PS2X_GS_GL_STATE_RUNS");
#if defined(PS2X_GAME_GT3)
    return v && *v && *v != '0';
#else
    return !(v && *v == '0');
#endif
}
static_assert(std::is_trivially_copyable_v<std::array<GSVertex, 3>>, "vertices are copied through the ring");
constexpr uint32_t kSameBytes = static_cast<uint32_t>(sizeof(std::array<GSVertex, 3>) + 1u); // vertices, vertex count
} // namespace

GSAsyncBackend::GSAsyncBackend(const Factory &factory, std::function<void()> threadStart, std::function<void()> threadExit)
    : m_ring(kRingBytes)
{
    m_stateRuns = stateRunsOn();
    m_inner = factory([this](const std::function<void()> &task) { runSync(task); });
    m_thread = std::thread([this, threadStart = std::move(threadStart), threadExit = std::move(threadExit)] { run(threadStart, threadExit); });
    m_threadId = m_thread.get_id();
}

GSAsyncBackend::~GSAsyncBackend()
{
    // The backend's objects belong to its thread (an OpenGL context).
    runSync([this] { m_inner.reset(); });
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_stop.store(true);
    }
    m_wake.notify_all();
    if (m_thread.joinable())
        m_thread.join();
}

void GSAsyncBackend::run(std::function<void()> threadStart, std::function<void()> threadExit)
{
    ThreadNaming::SetCurrentThreadName("GS GL");
    ThreadNaming::PreferPerformance();
    if (threadStart)
        threadStart();
    size_t tail = 0;
    auto idleSince = std::chrono::steady_clock::now();
    uint32_t idleSpins = 0;
    // Statistics (kRingStats).
    using StatClock = std::chrono::steady_clock;
    bool statIdle = false;
    StatClock::time_point statIdleFrom{}, statWindow = StatClock::now();
    uint64_t statIdleNs = 0, statWaits = 0, statSleeps = 0, statRecords = 0, statDraws = 0, statSame = 0, statBytes = 0;
    uint32_t statChecks = 0;
    auto statReport = [&](StatClock::time_point now) {
        const double seconds = std::chrono::duration<double>(now - statWindow).count();
        const double idleMs = static_cast<double>(statIdleNs) / 1e6;
        std::fprintf(stderr,
                     "[gs-ring] %.1f s: GL thread busy %.1f%% (idle %.0f ms: %llu waits, %llu sleeps); records %llu, draws %llu (same draw state %llu), "
                     "ring %.1f MB\n",
                     seconds, seconds > 0.0 ? 100.0 * (1.0 - idleMs / (seconds * 1000.0)) : 0.0, idleMs, (unsigned long long)statWaits,
                     (unsigned long long)statSleeps, (unsigned long long)statRecords, (unsigned long long)statDraws, (unsigned long long)statSame,
                     static_cast<double>(statBytes) / 1048576.0);
        statIdleNs = statWaits = statSleeps = statRecords = statDraws = statSame = statBytes = 0;
        statWindow = now;
    };
    for (;;)
    {
        const size_t head = m_published.load(std::memory_order_acquire);
        if (tail == head)
        {
            if (m_stop.load())
                break;
            if (idleSpins++ == 0)
            {
                idleSince = std::chrono::steady_clock::now();
                if (kRingStats && !statIdle)
                {
                    statIdle = true;
                    statIdleFrom = idleSince;
                    ++statWaits;
                }
            }
            if ((idleSpins & 63u) != 0u || std::chrono::steady_clock::now() - idleSince < kIdleSpin)
            {
                cpuRelax();
                continue;
            }
            // The backend's idle work (one step: e.g. GL objects made ahead,
            // PS2X_GS_GL_PREWARM), then the ring is checked and the spin window
            // runs again before the thread sleeps.
            if (m_inner && m_inner->IdleWork())
            {
                idleSpins = 0;
                continue;
            }
            std::unique_lock<std::mutex> lock(m_mutex);
            if (kRingStats)
                ++statSleeps;
            m_sleeping.store(true);
            m_wake.wait(lock, [&] { return m_published.load() != tail || m_stop.load(); });
            m_sleeping.store(false);
            idleSpins = 0;
            continue;
        }
        idleSpins = 0;
        if (kRingStats && (statIdle || (++statChecks & 255u) == 0u))
        {
            const auto now = StatClock::now();
            if (statIdle)
            {
                statIdleNs += static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(now - statIdleFrom).count());
                statIdle = false;
            }
            if (now - statWindow >= std::chrono::seconds(10))
                statReport(now);
        }
        while (tail != head)
        {
            const size_t at = tail & (kRingBytes - 1u);
            Header header;
            std::memcpy(&header, &m_ring[at], sizeof(header));
            if (header.kind == Kind::Wrap)
            {
                tail += kRingBytes - at;
                continue;
            }
            if (kRingStats)
            {
                ++statRecords;
                statBytes += recordBytes(header.bytes);
                statDraws += (header.kind == Kind::Submit || header.kind == Kind::SubmitSame) ? 1u : 0u;
                statSame += header.kind == Kind::SubmitSame ? 1u : 0u;
            }
            execute(header, &m_ring[at + kHeaderBytes]);
            tail += recordBytes(header.bytes);
            // Sequentially consistent with the waiter checks: a producer that
            // started waiting either sees this or is notified. Only when one
            // waits (a broadcast per record while Present waited for the ring
            // to drain was 13% of this thread).
            m_consumed.store(tail);
            if (m_roomWaiters.load() != 0u || (header.kind == Kind::Task && m_taskWaiters.load() != 0u))
            {
                {
                    std::lock_guard<std::mutex> lock(m_mutex);
                }
                m_progress.notify_all();
            }
        }
    }
    if (threadExit)
        threadExit();
}

void GSAsyncBackend::execute(const Header &header, const uint8_t *payload)
{
    switch (header.kind)
    {
    case Kind::Submit:
    {
        std::memcpy(&m_current, payload, sizeof(m_current));
        m_currentValid = true;
        m_directSubmit = false;
        m_inner->Submit(m_current);
        break;
    }
    case Kind::SubmitSame:
    {
        // The previous Submit record's state, these vertices.
        std::memcpy(m_current.vertices.data(), payload, sizeof(m_current.vertices));
        m_current.vertexCount = payload[sizeof(m_current.vertices)];
        if (!m_currentValid || m_directSubmit)
        {
            // (The backend's last Submit was another primitive's.)
            m_directSubmit = false;
            m_inner->Submit(m_current);
        }
        else
            m_inner->SubmitSameState(m_current);
        break;
    }
    case Kind::LoadClut:
    {
        GSTex0Reg tex;
        GSTexClutReg texclut;
        std::memcpy(&tex, payload, sizeof(tex));
        std::memcpy(&texclut, payload + sizeof(tex), sizeof(texclut));
        m_inner->LoadClut(tex, texclut);
        break;
    }
    case Kind::BeginTransfer:
    {
        GSTransferCommand command;
        std::memcpy(&command, payload, sizeof(command));
        m_inner->BeginTransfer(command);
        break;
    }
    case Kind::UploadImage:
        m_inner->UploadImage(payload, header.bytes);
        break;
    case Kind::Flush:
        m_inner->Flush();
        break;
    case Kind::TextureFlush:
        m_inner->TextureFlush();
        break;
    case Kind::Sync:
    {
        GSSyncReason reason;
        std::memcpy(&reason, payload, sizeof(reason));
        m_inner->Sync(reason);
        break;
    }
    case Kind::Task:
    {
        TaskRecord record;
        std::memcpy(&record, payload, sizeof(record));
        (*record.task)();
        m_tasksDone.fetch_add(1u);
        break;
    }
    case Kind::Marker:
    {
        uint32_t code = 0;
        std::memcpy(&code, payload, sizeof(code));
        m_inner->Marker(code);
        break;
    }
    case Kind::Wrap:
        break;
    }
}

uint8_t *GSAsyncBackend::reserve(size_t bytes)
{
    auto waitForRoom = [&](size_t need) {
        if (m_head + need - m_consumed.load(std::memory_order_acquire) <= kRingBytes)
            return;
        for (int i = 0; i < 4096; ++i)
        {
            if (m_head + need - m_consumed.load(std::memory_order_acquire) <= kRingBytes)
                return;
            cpuRelax();
        }
        m_roomWaiters.fetch_add(1u);
        {
            std::unique_lock<std::mutex> lock(m_mutex);
            m_progress.wait(lock, [&] { return m_head + need - m_consumed.load() <= kRingBytes; });
        }
        m_roomWaiters.fetch_sub(1u);
    };
    size_t at = m_head & (kRingBytes - 1u);
    if (at + bytes > kRingBytes)
    {
        // Not before the end: a Wrap record fills the rest of the ring.
        waitForRoom(kRingBytes - at);
        const Header wrap{Kind::Wrap, 0u};
        std::memcpy(&m_ring[at], &wrap, sizeof(wrap));
        m_head += kRingBytes - at;
        at = 0;
    }
    waitForRoom(bytes);
    return &m_ring[at];
}

void GSAsyncBackend::publish(size_t newHead)
{
    m_head = newHead;
    m_published.store(newHead);
}

void GSAsyncBackend::push2(Kind kind, const void *a, uint32_t aBytes, const void *b, uint32_t bBytes)
{
    const uint32_t payload = aBytes + bBytes;
    const size_t bytes = recordBytes(payload);
    while (m_produce.test_and_set(std::memory_order_acquire))
        cpuRelax();
    uint8_t *dst = reserve(bytes);
    const Header header{kind, payload};
    std::memcpy(dst, &header, sizeof(header));
    if (aBytes)
        std::memcpy(dst + kHeaderBytes, a, aBytes);
    if (bBytes)
        std::memcpy(dst + kHeaderBytes + aBytes, b, bBytes);
    publish(m_head + bytes);
    m_produce.clear(std::memory_order_release);
    if (m_sleeping.load())
    {
        {
            std::lock_guard<std::mutex> lock(m_mutex);
        }
        m_wake.notify_one();
    }
}

void GSAsyncBackend::push(Kind kind, const void *payload, uint32_t bytes) { push2(kind, payload, bytes, nullptr, 0u); }

void GSAsyncBackend::runSync(const std::function<void()> &task) const
{
    GSAsyncBackend &self = *const_cast<GSAsyncBackend *>(this);
    if (onBackendThread())
    {
        task();
        return;
    }
    const TaskRecord record{&task};
    const size_t bytes = recordBytes(sizeof(record));
    while (self.m_produce.test_and_set(std::memory_order_acquire))
        cpuRelax();
    uint8_t *dst = self.reserve(bytes);
    const Header header{Kind::Task, static_cast<uint32_t>(sizeof(record))};
    std::memcpy(dst, &header, sizeof(header));
    std::memcpy(dst + kHeaderBytes, &record, sizeof(record));
    const uint64_t ticket = ++self.m_tasksQueued;
    self.publish(self.m_head + bytes);
    self.m_produce.clear(std::memory_order_release);
    if (m_sleeping.load())
    {
        {
            std::lock_guard<std::mutex> lock(m_mutex);
        }
        m_wake.notify_one();
    }
    for (int i = 0; i < 2048; ++i)
    {
        if (m_tasksDone.load(std::memory_order_acquire) >= ticket)
            return;
        cpuRelax();
    }
    self.m_taskWaiters.fetch_add(1u);
    {
        std::unique_lock<std::mutex> lock(m_mutex);
        m_progress.wait(lock, [&] { return m_tasksDone.load() >= ticket; });
    }
    self.m_taskWaiters.fetch_sub(1u);
}

void GSAsyncBackend::Initialize(uint8_t *vram, uint32_t vramSize)
{
    runSync([&] { m_inner->Initialize(vram, vramSize); });
}

void GSAsyncBackend::Reset()
{
    runSync([&] { m_inner->Reset(); });
}

void GSAsyncBackend::Submit(const GSPrimitiveBatch &batch)
{
    if (onBackendThread())
    {
        m_directSubmit = true; // (the next SubmitSame record goes as a Submit)
        m_inner->Submit(batch);
    }
    else
        pushSubmit(batch);
}

void GSAsyncBackend::pushSubmit(const GSPrimitiveBatch &batch)
{
    while (m_produce.test_and_set(std::memory_order_acquire))
        cpuRelax();
    const bool same = m_stateRuns && m_lastStateValid && gsSameDrawState(batch.state, m_lastState);
    if (m_stateRuns && !same) // (switched off: no copy, as before T1)
    {
        m_lastState = batch.state;
        m_lastStateValid = true;
    }
    const uint32_t payload = same ? kSameBytes : static_cast<uint32_t>(sizeof(batch));
    const size_t bytes = recordBytes(payload);
    uint8_t *dst = reserve(bytes);
    const Header header{same ? Kind::SubmitSame : Kind::Submit, payload};
    std::memcpy(dst, &header, sizeof(header));
    if (same)
    {
        std::memcpy(dst + kHeaderBytes, batch.vertices.data(), sizeof(batch.vertices));
        dst[kHeaderBytes + sizeof(batch.vertices)] = batch.vertexCount;
    }
    else
        std::memcpy(dst + kHeaderBytes, &batch, sizeof(batch));
    publish(m_head + bytes);
    m_produce.clear(std::memory_order_release);
    if (m_sleeping.load())
    {
        {
            std::lock_guard<std::mutex> lock(m_mutex);
        }
        m_wake.notify_one();
    }
}

void GSAsyncBackend::LoadClut(const GSTex0Reg &tex, const GSTexClutReg &texclut)
{
    if (onBackendThread())
        m_inner->LoadClut(tex, texclut);
    else
        push2(Kind::LoadClut, &tex, sizeof(tex), &texclut, sizeof(texclut));
}

void GSAsyncBackend::BeginTransfer(const GSTransferCommand &command)
{
    if (onBackendThread())
        m_inner->BeginTransfer(command);
    else
        push(Kind::BeginTransfer, &command, sizeof(command));
}

void GSAsyncBackend::UploadImage(const uint8_t *data, uint32_t sizeBytes)
{
    if (onBackendThread())
        m_inner->UploadImage(data, sizeBytes);
    else if (recordBytes(sizeBytes) > kRingBytes / 4u)
        runSync([&] { m_inner->UploadImage(data, sizeBytes); }); // too large to copy through
    else
        push(Kind::UploadImage, data, sizeBytes);
}

void GSAsyncBackend::Flush()
{
    if (onBackendThread())
        m_inner->Flush();
    else
        push(Kind::Flush, nullptr, 0u);
}

void GSAsyncBackend::TextureFlush()
{
    if (onBackendThread())
        m_inner->TextureFlush();
    else
        push(Kind::TextureFlush, nullptr, 0u);
}

void GSAsyncBackend::Sync(GSSyncReason reason)
{
    if (onBackendThread())
        m_inner->Sync(reason);
    else
        push(Kind::Sync, &reason, sizeof(reason));
}

void GSAsyncBackend::Marker(uint32_t code)
{
    if (onBackendThread())
        m_inner->Marker(code);
    else
        push(Kind::Marker, &code, sizeof(code));
}

PresentationFrame GSAsyncBackend::Present(const GSPresentationRequest &request)
{
    PresentationFrame frame;
    runSync([&] { frame = m_inner->Present(request); });
    return frame;
}

bool GSAsyncBackend::ClearFramebuffer(const GSContext &context, uint32_t rgba)
{
    bool result = false;
    runSync([&] { result = m_inner->ClearFramebuffer(context, rgba); });
    return result;
}

uint32_t GSAsyncBackend::ConsumeLocalToHostBytes(uint8_t *dst, uint32_t maxBytes)
{
    uint32_t result = 0u;
    runSync([&] { result = m_inner->ConsumeLocalToHostBytes(dst, maxBytes); });
    return result;
}

uint32_t GSAsyncBackend::ReadVram(uint32_t psm, uint32_t base, uint32_t bw, uint32_t x, uint32_t y) const
{
    uint32_t result = 0u;
    runSync([&] { result = m_inner->ReadVram(psm, base, bw, x, y); });
    return result;
}

void GSAsyncBackend::WriteVram(uint32_t psm, uint32_t base, uint32_t bw, uint32_t x, uint32_t y, uint32_t value)
{
    runSync([&] { m_inner->WriteVram(psm, base, bw, x, y, value); });
}

void GSAsyncBackend::SnapshotVram(std::vector<uint8_t> &out) const
{
    runSync([&] { m_inner->SnapshotVram(out); });
}

GSTransferSnapshot GSAsyncBackend::GetTransferSnapshot() const
{
    GSTransferSnapshot result{};
    runSync([&] { result = m_inner->GetTransferSnapshot(); });
    return result;
}
