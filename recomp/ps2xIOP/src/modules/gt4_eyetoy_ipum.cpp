#include "gt4_eyetoy_ipum.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>

// Bitstream GT4 decodes (CORE 0x54c8f0 mode 1, all [C] from that function):
//   toIPU DMA from frame+0x10, FDEC 8 (0x563298) drops the byte at 0x10,
//   IPU_CTRL = 0: IDP 0 (8-bit intra DC), QST 0 (linear quantiser), AS 0 (zigzag),
//   IVF 0 (table B-14), MP1 0 (MPEG-2 escapes); SETIQ intra = CORE 0x64c410;
//   IDEC 0x10010000: QSC 1, DTD 0 (no dct_type bit), RGB32 output.
// Macroblocks: macroblock_type '01' + quantiser_scale_code for the first one,
// '1' for the others; four Y blocks then Cb and Cr; DC predictors reset to 128
// at IDEC start; macroblock_address_increment '1' between macroblocks; a start
// code ends IDEC. GT4 treats IPU_CTRL.ECD as a failed frame (0x54ccfc).

namespace ps2x::iop::gt4::eyetoy
{
    namespace
    {
        constexpr uint8_t kZigzag[64] = {0,  1,  8,  16, 9,  2,  3,  10, 17, 24, 32, 25, 18, 11, 4,  5,
                                         12, 19, 26, 33, 40, 48, 41, 34, 27, 20, 13, 6,  7,  14, 21, 28,
                                         35, 42, 49, 56, 57, 50, 43, 36, 29, 22, 15, 23, 30, 37, 44, 51,
                                         58, 59, 52, 45, 38, 31, 39, 46, 53, 60, 61, 54, 47, 55, 62, 63};

        // CORE 0x64c410 (64 bytes, read from game/CORE.elf): MPEG-2 default intra matrix in zigzag order.
        constexpr uint8_t kIntraZigzag[64] = {8,  16, 16, 19, 16, 19, 22, 22, 22, 22, 22, 22, 26, 24, 26, 27,
                                              27, 27, 26, 26, 26, 26, 27, 27, 27, 29, 29, 29, 34, 34, 34, 29,
                                              29, 29, 27, 27, 29, 29, 32, 32, 34, 34, 37, 38, 37, 35, 35, 34,
                                              35, 38, 38, 40, 40, 40, 48, 48, 46, 46, 56, 56, 58, 69, 69, 83};

        // ISO/IEC 13818-2 table B-14 without the sign bit (the "next coefficient"
        // form: run 0 level 1 is '11'), in the order of runOf/levelOf below. The
        // unit test checks every code against the runtime IPU decoder's table
        // (ps2xRuntime ipu_idec.cpp), so encoder and decoder cannot drift.
        constexpr uint16_t kB14[111][2] = {
            {0x3, 2},   {0x4, 4},   {0x5, 5},   {0x6, 7},   {0x26, 8},  {0x21, 8},  {0xa, 10},  {0x1d, 12},
            {0x18, 12}, {0x13, 12}, {0x10, 12}, {0x1a, 13}, {0x19, 13}, {0x18, 13}, {0x17, 13}, {0x1f, 14},
            {0x1e, 14}, {0x1d, 14}, {0x1c, 14}, {0x1b, 14}, {0x1a, 14}, {0x19, 14}, {0x18, 14}, {0x17, 14},
            {0x16, 14}, {0x15, 14}, {0x14, 14}, {0x13, 14}, {0x12, 14}, {0x11, 14}, {0x10, 14}, {0x18, 15},
            {0x17, 15}, {0x16, 15}, {0x15, 15}, {0x14, 15}, {0x13, 15}, {0x12, 15}, {0x11, 15}, {0x10, 15},
            {0x3, 3},   {0x6, 6},   {0x25, 8},  {0xc, 10},  {0x1b, 12}, {0x16, 13}, {0x15, 13}, {0x1f, 15},
            {0x1e, 15}, {0x1d, 15}, {0x1c, 15}, {0x1b, 15}, {0x1a, 15}, {0x19, 15}, {0x13, 16}, {0x12, 16},
            {0x11, 16}, {0x10, 16}, {0x5, 4},   {0x4, 7},   {0xb, 10},  {0x14, 12}, {0x14, 13}, {0x7, 5},
            {0x24, 8},  {0x1c, 12}, {0x13, 13}, {0x6, 5},   {0xf, 10},  {0x12, 12}, {0x7, 6},   {0x9, 10},
            {0x12, 13}, {0x5, 6},   {0x1e, 12}, {0x14, 16}, {0x4, 6},   {0x15, 12}, {0x7, 7},   {0x11, 12},
            {0x5, 7},   {0x11, 13}, {0x27, 8},  {0x10, 13}, {0x23, 8},  {0x1a, 16}, {0x22, 8},  {0x19, 16},
            {0x20, 8},  {0x18, 16}, {0xe, 10},  {0x17, 16}, {0xd, 10},  {0x16, 16}, {0x8, 10},  {0x15, 16},
            {0x1f, 12}, {0x1a, 12}, {0x19, 12}, {0x17, 12}, {0x16, 12}, {0x1f, 13}, {0x1e, 13}, {0x1d, 13},
            {0x1c, 13}, {0x1b, 13}, {0x1f, 16}, {0x1e, 16}, {0x1d, 16}, {0x1c, 16}, {0x1b, 16}};

