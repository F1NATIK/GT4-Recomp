#include "gt4stock_saves.h"

#include "gt4stock_util.h"

#include "raylib.h" // CompressData / DecompressData (raw DEFLATE, raylib's sdefl / sinfl)

#include <algorithm>
#include <chrono>
#include <cstring>
#include <ctime>
#include <fstream>

namespace gt4stock
{
    // ------------------------------------------------------------------ icon.sys
    std::string sjisTitle(const uint8_t *t, size_t n, size_t lineBreak)
    {
        std::string out;
        size_t i = 0;
        while (i < n && t[i])
        {
            if (lineBreak && i == lineBreak && !out.empty() && out.back() != ' ')
                out += ' ';
            const uint8_t c = t[i];
            if (c < 0x80)
            {
                out += static_cast<char>(c);
                ++i;
                continue;
            }
            if (i + 1 >= n)
                break;
            const unsigned code = (static_cast<unsigned>(c) << 8) | t[i + 1];
            i += 2;
            if (code == 0x8140)
                out += ' ';
            else if (code >= 0x824f && code <= 0x8258)
                out += static_cast<char>('0' + (code - 0x824f));
            else if (code >= 0x8260 && code <= 0x8279)
                out += static_cast<char>('A' + (code - 0x8260));
            else if (code >= 0x8281 && code <= 0x829a)
                out += static_cast<char>('a' + (code - 0x8281));
            else
            {
                static const struct { unsigned code; char c; } punct[] = {
                    {0x8143, ','}, {0x8144, '.'}, {0x8146, ':'}, {0x8147, ';'}, {0x8148, '?'}, {0x8149, '!'}, {0x815e, '/'},
                    {0x817c, '-'}, {0x815b, '-'}, {0x8169, '('}, {0x816a, ')'}, {0x8166, '\''}, {0x8168, '"'}, {0x817b, '+'},
                    {0x8195, '&'}, {0x8194, '#'}, {0x8193, '%'}, {0x8181, '='}};
                char c2 = '?';
                for (const auto &p : punct)
                    if (p.code == code)
                        c2 = p.c;
                out += c2;
            }
        }
        // Collapse double spaces.
        std::string clean;
        for (char ch : out)
            if (!(ch == ' ' && !clean.empty() && clean.back() == ' '))
                clean += ch;
        return trim(clean);
    }

    namespace
    {
        std::string iconTitle(const fs::path &folder)
        {
            std::vector<uint8_t> d;
            if (!readFile(folder / "icon.sys", d) || d.size() < 0xc0 + 68 || std::memcmp(d.data(), "PS2D", 4) != 0)
                return {};
            const size_t lineBreak = static_cast<size_t>(d[6] | (d[7] << 8));
            return sjisTitle(d.data() + 0xc0, 68, lineBreak);
        }

        fs::file_time_type newestIn(const fs::path &dir)
        {
            fs::file_time_type newest{};
            std::error_code ec;
            for (fs::recursive_directory_iterator it(dir, fs::directory_options::skip_permission_denied, ec), end; !ec && it != end;
                 it.increment(ec))
            {
                std::error_code e2;
                const auto t = it->last_write_time(e2);
                if (!e2 && t > newest)
                    newest = t;
            }
            return newest;
        }
    }

    CardInfo scanCard(const fs::path &dir, int slot)
    {
        CardInfo c;
        c.slot = slot;
        c.dir = dir;
        std::error_code ec;
        c.exists = fs::is_directory(dir, ec);
        if (!c.exists)
            return c;
        c.bytes = folderBytes(dir, &c.files);
        for (const auto &entry : fs::directory_iterator(dir, fs::directory_options::skip_permission_denied, ec))
        {
            std::error_code e2;
            if (!entry.is_directory(e2))
                continue;
            SaveEntry s;
            s.folder = pathText(entry.path().filename());
            s.gt4 = s.folder == kGt4SaveFolder;
            s.gt3 = startsWith(s.folder, "BASCUS-97102");
            s.title = iconTitle(entry.path());
            s.bytes = folderBytes(entry.path(), &s.files);
            s.changed = newestIn(entry.path());
            s.when = timeText(s.changed);
            if (s.gt4)
            {
                c.gt4Save = true;
                c.gt4 = s;
            }
            c.saves.push_back(s);
        }
        std::stable_sort(c.saves.begin(), c.saves.end(), [](const SaveEntry &a, const SaveEntry &b) { return a.gt4 && !b.gt4; });
        return c;
    }

