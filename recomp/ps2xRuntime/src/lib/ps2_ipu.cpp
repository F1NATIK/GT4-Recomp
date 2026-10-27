#include "runtime/ps2_ipu.h"
#include "runtime/ipu_idec.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>

// IPU command and register layout: PS2 EE User's Manual, IPU chapter (as
// summarised by the PCSX2 IPU sources). VLC tables: ISO/IEC 13818-2 annex B.

namespace ps2x::video
{
    namespace
    {
        constexpr uint32_t D3_CHCR = 0x1000B000u, D3_MADR = 0x1000B010u, D3_QWC = 0x1000B020u;
        constexpr uint32_t D4_CHCR = 0x1000B400u, D4_MADR = 0x1000B410u, D4_QWC = 0x1000B420u, D4_TADR = 0x1000B430u;

        // CTRL fields kept in ctrl_
        constexpr uint32_t CtrlCbpShift = 8, CtrlEcd = 1u << 14, CtrlScd = 1u << 15;
        constexpr uint32_t CtrlWritable = 0x07f30000u; // IDP(16-17) AS(20) IVF(21) QST(22) MP1(23) PCT(24-26)

        struct Vlc
        {
            const char *code;
            int value;
        };
        // coded_block_pattern (table B-9), MPEG-2 4:2:0 (cbp 0 included).
        constexpr Vlc kCbp[] = {
            {"111", 60}, {"1101", 4}, {"1100", 8}, {"1011", 16}, {"1010", 32}, {"10011", 12}, {"10010", 48}, {"10001", 20},
            {"10000", 40}, {"01111", 28}, {"01110", 44}, {"01101", 52}, {"01100", 56}, {"01011", 1}, {"01010", 61}, {"01001", 2},
            {"01000", 62}, {"001111", 24}, {"001110", 36}, {"001101", 3}, {"001100", 63}, {"0010111", 5}, {"0010110", 9},
            {"0010101", 17}, {"0010100", 33}, {"0010011", 6}, {"0010010", 10}, {"0010001", 18}, {"0010000", 34},
            {"00011111", 7}, {"00011110", 11}, {"00011101", 19}, {"00011100", 35}, {"00011011", 13}, {"00011010", 49},
            {"00011001", 21}, {"00011000", 41}, {"00010111", 14}, {"00010110", 50}, {"00010101", 22}, {"00010100", 42},
            {"00010011", 15}, {"00010010", 51}, {"00010001", 23}, {"00010000", 43}, {"00001111", 25}, {"00001110", 37},
            {"00001101", 26}, {"00001100", 38}, {"00001011", 29}, {"00001010", 45}, {"00001001", 53}, {"00001000", 57},
            {"00000111", 30}, {"00000110", 46}, {"00000101", 54}, {"00000100", 58}, {"000000111", 31}, {"000000110", 47},
            {"000000101", 55}, {"000000100", 59}, {"000000011", 27}, {"000000010", 39}, {"000000001", 0}};
        // motion_code magnitudes (table B-10); a sign bit follows every non-zero code.
        constexpr Vlc kMotion[] = {
            {"1", 0}, {"01", 1}, {"001", 2}, {"0001", 3}, {"000011", 4}, {"0000101", 5}, {"0000100", 6}, {"0000011", 7},
            {"000001011", 8}, {"000001010", 9}, {"000001001", 10}, {"0000010001", 11}, {"0000010000", 12},
            {"0000001111", 13}, {"0000001110", 14}, {"0000001101", 15}, {"0000001100", 16}};
        // macroblock_type flags: quant 0x10, motion_forward 0x08, motion_backward 0x04, pattern 0x02, intra 0x01.
        constexpr Vlc kMbTypeI[] = {{"1", 0x01}, {"01", 0x11}};
        constexpr Vlc kMbTypeP[] = {{"1", 0x0a}, {"01", 0x02}, {"001", 0x08}, {"00011", 0x01},
                                    {"00010", 0x1a}, {"00001", 0x12}, {"000001", 0x11}};
        constexpr Vlc kMbTypeB[] = {{"10", 0x0c}, {"11", 0x0e}, {"010", 0x04}, {"011", 0x06}, {"0010", 0x08}, {"0011", 0x0a},
                                    {"00011", 0x01}, {"00010", 0x1e}, {"000011", 0x1a}, {"000010", 0x16}, {"000001", 0x11}};
        constexpr Vlc kMbTypeD[] = {{"1", 0x01}};

        template <size_t N>
        bool matchVlc(const Vlc (&table)[N], uint32_t next16, int &value, int &length)
        {
            for (const Vlc &e : table)
            {
                const int len = static_cast<int>(std::strlen(e.code));
                uint32_t code = 0;
                for (int i = 0; i < len; ++i)
                    code = (code << 1) | static_cast<uint32_t>(e.code[i] == '1');
                if ((next16 >> (16 - len)) == code)
                {
                    value = e.value;
                    length = len;
                    return true;
                }
            }
            return false;
        }

        uint8_t clamp8(int v) { return static_cast<uint8_t>(std::clamp(v, 0, 255)); }
    }

