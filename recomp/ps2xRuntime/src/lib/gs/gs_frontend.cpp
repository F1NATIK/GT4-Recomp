#include "runtime/gs/gs_frontend.h"
#include "runtime/gs/gs_cpu_backend.h"
#include "runtime/gs/gs_crt.h"
#include "runtime/gs/gs_fullcolour.h"
#include "runtime/gs/gs_mip.h"
#include "ps2_log.h"
#include "runtime/ps2_memory.h"
#include "runtime/vblank_clock.h"
#if defined(PS2X_GAME_GT4) && !defined(GT4_STOCK_BUILD)
#include "../gt4_hud_move.h" // GT4_HUD_MOVE: GT4's own race HUD parts moved (HUD hand-off 7 B; not the Classic edition)
#endif
#include <atomic>
#include <chrono>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <set>
#include <tuple>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <sstream>

namespace
{
    static constexpr uint32_t kHostFrameWidth = 640u;

    GSPrimReg decodePrimRegister(uint64_t value)
    {
        GSPrimReg prim{};
        prim.type = static_cast<GSPrimType>(value & 0x7u);
        prim.iip = ((value >> 3) & 1u) != 0u;
        prim.tme = ((value >> 4) & 1u) != 0u;
        prim.fge = ((value >> 5) & 1u) != 0u;
        prim.abe = ((value >> 6) & 1u) != 0u;
        prim.aa1 = ((value >> 7) & 1u) != 0u;
        prim.fst = ((value >> 8) & 1u) != 0u;
        prim.ctxt = ((value >> 9) & 1u) != 0u;
        prim.fix = ((value >> 10) & 1u) != 0u;
        return prim;
    }

    static inline uint64_t loadLE64(const uint8_t *p)
    {
        uint64_t v;
        std::memcpy(&v, p, 8);
        return v;
    }

    struct PackedGifPacketTag
    {
        uint64_t lo = 0u;
        uint64_t hi = 0u;
        uint32_t payloadOffset = 0u;
        uint32_t nloop = 0u;
        uint32_t nreg = 0u;
        uint8_t regs[16]{};
    };

    template <typename Visitor>
    bool visitPackedGifPacket(const uint8_t *data, uint32_t sizeBytes, Visitor &&visitor)
    {
        uint32_t offset = 0u;
        while (offset + 16u <= sizeBytes)
        {
            PackedGifPacketTag tag{};
            tag.lo = loadLE64(data + offset);
            tag.hi = loadLE64(data + offset + 8u);

            const uint8_t flg = static_cast<uint8_t>((tag.lo >> 58u) & 0x3u);
            if (flg != GIF_FMT_PACKED)
                return false;

            tag.nloop = static_cast<uint32_t>(tag.lo & 0x7FFFu);
            tag.nreg = static_cast<uint32_t>((tag.lo >> 60u) & 0xFu);
            if (tag.nreg == 0u)
                tag.nreg = 16u;

            const uint64_t payloadBytes64 =
                static_cast<uint64_t>(tag.nloop) * static_cast<uint64_t>(tag.nreg) * 16ull;
            if (payloadBytes64 > 0xFFFFFFFFull)
                return false;

            offset += 16u;
            const uint32_t payloadBytes = static_cast<uint32_t>(payloadBytes64);
            if (payloadBytes > sizeBytes - offset)
                return false;

            tag.payloadOffset = offset;
            for (uint32_t i = 0u; i < tag.nreg; ++i)
                tag.regs[i] = static_cast<uint8_t>((tag.hi >> (i * 4u)) & 0xFu);

            if (!visitor(tag))
                return false;

            offset += payloadBytes;
        }

        return offset == sizeBytes;
    }

    bool validatePackedGifPacket(const uint8_t *data, uint32_t sizeBytes)
    {
        return visitPackedGifPacket(data, sizeBytes, [](const PackedGifPacketTag &)
                                    { return true; });
    }

    std::atomic<uint32_t> s_debugGifPacketCount{0};
    std::atomic<uint32_t> s_debugGsRegisterCount{0};
    std::atomic<uint32_t> s_debugGsPackedVertexCount{0};
    std::atomic<uint32_t> s_debugGsVertexKickCount{0};
    std::atomic<uint32_t> s_debugCopyRegCount{0};
    std::atomic<uint32_t> s_debugTexaWriteCount{0};
    std::atomic<uint32_t> s_debugCvFontUploadCount{0};
    std::atomic<uint32_t> s_debugLocalCopyCount{0};

    // GS mipmapping (gs_mip.h; analysis/phase5/specs/mipmap-design.md 4.2):
    // each textured primitive is resolved before Submit, after the draw logs
    // (they keep showing the game's own registers). PS2X_GS_MIPMAP=0 (GT3's
    // default): nothing changes.
    void reportMipStats(double seconds)
    {
        gsmip::Stats &s = gsmip::stats();
        auto take = [](std::atomic<uint64_t> &a) { return static_cast<unsigned long long>(a.exchange(0u, std::memory_order_relaxed)); };
        std::string ranges;
        for (int lo = 0; lo < 7; ++lo)
            for (int hi = lo; hi < 7; ++hi)
                if (const unsigned long long n = take(s.range[lo][hi]))
                {
                    char one[48];
                    std::snprintf(one, sizeof(one), "%s%d..%d %llu", ranges.empty() ? "" : ", ", lo, hi, n);
                    ranges += one;
                }
        std::string single, pixPer, pixSingle;
        for (int n = 0; n < 7; ++n)
        {
            char one[40];
            const unsigned long long a = take(s.single[n]), b = take(s.pixPerPixel[n]), c = take(s.pixSingle[n]);
            if (a || n < 4)
            {
                std::snprintf(one, sizeof(one), " L%d %llu", n, a);
                single += one;
            }
            if (b || n < 4)
            {
                std::snprintf(one, sizeof(one), " L%d %llu", n, b);
                pixPer += one;
            }
            if (c || n < 4)
            {
                std::snprintf(one, sizeof(one), " L%d %llu", n, c);
                pixSingle += one;
            }
        }
        const unsigned long long prims = take(s.prims), lcm = take(s.lcm), approx = take(s.approx), perPixel = take(s.perPixel);
        std::fprintf(stderr, "[gs-mip] %.1f s: prims %llu (lcm %llu) single%s (approx %llu) per-pixel %llu [%s]\n", seconds, prims, lcm,
                     single.c_str(), approx, perPixel, ranges.c_str());
        const unsigned long long blended = take(s.pixBlended), outside = take(s.outsideRange), mismatch = take(s.singleMismatch);
        std::fprintf(stderr, "[gs-mip]   cpu pixels: per-pixel prims%s (blended with the next level %llu); single-level prims%s; checks: "
                             "outside the level range %llu, resolved but own pick differs %llu\n",
                     pixPer.c_str(), blended, pixSingle.c_str(), outside, mismatch);
    }

    void resolveMip(GSPrimitiveBatch &batch)
    {
        static const bool on = gsmip::enabled();
        if (!on || !batch.state.prim.tme)
            return;
        static const bool collapse = gsmip::collapseEnabled();
        static const double statsSeconds = gsmip::statsSeconds();
        static bool announced = false;
        if (!announced)
        {
            announced = true;
            const int tint = gsmip::tintMode();
            std::fprintf(stderr, "[gs-mip] GS mipmapping on (PS2X_GS_MIPMAP=0 off): primitives resolved to one level%s; stats %s; "
                                 "level tint %s%s\n",
                         collapse ? " where their LOD range allows (PS2X_GS_MIP_COLLAPSE=0: per pixel)" : " never (PS2X_GS_MIP_COLLAPSE=0)",
                         statsSeconds > 0.0 ? "on (PS2X_GS_MIP_STATS)" : "off",
                         tint == 1 ? "on, both renderers (PS2X_GS_MIP_TINT=1)" : tint == 2 ? "on, CPU renderer only (PS2X_GS_MIP_TINT=2)" : "off",
                         gsmip::rangeClampEnabled() ? "" : "; per-pixel levels not kept in their range (PS2X_GS_MIP_RANGE_CLAMP=0)");
        }
        const gsmip::Resolved r = gsmip::resolve(batch, collapse);
        if (statsSeconds <= 0.0)
            return;
        static auto windowStart = std::chrono::steady_clock::now();
        static uint32_t calls = 0u;
        if (r != gsmip::Resolved::None)
        {
            gsmip::Stats &s = gsmip::stats();
            const GSDrawState &st = batch.state;
            s.prims.fetch_add(1u, std::memory_order_relaxed);
            if (st.context.tex1 & 1u)
                s.lcm.fetch_add(1u, std::memory_order_relaxed);
            if (r == gsmip::Resolved::PerPixel)
            {
                s.perPixel.fetch_add(1u, std::memory_order_relaxed);
                s.range[std::min<int>(st.mipLo, 6)][std::min<int>(st.mipHi, 6)].fetch_add(1u, std::memory_order_relaxed);
            }
            else
            {
                s.single[std::min<int>(st.mipTag - 1, 6)].fetch_add(1u, std::memory_order_relaxed);
                if (r == gsmip::Resolved::Approx)
                    s.approx.fetch_add(1u, std::memory_order_relaxed);
            }
        }
        if ((++calls & 1023u) == 0u)
        {
            const auto now = std::chrono::steady_clock::now();
            const double seconds = std::chrono::duration<double>(now - windowStart).count();
            if (seconds >= statsSeconds)
            {
                reportMipStats(seconds);
                windowStart = now;
            }
        }
    }
}


GS::GS()
    : m_backend(std::make_unique<GSCpuBackend>())
{
    reset();
}

namespace
{
// GT4_MOVIE_TRACE=1 (diagnostics, analysis/design/movie-stutter): a sampled hash of the image data uploaded since the
// last 16x16 upload to (0,0), i.e. per movie picture (GT4's player uploads a picture one macroblock at a time).
uint64_t g_movieTraceHash = 1469598103934665603ull;
uint64_t g_movieTraceBytes = 0;
}

void GS::init(uint8_t *vram, uint32_t vramSize, GSRegisters *privRegs)
{
    m_localMemoryStorage = vram;
    m_localMemorySize = vramSize;
    m_privRegs = privRegs;
    if (!m_backend)
        m_backend = std::make_unique<GSCpuBackend>();
    m_backend->Initialize(vram, vramSize);
    reset();
}

void GS::reset()
{
    std::lock_guard<std::recursive_mutex> lock(m_stateMutex);
    std::memset(m_ctx, 0, sizeof(m_ctx));
    m_prim = {};
    m_primRegister = {};
    m_prmodeRegister = {};
    m_curR = 0x80;
    m_curG = 0x80;
    m_curB = 0x80;
    m_curA = 0x80;
    m_curQ = 1.0f;
    m_curS = 0.0f;
    m_curT = 0.0f;
    m_curU = 0;
    m_curV = 0;
    m_curFog = 0;
    m_gifTag = {};
    m_fogR = 0;
    m_fogG = 0;
    m_fogB = 0;
    m_prmodecont = true;
    m_pabe = false;
    m_scanmsk = 0u;
    m_dimx = 0u;
    m_dthe = 0u;
    m_colclamp = 0u;
    m_texa = {0u, false, 0u};
    m_texclut = {0u, 0u, 0u};
    m_clutCbp0 = 0u;
    m_clutCbp1 = 0u;
    m_bitbltbuf = {};
    m_trxpos = {};
    m_trxreg = {};
    m_trxdir = 3;
    m_vtxCount = 0;
    m_vtxIndex = 0;
    m_preferredDisplaySourceFrame = {};
    m_preferredDisplayDestFbp = 0;
    m_hasPreferredDisplaySource = false;
    m_pageWriteSeq.fill(0u);
    for (auto &area : m_fbpPhaseArea)
        area.fill(0u);
    m_fbpPhase.fill(0xFFu);
    m_writeSeq = 0u;
    m_recentDisplayRegions.clear();
    m_latchCount = 0u;
    m_lastPresentedTick = ~0ull;
    m_displaySignature = ~0ull;
    if (m_backend)
    {
        m_backend->Flush();
        m_backend->Sync(GSSyncReason::Reset);
        m_backend->Reset();
    }
    {
        std::lock_guard<std::mutex> presentationLock(m_presentationMutex);
        m_hostPresentationFrame.clear();
        m_hostPresentationWidth = 0u;
        m_hostPresentationHeight = 0u;
        m_hostPresentationDisplayFbp = 0u;
        m_hostPresentationSourceFbp = 0u;
        m_hostPresentationUsedPreferred = false;
        m_hasHostPresentationFrame = false;
    }

    m_debugHistoryWrite = 0;
    m_debugHistoryCount = 0;
    m_debugNextSeq = 1;
    m_debugFrameIndex = 0;
    m_debugLastVsyncTick = UINT64_MAX;

    for (int i = 0; i < 2; ++i)
    {
        m_ctx[i].frame.fbw = 10;
        m_ctx[i].scissor = {0, 639, 0, 447};
        m_ctx[i].xyoffset = {0, 0};
    }
}

GSContext &GS::activeContext()
{
    return m_ctx[m_prim.ctxt ? 1 : 0];
}

void GS::snapshotVRAM()
{
    // Presentation/debug snapshots run outside m_stateMutex so the EE can keep
    // feeding the GS while a backend performs host-side conversion. Keep the
    // selected backend alive and unswappable for the duration of the call.
    std::lock_guard<std::mutex> backendLock(m_backendLifetimeMutex);
    if (!m_backend)
        return;
    std::vector<uint8_t> snapshot;
    m_backend->Sync(GSSyncReason::DebugReadback);
    m_backend->SnapshotVram(snapshot);
    std::lock_guard<std::mutex> lock(m_snapshotMutex);
    m_displaySnapshot.swap(snapshot);
}

const uint8_t *GS::lockDisplaySnapshot(uint32_t &outSize)
{
    m_snapshotMutex.lock();
    if (m_displaySnapshot.empty())
    {
        outSize = 0;
        return nullptr;
    }

    outSize = static_cast<uint32_t>(m_displaySnapshot.size());
    return m_displaySnapshot.data();
}

GSDebugSnapshot GS::getDebugSnapshot() const
{
    std::lock_guard<std::recursive_mutex> lock(m_stateMutex);

    GSDebugSnapshot snapshot{};
    snapshot.ctx[0] = m_ctx[0];
    snapshot.ctx[1] = m_ctx[1];
    snapshot.prim = m_prim;
    snapshot.texa = m_texa;
    snapshot.texclut = m_texclut;
    snapshot.scanmsk = m_scanmsk;
    snapshot.dimx = m_dimx;
    snapshot.dthe = m_dthe;
    snapshot.colclamp = m_colclamp;
    snapshot.bitbltbuf = m_bitbltbuf;
    snapshot.trxpos = m_trxpos;
    snapshot.trxreg = m_trxreg;
    const GSTransferSnapshot transfer = m_backend ? m_backend->GetTransferSnapshot() : GSTransferSnapshot{};
    snapshot.trxdir = transfer.direction;
    snapshot.transferX = transfer.x;
    snapshot.transferY = transfer.y;
    snapshot.transferTotalPixels = transfer.totalPixels;
    snapshot.transferCopiedPixels = transfer.copiedPixels;
    snapshot.lastDisplayBaseBytes = m_lastDisplayBaseBytes;
    snapshot.preferredDisplaySourceFrame = m_preferredDisplaySourceFrame;
    snapshot.preferredDisplayDestFbp = m_preferredDisplayDestFbp;
    snapshot.hasPreferredDisplaySource = m_hasPreferredDisplaySource;
    {
        std::lock_guard<std::mutex> presentationLock(m_presentationMutex);
        snapshot.hostPresentationWidth = m_hostPresentationWidth;
        snapshot.hostPresentationHeight = m_hostPresentationHeight;
        snapshot.hostPresentationDisplayFbp = m_hostPresentationDisplayFbp;
        snapshot.hostPresentationSourceFbp = m_hostPresentationSourceFbp;
        snapshot.hostPresentationUsedPreferred = m_hostPresentationUsedPreferred;
        snapshot.hasHostPresentationFrame = m_hasHostPresentationFrame;
    }
    snapshot.localToHostPendingBytes = transfer.localToHostPendingBytes;
    return snapshot;
}

std::vector<GSDebugHistoryEntry> GS::getDebugHistory() const
{
    std::lock_guard<std::recursive_mutex> lock(m_stateMutex);

    std::vector<GSDebugHistoryEntry> out;
    out.reserve(m_debugHistoryCount);
    const size_t first = (m_debugHistoryWrite + kDebugHistoryCapacity - m_debugHistoryCount) % kDebugHistoryCapacity;
    for (size_t i = 0; i < m_debugHistoryCount; ++i)
    {
        out.push_back(m_debugHistory[(first + i) % kDebugHistoryCapacity]);
    }
    return out;
}

void GS::clearDebugHistory()
{
    std::lock_guard<std::recursive_mutex> lock(m_stateMutex);
    m_debugHistoryWrite = 0;
    m_debugHistoryCount = 0;
    m_debugNextSeq = 1;
    m_debugFrameIndex = 0;
    m_debugLastVsyncTick = UINT64_MAX;
}

bool GS::isDebugHistoryPaused() const
{
    std::lock_guard<std::recursive_mutex> lock(m_stateMutex);
    return m_debugHistoryPaused;
}

void GS::setDebugHistoryPaused(bool paused)
{
    std::lock_guard<std::recursive_mutex> lock(m_stateMutex);
    m_debugHistoryPaused = paused;
}

GSDebugHistoryEntry GS::makeDebugEventUnlocked(GSDebugEventKind kind) const
{
    GSDebugHistoryEntry entry{};
    entry.kind = kind;
    entry.prim = m_prim;
    const uint32_t ci = m_prim.ctxt ? 1u : 0u;
    entry.frame = m_ctx[ci].frame;
    entry.zbuf = m_ctx[ci].zbuf;
    entry.tex0 = m_ctx[ci].tex0;
    entry.scissor = m_ctx[ci].scissor;
    entry.test = m_ctx[ci].test;
    entry.alpha = m_ctx[ci].alpha;
    entry.bitbltbuf = m_bitbltbuf;
    entry.trxpos = m_trxpos;
    entry.trxreg = m_trxreg;
    const GSTransferSnapshot transfer = m_backend ? m_backend->GetTransferSnapshot() : GSTransferSnapshot{};
    entry.trxdir = transfer.direction;
    entry.transferPixels = transfer.totalPixels;
    return entry;
}

void GS::recordDebugEventUnlocked(GSDebugHistoryEntry entry)
{
    if (m_debugHistoryPaused)
    {
        return;
    }

    const uint64_t tick = m_privRegs ? m_privRegs->vsyncTick.load(std::memory_order_acquire) : 0u;
    if (m_debugLastVsyncTick == UINT64_MAX)
    {
        m_debugLastVsyncTick = tick;
    }
    else if (tick != m_debugLastVsyncTick)
    {
        ++m_debugFrameIndex;
        m_debugLastVsyncTick = tick;
    }

    entry.seq = m_debugNextSeq++;
    entry.vsyncTick = tick;
    entry.frameIndex = m_debugFrameIndex;

    m_debugHistory[m_debugHistoryWrite] = entry;
    m_debugHistoryWrite = (m_debugHistoryWrite + 1u) % kDebugHistoryCapacity;
    if (m_debugHistoryCount < kDebugHistoryCapacity)
    {
        ++m_debugHistoryCount;
    }
}

