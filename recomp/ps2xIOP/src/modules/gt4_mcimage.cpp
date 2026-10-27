#include "gt4_mcimage.h"

// GT4 .ps2 memory card image. MCMAN references are module offsets in
// analysis/iop-sys/disasm/gt4/MCMAN.txt (GT4's MCMAN.IRX, identity 0x230):
//   open 0x6bc4 (create path 0x7138-0x75d8), close/flush 0xd20/0xee0 -> 0x6124, read 0x1188 -> 0x5cfc,
//   write 0x1340 -> 0x5eb4, seek 0x10b0, getdir 0x80fc, delete 0x85c0 -> 0x7e30, setinfo 0x5940,
//   getentspace 0x6268, new directory 0x4be4, format 0x44ec, allocatable end (probe) 0x43a0-0x44c0,
//   name check 0x3cb0.

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <filesystem>

namespace ps2x::iop::gt4::mcimage
{
    namespace
    {
        constexpr char Magic[] = "Sony PS2 Memory Card Format ";
        constexpr char Version[] = "1.2.0.0";
        constexpr uint32_t ChainEnd = 0xffffffffu;
        constexpr uint32_t FreeEntry = 0x7fffffffu;
        constexpr uint32_t InUse = 0x80000000u;
        constexpr uint32_t EntriesPerCluster = ClusterBytes / 4; // FAT entries per cluster
        constexpr uint32_t FirstIfc = 8;                          // 0x2000 / 1024
        constexpr uint32_t ClustersPerBlock = PagesPerBlock / PagesPerCluster;
        constexpr uint32_t SubdirSlotLimit = 20; // MCMAN 0x6e10: no new entry at slot >= 20 outside the root

        uint16_t rd16(const uint8_t *p) { return uint16_t(p[0] | (p[1] << 8)); }
        uint32_t rd32(const uint8_t *p) { return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24); }
        void wr16(uint8_t *p, uint16_t v) { p[0] = uint8_t(v); p[1] = uint8_t(v >> 8); }
        void wr32(uint8_t *p, uint32_t v)
        {
            p[0] = uint8_t(v);
            p[1] = uint8_t(v >> 8);
            p[2] = uint8_t(v >> 16);
            p[3] = uint8_t(v >> 24);
        }

        struct EccTables
        {
            uint8_t parity[256];
            uint8_t columnMask[256];
            EccTables()
            {
                static const uint8_t masks[7] = {0x55, 0x33, 0x0f, 0x00, 0xaa, 0xcc, 0xf0};
                for (int b = 0; b < 256; ++b)
                {
                    int x = b;
                    x ^= x >> 4;
                    x ^= x >> 2;
                    x ^= x >> 1;
                    parity[b] = uint8_t(x & 1);
                }
                for (int b = 0; b < 256; ++b)
                {
                    uint8_t m = 0;
                    for (int i = 0; i < 7; ++i)
                        m = uint8_t(m | (parity[b & masks[i]] << i));
                    columnMask[b] = m;
                }
            }
        };
        const EccTables &eccTables()
        {
            static const EccTables tables;
            return tables;
        }

        std::FILE *openFile(const std::string &path, const char *mode)
        {
#ifdef _WIN32
            // Narrow paths are the ANSI code page, as std::filesystem::path reads them elsewhere.
            const std::wstring wmode(mode, mode + std::strlen(mode));
            std::FILE *f = nullptr;
            return _wfopen_s(&f, std::filesystem::path(path).c_str(), wmode.c_str()) == 0 ? f : nullptr;
#else
            return std::fopen(path.c_str(), mode);
#endif
        }

        std::string nameOf(const uint8_t *p, size_t max)
        {
            size_t n = 0;
            while (n < max && p[n])
                ++n;
            return std::string(reinterpret_cast<const char *>(p), n);
        }

        // Lexical path split; "." dropped, ".." pops (the card has one directory level under root).
        std::vector<std::string> splitPath(std::string_view path)
        {
            std::vector<std::string> parts;
            std::string cur;
            auto push = [&]
            {
                if (cur.empty() || cur == ".")
                {
                }
                else if (cur == "..")
                {
                    if (!parts.empty())
                        parts.pop_back();
                }
                else
                    parts.push_back(cur);
                cur.clear();
            };
            for (char c : path)
            {
                if (c == '/' || c == '\\')
                    push();
                else
                    cur.push_back(c);
            }
            push();
            return parts;
        }

