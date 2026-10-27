#include "runtime/ps2_memory.h"
#include "runtime/ps2_address.h"
#include "runtime/vblank_clock.h"
#include "runtime/gs/gs_frontend.h"
#include "runtime/gs/gs_worker.h"
#include "ps2_log.h"
#include "graphics_transfer_trace.h"
#include <atomic>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <algorithm>
#include <string>
#include <vector>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <chrono>

namespace
{
// GT3_FRAME_TIMELINE=1: per frame (each large VIF1 chain), when the worker
// finished it, how long the mid-frame GS readback waited, and the second
// chain, in ms from the kick; the period between kicks.
// GT3_FRAME_TIMELINE_KB=<n>: the chain size that starts a frame (default
// 1024; GT4's lighter race frames send smaller chains).
struct FrameTimeline
{
    bool enabled = std::getenv("GT3_FRAME_TIMELINE") != nullptr;
    size_t frameBytes = [] {
        const char *v = std::getenv("GT3_FRAME_TIMELINE_KB");
        const long kb = v && *v ? std::strtol(v, nullptr, 10) : 0L;
        return kb > 0 ? static_cast<size_t>(kb) << 10u : size_t{1} << 20u;
    }();
    std::atomic<int64_t> kick{0}, workerDone{0}, kick2{0}, workerDone2{0}, syncStart{0}, syncEnd{0};
    std::atomic<int64_t> gatherUs{0}, gatherTags{0}; // the last frame-sized VIF1 chain's gathering on the EE
    int64_t lastKick = 0;
};
FrameTimeline &frameTimeline()
{
    static FrameTimeline timeline;
    return timeline;
}
int64_t timelineNowUs()
{
    return std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}
#if !defined(PS2X_GAME_GT3)
// PS2X_ASYNC_PATH3 (GT4, default on since phase 5 step 6 with fix H; =0 turns it off;
// analysis/phase5/specs/step4-ee-lateness.md L1, step 6 verify: L1+H had 0 lost steps
// in 5 of 5 rolling-start runs): a GIF (PATH3) DMA started while the VIF1 worker still has posted work
// is gathered on the EE as before and posted to the worker, which submits it
// behind that work (the same GS packet order as "quiesce, then submit"); the
// EE publishes its completion (D_STAT CIS2, CHCR.STR, DMAC cause 2) when the
// worker has run it, and holds VIF1's next I-bit interrupt until then. With
// the worker parked the start stays synchronous. The native GIF chain paths
// keep their quiesce.
bool asyncPath3Enabled()
{
    static const bool on = [] {
        const char *v = std::getenv("PS2X_ASYNC_PATH3");
        return !(v && *v == '0');
    }();
    return on;
}
#endif
}

namespace
{
    inline void inRange(uint32_t offset, size_t bytes, size_t regionSize, const char *op, uint32_t address)
    {
        if (static_cast<uint64_t>(offset) + static_cast<uint64_t>(bytes) > static_cast<uint64_t>(regionSize))
        {
            throw std::runtime_error(std::string(op) + " out-of-bounds at address: 0x" + std::to_string(address));
        }
    }

    template <typename T>
    inline T loadScalar(const uint8_t *base, uint32_t offset, size_t regionSize, const char *op, uint32_t address)
    {
        inRange(offset, sizeof(T), regionSize, op, address);
        T value{};
        std::memcpy(&value, base + offset, sizeof(T));
        return value;
    }

    template <typename T>
    inline void storeScalar(uint8_t *base, uint32_t offset, size_t regionSize, T value, const char *op, uint32_t address)
    {
        inRange(offset, sizeof(T), regionSize, op, address);
        std::memcpy(base + offset, &value, sizeof(T));
    }

    inline bool isGsPrivReg(uint32_t addr)
    {
        return Ps2AddressInRange(addr, PS2_GS_PRIV_REG_BASE, PS2_GS_PRIV_REG_SIZE);
    }

    inline bool isIoRegister(uint32_t addr)
    {
        return Ps2AddressInRange(addr, PS2_IO_BASE, PS2_IO_SIZE);
    }

    inline uint64_t *gsRegPtr(GSRegisters &gs, uint32_t addr)
    {
        // Support both 64-bit base offsets and +4 dword aliases.
        uint32_t off = (addr - PS2_GS_PRIV_REG_BASE) & ~0x7u;
        switch (off)
        {
        case 0x0000:
            return &gs.pmode;
        case 0x0010:
            return &gs.smode1;
        case 0x0020:
            return &gs.smode2;
        case 0x0030:
            return &gs.srfsh;
        case 0x0040:
            return &gs.synch1;
        case 0x0050:
            return &gs.synch2;
        case 0x0060:
            return &gs.syncv;
        case 0x0070:
            return &gs.dispfb1;
        case 0x0080:
            return &gs.display1;
        case 0x0090:
            return &gs.dispfb2;
        case 0x00A0:
            return &gs.display2;
        case 0x00B0:
            return &gs.extbuf;
        case 0x00C0:
            return &gs.extdata;
        case 0x00D0:
            return &gs.extwrite;
        case 0x00E0:
            return &gs.bgcolor;
        // CSR (offset 0x1000) is intentionally not handled here: it is
        // std::atomic<uint64_t> and no longer converts to uint64_t*. Callers must
        // check for offset 0x1000 themselves and go through writeCsrHalf/
        // writeCsrFull/gs.csr.load() instead of gsRegPtr().
        case 0x1010:
            return &gs.imr;
        case 0x1040:
            return &gs.busdir;
        case 0x1080:
            return &gs.siglblid;
        default:
            return nullptr;
        }
    }

    constexpr uint32_t kGsCsrRegOffset = 0x1000u;

    // Atomically apply a 32-bit write to one half (off=0 low dword, off=4 high
    // dword) of the GS CSR register. Bits 0..1 of the low dword (SIGNAL/FINISH) are
    // write-one-to-clear; everything else is a plain merge. Uses compare_exchange
    // so the whole read-modify-write is a single atomic step -- this register is
    // also touched by the vsync worker (FIELD bit) and the GIF (SIGNAL/FINISH) on
    // other threads, so a load-then-store here would race with them.
    inline void writeCsrHalf(std::atomic<uint64_t> &csr, uint32_t off, uint32_t value)
    {
        constexpr uint32_t kW1cMask = 0x3u;
        uint64_t expected = csr.load();
        uint64_t desired;
        do
        {
            if (off == 0u)
            {
                uint32_t oldLow = static_cast<uint32_t>(expected & 0xFFFFFFFFull);
                uint32_t mergedLow = (oldLow & kW1cMask) | (value & ~kW1cMask);
                desired = (expected & 0xFFFFFFFF00000000ull) | static_cast<uint64_t>(mergedLow);
                desired &= ~static_cast<uint64_t>(value & kW1cMask);
            }
            else
            {
                uint64_t mask = 0xFFFFFFFFull << (off * 8u);
                desired = (expected & ~mask) | (static_cast<uint64_t>(value) << (off * 8u));
            }
        } while (!csr.compare_exchange_weak(expected, desired));
    }

    // Same as writeCsrHalf but for a full 64-bit CSR write (bits 0..1 are still
    // write-one-to-clear against the current value).
    inline void writeCsrFull(std::atomic<uint64_t> &csr, uint64_t value)
    {
        constexpr uint64_t kW1cMask = 0x3ull;
        uint64_t expected = csr.load();
        uint64_t desired;
        do
        {
            desired = (expected & kW1cMask) | (value & ~kW1cMask);
            desired &= ~(value & kW1cMask);
        } while (!csr.compare_exchange_weak(expected, desired));
    }

    constexpr std::array<uint32_t, 4> kEeTimerBases = {
        0x10000000u,
        0x10000800u,
        0x10001000u,
        0x10001800u,
    };
    constexpr uint32_t kEeTimerCountOffset = 0x00u;
    constexpr uint32_t kEeTimerModeOffset = 0x10u;
    constexpr uint32_t kEeTimerCompareOffset = 0x20u;
    constexpr uint32_t kEeTimerHoldOffset = 0x30u;
    constexpr uint32_t kEeTimerModeClksMask = 0x3u;
    constexpr uint32_t kEeTimerModeConfigMask = 0x3FFu;
    constexpr uint32_t kEeTimerModeStatusMask = 0xC00u;
    constexpr uint32_t kEeTimerModeZret = 1u << 6;
    constexpr uint32_t kEeTimerModeCue = 1u << 7;
    constexpr uint32_t kEeTimerModeCmpe = 1u << 8;
    constexpr uint32_t kEeTimerModeOvfe = 1u << 9;
    constexpr uint32_t kEeTimerModeEquf = 1u << 10;
    constexpr uint32_t kEeTimerModeOvff = 1u << 11;
    constexpr uint64_t kEeClockHz = 294912000ull;
    constexpr std::array<uint64_t, 4> kEeTimerClockHz = {
        147456000ull,
        9216000ull,
        576000ull,
        15734ull,
    };

    inline bool decodeEeTimerRegister(uint32_t address, size_t &timerIndex, uint32_t &offset)
    {
        for (size_t index = 0; index < kEeTimerBases.size(); ++index)
        {
            const uint32_t candidateOffset = address - kEeTimerBases[index];
            if (candidateOffset == kEeTimerCountOffset ||
                candidateOffset == kEeTimerModeOffset ||
                candidateOffset == kEeTimerCompareOffset ||
                (index < 2u && candidateOffset == kEeTimerHoldOffset))
            {
                timerIndex = index;
                offset = candidateOffset;
                return true;
            }
        }
        return false;
    }

    constexpr uint64_t ticksUntilMatch(uint32_t count, uint32_t target)
    {
        const uint32_t distance = (target - count) & 0xFFFFu;
        return distance == 0u ? 0x10000ull : static_cast<uint64_t>(distance);
    }

    struct DmaTagView
    {
        uint16_t qwc = 0;
        uint8_t id = 0;
        bool irq = false;
        uint32_t addr = 0;
        uint32_t upper = 0;
    };

    inline DmaTagView decodeDmaTag(uint64_t tag)
    {
        DmaTagView out{};
        out.qwc = static_cast<uint16_t>(tag & 0xFFFFu);
        out.id = static_cast<uint8_t>((tag >> 28u) & 0x7u);
        out.irq = ((tag >> 31u) & 0x1ull) != 0ull;
        out.addr = static_cast<uint32_t>((tag >> 32u) & 0x7FFFFFFFu);
        out.upper = static_cast<uint32_t>((tag >> 16u) & 0xFFFFu);
        return out;
    }

    inline uint32_t gifTagNloop(uint64_t tagLo)
    {
        return static_cast<uint32_t>(tagLo & 0x7FFFu);
    }

    inline uint8_t gifTagFlg(uint64_t tagLo)
    {
        return static_cast<uint8_t>((tagLo >> 58u) & 0x3u);
    }

    inline uint32_t gifTagNreg(uint64_t tagLo)
    {
        uint32_t nreg = static_cast<uint32_t>((tagLo >> 60u) & 0xFu);
        return nreg == 0u ? 16u : nreg;
    }

}

// Helpers for GS VRAM addressing (PSMCT32 path).
static inline uint32_t gs_vram_offset(uint32_t basePage, uint32_t x, uint32_t y, uint32_t fbw)
{
    // basePage is in 2048-byte units; fbw is in blocks of 64 pixels.
    uint32_t strideBytes = fbw * 64 * 4;
    return basePage * 2048 + y * strideBytes + x * 4;
}

PS2Memory::PS2Memory()
    : m_rdram(nullptr), m_scratchpad(nullptr), iop_ram(nullptr), m_seenGifCopy(false), m_gsVRAM(nullptr)
{
    ps2SetScratchpadHostPtr(nullptr);
}

#if defined(PS2X_GAME_GT4)
ps2x::video::Ipu &PS2Memory::ipu()
{
    if (!m_ipu)
    {
        ps2x::video::IpuBus bus;
        auto locate = [this](uint32_t address, size_t size) -> uint8_t * {
            if (address & 0x80000000u)
                return ((address & 0x3fffu) + size <= PS2_SCRATCHPAD_SIZE) ? m_scratchpad + (address & 0x3fffu) : nullptr;
            const uint32_t phys = address & 0x01ffffffu;
            return (phys + size <= PS2_RAM_SIZE) ? m_rdram + phys : nullptr;
        };
        bus.read = [locate](uint32_t address, void *dst, size_t size) {
            const uint8_t *src = locate(address, size);
            if (!src)
                return false;
            std::memcpy(dst, src, size);
            return true;
        };
        bus.write = [this, locate](uint32_t address, const void *src, size_t size) {
            uint8_t *dst = locate(address, size);
            if (!dst)
                return false;
            std::memcpy(dst, src, size);
            if (!(address & 0x80000000u))
                markModified(address & 0x01ffffffu, static_cast<uint32_t>(size));
            return true;
        };
        bus.reg = [this](uint32_t address) -> uint32_t & { return m_ioRegisters[address]; };
        bus.finishDma = [this](unsigned channel) { finishIpuDma(channel); };
        bus.raiseInterrupt = [this] { m_pendingIpuInterrupts.fetch_add(1u); };
        m_ipu = std::make_unique<ps2x::video::Ipu>(std::move(bus));
    }
    return *m_ipu;
}

void PS2Memory::finishIpuDma(unsigned channel)
{
    uint32_t &status = m_ioRegisters[0x1000E010u];
    status |= 1u << channel;
    if ((status & (status >> 16) & 0x3FFu) != 0u) status |= 1u << 31;
    else status &= ~(1u << 31);
    const uint32_t base = channel == 3u ? 0x1000B000u : 0x1000B400u;
    m_ioRegisters[base] &= ~0x100u;
    m_ioRegisters[base + 0x20u] = 0u;
    queueCompletedDmacCause(channel);
}
#endif

PS2Memory::~PS2Memory()
{
    stopVif1Worker();
    if (m_rdram)
    {
        delete[] m_rdram;
        m_rdram = nullptr;
    }

    if (m_scratchpad)
    {
        ps2SetScratchpadHostPtr(nullptr);
        delete[] m_scratchpad;
        m_scratchpad = nullptr;
    }

    if (m_gsVRAM)
    {
        delete[] m_gsVRAM;
        m_gsVRAM = nullptr;
    }

    if (m_vu1Code)
    {
        delete[] m_vu1Code;
        m_vu1Code = nullptr;
    }
    if (m_vu1Data)
    {
        delete[] m_vu1Data;
        m_vu1Data = nullptr;
    }
    if (m_vu0Code)
    {
        delete[] m_vu0Code;
        m_vu0Code = nullptr;
    }
    if (m_vu0Data)
    {
        delete[] m_vu0Data;
        m_vu0Data = nullptr;
    }

    if (iop_ram)
    {
        delete[] iop_ram;
        iop_ram = nullptr;
    }
}