void GS::recordGifTagDebugEventUnlocked(uint32_t sizeBytes, uint32_t nloop, uint8_t flg, uint32_t nreg)
{
    if (m_debugHistoryPaused)
    {
        return;
    }

    GSDebugHistoryEntry entry = makeDebugEventUnlocked(GSDebugEventKind::GifTag);
    entry.gifSizeBytes = sizeBytes;
    entry.gifNloop = nloop;
    entry.gifFlg = flg;
    entry.gifNreg = static_cast<uint8_t>(std::min<uint32_t>(nreg, 16u));
    recordDebugEventUnlocked(entry);
}

void GS::recordRegisterDebugEventUnlocked(uint8_t regAddr, uint64_t value)
{
    if (m_debugHistoryPaused)
    {
        return;
    }

    switch (regAddr)
    {
    case GS_REG_PRIM:
    case GS_REG_TEX0_1:
    case GS_REG_TEX0_2:
    case GS_REG_TEX2_1:
    case GS_REG_TEX2_2:
    case GS_REG_TEXA:
    case GS_REG_TEXCLUT:
    case GS_REG_FRAME_1:
    case GS_REG_FRAME_2:
    case GS_REG_ZBUF_1:
    case GS_REG_ZBUF_2:
    case GS_REG_ALPHA_1:
    case GS_REG_ALPHA_2:
    case GS_REG_TEST_1:
    case GS_REG_TEST_2:
    case GS_REG_SCISSOR_1:
    case GS_REG_SCISSOR_2:
    case GS_REG_XYOFFSET_1:
    case GS_REG_XYOFFSET_2:
    case GS_REG_BITBLTBUF:
    case GS_REG_TRXPOS:
    case GS_REG_TRXREG:
    case GS_REG_TRXDIR:
        break;
    default:
        return;
    }

    GSDebugHistoryEntry entry = makeDebugEventUnlocked(GSDebugEventKind::Register);
    entry.reg = regAddr;
    entry.regValue = value;
    recordDebugEventUnlocked(entry);
}

void GS::recordDrawDebugEventUnlocked(int vertexCount)
{
    if (m_debugHistoryPaused)
    {
        return;
    }

    if (vertexCount <= 0)
    {
        return;
    }

    GSDebugHistoryEntry entry = makeDebugEventUnlocked(GSDebugEventKind::Draw);
    entry.vertexCount = static_cast<uint32_t>(vertexCount);

    const int count = std::min(vertexCount, kMaxVerts);
    entry.xMin = entry.xMax = m_vtxQueue[0].x;
    entry.yMin = entry.yMax = m_vtxQueue[0].y;
    entry.zMin = entry.zMax = m_vtxQueue[0].z;
    entry.aMin = entry.aMax = m_vtxQueue[0].a;

    for (int i = 1; i < count; ++i)
    {
        const GSVertex &v = m_vtxQueue[i];
        entry.xMin = std::min(entry.xMin, v.x);
        entry.xMax = std::max(entry.xMax, v.x);
        entry.yMin = std::min(entry.yMin, v.y);
        entry.yMax = std::max(entry.yMax, v.y);
        entry.zMin = std::min(entry.zMin, v.z);
        entry.zMax = std::max(entry.zMax, v.z);
        entry.aMin = std::min(entry.aMin, v.a);
        entry.aMax = std::max(entry.aMax, v.a);
    }

    recordDebugEventUnlocked(entry);
}

void GS::recordTransferDebugEventUnlocked()
{
    if (m_debugHistoryPaused)
    {
        return;
    }

    GSDebugHistoryEntry entry = makeDebugEventUnlocked(GSDebugEventKind::Transfer);
    entry.transferPixels = m_backend ? m_backend->GetTransferSnapshot().totalPixels : 0u;
    recordDebugEventUnlocked(entry);
}

void GS::recordPresentDebugEventUnlocked(uint32_t displayFbp, uint32_t sourceFbp, uint32_t width, uint32_t height, bool usedPreferred)
{
    if (m_debugHistoryPaused)
    {
        return;
    }

    GSDebugHistoryEntry entry = makeDebugEventUnlocked(GSDebugEventKind::Present);
    entry.displayFbp = displayFbp;
    entry.sourceFbp = sourceFbp;
    entry.width = width;
    entry.height = height;
    entry.usedPreferred = usedPreferred;
    recordDebugEventUnlocked(entry);
}

bool GS::getPreferredDisplaySource(GSFrameReg &outSource, uint32_t &outDestFbp) const
{
    std::lock_guard<std::recursive_mutex> lock(m_stateMutex);
    if (!m_hasPreferredDisplaySource)
    {
        outSource = {};
        outDestFbp = 0u;
        return false;
    }

    outSource = m_preferredDisplaySourceFrame;
    outDestFbp = m_preferredDisplayDestFbp;
    return true;
}

void GS::unlockDisplaySnapshot()
{
    m_snapshotMutex.unlock();
}

uint32_t GS::getLastDisplayBaseBytes() const
{
    return m_lastDisplayBaseBytes;
}

void GS::flushRaster()
{
    std::lock_guard<std::mutex> backendLock(m_backendLifetimeMutex);
    if (m_backend)
        m_backend->Flush();
}

void GS::refreshDisplaySnapshot()
{
    snapshotVRAM();
}

GSPresentationRequest GS::buildPresentationRequestUnlocked() const
{
    GSPresentationRequest request{};
    if (!m_privRegs)
        return request;
    request.pmode = m_privRegs->pmode;
    request.smode2 = m_privRegs->smode2;
    request.dispfb1 = m_privRegs->dispfb1;
    request.display1 = m_privRegs->display1;
    request.dispfb2 = m_privRegs->dispfb2;
    request.display2 = m_privRegs->display2;
    request.bgcolor = m_privRegs->bgcolor;
    request.vsyncTick = m_privRegs->vsyncTick.load(std::memory_order_acquire);
    request.crt = ps2x::crt::current();
    if (m_snapshot)
    {
        request.crt = m_snapshot->crt;
        request.pmode = m_snapshot->pmode;
        request.smode2 = m_snapshot->smode2;
        request.dispfb1 = m_snapshot->dispfb1;
        request.display1 = m_snapshot->display1;
        request.dispfb2 = m_snapshot->dispfb2;
        request.display2 = m_snapshot->display2;
        request.bgcolor = m_snapshot->bgcolor;
        request.vsyncTick = m_snapshot->vsyncTick;
    }
    request.contextFrames[0] = m_ctx[0].frame;
    request.contextFrames[1] = m_ctx[1].frame;
    request.preferredSource = m_preferredDisplaySourceFrame;
    request.preferredDestFbp = m_preferredDisplayDestFbp;
    request.hasPreferredSource = m_hasPreferredDisplaySource;
    return request;
}

namespace
{
    // Page geometry of a pixel storage format: CT32/CT24/Z32/Z24 and the
    // T8H/T4HL/T4HH formats (stored in the CT32 layout) use 64x32 pages,
    // 16-bit formats 64x64, PSMT8 128x64, PSMT4 128x128.
    void gsPageGeometry(uint8_t psm, uint32_t &pageWidth, uint32_t &pageHeight)
    {
        switch (psm & 0x3Fu)
        {
        case 0x02u: case 0x0Au: case 0x32u: case 0x3Au:
            pageWidth = 64u; pageHeight = 64u; break;
        case 0x13u:
            pageWidth = 128u; pageHeight = 64u; break;
        case 0x14u:
            pageWidth = 128u; pageHeight = 128u; break;
        default:
            pageWidth = 64u; pageHeight = 32u; break;
        }
    }
}

void GS::noteWrittenRect(uint32_t firstPage, uint32_t bufferWidth, uint8_t psm, int x0, int y0, int x1, int y1)
{
    uint32_t pageWidth = 64u, pageHeight = 32u;
    gsPageGeometry(psm, pageWidth, pageHeight);
    const uint32_t pagesPerRow = std::max<uint32_t>(1u, (std::max<uint32_t>(bufferWidth, 1u) * 64u) / pageWidth);
    x0 = std::max(x0, 0);
    y0 = std::max(y0, 0);
    x1 = std::min(x1, static_cast<int>(pagesPerRow * pageWidth) - 1);
    y1 = std::min(y1, 2047);
    if (x1 < x0 || y1 < y0)
        return;
    for (uint32_t row = static_cast<uint32_t>(y0) / pageHeight; row <= static_cast<uint32_t>(y1) / pageHeight; ++row)
        for (uint32_t col = static_cast<uint32_t>(x0) / pageWidth; col <= static_cast<uint32_t>(x1) / pageWidth; ++col)
            m_pageWriteSeq[(firstPage + row * pagesPerRow + col) % kGsPageCount] = m_writeSeq;
}

namespace
{
    std::atomic<float> s_hudSqueeze{1.0f};
    std::atomic<int> s_hudView{-1}; // GT3's race view (its HUD layout): 0 bumper, 1 chase, -1 not known
    std::atomic<float> s_hudMiddle{0.0f}; // triple screens: the middle screen's share of the width (0: 16:9, no bezels)
    std::atomic<uint32_t> s_hudHide{0u};  // GT3 HUD groups to hide in races (GS::HudHide bits)
    std::atomic<bool> s_hudRace{false};   // a race is on (the race camera runs): the HUD groups apply
    std::atomic<bool> s_hudHideMiddle{false}; // GT3's countdown hidden (GS::setHudHideMiddle; the grid-start lights)
    // The mirror's rectangle (x in frame pixels) as its 3D view last drew,
    // and when: its frame's 2D pieces lie within it.
    std::atomic<uint32_t> s_mirrorX{0u};  // x0 | x1 << 16
    std::atomic<uint64_t> s_mirrorSeen{0u};
    std::atomic<bool> s_insetWatch{false};
    std::atomic<uint64_t> s_insetRect{0u}; // x0 | y0 << 16 | x1 << 32 | y1 << 48
    std::atomic<uint64_t> s_insetTick{0u};
}

void GS::setHudSqueeze(float factor)
{
    s_hudSqueeze.store(factor > 0.0f && factor < 1.0f ? factor : 1.0f, std::memory_order_relaxed);
}

void GS::setHudView(int view) { s_hudView.store(view, std::memory_order_relaxed); }

void GS::setHudHide(uint32_t groups) { s_hudHide.store(groups, std::memory_order_relaxed); }
void GS::setHudRace(bool racing) { s_hudRace.store(racing, std::memory_order_relaxed); }
void GS::setHudHideMiddle(bool on) { s_hudHideMiddle.store(on, std::memory_order_relaxed); }

void GS::setHudMiddleScreen(float fraction) { s_hudMiddle.store(fraction > 0.0f && fraction < 1.0f ? fraction : 0.0f, std::memory_order_relaxed); }

void GS::setInsetWatch(bool on) { s_insetWatch.store(on, std::memory_order_relaxed); }

bool GS::insetView(uint16_t rect[4], uint64_t &tick)
{
    tick = s_insetTick.load(std::memory_order_relaxed);
    const uint64_t r = s_insetRect.load(std::memory_order_relaxed);
    if (tick == 0u || r == 0u)
        return false;
    for (int i = 0; i < 4; ++i)
        rect[i] = static_cast<uint16_t>(r >> (16 * i));
    return true;
}

void GS::noteInsetView(const GSPrimitiveBatch &batch)
{
#if defined(GT3_STOCK_BUILD) || defined(PS2X_GAME_GT4)
    (void)batch; // the stock build: no phone mirror
#else
    static const bool logOn = std::getenv("GT3_INSET_LOG") != nullptr;
    if (!s_insetWatch.load(std::memory_order_relaxed) && !logOn)
        return;
    const GSContext &ctx = batch.state.context;
    if (((ctx.test >> 16) & 1u) == 0u || ((ctx.test >> 17) & 3u) < 2u)
        return;
    const float width = static_cast<float>(std::max<uint32_t>(1u, ctx.frame.fbw) * 64u);
    const float half = width * 0.5f;
    if (std::fabs(static_cast<float>(ctx.xyoffset.ofx) / 16.0f + half - 2048.0f) > 1.0f)
        return;
    const GSScissorReg &sc = ctx.scissor;
    if ((sc.x0 == 0u && sc.x1 + 1.0f >= width) || sc.x0 >= half || sc.x1 < half || sc.x1 <= sc.x0 || sc.y1 <= sc.y0 || sc.y0 == 0u)
        return;
    // GT3_INSET_LOG=<n>: the first n distinct texture settings (TEX1, size,
    // format) the mirror is drawn with.
    static long insetLog = [] {
        const char *v = std::getenv("GT3_INSET_LOG");
        return v ? std::strtol(v, nullptr, 10) : 0L;
    }();
    if (insetLog > 0 && batch.state.prim.tme)
    {
        static std::set<std::tuple<uint64_t, uint32_t, uint32_t>> seen;
        if (seen.insert({ctx.tex1, (ctx.tex0.tw << 8) | ctx.tex0.th, ctx.tex0.psm}).second)
        {
            --insetLog;
            std::fprintf(stderr,
                         "[inset] tex1 %llx (mmag %u mmin %u mxl %u lcm %u) tex %ux%u psm %02x linear %d; frame %03x/%u psm %02x scissor "
                         "%u,%u-%u,%u ofs %u,%u v0 %.1f,%.1f\n",
                         static_cast<unsigned long long>(ctx.tex1), static_cast<unsigned>((ctx.tex1 >> 5) & 1u),
                         static_cast<unsigned>((ctx.tex1 >> 6) & 7u), static_cast<unsigned>((ctx.tex1 >> 2) & 7u),
                         static_cast<unsigned>(ctx.tex1 & 1u), 1u << ctx.tex0.tw, 1u << ctx.tex0.th, ctx.tex0.psm,
                         batch.state.linearFilter ? 1 : 0, ctx.frame.fbp, ctx.frame.fbw, ctx.frame.psm, sc.x0, sc.y0, sc.x1, sc.y1,
                         ctx.xyoffset.ofx / 16u, ctx.xyoffset.ofy / 16u, batch.vertexCount ? batch.vertices[0].x : 0.0f,
                         batch.vertexCount ? batch.vertices[0].y : 0.0f);
        }
    }
    if (!s_insetWatch.load(std::memory_order_relaxed))
        return;
    s_insetRect.store(static_cast<uint64_t>(sc.x0) | (static_cast<uint64_t>(sc.y0) << 16) | (static_cast<uint64_t>(sc.x1) << 32) |
                          (static_cast<uint64_t>(sc.y1) << 48),
                      std::memory_order_relaxed);
    s_insetTick.store(m_privRegs ? m_privRegs->vsyncTick.load() : 1u, std::memory_order_relaxed);
#endif
}

