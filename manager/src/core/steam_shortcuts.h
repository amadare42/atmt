// steam_shortcuts.h - ATMT Manager as a non-Steam game in the Steam library, and out of it again.
//
// Steam keeps non-Steam games in <steam>/userdata/<account>/config/shortcuts.vdf (binary KeyValues,
// one file per Steam account on the machine). Steam reads it when it starts and writes it back when
// it exits, so the file is only changed while Steam is not running: the caller closes Steam first
// (CloseSteam) and starts it again afterwards (StartSteam). Every other entry is kept byte for byte
// in meaning (all value types are read and written back); the file is backed up once as
// shortcuts.vdf.atmt_bak before the manager first changes it.
//
// The entry: AppName "ATMT Manager", Exe = this app (the AppImage on Linux), its folder as StartDir,
// the app's icon. Ours is recognised by its AppName.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "util.h"

namespace atmt {

constexpr const char* kShortcutName = "ATMT Manager";

// Binary KeyValues (shortcuts.vdf, appinfo-style files).
struct BinVdf {
    enum Type : uint8_t { Map = 0, String = 1, Int32 = 2, Float32 = 3, Ptr = 4, WString = 5, Color = 6, UInt64 = 7 };
    uint8_t type = Map;
    std::string name;
    std::string str;          // String (and WString, kept as raw bytes)
    uint32_t u32 = 0;         // Int32, Float32 (bits), Ptr, Color
    uint64_t u64 = 0;         // UInt64
    std::vector<BinVdf> children;

    BinVdf* Find(const std::string& key);   // case-insensitive
    const BinVdf* Find(const std::string& key) const;
};
bool ParseBinVdf(const std::string& data, BinVdf* root, std::string* error = nullptr);   // root = a Map of the top level
std::string WriteBinVdf(const BinVdf& root);

struct SteamAccount {
    fs::path config_dir;      // <steam>/userdata/<id>/config
    std::string id;
    bool has_shortcut = false;
};

// Every Steam account on this machine that has a config folder.
std::vector<SteamAccount> SteamAccounts();
bool InSteamLibrary();   // in at least one account

bool SteamRunning();
// Asks Steam to exit and waits for it (up to `timeout_ms`). Not in Game Mode: there Steam is the session.
bool CloseSteam(int timeout_ms, std::string* error = nullptr);
bool StartSteam();

// Adds (or refreshes) / removes the entry in every account. Steam must not be running.
// `exe` is what Steam starts; `icon` an image file for the library (may be empty).
bool AddToSteam(const fs::path& exe, const fs::path& icon, int* accounts, std::string* error = nullptr);
bool RemoveFromSteam(int* accounts, std::string* error = nullptr);

// One shortcuts.vdf: our entry added (an old one replaced) / removed (`removed` says whether there
// was one). The rest of the file is kept. What AddToSteam/RemoveFromSteam do per account.
bool AddShortcut(const fs::path& vdf, const fs::path& exe, const fs::path& icon, std::string* error = nullptr);
bool RemoveShortcut(const fs::path& vdf, bool* removed, std::string* error = nullptr);
bool HasShortcut(const fs::path& vdf);

// The Steam Deck controller layout for our entry: Steam keeps it per account in
// <steam>/steamapps/common/Steam Controller Configs/<account>/config/configset_controller_neptune.vdf,
// keyed by the shortcut's name in lower case. Added: "Gamepad with Mouse Trackpad" (the right
// trackpad is a pointer, its press is R3 - the app clicks with it) unless the player chose one;
// removed: only that one of ours. Steam must not be running. What AddToSteam/RemoveFromSteam do on Linux.
constexpr const char* kDeckControllerTemplate = "controller_neptune_gamepad+mouse.vdf";
fs::path DeckControllerConfig(const SteamAccount& account);
bool SetDeckControllerLayout(const fs::path& configset, std::string* error = nullptr);
bool ClearDeckControllerLayout(const fs::path& configset, std::string* error = nullptr);

// The app's icon as a file Steam can show (<DataDir>/atmt-manager.png, written from the copy
// compiled into the app); empty when it cannot be written.
fs::path WriteAppIcon();

// The shortcut's app id, as Steam computes it for a non-Steam game (for its artwork file names).
uint32_t ShortcutAppId(const std::string& exe_quoted, const std::string& name);

}  // namespace atmt
