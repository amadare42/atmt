// stick_rotation_speed.cpp - makes the right stick turn the field/dungeon camera faster.
//
// Mechanism: the camera-update function at 0x0053c0b0 loads a shared "30.0" .rdata double
// (0x00b3a198 - referenced from 52 unrelated call sites elsewhere in the exe, so it is not safe to
// repatch in place) through `fld qword ptr [0x00b3a198]` at 0x0053c874, and that value scales the
// per-frame turn the stick's deflection produces. SenPatcher's
// native/sen1/exe_patch_add_camera_sensitivity.cpp documents this exact instruction address for the EN build
// ("the resulting behavior should be identical to CS3's in-game camera sensitivity slider"), and
// its sibling exe_patch_disable_mouse_capture.cpp separately neuters a *different*, mouse-specific
// function (~0x4464xx) - so 0x0053c874 is the stick-turn path, not the mouse-look one. Verified
// against the supported ed8.exe (statically, from the exe on disk - no game process involved): the 6 bytes at 0x0053c874 are exactly
// `dd 05 98 a1 b3 00`, and the double at 0x00b3a198 is exactly 30.0.
//
// This mod never touches the shared constant itself (that would also move the other 51 call
// sites). It splices the *instruction*: a 5-byte jmp (+ 1 nop, the replaced instruction is 6 bytes)
// sends 0x0053c874 into a small codespace that runs the original `fld` unchanged and then does
// `fmul dword ptr [g_multiplier]` before jumping back to 0x0053c87a, exactly where the original
// code resumes. Nothing else that reads 0x00b3a198 is affected, and the multiplier only changes
// anything while the stick is actually producing nonzero input - the term it scales is already
// zero when the stick is centered.
#include "code_check.h"
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

namespace stick_rotation_speed {
// camera_test.cpp - opt-in (TestScript=<file>), see that file's header.
bool InstallCameraTest(const AtmtModApi* api, const char* script_name, std::string* why_not);

// The field camera's `this` (esi inside 0x0053c0b0), stored by the codespace below every frame,
// and a count of those frames.
volatile uintptr_t g_camera_this = 0;
volatile LONG g_camera_frames = 0;

// auto_rotation.cpp - the AutoRotation= modes, see that file's header.
enum class AutoRotationMode { kGame = 0, kOff = 1, kModern = 2 };
struct ModernTuning {
    float max_speed = 1.05f;
    float delay_after_manual = 1.5f;
    float fade_in = 1.0f;
    float smoothing = 0.35f;
    float toward_cutoff = 1.75f;
};
bool InstallAutoRotation(const AtmtModApi* api, AutoRotationMode mode, const ModernTuning& tuning);
bool SetAutoRotationMode(AutoRotationMode mode);
void SetModernTuning(const ModernTuning& tuning);
void RemoveAutoRotation();
}  // namespace stick_rotation_speed

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
constexpr uintptr_t kPatchAddr = 0x0053c874;
constexpr size_t kPatchLen = 6;   // dd 05 98 a1 b3 00 = fld qword ptr [0x00b3a198]
constexpr uint8_t kOriginalFld[kPatchLen] = {0xdd, 0x05, 0x98, 0xa1, 0xb3, 0x00};

// SenPatcher's CS1 camera sensitivity patch (exe_patch_add_camera_sensitivity.cpp; in its sources
// after v1.3.1, and on by default there) detours this very instruction: InjectJumpIntoCode<6> writes
// `jmp codespace; int3`, and its codespace starts with the original fld, then multiplies st0 by its
// sensitivity factor (fmulp) and jumps back to 0x0053c87a. With that in place this mod runs the fld
// and its own multiplier itself and continues in SenPatcher's codespace right after its copy of the
// fld - both multipliers apply (SenPatcher's default, 3, is a factor of 1.0).

float g_multiplier = 2.0f;   // a registered setting; the codespace reads it live every frame

uint8_t g_original_bytes[kPatchLen] = {};
void* g_codespace = nullptr;
volatile LONG g_patched = 0;

