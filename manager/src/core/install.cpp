// install.cpp - see install.h.
#include "install.h"

#include <algorithm>
#include <chrono>
#include <set>

#include "hash.h"
#include "ini.h"
#include "locator.h"
#include "platform.h"
#include "settings.h"

namespace atmt {

namespace {

// What to do when the game's own files are damaged or missing, for whichever store it came from.
const std::string kVerifyGameFiles =
    "verify the game's files (Steam: Verify integrity of game files; GOG Galaxy: Verify / Repair; "
    "elsewhere: reinstall the game)";

// What a backup holds and a restore puts back: everything the manager or the loader writes. That
// is known by layout, not by mod: the proxy and the game's renamed library, the install record,
// the toolkit's own inis and dlls next to the game (atmt_*), SenPatcher's order and the toolkit's
// files in SenPatcher's mods/ (mods/atmt_*: the icon pack and its record), and atmt_mods/ as a whole. Logs (atmt_*.log) are copied too
// (the state they describe is in the backup) but never restored.
std::vector<std::string> ManagedFiles(const fs::path& game) {
    std::vector<std::string> out = {kProxyDll, kOrigDll, kInstallRecord, "mods/order.txt"};
    std::error_code ec;
    for (fs::directory_iterator it(game, ec), end; !ec && it != end; it.increment(ec)) {
        const std::string n = U8(it->path().filename());
        const std::string l = Lower(n);
        if (it->is_regular_file() && StartsWith(n, kToolkitPrefix) && (EndsWith(l, ".ini") || EndsWith(l, ".dll"))
            && n != kInstallRecord) {
            out.push_back(n);
        }
    }
    for (fs::directory_iterator it(game / "mods", ec), end; !ec && it != end; it.increment(ec)) {
        const std::string n = U8(it->path().filename());
        if (it->is_regular_file() && StartsWith(n, kToolkitPrefix)) out.push_back("mods/" + n);
    }
    return out;
}

std::vector<std::string> LogFiles(const fs::path& game) {
    std::vector<std::string> out;
    std::error_code ec;
    for (fs::directory_iterator it(game, ec), end; !ec && it != end; it.increment(ec)) {
        const std::string n = U8(it->path().filename());
        if (it->is_regular_file() && StartsWith(n, kToolkitPrefix) && EndsWith(Lower(n), ".log")) out.push_back(n);
    }
    return out;
}

// install_rules.json patterns: '*' matches within one name, a trailing '/' means a folder.
bool Glob(const std::string& pattern, const std::string& name) {
    size_t p = 0, n = 0, star = std::string::npos, mark = 0;
    while (n < name.size()) {
        if (p < pattern.size() && (pattern[p] == name[n] || pattern[p] == '?')) {
            ++p;
            ++n;
        } else if (p < pattern.size() && pattern[p] == '*') {
            star = p++;
            mark = n;
        } else if (star != std::string::npos) {
            p = star + 1;
            n = ++mark;
        } else {
            return false;
        }
    }
    while (p < pattern.size() && pattern[p] == '*') ++p;
    return p == pattern.size();
}

std::string Stem(const std::string& file) {
    const size_t dot = file.find_last_of('.');
    return dot == std::string::npos ? file : file.substr(0, dot);
}

std::vector<std::string> DllStems(const fs::path& dir) {
    std::vector<std::string> out;
    std::error_code ec;
    for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
        const std::string name = U8(it->path().filename());
        if (it->is_regular_file() && EndsWith(Lower(name), ".dll")) out.push_back(Stem(name));
    }
    std::sort(out.begin(), out.end());
    return out;
}

bool Contains(const std::vector<std::string>& list, const std::string& v) {
    for (const std::string& s : list) {
        if (IEquals(s, v)) return true;
    }
    return false;
}

// ---------------------------------------------------------------- backup
struct Backup {
    fs::path dir;
    Json manifest;   // backup.json
};

bool MakeBackup(const fs::path& game, const std::string& tag, const std::string& reason, Backup* out,
                std::string* error) {
    out->dir = game / Path("atmt_backup_" + tag);
    std::error_code ec;
    if (Exists(out->dir)) {   // a tag used twice: never mix two states in one folder
        for (int n = 2;; ++n) {
            const fs::path alt = game / Path("atmt_backup_" + tag + "_" + std::to_string(n));
            if (!Exists(alt)) {
                out->dir = alt;
                break;
            }
        }
    }
    fs::create_directories(out->dir, ec);
    if (ec) {
        *error = "cannot create " + U8(out->dir) + ": " + ec.message();
        return false;
    }
    Json m = Json::MakeObject();
    m["created"] = IsoTimeUtc();
    // to the millisecond: several backups can be taken within one second (an install and its
    // rollback, a restore), and pruning keeps the newest
    m["created_ms"] = static_cast<int64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                               std::chrono::system_clock::now().time_since_epoch())
                                               .count());
    m["reason"] = reason;
    m["manager_version"] = AppVersion();
    const Json record = ReadInstallRecord(game);
    m["installed"] = record.Str("summary");
    Json items = Json::MakeObject();
    int n = 0;
    for (const std::string& rel : ManagedFiles(game)) {
        const bool present = Exists(game / Path(rel));
        items[rel] = present;
        if (!present) continue;
        std::error_code cec;
        fs::create_directories((out->dir / Path(rel)).parent_path(), cec);
        fs::copy_file(game / Path(rel), out->dir / Path(rel), fs::copy_options::overwrite_existing, cec);
        if (cec) {
            *error = "backup: cannot copy " + rel + ": " + cec.message();
            return false;
        }
        ++n;
    }
    const bool had_mods = IsDir(game / kModDir);
    items[std::string(kModDir) + "/"] = had_mods;
    if (had_mods) {
        if (!CopyTree(game / kModDir, out->dir / kModDir, error)) return false;
        ++n;
    }
    for (const std::string& rel : LogFiles(game)) {
        std::error_code cec;
        fs::copy_file(game / Path(rel), out->dir / Path(rel), fs::copy_options::overwrite_existing, cec);
    }
    m["items"] = items;
    out->manifest = m;
    if (!WriteFileAtomic(out->dir / "backup.json", m.Dump(), error)) return false;
    Logf("backed up %d item(s) to %s", n, U8(out->dir).c_str());
    return true;
}

