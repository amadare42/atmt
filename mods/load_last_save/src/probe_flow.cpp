// probe_flow.cpp - watching the game's own save/load *flow* entry (FUN_0064CAD0).
//
// The save manager is only the *data* half of a load: it reads the file into a buffer and calls back.
// The other half is the session module, and the game only ever enters it through
// `FUN_0064CAD0(self, phase)` - `__thiscall` (self in ecx) plus one stack argument, measured byte for
// byte on 2026-09-22:
//
//   0064cad0  55 8b ec               push ebp; mov ebp,esp
//   0064cad3  81 ec 84 00 00 00      sub  esp,0x84
//   0064cae6  8b 5d 08               mov  ebx,[ebp+8]        ; ebx = phase (stack argument)
//   0064cae9  8b f1                  mov  esi,ecx            ; esi = self
//   0064caeb  8b 46 1c               mov  eax,[esi+0x1c]     ; the save module object
//   0064caee  89 5e 0c               mov  [esi+0xc],ebx      ; self+0xC = phase  <- what cb1 reads
//   0064caf1  c6 46 10 00            mov  byte [esi+0x10],0
//   ...
//   0064cb12  0f 87 ...              ja   default            ; switch (phase) { case 0..6 }
//
// The switch (decompile: ghidra/logs/decomp_step.log):
//
//   phase 0        the session's own reset + module vtable +0x60 (buffer, size, cb1, cb2, self)
//   phase 1 and 2  module vtable +0x64 with the *game's* buffer (g_ctx+0xB6430, 0x708C0), the game's
//                  own callbacks (0x0064ACA0 / 0x0064A7B0) and `self` - cb1 enters the session on
//                  phase 1; phase 2 is what the title route uses (title_load.cpp)
//   phase 3        module vtable +0x68 (cb1, cb2, self)
//   phase 4 and 5  the 0x34-byte system blobs (phase 5 = "save511.dat" into &0x012EACBC, which is the
//                  request the game itself makes at startup)
//   phase 6        the full session reset + vtable +0x60 with the game's buffer and callbacks
//
// Every branch also primes the session first (reset, loading state, vtable +0x24/+0x2c/+0x34/+0x38,
// +0xC(400|500), +0x10(0)) and afterwards sets `self+4 = 2`, `*(float*)(self+8) = 0x00B3A164` and runs
// the module's own loading work (0x0064ADA0 / 0x0064B230 / 0x0064B940 / 0x0064A7D0) - see
// probe_fade.cpp, which hooks three of those directly.
//
// This hook is diagnostic only, installed unconditionally whenever "-al" is used: it logs the first
// time the game calls this entry with each phase (0-6), never more than once per phase per process.
// That is how phase 2 was confirmed as the title's own value and phase 5 as the game's own startup
// registration - and it is what proved that forging the save manager's fields directly (the ManagerLoad
// experiment, since removed) could never work: the flow was never told to start a load, so its
// session-entry callback had nowhere to land. The title route (title_load.cpp) goes through this same
// entry via the menu's own update, not through anything in this file.
#include "al.h"
#include "mod_api.h"

#include <cstdarg>
#include <cstdio>

namespace al {

namespace {

void (*g_log)(const char*) = nullptr;
LONG g_flow_phase_bits = 0;              // one bit per phase, so each phase is logged once

void Log(const char* fmt, ...) {
    if (g_log == nullptr) return;
    char buf[400];
    va_list args;
    va_start(args, fmt);
    _vsnprintf(buf, sizeof(buf) - 1, fmt, args);
    va_end(args);
    buf[sizeof(buf) - 1] = '\0';
    g_log(buf);
}

bool Readable(const void* p, size_t n) {
    if (p == nullptr) return false;
    MEMORY_BASIC_INFORMATION mbi;
    if (VirtualQuery(p, &mbi, sizeof(mbi)) == 0) return false;
    if (mbi.State != MEM_COMMIT) return false;
    if ((mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS)) != 0) return false;
    return static_cast<const uint8_t*>(p) + n
           <= static_cast<const uint8_t*>(mbi.BaseAddress) + mbi.RegionSize;
}

}  // namespace

void SetFlowLog(void* log_function) {
    g_log = reinterpret_cast<void (*)(const char*)>(log_function);
}

// Entry-only observer: save the flags/registers, report, jump to the original code. Nothing is
// changed and nothing is allocated on the game's thread.
extern "C" {
void* g_flow_trampoline_ptr = nullptr;
void atmt_flow_entry(void* snapshot);

__attribute__((naked, used)) void atmt_detour_flow() {
    __asm__ volatile(
        "pushfl\n\t"
        "pushal\n\t"
        "pushl $0\n\t"
        "pushl %esp\n\t"
        "call _atmt_flow_entry\n\t"
        "addl $8, %esp\n\t"
        "popal\n\t"
        "popfl\n\t"
        "jmp *_g_flow_trampoline_ptr\n\t");
}

void atmt_flow_entry(void* snapshot) {
    uintptr_t* snap = static_cast<uintptr_t*>(snapshot);
    const uintptr_t self = snap[7];       // ecx (__thiscall)
    const uintptr_t stack = snap[4];      // esp as saved (points at the saved eflags)
    if (!Readable(reinterpret_cast<const void*>(stack), 0x10) || self < 0x10000) return;
    const uintptr_t phase = *reinterpret_cast<const uint32_t*>(stack + 8);
    if (phase > 6) return;                // the switch accepts 0..6; anything else is a misread stack
    const LONG bit = 1L << phase;
    if ((InterlockedOr(&g_flow_phase_bits, bit) & bit) == 0) {
        Log("flow: the game itself called 0x%08x with phase %u (self=0x%08x)",
            static_cast<unsigned>(kGameFlowEntry), static_cast<unsigned>(phase),
            static_cast<unsigned>(self));
    }
}
}  // extern "C"

bool InstallFlowHook(const AtmtModApi* api, std::string* why_not) {
    if (api == nullptr || api->hook_create == nullptr || api->hook_enable == nullptr) {
        if (why_not != nullptr) *why_not = "the loader has no hook service";
        return false;
    }
    void* trampoline = nullptr;
    void* handle = api->hook_create(reinterpret_cast<void*>(kGameFlowEntry),
                                   reinterpret_cast<void*>(&atmt_detour_flow), &trampoline);
    if (handle == nullptr) {
        if (why_not != nullptr) *why_not = "could not hook the game's flow entry";
        return false;
    }
    g_flow_trampoline_ptr = trampoline;
    SetFlowLog(reinterpret_cast<void*>(api->log));
    api->hook_enable(handle);
    Log("flow: watching 0x%08x (the game's own save/load flow entry)",
        static_cast<unsigned>(kGameFlowEntry));
    return true;
}

}  // namespace al
