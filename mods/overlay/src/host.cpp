// host.cpp - the overlay itself: drawn in the game's own swapchain, once per process, for every mod.
//
// Where the device and the swapchain come from: ed8.exe creates them through
// d3d11!D3D11CreateDeviceAndSwapChain (docs/ENGINE_NOTES.md) - executable code inside d3d11.dll, so the
// loader's hook service accepts it like any other target. The swapchain's Present is hooked too,
// on the *code the vtable points at*, not on the vtable slot (read-only data, which the loader
// refuses to patch). A game that already had its device when this mod arrived gets a throwaway
// swapchain of ours instead, only to learn where Present lives.
//
// Everything that touches ImGui or D3D happens on the render thread - inside Present - under one
// lock that add_window/remove_window take too, so a window that is removed is never called again.
// With nothing open and nothing fading, a frame costs the windows' update() calls and no drawing.
#include "imgui.h"
#include "imgui_impl_dx11.h"
#include "imgui_impl_win32.h"

#include <d3d11.h>
#include <dxgi.h>

#include "overlay.h"
#include "settings_bar.h"
#include "atmt_input.h"
#include "atmt_theme.h"

#include <cstdio>
#include <cstring>
#include <vector>

// ImGui's win32 header leaves this declaration commented out on purpose ("copy the line below into
// your .cpp"): it is the one entry point that needs <windows.h>.
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hwnd, UINT msg, WPARAM wparam,
                                                             LPARAM lparam);

