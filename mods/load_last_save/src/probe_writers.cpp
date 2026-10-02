// probe_writers.cpp - who writes the game's "load this save" state?
//
// Off unless `ProbeWriters=true` in the mod's ini. The reader probe established that a load is
// requested by writing the slot's file name into 0x00C3E758 and raising the flag at 0x00C3E868,
// and that the reader itself is a worker thread (started by the CRT, so its stack never shows the
// game code that asked for the load). This probe watches those two addresses with hardware *write*
// breakpoints, so the code that raises the request names itself: the reported EIP is the instruction
// that performed the write, and its caller is the load action.
//
// It is the same machinery that found the dialog text parser earlier (debug/breakpoints.cpp): one
// vectored exception handler, DR0..DR3 on every thread, length 1 (a longer length must be aligned to
// it, and these addresses are not), RF set on resume (a data breakpoint stays asserted otherwise and
// the process dies inside the dispatcher), and hits recorded in a ring for a normal thread to format
// - never in the handler itself.
#include "al.h"
#include "mod_api.h"

#include <tlhelp32.h>

#include <cstdarg>
#include <cstdio>

namespace al {

namespace {

const AtmtModApi* g_api = nullptr;

// The two addresses that carry the request (see docs/ENGINE_NOTES.md, "Saving and loading").
const uintptr_t kWatchAddrs[] = {kGameSaveNameBuffer, kGameSaveReadyFlag};
constexpr int kWatchCount = static_cast<int>(sizeof(kWatchAddrs) / sizeof(kWatchAddrs[0]));

constexpr LONG kMaxRecords = 400;
volatile LONG g_hits = 0;

struct WatchHit {
    uint32_t address;
    uint32_t eip, esp, ebp, eax, ebx, ecx, edx, esi, edi;
};

constexpr int kRing = 256;
WatchHit g_ring[kRing];
volatile LONG g_seen = 0;
volatile LONG g_drained = 0;
PVOID g_veh = nullptr;

void Log(const char* fmt, ...) {
    if (g_api == nullptr || g_api->log == nullptr) return;
    char buf[400];
    va_list args;
    va_start(args, fmt);
    _vsnprintf(buf, sizeof(buf) - 1, fmt, args);
    va_end(args);
    buf[sizeof(buf) - 1] = '\0';
    g_api->log(buf);
}

LONG CALLBACK WatchHandler(EXCEPTION_POINTERS* info) {
    if (info->ExceptionRecord->ExceptionCode != EXCEPTION_SINGLE_STEP) {
        return EXCEPTION_CONTINUE_SEARCH;
    }
    CONTEXT* c = info->ContextRecord;
    c->EFlags |= 0x10000;   // RF: suppress the breakpoint for this instruction (see the header)
    const LONG index = InterlockedIncrement(&g_hits) - 1;
    if (index >= kMaxRecords) {
        c->Dr7 = 0;         // budget spent: stop this thread's breakpoints firing at all
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    const LONG slot = InterlockedIncrement(&g_seen) - 1;
    if (slot < kRing) {
        uint32_t hit_addr = static_cast<uint32_t>(kWatchAddrs[0]);
        for (int i = 0; i < kWatchCount; ++i) {
            if ((c->Dr6 & (1u << i)) != 0) hit_addr = static_cast<uint32_t>(kWatchAddrs[i]);
        }
        WatchHit& h = g_ring[slot];
        h.address = hit_addr;
        h.eip = c->Eip;
        h.esp = c->Esp;
        h.ebp = c->Ebp;
        h.eax = c->Eax;
        h.ebx = c->Ebx;
        h.ecx = c->Ecx;
        h.edx = c->Edx;
        h.esi = c->Esi;
        h.edi = c->Edi;
    }
    return EXCEPTION_CONTINUE_EXECUTION;
}

int ArmThreads() {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap == INVALID_HANDLE_VALUE) return 0;
    THREADENTRY32 te;
    te.dwSize = sizeof(te);
    const DWORD me = GetCurrentProcessId();
    const DWORD my_tid = GetCurrentThreadId();
    int armed = 0;
    if (Thread32First(snap, &te)) {
        do {
            if (te.th32OwnerProcessID != me || te.th32ThreadID == my_tid) continue;
            if (armed >= 64) continue;   // bounded: the game makes a handful of threads
            HANDLE th = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_SET_CONTEXT,
                                   FALSE, te.th32ThreadID);
            if (th == nullptr) continue;
            if (SuspendThread(th) == static_cast<DWORD>(-1)) {
                CloseHandle(th);
                continue;
            }
            CONTEXT ctx;
            memset(&ctx, 0, sizeof(ctx));
            ctx.ContextFlags = CONTEXT_FULL | CONTEXT_DEBUG_REGISTERS;
            if (GetThreadContext(th, &ctx)) {
                ctx.Dr0 = kWatchAddrs[0];
                ctx.Dr1 = kWatchCount > 1 ? kWatchAddrs[1] : 0;
                ctx.Dr2 = 0;
                ctx.Dr3 = 0;
                uint32_t dr7 = 0x100u;   // LE: exact breakpoints
                for (int i = 0; i < kWatchCount; ++i) {
                    dr7 |= (1u << (i * 2));          // Lx: local enable
                    dr7 |= (0x1u << (16 + i * 4));   // R/Wx = 01: break on writes
                    dr7 |= (0x0u << (18 + i * 4));   // LENx = 1 byte
                }
                ctx.Dr7 = dr7;
                ctx.Dr6 = 0;
                SetThreadContext(th, &ctx);
                ++armed;
            }
            ResumeThread(th);
            CloseHandle(th);
        } while (Thread32Next(snap, &te));
    }
    CloseHandle(snap);
    return armed;
}

DWORD WINAPI DrainThread(LPVOID) {
    DWORD last_arm = 0;
    int last_armed = -1;
    while (true) {
        Sleep(200);
        const LONG seen = g_seen;
        while (g_drained < seen && g_drained < kRing) {
            const WatchHit& h = g_ring[g_drained];
            const bool is_flag = h.address == static_cast<uint32_t>(kGameSaveReadyFlag);
            Log("writer: %s (0x%08x) eip=0x%08x esp=0x%08x ebp=0x%08x "
                "ecx=0x%08x edx=0x%08x eax=0x%08x esi=0x%08x edi=0x%08x ebx=0x%08x",
                is_flag ? "request flag" : "name buffer", h.address, h.eip, h.esp, h.ebp, h.ecx,
                h.edx, h.eax, h.esi, h.edi, h.ebx);
            ++g_drained;
        }
        // The game makes new threads; they need the breakpoints as well. Only the *change* is
        // logged: re-arming every second would otherwise write a line per second for hours.
        const DWORD now = GetTickCount();
        if (now - last_arm > 1000) {
            const int armed = ArmThreads();
            if (armed != last_armed) {
                Log("writer probe: %d thread(s) armed", armed);
                last_armed = armed;
            }
            last_arm = now;
        }
    }
    return 0;
}

}  // namespace

bool InstallWriterProbe(const AtmtModApi* api, std::string* why_not) {
    if (api == nullptr || api->log == nullptr) {
        if (why_not != nullptr) *why_not = "no loader services";
        return false;
    }
    g_api = api;
    g_veh = AddVectoredExceptionHandler(1, &WatchHandler);
    if (g_veh == nullptr) {
        if (why_not != nullptr) *why_not = "could not install the exception handler";
        return false;
    }
    if (ArmThreads() <= 0) {
        if (why_not != nullptr) *why_not = "no thread could be armed";
        return false;
    }
    HANDLE t = CreateThread(nullptr, 0, DrainThread, nullptr, 0, nullptr);
    if (t != nullptr) CloseHandle(t);
    Log("writer probe: watching writes to the name buffer (0x%08x) and the request flag (0x%08x)",
        static_cast<unsigned>(kGameSaveNameBuffer), static_cast<unsigned>(kGameSaveReadyFlag));
    return true;
}

}  // namespace al

