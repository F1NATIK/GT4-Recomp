#include "gt4_iop.h"
#include "gt4_dbcman_ports.h"

// DBCMAN 3.16 + DS2U_D 3.16 (analysis/phase4/specs/pads.md): the DBC socket
// manager behind EE libdbc / libpad2 / libvib. RPC 0x80001300 works in place on
// a 0x90-byte buffer; 0x8000131e/0x8000131f/0x8000131c are the send_data
// servers (vibration). Two virtual DualShock 2s (DS2U_D device type 1), one per
// controller port, slot 0 (analysis/phase5b/player2): port 0 is always plugged
// in and fed from the host pad snapshot, as before; port 1 is plugged in while the
// host has a player-2 source (IopHost::readPortPadSnapshot(1)), with DS2U_D's
// attach / detach counts (gt4_dbcman_ports.h; inside the detach window it differs from
// DS2U_D on purpose, as port 0 always has). Without one, port 1 has no device, as before.
// Every VBlank a 0x80-byte packet goes to half (counter & 1) of each linked
// socket's EE buffer, as DS2U_D does through dbcman#8.

namespace ps2x::iop::gt4
{
    namespace
    {
        constexpr uint32_t SidControl = 0x80001300u;
        constexpr uint32_t BufferBytes = 0x90u;
        constexpr uint32_t ModuleVersion = 0x310u; // halfword at 0x2714; EE requires (v >> 4) == 0x31
        constexpr uint32_t CommonBytes = 0x80u;    // socket table at 0x2778 DMA'd to the EE work address
        constexpr unsigned Sockets = 16;
        constexpr uint32_t DeviceType = 1u;        // DS2U_D registration type
        constexpr int32_t ErrNoDevice = -0xc, ErrNoSocket = -0x15, ErrBadSocket = -0x16, ErrNotLinked = -0x17;
        constexpr unsigned Ports = dbcports::Ports;

        struct Socket
        {
            uint32_t state = 0; // 0 free, 1 open, 2 linked
            uint32_t flags = 0, type = 0, port = 0, slot = 0, id = 0;
            uint32_t buf0 = 0, buf1 = 0;
        };

        // One DS2U_D device (dep = port number, our choice: GetDepNumber below).
        struct Port
        {
            dbcports::Presence presence;
            int linked = -1;      // socket, -1 none
            uint32_t counter = 0; // packet counter, from 0 after attach
            uint8_t small = 0, large = 0;
        };

        class Dbcman final : public Component
        {
        public:
            explicit Dbcman(IopHost &host) : host_(host) { ports_[0].presence.reset(true); }

            void reset() override
            {
                std::lock_guard lock(mutex_);
                workAddress_ = 0;
                sockets_ = {};
                for (unsigned p = 0; p < Ports; ++p)
                {
                    ports_[p].presence.reset(p == 0); // port 0 always plugged in; port 1 follows the host
                    ports_[p].linked = -1;
                    ports_[p].counter = 0;
                }
                logged_ = 0;
                for (unsigned p = 0; p < Ports; ++p)
                    host_.setPortPadActuators(p, false, 0);
            }

