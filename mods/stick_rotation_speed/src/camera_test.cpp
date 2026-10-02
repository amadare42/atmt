// camera_test.cpp - scripted stick input + camera yaw log, so the auto-rotation patch can be judged
// without a human on the pad. Off unless TestScript=<file> is set in the mod's ini.
//
// Script file (in the mod folder), one entry per line, times in ms from the moment the field camera
// first runs (or from the moment the file is rewritten, so a test can be re-run in a live game):
//
//     3000 LX=32767            from t=3000 hold the left stick fully right
//     6000 LX=0 RX=-32767      from t=6000 release it and hold the right stick fully left
//     8000 BTN=0x0080          from t=8000 hold R3 (XINPUT_GAMEPAD_RIGHT_THUMB)
//     8200 BTN=0
//     9000 EXIT                stop injecting (the real pad passes through again)
//
// MODE=0/1/2 on a line switches AutoRotation to game/off/modern at that moment, so one game session
// can compare the modes on the same walk.
//
// Each entry *replaces* the whole injected state (sticks and buttons not named are neutral). While the
// script runs, the camera's yaw and pitch ([this+0x5d4] / [this+0x5d0], written from atan2/asin at
// 0x0053d35f/0x0053d37f) are logged every 250 ms next to the injected stick values.
#include "mod_api.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace stick_rotation_speed {

// Set by the 0x0053c874 codespace (stick_rotation_speed.cpp) every frame the field camera updates.
extern volatile uintptr_t g_camera_this;
extern volatile LONG g_camera_frames;
// auto_rotation.cpp
enum class AutoRotationMode { kGame = 0, kOff = 1, kModern = 2 };
bool SetAutoRotationMode(AutoRotationMode mode);