bool Patched() { return InterlockedCompareExchange(&g_patched, 0, 0) != 0; }

// Writes `size` bytes at `address`, restoring whatever page protection was there before.
bool WriteCode(void* address, const void* bytes, size_t size) {
    DWORD old_protect = 0;
    if (!VirtualProtect(address, size, PAGE_EXECUTE_READWRITE, &old_protect)) return false;
    std::memcpy(address, bytes, size);
    DWORD ignored = 0;
    VirtualProtect(address, size, old_protect, &ignored);
    return true;
}

// Writes a 5-byte relative jmp into `buffer`, encoded as if it will execute from `from_addr` -
// which is `buffer`'s own address for in-place code, but must be passed explicitly when `buffer`
// is a staging copy (e.g. a stack array) that gets memcpy'd somewhere else before it ever runs.
void EmitJmp(uint8_t* buffer, uintptr_t from_addr, uintptr_t target) {
    buffer[0] = 0xe9;
    const uint32_t rel = static_cast<uint32_t>(target - (from_addr + 5));
    std::memcpy(buffer + 1, &rel, sizeof(rel));
}

bool InstallPatch() {
    uint8_t* const patch_site = reinterpret_cast<uint8_t*>(kPatchAddr);
    // Where the codespace continues: right after the fld (vanilla), or SenPatcher's codespace past
    // its copy of the fld.
    uintptr_t resume = kPatchAddr + kPatchLen;
    if (!atmt_code::Matches(kPatchAddr, kOriginalFld, kPatchLen)) {
        const uintptr_t senpatcher = atmt_code::InjectedJumpTarget(kPatchAddr, kPatchLen);
        if (senpatcher == 0 || !atmt_code::Matches(senpatcher, kOriginalFld, kPatchLen)) {
            char found[64];
            atmt_code::Hex(kPatchAddr, kPatchLen, found, sizeof(found));
            LogError("stick_rotation_speed: 0x%p is [%s], not the camera's fld (another game build, or "
                     "another patch changed it) - leaving it untouched",
                     reinterpret_cast<void*>(kPatchAddr), found);
            return false;
        }
        resume = senpatcher + kPatchLen;
        Log("stick_rotation_speed: SenPatcher's camera sensitivity patch is at 0x%p - chaining after it "
            "(both multipliers apply)", reinterpret_cast<void*>(kPatchAddr));
    }
    std::memcpy(g_original_bytes, patch_site, kPatchLen);   // what RemovePatch puts back

    // codespace layout: [0..5] mov [&g_camera_this], esi (esi is the camera's `this` throughout
    // 0x0053c0b0), [6..11] inc dword ptr [&g_camera_frames], [12..17] the original fld,
    // [18..23] fmul dword ptr [&g_multiplier], [24..28] jmp back to the resume point.
    void* mem = VirtualAlloc(nullptr, 32, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (mem == nullptr) {
        LogError("stick_rotation_speed: VirtualAlloc failed (%lu)", GetLastError());
        return false;
    }
    uint8_t* code = reinterpret_cast<uint8_t*>(mem);
    code[0] = 0x89;   // mov dword ptr [addr32], esi
    code[1] = 0x35;
    const uint32_t this_addr = static_cast<uint32_t>(
        reinterpret_cast<uintptr_t>(&stick_rotation_speed::g_camera_this));
    std::memcpy(code + 2, &this_addr, sizeof(this_addr));
    code[6] = 0xff;   // inc dword ptr [addr32]
    code[7] = 0x05;
    const uint32_t frames_addr = static_cast<uint32_t>(
        reinterpret_cast<uintptr_t>(&stick_rotation_speed::g_camera_frames));
    std::memcpy(code + 8, &frames_addr, sizeof(frames_addr));
    std::memcpy(code + 12, kOriginalFld, kPatchLen);
    code[12 + kPatchLen + 0] = 0xd8;   // fmul dword ptr [addr32]
    code[12 + kPatchLen + 1] = 0x0d;
    const uint32_t mult_addr = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(&g_multiplier));
    std::memcpy(code + 12 + kPatchLen + 2, &mult_addr, sizeof(mult_addr));
    const uintptr_t jmp_back_addr = reinterpret_cast<uintptr_t>(code) + 12 + kPatchLen + 6;
    EmitJmp(code + 12 + kPatchLen + 6, jmp_back_addr, resume);

    if (!WriteCode(mem, code, 12 + kPatchLen + 6 + 5)) {
        LogError("stick_rotation_speed: could not make the codespace executable");
        VirtualFree(mem, 0, MEM_RELEASE);
        return false;
    }

    // `site` is a stack buffer, not the real patch address: the jmp has to be encoded as if it
    // executes from `kPatchAddr` (where it actually ends up), not from `site` itself - the bug
    // that crashed the first build (it jumped to a near-random address as soon as the field
    // camera code ran, since `site`'s own stack address has nothing to do with 0x0053c874).
    uint8_t site[kPatchLen];
    EmitJmp(site, kPatchAddr, reinterpret_cast<uintptr_t>(mem));
    site[5] = 0x90;   // nop - the jmp is 5 bytes, the instruction it replaces is 6
    if (!WriteCode(patch_site, site, kPatchLen)) {
        LogError("stick_rotation_speed: VirtualProtect failed, leaving 0x%p untouched",
            reinterpret_cast<void*>(kPatchAddr));
        VirtualFree(mem, 0, MEM_RELEASE);
        return false;
    }

    g_codespace = mem;
    InterlockedExchange(&g_patched, 1);
    Log("stick_rotation_speed: patched 0x%p, right-stick rotation speed x%.2f",
        reinterpret_cast<void*>(kPatchAddr), g_multiplier);
    return true;
}