// Wide pictures: GT3 draws the 3D into a render target (camera widened by
// gt3_aspect.cpp), copies it to the display buffer and draws the HUD there in
// 2D (Z test ALWAYS, XYOFFSET centring the screen on 2048). Shown wider, that
// HUD would stretch, so its primitives are squeezed toward the centre: it
// keeps its shape, in the middle 4:3 of the picture. Left alone: depth-tested
// 3D, copies of a 3D target, anything spanning the whole width (fades,
// backgrounds), other buffers and offsets (GT3's scratch passes).
// The rear-view mirror, an inset 3D view drawn into the 3D target within a
// scissor around the middle (its camera is not widened), is squeezed the
// same way, scissor included, so it keeps its 4:3 shape too.
// Races, any shape (GS::setHudHide): the same classification hides GT3's HUD
// groups the user turned off (false: the batch is not drawn) - position and
// map (left), lap times (right), gauges and lights (the bottom band), the
// mirror (its 3D view and the frame around it); messages in the middle
// (countdown, pause, results) always stay.
bool GS::squeezeHud(GSPrimitiveBatch &batch)
{
#if defined(GT3_STOCK_BUILD) || defined(PS2X_GAME_GT4)
    (void)batch; // the stock build: GT3's HUD as drawn
    return true;
#else
    const float factor = s_hudSqueeze.load(std::memory_order_relaxed);
    const uint32_t hide = s_hudRace.load(std::memory_order_relaxed) ? s_hudHide.load(std::memory_order_relaxed) : 0u;
    const bool hideMiddle = s_hudHideMiddle.load(std::memory_order_relaxed);
    if ((factor >= 1.0f && hide == 0u && !hideMiddle) || batch.vertexCount == 0u)
        return true;
    const bool squeeze = factor < 1.0f;
    GSContext &ctx = batch.state.context;
    const uint32_t fbp = ctx.frame.fbp & 0x1FFu;
    const float width = static_cast<float>(std::max<uint32_t>(1u, ctx.frame.fbw) * 64u);
    const float centre = static_cast<float>(ctx.xyoffset.ofx) / 16.0f + width * 0.5f;
    const bool centred = std::fabs(centre - 2048.0f) <= 1.0f;
    const auto squeezeX = [&] {
        for (uint32_t i = 0; i < batch.vertexCount; ++i)
        {
            float &x = batch.vertices[i].x;
            x = std::round((centre + (x - centre) * factor) * 16.0f) / 16.0f;
        }
    };
    const bool depthTested = ((ctx.test >> 16) & 1u) != 0u && ((ctx.test >> 17) & 3u) >= 2u;
    const bool depthTarget = depthTested || ((m_depthTargets[fbp >> 6] >> (fbp & 63u)) & 1u) != 0u;
    if (depthTested)
        m_depthTargets[fbp >> 6] |= 1ull << (fbp & 63u);
    const float half = width * 0.5f;
    if (depthTarget && centred && (ctx.scissor.x0 > 0u || ctx.scissor.x1 + 1.0f < width) && ctx.scissor.x0 < half &&
        ctx.scissor.x1 >= half && ctx.scissor.y0 > 0u)
    {
        // The rear-view mirror's 3D view (below the top: a scissor from the
        // top is the main view's middle screen, angled triple screens).
        s_mirrorX.store(static_cast<uint32_t>(ctx.scissor.x0) | (static_cast<uint32_t>(ctx.scissor.x1) << 16), std::memory_order_relaxed);
        s_mirrorSeen.store(m_privRegs ? m_privRegs->vsyncTick.load() : 1u, std::memory_order_relaxed);
        if (hide & HudHideMirror)
            return false;
        if (!squeeze)
            return true;
        squeezeX();
        ctx.scissor.x0 = static_cast<uint16_t>(std::ceil(half + (ctx.scissor.x0 - half) * factor));
        ctx.scissor.x1 = static_cast<uint16_t>(std::floor(half + (ctx.scissor.x1 + 1.0f - half) * factor) - 1.0f);
        return true;
    }
    if (depthTested)
        return true;
    if (m_privRegs)
    {
        const uint64_t pmode = m_privRegs->pmode;
        for (int circuit = 0; circuit < 2; ++circuit)
        {
            if (((pmode >> circuit) & 1u) == 0u)
                continue;
            const uint32_t shown = static_cast<uint32_t>((circuit ? m_privRegs->dispfb2 : m_privRegs->dispfb1) & 0x1FFu);
            if (std::find(m_hudFbps.begin(), m_hudFbps.end(), shown) == m_hudFbps.end())
            {
                std::rotate(m_hudFbps.rbegin(), m_hudFbps.rbegin() + 1, m_hudFbps.rend());
                m_hudFbps[0] = shown;
            }
        }
    }
    if (!centred || ctx.frame.psm >= GS_PSM_Z32 || std::find(m_hudFbps.begin(), m_hudFbps.end(), fbp) == m_hudFbps.end())
        return true;
    if (batch.state.prim.tme)
    {
        const uint32_t page = (ctx.tex0.tbp0 >> 5) & 0x1FFu;
        if ((ctx.tex0.tbp0 & 31u) == 0u && ((m_depthTargets[page >> 6] >> (page & 63u)) & 1u) != 0u)
            return true;
        // Screen effects sampling a displayed frame (the replays' heat haze
        // reads the buffer it draws into) stay where their texels are.
        const uint32_t framePages = std::max<uint32_t>(1u, ctx.frame.fbw) * 14u; // up to 448 lines
        for (const uint32_t shown : m_hudFbps)
            if (shown != 0xFFFFFFFFu && page >= shown && page < shown + framePages)
                return true;
    }
    float x0 = batch.vertices[0].x, x1 = x0, y0 = batch.vertices[0].y, y1 = y0;
    for (uint32_t i = 1; i < batch.vertexCount; ++i)
    {
        x0 = std::min(x0, batch.vertices[i].x);
        x1 = std::max(x1, batch.vertices[i].x);
        y0 = std::min(y0, batch.vertices[i].y);
        y1 = std::max(y1, batch.vertices[i].y);
    }
    if (x1 - x0 >= width - 1.0f)
        return true;
    // Anchored to its side (the user: "the HUD was still in 4:3"): elements
    // at the left (position, track map) keep their place from the left edge,
    // those at the right (lap times, the chase view's rev counter) from the
    // right edge, the rest (mirror, the bumper view's gauges, countdown,
    // messages) from the middle; each keeps its 4:3 shape. On triple screens
    // (wider than 2.5:1) the edges are the middle screen's (16:9).
    // GT3_HUD_ANCHOR=0: all squeezed toward the middle, as before.
    static const bool anchor = [] {
        const char *v = std::getenv("GT3_HUD_ANCHOR");
        return !(v && *v == '0');
    }();
    const float half4x3 = width * 0.5f;
    const float dx = (x0 + x1) * 0.5f - centre;
    const float top = static_cast<float>(ctx.xyoffset.ofy) / 16.0f; // the frame's first line
    const float yMid = (y0 + y1) * 0.5f - top;
    // The frame's height: GT3 draws its HUD into 224-line fields (scissor
    // 0-223), so heights are taken from the scissor.
    const float height = static_cast<float>(ctx.scissor.y1) + 1.0f;
    // GT3's race HUD (640 wide): left column from 110 left of the middle,
    // right column from 100 right; in the bottom 40% the bumper view's two
    // gauges are one centred cluster, the chase view's rev counter is right
    // of 100.
    static int hudLog = [] {
        const char *v = std::getenv("GT3_HUD_LOG");
        return (v && *v) ? std::atoi(v) : 0;
    }();
    if (hudLog > 0)
    {
        --hudLog;
        std::fprintf(stderr, "[hud] x %.1f-%.1f y %.1f-%.1f ofx %.1f ofy %.1f scissor %u-%u / %u-%u fbp %03x view %d\n", x0, x1, y0, y1,
                     ctx.xyoffset.ofx / 16.0f, ctx.xyoffset.ofy / 16.0f, ctx.scissor.x0, ctx.scissor.x1, ctx.scissor.y0, ctx.scissor.y1, fbp,
                     s_hudView.load(std::memory_order_relaxed));
    }
    // Tables over the race (the results at the finish): a panel over 80% of
    // the 4:3 width, not all of it. While one is up (drawn this VSync or in
    // the last 3) the whole HUD keeps GT3's 4:3 layout in the middle, as on
    // a 4:3 screen, and nothing over the panel's lines is hidden: anchoring
    // split each row (position and the start of the car's name at the left
    // edge, the rest in the middle, times at the right; the user: "the end
    // of race screen ... was abit out of order"). Only the GS thread gets here.
    bool inPanel = false, panelUp = false;
    {
        struct Lines
        {
            float y0 = 0.0f, y1 = -1.0f;
            uint64_t tick = 0u;
        };
        static Lines current, previous;
        const uint64_t now = m_privRegs ? m_privRegs->vsyncTick.load() : 1u;
        const float py0 = y0 - top, py1 = y1 - top;
        if (x1 - x0 >= width * 0.8f && py1 - py0 >= 6.0f)
        {
            if (current.tick != now)
            {
                if (current.tick != 0u)
                    previous = current;
                current = {py0, py1, now};
            }
            else
            {
                current.y0 = std::min(current.y0, py0);
                current.y1 = std::max(current.y1, py1);
            }
            static int panelLog = [] {
                const char *v = std::getenv("GT3_HUD_PANEL_LOG");
                return (v && *v) ? std::atoi(v) : 0;
            }();
            if (panelLog > 0)
            {
                --panelLog;
                std::fprintf(stderr, "[hud-panel] tick %llu x %.1f-%.1f y %.1f-%.1f (lines %.1f-%.1f) prim %d tme %d abe %d\n",
                             static_cast<unsigned long long>(now), x0 - (centre - half4x3), x1 - (centre - half4x3), py0, py1, current.y0,
                             current.y1, static_cast<int>(batch.state.prim.type), batch.state.prim.tme ? 1 : 0, batch.state.prim.abe ? 1 : 0);
            }
        }
        // (8 VSyncs: under GT3_TURBO a frame can span several.)
        for (const Lines *l : {&current, &previous})
            if (l->tick != 0u && now - l->tick <= 8u)
            {
                panelUp = true;
                inPanel |= yMid >= l->y0 - 2.0f && yMid <= l->y1 + 2.0f;
            }
    }
    // Lines of text across an anchor line (the course title before a race
    // and in its replay, "SUPER SPEEDWAY", came out "SU | PER SPEEDW | AY"):
    // textured glyph-sized sprites whose heights and middles agree within
    // 20% of the height form a line of text (letter boxes differ a little:
    // the title's S 62.7-81.1, U 63.4-81.9, most 63.4-81.1). A line of 3+
    // glyphs with some in the middle and some beyond an anchor line stays
    // whole in the middle, and so does every glyph on it (the title's
    // letters fly in one by one from the left). Found from each VSync's
    // sprites and used during the next two.
    bool inTextRun = false;
    {
        struct Glyph
        {
            float y0, y1, x0, x1;
            uint32_t tex;
            float mid() const { return 0.5f * (y0 + y1); }
            float h() const { return y1 - y0; }
        };
        static std::vector<Glyph> glyphs, runs, runsBefore;
        static uint64_t glyphTick = 0u;
        const uint64_t now = m_privRegs ? m_privRegs->vsyncTick.load() : 1u;
        const auto sideOf = [&](float x) { return x - centre < -110.0f ? -1 : (x - centre > 100.0f ? 1 : 0); };
        const auto sameLine = [](const Glyph &line, float mid, float h) {
            return std::fabs(mid - line.mid()) <= 0.2f * line.h() && std::fabs(h - line.h()) <= 0.2f * line.h();
        };
        if (now != glyphTick)
        {
            // The last VSync's lines.
            runsBefore.swap(runs);
            runs.clear();
            std::sort(glyphs.begin(), glyphs.end(), [](const Glyph &a, const Glyph &b) { return a.mid() < b.mid(); });
            std::vector<char> used(glyphs.size(), 0);
            for (size_t i = 0; i < glyphs.size(); ++i)
            {
                if (used[i])
                    continue;
                const Glyph first = glyphs[i];
                Glyph run = first;
                int count = 0, sides = 0;
                for (size_t j = i; j < glyphs.size() && glyphs[j].mid() - first.mid() <= 0.2f * first.h(); ++j)
                {
                    const Glyph &g = glyphs[j];
                    if (used[j] || !sameLine(first, g.mid(), g.h()))
                        continue;
                    used[j] = 1;
                    ++count;
                    run.x0 = std::min(run.x0, g.x0);
                    run.x1 = std::max(run.x1, g.x1);
                    sides |= 1 << (sideOf(0.5f * (g.x0 + g.x1)) + 1);
                }
                if (count >= 3 && (sides & 2) != 0 && (sides & 5) != 0)
                    runs.push_back(run);
            }
            // GT3_HUD_TEXT_LOG=<n> (diagnostic): the lines found in the first n
            // VSyncs that had any.
            static int textLog = [] {
                const char *v = std::getenv("GT3_HUD_TEXT_LOG");
                return (v && *v) ? std::atoi(v) : 0;
            }();
            if (textLog > 0 && !runs.empty())
            {
                --textLog;
                for (const Glyph &r : runs)
                    std::fprintf(stderr, "[hud-text] tick %llu line tex %x y %.1f-%.1f x %.1f-%.1f (%zu glyphs this VSync)\n",
                                 static_cast<unsigned long long>(glyphTick), r.tex, r.y0, r.y1, r.x0 - (centre - half4x3), r.x1 - (centre - half4x3),
                                 glyphs.size());
            }
            glyphs.clear();
            glyphTick = now;
        }
        const float py0 = y0 - top, py1 = y1 - top;
        if (batch.state.prim.tme && batch.state.prim.type == GS_PRIM_SPRITE && py1 - py0 >= 4.0f && py1 - py0 <= 60.0f && x1 - x0 <= 64.0f &&
            yMid <= height * 0.6f && glyphs.size() < 4096u)
        {
            glyphs.push_back({py0, py1, x0, x1, static_cast<uint32_t>(ctx.tex0.tbp0)});
            // Any texture: GT3 alternates its font between copies frame by
            // frame (the replay title: 0x31a2 / 0x3e62).
            for (const std::vector<Glyph> *list : {&runs, &runsBefore})
                for (const Glyph &r : *list)
                    if (sameLine(r, 0.5f * (py0 + py1), py1 - py0))
                        inTextRun = true;
        }
    }
    int side = 0;
    const bool bottom = yMid > height * 0.6f && !inPanel;
    if (inPanel || inTextRun)
        side = 0;
    else if (bottom)
        side = (s_hudView.load(std::memory_order_relaxed) == 1 && dx > 100.0f) ? 1 : 0;
    else
        side = dx < -110.0f ? -1 : (dx > 100.0f ? 1 : 0);
    // The mirror's frame: in the upper part, within its 3D view's x range
    // (drawn within the last 10 VSyncs).
    bool mirror = false;
    if (!bottom && !inPanel && yMid < height * 0.3f)
    {
        const uint64_t now = m_privRegs ? m_privRegs->vsyncTick.load() : 1u;
        const uint32_t mx = s_mirrorX.load(std::memory_order_relaxed);
        const float left = centre - half4x3; // frame pixel 0
        const float mx0 = static_cast<float>(mx & 0xFFFFu) - 8.0f, mx1 = static_cast<float>(mx >> 16) + 9.0f;
        mirror = mx != 0u && now - s_mirrorSeen.load(std::memory_order_relaxed) < 10u && x0 - left >= mx0 && x1 - left <= mx1;
    }
    const uint32_t group = inPanel ? 0u : (mirror ? HudHideMirror : (bottom ? HudHideGauges : (side < 0 ? HudHideLeft : (side > 0 ? HudHideRight : 0u))));
    // GT3's "3 2 1 START" (the grid-start lights draw instead): the middle's 2D
    // pieces, while GS::setHudHideMiddle is on.
    if (hideMiddle && group == 0u && !inPanel && !bottom && !mirror)
        return false;
    if (hide & group)
    {
        // GT3_HUD_HIDE_LOG=<n> (diagnostic): the first n hidden primitives.
        static int hideLog = [] {
            const char *v = std::getenv("GT3_HUD_HIDE_LOG");
            return (v && *v) ? std::atoi(v) : 0;
        }();
        if (hideLog > 0)
        {
            --hideLog;
            std::fprintf(stderr, "[hud-hide] group %u x %.1f-%.1f y %.1f-%.1f prim %d tme %d tex %04x psm %02x fbp %03x\n", group, x0 - (centre - half4x3),
                         x1 - (centre - half4x3), y0 - top, y1 - top, static_cast<int>(batch.state.prim.type), batch.state.prim.tme ? 1 : 0,
                         ctx.tex0.tbp0, ctx.tex0.psm, fbp);
        }
        return false;
    }
    if (!squeeze)
        return true;
    if (!anchor || side == 0 || mirror || panelUp)
    {
        squeezeX();
        return true;
    }
    const float aspect = (4.0f / 3.0f) / factor;
    const float middle = s_hudMiddle.load(std::memory_order_relaxed);
    const float reach = half4x3 * (aspect <= 2.5f ? 1.0f : (middle > 0.0f ? middle : (16.0f / 9.0f) / aspect)); // the anchor edge's distance from the middle
    const float edge4x3 = centre + side * half4x3, edge = centre + side * reach;
    for (uint32_t i = 0; i < batch.vertexCount; ++i)
    {
        float &x = batch.vertices[i].x;
        x = std::round((edge + (x - edge4x3) * factor) * 16.0f) / 16.0f;
    }
    return true;
#endif
}

// Opt-in diagnostic: GT3_GS_DRAW_LOG=<vsync tick>[,<tick>...] logs, for the first
// frame (Z24 depth-copy onset to the next onset) starting at or after each
// tick, every run of primitives that share the same GS state: state, count,
// screen bounds and the first vertex colour.
void GS::logFrameDraw(const GSPrimitiveBatch &batch)
{
    static const bool onRequest = [] {
        const char *v = std::getenv("GT3_GS_DRAW_LOG");
        return v && std::strncmp(v, "teal", 4) == 0;
    }();
    static const std::vector<uint64_t> ticks = [] {
        std::vector<uint64_t> out;
        if (const char *v = std::getenv("GT3_GS_DRAW_LOG"); v && std::strncmp(v, "teal", 4) != 0)
            for (const char *p = v; *p;)
            {
                char *end = nullptr;
                const unsigned long long t = std::strtoull(p, &end, 10);
                if (end == p)
                    break;
                out.push_back(t);
                p = (*end == ',') ? end + 1 : end;
            }
        std::sort(out.begin(), out.end());
        return out;
    }();
    static const bool rangeMode = [] {
        const char *v = std::getenv("GT3_GS_DRAW_LOG");
        return v && std::strncmp(v, "range:", 6) == 0;
    }();
    if (ticks.empty() && !onRequest && !rangeMode)
        return;
    static size_t next = 0;
    static bool logging = false;
    static bool inDepthCopy = false;
    static std::string groupKey;
    static uint32_t groupCount = 0u;
    static float gx0 = 0.0f, gy0 = 0.0f, gx1 = 0.0f, gy1 = 0.0f;
    static uint32_t gz0 = 0u, gz1 = 0u;
    static std::vector<uint32_t> groupRgba;
    static uint64_t groupSeq = 0u;
#if !defined(PS2X_GAME_GT3)
    // GT4 (mipmap task T0, analysis/phase5/specs/mipmap-design.md 4.2): the
    // group's |Q| range over its STQ-textured vertices, and the GS LOD and mip
    // levels that range selects with the group's TEX1 (LOD = (log2(1/|Q|) << L)
    // + K, or K with LCM; levels as section 3.3), to see whether levels above 0
    // are ever used. Diagnostic only: drawing still uses level 0.
    static bool groupHasQ = false;
    static float gq0 = 0.0f, gq1 = 0.0f;
    static uint64_t groupTex1 = 0u;
#endif
    const auto flushGroup = [] {
        if (groupCount != 0u)
        {
            std::string colours;
            char one[16];
            for (const uint32_t c : groupRgba)
            {
                std::snprintf(one, sizeof(one), "%s%08x", colours.empty() ? "" : ",", c);
                colours += one;
            }
            char mip[160] = "";
#if !defined(PS2X_GAME_GT3)
            if (groupHasQ)
            {
                const uint64_t t1 = groupTex1;
                const bool lcm = (t1 & 1u) != 0u;
                const int mxl = std::min(static_cast<int>((t1 >> 2) & 7u), 6);
                const int mmin = static_cast<int>((t1 >> 6) & 7u);
                const int l = static_cast<int>((t1 >> 19) & 3u);
                const int kRaw = static_cast<int>((t1 >> 32) & 0xFFFu);
                const double k = static_cast<double>(kRaw >= 0x800 ? kRaw - 0x1000 : kRaw) / 16.0;
                const auto lodAt = [&](float q) { return lcm ? k : -std::log2(static_cast<double>(q)) * static_cast<double>(1 << l) + k; };
                const double lodLo = lodAt(gq1), lodHi = lodAt(gq0); // LOD falls as |Q| rises
                const bool mipCapable = mxl > 0 && mmin >= 2 && mmin <= 5;
                const bool trilinear = mmin == 3 || mmin == 5;
                // The lowest and highest level a LOD reads (two with a trilinear blend).
                const auto levels = [&](double lod, int &lo, int &hi) {
                    lo = hi = 0;
                    if (!mipCapable || lod <= 0.0)
                        return;
                    const double c = std::min(lod, static_cast<double>(mxl));
                    if (trilinear)
                    {
                        lo = static_cast<int>(std::floor(c));
                        hi = (c > std::floor(c) && lo < mxl) ? lo + 1 : lo;
                    }
                    else
                        lo = hi = std::min(static_cast<int>(std::floor(c + 0.5)), mxl);
                };
                int loA = 0, loB = 0, hiA = 0, hiB = 0;
                levels(lodLo, loA, loB);
                levels(lodHi, hiA, hiB);
                std::snprintf(mip, sizeof(mip), " q=%g..%g lod=%.3f..%.3f lvl=%d..%d", gq0, gq1, lodLo, lodHi, loA, std::max(loB, hiB));
            }
            groupHasQ = false;
#endif
            std::fprintf(stderr, "[gs-draw] #%llu n=%u xy=%.0f,%.0f..%.0f,%.0f zv=%x..%x rgba=%s %s%s\n",
                         static_cast<unsigned long long>(groupSeq), groupCount, gx0, gy0, gx1, gy1, gz0, gz1, colours.c_str(),
                         groupKey.c_str(), mip);
        }
        groupCount = 0u;
        groupRgba.clear();
        groupKey.clear();
    };

    const GSContext &ctx = batch.state.context;
    const bool depthCopy = ctx.frame.psm == GS_PSM_Z24;
    const uint64_t tick = m_privRegs ? m_privRegs->vsyncTick.load() : 0u;
    // GT3_GS_DRAW_LOG=range:<first tick>,<ticks>: every draw in those VSyncs
    // (menus have no depth-copy pass to mark their frames).
    static const std::pair<uint64_t, uint64_t> range = [] {
        const char *v = std::getenv("GT3_GS_DRAW_LOG");
        unsigned long long first = 0, count = 0;
        if (v && std::sscanf(v, "range:%llu,%llu", &first, &count) == 2)
            return std::pair<uint64_t, uint64_t>{first, first + count};
        return std::pair<uint64_t, uint64_t>{0u, 0u};
    }();
    if (range.second != 0u)
    {
        const bool inRange = tick >= range.first && tick < range.second;
        if (inRange && !logging)
            std::fprintf(stderr, "[gs-draw] range start tick=%llu\n", static_cast<unsigned long long>(tick));
        if (!inRange && logging)
        {
            flushGroup();
            std::fprintf(stderr, "[gs-draw] range end tick=%llu\n", static_cast<unsigned long long>(tick));
        }
        logging = inRange;
    }
    else if (depthCopy && !inDepthCopy)
    {
        if (logging)
        {
            flushGroup();
            std::fprintf(stderr, "[gs-draw] frame end tick=%llu\n", static_cast<unsigned long long>(tick));
            logging = false;
        }
        // A request (tick + 1) is honoured by the next frame within 30 VSyncs.
        const uint64_t requestTick = onRequest ? m_drawLogRequestTick.exchange(0u, std::memory_order_relaxed) : 0u;
        const bool requested = requestTick != 0u && tick + 1u >= requestTick && tick + 1u - requestTick <= 30u;
        if (requested || (next < ticks.size() && tick >= ticks[next]))
        {
            while (next < ticks.size() && tick >= ticks[next])
                ++next;
            logging = true;
            std::fprintf(stderr, "[gs-draw] frame start tick=%llu\n", static_cast<unsigned long long>(tick));
        }
    }
    inDepthCopy = depthCopy;
    if (!logging)
        return;

    const GSTex0Reg &t = ctx.tex0;
    const GSPrimReg &pr = batch.state.prim;
    char key[512];
    std::snprintf(key, sizeof(key),
                  "prim=%u tme=%u abe=%u fge=%u iip=%u fst=%u frame=%x/%u/%x/msk%x z=%x/%x/m%u test=%llx alpha=%llx "
                  "tex0=%x/%u/%x/%ux%u tcc%u tfx%u cbp=%x/%x/csm%u/csa%u/cld%u tex1=%llx clamp=%llx texa=%x/%u/%x "
                  "sc=%u,%u..%u,%u ofs=%u,%u pabe=%u fba=%llx dthe=%llx colclamp=%llx",
                  static_cast<unsigned>(pr.type), pr.tme, pr.abe, pr.fge, pr.iip, pr.fst, ctx.frame.fbp, ctx.frame.fbw,
                  ctx.frame.psm, ctx.frame.fbmsk, ctx.zbuf.zbp, ctx.zbuf.psm, ctx.zbuf.zmask,
                  static_cast<unsigned long long>(ctx.test), static_cast<unsigned long long>(ctx.alpha), t.tbp0, t.tbw, t.psm,
                  1u << t.tw, 1u << t.th, t.tcc, t.tfx, t.cbp, t.cpsm, t.csm, t.csa, t.cld,
                  static_cast<unsigned long long>(ctx.tex1), static_cast<unsigned long long>(ctx.clamp), batch.state.texa.ta0,
                  batch.state.texa.aem, batch.state.texa.ta1, ctx.scissor.x0, ctx.scissor.y0, ctx.scissor.x1, ctx.scissor.y1,
                  ctx.xyoffset.ofx, ctx.xyoffset.ofy, batch.state.pabe, static_cast<unsigned long long>(ctx.fba),
                  static_cast<unsigned long long>(batch.state.dthe), static_cast<unsigned long long>(batch.state.colclamp));
#if !defined(PS2X_GAME_GT3)
    // GT4: the context's mip base pointers MIPTBP1/2 (mipmap T0).
    {
        const size_t used = std::strlen(key);
        std::snprintf(key + used, sizeof(key) - used, " mip=%llx/%llx", static_cast<unsigned long long>(ctx.miptbp1),
                      static_cast<unsigned long long>(ctx.miptbp2));
    }
#endif
    if (groupKey != key)
    {
        flushGroup();
        groupKey = key;
#if !defined(PS2X_GAME_GT3)
        groupTex1 = ctx.tex1;
#endif
        groupSeq = m_writeSeq;
        const GSVertex &v0 = batch.vertices[0];
        gx0 = gx1 = v0.x;
        gy0 = gy1 = v0.y;
        gz0 = gz1 = static_cast<uint32_t>(v0.z);
    }
    ++groupCount;
#if !defined(PS2X_GAME_GT3)
    if (pr.tme && !pr.fst)
        for (uint32_t i = 0; i < batch.vertexCount; ++i)
        {
            // |Q| as the samplers take it (fabsQ: |Q| <= 1e-8 counts as 1).
            float q = std::fabs(batch.vertices[i].q);
            if (!(q > 1e-8f))
                q = 1.0f;
            if (!groupHasQ)
            {
                gq0 = gq1 = q;
                groupHasQ = true;
            }
            gq0 = std::min(gq0, q);
            gq1 = std::max(gq1, q);
        }
#endif
    for (uint32_t i = 0; i < batch.vertexCount; ++i)
    {
        const GSVertex &v = batch.vertices[i];
        const uint32_t c = static_cast<uint32_t>(v.r) | (static_cast<uint32_t>(v.g) << 8) | (static_cast<uint32_t>(v.b) << 16) |
                           (static_cast<uint32_t>(v.a) << 24);
        if (groupRgba.size() < 6u && std::find(groupRgba.begin(), groupRgba.end(), c) == groupRgba.end())
            groupRgba.push_back(c);
        gx0 = std::min(gx0, batch.vertices[i].x);
        gx1 = std::max(gx1, batch.vertices[i].x);
        gy0 = std::min(gy0, batch.vertices[i].y);
        gy1 = std::max(gy1, batch.vertices[i].y);
        gz0 = std::min(gz0, static_cast<uint32_t>(batch.vertices[i].z));
        gz1 = std::max(gz1, static_cast<uint32_t>(batch.vertices[i].z));
    }
}

