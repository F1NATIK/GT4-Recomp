#include "gt4_iop.h"
#include "gt4_eyetoy.h"
#include "gt4_net.h"
#include "ps2x/iop/gt4_host_glue.h"
#include "module_factories.h"

// GT4 IOP profile: one service owning every SID GT4's modules register, routed
// to per-module components. LOADFILE is IOPRP300 LOADFILE 2.2 (handler 0x57c,
// version word "3000" at 0x1c9c). CORE's group loader (0x577518) retries a load
// forever while the reply is negative, so a boot module whose services are not
// written yet is still accepted, and its RPCs are then rejected explicitly (the
// EE sees -1 from sceSifCallRpc) and logged.

namespace ps2x::iop::gt4
{
    namespace
    {
        constexpr uint32_t SidLoadfile = 0x80000006u;

        enum class ModuleKind
        {
            Served,   // a GT4 component answers its RPCs
            Absent,   // loads; reports no device / no network
            Pending,  // accepted so boot can proceed; RPCs not written yet
            NotResident, // entry returns 1 (module unloads itself)
        };

        struct ModuleInfo
        {
            std::string_view name;
            ModuleKind kind;
            std::string_view note;
        };

        // CORE boot groups A..C (analysis/iop-sys/gt4-module-load-order.json) and
        // the on-demand EYETOY / PDINETCNF loads.
        constexpr std::array<ModuleInfo, 25> Modules{{
            {"SIO2MAN", ModuleKind::Served, "SIO2 manager; pads served by the DBCMAN component"},
            {"MCMAN", ModuleKind::Served, "memory card manager (no EE RPC)"},
            {"MCSERV", ModuleKind::Served, "memory card server 0x80000400"},
            {"SIO2D", ModuleKind::Served, "SIO2 device layer; served with DBCMAN"},
            {"DBCMAN", ModuleKind::Served, "controller manager 0x80001300"},
            {"DS2U_D", ModuleKind::Served, "DualShock 2 driver; served with DBCMAN"},
            {"LIBSD", ModuleKind::Served, "sound library; software SPU2"},
            {"USBD", ModuleKind::Absent, "USB driver; no EE protocol (the USB stick is served by the PUST component)"},
            {"INET", ModuleKind::Absent, "TCP/IP stack; no network"},
            {"NETCNF", ModuleKind::Absent, "network configuration; no network"},
            {"INETCTL", ModuleKind::Absent, "network control; no network"},
            {"DEV9", ModuleKind::NotResident, "no network adapter (entry returns 1)"},
            {"SMAP", ModuleKind::Absent, "network adapter driver; never reached without DEV9"},
            {"MSIFRPC", ModuleKind::Absent, "multi-SIF RPC; no network"},
            {"LIBPDI", ModuleKind::Served, "Polyphony support library (no EE RPC)"},
            {"PDICDVD", ModuleKind::Served, "Polyphony disc reader PCDV/Pcdv"},
            {"PDISPU2", ModuleKind::Served, "Polyphony sound SPUP/SPUT"},
            {"PDISTR", ModuleKind::Served, "Polyphony streams STRP/PBGM/VOIC/MPG1/MPG2"},
            {"PDIUSB", ModuleKind::Absent, "Polyphony USB keyboard/mouse; no device"},
            {"USTORAGE", ModuleKind::Served, "Polyphony USB storage PUST; virtual FAT16 stick (GT4_USB_ROOT, default usb0)"},
            {"LGDEV", ModuleKind::Served, "Logitech wheel driver 0x046d046d (gt4_lgdev)"},
            {"POWOFF", ModuleKind::Served, "power-off handler; no EE RPC, host power button (gt4_powoff)"},
            {"RTINETB", ModuleKind::Absent, "network runtime; no network"},
            {"EYETOY", ModuleKind::Served, "EyeToy 0x80001400 (GT shirt unlock); camera per GT4_EYETOY"},
            {"PDINETCNF", ModuleKind::Absent, "Polyphony network configuration; no network"},
        }};

        const ModuleInfo *findModule(std::string_view path)
        {
            // cdrom0:\IRX\NAME.IRX;1 (CORE 0x577610 / 0x577af0 upper-cases it)
            constexpr std::string_view prefix = "cdrom0:\\IRX\\";
            constexpr std::string_view suffix = ".IRX;1";
            if (path.size() <= prefix.size() + suffix.size() || path.substr(0, prefix.size()) != prefix ||
                path.substr(path.size() - suffix.size()) != suffix)
            {
                return nullptr;
            }
            const auto name = path.substr(prefix.size(), path.size() - prefix.size() - suffix.size());
            for (const auto &module : Modules)
            {
                if (module.name == name)
                {
                    return &module;
                }
            }
            return nullptr;
        }

