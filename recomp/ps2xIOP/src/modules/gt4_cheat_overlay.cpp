#include "gt4_cheat_overlay.h"
#include "gt4_iop.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <queue>

// GT4_CHEATS: GT4's menu scripts patched as they are read from the disc (gt4_cheat_overlay.h).
// GT4 builds only (src/modules/gt4_*.cpp). Design: analysis/cheats/CHEATS-TAB-DESIGN.md 4.3, 5.2;
// facts: analysis/cheats/GT4-CHEATS.md 5.3.

namespace ps2x::iop::gt4::cheats
{
    namespace
    {
        // ---------------------------------------------------------------- bits
        struct BitWriter
        {
            std::vector<uint8_t> out;
            uint64_t acc = 0;
            int n = 0;
            void put(uint32_t value, int bits)
            {
                acc |= uint64_t(value) << n;
                n += bits;
                while (n >= 8)
                {
                    out.push_back(uint8_t(acc));
                    acc >>= 8;
                    n -= 8;
                }
            }
            void align()
            {
                if (n)
                    out.push_back(uint8_t(acc));
                acc = 0;
                n = 0;
            }
            uint64_t bits() const { return uint64_t(out.size()) * 8u + uint64_t(n); }
        };

        uint32_t reversed(uint32_t code, int length)
        {
            uint32_t r = 0;
            for (int i = 0; i < length; ++i)
            {
                r = (r << 1) | (code & 1u);
                code >>= 1;
            }
            return r;
        }

        // ---------------------------------------------------------------- deflate tables (RFC 1951 3.2.5)
        constexpr uint16_t kLenBase[29] = {3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
        constexpr uint8_t kLenExtra[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
        constexpr uint16_t kDistBase[30] = {1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145,
                                            8193, 12289, 16385, 24577};
        constexpr uint8_t kDistExtra[30] = {0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};
        constexpr uint8_t kClOrder[19] = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};

        int lengthIndex(int length)
        {
            int i = 28;
            while (kLenBase[i] > length)
                --i;
            return i;
        }
        int distIndex(int distance)
        {
            int i = 29;
            while (kDistBase[i] > distance)
                --i;
            return i;
        }

        // ---------------------------------------------------------------- Huffman code lengths
        // Optimal lengths; over `limit` the counts are halved (rounded up, so none drops to zero)
        // until the tree fits. At least two codes get a length (zlib does the same: a lone code
        // still costs one bit).
        std::vector<uint8_t> codeLengths(std::vector<uint32_t> freq, int limit)
        {
            int used = 0;
            for (uint32_t f : freq)
                used += f ? 1 : 0;
            for (size_t i = 0; used < 2 && i < freq.size(); ++i)
                if (!freq[i])
                {
                    freq[i] = 1;
                    ++used;
                }
            std::vector<uint8_t> lengths(freq.size(), 0);
            for (;;)
            {
                struct Node
                {
                    uint64_t weight;
                    int index; // < freq.size(): a leaf
                };
                std::vector<int> parent(freq.size() * 2, -1);
                auto cmp = [](const Node &a, const Node &b) { return a.weight != b.weight ? a.weight > b.weight : a.index > b.index; };
                std::priority_queue<Node, std::vector<Node>, decltype(cmp)> heap(cmp);
                for (size_t i = 0; i < freq.size(); ++i)
                    if (freq[i])
                        heap.push(Node{freq[i], int(i)});
                int next = int(freq.size());
                while (heap.size() > 1)
                {
                    const Node a = heap.top();
                    heap.pop();
                    const Node b = heap.top();
                    heap.pop();
                    parent[size_t(a.index)] = next;
                    parent[size_t(b.index)] = next;
                    heap.push(Node{a.weight + b.weight, next});
                    ++next;
                }
                int longest = 0;
                for (size_t i = 0; i < freq.size(); ++i)
                {
                    lengths[i] = 0;
                    if (!freq[i])
                        continue;
                    int depth = 0;
                    for (int p = parent[i]; p >= 0; p = parent[size_t(p)])
                        ++depth;
                    lengths[i] = uint8_t(depth);
                    longest = std::max(longest, depth);
                }
                if (longest <= limit)
                    return lengths;
                for (uint32_t &f : freq)
                    if (f)
                        f = (f + 1u) / 2u;
            }
        }

        // Canonical codes (RFC 1951 3.2.2), already bit-reversed for the LSB-first writer.
        std::vector<uint32_t> canonical(const std::vector<uint8_t> &lengths)
        {
            uint32_t count[16] = {};
            for (uint8_t l : lengths)
                ++count[l];
            count[0] = 0;
            uint32_t next[16] = {};
            uint32_t code = 0;
            for (int bits = 1; bits < 16; ++bits)
            {
                code = (code + count[bits - 1]) << 1;
                next[bits] = code;
            }
            std::vector<uint32_t> codes(lengths.size(), 0);
            for (size_t i = 0; i < lengths.size(); ++i)
                if (lengths[i])
                    codes[i] = reversed(next[lengths[i]]++, lengths[i]);
            return codes;
        }

        struct Symbol
        {
            uint16_t litlen; // a byte, or a match length 3..258
            uint16_t dist;   // 0: literal
        };

        // zlib's scan_tree / send_tree run-length rules for one tree's lengths.
        struct ClItem
        {
            uint8_t symbol;
            uint8_t extra;
        };
        void runLengths(const std::vector<uint8_t> &lengths, size_t count, std::vector<ClItem> &out)
        {
            int prev = -1;
            int nextLen = count ? lengths[0] : 0;
            int run = 0, maxRun = nextLen ? 7 : 138, minRun = nextLen ? 4 : 3;
            for (size_t n = 0; n < count; ++n)
            {
                const int cur = nextLen;
                nextLen = n + 1 < count ? lengths[n + 1] : 0xffff;
                if (++run < maxRun && cur == nextLen)
                    continue;
                if (run < minRun)
                {
                    for (int i = 0; i < run; ++i)
                        out.push_back({uint8_t(cur), 0});
                }
                else if (cur != 0)
                {
                    if (cur != prev)
                    {
                        out.push_back({uint8_t(cur), 0});
                        --run;
                    }
                    out.push_back({16, uint8_t(run - 3)});
                }
                else if (run <= 10)
                    out.push_back({17, uint8_t(run - 3)});
                else
                    out.push_back({18, uint8_t(run - 11)});
                run = 0;
                prev = cur;
                if (nextLen == 0)
                {
                    maxRun = 138;
                    minRun = 3;
                }
                else if (cur == nextLen)
                {
                    maxRun = 6;
                    minRun = 3;
                }
                else
                {
                    maxRun = 7;
                    minRun = 4;
                }
            }
        }

        constexpr uint8_t kClExtraBits[19] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 2, 3, 7};

