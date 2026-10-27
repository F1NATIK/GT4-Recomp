// Based on Blackline Interactive implementation
#include "runtime/ps2_memory.h"
#include "graphics_transfer_trace.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace vif1lat
{
    void noteRaise();
    void noteResume();
    extern std::atomic<uint64_t> raiseNs, drainSum;
}

namespace
{
// UNPACK fast path (see executeVIF1Commands): contiguous writes of `count`
// vectors from vuAddr (wrapping at 1024), no mask or row addition. VL 0/1/2 =
// 32/16/8-bit components, C = components, ZX = zero extension (USN).
// Components a V1-V3 format does not carry keep their value; V1 broadcasts.
template <int VL, int C, bool ZX>
void unpackRun(uint8_t *vuMem, const uint8_t *src, uint32_t vuAddr, uint32_t count)
{
    constexpr uint32_t width = VL == 0 ? 4u : (VL == 1 ? 2u : 1u);
    constexpr uint32_t stride = static_cast<uint32_t>(C) * width;
    for (uint32_t i = 0; i < count; ++i, src += stride)
    {
        uint8_t *dst = vuMem + ((vuAddr + i) & 0x3FFu) * 16u;
        uint32_t c[4];
        for (int k = 0; k < C; ++k)
        {
            if constexpr (VL == 0)
                std::memcpy(&c[k], src + k * 4, 4u);
            else if constexpr (VL == 1)
            {
                uint16_t raw;
                std::memcpy(&raw, src + k * 2, 2u);
                c[k] = ZX ? static_cast<uint32_t>(raw) : static_cast<uint32_t>(static_cast<int32_t>(static_cast<int16_t>(raw)));
            }
            else
                c[k] = ZX ? static_cast<uint32_t>(src[k]) : static_cast<uint32_t>(static_cast<int32_t>(static_cast<int8_t>(src[k])));
        }
        if constexpr (C == 1)
        {
            const uint32_t v[4] = {c[0], c[0], c[0], c[0]};
            std::memcpy(dst, v, 16u);
        }
        else
            std::memcpy(dst, c, static_cast<size_t>(C) * 4u);
    }
}

void unpackV4_32(uint8_t *vuMem, const uint8_t *src, uint32_t vuAddr, uint32_t count)
{
    while (count != 0u)
    {
        const uint32_t at = vuAddr & 0x3FFu;
        const uint32_t run = std::min(count, 1024u - at);
        std::memcpy(vuMem + at * 16u, src, static_cast<size_t>(run) * 16u);
        src += static_cast<size_t>(run) * 16u;
        vuAddr += run;
        count -= run;
    }
}

void unpackV4_5(uint8_t *vuMem, const uint8_t *src, uint32_t vuAddr, uint32_t count)
{
    for (uint32_t i = 0; i < count; ++i, src += 2)
    {
        uint16_t packed;
        std::memcpy(&packed, src, 2u);
        const uint32_t v[4] = {packed & 0x1Fu, (packed >> 5) & 0x1Fu, (packed >> 10) & 0x1Fu, (packed >> 15) & 0x01u};
        std::memcpy(vuMem + ((vuAddr + i) & 0x3FFu) * 16u, v, 16u);
    }
}

template <int VL, bool ZX>
void unpackByCount(int components, uint8_t *vuMem, const uint8_t *src, uint32_t vuAddr, uint32_t count)
{
    switch (components)
    {
    case 1:
        unpackRun<VL, 1, ZX>(vuMem, src, vuAddr, count);
        break;
    case 2:
        unpackRun<VL, 2, ZX>(vuMem, src, vuAddr, count);
        break;
    case 3:
        unpackRun<VL, 3, ZX>(vuMem, src, vuAddr, count);
        break;
    default:
        unpackRun<VL, 4, ZX>(vuMem, src, vuAddr, count);
        break;
    }
}

void unpackFastPath(uint32_t vl, uint32_t vn, bool zeroExtend, uint8_t *vuMem, const uint8_t *src, uint32_t vuAddr, uint32_t count)
{
    const int components = static_cast<int>(vn) + 1;
    if (vl == 3u)
        unpackV4_5(vuMem, src, vuAddr, count);
    else if (vl == 0u)
    {
        if (components == 4)
            unpackV4_32(vuMem, src, vuAddr, count);
        else
            unpackByCount<0, false>(components, vuMem, src, vuAddr, count);
    }
    else if (vl == 1u)
        zeroExtend ? unpackByCount<1, true>(components, vuMem, src, vuAddr, count)
                   : unpackByCount<1, false>(components, vuMem, src, vuAddr, count);
    else
        zeroExtend ? unpackByCount<2, true>(components, vuMem, src, vuAddr, count)
                   : unpackByCount<2, false>(components, vuMem, src, vuAddr, count);
}
} // namespace

enum VIFCmd : uint8_t
{
    VIF_NOP = 0x00,
    VIF_STCYCL = 0x01,
    VIF_OFFSET = 0x02,
    VIF_BASE = 0x03,
    VIF_ITOP = 0x04,
    VIF_STMOD = 0x05,
    VIF_MSKPATH3 = 0x06,
    VIF_MARK = 0x07,
    VIF_FLUSHE = 0x10,
    VIF_FLUSH = 0x11,
    VIF_FLUSHA = 0x13,
    VIF_MSCAL = 0x14,
    VIF_MSCALF = 0x15,
    VIF_MSCNT = 0x17,
    VIF_STMASK = 0x20,
    VIF_STROW = 0x30,
    VIF_STCOL = 0x31,
    VIF_MPG = 0x4A,
    VIF_DIRECT = 0x50,
    VIF_DIRECTHL = 0x51,
};

namespace
{
    constexpr uint8_t kGifFmtImage = 2u;