bool PS2Memory::initialize(size_t ramSize)
{
    auto cleanup = [this]()
    {
        delete[] m_rdram;
        delete[] m_scratchpad;
        delete[] iop_ram;
        delete[] m_gsVRAM;
        delete[] m_vu0Code;
        delete[] m_vu0Data;
        delete[] m_vu1Code;
        delete[] m_vu1Data;
        m_rdram = nullptr;
        m_scratchpad = nullptr;
        ps2SetScratchpadHostPtr(nullptr);
        iop_ram = nullptr;
        m_gsVRAM = nullptr;
        m_vu0Code = nullptr;
        m_vu0Data = nullptr;
        m_vu1Code = nullptr;
        m_vu1Data = nullptr;
    };

    stopVif1Worker();
    cleanup();
    m_seenGifCopy = false;
    m_dmaStartCount.store(0, std::memory_order_relaxed);
    m_gifCopyCount.store(0, std::memory_order_relaxed);
    m_gsWriteCount.store(0, std::memory_order_relaxed);
    m_vifWriteCount.store(0, std::memory_order_relaxed);
    {
        std::lock_guard<std::mutex> lock(m_completedDmacMutex);
        m_completedDmacCauses.clear();
    }
    m_codeRegions.clear();
    m_path3Masked = false;
    m_path3MaskedFifo.clear();
    m_vif1PendingPath2ImageQwc = 0u;
    m_vif1PendingPath2DirectHl = false;
    for (auto &input : m_vifInput) input.clear();
    m_vifInputOffset = {};
    m_vifDeferredDmac = {};
    m_pendingVifInterrupts.store(0u);
    resetEeTimers();

    try
    {
        // Allocate main RAM
        m_rdram = new uint8_t[ramSize];
        std::memset(m_rdram, 0, ramSize);

        // Allocate scratchpad
        m_scratchpad = new uint8_t[PS2_SCRATCHPAD_SIZE];
        std::memset(m_scratchpad, 0, PS2_SCRATCHPAD_SIZE);
        ps2SetScratchpadHostPtr(m_scratchpad);

        // Initialize EE TLB entries (R5900 has 48 entries).
        m_tlbEntries.assign(48, TLBEntry{0, 0, 0, false});

        // Allocate IOP RAM
        iop_ram = new uint8_t[2 * 1024 * 1024]; // 2MB

        // Initialize IOP RAM with zeros
        std::memset(iop_ram, 0, 2 * 1024 * 1024);

        // Initialize I/O registers
        m_ioRegisters.clear();

        // Initialize GS registers
        memset(&gs_regs, 0, sizeof(gs_regs));
        // memset zero-fills std::atomic<uint64_t>::csr's bytes, which is not itself
        // a guaranteed-valid atomic store; make the zero-initialization explicit.
        gs_regs.csr.store(0);
        gs_regs.dispfb1 = (0ULL << 0) | (10ULL << 9) | (0ULL << 15) | (0ULL << 32) | (0ULL << 43);
        gs_regs.display1 = (0ULL << 0) | (0ULL << 12) | (0ULL << 23) | (0ULL << 27) | (639ULL << 32) | (447ULL << 44);
        gs_regs.dispfb2 = gs_regs.dispfb1;
        gs_regs.display2 = gs_regs.display1;

        // Allocate GS VRAM (4MB)
        m_gsVRAM = new uint8_t[PS2_GS_VRAM_SIZE];
        std::memset(m_gsVRAM, 0, PS2_GS_VRAM_SIZE);

        m_vu0Code = new uint8_t[PS2_VU0_CODE_SIZE];
        m_vu0Data = new uint8_t[PS2_VU0_DATA_SIZE];
        std::memset(m_vu0Code, 0, PS2_VU0_CODE_SIZE);
        std::memset(m_vu0Data, 0, PS2_VU0_DATA_SIZE);

        m_vu1Code = new uint8_t[PS2_VU1_CODE_SIZE];
        m_vu1Data = new uint8_t[PS2_VU1_DATA_SIZE];
        std::memset(m_vu1Code, 0, PS2_VU1_CODE_SIZE);
        std::memset(m_vu1Data, 0, PS2_VU1_DATA_SIZE);
        markVU0CodeModified();
        markVU1CodeModified();

        // Initialize VIF registers
        memset(&vif0_regs, 0, sizeof(vif0_regs));
        memset(&vif1_regs, 0, sizeof(vif1_regs));

        // Initialize DMA registers
        memset(dma_regs, 0, sizeof(dma_regs));

        return true;
    }
    catch (const std::exception &e)
    {
        std::cerr << "Error initializing PS2 memory: " << e.what() << std::endl;
        cleanup();
        return false;
    }
}

void PS2Memory::resetEeTimers() noexcept
{
    m_eeTimers = {};
}

uint32_t PS2Memory::advanceEeTimers(uint64_t eeCycles) noexcept
{
    if (eeCycles == 0u)
    {
        return 0u;
    }

    uint32_t interruptMask = 0u;
    for (size_t index = 0; index < m_eeTimers.size(); ++index)
    {
        EeTimer &timer = m_eeTimers[index];
        if ((timer.mode & kEeTimerModeCue) == 0u)
        {
            continue;
        }

        const uint64_t clockHz = kEeTimerClockHz[timer.mode & kEeTimerModeClksMask];
        const uint64_t wholeSeconds = eeCycles / kEeClockHz;
        const uint64_t remainingCycles = eeCycles % kEeClockHz;
        const uint64_t scaled = remainingCycles * clockHz + timer.clockRemainder;
        const uint64_t ticks = wholeSeconds * clockHz + scaled / kEeClockHz;
        timer.clockRemainder = scaled % kEeClockHz;
        if (ticks == 0u)
        {
            continue;
        }

        const uint32_t oldCount = timer.count & 0xFFFFu;
        const uint32_t compare = timer.compare & 0xFFFFu;
        const uint64_t compareDistance = ticksUntilMatch(oldCount, compare);
        const uint64_t overflowDistance = 0x10000ull - oldCount;
        const bool zeroReturn = (timer.mode & kEeTimerModeZret) != 0u;
        const bool compareReached = ticks >= compareDistance;
        bool overflowReached = false;

        if (zeroReturn)
        {
            overflowReached = ticks >= overflowDistance && overflowDistance <= compareDistance;
            if (compareReached)
            {
                const uint64_t remaining = ticks - compareDistance;
                timer.count = compare == 0u
                                  ? static_cast<uint32_t>(remaining & 0xFFFFu)
                                  : static_cast<uint32_t>(remaining % compare);
            }
            else
            {
                timer.count = static_cast<uint32_t>((oldCount + ticks) & 0xFFFFu);
            }
        }
        else
        {
            overflowReached = ticks >= overflowDistance;
            timer.count = static_cast<uint32_t>((oldCount + ticks) & 0xFFFFu);
        }

#if defined(PS2X_GAME_GT4)
        {
            static const bool timerLog = [] { const char *v = std::getenv("GT4_TIMER_LOG"); return v && *v == '1'; }();
            static uint64_t compares = 0, overflows = 0;
            if (timerLog && index == 2u)
            {
                compares += compareReached ? 1u : 0u;
                overflows += overflowReached ? 1u : 0u;
                if (((compares + overflows) & 1023u) == 0u && (compareReached || overflowReached))
                    std::fprintf(stderr, "[timer] T2 compare events %llu overflow events %llu mode %04x\n",
                                 static_cast<unsigned long long>(compares), static_cast<unsigned long long>(overflows), timer.mode);
            }
        }
#endif
        if (compareReached && (timer.mode & kEeTimerModeCmpe) != 0u && (timer.mode & kEeTimerModeEquf) == 0u)
        {
            timer.mode |= kEeTimerModeEquf;
            interruptMask |= 1u << index;
        }
        if (overflowReached && (timer.mode & kEeTimerModeOvfe) != 0u && (timer.mode & kEeTimerModeOvff) == 0u)
        {
            timer.mode |= kEeTimerModeOvff;
            interruptMask |= 1u << index;
        }
    }
    return interruptMask;
}

uint64_t PS2Memory::cyclesUntilNextEeTimerInterrupt() const noexcept
{
    uint64_t nearest = std::numeric_limits<uint64_t>::max();
    for (const EeTimer &timer : m_eeTimers)
    {
        if ((timer.mode & kEeTimerModeCue) == 0u)
        {
            continue;
        }

        const uint32_t count = timer.count & 0xFFFFu;
        const uint32_t compare = timer.compare & 0xFFFFu;
        const uint64_t compareDistance = ticksUntilMatch(count, compare);
        const uint64_t overflowDistance = 0x10000ull - count;
        uint64_t eventTicks = std::numeric_limits<uint64_t>::max();

        if ((timer.mode & kEeTimerModeCmpe) != 0u &&
            (timer.mode & kEeTimerModeEquf) == 0u)
        {
            eventTicks = compareDistance;
        }
        const bool overflowCanOccur = (timer.mode & kEeTimerModeZret) == 0u ||
                                      overflowDistance <= compareDistance;
        if (overflowCanOccur &&
            (timer.mode & kEeTimerModeOvfe) != 0u &&
            (timer.mode & kEeTimerModeOvff) == 0u)
        {
            eventTicks = std::min(eventTicks, overflowDistance);
        }
        if (eventTicks == std::numeric_limits<uint64_t>::max())
        {
            continue;
        }

        const uint64_t clockHz = kEeTimerClockHz[timer.mode & kEeTimerModeClksMask];
        const uint64_t numerator = eventTicks * kEeClockHz - timer.clockRemainder;
        const uint64_t cycles = (numerator + clockHz - 1u) / clockHz;
        nearest = std::min(nearest, std::max<uint64_t>(1u, cycles));
    }
    return nearest;
}

bool PS2Memory::isScratchpad(uint32_t address) const
{
    return ps2IsScratchpadAddress(address);
}

uint8_t *PS2Memory::mapVuMemory(uint32_t physAddr, uint32_t size, uint32_t &offset, uint32_t &limit)
{
    return const_cast<uint8_t *>(static_cast<const PS2Memory *>(this)->mapVuMemory(physAddr, size, offset, limit));
}

const uint8_t *PS2Memory::mapVuMemory(uint32_t physAddr, uint32_t size, uint32_t &offset, uint32_t &limit) const
{
    auto mapRange = [&](uint32_t base, uint32_t rangeSize, const uint8_t *ptr) -> const uint8_t *
    {
        if (!ptr || physAddr < base)
        {
            return nullptr;
        }
        const uint32_t local = physAddr - base;
        if (local >= rangeSize || size > (rangeSize - local))
        {
            return nullptr;
        }
        offset = local;
        limit = rangeSize;
        return ptr;
    };

    if (const uint8_t *ptr = mapRange(PS2_VU0_CODE_BASE, PS2_VU0_CODE_SIZE, m_vu0Code))
    {
        return ptr;
    }
    if (const uint8_t *ptr = mapRange(PS2_VU0_DATA_BASE, PS2_VU0_DATA_SIZE, m_vu0Data))
    {
        return ptr;
    }
    const uint8_t *vu1 = mapRange(PS2_VU1_CODE_BASE, PS2_VU1_CODE_SIZE, m_vu1Code);
    if (!vu1)
        vu1 = mapRange(PS2_VU1_DATA_BASE, PS2_VU1_DATA_SIZE, m_vu1Data);
    // VU1 memory belongs to the VIF1 worker until it parks.
    if (vu1 && m_vif1Worker.active())
        const_cast<PS2Memory *>(this)->quiesceVif1(Vif1Worker::QuiesceSite::Vu1Memory);
    return vu1;
}

uint32_t PS2Memory::translateAddress(uint32_t virtualAddress)
{
    if (isScratchpad(virtualAddress))
    {
        return ps2ScratchpadOffset(virtualAddress);
    }

    // EE uncached aliases of main RAM (per PS2 memory map):
    //   0x20000000-0x3FFFFFFF -> 32MB mirror of RDRAM
    // This includes the accelerated window rooted at 0x30100000.
    if (Ps2IsUncachedRamMirrorAddress(virtualAddress))
    {
        return virtualAddress & PS2_RAM_MASK;
    }

    // KSEG0/KSEG1 direct-mapped window.
    if (Ps2IsKseg01Address(virtualAddress))
    {
        return Ps2DirectMappedPhysicalAddress(virtualAddress);
    }

    // In this runtime, low segments are treated as physical-style addresses already.
    if (virtualAddress < 0x80000000)
    {
        return virtualAddress;
    }

    // KSEG2/KSEG3 are TLB mapped.
    if (Ps2IsKseg23Address(virtualAddress))
    {
        for (const auto &entry : m_tlbEntries)
        {
            if (entry.valid)
            {
                // PageMask uses bits [24:13]. Build an address-level mask (plus 4KB base page bits).
                const uint32_t mask = entry.mask & 0x01FFE000u;
                const uint32_t compareMask = ~(mask | 0xFFFu);
                if ((virtualAddress & compareMask) == (entry.vpn & compareMask))
                {
                    // TLB hit
                    const uint32_t pageOffsetMask = mask | 0xFFFu;
                    const uint32_t physBase = entry.pfn << 12;
                    return physBase | (virtualAddress & pageOffsetMask);
                }
            }
        }
        throw std::runtime_error("TLB miss for address: 0x" + std::to_string(virtualAddress));
    }

    return virtualAddress;
}

bool PS2Memory::tlbRead(uint32_t index, uint32_t &vpn, uint32_t &pfn, uint32_t &mask, bool &valid) const
{
    if (index >= m_tlbEntries.size())
    {
        return false;
    }

    const TLBEntry &entry = m_tlbEntries[index];
    vpn = entry.vpn;
    pfn = entry.pfn;
    mask = entry.mask;
    valid = entry.valid;
    return true;
}

bool PS2Memory::tlbWrite(uint32_t index, uint32_t vpn, uint32_t pfn, uint32_t mask, bool valid)
{
    if (index >= m_tlbEntries.size())
    {
        return false;
    }

    TLBEntry &entry = m_tlbEntries[index];
    entry.vpn = vpn & 0xFFFFF000u;
    entry.pfn = pfn & 0x000FFFFFu;
    entry.mask = mask & 0x01FFE000u;
    entry.valid = valid;
    return true;
}

int32_t PS2Memory::tlbProbe(uint32_t vpn) const
{
    const uint32_t normalizedVpn = vpn & 0xFFFFF000u;
    for (uint32_t i = 0; i < static_cast<uint32_t>(m_tlbEntries.size()); ++i)
    {
        const TLBEntry &entry = m_tlbEntries[i];
        if (!entry.valid)
        {
            continue;
        }

        const uint32_t mask = entry.mask & 0x01FFE000u;
        const uint32_t compareMask = ~(mask | 0xFFFu);
        if ((normalizedVpn & compareMask) == (entry.vpn & compareMask))
        {
            return static_cast<int32_t>(i);
        }
    }

    return -1;
}

uint8_t PS2Memory::read8(uint32_t address)
{
    const bool scratch = isScratchpad(address);
    uint32_t physAddr = translateAddress(address);

    if (scratch)
    {
        return m_scratchpad[physAddr];
    }
    if (physAddr < PS2_RAM_SIZE)
    {
        return m_rdram[physAddr];
    }
    uint32_t vuOffset = 0;
    uint32_t vuLimit = 0;
    if (const uint8_t *vuMem = mapVuMemory(physAddr, sizeof(uint8_t), vuOffset, vuLimit))
    {
        (void)vuLimit;
        return vuMem[vuOffset];
    }
    else if (isIoRegister(physAddr))
    {
        uint32_t regAddr = physAddr & ~0x3;
        uint32_t value = readIORegister(regAddr);
        uint32_t shift = (physAddr & 3) * 8;
        return static_cast<uint8_t>((value >> shift) & 0xFF);
    }

    return 0;
}

