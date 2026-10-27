#include "gt4stock_util.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
#include <sstream>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif

namespace gt4stock
{
    std::string trim(std::string_view s)
    {
        size_t a = 0, b = s.size();
        while (a < b && std::isspace(static_cast<unsigned char>(s[a])))
            ++a;
        while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1])))
            --b;
        return std::string(s.substr(a, b - a));
    }

    std::string lower(std::string_view s)
    {
        std::string r(s);
        for (char &c : r)
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return r;
    }

    std::string upper(std::string_view s)
    {
        std::string r(s);
        for (char &c : r)
            c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        return r;
    }

    bool startsWith(std::string_view s, std::string_view prefix) { return s.substr(0, prefix.size()) == prefix; }
    bool endsWith(std::string_view s, std::string_view suffix)
    {
        return s.size() >= suffix.size() && s.substr(s.size() - suffix.size()) == suffix;
    }

    bool iequals(std::string_view a, std::string_view b)
    {
        if (a.size() != b.size())
            return false;
        for (size_t i = 0; i < a.size(); ++i)
            if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i])))
                return false;
        return true;
    }

    std::vector<std::string> split(std::string_view s, char sep)
    {
        std::vector<std::string> out;
        size_t start = 0;
        for (;;)
        {
            const size_t at = s.find(sep, start);
            out.emplace_back(s.substr(start, at == std::string_view::npos ? std::string_view::npos : at - start));
            if (at == std::string_view::npos)
                break;
            start = at + 1;
        }
        return out;
    }

    std::string join(const std::vector<std::string> &parts, std::string_view sep)
    {
        std::string r;
        for (size_t i = 0; i < parts.size(); ++i)
        {
            if (i)
                r += sep;
            r += parts[i];
        }
        return r;
    }

    std::string strf(const char *fmt, ...)
    {
        va_list args;
        va_start(args, fmt);
        char small[512];
        va_list copy;
        va_copy(copy, args);
        const int n = std::vsnprintf(small, sizeof(small), fmt, copy);
        va_end(copy);
        std::string out;
        if (n < 0)
            out.clear();
        else if (static_cast<size_t>(n) < sizeof(small))
            out.assign(small, static_cast<size_t>(n));
        else
        {
            out.resize(static_cast<size_t>(n) + 1);
            std::vsnprintf(out.data(), out.size(), fmt, args);
            out.resize(static_cast<size_t>(n));
        }
        va_end(args);
        return out;
    }

    bool parseInt(std::string_view s, long long &out)
    {
        const std::string t = trim(s);
        if (t.empty())
            return false;
        char *end = nullptr;
        const long long v = std::strtoll(t.c_str(), &end, 10);
        if (!end || *end != '\0')
            return false;
        out = v;
        return true;
    }

    bool parseFloat(std::string_view s, double &out)
    {
        const std::string t = trim(s);
        if (t.empty())
            return false;
        char *end = nullptr;
        const double v = std::strtod(t.c_str(), &end);
        if (!end || *end != '\0' || !std::isfinite(v))
            return false;
        out = v;
        return true;
    }

    std::string numberText(double v, int decimals)
    {
        std::string s = strf("%.*f", decimals, v);
        if (s.find('.') != std::string::npos)
        {
            while (!s.empty() && s.back() == '0')
                s.pop_back();
            if (!s.empty() && s.back() == '.')
                s.pop_back();
        }
        if (s == "-0")
            s = "0";
        return s;
    }

    std::string utf8(const std::wstring &w)
    {
#if defined(_WIN32)
        if (w.empty())
            return {};
        const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
        std::string out(static_cast<size_t>(std::max(0, n)), '\0');
        if (n > 0)
            WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), out.data(), n, nullptr, nullptr);
        return out;
#else
        return std::string(w.begin(), w.end());
#endif
    }

    std::wstring wide(std::string_view s)
    {
#if defined(_WIN32)
        if (s.empty())
            return {};
        const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
        std::wstring out(static_cast<size_t>(std::max(0, n)), L'\0');
        if (n > 0)
            MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), n);
        return out;
#else
        return std::wstring(s.begin(), s.end());
