// pad.h - what the pad asks the dialog panel to do.
//
// The pad itself is the overlay's (mods/overlay/src/input.cpp): it hooks the game's XInputGetState,
// takes the pad away from the game while something is open, and hands every window the raw state
// each frame (AtmtOverlayInput). This is the panel's reading of that state: which direction scrolls,
// which button closes, and how a held direction repeats and speeds up.
#pragma once

#include <string>

#include "overlay_api.h"
#include "atmt_input.h"   // the button bits and names, shared with the overlay

namespace atmt {

// What one frame of pad input asks the panel to do. Directions repeat while held (a log is read by
// holding DOWN), buttons do not.
struct PadInput {
    int lines = 0;      // <0 up, >0 down (a direction, already repeated and accelerated)
    int pages = 0;      // <0 page up, >0 page down (left/right, LB/RB)
    bool accept = false;   // A: back to following the newest line
    bool close = false;    // B (or whatever OverlayPadClose says)
    bool toggle = false;   // the configured OverlayPadToggle button or chord, when there is one
    // The right stick resizes the panel while held, the same shape as `lines`: up/right positive.
    int resize_height_pct = 0;
    int resize_width_pct = 0;
    // Y/X: the text size, the pad's equivalent of Ctrl+Up/Ctrl+Down (repeated, accelerated).
    int font_size_delta = 0;
};

// The close and toggle buttons, by name ("B", "L3+R3"); an unknown or empty close button means B,
// an empty toggle means none. Changed live from the settings bar.
void SetPadButtons(const std::string& close_button, const std::string& toggle_button);

// Reads one frame. Called every frame, open or not, so the edges stay true; the caller decides what
// applies (only `toggle` while the panel is closed).
PadInput DecodePanelPad(const AtmtOverlayInput& pad);

}  // namespace atmt
