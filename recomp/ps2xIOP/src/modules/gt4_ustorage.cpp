#include "gt4_iop.h"
#include "gt4_usbdisk.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <fstream>
#include <thread>

// USTORAGE 1.1 ('PDI_USBSTORAGE', SHA-256 f5f0a46c...014be), SID 'PUST'
// 0x50555354: RPC handler 0x14c8, jump table 0x1a48 {0x1524, 0x1534, 0x1544,
// 0x156c, 0x15d4, 0x1610, 0x1620}. CORE's client (singleton 0x64ccfc, ctor
// 0x554258, calls through 0x578168) sends and receives the same 0x40-byte
// buffer: +0 device id, +4 lun, +8 result (written by the server), +0xc..
// arguments and results (analysis/phase5/specs/p5b-usb-eyetoy.md 3.2).
//
//   fn 0 version                -> +8 0x101 (lhu 0x1a74)
//   fn 1 list devices           -> +8 count, +0xc {device id, LUN count}..., 0 (0x13dc)
//   fn 2 inquiry {id, lun}      -> +8 result, +0xc vendor[8], +0x14 product[16] (0x00d4)
//   fn 3 capacity {id, lun}     -> +8 result, +0xc last LBA, +0x10 block size (0x04d0)
//   fn 4 transfer {id, lun, LBA @8, EE address @0xc, bytes @0x10, 1 = read @0x14}
//                               -> +8 result (0x09f0; CORE 0x5544e8)
//   fn 5 register driver        -> +8 0 (0x1280; idempotent; GT4's mount 0x1cb3a0 calls it first)
//   fn 6 unregister driver      -> +8 0 (0x1330; only CORE's EPSON printer set-up 0x1c9468)
//   fn >= 7                     -> +8 -1; unknown device id -> -1 (0x05b0 finds no slot)
//
// The stick is a UsbDisk (virtual FAT16, 512-byte sectors) built from a host
// folder, rebuilt on fn 5 when the folder changed (GT4 remounts for every
// export: 0x1cad08 always calls 0x1cb3a0). GT4's FAT driver does the rest. New
// files GT4 writes (/GT4/IMG%04d.JPG) are copied into the folder once PUST has
// been quiet for 0.5 s with the volume unmounted (FAT[1] clean bit, set by
// GT4's Fat16 destructor 0x4cde88 at the end of every export), and on fn 6,
// IOP reset and exit; the copy runs on a short-lived thread.
//
// Environment: GT4_USB=0 (no stick: fn 1 lists nothing, "no interface" as on
// a console without one), GT4_USB_ROOT (default <elf dir>\usb0),
// GT4_USB_SIZE_MB (64..1024, default 128), GT4_USB_LOG=1 (every call) or 2
// (also FNV-1a hashes of transferred data), GT4_USB_TRACE=<file> (appends
// every call for offline replay: u32 'PUSR', fn, result, EE buffer address,
// the 16 request words as received, u64 FNV-1a of the fn 4 data, u32 payload
// bytes, then the payload of fn 4 writes; all little-endian).

namespace ps2x::iop::gt4
{
    namespace
    {
        namespace fs = std::filesystem;

        constexpr uint32_t Sid = 0x50555354u;
        constexpr uint32_t DeviceId = 1;        // [I] usbd's id; GT4 only passes it back
        constexpr uint32_t QuietVBlanks = 30;   // 0.5 s at 60 Hz after the last write
        constexpr uint32_t ForceVBlanks = 600;  // 10 s: export even if GT4 left it mounted
        constexpr uint32_t ChunkBytes = 0x10000;
        constexpr uint32_t MaxErrorLogs = 32;

        std::string envText(const char *name)
        {
#if defined(_MSC_VER)
            char *value = nullptr;
            size_t size = 0;
            std::string text;
            if (_dupenv_s(&value, &size, name) == 0 && value)
                text = value;
            std::free(value);
            return text;
#else
            const char *value = std::getenv(name);
            return value ? value : "";
#endif
        }

        uint32_t envNumber(const char *name, uint32_t fallback)
        {
            const std::string text = envText(name);
            if (text.empty())
                return fallback;
            char *end = nullptr;
            const unsigned long value = std::strtoul(text.c_str(), &end, 0);
            return end && *end == '\0' ? static_cast<uint32_t>(value) : fallback;
        }

