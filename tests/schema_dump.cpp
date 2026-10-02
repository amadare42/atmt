// schema_dump.cpp - writes settings_schema.json: every mod's settings table, as data.
//
//   atmt_schema_dump <out.json> <mod.dll> [<mod.dll> ...]
//
// The manager (manager/, docs/MANAGER.md) renders its settings forms from this
// file, so adding a setting to a mod makes it appear there with no UI work. The tables only exist
// inside the mods, so they are read the one way they are ever handed out: each mod dll is loaded
// and started with a stand-in AtmtModApi, and whatever it passes to settings_register is written
// down. The values in its storage at that moment are the mod's own defaults (no ini is read here).
//
// A mod is written for the game, not for this harness: after registering it goes on to check and
// hook fixed addresses of ed8.exe that do not exist here. So every mod runs in a child process of
// its own (this exe with --one), which writes its table and exits from inside settings_register -
// nothing after the registration ever runs. A mod that registers from a thread of its own (the
// dialog logger does, after its hooks) gets a few seconds; one that crashes before registering, or
// never registers, simply has no table, which is reported but is not an error.
//
// Every mod also says what it does (its AtmtModDescription export, read before it is started, so a
// mod without settings has one too): those go into "mods", one entry per dll, which is where the
// manager's Mods screen takes its descriptions from.
//
// The loader's own atmt_loader.ini is not a mod's table; it is described here, by hand, from
// loader/src/loader_config.cpp (the keys the loader reads).
#include <windows.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "atmt_input.h"
#include "mod_api.h"