// Brings the managed items back to the backup's state. With backup.json, an item that did not
// exist then is removed now; a backup from deck_install.sh (no backup.json) only puts back what it has.
bool RestoreFrom(const fs::path& game, const fs::path& backup, std::string* error) {
    std::string text;
    Json m;
    const bool has_manifest = ReadFile(backup / "backup.json", &text) && Json::Parse(text, &m);
    bool ok = true;
    std::vector<std::string> rels;
    if (has_manifest) {
        for (const auto& kv : m["items"].items()) {
            if (!EndsWith(kv.first, "/")) rels.push_back(kv.first);
        }
    }
    for (const std::string& rel : ManagedFiles(game)) {   // here now: removed unless the backup has it
        if (std::find(rels.begin(), rels.end(), rel) == rels.end()) rels.push_back(rel);
    }
    if (!has_manifest) {   // a backup from deck_install.sh: only what it has
        rels.clear();
        for (const std::string& rel : ManagedFiles(backup)) rels.push_back(rel);
    }
    for (const std::string& rel : rels) {
        const bool in_backup = Exists(backup / Path(rel));
        const bool existed = has_manifest ? m["items"][rel].AsBool(false) && in_backup : in_backup;
        std::string err;
        if (existed) {
            if (!CopyFileAtomic(backup / Path(rel), game / Path(rel), &err)) {
                Log("  restore: " + err);
                ok = false;
            }
        } else if (has_manifest && rel != "mods/order.txt") {
            if (!RemoveFile(game / Path(rel), &err)) {
                Log("  restore: " + err);
                ok = false;
            }
        }
    }
    const std::string mods_key = std::string(kModDir) + "/";
    const bool mods_in_backup = IsDir(backup / kModDir);
    const bool mods_existed = has_manifest ? m["items"][mods_key].AsBool(mods_in_backup) : mods_in_backup;
    if (mods_existed && mods_in_backup) {
        std::error_code ec;
        fs::remove_all(game / kModDir, ec);
        if (ec) {
            Log("  restore: cannot clear " + std::string(kModDir) + ": " + ec.message());
            ok = false;
        }
        std::string err;
        if (!CopyTree(backup / kModDir, game / kModDir, &err)) {
            Log("  restore: " + err);
            ok = false;
        }
    } else if (!mods_existed && has_manifest) {
        std::error_code ec;
        fs::remove_all(game / kModDir, ec);
        if (ec) ok = false;
    }
    if (!ok && error != nullptr) *error = "some files could not be restored (see the log)";
    return ok;
}

// ---------------------------------------------------------------- leftovers of earlier deployments
// The payload's install_rules.json names them (the manager knows no mod's history); a stale dll
// would still be loaded (a dll left in atmt_mods/ runs as one more mod).
//
// A rule may only name the toolkit's own files: everything it ever put into the game folder starts
// with atmt_ (kToolkitPrefix), so the rule's first path part must start with it (literally, so a
// wildcard can only come after it). Nothing else in the game folder - the game's files, other
// mods' - can match, whatever a rule says. Rooted paths, '\', ':' and '..' are refused.
bool SafeRemoveRule(const std::string& rule) {
    if (rule.empty() || rule[0] == '/' || rule.find('\\') != std::string::npos || rule.find(':') != std::string::npos) {
        return false;
    }
    for (const std::string& part : Split(rule, '/')) {
        if (part == "..") return false;
    }
    return StartsWith(rule.substr(0, rule.find('/')), kToolkitPrefix);
}

void RemoveLegacy(const fs::path& game, const std::vector<std::string>& rules) {
    std::string err;
    for (std::string rule : rules) {
        if (!SafeRemoveRule(rule)) {
            Log("  install rule refused (not a file of the toolkit's): " + rule);
            continue;
        }
        const bool dir = EndsWith(rule, "/");
        if (dir) rule.pop_back();
        const size_t slash = rule.find_last_of('/');
        const fs::path folder = slash == std::string::npos ? game : game / Path(rule.substr(0, slash));
        const std::string pattern = slash == std::string::npos ? rule : rule.substr(slash + 1);
        if (pattern.empty()) continue;
        std::vector<fs::path> hits;
        std::error_code ec;
        for (fs::directory_iterator it(folder, ec), end; !ec && it != end; it.increment(ec)) {
            if (it->is_directory() != dir) continue;
            if (Glob(pattern, U8(it->path().filename()))) hits.push_back(it->path());
        }
        for (const fs::path& p : hits) {
            bool removed;
            if (dir) {
                fs::remove_all(p, ec);
                removed = !ec;
            } else {
                removed = RemoveFile(p, &err);
            }
            if (removed) Log("  removed stale " + Rel(p, game) + (dir ? "/" : "") + " (an earlier version)");
        }
    }
    CleanupOldFiles(game, false);
    CleanupOldFiles(game / kModDir, true);
}

