#include "runtime/crash_report.h"
#if defined(PS2X_GAME_GT3)
#include "ps2x/iop/gt3/pdi1394_link.h"
#endif
#if defined(PS2X_GAME_GT4)
#include "ps2x/iop/gt4_host_glue.h"
#endif

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <dbghelp.h>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <csignal>
#include <tlhelp32.h>
#include <algorithm>
#include <array>
#include <chrono>
#include <map>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>
#pragma comment(lib, "dbghelp.lib")

namespace {
// setTestRunErrorMode: the report ends the process (exit code 3).
volatile LONG g_testRunExit = 0;

LONG WINAPI writeCrashReport(EXCEPTION_POINTERS *info)
{
    static volatile LONG entered = 0;
    static volatile DWORD reporter = 0;
    if (InterlockedExchange(&entered, 1))
    {
        // Test runs: a fault inside the report ends the process at once; a
        // second thread's waits for the first report, which ends it.
        if (g_testRunExit)
        {
            if (reporter != GetCurrentThreadId())
                Sleep(30000);
            TerminateProcess(GetCurrentProcess(), 3);
        }
        return EXCEPTION_CONTINUE_SEARCH;
    }
    reporter = GetCurrentThreadId();
    // Linked consoles (i.LINK Battle) go on at once instead of waiting.
#if defined(PS2X_GAME_GT3)
    ps2x::iop::gt3::LinkTransport::goodbyeAll(true);
#endif
    // A force-feedback wheel stops pushing before the slow part of the report (no blocking lock).
#if defined(PS2X_GAME_GT4)
    ps2x::iop::gt4::glue::hostWheelEmergencyStop();
#endif
    const EXCEPTION_RECORD *record = info->ExceptionRecord;
    HMODULE module = nullptr;
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       static_cast<LPCSTR>(record->ExceptionAddress), &module);
    char name[MAX_PATH] = "?";
    if (module)
        GetModuleFileNameA(module, name, MAX_PATH);
    const auto offset = module ? reinterpret_cast<uintptr_t>(record->ExceptionAddress) - reinterpret_cast<uintptr_t>(module)
                               : uintptr_t{0};
    std::fprintf(stderr, "[crash] unhandled exception 0x%08lx at %p (%s+0x%llx) on thread %lu\n",
                 static_cast<unsigned long>(record->ExceptionCode), record->ExceptionAddress, name,
                 static_cast<unsigned long long>(offset), GetCurrentThreadId());
    if (record->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && record->NumberParameters >= 2)
        std::fprintf(stderr, "[crash] %s of address 0x%llx\n", record->ExceptionInformation[0] ? "write" : "read",
                     static_cast<unsigned long long>(record->ExceptionInformation[1]));
    // Symbolized stack of the faulting thread (the runner's PDB sits beside it).
    {
        HANDLE process = GetCurrentProcess(), thread = GetCurrentThread();
        SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS | SYMOPT_LOAD_LINES);
        if (SymInitialize(process, nullptr, TRUE))
        {
            CONTEXT context = *info->ContextRecord;
            STACKFRAME64 frame{};
            frame.AddrPC.Offset = context.Rip;
            frame.AddrPC.Mode = AddrModeFlat;
            frame.AddrFrame.Offset = context.Rbp;
            frame.AddrFrame.Mode = AddrModeFlat;
            frame.AddrStack.Offset = context.Rsp;
            frame.AddrStack.Mode = AddrModeFlat;
            alignas(SYMBOL_INFO) char storage[sizeof(SYMBOL_INFO) + 512];
            for (int depth = 0; depth < 48; ++depth)
            {
                if (!StackWalk64(IMAGE_FILE_MACHINE_AMD64, process, thread, &frame, &context, nullptr,
                                 SymFunctionTableAccess64, SymGetModuleBase64, nullptr) ||
                    !frame.AddrPC.Offset)
                    break;
                auto *symbol = reinterpret_cast<SYMBOL_INFO *>(storage);
                symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
                symbol->MaxNameLen = 511;
                DWORD64 displacement = 0;
                IMAGEHLP_LINE64 line{};
                line.SizeOfStruct = sizeof(line);
                DWORD lineDisplacement = 0;
                const bool named = SymFromAddr(process, frame.AddrPC.Offset, &displacement, symbol);
                const bool lined = SymGetLineFromAddr64(process, frame.AddrPC.Offset, &lineDisplacement, &line);
                std::fprintf(stderr, "[crash]   #%02d 0x%llx %s+0x%llx %s:%lu\n", depth,
                             static_cast<unsigned long long>(frame.AddrPC.Offset), named ? symbol->Name : "?",
                             static_cast<unsigned long long>(displacement), lined ? line.FileName : "",
                             lined ? line.LineNumber : 0ul);
            }
            SymCleanup(process);
        }
    }
    const char *path = std::getenv("GT3_CRASH_DUMP");
    HANDLE file = CreateFileA(path ? path : "ps2EntryRunner-crash.dmp", GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file != INVALID_HANDLE_VALUE)
    {
        MINIDUMP_EXCEPTION_INFORMATION exception{GetCurrentThreadId(), info, FALSE};
        const BOOL written = MiniDumpWriteDump(
            GetCurrentProcess(), GetCurrentProcessId(), file,
            static_cast<MINIDUMP_TYPE>(MiniDumpWithThreadInfo | MiniDumpWithIndirectlyReferencedMemory), &exception,
            nullptr, nullptr);
        CloseHandle(file);
        std::fprintf(stderr, "[crash] minidump %s: %s\n", written ? "written" : "failed",
                     path ? path : "ps2EntryRunner-crash.dmp");
    }
    std::fflush(stderr);
    if (g_testRunExit)
        TerminateProcess(GetCurrentProcess(), 3); // never Windows Error Reporting (no dialog, no wait)
    return EXCEPTION_CONTINUE_SEARCH;
}

