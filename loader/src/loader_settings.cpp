// loader_settings.cpp - the settings registry (AtmtModApi::settings_*).
//
// Mods describe their settings as data (shared/mod_api.h, AtmtSetting); the registry
// loads them from each mod's ini, lets a UI enumerate and change them, and writes the
// changes back. It lives in the loader rather than in the mod that draws the UI for
// three reasons:
//   * load order stops mattering - a mod that registers before the overlay exists
//     (mods load alphabetically) is simply there when the overlay first looks;
//   * the loader already knows each mod's ini, and when a mod goes away, so an entry
//     never outlives the dll its `value` points into;
//   * it is still game-agnostic: a list and an ini writer.
#include "ini.h"
#include "loader.h"
#include "mod_api.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace atmt_loader {

namespace {

struct Group {
    const void* owner = nullptr;
    std::string mod_name;
    std::wstring ini_path;
    std::string title;
    std::string description;
    std::vector<AtmtSetting> settings;   // copies; `value` still points into the mod
    std::vector<char> dirty;            // changed since the last commit
};

struct Registry {
    CRITICAL_SECTION lock;
    std::vector<std::unique_ptr<Group>> groups;
    std::vector<AtmtSettingsGroup> view;   // what settings_acquire hands out
    uint32_t generation = 0;
    Registry() { InitializeCriticalSection(&lock); }
};

Registry& R() {
    static Registry registry;
    return registry;
}

class Locked {
public:
    Locked() { EnterCriticalSection(&R().lock); }
    ~Locked() { LeaveCriticalSection(&R().lock); }
    Locked(const Locked&) = delete;
    Locked& operator=(const Locked&) = delete;
};

const char* TypeName(uint32_t type) {
    switch (type) {
        case ATMT_SETTING_BOOL: return "bool";
        case ATMT_SETTING_INT: return "int";
        case ATMT_SETTING_FLOAT: return "float";
        case ATMT_SETTING_ENUM: return "enum";
        case ATMT_SETTING_STRING: return "string";
        case ATMT_SETTING_KEY: return "key";
        case ATMT_SETTING_PAD_BUTTON: return "pad button";
        case ATMT_SETTING_ACTION: return "action";
        case ATMT_SETTING_LABEL: return "label";
        default: return "?";
    }
}

bool HasStorage(uint32_t type) {
    return type <= ATMT_SETTING_PAD_BUTTON;   // everything but ACTION and LABEL
}

bool IsString(uint32_t type) {
    return type == ATMT_SETTING_STRING || type == ATMT_SETTING_KEY || type == ATMT_SETTING_PAD_BUTTON;
}

const char* SectionOf(const AtmtSetting& s) { return s.section != nullptr ? s.section : "General"; }

const char* KeyOf(const AtmtSetting& s) { return s.key != nullptr ? s.key : "(no key)"; }

// Why this entry cannot be registered, or NULL when it can.
const char* Invalid(const AtmtSetting& s) {
    if (s.type > ATMT_SETTING_LABEL) return "unknown type";
    if (!HasStorage(s.type)) return nullptr;
    if (s.key == nullptr || s.key[0] == '\0') return "no ini key";
    if (s.value == nullptr) return "no value storage";
    if (IsString(s.type) && s.capacity == 0) return "string storage without a capacity";
    if (s.type == ATMT_SETTING_ENUM) {
        if (s.choices == nullptr || s.choice_count == 0) return "enum without choices";
        for (uint32_t i = 0; i < s.choice_count; ++i) {
            if (s.choices[i] == nullptr) return "enum with a NULL choice";
        }
    }
    return nullptr;
}

bool Ranged(const AtmtSetting& s) { return s.min < s.max; }

int32_t ClampInt(const AtmtSetting& s, long long v) {
    if (Ranged(s)) {
        const long long lo = static_cast<long long>(std::ceil(s.min));
        const long long hi = static_cast<long long>(std::floor(s.max));
        if (v < lo) v = lo;
        if (v > hi) v = hi;
    }
    if (v < INT32_MIN) v = INT32_MIN;
    if (v > INT32_MAX) v = INT32_MAX;
    return static_cast<int32_t>(v);
}

float ClampFloat(const AtmtSetting& s, float v) {
    if (Ranged(s)) {
        if (v < s.min) v = s.min;
        if (v > s.max) v = s.max;
    }
    return v;
}

// The value as the ini spells it.
std::string Format(const AtmtSetting& s) {
    char buf[64];
    switch (s.type) {
        case ATMT_SETTING_BOOL:
            return *static_cast<const int32_t*>(s.value) != 0 ? "true" : "false";
        case ATMT_SETTING_INT:
            _snprintf(buf, sizeof(buf), "%d", static_cast<int>(*static_cast<const int32_t*>(s.value)));
            return buf;
        case ATMT_SETTING_FLOAT:
            // shortest of %g that reads back as the same float, so 2.0 stays "2" and 0.35
            // does not turn into 0.349999994
            for (int digits = 6; digits <= 9; ++digits) {
                _snprintf(buf, sizeof(buf), "%.*g", digits, *static_cast<const float*>(s.value));
                if (static_cast<float>(std::strtod(buf, nullptr)) == *static_cast<const float*>(s.value)) {
                    break;
                }
            }
            return buf;
        case ATMT_SETTING_ENUM: {
            const int32_t index = *static_cast<const int32_t*>(s.value);
            if (index >= 0 && static_cast<uint32_t>(index) < s.choice_count) return s.choices[index];
            return s.choices[0];
        }
        default:
            if (IsString(s.type)) {
                const char* text = static_cast<const char*>(s.value);
                return std::string(text, strnlen(text, s.capacity));
            }
            return std::string();
    }
}

// Stores `text` (a value read from the ini) into the mod's storage. False when it is
// not a valid value of the setting's type - the storage then keeps its default.
bool Parse(const AtmtSetting& s, const char* text) {
    char* end = nullptr;
    switch (s.type) {
        case ATMT_SETTING_BOOL: {
            const bool a = atmt_ini::Bool(text, false);
            const bool b = atmt_ini::Bool(text, true);
            if (a != b) return false;   // neither true nor false
            *static_cast<int32_t*>(s.value) = a ? 1 : 0;
            return true;
        }
        case ATMT_SETTING_INT: {
            const long long v = std::strtoll(text, &end, 10);   // base 10: "010" is 10, not octal 8
            if (end == text || *end != '\0') return false;
            *static_cast<int32_t*>(s.value) = ClampInt(s, v);
            return true;
        }
        case ATMT_SETTING_FLOAT: {
            const double v = std::strtod(text, &end);
            if (end == text || *end != '\0' || !std::isfinite(v)) return false;
            *static_cast<float*>(s.value) = ClampFloat(s, static_cast<float>(v));
            return true;
        }
        case ATMT_SETTING_ENUM:
            for (uint32_t i = 0; i < s.choice_count; ++i) {
                if (_stricmp(text, s.choices[i]) == 0) {
                    *static_cast<int32_t*>(s.value) = static_cast<int32_t>(i);
                    return true;
                }
            }
            return false;
        default:
            if (IsString(s.type)) {
                _snprintf(static_cast<char*>(s.value), s.capacity, "%s", text);
                static_cast<char*>(s.value)[s.capacity - 1] = '\0';
                return true;
            }
            return false;
    }
}

// Stores a value handed over by settings_set. Returns true when the storage changed.
bool Store(const AtmtSetting& s, const void* new_value) {
    if (new_value == nullptr) return false;
    switch (s.type) {
        case ATMT_SETTING_BOOL: {
            const int32_t v = *static_cast<const int32_t*>(new_value) != 0 ? 1 : 0;
            int32_t& cur = *static_cast<int32_t*>(s.value);
            if (cur == v) return false;
            cur = v;
            return true;
        }
        case ATMT_SETTING_INT: {
            const int32_t v = ClampInt(s, *static_cast<const int32_t*>(new_value));
            int32_t& cur = *static_cast<int32_t*>(s.value);
            if (cur == v) return false;
            cur = v;
            return true;
        }
        case ATMT_SETTING_FLOAT: {
            const float in = *static_cast<const float*>(new_value);
            if (!std::isfinite(in)) return false;
            const float v = ClampFloat(s, in);
            float& cur = *static_cast<float*>(s.value);
            if (cur == v) return false;
            cur = v;
            return true;
        }
        case ATMT_SETTING_ENUM: {
            const int32_t v = *static_cast<const int32_t*>(new_value);
            if (v < 0 || static_cast<uint32_t>(v) >= s.choice_count) return false;
            int32_t& cur = *static_cast<int32_t*>(s.value);
            if (cur == v) return false;
            cur = v;
            return true;
        }
        default: {
            if (!IsString(s.type)) return false;
            char* cur = static_cast<char*>(s.value);
            const char* in = static_cast<const char*>(new_value);
            if (strncmp(cur, in, s.capacity - 1) == 0) return false;
            _snprintf(cur, s.capacity, "%s", in);
            cur[s.capacity - 1] = '\0';
            return true;
        }
    }
}

// Writes `indices` of `g` into its ini, one WriteValues per section, keeping the
// table's order. `with_comments` adds each setting's help above a key that is new.
bool WriteSettings(const Group& g, const std::vector<size_t>& indices, bool with_comments) {
    bool ok = true;
    std::vector<bool> done(indices.size(), false);
    for (size_t first = 0; first < indices.size(); ++first) {
        if (done[first]) continue;
        const char* section = SectionOf(g.settings[indices[first]]);
        std::vector<atmt_ini::Entry> entries;
        for (size_t j = first; j < indices.size(); ++j) {
            const AtmtSetting& s = g.settings[indices[j]];
            if (done[j] || _stricmp(SectionOf(s), section) != 0) continue;
            done[j] = true;
            atmt_ini::Entry e;
            e.key = s.key;
            e.value = Format(s);
            if (with_comments && s.help != nullptr) e.comment = s.help;
            entries.push_back(std::move(e));
        }
        ok = atmt_ini::WriteValues(g.ini_path.c_str(), section, entries.data(), entries.size()) && ok;
    }
    return ok;
}

// Saves the dirty values of `g`. Returns how many were written, or -1. Lock held.
int CommitGroup(Group& g) {
    std::vector<size_t> indices;
    std::string list;
    for (size_t i = 0; i < g.settings.size(); ++i) {
        if (!g.dirty[i]) continue;
        indices.push_back(i);
        if (!list.empty()) list += ", ";
        list += std::string(KeyOf(g.settings[i])) + "=" + Format(g.settings[i]);
    }
    if (indices.empty()) return 0;
    if (!WriteSettings(g, indices, false)) {
        LogError("settings: %s: could NOT write its ini (%s)", g.mod_name.c_str(), list.c_str());
        return -1;
    }
    for (size_t i : indices) g.dirty[i] = 0;
    Log("settings: %s: saved %s", g.mod_name.c_str(), list.c_str());
    return static_cast<int>(indices.size());
}

// Lock held.
void RebuildView() {
    Registry& r = R();
    r.view.clear();
    for (const auto& g : r.groups) {
        AtmtSettingsGroup v;
        v.mod_name = g->mod_name.c_str();
        v.title = g->title.c_str();
        v.description = g->description.c_str();
        v.settings = g->settings.data();
        v.count = static_cast<uint32_t>(g->settings.size());
        r.view.push_back(v);
    }
    ++r.generation;
}

}  // namespace