void RemovePatch() {
    if (!Patched()) return;
    WriteCode(reinterpret_cast<void*>(kPatchAddr), g_original_bytes, kPatchLen);
    if (g_codespace != nullptr) {
        VirtualFree(g_codespace, 0, MEM_RELEASE);
        g_codespace = nullptr;
    }
    InterlockedExchange(&g_patched, 0);
}

// ---------------------------------------------------------------- settings
// Registered with the loader (AtmtModApi::settings_register): it fills these from the ini, adds the
// keys the ini lacks, and the overlay's settings bar changes them live. The mod never parses its
// ini itself.
int32_t g_enabled = 1;
int32_t g_auto_rotation = static_cast<int32_t>(stick_rotation_speed::AutoRotationMode::kModern);
float g_modern_speed_deg = 60.0f;
float g_modern_delay = 1.5f;
float g_modern_smoothing = 0.35f;
char g_test_script[128] = "";
bool g_auto_rotation_ready = false;   // the auto-rotation code matched: a mode can be switched live

const char* const kAutoRotationModes[] = {"game", "off", "modern"};   // AutoRotationMode's order

stick_rotation_speed::ModernTuning CurrentTuning() {
    stick_rotation_speed::ModernTuning tuning;
    tuning.max_speed = g_modern_speed_deg * 3.14159265f / 180.0f;
    tuning.delay_after_manual = g_modern_delay;
    tuning.smoothing = g_modern_smoothing;
    return tuning;
}

void __cdecl OnAutoRotationChanged(const AtmtSetting*, void*) {
    // Before InstallAutoRotation verified the code it patches (disabled mod, other game build),
    // switching would write into code this mod has not checked: the ini keeps the value for the
    // next start instead.
    if (!g_auto_rotation_ready) return;
    stick_rotation_speed::SetAutoRotationMode(
        static_cast<stick_rotation_speed::AutoRotationMode>(g_auto_rotation));
}

void __cdecl OnTuningChanged(const AtmtSetting*, void*) {
    stick_rotation_speed::SetModernTuning(CurrentTuning());
}

