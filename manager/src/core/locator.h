// locator.h - finding the game and its Wine/Proton prefix (docs/MANAGER.md).
//
// Every store's install is the same game folder (ed8.exe next to GFSDK_SSAO_D3D11.win32.dll), so an
// install is recognised by ed8.exe in it, wherever a store or launcher says it put the game:
//   Steam            libraryfolders.vdf + appmanifest_538680.acf (Windows, Linux, Flatpak, SD cards)
//   GOG / Galaxy     HKLM\SOFTWARE\WOW6432Node\GOG.com\Games\<id> "path" (product 2029703882)
//   Heroic           <config>/heroic/gog_store/installed.json, sideload_apps/library.json,
//                    the prefix from GamesConfig/<app>.json "winePrefix"
//   Lutris           its games/*.yml ("exe:" / "prefix:")
//   Wine prefixes    ~/.wine, Bottles' bottles, ~/Games/* (Lutris's default): drive_c's usual
//                    install folders
//   default folders  C:\GOG Games\*, GOG Galaxy\Games\* (Windows)
// A folder picked by hand always works; it only needs ed8.exe.
#pragma once

#include <string>
#include <utility>
#include <vector>

#include "util.h"

namespace atmt {

// Trails of Cold Steel's Steam app id (checked against appmanifest_538680.acf of real installs).
constexpr const char* kSteamAppId = "538680";
// Its GOG product id (gogdb.org/product/2029703882) - only a hint: any GOG game folder with ed8.exe counts.
constexpr const char* kGogProductId = "2029703882";
constexpr const char* kGameInstallDir = "Trails of Cold Steel";
constexpr const char* kGameExe = "ed8.exe";

// Valve's KeyValues text format (libraryfolders.vdf, appmanifest_*.acf).
struct Vdf {
    std::string value;                                  // a leaf
    std::vector<std::pair<std::string, Vdf>> children;  // a block
    bool is_block = false;

    const Vdf* Find(const std::string& key) const;     // case-insensitive, first match
    std::string Str(const std::string& key) const;     // a leaf child's value, or ""
};
bool ParseVdf(const std::string& text, Vdf* out);
// Text KeyValues as Steam writes them (tabs, quoted keys and values); `root` is the top level.
std::string WriteVdf(const Vdf& root);

struct GameInstall {
    fs::path dir;         // the folder with ed8.exe
    std::string source;   // "Steam", "GOG", "Heroic (GOG)", "Heroic",
                          // "Lutris", "Bottles", "Wine", or "" (a folder picked by hand)
    fs::path library;     // the Steam library it is in (empty elsewhere)
    fs::path prefix;      // the Wine/Proton prefix, when one is known: Steam's
                          // <library>/steamapps/compatdata/538680/pfx (may not exist yet), a
                          // launcher's, or the one the folder is inside of (Linux)
    bool prefix_exists = false;
    bool IsSteam() const { return source == "Steam"; }
};

// Every Steam root to look in on this machine (existing ones only).
std::vector<fs::path> SteamRoots();
// Every library listed by those roots' libraryfolders.vdf, plus the roots, plus SD cards.
std::vector<fs::path> SteamLibraries(const std::vector<fs::path>& roots);
// The installs found in `libraries`: an appmanifest for the app id whose folder has ed8.exe, or
// the default folder name with ed8.exe.
std::vector<GameInstall> FindGames(const std::vector<fs::path>& libraries);

// The other stores and launchers; each takes the folder it reads, so it can be tested on a sandbox.
std::vector<GameInstall> FindHeroicGames(const fs::path& heroic_config_dir);
std::vector<GameInstall> FindLutrisGames(const fs::path& games_yml_dir);
std::vector<GameInstall> FindGamesInPrefix(const fs::path& prefix, const std::string& source);
// `parent` itself or one of its sub-folders with ed8.exe (a store's default install folder).
std::vector<GameInstall> FindGamesUnder(const fs::path& parent, const std::string& source);
// Every non-Steam place this machine has (registry, launchers' files, prefixes, default folders).
std::vector<GameInstall> FindOtherGames();

// Everything: Steam first, then the rest; one entry per folder.
std::vector<GameInstall> FindGames();
// Appends `more` to `list`, leaving out folders already in it.
void MergeGames(std::vector<GameInstall>& list, const std::vector<GameInstall>& more);

// A folder the user picked: the install it describes (library/prefix worked out when the folder is
// inside a library's steamapps/common, or inside a Wine prefix's drive_c). An entry of `known`
// (FindGames()) for the same folder is returned as it is.
GameInstall DescribeGameDir(const fs::path& dir, const std::vector<GameInstall>& known = {});
bool HasGameExe(const fs::path& dir);
// What a typed or picked path means: quotes and spaces trimmed, ed8.exe itself -> its folder, a
// folder whose only game sub-folder has ed8.exe -> that sub-folder. Empty when there is no ed8.exe.
fs::path ResolveGameDir(const std::string& typed);
// "C:\Games\x" inside `prefix` -> <prefix>/drive_c/Games/x (Linux; Windows paths stay as they are).
fs::path WinePath(const fs::path& prefix, const std::string& windows_path);

}  // namespace atmt