            RpcResult handleRpc(const RpcRequest &request) override
            {
                std::lock_guard lock(mutex_);
                const uint32_t limit = request.sid == 0x8000131cu ? 0x2090u : BufferBytes;
                std::vector<uint32_t> b(limit / 4, 0);
                const size_t bytes = std::min<size_t>(limit, request.send.size);
                if (bytes && !host_.readGuest(request.send.address, b.data(), bytes))
                    return rejected();
                if (request.sid == 0x8000131eu || request.sid == 0x8000131fu || request.sid == 0x8000131cu)
                {
                    // send_data2/3 {socket, cmd, len, data @0xc}; result at +0x8c / +0x208c.
                    const int32_t r = sendData(b);
                    b[(request.sid == 0x8000131cu ? 0x208cu : 0x8cu) / 4] = static_cast<uint32_t>(r);
                    return reply(host_, request, b.data(), std::min<size_t>(limit, request.receive.size));
                }
                if (request.sid != SidControl)
                    return rejected();
                switch (request.function)
                {
                case 0x80001363u: // GetModVersion
                    b[0] = ModuleVersion;
                    break;
                case 0x80001304u: // SetWorkAddr(b[1])
                    workAddress_ = b[1];
                    publishCommon();
                    b[0] = 1;
                    break;
                case 0x80001301u: // CreateSocket {flags, type, port, slot, id, name @0x14, buf0 @0x28, buf1 @0x2c}
                {
                    int32_t s = ErrNoSocket;
                    for (unsigned i = 0; i < Sockets; ++i)
                    {
                        if (sockets_[i].state != 0)
                            continue;
                        sockets_[i] = {1, b[0], b[1], b[2], b[3], b[4], b[10], b[11]};
                        s = static_cast<int32_t>(i);
                        const int port = tryLink(i);
                        publishCommon();
                        host_.log(LogLevel::Info, "[GT4/DBCMAN] socket " + std::to_string(i) + " port " + std::to_string(b[2]) +
                                                      (port >= 0 ? " linked to the DualShock 2" : " (no device)"));
                        break;
                    }
                    b[9] = static_cast<uint32_t>(s);
                    break;
                }
                case 0x80001302u: // DeleteSocket(b[0])
                {
                    if (b[0] >= Sockets || sockets_[b[0]].state == 0)
                    {
                        b[1] = static_cast<uint32_t>(ErrBadSocket);
                        break;
                    }
                    if (const int p = portOf(b[0]); p >= 0)
                    {
                        ports_[p].linked = -1;
                        host_.setPortPadActuators(static_cast<unsigned>(p), false, 0);
                    }
                    sockets_[b[0]] = {};
                    publishCommon();
                    b[1] = 1;
                    break;
                }
                case 0x80001303u: // GetDepNumber(b[0]): the linked device's number. Here its port; on a PS2 it is
                                  // DBCMAN's device-table index of the DS2U_D context, which DS2U_D picks first-free
                                  // when a pad attaches, so a lone pad in port 2 would be dep 0 there [I]. GT4 (libpad2)
                                  // only tests the result for < 0 (pads.md 3.3) [C], so the value itself does not matter.
                {
                    const int p = portOf(b[0]);
                    b[1] = static_cast<uint32_t>(p >= 0 ? p : ErrNoDevice);
                    break;
                }
                case 0x80001315u: case 0x80001316u:
                    b[1] = static_cast<uint32_t>(b[0] >= Sockets || !sockets_[b[0]].state ? ErrBadSocket
                                                 : portOf(b[0]) >= 0 ? 1 : ErrNotLinked);
                    break;
                case 0x80001317u:
                    b[1] = 0;
                    break;
                case 0x8000131au: // ReceiveData {socket, cmd, len} -> data @0xc, result @0x8c
                    b[0x23] = static_cast<uint32_t>(receiveData(b));
                    break;
                case 0x80001319u:
                    b[0x23] = static_cast<uint32_t>(sendData(b));
                    break;
                case 0x80001305u:
                    break; // no table entry: buffer returned unchanged
                default:
                    if (logged_ < 32)
                    {
                        ++logged_;
                        host_.log(LogLevel::Warning, "[GT4/DBCMAN] unsupported fn " + hex(request.function));
                    }
                    return rejected();
                }
                return reply(host_, request, b.data(), std::min<size_t>(BufferBytes, request.receive.size));
            }

            void onVBlank(bool start, uint64_t tick) override
            {
                if (!start)
                    return;
                std::lock_guard lock(mutex_);
                // Port 2: plugged in while the host has a player-2 source (one host read per
                // VBlank, also while unlinked), attached / detached as DS2U_D does.
                std::array<std::array<uint8_t, 32>, Ports> snap{};
                std::array<bool, Ports> ok{};
                for (unsigned p = 1; p < Ports; ++p)
                {
                    ok[p] = host_.readPortPadSnapshot(p, snap[p]);
                    switch (ports_[p].presence.update(ok[p]))
                    {
                    case dbcports::Presence::Change::Connected: connect(p, tick); break;
                    case dbcports::Presence::Change::Disconnected: disconnect(p, tick); break;
                    case dbcports::Presence::Change::None: break;
                    }
                }
                for (unsigned p = 0; p < Ports; ++p)
                {
                    Port &port = ports_[p];
                    if (port.linked < 0)
                        continue;
                    if (p == 0)
                        ok[0] = host_.readPortPadSnapshot(0, snap[0]);
                    const auto &socket = sockets_[static_cast<unsigned>(port.linked)];
                    // A failed host read: the neutral pad, still with state 1 (buildPacket). Port 0 has always done
                    // this (HEAD's code); port 2 does the same inside its 10-VBlank detach window, so a host dropout
                    // shorter than that reads as an idle pad. A real DS2U_D would write state 3 ("poll failed,
                    // retrying", pads.md 3.4) on each failed poll, which GT4 takes as "clear the pad record" at once.
                    // A deliberate difference: kinder to host dropouts, at most 9 idle frames after a real unplug.
                    const auto packet = dbcports::buildPacket(ok[p] ? &snap[p] : nullptr, port.counter);
                    const uint32_t target = (port.counter & 1u) ? socket.buf1 : socket.buf0;
                    if (target)
                        host_.writeGuest(target, packet.data(), packet.size());
                    port.counter = (port.counter + 1u) & 0x7fffffffu;
                }
            }

        private:
            // Match rule (pads.md 1.4): bit0 type, bit1 port/slot, bit2 id == device word1 (0).
            bool matches(const Socket &s, unsigned port) const
            {
                if ((s.flags & 1u) && s.type != DeviceType)
                    return false;
                if ((s.flags & 2u) && (s.port != port || s.slot != 0))
                    return false;
                if ((s.flags & 4u) && s.id != 0)
                    return false;
                return true;
            }