        int runOf(int index)
        {
            if (index < 40) return 0;
            if (index < 58) return 1;
            if (index < 63) return 2;
            if (index < 67) return 3;
            if (index < 70) return 4;
            if (index < 73) return 5;
            if (index < 76) return 6;
            if (index < 96) return 7 + (index - 76) / 2;
            return 17 + (index - 96);
        }

        int levelOf(int index)
        {
            if (index < 40) return index + 1;
            if (index < 58) return index - 39;
            if (index < 63) return index - 57;
            if (index < 67) return index - 62;
            if (index < 70) return index - 66;
            if (index < 73) return index - 69;
            if (index < 76) return index - 72;
            if (index < 96) return 1 + (index - 76) % 2;
            return 1;
        }

        // dct_dc_size_luminance / chrominance (tables B-12, B-13): {code, length} by size.
        constexpr uint16_t kDcLuma[12][2] = {{0x4, 3},  {0x0, 2},  {0x1, 2},  {0x5, 3},  {0x6, 3},   {0xe, 4},
                                             {0x1e, 5}, {0x3e, 6}, {0x7e, 7}, {0xfe, 8}, {0x1fe, 9}, {0x1ff, 9}};
        constexpr uint16_t kDcChroma[12][2] = {{0x0, 2},  {0x1, 2},  {0x2, 2},  {0x6, 3},   {0xe, 4},    {0x1e, 5},
                                               {0x3e, 6}, {0x7e, 7}, {0xfe, 8}, {0x1fe, 9}, {0x3fe, 10}, {0x3ff, 10}};

        struct Vlc
        {
            uint16_t code = 0;
            uint8_t length = 0; // 0: not in the table (escape)
        };

        struct Tables
        {
            Vlc runLevel[64][41]{}; // [run][level 1..40]
            double dct[8][8]{};     // [frequency][sample], the decoder's IDCT basis
            Tables()
            {
                for (int i = 0; i < 111; ++i)
                    runLevel[runOf(i)][levelOf(i)] = Vlc{kB14[i][0], static_cast<uint8_t>(kB14[i][1])};
                const double pi = std::acos(-1.0);
                for (int u = 0; u < 8; ++u)
                    for (int x = 0; x < 8; ++x)
                        dct[u][x] = (u ? 0.5 : 0.5 / std::sqrt(2.0)) * std::cos((2 * x + 1) * u * pi / 16.0);
            }
        };

        const Tables &tables()
        {
            static const Tables value;
            return value;
        }

        class BitWriter
        {
        public:
            explicit BitWriter(std::vector<uint8_t> &out) : out_(out) {}
            void put(uint32_t value, int length)
            {
                for (int i = length - 1; i >= 0; --i)
                {
                    current_ = static_cast<uint8_t>((current_ << 1) | ((value >> i) & 1u));
                    if (++used_ == 8)
                    {
                        out_.push_back(current_);
                        current_ = 0;
                        used_ = 0;
                    }
                }
            }
            void alignWithZeros()
            {
                if (used_)
                    put(0, 8 - used_);
            }

        private:
            std::vector<uint8_t> &out_;
            uint8_t current_ = 0;
            int used_ = 0;
        };

        // 8x8 forward DCT (orthonormal, the transpose of the decoder's IDCT).
        void forwardDct(const int samples[64], double coefficients[64])
        {
            const auto &t = tables();
            double rows[64];
            for (int y = 0; y < 8; ++y)
                for (int u = 0; u < 8; ++u)
                {
                    double sum = 0.0;
                    for (int x = 0; x < 8; ++x)
                        sum += t.dct[u][x] * samples[y * 8 + x];
                    rows[y * 8 + u] = sum;
                }
            for (int u = 0; u < 8; ++u)
                for (int v = 0; v < 8; ++v)
                {
                    double sum = 0.0;
                    for (int y = 0; y < 8; ++y)
                        sum += t.dct[v][y] * rows[y * 8 + u];
                    coefficients[v * 8 + u] = sum;
                }
        }

        int bitsFor(int magnitude)
        {
            int size = 0;
            while (magnitude)
            {
                ++size;
                magnitude >>= 1;
            }
            return size;
        }

