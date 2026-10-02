// panel.h - the dialog log as a panel: the drawing, and the state the input moves.
//
// The panel owns one thing the overlay does not: where it is scrolled to, and whether it is still
// following the newest line. Everything else (when it is visible, what the fade is at, which input
// is being swallowed) belongs to the overlay, so the drawing code never has to know whether a
// scroll came from the pad, the keyboard or the mouse wheel.
#pragma once

#include <vector>

#include "history.h"
#include "atmt.h"

namespace atmt {

// What one frame asks the panel to do, already edge-detected and repeated.
struct PanelScroll {
    int lines = 0;           // <0 one line up, >0 one line down
    int pages = 0;           // <0 a screen up, >0 a screen down
    bool to_top = false;
    bool to_bottom = false;  // also starts following the newest line again
};

// What the panel draws from.
struct PanelView {
    const std::vector<LogLine>* lines = nullptr;   // oldest first
    const Config* config = nullptr;
    float alpha = 1.0f;      // 0..1: the open/close fade
    bool capture = true;     // the game's own input is being swallowed right now
};

class DialogPanel {
public:
    // One frame. Only called while the panel is on screen (the overlay skips the whole frame
    // otherwise, which is what keeps a closed panel free).
    void Draw(const PanelView& view, const PanelScroll& scroll);
    bool following() const { return follow_; }
    // Called when the panel is opened: it starts at the newest line, whatever the player was reading
    // when it was last closed (see the scroll block in panel.cpp).
    void FollowNewest() { follow_ = true; }

private:
    bool follow_ = true;
};

}  // namespace atmt