namespace {

std::string g_out_path;   // --one: where the child writes its group
std::string g_mod_name;   // --one: the dll's stem, which is what the loader calls the mod
LONG g_written = 0;

std::string Escape(const char* text) {
    std::string out = "\"";
    if (text != nullptr) {
        for (const unsigned char* p = reinterpret_cast<const unsigned char*>(text); *p != 0; ++p) {
            switch (*p) {
                case '"': out += "\\\""; break;
                case '\\': out += "\\\\"; break;
                case '\n': out += "\\n"; break;
                case '\r': out += "\\r"; break;
                case '\t': out += "\\t"; break;
                default:
                    if (*p < 0x20) {
                        char buf[8];
                        _snprintf(buf, sizeof(buf), "\\u%04x", *p);
                        out += buf;
                    } else {
                        out += static_cast<char>(*p);
                    }
            }
        }
    }
    return out + "\"";
}

const char* TypeName(uint32_t type) {
    switch (type) {
        case ATMT_SETTING_BOOL: return "bool";
        case ATMT_SETTING_INT: return "int";
        case ATMT_SETTING_FLOAT: return "float";
        case ATMT_SETTING_ENUM: return "enum";
        case ATMT_SETTING_STRING: return "string";
        case ATMT_SETTING_KEY: return "key";
        case ATMT_SETTING_PAD_BUTTON: return "pad_button";
        case ATMT_SETTING_ACTION: return "action";
        case ATMT_SETTING_LABEL: return "label";
        default: return "unknown";
    }
}

std::string Number(float v) {
    char buf[64];
    for (int digits = 6; digits <= 9; ++digits) {   // the registry's rule: shortest that reads back
        _snprintf(buf, sizeof(buf), "%.*g", digits, v);
        if (static_cast<float>(std::strtod(buf, nullptr)) == v) break;
    }
    return buf;
}

// The default as the ini spells it (loader_settings.cpp, Format).
std::string DefaultOf(const AtmtSetting& s) {
    if (s.value == nullptr) return std::string();
    switch (s.type) {
        case ATMT_SETTING_BOOL: return *static_cast<const int32_t*>(s.value) != 0 ? "true" : "false";
        case ATMT_SETTING_INT: return std::to_string(*static_cast<const int32_t*>(s.value));
        case ATMT_SETTING_FLOAT: return Number(*static_cast<const float*>(s.value));
        case ATMT_SETTING_ENUM: {
            const int32_t i = *static_cast<const int32_t*>(s.value);
            if (s.choices == nullptr || s.choice_count == 0) return std::string();
            return (i >= 0 && static_cast<uint32_t>(i) < s.choice_count) ? s.choices[i] : s.choices[0];
        }
        case ATMT_SETTING_STRING:
        case ATMT_SETTING_KEY:
        case ATMT_SETTING_PAD_BUTTON: {
            const char* text = static_cast<const char*>(s.value);
            return std::string(text, strnlen(text, s.capacity));
        }
        default: return std::string();
    }
}

std::string SettingJson(const AtmtSetting& s) {
    std::string j = "{\"type\": \"" + std::string(TypeName(s.type)) + "\"";
    std::string flags;
    if (s.flags & ATMT_SETTING_LIVE) flags += "\"live\", ";
    if (s.flags & ATMT_SETTING_RESTART) flags += "\"restart\", ";
    if (s.flags & ATMT_SETTING_ADVANCED) flags += "\"advanced\", ";
    if (s.flags & ATMT_SETTING_READONLY) flags += "\"readonly\", ";
    if (!flags.empty()) flags.resize(flags.size() - 2);
    j += ", \"flags\": [" + flags + "]";
    const bool stored = s.type <= ATMT_SETTING_PAD_BUTTON;
    if (stored) {
        j += ", \"section\": " + Escape(s.section != nullptr ? s.section : "General");
        j += ", \"key\": " + Escape(s.key);
    }
    if (s.label != nullptr) j += ", \"label\": " + Escape(s.label);
    if (s.help != nullptr) j += ", \"help\": " + Escape(s.help);
    if (stored) j += ", \"default\": " + Escape(DefaultOf(s).c_str());
    if ((s.type == ATMT_SETTING_INT || s.type == ATMT_SETTING_FLOAT) && s.min < s.max) {
        j += ", \"min\": " + Number(s.min) + ", \"max\": " + Number(s.max);
    }
    if ((s.type == ATMT_SETTING_INT || s.type == ATMT_SETTING_FLOAT) && s.step > 0) {
        j += ", \"step\": " + Number(s.step);
    }
    if (s.type == ATMT_SETTING_ENUM && s.choices != nullptr) {
        j += ", \"choices\": [";
        for (uint32_t i = 0; i < s.choice_count; ++i) {
            if (i != 0) j += ", ";
            j += Escape(s.choices[i]);
        }
        j += "]";
    }
    if ((s.type == ATMT_SETTING_STRING || s.type == ATMT_SETTING_KEY || s.type == ATMT_SETTING_PAD_BUTTON)
        && s.capacity > 0) {
        j += ", \"max_length\": " + std::to_string(s.capacity - 1);
    }
    return j + "}";
}

// ---------------------------------------------------------------- the stand-in api (child)
int __cdecl FakeRegister(const AtmtModApi*, const char* title, const AtmtSetting* table, uint32_t count) {
    if (InterlockedCompareExchange(&g_written, 1, 0) != 0) return static_cast<int>(count);
    std::string j = "    {\"mod\": " + Escape(g_mod_name.c_str());
    j += ", \"title\": " + Escape(title != nullptr && title[0] != '\0' ? title : g_mod_name.c_str());
    j += ", \"ini\": " + Escape(("atmt_mods/" + g_mod_name + ".ini").c_str());
    j += ", \"settings\": [\n";
    for (uint32_t i = 0; i < count; ++i) {
        j += "        " + SettingJson(table[i]) + (i + 1 < count ? ",\n" : "\n");
    }
    j += "    ]}";
    if (FILE* f = std::fopen(g_out_path.c_str(), "wb")) {
        std::fwrite(j.data(), 1, j.size(), f);
        std::fclose(f);
    }
    // Nothing the mod does after this matters here, and most of it would touch the game.
    ExitProcess(0);
}

void __cdecl FakeLog(const char*) {}
void* __cdecl FakeHookCreate(void*, void*, void** trampoline) {
    if (trampoline != nullptr) *trampoline = nullptr;
    return nullptr;   // "could not hook": the mod's own fallback, never a patch
}
int __cdecl FakeHookOp(void*) { return -1; }
void __cdecl FakeUnregister(const AtmtModApi*) {}
const AtmtSettingsGroup* __cdecl FakeAcquire(uint32_t* count, uint32_t* generation) {
    if (count != nullptr) *count = 0;
    if (generation != nullptr) *generation = 0;
    return nullptr;
}
void __cdecl FakeRelease(void) {}
int __cdecl FakeSet(const AtmtSetting*, const void*) { return -1; }
int __cdecl FakeCommit(void) { return 0; }
int __cdecl FakePublish(const AtmtModApi*, const char*, const void*) { return 0; }
const void* __cdecl FakeFind(const char*) { return nullptr; }

std::wstring Widen(const std::string& s) {
    std::wstring w(s.size() + 1, L'\0');
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, &w[0], static_cast<int>(w.size()));
    w.resize(n > 0 ? n - 1 : 0);
    return w;
}