    // Size of a complete VIF command, including its payload. Buffer split
    // commands before executing them so I-bit completion cannot precede data.
    uint32_t vifCommandBytes(uint32_t command, const VIFRegisters &regs)
    {
        const uint32_t opcode = (command >> 24) & 0x7Fu;
        const uint32_t num = (command >> 16) & 0xFFu;
        const uint32_t count = num ? num : 256u;
        switch (opcode)
        {
        case VIF_STMASK: return 8u;
        case VIF_STROW:
        case VIF_STCOL: return 20u;
        case VIF_MPG: return 4u + count * 8u;
        case VIF_DIRECT:
        case VIF_DIRECTHL: return 4u + ((command & 0xFFFFu) ? (command & 0xFFFFu) : 65536u) * 16u;
        default: break;
        }
        if ((opcode & 0x60u) != 0x60u)
            return 4u;
        const uint32_t vn = (opcode >> 2) & 3u;
        const uint32_t vl = opcode & 3u;
        const uint32_t vectorBits = vl == 3u && vn == 3u ? 16u :
            (vn + 1u) * (vl == 0u ? 32u : vl == 1u ? 16u : vl == 2u ? 8u : 16u);
        // CL = 0 is a valid filling mode (WL > CL): no source vectors at all,
        // every write comes from ROW/COL through the mask. GT3's race VIF1
        // chains use STCYCL 0x0400; clamping CL to 1 consumed the next 16
        // bytes of commands and desynchronized VIF1 for good.
        const uint32_t cl = regs.cycle & 0xFFu;
        const uint32_t wl = ((regs.cycle >> 8) & 0xFFu) ? ((regs.cycle >> 8) & 0xFFu) : 256u; // WL = 0 means 256
        const uint32_t sourceCount = cl < wl ? (count / wl) * cl + std::min(count % wl, cl) : count;
        return 4u + ((sourceCount * ((vectorBits + 7u) / 8u) + 3u) & ~3u);
    }

    uint32_t gifImageQwcFromTag(const uint8_t *data, uint32_t sizeBytes)
    {
        if (!data || sizeBytes < 16u)
            return 0u;

        uint64_t tagLo = 0u;
        std::memcpy(&tagLo, data, sizeof(tagLo));
        const uint8_t flg = static_cast<uint8_t>((tagLo >> 58) & 0x3u);
        if (flg != kGifFmtImage)
            return 0u;

        return static_cast<uint32_t>(tagLo & 0x7FFFu);
    }
}

void PS2Memory::processVIF0Data(uint32_t srcPhys, uint32_t sizeBytes)
{
    if (sizeBytes == 0u || srcPhys >= PS2_RAM_SIZE)
        return;

    const uint64_t requestedEnd = static_cast<uint64_t>(srcPhys) + static_cast<uint64_t>(sizeBytes);
    if (requestedEnd > static_cast<uint64_t>(PS2_RAM_SIZE))
        sizeBytes = PS2_RAM_SIZE - srcPhys;

    processVIF0Data(m_rdram + srcPhys, sizeBytes);
}

void PS2Memory::processVIF0Data(const uint8_t *data, uint32_t sizeBytes)
{
    ps2_graphics_trace::record(0x10u,0,data,sizeBytes,gs_regs.vsyncTick.load());
    processVifInput(0u, data, sizeBytes);
}

void PS2Memory::processVifInput(uint32_t channel, const uint8_t *data, uint32_t sizeBytes)
{
    auto &input = m_vifInput[channel];
    auto &offset = m_vifInputOffset[channel];
    auto &regs = channel == 0u ? vif0_regs : vif1_regs;
    if (offset > input.size())
        offset = 0; // the held input was cleared elsewhere
    // Nothing held back from an earlier packet: run the commands straight from
    // the source and keep only what is left over (no copy of every packet).
    const bool direct = input.empty() && sizeBytes != 0u;
    if (!direct && sizeBytes != 0u)
        input.insert(input.end(), data, data + sizeBytes);
    const uint8_t *const buf = direct ? data : input.data() + offset;
    const size_t size = direct ? sizeBytes : input.size() - offset;
    size_t pos = 0;
    while ((regs.stat & 0x700u) == 0u && pos + 4u <= size)
    {
        // Preserve the existing PATH2 image continuation across DMA packets.
        if (channel == 1u && m_vif1PendingPath2ImageQwc != 0u)
        {
            const uint32_t qwc = std::min(m_vif1PendingPath2ImageQwc,
                static_cast<uint32_t>((size - pos) / 16u));
            if (qwc == 0u) break;
            executeVIF1Commands(buf + pos, qwc * 16u);
            pos += qwc * 16u;
            continue;
        }
        uint32_t command;
        std::memcpy(&command, buf + pos, sizeof(command));
        const uint32_t bytes = vifCommandBytes(command, regs);
        if (bytes > size - pos)
        {
            regs.code = command;
            regs.num = (command >> 16) & 0xFFu;
            break;
        }
        if (channel == 0u) executeVIF0Commands(buf + pos, bytes);
        else executeVIF1Commands(buf + pos, bytes);
        pos += bytes;
        if ((command & 0x80000000u) != 0u && (regs.err & 1u) == 0u)
        {
            regs.stat |= 1u << 11; // INT, only after the command's work finishes.
            if (((command >> 24) & 0x7Fu) != VIF_MARK)
                regs.stat |= 1u << 10; // MARK is the non-stalling I-bit command.
            m_pendingVifInterrupts.fetch_or(1u << channel);
            if (channel == 1u)
                vif1lat::noteRaise();
            if (channel == 1u && (regs.stat & (1u << 10)) && ps2_graphics_trace::dmaStats().enabled) ++ps2_graphics_trace::dmaStats().vif1Istall;
            ps2_graphics_trace::record(0x30u + channel, command, reinterpret_cast<const uint8_t *>(&regs.stat), 4u, gs_regs.vsyncTick.load());
            ps2_graphics_trace::dmaTrace(gs_regs.vsyncTick.load(), "vif-irq ch%u cmd=%08x stat=%x remaining=%u", channel, command, regs.stat,
                                         static_cast<uint32_t>(size - pos));
        }
    }
    if (direct)
    {
        if (pos < size)
            input.assign(buf + pos, buf + size);
        offset = 0;
    }
    else
    {
        // Advance past what ran; compact only once the consumed prefix is large,
        // instead of shifting the remainder at every I-bit stall.
        offset += pos;
        if (offset >= input.size())
        {
            input.clear();
            offset = 0;
        }
        else if (offset >= (256u << 10) && offset * 2u >= input.size())
        {
            input.erase(input.begin(), input.begin() + static_cast<std::ptrdiff_t>(offset));
            offset = 0;
        }
    }
}