    std::vector<BackupInfo> listBackups(const fs::path &backupDir)
    {
        std::vector<BackupInfo> out;
        std::error_code ec;
        for (const auto &entry : fs::directory_iterator(backupDir, fs::directory_options::skip_permission_denied, ec))
        {
            std::error_code e2;
            if (!entry.is_regular_file(e2))
                continue;
            const std::string name = pathText(entry.path().filename());
            if (!startsWith(lower(name), "gt4-mc") || !endsWith(lower(name), ".zip"))
                continue;
            BackupInfo b;
            b.file = entry.path();
            b.bytes = entry.file_size(e2);
            b.time = entry.last_write_time(e2);
            b.slot = name.size() > 7 && name[6] >= '0' && name[6] <= '9' ? (name[6] - '0') + 1 : 0;
            b.beforeRestore = lower(name).find("before-restore") != std::string::npos;
            out.push_back(b);
        }
        for (const auto &entry : fs::directory_iterator(backupDir, fs::directory_options::skip_permission_denied, ec))
        {
            std::error_code e2;
            const std::string name = pathText(entry.path().filename());
            if (!entry.is_regular_file(e2) || !startsWith(lower(name), "gt4-mc") || !endsWith(lower(name), ".ps2"))
                continue;
            BackupInfo b;
            b.file = entry.path();
            b.bytes = entry.file_size(e2);
            b.time = entry.last_write_time(e2);
            b.slot = name.size() > 7 && name[6] >= '0' && name[6] <= '9' ? (name[6] - '0') + 1 : 0;
            b.beforeRestore = lower(name).find("before-restore") != std::string::npos;
            b.cardFile = true;
            out.push_back(b);
        }
        std::sort(out.begin(), out.end(), [](const BackupInfo &a, const BackupInfo &b) { return a.time > b.time; });
        return out;
    }

    // ------------------------------------------------------------------ zip
    namespace
    {
        void put16(std::vector<uint8_t> &v, uint32_t x)
        {
            v.push_back(static_cast<uint8_t>(x));
            v.push_back(static_cast<uint8_t>(x >> 8));
        }
        void put32(std::vector<uint8_t> &v, uint32_t x)
        {
            put16(v, x & 0xffff);
            put16(v, x >> 16);
        }
        uint32_t get16(const uint8_t *p) { return static_cast<uint32_t>(p[0] | (p[1] << 8)); }
        uint32_t get32(const uint8_t *p) { return get16(p) | (get16(p + 2) << 16); }

        void dosTime(fs::file_time_type t, uint32_t &time, uint32_t &date)
        {
            std::time_t tt = std::time(nullptr);
            if (t != fs::file_time_type{})
                tt = std::chrono::system_clock::to_time_t(std::chrono::clock_cast<std::chrono::system_clock>(t));
            std::tm tm{};
#if defined(_WIN32)
            localtime_s(&tm, &tt);
#else
            localtime_r(&tt, &tm);
#endif
            const int year = std::clamp(tm.tm_year + 1900, 1980, 2107);
            time = static_cast<uint32_t>((tm.tm_hour << 11) | (tm.tm_min << 5) | (tm.tm_sec / 2));
            date = static_cast<uint32_t>(((year - 1980) << 9) | ((tm.tm_mon + 1) << 5) | tm.tm_mday);
        }

        constexpr size_t kMaxTotal = 64u * 1024u * 1024u; // a PS2 card holds 8 MB
        constexpr size_t kMaxEntries = 8192;
    }

    bool safeZipName(const std::string &name)
    {
        if (name.empty() || name.size() > 512 || name[0] == '/' || name[0] == '\\' || name.find(':') != std::string::npos)
            return false;
        for (const std::string &part : split(name, '/'))
        {
            if (part == ".." || part.find('\\') != std::string::npos)
                return false;
            for (char ch : part)
                if (static_cast<unsigned char>(ch) < 0x20)
                    return false;
        }
        return true;
    }

