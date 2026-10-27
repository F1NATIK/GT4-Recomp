#include "gt4_usbdisk.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <ctime>
#include <cwctype>
#include <fstream>
#include <set>
#include <system_error>

// Layout (512-byte sectors): LBA 0 MBR with one partition {status 0, type 0x0e
// FAT16 LBA, start 2048}; the volume has 1 reserved sector (boot sector), two
// FATs, a 512-entry root directory and clusters sized so the cluster count
// stays inside GT4's FAT16 window (0x4ca0bc: clusters+2 in 0xff7..0xfff6).
// 128 MiB -> 4 sectors per cluster, 254 sectors per FAT, 64,888 clusters.
//
// GT4's FAT driver (checked in CORE): names are upper-cased 8.3 only (0x4cb1f0,
// ctype table 0x6d0e79), compared on all 11 bytes (0x4cba58, memcmp 0x594470);
// LFN entries (attr & 0x3f == 0x0f, 0x4cb1d8), deleted entries and volume labels
// are skipped and never written; new entries take the first free or deleted
// slot (0x4cbbc8); clusters are taken first-fit from the previous cluster
// (0x4ce078); the end-of-chain mark is 0xffff (0x60d6d8). Imported names that
// are not upper case therefore get NT case flags (byte 12) or, for mixed case,
// one LFN entry, so GT4 still matches them by their upper-case 8.3 name.

namespace ps2x::iop::gt4
{
    namespace fs = std::filesystem;

    namespace
    {
        constexpr uint8_t AttrVolume = 0x08, AttrDirectory = 0x10, AttrArchive = 0x20, AttrLfn = 0x0f;
        constexpr uint8_t NtLowerBase = 0x08, NtLowerExt = 0x10;
        constexpr uint32_t Fat16Eoc = 0xffffu;
        constexpr int MaxDepth = 8;
        constexpr uint32_t VolumeSerial = 0x47543404u;
        constexpr uint32_t MaxSkipLogs = 32;

        void put16(uint8_t *p, uint32_t v)
        {
            p[0] = static_cast<uint8_t>(v);
            p[1] = static_cast<uint8_t>(v >> 8);
        }
        void put32(uint8_t *p, uint32_t v)
        {
            put16(p, v);
            put16(p + 2, v >> 16);
        }
        uint32_t get16(const uint8_t *p) { return uint32_t(p[0]) | (uint32_t(p[1]) << 8); }
        uint32_t get32(const uint8_t *p) { return get16(p) | (get16(p + 2) << 16); }

        // CHS as written by PC partitioners for 255 heads / 63 sectors.
        void putChs(uint8_t *p, uint32_t lba)
        {
            uint32_t c = lba / (255u * 63u), h = (lba / 63u) % 255u, s = lba % 63u + 1u;
            if (c > 1023u)
            {
                c = 1023u;
                h = 254u;
                s = 63u;
            }
            p[0] = static_cast<uint8_t>(h);
            p[1] = static_cast<uint8_t>((s & 0x3fu) | ((c >> 2) & 0xc0u));
            p[2] = static_cast<uint8_t>(c);
        }

        bool shortNameChar(unsigned char c)
        {
            if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9'))
                return true;
            return std::strchr("!#$%&'()-@^_`{}~", c) != nullptr && c != 0;
        }

        struct ShortName
        {
            std::array<char, 11> raw{};
            uint8_t nt = 0;
            bool lfn = false;
        };

        // Host name -> 8.3 directory name; false when it has no 8.3 form.
        bool makeShortName(const std::string &name, ShortName &out)
        {
            const size_t dot = name.find('.');
            if (name.empty() || dot == 0 || (dot != std::string::npos && name.find('.', dot + 1) != std::string::npos))
                return false;
            const std::string base = dot == std::string::npos ? name : name.substr(0, dot);
            const std::string ext = dot == std::string::npos ? std::string() : name.substr(dot + 1);
            if (base.empty() || base.size() > 8 || ext.size() > 3 || (dot != std::string::npos && ext.empty()))
                return false;
            out = {};
            out.raw.fill(' ');
            const auto part = [&](const std::string &text, size_t at, uint8_t lowerFlag) {
                bool upper = false, lower = false;
                for (size_t i = 0; i < text.size(); ++i)
                {
                    const unsigned char c = static_cast<unsigned char>(text[i]);
                    if (!shortNameChar(c))
                        return false;
                    upper = upper || (c >= 'A' && c <= 'Z');
                    lower = lower || (c >= 'a' && c <= 'z');
                    out.raw[at + i] = static_cast<char>(c >= 'a' && c <= 'z' ? c - 0x20 : c);
                }
                if (upper && lower)
                    out.lfn = true;
                else if (lower)
                    out.nt |= lowerFlag;
                return true;
            };
            if (!part(base, 0, NtLowerBase) || !part(ext, 8, NtLowerExt))
                return false;
            if (out.lfn)
                out.nt = 0;
            return true;
        }

        uint8_t lfnChecksum(const uint8_t *raw)
        {
            uint8_t sum = 0;
            for (int i = 0; i < 11; ++i)
                sum = static_cast<uint8_t>(((sum & 1u) << 7) + (sum >> 1) + raw[i]);
            return sum;
        }

        constexpr std::array<int, 13> LfnOffsets{1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30};

