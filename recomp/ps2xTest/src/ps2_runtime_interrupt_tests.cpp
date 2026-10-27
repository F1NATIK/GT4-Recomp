#include "MiniTest.h"
#include "ps2_runtime.h"
#include "ps2_syscalls.h"
#include "runtime/ee_scheduler.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <exception>
#include <thread>
#include <vector>

using namespace ps2_syscalls;

namespace
{
    constexpr int KE_OK = 0;
    constexpr int KE_EVF_COND = -421;

    constexpr uint32_t WEF_OR = 1u;
    constexpr uint32_t WEF_CLEAR = 0x10u;
    constexpr uint32_t WEF_CLEAR_ALL = 0x20u;

    struct Ps2EventFlagInfo
    {
        uint32_t attr;
        uint32_t option;
        uint32_t initBits;
        uint32_t currBits;
        int32_t numThreads;
        int32_t reserved1;
        int32_t reserved2;
    };

    static_assert(sizeof(Ps2EventFlagInfo) == 28u, "Unexpected Ps2EventFlagInfo layout.");

    struct TestEnv
    {
        std::vector<uint8_t> rdram;
        PS2Runtime runtime;

        TestEnv() : rdram(PS2_RAM_SIZE, 0u)
        {
        }
    };

    std::atomic<uint32_t> g_lastIntcArg{0u};
    constexpr uint32_t kIdleVSyncWaitPc = 0x00160000u;
    constexpr uint32_t kVSyncWaitPc = 0x00160100u;
    constexpr uint32_t kVSyncResumePc = 0x00160110u;
    constexpr uint32_t kIrqWaitPc = 0x00160200u;
    constexpr uint32_t kIrqResumePc = 0x00160210u;
    constexpr uint32_t kIntcHandlerPc = 0x00160220u;
    constexpr uint32_t kVu0WaitPc = 0x00160230u;
    constexpr uint32_t kVu0ResumePc = 0x00160240u;
    constexpr uint32_t kVu0HandlerPc = 0x00160250u;
    constexpr uint32_t kISemaWaitPc = 0x00160300u;
    constexpr uint32_t kISemaResumePc = 0x00160310u;
    constexpr uint32_t kISemaDriverPc = 0x00160320u;
    constexpr uint32_t kISemaHandlerPc = 0x00160330u;
    constexpr uint32_t kISemaHandlerTailPc = 0x00160340u;
    constexpr uint32_t kISemaDriverTailPc = 0x00160350u;
    constexpr uint32_t kEventWaitPc = 0x00160400u;
    constexpr uint32_t kEventResumePc = 0x00160410u;
    constexpr uint32_t kEventProducerPc = 0x00160420u;
    constexpr uint32_t kTimer2WaitPc = 0x00160500u;
    constexpr uint32_t kTimer2ResumePc = 0x00160510u;
    constexpr uint32_t kTimer2HandlerPc = 0x00160520u;
    constexpr uint32_t kVifDriverPc = 0x00160600u;
    constexpr uint32_t kVifHandlerPc = 0x00160610u;
    constexpr uint32_t kVifResumePc = 0x00160620u;
    constexpr uint32_t kLagStallPc = 0x00160700u;
    constexpr uint32_t kLagWaitPc = 0x00160710u;
    uint32_t g_vifChannel = 0u;
    uint32_t g_vifSeenCause = 0u;
    uint32_t g_vifSeenCode = 0u;
    uint32_t g_vifSeenStat = 0u;

    constexpr uint32_t kTimer2Count = 0x10001000u;
    constexpr uint32_t kTimer2Mode = 0x10001010u;
    constexpr uint32_t kTimer2Compare = 0x10001020u;
    constexpr uint32_t kTimerModeBusClockDiv256 = 2u;
    constexpr uint32_t kTimerModeCue = 1u << 7u;
    constexpr uint32_t kTimerModeCmpe = 1u << 8u;
    constexpr uint32_t kTimerModeEquf = 1u << 10u;

    constexpr uint32_t kVSyncFlagAddr = 0x1800u;
    constexpr uint32_t kVSyncTickAddr = 0x1810u;
    constexpr uint32_t kEventResultAddr = 0x1820u;

    std::vector<int> g_dispatchTrace;
    int g_testSemaphoreId = 0;
    int g_testEventFlagId = 0;
    int32_t g_resumedResult = 0;
    uint32_t g_vsyncFlag = 0;
    uint64_t g_vsyncTick = 0;
    uint64_t g_vsyncCsr = 0;
    std::atomic<bool> g_timer2Resumed{false};

    void setRegU32(R5900Context &ctx, int reg, uint32_t value)
    {
        ctx.r[reg] = _mm_set_epi64x(0, static_cast<int64_t>(value));
    }

    int32_t getRegS32(const R5900Context &ctx, int reg)
    {
        return static_cast<int32_t>(::getRegU32(&ctx, reg));
    }

    bool callSyscall(uint32_t syscallNumber, uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        return dispatchNumericSyscall(syscallNumber, rdram, ctx, runtime);
    }

    void writeGuestU32(uint8_t *rdram, uint32_t addr, uint32_t value)
    {
        std::memcpy(rdram + addr, &value, sizeof(value));
    }

