// gt4save_disc.cpp - names for spec codes, read at run time from the GT4 disc image.
//
// Port of analysis/design/save-editor/gt4specdb.py. The Python tool finds the spec database files
// through analysis/disc/vol-files.csv; the launcher has no such list, so this reads the disc the way
// GT4's CORE does (analysis/disc/REPORT.md items 5, 10, 11; tools/gt4_disc_map.py parse_rofs):
//   ISO 9660 root -> GT4.VOL -> RoFS header at VOL page 0x2159 (CORE data word 0x68bb24) ->
//   the table of contents (page offsets XOR-keyed, each page raw DEFLATE + XOR 0x55, B-tree leaves) ->
//   file data at header + total pages + page; compressed files are C5 EE F7 FF, -size, raw DEFLATE.
// Nothing from the disc is compiled in; only the container format constants are.

#include "gt4save.h"

// GT4_EXTRA_CARS: the pack reader GT4.exe's disc layer uses (header-only), where the build gives its include folder (the
// Full launcher and GT4.exe; not the Classic launcher, which never serves a pack).
#if __has_include("ps2x/iop/gt4_extra_cars_pack.h")
#include "ps2x/iop/gt4_extra_cars_pack.h"
#include <mutex>
#define GT4SAVE_EXTRA_CARS 1
#endif

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>

namespace gt4save
{
    namespace
    {
        uint16_t le16(const uint8_t *p) { return uint16_t(p[0] | (p[1] << 8)); }
        uint32_t le32(const uint8_t *p) { return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24); }
        uint32_t be32(const uint8_t *p) { return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | uint32_t(p[3]); }

        // ------------------------------------------------------------------ raw DEFLATE (after zlib's puff.c)
        struct Huffman
        {
            short count[16];
            short symbol[288];
        };

        struct Inflater
        {
            const uint8_t *in;
            size_t inLen, inPos = 0;
            uint32_t bitBuf = 0;
            int bitCnt = 0;
            std::vector<uint8_t> &out;
            size_t maxOut;
            bool eof = false;     // the input ended (not an error: see inflateRaw)
            bool corrupt = false; // an invalid stream

            int bits(int need)
            {
                uint32_t val = bitBuf;
                while (bitCnt < need)
                {
                    if (inPos >= inLen)
                    {
                        eof = true;
                        return 0;
                    }
                    val |= uint32_t(in[inPos++]) << bitCnt;
                    bitCnt += 8;
                }
                bitBuf = val >> need;
                bitCnt -= need;
                return int(val & ((1u << need) - 1));
            }

            bool fail()
            {
                corrupt = true;
                return false;
            }

            bool stored()
            {
                bitBuf = 0;
                bitCnt = 0;
                if (inPos + 4 > inLen)
                {
                    eof = true;
                    return false;
                }
                const unsigned len = in[inPos] | (in[inPos + 1] << 8);
                const unsigned nlen = in[inPos + 2] | (in[inPos + 3] << 8);
                inPos += 4;
                if (len != (~nlen & 0xFFFFu))
                    return fail();
                const size_t avail = std::min<size_t>(len, inLen - inPos);
                if (out.size() + avail > maxOut)
                    return fail();
                out.insert(out.end(), in + inPos, in + inPos + avail);
                inPos += avail;
                if (avail < len)
                {
                    eof = true;
                    return false;
                }
                return true;
            }

            int decode(const Huffman &h)
            {
                int code = 0, first = 0, index = 0;
                for (int len = 1; len <= 15; ++len)
                {
                    code |= bits(1);
                    if (eof)
                        return -1;
                    const int count = h.count[len];
                    if (code - count < first)
                        return h.symbol[index + (code - first)];
                    index += count;
                    first += count;
                    first <<= 1;
                    code <<= 1;
                }
                corrupt = true;
                return -10;
            }