void PS2Memory::writeVifFbrst(uint32_t channel, uint32_t value)
{
    if (channel == 1u && m_vif1Worker.active() && !m_vif1Worker.onWorkerThread())
    {
        // A plain STC (GT3's DMAC handler writes one after every stall) resumes
        // the worker without waiting for it; anything else is applied here once
        // the worker has parked.
        if (value == 8u)
        {
            ps2_graphics_trace::dmaTrace(gs_regs.vsyncTick.load(), "fbrst ch1 value=8 (VIF1 worker)");
            if (ps2_graphics_trace::dmaStats().enabled)
                ++ps2_graphics_trace::dmaStats().vif1Stc;
            vif1lat::noteResume();
            m_vif1Worker.postResume(m_vu0FbrstReader ? m_vu0FbrstReader() : 0u);
            return;
        }
        m_vif1Worker.quiesce(Vif1Worker::QuiesceSite::Vif1Fbrst);
    }
    auto &regs = channel == 0u ? vif0_regs : vif1_regs;
    ps2_graphics_trace::record(0x32u + channel, value, reinterpret_cast<const uint8_t *>(&regs.code), 4u, gs_regs.vsyncTick.load());
    ps2_graphics_trace::dmaTrace(gs_regs.vsyncTick.load(), "fbrst ch%u value=%x stat=%x held=%u", channel, value, regs.stat,
                                 static_cast<uint32_t>(m_vifInput[channel].size()));
    if (value & 1u)
    {
        uint32_t row[4], col[4];
        std::memcpy(row, regs.row, sizeof(row));
        std::memcpy(col, regs.col, sizeof(col));
        regs = {};
        std::memcpy(regs.row, row, sizeof(row));
        std::memcpy(regs.col, col, sizeof(col));
        m_vifInput[channel].clear();
        m_vifInputOffset[channel] = 0;
        m_vifDeferredDmac[channel] = false;
        m_pendingVifInterrupts.fetch_and(~(1u << channel));
        (channel == 0u ? m_pendingVif0Transfers : m_pendingVif1Transfers).clear();
        m_ioRegisters[0x10008000u + channel * 0x1000u] &= ~0x100u;
        if (channel == 1u)
        {
            m_vif1PendingPath2ImageQwc = 0u;
            m_vif1PendingPath2DirectHl = false;
        }
    }
    if (value & 2u) regs.stat |= 1u << 9;
    if (value & 4u) regs.stat |= 1u << 8;
    if (value & 8u)
    {
        if (channel == 1u && ps2_graphics_trace::dmaStats().enabled) ++ps2_graphics_trace::dmaStats().vif1Stc;
        regs.stat &= ~0x3F00u;
        processVifInput(channel, nullptr, 0u);
        completeVifDmaIfReady(channel);
    }
}

void PS2Memory::vif1WorkerIngest(std::vector<uint8_t> &bytes, bool completesDma)
{
    auto &input = m_vifInput[1];
    auto &offset = m_vifInputOffset[1];
    if (offset >= input.size())
    {
        input.clear();
        offset = 0;
    }
    if (!bytes.empty())
    {
        ps2_graphics_trace::record(0x11u, 0, bytes.data(), static_cast<uint32_t>(bytes.size()), gs_regs.vsyncTick.load());
        if (input.empty())
            input.swap(bytes); // bytes gets the old (empty) buffer back
        else
            input.insert(input.end(), bytes.begin(), bytes.end());
    }
    if (completesDma)
        m_vifDeferredDmac[1] = true;
}

void PS2Memory::vif1WorkerResume()
{
    ps2_graphics_trace::dmaTrace(gs_regs.vsyncTick.load(), "vif1 worker resume stat=%x held=%u", vif1_regs.stat,
                                 static_cast<uint32_t>(m_vifInput[1].size()));
    vif1_regs.stat &= ~0x3F00u;
}

uint32_t PS2Memory::vif1WorkerRun()
{
    const bool intBefore = (vif1_regs.stat & (1u << 11)) != 0u;
    processVifInput(1u, nullptr, 0u);
    // Everything this run queued reaches the GS before the EE hears of it.
    const uint64_t drainFrom = vif1lat::raiseNs.load(std::memory_order_relaxed) ? std::chrono::duration_cast<std::chrono::nanoseconds>(
                                   std::chrono::steady_clock::now().time_since_epoch()).count() : 0;
    if (m_gifArbiter)
        m_gifArbiter->drain();
    if (drainFrom)
        vif1lat::drainSum += std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count() - drainFrom;
    uint32_t result = 0u;
    if (!intBefore && (vif1_regs.stat & (1u << 11)) != 0u)
        result |= 1u;
    // As completeVifDmaIfReady: an interrupt/stop stall holds the DMA while
    // commands follow it; a command waiting for more bytes does not.
    if (m_vifDeferredDmac[1])
    {
        if (!m_vifInput[1].empty() && (vif1_regs.stat & 0x700u) != 0u)
        {
            ps2_graphics_trace::dmaTrace(gs_regs.vsyncTick.load(), "hold ch1 (VIF stalled, stat=%x, %u bytes held)", vif1_regs.stat,
                                         static_cast<uint32_t>(m_vifInput[1].size()));
        }
        else
        {
            m_vifDeferredDmac[1] = false;
            result |= 2u;
        }
    }
    return result;
}