        // One block of symbols (and the raw bytes it covers) as the smallest of dynamic, fixed and stored.
        void writeBlock(BitWriter &w, const std::vector<Symbol> &symbols, std::span<const uint8_t> raw)
        {
            std::vector<uint32_t> litFreq(286, 0), distFreq(30, 0);
            for (const Symbol &s : symbols)
            {
                if (!s.dist)
                    ++litFreq[s.litlen];
                else
                {
                    ++litFreq[size_t(257 + lengthIndex(s.litlen))];
                    ++distFreq[size_t(distIndex(s.dist))];
                }
            }
            litFreq[256] = 1;
            const std::vector<uint8_t> litLen = codeLengths(litFreq, 15);
            const std::vector<uint8_t> distLen = codeLengths(distFreq, 15);
            size_t hlit = 286, hdist = 30;
            while (hlit > 257 && !litLen[hlit - 1])
                --hlit;
            while (hdist > 1 && !distLen[hdist - 1])
                --hdist;
            std::vector<ClItem> cl;
            runLengths(litLen, hlit, cl);
            runLengths(distLen, hdist, cl);
            std::vector<uint32_t> clFreq(19, 0);
            for (const ClItem &c : cl)
                ++clFreq[c.symbol];
            const std::vector<uint8_t> clLen = codeLengths(clFreq, 7);
            size_t hclen = 19;
            while (hclen > 4 && !clLen[kClOrder[hclen - 1]])
                --hclen;

            // Sizes in bits (header included).
            uint64_t extra = 0;
            for (const Symbol &s : symbols)
                if (s.dist)
                    extra += kLenExtra[lengthIndex(s.litlen)] + kDistExtra[distIndex(s.dist)];
            uint64_t dynamicBits = 3 + 5 + 5 + 4 + 3 * hclen + extra;
            for (const ClItem &c : cl)
                dynamicBits += clLen[c.symbol] + kClExtraBits[c.symbol];
            uint64_t fixedBits = 3 + extra;
            std::vector<uint8_t> fixedLit(288), fixedDist(30, 5);
            for (int i = 0; i < 288; ++i)
                fixedLit[size_t(i)] = i < 144 ? 8 : i < 256 ? 9 : i < 280 ? 7 : 8;
            for (size_t i = 0; i < 286; ++i)
            {
                dynamicBits += uint64_t(litFreq[i]) * litLen[i];
                fixedBits += uint64_t(litFreq[i]) * fixedLit[i];
            }
            for (size_t i = 0; i < 30; ++i)
            {
                dynamicBits += uint64_t(distFreq[i]) * distLen[i];
                fixedBits += uint64_t(distFreq[i]) * 5u;
            }
            const uint64_t pad = (8u - ((w.bits() + 3u) & 7u)) & 7u;
            const uint64_t storedBits = raw.size() <= 0xffffu ? 3u + pad + 32u + 8u * raw.size() : UINT64_MAX;

            if (storedBits <= dynamicBits && storedBits <= fixedBits)
            {
                w.put(0, 1);
                w.put(0, 2);
                w.align();
                w.put(uint32_t(raw.size()), 16);
                w.put(uint32_t(~raw.size()) & 0xffffu, 16);
                for (uint8_t b : raw)
                    w.put(b, 8);
                return;
            }
            const bool fixed = fixedBits <= dynamicBits;
            const std::vector<uint8_t> &ll = fixed ? fixedLit : litLen;
            const std::vector<uint8_t> &dl = fixed ? fixedDist : distLen;
            w.put(0, 1);
            w.put(fixed ? 1u : 2u, 2);
            if (!fixed)
            {
                const std::vector<uint32_t> clCodes = canonical(clLen);
                w.put(uint32_t(hlit - 257), 5);
                w.put(uint32_t(hdist - 1), 5);
                w.put(uint32_t(hclen - 4), 4);
                for (size_t i = 0; i < hclen; ++i)
                    w.put(clLen[kClOrder[i]], 3);
                for (const ClItem &c : cl)
                {
                    w.put(clCodes[c.symbol], clLen[c.symbol]);
                    if (kClExtraBits[c.symbol])
                        w.put(c.extra, kClExtraBits[c.symbol]);
                }
            }
            const std::vector<uint32_t> lc = canonical(ll), dc = canonical(dl);
            for (const Symbol &s : symbols)
            {
                if (!s.dist)
                {
                    w.put(lc[s.litlen], ll[s.litlen]);
                    continue;
                }
                const int li = lengthIndex(s.litlen), di = distIndex(s.dist);
                w.put(lc[size_t(257 + li)], ll[size_t(257 + li)]);
                if (kLenExtra[li])
                    w.put(uint32_t(s.litlen - kLenBase[li]), kLenExtra[li]);
                w.put(dc[size_t(di)], dl[size_t(di)]);
                if (kDistExtra[di])
                    w.put(uint32_t(s.dist - kDistBase[di]), kDistExtra[di]);
            }
            w.put(lc[256], ll[256]);
        }

