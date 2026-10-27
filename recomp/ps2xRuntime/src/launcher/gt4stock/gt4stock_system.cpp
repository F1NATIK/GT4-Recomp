#include "gt4stock_system.h"

#include "gt4stock_util.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cwctype>

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <objbase.h> // STDMETHOD: commdlg.h declares PRINTDLGEX (the print window) only with it
#include <commdlg.h>
#include <shellapi.h>
#include <shobjidl.h>
#include <tlhelp32.h>
#include <dxgi1_6.h>
#if defined(_MSC_VER)
#pragma comment(lib, "comdlg32.lib")
#pragma comment(lib, "shell32.lib")
#endif

namespace gt4stock::sys
{
    namespace
    {
        bool g_noShell = false;
    }
    void setNoShell(bool on) { g_noShell = on; }

    fs::path exePath()
    {
        std::wstring buf(32768, L'\0');
        const DWORD n = GetModuleFileNameW(nullptr, buf.data(), static_cast<DWORD>(buf.size()));
        buf.resize(n);
        return fs::path(buf);
    }

    std::vector<std::wstring> arguments()
    {
        std::vector<std::wstring> out;
        int n = 0;
        if (wchar_t **argv = CommandLineToArgvW(GetCommandLineW(), &n))
        {
            for (int i = 0; i < n; ++i)
                out.emplace_back(argv[i]);
            LocalFree(argv);
        }
        return out;
    }

    std::vector<uint8_t> resource(int id)
    {
        HRSRC res = FindResourceW(nullptr, MAKEINTRESOURCEW(id), MAKEINTRESOURCEW(10)); // RT_RCDATA
        if (!res)
            return {};
        HGLOBAL h = LoadResource(nullptr, res);
        const DWORD size = SizeofResource(nullptr, res);
        const void *data = h ? LockResource(h) : nullptr;
        if (!data || !size)
            return {};
        const auto *p = static_cast<const uint8_t *>(data);
        return std::vector<uint8_t>(p, p + size);
    }

