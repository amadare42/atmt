// settings.h - settings_model: the mods' inis, as the schema describes them (docs/MANAGER.md).
//
// Every value is read with shared/ini.h and written with atmt_ini::WriteValues, the same code the
// loader and the mods themselves use: comments and unrelated lines survive, and a key the ini does
// not have yet is added with its help text above it. Keys an ini has that the schema does not know
// are kept as "raw" entries, so nothing is uneditable.
//
// Concurrency with the game: each ini's contents (a hash) are remembered when it is read, and a save
// first checks them - an ini a mod in the game (or a person) changed meanwhile is a conflict, never silently overwritten.
// While ed8.exe runs, changes can be queued (atmt_manager_pending.json in the game folder) and are
// written once it has exited.
#pragma once

#include <map>
#include <string>
#include <vector>

#include "json.h"
#include "payload.h"
#include "util.h"

namespace atmt {

struct SettingDef {
    std::string type;    // bool int float enum string key pad_button action label
    bool live = false, restart = false, advanced = false, readonly = false;
    std::string section, key, label, help, def;
    bool ranged = false;
    double min = 0, max = 0, step = 0;
    std::vector<std::string> choices;
    int max_length = 0;

    bool stored() const { return !key.empty(); }   // everything but action and label
};

struct SettingsGroup {
    std::string mod;     // "my_mod"; "atmt_loader" for the loader's own ini
    std::string title;
    std::string ini;     // relative to the game folder, '/' separated
    std::vector<SettingDef> settings;
};

struct Schema {
    std::vector<SettingsGroup> groups;
    // The names a key / pad_button setting accepts (the mods' own parser, via the schema dump); a
    // pad chord joins buttons with `pad_chord_separator`.
    // A key setting is a key chord: `input_key_modifiers` (in the order they are written, "Ctrl",
    // "Shift", "Alt", "Win") and one of `input_keys`, joined by `key_chord_separator` ("Ctrl+F3").
    std::vector<std::string> input_keys;
    std::vector<std::string> input_key_modifiers;
    std::string key_chord_separator = "+";
    std::vector<std::string> input_pad_buttons;
    std::string pad_chord_separator = "+";
    // The value a recorded press is saved as, in the schema's own spelling: `mods` (any order and
    // case; unknown ones dropped) and `key` -> "Ctrl+F3"; empty when `key` is not one of input_keys.
    std::string KeyChordValue(const std::vector<std::string>& mods, const std::string& key) const;
    // Pad buttons held together -> "A+LB" (the schema's spelling, the order given); empty when
    // none of them is one of input_pad_buttons.
    std::string PadChordValue(const std::vector<std::string>& buttons) const;
    static Schema FromJson(const Json& j);
    const SettingsGroup* Find(const std::string& mod) const;
    const SettingDef* Find(const std::string& mod, const std::string& section, const std::string& key) const;
};

// `value` as the setting's type spells it in an ini ("1" -> "true", "2.50" -> "2.5", an enum token
// in its schema case); false when it is not a valid value (out of range, unknown choice).
bool NormalizeValue(const SettingDef& def, const std::string& value, std::string* out, std::string* why = nullptr);

struct RawEntry {
    std::string section, key, value;
};

class SettingsModel {
public:
    void Load(const fs::path& game_dir, const Schema& schema);
    const Schema& schema() const { return schema_; }
    const fs::path& game_dir() const { return game_dir_; }

    // The value shown: a pending change, else the ini's, else the default.
    std::string Get(const std::string& mod, const std::string& section, const std::string& key) const;
    bool InIni(const std::string& mod, const std::string& section, const std::string& key) const;
    // Stages a change (normalised; false and `why` when invalid). Setting the ini's own value again
    // drops the pending change.
    bool Set(const std::string& mod, const std::string& section, const std::string& key, const std::string& value,
             std::string* why = nullptr);
    // Raw keys: no schema, no normalisation.
    std::vector<RawEntry> Raw(const std::string& mod) const;
    void SetRaw(const std::string& mod, const std::string& section, const std::string& key, const std::string& value);

    bool Dirty() const { return !pending_.empty(); }
    size_t PendingCount() const { return pending_.size(); }
    void Discard() { pending_.clear(); }
    // Every pending change: mod, section, key, value.
    std::vector<PresetChange> Pending() const;

    // Every stored setting of `mod` (all groups when empty) back to its default.
    void ResetToDefaults(const std::string& mod = std::string());
    // What a preset would change from the current values (changes that are already in place are
    // left out), and staging it.
    std::vector<std::pair<PresetChange, std::string>> PresetDiff(const Preset& preset) const;   // change, old value
    bool ApplyPreset(const Preset& preset, std::string* why = nullptr);

    enum class SaveResult { Saved, Nothing, Conflict, Failed };
    // Writes every pending change. Conflict: an ini changed on disk since it was read (nothing is
    // written; Reload and decide). `conflicts` names the files.
    SaveResult Save(std::string* error = nullptr, std::vector<std::string>* conflicts = nullptr);
    // Has any ini changed on disk since it was read?
    bool ChangedOnDisk() const;
    // Re-reads every ini; pending changes stay (they are re-applied on top).
    void Reload();

    // Queues the pending changes in <game>/atmt_manager_pending.json (the game is running) and
    // clears them here.
    bool Queue(std::string* error = nullptr);

private:
    struct IniState {
        std::string hash;   // md5 of the file as read ("" = it did not exist): an edit within the
                            // same second leaves the mtime alone on some file systems
        std::map<std::string, std::string> values;   // "section\nkey" (lower case) -> value
        std::vector<RawEntry> entries;               // in file order
    };
    static std::string Id(const std::string& section, const std::string& key);
    void ReadIni(const SettingsGroup& g);

    fs::path game_dir_;
    Schema schema_;
    std::map<std::string, IniState> inis_;                        // by mod
    std::map<std::string, PresetChange> pending_;                 // mod + id -> change
};

// The queued changes, if any, written into their inis (call once the game has exited). Returns the
// number of values written, -1 on a failure.
int ApplyQueuedSettings(const fs::path& game_dir, const Schema& schema, std::string* error = nullptr);
bool HasQueuedSettings(const fs::path& game_dir);

// Install time: every key of `schema` that an existing ini lacks is added with its default and its
// help comment (an ini that does not exist yet is left to the loader). Returns how many were added.
int AddMissingKeys(const fs::path& game_dir, const Schema& schema);

}  // namespace atmt