        // ---------------------------------------------------------------- inflate (strict)
        // Modelled on Mark Adler's puff.c (zlib's contrib/puff, zlib licence; Copyright (C) 2002-2013
        // Mark Adler): construct() and decode() follow its canonical-Huffman tables and bit-by-bit decode.
        // Stricter than puff on what this needs (output limit, the end of the final block reported back).
        // ps2_iop's eyetoy::inflateRaw (gt4_eyetoy_source.cpp) decodes the same GT4.VOL streams but does
        // not report where the final block ends, which the size check of the rebuilt stream needs.
        struct InState
        {
            std::span<const uint8_t> in;
            size_t pos = 0;
            uint32_t bitBuf = 0;
            int bitCount = 0;
            std::vector<uint8_t> *out = nullptr;
            size_t limit = 0;
            bool bits(int need, uint32_t &value)
            {
                uint64_t v = bitBuf;
                while (bitCount < need)
                {
                    if (pos >= in.size())
                        return false;
                    v |= uint64_t(in[pos++]) << bitCount;
                    bitCount += 8;
                }
                value = uint32_t(v & ((uint64_t(1) << need) - 1u));
                bitBuf = uint32_t(v >> need);
                bitCount -= need;
                return true;
            }
        };
        struct Huff
        {
            uint16_t count[16] = {};
            std::vector<uint16_t> symbol;
        };
        // > 0 incomplete, 0 complete, < 0 over-subscribed.
        int construct(Huff &h, const uint8_t *length, int n)
        {
            std::fill(std::begin(h.count), std::end(h.count), uint16_t(0));
            for (int s = 0; s < n; ++s)
                ++h.count[length[s]];
            if (h.count[0] == n)
                return 0;
            int left = 1;
            for (int len = 1; len < 16; ++len)
            {
                left <<= 1;
                left -= h.count[len];
                if (left < 0)
                    return left;
            }
            uint16_t offs[16] = {};
            for (int len = 1; len < 15; ++len)
                offs[len + 1] = uint16_t(offs[len] + h.count[len]);
            h.symbol.assign(size_t(n), 0);
            for (int s = 0; s < n; ++s)
                if (length[s])
                    h.symbol[offs[length[s]]++] = uint16_t(s);
            return left;
        }
        int decode(InState &s, const Huff &h)
        {
            int code = 0, first = 0, index = 0;
            for (int len = 1; len < 16; ++len)
            {
                uint32_t bit = 0;
                if (!s.bits(1, bit))
                    return -1;
                code |= int(bit);
                const int count = h.count[len];
                if (code - count < first)
                    return h.symbol[size_t(index + (code - first))];
                index += count;
                first += count;
                first <<= 1;
                code <<= 1;
            }
            return -2;
        }
        bool codes(InState &s, const Huff &lencode, const Huff &distcode)
        {
            for (;;)
            {
                const int symbol = decode(s, lencode);
                if (symbol < 0)
                    return false;
                if (symbol < 256)
                {
                    if (s.out->size() >= s.limit)
                        return false;
                    s.out->push_back(uint8_t(symbol));
                    continue;
                }
                if (symbol == 256)
                    return true;
                const int li = symbol - 257;
                if (li >= 29)
                    return false;
                uint32_t e = 0;
                if (!s.bits(kLenExtra[li], e))
                    return false;
                const size_t length = kLenBase[li] + e;
                const int di = decode(s, distcode);
                if (di < 0 || di >= 30)
                    return false;
                if (!s.bits(kDistExtra[di], e))
                    return false;
                const size_t dist = kDistBase[di] + e;
                if (dist > s.out->size() || s.out->size() + length > s.limit)
                    return false;
                const size_t from = s.out->size() - dist;
                for (size_t i = 0; i < length; ++i)
                    s.out->push_back((*s.out)[from + i]);
            }
        }
        bool stored(InState &s)
        {
            s.bitBuf = 0;
            s.bitCount = 0;
            if (s.pos + 4 > s.in.size())
                return false;
            const uint32_t len = s.in[s.pos] | (uint32_t(s.in[s.pos + 1]) << 8);
            const uint32_t nlen = s.in[s.pos + 2] | (uint32_t(s.in[s.pos + 3]) << 8);
            s.pos += 4;
            if (len != (~nlen & 0xffffu) || s.pos + len > s.in.size() || s.out->size() + len > s.limit)
                return false;
            s.out->insert(s.out->end(), s.in.begin() + std::ptrdiff_t(s.pos), s.in.begin() + std::ptrdiff_t(s.pos + len));
            s.pos += len;
            return true;
        }
        bool fixedBlock(InState &s)
        {
            static const std::pair<Huff, Huff> tables = [] {
                uint8_t lengths[288];
                int i = 0;
                for (; i < 144; ++i)
                    lengths[i] = 8;
                for (; i < 256; ++i)
                    lengths[i] = 9;
                for (; i < 280; ++i)
                    lengths[i] = 7;
                for (; i < 288; ++i)
                    lengths[i] = 8;
                Huff l, d;
                construct(l, lengths, 288);
                uint8_t dl[30];
                std::fill(std::begin(dl), std::end(dl), uint8_t(5));
                construct(d, dl, 30);
                return std::pair<Huff, Huff>(l, d);
            }();
            return codes(s, tables.first, tables.second);
        }
        bool dynamicBlock(InState &s)
        {
            uint32_t nlen = 0, ndist = 0, ncode = 0;
            if (!s.bits(5, nlen) || !s.bits(5, ndist) || !s.bits(4, ncode))
                return false;
            nlen += 257;
            ndist += 1;
            ncode += 4;
            if (nlen > 286 || ndist > 30)
                return false;
            uint8_t lengths[320] = {};
            for (uint32_t i = 0; i < ncode; ++i)
            {
                uint32_t v = 0;
                if (!s.bits(3, v))
                    return false;
                lengths[kClOrder[i]] = uint8_t(v);
            }
            Huff lencode, distcode;
            if (construct(lencode, lengths, 19) != 0)
                return false;
            uint32_t index = 0;
            while (index < nlen + ndist)
            {
                int symbol = decode(s, lencode);
                if (symbol < 0)
                    return false;
                if (symbol < 16)
                {
                    lengths[index++] = uint8_t(symbol);
                    continue;
                }
                uint8_t len = 0;
                uint32_t rep = 0;
                if (symbol == 16)
                {
                    if (index == 0)
                        return false;
                    len = lengths[index - 1];
                    if (!s.bits(2, rep))
                        return false;
                    rep += 3;
                }
                else if (symbol == 17)
                {
                    if (!s.bits(3, rep))
                        return false;
                    rep += 3;
                }
                else
                {
                    if (!s.bits(7, rep))
                        return false;
                    rep += 11;
                }
                if (index + rep > nlen + ndist)
                    return false;
                while (rep--)
                    lengths[index++] = len;
            }
            if (lengths[256] == 0)
                return false;
            int err = construct(lencode, lengths, int(nlen));
            if (err < 0 || (err > 0 && int(nlen) - lencode.count[0] != 1))
                return false;
            err = construct(distcode, lengths + nlen, int(ndist));
            if (err < 0 || (err > 0 && int(ndist) - distcode.count[0] != 1))
                return false;
            return codes(s, lencode, distcode);
        }

        std::string lower(std::string_view text)
        {
            std::string s(text);
            for (char &c : s)
                c = char(std::tolower(static_cast<unsigned char>(c)));
            return s;
        }

        uint8_t nibble(char c) { return uint8_t(c <= '9' ? c - '0' : (c | 0x20) - 'a' + 10); }

        Digest digestFromHex(const char *hex)
        {
            Digest d{};
            for (size_t i = 0; i < 32; ++i)
                d[i] = uint8_t((nibble(hex[2 * i]) << 4) | nibble(hex[2 * i + 1]));
            return d;
        }

        std::vector<uint8_t> bytesFromHex(std::string_view hex)
        {
            std::vector<uint8_t> out(hex.size() / 2);
            for (size_t i = 0; i < out.size(); ++i)
                out[i] = uint8_t((nibble(hex[2 * i]) << 4) | nibble(hex[2 * i + 1]));
            return out;
        }

        std::vector<uint8_t> le32(int32_t v)
        {
            const uint32_t u = static_cast<uint32_t>(v);
            return {uint8_t(u), uint8_t(u >> 8), uint8_t(u >> 16), uint8_t(u >> 24)};
        }
        int32_t fromLe32(const uint8_t *b)
        {
            return static_cast<int32_t>(uint32_t(b[0]) | (uint32_t(b[1]) << 8) | (uint32_t(b[2]) << 16) | (uint32_t(b[3]) << 24));
        }
        std::string hexOffset(uint32_t v)
        {
            char t[16];
            std::snprintf(t, sizeof(t), "0x%x", v);
            return t;
        }
        constexpr uint8_t kHeaderMagic[4] = {0xc5, 0xee, 0xf7, 0xff};

        // ---------------------------------------------------------------- the USA disc (SCUS-97328)
        // Stock files (GT4-CHEATS.md 5.3) and the patched gtmode.adc / option.adc for each switch set
        // (MISSION-FIX.md 2.4; secret: the same check, analysis/cheats/runtime/README.md).
        constexpr const char *kGtmodeSha = "574411086b372da4a911e7ce926e92695f905d93cbf949fb5ce6ac37798d791e";
        constexpr const char *kOptionSha = "acd29dd0507bdb479b0a64e2d8603b3e40a2ad34ab270479772a3e40bf56448c";
        constexpr const char *kNodaySha = "b3ae327d73f062fc6864b906b74414ea314f19816ccf606505d6caef73f57ddc";
        constexpr const char *kMissionfixSha = "289de2bfffd30ee1aea48243966c5b5fcad7c03245f6df9474cb26a36fec1793";
        constexpr const char *kNodayMissionfixSha = "8d07e652fa696320c36f084bebcdafc7c4c1eda19ed25675a3ebaa112be93bc1";
        constexpr const char *kSecretSha = "6ac7fb421ad855f3fe3fd98f66d1135375d5f3dd2d6ab0447de90b74f6510e2e";
        // video-output alone and with secret (analysis/design/480p-colour/scripts/video_output_patch.py, the USA
        // disc's option.adc with the INT_CONST value at 0x024856 set to 0; and 0xbb10 set to -1).
        constexpr const char *kVideoOutputSha = "94452037a204317de1a679cc1e4da8d41217e7f3f8924a8d32e952f688f76b93";
        constexpr const char *kSecretVideoOutputSha = "6faa15d72279694ca115183e4fd96176d1155428c745377d0e0c974dd10fc0bf";