        // A folder or file named in the environment. On Windows the narrow value
        // is in the ANSI code page, not UTF-8, so the wide one is read instead.
        fs::path envPath(const char *name)
        {
#if defined(_MSC_VER)
            const std::wstring wide(name, name + std::strlen(name));
            wchar_t *value = nullptr;
            size_t size = 0;
            fs::path path;
            if (_wdupenv_s(&value, &size, wide.c_str()) == 0 && value)
                path = value;
            std::free(value);
            return path;
#else
            return fs::path(envText(name));
#endif
        }

        // Log text; never throws (u8string() does on an unpaired UTF-16 surrogate).
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

        std::string generic(const fs::path &path)
        {
            const std::u8string text = path.generic_u8string();
            return std::string(text.begin(), text.end());
        }

        class Ustorage final : public Component
        {
        public:
            explicit Ustorage(IopHost &host)
                : host_(host), disk_([this](bool warning, const std::string &message) {
                      host_.log(warning ? LogLevel::Warning : LogLevel::Info, "[GT4/PUST] " + message);
                  })
            {
                enabled_ = envNumber("GT4_USB", 1) != 0;
                sizeMiB_ = std::clamp(envNumber("GT4_USB_SIZE_MB", UsbDisk::DefaultSizeMiB), UsbDisk::MinSizeMiB,
                                      UsbDisk::MaxSizeMiB);
                logLevel_ = envNumber("GT4_USB_LOG", 0);
                attached_ = enabled_; // the IRX entry registers the driver at load (0x16d0 -> 0x1280)
            }

            ~Ustorage() override
            {
                try
                {
                    std::lock_guard lock(mutex_);
                    finishLocked("exit");
                }
                catch (...)
                {
                }
            }

            void reset() override
            {
                std::lock_guard lock(mutex_);
                try
                {
                    finishLocked("IOP reset");
                }
                catch (const std::exception &e)
                {
                    hostError("IOP reset", e);
                }
                disk_.clear();
                attached_ = enabled_;
                dirty_ = false;
                quiet_ = 0;
                errors_ = 0;
            }

            void onVBlank(bool start, uint64_t) override
            {
                if (!start)
                    return;
                std::lock_guard lock(mutex_);
                if (!dirty_ || ++quiet_ < QuietVBlanks)
                    return;
                const bool clean = disk_.volumeClean();
                if (!clean && quiet_ < ForceVBlanks)
                    return;
                if (!clean)
                    host_.log(LogLevel::Warning, "[GT4/PUST] volume still mounted 10 s after the last write; exporting anyway");
                try
                {
                    exportLocked(true, clean ? "idle" : "idle, mounted");
                }
                catch (const std::exception &e)
                {
                    hostError("export", e); // runs on the EE scheduler: never let it unwind there
                }
            }

            RpcResult handleRpc(const RpcRequest &request) override
            {
                if (request.sid != Sid)
                    return rejected();
                std::array<uint32_t, 0x40 / 4> w{};
                const size_t sendBytes = std::min<size_t>(request.send.size, sizeof(w));
                if (sendBytes < 0x18 || !host_.readGuest(request.send.address, w.data(), sendBytes))
                    return rejected();
                std::lock_guard lock(mutex_);
                const std::array<uint32_t, 16> sent = w;
                collect_ = tracing();
                dataHash_ = 0;
                payload_.clear();
                int32_t result = -1;
                try
                {
                    switch (request.function)
                    {
                    case 0:
                        result = 0x101;
                        break;
                    case 1:
                        result = listDevices(w);
                        break;
                    case 2:
                        result = inquiry(w);
                        break;
                    case 3:
                        result = capacity(w);
                        break;
                    case 4:
                        result = transfer(w);
                        break;
                    case 5:
                        result = registerDriver();
                        break;
                    case 6:
                        result = unregisterDriver();
                        break;
                    default:
                        error("fn " + hex(request.function) + " not in the table (fn >= 7) -> -1");
                        break;
                    }
                }
                catch (const std::exception &e)
                {
                    // A host failure (file system, memory) must not unwind into the EE:
                    // GT4 sees a failed call instead ("no interface" / "write error").
                    hostError("fn " + std::to_string(request.function), e);
                    result = request.function == 3 || request.function == 4 ? 1 : -1;
                }
                w[2] = static_cast<uint32_t>(result);
                if (collect_)
                    writeTrace(request, result, sent);
                if (logLevel_ >= 1 && request.function != 4)
                    host_.log(LogLevel::Info, "[GT4/PUST] fn " + std::to_string(request.function) + " id=" +
                                                  std::to_string(w[0]) + " lun=" + std::to_string(w[1]) + " -> " +
                                                  std::to_string(result));
                return reply(host_, request, w.data(), std::min<size_t>(request.receive.size, sizeof(w)));
            }

