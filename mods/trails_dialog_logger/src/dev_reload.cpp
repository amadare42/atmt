// dev_reload.cpp - the loop that saves reloading a save: hand a running game over to a newer build.
//
// Why it exists: the panel can only be judged inside the game, and getting back to a place where
// the game shows dialog means loading a save - once per iteration, by hand. That is a loop nobody
// keeps. So a build can be replaced inside the running game:
//
//   tools\dev_cycle.ps1 builds, copies the dll under a fresh name (a loaded dll's file is locked,
//   so the name has to change), writes the path into <game>\atmt_reload.txt - and the *running*
//   build does the rest:
//
//     1. the panel stops drawing: RequestOverlayTeardown, and the render thread does the work (it
//        is the only thread that may touch ImGui or the device),
//     2. this build stops writing the log (RetireInstance) - the new build opens the same files,
//        and two writers would interleave in atmt_dialogs.jsonl,
//     3. every hook this build owns is disabled and removed. MinHook refuses a second hook on an
//        address that is already hooked, so the old build has to let go first,
//     4. the new dll is loaded and its AtmtModInit is called with the same loader services.
//
// The old build stays in the process (a loaded module cannot be unloaded safely) but is inert: no
// hooks, no writer, no panel. See docs/OVERLAY.md for how the first build gets in at all.
#include "atmt.h"

#include "mod_api.h"

#include <cstdio>
#include <string>

namespace atmt {

namespace {

volatile LONG g_started = 0;
volatile LONG g_stop = 0;
Config g_config;                    // our own copy: the caller's is usually a local
const AtmtModApi* g_api = nullptr;
std::string g_last_path;            // never hand over to the same dll twice

constexpr DWORD kPollMs = 400;

std::string Narrow(const std::wstring& path) {
    std::string out;
    for (wchar_t c : path) out.push_back(c < 128 ? static_cast<char>(c) : '?');
    return out;
}

std::wstring Widen(const std::string& path) {
    std::wstring out;
    for (char c : path) out.push_back(static_cast<wchar_t>(static_cast<unsigned char>(c)));
    return out;
}

// <game>\atmt_reload.txt: the first non-empty, non-comment line is the dll to take over with. A
// file rather than a message because every step of this project has to be reproducible by hand,
// from a script, with no debugger attached.
bool ReadReloadRequest(const std::wstring& flag_path, std::string* out) {
    FILE* f = _wfopen(flag_path.c_str(), L"rb");
    if (f == nullptr) return false;
    char line[1024];
    bool found = false;
    while (!found && fgets(line, sizeof(line), f) != nullptr) {
        char* p = line;
        while (*p == ' ' || *p == '\t') ++p;
        if (*p == '\0' || *p == '\r' || *p == '\n' || *p == ';' || *p == '#') continue;
        if (char* end = strchr(p, '\r')) *end = '\0';
        if (char* end = strchr(p, '\n')) *end = '\0';
        *out = p;
        found = true;
    }
    fclose(f);
    return found;
}

}  // namespace

void HandOver(const std::string& narrow_path) {
    const std::wstring path = Widen(narrow_path);
    char note[640];
    _snprintf(note, sizeof(note), "dev reload: handing over to %s", narrow_path.c_str());
    HostLog(note);

    // 1. the panel leaves the overlay (which keeps running: it is the overlay mod's, not ours) and
    //    its settings are saved and dropped - the new build registers both again
    RemovePanel();

    // 2. this build stops writing
    RetireInstance();

    // 3. the message hook
    UninstallAddressHooks();

    // 4. the new build, with the same loader services
    HMODULE module = LoadLibraryW(path.c_str());
    if (module == nullptr) {
        _snprintf(note, sizeof(note),
                  "dev reload: could not load %s - this build is now inert, restart the game",
                  narrow_path.c_str());
        HostLog(note);
        return;
    }
    typedef uint32_t(__cdecl * InitFn)(const AtmtModApi*);
    InitFn init = reinterpret_cast<InitFn>(GetProcAddress(module, ATMT_MOD_ENTRY_NAME));
    if (init == nullptr) {
        _snprintf(note, sizeof(note), "dev reload: %s does not export %s - nothing took over",
                  narrow_path.c_str(), ATMT_MOD_ENTRY_NAME);
        HostLog(note);
        return;
    }
    const uint32_t version = init(g_api);
    _snprintf(note, sizeof(note), "dev reload: %s answered with api version %u%s",
              narrow_path.c_str(), version,
              version == 0 ? " (it declined - see the lines above)" : "");
    HostLog(note);
    if (version != 0) HostLog("dev reload: the new build is running");
}

DWORD WINAPI WatchThread(LPVOID) {
    const std::wstring flag_path = GetGameDir() + L"\\atmt_reload.txt";
    const std::string shown = Narrow(flag_path);
    HostLog(("dev reload: watching " + shown + " for the next build").c_str());
    while (InterlockedCompareExchange(&g_stop, 0, 0) == 0) {
        Sleep(kPollMs);
        std::string path;
        if (!ReadReloadRequest(flag_path, &path)) continue;
        _wremove(flag_path.c_str());   // handled: the next build writes its own
        if (path == g_last_path) continue;
        g_last_path = path;
        HandOver(path);
        // The loop keeps running: this module is inert now, but it can still report that a later
        // request found nothing to hand over to (which is exactly what a stale flag file deserves).
    }
    return 0;
}

void StartDevReloadWatcher(const Config& config, const AtmtModApi* api) {
    if (InterlockedCompareExchange(&g_started, 1, 0) != 0) return;
    g_config = config;   // kept (and unused) so the settings a handover was started under are known
    g_api = api;
    HANDLE thread = CreateThread(nullptr, 0, WatchThread, nullptr, 0, nullptr);
    if (thread == nullptr) {
        HostLogError("dev reload: could not start the watcher thread");
        InterlockedExchange(&g_started, 0);
        return;
    }
    CloseHandle(thread);
    InterlockedExchange(&g_stop, 0);
}

}  // namespace atmt