namespace atmt_overlay {

// At namespace scope: the device hook installs the Present hook, and a hook must point at exactly
// one function.
HRESULT __stdcall HookedPresent(IDXGISwapChain* self, UINT sync_interval, UINT flags);
bool InstallPresentHook(void* present_code, bool adopt_any);

namespace {

// ---------------------------------------------------------------- D3D state
using CreateDeviceAndSwapChainFn = HRESULT(__stdcall*)(IDXGIAdapter*, D3D_DRIVER_TYPE, HMODULE, UINT,
                                                       const D3D_FEATURE_LEVEL*, UINT, UINT,
                                                       const DXGI_SWAP_CHAIN_DESC*,
                                                       IDXGISwapChain**, ID3D11Device**,
                                                       D3D_FEATURE_LEVEL*, ID3D11DeviceContext**);
using PresentFn = HRESULT(__stdcall*)(IDXGISwapChain*, UINT, UINT);

CreateDeviceAndSwapChainFn g_real_create = nullptr;
PresentFn g_real_present = nullptr;

ID3D11Device* g_device = nullptr;
ID3D11DeviceContext* g_context = nullptr;
IDXGISwapChain* g_swapchain = nullptr;
IDXGISwapChain* g_probe_swapchain = nullptr;
bool g_adopt_any_swapchain = false;
volatile LONG g_present_hook_started = 0;
HWND g_hwnd = nullptr;
WNDPROC g_old_wndproc = nullptr;
ID3D11RenderTargetView* g_rtv = nullptr;
UINT g_backbuffer_w = 0;
UINT g_backbuffer_h = 0;

volatile LONG g_imgui_ready = 0;
volatile LONG g_dinput_tried = 0;   // the DirectInput hooks are installed on the first Present
volatile LONG g_imgui_failed = 0;
volatile LONG g_inert = 0;   // shut down: forward everything untouched

// Two interface ids, spelled out: dxguid would be one more thing to link for two constants.
const GUID kIidD3D11Device = {
    0xDB6F6DDB, 0xAC77, 0x4E88, {0x82, 0x53, 0x81, 0x9D, 0xF9, 0xBB, 0xF1, 0x40}};
const GUID kIidTexture2D = {
    0x6F15AAF2, 0xD208, 0x4E89, {0x9A, 0xB4, 0x48, 0x95, 0x35, 0xD3, 0x4F, 0x9C}};

// ---------------------------------------------------------------- windows (other mods' UI)
struct Window {
    const AtmtModApi* owner;
    std::string mod_name;   // the loader's name for the mod, as the settings registry has it
    AtmtOverlayWindow window;
    uint32_t flags = 0;     // what this frame told it
    uint32_t answer = 0;    // what it answered
};
std::vector<Window> g_windows;
CRITICAL_SECTION g_frame_lock;   // a whole frame, add_window and remove_window

// ---------------------------------------------------------------- the settings bar and the frame
SettingsBar g_bar;
bool g_bar_open = false;
float g_bar_alpha = 0.0f;
atmt::KeyEdge g_bar_key;
bool g_chord_down = false;
bool g_was_showing = false;   // the bar or a window was open last frame: closing saves
DWORD g_last_tick = 0;
int g_applied_font_size = 0;
atmt::Hold g_key_up, g_key_down, g_key_left, g_key_right, g_key_tab, g_key_enter, g_key_space, g_key_esc;
constexpr float kFadeMs = 120.0f;

std::string Narrow(const wchar_t* text) {
    std::string out;
    for (; text != nullptr && *text != L'\0'; ++text) {
        out.push_back(*text < 128 ? static_cast<char>(*text) : '?');
    }
    return out;
}

// ---------------------------------------------------------------- device creation
HRESULT __stdcall HookedCreateDeviceAndSwapChain(IDXGIAdapter* adapter, D3D_DRIVER_TYPE driver_type,
                                                 HMODULE software, UINT flags,
                                                 const D3D_FEATURE_LEVEL* levels, UINT level_count,
                                                 UINT sdk_version, const DXGI_SWAP_CHAIN_DESC* desc,
                                                 IDXGISwapChain** swapchain, ID3D11Device** device,
                                                 D3D_FEATURE_LEVEL* feature_level,
                                                 ID3D11DeviceContext** context) {
    const HRESULT hr = g_real_create(adapter, driver_type, software, flags, levels, level_count,
                                     sdk_version, desc, swapchain, device, feature_level, context);
    if (FAILED(hr) || swapchain == nullptr || *swapchain == nullptr) return hr;
    if (g_swapchain != nullptr) return hr;   // the game's first swapchain is the one it draws in
    g_swapchain = *swapchain;
    if (device != nullptr) g_device = *device;   // these references stay the caller's
    if (context != nullptr) g_context = *context;
    if (desc != nullptr) g_hwnd = desc->OutputWindow;
    void** vtbl = *reinterpret_cast<void***>(g_swapchain);
    if (!InstallPresentHook(vtbl[8], false)) {   // IDXGISwapChain::Present
        LogError("overlay: could not hook the game's Present - no overlay");
        return hr;
    }
    Log("overlay: game swapchain 0x%08x, Present hooked at 0x%08x (window 0x%08x)",
        static_cast<unsigned>(reinterpret_cast<uintptr_t>(g_swapchain)),
        static_cast<unsigned>(reinterpret_cast<uintptr_t>(vtbl[8])),
        static_cast<unsigned>(reinterpret_cast<uintptr_t>(g_hwnd)));
    return hr;
}

// ---------------------------------------------------------------- ImGui, on the render thread
LRESULT CALLBACK HookedWndProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
    // Passed on, all but one: the game reads its keys and buttons elsewhere (DirectInput, XInput,
    // GetKeyState), and swallowing a message could break it in ways that are hard to see from the
    // outside. The exception is the wheel, which the game's own window procedure (FUN_00447A00)
    // turns into its mouse-wheel input: while the overlay has the mouse, the wheel is the overlay's.
    if (InterlockedCompareExchange(&g_imgui_ready, 0, 0) != 0) {
        ImGui_ImplWin32_WndProcHandler(hwnd, msg, wparam, lparam);
    }
    if ((msg == WM_MOUSEWHEEL || msg == WM_MOUSEHWHEEL) && MouseCaptured()) return 0;
    if (g_old_wndproc != nullptr) return CallWindowProcW(g_old_wndproc, hwnd, msg, wparam, lparam);
    return DefWindowProcW(hwnd, msg, wparam, lparam);
}

// The size has to be in the style *before* a font is added: AddFontDefault() picks ImGui's bitmap
// face (ProggyClean, 13 px) under 15 px and the scalable one above it - setting the size afterwards
// drew a 24 px request with a 13 px bitmap scaled up (2026-09-21). Every window draws with this
// font at its own size (ImGui 1.92 rasterizes whatever size a frame asks for).
void LoadFont() {
    ImGuiIO& io = ImGui::GetIO();
    const float size = static_cast<float>(Opts().font_size);
    ImGui::GetStyle().FontSizeBase = size;
    const std::string& path = Opts().font_path;
    if (!path.empty()) {
        if (GetFileAttributesA(path.c_str()) != INVALID_FILE_ATTRIBUTES
            && io.Fonts->AddFontFromFileTTF(path.c_str(), size) != nullptr) {
            Log("overlay: font = %s", path.c_str());
            return;
        }
        LogError("overlay: could not load FontPath=%s - using ImGui's own font", path.c_str());
    }
    io.Fonts->AddFontDefault();
}

bool InitImGui() {
    if (g_device == nullptr || g_context == nullptr || g_swapchain == nullptr) return false;
    if (g_hwnd == nullptr) {
        DXGI_SWAP_CHAIN_DESC desc;
        memset(&desc, 0, sizeof(desc));
        if (SUCCEEDED(g_swapchain->GetDesc(&desc))) g_hwnd = desc.OutputWindow;
    }
    if (g_hwnd == nullptr) {
        Log("overlay: the swapchain has no window - no overlay");
        return false;
    }
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;   // a mod must not write imgui.ini into the game folder
    io.LogFilename = nullptr;
    // The cursor is drawn by ImGui (MouseDrawCursor, while something is open): the game hid the
    // system's with ShowCursor(0), and the backend must not fight it over the cursor's shape.
    io.ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange;
    LoadFont();
    atmt::theme::ApplyGameTheme(static_cast<float>(Opts().font_size));
    g_applied_font_size = Opts().font_size;
    if (!ImGui_ImplWin32_Init(g_hwnd) || !ImGui_ImplDX11_Init(g_device, g_context)) {
        LogError("overlay: the ImGui backends refused the device or the window");
        return false;
    }
    if (Opts().mouse) {
        g_old_wndproc = reinterpret_cast<WNDPROC>(
            SetWindowLongPtrW(g_hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&HookedWndProc)));
        if (g_old_wndproc == nullptr) LogError("overlay: could not chain the window procedure - no mouse");
    }
    g_bar_key.set_key(Opts().settings_key);
    g_bar_key.Reset();   // a key already down at this moment is not a press
    InterlockedExchange(&g_imgui_ready, 1);
    return true;
}

