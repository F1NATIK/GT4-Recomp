#include "gt4_iop.h"
#include "gt4_mcimage.h"

#include <cctype>
#include <cstdlib>

// MCSERV 2.16 (identity 0x210) over MCMAN 2.7 (0x230), SID 0x80000400, as the
// EE's libmc 3020 uses it (analysis/phase4/specs/mcserv.md). This component owns
// the GT4 packet layouts, the GetInfo card states and the EE-side data blocks
// libmc's end callbacks consume. Each slot is backed by one of:
//   - the runtime's game-independent folder card (IopHost::memoryCard, <game>\mc0);
//   - a PCSX2-style .ps2 card image (gt4_mcimage.cpp), with GT4's MCMAN file-system rules;
//   - nothing (no card in the slot).
// GT4_MC0 / GT4_MC1 choose per slot: unset -> slot 1 folder card, slot 2 empty;
// "folder" (slot 1 only), "none", or a path to a .ps2 file (relative paths are
// taken from the CORE.elf folder; a missing file is created as a new formatted card;
// both slots on one file is refused). A card-image write that does not reach the file
// is reported to GT4 as -5 on the write/close/flush.

namespace ps2x::iop::gt4
{
    namespace
    {
        constexpr uint32_t Sid = 0x80000400u;
        constexpr int32_t NoCard = -11;
        constexpr int32_t TypePs2 = 2;
        constexpr int32_t TotalClusters = 8173; // folder card free-space estimate (a real card: 8000 when empty)
        // Image handles are reported as ImageFdBase + port * 4 + MCMAN handle, apart from the folder
        // card's own handles (1, 2, 3, ... counting up for the whole run).
        constexpr int32_t ImageFdBase = 0x40000000;

        enum class Kind
        {
            None,
            Folder,
            Image
        };

        struct Slot
        {
            Kind kind = Kind::None;
            std::string path;
            std::unique_ptr<mcimage::Card> card;
            bool failed = false;
        };

        // MCMAN ignores 0x100 (append) and 0x400 (truncate); 0x200 (create) adds write and deletes
        // and recreates an existing file (MCMAN.txt 0x6be0, 0x7078). The folder card reads the same
        // bits as host fopen modes, so they are translated: create -> write + truncate.
        uint32_t folderOpenFlags(uint32_t flags)
        {
            uint32_t f = flags & ~(0x100u | 0x400u);
            if (f & 0x200u)
                f |= 0x402u;
            return f;
        }

        // True when two card paths name the same file (Windows: case-insensitive).
        bool sameCardFile(const std::string &a, const std::string &b)
        {
            std::error_code ec;
            if (std::filesystem::equivalent(std::filesystem::path(a), std::filesystem::path(b), ec) && !ec)
                return true;
            auto key = [](const std::string &s)
            {
                std::error_code e;
                std::string k = std::filesystem::absolute(std::filesystem::path(s), e).lexically_normal().string();
                for (char &c : k)
                    c = char(std::tolower(static_cast<unsigned char>(c)));
                return k;
            };
            return key(a) == key(b);
        }

        class Mcserv final : public Component
        {
        public:
            explicit Mcserv(IopHost &host) : host_(host) {}

            void reset() override
            {
                std::lock_guard lock(mutex_);
                seen_ = {};
                initialised_ = false;
                logged_ = 0;
                for (Slot &s : slots_)
                    if (s.card)
                        s.card->closeAll();
            }

