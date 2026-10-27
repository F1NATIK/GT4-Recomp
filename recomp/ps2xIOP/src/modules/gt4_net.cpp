#include "gt4_net.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <set>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#else
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

// INET on Winsock for GT4's RTINETB / PDINETCNF services (gt4_net.h).
// Semantics follow the INET calls RTINETB makes (RTINETB handler 0x3f8,
// threads 0x133c-0x1818) and the libnetb wrappers in CORE (0x56ef70-0x56f940):
// [C] where checked in those binaries, [I] where inferred (marked inline).

namespace ps2x::iop::gt4::net
{
    namespace
    {
        using Clock = std::chrono::steady_clock;

#if defined(_WIN32)
        using Handle = SOCKET;
        constexpr Handle NoHandle = INVALID_SOCKET;
        int lastError() { return WSAGetLastError(); }
        void closeHandle(Handle h)
        {
            if (h != NoHandle)
                closesocket(h);
        }
        bool nonBlocking(Handle h)
        {
            u_long one = 1;
            return ioctlsocket(h, FIONBIO, &one) == 0;
        }
        bool wouldBlock(int e) { return e == WSAEWOULDBLOCK || e == WSAEINPROGRESS; }
        int pendingBytes(Handle h)
        {
            u_long n = 0;
            return ioctlsocket(h, FIONREAD, &n) == 0 ? static_cast<int>(n) : 0;
        }
        constexpr int ShutSend = SD_SEND;
        using SockLen = int;
#else
        using Handle = int;
        constexpr Handle NoHandle = -1;
        int lastError() { return errno; }
        void closeHandle(Handle h)
        {
            if (h != NoHandle)
                ::close(h);
        }
        bool nonBlocking(Handle h)
        {
            const int flags = fcntl(h, F_GETFL, 0);
            return flags >= 0 && fcntl(h, F_SETFL, flags | O_NONBLOCK) == 0;
        }
        bool wouldBlock(int e) { return e == EWOULDBLOCK || e == EAGAIN || e == EINPROGRESS; }
        int pendingBytes(Handle h)
        {
            int n = 0;
            return ioctl(h, FIONREAD, &n) == 0 ? n : 0;
        }
        constexpr int ShutSend = SHUT_WR;
        using SockLen = socklen_t;
#endif

        // Winsock / errno -> sceINETE_* (spec 3.7).
        int32_t mapError(int e)
        {
#if defined(_WIN32)
            switch (e)
            {
            case WSAETIMEDOUT: return inet::Timeout;
            case WSAENETDOWN: return inet::LinkDown;
            case WSAENOBUFS:
            case WSAEWOULDBLOCK: return inet::Resources;
            case WSAEADDRINUSE:
            case WSAEISCONN: return inet::Exists;
            case WSAEADDRNOTAVAIL: return inet::LocalSocket;
            case WSAENOTCONN: return inet::NotExist;
            case WSAESHUTDOWN: return inet::Closing;
            case WSAECONNRESET:
            case WSAECONNABORTED:
            case WSAENETRESET: return inet::Reset;
            case WSAECONNREFUSED: return inet::Refused;
            case WSAENETUNREACH:
            case WSAEHOSTUNREACH:
            case WSAEACCES: return inet::NoRoute;
            case WSAEALREADY:
            case WSAEINPROGRESS: return inet::Busy;
            default: return inet::Invalid;
            }
#else
            switch (e)
            {
            case ETIMEDOUT: return inet::Timeout;
            case ENETDOWN: return inet::LinkDown;
            case ENOBUFS:
            case EAGAIN: return inet::Resources;
            case EADDRINUSE:
            case EISCONN: return inet::Exists;
            case EADDRNOTAVAIL: return inet::LocalSocket;
            case ENOTCONN: return inet::NotExist;
            case EPIPE: return inet::Closing;
            case ECONNRESET:
            case ECONNABORTED: return inet::Reset;
            case ECONNREFUSED: return inet::Refused;
            case ENETUNREACH:
            case EHOSTUNREACH:
            case EACCES: return inet::NoRoute;
            case EALREADY:
            case EINPROGRESS: return inet::Busy;
            default: return inet::Invalid;
            }
#endif
        }

        sockaddr_in endpoint(uint32_t address, uint16_t port)
        {
            sockaddr_in a{};
            a.sin_family = AF_INET;
            a.sin_addr.s_addr = htonl(address);
            a.sin_port = htons(port);
            return a;
        }

        bool localEndpoint(Handle h, uint32_t &address, uint16_t &port)
        {
            sockaddr_in a{};
            SockLen length = sizeof(a);
            if (h == NoHandle || getsockname(h, reinterpret_cast<sockaddr *>(&a), &length) != 0)
                return false;
            address = ntohl(a.sin_addr.s_addr);
            port = ntohs(a.sin_port);
            return true;
        }

        std::string trim(std::string s)
        {
            while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back())))
                s.pop_back();
            size_t i = 0;
            while (i < s.size() && std::isspace(static_cast<unsigned char>(s[i])))
                ++i;
            return s.substr(i);
        }

        std::string lower(std::string s)
        {
            for (auto &c : s)
                c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            return s;
        }

        bool parseNumber(const std::string &text, uint32_t &value)
        {
            if (text.empty() || text.size() > 9)
                return false;
            uint32_t v = 0;
            for (char c : text)
            {
                if (c < '0' || c > '9')
                    return false;
                v = v * 10u + uint32_t(c - '0');
            }
            value = v;
            return true;
        }

        std::vector<std::string> splitList(const std::string &text)
        {
            std::vector<std::string> out;
            std::string item;
            for (char c : text)
            {
                if (c == ',' || c == ';' || c == ' ')
                {
                    if (!trim(item).empty())
                        out.push_back(trim(item));
                    item.clear();
                }
                else
                    item.push_back(c);
            }
            if (!trim(item).empty())
                out.push_back(trim(item));
            return out;
        }

        bool contiguousMask(uint32_t mask) { return mask != 0 && ((~mask + 1u) & ~mask) == 0; }

        bool isMulticast(uint32_t a) { return (a >> 28) == 0xeu; }

        // Inbound policy for datagrams and accepted connections (spec 3.4): loop
        // takes its loopback subnet and the listed peers; lan its subnet except
        // the blocked addresses (the router).
        bool inboundAllowed(const Config &c, uint32_t source)
        {
            if (std::find(c.blocked.begin(), c.blocked.end(), source) != c.blocked.end())
                return false;
            if (c.mode == Mode::Loop)
                return isLoopback(source) &&
                       (c.inSubnet(source) || std::find(c.peers.begin(), c.peers.end(), source) != c.peers.end());
            return c.inSubnet(source);
        }

