// probe.cpp - a diagnostic: which saves does the game touch, and from which code?
//
// Off unless `ProbeFileApis=true` in the mod's ini. It answers the one question this feature
// needs before it can ask the game to load anything: WHERE does the game read saves, and which
// ed8.exe code does it (the caller address of the file call)?
//
// The game imports CreateFileW/CreateFileA and ReadFile but *no* FindFirstFile, so its save
// listing cannot come from scanning the folder - it reads sdslot.dat (the 64 x 5760 byte slot
// table) and then the individual slots. The probe logs:
//
//   probe: CreateFileW "???\ed8\save012.dat" access=0x80000000 disp=3 -> 0x00000123 caller=0x0051A2B4
//   probe: ReadFile   "???\ed8\save012.dat" asked=460992 got=460992 -> 1 caller=0x0051B0C7
//
// The caller address is the evidence: it goes into Ghidra (ghidra/proj/cs1) and is confirmed
// with an `exec` breakpoint before anything is patched (the rule from the earlier crash).
#include "al.h"
#include "mod_api.h"

#include <cstdarg>
#include <cstdio>
#include <cwchar>

namespace al {

namespace {

const AtmtModApi* g_api = nullptr;
volatile LONG g_lines = 0;
DWORD g_start_ms = 0;
const LONG kMaxLines = 1200;   // one load action is a few dozen lines; this leaves room for several

typedef HANDLE(WINAPI* CreateFileW_t)(LPCWSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD,
                                      HANDLE);
typedef HANDLE(WINAPI* CreateFileA_t)(LPCSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD,
                                      HANDLE);
typedef BOOL(WINAPI* ReadFile_t)(HANDLE, LPVOID, DWORD, LPDWORD, LPOVERLAPPED);
typedef size_t(__cdecl* fread_t)(void*, size_t, size_t, FILE*);
typedef int(__cdecl* read_t)(int, void*, unsigned);

CreateFileW_t g_real_CreateFileW = nullptr;
CreateFileA_t g_real_CreateFileA = nullptr;
ReadFile_t g_real_ReadFile = nullptr;
fread_t g_real_fread = nullptr;
read_t g_real_read = nullptr;

void LogLine(const char* fmt, ...) {
    if (g_api == nullptr || g_api->log == nullptr) return;
    if (InterlockedIncrement(&g_lines) > kMaxLines) {
        if (g_lines == kMaxLines + 1) g_api->log("log cap reached, further calls not logged");
        return;
    }
    char buf[640];
    va_list args;
    va_start(args, fmt);
    _vsnprintf(buf, sizeof(buf) - 1, fmt, args);
    va_end(args);
    buf[sizeof(buf) - 1] = '\0';
    // Elapsed milliseconds since the probe was installed: the loader's log has no timestamps, and
    // this is what tells the game's startup reads apart from the ones the load menu causes.
    char stamped[680];
    const DWORD elapsed = g_start_ms != 0 ? GetTickCount() - g_start_ms : 0;
    _snprintf(stamped, sizeof(stamped) - 1, "probe: [+%lu ms] %s",
              static_cast<unsigned long>(elapsed), buf);
    stamped[sizeof(stamped) - 1] = '\0';
    g_api->log(stamped);
}

std::string Narrow(const wchar_t* w) {
    std::string out;
    for (const wchar_t* p = w; p != nullptr && *p != L'\0'; ++p) {
        out.push_back(*p < 128 ? static_cast<char>(*p) : '?');
    }
    return out;
}

// Only what belongs to the save system: the game's own data files also end in .dat (scripts) and
// carry "save" in their names (data/system_us/saveicon.bmp), so those are excluded outright.
bool InterestingW(const wchar_t* path) {
    if (path == nullptr) return false;
    if (_wcsnicmp(path, L"data\\", 5) == 0 || _wcsnicmp(path, L"data/", 5) == 0) return false;
    // Deliberately no "thumb": the game probes 300+ thumbnail names that do not exist, which is
    // noise (and it once ate the whole log budget before the interesting calls happened).
    const wchar_t* needles[] = {L"save", L"sdslot", L"Falcom"};
    for (const wchar_t* n : needles) {
        if (wcsstr(path, n) != nullptr) return true;
    }
    return false;
}

bool InterestingA(const char* path) {
    if (path == nullptr) return false;
    std::wstring wide;
    for (const char* p = path; *p != '\0'; ++p) wide.push_back(static_cast<wchar_t>(*p));
    return InterestingW(wide.c_str());
}

std::wstring Widen(const char* s) {
    std::wstring wide;
    for (const char* p = s; p != nullptr && *p != '\0'; ++p) {
        wide.push_back(static_cast<wchar_t>(*p));
    }
    return wide;
}

// The game reaches everything through jump thunks (0x40xxxx stubs), and a lot of file I/O also
// goes through the CRT, so the immediate caller is often not the interesting one. This scans the
// stack for return addresses that land inside ed8.exe - a poor man's backtrace, which is enough
// to see which game code asked for the file.
//
// The first version of this printed nothing: it read SizeOfImage from the wrong place in the PE
// header, so the "inside the game" test only covered the first megabyte of a much larger image.
// VirtualQuery + AllocationBase is exact and needs no header parsing.
void Backtrace(unsigned max_frames, unsigned char* out, size_t out_size) {
    out[0] = '\0';
    if (g_api == nullptr || g_api->game_module == nullptr) return;
    const uintptr_t base = reinterpret_cast<uintptr_t>(g_api->game_module);

    volatile uintptr_t anchor = 0;
    const uintptr_t* stack = const_cast<const uintptr_t*>(&anchor);
    unsigned found = 0;
    char* w = reinterpret_cast<char*>(out);
    for (unsigned i = 0; i < 256 && found < max_frames; ++i) {
        const uintptr_t value = stack[i];
        if (value < 0x10000) continue;
        MEMORY_BASIC_INFORMATION mbi;
        if (VirtualQuery(reinterpret_cast<const void*>(value), &mbi, sizeof(mbi)) == 0) continue;
        if (mbi.State != MEM_COMMIT || mbi.Type != MEM_IMAGE) continue;
        if (reinterpret_cast<uintptr_t>(mbi.AllocationBase) != base) continue;   // ed8.exe itself
        const int written = _snprintf(w, out_size - (w - reinterpret_cast<char*>(out)), " 0x%08x",
                                      static_cast<unsigned>(value));
        if (written <= 0) break;
        w += written;
        ++found;
    }
}

// handle -> the path it was opened with, so a later ReadFile can name the file. Fixed size and
// never removed: a save session opens a handful of files, and a bounded table cannot grow.
const int kMaxHandles = 128;
struct OpenFile {
    HANDLE handle;
    wchar_t path[260];
};
OpenFile g_open[kMaxHandles];
int g_open_count = 0;
CRITICAL_SECTION g_lock;

void RememberHandle(HANDLE h, const wchar_t* path) {
    if (h == nullptr || h == INVALID_HANDLE_VALUE || path == nullptr) return;
    EnterCriticalSection(&g_lock);
    if (g_open_count < kMaxHandles) {
        OpenFile& slot = g_open[g_open_count++];
        slot.handle = h;
        wcsncpy(slot.path, path, 259);
        slot.path[259] = L'\0';
    }
    LeaveCriticalSection(&g_lock);
}

const wchar_t* PathForHandle(HANDLE h) {
    // ReadFile is a hot call in the game: do not take the lock while no save file has been seen
    // (the common case, including every ReadFile before a save is touched).
    if (g_open_count == 0) return nullptr;
    const wchar_t* found = nullptr;
    EnterCriticalSection(&g_lock);
    for (int i = g_open_count - 1; i >= 0; --i) {
        if (g_open[i].handle == h) {
            found = g_open[i].path;
            break;
        }
    }
    LeaveCriticalSection(&g_lock);
    return found;
}

HANDLE WINAPI Detour_CreateFileW(LPCWSTR name, DWORD access, DWORD share,
                                 LPSECURITY_ATTRIBUTES sa, DWORD disposition, DWORD flags,
                                 HANDLE templ) {
    HANDLE h = g_real_CreateFileW(name, access, share, sa, disposition, flags, templ);
    if (InterestingW(name)) {
        char trace[128];
        Backtrace(5, reinterpret_cast<unsigned char*>(trace), sizeof(trace));
        LogLine("CreateFileW \"%s\" access=0x%x -> %p caller=0x%08x game:%s",
                Narrow(name).c_str(), static_cast<unsigned>(access), h,
                static_cast<unsigned>(reinterpret_cast<uintptr_t>(__builtin_return_address(0))),
                trace);
        RememberHandle(h, name);
    }
    return h;
}

HANDLE WINAPI Detour_CreateFileA(LPCSTR name, DWORD access, DWORD share,
                                 LPSECURITY_ATTRIBUTES sa, DWORD disposition, DWORD flags,
                                 HANDLE templ) {
    HANDLE h = g_real_CreateFileA(name, access, share, sa, disposition, flags, templ);
    if (InterestingA(name)) {
        char trace[128];
        Backtrace(5, reinterpret_cast<unsigned char*>(trace), sizeof(trace));
        LogLine("CreateFileA \"%s\" access=0x%x -> %p caller=0x%08x game:%s", name,
                static_cast<unsigned>(access), h,
                static_cast<unsigned>(reinterpret_cast<uintptr_t>(__builtin_return_address(0))),
                trace);
        RememberHandle(h, Widen(name).c_str());
    }
    return h;
}

BOOL WINAPI Detour_ReadFile(HANDLE h, LPVOID buf, DWORD to_read, LPDWORD read, LPOVERLAPPED ov) {
    const wchar_t* path = PathForHandle(h);
    const BOOL ok = g_real_ReadFile(h, buf, to_read, read, ov);
    if (path != nullptr && InterestingW(path)) {
        char trace[128];
        Backtrace(5, reinterpret_cast<unsigned char*>(trace), sizeof(trace));
        LogLine("ReadFile   \"%s\" asked=%u got=%u -> %d caller=0x%08x game:%s",
                Narrow(path).c_str(), static_cast<unsigned>(to_read),
                read != nullptr ? *read : 0, static_cast<int>(ok),
                static_cast<unsigned>(reinterpret_cast<uintptr_t>(__builtin_return_address(0))),
                trace);
    }
    return ok;
}

// The CRT is where the game's own save code meets the file system, and - unlike the kernel32 path -
// the *immediate* caller of fread/_read is the game's code. That is the address this whole probe is
// after (which function reads a save), and it needs no stack walking to be right.
size_t __cdecl Detour_fread(void* buf, size_t size, size_t count, FILE* stream) {
    const size_t got = g_real_fread(buf, size, count, stream);
    const size_t asked = size * count;
    if (asked >= 4096) {
        LogLine("fread      size=%u count=%u got=%u caller=0x%08x", static_cast<unsigned>(size),
                static_cast<unsigned>(count), static_cast<unsigned>(got),
                static_cast<unsigned>(reinterpret_cast<uintptr_t>(__builtin_return_address(0))));
    }
    return got;
}

int __cdecl Detour_read(int fd, void* buf, unsigned count) {
    const int got = g_real_read(fd, buf, count);
    if (count >= 4096) {
        LogLine("_read      fd=%d count=%u got=%d caller=0x%08x", fd, count, got,
                static_cast<unsigned>(reinterpret_cast<uintptr_t>(__builtin_return_address(0))));
    }
    return got;
}

}  // namespace

bool InstallFileProbe(const AtmtModApi* api, std::string* why_not) {
    if (api == nullptr || api->hook_create == nullptr || api->hook_enable == nullptr) {
        if (why_not != nullptr) *why_not = "no hook service from the loader";
        return false;
    }
    g_api = api;
    g_start_ms = GetTickCount();
    InitializeCriticalSection(&g_lock);

    HMODULE kernel32 = GetModuleHandleW(L"kernel32.dll");
    if (kernel32 == nullptr) {
        if (why_not != nullptr) *why_not = "kernel32 not found";
        return false;
    }
    struct Target {
        const char* name;
        void* detour;
        void** real;
    };
    Target targets[] = {
        {"CreateFileW", reinterpret_cast<void*>(&Detour_CreateFileW),
         reinterpret_cast<void**>(&g_real_CreateFileW)},
        {"CreateFileA", reinterpret_cast<void*>(&Detour_CreateFileA),
         reinterpret_cast<void**>(&g_real_CreateFileA)},
        {"ReadFile", reinterpret_cast<void*>(&Detour_ReadFile),
         reinterpret_cast<void**>(&g_real_ReadFile)},
    };
    int installed = 0;
    for (Target& t : targets) {
        void* fn = reinterpret_cast<void*>(GetProcAddress(kernel32, t.name));
        if (fn == nullptr) continue;
        void* trampoline = nullptr;
        void* handle = api->hook_create(fn, t.detour, &trampoline);
        if (handle == nullptr) continue;
        *t.real = trampoline;
        api->hook_enable(handle);
        ++installed;
    }

    // The CRT's own file functions: their immediate caller is the game, which is where the save
    // code lives. The game imports MSVCR100 (fopen/fread), so the module is loaded already.
    HMODULE crt = GetModuleHandleW(L"MSVCR100.dll");
    if (crt == nullptr) crt = GetModuleHandleW(L"msvcr100.dll");
    if (crt == nullptr) crt = GetModuleHandleW(L"MSVCR120.dll");
    if (crt != nullptr) {
        struct CrtTarget {
            const char* name;
            void* detour;
            void** real;
        };
        CrtTarget crtTargets[] = {
            {"fread", reinterpret_cast<void*>(&Detour_fread),
             reinterpret_cast<void**>(&g_real_fread)},
            {"_read", reinterpret_cast<void*>(&Detour_read),
             reinterpret_cast<void**>(&g_real_read)},
        };
        for (CrtTarget& t : crtTargets) {
            void* fn = reinterpret_cast<void*>(GetProcAddress(crt, t.name));
            if (fn == nullptr) continue;
            void* trampoline = nullptr;
            void* handle = api->hook_create(fn, t.detour, &trampoline);
            if (handle == nullptr) continue;
            *t.real = trampoline;
            api->hook_enable(handle);
            ++installed;
        }
    }

    if (installed == 0) {
        if (why_not != nullptr) *why_not = "no file API could be hooked";
        return false;
    }
    LogLine("file API hooks installed (%d of 3), telling only about save paths", installed);
    return true;
}

}  // namespace al

