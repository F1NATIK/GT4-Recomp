// GT4 Recompilation launcher, Full edition: its Windows side (gt4full_system.h).

#include "gt4full_system.h"

#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>

#include <mmsystem.h>

#include <algorithm>
#include <cstdio>
#include <cwctype>
#include <vector>

namespace gt4stock::sys
{
    namespace
    {
        std::string narrow(const wchar_t *w)
        {
            if (!w)
                return std::string();
            const int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
            if (n <= 1)
                return std::string();
            std::string s(static_cast<size_t>(n - 1), '\0');
            WideCharToMultiByte(CP_UTF8, 0, w, -1, s.data(), n, nullptr, nullptr);
            return s;
        }
    }

    std::vector<NetAddress> ipv4Addresses(bool demo)
    {
        std::vector<NetAddress> out;
        if (demo)
        {
            out.push_back({"192.168.1.20", "Ethernet", true, true});
            out.push_back({"192.168.56.1", "VirtualBox Host-Only Network", true, false});
            return out;
        }
        ULONG size = 16 * 1024;
        std::vector<unsigned char> buf;
        ULONG rc = ERROR_BUFFER_OVERFLOW;
        for (int tries = 0; tries < 4 && rc == ERROR_BUFFER_OVERFLOW; ++tries)
        {
            buf.resize(size);
            rc = GetAdaptersAddresses(AF_INET, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER | GAA_FLAG_INCLUDE_GATEWAYS, nullptr,
                                      reinterpret_cast<IP_ADAPTER_ADDRESSES *>(buf.data()), &size);
        }
        if (rc != NO_ERROR)
            return out;
        for (auto *a = reinterpret_cast<IP_ADAPTER_ADDRESSES *>(buf.data()); a; a = a->Next)
        {
            if (a->OperStatus != IfOperStatusUp || a->IfType == IF_TYPE_SOFTWARE_LOOPBACK)
                continue;
            for (auto *u = a->FirstUnicastAddress; u; u = u->Next)
            {
                if (!u->Address.lpSockaddr || u->Address.lpSockaddr->sa_family != AF_INET)
                    continue;
                const auto *sin = reinterpret_cast<const sockaddr_in *>(u->Address.lpSockaddr);
                const unsigned long ip = ntohl(sin->sin_addr.s_addr);
                char text[32];
                std::snprintf(text, sizeof(text), "%lu.%lu.%lu.%lu", (ip >> 24) & 255, (ip >> 16) & 255, (ip >> 8) & 255, ip & 255);
                NetAddress n;
                n.address = text;
                n.adapter = narrow(a->FriendlyName);
                const unsigned long b0 = (ip >> 24) & 255, b1 = (ip >> 16) & 255;
                n.privateNet = b0 == 10 || (b0 == 172 && b1 >= 16 && b1 <= 31) || (b0 == 192 && b1 == 168);
                n.gateway = a->FirstGatewayAddress != nullptr;
                if (b0 == 127 || (b0 == 169 && b1 == 254))
                    continue; // loopback, link-local (no DHCP answer)
                out.push_back(n);
            }
        }
        std::stable_sort(out.begin(), out.end(), [](const NetAddress &x, const NetAddress &y) {
            return (x.privateNet && x.gateway) != (y.privateNet && y.gateway) ? (x.privateNet && x.gateway) : x.privateNet > y.privateNet;
        });
        return out;
    }

    std::string findPython()
    {
        for (const wchar_t *name : {L"py.exe", L"python.exe", L"python3.exe"})
        {
            // Every folder on PATH (SearchPathW stops at the first, which may be a Store placeholder).
            std::wstring path(32768, L'\0');
            const DWORD n = GetEnvironmentVariableW(L"PATH", path.data(), static_cast<DWORD>(path.size()));
            path.resize(n < path.size() ? n : 0);
            size_t at = 0;
            while (at <= path.size())
            {
                size_t end = path.find(L';', at);
                if (end == std::wstring::npos)
                    end = path.size();
                std::wstring dir = path.substr(at, end - at);
                at = end + 1;
                if (dir.empty())
                    continue;
                if (dir.back() != L'\\' && dir.back() != L'/')
                    dir += L'\\';
                const std::wstring file = dir + name;
                std::wstring lower = file;
                for (wchar_t &c : lower)
                    c = static_cast<wchar_t>(std::towlower(c));
                if (lower.find(L"\\windowsapps\\") != std::wstring::npos)
                    continue; // the Microsoft Store's placeholder
                const DWORD attr = GetFileAttributesW(file.c_str());
                if (attr != INVALID_FILE_ATTRIBUTES && !(attr & FILE_ATTRIBUTE_DIRECTORY))
                    return narrow(file.c_str());
                if (end >= path.size())
                    break;
            }
        }
        return std::string();
    }

    bool playWav(const std::string &utf8Path)
    {
        const int n = MultiByteToWideChar(CP_UTF8, 0, utf8Path.c_str(), -1, nullptr, 0);
        if (n <= 1)
            return false;
        std::wstring w(static_cast<size_t>(n), L'\0');
        MultiByteToWideChar(CP_UTF8, 0, utf8Path.c_str(), -1, w.data(), n);
        return PlaySoundW(w.c_str(), nullptr, SND_FILENAME | SND_ASYNC | SND_NODEFAULT) != FALSE;
    }

    void stopWav() { PlaySoundW(nullptr, nullptr, 0); }
}
