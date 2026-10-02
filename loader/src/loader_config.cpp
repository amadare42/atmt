// loader_config.cpp - reads atmt_loader.ini (see loader_config.h).
#include "loader_config.h"

#include "ini.h"

#include <cstdio>

namespace atmt_loader {

namespace {

const char kDefaultIni[] =
    "; atmt_loader - settings for the loader (the mods have their own ini files)\n"
    "; Everything here is optional; delete the file to get these values back.\n"
    "\n"
    "[Loader]\n"
    "; master switch for the whole loader\n"
    "Enabled=true\n"
    "\n"
    "; where the mod dlls live. A relative path is resolved against the loader dll.\n"
    "ModDir=atmt_mods\n"
    "\n"
    "; load exactly these files (comma separated, resolved inside ModDir).\n"
    "; empty = load every *.dll in ModDir, in alphabetical order.\n"
    "Mods=\n"
    "\n"
    "; wait this long before loading mods (milliseconds; 0 = immediately)\n"
    "LoadDelayMs=0\n"
    "\n"
    "; atmt_loader.log next to the game (each start begins it fresh): errors = only what failed (the\n"
    "; file is only created then), all = every line (for debugging), off = no file at all. The mods'\n"
    "; own lines share this file and follow the same setting.\n"
    "LogLevel=errors\n";

std::wstring Join(const std::wstring& dir, const wchar_t* leaf) {
    if (dir.empty()) return leaf;
    std::wstring out = dir;
    if (out.back() != L'\\' && out.back() != L'/') out += L'\\';
    out += leaf;
    return out;
}

// Splits "a.dll, b.dll" / "a, b" into names, trimming spaces.
std::vector<std::wstring> SplitList(const char* value) {
    std::vector<std::wstring> out;
    if (value == nullptr) return out;
    std::wstring current;
    for (const char* p = value; *p != '\0'; ++p) {
        if (*p == ',' || *p == ';') {
            if (!current.empty()) out.push_back(current);
            current.clear();
            continue;
        }
        if (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') continue;
        current.push_back(static_cast<wchar_t>(static_cast<unsigned char>(*p)));
    }
    if (!current.empty()) out.push_back(current);
    return out;
}

}  // namespace

LoaderConfig LoadLoaderConfig(const std::wstring& self_dir) {
    LoaderConfig config;
    std::wstring path = Join(self_dir, L"atmt_loader.ini");
    if (GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES) {
        atmt_ini::WriteDefaultIfMissing(path.c_str(), kDefaultIni);
    }

    char value[512];
    config.enabled = atmt_ini::ReadBool(path.c_str(), "Loader", "Enabled", config.enabled);
    config.load_delay_ms = atmt_ini::ReadInt(path.c_str(), "Loader", "LoadDelayMs", 0);
    if (config.load_delay_ms < 0) config.load_delay_ms = 0;
    if (atmt_ini::Read(path.c_str(), "Loader", "LogLevel", value, sizeof(value), "errors")) {
        if (_stricmp(value, "all") == 0) config.log_level = LogLevel::All;
        if (_stricmp(value, "off") == 0) config.log_level = LogLevel::Off;
    }

    if (atmt_ini::Read(path.c_str(), "Loader", "ModDir", value, sizeof(value), "atmt_mods")) {
        // a relative ModDir is relative to the loader dll, which is where a user
        // would expect it (the loader may live outside the game folder)
        const std::wstring dir(value, value + strlen(value));
        if (dir.find(L':') == std::wstring::npos && dir[0] != L'\\') {
            config.mod_dir = Join(self_dir, dir.c_str());
        } else {
            config.mod_dir = dir;
        }
    } else {
        config.mod_dir = Join(self_dir, L"atmt_mods");
    }

    if (atmt_ini::Read(path.c_str(), "Loader", "Mods", value, sizeof(value), "")) {
        config.mods = SplitList(value);
    }
    return config;
}

}  // namespace atmt_loader
