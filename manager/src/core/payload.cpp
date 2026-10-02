// payload.cpp - see payload.h.
#include "payload.h"

#include <algorithm>
#include <cstdlib>

#include "hash.h"
#include "platform.h"

#ifndef ATMT_MANAGER_VERSION
#define ATMT_MANAGER_VERSION "0.0.0-dev"
#endif

namespace atmt {

const char* AppVersion() { return ATMT_MANAGER_VERSION; }

int CompareVersions(const std::string& a, const std::string& b) {
    auto split = [](const std::string& v, std::string* pre) {
        std::string core = v;
        if (!core.empty() && (core[0] == 'v' || core[0] == 'V')) core = core.substr(1);
        const size_t dash = core.find('-');
        if (dash != std::string::npos) {
            *pre = core.substr(dash + 1);
            core = core.substr(0, dash);
        }
        std::vector<long> parts;
        for (const std::string& p : Split(core, '.')) parts.push_back(std::strtol(p.c_str(), nullptr, 10));
        while (parts.size() < 3) parts.push_back(0);
        return parts;
    };
    std::string pa, pb;
    const std::vector<long> va = split(a, &pa), vb = split(b, &pb);
    for (size_t i = 0; i < std::max(va.size(), vb.size()); ++i) {
        const long x = i < va.size() ? va[i] : 0, y = i < vb.size() ? vb[i] : 0;
        if (x != y) return x < y ? -1 : 1;
    }
    if (pa == pb) return 0;
    if (pa.empty()) return 1;   // 1.0.0 > 1.0.0-rc1
    if (pb.empty()) return -1;
    return pa < pb ? -1 : 1;
}

bool ValidComponentName(const std::string& name) {
    if (name.empty() || name.size() > 64) return false;
    for (char c : name) {
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_')) return false;
    }
    return true;
}

// ---------------------------------------------------------------- a component
bool Component::Load(const fs::path& dir, Component* out, std::string* error) {
    auto fail = [&](const std::string& why) {
        if (error != nullptr) *error = U8(dir) + ": " + why;
        return false;
    };
    Component c;
    c.dir = dir;
    std::string text, jerr;
    Json meta;
    if (!ReadFile(dir / "component.json", &text)) return fail("no component.json");
    if (!Json::Parse(text, &meta, &jerr)) return fail("component.json: " + jerr);
    c.name = meta.Str("name");
    c.kind = meta.Str("kind");
    c.version = meta.Str("version");
    c.min_manager_version = meta.Str("min_manager_version", "0.0.0");
    c.title = meta.Str("title", c.name);
    c.default_enabled = meta["default_enabled"].AsBool(true);
    if (!ValidComponentName(c.name)) return fail("component.json: the name '" + c.name + "' is not [a-z0-9_]+");
    if (c.version.empty()) return fail("component.json has no version");
    if (c.kind != "loader" && c.kind != "mod" && c.kind != "data") return fail("component.json: unknown kind '" + c.kind + "'");
    if ((c.kind == "loader") != (c.name == kLoaderComponent)) return fail("only the loader component is named loader");

    if (!ReadFile(dir / "manifest.md5", &text)) return fail("no manifest.md5");
    for (const std::string& line : Lines(text)) {
        if (line.size() < 35 || line[32] != ' ') continue;
        std::string path = line.substr(34);
        if (!path.empty() && path[0] == '*') path = path.substr(1);   // md5sum's binary marker
        c.manifest.emplace_back(path, Lower(line.substr(0, 32)));
    }
    auto listed = [&](const std::string& rel) {
        for (const auto& kv : c.manifest) {
            if (kv.first == rel) return true;
        }
        return false;
    };
    if (!listed("component.json")) return fail("component.json is not in manifest.md5");
    if (c.kind == "loader" && !listed(std::string("files/") + kProxyDll)) return fail(std::string("no files/") + kProxyDll);
    if (c.kind == "mod" && !listed("files/" + std::string(kModDir) + "/" + c.name + ".dll")) {
        return fail("no files/" + std::string(kModDir) + "/" + c.name + ".dll");
    }
    *out = std::move(c);
    return true;
}

bool Component::VerifyFiles(std::string* error) const {
    for (const auto& kv : manifest) {
        const std::string got = Md5File(dir / Path(kv.first));
        if (got != kv.second) {
            if (error != nullptr) {
                *error = name + " " + version + ": " + kv.first + (got.empty() ? " is missing" : " does not match manifest.md5");
            }
            return false;
        }
    }
    return true;
}

bool Component::Usable() const { return CompareVersions(min_manager_version, AppVersion()) <= 0; }

// ---------------------------------------------------------------- the payload
namespace {

bool ReadPresets(const fs::path& dir, std::vector<Preset>* out, std::string* error) {
    std::error_code ec;
    for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
        if (!EndsWith(Lower(U8(it->path().filename())), ".json")) continue;
        Json j;
        std::string text, jerr;
        if (!ReadFile(it->path(), &text) || !Json::Parse(text, &j, &jerr)) {
            *error = "preset " + U8(it->path().filename()) + ": " + jerr;
            return false;
        }
        Preset preset;
        preset.id = U8(it->path().stem());
        preset.name = j.Str("name", preset.id);
        preset.description = j.Str("description");
        for (const Json& c : j["changes"].elements()) {
            PresetChange change;
            change.mod = c.Str("mod");
            change.section = c.Str("section", "General");
            change.key = c.Str("key");
            change.value = c["value"].is_string() ? c.Str("value")
                           : c["value"].type() == Json::Bool ? (c["value"].AsBool() ? "true" : "false")
                                                             : c["value"].Dump(false);
            if (!change.mod.empty() && !change.key.empty()) preset.changes.push_back(change);
        }
        for (const auto& kv : j["mods"].items()) {
            if (!kv.first.empty() && kv.second.type() == Json::Bool) preset.mods.emplace_back(kv.first, kv.second.AsBool());
        }
        for (const Json& r : j["recommended_on"].elements()) preset.recommended_on.push_back(Lower(r.AsString()));
        preset.icon_pack = j["icon_pack"].AsBool(false);
        out->push_back(preset);
    }
    return true;
}

}  // namespace