            // A new socket links to a plugged-in, unlinked device it matches. Returns the port.
            int tryLink(unsigned index)
            {
                for (unsigned p = 0; p < Ports; ++p)
                {
                    if (!ports_[p].presence.present() || ports_[p].linked >= 0 || !matches(sockets_[index], p))
                        continue;
                    sockets_[index].state = 2;
                    ports_[p].linked = static_cast<int>(index);
                    return static_cast<int>(p);
                }
                return -1;
            }

            int portOf(uint32_t socket) const
            {
                if (socket >= Sockets)
                    return -1;
                for (unsigned p = 0; p < Ports; ++p)
                    if (ports_[p].linked == static_cast<int>(socket))
                        return static_cast<int>(p);
                return -1;
            }

            // dbcman#6 Connect: link to the first open socket that matches, Common, packets from counter 0.
            void connect(unsigned p, uint64_t tick)
            {
                Port &port = ports_[p];
                port.counter = 0;
                for (unsigned i = 0; i < Sockets && port.linked < 0; ++i)
                    if (sockets_[i].state == 1 && matches(sockets_[i], p))
                    {
                        sockets_[i].state = 2;
                        port.linked = static_cast<int>(i);
                    }
                if (port.linked >= 0)
                    publishCommon();
                host_.log(LogLevel::Info, "[GT4/DBCMAN] port " + std::to_string(p + 1) + " DualShock 2 connected" +
                                              (port.linked >= 0 ? " (socket " + std::to_string(port.linked) + ")" : std::string(" (no socket yet)")) +
                                              " at vsync " + std::to_string(tick));
            }

            // dbcman#7 Disconnect: Common 0, unlink, no more packets; the motors stop.
            void disconnect(unsigned p, uint64_t tick)
            {
                Port &port = ports_[p];
                if (port.linked >= 0)
                {
                    sockets_[static_cast<unsigned>(port.linked)].state = 1;
                    port.linked = -1;
                    publishCommon();
                }
                port.small = port.large = 0;
                host_.setPortPadActuators(p, false, 0);
                host_.log(LogLevel::Info, "[GT4/DBCMAN] port " + std::to_string(p + 1) + " DualShock 2 disconnected at vsync " +
                                              std::to_string(tick));
            }

            void publishCommon()
            {
                if (!workAddress_)
                    return;
                std::array<uint32_t, CommonBytes / 4> common{};
                for (unsigned i = 0; i < Sockets; ++i)
                    common[i] = portOf(i) >= 0 ? 1u : 0u;
                host_.writeGuest(workAddress_, common.data(), CommonBytes);
            }

            int32_t receiveData(std::vector<uint32_t> &b)
            {
                if (b[0] >= Sockets || !sockets_[b[0]].state)
                    return ErrBadSocket;
                if (portOf(b[0]) < 0)
                    return ErrNotLinked;
                const uint32_t cmd = b[1];
                if ((cmd >> 24) != 1u)
                    return -1;
                const uint32_t cls = (cmd >> 16) & 0xffu, fn = cmd & 0x3fffu;
                if (cls == 1 && fn == 0xc) { b[2] = 1; b[3] = 1; return 1; }          // pad state: stable
                if (cls == 3 && fn == 2) { b[2] = 1; b[3] = 3; return 1; }           // vibration profile: both motors
                if (cls == 1 && fn == 2) { b[2] = 4; b[3] = 0x79; return 1; }        // mode id: DualShock 2
                if (cls == 2 && fn == 2) { b[2] = 4; b[3] = 0xffffffffu; return 1; } // button profile
                return -3;
            }

            int32_t sendData(std::vector<uint32_t> &b)
            {
                if (b[0] >= Sockets || !sockets_[b[0]].state)
                    return ErrBadSocket;
                const int p = portOf(b[0]);
                if (p < 0)
                    return ErrNotLinked;
                const uint32_t cmd = b[1];
                if ((cmd >> 24) != 1u || ((cmd >> 16) & 0xffu) != 3u || (cmd & 0x3fffu) != 0xbu)
                    return -3;
                // payload @0xc: {u32 profSize, u32 n, data[n], profile[profSize]}
                const auto *payload = reinterpret_cast<const uint8_t *>(b.data() + 3);
                uint32_t n = 0;
                std::memcpy(&n, payload + 4, 4);
                if (n > 0x20u)
                    return -1;
                const uint8_t *d = payload + 8;
                const uint8_t profile = d[n];
                Port &port = ports_[p];
                if (profile == 1)
                    port.small = d[0] & 1u;
                else if (profile == 2)
                    port.large = d[0];
                else if (profile == 3)
                {
                    port.small = d[0] & 1u;
                    port.large = static_cast<uint8_t>((d[0] >> 1) | (d[1] << 7));
                }
                else
                    return -1;
                host_.setPortPadActuators(static_cast<unsigned>(p), port.small != 0, port.large);
                return 1;
            }

            IopHost &host_;
            std::mutex mutex_;
            uint32_t workAddress_ = 0;
            std::array<Socket, Sockets> sockets_{};
            std::array<Port, Ports> ports_{};
            uint32_t logged_ = 0;
        };
    }

    std::unique_ptr<Component> createDbcman(IopHost &host)
    {
        return std::make_unique<Dbcman>(host);
    }
}
