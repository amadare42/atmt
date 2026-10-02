// config.cpp - reads the mod's ini file.
#include "atmt.h"
#include "ini.h"

#include <cstdlib>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

namespace atmt {

static bool IniBool(const char* v, bool def) {
    if (v == nullptr) return def;
    if (_stricmp(v, "true") == 0 || _stricmp(v, "1") == 0 || _stricmp(v, "yes") == 0) return true;
    if (_stricmp(v, "false") == 0 || _stricmp(v, "0") == 0 || _stricmp(v, "no") == 0) return false;
    return def;
}

static unsigned IniUint(const char* v, unsigned def) {
    if (v == nullptr || *v == '\0') return def;
    return static_cast<unsigned>(strtoul(v, nullptr, 0));
}

// A percentage or a line count from the ini is clamped instead of trusted: a value out of range
// would be an invisible panel or a pointless allocation, not a typo the user would notice.
static unsigned ClampUint(unsigned v, unsigned lo, unsigned hi) {
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

// The argument is the mod's own ini (the loader guarantees <mod_dir>\trails_dialog_logger.ini
// exists - see AtmtModApi::config_path). This mod has one config file, not several: it used to also
// accept a legacy <game>\atmt_config.ini, layered on top of this one key by key, but that existed
// only for players upgrading from a pre-loader version of the project - a population this project
// does not have. (Treating the path as a directory once made every setting silently ignored - the
// self test covers it now.)
namespace {

// The section each key lives in. The same file is also read by the loader's settings registry
// (panel_client.cpp registers the keys with these sections, through shared/ini.h), which only looks
// in a key's own section; this reader follows the same rules - sections, and shared/ini.h's inline
// comments - so the two can never read different values from one file. NULL: not a key of this mod.
const char* HomeSection(const char* key) {
    static const char* const kGeneral[] = {"Enabled",           "LogToFile",       "Diagnostics", "MessageHook",
                                           "MessagePlateOffset", "MessageTextSkip", "PendingSpeakerMs"};
    static const char* const kOverlay[] = {
        "Overlay",         "OverlayKey",      "OverlayLines",      "OverlayNewestFirst", "OverlayCapture",
        "OverlayTimestamps", "OverlayGameFont", "OverlayAnchor",   "OverlayWidthPct",    "OverlayHeightPct",
        "OverlayOpacityPct", "OverlayDimPct", "OverlayFontSize",   "OverlayPadClose",    "OverlayPadToggle",
        "DevReload"};
    for (const char* k : kGeneral) {
        if (_stricmp(key, k) == 0) return "General";
    }
    for (const char* k : kOverlay) {
        if (_stricmp(key, k) == 0) return "Overlay";
    }
    return nullptr;
}

bool IsMovedKey(const char* key) {
    return _stricmp(key, "OverlayMouse") == 0 || _stricmp(key, "OverlayFontPath") == 0
           || _stricmp(key, "SettingsKey") == 0 || _stricmp(key, "SettingsPadToggle") == 0;
}

void AddName(std::string* list, const std::string& name) {
    if (!list->empty()) *list += ", ";
    *list += name;
}

}  // namespace

Config LoadConfig(const std::wstring& config_file) {
    Config c;
    FILE* f = _wfopen(config_file.c_str(), L"rb");
    if (f == nullptr) return c;   // built-in defaults
    char line[512];
    std::string section;   // "" before the first [section]
    while (fgets(line, sizeof(line), f)) {
        char* p = line;
        while (*p == ' ' || *p == '\t') ++p;
        if (*p == ';' || *p == '#' || *p == '\0' || *p == '\r' || *p == '\n') continue;
        if (*p == '[') {
            char* end = strchr(p, ']');
            if (end != nullptr) section.assign(p + 1, end);
            continue;
        }
        char* eq = strchr(p, '=');
        if (eq == nullptr) continue;
        *eq = '\0';
        char* key = p;
        char* val = eq + 1;
        while (*val == ' ' || *val == '\t') ++val;
        size_t len = strlen(key);
        while (len > 0 && (key[len - 1] == ' ' || key[len - 1] == '\t')) key[--len] = '\0';
        // An inline comment ("...=5   ; why") ends the value, as shared/ini.h reads it: a ';' after
        // a space or a tab.
        if (char* comment = atmt_ini::InlineComment(val)) *comment = '\0';
        len = strlen(val);
        while (len > 0
               && (val[len - 1] == '\r' || val[len - 1] == '\n' || val[len - 1] == ' ' || val[len - 1] == '\t')) {
            val[--len] = '\0';
        }

        // The overlay mod's now (atmt_overlay.ini: Mouse, FontPath, SettingsKey, SettingsPadToggle).
        if (IsMovedKey(key)) {
            AddName(&c.moved_keys, key);
            continue;
        }
        const char* home = HomeSection(key);
        if (home == nullptr) {
            AddName(&c.unknown_keys, key);
            continue;
        }
        // A known key outside its section is not read: the settings registry would not see it
        // either, and would add the key to its own section a second time.
        if (_stricmp(section.c_str(), home) != 0) {
            AddName(&c.misplaced_keys, std::string(key) + " (in "
                                           + (section.empty() ? std::string("no section") : "[" + section + "]")
                                           + ", belongs in [" + home + "])");
            continue;
        }

        if (_stricmp(key, "Enabled") == 0) c.enabled = IniBool(val, c.enabled);
        else if (_stricmp(key, "LogToFile") == 0) c.log_to_file = IniBool(val, c.log_to_file);
        else if (_stricmp(key, "Diagnostics") == 0) c.diagnostics = IniBool(val, c.diagnostics);
        // The one source: the game's own message setter.
        else if (_stricmp(key, "MessageHook") == 0) {
            c.message_hook_address = static_cast<uintptr_t>(strtoul(val, nullptr, 0));
        }
        else if (_stricmp(key, "MessagePlateOffset") == 0) {
            c.message_plate_offset = IniUint(val, c.message_plate_offset);
        }
        else if (_stricmp(key, "MessageTextSkip") == 0) {
            c.message_text_skip = IniUint(val, c.message_text_skip);
        }
        else if (_stricmp(key, "PendingSpeakerMs") == 0) {
            c.pending_speaker_ms = IniUint(val, c.pending_speaker_ms);
        }
        // ---------------------------------------------------------------- the overlay panel
        else if (_stricmp(key, "Overlay") == 0) c.overlay = IniBool(val, c.overlay);
        else if (_stricmp(key, "OverlayKey") == 0) {
            c.overlay_key = val;
            // Resolved here, once, so an unknown name is caught at startup instead of leaving a
            // panel that never opens: the name is reported and F3 is used.
            const unsigned vk = KeyChordFromName(c.overlay_key);
            c.overlay_key_known = (vk != 0);
            c.overlay_key_chord = c.overlay_key_known ? vk : kDefaultOverlayKey;
        }
        else if (_stricmp(key, "OverlayLines") == 0) {
            c.overlay_lines = ClampUint(IniUint(val, c.overlay_lines), 1, 100000);
        }
        else if (_stricmp(key, "OverlayNewestFirst") == 0) {
            c.overlay_newest_first = IniBool(val, c.overlay_newest_first);
        }
        else if (_stricmp(key, "OverlayCapture") == 0) c.overlay_capture = IniBool(val, c.overlay_capture);
        else if (_stricmp(key, "OverlayTimestamps") == 0) {
            c.overlay_timestamps = IniBool(val, c.overlay_timestamps);
        }
        else if (_stricmp(key, "OverlayGameFont") == 0) {
            c.overlay_game_font = IniBool(val, c.overlay_game_font);
        }
        else if (_stricmp(key, "OverlayAnchor") == 0) c.overlay_anchor = val;
        else if (_stricmp(key, "OverlayWidthPct") == 0) {
            c.overlay_width_pct = static_cast<int>(ClampUint(IniUint(val, 0), 5, 100));
        }
        else if (_stricmp(key, "OverlayHeightPct") == 0) {
            c.overlay_height_pct = static_cast<int>(ClampUint(IniUint(val, 0), 5, 100));
        }
        else if (_stricmp(key, "OverlayOpacityPct") == 0) {
            c.overlay_opacity_pct = ClampUint(IniUint(val, c.overlay_opacity_pct), 0, 100);
        }
        else if (_stricmp(key, "OverlayDimPct") == 0) {
            c.overlay_dim_pct = ClampUint(IniUint(val, c.overlay_dim_pct), 0, 100);
        }
        else if (_stricmp(key, "OverlayFontSize") == 0) {
            c.overlay_font_size = ClampUint(IniUint(val, c.overlay_font_size), kOverlayFontSizeMin,
                                            kOverlayFontSizeMax);
        }
        else if (_stricmp(key, "OverlayPadClose") == 0) c.overlay_pad_close = val;
        else if (_stricmp(key, "OverlayPadToggle") == 0) c.overlay_pad_toggle = val;
        // Development only (see docs/OVERLAY.md); off unless asked for.
        else if (_stricmp(key, "DevReload") == 0) c.dev_reload = IniBool(val, c.dev_reload);
    }
    fclose(f);
    return c;
}

}  // namespace atmt
