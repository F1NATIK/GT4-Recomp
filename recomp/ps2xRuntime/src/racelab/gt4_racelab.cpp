// gt4RaceLab: GT Race Lab served on this PC for GT4 (the Full launcher's "Race Lab" button starts it). A copy of the GT3
// tree's gt3_racelab.cpp (the GT3 recomp, Race Lab hand-off 1): GT4's defaults (port 8095, recordings = the
// GT4_DRIVE_LOG folder the launcher passes, else <root>\racelab-laps; the recorder's whole-race files are not laps and
// are left out of the lap list). The GT3 notes below hold for GT4 with those names.
//
// The same page as the claude.ai one (GT-RACE-LAB\page\index.html, one file kept in the project) and its
// track files, with what the page saves kept as plain files in the game folder instead of claude.ai's
// store: <data>\<collection>\<document>.json (the layout an ArtifactData export writes, so GT4-WORK's
// tools - ai_cars_export.py --rows <data>, drive_log_upload.py ... - and a Claude Code session read and
// write them directly). A small script it adds in front of the page gives the page the store it uses on
// claude.ai (window.claude.use("db")): documents and collections over a little HTTP API.
//
// Only this PC: it listens on 127.0.0.1, answers only requests addressed to 127.0.0.1 / localhost on its
// port, and the API wants an X-RaceLab header (a page on another site can't send one without the browser
// asking first, which this never allows), so other sites open in the browser can't read or write it.
//
// gt3RaceLab.exe [--root <game folder>] [--page <folder of index.html>] [--tracks <folder>]
//                [--data <folder>] [--recordings <folder>] [--port <n>] [--parent <pid>] [--no-browser]
//   --root        the game folder (default: the working folder); data = <root>\racelab-data,
//                 recordings = <root>\recordings (where F10 lap recording writes)
//   --page        where index.html is (default <exe folder>\racelab, else <root>\racelab,
//                 else <root>\..\GT-RACE-LAB\page); tracks = <page>\tracks2, else <page>\tracks
//   --port        the first port to try (default 8094; the next ten if it is taken)
//   --parent      quit when that process ends (the launcher passes its own)
//   --no-browser  don't open the page in the browser (tests)
//   --all-tracks  the page lists the GT3 and converted tracks too (also GT4_RACELAB_TRACKS=all); without it, GT4's own only
// It prints "GT Race Lab: http://127.0.0.1:<port>/" once it listens.
//
// The AI cars: the page's "Send to AI cars" also writes the game's files for them (PUT /api/ailines?name=<file>.txt)
// into <data>\ai-lines, the folder the launcher gives the game as GT3_AI_LINES when "AI cars 1-5" is set to
// the Race Lab's (gt3_ai_pilot.h: <course>.txt, read at the start of each race, and the line files it names).

#define _SILENCE_CXX20_U8PATH_DEPRECATION_WARNING // (fs::u8path: plain UTF-8 strings to paths, still the simplest)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <shellapi.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace fs = std::filesystem;

namespace
{
    fs::path g_page, g_tracks, g_data, g_recordings;
    // GT4's own courses only (developer 2026-10-20: "remove GT3 and converted tracks from the launcher for now"): the page hides
    // the GT3 and converted tracks unless --all-tracks or GT4_RACELAB_TRACKS=all is given (the track files stay as they are).
    bool g_allTracks = false;
    int g_port = 0;
    std::mutex g_writeMutex; // one document write at a time

    // The claude.ai page wrapper (what claude.ai puts round a published page), then the store script.
    const char *kHead =
        "<!doctype html><html><head><meta charset=utf8><meta name=viewport content=\"width=device-width,initial-scale=1,viewport-fit=cover\">"
        "<style>:root{color-scheme:light;box-sizing:border-box;padding-top:env(safe-area-inset-top,0px);padding-bottom:env(safe-area-inset-bottom,0px)}"
        "html{scroll-padding-top:env(safe-area-inset-top,0px)}body{margin:0;padding:0;font:14px -apple-system,BlinkMacSystemFont,sans-serif;"
        "background:#faf9f5;color:#141413}img{max-width:100%}[hidden]:not([hidden=until-found i]){display:none!important}</style></head><body>\n"
        "<script src=\"/racelab-local.js\"></script>\n";
    const char *kTail = "\n</body></html>";