std::string ExeMd5(const fs::path& game) { return Md5File(game / kGameExe); }

// SenPatcher's hook dll exports `SenPatcherHook` (native/sen1/main_cs1.cpp); the name is in its
// export table, so a plain search of the file finds it. Both spellings: Linux folders are case
// sensitive and SenPatcher ships DINPUT8.dll.
bool HasSenPatcher(const fs::path& game) {
    for (const char* name : {"DINPUT8.dll", "dinput8.dll"}) {
        std::string dll;
        if (ReadFile(game / name, &dll) && dll.find("SenPatcherHook") != std::string::npos) return true;
    }
    return false;
}

// What SenPatcher's own "restore the game" looks for (native/sentools/old_senpatcher_unpatch.cpp).
bool HasOldSenPatcherBackups(const fs::path& game) {
    return Exists(game / "ed8.exe.senpatcher.bkp") || Exists(game / "ed8jp.exe.senpatcher.bkp")
           || IsDir(game / "senpatcher_bkp") || Exists(game / "senpatcher_rerun_revert_data.bin");
}

}  // namespace

bool SenPatcherSetUp(const fs::path& game) { return HasSenPatcher(game) || Exists(game / "mods/order.txt"); }
bool SenPatcherDllPresent(const fs::path& game) { return HasSenPatcher(game); }

// ---------------------------------------------------------------- recognising our loader
bool IsOurLoader(const fs::path& dll) {
    std::string data;
    if (!ReadFile(dll, &data)) return false;
    // atmt_loader.ini is the loader's own settings file - a string the game's NVIDIA library never has.
    return data.find("atmt_loader.ini") != std::string::npos;
}

Json ReadInstallRecord(const fs::path& game_dir) {
    std::string text;
    Json j;
    if (!ReadFile(game_dir / kInstallRecord, &text) || !Json::Parse(text, &j) || !j.is_object()) return Json::MakeObject();
    return j;
}

bool WriteInstallRecord(const fs::path& game_dir, const Json& record) {
    return WriteFileAtomic(game_dir / kInstallRecord, record.Dump());
}

bool IconPackRemovedByPlayer(const fs::path& game_dir) {
    return ReadInstallRecord(game_dir)["icon_pack_removed"].AsBool(false);
}

bool SetIconPackRemovedByPlayer(const fs::path& game_dir, bool removed) {
    if (IconPackRemovedByPlayer(game_dir) == removed) return true;
    Json record = ReadInstallRecord(game_dir);
    if (removed) {
        record["icon_pack_removed"] = Json(true);
    } else {
        record.Erase("icon_pack_removed");
    }
    return WriteInstallRecord(game_dir, record);
}

bool RemoveIconPackByPlayer(const fs::path& game_dir, std::string* error) {
    if (!RemoveIconPack(game_dir, error)) return false;
    if (!SetIconPackRemovedByPlayer(game_dir, true)) {
        if (error != nullptr) *error = std::string("cannot write ") + kInstallRecord;
        return false;
    }
    return true;
}

IconPackResult BuildIconPackByPlayer(const fs::path& game_dir, const IconPackConfig& config) {
    IconPackResult r = InstallIconPack(game_dir, config);
    if (r.ok) SetIconPackRemovedByPlayer(game_dir, false);
    return r;
}

const char* ActionName(GameStatus::Action a) {
    switch (a) {
        case GameStatus::Action::Install: return "Install";
        case GameStatus::Action::Update: return "Update";
        case GameStatus::Action::Repair: return "Repair";
        case GameStatus::Action::Reinstall: return "Reinstall";
        default: return "";
    }
}

GameStatus::Action GameStatus::SuggestedAction(const Payload* payload) const {
    if (!hard_stops.empty() || payload == nullptr) return Action::None;
    if (!installed && !record_present) return Action::Install;
    // installed by a developer script (no atmt_install.json): taking it over is an update
    if (installed_versions.empty() || !Outdated(*payload).empty()) return Action::Update;
    if (!problems.empty()) return Action::Repair;
    return Action::Reinstall;
}

std::vector<std::string> GameStatus::Outdated(const Payload& payload) const {
    std::vector<std::string> out;
    for (const Component& c : payload.components) {
        auto it = installed_versions.find(c.name);
        if (it == installed_versions.end()) {
            out.push_back(c.title + " " + c.version + " (new)");
        } else if (CompareVersions(it->second, c.version) < 0) {
            out.push_back(c.title + " " + it->second + " -> " + c.version);
        }
    }
    return out;
}

