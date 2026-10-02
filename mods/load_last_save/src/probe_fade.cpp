// probe_fade.cpp - what's behind the fades the title route's load goes through.
//
// Off unless `ProbeFade=true` in the mod's ini. The title route calls the game's real save/load menu
// (see title_load.cpp), whose own flow entry (FUN_0064CAD0, probe_flow.cpp) primes the load and then
// unconditionally runs a handful of the game's own functions before handing control back - found by
// resolving the manager's vtable thunks offline (against the exe on disk, which is
// byte-for-byte the running exe: no ASLR) and decompiling what they land on (see al.h for the three
// addresses and what each decompile showed):
//
//   kFadeWidgetKick (0x0064A7D0)  loops over ~20 UI widgets; for each, reads its current value, resets
//                                 it to 0, then animates it to that value over a shared duration global
//                                 (kFadeDurationGlobal, measured offline as the float 0.2). This is the
//                                 one concrete, single-lever candidate for "the fade takes N ms".
//   kFadeListBuild (0x0064B940)   builds the visible save-slot list - thumbnails (fopen "thumbNNN.bmp"
//                                 per slot), the date/time text, autosave naming. Large, does file I/O
//                                 in a loop, so it is the candidate for a real (non-fade) stall rather
//                                 than an animated transition.
//   kFadePrimeSetter (0x00453F70) the vtable+0xc call FUN_0064CAD0 makes before any of the above, with
//                                 500 for the title's phase (2) or 400 for the others - written into a
//                                 field on the flow object (self+0x1c), not obviously a duration, but
//                                 logged so a real number from the game replaces a decompile guess.
//
// All three hooks are entry-only and tail-jump into the trampoline exactly like probe_flow.cpp and
// probe_reader.cpp: nothing about the game's behaviour changes, no calling convention is disturbed, and
// this is what decides which of the three to actually touch (docs/MODS.md, "Autoload") instead
// of patching kFadeDurationGlobal blind.
#include "al.h"
#include "mod_api.h"

#include <cstdarg>
#include <cstdio>