    bool writeZip(const fs::path &zip, const std::vector<ZipItem> &items, std::string &error)
    {
        std::vector<uint8_t> out, central;
        for (const ZipItem &it : items)
        {
            if (!safeZipName(it.name))
            {
                error = "bad name " + it.name;
                return false;
            }
            const bool dir = !it.name.empty() && it.name.back() == '/';
            const uint32_t crc = crc32(it.data.data(), it.data.size());
            std::vector<uint8_t> stored;
            uint32_t method = 0;
            const uint8_t *payload = it.data.data();
            size_t payloadSize = it.data.size();
            unsigned char *packed = nullptr;
            if (!dir && it.data.size() > 64 && it.data.size() < 0x7fffffffu)
            {
                int packedSize = 0;
                packed = CompressData(it.data.data(), static_cast<int>(it.data.size()), &packedSize);
                if (packed && packedSize > 0 && static_cast<size_t>(packedSize) < it.data.size())
                {
                    method = 8;
                    payload = packed;
                    payloadSize = static_cast<size_t>(packedSize);
                }
            }
            uint32_t t = 0, d = 0;
            dosTime(it.time, t, d);
            const uint32_t offset = static_cast<uint32_t>(out.size());
            // local header
            put32(out, 0x04034b50u);
            put16(out, 20);
            put16(out, 0x0800); // UTF-8 names
            put16(out, method);
            put16(out, t);
            put16(out, d);
            put32(out, crc);
            put32(out, static_cast<uint32_t>(payloadSize));
            put32(out, static_cast<uint32_t>(it.data.size()));
            put16(out, static_cast<uint32_t>(it.name.size()));
            put16(out, 0);
            out.insert(out.end(), it.name.begin(), it.name.end());
            out.insert(out.end(), payload, payload + payloadSize);
            if (packed)
                MemFree(packed);
            // central directory entry
            put32(central, 0x02014b50u);
            put16(central, 20);
            put16(central, 20);
            put16(central, 0x0800);
            put16(central, method);
            put16(central, t);
            put16(central, d);
            put32(central, crc);
            put32(central, static_cast<uint32_t>(payloadSize));
            put32(central, static_cast<uint32_t>(it.data.size()));
            put16(central, static_cast<uint32_t>(it.name.size()));
            put16(central, 0);
            put16(central, 0);
            put16(central, 0);
            put16(central, 0);
            put32(central, dir ? 0x10u : 0x20u); // MS-DOS directory / archive attribute
            put32(central, offset);
            central.insert(central.end(), it.name.begin(), it.name.end());
            if (out.size() > 0x7fffffffu)
            {
                error = "the backup is too large";
                return false;
            }
        }
        const uint32_t cdOffset = static_cast<uint32_t>(out.size());
        out.insert(out.end(), central.begin(), central.end());
        put32(out, 0x06054b50u);
        put16(out, 0);
        put16(out, 0);
        put16(out, static_cast<uint32_t>(items.size()));
        put16(out, static_cast<uint32_t>(items.size()));
        put32(out, static_cast<uint32_t>(central.size()));
        put32(out, cdOffset);
        put16(out, 0);
        return writeFileAtomic(zip, std::string_view(reinterpret_cast<const char *>(out.data()), out.size()), error);
    }