    // The page's store (as on claude.ai: db.doc(path).get / set / onSnapshot, db.collection(path).get /
    // where(field, "==", value).limit(n).get) over this program's API. The page's own diagnostics
    // (diag/<id>, for claude.ai) aren't kept.
    const char *kStoreJs = R"JS((function () {
  "use strict";
  // GT Race Lab served by the GT3 launcher (gt3RaceLab.exe): the page's store as files in the game folder
  const q = encodeURIComponent;
  const api = async (url, opts) => {
    const o = Object.assign({ cache: "no-store" }, opts || {});
    o.headers = Object.assign({ "X-RaceLab": "1" }, (opts && opts.headers) || {});
    const r = await fetch(url, o);
    if (!r.ok) { const e = new Error((await r.text()) || "HTTP " + r.status); e.code = "http_" + r.status; throw e; }
    return r;
  };
  const snap = (id, data, exists) => ({ id, exists, data: () => (data == null ? null : JSON.parse(JSON.stringify(data))) });
  function doc(path) {
    const id = path.split("/").pop();
    const get = async () => { const j = await (await api("/api/doc?path=" + q(path))).json(); return snap(id, j.data, !!j.exists); };
    return {
      get,
      set: async (v) => {
        if (/^diag\//.test(path)) return;
        await api("/api/doc?path=" + q(path), { method: "PUT", body: JSON.stringify(v), headers: { "Content-Type": "application/json" } });
      },
      onSnapshot: (cb, err) => {
        let on = true, last = null;
        const tick = async () => {
          if (!on) return;
          try { const s = await get(), k = JSON.stringify(s.data()); if (k !== last) { last = k; cb(s); } } catch (e) { if (err) err(e); }
          if (on) setTimeout(tick, 4000);
        };
        tick();
        return () => { on = false; };
      },
    };
  }
  function collection(path) {
    const all = async () => (await (await api("/api/coll?path=" + q(path))).json()).map((d) => snap(d.id, d.data, true));
    const query = (field, value, n) => ({
      get: async () => {
        let docs = await all();
        if (field != null) docs = docs.filter((d) => { const x = d.data(); return x && x[field] === value; });
        if (n) docs = docs.slice(0, n);
        return { docs, empty: !docs.length, size: docs.length };
      },
    });
    return {
      get: () => query(null).get(),
      limit: (n) => query(null, null, n),
      where: (field, op, value) => ({ get: () => query(field, value).get(), limit: (n) => query(field, value, n) }),
    };
  }
  window.claude = { local: true, game: "gt4", use: async (name) => (name === "db" ? { doc, collection } : null) };
})();
)JS";

    // ---------------------------------------------------------------- helpers
    // a path as UTF-8 text (C++20's u8string() is a u8string)
    std::string u8s(const fs::path &p)
    {
        const auto t = p.u8string();
        return std::string(t.begin(), t.end());
    }
    std::string readFile(const fs::path &p, bool &ok)
    {
        std::ifstream in(p, std::ios::binary);
        ok = static_cast<bool>(in);
        if (!ok)
            return {};
        std::ostringstream s;
        s << in.rdbuf();
        return s.str();
    }
    bool writeFileAtomic(const fs::path &p, const std::string &text)
    {
        std::error_code ec;
        fs::create_directories(p.parent_path(), ec);
        fs::path tmp = p;
        tmp += L".tmp";
        {
            std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
            if (!out)
                return false;
            out.write(text.data(), static_cast<std::streamsize>(text.size()));
            if (!out)
                return false;
        }
        return MoveFileExW(tmp.c_str(), p.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
    }
    std::string urlDecode(const std::string &s)
    {
        std::string out;
        for (size_t i = 0; i < s.size(); ++i)
        {
            if (s[i] == '%' && i + 2 < s.size() && std::isxdigit(static_cast<unsigned char>(s[i + 1])) && std::isxdigit(static_cast<unsigned char>(s[i + 2])))
            {
                out += static_cast<char>(std::stoi(s.substr(i + 1, 2), nullptr, 16));
                i += 2;
            }
            else
                out += s[i] == '+' ? ' ' : s[i];
        }
        return out;
    }
    std::string queryParam(const std::string &query, const std::string &name)
    {
        std::istringstream in(query);
        std::string pair;
        while (std::getline(in, pair, '&'))
        {
            const size_t eq = pair.find('=');
            if (eq != std::string::npos && pair.substr(0, eq) == name)
                return urlDecode(pair.substr(eq + 1));
        }
        return {};
    }
    std::string jsonString(const std::string &s)
    {
        std::string out = "\"";
        for (const char c : s)
        {
            if (c == '"' || c == '\\')
                out += '\\', out += c;
            else if (static_cast<unsigned char>(c) < 0x20)
            {
                char b[8];
                std::snprintf(b, sizeof(b), "\\u%04x", static_cast<unsigned char>(c));
                out += b;
            }
            else
                out += c;
        }
        return out + "\"";
    }
    // A store path: segments of letters, digits and _ - . ~ @ + (no "." / ".." segments); a document path has
    // an even number of them (collection/document[/collection/document...]), a collection path an odd one.
    bool storePath(const std::string &path, bool document, fs::path &out)
    {
        if (path.empty() || path.size() > 1000)
            return false;
        std::vector<std::string> parts;
        std::istringstream in(path);
        std::string part;
        while (std::getline(in, part, '/'))
        {
            if (part.empty() || part.size() > 200 || part == "." || part == "..")
                return false;
            for (const char c : part)
                if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '-' || c == '.' || c == '~' || c == '@' || c == '+'))
                    return false;
            parts.push_back(part);
        }
        if (parts.empty() || parts.size() > 15 || path.back() == '/' || (parts.size() % 2 == 0) != document)
            return false;
        out = g_data;
        for (size_t i = 0; i < parts.size(); ++i)
            out /= fs::u8path(i + 1 == parts.size() && document ? parts[i] + ".json" : parts[i]);
        return true;
    }
    bool plainName(const std::string &name, const char *ext)
    {
        if (name.empty() || name.size() > 200 || name[0] == '.')
            return false;
        for (const char c : name)
            if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '-' || c == '.'))
                return false;
        const std::string e = ext;
        return name.size() > e.size() && name.compare(name.size() - e.size(), e.size(), e) == 0;
    }

    // ---------------------------------------------------------------- HTTP
    struct Request
    {
        std::string method, path, query, host, body;
        bool labHeader = false;
    };
    void sendAll(SOCKET s, const std::string &data)
    {
        size_t sent = 0;
        while (sent < data.size())
        {
            const int n = send(s, data.data() + sent, static_cast<int>(std::min<size_t>(data.size() - sent, 1 << 20)), 0);
            if (n <= 0)
                return;
            sent += static_cast<size_t>(n);
        }
    }
    void respond(SOCKET s, int code, const char *type, const std::string &body, bool noStore = true)
    {
        const char *reason = code == 200 ? "OK" : code == 204 ? "No Content" : code == 400 ? "Bad Request" : code == 403 ? "Forbidden"
                           : code == 404 ? "Not Found" : code == 405 ? "Method Not Allowed" : code == 413 ? "Payload Too Large" : "Server Error";
        std::string head = "HTTP/1.1 " + std::to_string(code) + " " + reason + "\r\nContent-Type: " + type +
                           "\r\nContent-Length: " + std::to_string(body.size()) + "\r\nConnection: close\r\nX-Content-Type-Options: nosniff\r\n" +
                           (noStore ? "Cache-Control: no-store\r\n" : "Cache-Control: max-age=60\r\n") + "\r\n";
        sendAll(s, head);
        sendAll(s, body);
    }
    bool readRequest(SOCKET s, Request &req)
    {
        std::string data;
        char buf[65536];
        size_t headEnd = std::string::npos;
        while (headEnd == std::string::npos)
        {
            const int n = recv(s, buf, sizeof(buf), 0);
            if (n <= 0 || data.size() > 64 * 1024)
                return false;
            data.append(buf, static_cast<size_t>(n));
            headEnd = data.find("\r\n\r\n");
        }
        std::istringstream head(data.substr(0, headEnd));
        std::string line, target, version;
        std::getline(head, line);
        std::istringstream first(line);
        first >> req.method >> target >> version;
        const size_t qm = target.find('?');
        req.path = target.substr(0, qm);
        req.query = qm == std::string::npos ? "" : target.substr(qm + 1);
        size_t length = 0;
        while (std::getline(head, line))
        {
            if (!line.empty() && line.back() == '\r')
                line.pop_back();
            const size_t colon = line.find(':');
            if (colon == std::string::npos)
                continue;
            std::string key = line.substr(0, colon), value = line.substr(colon + 1);
            std::transform(key.begin(), key.end(), key.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            value.erase(0, value.find_first_not_of(" \t"));
            if (key == "host")
                req.host = value;
            else if (key == "content-length")
                length = static_cast<size_t>(std::strtoull(value.c_str(), nullptr, 10));
            else if (key == "x-racelab")
                req.labHeader = value == "1";
        }
        if (length > 64u * 1024 * 1024)
            return false;
        req.body = data.substr(headEnd + 4);
        while (req.body.size() < length)
        {
            const int n = recv(s, buf, sizeof(buf), 0);
            if (n <= 0)
                return false;
            req.body.append(buf, static_cast<size_t>(n));
        }
        req.body.resize(length);
        return true;
    }

    void handle(SOCKET s)
    {
        Request req;
        if (!readRequest(s, req))
            return;
        // only requests addressed to this PC on this port (a site that points its own name at 127.0.0.1 is refused)
        const std::string port = std::to_string(g_port);
        if (req.host != "127.0.0.1:" + port && req.host != "localhost:" + port)
            return respond(s, 403, "text/plain", "This page is for this PC only.");
        if (req.path == "/" || req.path == "/index.html")
        {
            bool ok;
            const std::string page = readFile(g_page / L"index.html", ok);
            if (!ok)
                return respond(s, 404, "text/plain", "The Race Lab page isn't there: " + u8s(g_page) + "\\index.html");
            const char *all = g_allTracks ? "<script>window.claude.allTracks = true;</script>\n" : "";
            return respond(s, 200, "text/html; charset=utf-8", std::string(kHead) + all + page + kTail);
        }
        if (req.path == "/racelab-local.js")
            return respond(s, 200, "text/javascript; charset=utf-8", kStoreJs);
        if (req.path.rfind("/tracks/", 0) == 0)
        {
            const std::string name = urlDecode(req.path.substr(8));
            if (!plainName(name, ".json"))
                return respond(s, 404, "text/plain", "no such track file");
            bool ok;
            const std::string text = readFile(g_tracks / fs::u8path(name), ok);
            return ok ? respond(s, 200, "application/json", text, false) : respond(s, 404, "text/plain", "no such track file");
        }
        if (req.path.rfind("/api/", 0) != 0)
            return respond(s, 404, "text/plain", "not found");
        if (!req.labHeader)
            return respond(s, 403, "text/plain", "missing X-RaceLab header");
        if (req.path == "/api/info")
            return respond(s, 200, "application/json",
                           "{\"local\":true,\"game\":\"gt4\",\"data\":" + jsonString(u8s(g_data)) + ",\"recordings\":" + jsonString(u8s(g_recordings)) + "}");
        if (req.path == "/api/doc")
        {
            fs::path file;
            if (!storePath(queryParam(req.query, "path"), true, file))
                return respond(s, 400, "text/plain", "bad document path");
            if (req.method == "GET")
            {
                bool ok;
                const std::string text = readFile(file, ok);
                return respond(s, 200, "application/json", ok && !text.empty() && text[0] == '{' ? "{\"exists\":true,\"data\":" + text + "}" : "{\"exists\":false}");
            }
            if (req.method == "PUT")
            {
                if (req.body.empty() || req.body[0] != '{')
                    return respond(s, 400, "text/plain", "a document is a JSON object");
                std::lock_guard<std::mutex> lock(g_writeMutex);
                return writeFileAtomic(file, req.body) ? respond(s, 204, "text/plain", "") : respond(s, 500, "text/plain", "couldn't write " + u8s(file));
            }
            return respond(s, 405, "text/plain", "GET or PUT");
        }
        if (req.path == "/api/coll" && req.method == "GET")
        {
            fs::path dir;
            if (!storePath(queryParam(req.query, "path"), false, dir))
                return respond(s, 400, "text/plain", "bad collection path");
            std::vector<fs::path> files;
            std::error_code ec;
            for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec))
                if (it->is_regular_file(ec) && it->path().extension() == L".json")
                    files.push_back(it->path());
            std::sort(files.begin(), files.end());
            std::string out = "[";
            for (const fs::path &f : files)
            {
                bool ok;
                const std::string text = readFile(f, ok);
                if (!ok || text.empty() || text[0] != '{')
                    continue;
                out += (out.size() > 1 ? "," : "") + std::string("{\"id\":") + jsonString(u8s(f.stem())) + ",\"data\":" + text + "}";
            }
            return respond(s, 200, "application/json", out + "]");
        }
        if (req.path == "/api/ailines" && req.method == "PUT")
        {
            const std::string name = queryParam(req.query, "name");
            if (!plainName(name, ".txt"))
                return respond(s, 400, "text/plain", "bad file name");
            // (GT4: gt4-drivers.txt, "gt4-ai-drivers 1": the AI cars' drivers' names, gt4_ai_drivers.h)
            if (req.body.rfind("gt3-ai-cars ", 0) != 0 && req.body.rfind("gt3-autopilot-line ", 0) != 0 && req.body.rfind("gt4-ai-drivers ", 0) != 0)
                return respond(s, 400, "text/plain", "not an AI cars, drivers or line file");
            const fs::path file = g_data / L"ai-lines" / fs::u8path(name);
            std::lock_guard<std::mutex> lock(g_writeMutex);
            return writeFileAtomic(file, req.body) ? respond(s, 204, "text/plain", "") : respond(s, 500, "text/plain", "couldn't write " + u8s(file));
        }
        if (req.path == "/api/recordings" && req.method == "GET")
        {
            // the lap files the game's recorder wrote (F10), newest first
            struct Item
            {
                std::string name;
                uintmax_t size;
                fs::file_time_type when;
            };
            std::vector<Item> items;
            std::error_code ec;
            for (fs::directory_iterator it(g_recordings, ec), end; !ec && it != end; it.increment(ec))
                if (it->is_regular_file(ec) && it->path().extension() == L".csv" &&
                    !u8s(it->path().filename()).ends_with("-race.csv")) // (GT4: the whole-race files are not laps)
                    items.push_back({u8s(it->path().filename()), it->file_size(ec), it->last_write_time(ec)});
            std::sort(items.begin(), items.end(), [](const Item &a, const Item &b) { return a.when > b.when; });
            std::string out = "[";
            for (const Item &i : items)
                out += (out.size() > 1 ? "," : "") + std::string("{\"name\":") + jsonString(i.name) + ",\"size\":" + std::to_string(i.size) + "}";
            return respond(s, 200, "application/json", out + "]");
        }
        if (req.path == "/api/racecars" && req.method == "GET")
        {
            // GT4: the newest race's cars (the recorder's <...>-race-cars.txt: who drove which car, gt4_drive_log.cpp):
            // {"file", "cars":[{"index","who" you|ai<k>,"driver","car"}]}; {"cars":[]} before the first recorded race
            fs::path newest;
            fs::file_time_type when{};
            std::error_code ec;
            for (fs::directory_iterator it(g_recordings, ec), end; !ec && it != end; it.increment(ec))
                if (it->is_regular_file(ec) && u8s(it->path().filename()).ends_with("-race-cars.txt") && (newest.empty() || it->last_write_time(ec) > when))
                    newest = it->path(), when = it->last_write_time(ec);
            if (newest.empty())
                return respond(s, 200, "application/json", "{\"cars\":[]}");
            bool ok;
            std::istringstream in(readFile(newest, ok));
            std::string line, out;
            while (std::getline(in, line))
            {
                if (!line.empty() && line.back() == '\r')
                    line.pop_back();
                if (line.rfind("car ", 0) != 0)
                    continue;
                std::vector<std::string> f;
                std::istringstream ls(line.substr(4));
                for (std::string x; std::getline(ls, x, '\t');)
                    f.push_back(x);
                if (f.size() < 4)
                    continue;
                out += (out.empty() ? "" : ",") + std::string("{\"index\":") + std::to_string(std::atoi(f[0].c_str())) + ",\"who\":" + jsonString(f[1]) +
                       ",\"driver\":" + jsonString(f[2]) + ",\"car\":" + jsonString(f[3]) + "}";
            }
            return respond(s, 200, "application/json", "{\"file\":" + jsonString(u8s(newest.filename())) + ",\"cars\":[" + out + "]}");
        }
        if (req.path == "/api/recording" && req.method == "GET")
        {
            const std::string name = queryParam(req.query, "name");
            if (!plainName(name, ".csv"))
                return respond(s, 400, "text/plain", "bad file name");
            bool ok;
            const std::string text = readFile(g_recordings / fs::u8path(name), ok);
            return ok ? respond(s, 200, "text/csv; charset=utf-8", text) : respond(s, 404, "text/plain", "no such recording");
        }
        respond(s, 404, "text/plain", "not found");
    }

    std::string arg(int argc, char **argv, const char *name, const std::string &fallback = {})
    {
        for (int i = 1; i + 1 < argc; ++i)
            if (std::string(argv[i]) == name)
                return argv[i + 1];
        return fallback;
    }
    bool flag(int argc, char **argv, const char *name)
    {
        for (int i = 1; i < argc; ++i)
            if (std::string(argv[i]) == name)
                return true;
        return false;
    }
    fs::path exeFolder()
    {
        wchar_t buf[MAX_PATH];
        const DWORD n = GetModuleFileNameW(nullptr, buf, MAX_PATH);
        return fs::path(std::wstring(buf, n)).parent_path();
    }
}