const AtmtSetting g_settings[] = {
    {ATMT_SETTING_BOOL, ATMT_SETTING_RESTART, "General", "Enabled", "Enabled",
     "the whole mod: faster stick rotation and the auto-rotation modes", &g_enabled},
    {ATMT_SETTING_FLOAT, ATMT_SETTING_LIVE, "General", "RotationSpeedMultiplier", "Stick rotation speed",
     "how much faster the right stick turns the field/dungeon camera than the game's own default\n"
     "(1.0 = unchanged, 2.0 = twice as fast - the top of SenPatcher's camera sensitivity range)",
     &g_multiplier, 0, 0.25f, 5.0f, 0.05f},
    {ATMT_SETTING_ENUM, ATMT_SETTING_LIVE, "General", "AutoRotation", "Auto-rotation",
     "what the camera does on its own while you walk/run (see auto_rotation.cpp):\n"
     "  game   - the game's own swing (fast, frame-rate dependent, fights the right stick)\n"
     "  off    - never; only the right stick turns the camera (R3 still snaps it behind)\n"
     "  modern - a gentle, eased follow that waits after you turn the camera by hand",
     &g_auto_rotation, 0, 0, 0, 0, kAutoRotationModes, 3, &OnAutoRotationChanged},
    {ATMT_SETTING_FLOAT, ATMT_SETTING_LIVE, "General", "ModernSpeed", "Follow speed (modern)",
     "modern only: top follow speed (degrees/second, at a full sideways run)", &g_modern_speed_deg, 0,
     5.0f, 360.0f, 5.0f, nullptr, 0, &OnTuningChanged},
    {ATMT_SETTING_FLOAT, ATMT_SETTING_LIVE, "General", "ModernDelayAfterManual", "Follow delay (modern)",
     "modern only: seconds without any follow after you turn the camera yourself", &g_modern_delay, 0,
     0.0f, 10.0f, 0.1f, nullptr, 0, &OnTuningChanged},
    {ATMT_SETTING_FLOAT, ATMT_SETTING_LIVE, "General", "ModernSmoothing", "Follow smoothing (modern)",
     "modern only: ease in/out time constant (seconds; larger = softer)", &g_modern_smoothing, 0,
     0.02f, 3.0f, 0.05f, nullptr, 0, &OnTuningChanged},
    {ATMT_SETTING_STRING, ATMT_SETTING_RESTART | ATMT_SETTING_ADVANCED, "General", "TestScript",
     "Test script", "test only: a stick script in the mod folder (see camera_test.cpp); empty = off",
     g_test_script, sizeof(g_test_script)},
};

}  // namespace

extern "C" {

__declspec(dllexport) uint32_t __cdecl AtmtModInit(const AtmtModApi* api) {
    if (api == nullptr || api->version < ATMT_MOD_API_VERSION) return 0;
    if (InterlockedCompareExchange(&g_started, 1, 0) != 0) {
        if (api->log != nullptr) {
            api->log("stick_rotation_speed: already initialised in this process, declining");
        }
        return 0;
    }
    g_api = api;

    api->settings_register(api, "Camera", g_settings, sizeof(g_settings) / sizeof(g_settings[0]));

    if (g_enabled == 0) {
        Log("stick_rotation_speed: disabled by its ini");
        return ATMT_MOD_API_VERSION;
    }

    if (!InstallPatch()) {
        LogError("stick_rotation_speed: patch not installed - rotation speed left at the game's default");
    }

    g_auto_rotation_ready = stick_rotation_speed::InstallAutoRotation(
        api, static_cast<stick_rotation_speed::AutoRotationMode>(g_auto_rotation), CurrentTuning());

    if (g_test_script[0] != '\0') {
        std::string why_not;
        if (!stick_rotation_speed::InstallCameraTest(api, g_test_script, &why_not)) {
            LogError("stick_rotation_speed: camera test not started - %s", why_not.c_str());
        }
    }

    return ATMT_MOD_API_VERSION;
}

__declspec(dllexport) void __cdecl AtmtModShutdown(void) {
    stick_rotation_speed::RemoveAutoRotation();
    RemovePatch();
}

// One sentence for the overlay and the manager (shared/mod_api.h, AtmtModDescription).
__declspec(dllexport) const char* __cdecl AtmtModDescription(void) {
    return "Faster right-stick camera rotation and a modern auto-rotation that swings the camera "
           "behind the character.";
}

}  // extern "C"