void PS2Memory::executeVIF0Commands(const uint8_t *data, uint32_t sizeBytes)
{
    if (sizeBytes == 0u)
        return;

    uint32_t pos = 0;
    while (pos + 4 <= sizeBytes)
    {
        uint32_t cmd = 0u;
        std::memcpy(&cmd, data + pos, sizeof(cmd));
        pos += 4u;

        const uint8_t opcode = static_cast<uint8_t>((cmd >> 24) & 0x7Fu);
        const uint16_t imm = static_cast<uint16_t>(cmd & 0xFFFFu);
        const uint8_t num = static_cast<uint8_t>((cmd >> 16) & 0xFFu);

        vif0_regs.code = cmd;
        vif0_regs.num = num;
        {
            // (tests, PS2X_VIF0_LOG=<n>: the first n VIF0 commands)
            static const long vif0Log = std::getenv("PS2X_VIF0_LOG") ? std::atol(std::getenv("PS2X_VIF0_LOG")) : 0L;
            static long vif0Logged = 0;
            static bool vif0LogOn = false; // (from the first 16-bit, 8-bit or V4-5 UNPACK on)
            if (vif0Log && (opcode & 0x60u) == 0x60u && (opcode & 3u) != 0u)
                vif0LogOn = true;
            if (vif0LogOn && vif0Logged < vif0Log)
            {
                ++vif0Logged;
                std::fprintf(stderr, "[vif0-log] %08x cycle %04x mask %08x mode %u stat %x\n", cmd, vif0_regs.cycle, vif0_regs.mask, vif0_regs.mode,
                             vif0_regs.stat);
            }
        }

        if (opcode == VIF_NOP)
        {
            continue;
        }
        else if (opcode == VIF_STCYCL)
        {
            vif0_regs.cycle = imm;
            continue;
        }
        else if (opcode == VIF_ITOP)
        {
            vif0_regs.itops = imm & 0x3FFu;
            continue;
        }
        else if (opcode == VIF_STMOD)
        {
            vif0_regs.mode = imm & 3u;
            continue;
        }
        else if (opcode == VIF_MARK)
        {
            vif0_regs.mark = imm;
            vif0_regs.stat |= (1u << 6);
            continue;
        }
        else if (opcode == VIF_FLUSHE || opcode == VIF_FLUSH || opcode == VIF_FLUSHA)
        {
            continue;
        }
        else if (opcode == VIF_STMASK)
        {
            if (pos + 4u > sizeBytes)
                break;
            std::memcpy(&vif0_regs.mask, data + pos, sizeof(vif0_regs.mask));
            pos += 4u;
            continue;
        }
        else if (opcode == VIF_STROW)
        {
            if (pos + 16u > sizeBytes)
                break;
            std::memcpy(vif0_regs.row, data + pos, 16u);
            pos += 16u;
            continue;
        }
        else if (opcode == VIF_STCOL)
        {
            if (pos + 16u > sizeBytes)
                break;
            std::memcpy(vif0_regs.col, data + pos, 16u);
            pos += 16u;
            continue;
        }
        else if (opcode == VIF_MPG)
        {
            const uint32_t destAddr = static_cast<uint32_t>(imm & 0x1FFu) * 8u;
            const uint32_t instructionCount = (num == 0u) ? 256u : static_cast<uint32_t>(num);
            const uint32_t mpgBytes = instructionCount * 8u;
            uint32_t copyBytes = 0u;
            if (m_vu0Code && destAddr < PS2_VU0_CODE_SIZE && mpgBytes > 0u)
            {
                copyBytes = mpgBytes;
                if (destAddr + copyBytes > PS2_VU0_CODE_SIZE)
                    copyBytes = PS2_VU0_CODE_SIZE - destAddr;
                if (pos + copyBytes <= sizeBytes)
                {
                    std::memcpy(m_vu0Code + destAddr, data + pos, copyBytes);
                    markVU0CodeModified();
                }
            }

            pos += mpgBytes;
            if (pos > sizeBytes)
                break;
            continue;
        }
        else if ((opcode & 0x60u) == 0x60u)
        {
            const uint8_t vn = static_cast<uint8_t>((opcode >> 2) & 0x3u);
            const uint8_t vl = static_cast<uint8_t>(opcode & 0x3u);
            const int components = static_cast<int>(vn) + 1;
            int bitsPerComponent = 32;
            switch (vl)
            {
            case 0:
                bitsPerComponent = 32;
                break;
            case 1:
                bitsPerComponent = 16;
                break;
            case 2:
                bitsPerComponent = 8;
                break;
            case 3:
                bitsPerComponent = (vn == 3u) ? 4 : 16;
                break;
            default:
                break;
            }
            const int bitsPerVector = (vl == 3u && vn == 3u) ? 16 : (components * bitsPerComponent);
            uint32_t bytesPerVector = static_cast<uint32_t>((bitsPerVector + 7) / 8);
            const uint32_t writeVectorCount = (num == 0u) ? 256u : static_cast<uint32_t>(num);
            // CL = 0 (filling, no source data) is valid; see vifCommandBytes.
            uint32_t cl = vif0_regs.cycle & 0xFFu;
            uint32_t wl = (vif0_regs.cycle >> 8) & 0xFFu;
            if (wl == 0u)
                wl = 256u; // WL = 0 means 256
            uint32_t sourceVectorCount = writeVectorCount;
            if (cl < wl)
            {
                const uint32_t fullBlocks = writeVectorCount / wl;
                uint32_t remainder = writeVectorCount % wl;
                if (remainder > cl)
                    remainder = cl;
                sourceVectorCount = fullBlocks * cl + remainder;
            }
            uint32_t totalBytes = sourceVectorCount * bytesPerVector;
            totalBytes = (totalBytes + 3u) & ~3u;

            // The 16-bit, 8-bit and V4-5 formats, masked writes (STMASK: ROW, COL or write protect per field) and ROW
            // addition (STMOD 1/2), as VIF1 does them. (Tourist Trophy's skinned riders send their vertices to VU0 so:
            // V3-16 positions, V3-8 normals, V4-5 bone indices; skipping them left VU0 holding other vertices.) V4-5
            // fields are the hardware's 8-bit expansion (5 bits << 3, the A bit << 7). VIF0 ignores FLG (no TOPS).
            // 32-bit UNPACKs keep the path below, as before.
            const bool maskEnable = (opcode & 0x10u) != 0u;
            const uint32_t unpackMode = vif0_regs.mode & 3u;
            if (vl != 0u)
            {
                if (m_vu0Data && pos + totalBytes <= sizeBytes)
                {
                    const uint32_t vuAddr = static_cast<uint32_t>(imm & 0x3FFu);
                    const bool zeroExtend = (imm & 0x4000u) != 0u;
                    const bool v45 = vl == 3u && vn == 3u;
                    const uint8_t *srcBase = data + pos;
                    uint32_t srcIndex = 0u;
                    for (uint32_t writeIndex = 0; writeIndex < writeVectorCount; ++writeIndex)
                    {
                        const uint32_t cyclePos = writeIndex % wl;
                        const bool sourceAvailable = (cl >= wl) || (cyclePos < cl);
                        const uint32_t destVec = (cl >= wl) ? ((vuAddr + (writeIndex / wl) * cl + cyclePos) & 0xFFu)
                                                            : ((vuAddr + writeIndex) & 0xFFu);
                        const uint32_t destOff = destVec * 16u;
                        uint32_t lanes[4];
                        std::memcpy(lanes, m_vu0Data + destOff, sizeof(lanes));
                        uint32_t decompressed[4] = {lanes[0], lanes[1], lanes[2], lanes[3]};
                        const uint8_t *srcVec = nullptr;
                        if (sourceAvailable && srcIndex < sourceVectorCount)
                            srcVec = srcBase + (srcIndex++) * bytesPerVector;
                        const bool decoded = srcVec != nullptr && (vl != 3u || v45);
                        if (decoded)
                        {
                            if (v45)
                            {
                                uint16_t packed = 0;
                                std::memcpy(&packed, srcVec, sizeof(packed));
                                decompressed[0] = (packed & 0x001Fu) << 3;
                                decompressed[1] = (packed & 0x03E0u) >> 2;
                                decompressed[2] = (packed & 0x7C00u) >> 7;
                                decompressed[3] = (packed & 0x8000u) >> 8;
                            }
                            else
                            {
                                uint32_t c[4] = {0u, 0u, 0u, 0u};
                                for (int k = 0; k < components && k < 4; ++k)
                                {
                                    if (vl == 0u)
                                        std::memcpy(&c[k], srcVec + k * 4, 4u);
                                    else if (vl == 1u)
                                    {
                                        uint16_t r16 = 0;
                                        std::memcpy(&r16, srcVec + k * 2, 2u);
                                        c[k] = zeroExtend ? static_cast<uint32_t>(r16) : static_cast<uint32_t>(static_cast<int32_t>(static_cast<int16_t>(r16)));
                                    }
                                    else
                                        c[k] = zeroExtend ? static_cast<uint32_t>(srcVec[k]) : static_cast<uint32_t>(static_cast<int32_t>(static_cast<int8_t>(srcVec[k])));
                                }
                                if (components == 1)
                                    decompressed[0] = decompressed[1] = decompressed[2] = decompressed[3] = c[0];
                                else
                                    for (int k = 0; k < components && k < 4; ++k)
                                        decompressed[k] = c[k];
                            }
                        }
                        const uint32_t maskCycle = cyclePos > 3u ? 3u : cyclePos;
                        for (uint32_t field = 0u; field < 4u; ++field)
                        {
                            uint32_t maskSpec = maskEnable ? (vif0_regs.mask >> (((maskCycle * 4u) + field) * 2u)) & 0x3u : 0u;
                            if (!decoded && maskSpec == 0u)
                                maskSpec = 1u; // (a fill cycle with no source data: ROW)
                            if (maskSpec == 0u)
                            {
                                uint32_t v = decompressed[field];
                                if (!v45 && (unpackMode == 1u || unpackMode == 2u))
                                {
                                    v += vif0_regs.row[field];
                                    if (unpackMode == 2u)
                                        vif0_regs.row[field] = v;
                                }
                                lanes[field] = v;
                            }
                            else if (maskSpec == 1u)
                                lanes[field] = vif0_regs.row[field];
                            else if (maskSpec == 2u)
                                lanes[field] = vif0_regs.col[maskCycle];
                            // (3: write protect)
                        }
                        std::memcpy(m_vu0Data + destOff, lanes, sizeof(lanes));
                    }
                }
                pos += totalBytes;
                if (pos > sizeBytes)
                    break;
                continue;
            }
            if (m_vu0Data && pos + totalBytes <= sizeBytes && vl == 0u)
            {
                uint32_t vuAddr = static_cast<uint32_t>(imm & 0x3FFu);
                if ((imm & 0x8000u) != 0u)
                    vuAddr = (vuAddr + (vif0_regs.tops & 0x3FFu)) & 0x3FFu;
                const uint8_t *srcBase = data + pos;
                uint32_t srcIndex = 0u;
                for (uint32_t writeIndex = 0; writeIndex < writeVectorCount; ++writeIndex)
                {
                    const uint32_t cyclePos = writeIndex % wl;
                    const bool sourceAvailable = (cl >= wl) || (cyclePos < cl);
                    uint32_t destVec = (cl >= wl) ? ((vuAddr + (writeIndex / wl) * cl + cyclePos) & 0x3FFu)
                                                  : ((vuAddr + writeIndex) & 0x3FFu);
                    const uint32_t destOff = destVec * 16u;
                    if (destOff + 16u > PS2_VU0_DATA_SIZE)
                    {
                        if (sourceAvailable && srcIndex < sourceVectorCount)
                            ++srcIndex;
                        continue;
                    }
                    if (!sourceAvailable || srcIndex >= sourceVectorCount)
                        continue;
                    const uint8_t *srcVec = srcBase + srcIndex * bytesPerVector;
                    ++srcIndex;
                    uint32_t lanes[4] = {0u, 0u, 0u, 0u};
                    std::memcpy(lanes, m_vu0Data + destOff, sizeof(lanes));
                    const uint32_t limit = (components > 4) ? 4u : static_cast<uint32_t>(components);
                    for (uint32_t c = 0; c < limit; ++c)
                    {
                        uint32_t scalar = 0u;
                        std::memcpy(&scalar, srcVec + c * 4u, sizeof(scalar));
                        lanes[c] = scalar;
                    }
                    _mm_storeu_si128(reinterpret_cast<__m128i *>(m_vu0Data + destOff), _mm_loadu_si128(reinterpret_cast<const __m128i *>(lanes)));
                }
            }
            pos += totalBytes;
            if (pos > sizeBytes)
                break;
            continue;
        }
        else
        {
            static uint32_t unknownLogs = 0;
            if (unknownLogs++ < 32)
                std::fprintf(stderr, "[vif0] unhandled VIFcode %08x (opcode %02x) - rest of transfer dropped\n", cmd, opcode);
            break;
        }
    }
}