        private:
            bool addressed(const std::array<uint32_t, 16> &w) const
            {
                return attached_ && w[0] == DeviceId && w[1] == 0;
            }

            int32_t listDevices(std::array<uint32_t, 16> &w)
            {
                if (!attached_)
                {
                    w[3] = 0;
                    return 0;
                }
                w[3] = DeviceId;
                w[4] = 1; // LUN count (GET MAX LUN + 1)
                w[5] = 0;
                return 1;
            }

            int32_t inquiry(std::array<uint32_t, 16> &w)
            {
                if (!addressed(w))
                    return -1;
                // Copied by the EE (0x1cb45c), never compared.
                static constexpr char ident[] = "PS2X    GT4 USB DISK    ";
                std::memcpy(reinterpret_cast<uint8_t *>(w.data()) + 0x0c, ident, 24);
                return 0;
            }

            int32_t capacity(std::array<uint32_t, 16> &w)
            {
                if (!addressed(w))
                    return -1;
                if (!ensureDisk())
                    return 1;
                w[3] = disk_.sectorCount() - 1u;
                w[4] = UsbDisk::SectorBytes;
                return 0;
            }

            int32_t transfer(std::array<uint32_t, 16> &w)
            {
                const uint32_t lba = w[2], address = w[3], bytes = w[4];
                const bool read = w[5] != 0;
                const auto note = [&](int32_t result, uint64_t hash) {
                    dataHash_ = hash;
                    if (logLevel_ >= 1)
                    {
                        char text[160];
                        std::snprintf(text, sizeof(text), "[GT4/PUST] fn 4 %s lba=%u bytes=%u ee=0x%08x -> %d", read ? "read" : "write",
                                      lba, bytes, address, result);
                        std::string line = text;
                        if (logLevel_ >= 2 && result == 0)
                        {
                            std::snprintf(text, sizeof(text), " fnv=%016llx", static_cast<unsigned long long>(hash));
                            line += text;
                        }
                        host_.log(LogLevel::Info, line);
                    }
                    return result;
                };
                if (!addressed(w))
                    return note(-1, 0);
                if (bytes == 0)
                    return note(0, 0); // 0x0a3c: nothing to do
                if (!ensureDisk())
                    return note(1, 0);
                const uint32_t sectors = disk_.sectorCount();
                if (bytes % UsbDisk::SectorBytes || lba >= sectors || bytes / UsbDisk::SectorBytes > sectors - lba)
                {
                    error("fn 4 " + std::string(read ? "read" : "write") + " lba=" + std::to_string(lba) + " bytes=" +
                          std::to_string(bytes) + " outside the " + std::to_string(sectors) + "-sector disk -> 1");
                    return note(1, 0);
                }
                std::vector<uint8_t> chunk(std::min(bytes, ChunkBytes));
                uint64_t hash = 0xcbf29ce484222325ull;
                for (uint32_t done = 0; done < bytes;)
                {
                    const uint32_t n = std::min<uint32_t>(bytes - done, static_cast<uint32_t>(chunk.size()));
                    const uint32_t sector = lba + done / UsbDisk::SectorBytes;
                    if (read)
                    {
                        if (!disk_.read(sector, n / UsbDisk::SectorBytes, chunk.data()) ||
                            !host_.writeGuest(address + done, chunk.data(), n))
                        {
                            error("fn 4 read: EE buffer " + hex(address) + " not writable -> 1");
                            return note(1, 0);
                        }
                    }
                    else
                    {
                        if (!host_.readGuest(address + done, chunk.data(), n) ||
                            !disk_.write(sector, n / UsbDisk::SectorBytes, chunk.data()))
                        {
                            error("fn 4 write: EE buffer " + hex(address) + " not readable -> 1");
                            return note(1, 0);
                        }
                        if (collect_)
                            payload_.insert(payload_.end(), chunk.begin(), chunk.begin() + n);
                    }
                    if (logLevel_ >= 2 || collect_)
                        for (uint32_t i = 0; i < n; ++i)
                            hash = (hash ^ chunk[i]) * 0x100000001b3ull;
                    done += n;
                }
                if (!read)
                {
                    dirty_ = true;
                    quiet_ = 0;
                }
                return note(0, hash);
            }

            bool tracing()
            {
                if (!traceOpened_)
                {
                    traceOpened_ = true;
                    const fs::path file = envPath("GT4_USB_TRACE");
                    if (!file.empty())
                    {
                        std::error_code ec;
                        if (file.has_parent_path())
                            fs::create_directories(file.parent_path(), ec);
                        trace_.open(file, std::ios::binary | std::ios::app);
                        host_.log(trace_.is_open() ? LogLevel::Info : LogLevel::Warning,
                                  std::string("[GT4/PUST] ") + (trace_.is_open() ? "recording calls to " : "cannot open trace ") +
                                      utf8(file));
                    }
                }
                return trace_.is_open();
            }