    void Ipu::reset()
    {
        input_.clear();
        bitPos_ = cursor_ = 0;
        inActive_ = inChainEnd_ = false;
        inMadr_ = inQwc_ = inTadr_ = 0;
        output_.clear();
        outRead_ = 0;
        busy_ = stalled_ = false;
        stallBit_ = 0;
        command_ = data_ = 0;
        ctrl_ &= CtrlWritable;
        dcPred_[0] = dcPred_[1] = dcPred_[2] = 128;
        quantScale_ = 2;
        idecDone_ = 0;
    }

    // ---- input window -------------------------------------------------------
    bool Ipu::pullDmaQword()
    {
        if (!inActive_)
            return false;
        for (;;)
        {
            if (inQwc_ > 0)
            {
                uint8_t q[16]{};
                bus_.read(inMadr_, q, 16);
                input_.insert(input_.end(), q, q + 16);
                inMadr_ += 16;
                --inQwc_;
                bus_.reg(D4_MADR) = inMadr_;
                bus_.reg(D4_QWC) = inQwc_;
                return true;
            }
            if (inChainEnd_)
            {
                inActive_ = false;
                bus_.finishDma(4);
                return false;
            }
            // source chain: next tag
            uint32_t tag[4]{};
            bus_.read(inTadr_, tag, 16);
            const uint32_t qwc = tag[0] & 0xffffu;
            const uint32_t id = (tag[0] >> 28) & 7u;
            const bool irq = (tag[0] >> 31) & 1u;
            const uint32_t addr = tag[1];
            const bool tie = (bus_.reg(D4_CHCR) >> 7) & 1u;
            bus_.reg(D4_CHCR) = (bus_.reg(D4_CHCR) & 0xffffu) | (tag[0] & 0xffff0000u);
            inQwc_ = qwc;
            switch (id)
            {
            case 0: // refe
                inMadr_ = addr;
                inChainEnd_ = true;
                inTadr_ += 16;
                break;
            case 1: // cnt
                inMadr_ = inTadr_ + 16;
                inTadr_ = inMadr_ + qwc * 16;
                break;
            case 2: // next
                inMadr_ = inTadr_ + 16;
                inTadr_ = addr;
                break;
            case 3: case 4: // ref / refs
                inMadr_ = addr;
                inTadr_ += 16;
                break;
            case 7: // end
                inMadr_ = inTadr_ + 16;
                inChainEnd_ = true;
                break;
            default: // call / ret: not used by the IPU paths seen; end the chain
                inMadr_ = inTadr_ + 16;
                inChainEnd_ = true;
                break;
            }
            if (irq && tie)
                inChainEnd_ = true;
            bus_.reg(D4_TADR) = inTadr_;
        }
    }

    // The toIPU channel only runs ahead of the decoder by the input FIFO (8
    // qwords) plus the bit decoder's 2 qwords, so D4_MADR/QWC and IPU_BP's
    // IFC/FP always describe the real stream position. GT4's movie player
    // saves its place from them when it pauses decoding for CSC.
    void Ipu::pullInput(size_t fromBit, size_t needBits)
    {
        // 10 qwords counted from the qword holding the bit position (IPU_BP:
        // FP 2 + IFC 8); a read that needs more pulls only what it needs.
        constexpr size_t FifoBits = 10u * 128u;
        const size_t want = std::max((fromBit & ~size_t(127)) + FifoBits, fromBit + needBits);
        while (input_.size() * 8u < want && pullDmaQword())
        {
        }
    }

    uint32_t Ipu::peek(int bits)
    {
        if (cursor_ + static_cast<size_t>(bits) > input_.size() * 8u)
        {
            pullInput(cursor_, static_cast<size_t>(bits));
            if (cursor_ + static_cast<size_t>(bits) > input_.size() * 8u)
                throw NeedData{};
        }
        if (bits <= 0)
            return 0u;
        // Five bytes hold any read of up to 32 bits at any bit offset; bytes past the end (never part of the value)
        // read as 0.
        const size_t byte = cursor_ >> 3;
        const int offset = static_cast<int>(cursor_ & 7u);
        const size_t size = input_.size();
        uint64_t window = 0;
        for (size_t k = 0; k < 5u; ++k)
            window = (window << 8) | (byte + k < size ? input_[byte + k] : 0u);
        return static_cast<uint32_t>((window >> (40 - offset - bits)) & ((uint64_t{1} << bits) - 1u));
    }

    uint32_t Ipu::get(int bits)
    {
        const uint32_t value = peek(bits);
        cursor_ += static_cast<size_t>(bits);
        return value;
    }

    // bytes whole bytes from a byte-aligned cursor (CSC / PACK input), as get(8) each would read them: all or
    // NeedData (the cursor is not committed, so a stalled command starts its macroblock again).
    void Ipu::getBytes(uint8_t *out, size_t bytes)
    {
        const size_t need = bytes * 8u;
        if (cursor_ + need > input_.size() * 8u)
        {
            pullInput(cursor_, need);
            if (cursor_ + need > input_.size() * 8u)
                throw NeedData{};
        }
        std::memcpy(out, input_.data() + (cursor_ >> 3), bytes);
        cursor_ += need;
    }

    void Ipu::skip(int bits)
    {
        if (bits <= 0)
            return;
        if (cursor_ + static_cast<size_t>(bits) > input_.size() * 8u)
        {
            pullInput(cursor_, static_cast<size_t>(bits));
            if (cursor_ + static_cast<size_t>(bits) > input_.size() * 8u)
                throw NeedData{};
        }
        cursor_ += static_cast<size_t>(bits);
    }

