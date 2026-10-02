// probe_fopen.cpp - which game function opens a save file? (diagnostic; ProbeFopen=true)
//
// The file probe showed that a menu load opens the slot with the CRT ( `CreateFileA` caller =
// 0x5ec27359, inside msvcr100), so the game-side caller is one frame further up. Hooking the CRT's
// public entry points gives that frame directly - the same technique that found the dialog text
// parser (the game calls into the CRT, so the CRT entry is where the game's own code is the caller).
//
// It logs the path, the immediate caller and the caller chain, and is read-only.
#include "al.h"
#include "mod_api.h"

#include <cstdarg>
#include <cstdio>
#include <string>

namespace al {

namespace {

const AtmtModApi* g_api = nullptr;
volatile LONG g_in_probe = 0;
volatile LONG g_seen = 0;
constexpr LONG kMaxRecords = 120;

void* g_tramp_fopen = nullptr;
void* g_tramp_fsopen = nullptr;
void* g_tramp_fopen_s = nullptr;

void Log(const char* fmt, ...) {
    if (g_api == nullptr || g_api->log == nullptr) return;
    char buf[700];
    va_list args;
    va_start(args, fmt);
    _vsnprintf(buf, sizeof(buf) - 1, fmt, args);
    va_end(args);
    buf[sizeof(buf) - 1] = '\0';
    g_api->log(buf);
}

bool Readable(const void* p, size_t n) {
    if (p == nullptr) return false;
    MEMORY_BASIC_INFORMATION mbi;
    if (VirtualQuery(p, &mbi, sizeof(mbi)) == 0) return false;
    if (mbi.State != MEM_COMMIT) return false;
    if ((mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS)) != 0) return false;
    const uint8_t* start = static_cast<const uint8_t*>(p);
    const uint8_t* limit = static_cast<const uint8_t*>(mbi.BaseAddress) + mbi.RegionSize;
    return start + n <= limit;
}

std::string CString(uintptr_t p, unsigned max_len) {
    std::string out;
    if (p < 0x10000) return out;
    for (unsigned i = 0; i < max_len; ++i) {
        if (i % 32 == 0 && !Readable(reinterpret_cast<const void*>(p + i), 1)) break;
        const char c = *reinterpret_cast<const char*>(p + i);
        if (c == '\0') break;
        out.push_back(c);
    }
    return out;
}

void Probe(const char* which, const void* snapshot) {
    if (InterlockedCompareExchange(&g_in_probe, 1, 0) != 0) return;
    struct Guard {
        ~Guard() { InterlockedExchange(&g_in_probe, 0); }
    } guard;
    if (InterlockedIncrement(&g_seen) > kMaxRecords) return;

    // Same geometry as the dialog logger's hook (see addr_hook.cpp): snap[1+3] is the saved ESP
    // plus the pushed eflags, so the return address is at +0 and the first argument at +4.
    const uintptr_t* snap = static_cast<const uintptr_t*>(snapshot);
    const uintptr_t entry_esp = snap[1 + 3] + 4;
    if (!Readable(reinterpret_cast<const void*>(entry_esp), 12)) return;
    const uintptr_t caller = *reinterpret_cast<const uint32_t*>(entry_esp);
    const uintptr_t arg0 = *reinterpret_cast<const uint32_t*>(entry_esp + 4);
    const uintptr_t arg1 = *reinterpret_cast<const uint32_t*>(entry_esp + 8);

    const std::string path = CString(arg0, 260);
    // Only save files are interesting: the game opens plenty of other things through the CRT.
    const bool interesting =
        path.find("save") != std::string::npos || path.find(".dat") != std::string::npos;
    if (!interesting) return;

    const std::string frames = HookStackFrames(snapshot, 6);
    Log("fopen: %s(\"%s\", \"%s\") caller=0x%08x game: %s", which, path.c_str(),
        CString(arg1, 8).c_str(), static_cast<unsigned>(caller), frames.c_str());
}

}  // namespace

