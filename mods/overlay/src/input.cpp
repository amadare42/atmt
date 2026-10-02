// input.cpp - what the overlay reads, and what it takes away from the game while something is open.
//
// Three paths, all installed once per process and all passing everything through untouched while
// nothing is open:
//   * the pad: the game polls XInputGetState itself (XINPUT1_3 is in ed8.exe's imports). The hook
//     reads the real pad through the trampoline for the overlay, and hands the game an idle state
//     while captured. Only the game's own import slot is patched (0x0136A500 in the shipped exe,
//     docs/ENGINE_NOTES.md, "Input"), never the export: Steam's overlay hooks the export for Steam Input, and
//     must find it untouched (InstallPad). Patched rather than overwritten, so whatever was there
//     (the -al mod's pad injector, sharing the slot) is what the detour calls.
//   * the keyboard and the mouse as the game reads them: DirectInput's GetDeviceState/GetDeviceData,
//     found through a throwaway device of our own (never acquired, never read - it exists for its
//     vtable, which the game's devices share), zeroed while captured. Installed on the first
//     Present, after the game has enumerated its devices (InstallKeyboardCapture).
//   * the mouse buttons and the wheel as the game reads them: not through DirectInput at all. Its
//     input bindings ask GetKeyState(VK_LBUTTON..VK_XBUTTON2) (FUN_0047B2D0), and the wheel is the
//     WM_MOUSEWHEEL its window procedure (FUN_00447A00) accumulates. The game's GetKeyState import
//     slot is patched here; host.cpp's window procedure keeps the wheel from it.
//   * the cursor: the game hides the system cursor and recentres it with SetCursorPos every frame
//     for its mouse camera (it imports SetCursorPos/GetCursorPos/ShowCursor, and SenPatcher's
//     "disable mouse capture" patch is exactly that function). Only the game's own import slots are
//     patched, so ImGui's calls - through this dll's imports - see the real cursor, while the game
//     is told the cursor still sits where it put it.
//
// Letting go: whatever is held at the moment the overlay lets go (the B, Esc or click that closed
// it, the chord that toggled it) stays hidden from the game until it is released, so closing never
// doubles as a press in the game. Each hook latches what is down on its first poll after the
// capture ended - in the hook, not when the frame lets go: a button pressed after the game's last
// captured poll and before the overlay's own read would otherwise reach the game as a fresh press.
#include "overlay.h"

#include "atmt_input.h"

#include <cstdio>
#include <cstring>