            RpcResult handleRpc(const RpcRequest &request) override
            {
                std::lock_guard lock(mutex_);
                if (request.sid != Sid)
                    return rejected();
                configure();
                std::array<uint32_t, 0x30 / 4> p{};
                if (request.function != 0xfe && !readSend(host_, request, p.data(), sizeof(p)))
                    return rejected();
                const uint32_t name = request.send.address + 0x14;
                int32_t result = 0;
                switch (request.function)
                {
                case 0xfe:
                {
                    // {mcman#37(w0) = 0, mcserv 0x210, mcman 0x230}; libmc needs >= 0x20a / 0x20e.
                    if (!initialised_)
                    {
                        if (slots_[0].kind == Kind::Folder)
                            call(MemoryCardOperation::Init, {});
                        for (uint32_t port = 0; port < 2; ++port)
                            if (slots_[port].kind == Kind::Image)
                                image(port);
                        initialised_ = true;
                    }
                    const std::array<uint32_t, 3> words{0u, 0x210u, 0x230u};
                    return reply(host_, request, words.data(), sizeof(words));
                }
                case 0x15: // GetSlotMax(port): no multitap
                    result = 1;
                    break;
                case 0x01:
                    result = getInfo(p);
                    break;
                case 0x02: // Open / Mkdir {port, slot, flags, ..., name @0x14}
                    if (mcimage::Card *card = image(p[0]))
                    {
                        result = card->open(readName(name), p[2]);
                        if (result >= 0 && !(p[2] & 0x40u))
                            result = ImageFdBase + int32_t(p[0] & 1) * 4 + result;
                        commit(p[0]);
                    }
                    else if (!folder(p[0]))
                        result = NoCard;
                    else if (p[2] & 0x40u)
                        result = call(MemoryCardOperation::Mkdir, {p[0], p[1], name});
                    else
                        result = call(MemoryCardOperation::Open, {p[0], p[1], name, folderOpenFlags(p[2])});
                    break;
                case 0x03:
                case 0x0a:
                {
                    const bool flush = request.function == 0x0a;
                    uint32_t port = 0;
                    int32_t fd = 0;
                    if (imageHandle(p[0], port, fd))
                    {
                        result = flush ? slots_[port].card->flush(fd) : slots_[port].card->close(fd);
                        // The save only counts when it reached the card file (GT4 then reports an error).
                        if (!commit(port) && result >= 0)
                            result = mcimage::Denied;
                    }
                    else
                        result = call(flush ? MemoryCardOperation::Flush : MemoryCardOperation::Close, {p[0]});
                    break;
                }
                case 0x04: // Seek {fd, -, -, -, offset @0x10, whence @0x14}
                {
                    uint32_t port = 0;
                    int32_t fd = 0;
                    if (imageHandle(p[0], port, fd))
                        result = slots_[port].card->seek(fd, int32_t(p[4]), int32_t(p[5]));
                    else
                        result = call(MemoryCardOperation::Seek, {p[0], p[4], p[5]});
                    break;
                }
                case 0x05:
                    result = read(p);
                    break;
                case 0x06:
                    result = write(request, p);
                    break;
                case 0x0d: // GetDir {port, slot, mode, maxent, table @0x10, name @0x14}
                    if (mcimage::Card *card = image(p[0]))
                        result = getDirImage(*card, readName(name), p);
                    else
                        result = folder(p[0]) ? call(MemoryCardOperation::GetDir, {p[0], p[1], name, p[2], p[3], p[4]}) : NoCard;
                    break;
                case 0x0e: // SetFileInfo / Rename {port, slot, valid, -, info @0x10, name @0x14}
                    if (mcimage::Card *card = image(p[0]))
                    {
                        std::array<uint8_t, 64> table{};
                        host_.readGuest(p[4], table.data(), table.size());
                        result = card->setInfo(readName(name), mcimage::DirInfo::fromTable(table.data()), p[2]);
                        commit(p[0]);
                    }
                    else
                        result = folder(p[0]) ? call(MemoryCardOperation::SetFileInfo, {p[0], p[1], name, p[4], p[2]}) : NoCard;
                    break;
                case 0x0f:
                    if (mcimage::Card *card = image(p[0]))
                    {
                        result = card->remove(readName(name));
                        commit(p[0]);
                    }
                    else
                        result = folder(p[0]) ? call(MemoryCardOperation::Delete, {p[0], p[1], name}) : NoCard;
                    break;
                case 0x10:
                case 0x11:
                {
                    const bool format = request.function == 0x10;
                    if (mcimage::Card *card = image(p[1]))
                    {
                        result = format ? card->format() : card->unformat();
                        seen_[p[1] & 1] = false;
                        commit(p[1]);
                    }
                    else if (folder(p[1]))
                        result = call(format ? MemoryCardOperation::Format : MemoryCardOperation::Unformat, {p[1], p[2]});
                    else
                        result = NoCard;
                    break;
                }
                case 0x12: // GetEntSpace
                    if (mcimage::Card *card = image(p[0]))
                        result = card->getEntSpace(readName(name));
                    else
                        result = folder(p[0]) ? freeClusters() : NoCard;
                    break;
                default:
                    if (logged_ < 32)
                    {
                        ++logged_;
                        host_.log(LogLevel::Warning, "[GT4/MCSERV] unsupported fn " + hex(request.function));
                    }
                    return rejected();
                }
                if (logged_ < 256 && request.function != 0x15 && request.function != 0x01)
                {
                    ++logged_;
                    host_.log(LogLevel::Info, "[GT4/MCSERV] fn " + hex(request.function) + " -> " + std::to_string(result));
                }
                return reply(host_, request, &result, 4);
            }