void PS2Memory::processVIF1Data(uint32_t srcPhys, uint32_t sizeBytes)
{
    if (sizeBytes == 0u || srcPhys >= PS2_RAM_SIZE)
        return;

    const uint64_t requestedEnd = static_cast<uint64_t>(srcPhys) + static_cast<uint64_t>(sizeBytes);
    if (requestedEnd > static_cast<uint64_t>(PS2_RAM_SIZE))
        sizeBytes = PS2_RAM_SIZE - srcPhys;

    processVIF1Data(m_rdram + srcPhys, sizeBytes);
}

void PS2Memory::processVIF1Data(const uint8_t *data, uint32_t sizeBytes)
{
    ps2_graphics_trace::record(0x11u,0,data,sizeBytes,gs_regs.vsyncTick.load());
    processVifInput(1u, data, sizeBytes);
}

void PS2Memory::executeVIF1Commands(const uint8_t *data, uint32_t sizeBytes)
{
    if (sizeBytes == 0u)
        return;

    uint32_t pos = 0;

    while (pos + 4 <= sizeBytes)
    {
        if (m_vif1PendingPath2ImageQwc != 0u)
        {
            const uint32_t availableQw = (sizeBytes - pos) / 16u;
            if (availableQw == 0u)
            {
                break;
            }

            const uint32_t chunkQw = std::min<uint32_t>(m_vif1PendingPath2ImageQwc, availableQw);
            // The GS keeps the IMAGE tag in progress across submissions, so the
            // raw qwords continue it directly (no synthesized tag).
            submitGifPacket(GifPathId::Path2,
                            data + pos,
                            chunkQw * 16u,
                            true,
                            m_vif1PendingPath2DirectHl);

            pos += chunkQw * 16u;
            m_vif1PendingPath2ImageQwc -= chunkQw;
            if (m_vif1PendingPath2ImageQwc == 0u)
            {
                m_vif1PendingPath2DirectHl = false;
            }
            continue;
        }

        uint32_t cmd;
        memcpy(&cmd, data + pos, 4);
        pos += 4;

        uint8_t opcode = (cmd >> 24) & 0x7F;
        uint16_t imm = cmd & 0xFFFF;
        uint8_t num = (cmd >> 16) & 0xFF;

        // Track most-recent command for VIFn_CODE emulation.
        vif1_regs.code = cmd;
        vif1_regs.num = num;

        if (opcode == VIF_NOP)
        {
            continue;
        }
        else if (opcode == VIF_STCYCL)
        {
            vif1_regs.cycle = imm;
            continue;
        }
        else if (opcode == VIF_OFFSET)
        {
            // VIF double-buffer setup. OFFSET clears DBF and resets TOPS to BASE.
            // Do not rewrite BASE from the previous TOPS value.
            vif1_regs.ofst = imm & 0x3FFu;
            vif1_regs.tops = vif1_regs.base & 0x3FFu;
            vif1_regs.stat &= ~(1u << 7); // clear DBF
            continue;
        }
        else if (opcode == VIF_BASE)
        {
            // BASE only updates the base register. TOPS changes on OFFSET/MSCAL.
            vif1_regs.base = imm & 0x3FFu;
            continue;
        }
        else if (opcode == VIF_ITOP)
        {
            // ITOP VIFcode writes pending ITOPS; VU XITOP observes it after MSCAL/MSCNT.
            vif1_regs.itops = imm & 0x3FFu;
            continue;
        }
        else if (opcode == VIF_STMOD)
        {
            vif1_regs.mode = imm & 3u;
            continue;
        }
        else if (opcode == VIF_MSKPATH3)
        {
            // VIF command docs: MSKPATH3 uses IMMEDIATE bit 15.
            const bool wasMasked = m_path3Masked;
            m_path3Masked = (imm & 0x8000u) != 0u;
            if (wasMasked != m_path3Masked)
                ps2_graphics_trace::dmaTrace(gs_regs.vsyncTick.load(), "mskpath3 %d", m_path3Masked ? 1 : 0);
            if (wasMasked && !m_path3Masked)
                flushMaskedPath3Packets();
            continue;
        }
        else if (opcode == VIF_MARK)
        {
            vif1_regs.mark = imm;
            vif1_regs.stat |= (1u << 6); // MRK
            continue;
        }
        else if (opcode == VIF_FLUSHE || opcode == VIF_FLUSH || opcode == VIF_FLUSHA)
        {
            continue;
        }
        else if (opcode == VIF_MSCAL || opcode == VIF_MSCALF)
        {
            uint32_t startPC = (uint32_t)imm * 8u;

            // Values visible to the VU program for this MSCAL.
            // DobieStation semantics: ITOP = ITOPS; TOP = current TOPS;
            // then TOPS/DBF are prepared for the next buffer.
            const uint32_t runTop = vif1_regs.tops & 0x3FFu;
            const uint32_t runItop = vif1_regs.itops & 0x3FFu;
            vif1_regs.top = runTop;
            vif1_regs.itop = runItop;

            const bool dbf = (vif1_regs.stat & (1u << 7)) != 0u;
            if (dbf)
                vif1_regs.tops = vif1_regs.base & 0x3FFu;
            else
                vif1_regs.tops = (vif1_regs.base + vif1_regs.ofst) & 0x3FFu;
            vif1_regs.stat ^= (1u << 7); // toggle DBF

            if (m_vu1MscalCallback)
                m_vu1MscalCallback(startPC, runTop, runItop);
            continue;
        }
        else if (opcode == VIF_MSCNT)
        {
            const uint32_t runTop = vif1_regs.tops & 0x3FFu;
            const uint32_t runItop = vif1_regs.itops & 0x3FFu;
            vif1_regs.top = runTop;
            vif1_regs.itop = runItop;

            const bool dbf = (vif1_regs.stat & (1u << 7)) != 0u;
            if (dbf)
                vif1_regs.tops = vif1_regs.base & 0x3FFu;
            else
                vif1_regs.tops = (vif1_regs.base + vif1_regs.ofst) & 0x3FFu;
            vif1_regs.stat ^= (1u << 7); // toggle DBF

            if (m_vu1MscntCallback)
                m_vu1MscntCallback(runTop, runItop);
            continue;
        }
        else if (opcode == VIF_STMASK)
        {
            if (pos + 4 > sizeBytes)
                break;
            uint32_t maskValue = 0;
            std::memcpy(&maskValue, data + pos, sizeof(maskValue));
            vif1_regs.mask = maskValue;
            pos += 4;
            continue;
        }
        else if (opcode == VIF_STROW)
        {
            if (pos + 16 > sizeBytes)
                break;
            std::memcpy(vif1_regs.row, data + pos, 16);
            pos += 16;
            continue;
        }
        else if (opcode == VIF_STCOL)
        {
            if (pos + 16 > sizeBytes)
                break;
            std::memcpy(vif1_regs.col, data + pos, 16);
            pos += 16;
            continue;
        }
        else if (opcode == VIF_MPG)
        {
            uint32_t destAddr = (uint32_t)imm * 8u;
            // VIF MPG semantics: NUM==0 means 256 instructions (2048 bytes).
            // MPG payload is instruction-packed and should not be QW-aligned.
            const uint32_t instructionCount = (num == 0u) ? 256u : static_cast<uint32_t>(num);
            const uint32_t mpgBytes = instructionCount * 8u;
            if (m_vu1Code && destAddr < PS2_VU1_CODE_SIZE && mpgBytes > 0)
            {
                uint32_t copyBytes = mpgBytes;
                if (destAddr + copyBytes > PS2_VU1_CODE_SIZE)
                    copyBytes = PS2_VU1_CODE_SIZE - destAddr;
                if (pos + copyBytes <= sizeBytes)
                {
                    std::memcpy(m_vu1Code + destAddr, data + pos, copyBytes);
                    markVU1CodeModified();
                }
            }
            pos += mpgBytes;
            if (pos > sizeBytes)
                break;
            continue;
        }
        else if (opcode == VIF_DIRECT || opcode == VIF_DIRECTHL)
        {
            uint32_t qwCount = imm;
            if (qwCount == 0)
                qwCount = 65536;
            const uint32_t availableQw = (sizeBytes - pos) / 16u;
            const bool truncated = qwCount > availableQw;
            if (qwCount > availableQw)
                qwCount = availableQw;

            if (qwCount > 0)
            {
                const bool directHl = (opcode == VIF_DIRECTHL);
                submitGifPacket(GifPathId::Path2, data + pos, qwCount * 16, true, directHl);

                const uint32_t imageQw = gifImageQwcFromTag(data + pos, qwCount * 16u);
                if (imageQw != 0u)
                {
                    const uint32_t inlineImageQw = (qwCount > 0u) ? (qwCount - 1u) : 0u;
                    if (imageQw > inlineImageQw)
                    {
                        m_vif1PendingPath2ImageQwc = imageQw - inlineImageQw;
                        m_vif1PendingPath2DirectHl = directHl;
                    }
                }
            }

            pos += qwCount * 16;
            if (truncated)
            {
                pos = sizeBytes;
                break;
            }
            continue;
        }
        else if ((opcode & 0x60) == 0x60)
        {
            uint8_t vn = (opcode >> 2) & 0x3;
            uint8_t vl = opcode & 0x3;
            const bool maskEnable = (opcode & 0x10u) != 0u;
            int components = vn + 1;
            int bitsPerComponent = 32;
            switch (vl)
            {
            case 0:
                bitsPerComponent = 32;
                break;
            case 1:
                bitsPerComponent = 16;
                break;
            case 2:
                bitsPerComponent = 8;
                break;
            case 3:
                bitsPerComponent = (vn == 3) ? 4 : 16;
                break;
            default:
                break;
            }
            int bitsPerVector = (vl == 3 && vn == 3) ? 16 : (components * bitsPerComponent);
            uint32_t bytesPerVector = (bitsPerVector + 7) / 8;
            // UNPACK semantics: NUM is 8-bit and NUM==0 means 256 vectors (writes).
            const uint32_t writeVectorCount = (num == 0u) ? 256u : static_cast<uint32_t>(num);

            // STCYCL controls write cycles for UNPACK.
            // CL = 0 (filling, no source data) is valid; see vifCommandBytes.
            uint32_t cl = vif1_regs.cycle & 0xFFu;
            uint32_t wl = (vif1_regs.cycle >> 8) & 0xFFu;
            if (wl == 0u)
                wl = 256u; // WL = 0 means 256

            uint32_t sourceVectorCount = writeVectorCount;
            if (cl < wl)
            {
                const uint32_t fullBlocks = writeVectorCount / wl;
                uint32_t remainder = writeVectorCount % wl;
                if (remainder > cl)
                    remainder = cl;
                sourceVectorCount = fullBlocks * cl + remainder;
            }

            uint32_t totalBytes = sourceVectorCount * bytesPerVector;
            totalBytes = (totalBytes + 3) & ~3u;

            uint32_t vuAddr = (uint32_t)imm & 0x3FFu;
            if ((imm & 0x8000u) != 0u)
                vuAddr = (vuAddr + (vif1_regs.tops & 0x3FFu)) & 0x3FFu;

            const bool zeroExtend = (imm & 0x4000u) != 0u;

            // Fast path for GT3's usual UNPACK: contiguous writes (CL = WL), no
            // mask, no row addition (MODE 0/3), a decoded format. Each vector
            // is decoded straight into VU memory; components a V1-V3 format
            // does not carry keep their value, as in the general loop below.
            // The general loop's per-vector divisions and field selection were
            // most of this function (about 8% of the VIF1/VU1 thread in a race).
            // GT3_VIF_UNPACK_VERIFY=1: both paths run and their VU memory is compared.
            const uint32_t unpackMode = vif1_regs.mode & 3u;
            const bool fastUnpack = m_vu1Data && pos + totalBytes <= sizeBytes && !maskEnable && cl == wl &&
                                    (unpackMode == 0u || unpackMode == 3u) && (vl != 3u || vn == 3u);
            auto unpackFast = [&](uint8_t *vuMem) { unpackFastPath(vl, vn, zeroExtend, vuMem, data + pos, vuAddr, writeVectorCount); };
            static const bool verifyUnpack = [] {
                const char *v = std::getenv("GT3_VIF_UNPACK_VERIFY");
                return v && *v == '1';
            }();
            std::vector<uint8_t> fastResult;
            if (fastUnpack)
            {
                if (!verifyUnpack)
                {
                    unpackFast(m_vu1Data);
                    pos += totalBytes;
                    if (pos > sizeBytes)
                        break;
                    continue;
                }
                fastResult.assign(m_vu1Data, m_vu1Data + PS2_VU1_DATA_SIZE);
                unpackFast(fastResult.data());
            }

            // A fill-only UNPACK (CL = 0) has no source bytes but still writes.
            if (m_vu1Data && pos + totalBytes <= sizeBytes)
            {
                const uint8_t *srcBase = data + pos;
                uint32_t srcIndex = 0u;
                for (uint32_t writeIndex = 0; writeIndex < writeVectorCount; ++writeIndex)
                {
                    const uint32_t cyclePos = writeIndex % wl;
                    const bool sourceAvailable = (cl >= wl) || (cyclePos < cl);

                    uint32_t destVec = 0;
                    if (cl >= wl)
                    {
                        destVec = (vuAddr + (writeIndex / wl) * cl + cyclePos) & 0x3FFu;
                    }
                    else
                    {
                        destVec = (vuAddr + writeIndex) & 0x3FFu;
                    }

                    uint32_t destOff = destVec * 16u;
                    if (destOff + 16u > PS2_VU1_DATA_SIZE)
                    {
                        if (sourceAvailable && srcIndex < sourceVectorCount)
                            ++srcIndex;
                        continue;
                    }

                    uint32_t lanes[4] = {0u, 0u, 0u, 0u};
                    std::memcpy(lanes, m_vu1Data + destOff, sizeof(lanes));
                    uint32_t decompressed[4] = {lanes[0], lanes[1], lanes[2], lanes[3]};
                    bool decoded = false;

                    const uint8_t *srcVec = nullptr;
                    if (sourceAvailable && srcIndex < sourceVectorCount)
                    {
                        srcVec = srcBase + srcIndex * bytesPerVector;
                        ++srcIndex;
                        decoded = true;
                    }

                    auto extend16 = [&](uint16_t raw) -> uint32_t
                    {
                        if (zeroExtend)
                            return static_cast<uint32_t>(raw);
                        return static_cast<uint32_t>(static_cast<int32_t>(static_cast<int16_t>(raw)));
                    };

                    auto extend8 = [&](uint8_t raw) -> uint32_t
                    {
                        if (zeroExtend)
                            return static_cast<uint32_t>(raw);
                        return static_cast<uint32_t>(static_cast<int32_t>(static_cast<int8_t>(raw)));
                    };

                    bool handledFormat = true;
                    if (!decoded)
                    {
                        handledFormat = false;
                    }
                    else if (vl == 0u)
                    {
                        if (components == 1)
                        {
                            uint32_t scalar = 0;
                            std::memcpy(&scalar, srcVec, sizeof(scalar));
                            decompressed[0] = scalar;
                            decompressed[1] = scalar;
                            decompressed[2] = scalar;
                            decompressed[3] = scalar;
                        }
                        else
                        {
                            const uint32_t limit = (components > 4) ? 4u : static_cast<uint32_t>(components);
                            for (uint32_t c = 0; c < limit; ++c)
                            {
                                uint32_t scalar = 0;
                                std::memcpy(&scalar, srcVec + c * 4u, sizeof(scalar));
                                decompressed[c] = scalar;
                            }
                        }
                    }
                    else if (vl == 1u)
                    {
                        if (components == 1)
                        {
                            uint16_t raw = 0;
                            std::memcpy(&raw, srcVec, sizeof(raw));
                            const uint32_t scalar = extend16(raw);
                            decompressed[0] = scalar;
                            decompressed[1] = scalar;
                            decompressed[2] = scalar;
                            decompressed[3] = scalar;
                        }
                        else
                        {
                            const uint32_t limit = (components > 4) ? 4u : static_cast<uint32_t>(components);
                            for (uint32_t c = 0; c < limit; ++c)
                            {
                                uint16_t raw = 0;
                                std::memcpy(&raw, srcVec + c * 2u, sizeof(raw));
                                decompressed[c] = extend16(raw);
                            }
                        }
                    }
                    else if (vl == 2u)
                    {
                        if (components == 1)
                        {
                            const uint32_t scalar = extend8(srcVec[0]);
                            decompressed[0] = scalar;
                            decompressed[1] = scalar;
                            decompressed[2] = scalar;
                            decompressed[3] = scalar;
                        }
                        else
                        {
                            const uint32_t limit = (components > 4) ? 4u : static_cast<uint32_t>(components);
                            for (uint32_t c = 0; c < limit; ++c)
                            {
                                decompressed[c] = extend8(srcVec[c]);
                            }
                        }
                    }
                    else if (vl == 3u && vn == 3u)
                    {
                        // V4-5: packed color-like format in a single 16-bit value.
                        uint16_t packed = 0;
                        std::memcpy(&packed, srcVec, sizeof(packed));
                        decompressed[0] = packed & 0x1Fu;
                        decompressed[1] = (packed >> 5) & 0x1Fu;
                        decompressed[2] = (packed >> 10) & 0x1Fu;
                        decompressed[3] = (packed >> 15) & 0x01u;
                    }
                    else
                    {
                        handledFormat = false;
                    }

                    // Unknown compressed format fallback: preserve legacy raw-copy behavior.
                    if (!handledFormat && decoded && !maskEnable && (vif1_regs.mode == 0u || vif1_regs.mode == 3u))
                    {
                        uint32_t copyBytes = (bytesPerVector < 16u) ? bytesPerVector : 16u;
                        std::memcpy(m_vu1Data + destOff, srcVec, copyBytes);
                        continue;
                    }

                    const bool canAdd = (vl != 3u || vn != 3u);
                    const uint32_t mode = vif1_regs.mode & 3u;
                    const uint32_t colIdx = (cyclePos > 3u) ? 3u : cyclePos;
                    const uint32_t maskCycle = (cyclePos > 3u) ? 3u : cyclePos;

                    for (uint32_t field = 0u; field < 4u; ++field)
                    {
                        uint32_t maskSpec = 0u;
                        if (maskEnable)
                        {
                            const uint32_t shift = ((maskCycle * 4u) + field) * 2u;
                            maskSpec = (vif1_regs.mask >> shift) & 0x3u;
                        }

                        // In fill-write cycles with suspended source reads, treat raw-data selections as row-fill.
                        if (!decoded && maskSpec == 0u)
                            maskSpec = 1u;

                        uint32_t writeVal = lanes[field];
                        if (maskSpec == 0u)
                        {
                            if (handledFormat)
                            {
                                writeVal = decompressed[field];
                                if (canAdd && (mode == 1u || mode == 2u))
                                {
                                    writeVal = writeVal + vif1_regs.row[field];
                                    if (mode == 2u)
                                        vif1_regs.row[field] = writeVal;
                                }
                            }
                        }
                        else if (maskSpec == 1u)
                        {
                            writeVal = vif1_regs.row[field];
                        }
                        else if (maskSpec == 2u)
                        {
                            writeVal = vif1_regs.col[colIdx];
                        }
                        else
                        {
                            continue; // write-protect
                        }

                        lanes[field] = writeVal;
                    }

                    std::memcpy(m_vu1Data + destOff, lanes, sizeof(lanes));
                }
            }
            if (!fastResult.empty())
            {
                static uint64_t checked = 0, mismatches = 0;
                ++checked;
                if (std::memcmp(fastResult.data(), m_vu1Data, PS2_VU1_DATA_SIZE) != 0 && mismatches++ < 20u)
                    std::fprintf(stderr, "[vif-unpack-verify] MISMATCH vn %u vl %u num %u addr %03x cl %u wl %u usn %d\n", vn, vl,
                                 writeVectorCount, vuAddr, cl, wl, zeroExtend ? 1 : 0);
                if ((checked & 0xFFFFu) == 0u)
                    std::fprintf(stderr, "[vif-unpack-verify] %llu unpacks, %llu mismatches\n", static_cast<unsigned long long>(checked),
                                 static_cast<unsigned long long>(mismatches));
            }
            pos += totalBytes;

            if (pos > sizeBytes)
                break;
            continue;
        }
        else
        {
            continue;
        }
    }
}