// abort(), an invalid CRT parameter or a pure virtual call end the process
// without an unhandled exception: report them through the same path (a stack
// and a minidump) by raising one here.
void reportFatal(const char *why)
{
    std::fprintf(stderr, "[crash] %s\n", why);
    std::fflush(stderr);
#if defined(_MSC_VER)
    __try
    {
        RaiseException(0xE0000ABBu, 0, 0, nullptr);
    }
    __except (writeCrashReport(GetExceptionInformation()), EXCEPTION_EXECUTE_HANDLER)
    {
    }
#else // clang for mingw has no __try: hand the report this thread's context directly
    CONTEXT context{};
    RtlCaptureContext(&context);
    EXCEPTION_RECORD record{};
    record.ExceptionCode = 0xE0000ABBu;
    record.ExceptionAddress = reinterpret_cast<void *>(context.Rip);
    EXCEPTION_POINTERS info{&record, &context};
    writeCrashReport(&info);
#endif
    _exit(3);
}

void onAbortSignal(int)
{
    reportFatal("abort() called");
}

void onInvalidParameter(const wchar_t *, const wchar_t *, const wchar_t *, unsigned int, uintptr_t)
{
    reportFatal("invalid CRT parameter");
}

void onPureCall()
{
    reportFatal("pure virtual function call");
}
} // namespace

void ps2x::installCrashReporter()
{
    SetUnhandledExceptionFilter(writeCrashReport);
    // abort() raises SIGABRT instead of a fast fail that no handler sees.
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
    std::signal(SIGABRT, onAbortSignal);
    _set_invalid_parameter_handler(onInvalidParameter);
    _set_purecall_handler(onPureCall);
}

void ps2x::setTestRunErrorMode()
{
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
    // Faults that skip the exception filter (fast fail) still reach WER: no UI there either.
    using WerSetFlagsFn = HRESULT(WINAPI *)(DWORD);
    if (auto werSetFlags = reinterpret_cast<WerSetFlagsFn>(GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "WerSetFlags")))
        werSetFlags(0x20 /* WER_FAULT_REPORTING_NO_UI */);
    InterlockedExchange(&g_testRunExit, 1);
}

