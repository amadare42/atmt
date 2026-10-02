// pad_inject.cpp - programmed input, for tests only.
//
// Why this exists: the game polls device state through DirectInput8 and XInput and never reads the
// Windows message queue, so injected keystrokes (SendInput) are ignored - verified in the game (see
// docs/ENGINE_NOTES.md, "Input"). To reach a screen without a human, the mod can hook XInputGetState and merge a
// scripted button sequence into the state the game polls. The shipped feature does NOT depend on this:
// "-al" calls the game's own load function instead of pressing buttons.
//
// Script file (default <mod dir>\pad_script.txt), one entry per line, times in ms from injector start:
//
//     3000 A          press A at t=3000 (held ~150 ms, which is one visible press)
//     3500 DOWN       d-pad down
//     9000 EXIT       stop injecting
#include "al.h"
#include "mod_api.h"

#include <cctype>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace al {

namespace {

const AtmtModApi* g_api = nullptr;

struct PadEntry {
    DWORD at;
    unsigned short button;
};

std::vector<PadEntry> g_script;
volatile LONG g_active = 0;
DWORD g_start = 0;
DWORD g_last_packet = 1;

constexpr unsigned short kUp = 0x0001, kDown = 0x0002, kLeft = 0x0004, kRight = 0x0008;
constexpr unsigned short kStart = 0x0010, kBack = 0x0020;
constexpr unsigned short kA = 0x1000, kB = 0x2000, kX = 0x4000, kY = 0x8000;

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

// A failure: written to atmt_loader.log unless LogLevel=off (Log is written with LogLevel=all only).
void LogError(const char* fmt, ...) {
    if (g_api == nullptr || g_api->log_error == nullptr) return;
    char buf[400];
    va_list args;
    va_start(args, fmt);
    _vsnprintf(buf, sizeof(buf) - 1, fmt, args);
    va_end(args);
    buf[sizeof(buf) - 1] = '\0';
    g_api->log_error(buf);
}

// Button names, so a script can be written by hand and read by a human later.
unsigned short ButtonFromName(const std::string& name, bool* is_exit) {
    struct Entry {
        const char* name;
        unsigned short button;
    };
    static const Entry kNames[] = {{"UP", kUp},       {"DOWN", kDown}, {"LEFT", kLeft},
                                   {"RIGHT", kRight}, {"START", kStart}, {"BACK", kBack},
                                   {"A", kA},         {"B", kB},       {"X", kX},
                                   {"Y", kY}};
    *is_exit = false;
    std::string upper;
    for (char c : name) upper.push_back(static_cast<char>(toupper(static_cast<unsigned char>(c))));
    if (upper == "EXIT" || upper == "STOP") {
        *is_exit = true;
        return 0;
    }
    for (const Entry& e : kNames) {
        if (upper == e.name) return e.button;
    }
    return 0xFFFF;   // unknown
}

}  // namespace

// One line: "<ms> <button>" (a comma may separate them instead of a space). Pure and exposed (outside
// the anonymous namespace) so the tests can check the parsing without a game.
bool PadParseEntry(const std::string& text, unsigned* at_ms, unsigned* button, bool* is_exit) {
    if (at_ms == nullptr || button == nullptr || is_exit == nullptr) return false;
    std::string line = text;
    while (!line.empty() && (line.back() == '\r' || line.back() == '\n' || line.back() == ' ')) {
        line.pop_back();
    }
    // Leading whitespace is not a separator. With a line written as " 5000 A" the split below hit
    // index 0, so the time parsed as 0 and the button name became "5000 A" - and a file whose only
    // line was indented reported "the pad script listed no buttons" (measured 2026-09-22).
    const size_t first = line.find_first_not_of(" \t");
    if (first == std::string::npos) return false;
    line.erase(0, first);
    if (line.empty() || line[0] == ';' || line[0] == '#') return false;
    size_t split = std::string::npos;
    for (size_t i = 0; i < line.size(); ++i) {
        if (line[i] == ' ' || line[i] == '\t' || line[i] == ',') {
            split = i;
            break;
        }
    }
    if (split == std::string::npos) return false;
    *at_ms = static_cast<unsigned>(strtoul(line.substr(0, split).c_str(), nullptr, 10));
    std::string name = line.substr(split + 1);
    const size_t start = name.find_first_not_of(" \t");
    if (start == std::string::npos) return false;
    name = name.substr(start);
    bool exit_flag = false;
    const unsigned short parsed = ButtonFromName(name, &exit_flag);
    if (parsed == 0xFFFF) return false;
    *is_exit = exit_flag;
    *button = exit_flag ? 0 : parsed;
    return true;
}

