// Diagnostics (GT3_EE_GUARD=<hex address>[,<from vsync>], EeScheduler.cpp): who writes that guest word, guest code or
// host code alike (DMA, IOP services, hooks). The host page holding it is made read-only; every write to the page
// faults, runs with the page open for that one instruction (trap flag) and closes it again; a write to the word itself is
// logged with the writing thread, the host instruction and the call stack as module+offset. Off unless set; slow while on.
#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <thread>

// The running guest thread's registers at a guarded write (EeScheduler.cpp gives them): pc, ra, sp, a0..a3, s0..s3.
struct Ps2xGuardRegs
{
    uint32_t pc, ra, sp, a[4], s[4];
};
bool (*g_ps2xGuardRegs)(Ps2xGuardRegs &) = nullptr;

namespace
{
    std::atomic<uintptr_t> g_page{0};
    uintptr_t g_word = 0;
    uint32_t g_guestWord = 0;
    std::atomic<int> g_logged{0};

    void describe(void *address, char *out, size_t size)
    {
        HMODULE module = nullptr;
        char name[MAX_PATH] = "?";
        if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               static_cast<LPCSTR>(address), &module) &&
            module)
        {
            GetModuleFileNameA(module, name, MAX_PATH);
            const char *base = std::strrchr(name, '\\');
            std::snprintf(out, size, "%s+0x%llx", base ? base + 1 : name,
                          static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(address) - reinterpret_cast<uintptr_t>(module)));
        }
        else
            std::snprintf(out, size, "%p", address);
    }

    thread_local bool t_stepping = false;

    LONG CALLBACK onFault(EXCEPTION_POINTERS *info)
    {
        // (after the faulting write ran: the page closed again)
        if (info->ExceptionRecord->ExceptionCode == EXCEPTION_SINGLE_STEP && t_stepping)
        {
            t_stepping = false;
            DWORD old = 0;
            VirtualProtect(reinterpret_cast<void *>(g_page.load()), 4096u, PAGE_READONLY, &old);
            return EXCEPTION_CONTINUE_EXECUTION;
        }
        if (info->ExceptionRecord->ExceptionCode != EXCEPTION_ACCESS_VIOLATION || info->ExceptionRecord->NumberParameters < 2)
            return EXCEPTION_CONTINUE_SEARCH;
        const uintptr_t at = static_cast<uintptr_t>(info->ExceptionRecord->ExceptionInformation[1]);
        const uintptr_t page = g_page.load();
        if (!page || at < page || at >= page + 4096u || info->ExceptionRecord->ExceptionInformation[0] != 1u)
            return EXCEPTION_CONTINUE_SEARCH;
        DWORD old = 0;
        VirtualProtect(reinterpret_cast<void *>(page), 4096u, PAGE_READWRITE, &old);
        // (GT3_EE_GUARD_MAX=<n>: the writes logged, default 12)
        static const int maxLogs = [] {
            const char *v = std::getenv("GT3_EE_GUARD_MAX");
            return v && *v ? std::atoi(v) : 12;
        }();
        if (at + 8u > g_word && at < g_word + 8u && g_logged.fetch_add(1) < maxLogs)
        {
            char where[300];
            describe(reinterpret_cast<void *>(info->ContextRecord->Rip), where, sizeof(where));
            std::fprintf(stderr, "[ee-guard] write to the guarded word (+%lld) by thread %lu at %s; stack:", static_cast<long long>(at - g_word),
                         GetCurrentThreadId(), where);
            void *frames[24] = {};
            const USHORT n = CaptureStackBackTrace(0, 24, frames, nullptr);
            for (USHORT i = 0; i < n; ++i)
            {
                describe(frames[i], where, sizeof(where));
                std::fprintf(stderr, " %s", where);
            }
            std::fputc('\n', stderr);
            Ps2xGuardRegs r{};
            if (g_ps2xGuardRegs && g_ps2xGuardRegs(r))
            {
                std::fprintf(stderr, "[ee-guard]   guest pc %08x ra %08x sp %08x a0 %08x a1 %08x a2 %08x a3 %08x s0 %08x s1 %08x s2 %08x s3 %08x\n", r.pc,
                             r.ra, r.sp, r.a[0], r.a[1], r.a[2], r.a[3], r.s[0], r.s[1], r.s[2], r.s[3]);
                // (the guest stack's return addresses: words just after a jal / jalr in the program, innermost first)
                const uint8_t *rdram = reinterpret_cast<const uint8_t *>(g_word - g_guestWord);
                const auto word = [rdram](uint32_t a) {
                    uint32_t v = 0;
                    std::memcpy(&v, rdram + (a & 0x01fffffcu), 4);
                    return v;
                };
                std::fprintf(stderr, "[ee-guard]   guest stack words:");
                for (uint32_t k = 0; k < 24; ++k)
                    std::fprintf(stderr, " %08x", word((r.sp & ~3u) + 4u * k));
                std::fprintf(stderr, "\n[ee-guard]   guest stack:");
                for (uint32_t a = r.sp & ~3u, n = 0; a < 0x02000000u && a < (r.sp & ~3u) + 0x2000u && n < 24; a += 4)
                {
                    const uint32_t v = word(a);
                    if (v < 0x100008u || v >= 0x600000u || (v & 3u))
                        continue;
                    const uint32_t call = word(v - 8u);
                    if ((call >> 26) == 3u || (call & 0xfc00003fu) == 9u)
                        std::fprintf(stderr, " %x", v), ++n;
                }
                std::fputc('\n', stderr);
            }
            std::fflush(stderr);
        }
        // the write runs with the page open, one instruction (trap flag), then the page closes again
        info->ContextRecord->EFlags |= 0x100u;
        t_stepping = true;
        return EXCEPTION_CONTINUE_EXECUTION;
    }
} // namespace

// Arms the guard on rdram + guestAddress (once).
void ps2xGuardDiagArm(uint8_t *rdram, uint32_t guestAddress)
{
    static bool armed = false;
    if (armed || !rdram)
        return;
    armed = true;
    g_guestWord = guestAddress & 0x01fffffcu;
    g_word = reinterpret_cast<uintptr_t>(rdram + g_guestWord);
    g_page.store(g_word & ~static_cast<uintptr_t>(4095u));
    AddVectoredExceptionHandler(1, &onFault);
    std::fprintf(stderr, "[ee-guard] guarding guest %08x (host page %p)\n", guestAddress, reinterpret_cast<void *>(g_page.load()));
    DWORD old = 0;
    VirtualProtect(reinterpret_cast<void *>(g_page.load()), 4096u, PAGE_READONLY, &old);
}
#else
#include <cstdint>
void ps2xGuardDiagArm(uint8_t *, uint32_t) {}
#endif
