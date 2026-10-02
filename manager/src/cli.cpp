// cli.cpp - atmt_manager --cli <command>: everything manager_core does, scriptable.
//
// The developer flow (tools\deploy.ps1, tools\deploy_deck.ps1 over ssh) runs exactly this, so a
// developer and a player run the same install code.
#include "cli.h"

#include <cstdio>
#include <cstring>

#include "core/hash.h"
#include "core/icon_pack.h"
#include "core/install.h"
#include "core/locator.h"
#include "core/payload.h"
#include "core/platform.h"
#include "core/senpatcher.h"
#include "core/settings.h"
#include "core/sign.h"
#include "core/steam_shortcuts.h"
#include "core/updater.h"
#include "core/util.h"

namespace atmt {

namespace {

const char kUsage[] =
    "atmt_manager %s - installs and configures the ATMT loader and mods\n"
    "\n"
    "usage: atmt_manager --cli <command> [options]\n"
    "\n"
    "  status                      the game folder, ed8.exe verdict, what is installed\n"
    "  find                        every install of the game (Steam, GOG, Heroic, Lutris, Bottles, Wine)\n"
    "  install                     install / update / repair (backup, verify, roll back on failure)\n"
    "      --all                   everything: every mod on, and the presets recommended here\n"
    "      --mods a,b              enable exactly these mods (default: keep the current choice)\n"
    "      --preset <id>           apply a preset afterwards\n"
    "      --tag <tag>             name of the backup folder (atmt_backup_<tag>)\n"
    "      --yes                   install on an unverified ed8.exe build without asking\n"
    "      --icon-pack             build the icon pack (default: only rebuild one that is there;\n"
    "                              --all builds it where the preset is for a small screen)\n"
    "      --no-icon-pack          do not (re)build the icon pack\n"
    "  uninstall [--tag <tag>]     put the game's own files back (everything is backed up first)\n"
    "      --purge                 also every log, output file and backup: the folder as the store installed it\n"
    "  icon-pack status|build|remove   the icon pack: small UI icons pre-shrunk from the textures in use,\n"
    "                              mods/atmt_icon_pack.p3a first in mods/order.txt (needs SenPatcher).\n"
    "                              Only needed on small screens (Steam Deck, 720p). remove: installs\n"
    "                              leave it out from then on; build takes that back\n"
    "      --out <file>            build: write the pack there instead (the game folder is only read)\n"
    "      --config <json>         build: these targets instead of the payload's icon_pack.json\n"
    "  senpatcher status|download|run|override   SenPatcher's own window. run: downloads the latest\n"
    "                              release (when newer than the copy here) and opens SenPatcher; on Linux\n"
    "                              (the Steam version) through the game's Proton, after setting the dll\n"
    "                              override dinput8=n,b in the game's prefix; override: only the override\n"
    "  steam add|remove|status     this app in the Steam library (as a non-Steam game)\n"
    "      --restart-steam         close Steam for the change and start it again (not in Game Mode)\n"
    "  enable <mod> | disable <mod>\n"
    "  settings [<mod>]            every setting and its current value\n"
    "  set <mod> <section> <key> <value>\n"
    "  presets                     the presets and what each would change\n"
    "  preset <id>                 apply a preset\n"
    "  reset [<mod>]               settings back to their defaults\n"
    "  backups                     the backups in the game folder\n"
    "  restore <backup>            bring a backup back (the current state is backed up first)\n"
    "  check-update [--force]      read the release manifest: the newer app and components\n"
    "  update [--yes]              download the newer components and install them (not the app itself)\n"
    "  verify-payload              check every component against its manifest.md5\n"
    "  keygen <dir>                a release signing key pair (atmt_release.pub / .key)\n"
    "  sign <secret key> <file>    write <file>.sig (minisign format)\n"
    "  verify-sig <pub key> <file> check <file>.sig\n"
    "  verify-manifest <pub key> <manifest.json>   check a release manifest and list what it offers\n"
    "\n"
    "common options:\n"
    "  --game-dir <dir>            the game folder (default: the last one used, else the one found)\n"
    "  --payload <dir>             a folder of components to install (default: the newest of each the app has)\n";

struct Args {
    std::string command;
    std::vector<std::string> positional;
    std::map<std::string, std::string> options;
    std::set<std::string> flags;