// ---------------------------------------------------------------- status
GameStatus Inspect(const fs::path& game_dir, const Payload* payload) {
    GameStatus s;
    s.dir = game_dir;
    s.dir_exists = IsDir(game_dir);
    s.game_running = GameRunning();
    if (!s.dir_exists) {
        s.hard_stops.push_back("the folder does not exist");
        return s;
    }
    if (HasGameExe(game_dir)) {
        s.exe_md5 = ExeMd5(game_dir);
        s.verdict = (payload != nullptr && payload->IsSupportedExe(s.exe_md5)) ? ExeVerdict::Supported : ExeVerdict::Unverified;
    } else {
        s.hard_stops.push_back("no ed8.exe in this folder - pick the game's folder");
    }
    s.proxy_present = Exists(game_dir / kProxyDll);
    s.proxy_is_ours = s.proxy_present && IsOurLoader(game_dir / kProxyDll);
    s.orig_present = Exists(game_dir / kOrigDll);
    s.installed = s.proxy_is_ours;
    s.record = ReadInstallRecord(game_dir);
    s.record_present = Exists(game_dir / kInstallRecord);
    for (const auto& kv : s.record["components"].items()) s.installed_versions[kv.first] = kv.second.AsString();
    s.unverified_acknowledged = !s.exe_md5.empty() && s.record.Str("unverified_ack_md5") == s.exe_md5;
    s.senpatcher = HasSenPatcher(game_dir);
    s.old_senpatcher = HasOldSenPatcherBackups(game_dir);
    s.senpatcher_mods = s.senpatcher || Exists(game_dir / "mods/order.txt");
    if (payload != nullptr) s.icon_pack = GetIconPackStatus(game_dir, payload->icon_pack, s.senpatcher_mods);
    if (s.icon_pack.CanBuild() && s.record["icon_pack_removed"].AsBool(false)
        && !Exists(game_dir / "mods" / kIconPackFile)) {
        s.icon_pack.state = IconPackState::Removed;
        s.icon_pack.text = "removed (installs leave it out)";
    }
    if (s.proxy_is_ours && !s.orig_present) {
        s.hard_stops.push_back(std::string("the loader is installed but the game's own library (") + kOrigDll
                               + ") is gone, so there is nothing to forward to - " + kVerifyGameFiles + ", then install again");
    } else if (!s.proxy_present && !s.orig_present) {
        s.hard_stops.push_back(std::string("the game's ") + kProxyDll
                               + " is missing - " + kVerifyGameFiles + " first");
    }

    const std::vector<std::string> enabled = DllStems(game_dir / kModDir);
    const std::vector<std::string> parked = DllStems(game_dir / kDisabledDir);
    std::vector<std::string> names = enabled;
    for (const std::string& n : parked) {
        if (!Contains(names, n)) names.push_back(n);
    }
    if (payload != nullptr) {
        for (const ModInfo& m : payload->mods) {
            if (!Contains(names, m.name)) names.push_back(m.name);
        }
    }
    for (const std::string& n : names) {
        ModState m;
        m.name = n;
        m.enabled = Contains(enabled, n);
        m.disabled = Contains(parked, n);
        m.in_payload = payload != nullptr && payload->FindMod(n) != nullptr;
        s.mods.push_back(m);
    }

    // What a repair would fix: only meaningful for the components whose installed version is the
    // payload's.
    auto same_version = [&](const std::string& component) {
        const Component* c = payload->FindComponent(component);
        auto it = s.installed_versions.find(component);
        return c != nullptr && it != s.installed_versions.end() && it->second == c->version;
    };
    if (payload != nullptr && (s.installed || s.record_present)) {
        if (!s.proxy_is_ours) {
            s.problems.push_back(std::string(kProxyDll) + " is the game's own again (the store's file check?) - the loader is not active");
        } else if (same_version(kLoaderComponent) && Md5File(game_dir / kProxyDll) != payload->ExpectedMd5(kProxyDll)) {
            s.problems.push_back(std::string(kProxyDll) + " differs from this version's loader");
        }
        for (const ModState& m : s.mods) {
            if (!m.in_payload || !same_version(m.name)) continue;
            const std::string expect = payload->ExpectedMd5(std::string(kModDir) + "/" + m.name + ".dll");
            if (m.enabled) {
                if (Md5File(game_dir / kModDir / Path(m.name + ".dll")) != expect) {
                    s.problems.push_back(m.name + ".dll differs from this version");
                }
            } else if (!m.disabled) {
                s.problems.push_back(m.name + ".dll is missing");
            }
        }
    }
    return s;
}