#endif
    }

    std::string pathText(const fs::path &p) { return utf8(p.wstring()); }
    fs::path pathFrom(std::string_view s) { return fs::path(wide(s)); }

    std::string shortPath(const fs::path &p, const fs::path &base)
    {
        if (p.empty())
            return {};
        std::error_code ec;
        const fs::path rel = fs::relative(p, base, ec);
        if (!ec && !rel.empty())
        {
            const std::string r = pathText(rel);
            if (!startsWith(r, "..") && r.find(':') == std::string::npos)
                return r;
        }
        return pathText(p);
    }

    std::string sizeText(uint64_t bytes)
    {
        const double b = static_cast<double>(bytes);
        if (bytes < 1024)
            return strf("%llu bytes", static_cast<unsigned long long>(bytes));
        if (b < 1024.0 * 1024.0)
            return numberText(b / 1024.0, b < 10 * 1024.0 ? 1 : 0) + " KB";
        if (b < 1024.0 * 1024.0 * 1024.0)
            return numberText(b / (1024.0 * 1024.0), b < 100 * 1024.0 * 1024.0 ? 1 : 0) + " MB";
        return numberText(b / (1024.0 * 1024.0 * 1024.0), 2) + " GB";
    }

    std::string groupedNumber(uint64_t value)
    {
        std::string digits = std::to_string(value);
        std::string out;
        const int n = static_cast<int>(digits.size());
        for (int i = 0; i < n; ++i)
        {
            if (i && (n - i) % 3 == 0)
                out += ',';
            out += digits[static_cast<size_t>(i)];
        }
        return out;
    }

    namespace
    {
        std::tm localTime(std::time_t t)
        {
            std::tm tm{};
#if defined(_WIN32)
            localtime_s(&tm, &t);
#else
            localtime_r(&t, &tm);
#endif
            return tm;
        }
        const char *const kMonths[12] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
    }

    std::string timeText(fs::file_time_type t)
    {
#if defined(_MSC_VER)
        const auto sys = std::chrono::clock_cast<std::chrono::system_clock>(t);
#else // libc++ has no clock_cast: shift by the two clocks' difference now (seconds are enough here)
        const auto sys = std::chrono::time_point_cast<std::chrono::system_clock::duration>(
            std::chrono::system_clock::now() + (t - fs::file_time_type::clock::now()));
#endif
        const std::tm tm = localTime(std::chrono::system_clock::to_time_t(sys));
        return strf("%d %s %d %02d:%02d", tm.tm_mday, kMonths[std::clamp(tm.tm_mon, 0, 11)], tm.tm_year + 1900, tm.tm_hour, tm.tm_min);
    }

    std::string nowText()
    {
        const std::tm tm = localTime(std::time(nullptr));
        return strf("%04d-%02d-%02d %02d:%02d", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min);
    }

    std::string nowFileStamp()
    {
        const std::tm tm = localTime(std::time(nullptr));
        return strf("%04d-%02d-%02d_%02d%02d-%02d", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec);
    }

    uint32_t crc32(const void *data, size_t size, uint32_t crc)
    {
        static uint32_t table[256];
        static bool ready = false;
        if (!ready)
        {
            for (uint32_t i = 0; i < 256; ++i)
            {
                uint32_t c = i;
                for (int k = 0; k < 8; ++k)
                    c = (c & 1u) ? 0xedb88320u ^ (c >> 1) : c >> 1;
                table[i] = c;
            }
            ready = true;
        }
        const auto *p = static_cast<const uint8_t *>(data);
        crc = ~crc;
        for (size_t i = 0; i < size; ++i)
            crc = table[(crc ^ p[i]) & 0xffu] ^ (crc >> 8);
        return ~crc;
    }

    bool readFile(const fs::path &path, std::vector<uint8_t> &data)
    {
        std::ifstream in(path, std::ios::binary);
        if (!in)
            return false;
        in.seekg(0, std::ios::end);
        const std::streamoff n = in.tellg();
        if (n < 0)
            return false;
        in.seekg(0, std::ios::beg);
        data.resize(static_cast<size_t>(n));
        if (n > 0)
            in.read(reinterpret_cast<char *>(data.data()), n);
        return static_cast<bool>(in) || in.eof();
    }

    bool readText(const fs::path &path, std::string &text)
    {
        std::vector<uint8_t> data;
        if (!readFile(path, data))
            return false;
        text.assign(data.begin(), data.end());
        // A UTF-8 byte order mark (Notepad) is not part of the first key.
        if (startsWith(text, "\xEF\xBB\xBF"))
            text.erase(0, 3);
        return true;
    }

    bool writeFileAtomic(const fs::path &path, std::string_view data, std::string &error)
    {
        std::error_code ec;
        if (path.has_parent_path())
            fs::create_directories(path.parent_path(), ec);
        fs::path tmp = path;
        tmp += L".tmp";
        {
            std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
            if (!out)
            {
                error = "cannot write " + pathText(tmp);
                return false;
            }
            out.write(data.data(), static_cast<std::streamsize>(data.size()));
            out.flush();
            if (!out)
            {
                error = "write failed: " + pathText(tmp);
                return false;
            }
        }
#if defined(_WIN32)
        if (!MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
        {
            error = strf("cannot replace %s (error %lu)", pathText(path).c_str(), GetLastError());
            fs::remove(tmp, ec);
            return false;
        }
#else
        fs::rename(tmp, path, ec);
        if (ec)
        {
            error = "cannot replace " + pathText(path) + ": " + ec.message();
            return false;
        }
#endif
        return true;
    }

    uint64_t folderBytes(const fs::path &dir, size_t *files)
    {
        uint64_t total = 0;
        size_t count = 0;
        std::error_code ec;
        if (!fs::is_directory(dir, ec))
        {
            if (files)
                *files = 0;
            return 0;
        }
        for (fs::recursive_directory_iterator it(dir, fs::directory_options::skip_permission_denied, ec), end; !ec && it != end;
             it.increment(ec))
        {
            std::error_code e2;
            if (it->is_regular_file(e2))
            {
                total += it->file_size(e2);
                ++count;
            }
        }
        if (files)
            *files = count;
        return total;
    }

    bool peExportNames(const fs::path &exe, std::vector<std::string> &names, std::string &error)
    {
        names.clear();
        std::ifstream in(exe, std::ios::binary);
        if (!in)
        {
            error = "cannot open the file";
            return false;
        }
        auto readAt = [&](uint64_t off, void *dst, size_t n) {
            in.clear();
            in.seekg(static_cast<std::streamoff>(off));
            in.read(static_cast<char *>(dst), static_cast<std::streamsize>(n));
            return static_cast<size_t>(in.gcount()) == n;
        };
        auto u16 = [](const uint8_t *p) { return static_cast<uint32_t>(p[0] | (p[1] << 8)); };
        auto u32 = [](const uint8_t *p) { return static_cast<uint32_t>(p[0] | (p[1] << 8) | (p[2] << 16)) | (static_cast<uint32_t>(p[3]) << 24); };
        uint8_t dos[64];
        if (!readAt(0, dos, sizeof(dos)) || dos[0] != 'M' || dos[1] != 'Z')
        {
            error = "not a Windows program (no MZ header)";
            return false;
        }
        const uint32_t pe = u32(dos + 0x3c);
        uint8_t hdr[24];
        if (pe > 0x10000000u || !readAt(pe, hdr, sizeof(hdr)) || hdr[0] != 'P' || hdr[1] != 'E' || hdr[2] != 0 || hdr[3] != 0)
        {
            error = "not a Windows program (no PE header)";
            return false;
        }
        const uint32_t sections = u16(hdr + 6), optSize = u16(hdr + 20);
        std::vector<uint8_t> opt(optSize);
        if (optSize < 2 || !readAt(pe + 24, opt.data(), opt.size()))
        {
            error = "no optional header";
            return false;
        }
        const uint32_t magic = u16(opt.data());
        const size_t dirAt = magic == 0x20b ? 112 : magic == 0x10b ? 96 : 0; // PE32+ / PE32: the data directories
        if (!dirAt || opt.size() < dirAt + 8)
        {
            error = "no data directories";
            return false;
        }
        const uint32_t exportRva = u32(opt.data() + dirAt), exportSize = u32(opt.data() + dirAt + 4);
        if (!exportRva || !exportSize)
            return true; // a program that exports nothing
        std::vector<uint8_t> table(static_cast<size_t>(std::min<uint32_t>(sections, 256)) * 40);
        if (!readAt(pe + 24 + optSize, table.data(), table.size()))
        {
            error = "no section table";
            return false;
        }
        auto fileOffset = [&](uint32_t rva, uint64_t &off) {
            for (size_t i = 0; i + 40 <= table.size(); i += 40)
            {
                const uint8_t *sec = table.data() + i;
                const uint32_t vsize = u32(sec + 8), va = u32(sec + 12), rawSize = u32(sec + 16), raw = u32(sec + 20);
                if (rva >= va && rva < va + std::max(vsize, rawSize))
                {
                    off = static_cast<uint64_t>(raw) + (rva - va);
                    return true;
                }
            }
            return false;
        };
        uint64_t off = 0;
        uint8_t dir[40];
        if (!fileOffset(exportRva, off) || !readAt(off, dir, sizeof(dir)))
        {
            error = "the export table is outside the file";
            return false;
        }
        const uint32_t count = std::min<uint32_t>(u32(dir + 24), 100000u), namesRva = u32(dir + 32);
        if (!count)
            return true;
        std::vector<uint8_t> rvas(static_cast<size_t>(count) * 4);
        if (!fileOffset(namesRva, off) || !readAt(off, rvas.data(), rvas.size()))
        {
            error = "the export names are outside the file";
            return false;
        }
        for (uint32_t i = 0; i < count; ++i)
        {
            uint64_t at = 0;
            char name[257] = {};
            if (!fileOffset(u32(rvas.data() + i * 4), at))
                continue;
            in.clear();
            in.seekg(static_cast<std::streamoff>(at));
            in.read(name, 256);
            name[std::min<std::streamsize>(in.gcount(), 256)] = '\0';
            names.emplace_back(name);
        }
        return true;
    }

    std::string ExeFeatures::text() const
    {
        if (!read)
            return "not read (" + error + ")";
        auto yn = [](bool b) { return b ? "yes" : "no"; };
        return strf("cheat switches %s, in-game card %s, Mission Hall fix %s, player 2 %s, Classic build %s", yn(cheats), yn(card), yn(missionfix),
                    yn(pad2), yn(classic));
    }

    ExeFeatures exeFeatures(const fs::path &exe)
    {
        ExeFeatures f;
        std::ifstream in(exe, std::ios::binary);
        if (!in)
        {
            f.error = "cannot open the file";
            return f;
        }
        auto readAt = [&](uint64_t off, void *dst, size_t n) {
            in.clear();
            in.seekg(static_cast<std::streamoff>(off));
            in.read(static_cast<char *>(dst), static_cast<std::streamsize>(n));
            return static_cast<size_t>(in.gcount()) == n;
        };
        auto u16 = [](const uint8_t *p) { return static_cast<uint32_t>(p[0] | (p[1] << 8)); };
        auto u32 = [](const uint8_t *p) { return static_cast<uint32_t>(p[0] | (p[1] << 8) | (p[2] << 16)) | (static_cast<uint32_t>(p[3]) << 24); };
        uint8_t dos[64];
        if (!readAt(0, dos, sizeof(dos)) || dos[0] != 'M' || dos[1] != 'Z')
        {
            f.error = "not a Windows program (no MZ header)";
            return f;
        }
        const uint32_t pe = u32(dos + 0x3c);
        uint8_t hdr[24];
        if (pe > 0x10000000u || !readAt(pe, hdr, sizeof(hdr)) || hdr[0] != 'P' || hdr[1] != 'E' || hdr[2] != 0 || hdr[3] != 0)
        {
            f.error = "not a Windows program (no PE header)";
            return f;
        }
        const uint32_t sections = std::min<uint32_t>(u16(hdr + 6), 96), optSize = u16(hdr + 20);
        std::vector<uint8_t> table(static_cast<size_t>(sections) * 40);
        if (!readAt(pe + 24 + optSize, table.data(), table.size()))
        {
            f.error = "no section table";
            return f;
        }
        bool any = false;
        for (size_t i = 0; i + 40 <= table.size(); i += 40)
        {
            const uint8_t *sec = table.data() + i;
            const std::string name(reinterpret_cast<const char *>(sec), strnlen(reinterpret_cast<const char *>(sec), 8));
            const uint32_t rawSize = u32(sec + 16), raw = u32(sec + 20);
            if ((name != ".rdata" && name != ".data") || rawSize == 0 || rawSize > (256u << 20))
                continue;
            std::string data(rawSize, '\0');
            if (!readAt(raw, data.data(), data.size()))
                continue;
            any = true;
            auto has = [&](std::string_view word) {
                std::string pattern(1, '\0');
                pattern.append(word);
                pattern.push_back('\0');
                return data.find(pattern) != std::string::npos;
            };
            f.cheats = f.cheats || has("GT4_CHEATS");
            f.card = f.card || has("GT4_CHEAT_CARD");
            f.missionfix = f.missionfix || has("missionfix");
            f.pad2 = f.pad2 || has("GT4_PAD2");
            // main.cpp's start line, "GT4_CLASSIC_BUILD=1 (GT4 Recompilation, Classic edition)", put together here at run time: this
            // file is linked into every GT4.exe, so the bare "GT4_CLASSIC_BUILD=1" was in each one and every GT4 read as Classic.
            static const std::string kClassicLine = std::string("GT4_CLASSIC_BUILD=1") + " (GT4 Recompilation";
            f.classic = f.classic || data.find(kClassicLine) != std::string::npos;
        }
        if (!any)
        {
            f.error = "no data section";
            return f;
        }
        f.read = true;
        return f;
    }

    bool hasShortNameForm(const std::string &name)
    {
        const size_t dot = name.find('.');
        if (name.empty() || dot == 0 || (dot != std::string::npos && name.find('.', dot + 1) != std::string::npos))
            return false;
        const size_t baseLen = dot == std::string::npos ? name.size() : dot;
        const size_t extLen = dot == std::string::npos ? 0 : name.size() - dot - 1;
        if (baseLen == 0 || baseLen > 8 || extLen > 3 || (dot != std::string::npos && extLen == 0))
            return false;
        for (const char ch : name)
        {
            const unsigned char c = static_cast<unsigned char>(ch);
            if (c <= 0x20 || c >= 0x7f) // a space or a name that is not plain ASCII: skipped by GT4's stick
                return false;
        }
        return true;
    }
}