        // missionfix, "parity" (MISSION-FIX.md 2.1-2.3). The stock MissionRoot::doCraft frame
        // (0x0a0d36-0x0a0e2c, `main::game.incMoney(10000000); refreshMoney(MissionRoot);`) is checked by
        // its SHA-256 only; the new frame is our own bytecode (78 instructions, header words 7 and 10,
        // SHA-256 d439efaa...37f6), built from GT4's event-race code step for step:
        //   var fwidget = MissionRoot.getFocusedWidget(); if (fwidget == nil) return;
        //   if (!fwidget.defined("idx")) return;
        //   var race_code = "usl0m%04d".format(fwidget["idx"]);
        //   var unit = main::game.race_record.getUnit(race_code); unit.update("mission", "1");
        //   main::game.calendar.putRunMissionEvent(race_code, unit.result, "", 10000);
        constexpr uint32_t kDoCraftOffset = 0x0a0d36u, kDoCraftLength = 246u;
        constexpr const char *kDoCraftSha = "64ec97423b45e2d10dbc7a1294137257ef7d4d9b5ab32f1b422a37506a33d1c4";
        constexpr const char *kDoCraftParity =
            "0000010000000700636f6e74657874070000000a0000004e000000d82200002401000000070066776964676574020000"
            "00d822000024020000000b004d697373696f6e526f6f740b004d697373696f6e526f6f7403000000d822000008d82200"
            "00031000676574466f6375736564576964676574d822000008d82200000600000000d822000008d822000001d8220000"
            "18d8220000240100000007006677696467657402000000d822000008d822000016d82200000502003d3dd82200000f0f"
            "000000d82200001b01d8220000240100000007006677696467657402000000d822000008d8220000030700646566696e"
            "6564d822000008d82200001d0300696478d82200000601000000d822000008d822000022010021d82200000f19000000"
            "d82200001b01d822000024010000000900726163655f636f646504000000d82200001d090075736c306d25303464d822"
            "0000030600666f726d6174d822000008d8220000240100000007006677696467657402000000d822000008d82200001d"
            "0300696478d82200000502005b5dd822000008d82200000601000000d822000008d822000001d822000018d822000024"
            "010000000400756e697405000000d8220000240300000004006d61696e040067616d650a006d61696e3a3a67616d6506"
            "000000d822000008d8220000030b00726163655f7265636f7264d822000008d8220000030700676574556e6974d82200"
            "0008d822000024010000000900726163655f636f646504000000d822000008d82200000601000000d822000008d82200"
            "0001d822000018d822000024010000000400756e697405000000d822000008d8220000030600757064617465d8220000"
            "08d82200001d07006d697373696f6ed82200001d010031d82200000602000000d822000008d822000018d82200002403"
            "00000004006d61696e040067616d650a006d61696e3a3a67616d6506000000d822000008d822000003080063616c656e"
            "646172d822000008d822000003120070757452756e4d697373696f6e4576656e74d822000008d8220000240100000009"
            "00726163655f636f646504000000d822000008d822000024010000000400756e697405000000d822000008d822000003"
            "0600726573756c74d822000008d82200001d0000d82200000c10270000d82200000604000000d822000008d822000018"
            "da2200001b01";
        // The file keeps its size (GT4's loader allocates the header's size): the +624 bytes come from the
        // source-path strings of the 45 code frames after doCraft, which GT4's frame reader (0x2f4a10) reads
        // into a temporary and drops. 44 become empty, the 45th "_tmp_g" (MISSION-FIX.md 2.2, 2.3).
        constexpr std::string_view kSourcePath{"\x0e\x00_tmp_gtmode.ad", 16};
        constexpr uint32_t kEmptiedPaths[44] = {
            0x0a0e3d, 0x0a0f73, 0x0a1145, 0x0a16f3, 0x0a17f7, 0x0a19e9, 0x0a1a6e, 0x0a1d02, 0x0a2c7c, 0x0a2f9d, 0x0a3457,
            0x0a3e57, 0x0a42db, 0x0a55ab, 0x0a631c, 0x0a6c64, 0x0a790e, 0x0a8256, 0x0a8820, 0x0a9705, 0x0aa0b7, 0x0aa10b,
            0x0aa1e3, 0x0aa4b3, 0x0aa907, 0x0aaa97, 0x0aad90, 0x0adf8a, 0x0ae1b8, 0x0ae95f, 0x0af078, 0x0af5e0, 0x0af9a8,
            0x0afad6, 0x0b01c2, 0x0b036f, 0x0b0c27, 0x0b0cac, 0x0b3c57, 0x0b3faf, 0x0b425b, 0x0b4c0f, 0x0b4cf3, 0x0b4dd3};
        constexpr uint32_t kShortenedPath = 0x0b4f99u;

        std::optional<Digest> publishedGtmode(bool noday, int32_t value, bool missionfix)
        {
            if (noday && value != -1)
                return std::nullopt; // GT4_CHEATS_TEST_NODAY: no published hash for a test value
            if (noday && missionfix)
                return digestFromHex(kNodayMissionfixSha);
            if (noday)
                return digestFromHex(kNodaySha);
            if (missionfix)
                return digestFromHex(kMissionfixSha);
            return std::nullopt;
        }

        bool sha256Of(const Sha256 &sha, std::span<const uint8_t> bytes, Digest &digest) { return sha && sha(bytes, digest); }

        // servedState(): bit 31 known, bits 8-15 asked, bits 0-7 served (ServedBit).
        std::atomic<uint32_t> g_served{0};
        // setVideoOutputHidden: -1 not said yet, 0 shown, 1 hidden.
        std::atomic<int> g_videoOutput{-1};

        uint8_t bitsOfAttempt(const Attempt &attempt)
        {
            unsigned bits = 0;
            for (const SwitchPatch &p : attempt.parts)
                bits |= p.switchName == "noday" ? unsigned(ServedNoday) : p.switchName == "secret" ? unsigned(ServedSecret) : p.switchName == "missionfix" ? unsigned(ServedMissionfix) : 0u;
            return uint8_t(bits);
        }
    }

    uint8_t switchBits(const Switches &s)
    {
        return uint8_t((s.noday ? ServedNoday : 0) | (s.secret ? ServedSecret : 0) | (s.missionfix ? ServedMissionfix : 0));
    }

    void publishServed(uint8_t asked, uint8_t served)
    {
        g_served.store(0x80000000u | (uint32_t(asked) << 8) | served, std::memory_order_release);
    }

    uint8_t askedSwitches() { return switchBits(switchesFromEnvironment()); }

    void setVideoOutputHidden(bool hidden) { g_videoOutput.store(hidden ? 1 : 0, std::memory_order_release); }