// ---------------------------------------------------------------- install
InstallResult Install(const fs::path& game, const Payload& payload, const InstallOptions& options) {
    InstallResult r;
    const GameStatus status = Inspect(game, &payload);
    if (!status.hard_stops.empty()) {
        r.error = status.hard_stops.front();
        return r;
    }
    if (status.verdict == ExeVerdict::Unverified && !status.unverified_acknowledged && !options.acknowledge_unverified) {
        r.needs_ack = true;
        r.error = "unverified game build (ed8.exe md5 " + status.exe_md5
                  + "): not a build the mods were checked against (a game patch, or another store's build); "
                    "the mods hook fixed addresses, so it can crash or lose features - confirm to install anyway";
        if (status.old_senpatcher) r.error += std::string(" (") + kOldSenPatcherHint + ")";
        return r;
    }
    std::string why;
    if (!payload.VerifyFiles(&why)) {
        r.error = "the payload is damaged: " + why;
        return r;
    }
    const std::string tag = options.tag.empty() ? TimeTag() : options.tag;
    Logf("install: %s into %s", payload.Summary().c_str(), U8(game).c_str());
    if (status.verdict == ExeVerdict::Unverified) {
        Log("WARNING: unverified game build, ed8.exe md5 " + status.exe_md5 + " - installing as acknowledged");
    }
    r.game_running = status.game_running;
    if (r.game_running) {
        Log("WARNING: ed8.exe is running - the new files are loaded on the game's next start");
        r.notes.push_back("the game is running: restart it for the new files to be loaded");
    }

    // The selection is decided before anything moves: what is enabled or parked now stays so, a
    // mod new in this payload gets its default.
    std::set<std::string> enable;
    for (const ModInfo& m : payload.mods) {
        bool on;
        if (options.explicit_mods) {
            on = Contains(options.enable, m.name);
        } else {
            bool found = false;
            on = false;
            for (const ModState& st : status.mods) {
                if (!IEquals(st.name, m.name) || (!st.enabled && !st.disabled)) continue;
                found = true;
                on = st.enabled;
            }
            if (!found) on = m.default_enabled;
        }
        if (on) enable.insert(Lower(m.name));
    }
    if (options.explicit_mods) {
        for (const std::string& n : options.enable) {
            if (payload.FindMod(n) == nullptr) {
                r.error = "no mod named '" + n + "' in this payload";
                return r;
            }
        }
    }

    Backup backup;
    if (!MakeBackup(game, tag, "install", &backup, &r.error)) return r;
    r.backup_dir = backup.dir;

    // From here on any failure rolls back to the backup.
    struct Written {
        std::string game_rel;
        std::string md5;
    };
    std::vector<Written> written;
    auto rollback = [&](const std::string& why_failed) {
        Log("FAILED: " + why_failed);
        Log("rolling back from " + U8(backup.dir) + " ...");
        std::string err;
        if (RestoreFrom(game, backup.dir, &err)) {
            Log("rolled back: the game folder is as it was before this install");
        } else {
            Log("ROLLBACK INCOMPLETE: " + err + " - the backup is in " + U8(backup.dir));
        }
        r.rolled_back = true;
        r.error = why_failed;
        return r;
    };
    auto put = [&](const std::string& payload_rel, const std::string& game_rel) {
        const PayloadFile* f = payload.FindFile(payload_rel);
        std::string err;
        if (f == nullptr || !CopyFileAtomic(f->source, game / Path(game_rel), &err)) return false;
        written.push_back({game_rel, f->md5});
        return true;
    };

    RemoveLegacy(game, payload.remove_rules);
    std::error_code ec;
    fs::create_directories(game / kModDir, ec);

    // The game's own library is kept, renamed; the loader forwards its exports to it. A proxy that
    // is not ours is the game's (a store's file check puts it back over ours), so it refreshes .orig.
    if (status.proxy_present && !status.proxy_is_ours) {
        std::string err;
        if (!CopyFileAtomic(game / kProxyDll, game / kOrigDll, &err)) return rollback(err);
        Log(std::string("kept the game's own library as ") + kOrigDll);
    }
    if (!put(kProxyDll, kProxyDll)) return rollback(std::string("cannot write ") + kProxyDll);
    Log(std::string("+ ") + kProxyDll + " (the loader; forwards to " + kOrigDll + ")");

    for (const ModInfo& m : payload.mods) {
        const std::string file = m.name + ".dll";
        const std::string from = std::string(kModDir) + "/" + file;
        const bool on = enable.count(Lower(m.name)) != 0;
        const std::string to = on ? std::string(kModDir) + "/" + file : std::string(kDisabledDir) + "/" + file;
        const std::string other = on ? std::string(kDisabledDir) + "/" + file : std::string(kModDir) + "/" + file;
        if (!put(from, to)) return rollback("cannot write " + to);
        std::string err;
        if (Exists(game / Path(other)) && !RemoveFile(game / Path(other), &err)) return rollback(err);
        Log((on ? "+ " : "- ") + to + (on ? "" : " (disabled)"));
    }

    // The documented configs travel with the payload, but an ini the player already has is theirs:
    // it is only written when it is missing.
    for (const PayloadFile& f : payload.files) {
        const std::string& rel = f.rel;
        if (!StartsWith(rel, std::string(kModDir) + "/") || !EndsWith(Lower(rel), ".ini")) continue;
        if (Exists(game / Path(rel))) {
            Log("= kept the existing " + rel);
            continue;
        }
        if (!put(rel, rel)) return rollback("cannot write " + rel);
        Log("+ " + rel + " (documented template)");
    }

    // The loader's settings say which folder the mods come from; an experiment once left ModDir on
    // atmt_mods2 and the freshly installed mods were never loaded. Only ModDir is touched.
    const fs::path loader_ini = game / "atmt_loader.ini";
    char moddir[512];
    if (atmt_ini::Read(loader_ini.wstring().c_str(), "Loader", "ModDir", moddir, sizeof(moddir), nullptr)
        && Trim(moddir) != kModDir) {
        atmt_ini::Entry e{"ModDir", kModDir, ""};
        atmt_ini::WriteValues(loader_ini.wstring().c_str(), "Loader", &e, 1);
        Log(std::string("! atmt_loader.ini: ModDir was '") + moddir + "' - set back to " + kModDir);
    }

    if (options.before_verify) options.before_verify();

    // Only what this run wrote is checked: the inis that were kept are the player's own.
    Logf("verifying the %u file(s) this run wrote ...", static_cast<unsigned>(written.size()));
    for (const Written& w : written) {
        const std::string got = Md5File(game / Path(w.game_rel));
        if (got != w.md5) return rollback(w.game_rel + " does not match the payload's manifest (" + (got.empty() ? "missing" : got) + ")");
        r.written.push_back(w.game_rel);
    }
    Log("verified: every written file matches manifest.md5");

    // A key a newer mod version added is written into the player's ini with its help text (after
    // the check: a template written above is then no longer the manifest's file).
    AddMissingKeys(game, Schema::FromJson(payload.schema));

    // The icon pack is built here from the textures in use (it cannot ship: it is made of them).
    // It is an extra: a failure is a note, not a rollback. Only small screens need it, so a new one
    // is made only when asked for; one that is there is rebuilt to follow the textures in use.
    const bool have_pack = Exists(game / "mods" / kIconPackFile) || Exists(game / "mods" / kIconPackRecord);
    if (options.icon_pack != InstallOptions::IconPack::Skip && !payload.icon_pack.empty()) {
        if (options.icon_pack == InstallOptions::IconPack::Refresh && !have_pack) {
            Log("= icon pack not built: only small screens need it (Status > Build icon pack)");
        } else if (!status.senpatcher_mods) {
            Log("= icon pack not built: SenPatcher is not set up here (only its mods/ loader reads the pack)");
        } else if (status.record["icon_pack_removed"].AsBool(false)) {
            Log("= icon pack left out: it was removed (Status > Build icon pack adds it back)");
        } else if (status.game_running) {
            Log("= icon pack not rebuilt: the game is running");
            r.notes.push_back("the icon pack was not rebuilt while the game runs (Status > Rebuild icon pack)");
        } else {
            Log("building the icon pack from the textures in use ...");
            const IconPackResult ip = InstallIconPack(game, payload.icon_pack);
            if (!ip.ok) {
                Log("icon pack: " + ip.error);
                r.notes.push_back("the icon pack was not built: " + ip.error);
            } else {
                r.notes.push_back(ip.packages > 0 ? "the icon pack was built from the textures in use"
                                                  : "no icon pack needed: the textures in use are small enough");
            }
        }
    }

    Json record = ReadInstallRecord(game);
    Json history = record["history"].is_array() ? record["history"] : Json::MakeArray();
    const bool icon_pack_removed = record["icon_pack_removed"].AsBool(false);
    record = Json::MakeObject();
    if (icon_pack_removed) record["icon_pack_removed"] = Json(true);
    record["manager_version"] = AppVersion();
    Json versions = Json::MakeObject();
    for (const auto& kv : payload.Versions()) versions[kv.first] = kv.second;
    record["components"] = versions;
    record["summary"] = payload.Summary();
    record["installed_at"] = IsoTimeUtc();
    record["backup"] = U8(backup.dir.filename());
    record["exe_md5"] = status.exe_md5;
    record["exe_verified"] = status.verdict == ExeVerdict::Supported;
    if (status.verdict == ExeVerdict::Unverified) {
        record["unverified_ack_md5"] = status.exe_md5;
        record["warning"] = "installed on an unverified ed8.exe build";
    }
    Json files = Json::MakeArray();
    for (const std::string& f : r.written) files.Push(f);
    record["files"] = files;
    Json entry = Json::MakeObject();
    entry["action"] = "install";
    entry["components"] = versions;
    entry["at"] = IsoTimeUtc();
    entry["backup"] = U8(backup.dir.filename());
    history.Push(entry);
    while (history.elements().size() > 20) history.elements().erase(history.elements().begin());
    record["history"] = history;
    if (!WriteInstallRecord(game, record)) return rollback(std::string("cannot write ") + kInstallRecord);
    PruneBackups(game);

    Log("installed into " + U8(game));
    Log("  backup:      " + U8(backup.dir));
    Log("  loader log:  " + U8(game / "atmt_loader.log"));
    r.ok = true;
    return r;
}