// The bar's keyboard map, the same shape as the pad's (PadNav): arrows pick and change, Tab and
// Shift+Tab switch menus, Enter/Space act, Esc closes.
BarInput PollBarKeys() {
    using atmt::Pressed;
    BarInput in;
    if (Pressed(g_key_up, atmt::KeyDown(VK_UP), true)) --in.rows;
    if (Pressed(g_key_down, atmt::KeyDown(VK_DOWN), true)) ++in.rows;
    if (Pressed(g_key_left, atmt::KeyDown(VK_LEFT), true)) in.adjust -= atmt::HeldStep(g_key_left);
    if (Pressed(g_key_right, atmt::KeyDown(VK_RIGHT), true)) in.adjust += atmt::HeldStep(g_key_right);
    if (Pressed(g_key_tab, atmt::KeyDown(VK_TAB), false)) in.menus += atmt::KeyDown(VK_SHIFT) ? -1 : 1;
    if (Pressed(g_key_enter, atmt::KeyDown(VK_RETURN), false)) in.accept = true;
    if (Pressed(g_key_space, atmt::KeyDown(VK_SPACE), false)) in.accept = true;
    if (Pressed(g_key_esc, atmt::KeyDown(VK_ESCAPE), false)) in.back = true;
    return in;
}

std::string BarHint() {
    const Options& o = Opts();
    std::string keys = o.settings_key != 0 ? o.settings_key_name : std::string();
    if (o.settings_chord != 0) keys += (keys.empty() ? "" : " / ") + o.settings_chord_name;
    return "LB/RB menus   A change   B close" + (keys.empty() ? std::string() : "   [" + keys + "]");
}

float Fade(float alpha, bool open, float step) {
    alpha += open ? step : -step;
    if (alpha > 1.0f) alpha = 1.0f;
    if (alpha < 0.0f) alpha = 0.0f;
    return alpha;
}

