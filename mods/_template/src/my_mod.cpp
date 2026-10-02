// my_mod.cpp - template for a new mod (copy this folder and rename it).
//
// A mod is a dll that exports these functions (see shared/mod_api.h):
//
//   uint32_t AtmtModInit(const AtmtModApi* api)   return ATMT_MOD_API_VERSION to accept
//   void     AtmtModShutdown(void)               optional; undo what init did
//   const char* AtmtModDescription(void)         what the mod does, one UTF-8 sentence: the overlay
//                                                shows it atop the mod's menu, the manager on its
//                                                Mods screen (via settings_schema.json)
//
// Report 0 from AtmtModInit if you cannot work with the offered api (a different
// version, a missing service): the loader then unloads the dll instead of leaving a
// half-working mod in the process.
//
// What you get from the loader (no guessing, no rediscovering paths):
//   api->game_dir      where ed8.exe runs - put user-visible files here
//   api->mod_dir       your own folder - put assets here
//   api->config_path   <mod_dir>\<your dll name>.ini, already created for you
//   api->log           one line to the shared atmt_loader.log
//   api->hook_create/hook_enable/hook_disable/hook_remove   shared MinHook, which
//                      refuses addresses that are not executable code
//
// Use atmt_ini (shared/ini.h) to read your settings, as below.
#include "ini.h"
#include "mod_api.h"

#include <windows.h>

#include <cstdio>
#include <cstring>
#include <string>

namespace {

const AtmtModApi* g_api = nullptr;
volatile LONG g_stop = 0;

// What happened: written to atmt_loader.log only with LogLevel=all (the default is errors).
void Log(const char* text) {
    if (g_api != nullptr && g_api->log != nullptr) g_api->log(text);
}

// Something did not work: written unless LogLevel=off. Keep it to failures - it is what a player sees.
void LogError(const char* text) {
    if (g_api != nullptr && g_api->log_error != nullptr) g_api->log_error(text);
}


// Example of a hook: this one does nothing but prove the plumbing. Replace the
// address with something you found (docs/ENGINE_NOTES.md, "Finding addresses") - and remember
// that an address that is wrong for the build may not run at all: check its bytes first with
// shared/code_check.h.
typedef void(__cdecl* TargetFn)(void);
TargetFn g_target = nullptr;

void __cdecl MyDetour(void) {
    // careful: this runs in the game's own thread, keep it short and never throw
    static volatile LONG calls = 0;
    InterlockedIncrement(&calls);
    if (g_target != nullptr) g_target();
}

DWORD WINAPI InitThread(LPVOID) {
    // Reading settings: everything optional, defaults in the call.
    const bool enabled = atmt_ini::ReadBool(g_api->config_path, "General", "Enabled", true);
    if (!enabled) {
        Log("my_mod: disabled by its ini");
        return 0;
    }

    char hook_address[32];
    const bool have_hook = atmt_ini::Read(g_api->config_path, "General", "HookAddress",
                                         hook_address, sizeof(hook_address), "");
    if (have_hook && std::strlen(hook_address) > 0) {
        void* target = reinterpret_cast<void*>(
            static_cast<uintptr_t>(std::strtoul(hook_address, nullptr, 0)));
        void** trampoline = reinterpret_cast<void**>(&g_target);
        if (g_api->hook_create(target, reinterpret_cast<void*>(&MyDetour), trampoline)
            != nullptr) {
            g_api->hook_enable(nullptr);   // NULL = all hooks
            Log("my_mod: hook installed");
        }
    }

    // Output goes through api->log (LogLevel=all) / api->log_error (failures). A mod writes files of
    // its own only behind a setting that is off by default - a player's game folder stays clean.
    Log("my_mod: started");

    while (InterlockedCompareExchange(&g_stop, 0, 0) == 0) Sleep(1000);
    return 0;
}

}  // namespace

extern "C" {

__declspec(dllexport) uint32_t __cdecl AtmtModInit(const AtmtModApi* api) {
    if (api == nullptr || api->version < ATMT_MOD_API_VERSION || api->size < sizeof(AtmtModApi)) return 0;
    if (api->hook_create == nullptr) return 0;   // this mod needs hooks
    g_api = api;

    HANDLE thread = CreateThread(nullptr, 0, InitThread, nullptr, 0, nullptr);
    if (thread == nullptr) return 0;
    CloseHandle(thread);
    return ATMT_MOD_API_VERSION;
}

__declspec(dllexport) void __cdecl AtmtModShutdown(void) {
    InterlockedExchange(&g_stop, 1);
    if (g_api != nullptr) g_api->hook_disable(nullptr);
}

// One sentence for the overlay and the manager (shared/mod_api.h, AtmtModDescription).
__declspec(dllexport) const char* __cdecl AtmtModDescription(void) {
    return "What this mod does, in one sentence (the overlay and the manager show it).";
}

}  // extern "C"
