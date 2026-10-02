// loader_config.h - settings for the loader itself (atmt_loader.ini).
//
// Deliberately separate from any mod's settings: the loader only needs to know
// whether it is enabled, where the mods live, which ones to load, and whether to
// wait before loading (some games are happier if mods attach after startup).
#pragma once

#include <windows.h>

#include <string>
#include <vector>

namespace atmt_loader {

// LogLevel in atmt_loader.ini: off = no file at all; errors (the default) = only failures, and the
// file is only created when there is one; all = every line, as a developer wants it.
enum class LogLevel { Off, Errors, All };

struct LoaderConfig {
    bool enabled = true;
    std::wstring mod_dir;                  // absolute; defaults to <loader>\atmt_mods
    std::vector<std::wstring> mods;        // empty = every *.dll in mod_dir
    int load_delay_ms = 0;                 // wait before touching the game
    LogLevel log_level = LogLevel::Errors; // what goes into atmt_loader.log
};

// Reads <self_dir>\atmt_loader.ini, falling back to <game_dir>\atmt_loader.ini, and
// writes the default file if neither exists.
LoaderConfig LoadLoaderConfig(const std::wstring& self_dir);

}  // namespace atmt_loader