bool Payload::FromComponents(std::vector<Component> components, Payload* out, std::string* error) {
    auto fail = [&](const std::string& why) {
        if (error != nullptr) *error = why;
        return false;
    };
    std::sort(components.begin(), components.end(), [](const Component& a, const Component& b) {
        const bool la = a.kind == "loader", lb = b.kind == "loader";
        return la != lb ? la : a.name < b.name;
    });
    for (size_t i = 1; i < components.size(); ++i) {
        if (components[i].name == components[i - 1].name) return fail("two components named " + components[i].name);
    }
    if (components.empty() || components[0].kind != "loader") return fail("no loader component");

    Payload p;
    Json schema = Json::MakeObject();
    Json schema_mods = Json::MakeArray(), schema_groups = Json::MakeArray();
    std::string text, jerr;
    std::string input_from;   // the component whose schema.json has "input"
    for (const Component& c : components) {
        const std::string where = c.name + " " + c.version + ": ";
        for (const auto& kv : c.manifest) {
            if (!StartsWith(kv.first, "files/")) continue;
            PayloadFile f;
            f.rel = kv.first.substr(6);
            f.source = c.dir / Path(kv.first);
            f.md5 = kv.second;
            f.component = c.name;
            for (const PayloadFile& other : p.files) {
                if (other.rel == f.rel) return fail(where + f.rel + " is also in " + other.component);
            }
            p.files.push_back(f);
        }
        if (c.kind == "mod") {
            ModInfo m;
            m.name = c.name;
            m.title = c.title;
            m.version = c.version;
            m.default_enabled = c.default_enabled;
            p.mods.push_back(m);
        }
        if (ReadFile(c.dir / "schema.json", &text)) {
            Json part;
            if (!Json::Parse(text, &part, &jerr)) return fail(where + "schema.json: " + jerr);
            if (part["input"].is_object()) {
                // one component has the input names (the data component); two would silently
                // override each other, depending on their order
                if (!input_from.empty()) return fail(where + "schema.json has \"input\", and so has " + input_from);
                schema["input"] = part["input"];
                input_from = c.name;
            }
            if (part["schema_version"].AsInt(0) > schema["schema_version"].AsInt(0)) schema["schema_version"] = part["schema_version"];
            for (const Json& m : part["mods"].elements()) schema_mods.Push(m);
            for (const Json& g : part["groups"].elements()) schema_groups.Push(g);
        }
        if (!ReadPresets(c.dir / "presets", &p.presets, &jerr)) return fail(where + jerr);
        if (ReadFile(c.dir / "supported_exe.txt", &text)) {
            for (const std::string& raw : Lines(text)) {
                const std::string line = Trim(raw);
                if (line.size() < 32 || line[0] == '#') continue;
                SupportedExe s;
                s.md5 = Lower(line.substr(0, 32));
                s.note = Trim(line.substr(32));
                p.supported.push_back(s);
            }
        }
        if (ReadFile(c.dir / "icon_pack.json", &text)) {
            Json icons;
            if (!Json::Parse(text, &icons, &jerr)) return fail(where + "icon_pack.json: " + jerr);
            IconPackConfig config;
            if (!IconPackConfig::FromJson(icons, &config, &jerr)) return fail(where + "icon_pack.json: " + jerr);
            if (!config.empty()) p.icon_pack = config;
        }
        if (ReadFile(c.dir / "install_rules.json", &text)) {
            Json rules;
            if (!Json::Parse(text, &rules, &jerr)) return fail(where + "install_rules.json: " + jerr);
            for (const Json& r : rules["remove"].elements()) {
                if (!r.AsString().empty()) p.remove_rules.push_back(r.AsString());
            }
        }
    }
    std::sort(p.presets.begin(), p.presets.end(), [](const Preset& a, const Preset& b) { return a.id < b.id; });
    schema["mods"] = schema_mods;
    schema["groups"] = schema_groups;
    p.schema = schema;
    // What each mod does comes from the mod itself (its AtmtModDescription, dumped into its schema.json).
    for (const Json& j : p.schema["mods"].elements()) {
        for (ModInfo& m : p.mods) {
            if (IEquals(m.name, j.Str("mod"))) m.description = j.Str("description");
        }
    }
    p.components = std::move(components);
    *out = std::move(p);
    return true;
}

