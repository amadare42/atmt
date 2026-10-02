// locator.cpp - see locator.h.
#include "locator.h"

#include <algorithm>
#include <cctype>

#include "json.h"
#include "platform.h"

namespace atmt {

namespace {

class VdfParser {
public:
    explicit VdfParser(const std::string& s) : s_(s) {}

    bool Block(Vdf* out, bool top) {
        out->is_block = true;
        for (;;) {
            std::string key;
            const int t = Token(&key);
            if (t == kEnd) return top;
            if (t == kClose) return !top;
            if (t != kString) return false;
            std::string value;
            const int v = Token(&value);
            Vdf child;
            if (v == kOpen) {
                if (!Block(&child, false)) return false;
            } else if (v == kString) {
                child.value = value;
            } else {
                return false;
            }
            out->children.emplace_back(key, std::move(child));
        }
    }

private:
    enum { kEnd, kOpen, kClose, kString };

    void Skip() {
        for (;;) {
            while (pos_ < s_.size() && (s_[pos_] == ' ' || s_[pos_] == '\t' || s_[pos_] == '\r' || s_[pos_] == '\n')) ++pos_;
            if (s_.compare(pos_, 2, "//") == 0) {
                while (pos_ < s_.size() && s_[pos_] != '\n') ++pos_;
                continue;
            }
            // "[$WIN32]"-style conditionals after a value: ignored
            if (pos_ < s_.size() && s_[pos_] == '[') {
                while (pos_ < s_.size() && s_[pos_] != ']') ++pos_;
                ++pos_;
                continue;
            }
            return;
        }
    }

    int Token(std::string* out) {
        Skip();
        if (pos_ >= s_.size()) return kEnd;
        const char c = s_[pos_];
        if (c == '{') {
            ++pos_;
            return kOpen;
        }
        if (c == '}') {
            ++pos_;
            return kClose;
        }
        if (c == '"') {
            ++pos_;
            while (pos_ < s_.size() && s_[pos_] != '"') {
                if (s_[pos_] == '\\' && pos_ + 1 < s_.size()) {
                    const char e = s_[pos_ + 1];
                    *out += e == 'n' ? '\n' : e == 't' ? '\t' : e;
                    pos_ += 2;
                    continue;
                }
                *out += s_[pos_++];
            }
            ++pos_;
            return kString;
        }
        while (pos_ < s_.size() && s_[pos_] != ' ' && s_[pos_] != '\t' && s_[pos_] != '\r' && s_[pos_] != '\n'
               && s_[pos_] != '{' && s_[pos_] != '}' && s_[pos_] != '"') {
            *out += s_[pos_++];
        }
        return kString;
    }

