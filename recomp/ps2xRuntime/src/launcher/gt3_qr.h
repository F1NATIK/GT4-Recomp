#pragma once

// A small QR code encoder for the launcher's Phone page (the phone's camera
// opens the dashboard's address): byte mode, error correction level M,
// versions 1-6 (up to 106 bytes; no version information needed below 7).
// Follows the QR Code standard (ISO/IEC 18004) as Project Nayuki's reference
// implementation lays it out; the mask is chosen by the standard's penalty.

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <vector>

namespace gt3qr
{
    struct Code
    {
        int size = 0;                 // modules per side (0: the text did not fit)
        std::vector<uint8_t> modules; // size * size, row by row, 1 = dark
        bool dark(int x, int y) const { return modules[static_cast<size_t>(y) * size + x] != 0; }
    };

    namespace detail
    {
        // Level M: error correction codewords per block and blocks, versions 1-6.
        constexpr int kEccPerBlock[7] = {0, 10, 16, 26, 18, 24, 16};
        constexpr int kBlocks[7] = {0, 1, 1, 1, 2, 2, 4};

        inline int rawDataModules(int ver)
        {
            int result = (16 * ver + 128) * ver + 64;
            if (ver >= 2)
            {
                const int align = ver / 7 + 2;
                result -= (25 * align - 10) * align - 55;
            }
            return result;
        }
        inline int dataCodewords(int ver) { return rawDataModules(ver) / 8 - kEccPerBlock[ver] * kBlocks[ver]; }

        inline uint8_t gfMultiply(uint8_t x, uint8_t y)
        {
            int z = 0;
            for (int i = 7; i >= 0; --i)
            {
                z = (z << 1) ^ ((z >> 7) * 0x11D);
                z ^= ((y >> i) & 1) * x;
            }
            return static_cast<uint8_t>(z);
        }
        inline std::vector<uint8_t> rsDivisor(int degree)
        {
            std::vector<uint8_t> result(static_cast<size_t>(degree), 0);
            result.back() = 1;
            uint8_t root = 1;
            for (int i = 0; i < degree; ++i)
            {
                for (size_t j = 0; j < result.size(); ++j)
                {
                    result[j] = gfMultiply(result[j], root);
                    if (j + 1 < result.size())
                        result[j] ^= result[j + 1];
                }
                root = gfMultiply(root, 0x02);
            }
            return result;
        }
        inline std::vector<uint8_t> rsRemainder(const std::vector<uint8_t> &data, const std::vector<uint8_t> &divisor)
        {
            std::vector<uint8_t> result(divisor.size(), 0);
            for (const uint8_t b : data)
            {
                const uint8_t factor = b ^ result[0];
                result.erase(result.begin());
                result.push_back(0);
                for (size_t i = 0; i < result.size(); ++i)
                    result[i] ^= gfMultiply(divisor[i], factor);
            }
            return result;
        }

        struct Grid
        {
            int size;
            std::vector<uint8_t> dark, function;
            explicit Grid(int s) : size(s), dark(static_cast<size_t>(s * s), 0), function(static_cast<size_t>(s * s), 0) {}
            void set(int x, int y, bool d)
            {
                dark[static_cast<size_t>(y) * size + x] = d ? 1 : 0;
                function[static_cast<size_t>(y) * size + x] = 1;
            }
            bool isFunction(int x, int y) const { return function[static_cast<size_t>(y) * size + x] != 0; }
            bool get(int x, int y) const { return dark[static_cast<size_t>(y) * size + x] != 0; }
        };

        inline void drawFormatBits(Grid &g, int mask)
        {
            const int data = (0 << 3) | mask; // level M: format bits 00
            int rem = data;
            for (int i = 0; i < 10; ++i)
                rem = (rem << 1) ^ ((rem >> 9) * 0x537);
            const int bits = (data << 10 | rem) ^ 0x5412;
            auto bit = [&](int i) { return ((bits >> i) & 1) != 0; };
            for (int i = 0; i <= 5; ++i)
                g.set(8, i, bit(i));
            g.set(8, 7, bit(6));
            g.set(8, 8, bit(7));
            g.set(7, 8, bit(8));
            for (int i = 9; i < 15; ++i)
                g.set(14 - i, 8, bit(i));
            for (int i = 0; i < 8; ++i)
                g.set(g.size - 1 - i, 8, bit(i));
            for (int i = 8; i < 15; ++i)
                g.set(8, g.size - 15 + i, bit(i));
            g.set(8, g.size - 8, true); // the dark module
        }

