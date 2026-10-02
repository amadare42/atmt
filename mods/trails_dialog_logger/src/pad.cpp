// pad.cpp - the dialog panel's reading of the pad (see pad.h).
#include "pad.h"

namespace atmt {

namespace {

unsigned short g_close_button = kPadB;
unsigned short g_toggle_button = 0;

Hold g_up, g_down, g_left, g_right, g_page_up, g_page_down, g_accept, g_close, g_toggle;
Hold g_resize_taller, g_resize_shorter, g_resize_wider, g_resize_narrower;
Hold g_font_bigger, g_font_smaller;

}  // namespace

void SetPadButtons(const std::string& close_button, const std::string& toggle_button) {
    const unsigned short close = PadChordFromName(close_button);
    g_close_button = close != 0 ? close : kPadB;   // "B closes it" is the documented default
    g_toggle_button = PadChordFromName(toggle_button);
}

PadInput DecodePanelPad(const AtmtOverlayInput& pad) {
    PadInput in;
    const bool have = pad.have_pad != 0;
    const unsigned short buttons = have ? pad.pad_buttons : 0;
    // The d-pad, or the left stick pushed past the deadzone (up is positive in XINPUT_GAMEPAD).
    const bool up = (buttons & kPadUp) != 0 || (have && pad.thumb_ly > kStickDeadzone);
    const bool down = (buttons & kPadDown) != 0 || (have && pad.thumb_ly < -kStickDeadzone);

    if (Pressed(g_up, up, true)) in.lines -= HeldStep(g_up);
    if (Pressed(g_down, down, true)) in.lines += HeldStep(g_down);
    if (Pressed(g_left, (buttons & kPadLeft) != 0, true)) --in.pages;
    if (Pressed(g_right, (buttons & kPadRight) != 0, true)) ++in.pages;
    if (Pressed(g_page_up, (buttons & kPadLB) != 0, false)) --in.pages;
    if (Pressed(g_page_down, (buttons & kPadRB) != 0, false)) ++in.pages;
    in.accept = Pressed(g_accept, (buttons & kPadA) != 0, false);
    in.close = Pressed(g_close, (buttons & g_close_button) == g_close_button, false);
    in.toggle = g_toggle_button != 0
                && Pressed(g_toggle, (buttons & g_toggle_button) == g_toggle_button, false);

    // The right stick resizes the panel while it is open: up/right grow it, the way dragging a
    // window's corner away from itself would - held longer, faster.
    if (Pressed(g_resize_taller, have && pad.thumb_ry > kStickDeadzone, true)) {
        in.resize_height_pct += HeldStep(g_resize_taller);
    }
    if (Pressed(g_resize_shorter, have && pad.thumb_ry < -kStickDeadzone, true)) {
        in.resize_height_pct -= HeldStep(g_resize_shorter);
    }
    if (Pressed(g_resize_wider, have && pad.thumb_rx > kStickDeadzone, true)) {
        in.resize_width_pct += HeldStep(g_resize_wider);
    }
    if (Pressed(g_resize_narrower, have && pad.thumb_rx < -kStickDeadzone, true)) {
        in.resize_width_pct -= HeldStep(g_resize_narrower);
    }

    // Y/X: the pad's Ctrl+Up/Ctrl+Down, the text size - free buttons (nothing else here uses them).
    if (Pressed(g_font_bigger, (buttons & kPadY) != 0, true)) in.font_size_delta += HeldStep(g_font_bigger);
    if (Pressed(g_font_smaller, (buttons & kPadX) != 0, true)) {
        in.font_size_delta -= HeldStep(g_font_smaller);
    }
    return in;
}

}  // namespace atmt
