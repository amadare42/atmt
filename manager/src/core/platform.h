// platform.h - what differs between Windows and Linux (the Steam Deck): processes, the app's own
// file, where the manager keeps its state.
#pragma once

#include <string>
#include <utility>
#include <vector>

#include "util.h"

namespace atmt {

// Is a process with this executable name running (case-insensitive; on Linux its comm or the file
// name of its argv[0], which for a Proton game is a Windows path)?
bool ProcessRunning(const std::string& name);
// Is ed8.exe running? On Linux that is the Proton process, whose command line names the exe.
bool GameRunning();
// Is a wineserver running (section 9: the prefix registry must not be edited then)?
bool WineserverRunning();

// The running executable. With an AppImage that is the AppImage file ($APPIMAGE), not the
// mounted binary inside it: that file is what an app update replaces.
fs::path SelfExe();
// The folder of the actual binary (inside the AppImage mount, if any): where the bundled payload is.
fs::path SelfDir();
bool RunningFromAppImage();

// Per-user state: update settings, downloaded payloads. %LOCALAPPDATA%\atmt_manager on Windows,
// $XDG_DATA_HOME/atmt_manager (~/.local/share/atmt_manager) on Linux.
fs::path DataDir();
fs::path HomeDir();
std::string GetEnv(const char* name);

// "SteamPath" from HKCU\Software\Valve\Steam; empty elsewhere.
std::string SteamPathFromRegistry();
// GOG's (and GOG Galaxy's) installed games: (product id, "path") of every
// HKLM\SOFTWARE\WOW6432Node\GOG.com\Games\<id>; empty elsewhere.
std::vector<std::pair<std::string, std::string>> GogGamesFromRegistry();

// The system's folder picker (Windows: the shell's dialog; Linux: zenity or kdialog, when there).
// False when cancelled or when there is no picker; `start` is where it opens (may be empty).
bool PickFolder(const std::string& title, const std::string& start, std::string* out);

bool OpenUrl(const std::string& url);
// Starts `exe` with `args` detached (the "Restart now" after an app update).
bool Launch(const fs::path& exe, const std::vector<std::string>& args);
// Runs `argv` (argv[0] a full path) in `cwd` with this process's environment plus `env` (an empty
// value removes the variable), its output appended to `log` (none when empty), and waits for it to
// exit. False when it could not be started; `exit_code` is its exit status (-1: killed by a signal).
// On Linux Windows programs run through Proton this way; on Windows SenPatcher.exe itself.
bool RunAndWait(const std::vector<std::string>& argv, const std::vector<std::pair<std::string, std::string>>& env,
                const fs::path& cwd, const fs::path& log, int* exit_code, std::string* error = nullptr);

std::string PlatformName();   // "windows" / "linux"
bool IsSteamGameMode();       // SteamOS Game Mode (gamescope), for the GUI's layout choices

}  // namespace atmt