            static int construct(Huffman &h, const short *length, int n)
            {
                for (int len = 0; len <= 15; ++len)
                    h.count[len] = 0;
                for (int sym = 0; sym < n; ++sym)
                    h.count[length[sym]]++;
                if (h.count[0] == n)
                    return 0;
                int left = 1;
                for (int len = 1; len <= 15; ++len)
                {
                    left <<= 1;
                    left -= h.count[len];
                    if (left < 0)
                        return left;
                }
                short offs[16];
                offs[1] = 0;
                for (int len = 1; len < 15; ++len)
                    offs[len + 1] = short(offs[len] + h.count[len]);
                for (int sym = 0; sym < n; ++sym)
                    if (length[sym] != 0)
                        h.symbol[offs[length[sym]]++] = short(sym);
                return left;
            }

            bool codes(const Huffman &lencode, const Huffman &distcode)
            {
                static const short lbase[29] = {3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
                static const short lext[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
                static const short dbase[30] = {1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
                static const short dext[30] = {0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};
                for (;;)
                {
                    int sym = decode(lencode);
                    if (sym < 0)
                        return false; // eof or corrupt (decode set the flag)
                    if (sym < 256)
                    {
                        if (out.size() >= maxOut)
                            return fail();
                        out.push_back(uint8_t(sym));
                    }
                    else if (sym == 256)
                        return true;
                    else
                    {
                        sym -= 257;
                        if (sym >= 29)
                            return fail();
                        const size_t len = size_t(lbase[sym] + bits(lext[sym]));
                        if (eof)
                            return false;
                        const int ds = decode(distcode);
                        if (ds < 0)
                            return false;
                        if (ds >= 30)
                            return fail();
                        const size_t dist = size_t(dbase[ds] + bits(dext[ds]));
                        if (eof)
                            return false;
                        if (dist > out.size() || out.size() + len > maxOut)
                            return fail();
                        const size_t from = out.size() - dist;
                        for (size_t i = 0; i < len; ++i)
                            out.push_back(out[from + i]);
                    }
                }
            }

            bool fixed()
            {
                // Built once, thread-safely (a function-local static).
                static const std::pair<Huffman, Huffman> tables = [] {
                    std::pair<Huffman, Huffman> t{};
                    short lengths[288];
                    int sym = 0;
                    for (; sym < 144; ++sym)
                        lengths[sym] = 8;
                    for (; sym < 256; ++sym)
                        lengths[sym] = 9;
                    for (; sym < 280; ++sym)
                        lengths[sym] = 7;
                    for (; sym < 288; ++sym)
                        lengths[sym] = 8;
                    construct(t.first, lengths, 288);
                    for (sym = 0; sym < 30; ++sym)
                        lengths[sym] = 5;
                    construct(t.second, lengths, 30);
                    return t;
                }();
                return codes(tables.first, tables.second);
            }

            bool dynamic()
            {
                static const short order[19] = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};
                short lengths[320];
                const int nlen = bits(5) + 257, ndist = bits(5) + 1, ncode = bits(4) + 4;
                if (eof)
                    return false;
                if (nlen > 286 || ndist > 30)
                    return fail();
                int index = 0;
                for (; index < ncode; ++index)
                    lengths[order[index]] = short(bits(3));
                for (; index < 19; ++index)
                    lengths[order[index]] = 0;
                if (eof)
                    return false;
                Huffman lencode, distcode;
                if (construct(lencode, lengths, 19) != 0)
                    return fail();
                index = 0;
                while (index < nlen + ndist)
                {
                    int sym = decode(lencode);
                    if (sym < 0)
                        return false;
                    if (sym < 16)
                        lengths[index++] = short(sym);
                    else
                    {
                        short len = 0;
                        if (sym == 16)
                        {
                            if (index == 0)
                                return fail();
                            len = lengths[index - 1];
                            sym = 3 + bits(2);
                        }
                        else if (sym == 17)
                            sym = 3 + bits(3);
                        else
                            sym = 11 + bits(7);
                        if (eof)
                            return false;
                        if (index + sym > nlen + ndist)
                            return fail();
                        while (sym--)
                            lengths[index++] = len;
                    }
                }
                if (lengths[256] == 0)
                    return fail();
                int err = construct(lencode, lengths, nlen);
                if (err && (err < 0 || nlen != lencode.count[0] + lencode.count[1]))
                    return fail();
                err = construct(distcode, lengths + nlen, ndist);
                if (err && (err < 0 || ndist != distcode.count[0] + distcode.count[1]))
                    return fail();
                return codes(lencode, distcode);
            }
        };

        constexpr uint32_t kSector = 2048;
        constexpr uint32_t kRofsMagic = 0xACB990ADu, kTocKey = 0x14AC327Au, kTocPage = 0x2159;
        constexpr const char *kSpecDir = "specdb/GT4_US2560/";
#if defined(GT4SAVE_EXTRA_CARS)
        using ExtraPack = ps2x::iop::gt4::extracars::Pack;
        std::mutex g_extraMutex;
        std::shared_ptr<const ExtraPack> g_extra; // Gt4Disc::useExtraCars
#endif

        bool readAt(std::ifstream &f, uint64_t offset, size_t n, std::vector<uint8_t> &out)
        {
            out.assign(n, 0);
            f.clear();
            f.seekg(std::streamoff(offset), std::ios::beg);
            if (!f)
                return false;
            if (n)
                f.read(reinterpret_cast<char *>(out.data()), std::streamsize(n));
            return size_t(f.gcount()) == n;
        }

        // ------------------------------------------------------------------ spec DB file parsers
        bool parseIdi(const std::vector<uint8_t> &d, std::vector<std::pair<uint32_t, std::string>> &rows)
        {
            if (d.size() < 16 || std::memcmp(d.data(), "GTID", 4) != 0)
                return false;
            const uint32_t n = le32(d.data() + 4);
            const uint64_t base = 0x10 + uint64_t(n) * 8;
            if (base > d.size())
                return false;
            for (uint32_t i = 0; i < n; ++i)
            {
                const uint32_t off = le32(d.data() + 0x10 + 8 * size_t(i)), code = le32(d.data() + 0x14 + 8 * size_t(i));
                const uint64_t p = base + off;
                if (p + 2 > d.size())
                    return false;
                const uint16_t ln = le16(d.data() + p);
                if (p + 2 + ln > d.size())
                    return false;
                const char *s = reinterpret_cast<const char *>(d.data() + p + 2);
                rows.push_back({code, std::string(s, strnlen(s, ln))}); // split(b"\0")[0], latin-1 bytes
            }
            return true;
        }

        bool parseDbtCodes(const std::vector<uint8_t> &d, std::vector<uint32_t> &codes)
        {
            if (d.size() < 16 || std::memcmp(d.data(), "GTDB", 4) != 0)
                return false;
            const uint32_t n = le32(d.data() + 8);
            if (0x10 + uint64_t(n) * 8 > d.size())
                return false;
            for (uint32_t i = 0; i < n; ++i)
                codes.push_back(le32(d.data() + 0x10 + 8 * size_t(i)));
            return true;
        }

        // Table ids, read from each .idi header (u32 at +12) of the US disc [gt4specdb.TABLES].
        struct TableDef
        {
            uint32_t id;
            const char *name;
        };
        const TableDef kTables[] = {
            {0, "GENERIC_CAR"}, {1, "BRAKE"}, {2, "BRAKECONTROLLER"}, {3, "SUSPENSION"}, {4, "ASCC"}, {5, "TCSC"}, {6, "CHASSIS"},
            {7, "RACINGMODIFY"}, {8, "LIGHTWEIGHT"}, {9, "STEER"}, {10, "DRIVETRAIN"}, {11, "GEAR"}, {12, "ENGINE"}, {13, "NATUNE"},
            {14, "TURBINEKIT"}, {15, "PORTPOLISH"}, {16, "ENGINEBALANCE"}, {17, "DISPLACEMENT"}, {18, "COMPUTER"}, {19, "INTERCOOLER"},
            {20, "MUFFLER"}, {21, "CLUTCH"}, {22, "FLYWHEEL"}, {23, "PROPELLERSHAFT"}, {24, "LSD"}, {25, "FRONTTIRE"}, {26, "REARTIRE"},
            {27, "NOS"}, {28, "SUPERCHARGER"}, {29, "WHEEL"}, {30, "WING"}, {31, "TIRESIZE"}, {32, "TIRECOMPOUND"}, {33, "TIREFORCEVOL"},
            {34, "COURSE"}, {35, "RACE"}, {36, "DEFAULT_PARTS"}, {37, "DEFAULT_PARAM"}, {38, "ENEMY_CARS"}, {39, "CAR_NAME"}, {42, "VARIATION"}};
    }