namespace {

const AtmtModApi* g_api = nullptr;

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

struct Step {
    DWORD at = 0;
    bool exit = false;
    short lx = 0, ly = 0, rx = 0, ry = 0;
    unsigned short buttons = 0;
    int patch = -1;   // AutoRotation mode to switch to; -1 = leave as is
};

CRITICAL_SECTION g_lock;
std::vector<Step> g_script;
std::wstring g_script_path;
DWORD g_script_stamp = 0;
volatile LONG g_active = 0;   // 1 while the script clock runs
DWORD g_start = 0;
DWORD g_packet = 1;
Step g_current;               // what the hook injects right now (updated by the clock thread)
size_t g_reached = 0;         // how many script steps have started (to fire PATCH= once)

bool ParseLine(const char* text, Step* out) {
    while (*text == ' ' || *text == '\t') ++text;
    if (*text == '\0' || *text == ';' || *text == '#' || *text == '\r' || *text == '\n') return false;
    char* end = nullptr;
    out->at = static_cast<DWORD>(strtoul(text, &end, 10));
    if (end == text) return false;
    const char* p = end;
    while (*p != '\0') {
        while (*p == ' ' || *p == '\t') ++p;
        if (*p == '\0' || *p == '\r' || *p == '\n') break;
        if (_strnicmp(p, "EXIT", 4) == 0) {
            out->exit = true;
            p += 4;
            continue;
        }
        const char* eq = strchr(p, '=');
        if (eq == nullptr) return false;
        const std::string key(p, eq - p);
        const long value = strtol(eq + 1, &end, 0);
        p = end;
        if (_stricmp(key.c_str(), "LX") == 0) out->lx = static_cast<short>(value);
        else if (_stricmp(key.c_str(), "LY") == 0) out->ly = static_cast<short>(value);
        else if (_stricmp(key.c_str(), "RX") == 0) out->rx = static_cast<short>(value);
        else if (_stricmp(key.c_str(), "RY") == 0) out->ry = static_cast<short>(value);
        else if (_stricmp(key.c_str(), "BTN") == 0) out->buttons = static_cast<unsigned short>(value);
        else if (_stricmp(key.c_str(), "MODE") == 0) out->patch = static_cast<int>(value);
        else return false;
    }
    return true;
}

bool LoadScript() {
    FILE* f = _wfopen(g_script_path.c_str(), L"rb");
    if (f == nullptr) return false;
    std::vector<Step> steps;
    char line[256];
    while (fgets(line, sizeof(line), f)) {
        Step s;
        if (ParseLine(line, &s)) steps.push_back(s);
    }
    fclose(f);
    if (steps.empty()) return false;
    EnterCriticalSection(&g_lock);
    g_script = steps;
    g_current = Step();
    LeaveCriticalSection(&g_lock);
    return true;
}

typedef DWORD(WINAPI* XInputGetStateFn)(DWORD, void*);
XInputGetStateFn g_real_get_state = nullptr;

// The game imports XInputGetState by ordinal; its import slot is at this VA in the shipped ed8.exe
// (the same slot load_last_save's pad_inject.cpp patches - whoever patches second chains the first).
constexpr uintptr_t kXInputGetStateIatVa = 0x0136A500;

DWORD WINAPI HookedGetState(DWORD index, void* state) {
    const DWORD rc = g_real_get_state != nullptr ? g_real_get_state(index, state) : 1167;
    if (index != 0 || state == nullptr || InterlockedCompareExchange(&g_active, 0, 0) == 0) return rc;
    EnterCriticalSection(&g_lock);
    const Step s = g_current;
    LeaveCriticalSection(&g_lock);
    unsigned char* bytes = static_cast<unsigned char*>(state);
    if (rc != 0) ZeroMemory(state, 16);
    // XINPUT_STATE: dwPacketNumber, wButtons, bLeftTrigger, bRightTrigger, sThumbLX/LY/RX/RY.
    *reinterpret_cast<DWORD*>(bytes) = ++g_packet;
    *reinterpret_cast<unsigned short*>(bytes + 4) = s.buttons;
    bytes[6] = 0;
    bytes[7] = 0;
    short* thumbs = reinterpret_cast<short*>(bytes + 8);
    thumbs[0] = s.lx;
    thumbs[1] = s.ly;
    thumbs[2] = s.rx;
    thumbs[3] = s.ry;
    return 0;
}

float ReadFloat(uintptr_t address) {
    float value = 0.0f;
    std::memcpy(&value, reinterpret_cast<const void*>(address), sizeof(value));
    return value;
}

DWORD WINAPI ClockThread(LPVOID) {
    DWORD last_log = 0;
    LONG last_frames = 0;
    bool announced_camera = false;
    while (true) {
        Sleep(20);
        // (Re)start: the first time the field camera runs, and whenever the file is rewritten.
        WIN32_FILE_ATTRIBUTE_DATA info;
        if (GetFileAttributesExW(g_script_path.c_str(), GetFileExInfoStandard, &info) != 0) {
            const DWORD stamp =
                info.ftLastWriteTime.dwLowDateTime ^ info.ftLastWriteTime.dwHighDateTime;
            if (stamp != g_script_stamp && announced_camera) {
                g_script_stamp = stamp;
                if (LoadScript()) {
                    g_start = GetTickCount();
                    g_reached = 0;
                    InterlockedExchange(&g_active, 1);
                    Log("camera_test: script (re)loaded, clock restarted");
                }
            }
        }
        const uintptr_t cam = g_camera_this;
        if (cam == 0) continue;
        if (!announced_camera) {
            announced_camera = true;
            Log("camera_test: field camera seen (this=0x%08x), script clock started",
                static_cast<unsigned>(cam));
            if (GetFileAttributesExW(g_script_path.c_str(), GetFileExInfoStandard, &info) != 0) {
                g_script_stamp =
                    info.ftLastWriteTime.dwLowDateTime ^ info.ftLastWriteTime.dwHighDateTime;
            }
            g_start = GetTickCount();
            InterlockedExchange(&g_active, 1);
        }
        if (InterlockedCompareExchange(&g_active, 0, 0) == 0) continue;

        const DWORD now = GetTickCount() - g_start;
        EnterCriticalSection(&g_lock);
        Step next;
        bool done = false;
        size_t reached = 0;
        for (const Step& s : g_script) {
            if (now < s.at) break;
            if (s.exit) {
                done = true;
                break;
            }
            next = s;
            ++reached;
        }
        if (reached != g_reached) {
            g_reached = reached;
            if (next.patch >= 0) {
                Log("camera_test: t=%5u MODE=%d", static_cast<unsigned>(now), next.patch);
                SetAutoRotationMode(static_cast<AutoRotationMode>(next.patch));
            }
        }
        g_current = next;
        LeaveCriticalSection(&g_lock);
        if (done) {
            InterlockedExchange(&g_active, 0);
            Log("camera_test: t=%5u script done", static_cast<unsigned>(now));
            continue;
        }
        if (now - last_log >= 250 || now < last_log) {
            const LONG frames = g_camera_frames;
            const unsigned fps = now > last_log && last_frames != 0
                ? static_cast<unsigned>((frames - last_frames) * 1000 / (now - last_log)) : 0;
            last_log = now;
            last_frames = frames;
            // [this+0x5a0/+0x5a8]: the look-at point (the followed character), x and z.
            Log("camera_test: t=%5u yaw=%8.4f pitch=%8.4f pos=(%8.2f,%8.2f) L=(%6d,%6d) R=(%6d,%6d) "
                "btn=0x%04x fps=%u",
                static_cast<unsigned>(now), ReadFloat(cam + 0x5d4), ReadFloat(cam + 0x5d0),
                ReadFloat(cam + 0x5a0), ReadFloat(cam + 0x5a8), next.lx, next.ly, next.rx, next.ry,
                next.buttons, fps);
        }
    }
    return 0;
}

}  // namespace

bool InstallCameraTest(const AtmtModApi* api, const char* script_name, std::string* why_not) {
    g_api = api;
    if (api->mod_dir == nullptr) {
        *why_not = "no mod folder";
        return false;
    }
    g_script_path = api->mod_dir;
    g_script_path += L"\\";
    for (const char* c = script_name; *c != '\0'; ++c) g_script_path.push_back(static_cast<wchar_t>(*c));
    InitializeCriticalSection(&g_lock);
    if (!LoadScript()) {
        *why_not = "the script file is missing or has no usable line";
        return false;
    }
    void** slot = reinterpret_cast<void**>(kXInputGetStateIatVa);
    DWORD old = 0;
    if (!VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &old)) {
        *why_not = "could not unprotect the XInputGetState import slot";
        return false;
    }
    g_real_get_state = reinterpret_cast<XInputGetStateFn>(*slot);
    *slot = reinterpret_cast<void*>(&HookedGetState);
    DWORD ignored = 0;
    VirtualProtect(slot, sizeof(void*), old, &ignored);
    CreateThread(nullptr, 0, ClockThread, nullptr, 0, nullptr);
    Log("camera_test: armed (%u script lines), waiting for the field camera",
        static_cast<unsigned>(g_script.size()));
    return true;
}

}  // namespace stick_rotation_speed