        inline void drawFunctionPatterns(Grid &g, int ver)
        {
            for (int i = 0; i < g.size; ++i)
            {
                g.set(6, i, i % 2 == 0);
                g.set(i, 6, i % 2 == 0);
            }
            auto finder = [&](int cx, int cy) {
                for (int dy = -4; dy <= 4; ++dy)
                    for (int dx = -4; dx <= 4; ++dx)
                    {
                        const int x = cx + dx, y = cy + dy, dist = std::max(std::abs(dx), std::abs(dy));
                        if (x >= 0 && x < g.size && y >= 0 && y < g.size)
                            g.set(x, y, dist != 2 && dist != 4);
                    }
            };
            finder(3, 3);
            finder(g.size - 4, 3);
            finder(3, g.size - 4);
            if (ver >= 2)
            {
                const int positions[2] = {6, g.size - 7}; // versions 2-6: one alignment pattern
                for (const int y : positions)
                    for (const int x : positions)
                    {
                        if ((x == 6 && y == 6) || (x == 6 && y == g.size - 7) || (x == g.size - 7 && y == 6))
                            continue;
                        for (int dy = -2; dy <= 2; ++dy)
                            for (int dx = -2; dx <= 2; ++dx)
                                g.set(x + dx, y + dy, std::max(std::abs(dx), std::abs(dy)) != 1);
                    }
            }
            drawFormatBits(g, 0); // placeholders (marks the format areas as function modules)
        }

        inline void drawCodewords(Grid &g, const std::vector<uint8_t> &data)
        {
            size_t i = 0;
            for (int right = g.size - 1; right >= 1; right -= 2)
            {
                if (right == 6)
                    right = 5;
                for (int vert = 0; vert < g.size; ++vert)
                    for (int j = 0; j < 2; ++j)
                    {
                        const int x = right - j;
                        const bool upward = ((right + 1) & 2) == 0;
                        const int y = upward ? g.size - 1 - vert : vert;
                        if (!g.isFunction(x, y) && i < data.size() * 8)
                        {
                            g.dark[static_cast<size_t>(y) * g.size + x] = ((data[i >> 3] >> (7 - (i & 7))) & 1) != 0;
                            ++i;
                        }
                    }
            }
        }

        inline bool maskBit(int mask, int x, int y)
        {
            switch (mask)
            {
            case 0: return (x + y) % 2 == 0;
            case 1: return y % 2 == 0;
            case 2: return x % 3 == 0;
            case 3: return (x + y) % 3 == 0;
            case 4: return (x / 3 + y / 2) % 2 == 0;
            case 5: return x * y % 2 + x * y % 3 == 0;
            case 6: return (x * y % 2 + x * y % 3) % 2 == 0;
            default: return ((x + y) % 2 + x * y % 3) % 2 == 0;
            }
        }
        inline void applyMask(Grid &g, int mask)
        {
            for (int y = 0; y < g.size; ++y)
                for (int x = 0; x < g.size; ++x)
                    if (!g.isFunction(x, y) && maskBit(mask, x, y))
                        g.dark[static_cast<size_t>(y) * g.size + x] ^= 1;
        }