    void Ipu::commitInput()
    {
        bitPos_ = cursor_;
        // Drop consumed whole qwords now and then (not per command: the window
        // can hold a whole ~1 MB toIPU transfer).
        const size_t whole = (bitPos_ >> 7) << 4;
        if (whole >= 0x10000u && whole * 2u >= input_.size())
        {
            input_.erase(input_.begin(), input_.begin() + static_cast<ptrdiff_t>(whole));
            bitPos_ -= whole * 8u;
            cursor_ = bitPos_;
        }
    }

    // ---- registers ----------------------------------------------------------
    uint64_t Ipu::readCmd()
    {
        step();
        const bool busy = busy_; // the decode is done; output streams from the macroblock buffer
        return data_ | (busy ? (1ull << 63) : 0ull);
    }

    // IPU_BP's view of the input: BP within the current qword, FP qwords in the
    // bit decoder (0-2), IFC qwords in the FIFO (0-8). A command stalled for
    // data has consumed the FIFO up to where it stopped (IFC 0), which is what
    // GT4's IPU sync loop (0x54d5c8) waits for before refilling (0x550188).
    void Ipu::fifoState(uint32_t &bp, uint32_t &ifc, uint32_t &fp) const
    {
        const size_t at = (busy_ && stalled_) ? stallBit_ : bitPos_;
        const size_t qwords = (input_.size() * 8u - std::min(input_.size() * 8u, at & ~size_t(127))) / 128u;
        fp = static_cast<uint32_t>(std::min<size_t>(qwords, 2u));
        ifc = static_cast<uint32_t>(std::min<size_t>(qwords - fp, 8u));
        bp = static_cast<uint32_t>(at & 127u);
    }

    uint32_t Ipu::readCtrl()
    {
        step();
        uint32_t bp = 0, ifc = 0, fp = 0;
        fifoState(bp, ifc, fp);
        // Decoded data waits in the IPU's macroblock buffer and only enters the
        // 8-qword output FIFO while fromIPU (channel 3) runs; GT4's IPU
        // context switch (0x562e90) waits for OFC 0 with the channel stopped.
        const bool outDma = (bus_.reg(D3_CHCR) & 0x100u) != 0u;
        const uint32_t ofc = outDma ? static_cast<uint32_t>(std::min<size_t>(outSize() / 16u, 8u)) : 0u;
        const bool busy = busy_; // the decode is done; output streams from the macroblock buffer
        return ifc | (ofc << 4) | ctrl_ | (busy ? (1u << 31) : 0u);
    }

    uint32_t Ipu::readBp()
    {
        step();
        uint32_t bp = 0, ifc = 0, fp = 0;
        fifoState(bp, ifc, fp);
        return bp | (ifc << 8) | (fp << 16);
    }

    uint64_t Ipu::readTop()
    {
        step();
        cursor_ = bitPos_;
        try
        {
            return peek(32);
        }
        catch (const NeedData &)
        {
            cursor_ = bitPos_;
            return 1ull << 63;
        }
    }

    void Ipu::writeCtrl(uint32_t value)
    {
        if (value & (1u << 30))
        {
            // RST: abort the command, clear both FIFOs
            reset();
        }
        ctrl_ = (ctrl_ & ~CtrlWritable) | (value & CtrlWritable);
    }

    namespace
    {
        // GT4_IPU_LOG=<n>: log the first n commands / DMA events.
        uint64_t statOpNanos[16] = {}; // GT4_IPU_STATS: step() time by the running command's opcode

        bool ipuLog()
        {
            static const long limit = [] {
                const char *v = std::getenv("GT4_IPU_LOG");
                return v && *v ? std::strtol(v, nullptr, 10) : 0L;
            }();
            static long count = 0;
            return count++ < limit;
        }
    }

    void Ipu::writeCmd(uint32_t value)
    {
        // GT4_IPU_STATS=1: command counts by opcode every 5 s (cheap; the
        // per-command GT4_IPU_LOG slows decoding down a lot).
        {
            static const bool stats = std::getenv("GT4_IPU_STATS") != nullptr;
            static uint64_t counts[16] = {};
            static auto last = std::chrono::steady_clock::now();
            if (stats)
            {
                ++counts[value >> 28];
                const auto now = std::chrono::steady_clock::now();
                if (now - last >= std::chrono::seconds(5))
                {
                    last = now;
                    std::fprintf(stderr, "[ipu] 5s: bclr %llu idec %llu bdec %llu vdec %llu fdec %llu csc %llu other %llu; decoding %.0f ms\n",
                                 (unsigned long long)counts[0], (unsigned long long)counts[1], (unsigned long long)counts[2],
                                 (unsigned long long)counts[3], (unsigned long long)counts[4], (unsigned long long)counts[7],
                                 (unsigned long long)(counts[5] + counts[6] + counts[8] + counts[9]), statNanos_ / 1e6);
                    std::fprintf(stderr, "[ipu] 5s ms: bclr %.0f idec %.0f bdec %.0f vdec %.0f fdec %.0f csc %.0f\n", statOpNanos[0] / 1e6,
                                 statOpNanos[1] / 1e6, statOpNanos[2] / 1e6, statOpNanos[3] / 1e6, statOpNanos[4] / 1e6, statOpNanos[7] / 1e6);
                    std::fill(std::begin(counts), std::end(counts), 0ull);
                    std::fill(std::begin(statOpNanos), std::end(statOpNanos), 0ull);
                    statNanos_ = 0;
                }
            }
        }
        if (ipuLog())
        {
            std::fprintf(stderr, "[ipu] cmd %08x ctrl %08x input %zu bytes bit %zu out %zu dma4 %s qwc %u\n", value, ctrl_,
                         input_.size(), bitPos_, outSize(), inActive_ ? "on" : "off", inQwc_);
        }
        command_ = value;
        busy_ = true;
        idecDone_ = 0;
        ++stats_.commands;
        ctrl_ &= ~(CtrlEcd | CtrlScd);
        step();
    }