namespace atmt_overlay {

namespace {

// ---------------------------------------------------------------- import slots
// The slot in `module`'s import table through which it calls `function` of `dll`, or NULL. Read
// from the module's own PE headers, so it holds for any build of the game.
void** FindImportSlot(HMODULE module, const char* dll, const char* function) {
    auto* base = reinterpret_cast<uint8_t*>(module);
    auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return nullptr;
    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    const IMAGE_DATA_DIRECTORY& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (dir.VirtualAddress == 0) return nullptr;
    for (auto* desc = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(base + dir.VirtualAddress);
         desc->Name != 0; ++desc) {
        if (_stricmp(reinterpret_cast<const char*>(base + desc->Name), dll) != 0) continue;
        if (desc->OriginalFirstThunk == 0) continue;   // no names to match against
        auto* names = reinterpret_cast<IMAGE_THUNK_DATA*>(base + desc->OriginalFirstThunk);
        auto* slots = reinterpret_cast<IMAGE_THUNK_DATA*>(base + desc->FirstThunk);
        for (; names->u1.AddressOfData != 0; ++names, ++slots) {
            if (IMAGE_SNAP_BY_ORDINAL(names->u1.Ordinal)) continue;
            auto* by_name = reinterpret_cast<IMAGE_IMPORT_BY_NAME*>(base + names->u1.AddressOfData);
            if (strcmp(reinterpret_cast<const char*>(by_name->Name), function) == 0) {
                return reinterpret_cast<void**>(&slots->u1.Function);
            }
        }
    }
    return nullptr;
}

bool PatchSlot(void** slot, void* replacement, void** original) {
    if (slot == nullptr) return false;
    DWORD old_protect = 0;
    if (VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &old_protect) == 0) return false;
    *original = *slot;
    *slot = replacement;
    DWORD ignored = 0;
    VirtualProtect(slot, sizeof(void*), old_protect, &ignored);
    return true;
}

// ---------------------------------------------------------------- the pad
constexpr uintptr_t kXInputGetStateIatVa = 0x0136A500;   // the shipped exe's slot (no ASLR)
constexpr uintptr_t kShippedImageBase = 0x00400000;

struct XInputState {
    uint32_t packet;
    uint16_t buttons;
    uint8_t left_trigger;
    uint8_t right_trigger;
    int16_t thumb_lx;
    int16_t thumb_ly;
    int16_t thumb_rx;
    int16_t thumb_ry;
};
static_assert(sizeof(XInputState) == 16, "XINPUT_STATE is 16 bytes");

typedef uint32_t(__stdcall* XInputGetStateFn)(uint32_t index, XInputState* state);

XInputGetStateFn g_real_xinput = nullptr;
volatile LONG g_pad_installed = 0;
volatile LONG g_pad_capture = 0;
uint32_t g_packet = 1;
uint32_t g_pad_index = 0;       // the slot that answered last
DWORD g_pad_retry_tick = 0;     // no pad anywhere: when to look again
volatile LONG g_logged_swallow = 0;

// XINPUT_GAMEPAD_TRIGGER_THRESHOLD, and the two triggers as bits above the 16 button bits.
constexpr uint8_t kTriggerThreshold = 30;
constexpr uint32_t kLeftTriggerBit = 0x10000;
constexpr uint32_t kRightTriggerBit = 0x20000;

// Per pad slot: what was held when the capture ended and is not released yet ("Letting go").
struct PadLatch {
    bool captured = false;   // the last poll of this slot was captured
    uint32_t mask = 0;
};
PadLatch g_pad_latch[4];

uint32_t PadDown(const XInputState& state) {
    uint32_t down = state.buttons;
    if (state.left_trigger > kTriggerThreshold) down |= kLeftTriggerBit;
    if (state.right_trigger > kTriggerThreshold) down |= kRightTriggerBit;
    return down;
}

uint32_t __stdcall HookedGetState(uint32_t index, XInputState* state) {
    const uint32_t rc = g_real_xinput(index, state);
    if (rc != 0 || state == nullptr) return rc;
    PadLatch* latch = index < 4 ? &g_pad_latch[index] : nullptr;
    // The settings bar's chord never reaches the game whole: the game polls before the frame is
    // presented, so on the poll the chord completes the overlay is not open yet.
    const unsigned short chord = Opts().settings_chord;
    if (latch != nullptr && chord != 0 && (state->buttons & chord) == chord) latch->mask |= chord;
    if (g_pad_capture == 0) {
        if (latch == nullptr) return rc;
        const uint32_t down = PadDown(*state);
        if (latch->captured) latch->mask = down;   // the first poll after the overlay let go
        latch->captured = false;
        latch->mask &= down;
        if (latch->mask != 0) {
            state->buttons = static_cast<uint16_t>(state->buttons & ~latch->mask);
            if ((latch->mask & kLeftTriggerBit) != 0) state->left_trigger = 0;
            if ((latch->mask & kRightTriggerBit) != 0) state->right_trigger = 0;
        }
        return rc;
    }
    if (latch != nullptr) latch->captured = true;
    // Captured: the game sees a pad on which nothing is pressed. The packet number moves on when
    // the real state changed, so the game notices the "release" of whatever was held.
    const bool moved = state->buttons != 0 || state->left_trigger != 0 || state->right_trigger != 0
                       || state->thumb_lx != 0 || state->thumb_ly != 0 || state->thumb_rx != 0
                       || state->thumb_ry != 0;
    memset(&state->buttons, 0, sizeof(XInputState) - sizeof(state->packet));
    if (moved) state->packet = ++g_packet;
    if (InterlockedCompareExchange(&g_logged_swallow, 1, 0) == 0) {
        Log("overlay: the game's XInput poll is swallowed while the overlay is open (slot %u)", index);
    }
    return rc;
}

atmt::Hold g_nav_up, g_nav_down, g_nav_left, g_nav_right, g_nav_prev, g_nav_next, g_nav_accept,
    g_nav_back;

// ---------------------------------------------------------------- DirectInput
constexpr unsigned kDirectInputVersion = 0x0800;
constexpr unsigned kCreateDeviceIndex = 3;
constexpr unsigned kGetDeviceStateIndex = 9;
constexpr unsigned kGetDeviceDataIndex = 10;

const GUID kIidDirectInput8W = {
    0xBF798031, 0x483A, 0x4DA2, {0xAA, 0x99, 0x5D, 0x64, 0xED, 0x36, 0x97, 0x00}};
const GUID kGuidSysKeyboard = {
    0x6F1D2B61, 0xD5A0, 0x11CF, {0xBF, 0xC7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00}};

struct DeviceObjectData {
    uint32_t ofs;
    uint32_t data;
    uint32_t timestamp;
    uintptr_t sequence;
};

typedef int32_t(__stdcall* GetDeviceStateFn)(void* self, uint32_t size, void* data);
typedef int32_t(__stdcall* GetDeviceDataFn)(void* self, uint32_t size, DeviceObjectData* data,
                                           uint32_t* count, uint32_t flags);

GetDeviceStateFn g_real_state = nullptr;
GetDeviceDataFn g_real_data = nullptr;
volatile LONG g_keyboard_installed = 0;
volatile LONG g_keyboard_capture = 0;
volatile LONG g_logged_poll = 0;

volatile LONG g_mouse_capture = 0;

// The device states the game reads, and where their buttons are (a byte each, high bit = down):
// the keyboard (256 bytes, every one a key), the mouse (20 bytes: three axes, then 8 buttons) and a
// DirectInput pad (DIJOYSTATE2: 128 buttons after the axes, sliders and POVs).
struct DeviceLatch {
    uint32_t size;
    uint32_t first;
    uint32_t count;
    bool captured;       // the last poll of this device was captured
    bool held[256];      // held since the capture ended ("Letting go")
};
DeviceLatch g_device_latch[] = {{0x100, 0, 256}, {0x14, 12, 8}, {0x110, 48, 128}};

DeviceLatch* FindDeviceLatch(uint32_t size) {
    for (DeviceLatch& latch : g_device_latch) {
        if (latch.size == size) return &latch;
    }
    return nullptr;
}

void ApplyDeviceLatch(DeviceLatch* latch, uint8_t* data) {
    const bool first = latch->captured;   // the first poll after the overlay let go
    latch->captured = false;
    for (uint32_t i = 0; i < latch->count; ++i) {
        uint8_t& button = data[latch->first + i];
        const bool down = (button & 0x80) != 0;
        if (first) latch->held[i] = down;
        if (!down) latch->held[i] = false;
        if (latch->held[i]) button = 0;
    }
}

int32_t __stdcall HookedGetDeviceState(void* self, uint32_t size, void* data) {
    const int32_t hr = g_real_state(self, size, data);
    DeviceLatch* latch = FindDeviceLatch(size);
    if (hr != 0 || data == nullptr || latch == nullptr) return hr;
    // The mouse is also held while only the pointer is the overlay's (a window that holds nothing).
    const bool captured = g_keyboard_capture != 0 || (size == 0x14 && g_mouse_capture != 0);
    if (!captured) {
        ApplyDeviceLatch(latch, static_cast<uint8_t*>(data));
    } else {
        latch->captured = true;
        ZeroMemory(data, size);
        if (InterlockedCompareExchange(&g_logged_poll, 1, 0) == 0) {
            Log("overlay: the game's DirectInput poll (%u bytes) is swallowed while the overlay is "
                "open", size);
        }
    }
    return hr;
}

int32_t __stdcall HookedGetDeviceData(void* self, uint32_t size, DeviceObjectData* data,
                                      uint32_t* count, uint32_t flags) {
    const int32_t hr = g_real_data(self, size, data, count, flags);
    if (hr == 0 && data != nullptr && count != nullptr
        && (g_keyboard_capture != 0 || g_mouse_capture != 0)) {
        for (uint32_t i = 0; i < *count; ++i) data[i].data = 0;   // every event reads as a release
    }
    return hr;
}

// ---------------------------------------------------------------- the mouse buttons
typedef SHORT(WINAPI* GetKeyStateFn)(int vk);

GetKeyStateFn g_real_get_key_state = nullptr;
bool g_button_captured[VK_XBUTTON2 + 1] = {};   // by virtual key: the last ask was captured
bool g_button_held[VK_XBUTTON2 + 1] = {};       // held since the capture ended ("Letting go")

bool IsMouseButton(int vk) {
    return vk == VK_LBUTTON || vk == VK_RBUTTON || (vk >= VK_MBUTTON && vk <= VK_XBUTTON2);
}

// Only the game calls this (its own import slot), and only for its mouse bindings: its keys come
// from DirectInput, so anything but a mouse button passes untouched.
SHORT WINAPI HookedGetKeyState(int vk) {
    const SHORT state = g_real_get_key_state(vk);
    if (!IsMouseButton(vk)) return state;
    const bool down = state < 0;
    if (g_mouse_capture != 0) {
        g_button_captured[vk] = true;
        return 0;
    }
    if (g_button_captured[vk]) g_button_held[vk] = down;   // the first ask after the overlay let go
    g_button_captured[vk] = false;
    if (!down) g_button_held[vk] = false;
    return g_button_held[vk] ? 0 : state;
}

// ---------------------------------------------------------------- the cursor
typedef BOOL(WINAPI* SetCursorPosFn)(int x, int y);
typedef BOOL(WINAPI* GetCursorPosFn)(LPPOINT point);

SetCursorPosFn g_real_set_cursor = nullptr;
GetCursorPosFn g_real_get_cursor = nullptr;
volatile LONG g_cursor_free = 0;
volatile LONG g_game_point_x = 0;
volatile LONG g_game_point_y = 0;
volatile LONG g_have_game_point = 0;

// Only the game calls these (they sit in its own import slots).
BOOL WINAPI HookedSetCursorPos(int x, int y) {
    InterlockedExchange(&g_game_point_x, x);
    InterlockedExchange(&g_game_point_y, y);
    InterlockedExchange(&g_have_game_point, 1);
    if (g_cursor_free != 0) return TRUE;   // the pointer is the overlay's: do not snap it back
    return g_real_set_cursor(x, y);
}

BOOL WINAPI HookedGetCursorPos(LPPOINT point) {
    if (g_cursor_free != 0 && point != nullptr && g_have_game_point != 0) {
        // Where the game last put it: its mouse camera sees no movement while the pointer moves.
        point->x = g_game_point_x;
        point->y = g_game_point_y;
        return TRUE;
    }
    return g_real_get_cursor(point);
}

}  // namespace