    int videoOutputDecision() { return g_videoOutput.load(std::memory_order_acquire); }

    bool touchesScriptFiles(uint64_t offset, size_t bytes)
    {
        for (const ScriptFile &f : {gtmodeFile(), optionFile()})
            if (offset < f.discEnd() && offset + bytes > f.discOffset())
                return true;
        return false;
    }

    ServedState servedState()
    {
        const uint32_t v = g_served.load(std::memory_order_acquire);
        ServedState s;
        s.known = (v >> 31) != 0;
        s.asked = uint8_t(v >> 8);
        s.served = uint8_t(v);
        return s;
    }

    // ---------------------------------------------------------------- public
    std::string shortHex(const Digest &d)
    {
        char t[24];
        std::snprintf(t, sizeof(t), "%02x%02x%02x%02x...%02x%02x", d[0], d[1], d[2], d[3], d[30], d[31]);
        return t;
    }

    Switches parseSwitches(std::string_view text)
    {
        Switches s;
        std::string word;
        auto take = [&] {
            if (word.empty())
                return;
            const std::string w = lower(word);
            if (w == "noday")
                s.noday = true;
            else if (w == "secret")
                s.secret = true;
            else if (w == "missionfix")
                s.missionfix = true;
            else if (w != "off" && w != "none")
                s.unknown.push_back(word);
            word.clear();
        };
        for (char c : text)
        {
            if (c == ',' || c == ' ' || c == ';' || c == '\t')
                take();
            else
                word.push_back(c);
        }
        take();
        return s;
    }

    Switches switchesFromEnvironment()
    {
        const char *v = std::getenv("GT4_CHEATS");
        Switches s = parseSwitches(v ? v : "");
#if !defined(GT4_STOCK_BUILD)
        // Tests only (T3, CHEATS-TAB-DESIGN.md 4.3): the value written over 365. Compiled out of the stock build.
        if (const char *t = std::getenv("GT4_CHEATS_TEST_NODAY"); t && *t)
            s.nodayValue = static_cast<int32_t>(std::strtol(t, nullptr, 10));
#endif
        s.hideVideoOutput = videoOutputDecision() == 1;
        return s;
    }

    ScriptFile gtmodeFile()
    {
        ScriptFile f;
        f.name = "projects/gtmode/gtmode.adc";
        f.lba = 146934u;
        f.storedBytes = 249951u;
        f.size = 997339u;
        f.sha256 = digestFromHex(kGtmodeSha);
        return f;
    }

    ScriptFile optionFile()
    {
        ScriptFile f;
        f.name = "projects/option/option.adc";
        f.lba = 148435u;
        f.storedBytes = 55353u;
        f.size = 251715u;
        f.sha256 = digestFromHex(kOptionSha);
        return f;
    }

    SwitchPatch nodayPatch(int32_t value)
    {
        SwitchPatch p;
        p.switchName = "noday";
        p.summary = "0x3b501 365 -> " + std::to_string(value);
        Splice s;
        s.offset = 0x3b501u; // craft(): INT_CONST 365 at 0x3b500, operator '>'
        s.oldLength = 4;
        s.oldBytes = le32(365);
        s.newBytes = le32(value);
        p.splices.push_back(std::move(s));
        return p;
    }

    SwitchPatch missionfixPatch()
    {
        SwitchPatch p;
        p.switchName = "missionfix";
        p.summary = "MissionRoot::doCraft 0x0a0d36 246 -> 870 bytes, 45 source paths shortened";
        Splice frame;
        frame.offset = kDoCraftOffset;
        frame.oldLength = kDoCraftLength;
        frame.oldSha256 = digestFromHex(kDoCraftSha);
        frame.newBytes = bytesFromHex(kDoCraftParity);
        p.splices.push_back(std::move(frame));
        const std::vector<uint8_t> path(kSourcePath.begin(), kSourcePath.end());
        for (uint32_t offset : kEmptiedPaths)
        {
            Splice s;
            s.offset = offset;
            s.oldLength = uint32_t(path.size());
            s.oldBytes = path;
            s.newBytes = {0x00, 0x00};
            p.splices.push_back(std::move(s));
        }
        Splice last;
        last.offset = kShortenedPath;
        last.oldLength = uint32_t(path.size());
        last.oldBytes = path;
        last.newBytes = {0x06, 0x00, '_', 't', 'm', 'p', '_', 'g'};
        p.splices.push_back(std::move(last));
        return p;
    }

    SwitchPatch secretPatch()
    {
        SwitchPatch p;
        p.switchName = "secret";
        p.summary = "0xbb10 0 -> -1";
        Splice s;
        s.offset = 0xbb10u; // OptionRoot.onKeyPress: INT_CONST 0 at 0xbb0f in "event.port == 0"
        s.oldLength = 4;
        s.oldBytes = le32(0);
        s.newBytes = le32(-1);
        p.splices.push_back(std::move(s));
        return p;
    }

    SwitchPatch videoOutputPatch()
    {
        SwitchPatch p;
        p.switchName = "video-output";
        p.summary = "0x24856 1 -> 0 (Options > Screen Settings without the Video Output line)";
        Splice s;
        s.offset = 0x24856u; // ModeType::onInitialize: INT_CONST 1 at 0x024855 in "Pane::HiVision.visible = 1"
        s.oldLength = 4;
        s.oldBytes = le32(1);
        s.newBytes = le32(0);
        p.splices.push_back(std::move(s));
        return p;
    }

    std::string Attempt::switchNames() const
    {
        std::string t;
        for (const SwitchPatch &p : parts)
            t += (t.empty() ? "" : ", ") + p.switchName;
        return t;
    }

    std::string Attempt::summary() const
    {
        std::string t;
        for (const SwitchPatch &p : parts)
            t += (t.empty() ? "" : "; ") + p.summary;
        return t;
    }

    std::vector<FilePatch> patchesFor(const Switches &switches)
    {
        std::vector<FilePatch> patches;
        if (switches.noday || switches.missionfix)
        {
            FilePatch f;
            f.file = gtmodeFile();
            Attempt all;
            if (switches.noday)
                all.parts.push_back(nodayPatch(switches.nodayValue));
            if (switches.missionfix)
                all.parts.push_back(missionfixPatch());
            all.resultSha256 = publishedGtmode(switches.noday, switches.nodayValue, switches.missionfix);
            f.attempts.push_back(std::move(all));
            if (switches.noday && switches.missionfix)
            {
                // The fallback within what was asked for: noday alone (MISSION-FIX.md 4, step 4).
                Attempt noday;
                noday.parts.push_back(nodayPatch(switches.nodayValue));
                noday.resultSha256 = publishedGtmode(true, switches.nodayValue, false);
                f.attempts.push_back(std::move(noday));
            }
            patches.push_back(std::move(f));
        }
        if (switches.secret || switches.hideVideoOutput)
        {
            FilePatch f;
            f.file = optionFile();
            if (switches.secret && switches.hideVideoOutput)
            {
                Attempt both;
                both.parts.push_back(secretPatch());
                both.parts.push_back(videoOutputPatch());
                both.resultSha256 = digestFromHex(kSecretVideoOutputSha);
                f.attempts.push_back(std::move(both));
            }
            // Each alone (with both asked for: the fallbacks within what was asked for).
            if (switches.hideVideoOutput)
            {
                Attempt hide;
                hide.parts.push_back(videoOutputPatch());
                hide.resultSha256 = digestFromHex(kVideoOutputSha);
                f.attempts.push_back(std::move(hide));
            }
            if (switches.secret)
            {
                Attempt secret;
                secret.parts.push_back(secretPatch());
                secret.resultSha256 = digestFromHex(kSecretSha);
                f.attempts.push_back(std::move(secret));
            }
            patches.push_back(std::move(f));
        }
        return patches;
    }