namespace {

// Pushes one parsed line onto the script.
bool ParseEntry(const std::string& text) {
    unsigned at = 0;
    unsigned button = 0;
    bool is_exit = false;
    if (!PadParseEntry(text, &at, &button, &is_exit)) return false;
    PadEntry entry;
    entry.at = at;
    entry.button = static_cast<unsigned short>(button);   // 0 marks "stop injecting from here on"
    g_script.push_back(entry);
    return true;
}

}  // namespace

namespace {

typedef DWORD(WINAPI* XInputGetStateFn)(DWORD, void*);
XInputGetStateFn g_real_get_state = nullptr;

// The game imports XInput *by ordinal* (ordinal 2 = XInputGetState), so its import-table slot is known
// exactly: VA 0x0136A500 in the shipped ed8.exe. Hooking the export in xinput1_3.dll is refused
// because Steam's overlay hooked that function first, so the game's own import slot is patched
// instead - same effect, and the previous pointer is kept for passthrough.
constexpr uintptr_t kXInputGetStateIatVa = 0x0136A500;
constexpr uintptr_t kShippedImageBase = 0x00400000;

bool PatchImportSlot(void** slot, void* replacement, void** original) {
    DWORD old = 0;
    if (VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &old) == 0) return false;
    *original = *slot;
    *slot = replacement;
    DWORD ignored = 0;
    VirtualProtect(slot, sizeof(void*), old, &ignored);
    return true;
}

std::wstring g_script_path;
DWORD g_script_stamp = 0;

// The script is re-read whenever the file changes, and its clock restarts from that moment. That makes
// a sequence editable while the game runs ("write a file, watch the game"), which matters because the
// game must otherwise be restarted for every attempt.
DWORD WINAPI ReloadThread(LPVOID) {
    while (true) {
        Sleep(400);
        if (g_script_path.empty()) continue;
        WIN32_FILE_ATTRIBUTE_DATA info;
        if (GetFileAttributesExW(g_script_path.c_str(), GetFileExInfoStandard, &info) == 0) continue;
        const DWORD stamp = info.ftLastWriteTime.dwLowDateTime
                            ^ info.ftLastWriteTime.dwHighDateTime;
        if (stamp == g_script_stamp) continue;
        g_script_stamp = stamp;
        const std::vector<PadEntry> previous = g_script;
        g_script.clear();
        FILE* f = _wfopen(g_script_path.c_str(), L"rb");
        if (f != nullptr) {
            char line[256];
            while (fgets(line, sizeof(line), f)) ParseEntry(std::string(line));
            fclose(f);
        }
        if (g_script.empty()) {
            g_script = previous;   // a half-written file is ignored, the old plan stays
            Log("pad inject: the script file had no usable entry - keeping the previous script");
            continue;
        }
        g_start = GetTickCount();
        InterlockedExchange(&g_active, 1);
        Log("pad inject: script reloaded (%u entries), times start now",
            static_cast<unsigned>(g_script.size()));
    }
    return 0;
}

// XINPUT_STATE: dwPacketNumber (4 bytes) followed by the gamepad, whose first word is wButtons. The
// packet number is incremented whenever the scripted buttons change, because games compare it to
// notice that a device state changed at all.
DWORD WINAPI HookedGetState(DWORD index, void* state) {
    if (g_real_get_state == nullptr || state == nullptr) return 1167;   // ERROR_DEVICE_NOT_CONNECTED
    const DWORD rc = g_real_get_state(index, state);
    if (InterlockedCompareExchange(&g_active, 0, 0) == 0) return rc;
    const DWORD now = GetTickCount() - g_start;
    unsigned short buttons = 0;
    for (const PadEntry& e : g_script) {
        if (e.button == 0) {
            if (now >= e.at) {   // EXIT: from this moment the script is done
                InterlockedExchange(&g_active, 0);
                return rc;
            }
            continue;
        }
        if (now >= e.at && now < e.at + 150) {
            buttons = static_cast<unsigned short>(buttons | e.button);
        }
    }
    // The game decides whether a pad exists from this call's return value, and on a machine with no
    // pad the real call answers "not connected" (1167). A scripted press arriving from a pad that the
    // game believes is absent is ignored - measured in the game, which is why the earlier injector did
    // nothing. So while the script runs, a pad is always reported: the buttons are OR-ed onto the real
    // device's state when there is one, and a neutral state is fabricated when there is not.
    unsigned char* bytes = static_cast<unsigned char*>(state);
    if (rc != 0) {
        ZeroMemory(state, 16);   // XINPUT_STATE: dwPacketNumber + XINPUT_GAMEPAD
    }
    if (buttons != 0) {
        *reinterpret_cast<DWORD*>(bytes) = ++g_last_packet;   // a changed state needs a new packet
        unsigned short* btn = reinterpret_cast<unsigned short*>(bytes + 4);
        *btn = static_cast<unsigned short>(*btn | buttons);
    }
    return 0;
}

}  // namespace