uint16_t PS2Memory::read16(uint32_t address)
{
    if (address & 1)
    {
        throw std::runtime_error("Unaligned 16-bit read at address: 0x" + std::to_string(address));
    }

    const bool scratch = isScratchpad(address);
    uint32_t physAddr = translateAddress(address);

    if (scratch)
    {
        return loadScalar<uint16_t>(m_scratchpad, physAddr, PS2_SCRATCHPAD_SIZE, "read16 scratchpad", address);
    }
    if (physAddr < PS2_RAM_SIZE)
    {
        return loadScalar<uint16_t>(m_rdram, physAddr, PS2_RAM_SIZE, "read16 rdram", address);
    }
    uint32_t vuOffset = 0;
    uint32_t vuLimit = 0;
    if (const uint8_t *vuMem = mapVuMemory(physAddr, sizeof(uint16_t), vuOffset, vuLimit))
    {
        return loadScalar<uint16_t>(vuMem, vuOffset, vuLimit, "read16 vu", address);
    }
    else if (isIoRegister(physAddr))
    {
        uint32_t regAddr = physAddr & ~0x3;
        uint32_t value = readIORegister(regAddr);
        uint32_t shift = (physAddr & 2) * 8;
        return static_cast<uint16_t>((value >> shift) & 0xFFFF);
    }

    return 0;
}

uint32_t PS2Memory::read32(uint32_t address)
{
    if (address & 3)
    {
        throw std::runtime_error("Unaligned 32-bit read at address: 0x" + std::to_string(address));
    }

    if (isGsPrivReg(address))
    {
        uint32_t off = address & 7;
        const uint32_t regOff = (address - PS2_GS_PRIV_REG_BASE) & ~0x7u;
        if (regOff == kGsCsrRegOffset)
        {
            uint64_t val = gs_regs.csr.load();
#if !defined(PS2X_GAME_GT3)
            GsWorker::noteGuestRead(regOff, off == 0u ? val : 0u); // GT3_GS_WORKER_STATS
#endif
            return (uint32_t)(val >> (off * 8));
        }
        uint64_t *reg = gsRegPtr(gs_regs, address);
        if (!reg)
            return 0;
        uint64_t val = *reg;
#if !defined(PS2X_GAME_GT3)
        GsWorker::noteGuestRead(regOff, val); // counts SIGLBLID reads (GT3_GS_WORKER_STATS)
#endif
        return (uint32_t)(val >> (off * 8));
    }

    const bool scratch = isScratchpad(address);
    uint32_t physAddr = translateAddress(address);

    if (scratch)
    {
        return loadScalar<uint32_t>(m_scratchpad, physAddr, PS2_SCRATCHPAD_SIZE, "read32 scratchpad", address);
    }
    if (physAddr < PS2_RAM_SIZE)
    {
        return loadScalar<uint32_t>(m_rdram, physAddr, PS2_RAM_SIZE, "read32 rdram", address);
    }
    uint32_t vuOffset = 0;
    uint32_t vuLimit = 0;
    if (const uint8_t *vuMem = mapVuMemory(physAddr, sizeof(uint32_t), vuOffset, vuLimit))
    {
        return loadScalar<uint32_t>(vuMem, vuOffset, vuLimit, "read32 vu", address);
    }
    else if (isIoRegister(physAddr))
    {
        return readIORegister(physAddr);
    }

    return 0;
}

uint64_t PS2Memory::read64(uint32_t address)
{
    if (address & 7)
    {
        throw std::runtime_error("Unaligned 64-bit read at address: 0x" + std::to_string(address));
    }

    if (isGsPrivReg(address))
    {
        const uint32_t regOff = (address - PS2_GS_PRIV_REG_BASE) & ~0x7u;
        if (regOff == kGsCsrRegOffset)
        {
#if !defined(PS2X_GAME_GT3)
            const uint64_t val = gs_regs.csr.load();
            GsWorker::noteGuestRead(regOff, val); // GT3_GS_WORKER_STATS
            return val;
#else
            return gs_regs.csr.load();
#endif
        }
        uint64_t *reg = gsRegPtr(gs_regs, address);
#if !defined(PS2X_GAME_GT3)
        if (reg)
            GsWorker::noteGuestRead(regOff, *reg); // counts SIGLBLID reads (GT3_GS_WORKER_STATS)
#endif
        return reg ? *reg : 0;
    }

    const bool scratch = isScratchpad(address);
    uint32_t physAddr = translateAddress(address);

    if (scratch)
    {
        return loadScalar<uint64_t>(m_scratchpad, physAddr, PS2_SCRATCHPAD_SIZE, "read64 scratchpad", address);
    }
    if (physAddr < PS2_RAM_SIZE)
    {
        return loadScalar<uint64_t>(m_rdram, physAddr, PS2_RAM_SIZE, "read64 rdram", address);
    }
    uint32_t vuOffset = 0;
    uint32_t vuLimit = 0;
    if (const uint8_t *vuMem = mapVuMemory(physAddr, sizeof(uint64_t), vuOffset, vuLimit))
    {
        return loadScalar<uint64_t>(vuMem, vuOffset, vuLimit, "read64 vu", address);
    }

#if defined(PS2X_GAME_GT4)
    if (physAddr >= 0x10002000u && physAddr < 0x10002040u)
    {
        uint64_t v = 0;
        switch (physAddr & ~7u)
        {
        case 0x10002000u: v = ipu().readCmd(); break;
        case 0x10002010u: v = ipu().readCtrl(); break;
        case 0x10002020u: v = ipu().readBp(); break;
        case 0x10002030u: v = ipu().readTop(); break;
        default: break;
        }
        ps2x::vblank::noteIpuWork(); // PS2X_IPU_VBLANK (L2n)
        return v;
    }
#endif
    // 64-bit IO read: compose from the two adjacent 32-bit IO register slots
    // to avoid any side-effects from read32 handlers.
    if (isIoRegister(address))
    {
        uint32_t lo = m_ioRegisters.count(address) ? m_ioRegisters[address] : 0u;
        uint32_t hi = m_ioRegisters.count(address + 4) ? m_ioRegisters[address + 4] : 0u;
        return static_cast<uint64_t>(lo) | (static_cast<uint64_t>(hi) << 32);
    }
    return (uint64_t)read32(address) | ((uint64_t)read32(address + 4) << 32);
}

__m128i PS2Memory::read128(uint32_t address)
{
    if (address & 15)
    {
        throw std::runtime_error("Unaligned 128-bit read at address: 0x" + std::to_string(address));
    }

    const bool scratch = isScratchpad(address);
    uint32_t physAddr = translateAddress(address);

    if (scratch)
    {
        inRange(physAddr, sizeof(__m128i), PS2_SCRATCHPAD_SIZE, "read128 scratchpad", address);
        return _mm_loadu_si128(reinterpret_cast<__m128i *>(&m_scratchpad[physAddr]));
    }
    if (physAddr < PS2_RAM_SIZE)
    {
        inRange(physAddr, sizeof(__m128i), PS2_RAM_SIZE, "read128 rdram", address);
        return _mm_loadu_si128(reinterpret_cast<__m128i *>(&m_rdram[physAddr]));
    }
    uint32_t vuOffset = 0;
    uint32_t vuLimit = 0;
    if (const uint8_t *vuMem = mapVuMemory(physAddr, sizeof(__m128i), vuOffset, vuLimit))
    {
        inRange(vuOffset, sizeof(__m128i), vuLimit, "read128 vu", address);
        return _mm_loadu_si128(reinterpret_cast<const __m128i *>(vuMem + vuOffset));
    }

#if defined(PS2X_GAME_GT4)
    if (physAddr == 0x10007000u)
    {
        alignas(16) uint8_t q[16]{};
        ipu().readOutputFifo(q);
        ps2x::vblank::noteIpuWork();
        return _mm_load_si128(reinterpret_cast<const __m128i *>(q));
    }
#endif
    // 128-bit reads are primarily for quad-word loads in the EE, which are only valid for RAM areas
    // Return zeroes for unsupported areas
    return _mm_setzero_si128();
}

void PS2Memory::write8(uint32_t address, uint8_t value)
{
    const bool scratch = isScratchpad(address);
    uint32_t physAddr = translateAddress(address);

    if (scratch)
    {
        m_scratchpad[physAddr] = value;
    }
    else if (physAddr < PS2_RAM_SIZE)
    {
        m_rdram[physAddr] = value;
    }
    else
    {
        uint32_t vuOffset = 0;
        uint32_t vuLimit = 0;
        if (uint8_t *vuMem = mapVuMemory(physAddr, sizeof(uint8_t), vuOffset, vuLimit))
        {
            (void)vuLimit;
            vuMem[vuOffset] = value;
            if (vuMem == m_vu0Code)
                markVU0CodeModified();
            else if (vuMem == m_vu1Code)
                markVU1CodeModified();
            return;
        }
    }
    if (isIoRegister(physAddr))
    {
        // IO registers - handle byte writes by modifying the appropriate byte in the word
        uint32_t regAddr = physAddr & ~0x3;
        uint32_t shift = (physAddr & 3) * 8;
        uint32_t mask = ~(0xFF << shift);
        uint32_t newValue = (m_ioRegisters[regAddr] & mask) | ((uint32_t)value << shift);
        writeIORegister(regAddr, newValue);
    }
}

void PS2Memory::write16(uint32_t address, uint16_t value)
{
    if (address & 1)
    {
        throw std::runtime_error("Unaligned 16-bit write at address: 0x" + std::to_string(address));
    }

    const bool scratch = isScratchpad(address);
    uint32_t physAddr = translateAddress(address);

    if (scratch)
    {
        storeScalar<uint16_t>(m_scratchpad, physAddr, PS2_SCRATCHPAD_SIZE, value, "write16 scratchpad", address);
    }
    else if (physAddr < PS2_RAM_SIZE)
    {
        storeScalar<uint16_t>(m_rdram, physAddr, PS2_RAM_SIZE, value, "write16 rdram", address);
    }
    else
    {
        uint32_t vuOffset = 0;
        uint32_t vuLimit = 0;
        if (uint8_t *vuMem = mapVuMemory(physAddr, sizeof(uint16_t), vuOffset, vuLimit))
        {
            storeScalar<uint16_t>(vuMem, vuOffset, vuLimit, value, "write16 vu", address);
            if (vuMem == m_vu0Code)
                markVU0CodeModified();
            else if (vuMem == m_vu1Code)
                markVU1CodeModified();
            return;
        }
    }
    if (isIoRegister(physAddr))
    {
        uint32_t regAddr = physAddr & ~0x3;
        uint32_t shift = (physAddr & 2) * 8;
        uint32_t mask = ~(0xFFFF << shift);
        uint32_t newValue = (m_ioRegisters[regAddr] & mask) | ((uint32_t)value << shift);
        writeIORegister(regAddr, newValue);
    }
}

void PS2Memory::write32(uint32_t address, uint32_t value)
{
    if (address & 3)
    {
        throw std::runtime_error("Unaligned 32-bit write at address: 0x" + std::to_string(address));
    }

    if (isGsPrivReg(address))
    {
        uint32_t off = address & 7;
        const uint32_t regOff = (address - PS2_GS_PRIV_REG_BASE) & ~0x7u;
        if (regOff == kGsCsrRegOffset)
        {
            // CSR: bits 0..1 of the low dword are write-one-to-clear status bits.
            // Done as a single atomic RMW -- see writeCsrHalf's comment.
            writeCsrHalf(gs_regs.csr, off, value);
        }
        else if (uint64_t *reg = gsRegPtr(gs_regs, address))
        {
            uint64_t mask = 0xFFFFFFFFULL << (off * 8);
            uint64_t newVal = (*reg & ~mask) | ((uint64_t)value << (off * 8));
            *reg = newVal;
        }
        return;
    }

    const bool scratch = isScratchpad(address);
    uint32_t physAddr = translateAddress(address);

    if (scratch)
    {
        storeScalar<uint32_t>(m_scratchpad, physAddr, PS2_SCRATCHPAD_SIZE, value, "write32 scratchpad", address);
    }
    else if (physAddr < PS2_RAM_SIZE)
    {
        // Check if this might be code modification
        markModified(address, 4);

        storeScalar<uint32_t>(m_rdram, physAddr, PS2_RAM_SIZE, value, "write32 rdram", address);
    }
    else
    {
        uint32_t vuOffset = 0;
        uint32_t vuLimit = 0;
        if (uint8_t *vuMem = mapVuMemory(physAddr, sizeof(uint32_t), vuOffset, vuLimit))
        {
            storeScalar<uint32_t>(vuMem, vuOffset, vuLimit, value, "write32 vu", address);
            if (vuMem == m_vu0Code)
                markVU0CodeModified();
            else if (vuMem == m_vu1Code)
                markVU1CodeModified();
            return;
        }
    }
    if (isIoRegister(physAddr))
    {
        writeIORegister(physAddr, value);
    }
}

void PS2Memory::write64(uint32_t address, uint64_t value)
{
    if (address & 7)
    {
        throw std::runtime_error("Unaligned 64-bit write at address: 0x" + std::to_string(address));
    }

    if (isGsPrivReg(address))
    {
        const uint32_t regOff = (address - PS2_GS_PRIV_REG_BASE) & ~0x7u;
        if (regOff == kGsCsrRegOffset)
        {
            // CSR: bits 0..1 are write-one-to-clear status bits. Done as a single
            // atomic RMW -- see writeCsrFull's comment.
            writeCsrFull(gs_regs.csr, value);
        }
        else if (uint64_t *reg = gsRegPtr(gs_regs, address))
        {
            *reg = value;
        }
        return;
    }

    const bool scratch = isScratchpad(address);
    uint32_t physAddr = translateAddress(address);

    if (scratch)
    {
        storeScalar<uint64_t>(m_scratchpad, physAddr, PS2_SCRATCHPAD_SIZE, value, "write64 scratchpad", address);
    }
    else if (physAddr < PS2_RAM_SIZE)
    {
        markModified(address, 8);
        storeScalar<uint64_t>(m_rdram, physAddr, PS2_RAM_SIZE, value, "write64 rdram", address);
    }
    else
    {
        uint32_t vuOffset = 0;
        uint32_t vuLimit = 0;
        if (uint8_t *vuMem = mapVuMemory(physAddr, sizeof(uint64_t), vuOffset, vuLimit))
        {
            storeScalar<uint64_t>(vuMem, vuOffset, vuLimit, value, "write64 vu", address);
            if (vuMem == m_vu0Code)
                markVU0CodeModified();
            else if (vuMem == m_vu1Code)
                markVU1CodeModified();
            return;
        }
    }
    if (isIoRegister(physAddr))
    {
        write32(address, (uint32_t)value);
        write32(address + 4, (uint32_t)(value >> 32));
    }
}

void PS2Memory::write128(uint32_t address, __m128i value)
{
    if (address & 15)
    {
        throw std::runtime_error("Unaligned 128-bit write at address: 0x" + std::to_string(address));
    }

    const bool scratch = isScratchpad(address);
    uint32_t physAddr = translateAddress(address);

    if (scratch)
    {
        inRange(physAddr, sizeof(__m128i), PS2_SCRATCHPAD_SIZE, "write128 scratchpad", address);
        _mm_storeu_si128(reinterpret_cast<__m128i *>(&m_scratchpad[physAddr]), value);
    }
    else if (physAddr < PS2_RAM_SIZE)
    {
        markModified(address, 16);
        inRange(physAddr, sizeof(__m128i), PS2_RAM_SIZE, "write128 rdram", address);
        _mm_storeu_si128(reinterpret_cast<__m128i *>(&m_rdram[physAddr]), value);
    }
    else
    {
        uint32_t vuOffset = 0;
        uint32_t vuLimit = 0;
        if (uint8_t *vuMem = mapVuMemory(physAddr, sizeof(__m128i), vuOffset, vuLimit))
        {
            inRange(vuOffset, sizeof(__m128i), vuLimit, "write128 vu", address);
            _mm_storeu_si128(reinterpret_cast<__m128i *>(vuMem + vuOffset), value);
            if (vuMem == m_vu0Code)
                markVU0CodeModified();
            else if (vuMem == m_vu1Code)
                markVU1CodeModified();
            return;
        }
    }
#if defined(PS2X_GAME_GT4)
    if (physAddr == 0x10007010u)
    {
        alignas(16) uint8_t q[16];
        _mm_store_si128(reinterpret_cast<__m128i *>(q), value);
        ipu().writeInputFifo(q);
        ps2x::vblank::noteIpuWork();
        return;
    }
#endif
    if (isIoRegister(physAddr))
    {
        // Non-RAM 128-bit stores are modeled as two 64-bit stores.
        uint64_t lo = _mm_extract_epi64(value, 0);
        uint64_t hi = _mm_extract_epi64(value, 1);

        write64(address, lo);
        write64(address + 8, hi);
    }
}

