// quality_tweaks.cpp - small gameplay conveniences, each behind its own setting.
//
// Turbo run (TurboRun=true): while turbo is engaged the field character runs (as if R1, "Run", were held)
// whenever it moves - turbo mode is for covering ground, and walking at turbo speed is not.
//
// Mechanism. The field control update FUN_005B7FC0 (called by the scene's tick, FUN_005be250) picks the
// character's speed once per frame:
//
//   005b87e5  test [ctx+0x170C], 0x1000000 / FUN_00401FC3(0x106)   running forbidden here: skip the query
//   005b8800  push 1; push 0; push 5; mov ecx, edi
//   005b8808  call 0x0040DB7A                                       the button query: R1 held? (mode 0)
//   005b880d  mov bl, al                                            bl = run
//   005b8817  ... the same query for button 2 (Circle, "Walk") when bl is clear
//   005b884d  bl -> [this+0x13A0] (run speed); walk -> [+0x1394]; otherwise the stick's tilt picks
//             [+0x1394] or [+0x1398]. All of it is skipped while the stick is centred.
//
// The mod points that one call at RunQuery below, which asks the game's query exactly as before (through
// the same thunk, so battle_load's hook on FUN_00446240 still sees it) and answers "held" when it was not
// but turbo is engaged and Walk is not held. Every other R1 query (menus, "switch to right") is untouched,
// a scene that forbids running still skips the call, and a centred stick ignores the answer - so it only
// ever changes the speed of a character that is actually moving. The button query itself is not hooked:
// battle_load already owns that hook, and the shared MinHook takes one hook per target.
//
// Turbo is read, never driven: FUN_00482FE0 (on the turbo object, [[0x00C7C0CC]]) writes +4 = 1 every
// frame turbo is in effect and 0 otherwise (also when the launcher's Turbo Mode is off). SenPatcher's turbo
// patches (exe_patch_turbo_mode.cpp: a remapped button, MakeTurboToggle) change the call that feeds that
// byte, not the byte, so a toggled turbo reads the same way.
#include "code_check.h"
#include "mod_api.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace {

const AtmtModApi* g_api = nullptr;
volatile LONG g_started = 0;

void Log(const char* fmt, ...) {
    if (g_api == nullptr || g_api->log == nullptr) return;
    char buf[384];
    va_list args;
    va_start(args, fmt);
    _vsnprintf(buf, sizeof(buf) - 1, fmt, args);
    va_end(args);
    buf[sizeof(buf) - 1] = '\0';
    g_api->log(buf);
}

// A failure: written to atmt_loader.log unless LogLevel=off (Log is written with LogLevel=all only).
void LogError(const char* fmt, ...) {
    if (g_api == nullptr || g_api->log_error == nullptr) return;
    char buf[384];
    va_list args;
    va_start(args, fmt);
    _vsnprintf(buf, sizeof(buf) - 1, fmt, args);
    va_end(args);
    buf[sizeof(buf) - 1] = '\0';
    g_api->log_error(buf);
}

// ---------------------------------------------------------------- addresses (ed8.exe EN, no ASLR)
constexpr uintptr_t kRunQueryCall = 0x005B8808;   // e8 rel32 -> the button query's thunk
constexpr uintptr_t kButtonQueryThunk = 0x0040DB7A;   // jmp FUN_00446240(input, button, mode, flag), ret 0xC
constexpr uintptr_t kTurboObject = 0x00C7C0CC;   // FUN_00483110's singleton: -> { state* }
constexpr uint32_t kTurboActiveOffset = 4;       // state+4: turbo in effect this frame

constexpr int kButtonWalk = 2;   // Circle: "Cancel / Walk" in the launcher's key config

// The run query and what follows it, the turbo byte's two writers and the singleton's getter.
const atmt_code::Expect kExpected[] = {
    {0x005B8800, "\x6a\x01\x6a\x00\x6a\x05\x8b\xcf\xe8\x6d\x53\xe5\xff\x8a\xd8", 15, "the field's run query"},
    {0x00483016, "\x8b\x06\x88\x58\x04", 5, "the turbo byte's 'on' write"},
    {0x0048307C, "\x8b\x06\xd9\x45\x08\x5e\xc6\x40\x04\x00", 10, "the turbo byte's 'off' write"},
    {0x00483135, "\xa1\xcc\xc0\xc7\x00", 5, "the turbo object's getter"},
};

typedef uint32_t(__attribute__((thiscall)) * ButtonQueryFn)(void* input, int button, int mode, int flag);
const ButtonQueryFn g_button_query = reinterpret_cast<ButtonQueryFn>(kButtonQueryThunk);

uint8_t g_original_call[5] = {};
volatile LONG g_patched = 0;

