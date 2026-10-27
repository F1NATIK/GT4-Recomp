#pragma once

// The PS2 IPU (image processing unit, EE 0x10002000 / FIFOs 0x10007000) with its
// DMA channels 3 (fromIPU) and 4 (toIPU). Game-independent hardware model,
// written for GT4, whose own MPEG player drives the IPU at register level
// (BCLR/IDEC/BDEC/VDEC/FDEC/SETIQ/SETVQ/CSC/PACK/SETTH). Commands run
// synchronously when the EE touches the IPU or its DMA channels: all data the
// toIPU channel runs at most 10 qwords (input FIFO + bit decoder) ahead of the
// decoder, a command that runs out of input is rolled back and stays busy
// until more data arrives,
// and output drains to the fromIPU channel (or the output FIFO register).

#include <array>
#include <cstdint>
#include <deque>
#include <functional>
#include <string>
#include <vector>

namespace ps2x::video
{
    struct IpuBus
    {
        // Guest memory as the DMAC sees it (bit 31 of an address = scratchpad).
        std::function<bool(uint32_t, void *, size_t)> read;
        std::function<bool(uint32_t, const void *, size_t)> write;
        // DMAC channel registers (CHCR, MADR, QWC, TADR) by absolute address.
        std::function<uint32_t &(uint32_t)> reg;
        // Channel finished: clear STR, set D_STAT, raise the DMAC interrupt.
        std::function<void(unsigned)> finishDma;
        // INTC cause 8: raised when a command completes.
        std::function<void()> raiseInterrupt;
    };

    class Ipu
    {
    public:
        explicit Ipu(IpuBus bus) : bus_(std::move(bus)) { reset(); }
        void reset();

        uint64_t readCmd();
        uint32_t readCtrl();
        uint32_t readBp();
        uint64_t readTop();
        void writeCmd(uint32_t value);
        void writeCtrl(uint32_t value);
        void writeInputFifo(const uint8_t qword[16]);
        bool readOutputFifo(uint8_t qword[16]);

        // CHCR written with STR set / clear on channel 3 (fromIPU) or 4 (toIPU).
        void dmaStarted(unsigned channel);
        void dmaStopped(unsigned channel);
        // Advance: pull input, run the pending command, drain output.
        void step();

        struct Stats
        {
            uint64_t commands = 0, macroblocks = 0, stalls = 0, errors = 0;
        };
        const Stats &stats() const { return stats_; }

    private:
        struct NeedData
        {
        };

        // bit window over input bytes
        uint32_t peek(int bits);
        uint32_t get(int bits);
        void skip(int bits);
        void getBytes(uint8_t *out, size_t bytes);
        void pullInput(size_t fromBit, size_t needBits);
        void fifoState(uint32_t &bp, uint32_t &ifc, uint32_t &fp) const;
        bool pullDmaQword();
        void commitInput();

        bool runCommand();
        void cmdIdec();
        void cmdBdec();
        void cmdVdec();
        void cmdFdec();
        void cmdSetIq();
        void cmdSetVq();
        void cmdCsc();
        void cmdPack();
        bool decodeBlock(bool intra, int component, int16_t coefficients[64]);
        int macroblockAddressIncrement(int &length);
        int macroblockModes(int &length);
        int motionCode(int &length);
        int dmVector(int &length);
        void cscMacroblock(const uint8_t *y, const uint8_t *cb, const uint8_t *cr, bool rgb16, bool dither);
        void pushOutput(const void *data, size_t bytes);
        void drainOutput();
        void fail(const char *what);

        IpuBus bus_;
        // input
        std::vector<uint8_t> input_;
        size_t bitPos_ = 0;     // committed bit position in input_
        size_t cursor_ = 0;     // working bit position during a command
        // toIPU source chain state
        bool inActive_ = false, inChainEnd_ = false;
        uint32_t inMadr_ = 0, inQwc_ = 0, inTadr_ = 0;
        // output: bytes [outRead_, size) not yet taken (a vector, not a byte deque: GT4's movies push ~100,000
        // macroblocks a second through it)
        std::vector<uint8_t> output_;
        size_t outRead_ = 0;
        size_t outSize() const { return output_.size() - outRead_; }
        void consumeOutput(size_t bytes);
        // command
        bool busy_ = false;
        bool stalled_ = false; // the running command waits for input
        size_t stallBit_ = 0;  // where it stopped
        uint32_t command_ = 0;
        uint32_t data_ = 0;
        uint32_t ctrl_ = 0; // IDP/AS/IVF/QST/MP1/PCT, CBP, ECD, SCD
        int dcPred_[3] = {128, 128, 128};
        int quantScale_ = 2;
        std::array<uint8_t, 64> intraMatrix_{}, nonIntraMatrix_{}; // natural order
        std::array<uint16_t, 16> vqClut_{};
        uint32_t th0_ = 0, th1_ = 0;
        uint32_t idecDone_ = 0; // macroblocks already produced by the running IDEC
        Stats stats_;
        uint64_t statNanos_ = 0; // GT4_IPU_STATS: time in step() since the last 5-second line
        uint32_t errorsLogged_ = 0;
    };
}
