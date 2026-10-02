// probe_memset.cpp - who calls memset with a destination that cannot work?
//
// Off unless `ProbeMemset=true`. The "-al" load crashed inside msvcr100's memset (fault offset
// 0x2b84, the byte-writing loop), which is what happens when the destination is null and the count is
// large. Setting the manager's own buffer field was not enough, so this records the call itself: the
// destination, the count and the return address of whoever asked for it.
//
// Only dangerous-looking calls are logged (null destination, or a count larger than anything the game
// allocates), so the log stays readable - memset is called constantly in normal operation.
//
// Pass-through hook: the arguments are untouched and the return value is the original's.
#include "al.h"
#include "mod_api.h"

#include <cstdarg>
#include <cstdio>

namespace al {

namespace {

void (*g_log)(const char*) = nullptr;
volatile LONG g_logged = 0;

void Log(const char* fmt, ...) {
    if (g_log == nullptr) return;
    char buf[320];
    va_list args;
    va_start(args, fmt);
    _vsnprintf(buf, sizeof(buf) - 1, fmt, args);
    va_end(args);
    buf[sizeof(buf) - 1] = '\0';
    g_log(buf);
}

// snapshot layout (see probe_reader.cpp): [1..8] = edi,esi,ebp,esp,ebx,edx,ecx,eax, [9] = eflags.
// The stack argument is ESP as saved by pushad, which points at the saved eflags: the return address
// is 4 bytes above it and the first argument 8.
void Report(void* snapshot) {
    uintptr_t* snap = static_cast<uintptr_t*>(snapshot);
    const uintptr_t stack = snap[4];
    MEMORY_BASIC_INFORMATION mbi;
    if (VirtualQuery(reinterpret_cast<const void*>(stack), &mbi, sizeof(mbi)) == 0) return;
    if (mbi.State != MEM_COMMIT) return;
    const uintptr_t ret = *reinterpret_cast<const uint32_t*>(stack + 4);
    const uint32_t dst = *reinterpret_cast<const uint32_t*>(stack + 8);
    const uint32_t value = *reinterpret_cast<const uint32_t*>(stack + 0xc);
    const uint32_t count = *reinterpret_cast<const uint32_t*>(stack + 0x10);
    // Log only calls that cannot work: a destination that is not committed writable memory for at
    // least the first page of the write, or a count larger than anything the game allocates. A plain
    // null test missed the real one (its destination was non-zero but unwritable), so the check is on
    // the memory itself.
    bool bad = false;
    MEMORY_BASIC_INFORMATION dst_mbi;
    if (VirtualQuery(reinterpret_cast<const void*>(dst), &dst_mbi, sizeof(dst_mbi)) == 0) {
        bad = true;
    } else if (dst_mbi.State != MEM_COMMIT
               || (dst_mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) != 0
               || (dst_mbi.Protect & (PAGE_READONLY | PAGE_EXECUTE)) != 0) {
        bad = true;
    } else {
        const uintptr_t end = reinterpret_cast<uintptr_t>(dst_mbi.BaseAddress) + dst_mbi.RegionSize;
        // Check the whole write (bounded), not just the first page: a destination that is valid for
        // 4 KB but has a 460 KB count in a smaller region is exactly the shape that killed the load.
        const uintptr_t want = count < 0x200000 ? count : 0x200000;
        if (static_cast<uintptr_t>(dst) + want > end) bad = true;
    }
    if (count >= 0x10000) bad = true;   // large writes are worth seeing whatever the destination
    if (!bad) return;
    if (InterlockedIncrement(&g_logged) > 40) return;
    Log("memset dst=0x%08x value=0x%02x count=%u caller=0x%08x eax=0x%08x ebx=0x%08x edi=0x%08x",
        dst, value, count, static_cast<unsigned>(ret), static_cast<unsigned>(snap[8]),
        static_cast<unsigned>(snap[5]), static_cast<unsigned>(snap[1]));
}

}  // namespace

}  // namespace al

extern "C" {
void* g_memset_trampoline = nullptr;
void atmt_memset_entry(void* snapshot);

__attribute__((naked, used)) void atmt_detour_memset() {
    __asm__ volatile(
        "pushfl\n\t"
        "pushal\n\t"
        "pushl $0\n\t"
        "pushl %esp\n\t"
        "call _atmt_memset_entry\n\t"
        "addl $8, %esp\n\t"
        "popal\n\t"
        "popfl\n\t"
        "jmp *_g_memset_trampoline\n\t");
}

void atmt_memset_entry(void* snapshot) {
    al::Report(snapshot);
}
}  // extern "C"

namespace al {

bool InstallMemsetProbe(const AtmtModApi* api, std::string* why_not) {
    if (api == nullptr || api->hook_create == nullptr || api->hook_enable == nullptr) {
        if (why_not != nullptr) *why_not = "the loader has no hook service";
        return false;
    }
    g_log = api->log;
    HMODULE crt = GetModuleHandleA("msvcr100.dll");
    if (crt == nullptr) crt = LoadLibraryA("msvcr100.dll");
    if (crt == nullptr) {
        if (why_not != nullptr) *why_not = "msvcr100.dll is not loaded";
        return false;
    }
    void* target = reinterpret_cast<void*>(GetProcAddress(crt, "memset"));
    if (target == nullptr) {
        if (why_not != nullptr) *why_not = "memset is not exported";
        return false;
    }
    void* trampoline = nullptr;
    void* handle = api->hook_create(target, reinterpret_cast<void*>(&atmt_detour_memset), &trampoline);
    if (handle == nullptr) {
        if (why_not != nullptr) *why_not = "could not hook memset";
        return false;
    }
    g_memset_trampoline = trampoline;
    api->hook_enable(handle);
    Log("memset probe: watching msvcr100!memset at 0x%08x (only suspicious calls are logged)",
        static_cast<unsigned>(reinterpret_cast<uintptr_t>(target)));
    return true;
}

}  // namespace al