std::string StemOf(const std::string& path) {
    size_t slash = path.find_last_of("\\/");
    std::string leaf = slash == std::string::npos ? path : path.substr(slash + 1);
    size_t dot = leaf.find_last_of('.');
    return dot == std::string::npos ? leaf : leaf.substr(0, dot);
}

int RunOne(const std::string& dll, const std::string& out, const std::string& info_out) {
    g_out_path = out;
    g_mod_name = StemOf(dll);
    // A crash is an answer ("no table"), never a dialog box waiting on a build.
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);

    // The working folder is a scratch one (main sets it), so anything the mod writes lands there.
    static std::wstring dir, name, ini, log;
    wchar_t cwd[MAX_PATH];
    GetCurrentDirectoryW(MAX_PATH, cwd);
    dir = cwd;
    name = Widen(g_mod_name);
    ini = dir + L"\\" + name + L".ini";
    log = dir + L"\\atmt_loader.log";

    static AtmtModApi api;
    ZeroMemory(&api, sizeof(api));
    api.version = ATMT_MOD_API_VERSION;
    api.size = sizeof(AtmtModApi);
    api.game_module = GetModuleHandleW(nullptr);
    api.game_dir = dir.c_str();
    api.mod_dir = dir.c_str();
    api.mod_name = name.c_str();
    api.config_path = ini.c_str();
    api.log_path = log.c_str();
    api.log = &FakeLog;
    api.log_error = &FakeLog;
    api.hook_create = &FakeHookCreate;
    api.hook_enable = &FakeHookOp;
    api.hook_disable = &FakeHookOp;
    api.hook_remove = &FakeHookOp;
    api.settings_register = &FakeRegister;
    api.settings_unregister = &FakeUnregister;
    api.settings_acquire = &FakeAcquire;
    api.settings_release = &FakeRelease;
    api.settings_set = &FakeSet;
    api.settings_commit = &FakeCommit;
    api.service_publish = &FakePublish;
    api.service_find = &FakeFind;

    HMODULE module = LoadLibraryW(Widen(dll).c_str());
    if (module == nullptr) return 3;
    // What the mod is, before anything of it runs (and whether or not it has a table).
    const auto describe =
        reinterpret_cast<AtmtModDescriptionFn>(GetProcAddress(module, ATMT_MOD_DESCRIPTION_NAME));
    const char* description = describe != nullptr ? describe() : nullptr;
    const std::string info = "    {\"mod\": " + Escape(g_mod_name.c_str())
                             + ", \"description\": " + Escape(description) + "}";
    if (FILE* f = std::fopen(info_out.c_str(), "wb")) {
        std::fwrite(info.data(), 1, info.size(), f);
        std::fclose(f);
    }
    auto init = reinterpret_cast<AtmtModInitFn>(GetProcAddress(module, ATMT_MOD_ENTRY_NAME));
    if (init == nullptr) return 4;
    const uint32_t accepted = init(&api);
    // Registered from a thread of its own, if at all.
    for (int waited = 0; waited < 8000; waited += 50) Sleep(50);
    return accepted == 0 ? 5 : 2;
}