// ---------------------------------------------------------------- the pad
bool InstallPad(std::string* why_not) {
    if (InterlockedCompareExchange(&g_pad_installed, 1, 0) != 0) return true;
    // Only ever the game's own import slot, never the export in xinput1_3.dll: the mods start
    // before the game's entry point (the loader's entry gate), i.e. before Steam's overlay hooks
    // that export for Steam Input, and a hook of ours already sitting on it left the game blind to
    // the pad until the Guide button was pressed (2026-10-01). The slot is only the game's calls,
    // which is all that is taken away; whatever the slot held (the export, with whatever Steam put
    // on it later) is what the detour and ReadPad call.
    const uintptr_t base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    // The game imports XInput by ordinal (2 = XInputGetState), which FindImportSlot does not match
    // by name: the shipped exe's slot is the usual answer.
    void** slot = FindImportSlot(GetModuleHandleW(nullptr), "XINPUT1_3.dll", "XInputGetState");
    if (slot == nullptr) {
        slot = reinterpret_cast<void**>(base + (kXInputGetStateIatVa - kShippedImageBase));
    }
    if (!IsExecutableCode(*slot)) {   // not the exe this was written for
        if (why_not != nullptr) *why_not = "the game's XInputGetState import slot holds no code";
        InterlockedExchange(&g_pad_installed, 0);
        return false;
    }
    void* original = nullptr;
    if (!PatchSlot(slot, reinterpret_cast<void*>(&HookedGetState), &original) || original == nullptr) {
        if (why_not != nullptr) *why_not = "could not patch the game's XInputGetState import slot";
        InterlockedExchange(&g_pad_installed, 0);
        return false;
    }
    g_real_xinput = reinterpret_cast<XInputGetStateFn>(original);
    Log("overlay: XInputGetState patched in the game's import slot (was 0x%08x)",
        static_cast<unsigned>(reinterpret_cast<uintptr_t>(original)));
    return true;
}

