// steam_shortcuts.cpp - see steam_shortcuts.h.
#include "steam_shortcuts.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <thread>

#include "app_icon.h"
#include "archive.h"
#include "locator.h"
#include "platform.h"

namespace atmt {

namespace {

class Reader {
public:
    explicit Reader(const std::string& d) : d_(d) {}

    bool Map(BinVdf* out, int depth) {
        if (depth > 32) return Fail("nested too deep");
        for (;;) {
            if (pos_ >= d_.size()) return Fail("unexpected end");
            const uint8_t type = static_cast<uint8_t>(d_[pos_++]);
            if (type == 8) return true;   // end of this map
            BinVdf node;
            node.type = type;
            if (!CStr(&node.name)) return false;
            switch (type) {
                case BinVdf::Map:
                    if (!Map(&node, depth + 1)) return false;
                    break;
                case BinVdf::String:
                    if (!CStr(&node.str)) return false;
                    break;
                case BinVdf::WString: {   // UTF-16, NUL-terminated
                    size_t start = pos_;
                    while (pos_ + 1 < d_.size() && (d_[pos_] != 0 || d_[pos_ + 1] != 0)) pos_ += 2;
                    if (pos_ + 1 >= d_.size()) return Fail("unterminated wide string");
                    node.str = d_.substr(start, pos_ - start);
                    pos_ += 2;
                    break;
                }
                case BinVdf::Int32:
                case BinVdf::Float32:
                case BinVdf::Ptr:
                case BinVdf::Color:
                    if (pos_ + 4 > d_.size()) return Fail("unexpected end");
                    std::memcpy(&node.u32, d_.data() + pos_, 4);
                    pos_ += 4;
                    break;
                case BinVdf::UInt64:
                    if (pos_ + 8 > d_.size()) return Fail("unexpected end");
                    std::memcpy(&node.u64, d_.data() + pos_, 8);
                    pos_ += 8;
                    break;
                default:
                    return Fail("unknown value type " + std::to_string(type));
            }
            out->children.push_back(std::move(node));
        }
    }

