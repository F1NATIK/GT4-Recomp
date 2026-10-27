#pragma once

// gt4save_crypto.h - GT4 (US, SCUS-97328) save-card crypto primitives (internal to gt4save).
//
// A line-for-line port of analysis/saves/save-crypto/gt4save_crypto.py, which mirrors GT4's own
// EE code (CORE.GT4 addresses in brackets; evidence in GT4-SAVE-FORMAT.md):
//   MT19937 [0x579420/0x579438 seed, 0x5794c0 next, 0x5795e0 twist], LCG17 [0x5792e8/0x579330],
//   CRC-32 [0x572c90], word cipher [0x5d2680 enc / 0x5d2750 dec], word swap [0x1d4ca8],
//   Fisher-Yates over bits or bytes [0x5d2818 forward / 0x5d2900 inverse] with the EE's
//   single-precision position maths modelled exactly in integers (round toward zero).
// Header-only so the read path (gt4save.cpp) and the test-only re-encrypt path
// (gt4save_roundtrip.cpp) share one copy. No game data lives here: only the algorithm constants.

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

#include "gt4save.h"

namespace gt4save::crypto
{
    // ------------------------------------------------------------------ little-endian access
    inline uint32_t rd32(const uint8_t *p) { return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24); }
    inline void wr32(uint8_t *p, uint32_t v)
    {
        p[0] = uint8_t(v);
        p[1] = uint8_t(v >> 8);
        p[2] = uint8_t(v >> 16);
        p[3] = uint8_t(v >> 24);
    }

    // ------------------------------------------------------------------ CRC-32 (0x572c90 == zlib.crc32)
    inline uint32_t crc32(const uint8_t *data, size_t n, uint32_t crc = 0)
    {
        static const std::array<uint32_t, 256> table = [] {
            std::array<uint32_t, 256> t{};
            for (uint32_t i = 0; i < 256; ++i)
            {
                uint32_t c = i;
                for (int k = 0; k < 8; ++k)
                    c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
                t[i] = c;
            }
            return t;
        }();
        crc = ~crc;
        for (size_t i = 0; i < n; ++i)
            crc = table[(crc ^ data[i]) & 0xFF] ^ (crc >> 8);
        return ~crc;
    }

    // ------------------------------------------------------------------ PRNGs
    // MT19937 as GT4 uses it: init_genrand(seed), standard twist and tempering.
    struct MT
    {
        uint32_t mt[624];
        int idx = 624;
        explicit MT(uint32_t seed)
        {
            mt[0] = seed;
            for (uint32_t i = 1; i < 624; ++i)
                mt[i] = 1812433253u * (mt[i - 1] ^ (mt[i - 1] >> 30)) + i;
        }
        void twist()
        {
            for (int kk = 0; kk < 624; ++kk)
            {
                const uint32_t y = (mt[kk] & 0x80000000u) | (mt[(kk + 1) % 624] & 0x7FFFFFFFu);
                mt[kk] = mt[(kk + 397) % 624] ^ (y >> 1) ^ ((y & 1) ? 0x9908B0DFu : 0u);
            }
        }
        uint32_t next()
        {
            if (idx >= 624)
            {
                twist();
                idx = 0;
            }
            uint32_t y = mt[idx++];
            y ^= y >> 11;
            y ^= (y << 7) & 0x9D2C5680u;
            y ^= (y << 15) & 0xEFC60000u;
            y ^= y >> 18;
            return y;
        }
    };

    // 0x5792e8 seed (s = seed) / 0x579330 next: s = s*17 + 17; returns s ^ rotl(s, 16).
    struct LCG17
    {
        uint32_t s;
        explicit LCG17(uint32_t seed) : s(seed) {}
        uint32_t next()
        {
            s = s * 17u + 17u;
            return s ^ ((s << 16) | (s >> 16));
        }
    };

    // ------------------------------------------------------------------ EE single-precision position maths
    // Round a non-negative integer to 24 significant bits (a float32 mantissa).
    inline uint64_t round24(uint64_t x, Rounding mode)
    {
        int n = 0;
        for (uint64_t t = x; t; t >>= 1)
            ++n;
        if (n <= 24)
            return x;
        const int sh = n - 24;
        uint64_t q = x >> sh;
        if (mode == Rounding::Nearest)
        {
            const uint64_t rem = x & ((uint64_t(1) << sh) - 1);
            const uint64_t half = uint64_t(1) << (sh - 1);
            if (rem > half || (rem == half && (q & 1)))
                ++q;
        }
        return q << sh;
    }

    // (int)(mt_float * (float)n), mt_float = (float)v * 2^-32 (0x579548); n in 1 .. 2^24.
    inline uint32_t eeRandIndex(uint32_t v, uint32_t n, Rounding mode)
    {
        return uint32_t(round24(round24(v, mode) * n, mode) >> 32);
    }

    // (int)(frac * (float)n) for a float32 constant frac in (0,1) (0x1d4ca8).
    inline uint64_t eeFracIndex(uint32_t fracBits, uint64_t n, Rounding mode)
    {
        const uint64_t m = (fracBits & 0x7FFFFFu) | 0x800000u;
        const int e = int((fracBits >> 23) & 0xFF) - 150; // negative for frac < 1
        return round24(m * round24(n, mode), mode) >> (-e);
    }

    // Fisher-Yates draws of 0x5d2818: for i = n-1 .. 1, j = rand_index(i+1); swap(i, j).
    // js[k] is the partner of i = n-1-k (draw order). The inverse (0x5d2900) applies the same
    // swaps in the opposite order.
    inline void shufflePairs(uint32_t n, uint32_t seed, Rounding mode, std::vector<uint32_t> &js)
    {
        js.clear();
        if (n < 2)
            return;
        js.resize(n - 1);
        MT mt(seed);
        for (uint32_t k = 0; k + 1 < n; ++k)
        {
            const uint32_t i = n - 1 - k;
            js[k] = eeRandIndex(mt.next(), i + 1, mode);
        }
    }

    // j < n always holds with round-toward-zero; round-to-nearest can produce j == i + 1 == n for
    // an MT output >= 0xFFFFFF80 (the Python model raises there), so such a swap is skipped.
    template <class T>
    inline void applyPairs(T *seq, uint32_t n, const std::vector<uint32_t> &js, bool inverse)
    {
        if (n < 2)
            return;
        if (!inverse)
        {
            for (uint32_t k = 0; k + 1 < n; ++k)
            {
                const uint32_t i = n - 1 - k, j = js[k];
                if (j < n)
                    std::swap(seq[i], seq[j]);
            }
        }
        else
        {
            for (uint32_t k = n - 1; k-- > 0;)
            {
                const uint32_t i = n - 1 - k, j = js[k];
                if (j < n)
                    std::swap(seq[i], seq[j]);
            }
        }
    }

    // 0x1d4b78 (forward) / 0x1d4ba0 (inverse) over len*8 bits; bit k = byte k>>3, bit k&7 (LSB first).
    inline void bitShuffle(uint8_t *data, size_t len, uint32_t seed, Rounding mode, bool inverse)
    {
        const uint32_t n = uint32_t(len * 8);
        std::vector<uint8_t> bits(n);
        for (uint32_t k = 0; k < n; ++k)
            bits[k] = (data[k >> 3] >> (k & 7)) & 1;
        std::vector<uint32_t> js;
        shufflePairs(n, seed, mode, js);
        applyPairs(bits.data(), n, js, inverse);
        std::memset(data, 0, len);
        for (uint32_t k = 0; k < n; ++k)
            if (bits[k])
                data[k >> 3] |= uint8_t(1u << (k & 7));
    }

    // 0x1d4bc8 / 0x1d4c18 over n BYTES (callback 0x1d4b58). Not used by GAMEDATA or the garage.
    inline void byteShuffle(uint8_t *data, uint32_t n, uint32_t seed, Rounding mode, bool inverse)
    {
        std::vector<uint32_t> js;
        shufflePairs(n, seed, mode, js);
        applyPairs(data, n, js, inverse);
    }

    // 0x1d4ca8(base, length): swap the 4 bytes at base with the 4 at base + (int)(frac * (length - 4)).
    // Self-inverse. Returns false (and changes nothing) when the partner lies outside buf.
    inline bool wordSwap(uint8_t *buf, size_t bufLen, size_t base, uint64_t length, uint32_t fracBits, Rounding mode, uint64_t &t)
    {
        if (length < 4)
            return false;
        t = eeFracIndex(fracBits, length - 4, mode);
        if (base + 4 > bufLen || base + t + 4 > bufLen)
            return false;
        for (int i = 0; i < 4; ++i)
            std::swap(buf[base + i], buf[base + t + i]);
        return true;
    }

    // 0x5d2680: LE words w = (w ^ mt) - lcg; a trailing <4 bytes the same per byte.
    inline void wordCipherEnc(uint8_t *p, size_t length, MT &mt, LCG17 &lcg)
    {
        const size_t nw = length / 4;
        for (size_t k = 0; k < nw; ++k)
        {
            const uint32_t r = mt.next(), s = lcg.next();
            wr32(p + 4 * k, (rd32(p + 4 * k) ^ r) - s);
        }
        for (size_t q = 4 * nw; q < length; ++q)
        {
            const uint32_t r = mt.next(), s = lcg.next();
            p[q] = uint8_t((p[q] ^ r) - s);
        }
    }

    // 0x5d2750: w = (w + lcg) ^ mt (MT drawn first, then LCG17, as on encrypt).
    inline void wordCipherDec(uint8_t *p, size_t length, MT &mt, LCG17 &lcg)
    {
        const size_t nw = length / 4;
        for (size_t k = 0; k < nw; ++k)
        {
            const uint32_t r = mt.next(), s = lcg.next();
            wr32(p + 4 * k, (rd32(p + 4 * k) + s) ^ r);
        }
        for (size_t q = 4 * nw; q < length; ++q)
        {
            const uint32_t r = mt.next(), s = lcg.next();
            p[q] = uint8_t(uint8_t(p[q] + s) ^ r);
        }
    }

    // ------------------------------------------------------------------ the GT4MC file envelope (0x1d4d10 / 0x1d4e60)
    struct EnvelopeKind
    {
        uint32_t fracA, fracB, key;
        bool byteShuffle;
    };
    // GAMEDATA: 0x1ce020 save / 0x1cdac8 load (verified on real files). The other GT4 card files
    // (thumbnail, replay, film, photo) use the same envelope with other constants; not needed here.
    constexpr EnvelopeKind kGamedataEnvelope{0x3E0B0C88u, 0x3F27A4E7u, 0u, false};

    struct EnvelopeResult
    {
        bool ok = false; // false: the file's size does not allow the envelope (not a GT4 file)
        uint32_t seed = 0, crcStored = 0, crc = 0;
        bool crcOk = false;
        uint64_t swapA = 0, swapB = 0;
        std::vector<uint8_t> payload;
    };

    // Undo 0x1d4d10 the way 0x1d4e60 does. raw is not modified.
    inline EnvelopeResult envDecrypt(const uint8_t *raw, size_t size, const EnvelopeKind &kind, Rounding mode)
    {
        EnvelopeResult r;
        if (size < 8)
            return r;
        std::vector<uint8_t> buf(raw, raw + size);
        const uint64_t padded = (uint64_t(size) + 63) & ~uint64_t(63);
        if (!wordSwap(buf.data(), size, 0, size, kind.fracB, mode, r.swapB))
            return r;
        if (kind.byteShuffle)
        {
            if (padded - 4 > size - 4)
                return r; // GT4 shuffles the padded length; only whole files can be undone here
            byteShuffle(buf.data() + 4, uint32_t(padded - 4), rd32(buf.data()) ^ kind.key, mode, true);
        }
        if (!wordSwap(buf.data(), size, 4, padded - 4, kind.fracA, mode, r.swapA))
            return r;
        r.seed = rd32(buf.data());
        const uint32_t w1 = rd32(buf.data() + 4);
        MT mt(w1 + r.seed);
        LCG17 lcg(w1 ^ r.seed);
        wordCipherDec(buf.data() + 8, size - 8, mt, lcg);
        r.crc = crc32(buf.data() + 8, size - 8);
        r.crcStored = w1 ^ kind.key;
        r.crcOk = r.crc == r.crcStored;
        r.payload.assign(buf.begin() + 8, buf.end());
        r.ok = true;
        return r;
    }

    // ------------------------------------------------------------------ garage file (keyed by the GAMEDATA garage key)
    constexpr uint32_t kSerialCarBytes = 1216; // 1208-byte car + 8 zero bytes (0x43ded8 -> align16)

    // 0x1cb9b8: undo 0x1cb900 with the GAMEDATA garage key.
    inline std::array<uint8_t, 64> garageHeaderDecrypt(const uint8_t *raw64, uint32_t key, Rounding mode)
    {
        std::array<uint8_t, 64> h{};
        std::memcpy(h.data(), raw64, 64);
        LCG17 lcg(key);
        const uint32_t x3c = lcg.next();
        bitShuffle(h.data(), 64, lcg.next(), mode, true);
        const uint32_t v = rd32(h.data() + 0x3C) ^ x3c;
        wr32(h.data() + 0x3C, v);
        MT mt(key + v);
        for (int i = 0; i < 60; ++i)
            h[i] ^= uint8_t(mt.next() & 0xFF);
        return h;
    }

    // 0x1cb900 (key = plain word 0x38). Deterministic. The read path uses it to recognise a header
    // GT4 meant for offset 0 that the recomp's append bug put past the end of the file.
    inline std::array<uint8_t, 64> garageHeaderEncrypt(const uint8_t *plain64, Rounding mode)
    {
        std::array<uint8_t, 64> h{};
        std::memcpy(h.data(), plain64, 64);
        const uint32_t key = rd32(h.data() + 0x38), x3c = rd32(h.data() + 0x3C);
        MT mt(key + x3c);
        for (int i = 0; i < 60; ++i)
            h[i] ^= uint8_t(mt.next() & 0xFF);
        LCG17 lcg(key);
        wr32(h.data() + 0x3C, x3c ^ lcg.next());
        bitShuffle(h.data(), 64, lcg.next(), mode, false);
        return h;
    }

    struct SlotDecrypt
    {
        uint32_t seed = 0, crcStored = 0, crc = 0;
        bool crcOk = false;
        std::vector<uint8_t> plain; // 1,272 bytes: the 1,216 serialised car bytes + zero padding
    };

    // 0x1ceec0: inverse bit shuffle with MT(key + slot), then the word cipher with MT(key ^ w0),
    // LCG17(w1); w1 must equal the CRC-32 of the 1,216 serialised car bytes.
    inline SlotDecrypt garageSlotDecrypt(const uint8_t *raw, size_t len, uint32_t key, uint32_t slot, Rounding mode)
    {
        SlotDecrypt d;
        std::vector<uint8_t> b(raw, raw + len);
        bitShuffle(b.data(), len, key + slot, mode, true);
        d.seed = rd32(b.data());
        d.crcStored = rd32(b.data() + 4);
        MT mt(key ^ d.seed);
        LCG17 lcg(d.crcStored);
        wordCipherDec(b.data() + 8, len - 8, mt, lcg);
        d.crc = crc32(b.data() + 8, kSerialCarBytes);
        d.crcOk = d.crc == d.crcStored;
        d.plain.assign(b.begin() + 8, b.end());
        return d;
    }

    // 0x1ce630 fill of a new garage file: byte i (i >= 64) = (lcg() ^ mt()) & 0xff, MT and LCG17
    // both seeded with the garage key. Bytes 0..63 (the header) are left zero here.
    inline std::vector<uint8_t> garageNoise(uint32_t key, size_t length)
    {
        std::vector<uint8_t> out(length, 0);
        MT mt(key);
        LCG17 lcg(key);
        for (size_t i = 64; i < length; ++i)
        {
            const uint32_t r = mt.next();
            out[i] = uint8_t((lcg.next() ^ r) & 0xFF);
        }
        return out;
    }
}