bool PS2Memory::writeIORegister(uint32_t address, uint32_t value)
{
    size_t timerIndex = 0u;
    uint32_t timerOffset = 0u;
    if (decodeEeTimerRegister(address, timerIndex, timerOffset))
    {
        EeTimer &timer = m_eeTimers[timerIndex];
        switch (timerOffset)
        {
        case kEeTimerCountOffset:
            timer.count = value & 0xFFFFu;
            timer.clockRemainder = 0u;
            break;
        case kEeTimerModeOffset:
        {
            const uint32_t previousMode = timer.mode;
            const uint32_t status = (previousMode & kEeTimerModeStatusMask) &~(value & kEeTimerModeStatusMask);
            timer.mode = (value & kEeTimerModeConfigMask) | status;
            if (((previousMode ^ timer.mode) & (kEeTimerModeClksMask | kEeTimerModeCue)) != 0u)
            {
                timer.clockRemainder = 0u;
            }
            break;
        }
        case kEeTimerCompareOffset:
            timer.compare = value & 0xFFFFu;
#if defined(PS2X_GAME_GT4)
            {
                static const bool timerLog = [] { const char *v = std::getenv("GT4_TIMER_LOG"); return v && *v == '1'; }();
                static uint32_t logs = 0;
                if (timerLog && logs++ < 200)
                    std::fprintf(stderr, "[timer] T%zu COMP=%04x mode=%04x count=%04x\n", timerIndex, timer.compare, timer.mode, timer.count & 0xffffu);
            }
#endif
            break;
        case kEeTimerHoldOffset:
            timer.hold = value & 0xFFFFu;
            break;
        default:
            return false;
        }
        return true;
    }

    if (isGsPrivReg(address))
    {
        // NB: unreachable from write8/16/32/64 today since those all funnel IO
        // register writes through addresses in PS2_IO_BASE's range, which is
        // disjoint from PS2_GS_PRIV_REG_BASE; kept correct for direct callers.
        m_ioRegisters[address] = value;
        const uint32_t off = address & 7u;
        const uint32_t regOff = (address - PS2_GS_PRIV_REG_BASE) & ~0x7u;
        if (regOff == kGsCsrRegOffset)
        {
            writeCsrHalf(gs_regs.csr, off, value);
        }
        else if (uint64_t *reg = gsRegPtr(gs_regs, address))
        {
            const uint64_t mask = 0xFFFFFFFFull << (off * 8u);
            *reg = (*reg & ~mask) | (static_cast<uint64_t>(value) << (off * 8u));
        }
        m_gsWriteCount.fetch_add(1, std::memory_order_relaxed);
        return true;
    }

#if defined(PS2X_GAME_GT4)
    if (address >= 0x10002000u && address < 0x10002040u)
    {
        if (address == 0x10002000u)
            ipu().writeCmd(value);
        else if (address == 0x10002010u)
            ipu().writeCtrl(value);
        ps2x::vblank::noteIpuWork(); // PS2X_IPU_VBLANK (L2n)
        return true;
    }
    if ((address == 0x1000B000u || address == 0x1000B400u))
    {
        m_ioRegisters[address] = value;
        if (value & 0x100u)
        {
            const auto dctrlIt = m_ioRegisters.find(0x1000E000u);
            const bool dmacEnabled = (dctrlIt == m_ioRegisters.end()) || ((dctrlIt->second & 0x1u) != 0u);
            if (dmacEnabled)
                ipu().dmaStarted(address == 0x1000B000u ? 3u : 4u);
        }
        else
            ipu().dmaStopped(address == 0x1000B000u ? 3u : 4u);
        ps2x::vblank::noteIpuWork();
        return true;
    }
#endif
    if (address >= 0x10002000 && address <= 0x10002030)
    {
        if (address == 0x10002010)
        {
            m_ioRegisters[address] = value & ~(1u << 31);
            if (value & (1u << 30))
            {
                m_ioRegisters[0x10002000] = 0;
                m_ioRegisters[0x10002020] = 0;
                m_ioRegisters[0x10002030] = 0;
            }
        }
        else
        {
            m_ioRegisters[address] = value;
        }
        return true;
    }

    if (address == 0x1000E010u)
    {
        const uint32_t current = m_ioRegisters.count(address) ? m_ioRegisters[address] : 0u;
        uint32_t status = current & 0x3FFu;
        uint32_t mask = (current >> 16) & 0x3FFu;

        // D_STAT low bits are W1C status, high bits [16..25] toggle masks on write-one.
        status &= ~(value & 0x3FFu);
        mask ^= ((value >> 16) & 0x3FFu);

        uint32_t next = (current & ~((0x3FFu) | (0x3FFu << 16) | (1u << 31)));
        next |= status | (mask << 16);
        if ((status & mask) != 0u)
            next |= (1u << 31);
        m_ioRegisters[address] = next;
        return true;
    }

    m_ioRegisters[address] = value;

    if ((address >= 0x10003800u && address < 0x10003A00u) ||
        (address >= 0x10003C00u && address < 0x10003E00u))
    {
        m_vifWriteCount.fetch_add(1, std::memory_order_relaxed);
        const uint32_t channel = (address >> 10) & 1u;
        // FBRST decides for itself whether the VIF1 worker has to park first.
        if (channel == 1u && (address & 0x3FFu) != 0x10u)
            quiesceVif1(Vif1Worker::QuiesceSite::Vif1Write);
        auto &regs = channel == 0u ? vif0_regs : vif1_regs;

        switch (address & 0x3FFu)
        {
        case 0x10u:
            writeVifFbrst(channel, value);
            break;
        case 0x20u:
            regs.err = value & 7u;
            break;
        case 0x30u:
            regs.mark = value & 0xFFFFu;
            regs.stat &= ~(1u << 6); // clear MRK flag on CPU write
            break;
        case 0x40u:
            regs.cycle = value & 0xFFFFu;
            break;
        case 0x50u:
            regs.mode = value & 0x3u;
            break;
        case 0x60u:
            regs.num = value & 0xFFu;
            break;
        case 0x70u:
            regs.mask = value;
            break;
        case 0x80u:
            regs.code = value;
            break;
        case 0x90u:
            regs.itops = value & 0x3FFu;
            break;
        case 0xA0u:
            regs.base = value & 0x3FFu;
            break;
        case 0xB0u:
            regs.ofst = value & 0x3FFu;
            break;
        case 0xC0u:
            regs.tops = value & 0x3FFu;
            break;
        case 0xD0u:
            regs.itop = value & 0x3FFu;
            break;
        case 0xE0u:
            regs.top = value & 0x3FFu;
            break;
        default:
            break;
        }

        return true;
    }

    if (address >= 0x10008000 && address < 0x1000F000)
    {
        if ((address & 0xFF) == 0x00 && (value & 0x100))
        {
            const auto dctrlIt = m_ioRegisters.find(0x1000E000u);
            const bool dmacEnabled = (dctrlIt == m_ioRegisters.end()) || ((dctrlIt->second & 0x1u) != 0u);
            if (!dmacEnabled)
            {
                return true;
            }

            const uint32_t channelBase = address & 0xFFFFFF00;
            const uint32_t madr = m_ioRegisters[channelBase + 0x10];
            const uint32_t qwc = m_ioRegisters[channelBase + 0x20];
            m_dmaStartCount.fetch_add(1, std::memory_order_relaxed);
            // PATH3 goes through the GIF arbiter that the VIF1 worker feeds.
            if (channelBase == 0x1000A000u)
            {
#if !defined(PS2X_GAME_GT3)
                m_asyncPath3Start = false;
                if (m_asyncPath3Posted != m_asyncPath3Published)
                {
                    // An earlier posted PATH3 is not published yet (the guest
                    // restarted the channel without waiting for it): it
                    // completes first, then this start runs synchronously.
                    if (m_vif1Worker.running())
                        m_vif1Worker.notePath3Fallback();
                    quiesceVif1(Vif1Worker::QuiesceSite::GifDmaStart);
                    publishAsyncPath3Completions();
                }
                else if (asyncPath3Enabled() && m_vif1Worker.running())
                {
                    m_asyncPath3Start = true; // processPendingTransfers posts it
                }
                if (!m_asyncPath3Start)
#endif
                    quiesceVif1(Vif1Worker::QuiesceSite::GifDmaStart);
            }
            if(channelBase==0x10008000u || channelBase==0x10009000u || channelBase==0x1000a000u) {
                const uint32_t start[]={value,madr,qwc,m_ioRegisters[channelBase+0x30]};
                ps2_graphics_trace::dmaTrace(gs_regs.vsyncTick.load(), "start ch%u chcr=%08x madr=%08x qwc=%x tadr=%08x",
                                             ((channelBase >> 12) & 0xFu) - 8u, value, madr, qwc, m_ioRegisters[channelBase + 0x30]);
                ps2_graphics_trace::record(channelBase,0,reinterpret_cast<const uint8_t*>(start),sizeof(start),gs_regs.vsyncTick.load());
                if(channelBase==0x10009000u) ps2_graphics_trace::noteVif1Kick(gs_regs.vsyncTick.load());
                else if(channelBase==0x1000a000u && ps2_graphics_trace::dmaStats().enabled) { ++ps2_graphics_trace::dmaStats().gifKicks; if (m_path3Masked) ++ps2_graphics_trace::dmaStats().gifKicksMasked; }
            }

            if (channelBase == 0x10009000u && (value & 1u) == 0u && ((value >> 2) & 3u) == 0u &&
                (m_ioRegisters[0x10003C00u] & 0x00800000u) != 0u)
            {
                // CHCR.DIR 0 in normal mode with VIF1_STAT.FDR set: VIF1 ->
                // memory. GT3 reads GS memory back this way (GS BUSDIR 1, FDR,
                // TRXDIR 1) between its VIF1 and GIF chains (0x25a830); the
                // destination buffer is not VIF data. Other DIR 0 starts are
                // still treated as memory -> VIF1.
                completeVif1FromTransfer(value);
                return true;
            }

            if ((channelBase == 0x1000A000u || channelBase == 0x10009000u || channelBase == 0x10008000u) &&
                (m_gsVRAM || channelBase == 0x10008000u))
            {
                auto enqueueTransfer = [&](uint32_t srcAddr, uint32_t qwCount)
                {
                    if (qwCount == 0)
                        return;
                    const bool scratch = isScratchpad(srcAddr);
                    PendingTransfer pt;
                    pt.fromScratchpad = scratch;
                    pt.srcAddr = srcAddr;
                    pt.qwc = qwCount;
                    if (channelBase == 0x1000A000u)
                        m_pendingGifTransfers.push_back(pt);
                    else if (channelBase == 0x10009000u)
                        m_pendingVif1Transfers.push_back(pt);
                    else if (channelBase == 0x10008000u)
                        m_pendingVif0Transfers.push_back(pt);
                };

                uint32_t chcr = value;
                uint32_t mode = (chcr >> 2) & 0x3;

                if (mode == 0 && qwc > 0)
                {
                    enqueueTransfer(madr, qwc);
                }
                else if (mode == 1)
                {
                    uint32_t tagAddr = m_ioRegisters[channelBase + 0x30];
                    uint32_t asr0 = m_ioRegisters[channelBase + 0x40];
                    uint32_t asr1 = m_ioRegisters[channelBase + 0x50];
                    uint32_t asp = (chcr >> 4) & 0x3u;
                    const bool tieEnabled = (chcr & (1u << 7)) != 0u;
                    const bool transferVifTag = (chcr & (1u << 6)) != 0u &&
                        (channelBase == 0x10008000u || channelBase == 0x10009000u);
                    // Only a guard against chains that loop forever: GT3's race
                    // display list alone passes 4096 tags when the view gets busy,
                    // and a chain cut there never completes (every frame then hit
                    // the game's 120-VSync GIF timeout).
                    const int kMaxChainTags = 1 << 20;
                    const int64_t gatherStart = frameTimeline().enabled ? timelineNowUs() : 0;
                    std::vector<uint8_t> chainBuf;
                    // VIF1 chains (GT3's race: 2.6 MB a frame) take a large
                    // buffer back from the worker first, and room for the last
                    // chain's size: grown from a small pool buffer, the gather
                    // reallocated ~20 times into fresh, page-faulting memory
                    // and took 1.3-2.6 ms of the EE per frame.
                    if (channelBase == 0x10009000u && m_vif1Worker.takeSpareBuffer(chainBuf))
                    {
                        chainBuf.clear();
                    }
                    else if (!m_chainBufPool.empty())
                    {
                        chainBuf = std::move(m_chainBufPool.back());
                        m_chainBufPool.pop_back();
                        chainBuf.clear();
                    }
                    if (channelBase == 0x10009000u)
                        chainBuf.reserve(m_lastVif1ChainBytes + m_lastVif1ChainBytes / 4u);

                    auto appendData = [&](uint32_t srcAddr, uint32_t qwCount)
                    {
                        const uint64_t bytes64 = static_cast<uint64_t>(qwCount) * 16ull;
                        uint32_t bytes = (bytes64 > 0xFFFFFFFFull) ? 0xFFFFFFFFu : static_cast<uint32_t>(bytes64);
                        const bool scratch = isScratchpad(srcAddr);
                        uint32_t src = 0;
                        src = translateAddress(srcAddr);
                        const uint8_t *base2;
                        uint32_t maxSz2;
                        if (scratch)
                        {
                            base2 = m_scratchpad;
                            maxSz2 = PS2_SCRATCHPAD_SIZE;
                        }
                        else
                        {
                            base2 = m_rdram;
                            maxSz2 = PS2_RAM_SIZE;
                        }

                        while (bytes > 0)
                        {
                            if (src >= maxSz2)
                                src = 0;
                            uint32_t chunk = bytes;
                            if (src + chunk > maxSz2)
                                chunk = maxSz2 - src;
                            if (chunk == 0)
                                break;
                            chainBuf.insert(chainBuf.end(), base2 + src, base2 + src + chunk);
                            bytes -= chunk;
                            src += chunk;
                        }
                    };

                    int tagsProcessed = 0;
                    bool chainTerminated = false;
                    uint32_t lastTagUpper = (chcr >> 16) & 0xFFFFu;

                    while (tagsProcessed < kMaxChainTags)
                    {
                        const uint32_t currentTagAddr = tagAddr;
                        const bool tagInSPR = isScratchpad(tagAddr);
                        uint32_t physTag = 0;
                        try
                        {
                            physTag = translateAddress(tagAddr);
                        }
                        catch (...)
                        {
                            break;
                        }
                        const uint8_t *tagBase;
                        uint32_t tagMax;
                        if (tagInSPR)
                        {
                            tagBase = m_scratchpad;
                            tagMax = PS2_SCRATCHPAD_SIZE;
                        }
                        else
                        {
                            tagBase = m_rdram;
                            tagMax = PS2_RAM_SIZE;
                        }
                        if (physTag + 16 > tagMax)
                            break;

                        const uint8_t *tp = tagBase + physTag;
                        ps2_graphics_trace::record(channelBase+1u,currentTagAddr,tp,16u,gs_regs.vsyncTick.load());
                        uint64_t tag = loadScalar<uint64_t>(tp, 0, 16, "dma chain tag", tagAddr);
                        uint16_t tagQwc = static_cast<uint16_t>(tag & 0xFFFF);
                        uint32_t id = static_cast<uint32_t>((tag >> 28) & 0x7);
                        const bool irq = ((tag >> 31) & 0x1ull) != 0ull;
                        uint32_t addr = static_cast<uint32_t>((tag >> 32) & 0x7FFFFFFF);
                        lastTagUpper = static_cast<uint32_t>((tag >> 16) & 0xFFFFu);
                        ++tagsProcessed;

                        uint32_t dataAddr = 0;
                        bool hasPayload = (tagQwc > 0);
                        bool endChain = false;

                        switch (id)
                        {
                        case 0:
                            dataAddr = addr;
                            tagAddr = tagAddr + 16;
                            endChain = true;
                            break;
                        case 1:
                            dataAddr = tagAddr + 16;
                            tagAddr = dataAddr + static_cast<uint32_t>(tagQwc) * 16u;
                            break;
                        case 2:
                            dataAddr = tagAddr + 16;
                            tagAddr = addr;
                            break;
                        case 3:
                        case 4:
                            dataAddr = addr;
                            tagAddr = tagAddr + 16;
                            break;
                        case 5:
                            dataAddr = tagAddr + 16;
                            {
                                const uint32_t retAddr = dataAddr + static_cast<uint32_t>(tagQwc) * 16u;
                                if (asp == 0u)
                                {
                                    asr0 = retAddr;
                                    asp = 1u;
                                }
                                else if (asp == 1u)
                                {
                                    asr1 = retAddr;
                                    asp = 2u;
                                }
                            }
                            tagAddr = addr;
                            break;
                        case 6:
                            dataAddr = tagAddr + 16;
                            if (asp == 2u)
                            {
                                tagAddr = asr1;
                                asp = 1u;
                            }
                            else if (asp == 1u)
                            {
                                tagAddr = asr0;
                                asp = 0u;
                            }
                            else
                            {
                                endChain = true;
                            }
                            break;
                        case 7:
                            dataAddr = tagAddr + 16;
                            endChain = true;
                            break;
                        default:
                            hasPayload = false;
                            endChain = true;
                            break;
                        }

                        // TTE sends the upper two words of every tag to VIF, including
                        // REF/REFE/REFS and zero-QWC tags, before that tag's payload.
                        if (transferVifTag)
                            chainBuf.insert(chainBuf.end(), tp + 8u, tp + 16u);
                        if (hasPayload)
                            appendData(dataAddr, tagQwc);
                        if (irq && tieEnabled)
                            endChain = true;
                        if (endChain)
                        {
                            chainTerminated = true;
                            break;
                        }
                    }

                    if (!chainTerminated && tagsProcessed >= kMaxChainTags)
                    {
                        static uint32_t reported = 0;
                        if (reported++ < 8u)
                            std::cerr << "[dma] chain on 0x" << std::hex << channelBase << " stopped after " << std::dec << tagsProcessed
                                      << " tags without an end (looping chain?) at tadr 0x" << std::hex << tagAddr << std::dec << std::endl;
                    }
                    if (channelBase == 0x10009000u)
                        m_lastVif1ChainBytes = std::max(m_lastVif1ChainBytes, chainBuf.size());
                    if (frameTimeline().enabled && channelBase == 0x10009000u && chainBuf.size() >= frameTimeline().frameBytes)
                    {
                        frameTimeline().gatherUs.store(timelineNowUs() - gatherStart);
                        frameTimeline().gatherTags.store(tagsProcessed);
                    }
                    m_ioRegisters[channelBase + 0x30] = tagAddr;
                    m_ioRegisters[channelBase + 0x40] = asr0;
                    m_ioRegisters[channelBase + 0x50] = asr1;
                    chcr = (chcr & ~(0x3u << 4)) | ((asp & 0x3u) << 4);
                    chcr = (chcr & 0x0000FFFFu) | (lastTagUpper << 16);
                    m_ioRegisters[channelBase + 0x00] = chcr;
                    ps2_graphics_trace::dmaTrace(gs_regs.vsyncTick.load(), "chain ch%u tags=%d last=%04x terminated=%d bytes=%u next-tadr=%08x",
                                                 ((channelBase >> 12) & 0xFu) - 8u, tagsProcessed, lastTagUpper, chainTerminated ? 1 : 0,
                                                 static_cast<uint32_t>(chainBuf.size()), tagAddr);

                    // An empty terminal/IRQ tag still completes DMA. Queue an empty
                    // transfer so drain publishes D_STAT/cause and clears STR without
                    // submitting invented GIF/VIF bytes. Invalid/unterminated empty
                    // chains must not acquire a successful completion.
                    if (!chainBuf.empty() || chainTerminated)
                    {
                        PendingTransfer pt;
                        pt.fromScratchpad = false;
                        pt.srcAddr = 0;
                        pt.qwc = 0;
                        pt.chainData = std::move(chainBuf);
                        if (channelBase == 0x1000A000)
                        {
                            m_pendingGifTransfers.push_back(std::move(pt));
                        }
                        else if (channelBase == 0x10009000u)
                        {
                            m_pendingVif1Transfers.push_back(std::move(pt));
                        }
                        else if (channelBase == 0x10008000u)
                        {
                            m_pendingVif0Transfers.push_back(std::move(pt));
                        }
                    }
                    // else if (channelBase == 0x10009000u)
                    // {

                    // }
                }
                else if (qwc > 0)
                {
                    enqueueTransfer(madr, qwc);
                }

                const bool autoProcessTransfers =
                    (channelBase == 0x1000A000u) ? (m_gifPacketCallback || m_gifArbiter != nullptr) : true;
                if (autoProcessTransfers)
                {
                    processPendingTransfers();
                }
            }
#if !defined(PS2X_GAME_GT3)
            m_asyncPath3Start = false; // only this start's own processPendingTransfers may post
#endif
        }
        return true;
    }

    if (address >= 0x10000000 && address < 0x10010000)
    {
        if (address >= 0x10000200 && address < 0x10000300)
        {
            return true;
        }
        if (address >= 0x10000000 && address < 0x10000100)
        {
            return true;
        }
    }

    return false;
}