    size_t pos() const { return pos_; }
    const std::string& why() const { return why_; }

private:
    bool Fail(const std::string& why) {
        why_ = why;
        return false;
    }
    bool CStr(std::string* out) {
        const size_t end = d_.find('\0', pos_);
        if (end == std::string::npos) return Fail("unterminated string");
        *out = d_.substr(pos_, end - pos_);
        pos_ = end + 1;
        return true;
    }
    const std::string& d_;
    size_t pos_ = 0;
    std::string why_;
};

void WriteMap(const BinVdf& m, std::string& out) {
    for (const BinVdf& c : m.children) {
        out += static_cast<char>(c.type);
        out += c.name;
        out += '\0';
        switch (c.type) {
            case BinVdf::Map:
                WriteMap(c, out);
                out += '\x08';
                break;
            case BinVdf::String:
                out += c.str;
                out += '\0';
                break;
            case BinVdf::WString:
                out += c.str;
                out += std::string(2, '\0');
                break;
            case BinVdf::UInt64:
                out.append(reinterpret_cast<const char*>(&c.u64), 8);
                break;
            default:
                out.append(reinterpret_cast<const char*>(&c.u32), 4);
                break;
        }
    }
}

BinVdf Str(const char* name, const std::string& v) {
    BinVdf n;
    n.type = BinVdf::String;
    n.name = name;
    n.str = v;
    return n;
}

BinVdf Int(const char* name, uint32_t v) {
    BinVdf n;
    n.type = BinVdf::Int32;
    n.name = name;
    n.u32 = v;
    return n;
}

bool IsOurs(const BinVdf& entry) {
    const BinVdf* name = entry.Find("AppName");
    if (name == nullptr) name = entry.Find("appname");
    return name != nullptr && name->str == kShortcutName;
}

// shortcuts.vdf: {"shortcuts": {"0": {...}, "1": {...}}}. Missing or empty is an empty list.
bool Load(const fs::path& file, BinVdf* root, std::string* error) {
    *root = BinVdf();
    std::string data;
    if (!ReadFile(file, &data) || data.empty()) {
        BinVdf list;
        list.type = BinVdf::Map;
        list.name = "shortcuts";
        root->children.push_back(list);
        return true;
    }
    if (!ParseBinVdf(data, root, error)) return false;
    if (root->Find("shortcuts") == nullptr) {
        if (error != nullptr) *error = U8(file) + " has no shortcuts list";
        return false;
    }
    return true;
}

bool Save(const fs::path& file, const BinVdf& root, std::string* error) {
    const fs::path bak = fs::path(file).concat(".atmt_bak");
    if (Exists(file) && !Exists(bak)) {
        std::error_code ec;
        fs::copy_file(file, bak, ec);
    }
    return WriteFileAtomic(file, WriteBinVdf(root), error);
}

void Renumber(BinVdf* list) {
    for (size_t i = 0; i < list->children.size(); ++i) list->children[i].name = std::to_string(i);
}

std::string Quoted(const fs::path& p) { return "\"" + U8(p) + "\""; }

}  // namespace

BinVdf* BinVdf::Find(const std::string& key) {
    for (BinVdf& c : children) {
        if (IEquals(c.name, key)) return &c;
    }
    return nullptr;
}

const BinVdf* BinVdf::Find(const std::string& key) const {
    for (const BinVdf& c : children) {
        if (IEquals(c.name, key)) return &c;
    }
    return nullptr;
}

bool ParseBinVdf(const std::string& data, BinVdf* root, std::string* error) {
    Reader r(data);
    *root = BinVdf();
    // The top level ends with its own 0x08, like a map does.
    if (!r.Map(root, 0)) {
        if (error != nullptr) *error = "shortcuts.vdf: " + r.why();
        return false;
    }
    return true;
}

std::string WriteBinVdf(const BinVdf& root) {
    std::string out;
    WriteMap(root, out);
    out += '\x08';
    return out;
}

fs::path WriteAppIcon() {
    const fs::path file = DataDir() / "atmt-manager.png";
    const std::string png(reinterpret_cast<const char*>(kAppIconPng), sizeof(kAppIconPng));
    std::string have;
    if (ReadFile(file, &have) && have == png) return file;
    return WriteFileAtomic(file, png) ? file : fs::path();
}

uint32_t ShortcutAppId(const std::string& exe_quoted, const std::string& name) {
    return Crc32(exe_quoted + name) | 0x80000000u;
}

std::vector<SteamAccount> SteamAccounts() {
    std::vector<SteamAccount> out;
    for (const fs::path& root : SteamRoots()) {
        std::error_code ec;
        for (fs::directory_iterator it(root / "userdata", ec), end; !ec && it != end; it.increment(ec)) {
            const std::string id = U8(it->path().filename());
            if (id.empty() || id == "0" || id.find_first_not_of("0123456789") != std::string::npos) continue;
            if (!IsDir(it->path() / "config")) continue;
            SteamAccount a;
            a.config_dir = it->path() / "config";
            a.id = id;
            a.has_shortcut = HasShortcut(a.config_dir / "shortcuts.vdf");
            bool dup = false;
            for (const SteamAccount& o : out) dup = dup || fs::equivalent(o.config_dir, a.config_dir, ec);
            if (!dup) out.push_back(a);
        }
    }
    return out;
}

bool InSteamLibrary() {
    for (const SteamAccount& a : SteamAccounts()) {
        if (a.has_shortcut) return true;
    }
    return false;
}

bool SteamRunning() { return ProcessRunning("steam") || ProcessRunning("steam.exe"); }

bool CloseSteam(int timeout_ms, std::string* error) {
    if (!SteamRunning()) return true;
    if (IsSteamGameMode()) {
        if (error != nullptr) *error = "in Game Mode Steam cannot be closed - switch to Desktop Mode for this";
        return false;
    }
#ifdef _WIN32
    bool asked = false;
    for (const fs::path& root : SteamRoots()) {
        if (Exists(root / "steam.exe")) {
            asked = Launch(root / "steam.exe", {"-shutdown"});
            break;
        }
    }
#else
    bool asked = false;
    for (const char* exe : {"/usr/bin/steam", "/usr/games/steam", "/app/bin/steam"}) {
        if (Exists(exe)) {
            asked = Launch(exe, {"-shutdown"});
            break;
        }
    }
    if (!asked) asked = Launch("/usr/bin/flatpak", {"run", "com.valvesoftware.Steam", "-shutdown"});
#endif
    if (!asked) {
        if (error != nullptr) *error = "could not ask Steam to close - close it by hand";
        return false;
    }
    for (int waited = 0; waited < timeout_ms; waited += 500) {
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        if (!SteamRunning()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1500));   // its files are written on the way out
            return true;
        }
    }
    if (error != nullptr) *error = "Steam did not close in time - close it by hand and try again";
    return false;
}

