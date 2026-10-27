#include "runtime/ipu_idec.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>

namespace ps2x::video {
namespace {

// DCT coefficient VLCs (ISO/IEC 13818-2 tables B-14 and B-15), sign bit
// excluded, as {code, length} in the run/level order of kRun/kLevel.
constexpr int kRlCount = 111;
constexpr uint16_t kTableB14[kRlCount][2] = {
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
constexpr uint16_t kTableB15[kRlCount][2] = {
    {0x02, 2},  {0x06, 3},  {0x07, 4},  {0x1c, 5},  {0x1d, 5},  {0x05, 6},  {0x04, 6},  {0x7b, 7},
    {0x7c, 7},  {0x23, 8},  {0x22, 8},  {0xfa, 8},  {0xfb, 8},  {0xfe, 8},  {0xff, 8},  {0x1f, 14},
    {0x1e, 14}, {0x1d, 14}, {0x1c, 14}, {0x1b, 14}, {0x1a, 14}, {0x19, 14}, {0x18, 14}, {0x17, 14},
    {0x16, 14}, {0x15, 14}, {0x14, 14}, {0x13, 14}, {0x12, 14}, {0x11, 14}, {0x10, 14}, {0x18, 15},
    {0x17, 15}, {0x16, 15}, {0x15, 15}, {0x14, 15}, {0x13, 15}, {0x12, 15}, {0x11, 15}, {0x10, 15},
    {0x02, 3},  {0x06, 5},  {0x79, 7},  {0x27, 8},  {0x20, 8},  {0x16, 13}, {0x15, 13}, {0x1f, 15},
    {0x1e, 15}, {0x1d, 15}, {0x1c, 15}, {0x1b, 15}, {0x1a, 15}, {0x19, 15}, {0x13, 16}, {0x12, 16},
    {0x11, 16}, {0x10, 16}, {0x05, 5},  {0x07, 7},  {0xfc, 8},  {0x0c, 10}, {0x14, 13}, {0x07, 5},
    {0x26, 8},  {0x1c, 12}, {0x13, 13}, {0x06, 6},  {0xfd, 8},  {0x12, 12}, {0x07, 6},  {0x04, 9},
    {0x12, 13}, {0x06, 7},  {0x1e, 12}, {0x14, 16}, {0x04, 7},  {0x15, 12}, {0x05, 7},  {0x11, 12},
    {0x78, 7},  {0x11, 13}, {0x7a, 7},  {0x10, 13}, {0x21, 8},  {0x1a, 16}, {0x25, 8},  {0x19, 16},
    {0x24, 8},  {0x18, 16}, {0x05, 9},  {0x17, 16}, {0x07, 9},  {0x16, 16}, {0x0d, 10}, {0x15, 16},
    {0x1f, 12}, {0x1a, 12}, {0x19, 12}, {0x17, 12}, {0x16, 12}, {0x1f, 13}, {0x1e, 13}, {0x1d, 13},
    {0x1c, 13}, {0x1b, 13}, {0x1f, 16}, {0x1e, 16}, {0x1d, 16}, {0x1c, 16}, {0x1b, 16}};
// Escape and end of block, also without sign.
constexpr uint16_t kEscapeB14[2] = {0x1, 6}, kEobB14[2] = {0x2, 2};
constexpr uint16_t kEscapeB15[2] = {0x1, 6}, kEobB15[2] = {0x6, 4};

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
constexpr uint16_t kDcLuma[12][2] = {{0x4, 3},  {0x0, 2},  {0x1, 2},  {0x5, 3},   {0x6, 3},   {0xe, 4},
                                     {0x1e, 5}, {0x3e, 6}, {0x7e, 7}, {0xfe, 8}, {0x1fe, 9}, {0x1ff, 9}};
constexpr uint16_t kDcChroma[12][2] = {{0x0, 2},  {0x1, 2},  {0x2, 2},  {0x6, 3},    {0xe, 4},     {0x1e, 5},
                                       {0x3e, 6}, {0x7e, 7}, {0xfe, 8}, {0x1fe, 9}, {0x3fe, 10}, {0x3ff, 10}};
// macroblock_address_increment 1..33 (table B-1), then escape and stuffing.
constexpr uint16_t kMba[35][2] = {
    {0x1, 1},   {0x3, 3},   {0x2, 3},   {0x3, 4},   {0x2, 4},   {0x3, 5},   {0x2, 5},   {0x7, 7},   {0x6, 7},
    {0xb, 8},   {0xa, 8},   {0x9, 8},   {0x8, 8},   {0x7, 8},   {0x6, 8},   {0x17, 10}, {0x16, 10}, {0x15, 10},
    {0x14, 10}, {0x13, 10}, {0x12, 10}, {0x23, 11}, {0x22, 11}, {0x21, 11}, {0x20, 11}, {0x1f, 11}, {0x1e, 11},
    {0x1d, 11}, {0x1c, 11}, {0x1b, 11}, {0x1a, 11}, {0x19, 11}, {0x18, 11}, {0x8, 11},  {0xf, 11}};
constexpr int kMbaEscape = 34, kMbaStuffing = 35;

constexpr uint8_t kScanZigzag[64] = {0,  1,  8,  16, 9,  2,  3,  10, 17, 24, 32, 25, 18, 11, 4,  5,
                                     12, 19, 26, 33, 40, 48, 41, 34, 27, 20, 13, 6,  7,  14, 21, 28,
                                     35, 42, 49, 56, 57, 50, 43, 36, 29, 22, 15, 23, 30, 37, 44, 51,
                                     58, 59, 52, 45, 38, 31, 39, 46, 53, 60, 61, 54, 47, 55, 62, 63};
constexpr uint8_t kScanAlternate[64] = {0,  8,  16, 24, 1,  9,  2,  10, 17, 25, 32, 40, 48, 56, 57, 49,
                                        41, 33, 26, 18, 3,  11, 4,  12, 19, 27, 34, 42, 50, 58, 35, 43,
                                        51, 59, 20, 28, 5,  13, 6,  14, 21, 29, 36, 44, 52, 60, 37, 45,
                                        53, 61, 22, 30, 7,  15, 23, 31, 38, 46, 54, 62, 39, 47, 55, 63};
constexpr uint8_t kNonLinearQuant[32] = {0,  1,  2,  3,  4,  5,  6,  7,  8,  10, 12, 14, 16, 18,  20,  22,
                                         24, 28, 32, 36, 40, 44, 48, 52, 56, 64, 72, 80, 88, 96, 104, 112};

enum : int8_t { kInvalid = 0, kCoeff = 1, kEob = 2, kEscape = 3 };
struct DctEntry {
    int8_t kind = kInvalid;
    uint8_t length = 0;
    uint8_t run = 0;
    uint8_t level = 0;
};
struct SmallEntry {
    int8_t value = -1; // size or increment index; -1 invalid
    uint8_t length = 0;
};

struct Tables {
    std::array<DctEntry, 65536> b14{}, b15{};
    std::array<SmallEntry, 512> dcLuma{};    // 9-bit index
    std::array<SmallEntry, 1024> dcChroma{}; // 10-bit index
    std::array<SmallEntry, 2048> mba{};      // 11-bit index
    double idct[8][8]{};                     // [frequency][sample]
    std::string error;

    template <size_t N>
    void fillDct(std::array<DctEntry, N>& table, const uint16_t (&codes)[kRlCount][2], const uint16_t (&esc)[2],
                 const uint16_t (&eob)[2], const char* name)
    {
        auto put = [&](uint16_t code, int length, DctEntry entry) {
            entry.length = static_cast<uint8_t>(length);
            const int shift = 16 - length;
            for (uint32_t low = 0; low < (1u << shift); ++low) {
                DctEntry& slot = table[(static_cast<uint32_t>(code) << shift) | low];
                if (slot.kind != kInvalid && error.empty())
                    error = std::string(name) + " is not prefix-free";
                slot = entry;
            }
        };
        for (int i = 0; i < kRlCount; ++i)
            put(codes[i][0], codes[i][1],
                DctEntry{kCoeff, 0, static_cast<uint8_t>(runOf(i)), static_cast<uint8_t>(levelOf(i))});
        put(esc[0], esc[1], DctEntry{kEscape});
        put(eob[0], eob[1], DctEntry{kEob});
    }
    template <size_t N, size_t M>
    void fillSmall(std::array<SmallEntry, N>& table, int bits, const uint16_t (&codes)[M][2], const char* name)
    {
        for (size_t v = 0; v < M; ++v) {
            const int shift = bits - codes[v][1];
            for (uint32_t low = 0; low < (1u << shift); ++low) {
                SmallEntry& slot = table[(static_cast<uint32_t>(codes[v][0]) << shift) | low];
                if (slot.value >= 0 && error.empty())
                    error = std::string(name) + " is not prefix-free";
                slot = SmallEntry{static_cast<int8_t>(v), static_cast<uint8_t>(codes[v][1])};
            }
        }
    }

    Tables()
    {
        fillDct(b14, kTableB14, kEscapeB14, kEobB14, "table B-14");
        fillDct(b15, kTableB15, kEscapeB15, kEobB15, "table B-15");
        fillSmall(dcLuma, 9, kDcLuma, "DC luminance size");
        fillSmall(dcChroma, 10, kDcChroma, "DC chrominance size");
        fillSmall(mba, 11, kMba, "macroblock_address_increment");
        // Coverage: B-14 leaves only the twelve-zero prefix unused (16 of 65536
        // slots); B-15 also leaves free the six 12-bit and four 13-bit B-14 codes
        // whose run/level pairs it moves to short codes (96 + 32 more).
        auto invalidSlots = [](const std::array<DctEntry, 65536>& table) {
            return std::count_if(table.begin(), table.end(), [](const DctEntry& e) { return e.kind == kInvalid; });
        };
        if ((invalidSlots(b14) != 16 || invalidSlots(b15) != 144) && error.empty())
            error = "DCT table coverage mismatch";
        const double pi = std::acos(-1.0);
        for (int u = 0; u < 8; ++u)
            for (int x = 0; x < 8; ++x)
                idct[u][x] = (u ? 0.5 : 0.5 / std::sqrt(2.0)) * std::cos((2 * x + 1) * u * pi / 16.0);
    }
};

const Tables& tables()
{
    static const Tables value;
    return value;
}

int signExtend(uint32_t value, int bits)
{
    const uint32_t sign = 1u << (bits - 1);
    return static_cast<int>((value ^ sign) - sign);
}

// The separable IDCT in double precision (IEEE 1180 accuracy): the row pass sums over u, the column pass over v,
// each sum from 0.0 in ascending order. Terms that are exactly zero (a zero coefficient, a row whose coefficients are
// all zero) are left out: adding 0.0 changes no sum but the sign of a zero, which the rounding below ignores, so the
// results are bit-identical to summing all 64 terms. The sums are kept 8 at a time (along x) so the compiler can do
// them side by side; GT4's menu movies decode ~200,000 blocks a second on the game thread.
void idctSums(const int16_t coefficients[64], double out[64])
{
    const auto& t = tables();
    double rows[64];
    int used[8], usedCount = 0;
    for (int y = 0; y < 8; ++y) {
        const int16_t* in = coefficients + y * 8;
        double* r = rows + y * 8;
        bool any = false;
        for (int x = 0; x < 8; ++x)
            r[x] = 0.0;
        for (int u = 0; u < 8; ++u) {
            if (!in[u])
                continue;
            any = true;
            const double c = in[u];
            for (int x = 0; x < 8; ++x)
                r[x] += t.idct[u][x] * c;
        }
        if (any)
            used[usedCount++] = y;
    }
    for (int i = 0; i < 64; ++i)
        out[i] = 0.0;
    for (int k = 0; k < usedCount; ++k) {
        const int v = used[k];
        const double* r = rows + v * 8;
        for (int y = 0; y < 8; ++y) {
            const double c = t.idct[v][y];
            double* o = out + y * 8;
            for (int x = 0; x < 8; ++x)
                o[x] += c * r[x];
        }
    }
}

// std::lround (half away from zero) for the IDCT's small sums: v - trunc(v) is exact.
inline long roundHalfAway(double v)
{
    const long whole = static_cast<long>(v);
    const double frac = v - static_cast<double>(whole);
    return frac >= 0.5 ? whole + 1 : (frac <= -0.5 ? whole - 1 : whole);
}

// The IDCT rounded and clamped to pixels.
void idctToPixels(const int16_t block[64], uint8_t* dest, int stride)
{
    double sums[64];
    idctSums(block, sums);
    for (int y = 0; y < 8; ++y)
        for (int x = 0; x < 8; ++x)
            dest[y * stride + x] = static_cast<uint8_t>(std::clamp(roundHalfAway(sums[y * 8 + x]), 0L, 255L));
}

} // namespace

uint32_t IpuBitReader::peek(int count) const
{
    uint32_t value = 0;
    size_t pos = pos_;
    for (int remaining = count; remaining > 0;) {
        const size_t byte = pos >> 3;
        const int offset = static_cast<int>(pos & 7u);
        const int take = std::min(remaining, 8 - offset);
        const uint32_t bits = byte < (bits_ >> 3) ? data_[byte] : 0u;
        value = (value << take) | ((bits >> (8 - offset - take)) & ((1u << take) - 1u));
        pos += static_cast<size_t>(take);
        remaining -= take;
    }
    return value;
}

IpuIdecDecoder::IpuIdecDecoder(const IpuIdecParams& params) : params_(params)
{
    // SETIQ loads the matrix in zigzag order whatever the scan (as MPEG streams do).
    for (int i = 0; i < 64; ++i)
        intraMatrixNatural_[kScanZigzag[i]] = params.intraMatrix[i];
    setQuantizerScaleCode(params.quantizerScaleCode);
    resetDcPredictors();
}

void IpuIdecDecoder::resetDcPredictors()
{
    dcPred_[0] = dcPred_[1] = dcPred_[2] = 128 << params_.intraDcPrecision;
}

void IpuIdecDecoder::setQuantizerScaleCode(int code)
{
    code &= 31;
    quantizerScale_ = params_.nonLinearQuant ? kNonLinearQuant[code] : code * 2;
}

bool IpuIdecDecoder::fail(const char* message)
{
    if (error_.empty())
        error_ = message;
    return false;
}

bool IpuIdecDecoder::decodeBlock(IpuBitReader& bits, int component, int16_t block[64])
{
    const Tables& t = tables();
    std::memset(block, 0, 64 * sizeof(int16_t));

    const SmallEntry dc = component == 0 ? t.dcLuma[bits.peek(9)] : t.dcChroma[bits.peek(10)];
    if (dc.value < 0)
        return fail("invalid dct_dc_size");
    bits.skip(dc.length);
    int diff = 0;
    if (const int size = dc.value) {
        diff = static_cast<int>(bits.get(size));
        if (!(diff >> (size - 1)))
            diff -= (1 << size) - 1;
    }
    dcPred_[component] += diff;
    block[0] = static_cast<int16_t>(dcPred_[component] << (3 - params_.intraDcPrecision));

    const auto& table = (params_.intraVlcFormat && !params_.mpeg1) ? t.b15 : t.b14;
    const uint8_t* scan = params_.alternateScan ? kScanAlternate : kScanZigzag;
    int sum = block[0];
    for (int i = 0;;) {
        const DctEntry& e = table[bits.peek(16)];
        int level;
        if (e.kind == kEob) {
            bits.skip(e.length);
            break;
        }
        if (e.kind == kCoeff) {
            bits.skip(e.length);
            i += e.run + 1;
            level = bits.get(1) ? -e.level : e.level;
        } else if (e.kind == kEscape) {
            bits.skip(e.length);
            i += static_cast<int>(bits.get(6)) + 1;
            if (params_.mpeg1) {
                level = signExtend(bits.get(8), 8);
                if (level == -128)
                    level = static_cast<int>(bits.get(8)) - 256;
                else if (level == 0)
                    level = static_cast<int>(bits.get(8));
            } else {
                level = signExtend(bits.get(12), 12);
            }
            if (level == 0)
                return fail("escaped DCT level of zero");
        } else {
            return fail("invalid DCT coefficient code");
        }
        if (i > 63)
            return fail("DCT coefficients past the end of the block");
        const int j = scan[i];
        // (2 * level * quantiser_scale * W) / 32, truncated towards zero.
        int value = level * quantizerScale_ * intraMatrixNatural_[j] / 16;
        if (params_.mpeg1 && value && !(value & 1))
            value -= value > 0 ? 1 : -1; // MPEG-1 oddification
        value = std::clamp(value, -2048, 2047);
        block[j] = static_cast<int16_t>(value);
        sum += value;
        if (bits.overrun())
            return fail("macroblock data runs past the input");
    }
    if (!params_.mpeg1 && !(sum & 1))
        block[63] ^= 1; // MPEG-2 mismatch control
    return !bits.overrun() || fail("macroblock data runs past the input");
}

bool IpuIdecDecoder::decodeMacroblock(IpuBitReader& bits, IpuMacroblock& mb)
{
    // macroblock_type in an I picture: 1 = intra, 01 = intra with quantiser_scale_code.
    bool quant;
    if (bits.peek(1)) {
        bits.skip(1);
        quant = false;
    } else if (bits.peek(2) == 1) {
        bits.skip(2);
        quant = true;
    } else {
        return fail("invalid macroblock_type");
    }
    const bool fieldDct = params_.dctTypeDecode && bits.get(1);
    if (quant)
        setQuantizerScaleCode(static_cast<int>(bits.get(5)));

    int16_t block[64];
    for (int b = 0; b < 4; ++b) {
        if (!decodeBlock(bits, 0, block))
            return false;
        const int x = (b & 1) * 8;
        // Frame DCT: 8x8 quadrants; field DCT: blocks 0/1 take the even lines, 2/3 the odd.
        const int y = fieldDct ? (b >> 1) : (b >> 1) * 8;
        idctToPixels(block, mb.y + y * 16 + x, fieldDct ? 32 : 16);
    }
    if (!decodeBlock(bits, 1, block))
        return false;
    idctToPixels(block, mb.cb, 8);
    if (!decodeBlock(bits, 2, block))
        return false;
    idctToPixels(block, mb.cr, 8);
    return true;
}

int IpuIdecDecoder::macroblockAddressIncrement(IpuBitReader& bits)
{
    const Tables& t = tables();
    int increment = 0;
    for (;;) {
        const SmallEntry e = t.mba[bits.peek(11)];
        if (e.value < 0 || bits.overrun())
            return 0; // start code (or zero padding): end of the IDEC stream
        bits.skip(e.length);
        if (e.value + 1 == kMbaEscape)
            increment += 33;
        else if (e.value + 1 != kMbaStuffing)
            return increment + e.value + 1;
    }
}

void ipuCscRgb32(const IpuMacroblock& mb, const IpuIdecParams& params, uint8_t* rgba)
{
    // The IPU's fixed YCbCr->RGB: 1.1640625, 1.59375, -0.8125, -0.390625, 2.015625. The chroma terms are worked out
    // once per chroma sample (2x2 pixels), the same integers as per pixel.
    int rTerm[64], gTerm[64], bTerm[64];
    for (int i = 0; i < 64; ++i) {
        const int cb = mb.cb[i] - 128;
        const int cr = mb.cr[i] - 128;
        rTerm[i] = (0xcc * cr) >> 6;
        gTerm[i] = ((-0x68 * cr) >> 6) + ((-0x32 * cb) >> 6);
        bTerm[i] = (0x102 * cb) >> 6;
    }
    for (int y = 0; y < 16; ++y)
        for (int x = 0; x < 16; ++x) {
            const int lum = (0x95 * std::max(0, mb.y[y * 16 + x] - 16)) >> 6;
            const int c = (y >> 1) * 8 + (x >> 1);
            const int r = std::clamp((lum + rTerm[c] + 1) >> 1, 0, 255);
            const int g = std::clamp((lum + gTerm[c] + 1) >> 1, 0, 255);
            const int b = std::clamp((lum + bTerm[c] + 1) >> 1, 0, 255);
            uint8_t* p = rgba + (y * 16 + x) * 4;
            p[0] = static_cast<uint8_t>(r);
            p[1] = static_cast<uint8_t>(g);
            p[2] = static_cast<uint8_t>(b);
            p[3] = 0x80;
            // SETTH: below TH0 in all channels is transparent black, below TH1 half alpha.
            const uint32_t lo = params.th0, hi = params.th1;
            if (lo > 0 && uint32_t(r) < lo && uint32_t(g) < lo && uint32_t(b) < lo)
                std::memset(p, 0, 4);
            else if (hi > 0 && uint32_t(r) < hi && uint32_t(g) < hi && uint32_t(b) < hi)
                p[3] = 0x40;
            if (params.signedOutput) {
                p[0] ^= 0x80;
                p[1] ^= 0x80;
                p[2] ^= 0x80;
            }
        }
}

bool ipuDecodeIdecRgb32(IpuBitReader& bits, const IpuIdecParams& params, uint8_t* out, size_t outBytes,
                        IpuIdecResult& result, std::string& error)
{
    IpuIdecDecoder decoder(params);
    IpuMacroblock mb;
    const size_t start = bits.position();
    result = {};
    for (size_t offset = 0; offset + 1024 <= outBytes; offset += 1024) {
        if (!decoder.decodeMacroblock(bits, mb)) {
            error = decoder.error();
            result.bitsConsumed = bits.position() - start;
            return false;
        }
        ipuCscRgb32(mb, params, out + offset);
        ++result.macroblocks;
        if (offset + 2048 > outBytes)
            break; // output full: the IPU stalls here and the game stops it
        if (!decoder.macroblockAddressIncrement(bits)) {
            result.reachedEnd = true;
            break;
        }
    }
    result.bitsConsumed = bits.position() - start;
    return true;
}

IpuDctCode ipuDctLookup(uint32_t next16, bool tableB15)
{
    const DctEntry& e = (tableB15 ? tables().b15 : tables().b14)[next16 & 0xffffu];
    return IpuDctCode{e.kind, e.length, e.run, e.level};
}

int ipuDcSizeLookup(uint32_t next10, bool chroma, int& length)
{
    const SmallEntry e = chroma ? tables().dcChroma[next10 & 0x3ffu] : tables().dcLuma[(next10 >> 1) & 0x1ffu];
    length = e.length;
    return e.value;
}

int ipuMbaLookup(uint32_t next11, int& length)
{
    const SmallEntry e = tables().mba[next11 & 0x7ffu];
    length = e.length;
    return e.value < 0 ? -1 : e.value + 1;
}

const uint8_t* ipuScanOrder(bool alternate)
{
    return alternate ? kScanAlternate : kScanZigzag;
}

int ipuQuantizerScale(int code, bool nonLinear)
{
    code &= 31;
    return nonLinear ? kNonLinearQuant[code] : code * 2;
}

void ipuIdct(const int16_t coefficients[64], int32_t samples[64])
{
    double sums[64];
    idctSums(coefficients, sums);
    for (int i = 0; i < 64; ++i)
        samples[i] = static_cast<int32_t>(roundHalfAway(sums[i]));
}

std::string ipuIdecSelfCheck()
{
    return tables().error;
}

} // namespace ps2x::video