        // MCMAN's pattern match (0x528): '*' any run, '?' one character, otherwise exact.
        bool match(std::string_view pattern, std::string_view name)
        {
            size_t p = 0, n = 0, star = std::string_view::npos, mark = 0;
            while (n < name.size())
            {
                if (p < pattern.size() && (pattern[p] == '?' || pattern[p] == name[n]))
                {
                    ++p;
                    ++n;
                }
                else if (p < pattern.size() && pattern[p] == '*')
                {
                    star = p++;
                    mark = n;
                }
                else if (star != std::string_view::npos)
                {
                    p = star + 1;
                    n = ++mark;
                }
                else
                    return false;
            }
            while (p < pattern.size() && pattern[p] == '*')
                ++p;
            return p == pattern.size();
        }
    }

    // ---------------------------------------------------------------- small types

    void Tod::store(uint8_t *p) const
    {
        p[0] = 0;
        p[1] = sec;
        p[2] = min;
        p[3] = hour;
        p[4] = day;
        p[5] = month;
        wr16(p + 6, year);
    }

    Tod Tod::load(const uint8_t *p)
    {
        Tod t;
        t.sec = p[1];
        t.min = p[2];
        t.hour = p[3];
        t.day = p[4];
        t.month = p[5];
        t.year = rd16(p + 6);
        return t;
    }

    void DirInfo::toTable(uint8_t out[64]) const
    {
        std::memset(out, 0, 64);
        created.store(out);
        modified.store(out + 8);
        wr32(out + 0x10, size);
        wr16(out + 0x14, mode);
        wr16(out + 0x16, unused);
        wr32(out + 0x18, attr);
        std::memcpy(out + 0x20, name.data(), std::min<size_t>(name.size(), 32));
    }

    DirInfo DirInfo::fromTable(const uint8_t in[64])
    {
        DirInfo d;
        d.created = Tod::load(in);
        d.modified = Tod::load(in + 8);
        d.size = rd32(in + 0x10);
        d.mode = rd16(in + 0x14);
        d.unused = rd16(in + 0x16);
        d.attr = rd32(in + 0x18);
        d.name = nameOf(in + 0x20, 32);
        return d;
    }

    void ecc128(const uint8_t *chunk, uint8_t out[3])
    {
        const EccTables &t = eccTables();
        uint8_t column = 0x77, line0 = 0x7f, line1 = 0x7f;
        for (uint32_t i = 0; i < 128; ++i)
        {
            const uint8_t b = chunk[i];
            column ^= t.columnMask[b];
            if (t.parity[b])
            {
                line0 ^= uint8_t(~i);
                line1 ^= uint8_t(i);
            }
        }
        out[0] = column;
        out[1] = uint8_t(line0 & 0x7f);
        out[2] = uint8_t(line1 & 0x7f);
    }

    void eccPage(const uint8_t *page, uint8_t spare[SpareBytes])
    {
        for (uint32_t c = 0; c < 4; ++c)
            ecc128(page + c * 128, spare + c * 3);
        std::memset(spare + 12, 0, SpareBytes - 12);
    }

    Tod todFromUnix(int64_t seconds)
    {
        const std::time_t t = static_cast<std::time_t>(seconds + 9 * 3600);
        std::tm tm{};
#ifdef _WIN32
        gmtime_s(&tm, &t);
#else
        gmtime_r(&t, &tm);
#endif
        Tod d;
        d.sec = uint8_t(tm.tm_sec);
        d.min = uint8_t(tm.tm_min);
        d.hour = uint8_t(tm.tm_hour);
        d.day = uint8_t(tm.tm_mday);
        d.month = uint8_t(tm.tm_mon + 1);
        d.year = uint16_t(tm.tm_year + 1900);
        return d;
    }

    Tod hostTimeJst() { return todFromUnix(static_cast<int64_t>(std::time(nullptr))); }

    // ---------------------------------------------------------------- image

    Card::Card(Clock clock) : clock_(clock ? std::move(clock) : Clock(hostTimeJst))
    {
        createUnformatted();
        savedPath_.clear();
    }

    void Card::touchPage(uint32_t page)
    {
        pageDirty_[page] = PageWritten;
        anyDirty_ = true;
    }

    void Card::readCluster(uint32_t absolute, uint8_t *out) const
    {
        std::memcpy(out, pageData(absolute * PagesPerCluster), ClusterBytes);
    }

    void Card::writeCluster(uint32_t absolute, const uint8_t *in)
    {
        std::memcpy(pageData(absolute * PagesPerCluster), in, ClusterBytes);
        touchPage(absolute * PagesPerCluster);
        touchPage(absolute * PagesPerCluster + 1);
    }

    void Card::createUnformatted()
    {
        data_.assign(size_t(PagesPerCard) * PageBytes, 0xff);
        spare_.assign(size_t(PagesPerCard) * SpareBytes, 0xff);
        pageDirty_.assign(PagesPerCard, PageErased);
        anyDirty_ = true;
        closeAll();
        listValid_ = false;
        parseSuperblock();
    }

    void Card::createFormatted()
    {
        // MCMAN's format (0x44ec): the card is erased, then the superblock (MCMAN's in-memory
        // device info, 0x184 bytes, as on a PCSX2 card the PS2 BIOS formatted), the indirect FAT,
        // the FAT and the root directory are written. Pages 1-15 are written as 0xFF too.
        createUnformatted();
        uint8_t *sb = pageData(0);
        std::memcpy(sb, Magic, 28);
        std::memset(sb + 0x1c, 0, 12);
        std::memcpy(sb + 0x1c, Version, sizeof(Version) - 1);
        wr16(sb + 0x28, PageBytes);
        wr16(sb + 0x2a, PagesPerCluster);
        wr16(sb + 0x2c, PagesPerBlock);
        wr16(sb + 0x2e, 0xff00);
        wr32(sb + 0x30, ClustersPerCard);
        const uint32_t fatClusters = (ClustersPerCard + EntriesPerCluster - 1) / EntriesPerCluster; // 32
        const uint32_t allocOffset = FirstIfc + 1 + fatClusters;                                  // 41
        const uint32_t blocks = ClustersPerCard / ClustersPerBlock;                               // 1024
        const uint32_t allocEnd = (blocks - 2) * ClustersPerBlock - allocOffset;                  // 8135
        wr32(sb + 0x34, allocOffset);
        wr32(sb + 0x38, allocEnd);
        wr32(sb + 0x3c, 0);          // root directory cluster
        wr32(sb + 0x40, blocks - 1); // backup block 1
        wr32(sb + 0x44, blocks - 2); // backup block 2
        std::memset(sb + 0x48, 0, 8);
        std::memset(sb + 0x50, 0, 128);
        wr32(sb + 0x50, FirstIfc);
        std::memset(sb + 0xd0, 0xff, 128); // no bad blocks
        sb[0x150] = 2;                     // PS2 card
        sb[0x151] = 0x2b;                  // card flags PCSX2 reports (ECC, bad-block list)
        wr16(sb + 0x152, 0);
        wr32(sb + 0x154, ClusterBytes);
        wr32(sb + 0x158, EntriesPerCluster);
        wr32(sb + 0x15c, ClustersPerBlock);
        wr32(sb + 0x160, 0xffffffffu);
        wr32(sb + 0x164, 0);
        wr32(sb + 0x168, 0);
        wr32(sb + 0x16c, 0);
        wr32(sb + 0x170, (ClustersPerCard / 1000) * 1000 + 1); // 8001, MCMAN 0x49a0-0x4a50
        wr32(sb + 0x174, 0);
        wr32(sb + 0x178, 0);
        std::memset(sb + 0x17c, 0xff, PageBytes - 0x17c);
        for (uint32_t p = 0; p < 16; ++p)
            touchPage(p);

        // Indirect FAT (cluster 8) lists the FAT clusters 9..40.
        std::vector<uint8_t> buf(ClusterBytes, 0xff);
        for (uint32_t i = 0; i < fatClusters; ++i)
            wr32(buf.data() + i * 4, FirstIfc + 1 + i);
        writeCluster(FirstIfc, buf.data());
        // FAT: cluster 0 is the root (end of chain), the allocatable range free, the rest unusable.
        for (uint32_t f = 0; f < fatClusters; ++f)
        {
            for (uint32_t i = 0; i < EntriesPerCluster; ++i)
            {
                const uint32_t n = f * EntriesPerCluster + i;
                wr32(buf.data() + i * 4, n == 0 ? ChainEnd : (n < allocEnd ? FreeEntry : ChainEnd));
            }
            writeCluster(FirstIfc + 1 + f, buf.data());
        }
        parseSuperblock();

        // Root directory: "." (length 2) and ".." (hidden), MCMAN 0x4be4 with cluster 0 / slot 0.
        std::fill(buf.begin(), buf.end(), uint8_t(0));
        const Tod now = clock_();
        wr16(buf.data(), ModeExists | Mode0400 | ModeDir | ModeRead | ModeWrite | ModeExecute); // 0x8427
        wr32(buf.data() + 4, 2);
        now.store(buf.data() + 8);
        now.store(buf.data() + 0x18);
        buf[0x40] = '.';
        uint8_t *dd = buf.data() + EntryBytes;
        wr16(dd, ModeExists | ModeHidden | Mode0400 | ModeDir | ModeWrite | ModeExecute); // 0xa426
        now.store(dd + 8);
        now.store(dd + 0x18);
        dd[0x40] = '.';
        dd[0x41] = '.';
        writeCluster(allocOffset_ + rootCluster_, buf.data());
    }

    void Card::parseSuperblock()
    {
        const uint8_t *sb = pageData(0);
        formatted_ = std::memcmp(sb, Magic, 28) == 0 && rd16(sb + 0x28) == PageBytes &&
                     rd16(sb + 0x2a) == PagesPerCluster && rd16(sb + 0x2c) == PagesPerBlock &&
                     rd32(sb + 0x30) == ClustersPerCard && sb[0x150] == 2;
        hint_ = 0;
        if (!formatted_)
            return;
        allocOffset_ = rd32(sb + 0x34);
        allocEnd_ = rd32(sb + 0x38);
        rootCluster_ = rd32(sb + 0x3c);
        backup1_ = rd32(sb + 0x40);
        backup2_ = rd32(sb + 0x44);
        for (uint32_t i = 0; i < 32; ++i)
            ifc_[i] = rd32(sb + 0x50 + i * 4);
        if (allocOffset_ >= ClustersPerCard || allocEnd_ == 0 || allocOffset_ + allocEnd_ > ClustersPerCard ||
            ifc_[0] >= ClustersPerCard || rootCluster_ >= allocEnd_)
        {
            formatted_ = false;
            return;
        }
        // The allocatable end MCMAN uses at run time (probe, 0x43a0-0x44c0): count good clusters from
        // the first allocatable one until (clusters/1000)*1000+1 are found, skipping bad blocks.
        std::array<int32_t, 16> bad{};
        for (uint32_t i = 0; i < 16; ++i)
            bad[i] = int32_t(rd32(sb + 0xd0 + i * 4));
        const uint32_t target = (ClustersPerCard / 1000) * 1000 + 1; // 8001
        uint32_t good = 0, c = allocOffset_;
        const uint32_t end = allocOffset_ + allocEnd_;
        for (; c < end && good < target; ++c)
        {
            const int32_t block = int32_t(c / ClustersPerBlock);
            if (std::find(bad.begin(), bad.end(), block) == bad.end())
                ++good;
        }
        maxAlloc_ = c - allocOffset_;
    }

    bool Card::loadBytes(const std::vector<uint8_t> &raw, std::string &error)
    {
        if (raw.size() != ImageBytes)
        {
            error = "not an 8 MB PS2 memory card image with ECC (" + std::to_string(raw.size()) + " bytes, expected " +
                    std::to_string(ImageBytes) + ")";
            return false;
        }
        data_.resize(size_t(PagesPerCard) * PageBytes);
        spare_.resize(size_t(PagesPerCard) * SpareBytes);
        for (uint32_t p = 0; p < PagesPerCard; ++p)
        {
            std::memcpy(pageData(p), raw.data() + size_t(p) * RawPageBytes, PageBytes);
            std::memcpy(spare_.data() + size_t(p) * SpareBytes, raw.data() + size_t(p) * RawPageBytes + PageBytes, SpareBytes);
        }
        pageDirty_.assign(PagesPerCard, 0);
        anyDirty_ = false;
        closeAll();
        listValid_ = false;
        parseSuperblock();
        return true;
    }

    bool Card::loadFile(const std::string &path, std::string &error)
    {
        std::FILE *f = openFile(path, "rb");
        if (!f)
        {
            error = "cannot open " + path;
            return false;
        }
        std::vector<uint8_t> raw;
        std::fseek(f, 0, SEEK_END);
        const long size = std::ftell(f);
        std::fseek(f, 0, SEEK_SET);
        if (size >= 0 && uint64_t(size) != ImageBytes)
        {
            // Only the 8 MB size is read (a 16/32/64 MB PCSX2 card is reported with its real size).
            std::fclose(f);
            error = path + ": not an 8 MB PS2 memory card image with ECC (" + std::to_string(size) + " bytes, expected " +
                    std::to_string(ImageBytes) + ")";
            return false;
        }
        if (size > 0)
        {
            raw.resize(size_t(size));
            if (std::fread(raw.data(), 1, raw.size(), f) != raw.size())
                raw.clear();
        }
        std::fclose(f);
        if (!loadBytes(raw, error))
        {
            error = path + ": " + error;
            return false;
        }
        savedPath_ = path;
        return true;
    }

    bool Card::saveFile(const std::string &path, std::string &error)
    {
        const bool whole = path != savedPath_;
        if (!whole && !anyDirty_)
            return true;
        std::FILE *f = openFile(path, whole ? "wb" : "r+b");
        if (!f)
        {
            error = "cannot write " + path;
            return false;
        }
        std::vector<uint8_t> run;
        std::vector<std::pair<uint32_t, uint32_t>> written; // page runs [first, end) sent to the file
        bool ok = true;
        uint32_t p = 0;
        while (p < PagesPerCard && ok)
        {
            if (!whole && !pageDirty_[p])
            {
                ++p;
                continue;
            }
            const uint32_t first = p;
            run.clear();
            while (p < PagesPerCard && (whole || pageDirty_[p]))
            {
                uint8_t *sp = spare_.data() + size_t(p) * SpareBytes;
                // A written page gets its ECC; an erased one (never written since) keeps 0xFF data and spare.
                if (pageDirty_[p] == PageWritten)
                    eccPage(pageData(p), sp);
                else if (pageDirty_[p] == PageErased)
                    std::memset(sp, 0xff, SpareBytes);
                run.insert(run.end(), pageData(p), pageData(p) + PageBytes);
                run.insert(run.end(), sp, sp + SpareBytes);
                ++p;
                if (run.size() >= size_t(256) * RawPageBytes)
                    break;
            }
            ok = std::fseek(f, long(first) * long(RawPageBytes), SEEK_SET) == 0 &&
                 std::fwrite(run.data(), 1, run.size(), f) == run.size();
            written.emplace_back(first, p);
        }
        ok = (std::fclose(f) == 0) && ok;
        if (!ok)
        {
            // Nothing is marked saved: every changed page stays changed and the next save writes it
            // again (a failed whole-image save is redone whole, as savedPath_ is not updated).
            error = "write failed: " + path;
            return false;
        }
        for (const auto &[from, to] : written)
            std::fill(pageDirty_.begin() + from, pageDirty_.begin() + to, uint8_t(PageClean));
        anyDirty_ = std::any_of(pageDirty_.begin(), pageDirty_.end(), [](uint8_t d) { return d != 0; });
        savedPath_ = path;
        return true;
    }

    std::vector<uint8_t> Card::rawImage() const
    {
        std::vector<uint8_t> raw(ImageBytes);
        for (uint32_t p = 0; p < PagesPerCard; ++p)
        {
            uint8_t *out = raw.data() + size_t(p) * RawPageBytes;
            std::memcpy(out, pageData(p), PageBytes);
            if (pageDirty_[p] == PageWritten)
                eccPage(pageData(p), out + PageBytes);
            else if (pageDirty_[p] == PageErased)
                std::memset(out + PageBytes, 0xff, SpareBytes);
            else
                std::memcpy(out + PageBytes, spare_.data() + size_t(p) * SpareBytes, SpareBytes);
        }
        return raw;
    }

    // ---------------------------------------------------------------- FAT

    uint32_t Card::fat(uint32_t n) const
    {
        const uint32_t ifcIndex = n / (EntriesPerCluster * EntriesPerCluster);
        if (ifcIndex >= 32 || ifc_[ifcIndex] >= ClustersPerCard)
            return ChainEnd;
        const uint32_t slot = (n / EntriesPerCluster) % EntriesPerCluster;
        const uint32_t fatCluster = rd32(pageData(ifc_[ifcIndex] * PagesPerCluster + (slot * 4) / PageBytes) + (slot * 4) % PageBytes);
        if (fatCluster >= ClustersPerCard)
            return ChainEnd;
        const uint32_t offset = (n % EntriesPerCluster) * 4;
        return rd32(pageData(fatCluster * PagesPerCluster + offset / PageBytes) + offset % PageBytes);
    }

    void Card::setFat(uint32_t n, uint32_t value)
    {
        const uint32_t ifcIndex = n / (EntriesPerCluster * EntriesPerCluster);
        if (ifcIndex >= 32 || ifc_[ifcIndex] >= ClustersPerCard)
            return;
        const uint32_t slot = (n / EntriesPerCluster) % EntriesPerCluster;
        const uint32_t fatCluster = rd32(pageData(ifc_[ifcIndex] * PagesPerCluster + (slot * 4) / PageBytes) + (slot * 4) % PageBytes);
        if (fatCluster >= ClustersPerCard)
            return;
        const uint32_t offset = (n % EntriesPerCluster) * 4;
        const uint32_t page = fatCluster * PagesPerCluster + offset / PageBytes;
        wr32(pageData(page) + offset % PageBytes, value);
        touchPage(page);
    }

    bool Card::chain(uint32_t first, std::vector<uint32_t> &out, size_t limit) const
    {
        out.clear();
        uint32_t c = first;
        while (c != ChainEnd)
        {
            if (c >= allocEnd_ || out.size() >= limit)
                return false;
            out.push_back(c);
            const uint32_t v = fat(c);
            if (v == ChainEnd)
                break;
            if (!(v & InUse))
                return false; // a free entry inside a chain
            c = v & ~InUse;
        }
        return true;
    }

    int32_t Card::allocate(uint32_t &cluster)
    {
        // MCMAN 0x51d0 (reserve): the first free entry from the hint up to the allocatable end.
        for (uint32_t n = hint_; n < maxAlloc_; ++n)
        {
            if (!(fat(n) & InUse))
            {
                setFat(n, ChainEnd);
                hint_ = n + 1;
                cluster = n;
                return Ok;
            }
        }
        hint_ = maxAlloc_;
        return Full;
    }

    int32_t Card::freeClusters() const
    {
        if (!formatted_)
            return 0;
        int32_t count = 0;
        for (uint32_t n = 0; n < maxAlloc_; ++n)
            if (!(fat(n) & InUse))
                ++count;
        return count;
    }

    // ---------------------------------------------------------------- entries

    bool Card::entryOffset(const Loc &loc, uint32_t &absCluster, uint32_t &offset) const
    {
        uint32_t c = loc.cluster;
        if (c >= allocEnd_)
            return false;
        for (uint32_t step = loc.index / 2; step; --step)
        {
            const uint32_t v = fat(c);
            if (v == ChainEnd || !(v & InUse))
                return false;
            c = v & ~InUse;
            if (c >= allocEnd_)
                return false;
        }
        absCluster = allocOffset_ + c;
        offset = (loc.index % 2) * EntryBytes;
        return true;
    }

    Entry Card::readEntry(const Loc &loc) const
    {
        Entry e;
        uint32_t abs = 0, off = 0;
        if (!entryOffset(loc, abs, off))
            return e;
        const uint8_t *p = pageData(abs * PagesPerCluster + off / PageBytes);
        e.mode = rd16(p);
        e.unused = rd16(p + 2);
        e.length = rd32(p + 4);
        e.created = Tod::load(p + 8);
        e.cluster = rd32(p + 0x10);
        e.dirEntry = rd32(p + 0x14);
        e.modified = Tod::load(p + 0x18);
        e.attr = rd32(p + 0x20);
        e.name = nameOf(p + 0x40, 32);
        return e;
    }

    void Card::writeEntry(const Loc &loc, const Entry &e)
    {
        uint32_t abs = 0, off = 0;
        if (!entryOffset(loc, abs, off))
            return;
        const uint32_t page = abs * PagesPerCluster + off / PageBytes;
        uint8_t *p = pageData(page);
        wr16(p, e.mode);
        wr16(p + 2, e.unused);
        wr32(p + 4, e.length);
        e.created.store(p + 8);
        wr32(p + 0x10, e.cluster);
        wr32(p + 0x14, e.dirEntry);
        e.modified.store(p + 0x18);
        wr32(p + 0x20, e.attr);
        std::memset(p + 0x40, 0, 32);
        std::memcpy(p + 0x40, e.name.data(), std::min<size_t>(e.name.size(), 32));
        touchPage(page);
    }

    Card::Dir Card::rootDir() const { return Dir{rootCluster_, Loc{rootCluster_, 0}}; }

    bool Card::validName(std::string_view name) const
    {
        // MCMAN 0x3cb0: the leaf name must be shorter than 32 bytes.
        return !name.empty() && name.size() < 32;
    }

    Card::Lookup Card::lookup(std::string_view path) const
    {
        Lookup r;
        r.parent = rootDir();
        const std::vector<std::string> parts = splitPath(path);
        if (parts.empty())
        {
            r.status = 0;
            r.found = r.parent.info; // the root itself
            return r;
        }
        Dir cur = rootDir();
        for (size_t i = 0; i + 1 < parts.size(); ++i)
        {
            const uint32_t len = dirLength(cur);
            bool hit = false;
            for (uint32_t k = 0; k < len; ++k)
            {
                const Loc loc{cur.first, k};
                const Entry e = readEntry(loc);
                if ((e.mode & ModeExists) && e.name == parts[i])
                {
                    if (!(e.mode & ModeDir))
                    {
                        r.status = 3;
                        return r;
                    }
                    cur = dirOf(loc, e);
                    hit = true;
                    break;
                }
            }
            if (!hit)
            {
                r.status = 2;
                return r;
            }
        }
        r.parent = cur;
        r.leaf = parts.back();
        const uint32_t len = dirLength(cur);
        r.freeSlot = len;
        for (uint32_t k = 0; k < len; ++k)
        {
            const Loc loc{cur.first, k};
            const Entry e = readEntry(loc);
            if (!(e.mode & ModeExists))
            {
                if (r.freeSlot == len)
                    r.freeSlot = k;
                continue;
            }
            if (e.name == r.leaf)
            {
                r.status = 0;
                r.found = loc;
                return r;
            }
        }
        r.status = 1;
        return r;
    }

    int32_t Card::extendDirectory(const Dir &d, uint32_t length)
    {
        // A directory holds 2 entries per cluster; entry `length` needs a new cluster when even.
        if (length % 2)
            return Ok;
        std::vector<uint32_t> c;
        if (!chain(d.first, c) || c.empty())
            return Denied;
        uint32_t fresh = 0;
        const int32_t r = allocate(fresh);
        if (r < 0)
            return r;
        setFat(c.back(), fresh | InUse);
        const std::vector<uint8_t> zero(ClusterBytes, 0);
        writeCluster(allocOffset_ + fresh, zero.data());
        return Ok;
    }

    int32_t Card::deleteEntry(const Loc &loc)
    {
        // MCMAN 0x7e30: clear "exists" and the in-use bit of each chain entry (the links stay).
        Entry e = readEntry(loc);
        if (e.name == "." || e.name == "..")
            return NoEntry;
        for (const Handle &h : handles_)
            if (h.used && h.entry.cluster == loc.cluster && h.entry.index == loc.index)
                return Denied;
        e.mode = uint16_t(e.mode & 0x7fff);
        writeEntry(loc, e);
        uint32_t c = e.cluster;
        for (uint32_t guard = 0; c != ChainEnd && c < allocEnd_ && guard < ClustersPerCard; ++guard)
        {
            const uint32_t v = fat(c);
            if (!(v & InUse))
                break;
            setFat(c, v & ~InUse);
            hint_ = std::min(hint_, c);
            if ((v & ~InUse) == FreeEntry)
                break;
            c = v & ~InUse;
        }
        return Ok;
    }

    void Card::updateFileEntry(const Handle &h, bool closed)
    {
        // MCMAN 0x6124: the "closed" bit, modified time, first cluster and size; the parent's modified time.
        Entry e = readEntry(h.entry);
        e.mode = closed ? uint16_t(e.mode | ModeClosed) : uint16_t(e.mode & ~ModeClosed);
        e.modified = clock_();
        e.cluster = h.first;
        e.length = h.size;
        writeEntry(h.entry, e);
        Entry p = readEntry(h.parentInfo);
        p.modified = e.modified;
        writeEntry(h.parentInfo, p);
    }

    // ---------------------------------------------------------------- MCMAN operations

    void Card::closeAll()
    {
        for (Handle &h : handles_)
            h = Handle{};
        listValid_ = false;
    }

    int32_t Card::open(std::string_view path, uint32_t flags)
    {
        if (!formatted_)
            return NoFormat;
        if (flags & (OpenMkdir | OpenCreate))
        {
            flags |= OpenWrite;
            const std::vector<std::string> parts = splitPath(path);
            if (parts.empty() || !validName(parts.back()))
                return NoEntry;
        }
        if (path.empty())
            return NoEntry;
        int32_t fd = -1;
        for (int32_t i = 0; i < 3; ++i)
            if (!handles_[i].used)
            {
                fd = i;
                break;
            }
        if (fd < 0)
            return NoHandle;

        const Lookup r = lookup(path);
        if (r.status == 2)
            return NoEntry;
        if (r.status == 3)
            return Denied;
        const bool create = (flags & (OpenMkdir | OpenCreate)) != 0;
        const bool inRoot = r.parent.first == rootCluster_ && r.parent.info.cluster == rootCluster_ && r.parent.info.index == 0;
        if (r.status == 0 && (flags & OpenMkdir))
            return NoEntry; // the directory exists (MCMAN 0x6db8)
        if (r.status == 1 && (flags & OpenMkdir) && !inRoot)
            return Denied; // directories only in the root (MCMAN 0x6dc8)
        if (r.status == 1 && create && !inRoot && r.freeSlot >= SubdirSlotLimit)
            return Denied; // MCMAN 0x6e10
        if (r.status == 1 && !create)
            return NoEntry;
        if (r.status == 0 && r.leaf.empty())
            return Denied; // the root itself

        Handle h;
        h.used = true;
        h.rd = (flags & OpenRead) != 0;
        h.wr = (flags & OpenWrite) != 0;
        h.parentInfo = r.parent.info;
        uint32_t slot = r.freeSlot;

        if (r.status == 0)
        {
            const Entry e = readEntry(r.found);
            if (h.wr && !(e.mode & ModeWrite))
                return Denied;
            if (h.rd && !(e.mode & ModeRead))
                return Denied;
            if (e.mode & ModeDir)
            {
                if ((e.mode & (ModeRead | ModeExecute)) != (ModeRead | ModeExecute))
                    return Denied;
                h.isDir = true;
                h.rd = h.wr = false;
                h.entry = r.found;
                h.size = e.length;
                h.first = e.cluster;
                handles_[fd] = h;
                return fd;
            }
            if (h.wr)
                for (const Handle &o : handles_)
                    if (o.used && o.wr && o.entry.cluster == r.found.cluster && o.entry.index == r.found.index)
                        return Denied;
            if (!(flags & OpenCreate))
            {
                h.entry = r.found;
                h.size = e.length;
                h.first = e.cluster;
                handles_[fd] = h;
                return fd;
            }
            // O_CREAT on an existing file: delete it, then create a new one (MCMAN 0x7078-0x70cc).
            const int32_t d = deleteEntry(r.found);
            if (d < 0)
                return d;
            slot = std::min(slot, r.found.index);
        }

        // Create path (MCMAN 0x7138-0x75d8).
        Entry info = readEntry(r.parent.info);
        if (slot == info.length)
        {
            const int32_t x = extendDirectory(r.parent, info.length);
            if (x < 0)
                return x;
            info.length += 1;
        }
        uint32_t dirCluster = ChainEnd;
        if (flags & OpenMkdir)
        {
            const int32_t a = allocate(dirCluster);
            if (a < 0)
                return a;
        }
        const Tod now = clock_();
        info.modified = now;
        writeEntry(r.parent.info, info);

        const Loc loc{r.parent.first, slot};
        {
            // The new entry starts from zero (MCMAN clears the 512 bytes).
            uint32_t abs = 0, off = 0;
            if (!entryOffset(loc, abs, off))
                return Denied;
            const uint32_t page = abs * PagesPerCluster + off / PageBytes;
            std::memset(pageData(page), 0, PageBytes);
            touchPage(page);
        }
        Entry e;
        e.name = r.leaf;
        e.created = e.modified = now;
        if (flags & OpenMkdir)
        {
            e.mode = uint16_t(0x8427 | (flags & 0x1800));
            e.length = 2;
            e.cluster = dirCluster;
            writeEntry(loc, e);
            // "." -> (parent's first cluster, our slot); ".." -> what the parent's "." holds, with
            // the parent's created time (MCMAN 0x4be4).
            std::vector<uint8_t> buf(ClusterBytes, 0);
            uint8_t *dot = buf.data();
            wr16(dot, 0x8427);
            now.store(dot + 8);
            now.store(dot + 0x18);
            wr32(dot + 0x10, r.parent.first);
            wr32(dot + 0x14, slot);
            dot[0x40] = '.';
            const Entry parentDot = readEntry(Loc{r.parent.first, 0});
            uint8_t *dd = buf.data() + EntryBytes;
            wr16(dd, 0x8427);
            parentDot.created.store(dd + 8);
            parentDot.created.store(dd + 0x18);
            wr32(dd + 0x10, parentDot.cluster);
            wr32(dd + 0x14, parentDot.dirEntry);
            dd[0x40] = '.';
            dd[0x41] = '.';
            writeCluster(allocOffset_ + dirCluster, buf.data());
            return Ok;
        }
        e.mode = uint16_t(0x8417 | (flags & 0x1800));
        e.length = 0;
        e.cluster = ChainEnd;
        writeEntry(loc, e);
        h.entry = loc;
        h.dirty = true; // MCMAN sets the handle's "write pending" flag on create (0x75cc)
        h.size = 0;
        h.first = ChainEnd;
        handles_[fd] = h;
        return fd;
    }

    int32_t Card::close(int32_t fd)
    {
        if (fd < 0 || fd > 2 || !handles_[fd].used)
            return NoEntry;
        Handle &h = handles_[fd];
        if (h.dirty && !h.isDir)
        {
            h.dirty = false;
            updateFileEntry(h, true);
        }
        h = Handle{};
        return Ok;
    }

    int32_t Card::flush(int32_t fd)
    {
        if (fd < 0 || fd > 2 || !handles_[fd].used)
            return NoEntry;
        Handle &h = handles_[fd];
        if (h.dirty && !h.isDir)
        {
            h.dirty = false;
            updateFileEntry(h, true);
        }
        return Ok;
    }

    int32_t Card::seek(int32_t fd, int32_t offset, int32_t whence)
    {
        if (fd < 0 || fd > 2 || !handles_[fd].used)
            return NoEntry;
        Handle &h = handles_[fd];
        int64_t pos;
        if (whence == 0)
            pos = offset;
        else if (whence == 2)
            pos = int64_t(h.size) + offset;
        else
            pos = int64_t(h.position) + offset;
        if (pos < 0)
            pos = 0;
        h.position = uint32_t(std::min<int64_t>(pos, INT32_MAX));
        return int32_t(h.position);
    }

    int32_t Card::read(int32_t fd, void *data, uint32_t size)
    {
        if (fd < 0 || fd > 2 || !handles_[fd].used)
            return NoEntry;
        Handle &h = handles_[fd];
        if (!h.rd)
            return Denied;
        if (h.position >= h.size || !size)
            return 0;
        const uint32_t total = std::min(size, h.size - h.position);
        std::vector<uint32_t> c;
        chain(h.first, c);
        uint8_t *out = static_cast<uint8_t *>(data);
        uint32_t done = 0;
        while (done < total)
        {
            const uint32_t k = h.position / ClusterBytes, within = h.position % ClusterBytes;
            if (k >= c.size())
                break; // the chain is shorter than the size says
            const uint32_t n = std::min(ClusterBytes - within, total - done);
            const uint32_t abs = allocOffset_ + c[k];
            for (uint32_t i = 0; i < n;)
            {
                const uint32_t at = within + i;
                const uint32_t page = abs * PagesPerCluster + at / PageBytes;
                const uint32_t part = std::min(PageBytes - at % PageBytes, n - i);
                std::memcpy(out + done + i, pageData(page) + at % PageBytes, part);
                i += part;
            }
            done += n;
            h.position += n;
        }
        return int32_t(done);
    }

    int32_t Card::write(int32_t fd, const void *data, uint32_t size)
    {
        if (fd < 0 || fd > 2 || !handles_[fd].used)
            return NoEntry;
        Handle &h = handles_[fd];
        if (!h.wr)
            return Denied;
        if (size && !h.dirty)
        {
            // First write since open/flush: the entry is marked "not closed" (MCMAN 0x5f58).
            h.dirty = true;
            updateFileEntry(h, false);
        }
        std::vector<uint32_t> c;
        chain(h.first, c);
        const uint8_t *in = static_cast<const uint8_t *>(data);
        uint32_t done = 0;
        int32_t result = 0;
        while (done < size)
        {
            const uint32_t k = h.position / ClusterBytes, within = h.position % ClusterBytes;
            while (c.size() <= k)
            {
                uint32_t fresh = 0;
                if (allocate(fresh) < 0)
                {
                    result = Full;
                    break;
                }
                if (c.empty())
                    h.first = fresh;
                else
                    setFat(c.back(), fresh | InUse);
                c.push_back(fresh);
            }
            if (result < 0)
                break;
            const uint32_t n = std::min(ClusterBytes - within, size - done);
            const uint32_t abs = allocOffset_ + c[k];
            for (uint32_t i = 0; i < n;)
            {
                const uint32_t at = within + i;
                const uint32_t page = abs * PagesPerCluster + at / PageBytes;
                const uint32_t part = std::min(PageBytes - at % PageBytes, n - i);
                std::memcpy(pageData(page) + at % PageBytes, in + done + i, part);
                touchPage(page);
                i += part;
            }
            done += n;
            h.position += n;
            h.size = std::max(h.size, h.position);
        }
        updateFileEntry(h, !h.dirty);
        return result < 0 ? result : int32_t(done);
    }

    int32_t Card::getDir(std::string_view path, bool continueListing, int32_t maxent, std::vector<DirInfo> *out)
    {
        if (!formatted_)
            return NoFormat;
        if (!continueListing)
        {
            // "dir/pattern": the part after the last '/' is the pattern (MCMAN 0x80fc).
            listValid_ = false;
            const std::string full(path.substr(0, std::min<size_t>(path.size(), 0x3ff)));
            const size_t slash = full.rfind('/');
            std::string dirPath, pattern;
            if (slash == std::string::npos)
                pattern = full; // the current directory (always the root here)
            else
            {
                dirPath = slash == 0 ? std::string("/") : full.substr(0, slash);
                pattern = full.substr(slash + 1);
            }
            const Lookup r = lookup(dirPath);
            if (r.status != 0)
                return NoEntry;
            const Entry e = readEntry(r.found);
            if (!(e.mode & ModeDir))
                return NoEntry;
            listDir_ = r.leaf.empty() ? rootDir() : dirOf(r.found, e);
            listCount_ = readEntry(listDir_.info).length;
            const bool isRoot = listDir_.first == rootCluster_ && listDir_.info.cluster == rootCluster_ && listDir_.info.index == 0;
            listIndex_ = isRoot ? 2 : 0; // the root lists without "." and ".." (MCMAN 0x82dc)
            listPattern_ = pattern;
            listValid_ = true;
        }
        else if (!listValid_)
            return NoEntry;

        int32_t count = 0;
        while (maxent != 0 && listIndex_ < listCount_)
        {
            const uint32_t index = listIndex_++;
            const Entry raw = readEntry(Loc{listDir_.first, index});
            if (!(raw.mode & ModeExists))
                continue;
            if (raw.mode & ModeHidden)
                continue;
            if (!match(listPattern_, raw.name))
                continue;
            if (maxent < 0)
            {
                ++count;
                continue;
            }
            Entry shown = raw;
            std::string name = raw.name;
            if (index == 0) // "." shows the directory's own entry
            {
                shown = readEntry(Loc{raw.cluster, raw.dirEntry});
                name = ".";
            }
            else if (index == 1) // ".." shows the parent's entry
            {
                const Entry dot = readEntry(Loc{listDir_.first, 0});
                const Entry parentDot = readEntry(Loc{dot.cluster, 0});
                shown = readEntry(Loc{parentDot.cluster, parentDot.dirEntry});
                name = "..";
            }
            DirInfo d;
            d.created = shown.created;
            d.modified = shown.modified;
            d.size = (shown.mode & ModeDir) ? 0 : shown.length;
            d.mode = shown.mode;
            d.unused = shown.unused;
            d.attr = shown.attr;
            d.name = name;
            if (out)
                out->push_back(d);
            ++count;
            --maxent;
        }
        return count;
    }

    int32_t Card::remove(std::string_view path)
    {
        if (!formatted_)
            return NoFormat;
        const Lookup r = lookup(path);
        if (r.status == 3)
            return Denied;
        if (r.status != 0 || r.leaf.empty())
            return NoEntry;
        const Entry e = readEntry(r.found);
        if (!(e.mode & ModeExists))
            return NoEntry;
        if (!(e.mode & ModeWrite))
            return Denied;
        if (e.mode & ModeDir)
        {
            const Dir d = dirOf(r.found, e);
            for (uint32_t i = 2; i < e.length; ++i)
                if (readEntry(Loc{d.first, i}).mode & ModeExists)
                    return NotEmpty;
        }
        return deleteEntry(r.found);
    }

    int32_t Card::setInfo(std::string_view path, const DirInfo &info, uint32_t valid)
    {
        if (!formatted_)
            return NoFormat;
        const Lookup r = lookup(path);
        if (r.status == 3)
            return Denied;
        if (r.status != 0 || r.leaf.empty())
            return NoEntry;
        std::string newName;
        if (valid & InfoName)
        {
            newName = info.name.substr(0, std::min<size_t>(info.name.size(), 31));
            if (newName.empty() || newName == "." || newName == ".." || newName.find('/') != std::string::npos)
                return NoEntry;
            const size_t slash = path.rfind('/');
            const std::string sibling(path.substr(0, slash == std::string_view::npos ? 0 : slash + 1));
            const Lookup other = lookup(sibling + newName);
            if (other.status == 0 && !(other.found.cluster == r.found.cluster && other.found.index == r.found.index))
                return NoEntry; // the new name exists
        }
        Entry e = readEntry(r.found);
        if (valid & InfoAttr)
            e.attr = info.attr;
        if ((valid & InfoMode) && (e.mode & ModeDir))
        {
            // MCMAN 0x5bbc: only a directory's mode changes here, in the bits 0x180f.
            constexpr uint16_t mask = 0x180f;
            e.mode = uint16_t((e.mode & ~mask) | (info.mode & mask));
        }
        if (valid & InfoCreated)
            e.created = info.created;
        if (valid & InfoModified)
            e.modified = info.modified;
        if (valid & InfoName)
            e.name = newName;
        writeEntry(r.found, e);
        return Ok;
    }

    int32_t Card::getEntSpace(std::string_view path)
    {
        if (!formatted_)
            return NoFormat;
        const Lookup r = lookup(path);
        if (r.status != 0)
            return NoEntry;
        // MCMAN 0x6268: the free half of the last cluster plus every deleted entry.
        const Entry e = readEntry(r.found);
        const Dir d = r.leaf.empty() ? rootDir() : dirOf(r.found, e);
        const uint32_t len = readEntry(d.info).length;
        int32_t space = int32_t(len & 1);
        for (uint32_t i = 0; i < len; ++i)
            if (!(readEntry(Loc{d.first, i}).mode & ModeExists))
                ++space;
        return space;
    }

    int32_t Card::format()
    {
        const std::string keep = savedPath_;
        createFormatted();
        savedPath_ = keep;
        return Ok;
    }

    int32_t Card::unformat()
    {
        const std::string keep = savedPath_;
        createUnformatted();
        savedPath_ = keep;
        return Ok;
    }

    // ---------------------------------------------------------------- tools

    bool Card::tree(Node &root, std::string &error) const
    {
        if (!formatted_)
        {
            error = "card is not formatted";
            return false;
        }
        root = Node{};
        root.entry = readEntry(rootDir().info);
        if (!chain(rootCluster_, root.chain))
        {
            error = "broken FAT chain for the root directory";
            return false;
        }
        std::function<bool(const Dir &, Node &, int)> walk = [&](const Dir &d, Node &node, int depth) -> bool
        {
            if (depth > 4)
            {
                error = "directories nested too deep";
                return false;
            }
            const uint32_t len = readEntry(d.info).length;
            for (uint32_t i = 2; i < len; ++i)
            {
                const Loc loc{d.first, i};
                const Entry e = readEntry(loc);
                if (!(e.mode & ModeExists))
                    continue;
                Node child;
                child.entry = e;
                if (!chain(e.cluster, child.chain))
                {
                    error = "broken FAT chain for '" + e.name + "'";
                    return false;
                }
                if ((e.mode & ModeDir) && !walk(dirOf(loc, e), child, depth + 1))
                    return false;
                node.children.push_back(std::move(child));
            }
            return true;
        };
        return walk(rootDir(), root, 0);
    }

    std::vector<std::string> Card::check() const
    {
        std::vector<std::string> problems;
        if (!formatted_)
        {
            problems.push_back("not formatted (no superblock)");
            return problems;
        }
        Node root;
        std::string error;
        if (!tree(root, error))
        {
            problems.push_back(error);
            return problems;
        }
        std::vector<uint8_t> owner(allocEnd_, 0);
        auto claim = [&](const std::vector<uint32_t> &c, const std::string &what)
        {
            for (uint32_t x : c)
            {
                if (x >= maxAlloc_)
                    problems.push_back(what + ": cluster " + std::to_string(x) + " beyond the allocatable end");
                if (x < owner.size() && owner[x]++)
                    problems.push_back(what + ": cluster " + std::to_string(x) + " used twice");
            }
        };
        claim(root.chain, "/");
        if (root.chain.size() != (size_t(root.entry.length) + 1) / 2)
            problems.push_back("/: " + std::to_string(root.chain.size()) + " clusters for " +
                               std::to_string(root.entry.length) + " entries");
        std::function<void(const Node &, const std::string &)> visit = [&](const Node &n, const std::string &where)
        {
            for (const Node &c : n.children)
            {
                const std::string path = where + "/" + c.entry.name;
                claim(c.chain, path);
                if (c.entry.mode & ModeDir)
                {
                    if (c.chain.size() != (size_t(c.entry.length) + 1) / 2)
                        problems.push_back(path + ": " + std::to_string(c.chain.size()) + " clusters for " +
                                           std::to_string(c.entry.length) + " entries");
                    visit(c, path);
                }
                else
                {
                    const size_t need = (size_t(c.entry.length) + ClusterBytes - 1) / ClusterBytes;
                    if (c.chain.size() != need)
                        problems.push_back(path + ": " + std::to_string(c.chain.size()) + " clusters for " +
                                           std::to_string(c.entry.length) + " bytes");
                    if (!(c.entry.mode & ModeClosed))
                        problems.push_back(path + ": not closed (an interrupted write)");
                }
            }
        };
        visit(root, "");
        uint32_t lost = 0;
        for (uint32_t n = 0; n < std::min(allocEnd_, maxAlloc_); ++n)
            if ((fat(n) & InUse) && !owner[n])
                ++lost;
        if (lost)
            problems.push_back(std::to_string(lost) + " FAT entries in use by no file");
        // MCMAN restores from backup block 1 at start-up when backup block 2 is not erased.
        const uint8_t *b2 = pageData(backup2_ * PagesPerBlock);
        if (!std::all_of(b2, b2 + 2 * PageBytes, [](uint8_t b) { return b == 0xff; }))
            problems.push_back("backup block 2 is not erased (MCMAN would run a recovery)");
        // ECC of every page as loaded (pages changed since are given their ECC when saved).
        uint32_t badEcc = 0, dataOnErased = 0, firstBad = 0;
        for (uint32_t pg = 0; pg < PagesPerCard; ++pg)
        {
            if (pageDirty_[pg] != PageClean)
                continue;
            const uint8_t *sp = spare_.data() + size_t(pg) * SpareBytes;
            const uint8_t *d = pageData(pg);
            if (std::all_of(sp, sp + SpareBytes, [](uint8_t b) { return b == 0xff; }))
            {
                if (!std::all_of(d, d + PageBytes, [](uint8_t b) { return b == 0xff; }))
                    ++dataOnErased;
                continue;
            }
            uint8_t ecc[SpareBytes];
            eccPage(d, ecc);
            // Page 1 (second half of the superblock cluster) is not used by MCMAN; a PCSX2 card the
            // BIOS formatted carries 8 bytes there under the ECC of an erased page.
            if (std::memcmp(ecc, sp, 12) != 0 && pg != 1)
            {
                if (!badEcc++)
                    firstBad = pg;
            }
        }
        if (badEcc)
            problems.push_back(std::to_string(badEcc) + " pages with a wrong ECC (first: page " + std::to_string(firstBad) + ")");
        if (dataOnErased)
            problems.push_back(std::to_string(dataOnErased) + " pages hold data but an erased (0xFF) spare");
        return problems;
    }
}