bool StartSteam() {
#ifdef _WIN32
    for (const fs::path& root : SteamRoots()) {
        if (Exists(root / "steam.exe")) return Launch(root / "steam.exe", {});
    }
    return false;
#else
    for (const char* exe : {"/usr/bin/steam", "/usr/games/steam"}) {
        if (Exists(exe)) return Launch(exe, {});
    }
    return Launch("/usr/bin/flatpak", {"run", "com.valvesoftware.Steam"});
#endif
}

bool HasShortcut(const fs::path& vdf) {
    BinVdf root;
    if (!Load(vdf, &root, nullptr)) return false;
    for (const BinVdf& e : root.Find("shortcuts")->children) {
        if (IsOurs(e)) return true;
    }
    return false;
}

bool AddShortcut(const fs::path& file, const fs::path& exe, const fs::path& icon, std::string* error) {
    BinVdf root;
    if (!Load(file, &root, error)) return false;
    BinVdf* shortcuts = root.Find("shortcuts");
    std::vector<BinVdf>& entries = shortcuts->children;
    for (size_t i = entries.size(); i-- > 0;) {
        if (IsOurs(entries[i])) entries.erase(entries.begin() + static_cast<std::ptrdiff_t>(i));
    }
    const std::string exe_q = Quoted(exe);
    BinVdf e;
    e.type = BinVdf::Map;
    e.children.push_back(Int("appid", ShortcutAppId(exe_q, kShortcutName)));
    e.children.push_back(Str("AppName", kShortcutName));
    e.children.push_back(Str("Exe", exe_q));
    e.children.push_back(Str("StartDir", Quoted(exe.parent_path())));
    e.children.push_back(Str("icon", icon.empty() ? std::string() : U8(icon)));
    e.children.push_back(Str("ShortcutPath", ""));
    e.children.push_back(Str("LaunchOptions", ""));
    e.children.push_back(Int("IsHidden", 0));
    e.children.push_back(Int("AllowDesktopConfig", 1));
    e.children.push_back(Int("AllowOverlay", 1));
    e.children.push_back(Int("OpenVR", 0));
    e.children.push_back(Int("Devkit", 0));
    e.children.push_back(Str("DevkitGameID", ""));
    e.children.push_back(Int("DevkitOverrideAppID", 0));
    e.children.push_back(Int("LastPlayTime", 0));
    e.children.push_back(Str("FlatpakAppID", ""));
    BinVdf tags;
    tags.type = BinVdf::Map;
    tags.name = "tags";
    e.children.push_back(tags);
    entries.push_back(e);
    Renumber(shortcuts);
    return Save(file, root, error);
}

bool RemoveShortcut(const fs::path& file, bool* removed, std::string* error) {
    if (removed != nullptr) *removed = false;
    BinVdf root;
    if (!Load(file, &root, error)) return false;
    BinVdf* shortcuts = root.Find("shortcuts");
    std::vector<BinVdf>& entries = shortcuts->children;
    const size_t before = entries.size();
    for (size_t i = entries.size(); i-- > 0;) {
        if (IsOurs(entries[i])) entries.erase(entries.begin() + static_cast<std::ptrdiff_t>(i));
    }
    if (entries.size() == before) return true;
    if (removed != nullptr) *removed = true;
    Renumber(shortcuts);
    return Save(file, root, error);
}

fs::path DeckControllerConfig(const SteamAccount& a) {
    const fs::path root = a.config_dir.parent_path().parent_path().parent_path();   // <steam>/userdata/<id>/config
    return root / "steamapps" / "common" / "Steam Controller Configs" / a.id / "config" / "configset_controller_neptune.vdf";
}

