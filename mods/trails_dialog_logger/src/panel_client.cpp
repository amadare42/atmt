// panel_client.cpp - the dialog panel, as a window in the overlay mod's overlay.
//
// The overlay (mods/overlay, shared/overlay_api.h) owns the swapchain, ImGui, the mouse and the
// hooks that take input away from the game; this is only the panel: its hotkeys, what the pad and
// the keys do to it while it is open, its settings, and its drawing. Every frame the overlay calls
// PanelUpdate on the render thread - open or not, so F3 works while it is closed - and PanelDraw
// inside its ImGui frame when the panel asked to be drawn.
//
// The panel's settings are registered with the loader (AtmtModApi::settings_register): the settings
// bar shows them in the "Dialog log" menu and changes them live, and a live change made here (the
// right stick, Ctrl+Up) goes through the same registry, so the overlay saves both the same way -
// once, in place, when the last thing on screen closes.
#include "imgui.h"
#include "imgui_internal.h"   // ImTextInitClassifiers (see PanelDraw)

#include "mod_api.h"
#include "overlay_api.h"
#include "itf_font.h"
#include "pad.h"
#include "panel.h"
#include "panel_font.h"
#include "atmt.h"

#include <cstdio>
#include <cstring>
#include <vector>

namespace atmt {

namespace {

// ---------------------------------------------------------------- state
Config g_config;              // our own copy: the caller's Config is usually a local
LogSink* g_sink = nullptr;
History* g_history = nullptr;
const AtmtModApi* g_api = nullptr;
const AtmtOverlayApi* g_overlay = nullptr;
volatile LONG g_window_added = 0;
volatile LONG g_stop_finding = 0;
volatile LONG g_font_remove = 0;   // 1 = asked, 2 = done (RemovePanel waits for the update to do it)

bool g_open = false;
bool g_open_requested = false;   // the bar's "Open the dialog log", or F3 while the bar had the input
float g_alpha = 0.0f;            // the open/close fade
DWORD g_last_tick = 0;
DialogPanel g_panel;
PanelScroll g_scroll;            // decided in update, applied in draw
KeyEdge g_key;
std::vector<LogLine> g_lines;
uint64_t g_lines_seq = 0;        // the newest seq the snapshot was taken at

constexpr float kFadeMs = 120.0f;
Hold g_key_up, g_key_down, g_key_pgup, g_key_pgdn, g_key_home, g_key_end, g_key_close;
Hold g_key_font_up, g_key_font_down;
bool g_ctrl_down = false;

// ---------------------------------------------------------------- the panel's settings
const char kLoggerMenu[] = "Dialog log";
const char* const kAnchors[] = {"BottomLeft", "BottomRight", "TopLeft", "TopRight", "Center"};
bool g_registered = false;

int32_t g_set_font = 20;
int32_t g_set_width = 62;
int32_t g_set_height = 48;
int32_t g_set_anchor = 0;
int32_t g_set_opacity = 92;
int32_t g_set_dim = 30;
int32_t g_set_timestamps = 1;
int32_t g_set_game_font = 1;
int32_t g_set_newest_first = 0;
int32_t g_set_capture = 1;
int32_t g_set_lines = 500;
int32_t g_set_log_to_file = 0;
int32_t g_set_diagnostics = 0;
int32_t g_set_dev_reload = 0;
char g_set_key[32] = "";
char g_set_pad_toggle[32] = "";
char g_set_pad_close[32] = "";

void CopyText(char* out, size_t size, const std::string& text) {
    _snprintf(out, size, "%s", text.c_str());
    out[size - 1] = '\0';
}

// The registered values -> what the panel reads. On the render thread when the bar changes one;
// on the logger's thread once, right after registering (before the window exists).
void __cdecl OnPanelSetting(const AtmtSetting*, void*) {
    g_config.overlay_font_size = static_cast<unsigned>(g_set_font);
    g_config.overlay_width_pct = g_set_width;
    g_config.overlay_height_pct = g_set_height;
    if (g_set_anchor >= 0 && g_set_anchor < 5) g_config.overlay_anchor = kAnchors[g_set_anchor];
    g_config.overlay_opacity_pct = static_cast<unsigned>(g_set_opacity);
    g_config.overlay_dim_pct = static_cast<unsigned>(g_set_dim);
    g_config.overlay_timestamps = g_set_timestamps != 0;
    g_config.overlay_game_font = g_set_game_font != 0;
    g_config.overlay_newest_first = g_set_newest_first != 0;
    g_config.overlay_capture = g_set_capture != 0;
    // The key and the buttons take over at once. A key that is down right now (the one just picked)
    // is not a press of it.
    g_config.overlay_key = g_set_key;
    const unsigned key = KeyChordFromName(g_config.overlay_key);
    g_config.overlay_key_known = key != 0;
    g_config.overlay_key_chord = key != 0 ? key : kDefaultOverlayKey;
    if (g_key.key() != g_config.overlay_key_chord) {
        g_key.set_key(g_config.overlay_key_chord);
        g_key.Reset();
    }
    g_config.overlay_pad_toggle = g_set_pad_toggle;
    g_config.overlay_pad_close = g_set_pad_close;
    SetPadButtons(g_config.overlay_pad_close, g_config.overlay_pad_toggle);
}

void __cdecl OnOpenLog(const AtmtSetting*, void*) { g_open_requested = true; }

// The panel's keys live in [Overlay] and the logger's own in [General], as dist/atmt_config.ini
// documents them: LoadConfig ignores sections, the registry does not, and a key looked up in the
// wrong one would be added to the file a second time.
#define ATMT_LIVE(type, key, label, help, storage, lo, hi, step)                                     \
    {type, ATMT_SETTING_LIVE, "Overlay", key, label, help, storage, 0, lo, hi, step, nullptr, 0,       \
     &OnPanelSetting}
#define ATMT_TEXT(type, key, label, help, buffer)                                                   \
    {type, ATMT_SETTING_LIVE, "Overlay", key, label, help, buffer, sizeof(buffer), 0, 0, 0, nullptr,   \
     0, &OnPanelSetting}
#define ATMT_RESTART(type, flags, section, key, label, help, storage, lo, hi, step)                  \
    {type, ATMT_SETTING_RESTART | (flags), section, key, label, help, storage, 0, lo, hi, step}

const AtmtSetting kPanelSettings[] = {
    {ATMT_SETTING_ACTION, 0, nullptr, nullptr, "Open the dialog log",
     "the panel itself - also OverlayKey, or OverlayPadToggle on the pad", nullptr, 0, 0, 0, 0,
     nullptr, 0, &OnOpenLog},
    {ATMT_SETTING_LABEL, 0, nullptr, nullptr, "Panel"},
    ATMT_LIVE(ATMT_SETTING_INT, "OverlayFontSize", "Text size",
             "the panel's text size in pixels (also Ctrl+Up/Down or Y/X while it is open)",
             &g_set_font, 8.0f, static_cast<float>(kOverlayFontSizeMax), 1.0f),
    ATMT_LIVE(ATMT_SETTING_INT, "OverlayWidthPct", "Width",
             "percent of the game's window (also the right stick while the panel is open)",
             &g_set_width, 5.0f, 100.0f, 1.0f),
    ATMT_LIVE(ATMT_SETTING_INT, "OverlayHeightPct", "Height",
             "percent of the game's window (also the right stick while the panel is open)",
             &g_set_height, 5.0f, 100.0f, 1.0f),
    {ATMT_SETTING_ENUM, ATMT_SETTING_LIVE, "Overlay", "OverlayAnchor", "Position",
     "where the sheet sits on the screen", &g_set_anchor, 0, 0, 0, 0, kAnchors, 5, &OnPanelSetting},
    ATMT_LIVE(ATMT_SETTING_INT, "OverlayOpacityPct", "Opacity", "the sheet's background, percent",
             &g_set_opacity, 0.0f, 100.0f, 5.0f),
    ATMT_LIVE(ATMT_SETTING_INT, "OverlayDimPct", "Dim the game",
             "darken the game behind the open panel, percent (0 = not at all)", &g_set_dim, 0.0f,
             100.0f, 5.0f),
    ATMT_LIVE(ATMT_SETTING_BOOL, "OverlayTimestamps", "Timestamps", "show when each line was logged",
             &g_set_timestamps, 0.0f, 0.0f, 0.0f),
    ATMT_LIVE(ATMT_SETTING_BOOL, "OverlayGameFont", "Game font",
             "draw the log with the game's own font, symbols included (off = the overlay's font)",
             &g_set_game_font, 0.0f, 0.0f, 0.0f),
    ATMT_LIVE(ATMT_SETTING_BOOL, "OverlayNewestFirst", "Newest line first",
             "a tail (the newest line at the top) instead of a chronicle (newest at the bottom)",
             &g_set_newest_first, 0.0f, 0.0f, 0.0f),
    {ATMT_SETTING_LABEL, 0, nullptr, nullptr, "Controls"},
    ATMT_TEXT(ATMT_SETTING_KEY, "OverlayKey", "Log key", "the key (or chord, e.g. Ctrl+F3) that opens and closes the dialog log",
             g_set_key),
    ATMT_TEXT(ATMT_SETTING_PAD_BUTTON, "OverlayPadToggle", "Log pad button",
             "a pad button (or chord) that opens and closes the dialog log; none = keyboard only",
             g_set_pad_toggle),
    ATMT_TEXT(ATMT_SETTING_PAD_BUTTON, "OverlayPadClose", "Log close button",
             "the pad button that closes the dialog log", g_set_pad_close),
    ATMT_LIVE(ATMT_SETTING_BOOL, "OverlayCapture", "Hold the game's input",
             "while the panel is open the game gets no keys, buttons or mouse, so reading the log "
             "cannot advance the dialog",
             &g_set_capture, 0.0f, 0.0f, 0.0f),
    {ATMT_SETTING_LABEL, 0, nullptr, nullptr, "Logger"},
    ATMT_RESTART(ATMT_SETTING_BOOL, 0, "General", "LogToFile", "Write log files",
                "atmt_dialogs.jsonl and atmt_latest.txt in the game folder; off = memory only (the "
                "panel still shows every line)",
                &g_set_log_to_file, 0.0f, 0.0f, 0.0f),
    ATMT_RESTART(ATMT_SETTING_INT, 0, "Overlay", "OverlayLines", "Lines kept",
                "how far back the panel can scroll", &g_set_lines, 1.0f, 100000.0f, 50.0f),
    ATMT_RESTART(ATMT_SETTING_BOOL, ATMT_SETTING_ADVANCED, "General", "Diagnostics", "Diagnostics",
                "extra diagnostic lines in the log files", &g_set_diagnostics, 0.0f, 0.0f, 0.0f),
    ATMT_RESTART(ATMT_SETTING_BOOL, ATMT_SETTING_ADVANCED, "Overlay", "DevReload", "Dev reload",
                "development: hand a running game over to a freshly built dll (atmt_reload.txt)",
                &g_set_dev_reload, 0.0f, 0.0f, 0.0f),
};

#undef ATMT_LIVE
#undef ATMT_TEXT
#undef ATMT_RESTART

// Registers kPanelSettings, seeded with the values the mod started with: the registry writes a key
// the ini does not have yet from exactly these, so a fresh ini gets the running values.
void RegisterPanelSettings(LogSink& sink) {
    if (g_api == nullptr || g_api->settings_register == nullptr) return;
    g_set_font = static_cast<int32_t>(g_config.overlay_font_size);
    g_set_width = g_config.overlay_width_pct;
    g_set_height = g_config.overlay_height_pct;
    g_set_anchor = 0;
    for (int i = 0; i < 5; ++i) {
        if (_stricmp(g_config.overlay_anchor.c_str(), kAnchors[i]) == 0) g_set_anchor = i;
    }
    g_set_opacity = static_cast<int32_t>(g_config.overlay_opacity_pct);
    g_set_dim = static_cast<int32_t>(g_config.overlay_dim_pct);
    g_set_timestamps = g_config.overlay_timestamps ? 1 : 0;
    g_set_game_font = g_config.overlay_game_font ? 1 : 0;
    g_set_newest_first = g_config.overlay_newest_first ? 1 : 0;
    g_set_capture = g_config.overlay_capture ? 1 : 0;
    g_set_lines = static_cast<int32_t>(g_config.overlay_lines);
    g_set_log_to_file = g_config.log_to_file ? 1 : 0;
    g_set_diagnostics = g_config.diagnostics ? 1 : 0;
    g_set_dev_reload = g_config.dev_reload ? 1 : 0;
    CopyText(g_set_key, sizeof(g_set_key), g_config.overlay_key);
    CopyText(g_set_pad_toggle, sizeof(g_set_pad_toggle), g_config.overlay_pad_toggle);
    CopyText(g_set_pad_close, sizeof(g_set_pad_close), g_config.overlay_pad_close);
    const int n = g_api->settings_register(g_api, kLoggerMenu, kPanelSettings,
                                           sizeof(kPanelSettings) / sizeof(kPanelSettings[0]));
    if (n < 0) {
        sink.Note("panel: the loader refused the panel's settings - live changes are not saved");
        return;
    }
    g_registered = true;
    OnPanelSetting(nullptr, nullptr);
}

// A live change made outside the bar (the stick, Ctrl+Up): stored through the registry, so it is
// saved with the rest. Matched by the storage pointer (see settings_set).
void Persist(int32_t* storage, int32_t value) {
    if (!g_registered) {
        *storage = value;
        return;
    }
    AtmtSetting probe;
    memset(&probe, 0, sizeof(probe));
    probe.value = storage;
    g_api->settings_set(&probe, &value);
}

// The panel's text size while it is open: Ctrl+Up/Ctrl+Down, or Y/X on the pad. Clamped, not
// rejected: a held button's step grows (RepeatStep), and a step that would overshoot the limit must
// still land on it.
void ChangeFontSize(int delta) {
    int wanted = static_cast<int>(g_config.overlay_font_size) + delta;
    if (wanted < static_cast<int>(kOverlayFontSizeMin)) wanted = static_cast<int>(kOverlayFontSizeMin);
    if (wanted > static_cast<int>(kOverlayFontSizeMax)) wanted = static_cast<int>(kOverlayFontSizeMax);
    if (wanted == static_cast<int>(g_config.overlay_font_size)) return;
    g_config.overlay_font_size = static_cast<unsigned>(wanted);
    Persist(&g_set_font, wanted);
}

// The panel's size, live, from the right stick: width and height independently, 5..100 percent.
void ChangePanelSize(int dw, int dh) {
    auto clamp = [](int v) { return v < 5 ? 5 : (v > 100 ? 100 : v); };
    const int width = clamp(g_config.overlay_width_pct + dw);
    const int height = clamp(g_config.overlay_height_pct + dh);
    if (width != g_config.overlay_width_pct) {
        g_config.overlay_width_pct = width;
        Persist(&g_set_width, width);
    }
    if (height != g_config.overlay_height_pct) {
        g_config.overlay_height_pct = height;
        Persist(&g_set_height, height);
    }
}

void Open() {
    g_open = true;
    g_panel.FollowNewest();   // it always opens at the newest line (see panel.cpp's scroll block)
}

// ---------------------------------------------------------------- the window
// Every frame, on the render thread, before the overlay's ImGui frame.
uint32_t __cdecl PanelUpdate(const AtmtOverlayFrame* frame, void*) {
    // The game's font goes into (and out of) the overlay's atlas here, between ImGui frames - never
    // inside draw(), where the atlas is in use (panel_font.cpp).
    ImGui::SetCurrentContext(static_cast<ImGuiContext*>(frame->imgui_context));
    ImGui::SetAllocatorFunctions(frame->imgui_alloc, frame->imgui_free, frame->imgui_alloc_user);
    if (InterlockedCompareExchange(&g_font_remove, 0, 0) == 1) {
        RemovePanelFont();
        InterlockedExchange(&g_font_remove, 2);
        return 0;
    }
    if (g_config.overlay_game_font && PanelFont() == nullptr && AddPanelFont() != nullptr) {
        HostLog("panel: drawing with the game's font");
    }

    const bool input = (frame->flags & ATMT_OVERLAY_FRAME_INPUT) != 0;
    const bool hotkeys = (frame->flags & ATMT_OVERLAY_FRAME_HOTKEYS) != 0;
    uint32_t answer = 0;

    // Sampled every frame so the edges stay true; acted on only when hotkeys may act.
    const bool key = g_key.PressedOnce();
    const PadInput pad = DecodePanelPad(frame->input);
    if (hotkeys && (key || pad.toggle)) {
        if (g_open) g_open = false;
        else g_open_requested = true;
    }
    if ((frame->flags & ATMT_OVERLAY_FRAME_CLOSE) != 0) g_open = false;   // the bar has the input
    if (g_open_requested) {
        if (input) {
            g_open_requested = false;
            Open();
        } else {
            answer |= ATMT_OVERLAY_WANT_FOCUS;   // the overlay closes the bar; we open next frame
        }
    }

    g_scroll = PanelScroll();
    if (g_open && input) {
        if (pad.close || Pressed(g_key_close, KeyDown(VK_ESCAPE), false)) g_open = false;
        if (pad.accept || Pressed(g_key_end, KeyDown(VK_END), false)) g_scroll.to_bottom = true;
        if (Pressed(g_key_home, KeyDown(VK_HOME), false)) g_scroll.to_top = true;
        if (Pressed(g_key_pgup, KeyDown(VK_PRIOR), false)) --g_scroll.pages;
        if (Pressed(g_key_pgdn, KeyDown(VK_NEXT), false)) ++g_scroll.pages;
        // Up/Down scroll, Ctrl+Up/Ctrl+Down resize the text. When Ctrl goes up or down the holds are
        // dropped, so a key held through the change is not read as a fresh press by the other.
        const bool ctrl = KeyDown(VK_CONTROL);
        if (ctrl != g_ctrl_down) {
            g_ctrl_down = ctrl;
            g_key_up = Hold();
            g_key_down = Hold();
            g_key_font_up = Hold();
            g_key_font_down = Hold();
        }
        if (!ctrl) {
            if (Pressed(g_key_up, KeyDown(VK_UP), true)) g_scroll.lines -= HeldStep(g_key_up);
            if (Pressed(g_key_down, KeyDown(VK_DOWN), true)) g_scroll.lines += HeldStep(g_key_down);
        } else {
            if (Pressed(g_key_font_up, KeyDown(VK_UP), true)) ChangeFontSize(+1);
            if (Pressed(g_key_font_down, KeyDown(VK_DOWN), true)) ChangeFontSize(-1);
        }
        g_scroll.lines += pad.lines;
        g_scroll.pages += pad.pages;
        ChangePanelSize(pad.resize_width_pct, pad.resize_height_pct);
        if (pad.font_size_delta != 0) ChangeFontSize(pad.font_size_delta);
    }

    // The fade: a sheet that pops in and out over a dialog box is unpleasant to read.
    const DWORD tick = GetTickCount();
    const DWORD elapsed = g_last_tick == 0 ? 0 : tick - g_last_tick;
    g_last_tick = tick;
    const float step = static_cast<float>(elapsed) / kFadeMs;
    g_alpha += g_open ? step : -step;
    if (g_alpha > 1.0f) g_alpha = 1.0f;
    if (g_alpha < 0.0f) g_alpha = 0.0f;

    // Drawn while open or fading, and as a live preview while the bar shows this mod's menu - a size
    // or a position is judged on the sheet itself, not on a number.
    if (g_alpha > 0.0f || (frame->flags & ATMT_OVERLAY_FRAME_PREVIEW) != 0) answer |= ATMT_OVERLAY_WANT_DRAW;
    if (g_open) {
        answer |= ATMT_OVERLAY_WANT_INPUT;
        if (g_config.overlay_capture) answer |= ATMT_OVERLAY_HOLD_INPUT;
    }
    return answer;
}

// Inside the overlay's ImGui frame: into its context, with its allocator (overlay_api.h).
void __cdecl PanelDraw(const AtmtOverlayFrame* frame, void*) {
    ImGui::SetCurrentContext(static_cast<ImGuiContext*>(frame->imgui_context));
    ImGui::SetAllocatorFunctions(frame->imgui_alloc, frame->imgui_free, frame->imgui_alloc_user);
    // Word wrapping reads a separator table that is a static of *this dll's* ImGui, filled only when
    // a font atlas is built - and the atlas is the overlay's, built in its own copy. Left empty, every
    // character counted as a blank and TextWrapped broke lines mid-word ("motiv" / "ated."). It
    // returns at once after the first call.
    ImTextInitClassifiers();

    const bool preview = (frame->flags & ATMT_OVERLAY_FRAME_PREVIEW) != 0;
    const float alpha = preview && frame->bar_alpha > g_alpha ? frame->bar_alpha : g_alpha;

    // Dimming the game behind an open sheet: reading a log over a dialog box needs the contrast.
    if (g_config.overlay_dim_pct > 0 && g_alpha > 0.0f) {
        const ImGuiViewport* viewport = ImGui::GetMainViewport();
        ImGui::GetBackgroundDrawList()->AddRectFilled(
            viewport->Pos,
            ImVec2(viewport->Pos.x + viewport->Size.x, viewport->Pos.y + viewport->Size.y),
            IM_COL32(0, 0, 0,
                     static_cast<int>(255.0f * static_cast<float>(g_config.overlay_dim_pct) / 100.0f
                                      * g_alpha)));
    }
    // The lines are copied only when the log has moved on.
    if (g_history != nullptr) {
        const uint64_t newest = g_history->newest_seq();
        if (newest != g_lines_seq || g_lines.empty()) {
            g_history->Snapshot(g_lines);
            g_lines_seq = newest;
        }
    }
    // The panel's own text size: the overlay's font, at OverlayFontSize (ImGui 1.92 rasterizes
    // whatever size is pushed, so a live change is instant).
    ImGui::PushFont(g_config.overlay_game_font ? PanelFont() : nullptr,
                    static_cast<float>(g_config.overlay_font_size));
    PanelView view;
    view.lines = &g_lines;
    view.config = &g_config;
    view.alpha = alpha;
    view.capture = g_config.overlay_capture && g_open;
    g_panel.Draw(view, g_scroll);
    ImGui::PopFont();
}

// The game's font, from the game's own memory (itf_font.h): it is loaded when the game sets up its
// UI, which may be after this mod starts, so this looks again for a while. Failing that, the loose
// file - which an HD pack or SenPatcher may have replaced in an archive instead, hence second.
DWORD WINAPI FindFontThread(LPVOID) {
    std::string where;
    for (int i = 0; i < 30 && InterlockedCompareExchange(&g_stop_finding, 0, 0) == 0; ++i) {
        ItfFont font;
        if (FindGameFontInMemory(&font, &where)) {
            OfferPanelFont(static_cast<ItfFont&&>(font));
            HostLog(("panel: " + where).c_str());
            return 0;
        }
        Sleep(2000);
    }
    HostLog(("panel: " + where + " - trying data/fonts").c_str());
    const wchar_t* const files[] = {L"\\data\\fonts\\font_us_hd.itf", L"\\data\\fonts\\font_us.itf"};
    for (const wchar_t* file : files) {
        ItfFont font;
        if (LoadItfFile(GetGameDir() + file, &font)) {
            OfferPanelFont(static_cast<ItfFont&&>(font));
            HostLog("panel: the game's font from data/fonts (the game's own copy was not found)");
            return 0;
        }
    }
    HostLog("panel: no game font - the log uses the overlay's");
    return 0;
}

// The overlay mod may load before or after this one: look for its service until it is there.
DWORD WINAPI FindOverlayThread(LPVOID) {
    for (int i = 0; i < 300 && InterlockedCompareExchange(&g_stop_finding, 0, 0) == 0; ++i) {
        const void* found = g_api->service_find(ATMT_OVERLAY_SERVICE);
        if (found != nullptr) {
            const AtmtOverlayApi* overlay = static_cast<const AtmtOverlayApi*>(found);
            if (overlay->version < ATMT_OVERLAY_API_VERSION || overlay->size < sizeof(AtmtOverlayApi)) {
                HostLogError("panel: the overlay mod is older than this one - no panel");
                return 0;
            }
            AtmtOverlayWindow window;
            memset(&window, 0, sizeof(window));
            window.name = kLoggerMenu;
            window.imgui_version = IMGUI_VERSION_NUM;
            window.update = &PanelUpdate;
            window.draw = &PanelDraw;
            if (overlay->add_window(g_api, &window) != 0) {
                HostLogError("panel: the overlay refused the panel (see the line above)");
                return 0;
            }
            g_overlay = overlay;
            InterlockedExchange(&g_window_added, 1);
            char note[160];
            _snprintf(note, sizeof(note), "panel: ready in the overlay - %s opens it",
                      g_config.overlay_key.c_str());
            HostLog(note);
            return 0;
        }
        Sleep(100);
    }
    HostLogError("panel: no overlay mod (atmt_overlay.dll) in this process - no panel");
    return 0;
}

}  // namespace

// ---------------------------------------------------------------- the mod's entry points
bool InstallPanel(const Config& config, LogSink& sink, History& history, const AtmtModApi* api) {
    g_config = config;
    g_sink = &sink;
    g_history = &history;
    g_api = api;
    g_key.set_key(config.overlay_key_chord);
    g_key.Reset();
    SetPadButtons(config.overlay_pad_close, config.overlay_pad_toggle);
    if (api == nullptr) return false;
    // Registered even with Overlay=false: the ini still lists every setting, and the bar can turn
    // the rest on for the next start.
    RegisterPanelSettings(sink);
    if (!config.overlay) {
        sink.Note("panel: off in the config (Overlay=false)");
        return true;
    }
    if (!config.overlay_key_known) {
        sink.Note("panel: OverlayKey=" + config.overlay_key + " is not a key (or chord) this mod knows - using F3");
    }
    if (api->service_find == nullptr) return false;
    HANDLE thread = CreateThread(nullptr, 0, FindOverlayThread, nullptr, 0, nullptr);
    if (thread == nullptr) return false;
    CloseHandle(thread);
    if (HANDLE font = CreateThread(nullptr, 0, FindFontThread, nullptr, 0, nullptr)) CloseHandle(font);
    return true;
}

void RemovePanel() {
    InterlockedExchange(&g_stop_finding, 1);
    if (InterlockedCompareExchange(&g_window_added, 0, 0) != 0 && PanelFont() != nullptr) {
        // The font leaves the atlas on the render thread, in the next update - the atlas would
        // otherwise keep calling into this dll after it is gone.
        InterlockedExchange(&g_font_remove, 1);
        for (int i = 0; i < 100 && InterlockedCompareExchange(&g_font_remove, 0, 0) != 2; ++i) Sleep(10);
    }
    if (InterlockedExchange(&g_window_added, 0) != 0 && g_overlay != nullptr) {
        g_overlay->remove_window(g_api);   // waits for a frame in progress
    }
    if (g_registered) {
        g_registered = false;
        g_api->settings_unregister(g_api);   // saves what is unsaved, while this dll is loaded
    }
}

}  // namespace atmt