int SettingsRegister(const void* owner, const std::string& mod_name, const std::wstring& ini_path,
                     const char* title, const char* description, const AtmtSetting* table,
                     uint32_t count) {
    if (owner == nullptr || (table == nullptr && count != 0)) return -1;
    for (uint32_t i = 0; i < count; ++i) {
        if (const char* why = Invalid(table[i])) {
            LogError("settings: %s: entry %u (%s) refused: %s - nothing registered", mod_name.c_str(), i,
                KeyOf(table[i]), why);
            return -1;
        }
    }

    auto g = std::make_unique<Group>();
    g->owner = owner;
    g->mod_name = mod_name;
    g->ini_path = ini_path;
    g->title = (title != nullptr && title[0] != '\0') ? title : mod_name;
    if (description != nullptr) g->description = description;
    g->settings.assign(table, table + count);
    g->dirty.assign(count, 0);

    // The ini wins over the defaults the mod put in its storage; a key it does not have
    // yet gets the default written into it, so the file always lists every setting.
    std::vector<size_t> missing;
    for (size_t i = 0; i < g->settings.size(); ++i) {
        const AtmtSetting& s = g->settings[i];
        if (!HasStorage(s.type)) continue;
        std::vector<char> buf(IsString(s.type) && s.capacity > 512 ? s.capacity : 512);
        if (!atmt_ini::Read(ini_path.c_str(), SectionOf(s), s.key, buf.data(), buf.size(), nullptr)) {
            missing.push_back(i);
            continue;
        }
        if (!Parse(s, buf.data())) {
            LogError("settings: %s: [%s] %s=%s is not a valid %s - using %s", mod_name.c_str(),
                SectionOf(s), s.key, buf.data(), TypeName(s.type), Format(s).c_str());
        }
    }
    if (!missing.empty()) {
        if (WriteSettings(*g, missing, true)) {
            Log("settings: %s: added %u missing key(s) to its ini", mod_name.c_str(),
                static_cast<unsigned>(missing.size()));
        } else {
            LogError("settings: %s: could NOT add its missing keys to its ini", mod_name.c_str());
        }
    }

    Locked lock;
    Registry& r = R();
    bool replaced = false;
    for (auto& existing : r.groups) {
        if (existing->owner != owner) continue;
        // No commit of the old table here: its dll may already be gone (a dev reload
        // re-registering from a fresh build). A mod that wants its unsaved changes kept
        // across that calls settings_unregister in its own teardown.
        existing = std::move(g);
        replaced = true;
        break;
    }
    if (!replaced) r.groups.push_back(std::move(g));
    RebuildView();
    Log("settings: %s: %s %u setting(s) as \"%s\"", mod_name.c_str(),
        replaced ? "re-registered" : "registered", count, title != nullptr ? title : mod_name.c_str());
    return static_cast<int>(count);
}