extern "C" {
void* g_fopen_tramp = nullptr;
void* g_fsopen_tramp = nullptr;
void* g_fopen_s_tramp = nullptr;
void probe_fopen_entry(void* snapshot);
void probe_fsopen_entry(void* snapshot);
void probe_fopen_s_entry(void* snapshot);

__attribute__((naked, used)) void detour_fopen() {
    __asm__ volatile(
        "pushfl\n\t"
        "pushal\n\t"
        "pushl $0\n\t"
        "pushl %esp\n\t"
        "call _probe_fopen_entry\n\t"
        "addl $8, %esp\n\t"
        "popal\n\t"
        "popfl\n\t"
        "jmp *_g_fopen_tramp\n\t");
}

__attribute__((naked, used)) void detour_fsopen() {
    __asm__ volatile(
        "pushfl\n\t"
        "pushal\n\t"
        "pushl $0\n\t"
        "pushl %esp\n\t"
        "call _probe_fsopen_entry\n\t"
        "addl $8, %esp\n\t"
        "popal\n\t"
        "popfl\n\t"
        "jmp *_g_fsopen_tramp\n\t");
}

__attribute__((naked, used)) void detour_fopen_s() {
    __asm__ volatile(
        "pushfl\n\t"
        "pushal\n\t"
        "pushl $0\n\t"
        "pushl %esp\n\t"
        "call _probe_fopen_s_entry\n\t"
        "addl $8, %esp\n\t"
        "popal\n\t"
        "popfl\n\t"
        "jmp *_g_fopen_s_tramp\n\t");
}

void probe_fopen_entry(void* snapshot) { Probe("fopen", snapshot); }
void probe_fsopen_entry(void* snapshot) { Probe("_fsopen", snapshot); }
void probe_fopen_s_entry(void* snapshot) { Probe("fopen_s", snapshot); }
}  // extern "C"

namespace {

// Hooks one CRT entry point. The loader's hook service refuses addresses that are not executable
// code, so a missing export is reported rather than patched blindly.
bool InstallOne(const AtmtModApi* api, const char* dll, const char* name, void* detour,
                void** tramp_out, void** tramp_global, std::string* why_not) {
    HMODULE mod = GetModuleHandleA(dll);
    if (mod == nullptr) mod = LoadLibraryA(dll);
    if (mod == nullptr) {
        if (why_not != nullptr) *why_not += std::string(" (") + dll + " not loaded)";
        return false;
    }
    void* target = reinterpret_cast<void*>(GetProcAddress(mod, name));
    if (target == nullptr) {
        if (why_not != nullptr) *why_not += std::string(" (no export ") + name + ")";
        return false;
    }
    void* trampoline = nullptr;
    // The loader's hook service returns the target address as the handle, and nullptr means the
    // hook was refused (see loader_hooks.cpp). Testing "!= 0" here would report success as failure.
    void* handle = api->hook_create(target, detour, &trampoline);
    if (handle == nullptr) {
        if (why_not != nullptr) *why_not += std::string(" (hook refused: ") + name + ")";
        return false;
    }
    *tramp_global = trampoline;
    *tramp_out = trampoline;
    if (api->hook_enable != nullptr) api->hook_enable(handle);
    return true;
}

}  // namespace

bool InstallFopenProbe(const AtmtModApi* api, std::string* why_not) {
    if (api == nullptr || api->hook_create == nullptr) {
        if (why_not != nullptr) *why_not = "no hook service";
        return false;
    }
    g_api = api;
    const char* crt = "msvcr100.dll";
    int installed = 0;
    std::string detail;
    if (InstallOne(api, crt, "fopen", reinterpret_cast<void*>(&detour_fopen), &g_tramp_fopen,
                   &g_fopen_tramp, &detail)) {
        ++installed;
    }
    if (InstallOne(api, crt, "_fsopen", reinterpret_cast<void*>(&detour_fsopen), &g_tramp_fsopen,
                   &g_fsopen_tramp, &detail)) {
        ++installed;
    }
    if (InstallOne(api, crt, "fopen_s", reinterpret_cast<void*>(&detour_fopen_s),
                   &g_tramp_fopen_s, &g_fopen_s_tramp, &detail)) {
        ++installed;
    }
    if (installed == 0) {
        if (why_not != nullptr) *why_not = "no CRT entry point could be hooked" + detail;
        return false;
    }
    Log("fopen probe: %d CRT entry point(s) hooked (%s)", installed, detail.c_str());
    return true;
}

}  // namespace al