        private:
            void configure()
            {
                if (configured_)
                    return;
                configured_ = true;
                slots_[0].kind = Kind::Folder;
                slots_[1].kind = Kind::None;
                for (uint32_t port = 0; port < 2; ++port)
                {
                    const char *value = std::getenv(port ? "GT4_MC1" : "GT4_MC0");
                    if (!value || !*value)
                        continue;
                    std::string text(value);
                    std::string lower(text);
                    for (char &c : lower)
                        c = char(std::tolower(static_cast<unsigned char>(c)));
                    if (lower == "folder")
                    {
                        if (port == 0)
                            slots_[0].kind = Kind::Folder;
                        else
                            host_.log(LogLevel::Warning, "[GT4/MCSERV] GT4_MC1=folder is not supported; slot 2 stays empty");
                    }
                    else if (lower == "none" || lower == "empty")
                        slots_[port].kind = Kind::None;
                    else
                    {
                        std::filesystem::path path(text);
                        const std::string base = host_.hostPath(HostPathKind::ElfDirectory);
                        if (path.is_relative() && !base.empty())
                            path = std::filesystem::path(base) / path;
                        slots_[port].kind = Kind::Image;
                        slots_[port].path = path.lexically_normal().string();
                    }
                    host_.log(LogLevel::Info, std::string("[GT4/MCSERV] slot ") + char('1' + port) + ": " +
                                                  (slots_[port].kind == Kind::Image ? "card image " + slots_[port].path
                                                   : slots_[port].kind == Kind::Folder ? std::string("folder card")
                                                                                       : std::string("no card")));
                }
                // Two slots on one file would overwrite each other's pages.
                if (slots_[0].kind == Kind::Image && slots_[1].kind == Kind::Image &&
                    sameCardFile(slots_[0].path, slots_[1].path))
                {
                    slots_[1].kind = Kind::None;
                    host_.log(LogLevel::Error, "[GT4/MCSERV] GT4_MC1 names the same card file as GT4_MC0; slot 2 stays empty");
                }
            }

            bool folder(uint32_t port) const { return port == 0 && slots_[0].kind == Kind::Folder; }

            // The image card of a port, loaded (or created) on first use; null when the port has none.
            mcimage::Card *image(uint32_t port)
            {
                if (port > 1)
                    return nullptr;
                Slot &s = slots_[port];
                if (s.kind != Kind::Image || s.failed)
                    return nullptr;
                if (!s.card)
                {
                    auto card = std::make_unique<mcimage::Card>();
                    std::string error;
                    std::error_code ec;
                    if (std::filesystem::exists(std::filesystem::path(s.path), ec))
                    {
                        if (!card->loadFile(s.path, error))
                        {
                            s.failed = true;
                            host_.log(LogLevel::Error, "[GT4/MCSERV] card image not used (slot stays empty): " + error);
                            return nullptr;
                        }
                    }
                    else
                    {
                        card->createFormatted();
                        if (!card->saveFile(s.path, error))
                        {
                            s.failed = true;
                            host_.log(LogLevel::Error, "[GT4/MCSERV] cannot create card image: " + error);
                            return nullptr;
                        }
                        host_.log(LogLevel::Info, "[GT4/MCSERV] new formatted card image: " + s.path);
                    }
                    s.card = std::move(card);
                }
                return s.card.get();
            }