void PS2Memory::recycleTransfers(std::vector<PendingTransfer> &transfers)
{
    for (auto &p : transfers)
    {
        if (p.chainData.capacity() != 0u && m_chainBufPool.size() < 8u)
            m_chainBufPool.push_back(std::move(p.chainData));
    }
    transfers.clear();
}

void PS2Memory::processPendingTransfers()
{
    const bool hadGif = !m_pendingGifTransfers.empty();
#if !defined(PS2X_GAME_GT3)
    // Async PATH3 (asyncPath3Enabled): this GIF start's bytes go to the VIF1
    // worker, behind the VIF1 work posted before it, in the pieces the
    // synchronous path would submit (a normal-mode source split at the end of
    // RAM / scratchpad); the last piece completes the DMA. Copied now, as the
    // synchronous path reads them now.
    const bool asyncGif = hadGif && m_asyncPath3Start && m_vif1Worker.active();
    m_asyncPath3Start = false;
    if (asyncGif)
    {
        std::vector<std::vector<uint8_t>> pieces;
        for (auto &p : m_pendingGifTransfers)
        {
            if (!p.chainData.empty())
            {
                pieces.push_back(std::move(p.chainData));
                continue;
            }
            if (p.qwc == 0u)
                continue;
            const uint64_t bytes64 = static_cast<uint64_t>(p.qwc) * 16ull;
            uint32_t bytesLeft = (bytes64 > 0xFFFFFFFFull) ? 0xFFFFFFFFu : static_cast<uint32_t>(bytes64);
            uint32_t srcPhys = 0;
            try
            {
                srcPhys = translateAddress(p.srcAddr);
            }
            catch (const std::exception &)
            {
                continue;
            }
            const uint8_t *base = p.fromScratchpad ? m_scratchpad : m_rdram;
            const uint32_t limit = p.fromScratchpad ? PS2_SCRATCHPAD_SIZE : PS2_RAM_SIZE;
            while (bytesLeft >= 16)
            {
                if (srcPhys >= limit)
                    srcPhys = 0;
                uint32_t chunk = bytesLeft;
                if (srcPhys + chunk > limit)
                    chunk = limit - srcPhys;
                if (chunk == 0)
                    break;
                pieces.emplace_back(base + srcPhys, base + srcPhys + chunk);
                bytesLeft -= chunk;
                srcPhys += chunk;
            }
        }
        m_pendingGifTransfers.clear();
        if (pieces.empty())
            pieces.emplace_back(); // nothing to submit: the DMA still completes
        for (size_t i = 0; i < pieces.size(); ++i)
            m_vif1Worker.postPath3(pieces[i], i + 1u == pieces.size());
        ++m_asyncPath3Posted;
        ps2_graphics_trace::dmaTrace(gs_regs.vsyncTick.load(), "post ch2 (async PATH3, %u pieces)", static_cast<uint32_t>(pieces.size()));
    }
    const bool syncGif = hadGif && !asyncGif; // submitted and completed here
#else
    const bool syncGif = hadGif;
#endif
    if (syncGif)
        quiesceVif1(Vif1Worker::QuiesceSite::GifPending);
    for (size_t idx = 0; idx < m_pendingGifTransfers.size(); ++idx)
    {
        auto &p = m_pendingGifTransfers[idx];
        if (!p.chainData.empty())
        {
            m_seenGifCopy = true;
            m_gifCopyCount.fetch_add(1, std::memory_order_relaxed);
            submitGifPacket(GifPathId::Path3, p.chainData.data(), static_cast<uint32_t>(p.chainData.size()), false);
        }
        else if (p.qwc > 0)
        {
            const uint64_t bytes64 = static_cast<uint64_t>(p.qwc) * 16ull;
            uint32_t sizeBytes = (bytes64 > 0xFFFFFFFFull) ? 0xFFFFFFFFu : static_cast<uint32_t>(bytes64);
            uint32_t srcPhys = 0;
            try
            {
                srcPhys = translateAddress(p.srcAddr);
            }
            catch (const std::exception &)
            {
                continue;
            }
            if (p.fromScratchpad)
            {
                uint32_t bytesLeft = sizeBytes;
                while (bytesLeft >= 16)
                {
                    if (srcPhys >= PS2_SCRATCHPAD_SIZE)
                        srcPhys = 0;
                    uint32_t chunk = bytesLeft;
                    if (srcPhys + chunk > PS2_SCRATCHPAD_SIZE)
                        chunk = PS2_SCRATCHPAD_SIZE - srcPhys;
                    if (chunk == 0)
                        break;
                    m_seenGifCopy = true;
                    m_gifCopyCount.fetch_add(1, std::memory_order_relaxed);
                    submitGifPacket(GifPathId::Path3, m_scratchpad + srcPhys, chunk, false);
                    bytesLeft -= chunk;
                    srcPhys += chunk;
                }
            }
            else
            {
                uint32_t bytesLeft = sizeBytes;
                while (bytesLeft >= 16)
                {
                    if (srcPhys >= PS2_RAM_SIZE)
                        srcPhys = 0;
                    uint32_t chunk = bytesLeft;
                    if (srcPhys + chunk > PS2_RAM_SIZE)
                        chunk = PS2_RAM_SIZE - srcPhys;
                    if (chunk == 0)
                        break;
                    m_seenGifCopy = true;
                    m_gifCopyCount.fetch_add(1, std::memory_order_relaxed);
                    submitGifPacket(GifPathId::Path3, m_rdram + srcPhys, chunk, false);
                    bytesLeft -= chunk;
                    srcPhys += chunk;
                }
            }
        }
    }
    recycleTransfers(m_pendingGifTransfers);
    // The GIF DMA only completes (and raises its tag IRQ) once its PATH3 data
    // has gone through the GIF. Drain it now, before VIF processing queues
    // PATH1/PATH2 packets: drain() orders by path priority, so later VU1
    // draws would otherwise overtake these uploads. GT3's race loads CLUTs
    // (TEX2 CLD) right after a GIF IRQ that says the texture/CLUT upload is
    // done; reordered, every palette was read before its upload landed.
    if (syncGif && m_gifArbiter)
        m_gifArbiter->drain();

    const bool hadVif0 = !m_pendingVif0Transfers.empty();
    for (auto &p : m_pendingVif0Transfers)
    {
        if (!p.chainData.empty())
        {
            processVIF0Data(p.chainData.data(), static_cast<uint32_t>(p.chainData.size()));
        }
        else if (p.qwc > 0)
        {
            uint32_t srcPhys = 0;
            const uint64_t bytes64 = static_cast<uint64_t>(p.qwc) * 16ull;
            uint32_t sizeBytes = (bytes64 > 0xFFFFFFFFull) ? 0xFFFFFFFFu : static_cast<uint32_t>(bytes64);
            try
            {
                srcPhys = translateAddress(p.srcAddr);
            }
            catch (const std::exception &)
            {
                continue;
            }
            if (p.fromScratchpad)
            {
                uint32_t bytesLeft = sizeBytes;
                while (bytesLeft > 0)
                {
                    if (srcPhys >= PS2_SCRATCHPAD_SIZE)
                        srcPhys = 0;
                    uint32_t chunk = bytesLeft;
                    if (srcPhys + chunk > PS2_SCRATCHPAD_SIZE)
                        chunk = PS2_SCRATCHPAD_SIZE - srcPhys;
                    if (chunk == 0)
                        break;
                    processVIF0Data(m_scratchpad + srcPhys, chunk);
                    bytesLeft -= chunk;
                    srcPhys += chunk;
                }
            }
            else
            {
                uint32_t bytesLeft = sizeBytes;
                while (bytesLeft > 0)
                {
                    if (srcPhys >= PS2_RAM_SIZE)
                        srcPhys = 0;
                    uint32_t chunk = bytesLeft;
                    if (srcPhys + chunk > PS2_RAM_SIZE)
                        chunk = PS2_RAM_SIZE - srcPhys;
                    if (chunk == 0)
                        break;
                    processVIF0Data(srcPhys, chunk);
                    bytesLeft -= chunk;
                    srcPhys += chunk;
                }
            }
        }
    }
    recycleTransfers(m_pendingVif0Transfers);

    const bool hadVif1 = !m_pendingVif1Transfers.empty();
    const bool vif1Worker = m_vif1Worker.active();
    if (vif1Worker && hadVif1)
    {
        // The worker runs VIF1 from its own copy of each DMA's bytes: a
        // chain's gathered data, or a copy of a normal-mode source. The last
        // post completes the DMA once the worker has run it.
        const uint32_t fbrst = m_vu0FbrstReader ? m_vu0FbrstReader() : 0u;
        for (size_t i = 0; i < m_pendingVif1Transfers.size(); ++i)
        {
            auto &p = m_pendingVif1Transfers[i];
            std::vector<uint8_t> bytes = std::move(p.chainData);
            if (bytes.empty() && p.qwc > 0)
            {
                try
                {
                    uint32_t src = translateAddress(p.srcAddr);
                    const uint8_t *base = p.fromScratchpad ? m_scratchpad : m_rdram;
                    const uint32_t limit = p.fromScratchpad ? PS2_SCRATCHPAD_SIZE : PS2_RAM_SIZE;
                    uint64_t left = static_cast<uint64_t>(p.qwc) * 16ull;
                    while (left > 0)
                    {
                        if (src >= limit)
                            src = 0;
                        const uint32_t chunk = static_cast<uint32_t>(std::min<uint64_t>(left, limit - src));
                        bytes.insert(bytes.end(), base + src, base + src + chunk);
                        left -= chunk;
                        src += chunk;
                    }
                }
                catch (const std::exception &)
                {
                }
            }
            const bool last = i + 1u == m_pendingVif1Transfers.size();
            if (frameTimeline().enabled)
            {
                FrameTimeline &tl = frameTimeline();
                const int64_t now = timelineNowUs();
                if (bytes.size() >= tl.frameBytes)
                {
                    if (tl.lastKick != 0)
                    {
                        const int64_t k = tl.kick.load();
                        auto rel = [&](const std::atomic<int64_t> &t) { const int64_t v = t.load(); return v >= k ? (v - k) / 1000.0 : -1.0; };
                        std::fprintf(stderr, "[timeline] period %.2f worker-done %.2f sync %.2f-%.2f kick2 %.2f done2 %.2f ms chain %zu KB\n",
                                     (now - tl.lastKick) / 1000.0, rel(tl.workerDone), rel(tl.syncStart), rel(tl.syncEnd), rel(tl.kick2),
                                     rel(tl.workerDone2), bytes.size() >> 10u);
                        std::fprintf(stderr, "[timeline]   gather %lld us, %lld tags\n", static_cast<long long>(tl.gatherUs.load()),
                                     static_cast<long long>(tl.gatherTags.load()));
                    }
                    tl.lastKick = now;
                    tl.kick.store(now);
                }
                else
                    tl.kick2.store(now);
            }
            if (!bytes.empty() || last)
                m_vif1Worker.postData(bytes, last, fbrst);
        }
        m_pendingVif1Transfers.clear();
    }
    for (auto &p : m_pendingVif1Transfers)
    {
        if (!p.chainData.empty())
        {
            processVIF1Data(p.chainData.data(), static_cast<uint32_t>(p.chainData.size()));
        }
        else if (p.qwc > 0)
        {
            uint32_t srcPhys = 0;
            const uint64_t bytes64 = static_cast<uint64_t>(p.qwc) * 16ull;
            uint32_t sizeBytes = (bytes64 > 0xFFFFFFFFull) ? 0xFFFFFFFFu : static_cast<uint32_t>(bytes64);
            try
            {
                srcPhys = translateAddress(p.srcAddr);
            }
            catch (const std::exception &)
            {
                continue;
            }
            if (p.fromScratchpad)
            {
                uint32_t bytesLeft = sizeBytes;
                while (bytesLeft > 0)
                {
                    if (srcPhys >= PS2_SCRATCHPAD_SIZE)
                        srcPhys = 0;
                    uint32_t chunk = bytesLeft;
                    if (srcPhys + chunk > PS2_SCRATCHPAD_SIZE)
                        chunk = PS2_SCRATCHPAD_SIZE - srcPhys;
                    if (chunk == 0)
                        break;
                    processVIF1Data(m_scratchpad + srcPhys, chunk);
                    bytesLeft -= chunk;
                    srcPhys += chunk;
                }
            }
            else
            {
                uint32_t bytesLeft = sizeBytes;
                while (bytesLeft > 0)
                {
                    if (srcPhys >= PS2_RAM_SIZE)
                        srcPhys = 0;
                    uint32_t chunk = bytesLeft;
                    if (srcPhys + chunk > PS2_RAM_SIZE)
                        chunk = PS2_RAM_SIZE - srcPhys;
                    if (chunk == 0)
                        break;
                    processVIF1Data(srcPhys, chunk);
                    bytesLeft -= chunk;
                    srcPhys += chunk;
                }
            }
        }
    }
    recycleTransfers(m_pendingVif1Transfers);

    // With the VIF1 worker, it drains its own PATH1/PATH2 packets (and a GIF
    // DMA was drained above, after the worker parked).
    if (m_gifArbiter && !vif1Worker)
        m_gifArbiter->drain();

    static constexpr uint32_t GIF_CHANNEL = 0x1000A000;
    static constexpr uint32_t D_STAT = 0x1000E010u;

    auto raiseDStatChannel = [&](uint32_t channelBit)
    {
        uint32_t dstat = m_ioRegisters.count(D_STAT) ? m_ioRegisters[D_STAT] : 0u;
        dstat |= (1u << channelBit);

        const uint32_t status = dstat & 0x3FFu;
        const uint32_t mask = (dstat >> 16) & 0x3FFu;
        if ((status & mask) != 0u)
            dstat |= (1u << 31);
        else
            dstat &= ~(1u << 31);

        m_ioRegisters[D_STAT] = dstat;
    };

    if (syncGif)
    {
        ps2_graphics_trace::dmaTrace(gs_regs.vsyncTick.load(), "done ch2");
        raiseDStatChannel(2u); // GIF channel
        queueCompletedDmacCause(2u);
        if (ps2_graphics_trace::dmaStats().enabled) ++ps2_graphics_trace::dmaStats().gifDone;
        m_ioRegisters[GIF_CHANNEL + 0x00] &= ~0x100u;
        m_ioRegisters[GIF_CHANNEL + 0x20] = 0;
    }
    if (hadVif0)
    {
        m_vifDeferredDmac[0] = true;
        completeVifDmaIfReady(0u);
    }
    if (hadVif1 && !vif1Worker)
    {
        m_vifDeferredDmac[1] = true;
        completeVifDmaIfReady(1u);
    }
}