        class Encoder
        {
        public:
            Encoder(const YuvPicture &picture, int code, bool dcOnly) : picture_(picture), code_(code), dcOnly_(dcOnly)
            {
                for (int i = 0; i < 64; ++i)
                    matrix_[kZigzag[i]] = kIntraZigzag[i];
            }

            void run(std::vector<uint8_t> &bits)
            {
                BitWriter writer(bits);
                dcPred_[0] = dcPred_[1] = dcPred_[2] = 128; // IDEC start (IDP 0)
                const uint32_t mbw = picture_.width / 16, mbh = picture_.height / 16;
                for (uint32_t my = 0; my < mbh; ++my)
                    for (uint32_t mx = 0; mx < mbw; ++mx)
                    {
                        const bool first = mx == 0 && my == 0;
                        if (!first)
                            writer.put(1, 1); // macroblock_address_increment 1
                        if (first)
                        {
                            writer.put(1, 2); // macroblock_type: intra + quant
                            writer.put(static_cast<uint32_t>(code_), 5);
                        }
                        else
                        {
                            writer.put(1, 1); // macroblock_type: intra
                        }
                        int block[64];
                        for (int b = 0; b < 4; ++b)
                        {
                            loadBlock(picture_.y.data(), picture_.width, mx * 16 + (b & 1) * 8, my * 16 + (b >> 1) * 8, block);
                            encodeBlock(writer, 0, block);
                        }
                        loadBlock(picture_.cb.data(), picture_.width / 2, mx * 8, my * 8, block);
                        encodeBlock(writer, 1, block);
                        loadBlock(picture_.cr.data(), picture_.width / 2, mx * 8, my * 8, block);
                        encodeBlock(writer, 2, block);
                    }
                // Zero stuffing to a byte boundary, then sequence_end_code: the
                // increment decode sees the start code prefix and IDEC ends.
                writer.alignWithZeros();
                writer.put(0x000001b7u, 32);
            }

        private:
            static void loadBlock(const uint8_t *plane, uint32_t stride, uint32_t x0, uint32_t y0, int block[64])
            {
                for (int y = 0; y < 8; ++y)
                    for (int x = 0; x < 8; ++x)
                        block[y * 8 + x] = plane[(y0 + y) * stride + x0 + x];
            }

            void encodeBlock(BitWriter &writer, int component, const int samples[64])
            {
                double f[64];
                forwardDct(samples, f);
                // Intra DC: value * 8 with 8-bit precision (block[0] = pred << 3).
                const int dc = std::clamp(static_cast<int>(std::lround(f[0] / 8.0)), 0, 255);
                const int diff = dc - dcPred_[component];
                dcPred_[component] = dc;
                const int size = bitsFor(std::abs(diff));
                const auto &dcCode = component == 0 ? kDcLuma[size] : kDcChroma[size];
                writer.put(dcCode[0], dcCode[1]);
                if (size)
                    writer.put(static_cast<uint32_t>(diff > 0 ? diff : diff + (1 << size) - 1), size);

                if (!dcOnly_)
                {
                    // Decoder: value = level * (2 * code) * W / 16, truncated towards zero.
                    const double scale = 2.0 * code_;
                    int run = 0;
                    for (int k = 1; k < 64; ++k)
                    {
                        const int j = kZigzag[k];
                        const int level =
                            std::clamp(static_cast<int>(std::lround(f[j] * 16.0 / (scale * matrix_[j]))), -2047, 2047);
                        if (!level)
                        {
                            ++run;
                            continue;
                        }
                        const int magnitude = std::abs(level);
                        const Vlc vlc = magnitude <= 40 ? tables().runLevel[run][magnitude] : Vlc{};
                        if (vlc.length)
                        {
                            writer.put(vlc.code, vlc.length);
                            writer.put(level < 0 ? 1u : 0u, 1);
                        }
                        else
                        {
                            writer.put(1, 6); // escape '000001'
                            writer.put(static_cast<uint32_t>(run), 6);
                            writer.put(static_cast<uint32_t>(level) & 0xfffu, 12);
                        }
                        run = 0;
                    }
                }
                writer.put(2, 2); // end of block '10'
            }

            const YuvPicture &picture_;
            int code_;
            bool dcOnly_;
            int dcPred_[3]{};
            uint8_t matrix_[64]{};
        };

        void assemble(const YuvPicture &picture, int code, bool dcOnly, std::vector<uint8_t> &frame)
        {
            frame.assign(IpumHeaderBytes + 1, 0);
            Encoder(picture, code, dcOnly).run(frame);
            frame.resize((frame.size() + 15u) & ~size_t(15), 0);
            const uint32_t size = static_cast<uint32_t>(frame.size());
            const uint16_t w = static_cast<uint16_t>(picture.width), h = static_cast<uint16_t>(picture.height);
            std::memcpy(frame.data(), &IpumMagic, 4);
            std::memcpy(frame.data() + 4, &size, 4); // bytes from the header to the end (EYETOY 0x1e5c)
            std::memcpy(frame.data() + 8, &w, 2);    // must equal the opened size (CORE 0x58cbe8)
            std::memcpy(frame.data() + 10, &h, 2);
        }
    }

