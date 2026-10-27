#pragma once

// GT4 Recompilation launcher: small helpers (strings, paths, sizes, times, CRC-32,
// files). No raylib, no Win32 in this header.

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace gt4stock
{
    namespace fs = std::filesystem;

    std::string trim(std::string_view s);
    std::string lower(std::string_view s);
    std::string upper(std::string_view s);
    bool startsWith(std::string_view s, std::string_view prefix);
    bool endsWith(std::string_view s, std::string_view suffix);
    bool iequals(std::string_view a, std::string_view b);
    std::vector<std::string> split(std::string_view s, char sep);
    std::string join(const std::vector<std::string> &parts, std::string_view sep);
    std::string strf(const char *fmt, ...);
    bool parseInt(std::string_view s, long long &out);
    bool parseFloat(std::string_view s, double &out);
    // A number with up to `decimals` decimals, trailing zeros removed ("0.25", "4").
    std::string numberText(double v, int decimals = 3);

    std::string utf8(const std::wstring &w);
    std::wstring wide(std::string_view s);
    std::string pathText(const fs::path &p); // UTF-8
    fs::path pathFrom(std::string_view utf8);
    // The path relative to base when it is inside it ("game\mc0"), else the full path.
    std::string shortPath(const fs::path &p, const fs::path &base);

    std::string sizeText(uint64_t bytes);       // "1.5 MB", "5.31 GB", "964 bytes"
    std::string groupedNumber(uint64_t value);  // "5,314,478,080"
    std::string timeText(fs::file_time_type t); // "3 Oct 2026 15:27"
    std::string nowText();                      // "2026-10-04 19:02"
    std::string nowFileStamp();                 // "2026-10-04_1902-05" (seconds, for file names)

    uint32_t crc32(const void *data, size_t size, uint32_t crc = 0);

    bool readFile(const fs::path &path, std::vector<uint8_t> &data);
    bool readText(const fs::path &path, std::string &text);
    // Writes through "<path>.tmp" and a replace, so a crash never leaves half a file.
    bool writeFileAtomic(const fs::path &path, std::string_view data, std::string &error);

    // Size of every file below a folder (0 when missing), and the number of files.
    uint64_t folderBytes(const fs::path &dir, size_t *files = nullptr);

    // The names a Windows program exports (its PE export table), read from the file: the headers
    // and the export directory only. False (with a reason) when it is not a readable program.
    bool peExportNames(const fs::path &exe, std::vector<std::string> &names, std::string &error);

    // (1.4 review) What a GT4 program reads, told from its read-only data (.rdata / .data, read only; nothing is
    // started): the cheats runtime's keys "GT4_CHEATS" (noday, secret) and "GT4_CHEAT_CARD" (the in-game card) and its
    // word "missionfix" (the Mission Hall fix), and the player-2 runtime's "GT4_PAD2". Each must be a whole string there
    // (a 0 byte before and after it), as the compiler stores a string literal.
    struct ExeFeatures
    {
        bool read = false;       // the program's data sections were read
        bool cheats = false;     // GT4_CHEATS: Skip the 365-day wait, Let controller 1 type the hidden menu code
        bool card = false;       // GT4_CHEAT_CARD: the in-game cheat card (F2)
        bool missionfix = false; // GT4_CHEATS=missionfix: Fix the Mission Hall code
        bool pad2 = false;       // GT4_PAD2: player 2 (a second controller in port 2)
        // (1.4.2) The Classic build's marker "GT4_CLASSIC_BUILD=1" (main.cpp prints it at start; part of a longer string,
        // so it is found anywhere in the data). Without it GT4.exe is a development build, which draws on the CPU unless
        // GT3_RENDERER says otherwise (launchRenderer, gt4stock_settings.h).
        bool classic = false;
        std::string error;
        std::string text() const; // "cheat switches yes, in-game card yes, Mission Hall fix yes, player 2 no, Classic build no"
    };
    ExeFeatures exeFeatures(const fs::path &exe);

    // (1.4 review) A file name GT4's USB stick takes (gt4_usbdisk.cpp makeShortName: an 8.3 name, one dot at most, a name
    // of 1-8 and an extension of 0-3 plain ASCII characters, no space). Conservative: a few odd names GT4 skips count
    // too. The launcher never saves a file GT4 would list as a photo.
    bool hasShortNameForm(const std::string &name);
}
