// senpatcher.cpp - see senpatcher.h.
#include "senpatcher.h"

#include <algorithm>
#include <cctype>

#include "archive.h"
#include "hash.h"
#include "install.h"
#include "payload.h"
#include "platform.h"

namespace atmt {

namespace {

constexpr const char* kCopyRecord = "atmt_senpatcher.json";
constexpr const char* kCs1Folder = "Trails of Cold Steel";
// What SenPatcher.exe needs for CS1: itself, and the hook dll and the default settings it installs
// from "Trails of Cold Steel/" next to it (the other games' folders are left in the zip).
const char* const kCopyFiles[] = {"SenPatcher.exe", "LICENSE.txt", "README.txt"};
// The verb Steam starts games with: wait for the prefix's earlier processes, run, wait for it to end.
constexpr const char* kVerb = "waitforexitandrun";

bool SafeTag(const std::string& tag) {
    if (tag.empty() || tag[0] == '.') return false;
    for (char c : tag) {
        if (!std::isalnum(static_cast<unsigned char>(c)) && c != '.' && c != '-' && c != '_') return false;
    }
    return true;
}

}  // namespace

// ---------------------------------------------------------------- the release
bool ParseSenPatcherRelease(const Json& j, SenPatcherRelease* out, std::string* error) {
    auto fail = [&](const std::string& why) {
        if (error != nullptr) *error = why;
        return false;
    };
    SenPatcherRelease r;
    r.tag = j.Str("tag_name");
    r.page = j.Str("html_url");
    if (!SafeTag(r.tag)) return fail("SenPatcher's latest release has no usable tag ('" + r.tag + "')");
    for (const Json& a : j["assets"].elements()) {
        const std::string name = a.Str("name");
        if (!StartsWith(Lower(name), "senpatcher") || !EndsWith(Lower(name), ".zip")) continue;
        r.asset = name;
        r.url = a.Str("browser_download_url");
        r.size = static_cast<uint64_t>(a["size"].AsInt(0));
        const std::string digest = a.Str("digest");   // "sha256:<hex>"
        if (StartsWith(Lower(digest), "sha256:")) r.sha256 = Lower(digest.substr(7));
        break;
    }
    if (r.url.empty()) return fail("SenPatcher's latest release (" + r.tag + ") has no SenPatcher*.zip download");
    if (!StartsWith(r.url, "https://")) return fail("SenPatcher's download is not https: " + r.url);
    *out = r;
    return true;
}

bool FetchLatestSenPatcher(Http* http, SenPatcherRelease* out, std::string* error) {
    if (http == nullptr) {
        if (error != nullptr) *error = "no HTTPS client on this system";
        return false;
    }
    const std::string url = std::string("https://api.github.com/repos/") + kSenPatcherRepo + "/releases/latest";
    HttpResponse resp;
    if (!http->Get(url, {{"Accept", "application/vnd.github+json"}}, &resp) || resp.status != 200) {
        if (error != nullptr) *error = resp.error.empty() ? "HTTP " + std::to_string(resp.status) + " for " + url : resp.error;
        return false;
    }
    Json j;
    std::string why;
    if (!Json::Parse(resp.body, &j, &why)) {
        if (error != nullptr) *error = "GitHub's answer is not JSON: " + why;
        return false;
    }
    return ParseSenPatcherRelease(j, out, error);
}

fs::path SenPatcherRoot() { return DataDir() / "senpatcher"; }

SenPatcherCopy LocalSenPatcher() {
    SenPatcherCopy best;
    std::error_code ec;
    for (fs::directory_iterator it(SenPatcherRoot(), ec), end; !ec && it != end; it.increment(ec)) {
        const std::string tag = U8(it->path().filename());
        if (!it->is_directory() || !SafeTag(tag)) continue;
        std::string text;
        Json record;
        if (!ReadFile(it->path() / kCopyRecord, &text) || !Json::Parse(text, &record) || record.Str("tag") != tag) continue;
        if (!Exists(it->path() / "SenPatcher.exe")) continue;
        if (best.ok() && CompareVersions(tag, best.tag) <= 0) continue;
        best.tag = tag;
        best.dir = it->path();
        best.exe = it->path() / "SenPatcher.exe";
    }
    return best;
}

bool InstallSenPatcherZip(const SenPatcherRelease& release, const std::string& zip, SenPatcherCopy* out, std::string* error) {
    auto fail = [&](const std::string& why) {
        if (error != nullptr) *error = why;
        return false;
    };
    if (!SafeTag(release.tag)) return fail("not a usable release tag: '" + release.tag + "'");
    if (release.size != 0 && zip.size() != release.size) {
        return fail(release.asset + ": " + std::to_string(zip.size()) + " bytes, GitHub says " + std::to_string(release.size));
    }
    if (!release.sha256.empty() && Sha256Hex(zip) != release.sha256) {
        return fail(release.asset + " does not match the sha256 GitHub records for it");
    }
    std::vector<ZipEntry> entries;
    std::string why;
    if (!ListZip(zip, &entries, &why)) return fail(release.asset + ": " + why);
    // the files sit in one top folder ("SenPatcher-v1.3.1/"), or none
    std::string top;
    bool found = false;
    for (const ZipEntry& e : entries) {
        if (e.name == "SenPatcher.exe" || (EndsWith(e.name, "/SenPatcher.exe") && e.name.find('/') == e.name.size() - 15)) {
            top = e.name.substr(0, e.name.size() - 14);
            found = true;
            break;
        }
    }
    if (!found) return fail(release.asset + " has no SenPatcher.exe - its layout changed; download it by hand");
    const fs::path dest = SenPatcherRoot() / release.tag;
    const fs::path partial = SenPatcherRoot() / (release.tag + ".partial");
    std::error_code ec;
    fs::remove_all(partial, ec);
    bool dll = false;
    for (const ZipEntry& e : entries) {
        if (e.IsDir() || !StartsWith(e.name, top)) continue;
        const std::string rel = e.name.substr(top.size());
        bool wanted = StartsWith(rel, std::string(kCs1Folder) + "/");
        for (const char* f : kCopyFiles) wanted = wanted || rel == f;
        if (!wanted || rel.find("..") != std::string::npos || rel.find(':') != std::string::npos) continue;
        std::string data;
        if (!ReadZipEntry(zip, e, &data, &why)) return fail(release.asset + ": " + why);
        if (IEquals(rel, std::string(kCs1Folder) + "/DINPUT8.dll")) dll = data.find("SenPatcherHook") != std::string::npos;
        if (!WriteFileAtomic(partial / Path(rel), data, &why)) return fail(why);
    }
    if (!dll) {
        fs::remove_all(partial, ec);
        return fail(release.asset + " has no SenPatcher DINPUT8.dll for Trails of Cold Steel - its layout changed");
    }
    Json record = Json::MakeObject();
    record["tag"] = release.tag;
    record["asset"] = release.asset;
    record["url"] = release.url;
    record["sha256"] = Sha256Hex(zip);
    record["downloaded"] = IsoTimeUtc();
    if (!WriteFileAtomic(partial / kCopyRecord, record.Dump(), &why)) return fail(why);
    fs::remove_all(dest, ec);
    fs::rename(partial, dest, ec);
    if (ec) return fail("cannot move SenPatcher into " + U8(dest) + ": " + ec.message());
    // older copies (and leftovers of interrupted downloads) go
    for (fs::directory_iterator it(SenPatcherRoot(), ec), end; !ec && it != end; it.increment(ec)) {
        if (!it->is_directory() || it->path().filename() == dest.filename()) continue;
        std::error_code ec2;
        fs::remove_all(it->path(), ec2);
    }
    out->tag = release.tag;
    out->dir = dest;
    out->exe = dest / "SenPatcher.exe";
    return true;
}

bool EnsureSenPatcher(Http* http, SenPatcherCopy* out, std::string* note, std::string* error, const ProgressFn& progress) {
    const SenPatcherCopy local = LocalSenPatcher();
    SenPatcherRelease latest;
    std::string why;
    if (!FetchLatestSenPatcher(http, &latest, &why)) {
        if (!local.ok()) {
            if (error != nullptr) *error = "SenPatcher's latest release could not be found: " + why;
            return false;
        }
        if (note != nullptr) *note = "GitHub could not be asked (" + why + "): using SenPatcher " + local.tag + " from before";
        *out = local;
        return true;
    }
    if (local.ok() && CompareVersions(local.tag, latest.tag) >= 0) {
        *out = local;
        return true;
    }
    Log("downloading SenPatcher " + latest.tag + " (" + latest.asset + ") ...");
    if (latest.sha256.empty()) Log("  GitHub records no sha256 for it: only HTTPS vouches for the download");
    HttpResponse resp;
    if (!http->Get(latest.url, {{"Accept", "application/octet-stream"}}, &resp, progress) || resp.status != 200) {
        why = resp.error.empty() ? "HTTP " + std::to_string(resp.status) + " for " + latest.url : resp.error;
        if (!local.ok()) {
            if (error != nullptr) *error = "SenPatcher " + latest.tag + " could not be downloaded: " + why;
            return false;
        }
        if (note != nullptr) *note = "SenPatcher " + latest.tag + " could not be downloaded (" + why + "): using " + local.tag;
        *out = local;
        return true;
    }
    if (!InstallSenPatcherZip(latest, resp.body, out, &why)) {
        if (error != nullptr) *error = why;
        return false;
    }
    Log("  SenPatcher " + latest.tag + " is in " + U8(out->dir));
    return true;
}

// ---------------------------------------------------------------- Proton
namespace {

bool ReadVdfFile(const fs::path& p, Vdf* out) {
    std::string text;
    return ReadFile(p, &text) && ParseVdf(text, out);
}

// "Proton 9.0 (Beta)" -> 900 (sorting the official ones: newest first)
int OfficialProtonVersion(const std::string& folder) {
    if (!StartsWith(folder, "Proton ")) return -1;
    int major = 0, minor = 0;
    size_t i = 7;
    if (i >= folder.size() || !std::isdigit(static_cast<unsigned char>(folder[i]))) return -1;
    while (i < folder.size() && std::isdigit(static_cast<unsigned char>(folder[i]))) major = major * 10 + (folder[i++] - '0');
    if (i < folder.size() && folder[i] == '.') {
        ++i;
        while (i < folder.size() && std::isdigit(static_cast<unsigned char>(folder[i]))) minor = minor * 10 + (folder[i++] - '0');
    }
    return major * 100 + minor;
}

bool SamePath(const fs::path& a, const fs::path& b) {
    std::error_code ec;
    return fs::equivalent(a, b, ec);
}

// A tool's toolmanifest.vdf: {"manifest": {"commandline": "/proton %verb%", "require_tool_appid": "1628350"}}
struct ToolManifest {
    std::string commandline;
    std::string require_appid;
};
bool ReadToolManifest(const fs::path& dir, ToolManifest* out) {
    Vdf root;
    if (!ReadVdfFile(dir / "toolmanifest.vdf", &root)) return false;
    const Vdf* m = root.Find("manifest");
    if (m == nullptr) return false;
    out->commandline = m->Str("commandline");
    out->require_appid = m->Str("require_tool_appid");
    return true;
}

// The tool's command line with its own path in front: "/proton %verb%" -> <dir>/proton waitforexitandrun
std::vector<std::string> ToolCommand(const fs::path& dir, const std::string& commandline) {
    std::vector<std::string> out;
    for (const std::string& raw : Split(commandline, ' ')) {
        std::string t = Trim(raw);
        if (t.empty()) continue;
        for (size_t p; (p = t.find("%verb%")) != std::string::npos;) t.replace(p, 6, kVerb);
        if (out.empty() && t[0] == '/') t = U8(dir) + t;
        out.push_back(t);
    }
    return out;
}

// The folder of an installed Steam app (a runtime: SteamLinuxRuntime_sniper is app 1628350).
fs::path AppDir(const std::vector<fs::path>& libraries, const std::string& appid) {
    for (const fs::path& lib : libraries) {
        Vdf root;
        if (!ReadVdfFile(lib / "steamapps" / ("appmanifest_" + appid + ".acf"), &root)) continue;
        const Vdf* state = root.Find("AppState");
        const std::string dir = state != nullptr ? state->Str("installdir") : std::string();
        if (!dir.empty() && IsDir(lib / "steamapps" / "common" / Path(dir))) return lib / "steamapps" / "common" / Path(dir);
    }
    return fs::path();
}

}  // namespace

std::string OfficialProtonName(const std::string& folder) {
    if (folder == "Proton - Experimental") return "proton_experimental";
    if (folder == "Proton Hotfix") return "proton_hotfix";
    const int v = OfficialProtonVersion(folder);
    if (v < 0) return std::string();
    const int major = v / 100, minor = v % 100;
    return "proton_" + std::to_string(major) + (minor == 0 ? std::string() : std::to_string(minor));
}

std::vector<ProtonTool> FindProtonTools(const std::vector<fs::path>& roots, const std::vector<fs::path>& libraries) {
    std::vector<ProtonTool> out;
    auto add = [&](const ProtonTool& t) {
        for (const ProtonTool& e : out) {
            if (e.name == t.name || SamePath(e.dir, t.dir)) return;
        }
        out.push_back(t);
    };
    for (const fs::path& lib : libraries) {
        std::error_code ec;
        for (fs::directory_iterator it(lib / "steamapps" / "common", ec), end; !ec && it != end; it.increment(ec)) {
            const std::string folder = U8(it->path().filename());
            const std::string name = OfficialProtonName(folder);
            if (name.empty() || !Exists(it->path() / "proton") || !Exists(it->path() / "toolmanifest.vdf")) continue;
            add(ProtonTool{name, folder, it->path()});
        }
    }
    std::vector<fs::path> custom;
    for (const fs::path& root : roots) custom.push_back(root / "compatibilitytools.d");
#ifndef _WIN32
    custom.push_back(Path("/usr/share/steam/compatibilitytools.d"));
#endif
    for (const fs::path& d : custom) {
        std::error_code ec;
        for (fs::directory_iterator it(d, ec), end; !ec && it != end; it.increment(ec)) {
            // compatibilitytool.vdf: {"compatibilitytools": {"compat_tools": {"<name>": {"install_path", "display_name"}}}}
            Vdf root;
            if (!ReadVdfFile(it->path() / "compatibilitytool.vdf", &root)) continue;
            const Vdf* top = root.Find("compatibilitytools");
            const Vdf* tools = top != nullptr ? top->Find("compat_tools") : nullptr;
            if (tools == nullptr) continue;
            for (const auto& kv : tools->children) {
                if (!kv.second.is_block) continue;
                const std::string install = kv.second.Str("install_path");
                const fs::path dir = install.empty() || install == "." ? it->path() : it->path() / Path(install);
                if (!Exists(dir / "proton") || !Exists(dir / "toolmanifest.vdf")) continue;   // not a Proton
                const std::string display = kv.second.Str("display_name");
                add(ProtonTool{kv.first, display.empty() ? kv.first : display, dir});
            }
        }
    }
    return out;
}

std::string CompatToolSetting(const fs::path& steam_root, const std::string& appid) {
    Vdf root;
    if (!ReadVdfFile(steam_root / "config" / "config.vdf", &root)) return std::string();
    const Vdf* v = root.Find("InstallConfigStore");
    for (const char* key : {"Software", "Valve", "Steam", "CompatToolMapping", appid.c_str()}) {
        if (v == nullptr) return std::string();
        v = v->Find(key);
    }
    return v != nullptr ? v->Str("name") : std::string();
}

bool PlanProtonLaunch(const GameInstall& game, const std::vector<fs::path>& roots, const std::vector<fs::path>& libraries,
                      ProtonLaunch* out, std::string* error) {
    auto fail = [&](const std::string& why) {
        if (error != nullptr) *error = why;
        return false;
    };
    if (!game.IsSteam() || game.library.empty()) return fail("not the Steam version of the game");
    if (roots.empty()) return fail("Steam was not found");
    ProtonLaunch l;
    l.game_dir = game.dir;
    l.library = game.library;
    l.compat_data = game.library / "steamapps" / "compatdata" / kSteamAppId;
    l.prefix = l.compat_data / "pfx";
    // the Steam whose libraries have the game (several roots are usually links to one)
    l.steam_root = roots[0];
    for (const fs::path& r : roots) {
        bool has = false;
        for (const fs::path& lib : SteamLibraries({r})) has = has || SamePath(lib, game.library);
        if (has) {
            l.steam_root = r;
            break;
        }
    }
    const std::vector<ProtonTool> tools = FindProtonTools(roots, libraries);
    if (tools.empty()) return fail("no Proton is installed - start the game once from Steam, which installs it");
    auto find = [&](const std::string& name) -> const ProtonTool* {
        for (const ProtonTool& t : tools) {
            if (!name.empty() && t.name == name) return &t;
        }
        return nullptr;
    };
    const std::string own = CompatToolSetting(l.steam_root, kSteamAppId);
    const std::string def = CompatToolSetting(l.steam_root, "0");
    const ProtonTool* chosen = nullptr;
    if ((chosen = find(own)) != nullptr) {
        l.why = "the game's compatibility setting in Steam";
    } else if (own.empty() && (chosen = find(def)) != nullptr) {
        l.why = "Steam's default compatibility tool";
    } else {
        // the newest numbered Proton of Valve's, then Experimental / Hotfix, then any
        int best = -1;
        for (const ProtonTool& t : tools) {
            const int v = OfficialProtonVersion(t.display);
            if (v > best && StartsWith(t.name, "proton_")) {
                best = v;
                chosen = &t;
            }
        }
        if (chosen == nullptr) chosen = find("proton_experimental");
        if (chosen == nullptr) chosen = find("proton_hotfix");
        if (chosen == nullptr) chosen = &tools[0];
        const std::string set = own.empty() ? def : own;
        l.why = set.empty() ? "the newest installed (Steam names none for the game)"
                            : "the newest installed (Steam's setting '" + set + "' is not installed)";
    }
    l.tool = *chosen;
    // the command: the runtime the tool asks for (and the one that one asks for), outermost first
    ToolManifest m;
    if (!ReadToolManifest(l.tool.dir, &m)) return fail("cannot read " + U8(l.tool.dir / "toolmanifest.vdf"));
    std::vector<std::string> command = ToolCommand(l.tool.dir, m.commandline.empty() ? "/proton %verb%" : m.commandline);
    l.tool_dirs.push_back(l.tool.dir);
    std::string need = m.require_appid;
    for (int depth = 0; !need.empty() && depth < 4; ++depth) {
        const fs::path dir = AppDir(libraries, need);
        ToolManifest rm;
        if (dir.empty() || !ReadToolManifest(dir, &rm) || rm.commandline.empty()) {
            Log("  the Steam Linux Runtime (app " + need + ") " + l.tool.display + " asks for is not installed: running Proton directly");
            break;
        }
        std::vector<std::string> outer = ToolCommand(dir, rm.commandline);
        outer.insert(outer.end(), command.begin(), command.end());
        command = outer;
        l.tool_dirs.push_back(dir);
        need = rm.require_appid;
    }
    l.command = command;
    *out = l;
    return true;
}

std::vector<std::pair<std::string, std::string>> ProtonEnv(const ProtonLaunch& l, const std::vector<fs::path>& mounts) {
    std::vector<std::string> tool_paths, mount_paths;
    for (const fs::path& d : l.tool_dirs) tool_paths.push_back(U8(d));
    for (const fs::path& d : mounts) mount_paths.push_back(U8(d));
    // SteamGameId / SteamAppId are left as they are: in Game Mode the manager's own (a non-Steam
    // shortcut's) are what lets gamescope show the program's window as part of the manager.
    return {
        {"STEAM_COMPAT_CLIENT_INSTALL_PATH", U8(l.steam_root)},
        {"STEAM_COMPAT_DATA_PATH", U8(l.compat_data)},
        {"STEAM_COMPAT_APP_ID", kSteamAppId},
        {"STEAM_COMPAT_INSTALL_PATH", U8(l.game_dir)},
        {"STEAM_COMPAT_LIBRARY_PATHS", U8(l.library)},
        {"STEAM_COMPAT_TOOL_PATHS", Join(tool_paths, ":")},
        {"STEAM_COMPAT_MOUNTS", Join(mount_paths, ":")},
        {"WINEPREFIX", ""},
    };
}

bool RunInProton(const ProtonLaunch& l, const std::vector<std::string>& args, const fs::path& cwd,
                 const std::vector<fs::path>& mounts, int* exit_code, std::string* error) {
    std::vector<std::string> argv = l.command;
    argv.insert(argv.end(), args.begin(), args.end());
    std::error_code ec;
    fs::create_directories(l.compat_data, ec);   // Proton makes pfx/ inside it
    const fs::path log = SenPatcherRoot() / "proton.log";
    std::string line = "\n==== " + IsoTimeUtc() + "  " + Join(argv, " ") + "\n";
    std::string old;
    ReadFile(log, &old);
    if (old.size() > 4 * 1024 * 1024) old.clear();   // keep the log from growing without end
    WriteFileAtomic(log, old + line);
    return RunAndWait(argv, ProtonEnv(l, mounts), cwd, log, exit_code, error);
}

// ---------------------------------------------------------------- the dll override
std::string PrefixDllOverride(const fs::path& prefix, const std::string& dll) {
    std::string text;
    if (!ReadFile(prefix / "user.reg", &text)) return std::string();
    bool in_section = false;
    for (const std::string& raw : Lines(text)) {
        const std::string line = Trim(raw);
        if (!line.empty() && line[0] == '[') {
            // "[Software\\Wine\\DllOverrides] 1700000000": key names are case-insensitive
            in_section = StartsWith(Lower(line), "[software\\\\wine\\\\dlloverrides]");
            continue;
        }
        if (!in_section || line.size() < 2 || line[0] != '"') continue;
        const size_t end = line.find('"', 1);
        if (end == std::string::npos) continue;
        std::string name = Lower(line.substr(1, end - 1));
        if (!name.empty() && name[0] == '*') name = name.substr(1);
        if (name != Lower(dll)) continue;
        const size_t eq = line.find('=', end);
        if (eq == std::string::npos) continue;
        std::string value = Trim(line.substr(eq + 1));
        if (value.size() >= 2 && value.front() == '"' && value.back() == '"') value = value.substr(1, value.size() - 2);
        return value;
    }
    return std::string();
}

std::vector<std::string> SteamLaunchOptions(const std::vector<fs::path>& roots, const std::string& appid) {
    std::vector<std::string> out;
    std::vector<fs::path> seen;
    for (const fs::path& root : roots) {
        std::error_code ec;
        for (fs::directory_iterator it(root / "userdata", ec), end; !ec && it != end; it.increment(ec)) {
            const fs::path vdf = it->path() / "config" / "localconfig.vdf";
            bool dup = false;
            for (const fs::path& s : seen) dup = dup || SamePath(s, vdf);
            if (dup) continue;
            seen.push_back(vdf);
            Vdf v;
            if (!ReadVdfFile(vdf, &v)) continue;
            const Vdf* n = v.Find("UserLocalConfigStore");
            for (const char* key : {"Software", "Valve", "Steam", "apps", appid.c_str()}) {
                if (n == nullptr) break;
                n = n->Find(key);
            }
            if (n != nullptr && !n->Str("LaunchOptions").empty()) out.push_back(n->Str("LaunchOptions"));
        }
    }
    return out;
}

bool OverrideLoadsNative(const std::string& value) {
    const std::string v = Lower(Trim(value));
    return !v.empty() && v[0] == 'n';
}

bool LaunchOptionLoadsNative(const std::string& options, const std::string& dll) {
    // WINEDLLOVERRIDES="d3d9=n;dinput8=n,b" %command%: entries split by ';', "a,b=n,b" names several
    const std::string o = Lower(options);
    const size_t at = o.find("winedlloverrides=");
    if (at == std::string::npos) return false;
    std::string value = o.substr(at + 17);
    if (!value.empty() && (value[0] == '"' || value[0] == '\'')) {
        const size_t close = value.find(value[0], 1);
        value = value.substr(1, close == std::string::npos ? std::string::npos : close - 1);
    } else {
        value = value.substr(0, value.find(' '));
    }
    for (const std::string& entry : Split(value, ';')) {
        const size_t eq = entry.find('=');
        if (eq == std::string::npos) continue;
        for (const std::string& name : Split(entry.substr(0, eq), ',')) {
            if (Trim(name) == Lower(dll) && OverrideLoadsNative(entry.substr(eq + 1))) return true;
        }
    }
    return false;
}

bool SetSenPatcherOverride(const ProtonLaunch& l, std::string* error) {
    int code = -1;
    std::string why;
    const std::vector<std::string> args = {"c:\\windows\\system32\\reg.exe", "add", "HKCU\\Software\\Wine\\DllOverrides",
                                           "/v", kSenPatcherDll, "/t", "REG_SZ", "/d", kNativeFirst, "/f"};
    if (!RunInProton(l, args, l.game_dir, {}, &code, &why)) {
        if (error != nullptr) *error = "Proton could not be started: " + why;
        return false;
    }
    if (code != 0) {
        if (error != nullptr) {
            *error = "setting the dll override failed (exit code " + std::to_string(code) + "; see "
                     + U8(SenPatcherRoot() / "proton.log") + ")";
        }
        return false;
    }
    return true;
}

// ---------------------------------------------------------------- status and the run
std::string SenPatcherStatus::Text() const {
    std::string s = dll ? "installed (DINPUT8.dll)" : "not installed";
    if (dll && needs_override) {
        s += override_set ? ", dinput8 override set (" + override_where + ")"
                          : " - but Proton loads its own dinput8 until the dll override is set";
    }
    return s;
}

SenPatcherStatus GetSenPatcherStatus(const GameInstall& game) {
    SenPatcherStatus s;
    s.dll = SenPatcherDllPresent(game.dir);
    s.local_tag = LocalSenPatcher().tag;
#ifdef _WIN32
    s.can_run = true;   // SenPatcher.exe itself, for any store's game
#else
    s.needs_override = true;
    if (!game.prefix.empty() && OverrideLoadsNative(PrefixDllOverride(game.prefix, kSenPatcherDll))) {
        s.override_set = true;
        s.override_where = "prefix";
    }
    if (game.IsSteam()) {
        for (const std::string& o : SteamLaunchOptions(SteamRoots(), kSteamAppId)) {
            if (!s.override_set && LaunchOptionLoadsNative(o, kSenPatcherDll)) {
                s.override_set = true;
                s.override_where = "launch options";
            }
        }
        s.can_run = true;
    } else {
        s.cannot_run = "only for the Steam version - elsewhere run SenPatcher.exe in the game's Wine prefix "
                       "and set the dll override dinput8=n,b there";
    }
#endif
    return s;
}

namespace {

// SenPatcher's file picker opens at its remembered CS1 folder: the game's, when it remembers none yet
// (gui.ini in its settings folder, %LOCALAPPDATA%\SenPatcherGui; `game` as SenPatcher sees it).
void PointSenPatcherAtGame(const fs::path& gui_ini, const std::string& game) {
    if (Exists(gui_ini)) return;
    WriteFileAtomic(gui_ini, "[GamePaths]\nSen1Path=" + game + "\n");
}

}  // namespace

SenPatcherRun RunSenPatcher(const fs::path& game_dir, Http* http, bool override_only, const ProgressFn& progress) {
    SenPatcherRun r;
    if (GameRunning()) {
        r.error = "close the game first: SenPatcher changes the game folder";
        return r;
    }
    std::string why;
#ifdef _WIN32
    // SenPatcher.exe itself: no Proton, and Windows loads the DINPUT8.dll next to ed8.exe on its own
    if (override_only) {
        r.error = "no dll override is needed on Windows";
        return r;
    }
    SenPatcherCopy copy;
    std::string note;
    if (!EnsureSenPatcher(http, &copy, &note, &why, progress)) {
        r.error = why;
        return r;
    }
    if (!note.empty()) {
        Log(note);
        r.notes.push_back(note);
    }
    r.tag = copy.tag;
    const std::string local_app_data = GetEnv("LOCALAPPDATA");
    if (!local_app_data.empty()) {
        std::string dir = U8(game_dir);
        for (char& c : dir) {
            if (c == '/') c = '\\';
        }
        PointSenPatcherAtGame(Path(local_app_data) / "SenPatcherGui" / "gui.ini", dir);
    }
    Log("SenPatcher " + copy.tag + " is open: press \"Patch game\" for Trails of Cold Steel, then close it");
    int code = -1;
    if (!RunAndWait({U8(copy.exe)}, {}, copy.dir, fs::path(), &code, &why)) {
        r.error = "SenPatcher could not be started: " + why;
        return r;
    }
    if (code != 0) r.notes.push_back("SenPatcher exited with code " + std::to_string(code));
#else
    const GameInstall game = DescribeGameDir(game_dir, FindGames());
    if (!game.IsSteam()) {
        r.error = "SenPatcher can only be run here for the Steam version of the game (through its Proton). "
                  "Elsewhere run SenPatcher.exe in the game's Wine prefix and set the dll override dinput8=n,b there.";
        return r;
    }
    const std::vector<fs::path> roots = SteamRoots();
    ProtonLaunch launch;
    if (!PlanProtonLaunch(game, roots, SteamLibraries(roots), &launch, &why)) {
        r.error = "the game's Proton was not found: " + why;
        return r;
    }
    Log("Proton: " + launch.tool.display + " (" + launch.why + ") in " + U8(launch.tool.dir));
    SenPatcherCopy copy;
    if (!override_only) {
        std::string note;
        if (!EnsureSenPatcher(http, &copy, &note, &why, progress)) {
            r.error = why;
            return r;
        }
        if (!note.empty()) {
            Log(note);
            r.notes.push_back(note);
        }
        r.tag = copy.tag;
    }
    Log(IsDir(launch.prefix) ? "setting the dll override dinput8=native,builtin in the game's prefix ..."
                             : "creating the game's Proton prefix and setting the dll override (this takes a minute) ...");
    if (!SetSenPatcherOverride(launch, &why)) {
        r.error = why;
        return r;
    }
    r.notes.push_back("Wine's dll override dinput8=native,builtin is set in the game's prefix: Proton loads SenPatcher's DINPUT8.dll.");
    if (override_only) {
        r.ok = true;
        r.installed = SenPatcherDllPresent(game.dir);
        return r;
    }
    // in the prefix: Proton's user is steamuser, the game a Wine path
    std::string wine_dir = "Z:" + U8(game.dir);
    for (char& c : wine_dir) {
        if (c == '/') c = '\\';
    }
    PointSenPatcherAtGame(launch.prefix / "drive_c" / "users" / "steamuser" / "AppData" / "Local" / "SenPatcherGui" / "gui.ini",
                          wine_dir);
    Log("SenPatcher " + copy.tag + " is open: press \"Patch game\" for Trails of Cold Steel, then close it");
    int code = -1;
    if (!RunInProton(launch, {U8(copy.exe)}, copy.dir, {copy.dir}, &code, &why)) {
        r.error = "SenPatcher could not be started: " + why;
        return r;
    }
    if (code != 0) {
        r.notes.push_back("SenPatcher exited with code " + std::to_string(code) + " (Proton's output: "
                          + U8(SenPatcherRoot() / "proton.log") + ")");
    }
#endif
    r.installed = SenPatcherDllPresent(game_dir);
    Log(r.installed ? "SenPatcher's DINPUT8.dll is next to ed8.exe" : "SenPatcher was closed without patching the game");
    r.ok = true;
    return r;
}

}  // namespace atmt