#if defined(_WIN32)
        // lan without GT4_NET_IP: the up interface with a private address that
        // holds a default gateway; refuse when none or more than one qualify.
        // With GT4_NET_IP: that address's prefix and gateways (gateways go to
        // the blocked list: no router services through the policy).
        bool lanAdapter(uint32_t wanted, uint32_t &address, uint32_t &netmask, std::vector<uint32_t> &gateways,
                        std::string &error)
        {
            ULONG size = 32 * 1024;
            std::vector<uint8_t> buffer;
            ULONG status = ERROR_BUFFER_OVERFLOW;
            for (int attempt = 0; attempt < 3 && status == ERROR_BUFFER_OVERFLOW; ++attempt)
            {
                buffer.resize(size);
                status = GetAdaptersAddresses(AF_INET,
                                              GAA_FLAG_INCLUDE_GATEWAYS | GAA_FLAG_SKIP_ANYCAST |
                                                  GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER,
                                              nullptr, reinterpret_cast<IP_ADAPTER_ADDRESSES *>(buffer.data()), &size);
            }
            if (status != NO_ERROR)
            {
                error = "GetAdaptersAddresses failed (" + std::to_string(status) + ")";
                return false;
            }
            struct Candidate
            {
                uint32_t address, netmask;
                std::vector<uint32_t> gateways;
            };
            std::vector<Candidate> candidates;
            for (auto *a = reinterpret_cast<IP_ADAPTER_ADDRESSES *>(buffer.data()); a; a = a->Next)
            {
                if (a->OperStatus != IfOperStatusUp || a->IfType == IF_TYPE_SOFTWARE_LOOPBACK)
                    continue;
                std::vector<uint32_t> gw;
                for (auto *g = a->FirstGatewayAddress; g; g = g->Next)
                    if (g->Address.lpSockaddr && g->Address.lpSockaddr->sa_family == AF_INET)
                        gw.push_back(ntohl(reinterpret_cast<sockaddr_in *>(g->Address.lpSockaddr)->sin_addr.s_addr));
                for (auto *u = a->FirstUnicastAddress; u; u = u->Next)
                {
                    if (!u->Address.lpSockaddr || u->Address.lpSockaddr->sa_family != AF_INET)
                        continue;
                    const uint32_t ip = ntohl(reinterpret_cast<sockaddr_in *>(u->Address.lpSockaddr)->sin_addr.s_addr);
                    const uint32_t bits = std::min<uint32_t>(u->OnLinkPrefixLength, 32u);
                    const uint32_t mask = bits ? ~uint32_t(0) << (32u - bits) : 0u;
                    if (wanted ? ip == wanted : isPrivate(ip))
                        candidates.push_back({ip, mask, gw});
                }
            }
            if (wanted)
            {
                if (candidates.empty())
                {
                    error = "GT4_NET_IP " + formatIpv4(wanted) + " is not an address of an up interface";
                    return false;
                }
                address = wanted;
                netmask = candidates.front().netmask;
                gateways = candidates.front().gateways;
                return true;
            }
            std::vector<Candidate> routed;
            for (const auto &c : candidates)
                if (!c.gateways.empty())
                    routed.push_back(c);
            const auto &pool = routed.empty() ? candidates : routed;
            if (pool.size() != 1)
            {
                error = pool.empty() ? "no up interface with a private IPv4 address"
                                     : "several private interfaces; set GT4_NET_IP";
                return false;
            }
            address = pool.front().address;
            netmask = pool.front().netmask;
            gateways = pool.front().gateways;
            return true;
        }