    bool inflateRaw(const uint8_t *in, size_t inLen, std::vector<uint8_t> &out, size_t maxOut, bool *complete)
    {
        out.clear();
        if (complete)
            *complete = false;
        Inflater s{in, inLen, 0, 0, 0, out, maxOut};
        for (;;)
        {
            const int last = s.bits(1);
            const int type = s.bits(2);
            if (s.eof)
                return true; // the input ends between blocks
            bool ok = false;
            if (type == 0)
                ok = s.stored();
            else if (type == 1)
                ok = s.fixed();
            else if (type == 2)
                ok = s.dynamic();
            else
                s.corrupt = true;
            if (s.corrupt)
                return false;
            if (!ok)
                return true; // the input ends inside a block: what was decoded so far
            if (last)
            {
                if (complete)
                    *complete = true;
                return true;
            }
        }
    }

    const char *tableName(uint32_t table)
    {
        for (const TableDef &t : kTables)
            if (t.id == table)
                return t.name;
        return nullptr;
    }

    std::string tableNameOr(uint64_t code64)
    {
        const uint64_t t = code64 >> 32;
        const char *n = t <= 0xFFFFFFFFu ? tableName(uint32_t(t)) : nullptr;
        return n ? std::string(n) : "T" + std::to_string(t);
    }

