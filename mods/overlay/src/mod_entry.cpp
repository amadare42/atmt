// mod_entry.cpp - the overlay mod: its entry points, its own settings, and its log.
#include "overlay.h"

#include "atmt_input.h"

#include <cstdarg>
#include <cstdio>
#include <cstring>

namespace atmt_overlay {

namespace {

const AtmtModApi* g_api = nullptr;
volatile LONG g_started = 0;
Options g_opts;

// ---------------------------------------------------------------- the "Overlay" menu
// Registered with the loader (AtmtModApi::settings_register): it loads them from atmt_overlay.ini,
// adds what the ini lacks, and the settings bar - which this mod draws - changes them.
char g_key[32] = "F2";
char g_chord[32] = "L3+R3";
int32_t g_font_size = 20;
int32_t g_mouse = 1;
int32_t g_cursor = 1;
char g_font_path[260] = "";

void __cdecl OnSetting(const AtmtSetting*, void*) {
    g_opts.settings_key_name = g_key;
    g_opts.settings_key = atmt::KeyChordFromName(g_opts.settings_key_name);   // 0: the pad chord only
    g_opts.settings_chord_name = g_chord;
    g_opts.settings_chord = atmt::PadChordFromName(g_opts.settings_chord_name);
    g_opts.font_size = g_font_size;
    g_opts.mouse = g_mouse != 0;
    g_opts.cursor = g_cursor != 0;
    g_opts.font_path = g_font_path;
}

const AtmtSetting kSettings[] = {
    {ATMT_SETTING_LABEL, 0, nullptr, nullptr, "Settings bar"},
    {ATMT_SETTING_KEY, ATMT_SETTING_LIVE, "General", "SettingsKey", "Open key",
     "the key (or chord, e.g. Ctrl+F2) that opens and closes this settings bar (none = the pad chord only)", g_key,
     sizeof(g_key), 0, 0, 0, nullptr, 0, &OnSetting},
    {ATMT_SETTING_PAD_BUTTON, ATMT_SETTING_LIVE, "General", "SettingsPadToggle", "Open pad chord",
     "the pad chord that opens and closes this settings bar (e.g. L3+R3; none = the key only)",
     g_chord, sizeof(g_chord), 0, 0, 0, nullptr, 0, &OnSetting},
    {ATMT_SETTING_INT, ATMT_SETTING_LIVE, "General", "FontSize", "Text size",
     "the bar's text size in pixels (a window like the dialog log has its own)", &g_font_size, 0,
     8.0f, 64.0f, 1.0f, nullptr, 0, &OnSetting},
    {ATMT_SETTING_LABEL, 0, nullptr, nullptr, "Mouse"},
    {ATMT_SETTING_BOOL, ATMT_SETTING_RESTART, "General", "Mouse", "Mouse",
     "use the mouse in the overlay: click, scroll, and a free cursor while anything is open (the "
     "game's own mouse camera is held meanwhile)",
     &g_mouse, 0, 0, 0, 0, nullptr, 0, &OnSetting},
    {ATMT_SETTING_BOOL, ATMT_SETTING_LIVE, "General", "ShowCursor", "Show a cursor",
     "draw a pointer while anything is open (the game hides the system's own)", &g_cursor, 0, 0, 0,
     0, nullptr, 0, &OnSetting},
    {ATMT_SETTING_STRING, ATMT_SETTING_RESTART | ATMT_SETTING_ADVANCED, "General", "FontPath",
     "Font file",
     "a .ttf/.otf for everything the overlay draws (a CJK face for a Japanese or Chinese dialog "
     "log); none = ImGui's own",
     g_font_path, sizeof(g_font_path)},
};

}  // namespace

const AtmtModApi* Api() { return g_api; }

const Options& Opts() { return g_opts; }

void Log(const char* format, ...) {
    if (g_api == nullptr || g_api->log == nullptr) return;
    char buf[512];
    va_list args;
    va_start(args, format);
    _vsnprintf(buf, sizeof(buf) - 1, format, args);
    va_end(args);
    buf[sizeof(buf) - 1] = '\0';
    g_api->log(buf);
}

// A failure: written to atmt_loader.log unless LogLevel=off (Log is written with LogLevel=all only).
void LogError(const char* format, ...) {
    if (g_api == nullptr || g_api->log_error == nullptr) return;
    char buf[512];
    va_list args;
    va_start(args, format);
    _vsnprintf(buf, sizeof(buf) - 1, format, args);
    va_end(args);
    buf[sizeof(buf) - 1] = '\0';
    g_api->log_error(buf);
}

bool IsExecutableCode(const void* p) {
    if (p == nullptr) return false;
    MEMORY_BASIC_INFORMATION mbi;
    if (VirtualQuery(p, &mbi, sizeof(mbi)) == 0) return false;
    if (mbi.State != MEM_COMMIT || mbi.Type != MEM_IMAGE) return false;
    const DWORD kExecutable = PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE
                              | PAGE_EXECUTE_WRITECOPY;
    return (mbi.Protect & kExecutable) != 0;
}

}  // namespace atmt_overlay