bool PadInstalled() { return g_real_xinput != nullptr; }

bool ReadPad(AtmtOverlayInput* out) {
    memset(out, 0, sizeof(*out));
    if (g_real_xinput == nullptr) return false;
    XInputState state;
    memset(&state, 0, sizeof(state));
    if (g_real_xinput(g_pad_index, &state) != 0) {
        // XInputGetState on an empty slot is slow enough to show in a frame: look for another pad
        // at most once a second.
        const DWORD now = GetTickCount();
        if (static_cast<LONG>(now - g_pad_retry_tick) < 0) return false;
        bool found = false;
        for (uint32_t index = 0; index < 4 && !found; ++index) {
            if (index != g_pad_index && g_real_xinput(index, &state) == 0) {
                g_pad_index = index;
                found = true;
            }
        }
        if (!found) {
            g_pad_retry_tick = now + 1000;
            return false;
        }
    }
    out->pad_buttons = state.buttons;
    out->thumb_lx = state.thumb_lx;
    out->thumb_ly = state.thumb_ly;
    out->thumb_rx = state.thumb_rx;
    out->thumb_ry = state.thumb_ry;
    out->have_pad = 1;
    return true;
}

// Told every frame, including the frames on which nothing is open: a flag cleared only by a poll
// that no longer happens left the game without its pad once (docs/OVERLAY.md, 2026-09-21).
bool PadCaptured() { return g_pad_capture != 0; }