bool PS2Memory::completeVif1FromTransfer(uint32_t chcr)
{
    constexpr uint32_t base = 0x10009000u;
    // GT4, GT3_GS_READBACK_LOG (any value but 0): every transfer's [dma] line
    // below (not only the first 8) with the EE's whole wait in this function,
    // split into the VIF1 worker quiesce, the GIF arbiter drain and the GS read
    // (GS worker sync + readback), and a [gs-download] summary (count, bytes,
    // total and longest wait, the three parts) at the first transfer 10 s or
    // more after the last summary (analysis/phase5/specs/step4-hitches.md D1).
    // GT3 builds: off, GT3_GS_READBACK_LOG keeps its GT3 meaning there.
#if defined(PS2X_GAME_GT3)
    constexpr bool downloadLog = false;
#else
    static const bool downloadLog = [] {
        const char *v = std::getenv("GT3_GS_READBACK_LOG");
        return v && *v && *v != '0';
    }();
#endif
    using WaitClock = std::chrono::steady_clock;
    const WaitClock::time_point tEntry = downloadLog ? WaitClock::now() : WaitClock::time_point{};
    quiesceVif1(Vif1Worker::QuiesceSite::Vif1DmaEnd); // the VIF1 worker's packets reach the GS first
    const WaitClock::time_point tQuiesced = downloadLog ? WaitClock::now() : WaitClock::time_point{};
    uint32_t madr = m_ioRegisters[base + 0x10];
    const uint32_t qwc = m_ioRegisters[base + 0x20];
    const uint32_t mode = (chcr >> 2) & 3u;
    const uint32_t bytes = qwc * 16u;

    // The GS path data (GIF/VIF packets that set TRXDIR 1) must have reached
    // the GS before its local->host bytes are read.
    if (m_gifArbiter)
        m_gifArbiter->drain();
    const WaitClock::time_point tDrained = downloadLog ? WaitClock::now() : WaitClock::time_point{};
    std::vector<uint8_t> data(bytes, 0u);
    if (frameTimeline().enabled)
        frameTimeline().syncStart.store(timelineNowUs());
    // GT3_READBACK_EVERY=<n> (diagnostics, not exact): only every n-th read of
    // the same address and size goes to the GS; the others get the last
    // bytes read, without waiting for the renderer and the GPU. Measures what
    // the per-frame read back costs.
    static const uint32_t readbackEvery = [] {
        const char *v = std::getenv("GT3_READBACK_EVERY");
        return (v && *v) ? static_cast<uint32_t>(std::strtoul(v, nullptr, 10)) : 0u;
    }();
    static uint32_t cachedMadr = 0, cachedCount = 0;
    static std::vector<uint8_t> cachedData;
    uint32_t got = 0;
    double waitedMs = 0.0, quiesceMs = 0.0, drainMs = 0.0, readMs = 0.0;
    if (readbackEvery > 1u && cachedMadr == madr && cachedData.size() == bytes && (++cachedCount % readbackEvery) != 0u)
    {
        std::memcpy(data.data(), cachedData.data(), bytes);
        got = bytes;
    }
    else
    {
        got = (bytes != 0u && m_localToHostReader) ? m_localToHostReader(data.data(), bytes) : 0u;
        if (readbackEvery > 1u)
        {
            cachedMadr = madr;
            cachedData = data;
        }
    }
    if (downloadLog)
    {
        static auto windowStart = tEntry;
        static uint64_t transfers = 0, totalBytes = 0;
        static double totalMs = 0.0, maxMs = 0.0, totalQuiesceMs = 0.0, totalDrainMs = 0.0, totalReadMs = 0.0;
        const auto now = WaitClock::now();
        using Ms = std::chrono::duration<double, std::milli>;
        quiesceMs = Ms(tQuiesced - tEntry).count();
        drainMs = Ms(tDrained - tQuiesced).count();
        readMs = Ms(now - tDrained).count();
        waitedMs = Ms(now - tEntry).count();
        ++transfers;
        totalBytes += bytes;
        totalMs += waitedMs;
        maxMs = std::max(maxMs, waitedMs);
        totalQuiesceMs += quiesceMs;
        totalDrainMs += drainMs;
        totalReadMs += readMs;
        const double window = std::chrono::duration<double>(now - windowStart).count();
        if (window >= 10.0)
        {
            std::fprintf(stderr,
                         "[gs-download] %.1f s to vsync %llu: %llu GS->EE transfers, %llu bytes, EE waited %.1f ms (longest %.1f ms; "
                         "VIF1 quiesce %.1f, GIF drain %.1f, GS read %.1f)\n",
                         window, static_cast<unsigned long long>(gs().vsyncTick.load(std::memory_order_relaxed)),
                         static_cast<unsigned long long>(transfers), static_cast<unsigned long long>(totalBytes), totalMs, maxMs,
                         totalQuiesceMs, totalDrainMs, totalReadMs);
            windowStart = now;
            transfers = totalBytes = 0;
            totalMs = maxMs = totalQuiesceMs = totalDrainMs = totalReadMs = 0.0;
        }
    }
    if (frameTimeline().enabled)
        frameTimeline().syncEnd.store(timelineNowUs());
    // GT3_READBACK_LOG=1: every 60th read back, a summary of the words read.
    static const bool readbackLog = std::getenv("GT3_READBACK_LOG") != nullptr;
    static uint32_t readbacks = 0;
    if (readbackLog && (readbacks++ % 60u) == 0u && bytes >= 64u)
    {
        uint32_t nonzero = 0, first[8] = {};
        std::map<uint32_t, uint32_t> values;
        for (uint32_t i = 0; i < bytes / 4u; ++i)
        {
            uint32_t w;
            std::memcpy(&w, data.data() + i * 4u, 4);
            nonzero += w != 0u;
            if (values.size() < 16u || values.count(w))
                ++values[w];
        }
        std::memcpy(first, data.data(), sizeof(first));
        std::fprintf(stderr, "[readback] #%u madr=%x bytes=%u nonzero=%u first %08x %08x %08x %08x %08x %08x %08x %08x values:",
                     readbacks, madr, bytes, nonzero, first[0], first[1], first[2], first[3], first[4], first[5], first[6], first[7]);
        for (const auto &[value, count] : values)
            std::fprintf(stderr, " %08x*%u", value, count);
        std::fprintf(stderr, "\n");
    }

    static uint32_t reported = 0;
    if (reported < 8u || mode != 0u || got != bytes || downloadLog)
    {
        if (reported < 64u || downloadLog)
        {
            ++reported;
#if !defined(PS2X_GAME_GT3)
            // GT4: the same line formatted first and written in one call
            // (analysis/phase5/specs/step6-loading-frame.md F1). std::cerr wrote
            // it one character per WriteFile (stderr unbuffered, redirected to
            // a file): ~2.5-3 ms per line on the EE thread, inside GT4's
            // GS/DMAC interrupt handler; transfers 3-8 of every run are steps
            // 1-6 of the loading frame's readback chain (15-17 ms of it).
            // PS2X_LOG_ONE_WRITE=0 restores the std::cerr form (A/B only).
            static const bool oneWrite = [] {
                const char *v = std::getenv("PS2X_LOG_ONE_WRITE");
                return !(v && *v == '0');
            }();
            if (oneWrite)
            {
                char line[384];
                int n = std::snprintf(line, sizeof(line), "[dma] VIF1 from-transfer (GS local->host): madr=0x%x qwc=0x%x chcr=0x%x mode=%u bytes %u/%u%s",
                                      madr, qwc, chcr, mode, got, bytes, mode != 0u ? " (only normal mode is modelled)" : "");
                if (n > 0 && downloadLog && static_cast<size_t>(n) < sizeof(line))
                {
                    // (%g: std::ostream's default floating-point format)
                    const int more = std::snprintf(line + n, sizeof(line) - static_cast<size_t>(n),
                                                   " waited %g ms (VIF1 quiesce %g, GIF drain %g, GS read %g) at vsync %llu", waitedMs, quiesceMs,
                                                   drainMs, readMs, static_cast<unsigned long long>(gs().vsyncTick.load(std::memory_order_relaxed)));
                    if (more > 0)
                        n += more;
                }
                if (n > 0)
                {
                    n = std::min<int>(n, static_cast<int>(sizeof(line)) - 2);
                    line[n] = '\n';
                    line[n + 1] = '\0';
                    std::fputs(line, stderr);
                }
            }
            else
#endif
            {
            std::cerr << "[dma] VIF1 from-transfer (GS local->host): madr=0x" << std::hex << madr << " qwc=0x" << qwc
                      << " chcr=0x" << chcr << std::dec << " mode=" << mode << " bytes " << got << "/" << bytes
                      << (mode != 0u ? " (only normal mode is modelled)" : "");
            if (downloadLog)
                std::cerr << " waited " << waitedMs << " ms (VIF1 quiesce " << quiesceMs << ", GIF drain " << drainMs << ", GS read "
                          << readMs << ") at vsync " << gs().vsyncTick.load(std::memory_order_relaxed);
            std::cerr << std::endl;
            }
        }
    }

    const bool spr = (madr & 0x80000000u) != 0u;
    uint8_t *dst = spr ? m_scratchpad : m_rdram;
    const uint32_t limit = spr ? PS2_SCRATCHPAD_SIZE : PS2_RAM_SIZE;
    uint32_t addr = spr ? (madr & (PS2_SCRATCHPAD_SIZE - 1u)) : (madr & (PS2_RAM_SIZE - 1u));
    for (uint32_t done = 0; done < bytes;)
    {
        const uint32_t chunk = std::min(bytes - done, limit - addr);
        std::memcpy(dst + addr, data.data() + done, chunk);
        if (!spr)
            markModified(addr, chunk);
        done += chunk;
        addr = (addr + chunk) % limit;
    }

    m_ioRegisters[base + 0x10] = madr + bytes;
    m_ioRegisters[base + 0x20] = 0u;
    m_ioRegisters[base + 0x00] = chcr & ~0x100u;
    uint32_t &status = m_ioRegisters[0x1000E010u];
    status |= 1u << 1;
    if ((status & (status >> 16) & 0x3FFu) != 0u)
        status |= 1u << 31;
    else
        status &= ~(1u << 31);
    queueCompletedDmacCause(1u);
    return got == bytes;
}