void GS::noteDrawWrites(const GSPrimitiveBatch &batch)
{
    const GSContext &ctx = batch.state.context;
    if (batch.vertexCount == 0u)
        return;
    float minX = batch.vertices[0].x, maxX = minX, minY = batch.vertices[0].y, maxY = minY;
    for (uint32_t i = 1; i < batch.vertexCount; ++i)
    {
        minX = std::min(minX, batch.vertices[i].x);
        maxX = std::max(maxX, batch.vertices[i].x);
        minY = std::min(minY, batch.vertices[i].y);
        maxY = std::max(maxY, batch.vertices[i].y);
    }
    const float ofx = static_cast<float>(ctx.xyoffset.ofx) / 16.0f, ofy = static_cast<float>(ctx.xyoffset.ofy) / 16.0f;
    const int x0 = std::max(static_cast<int>(std::floor(minX - ofx)), static_cast<int>(ctx.scissor.x0));
    const int y0 = std::max(static_cast<int>(std::floor(minY - ofy)), static_cast<int>(ctx.scissor.y0));
    const int x1 = std::min(static_cast<int>(std::ceil(maxX - ofx)), static_cast<int>(ctx.scissor.x1));
    const int y1 = std::min(static_cast<int>(std::ceil(maxY - ofy)), static_cast<int>(ctx.scissor.y1));
    ++m_writeSeq;
    if (ctx.frame.fbmsk != 0xFFFFFFFFu)
    {
        noteWrittenRect(ctx.frame.fbp, ctx.frame.fbw, ctx.frame.psm, x0, y0, x1, y1);
#if defined(PS2X_GAME_GT3)
        const bool phaseArea = true;
#else
        // A draw that changes no displayed colour says nothing about its field. GT4 clears
        // the alpha byte of each CT24 display field (FBMSK 0x00FFFFFF, a no-op on CT24) at
        // OFY fraction 8, which made both fields "odd" in the movie and menus, so the GL
        // presenter bobbed them (analysis/phase5/specs/step3-compare.md 4.1).
        const uint32_t rgb = (ctx.frame.psm & 0x2u) ? 0x00F8F8F8u : 0x00FFFFFFu; // CT16/CT16S: 5 bits each
        const bool phaseArea = (ctx.frame.fbmsk & rgb) != rgb;
#endif
        if (phaseArea && x1 >= x0 && y1 >= y0)
        {
            auto &area = m_fbpPhaseArea[ctx.frame.fbp % kGsPageCount][ctx.xyoffset.ofy & 0xFu];
            area = std::min<uint64_t>(0xFFFFFFFFu, uint64_t(area) + uint64_t(x1 - x0 + 1) * uint64_t(y1 - y0 + 1));
        }
    }
    if (((ctx.test >> 16) & 1u) != 0u && !ctx.zbuf.zmask)
        noteWrittenRect(ctx.zbuf.zbp, ctx.frame.fbw, ctx.zbuf.psm, x0, y0, x1, y1);
}

uint64_t GS::lastWriteSeqUnlocked(const std::vector<uint32_t> &pages) const
{
    uint64_t last = 0u;
    for (const uint32_t page : pages)
        last = std::max(last, m_pageWriteSeq[page]);
    return last;
}

std::vector<uint32_t> GS::displayPagesUnlocked(const GSPresentationRequest &request) const
{
    std::vector<uint32_t> pages;
    const bool halfHeight = (request.smode2 & 3u) == 3u; // interlaced FFMD: the buffer holds one field
    for (int circuit = 0; circuit < 2; ++circuit)
    {
        if (((request.pmode >> circuit) & 1u) == 0u)
            continue;
        const uint64_t dispfb = circuit ? request.dispfb2 : request.dispfb1;
        const uint64_t display = circuit ? request.display2 : request.display1;
        const uint32_t fbp = static_cast<uint32_t>(dispfb & 0x1FFu);
        const uint32_t fbw = std::max<uint32_t>(1u, static_cast<uint32_t>((dispfb >> 9) & 0x3Fu));
        const uint8_t psm = static_cast<uint8_t>((dispfb >> 15) & 0x1Fu);
        const int dbx = static_cast<int>((dispfb >> 32) & 0x7FFu), dby = static_cast<int>((dispfb >> 43) & 0x7FFu);
        const uint32_t magh = static_cast<uint32_t>((display >> 23) & 0xFu);
        uint32_t width = static_cast<uint32_t>(((display >> 32) & 0xFFFu) + 1u) / (magh + 1u);
        uint32_t height = static_cast<uint32_t>(((display >> 44) & 0x7FFu) + 1u);
        if (halfHeight)
            height = (height + 1u) / 2u;
        uint32_t pageWidth = 64u, pageHeight = 32u;
        gsPageGeometry(psm, pageWidth, pageHeight);
        const uint32_t pagesPerRow = std::max<uint32_t>(1u, (fbw * 64u) / pageWidth);
        width = std::min(width, pagesPerRow * pageWidth);
        for (uint32_t row = static_cast<uint32_t>(dby) / pageHeight; row <= (dby + height - 1u) / pageHeight; ++row)
            for (uint32_t col = static_cast<uint32_t>(dbx) / pageWidth; col <= std::min<uint32_t>((dbx + width - 1u) / pageWidth, pagesPerRow - 1u); ++col)
                pages.push_back((fbp + row * pagesPerRow + col) % kGsPageCount);
    }
    return pages;
}

size_t GS::recentFrameGapsMs(float *out, size_t max) const
{
    std::lock_guard<std::mutex> lock(m_flipMutex);
    const size_t n = std::min(m_flipCount, m_flipTimesNs.size());
    size_t written = 0;
    for (size_t k = 1; k < n && written < max; ++k)
    {
        const int64_t newer = m_flipTimesNs[(m_flipCount - k) % m_flipTimesNs.size()];
        const int64_t older = m_flipTimesNs[(m_flipCount - 1u - k) % m_flipTimesNs.size()];
        out[written++] = static_cast<float>(static_cast<double>(newer - older) / 1e6);
    }
    return written;
}

GS::FrameStats GS::frameStats() const
{
    FrameStats stats;
    const int64_t now = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
    std::lock_guard<std::mutex> lock(m_flipMutex);
    const size_t n = std::min(m_flipCount, m_flipTimesNs.size());
    if (n == 0u)
        return stats;
    // Flips in the last second (newest first), and the gaps between them.
    int frames = 0;
    double sumMs = 0.0, maxMs = 0.0;
    int64_t newer = m_flipTimesNs[(m_flipCount - 1u) % m_flipTimesNs.size()];
    if (now - newer > 1000000000ll)
        return stats; // nothing new for a second
    for (size_t k = 1; k < n; ++k)
    {
        const int64_t t = m_flipTimesNs[(m_flipCount - 1u - k) % m_flipTimesNs.size()];
        if (now - t > 1000000000ll)
            break;
        const double gap = static_cast<double>(newer - t) / 1e6;
        sumMs += gap;
        maxMs = std::max(maxMs, gap);
        ++frames;
        newer = t;
    }
    if (frames > 0)
    {
        stats.avgMs = sumMs / frames;
        stats.maxMs = maxMs;
        stats.fps = 1000.0 / stats.avgMs;
    }
    return stats;
}

void GS::latchPresentationSnapshot(const DisplaySnapshot &snapshot)
{
    {
        // A flip: the displayed buffer (read circuit 2 if enabled, else 1) changed.
        const uint64_t dispfb = (snapshot.pmode & 2u) ? snapshot.dispfb2 : snapshot.dispfb1;
        const uint32_t fbp = static_cast<uint32_t>(dispfb & 0x1FFu);
        if (fbp != m_lastFlipFbp)
        {
            m_lastFlipFbp = fbp;
            std::lock_guard<std::mutex> lock(m_flipMutex);
            m_flipTimesNs[m_flipCount % m_flipTimesNs.size()] =
                std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
            ++m_flipCount;
        }
    }
    m_snapshot = &snapshot;
    latchPresentation();
    m_snapshot = nullptr;
    m_publishedFrames.fetch_add(1u, std::memory_order_acq_rel);
    // The host loop's grid pacing waits for this (after the count, which
    // UploadFrame checks before it copies a frame).
    ps2x::vblank::presentationReady(snapshot.vsyncTick);
}

void GS::latchHostPresentationFrame()
{
    latchPresentation();
}

void GS::latchPresentation()
{
    GSPresentationRequest request{};
    {
        std::lock_guard<std::recursive_mutex> lock(m_stateMutex);
        if (!m_backend || !m_privRegs)
        {
            std::lock_guard<std::mutex> presentationLock(m_presentationMutex);
            m_hostPresentationFrame.clear();
            m_hasHostPresentationFrame = false;
            m_hostPresentationWidth = m_hostPresentationHeight = 0u;
            return;
        }
        request = buildPresentationRequestUnlocked();
    }
    // The latch's own tick (request.vsyncTick may become the shown buffer's
    // below): the GL presenter labels its GPU frame with it.
    ps2x::vblank::beginPresentation(request.vsyncTick);

    {
        // Opt-in diagnostic: GT3_GS_DISPLAY_LOG=1 logs the CRTC setup whenever it changes.
        static const bool displayLog = [] {
            const char *v = std::getenv("GT3_GS_DISPLAY_LOG");
            return v && *v == '1';
        }();
        static uint64_t last[7] = {~0ull, ~0ull, ~0ull, ~0ull, ~0ull, ~0ull, ~0ull};
        const uint64_t now[7] = {request.pmode, request.smode2, request.dispfb1, request.display1,
                                 request.dispfb2, request.display2,
                                 (static_cast<uint64_t>(request.preferredDestFbp) << 32) |
                                     (request.hasPreferredSource ? request.preferredSource.fbp : 0xffffffffu)};
        if (displayLog && !std::equal(std::begin(now), std::end(now), std::begin(last)))
        {
            std::copy(std::begin(now), std::end(now), std::begin(last));
            std::cerr << std::hex << "[gs-display] tick=" << std::dec << request.vsyncTick << std::hex
                      << " pmode=" << now[0] << " smode2=" << now[1] << " dispfb1=" << now[2] << " display1=" << now[3]
                      << " dispfb2=" << now[4] << " display2=" << now[5] << " preferred=" << now[6] << std::dec << std::endl;
        }
    }

    // While the game flips between display buffers, the buffer written most
    // recently may still be in progress (the game runs slower than the VSync that
    // flips it), so show the most recently displayed of the other buffers, woven
    // with its own field parity. GT3_PRESENT_HOLD=0 shows DISPFB as is.
    static const bool holdEnabled = [] {
        const char *v = std::getenv("GT3_PRESENT_HOLD");
        return !(v && *v == '0');
    }();
    static const bool presentLog = [] {
        const char *v = std::getenv("GT3_PRESENT_LOG");
        return v && *v == '1';
    }();
    constexpr uint64_t kDisplayRegionTicks = 600u; // a buffer counts as flipped-to for 10 s of VSyncs
    {
        std::lock_guard<std::recursive_mutex> lock(m_stateMutex);
        ++m_latchCount;
#if !defined(PS2X_GAME_GT3)
        // A video mode switch (GT4: 480i menus <-> a 480p / 1080i race):
        // the display buffers and field phases remembered so far belong to
        // the old mode, so one of them could be shown as "the other buffer"
        // with the new mode's registers (VIDEO-MODES.md 3.5). The signature:
        // the video mode, SMODE2, and the shown circuit's DISPLAY size and
        // DISPFB format; kept while both circuits are off (GT4 blanks the
        // picture while it switches). PS2X_MODE_SWITCH_RESET=0: off.
        {
            static const bool resetOnSwitch = [] {
                const char *v = std::getenv("PS2X_MODE_SWITCH_RESET");
                return !(v && *v == '0');
            }();
            if (resetOnSwitch && (request.pmode & 3u) != 0u)
            {
                const bool c2 = (request.pmode & 2u) != 0u;
                const uint64_t display = c2 ? request.display2 : request.display1;
                const uint64_t dispfb = c2 ? request.dispfb2 : request.dispfb1;
                const uint64_t signature = (static_cast<uint64_t>(request.crt & 0x3FFu)) | ((request.smode2 & 3u) << 10) |
                                           (((display >> 32) & 0xFFFu) << 12) | (((display >> 44) & 0x7FFu) << 24) |
                                           (((display >> 23) & 0xFu) << 35) | (((dispfb >> 9) & 0x3Fu) << 39) |
                                           (((dispfb >> 15) & 0x1Fu) << 45);
                if (signature != m_displaySignature)
                {
                    static int switchLogs = 0;
                    if (m_displaySignature != ~0ull && switchLogs++ < 200)
                        std::fprintf(stderr, "[gs-mode] display setup changed at vsync %llu (%s, smode2 %llx, %llux%llu, psm %llx): %zu remembered "
                                             "display buffers and the field phases dropped\n",
                                     static_cast<unsigned long long>(request.vsyncTick), ps2x::crt::name(request.crt),
                                     static_cast<unsigned long long>(request.smode2 & 3u),
                                     static_cast<unsigned long long>((((display >> 32) & 0xFFFu) + 1u) / (((display >> 23) & 0xFu) + 1u)),
                                     static_cast<unsigned long long>(((display >> 44) & 0x7FFu) + 1u),
                                     static_cast<unsigned long long>((dispfb >> 15) & 0x1Fu), m_recentDisplayRegions.size());
                    m_displaySignature = signature;
                    m_recentDisplayRegions.clear();
                    for (auto &area : m_fbpPhaseArea)
                        area.fill(0u);
                    m_fbpPhase.fill(0xFFu);
                }
            }
        }
#endif
        std::vector<uint32_t> currentPages = displayPagesUnlocked(request);
        std::erase_if(m_recentDisplayRegions, [&request](const DisplayRegion &region) {
            return region.lastDisplayTick + kDisplayRegionTicks < request.vsyncTick || region.lastDisplayTick > request.vsyncTick;
        });
        // One entry per displayed buffer (its page set), whatever PMODE/DISPFB
        // combination selected it.
        auto current = std::find_if(m_recentDisplayRegions.begin(), m_recentDisplayRegions.end(),
                                    [&currentPages](const DisplayRegion &region) { return region.pages == currentPages; });
        if (current == m_recentDisplayRegions.end())
            current = m_recentDisplayRegions.insert(m_recentDisplayRegions.end(), DisplayRegion{});
        current->dispfb1 = request.dispfb1;
        current->dispfb2 = request.dispfb2;
        current->lastLatch = m_latchCount;
        current->lastDisplayTick = request.vsyncTick;
        current->pages = std::move(currentPages);

        const DisplayRegion *shown = &*current;
        if (holdEnabled && m_recentDisplayRegions.size() >= 2u)
        {
            const DisplayRegion *newest = nullptr;
            uint64_t newestWrite = 0u;
            for (const DisplayRegion &region : m_recentDisplayRegions)
            {
                const uint64_t last = lastWriteSeqUnlocked(region.pages);
                if (!newest || last > newestWrite)
                {
                    newest = &region;
                    newestWrite = last;
                }
            }
            if (newest == shown && newestWrite != 0u)
            {
                const DisplayRegion *other = nullptr;
                for (const DisplayRegion &region : m_recentDisplayRegions)
                    if (&region != newest && (!other || region.lastDisplayTick > other->lastDisplayTick))
                        other = &region;
                if (other)
                    shown = other;
            }
        }
        if (presentLog)
            std::cerr << "[present] tick=" << request.vsyncTick << " dispfb2=0x" << std::hex << (request.dispfb2 & 0x1FFu)
                      << " shown=0x" << (shown->dispfb2 & 0x1FFu) << std::dec << " shownTick=" << shown->lastDisplayTick
                      << " regions=" << m_recentDisplayRegions.size();
        if (presentLog)
        {
            for (const DisplayRegion &region : m_recentDisplayRegions)
                std::cerr << " [0x" << std::hex << (region.pages.empty() ? 0u : region.pages.front()) << std::dec << " n="
                          << region.pages.size() << " w=" << lastWriteSeqUnlocked(region.pages) << " t=" << region.lastDisplayTick << "]";
            std::cerr << " seq=" << m_writeSeq << std::endl;
        }
        request.dispfb1 = shown->dispfb1;
        request.dispfb2 = shown->dispfb2;
        request.vsyncTick = shown->lastDisplayTick;
        {
            // Field phase of the shown buffer: the fractional OFY covering the
            // most drawn area since it was last shown (GT3 alternates a half
            // line per field); kept while the same field is shown again.
            const bool crt2 = (request.pmode & 2u) != 0u;
            const uint32_t fbp = static_cast<uint32_t>((crt2 ? shown->dispfb2 : shown->dispfb1) & 0x1FFu) % kGsPageCount;
            auto &area = m_fbpPhaseArea[fbp];
            static const uint64_t fieldLogFrom = [] {
                const char *v = std::getenv("GT3_FIELD_LOG");
                return v && *v ? std::strtoull(v, nullptr, 10) : ~0ull;
            }();
            static int fieldLogLeft = 120;
            if (request.vsyncTick >= fieldLogFrom && fieldLogLeft > 0 &&
                std::any_of(area.begin(), area.end(), [](uint32_t a) { return a != 0u; }))
            {
                --fieldLogLeft;
                // One write per line (other threads' lines landed inside it:
                // analysis/phase5/specs/step6-coverage.md B7). Same text.
                char line[512];
                size_t used = 0;
                auto advance = [&](int n) {
                    if (n > 0)
                        used = std::min(sizeof(line) - 1u, used + static_cast<size_t>(n));
                };
                advance(std::snprintf(line, sizeof(line), "[field-area] tick=%llu fbp=0x%x", static_cast<unsigned long long>(request.vsyncTick), fbp));
                for (size_t i = 0; i < area.size(); ++i)
                    if (area[i])
                        advance(std::snprintf(line + used, sizeof(line) - used, " %zu:%u", i, area[i]));
                advance(std::snprintf(line + used, sizeof(line) - used, "\n"));
                std::fputs(line, stderr);
            }
            // GT3's odd field draws its scene at fractions 8..14 (a half line
            // plus sub-pixel jitter), the even one at 0..6; full-screen passes
            // at 0 cover most of both. Odd: over a fifth of the area at 8..15.
            uint64_t total = 0u, half = 0u;
            for (size_t i = 0; i < area.size(); ++i)
            {
                total += area[i];
                if (i & 8u)
                    half += area[i];
            }
#if defined(PS2X_GAME_GT3)
            if (total)
                m_fbpPhase[fbp] = half * 5u > total ? 8u : 0u;
#else
            // GT4 shifts a field a quarter field line off the half-pixel centre:
            // OFY fraction 4 / 12 (movie, menus, car select), 6 / 10 plus jitter
            // (race). Fractions 0 and 8 carry no field: 1:1 copies, clears, HUD
            // and colour grading, and the car select's 3D car, which GT4 renders
            // in FBW-2 scratch tiles inside the display buffer at 8 (~40% of the
            // area, so both buffers came out "odd" and were bobbed;
            // analysis/phase5/specs/step4-picture.md 2). A field with only 0/8
            // draws has no phase (0xFF): the presenter pairs it by VSync, as the
            // hardware shows it. PS2X_FIELD_NEUTRAL=0: the rule above (GT3's).
            static const bool neutral08 = [] {
                const char *v = std::getenv("PS2X_FIELD_NEUTRAL");
                return !(v && *v == '0');
            }();
            // 1080i's 480-line fields follow the same rule (measured,
            // analysis/design/video-modes results vm-1080i-b: the race's 3D at
            // fractions 4 / 12 with 1-7 / 9-15 jitter, a full-field pass at 8
            // in both buffers), so it needs no rule of its own.
            if (total && !neutral08)
                m_fbpPhase[fbp] = half * 5u > total ? 8u : 0u;
            else if (total)
            {
                uint64_t evenArea = 0u, oddArea = 0u;
                for (size_t i = 1; i < 8u; ++i)
                {
                    evenArea += area[i];
                    oddArea += area[i + 8u];
                }
                m_fbpPhase[fbp] = oddArea > evenArea ? 8u : (evenArea > oddArea ? 0u : 0xFFu);
            }
#endif
            area.fill(0u);
            request.fieldPhase = m_fbpPhase[fbp];
            request.fieldSeq = lastWriteSeqUnlocked(shown->pages);
        }
        request.allowWeave = m_lastPresentedTick + 1u == shown->lastDisplayTick;
        m_lastPresentedTick = shown->lastDisplayTick;
    }

    PresentationFrame frame{};
    {
        std::lock_guard<std::mutex> backendLock(m_backendLifetimeMutex);
        if (m_backend)
        {
            m_backend->Flush();
            m_backend->Sync(GSSyncReason::Presentation);
            frame = m_backend->Present(request);
            frame.crt = request.crt;
#if !defined(PS2X_GAME_GT3)
            // (Picture track review) The frame's FFMD bit as the latched SMODE2 has it: GT4's
            // Race Monitor writes SMODE2 = 1 (interlaced, FFMD 0: one full-height buffer read
            // every other line) without calling SetGsCrt, whose FFMD still says 1 (one
            // half-height field per VSync). The host blends the lines (Blend) only for a picture
            // woven from such fields (ps2_runtime.cpp UploadFrame, g_shownInterlaced).
            if (ps2x::crt::known(frame.crt) && (request.smode2 & 1ull) != 0ull)
                frame.crt = (frame.crt & ~0x200u) | (static_cast<uint32_t>((request.smode2 >> 1) & 1ull) << 9);
#endif
        }
    }
    publishPresentationFrame(std::move(frame));
}