namespace {

// configset_controller_neptune.vdf: {"controller_config": {"<app id or lower-case name>": {"template": ...}}}
bool LoadConfigset(const fs::path& file, Vdf* root, Vdf** apps, std::string* error) {
    std::string text;
    *root = Vdf();
    root->is_block = true;
    if (ReadFile(file, &text)) {
        if (text.compare(0, 3, "\xEF\xBB\xBF") == 0) text = text.substr(3);
        if (!ParseVdf(text, root)) {
            if (error != nullptr) *error = "cannot read " + U8(file);
            return false;
        }
    }
    for (auto& kv : root->children) {
        if (IEquals(kv.first, "controller_config") && kv.second.is_block) {
            *apps = &kv.second;
            return true;
        }
    }
    Vdf block;
    block.is_block = true;
    root->children.emplace_back("controller_config", block);
    *apps = &root->children.back().second;
    return true;
}

}  // namespace

bool SetDeckControllerLayout(const fs::path& file, std::string* error) {
    Vdf root;
    Vdf* apps = nullptr;
    if (!LoadConfigset(file, &root, &apps, error)) return false;
    const std::string key = Lower(kShortcutName);
    if (apps->Find(key) != nullptr) return true;   // the player's choice (or ours from before) stays
    Vdf entry;
    entry.is_block = true;
    Vdf value;
    value.value = kDeckControllerTemplate;
    entry.children.emplace_back("template", value);
    apps->children.emplace_back(key, entry);
    return WriteFileAtomic(file, WriteVdf(root), error);
}

bool ClearDeckControllerLayout(const fs::path& file, std::string* error) {
    if (!Exists(file)) return true;
    Vdf root;
    Vdf* apps = nullptr;
    if (!LoadConfigset(file, &root, &apps, error)) return false;
    const std::string key = Lower(kShortcutName);
    auto& c = apps->children;
    const size_t before = c.size();
    c.erase(std::remove_if(c.begin(), c.end(),
                           [&](const std::pair<std::string, Vdf>& kv) {
                               return IEquals(kv.first, key) && kv.second.Str("template") == kDeckControllerTemplate
                                      && kv.second.children.size() == 1;
                           }),
            c.end());
    if (c.size() == before) return true;
    return WriteFileAtomic(file, WriteVdf(root), error);
}

bool AddToSteam(const fs::path& exe, const fs::path& icon, int* accounts, std::string* error) {
    if (SteamRunning()) {
        if (error != nullptr) *error = "Steam is running: it would overwrite the change when it exits - close it first";
        return false;
    }
    const std::vector<SteamAccount> list = SteamAccounts();
    if (list.empty()) {
        if (error != nullptr) *error = "no Steam account on this machine (log in to Steam once first)";
        return false;
    }
    int done = 0;
    for (const SteamAccount& a : list) {
        if (!AddShortcut(a.config_dir / "shortcuts.vdf", exe, icon, error)) return false;
        Log("Steam: added \"" + std::string(kShortcutName) + "\" to account " + a.id);
#ifndef _WIN32
        std::string why;   // a missing layout is not worth failing the entry over: the player can pick one
        if (!SetDeckControllerLayout(DeckControllerConfig(a), &why)) Log("  its Deck controller layout was not set: " + why);
#endif
        ++done;
    }
    if (accounts != nullptr) *accounts = done;
    return true;
}

bool RemoveFromSteam(int* accounts, std::string* error) {
    if (SteamRunning()) {
        if (error != nullptr) *error = "Steam is running: it would put the entry back when it exits - close it first";
        return false;
    }
    int done = 0;
    for (const SteamAccount& a : SteamAccounts()) {
        bool removed = false;
        if (!RemoveShortcut(a.config_dir / "shortcuts.vdf", &removed, error)) return false;
#ifndef _WIN32
        ClearDeckControllerLayout(DeckControllerConfig(a));
#endif
        if (!removed) continue;
        Log("Steam: removed \"" + std::string(kShortcutName) + "\" from account " + a.id);
        ++done;
    }
    if (accounts != nullptr) *accounts = done;
    return true;
}

}  // namespace atmt
