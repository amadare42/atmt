// install.h - install_core: puts a payload into the game folder, and takes it out again.
//
// What it does (docs/MANAGER.md):
//
//   * back up everything mod-related into atmt_backup_<tag>/ first (backup.json in there says
//     which items existed, so a restore can also remove what was not there);
//   * keep the game's own GFSDK_SSAO_D3D11.win32.dll as .orig - our loader is recognised by its
//     marker and never renamed to .orig;
//   * install the loader and atmt_mods/*.dll (disabled mods into atmt_mods/disabled, which the
//     loader skips), keep existing inis (templates only fill in missing files; missing keys are
//     added with their help comments), reset ModDir, remove the leftovers of earlier deployments;
//   * verify every written file against manifest.md5 and roll back from the backup on a mismatch
//     or on any failure on the way;
//   * record what was done in atmt_install.json;
//   * keep only the newest kKeepBackups backups once it has succeeded;
//   * where SenPatcher is set up, (re)build the icon pack (icon_pack.h) from the textures in use.
//
// Hard stops (they would corrupt, not merely misbehave): no ed8.exe; nothing for the loader to
// forward to (our loader in place and no .orig, or neither file). An ed8.exe whose md5 is not in
// supported_exe.txt only needs an acknowledgement (options.acknowledge_unverified), stored per exe
// hash in atmt_install.json.
#pragma once

#include <functional>
#include <map>
#include <string>
#include <vector>

#include "json.h"
#include "payload.h"
#include "util.h"

namespace atmt {

enum class ExeVerdict { Missing, Supported, Unverified };

struct ModState {
    std::string name;
    bool enabled = false;    // atmt_mods/<name>.dll
    bool disabled = false;   // atmt_mods/disabled/<name>.dll
    bool in_payload = false;
};

struct GameStatus {
    fs::path dir;
    bool dir_exists = false;
    ExeVerdict verdict = ExeVerdict::Missing;
    std::string exe_md5;
    bool proxy_present = false;
    bool proxy_is_ours = false;
    bool orig_present = false;
    bool installed = false;              // our loader is in place
    bool record_present = false;         // atmt_install.json
    // component -> version, from atmt_install.json (empty: installed by a developer script, or not)
    std::map<std::string, std::string> installed_versions;
    bool unverified_acknowledged = false;
    // SenPatcher v1.x: its DINPUT8.dll is next to the game. It patches ed8.exe in memory only, so
    // the exe on disk (and its md5) stays the vanilla one, and the mods check the code they touch.
    bool senpatcher = false;
    // A SenPatcher older than v1.0 patched ed8.exe (and data files) on disk and left its backups
    // (ed8.exe.senpatcher.bkp, senpatcher_bkp/, senpatcher_rerun_revert_data.bin): the exe is then
    // not the supported build until it is restored (SenPatcherGui, or Steam's file check).
    bool old_senpatcher = false;
    // SenPatcher's mods/ loader is set up: its DINPUT8.dll is here, or there is a mods/order.txt
    // (SenPatcher creates mods/ but not order.txt, which is optional for it).
    bool senpatcher_mods = false;
    IconPackStatus icon_pack;            // only with a payload that has an icon_pack.json
    bool game_running = false;
    std::vector<ModState> mods;          // installed (enabled or parked) and payload mods
    std::vector<std::string> problems;   // what a Repair would fix (compared with the payload)
    std::vector<std::string> hard_stops; // why Install cannot run at all
    Json record;