    std::string describe(const Switches &switches)
    {
        std::string text = "[cheats] GT4_CHEATS:";
        std::string sep = " ";
        if (switches.noday)
        {
            text += sep + "noday (the 365-day rule of GT4's button codes skipped: getElapsedDate() > " + std::to_string(switches.nodayValue) + ")";
            sep = ", ";
        }
        if (switches.missionfix)
        {
            text += sep + "missionfix (the Mission Hall code passes the highlighted mission instead of giving 10,000,000 credits; our fix)";
            sep = ", ";
        }
        if (switches.secret)
            text += sep + "secret (controller 1 may type the hidden Options code)";
        if (!switches.cheats())
            text += " off";
        for (const std::string &u : switches.unknown)
            text += "; '" + u + "' is not a GT4_CHEATS word (noday, missionfix, secret): ignored";
        return text;
    }

    std::vector<uint8_t> deflateRaw(std::span<const uint8_t> data)
    {
        // zlib level 9: good 32, lazy 258, nice 258, chain 4096; window distances up to
        // 32768 - 262 (zlib's MAX_DIST); a block every 16383 symbols (memLevel 8).
        constexpr int kGood = 32, kLazy = 258, kNice = 258, kChain = 4096, kMaxDist = 32768 - 262, kTooFar = 4096;
        constexpr size_t kBlockSymbols = 16383;
        const size_t n = data.size();
        std::vector<int32_t> head(1u << 15, -1), prev(n, -1);
        auto hash = [&](size_t i) { return ((uint32_t(data[i]) << 10) ^ (uint32_t(data[i + 1]) << 5) ^ data[i + 2]) & 0x7fffu; };
        auto insert = [&](size_t i) -> int32_t {
            if (i + 2 >= n)
                return -1;
            const uint32_t h = hash(i);
            const int32_t old = head[h];
            prev[i] = old;
            head[h] = int32_t(i);
            return old;
        };
        auto longest = [&](size_t cur, int32_t cand, int best, size_t &matchStart) {
            int chain = best >= kGood ? kChain >> 2 : kChain;
            const int maxLen = int(std::min<size_t>(258, n - cur));
            const int nice = std::min(kNice, maxLen);
            while (cand >= 0 && cur - size_t(cand) <= size_t(kMaxDist) && chain-- > 0)
            {
                const uint8_t *a = data.data() + cand, *b = data.data() + cur;
                if (best < maxLen && a[best] == b[best] && a[0] == b[0])
                {
                    int len = 0;
                    while (len < maxLen && a[len] == b[len])
                        ++len;
                    if (len > best)
                    {
                        best = len;
                        matchStart = size_t(cand);
                        if (len >= nice)
                            break;
                    }
                }
                cand = prev[size_t(cand)];
            }
            return best;
        };

        BitWriter w;
        std::vector<Symbol> symbols;
        symbols.reserve(kBlockSymbols);
        size_t blockStart = 0, covered = 0;
        auto flush = [&] {
            if (symbols.empty())
                return;
            writeBlock(w, symbols, data.subspan(blockStart, covered - blockStart));
            symbols.clear();
            blockStart = covered;
        };
        auto emit = [&](Symbol s) {
            symbols.push_back(s);
            covered += s.dist ? s.litlen : 1u;
            if (symbols.size() >= kBlockSymbols)
                flush();
        };

        int matchLength = 2, prevLength = 2;
        size_t matchStart = 0, prevMatch = 0;
        bool matchAvailable = false;
        size_t s = 0;
        while (s < n)
        {
            const int32_t hashHead = insert(s);
            prevLength = matchLength;
            prevMatch = matchStart;
            matchLength = 2;
            if (hashHead >= 0 && prevLength < kLazy && s - size_t(hashHead) <= size_t(kMaxDist))
            {
                matchLength = longest(s, hashHead, prevLength, matchStart);
                if (matchLength <= prevLength)
                    matchStart = prevMatch; // nothing longer: keep the earlier match
                if (matchLength == 3 && s - matchStart > size_t(kTooFar))
                    matchLength = 2;
            }
            if (prevLength >= 3 && matchLength <= prevLength)
            {
                emit(Symbol{uint16_t(prevLength), uint16_t((s - 1) - prevMatch)});
                // The strings inside the match go into the hash too (zlib).
                const size_t end = s - 1 + size_t(prevLength);
                for (size_t i = s + 1; i < end; ++i)
                    insert(i);
                matchAvailable = false;
                matchLength = 2;
                s = end;
            }
            else if (matchAvailable)
            {
                emit(Symbol{data[s - 1], 0});
                ++s;
            }
            else
            {
                matchAvailable = true;
                ++s;
            }
        }
        if (matchAvailable)
            emit(Symbol{data[n - 1], 0});
        flush();
        // Byte aligned, not final: an empty stored block (a sync flush).
        w.put(0, 1);
        w.put(0, 2);
        w.align();
        w.put(0x0000u, 16);
        w.put(0xffffu, 16);
        return w.out;
    }

    std::vector<uint8_t> emptyBlocks(size_t bytes)
    {
        // Each piece starts and ends byte aligned: S = an empty stored block (5 bytes);
        // FS = an empty fixed block and an empty stored block (6 bytes); the end = k empty
        // fixed blocks and a final one (10 (k + 1) bits: 2, 3, 4 or 5 bytes for k = 0..3).
        for (size_t fs = 0; fs < 5; ++fs)
            for (size_t k = 0; k < 4; ++k)
            {
                const size_t tail = (10 * (k + 1) + 7) / 8;
                if (bytes < tail + 6 * fs || (bytes - tail - 6 * fs) % 5)
                    continue;
                const size_t s5 = (bytes - tail - 6 * fs) / 5;
                BitWriter w;
                auto storedEmpty = [&] {
                    w.put(0, 1);
                    w.put(0, 2);
                    w.align();
                    w.put(0x0000u, 16);
                    w.put(0xffffu, 16);
                };
                auto fixedEmpty = [&](bool final) {
                    w.put(final ? 1u : 0u, 1);
                    w.put(1, 2);
                    w.put(0, 7); // end of block, code 256 = 0000000
                };
                for (size_t i = 0; i < s5; ++i)
                    storedEmpty();
                for (size_t i = 0; i < fs; ++i)
                {
                    fixedEmpty(false);
                    storedEmpty();
                }
                for (size_t i = 0; i < k; ++i)
                    fixedEmpty(false);
                fixedEmpty(true);
                w.align();
                if (w.out.size() == bytes)
                    return w.out;
            }
        return {};
    }

