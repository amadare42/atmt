// auto_rotation.cpp - what the field camera does on its own while the character moves.
//
// The game's behaviour (0x0053c0b0, decompiled in ghidra/logs/decomp_0053c0b0.log): every frame the
// followed character moves, the camera's eye direction (character -> eye, unit vector) is pulled
// toward "straight behind the direction of travel" by 0x005397b0, a move-towards on the two unit
// vectors, and the yaw is re-derived from the result with atan2 (0x0053d1a9):
//
//     0053d0f7  jne 0x0053d194          ; skip unless the character moves along its own facing
//     ...                               ;  (and not within 20 deg of straight at the camera)
//     0053d11e  step = 0.04 * dot(facing, travel) * (1 - |dot(eye dir, travel)|), capped at 0.05
//     0053d18d  call 0x00405aa6         ; -> 0x005397b0(&out, eye dir, behind dir, step, 0)
//
// `step` is a chord length *per frame*: nothing multiplies it by the frame time, so the swing speed
// is tied to the frame rate. It is also largest exactly when the character moves sideways, which with
// camera-relative controls turns "hold right" into running in a tight circle. It starts and stops at
// full speed, and it keeps running while the right stick is held, and right after it is released.
//
// Modes (AutoRotation= in the ini):
//   game   - untouched.
//   off    - the jne above becomes a jmp to the game's own skip path (its FPU cleanup included), so
//            the eye never moves; the atan2 after it just reproduces the current yaw.
//   modern - the call above goes to ModernSwing instead: same inputs, same output (the new eye
//            direction), but the yaw turns at an angular speed in rad/s (frame-rate independent),
//            eased in and out, proportional to how sideways (and how fast) the character runs, never while
//            running toward the camera, and paused for a moment after any manual camera turn.
// The right stick ([this+0x1a0], 0x0053c851) and R3's reset (0x0053af80) do not pass through either
// site, so they behave the same in all three modes.
#include "mod_api.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace stick_rotation_speed {

// stick_rotation_speed.cpp's 0x0053c874 codespace: the camera's `this`, and a frame counter.
extern volatile uintptr_t g_camera_this;
extern volatile LONG g_camera_frames;

enum class AutoRotationMode { kGame = 0, kOff = 1, kModern = 2 };

struct ModernTuning {
    float max_speed = 1.05f;          // rad/s at a full sideways run (~60 deg/s)
    float delay_after_manual = 1.5f;  // s without auto-rotation after the camera was turned by hand
    float fade_in = 1.0f;             // s to reach full strength after that delay
    float smoothing = 0.35f;          // s, time constant of the ease in/out
    float toward_cutoff = 1.75f;      // rad (~100 deg): beyond this, the run is toward the camera
};

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

bool WriteCode(void* address, const void* bytes, size_t size) {
    DWORD old_protect = 0;
    if (!VirtualProtect(address, size, PAGE_EXECUTE_READWRITE, &old_protect)) return false;
    std::memcpy(address, bytes, size);
    DWORD ignored = 0;
    VirtualProtect(address, size, old_protect, &ignored);
    FlushInstructionCache(GetCurrentProcess(), address, size);
    return true;
}

constexpr uintptr_t kGateAddr = 0x0053d0f7;
constexpr uint8_t kGateOriginal[6] = {0x0f, 0x85, 0x97, 0x00, 0x00, 0x00};   // jne 0x53d194
constexpr uint8_t kGateSkip[6] = {0xe9, 0x98, 0x00, 0x00, 0x00, 0x90};       // jmp 0x53d194

constexpr uintptr_t kSwingCallAddr = 0x0053d18d;
constexpr uint8_t kSwingCallOriginal[5] = {0xe8, 0x14, 0x89, 0xec, 0xff};    // call 0x00405aa6

AutoRotationMode g_mode = AutoRotationMode::kGame;
ModernTuning g_tuning;

