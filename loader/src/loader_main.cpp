// loader_main.cpp - the loader dll.
//
// Responsibilities, and nothing else:
//   1. get into the game process: either injected by path (atmt_inject)
//      or impersonating a dll the game imports from its own folder (the GFSDK
//      flavour of this target), forwarding that library's exports.
//   2. provide services to mods: paths, logging, hooks, settings, and a place for one mod to
//      offer an interface to the others (see shared/mod_api.h).
//   3. discover and load mod dlls from the mod folder, on its own thread, logging
//      what happened.
//   4. hand over the actual work to the mods.
//
// It knows nothing about Trails of Cold Steel: the dialog logger is a mod
// (mods/trails_dialog_logger), and further mods can be added without touching
// this file.
#include "ini.h"
#include "loader.h"
#include "loader_config.h"
#include "mod_api.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <exception>
#include <utility>
#include <vector>

namespace atmt_loader {

namespace {

constexpr size_t kMaxMods = 16;

struct LoadedMod {
    HMODULE module = nullptr;
    AtmtModShutdownFn shutdown = nullptr;
    AtmtModApi api;                 // kept alive for the mod's whole lifetime
    std::wstring name;
    std::wstring dir;
    std::wstring config_path;
    bool initialized = false;
};

LoadedMod g_mods[kMaxMods];
size_t g_mod_count = 0;
std::wstring g_log_path;           // the string the api's log_path points at
CRITICAL_SECTION g_mods_lock;
INIT_ONCE g_mods_lock_once = INIT_ONCE_STATIC_INIT;

BOOL CALLBACK InitModsLock(PINIT_ONCE, PVOID, PVOID*) {
    InitializeCriticalSection(&g_mods_lock);
    return TRUE;
}

void EnsureModsLock() { InitOnceExecuteOnce(&g_mods_lock_once, &InitModsLock, nullptr, nullptr); }

std::wstring Join(const std::wstring& dir, const wchar_t* leaf) {
    if (dir.empty()) return leaf;
    std::wstring out = dir;
    if (out.back() != L'\\' && out.back() != L'/') out += L'\\';
    out += leaf;
    return out;
}

std::wstring BaseName(const std::wstring& path) {
    const size_t slash = path.find_last_of(L"\\/");
    return slash == std::wstring::npos ? path : path.substr(slash + 1);
}

std::wstring Stem(const std::wstring& path) {
    std::wstring name = BaseName(path);
    const size_t dot = name.find_last_of(L'.');
    return dot == std::wstring::npos ? name : name.substr(0, dot);
}

std::string Narrow(const std::wstring& text) {
    std::string out;
    out.reserve(text.size());
    for (wchar_t c : text) out.push_back(c < 128 ? static_cast<char>(c) : '?');
    return out;
}

// Each mod gets an ini created on first run, so its settings are discoverable
// without reading any documentation.
void WriteModIniDefaults(const std::wstring& path, const std::wstring& mod_name) {
    char content[512];
    _snprintf(content, sizeof(content),
              "; settings for the %s mod\n"
              "; delete this file to get the defaults back\n"
              "[General]\n"
              "Enabled=true\n",
              Narrow(mod_name).c_str());
    atmt_ini::WriteDefaultIfMissing(path.c_str(), content);
}

// ---------------------------------------------------------------- settings, as mods see them
// The api pointer a mod passes as `self` is how the registry tells mods apart: every mod
// has its own AtmtModApi in its slot (and a dev reload hands the fresh build that same
// pointer, so re-registering replaces the old table instead of adding a second one).
// Slots are searched regardless of g_mod_count because a mod registers from inside
// AtmtModInit, before it is counted.
LoadedMod* SlotOf(const AtmtModApi* self) {
    for (LoadedMod& slot : g_mods) {
        if (&slot.api == self) return &slot;
    }
    return nullptr;
}

int __cdecl ApiSettingsRegister(const AtmtModApi* self, const char* title, const AtmtSetting* table,
                                uint32_t count) {
    const LoadedMod* slot = SlotOf(self);
    if (slot == nullptr) return -1;
    try {
        // The mod's AtmtModDescription export (optional): a static string of its dll.
        const auto describe = reinterpret_cast<AtmtModDescriptionFn>(
            GetProcAddress(static_cast<HMODULE>(slot->module), ATMT_MOD_DESCRIPTION_NAME));
        return SettingsRegister(self, Narrow(slot->name), slot->config_path, title,
                                describe != nullptr ? describe() : nullptr, table, count);
    } catch (...) {
        LogErrorW(L"settings: %s: registering failed (out of memory?)", slot->name.c_str());
        return -1;
    }
}

void __cdecl ApiSettingsUnregister(const AtmtModApi* self) {
    try {
        SettingsUnregister(self);
    } catch (...) {
    }
}

const AtmtSettingsGroup* __cdecl ApiSettingsAcquire(uint32_t* count, uint32_t* generation) {
    return SettingsAcquire(count, generation);
}

void __cdecl ApiSettingsRelease() { SettingsRelease(); }

int __cdecl ApiSettingsSet(const AtmtSetting* setting, const void* new_value) {
    try {
        return SettingsSet(setting, new_value);
    } catch (...) {
        return -1;
    }
}

int __cdecl ApiSettingsCommit() {
    try {
        return SettingsCommit();
    } catch (...) {
        return -1;
    }
}

// ---------------------------------------------------------------- services
// One mod's interface for the others (AtmtModApi::service_*). A plain list: there are a handful of
// services at most, and a lookup is a mod asking once. An entry belongs to the mod that published it
// and goes when that mod does (DropServices, next to the settings' unregister).
struct Service {
    const void* owner;
    std::string name;
    const void* iface;
};
std::vector<Service> g_services;
CRITICAL_SECTION g_services_lock;
INIT_ONCE g_services_lock_once = INIT_ONCE_STATIC_INIT;

BOOL CALLBACK InitServicesLock(PINIT_ONCE, PVOID, PVOID*) {
    InitializeCriticalSection(&g_services_lock);
    return TRUE;
}

void EnsureServicesLock() { InitOnceExecuteOnce(&g_services_lock_once, &InitServicesLock, nullptr, nullptr); }

int __cdecl ApiServicePublish(const AtmtModApi* self, const char* name, const void* iface) {
    const LoadedMod* slot = SlotOf(self);
    if (slot == nullptr || name == nullptr || name[0] == '\0') return -1;
    EnsureServicesLock();
    EnterCriticalSection(&g_services_lock);
    bool replaced = false;
    for (Service& s : g_services) {
        if (s.name == name) {
            s.owner = self;
            s.iface = iface;
            replaced = true;
        }
    }
    if (!replaced) g_services.push_back(Service{self, name, iface});
    LeaveCriticalSection(&g_services_lock);
    LogW(L"service: %s published \"%S\"", slot->name.c_str(), name);
    return 0;
}

const void* __cdecl ApiServiceFind(const char* name) {
    if (name == nullptr) return nullptr;
    const void* found = nullptr;
    EnsureServicesLock();
    EnterCriticalSection(&g_services_lock);
    for (const Service& s : g_services) {
        if (s.name == name) found = s.iface;
    }
    LeaveCriticalSection(&g_services_lock);
    return found;
}

void DropServices(const AtmtModApi* owner) {
    EnsureServicesLock();
    EnterCriticalSection(&g_services_lock);
    for (size_t i = g_services.size(); i-- > 0;) {
        if (g_services[i].owner == owner) g_services.erase(g_services.begin() + static_cast<std::ptrdiff_t>(i));
    }
    LeaveCriticalSection(&g_services_lock);
}

// Everything the loader keeps on a mod's behalf, dropped when the mod goes: its settings (saved
// first, while its dll is still mapped) and its services.
void ForgetMod(const AtmtModApi* api) {
    ApiSettingsUnregister(api);
    DropServices(api);
}

// Loads one mod dll, fills in its api struct and calls its entry point. Returns
// true when the mod reported that it started.
bool LoadOneMod(const std::wstring& path) {
    HMODULE module = LoadLibraryW(path.c_str());
    if (module == nullptr) {
        LogErrorW(L"mod: LoadLibrary failed for %s (error %lu)", path.c_str(), GetLastError());
        return false;
    }
    auto init = reinterpret_cast<AtmtModInitFn>(GetProcAddress(module, ATMT_MOD_ENTRY_NAME));
    if (init == nullptr) {
        LogErrorW(L"mod: %s is not a mod (no %s export), skipping", BaseName(path).c_str(),
             L"" ATMT_MOD_ENTRY_NAME);
        FreeLibrary(module);
        return false;
    }

    EnsureModsLock();
    EnterCriticalSection(&g_mods_lock);
    const bool too_many = (g_mod_count >= kMaxMods);
    LoadedMod* slot = too_many ? nullptr : &g_mods[g_mod_count];
    LeaveCriticalSection(&g_mods_lock);
    if (slot == nullptr) {
        LogError("mod: too many mods (max %u)", static_cast<unsigned>(kMaxMods));
        FreeLibrary(module);
        return false;
    }

    slot->module = module;
    slot->name = Stem(path);
    slot->dir = path.substr(0, path.find_last_of(L"\\/"));
    if (slot->dir.empty()) slot->dir = SelfDir();
    slot->config_path = Join(slot->dir, (slot->name + L".ini").c_str());
    WriteModIniDefaults(slot->config_path, slot->name);

    // The api must stay valid for the mod's whole lifetime, so it lives in the slot
    // and every pointer in it points at memory that lives just as long.
    AtmtModApi& api = slot->api;
    ZeroMemory(&api, sizeof(api));
    api.version = ATMT_MOD_API_VERSION;
    api.size = sizeof(AtmtModApi);
    api.loader_module = g_self_module;
    api.game_module = GetModuleHandleW(nullptr);
    api.game_dir = GameDir().c_str();
    api.mod_dir = slot->dir.c_str();
    api.mod_name = slot->name.c_str();
    api.config_path = slot->config_path.c_str();
    api.log_path = g_log_path.c_str();
    api.log = &ModLog;
    api.log_error = &ModLogError;
    api.hook_create = &HookCreate;
    api.hook_enable = &HookEnable;
    api.hook_disable = &HookDisable;
    api.hook_remove = &HookRemove;
    api.settings_register = &ApiSettingsRegister;
    api.settings_unregister = &ApiSettingsUnregister;
    api.settings_acquire = &ApiSettingsAcquire;
    api.settings_release = &ApiSettingsRelease;
    api.settings_set = &ApiSettingsSet;
    api.settings_commit = &ApiSettingsCommit;
    api.service_publish = &ApiServicePublish;
    api.service_find = &ApiServiceFind;

    LogW(L"mod: %s -> init (module 0x%08x)", slot->name.c_str(),
         static_cast<unsigned>(reinterpret_cast<uintptr_t>(module)));

    uint32_t accepted = 0;
    try {
        accepted = init(&api);
    } catch (...) {
        LogErrorW(L"mod: %s threw during init", slot->name.c_str());
        accepted = 0;
    }

    if (accepted == 0) {
        LogW(L"mod: %s refused to start", slot->name.c_str());
        ForgetMod(&api);
        FreeLibrary(module);
        return false;
    }
    if (accepted > ATMT_MOD_API_VERSION) {
        // never pretend to implement more than we do
        LogErrorW(L"mod: %s needs api %u, this loader implements %u - unloading",
             slot->name.c_str(), accepted, ATMT_MOD_API_VERSION);
        if (auto shutdown = reinterpret_cast<AtmtModShutdownFn>(
                GetProcAddress(module, ATMT_MOD_SHUTDOWN_NAME))) {
            shutdown();
        }
        ForgetMod(&api);
        FreeLibrary(module);
        return false;
    }

    slot->shutdown = reinterpret_cast<AtmtModShutdownFn>(
        GetProcAddress(module, ATMT_MOD_SHUTDOWN_NAME));
    slot->initialized = true;

    EnterCriticalSection(&g_mods_lock);
    ++g_mod_count;
    LeaveCriticalSection(&g_mods_lock);

    LogW(L"mod: %s started (api %u)", slot->name.c_str(), accepted);
    return true;
}

void UnloadMods(bool process_is_exiting) {
    EnsureModsLock();
    EnterCriticalSection(&g_mods_lock);
    const size_t count = g_mod_count;
    g_mod_count = 0;
    LeaveCriticalSection(&g_mods_lock);

    for (size_t i = 0; i < count; ++i) {
        LoadedMod& slot = g_mods[i];
        if (!slot.initialized) continue;
        slot.initialized = false;
        if (process_is_exiting) {
            // Calling into a mod while the process is tearing down can deadlock on
            // the loader lock; the OS reclaims everything anyway.
            LogW(L"mod: %s shutdown skipped (process is exiting)", slot.name.c_str());
            continue;
        }
        if (slot.shutdown == nullptr) {
            ForgetMod(&slot.api);
            continue;
        }
        LogW(L"mod: shutting down %s", slot.name.c_str());
        try {
            slot.shutdown();
        } catch (...) {
            LogErrorW(L"mod: %s threw during shutdown", slot.name.c_str());
        }
        // After the mod's own shutdown (which may have unregistered already) and while its
        // dll is still mapped: any unsaved change is written, then the table is dropped.
        ForgetMod(&slot.api);
    }
}

// Collects the mod dlls to load: the explicit list from the ini, or every *.dll in
// the mod folder in a deterministic (alphabetical) order.
std::vector<std::wstring> DiscoverMods(const LoaderConfig& config) {
    std::vector<std::wstring> found;
    if (!config.mods.empty()) {
        for (const std::wstring& name : config.mods) {
            std::wstring path = name;
            if (path.find(L'\\') == std::wstring::npos && path.find(L'/') == std::wstring::npos) {
                path = Join(config.mod_dir, name.c_str());
            }
            if (GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES) {
                LogErrorW(L"mod: listed in the ini but not found: %s", path.c_str());
                continue;
            }
            found.push_back(path);
        }
        return found;
    }

    WIN32_FIND_DATAW data;
    const std::wstring pattern = Join(config.mod_dir, L"*.dll");
    HANDLE search = FindFirstFileW(pattern.c_str(), &data);
    if (search == INVALID_HANDLE_VALUE) {
        LogErrorW(L"loader: no mod folder at %s (create it, or set ModDir)",
             config.mod_dir.c_str());
        return found;
    }
    do {
        if ((data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) continue;
        found.push_back(Join(config.mod_dir, data.cFileName));
    } while (FindNextFileW(search, &data) != 0);
    FindClose(search);

    for (size_t i = 1; i < found.size(); ++i) {
        for (size_t j = i; j > 0 && _wcsicmp(found[j - 1].c_str(), found[j].c_str()) > 0; --j) {
            std::swap(found[j - 1], found[j]);
        }
    }
    return found;
}

// Signalled once the mods have been initialised (or there is nothing to load, or loading failed):
// the entry gate below holds the game's own startup until then.
HANDLE g_mods_ready = nullptr;

struct SignalModsReady {
    ~SignalModsReady() {
        if (g_mods_ready != nullptr) SetEvent(g_mods_ready);
    }
};

DWORD WINAPI LoaderThread(LPVOID) {
    SignalModsReady ready;
    try {
        g_log_path = GameDir() + L"\\atmt_loader.log";
        EnsureServicesLock();
        // Config has to be read before anything is logged: LogLevel decides what reaches the file
        // (and whether there is one), and DiscoverMods/LoadOneMod below need it regardless.
        const LoaderConfig config = LoadLoaderConfig(SelfDir());
        LogInit(config.log_level);
        LogW(L"loader: attached to %s", GameModulePath().c_str());
        LogW(L"loader: this dll is %s", SelfPath().c_str());

        if (!config.enabled) {
            Log("loader: disabled by atmt_loader.ini - no mods will be loaded");
            return 0;
        }
        LogW(L"loader: mod folder %s", config.mod_dir.c_str());
        if (config.load_delay_ms > 0) {
            // A delay asks for mods after startup, so the game is not held for it.
            if (g_mods_ready != nullptr) SetEvent(g_mods_ready);
            Log("loader: waiting %d ms before loading mods", config.load_delay_ms);
            Sleep(static_cast<DWORD>(config.load_delay_ms));
        }

        const std::vector<std::wstring> mods = DiscoverMods(config);
        if (mods.empty()) {
            Log("loader: nothing to load");
            return 0;
        }
        size_t started = 0;
        for (const std::wstring& path : mods) {
            if (LoadOneMod(path)) ++started;
        }
        Log("loader: ready (%u of %u mods started)", static_cast<unsigned>(started),
            static_cast<unsigned>(mods.size()));
        if (g_mods_ready != nullptr) SetEvent(g_mods_ready);

        // Keep the thread alive: a mod may want to know that the loader is here, and
        // a returning thread would be one more thing to reason about.
        for (;;) Sleep(60000);
    } catch (const std::exception& e) {
        LogError("loader: failed: %s", e.what());
        const std::wstring dir = SelfDir();
        if (FILE* f = _wfopen((dir + L"\\atmt_loader_error.txt").c_str(), L"wb")) {
            std::fprintf(f, "loader startup failed: %s\n", e.what());
            std::fclose(f);
        }
    } catch (...) {
        LogError("loader: failed with an unknown exception");
    }
    return 0;
}

}  // namespace

// Called by mods through api->log / api->log_error, so every message ends up in one file.
void ModLog(const char* text) {
    Log("[mod] %s", text != nullptr ? text : "");
}

void ModLogError(const char* text) {
    LogError("[mod] %s", text != nullptr ? text : "");
}

}  // namespace atmt_loader

namespace {

// ------------------------------------------------------------ proxy: GFSDK_SSAO
// The game imports GFSDK_SSAO_D3D11.win32.dll from its own folder, and it is the
// only module that does (verified against the live process: ed8.exe alone, unlike
// winmm.dll, which seven modules - the NVIDIA driver among them - import).
// Impersonating it is what lets the loader start by itself: no injection step, no
// game file modified, and the original library is merely renamed next to us.
// Targets of the forwarding thunks in proxy_gfsdk.s. C linkage on purpose: the
// assembler references them by their unmangled names (_g_real_CreateContext).
extern "C" {
void* g_real_CreateContext = nullptr;
void* g_real_GetVersion = nullptr;
}

#ifdef ATMT_LOADER_PROXY_GFSDK

volatile LONG g_gfsdk_state = 0;   // 0 = untouched, 1 = loading, 2 = done

// The game cannot run without the real library. Nothing is written to disk on the way (the "No
// files written" preset means it): when it is not found, the player is told in a message box which
// paths were tried, and the game exits instead of jumping to nothing.
[[noreturn]] void RealGfsdkMissing(const std::wstring& tried) {
    const std::wstring text =
        L"The game's own GFSDK_SSAO_D3D11.win32.dll was not found next to the ATMT loader, so the "
        L"game cannot start.\n\nLooked for:\n" + tried +
        L"\nReinstall the mods with ATMT Manager, or verify the game's files (Steam: Verify integrity "
        L"of game files) to remove the loader.";
    MessageBoxW(nullptr, text.c_str(), L"ATMT loader", MB_OK | MB_ICONERROR | MB_SETFOREGROUND);
    ExitProcess(1);
}

// The manager renames the original library, keeping it next to us.
void LoadRealGfsdk() {
    const std::wstring candidates[] = {
        atmt_loader::SelfDir() + L"\\GFSDK_SSAO_D3D11.win32.orig.dll",
        atmt_loader::GameDir() + L"\\GFSDK_SSAO_D3D11.win32.orig.dll",
        atmt_loader::GameDir() + L"\\proxy_orig\\GFSDK_SSAO_D3D11.win32.dll",
    };
    HMODULE real = nullptr;
    std::wstring tried;
    for (const std::wstring& path : candidates) {
        real = LoadLibraryW(path.c_str());
        if (real != nullptr) break;
        tried += L"  " + path + L"\n";
    }
    if (real == nullptr) RealGfsdkMissing(tried);
    g_real_CreateContext = reinterpret_cast<void*>(
        GetProcAddress(real, "GFSDK_SSAO_CreateContext_D3D11"));
    g_real_GetVersion =
        reinterpret_cast<void*>(GetProcAddress(real, "GFSDK_SSAO_GetVersion"));
    if (g_real_CreateContext == nullptr || g_real_GetVersion == nullptr) {
        RealGfsdkMissing(L"  (a library was found, but it does not export GFSDK_SSAO_CreateContext_D3D11 "
                         L"and GFSDK_SSAO_GetVersion)\n");
    }
}

#endif  // ATMT_LOADER_PROXY_GFSDK
// ------------------------------------------------------------ entry gate
// When the game imports this dll, it is loaded before the game's own code runs, but the mods load
// on a thread that can only start once DllMain returns - and then races the game's startup, which
// reads its settings, creates the window and the device within a few milliseconds. A mod that has
// to change any of that (deckscreen's forced resolution) would lose that race. So the exe's
// entry point is patched with a jump to EntryGate, which waits for the loader thread to finish
// initialising the mods (at most kEntryGateTimeoutMs), puts the entry point's bytes back and runs
// it. Only done for a load at process start (DllMain's `reserved` is non-null): when the loader is
// injected into a running game its entry point has long run.
constexpr DWORD kEntryGateTimeoutMs = 20000;
uint8_t* g_entry = nullptr;
uint8_t g_entry_bytes[5];

void WriteCode(uint8_t* at, const uint8_t* bytes, size_t size) {
    DWORD old_protect = 0;
    VirtualProtect(at, size, PAGE_EXECUTE_READWRITE, &old_protect);
    std::memcpy(at, bytes, size);
    VirtualProtect(at, size, old_protect, &old_protect);
    FlushInstructionCache(GetCurrentProcess(), at, size);
}

DWORD WINAPI EntryGate(void* peb) {
    const DWORD start = GetTickCount();
    const DWORD waited = WaitForSingleObject(atmt_loader::g_mods_ready, kEntryGateTimeoutMs);
    atmt_loader::Log("loader: game start held %lu ms for the mods%s", GetTickCount() - start,
                    waited == WAIT_OBJECT_0 ? "" : " - timed out, starting anyway");
    WriteCode(g_entry, g_entry_bytes, sizeof(g_entry_bytes));
    return reinterpret_cast<DWORD(WINAPI*)(void*)>(g_entry)(peb);
}

void InstallEntryGate() {
    auto* base = reinterpret_cast<uint8_t*>(GetModuleHandleW(nullptr));
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    if (nt->OptionalHeader.AddressOfEntryPoint == 0) return;
    g_entry = base + nt->OptionalHeader.AddressOfEntryPoint;
    std::memcpy(g_entry_bytes, g_entry, sizeof(g_entry_bytes));
    uint8_t jump[5] = {0xE9};
    const int32_t rel = static_cast<int32_t>(reinterpret_cast<uintptr_t>(&EntryGate) -
                                             reinterpret_cast<uintptr_t>(g_entry + 5));
    std::memcpy(jump + 1, &rel, sizeof(rel));
    WriteCode(g_entry, jump, sizeof(jump));
}

}  // namespace

extern "C" {

#ifdef ATMT_LOADER_PROXY_GFSDK
// Called from the thunks (proxy_gfsdk.s) before they jump to the real library.
// Deliberately not done in DllMain: a graphics middleware must not be loaded while
// the loader lock is held (verified), and by the time the game calls it a D3D11
// device exists anyway.
void __cdecl atmt_gfsdk_resolve(void) {
    if (g_real_CreateContext != nullptr && g_real_GetVersion != nullptr) return;
    if (InterlockedCompareExchange(&g_gfsdk_state, 1, 0) == 0) {
        LoadRealGfsdk();
        InterlockedExchange(&g_gfsdk_state, 2);
        return;
    }
    while (InterlockedCompareExchange(&g_gfsdk_state, 2, 2) != 2) Sleep(0);
}
#endif

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID reserved) {
    if (reason == DLL_PROCESS_ATTACH) {
        atmt_loader::g_self_module = inst;
        DisableThreadLibraryCalls(inst);
        // Mods are loaded on a thread: LoadLibrary while holding the loader lock
        // (which is the case inside DllMain) is not allowed.
        atmt_loader::g_mods_ready = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        HANDLE thread = CreateThread(nullptr, 0, atmt_loader::LoaderThread, nullptr, 0,
                                     nullptr);
        if (thread != nullptr) {
            CloseHandle(thread);
            if (reserved != nullptr && atmt_loader::g_mods_ready != nullptr) InstallEntryGate();
        }
    } else if (reason == DLL_PROCESS_DETACH) {
        // reserved != NULL means the process is exiting, and calling into a mod then
        // can deadlock on the loader lock; only do it on an explicit FreeLibrary,
        // where a normal stack is available.
        atmt_loader::UnloadMods(reserved != nullptr);
        atmt_loader::HookShutdown();
        atmt_loader::LogClose();
    }
    return TRUE;
}

}  // extern "C"