void Render(IDXGISwapChain* self) {
    ImGui::Render();
    DXGI_SWAP_CHAIN_DESC desc;
    memset(&desc, 0, sizeof(desc));
    if (SUCCEEDED(self->GetDesc(&desc))
        && (desc.BufferDesc.Width != g_backbuffer_w || desc.BufferDesc.Height != g_backbuffer_h)) {
        // Resized (or still empty): the view of the old back buffer is not valid any more.
        if (g_rtv != nullptr) {
            g_rtv->Release();
            g_rtv = nullptr;
        }
        g_backbuffer_w = desc.BufferDesc.Width;
        g_backbuffer_h = desc.BufferDesc.Height;
    }
    if (g_rtv == nullptr) {
        ID3D11Texture2D* back_buffer = nullptr;
        if (SUCCEEDED(self->GetBuffer(0, kIidTexture2D, reinterpret_cast<void**>(&back_buffer)))
            && back_buffer != nullptr) {
            g_device->CreateRenderTargetView(back_buffer, nullptr, &g_rtv);
            back_buffer->Release();
        }
    }
    if (g_rtv == nullptr) return;
    g_context->OMSetRenderTargets(1, &g_rtv, nullptr);
    D3D11_VIEWPORT viewport;
    memset(&viewport, 0, sizeof(viewport));
    viewport.Width = static_cast<float>(g_backbuffer_w);
    viewport.Height = static_cast<float>(g_backbuffer_h);
    viewport.MaxDepth = 1.0f;
    g_context->RSSetViewports(1, &viewport);
    ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
}

// One frame of the overlay: input, the windows, the bar, and - only when something is on screen -
// an ImGui frame. Everything the game is (not) handed is decided here, every frame.
void Frame(IDXGISwapChain* self) {
    const Options& o = Opts();
    const AtmtModApi* api = Api();
    if (g_bar_key.key() != o.settings_key) {   // the bar's key changed in the bar itself
        g_bar_key.set_key(o.settings_key);
        g_bar_key.Reset();
    }

    AtmtOverlayInput pad;
    ReadPad(&pad);

    // ---- the bar's toggle: while it picks a key or a button, the one being pressed is the pick
    const bool picking = g_bar_open && g_bar.picking();
    bool toggle = g_bar_key.PressedOnce();
    const bool chord = pad.have_pad != 0 && o.settings_chord != 0
                       && (pad.pad_buttons & o.settings_chord) == o.settings_chord;
    if (chord && !g_chord_down) toggle = true;
    g_chord_down = chord;
    bool bar_opened = false;
    if (toggle && !picking) {
        g_bar_open = !g_bar_open;
        if (g_bar_open) {
            g_bar.Open();
            g_bar.set_hint(BarHint());
            bar_opened = true;
        }
    }

    // ---- the windows: every frame, open or not, so each can watch its own hotkey
    ImGuiMemAllocFunc alloc = nullptr;
    ImGuiMemFreeFunc free_fn = nullptr;
    void* alloc_user = nullptr;
    ImGui::GetAllocatorFunctions(&alloc, &free_fn, &alloc_user);
    AtmtOverlayFrame frame;
    memset(&frame, 0, sizeof(frame));
    frame.size = sizeof(frame);
    frame.imgui_context = ImGui::GetCurrentContext();
    frame.imgui_alloc = alloc;
    frame.imgui_free = free_fn;
    frame.imgui_alloc_user = alloc_user;
    frame.input = pad;
    frame.bar_alpha = g_bar_alpha;

    bool interactive = false;   // a window has the input (only one at a time)
    bool hold = false;
    bool focus = false;
    bool any_draw = false;
    // While the bar's chord is down, no window acts on its own pad hotkey: one that shares a button
    // with the chord (the dialog log's toggle on START, with the bar on BACK+START) would fire in the
    // same frame, ask for focus and close the bar it had just opened.
    for (Window& w : g_windows) {
        uint32_t flags = picking || chord ? 0 : ATMT_OVERLAY_FRAME_HOTKEYS;
        if (g_bar_open) {
            flags |= ATMT_OVERLAY_FRAME_CLOSE;
            if (g_bar.current_mod() == w.mod_name) flags |= ATMT_OVERLAY_FRAME_PREVIEW;
        } else if (!interactive) {
            flags |= ATMT_OVERLAY_FRAME_INPUT;
        } else {
            flags |= ATMT_OVERLAY_FRAME_CLOSE;
        }
        w.flags = flags;
        frame.flags = flags;
        w.answer = w.window.update != nullptr ? w.window.update(&frame, w.window.user) : 0;
        if ((w.answer & ATMT_OVERLAY_WANT_INPUT) != 0 && (flags & ATMT_OVERLAY_FRAME_INPUT) != 0) {
            interactive = true;
            if ((w.answer & ATMT_OVERLAY_HOLD_INPUT) != 0) hold = true;
        }
        if ((w.answer & ATMT_OVERLAY_WANT_FOCUS) != 0) focus = true;
        if ((w.answer & ATMT_OVERLAY_WANT_DRAW) != 0) any_draw = true;
    }
    if (focus && g_bar_open && !picking) g_bar_open = false;   // the window takes over next frame

    // ---- the bar's input
    BarInput bar_in;
    if (g_bar_open) {
        // Sampled every frame the bar is open so the edges stay true, acted on only when this is not
        // the frame it opened (the key that opened it is still down) and nothing is being picked.
        const BarInput keys = PollBarKeys();
        const PadNav nav = DecodePadNav(pad);
        if (!bar_opened && !picking) {
            bar_in.rows = keys.rows + nav.rows;
            bar_in.adjust = keys.adjust + nav.adjust;
            bar_in.menus = keys.menus + nav.menus;
            bar_in.accept = keys.accept || nav.accept;
            bar_in.back = keys.back || nav.back;
        }
        bar_in.pad_buttons = pad.pad_buttons;
    }

    // ---- saved once, when the last thing closes - not on every tick a value is held
    const bool showing = g_bar_open || interactive;
    if (g_was_showing && !showing && api->settings_commit != nullptr) api->settings_commit();
    g_was_showing = showing;

    // ---- what the game is handed: told every frame, including the frames nothing is open
    const bool take = g_bar_open || hold;   // the bar always holds: navigating it would walk
    SetKeyboardCapture(take);
    SetPadCapture(take);
    const bool pointer = showing && o.mouse;
    SetCursorFree(pointer);
    SetMouseCapture(take || pointer);   // a click on a window must not land in the game either

    const DWORD tick = GetTickCount();
    const DWORD elapsed = g_last_tick == 0 ? 0 : tick - g_last_tick;
    g_last_tick = tick;
    g_bar_alpha = Fade(g_bar_alpha, g_bar_open, static_cast<float>(elapsed) / kFadeMs);

    if (!any_draw && g_bar_alpha <= 0.0f) return;

    // ---- the ImGui frame
    if (g_applied_font_size != o.font_size) {
        // Between frames: ImGui reads the size when the next frame starts, and 1.92 rasterizes
        // whatever size is asked for, so this is instant rather than a re-init.
        g_applied_font_size = o.font_size;
        ImGui::GetStyle().FontSizeBase = static_cast<float>(o.font_size);
        atmt::theme::ApplyGameTheme(static_cast<float>(o.font_size));
    }
    ImGui::GetIO().MouseDrawCursor = pointer && o.cursor;
    ImGui_ImplDX11_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();
    for (Window& w : g_windows) {
        if ((w.answer & ATMT_OVERLAY_WANT_DRAW) == 0 || w.window.draw == nullptr) continue;
        frame.flags = w.flags;
        w.window.draw(&frame, w.window.user);
        ImGui::SetCurrentContext(static_cast<ImGuiContext*>(frame.imgui_context));   // ours again
    }
    if (g_bar_alpha > 0.0f && !g_bar.Draw(api, bar_in, g_bar_alpha)) g_bar_open = false;
    Render(self);
}