void PS2Memory::completeVifDmaIfReady(uint32_t channel)
{
    // Waiting for the remainder of a split command does not hold the DMA:
    // those bytes have been accepted, and another DMA may supply its payload.
    // An interrupt/stop stall instead holds unprocessed following commands.
    const auto &regs = channel == 0u ? vif0_regs : vif1_regs;
    if (!m_vifDeferredDmac[channel])
        return;
    if (!m_vifInput[channel].empty() && (regs.stat & 0x700u) != 0u)
    {
        ps2_graphics_trace::dmaTrace(gs_regs.vsyncTick.load(), "hold ch%u (VIF stalled, stat=%x, %u bytes held)", channel, regs.stat,
                                     static_cast<uint32_t>(m_vifInput[channel].size()));
        return;
    }
    m_vifDeferredDmac[channel] = false;
    finishVifDma(channel);
}

void PS2Memory::finishVifDma(uint32_t channel)
{
    ps2_graphics_trace::dmaTrace(gs_regs.vsyncTick.load(), "done ch%u", channel);
    uint32_t &status = m_ioRegisters[0x1000E010u];
    status |= 1u << channel;
    if ((status & (status >> 16) & 0x3FFu) != 0u) status |= 1u << 31;
    else status &= ~(1u << 31);
    m_ioRegisters[0x10008000u + channel * 0x1000u] &= ~0x100u;
    m_ioRegisters[0x10008020u + channel * 0x1000u] = 0u;
    queueCompletedDmacCause(channel);
    if (channel == 1u && ps2_graphics_trace::dmaStats().enabled) ++ps2_graphics_trace::dmaStats().vif1Done;
}

#if !defined(PS2X_GAME_GT3)
void PS2Memory::publishAsyncPath3Completions()
{
    static constexpr uint32_t GIF_CHANNEL = 0x1000A000u;
    static constexpr uint32_t D_STAT = 0x1000E010u;
    for (uint32_t n = m_vif1Worker.takePath3Completions(); n != 0u; --n)
    {
        ++m_asyncPath3Published;
        ps2_graphics_trace::dmaTrace(gs_regs.vsyncTick.load(), "done ch2 (async PATH3)");
        uint32_t &dstat = m_ioRegisters[D_STAT];
        dstat |= 1u << 2u;
        if ((dstat & (dstat >> 16) & 0x3FFu) != 0u)
            dstat |= 1u << 31;
        else
            dstat &= ~(1u << 31);
        queueCompletedDmacCause(2u);
        if (ps2_graphics_trace::dmaStats().enabled)
            ++ps2_graphics_trace::dmaStats().gifDone;
        m_ioRegisters[GIF_CHANNEL + 0x00] &= ~0x100u;
        m_ioRegisters[GIF_CHANNEL + 0x20] = 0;
    }
}
#endif

void PS2Memory::vif1WorkerPath3(const std::vector<uint8_t> &bytes)
{
    // As processPendingTransfers' synchronous GIF path, on the worker (which
    // owns the GIF arbiter and PATH3 masking while it runs).
    if (bytes.size() >= 16u)
    {
        m_seenGifCopy = true;
        m_gifCopyCount.fetch_add(1, std::memory_order_relaxed);
        submitGifPacket(GifPathId::Path3, bytes.data(), static_cast<uint32_t>(bytes.size()), false);
    }
    if (m_gifArbiter)
        m_gifArbiter->drain();
}

void PS2Memory::serviceVif1Worker()
{
    if (!m_vif1Worker.active())
        return;
#if !defined(PS2X_GAME_GT3)
    // Posted PATH3 transfers first: the synchronous start published each
    // before any VIF1 DMA completion its quiesce had let finish.
    if (m_asyncPath3Posted != m_asyncPath3Published)
        publishAsyncPath3Completions();
#endif
    for (uint32_t n = m_vif1Worker.takeDmaCompletions(); n != 0u; --n)
    {
        if (frameTimeline().enabled)
        {
            FrameTimeline &tl = frameTimeline();
            // The first completion after a kick is the main chain's; later the second's.
            (tl.kick2.load() > tl.kick.load() ? tl.workerDone2 : tl.workerDone).store(timelineNowUs());
        }
        finishVifDma(1u);
    }
}

std::string PS2Memory::describeVif1()
{
    char line[320];
    const auto d = [this](uint32_t a) { const auto it = m_ioRegisters.find(a); return it == m_ioRegisters.end() ? 0u : it->second; };
    std::snprintf(line, sizeof(line),
                  "stat=%x code=%08x err=%x held=%zu offset=%zu deferred=%d pending-vif-irq=%x d_stat=%08x ch1_chcr=%08x ch2_chcr=%08x %s",
                  vif1_regs.stat, vif1_regs.code, vif1_regs.err, m_vifInput[1].size(), m_vifInputOffset[1],
                  m_vifDeferredDmac[1] ? 1 : 0, m_pendingVifInterrupts.load(), d(0x1000E010u), d(0x10009000u), d(0x1000A000u),
                  m_vif1Worker.describe().c_str());
    return line;
}

void PS2Memory::startVif1Worker(Vif1Worker::WakeFn wakeEe, std::function<void()> threadInit,
                                std::function<uint32_t()> vu0FbrstReader)
{
    stopVif1Worker();
    m_vu0FbrstReader = std::move(vu0FbrstReader);
    m_vif1Worker.start(*this, std::move(wakeEe), std::move(threadInit));
}

void PS2Memory::stopVif1Worker()
{
    if (!m_vif1Worker.active())
        return;
    m_vif1Worker.quiesce();
    m_vif1Worker.stop();
    m_vu0FbrstReader = {};
}

void PS2Memory::queueCompletedDmacCause(uint32_t cause)
{
    std::lock_guard<std::mutex> lock(m_completedDmacMutex);
    m_completedDmacCauses.push_back(cause);
}

std::vector<uint32_t> PS2Memory::consumeCompletedDmacCauses()
{
    std::lock_guard<std::mutex> lock(m_completedDmacMutex);
    std::vector<uint32_t> causes;
    causes.swap(m_completedDmacCauses);
    return causes;
}

void PS2Memory::flushMaskedPath3Packets(bool drainImmediately)
{
    if (m_path3Masked || m_path3MaskedFifo.empty())
        return;

    auto emit = [&](const uint8_t *packetData, uint32_t packetSize)
    {
        if (m_gifArbiter)
            m_gifArbiter->submit(GifPathId::Path3, packetData, packetSize, false);
        else if (m_gifPacketCallback)
            m_gifPacketCallback(packetData, packetSize);
    };

    for (const auto &packet : m_path3MaskedFifo)
    {
        if (packet.size() >= 16u)
            emit(packet.data(), static_cast<uint32_t>(packet.size()));
    }
    m_path3MaskedFifo.clear();

    if (m_gifArbiter && drainImmediately)
        m_gifArbiter->drain();
}

void PS2Memory::submitGifPacket(GifPathId pathId, const uint8_t *data, uint32_t sizeBytes, bool drainImmediately, bool path2DirectHl)
{
    ps2_graphics_trace::record(0x20u+static_cast<uint32_t>(pathId),0,data,sizeBytes,gs_regs.vsyncTick.load());
    if (!data || sizeBytes < 16)
        return;

    if (pathId == GifPathId::Path3)
    {
        if (m_path3Masked)
        {
            m_path3MaskedFifo.emplace_back(data, data + sizeBytes);
            return;
        }
        flushMaskedPath3Packets(false);
    }

    if (m_gifArbiter)
        m_gifArbiter->submit(pathId, data, sizeBytes, path2DirectHl);
    else if (m_gifPacketCallback)
        m_gifPacketCallback(data, sizeBytes);

    if (m_gifArbiter && drainImmediately)
        m_gifArbiter->drain();
}

void PS2Memory::processGIFPacket(uint32_t srcPhysAddr, uint32_t qwCount)
{
    if (!m_rdram || qwCount == 0)
        return;
    quiesceVif1(Vif1Worker::QuiesceSite::GifPacketRam);
    const uint64_t bytes64 = static_cast<uint64_t>(qwCount) * 16ull;
    uint32_t sizeBytes = (bytes64 > 0xFFFFFFFFull) ? 0xFFFFFFFFu : static_cast<uint32_t>(bytes64);
    uint32_t bytesLeft = sizeBytes;
    while (bytesLeft >= 16)
    {
        if (srcPhysAddr >= PS2_RAM_SIZE)
            srcPhysAddr = 0;
        uint32_t chunk = bytesLeft;
        if (srcPhysAddr + chunk > PS2_RAM_SIZE)
            chunk = PS2_RAM_SIZE - srcPhysAddr;
        if (chunk == 0)
            break;

        m_seenGifCopy = true;
        m_gifCopyCount.fetch_add(1, std::memory_order_relaxed);
        submitGifPacket(GifPathId::Path3, m_rdram + srcPhysAddr, chunk);

        bytesLeft -= chunk;
        srcPhysAddr += chunk;
    }
}

void PS2Memory::processGIFPacket(const uint8_t *data, uint32_t sizeBytes)
{
    quiesceVif1(Vif1Worker::QuiesceSite::GifPacket);
    if (m_gifArbiter)
        submitGifPacket(GifPathId::Path3, data, sizeBytes);
    else if (m_gifPacketCallback && data && sizeBytes >= 16)
        m_gifPacketCallback(data, sizeBytes);
}

bool PS2Memory::tryProcessNativeGifImageUploadChain(GS &gs, uint32_t tadr, uint32_t chcr)
{
    quiesceVif1(Vif1Worker::QuiesceSite::GifImageChain);
    static constexpr uint32_t GIF_CHANNEL = 0x1000A000u;
    static constexpr uint32_t D_STAT = 0x1000E010u;
    static constexpr uint32_t D_CTRL = 0x1000E000u;

    if (!m_rdram || !m_gsVRAM || m_path3Masked)
        return false;
    if (m_gifArbiter && !m_gifArbiter->empty())
        return false;
    if ((chcr & 0x100u) == 0u || ((chcr >> 2u) & 0x3u) != 1u)
        return false;
    if ((chcr & (1u << 7u)) != 0u || ((chcr >> 4u) & 0x3u) != 0u)
        return false;

    const auto dctrlIt = m_ioRegisters.find(D_CTRL);
    if (dctrlIt != m_ioRegisters.end() && ((dctrlIt->second & 0x1u) == 0u))
        return false;

    auto resolveContiguous = [&](uint32_t guestAddr, uint32_t bytes, const uint8_t *&out) -> bool
    {
        try
        {
            const bool scratch = isScratchpad(guestAddr);
            const uint32_t phys = translateAddress(guestAddr);
            const uint8_t *base = scratch ? m_scratchpad : m_rdram;
            const uint32_t limit = scratch ? PS2_SCRATCHPAD_SIZE : PS2_RAM_SIZE;
            if (!base || phys > limit || bytes > limit - phys)
                return false;
            out = base + phys;
            return true;
        }
        catch (const std::exception &)
        {
            return false;
        }
    };

    auto loadDmaTagAt = [&](uint32_t guestAddr, DmaTagView &out) -> bool
    {
        const uint8_t *ptr = nullptr;
        if (!resolveContiguous(guestAddr, 16u, ptr))
            return false;
        out = decodeDmaTag(loadScalar<uint64_t>(ptr, 0u, 16u, "native gif dma tag", guestAddr));
        return true;
    };

    auto decodeSetupPayload = [&](const uint8_t *payload, uint64_t (&regs)[4]) -> bool
    {
        const uint64_t tagLo = loadScalar<uint64_t>(payload, 0u, 80u, "native gif setup tag", 0u);
        const uint64_t tagHi = loadScalar<uint64_t>(payload, 8u, 80u, "native gif setup regs", 0u);
        if (gifTagNloop(tagLo) != 4u ||
            gifTagFlg(tagLo) != GIF_FMT_PACKED ||
            gifTagNreg(tagLo) != 1u ||
            (tagHi & 0xFull) != 0x0Eull)
        {
            return false;
        }

        static constexpr uint8_t kExpectedRegs[4] = {
            GS_REG_BITBLTBUF,
            GS_REG_TRXPOS,
            GS_REG_TRXREG,
            GS_REG_TRXDIR,
        };

        uint32_t offset = 16u;
        for (uint32_t i = 0; i < 4u; ++i)
        {
            regs[i] = loadScalar<uint64_t>(payload, offset, 80u, "native gif setup value", 0u);
            const uint64_t reg = loadScalar<uint64_t>(payload, offset + 8u, 80u, "native gif setup register", 0u);
            if ((reg & 0xFFu) != kExpectedRegs[i])
                return false;
            offset += 16u;
        }

        const uint32_t trxdirMode = static_cast<uint32_t>(regs[3] & 0x3ull);
        const uint32_t rrw = static_cast<uint32_t>(regs[2] & 0xFFFull);
        const uint32_t rrh = static_cast<uint32_t>((regs[2] >> 32u) & 0xFFFull);
        return trxdirMode == 0u && rrw != 0u && rrh != 0u;
    };

    DmaTagView setupTag{};
    if (!loadDmaTagAt(tadr, setupTag) ||
        setupTag.id != 1u ||
        setupTag.qwc != 5u ||
        setupTag.irq)
    {
        return false;
    }

    const uint8_t *setupPayload = nullptr;
    const uint32_t setupPayloadAddr = tadr + 16u;
    if (!resolveContiguous(setupPayloadAddr, 5u * 16u, setupPayload))
        return false;

    uint64_t setupRegs[4] = {};
    if (!decodeSetupPayload(setupPayload, setupRegs))
        return false;

    uint32_t imageTagDmaAddr = setupPayloadAddr + 5u * 16u;
    DmaTagView imageTagDma{};
    if (!loadDmaTagAt(imageTagDmaAddr, imageTagDma) ||
        imageTagDma.id != 1u ||
        imageTagDma.qwc != 1u ||
        imageTagDma.irq)
    {
        return false;
    }

    const uint8_t *imageGifTag = nullptr;
    if (!resolveContiguous(imageTagDmaAddr + 16u, 16u, imageGifTag))
        return false;

    const uint64_t imageTagLo = loadScalar<uint64_t>(imageGifTag, 0u, 16u, "native gif image tag", imageTagDmaAddr + 16u);
    if (gifTagFlg(imageTagLo) != GIF_FMT_IMAGE)
        return false;

    const uint32_t imageQwc = gifTagNloop(imageTagLo);
    if (imageQwc == 0u)
        return false;

    const uint64_t imageBytes64 = static_cast<uint64_t>(imageQwc) * 16ull;
    if (imageBytes64 > 0xFFFFFFFFull)
        return false;
    const uint32_t imageBytes = static_cast<uint32_t>(imageBytes64);

    const uint32_t payloadTagAddr = imageTagDmaAddr + 32u;
    DmaTagView payloadTag{};
    if (!loadDmaTagAt(payloadTagAddr, payloadTag) ||
        payloadTag.qwc != imageQwc ||
        payloadTag.irq)
    {
        return false;
    }

    uint32_t imageDataAddr = 0u;
    uint32_t finalTadr = payloadTagAddr;
    uint32_t lastTagUpper = payloadTag.upper;
    if (payloadTag.id == 3u || payloadTag.id == 4u)
    {
        imageDataAddr = payloadTag.addr;
        const uint32_t terminalTagAddr = payloadTagAddr + 16u;
        DmaTagView terminalTag{};
        if (!loadDmaTagAt(terminalTagAddr, terminalTag) ||
            terminalTag.qwc != 0u ||
            terminalTag.irq ||
            (terminalTag.id != 0u && terminalTag.id != 7u))
        {
            return false;
        }
        finalTadr = (terminalTag.id == 0u) ? (terminalTagAddr + 16u) : terminalTagAddr;
        lastTagUpper = terminalTag.upper;
    }
    else if (payloadTag.id == 7u)
    {
        imageDataAddr = payloadTagAddr + 16u;
        finalTadr = payloadTagAddr;
    }
    else
    {
        return false;
    }

    const uint8_t *imageData = nullptr;
    if (!resolveContiguous(imageDataAddr, imageBytes, imageData))
        return false;

    m_dmaStartCount.fetch_add(1, std::memory_order_relaxed);
    m_seenGifCopy = true;
    m_gifCopyCount.fetch_add(1, std::memory_order_relaxed);
    gs.uploadImageNative(setupRegs[0], setupRegs[1], setupRegs[2], setupRegs[3], imageData, imageBytes);

    m_ioRegisters[GIF_CHANNEL + 0x30u] = finalTadr;
    m_ioRegisters[GIF_CHANNEL + 0x40u] = 0u;
    m_ioRegisters[GIF_CHANNEL + 0x50u] = 0u;
    m_ioRegisters[GIF_CHANNEL + 0x00u] = ((chcr & 0x0000FFFFu) | (lastTagUpper << 16u)) & ~0x100u;
    m_ioRegisters[GIF_CHANNEL + 0x20u] = 0u;

    uint32_t dstat = m_ioRegisters.count(D_STAT) ? m_ioRegisters[D_STAT] : 0u;
    dstat |= (1u << 2u);
    const uint32_t status = dstat & 0x3FFu;
    const uint32_t mask = (dstat >> 16u) & 0x3FFu;
    if ((status & mask) != 0u)
        dstat |= (1u << 31u);
    else
        dstat &= ~(1u << 31u);
    m_ioRegisters[D_STAT] = dstat;
    queueCompletedDmacCause(2u);
    return true;
}