        // DEV9, SMAP and the INET stack load as served modules when GT4_NET is on
        // (p5b-network.md 3.6); with GT4_NET=off the table above applies unchanged.
        bool networkModule(std::string_view name)
        {
            for (std::string_view n : {"INET", "NETCNF", "INETCTL", "DEV9", "SMAP", "MSIFRPC", "RTINETB", "PDINETCNF"})
                if (name == n)
                    return true;
            return false;
        }

        std::unique_ptr<net::Network> makeNetwork(IopHost &host)
        {
            std::string error;
            const net::Config config = net::configFromEnvironment(error);
            if (!error.empty())
                host.log(LogLevel::Error, "[GT4/NET] " + error + "; network adaptor absent");
            if (!config.enabled())
                return nullptr;
            // RTINETB is reached through libmrpc, not sceSifCallRpc: the runtime's native
            // sceSifMBindRpc / sceSifMUnBindRpc / sceSifMCallRpc (gt4_host_glue.h, installed
            // with GT4_NET_MRPC=auto|on) route SID 0x80001201 to net::mrpcService(). With
            // GT4_NET_MRPC=off CORE's translated libmrpc is left and sceLibnetInitialize
            // (0x56f9b8) re-binds forever once LAN code runs.
            if (glue::mrpcMode() == glue::MrpcMode::Off)
                host.log(LogLevel::Warning, "[GT4/NET] GT4_NET on with GT4_NET_MRPC=off: CORE's translated libmrpc "
                                            "(0x590cc8 / 0x590fd8) never reaches RTINETB; LAN code will hang");
            else
                host.log(LogLevel::Info, "[GT4/NET] RTINETB reached through the runtime's native libmrpc "
                                         "(sceSifMBindRpc 0x590cc8 / sceSifMCallRpc 0x590fd8)");
            auto network = std::make_unique<net::Network>(host, config);
            if (!network->stack().ready())
            {
                // No usable host stack (Winsock): no adaptor rather than one whose sockets all fail.
                host.log(LogLevel::Error, "[GT4/NET] network stack unavailable; network adaptor absent");
                return nullptr;
            }
            return network;
        }

        const char *kindText(ModuleKind kind)
        {
            switch (kind)
            {
            case ModuleKind::Served: return "served";
            case ModuleKind::Absent: return "device/network absent";
            case ModuleKind::Pending: return "accepted, services not written yet";
            case ModuleKind::NotResident: return "not resident";
            }
            return "?";
        }
    }

    class Gt4Iop final : public detail::IopService
    {
    public:
        explicit Gt4Iop(IopHost &host)
            : host_(host), disc_(host), network_(makeNetwork(host)), cdvdfsv_(createCdvdfsv(host, disc_)), dbcman_(createDbcman(host)), mcserv_(createMcserv(host)), fileio_(createFileio(host, network_ != nullptr)), pdicdvd_(createPdicdvd(host, disc_)), absent_(createAbsent(host)), sound_(createSound(host, disc_)),
              ustorage_(createUstorage(host)), eyetoy_(createEyeToy(host, disc_)), lgdev_(createLgdev(host)), powoff_(createPowoff(host, [this](PowerOffStage stage) {
                  for (auto *component : components())
                      component->onPowerOff(stage);
                  // DEV9 off takes the network adaptor down too (net::Network is not a Component):
                  // every socket closes and blocked calls end with sceINETE_ABORT. Non-blocking.
                  if (stage == PowerOffStage::Dev9Off && network_)
                  {
                      network_->stack().shutdown();
                      host_.log(LogLevel::Info, "[GT4/NET] power-off: sockets closed");
                  }
              }))
        {
            if (network_)
            {
                routes_.emplace(net::SidRtinetb, &network_->rtinetb());
                routes_.emplace(net::SidPncf, &network_->pdinetcnf());
            }
            for (uint32_t sid : {0x80000592u, 0x80000593u, 0x80000595u, 0x80000597u, 0x8000059au, 0x8000059cu})
                routes_.emplace(sid, cdvdfsv_.get());
            for (uint32_t sid : {0x80001300u, 0x8000131cu, 0x8000131eu, 0x8000131fu})
                routes_.emplace(sid, dbcman_.get());
            routes_.emplace(0x80000400u, mcserv_.get());
            routes_.emplace(0x80000001u, fileio_.get());
            routes_.emplace(0x50434456u, pdicdvd_.get());
            routes_.emplace(0x50636476u, pdicdvd_.get());
            for (uint32_t sid : {0x53505550u, 0x53505554u, 0x5042474du, 0x53545250u, 0x564f4943u, 0x4d504731u, 0x4d504732u})
                routes_.emplace(sid, sound_.get());
            routes_.emplace(0x046d046du, lgdev_.get());
            routes_.emplace(0x50555354u, ustorage_.get());
            for (uint32_t sid : {0x62737550u, 0x80001201u, 0x504e4346u})
                routes_.emplace(sid, absent_.get());
            routes_.emplace(0x80001400u, eyetoy_.get());
            sids_.push_back(SidLoadfile);
            for (const auto &[sid, component] : routes_)
                sids_.push_back(sid);
            // Registered so they reach this service (and are logged) rather than
            // the generic core services; served as their components are written.
            for (uint32_t sid : {0x80000480u,
                                 0x50434456u, 0x50636476u, 0x5042474du, 0x4d504731u, 0x4d504732u, 0x53545250u,
                                 0x564f4943u, 0x53505550u, 0x53505554u, 0x62737550u, 0x50555354u, 0x046d046du,
                                 0x80001201u, 0x80001400u, 0x504e4346u})
                if (!routes_.contains(sid))
                    sids_.push_back(sid);
        }

