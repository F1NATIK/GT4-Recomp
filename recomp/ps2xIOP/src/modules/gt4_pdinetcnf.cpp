#include "gt4_net.h"

#include <cstdlib>
#include <cstring>
#include <string_view>

// PDINETCNF.IRX ("PDI_NETCNF" v1.1, SID 'PNCF' 0x504e4346) as GT4 uses it
// (analysis\iop-sys\disasm\gt4\PDINETCNF.txt, handler 0x0, table 0x2050) [C]:
//   0 version (halfword 0x101 at 0x20f4)   1 GetCount   2 GetList
//   3 env from the EE's sceNetcnfifData (0x9c -> 0xa90): reply {result, env}
//   4 load entry  5 add  6 edit  7 delete  8 delete all  9 set latest; >= 10 -> -1.
// The LAN path (mNetwork.initializeInterface -> NetworkInterface::configure,
// CORE 0x4eee70) needs 0, 1 and 3; 1, 2 and 4-9 belong to GT4's network page
// (projects/network ConnectRoot) over the memory-card network files (BWNETCNF).
// No card file is read or written. Instead (multi-monitor work, 2026-10-14) the
// page is shown one ready configuration, as if the card held a "Your Network
// Configuration" file for the PS2's own Ethernet adaptor: GetCount 1 per type,
// GetList one combination ("Combination1": interface "GT4 LAN", device
// "SCE/Ethernet (Network Adaptor)", GT4's own adaptor names) and LoadEntry its settings (static, the LAN address in use).
// GT4 then connects with it (by itself with LAN_auto_connect). Add, edit,
// delete (5-9) answer an open error [I code]. GT4_NET_CONFIG=none: no entries
// (GT4's "No network configuration file found").
// Layouts (PDINETCNF 0x4e0 / 0x740 / 0x95c, CORE 0x4ed888 / 0x4ed980) [C]:
//   request {word0 EE buffer (fn 2, 4) or type (fn 1), word1 type, path @0x10,
//   name @0x1c}; fn 2 type 0: 0x70-byte rows {+4 stat, +0x40 name[16],
//   +0x50 interface file[16], +0x60 device file[16]}; types 1, 2: 0x1d0-byte rows
//   {+0 kind, +4 stat, +0xc0 file name[16], +0xd0 name[256]}; fn 4: 0x1340
//   bytes of sceNetcnfifData.

namespace ps2x::iop::gt4::net
{
    namespace
    {
        constexpr uint32_t DataBytes = 0x1340u; // sceNetcnfifData [C size, CORE 0x571d60 / PDINETCNF 0x2f4]
        constexpr int32_t NoFile = -3;          // [I] sceNETCNF open error

        std::string field(const uint8_t *data, size_t offset)
        {
            const char *text = reinterpret_cast<const char *>(data + offset);
            return std::string(text, strnlen(text, 0x100));
        }

        class Pdinetcnf final : public Component
        {
        public:
            Pdinetcnf(IopHost &host, Stack &stack, EnvStore &env) : host_(host), stack_(stack), env_(env) {}

            void reset() override
            {
                std::lock_guard lock(mutex_);
                logged_.clear();
            }