namespace al {

namespace {

void (*g_log)(const char*) = nullptr;
LONG g_widget_calls = 0;
LONG g_list_calls = 0;
LONG g_prime_calls = 0;

void Log(const char* fmt, ...) {
    if (g_log == nullptr) return;
    char buf[300];
    int used = 0;
    const unsigned long start = TitleLoadStartTick();
    if (start != 0) {
        const int n = _snprintf(buf, sizeof(buf) - 1, "[+%lums] ", GetTickCount() - start);
        if (n > 0) used = n;
    }
    va_list args;
    va_start(args, fmt);
    _vsnprintf(buf + used, sizeof(buf) - 1 - static_cast<size_t>(used), fmt, args);
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

// ---------------------------------------------------------------- three entry-only observers
// Same shape throughout (probe_flow.cpp, probe_reader.cpp): pushfl/pushal, a snapshot pointer, call a
// C function, restore, jump into the trampoline. The snapshot layout (see probe_flow.cpp's comment):
// snap[7] = ECX (the object, thiscall/fastcall "this"), snap[4]+4 = the hooked function's own entry
// ESP (return address, then any stack arguments after it).
extern "C" {
void* g_widget_trampoline_ptr = nullptr;
void* g_list_trampoline_ptr = nullptr;
void* g_prime_trampoline_ptr = nullptr;
void atmt_widget_entry(void* snapshot);
void atmt_list_entry(void* snapshot);
void atmt_prime_entry(void* snapshot);

__attribute__((naked, used)) void atmt_detour_widget() {
    __asm__ volatile(
        "pushfl\n\t"
        "pushal\n\t"
        "pushl $0\n\t"
        "pushl %esp\n\t"
        "call _atmt_widget_entry\n\t"
        "addl $8, %esp\n\t"
        "popal\n\t"
        "popfl\n\t"
        "jmp *_g_widget_trampoline_ptr\n\t");
}

__attribute__((naked, used)) void atmt_detour_list() {
    __asm__ volatile(
        "pushfl\n\t"
        "pushal\n\t"
        "pushl $0\n\t"
        "pushl %esp\n\t"
        "call _atmt_list_entry\n\t"
        "addl $8, %esp\n\t"
        "popal\n\t"
        "popfl\n\t"
        "jmp *_g_list_trampoline_ptr\n\t");
}

__attribute__((naked, used)) void atmt_detour_prime() {
    __asm__ volatile(
        "pushfl\n\t"
        "pushal\n\t"
        "pushl $0\n\t"
        "pushl %esp\n\t"
        "call _atmt_prime_entry\n\t"
        "addl $8, %esp\n\t"
        "popal\n\t"
        "popfl\n\t"
        "jmp *_g_prime_trampoline_ptr\n\t");
}

void atmt_widget_entry(void* snapshot) {
    InterlockedIncrement(&g_widget_calls);
    const uintptr_t* snap = static_cast<const uintptr_t*>(snapshot);
    const uintptr_t self = snap[7];
    const float duration = Readable(reinterpret_cast<const void*>(kFadeDurationGlobal), 4)
                                ? *reinterpret_cast<const float*>(kFadeDurationGlobal)
                                : -1.0f;
    Log("fade: widget fade-in kicked (0x%08x) call #%ld self=0x%08x duration=%.3f",
        static_cast<unsigned>(kFadeWidgetKick), g_widget_calls, static_cast<unsigned>(self),
        duration);
}

void atmt_list_entry(void* snapshot) {
    InterlockedIncrement(&g_list_calls);
    const uintptr_t* snap = static_cast<const uintptr_t*>(snapshot);
    const uintptr_t self = snap[7];
    const uintptr_t entry_esp = snap[4] + 4;
    float dt = 0.0f;
    if (Readable(reinterpret_cast<const void*>(entry_esp + 4), 4)) {
        dt = *reinterpret_cast<const float*>(entry_esp + 4);
    }
    Log("fade: save-list build entered (0x%08x) call #%ld self=0x%08x dt=%.4f",
        static_cast<unsigned>(kFadeListBuild), g_list_calls, static_cast<unsigned>(self), dt);
}

void atmt_prime_entry(void* snapshot) {
    InterlockedIncrement(&g_prime_calls);
    const uintptr_t* snap = static_cast<const uintptr_t*>(snapshot);
    const uintptr_t self = snap[7];
    const uintptr_t entry_esp = snap[4] + 4;
    uint32_t arg = 0;
    if (Readable(reinterpret_cast<const void*>(entry_esp + 4), 4)) {
        arg = *reinterpret_cast<const uint32_t*>(entry_esp + 4);
    }
    Log("fade: prime setter (0x%08x) call #%ld self=0x%08x arg=%u", static_cast<unsigned>(kFadePrimeSetter),
        g_prime_calls, static_cast<unsigned>(self), arg);
}
}  // extern "C"

bool InstallFadeProbe(const AtmtModApi* api, std::string* why_not) {
    if (api == nullptr || api->hook_create == nullptr || api->hook_enable == nullptr) {
        if (why_not != nullptr) *why_not = "the loader has no hook service";
        return false;
    }
    g_log = reinterpret_cast<void (*)(const char*)>(api->log);

    void* trampoline = nullptr;
    void* handle = api->hook_create(reinterpret_cast<void*>(kFadeWidgetKick),
                                    reinterpret_cast<void*>(&atmt_detour_widget), &trampoline);
    if (handle == nullptr) {
        if (why_not != nullptr) *why_not = "could not hook the widget fade-in (0x0064A7D0)";
        return false;
    }
    g_widget_trampoline_ptr = trampoline;
    api->hook_enable(handle);

    trampoline = nullptr;
    handle = api->hook_create(reinterpret_cast<void*>(kFadeListBuild),
                              reinterpret_cast<void*>(&atmt_detour_list), &trampoline);
    if (handle == nullptr) {
        if (why_not != nullptr) *why_not = "could not hook the save-list build (0x0064B940)";
        return false;
    }
    g_list_trampoline_ptr = trampoline;
    api->hook_enable(handle);

    trampoline = nullptr;
    handle = api->hook_create(reinterpret_cast<void*>(kFadePrimeSetter),
                              reinterpret_cast<void*>(&atmt_detour_prime), &trampoline);
    if (handle == nullptr) {
        if (why_not != nullptr) *why_not = "could not hook the prime setter (0x00453F70)";
        return false;
    }
    g_prime_trampoline_ptr = trampoline;
    api->hook_enable(handle);

    Log("fade: watching 0x%08x (widget fade-in), 0x%08x (save-list build) and 0x%08x (prime setter)",
        static_cast<unsigned>(kFadeWidgetKick), static_cast<unsigned>(kFadeListBuild),
        static_cast<unsigned>(kFadePrimeSetter));
    return true;
}

}  // namespace al