// ---------------------------------------------------------------- modern swing
float g_omega = 0.0f;         // current angular speed, rad/s (unsigned; the sign comes from delta)
double g_last_time = 0.0;
LONG g_last_frame = 0;
double g_last_manual = -1e9;  // when the camera was last turned by hand
float g_last_pos[2] = {0.0f, 0.0f};  // the look-at point (the character) at the previous call
float g_speed = 0.0f;         // its smoothed horizontal speed, units/s

// Full run speed, units/s: measured 6.5 in the field (camera_test pos log, full stick). Below it the
// follow weakens in proportion, so walking or sliding along a wall barely turns the camera.
constexpr float kRunSpeed = 6.0f;
LARGE_INTEGER g_qpf;

double Now() {
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return static_cast<double>(t.QuadPart) / static_cast<double>(g_qpf.QuadPart);
}

// Replaces 0x005397b0 at its one call site. Same stack arguments and callee-cleaned 0x2c bytes; ecx is
// the camera (the game sets it to esi before the call), edx is scratch.
//   eye / behind: unit vectors, character -> eye now, and character -> "straight behind the run"
//   out:          the eye direction to use this frame (only x and z are read back, by the atan2)
int __fastcall ModernSwing(uintptr_t camera, uintptr_t, float* out, float eye_x, float eye_y,
                           float eye_z, float, float behind_x, float, float behind_z, float, float step,
                           float) {
    // On horseback the game's travel vector is zero (step 0, "behind" on the z axis): keep the game's
    // own behaviour there, which leaves the eye where it is.
    if (step == 0.0f && behind_x == 0.0f) {
        out[0] = eye_x;
        out[1] = eye_y;
        out[2] = eye_z;
        return 0;
    }
    const double now = Now();
    const LONG frame = g_camera_frames;
    const LONG frames = frame - g_last_frame;
    double elapsed = now - g_last_time;
    g_last_frame = frame;
    g_last_time = now;
    if (elapsed < 0.0 || elapsed > 1.0 || frames <= 0) elapsed = 0.0;
    // This only runs on frames where the game would swing. If frames were skipped in between (the
    // character stood still, or ran at the camera), let the speed decay over that time first.
    float dt = static_cast<float>(elapsed / (frames > 0 ? frames : 1));
    if (dt > 0.05f) dt = 0.05f;
    if (frames > 1) {
        g_omega *= std::exp(-static_cast<float>(elapsed - dt) / g_tuning.smoothing);
    }

    // How fast the character really moves ([this+0x5a0]/[+0x5a8], the look-at point's x and z).
    float pos[2];
    std::memcpy(&pos[0], reinterpret_cast<const void*>(camera + 0x5a0), sizeof(float));
    std::memcpy(&pos[1], reinterpret_cast<const void*>(camera + 0x5a8), sizeof(float));
    if (elapsed > 0.0 && elapsed < 0.2) {   // also across a few skipped frames
        const float dx = pos[0] - g_last_pos[0];
        const float dz = pos[1] - g_last_pos[1];
        const float speed = std::sqrt(dx * dx + dz * dz) / static_cast<float>(elapsed);
        g_speed += (speed - g_speed) * (1.0f - std::exp(-dt / 0.1f));
    } else {
        g_speed = 0.0f;   // (re)started moving after a pause: ramp up from the next frame
    }
    g_last_pos[0] = pos[0];
    g_last_pos[1] = pos[1];

    // A hand-driven turn ([this+0x1a0] is the stick/mouse yaw input, decaying to 0 after release).
    float manual = 0.0f;
    std::memcpy(&manual, reinterpret_cast<const void*>(camera + 0x1a0), sizeof(manual));
    if (std::fabs(manual) > 1e-3f) {
        g_last_manual = now;
        g_omega = 0.0f;
    }

    // Signed angle from the eye direction to "behind", in the horizontal plane.
    const float cross = eye_x * behind_z - eye_z * behind_x;
    const float dot = eye_x * behind_x + eye_z * behind_z;
    const float delta = std::atan2(cross, dot);
    const float off = std::fabs(delta);

    float strength = std::sin(off > 1.5707964f ? 1.5707964f : off);   // 0 when already behind
    const float toward_fade = (g_tuning.toward_cutoff - off) / 0.35f;   // ~20 deg fade to 0
    if (toward_fade < 1.0f) strength *= toward_fade > 0.0f ? toward_fade : 0.0f;
    const float since = static_cast<float>(now - g_last_manual) - g_tuning.delay_after_manual;
    if (since < g_tuning.fade_in) {
        strength *= since > 0.0f ? since / g_tuning.fade_in : 0.0f;
    }

    strength *= g_speed < kRunSpeed ? g_speed / kRunSpeed : 1.0f;

    const float target = g_tuning.max_speed * strength;
    g_omega += (target - g_omega) * (1.0f - std::exp(-dt / g_tuning.smoothing));

    float turn = g_omega * dt;
    if (turn > off) turn = off;
    if (delta < 0.0f) turn = -turn;
    const float c = std::cos(turn);
    const float s = std::sin(turn);
    out[0] = eye_x * c - eye_z * s;
    out[1] = eye_y;
    out[2] = eye_x * s + eye_z * c;
    return 0;
}

