#include "runtime/gt3_movie_worker.h"
#include "ps2_runtime.h"
#include "ps2_runtime_macros.h"
#include "runtime/ee_scheduler.h"
#include "runtime/mpeg_video_packet.h"
#include <cstdio>
#include <iostream>
#include <stdexcept>

namespace ps2x::video {
namespace {
constexpr uint32_t decoderAddress = 0x70002344, inputAddress = 0x70002300;
constexpr uint32_t stateAddress = 0x353140, stateMutex = 0x353144;
constexpr uint32_t primingAddress = 0x353114, sleepingProducer = 0x353118;
}
void Gt3MovieWorker::reset() {
    session_.stop(); token_ = 0; owner_ = 0;
    ended_ = boundary_ = stopping_ = playing_ = false; error_.clear();
}
void Gt3MovieWorker::fail(const std::string& message) {
    if (error_.empty()) {
        error_ = message;
        std::cerr << "[GT3 native movie] " << error_ << '\n';
    }
    session_.stop(); ended_ = true;
}
uint32_t Gt3MovieWorker::terminalFeed() const {
    // Engine completion and feed-side failure share one handoff: hold the reader
    // (retry 3) while original input+40 is active; original25f5f8 cleanup clears
    // it and wakes the reader, whose next feed then stops (1). Returning 1 early
    // lets 25ef00 free input and destroy output under the still-running worker.
    return runtime_.memory().read32(inputAddress + 0x40) ? 3 : 1;
}
void Gt3MovieWorker::wakeProducer() {
    auto& ee = runtime_.eeScheduler();
    const auto id = int(runtime_.memory().read32(sleepingProducer));
    const auto* thread = ee.thread(id);
    // Wake only the original producer actually waiting in SleepThread. Repeated
    // wakeups of a ready thread otherwise accumulate permits for future sleeps.
    if (thread && thread->entry == 0x25ee18 &&
        (thread->status == EeThreadStatus::Waiting || thread->status == EeThreadStatus::WaitingSuspended) &&
        thread->wait.reason == EeWaitReason::Sleep)
        ee.wakeupThread(id, false);
}
uint32_t Gt3MovieWorker::feed(uint32_t decoder, uint32_t record) {
    if (decoder != decoderAddress) { fail("Unexpected decoder passed to 25f6e0"); return 1; }
    // Engine completion precedes the original25f5f8 cleanup. Keep the reader
    // asleep until that cleanup releases the input owner and wakes it; returning
    // 1 early lets25ef00 destroy resources still used by the worker.
    if (ended_) return terminalFeed();
    if (!token_) return 3; // Readiness can preempt 25f5f8 before it enters 260998.
    if (stopping_ || runtime_.memory().read32(stateAddress) == 1) return 3;
    uint32_t offset = 0; bool scratch = false;
    if (!ps2ResolveGuestPointer(record, offset, scratch) || scratch || offset < 0x10000 ||
        uint64_t(offset) + 4096 > PS2_RAM_SIZE) {
        fail("Invalid movie record address"); return terminalFeed();
    }
    const std::span<const uint8_t> bytes(runtime_.memory().getRDRAM() + offset, 4096);
    const auto parsed = parseGt3MovieRecord(bytes);
    Gt3MovieStep result;
    if (parsed.kind == Gt3MovieRecordKind::Boundary) {
        // Return 2 only after the last decoded picture is published. On a full
        // queue the producer retries this same record after the worker wakes it.
        result = session_.finish(token_);
        if (result == Gt3MovieStep::InputDrained) { boundary_ = true; return 2; }
    } else {
        if (boundary_) {
            result = session_.repeat(token_);
            if (result == Gt3MovieStep::NeedOutput) return 3;
            if (result != Gt3MovieStep::Ready) { fail("Movie repeat failed: " + session_.error()); return terminalFeed(); }
            boundary_ = false;
        }
        result = session_.feedRecord(token_, bytes);
        if (result == Gt3MovieStep::Accepted) return 0;
    }
    if (result == Gt3MovieStep::NeedOutput) return 3;
    fail("Movie input stopped: " + session_.error()); return terminalFeed();
}
void Gt3MovieWorker::engine(R5900Context& context) {
    auto* ctx = &context;
    auto& ee = runtime_.eeScheduler(); auto& memory = runtime_.memory();
    const auto* thread = ee.currentThread();
    if (!thread || thread->entry != 0x25f5f8 || ee.currentContext() != ctx ||
        GPR_U32(ctx, 16) != decoderAddress || GPR_U32(ctx, 28) != 0x3579f0 ||
        GPR_U32(ctx, 31) != 0x25f644 || memory.read32(decoderAddress + 8) != inputAddress)
        throw std::runtime_error("GT3 native movie engine called outside its verified worker context");
    if (!token_ && !ended_) {
        owner_ = thread->id;
        token_ = session_.start(memory.read32(decoderAddress + 12), primingAddress,
                               memory.read32(decoderAddress + 16), memory.read32(decoderAddress + 20));
        if (!token_) fail("Movie output bind failed: " + session_.error());
        else std::cerr << "[GT3 native movie] decoder worker bound to original picture queue\n";
    }
    if (owner_ && owner_ != thread->id)
        throw std::runtime_error("GT3 native movie worker ownership changed without reset");
    const int mutex = int(memory.read32(stateMutex));
    const auto* semaphore = ee.semaphore(mutex);
    if (!semaphore || semaphore->maxCount != 1)
        throw std::runtime_error("GT3 native movie state mutex unavailable");
    if (ee.pollSemaphore(mutex) == mutex) {
        if (memory.read32(stateAddress) == 1) {
            stopping_ = true; memory.write32(stateAddress, 2);
        } else if (!playing_ && session_.picturesPublished() && !stopping_ && !ended_) {
            playing_ = true; memory.write32(stateAddress, 4);
        }
        ee.signalSemaphore(mutex, false);
        if (!ended_) {
            const auto result = stopping_ ? session_.drain(token_) : session_.pump(token_);
            if (result == Gt3MovieStep::Complete) ended_ = true;
            else if (result == Gt3MovieStep::Stopped || result == Gt3MovieStep::Failed)
                fail("Movie worker stopped: " + session_.error());
            if (!ended_ && result != Gt3MovieStep::NeedOutput) wakeProducer();
        }
        if (ended_ && ee.pollSemaphore(mutex) == mutex) {
            memory.write32(stateAddress, 3); ee.signalSemaphore(mutex, false);
            // Original25f5f8 clears input+40 and performs the terminal wake.
            SET_GPR_S32(ctx, 2, error_.empty() ? 0 : -1);
            ctx->pc = 0x25f644; return; // Original cleanup executes only after drain or cancellation.
        }
    }
    ctx->pc = 0x260998;
    ee.waitVSync(ee.currentVSyncTick(), 0);
}
bool tryGt3MovieEngine(PS2Runtime& runtime, R5900Context& context) {
    auto* worker = runtime.gt3MovieWorker();
    if (!worker) return false;
    worker->engine(context); return true;
}
bool tryGt3MovieFeed(PS2Runtime& runtime, R5900Context& context) {
    auto* worker = runtime.gt3MovieWorker();
    if (!worker) return false;
    auto* ctx = &context;
    SET_GPR_U32(ctx, 2, worker->feed(GPR_U32(ctx, 4), GPR_U32(ctx, 5)));
    ctx->pc = GPR_U32(ctx, 31); return true;
}
bool trapGt3MovieBufferWait(PS2Runtime& runtime, R5900Context& context) {
    if (!runtime.gt3MovieWorker()) return false; // Unverified image: original code.
    // audit_movie_buffer_wait.py finds no static caller. If one appears (computed
    // address, later-loaded code), sleeping here would never be woken because
    // 25ed78 measures the bypassed IPU queue. Fail visibly, not as a silent hang.
    auto* ctx = &context;
    char caller[16]; std::snprintf(caller, sizeof(caller), "%x", GPR_U32(ctx, 31));
    throw std::runtime_error(std::string("GT3 native movie: unsupported original 25ed30 buffering wait reached from ra=0x") + caller);
}
void beginGt3MovieWorker(PS2Runtime& runtime) {
    if (auto* worker = runtime.gt3MovieWorker()) worker->reset();
}
}