// ---------------------------------------------------------------- the loader's own ini
const char kLoaderGroup[] =
    "    {\"mod\": \"atmt_loader\", \"title\": \"Loader\", \"ini\": \"atmt_loader.ini\", \"settings\": [\n"
    "        {\"type\": \"bool\", \"flags\": [\"restart\"], \"section\": \"Loader\", \"key\": \"Enabled\", "
    "\"label\": \"Load mods\", \"help\": \"master switch for the whole loader\", \"default\": \"true\"},\n"
    "        {\"type\": \"enum\", \"flags\": [\"restart\"], \"section\": \"Loader\", \"key\": \"LogLevel\", "
    "\"label\": \"atmt_loader.log\", \"help\": \"errors = only what failed (the file is only created then)\\n"
    "all = every line, for debugging\\noff = no file at all (the mods' own lines share it)\", "
    "\"default\": \"errors\", \"choices\": [\"errors\", \"all\", \"off\"]},\n"
    "        {\"type\": \"int\", \"flags\": [\"restart\", \"advanced\"], \"section\": \"Loader\", "
    "\"key\": \"LoadDelayMs\", \"label\": \"Load delay (ms)\", \"help\": \"wait this long before loading "
    "mods (0 = immediately)\", \"default\": \"0\", \"min\": 0, \"max\": 60000, \"step\": 100},\n"
    "        {\"type\": \"string\", \"flags\": [\"restart\", \"advanced\", \"readonly\"], \"section\": \"Loader\", "
    "\"key\": \"ModDir\", \"label\": \"Mod folder\", \"help\": \"where the mod dlls live; the manager "
    "always installs into atmt_mods\", \"default\": \"atmt_mods\"},\n"
    "        {\"type\": \"string\", \"flags\": [\"restart\", \"advanced\"], \"section\": \"Loader\", "
    "\"key\": \"Mods\", \"label\": \"Load only these\", \"help\": \"comma separated file names inside the mod "
    "folder; empty = every *.dll\", \"default\": \"\"}\n"
    "    ]}";

std::string ReadAll(const std::string& path) {
    std::string out;
    if (FILE* f = std::fopen(path.c_str(), "rb")) {
        char buf[4096];
        size_t n;
        while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) out.append(buf, n);
        std::fclose(f);
    }
    return out;
}