        // Display form of an 8.3 entry name, honouring the NT lower-case flags.
        std::string formatShort(const uint8_t *raw, uint8_t nt)
        {
            std::string base(reinterpret_cast<const char *>(raw), 8), ext(reinterpret_cast<const char *>(raw) + 8, 3);
            if (!base.empty() && static_cast<unsigned char>(base[0]) == 0x05)
                base[0] = static_cast<char>(0xe5);
            while (!base.empty() && base.back() == ' ')
                base.pop_back();
            while (!ext.empty() && ext.back() == ' ')
                ext.pop_back();
            const auto lower = [](std::string &s) {
                for (char &c : s)
                    if (c >= 'A' && c <= 'Z')
                        c = static_cast<char>(c + 0x20);
            };
            if (nt & NtLowerBase)
                lower(base);
            if (nt & NtLowerExt)
                lower(ext);
            return ext.empty() ? base : base + "." + ext;
        }

        void fatTimestamp(fs::file_time_type when, uint16_t &date, uint16_t &time)
        {
            // file_clock -> system_clock without clock_cast (portable; FAT has 2 s steps).
            const auto sys = std::chrono::system_clock::now() +
                             std::chrono::duration_cast<std::chrono::system_clock::duration>(when - fs::file_time_type::clock::now());
            const std::time_t t = std::chrono::system_clock::to_time_t(sys);
            std::tm tm{};
#if defined(_WIN32)
            localtime_s(&tm, &t);
#else
            localtime_r(&t, &tm);
#endif
            if (tm.tm_year < 80)
            {
                date = (1u << 5) | 1u; // 1980-01-01
                time = 0;
                return;
            }
            const int year = std::min(tm.tm_year - 80, 127);
            date = static_cast<uint16_t>((year << 9) | ((tm.tm_mon + 1) << 5) | tm.tm_mday);
            time = static_cast<uint16_t>((tm.tm_hour << 11) | (tm.tm_min << 5) | (tm.tm_sec / 2));
        }

        // For log lines and the folder fingerprint; never throws (u8string() does for
        // a Windows name with an unpaired UTF-16 surrogate).
        std::string utf8(const fs::path &path)
        {
            try
            {
                const std::u8string text = path.u8string();
                return std::string(text.begin(), text.end());
            }
            catch (const std::exception &)
            {
                std::string out;
                for (const auto c : path.native())
                    out.push_back(c > 0 && c < 0x80 ? static_cast<char>(c) : '?');
                return out;
            }
        }

        fs::path fromUtf8(const std::string &text)
        {
            return fs::path(std::u8string(text.begin(), text.end()));
        }

        // The file name when it is printable ASCII (what 8.3 allows), else empty.
        // Reads the native name, so no conversion can fail.
        std::string asciiName(const fs::path &path)
        {
            const fs::path::string_type name = path.filename().native();
            std::string out;
            out.reserve(name.size());
            for (const auto c : name)
            {
                if (c < 0x20 || c >= 0x7f)
                    return {};
                out.push_back(static_cast<char>(c));
            }
            return out;
        }
    }

    struct UsbDisk::Child
    {
        std::string name; // host spelling
        ShortName shortName;
        bool directory = false;
        uint64_t size = 0;
        fs::file_time_type time{};
        fs::path path;
        uint32_t entries() const { return shortName.lfn ? 2u : 1u; }
    };

    struct UsbDisk::BuildContext
    {
        std::vector<uint16_t> fat;
        uint32_t next = 2;
        BuildStats stats;
        uint32_t skipLogs = 0;
        std::vector<std::pair<uint64_t, std::vector<uint8_t>>> directoryWrites;
        std::set<uint32_t> fatSectorsUsed;

        uint32_t allocate(uint32_t count, uint32_t clusters)
        {
            if (count == 0 || next + count > clusters + 2u)
                return 0;
            const uint32_t first = next;
            for (uint32_t i = 0; i < count; ++i)
                fat[first + i] = static_cast<uint16_t>(i + 1u < count ? first + i + 1u : Fat16Eoc);
            next += count;
            return first;
        }
    };

    UsbDisk::UsbDisk(Log log) : log_(std::move(log)) {}

    void UsbDisk::log(bool warning, const std::string &message) const
    {
        if (log_)
            log_(warning, message);
    }

    UsbDisk::Geometry UsbDisk::computeGeometry(uint32_t sizeMiB)
    {
        sizeMiB = std::clamp(sizeMiB, MinSizeMiB, MaxSizeMiB);
        Geometry g;
        g.totalSectors = sizeMiB * 2048u;
        g.partitionSectors = g.totalSectors - PartitionStart;
        g.reservedSectors = 1;
        g.rootSectors = RootEntries * 32u / SectorBytes;
        for (uint32_t spc = 1; spc <= 64; spc <<= 1)
        {
            // FATSz as the Microsoft FAT specification computes it for FAT16 (a slight
            // over-estimate), then the cluster count exactly as GT4 derives it (0x4ca09c).
            const uint32_t span = g.partitionSectors - (g.reservedSectors + g.rootSectors);
            const uint32_t perFatSector = 256u * spc + 2u;
            const uint32_t fat = (span + perFatSector - 1u) / perFatSector;
            const uint32_t clusters = (g.partitionSectors - (g.reservedSectors + 2u * fat + g.rootSectors)) / spc;
            if (clusters <= 65524u)
            {
                g.sectorsPerCluster = spc;
                g.fatSectors = fat;
                g.clusters = clusters;
                break;
            }
        }
        g.fatLba = PartitionStart + g.reservedSectors;
        g.rootLba = g.fatLba + 2u * g.fatSectors;
        g.dataLba = g.rootLba + g.rootSectors;
        return g;
    }