// ---------------------------------------------------------------- attaching late
bool AdoptSwapchain(IDXGISwapChain* self) {
    if (self == nullptr || self == g_probe_swapchain) return false;
    ID3D11Device* device = nullptr;
    if (FAILED(self->GetDevice(kIidD3D11Device, reinterpret_cast<void**>(&device))) || device == nullptr) {
        return false;
    }
    device->GetImmediateContext(&g_context);   // these references are ours, kept for the process
    g_device = device;
    DXGI_SWAP_CHAIN_DESC desc;
    memset(&desc, 0, sizeof(desc));
    if (SUCCEEDED(self->GetDesc(&desc))) g_hwnd = desc.OutputWindow;
    g_swapchain = self;
    Log("overlay: adopted the game's swapchain 0x%08x (window 0x%08x)",
        static_cast<unsigned>(reinterpret_cast<uintptr_t>(self)),
        static_cast<unsigned>(reinterpret_cast<uintptr_t>(g_hwnd)));
    return true;
}

// A device and a swapchain of our own, on a window of our own: they exist only to learn where
// Present lives, and are never drawn on or presented (Windows allows one swapchain per window, so
// the game's cannot be used).
bool CreateProbeSwapchain() {
    WNDCLASSEXW window_class;
    memset(&window_class, 0, sizeof(window_class));
    window_class.cbSize = sizeof(window_class);
    window_class.lpfnWndProc = DefWindowProcW;
    window_class.hInstance = GetModuleHandleW(nullptr);
    window_class.lpszClassName = L"atmt_overlay_probe";
    RegisterClassExW(&window_class);
    HWND hwnd = CreateWindowExW(0, window_class.lpszClassName, L"atmt", WS_POPUP, 0, 0, 64, 64,
                                nullptr, nullptr, window_class.hInstance, nullptr);
    if (hwnd == nullptr) return false;
    DXGI_SWAP_CHAIN_DESC desc;
    memset(&desc, 0, sizeof(desc));
    desc.BufferDesc.Width = 64;
    desc.BufferDesc.Height = 64;
    desc.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.BufferCount = 1;
    desc.OutputWindow = hwnd;
    desc.Windowed = TRUE;
    desc.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
    ID3D11Device* device = nullptr;
    IDXGISwapChain* swapchain = nullptr;
    // Through the trampoline: our own hook must not take this for the game's device.
    const HRESULT hr = g_real_create(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0,
                                     D3D11_SDK_VERSION, &desc, &swapchain, &device, nullptr, nullptr);
    if (device != nullptr) device->Release();
    if (FAILED(hr) || swapchain == nullptr) return false;
    g_probe_swapchain = swapchain;   // kept for the process; releasing it would race a live call
    return true;
}

