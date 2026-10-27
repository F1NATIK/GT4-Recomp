// Raw DEFLATE (RFC 1951) decoder: the equivalent of Python's zlib.decompressobj(-15),
// used for CORE.GT4. Canonical Huffman decoding after Mark Adler's puff.c.
#include "inflate.h"

namespace gt4setup {

namespace {

constexpr int MAXBITS = 15;
constexpr int MAXLCODES = 286;
constexpr int MAXDCODES = 30;
constexpr int FIXLCODES = 288;

struct Huffman {
    short count[MAXBITS + 1];
    short symbol[FIXLCODES];
};

struct State {
    const uint8_t *in;
    size_t inLen;
    size_t inPos = 0;
    uint32_t bitbuf = 0;
    int bitcnt = 0;
    Bytes *out;
    size_t maxOut;
};

struct Stop {}; // output limit reached
struct Bad {
    const char *why;
};

int bits(State &s, int need)
{
    uint32_t val = s.bitbuf;
    while (s.bitcnt < need) {
        if (s.inPos >= s.inLen)
            throw Bad{"input ended inside the stream"};
        val |= static_cast<uint32_t>(s.in[s.inPos++]) << s.bitcnt;
        s.bitcnt += 8;
    }
    s.bitbuf = val >> need;
    s.bitcnt -= need;
    return static_cast<int>(val & ((1u << need) - 1u));
}

void put(State &s, uint8_t b)
{
    if (s.out->size() >= s.maxOut)
        throw Stop{};
    s.out->push_back(b);
}

void stored(State &s)
{
    s.bitbuf = 0;
    s.bitcnt = 0;
    if (s.inPos + 4 > s.inLen)
        throw Bad{"stored block header cut off"};
    unsigned len = s.in[s.inPos] | (s.in[s.inPos + 1] << 8);
    const unsigned nlen = s.in[s.inPos + 2] | (s.in[s.inPos + 3] << 8);
    s.inPos += 4;
    if (len != (~nlen & 0xFFFFu))
        throw Bad{"stored block length check"};
    while (len--) {
        if (s.inPos >= s.inLen)
            throw Bad{"stored block cut off"};
        put(s, s.in[s.inPos++]);
    }
}

int decode(State &s, const Huffman &h)
{
    int code = 0, first = 0, index = 0;
    for (int len = 1; len <= MAXBITS; ++len) {
        code |= bits(s, 1);
        const int count = h.count[len];
        if (code - count < first)
            return h.symbol[index + (code - first)];
        index += count;
        first += count;
        first <<= 1;
        code <<= 1;
    }
    throw Bad{"invalid Huffman code"};
}

int construct(Huffman &h, const short *length, int n)
{
    for (int len = 0; len <= MAXBITS; ++len)
        h.count[len] = 0;
    for (int symbol = 0; symbol < n; ++symbol)
        ++h.count[length[symbol]];
    if (h.count[0] == n)
        return 0;
    int left = 1;
    for (int len = 1; len <= MAXBITS; ++len) {
        left <<= 1;
        left -= h.count[len];
        if (left < 0)
            return left;
    }
    short offs[MAXBITS + 1];
    offs[1] = 0;
    for (int len = 1; len < MAXBITS; ++len)
        offs[len + 1] = static_cast<short>(offs[len] + h.count[len]);
    for (int symbol = 0; symbol < n; ++symbol)
        if (length[symbol] != 0)
            h.symbol[offs[length[symbol]]++] = static_cast<short>(symbol);
    return left;
}

const short LBASE[29] = {3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
const short LEXT[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
const short DBASE[30] = {1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
const short DEXT[30] = {0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};

void codes(State &s, const Huffman &lencode, const Huffman &distcode)
{
    for (;;) {
        int symbol = decode(s, lencode);
        if (symbol < 256) {
            put(s, static_cast<uint8_t>(symbol));
        } else if (symbol == 256) {
            return;
        } else {
            symbol -= 257;
            if (symbol >= 29)
                throw Bad{"invalid length symbol"};
            const int len = LBASE[symbol] + bits(s, LEXT[symbol]);
            const int dsym = decode(s, distcode);
            if (dsym >= 30)
                throw Bad{"invalid distance symbol"};
            const size_t dist = static_cast<size_t>(DBASE[dsym] + bits(s, DEXT[dsym]));
            if (dist > s.out->size())
                throw Bad{"distance too far back"};
            for (int k = 0; k < len; ++k)
                put(s, (*s.out)[s.out->size() - dist]);
        }
    }
}

void fixed(State &s)
{
    static Huffman lencode, distcode;
    static bool built = false;
    if (!built) {
        short lengths[FIXLCODES];
        int symbol = 0;
        for (; symbol < 144; ++symbol)
            lengths[symbol] = 8;
        for (; symbol < 256; ++symbol)
            lengths[symbol] = 9;
        for (; symbol < 280; ++symbol)
            lengths[symbol] = 7;
        for (; symbol < FIXLCODES; ++symbol)
            lengths[symbol] = 8;
        construct(lencode, lengths, FIXLCODES);
        for (symbol = 0; symbol < MAXDCODES; ++symbol)
            lengths[symbol] = 5;
        construct(distcode, lengths, MAXDCODES);
        built = true;
    }
    codes(s, lencode, distcode);
}

void dynamic(State &s)
{
    static const short order[19] = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};
    short lengths[MAXLCODES + MAXDCODES];
    const int nlen = bits(s, 5) + 257;
    const int ndist = bits(s, 5) + 1;
    const int ncode = bits(s, 4) + 4;
    if (nlen > MAXLCODES || ndist > MAXDCODES)
        throw Bad{"bad code counts"};
    int index = 0;
    for (; index < ncode; ++index)
        lengths[order[index]] = static_cast<short>(bits(s, 3));
    for (; index < 19; ++index)
        lengths[order[index]] = 0;
    Huffman lencode, distcode;
    if (construct(lencode, lengths, 19) != 0)
        throw Bad{"incomplete code-length code"};
    index = 0;
    while (index < nlen + ndist) {
        int symbol = decode(s, lencode);
        if (symbol < 16) {
            lengths[index++] = static_cast<short>(symbol);
        } else {
            short len = 0;
            if (symbol == 16) {
                if (index == 0)
                    throw Bad{"repeat with no first length"};
                len = lengths[index - 1];
                symbol = 3 + bits(s, 2);
            } else if (symbol == 17) {
                symbol = 3 + bits(s, 3);
            } else {
                symbol = 11 + bits(s, 7);
            }
            if (index + symbol > nlen + ndist)
                throw Bad{"too many lengths"};
            while (symbol--)
                lengths[index++] = len;
        }
    }
    if (lengths[256] == 0)
        throw Bad{"no end-of-block code"};
    int err = construct(lencode, lengths, nlen);
    if (err && (err < 0 || nlen != lencode.count[0] + lencode.count[1]))
        throw Bad{"incomplete literal/length code"};
    err = construct(distcode, lengths + nlen, ndist);
    if (err && (err < 0 || ndist != distcode.count[0] + distcode.count[1]))
        throw Bad{"incomplete distance code"};
    codes(s, lencode, distcode);
}

} // namespace

InflateResult inflateRaw(const uint8_t *in, size_t inLen, size_t maxOut)
{
    InflateResult r;
    State s;
    s.in = in;
    s.inLen = inLen;
    s.out = &r.data;
    s.maxOut = maxOut;
    try {
        int last;
        do {
            last = bits(s, 1);
            const int type = bits(s, 2);
            if (type == 0)
                stored(s);
            else if (type == 1)
                fixed(s);
            else if (type == 2)
                dynamic(s);
            else
                throw Bad{"invalid block type"};
        } while (!last);
        r.ended = true;
        r.consumed = s.inPos;
    } catch (const Stop &) {
        r.ended = false;
        r.consumed = s.inPos;
    } catch (const Bad &b) {
        r.error = b.why;
    }
    return r;
}

} // namespace gt4setup