    // ====================================================================== the disc
    bool Gt4Disc::useExtraCars(const fs::path &pack, std::string &error)
    {
#if defined(GT4SAVE_EXTRA_CARS)
        std::shared_ptr<const ExtraPack> p;
        if (!pack.empty())
            p = ExtraPack::load(pack, error);
        std::lock_guard lock(g_extraMutex);
        g_extra = p;
        return pack.empty() || p != nullptr;
#else
        if (pack.empty())
            return true;
        error = "this program does not read extra-cars packs";
        return false;
#endif
    }

    bool Gt4Disc::readImage(std::ifstream &f, uint64_t offset, size_t n, std::vector<uint8_t> &out) const
    {
#if defined(GT4SAVE_EXTRA_CARS)
        if (pack_)
        {
            const ExtraPack *p = static_cast<const ExtraPack *>(pack_.get());
            if (offset + n > std::max(imageBytes_, p->extEnd()))
                return false;
            out.assign(n, 0);
            if (offset < imageBytes_)
            {
                std::vector<uint8_t> part;
                const size_t m = size_t(std::min<uint64_t>(n, imageBytes_ - offset));
                if (!readAt(f, offset, m, part))
                    return false;
                std::memcpy(out.data(), part.data(), m);
            }
            p->apply(offset, out.data(), n);
            return true;
        }
#endif
        return readAt(f, offset, n, out);
    }

