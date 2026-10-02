// settings_bar.h - the top bar: every mod's settings, in one place, over the game.
//
// A strip across the top of the screen with one menu per mod that registered settings with the
// loader (AtmtModApi::settings_register), and the open menu's settings below it. It draws nothing of
// its own knowledge: the menus, the rows, their types and ranges all come from the loader's
// registry, and a change goes back through settings_set, so the mod that owns the value sees it
// (its on_change) and the loader saves it (settings_commit, when the overlay closes the bar).
//
// Built for the pad first (the game is played on one, the Steam Deck included): LB/RB switch
// menus, up/down pick a row, left/right change the value, A toggles/cycles/runs/picks, B closes.
// The keyboard has the same map (Tab/Shift+Tab, arrows, Enter/Space, Esc), and the mouse can click
// (Mouse=true in atmt_overlay.ini). The input is decoded by the host (host.cpp) - this only acts on it.
//
// Each menu opens with the mod's own one-sentence description (AtmtSettingsGroup::description). When
// the menus do not fit across the screen the strip scrolls sideways, keeping the one showing in view;
// arrows at its ends show (and, clicked, scroll to) the menus beyond them.
#pragma once

#include <string>
#include <vector>

#include "mod_api.h"

namespace atmt_overlay {

// One frame of input, already edge-detected and repeated.
struct BarInput {
    int rows = 0;       // <0 up, >0 down
    int adjust = 0;     // <0 left, >0 right (accelerated: may be more than one step)
    int menus = 0;      // <0 previous menu, >0 next
    bool accept = false;
    bool back = false;
    unsigned short pad_buttons = 0;   // the raw pad, for picking a pad button (PAD_BUTTON rows)
};

class SettingsBar {
public:
    // Called when the bar opens: a pick in progress is dropped, the menu stays where it was.
    void Open();
    // One frame: acts on `in`, then draws. Returns false when the player asked to close the bar.
    bool Draw(const AtmtModApi* api, const BarInput& in, float alpha);
    // A key or a pad button is being picked: the overlay must not read the toggle keys meanwhile
    // (the key being pressed is the one being picked) and passes no navigation.
    bool picking() const { return pick_.active; }
    // The menu showing: its title, and the mod that registered it ("" before the first frame) -
    // the host tells that mod's window to draw a preview.
    const std::string& current_title() const { return current_title_; }
    const std::string& current_mod() const { return current_mod_; }
    // The right-hand hint on the bar: what opens/closes it.
    void set_hint(const std::string& hint) { hint_ = hint; }

private:
    struct Pick {
        bool active = false;
        bool pad = false;       // a pad chord, not a key
        bool armed = false;     // everything was released once since the pick started
        unsigned short held = 0;
        unsigned long start = 0;
        std::string key;        // the setting's ini key, in the current menu
    };

    void Change(const AtmtModApi* api, const AtmtSetting& s, int adjust, bool accept);
    void StepPick(const AtmtModApi* api, const AtmtSetting* s, const BarInput& in);

    std::string tab_id_;        // mod name + title of the menu showing, to find it again
    std::string current_title_;
    std::string current_mod_;
    std::string hint_;
    int row_ = 0;
    bool show_advanced_ = false;
    bool scroll_to_row_ = false;
    bool restart_pending_ = false;   // a setting that only applies after a restart was changed
    Pick pick_;
    std::vector<float> tab_x_;
    float menu_width_ = 0.0f;

    // The menus scroll sideways when they do not fit the bar (many mods, a small screen, a big
    // TextScale): in px from the first menu's left edge. The one showing is scrolled into view
    // whenever it changes; the wheel, a drag or the edge arrows scroll freely in between.
    float tabs_scroll_ = 0.0f;
    float tabs_target_ = 0.0f;       // where tabs_scroll_ is gliding to
    std::string tabs_followed_;      // the tab_id_ last scrolled into view
    bool tabs_snap_ = true;          // jump there (the bar just opened) rather than glide
    bool tabs_dragging_ = false;
};

}  // namespace atmt_overlay