// PS2X_SAMPLE_PROFILE=<seconds>: a sampling profiler. Every 2 ms it suspends
// each busy thread of the process, records the leaf function and the
// functions on its stack (RtlVirtualUnwind), and every <seconds> prints the
// busiest threads plus the top functions by self and by total samples.
void ps2x::startSamplingProfiler()
{
    const char *spec = std::getenv("PS2X_SAMPLE_PROFILE");
    const int period = spec && *spec ? std::atoi(spec) : 0;
    if (period <= 0)
        return;
    std::thread([period] {
        HANDLE process = GetCurrentProcess();
        SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS);
        SymInitialize(process, nullptr, TRUE);
        const DWORD me = GetCurrentThreadId();
        struct ThreadInfo
        {
            HANDLE handle = nullptr;
            ULONGLONG lastCpu = 0;
            uint64_t samples = 0;
        };
        std::map<DWORD, ThreadInfo> threads;
        std::unordered_map<DWORD64, uint64_t> leaf, total;
        std::map<std::array<DWORD64, 4>, uint64_t> edges; // leaf and its first three distinct callers
        auto refresh = [&] {
            HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
            THREADENTRY32 entry{};
            entry.dwSize = sizeof(entry);
            for (BOOL ok = Thread32First(snap, &entry); ok; ok = Thread32Next(snap, &entry))
            {
                if (entry.th32OwnerProcessID != GetCurrentProcessId() || entry.th32ThreadID == me ||
                    threads.count(entry.th32ThreadID))
                    continue;
                HANDLE h = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE,
                                      entry.th32ThreadID);
                if (h)
                    threads[entry.th32ThreadID].handle = h;
            }
            CloseHandle(snap);
        };
        auto functionOf = [](DWORD64 pc) -> DWORD64 {
            DWORD64 imageBase = 0;
            if (auto *fn = RtlLookupFunctionEntry(pc, &imageBase, nullptr))
                return imageBase + fn->BeginAddress;
            return pc;
        };
        auto nameOf = [&](DWORD64 address) {
            alignas(SYMBOL_INFO) char storage[sizeof(SYMBOL_INFO) + 256];
            auto *symbol = reinterpret_cast<SYMBOL_INFO *>(storage);
            symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
            symbol->MaxNameLen = 255;
            DWORD64 displacement = 0;
            if (SymFromAddr(process, address, &displacement, symbol))
                return std::string(symbol->Name);
            char buffer[32];
            std::snprintf(buffer, sizeof(buffer), "0x%llx", static_cast<unsigned long long>(address));
            return std::string(buffer);
        };
        auto start = std::chrono::steady_clock::now();
        uint64_t round = 0;
        for (;;)
        {
            if (round++ % 500 == 0)
                refresh();
            for (auto &[id, info] : threads)
            {
                FILETIME created, exited, kernel, user;
                if (!GetThreadTimes(info.handle, &created, &exited, &kernel, &user))
                    continue;
                const ULONGLONG cpu = ((ULONGLONG(user.dwHighDateTime) << 32) | user.dwLowDateTime) +
                                      ((ULONGLONG(kernel.dwHighDateTime) << 32) | kernel.dwLowDateTime);
                const bool busy = cpu != info.lastCpu;
                info.lastCpu = cpu;
                if (!busy || SuspendThread(info.handle) == DWORD(-1))
                    continue;
                CONTEXT context{};
                context.ContextFlags = CONTEXT_FULL;
                if (GetThreadContext(info.handle, &context))
                {
                    ++info.samples;
                    ++leaf[functionOf(context.Rip)];
                    DWORD64 seen[24];
                    int seenCount = 0;
                    const DWORD64 leafFn = functionOf(context.Rip);
                    for (int depth = 0; depth < 24 && context.Rip; ++depth)
                    {
                        const DWORD64 fn = functionOf(context.Rip);
                        if (std::find(seen, seen + seenCount, fn) == seen + seenCount)
                        {
                            seen[seenCount++] = fn;
                            ++total[fn];
                        }
                        DWORD64 imageBase = 0;
                        auto *entry = RtlLookupFunctionEntry(context.Rip, &imageBase, nullptr);
                        if (!entry)
                        {
                            if (depth != 0)
                                break;
                            // leaf function without unwind data: return address on top
                            context.Rip = *reinterpret_cast<const DWORD64 *>(context.Rsp);
                            context.Rsp += 8;
                            continue;
                        }
                        void *handlerData = nullptr;
                        DWORD64 establisher = 0;
                        RtlVirtualUnwind(UNW_FLAG_NHANDLER, imageBase, context.Rip, entry, &context, &handlerData,
                                         &establisher, nullptr);
                    }
                    std::array<DWORD64, 4> chain{};
                    for (int i = 0; i < 4 && i < seenCount; ++i)
                        chain[i] = seen[i];
                    ++edges[chain];
                }
                ResumeThread(info.handle);
            }
            Sleep(2);
            if (std::chrono::steady_clock::now() - start < std::chrono::seconds(period))
                continue;
            start = std::chrono::steady_clock::now();
            std::fprintf(stderr, "[profile] threads:");
            for (auto &[id, info] : threads)
            {
                if (!info.samples)
                    continue;
                PWSTR description = nullptr;
                char name[64] = "";
                if (SUCCEEDED(GetThreadDescription(info.handle, &description)) && description)
                {
                    WideCharToMultiByte(CP_UTF8, 0, description, -1, name, sizeof(name), nullptr, nullptr);
                    LocalFree(description);
                }
                std::fprintf(stderr, " %lu(%s)=%llu", id, name, static_cast<unsigned long long>(info.samples));
                info.samples = 0;
            }
            std::fprintf(stderr, "\n");
            for (auto *table : {&leaf, &total})
            {
                std::vector<std::pair<uint64_t, DWORD64>> top;
                for (const auto &[fn, n] : *table)
                    top.emplace_back(n, fn);
                std::sort(top.rbegin(), top.rend());
                std::fprintf(stderr, "[profile] %s:", table == &leaf ? "self" : "total");
                for (size_t i = 0; i < top.size() && i < 40; ++i)
                    std::fprintf(stderr, " %s=%llu", nameOf(top[i].second).c_str(), static_cast<unsigned long long>(top[i].first));
                std::fprintf(stderr, "\n");
                table->clear();
            }
            {
                std::vector<std::pair<uint64_t, std::array<DWORD64, 4>>> top;
                for (const auto &[edge, n] : edges)
                    top.emplace_back(n, edge);
                std::sort(top.rbegin(), top.rend());
                std::fprintf(stderr, "[profile] callers:");
                for (size_t i = 0; i < top.size() && i < 30; ++i)
                {
                    std::fprintf(stderr, " [%llu]", static_cast<unsigned long long>(top[i].first));
                    for (DWORD64 fn : top[i].second)
                        if (fn)
                            std::fprintf(stderr, " <%s", nameOf(fn).c_str());
                }
                std::fprintf(stderr, "\n");
                edges.clear();
            }
            std::fflush(stderr);
        }
    }).detach();
}
#else
void ps2x::installCrashReporter() {}
void ps2x::setTestRunErrorMode() {}
void ps2x::startSamplingProfiler() {}
#endif