    uint32_t readGuestU32(const uint8_t *rdram, uint32_t addr)
    {
        uint32_t value = 0;
        std::memcpy(&value, rdram + addr, sizeof(value));
        return value;
    }

    uint64_t readGuestU64(const uint8_t *rdram, uint32_t addr)
    {
        uint64_t value = 0;
        std::memcpy(&value, rdram + addr, sizeof(value));
        return value;
    }

    template <typename Predicate>
    bool waitUntil(Predicate pred, std::chrono::milliseconds timeout)
    {
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        while (std::chrono::steady_clock::now() < deadline)
        {
            if (pred())
            {
                return true;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        return pred();
    }

    void cleanupRuntime(TestEnv &env)
    {
        env.runtime.requestStop();
    }

    void idleVSyncWait(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        WaitVSyncTick(rdram, ctx, runtime, -1);
    }

    void schedulerVSyncWait(uint8_t *, R5900Context *ctx, PS2Runtime *runtime)
    {
        EeScheduler &scheduler = runtime->eeScheduler();
        scheduler.setVSyncFlag(kVSyncFlagAddr, kVSyncTickAddr);
        ctx->pc = kVSyncResumePc;
        scheduler.waitVSync(scheduler.currentVSyncTick());
    }

    // Lag test: one dispatch holds the EE for 300 ms of wall time (a slow,
    // CPU-bound stretch), then the thread waits for VSyncs for 100 ms.
    std::chrono::steady_clock::time_point g_lagStallEnd{};
    uint64_t g_lagTickAtStallEnd = 0u;
    uint64_t g_lagTicksInWindow = 0u;

    void schedulerLagStall(uint8_t *, R5900Context *ctx, PS2Runtime *runtime)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        g_lagStallEnd = std::chrono::steady_clock::now();
        g_lagTickAtStallEnd = runtime->eeScheduler().currentVSyncTick();
        ctx->pc = kLagWaitPc;
    }

    void schedulerLagWait(uint8_t *, R5900Context *ctx, PS2Runtime *runtime)
    {
        EeScheduler &scheduler = runtime->eeScheduler();
        if (std::chrono::steady_clock::now() - g_lagStallEnd < std::chrono::milliseconds(100))
        {
            ctx->pc = kLagWaitPc;
            scheduler.waitVSync(scheduler.currentVSyncTick());
            return;
        }
        g_lagTicksInWindow = scheduler.currentVSyncTick() - g_lagTickAtStallEnd;
        ctx->pc = 0u;
        runtime->requestStop();
    }

    void schedulerVSyncResume(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        g_vsyncFlag = readGuestU32(rdram, kVSyncFlagAddr);
        g_vsyncTick = readGuestU64(rdram, kVSyncTickAddr);
        g_vsyncCsr = runtime->memory().gs().csr.load(std::memory_order_acquire);
        g_resumedResult = getRegS32(*ctx, 2);
        ctx->pc = 0u;
        runtime->requestStop();
    }

    void schedulerIntcHandler(uint8_t *, R5900Context *ctx, PS2Runtime *)
    {
        g_dispatchTrace.push_back(2);
        g_lastIntcArg.store(getRegU32(ctx, 5), std::memory_order_relaxed);
        ctx->pc = 0u;
    }

    void schedulerVifHandler(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        g_dispatchTrace.push_back(2);
        g_vifSeenCause = getRegU32(ctx, 4);
        const uint32_t base = 0x10003800u + g_vifChannel * 0x400u;
        g_vifSeenCode = runtime->memory().read32(base + 0x80u);
        g_vifSeenStat = runtime->memory().read32(base);
        runtime->Store32(rdram, ctx, base + 0x10u, 8u);
        runtime->eeScheduler().signalSemaphore(g_testSemaphoreId, true);
        ctx->pc = 0u;
    }

    void schedulerVifDriver(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        g_dispatchTrace.push_back(1);
        auto &scheduler = runtime->eeScheduler();
        g_testSemaphoreId = scheduler.createSemaphore(0, 1, 0u, 0u);
        scheduler.addIrqHandler(false, 4u + g_vifChannel, kVifHandlerPc, true, 0u, 0u, 0u);
        scheduler.setIrqCauseEnabled(false, 4u + g_vifChannel, true);
        const uint32_t words[4] = {0x91020000u, 0x01000403u, 0u, 0u};
        std::memcpy(runtime->memory().getRDRAM() + 0x20000u, words, sizeof(words));
        const uint32_t dma = 0x10008000u + g_vifChannel * 0x1000u;
        runtime->Store32(rdram, ctx, dma + 0x10u, 0x20000u);
        runtime->Store32(rdram, ctx, dma + 0x20u, 1u);
        runtime->Store32(rdram, ctx, dma, 0x100u);
        runtime->memory().processPendingTransfers();
        // The scheduler boundary also drains IRQs from explicit transfer drains.
        ctx->pc = kVifResumePc;
        scheduler.waitSemaphore(g_testSemaphoreId);
    }

    void schedulerVifResume(uint8_t *, R5900Context *ctx, PS2Runtime *runtime)
    {
        g_dispatchTrace.push_back(3);
        ctx->pc = 0u;
        runtime->requestStop();
    }

    // GT3 keeps matrix-stack pointers in vi11..vi13 and the current matrix in
    // vf28..vf31 across its threads and interrupt handlers (VU0 is one unit).
    uint16_t g_vu0HandlerSawVi11 = 0u;
    float g_vu0HandlerSawVf28[4] = {};
    uint16_t g_vu0ResumeSawVi12 = 0u;
    float g_vu0ResumeSawVf29[4] = {};

    void schedulerVu0Handler(uint8_t *, R5900Context *ctx, PS2Runtime *)
    {
        g_vu0HandlerSawVi11 = ctx->vi[11];
        _mm_storeu_ps(g_vu0HandlerSawVf28, ctx->vu0_vf[28]);
        ctx->vi[12] = 0x55u;
        ctx->vu0_vf[29] = _mm_setr_ps(5.0f, 6.0f, 7.0f, 8.0f);
        ctx->pc = 0u;
    }

    void schedulerVu0Wait(uint8_t *, R5900Context *ctx, PS2Runtime *runtime)
    {
        EeScheduler &scheduler = runtime->eeScheduler();
        ctx->vi[11] = 0x10u;
        ctx->vu0_vf[28] = _mm_setr_ps(1.0f, 2.0f, 3.0f, 4.0f);
        scheduler.addIrqHandler(false, 2u, kVu0HandlerPc, true, 0u, 0u, 0u);
        ctx->pc = kVu0ResumePc;
        scheduler.waitVSync(scheduler.currentVSyncTick());
    }

    void schedulerVu0Resume(uint8_t *, R5900Context *ctx, PS2Runtime *runtime)
    {
        g_vu0ResumeSawVi12 = ctx->vi[12];
        _mm_storeu_ps(g_vu0ResumeSawVf29, ctx->vu0_vf[29]);
        ctx->pc = 0u;
        runtime->requestStop();
    }

    void schedulerIrqWait(uint8_t *, R5900Context *ctx, PS2Runtime *runtime)
    {
        g_dispatchTrace.push_back(1);
        EeScheduler &scheduler = runtime->eeScheduler();
        scheduler.addIrqHandler(false, 2u, kIntcHandlerPc, true, 0xCAFEu, 0u, 0u);
        ctx->pc = kIrqResumePc;
        scheduler.waitVSync(scheduler.currentVSyncTick());
    }

    void schedulerIrqResume(uint8_t *, R5900Context *ctx, PS2Runtime *runtime)
    {
        g_dispatchTrace.push_back(3);
        ctx->pc = 0u;
        runtime->requestStop();
    }

    void schedulerISemaHandler(uint8_t *, R5900Context *ctx, PS2Runtime *runtime)
    {
        g_dispatchTrace.push_back(3);
        runtime->eeScheduler().signalSemaphore(g_testSemaphoreId, true);
        g_dispatchTrace.push_back(4);
        ctx->pc = 0u;
    }

    void schedulerCheckpointIrqHandler(uint8_t *, R5900Context *ctx, PS2Runtime *runtime)
    {
        g_dispatchTrace.push_back(3);
        runtime->eeScheduler().signalSemaphore(g_testSemaphoreId, true);
        // Original handlers span multiple generated functions/checkpoints.
        // A newly ready thread must not outrun the remainder of the handler.
        ctx->pc = kISemaHandlerTailPc;
        (void)runtime->eeScheduler().checkpointDue(EeScheduler::kDefaultTimeSliceCycles + 32u);
    }

    void schedulerCheckpointIrqTail(uint8_t *, R5900Context *ctx, PS2Runtime *)
    {
        g_dispatchTrace.push_back(4);
        ctx->pc = 0u;
    }

    void schedulerCheckpointIrqDriver(uint8_t *, R5900Context *ctx, PS2Runtime *runtime)
    {
        g_dispatchTrace.push_back(2);
        ctx->pc = kISemaDriverTailPc;
        runtime->eeScheduler().dispatchIrq(true, 5u);
    }

    void schedulerCheckpointDriverTail(uint8_t *, R5900Context *ctx, PS2Runtime *runtime)
    {
        g_dispatchTrace.push_back(6);
        ctx->pc = 0u;
        runtime->requestStop();
    }

    void schedulerISemaDriver(uint8_t *, R5900Context *ctx, PS2Runtime *runtime)
    {
        g_dispatchTrace.push_back(2);
        ctx->pc = 0u;
        runtime->eeScheduler().dispatchIrq(true, 5u);
    }

    void schedulerISemaWait(uint8_t *, R5900Context *ctx, PS2Runtime *runtime)
    {
        g_dispatchTrace.push_back(1);
        EeScheduler &scheduler = runtime->eeScheduler();
        g_testSemaphoreId = scheduler.createSemaphore(0, 1, 0u, 0u);
        scheduler.addIrqHandler(true, 5u, kISemaHandlerPc, true, 0u, 0u, 0u);

        EeThreadCreateParams driver{};
        driver.entry = kISemaDriverPc;
        driver.stack = 0x1C000u;
        driver.stackSize = 0x1000u;
        driver.priority = 10;
        const int driverId = scheduler.createThread(driver);
        scheduler.startThread(driverId, 0u, *ctx, false);

        ctx->pc = kISemaResumePc;
        scheduler.waitSemaphore(g_testSemaphoreId);
    }

    void schedulerISemaResume(uint8_t *, R5900Context *ctx, PS2Runtime *runtime)
    {
        g_dispatchTrace.push_back(5);
        g_resumedResult = getRegS32(*ctx, 2);
        ctx->pc = 0u;
        runtime->requestStop();
    }

    void schedulerEventProducer(uint8_t *, R5900Context *ctx, PS2Runtime *runtime)
    {
        g_dispatchTrace.push_back(2);
        ctx->pc = 0u;
        runtime->eeScheduler().setEventFlag(g_testEventFlagId, 0x6u, false);
        runtime->eeScheduler().transferIfRequested(false);
    }

    void schedulerEventWait(uint8_t *, R5900Context *ctx, PS2Runtime *runtime)
    {
        g_dispatchTrace.push_back(1);
        EeScheduler &scheduler = runtime->eeScheduler();
        g_testEventFlagId = scheduler.createEventFlag(0u, 0u, 0u);

        EeThreadCreateParams producer{};
        producer.entry = kEventProducerPc;
        producer.stack = 0x1D000u;
        producer.stackSize = 0x1000u;
        producer.priority = 10;
        const int producerId = scheduler.createThread(producer);
        scheduler.startThread(producerId, 0u, *ctx, false);

        ctx->pc = kEventResumePc;
        scheduler.waitEventFlag(g_testEventFlagId, 0x2u, WEF_OR | WEF_CLEAR, kEventResultAddr);
    }

    void schedulerEventResume(uint8_t *, R5900Context *ctx, PS2Runtime *runtime)
    {
        g_dispatchTrace.push_back(3);
        g_resumedResult = getRegS32(*ctx, 2);
        ctx->pc = 0u;
        runtime->requestStop();
    }

    void schedulerTimer2Handler(uint8_t *, R5900Context *ctx, PS2Runtime *runtime)
    {
        g_dispatchTrace.push_back(2);
        PS2Memory &memory = runtime->memory();
        memory.writeIORegister(kTimer2Mode, memory.readIORegister(kTimer2Mode) | kTimerModeEquf);
        runtime->eeScheduler().signalSemaphore(g_testSemaphoreId, true);
        ctx->pc = 0u;
    }

    void schedulerTimer2Wait(uint8_t *, R5900Context *ctx, PS2Runtime *runtime)
    {
        g_dispatchTrace.push_back(1);
        EeScheduler &scheduler = runtime->eeScheduler();
        g_testSemaphoreId = scheduler.createSemaphore(0, 1, 0u, 0u);
        scheduler.addIrqHandler(false, 11u, kTimer2HandlerPc, true, 0u, 0u, 0u);

        PS2Memory &memory = runtime->memory();
        memory.writeIORegister(kTimer2Count, 0u);
        memory.writeIORegister(kTimer2Compare, 8u);
        memory.writeIORegister(kTimer2Mode,
                               kTimerModeBusClockDiv256 | kTimerModeCue | kTimerModeCmpe | kTimerModeEquf);

        ctx->pc = kTimer2ResumePc;
        scheduler.waitSemaphore(g_testSemaphoreId);
    }

    void schedulerTimer2Resume(uint8_t *, R5900Context *ctx, PS2Runtime *runtime)
    {
        g_dispatchTrace.push_back(3);
        g_resumedResult = getRegS32(*ctx, 2);
        g_timer2Resumed.store(true, std::memory_order_release);
        ctx->pc = 0u;
        runtime->requestStop();
    }
}

void register_ps2_runtime_interrupt_tests()
{
    MiniTest::Case("PS2RuntimeInterrupt", [](TestCase &tc)
    {
        for (const bool interruptSafe : {false, true})
        tc.Run(std::string(interruptSafe ? "iChange" : "Change") +
               "ThreadPriority returns the saved priority for restoration", [interruptSafe](TestCase &t)
        {
            TestEnv env;
            R5900Context context{};
            const auto change = interruptSafe ? iChangeThreadPriority : ChangeThreadPriority;
            auto call = [&](int id, int priority) {
                setRegU32(context, 4, static_cast<uint32_t>(id));
                setRegU32(context, 5, static_cast<uint32_t>(priority));
                change(env.rdram.data(), &context, &env.runtime);
                return getRegS32(context, 2);
            };
            call(0, 9);
            const int saved = call(0, 1);
            t.Equals(saved, 9, "temporarily raising priority must return the old priority");
            t.Equals(call(0, saved), 1, "restoring the saved priority must return the temporary priority");
            const auto *main = env.runtime.eeScheduler().thread(EeScheduler::kMainThreadId);
            t.IsTrue(main != nullptr, "main thread must remain present");
            if (main) t.Equals(main->currentPriority, 9, "restoration must really restore scheduling priority");
            t.Equals(call(1, 9), 9, "an unchanged priority still returns its previous value");
            t.IsTrue(call(1, 128) < 0, "invalid priority must remain an error");
            t.IsTrue(call(255, 4) < 0, "unknown thread must remain an error");
            if (main) t.Equals(main->currentPriority, 9, "errors must not mutate the live priority");
        });

        tc.Run("negative interrupt-safe EE syscall ids dispatch", [](TestCase &t)
        {
            TestEnv env;

            constexpr uint32_t kEventParamAddr = 0x1200u;
            constexpr uint32_t kStatusAddr = 0x1210u;

            const uint32_t eventParam[3] = {
                0u,
                0u,
                0u
            };
            std::memcpy(env.rdram.data() + kEventParamAddr, eventParam, sizeof(eventParam));

            R5900Context createCtx{};
            setRegU32(createCtx, 4, kEventParamAddr);
            CreateEventFlag(env.rdram.data(), &createCtx, &env.runtime);
            const int32_t eid = getRegS32(createCtx, 2);
            t.IsTrue(eid > 0, "CreateEventFlag should return a valid event id");

            R5900Context disableIntcCtx{};
            setRegU32(disableIntcCtx, 4, 2u);
            t.IsTrue(callSyscall(static_cast<uint32_t>(-0x1B), env.rdram.data(), &disableIntcCtx, &env.runtime),
                     "negative iDisableIntc syscall id should dispatch");
            t.Equals(getRegS32(disableIntcCtx, 2), KE_OK, "negative iDisableIntc should return KE_OK");

            R5900Context enableIntcCtx{};
            setRegU32(enableIntcCtx, 4, 2u);
            t.IsTrue(callSyscall(static_cast<uint32_t>(-0x1A), env.rdram.data(), &enableIntcCtx, &env.runtime),
                     "negative iEnableIntc syscall id should dispatch");
            t.Equals(getRegS32(enableIntcCtx, 2), KE_OK, "negative iEnableIntc should return KE_OK");

            R5900Context disableDmacCtx{};
            setRegU32(disableDmacCtx, 4, 5u);
            t.IsTrue(callSyscall(static_cast<uint32_t>(-0x1D), env.rdram.data(), &disableDmacCtx, &env.runtime),
                     "negative iDisableDmac syscall id should dispatch");
            t.Equals(getRegS32(disableDmacCtx, 2), KE_OK, "negative iDisableDmac should return KE_OK");

            R5900Context enableDmacCtx{};
            setRegU32(enableDmacCtx, 4, 5u);
            t.IsTrue(callSyscall(static_cast<uint32_t>(-0x1C), env.rdram.data(), &enableDmacCtx, &env.runtime),
                     "negative iEnableDmac syscall id should dispatch");
            t.Equals(getRegS32(enableDmacCtx, 2), KE_OK, "negative iEnableDmac should return KE_OK");

            R5900Context setEventFlagCtx{};
            setRegU32(setEventFlagCtx, 4, static_cast<uint32_t>(eid));
            setRegU32(setEventFlagCtx, 5, 0x6u);
            t.IsTrue(callSyscall(static_cast<uint32_t>(-0x53), env.rdram.data(), &setEventFlagCtx, &env.runtime),
                     "negative iSetEventFlag syscall id should dispatch");
            t.Equals(getRegS32(setEventFlagCtx, 2), KE_OK, "negative iSetEventFlag should return KE_OK");

            R5900Context referCtx{};
            setRegU32(referCtx, 4, static_cast<uint32_t>(eid));
            setRegU32(referCtx, 5, kStatusAddr);
            ReferEventFlagStatus(env.rdram.data(), &referCtx, &env.runtime);
            t.Equals(getRegS32(referCtx, 2), KE_OK, "ReferEventFlagStatus should succeed after iSetEventFlag");
            t.Equals(readGuestU32(env.rdram.data(), kStatusAddr + 12u), 0x6u,
                     "negative iSetEventFlag should publish the requested bits");

            R5900Context deleteCtx{};
            setRegU32(deleteCtx, 4, static_cast<uint32_t>(eid));
            DeleteEventFlag(env.rdram.data(), &deleteCtx, &env.runtime);

            cleanupRuntime(env);
        });

        tc.Run("PollEventFlag WEF_CLEAR clears only matched bits", [](TestCase &t)
        {
            TestEnv env;

            constexpr uint32_t kParamAddr = 0x1400u;
            constexpr uint32_t kResBitsAddr = 0x1410u;
            constexpr uint32_t kStatusAddr = 0x1420u;

            const uint32_t eventParam[3] = {
                0u, // attr
                0u, // option
                0x7u // init bits: 0b111
            };
            std::memcpy(env.rdram.data() + kParamAddr, eventParam, sizeof(eventParam));

            R5900Context createCtx{};
            setRegU32(createCtx, 4, kParamAddr);
            CreateEventFlag(env.rdram.data(), &createCtx, &env.runtime);
            const int32_t eid = getRegS32(createCtx, 2);
            t.IsTrue(eid > 0, "CreateEventFlag should return a valid id");

            R5900Context pollCtx{};
            setRegU32(pollCtx, 4, static_cast<uint32_t>(eid));
            setRegU32(pollCtx, 5, 0x1u);
            setRegU32(pollCtx, 6, WEF_OR | WEF_CLEAR);
            setRegU32(pollCtx, 7, kResBitsAddr);
            PollEventFlag(env.rdram.data(), &pollCtx, &env.runtime);
            t.Equals(getRegS32(pollCtx, 2), KE_OK, "PollEventFlag should succeed when condition is met");
            t.Equals(readGuestU32(env.rdram.data(), kResBitsAddr), 0x7u, "PollEventFlag should report bits before clear");

            R5900Context referCtx{};
            setRegU32(referCtx, 4, static_cast<uint32_t>(eid));
            setRegU32(referCtx, 5, kStatusAddr);
            ReferEventFlagStatus(env.rdram.data(), &referCtx, &env.runtime);
            t.Equals(getRegS32(referCtx, 2), KE_OK, "ReferEventFlagStatus should succeed");

            Ps2EventFlagInfo info{};
            std::memcpy(&info, env.rdram.data() + kStatusAddr, sizeof(info));
            t.Equals(info.currBits, 0x6u, "WEF_CLEAR should clear only requested bits, not all bits");

            R5900Context pollMissCtx{};
            setRegU32(pollMissCtx, 4, static_cast<uint32_t>(eid));
            setRegU32(pollMissCtx, 5, 0x1u);
            setRegU32(pollMissCtx, 6, WEF_OR);
            setRegU32(pollMissCtx, 7, 0u);
            PollEventFlag(env.rdram.data(), &pollMissCtx, &env.runtime);
            t.Equals(getRegS32(pollMissCtx, 2), KE_EVF_COND,
                     "after clearing bit 0, polling for bit 0 should fail condition");

            R5900Context deleteCtx{};
            setRegU32(deleteCtx, 4, static_cast<uint32_t>(eid));
            DeleteEventFlag(env.rdram.data(), &deleteCtx, &env.runtime);
            t.Equals(getRegS32(deleteCtx, 2), KE_OK, "DeleteEventFlag should succeed");

            cleanupRuntime(env);
        });

        tc.Run("VBlank deadline resumes the waiter and publishes flag tick and FIELD atomically", [](TestCase &t)
        {
            TestEnv env;
            t.IsTrue(env.runtime.memory().initialize(), "runtime memory initialize should succeed");
            env.runtime.registerFunction(kVSyncWaitPc, schedulerVSyncWait);
            env.runtime.registerFunction(kVSyncResumePc, schedulerVSyncResume);

            g_resumedResult = -1;
            g_vsyncFlag = 0u;
            g_vsyncTick = 0u;
            g_vsyncCsr = 0u;
            R5900Context mainContext{};
            mainContext.pc = kVSyncWaitPc;
            env.runtime.eeScheduler().reset(env.rdram.data(), mainContext);
            env.runtime.eeScheduler().run();

            t.Equals(g_vsyncFlag, 1u, "VBlank start should set the registered guest flag");
            t.Equals(g_vsyncTick, 1ull, "the first centralized VBlank deadline should publish tick one");
            t.Equals(g_resumedResult, 0, "the first VBlank field should return even-field parity");
            t.Equals(g_vsyncCsr & 0x2000ull, 0x2000ull,
                     "the first VBlank should publish GS CSR.FIELD before resuming guest code");
        });

        for (uint32_t channel = 0; channel < 2; ++channel)
        {
            tc.Run("VIF IRQ invokes registered guest handler channel=" + std::to_string(channel), [channel](TestCase &t)
            {
                TestEnv env;
                t.IsTrue(env.runtime.memory().initialize(), "memory initializes");
                env.runtime.registerFunction(kVifDriverPc, schedulerVifDriver);
                env.runtime.registerFunction(kVifHandlerPc, schedulerVifHandler);
                env.runtime.registerFunction(kVifResumePc, schedulerVifResume);
                g_vifChannel = channel;
                g_vifSeenCause = g_vifSeenCode = g_vifSeenStat = 0u;
                g_dispatchTrace.clear();
                R5900Context context{};
                context.pc = kVifDriverPc;
                env.runtime.eeScheduler().reset(env.rdram.data(), context);
                std::atomic<bool> done{false};
                std::thread watchdog([&]
                {
                    if (!waitUntil([&] { return done.load(); }, std::chrono::seconds(2)))
                        env.runtime.requestStop();
                });
                env.runtime.eeScheduler().run();
                done.store(true);
                watchdog.join();
                t.IsTrue(g_dispatchTrace == std::vector<int>{1, 2, 3}, "DMA, guest IRQ acknowledgement, waiter resume occur in order");
                t.Equals(g_vifSeenCause, 4u + channel, "VIF uses INTC cause 4/5");
                t.Equals(g_vifSeenCode, 0x91020000u, "guest sees the interrupting CODE");
                t.IsTrue((g_vifSeenStat & 0xC00u) == 0xC00u, "guest sees INT/VIS before acknowledgement");
                const auto &regs = channel == 0 ? env.runtime.memory().vif0_regs : env.runtime.memory().vif1_regs;
                t.IsTrue(regs.cycle == 0x403u && (regs.stat & 0xC00u) == 0u, "guest STC resumes queued commands");
            });
        }

        tc.Run("VBlank pacing does not replay a slow stretch as a burst of fields at the next idle wait", [](TestCase &t)
        {
            TestEnv env;
            t.IsTrue(env.runtime.memory().initialize(), "runtime memory initialize should succeed");
            env.runtime.registerFunction(kLagStallPc, schedulerLagStall);
            env.runtime.registerFunction(kLagWaitPc, schedulerLagWait);
            g_lagTicksInWindow = 0u;
            R5900Context mainContext{};
            mainContext.pc = kLagStallPc;
            env.runtime.eeScheduler().reset(env.rdram.data(), mainContext);
            env.runtime.eeScheduler().run();

            // 100 ms is 6 fields; the host clock may trail wall time by at most
            // 4 fields. Replaying the whole 300 ms stall would give about 24.
            t.IsTrue(g_lagTicksInWindow >= 1u, "VSyncs still arrive after the stall");
            t.IsTrue(g_lagTicksInWindow <= 12u,
                     "the 300 ms stall must not be replayed as a burst (" + std::to_string(g_lagTicksInWindow) + " fields in 100 ms)");
        });

        tc.Run("VBlank IRQ invocation completes before the resumed base context", [](TestCase &t)
        {
            TestEnv env;
            env.runtime.registerFunction(kIrqWaitPc, schedulerIrqWait);
            env.runtime.registerFunction(kIrqResumePc, schedulerIrqResume);
            env.runtime.registerFunction(kIntcHandlerPc, schedulerIntcHandler);

            g_dispatchTrace.clear();
            g_lastIntcArg.store(0u, std::memory_order_relaxed);
            R5900Context mainContext{};
            mainContext.pc = kIrqWaitPc;
            env.runtime.eeScheduler().reset(env.rdram.data(), mainContext);
            env.runtime.eeScheduler().run();

            const std::vector<int> expected{1, 2, 3};
            t.IsTrue(g_dispatchTrace == expected,
                     "the dispatcher should run wait, IRQ frame, then the resumed base context in exact order");
            t.Equals(g_lastIntcArg.load(std::memory_order_relaxed), 0xCAFEu,
                     "the IRQ frame should receive its registered argument");
        });

        tc.Run("VU0 macro registers are shared between a thread and its IRQ handler (GT3 matrix stacks)", [](TestCase &t)
        {
            TestEnv env;
            env.runtime.registerFunction(kVu0WaitPc, schedulerVu0Wait);
            env.runtime.registerFunction(kVu0ResumePc, schedulerVu0Resume);
            env.runtime.registerFunction(kVu0HandlerPc, schedulerVu0Handler);

            g_vu0HandlerSawVi11 = 0u;
            g_vu0ResumeSawVi12 = 0u;
            R5900Context mainContext{};
            mainContext.pc = kVu0WaitPc;
            env.runtime.eeScheduler().reset(env.rdram.data(), mainContext);
            env.runtime.eeScheduler().run();

            t.Equals(static_cast<uint32_t>(g_vu0HandlerSawVi11), 0x10u, "the IRQ handler should see vi11 set by the thread");
            t.IsTrue(g_vu0HandlerSawVf28[0] == 1.0f && g_vu0HandlerSawVf28[3] == 4.0f, "the IRQ handler should see vf28 set by the thread");
            t.Equals(static_cast<uint32_t>(g_vu0ResumeSawVi12), 0x55u, "the resumed thread should see vi12 set by the handler");
            t.IsTrue(g_vu0ResumeSawVf29[0] == 5.0f && g_vu0ResumeSawVf29[3] == 8.0f, "the resumed thread should see vf29 set by the handler");
        });

        tc.Run("iSignalSema defers selection until IRQ return", [](TestCase &t)
        {
            TestEnv env;
            env.runtime.registerFunction(kISemaWaitPc, schedulerISemaWait);
            env.runtime.registerFunction(kISemaResumePc, schedulerISemaResume);
            env.runtime.registerFunction(kISemaDriverPc, schedulerISemaDriver);
            env.runtime.registerFunction(kISemaHandlerPc, schedulerISemaHandler);

            g_dispatchTrace.clear();
            g_resumedResult = -1;
            R5900Context mainContext{};
            mainContext.pc = kISemaWaitPc;
            env.runtime.eeScheduler().reset(env.rdram.data(), mainContext);
            env.runtime.eeScheduler().run();

            const std::vector<int> expected{1, 2, 3, 4, 5};
            t.IsTrue(g_dispatchTrace == expected,
                     "iSignalSema should make the waiter ready but finish the IRQ frame before selecting it");
            t.Equals(g_resumedResult, g_testSemaphoreId,
                     "the resumed waiter should receive the semaphore id from the direct FIFO handoff");
            const EeSemaphore *semaphore = env.runtime.eeScheduler().semaphore(g_testSemaphoreId);
            t.IsTrue(semaphore != nullptr, "the signaled semaphore should still exist");
            if (semaphore)
            {
                t.Equals(semaphore->count, 0, "direct handoff must not increment the semaphore count");
                t.Equals(static_cast<uint32_t>(semaphore->waiters.size()), 0u,
                         "the awakened waiter must be removed from the semaphore queue");
            }
        });

        tc.Run("IRQ checkpoint and expired slice cannot preempt the unfinished handler", [](TestCase &t)
        {
            TestEnv env;
            env.runtime.registerFunction(kISemaWaitPc, schedulerISemaWait);
            env.runtime.registerFunction(kISemaResumePc, schedulerISemaResume);
            env.runtime.registerFunction(kISemaDriverPc, schedulerCheckpointIrqDriver);
            env.runtime.registerFunction(kISemaHandlerPc, schedulerCheckpointIrqHandler);
            env.runtime.registerFunction(kISemaHandlerTailPc, schedulerCheckpointIrqTail);
            env.runtime.registerFunction(kISemaDriverTailPc, schedulerCheckpointDriverTail);
            g_dispatchTrace.clear();
            g_resumedResult = -1;
            R5900Context context{};
            context.pc = kISemaWaitPc;
            env.runtime.eeScheduler().reset(env.rdram.data(), context);
            env.runtime.eeScheduler().run();
            t.IsTrue(g_dispatchTrace == std::vector<int>({1,2,3,4,5}),
                     "finish the IRQ tail before selecting the awakened waiter, then preempt its lower-priority owner");
            t.Equals(g_resumedResult, g_testSemaphoreId, "the awakened waiter must keep its handoff result");
        });

        tc.Run("event-flag completion writes observed bits before strict-priority resume", [](TestCase &t)
        {
            TestEnv env;
            env.runtime.registerFunction(kEventWaitPc, schedulerEventWait);
            env.runtime.registerFunction(kEventResumePc, schedulerEventResume);
            env.runtime.registerFunction(kEventProducerPc, schedulerEventProducer);

            g_dispatchTrace.clear();
            g_resumedResult = -1;
            R5900Context mainContext{};
            mainContext.pc = kEventWaitPc;
            env.runtime.eeScheduler().reset(env.rdram.data(), mainContext);
            env.runtime.eeScheduler().run();

            const std::vector<int> expected{1, 2, 3};
            t.IsTrue(g_dispatchTrace == expected,
                     "the higher-priority event waiter should resume at the producer scheduling point");
            t.Equals(g_resumedResult, KE_OK, "the resumed event waiter should receive KE_OK");
            t.Equals(readGuestU32(env.rdram.data(), kEventResultAddr), 0x6u,
                     "the event output should contain the bits observed before clear mode is applied");
            const EeEventFlag *flag = env.runtime.eeScheduler().eventFlag(g_testEventFlagId);
            t.IsTrue(flag != nullptr, "the event flag should still exist");
            if (flag)
            {
                t.Equals(flag->bits, 0x4u, "WEF_CLEAR should remove only the requested matched bit");
            }
        });

        tc.Run("EE Timer2 compare IRQ wakes a DelayThread-style semaphore wait", [](TestCase &t)
        {
            TestEnv env;
            t.IsTrue(env.runtime.memory().initialize(), "runtime memory initialize should succeed");
            env.runtime.registerFunction(kTimer2WaitPc, schedulerTimer2Wait);
            env.runtime.registerFunction(kTimer2ResumePc, schedulerTimer2Resume);
            env.runtime.registerFunction(kTimer2HandlerPc, schedulerTimer2Handler);

            g_dispatchTrace.clear();
            g_resumedResult = -1;
            g_timer2Resumed.store(false, std::memory_order_release);
            R5900Context mainContext{};
            mainContext.pc = kTimer2WaitPc;
            std::atomic<bool> schedulerThrew{false};
            std::thread gameThread([&]()
            {
                try
                {
                    env.runtime.eeScheduler().reset(env.rdram.data(), mainContext);
                    env.runtime.eeScheduler().run();
                }
                catch (...)
                {
                    schedulerThrew.store(true, std::memory_order_release);
                }
            });

            const bool resumed = waitUntil([]()
            {
                return g_timer2Resumed.load(std::memory_order_acquire);
            }, std::chrono::milliseconds(150));
            if (!resumed)
            {
                env.runtime.requestStop();
            }
            gameThread.join();

            t.IsTrue(resumed, "Timer2 compare should dispatch INTC_TIM2 and wake the semaphore waiter");
            t.IsFalse(schedulerThrew.load(std::memory_order_acquire), "Timer2 IRQ path should not throw");
            const std::vector<int> expected{1, 2, 3};
            t.IsTrue(g_dispatchTrace == expected,
                     "Timer2 flow should run wait, interrupt handler, then the resumed thread");
            t.Equals(g_resumedResult, g_testSemaphoreId,
                     "the Timer2 handler should hand the semaphore directly to the waiter");
        });

        tc.Run("scheduler stop wakes an idle VSync wait without a timeout", [](TestCase &t)
        {
            TestEnv env;
            env.runtime.registerFunction(kIdleVSyncWaitPc, idleVSyncWait);

            R5900Context mainContext{};
            mainContext.pc = kIdleVSyncWaitPc;
            std::atomic<bool> schedulerDone{false};
            std::atomic<bool> schedulerThrew{false};
            std::thread gameThread([&]()
            {
                try
                {
                    env.runtime.eeScheduler().reset(env.rdram.data(), mainContext);
                    env.runtime.eeScheduler().run();
                }
                catch (...)
                {
                    schedulerThrew.store(true, std::memory_order_release);
                }
                schedulerDone.store(true, std::memory_order_release);
            });

            const bool becameIdle = waitUntil([&]() {
                const EeKernelSnapshot snapshot = env.runtime.eeScheduler().snapshot();
                return snapshot.runningThreadId == 0 &&
                       !snapshot.threads.empty() &&
                       snapshot.threads.front().waitReason == EeWaitReason::VSync;
            }, std::chrono::milliseconds(80));

            env.runtime.requestStop();
            gameThread.join();

            t.IsTrue(becameIdle, "VSync wait should leave the sole guest thread waiting");
            t.IsTrue(schedulerDone.load(std::memory_order_acquire),
                     "requestStop should wake the scheduler's event wait");
            t.IsFalse(schedulerThrew.load(std::memory_order_acquire),
                      "the scheduler stop path should not throw");

            cleanupRuntime(env);
        });
    });
}