    void rgbToYuv420(const uint8_t *rgb, uint32_t width, uint32_t height, YuvPicture &out)
    {
        out.width = width;
        out.height = height;
        out.y.assign(size_t(width) * height, 16);
        out.cb.assign(size_t(width / 2) * (height / 2), 128);
        out.cr.assign(size_t(width / 2) * (height / 2), 128);
        std::vector<double> cbSum(out.cb.size(), 0.0), crSum(out.cr.size(), 0.0);
        for (uint32_t y = 0; y < height; ++y)
            for (uint32_t x = 0; x < width; ++x)
            {
                const uint8_t *p = rgb + (size_t(y) * width + x) * 3;
                const double r = p[0], g = p[1], b = p[2];
                // BT.601 limited range, the inverse of the IPU CSC (1.164, 1.594, 0.8125, 0.391, 2.016).
                out.y[size_t(y) * width + x] =
                    static_cast<uint8_t>(std::clamp(std::lround(16.0 + 0.256788 * r + 0.504129 * g + 0.097906 * b), 16L, 235L));
                const size_t c = size_t(y / 2) * (width / 2) + x / 2;
                cbSum[c] += -0.148223 * r - 0.290993 * g + 0.439216 * b;
                crSum[c] += 0.439216 * r - 0.367788 * g - 0.071427 * b;
            }
        for (size_t i = 0; i < out.cb.size(); ++i)
        {
            out.cb[i] = static_cast<uint8_t>(std::clamp(std::lround(128.0 + cbSum[i] / 4.0), 16L, 240L));
            out.cr[i] = static_cast<uint8_t>(std::clamp(std::lround(128.0 + crSum[i] / 4.0), 16L, 240L));
        }
    }

    bool ipumHasPackedQword(const uint8_t *frame, size_t bytes)
    {
        for (size_t offset = 0; offset + 4 <= bytes; offset += 16)
        {
            uint32_t word = 0;
            std::memcpy(&word, frame + offset, 4);
            if (word == 0xffffffffu)
                return true;
        }
        return false;
    }

    const uint8_t *ipumIntraMatrix()
    {
        return kIntraZigzag;
    }

    bool ipumRunLevelCode(int run, int level, uint16_t &code, int &length)
    {
        if (run < 0 || run > 63 || level < 1 || level > 40)
            return false;
        const Vlc vlc = tables().runLevel[run][level];
        code = vlc.code;
        length = vlc.length;
        return vlc.length != 0;
    }

    bool encodeIpum(const YuvPicture &picture, size_t maxBytes, int preferredCode, IpumFrame &out)
    {
        out = {};
        if (!picture.width || !picture.height || picture.width % 16 || picture.height % 16 ||
            picture.y.size() < size_t(picture.width) * picture.height ||
            picture.cb.size() < size_t(picture.width / 2) * (picture.height / 2) ||
            picture.cr.size() < size_t(picture.width / 2) * (picture.height / 2))
            return false;
        std::vector<uint8_t> frame;
        auto attempt = [&](int code, bool dcOnly) {
            assemble(picture, code, dcOnly, frame);
            ++out.passes;
            return frame.size() <= maxBytes;
        };

        // Rate control: the smallest code from `preferredCode` up that fits.
        int code = std::clamp(preferredCode, 1, 31);
        bool dcOnly = false;
        if (!attempt(code, false))
        {
            int lo = code + 1, hi = 31;
            if (lo > hi || !attempt(hi, false))
            {
                dcOnly = true;
                code = 31;
                if (!attempt(code, true))
                    return false;
            }
            else
            {
                while (lo < hi)
                {
                    const int mid = (lo + hi) / 2;
                    if (attempt(mid, false))
                        hi = mid;
                    else
                        lo = mid + 1;
                }
                code = hi;
                attempt(code, false);
            }
        }

        // Pack rule: change the quantiser (it changes every coefficient's code)
        // until no aligned qword starts with 0xFFFFFFFF.
        for (int retry = 0; ipumHasPackedQword(frame.data(), frame.size()); ++retry)
        {
            if (retry == 8)
                return false;
            ++out.packRetries;
            const int next = code < 31 ? code + 1 : code - 1;
            if (!attempt(next, dcOnly))
            {
                if (dcOnly || !attempt(next, true))
                    return false;
                dcOnly = true;
            }
            code = next;
        }
        out.bytes = std::move(frame);
        out.quantiserScaleCode = code;
        out.dcOnly = dcOnly;
        return true;
    }
}
