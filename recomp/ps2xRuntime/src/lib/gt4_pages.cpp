// GT4's pages service: see gt4_pages.h and hud/pages/FEED.md.
#include "gt4_pages.h"

#if defined(PS2X_GAME_GT4) && !defined(GT4_STOCK_BUILD) && defined(_WIN32)

#include "gt4_mirror.h"
#include "gt4_race_reader.h"
#include "gt4_ai_drivers.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

// (HUD hand-off 8: the telemetry window's monitor by its EDID name and place, as the launcher writes it)
#define GT4_TELE_MONITORS_WIN32 1
#include "../launcher/gt4stock/shared/gt4_tele_monitors.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace gt4pages
{
    namespace
    {
        std::atomic<int> g_clients{0};
        constexpr int kMaxClients = 8;
        std::string g_dir;
        HANDLE g_window = nullptr; // the telemetry window's process

        std::string envOr(const char *k, const char *def)
        {
            const char *v = std::getenv(k);
            return (v && *v) ? std::string(v) : std::string(def);
        }

        std::string exeDir()
        {
            char path[MAX_PATH] = {};
            GetModuleFileNameA(nullptr, path, MAX_PATH);
            std::string p = path;
            const size_t s = p.find_last_of("\\/");
            return s == std::string::npos ? std::string(".") : p.substr(0, s);
        }

        bool sendAll(SOCKET s, const char *data, size_t n)
        {
            while (n > 0)
            {
                const int sent = send(s, data, static_cast<int>(std::min<size_t>(n, 1u << 20)), 0);
                if (sent <= 0)
                    return false;
                data += sent;
                n -= static_cast<size_t>(sent);
            }
            return true;
        }
        bool sendStr(SOCKET s, const std::string &t) { return sendAll(s, t.data(), t.size()); }

        const char *phaseName(gt4race::Phase p)
        {
            switch (p)
            {
            case gt4race::Phase::PreRace: return "prerace";
            case gt4race::Phase::Starting: return "starting";
            case gt4race::Phase::Racing: return "racing";
            case gt4race::Phase::Paused: return "paused";
            case gt4race::Phase::Finished: return "finished";
            case gt4race::Phase::Replay: return "replay";
            default: return "menu";
            }
        }

        void jsonString(std::ostringstream &o, const char *s)
        {
            o << '"';
            for (; *s; ++s)
            {
                const unsigned char c = static_cast<unsigned char>(*s);
                if (c == '"' || c == '\\')
                    o << '\\' << static_cast<char>(c);
                else if (c < 32)
                    o << ' ';
                else
                    o << static_cast<char>(c);
            }
            o << '"';
        }

        // One feed message (FEED.md "Message"); "menu" while no race frame came for a second.
        std::string message()
        {
            gt4race::State s;
            if (gt4race::msSinceLastFrame() > 1000u || !gt4race::snapshot(s) || s.phase == gt4race::Phase::None)
                return "{\"v\":1,\"phase\":\"menu\"}";
            std::ostringstream o;
            o.setf(std::ios::fixed);
            o.precision(3);
            o << "{\"v\":1,\"phase\":\"" << phaseName(s.phase) << "\",\"mode\":" << s.mode << ",\"course\":" << s.course << ",\"laps\":" << s.laps
              << ",\"clock_ms\":" << s.clockMs << ",\"course_len_m\":";
            if (s.courseLength > 0.0f)
                o << s.courseLength;
            else
                o << "null";
            o << ",\"start_hold\":" << s.startHold;
            // (the rear-view mirror page: how old its newest picture is, -1 none; gt4_mirror.h)
            o << ",\"mirror_ms\":" << gt4mirror::lastAgeMs();
            if (s.unitSpeed >= 0)
                o << ",\"units\":{\"speed\":\"" << (s.unitSpeed == 1 ? "mph" : "kmh") << "\",\"power\":" << s.unitPower << ",\"torque\":" << s.unitTorque << '}';
            o << ",\"cars\":[";
            for (int i = 0; i < s.carCount; ++i)
            {
                const gt4race::Car &c = s.cars[i];
                if (i)
                    o << ',';
                o << "{\"car\":" << i << ",\"player\":" << (c.player ? "true" : "false") << ",\"place\":" << c.place << ",\"lap\":" << c.lap
                  << ",\"laps_done\":" << c.lapsDone << ",\"lap_ms\":" << c.lapTimeMs << ",\"last_ms\":" << c.lastLapMs << ",\"best_ms\":" << c.bestLapMs
                  << ",\"total_ms\":" << c.totalMs << ",\"finished\":" << (c.finished ? "true" : "false") << ",\"dist\":" << c.lapDistance
                  << ",\"x\":" << c.x << ",\"y\":" << c.y << ",\"z\":" << c.z << ",\"yaw\":" << c.yaw << ",\"speed\":" << c.speed
                  << ",\"code\":" << c.carCode << ",\"name\":";
                jsonString(o, c.name);
                // (Race Lab: the driver it sent for this AI car, UTF-8; none for yours or a car it named nobody for: FEED.md "driver")
                if (const std::string d = gt4drivers::driverOf(s, i); !d.empty())
                {
                    o << ",\"driver\":";
                    jsonString(o, d.c_str());
                    o << ",\"ai\":" << gt4drivers::aiNumber(s, i);
                }
                // (HUD hand-off 7 C: the split times, the course's sectors each; ms, 0 = no time)
                o << ",\"sector\":" << c.sector << ",\"sector_ms\":[";
                for (int k = 0; k < s.sectorCount && k < gt4race::kMaxSectors; ++k)
                    o << (k ? "," : "") << c.sectorMs[k];
                o << "],\"best_sector_ms\":[";
                for (int k = 0; k < s.sectorCount && k < gt4race::kMaxSectors; ++k)
                    o << (k ? "," : "") << c.bestSectorMs[k];
                o << "]}";
            }
            o << "]";
            if (s.player.index >= 0)
            {
                const gt4race::Player &p = s.player;
                o << ",\"player\":{\"gear\":" << p.gear << ",\"rpm\":" << p.rpm << ",\"throttle\":" << p.throttle << ",\"brake\":" << p.brake
                  << ",\"steer\":" << p.steer << ",\"acc_long\":" << p.accelLong << ",\"acc_lat\":" << p.accelLat << '}';
            }
            o << '}';
            return o.str();
        }

        void serveFeed(SOCKET s)
        {
            if (!sendStr(s, "HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\nCache-Control: no-cache\r\nConnection: keep-alive\r\n"
                            "Access-Control-Allow-Origin: *\r\n\r\n"))
                return;
            auto next = std::chrono::steady_clock::now();
            int menuTicks = 0;
            for (;;)
            {
                next += std::chrono::microseconds(33333);
                const std::string m = message();
                const bool menu = m.size() < 40u;
                // In the menus: once a second (FEED.md); in a race: every tick.
                if (!menu || menuTicks++ % 30 == 0)
                {
                    if (!menu)
                        menuTicks = 0;
                    if (!sendStr(s, "data: " + m + "\n\n"))
                        return;
                }
                std::this_thread::sleep_until(next);
            }
        }

        // GET /mirror.mjpg: GT4's rear-view mirror as an MJPEG stream (gt4_mirror.h), while the viewer stays; GET /mirror.jpg:
        // the newest picture (404 before the first).
        void serveMirror(SOCKET s, bool stream)
        {
            if (!stream)
            {
                gt4mirror::stillAsked();
                const auto jpeg = gt4mirror::lastJpeg();
                if (!jpeg)
                {
                    sendStr(s, "HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\nConnection: close\r\n\r\n");
                    return;
                }
                if (sendStr(s, "HTTP/1.1 200 OK\r\nContent-Type: image/jpeg\r\nContent-Length: " + std::to_string(jpeg->size()) +
                                   "\r\nCache-Control: no-cache\r\nConnection: close\r\n\r\n"))
                    sendAll(s, reinterpret_cast<const char *>(jpeg->data()), jpeg->size());
                return;
            }
            if (!sendStr(s, "HTTP/1.1 200 OK\r\nContent-Type: multipart/x-mixed-replace; boundary=gt4frame\r\nCache-Control: no-cache\r\n"
                            "Connection: close\r\nAccess-Control-Allow-Origin: *\r\n\r\n"))
                return;
            gt4mirror::viewerJoin();
            uint64_t serial = 0;
            for (;;)
            {
                // (no new picture within a second, outside races or out of the bumper view: an empty part keeps the
                // connection checked, so a viewer that went is noticed)
                auto jpeg = gt4mirror::nextJpeg(serial, 1000);
                // (21 Oct: no new picture within a second: the last one again, so a phone's picture stays a picture; an empty
                // text part there broke the phone's image and it waited 3 s to open the stream again: the developer's drop-outs)
                if (!jpeg)
                    jpeg = gt4mirror::lastJpeg();
                if (!jpeg)
                {
                    if (!sendStr(s, "--gt4frame\r\nContent-Type: text/plain\r\nContent-Length: 0\r\n\r\n\r\n"))
                        break;
                    continue;
                }
                const std::string head = "--gt4frame\r\nContent-Type: image/jpeg\r\nContent-Length: " + std::to_string(jpeg->size()) + "\r\n\r\n";
                if (!sendStr(s, head) || !sendAll(s, reinterpret_cast<const char *>(jpeg->data()), jpeg->size()) || !sendStr(s, "\r\n"))
                    break;
            }
            gt4mirror::viewerLeave();
        }

        const char *contentType(const std::string &path)
        {
            auto ends = [&](const char *e) { return path.size() >= std::strlen(e) && path.compare(path.size() - std::strlen(e), std::string::npos, e) == 0; };
            if (ends(".html") || ends(".htm"))
                return "text/html; charset=utf-8";
            if (ends(".js"))
                return "text/javascript; charset=utf-8";
            if (ends(".css"))
                return "text/css; charset=utf-8";
            if (ends(".json"))
                return "application/json";
            if (ends(".png"))
                return "image/png";
            if (ends(".svg"))
                return "image/svg+xml";
            if (ends(".woff2"))
                return "font/woff2";
            return "application/octet-stream";
        }

        // GET /layout (HUD hand-off 8 T4): telemetry-layout.json as the launcher's Telemetry screen page writes it, beside
        // gt4-start.env (GT4.exe's working folder; else next to GT4.exe; GT4_TELE_LAYOUT_FILE names another file). 404 without
        // one: the page then uses its own defaults (the mock-up's).
        std::string layoutPath()
        {
            const std::string forced = envOr("GT4_TELE_LAYOUT_FILE", "");
            if (!forced.empty())
                return forced;
            for (const std::string &p : {std::string("telemetry-layout.json"), exeDir() + "\\telemetry-layout.json"})
                if (GetFileAttributesA(p.c_str()) != INVALID_FILE_ATTRIBUTES)
                    return p;
            return {};
        }
        void serveLayout(SOCKET s)
        {
            const std::string path = layoutPath();
            std::ifstream f(path, std::ios::binary);
            if (path.empty() || !f.good())
            {
                sendStr(s, "HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\nConnection: close\r\n\r\n");
                return;
            }
            const std::string body((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
            if (sendStr(s, "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: " + std::to_string(body.size()) +
                               "\r\nCache-Control: no-cache\r\nConnection: close\r\n\r\n"))
                sendAll(s, body.data(), body.size());
        }
        // GET /config (FEED.md "Phone panels and /config", "Radar zoom"): the launcher's settings the pages read, one key per
        // gt4-start.env line that is set, lower case without GT4_ (numbers as numbers); keys not set: the page's defaults.
        void serveConfig(SOCKET s)
        {
            static const char *const keys[] = {"GT4_RADAR_RANGE",  "GT4_RADAR_ZOOM",    "GT4_RADAR_LABELS", "GT4_RADAR_CARS",     "GT4_RADAR_COLOURS",
                                               "GT4_PHONE_PANELS", "GT4_PHONE_ARRANGE", "GT4_PHONE_MAP",    "GT4_PHONE_MAP_VIEW", "GT4_PHONE_NAMES",
                                               "GT4_PHONE_RADAR_RANGE", "GT4_TELE_BG", "GT4_RADAR_BG"};
            std::ostringstream o;
            o << '{';
            bool first = true;
            for (const char *k : keys)
            {
                const std::string v = envOr(k, "");
                if (v.empty())
                    continue;
                std::string name = k + 4;
                for (char &c : name)
                    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                o << (first ? "" : ",") << '"' << name << "\":";
                first = false;
                if (v.size() < 10 && v.find_first_not_of("0123456789") == std::string::npos)
                    o << v;
                else
                    jsonString(o, v.c_str());
            }
            o << '}';
            const std::string body = o.str();
            if (sendStr(s, "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: " + std::to_string(body.size()) +
                               "\r\nCache-Control: no-cache\r\nConnection: close\r\n\r\n"))
                sendAll(s, body.data(), body.size());
        }

        void serveFile(SOCKET s, std::string path)
        {
            const size_t q = path.find_first_of("?#");
            if (q != std::string::npos)
                path.resize(q);
            if (path.empty() || path == "/")
                path = "/telemetry.html";
            // Only plain relative names inside the pages folder (no "..", no drive or backslash).
            bool ok = path[0] == '/' && path.find("..") == std::string::npos && path.find('\\') == std::string::npos &&
                      path.find(':') == std::string::npos && path.rfind("/dev/", 0) != 0;
            std::string body;
            if (ok)
            {
                std::ifstream f(g_dir + path, std::ios::binary);
                ok = f.good();
                if (ok)
                    body.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
            }
            if (!ok)
            {
                sendStr(s, "HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\nConnection: close\r\n\r\n");
                return;
            }
            std::ostringstream h;
            h << "HTTP/1.1 200 OK\r\nContent-Type: " << contentType(path) << "\r\nContent-Length: " << body.size()
              << "\r\nCache-Control: no-cache\r\nConnection: close\r\n\r\n";
            if (sendStr(s, h.str()))
                sendAll(s, body.data(), body.size());
        }

        void client(SOCKET s)
        {
            char buf[2048];
            int n = 0;
            // The request line (headers are read and ignored).
            while (n < static_cast<int>(sizeof(buf)) - 1)
            {
                const int r = recv(s, buf + n, static_cast<int>(sizeof(buf)) - 1 - n, 0);
                if (r <= 0)
                    break;
                n += r;
                buf[n] = 0;
                if (std::strstr(buf, "\r\n\r\n") || std::strstr(buf, "\n\n"))
                    break;
            }
            buf[n] = 0;
            char method[8] = {}, target[512] = {};
            if (std::sscanf(buf, "%7s %511s", method, target) == 2 && std::strcmp(method, "GET") == 0)
            {
                if (std::strcmp(target, "/feed") == 0 || std::strncmp(target, "/feed?", 6) == 0)
                    serveFeed(s);
                else if (std::strcmp(target, "/mirror.mjpg") == 0 || std::strncmp(target, "/mirror.mjpg?", 13) == 0)
                    serveMirror(s, true);
                else if (std::strcmp(target, "/mirror.jpg") == 0 || std::strncmp(target, "/mirror.jpg?", 12) == 0)
                    serveMirror(s, false);
                else if (std::strcmp(target, "/layout") == 0 || std::strncmp(target, "/layout?", 8) == 0)
                    serveLayout(s);
                else if (std::strcmp(target, "/config") == 0 || std::strncmp(target, "/config?", 8) == 0)
                    serveConfig(s);
                else
                    serveFile(s, target);
            }
            closesocket(s);
            --g_clients;
        }

        void server(SOCKET listener)
        {
            for (;;)
            {
                SOCKET s = accept(listener, nullptr, nullptr);
                if (s == INVALID_SOCKET)
                    return;
                if (g_clients.load() >= kMaxClients)
                {
                    closesocket(s);
                    continue;
                }
                const int noDelay = 1;
                setsockopt(s, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char *>(&noDelay), sizeof(noDelay));
                ++g_clients;
                std::thread(client, s).detach();
            }
        }

        // Microsoft Edge in app mode, full screen on the chosen monitor, its own profile next to the pages; it never stays in
        // front of the game (no activation, no taskbar button).
        struct FindWindowFor
        {
            DWORD pid;
            HWND hwnd;
        };

        void openWindow(int port)
        {
            // GT4_TELEMETRY_MONITOR (shared/gt4_tele_monitors.h): <DISPLAYn>,<name>,<W>x<H>,<x>,<y> (HUD hand-off 8: the name and
            // place first, then the device name), an older launcher's rectangle "x,y,w,h", or <n> (1-based, Windows' order).
            const std::string want = envOr("GT4_TELEMETRY_MONITOR", "0");
            const gt4tele::Setting setting = gt4tele::parse(want);
            if (setting.kind == gt4tele::Setting::Off)
                return;
            int monitor = -1;
            RECT rect{};
            bool found = false;
            const std::vector<gt4tele::Monitor> mons = gt4tele::list();
            const int at = gt4tele::match(setting, mons);
            if (at >= 0)
            {
                const gt4tele::Monitor &m = mons[static_cast<size_t>(at)];
                rect = RECT{m.x, m.y, m.x + m.w, m.y + m.h};
                found = true;
                monitor = at + 1;
                std::fprintf(stderr, "[gt4-pages] telemetry window: monitor %d, %s (%s)\n", monitor, m.friendly.c_str(), m.device.c_str());
            }
            else if (setting.kind == gt4tele::Setting::Rect)
            {
                rect = RECT{setting.x, setting.y, setting.x + setting.w, setting.y + setting.h};
                found = true;
            }
            if (!found)
            {
                std::fprintf(stderr, "[gt4-pages] telemetry window: %s is not connected: no window\n", want.c_str());
                return;
            }
            // The page: the launcher's layouts (telemetry.html?screen=monitor, #custom; HUD hand-off 8), or an older launcher's
            // GT4_TELEMETRY_LAYOUT page (#timing-map, #mirror, ...) when there is no telemetry-layout.json.
            const std::string oldLayout = envOr("GT4_TELEMETRY_LAYOUT", "");
            const std::string layout = layoutPath().empty() && !oldLayout.empty() && oldLayout != "custom" ? "#" + oldLayout : std::string("?screen=monitor");
            // GT4_TELE_BG: the window's own background before the page draws (no white flash).
            const bool black = envOr("GT4_TELE_BG", "dark") == "black";
            std::string edge;
            for (const char *base : {"ProgramFiles(x86)", "ProgramFiles", "LOCALAPPDATA"})
            {
                const char *b = std::getenv(base);
                if (!b)
                    continue;
                const std::string p = std::string(b) + "\\Microsoft\\Edge\\Application\\msedge.exe";
                if (GetFileAttributesA(p.c_str()) != INVALID_FILE_ATTRIBUTES)
                {
                    edge = p;
                    break;
                }
            }
            if (edge.empty())
            {
                std::fprintf(stderr, "[gt4-pages] telemetry window: Microsoft Edge not found: open http://127.0.0.1:%d/telemetry.html in a browser\n", port);
                return;
            }
            const RECT &r = rect;
            char cmd[2048];
            std::snprintf(cmd, sizeof(cmd),
                          "\"%s\" --app=http://127.0.0.1:%d/telemetry.html%s --user-data-dir=\"%s\\edge-profile\" --no-first-run --no-default-browser-check "
                          "--default-background-color=%s --window-position=%ld,%ld --window-size=%ld,%ld --start-fullscreen",
                          edge.c_str(), port, layout.c_str(), g_dir.c_str(), black ? "ff000000" : "ff121920", r.left, r.top, r.right - r.left, r.bottom - r.top);
            STARTUPINFOA si{};
            si.cb = sizeof(si);
            si.dwFlags = STARTF_USESHOWWINDOW;
            si.wShowWindow = SW_SHOWNOACTIVATE;
            PROCESS_INFORMATION pi{};
            const HWND game = GetForegroundWindow();
            if (!CreateProcessA(nullptr, cmd, nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi))
            {
                std::fprintf(stderr, "[gt4-pages] telemetry window: Edge did not start (error %lu)\n", GetLastError());
                return;
            }
            CloseHandle(pi.hThread);
            g_window = pi.hProcess;
            std::fprintf(stderr, "[gt4-pages] telemetry window: monitor %d (%ld,%ld %ldx%ld), layout %s\n", monitor, r.left, r.top, r.right - r.left,
                         r.bottom - r.top, layout.c_str());
            // Edge's window (its first visible top-level window on that monitor): no activation, no taskbar button; the
            // game window back in front.
            std::thread([game, r]() {
                for (int tries = 0; tries < 100; ++tries)
                {
                    std::this_thread::sleep_for(std::chrono::milliseconds(100));
                    struct Ctx
                    {
                        RECT r;
                        HWND found;
                    } ctx{r, nullptr};
                    EnumWindows(
                        [](HWND h, LPARAM p) -> BOOL {
                            Ctx &c = *reinterpret_cast<Ctx *>(p);
                            char cls[64] = {};
                            GetClassNameA(h, cls, sizeof(cls));
                            if (!IsWindowVisible(h) || std::strncmp(cls, "Chrome_WidgetWin", 16) != 0)
                                return TRUE;
                            RECT w{};
                            GetWindowRect(h, &w);
                            const POINT centre{(w.left + w.right) / 2, (w.top + w.bottom) / 2};
                            if (PtInRect(&c.r, centre))
                            {
                                c.found = h;
                                return FALSE;
                            }
                            return TRUE;
                        },
                        reinterpret_cast<LPARAM>(&ctx));
                    if (ctx.found)
                    {
                        const LONG_PTR ex = GetWindowLongPtrA(ctx.found, GWL_EXSTYLE);
                        SetWindowLongPtrA(ctx.found, GWL_EXSTYLE, (ex | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW) & ~WS_EX_APPWINDOW);
                        if (game && IsWindow(game))
                            SetForegroundWindow(game);
                        std::fprintf(stderr, "[gt4-pages] telemetry window up (no activation)\n");
                        return;
                    }
                }
            }).detach();
        }
    } // namespace

    void start()
    {
        const std::string mode = envOr("GT4_PAGES", "off");
        if (mode != "local" && mode != "lan")
            return;
        g_dir = envOr("GT4_PAGES_DIR", "");
        if (g_dir.empty())
            g_dir = exeDir() + "\\pages";
        const int port = std::atoi(envOr("GT4_PAGES_PORT", "8770").c_str());
        WSADATA wsa{};
        if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0)
            return;
        SOCKET l = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (l == INVALID_SOCKET)
            return;
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_port = htons(static_cast<u_short>(port));
        a.sin_addr.s_addr = mode == "lan" ? htonl(INADDR_ANY) : htonl(INADDR_LOOPBACK);
        if (bind(l, reinterpret_cast<sockaddr *>(&a), sizeof(a)) != 0 || listen(l, 8) != 0)
        {
            std::fprintf(stderr, "[gt4-pages] port %d is in use or blocked (error %d): no pages\n", port, WSAGetLastError());
            closesocket(l);
            return;
        }
        std::fprintf(stderr, "[gt4-pages] pages from %s at http://%s:%d/telemetry.html (GT4_PAGES=%s)\n", g_dir.c_str(),
                     mode == "lan" ? "<this PC's address>" : "127.0.0.1", port, mode.c_str());
        std::thread(server, l).detach();
        openWindow(port);
    }

    void shutdown()
    {
        if (g_window)
        {
            TerminateProcess(g_window, 0);
            CloseHandle(g_window);
            g_window = nullptr;
        }
    }
} // namespace gt4pages

#else

namespace gt4pages
{
    void start() {}
    void shutdown() {}
} // namespace gt4pages

#endif