            // Writes the card's changed pages to its file. False when that failed: the pages stay
            // changed in memory and the next commit writes them again.
            bool commit(uint32_t port)
            {
                Slot &s = slots_[port & 1];
                std::string error;
                if (!s.card || !s.card->dirty() || s.card->saveFile(s.path, error))
                    return true;
                if (commitErrors_ < 32)
                {
                    ++commitErrors_;
                    host_.log(LogLevel::Error, "[GT4/MCSERV] card image not saved: " + error);
                }
                return false;
            }

            bool imageHandle(uint32_t value, uint32_t &port, int32_t &fd)
            {
                const int32_t v = int32_t(value);
                if (v < ImageFdBase || v >= ImageFdBase + 8)
                    return false;
                port = uint32_t(v - ImageFdBase) / 4;
                fd = (v - ImageFdBase) % 4;
                return fd < 3 && image(port) != nullptr;
            }

            std::string readName(uint32_t address) const
            {
                std::array<char, 0x400> text{};
                host_.readGuest(address, text.data(), text.size() - 1);
                return std::string(text.data());
            }

            int32_t call(MemoryCardOperation operation, std::initializer_list<uint32_t> args)
            {
                MemoryCardRequest request;
                request.operation = operation;
                size_t i = 0;
                for (uint32_t value : args)
                    request.arguments[i++] = value;
                return host_.memoryCard(request);
            }

            int32_t freeClusters()
            {
                // Clusters used by the card folder, rounded per file.
                uint64_t used = 0;
                const std::string root = host_.hostPath(HostPathKind::MemoryCardRoot);
                std::error_code ec;
                for (auto it = std::filesystem::recursive_directory_iterator(root, ec);
                     !ec && it != std::filesystem::recursive_directory_iterator(); it.increment(ec))
                {
                    used += it->is_directory(ec) ? 2u : (it->file_size(ec) + 1023u) / 1024u + 1u;
                }
                return static_cast<int32_t>(std::max<int64_t>(0, TotalClusters - static_cast<int64_t>(used)));
            }

            int32_t getInfo(const std::array<uint32_t, 12> &p)
            {
                // {port, slot, format?, free?, type? @0x14, -, -, rdata @0x1c}; rdata (0xc0)
                // +0 type, +4 free, +0x90 formatted, read by libmc's end callback 0x58dfe0.
                const uint32_t port = p[1], rdata = p[7];
                std::array<int32_t, 0xc0 / 4> rd{};
                int32_t result = NoCard;
                if (mcimage::Card *card = image(port))
                {
                    rd[0] = TypePs2;
                    if (card->formatted())
                    {
                        rd[1] = card->freeClusters();
                        rd[0x90 / 4] = 1;
                        result = seen_[port & 1] ? 0 : -1;
                        seen_[port & 1] = true;
                    }
                    else
                    {
                        // Unformatted: -2 on every poll, free 0, format 0 (mcserv.md section 6).
                        result = -2;
                        seen_[port & 1] = false;
                    }
                }
                else if (folder(port))
                {
                    rd[0] = TypePs2;
                    rd[1] = freeClusters();
                    rd[0x90 / 4] = 1;
                    // First sight of a card is -1 (GT4 then asks for free space and format).
                    result = seen_[port & 1] ? 0 : -1;
                    seen_[port & 1] = true;
                }
                if (rdata)
                    host_.writeGuest(rdata, rd.data(), sizeof(rd));
                return result;
            }