bool SiteMatches(uintptr_t address, const uint8_t* expected, size_t size) {
    return std::memcmp(reinterpret_cast<const void*>(address), expected, size) == 0;
}

void RestoreAll() {
    if (!SiteMatches(kGateAddr, kGateOriginal, sizeof(kGateOriginal))) {
        WriteCode(reinterpret_cast<void*>(kGateAddr), kGateOriginal, sizeof(kGateOriginal));
    }
    if (!SiteMatches(kSwingCallAddr, kSwingCallOriginal, sizeof(kSwingCallOriginal))) {
        WriteCode(reinterpret_cast<void*>(kSwingCallAddr), kSwingCallOriginal,
                  sizeof(kSwingCallOriginal));
    }
}

const char* ModeName(AutoRotationMode mode) {
    switch (mode) {
        case AutoRotationMode::kOff: return "off";
        case AutoRotationMode::kModern: return "modern";
        default: return "game";
    }
}

}  // namespace

// Switches the mode (also live, from camera_test.cpp's MODE= lines).
bool SetAutoRotationMode(AutoRotationMode mode) {
    RestoreAll();
    g_mode = AutoRotationMode::kGame;
    if (mode == AutoRotationMode::kOff) {
        if (!WriteCode(reinterpret_cast<void*>(kGateAddr), kGateSkip, sizeof(kGateSkip))) return false;
    } else if (mode == AutoRotationMode::kModern) {
        g_omega = 0.0f;
        g_last_manual = -1e9;
        uint8_t call[5];
        call[0] = 0xe8;
        const uint32_t rel = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(&ModernSwing) -
                                                   (kSwingCallAddr + 5));
        std::memcpy(call + 1, &rel, sizeof(rel));
        if (!WriteCode(reinterpret_cast<void*>(kSwingCallAddr), call, sizeof(call))) return false;
    }
    g_mode = mode;
    Log("stick_rotation_speed: movement auto-rotation = %s", ModeName(mode));
    return true;
}

bool InstallAutoRotation(const AtmtModApi* api, AutoRotationMode mode, const ModernTuning& tuning) {
    g_api = api;
    g_tuning = tuning;
    QueryPerformanceFrequency(&g_qpf);
    if (!SiteMatches(kGateAddr, kGateOriginal, sizeof(kGateOriginal)) ||
        !SiteMatches(kSwingCallAddr, kSwingCallOriginal, sizeof(kSwingCallOriginal))) {
        LogError("stick_rotation_speed: the auto-rotation code is not what this mod expects (a different "
            "game build?) - AutoRotation left at the game's own behaviour");
        return false;
    }
    if (mode == AutoRotationMode::kGame) return true;
    return SetAutoRotationMode(mode);
}

// Live, from the settings bar: ModernSwing reads g_tuning every frame, and each float is written
// whole, so the worst a frame in between sees is a mix of old and new tuning.
void SetModernTuning(const ModernTuning& tuning) { g_tuning = tuning; }

void RemoveAutoRotation() {
    if (g_mode != AutoRotationMode::kGame) RestoreAll();
    g_mode = AutoRotationMode::kGame;
}

}  // namespace stick_rotation_speed
