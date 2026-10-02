// senpatcher.h - SenPatcher's own window, run through the game's Proton (docs/MANAGER.md "SenPatcher").
//
// SenPatcher has no Linux release, and it needs none: SenPatcher.exe (its GUI) runs under Proton
// like the game does, and installs its DINPUT8.dll next to ed8.exe. So on Linux, for the Steam
// version of the game, the manager:
//   1. asks GitHub for SenPatcher's latest release (no version is pinned) and downloads its zip when
//      it is newer than the copy it has - checked against the sha256 GitHub records for the asset -
//      keeping only SenPatcher.exe, its license and readme, and "Trails of Cold Steel/" (the CS1
//      hook dll SenPatcher.exe installs from there) in <DataDir>/senpatcher/<tag>/;
//   2. finds the Proton the game uses - its CompatToolMapping in Steam's config/config.vdf, else
//      Steam's default, else the newest installed - and builds the command line from the tools'
//      toolmanifest.vdf, the way Steam starts a game (inside the Steam Linux Runtime when the Proton
//      asks for it);
//   3. sets Wine's dll override dinput8=native,builtin in the game's prefix (reg.exe through that
//      Proton): without it Wine loads its own dinput8 and SenPatcher never runs;
//   4. points SenPatcher's game folder setting at the game (its gui.ini in the prefix, only when it
//      has none) and runs SenPatcher.exe in the game's prefix, waiting until it is closed.
// The player patches the game in SenPatcher's window as on Windows. Proton's output goes to
// <DataDir>/senpatcher/proton.log.
// On Windows only 1 and 4 apply, for any store's game: SenPatcher.exe runs as it is (its gui.ini is
// %LOCALAPPDATA%\SenPatcherGui\gui.ini) and Windows loads the dll next to ed8.exe by itself.
#pragma once

#include <string>
#include <utility>
#include <vector>

#include "http.h"
#include "json.h"
#include "locator.h"
#include "util.h"