    bool readZip(const fs::path &zip, std::vector<ZipItem> &items, std::string &error)
    {
        std::vector<uint8_t> f;
        if (!readFile(zip, f))
        {
            error = "cannot read " + pathText(zip);
            return false;
        }
        if (f.size() < 22)
        {
            error = "not a zip file";
            return false;
        }
        size_t eocd = std::string::npos;
        const size_t lowest = f.size() > 22 + 65535 ? f.size() - 22 - 65535 : 0;
        for (size_t i = f.size() - 22 + 1; i-- > lowest;)
            if (get32(&f[i]) == 0x06054b50u)
            {
                eocd = i;
                break;
            }
        if (eocd == std::string::npos)
        {
            error = "not a zip file (no end record)";
            return false;
        }
        const uint32_t count = get16(&f[eocd + 10]);
        const uint32_t cdSize = get32(&f[eocd + 12]);
        const uint32_t cdOffset = get32(&f[eocd + 16]);
        if (count > kMaxEntries || static_cast<uint64_t>(cdOffset) + cdSize > f.size())
        {
            error = "damaged zip file (directory)";
            return false;
        }
        size_t total = 0;
        size_t p = cdOffset;
        std::vector<ZipItem> out;
        for (uint32_t e = 0; e < count; ++e)
        {
            if (p + 46 > f.size() || get32(&f[p]) != 0x02014b50u)
            {
                error = "damaged zip file (entry)";
                return false;
            }
            const uint32_t flags = get16(&f[p + 8]);
            const uint32_t method = get16(&f[p + 10]);
            const uint32_t crc = get32(&f[p + 16]);
            const uint32_t csize = get32(&f[p + 20]);
            const uint32_t usize = get32(&f[p + 24]);
            const uint32_t nlen = get16(&f[p + 28]), xlen = get16(&f[p + 30]), clen = get16(&f[p + 32]);
            const uint32_t local = get32(&f[p + 42]);
            if (p + 46 + nlen > f.size())
            {
                error = "damaged zip file (name)";
                return false;
            }
            ZipItem it;
            it.name.assign(reinterpret_cast<const char *>(&f[p + 46]), nlen);
            std::replace(it.name.begin(), it.name.end(), '\\', '/');
            p += 46 + nlen + xlen + clen;
            if (flags & 1u)
            {
                error = it.name + " is encrypted";
                return false;
            }
            if (!safeZipName(it.name))
            {
                error = "unsafe name in the zip: " + it.name;
                return false;
            }
            if (static_cast<uint64_t>(local) + 30 > f.size() || get32(&f[local]) != 0x04034b50u)
            {
                error = "damaged zip file (" + it.name + ")";
                return false;
            }
            const size_t data = local + 30 + get16(&f[local + 26]) + get16(&f[local + 28]);
            if (static_cast<uint64_t>(data) + csize > f.size())
            {
                error = "damaged zip file (" + it.name + " data)";
                return false;
            }
            total += usize;
            if (total > kMaxTotal)
            {
                error = "the zip is larger than a memory card could be";
                return false;
            }
            if (method == 0)
            {
                if (csize != usize)
                {
                    error = "damaged zip file (" + it.name + " size)";
                    return false;
                }
                it.data.assign(f.begin() + static_cast<std::ptrdiff_t>(data), f.begin() + static_cast<std::ptrdiff_t>(data + csize));
            }
            else if (method == 8)
            {
                if (usize > 0)
                {
                    int got = 0;
                    unsigned char *u = DecompressData(&f[data], static_cast<int>(csize), &got);
                    if (!u || got < 0 || static_cast<uint32_t>(got) != usize)
                    {
                        if (u)
                            MemFree(u);
                        error = "damaged zip file (" + it.name + " does not unpack)";
                        return false;
                    }
                    it.data.assign(u, u + got);
                    MemFree(u);
                }
            }
            else
            {
                error = strf("%s uses compression method %u (only stored and deflate are read)", it.name.c_str(), method);
                return false;
            }
            if (crc32(it.data.data(), it.data.size()) != crc)
            {
                error = "CRC error in " + it.name;
                return false;
            }
            out.push_back(std::move(it));
        }
        items = std::move(out);
        return true;
    }

