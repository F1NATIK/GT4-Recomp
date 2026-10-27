#include "gt4_eyetoy_source.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>

namespace ps2x::iop::gt4::eyetoy
{
    namespace
    {
        // ---- RFC 1951 raw DEFLATE decoder (GT4.VOL entries) ----
        constexpr uint16_t kLengthBase[29] = {3,  4,  5,  6,  7,  8,  9,  10, 11,  13,  15,  17,  19,  23, 27,
                                              31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
        constexpr uint8_t kLengthExtra[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2,
                                              2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
        constexpr uint16_t kDistanceBase[30] = {1,   2,   3,   4,   5,   7,    9,    13,   17,   25,   33,   49,    65,    97,    129,
                                                193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
        constexpr uint8_t kDistanceExtra[30] = {0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6,
                                                6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};
        constexpr uint8_t kCodeLengthOrder[19] = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};

        struct Huffman
        {
            std::array<uint16_t, 16> counts{};   // codes per length
            std::array<uint16_t, 320> symbols{}; // symbols ordered by (length, value)
        };

        // Canonical code from code lengths. Incomplete codes are allowed (RFC 1951
        // permits a single distance code); over-subscribed ones are not.
        bool buildHuffman(Huffman &h, const uint8_t *lengths, int count)
        {
            h.counts.fill(0);
            for (int i = 0; i < count; ++i)
                ++h.counts[lengths[i]];
            h.counts[0] = 0;
            int left = 1;
            for (int length = 1; length < 16; ++length)
            {
                left = (left << 1) - h.counts[length];
                if (left < 0)
                    return false;
            }
            std::array<uint16_t, 16> next{};
            for (int length = 1; length < 15; ++length)
                next[length + 1] = static_cast<uint16_t>(next[length] + h.counts[length]);
            for (int symbol = 0; symbol < count; ++symbol)
                if (lengths[symbol])
                    h.symbols[next[lengths[symbol]]++] = static_cast<uint16_t>(symbol);
            return true;
        }

        class Inflater
        {
        public:
            Inflater(const uint8_t *data, size_t bytes, size_t limit, std::vector<uint8_t> &out)
                : in_(data), size_(bytes), limit_(limit), out_(out)
            {
            }

            bool run()
            {
                for (int last = 0; !last;)
                {
                    int type = 0;
                    if (!bits(1, last) || !bits(2, type))
                        return false;
                    if (type == 0 ? !stored() : type == 1 ? !fixed() : type == 2 ? !dynamic() : true)
                        return false;
                }
                return true;
            }

        private:
            bool bits(int count, int &value)
            {
                while (have_ < count)
                {
                    if (pos_ >= size_)
                        return false;
                    buffer_ |= static_cast<uint32_t>(in_[pos_++]) << have_;
                    have_ += 8;
                }
                value = static_cast<int>(buffer_ & ((1u << count) - 1u));
                buffer_ >>= count;
                have_ -= count;
                return true;
            }

            bool decode(const Huffman &h, int &symbol)
            {
                int code = 0, first = 0, index = 0;
                for (int length = 1; length < 16; ++length)
                {
                    int bit = 0;
                    if (!bits(1, bit))
                        return false;
                    code |= bit;
                    const int count = h.counts[length];
                    if (code - first < count)
                    {
                        symbol = h.symbols[index + code - first];
                        return true;
                    }
                    index += count;
                    first = (first + count) << 1;
                    code <<= 1;
                }
                return false;
            }

            bool stored()
            {
                buffer_ = 0; // the rest of the current byte
                have_ = 0;
                if (size_ - pos_ < 4)
                    return false;
                const unsigned length = in_[pos_] | (in_[pos_ + 1] << 8);
                const unsigned check = in_[pos_ + 2] | (in_[pos_ + 3] << 8);
                pos_ += 4;
                if ((length ^ 0xffffu) != check || size_ - pos_ < length || out_.size() + length > limit_)
                    return false;
                out_.insert(out_.end(), in_ + pos_, in_ + pos_ + length);
                pos_ += length;
                return true;
            }

            bool fixed()
            {
                uint8_t lengths[288 + 30];
                std::fill(lengths, lengths + 144, uint8_t(8));
                std::fill(lengths + 144, lengths + 256, uint8_t(9));
                std::fill(lengths + 256, lengths + 280, uint8_t(7));
                std::fill(lengths + 280, lengths + 288, uint8_t(8));
                std::fill(lengths + 288, lengths + 318, uint8_t(5));
                Huffman literal, distance;
                return buildHuffman(literal, lengths, 288) && buildHuffman(distance, lengths + 288, 30) &&
                       codes(literal, distance);
            }

            bool dynamic()
            {
                int literals = 0, distances = 0, codeLengths = 0;
                if (!bits(5, literals) || !bits(5, distances) || !bits(4, codeLengths))
                    return false;
                literals += 257;
                distances += 1;
                codeLengths += 4;
                if (literals > 286 || distances > 30)
                    return false;
                uint8_t lengths[320]{};
                for (int i = 0; i < codeLengths; ++i)
                {
                    int value = 0;
                    if (!bits(3, value))
                        return false;
                    lengths[kCodeLengthOrder[i]] = static_cast<uint8_t>(value);
                }
                Huffman lengthCode;
                if (!buildHuffman(lengthCode, lengths, 19))
                    return false;
                std::fill(std::begin(lengths), std::end(lengths), uint8_t(0));
                for (int i = 0; i < literals + distances;)
                {
                    int symbol = 0;
                    if (!decode(lengthCode, symbol))
                        return false;
                    if (symbol < 16)
                    {
                        lengths[i++] = static_cast<uint8_t>(symbol);
                        continue;
                    }
                    int repeat = 0, value = 0, extra = 0;
                    if (symbol == 16)
                    {
                        if (i == 0 || !bits(2, extra))
                            return false;
                        value = lengths[i - 1];
                        repeat = 3 + extra;
                    }
                    else if (symbol == 17)
                    {
                        if (!bits(3, extra))
                            return false;
                        repeat = 3 + extra;
                    }
                    else
                    {
                        if (!bits(7, extra))
                            return false;
                        repeat = 11 + extra;
                    }
                    if (i + repeat > literals + distances)
                        return false;
                    while (repeat--)
                        lengths[i++] = static_cast<uint8_t>(value);
                }
                if (!lengths[256])
                    return false; // no end-of-block code
                Huffman literal, distance;
                return buildHuffman(literal, lengths, literals) && buildHuffman(distance, lengths + literals, distances) &&
                       codes(literal, distance);
            }

            bool codes(const Huffman &literal, const Huffman &distance)
            {
                for (;;)
                {
                    int symbol = 0;
                    if (!decode(literal, symbol))
                        return false;
                    if (symbol < 256)
                    {
                        if (out_.size() >= limit_)
                            return false;
                        out_.push_back(static_cast<uint8_t>(symbol));
                        continue;
                    }
                    if (symbol == 256)
                        return true;
                    symbol -= 257;
                    if (symbol >= 29)
                        return false;
                    int extra = 0;
                    if (!bits(kLengthExtra[symbol], extra))
                        return false;
                    const size_t length = kLengthBase[symbol] + static_cast<size_t>(extra);
                    int code = 0;
                    if (!decode(distance, code) || code >= 30 || !bits(kDistanceExtra[code], extra))
                        return false;
                    const size_t back = kDistanceBase[code] + static_cast<size_t>(extra);
                    if (back > out_.size() || out_.size() + length > limit_)
                        return false;
                    for (size_t i = 0; i < length; ++i)
                        out_.push_back(out_[out_.size() - back]);
                }
            }

            const uint8_t *in_;
            size_t size_;
            size_t limit_;
            std::vector<uint8_t> &out_;
            size_t pos_ = 0;
            uint32_t buffer_ = 0;
            int have_ = 0;
        };

        uint32_t readLe32(const uint8_t *p)
        {
            return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24;
        }
        uint32_t readBe32(const uint8_t *p)
        {
            return uint32_t(p[3]) | uint32_t(p[2]) << 8 | uint32_t(p[1]) << 16 | uint32_t(p[0]) << 24;
        }

        // ---- image files ----
        bool loadNetpbm(const std::vector<uint8_t> &file, RgbImage &out, std::string &error)
        {
            const bool colour = file[1] == '6';
            size_t pos = 2;
            uint32_t values[3]{};
            for (int v = 0; v < 3; ++v)
            {
                while (pos < file.size() && (std::isspace(file[pos]) || file[pos] == '#'))
                {
                    if (file[pos] == '#')
                        while (pos < file.size() && file[pos] != '\n')
                            ++pos;
                    else
                        ++pos;
                }
                if (pos >= file.size() || !std::isdigit(file[pos]))
                    return error = "bad PNM header", false;
                while (pos < file.size() && std::isdigit(file[pos]))
                    values[v] = values[v] * 10u + (file[pos++] - '0');
            }
            ++pos; // one whitespace byte before the samples
            const uint32_t w = values[0], h = values[1];
            const size_t channels = colour ? 3 : 1;
            if (!w || !h || w > 8192 || h > 8192 || values[2] == 0 || values[2] > 255 || pos + size_t(w) * h * channels > file.size())
                return error = "unsupported PNM (8-bit P5/P6 only)", false;
            out.width = w;
            out.height = h;
            out.rgb.resize(size_t(w) * h * 3);
            for (size_t i = 0; i < size_t(w) * h; ++i)
                for (size_t c = 0; c < 3; ++c)
                    out.rgb[i * 3 + c] =
                        static_cast<uint8_t>(file[pos + i * channels + (colour ? c : 0)] * 255u / values[2]);
            return true;
        }

        bool loadBmp(const std::vector<uint8_t> &file, RgbImage &out, std::string &error)
        {
            if (file.size() < 54)
                return error = "short BMP", false;
            const uint32_t offset = readLe32(&file[10]);
            const int32_t w = static_cast<int32_t>(readLe32(&file[18]));
            const int32_t hSigned = static_cast<int32_t>(readLe32(&file[22]));
            const uint16_t bpp = static_cast<uint16_t>(file[28] | file[29] << 8);
            const uint32_t compression = readLe32(&file[30]);
            const uint32_t h = static_cast<uint32_t>(std::abs(hSigned));
            if (w <= 0 || w > 8192 || h == 0 || h > 8192 || (bpp != 24 && bpp != 32) || (compression != 0 && compression != 3))
                return error = "unsupported BMP (24/32-bit uncompressed only)", false;
            const size_t stride = ((size_t(w) * bpp / 8) + 3) & ~size_t(3);
            if (offset + stride * h > file.size())
                return error = "short BMP", false;
            out.width = static_cast<uint32_t>(w);
            out.height = h;
            out.rgb.resize(size_t(w) * h * 3);
            for (uint32_t y = 0; y < h; ++y)
            {
                const uint32_t row = hSigned > 0 ? h - 1 - y : y; // positive height: bottom-up
                const uint8_t *src = &file[offset + row * stride];
                for (int32_t x = 0; x < w; ++x)
                {
                    const uint8_t *p = src + size_t(x) * (bpp / 8);
                    uint8_t *d = &out.rgb[(size_t(y) * w + x) * 3];
                    d[0] = p[2];
                    d[1] = p[1];
                    d[2] = p[0];
                }
            }
            return true;
        }

        bool loadPng(const std::vector<uint8_t> &file, RgbImage &out, std::string &error)
        {
            size_t pos = 8;
            uint32_t w = 0, h = 0;
            int depth = 0, colourType = -1, interlace = 0;
            std::vector<uint8_t> idat;
            while (pos + 12 <= file.size())
            {
                const uint32_t length = readBe32(&file[pos]);
                const uint8_t *type = &file[pos + 4];
                if (pos + 12 + size_t(length) > file.size())
                    return error = "short PNG", false;
                const uint8_t *data = &file[pos + 8];
                if (!std::memcmp(type, "IHDR", 4) && length >= 13)
                {
                    w = readBe32(data);
                    h = readBe32(data + 4);
                    depth = data[8];
                    colourType = data[9];
                    interlace = data[12];
                }
                else if (!std::memcmp(type, "IDAT", 4))
                {
                    idat.insert(idat.end(), data, data + length);
                }
                else if (!std::memcmp(type, "IEND", 4))
                {
                    break;
                }
                pos += 12 + size_t(length);
            }
            const int channels = colourType == 0 ? 1 : colourType == 2 ? 3 : colourType == 4 ? 2 : colourType == 6 ? 4 : 0;
            if (!w || !h || w > 8192 || h > 8192 || depth != 8 || !channels || interlace || idat.size() < 6)
                return error = "unsupported PNG (8-bit grey/RGB/RGBA, not interlaced)", false;
            const size_t rowBytes = size_t(w) * channels;
            std::vector<uint8_t> raw;
            // zlib stream: 2-byte header, raw DEFLATE, Adler-32.
            if ((idat[0] & 0x0f) != 8 || !inflateRaw(idat.data() + 2, idat.size() - 2, (rowBytes + 1) * h, raw) ||
                raw.size() != (rowBytes + 1) * h)
                return error = "bad PNG data", false;
            std::vector<uint8_t> pixels(rowBytes * h);
            for (uint32_t y = 0; y < h; ++y)
            {
                const uint8_t filter = raw[y * (rowBytes + 1)];
                const uint8_t *src = &raw[y * (rowBytes + 1) + 1];
                uint8_t *dst = &pixels[y * rowBytes];
                const uint8_t *up = y ? &pixels[(y - 1) * rowBytes] : nullptr;
                for (size_t i = 0; i < rowBytes; ++i)
                {
                    const int a = i >= size_t(channels) ? dst[i - channels] : 0;
                    const int b = up ? up[i] : 0;
                    const int c = (up && i >= size_t(channels)) ? up[i - channels] : 0;
                    int predictor = 0;
                    switch (filter)
                    {
                    case 0: predictor = 0; break;
                    case 1: predictor = a; break;
                    case 2: predictor = b; break;
                    case 3: predictor = (a + b) / 2; break;
                    case 4:
                    {
                        const int p = a + b - c, pa = std::abs(p - a), pb = std::abs(p - b), pc = std::abs(p - c);
                        predictor = (pa <= pb && pa <= pc) ? a : pb <= pc ? b : c;
                        break;
                    }
                    default: return error = "bad PNG filter", false;
                    }
                    dst[i] = static_cast<uint8_t>(src[i] + predictor);
                }
            }
            out.width = w;
            out.height = h;
            out.rgb.resize(size_t(w) * h * 3);
            for (size_t i = 0; i < size_t(w) * h; ++i)
            {
                const uint8_t *p = &pixels[i * channels];
                for (int c = 0; c < 3; ++c)
                    out.rgb[i * 3 + c] = channels >= 3 ? p[c] : p[0];
            }
            return true;
        }

        class StillSource final : public FrameSource
        {
        public:
            StillSource(std::string name, std::function<void(uint32_t, uint32_t, RgbImage &)> render, bool fixedOrientation)
                : name_(std::move(name)), render_(std::move(render)), fixed_(fixedOrientation)
            {
            }
            std::string describe() const override { return name_; }
            bool live() const override { return false; }
            bool fixedOrientation() const override { return fixed_; }
            bool start(uint32_t width, uint32_t height, uint32_t) override
            {
                if (picture_.width != width || picture_.height != height)
                {
                    render_(width, height, picture_);
                    ++sequence_;
                }
                return true;
            }
            void stop() override {}
            bool latest(RgbImage &out, uint64_t &sequence) override
            {
                if (!picture_.width)
                    return false;
                out = picture_;
                sequence = sequence_;
                return true;
            }

        private:
            std::string name_;
            std::function<void(uint32_t, uint32_t, RgbImage &)> render_;
            bool fixed_;
            RgbImage picture_;
            uint64_t sequence_ = 0;
        };
    }

    uint32_t crc32(const uint8_t *data, size_t bytes)
    {
        static const std::array<uint32_t, 256> table = [] {
            std::array<uint32_t, 256> t{};
            for (uint32_t i = 0; i < 256; ++i)
            {
                uint32_t c = i;
                for (int k = 0; k < 8; ++k)
                    c = (c & 1u) ? 0xedb88320u ^ (c >> 1) : c >> 1;
                t[i] = c;
            }
            return t;
        }();
        uint32_t crc = 0xffffffffu;
        for (size_t i = 0; i < bytes; ++i)
            crc = table[(crc ^ data[i]) & 0xffu] ^ (crc >> 8);
        return crc ^ 0xffffffffu;
    }

    bool inflateRaw(const uint8_t *data, size_t bytes, size_t expectedBytes, std::vector<uint8_t> &out)
    {
        out.clear();
        out.reserve(expectedBytes);
        return Inflater(data, bytes, expectedBytes, out).run();
    }

    bool unpackVolEntry(const uint8_t *data, size_t bytes, std::vector<uint8_t> &out)
    {
        // gt4vol_real.py: compressed data = C5 EE F7 FF, int -size, raw deflate.
        if (bytes < 8 || readLe32(data) != 0xfff7eec5u)
            return false;
        const int32_t negative = static_cast<int32_t>(readLe32(data + 4));
        if (negative >= 0 || negative < -(64 << 20))
            return false;
        const size_t size = static_cast<size_t>(-static_cast<int64_t>(negative));
        return inflateRaw(data + 8, bytes - 8, size, out) && out.size() == size;
    }

    bool parseShirtTemplate(const uint8_t *data, size_t bytes, ShirtTemplate &out)
    {
        if (bytes < 16 || std::memcmp(data, "GTSH", 4))
            return false;
        const uint32_t w = readLe32(data + 4), h = readLe32(data + 8);
        if (!w || !h || w > 1024 || h > 1024 || bytes != 16 + size_t(w) * h)
            return false;
        out.width = w;
        out.height = h;
        out.samples.assign(data + 16, data + bytes);
        return true;
    }

    void renderShirtTarget(const ShirtTemplate &shirt, uint32_t width, uint32_t height, RgbImage &out)
    {
        // Colours: luma (3R+4G+B)/8 (GT4's grey, CORE 0x1c1678) of the wall and
        // the shirt stays far below the recogniser's threshold 160 and the print
        // far above it; the template's own threshold maps to sample ~160.
        constexpr uint8_t wall[3] = {124, 118, 108}, cloth[3] = {22, 24, 30}, print[3] = {236, 236, 232};
        out.width = width;
        out.height = height;
        out.rgb.resize(size_t(width) * height * 3);
        for (size_t i = 0; i < size_t(width) * height; ++i)
            std::memcpy(&out.rgb[i * 3], wall, 3);
        if (!shirt.width || !shirt.height)
            return;
        const double scale = width / 320.0;
        const int tw = static_cast<int>(std::lround(shirt.width * scale));
        const int th = static_cast<int>(std::lround(shirt.height * scale));
        const int x0 = (static_cast<int>(width) - tw) / 2, y0 = (static_cast<int>(height) - th) / 2;
        // The shirt: a torso around the print, down to the bottom of the frame.
        const int margin = static_cast<int>(std::lround(28 * scale));
        for (int y = std::max(0, y0 - margin); y < static_cast<int>(height); ++y)
            for (int x = std::max(0, x0 - margin); x < std::min(static_cast<int>(width), x0 + tw + margin); ++x)
                std::memcpy(&out.rgb[(size_t(y) * width + x) * 3], cloth, 3);
        // The print: template samples blended from cloth (0) to print (255),
        // box-filtered when scaled down, replicated when scaled up.
        for (int y = 0; y < th; ++y)
            for (int x = 0; x < tw; ++x)
            {
                const int px = x0 + x, py = y0 + y;
                if (px < 0 || py < 0 || px >= static_cast<int>(width) || py >= static_cast<int>(height))
                    continue;
                const uint32_t sx0 = static_cast<uint32_t>(x / scale), sy0 = static_cast<uint32_t>(y / scale);
                const uint32_t sx1 = std::max(sx0 + 1, std::min(shirt.width, static_cast<uint32_t>((x + 1) / scale)));
                const uint32_t sy1 = std::max(sy0 + 1, std::min(shirt.height, static_cast<uint32_t>((y + 1) / scale)));
                uint32_t sum = 0, count = 0;
                for (uint32_t sy = sy0; sy < sy1 && sy < shirt.height; ++sy)
                    for (uint32_t sx = sx0; sx < sx1 && sx < shirt.width; ++sx)
                    {
                        sum += shirt.samples[size_t(sy) * shirt.width + sx];
                        ++count;
                    }
                const int s = count ? static_cast<int>((sum + count / 2) / count) : 0;
                uint8_t *d = &out.rgb[(size_t(py) * width + px) * 3];
                for (int c = 0; c < 3; ++c)
                    d[c] = static_cast<uint8_t>(cloth[c] + ((print[c] - cloth[c]) * s + 127) / 255);
            }
    }

    bool loadImageFile(const std::string &path, RgbImage &out, std::string &error)
    {
        std::ifstream in(path, std::ios::binary);
        if (!in)
            return error = "cannot open " + path, false;
        std::vector<uint8_t> file((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        if (file.size() > (64u << 20))
            return error = "file too large", false;
        if (file.size() >= 3 && file[0] == 'P' && (file[1] == '5' || file[1] == '6'))
            return loadNetpbm(file, out, error);
        if (file.size() >= 2 && file[0] == 'B' && file[1] == 'M')
            return loadBmp(file, out, error);
        if (file.size() >= 8 && !std::memcmp(file.data(), "\x89PNG\r\n\x1a\n", 8))
            return loadPng(file, out, error);
        return error = "unknown image format (PGM/PPM, BMP or PNG)", false;
    }

    void scaleCover(const RgbImage &in, uint32_t width, uint32_t height, RgbImage &out)
    {
        out.width = width;
        out.height = height;
        out.rgb.assign(size_t(width) * height * 3, 0);
        if (!in.width || !in.height)
            return;
        // Source crop with the target aspect ratio, centred.
        double cw = in.width, ch = in.height;
        if (cw * height > ch * width)
            cw = ch * width / height;
        else
            ch = cw * height / width;
        const double cx = (in.width - cw) / 2.0, cy = (in.height - ch) / 2.0;
        const double sx = cw / width, sy = ch / height;
        for (uint32_t y = 0; y < height; ++y)
        {
            const uint32_t ya = static_cast<uint32_t>(cy + y * sy);
            const uint32_t yb = std::max(ya + 1, std::min(in.height, static_cast<uint32_t>(cy + (y + 1) * sy)));
            for (uint32_t x = 0; x < width; ++x)
            {
                const uint32_t xa = static_cast<uint32_t>(cx + x * sx);
                const uint32_t xb = std::max(xa + 1, std::min(in.width, static_cast<uint32_t>(cx + (x + 1) * sx)));
                uint32_t sum[3]{}, count = 0;
                for (uint32_t yy = ya; yy < yb && yy < in.height; ++yy)
                    for (uint32_t xx = xa; xx < xb && xx < in.width; ++xx)
                    {
                        const uint8_t *p = &in.rgb[(size_t(yy) * in.width + xx) * 3];
                        sum[0] += p[0];
                        sum[1] += p[1];
                        sum[2] += p[2];
                        ++count;
                    }
                uint8_t *d = &out.rgb[(size_t(y) * width + x) * 3];
                for (int c = 0; c < 3; ++c)
                    d[c] = static_cast<uint8_t>(count ? (sum[c] + count / 2) / count : 0);
            }
        }
    }

    void mirrorHorizontally(RgbImage &image)
    {
        for (uint32_t y = 0; y < image.height; ++y)
        {
            uint8_t *row = &image.rgb[size_t(y) * image.width * 3];
            for (uint32_t a = 0, b = image.width - 1; a < b; ++a, --b)
                for (int c = 0; c < 3; ++c)
                    std::swap(row[a * 3 + c], row[b * 3 + c]);
        }
    }

    void applyLedReflection(RgbImage &image)
    {
        // State 1 of MGTShirt (CORE 0x1c3cd8-0x1c3d84): more than 10% and less
        // than 50% of the found rectangle (the bounding box of the print, about
        // 54% white on the GT shirt) has Cr > 160 by GT4's colour difference
        // (CORE 0x1c1760: (256R - 216G - 24B) / 512 + 128, threshold_cr 160).
        // So half of the print reflects: bright neutral pixels in every other
        // band of rows (8 rows at 240 lines; whole chroma rows, so 4:2:0 keeps
        // them). The pattern is matched on grey ((3R+4G+B)/8, CORE 0x1c1678)
        // above `threshold` (gtmode.adc: 64; default 160): a reflecting pixel
        // becomes (255, x, x) with grey min(grey, 200) and Cr 177..206, so no
        // pixel crosses either grey threshold.
        const uint32_t band = std::max<uint32_t>(2, (image.height / 30u + 1u) & ~1u);
        for (uint32_t y = 0; y < image.height; ++y)
        {
            if ((y / band) & 1u)
                continue;
            for (uint32_t x = 0; x < image.width; ++x)
            {
                uint8_t *p = &image.rgb[(size_t(y) * image.width + x) * 3];
                const int grey = (3 * p[0] + 4 * p[1] + p[2]) / 8;
                const int spread = std::max({p[0], p[1], p[2]}) - std::min({p[0], p[1], p[2]});
                if (grey > 160 && spread < 64)
                {
                    const int target = std::min(grey, 200);
                    const int v = std::clamp((8 * target - 765 + 4) / 5, 0, 255);
                    p[0] = 255;
                    p[1] = p[2] = static_cast<uint8_t>(v);
                }
            }
        }
    }

    std::unique_ptr<FrameSource> createShirtSource(ShirtTemplate shirt)
    {
        return std::make_unique<StillSource>(
            "built-in GT shirt target (" + std::to_string(shirt.width) + "x" + std::to_string(shirt.height) + " template)",
            [shirt = std::move(shirt)](uint32_t w, uint32_t h, RgbImage &out) { renderShirtTarget(shirt, w, h, out); }, true);
    }

    std::unique_ptr<FrameSource> createImageSource(RgbImage image, std::string name)
    {
        return std::make_unique<StillSource>(
            "image " + name + " (" + std::to_string(image.width) + "x" + std::to_string(image.height) + ")",
            [image = std::move(image)](uint32_t w, uint32_t h, RgbImage &out) { scaleCover(image, w, h, out); }, false);
    }
}