    std::unique_ptr<Gt4Disc> Gt4Disc::open(const fs::path &iso, std::string &error)
    {
        std::ifstream f(iso, std::ios::binary);
        if (!f)
        {
            error = "cannot open the disc image";
            return nullptr;
        }
        std::unique_ptr<Gt4Disc> disc(new Gt4Disc());
        disc->iso_ = iso;
        f.seekg(0, std::ios::end);
        disc->imageBytes_ = uint64_t(std::streamoff(f.tellg()));
        f.clear();
        std::vector<uint8_t> pvd;
        if (!readAt(f, 16ull * kSector, kSector, pvd) || pvd[0] != 1 || std::memcmp(pvd.data() + 1, "CD001", 5) != 0)
        {
            error = "not an ISO 9660 disc image";
            return nullptr;
        }
        const uint32_t rootExtent = le32(pvd.data() + 156 + 2), rootSize = le32(pvd.data() + 156 + 10);
        std::vector<uint8_t> root;
        if (rootSize > (1u << 20) || !readAt(f, uint64_t(rootExtent) * kSector, rootSize, root))
        {
            error = "cannot read the disc's root folder";
            return nullptr;
        }
        uint64_t volLba = 0;
        bool found = false;
        for (size_t off = 0; off < root.size();)
        {
            const uint8_t n = root[off];
            if (n == 0)
            {
                off = (off / kSector + 1) * kSector;
                continue;
            }
            if (off + n > root.size() || n < 34)
                break;
            const uint8_t *rec = root.data() + off;
            std::string name(reinterpret_cast<const char *>(rec + 33), std::min<size_t>(rec[32], n - 33));
            for (char &ch : name)
                ch = char(std::toupper(static_cast<unsigned char>(ch)));
            if ((name == "GT4.VOL;1" || name == "GT4.VOL") && !(rec[25] & 2))
            {
                volLba = le32(rec + 2);
                found = true;
                break;
            }
            off += n;
        }
        if (!found)
        {
            error = "GT4.VOL is not on this disc (not the GT4 disc?)";
            return nullptr;
        }
        // The real RoFS header (the VOL starts with a decoy GT3-style table of contents).
        const uint64_t headerLba = volLba + kTocPage;
#if defined(GT4SAVE_EXTRA_CARS)
        // GT4_EXTRA_CARS: the pack is served only over the US disc it was built from (as GT4.exe checks it).
        std::shared_ptr<const ExtraPack> extra;
        {
            std::lock_guard lock(g_extraMutex);
            extra = g_extra;
        }
        if (extra)
        {
            std::string why;
            if (extra->matches(disc->imageBytes_,
                               [&](uint64_t at, void *dst, size_t n) {
                                   std::vector<uint8_t> b;
                                   if (!readAt(f, at, n, b))
                                       return false;
                                   std::memcpy(dst, b.data(), n);
                                   return true;
                               },
                               why))
            {
                disc->pack_ = extra;
                disc->extraCars = extra->name();
            }
            else
                disc->extraCarsNote = why;
        }
#endif
        std::vector<uint8_t> head;
        if (!disc->readImage(f, headerLba * kSector, 0x40, head) || le32(head.data()) != kRofsMagic)
        {
            error = "GT4.VOL's table of contents was not found";
            return nullptr;
        }
        const int32_t totalPages = int32_t(le32(head.data() + 12));
        const uint16_t tocPages = le16(head.data() + 0x12);
        std::vector<uint8_t> enc;
        if (!disc->readImage(f, headerLba * kSector + 0x40, 4 * (size_t(tocPages) + 1), enc))
        {
            error = "GT4.VOL's table of contents cannot be read";
            return nullptr;
        }
        std::vector<uint32_t> offs(size_t(tocPages) + 1);
        for (size_t i = 0; i <= tocPages; ++i)
            offs[i] = le32(enc.data() + 4 * i) ^ (uint32_t(i) * kTocKey + kTocKey);
        std::vector<uint8_t> blob;
        if (offs.back() > (64u << 20) || !disc->readImage(f, headerLba * kSector, offs.back(), blob))
        {
            error = "GT4.VOL's table of contents cannot be read";
            return nullptr;
        }
        struct TocEntry
        {
            uint32_t parent;
            std::string name;
            uint8_t type;
            std::vector<int32_t> fields;
        };
        std::vector<TocEntry> entries;
        std::vector<uint8_t> page;
        for (size_t i = 0; i < tocPages; ++i)
        {
            if (offs[i] > offs[i + 1] || offs[i + 1] > blob.size() || !inflateRaw(blob.data() + offs[i], offs[i + 1] - offs[i], page, 1u << 20))
            {
                error = "GT4.VOL's table of contents is damaged (page " + std::to_string(i) + ")";
                return nullptr;
            }
            for (uint8_t &b : page)
                b ^= 0x55;
            if (page.size() < 4)
                continue;
            const uint16_t isIndex = le16(page.data()), count = le16(page.data() + 2);
            if (isIndex)
                continue;
            size_t pos = page.size();
            for (int k = 0; k < count / 2; ++k)
            {
                if (pos < 8)
                    break;
                pos -= 8;
                const uint16_t ko = le16(page.data() + pos), kl = le16(page.data() + pos + 2), vo = le16(page.data() + pos + 4), vl = le16(page.data() + pos + 6);
                if (size_t(ko) + kl > page.size() || size_t(vo) + vl > page.size() || kl < 4 || vl < 1)
                    continue;
                TocEntry e;
                e.parent = be32(page.data() + ko);
                e.name.assign(reinterpret_cast<const char *>(page.data() + ko + 4), kl - 4);
                e.type = page[vo];
                for (size_t q = 0; q + 4 <= size_t(vl) - 1; q += 4)
                    e.fields.push_back(int32_t(le32(page.data() + vo + 1 + q)));
                entries.push_back(std::move(e));
            }
        }
        std::map<uint32_t, std::pair<uint32_t, std::string>> dirs;
        for (const TocEntry &e : entries)
            if (e.type == 0 && !e.fields.empty())
                dirs[uint32_t(e.fields[0])] = {e.parent, e.name};
        auto full = [&](uint32_t parent, const std::string &name) {
            std::vector<std::string> parts{name};
            for (int guard = 0; guard < 64; ++guard)
            {
                const auto it = dirs.find(parent);
                if (it == dirs.end())
                    break;
                parts.push_back(it->second.second);
                parent = it->second.first;
            }
            std::string p; // "/".join(reversed(parts)).lstrip("./")
            for (size_t i = parts.size(); i-- > 0;)
                p += (i + 1 == parts.size() ? "" : "/") + parts[i];
            const size_t start = p.find_first_not_of("./");
            return start == std::string::npos ? std::string() : p.substr(start);
        };
        const uint64_t dataStart = headerLba + uint64_t(int64_t(totalPages));
        for (const TocEntry &e : entries)
        {
            if (!((e.type == 1 && e.fields.size() >= 3) || (e.type == 2 && e.fields.size() >= 4)))
                continue;
            VolFile v;
            v.compressed = e.type == 2;
            v.lba = dataStart + uint64_t(int64_t(e.fields[0]));
            v.stored = uint32_t(e.fields[2]);
            v.real = v.compressed ? uint32_t(e.fields[3]) : v.stored;
            const std::string path = full(e.parent, e.name);
            const auto it = disc->files_.find(path);
            if (it == disc->files_.end() || it->second.lba <= v.lba) // the CSV the Python tool reads keeps the last by LBA
                disc->files_[path] = v;
        }
        if (disc->files_.empty())
        {
            error = "GT4.VOL lists no files";
            return nullptr;
        }
        return disc;
    }