    void UsbDisk::clear()
    {
        pages_.clear();
        extents_.clear();
        imported_.clear();
        manifest_.clear();
        built_ = false;
    }

    uint32_t UsbDisk::clusterLba(uint32_t cluster) const
    {
        return geometry_.dataLba + (cluster - 2u) * geometry_.sectorsPerCluster;
    }

    void UsbDisk::fillFromHost(uint32_t lba, uint8_t *out) const
    {
        std::memset(out, 0, SectorBytes);
        auto it = std::upper_bound(extents_.begin(), extents_.end(), lba,
                                   [](uint32_t value, const Extent &extent) { return value < extent.lba; });
        if (it == extents_.begin())
            return;
        --it;
        if (lba >= it->lba + it->sectors)
            return;
        std::ifstream file(it->host, std::ios::binary);
        if (!file)
            return;
        file.seekg(static_cast<std::streamoff>(uint64_t(lba - it->lba) * SectorBytes));
        file.read(reinterpret_cast<char *>(out), SectorBytes);
    }

    void UsbDisk::readSector(uint32_t lba, uint8_t *out) const
    {
        if (const auto it = pages_.find(lba / PageSectors); it != pages_.end())
        {
            std::memcpy(out, it->second->data() + (lba % PageSectors) * SectorBytes, SectorBytes);
            return;
        }
        fillFromHost(lba, out);
    }

    void UsbDisk::writeSector(uint32_t lba, const uint8_t *in)
    {
        auto &page = pages_[lba / PageSectors];
        if (!page)
        {
            page = std::make_unique<Page>();
            const uint32_t first = lba - lba % PageSectors;
            for (uint32_t i = 0; i < PageSectors; ++i)
                fillFromHost(first + i, page->data() + i * SectorBytes);
        }
        std::memcpy(page->data() + (lba % PageSectors) * SectorBytes, in, SectorBytes);
    }

    void UsbDisk::writeBytes(uint64_t offset, const uint8_t *data, size_t size)
    {
        std::array<uint8_t, SectorBytes> sector{};
        while (size)
        {
            const uint32_t lba = static_cast<uint32_t>(offset / SectorBytes);
            const size_t at = static_cast<size_t>(offset % SectorBytes);
            const size_t n = std::min(size, SectorBytes - at);
            readSector(lba, sector.data());
            std::memcpy(sector.data() + at, data, n);
            writeSector(lba, sector.data());
            offset += n;
            data += n;
            size -= n;
        }
    }

    bool UsbDisk::read(uint32_t lba, uint32_t count, uint8_t *out) const
    {
        if (!built_ || lba > geometry_.totalSectors || count > geometry_.totalSectors - lba)
            return false;
        for (uint32_t i = 0; i < count; ++i)
            readSector(lba + i, out + size_t(i) * SectorBytes);
        return true;
    }

    bool UsbDisk::write(uint32_t lba, uint32_t count, const uint8_t *in)
    {
        if (!built_ || lba > geometry_.totalSectors || count > geometry_.totalSectors - lba)
            return false;
        for (uint32_t i = 0; i < count; ++i)
            writeSector(lba + i, in + size_t(i) * SectorBytes);
        return true;
    }

    uint32_t UsbDisk::fatEntry(uint32_t cluster) const
    {
        std::array<uint8_t, SectorBytes> sector{};
        const uint32_t offset = cluster * 2u;
        readSector(geometry_.fatLba + offset / SectorBytes, sector.data());
        return get16(sector.data() + offset % SectorBytes);
    }

    bool UsbDisk::volumeClean() const
    {
        return built_ && (fatEntry(1) & 0x8000u) != 0;
    }

    void UsbDisk::formatEmpty(BuildContext &ctx)
    {
        const Geometry &g = geometry_;
        std::array<uint8_t, SectorBytes> s{};
        // MBR: no boot code, disk signature, partition 1 = FAT16 LBA (0x0e).
        put32(&s[0x1b8], VolumeSerial);
        uint8_t *p = &s[0x1be];
        p[0] = 0x00; // GT4 0x4c9e98: status & 0x7f must be 0
        putChs(p + 1, PartitionStart);
        p[4] = 0x0e;
        putChs(p + 5, g.totalSectors - 1u);
        put32(p + 8, PartitionStart);
        put32(p + 12, g.partitionSectors);
        s[0x1fe] = 0x55;
        s[0x1ff] = 0xaa;
        writeSector(0, s.data());

        // FAT16 boot sector (fields GT4 reads: 0x0b, 0x0d, 0x0e, 0x10, 0x11, 0x13/0x20, 0x16, 0x1fe).
        s.fill(0);
        s[0] = 0xeb;
        s[1] = 0x3c;
        s[2] = 0x90;
        std::memcpy(&s[3], "MSDOS5.0", 8);
        put16(&s[0x0b], SectorBytes);
        s[0x0d] = static_cast<uint8_t>(g.sectorsPerCluster);
        put16(&s[0x0e], g.reservedSectors);
        s[0x10] = 2;
        put16(&s[0x11], RootEntries);
        put16(&s[0x13], g.partitionSectors < 0x10000u ? g.partitionSectors : 0u);
        s[0x15] = 0xf8;
        put16(&s[0x16], g.fatSectors);
        put16(&s[0x18], 63);
        put16(&s[0x1a], 255);
        put32(&s[0x1c], PartitionStart);
        put32(&s[0x20], g.partitionSectors < 0x10000u ? 0u : g.partitionSectors);
        s[0x24] = 0x80;
        s[0x26] = 0x29;
        put32(&s[0x27], VolumeSerial);
        std::memcpy(&s[0x2b], "NO NAME    ", 11);
        std::memcpy(&s[0x36], "FAT16   ", 8);
        s[0x3e] = 0xcd; // int 18h: not bootable
        s[0x3f] = 0x18;
        s[0x40] = 0xeb;
        s[0x41] = 0xfe;
        s[0x1fe] = 0x55;
        s[0x1ff] = 0xaa;
        writeSector(PartitionStart, s.data());

        ctx.fat.assign(size_t(g.clusters) + 2u, 0);
        ctx.fat[0] = 0xfff8; // media 0xf8
        ctx.fat[1] = 0xffff; // clean, no I/O errors
    }