int main(int argc, char **argv)
{
    std::error_code ec;
    const fs::path root = fs::absolute(fs::u8path(arg(argc, argv, "--root", ".")), ec);
    // the page: given, else next to this program, in the game folder, or the project's copy (development)
    std::string page = arg(argc, argv, "--page");
    if (page.empty())
        for (const fs::path &p : {exeFolder() / L"racelab", root / L"racelab", root / L".." / L"GT-RACE-LAB" / L"page"})
            if (fs::exists(p / L"index.html", ec))
            {
                page = u8s(p);
                break;
            }
    if (page.empty())
    {
        std::fprintf(stderr, "GT Race Lab: no page found (give --page <folder of index.html>)\n");
        return 2;
    }
    g_page = fs::weakly_canonical(fs::u8path(page), ec);
    const std::string tracks = arg(argc, argv, "--tracks");
    g_tracks = !tracks.empty() ? fs::u8path(tracks) : fs::exists(g_page / L"tracks2" / L"index.json", ec) ? g_page / L"tracks2" : g_page / L"tracks";
    g_data = fs::absolute(fs::u8path(arg(argc, argv, "--data", u8s(root / L"racelab-data"))), ec);
    g_recordings = fs::absolute(fs::u8path(arg(argc, argv, "--recordings", u8s(root / L"racelab-laps"))), ec);
    fs::create_directories(g_data, ec);
    {
        const char *e = std::getenv("GT4_RACELAB_TRACKS");
        std::string v = e ? e : "";
        for (char &c : v)
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        g_allTracks = flag(argc, argv, "--all-tracks") || v == "all";
    }

    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0)
        return 3;
    const int first = std::atoi(arg(argc, argv, "--port", "8095").c_str()); // (GT3's is 8094)
    SOCKET listener = INVALID_SOCKET;
    for (int p = first; p < first + 10 && listener == INVALID_SOCKET; ++p)
    {
        SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        BOOL exclusive = TRUE;
        setsockopt(s, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, reinterpret_cast<const char *>(&exclusive), sizeof(exclusive));
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(static_cast<u_short>(p));
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK); // this PC only
        if (bind(s, reinterpret_cast<const sockaddr *>(&addr), sizeof(addr)) == 0 && listen(s, 16) == 0)
        {
            listener = s;
            g_port = p;
        }
        else
            closesocket(s);
    }
    if (listener == INVALID_SOCKET)
    {
        std::fprintf(stderr, "GT Race Lab: ports %d-%d are all taken\n", first, first + 9);
        return 4;
    }
    const std::string url = "http://127.0.0.1:" + std::to_string(g_port) + "/";
    std::printf("GT Race Lab: %s\n  page %s\n  tracks %s (%s)\n  data %s\n  recordings %s\n", url.c_str(), u8s(g_page).c_str(),
                u8s(g_tracks).c_str(), g_allTracks ? "all: GT3 and converted too" : "GT4's own only; --all-tracks shows the GT3 and converted ones",
                u8s(g_data).c_str(), u8s(g_recordings).c_str());
    std::fflush(stdout);

    // with the launcher: gone when it is
    const DWORD parent = static_cast<DWORD>(std::strtoul(arg(argc, argv, "--parent", "0").c_str(), nullptr, 10));
    if (parent)
        if (HANDLE h = OpenProcess(SYNCHRONIZE, FALSE, parent))
            std::thread([h] {
                WaitForSingleObject(h, INFINITE);
                ExitProcess(0);
            }).detach();
    if (!flag(argc, argv, "--no-browser"))
        ShellExecuteA(nullptr, "open", url.c_str(), nullptr, nullptr, SW_SHOWNORMAL);

    for (;;)
    {
        SOCKET c = accept(listener, nullptr, nullptr);
        if (c == INVALID_SOCKET)
            continue;
        std::thread([c] {
            handle(c);
            shutdown(c, SD_SEND);
            closesocket(c);
        }).detach();
    }
}
