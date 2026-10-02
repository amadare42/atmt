// settings.cpp - see settings.h.
#include "settings.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>

#include "hash.h"
#include "ini.h"

namespace atmt {

namespace {

constexpr const char* kQueueFile = "atmt_manager_pending.json";

std::wstring IniPath(const fs::path& p) { return p.wstring(); }

std::string FormatNumber(double v, bool integer) {
    char buf[64];
    if (integer) {
        std::snprintf(buf, sizeof(buf), "%lld", static_cast<long long>(std::llround(v)));
        return buf;
    }
    // the loader's rule: the shortest %g that reads back as the same float
    const float f = static_cast<float>(v);
    for (int digits = 6; digits <= 9; ++digits) {
        std::snprintf(buf, sizeof(buf), "%.*g", digits, static_cast<double>(f));
        if (static_cast<float>(std::strtod(buf, nullptr)) == f) break;
    }
    return buf;
}

// Writes `changes` (all for the ini at `path`), one WriteValues per section.
bool WriteChanges(const fs::path& path, const std::vector<PresetChange>& changes, const Schema& schema) {
    std::vector<bool> done(changes.size(), false);
    bool ok = true;
    for (size_t i = 0; i < changes.size(); ++i) {
        if (done[i]) continue;
        std::vector<atmt_ini::Entry> entries;
        for (size_t j = i; j < changes.size(); ++j) {
            if (done[j] || !IEquals(changes[j].section, changes[i].section)) continue;
            done[j] = true;
            atmt_ini::Entry e;
            e.key = changes[j].key;
            e.value = changes[j].value;
            if (const SettingDef* def = schema.Find(changes[j].mod, changes[j].section, changes[j].key)) {
                e.comment = def->help;
            }
            entries.push_back(e);
        }
        ok = atmt_ini::WriteValues(IniPath(path).c_str(), changes[i].section.c_str(), entries.data(), entries.size()) && ok;
    }
    return ok;
}

}  // namespace

// ---------------------------------------------------------------- schema
Schema Schema::FromJson(const Json& j) {
    Schema s;
    for (const Json& k : j["input"]["keys"].elements()) s.input_keys.push_back(k.AsString());
    for (const Json& m : j["input"]["key_modifiers"].elements()) s.input_key_modifiers.push_back(m.AsString());
    s.key_chord_separator = j["input"].Str("key_chord_separator", "+");
    for (const Json& b : j["input"]["pad_buttons"].elements()) s.input_pad_buttons.push_back(b.AsString());
    s.pad_chord_separator = j["input"].Str("pad_chord_separator", "+");
    for (const Json& g : j["groups"].elements()) {
        SettingsGroup group;
        group.mod = g.Str("mod");
        group.title = g.Str("title", group.mod);
        group.ini = g.Str("ini", std::string(kModDir) + "/" + group.mod + ".ini");
        for (const Json& e : g["settings"].elements()) {
            SettingDef d;
            d.type = e.Str("type");
            for (const Json& f : e["flags"].elements()) {
                const std::string flag = f.AsString();
                if (flag == "live") d.live = true;
                if (flag == "restart") d.restart = true;
                if (flag == "advanced") d.advanced = true;
                if (flag == "readonly") d.readonly = true;
            }
            d.section = e.Str("section", "General");
            d.key = e.Str("key");
            d.label = e.Str("label", d.key);
            d.help = e.Str("help");
            d.def = e.Str("default");
            if (e.Has("min") && e.Has("max")) {
                d.ranged = true;
                d.min = e["min"].AsNumber();
                d.max = e["max"].AsNumber();
            }
            d.step = e["step"].AsNumber(0);
            for (const Json& c : e["choices"].elements()) d.choices.push_back(c.AsString());
            d.max_length = static_cast<int>(e["max_length"].AsInt(0));
            if (d.type == "action" || d.type == "label") d.key.clear();
            group.settings.push_back(d);
        }
        s.groups.push_back(group);
    }
    return s;
}

std::string Schema::KeyChordValue(const std::vector<std::string>& mods, const std::string& key) const {
    std::string name;
    for (const std::string& k : input_keys) {
        if (IEquals(k, key)) name = k;
    }
    if (name.empty()) return std::string();
    std::string out;
    for (const std::string& m : input_key_modifiers) {   // the schema's order, whatever `mods` has
        for (const std::string& held : mods) {
            if (IEquals(m, held)) {
                out += m + key_chord_separator;
                break;
            }
        }
    }
    return out + name;
}

std::string Schema::PadChordValue(const std::vector<std::string>& buttons) const {
    std::string out;
    for (const std::string& b : buttons) {
        for (const std::string& known : input_pad_buttons) {
            if (!IEquals(known, b)) continue;
            if (!out.empty()) out += pad_chord_separator;
            out += known;
            break;
        }
    }
    return out;
}

const SettingsGroup* Schema::Find(const std::string& mod) const {
    for (const SettingsGroup& g : groups) {
        if (IEquals(g.mod, mod)) return &g;
    }
    return nullptr;
}

const SettingDef* Schema::Find(const std::string& mod, const std::string& section, const std::string& key) const {
    const SettingsGroup* g = Find(mod);
    if (g == nullptr) return nullptr;
    for (const SettingDef& d : g->settings) {
        if (d.stored() && IEquals(d.section, section) && IEquals(d.key, key)) return &d;
    }
    return nullptr;
}

bool NormalizeValue(const SettingDef& def, const std::string& value, std::string* out, std::string* why) {
    const std::string v = Trim(value);
    auto fail = [&](const std::string& reason) {
        if (why != nullptr) *why = def.key + ": " + reason;
        return false;
    };
    if (def.type == "bool") {
        const bool t = atmt_ini::Bool(v.c_str(), true), f = atmt_ini::Bool(v.c_str(), false);
        if (t != f) return fail("'" + v + "' is neither true nor false");
        *out = t ? "true" : "false";
        return true;
    }
    if (def.type == "int" || def.type == "float") {
        char* end = nullptr;
        const double d = def.type == "int" ? static_cast<double>(std::strtoll(v.c_str(), &end, 0)) : std::strtod(v.c_str(), &end);
        if (v.empty() || end == nullptr || *end != '\0' || !std::isfinite(d)) return fail("'" + v + "' is not a number");
        if (def.ranged && (d < def.min - 1e-9 || d > def.max + 1e-9)) {
            return fail(v + " is outside " + FormatNumber(def.min, false) + ".." + FormatNumber(def.max, false));
        }
        *out = FormatNumber(d, def.type == "int");
        return true;
    }
    if (def.type == "enum") {
        for (const std::string& c : def.choices) {
            if (IEquals(c, v)) {
                *out = c;
                return true;
            }
        }
        return fail("'" + v + "' is not one of " + Join(def.choices, ", "));
    }
    if (def.max_length > 0 && static_cast<int>(v.size()) > def.max_length) {
        return fail("longer than " + std::to_string(def.max_length) + " characters");
    }
    if (v.find('\n') != std::string::npos) return fail("a value cannot span lines");
    *out = v;
    return true;
}

// ---------------------------------------------------------------- the model
std::string SettingsModel::Id(const std::string& section, const std::string& key) {
    return Lower(section) + "\n" + Lower(key);
}

void SettingsModel::ReadIni(const SettingsGroup& g) {
    IniState st;
    const fs::path path = game_dir_ / Path(g.ini);
    std::string text;
    if (ReadFile(path, &text)) {
        st.hash = Md5Hex(text);
        std::string section;
        for (const std::string& raw : Lines(text)) {
            const std::string line = Trim(raw);
            if (line.empty() || line[0] == ';' || line[0] == '#') continue;
            if (line[0] == '[') {
                const size_t end = line.find(']');
                if (end != std::string::npos) section = line.substr(1, end - 1);
                continue;
            }
            const size_t eq = line.find('=');
            if (eq == std::string::npos) continue;
            const std::string key = Trim(line.substr(0, eq));
            std::string value = line.substr(eq + 1);
            for (size_t i = 1; i < value.size(); ++i) {   // ini.h's inline comment rule
                if (value[i] == ';' && (value[i - 1] == ' ' || value[i - 1] == '\t')) {
                    value = value.substr(0, i);
                    break;
                }
            }
            value = Trim(value);
            const std::string id = Id(section, key);
            if (st.values.count(id) != 0) continue;   // the first one wins, as for the readers
            st.values[id] = value;
            st.entries.push_back({section, key, value});
        }
    }
    inis_[g.mod] = st;
}

void SettingsModel::Load(const fs::path& game_dir, const Schema& schema) {
    game_dir_ = game_dir;
    schema_ = schema;
    inis_.clear();
    pending_.clear();
    for (const SettingsGroup& g : schema_.groups) ReadIni(g);
}

void SettingsModel::Reload() {
    for (const SettingsGroup& g : schema_.groups) ReadIni(g);
}

std::string SettingsModel::Get(const std::string& mod, const std::string& section, const std::string& key) const {
    auto p = pending_.find(Lower(mod) + "\n" + Id(section, key));
    if (p != pending_.end()) return p->second.value;
    auto ini = inis_.find(mod);
    if (ini != inis_.end()) {
        auto v = ini->second.values.find(Id(section, key));
        if (v != ini->second.values.end()) {
            // shown as its type spells it; an invalid value shows as is (the loader falls back to
            // the default for it, and the form says so)
            if (const SettingDef* def = schema_.Find(mod, section, key)) {
                std::string norm;
                if (NormalizeValue(*def, v->second, &norm)) return norm;
            }
            return v->second;
        }
    }
    if (const SettingDef* def = schema_.Find(mod, section, key)) return def->def;
    return std::string();
}

bool SettingsModel::InIni(const std::string& mod, const std::string& section, const std::string& key) const {
    auto ini = inis_.find(mod);
    return ini != inis_.end() && ini->second.values.count(Id(section, key)) != 0;
}

bool SettingsModel::Set(const std::string& mod, const std::string& section, const std::string& key,
                        const std::string& value, std::string* why) {
    const SettingDef* def = schema_.Find(mod, section, key);
    if (def == nullptr) {
        if (why != nullptr) *why = mod + ": no setting [" + section + "] " + key;
        return false;
    }
    std::string norm;
    if (!NormalizeValue(*def, value, &norm, why)) return false;
    const std::string pid = Lower(mod) + "\n" + Id(section, key);
    // back to what the ini already has: nothing to write
    auto ini = inis_.find(mod);
    if (ini != inis_.end()) {
        auto v = ini->second.values.find(Id(section, key));
        std::string current;
        if (v != ini->second.values.end() && NormalizeValue(*def, v->second, &current) && current == norm) {
            pending_.erase(pid);
            return true;
        }
    }
    pending_[pid] = PresetChange{mod, def->section, def->key, norm};
    return true;
}

std::vector<RawEntry> SettingsModel::Raw(const std::string& mod) const {
    std::vector<RawEntry> out;
    auto ini = inis_.find(mod);
    if (ini == inis_.end()) return out;
    for (const RawEntry& e : ini->second.entries) {
        if (schema_.Find(mod, e.section, e.key) != nullptr) continue;
        RawEntry r = e;
        auto p = pending_.find(Lower(mod) + "\n" + Id(e.section, e.key));
        if (p != pending_.end()) r.value = p->second.value;
        out.push_back(r);
    }
    return out;
}

void SettingsModel::SetRaw(const std::string& mod, const std::string& section, const std::string& key,
                           const std::string& value) {
    const std::string v = Trim(value);
    const std::string pid = Lower(mod) + "\n" + Id(section, key);
    auto ini = inis_.find(mod);
    if (ini != inis_.end()) {
        auto cur = ini->second.values.find(Id(section, key));
        if (cur != ini->second.values.end() && cur->second == v) {
            pending_.erase(pid);
            return;
        }
    }
    pending_[pid] = PresetChange{mod, section, key, v};
}

std::vector<PresetChange> SettingsModel::Pending() const {
    std::vector<PresetChange> out;
    for (const auto& kv : pending_) out.push_back(kv.second);
    return out;
}

void SettingsModel::ResetToDefaults(const std::string& mod) {
    for (const SettingsGroup& g : schema_.groups) {
        if (!mod.empty() && !IEquals(g.mod, mod)) continue;
        for (const SettingDef& d : g.settings) {
            if (!d.stored() || d.readonly) continue;
            Set(g.mod, d.section, d.key, d.def);
        }
    }
}

std::vector<std::pair<PresetChange, std::string>> SettingsModel::PresetDiff(const Preset& preset) const {
    std::vector<std::pair<PresetChange, std::string>> out;
    for (const PresetChange& c : preset.changes) {
        const SettingDef* def = schema_.Find(c.mod, c.section, c.key);
        std::string norm = c.value;
        if (def != nullptr) NormalizeValue(*def, c.value, &norm);
        const std::string current = Get(c.mod, c.section, c.key);
        if (current == norm && InIni(c.mod, c.section, c.key)) continue;
        PresetChange n = c;
        n.value = norm;
        out.emplace_back(n, current);
    }
    return out;
}

bool SettingsModel::ApplyPreset(const Preset& preset, std::string* why) {
    for (const PresetChange& c : preset.changes) {
        if (schema_.Find(c.mod) == nullptr) continue;   // a mod this payload does not have
        if (!Set(c.mod, c.section, c.key, c.value, why)) return false;
    }
    return true;
}

bool SettingsModel::ChangedOnDisk() const {
    for (const SettingsGroup& g : schema_.groups) {
        auto it = inis_.find(g.mod);
        if (it == inis_.end()) continue;
        if (Md5File(game_dir_ / Path(g.ini)) != it->second.hash) return true;
    }
    return false;
}

SettingsModel::SaveResult SettingsModel::Save(std::string* error, std::vector<std::string>* conflicts) {
    if (pending_.empty()) return SaveResult::Nothing;
    std::map<std::string, std::vector<PresetChange>> by_mod;
    for (const auto& kv : pending_) by_mod[kv.second.mod].push_back(kv.second);
    std::vector<std::string> changed;
    for (const auto& kv : by_mod) {
        const SettingsGroup* g = schema_.Find(kv.first);
        if (g == nullptr) continue;
        if (Md5File(game_dir_ / Path(g->ini)) != inis_[g->mod].hash) changed.push_back(g->ini);
    }
    if (!changed.empty()) {
        if (conflicts != nullptr) *conflicts = changed;
        if (error != nullptr) *error = "changed on disk since it was read: " + Join(changed, ", ");
        return SaveResult::Conflict;
    }
    for (const auto& kv : by_mod) {
        const SettingsGroup* g = schema_.Find(kv.first);
        if (g == nullptr) continue;
        const fs::path path = game_dir_ / Path(g->ini);
        std::error_code ec;
        fs::create_directories(path.parent_path(), ec);
        if (!WriteChanges(path, kv.second, schema_)) {
            if (error != nullptr) *error = "could not write " + g->ini;
            return SaveResult::Failed;
        }
        std::vector<std::string> list;
        for (const PresetChange& c : kv.second) list.push_back(c.key + "=" + c.value);
        Log("settings: " + g->ini + ": " + Join(list, ", "));
        for (const PresetChange& c : kv.second) pending_.erase(Lower(c.mod) + "\n" + Id(c.section, c.key));
        ReadIni(*g);
    }
    return SaveResult::Saved;
}

bool SettingsModel::Queue(std::string* error) {
    Json q;
    std::string text;
    if (ReadFile(game_dir_ / kQueueFile, &text)) Json::Parse(text, &q);
    Json& list = q["changes"];
    if (!list.is_array()) list = Json::MakeArray();
    for (const auto& kv : pending_) {
        Json c = Json::MakeObject();
        c["mod"] = kv.second.mod;
        c["section"] = kv.second.section;
        c["key"] = kv.second.key;
        c["value"] = kv.second.value;
        list.Push(c);
    }
    q["queued_at"] = IsoTimeUtc();
    if (!WriteFileAtomic(game_dir_ / kQueueFile, q.Dump(), error)) return false;
    Logf("settings: %u change(s) queued until the game has exited", static_cast<unsigned>(pending_.size()));
    pending_.clear();
    return true;
}

bool HasQueuedSettings(const fs::path& game_dir) { return Exists(game_dir / kQueueFile); }

int ApplyQueuedSettings(const fs::path& game_dir, const Schema& schema, std::string* error) {
    std::string text;
    if (!ReadFile(game_dir / kQueueFile, &text)) return 0;
    Json q;
    if (!Json::Parse(text, &q, error)) return -1;
    // later entries win: the same key queued twice keeps the last value
    std::map<std::string, std::vector<PresetChange>> by_ini;
    std::map<std::string, size_t> seen;
    for (const Json& c : q["changes"].elements()) {
        PresetChange ch{c.Str("mod"), c.Str("section", "General"), c.Str("key"), c.Str("value")};
        const SettingsGroup* g = schema.Find(ch.mod);
        if (g == nullptr || ch.key.empty()) continue;
        const std::string id = g->ini + "\n" + Lower(ch.section) + "\n" + Lower(ch.key);
        auto it = seen.find(id);
        if (it != seen.end()) {
            by_ini[g->ini][it->second] = ch;
            continue;
        }
        seen[id] = by_ini[g->ini].size();
        by_ini[g->ini].push_back(ch);
    }
    int written = 0;
    for (const auto& kv : by_ini) {
        if (!WriteChanges(game_dir / Path(kv.first), kv.second, schema)) {
            if (error != nullptr) *error = "could not write " + kv.first;
            return -1;
        }
        written += static_cast<int>(kv.second.size());
    }
    std::string err;
    RemoveFile(game_dir / kQueueFile, &err);
    Logf("settings: %d queued change(s) written", written);
    return written;
}

int AddMissingKeys(const fs::path& game_dir, const Schema& schema) {
    int added = 0;
    for (const SettingsGroup& g : schema.groups) {
        const fs::path path = game_dir / Path(g.ini);
        if (!Exists(path)) continue;
        std::vector<PresetChange> missing;
        for (const SettingDef& d : g.settings) {
            if (!d.stored()) continue;
            char buf[8];
            if (atmt_ini::Read(IniPath(path).c_str(), d.section.c_str(), d.key.c_str(), buf, sizeof(buf), nullptr)) continue;
            missing.push_back(PresetChange{g.mod, d.section, d.key, d.def});
        }
        if (missing.empty()) continue;
        if (WriteChanges(path, missing, schema)) {
            added += static_cast<int>(missing.size());
            std::vector<std::string> keys;
            for (const PresetChange& c : missing) keys.push_back(c.key);
            Log("  " + g.ini + ": added " + Join(keys, ", ") + " (new in this version)");
        }
    }
    return added;
}

}  // namespace atmt