    // ------------------------------------------------------------------ backup / restore
    bool backupCard(const fs::path &cardDir, int slot, const fs::path &backupDir, const std::string &version, const std::string &suffix,
                    fs::path &zipOut, std::string &error)
    {
        std::error_code ec;
        if (!fs::is_directory(cardDir, ec))
        {
            error = "the card folder " + pathText(cardDir) + " does not exist";
            return false;
        }
        const std::string top = strf("mc%d", slot - 1);
        std::vector<ZipItem> items;
        ZipItem manifest;
        manifest.name = "GT4-BACKUP.txt";
        const std::string text = "GT4 Recompilation launcher " + version + ": backup of memory card slot " + std::to_string(slot) + " (" +
                                 pathText(cardDir) + "), made " + nowText() + ".\r\nRestore it on the launcher's Saves page, or copy the " +
                                 top + " folder in this zip over the card folder (game\\" + top + ") while GT4 is closed.\r\n";
        manifest.data.assign(text.begin(), text.end());
        items.push_back(manifest);
        for (fs::recursive_directory_iterator it(cardDir, fs::directory_options::skip_permission_denied, ec), end; !ec && it != end;
             it.increment(ec))
        {
            std::error_code e2;
            const fs::path rel = fs::relative(it->path(), cardDir, e2);
            if (e2)
                continue;
            std::string name = top + "/" + pathText(rel);
            std::replace(name.begin(), name.end(), '\\', '/');
            ZipItem z;
            z.time = it->last_write_time(e2);
            if (it->is_directory(e2))
            {
                z.name = name + "/";
                items.push_back(std::move(z));
                continue;
            }
            if (!it->is_regular_file(e2))
                continue;
            z.name = name;
            if (!readFile(it->path(), z.data))
            {
                error = "cannot read " + pathText(it->path());
                return false;
            }
            items.push_back(std::move(z));
        }
        if (ec)
        {
            error = "cannot list " + pathText(cardDir) + ": " + ec.message();
            return false;
        }
        fs::create_directories(backupDir, ec);
        const std::string stamp = nowFileStamp();
        zipOut = backupDir / pathFrom(strf("GT4-mc%d-%s%s.zip", slot - 1, stamp.c_str(), suffix.c_str()));
        for (int n = 2; fs::exists(zipOut, ec) && n < 100; ++n) // two backups in one second
            zipOut = backupDir / pathFrom(strf("GT4-mc%d-%s-%d%s.zip", slot - 1, stamp.c_str(), n, suffix.c_str()));
        if (!writeZip(zipOut, items, error))
            return false;
        // Read it back: a backup that does not unpack is no backup.
        std::vector<ZipItem> check;
        if (!readZip(zipOut, check, error) || check.size() != items.size())
        {
            error = "the new backup does not read back: " + error;
            return false;
        }
        return true;
    }

    bool restoreCard(const fs::path &zip, const fs::path &cardDir, int slot, const fs::path &backupDir, const std::string &version,
                     std::string &report, std::string &error)
    {
        std::vector<ZipItem> items;
        if (!readZip(zip, items, error))
            return false;
        // The card's files: everything under the first folder named mc<n>.
        std::vector<ZipItem> files;
        for (ZipItem &it : items)
        {
            const size_t slash = it.name.find('/');
            if (slash == std::string::npos)
                continue; // the manifest
            const std::string top = lower(it.name.substr(0, slash));
            if (top.size() < 3 || !startsWith(top, "mc"))
                continue;
            it.name = it.name.substr(slash + 1);
            if (!it.name.empty())
                files.push_back(std::move(it));
        }
        if (files.empty())
        {
            error = "this zip holds no memory card (no mc0 / mc1 folder)";
            return false;
        }
        std::error_code ec;
        const std::string stamp = nowFileStamp();
        report.clear();
        // 1. The card as it is now, backed up.
        if (fs::is_directory(cardDir, ec) && folderBytes(cardDir) > 0)
        {
            fs::path before;
            if (!backupCard(cardDir, slot, backupDir, version, "-before-restore", before, error))
            {
                error = "nothing was changed: the backup of the card as it is failed (" + error + ")";
                return false;
            }
            report += "Backed up the card as it was to " + pathText(before.filename()) + ". ";
        }
        // 2. The zip's files into a new folder next to the card.
        // New names every time (two restores in one second must not meet).
        auto unique = [&](const fs::path &first) {
            fs::path p = first;
            for (int n = 2; fs::exists(p, ec) && n < 100; ++n)
            {
                p = first;
                p += pathFrom("-" + std::to_string(n));
            }
            return p;
        };
        fs::path fresh = cardDir;
        fresh += pathFrom(".restore-" + stamp);
        fresh = unique(fresh);
        fs::create_directories(fresh, ec);
        for (const ZipItem &it : files)
        {
            const fs::path target = fresh / pathFrom(it.name);
            if (it.name.back() == '/')
            {
                fs::create_directories(target, ec);
                continue;
            }
            fs::create_directories(target.parent_path(), ec);
            std::ofstream out(target, std::ios::binary | std::ios::trunc);
            out.write(reinterpret_cast<const char *>(it.data.data()), static_cast<std::streamsize>(it.data.size()));
            if (!out)
            {
                error = "nothing was changed: cannot write " + pathText(target);
                return false;
            }
            out.close();
            if (it.time != fs::file_time_type{})
                fs::last_write_time(target, it.time, ec);
        }
        // 3. The old card moved aside (kept), the restored one in its place.
        fs::path replaced;
        if (fs::exists(cardDir, ec))
        {
            replaced = unique(backupDir / "replaced" / pathFrom(strf("mc%d-%s", slot - 1, stamp.c_str())));
            fs::create_directories(replaced.parent_path(), ec);
            fs::rename(cardDir, replaced, ec);
            if (ec)
            {
                error = "nothing was changed: the card folder is in use (" + ec.message() + "); the restored files are in " +
                        pathText(fresh);
                return false;
            }
        }
        fs::rename(fresh, cardDir, ec);
        if (ec)
        {
            std::error_code back;
            if (!replaced.empty())
                fs::rename(replaced, cardDir, back);
            error = "the restored card could not be put in place (" + ec.message() + "); the card is as it was";
            return false;
        }
        const size_t fileCount = static_cast<size_t>(std::count_if(files.begin(), files.end(), [](const ZipItem &z) { return z.name.back() != '/'; }));
        report += "Restored " + std::to_string(fileCount) + " files from " + pathText(zip.filename()) + ".";
        if (!replaced.empty())
            report += " The previous card folder is kept in backups\\replaced\\" + pathText(replaced.filename()) + ".";
        return true;
    }