bool InstallPadInjector(const AtmtModApi* api, const std::wstring& script_path,
                        std::string* why_not) {
    if (api == nullptr) {
        if (why_not != nullptr) *why_not = "no loader services";
        return false;
    }
    g_api = api;
    FILE* f = _wfopen(script_path.c_str(), L"rb");
    if (f == nullptr) {
        if (why_not != nullptr) *why_not = "no pad script found";
        return false;
    }
    char line[256];
    while (fgets(line, sizeof(line), f)) ParseEntry(std::string(line));
    fclose(f);
    if (g_script.empty()) {
        if (why_not != nullptr) *why_not = "the pad script listed no buttons";
        return false;
    }
    HMODULE xi = LoadLibraryW(L"xinput1_3.dll");
    if (xi == nullptr) {
        if (why_not != nullptr) *why_not = "xinput1_3.dll is not available";
        return false;
    }
    void* target = reinterpret_cast<void*>(GetProcAddress(xi, "XInputGetState"));
    if (target == nullptr) {
        if (why_not != nullptr) *why_not = "XInputGetState is not exported";
        return false;
    }
    void* trampoline = nullptr;
    bool hooked = false;
    // The loader's hook_create returns the target address as its handle and NULL on failure (which
    // includes "MH_CreateHook failed", i.e. Steam's overlay or another mod already hooked this exact
    // function - measured 2026-09-22: the dialog logger's overlay hooks XInputGetState's export).
    // A NULL return must therefore fall through to the import-slot patch below; testing it with `== 0`
    // (as this code did) took the *failure* for success, left the trampoline NULL and reported
    // "could not hook or patch XInputGetState" - which is why the menu replay was never verified.
    if (api->hook_create != nullptr
        && api->hook_create(target, reinterpret_cast<void*>(&HookedGetState), &trampoline) != nullptr) {
        if (api->hook_enable != nullptr) api->hook_enable(target);
        hooked = true;
        Log("pad inject: XInputGetState hooked on the export");
    } else {
        // The export is not hookable here (Steam's overlay got there first): patch the game's own
        // import slot. The address is relative to the shipped image base and ed8.exe does not relocate,
        // but the base comes from the running process so a rebuilt exe would still be handled.
        HMODULE exe = GetModuleHandleW(nullptr);
        const uintptr_t base = reinterpret_cast<uintptr_t>(exe);
        void** slot = reinterpret_cast<void**>(base + (kXInputGetStateIatVa - kShippedImageBase));
        void* original = nullptr;
        if (PatchImportSlot(slot, reinterpret_cast<void*>(&HookedGetState), &original)) {
            trampoline = original;
            hooked = true;
            Log("pad inject: XInputGetState patched in the import slot (0x%08x, was 0x%08x)",
                static_cast<unsigned>(reinterpret_cast<uintptr_t>(slot)),
                static_cast<unsigned>(reinterpret_cast<uintptr_t>(original)));
        } else {
            LogError("pad inject: the import slot at 0x%08x could not be made writable (error %lu)",
                static_cast<unsigned>(reinterpret_cast<uintptr_t>(slot)), GetLastError());
        }
    }
    if (!hooked || trampoline == nullptr) {
        if (why_not != nullptr) *why_not = "could not hook or patch XInputGetState";
        return false;
    }
    g_real_get_state = reinterpret_cast<XInputGetStateFn>(trampoline);
    g_script_path = script_path;
    g_start = GetTickCount();
    g_active = 1;
    HANDLE watcher = CreateThread(nullptr, 0, ReloadThread, nullptr, 0, nullptr);
    if (watcher != nullptr) CloseHandle(watcher);
    Log("pad inject: %u scripted press(es) will be fed through XInputGetState",
        static_cast<unsigned>(g_script.size()));
    return true;
}

}  // namespace al