void GS::publishPresentationFrame(PresentationFrame frame)
{
    const bool hasFrame = static_cast<bool>(frame);
    const uint32_t displayFbp = frame.displayFbp;
    const uint32_t sourceFbp = frame.sourceFbp;
    const uint32_t width = frame.width;
    const uint32_t height = frame.height;
    const bool usedPreferred = frame.usedPreferred;
    const uint32_t crt = frame.crt;
    {
        std::lock_guard<std::mutex> presentationLock(m_presentationMutex);
        m_hostPresentationCrt = crt;
        m_hostPresentationFrame = std::move(frame.pixels);
        m_hostPresentationWidth = width;
        m_hostPresentationHeight = height;
        m_hostPresentationDisplayFbp = displayFbp;
        m_hostPresentationSourceFbp = sourceFbp;
        m_hostPresentationUsedPreferred = usedPreferred;
        m_hasHostPresentationFrame = hasFrame;
    }

    if (hasFrame)
    {
        std::lock_guard<std::recursive_mutex> lock(m_stateMutex);
        recordPresentDebugEventUnlocked(displayFbp, sourceFbp, width, height, usedPreferred);
    }
}

bool GS::copyLatchedHostPresentationFrame(std::vector<uint8_t> &outPixels,
                                          uint32_t &outWidth,
                                          uint32_t &outHeight,
                                          uint32_t *outDisplayFbp,
                                          uint32_t *outSourceFbp,
                                          bool *outUsedPreferred,
                                          uint32_t *outCrt) const
{
    std::lock_guard<std::mutex> lock(m_presentationMutex);
    if (outCrt)
        *outCrt = m_hostPresentationCrt;
    if (!m_hasHostPresentationFrame || m_hostPresentationFrame.empty())
    {
        outPixels.clear();
        outWidth = 0u;
        outHeight = 0u;
        if (outDisplayFbp)
            *outDisplayFbp = 0u;
        if (outSourceFbp)
            *outSourceFbp = 0u;
        if (outUsedPreferred)
            *outUsedPreferred = false;
        return false;
    }

    outWidth = m_hostPresentationWidth;
    outHeight = m_hostPresentationHeight;
    if (outDisplayFbp)
        *outDisplayFbp = m_hostPresentationDisplayFbp;
    if (outSourceFbp)
        *outSourceFbp = m_hostPresentationSourceFbp;
    if (outUsedPreferred)
        *outUsedPreferred = m_hostPresentationUsedPreferred;

    const size_t packedRowBytes = static_cast<size_t>(outWidth) * 4u;
    outPixels.resize(packedRowBytes * static_cast<size_t>(outHeight));
    if (outWidth != 0u && outHeight != 0u)
    {
        const size_t sourceRowBytes = static_cast<size_t>(kHostFrameWidth) * 4u;
        for (uint32_t y = 0; y < outHeight; ++y)
        {
            const size_t srcOffset = static_cast<size_t>(y) * sourceRowBytes;
            const size_t dstOffset = static_cast<size_t>(y) * packedRowBytes;
            if (srcOffset + packedRowBytes > m_hostPresentationFrame.size() ||
                dstOffset + packedRowBytes > outPixels.size())
            {
                outPixels.clear();
                outWidth = 0u;
                outHeight = 0u;
                if (outDisplayFbp)
                    *outDisplayFbp = 0u;
                if (outSourceFbp)
                    *outSourceFbp = 0u;
                if (outUsedPreferred)
                    *outUsedPreferred = false;
                return false;
            }

            std::memcpy(outPixels.data() + dstOffset,
                        m_hostPresentationFrame.data() + srcOffset,
                        packedRowBytes);
        }
    }
    return true;
}

void GS::processGIFPacket(const uint8_t *data, uint32_t sizeBytes)
{
    std::lock_guard<std::recursive_mutex> lock(m_stateMutex);
    if (!data || sizeBytes < 16 || !m_backend)
        return;

    if (!m_gifTag.active && tryProcessNativeImageUploadPacket(data, sizeBytes))
        return;

    PS2_IF_AGRESSIVE_LOGS({
        const uint32_t packetIndex = s_debugGifPacketCount.fetch_add(1, std::memory_order_relaxed);
        if (packetIndex < 48u)
        {
            const uint64_t tagLo = loadLE64(data);
            const uint32_t nloop = static_cast<uint32_t>(tagLo & 0x7FFFu);
            const uint8_t flg = static_cast<uint8_t>((tagLo >> 58) & 0x3u);
            uint32_t nreg = static_cast<uint32_t>((tagLo >> 60) & 0xFu);
            if (nreg == 0u)
                nreg = 16u;
            RUNTIME_LOG("[gs:gif] idx=" << packetIndex
                                        << " size=" << sizeBytes
                                        << " nloop=" << nloop
                                        << " flg=" << static_cast<uint32_t>(flg)
                                        << " nreg=" << nreg
                                        << " ctx0fbp=" << m_ctx[0].frame.fbp
                                        << " ctx1fbp=" << m_ctx[1].frame.fbp
                                        << std::endl);
        }
    });

    uint32_t offset = 0;
    GifTagProgress &t = m_gifTag;
    while (offset < sizeBytes)
    {
        if (!t.active)
        {
            if (offset + 16 > sizeBytes)
                return;
            uint64_t tagLo = loadLE64(data + offset);
            uint64_t tagHi = loadLE64(data + offset + 8);
            offset += 16;

            m_curQ = 1.0f;
#if defined(PS2X_GAME_GT4) && !defined(GT4_STOCK_BUILD)
            {
                // GT4's VU1 tap marks its packets in the tag's unused bits 16-31 (gt4_vu1_tap.cpp).
                static const bool tapOn = [] {
                    const char *v = std::getenv("GT4_VU1_TAP");
                    const char *up = std::getenv("GT4_UPSCALER"); // (DLSS turns the tap on, as gt4tap::enabled)
                    if ((!v || !*v) && up && (std::strcmp(up, "dlss") == 0 || std::strcmp(up, "fsr") == 0 || std::strcmp(up, "fsr4") == 0))
                        return true;
                    const char *lit = std::getenv("GT4_CAR_LIGHTING"); // (per-pixel car lighting too)
                    if ((!v || !*v) && lit && (std::strcmp(lit, "pixel") == 0 || std::strcmp(lit, "split") == 0 || std::strcmp(lit, "show") == 0))
                        return true;
                    const char *ao = std::getenv("GT4_AO"); // (ambient occlusion too)
                    if ((!v || !*v) && ao && *ao && std::strcmp(ao, "0") != 0 && std::strcmp(ao, "off") != 0)
                        return true;
                    const char *ssr = std::getenv("GT4_SSR"); // (and screen-space reflections)
                    if ((!v || !*v) && ssr && *ssr && std::strcmp(ssr, "0") != 0 && std::strcmp(ssr, "off") != 0)
                        return true;
                    const char *rl = std::getenv("GT4_RELIGHT"); // (and relighting)
                    if ((!v || !*v) && rl && *rl && std::strcmp(rl, "0") != 0 && std::strcmp(rl, "off") != 0)
                        return true;
                    const char *sh = std::getenv("GT4_SHADOWS"); // (and sun shadows)
                    if ((!v || !*v) && sh && *sh && std::strcmp(sh, "0") != 0 && std::strcmp(sh, "off") != 0)
                        return true;
                    for (const char *name : {"GT4_SUN_GLOW", "GT4_BLOOM"}) // (sun glow, bloom)
                    {
                        const char *e = std::getenv(name);
                        if ((!v || !*v) && e && *e && std::strcmp(e, "0") != 0 && std::strcmp(e, "off") != 0)
                            return true;
                    }
                    return v && *v && std::strcmp(v, "0") != 0 && std::strcmp(v, "off") != 0;
                }();
                m_tapSlot = tapOn ? static_cast<uint16_t>((tagLo >> 16) & 0xFFFFu) : 0u;
                m_tapK = 0;
            }
#endif

            uint32_t nloop = static_cast<uint32_t>(tagLo & 0x7FFF);
            uint8_t flg = static_cast<uint8_t>((tagLo >> 58) & 0x3);
            uint32_t nreg = static_cast<uint32_t>((tagLo >> 60) & 0xF);
            if (nreg == 0)
                nreg = 16;

            recordGifTagDebugEventUnlocked(sizeBytes, nloop, flg, nreg);

            bool pre = ((tagLo >> 46) & 1) != 0;
            if (pre)
            {
                writeRegisterUnlocked(GS_REG_PRIM, (tagLo >> 47) & 0x7FF);
            }

            t.flg = (flg == 3) ? GIF_FMT_IMAGE : flg; // FLG 3 behaves as IMAGE
            t.nloopLeft = nloop;
            t.nreg = nreg;
            t.regIndex = 0;
            t.regListOdd = (flg == GIF_FMT_REGLIST) && ((nloop * nreg) & 1u);
            for (uint32_t i = 0; i < nreg; ++i)
                t.regs[i] = static_cast<uint8_t>((tagHi >> (i * 4)) & 0xF);
            t.active = nloop != 0;
            continue;
        }

        if (t.flg == GIF_FMT_PACKED)
        {
            while (t.nloopLeft && offset + 16 <= sizeBytes)
            {
                uint64_t lo = loadLE64(data + offset);
                uint64_t hi = loadLE64(data + offset + 8);
                offset += 16;
                writeRegisterPacked(t.regs[t.regIndex], lo, hi);
                if (++t.regIndex == t.nreg)
                {
                    t.regIndex = 0;
                    --t.nloopLeft;
                }
            }
            if (t.nloopLeft)
                return; // continues in the next submission
            t.active = false;
        }
        else if (t.flg == GIF_FMT_REGLIST)
        {
            while (t.nloopLeft && offset + 8 <= sizeBytes)
            {
                writeRegisterUnlocked(t.regs[t.regIndex], loadLE64(data + offset));
                offset += 8;
                if (++t.regIndex == t.nreg)
                {
                    t.regIndex = 0;
                    --t.nloopLeft;
                }
            }
            if (t.nloopLeft)
                return;
            if (t.regListOdd)
            {
                if (offset + 8 > sizeBytes)
                    return; // padding word arrives next
                offset += 8;
                t.regListOdd = false;
            }
            t.active = false;
        }
        else
        {
            const uint64_t pending = uint64_t(t.nloopLeft) * 16u;
            const uint32_t imageBytes = static_cast<uint32_t>(std::min<uint64_t>(pending, (sizeBytes - offset) & ~15u));
            if (imageBytes == 0)
                return;
            processImageData(data + offset, imageBytes);
            offset += imageBytes;
            t.nloopLeft -= imageBytes / 16u;
            if (t.nloopLeft)
                return;
            t.active = false;
        }
    }
}

bool GS::processNativePackedGIFPacket(const uint8_t *data, uint32_t sizeBytes)
{
    std::lock_guard<std::recursive_mutex> lock(m_stateMutex);
    if (!data || sizeBytes < 16u || !m_backend)
        return false;

    if (!validatePackedGifPacket(data, sizeBytes))
        return false;

    const bool processed = visitPackedGifPacket(data, sizeBytes, [&](const PackedGifPacketTag &tag)
                                                {
        m_curQ = 1.0f;

        recordGifTagDebugEventUnlocked(sizeBytes, tag.nloop, GIF_FMT_PACKED, tag.nreg);

        const bool pre = ((tag.lo >> 46u) & 1u) != 0u;
        if (pre)
            writeRegisterUnlocked(GS_REG_PRIM, (tag.lo >> 47u) & 0x7FFu);

        uint32_t offset = tag.payloadOffset;
        for (uint32_t loop = 0u; loop < tag.nloop; ++loop)
        {
            for (uint32_t r = 0u; r < tag.nreg; ++r)
            {
                const uint64_t lo = loadLE64(data + offset);
                const uint64_t hi = loadLE64(data + offset + 8u);
                offset += 16u;
                writeRegisterPacked(tag.regs[r], lo, hi);
            }
        }

        return true; });

    if (!processed)
        return false;

    ++m_nativePackedGIFPacketCount;
    return true;
}

void GS::uploadImageNative(uint64_t bitbltbuf,
                           uint64_t trxpos,
                           uint64_t trxreg,
                           uint64_t trxdir,
                           const uint8_t *data,
                           uint32_t sizeBytes)
{
    std::lock_guard<std::recursive_mutex> lock(m_stateMutex);
    uploadImageNativeUnlocked(bitbltbuf, trxpos, trxreg, trxdir, data, sizeBytes);
}

void GS::uploadImageNativeUnlocked(uint64_t bitbltbuf,
                                   uint64_t trxpos,
                                   uint64_t trxreg,
                                   uint64_t trxdir,
                                   const uint8_t *data,
                                   uint32_t sizeBytes)
{
    if (!data || sizeBytes == 0 || !m_backend)
        return;

    writeRegisterUnlocked(GS_REG_BITBLTBUF, bitbltbuf);
    writeRegisterUnlocked(GS_REG_TRXPOS, trxpos);
    writeRegisterUnlocked(GS_REG_TRXREG, trxreg);
    writeRegisterUnlocked(GS_REG_TRXDIR, trxdir);
    processImageData(data, sizeBytes);
    ++m_nativeImageUploadCount;
}

