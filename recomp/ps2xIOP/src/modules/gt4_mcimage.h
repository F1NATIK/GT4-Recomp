#pragma once

// PS2 8 MB memory card image in PCSX2's .ps2 layout (16384 pages of 512 data bytes + 16 spare
// bytes, the spare holding the page's 12-byte ECC), with the file-system behaviour of GT4's own
// MCMAN.IRX (mcman 0x230, analysis/iop-sys/disasm/gt4/MCMAN.txt). Written for GT4 from GT4's
// MCMAN and from a PCSX2 card formatted by the PS2 BIOS (analysis/saves/ps2-card/README.md);
// no GT3 code. It has no IOP or runtime dependency, so the card tools and the launcher can build
// it on its own.
//
// Results are MCMAN's: >= 0 success (a handle, a count, a position or bytes), -2 not formatted,
// -3 card full, -4 no such entry / bad name, -5 denied, -6 directory not empty, -7 no free handle.

#include <array>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace ps2x::iop::gt4::mcimage
{
    constexpr uint32_t PageBytes = 512;
    constexpr uint32_t SpareBytes = 16;
    constexpr uint32_t RawPageBytes = PageBytes + SpareBytes;
    constexpr uint32_t PagesPerCluster = 2;
    constexpr uint32_t ClusterBytes = PageBytes * PagesPerCluster;
    constexpr uint32_t PagesPerBlock = 16;
    constexpr uint32_t ClustersPerCard = 8192;
    constexpr uint32_t PagesPerCard = ClustersPerCard * PagesPerCluster;
    constexpr uint64_t ImageBytes = uint64_t(PagesPerCard) * RawPageBytes; // 8,650,752
    constexpr uint32_t EntryBytes = 512;

    enum Result : int32_t
    {
        Ok = 0,
        NoFormat = -2,
        Full = -3,
        NoEntry = -4,
        Denied = -5,
        NotEmpty = -6,
        NoHandle = -7,
    };

    // Directory entry mode bits (sceMcFileAttr*).
    constexpr uint16_t ModeRead = 0x0001, ModeWrite = 0x0002, ModeExecute = 0x0004, ModeProtect = 0x0008;
    constexpr uint16_t ModeFile = 0x0010, ModeDir = 0x0020, ModeClosed = 0x0080, Mode0400 = 0x0400;
    constexpr uint16_t ModePda = 0x0800, ModePs1 = 0x1000, ModeHidden = 0x2000, ModeExists = 0x8000;

    // Open flags as GT4's libmc passes them. MCMAN tests only 1, 2, 0x40, 0x200 and 0x1800; it
    // ignores 0x100 (append) and 0x400 (truncate). 0x200 on an existing file deletes and recreates it.
    constexpr uint32_t OpenRead = 0x1, OpenWrite = 0x2, OpenMkdir = 0x40, OpenCreate = 0x200;

    // SetFileInfo valid bits.
    constexpr uint32_t InfoCreated = 0x1, InfoModified = 0x2, InfoMode = 0x4, InfoAttr = 0x8, InfoName = 0x10;

    // sceMcStDateTime as stored on the card: {0, sec, min, hour, day, month, year lo, year hi}.
    struct Tod
    {
        uint8_t sec = 0, min = 0, hour = 0, day = 1, month = 1;
        uint16_t year = 2000;
        void store(uint8_t *p) const;
        static Tod load(const uint8_t *p);
        bool operator==(const Tod &o) const = default;
    };

    // The 64-byte sceMcTblGetDir row MCMAN's GetDir fills.
    struct DirInfo
    {
        Tod created, modified;
        uint32_t size = 0;
        uint16_t mode = 0;
        uint16_t unused = 0;
        uint32_t attr = 0;
        std::string name;
        void toTable(uint8_t out[64]) const;
        static DirInfo fromTable(const uint8_t in[64]);
    };

    // One raw directory entry (512 bytes on the card).
    struct Entry
    {
        uint16_t mode = 0, unused = 0;
        uint32_t length = 0;
        Tod created, modified;
        uint32_t cluster = 0, dirEntry = 0, attr = 0;
        std::string name;
    };

    // A walk of everything that exists on the card (hidden entries included), for the tools.
    struct Node
    {
        Entry entry;
        std::vector<uint32_t> chain; // allocatable clusters, in order
        std::vector<Node> children;  // directories only, "." and ".." left out
    };

    // PS2 memory card ECC: 3 bytes per 128-byte chunk; a page's spare is the 4 chunks' 12 bytes + 4 zeros.
    void ecc128(const uint8_t *chunk, uint8_t out[3]);
    void eccPage(const uint8_t *page, uint8_t spare[SpareBytes]);

    // The card's clock. Default: the host clock as the PS2's RTC keeps it (Japan time, UTC+9).
    using Clock = std::function<Tod()>;
    Tod hostTimeJst();
    Tod todFromUnix(int64_t seconds); // UTC seconds -> JST card time

    class Card
    {
    public:
        explicit Card(Clock clock = hostTimeJst);

        // ---- the image ----
        void createFormatted();   // a new card as the PS2's MCMAN formats it (all pages written)
        void createUnformatted(); // every byte 0xFF, the way PCSX2 creates a new card file
        bool loadFile(const std::string &path, std::string &error);
        bool loadBytes(const std::vector<uint8_t> &raw, std::string &error);
        // Writes the pages changed since the last save (the whole image the first time or when
        // the path changes). The file is opened and closed inside the call. When it fails, the
        // pages stay marked changed, so the next call writes them again.
        bool saveFile(const std::string &path, std::string &error);
        std::vector<uint8_t> rawImage() const; // 8,650,752 bytes, spares refreshed
        bool dirty() const { return anyDirty_; }

        // ---- card state ----
        bool formatted() const { return formatted_; }
        int32_t freeClusters() const;       // GetInfo "free": free FAT entries below the allocatable end
        uint32_t allocatableClusters() const { return maxAlloc_; }

        // ---- MCMAN operations (GT4's MCMAN #6..#16, #23, #11, #36) ----
        int32_t open(std::string_view path, uint32_t flags); // file handle 0..2, or 0 for a new directory
        int32_t close(int32_t fd);
        int32_t flush(int32_t fd);
        int32_t seek(int32_t fd, int32_t offset, int32_t whence);
        int32_t read(int32_t fd, void *data, uint32_t size);
        int32_t write(int32_t fd, const void *data, uint32_t size);
        // MCMAN #12: continueListing=false starts a listing (path = "dir/pattern"); maxent < 0 counts only.
        int32_t getDir(std::string_view path, bool continueListing, int32_t maxent, std::vector<DirInfo> *out);
        int32_t remove(std::string_view path);
        int32_t setInfo(std::string_view path, const DirInfo &info, uint32_t valid);
        int32_t getEntSpace(std::string_view path);
        int32_t format();
        int32_t unformat();
        void closeAll(); // IOP reset: forget open handles

        // ---- tools ----
        bool tree(Node &root, std::string &error) const;
        // Structural checks (FAT chains, sizes, cross links, free count, backup block, dir links).
        std::vector<std::string> check() const;

    private:
        struct Loc
        {
            uint32_t cluster = 0; // first allocatable cluster of the directory
            uint32_t index = 0;   // entry index in that directory
        };
        struct Dir
        {
            uint32_t first = 0; // first allocatable cluster of the directory's entries
            Loc info;           // where its own entry (length, times) lives: root "." or its entry in the parent
        };
        struct Lookup
        {
            int32_t status = 0; // 0 found, 1 missing (parent exists), 2 parent path missing, 3 parent not a directory
            Dir parent;
            Loc found;
            uint32_t freeSlot = 0;
            std::string leaf;
        };
        struct Handle
        {
            bool used = false, isDir = false, rd = false, wr = false, dirty = false;
            Loc entry;      // the file's own entry
            Loc parentInfo; // its directory's info entry
            uint32_t position = 0, size = 0, first = 0xffffffffu;
        };

        // pages and clusters
        uint8_t *pageData(uint32_t page) { return data_.data() + size_t(page) * PageBytes; }
        const uint8_t *pageData(uint32_t page) const { return data_.data() + size_t(page) * PageBytes; }
        void touchPage(uint32_t page);
        void readCluster(uint32_t absolute, uint8_t *out) const;
        void writeCluster(uint32_t absolute, const uint8_t *in);
        uint32_t fat(uint32_t n) const;
        void setFat(uint32_t n, uint32_t value);
        bool chain(uint32_t first, std::vector<uint32_t> &out, size_t limit = ClustersPerCard) const;
        int32_t allocate(uint32_t &cluster); // lowest free cluster, marked end-of-chain
        void parseSuperblock();

        // entries
        Entry readEntry(const Loc &loc) const;
        void writeEntry(const Loc &loc, const Entry &e);
        bool entryOffset(const Loc &loc, uint32_t &absCluster, uint32_t &offset) const;
        Dir rootDir() const;
        Dir dirOf(const Loc &loc, const Entry &e) const { return Dir{e.cluster, loc}; }
        uint32_t dirLength(const Dir &d) const { return readEntry(d.info).length; }
        Lookup lookup(std::string_view path) const;
        int32_t extendDirectory(const Dir &d, uint32_t length);
        int32_t deleteEntry(const Loc &loc);
        void updateFileEntry(const Handle &h, bool closed);
        bool validName(std::string_view name) const;

        Clock clock_;
        std::vector<uint8_t> data_;  // PagesPerCard * 512
        std::vector<uint8_t> spare_; // PagesPerCard * 16
        enum : uint8_t { PageClean = 0, PageWritten = 1, PageErased = 2 };
        std::vector<uint8_t> pageDirty_; // per page: clean, written (ECC computed on save) or erased
        uint32_t hint_ = 0;              // no free cluster below this one (MCMAN keeps the same hint)
        bool anyDirty_ = false;
        std::string savedPath_;
        bool formatted_ = false;
        // superblock values
        uint32_t allocOffset_ = 41, allocEnd_ = 8135, rootCluster_ = 0, backup1_ = 1023, backup2_ = 1022;
        uint32_t maxAlloc_ = 8001;
        std::array<uint32_t, 32> ifc_{};
        std::array<Handle, 3> handles_{};
        // GetDir listing state (MCMAN keeps one, global)
        bool listValid_ = false;
        Dir listDir_{};
        uint32_t listIndex_ = 0, listCount_ = 0;
        std::string listPattern_;
    };
}
