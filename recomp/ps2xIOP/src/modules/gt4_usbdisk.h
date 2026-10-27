#pragma once

// GT4's USB stick as USTORAGE ('PUST') presents it: a 512-byte-sector disk with
// one FAT16 volume (MBR partition type 0x0e at LBA 2048) built from a host
// folder (default game\usb0). GT4's own FAT code runs on the EE
// (PDISTD::Fat16, CORE 0x4c9d30..0x4ce260) and reads/writes this disk through
// PUST fn 4; this class formats it, imports the folder and reads back the
// files GT4 created (analysis/phase5/specs/p5b-usb-eyetoy.md 3.4-3.5).
// No IopHost dependency: unit-tested on its own.
//
// Storage is sparse: 4 KiB pages hold the metadata built here and every
// sector written; data of imported host files is read from the host file only
// when a sector of it is read (GT4 never reads file data back). Nothing is
// written to the host folder except by UsbFolderWriter (new / changed files).

#include <array>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace ps2x::iop::gt4
{
    class UsbDisk
    {
    public:
        static constexpr uint32_t SectorBytes = 512;
        static constexpr uint32_t PartitionStart = 2048; // 1 MiB aligned
        static constexpr uint32_t RootEntries = 512;
        static constexpr uint32_t MinSizeMiB = 64, MaxSizeMiB = 1024, DefaultSizeMiB = 128;

        struct Geometry
        {
            uint32_t totalSectors = 0;     // whole disk (fn 3: last LBA + 1)
            uint32_t partitionSectors = 0; // FAT volume (BPB total sectors)
            uint32_t sectorsPerCluster = 0;
            uint32_t reservedSectors = 0;
            uint32_t fatSectors = 0; // per FAT; 2 FATs
            uint32_t rootSectors = 0;
            uint32_t clusters = 0; // data clusters 2 .. clusters+1
            uint32_t fatLba = 0, rootLba = 0, dataLba = 0; // absolute LBAs
        };

        struct BuildStats
        {
            uint32_t files = 0, directories = 0, skipped = 0;
            uint64_t bytes = 0;
        };

        // Host-side record of a file the image was built from (or that was exported).
        struct Imported
        {
            std::string hostPath; // relative, '/' separated, host spelling
            bool fromHost = false;
            uint64_t hostSize = 0;
            std::filesystem::file_time_type hostTime{};
            uint32_t size = 0, firstCluster = 0;
            uint16_t date = 0, time = 0;
        };

        // A file in the FAT tree that differs from the import record.
        struct ChangedFile
        {
            std::string key;      // upper-case 8.3 path, '/' separated ("GT4/IMG0000.JPG")
            std::string hostPath; // name as a PC shows it: LFN, else 8.3 with NT case flags
            std::vector<uint8_t> data;
            uint32_t firstCluster = 0;
            uint16_t date = 0, time = 0;
        };

        using Log = std::function<void(bool warning, const std::string &message)>;

        explicit UsbDisk(Log log = {});

        // Formats a sizeMiB disk (clamped to 64..1024) and imports root; a missing
        // root is an empty stick. Replaces any previous image.
        BuildStats build(const std::filesystem::path &root, uint32_t sizeMiB);
        void clear();
        bool built() const { return built_; }
        const Geometry &geometry() const { return geometry_; }
        uint32_t sectorCount() const { return geometry_.totalSectors; }
        // Folder fingerprint taken at build (see manifest()).
        const std::string &builtManifest() const { return manifest_; }

        // Whole sectors; false when the range is outside the disk.
        bool read(uint32_t lba, uint32_t count, uint8_t *out) const;
        bool write(uint32_t lba, uint32_t count, const uint8_t *in);

        // FAT[1] bit 15 (first FAT): cleared by GT4's Fat16 constructor at mount
        // (CORE 0x4cdd30), set again by its destructor (0x4cde88) after an export.
        bool volumeClean() const;

        // Walks the FAT16 tree; returns files that are not in the import record or
        // whose size / first cluster / write time changed. Problems (broken chains,
        // loops) are appended to `problems` when given.
        std::vector<ChangedFile> collectChanges(std::vector<std::string> *problems = nullptr) const;
        // Records an exported file so later walks skip it; forget() undoes that.
        void acknowledge(const ChangedFile &file);
        void forget(const std::string &key);
        std::optional<Imported> imported(const std::string &key) const;

        // GT4's mount rules on the current image (MBR 0x4c9d30/0x4c9e68, boot sector
        // 0x4c9f10, FAT12 rejected at 0x1cb560): empty when GT4 mounts it as FAT16,
        // otherwise the reason.
        std::string checkLikeGt4() const;

        size_t residentPages() const { return pages_.size(); }

        // Cheap fingerprint of a folder (relative path, size, mtime of everything
        // the import looks at); empty for a missing folder.
        static std::string manifest(const std::filesystem::path &root);
        static Geometry computeGeometry(uint32_t sizeMiB);

    private:
        static constexpr uint32_t PageSectors = 8;
        using Page = std::array<uint8_t, PageSectors * SectorBytes>;

        struct Extent
        {
            uint32_t lba = 0, sectors = 0;
            std::filesystem::path host;
        };

        struct BuildContext;
        struct Child;

        void readSector(uint32_t lba, uint8_t *out) const;
        void writeSector(uint32_t lba, const uint8_t *in);
        void fillFromHost(uint32_t lba, uint8_t *out) const;
        uint32_t fatEntry(uint32_t cluster) const;
        uint32_t clusterLba(uint32_t cluster) const;
        void writeBytes(uint64_t offset, const uint8_t *data, size_t size);

        void formatEmpty(BuildContext &ctx);
        std::vector<Child> listDirectory(BuildContext &ctx, const std::filesystem::path &dir, int depth);
        uint32_t importDirectory(BuildContext &ctx, const std::filesystem::path &dir, uint32_t parentCluster,
                                 const std::string &keyPrefix, const std::string &hostPrefix, int depth,
                                 std::vector<Child> children, uint32_t entryCapacity);

        void log(bool warning, const std::string &message) const;

        Log log_;
        Geometry geometry_{};
        bool built_ = false;
        std::unordered_map<uint32_t, std::unique_ptr<Page>> pages_;
        std::vector<Extent> extents_; // sorted by lba
        std::map<std::string, Imported> imported_;
        std::string manifest_;
    };

    // Writes exported files into the host folder through <name>.pust-tmp + rename.
    // Never deletes; never overwrites a host file unless it is unchanged since it
    // was imported or since this writer last wrote it. Otherwise the file is
    // written under a free alternate name (<stem>~N<ext>) and the conflict logged.
    class UsbFolderWriter
    {
    public:
        struct Item
        {
            UsbDisk::ChangedFile file;
            std::optional<UsbDisk::Imported> imported;
        };
        struct Outcome
        {
            std::string key;
            bool ok = false;
            std::filesystem::path written; // final host path
            std::string message;
        };

        std::vector<Outcome> write(const std::filesystem::path &root, const std::vector<Item> &items);

    private:
        struct Written
        {
            uint64_t size = 0;
            std::filesystem::file_time_type time{};
        };
        std::map<std::wstring, Written> written_;
    };
}