            void writeTrace(const RpcRequest &request, int32_t result, const std::array<uint32_t, 16> &sent)
            {
                const uint32_t head[4] = {0x52535550u /* 'PUSR' */, request.function, static_cast<uint32_t>(result),
                                          request.send.address};
                const uint32_t bytes = static_cast<uint32_t>(payload_.size());
                trace_.write(reinterpret_cast<const char *>(head), sizeof(head));
                trace_.write(reinterpret_cast<const char *>(sent.data()), sizeof(sent));
                trace_.write(reinterpret_cast<const char *>(&dataHash_), sizeof(dataHash_));
                trace_.write(reinterpret_cast<const char *>(&bytes), sizeof(bytes));
                trace_.write(reinterpret_cast<const char *>(payload_.data()), bytes);
                trace_.flush();
            }

            int32_t registerDriver()
            {
                if (!enabled_)
                    return 0; // usbd accepts the driver; no stick on the bus
                if (!attached_)
                    host_.log(LogLevel::Info, "[GT4/PUST] stick attached");
                attached_ = true;
                prepareStick(true);
                return 0;
            }

            int32_t unregisterDriver()
            {
                if (attached_)
                    host_.log(LogLevel::Info, "[GT4/PUST] stick detached (fn 6)");
                attached_ = false;
                if (dirty_)
                    exportLocked(true, "fn 6");
                return 0;
            }

            bool ensureDisk()
            {
                if (!disk_.built())
                    prepareStick(false);
                return disk_.built();
            }

            void resolveRoot()
            {
                if (!root_.empty())
                    return;
                if (fs::path path = envPath("GT4_USB_ROOT"); !path.empty())
                {
                    root_ = std::move(path);
                }
                else
                {
                    const std::string elf = host_.hostPath(HostPathKind::ElfDirectory);
                    std::error_code ec;
                    root_ = (elf.empty() ? fs::current_path(ec) : fs::path(elf)) / "usb0";
                }
                root_ = root_.lexically_normal();
            }

            // fn 5 (and the first access): export what GT4 left, then build or, when
            // the folder changed, rebuild the image from it.
            void prepareStick(bool checkFolder)
            {
                resolveRoot();
                joinWorker();
                if (dirty_ || takeFailures())
                    exportLocked(false, "remount");
                bool failed = false;
                {
                    std::lock_guard exportLock(exportMutex_);
                    failed = !failed_.empty();
                }
                if (!disk_.built())
                {
                    build("attach");
                }
                else if (checkFolder && !failed && UsbDisk::manifest(root_) != disk_.builtManifest())
                {
                    build("folder changed");
                }
                else if (failed)
                {
                    host_.log(LogLevel::Warning, "[GT4/PUST] keeping the current image: an export failed and will be retried");
                }
            }

            void build(const char *reason)
            {
                UsbDisk::BuildStats stats;
                try
                {
                    stats = disk_.build(root_, sizeMiB_);
                }
                catch (...)
                {
                    disk_.clear(); // never serve a half-built image; the next access tries again
                    throw;
                }
                const UsbDisk::Geometry &g = disk_.geometry();
                host_.log(LogLevel::Info, "[GT4/PUST] stick " + utf8(root_) + " (" + reason + "): " +
                                              std::to_string(sizeMiB_) + " MiB FAT16, " + std::to_string(g.clusters) +
                                              " clusters of " + std::to_string(g.sectorsPerCluster * UsbDisk::SectorBytes) +
                                              " bytes; imported " + std::to_string(stats.files) + " files, " +
                                              std::to_string(stats.directories) + " folders, " +
                                              std::to_string(stats.bytes) + " bytes; skipped " + std::to_string(stats.skipped));
                if (const std::string check = disk_.checkLikeGt4(); !check.empty())
                    host_.log(LogLevel::Error, "[GT4/PUST] image fails GT4's mount rules: " + check);
                dirty_ = false;
                quiet_ = 0;
            }

            // Moves keys whose last export failed back into the "changed" set.
            bool takeFailures()
            {
                std::vector<std::string> keys;
                {
                    std::lock_guard exportLock(exportMutex_);
                    keys.swap(failed_);
                }
                for (const std::string &key : keys)
                    disk_.forget(key);
                return !keys.empty();
            }

