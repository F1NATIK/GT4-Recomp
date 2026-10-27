#pragma once

// GT4 Recompilation launcher: the memory cards and the USB stick.
//
// GT4.exe keeps each memory card as a folder (the runtime's folder-backed
// card): slot 1 = <CORE.elf folder>\mc0, slot 2 = ...\mc1 (MemoryCard.cpp
// getMcRootPath). GT4's own save is the folder BASCUS-97328GAMEDATA.
// Backups are zip files (deflate or stored, CRC-32 checked) in
// <game folder>\backups, named GT4-mc<N>-<date>_<time>.zip. A restore first
// backs up the card as it is, then moves it to backups\replaced\ (nothing is
// deleted) and puts the backup's files in its place.

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace gt4stock
{
    namespace fs = std::filesystem;

    constexpr const char *kGt4SaveFolder = "BASCUS-97328GAMEDATA";

    struct SaveEntry
    {
        std::string when;    // the last change, as text (a card file's own clock)
        std::string folder;  // BASCUS-97328GAMEDATA
        std::string title;   // from icon.sys ("GRAN TURISMO 4 Game data")
        bool gt4 = false;    // GT4's own game data
        bool gt3 = false;    // GT3's (BASCUS-97102...) - GT4 can read it
        uint64_t bytes = 0;
        size_t files = 0;
        fs::file_time_type changed{};
    };

    struct CardInfo
    {
        int slot = 1;         // 1 or 2
        fs::path dir;
        bool exists = false;
        uint64_t bytes = 0;
        size_t files = 0;
        bool gt4Save = false;
        SaveEntry gt4;        // valid when gt4Save
        std::vector<SaveEntry> saves; // every save folder, GT4's first
    };
    CardInfo scanCard(const fs::path &dir, int slot);

    // A PCSX2-style .ps2 card file (8 MB of 512-byte pages, each followed by 16 spare
    // bytes): read only, to list its saves (analysis/saves/ps2-card has the full tools).
    constexpr uint64_t kPs2CardBytes = 8650752;
    struct CardFileInfo
    {
        fs::path file;
        bool exists = false;
        uint64_t bytes = 0;
        bool card = false;       // an 8 MB PS2 card, formatted
        std::string problem;     // why not ("not formatted", "not an 8 MB card", ...)
        int freeKB = 0;
        bool gt4Save = false;
        SaveEntry gt4;
        std::vector<SaveEntry> saves;
    };
    CardFileInfo scanCardFile(const fs::path &file);
    CardFileInfo scanCardImage(const std::vector<uint8_t> &image); // the same, from memory (tests)

    struct BackupInfo
    {
        fs::path file;
        int slot = 0;          // from the name (GT4-mc0-... -> 1), 0 unknown
        uint64_t bytes = 0;
        fs::file_time_type time{};
        bool beforeRestore = false; // made automatically before a restore
        bool cardFile = false;      // a .ps2 card file's copy (else a folder card's zip)
    };
    std::vector<BackupInfo> listBackups(const fs::path &backupDir); // newest first

    // The card folder into backupDir\GT4-mc<slot-1>-<stamp><suffix>.zip.
    bool backupCard(const fs::path &cardDir, int slot, const fs::path &backupDir, const std::string &version, const std::string &suffix,
                    fs::path &zipOut, std::string &error);
    // Replaces the card folder with the zip's files (see the header comment).
    bool restoreCard(const fs::path &zip, const fs::path &cardDir, int slot, const fs::path &backupDir, const std::string &version,
                     std::string &report, std::string &error);
    // A .ps2 card file: a dated copy in backupDir (GT4-mc<slot-1>-<stamp><suffix>.ps2),
    // checked byte for byte; and back (the file as it is is copied away first).
    bool backupCardFile(const fs::path &file, int slot, const fs::path &backupDir, const std::string &suffix, fs::path &out,
                        std::string &error);
    bool restoreCardFile(const fs::path &backup, const fs::path &file, int slot, const fs::path &backupDir, std::string &report,
                         std::string &error);

    struct PhotoInfo
    {
        fs::path dir;         // <USB root>\GT4
        bool exists = false;
        size_t count = 0;     // *.JPG
        uint64_t bytes = 0;
        std::string newest;   // file name
        fs::file_time_type newestTime{};
    };
    PhotoInfo scanPhotos(const fs::path &usbRoot);

    // ---- zip (also used by the tests) ----
    struct ZipItem
    {
        std::string name; // forward slashes, UTF-8
        std::vector<uint8_t> data;
        fs::file_time_type time{};
    };
    bool writeZip(const fs::path &zip, const std::vector<ZipItem> &items, std::string &error);
    bool readZip(const fs::path &zip, std::vector<ZipItem> &items, std::string &error);
    // A safe relative name inside a folder (no drive, no "..", no absolute path).
    bool safeZipName(const std::string &name);

    std::string sjisTitle(const uint8_t *text, size_t size, size_t lineBreak); // icon.sys title to ASCII
}