DWORD WINAPI AttachFallbackThread(LPVOID) {
    // A cold start waits for the game's own device; when it does not come, the game was already
    // running before this mod was loaded. Ten seconds, not two: the loader reaches this mod early
    // in the game's startup, and two seconds ran out before the game made its device (2026-09-25).
    for (int i = 0; i < 200 && g_swapchain == nullptr; ++i) Sleep(50);
    if (g_swapchain != nullptr || g_real_create == nullptr) return 0;
    if (!CreateProbeSwapchain()) {
        LogError("overlay: could not create a probe swapchain - no overlay until a restart");
        return 0;
    }
    void** vtbl = *reinterpret_cast<void***>(g_probe_swapchain);
    Log(InstallPresentHook(vtbl[8], true)
            ? "overlay: the game already had its device - hooked Present instead"
            : "overlay: could not hook Present through the probe swapchain");
    return 0;
}

// ---------------------------------------------------------------- the service
int __cdecl AddWindow(const AtmtModApi* self, const AtmtOverlayWindow* window) {
    if (self == nullptr || window == nullptr) return -1;
    if (window->imgui_version != IMGUI_VERSION_NUM) {
        // Two ImGui versions would disagree about the context's layout: a crash, not a warning.
        LogError("overlay: %s was built with ImGui %u, this overlay with %u - its window is refused",
            window->name != nullptr ? window->name : "a window", window->imgui_version,
            static_cast<unsigned>(IMGUI_VERSION_NUM));
        return -1;
    }
    Window w;
    w.owner = self;
    w.mod_name = Narrow(self->mod_name);
    w.window = *window;
    EnterCriticalSection(&g_frame_lock);
    bool replaced = false;
    for (Window& existing : g_windows) {
        if (existing.owner == self) {
            existing = w;
            replaced = true;
        }
    }
    if (!replaced) g_windows.push_back(w);
    LeaveCriticalSection(&g_frame_lock);
    Log("overlay: window \"%s\" %s (%s)", window->name != nullptr ? window->name : "?",
        replaced ? "replaced" : "added", w.mod_name.c_str());
    return 0;
}

void __cdecl RemoveWindow(const AtmtModApi* self) {
    EnterCriticalSection(&g_frame_lock);   // waits for a frame in progress
    for (size_t i = g_windows.size(); i-- > 0;) {
        if (g_windows[i].owner == self) g_windows.erase(g_windows.begin() + static_cast<std::ptrdiff_t>(i));
    }
    LeaveCriticalSection(&g_frame_lock);
}

int __cdecl ReadPadService(AtmtOverlayInput* out, int32_t* held) {
    AtmtOverlayInput pad;
    ReadPad(&pad);
    if (out != nullptr) *out = pad;
    if (held != nullptr) *held = PadCaptured() ? 1 : 0;
    return pad.have_pad;
}