void GS::loadClutForCld(const GSTex0Reg &tex)
{
    // TEX0/TEX2 CLD: 0 keep, 1 load, 2/3 load and set CBP0/CBP1, 4/5 load
    // only if CBP differs from CBP0/CBP1 (then set it). 6/7 reserved.
    bool load = false;
    switch (tex.cld)
    {
    case 1: load = true; break;
    case 2: load = true; m_clutCbp0 = tex.cbp; break;
    case 3: load = true; m_clutCbp1 = tex.cbp; break;
    case 4: if (tex.cbp != m_clutCbp0) { load = true; m_clutCbp0 = tex.cbp; } break;
    case 5: if (tex.cbp != m_clutCbp1) { load = true; m_clutCbp1 = tex.cbp; } break;
    default: break;
    }
    if (load && m_backend)
        m_backend->LoadClut(tex, m_texclut);
}

bool GS::tryProcessNativeImageUploadPacket(const uint8_t *data, uint32_t sizeBytes)
{
    constexpr uint32_t kSetupRegisters = 4u;
    constexpr uint32_t kPackedAdPayloadBytes = kSetupRegisters * 16u;
    constexpr uint64_t kPackedAdDescriptor = 0x0Eull;

    if (!data || sizeBytes < 16u + kPackedAdPayloadBytes + 16u)
        return false;

    const uint64_t setupTagLo = loadLE64(data);
    const uint64_t setupTagHi = loadLE64(data + 8u);
    const uint32_t setupNloop = static_cast<uint32_t>(setupTagLo & 0x7FFFu);
    const uint8_t setupFlg = static_cast<uint8_t>((setupTagLo >> 58u) & 0x3u);
    uint32_t setupNreg = static_cast<uint32_t>((setupTagLo >> 60u) & 0xFu);
    if (setupNreg == 0u)
        setupNreg = 16u;

    if (setupNloop != kSetupRegisters ||
        setupFlg != GIF_FMT_PACKED ||
        setupNreg != 1u ||
        (setupTagHi & 0xFull) != kPackedAdDescriptor)
    {
        return false;
    }

    uint64_t regs[kSetupRegisters] = {};
    uint32_t offset = 16u;
    constexpr uint8_t expectedRegs[kSetupRegisters] = {
        GS_REG_BITBLTBUF,
        GS_REG_TRXPOS,
        GS_REG_TRXREG,
        GS_REG_TRXDIR,
    };

    for (uint32_t i = 0; i < kSetupRegisters; ++i)
    {
        regs[i] = loadLE64(data + offset);
        const uint64_t reg = loadLE64(data + offset + 8u);
        if ((reg & 0xFFu) != expectedRegs[i])
            return false;
        offset += 16u;
    }

    const uint32_t trxdirMode = static_cast<uint32_t>(regs[3] & 0x3ull);
    const uint32_t rrw = static_cast<uint32_t>(regs[2] & 0xFFFull);
    const uint32_t rrh = static_cast<uint32_t>((regs[2] >> 32u) & 0xFFFull);
    if (trxdirMode != 0u || rrw == 0u || rrh == 0u)
        return false;

    if (offset + 16u > sizeBytes)
        return false;

    const uint64_t imageTagLo = loadLE64(data + offset);
    const uint8_t imageFlg = static_cast<uint8_t>((imageTagLo >> 58u) & 0x3u);
    const uint32_t imageNloop = static_cast<uint32_t>(imageTagLo & 0x7FFFu);
    if (imageFlg != GIF_FMT_IMAGE || imageNloop == 0u)
        return false;

    offset += 16u;
    const uint64_t imageBytes64 = static_cast<uint64_t>(imageNloop) * 16ull;
    if (imageBytes64 > 0xFFFFFFFFull)
        return false;
    const uint32_t imageBytes = static_cast<uint32_t>(imageBytes64);
    if (offset + imageBytes != sizeBytes)
        return false;

    uploadImageNativeUnlocked(regs[0], regs[1], regs[2], regs[3], data + offset, imageBytes);
    return true;
}

void GS::writeRegisterPacked(uint8_t regDesc, uint64_t lo, uint64_t hi)
{
    switch (regDesc)
    {
    case 0x00:
        writeRegisterUnlocked(GS_REG_PRIM, lo & 0x7FF);
        break;
    case 0x01:
        m_curR = static_cast<uint8_t>(lo & 0xFF);
        m_curG = static_cast<uint8_t>((lo >> 32) & 0xFF);
        m_curB = static_cast<uint8_t>(hi & 0xFF);
        m_curA = static_cast<uint8_t>((hi >> 32) & 0xFF);
        break;
    case 0x02:
    {
        uint32_t sBits = static_cast<uint32_t>(lo & 0xFFFFFFFF);
        uint32_t tBits = static_cast<uint32_t>((lo >> 32) & 0xFFFFFFFF);
        uint32_t qBits = static_cast<uint32_t>(hi & 0xFFFFFFFF);
        std::memcpy(&m_curS, &sBits, 4);
        std::memcpy(&m_curT, &tBits, 4);
        std::memcpy(&m_curQ, &qBits, 4);
        if (m_curQ == 0.0f)
            m_curQ = 1.0f;
        break;
    }
    case 0x03:
        m_curU = static_cast<uint16_t>(lo & 0x3FFFu);
        m_curV = static_cast<uint16_t>((lo >> 32) & 0x3FFFu);
        break;
    case 0x04:
    {
        uint16_t x = static_cast<uint16_t>(lo & 0xFFFF);
        uint16_t y = static_cast<uint16_t>((lo >> 32) & 0xFFFF);
        uint32_t z = static_cast<uint32_t>((hi >> 4) & 0xFFFFFF);
        uint8_t f = static_cast<uint8_t>((hi >> 36) & 0xFF);
        bool adk = ((hi >> 47) & 1) != 0;
        PS2_IF_AGRESSIVE_LOGS({
            const uint32_t debugIndex = s_debugGsPackedVertexCount.fetch_add(1, std::memory_order_relaxed);
            if (debugIndex < 64u)
            {
                RUNTIME_LOG("[gs:packed-xyzf] idx=" << debugIndex
                                                    << " x=" << x
                                                    << " y=" << y
                                                    << " z=0x" << std::hex << z
                                                    << std::dec
                                                    << " fog=" << static_cast<uint32_t>(f)
                                                    << " kick=" << static_cast<uint32_t>(!adk ? 1u : 0u)
                                                    << " prim=" << static_cast<uint32_t>(m_prim.type)
                                                    << std::endl);
            }
        });
        GSVertex &vtx = m_vtxQueue[m_vtxCount % kMaxVerts];
        vtx.tapSlot = 0; // (GT4's VU1 tap: only PACKED XYZF2 vertices carry one)
        vtx.x = static_cast<float>(x) / 16.0f;
        vtx.y = static_cast<float>(y) / 16.0f;
        vtx.z = static_cast<float>(z);
        vtx.r = m_curR;
        vtx.g = m_curG;
        vtx.b = m_curB;
        vtx.a = m_curA;
        vtx.q = m_curQ;
        vtx.s = m_curS;
        vtx.t = m_curT;
        vtx.u = m_curU;
        vtx.v = m_curV;
        vtx.fog = f;
        vtx.tapSlot = m_tapSlot;
        vtx.tapK = m_tapK;
        m_tapK = static_cast<uint8_t>(m_tapK + 1u);
        vertexKick(!adk);
        break;
    }
    case 0x05:
    {
        uint16_t x = static_cast<uint16_t>(lo & 0xFFFF);
        uint16_t y = static_cast<uint16_t>((lo >> 32) & 0xFFFF);
        uint32_t z = static_cast<uint32_t>(hi & 0xFFFFFFFF);
        bool adk = ((hi >> 47) & 1) != 0;
        PS2_IF_AGRESSIVE_LOGS({
            const uint32_t debugIndex = s_debugGsPackedVertexCount.fetch_add(1, std::memory_order_relaxed);
            if (debugIndex < 64u)
            {
                RUNTIME_LOG("[gs:packed-xyz] idx=" << debugIndex
                                                   << " x=" << x
                                                   << " y=" << y
                                                   << " z=0x" << std::hex << z
                                                   << std::dec
                                                   << " kick=" << static_cast<uint32_t>(!adk ? 1u : 0u)
                                                   << " prim=" << static_cast<uint32_t>(m_prim.type)
                                                   << std::endl);
            }
        });
        GSVertex &vtx = m_vtxQueue[m_vtxCount % kMaxVerts];
        vtx.tapSlot = 0; // (GT4's VU1 tap: only PACKED XYZF2 vertices carry one)
        vtx.x = static_cast<float>(x) / 16.0f;
        vtx.y = static_cast<float>(y) / 16.0f;
        vtx.z = static_cast<float>(z);
        vtx.r = m_curR;
        vtx.g = m_curG;
        vtx.b = m_curB;
        vtx.a = m_curA;
        vtx.q = m_curQ;
        vtx.s = m_curS;
        vtx.t = m_curT;
        vtx.u = m_curU;
        vtx.v = m_curV;
        vtx.fog = m_curFog;
        vertexKick(!adk);
        break;
    }
    case 0x0A:
        m_curFog = static_cast<uint8_t>((hi >> 36) & 0xFF);
        break;
    case 0x0C:
    {
        PS2_IF_AGRESSIVE_LOGS({
            const uint32_t debugIndex = s_debugGsPackedVertexCount.fetch_add(1, std::memory_order_relaxed);
            if (debugIndex < 64u)
            {
                RUNTIME_LOG("[gs:packed-xyzf3] idx=" << debugIndex
                                                     << " x=" << static_cast<uint32_t>(lo & 0xFFFFu)
                                                     << " y=" << static_cast<uint32_t>((lo >> 32) & 0xFFFFu)
                                                     << " kick=0"
                                                     << " prim=" << static_cast<uint32_t>(m_prim.type)
                                                     << std::endl);
            }
        });
        GSVertex &vtx = m_vtxQueue[m_vtxCount % kMaxVerts];
        vtx.tapSlot = 0; // (GT4's VU1 tap: only PACKED XYZF2 vertices carry one)
        vtx.x = static_cast<float>(lo & 0xFFFF) / 16.0f;
        vtx.y = static_cast<float>((lo >> 32) & 0xFFFF) / 16.0f;
        vtx.z = static_cast<float>((hi >> 4) & 0xFFFFFF);
        vtx.r = m_curR;
        vtx.g = m_curG;
        vtx.b = m_curB;
        vtx.a = m_curA;
        vtx.q = m_curQ;
        vtx.s = m_curS;
        vtx.t = m_curT;
        vtx.u = m_curU;
        vtx.v = m_curV;
        vtx.fog = static_cast<uint8_t>((hi >> 36) & 0xFF);
        vertexKick(false);
        break;
    }
    case 0x0D:
    {
        PS2_IF_AGRESSIVE_LOGS({
            const uint32_t debugIndex = s_debugGsPackedVertexCount.fetch_add(1, std::memory_order_relaxed);
            if (debugIndex < 64u)
            {
                RUNTIME_LOG("[gs:packed-xyz3] idx=" << debugIndex
                                                    << " x=" << static_cast<uint32_t>(lo & 0xFFFFu)
                                                    << " y=" << static_cast<uint32_t>((lo >> 32) & 0xFFFFu)
                                                    << " kick=0"
                                                    << " prim=" << static_cast<uint32_t>(m_prim.type)
                                                    << std::endl);
            }
        });
        GSVertex &vtx = m_vtxQueue[m_vtxCount % kMaxVerts];
        vtx.tapSlot = 0; // (GT4's VU1 tap: only PACKED XYZF2 vertices carry one)
        vtx.x = static_cast<float>(lo & 0xFFFF) / 16.0f;
        vtx.y = static_cast<float>((lo >> 32) & 0xFFFF) / 16.0f;
        vtx.z = static_cast<float>(hi & 0xFFFFFFFF);
        vtx.r = m_curR;
        vtx.g = m_curG;
        vtx.b = m_curB;
        vtx.a = m_curA;
        vtx.q = m_curQ;
        vtx.s = m_curS;
        vtx.t = m_curT;
        vtx.u = m_curU;
        vtx.v = m_curV;
        vtx.fog = m_curFog;
        vertexKick(false);
        break;
    }
    case 0x0E:
    {
        uint8_t addr = static_cast<uint8_t>(hi & 0xFF);
        writeRegisterUnlocked(addr, lo);
        break;
    }
    case 0x0F:
        break;
    default:
        writeRegisterUnlocked(regDesc, lo);
        break;
    }
}

void GS::writeRegister(uint8_t regAddr, uint64_t value)
{
    std::lock_guard<std::recursive_mutex> lock(m_stateMutex);
    writeRegisterUnlocked(regAddr, value);
}