    enum class Action { None, Install, Update, Repair, Reinstall };
    Action SuggestedAction(const Payload* payload) const;
    // The payload's components newer than the installed ones (or new): "Title 1.0.0 -> 1.1.0".
    std::vector<std::string> Outdated(const Payload& payload) const;
};

GameStatus Inspect(const fs::path& game_dir, const Payload* payload);

// Shown with an unverified exe when GameStatus::old_senpatcher is set.
inline constexpr const char* kOldSenPatcherHint =
    "a SenPatcher older than v1.0 patched ed8.exe on disk: restore the game with SenPatcherGui (or "
    "Steam's \"Verify integrity of game files\") and use SenPatcher 1.x, which leaves ed8.exe untouched";
const char* ActionName(GameStatus::Action a);

struct InstallOptions {
    std::string tag;                     // atmt_backup_<tag>; default: a time stamp
    bool explicit_mods = false;          // `enable` is the complete set of mods to enable
    std::vector<std::string> enable;
    bool acknowledge_unverified = false; // "Install anyway" / --yes
    // The icon pack, where SenPatcher is set up. Refresh: rebuild one that is already there (it
    // follows the textures in use) but make none - only small screens need it, so a new one comes
    // from a preset's "icon_pack" (Install everything), --icon-pack or Status > Build icon pack.
    enum class IconPack { Refresh, Build, Skip };
    IconPack icon_pack = IconPack::Refresh;
    // Test hook: called after the files are written and before they are verified (a test damages
    // one to prove the rollback).
    std::function<void()> before_verify;
};

struct InstallResult {
    bool ok = false;
    bool needs_ack = false;      // the exe is unverified and was not acknowledged
    bool rolled_back = false;
    bool game_running = false;   // the new files take effect on the next start
    std::string error;
    fs::path backup_dir;
    std::vector<std::string> written;
    std::vector<std::string> notes;
};

InstallResult Install(const fs::path& game_dir, const Payload& payload, const InstallOptions& options);

// "Install everything" - the one-button setup: every mod the payload has switched on (except the
// ones a preset recommended for this platform switches off with its "mods"), then the settings of
// those presets ("recommended_on" in presets/*.json) applied; one with "icon_pack": true builds the
// icon pack (unless the options Skip it). With the game running the settings
// are queued for its exit. The applied presets are named in the result's notes.
InstallResult InstallEverything(const fs::path& game_dir, const Payload& payload, const InstallOptions& options);
struct UninstallOptions {
    std::string tag;      // atmt_backup_<tag>; default uninstall_<time>
    bool purge = false;   // everything of the toolkit's (atmt_* in the game folder: logs, output files,
                          // backups, caches) as well, and no backup is made
};

// Puts the game's own library back and removes the loader, the mods and their settings (all of it
// backed up first into atmt_backup_<tag>/). Logs and earlier backups stay - unless options.purge,
// which leaves the game folder as Steam installed it.
InstallResult Uninstall(const fs::path& game_dir, const UninstallOptions& options = UninstallOptions());

// Moves a mod between atmt_mods/ and atmt_mods/disabled/.
bool SetModEnabled(const fs::path& game_dir, const std::string& mod, bool enabled, std::string* error = nullptr);

// A preset's mod switches ("mods" in presets/*.json) that differ from the game folder now: (mod,
// wanted state). A mod that is not installed is left out.
std::vector<std::pair<std::string, bool>> PresetModChanges(const fs::path& game_dir, const Preset& preset);
// A preset with "icon_pack": true (a small screen) builds the icon pack when it is applied, where
// it can be built and is not there or out of date (one the player removed stays removed).
bool PresetBuildsIconPack(const Preset& preset, const IconPackStatus& status);
// Switches them (SetModEnabled); the switched mods' names go into `changed`.
bool ApplyPresetMods(const fs::path& game_dir, const Preset& preset, std::vector<std::string>* changed = nullptr,
                     std::string* error = nullptr);

struct BackupInfo {
    std::string name;   // folder name, atmt_backup_<tag>
    fs::path dir;
    std::string created;          // from backup.json (empty for a backup made by deck_install.sh)
    int64_t created_ms = 0;       // the same to the millisecond (0: an older backup.json)
    std::string reason;           // "install", "uninstall", "restore", ...
    std::string installed;        // what was installed when it was taken ("loader 1.0.0, 6 mods")
    std::vector<std::string> items;
};
std::vector<BackupInfo> ListBackups(const fs::path& game_dir);   // newest first
// How many backups an install, uninstall or restore leaves (the newest; the one it just took among them).
constexpr size_t kKeepBackups = 2;
// Removes every backup but the newest `keep`.
void PruneBackups(const fs::path& game_dir, size_t keep = kKeepBackups);
// Brings the managed files back to the state of a backup (the current state is backed up first).
InstallResult RestoreBackup(const fs::path& game_dir, const std::string& backup_name);

// The loader carries its own ini name as a string; a GFSDK dll that has it is ours.
bool IsOurLoader(const fs::path& dll);

// GameStatus::senpatcher_mods for a folder.
bool SenPatcherSetUp(const fs::path& game_dir);
// SenPatcher's own DINPUT8.dll is next to ed8.exe (GameStatus::senpatcher for a folder).
bool SenPatcherDllPresent(const fs::path& game_dir);

Json ReadInstallRecord(const fs::path& game_dir);
bool WriteInstallRecord(const fs::path& game_dir, const Json& record);
// The player's choice about the icon pack, kept in atmt_install.json ("icon_pack_removed"): removing
// it keeps every later install from building it again; building it by hand takes it back.
bool IconPackRemovedByPlayer(const fs::path& game_dir);
bool SetIconPackRemovedByPlayer(const fs::path& game_dir, bool removed);
// RemoveIconPack and remembering it; BuildIconPack (InstallIconPack) after forgetting it.
bool RemoveIconPackByPlayer(const fs::path& game_dir, std::string* error = nullptr);
IconPackResult BuildIconPackByPlayer(const fs::path& game_dir, const IconPackConfig& config);

constexpr const char* kInstallRecord = "atmt_install.json";

}  // namespace atmt