    bool Gt4Disc::read(const std::string &path, std::vector<uint8_t> &out, std::string &error) const
    {
        const auto it = files_.find(path);
        if (it == files_.end())
        {
            error = path + " is not in GT4.VOL";
            return false;
        }
        std::ifstream f(iso_, std::ios::binary);
        std::vector<uint8_t> raw;
        if (!f || !readImage(f, it->second.lba * kSector, it->second.stored, raw))
        {
            error = "cannot read " + path + " from the disc image";
            return false;
        }
        if (!it->second.compressed)
        {
            out = std::move(raw);
            return true;
        }
        static const uint8_t magic[4] = {0xC5, 0xEE, 0xF7, 0xFF};
        if (raw.size() < 8 || std::memcmp(raw.data(), magic, 4) != 0 || !inflateRaw(raw.data() + 8, raw.size() - 8, out))
        {
            error = path + " does not unpack";
            return false;
        }
        return true;
    }

    // ====================================================================== spec database
    template <class Get>
    std::unique_ptr<SpecDb> SpecDb::loadWith(Get &&get, std::string source)
    {
        std::unique_ptr<SpecDb> db(new SpecDb());
        db->source = std::move(source);
        std::vector<uint8_t> d;
        std::vector<std::pair<uint32_t, std::string>> rows;
        std::vector<uint32_t> raceFirstOrder; // RACE codes in first-insertion order (Python dict order)
        for (const TableDef &t : kTables)
        {
            if (t.id >= 36 && t.id <= 39)
                continue; // DEFAULT_PARTS, DEFAULT_PARAM, ENEMY_CARS, CAR_NAME: no labels loaded (as gt4specdb.py)
            const std::string file = t.id == 42 ? "VARIATIONamerican" : t.name;
            rows.clear();
            if (!get(file + ".idi", d) || !parseIdi(d, rows))
                continue;
            std::map<uint32_t, std::string> &lab = db->labels[t.id];
            lab.clear();
            for (auto &[code, label] : rows)
            {
                if (t.id == 35 && !lab.count(code))
                    raceFirstOrder.push_back(code);
                lab[code] = label; // a later duplicate replaces the label (dict assignment)
            }
        }
        if (get("RACE.dbt", d))
        {
            std::vector<uint32_t> codes;
            if (parseDbtCodes(d, codes))
                db->raceRows = std::move(codes);
        }
        // VARIATION.dbt holds one row per colour, grouped by the car's variation code.
        if (get("VARIATIONamerican.dbt", d))
        {
            std::vector<uint32_t> codes;
            if (parseDbtCodes(d, codes))
                for (uint32_t c : codes)
                    db->colourCount[c]++;
        }
        const auto race = db->labels.find(35);
        if (race != db->labels.end())
            for (uint32_t code : raceFirstOrder)
                db->raceInv_[race->second.at(code)] = code; // {v: k for k, v in labels[35].items()}
        return db;
    }