    bool inflateRaw(std::span<const uint8_t> stream, size_t limit, std::vector<uint8_t> &out, size_t &consumed)
    {
        InState s;
        s.in = stream;
        s.out = &out;
        s.limit = limit;
        out.clear();
        consumed = 0;
        for (;;)
        {
            uint32_t last = 0, type = 0;
            if (!s.bits(1, last) || !s.bits(2, type))
                return false;
            bool ok = false;
            if (type == 0)
                ok = stored(s);
            else if (type == 1)
                ok = fixedBlock(s);
            else if (type == 2)
                ok = dynamicBlock(s);
            if (!ok)
                return false;
            if (last)
                break;
        }
        consumed = s.pos;
        return true;
    }

    bool applySplices(std::span<const uint8_t> file, const Attempt &attempt, const Sha256 &sha, std::vector<uint8_t> &out, std::string &why)
    {
        out.clear();
        std::vector<const Splice *> splices;
        for (const SwitchPatch &p : attempt.parts)
            for (const Splice &s : p.splices)
                splices.push_back(&s);
        std::stable_sort(splices.begin(), splices.end(), [](const Splice *a, const Splice *b) { return a->offset < b->offset; });
        uint64_t previousEnd = 0;
        for (const Splice *s : splices)
        {
            const uint64_t end = uint64_t(s->offset) + s->oldLength;
            if (end > file.size())
            {
                why = "a change at " + hexOffset(s->offset) + " runs past the end of the file";
                return false;
            }
            if (s->offset < previousEnd)
            {
                why = "two changes overlap at " + hexOffset(s->offset);
                return false;
            }
            previousEnd = end;
            const std::span<const uint8_t> old = file.subspan(s->offset, s->oldLength);
            if (!s->oldBytes.empty())
            {
                if (s->oldBytes.size() != s->oldLength || !std::equal(old.begin(), old.end(), s->oldBytes.begin()))
                {
                    why = "the bytes at " + hexOffset(s->offset) + " are not the expected old value";
                    return false;
                }
            }
            else if (s->oldSha256)
            {
                Digest d{};
                if (!sha256Of(sha, old, d) || d != *s->oldSha256)
                {
                    why = "the " + std::to_string(s->oldLength) + " bytes at " + hexOffset(s->offset) + " are not the expected old code";
                    return false;
                }
            }
            else
            {
                why = "the change at " + hexOffset(s->offset) + " has no old value to check";
                return false;
            }
        }
        // Built in file order; the same as applying them from the highest offset down (MISSION-FIX.md 2.3).
        out.reserve(file.size());
        size_t at = 0;
        for (const Splice *s : splices)
        {
            out.insert(out.end(), file.begin() + std::ptrdiff_t(at), file.begin() + std::ptrdiff_t(s->offset));
            out.insert(out.end(), s->newBytes.begin(), s->newBytes.end());
            at = size_t(s->offset) + s->oldLength;
        }
        out.insert(out.end(), file.begin() + std::ptrdiff_t(at), file.end());
        if (out.size() != file.size())
        {
            why = "the changes make the file " + std::to_string(out.size()) + " bytes, not " + std::to_string(file.size());
            out.clear();
            return false;
        }
        if (attempt.resultSha256)
        {
            Digest d{};
            if (!sha256Of(sha, out, d) || d != *attempt.resultSha256)
            {
                why = "the patched file's SHA-256 (" + shortHex(d) + ") is not the published " + shortHex(*attempt.resultSha256);
                out.clear();
                return false;
            }
        }
        return true;
    }

    bool buildReplacement(const FilePatch &patch, std::span<const uint8_t> stored, const Sha256 &sha, Built &built, std::string &why)
    {
        built = Built{};
        const ScriptFile &f = patch.file;
        if (stored.size() != f.storedBytes || stored.size() < 8)
        {
            why = "read " + std::to_string(stored.size()) + " bytes, " + std::to_string(f.storedBytes) + " expected";
            return false;
        }
        const int32_t negSize = fromLe32(stored.data() + 4);
        if (!std::equal(std::begin(kHeaderMagic), std::end(kHeaderMagic), stored.begin()) || negSize != -int32_t(f.size))
        {
            why = "not a compressed GT4.VOL file of " + std::to_string(f.size) + " bytes at sector " + std::to_string(f.lba);
            return false;
        }
        std::vector<uint8_t> file;
        size_t consumed = 0;
        if (!inflateRaw(stored.subspan(8), f.size, file, consumed) || file.size() != f.size)
        {
            why = "the stored stream does not inflate to " + std::to_string(f.size) + " bytes";
            return false;
        }
        Digest digest{};
        if (!sha256Of(sha, file, digest))
        {
            why = "no SHA-256 available";
            return false;
        }
        if (digest != f.sha256)
        {
            why = "SHA-256 differs from the USA disc's (another disc or version)";
            return false;
        }
        const size_t room = f.storedBytes - 8u;
        for (size_t i = 0; i < patch.attempts.size(); ++i)
        {
            const Attempt &attempt = patch.attempts[i];
            std::string fail;
            std::vector<uint8_t> patched;
            if (!applySplices(file, attempt, sha, patched, fail))
            {
                built.failures.push_back(attempt.switchNames() + ": " + fail);
                continue;
            }
            const std::vector<uint8_t> body = deflateRaw(patched);
            if (body.size() + 2u > room)
            {
                built.failures.push_back(attempt.switchNames() + ": the patched file deflates to " + std::to_string(body.size()) + " bytes; " +
                                         std::to_string(room) + " fit");
                continue;
            }
            const std::vector<uint8_t> padding = emptyBlocks(room - body.size());
            if (padding.empty())
            {
                built.failures.push_back(attempt.switchNames() + ": no padding of " + std::to_string(room - body.size()) + " bytes");
                continue;
            }
            std::vector<uint8_t> out(stored.begin(), stored.begin() + 8);
            out.insert(out.end(), body.begin(), body.end());
            out.insert(out.end(), padding.begin(), padding.end());
            // Checked as GT4 will read it: the whole stored size, inflated to exactly the patched file.
            std::vector<uint8_t> check;
            if (out.size() != f.storedBytes || !inflateRaw(std::span<const uint8_t>(out).subspan(8), f.size, check, consumed) || consumed != room ||
                check != patched)
            {
                built.failures.push_back(attempt.switchNames() + ": the rebuilt stream did not inflate back to the patched file");
                continue;
            }
            sha256Of(sha, patched, built.result);
            built.stream = std::move(out);
            built.attempt = i;
            built.deflated = body.size();
            built.padding = padding.size();
            return true;
        }
        why.clear();
        for (const std::string &failure : built.failures)
            why += (why.empty() ? "" : "; ") + failure;
        return false;
    }

    Overlay::Overlay(Switches switches, std::vector<FilePatch> patches, Sha256 sha, Log log)
        : patches_(switches.any() ? std::move(patches) : std::vector<FilePatch>{}), sha_(std::move(sha)), log_(std::move(log))
    {
        // One entry per file. Two entries over the same sectors would overwrite each other's bytes
        // silently (MISSION-FIX.md 4, step 2): refuse both.
        std::vector<bool> refused(patches_.size(), false);
        for (size_t i = 0; i < patches_.size(); ++i)
            for (size_t j = i + 1; j < patches_.size(); ++j)
            {
                const ScriptFile &a = patches_[i].file, &b = patches_[j].file;
                if (a.lba < b.lba + b.sectors() && b.lba < a.lba + a.sectors())
                {
                    refused[i] = refused[j] = true;
                    if (log_)
                        log_(true, std::string("[cheats] ") + a.name + " (sectors " + std::to_string(a.lba) + "+" + std::to_string(a.sectors()) + ") and " +
                                       b.name + " (sectors " + std::to_string(b.lba) + "+" + std::to_string(b.sectors()) +
                                       ") overlap: one overlay entry per file only; both served unchanged");
                }
            }
        std::vector<FilePatch> kept;
        for (size_t i = 0; i < patches_.size(); ++i)
            if (!refused[i])
                kept.push_back(std::move(patches_[i]));
        patches_ = std::move(kept);
    }