// ---------------------------------------------------------------- settings
int32_t g_enabled = 1;
int32_t g_turbo_run = 1;   // read live by RunQuery

const AtmtSetting g_settings[] = {
    {ATMT_SETTING_BOOL, ATMT_SETTING_RESTART, "General", "Enabled", "Enabled",
     "the whole mod: every tweak below", &g_enabled},
    {ATMT_SETTING_BOOL, ATMT_SETTING_LIVE, "General", "TurboRun", "Run in turbo",
     "while turbo is engaged, the character runs whenever it moves, as if Run (R1) were held\n"
     "(holding Walk still walks)",
     &g_turbo_run},
};

// ---------------------------------------------------------------- turbo run
bool TurboActive() {
    const uintptr_t object = *reinterpret_cast<const volatile uintptr_t*>(kTurboObject);
    if (object == 0) return false;   // made on the first frame turbo is checked
    const uintptr_t state = *reinterpret_cast<const volatile uintptr_t*>(object);
    if (state == 0) return false;
    return *reinterpret_cast<const volatile uint8_t*>(state + kTurboActiveOffset) != 0;
}

// Stands in for the game's query at kRunQueryCall: same arguments, same convention, the answer in al.
uint32_t __attribute__((thiscall)) RunQuery(void* input, int button, int mode, int flag) {
    const uint32_t held = g_button_query(input, button, mode, flag);
    if ((held & 0xFF) != 0 || g_turbo_run == 0 || !TurboActive()) return held;
    // Walk held: the player asked for walking, turbo or not (the game only asks when Run is not held).
    if ((g_button_query(input, kButtonWalk, 0, 1) & 0xFF) != 0) return held;
    return 1;
}

bool WriteCode(uintptr_t address, const void* bytes, size_t size) {
    void* const at = reinterpret_cast<void*>(address);
    DWORD old_protect = 0;
    if (!VirtualProtect(at, size, PAGE_EXECUTE_READWRITE, &old_protect)) return false;
    std::memcpy(at, bytes, size);
    DWORD ignored = 0;
    VirtualProtect(at, size, old_protect, &ignored);
    FlushInstructionCache(GetCurrentProcess(), at, size);
    return true;
}

bool InstallTurboRun() {
    char why[256];
    if (!atmt_code::CheckAll(kExpected, sizeof(kExpected) / sizeof(kExpected[0]), why, sizeof(why))) {
        LogError("quality_tweaks: %s - turbo run not installed", why);
        return false;
    }
    std::memcpy(g_original_call, reinterpret_cast<const void*>(kRunQueryCall), sizeof(g_original_call));
    uint8_t call[5];
    call[0] = 0xE8;
    const uint32_t rel = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(&RunQuery) - (kRunQueryCall + 5));
    std::memcpy(call + 1, &rel, sizeof(rel));
    if (!WriteCode(kRunQueryCall, call, sizeof(call))) {
        LogError("quality_tweaks: VirtualProtect failed (%lu) - turbo run not installed", GetLastError());
        return false;
    }
    InterlockedExchange(&g_patched, 1);
    Log("quality_tweaks: the run query at 0x%p goes through the mod (turbo run %s)",
        reinterpret_cast<void*>(kRunQueryCall), g_turbo_run != 0 ? "on" : "off");
    return true;
}

void RemoveTurboRun() {
    if (InterlockedExchange(&g_patched, 0) == 0) return;
    WriteCode(kRunQueryCall, g_original_call, sizeof(g_original_call));
}

}  // namespace

extern "C" {

__declspec(dllexport) uint32_t __cdecl AtmtModInit(const AtmtModApi* api) {
    if (api == nullptr || api->version < ATMT_MOD_API_VERSION || api->size < sizeof(AtmtModApi)) return 0;
    if (InterlockedCompareExchange(&g_started, 1, 0) != 0) {
        if (api->log != nullptr) api->log("quality_tweaks: already initialised in this process, declining");
        return 0;
    }
    g_api = api;

    api->settings_register(api, "Quality tweaks", g_settings, sizeof(g_settings) / sizeof(g_settings[0]));

    if (g_enabled == 0) {
        Log("quality_tweaks: disabled by its ini");
        return ATMT_MOD_API_VERSION;
    }
    // Installed whatever TurboRun says, so the overlay can switch it on live; off, RunQuery only forwards.
    InstallTurboRun();
    return ATMT_MOD_API_VERSION;
}

__declspec(dllexport) void __cdecl AtmtModShutdown(void) { RemoveTurboRun(); }

// One sentence for the overlay and the manager (shared/mod_api.h, AtmtModDescription).
__declspec(dllexport) const char* __cdecl AtmtModDescription(void) {
    return "Small gameplay conveniences: the character runs whenever it moves while turbo is engaged.";
}

}  // extern "C"