using namespace atmt_overlay;

extern "C" {

__declspec(dllexport) uint32_t __cdecl AtmtModInit(const AtmtModApi* api) {
    if (api == nullptr || api->version < ATMT_MOD_API_VERSION || api->size < sizeof(AtmtModApi)) {
        return 0;
    }
    if (InterlockedCompareExchange(&g_started, 1, 0) != 0) {
        if (api->log != nullptr) api->log("overlay: already initialised in this process, declining");
        return 0;
    }
    g_api = api;

    api->settings_register(api, "Overlay", kSettings, sizeof(kSettings) / sizeof(kSettings[0]));
    OnSetting(nullptr, nullptr);
    if (g_opts.settings_key == 0 && !g_opts.settings_key_name.empty()) {
        LogError("overlay: SettingsKey=%s is not a key (or chord) this mod knows - the bar opens with the pad chord",
            g_opts.settings_key_name.c_str());
    }
    if (g_opts.settings_chord == 0 && !g_opts.settings_chord_name.empty()) {
        LogError("overlay: SettingsPadToggle=%s is not a pad chord this mod knows - the bar opens with "
            "the key", g_opts.settings_chord_name.c_str());
    }

    // The input paths. Each is optional: without one, the overlay still draws, the game simply also
    // sees what is pressed while it is open. The DirectInput hooks are not among them: they are
    // installed on the first Present (host.cpp), after the game has enumerated its own devices.
    std::string why;
    if (!InstallPad(&why)) LogError("overlay: no pad - %s", why.c_str());
    if (!InstallMouseButtonHook(&why)) LogError("overlay: the game keeps seeing mouse clicks - %s", why.c_str());
    if (g_opts.mouse && !InstallCursorHooks(&why)) {
        LogError("overlay: the game keeps its cursor while the overlay is open - %s", why.c_str());
    }

    // Straight away, not on a thread of our own: the device hook has to be in before the game
    // creates its device, and the loader runs this while the game is starting up.
    if (!InstallHost()) LogError("overlay: no overlay in this process (see the lines above)");

    api->service_publish(api, ATMT_OVERLAY_SERVICE, Service());
    Log("overlay: ready - %s%s%s opens the settings bar", g_opts.settings_key_name.c_str(),
        g_opts.settings_key_name.empty() || g_opts.settings_chord_name.empty() ? "" : " / ",
        g_opts.settings_chord_name.c_str());
    return ATMT_MOD_API_VERSION;
}

__declspec(dllexport) void __cdecl AtmtModShutdown(void) { ShutdownHost(); }

// One sentence for the overlay and the manager (shared/mod_api.h, AtmtModDescription).
__declspec(dllexport) const char* __cdecl AtmtModDescription(void) {
    return "The in-game settings bar (F2 / L3+R3) that every mod's settings appear in; the "
           "dialog log panel draws in it too.";
}

}  // extern "C"
