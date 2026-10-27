// GT4 Recompilation launcher, Full edition: Driving > Telemetry screen's Windows side (gt4full_telesys.h).

#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#include <windows.h>

#define GT4_TELE_MONITORS_WIN32 1
#include "gt4full_telesys.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <thread>

namespace gt4stock::sys
{
    std::vector<gt4tele::Monitor> teleMonitors() { return gt4tele::list(); }

    namespace
    {
        LRESULT CALLBACK identifyProc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
        {
            if (msg == WM_PAINT)
            {
                PAINTSTRUCT ps;
                HDC dc = BeginPaint(h, &ps);
                RECT r;
                GetClientRect(h, &r);
                HBRUSH bg = CreateSolidBrush(RGB(17, 21, 28));
                FillRect(dc, &r, bg);
                DeleteObject(bg);
                HBRUSH edge = CreateSolidBrush(RGB(76, 141, 255));
                RECT top{r.left, r.top, r.right, r.top + 8};
                FillRect(dc, &top, edge);
                DeleteObject(edge);
                wchar_t text[16];
                std::swprintf(text, 16, L"%d", static_cast<int>(GetWindowLongPtrW(h, GWLP_USERDATA)));
                HFONT f = CreateFontW(-(r.bottom - r.top) * 7 / 10, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                      CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
                HGDIOBJ old = SelectObject(dc, f);
                SetBkMode(dc, TRANSPARENT);
                SetTextColor(dc, RGB(232, 236, 242));
                DrawTextW(dc, text, -1, &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
                SelectObject(dc, old);
                DeleteObject(f);
                EndPaint(h, &ps);
                return 0;
            }
            if (msg == WM_MOUSEACTIVATE)
                return MA_NOACTIVATE;
            return DefWindowProcW(h, msg, wp, lp);
        }
    }

    void identifyMonitors(const std::vector<gt4tele::Monitor> &list)
    {
        std::thread([list]() {
            HINSTANCE inst = GetModuleHandleW(nullptr);
            WNDCLASSW wc{};
            wc.lpfnWndProc = identifyProc;
            wc.hInstance = inst;
            wc.lpszClassName = L"GT4IdentifyMonitor";
            wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
            RegisterClassW(&wc); // (fails harmlessly the second time)
            std::vector<HWND> windows;
            for (size_t i = 0; i < list.size(); ++i)
            {
                const gt4tele::Monitor &m = list[i];
                const int side = std::max(120, std::min(m.w, m.h) / 3);
                HWND h = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, wc.lpszClassName, L"", WS_POPUP, m.x + (m.w - side) / 2,
                                         m.y + (m.h - side) / 2, side, side, nullptr, nullptr, inst, nullptr);
                if (!h)
                    continue;
                SetWindowLongPtrW(h, GWLP_USERDATA, static_cast<LONG_PTR>(i + 1));
                ShowWindow(h, SW_SHOWNOACTIVATE);
                windows.push_back(h);
            }
            const ULONGLONG end = GetTickCount64() + 3000;
            while (GetTickCount64() < end)
            {
                MSG msg;
                while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE))
                {
                    TranslateMessage(&msg);
                    DispatchMessageW(&msg);
                }
                Sleep(15);
            }
            for (HWND h : windows)
                DestroyWindow(h);
        }).detach();
    }

    std::string lanAddress(std::string *adapter)
    {
        // The adapter Windows would send a packet to the internet through (a routing table lookup only: nothing is sent).
        DWORD best = 0;
        const bool haveBest = GetBestInterface(htonl(0x08080808), &best) == NO_ERROR;
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
            return {};
        std::string fallback, fallbackName;
        for (auto *a = reinterpret_cast<IP_ADAPTER_ADDRESSES *>(buf.data()); a; a = a->Next)
        {
            if (a->OperStatus != IfOperStatusUp || a->IfType == IF_TYPE_SOFTWARE_LOOPBACK)
                continue;
            for (auto *u = a->FirstUnicastAddress; u; u = u->Next)
            {
                if (!u->Address.lpSockaddr || u->Address.lpSockaddr->sa_family != AF_INET)
                    continue;
                const unsigned long ip = ntohl(reinterpret_cast<const sockaddr_in *>(u->Address.lpSockaddr)->sin_addr.s_addr);
                const unsigned long b0 = (ip >> 24) & 255, b1 = (ip >> 16) & 255;
                if (b0 == 127 || (b0 == 169 && b1 == 254))
                    continue;
                char text[32];
                std::snprintf(text, sizeof(text), "%lu.%lu.%lu.%lu", b0, b1, (ip >> 8) & 255, ip & 255);
                char name[256] = {};
                WideCharToMultiByte(CP_UTF8, 0, a->FriendlyName, -1, name, sizeof(name) - 1, nullptr, nullptr);
                if (haveBest && a->IfIndex == best)
                {
                    if (adapter)
                        *adapter = name;
                    return text;
                }
                if (fallback.empty() && a->FirstGatewayAddress)
                    fallback = text, fallbackName = name;
            }
        }
        if (adapter)
            *adapter = fallbackName;
        return fallback;
    }

    std::string edgePath()
    {
        for (const char *base : {"ProgramFiles(x86)", "ProgramFiles", "LOCALAPPDATA"})
        {
            char b[MAX_PATH] = {};
            const DWORD n = GetEnvironmentVariableA(base, b, MAX_PATH);
            if (n == 0 || n >= MAX_PATH)
                continue;
            const std::string p = std::string(b) + "\\Microsoft\\Edge\\Application\\msedge.exe";
            if (GetFileAttributesA(p.c_str()) != INVALID_FILE_ATTRIBUTES)
                return p;
        }
        return {};
    }
}