    bool openInExplorer(const fs::path &path)
    {
        if (g_noShell)
            return false;
        const HINSTANCE r = ShellExecuteW(nullptr, L"open", path.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        return reinterpret_cast<INT_PTR>(r) > 32;
    }

    fs::path browseForFile(void *owner, const std::wstring &title, const std::wstring &filter, const fs::path &start, bool mustExist)
    {
        if (g_noShell)
            return {};
        std::wstring file(32768, L'\0');
        std::wstring dir;
        std::error_code ec;
        if (!start.empty())
        {
            if (fs::is_regular_file(start, ec))
            {
                const std::wstring name = start.filename().wstring();
                std::copy(name.begin(), name.end(), file.begin());
                dir = start.parent_path().wstring();
            }
            else if (fs::is_directory(start, ec))
                dir = start.wstring();
        }
        OPENFILENAMEW ofn{};
        ofn.lStructSize = sizeof(ofn);
        ofn.hwndOwner = static_cast<HWND>(owner);
        ofn.lpstrFilter = filter.c_str();
        ofn.lpstrFile = file.data();
        ofn.nMaxFile = static_cast<DWORD>(file.size());
        ofn.lpstrInitialDir = dir.empty() ? nullptr : dir.c_str();
        ofn.lpstrTitle = title.c_str();
        ofn.Flags = (mustExist ? OFN_FILEMUSTEXIST : 0) | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR | OFN_EXPLORER | OFN_HIDEREADONLY;
        if (!GetOpenFileNameW(&ofn))
            return {};
        file.resize(wcslen(file.c_str()));
        return fs::path(file);
    }

    fs::path browseForSaveFile(void *owner, const std::wstring &title, const std::wstring &filter, const fs::path &suggested,
                               const std::wstring &extension)
    {
        if (g_noShell)
            return {};
        std::wstring file(32768, L'\0');
        const std::wstring name = suggested.filename().wstring();
        std::copy(name.begin(), name.end(), file.begin());
        const std::wstring dir = suggested.parent_path().wstring();
        OPENFILENAMEW ofn{};
        ofn.lStructSize = sizeof(ofn);
        ofn.hwndOwner = static_cast<HWND>(owner);
        ofn.lpstrFilter = filter.c_str();
        ofn.lpstrFile = file.data();
        ofn.nMaxFile = static_cast<DWORD>(file.size());
        ofn.lpstrInitialDir = dir.empty() ? nullptr : dir.c_str();
        ofn.lpstrTitle = title.c_str();
        ofn.lpstrDefExt = extension.empty() ? nullptr : extension.c_str();
        ofn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR | OFN_EXPLORER | OFN_HIDEREADONLY;
        if (!GetSaveFileNameW(&ofn))
            return {};
        file.resize(wcslen(file.c_str()));
        return fs::path(file);
    }

    bool printPages(void *owner, const std::wstring &documentName, const std::vector<PrintPage> &pages, std::string &message)
    {
        if (g_noShell)
        {
            message = "Printing is off in the test mode.";
            return false;
        }
        if (pages.empty())
        {
            message = "Nothing to print: tick at least one card.";
            return false;
        }
        PRINTPAGERANGE range{1, static_cast<DWORD>(pages.size())};
        PRINTDLGEXW pd{};
        pd.lStructSize = sizeof(pd);
        pd.hwndOwner = static_cast<HWND>(owner);
        pd.Flags = PD_RETURNDC | PD_NOSELECTION | PD_NOCURRENTPAGE | PD_USEDEVMODECOPIESANDCOLLATE;
        pd.nPageRanges = pages.size() > 1 ? 1 : 0;
        pd.nMaxPageRanges = 1;
        pd.lpPageRanges = &range;
        pd.nMinPage = 1;
        pd.nMaxPage = static_cast<DWORD>(pages.size());
        pd.nCopies = 1;
        pd.nStartPage = START_PAGE_GENERAL;
        if (pages.size() <= 1)
            pd.Flags |= PD_NOPAGENUMS;
        const HRESULT hr = PrintDlgExW(&pd);
        auto release = [&] {
            if (pd.hDevMode)
                GlobalFree(pd.hDevMode);
            if (pd.hDevNames)
                GlobalFree(pd.hDevNames);
            if (pd.hDC)
                DeleteDC(pd.hDC);
        };
        if (FAILED(hr) || pd.dwResultAction != PD_RESULT_PRINT || !pd.hDC)
        {
            release();
            message = FAILED(hr) ? "Windows' print window could not open." : "Printing cancelled.";
            return false;
        }
        HDC dc = pd.hDC;
        DOCINFOW di{};
        di.cbSize = sizeof(di);
        di.lpszDocName = documentName.c_str();
        size_t first = 0, last = pages.size();
        if ((pd.Flags & PD_PAGENUMS) && pd.nPageRanges > 0)
        {
            first = std::clamp<size_t>(range.nFromPage, 1, pages.size()) - 1;
            last = std::clamp<size_t>(range.nToPage, first + 1, pages.size());
        }
        bool ok = StartDocW(dc, &di) > 0;
        const int pageW = GetDeviceCaps(dc, HORZRES), pageH = GetDeviceCaps(dc, VERTRES);
        for (size_t i = first; ok && i < last; ++i)
        {
            const PrintPage &p = pages[i];
            // A DIB: BGR rows, each padded to 4 bytes; a negative height means top row first.
            const size_t stride = (static_cast<size_t>(p.width) * 3u + 3u) & ~size_t(3);
            std::vector<uint8_t> dib(stride * static_cast<size_t>(p.height));
            for (int y = 0; y < p.height; ++y)
            {
                const uint8_t *src = p.rgb + static_cast<size_t>(y) * static_cast<size_t>(p.width) * 3u;
                uint8_t *dst = dib.data() + static_cast<size_t>(y) * stride;
                for (int x = 0; x < p.width; ++x)
                {
                    dst[x * 3 + 0] = src[x * 3 + 2];
                    dst[x * 3 + 1] = src[x * 3 + 1];
                    dst[x * 3 + 2] = src[x * 3 + 0];
                }
            }
            BITMAPINFO bi{};
            bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
            bi.bmiHeader.biWidth = p.width;
            bi.bmiHeader.biHeight = -p.height;
            bi.bmiHeader.biPlanes = 1;
            bi.bmiHeader.biBitCount = 24;
            bi.bmiHeader.biCompression = BI_RGB;
            const double scale = std::min(static_cast<double>(pageW) / p.width, static_cast<double>(pageH) / p.height);
            const int w = static_cast<int>(p.width * scale), h = static_cast<int>(p.height * scale);
            ok = StartPage(dc) > 0;
            if (ok)
            {
                SetStretchBltMode(dc, HALFTONE);
                SetBrushOrgEx(dc, 0, 0, nullptr);
                ok = StretchDIBits(dc, (pageW - w) / 2, (pageH - h) / 2, w, h, 0, 0, p.width, p.height, dib.data(), &bi, DIB_RGB_COLORS, SRCCOPY) != 0 &&
                     EndPage(dc) > 0;
            }
        }
        if (ok)
            ok = EndDoc(dc) > 0;
        else
            AbortDoc(dc);
        release();
        message = ok ? strf("Sent the cheat sheet to the printer (%zu page%s).", last - first, last - first == 1 ? "" : "s") : "Printing failed.";
        return ok;
    }

    fs::path browseForFolder(void *owner, const std::wstring &title, const fs::path &start)
    {
        if (g_noShell)
            return {};
        fs::path result;
        const HRESULT init = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
        IFileOpenDialog *dlg = nullptr;
        if (SUCCEEDED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dlg))) && dlg)
        {
            DWORD options = 0;
            dlg->GetOptions(&options);
            dlg->SetOptions(options | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST);
            dlg->SetTitle(title.c_str());
            std::error_code ec;
            if (!start.empty() && fs::is_directory(start, ec))
            {
                IShellItem *folder = nullptr;
                if (SUCCEEDED(SHCreateItemFromParsingName(start.c_str(), nullptr, IID_PPV_ARGS(&folder))) && folder)
                {
                    dlg->SetFolder(folder);
                    folder->Release();
                }
            }
            if (SUCCEEDED(dlg->Show(static_cast<HWND>(owner))))
            {
                IShellItem *item = nullptr;
                if (SUCCEEDED(dlg->GetResult(&item)) && item)
                {
                    PWSTR path = nullptr;
                    if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path)) && path)
                    {
                        result = fs::path(path);
                        CoTaskMemFree(path);
                    }
                    item->Release();
                }
            }
            dlg->Release();
        }
        if (SUCCEEDED(init))
            CoUninitialize();
        return result;
    }

    namespace
    {
        std::wstring quoteArg(const std::wstring &a)
        {
            if (!a.empty() && a.find_first_of(L" \t\"") == std::wstring::npos)
                return a;
            std::wstring out = L"\"";
            size_t backslashes = 0;
            for (wchar_t c : a)
            {
                if (c == L'\\')
                {
                    ++backslashes;
                    continue;
                }
                if (c == L'"')
                    out.append(backslashes * 2 + 1, L'\\');
                else
                    out.append(backslashes, L'\\');
                backslashes = 0;
                out += c;
            }
            out.append(backslashes * 2, L'\\');
            out += L'"';
            return out;
        }

        bool lessNoCase(const std::wstring &a, const std::wstring &b)
        {
            const size_t n = std::min(a.size(), b.size());
            for (size_t i = 0; i < n; ++i)
            {
                const wchar_t x = static_cast<wchar_t>(std::towupper(a[i])), y = static_cast<wchar_t>(std::towupper(b[i]));
                if (x != y)
                    return x < y;
            }
            return a.size() < b.size();
        }

        std::vector<std::wstring> buildEnvironment(const Launch &l)
        {
            std::vector<std::wstring> vars;
            if (wchar_t *block = GetEnvironmentStringsW())
            {
                for (const wchar_t *p = block; *p; p += wcslen(p) + 1)
                {
                    const std::wstring v = p;
                    const size_t eq = v.find(L'=', 1); // "=C:=C:\" entries keep their leading '='
                    const std::string name = utf8(v.substr(0, eq == std::wstring::npos ? v.size() : eq));
                    bool strip = false;
                    for (const std::string &prefix : l.stripPrefixes)
                        if (startsWith(upper(name), prefix))
                            strip = true;
                    if (!strip)
                        vars.push_back(v);
                }
                FreeEnvironmentStringsW(block);
            }
            for (const auto &[k, v] : l.env)
            {
                const std::wstring key = wide(k);
                vars.erase(std::remove_if(vars.begin(), vars.end(),
                                          [&](const std::wstring &e) {
                                              const size_t eq = e.find(L'=', 1);
                                              return !lessNoCase(e.substr(0, eq), key) && !lessNoCase(key, e.substr(0, eq));
                                          }),
                           vars.end());
                vars.push_back(key + L"=" + wide(v));
            }
            std::stable_sort(vars.begin(), vars.end(), [](const std::wstring &a, const std::wstring &b) {
                const bool da = !a.empty() && a[0] == L'=', db = !b.empty() && b[0] == L'=';
                if (da != db)
                    return da;
                return lessNoCase(a.substr(0, a.find(L'=', 1)), b.substr(0, b.find(L'=', 1)));
            });
            return vars;
        }
    }

    std::wstring commandLine(const Launch &l)
    {
        std::wstring cmd = quoteArg(l.exe.wstring());
        for (const std::wstring &a : l.args)
            cmd += L" " + quoteArg(a);
        return cmd;
    }

    std::vector<std::string> environmentFor(const Launch &l)
    {
        std::vector<std::string> out;
        for (const std::wstring &v : buildEnvironment(l))
            out.push_back(utf8(v));
        return out;
    }

    uint64_t tickMs() { return GetTickCount64(); }

    bool start(const Launch &l, Process &p, std::string &error)
    {
        if (g_noShell)
        {
            error = "not in the hidden test mode";
            return false;
        }
        std::vector<std::wstring> vars = buildEnvironment(l);
        std::wstring block;
        for (const std::wstring &v : vars)
        {
            block += v;
            block += L'\0';
        }
        block += L'\0';
        std::wstring cmd = commandLine(l);
        STARTUPINFOW si{};
        si.cb = sizeof(si);
        HANDLE log = INVALID_HANDLE_VALUE;
        BOOL inherit = FALSE;
        if (!l.log.empty())
        {
            std::error_code ec;
            fs::create_directories(l.log.parent_path(), ec);
            SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
            log = CreateFileW(l.log.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (log != INVALID_HANDLE_VALUE)
            {
                si.dwFlags |= STARTF_USESTDHANDLES;
                si.hStdOutput = log;
                si.hStdError = log;
                si.hStdInput = nullptr;
                inherit = TRUE;
            }
        }
        PROCESS_INFORMATION pi{};
        const BOOL ok = CreateProcessW(l.exe.c_str(), cmd.data(), nullptr, nullptr, inherit, CREATE_UNICODE_ENVIRONMENT | CREATE_NO_WINDOW,
                                       block.data(), l.cwd.empty() ? nullptr : l.cwd.c_str(), &si, &pi);
        if (log != INVALID_HANDLE_VALUE)
            CloseHandle(log);
        if (!ok)
        {
            error = strf("could not start %s (Windows error %lu)", pathText(l.exe).c_str(), GetLastError());
            return false;
        }
        CloseHandle(pi.hThread);
        p.handle = pi.hProcess;
        p.pid = pi.dwProcessId;
        p.startMs = GetTickCount64();
        p.closeSent = false;
        return true;
    }

    bool poll(Process &p, int &exitCode)
    {
        if (!p.handle)
            return false;
        if (WaitForSingleObject(static_cast<HANDLE>(p.handle), 0) != WAIT_OBJECT_0)
            return false;
        DWORD code = 0;
        GetExitCodeProcess(static_cast<HANDLE>(p.handle), &code);
        CloseHandle(static_cast<HANDLE>(p.handle));
        p.handle = nullptr;
        exitCode = static_cast<int>(code);
        return true;
    }

    namespace
    {
        struct CloseContext
        {
            DWORD pid;
            int sent;
        };
        BOOL CALLBACK closeWindow(HWND hwnd, LPARAM lp)
        {
            auto *ctx = reinterpret_cast<CloseContext *>(lp);
            DWORD pid = 0;
            GetWindowThreadProcessId(hwnd, &pid);
            if (pid == ctx->pid && IsWindowVisible(hwnd))
            {
                PostMessageW(hwnd, WM_CLOSE, 0, 0);
                ++ctx->sent;
            }
            return TRUE;
        }
        struct VisibleContext
        {
            DWORD pid;
            bool visible;
        };
        BOOL CALLBACK visibleWindow(HWND hwnd, LPARAM lp)
        {
            auto *ctx = reinterpret_cast<VisibleContext *>(lp);
            DWORD pid = 0;
            GetWindowThreadProcessId(hwnd, &pid);
            if (pid == ctx->pid && IsWindowVisible(hwnd))
                ctx->visible = true;
            return TRUE;
        }
    }

    void requestClose(Process &p)
    {
        if (!p.handle)
            return;
        CloseContext ctx{p.pid, 0};
        EnumWindows(closeWindow, reinterpret_cast<LPARAM>(&ctx));
        p.closeSent = true;
        p.closeSentMs = GetTickCount64();
    }

    void terminate(Process &p)
    {
        if (p.handle)
            TerminateProcess(static_cast<HANDLE>(p.handle), 1);
    }

    bool gameRunning(const std::vector<std::wstring> &exeNames)
    {
        HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
        if (snap == INVALID_HANDLE_VALUE)
            return false;
        PROCESSENTRY32W e{};
        e.dwSize = sizeof(e);
        bool found = false;
        // A GT4 started by the project's hidden test tools (from the build folder recomp\out or a temp folder) is not the
        // player's: it neither blocks Play nor greys the page (developer, 2026-10-20: Play said "Another GT4 is already running"
        // while a test ran).
        auto testCopy = [](DWORD pid) {
            bool test = false;
            if (HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid))
            {
                wchar_t path[MAX_PATH * 2] = {};
                DWORD len = static_cast<DWORD>(std::size(path));
                if (QueryFullProcessImageNameW(h, 0, path, &len))
                {
                    std::wstring p(path, len);
                    for (wchar_t &c : p)
                        c = static_cast<wchar_t>(towlower(c));
                    test = p.find(L"\\recomp\\out\\") != std::wstring::npos || p.find(L"\\appdata\\local\\temp\\") != std::wstring::npos;
                }
                CloseHandle(h);
            }
            return test;
        };
        for (BOOL ok = Process32FirstW(snap, &e); ok && !found; ok = Process32NextW(snap, &e))
            for (const std::wstring &n : exeNames)
                if (_wcsicmp(e.szExeFile, n.c_str()) == 0 && !testCopy(e.th32ProcessID))
                    found = true;
        CloseHandle(snap);
        return found;
    }

    void attachParentConsole()
    {
        // Output already redirected (a pipe or a file from the caller): keep it.
        const HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
        if (out && out != INVALID_HANDLE_VALUE && GetFileType(out) != FILE_TYPE_UNKNOWN)
            return;
        if (AttachConsole(ATTACH_PARENT_PROCESS))
        {
            FILE *f = nullptr;
            freopen_s(&f, "CONOUT$", "w", stdout);
            freopen_s(&f, "CONOUT$", "w", stderr);
        }
    }

    bool anyVisibleWindow()
    {
        VisibleContext ctx{GetCurrentProcessId(), false};
        EnumWindows(visibleWindow, reinterpret_cast<LPARAM>(&ctx));
        return ctx.visible;
    }

    std::string computerName()
    {
        wchar_t buf[MAX_COMPUTERNAME_LENGTH + 1] = {};
        DWORD n = MAX_COMPUTERNAME_LENGTH + 1;
        if (!GetComputerNameW(buf, &n))
            return {};
        return utf8(std::wstring(buf, n));
    }

    std::string GpuChoice::label() const { return game.empty() ? std::string("chosen by Windows") : game + " (game)"; }

    namespace
    {
        bool hardwareAdapter(const DXGI_ADAPTER_DESC1 &d)
        {
            // Not the software rasteriser (Microsoft Basic Render Driver).
            return !(d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) && !(d.VendorId == 0x1414 && d.DeviceId == 0x8c);
        }
        std::string adapterName(const DXGI_ADAPTER_DESC1 &d) { return trim(utf8(d.Description)); }
    }

    GpuChoice gameGpu(const fs::path &exe)
    {
        GpuChoice g;
        // What the program asks for: the hybrid-graphics exports.
        std::vector<std::string> names;
        std::string error;
        if (!exe.empty() && peExportNames(exe, names, error))
        {
            g.exeRead = true;
            for (const std::string &n : names)
            {
                g.nvExport = g.nvExport || n == "NvOptimusEnablement";
                g.amdExport = g.amdExport || n == "AmdPowerXpressRequestHighPerformance";
            }
        }
        // Windows' graphics setting for this program (it wins over the program's own request).
        if (!exe.empty())
        {
            std::error_code ec;
            const fs::path abs = fs::absolute(exe, ec);
            wchar_t buf[512] = {};
            DWORD size = sizeof(buf);
            if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\DirectX\\UserGpuPreferences", abs.c_str(), RRF_RT_REG_SZ, nullptr, buf, &size) ==
                ERROR_SUCCESS)
            {
                const std::string v = lower(utf8(buf));
                const size_t p = v.find("gpupreference=");
                if (p != std::string::npos)
                    g.windowsSetting = std::atoi(v.c_str() + p + 14);
            }
        }
        // The adapters (listed only).
        if (HMODULE dxgi = LoadLibraryW(L"dxgi.dll"))
        {
            using CreateFactory = HRESULT(WINAPI *)(REFIID, void **);
            const auto create = reinterpret_cast<CreateFactory>(reinterpret_cast<void *>(GetProcAddress(dxgi, "CreateDXGIFactory1")));
            IDXGIFactory1 *f = nullptr;
            if (create && SUCCEEDED(create(__uuidof(IDXGIFactory1), reinterpret_cast<void **>(&f))) && f)
            {
                IDXGIAdapter1 *a = nullptr;
                for (UINT i = 0; f->EnumAdapters1(i, &a) != DXGI_ERROR_NOT_FOUND && a; ++i)
                {
                    DXGI_ADAPTER_DESC1 d{};
                    if (SUCCEEDED(a->GetDesc1(&d)) && hardwareAdapter(d))
                        g.adapters.push_back(adapterName(d));
                    a->Release();
                    a = nullptr;
                }
                IDXGIFactory6 *f6 = nullptr;
                if (SUCCEEDED(f->QueryInterface(__uuidof(IDXGIFactory6), reinterpret_cast<void **>(&f6))) && f6)
                {
                    auto pick = [&](DXGI_GPU_PREFERENCE pref, std::string &name, unsigned *vendor) {
                        for (UINT i = 0; i < 16; ++i)
                        {
                            IDXGIAdapter1 *x = nullptr;
                            if (FAILED(f6->EnumAdapterByGpuPreference(i, pref, __uuidof(IDXGIAdapter1), reinterpret_cast<void **>(&x))) || !x)
                                break;
                            DXGI_ADAPTER_DESC1 d{};
                            const bool ok = SUCCEEDED(x->GetDesc1(&d)) && hardwareAdapter(d);
                            x->Release();
                            if (ok)
                            {
                                name = adapterName(d);
                                if (vendor)
                                    *vendor = d.VendorId;
                                break;
                            }
                        }
                    };
                    pick(DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, g.highPerformance, &g.highPerformanceVendor);
                    pick(DXGI_GPU_PREFERENCE_MINIMUM_POWER, g.minimumPower, nullptr);
                    f6->Release();
                }
                f->Release();
            }
        }
        // The exports apply to their own vendor's switchable graphics only.
        const unsigned v = g.highPerformanceVendor;
        const bool asked = (v == 0x10de && g.nvExport) || ((v == 0x1002 || v == 0x1022) && g.amdExport);
        if (g.adapters.empty())
            g.why = "no adapter list (DXGI)";
        else if (g.windowsSetting == 2 && !g.highPerformance.empty())
        {
            g.game = g.highPerformance;
            g.why = "Windows' graphics setting for the program: high performance";
        }
        else if (g.windowsSetting == 1 && !g.minimumPower.empty())
        {
            g.game = g.minimumPower;
            g.why = "Windows' graphics setting for the program: power saving";
        }
        else if (g.adapters.size() == 1)
        {
            g.game = g.adapters[0];
            g.why = "the only graphics adapter";
        }
        else if (asked)
        {
            g.game = g.highPerformance;
            g.why = std::string("the program asks for the high-performance GPU (") + (v == 0x10de ? "NvOptimusEnablement" : "AmdPowerXpressRequestHighPerformance") +
                    " exported)";
        }
        else if (!g.exeRead)
            g.why = "the program could not be read" + (error.empty() ? std::string() : ": " + error);
        else
            g.why = (g.nvExport || g.amdExport) ? "the program's request does not apply to the high-performance GPU's vendor" : "the program asks for no GPU";
        return g;
    }
}