bool PS2Memory::tryProcessNativeGifPackedChain(GS &gs, uint32_t tadr, uint32_t chcr)
{
    quiesceVif1(Vif1Worker::QuiesceSite::GifPackedChain);
    static constexpr uint32_t GIF_CHANNEL = 0x1000A000u;
    static constexpr uint32_t D_STAT = 0x1000E010u;
    static constexpr uint32_t D_CTRL = 0x1000E000u;

    if (!m_rdram || !m_gsVRAM || m_path3Masked)
        return false;
    if (m_gifArbiter && !m_gifArbiter->empty())
        return false;
    if ((chcr & 0x100u) == 0u || ((chcr >> 2u) & 0x3u) != 1u)
        return false;
    if ((chcr & (1u << 7u)) != 0u || ((chcr >> 4u) & 0x3u) != 0u)
        return false;

    const auto dctrlIt = m_ioRegisters.find(D_CTRL);
    if (dctrlIt != m_ioRegisters.end() && ((dctrlIt->second & 0x1u) == 0u))
        return false;

    auto resolveContiguous = [&](uint32_t guestAddr, uint32_t bytes, const uint8_t *&out) -> bool
    {
        try
        {
            const bool scratch = isScratchpad(guestAddr);
            const uint32_t phys = translateAddress(guestAddr);
            const uint8_t *base = scratch ? m_scratchpad : m_rdram;
            const uint32_t limit = scratch ? PS2_SCRATCHPAD_SIZE : PS2_RAM_SIZE;
            if (!base || phys > limit || bytes > limit - phys)
                return false;
            out = base + phys;
            return true;
        }
        catch (const std::exception &)
        {
            return false;
        }
    };

    const uint8_t *tagPtr = nullptr;
    if (!resolveContiguous(tadr, 16u, tagPtr))
        return false;

    const DmaTagView tag = decodeDmaTag(loadScalar<uint64_t>(tagPtr, 0u, 16u, "native packed gif dma tag", tadr));
    if (tag.id != 7u || tag.qwc == 0u || tag.irq)
        return false;

    const uint64_t payloadBytes64 = static_cast<uint64_t>(tag.qwc) * 16ull;
    if (payloadBytes64 > 0xFFFFFFFFull)
        return false;
    const uint32_t payloadBytes = static_cast<uint32_t>(payloadBytes64);

    const uint8_t *payload = nullptr;
    if (!resolveContiguous(tadr + 16u, payloadBytes, payload))
        return false;
    if (!gs.processNativePackedGIFPacket(payload, payloadBytes))
        return false;

    m_dmaStartCount.fetch_add(1, std::memory_order_relaxed);
    m_seenGifCopy = true;
    m_gifCopyCount.fetch_add(1, std::memory_order_relaxed);

    m_ioRegisters[GIF_CHANNEL + 0x30u] = tadr;
    m_ioRegisters[GIF_CHANNEL + 0x40u] = 0u;
    m_ioRegisters[GIF_CHANNEL + 0x50u] = 0u;
    m_ioRegisters[GIF_CHANNEL + 0x00u] = ((chcr & 0x0000FFFFu) | (tag.upper << 16u)) & ~0x100u;
    m_ioRegisters[GIF_CHANNEL + 0x20u] = 0u;

    uint32_t dstat = m_ioRegisters.count(D_STAT) ? m_ioRegisters[D_STAT] : 0u;
    dstat |= (1u << 2u);
    const uint32_t status = dstat & 0x3FFu;
    const uint32_t mask = (dstat >> 16u) & 0x3FFu;
    if ((status & mask) != 0u)
        dstat |= (1u << 31u);
    else
        dstat &= ~(1u << 31u);
    m_ioRegisters[D_STAT] = dstat;
    queueCompletedDmacCause(2u);
    return true;
}

int PS2Memory::pollDmaRegisters()
{
    return 0;
}

uint32_t PS2Memory::readIORegister(uint32_t address)
{
#if defined(PS2X_GAME_GT4)
    if (address >= 0x10002000u && address < 0x10002040u)
    {
        uint64_t v = 0;
        switch (address & ~7u)
        {
        case 0x10002000u: v = ipu().readCmd(); break;
        case 0x10002010u: v = ipu().readCtrl(); break;
        case 0x10002020u: v = ipu().readBp(); break;
        case 0x10002030u: v = ipu().readTop(); break;
        default: break;
        }
        ps2x::vblank::noteIpuWork(); // PS2X_IPU_VBLANK (L2n)
        return static_cast<uint32_t>((address & 4u) ? (v >> 32) : v);
    }
#endif
    if ((address >= 0x10003800u && address < 0x10003A00u) ||
        (address >= 0x10003C00u && address < 0x10003E00u))
    {
        if (((address >> 10) & 1u) != 0u)
            quiesceVif1(Vif1Worker::QuiesceSite::Vif1Read);
        const auto &regs = ((address >> 10) & 1u) == 0u ? vif0_regs : vif1_regs;
        switch (address & 0x3FFu)
        {
        case 0x00u: return regs.stat;
        case 0x10u: return 0u; // FBRST is a write-only action register.
        case 0x20u: return regs.err;
        case 0x30u: return regs.mark;
        case 0x40u: return regs.cycle;
        case 0x50u: return regs.mode;
        case 0x60u: return regs.num;
        case 0x70u: return regs.mask;
        case 0x80u: return regs.code;
        case 0x90u: return regs.itops;
        case 0xA0u: return regs.base;
        case 0xB0u: return regs.ofst;
        case 0xC0u: return regs.tops;
        case 0xD0u: return regs.itop;
        case 0xE0u: return regs.top;
        case 0x100u: case 0x110u: case 0x120u: case 0x130u:
            return regs.row[((address & 0x3FFu) - 0x100u) / 16u];
        case 0x140u: case 0x150u: case 0x160u: case 0x170u:
            return regs.col[((address & 0x3FFu) - 0x140u) / 16u];
        default: return 0u;
        }
    }
    size_t timerIndex = 0u;
    uint32_t timerOffset = 0u;
    if (decodeEeTimerRegister(address, timerIndex, timerOffset))
    {
        const EeTimer &timer = m_eeTimers[timerIndex];
        switch (timerOffset)
        {
        case kEeTimerCountOffset:
            return timer.count & 0xFFFFu;
        case kEeTimerModeOffset:
            return timer.mode & (kEeTimerModeConfigMask | kEeTimerModeStatusMask);
        case kEeTimerCompareOffset:
            return timer.compare & 0xFFFFu;
        case kEeTimerHoldOffset:
            return timer.hold & 0xFFFFu;
        default:
            return 0u;
        }
    }

    if (isGsPrivReg(address))
    {
        // NB: unreachable from read8/16/32/64 today, same reasoning as the write
        // path above; kept correct for direct callers.
        const uint32_t off = address & 7u;
        const uint32_t regOff = (address - PS2_GS_PRIV_REG_BASE) & ~0x7u;
        if (regOff == kGsCsrRegOffset)
        {
            return static_cast<uint32_t>((gs_regs.csr.load() >> (off * 8u)) & 0xFFFFFFFFull);
        }
        if (uint64_t *reg = gsRegPtr(gs_regs, address))
        {
            return static_cast<uint32_t>((*reg >> (off * 8u)) & 0xFFFFFFFFull);
        }
        return 0u;
    }

    if (address >= 0x10002000 && address <= 0x10002030)
    {
        uint32_t val = 0;
        switch (address)
        {
        case 0x10002000:
            val = m_ioRegisters[address];
            break;
        case 0x10002010:
            val = m_ioRegisters[address] & ~(1u << 31);
            break;
        case 0x10002020:
        case 0x10002030:
            val = m_ioRegisters[address];
            break;
        default:
            val = 0;
            break;
        }
        return val;
    }
    if (address >= 0x10000000 && address < 0x10010000)
    {
        if (address >= 0x10008000 && address < 0x1000F000)
        {
#if defined(PS2X_GAME_GT4)
            // The IPU channels finish when the IPU has moved their data
            // (ps2_ipu.cpp), not on the first read-back of CHCR.
            if (address == 0x1000B000u || address == 0x1000B400u)
            {
                ipu().step();
                ps2x::vblank::noteIpuWork(); // the decoder polls these
                return m_ioRegisters[address];
            }
#endif
#if !defined(PS2X_GAME_GT3)
            // A posted PATH3 (async) keeps the GIF channel busy until the EE
            // has published it (a read publishes it if the worker has run it).
            if (address == 0x1000E010u && m_asyncPath3Posted != m_asyncPath3Published)
                publishAsyncPath3Completions();
            if (address == 0x1000A000u && m_asyncPath3Posted != m_asyncPath3Published)
            {
                publishAsyncPath3Completions();
                if (m_asyncPath3Posted != m_asyncPath3Published)
                    return m_ioRegisters[address];
            }
#endif
            if ((address & 0xFF) == 0x00)
            {
                uint32_t channelStatus = m_ioRegisters[address] & ~0x100u;
                m_ioRegisters[address] = channelStatus;
                return channelStatus;
            }
        }

        if (address >= 0x10000200 && address < 0x10000300)
        {
            return 0;
        }

        if (address >= 0x1000F200 && address <= 0x1000F260)
        {
            if (address == 0x1000F230)
            {
                return 0x60000;
            }
            if (address == 0x1000F240)
            {
                return 0xF0000002;
            }
            return 0;
        }
    }

    auto it = m_ioRegisters.find(address);
    if (it != m_ioRegisters.end())
    {
        return it->second;
    }

    return 0;
}

void PS2Memory::registerCodeRegion(uint32_t start, uint32_t end)
{
    if (end <= start)
    {
        std::cerr << "Ignoring invalid code region: start=0x" << std::hex << start
                  << " end=0x" << end << std::dec << std::endl;
        return;
    }

    if ((end - start) > PS2_RAM_SIZE)
    {
        std::cerr << "Ignoring oversized code region: start=0x" << std::hex << start
                  << " end=0x" << end << std::dec << std::endl;
        return;
    }

    for (const auto &existing : m_codeRegions)
    {
        if (existing.start == start && existing.end == end)
        {
            return;
        }
    }

    CodeRegion region;
    region.start = start;
    region.end = end;

    size_t sizeInWords = (end - start + 3u) / 4u;
    region.modified.resize(sizeInWords, false);

    m_codeRegions.push_back(region);
    RUNTIME_LOG("Registered code region: " << std::hex << start << " - " << end << std::dec);
}

bool PS2Memory::isAddressInRegion(uint32_t address, const CodeRegion &region)
{
    return (address >= region.start && address < region.end);
}

bool PS2Memory::isCodeAddress(uint32_t address) const
{
    for (const auto &region : m_codeRegions)
    {
        if (address >= region.start && address < region.end)
        {
            return true;
        }
    }
    return false;
}

void PS2Memory::markModified(uint32_t address, uint32_t size)
{
    if (size == 0)
    {
        return;
    }

    const uint64_t writeEnd = static_cast<uint64_t>(address) + static_cast<uint64_t>(size);
    for (auto &region : m_codeRegions)
    {
        const uint64_t regionStart = region.start;
        const uint64_t regionEnd = region.end;
        if (writeEnd <= regionStart || static_cast<uint64_t>(address) >= regionEnd)
        {
            continue;
        }

        uint32_t overlapStart = static_cast<uint32_t>(std::max<uint64_t>(address, regionStart));
        uint32_t overlapEnd = static_cast<uint32_t>(std::min<uint64_t>(writeEnd, regionEnd));

        for (uint32_t addr = overlapStart; addr < overlapEnd; addr += 4)
        {
            size_t bitIndex = (addr - region.start) / 4;
            if (bitIndex < region.modified.size())
            {
                region.modified[bitIndex] = true;
                RUNTIME_LOG("Marked code at " << std::hex << addr << std::dec << " as modified");
            }
        }
    }
}

bool PS2Memory::isCodeModified(uint32_t address, uint32_t size)
{
    if (size == 0)
    {
        return false;
    }

    const uint64_t writeEnd = static_cast<uint64_t>(address) + static_cast<uint64_t>(size);
    for (const auto &region : m_codeRegions)
    {
        const uint64_t regionStart = region.start;
        const uint64_t regionEnd = region.end;
        if (writeEnd <= regionStart || static_cast<uint64_t>(address) >= regionEnd)
        {
            continue;
        }

        uint32_t overlapStart = static_cast<uint32_t>(std::max<uint64_t>(address, regionStart));
        uint32_t overlapEnd = static_cast<uint32_t>(std::min<uint64_t>(writeEnd, regionEnd));

        for (uint32_t addr = overlapStart; addr < overlapEnd; addr += 4)
        {
            size_t bitIndex = (addr - region.start) / 4;
            if (bitIndex < region.modified.size() && region.modified[bitIndex])
            {
                return true; // Found modified code
            }
        }
    }

    return false; // No modifications found
}

void PS2Memory::clearModifiedFlag(uint32_t address, uint32_t size)
{
    if (size == 0)
    {
        return;
    }

    const uint64_t writeEnd = static_cast<uint64_t>(address) + static_cast<uint64_t>(size);
    for (auto &region : m_codeRegions)
    {
        const uint64_t regionStart = region.start;
        const uint64_t regionEnd = region.end;
        if (writeEnd <= regionStart || static_cast<uint64_t>(address) >= regionEnd)
        {
            continue;
        }

        uint32_t overlapStart = static_cast<uint32_t>(std::max<uint64_t>(address, regionStart));
        uint32_t overlapEnd = static_cast<uint32_t>(std::min<uint64_t>(writeEnd, regionEnd));

        for (uint32_t addr = overlapStart; addr < overlapEnd; addr += 4)
        {
            size_t bitIndex = (addr - region.start) / 4;
            if (bitIndex < region.modified.size())
            {
                region.modified[bitIndex] = false;
            }
        }
    }
}