            int32_t getDirImage(mcimage::Card &card, const std::string &name, const std::array<uint32_t, 12> &p)
            {
                // MCSERV 0xad0: one mcman#12 call per entry, the packet's mode first and then 1
                // (continue); each 64-byte row goes to the EE table at +0x10 + 64*i.
                const int32_t maxent = int32_t(p[3]);
                const bool cont = (p[2] & 0xffffu) != 0;
                if (maxent < 0)
                    return card.getDir(name, cont, -1, nullptr);
                int32_t count = 0;
                for (int32_t i = 0; i < maxent; ++i)
                {
                    std::vector<mcimage::DirInfo> row;
                    const int32_t r = card.getDir(name, i == 0 ? cont : true, 1, &row);
                    if (r < 0)
                        return r;
                    if (r == 0 || row.empty())
                        break;
                    std::array<uint8_t, 64> table{};
                    row[0].toTable(table.data());
                    host_.writeGuest(p[4] + 64u * uint32_t(i), table.data(), table.size());
                    ++count;
                }
                return count;
            }

            int32_t read(const std::array<uint32_t, 12> &p)
            {
                // {fd, -, -, size @0xc, -, -, buf @0x18, rdata @0x1c}: data goes straight
                // to buf; rdata head/tail counts 0 so the end callback copies nothing.
                int32_t result;
                uint32_t port = 0;
                int32_t fd = 0;
                if (imageHandle(p[0], port, fd))
                {
                    // A file on an 8 MB card is never larger than the card.
                    std::vector<uint8_t> data(std::min<uint64_t>(p[3], mcimage::ImageBytes));
                    result = slots_[port].card->read(fd, data.data(), uint32_t(data.size()));
                    if (result > 0 && !host_.writeGuest(p[6], data.data(), size_t(result)))
                        result = mcimage::Denied;
                }
                else
                    result = call(MemoryCardOperation::Read, {p[0], p[6], p[3]});
                const std::array<uint32_t, 0xc0 / 4> rd{};
                if (p[7])
                    host_.writeGuest(p[7], rd.data(), sizeof(rd));
                return result;
            }

            int32_t write(const RpcRequest &request, const std::array<uint32_t, 12> &p)
            {
                // {fd, -, -, rest @0xc, -, head @0x14, rest buf @0x18, -, head bytes @0x20}
                const uint32_t head = std::min<uint32_t>(p[5], 16u);
                uint32_t port = 0;
                int32_t fd = 0;
                if (imageHandle(p[0], port, fd))
                {
                    mcimage::Card &card = *slots_[port].card;
                    int32_t total = 0;
                    if (head)
                    {
                        std::array<uint8_t, 16> bytes{};
                        host_.readGuest(request.send.address + 0x20u, bytes.data(), head);
                        const int32_t r = card.write(fd, bytes.data(), head);
                        if (r != static_cast<int32_t>(head))
                        {
                            commit(port);
                            return r;
                        }
                        total = r;
                    }
                    if (p[3] && p[6])
                    {
                        if (p[3] > mcimage::ImageBytes)
                        {
                            commit(port);
                            return mcimage::Full;
                        }
                        std::vector<uint8_t> bytes(p[3]);
                        if (!host_.readGuest(p[6], bytes.data(), bytes.size()))
                        {
                            commit(port);
                            return mcimage::Denied;
                        }
                        const int32_t r = card.write(fd, bytes.data(), p[3]);
                        if (r < 0)
                        {
                            commit(port);
                            return r;
                        }
                        total += r;
                    }
                    if (!commit(port))
                        return mcimage::Denied; // the card file could not be written (the pages stay pending)
                    return total;
                }
                int32_t total = 0;
                if (head)
                {
                    const int32_t r = call(MemoryCardOperation::Write, {p[0], request.send.address + 0x20u, head});
                    if (r != static_cast<int32_t>(head))
                        return r;
                    total = r;
                }
                if (p[3] && p[6])
                {
                    const int32_t r = call(MemoryCardOperation::Write, {p[0], p[6], p[3]});
                    if (r < 0)
                        return r;
                    total += r;
                }
                return total;
            }

            IopHost &host_;
            std::mutex mutex_;
            std::array<bool, 2> seen_{};
            std::array<Slot, 2> slots_{};
            bool configured_ = false;
            bool initialised_ = false;
            uint32_t logged_ = 0;
            uint32_t commitErrors_ = 0;
        };
    }

    std::unique_ptr<Component> createMcserv(IopHost &host)
    {
        return std::make_unique<Mcserv>(host);
    }
}