bool Payload::Load(const fs::path& root, Payload* out, std::string* error) {
    std::vector<Component> components;
    std::error_code ec;
    for (fs::directory_iterator it(root, ec), end; !ec && it != end; it.increment(ec)) {
        if (!it->is_directory() || !Exists(it->path() / "component.json")) continue;
        Component c;
        if (!Component::Load(it->path(), &c, error)) return false;
        components.push_back(std::move(c));
    }
    if (components.empty()) {
        if (error != nullptr) *error = U8(root) + ": no components";
        return false;
    }
    std::string why;
    if (!FromComponents(std::move(components), out, &why)) {
        if (error != nullptr) *error = U8(root) + ": " + why;
        return false;
    }
    return true;
}

const Component* Payload::FindComponent(const std::string& name) const {
    for (const Component& c : components) {
        if (c.name == name) return &c;
    }
    return nullptr;
}

const ModInfo* Payload::FindMod(const std::string& name) const {
    for (const ModInfo& m : mods) {
        if (IEquals(m.name, name)) return &m;
    }
    return nullptr;
}

const PayloadFile* Payload::FindFile(const std::string& rel) const {
    for (const PayloadFile& f : files) {
        if (f.rel == rel) return &f;
    }
    return nullptr;
}

std::string Payload::ExpectedMd5(const std::string& rel) const {
    const PayloadFile* f = FindFile(rel);
    return f != nullptr ? f->md5 : std::string();
}

bool Payload::IsSupportedExe(const std::string& md5) const {
    for (const SupportedExe& s : supported) {
        if (s.md5 == Lower(md5)) return true;
    }
    return false;
}

std::map<std::string, std::string> Payload::Versions() const {
    std::map<std::string, std::string> out;
    for (const Component& c : components) out[c.name] = c.version;
    return out;
}

std::string Payload::Summary() const {
    const Component* loader = FindComponent(kLoaderComponent);
    return std::string("loader ") + (loader != nullptr ? loader->version : "-") + ", " + std::to_string(mods.size())
           + (mods.size() == 1 ? " mod" : " mods");
}

bool Payload::VerifyFiles(std::string* error) const {
    for (const Component& c : components) {
        if (!c.VerifyFiles(error)) return false;
    }
    return true;
}

// ---------------------------------------------------------------- where payloads are
std::vector<fs::path> PayloadRoots() {
    std::vector<fs::path> out;
    const std::string env = GetEnv("ATMT_PAYLOAD");
    if (!env.empty()) out.push_back(Path(env));
    const fs::path self = SelfDir();
    out.push_back(self / "payload");
    out.push_back(self.parent_path() / "share" / "atmt_manager" / "payload");
    return out;
}

fs::path DownloadedComponentsDir() { return DataDir() / "components"; }

std::vector<Component> AvailableComponents() {
    std::vector<fs::path> dirs;
    for (const fs::path& root : PayloadRoots()) {
        std::error_code ec;
        for (fs::directory_iterator it(root, ec), end; !ec && it != end; it.increment(ec)) {
            if (it->is_directory()) dirs.push_back(it->path());
        }
    }
    std::error_code ec;
    for (fs::directory_iterator it(DownloadedComponentsDir(), ec), end; !ec && it != end; it.increment(ec)) {
        if (!it->is_directory()) continue;
        std::error_code vec;
        for (fs::directory_iterator v(it->path(), vec), vend; !vec && v != vend; v.increment(vec)) {
            // ".<version>.tmp": a download that did not finish
            if (v->is_directory() && !StartsWith(U8(v->path().filename()), ".")) dirs.push_back(v->path());
        }
    }
    std::vector<Component> out;
    for (const fs::path& dir : dirs) {
        Component c;
        if (Exists(dir / "component.json") && Component::Load(dir, &c)) out.push_back(std::move(c));
    }
    return out;
}

bool FindBestPayload(Payload* out, std::string* error) {
    std::map<std::string, Component> best;
    std::string needs_app;
    for (Component& c : AvailableComponents()) {
        if (!c.Usable()) {
            needs_app = c.name + " " + c.version + " needs app " + c.min_manager_version + " or newer";
            continue;
        }
        auto it = best.find(c.name);
        if (it == best.end() || CompareVersions(c.version, it->second.version) > 0) best[c.name] = std::move(c);
    }
    if (best.count(kLoaderComponent) == 0) {
        if (error != nullptr) {
            *error = needs_app.empty() ? "no payload found (looked next to the app and in the downloaded components)"
                                       : needs_app;
        }
        return false;
    }
    std::vector<Component> chosen;
    for (auto& kv : best) chosen.push_back(std::move(kv.second));
    return Payload::FromComponents(std::move(chosen), out, error);
}

}  // namespace atmt