// ---------------------------------------------------------------- install everything
InstallResult InstallEverything(const fs::path& game, const Payload& payload, const InstallOptions& base) {
    InstallOptions o = base;
    o.explicit_mods = true;
    o.enable.clear();
    // every mod on, except what a recommended preset switches off
    std::vector<std::pair<std::string, bool>> switches;
    for (const Preset& p : payload.presets) {
        if (std::find(p.recommended_on.begin(), p.recommended_on.end(), PlatformName()) == p.recommended_on.end()) continue;
        switches.insert(switches.end(), p.mods.begin(), p.mods.end());
        if (p.icon_pack && o.icon_pack == InstallOptions::IconPack::Refresh) o.icon_pack = InstallOptions::IconPack::Build;
    }
    for (const ModInfo& m : payload.mods) {
        bool on = true;
        for (const auto& s : switches) {
            if (IEquals(s.first, m.name)) on = s.second;
        }
        if (on) o.enable.push_back(m.name);
    }
    InstallResult r = Install(game, payload, o);
    if (!r.ok) return r;
    SettingsModel model;
    model.Load(game, Schema::FromJson(payload.schema));
    std::vector<std::string> applied;
    for (const Preset& p : payload.presets) {
        if (std::find(p.recommended_on.begin(), p.recommended_on.end(), PlatformName()) == p.recommended_on.end()) continue;
        std::string why;
        if (!model.ApplyPreset(p, &why)) {
            r.notes.push_back("preset \"" + p.name + "\" not applied: " + why);
            continue;
        }
        applied.push_back(p.name);
    }
    if (model.Dirty()) {
        std::string why;
        if (GameRunning()) {
            if (!model.Queue(&why)) r.notes.push_back("the recommended settings could not be queued: " + why);
        } else if (model.Save(&why) != SettingsModel::SaveResult::Saved) {
            r.notes.push_back("the recommended settings could not be written: " + why);
        }
    }
    for (const std::string& name : applied) {
        r.notes.push_back("preset \"" + name + "\" applied");
    }
    return r;
}

