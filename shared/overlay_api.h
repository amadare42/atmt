// overlay_api.h - the overlay mod's interface for the mods that draw in it.
//
// The overlay (mods/overlay, atmt_overlay.dll) owns everything a UI over the game needs exactly once
// per process: the Present hook, the ImGui context and its D3D11/win32 backends, the mouse (its
// cursor included), the pad and keyboard hooks that take input away from the game, and the settings
// bar. It publishes this interface with the loader (AtmtModApi::service_publish, name
// ATMT_OVERLAY_SERVICE); a mod that wants a window of its own finds it with service_find and adds a
// AtmtOverlayWindow. The dialog log's panel is one (mods/trails_dialog_logger/src/panel_client.cpp).
//
// Every frame, on the render thread, the overlay calls each window's update() - always, so a window
// can watch its own hotkey while it is closed - and then, inside one ImGui frame, draw() for those
// that asked to be drawn. A window never touches D3D and never hooks input: it reads keys with
// GetAsyncKeyState (shared/atmt_input.h) and the pad from the frame, and says through update()'s
// answer whether it wants the input held and the cursor shown.
//
// ImGui across dlls: each mod links its own copy of ImGui (the same pinned version - checked with
// imgui_version), and a copy has its own "current context" global. So a window's draw() starts with
//     ImGui::SetCurrentContext((ImGuiContext*)frame->imgui_context);
//     ImGui::SetAllocatorFunctions(frame->imgui_alloc, frame->imgui_free, frame->imgui_alloc_user);
// and then draws with plain ImGui calls, into the overlay's context.
//
// ABI rules as in mod_api.h: plain C, 32-bit, __cdecl, no exceptions across the boundary.
#ifndef ATMT_OVERLAY_API_H
#define ATMT_OVERLAY_API_H

#include <stddef.h>
#include <stdint.h>

#include "mod_api.h"

#define ATMT_OVERLAY_SERVICE "atmt.overlay"
#define ATMT_OVERLAY_API_VERSION 1u

/* The raw pad, read through the overlay's XInputGetState hook (the real pad, never the idle one the
 * game may be handed). XINPUT_GAMEPAD's buttons and sticks; have_pad = 0 when no pad answered. */
typedef struct AtmtOverlayInput {
    uint16_t pad_buttons;
    int16_t thumb_lx, thumb_ly, thumb_rx, thumb_ry;
    int32_t have_pad;
} AtmtOverlayInput;

/* frame->flags */
#define ATMT_OVERLAY_FRAME_INPUT   0x1u  /* nothing else holds the input: act on keys and the pad */
#define ATMT_OVERLAY_FRAME_HOTKEYS 0x2u  /* toggle hotkeys may act (not while the bar picks a key) */
#define ATMT_OVERLAY_FRAME_CLOSE   0x4u  /* the settings bar (or another window) has the input: close */
#define ATMT_OVERLAY_FRAME_PREVIEW 0x8u  /* the bar shows this mod's menu: draw as a live preview */

typedef struct AtmtOverlayFrame {
    uint32_t size;          /* sizeof(AtmtOverlayFrame) */
    uint32_t flags;         /* ATMT_OVERLAY_FRAME_* */
    void* imgui_context;    /* ImGuiContext* */
    void* (*imgui_alloc)(size_t size, void* user);   /* ImGuiMemAllocFunc */
    void (*imgui_free)(void* ptr, void* user);       /* ImGuiMemFreeFunc */
    void* imgui_alloc_user;
    AtmtOverlayInput input;
    float bar_alpha;        /* the settings bar's fade, 0..1 (for a preview's own fade) */
} AtmtOverlayFrame;

/* What update() answers */
#define ATMT_OVERLAY_WANT_DRAW  0x1u  /* call draw() this frame */
#define ATMT_OVERLAY_WANT_INPUT 0x2u  /* open and interactive: show the cursor, route the mouse */
#define ATMT_OVERLAY_HOLD_INPUT 0x4u  /* ...and take the keyboard/pad/mouse away from the game */
#define ATMT_OVERLAY_WANT_FOCUS 0x8u  /* close the settings bar: this window wants the input next */

typedef struct AtmtOverlayWindow {
    const char* name;           /* for the log */
    uint32_t imgui_version;     /* IMGUI_VERSION_NUM this mod was built with: must match */
    uint32_t(__cdecl* update)(const AtmtOverlayFrame* frame, void* user);
    void(__cdecl* draw)(const AtmtOverlayFrame* frame, void* user);
    void* user;
} AtmtOverlayWindow;

typedef struct AtmtOverlayApi {
    uint32_t version;           /* ATMT_OVERLAY_API_VERSION */
    uint32_t size;              /* sizeof(AtmtOverlayApi) */
    /* Adds `window` (copied) for the mod `self`; adding again replaces that mod's window. Returns 0,
     * or -1 (logged) when the ImGui version differs. */
    int(__cdecl* add_window)(const AtmtModApi* self, const AtmtOverlayWindow* window);
    /* Removes the mod's window. Waits for a frame in progress, so once it returns neither callback
     * runs again - call it before the dll that holds them goes away (a dev reload, a shutdown). */
    void(__cdecl* remove_window)(const AtmtModApi* self);
    /* The real pad, read now (any thread) - the same read the overlay's own chords use, so a mod with
     * a pad hotkey outside a window (on the game's thread) sees exactly what the overlay sees.
     * `held` = 1 while the overlay holds the input (the game is handed an idle pad meanwhile).
     * Returns out->have_pad. */
    int(__cdecl* read_pad)(AtmtOverlayInput* out, int32_t* held);
} AtmtOverlayApi;

#endif  // ATMT_OVERLAY_API_H