    void Ipu::writeInputFifo(const uint8_t qword[16])
    {
        input_.insert(input_.end(), qword, qword + 16);
        step();
    }

    bool Ipu::readOutputFifo(uint8_t qword[16])
    {
        step();
        if (outSize() < 16)
            return false;
        std::memcpy(qword, output_.data() + outRead_, 16);
        consumeOutput(16);
        step();
        return true;
    }

    void Ipu::dmaStarted(unsigned channel)
    {
        if (ipuLog())
            std::fprintf(stderr, "[ipu] dma%u start chcr %08x madr %08x qwc %x tadr %08x out %zu busy %d\n", channel,
                         bus_.reg(channel == 3 ? D3_CHCR : D4_CHCR), bus_.reg(channel == 3 ? D3_MADR : D4_MADR),
                         bus_.reg(channel == 3 ? D3_QWC : D4_QWC), bus_.reg(D4_TADR), outSize(), busy_ ? 1 : 0);
        if (channel == 4)
        {
            const uint32_t chcr = bus_.reg(D4_CHCR);
            const uint32_t mode = (chcr >> 2) & 3u;
            inMadr_ = bus_.reg(D4_MADR);
            inQwc_ = bus_.reg(D4_QWC) & 0xffffu;
            inTadr_ = bus_.reg(D4_TADR);
            inChainEnd_ = mode != 1u;
            if (mode == 1u && inQwc_ > 0)
            {
                // Resuming a chain mid-tag: the tag kept in CHCR[31:16] decides
                // whether the chain ends after these qwords (refe / end, or IRQ
                // with TIE). GT4's IPU context switch and its video buffer
                // (0x550188, which patches the CHCR tag to 'ref' before adding
                // chunks) rely on this to stop at the last filled chunk.
                const uint32_t id = (chcr >> 28) & 7u;
                const bool irq = (chcr >> 31) & 1u, tie = (chcr >> 7) & 1u;
                inChainEnd_ = id == 0u || id == 7u || (irq && tie);
            }
            inActive_ = true;
        }
        step();
    }

    void Ipu::dmaStopped(unsigned channel)
    {
        // CHCR written with STR clear: the channel stops where it is; data
        // already in the input FIFO stays there.
        if (channel == 4)
            inActive_ = false;
    }