void SetPadCapture(bool capture) {
    const LONG wanted = capture ? 1 : 0;
    if (InterlockedExchange(&g_pad_capture, wanted) != wanted) {
        Log(capture ? "overlay: the pad is the overlay's - the game gets an idle pad"
                    : "overlay: the pad is the game's again");
    }
}

PadNav DecodePadNav(const AtmtOverlayInput& pad) {
    using atmt::Pressed;
    PadNav nav;
    const unsigned short b = pad.pad_buttons;
    // Rows move one at a time however long a direction is held (a list of ten settings needs no
    // acceleration); a value accelerates the way the log's scroll does.
    if (Pressed(g_nav_up, (b & atmt::kPadUp) != 0 || pad.thumb_ly > atmt::kStickDeadzone, true)) --nav.rows;
    if (Pressed(g_nav_down, (b & atmt::kPadDown) != 0 || pad.thumb_ly < -atmt::kStickDeadzone, true)) {
        ++nav.rows;
    }
    if (Pressed(g_nav_left, (b & atmt::kPadLeft) != 0 || pad.thumb_lx < -atmt::kStickDeadzone, true)) {
        nav.adjust -= atmt::HeldStep(g_nav_left);
    }
    if (Pressed(g_nav_right, (b & atmt::kPadRight) != 0 || pad.thumb_lx > atmt::kStickDeadzone, true)) {
        nav.adjust += atmt::HeldStep(g_nav_right);
    }
    if (Pressed(g_nav_prev, (b & atmt::kPadLB) != 0, false)) --nav.menus;
    if (Pressed(g_nav_next, (b & atmt::kPadRB) != 0, false)) ++nav.menus;
    nav.accept = Pressed(g_nav_accept, (b & atmt::kPadA) != 0, false);
    nav.back = Pressed(g_nav_back, (b & atmt::kPadB) != 0, false);
    return nav;
}

// ---------------------------------------------------------------- DirectInput
// Called on the first Present, never from AtmtModInit. The game makes its pads in one DirectInput
// EnumDevices at startup (FUN_00940530 -> FUN_0093B1E0 -> FUN_0093AF90: an XInput pad object only
// for a device that enumeration lists) and enumerates again only after WM_DEVICECHANGE
// (FUN_00940580). With a DirectInput object and hooks of ours made while the loader held the game's
// entry point - before the game's own DirectInput and Steam's overlay - the game found no pad
// until the Guide button was pressed (2026-10-01; the suspected cause, see docs/OVERLAY.md).
bool InstallKeyboardCapture(std::string* why_not) {
    if (InterlockedCompareExchange(&g_keyboard_installed, 1, 0) != 0) return true;
    auto fail = [&](const char* why) {
        if (why_not != nullptr) *why_not = why;
        InterlockedExchange(&g_keyboard_installed, 0);
        return false;
    };
    HMODULE dinput = LoadLibraryW(L"dinput8.dll");
    if (dinput == nullptr) return fail("dinput8.dll is not available");
    typedef int32_t(__stdcall* CreateFn)(void*, uint32_t, const GUID&, void**, void*);
    auto create = reinterpret_cast<CreateFn>(GetProcAddress(dinput, "DirectInput8Create"));
    if (create == nullptr) return fail("DirectInput8Create is not exported");
    void* dinput8 = nullptr;
    if (create(reinterpret_cast<void*>(GetModuleHandleW(nullptr)), kDirectInputVersion,
               kIidDirectInput8W, &dinput8, nullptr) != 0
        || dinput8 == nullptr) {
        return fail("DirectInput8Create failed");
    }
    typedef int32_t(__stdcall* CreateDeviceFn)(void*, const GUID&, void**, void*);
    auto create_device =
        reinterpret_cast<CreateDeviceFn>((*reinterpret_cast<void***>(dinput8))[kCreateDeviceIndex]);
    void* device = nullptr;
    if (create_device(dinput8, kGuidSysKeyboard, &device, nullptr) != 0 || device == nullptr) {
        return fail("could not create a DirectInput keyboard device");
    }
    void** vtbl = *reinterpret_cast<void***>(device);
    void* state_fn = vtbl[kGetDeviceStateIndex];
    void* data_fn = vtbl[kGetDeviceDataIndex];
    if (!IsExecutableCode(state_fn) || !IsExecutableCode(data_fn)) {
        return fail("the device's poll is not code inside a module");
    }
    // The device is kept (never acquired, never read): releasing it would gain nothing, and the
    // hooks below sit in code that stays loaded either way.
    const AtmtModApi* api = Api();
    void* state_trampoline = nullptr;
    void* data_trampoline = nullptr;
    const bool ok =
        api->hook_create(state_fn, reinterpret_cast<void*>(&HookedGetDeviceState), &state_trampoline)
            != nullptr
        && api->hook_create(data_fn, reinterpret_cast<void*>(&HookedGetDeviceData), &data_trampoline)
               != nullptr
        && api->hook_enable(state_fn) == 0 && api->hook_enable(data_fn) == 0;
    if (!ok || state_trampoline == nullptr || data_trampoline == nullptr) {
        return fail("could not hook the DirectInput poll");
    }
    g_real_state = reinterpret_cast<GetDeviceStateFn>(state_trampoline);
    g_real_data = reinterpret_cast<GetDeviceDataFn>(data_trampoline);
    Log("overlay: DirectInput keyboard/mouse poll hooked");
    return true;
}