    Overlay::~Overlay()
    {
        if (built_.valid())
            built_.wait();
    }

    void Overlay::start(const Reader &read)
    {
        std::call_once(started_, [&] {
            if (patches_.empty())
                return;
            for (const FilePatch &p : patches_)
            {
                Entry e;
                e.patch = p;
                e.stored.resize(p.file.storedBytes);
                if (!read(p.file.discOffset(), e.stored.data(), e.stored.size()))
                {
                    e.stored.clear();
                    log_(true, "[cheats] " + (p.attempts.empty() ? std::string("?") : p.attempts.front().switchNames()) + ": cannot read " + p.file.name +
                                   " (sector " + std::to_string(p.file.lba) + "); served unchanged");
                }
                entries_.push_back(std::move(e));
            }
            built_ = std::async(std::launch::async, [this] { build(); }).share();
        });
    }

    void Overlay::build()
    {
        for (Entry &e : entries_)
        {
            if (e.stored.empty() || e.patch.attempts.empty())
                continue;
            const ScriptFile &f = e.patch.file;
            const std::string asked = e.patch.attempts.front().switchNames();
            Built built;
            std::string why;
            if (buildReplacement(e.patch, e.stored, sha_, built, why))
            {
                const Attempt &used = e.patch.attempts[built.attempt];
                if (built.attempt > 0)
                {
                    std::string failed;
                    for (const std::string &failure : built.failures)
                        failed += (failed.empty() ? "" : "; ") + failure;
                    log_(true, "[cheats] " + asked + ": " + f.name + " not patched as asked: " + failed + "; serving " + used.switchNames() + " only");
                }
                e.switches = used.switchNames();
                e.bits = bitsOfAttempt(used);
                e.replaced = std::move(built.stream);
                log_(false, "[cheats] " + e.switches + ": " + f.name + " " + used.summary() + "; sha256 " + shortHex(built.result) + " (overlay " +
                                std::to_string(f.sectors()) + " sectors from " + std::to_string(f.lba) + ", stream " + std::to_string(built.deflated) +
                                " + " + std::to_string(built.padding) + " padding bytes)");
            }
            else
                log_(true, "[cheats] " + asked + ": " + f.name + " not patched: " + why + "; served unchanged");
            e.stored.clear();
            e.stored.shrink_to_fit();
        }
        if (onBuilt_)
        {
            unsigned bits = 0;
            for (const Entry &e : entries_)
                bits |= e.replaced.empty() ? 0u : unsigned(e.bits);
            onBuilt_(uint8_t(bits));
        }
    }

    uint8_t Overlay::servedBits()
    {
        ready();
        unsigned bits = 0;
        for (const Entry &e : entries_)
            bits |= e.replaced.empty() ? 0u : unsigned(e.bits);
        return uint8_t(bits);
    }

    size_t Overlay::ready()
    {
        if (built_.valid())
            built_.wait();
        size_t n = 0;
        for (const Entry &e : entries_)
            n += e.replaced.empty() ? 0u : 1u;
        return n;
    }

    std::vector<std::string> Overlay::servedSwitches()
    {
        ready();
        std::vector<std::string> out;
        for (const Entry &e : entries_)
            out.push_back(std::string(e.patch.file.name) + ": " + (e.replaced.empty() ? std::string("unchanged") : e.switches));
        return out;
    }

    void Overlay::apply(uint64_t offset, uint8_t *data, size_t bytes, const char *path)
    {
        if (entries_.empty() || !bytes)
            return;
        static const bool trace = [] {
            const char *v = std::getenv("GT4_CHEATS_LOG"); // diagnostics: every read of a patched file
            return v && *v && *v != '0';
        }();
        for (Entry &e : entries_)
        {
            const uint64_t base = e.patch.file.discOffset(), end = e.patch.file.discEnd();
            if (offset >= end || offset + bytes <= base)
                continue;
            if (built_.valid())
            {
                const std::shared_future<void> done = built_;
                done.wait();
            }
            const uint64_t lo = std::max(offset, base), hi = std::min<uint64_t>(offset + bytes, end);
            if (!e.replaced.empty())
                std::memcpy(data + (lo - offset), e.replaced.data() + (lo - base), size_t(hi - lo));
            std::lock_guard lock(servedMutex_);
            if (trace || !e.served)
                log_(false, std::string("[cheats] ") + e.patch.file.name + (e.replaced.empty() ? " read unchanged (" : " served patched (") + path +
                                " at byte " + std::to_string(offset) + ", " + std::to_string(bytes) + " bytes; file bytes " + std::to_string(lo - base) +
                                ".." + std::to_string(hi - base) + ")");
            e.served = true;
        }
    }
}

namespace ps2x::iop::gt4
{
    // Disc reads (gt4_common.cpp) pass through here with the disc's mutex held.
    void Disc::overlayCheats(uint64_t offset, void *data, size_t bytes, const char *path)
    {
        if (!cheats_)
        {
            // GT4.exe says whether GT4's Video Output line is hidden (GT4_RACE_MODE=480p) once CORE is loaded,
            // before GT4 runs: until then a read that touches no script file passes unchanged and the overlay is
            // built at a later read.
            if (cheats::videoOutputDecision() < 0 && !cheats::touchesScriptFiles(offset, bytes))
                return;
            const cheats::Switches switches = cheats::switchesFromEnvironment();
            if (switches.cheats() || !switches.unknown.empty())
                host_.log(LogLevel::Info, cheats::describe(switches));
            if (switches.hideVideoOutput)
                host_.log(LogLevel::Info, "[gt4-video] GT4_RACE_MODE=480p: Options > Screen Settings without GT4's Video Output line "
                                          "(projects/option/option.adc served with the line's visible = 0)");
            cheats_ = std::make_shared<cheats::Overlay>(
                switches, cheats::patchesFor(switches),
                [this](std::span<const uint8_t> bytes, cheats::Digest &digest) { return host_.sha256(bytes, digest); },
                [this](bool error, const std::string &message) { host_.log(error ? LogLevel::Error : LogLevel::Info, message); });
            // What GT4 is actually given, for the in-game card (ps2x/iop/gt4_cheats_served.h).
            const uint8_t asked = cheats::switchBits(switches);
            if (cheats_->enabled())
                cheats_->onBuilt([asked](uint8_t served) { cheats::publishServed(asked, served); });
            else
                cheats::publishServed(asked, 0);
            cheats_->start([this](uint64_t at, void *destination, size_t n) {
                size_t got = 0;
                return host_.readHostFile(file_, at, destination, n, got) && got == n;
            });
        }
        if (cheats_->enabled())
            cheats_->apply(offset, static_cast<uint8_t *>(data), bytes, path);
    }
}
