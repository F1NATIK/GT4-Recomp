#pragma once

// GT4 Recompilation launcher, Full edition: a small QR code encoder (HUD hand-off 8 T2: the phone's address on Driving >
// Telemetry screen), written here from the QR code standard (ISO/IEC 18004), no web call and no outside library. Byte mode,
// error correction level M, versions 1 to 10 (up to 213 bytes: an address is about 30), the mask with the lowest penalty.
// Header-only, no Windows or raylib, so the launcher's host test checks it too.

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <vector>

namespace gt4stock::qr
{
    struct Code
    {
        int size = 0;               // modules a side (21 for version 1, 4 more a version); 0: the text did not fit
        int version = 0;
        int mask = 0;
        std::vector<uint8_t> dark;  // size * size, row by row: 1 dark
        bool at(int x, int y) const { return x >= 0 && y >= 0 && x < size && y < size && dark[static_cast<size_t>(y * size + x)] != 0; }
    };

    namespace detail
    {
        // Level M, versions 1..10: error correction codewords per block, and blocks.
        inline constexpr int kEccPerBlock[11] = {0, 10, 16, 26, 18, 24, 16, 18, 22, 22, 26};
        inline constexpr int kBlocks[11] = {0, 1, 1, 1, 2, 2, 4, 4, 4, 5, 5};

        inline int rawModules(int ver)
        {
            int r = (16 * ver + 128) * ver + 64;
            if (ver >= 2)
            {
                const int n = ver / 7 + 2;
                r -= (25 * n - 10) * n - 55;
                if (ver >= 7)
                    r -= 36;
            }
            return r;
        }
        inline int dataCodewords(int ver) { return rawModules(ver) / 8 - kEccPerBlock[ver] * kBlocks[ver]; }

        inline uint8_t gfMul(uint8_t x, uint8_t y)
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
            std::vector<uint8_t> r(static_cast<size_t>(degree), 0);
            r.back() = 1;
            uint8_t root = 1;
            for (int i = 0; i < degree; ++i)
            {
                for (int j = 0; j < degree; ++j)
                {
                    r[static_cast<size_t>(j)] = gfMul(r[static_cast<size_t>(j)], root);
                    if (j + 1 < degree)
                        r[static_cast<size_t>(j)] ^= r[static_cast<size_t>(j + 1)];
                }
                root = gfMul(root, 0x02);
            }
            return r;
        }
        inline std::vector<uint8_t> rsRemainder(const std::vector<uint8_t> &data, const std::vector<uint8_t> &div)
        {
            std::vector<uint8_t> r(div.size(), 0);
            for (uint8_t b : data)
            {
                const uint8_t f = static_cast<uint8_t>(b ^ r[0]);
                r.erase(r.begin());
                r.push_back(0);
                for (size_t i = 0; i < r.size(); ++i)
                    r[i] ^= gfMul(div[i], f);
            }
            return r;
        }

        inline std::vector<int> alignment(int ver, int size)
        {
            if (ver == 1)
                return {};
            const int n = ver / 7 + 2;
            const int step = (ver * 4 + n * 2 + 1) / (n * 2 - 2) * 2;
            std::vector<int> r(static_cast<size_t>(n));
            r[0] = 6;
            for (int i = 0, pos = size - 7; i < n - 1; ++i, pos -= step)
                r[static_cast<size_t>(n - 1 - i)] = pos;
            return r;
        }

        struct Grid
        {
            int size;
            std::vector<uint8_t> m, fn;
            explicit Grid(int s) : size(s), m(static_cast<size_t>(s * s), 0), fn(static_cast<size_t>(s * s), 0) {}
            void set(int x, int y, bool dark)
            {
                m[static_cast<size_t>(y * size + x)] = dark ? 1 : 0;
                fn[static_cast<size_t>(y * size + x)] = 1;
            }
            bool isFn(int x, int y) const { return fn[static_cast<size_t>(y * size + x)] != 0; }
            uint8_t &mod(int x, int y) { return m[static_cast<size_t>(y * size + x)]; }
            uint8_t mod(int x, int y) const { return m[static_cast<size_t>(y * size + x)]; }
        };