namespace atmt {

constexpr const char* kSenPatcherRepo = "AdmiralCurtiss/SenPatcher";
// The dll SenPatcher installs for CS1, and the override Wine needs to load it.
constexpr const char* kSenPatcherDll = "dinput8";
constexpr const char* kNativeFirst = "native,builtin";

// ---------------------------------------------------------------- the release
struct SenPatcherRelease {
    std::string tag;       // "v1.3.1"
    std::string page;      // the release page
    std::string asset;     // "SenPatcher-v1.3.1.zip"
    std::string url;
    std::string sha256;    // from the asset's "digest"; empty when GitHub has none
    uint64_t size = 0;
};
// GitHub's "latest release" answer (api.github.com/repos/<repo>/releases/latest): its tag and its
// SenPatcher*.zip asset.
bool ParseSenPatcherRelease(const Json& release, SenPatcherRelease* out, std::string* error = nullptr);
bool FetchLatestSenPatcher(Http* http, SenPatcherRelease* out, std::string* error = nullptr);

// A copy of SenPatcher here: <DataDir>/senpatcher/<tag>/ with SenPatcher.exe.
struct SenPatcherCopy {
    std::string tag;
    fs::path dir;
    fs::path exe;
    bool ok() const { return !tag.empty(); }
};
fs::path SenPatcherRoot();
// The newest complete copy (one whose atmt_senpatcher.json was written last), or none.
SenPatcherCopy LocalSenPatcher();
// A downloaded release zip: its size and sha256 (when the release has them), then the files
// SenPatcher needs for CS1 into SenPatcherRoot()/<tag>/; older copies are removed.
bool InstallSenPatcherZip(const SenPatcherRelease& release, const std::string& zip, SenPatcherCopy* out,
                          std::string* error = nullptr);
// The latest release when it is newer than the copy here (downloaded and installed), else the copy
// here; the copy here, with a note, when GitHub cannot be asked.
bool EnsureSenPatcher(Http* http, SenPatcherCopy* out, std::string* note, std::string* error,
                      const ProgressFn& progress = nullptr);

// ---------------------------------------------------------------- Proton
struct ProtonTool {
    std::string name;      // the compat tool name Steam's settings use: "proton_9", "GE-Proton9-20"
    std::string display;   // "Proton 9.0 (Beta)"
    fs::path dir;          // the folder with `proton` and toolmanifest.vdf
};
// "Proton 9.0 (Beta)" -> "proton_9", "Proton 6.3" -> "proton_63", "Proton - Experimental" ->
// "proton_experimental", "Proton Hotfix" -> "proton_hotfix"; "" for anything else.
std::string OfficialProtonName(const std::string& folder_name);
// Valve's Protons in the libraries' steamapps/common, and the custom ones in the roots'
// compatibilitytools.d (and /usr/share/steam/compatibilitytools.d).
std::vector<ProtonTool> FindProtonTools(const std::vector<fs::path>& roots, const std::vector<fs::path>& libraries);
// The compat tool Steam is set to use for `appid` (<root>/config/config.vdf CompatToolMapping), ""
// when none; appid "0" is Steam's default for every other game.
std::string CompatToolSetting(const fs::path& steam_root, const std::string& appid);

struct ProtonLaunch {
    ProtonTool tool;
    std::string why;               // how the tool was chosen, for the log
    fs::path steam_root;
    fs::path library;
    fs::path game_dir;
    fs::path compat_data;          // <library>/steamapps/compatdata/538680
    fs::path prefix;               // compat_data / pfx
    std::vector<std::string> command;   // what the program to run is appended to
    std::vector<fs::path> tool_dirs;    // Proton and the runtime it runs in
};
// How to run a program in `game`'s Proton prefix. `game` must be a Steam install.
bool PlanProtonLaunch(const GameInstall& game, const std::vector<fs::path>& roots, const std::vector<fs::path>& libraries,
                      ProtonLaunch* out, std::string* error = nullptr);
// The environment Steam gives Proton for the game (STEAM_COMPAT_*); `mounts` are more folders the
// runtime's container must see.
std::vector<std::pair<std::string, std::string>> ProtonEnv(const ProtonLaunch& launch, const std::vector<fs::path>& mounts = {});
// Runs `args` (args[0] a Windows program: a Linux path or a C:\ one) in the game's prefix and waits.
bool RunInProton(const ProtonLaunch& launch, const std::vector<std::string>& args, const fs::path& cwd,
                 const std::vector<fs::path>& mounts, int* exit_code, std::string* error = nullptr);

// ---------------------------------------------------------------- the dll override
// The value of a dll's override in a prefix's user.reg ([Software\\Wine\\DllOverrides]), "" when none.
std::string PrefixDllOverride(const fs::path& prefix, const std::string& dll);
// The game's Steam launch options (userdata/<account>/config/localconfig.vdf), every account's.
std::vector<std::string> SteamLaunchOptions(const std::vector<fs::path>& roots, const std::string& appid);
// Does an override value load the dll from the game folder first ("native,builtin", "n,b", "n")?
bool OverrideLoadsNative(const std::string& value);
// Does a launch option set it (WINEDLLOVERRIDES=...dinput8=n,b...)?
bool LaunchOptionLoadsNative(const std::string& options, const std::string& dll);
// Sets dinput8=native,builtin in the game's prefix (reg.exe through its Proton; creates the prefix
// when the game never ran).
bool SetSenPatcherOverride(const ProtonLaunch& launch, std::string* error = nullptr);

// ---------------------------------------------------------------- status and the run
struct SenPatcherStatus {
    bool dll = false;               // SenPatcher's DINPUT8.dll is next to ed8.exe
    bool can_run = false;           // Windows, or Linux and the Steam version: "Run SenPatcher" is offered
    std::string cannot_run;         // why not, when on Linux
    bool needs_override = false;    // Linux: Wine must be told to load the dll
    bool override_set = false;      // in the prefix, or by a launch option
    std::string override_where;     // "prefix" / "launch options"
    std::string local_tag;          // the copy here
    std::string Text() const;       // the Status row
};
SenPatcherStatus GetSenPatcherStatus(const GameInstall& game);

struct SenPatcherRun {
    bool ok = false;
    std::string error;
    std::vector<std::string> notes;
    std::string tag;                // the SenPatcher that ran
    bool installed = false;         // its DINPUT8.dll is next to ed8.exe afterwards
};
// The whole flow above, for the game in `game_dir` (the game not running; on Linux the Steam version).
// `override_only` (Linux): set the dll override and stop (no download, no window).
SenPatcherRun RunSenPatcher(const fs::path& game_dir, Http* http, bool override_only = false,
                            const ProgressFn& progress = nullptr);

}  // namespace atmt
