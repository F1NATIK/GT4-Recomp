// The AI cars' drivers from the Race Lab: see gt4_ai_drivers.h.
#include "gt4_ai_drivers.h"
#include "gt4_race_reader.h"

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <sstream>
#include <string>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace gt4drivers
{
    namespace
    {
        namespace fs = std::filesystem;
        std::mutex g_lock;
        bool g_init = false, g_off = false;
        fs::path g_folder;
        std::string g_names[kMaxAi + 1];
        bool g_any = false;
        fs::file_time_type g_stamp{};
        uintmax_t g_size = ~uintmax_t(0);
        std::chrono::steady_clock::time_point g_nextLook{};

        fs::path exeDir()
        {
#if defined(_WIN32)
            wchar_t b[MAX_PATH] = {};
            const DWORD n = GetModuleFileNameW(nullptr, b, MAX_PATH);
            return fs::path(std::wstring(b, n)).parent_path();
#else
            return fs::current_path();
#endif
        }

        void init()
        {
            if (g_init)
                return;
            g_init = true;
            const char *v = std::getenv("GT4_AI_DRIVERS");
            const std::string s = v ? v : "";
            if (s == "off" || s == "0")
            {
                g_off = true;
                std::fprintf(stderr, "[gt4-drivers] off (GT4_AI_DRIVERS=off): the AI cars keep their car names\n");
                return;
            }
            const fs::path given = fs::u8path(s.empty() ? std::string("racelab-data\\ai-lines") : s);
            std::error_code ec;
            g_folder = given;
            if (given.is_relative() && !fs::exists(given, ec) && fs::exists(exeDir() / given, ec))
                g_folder = exeDir() / given;
            const auto u = fs::absolute(g_folder, ec).u8string();
            std::fprintf(stderr, "[gt4-drivers] the AI cars' drivers from %s\\gt4-drivers.txt (the Race Lab's Send to AI cars)\n",
                         std::string(u.begin(), u.end()).c_str());
        }

        // (under g_lock) the file read again when it changed; looked at every 2 s at most
        void refresh()
        {
            init();
            if (g_off)
                return;
            const auto now = std::chrono::steady_clock::now();
            if (now < g_nextLook)
                return;
            g_nextLook = now + std::chrono::seconds(2);
            const fs::path file = g_folder / L"gt4-drivers.txt";
            std::error_code ec;
            const uintmax_t size = fs::file_size(file, ec);
            if (ec)
            {
                if (g_size != ~uintmax_t(0) || g_any)
                    std::fprintf(stderr, "[gt4-drivers] no gt4-drivers.txt now: the AI cars keep their car names\n");
                for (std::string &n : g_names)
                    n.clear();
                g_any = false;
                g_size = ~uintmax_t(0);
                return;
            }
            const fs::file_time_type stamp = fs::last_write_time(file, ec);
            if (size == g_size && stamp == g_stamp)
                return;
            g_size = size;
            g_stamp = stamp;
            std::ifstream in(file, std::ios::binary);
            std::string line;
            bool header = false;
            std::string names[kMaxAi + 1];
            int count = 0;
            while (std::getline(in, line))
            {
                if (!line.empty() && line.back() == '\r')
                    line.pop_back();
                if (!header)
                {
                    if (line.size() >= 3 && static_cast<unsigned char>(line[0]) == 0xef && static_cast<unsigned char>(line[1]) == 0xbb)
                        line.erase(0, 3); // (a UTF-8 mark)
                    header = line.rfind("gt4-ai-drivers ", 0) == 0;
                    if (!header)
                        break;
                    continue;
                }
                if (line.rfind("car ", 0) != 0)
                    continue;
                std::istringstream ls(line.substr(4));
                int k = 0;
                ls >> k;
                std::string name;
                std::getline(ls, name);
                const size_t a = name.find_first_not_of(" \t"), b = name.find_last_not_of(" \t");
                name = a == std::string::npos ? std::string() : name.substr(a, b - a + 1);
                for (char &c : name)
                    if (static_cast<unsigned char>(c) < 0x20)
                        c = ' ';
                if (k >= 1 && k <= kMaxAi && !name.empty())
                {
                    names[k] = name.substr(0, 60);
                    ++count;
                }
            }
            if (!header)
            {
                std::fprintf(stderr, "[gt4-drivers] gt4-drivers.txt has no \"gt4-ai-drivers 1\" first line: not read\n");
                count = 0;
            }
            for (int k = 0; k <= kMaxAi; ++k)
                g_names[k] = header ? names[k] : std::string();
            g_any = count > 0;
            std::string list;
            for (int k = 1; k <= kMaxAi; ++k)
                if (!g_names[k].empty())
                    list += (list.empty() ? "" : ", ") + std::to_string(k) + " " + g_names[k];
            std::fprintf(stderr, "[gt4-drivers] %d AI car driver%s: %s\n", count, count == 1 ? "" : "s", list.empty() ? "(none)" : list.c_str());
        }
    } // namespace

    int aiNumber(const gt4race::State &s, int index)
    {
        if (index < 0 || index >= s.carCount || s.cars[index].player)
            return 0;
        int k = 0;
        for (int i = 0; i <= index; ++i)
            if (!s.cars[i].player)
                ++k;
        return k;
    }

    std::string driverOf(const gt4race::State &s, int index)
    {
        const int k = aiNumber(s, index);
        std::lock_guard<std::mutex> lk(g_lock);
        refresh();
        return k >= 1 && k <= kMaxAi ? g_names[k] : std::string();
    }

    std::string asciiName(const std::string &u)
    {
        // U+00C0..U+00FF without their accents (x: no plain letter); U+0100..U+017F (Latin Extended-A) by pairs.
        static const char kLatin1[] = "AAAAAAACEEEEIIIIDNOOOOOxOUUUUYTsaaaaaaaceeeeiiiidnooooo/ouuuuyty";
        static const char kExtA[] = "AaAaAaCcCcCcCcDdDdEeEeEeEeEeGgGgGgGgHhHhIiIiIiIiIiIiJjKkkLlLlLlLlLlNnNnNnnNnOoOoOoOoRrRrRrSsSsSsSsTtTtTtUuUuUuUuUuUuWwYyYZzZzZzs";
        std::string out;
        for (size_t i = 0; i < u.size();)
        {
            const unsigned char c = static_cast<unsigned char>(u[i]);
            if (c < 0x80)
            {
                out += static_cast<char>(c);
                ++i;
                continue;
            }
            int len = c >= 0xf0 ? 4 : c >= 0xe0 ? 3 : c >= 0xc0 ? 2 : 1;
            uint32_t cp = len == 2 ? (c & 0x1fu) : len == 3 ? (c & 0x0fu) : (c & 0x07u);
            for (int k = 1; k < len; ++k)
                cp = i + k < u.size() ? (cp << 6) | (static_cast<unsigned char>(u[i + k]) & 0x3fu) : 0u;
            i += static_cast<size_t>(len);
            char a = '?';
            if (cp == 0xdf)
                out += "ss", a = 0;
            else if (cp >= 0xc0 && cp <= 0xff && kLatin1[cp - 0xc0] != 'x' && kLatin1[cp - 0xc0] != '/')
                a = kLatin1[cp - 0xc0];
            else if (cp >= 0x100 && cp < 0x100 + sizeof(kExtA) - 1)
                a = kExtA[cp - 0x100];
            else if (cp == 0x2019 || cp == 0x2018)
                a = '\'';
            else if (cp == 0x2013 || cp == 0x2014)
                a = '-';
            if (a)
                out += a;
        }
        return out;
    }

    bool any()
    {
        std::lock_guard<std::mutex> lk(g_lock);
        refresh();
        return g_any;
    }

    std::string folder()
    {
        std::lock_guard<std::mutex> lk(g_lock);
        init();
        const auto u = g_folder.u8string();
        return std::string(u.begin(), u.end());
    }
} // namespace gt4drivers