            RpcResult handleRpc(const RpcRequest &request) override
            {
                if (request.sid != SidPncf)
                    return rejected();
                std::lock_guard lock(mutex_);
                const size_t size = std::min<size_t>(std::max<size_t>({request.send.size, request.receive.size, 0x20u}),
                                                     0x10000u);
                std::vector<uint8_t> b(size, 0);
                if (request.send.size && !readSend(host_, request, b.data(), std::min<size_t>(request.send.size, size)))
                    return rejected();
                int32_t result = -1;
                switch (request.function)
                {
                case 0:
                    result = 0x101;
                    break;
                case 1: // netcnf#4 GetCount(path @+0x10, type [0])
                    result = offered() ? 1 : 0;
                    trace(request.function, b, "-> " + std::to_string(result));
                    note(request.function, offered() ? "one network configuration offered (GT4 LAN)"
                                                     : "no memory-card network configuration (0 entries, GT4_NET_CONFIG=none)");
                    break;
                case 2: // GetList(EE rows [0], type [1], path @+0x10)
                    result = offered() ? list(rd32(b.data()), rd32(b.data() + 4)) : 0;
                    trace(request.function, b, "-> " + std::to_string(result));
                    break;
                case 3:
                    result = loadSettings(rd32(b.data()), b);
                    break;
                case 4: // LoadEntry(EE data [0], type [1], path @+0x10, name @+0x1c)
                    if (offered())
                    {
                        result = loadEntry(rd32(b.data()), rd32(b.data() + 4));
                        trace(request.function, b, "-> " + std::to_string(result));
                        break;
                    }
                    [[fallthrough]];
                case 5: case 6: case 7: case 8: case 9:
                    result = NoFile;
                    note(request.function, "memory-card network files not provided (" + std::to_string(NoFile) + ")");
                    break;
                default:
                    note(request.function, "unknown function (-1)");
                    break;
                }
                wr32(b.data(), uint32_t(result));
                return reply(host_, request, b.data(), std::min<size_t>(b.size(), request.receive.size));
            }

        private:
            static constexpr const char *kNet = "Combination1";
            static constexpr const char *kIfc = "GT4 LAN";      // the interface setting's name (15 characters at most)
            static constexpr const char *kDev = "SCE/Ethernet (Network Adaptor)"; // the device setting's name (vendor/product)
            static bool offered()
            {
                static const bool on = [] {
                    const char *v = std::getenv("GT4_NET_CONFIG");
                    return !(v && (std::string_view(v) == "none" || std::string_view(v) == "off"));
                }();
                return on;
            }
            static void put(uint8_t *p, size_t n, const std::string &s) { std::memcpy(p, s.data(), std::min(n - 1, s.size())); }

            void trace(uint32_t fn, const std::vector<uint8_t> &b, const std::string &what)
            {
                if (traced_++ >= 64)
                    return;
                const std::string path = b.size() > 0x1c ? field(b.data(), 0x10).substr(0, 11) : std::string();
                const std::string name = b.size() > 0x11c ? field(b.data(), 0x1c).substr(0, 0xff) : std::string();
                host_.log(LogLevel::Info, "[GT4/NET] PNCF fn " + std::to_string(fn) + " word0 " + hex(rd32(b.data())) + " word1 " +
                                              hex(rd32(b.data() + 4)) + " path '" + path + "' name '" + name + "' " + what);
            }

            // fn 2: one row of the asked type written to the EE's buffer.
            int32_t list(uint32_t ee, uint32_t type)
            {
                if (!ee || type > 2)
                    return -1;
                if (type == 0)
                {
                    std::vector<uint8_t> row(0x70, 0);
                    put(row.data() + 0x40, 16, kNet);
                    put(row.data() + 0x50, 16, "ifc000.cnf"); // the interface by its file name (CORE 0x4ee6b8 -> 0x4ee578)
                    put(row.data() + 0x60, 16, "dev000.cnf"); // the device by its file name (CORE 0x4ee6f0)
                    return host_.writeGuest(ee, row.data(), row.size()) ? 1 : -2;
                }
                std::vector<uint8_t> row(0x1d0, 0);
                wr32(row.data(), type == 1 ? 3u : 0u); // the interface's kind (GT4's own builder: 3, Ethernet)
                put(row.data() + 0xc0, 16, type == 1 ? "ifc000.cnf" : "dev000.cnf");
                put(row.data() + 0xd0, 0x100, type == 1 ? kIfc : kDev);
                return host_.writeGuest(ee, row.data(), row.size()) ? 1 : -2;
            }