    // ------------------------------------------------------------------ .ps2 card files
    namespace
    {
        constexpr size_t kPage = 512, kRawPage = 528;
        constexpr uint32_t kEnd = 0xffffffffu, kInUse = 0x80000000u;

        struct CardReader
        {
            const std::vector<uint8_t> &raw;
            uint32_t allocOffset = 0, allocEnd = 0, root = 0;
            uint32_t usable = 0, maxAlloc = 0; // the good clusters MCMAN counts, and how far they reach
            std::vector<uint32_t> fat;

            const uint8_t *page(size_t n) const { return raw.data() + n * kRawPage; }
            void cluster(uint32_t abs, uint8_t *out) const
            {
                std::memcpy(out, page(size_t(abs) * 2), kPage);
                std::memcpy(out + kPage, page(size_t(abs) * 2 + 1), kPage);
            }
            static uint32_t u32(const uint8_t *p) { return get32(p); }

            bool parse(std::string &problem)
            {
                const uint8_t *sb = page(0);
                if (std::memcmp(sb, "Sony PS2 Memory Card Format ", 28) != 0)
                {
                    problem = "not formatted";
                    return false;
                }
                if (get16(sb + 0x28) != 512 || get16(sb + 0x2a) != 2 || u32(sb + 0x30) != 8192)
                {
                    problem = "not an 8 MB PS2 card layout";
                    return false;
                }
                allocOffset = u32(sb + 0x34);
                allocEnd = u32(sb + 0x38);
                root = u32(sb + 0x3c);
                // MCMAN's usable end: (clusters / 1000) * 1000 + 1 good clusters from the
                // allocation start, skipping the bad blocks (8 clusters each) of the superblock.
                int32_t bad[16];
                for (int i = 0; i < 16; ++i)
                    bad[i] = static_cast<int32_t>(u32(sb + 0xd0 + 4 * i));
                const uint32_t target = (8192 / 1000) * 1000 + 1;
                uint32_t at = allocOffset;
                for (; at < allocOffset + allocEnd && usable < target; ++at)
                    if (std::find(std::begin(bad), std::end(bad), static_cast<int32_t>(at / 8)) == std::end(bad))
                        ++usable;
                maxAlloc = at - allocOffset;
                std::vector<uint8_t> c(1024);
                std::vector<uint32_t> fatClusters;
                for (int i = 0; i < 32 && fatClusters.size() * 256 < 8192; ++i)
                {
                    const uint32_t ifc = u32(sb + 0x50 + 4 * i);
                    if (ifc == 0 && !fatClusters.empty())
                        break;
                    if (ifc >= 8192)
                        break;
                    cluster(ifc, c.data());
                    for (int k = 0; k < 256; ++k)
                    {
                        const uint32_t e = u32(c.data() + 4 * k);
                        if (e == kEnd)
                            break;
                        fatClusters.push_back(e);
                    }
                }
                for (uint32_t fc : fatClusters)
                {
                    if (fc >= 8192)
                    {
                        problem = "damaged card (FAT)";
                        return false;
                    }
                    cluster(fc, c.data());
                    for (int k = 0; k < 256; ++k)
                        fat.push_back(u32(c.data() + 4 * k));
                }
                if (fat.size() < allocEnd || allocOffset + allocEnd > 8192)
                {
                    problem = "damaged card (FAT size)";
                    return false;
                }
                return true;
            }