    // ---- execution ----------------------------------------------------------
    void Ipu::step()
    {
        // GT4_IPU_STATS: the time spent here (decoding, on the game thread) goes into the 5-second line.
        static const bool timed = std::getenv("GT4_IPU_STATS") != nullptr;
        struct Timed
        {
            bool on;
            std::chrono::steady_clock::time_point t0;
            uint64_t &sum;
            uint64_t *perOp;
            ~Timed()
            {
                if (on)
                {
                    const uint64_t ns = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t0).count());
                    sum += ns;
                    if (perOp)
                        *perOp += ns;
                }
            }
        } timing{timed, timed ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{}, statNanos_,
                 timed && busy_ ? &statOpNanos[command_ >> 28] : nullptr};
        if (inActive_)
            pullInput(bitPos_, 0);
        if (busy_)
        {
            cursor_ = bitPos_;
            stalled_ = false;
            if (runCommand())
            {
                busy_ = false;
                if (bus_.raiseInterrupt)
                    bus_.raiseInterrupt();
                if ((command_ >> 28) == 7 || (command_ >> 28) == 1 || (command_ >> 28) == 8)
                    if (ipuLog())
                        std::fprintf(stderr, "[ipu] cmd %08x done: out %zu ch3 chcr %08x qwc %x\n", command_, outSize(),
                                     bus_.reg(D3_CHCR), bus_.reg(D3_QWC));
            }
        }
        drainOutput();
    }

    void Ipu::drainOutput()
    {
        if (!(bus_.reg(D3_CHCR) & 0x100u))
            return;
        uint32_t qwc = bus_.reg(D3_QWC) & 0xffffu;
        uint32_t madr = bus_.reg(D3_MADR);
        // Every whole qword the transfer still takes, in one copy and one write (a write the bus refuses as a whole
        // goes qword by qword, as before): GT4's menu movies send ~100,000 macroblocks a second through here.
        if (const size_t n = std::min<size_t>(qwc, outSize() / 16u); n > 0u)
        {
            const uint8_t *chunk = output_.data() + outRead_;
            if (!bus_.write(madr, chunk, n * 16u))
                for (size_t k = 0; k < n; ++k)
                    bus_.write(madr + static_cast<uint32_t>(k * 16u), chunk + k * 16u, 16);
            consumeOutput(n * 16u);
            madr += static_cast<uint32_t>(n * 16u);
            qwc -= static_cast<uint32_t>(n);
        }
        bus_.reg(D3_MADR) = madr;
        bus_.reg(D3_QWC) = qwc;
        if (qwc == 0)
        {
            if (ipuLog())
                std::fprintf(stderr, "[ipu] dma3 done at %08x, %zu bytes left\n", madr, outSize());
            bus_.finishDma(3);
        }
    }

    void Ipu::pushOutput(const void *data, size_t bytes)
    {
        const auto *p = static_cast<const uint8_t *>(data);
        output_.insert(output_.end(), p, p + bytes);
    }

    void Ipu::consumeOutput(size_t bytes)
    {
        outRead_ += bytes;
        if (outRead_ >= output_.size())
        {
            output_.clear();
            outRead_ = 0;
        }
        else if (outRead_ >= 0x10000u && outRead_ * 2u >= output_.size())
        {
            output_.erase(output_.begin(), output_.begin() + static_cast<std::ptrdiff_t>(outRead_));
            outRead_ = 0;
        }
    }

    void Ipu::fail(const char *what)
    {
        ++stats_.errors;
        if (errorsLogged_++ < 32)
            std::fprintf(stderr, "[ipu] command %08x: %s (bit %zu of %zu)\n", command_, what, cursor_, input_.size() * 8u);
    }

    bool Ipu::runCommand()
    {
        const uint32_t opcode = command_ >> 28;
        const int fb = static_cast<int>(command_ & 0x3fu);
        try
        {
            switch (opcode)
            {
            case 0: // BCLR: drop the input FIFO; BP applies to the data that follows
                input_.clear();
                bitPos_ = cursor_ = command_ & 0x7fu;
                        return true;
            case 1:
                cmdIdec();
                return true;
            case 2:
                skip(fb);
                cmdBdec();
                commitInput();
                return true;
            case 3:
                skip(fb);
                cmdVdec();
                commitInput();
                return true;
            case 4:
                skip(fb);
                cmdFdec();
                commitInput();
                return true;
            case 5:
                skip(fb);
                cmdSetIq();
                commitInput();
                return true;
            case 6:
                cmdSetVq();
                commitInput();
                return true;
            case 7:
                cmdCsc();
                {
                    // GT4_MOVIE_TRACE=1 (diagnostics, analysis/design/movie-stutter): each finished CSC (a picture's colour conversion).
                    static const bool movieTrace = std::getenv("GT4_MOVIE_TRACE") != nullptr;
                    if (movieTrace)
                        std::fprintf(stderr, "[mvt] csc t=%lld mbc=%u\n",
                                     static_cast<long long>(std::chrono::duration_cast<std::chrono::microseconds>(
                                         std::chrono::steady_clock::now().time_since_epoch()).count()),
                                     command_ & 0x7ffu);
                }
                return true;
            case 8:
                cmdPack();
                return true;
            case 9: // SETTH
                th0_ = command_ & 0x1ffu;
                th1_ = (command_ >> 16) & 0x1ffu;
                return true;
            default:
                fail("unknown command");
                return true;
            }
        }
        catch (const NeedData &)
        {
            // A stall with plenty of input left means the bitstream was misparsed.
            if (input_.size() * 8u - bitPos_ > 0x10000u && errorsLogged_ < 32)
            {
                ++errorsLogged_;
                std::fprintf(stderr, "[ipu] command %08x ran past %zu bits of input from bit %zu (misparse)\n", command_,
                             input_.size() * 8u - bitPos_, bitPos_);
            }
            stalled_ = true;
            stallBit_ = cursor_;
            cursor_ = bitPos_;
            ++stats_.stalls;
            return false;
        }
    }

    bool Ipu::decodeBlock(bool intra, int component, int16_t block[64])
    {
        std::memset(block, 0, 64 * sizeof(int16_t));
        const bool mpeg1 = (ctrl_ >> 23) & 1u;
        const bool alternate = (ctrl_ >> 20) & 1u;
        const bool ivf = (ctrl_ >> 21) & 1u;
        const int idp = static_cast<int>((ctrl_ >> 16) & 3u);
        const uint8_t *scan = ipuScanOrder(alternate);
        const uint8_t *matrix = intra ? intraMatrix_.data() : nonIntraMatrix_.data();
        int sum = 0;
        int i = 0;
        if (intra)
        {
            int length = 0;
            const int size = ipuDcSizeLookup(peek(10), component != 0, length);
            if (size < 0)
                return fail("invalid dct_dc_size"), false;
            skip(length);
            int diff = 0;
            if (size)
            {
                diff = static_cast<int>(get(size));
                if (!(diff >> (size - 1)))
                    diff -= (1 << size) - 1;
            }
            dcPred_[component] += diff;
            block[0] = static_cast<int16_t>(dcPred_[component] << (3 - idp));
            sum = block[0];
        }
        else
        {
            i = -1;
        }
        const bool b15 = intra && ivf && !mpeg1;
        bool first = !intra;
        for (;;)
        {
            int run = 0, level = 0;
            if (first && peek(1))
            {
                // non-intra first coefficient: '1s' = run 0, level 1
                skip(1);
                level = get(1) ? -1 : 1;
            }
            else
            {
                const IpuDctCode e = ipuDctLookup(peek(16), b15);
                if (e.kind == 2)
                {
                    skip(e.length);
                    break;
                }
                if (e.kind == 1)
                {
                    skip(e.length);
                    run = e.run;
                    level = get(1) ? -e.level : e.level;
                }
                else if (e.kind == 3)
                {
                    skip(e.length);
                    run = static_cast<int>(get(6));
                    if (mpeg1)
                    {
                        level = static_cast<int8_t>(get(8));
                        if (level == -128)
                            level = static_cast<int>(get(8)) - 256;
                        else if (level == 0)
                            level = static_cast<int>(get(8));
                    }
                    else
                    {
                        const uint32_t v = get(12);
                        level = static_cast<int>((v ^ 0x800u) - 0x800u);
                    }
                    if (!level)
                        return fail("escaped DCT level of zero"), false;
                }
                else
                {
                    return fail("invalid DCT coefficient code"), false;
                }
            }
            first = false;
            i += run + 1;
            if (i > 63)
                return fail("DCT coefficients past the end of the block"), false;
            const int j = scan[i];
            int value;
            if (intra)
                value = level * quantScale_ * matrix[j] / 16;
            else
                value = ((2 * level + (level > 0 ? 1 : -1)) * quantScale_ * matrix[j]) / 32;
            if (mpeg1 && value && !(value & 1))
                value -= value > 0 ? 1 : -1;
            value = std::clamp(value, -2048, 2047);
            block[j] = static_cast<int16_t>(value);
            sum += value;
        }
        if (!mpeg1 && !(sum & 1))
            block[63] ^= 1;
        return true;
    }

    void Ipu::cmdBdec()
    {
        // BDEC: [5:0] FB, [20:16] QSC, [25] DT, [26] DCR, [27] MBI. One macroblock as
        // RAW16: Y 16x16, Cb 8x8, Cr 8x8 (384 signed halfwords). Committed atomically.
        const int idp = static_cast<int>((ctrl_ >> 16) & 3u);
        const bool qst = (ctrl_ >> 22) & 1u;
        const bool intra = (command_ >> 27) & 1u;
        const bool fieldDct = (command_ >> 25) & 1u;
        int savedDc[3];
        std::copy(std::begin(dcPred_), std::end(dcPred_), savedDc);
        if ((command_ >> 26) & 1u)
            dcPred_[0] = dcPred_[1] = dcPred_[2] = 128 << idp;
        quantScale_ = ipuQuantizerScale(static_cast<int>((command_ >> 16) & 31u), qst);
        try
        {
            int cbp = 0x3f;
            if (!intra)
            {
                int value = 0, length = 0;
                if (!matchVlc(kCbp, peek(16), value, length))
                {
                    fail("invalid coded_block_pattern");
                    value = 0, length = 0;
                }
                skip(length);
                cbp = value;
            }
            std::array<int16_t, 384> out{};
            int16_t block[64];
            int32_t samples[64];
            for (int b = 0; b < 6; ++b)
            {
                if (!((cbp >> (5 - b)) & 1))
                    continue;
                const int component = b < 4 ? 0 : b - 3;
                if (!decodeBlock(intra, component, block))
                    break;
                ipuIdct(block, samples);
                const int lo = intra ? 0 : -256, hi = 255;
                if (b < 4)
                {
                    const int x = (b & 1) * 8;
                    const int y = fieldDct ? (b >> 1) : (b >> 1) * 8;
                    const int stride = fieldDct ? 32 : 16;
                    for (int r = 0; r < 8; ++r)
                        for (int c = 0; c < 8; ++c)
                            out[(y * 16 + x) + r * stride + c] = static_cast<int16_t>(std::clamp(samples[r * 8 + c], lo, hi));
                }
                else
                {
                    const int base = b == 4 ? 256 : 320;
                    for (int k = 0; k < 64; ++k)
                        out[base + k] = static_cast<int16_t>(std::clamp(samples[k], lo, hi));
                }
            }
            ctrl_ = (ctrl_ & ~(0x3fu << CtrlCbpShift)) | (static_cast<uint32_t>(cbp) << CtrlCbpShift);
            pushOutput(out.data(), sizeof(out));
            ++stats_.macroblocks;
        }
        catch (const NeedData &)
        {
            std::copy(std::begin(savedDc), std::end(savedDc), dcPred_);
            throw;
        }
    }

    int Ipu::macroblockAddressIncrement(int &length)
    {
        int increment = 0;
        length = 0;
        for (;;)
        {
            int len = 0;
            const int v = ipuMbaLookup(peek(11), len);
            if (v < 0)
                return increment ? increment : 0;
            skip(len);
            length += len;
            if (v == 34)
                increment += 33;
            else if (v != 35)
                return increment + v;
        }
    }

    int Ipu::macroblockModes(int &length)
    {
        int value = 0;
        length = 0;
        const uint32_t next = peek(16);
        const uint32_t pct = (ctrl_ >> 24) & 7u;
        bool ok = false;
        switch (pct)
        {
        case 2: ok = matchVlc(kMbTypeP, next, value, length); break;
        case 3: ok = matchVlc(kMbTypeB, next, value, length); break;
        case 4: ok = matchVlc(kMbTypeD, next, value, length); break;
        default: ok = matchVlc(kMbTypeI, next, value, length); break;
        }
        if (!ok)
        {
            fail("invalid macroblock_type");
            return 0;
        }
        skip(length);
        return value;
    }

    int Ipu::motionCode(int &length)
    {
        int value = 0;
        if (!matchVlc(kMotion, peek(16), value, length))
        {
            fail("invalid motion_code");
            return 0;
        }
        skip(length);
        if (value)
        {
            ++length;
            if (get(1))
                value = -value;
        }
        return value;
    }

    int Ipu::dmVector(int &length)
    {
        if (!get(1))
        {
            length = 1;
            return 0;
        }
        length = 2;
        return get(1) ? -1 : 1;
    }

    void Ipu::cmdVdec()
    {
        // VDEC: [27:26] TBL. DATA = decoded value (16 bits) | code length << 16.
        int length = 0, value = 0;
        switch ((command_ >> 26) & 3u)
        {
        case 0: value = macroblockAddressIncrement(length); break;
        case 1: value = macroblockModes(length); break;
        case 2: value = motionCode(length); break;
        case 3: value = dmVector(length); break;
        }
        data_ = (static_cast<uint32_t>(value) & 0xffffu) | (static_cast<uint32_t>(length) << 16);
    }

    void Ipu::cmdFdec()
    {
        // FDEC: DATA = the next 32 bits (not consumed).
        data_ = peek(32);
    }

    void Ipu::cmdSetIq()
    {
        // SETIQ: 64 bytes in zigzag order; [27] = non-intra matrix.
        const uint8_t *scan = ipuScanOrder(false);
        std::array<uint8_t, 64> m{};
        for (int i = 0; i < 64; ++i)
            m[scan[i]] = static_cast<uint8_t>(get(8));
        if ((command_ >> 27) & 1u)
            nonIntraMatrix_ = m;
        else
            intraMatrix_ = m;
    }

    void Ipu::cmdSetVq()
    {
        for (auto &entry : vqClut_)
        {
            const uint32_t hi = get(8), lo = get(8);
            entry = static_cast<uint16_t>(lo | (hi << 8));
        }
    }

    namespace
    {
        // GT4_IPU_DUMP=<path prefix>: writes the first 40 colour-converted frames
        // as <prefix>NNN.ppm, assuming 640-wide frames of 16x16 macroblocks in
        // raster order (a frame ends every 1120 macroblocks).
        void dumpCsc(const uint8_t *rgba)
        {
            static const char *prefix = std::getenv("GT4_IPU_DUMP");
            static std::vector<uint8_t> frame;
            static uint32_t mb = 0, frames = 0;
            if (!prefix || !*prefix || frames >= 40)
                return;
            constexpr int Width = 640, Height = 448, PerRow = Width / 16, PerFrame = PerRow * (Height / 16);
            if (frame.empty())
                frame.assign(size_t(Width) * Height * 3u, 0);
            const int mbx = int(mb % PerRow) * 16, mby = int(mb / PerRow) * 16;
            for (int row = 0; row < 16; ++row)
                for (int col = 0; col < 16; ++col)
                {
                    const uint8_t *src = rgba + (row * 16 + col) * 4;
                    uint8_t *dst = &frame[(size_t(mby + row) * Width + mbx + col) * 3u];
                    dst[0] = src[0];
                    dst[1] = src[1];
                    dst[2] = src[2];
                }
            if (++mb < uint32_t(PerFrame))
                return;
            mb = 0;
            char path[512];
            std::snprintf(path, sizeof(path), "%s%03u.ppm", prefix, frames++);
            if (std::FILE *f = std::fopen(path, "wb"))
            {
                std::fprintf(f, "P6 %d %d 255 ", Width, Height);
                std::fwrite(frame.data(), 1, frame.size(), f);
                std::fclose(f);
            }
        }
    }

    void Ipu::cscMacroblock(const uint8_t *y, const uint8_t *cb, const uint8_t *cr, bool rgb16, bool dither)
    {
        (void)dither;
        IpuMacroblock mb;
        std::memcpy(mb.y, y, 256);
        std::memcpy(mb.cb, cb, 64);
        std::memcpy(mb.cr, cr, 64);
        IpuIdecParams params;
        params.th0 = th0_;
        params.th1 = th1_;
        uint8_t rgba[1024];
        ipuCscRgb32(mb, params, rgba);
        dumpCsc(rgba);
        if (!rgb16)
        {
            pushOutput(rgba, sizeof(rgba));
            return;
        }
        uint16_t out[256];
        for (int i = 0; i < 256; ++i)
        {
            const uint8_t *p = rgba + i * 4;
            out[i] = static_cast<uint16_t>((p[0] >> 3) | ((p[1] >> 3) << 5) | ((p[2] >> 3) << 10) | (p[3] ? 0x8000u : 0u));
        }
        pushOutput(out, sizeof(out));
    }

    void Ipu::cmdIdec()
    {
        // IDEC: [5:0] FB, [20:16] QSC, [24] DTD, [25] SGN, [26] DTE, [27] OFM.
        // Macroblocks to RGB32/RGB16 until the next start code; each one is
        // committed as soon as it is decoded, so a stall resumes at a macroblock.
        const int idp = static_cast<int>((ctrl_ >> 16) & 3u);
        const bool qst = (ctrl_ >> 22) & 1u;
        const bool dtd = (command_ >> 24) & 1u;
        const bool rgb16 = (command_ >> 27) & 1u;
        if (idecDone_ == 0)
        {
            skip(static_cast<int>(command_ & 0x3fu));
            dcPred_[0] = dcPred_[1] = dcPred_[2] = 128 << idp;
            quantScale_ = ipuQuantizerScale(static_cast<int>((command_ >> 16) & 31u), qst);
            commitInput();
            idecDone_ = 1;
        }
        for (;;)
        {
            int savedDc[3];
            std::copy(std::begin(dcPred_), std::end(dcPred_), savedDc);
            const int savedQuant = quantScale_;
            try
            {
                int length = 0;
                if (idecDone_ > 1)
                {
                    const int increment = macroblockAddressIncrement(length);
                    if (!increment)
                    {
                        ctrl_ |= CtrlScd;
                        commitInput();
                        return; // start code: IDEC ends
                    }
                }
                bool quant = false;
                if (peek(1))
                    skip(1);
                else if (peek(2) == 1)
                    skip(2), quant = true;
                else
                {
                    fail("invalid IDEC macroblock_type");
                    commitInput();
                    return;
                }
                const bool fieldDct = dtd && get(1);
                if (quant)
                    quantScale_ = ipuQuantizerScale(static_cast<int>(get(5)), qst);
                uint8_t y[256], cb[64], cr[64];
                int16_t block[64];
                int32_t samples[64];
                for (int b = 0; b < 6; ++b)
                {
                    if (!decodeBlock(true, b < 4 ? 0 : b - 3, block))
                    {
                        commitInput();
                        return;
                    }
                    ipuIdct(block, samples);
                    if (b < 4)
                    {
                        const int x = (b & 1) * 8;
                        const int yy = fieldDct ? (b >> 1) : (b >> 1) * 8;
                        const int stride = fieldDct ? 32 : 16;
                        for (int r = 0; r < 8; ++r)
                            for (int c = 0; c < 8; ++c)
                                y[(yy * 16 + x) + r * stride + c] = clamp8(samples[r * 8 + c]);
                    }
                    else
                    {
                        uint8_t *dst = b == 4 ? cb : cr;
                        for (int k = 0; k < 64; ++k)
                            dst[k] = clamp8(samples[k]);
                    }
                }
                cscMacroblock(y, cb, cr, rgb16, (command_ >> 26) & 1u);
                ++stats_.macroblocks;
                ++idecDone_;
                commitInput();
                drainOutput();
            }
            catch (const NeedData &)
            {
                std::copy(std::begin(savedDc), std::end(savedDc), dcPred_);
                quantScale_ = savedQuant;
                throw;
            }
        }
    }

    void Ipu::cmdCsc()
    {
        // CSC: [10:0] MBC macroblocks of RAW8 (Y 256, Cb 64, Cr 64 bytes) -> RGB32/16.
        const uint32_t count = command_ & 0x7ffu;
        const bool rgb16 = (command_ >> 27) & 1u;
        for (; idecDone_ < count; ++idecDone_)
        {
            uint8_t raw[384];
            cursor_ = (bitPos_ + 7u) & ~size_t(7);
            getBytes(raw, sizeof(raw));
            commitInput();
            cscMacroblock(raw, raw + 256, raw + 320, rgb16, (command_ >> 26) & 1u);
            drainOutput();
        }
    }

    void Ipu::cmdPack()
    {
        // PACK: [10:0] macroblocks of RGB32 -> RGB16 ([27] OFM 1) or 4-bit
        // indices through the VQ CLUT (OFM 0).
        const uint32_t count = command_ & 0x7ffu;
        const bool rgb16 = (command_ >> 27) & 1u;
        for (; idecDone_ < count; ++idecDone_)
        {
            uint8_t rgba[1024];
            cursor_ = (bitPos_ + 7u) & ~size_t(7);
            getBytes(rgba, sizeof(rgba));
            commitInput();
            if (rgb16)
            {
                uint16_t out[256];
                for (int i = 0; i < 256; ++i)
                {
                    const uint8_t *p = rgba + i * 4;
                    out[i] = static_cast<uint16_t>((p[0] >> 3) | ((p[1] >> 3) << 5) | ((p[2] >> 3) << 10) | (p[3] ? 0x8000u : 0u));
                }
                pushOutput(out, sizeof(out));
            }
            else
            {
                uint8_t out[128]{};
                for (int i = 0; i < 256; ++i)
                {
                    const uint8_t *p = rgba + i * 4;
                    int best = 0, bestDist = 1 << 30;
                    for (int k = 0; k < 16; ++k)
                    {
                        const int r = (vqClut_[k] & 31) << 3, g = ((vqClut_[k] >> 5) & 31) << 3, b = ((vqClut_[k] >> 10) & 31) << 3;
                        const int d = (r - p[0]) * (r - p[0]) + (g - p[1]) * (g - p[1]) + (b - p[2]) * (b - p[2]);
                        if (d < bestDist)
                            bestDist = d, best = k;
                    }
                    out[i / 2] |= static_cast<uint8_t>(best << ((i & 1) * 4));
                }
                pushOutput(out, sizeof(out));
            }
            drainOutput();
        }
    }
}
