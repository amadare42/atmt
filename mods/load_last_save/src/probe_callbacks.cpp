// probe_callbacks.cpp - what the load menu installs in the save manager.
//
// The save manager's state machine (FUN_004855B0, one call per frame) finishes a load by calling a
// callback the *caller* installed:
//
//     case 3: if (ready flag cleared) { pcVar = *(this + 0x14); state = success ? 5 : 1; }
//     case 6: pcVar = *(this + 0x10); (*pcVar)(flag, *(this + 0x18));
//
// At the title screen those three fields are zero (measured: cb1=cb2=arg=0), which is exactly why a
// load driven from the title screen only ever reads the file: nobody is there to be told "it is
// read", so nothing enters the session. The load menu fills them in.
//
// The manager's pointer is a global (0x00C3E750), so this probe needs no hook and patches nothing: it
// polls the three fields and reports every change. Run one menu-driven load with ProbeCallbacks=true
// and the log names the callbacks and the argument they are given - the values the "-al" path has to
// supply itself.
#include "al.h"
#include "mod_api.h"

#include <cstdarg>
#include <cstdio>

namespace al {

namespace {

const AtmtModApi* g_api = nullptr;

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

// Reads one dword of the game's memory, or 0 when the address is not readable. Everything here is a
// read of a live process, so nothing may be assumed to be mapped.
uint32_t ReadU32(uintptr_t address) {
    MEMORY_BASIC_INFORMATION mbi;
    if (VirtualQuery(reinterpret_cast<const void*>(address), &mbi, sizeof(mbi)) == 0) return 0;
    if (mbi.State != MEM_COMMIT) return 0;
    if ((mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) != 0) return 0;
    return *reinterpret_cast<const uint32_t*>(address);
}

// True when n bytes at address can be read (the manager is heap memory in a live process).
bool ReadableRange(uintptr_t address, size_t n) {
    MEMORY_BASIC_INFORMATION mbi;
    if (VirtualQuery(reinterpret_cast<const void*>(address), &mbi, sizeof(mbi)) == 0) return false;
    if (mbi.State != MEM_COMMIT) return false;
    if ((mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) != 0) return false;
    const uintptr_t region_end =
        reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
    return address + n <= region_end;
}

DWORD WINAPI PollThread(LPVOID) {
    uint32_t last[6] = {0, 0, 0, 0, 0, 0};
    bool first = true;
    uint32_t last_manager = 0;
    while (true) {
        Sleep(100);
        const uint32_t manager = ReadU32(kGameSaveManagerPtr);
        if (manager < 0x10000 || !ReadableRange(manager, 0x60)) {
            continue;
        }
        // The three fields the state machine calls, plus the state it switches on and the sub-state
        // (so the sequence reads in order in the log).
        const uint32_t fields[6] = {
            manager,
            ReadU32(manager + 0x10),   // completion callback  ("the save is read")
            ReadU32(manager + 0x14),   // progress callback
            ReadU32(manager + 0x18),   // the argument both are given
            ReadU32(manager + 0x50),   // the state the machine switches on
            ReadU32(manager + 0x54),   // its sub-state source
        };
        bool changed = false;
        for (int i = 0; i < 6; ++i) {
            if (fields[i] != last[i]) changed = true;
        }
        if (changed || manager != last_manager) {
            Log("cb manager=0x%08x cb1=0x%08x cb2=0x%08x arg=0x%08x state=0x%08x sub=0x%08x%s",
                fields[0], fields[1], fields[2], fields[3], fields[4], fields[5],
                first ? " (first read)" : "");
            for (int i = 0; i < 6; ++i) last[i] = fields[i];
            last_manager = manager;
            first = false;
        }
    }
    return 0;
}

}  // namespace

bool InstallCallbackProbe(const AtmtModApi* api, std::string* why_not) {
    if (api == nullptr || api->log == nullptr) {
        if (why_not != nullptr) *why_not = "no loader services";
        return false;
    }
    g_api = api;
    HANDLE t = CreateThread(nullptr, 0, PollThread, nullptr, 0, nullptr);
    if (t == nullptr) {
        if (why_not != nullptr) *why_not = "could not start the poll thread";
        return false;
    }
    CloseHandle(t);
    Log("callback probe: polling the save manager at [0x%08x] (cb1 at +0x10, cb2 at +0x14, arg at "
        "+0x18)",
        static_cast<unsigned>(kGameSaveManagerPtr));
    return true;
}

}  // namespace al
