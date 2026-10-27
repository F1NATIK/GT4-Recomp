#pragma once

// The telemetry window's monitor (HUD hand-off 8 T1), shared by the Full launcher (Driving > Telemetry screen writes it) and
// GT4.exe (gt4_pages.cpp opens the window there). Header-only.
//   GT4_TELEMETRY_MONITOR=<DISPLAYn>,<friendly name>,<W>x<H>,<x>,<y>   (hand-off 8: "DISPLAY4,MSI G321Q,2560x1440,0,-1440")
//                        | <x>,<y>,<w>,<h>                              (older launchers: the monitor's rectangle)
//                        | <n>                                          (1 = Windows' first monitor)
//                        | 0 / off / empty                              (no window)
// GT4.exe matches the friendly name and the desktop position first, then the device name (Windows renumbers displays after
// driver updates and replugging); a monitor that is not there: no window, no error.
// The parsing and matching are plain C++ (the launcher's host test checks them); list() is Windows' side and is only
// compiled where GT4_TELE_MONITORS_WIN32 is defined before this header (a Windows translation unit without raylib).

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace gt4tele
{
    struct Monitor
    {
        std::string device;   // "DISPLAY4" (Windows' \\.\DISPLAY4 without the prefix)
        std::string friendly; // the EDID name ("MSI G321Q"); the device name when Windows gives none
        int x = 0, y = 0, w = 0, h = 0, hz = 0;
        bool primary = false;
    };

    struct Setting
    {
        enum Kind
        {
            Off,
            Named, // the hand-off 8 form
            Rect,  // an older launcher's x,y,w,h
            Index, // n, 1-based
        } kind = Off;
        std::string device, friendly;
        int x = 0, y = 0, w = 0, h = 0, index = 0;
    };

    // A friendly name as the setting holds it: no commas (the separator) and no line breaks.
    inline std::string cleanName(std::string s)
    {
        for (char &c : s)
            if (c == ',' || c == '\r' || c == '\n' || c == '=')
                c = ' ';
        while (!s.empty() && s.back() == ' ')
            s.pop_back();
        while (!s.empty() && s.front() == ' ')
            s.erase(s.begin());
        return s;
    }
    inline std::string shortDevice(std::string d)
    {
        const size_t at = d.rfind('\\');
        return at == std::string::npos ? d : d.substr(at + 1);
    }

    inline std::string format(const Monitor &m)
    {
        char buf[64];
        std::snprintf(buf, sizeof(buf), ",%dx%d,%d,%d", m.w, m.h, m.x, m.y);
        return shortDevice(m.device) + "," + cleanName(m.friendly) + buf;
    }

    inline std::vector<std::string> split(const std::string &s, char sep)
    {
        std::vector<std::string> out(1);
        for (char c : s)
            if (c == sep)
                out.emplace_back();
            else
                out.back() += c;
        return out;
    }
    inline bool parseInt(const std::string &s, int &v)
    {
        if (s.empty())
            return false;
        char *end = nullptr;
        const long n = std::strtol(s.c_str(), &end, 10);
        if (!end || *end)
            return false;
        v = static_cast<int>(n);
        return true;
    }

    inline Setting parse(const std::string &value)
    {
        Setting s;
        if (value.empty() || value == "0" || value == "off")
            return s;
        const std::vector<std::string> p = split(value, ',');
        int a = 0, b = 0, c = 0, d = 0;
        if (p.size() == 4 && parseInt(p[0], a) && parseInt(p[1], b) && parseInt(p[2], c) && parseInt(p[3], d))
        {
            if (c > 0 && d > 0)
                s.kind = Setting::Rect, s.x = a, s.y = b, s.w = c, s.h = d;
            return s;
        }
        if (p.size() == 1 && parseInt(p[0], a))
        {
            if (a > 0)
                s.kind = Setting::Index, s.index = a;
            return s;
        }
        if (p.size() == 5)
        {
            int w = 0, h = 0;
            const size_t xat = p[2].find('x');
            if (xat != std::string::npos && parseInt(p[2].substr(0, xat), w) && parseInt(p[2].substr(xat + 1), h) && parseInt(p[3], a) && parseInt(p[4], b))
            {
                s.kind = Setting::Named;
                s.device = shortDevice(p[0]);
                s.friendly = p[1];
                s.w = w, s.h = h, s.x = a, s.y = b;
            }
        }
        return s;
    }

    // The monitor the setting means among those connected now (-1: none). Named: friendly name and position, then the
    // device name, then the friendly name alone when only one monitor has it. Rect: the same rectangle, else the monitor
    // with the rectangle's top-left corner. Index: the n-th.
    inline int match(const Setting &s, const std::vector<Monitor> &list)
    {
        const int n = static_cast<int>(list.size());
        switch (s.kind)
        {
        case Setting::Named:
            for (int i = 0; i < n; ++i)
                if (cleanName(list[i].friendly) == s.friendly && list[i].x == s.x && list[i].y == s.y)
                    return i;
            for (int i = 0; i < n; ++i)
                if (shortDevice(list[i].device) == s.device)
                    return i;
            {
                int found = -1, count = 0;
                for (int i = 0; i < n; ++i)
                    if (cleanName(list[i].friendly) == s.friendly)
                        found = i, ++count;
                if (count == 1)
                    return found;
            }
            return -1;
        case Setting::Rect:
            for (int i = 0; i < n; ++i)
                if (list[i].x == s.x && list[i].y == s.y && list[i].w == s.w && list[i].h == s.h)
                    return i;
            for (int i = 0; i < n; ++i)
                if (s.x >= list[i].x && s.y >= list[i].y && s.x < list[i].x + list[i].w && s.y < list[i].y + list[i].h)
                    return i;
            return -1;
        case Setting::Index: return s.index <= n ? s.index - 1 : -1;
        default: return -1;
        }
    }
} // namespace gt4tele