            void exportLocked(bool async, const char *reason)
            {
                dirty_ = false;
                quiet_ = 0;
                joinWorker();
                takeFailures();
                std::vector<std::string> problems;
                std::vector<UsbDisk::ChangedFile> changed = disk_.collectChanges(&problems);
                for (const std::string &problem : problems)
                    error("FAT check: " + problem);
                if (changed.empty())
                    return;
                std::vector<UsbFolderWriter::Item> items;
                items.reserve(changed.size());
                for (UsbDisk::ChangedFile &file : changed)
                {
                    std::optional<UsbDisk::Imported> imported = disk_.imported(file.key);
                    disk_.acknowledge(file);
                    if (file.data.empty())
                    {
                        // GT4 creates the entry before it allocates clusters (0x4cc0a0, then
                        // 0x4cc380), so a write that failed on a full stick leaves an empty file.
                        host_.log(LogLevel::Warning, "[GT4/PUST] " + file.hostPath + " is empty on the stick (failed write?); not copied");
                        continue;
                    }
                    items.push_back({std::move(file), std::move(imported)});
                }
                if (items.empty())
                    return;
                if (async)
                {
                    try
                    {
                        worker_ = std::thread([this, root = root_, items, reason]() {
                            try
                            {
                                runExport(root, items, reason);
                            }
                            catch (...)
                            {
                                // an exception leaving a std::thread would end the process
                            }
                        });
                        return;
                    }
                    catch (const std::system_error &)
                    {
                        // no thread available: copy on this thread instead
                    }
                }
                runExport(root_, items, reason);
            }

            // Worker side: no mutex_ (only exportMutex_ for the failure list).
            void runExport(const fs::path &root, const std::vector<UsbFolderWriter::Item> &items, const char *reason)
            {
                std::vector<UsbFolderWriter::Outcome> outcomes;
                try
                {
                    outcomes = writer_.write(root, items);
                }
                catch (const std::exception &e)
                {
                    hostError("export", e); // items without an outcome are retried at the next mount
                }
                std::lock_guard exportLock(exportMutex_);
                for (size_t i = outcomes.size(); i < items.size(); ++i)
                    failed_.push_back(items[i].file.key);
                for (size_t i = 0; i < outcomes.size(); ++i)
                {
                    const UsbFolderWriter::Outcome &outcome = outcomes[i];
                    if (outcome.ok)
                    {
                        std::error_code ec;
                        std::string line = "[GT4/PUST] export " + generic(fs::relative(outcome.written, root, ec)) + " " +
                                           std::to_string(items[i].file.data.size()) + " bytes (" + reason + ")";
                        if (!outcome.message.empty())
                            line += "; " + outcome.message;
                        host_.log(outcome.message.empty() ? LogLevel::Info : LogLevel::Warning, line);
                    }
                    else
                    {
                        host_.log(LogLevel::Error, "[GT4/PUST] export of " + outcome.key + " failed: " + outcome.message);
                        failed_.push_back(outcome.key);
                    }
                }
            }

            void joinWorker()
            {
                if (worker_.joinable())
                    worker_.join();
            }

            void finishLocked(const char *reason)
            {
                joinWorker();
                if (disk_.built() && (dirty_ || takeFailures()))
                    exportLocked(false, reason);
                joinWorker();
            }

            void hostError(const std::string &where, const std::exception &e)
            {
                host_.log(LogLevel::Error, "[GT4/PUST] " + where + ": host error: " + e.what());
            }

            void error(const std::string &message)
            {
                if (errors_ < MaxErrorLogs)
                {
                    ++errors_;
                    host_.log(LogLevel::Warning, "[GT4/PUST] " + message);
                }
            }

            IopHost &host_;
            UsbDisk disk_;
            UsbFolderWriter writer_; // used by one export at a time (joinWorker)
            std::mutex mutex_;
            std::mutex exportMutex_;
            std::vector<std::string> failed_; // exportMutex_
            std::thread worker_;
            fs::path root_;
            bool enabled_ = true;
            bool attached_ = true;
            bool dirty_ = false;
            uint32_t quiet_ = 0;
            uint32_t sizeMiB_ = UsbDisk::DefaultSizeMiB;
            uint32_t logLevel_ = 0;
            uint32_t errors_ = 0;
            std::ofstream trace_; // GT4_USB_TRACE
            bool traceOpened_ = false;
            bool collect_ = false;
            uint64_t dataHash_ = 0;
            std::vector<uint8_t> payload_;
        };
    }

    std::unique_ptr<Component> createUstorage(IopHost &host)
    {
        return std::make_unique<Ustorage>(host);
    }
}