void GS::writeRegisterUnlocked(uint8_t regAddr, uint64_t value)
{
    const bool interestingReg =
        regAddr == GS_REG_PRIM ||
        regAddr == GS_REG_RGBAQ ||
        regAddr == GS_REG_ST ||
        regAddr == GS_REG_UV ||
        regAddr == GS_REG_XYZ2 ||
        regAddr == GS_REG_XYZ3 ||
        regAddr == GS_REG_XYZF2 ||
        regAddr == GS_REG_XYZF3 ||
        regAddr == GS_REG_TEX0_1 ||
        regAddr == GS_REG_TEX0_2 ||
        regAddr == GS_REG_TEX2_1 ||
        regAddr == GS_REG_TEX2_2 ||
        regAddr == GS_REG_TEXCLUT ||
        regAddr == GS_REG_TEXA ||
        regAddr == GS_REG_XYOFFSET_1 ||
        regAddr == GS_REG_XYOFFSET_2 ||
        regAddr == GS_REG_SCISSOR_1 ||
        regAddr == GS_REG_SCISSOR_2 ||
        regAddr == GS_REG_FRAME_1 ||
        regAddr == GS_REG_FRAME_2 ||
        regAddr == GS_REG_ALPHA_1 ||
        regAddr == GS_REG_ALPHA_2 ||
        regAddr == GS_REG_TEST_1 ||
        regAddr == GS_REG_TEST_2 ||
        regAddr == GS_REG_BITBLTBUF ||
        regAddr == GS_REG_TRXPOS ||
        regAddr == GS_REG_TRXREG ||
        regAddr == GS_REG_TRXDIR;

    PS2_IF_AGRESSIVE_LOGS({
        if (interestingReg)
        {
            const uint32_t debugIndex = s_debugGsRegisterCount.fetch_add(1, std::memory_order_relaxed);
            if (debugIndex < 128u)
            {
                RUNTIME_LOG("[gs:reg] idx=" << debugIndex
                                            << " reg=0x" << std::hex << static_cast<uint32_t>(regAddr)
                                            << " value=0x" << value
                                            << std::dec
                                            << std::endl);
            }
        }
    });

    const bool isCopyRelevantReg =
        regAddr == GS_REG_PRIM ||
        regAddr == GS_REG_TEX0_2 ||
        regAddr == GS_REG_TEX1_2 ||
        regAddr == GS_REG_ALPHA_2 ||
        regAddr == GS_REG_TEST_2 ||
        regAddr == GS_REG_PABE ||
        regAddr == GS_REG_FRAME_2 ||
        regAddr == GS_REG_XYOFFSET_2 ||
        regAddr == GS_REG_SCISSOR_2;
    PS2_IF_AGRESSIVE_LOGS({
        if (isCopyRelevantReg &&
            s_debugCopyRegCount.fetch_add(1u, std::memory_order_relaxed) < 64u)
        {
            RUNTIME_LOG("[gs:copy-reg] reg=0x"
                        << std::hex << static_cast<uint32_t>(regAddr)
                        << " value=0x" << value
                        << std::dec
                        << " primCtxt=" << static_cast<uint32_t>(m_prim.ctxt)
                        << " ctx0fbp=" << m_ctx[0].frame.fbp
                        << " ctx1fbp=" << m_ctx[1].frame.fbp
                        << std::endl);
        }
    });

#if defined(PS2X_GAME_GT4) && !defined(GT4_STOCK_BUILD)
    // GT4's race pictures (gt4_hud_pictures.h): a marker in the stream, register 0x7f (no GS register) with 'HUDP' in its
    // high word, passed to the backend in order with the primitives.
    if (regAddr == 0x7Fu)
    {
        if ((value >> 32) == 0x48554450ull && m_backend)
        {
            gt4hudmove::gsMarker(static_cast<uint32_t>(value)); // (GT4_HUD_MOVE: the moved parts' brackets)
            m_backend->Marker(static_cast<uint32_t>(value));
        }
        return;
    }
#endif
    switch (regAddr)
    {
    case GS_REG_PRIM:
    {
        m_primRegister = decodePrimRegister(value);
        if (m_prmodecont)
        {
            m_prim = m_primRegister;
        }
        else
        {
            // PRIM always selects the primitive topology. With AC=0, all
            // rendering attributes remain sourced from PRMODE.
            m_prim.type = m_primRegister.type;
        }
        m_vtxCount = 0;
        m_vtxIndex = 0;
        break;
    }
    case GS_REG_RGBAQ:
    {
        m_curR = static_cast<uint8_t>(value & 0xFF);
        m_curG = static_cast<uint8_t>((value >> 8) & 0xFF);
        m_curB = static_cast<uint8_t>((value >> 16) & 0xFF);
        m_curA = static_cast<uint8_t>((value >> 24) & 0xFF);
        uint32_t qBits = static_cast<uint32_t>((value >> 32) & 0xFFFFFFFF);
        std::memcpy(&m_curQ, &qBits, 4);
        if (m_curQ == 0.0f)
            m_curQ = 1.0f;
        break;
    }
    case GS_REG_ST:
    {
        uint32_t sBits = static_cast<uint32_t>(value & 0xFFFFFFFF);
        uint32_t tBits = static_cast<uint32_t>((value >> 32) & 0xFFFFFFFF);
        std::memcpy(&m_curS, &sBits, 4);
        std::memcpy(&m_curT, &tBits, 4);
        break;
    }
    case GS_REG_UV:
    {
        m_curU = static_cast<uint16_t>(value & 0x3FFFu);
        m_curV = static_cast<uint16_t>((value >> 16) & 0x3FFFu);
        break;
    }
    case GS_REG_XYZF2:
    case GS_REG_XYZF3:
    {
        GSVertex &vtx = m_vtxQueue[m_vtxCount % kMaxVerts];
        vtx.tapSlot = 0; // (GT4's VU1 tap: only PACKED XYZF2 vertices carry one)
        vtx.x = static_cast<float>(value & 0xFFFF) / 16.0f;
        vtx.y = static_cast<float>((value >> 16) & 0xFFFF) / 16.0f;
        vtx.z = static_cast<double>((value >> 32) & 0xFFFFFF);
        vtx.fog = static_cast<uint8_t>((value >> 56) & 0xFF);
        vtx.r = m_curR;
        vtx.g = m_curG;
        vtx.b = m_curB;
        vtx.a = m_curA;
        vtx.q = m_curQ;
        vtx.s = m_curS;
        vtx.t = m_curT;
        vtx.u = m_curU;
        vtx.v = m_curV;
        vertexKick(regAddr == GS_REG_XYZF2);
        break;
    }
    case GS_REG_XYZ2:
    case GS_REG_XYZ3:
    {
        GSVertex &vtx = m_vtxQueue[m_vtxCount % kMaxVerts];
        vtx.tapSlot = 0; // (GT4's VU1 tap: only PACKED XYZF2 vertices carry one)
        vtx.x = static_cast<float>(value & 0xFFFF) / 16.0f;
        vtx.y = static_cast<float>((value >> 16) & 0xFFFF) / 16.0f;
        vtx.z = static_cast<double>((value >> 32) & 0xFFFFFFFF);
        vtx.r = m_curR;
        vtx.g = m_curG;
        vtx.b = m_curB;
        vtx.a = m_curA;
        vtx.q = m_curQ;
        vtx.s = m_curS;
        vtx.t = m_curT;
        vtx.u = m_curU;
        vtx.v = m_curV;
        vtx.fog = m_curFog;
        vertexKick(regAddr == GS_REG_XYZ2);
        break;
    }
    case GS_REG_TEX0_1:
    case GS_REG_TEX0_2:
    {
        int ci = (regAddr == GS_REG_TEX0_2) ? 1 : 0;
        auto &t = m_ctx[ci].tex0;
        t.tbp0 = static_cast<uint32_t>(value & 0x3FFF);
        t.tbw = static_cast<uint8_t>((value >> 14) & 0x3F);
        t.psm = static_cast<uint8_t>((value >> 20) & 0x3F);
        t.tw = static_cast<uint8_t>((value >> 26) & 0xF);
        t.th = static_cast<uint8_t>((value >> 30) & 0xF);
        t.tcc = static_cast<uint8_t>((value >> 34) & 0x1);
        t.tfx = static_cast<uint8_t>((value >> 35) & 0x3);
        t.cbp = static_cast<uint32_t>((value >> 37) & 0x3FFF);
        t.cpsm = static_cast<uint8_t>((value >> 51) & 0xF);
        t.csm = static_cast<uint8_t>((value >> 55) & 0x1);
        t.csa = static_cast<uint8_t>((value >> 56) & 0x1F);
        t.cld = static_cast<uint8_t>((value >> 61) & 0x7);
        {
            // Opt-in diagnostic: GT3_GS_SNAPSHOT_TEX0=<hex TBP> [GT3_GS_SNAPSHOT_SKIP=n[,n...]]
            // dumps VRAM to GT3_GS_SNAPSHOT_PATH (".<n>" appended when several are
            // listed) before the (n+1)-th TEX0 write with that TBP is applied
            // (e.g. right before a post-process pass).
            static const long snapshotTbp = [] {
                const char *v = std::getenv("GT3_GS_SNAPSHOT_TEX0");
                return (v && *v) ? std::strtol(v, nullptr, 16) : -1L;
            }();
            static const std::vector<long> snapshotAt = [] {
                std::vector<long> at;
                const char *v = std::getenv("GT3_GS_SNAPSHOT_SKIP");
                for (const char *p = (v && *v) ? v : "0"; *p;)
                {
                    char *end = nullptr;
                    at.push_back(std::strtol(p, &end, 10));
                    p = (*end == ',') ? end + 1 : end;
                    if (end == p && *p) break;
                }
                return at;
            }();
            static long snapshotSeen = 0;
            if (snapshotTbp >= 0 && static_cast<long>(t.tbp0) == snapshotTbp && m_backend)
            {
                const long occurrence = snapshotSeen++;
                if (std::find(snapshotAt.begin(), snapshotAt.end(), occurrence) != snapshotAt.end())
                {
                    std::vector<uint8_t> snapshot;
                    m_backend->Flush();
                    m_backend->SnapshotVram(snapshot);
                    g_gsDiagSnapshotTaken.store(true, std::memory_order_relaxed);
                    const char *path = std::getenv("GT3_GS_SNAPSHOT_PATH");
                    std::string file = (path && *path) ? path : "gs-snapshot.vram.bin";
                    if (snapshotAt.size() > 1)
                        file += "." + std::to_string(occurrence);
                    if (FILE *out = std::fopen(file.c_str(), "wb"))
                    {
                        std::fwrite(snapshot.data(), 1, snapshot.size(), out);
                        std::fclose(out);
                        std::cerr << "[gs-snapshot] VRAM saved at TEX0 TBP 0x" << std::hex << t.tbp0 << std::dec
                                  << " occurrence " << occurrence << std::endl;
                    }
                }
            }
        }
        // MTBA: the level bases packed after level 0 (gs_mip.h applyMtba).
        if (gsmip::enabled() && ((m_ctx[ci].tex1 >> 9) & 1u) != 0u)
            gsmip::applyMtba(m_ctx[ci]);
        loadClutForCld(t);
        break;
    }
    case GS_REG_CLAMP_1:
    case GS_REG_CLAMP_2:
    {
        int ci = (regAddr == GS_REG_CLAMP_2) ? 1 : 0;
        m_ctx[ci].clamp = value;
        break;
    }
    case GS_REG_FOG:
        m_curFog = static_cast<uint8_t>((value >> 56) & 0xFF);
        break;
    case GS_REG_TEX1_1:
    case GS_REG_TEX1_2:
    {
        int ci = (regAddr == GS_REG_TEX1_2) ? 1 : 0;
        m_ctx[ci].tex1 = value;
        break;
    }
    case GS_REG_TEX2_1:
    case GS_REG_TEX2_2:
    {
        int ci = (regAddr == GS_REG_TEX2_2) ? 1 : 0;
        auto &t = m_ctx[ci].tex0;
        t.psm = static_cast<uint8_t>((value >> 20) & 0x3F);
        t.cbp = static_cast<uint32_t>((value >> 37) & 0x3FFF);
        t.cpsm = static_cast<uint8_t>((value >> 51) & 0xF);
        t.csm = static_cast<uint8_t>((value >> 55) & 0x1);
        t.csa = static_cast<uint8_t>((value >> 56) & 0x1F);
        t.cld = static_cast<uint8_t>((value >> 61) & 0x7);
        if (gsmip::enabled() && ((m_ctx[ci].tex1 >> 9) & 1u) != 0u)
            gsmip::applyMtba(m_ctx[ci]);
        loadClutForCld(t);
        break;
    }
    case GS_REG_XYOFFSET_1:
    case GS_REG_XYOFFSET_2:
    {
        int ci = (regAddr == GS_REG_XYOFFSET_2) ? 1 : 0;
        m_ctx[ci].xyoffset.ofx = static_cast<uint16_t>(value & 0xFFFF);
        m_ctx[ci].xyoffset.ofy = static_cast<uint16_t>((value >> 32) & 0xFFFF);
        // GT3_XYOFFSET_LOG=<n>: the first n XYOFFSET writes (field offsets).
        static const long xyLog = [] {
            const char *v = std::getenv("GT3_XYOFFSET_LOG");
            return v ? std::strtol(v, nullptr, 10) : 0L;
        }();
        static long xyLogged = 0;
        if (xyLogged < xyLog)
        {
            ++xyLogged;
            std::fprintf(stderr, "[xyoffset] tick %llu ctx %d ofx %u.%u ofy %u.%u\n",
                         static_cast<unsigned long long>(m_privRegs ? m_privRegs->vsyncTick.load() : 0ull), ci,
                         m_ctx[ci].xyoffset.ofx >> 4, m_ctx[ci].xyoffset.ofx & 15u, m_ctx[ci].xyoffset.ofy >> 4,
                         m_ctx[ci].xyoffset.ofy & 15u);
        }
        break;
    }
    case GS_REG_PRMODECONT:
    {
        m_prmodecont = (value & 1) != 0;
        const GSPrimType type = m_primRegister.type;
        m_prim = m_prmodecont ? m_primRegister : m_prmodeRegister;
        m_prim.type = type;
        break;
    }
    case GS_REG_PRMODE:
    {
        m_prmodeRegister = decodePrimRegister(value);
        if (!m_prmodecont)
        {
            const GSPrimType type = m_primRegister.type;
            m_prim = m_prmodeRegister;
            m_prim.type = type;
        }
        break;
    }
    case GS_REG_TEXCLUT:
        m_texclut.cbw = static_cast<uint8_t>(value & 0x3Fu);
        m_texclut.cou = static_cast<uint8_t>((value >> 6) & 0x3Fu);
        m_texclut.cov = static_cast<uint16_t>((value >> 12) & 0x3FFu);
        break;
    case GS_REG_SCISSOR_1:
    case GS_REG_SCISSOR_2:
    {
        int ci = (regAddr == GS_REG_SCISSOR_2) ? 1 : 0;
        m_ctx[ci].scissor.x0 = static_cast<uint16_t>(value & 0x7FF);
        m_ctx[ci].scissor.x1 = static_cast<uint16_t>((value >> 16) & 0x7FF);
        m_ctx[ci].scissor.y0 = static_cast<uint16_t>((value >> 32) & 0x7FF);
        m_ctx[ci].scissor.y1 = static_cast<uint16_t>((value >> 48) & 0x7FF);
        break;
    }
    case GS_REG_ALPHA_1:
    case GS_REG_ALPHA_2:
    {
        int ci = (regAddr == GS_REG_ALPHA_2) ? 1 : 0;
        m_ctx[ci].alpha = value;
        break;
    }
    case GS_REG_TEST_1:
    case GS_REG_TEST_2:
    {
        int ci = (regAddr == GS_REG_TEST_2) ? 1 : 0;
        m_ctx[ci].test = value;
        break;
    }
    case GS_REG_FRAME_1:
    case GS_REG_FRAME_2:
    {
        int ci = (regAddr == GS_REG_FRAME_2) ? 1 : 0;
        m_ctx[ci].frame.fbp = static_cast<uint32_t>(value & 0x1FF);
        m_ctx[ci].frame.fbw = static_cast<uint32_t>((value >> 16) & 0x3F);
        m_ctx[ci].frame.psm = static_cast<uint8_t>((value >> 24) & 0x3F);
        m_ctx[ci].frame.fbmsk = static_cast<uint32_t>((value >> 32) & 0xFFFFFFFF);
        break;
    }
    case GS_REG_ZBUF_1:
    case GS_REG_ZBUF_2:
    {
        int ci = (regAddr == GS_REG_ZBUF_2) ? 1 : 0;
        m_ctx[ci].zbuf.zbp = value & 0x1FF;
        m_ctx[ci].zbuf.psm = ((value >> 24) & 0xF) | 0x30;
        m_ctx[ci].zbuf.zmask = (value >> 32) & 1;
        break;
    }
    case GS_REG_FBA_1:
    case GS_REG_FBA_2:
    {
        int ci = (regAddr == GS_REG_FBA_2) ? 1 : 0;
        m_ctx[ci].fba = value;
        break;
    }
    case GS_REG_BITBLTBUF:
    {
        m_bitbltbuf.sbp = static_cast<uint32_t>(value & 0x3FFF);
        m_bitbltbuf.sbw = static_cast<uint8_t>((value >> 16) & 0x3F);
        m_bitbltbuf.spsm = static_cast<uint8_t>((value >> 24) & 0x3F);
        m_bitbltbuf.dbp = static_cast<uint32_t>((value >> 32) & 0x3FFF);
        m_bitbltbuf.dbw = static_cast<uint8_t>((value >> 48) & 0x3F);
        m_bitbltbuf.dpsm = static_cast<uint8_t>((value >> 56) & 0x3F);
        break;
    }
    case GS_REG_TRXPOS:
    {
        m_trxpos.ssax = static_cast<uint16_t>(value & 0x7FF);
        m_trxpos.ssay = static_cast<uint16_t>((value >> 16) & 0x7FF);
        m_trxpos.dsax = static_cast<uint16_t>((value >> 32) & 0x7FF);
        m_trxpos.dsay = static_cast<uint16_t>((value >> 48) & 0x7FF);
        m_trxpos.dir = static_cast<uint8_t>((value >> 59) & 0x3);
        break;
    }
    case GS_REG_TRXREG:
    {
        m_trxreg.rrw = static_cast<uint16_t>(value & 0xFFF);
        m_trxreg.rrh = static_cast<uint16_t>((value >> 32) & 0xFFF);
        break;
    }
    case GS_REG_TRXDIR:
    {
        m_trxdir = static_cast<uint32_t>(value & 0x3);
        if (m_trxdir == 0u)
        {
            // GT4_MOVIE_TRACE=1 (diagnostics, analysis/design/movie-stutter): host-to-GS image uploads summed per VSync tick.
            static const bool movieTrace = std::getenv("GT4_MOVIE_TRACE") != nullptr;
            if (movieTrace)
            {
                static uint64_t traceTick = 0, tracePixels = 0, traceCount = 0;
                static uint32_t traceDbp = 0;
                const uint64_t tick = m_privRegs ? m_privRegs->vsyncTick.load(std::memory_order_acquire) : 0u;
                if (tick != traceTick)
                {
                    if (tracePixels)
                        std::fprintf(stderr, "[mvt] up tick=%llu n=%llu px=%llu dbp=%x\n", static_cast<unsigned long long>(traceTick),
                                     static_cast<unsigned long long>(traceCount), static_cast<unsigned long long>(tracePixels), traceDbp);
                    traceTick = tick;
                    tracePixels = traceCount = 0;
                }
                if (m_trxpos.dsax == 0u && m_trxpos.dsay == 0u && m_trxreg.rrw == 16u && m_trxreg.rrh == 16u)
                {
                    static uint64_t picTick = 0;
                    if (g_movieTraceBytes)
                        std::fprintf(stderr, "[mvt] pic tick=%llu end=%llu bytes=%llu hash=%016llx\n", static_cast<unsigned long long>(picTick),
                                     static_cast<unsigned long long>(tick), static_cast<unsigned long long>(g_movieTraceBytes),
                                     static_cast<unsigned long long>(g_movieTraceHash));
                    picTick = tick;
                    g_movieTraceHash = 1469598103934665603ull;
                    g_movieTraceBytes = 0;
                }
                if (!traceCount)
                    traceDbp = m_bitbltbuf.dbp;
                ++traceCount;
                tracePixels += uint64_t(m_trxreg.rrw) * m_trxreg.rrh;
            }
        }
        if (m_trxdir == 0u || m_trxdir == 2u)
        {
            ++m_writeSeq;
            noteWrittenRect(m_bitbltbuf.dbp >> 5, m_bitbltbuf.dbw, m_bitbltbuf.dpsm, m_trxpos.dsax, m_trxpos.dsay,
                            m_trxpos.dsax + m_trxreg.rrw - 1, m_trxpos.dsay + m_trxreg.rrh - 1);
        }

        if (m_backend)
        {
            GSTransferCommand command{};
            command.bitbltbuf = m_bitbltbuf;
            command.trxpos = m_trxpos;
            command.trxreg = m_trxreg;
            command.direction = m_trxdir;
            m_backend->BeginTransfer(command);
        }
        recordTransferDebugEventUnlocked();
        break;
    }
    case GS_REG_HWREG:
    {
        uint8_t buf[8];
        std::memcpy(buf, &value, 8);
        processImageData(buf, 8);
        break;
    }
    case GS_REG_PABE:
        m_pabe = (value & 1u) != 0u;
        break;
    case GS_REG_FOGCOL:
        m_fogR = static_cast<uint8_t>(value & 0xFFu);
        m_fogG = static_cast<uint8_t>((value >> 8) & 0xFFu);
        m_fogB = static_cast<uint8_t>((value >> 16) & 0xFFu);
        break;
    case GS_REG_TEXFLUSH:
        if (m_backend)
            m_backend->TextureFlush();
        break;
    case GS_REG_SCANMSK:
        m_scanmsk = value;
        break;
    case GS_REG_DIMX:
        m_dimx = value;
        break;
    case GS_REG_DTHE:
        m_dthe = value;
        break;
    case GS_REG_COLCLAMP:
        m_colclamp = value;
        break;
    case GS_REG_MIPTBP1_1:
    case GS_REG_MIPTBP1_2:
    {
        const int ci = (regAddr == GS_REG_MIPTBP1_2) ? 1 : 0;
        m_ctx[ci].miptbp1 = value;
        break;
    }
    case GS_REG_MIPTBP2_1:
    case GS_REG_MIPTBP2_2:
    {
        const int ci = (regAddr == GS_REG_MIPTBP2_2) ? 1 : 0;
        m_ctx[ci].miptbp2 = value;
        break;
    }
    case GS_REG_TEXA:
    {
        m_texa.ta0 = static_cast<uint8_t>(value & 0xFFu);
        m_texa.aem = ((value >> 15) & 0x1u) != 0u;
        m_texa.ta1 = static_cast<uint8_t>((value >> 32) & 0xFFu);
        PS2_IF_AGRESSIVE_LOGS({
            const uint32_t texaIndex = s_debugTexaWriteCount.fetch_add(1u, std::memory_order_relaxed);
            if (texaIndex < 24u)
            {
                RUNTIME_LOG("[gs:texa] idx=" << texaIndex
                                             << " value=0x" << std::hex << value
                                             << " ta0=0x" << ((value >> 0) & 0xFFu)
                                             << " aem=" << ((value >> 15) & 0x1u)
                                             << " ta1=0x" << ((value >> 32) & 0xFFu)
                                             << std::dec
                                             << std::endl);
            }
        });
        break;
    }
    case GS_REG_SIGNAL:
    {
        if (m_privRegs)
        {
            uint32_t id = static_cast<uint32_t>(value & 0xFFFFFFFF);
            uint32_t mask = static_cast<uint32_t>(value >> 32);
            uint32_t lo = static_cast<uint32_t>(m_privRegs->siglblid & 0xFFFFFFFF);
            lo = (lo & ~mask) | (id & mask);
            m_privRegs->siglblid = (m_privRegs->siglblid & 0xFFFFFFFF00000000ULL) | lo;
            m_privRegs->csr.fetch_or(0x1);
        }
        if (m_eventSink)
            m_eventSink(0x1u);
        break;
    }
    case GS_REG_FINISH:
    {
        if (m_backend)
        {
            m_backend->Flush();
            m_backend->Sync(GSSyncReason::Finish);
        }
        if (m_privRegs)
            m_privRegs->csr.fetch_or(0x2);
        // GT3's race waits for this interrupt (handler 0x25a5b8) before its
        // VIF1 readback; without it every such frame hit a 120-VSync timeout.
        if (m_eventSink)
            m_eventSink(0x2u);
        break;
    }
    case GS_REG_LABEL:
    {
        if (m_privRegs)
        {
            uint32_t id = static_cast<uint32_t>(value & 0xFFFFFFFF);
            uint32_t mask = static_cast<uint32_t>(value >> 32);
            uint32_t hi = static_cast<uint32_t>(m_privRegs->siglblid >> 32);
            hi = (hi & ~mask) | (id & mask);
            m_privRegs->siglblid = (static_cast<uint64_t>(hi) << 32) | (m_privRegs->siglblid & 0xFFFFFFFF);
        }
        break;
    }
    case 0x59:
        if (m_privRegs)
            m_privRegs->dispfb1 = value;
        break;
    case 0x5a:
        if (m_privRegs)
            m_privRegs->display1 = value;
        break;
    case 0x5b:
        if (m_privRegs)
            m_privRegs->dispfb2 = value;
        break;
    case 0x5c:
        if (m_privRegs)
            m_privRegs->display2 = value;
        break;
    case 0x5f:
        if (m_privRegs)
            m_privRegs->bgcolor = value;
        break;
    default:
        break;
    }

    recordRegisterDebugEventUnlocked(regAddr, value);
}