// ---------------------------------------------------------------- uninstall
InstallResult Uninstall(const fs::path& game, const UninstallOptions& options) {
    InstallResult r;
    const GameStatus status = Inspect(game, nullptr);
    if (!status.dir_exists) {
        r.error = "the folder does not exist";
        return r;
    }
    // everything the toolkit ever put here starts with atmt_ (and so do our files in SenPatcher's mods/)
    std::vector<fs::path> toolkit;
    std::error_code ec;
    for (fs::directory_iterator it(game, ec), end; !ec && it != end; it.increment(ec)) {
        if (StartsWith(U8(it->path().filename()), kToolkitPrefix)) toolkit.push_back(it->path());
    }
    if (!status.proxy_is_ours && !status.record_present && !IsDir(game / kModDir) && (!options.purge || toolkit.empty())) {
        r.error = "the mods are not installed here";
        return r;
    }
    if (status.game_running) {
        r.error = "close the game first: its files are in use";
        return r;
    }
    if (status.proxy_is_ours && !status.orig_present) {
        r.error = std::string("the game's own ") + kProxyDll
                  + " is not here to put back - " + kVerifyGameFiles + ", which replaces the loader with it, then "
                    "uninstall again for the rest";
        return r;
    }
    Backup backup;
    if (!options.purge) {
        if (!MakeBackup(game, options.tag.empty() ? "uninstall_" + TimeTag() : options.tag, "uninstall", &backup, &r.error)) {
            return r;
        }
        r.backup_dir = backup.dir;
    }
    std::string err;
    bool ok = true;
    if (status.orig_present) {
        bool restored = true;
        if (status.proxy_is_ours || !status.proxy_present) {
            restored = CopyFileAtomic(game / kOrigDll, game / kProxyDll, &err);
            if (restored) Log(std::string("restored the game's own ") + kProxyDll);
        }
        // .orig only goes once the game's library is back in place: without it our loader would be
        // left with nothing to forward to, and the game would not start
        if (restored) {
            ok = RemoveFile(game / kOrigDll, &err) && ok;
        } else {
            ok = false;
            Log(std::string("kept ") + kOrigDll + ": " + err);
        }
    }
    // the loader's and the mods' files and settings (and the icon pack, out of order.txt too)
    for (const std::string& rel : ManagedFiles(game)) {
        if (rel == kProxyDll || rel == kOrigDll || rel == "mods/order.txt") continue;
        ok = RemoveFile(game / Path(rel), &err) && ok;
        if (StartsWith(rel, "mods/")) {
            std::string order;
            if (ReadFile(game / "mods/order.txt", &order)) {
                std::string updated;
                for (const std::string& line : Lines(order)) {
                    if (Trim(line) != rel.substr(5)) updated += line + "\n";
                }
                WriteFileAtomic(game / "mods/order.txt", updated);
            }
            Log("removed " + rel);
        }
    }
    for (const char* f : {"atmt_manager_pending.json", kInstallRecord}) ok = RemoveFile(game / f, &err) && ok;
    fs::remove_all(game / kModDir, ec);
    if (ec) {
        ok = false;
        err = "cannot remove " + std::string(kModDir) + ": " + ec.message();
    }
    if (options.purge) {
        // everything else of the toolkit's: logs, the mods' output files, the backups, caches
        for (const fs::path& p : toolkit) {
            if (!Exists(p)) continue;
            fs::remove_all(p, ec);
            if (ec) {
                ok = false;
                err = "cannot remove " + U8(p.filename()) + ": " + ec.message();
            } else {
                Log("removed " + U8(p.filename()) + (IsDir(p) ? "/" : ""));
            }
        }
    }
    CleanupOldFiles(game, false);
    if (!ok) {
        r.error = err + (options.purge ? std::string() : " - the backup in " + U8(backup.dir) + " has everything");
        return r;
    }
    if (options.purge) {
        Log("removed everything of the mods from " + U8(game) + ": the game folder is as the store installed it");
    } else {
        PruneBackups(game);
        Log("uninstalled from " + U8(game) + " (the logs and the newest backups were kept)");
        Log("  everything removed is in " + U8(backup.dir));
    }
    r.ok = true;
    return r;
}