    std::vector<UsbDisk::Child> UsbDisk::listDirectory(BuildContext &ctx, const fs::path &dir, int depth)
    {
        std::vector<Child> children;
        std::error_code ec;
        std::set<std::string> taken;
        const auto skip = [&](const fs::path &path, const char *why) {
            ++ctx.stats.skipped;
            if (ctx.skipLogs++ < MaxSkipLogs)
                log(false, "skip " + utf8(path) + ": " + why);
        };
        for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec))
        {
            const fs::directory_entry &entry = *it;
            std::error_code e2;
            if (entry.is_symlink(e2))
            {
                skip(entry.path(), "symbolic link");
                continue;
            }
            const bool directory = entry.is_directory(e2);
            if (!directory && !entry.is_regular_file(e2))
                continue;
            Child child;
            child.name = asciiName(entry.path());
            if (child.name.empty() || !makeShortName(child.name, child.shortName))
            {
                skip(entry.path(), "name has no 8.3 form");
                continue;
            }
            if (!taken.insert(std::string(child.shortName.raw.data(), 11)).second)
            {
                skip(entry.path(), "8.3 name already used in this folder");
                continue;
            }
            if (directory && depth + 1 > MaxDepth)
            {
                skip(entry.path(), "folder nested too deep");
                continue;
            }
            child.directory = directory;
            child.path = entry.path();
            child.time = entry.last_write_time(e2);
            if (!directory)
            {
                child.size = entry.file_size(e2);
                if (e2 || child.size > 0xffffffffull)
                {
                    skip(entry.path(), "too large for FAT");
                    continue;
                }
            }
            children.push_back(std::move(child));
        }
        std::sort(children.begin(), children.end(), [](const Child &a, const Child &b) {
            return std::lexicographical_compare(a.shortName.raw.begin(), a.shortName.raw.end(), b.shortName.raw.begin(),
                                                b.shortName.raw.end());
        });
        return children;
    }

    uint32_t UsbDisk::importDirectory(BuildContext &ctx, const fs::path &dir, uint32_t parentCluster,
                                      const std::string &keyPrefix, const std::string &hostPrefix, int depth,
                                      std::vector<Child> children, uint32_t entryCapacity)
    {
        const Geometry &g = geometry_;
        const uint32_t clusterBytes = g.sectorsPerCluster * SectorBytes;
        const bool root = depth == 0;
        uint32_t self = 0;
        std::vector<uint8_t> entries;
        const auto appendEntry = [&](const uint8_t *raw, uint8_t attr, uint8_t nt, uint32_t cluster, uint32_t size,
                                     uint16_t date, uint16_t time) {
            uint8_t e[32]{};
            std::memcpy(e, raw, 11);
            e[11] = attr;
            e[12] = nt;
            put16(e + 22, time);
            put16(e + 24, date);
            put16(e + 26, cluster);
            put32(e + 28, size);
            entries.insert(entries.end(), e, e + 32);
        };

        if (!root)
        {
            uint32_t needed = 2;
            for (const Child &child : children)
                needed += child.entries();
            const uint32_t clusterCount = std::max(1u, (needed * 32u + clusterBytes - 1u) / clusterBytes);
            self = ctx.allocate(clusterCount, g.clusters);
            if (!self)
                return 0;
            entryCapacity = clusterCount * clusterBytes / 32u;
            uint16_t date = 0, time = 0;
            std::error_code ec;
            fatTimestamp(fs::last_write_time(dir, ec), date, time);
            appendEntry(reinterpret_cast<const uint8_t *>(".          "), AttrDirectory, 0, self, 0, date, time);
            appendEntry(reinterpret_cast<const uint8_t *>("..         "), AttrDirectory, 0, parentCluster, 0, date, time);
        }

        for (Child &child : children)
        {
            const std::string key = keyPrefix + formatShort(reinterpret_cast<const uint8_t *>(child.shortName.raw.data()), 0);
            const std::string hostName = hostPrefix + child.name;
            if (entries.size() / 32u + child.entries() > entryCapacity)
            {
                ++ctx.stats.skipped;
                if (ctx.skipLogs++ < MaxSkipLogs)
                    log(true, "skip " + hostName + ": directory full");
                continue;
            }
            uint16_t date = 0, time = 0;
            fatTimestamp(child.time, date, time);
            uint32_t cluster = 0, entrySize = 0;
            if (child.directory)
            {
                std::vector<Child> sub = listDirectory(ctx, child.path, depth + 1);
                cluster = importDirectory(ctx, child.path, self, key + "/", hostName + "/", depth + 1, std::move(sub), 0);
                if (!cluster)
                {
                    ++ctx.stats.skipped;
                    if (ctx.skipLogs++ < MaxSkipLogs)
                        log(true, "skip " + hostName + "/: stick full");
                    continue;
                }
                ++ctx.stats.directories;
            }
            else
            {
                const uint32_t count = static_cast<uint32_t>((child.size + clusterBytes - 1u) / clusterBytes);
                cluster = ctx.allocate(count, g.clusters);
                if (count && !cluster)
                {
                    // The folder holds more than the stick: keep the name as an empty
                    // entry so GT4 does not reuse its number (the host file is never
                    // touched: the import record matches, so it is never exported).
                    ++ctx.stats.skipped;
                    if (ctx.skipLogs++ < MaxSkipLogs)
                        log(true, "stick full: " + hostName + " listed as an empty entry (its data does not fit)");
                }
                else
                {
                    entrySize = static_cast<uint32_t>(child.size);
                    if (count)
                        extents_.push_back({clusterLba(cluster), count * g.sectorsPerCluster, child.path});
                    ++ctx.stats.files;
                    ctx.stats.bytes += child.size;
                }
                Imported record;
                record.hostPath = hostName;
                record.fromHost = true;
                record.hostSize = child.size;
                record.hostTime = child.time;
                record.size = entrySize;
                record.firstCluster = cluster;
                record.date = date;
                record.time = time;
                imported_[key] = record;
            }
            if (child.shortName.lfn)
            {
                uint8_t e[32]{};
                e[0] = 0x41; // last (and only) LFN entry, sequence 1: 8.3 names fit 13 characters
                e[11] = AttrLfn;
                e[13] = lfnChecksum(reinterpret_cast<const uint8_t *>(child.shortName.raw.data()));
                for (size_t i = 0; i < LfnOffsets.size(); ++i)
                {
                    const uint32_t ch = i < child.name.size() ? static_cast<unsigned char>(child.name[i])
                                                              : (i == child.name.size() ? 0u : 0xffffu);
                    put16(e + LfnOffsets[i], ch);
                }
                entries.insert(entries.end(), e, e + 32);
            }
            appendEntry(reinterpret_cast<const uint8_t *>(child.shortName.raw.data()),
                        child.directory ? AttrDirectory : AttrArchive, child.shortName.nt, cluster, entrySize, date, time);
        }

        const uint64_t at = uint64_t(root ? g.rootLba : clusterLba(self)) * SectorBytes;
        if (!entries.empty())
            ctx.directoryWrites.emplace_back(at, std::move(entries));
        return root ? 1u : self;
    }

    UsbDisk::BuildStats UsbDisk::build(const fs::path &root, uint32_t sizeMiB)
    {
        clear();
        // Fingerprint first: a file that appears while the folder is imported
        // makes the next fn 5 see a change and rebuild, instead of being missed.
        std::string fingerprint = manifest(root);
        geometry_ = computeGeometry(sizeMiB);
        built_ = true; // reads/writes below go through the normal sector path
        BuildContext ctx;
        formatEmpty(ctx);
        std::error_code ec;
        if (fs::is_directory(root, ec))
        {
            std::vector<Child> top = listDirectory(ctx, root, 0);
            importDirectory(ctx, root, 0, std::string(), std::string(), 0, std::move(top), RootEntries);
        }
        // Directory contents and FATs last, after every file extent is known, so a
        // page shared between a directory cluster and file data is filled correctly.
        for (const auto &[offset, bytes] : ctx.directoryWrites)
            writeBytes(offset, bytes.data(), bytes.size());
        const uint32_t entriesPerSector = SectorBytes / 2u;
        for (uint32_t sector = 0; sector < geometry_.fatSectors; ++sector)
        {
            const size_t first = size_t(sector) * entriesPerSector;
            if (first >= ctx.fat.size())
                break;
            const size_t last = std::min(ctx.fat.size(), first + entriesPerSector);
            if (std::all_of(ctx.fat.begin() + first, ctx.fat.begin() + last, [](uint16_t v) { return v == 0; }))
                continue;
            std::array<uint8_t, SectorBytes> s{};
            for (size_t i = first; i < last; ++i)
                put16(&s[(i - first) * 2u], ctx.fat[i]);
            writeSector(geometry_.fatLba + sector, s.data());
            writeSector(geometry_.fatLba + geometry_.fatSectors + sector, s.data());
        }
        manifest_ = std::move(fingerprint);
        return ctx.stats;
    }

    std::vector<UsbDisk::ChangedFile> UsbDisk::collectChanges(std::vector<std::string> *problems) const
    {
        std::vector<ChangedFile> out;
        if (!built_)
            return out;
        std::set<uint32_t> visited;

        struct Walker
        {
            const UsbDisk &disk;
            std::vector<std::string> *problems;
            std::set<uint32_t> &visited;
            std::vector<ChangedFile> &out;

            void problem(const std::string &text) const
            {
                if (problems)
                    problems->push_back(text);
            }

            bool validCluster(uint32_t c) const { return c >= 2u && c < disk.geometry_.clusters + 2u; }

            void walk(uint32_t cluster, const std::string &keyPrefix, const std::string &hostPrefix, int depth)
            {
                const Geometry &g = disk.geometry_;
                std::vector<uint32_t> sectors;
                if (cluster == 0)
                {
                    for (uint32_t i = 0; i < g.rootSectors; ++i)
                        sectors.push_back(g.rootLba + i);
                }
                else
                {
                    for (uint32_t c = cluster, n = 0; n <= g.clusters; ++n)
                    {
                        if (!validCluster(c))
                        {
                            problem("directory " + keyPrefix + ": broken cluster chain at " + std::to_string(c));
                            break;
                        }
                        if (!visited.insert(c).second)
                        {
                            problem("directory " + keyPrefix + ": cluster " + std::to_string(c) + " used twice");
                            break;
                        }
                        for (uint32_t i = 0; i < g.sectorsPerCluster; ++i)
                            sectors.push_back(disk.clusterLba(c) + i);
                        c = disk.fatEntry(c);
                        if (c >= 0xfff8u)
                            break;
                    }
                }
                std::vector<uint8_t> data(sectors.size() * SectorBytes);
                for (size_t i = 0; i < sectors.size(); ++i)
                    disk.readSector(sectors[i], data.data() + i * SectorBytes);

                std::array<uint16_t, 13 * 20> lfn{};
                int lfnNext = 0;
                uint8_t lfnSum = 0;
                bool lfnValid = false;
                for (size_t at = 0; at + 32u <= data.size(); at += 32u)
                {
                    const uint8_t *e = data.data() + at;
                    if (e[0] == 0)
                        break;
                    if (e[0] == 0xe5)
                    {
                        lfnValid = false;
                        continue;
                    }
                    if ((e[11] & 0x3fu) == AttrLfn)
                    {
                        const int order = e[0] & 0x3f;
                        if (e[0] & 0x40)
                        {
                            lfn.fill(0xffff);
                            lfnValid = order >= 1 && order <= 20;
                            lfnSum = e[13];
                        }
                        else if (!lfnValid || order != lfnNext || e[13] != lfnSum)
                        {
                            lfnValid = false;
                        }
                        if (lfnValid)
                        {
                            for (size_t i = 0; i < LfnOffsets.size(); ++i)
                                lfn[size_t(order - 1) * 13u + i] = static_cast<uint16_t>(get16(e + LfnOffsets[i]));
                            lfnNext = order - 1;
                        }
                        continue;
                    }
                    const bool longName = lfnValid && lfnNext == 0 && lfnChecksum(e) == lfnSum;
                    lfnValid = false;
                    if (e[11] & AttrVolume)
                        continue;
                    if (e[0] == '.' && (e[1] == ' ' || (e[1] == '.' && e[2] == ' ')))
                        continue;
                    std::string display = formatShort(e, e[12]);
                    if (longName)
                    {
                        std::string name;
                        for (uint16_t ch : lfn)
                        {
                            if (ch == 0 || ch == 0xffff)
                                break;
                            name.push_back(ch < 0x80 ? static_cast<char>(ch) : '_');
                        }
                        if (!name.empty())
                            display = name;
                    }
                    const std::string key = keyPrefix + formatShort(e, 0);
                    const std::string host = hostPrefix + display;
                    const uint32_t first = get16(e + 26);
                    if (e[11] & AttrDirectory)
                    {
                        if (depth >= 16 || !validCluster(first))
                        {
                            problem("directory " + key + ": bad first cluster " + std::to_string(first));
                            continue;
                        }
                        walk(first, key + "/", host + "/", depth + 1);
                        continue;
                    }
                    const uint32_t size = get32(e + 28);
                    if (uint64_t(size) > uint64_t(g.clusters) * g.sectorsPerCluster * SectorBytes)
                    {
                        problem("file " + key + ": size " + std::to_string(size) + " larger than the stick");
                        continue;
                    }
                    const uint16_t date = static_cast<uint16_t>(get16(e + 24)), time = static_cast<uint16_t>(get16(e + 22));
                    if (const auto it = disk.imported_.find(key); it != disk.imported_.end() && it->second.size == size &&
                                                                     it->second.firstCluster == first &&
                                                                     it->second.date == date && it->second.time == time)
                    {
                        continue;
                    }
                    ChangedFile file;
                    file.key = key;
                    file.hostPath = host;
                    file.firstCluster = first;
                    file.date = date;
                    file.time = time;
                    // The data grows with the chain, so a corrupt size field never
                    // allocates more than the chain really holds.
                    file.data.reserve(static_cast<size_t>(std::min<uint32_t>(size, 1u << 20)));
                    uint32_t c = first;
                    std::set<uint32_t> chain;
                    for (uint64_t done = 0; done < size;)
                    {
                        if (!validCluster(c) || !chain.insert(c).second)
                        {
                            problem("file " + key + ": cluster chain ends after " + std::to_string(done) + " of " +
                                    std::to_string(size) + " bytes");
                            break;
                        }
                        std::array<uint8_t, SectorBytes> sector{};
                        for (uint32_t i = 0; i < g.sectorsPerCluster && done < size; ++i)
                        {
                            disk.readSector(disk.clusterLba(c) + i, sector.data());
                            const size_t n = static_cast<size_t>(std::min<uint64_t>(SectorBytes, size - done));
                            file.data.insert(file.data.end(), sector.data(), sector.data() + n);
                            done += n;
                        }
                        if (done < size)
                        {
                            c = disk.fatEntry(c);
                            if (c >= 0xfff8u)
                            {
                                problem("file " + key + ": chain shorter than its size " + std::to_string(size));
                                break;
                            }
                        }
                    }
                    out.push_back(std::move(file));
                }
            }
        };

        Walker walker{*this, problems, visited, out};
        walker.walk(0, std::string(), std::string(), 0);
        return out;
    }

    void UsbDisk::acknowledge(const ChangedFile &file)
    {
        Imported record;
        record.hostPath = file.hostPath;
        record.size = static_cast<uint32_t>(file.data.size());
        record.firstCluster = file.firstCluster;
        record.date = file.date;
        record.time = file.time;
        imported_[file.key] = record;
    }

    void UsbDisk::forget(const std::string &key)
    {
        imported_.erase(key);
    }

    std::optional<UsbDisk::Imported> UsbDisk::imported(const std::string &key) const
    {
        if (const auto it = imported_.find(key); it != imported_.end())
            return it->second;
        return std::nullopt;
    }

    std::string UsbDisk::checkLikeGt4() const
    {
        if (!built_)
            return "no image";
        std::array<uint8_t, SectorBytes> s{};
        readSector(0, s.data());
        uint32_t start = 0;
        // 0x4c9d30: MBR if 0xaa55 and status byte 0x1be & 0x7f == 0; 0x4c9e68: first entry
        // with status & 0x7f == 0, nonzero start, type in {1, 4, 6, 0x0b, 0x0c, 0x0e}.
        if (get16(&s[0x1fe]) == 0xaa55u && (s[0x1be] & 0x7fu) == 0)
        {
            for (int i = 0; i < 4; ++i)
            {
                const uint8_t *p = &s[0x1be + i * 16];
                const uint32_t lba = get32(p + 8);
                const uint8_t type = p[4];
                if ((p[0] & 0x7fu) == 0 && lba != 0 &&
                    (type == 1 || type == 4 || type == 6 || type == 0x0b || type == 0x0c || type == 0x0e))
                {
                    start = lba;
                    break;
                }
            }
        }
        if (start != PartitionStart || s[0x1be + 4] != 0x0e)
            return "partition: start " + std::to_string(start) + " type " + std::to_string(s[0x1be + 4]);
        // 0x4c9f10: boot sector rules.
        readSector(start, s.data());
        if (get16(&s[0x1fe]) != 0xaa55u)
            return "boot sector: no 0xaa55";
        const uint32_t spc = s[0x0d];
        if (spc == 0 || (spc & (spc - 1u)) != 0)
            return "boot sector: sectors per cluster not a power of two";
        if (get16(&s[0x0b]) != SectorBytes)
            return "boot sector: bytes per sector != block size 512";
        uint32_t fat = get16(&s[0x16]);
        if (!fat)
            fat = get32(&s[0x24]);
        const uint32_t root = get16(&s[0x0e]) + s[0x10] * fat;
        const uint32_t rootSectors = (get16(&s[0x11]) * 32u + SectorBytes - 1u) / SectorBytes;
        const uint32_t firstData = root + rootSectors;
        uint32_t total = get16(&s[0x13]);
        if (!total)
            total = get32(&s[0x20]);
        if (total <= firstData)
            return "boot sector: no data area";
        const uint32_t clustersPlus2 = (total - firstData) / spc + 2u;
        if (clustersPlus2 < 0xff7u)
            return "FAT12 (GT4: mount error)";
        if (clustersPlus2 > 0xfff6u)
            return "FAT32, expected FAT16";
        if (uint64_t(fat) * (SectorBytes / 2u) < clustersPlus2)
            return "FAT too small for the cluster count";
        if (start + total > geometry_.totalSectors)
            return "volume larger than the disk";
        return {};
    }

    std::string UsbDisk::manifest(const fs::path &root)
    {
        std::string out;
        std::error_code ec;
        if (!fs::is_directory(root, ec))
            return out;
        std::vector<std::pair<fs::path, int>> stack{{root, 0}};
        std::vector<std::string> lines;
        while (!stack.empty())
        {
            auto [dir, depth] = stack.back();
            stack.pop_back();
            for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec))
            {
                std::error_code e2;
                if (it->is_symlink(e2))
                    continue;
                std::string line = utf8(fs::relative(it->path(), root, e2));
                if (it->is_directory(e2))
                {
                    lines.push_back(line + "|d");
                    if (depth + 1 <= MaxDepth)
                        stack.emplace_back(it->path(), depth + 1);
                }
                else if (it->is_regular_file(e2))
                {
                    const auto size = it->file_size(e2);
                    const auto time = it->last_write_time(e2).time_since_epoch().count();
                    lines.push_back(line + "|" + std::to_string(size) + "|" + std::to_string(static_cast<long long>(time)));
                }
            }
        }
        std::sort(lines.begin(), lines.end());
        for (const std::string &line : lines)
            out += line + "\n";
        return out;
    }

    namespace
    {
        constexpr const wchar_t *TemporarySuffix = L".pust-tmp";

        std::wstring pathKey(const fs::path &path)
        {
            std::wstring key = path.lexically_normal().wstring();
            for (wchar_t &c : key)
                c = static_cast<wchar_t>(std::towlower(c));
            return key;
        }

        // One FAT name component -> a name Windows accepts.
        std::string hostComponent(std::string name)
        {
            for (char &c : name)
            {
                const unsigned char u = static_cast<unsigned char>(c);
                if (u < 0x20 || u >= 0x7f || std::strchr("<>:\"/\\|?*", c))
                    c = '_';
            }
            while (!name.empty() && (name.back() == ' ' || name.back() == '.'))
                name.pop_back();
            if (name.empty())
                return "_";
            std::string base = name.substr(0, name.find('.'));
            for (char &c : base)
                if (c >= 'a' && c <= 'z')
                    c = static_cast<char>(c - 0x20);
            static const char *const reserved[] = {"CON", "PRN", "AUX", "NUL", "COM1", "COM2", "COM3", "COM4", "COM5",
                                                   "COM6", "COM7", "COM8", "COM9", "LPT1", "LPT2", "LPT3", "LPT4",
                                                   "LPT5", "LPT6", "LPT7", "LPT8", "LPT9"};
            for (const char *r : reserved)
                if (base == r)
                    return "_" + name;
            return name;
        }
    }

    std::vector<UsbFolderWriter::Outcome> UsbFolderWriter::write(const fs::path &root, const std::vector<Item> &items)
    {
        std::vector<Outcome> outcomes;
        for (const Item &item : items)
        {
            Outcome outcome;
            outcome.key = item.file.key;
            fs::path target = root;
            for (size_t begin = 0; begin <= item.file.hostPath.size();)
            {
                size_t end = item.file.hostPath.find('/', begin);
                if (end == std::string::npos)
                    end = item.file.hostPath.size();
                target /= fromUtf8(hostComponent(item.file.hostPath.substr(begin, end - begin)));
                begin = end + 1;
            }
            std::error_code ec;
            fs::create_directories(target.parent_path(), ec);
            if (ec)
            {
                outcome.message = "cannot create folder " + utf8(target.parent_path()) + ": " + ec.message();
                outcomes.push_back(std::move(outcome));
                continue;
            }
            const bool exists = fs::exists(target, ec);
            if (ec)
            {
                // Unknown state: never risk replacing a host file.
                outcome.message = "cannot check " + utf8(target) + ": " + ec.message();
                outcomes.push_back(std::move(outcome));
                continue;
            }
            if (exists)
            {
                bool allowed = false;
                if (fs::is_regular_file(target, ec))
                {
                    const uint64_t size = fs::file_size(target, ec);
                    const auto time = fs::last_write_time(target, ec);
                    if (item.imported && item.imported->fromHost && item.imported->hostSize == size &&
                        item.imported->hostTime == time)
                        allowed = true; // the file GT4 saw at mount, unchanged on the host since
                    if (const auto it = written_.find(pathKey(target));
                        it != written_.end() && it->second.size == size && it->second.time == time)
                        allowed = true; // written by this exporter, unchanged since
                }
                if (!allowed)
                {
                    const fs::path original = target;
                    const std::wstring stem = original.stem().wstring(), extension = original.extension().wstring();
                    for (int n = 1; n < 1000 && fs::exists(target, ec); ++n)
                        target = original.parent_path() / (stem + L"~" + std::to_wstring(n) + extension);
                    outcome.message = "host file " + utf8(original.filename()) + " changed since mount; kept it and wrote " +
                                      utf8(target.filename());
                    if (fs::exists(target, ec))
                    {
                        outcome.message = "no free alternate name for " + item.file.hostPath;
                        outcomes.push_back(std::move(outcome));
                        continue;
                    }
                }
            }
            fs::path temporary = target;
            temporary += TemporarySuffix;
            // Only ever a regular file this writer creates (or left behind by a crash) is
            // replaced or removed; anything else under that name is left alone. The
            // suffix is the writer's own, so no file a user keeps there is touched.
            const auto removeTemporary = [&]() {
                std::error_code e2;
                if (fs::is_regular_file(fs::symlink_status(temporary, e2)))
                    fs::remove(temporary, e2);
            };
            if (fs::exists(fs::symlink_status(temporary, ec)) && !fs::is_regular_file(fs::symlink_status(temporary, ec)))
            {
                outcome.message = "cannot write " + item.file.hostPath + ": " + utf8(temporary.filename()) + " is in the way";
                outcomes.push_back(std::move(outcome));
                continue;
            }
            {
                std::ofstream file(temporary, std::ios::binary | std::ios::trunc);
                if (file)
                    file.write(reinterpret_cast<const char *>(item.file.data.data()), static_cast<std::streamsize>(item.file.data.size()));
                file.flush();
                if (!file)
                {
                    file.close();
                    removeTemporary();
                    outcome.message = "cannot write " + item.file.hostPath;
                    outcomes.push_back(std::move(outcome));
                    continue;
                }
            }
            fs::rename(temporary, target, ec);
            if (ec)
            {
                removeTemporary();
                outcome.message = "cannot rename into " + item.file.hostPath + ": " + ec.message();
                outcomes.push_back(std::move(outcome));
                continue;
            }
            Written record;
            record.size = fs::file_size(target, ec);
            record.time = fs::last_write_time(target, ec);
            written_[pathKey(target)] = record;
            outcome.ok = true;
            outcome.written = target;
            outcomes.push_back(std::move(outcome));
        }
        return outcomes;
    }
}
