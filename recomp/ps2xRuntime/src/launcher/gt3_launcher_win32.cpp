#include "gt3_launcher_win32.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#include <windows.h>
#include <commdlg.h>
#include <shellapi.h>

#include <algorithm>
#include <cwctype>

namespace gt3launcher
{
    namespace
    {
        std::wstring widen(const std::string &s)
        {
            if (s.empty())
                return {};
            const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
            std::wstring out(static_cast<size_t>(n), L'\0');
            MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), n);
            return out;
        }

        std::string narrow(const std::wstring &s)
        {
            if (s.empty())
                return {};
            const int n = WideCharToMultiByte(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0, nullptr, nullptr);
            std::string out(static_cast<size_t>(n), '\0');
            WideCharToMultiByte(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), n, nullptr, nullptr);
            return out;
        }

        // "Name|*.ext|All|*.*" -> the double-NUL-terminated list the dialogs want.
        std::wstring dialogFilter(const std::string &filter)
        {
            std::wstring f = widen(filter);
            std::replace(f.begin(), f.end(), L'|', L'\0');
            f.push_back(L'\0');
            f.push_back(L'\0');
            return f;
        }

        bool fileDialog(bool save, void *owner, const std::string &title, const std::string &filter, std::string &path)
        {
            wchar_t buffer[MAX_PATH * 4] = {};
            const std::wstring current = widen(path);
            wcsncpy_s(buffer, current.c_str(), _TRUNCATE);
            const std::wstring filterList = dialogFilter(filter);
            const std::wstring caption = widen(title);
            std::wstring initialDir;
            const size_t slash = current.find_last_of(L"\\/");
            if (slash != std::wstring::npos)
                initialDir = current.substr(0, slash);
            OPENFILENAMEW ofn{};
            ofn.lStructSize = sizeof(ofn);
            ofn.hwndOwner = static_cast<HWND>(owner);
            ofn.lpstrFilter = filterList.c_str();
            ofn.lpstrFile = buffer;
            ofn.nMaxFile = static_cast<DWORD>(std::size(buffer));
            ofn.lpstrTitle = caption.c_str();
            ofn.lpstrInitialDir = initialDir.empty() ? nullptr : initialDir.c_str();
            // Save: an existing card may be chosen as well (no overwrite prompt).
            ofn.Flags = OFN_NOCHANGEDIR | OFN_PATHMUSTEXIST | (save ? 0u : static_cast<DWORD>(OFN_FILEMUSTEXIST));
            const BOOL ok = save ? GetSaveFileNameW(&ofn) : GetOpenFileNameW(&ofn);
            if (!ok)
                return false;
            path = narrow(buffer);
            return true;
        }

        bool isOwnVariable(const std::wstring &entry)
        {
            auto startsWith = [&](const wchar_t *prefix) {
                const size_t n = wcslen(prefix);
                if (entry.size() < n)
                    return false;
                for (size_t i = 0; i < n; ++i)
                    if (std::towupper(entry[i]) != prefix[i])
                        return false;
                return true;
            };
            return startsWith(L"GT3_") || startsWith(L"PS2X_");
        }

        std::wstring quoteArgument(const std::wstring &arg)
        {
            if (!arg.empty() && arg.find_first_of(L" \t\"") == std::wstring::npos)
                return arg;
            std::wstring out = L"\"";
            size_t backslashes = 0;
            for (const wchar_t c : arg)
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
                out.push_back(c);
            }
            out.append(backslashes * 2, L'\\');
            out.push_back(L'"');
            return out;
        }
    } // namespace

    std::string executableDirectory()
    {
        wchar_t buffer[MAX_PATH * 4] = {};
        const DWORD n = GetModuleFileNameW(nullptr, buffer, static_cast<DWORD>(std::size(buffer)));
        std::wstring path(buffer, n);
        const size_t slash = path.find_last_of(L"\\/");
        return narrow(slash == std::wstring::npos ? path : path.substr(0, slash));
    }

    bool fileExists(const std::string &path)
    {
        const DWORD attributes = GetFileAttributesW(widen(path).c_str());
        return attributes != INVALID_FILE_ATTRIBUTES && !(attributes & FILE_ATTRIBUTE_DIRECTORY);
    }

    bool directoryExists(const std::string &path)
    {
        const DWORD attributes = GetFileAttributesW(widen(path).c_str());
        return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY);
    }

    std::vector<std::string> filesIn(const std::string &directory, const std::string &pattern)
    {
        std::vector<std::string> names;
        WIN32_FIND_DATAW found{};
        const HANDLE find = FindFirstFileW(widen(directory + "\\" + pattern).c_str(), &found);
        if (find == INVALID_HANDLE_VALUE)
            return names;
        do
        {
            if (!(found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
                names.push_back(narrow(found.cFileName));
        } while (FindNextFileW(find, &found));
        FindClose(find);
        return names;
    }

    bool browseOpenFile(void *owner, const std::string &title, const std::string &filter, std::string &path)
    {
        return fileDialog(false, owner, title, filter, path);
    }

    bool browseSaveFile(void *owner, const std::string &title, const std::string &filter, std::string &path)
    {
        return fileDialog(true, owner, title, filter, path);
    }

    void shellOpen(const std::string &path)
    {
        ShellExecuteW(nullptr, L"open", widen(path).c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    }

    bool startGame(const std::string &exe, const std::vector<std::string> &args, const std::string &workingDirectory,
                   const std::vector<std::pair<std::string, std::string>> &env, const std::string &logPath, GameProcess &process,
                   std::string &error)
    {
        // Environment: the launcher's own minus GT3_*/PS2X_* (diagnostic
        // settings left in the user's environment must not leak in), then ours.
        std::vector<std::wstring> entries;
        if (wchar_t *block = GetEnvironmentStringsW())
        {
            for (const wchar_t *p = block; *p; p += wcslen(p) + 1)
            {
                const std::wstring entry(p);
                if (!entry.empty() && entry[0] != L'=' && !isOwnVariable(entry))
                    entries.push_back(entry);
            }
            FreeEnvironmentStringsW(block);
        }
        for (const auto &[name, value] : env)
            if (!value.empty())
                entries.push_back(widen(name) + L"=" + widen(value));
        std::sort(entries.begin(), entries.end(), [](const std::wstring &a, const std::wstring &b) { return _wcsicmp(a.c_str(), b.c_str()) < 0; });
        std::wstring block;
        for (const std::wstring &entry : entries)
        {
            block += entry;
            block.push_back(L'\0');
        }
        block.push_back(L'\0');

        SECURITY_ATTRIBUTES inherit{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
        HANDLE log = CreateFileW(widen(logPath).c_str(), GENERIC_WRITE, FILE_SHARE_READ, &inherit, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (log == INVALID_HANDLE_VALUE)
        {
            error = "cannot create the log file " + logPath;
            return false;
        }
        STARTUPINFOW si{};
        si.cb = sizeof(si);
        si.dwFlags = STARTF_USESTDHANDLES;
        si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
        si.hStdOutput = log;
        si.hStdError = log;
        std::wstring commandLine = quoteArgument(widen(exe));
        for (const std::string &arg : args)
            commandLine += L" " + quoteArgument(widen(arg));
        PROCESS_INFORMATION pi{};
        const BOOL ok = CreateProcessW(widen(exe).c_str(), commandLine.data(), nullptr, nullptr, TRUE, CREATE_UNICODE_ENVIRONMENT | CREATE_NO_WINDOW,
                                       block.data(), widen(workingDirectory).c_str(), &si, &pi);
        CloseHandle(log);
        if (!ok)
        {
            error = "cannot start " + exe + " (Windows error " + std::to_string(GetLastError()) + ")";
            return false;
        }
        CloseHandle(pi.hThread);
        process.handle = pi.hProcess;
        process.running = true;
        process.exitCode = 0;
        return true;
    }

    bool pollGame(GameProcess &process)
    {
        if (!process.running || !process.handle)
            return false;
        if (WaitForSingleObject(static_cast<HANDLE>(process.handle), 0) != WAIT_OBJECT_0)
        {
            // Asked to close (stopGame) but still running after 5 s: end it.
            if (process.closeRequested && GetTickCount64() - process.closeRequested > 5000)
                TerminateProcess(static_cast<HANDLE>(process.handle), 1);
            return false;
        }
        process.closeRequested = 0;
        DWORD code = 0;
        GetExitCodeProcess(static_cast<HANDLE>(process.handle), &code);
        CloseHandle(static_cast<HANDLE>(process.handle));
        process.handle = nullptr;
        process.running = false;
        process.exitCode = code;
        return true;
    }

    // Closes the game window as the user would, so the game shuts down
    // properly (i.LINK Battle partners are told at once); pollGame ends the
    // process if it has not gone after 5 s.
    void stopGame(GameProcess &process)
    {
        if (!process.running || !process.handle)
            return;
        struct Search { DWORD pid; bool posted; } search{GetProcessId(static_cast<HANDLE>(process.handle)), false};
        EnumWindows(
            [](HWND window, LPARAM param) -> BOOL {
                auto *s = reinterpret_cast<Search *>(param);
                DWORD pid = 0;
                GetWindowThreadProcessId(window, &pid);
                if (pid == s->pid && IsWindowVisible(window))
                    s->posted |= PostMessageW(window, WM_CLOSE, 0, 0) != 0;
                return TRUE;
            },
            reinterpret_cast<LPARAM>(&search));
        if (search.posted)
            process.closeRequested = GetTickCount64();
        else
            TerminateProcess(static_cast<HANDLE>(process.handle), 1);
    }

    void rotateLogs(const std::string &directory, const std::string &latestName, const std::string &previousPrefix, int keep)
    {
        auto name = [&](int i) { return widen(directory + "\\" + previousPrefix + std::to_string(i) + ".log"); };
        for (int i = keep - 1; i >= 1; --i)
            MoveFileExW(name(i).c_str(), name(i + 1).c_str(), MOVEFILE_REPLACE_EXISTING);
        MoveFileExW(widen(directory + "\\" + latestName).c_str(), name(1).c_str(), MOVEFILE_REPLACE_EXISTING);
    }

    std::vector<std::string> localAddresses()
    {
        std::vector<std::string> list;
        ULONG size = 16 * 1024;
        std::vector<unsigned char> buffer(size);
        auto *adapters = reinterpret_cast<IP_ADAPTER_ADDRESSES *>(buffer.data());
        const ULONG flags = GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER;
        if (GetAdaptersAddresses(AF_INET, flags, nullptr, adapters, &size) == ERROR_BUFFER_OVERFLOW)
        {
            buffer.resize(size);
            adapters = reinterpret_cast<IP_ADAPTER_ADDRESSES *>(buffer.data());
            if (GetAdaptersAddresses(AF_INET, flags, nullptr, adapters, &size) != NO_ERROR)
                return list;
        }
        for (auto *a = adapters; a; a = a->Next)
        {
            if (a->OperStatus != IfOperStatusUp || a->IfType == IF_TYPE_SOFTWARE_LOOPBACK)
                continue;
            for (auto *u = a->FirstUnicastAddress; u; u = u->Next)
            {
                char text[64]{};
                const auto *v4 = reinterpret_cast<const sockaddr_in *>(u->Address.lpSockaddr);
                if (inet_ntop(AF_INET, &v4->sin_addr, text, sizeof(text)) && std::string(text).rfind("169.254.", 0) != 0)
                    list.push_back(narrow(a->FriendlyName) + ": " + text);
            }
        }
        return list;
    }

    std::string computerName()
    {
        wchar_t name[MAX_COMPUTERNAME_LENGTH + 1]{};
        DWORD size = MAX_COMPUTERNAME_LENGTH + 1;
        return GetComputerNameW(name, &size) ? narrow(name) : std::string{};
    }

    unsigned long currentProcessId() { return GetCurrentProcessId(); }
}