    const std::string& s_;
    size_t pos_ = 0;
};

void AddUnique(std::vector<fs::path>& list, const fs::path& p) {
    if (!IsDir(p)) return;
    std::error_code ec;
    const fs::path canon = fs::weakly_canonical(p, ec);
    const fs::path key = ec ? p : canon;
    for (const fs::path& existing : list) {
        std::error_code ec2;
        if (fs::equivalent(existing, key, ec2)) return;
    }
    list.push_back(key);
}

std::vector<fs::path> LibrariesFromVdf(const fs::path& vdf_path) {
    std::vector<fs::path> out;
    std::string text;
    if (!ReadFile(vdf_path, &text)) return out;
    Vdf root;
    if (!ParseVdf(text, &root)) return out;
    for (const auto& top : root.children) {   // "libraryfolders" / "LibraryFolders"
        for (const auto& entry : top.second.children) {
            if (entry.second.is_block) {
                const std::string p = entry.second.Str("path");
                if (!p.empty()) out.push_back(Path(p));
            } else if (!entry.first.empty() && entry.first[0] >= '0' && entry.first[0] <= '9') {
                out.push_back(Path(entry.second.value));   // the old format: "1" "D:\\SteamLibrary"
            }
        }
    }
    return out;
}

}  // namespace

const Vdf* Vdf::Find(const std::string& key) const {
    for (const auto& kv : children) {
        if (IEquals(kv.first, key)) return &kv.second;
    }
    return nullptr;
}

std::string Vdf::Str(const std::string& key) const {
    const Vdf* v = Find(key);
    return (v != nullptr && !v->is_block) ? v->value : std::string();
}

bool ParseVdf(const std::string& text, Vdf* out) {
    VdfParser p(text);
    *out = Vdf();
    return p.Block(out, true);
}

namespace {

std::string VdfQuote(const std::string& s) {
    std::string out = "\"";
    for (char c : s) {
        if (c == '"' || c == '\\') out += '\\';
        out += c;
    }
    return out + "\"";
}

void WriteVdfBlock(const Vdf& v, int depth, std::string* out) {
    const std::string tabs(static_cast<size_t>(depth), '\t');
    for (const auto& kv : v.children) {
        if (kv.second.is_block) {
            *out += tabs + VdfQuote(kv.first) + "\n" + tabs + "{\n";
            WriteVdfBlock(kv.second, depth + 1, out);
            *out += tabs + "}\n";
        } else {
            *out += tabs + VdfQuote(kv.first) + "\t\t" + VdfQuote(kv.second.value) + "\n";
        }
    }
}

}  // namespace

std::string WriteVdf(const Vdf& root) {
    std::string out;
    WriteVdfBlock(root, 0, &out);
    return out;
}

bool HasGameExe(const fs::path& dir) { return Exists(dir / kGameExe); }

std::vector<fs::path> SteamRoots() {
    std::vector<fs::path> roots;
#ifdef _WIN32
    const std::string reg = SteamPathFromRegistry();
    if (!reg.empty()) AddUnique(roots, Path(reg));
    AddUnique(roots, Path("C:\\Program Files (x86)\\Steam"));
    AddUnique(roots, Path("C:\\Program Files\\Steam"));
#else
    const fs::path home = HomeDir();
    AddUnique(roots, home / ".local/share/Steam");
    AddUnique(roots, home / ".steam/steam");
    AddUnique(roots, home / ".steam/root");
    AddUnique(roots, home / ".var/app/com.valvesoftware.Steam/.local/share/Steam");   // Flatpak
    AddUnique(roots, home / ".var/app/com.valvesoftware.Steam/data/Steam");
#endif
    return roots;
}

std::vector<fs::path> SteamLibraries(const std::vector<fs::path>& roots) {
    std::vector<fs::path> libs;
    for (const fs::path& root : roots) {
        AddUnique(libs, root);
        for (const fs::path& vdf : {root / "steamapps" / "libraryfolders.vdf", root / "config" / "libraryfolders.vdf"}) {
            for (const fs::path& lib : LibrariesFromVdf(vdf)) AddUnique(libs, lib);
        }
    }
#ifndef _WIN32
    // SD cards and USB drives: SteamOS mounts them under /run/media/<user>/<label> (older releases
    // /run/media/mmcblk0p1); a library there is listed in libraryfolders.vdf too, this only catches
    // one Steam has not been told about yet.
    std::error_code ec;
    for (fs::directory_iterator a("/run/media", ec), end; !ec && a != end; a.increment(ec)) {
        if (Exists(a->path() / "steamapps")) AddUnique(libs, a->path());
        std::error_code ec2;
        for (fs::directory_iterator b(a->path(), ec2); !ec2 && b != end; b.increment(ec2)) {
            if (Exists(b->path() / "steamapps")) AddUnique(libs, b->path());
        }
    }
#endif
    return libs;
}

GameInstall DescribeGameDir(const fs::path& dir, const std::vector<GameInstall>& known) {
    for (const GameInstall& k : known) {
        std::error_code ec;
        if (fs::equivalent(k.dir, dir, ec)) return k;
    }
    GameInstall g;
    std::error_code ec;
    g.dir = fs::weakly_canonical(dir, ec);
    if (ec) g.dir = dir;
    // <library>/steamapps/common/<installdir>
    const fs::path common = g.dir.parent_path();
    if (IEquals(U8(common.filename()), "common") && IEquals(U8(common.parent_path().filename()), "steamapps")) {
        g.source = "Steam";
        g.library = common.parent_path().parent_path();
        g.prefix = g.library / "steamapps" / "compatdata" / kSteamAppId / "pfx";
        g.prefix_exists = IsDir(g.prefix);
        return g;
    }
#ifndef _WIN32
    // <prefix>/drive_c/...: a game installed inside a Wine prefix (Lutris, Bottles, plain Wine)
    for (fs::path p = g.dir; !p.empty() && p != p.parent_path(); p = p.parent_path()) {
        if (IEquals(U8(p.filename()), "drive_c")) {
            g.prefix = p.parent_path();
            g.prefix_exists = IsDir(g.prefix);
            break;
        }
    }
#endif
    return g;
}

std::vector<GameInstall> FindGames(const std::vector<fs::path>& libraries) {
    std::vector<GameInstall> out;
    std::vector<fs::path> seen;
    for (const fs::path& lib : libraries) {
        std::vector<fs::path> candidates;
        std::string acf;
        if (ReadFile(lib / "steamapps" / (std::string("appmanifest_") + kSteamAppId + ".acf"), &acf)) {
            Vdf root;
            if (ParseVdf(acf, &root)) {
                const Vdf* state = root.Find("AppState");
                const std::string installdir = state != nullptr ? state->Str("installdir") : std::string();
                if (!installdir.empty()) candidates.push_back(lib / "steamapps" / "common" / Path(installdir));
            }
        }
        candidates.push_back(lib / "steamapps" / "common" / kGameInstallDir);
        for (const fs::path& c : candidates) {
            if (!HasGameExe(c)) continue;
            bool dup = false;
            for (const fs::path& s : seen) {
                std::error_code ec;
                if (fs::equivalent(s, c, ec)) dup = true;
            }
            if (dup) continue;
            seen.push_back(c);
            GameInstall g = DescribeGameDir(c);
            g.source = "Steam";
            g.library = lib;
            g.prefix = lib / "steamapps" / "compatdata" / kSteamAppId / "pfx";
            g.prefix_exists = IsDir(g.prefix);
            out.push_back(g);
        }
    }
    return out;
}

// ---------------------------------------------------------------- the other stores
namespace {

std::string Unquote(std::string s) {
    s = Trim(s);
    if (s.size() >= 2 && (s.front() == '"' || s.front() == '\'') && s.back() == s.front()) s = s.substr(1, s.size() - 2);
    return s;
}

// The game folder a store's install path stands for: the path itself, ed8.exe's folder when the
// path is the exe, or a sub-folder with ed8.exe (one level).
fs::path GameDirIn(const fs::path& p) {
    if (p.empty()) return fs::path();
    if (IEquals(U8(p.filename()), kGameExe) && Exists(p) && !IsDir(p)) return p.parent_path();
    if (HasGameExe(p)) return p;
    std::error_code ec;
    for (fs::directory_iterator it(p, ec), end; !ec && it != end; it.increment(ec)) {
        if (it->is_directory() && HasGameExe(it->path())) return it->path();
    }
    return fs::path();
}

void AddGame(std::vector<GameInstall>& out, const fs::path& install_path, const std::string& source,
             const fs::path& prefix = fs::path()) {
    const fs::path dir = GameDirIn(install_path);
    if (dir.empty()) return;
    GameInstall g = DescribeGameDir(dir);
    g.source = source;
    if (!prefix.empty()) {
        g.prefix = prefix;
        g.prefix_exists = IsDir(prefix);
    }
    MergeGames(out, {g});
}

// Heroic: GamesConfig/<app>.json {"<app>": {"winePrefix": "..."}}
fs::path HeroicPrefix(const fs::path& heroic, const std::string& app) {
    if (heroic.empty() || app.empty()) return fs::path();
    std::string text;
    Json j;
    if (!ReadFile(heroic / "GamesConfig" / Path(app + ".json"), &text) || !Json::Parse(text, &j)) return fs::path();
    const std::string prefix = j[app].Str("winePrefix");
    return prefix.empty() ? fs::path() : Path(prefix);
}

std::vector<fs::path> ChildDirs(const fs::path& parent) {
    std::vector<fs::path> out;
    std::error_code ec;
    for (fs::directory_iterator it(parent, ec), end; !ec && it != end; it.increment(ec)) {
        if (it->is_directory()) out.push_back(it->path());
    }
    std::sort(out.begin(), out.end());
    return out;
}

}  // namespace

void MergeGames(std::vector<GameInstall>& list, const std::vector<GameInstall>& more) {
    for (const GameInstall& g : more) {
        bool dup = false;
        for (const GameInstall& e : list) {
            std::error_code ec;
            if (fs::equivalent(e.dir, g.dir, ec)) dup = true;
        }
        if (!dup) list.push_back(g);
    }
}

fs::path WinePath(const fs::path& prefix, const std::string& windows_path) {
#ifdef _WIN32
    (void)prefix;
    return Path(windows_path);
#else
    std::string p = windows_path;
    for (char& c : p) {
        if (c == '\\') c = '/';
    }
    if (p.size() >= 2 && p[1] == ':') {
        const char drive = static_cast<char>(std::tolower(static_cast<unsigned char>(p[0])));
        std::string rest = p.substr(2);
        while (!rest.empty() && rest[0] == '/') rest = rest.substr(1);
        // dosdevices/<x>: links to wherever that drive is; drive_c is the usual c:
        const fs::path base = drive == 'c' ? prefix / "drive_c" : prefix / "dosdevices" / (std::string(1, drive) + ":");
        return rest.empty() ? base : base / Path(rest);
    }
    return Path(p);
#endif
}

std::vector<GameInstall> FindHeroicGames(const fs::path& heroic) {
    std::vector<GameInstall> out;
    std::string text;
    Json j;
    // GOG: {"installed": [{"appName", "install_path", "platform"}]}
    if (ReadFile(heroic / "gog_store" / "installed.json", &text) && Json::Parse(text, &j)) {
        for (const Json& e : j["installed"].elements()) {
            const std::string path = e.Str("install_path");
            if (!path.empty()) AddGame(out, Path(path), "Heroic (GOG)", HeroicPrefix(heroic, e.Str("appName")));
        }
    }
    // games added to Heroic by hand: sideload_apps/library.json {"games": [{"app_name", "install": {"executable"}}]}
    if (ReadFile(heroic / "sideload_apps" / "library.json", &text) && Json::Parse(text, &j)) {
        for (const Json& e : j["games"].elements()) {
            const std::string exe = e["install"].Str("executable");
            if (!exe.empty()) AddGame(out, Path(exe), "Heroic", HeroicPrefix(heroic, e.Str("app_name")));
        }
    }
    return out;
}

std::vector<GameInstall> FindLutrisGames(const fs::path& games_dir) {
    std::vector<GameInstall> out;
    std::error_code ec;
    std::vector<fs::path> ymls;
    for (fs::directory_iterator it(games_dir, ec), end; !ec && it != end; it.increment(ec)) {
        const std::string n = Lower(U8(it->path().filename()));
        if (EndsWith(n, ".yml") || EndsWith(n, ".yaml")) ymls.push_back(it->path());
    }
    std::sort(ymls.begin(), ymls.end());
    for (const fs::path& yml : ymls) {
        std::string text;
        if (!ReadFile(yml, &text)) continue;
        std::string exe, prefix;
        for (const std::string& raw : Lines(text)) {   // "game:\n  exe: ...\n  prefix: ..."
            const std::string line = Trim(raw);
            if (StartsWith(line, "exe:") && exe.empty()) exe = Unquote(line.substr(4));
            if (StartsWith(line, "prefix:") && prefix.empty()) prefix = Unquote(line.substr(7));
        }
        if (exe.empty()) continue;
        fs::path exe_path = Path(exe);
        if (exe_path.is_relative() && !prefix.empty()) exe_path = Path(prefix) / exe_path;
        AddGame(out, exe_path, "Lutris", prefix.empty() ? fs::path() : Path(prefix));
    }
    return out;
}

std::vector<GameInstall> FindGamesUnder(const fs::path& parent, const std::string& source) {
    std::vector<GameInstall> out;
    if (!IsDir(parent)) return out;
    if (HasGameExe(parent)) AddGame(out, parent, source);
    for (const fs::path& c : ChildDirs(parent)) {
        if (HasGameExe(c)) AddGame(out, c, source);
    }
    return out;
}

std::vector<GameInstall> FindGamesInPrefix(const fs::path& prefix, const std::string& source) {
    std::vector<GameInstall> out;
    const fs::path c = prefix / "drive_c";
    if (!IsDir(c)) return out;
    const fs::path pf86 = c / "Program Files (x86)";
    const fs::path pf = c / "Program Files";
    for (const fs::path& parent : {c / "GOG Games", pf86 / "GOG Galaxy" / "Games", pf / "GOG Galaxy" / "Games",
                                   pf86 / "GOG.com", c / "Games", pf86, pf,
                                   pf86 / "Steam" / "steamapps" / "common", pf / "Steam" / "steamapps" / "common"}) {
        for (GameInstall g : FindGamesUnder(parent, source)) {
            g.prefix = prefix;
            g.prefix_exists = true;
            MergeGames(out, {g});
        }
    }
    return out;
}

std::vector<GameInstall> FindOtherGames() {
    std::vector<GameInstall> out;
#ifdef _WIN32
    for (const auto& kv : GogGamesFromRegistry()) AddGame(out, Path(kv.second), "GOG");
    const std::string appdata = GetEnv("APPDATA");
    if (!appdata.empty()) MergeGames(out, FindHeroicGames(Path(appdata) / "heroic"));
    std::vector<fs::path> program_files = {Path("C:\\Program Files (x86)"), Path("C:\\Program Files")};
    for (const char* env : {"ProgramFiles(x86)", "ProgramFiles"}) {
        const std::string v = GetEnv(env);
        if (!v.empty()) program_files.push_back(Path(v));
    }
    MergeGames(out, FindGamesUnder(Path("C:\\GOG Games"), "GOG"));
    for (const fs::path& pf : program_files) {
        MergeGames(out, FindGamesUnder(pf / "GOG Galaxy" / "Games", "GOG"));
        MergeGames(out, FindGamesUnder(pf / "GOG.com", "GOG"));
    }
#else
    const fs::path home = HomeDir();
    const std::string xdg_config = GetEnv("XDG_CONFIG_HOME");
    const fs::path config = xdg_config.empty() ? home / ".config" : Path(xdg_config);
    const std::string xdg_data = GetEnv("XDG_DATA_HOME");
    const fs::path data = xdg_data.empty() ? home / ".local" / "share" : Path(xdg_data);
    // Heroic (native and Flatpak)
    MergeGames(out, FindHeroicGames(config / "heroic"));
    MergeGames(out, FindHeroicGames(home / ".var/app/com.heroicgameslauncher.hgl/config/heroic"));
    // Lutris (older and newer config places, and Flatpak)
    for (const fs::path& d : {config / "lutris" / "games", data / "lutris" / "games",
                              home / ".var/app/net.lutris.Lutris/config/lutris/games",
                              home / ".var/app/net.lutris.Lutris/data/lutris/games"}) {
        MergeGames(out, FindLutrisGames(d));
    }
    // Wine prefixes: Bottles' bottles, Lutris's default ~/Games/<game>, Heroic's prefixes, ~/.wine
    for (const fs::path& d : {data / "bottles" / "bottles", home / ".var/app/com.usebottles.bottles/data/bottles/bottles"}) {
        for (const fs::path& b : ChildDirs(d)) MergeGames(out, FindGamesInPrefix(b, "Bottles"));
    }
    for (const fs::path& d : {home / "Games", home / "Games" / "Heroic" / "Prefixes" / "default"}) {
        for (const fs::path& b : ChildDirs(d)) MergeGames(out, FindGamesInPrefix(b, "Wine"));
    }
    MergeGames(out, FindGamesInPrefix(home / ".wine", "Wine"));
    // Heroic's default install folder (its installed.json normally lists these already)
    MergeGames(out, FindGamesUnder(home / "Games" / "Heroic", "Heroic"));
#endif
    return out;
}

std::vector<GameInstall> FindGames() {
    std::vector<GameInstall> out = FindGames(SteamLibraries(SteamRoots()));
    MergeGames(out, FindOtherGames());
    return out;
}

fs::path ResolveGameDir(const std::string& typed) {
    const std::string t = Unquote(typed);
    if (t.empty()) return fs::path();
    return GameDirIn(Path(t));
}

}  // namespace atmt