void SetKeyboardCapture(bool capture) { InterlockedExchange(&g_keyboard_capture, capture ? 1 : 0); }

bool KeyboardCaptureActive() { return g_real_state != nullptr && g_keyboard_capture != 0; }

// ---------------------------------------------------------------- the mouse buttons
bool InstallMouseButtonHook(std::string* why_not) {
    void** slot = FindImportSlot(GetModuleHandleW(nullptr), "USER32.dll", "GetKeyState");
    void* original = nullptr;
    if (!PatchSlot(slot, reinterpret_cast<void*>(&HookedGetKeyState), &original) || original == nullptr) {
        if (why_not != nullptr) *why_not = "could not patch the game's GetKeyState slot";
        return false;
    }
    g_real_get_key_state = reinterpret_cast<GetKeyStateFn>(original);
    Log("overlay: the game's GetKeyState patched (its mouse buttons are held while the overlay has "
        "the mouse)");
    return true;
}

void SetMouseCapture(bool capture) { InterlockedExchange(&g_mouse_capture, capture ? 1 : 0); }

bool MouseCaptured() { return g_mouse_capture != 0; }

// ---------------------------------------------------------------- the cursor
bool InstallCursorHooks(std::string* why_not) {
    HMODULE game = GetModuleHandleW(nullptr);
    void** set_slot = FindImportSlot(game, "USER32.dll", "SetCursorPos");
    void** get_slot = FindImportSlot(game, "USER32.dll", "GetCursorPos");
    if (set_slot == nullptr || get_slot == nullptr) {
        if (why_not != nullptr) *why_not = "the game does not import SetCursorPos/GetCursorPos";
        return false;
    }
    void* original_set = nullptr;
    void* original_get = nullptr;
    if (!PatchSlot(set_slot, reinterpret_cast<void*>(&HookedSetCursorPos), &original_set)) {
        if (why_not != nullptr) *why_not = "could not patch the game's SetCursorPos slot";
        return false;
    }
    g_real_set_cursor = reinterpret_cast<SetCursorPosFn>(original_set);
    if (!PatchSlot(get_slot, reinterpret_cast<void*>(&HookedGetCursorPos), &original_get)) {
        if (why_not != nullptr) *why_not = "could not patch the game's GetCursorPos slot";
        return false;
    }
    g_real_get_cursor = reinterpret_cast<GetCursorPosFn>(original_get);
    Log("overlay: the game's SetCursorPos/GetCursorPos patched (the pointer is free while the "
        "overlay is open)");
    return true;
}

void SetCursorFree(bool free) { InterlockedExchange(&g_cursor_free, free ? 1 : 0); }

}  // namespace atmt_overlay