const AtmtOverlayApi kService = {ATMT_OVERLAY_API_VERSION, sizeof(AtmtOverlayApi), &AddWindow,
                                &RemoveWindow, &ReadPadService};

}  // namespace

bool InstallPresentHook(void* present_code, bool adopt_any) {
    if (InterlockedCompareExchange(&g_present_hook_started, 1, 0) != 0) return true;   // once
    if (!IsExecutableCode(present_code)) {
        LogError("overlay: Present is not code inside a module");
        return false;
    }
    const AtmtModApi* api = Api();
    void* trampoline = nullptr;
    if (api->hook_create(present_code, reinterpret_cast<void*>(&HookedPresent), &trampoline) == nullptr
        || trampoline == nullptr) {
        LogError("overlay: could not hook Present (another overlay?)");
        return false;
    }
    g_real_present = reinterpret_cast<PresentFn>(trampoline);
    g_adopt_any_swapchain = adopt_any;
    if (api->hook_enable(present_code) != 0) {
        Log("overlay: the Present hook was created but not enabled");
        return false;
    }
    return true;
}

HRESULT __stdcall HookedPresent(IDXGISwapChain* self, UINT sync_interval, UINT flags) {
    if (g_real_present == nullptr) return E_FAIL;
    if (self != g_swapchain) {
        // A swapchain of another window (ignored), or - when the hook came from the probe
        // swapchain - the game's own, seen for the first time.
        if (g_adopt_any_swapchain && g_swapchain == nullptr) AdoptSwapchain(self);
        if (self != g_swapchain) return g_real_present(self, sync_interval, flags);
    }
    if (InterlockedCompareExchange(&g_inert, 0, 0) == 0) {
        EnterCriticalSection(&g_frame_lock);
        if (InterlockedCompareExchange(&g_dinput_tried, 1, 0) == 0) {
            // Only now, not in AtmtModInit: see InstallKeyboardCapture (a DirectInput object made
            // before the game enumerates its pads left the game without its pad).
            std::string why;
            if (!InstallKeyboardCapture(&why)) {
                LogError("overlay: the game keeps seeing the keyboard - %s", why.c_str());
            }
        }
        if (InterlockedCompareExchange(&g_imgui_ready, 0, 0) == 0
            && InterlockedCompareExchange(&g_imgui_failed, 0, 0) == 0 && !InitImGui()) {
            InterlockedExchange(&g_imgui_failed, 1);
            LogError("overlay: could not start (see the lines above)");
        }
        if (InterlockedCompareExchange(&g_imgui_ready, 0, 0) != 0) Frame(self);
        LeaveCriticalSection(&g_frame_lock);
    }
    return g_real_present(self, sync_interval, flags);
}

bool InstallHost() {
    InitializeCriticalSection(&g_frame_lock);
    HMODULE d3d11 = LoadLibraryW(L"d3d11.dll");
    void* target = d3d11 != nullptr
                       ? reinterpret_cast<void*>(GetProcAddress(d3d11, "D3D11CreateDeviceAndSwapChain"))
                       : nullptr;
    if (target == nullptr || !IsExecutableCode(target)) {
        LogError("overlay: D3D11CreateDeviceAndSwapChain is not hookable here");
        return false;
    }
    const AtmtModApi* api = Api();
    void* trampoline = nullptr;
    if (api->hook_create(target, reinterpret_cast<void*>(&HookedCreateDeviceAndSwapChain), &trampoline)
            == nullptr
        || trampoline == nullptr) {
        LogError("overlay: could not hook D3D11CreateDeviceAndSwapChain");
        return false;
    }
    g_real_create = reinterpret_cast<CreateDeviceAndSwapChainFn>(trampoline);
    if (api->hook_enable(target) != 0) {
        Log("overlay: the device hook was created but not enabled");
        return false;
    }
    HANDLE thread = CreateThread(nullptr, 0, AttachFallbackThread, nullptr, 0, nullptr);
    if (thread != nullptr) CloseHandle(thread);
    return true;
}

const AtmtOverlayApi* Service() { return &kService; }

// The loader's shutdown of this mod (an explicit unload, never the process exit): nothing more is
// drawn, and the game gets its input back. The hooks go with the loader's own shutdown.
void ShutdownHost() {
    InterlockedExchange(&g_inert, 1);
    SetKeyboardCapture(false);
    SetPadCapture(false);
    SetCursorFree(false);
    SetMouseCapture(false);
}

}  // namespace atmt_overlay
