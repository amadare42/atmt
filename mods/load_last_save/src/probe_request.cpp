// probe_request.cpp - what does the game pass when it asks for a save operation?
//
// Off unless `ProbeRequest=true`. Static analysis pinned the request down to FUN_00484870
// (thiscall + 5 stack arguments, `ret 0x14`):
//
//   FUN_00484870(this = manager /*ecx*/, arg0, arg1, arg2 = "<dir>/saveNNN.dat", arg3, arg4) {
//       idx = [0x01369ff4](arg0, 0, 0);        // slot lookup; -1 => the call returns 9
//       strncpy(0x00C3E758, arg2, 0x103);      // the file path -> the name buffer
//       [0x00C3E750] = this;  [0x00C3E754] = arg1;
//       [0x00C3E864] = idx;   [0x00C3E85B] = 0;
//       [0x00C3E85C] = arg3;  [0x00C3E860] = arg4;
//       [0x00C3E868] = 1;                      // the request flag the worker waits on
//   }
//
// The fallback route (trigger.cpp, used only if the title route's hook fails) writes the name buffer
// and the flag itself, which gets the file *read* and nothing more, because the state block
// (0xC3E750/54/5C/60/64) stays as it was - entering the game needs the real menu (title_load.cpp).
// This probe exists to see what the game's own request looks like: it records the arguments from a
// real load (the menu), plus the two function pointers it goes through.
//
// Observation only: the detour saves the flags/registers, logs, and jumps to the trampoline.
#include "al.h"
#include "mod_api.h"

#include <cstdarg>
#include <cstdio>

namespace al {

namespace {

void* g_trampoline = nullptr;
void (*g_log)(const char*) = nullptr;
volatile LONG g_seen = 0;

void Log(const char* fmt, ...) {
    if (g_log == nullptr) return;
    char buf[512];
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

std::string Str(uintptr_t p) {
    std::string out;
    if (p < 0x10000) return out;
    for (int i = 0; i < 256; ++i) {
        if (i % 32 == 0 && !Readable(reinterpret_cast<const void*>(p + i), 1)) break;
        const char c = *reinterpret_cast<const char*>(p + i);
        if (c == '\0') break;
        if (static_cast<unsigned char>(c) < 0x20) break;
        out.push_back(c);
    }
    return out;
}

// Runs on the game's thread with the registers saved: read the 5 arguments off the stack and
// record them. Nothing here allocates or touches game state.
void Report(void* snapshot) {
    uintptr_t* snap = static_cast<uintptr_t*>(snapshot);
    const uintptr_t this_ptr = snap[7];                 // ecx (thiscall)
    const uintptr_t stack = snap[4];                    // esp as saved (points at the saved eflags)
    if (!Readable(reinterpret_cast<const void*>(stack), 0x20)) return;
    const uintptr_t ret = *reinterpret_cast<const uint32_t*>(stack + 4);
    const uintptr_t a0 = *reinterpret_cast<const uint32_t*>(stack + 8);
    const uintptr_t a1 = *reinterpret_cast<const uint32_t*>(stack + 0xc);
    const uintptr_t a2 = *reinterpret_cast<const uint32_t*>(stack + 0x10);
    const uintptr_t a3 = *reinterpret_cast<const uint32_t*>(stack + 0x14);
    const uintptr_t a4 = *reinterpret_cast<const uint32_t*>(stack + 0x18);
    const LONG n = InterlockedIncrement(&g_seen);
    // The first calls are the game's own startup (registering the system slot); the interesting
    // one is whatever the menu does, so the sequence number is part of the record.
    Log("request #%ld: this=0x%08x arg0=0x%08x arg1=0x%08x arg2=0x%08x(\"%s\") arg3=0x%08x "
        "arg4=0x%08x  ret=0x%08x",
        static_cast<long>(n), static_cast<unsigned>(this_ptr), static_cast<unsigned>(a0),
        static_cast<unsigned>(a1), static_cast<unsigned>(a2), Str(a2).c_str(),
        static_cast<unsigned>(a3), static_cast<unsigned>(a4), static_cast<unsigned>(ret));
    if (n == 1) {
        // The two indirect calls the function makes: knowing them lets the mod call through the
        // same code (they are set by the game at runtime, so they cannot be read from the file).
        const uintptr_t* slot_lookup = reinterpret_cast<const uintptr_t*>(kGameSlotLookupPtr);
        const uintptr_t* str_copy = reinterpret_cast<const uintptr_t*>(kGameStrCopyPtr);
        Log("request: [0x%08x]=0x%08x (slot lookup)   [0x%08x]=0x%08x (string copy)   "
            "manager[0x%08x]=0x%08x",
            static_cast<unsigned>(kGameSlotLookupPtr), static_cast<unsigned>(*slot_lookup),
            static_cast<unsigned>(kGameStrCopyPtr), static_cast<unsigned>(*str_copy),
            static_cast<unsigned>(kGameSaveManagerPtr),
            static_cast<unsigned>(*reinterpret_cast<const uintptr_t*>(kGameSaveManagerPtr)));
    }
}

}  // namespace

// Named in namespace al so the extern "C" wrapper below can call it (the naked detour needs an
// unmangled symbol, exactly as probe_reader.cpp does).
void ReportRequestSnapshot(void* snapshot) {
    Report(snapshot);
}

extern "C" {
void* g_request_trampoline_ptr = nullptr;
void atmt_request_snapshot(void* snapshot);

__attribute__((naked, used)) void atmt_detour_request() {
    __asm__ volatile(
        "pushfl\n\t"
        "pushal\n\t"
        "pushl $0\n\t"
        "pushl %esp\n\t"
        "call _atmt_request_snapshot\n\t"
        "addl $8, %esp\n\t"
        "popal\n\t"
        "popfl\n\t"
        "jmp *_g_request_trampoline_ptr\n\t");
}

void atmt_request_snapshot(void* snapshot) {
    al::ReportRequestSnapshot(snapshot);
}
}  // extern "C"

bool InstallRequestProbe(const AtmtModApi* api, std::string* why_not) {
    if (api == nullptr || api->hook_create == nullptr || api->hook_enable == nullptr) {
        if (why_not != nullptr) *why_not = "the loader has no hook service";
        return false;
    }
    g_log = api->log;
    void* trampoline = nullptr;
    void* handle = api->hook_create(reinterpret_cast<void*>(kGameSaveRequest),
                                    reinterpret_cast<void*>(&atmt_detour_request), &trampoline);
    if (handle == nullptr) {
        if (why_not != nullptr) *why_not = "could not hook the request function";
        return false;
    }
    g_request_trampoline_ptr = trampoline;
    g_trampoline = trampoline;
    api->hook_enable(handle);
    Log("request probe: watching 0x%08x (the game's save/load request)",
        static_cast<unsigned>(kGameSaveRequest));
    return true;
}

void* RequestTrampoline() {
    return g_trampoline;
}

}  // namespace al