            // fn 4: the configuration's settings as sceNetcnfifData (GT4's own static builder 0x4ed660 and its
            // Ethernet values 0x4ed3a8: ifc_type 3, dev_type 3, phy_config 1), the address the LAN service uses.
            int32_t loadEntry(uint32_t ee, uint32_t type)
            {
                if (!ee || type > 2)
                    return -1;
                std::vector<uint8_t> data(DataBytes, 0);
                put(data.data() + 0x000, 0x100, kIfc);
                put(data.data() + 0x100, 0x100, kDev);
                put(data.data() + 0x300, 0x100, formatIpv4(stack_.config().address));
                put(data.data() + 0x400, 0x100, formatIpv4(stack_.config().netmask));
                put(data.data() + 0xe00, 0x100, "SCE");
                put(data.data() + 0xf00, 0x100, "Ethernet (Network Adaptor)");
                wr32(data.data() + 0x1300, 3u);          // ifc_type
                wr32(data.data() + 0x1304, 0xffffffffu); // mtu (GT4's static builder: -1)
                wr32(data.data() + 0x1308, 0xffffffffu); // idle timeout
                wr32(data.data() + 0x130c, 3u);          // dev_type
                wr32(data.data() + 0x1310, 1u);          // phy_config (auto)
                data[0x1320] = 0;                        // static (no DHCP)
                return host_.writeGuest(ee, data.data(), data.size()) ? 0 : -2;
            }

            // fn 3: libpdi#41 reads 0x1340 bytes from the EE, 0x1da4 converts them
            // into the static env at 0x2370, reply word 1 = the env (0xb34) [C].
            int32_t loadSettings(uint32_t eeData, std::vector<uint8_t> &b)
            {
                std::vector<uint8_t> data(DataBytes);
                if (!eeData || !host_.readGuest(eeData, data.data(), data.size()))
                {
                    host_.log(LogLevel::Warning, "[GT4/NET] PNCF fn 3: interface data at " + hex(eeData) + " unreadable");
                    return -2;
                }
                // Field offsets checked against GT4's builder (CORE 0x4ee7c8-0x4ee9f0) [C]:
                // attach_ifc 0, attach_dev 0x100, dhcp host 0x200, address 0x300,
                // netmask 0x400, gateway 0x500, dns 0x600/0x700, vendor 0xe00,
                // product 0xf00, ifc_type 0x1300, dev_type 0x130c, phy 0x1310, dhcp 0x1320.
                InterfaceSettings s;
                s.attachIfc = field(data.data(), 0x000);
                s.attachDev = field(data.data(), 0x100);
                s.dhcpHostName = field(data.data(), 0x200);
                s.address = field(data.data(), 0x300);
                s.netmask = field(data.data(), 0x400);
                s.gateway = field(data.data(), 0x500);
                s.dns1 = field(data.data(), 0x600);
                s.dns2 = field(data.data(), 0x700);
                s.vendor = field(data.data(), 0xe00);
                s.product = field(data.data(), 0xf00);
                s.ifcType = int32_t(rd32(data.data() + 0x1300));
                s.mtu = int32_t(rd32(data.data() + 0x1304));
                s.devType = int32_t(rd32(data.data() + 0x130c));
                s.phyConfig = int32_t(rd32(data.data() + 0x1310));
                s.dhcp = data[0x1320];
                const uint32_t env = env_.put(host_, s);
                if (b.size() >= 8)
                    wr32(b.data() + 4, env);
                host_.log(LogLevel::Info, "[GT4/NET] PNCF interface settings from GT4: " + s.describe() + " -> env " +
                                              hex(env) + " (the LAN address in use is " +
                                              formatIpv4(stack_.config().address) + ")");
                return 0;
            }

            void note(uint32_t fn, const std::string &text)
            {
                if (logged_.insert(fn).second)
                    host_.log(LogLevel::Info, "[GT4/NET] PNCF fn " + std::to_string(fn) + ": " + text);
            }

            IopHost &host_;
            Stack &stack_;
            EnvStore &env_;
            std::mutex mutex_;
            std::set<uint32_t> logged_;
            uint32_t traced_ = 0;
        };
    }

    std::unique_ptr<Component> createPdinetcnf(IopHost &host, Stack &stack, EnvStore &env)
    {
        return std::make_unique<Pdinetcnf>(host, stack, env);
    }
}
