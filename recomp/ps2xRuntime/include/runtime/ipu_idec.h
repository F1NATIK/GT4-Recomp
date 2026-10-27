#pragma once
#include <cstddef>
#include <cstdint>
#include <string>

namespace ps2x::video {

// Native decoder for the macroblock stream the PS2 IPU decodes with IDEC (intra
// pictures: MPEG-1/2 intra macroblocks without picture or slice headers),
// colour-converted to RGB32 the way the IPU's CSC stage does. Nothing of the IPU
// itself (registers, FIFOs, DMA) is modelled: callers pass the IPU_CTRL / IDEC /
// SETIQ / SETTH values the game programmed and receive the macroblocks the
// fromIPU DMA would have written.
struct IpuIdecParams {
    int intraDcPrecision = 0;    // IPU_CTRL.IDP (0..3)
    bool alternateScan = false;  // IPU_CTRL.AS
    bool intraVlcFormat = false; // IPU_CTRL.IVF (DCT table B-15 instead of B-14)
    bool nonLinearQuant = false; // IPU_CTRL.QST
    bool mpeg1 = false;          // IPU_CTRL.MP1
    bool dctTypeDecode = false;  // IDEC.DTD (a dct_type bit follows macroblock_type)
    int quantizerScaleCode = 1;  // IDEC.QSC, in force until a macroblock sets one
    bool signedOutput = false;   // IDEC.SGN
    uint32_t th0 = 0, th1 = 0;   // SETTH thresholds (9 bits each)
    uint8_t intraMatrix[64]{};   // SETIQ intra matrix in load (zigzag) order
};

// MSB-first reader over a byte stream. Reads past the end return zero bits and
// set overrun(); the IPU would instead wait for more input.
class IpuBitReader {
public:
    IpuBitReader(const uint8_t* data, size_t bytes, size_t bitOffset = 0)
        : data_(data), bits_(bytes * 8u), pos_(bitOffset) {}
    uint32_t peek(int count) const;
    void skip(int count) { pos_ += static_cast<size_t>(count); }
    uint32_t get(int count) { const uint32_t v = peek(count); skip(count); return v; }
    bool overrun() const { return pos_ > bits_; }
    size_t position() const { return pos_; }

private:
    const uint8_t* data_;
    size_t bits_;
    size_t pos_;
};

struct IpuMacroblock {
    uint8_t y[16 * 16];
    uint8_t cb[8 * 8];
    uint8_t cr[8 * 8];
};

class IpuIdecDecoder {
public:
    explicit IpuIdecDecoder(const IpuIdecParams& params);
    // IDEC starts with the DC predictors reset; MPEG slices reset them too.
    void resetDcPredictors();
    void setQuantizerScaleCode(int code);
    // macroblock_modes, dct_type, quantiser_scale_code and the six intra blocks.
    bool decodeMacroblock(IpuBitReader& bits, IpuMacroblock& mb);
    // macroblock_address_increment after a macroblock (escapes and stuffing
    // consumed). Returns the increment, or 0 when the next bits are not an
    // increment: a start code or the end of the data, where IDEC stops.
    int macroblockAddressIncrement(IpuBitReader& bits);
    const std::string& error() const { return error_; }

private:
    bool decodeBlock(IpuBitReader& bits, int component, int16_t block[64]);
    bool fail(const char* message);

    IpuIdecParams params_;
    int quantizerScale_ = 2;
    int dcPred_[3]{};
    uint8_t intraMatrixNatural_[64]{};
    std::string error_;
};

// The IPU CSC for one macroblock: RGBA bytes, 16x16 in raster order (1024 bytes).
void ipuCscRgb32(const IpuMacroblock& mb, const IpuIdecParams& params, uint8_t* rgba);

struct IpuIdecResult {
    size_t macroblocks = 0;
    size_t bitsConsumed = 0;
    bool reachedEnd = false; // the stream ended (IDEC's end condition) before output filled
};
// A whole IDEC to RGB32: macroblocks until the stream ends or outBytes is full.
bool ipuDecodeIdecRgb32(IpuBitReader& bits, const IpuIdecParams& params, uint8_t* out, size_t outBytes,
                        IpuIdecResult& result, std::string& error);

// Shared pieces for the IPU core (ps2_ipu.cpp): VLC lookups, scans, IDCT.
struct IpuDctCode {
    int kind = 0; // 0 invalid, 1 coefficient, 2 end of block, 3 escape
    int length = 0;
    int run = 0;
    int level = 0;
};
IpuDctCode ipuDctLookup(uint32_t next16, bool tableB15);
// dct_dc_size: size, or -1 for an invalid code; length set on success.
int ipuDcSizeLookup(uint32_t next10, bool chroma, int& length);
// macroblock_address_increment: 1..33, 34 escape, 35 stuffing, or -1 (start code / invalid).
int ipuMbaLookup(uint32_t next11, int& length);
const uint8_t* ipuScanOrder(bool alternate);
int ipuQuantizerScale(int code, bool nonLinear);
// IEEE-1180 IDCT of dequantised coefficients, rounded, not clamped.
void ipuIdct(const int16_t coefficients[64], int32_t samples[64]);

// Table self-check (prefix-free VLC tables); empty on success.
std::string ipuIdecSelfCheck();

} // namespace ps2x::video