std::string FullPath(const std::string& p) {
    char buf[MAX_PATH * 2];
    DWORD n = GetFullPathNameA(p.c_str(), sizeof(buf), buf, nullptr);
    return (n > 0 && n < sizeof(buf)) ? std::string(buf) : p;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc == 5 && std::strcmp(argv[1], "--one") == 0) return RunOne(argv[2], argv[3], argv[4]);
    if (argc < 3) {
        std::fprintf(stderr, "usage: atmt_schema_dump <out.json> <mod.dll> [<mod.dll> ...]\n");
        return 1;
    }

    char self[MAX_PATH];
    GetModuleFileNameA(nullptr, self, MAX_PATH);
    char temp[MAX_PATH];
    GetTempPathA(MAX_PATH, temp);
    const std::string scratch = std::string(temp) + "atmt_schema_dump";
    CreateDirectoryA(scratch.c_str(), nullptr);

    std::vector<std::string> groups, mods;
    groups.push_back(kLoaderGroup);
    int with_table = 0;
    for (int i = 2; i < argc; ++i) {
        const std::string dll = FullPath(argv[i]);
        const std::string stem = StemOf(dll);
        const std::string dir = scratch + "\\" + stem;
        CreateDirectoryA(dir.c_str(), nullptr);
        const std::string out = dir + "\\group.json";
        const std::string info_out = dir + "\\mod.json";
        DeleteFileA(out.c_str());
        DeleteFileA(info_out.c_str());

        std::string cmd = "\"" + std::string(self) + "\" --one \"" + dll + "\" \"" + out + "\" \"" + info_out
                          + "\"";
        STARTUPINFOA si;
        ZeroMemory(&si, sizeof(si));
        si.cb = sizeof(si);
        PROCESS_INFORMATION pi;
        std::vector<char> line(cmd.begin(), cmd.end());
        line.push_back('\0');
        if (!CreateProcessA(nullptr, line.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr,
                            dir.c_str(), &si, &pi)) {
            std::fprintf(stderr, "schema: %s: could not start the child (%lu)\n", stem.c_str(), GetLastError());
            return 1;
        }
        if (WaitForSingleObject(pi.hProcess, 20000) == WAIT_TIMEOUT) TerminateProcess(pi.hProcess, 9);
        DWORD code = 0;
        GetExitCodeProcess(pi.hProcess, &code);
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);

        const std::string info = ReadAll(info_out);
        if (!info.empty()) {
            mods.push_back(info);
            if (info.find("\"description\": \"\"") != std::string::npos) {
                std::printf("schema: %-24s has no %s\n", stem.c_str(), ATMT_MOD_DESCRIPTION_NAME);
            }
        }
        const std::string group = ReadAll(out);
        if (!group.empty()) {
            groups.push_back(group);
            ++with_table;
            std::printf("schema: %-24s table written\n", stem.c_str());
        } else {
            std::printf("schema: %-24s no settings table (exit %lu)\n", stem.c_str(), code);
        }
    }

    // The names a KEY or PAD_BUTTON setting accepts, straight from the parser the mods use
    // (shared/atmt_input.h), so the manager's pickers offer exactly those and know none of their own.
    std::string keys, pads;
    for (unsigned vk = 1; vk < 256; ++vk) {
        const std::string name = atmt::KeyName(vk);
        if (!name.empty() && atmt::KeyFromName(name) == vk) keys += (keys.empty() ? "" : ", ") + Escape(name.c_str());
    }
    for (unsigned bit = 1; bit <= 0x8000; bit <<= 1) {
        const char* name = atmt::PadButtonName(static_cast<unsigned short>(bit));
        if (std::strcmp(name, "(none)") != 0) pads += (pads.empty() ? "" : ", ") + Escape(name);
    }

    std::string json = "{\n  \"schema_version\": 3,\n";
    // A KEY setting is a key chord: these modifiers, then one of the keys, joined by the separator
    // ("Ctrl+F3", in atmt::KeyChordName's order).
    std::string key_mods;
    size_t mod_count = 0;
    const atmt::ChordModifierName* mod_names = atmt::ChordModifierNames(&mod_count);
    for (size_t i = 0; i < mod_count; ++i) key_mods += (key_mods.empty() ? "" : ", ") + Escape(mod_names[i].name);
    json += "  \"input\": {\"keys\": [" + keys + "], \"key_modifiers\": [" + key_mods
            + "], \"key_chord_separator\": \"+\", \"pad_buttons\": [" + pads
            + "], \"pad_chord_separator\": \"+\"},\n";
    json += "  \"mods\": [\n";
    for (size_t i = 0; i < mods.size(); ++i) json += mods[i] + (i + 1 < mods.size() ? ",\n" : "\n");
    json += "  ],\n";
    json += "  \"groups\": [\n";
    for (size_t i = 0; i < groups.size(); ++i) json += groups[i] + (i + 1 < groups.size() ? ",\n" : "\n");
    json += "  ]\n}\n";
    FILE* f = std::fopen(argv[1], "wb");
    if (f == nullptr) {
        std::fprintf(stderr, "schema: cannot write %s\n", argv[1]);
        return 1;
    }
    std::fwrite(json.data(), 1, json.size(), f);
    std::fclose(f);
    std::printf("schema: %d mod table(s) + the loader -> %s\n", with_table, argv[1]);
    return with_table > 0 ? 0 : 1;
}