// ---------------------------------------------------------------- mods on/off
std::vector<std::pair<std::string, bool>> PresetModChanges(const fs::path& game, const Preset& preset) {
    std::vector<std::pair<std::string, bool>> out;
    for (const auto& s : preset.mods) {
        const bool on = Exists(game / kModDir / Path(s.first + ".dll"));
        const bool off = Exists(game / kDisabledDir / Path(s.first + ".dll"));
        if (!on && !off) continue;   // not installed: nothing to switch
        if (on == s.second && !(on && off)) continue;
        out.push_back(s);
    }
    return out;
}

bool PresetBuildsIconPack(const Preset& preset, const IconPackStatus& status) {
    return preset.icon_pack && (status.state == IconPackState::NotBuilt || status.state == IconPackState::Stale);
}

bool ApplyPresetMods(const fs::path& game, const Preset& preset, std::vector<std::string>* changed, std::string* error) {
    for (const auto& s : PresetModChanges(game, preset)) {
        if (!SetModEnabled(game, s.first, s.second, error)) return false;
        if (changed != nullptr) changed->push_back(s.first);
    }
    return true;
}

bool SetModEnabled(const fs::path& game, const std::string& mod, bool enabled, std::string* error) {
    // a dll stem in atmt_mods/ (a developer's own dll may have any name, so only what would leave
    // the folder is refused)
    if (mod.empty() || mod == "." || mod == ".." || mod.find_first_of("/\\:") != std::string::npos) {
        if (error != nullptr) *error = "'" + mod + "' is not a mod name";
        return false;
    }
    const fs::path on = game / kModDir / Path(mod + ".dll");
    const fs::path off = game / kDisabledDir / Path(mod + ".dll");
    const fs::path& from = enabled ? off : on;
    const fs::path& to = enabled ? on : off;
    if (Exists(to) && !Exists(from)) return true;
    if (!Exists(from)) {
        if (error != nullptr) *error = mod + ".dll is not installed";
        return false;
    }
    std::error_code ec;
    fs::create_directories(to.parent_path(), ec);
    std::string err;
    if (Exists(to) && !RemoveFile(to, &err)) {
        if (error != nullptr) *error = err;
        return false;
    }
    fs::rename(from, to, ec);
    if (ec) {
        if (error != nullptr) *error = "cannot move " + mod + ".dll: " + ec.message();
        return false;
    }
    Log(std::string(enabled ? "enabled " : "disabled ") + mod + (enabled ? "" : " (moved to atmt_mods/disabled)"));
    return true;
}

// ---------------------------------------------------------------- backups
std::vector<BackupInfo> ListBackups(const fs::path& game) {
    std::vector<BackupInfo> out;
    std::error_code ec;
    for (fs::directory_iterator it(game, ec), end; !ec && it != end; it.increment(ec)) {
        const std::string name = U8(it->path().filename());
        if (!it->is_directory() || !StartsWith(name, "atmt_backup_")) continue;
        BackupInfo b;
        b.name = name;
        b.dir = it->path();
        std::string text;
        Json m;
        if (ReadFile(b.dir / "backup.json", &text) && Json::Parse(text, &m)) {
            b.created = m.Str("created");
            b.created_ms = m["created_ms"].AsInt(0);
            b.reason = m.Str("reason");
            b.installed = m.Str("installed");
            for (const auto& kv : m["items"].items()) {
                if (kv.second.AsBool()) b.items.push_back(kv.first);
            }
        } else {
            b.reason = "deck_install.sh";
            for (const std::string& rel : ManagedFiles(b.dir)) {
                if (Exists(b.dir / Path(rel))) b.items.push_back(rel);
            }
            if (IsDir(b.dir / kModDir)) b.items.push_back(std::string(kModDir) + "/");
        }
        out.push_back(b);
    }
    std::sort(out.begin(), out.end(), [](const BackupInfo& a, const BackupInfo& b) {
        if (a.created_ms != b.created_ms) return a.created_ms > b.created_ms;
        return a.created != b.created ? a.created > b.created : a.name > b.name;
    });
    return out;
}

void PruneBackups(const fs::path& game, size_t keep) {
    const std::vector<BackupInfo> backups = ListBackups(game);   // newest first
    for (size_t i = keep; i < backups.size(); ++i) {
        std::error_code ec;
        fs::remove_all(backups[i].dir, ec);
        if (ec) {
            Log("  could not remove the old backup " + backups[i].name + ": " + ec.message());
        } else {
            Log("  removed the old backup " + backups[i].name);
        }
    }
}

InstallResult RestoreBackup(const fs::path& game, const std::string& backup_name) {
    InstallResult r;
    const fs::path dir = game / Path(backup_name);
    if (!StartsWith(backup_name, "atmt_backup_") || !IsDir(dir) || backup_name.find('/') != std::string::npos
        || backup_name.find('\\') != std::string::npos) {
        r.error = "no backup named " + backup_name;
        return r;
    }
    if (GameRunning()) {
        r.error = "close the game first: its files are in use";
        return r;
    }
    Backup before;
    if (!MakeBackup(game, "before_restore_" + TimeTag(), "restore", &before, &r.error)) return r;
    r.backup_dir = before.dir;
    if (!RestoreFrom(game, dir, &r.error)) return r;
    PruneBackups(game);
    Log("restored " + backup_name + " (the state before it is in " + U8(before.dir.filename()) + ")");
    r.ok = true;
    return r;
}

}  // namespace atmt