    bool Has(const std::string& f) const { return flags.count(f) != 0 || options.count(f) != 0; }
    std::string Opt(const std::string& k, const std::string& def = std::string()) const {
        auto it = options.find(k);
        return it == options.end() ? def : it->second;
    }
};

const std::set<std::string> kValueOptions = {"--game-dir", "--payload", "--mods", "--preset", "--tag", "--comment", "--out", "--config"};

Args Parse(const std::vector<std::string>& argv) {
    Args a;
    for (size_t i = 0; i < argv.size(); ++i) {
        const std::string& s = argv[i];
        if (s == "--cli") continue;
        if (StartsWith(s, "--")) {
            const size_t eq = s.find('=');
            if (eq != std::string::npos) {
                a.options[s.substr(0, eq)] = s.substr(eq + 1);
            } else if (kValueOptions.count(s) != 0 && i + 1 < argv.size()) {
                a.options[s] = argv[++i];
            } else if (a.command.empty() && (s == "--install" || s == "--uninstall" || s == "--status")) {
                a.command = s.substr(2);   // the design's spelling: --install --game-dir ...
            } else {
                a.flags.insert(s);
            }
        } else if (a.command.empty()) {
            a.command = s;
        } else {
            a.positional.push_back(s);
        }
    }
    return a;
}

int Fail(const std::string& why) {
    std::fprintf(stderr, "ERROR: %s\n", why.c_str());
    return 1;
}

bool ResolveGame(const Args& a, fs::path* out, std::string* why) {
    std::string dir = a.Opt("--game-dir");
    if (dir.empty()) dir = LoadAppState().Str("game_dir");
    if (!dir.empty()) {
        const fs::path resolved = ResolveGameDir(dir);   // ed8.exe itself, quotes: the folder
        *out = resolved.empty() ? Path(dir) : resolved;
        return true;
    }
    const std::vector<GameInstall> games = FindGames();
    if (games.size() == 1) {
        *out = games[0].dir;
        return true;
    }
    if (games.empty()) {
        *why = "the game was not found (Steam, GOG, Heroic, Lutris, Bottles, Wine) - pass --game-dir "
               "with the folder that has ed8.exe";
    } else {
        *why = "several installs found - pass --game-dir with one of:";
        for (const GameInstall& g : games) *why += "\n  " + U8(g.dir);
    }
    return false;
}

bool ResolvePayload(const Args& a, Payload* out, std::string* why) {
    const std::string dir = a.Opt("--payload");
    if (!dir.empty()) return Payload::Load(Path(dir), out, why);
    return FindBestPayload(out, why);
}

void Remember(const fs::path& game) {
    UpdateAppState([&](Json& st) { st["game_dir"] = U8(game); });
}

const char* VerdictName(ExeVerdict v) {
    switch (v) {
        case ExeVerdict::Supported: return "supported build";
        case ExeVerdict::Unverified: return "UNVERIFIED build";
        default: return "missing";
    }
}

int CmdStatus(const Args& a) {
    fs::path game;
    std::string why;
    if (!ResolveGame(a, &game, &why)) return Fail(why);
    Payload payload;
    const bool have_payload = ResolvePayload(a, &payload, &why);
    const GameStatus s = Inspect(game, have_payload ? &payload : nullptr);
    const GameInstall g = DescribeGameDir(game, FindGames());
    std::printf("game folder     %s%s\n", U8(game).c_str(), g.source.empty() ? "" : ("  (" + g.source + ")").c_str());
    if (!g.prefix.empty() && PlatformName() == "linux") {
        std::printf("%s   %s%s\n", g.IsSteam() ? "proton prefix" : "wine prefix  ", U8(g.prefix).c_str(),
                    g.prefix_exists ? "" : " (not created yet)");
    }
    std::printf("ed8.exe         %s%s%s\n", VerdictName(s.verdict), s.exe_md5.empty() ? "" : ", md5 ", s.exe_md5.c_str());
    if (s.old_senpatcher && s.verdict == ExeVerdict::Unverified) std::printf("                %s\n", kOldSenPatcherHint);
    std::printf("senpatcher      %s\n", s.senpatcher ? "yes (DINPUT8.dll)" : s.senpatcher_mods ? "mods/order.txt only" : "no");
    if (s.icon_pack.state != IconPackState::NoConfig) std::printf("icon pack       %s\n", s.icon_pack.text.c_str());
    std::printf("loader          %s\n", s.proxy_is_ours ? "installed" : s.proxy_present ? "not installed (the game's own dll)" : "missing");
    std::printf("installed       %s\n", s.record.Str("summary", "-").c_str());
    std::printf("payload         %s\n", have_payload ? payload.Summary().c_str() : ("none: " + why).c_str());
    if (have_payload && (s.installed || s.record_present)) {
        for (const std::string& o : s.Outdated(payload)) std::printf("  newer here:   %s\n", o.c_str());
    }
    std::printf("game running    %s\n", s.game_running ? "yes" : "no");
    for (const ModState& m : s.mods) {
        const ModInfo* info = have_payload ? payload.FindMod(m.name) : nullptr;
        auto it = s.installed_versions.find(m.name);
        const std::string version = it != s.installed_versions.end() && (m.enabled || m.disabled) ? it->second : std::string();
        std::printf("  [%s] %-22s %-10s %s\n", m.enabled ? "x" : " ", m.name.c_str(), version.c_str(),
                    m.enabled ? (info != nullptr ? info->title.c_str() : "")
                              : m.disabled ? "(disabled)" : "(not installed)");
    }
    for (const std::string& p : s.problems) std::printf("problem: %s\n", p.c_str());
    for (const std::string& h : s.hard_stops) std::printf("cannot install: %s\n", h.c_str());
    if (HasQueuedSettings(game)) std::printf("queued settings are waiting for the game to exit\n");
    const GameStatus::Action action = s.SuggestedAction(have_payload ? &payload : nullptr);
    if (action != GameStatus::Action::None) std::printf("next step       %s\n", ActionName(action));
    return 0;
}

int CmdFind() {
    const std::vector<fs::path> libs = SteamLibraries(SteamRoots());
    for (const fs::path& l : libs) std::printf("library  %s\n", U8(l).c_str());
    std::vector<GameInstall> games = FindGames(libs);
    MergeGames(games, FindOtherGames());
    for (const GameInstall& g : games) {
        std::printf("game     %s  (%s)\n", U8(g.dir).c_str(), g.source.empty() ? "folder" : g.source.c_str());
        if (!g.prefix.empty() && PlatformName() == "linux") std::printf("  prefix %s%s\n", U8(g.prefix).c_str(), g.prefix_exists ? "" : " (not created yet)");
    }
    if (games.empty()) std::printf("the game was not found\n");
    return games.empty() ? 1 : 0;
}

int ApplyPresetCmd(const fs::path& game, const Payload& payload, const std::string& id) {
    for (const Preset& p : payload.presets) {
        if (p.id != id) continue;
        SettingsModel model;
        model.Load(game, Schema::FromJson(payload.schema));
        std::string why;
        if (!model.ApplyPreset(p, &why)) return Fail(why);
        if (GameRunning()) Log("WARNING: the game is running - a mod in the game may write these inis too");
        if (model.Save(&why) == SettingsModel::SaveResult::Failed) return Fail(why);
        if (!ApplyPresetMods(game, p, nullptr, &why)) return Fail(why);
        if (GameRunning() && !p.mods.empty()) Log("the game is running: switched mods take effect on its next start");
        if (PresetBuildsIconPack(p, Inspect(game, &payload).icon_pack)) {
            if (GameRunning()) {
                Log("the game is running: the icon pack was not built (icon-pack build once it has exited)");
            } else {
                Log("building the icon pack from the textures in use ...");
                const IconPackResult r = BuildIconPackByPlayer(game, payload.icon_pack);
                Log(r.ok ? std::string("the icon pack is built") : "the icon pack was not built: " + r.error);
            }
        }
        Log("preset \"" + p.name + "\" applied");
        return 0;
    }
    return Fail("no preset '" + id + "'");
}

int CmdInstall(const Args& a) {
    fs::path game;
    std::string why;
    if (!ResolveGame(a, &game, &why)) return Fail(why);
    Payload payload;
    if (!ResolvePayload(a, &payload, &why)) return Fail(why);
    InstallOptions o;
    o.tag = a.Opt("--tag");
    o.acknowledge_unverified = a.Has("--yes");
    if (a.Has("--icon-pack") && a.Has("--no-icon-pack")) return Fail("--icon-pack and --no-icon-pack do not go together");
    if (a.Has("--icon-pack")) o.icon_pack = InstallOptions::IconPack::Build;
    if (a.Has("--no-icon-pack")) o.icon_pack = InstallOptions::IconPack::Skip;
    if (a.Has("--mods")) {
        o.explicit_mods = true;
        for (const std::string& m : Split(a.Opt("--mods"), ',')) {
            if (!Trim(m).empty()) o.enable.push_back(Trim(m));
        }
    }
    if (a.Has("--all") && a.Has("--mods")) return Fail("--all and --mods do not go together");
    const InstallResult r = a.Has("--all") ? InstallEverything(game, payload, o) : Install(game, payload, o);
    for (const std::string& n : r.notes) Log(n);
    if (!r.ok) {
        if (r.needs_ack) {
            const GameStatus s = Inspect(game, &payload);
            std::fprintf(stderr, "ed8.exe md5 %s is not a build these mods were checked against (a game patch, or\n"
                                 "a GOG build):\n", s.exe_md5.c_str());
            for (const SupportedExe& e : payload.supported) std::fprintf(stderr, "  supported: %s  %s\n", e.md5.c_str(), e.note.c_str());
            std::fprintf(stderr, "The mods hook fixed addresses, so a game patch can crash them or hide features;\n"
                                 "uninstall restores everything. Run again with --yes to install anyway.\n");
            return 2;
        }
        return Fail(r.error + (r.rolled_back ? " (rolled back)" : ""));
    }
    Remember(game);
    if (a.Has("--preset")) return ApplyPresetCmd(game, payload, a.Opt("--preset"));
    return 0;
}

int CmdUninstall(const Args& a) {
    fs::path game;
    std::string why;
    if (!ResolveGame(a, &game, &why)) return Fail(why);
    UninstallOptions o;
    o.tag = a.Opt("--tag");
    o.purge = a.Has("--purge");
    const InstallResult r = Uninstall(game, o);
    return r.ok ? 0 : Fail(r.error);
}

int CmdIconPack(const Args& a) {
    const std::string what = a.positional.empty() ? "status" : a.positional[0];
    if (what != "status" && what != "build" && what != "remove") return Fail("usage: icon-pack status|build|remove");
    fs::path game;
    std::string why;
    if (!ResolveGame(a, &game, &why)) return Fail(why);
    if (what == "remove") {
        if (!RemoveIconPackByPlayer(game, &why)) return Fail(why);
        Log("the icon pack is removed; installs leave it out until `icon-pack build`");
        return 0;
    }
    IconPackConfig config;
    if (a.Has("--config")) {
        std::string text;
        Json j;
        if (!ReadFile(Path(a.Opt("--config")), &text) || !Json::Parse(text, &j, &why)) {
            return Fail("cannot read " + a.Opt("--config") + " " + why);
        }
        if (!IconPackConfig::FromJson(j, &config, &why)) return Fail(a.Opt("--config") + ": " + why);
    } else {
        Payload payload;
        if (!ResolvePayload(a, &payload, &why)) return Fail(why);
        config = payload.icon_pack;
    }
    if (config.empty()) return Fail("no icon pack targets (the payload has no icon_pack.json)");
    const bool senpatcher = SenPatcherSetUp(game);
    if (what == "status") {
        std::printf("icon pack       %s\n", GetIconPackStatus(game, config, senpatcher).text.c_str());
        return 0;
    }
    if (a.Has("--out")) {
        const IconPackResult r = BuildIconPackFile(game, config, Path(a.Opt("--out")));
        for (const std::string& n : r.notes) Log(n);
        return r.ok ? 0 : Fail(r.error);
    }
    if (!senpatcher) return Fail("SenPatcher is not set up in this game folder: only its mods/ loader reads the icon pack");
    Log("building the icon pack from the textures in use ...");
    const IconPackResult r = BuildIconPackByPlayer(game, config);
    return r.ok ? 0 : Fail(r.error);
}

int CmdSenPatcher(const Args& a) {
    const std::string what = a.positional.empty() ? "status" : a.positional[0];
    if (what != "status" && what != "download" && what != "run" && what != "override") {
        return Fail("usage: senpatcher status|download|run|override");
    }
    std::string why;
    std::unique_ptr<Http> http = MakeHttp(&why);
    if (what == "download") {
        SenPatcherCopy copy;
        std::string note;
        if (!EnsureSenPatcher(http.get(), &copy, &note, &why)) return Fail(why);
        if (!note.empty()) Log(note);
        std::printf("SenPatcher %s in %s\n", copy.tag.c_str(), U8(copy.dir).c_str());
        return 0;
    }
    fs::path game;
    if (!ResolveGame(a, &game, &why)) return Fail(why);
    const GameInstall g = DescribeGameDir(game, FindGames());
    if (what == "status") {
        const SenPatcherStatus s = GetSenPatcherStatus(g);
        std::printf("senpatcher      %s\n", s.Text().c_str());
        std::printf("copy here       %s\n", s.local_tag.empty() ? "none" : (s.local_tag + " in " + U8(SenPatcherRoot())).c_str());
        if (!s.cannot_run.empty()) std::printf("run             %s\n", s.cannot_run.c_str());
#ifndef _WIN32
        if (s.can_run) {
            const std::vector<fs::path> roots = SteamRoots();
            ProtonLaunch l;
            if (PlanProtonLaunch(g, roots, SteamLibraries(roots), &l, &why)) {
                std::printf("proton          %s (%s)\n                %s\n", l.tool.display.c_str(), l.why.c_str(), U8(l.tool.dir).c_str());
                std::printf("command         %s <program>\n", Join(l.command, " ").c_str());
                std::printf("prefix          %s%s\n", U8(l.prefix).c_str(), IsDir(l.prefix) ? "" : " (not created yet)");
            } else {
                std::printf("proton          not found: %s\n", why.c_str());
            }
        }
#endif
        return 0;
    }
    Remember(game);
    const SenPatcherRun r = RunSenPatcher(game, http.get(), what == "override", [](uint64_t, uint64_t) { return true; });
    if (!r.ok) return Fail(r.error);
    for (const std::string& n : r.notes) Log(n);
    if (what == "run") std::printf("SenPatcher %s: %s\n", r.tag.c_str(), r.installed ? "installed in the game folder" : "the game was not patched");
    return 0;
}

int CmdSteam(const Args& a) {
    const std::string what = a.positional.empty() ? "status" : a.positional[0];
    if (what == "status") {
        const std::vector<SteamAccount> accounts = SteamAccounts();
        if (accounts.empty()) std::printf("no Steam account found on this machine\n");
        for (const SteamAccount& acc : accounts) {
            std::printf("account %-12s %s\n", acc.id.c_str(), acc.has_shortcut ? "in the library" : "not in the library");
        }
        std::printf("Steam is %srunning\n", SteamRunning() ? "" : "not ");
        return 0;
    }
    if (what != "add" && what != "remove") return Fail("usage: steam add|remove|status [--restart-steam]");
    if (SteamAccounts().empty()) {
        return Fail("no Steam account found on this machine - the Steam library entry needs Steam (everything else "
                    "works without it)");
    }
    std::string why;
    const bool restart = SteamRunning();
    if (restart) {
        if (!a.Has("--restart-steam")) {
            return Fail("Steam is running and would undo the change when it exits - close it, or pass --restart-steam");
        }
        Log("closing Steam ...");
        if (!CloseSteam(60000, &why)) return Fail(why);
    }
    int n = 0;
    const bool ok = what == "add" ? AddToSteam(SelfExe(), WriteAppIcon(), &n, &why) : RemoveFromSteam(&n, &why);
    if (restart) {
        Log("starting Steam again ...");
        StartSteam();
    }
    if (!ok) return Fail(why);
    std::printf("%s %d Steam account(s)\n", what == "add" ? "added to" : "removed from", n);
    return 0;
}

int CmdToggle(const Args& a, bool enable) {
    if (a.positional.empty()) return Fail("which mod?");
    fs::path game;
    std::string why;
    if (!ResolveGame(a, &game, &why)) return Fail(why);
    for (const std::string& m : a.positional) {
        if (!SetModEnabled(game, m, enable, &why)) return Fail(why);
    }
    if (GameRunning()) Log("the game is running: this takes effect on its next start");
    return 0;
}

int CmdSettings(const Args& a) {
    fs::path game;
    std::string why;
    if (!ResolveGame(a, &game, &why)) return Fail(why);
    Payload payload;
    if (!ResolvePayload(a, &payload, &why)) return Fail(why);
    SettingsModel model;
    model.Load(game, Schema::FromJson(payload.schema));
    const std::string only = a.positional.empty() ? std::string() : a.positional[0];
    for (const SettingsGroup& g : model.schema().groups) {
        if (!only.empty() && !IEquals(only, g.mod)) continue;
        std::printf("[%s] %s  (%s)\n", g.mod.c_str(), g.title.c_str(), g.ini.c_str());
        for (const SettingDef& d : g.settings) {
            if (!d.stored()) continue;
            const std::string v = model.Get(g.mod, d.section, d.key);
            std::printf("  %-10s %-26s = %-14s%s%s\n", d.section.c_str(), d.key.c_str(), v.c_str(),
                        v == d.def ? "" : ("  (default " + d.def + ")").c_str(), d.advanced ? "  [advanced]" : "");
        }
        for (const RawEntry& r : model.Raw(g.mod)) {
            std::printf("  %-10s %-26s = %s  [raw]\n", r.section.c_str(), r.key.c_str(), r.value.c_str());
        }
    }
    return 0;
}

int CmdSet(const Args& a) {
    if (a.positional.size() != 4) return Fail("usage: set <mod> <section> <key> <value>");
    fs::path game;
    std::string why;
    if (!ResolveGame(a, &game, &why)) return Fail(why);
    Payload payload;
    if (!ResolvePayload(a, &payload, &why)) return Fail(why);
    SettingsModel model;
    model.Load(game, Schema::FromJson(payload.schema));
    const std::string& mod = a.positional[0];
    if (model.schema().Find(mod, a.positional[1], a.positional[2]) != nullptr) {
        if (!model.Set(mod, a.positional[1], a.positional[2], a.positional[3], &why)) return Fail(why);
    } else if (model.schema().Find(mod) != nullptr) {
        model.SetRaw(mod, a.positional[1], a.positional[2], a.positional[3]);
    } else {
        return Fail("no mod '" + mod + "' in the schema");
    }
    if (GameRunning()) Log("WARNING: the game is running - a mod in the game may write this ini too");
    const SettingsModel::SaveResult r = model.Save(&why);
    if (r == SettingsModel::SaveResult::Failed || r == SettingsModel::SaveResult::Conflict) return Fail(why);
    if (r == SettingsModel::SaveResult::Nothing) Log("unchanged");
    return 0;
}

int CmdPresets(const Args& a) {
    fs::path game;
    std::string why;
    if (!ResolveGame(a, &game, &why)) return Fail(why);
    Payload payload;
    if (!ResolvePayload(a, &payload, &why)) return Fail(why);
    SettingsModel model;
    model.Load(game, Schema::FromJson(payload.schema));
    for (const Preset& p : payload.presets) {
        std::printf("%s - %s\n  %s\n", p.id.c_str(), p.name.c_str(), p.description.c_str());
        const auto diff = model.PresetDiff(p);
        const auto mods = PresetModChanges(game, p);
        if (diff.empty() && mods.empty()) std::printf("  (already in place)\n");
        for (const auto& m : mods) std::printf("  mod %s: %s\n", m.first.c_str(), m.second ? "off -> on" : "on -> off");
        for (const auto& d : diff) {
            std::printf("  %s [%s] %s: %s -> %s\n", d.first.mod.c_str(), d.first.section.c_str(), d.first.key.c_str(),
                        d.second.c_str(), d.first.value.c_str());
        }
    }
    return 0;
}

int CmdPreset(const Args& a) {
    if (a.positional.empty()) return Fail("which preset? (see: presets)");
    fs::path game;
    std::string why;
    if (!ResolveGame(a, &game, &why)) return Fail(why);
    Payload payload;
    if (!ResolvePayload(a, &payload, &why)) return Fail(why);
    return ApplyPresetCmd(game, payload, a.positional[0]);
}

int CmdReset(const Args& a) {
    fs::path game;
    std::string why;
    if (!ResolveGame(a, &game, &why)) return Fail(why);
    Payload payload;
    if (!ResolvePayload(a, &payload, &why)) return Fail(why);
    SettingsModel model;
    model.Load(game, Schema::FromJson(payload.schema));
    model.ResetToDefaults(a.positional.empty() ? std::string() : a.positional[0]);
    const SettingsModel::SaveResult r = model.Save(&why);
    if (r == SettingsModel::SaveResult::Failed || r == SettingsModel::SaveResult::Conflict) return Fail(why);
    Log(r == SettingsModel::SaveResult::Nothing ? "already at the defaults" : "settings reset to their defaults");
    return 0;
}

int CmdBackups(const Args& a) {
    fs::path game;
    std::string why;
    if (!ResolveGame(a, &game, &why)) return Fail(why);
    for (const BackupInfo& b : ListBackups(game)) {
        std::printf("%-40s %-20s %-10s %s\n", b.name.c_str(), b.created.c_str(), b.reason.c_str(),
                    b.installed.c_str());
    }
    return 0;
}

int CmdRestore(const Args& a) {
    if (a.positional.empty()) return Fail("which backup? (see: backups)");
    fs::path game;
    std::string why;
    if (!ResolveGame(a, &game, &why)) return Fail(why);
    const InstallResult r = RestoreBackup(game, a.positional[0]);
    return r.ok ? 0 : Fail(r.error);
}

int CmdCheckUpdate(const Args& a, bool install) {
    std::string why;
    auto http = MakeHttp(&why);
    if (!http) Log("note: " + why);
    Updater updater(http.get());
    Payload local;
    const bool have_local = FindBestPayload(&local);
    const UpdateCheck c = updater.Check(a.Has("--force") || install,
                                        have_local ? local.Versions() : std::map<std::string, std::string>());
    std::printf("%s%s\n", c.message.c_str(), c.from_cache ? " (last check)" : "");
    if (c.app_newer) {
        std::printf("  app %s -> %s\n", AppVersion(), c.manifest.manager_version.c_str());
        if (!c.manifest.manager_changelog.empty()) std::printf("%s\n", c.manifest.manager_changelog.c_str());
    }
    for (const ComponentUpdate& u : c.components) {
        std::printf("  %s %s -> %s%s\n", u.remote.name.c_str(), u.local_version.empty() ? "(new)" : u.local_version.c_str(),
                    u.remote.version.c_str(), u.needs_newer_app ? ("  (needs app " + u.remote.min_manager_version + ")").c_str() : "");
        if (!u.remote.changelog.empty()) std::printf("%s\n", u.remote.changelog.c_str());
    }
    if (!install) return c.status == UpdateCheck::Status::Failed ? 1 : 0;
    if (c.status == UpdateCheck::Status::Failed || c.status == UpdateCheck::Status::NotConfigured) return 1;
    const std::vector<ComponentUpdate> todo = c.Installable();
    if (!todo.empty() && !updater.DownloadComponents(todo, &why)) return Fail(why);
    if (c.NeedsNewerApp()) Log("note: some components need a newer app - update the app for them");
    if (c.app_newer) {
        Log("note: the app " + c.manifest.manager_version + " is out; this command updates the components only - "
            "the window updates the app (System > Updates)"
            + (c.manifest.page.empty() ? std::string() : ", or download it from " + c.manifest.page));
    }
    // Installed when the game folder is behind what the app has now (also when an earlier update
    // was only downloaded, the game running then).
    fs::path game;
    if (!ResolveGame(a, &game, &why)) return Fail(why);
    Payload fresh;
    if (!FindBestPayload(&fresh, &why)) return Fail(why);
    const GameStatus s = Inspect(game, &fresh);
    if ((!s.installed && !s.record_present) || s.Outdated(fresh).empty()) return 0;
    if (GameRunning()) return Fail("the components are downloaded; close the game, then run install");
    InstallOptions o;
    o.acknowledge_unverified = a.Has("--yes");
    const InstallResult r = Install(game, fresh, o);
    return r.ok ? 0 : Fail(r.error);
}

int CmdVerifyPayload(const Args& a) {
    Payload payload;
    std::string why;
    if (!ResolvePayload(a, &payload, &why)) return Fail(why);
    if (!payload.VerifyFiles(&why)) return Fail(why);
    for (const Component& c : payload.components) {
        std::printf("%-22s %-10s %-6s %s\n", c.name.c_str(), c.version.c_str(), c.kind.c_str(), U8(c.dir).c_str());
    }
    std::printf("%u component(s) match their manifest.md5: %u file(s) for the game, %u preset(s), %u supported exe(s)\n",
                static_cast<unsigned>(payload.components.size()), static_cast<unsigned>(payload.files.size()),
                static_cast<unsigned>(payload.presets.size()), static_cast<unsigned>(payload.supported.size()));
    return 0;
}

int CmdKeygen(const Args& a) {
    if (a.positional.empty()) return Fail("usage: keygen <dir>");
    const fs::path dir = Path(a.positional[0]);
    if (Exists(dir / "atmt_release.key")) return Fail(U8(dir / "atmt_release.key") + " exists - not overwritten");
    std::string pub, sec, why;
    if (!GenerateKeyPair(&pub, &sec, &why)) return Fail(why);
    if (!WriteFileAtomic(dir / "atmt_release.key", sec, &why) || !WriteFileAtomic(dir / "atmt_release.pub", pub, &why)) {
        return Fail(why);
    }
    std::printf("secret key  %s  (keep it out of the repo and CI logs)\n", U8(dir / "atmt_release.key").c_str());
    std::printf("public key  %s  -> copy it to manager/release_pubkey.txt\n", U8(dir / "atmt_release.pub").c_str());
    return 0;
}

int CmdSign(const Args& a) {
    if (a.positional.size() != 2) return Fail("usage: sign <secret key> <file>");
    std::string key, data, sig, why;
    if (!ReadFile(Path(a.positional[0]), &key)) return Fail("cannot read " + a.positional[0]);
    if (!ReadFile(Path(a.positional[1]), &data)) return Fail("cannot read " + a.positional[1]);
    const std::string comment = a.Opt("--comment", "timestamp:" + std::to_string(UnixNow()) + "\tfile:"
                                                       + U8(Path(a.positional[1]).filename()));
    if (!SignMessage(key, data, comment, &sig, &why)) return Fail(why);
    if (!WriteFileAtomic(Path(a.positional[1] + ".sig"), sig, &why)) return Fail(why);
    std::printf("signed: %s.sig\n", a.positional[1].c_str());
    return 0;
}

int CmdVerifySig(const Args& a) {
    if (a.positional.size() != 2) return Fail("usage: verify-sig <public key> <file>");
    std::string pub, data, sig, why, comment;
    if (!ReadFile(Path(a.positional[0]), &pub)) pub = a.positional[0];   // or the key itself
    if (!ReadFile(Path(a.positional[1]), &data) || !ReadFile(Path(a.positional[1] + ".sig"), &sig)) {
        return Fail("cannot read the file or its .sig");
    }
    PublicKey key;
    if (!ParsePublicKey(pub, &key, &why)) return Fail(why);
    if (!VerifySignature(key, data, sig, &why, &comment)) return Fail(why);
    std::printf("signature OK (trusted comment: %s)\n", comment.c_str());
    return 0;
}

// What the app would read from a release manifest (tools\release.ps1 checks its output with it).
int CmdVerifyManifest(const Args& a) {
    if (a.positional.size() != 2) return Fail("usage: verify-manifest <public key> <manifest.json>");
    std::string pub, text, sig, why;
    if (!ReadFile(Path(a.positional[0]), &pub)) pub = a.positional[0];
    if (!ReadFile(Path(a.positional[1]), &text) || !ReadFile(Path(a.positional[1] + ".sig"), &sig)) {
        return Fail("cannot read the manifest or its .sig");
    }
    Manifest m;
    if (!Updater::VerifyManifest(text, sig, pub, &m, &why)) return Fail(why);
    std::printf("manifest OK, published %s\n", m.published.c_str());
    std::vector<std::string> systems;
    for (const auto& kv : m.manager_downloads) systems.push_back(kv.first);
    std::printf("  %-22s %-10s %s\n", "app", m.manager_version.c_str(), Join(systems, ", ").c_str());
    for (const RemoteComponent& c : m.components) {
        std::printf("  %-22s %-10s %s\n", c.name.c_str(), c.version.c_str(), c.download.url.c_str());
    }
    return 0;
}

}  // namespace

bool IsCliInvocation(const std::vector<std::string>& args) {
    for (const std::string& a : args) {
        if (a == "--cli" || a == "--install" || a == "--uninstall" || a == "--status" || a == "--help" || a == "-h") {
            return true;
        }
    }
    return false;
}

int RunCli(const std::vector<std::string>& argv) {
    const Args a = Parse(argv);
    const std::string& c = a.command;
    if (c.empty() || c == "help" || a.Has("--help")) {
        std::printf(kUsage, AppVersion());
        return c.empty() && !a.Has("--help") ? 1 : 0;
    }
    if (c == "status") return CmdStatus(a);
    if (c == "find") return CmdFind();
    if (c == "install") return CmdInstall(a);
    if (c == "uninstall") return CmdUninstall(a);
    if (c == "enable") return CmdToggle(a, true);
    if (c == "disable") return CmdToggle(a, false);
    if (c == "settings") return CmdSettings(a);
    if (c == "set") return CmdSet(a);
    if (c == "presets") return CmdPresets(a);
    if (c == "preset") return CmdPreset(a);
    if (c == "reset") return CmdReset(a);
    if (c == "backups") return CmdBackups(a);
    if (c == "restore") return CmdRestore(a);
    if (c == "steam") return CmdSteam(a);
    if (c == "senpatcher") return CmdSenPatcher(a);
    if (c == "icon-pack") return CmdIconPack(a);
    if (c == "check-update") return CmdCheckUpdate(a, false);
    if (c == "update") return CmdCheckUpdate(a, true);
    if (c == "verify-payload") return CmdVerifyPayload(a);
    if (c == "keygen") return CmdKeygen(a);
    if (c == "sign") return CmdSign(a);
    if (c == "verify-sig") return CmdVerifySig(a);
    if (c == "verify-manifest") return CmdVerifyManifest(a);
    return Fail("unknown command '" + c + "' (see --help)");
}

}  // namespace atmt