        std::string_view name() const override { return "gt4-iop"; }
        std::span<const uint32_t> sids() const override { return sids_; }

        void reset() override
        {
            std::lock_guard lock(mutex_);
            loaded_.clear();
            nextId_ = 0x100;
            attempts_ = 0;
            lastPath_.clear();
            unknownLogged_ = 0;
            for (auto *component : components())
                component->reset();
            if (network_)
                network_->reset();
        }

        // RTINETB's asynchronous rings watch the EE's SIF DMA into their IOP memory.
        void onSifTransfer(const SifTransfer &transfer) override
        {
            if (network_)
                network_->onSifTransfer(transfer);
        }

        void appendDebugMetrics(std::vector<DebugMetric> &metrics) const override
        {
            if (network_)
                network_->appendMetrics(metrics);
        }

        void onVBlank(bool start, uint64_t tick) override
        {
            for (auto *component : components())
                component->onVBlank(start, tick);
        }

        RpcResult handleRpc(const RpcRequest &request) override
        {
            if (request.sid == SidLoadfile)
                return loadfile(request);
            if (const auto it = routes_.find(request.sid); it != routes_.end())
                return it->second->handleRpc(request);
            {
                std::lock_guard lock(mutex_);
                if (unknownLogged_ < 256)
                {
                    ++unknownLogged_;
                    host_.log(LogLevel::Warning, "[GT4/IOP] no service yet for sid=" + hex(request.sid) +
                                                     " fn=" + hex(request.function) + " send=" + std::to_string(request.send.size) +
                                                     " recv=" + std::to_string(request.receive.size) + "; rejected");
                }
            }
            return rejected();
        }

    private:
        // Every component, in VBlank order (POWOFF last: its power-off sequence runs after the others' VBlank work).
        std::array<Component *, 11> components() const
        {
            return {cdvdfsv_.get(), dbcman_.get(), mcserv_.get(), fileio_.get(), pdicdvd_.get(), absent_.get(), sound_.get(), ustorage_.get(), eyetoy_.get(), lgdev_.get(), powoff_.get()};
        }