        inline void drawFormat(Grid &g, int mask)
        {
            const int data = (0 << 3) | mask; // level M's format bits are 00
            int rem = data;
            for (int i = 0; i < 10; ++i)
                rem = (rem << 1) ^ ((rem >> 9) * 0x537);
            const int bits = ((data << 10) | rem) ^ 0x5412;
            auto bit = [&](int i) { return ((bits >> i) & 1) != 0; };
            const int s = g.size;
            for (int i = 0; i <= 5; ++i)
                g.set(8, i, bit(i));
            g.set(8, 7, bit(6));
            g.set(8, 8, bit(7));
            g.set(7, 8, bit(8));
            for (int i = 9; i < 15; ++i)
                g.set(14 - i, 8, bit(i));
            for (int i = 0; i < 8; ++i)
                g.set(s - 1 - i, 8, bit(i));
            for (int i = 8; i < 15; ++i)
                g.set(8, s - 15 + i, bit(i));
            g.set(8, s - 8, true); // the dark module
        }

        inline void drawFunctions(Grid &g, int ver)
        {
            const int s = g.size;
            for (int i = 0; i < s; ++i)
            {
                g.set(6, i, i % 2 == 0);
                g.set(i, 6, i % 2 == 0);
            }
            auto finder = [&](int cx, int cy) {
                for (int dy = -4; dy <= 4; ++dy)
                    for (int dx = -4; dx <= 4; ++dx)
                    {
                        const int d = std::max(std::abs(dx), std::abs(dy)), x = cx + dx, y = cy + dy;
                        if (x >= 0 && y >= 0 && x < s && y < s)
                            g.set(x, y, d != 2 && d != 4);
                    }
            };
            finder(3, 3);
            finder(s - 4, 3);
            finder(3, s - 4);
            const std::vector<int> al = alignment(ver, s);
            const int n = static_cast<int>(al.size());
            for (int i = 0; i < n; ++i)
                for (int j = 0; j < n; ++j)
                {
                    if ((i == 0 && j == 0) || (i == 0 && j == n - 1) || (i == n - 1 && j == 0))
                        continue;
                    for (int dy = -2; dy <= 2; ++dy)
                        for (int dx = -2; dx <= 2; ++dx)
                            g.set(al[static_cast<size_t>(i)] + dx, al[static_cast<size_t>(j)] + dy, std::max(std::abs(dx), std::abs(dy)) != 1);
                }
            drawFormat(g, 0); // reserved now, drawn again with the chosen mask
            if (ver >= 7)
            {
                int rem = ver;
                for (int i = 0; i < 12; ++i)
                    rem = (rem << 1) ^ ((rem >> 11) * 0x1F25);
                const long bits = (static_cast<long>(ver) << 12) | rem;
                for (int i = 0; i < 18; ++i)
                {
                    const bool b = ((bits >> i) & 1) != 0;
                    const int a = s - 11 + i % 3, c = i / 3;
                    g.set(a, c, b);
                    g.set(c, a, b);
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
                    if (!g.isFn(x, y) && maskBit(mask, x, y))
                        g.mod(x, y) ^= 1;
        }

        // The standard's penalty: runs of five or more, 2x2 blocks, finder-like runs, and the dark share.
        inline long penalty(const Grid &g)
        {
            const int s = g.size;
            long p = 0;
            auto line = [&](bool rows) {
                for (int a = 0; a < s; ++a)
                {
                    int run = 0, prev = -1;
                    std::vector<uint8_t> v(static_cast<size_t>(s));
                    for (int b = 0; b < s; ++b)
                    {
                        const int c = rows ? g.mod(b, a) : g.mod(a, b);
                        v[static_cast<size_t>(b)] = static_cast<uint8_t>(c);
                        if (c == prev)
                        {
                            ++run;
                            if (run == 5)
                                p += 3;
                            else if (run > 5)
                                p += 1;
                        }
                        else
                        {
                            run = 1;
                            prev = c;
                        }
                    }
                    // 1:1:3:1:1 with four light modules on one side (outside the code counts as light)
                    auto at = [&](int i) { return i < 0 || i >= s ? 0 : static_cast<int>(v[static_cast<size_t>(i)]); };
                    for (int b = -4; b < s; ++b)
                    {
                        const int core[7] = {1, 0, 1, 1, 1, 0, 1};
                        bool ok = true;
                        for (int k = 0; k < 7 && ok; ++k)
                            ok = at(b + k) == core[k];
                        if (!ok)
                            continue;
                        bool before = true, after = true;
                        for (int k = 1; k <= 4; ++k)
                        {
                            before = before && at(b - k) == 0;
                            after = after && at(b + 6 + k) == 0;
                        }
                        if (before || after)
                            p += 40;
                    }
                }
            };
            line(true);
            line(false);
            for (int y = 0; y + 1 < s; ++y)
                for (int x = 0; x + 1 < s; ++x)
                {
                    const int c = g.mod(x, y);
                    if (c == g.mod(x + 1, y) && c == g.mod(x, y + 1) && c == g.mod(x + 1, y + 1))
                        p += 3;
                }
            long dark = 0;
            for (uint8_t c : g.m)
                dark += c;
            const long total = static_cast<long>(s) * s;
            const long k = (std::abs(dark * 20 - total * 10) + total - 1) / total - 1;
            p += std::max(0L, k) * 10;
            return p;
        }
    } // namespace detail

    // The text's bytes as a QR code (empty Code when longer than version 10 holds).
    inline Code encode(const std::string &text)
    {
        using namespace detail;
        const int len = static_cast<int>(text.size());
        int ver = 1;
        for (; ver <= 10; ++ver)
        {
            const int bits = 4 + (ver < 10 ? 8 : 16) + len * 8;
            if (bits <= dataCodewords(ver) * 8)
                break;
        }
        if (ver > 10)
            return {};
        // The bit stream: byte mode, the count, the bytes, the terminator, then pad bytes.
        std::vector<int> bits;
        auto put = [&](unsigned v, int n) {
            for (int i = n - 1; i >= 0; --i)
                bits.push_back((v >> i) & 1);
        };
        put(4, 4);
        put(static_cast<unsigned>(len), ver < 10 ? 8 : 16);
        for (unsigned char c : text)
            put(c, 8);
        const int cap = dataCodewords(ver) * 8;
        put(0, std::min(4, cap - static_cast<int>(bits.size())));
        put(0, (8 - static_cast<int>(bits.size()) % 8) % 8);
        for (unsigned pad = 0xEC; static_cast<int>(bits.size()) < cap; pad ^= 0xEC ^ 0x11)
            put(pad, 8);
        std::vector<uint8_t> data(bits.size() / 8, 0);
        for (size_t i = 0; i < bits.size(); ++i)
            data[i >> 3] = static_cast<uint8_t>(data[i >> 3] | (bits[i] << (7 - (i & 7))));
        // The blocks with their error correction, interleaved.
        const int nb = kBlocks[ver], ecc = kEccPerBlock[ver], raw = rawModules(ver) / 8;
        const int nShort = nb - raw % nb, shortLen = raw / nb;
        const std::vector<uint8_t> div = rsDivisor(ecc);
        std::vector<std::vector<uint8_t>> blocks;
        for (int i = 0, k = 0; i < nb; ++i)
        {
            const int n = shortLen - ecc + (i < nShort ? 0 : 1);
            std::vector<uint8_t> d(data.begin() + k, data.begin() + k + n);
            k += n;
            const std::vector<uint8_t> e = rsRemainder(d, div);
            if (i < nShort)
                d.push_back(0);
            d.insert(d.end(), e.begin(), e.end());
            blocks.push_back(d);
        }
        std::vector<uint8_t> all;
        for (size_t i = 0; i < blocks[0].size(); ++i)
            for (int j = 0; j < nb; ++j)
                if (static_cast<int>(i) != shortLen - ecc || j >= nShort)
                    all.push_back(blocks[static_cast<size_t>(j)][i]);
        // Into the grid, two columns at a time in a zigzag from the bottom right.
        const int size = ver * 4 + 17;
        Grid g(size);
        drawFunctions(g, ver);
        size_t i = 0;
        for (int right = size - 1; right >= 1; right -= 2)
        {
            if (right == 6)
                right = 5;
            for (int v = 0; v < size; ++v)
                for (int j = 0; j < 2; ++j)
                {
                    const int x = right - j;
                    const bool up = ((right + 1) & 2) == 0;
                    const int y = up ? size - 1 - v : v;
                    if (!g.isFn(x, y) && i < all.size() * 8)
                    {
                        g.mod(x, y) = static_cast<uint8_t>((all[i >> 3] >> (7 - (i & 7))) & 1);
                        ++i;
                    }
                }
        }
        int best = 0;
        long bestP = -1;
        for (int m = 0; m < 8; ++m)
        {
            Grid t = g;
            applyMask(t, m);
            drawFormat(t, m);
            const long p = penalty(t);
            if (bestP < 0 || p < bestP)
                bestP = p, best = m;
        }
        applyMask(g, best);
        drawFormat(g, best);
        Code c;
        c.size = size;
        c.version = ver;
        c.mask = best;
        c.dark = g.m;
        return c;
    }
} // namespace gt4stock::qr
