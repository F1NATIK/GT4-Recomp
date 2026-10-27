#pragma once

// GT4 Recompilation launcher, Full edition: its Windows side (kept out of the raylib translation units): this PC's
// network addresses for the Network page (listed only; nothing is opened or sent).

#include <string>
#include <vector>

namespace gt4stock::sys
{
    struct NetAddress
    {
        std::string address; // "192.168.1.20"
        std::string adapter; // Windows' name for the adapter ("Wi-Fi")
        bool privateNet = false; // 10.x, 172.16-31.x, 192.168.x (GT4's LAN Battle stays on the local network)
        bool gateway = false;    // the adapter has a default gateway
    };
    // The PC's IPv4 addresses on adapters that are up (no loopback), private ones first. demo: the developer's two PCs' kind of list.
    std::vector<NetAddress> ipv4Addresses(bool demo);

    // Sound > Sound packs: Python 3 for sound_pack.py, py.exe (the python.org launcher) or python.exe on PATH; the Microsoft
    // Store's python.exe placeholders in WindowsApps are skipped (they open the Store). Empty when there is none.
    std::string findPython();
    // A WAV file played once in the background (Windows' PlaySound); stopWav ends it. False when Windows cannot play it.
    bool playWav(const std::string &utf8Path);
    void stopWav();
}