void SettingsUnregister(const void* owner) {
    Locked lock;
    Registry& r = R();
    for (size_t i = 0; i < r.groups.size(); ++i) {
        if (r.groups[i]->owner != owner) continue;
        CommitGroup(*r.groups[i]);   // the mod is still loaded here: its storage is readable
        r.groups.erase(r.groups.begin() + static_cast<std::ptrdiff_t>(i));
        RebuildView();
        return;
    }
}

const AtmtSettingsGroup* SettingsAcquire(uint32_t* count, uint32_t* generation) {
    Registry& r = R();
    EnterCriticalSection(&r.lock);   // left in SettingsRelease
    if (count != nullptr) *count = static_cast<uint32_t>(r.view.size());
    if (generation != nullptr) *generation = r.generation;
    return r.view.empty() ? nullptr : r.view.data();
}

void SettingsRelease() { LeaveCriticalSection(&R().lock); }

int SettingsSet(const AtmtSetting* setting, const void* new_value) {
    if (setting == nullptr) return -1;
    Locked lock;
    for (auto& g : R().groups) {
        for (size_t i = 0; i < g->settings.size(); ++i) {
            AtmtSetting& s = g->settings[i];
            const bool match =
                &s == setting || (setting->value != nullptr && s.value == setting->value);
            if (!match) continue;
            if (s.type == ATMT_SETTING_LABEL) return 0;
            bool changed = false;
            if (s.type != ATMT_SETTING_ACTION) {
                changed = Store(s, new_value);
                if (!changed) return 0;
                g->dirty[i] = 1;
            }
            if (s.on_change != nullptr) {
                try {
                    s.on_change(&s, s.user);
                } catch (...) {
                    LogError("settings: %s: on_change for %s threw", g->mod_name.c_str(), KeyOf(s));
                }
            }
            return changed ? 1 : 0;
        }
    }
    return -1;
}

int SettingsCommit() {
    Locked lock;
    int written = 0;
    bool failed = false;
    for (auto& g : R().groups) {
        const int n = CommitGroup(*g);
        if (n < 0) {
            failed = true;
        } else {
            written += n;
        }
    }
    return failed ? -1 : written;
}

}  // namespace atmt_loader