void GS::vertexKick(bool drawing)
{
    ++m_vtxCount;
    ++m_vtxIndex;

    PS2_IF_AGRESSIVE_LOGS({
        const uint32_t debugIndex = s_debugGsVertexKickCount.fetch_add(1, std::memory_order_relaxed);
        if (debugIndex < 96u)
        {
            RUNTIME_LOG("[gs:kick] idx=" << debugIndex
                                         << " drawing=" << static_cast<uint32_t>(drawing ? 1u : 0u)
                                         << " prim=" << static_cast<uint32_t>(m_prim.type)
                                         << " vtxCount=" << m_vtxCount
                                         << std::endl);
        }
    });

    int needed = 0;
    switch (m_prim.type)
    {
    case GS_PRIM_POINT:
        needed = 1;
        break;
    case GS_PRIM_LINE:
        needed = 2;
        break;
    case GS_PRIM_LINESTRIP:
        needed = 2;
        break;
    case GS_PRIM_TRIANGLE:
        needed = 3;
        break;
    case GS_PRIM_TRISTRIP:
        needed = 3;
        break;
    case GS_PRIM_TRIFAN:
        needed = 3;
        break;
    case GS_PRIM_SPRITE:
        needed = 2;
        break;
    default:
        return;
    }

    if (m_vtxCount < needed)
        return;

    if (drawing && m_backend)
    {
        GSPrimitiveBatch batch = buildDrawBatch(needed);
        // false: a hidden GT3 HUD element (GS::setHudHide), not drawn (the
        // vertex queue below still advances).
#if defined(GT3_STOCK_BUILD) || defined(PS2X_GAME_GT4)
        constexpr bool hudKeep = true; // the stock build: GT3's HUD as drawn
#else
        const bool hudKeep = squeezeHud(batch);
        if (hudKeep)
            noteInsetView(batch);
#endif
        {
            // Opt-in diagnostic: GT3_FPS_LOG=1 prints GT3 race frames per second.
            // GT3 draws one depth-copy pass (a Z24 frame buffer) per rendered frame.
            static const bool fpsLog = [] {
                const char *v = std::getenv("GT3_FPS_LOG");
                return v && *v == '1';
            }();
            if (fpsLog)
            {
                static bool inDepthCopy = false;
                static uint32_t frames = 0u;
                static auto windowStart = std::chrono::steady_clock::now();
                const bool depthCopy = batch.state.context.frame.psm == GS_PSM_Z24;
                const bool newFrame = depthCopy && !inDepthCopy;
                if (newFrame)
                    ++frames;
                inDepthCopy = depthCopy;
                // The clock once a frame (or 1024 draws): read on every draw
                // it cost the GS thread 2% of its time.
                static uint32_t draws = 0u;
                const auto now = (newFrame || (++draws & 1023u) == 0u) ? std::chrono::steady_clock::now() : windowStart;
                // Each frame's time: the window's slowest and how many took
                // over 17.5 ms (a missed 60 Hz frame); GT3_FRAME_TIMES=<file>
                // also writes every frame (vsync tick, ms) there as CSV.
                static std::chrono::steady_clock::time_point lastFrame{};
                static double worstMs = 0.0;
                static uint32_t slowFrames = 0u;
                static FILE *frameTimes = [] {
                    const char *path = std::getenv("GT3_FRAME_TIMES");
                    FILE *f = (path && *path) ? std::fopen(path, "w") : nullptr;
                    if (f)
                        std::fputs("vsync,ms\n", f);
                    return f;
                }();
                if (newFrame)
                {
                    if (lastFrame.time_since_epoch().count() != 0)
                    {
                        const double ms = std::chrono::duration<double, std::milli>(now - lastFrame).count();
                        worstMs = std::max(worstMs, ms);
                        slowFrames += ms > 17.5 ? 1u : 0u;
                        if (frameTimes)
                            std::fprintf(frameTimes, "%llu,%.2f\n", static_cast<unsigned long long>(m_privRegs ? m_privRegs->vsyncTick.load() : 0ull), ms);
                    }
                    lastFrame = now;
                }
                const double seconds = std::chrono::duration<double>(now - windowStart).count();
                if (seconds >= 5.0)
                {
                    std::cerr << "[fps] " << frames / seconds << " frames/s over " << seconds << " s (vsync "
                              << (m_privRegs ? m_privRegs->vsyncTick.load() : 0ull) << ") worst " << worstMs << " ms, " << slowFrames
                              << " over 17.5 ms" << std::endl;
                    if (frameTimes)
                        std::fflush(frameTimes);
                    frames = 0u;
                    worstMs = 0.0;
                    slowFrames = 0u;
                    windowStart = now;
                }
            }
            // Opt-in diagnostic: GT3_GS_SHUFFLE_LOG=<frames> prints, once per race
            // frame, the distinct vertex colour / ALPHA / TFX of GT3's channel
            // shuffle sprites (PSMT8 view drawn into 0x1c3..0x1c9): the per-frame
            // colour matrix of its colour-correction pass.
            static const long shuffleLogFrames = [] {
                const char *v = std::getenv("GT3_GS_SHUFFLE_LOG");
                return (v && *v) ? std::strtol(v, nullptr, 10) : 0L;
            }();
            if (shuffleLogFrames > 0)
            {
                static bool inDepthCopy = false;
                static long printed = 0;
                static std::vector<uint64_t> seen;
                const auto &ctx = batch.state.context;
                const bool depthCopy = ctx.frame.psm == GS_PSM_Z24;
                if (depthCopy && !inDepthCopy && printed < shuffleLogFrames)
                {
                    ++printed;
                    std::cerr << "[gs-shuffle] tick=" << (m_privRegs ? m_privRegs->vsyncTick.load() : 0) << std::hex;
                    for (const uint64_t key : seen)
                        std::cerr << " fbp" << ((key >> 51) & 0x1FF) << ":rgba=" << (key & 0xFFFFFFFFu) << "/a" << ((key >> 32) & 0xFFu)
                                  << "/fix" << ((key >> 40) & 0xFFu) << "/tfx" << ((key >> 48) & 0x3u) << "/abe" << ((key >> 50) & 1u);
                    std::cerr << std::dec << std::endl;
                    seen.clear();
                }
                inDepthCopy = depthCopy;
                if (batch.state.prim.tme && ctx.tex0.psm == GS_PSM_T8)
                {
                    const GSVertex &v = batch.vertices[batch.vertexCount ? batch.vertexCount - 1u : 0u];
                    const uint64_t key = (static_cast<uint64_t>(v.r) | (static_cast<uint64_t>(v.g) << 8) | (static_cast<uint64_t>(v.b) << 16) |
                                          (static_cast<uint64_t>(v.a) << 24)) |
                                         ((ctx.alpha & 0xFFull) << 32) | (((ctx.alpha >> 32) & 0xFFull) << 40) |
                                         (static_cast<uint64_t>(ctx.tex0.tfx & 3u) << 48) | (static_cast<uint64_t>(batch.state.prim.abe ? 1u : 0u) << 50) |
                                         (static_cast<uint64_t>(ctx.frame.fbp & 0x1FFu) << 51);
                    if (std::find(seen.begin(), seen.end(), key) == seen.end() && seen.size() < 32u)
                        seen.push_back(key);
                }
            }
            logFrameDraw(batch);
        }
        // A hidden HUD element still goes through the bookkeeping, only not
        // drawn: skipping it too left GT3's display-copy source "preferred"
        // (no later draw into the display buffer cleared it), and that path
        // showed the whole picture at PS2 resolution at 2x-4x (2026-09-29).
        // GT3_FIELD_DRAW_LOG=<tick>: 2 frames of draws from that tick (field
        // placement: vertex y, OFY, the y in the target, texture v).
        {
            static const uint64_t drawLogFrom = [] {
                const char *v = std::getenv("GT3_FIELD_DRAW_LOG");
                return v && *v ? std::strtoull(v, nullptr, 10) : ~0ull;
            }();
            const uint64_t tick = m_privRegs ? m_privRegs->vsyncTick.load(std::memory_order_relaxed) : 0u;
            if (tick >= drawLogFrom && tick < drawLogFrom + 2u && batch.vertexCount)
            {
                const auto &ctx = batch.state.context;
                float y0 = batch.vertices[0].y, y1 = y0, x0 = batch.vertices[0].x, x1 = x0, t0 = batch.vertices[0].v, t1 = t0;
                for (uint32_t i = 1; i < batch.vertexCount; ++i)
                {
                    const GSVertex &v = batch.vertices[i];
                    y0 = std::min(y0, v.y), y1 = std::max(y1, v.y), x0 = std::min(x0, v.x), x1 = std::max(x1, v.x);
                    const float tv = batch.state.prim.fst ? static_cast<float>(v.v) / 16.0f : v.t / std::max(v.q, 1e-6f);
                    t0 = std::min(t0, tv), t1 = std::max(t1, tv);
                }
                const float ofy = static_cast<float>(ctx.xyoffset.ofy) / 16.0f, ofx = static_cast<float>(ctx.xyoffset.ofx) / 16.0f;
                const GSVertex &lv = batch.vertices[batch.vertexCount - 1u];
                std::fprintf(stderr, "[field-draw] tick=%llu prim=%u tme=%u fst=%u fbp=0x%x fbw=%u psm=%u fbmsk=%x ofy=%u.%u x=%.2f..%.2f y=%.4f..%.4f (target %.4f..%.4f) tv=%.3f..%.3f n=%u"
                                     " rgba=%u,%u,%u,%u abe=%u alpha=%llx test=%llx fba=%llx pabe=%u tex=%x/%u/psm%u tfx=%u tcc=%u\n",
                             static_cast<unsigned long long>(tick), static_cast<unsigned>(batch.state.prim.type), batch.state.prim.tme ? 1u : 0u,
                             batch.state.prim.fst ? 1u : 0u, ctx.frame.fbp, ctx.frame.fbw, ctx.frame.psm, ctx.frame.fbmsk, ctx.xyoffset.ofy >> 4,
                             ctx.xyoffset.ofy & 0xFu, x0 - ofx, x1 - ofx, y0, y1, y0 - ofy, y1 - ofy, t0, t1, batch.vertexCount, lv.r, lv.g, lv.b, lv.a,
                             batch.state.prim.abe ? 1u : 0u, static_cast<unsigned long long>(ctx.alpha), static_cast<unsigned long long>(ctx.test),
                             static_cast<unsigned long long>(ctx.fba), batch.state.pabe ? 1u : 0u, ctx.tex0.tbp0, ctx.tex0.tbw, ctx.tex0.psm, ctx.tex0.tfx,
                             ctx.tex0.tcc);
            }
        }
#if defined(PS2X_GAME_GT4) && !defined(GT4_STOCK_BUILD)
        // GT4_HUD_MOVE (gt4_hud_move.h): a primitive of a moved part of GT4's race HUD moved and scaled here, before the
        // bookkeeping and the renderer.
        static const bool hudMove = gt4hudmove::on();
        if (hudMove)
            gt4hudmove::apply(batch);
#endif
        noteDrawWrites(batch);
#if defined(PS2X_GAME_GT3)
        // GT3's display-copy pattern (640x448 sprite, ALPHA 0x64 FIX 0x60/0x80, CTXT 1). GT4's
        // fields are 224 lines, so it never matched a GT4 draw (4,464 latches, all
        // preferred=ffffffff); if it ever did it would show the wrong buffer
        // (analysis/phase5/specs/step3-compare.md 4.3).
        updatePreferredDisplaySourceForDraw(batch);
#endif
        // The mip level(s) (PS2X_GS_MIPMAP; after the logs and bookkeeping above,
        // which keep the game's own TEX0).
        resolveMip(batch);
        if (batch.state.ftex && batch.state.mipTag != 0u)
        {
            // GT4's race colour texture reads (gs_fullcolour.h texFlag) for the level the renderers will read: a
            // per-pixel mip primitive reads raw memory in the GL renderer, so never full colour.
            const GSTex0Reg &t = batch.state.context.tex0;
            batch.state.ftex = batch.state.mipTag == gsmip::kTagPerPixel ? 0u : gsfc::texFlag(true, t.psm, t.tbp0, t.tbw, ps2x::crt::current());
        }
        if (hudKeep)
            m_backend->Submit(batch);
        recordDrawDebugEventUnlocked(needed);
    }

    switch (m_prim.type)
    {
    case GS_PRIM_LINE:
    case GS_PRIM_TRIANGLE:
    case GS_PRIM_SPRITE:
    case GS_PRIM_POINT:
        m_vtxCount = 0;
        break;
    case GS_PRIM_LINESTRIP:
        m_vtxQueue[0] = m_vtxQueue[1];
        m_vtxCount = 1;
        break;
    case GS_PRIM_TRISTRIP:
        m_vtxQueue[0] = m_vtxQueue[1];
        m_vtxQueue[1] = m_vtxQueue[2];
        m_vtxCount = 2;
        break;
    case GS_PRIM_TRIFAN:
        m_vtxQueue[1] = m_vtxQueue[2];
        m_vtxCount = 2;
        break;
    default:
        m_vtxCount = 0;
        break;
    }
}

void GS::processImageData(const uint8_t *data, uint32_t sizeBytes)
{
    static const bool movieTrace = std::getenv("GT4_MOVIE_TRACE") != nullptr;
    if (movieTrace)
    {
        for (uint32_t i = 0; i < sizeBytes; i += 7u)
            g_movieTraceHash = (g_movieTraceHash ^ data[i]) * 1099511628211ull;
        g_movieTraceBytes += sizeBytes;
    }
    if (m_backend)
        m_backend->UploadImage(data, sizeBytes);
}


bool GS::clearFramebufferContext(uint32_t contextIndex, uint32_t rgba)
{
    std::lock_guard<std::recursive_mutex> lock(m_stateMutex);
    return m_backend && m_backend->ClearFramebuffer(m_ctx[(contextIndex != 0u) ? 1 : 0], rgba);
}

bool GS::clearActiveFramebuffer(uint32_t rgba)
{
    std::lock_guard<std::recursive_mutex> lock(m_stateMutex);
    return m_backend && m_backend->ClearFramebuffer(activeContext(), rgba);
}

uint32_t GS::consumeLocalToHostBytes(uint8_t *dst, uint32_t maxBytes)
{
    std::lock_guard<std::recursive_mutex> lock(m_stateMutex);
    return m_backend ? m_backend->ConsumeLocalToHostBytes(dst, maxBytes) : 0u;
}

void GS::setRasterBackend(std::unique_ptr<GSRasterBackend> backend)
{
    if (!backend)
        backend = std::make_unique<GSCpuBackend>();

    std::lock_guard<std::recursive_mutex> lock(m_stateMutex);
    std::lock_guard<std::mutex> backendLock(m_backendLifetimeMutex);
    if (m_backend)
    {
        m_backend->Flush();
        m_backend->Sync(GSSyncReason::Reset);

        // The external 4 MiB GS allocation is the backend hand-off format.
        // This keeps hot backend replacement deterministic even when a future
        // GPU backend keeps a private/mirrored local-memory representation.
        if (m_localMemoryStorage && m_localMemorySize != 0u)
        {
            std::vector<uint8_t> localMemory;
            m_backend->SnapshotVram(localMemory);
            const size_t bytes = std::min<size_t>(localMemory.size(), m_localMemorySize);
            if (bytes != 0u)
                std::memcpy(m_localMemoryStorage, localMemory.data(), bytes);
        }
    }

    m_backend = std::move(backend);
    m_backend->Initialize(m_localMemoryStorage, m_localMemorySize);
}

uint32_t GS::ReadVram(uint32_t psm, uint32_t base, uint32_t bw, uint32_t x, uint32_t y) const
{
    std::lock_guard<std::recursive_mutex> lock(m_stateMutex);
    return m_backend ? m_backend->ReadVram(psm, base, bw, x, y) : 0u;
}

void GS::WriteVram(uint32_t psm, uint32_t base, uint32_t bw, uint32_t x, uint32_t y, uint32_t value)
{
    std::lock_guard<std::recursive_mutex> lock(m_stateMutex);
    if (m_backend)
        m_backend->WriteVram(psm, base, bw, x, y, value);
}

GSPrimitiveBatch GS::buildDrawBatch(int vertexCount) const
{
    GSPrimitiveBatch batch{};
    batch.vertexCount = static_cast<uint8_t>(std::min(vertexCount, 3));
    for (int i = 0; i < batch.vertexCount; ++i)
        batch.vertices[static_cast<size_t>(i)] = m_vtxQueue[i];
    batch.state.context = m_ctx[m_prim.ctxt ? 1 : 0];
    batch.state.prim = m_prim;
    batch.state.texa = m_texa;
    batch.state.texclut = m_texclut;
    batch.state.pabe = m_pabe;
    batch.state.scanmsk = m_scanmsk;
    batch.state.dimx = m_dimx;
    batch.state.dthe = m_dthe;
    batch.state.colclamp = m_colclamp;
    batch.state.fogR = m_fogR;
    batch.state.fogG = m_fogG;
    batch.state.fogB = m_fogB;
    batch.state.textureWidth = static_cast<uint16_t>(1u << std::min<uint32_t>(batch.state.context.tex0.tw, 10u));
    batch.state.textureHeight = static_cast<uint16_t>(1u << std::min<uint32_t>(batch.state.context.tex0.th, 10u));
    const uint64_t tex1 = batch.state.context.tex1;
    const uint8_t mmag = static_cast<uint8_t>((tex1 >> 5u) & 0x1u);
    const uint8_t mmin = static_cast<uint8_t>((tex1 >> 6u) & 0x7u);
    batch.state.linearFilter = mmag != 0u || mmin == 1u || (mmin & 0x4u) != 0u;
    // GT4's race colour (gs_fullcolour.h): decided once here, carried to every
    // renderer (and both halves of the GL-vs-CPU compare). A change of the
    // video mode (SetGsCrt) starts a new epoch: every renderer forgets each
    // pixel's full colour at the first draw of it (GT4 builds).
    const uint32_t crt = ps2x::crt::current();
#if defined(PS2X_GAME_GT4)
    if (crt != m_fcCrt)
    {
        m_fcCrt = crt;
        ++m_fcEpoch;
    }
    batch.state.fcEpoch = m_fcEpoch;
#endif
    batch.state.fc = gsfc::drawFlag(batch.state.context.frame.psm, batch.state.context.frame.fbw, crt);
    const GSTex0Reg &tex0 = batch.state.context.tex0;
    batch.state.ftex = gsfc::texFlag(batch.state.prim.tme, tex0.psm, tex0.tbp0, tex0.tbw, crt);
    return batch;
}

void GS::updatePreferredDisplaySourceForDraw(const GSPrimitiveBatch &batch)
{
    const GSDrawState &state = batch.state;
    const GSContext &ctx = state.context;
    if (m_hasPreferredDisplaySource && ctx.frame.fbp == m_preferredDisplayDestFbp)
        m_hasPreferredDisplaySource = false;
    if (state.prim.type != GS_PRIM_SPRITE || batch.vertexCount < 2u)
        return;

    const GSVertex &v0 = batch.vertices[0];
    const GSVertex &v1 = batch.vertices[1];
    int x0 = static_cast<int>(v0.x) - (ctx.xyoffset.ofx >> 4);
    int y0 = static_cast<int>(v0.y) - (ctx.xyoffset.ofy >> 4);
    int x1 = static_cast<int>(v1.x) - (ctx.xyoffset.ofx >> 4);
    int y1 = static_cast<int>(v1.y) - (ctx.xyoffset.ofy >> 4);
    if (x0 > x1) std::swap(x0, x1);
    if (y0 > y1) std::swap(y0, y1);
    const int xEnd = x0 + std::max(1, x1 - x0) - 1;
    const int yEnd = y0 + std::max(1, y1 - y0) - 1;
    const uint8_t alphaMode = static_cast<uint8_t>(ctx.alpha & 0xFFu);
    const uint8_t alphaFix = static_cast<uint8_t>((ctx.alpha >> 32u) & 0xFFu);
    const bool displayCopy = state.prim.tme && state.prim.abe && state.prim.fst && state.prim.ctxt &&
                             ctx.frame.fbp != ctx.tex0.tbp0 && alphaMode == 0x64u &&
                             (alphaFix == 0x60u || alphaFix == 0x80u) &&
                             x0 <= 0 && y0 <= 0 && xEnd >= 639 && yEnd >= 447;
    if (displayCopy)
    {
        m_preferredDisplaySourceFrame = {ctx.tex0.tbp0, ctx.tex0.tbw, ctx.tex0.psm, 0u};
        m_preferredDisplayDestFbp = ctx.frame.fbp;
        m_hasPreferredDisplaySource = true;
    }
}