#endif
    }

    bool parseIpv4(std::string_view text, uint32_t &address)
    {
        uint32_t parts[4]{};
        size_t part = 0, digits = 0;
        uint32_t value = 0;
        for (char c : text)
        {
            if (c >= '0' && c <= '9')
            {
                value = value * 10u + uint32_t(c - '0');
                if (++digits > 3 || value > 255u)
                    return false;
            }
            else if (c == '.' && digits && part < 3)
            {
                parts[part++] = value;
                value = 0;
                digits = 0;
            }
            else
                return false;
        }
        if (part != 3 || !digits)
            return false;
        parts[3] = value;
        address = parts[0] << 24 | parts[1] << 16 | parts[2] << 8 | parts[3];
        return true;
    }

    std::string formatIpv4(uint32_t a)
    {
        char text[20];
        std::snprintf(text, sizeof(text), "%u.%u.%u.%u", a >> 24, (a >> 16) & 255u, (a >> 8) & 255u, a & 255u);
        return text;
    }

    bool isPrivate(uint32_t a)
    {
        return (a >> 24) == 10u || (a >> 20) == 0xac1u || (a >> 16) == 0xc0a8u || (a >> 16) == 0xa9feu;
    }

    std::string Config::describe() const
    {
        if (!enabled())
            return "off";
        std::string text = std::string(mode == Mode::Loop ? "loop" : "lan") + " " + formatIpv4(address) + "/" +
                           formatIpv4(netmask) + " broadcast " + formatIpv4(broadcast());
        if (!peers.empty())
        {
            text += " peers";
            for (uint32_t p : peers)
                text += " " + formatIpv4(p);
        }
        if (!blocked.empty())
        {
            text += " blocked";
            for (uint32_t b : blocked)
                text += " " + formatIpv4(b);
        }
        if (sendDelayMs)
            text += " send_delay " + std::to_string(sendDelayMs) + " ms";
        return text;
    }

    Config configFrom(const Lookup &lookup, std::string &error)
    {
        auto get = [&](const char *name) {
            const char *v = lookup ? lookup(name) : nullptr;
            return v ? trim(v) : std::string();
        };
        Config c;
        const std::string mode = lower(get("GT4_NET"));
        if (mode.empty() || mode == "off" || mode == "0")
            return c;
        if (mode == "loop")
            c.mode = Mode::Loop;
        else if (mode == "lan")
            c.mode = Mode::Lan;
        else
        {
            error = "GT4_NET='" + mode + "' (expected off, loop or lan)";
            return Config{};
        }
        auto fail = [&](std::string why) {
            error = std::move(why);
            return Config{};
        };
        uint32_t value = 0;
        if (!get("GT4_NET_LOG").empty() && parseNumber(get("GT4_NET_LOG"), value))
            c.log = std::min<uint32_t>(value, 2u);
        if (!get("GT4_NET_SEND_DELAY_MS").empty() && parseNumber(get("GT4_NET_SEND_DELAY_MS"), value))
            c.sendDelayMs = std::min<uint32_t>(value, 100u);
        uint32_t index = 0;
        if (!get("GT4_NET_INDEX").empty())
        {
            if (!parseNumber(get("GT4_NET_INDEX"), index) || index < 1 || index > 16)
                return fail("GT4_NET_INDEX must be 1..16");
        }
        uint32_t ip = 0, mask = 0;
        const std::string ipText = get("GT4_NET_IP"), maskText = get("GT4_NET_MASK");
        if (!ipText.empty() && !parseIpv4(ipText, ip))
            return fail("GT4_NET_IP '" + ipText + "' is not an IPv4 address");
        if (!maskText.empty() && (!parseIpv4(maskText, mask) || !contiguousMask(mask)))
            return fail("GT4_NET_MASK '" + maskText + "' is not a netmask");
        for (const auto &item : splitList(get("GT4_NET_BLOCK")))
        {
            uint32_t b = 0;
            if (!parseIpv4(item, b))
                return fail("GT4_NET_BLOCK entry '" + item + "' is not an IPv4 address");
            c.blocked.push_back(b);
        }

        if (c.mode == Mode::Loop)
        {
            // Loopback only: binding 127.x never involves the firewall [I].
            c.address = ip ? ip : index ? (0x7f4d0000u | (10u + index)) : 0x7f000001u; // 127.77.0.(10+n) / 127.0.0.1
            if (!isLoopback(c.address))
                return fail("loop mode needs a 127.x GT4_NET_IP (got " + formatIpv4(c.address) + ")");
            c.netmask = mask ? mask : 0xffffff00u;
            const std::string peers = get("GT4_NET_PEERS");
            uint32_t count = 0;
            if (peers.empty())
            {
                if ((c.address & 0xffffff00u) == 0x7f4d0000u)
                    count = 6;
            }
            else if (parseNumber(peers, count))
            {
                if (count < 1 || count > 16)
                    return fail("GT4_NET_PEERS count must be 1..16");
            }
            else
            {
                for (const auto &item : splitList(peers))
                {
                    uint32_t p = 0;
                    if (!parseIpv4(item, p) || !isLoopback(p))
                        return fail("GT4_NET_PEERS entry '" + item + "' is not a 127.x address");
                    c.peers.push_back(p);
                }
            }
            for (uint32_t n = 1; n <= count; ++n)
                c.peers.push_back(0x7f4d0000u | (10u + n));
        }
        else
        {
            uint32_t address = ip, netmask = mask ? mask : 0xffffff00u;
#if defined(_WIN32)
            std::vector<uint32_t> gateways;
            std::string why;
            uint32_t found = 0, foundMask = 0;
            // The address must belong to an up interface of this PC (a bind to
            // anything else fails later anyway); its gateways are refused.
            if (!lanAdapter(ip, found, foundMask, gateways, why))
                return fail("lan: " + why);
            address = found;
            if (!mask && foundMask)
                netmask = foundMask;
            for (uint32_t g : gateways)
                c.blocked.push_back(g);
#endif
            if (!address)
                return fail("lan mode needs GT4_NET_IP");
            if (isLoopback(address) || !isPrivate(address))
                return fail("lan mode needs a private (RFC 1918 / link-local) address, got " + formatIpv4(address));
            c.address = address;
            c.netmask = netmask;
        }
        std::sort(c.peers.begin(), c.peers.end());
        c.peers.erase(std::unique(c.peers.begin(), c.peers.end()), c.peers.end());
        c.peers.erase(std::remove(c.peers.begin(), c.peers.end(), c.address), c.peers.end());
        c.mac[5] = static_cast<uint8_t>(index ? index : (c.address & 0xffu));
        return c;
    }

    Config configFromEnvironment(std::string &error)
    {
        return configFrom([](const char *name) -> const char * { return std::getenv(name); }, error);
    }

    std::string InterfaceSettings::describe() const
    {
        std::string text = dhcp == 1 ? "DHCP" : dhcp == 0 ? "static" : "dhcp unset";
        if (!address.empty())
            text += " address " + address;
        if (!netmask.empty())
            text += " netmask " + netmask;
        if (!gateway.empty())
            text += " gateway " + gateway;
        if (!dns1.empty())
            text += " dns " + dns1;
        if (!vendor.empty() || !product.empty())
            text += " device '" + vendor + "' '" + product + "'";
        text += " ifc_type " + std::to_string(ifcType) + " phy " + std::to_string(phyConfig);
        return text;
    }

    uint32_t EnvStore::put(IopHost &host, const InterfaceSettings &settings)
    {
        std::lock_guard lock(mutex_);
        if (!address_)
        {
            address_ = host.allocateIopMemory(0x40);
            synthetic_ = address_ == 0;
            if (synthetic_)
                address_ = 0x0007f000u; // [I] a plausible IOP RAM address (no native heap on this host)
        }
        settings_ = settings;
        valid_ = true;
        return address_;
    }

    bool EnvStore::get(uint32_t env, InterfaceSettings &settings) const
    {
        std::lock_guard lock(mutex_);
        if (!valid_ || env != address_)
            return false;
        settings = settings_;
        return true;
    }

    void EnvStore::reset(IopHost &host)
    {
        std::lock_guard lock(mutex_);
        if (address_ && !synthetic_)
            host.freeIopMemory(address_);
        address_ = 0;
        synthetic_ = false;
        valid_ = false;
        settings_ = {};
    }

    // ---------------------------------------------------------------- Stack

    struct Stack::Socket
    {
        int32_t cid = 0;
        SocketType type = SocketType::Udp;
        Handle handle = NoHandle;   // UDP socket / TCP connection
        Handle listener = NoHandle; // TCP passive until a connection is accepted
        uint32_t localAddress = 0;
        uint16_t localPort = 0;
        uint32_t remoteAddress = 0;
        uint16_t remotePort = 0;
        std::mutex mutex; // connect / accept / state transitions
        std::atomic<uint32_t> aborts{0};
        std::atomic<bool> closed{false};
        bool opened = false;
        bool listening = false;
        bool connecting = false;
        bool connected = false;
        bool eof = false;
        bool finReported = false;
        bool noDelay = false;
        int32_t state = 2; // sceINETI_STATE_* (RTINETB 0x2378 strings, table 0x5528) [C]

        ~Socket()
        {
            closeHandle(handle);
            closeHandle(listener);
        }

        bool udpLike() const { return type == SocketType::Udp || type == SocketType::Raw; }
        Handle waitHandle()
        {
            std::lock_guard lock(mutex);
            return type == SocketType::TcpPassive && !connected ? listener : handle;
        }
    };

    Stack::Stack(IopHost &host, const Config &config) : host_(host), config_(config)
    {
#if defined(_WIN32)
        WSADATA data{};
        wsa_ = WSAStartup(MAKEWORD(2, 2), &data) == 0;
#else
        wsa_ = true;
#endif
        ready_ = wsa_ && config_.enabled() && (config_.mode != Mode::Loop || isLoopback(config_.address));
        if (!ready_)
            host_.log(LogLevel::Error, "[GT4/NET] network stack not available (" + config_.describe() + ")");
    }

    Stack::~Stack()
    {
        stopping_ = true;
        shutdown();
#if defined(_WIN32)
        if (wsa_)
            WSACleanup();
#endif
    }

    void Stack::logLine(uint32_t level, const std::string &text) const
    {
        if (config_.log >= level)
            host_.log(LogLevel::Info, "[GT4/NET] " + text);
    }

    std::shared_ptr<Stack::Socket> Stack::find(int32_t cid) const
    {
        std::lock_guard lock(mutex_);
        const auto it = sockets_.find(cid);
        return it == sockets_.end() ? nullptr : it->second;
    }

    bool Stack::known(int32_t cid) const { return find(cid) != nullptr; }

    bool Stack::isUdpLike(int32_t cid) const
    {
        const auto s = find(cid);
        return s && s->udpLike();
    }

    void Stack::refuse(uint32_t address, const char *why)
    {
        ++refusals_;
        bool first = false;
        {
            std::lock_guard lock(mutex_);
            first = refusedLogged_.insert(address).second;
        }
        if (first)
            host_.log(LogLevel::Info, "[GT4/NET] refused " + formatIpv4(address) + ": " + why +
                                          " (LAN only, sceINETE_NO_ROUTE)");
    }

    // LAN-only destination policy (spec 3.4). Fills the host addresses to send to.
    int32_t Stack::route(uint32_t address, int32_t port, bool datagram, std::vector<uint32_t> &targets)
    {
        targets.clear();
        if (port <= 0 || port > 65535 || address == 0)
            return inet::ForeignSocket;
        if (isMulticast(address))
        {
            refuse(address, "multicast");
            return inet::NoRoute;
        }
        if (address == 0xffffffffu || address == config_.broadcast())
        {
            if (!datagram)
            {
                refuse(address, "broadcast stream");
                return inet::NoRoute;
            }
            if (config_.mode == Mode::Loop)
                targets = config_.peers; // loopback has no broadcast: unicast copies [I]
            else
                targets.push_back(config_.broadcast());
            return 0;
        }
        if (std::find(config_.blocked.begin(), config_.blocked.end(), address) != config_.blocked.end())
        {
            refuse(address, "blocked (router)");
            return inet::NoRoute;
        }
        const bool peer = std::find(config_.peers.begin(), config_.peers.end(), address) != config_.peers.end();
        const bool allowed = config_.mode == Mode::Loop ? isLoopback(address) && (config_.inSubnet(address) || peer)
                                                        : config_.inSubnet(address);
        if (!allowed)
        {
            refuse(address, "outside the LAN");
            return inet::NoRoute;
        }
        targets.push_back(address);
        return 0;
    }

    int32_t Stack::create(int32_t type, int32_t localPort, uint32_t remoteAddress, int32_t remotePort)
    {
        if (!ready_)
            return inet::LinkDown;
        if (type != int32_t(SocketType::Udp) && type != int32_t(SocketType::TcpActive) &&
            type != int32_t(SocketType::TcpPassive))
        {
            host_.log(LogLevel::Info, "[GT4/NET] socket type " + std::to_string(type) +
                                          (type == int32_t(SocketType::Raw) ? " (raw IP)" : "") +
                                          " not provided (sceINETE_INVALID_ARGUMENT)");
            return inet::Invalid;
        }
        // Every bind is to the configured address, never INADDR_ANY (spec 3.4);
        // loop mode binds 127.x only.
        if (!config_.address || (config_.mode == Mode::Loop && !isLoopback(config_.address)))
            return inet::LocalSocket;
        auto s = std::make_shared<Socket>();
        s->type = SocketType(type);
        s->remoteAddress = remoteAddress;
        s->remotePort = static_cast<uint16_t>(remotePort > 0 && remotePort <= 65535 ? remotePort : 0);
        const uint16_t port = static_cast<uint16_t>(localPort > 0 && localPort <= 65535 ? localPort : 0);
        const bool udp = s->type == SocketType::Udp;
        Handle h = ::socket(AF_INET, udp ? SOCK_DGRAM : SOCK_STREAM, udp ? IPPROTO_UDP : IPPROTO_TCP);
        if (h == NoHandle)
            return mapError(lastError());
        int one = 1;
#if defined(_WIN32)
        setsockopt(h, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, reinterpret_cast<const char *>(&one), sizeof(one));
#endif
        if (udp)
        {
            setsockopt(h, SOL_SOCKET, SO_BROADCAST, reinterpret_cast<const char *>(&one), sizeof(one));
            int buffer = 256 * 1024; // holds datagrams while a receive ring is full (INET's queue) [I size]
            setsockopt(h, SOL_SOCKET, SO_RCVBUF, reinterpret_cast<const char *>(&buffer), sizeof(buffer));
#if defined(_WIN32)
#ifndef SIO_UDP_CONNRESET
#define SIO_UDP_CONNRESET _WSAIOW(IOC_VENDOR, 12)
#endif
            BOOL report = FALSE;
            DWORD returned = 0;
            WSAIoctl(h, SIO_UDP_CONNRESET, &report, sizeof(report), nullptr, 0, &returned, nullptr, nullptr);
#endif
        }
        const sockaddr_in local = endpoint(config_.address, port);
        if (!nonBlocking(h) || ::bind(h, reinterpret_cast<const sockaddr *>(&local), sizeof(local)) != 0)
        {
            const int32_t error = mapError(lastError());
            closeHandle(h);
            logLine(1, "create type " + std::to_string(type) + " port " + std::to_string(port) + " failed " +
                           std::to_string(error));
            return error;
        }
        localEndpoint(h, s->localAddress, s->localPort);
        if (s->type == SocketType::TcpPassive)
        {
            s->listener = h;
            s->state = 1; // CLOSED until opened (LISTEN)
        }
        else
        {
            s->handle = h;
            s->state = udp ? 2 : 1; // UDP CREATED; TCP CLOSED until opened
        }
        {
            std::lock_guard lock(mutex_);
            // INET cids are IOP pointers in the real module (inet#15 checks the
            // 'Inet' magic) [C]; any positive non-zero value works for the EE
            // (0x56ef70 treats 0 as failure) [C].
            s->cid = 0x00080000 + nextCid_++ * 0x100;
            sockets_[s->cid] = s;
        }
        static const char *names[] = {"UDP", "TCP connect", "TCP listen"};
        logLine(1, "create cid " + hex(uint32_t(s->cid)) + " " + names[type] + " " + formatIpv4(s->localAddress) +
                       ":" + std::to_string(s->localPort) +
                       (s->remotePort ? " -> " + formatIpv4(remoteAddress) + ":" + std::to_string(s->remotePort) : ""));
        return s->cid;
    }

    int32_t Stack::waitReady(Socket &socket, bool write, int32_t ms, uint32_t abortSeen)
    {
        const auto start = Clock::now();
        for (;;)
        {
            if (stopping_ || socket.closed || socket.aborts.load() != abortSeen)
                return inet::Abort;
            const Handle h = socket.waitHandle();
            if (h == NoHandle)
                return inet::NotExist;
            int32_t slice = 20;
            if (ms >= 0)
            {
                const auto spent = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - start).count();
                slice = static_cast<int32_t>(std::clamp<long long>(ms - spent, 0, 20));
            }
            fd_set readable, writable, failed;
            FD_ZERO(&readable);
            FD_ZERO(&writable);
            FD_ZERO(&failed);
            FD_SET(h, write ? &writable : &readable);
            FD_SET(h, &failed);
            timeval tv{0, slice * 1000};
            const int n = ::select(static_cast<int>(h) + 1, &readable, &writable, &failed, &tv);
            if (n > 0)
                return 0;
            if (n < 0)
            {
                // The handle changed under us (accept, close): look again.
                if (socket.closed || socket.waitHandle() != h)
                    continue;
                return mapError(lastError());
            }
            if (ms >= 0 &&
                std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - start).count() >= ms)
                return inet::Timeout;
        }
    }

    int32_t Stack::finishConnect(Socket &s)
    {
        int error = 0;
        SockLen length = sizeof(error);
        getsockopt(s.handle, SOL_SOCKET, SO_ERROR, reinterpret_cast<char *>(&error), &length);
        s.connecting = false;
        if (error)
        {
            s.state = 1;
            return mapError(error);
        }
        s.connected = true;
        s.state = 7; // ESTABLISHED
        logLine(1, "cid " + hex(uint32_t(s.cid)) + " connected to " + formatIpv4(s.remoteAddress) + ":" +
                       std::to_string(s.remotePort));
        return 0;
    }

    int32_t Stack::tryAccept(Socket &s)
    {
        std::lock_guard lock(s.mutex);
        if (s.connected)
            return 0;
        if (!s.listening || s.listener == NoHandle)
            return inet::NotExist;
        for (;;)
        {
            sockaddr_in from{};
            SockLen length = sizeof(from);
            const Handle h = ::accept(s.listener, reinterpret_cast<sockaddr *>(&from), &length);
            if (h == NoHandle)
            {
                const int e = lastError();
                return wouldBlock(e) ? inet::Timeout : mapError(e);
            }
            const uint32_t peer = ntohl(from.sin_addr.s_addr);
            if (!inboundAllowed(config_, peer)) // the same rule as datagrams (recvFrom)
            {
                closeHandle(h);
                ++inboundDropped_;
                refuse(peer, "inbound connection from outside the LAN");
                continue;
            }
            nonBlocking(h);
            if (s.noDelay)
            {
                int one = 1;
                setsockopt(h, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char *>(&one), sizeof(one));
            }
            // INET's passive open owns one connection [I]: the cid becomes it and
            // the listening socket is released (a new listener may reuse the port).
            s.handle = h;
            s.remoteAddress = peer;
            s.remotePort = ntohs(from.sin_port);
            s.connected = true;
            s.state = 7;
            closeHandle(s.listener);
            s.listener = NoHandle;
            logLine(1, "cid " + hex(uint32_t(s.cid)) + " accepted " + formatIpv4(peer) + ":" +
                           std::to_string(s.remotePort));
            return 0;
        }
    }

    int32_t Stack::ensureConnection(Socket &s, int32_t ms, uint32_t abortSeen)
    {
        if (s.udpLike())
            return 0;
        const auto start = Clock::now();
        auto remaining = [&]() -> int32_t {
            if (ms < 0)
                return -1;
            const auto spent = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - start).count();
            return static_cast<int32_t>(std::max<long long>(0, ms - spent));
        };
        if (s.type == SocketType::TcpActive)
        {
            {
                std::lock_guard lock(s.mutex);
                if (s.connected)
                    return 0;
                if (!s.connecting)
                    return inet::NotExist;
            }
            const int32_t r = waitReady(s, true, ms, abortSeen);
            if (r < 0)
                return r;
            std::lock_guard lock(s.mutex);
            return s.connected ? 0 : finishConnect(s);
        }
        for (;;)
        {
            const int32_t a = tryAccept(s);
            if (a != inet::Timeout)
                return a;
            const int32_t left = remaining();
            if (left == 0)
                return inet::Timeout;
            const int32_t r = waitReady(s, false, left, abortSeen);
            if (r < 0)
                return r;
        }
    }

    int32_t Stack::open(int32_t cid, int32_t ms)
    {
        const auto s = find(cid);
        if (!s)
            return inet::Invalid;
        const uint32_t abortSeen = s->aborts.load();
        if (s->udpLike())
        {
            std::lock_guard lock(s->mutex);
            s->opened = true;
            s->state = 3; // OPENED
            return 0;
        }
        if (s->type == SocketType::TcpActive)
        {
            std::vector<uint32_t> targets;
            const int32_t routed = route(s->remoteAddress, s->remotePort, false, targets);
            if (routed < 0)
                return routed;
            {
                std::lock_guard lock(s->mutex);
                if (s->connected)
                    return inet::Exists;
                if (!s->connecting)
                {
                    const sockaddr_in to = endpoint(targets.front(), s->remotePort);
                    if (::connect(s->handle, reinterpret_cast<const sockaddr *>(&to), sizeof(to)) == 0)
                    {
                        s->connected = true;
                        s->state = 7;
                        return 0;
                    }
                    const int e = lastError();
                    if (!wouldBlock(e))
                        return mapError(e);
                    s->connecting = true;
                    s->state = 5; // SYN_SENT
                }
            }
            return ensureConnection(*s, ms, abortSeen);
        }
        {
            std::lock_guard lock(s->mutex);
            if (!s->listening)
            {
                if (::listen(s->listener, 1) != 0)
                    return mapError(lastError());
                s->listening = true;
                s->state = 4; // LISTEN
                logLine(1, "cid " + hex(uint32_t(cid)) + " listening on " + formatIpv4(s->localAddress) + ":" +
                               std::to_string(s->localPort));
            }
        }
        // GT4 opens listeners with ms 0 and takes the -500 as "listening"
        // (CORE 0x570358-0x5703ac) [C]; the connection is accepted later.
        return ensureConnection(*s, ms, abortSeen);
    }

    int32_t Stack::close(int32_t cid, int32_t)
    {
        std::shared_ptr<Socket> s;
        {
            std::lock_guard lock(mutex_);
            const auto it = sockets_.find(cid);
            if (it == sockets_.end())
                return inet::Invalid;
            s = it->second;
            sockets_.erase(it);
        }
        {
            std::lock_guard lock(s->mutex);
            if (s->connected && s->handle != NoHandle)
                ::shutdown(s->handle, ShutSend);
            s->state = 1;
        }
        s->closed = true;
        ++s->aborts;
        logLine(1, "close cid " + hex(uint32_t(cid)));
        return 0; // handles close when the last waiter lets go
    }

    int32_t Stack::recvFrom(int32_t cid, uint8_t *data, int32_t count, int32_t &flags, uint32_t &address,
                            int32_t &port, int32_t ms)
    {
        const auto s = find(cid);
        if (!s)
            return inet::Invalid;
        if (count < 0 || (count && !data))
            return inet::Invalid;
        const uint32_t abortSeen = s->aborts.load();
        if (!s->udpLike())
        {
            const int32_t n = recv(cid, data, count, flags, ms);
            if (n >= 0)
            {
                address = s->remoteAddress;
                port = s->remotePort;
            }
            return n;
        }
        const auto start = Clock::now();
        for (;;)
        {
            int32_t left = -1;
            if (ms >= 0)
            {
                const auto spent = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - start).count();
                left = static_cast<int32_t>(std::max<long long>(0, ms - spent));
            }
            const int32_t r = waitReady(*s, false, left, abortSeen);
            if (r < 0)
                return r;
            sockaddr_in from{};
            SockLen length = sizeof(from);
            int n = ::recvfrom(s->handle, reinterpret_cast<char *>(data), count, 0, reinterpret_cast<sockaddr *>(&from),
                               &length);
            if (n < 0)
            {
                const int e = lastError();
                bool truncated = false, retry = wouldBlock(e);
#if defined(_WIN32)
                truncated = e == WSAEMSGSIZE; // the caller's buffer holds the head [I INET behaviour]
                retry = retry || e == WSAECONNRESET;
#endif
                if (truncated)
                    n = count;
                else if (retry)
                {
                    if (left == 0)
                        return inet::Timeout;
                    continue;
                }
                else
                    return mapError(e);
            }
            const uint32_t source = ntohl(from.sin_addr.s_addr);
            if (!inboundAllowed(config_, source))
            {
                ++inboundDropped_;
                refuse(source, "inbound datagram from outside the LAN");
                continue;
            }
            address = source;
            port = ntohs(from.sin_port);
            flags = 0;
            ++datagramsIn_;
            bytesIn_ += uint64_t(n);
            logLine(2, "cid " + hex(uint32_t(cid)) + " <- " + formatIpv4(source) + ":" + std::to_string(port) + " " +
                           std::to_string(n) + " bytes");
            return n;
        }
    }

    int32_t Stack::recv(int32_t cid, uint8_t *data, int32_t count, int32_t &flags, int32_t ms)
    {
        const auto s = find(cid);
        if (!s)
            return inet::Invalid;
        if (count < 0 || (count && !data))
            return inet::Invalid;
        if (s->udpLike())
        {
            uint32_t address = 0;
            int32_t port = 0;
            return recvFrom(cid, data, count, flags, address, port, ms);
        }
        const uint32_t abortSeen = s->aborts.load();
        const auto start = Clock::now();
        int32_t r = ensureConnection(*s, ms, abortSeen);
        if (r < 0)
            return r;
        {
            std::lock_guard lock(s->mutex);
            if (s->eof)
            {
                if (s->finReported)
                    return inet::Closing; // FIN already delivered [I]
                s->finReported = true;
                flags = inet::FlagFin;
                return 0;
            }
        }
        for (;;)
        {
            int32_t left = -1;
            if (ms >= 0)
            {
                const auto spent = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - start).count();
                left = static_cast<int32_t>(std::max<long long>(0, ms - spent));
            }
            r = waitReady(*s, false, left, abortSeen);
            if (r < 0)
                return r;
            const int n = ::recv(s->handle, reinterpret_cast<char *>(data), count, 0);
            if (n > 0)
            {
                flags = 0;
                // A FIN already queued behind the data travels with it, as the
                // RTINETB TCP thread expects (data, then flag 4 -> exit) [I].
                fd_set readable;
                FD_ZERO(&readable);
                FD_SET(s->handle, &readable);
                timeval tv{0, 0};
                char probe = 0;
                if (::select(static_cast<int>(s->handle) + 1, &readable, nullptr, nullptr, &tv) > 0 &&
                    ::recv(s->handle, &probe, 1, MSG_PEEK) == 0)
                {
                    std::lock_guard lock(s->mutex);
                    s->eof = true;
                    s->finReported = true;
                    s->state = 10; // CLOSE_WAIT
                    flags |= inet::FlagFin;
                }
                bytesIn_ += uint64_t(n);
                logLine(2, "cid " + hex(uint32_t(cid)) + " <- " + std::to_string(n) + " bytes" +
                               (flags & inet::FlagFin ? " + FIN" : ""));
                return n;
            }
            if (n == 0)
            {
                std::lock_guard lock(s->mutex);
                s->eof = true;
                s->state = 10;
                if (s->finReported)
                    return inet::Closing;
                s->finReported = true;
                flags = inet::FlagFin;
                logLine(1, "cid " + hex(uint32_t(cid)) + " closed by the peer");
                return 0;
            }
            const int e = lastError();
            if (wouldBlock(e))
            {
                if (left == 0)
                    return inet::Timeout;
                continue;
            }
            return mapError(e);
        }
    }

    int32_t Stack::sendTo(int32_t cid, const uint8_t *data, int32_t count, int32_t &flags, uint32_t address,
                          int32_t port, int32_t ms)
    {
        const auto s = find(cid);
        if (!s)
            return inet::Invalid;
        if (count < 0 || (count && !data))
            return inet::Invalid;
        if (!s->udpLike())
            return send(cid, data, count, flags, ms);
        std::vector<uint32_t> targets;
        const int32_t routed = route(address, port, true, targets);
        if (routed < 0)
            return routed;
        for (uint32_t target : targets)
        {
            const sockaddr_in to = endpoint(target, static_cast<uint16_t>(port));
            const int n = ::sendto(s->handle, reinterpret_cast<const char *>(data), count, 0,
                                   reinterpret_cast<const sockaddr *>(&to), sizeof(to));
            if (n < 0)
            {
                const int e = lastError();
                return wouldBlock(e) ? inet::Resources : mapError(e);
            }
        }
        flags = 0;
        ++datagramsOut_;
        bytesOut_ += uint64_t(count);
        logLine(2, "cid " + hex(uint32_t(cid)) + " -> " + formatIpv4(address) + ":" + std::to_string(port) + " " +
                       std::to_string(count) + " bytes" +
                       (targets.size() != 1 ? " (" + std::to_string(targets.size()) + " copies)" : ""));
        return count;
    }

    int32_t Stack::send(int32_t cid, const uint8_t *data, int32_t count, int32_t &flags, int32_t ms)
    {
        const auto s = find(cid);
        if (!s)
            return inet::Invalid;
        if (count < 0 || (count && !data))
            return inet::Invalid;
        if (s->udpLike())
        {
            if (!s->remotePort)
                return inet::ForeignSocket;
            return sendTo(cid, data, count, flags, s->remoteAddress, s->remotePort, ms);
        }
        const uint32_t abortSeen = s->aborts.load();
        const auto start = Clock::now();
        int32_t r = ensureConnection(*s, ms, abortSeen);
        if (r < 0)
            return r;
        int32_t sent = 0;
        while (sent < count)
        {
            const int n = ::send(s->handle, reinterpret_cast<const char *>(data + sent), count - sent, 0);
            if (n > 0)
            {
                sent += n;
                continue;
            }
            const int e = lastError();
            if (!wouldBlock(e))
                return sent ? sent : mapError(e);
            int32_t left = -1;
            if (ms >= 0)
            {
                const auto spent = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - start).count();
                left = static_cast<int32_t>(std::max<long long>(0, ms - spent));
            }
            r = waitReady(*s, true, left, abortSeen);
            if (r < 0)
                return sent ? sent : r;
        }
        flags = 0;
        bytesOut_ += uint64_t(sent);
        logLine(2, "cid " + hex(uint32_t(cid)) + " -> " + std::to_string(sent) + " bytes");
        return sent;
    }

    int32_t Stack::abort(int32_t cid, int32_t)
    {
        const auto s = find(cid);
        if (!s)
            return inet::Invalid;
        ++s->aborts;
        logLine(1, "abort cid " + hex(uint32_t(cid)));
        return 0;
    }

    int32_t Stack::control(int32_t cid, int32_t code, uint8_t *data, int32_t length)
    {
        const auto s = cid ? find(cid) : nullptr;
        if (!s)
        {
            logLine(1, "control cid " + hex(uint32_t(cid)) + " code " + std::to_string(code) + " not provided");
            return inet::Invalid;
        }
        if (code == 1)
        {
            // sceInetInfo (0x4c): cid, proto (1 TCP, 2 UDP, 3 raw), recv / send
            // queue bytes, local address +0x10 and port +0x20, remote address
            // +0x24 and port +0x34, state +0x38 (RTINETB dump 0x2378) [C].
            if (!data || length < 0x4c)
                return inet::Invalid;
            std::lock_guard lock(s->mutex);
            std::memset(data, 0, 0x4c);
            wr32(data + 0x00, uint32_t(cid));
            wr32(data + 0x04, s->udpLike() ? 2u : 1u);
            const Handle h = s->handle != NoHandle ? s->handle : s->listener;
            wr32(data + 0x08, uint32_t(h != NoHandle ? pendingBytes(h) : 0));
            writeInetAddress(data + 0x10, s->localAddress);
            wr32(data + 0x20, s->localPort);
            writeInetAddress(data + 0x24, s->remoteAddress);
            wr32(data + 0x34, s->remotePort);
            wr32(data + 0x38, uint32_t(s->state));
            return 0;
        }
        if (code == 3 && data && length == 4)
        {
            // rt_comm sets option 3 = 1 on TCP sockets (CORE 0x5229ac) [C]; taken
            // as no-delay [I].
            std::lock_guard lock(s->mutex);
            s->noDelay = rd32(data) != 0;
            if (!s->udpLike() && s->handle != NoHandle)
            {
                int on = s->noDelay ? 1 : 0;
                setsockopt(s->handle, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char *>(&on), sizeof(on));
            }
            return 0;
        }
        logLine(1, "control cid " + hex(uint32_t(cid)) + " code " + std::to_string(code) + " length " +
                       std::to_string(length) + " not provided");
        return inet::Invalid;
    }

    int32_t Stack::poll(std::span<PollEntry> entries, int32_t ms)
    {
        const auto start = Clock::now();
        const uint64_t generation = generation_.load();
        for (;;)
        {
            int32_t ready = 0;
            for (auto &e : entries)
            {
                e.revents = 0;
                const auto s = find(e.cid);
                if (!s)
                {
                    e.revents = inet::PollNval;
                    ++ready;
                    continue;
                }
                const Handle h = s->waitHandle();
                if (h == NoHandle)
                {
                    e.revents = inet::PollHup;
                    ++ready;
                    continue;
                }
                fd_set readable, writable, failed;
                FD_ZERO(&readable);
                FD_ZERO(&writable);
                FD_ZERO(&failed);
                FD_SET(h, &readable);
                FD_SET(h, &writable);
                FD_SET(h, &failed);
                timeval tv{0, 0};
                if (::select(static_cast<int>(h) + 1, &readable, &writable, &failed, &tv) > 0)
                {
                    int16_t r = 0;
                    if (FD_ISSET(h, &readable))
                        r |= inet::PollIn;
                    if (FD_ISSET(h, &writable) && (s->udpLike() || s->connected))
                        r |= inet::PollOut;
                    if (FD_ISSET(h, &failed))
                        r |= inet::PollErr;
                    e.revents = static_cast<int16_t>(r & (e.events | inet::PollErr | inet::PollHup));
                }
                if (e.revents)
                    ++ready;
            }
            // A reset ends the wait even with no (valid) entry to look at.
            if (ready || ms == 0 || stopping_ || generation_.load() != generation)
                return ready;
            const auto spent = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - start).count();
            if (ms > 0 && spent >= ms)
                return 0;
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
    }

    int32_t Stack::name2Address(std::string_view name, uint32_t &address)
    {
        if (parseIpv4(name, address))
            return 0;
        // No DNS (spec 3.4): names fail at once [I code]; one log line per name.
        ++refusals_;
        bool first = false;
        {
            std::lock_guard lock(mutex_);
            first = refusedNames_.size() < 64 && refusedNames_.insert(std::string(name)).second;
        }
        if (first)
            host_.log(LogLevel::Info, "[GT4/NET] refused name lookup '" + std::string(name) +
                                          "' (no DNS, LAN only)");
        return inet::Timeout;
    }

    void Stack::shutdown()
    {
        std::map<int32_t, std::shared_ptr<Socket>> all;
        {
            std::lock_guard lock(mutex_);
            all.swap(sockets_);
            refusedLogged_.clear();
            refusedNames_.clear();
        }
        ++generation_;
        for (auto &[cid, s] : all)
        {
            s->closed = true;
            ++s->aborts;
        }
    }

    std::vector<BoundEndpoint> Stack::boundEndpoints() const
    {
        std::vector<BoundEndpoint> out;
        // Socket locks are taken without the table lock: tryAccept holds a
        // socket lock while refuse() takes the table lock (lock order).
        std::map<int32_t, std::shared_ptr<Socket>> all;
        {
            std::lock_guard lock(mutex_);
            all = sockets_;
        }
        for (const auto &[cid, s] : all)
        {
            std::lock_guard socketLock(s->mutex);
            for (Handle h : {s->handle, s->listener})
            {
                BoundEndpoint e;
                e.cid = cid;
                e.listening = h == s->listener;
                if (localEndpoint(h, e.address, e.port))
                    out.push_back(e);
            }
        }
        return out;
    }

    void Stack::appendMetrics(std::vector<DebugMetric> &metrics) const
    {
        size_t open = 0;
        {
            std::lock_guard lock(mutex_);
            open = sockets_.size();
        }
        metrics.push_back({"net.sockets", open, false});
        metrics.push_back({"net.datagrams_out", datagramsOut_.load(), false});
        metrics.push_back({"net.datagrams_in", datagramsIn_.load(), false});
        metrics.push_back({"net.bytes_out", bytesOut_.load(), false});
        metrics.push_back({"net.bytes_in", bytesIn_.load(), false});
        metrics.push_back({"net.refusals", refusals_.load(), false});
        metrics.push_back({"net.inbound_dropped", inboundDropped_.load(), false});
    }

    // -------------------------------------------------------------- Network

    namespace
    {
        std::mutex g_mrpcMutex;
        Rtinetb *g_mrpc = nullptr;
    }

    Rtinetb *mrpcService(uint32_t sid)
    {
        std::lock_guard lock(g_mrpcMutex);
        return sid == SidRtinetb ? g_mrpc : nullptr;
    }

    void registerMrpcService(Rtinetb *service)
    {
        std::lock_guard lock(g_mrpcMutex);
        g_mrpc = service;
    }

    Network::Network(IopHost &host, const Config &config) : host_(host)
    {
        stack_ = std::make_unique<Stack>(host, config);
        rtinetb_ = createRtinetb(host, *stack_, env_);
        pdinetcnf_ = createPdinetcnf(host, *stack_, env_);
        registerMrpcService(rtinetb_.get());
        host_.log(LogLevel::Info, "[GT4/NET] LAN network " + config.describe() +
                                      " (RTINETB 0x80001201 via libmrpc, PNCF, DEV9/SMAP present)");
    }

    Network::~Network()
    {
        {
            std::lock_guard lock(g_mrpcMutex);
            if (g_mrpc == rtinetb_.get())
                g_mrpc = nullptr;
        }
        rtinetb_.reset(); // joins its threads before the stack goes
        pdinetcnf_.reset();
        stack_.reset();
        env_.reset(host_);
    }

    void Network::reset()
    {
        rtinetb_->reset();
        pdinetcnf_->reset();
        stack_->shutdown();
        env_.reset(host_);
    }

    void Network::onSifTransfer(const SifTransfer &transfer) { rtinetb_->onSifTransfer(transfer); }

    void Network::appendMetrics(std::vector<DebugMetric> &metrics) const
    {
        stack_->appendMetrics(metrics);
        rtinetb_->appendMetrics(metrics);
    }
}