        // The standard's penalty (lower is better) for choosing the mask.
        inline long penalty(const Grid &g)
        {
            long result = 0;
            const int n = g.size;
            auto lineRuns = [&](bool rows) {
                for (int a = 0; a < n; ++a)
                {
                    int run = 1;
                    std::vector<int> pattern; // finder-like 1:1:3:1:1 runs with light space
                    for (int b = 1; b <= n; ++b)
                    {
                        const bool same = b < n && (rows ? g.get(b, a) == g.get(b - 1, a) : g.get(a, b) == g.get(a, b - 1));
                        if (same)
                        {
                            ++run;
                            continue;
                        }
                        if (run >= 5)
                            result += 3 + (run - 5);
                        run = 1;
                    }
                    for (int b = 0; b + 10 < n; ++b)
                    {
                        // 1011101 with four light modules on either side.
                        auto at = [&](int k) { return rows ? g.get(b + k, a) : g.get(a, b + k); };
                        const bool core = at(4) && !at(5) && at(6) && at(7) && at(8) && !at(9) && at(10);
                        const bool coreLeft = at(0) && !at(1) && at(2) && at(3) && at(4) && !at(5) && at(6);
                        if (core && !at(0) && !at(1) && !at(2) && !at(3))
                            result += 40;
                        if (coreLeft && !at(7) && !at(8) && !at(9) && !at(10))
                            result += 40;
                    }
                }
            };
            lineRuns(true);
            lineRuns(false);
            for (int y = 0; y + 1 < n; ++y)
                for (int x = 0; x + 1 < n; ++x)
                {
                    const bool c = g.get(x, y);
                    if (c == g.get(x + 1, y) && c == g.get(x, y + 1) && c == g.get(x + 1, y + 1))
                        result += 3;
                }
            int darkCount = 0;
            for (const uint8_t d : g.dark)
                darkCount += d;
            const int total = n * n;
            const int k = (std::abs(darkCount * 20 - total * 10) + total - 1) / total - 1;
            result += std::max(0, k) * 10;
            return result;
        }
    }

    // `text` as a QR code (empty Code if it does not fit in version 6).
    inline Code encode(const std::string &text)
    {
        using namespace detail;
        int ver = 1;
        for (; ver <= 6; ++ver)
            if (4 + 8 + static_cast<int>(text.size()) * 8 <= dataCodewords(ver) * 8)
                break;
        if (ver > 6)
            return {};
        // Byte mode, 8-bit count, data, terminator, padding.
        std::vector<int> bits;
        auto put = [&](uint32_t value, int count) {
            for (int i = count - 1; i >= 0; --i)
                bits.push_back(static_cast<int>((value >> i) & 1));
        };
        put(0x4, 4);
        put(static_cast<uint32_t>(text.size()), 8);
        for (const char c : text)
            put(static_cast<uint8_t>(c), 8);
        const int capacity = dataCodewords(ver) * 8;
        put(0, std::min(4, capacity - static_cast<int>(bits.size())));
        put(0, (8 - static_cast<int>(bits.size()) % 8) % 8);
        for (uint8_t pad = 0xEC; static_cast<int>(bits.size()) < capacity; pad ^= 0xEC ^ 0x11)
            put(pad, 8);
        std::vector<uint8_t> data(bits.size() / 8, 0);
        for (size_t i = 0; i < bits.size(); ++i)
            data[i >> 3] |= static_cast<uint8_t>(bits[i] << (7 - (i & 7)));
        // Error correction per block, interleaved.
        const int numBlocks = kBlocks[ver], eccLen = kEccPerBlock[ver];
        const int raw = rawDataModules(ver) / 8;
        const int numShort = numBlocks - raw % numBlocks, shortLen = raw / numBlocks;
        const std::vector<uint8_t> divisor = rsDivisor(eccLen);
        std::vector<std::vector<uint8_t>> blocks;
        for (int i = 0, k = 0; i < numBlocks; ++i)
        {
            const int len = shortLen - eccLen + (i < numShort ? 0 : 1);
            std::vector<uint8_t> dat(data.begin() + k, data.begin() + k + len);
            k += len;
            const std::vector<uint8_t> ecc = rsRemainder(dat, divisor);
            if (i < numShort)
                dat.push_back(0);
            dat.insert(dat.end(), ecc.begin(), ecc.end());
            blocks.push_back(dat);
        }
        std::vector<uint8_t> codewords;
        for (size_t i = 0; i < blocks[0].size(); ++i)
            for (int j = 0; j < numBlocks; ++j)
                if (i != static_cast<size_t>(shortLen - eccLen) || j >= numShort)
                    codewords.push_back(blocks[static_cast<size_t>(j)][i]);
        // Modules, then the mask with the lowest penalty.
        Grid base(ver * 4 + 17);
        drawFunctionPatterns(base, ver);
        drawCodewords(base, codewords);
        Grid best = base;
        long bestPenalty = -1;
        for (int mask = 0; mask < 8; ++mask)
        {
            Grid g = base;
            applyMask(g, mask);
            drawFormatBits(g, mask);
            const long p = penalty(g);
            if (bestPenalty < 0 || p < bestPenalty)
            {
                bestPenalty = p;
                best = g;
            }
        }
        Code code;
        code.size = best.size;
        code.modules = best.dark;
        return code;
    }
}