#if defined(GT4_TELE_MONITORS_WIN32) && defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

namespace gt4tele
{
    namespace win
    {
        inline std::string narrow(const wchar_t *w)
        {
            if (!w || !*w)
                return {};
            const int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
            std::string s(static_cast<size_t>(n > 0 ? n - 1 : 0), '\0');
            if (n > 1)
                WideCharToMultiByte(CP_UTF8, 0, w, -1, s.data(), n, nullptr, nullptr);
            return s;
        }
        struct Found
        {
            std::vector<Monitor> list;
        };
        inline BOOL CALLBACK each(HMONITOR hm, HDC, LPRECT, LPARAM p)
        {
            MONITORINFOEXW mi{};
            mi.cbSize = sizeof(mi);
            if (!GetMonitorInfoW(hm, &mi))
                return TRUE;
            Monitor m;
            m.device = shortDevice(narrow(mi.szDevice));
            m.x = mi.rcMonitor.left;
            m.y = mi.rcMonitor.top;
            m.w = mi.rcMonitor.right - mi.rcMonitor.left;
            m.h = mi.rcMonitor.bottom - mi.rcMonitor.top;
            m.primary = (mi.dwFlags & MONITORINFOF_PRIMARY) != 0;
            DEVMODEW dm{};
            dm.dmSize = sizeof(dm);
            if (EnumDisplaySettingsW(mi.szDevice, ENUM_CURRENT_SETTINGS, &dm))
                m.hz = static_cast<int>(dm.dmDisplayFrequency);
            reinterpret_cast<Found *>(p)->list.push_back(m);
            return TRUE;
        }
    } // namespace win

    // The monitors as Windows arranges them now, in Windows' own order: the device name, the desktop rectangle, the refresh
    // rate (QueryDisplayConfig's, else the display settings') and the EDID friendly name.
    inline std::vector<Monitor> list()
    {
        win::Found f;
        EnumDisplayMonitors(nullptr, nullptr, win::each, reinterpret_cast<LPARAM>(&f));
        UINT32 np = 0, nm = 0;
        if (GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &np, &nm) == ERROR_SUCCESS && np > 0)
        {
            std::vector<DISPLAYCONFIG_PATH_INFO> paths(np);
            std::vector<DISPLAYCONFIG_MODE_INFO> modes(nm);
            if (QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &np, paths.data(), &nm, modes.data(), nullptr) == ERROR_SUCCESS)
            {
                paths.resize(np);
                for (const DISPLAYCONFIG_PATH_INFO &p : paths)
                {
                    DISPLAYCONFIG_SOURCE_DEVICE_NAME src{};
                    src.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
                    src.header.size = sizeof(src);
                    src.header.adapterId = p.sourceInfo.adapterId;
                    src.header.id = p.sourceInfo.id;
                    if (DisplayConfigGetDeviceInfo(&src.header) != ERROR_SUCCESS)
                        continue;
                    const std::string dev = shortDevice(win::narrow(src.viewGdiDeviceName));
                    DISPLAYCONFIG_TARGET_DEVICE_NAME tgt{};
                    tgt.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_TARGET_NAME;
                    tgt.header.size = sizeof(tgt);
                    tgt.header.adapterId = p.targetInfo.adapterId;
                    tgt.header.id = p.targetInfo.id;
                    std::string name;
                    if (DisplayConfigGetDeviceInfo(&tgt.header) == ERROR_SUCCESS)
                        name = win::narrow(tgt.monitorFriendlyDeviceName);
                    const DISPLAYCONFIG_RATIONAL r = p.targetInfo.refreshRate;
                    for (Monitor &m : f.list)
                        if (m.device == dev)
                        {
                            if (!name.empty() && m.friendly.empty())
                                m.friendly = name;
                            if (r.Denominator && r.Numerator)
                                m.hz = static_cast<int>((static_cast<double>(r.Numerator) / r.Denominator) + 0.5);
                        }
                }
            }
        }
        for (Monitor &m : f.list)
            if (m.friendly.empty())
                m.friendly = m.device;
        return f.list;
    }
} // namespace gt4tele
#endif