    std::unique_ptr<SpecDb> SpecDb::loadFromIso(const fs::path &iso, std::string &error)
    {
        std::unique_ptr<Gt4Disc> disc = Gt4Disc::open(iso, error);
        if (!disc)
            return nullptr;
        const auto u = iso.u8string();
        std::string err;
        return loadWith([&](const std::string &name, std::vector<uint8_t> &out) { return disc->read(kSpecDir + name, out, err); },
                        std::string(u.begin(), u.end()));
    }

    std::unique_ptr<SpecDb> SpecDb::loadFromFolder(const fs::path &dir, std::string &error)
    {
        std::error_code ec;
        if (!fs::is_directory(dir, ec))
        {
            error = "the spec database folder does not exist";
            return nullptr;
        }
        const auto u = dir.u8string();
        return loadWith(
            [&](const std::string &name, std::vector<uint8_t> &out) {
                std::ifstream f(dir / fs::path(name), std::ios::binary);
                if (!f)
                    return false;
                out.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
                return true;
            },
            std::string(u.begin(), u.end()));
    }

    std::optional<std::string> SpecDb::label(uint64_t code64) const
    {
        if (code64 == kNone64)
            return std::nullopt;
        const uint64_t tid = code64 >> 32;
        const uint32_t code = uint32_t(code64 & 0xFFFFFFFFu);
        const auto t = labels.find(uint32_t(tid));
        if (t != labels.end() && tid <= 0xFFFFFFFFu)
        {
            const auto it = t->second.find(code);
            if (it != t->second.end())
                return it->second;
        }
        return "#" + std::to_string(code);
    }

    std::optional<int> SpecDb::colours(std::optional<uint64_t> v) const
    {
        if (!v || *v == kNone64 || (*v >> 32) != 42)
            return std::nullopt;
        const auto it = colourCount.find(uint32_t(*v & 0xFFFFFFFFu));
        return it == colourCount.end() ? std::nullopt : std::optional<int>(it->second);
    }

    std::optional<std::string> SpecDb::raceLabel(int row) const
    {
        if (row < 0 || size_t(row) >= raceRows.size())
            return std::nullopt;
        const uint32_t code = raceRows[size_t(row)];
        const auto t = labels.find(35);
        if (t != labels.end())
        {
            const auto it = t->second.find(code);
            if (it != t->second.end())
                return it->second;
        }
        return "#" + std::to_string(code);
    }

    std::optional<int> SpecDb::raceRow(const std::string &label) const
    {
        for (const std::string &cand : {label, "us" + label, "jp" + label})
        {
            const auto it = raceInv_.find(cand);
            if (it == raceInv_.end())
                continue;
            for (size_t i = 0; i < raceRows.size(); ++i)
                if (raceRows[i] == it->second)
                    return int(i);
        }
        return std::nullopt;
    }

    bool SpecDb::hasTable(uint32_t table) const
    {
        const auto t = labels.find(table);
        return t != labels.end() && !t->second.empty();
    }

    bool SpecDb::hasCode(uint32_t table, uint32_t code) const
    {
        const auto t = labels.find(table);
        return t != labels.end() && t->second.count(code) != 0;
    }
}
