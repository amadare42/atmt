// overlay.h - the overlay mod's internals (see shared/overlay_api.h for what it offers other mods).
#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <cstdint>
#include <string>

#include "mod_api.h"
#include "overlay_api.h"

namespace atmt_overlay {

// ---------------------------------------------------------------- mod_entry.cpp
const AtmtModApi* Api();
void Log(const char* format, ...);          // one line in atmt_loader.log, prefixed by the loader
void LogError(const char* format, ...);     // a failure: logged unless LogLevel=off (Log: LogLevel=all)
// Committed, executable memory inside a loaded module: hooks only ever go on code.
bool IsExecutableCode(const void* p);

// The overlay's own settings (its "Overlay" menu, saved in atmt_overlay.ini), as the frame reads
// them. Written by the registry's on_change on the render thread, or once at startup.
struct Options {
    unsigned settings_key = 0x71;            // VK_F2
    unsigned short settings_chord = 0x00C0;  // L3+R3
    int font_size = 20;                      // the bar's text size (and the shared style's)
    bool mouse = true;                       // restart: the window procedure hook
    bool cursor = true;                      // draw a cursor while something is open
    std::string font_path;                   // restart: a .ttf/.otf for everything drawn
    std::string settings_key_name = "F2";
    std::string settings_chord_name = "L3+R3";
};
const Options& Opts();

// ---------------------------------------------------------------- host.cpp
// Hooks the game's device creation (and, when the game already has one, Present through a probe
// swapchain). Everything that touches ImGui or D3D then happens on the render thread.
bool InstallHost();
const AtmtOverlayApi* Service();              // what mod_entry.cpp publishes
void ShutdownHost();

// ---------------------------------------------------------------- input.cpp
// The pad: XInputGetState, hooked so the overlay reads the real pad and the game can be handed an
// idle one while something is open.
bool InstallPad(std::string* why_not);
bool PadInstalled();
bool ReadPad(AtmtOverlayInput* out);          // the real pad, now
void SetPadCapture(bool capture);            // called every frame: see input.cpp
bool PadCaptured();                          // the game is handed an idle pad right now

// The settings bar's reading of the pad: up/down pick a row, left/right change the value (repeated,
// accelerated), LB/RB switch menus, A acts, B closes.
struct PadNav {
    int rows = 0;
    int adjust = 0;
    int menus = 0;
    bool accept = false;
    bool back = false;
};
PadNav DecodePadNav(const AtmtOverlayInput& pad);

// The keyboard and the mouse as the game reads them (DirectInput): zeroed while captured.
bool InstallKeyboardCapture(std::string* why_not);
void SetKeyboardCapture(bool capture);
bool KeyboardCaptureActive();

// The game's mouse buttons (it asks GetKeyState for them) and its wheel (WM_MOUSEWHEEL, dropped by
// host.cpp's window procedure): held while the overlay has the mouse.
bool InstallMouseButtonHook(std::string* why_not);
void SetMouseCapture(bool capture);          // called every frame, like the others
bool MouseCaptured();

// The game's own cursor handling (it recentres the cursor with SetCursorPos for its mouse camera):
// while the cursor is "free" the game's SetCursorPos is skipped and its GetCursorPos answers the
// point it last set, so the pointer can move and the camera does not.
bool InstallCursorHooks(std::string* why_not);
void SetCursorFree(bool free);

}  // namespace atmt_overlay