            std::vector<uint32_t> chain(uint32_t first) const
            {
                std::vector<uint32_t> out;
                for (uint32_t c = first; c != kEnd && out.size() < 8192;)
                {
                    if (c >= allocEnd)
                        break;
                    out.push_back(c);
                    const uint32_t v = fat[c];
                    if (v == kEnd || !(v & kInUse))
                        break;
                    c = v & 0x7fffffffu;
                }
                return out;
            }

            std::vector<uint8_t> data(uint32_t first, size_t length) const
            {
                std::vector<uint8_t> out;
                std::vector<uint8_t> c(1024);
                for (uint32_t rel : chain(first))
                {
                    if (out.size() >= length)
                        break;
                    cluster(allocOffset + rel, c.data());
                    out.insert(out.end(), c.begin(), c.end());
                }
                if (out.size() > length)
                    out.resize(length);
                return out;
            }

            struct Entry
            {
                uint16_t mode = 0;
                uint32_t length = 0, cluster = 0;
                std::string name, when;
            };
            static Entry entry(const uint8_t *e)
            {
                Entry x;
                x.mode = static_cast<uint16_t>(get16(e));
                x.length = get32(e + 4);
                x.cluster = get32(e + 0x10);
                const char *n = reinterpret_cast<const char *>(e + 0x40);
                x.name.assign(n, strnlen(n, 32));
                // {0, sec, min, hour, day, month, year lo, year hi}: the PS2's clock (Japan time).
                // Shown like the folder card's dates ("3 Oct 2026 14:27"), marked as the PS2's clock.
                const uint8_t *t = e + 0x18;
                static const char *const months[12] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
                x.when = t[5] >= 1 && t[5] <= 12 ? strf("%u %s %u %02u:%02u (PS2 clock)", t[4], months[t[5] - 1], t[6] | (t[7] << 8), t[3], t[2])
                                                 : strf("%04u-%02u-%02u %02u:%02u (PS2 clock)", t[6] | (t[7] << 8), t[5], t[4], t[3], t[2]);
                return x;
            }
            std::vector<Entry> list(uint32_t first, uint32_t count) const
            {
                const std::vector<uint8_t> bytes = data(first, size_t(count) * 512);
                std::vector<Entry> out;
                for (size_t i = 0; i + 512 <= bytes.size(); i += 512)
                    out.push_back(entry(bytes.data() + i));
                return out;
            }
        };
    }

    CardFileInfo scanCardImage(const std::vector<uint8_t> &image)
    {
        CardFileInfo info;
        info.exists = true;
        info.bytes = image.size();
        if (image.size() != kPs2CardBytes)
        {
            info.problem = "not an 8 MB PS2 card (" + groupedNumber(image.size()) + " bytes)";
            return info;
        }
        CardReader r{image};
        if (!r.parse(info.problem))
            return info;
        info.card = true;
        // Free space as the PS2 shows it (1 KB clusters): the usable ones not in use.
        uint32_t used = 0;
        for (uint32_t c = 0; c < r.maxAlloc && c < r.fat.size(); ++c)
            used += (r.fat[c] & kInUse) ? 1u : 0u;
        info.freeKB = static_cast<int>(r.usable > used ? r.usable - used : 0);
        std::vector<uint8_t> first(1024);
        r.cluster(r.allocOffset + r.root, first.data());
        const auto dot = CardReader::entry(first.data());
        for (const auto &e : r.list(r.root, std::min<uint32_t>(dot.length, 4096)))
        {
            if (!(e.mode & 0x8000) || !(e.mode & 0x20) || e.name == "." || e.name == "..")
                continue;
            SaveEntry s;
            s.folder = e.name;
            s.gt4 = s.folder == kGt4SaveFolder;
            s.gt3 = startsWith(s.folder, "BASCUS-97102");
            s.when = e.when;
            for (const auto &f : r.list(e.cluster, std::min<uint32_t>(e.length, 4096)))
            {
                if (!(f.mode & 0x8000) || !(f.mode & 0x10))
                    continue;
                ++s.files;
                s.bytes += f.length;
                if (f.name == "icon.sys" && f.length >= 0xc0 + 68)
                {
                    const std::vector<uint8_t> icon = r.data(f.cluster, f.length);
                    if (icon.size() >= 0xc0 + 68 && std::memcmp(icon.data(), "PS2D", 4) == 0)
                        s.title = sjisTitle(icon.data() + 0xc0, 68, static_cast<size_t>(icon[6] | (icon[7] << 8)));
                }
            }
            if (s.gt4)
            {
                info.gt4Save = true;
                info.gt4 = s;
            }
            info.saves.push_back(s);
        }
        std::stable_sort(info.saves.begin(), info.saves.end(), [](const SaveEntry &a, const SaveEntry &b) { return a.gt4 && !b.gt4; });
        return info;
    }