        RpcResult loadfile(const RpcRequest &request)
        {
            std::lock_guard lock(mutex_);
            if (request.function == 0xff)
            {
                // Version word at LOADFILE 0x1c9c; CORE 0x5b6368 accepts only "3000".
                constexpr uint32_t version = 0x30303033u;
                return reply(host_, request, &version, 4);
            }
            if (request.function == 0)
            {
                // LoadModule packet (CORE 0x5b6a40): +0 argument bytes, +8 path
                // (0xfc), +0x104 arguments; reply {module id, entry result}.
                std::array<char, 0xfc> raw{};
                uint32_t argumentBytes = 0;
                if (!readSend(host_, request, &argumentBytes, 4) || request.send.size < 8 + raw.size() ||
                    !host_.readGuest(request.send.address + 8, raw.data(), raw.size()))
                {
                    return rejected();
                }
                const std::string path(raw.data(), strnlen(raw.data(), raw.size()));
                const ModuleInfo *module = findModule(path);
                if (!module && path.rfind("rom0:", 0) == 0)
                {
                    // BIOS modules (liberx's rom0:D2ELOADP300 / D2ELOADP) are not
                    // provided; modload#7 would answer KE_NOFILE (-203). liberx
                    // prints 'ELOADP is not available' and carries on.
                    host_.log(LogLevel::Info, "[GT4/LOADFILE] BIOS module " + path + " not available (KE_NOFILE)");
                    const std::array<int32_t, 2> failure{-203, 0};
                    return reply(host_, request, failure.data(), sizeof(failure));
                }
                if (!module)
                {
                    if (path != lastPath_)
                    {
                        attempts_ = 0;
                    }
                    lastPath_ = path;
                    ++attempts_;
                    if ((attempts_ & (attempts_ - 1)) == 0)
                    {
                        host_.log(LogLevel::Warning, "[GT4/LOADFILE] unknown module " + path + " rejected (attempt " +
                                                         std::to_string(attempts_) + ")");
                    }
                    const std::array<int32_t, 2> failure{-1, -1};
                    return reply(host_, request, failure.data(), sizeof(failure));
                }
                const ModuleKind kind = network_ && networkModule(module->name) ? ModuleKind::Served : module->kind;
                const std::string_view note = network_ && networkModule(module->name) ? std::string_view("LAN network (GT4_NET)") : module->note;
                const int32_t entryResult = kind == ModuleKind::NotResident ? 1 : 0;
                int32_t id = 0;
                if (const auto it = loaded_.find(std::string(module->name)); it != loaded_.end())
                {
                    id = it->second;
                }
                else
                {
                    id = nextId_++;
                    if (entryResult == 0)
                    {
                        loaded_.emplace(std::string(module->name), id);
                    }
                }
                host_.log(LogLevel::Info, "[GT4/LOADFILE] " + std::string(module->name) + " (" + std::string(note) +
                                              "): " + kindText(kind) + "; id=" + std::to_string(id) +
                                              " entry=" + std::to_string(entryResult) + " args=" + std::to_string(argumentBytes));
                if (module->name == "MSIFRPC")
                {
                    // MSIFRPC's entry (0x38) and CORE's libmrpc init (0x590978) each set the
                    // other side's SIF register 1 with command 0x80000001 and wait for their
                    // own. Native equivalent of the IOP half: EE sreg[1] = 1 in libkernel's
                    // register table (sceSifGetSreg 0x5b0880 reads 0x8869c0 + 4n).
                    constexpr uint32_t EeSregTable = 0x008869c0u;
                    const uint32_t one = 1;
                    host_.writeGuest(EeSregTable + 4u, &one, 4);
                }
                if (module->name == "EYETOY")
                {
                    // Arguments follow the path at +0x104, NUL-separated (GT4: "framesize=37\0samprate=0\0").
                    std::array<char, 0x100> args{};
                    size_t bytes = std::min<size_t>({argumentBytes, args.size(),
                                                     request.send.size > 0x104u ? request.send.size - 0x104u : 0u});
                    if (bytes && !host_.readGuest(request.send.address + 0x104u, args.data(), bytes))
                        bytes = 0; // the IRX defaults
                    eyetoy_->moduleLoaded(std::string_view(args.data(), bytes));
                }
                const std::array<int32_t, 2> success{id, entryResult};
                return reply(host_, request, success.data(), sizeof(success));
            }
            if (request.function == 6)
            {
                return loadBuffer(request);
            }
            if (request.function == 7 || request.function == 8 || request.function == 9)
            {
                // 7 StopModule {id, arg bytes, ..., args @0x104} -> {modload#20, module result};
                // 8 UnloadModule {id} -> {modload#21}; 9 SearchModuleByName {name @8} -> {id}.
                std::array<int32_t, 2> result{-202, 0}; // KE_UNKNOWN_MODULE
                if (request.function == 9)
                {
                    std::array<char, 0x40> raw{};
                    if (request.send.size < 8 + raw.size() || !host_.readGuest(request.send.address + 8, raw.data(), raw.size()))
                        return rejected();
                    const std::string name(raw.data(), strnlen(raw.data(), raw.size()));
                    // Internal names: DEV9 never stays resident (no adapter); PDINETCNF registers 'PDI_NETCNF'.
                    if (name == "PDI_NETCNF")
                        if (const auto it = loaded_.find("PDINETCNF"); it != loaded_.end())
                            result[0] = it->second;
                    // 'dev9' at shutdown (CORE 0x1027ec) finds DEV9 only when it stayed resident (GT4_NET on).
                    if (name == "dev9")
                        if (const auto it = loaded_.find("DEV9"); it != loaded_.end())
                            result[0] = it->second;
                    host_.log(LogLevel::Info, "[GT4/LOADFILE] search module '" + name + "' -> " + std::to_string(result[0]));
                }
                else
                {
                    int32_t id = 0;
                    if (!readSend(host_, request, &id, 4))
                        return rejected();
                    for (auto it = loaded_.begin(); it != loaded_.end(); ++it)
                    {
                        if (it->second != id)
                            continue;
                        result[0] = request.function == 7 ? id : 0; // stopped (module returns NO_RESIDENT_END)
                        result[1] = 1;
                        if (it->first == "EYETOY")
                            eyetoy_->moduleStopped(); // module stop clears the camera's SIF register 7
                        if (request.function == 8)
                        {
                            host_.log(LogLevel::Info, "[GT4/LOADFILE] unloaded " + it->first);
                            loaded_.erase(it);
                        }
                        else
                        {
                            host_.log(LogLevel::Info, "[GT4/LOADFILE] stopped " + it->first);
                        }
                        break;
                    }
                }
                return reply(host_, request, result.data(), std::min<size_t>(sizeof(result), request.receive.size));
            }
            host_.log(LogLevel::Warning, "[GT4/LOADFILE] unsupported function " + hex(request.function) +
                                             " send=" + std::to_string(request.send.size) +
                                             " recv=" + std::to_string(request.receive.size));
            return rejected();
        }