    CardFileInfo scanCardFile(const fs::path &file)
    {
        CardFileInfo info;
        info.file = file;
        std::error_code ec;
        info.exists = fs::is_regular_file(file, ec);
        if (!info.exists)
            return info;
        info.bytes = fs::file_size(file, ec);
        if (info.bytes != kPs2CardBytes)
        {
            info.problem = "not an 8 MB PS2 card (" + groupedNumber(info.bytes) + " bytes)";
            return info;
        }
        std::vector<uint8_t> image;
        if (!readFile(file, image))
        {
            info.problem = "cannot be read";
            return info;
        }
        CardFileInfo scanned = scanCardImage(image);
        scanned.file = file;
        return scanned;
    }

    bool backupCardFile(const fs::path &file, int slot, const fs::path &backupDir, const std::string &suffix, fs::path &out, std::string &error)
    {
        std::vector<uint8_t> data;
        if (!readFile(file, data))
        {
            error = "cannot read " + pathText(file);
            return false;
        }
        std::error_code ec;
        fs::create_directories(backupDir, ec);
        const std::string stamp = nowFileStamp();
        out = backupDir / pathFrom(strf("GT4-mc%d-%s%s.ps2", slot - 1, stamp.c_str(), suffix.c_str()));
        for (int n = 2; fs::exists(out, ec) && n < 100; ++n)
            out = backupDir / pathFrom(strf("GT4-mc%d-%s-%d%s.ps2", slot - 1, stamp.c_str(), n, suffix.c_str()));
        if (!writeFileAtomic(out, std::string_view(reinterpret_cast<const char *>(data.data()), data.size()), error))
            return false;
        std::vector<uint8_t> check;
        if (!readFile(out, check) || check != data)
        {
            error = "the copy does not read back the same";
            return false;
        }
        return true;
    }

    bool restoreCardFile(const fs::path &backup, const fs::path &file, int slot, const fs::path &backupDir, std::string &report, std::string &error)
    {
        std::vector<uint8_t> data;
        if (!readFile(backup, data))
        {
            error = "cannot read " + pathText(backup);
            return false;
        }
        if (data.size() != kPs2CardBytes)
        {
            error = "the backup is not an 8 MB PS2 card";
            return false;
        }
        report.clear();
        std::error_code ec;
        if (fs::exists(file, ec))
        {
            fs::path before;
            if (!backupCardFile(file, slot, backupDir, "-before-restore", before, error))
            {
                error = "nothing was changed: the copy of the card as it is failed (" + error + ")";
                return false;
            }
            report += "Backed up the card as it was to " + pathText(before.filename()) + ". ";
        }
        if (!writeFileAtomic(file, std::string_view(reinterpret_cast<const char *>(data.data()), data.size()), error))
            return false;
        report += "Restored " + pathText(file.filename()) + " from " + pathText(backup.filename()) + ".";
        return true;
    }

    PhotoInfo scanPhotos(const fs::path &usbRoot)
    {
        PhotoInfo p;
        p.dir = usbRoot / "GT4";
        std::error_code ec;
        p.exists = fs::is_directory(p.dir, ec);
        if (!p.exists)
            return p;
        for (const auto &entry : fs::directory_iterator(p.dir, fs::directory_options::skip_permission_denied, ec))
        {
            std::error_code e2;
            if (!entry.is_regular_file(e2) || lower(pathText(entry.path().extension())) != ".jpg")
                continue;
            ++p.count;
            p.bytes += entry.file_size(e2);
            const auto t = entry.last_write_time(e2);
            if (p.newest.empty() || t > p.newestTime)
            {
                p.newest = pathText(entry.path().filename());
                p.newestTime = t;
            }
        }
        return p;
    }
}