        RpcResult loadBuffer(const RpcRequest &request)
        {
            // fn 6 LoadModuleBuffer: {IOP buffer, arg bytes, ..., args @0x104}. The only
            // GT4 user is CORE's embedded 'rt_ac' probe (0x520758, data 0x64ae80): a
            // non-resident module whose first argument character selects an IOP status
            // query and whose entry result is (value << 8) | 1. Answered natively.
            std::array<uint32_t, 2> header{};
            std::array<char, 0x40> args{};
            if (!readSend(host_, request, header.data(), sizeof(header)) || request.send.size < 0x104 + args.size() ||
                !host_.readGuest(request.send.address + 0x104, args.data(), args.size()))
                return rejected();
            const char query = header[1] ? args[0] : '\0';
            uint32_t value = 0;
            constexpr uint32_t IopRamBytes = 2u * 1024u * 1024u;
            switch (query)
            {
            case '0': value = host_.maxFreeIopMemory(); break;                                      // sysmem#8
            case '1': value = IopRamBytes - std::min(IopRamBytes, host_.maxFreeIopMemory()); break; // total - free
            case '2': value = 16; break;                                                             // thread count (native IOP has none)
            case '3': value = static_cast<uint32_t>(loaded_.size()) + 16u; break;                    // IOPRP300 modules + loaded
            default:
                host_.log(LogLevel::Warning, std::string("[GT4/LOADFILE] rt_ac query '") + (query ? query : '?') + "' not answered");
                return rejected();
            }
            const std::array<int32_t, 2> result{nextId_++, static_cast<int32_t>((value << 8) | 1u)};
            host_.log(LogLevel::Info, std::string("[GT4/LOADFILE] rt_ac query '") + query + "' answered natively: " + std::to_string(value));
            return reply(host_, request, result.data(), sizeof(result));
        }

        IopHost &host_;
        Disc disc_;
        std::unique_ptr<net::Network> network_; // GT4_NET != off
        std::unique_ptr<Component> cdvdfsv_;
        std::unique_ptr<Component> dbcman_;
        std::unique_ptr<Component> mcserv_;
        std::unique_ptr<Component> fileio_;
        std::unique_ptr<Component> pdicdvd_;
        std::unique_ptr<Component> absent_;
        std::unique_ptr<Component> sound_;
        std::unique_ptr<Component> ustorage_;
        std::unique_ptr<EyeToyService> eyetoy_;
        std::unique_ptr<Component> lgdev_;
        std::unique_ptr<Component> powoff_;
        std::map<uint32_t, Component *> routes_;
        std::vector<uint32_t> sids_;
        std::mutex mutex_;
        std::map<std::string, int32_t> loaded_;
        int32_t nextId_ = 0x100;
        uint64_t attempts_ = 0;
        std::string lastPath_;
        uint32_t unknownLogged_ = 0;
    };
}

namespace ps2x::iop::detail
{
    std::unique_ptr<IopService> createGt4IopService(IopHost &host)
    {
        return std::make_unique<gt4::Gt4Iop>(host);
    }
}
