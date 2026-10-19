// options.cpp - the command line, and the settings file.
#include "al.h"

#include "atmt_input.h"

#include <shellapi.h>

#include <cstdlib>
#include <cstring>

namespace al {

std::wstring ProcessCommandLine() {
    const wchar_t* line = GetCommandLineW();
    return line != nullptr ? std::wstring(line) : std::wstring();
}

bool HasSwitch(const std::wstring& command_line, const wchar_t* sw) {
    if (command_line.empty() || sw == nullptr || *sw == L'\0') return false;
    // The same split Windows itself uses, so quoting quirks cannot make the check lie.
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(command_line.c_str(), &argc);
    if (argv == nullptr) return false;
    bool found = false;
    for (int i = 1; i < argc && !found; ++i) {   // [0] is the executable
        if (argv[i] != nullptr && _wcsicmp(argv[i], sw) == 0) found = true;
    }
    LocalFree(argv);
    return found;
}

bool WantsLoad(const Options& options, const std::wstring& command_line, bool override_held) {
    if (!options.enabled) return false;
    bool load = false;
    switch (options.autoload) {
        case Autoload::kAlways: load = true; break;
        case Autoload::kParameter: load = HasSwitch(command_line, kSwitch); break;
        default: break;
    }
    return load != override_held;
}

namespace {

// XINPUT_STATE, read straight from xinput1_3's export: the game imports it, so it is loaded, and
// the export is not the game's import slot the overlay patches - this read changes nothing for anyone.
struct PadState {
    uint32_t packet;
    uint16_t buttons;
    uint8_t lt, rt;
    int16_t lx, ly, rx, ry;
};
typedef uint32_t(__stdcall* XInputGetStateFn)(uint32_t index, PadState* state);

uint16_t PadButtonsDown() {
    static XInputGetStateFn xinput = nullptr;
    if (xinput == nullptr) {
        HMODULE module = GetModuleHandleW(L"xinput1_3.dll");
        if (module == nullptr) return 0;
        xinput = reinterpret_cast<XInputGetStateFn>(GetProcAddress(module, "XInputGetState"));
        if (xinput == nullptr) return 0;
    }
    uint16_t buttons = 0;
    for (uint32_t i = 0; i < 4; ++i) {
        PadState state{};
        if (xinput(i, &state) == 0) {
            buttons = static_cast<uint16_t>(buttons | atmt::PadButtons(state.buttons, state.lt, state.rt));
        }
    }
    return buttons;
}

}  // namespace

bool OverrideHeld(unsigned key_chord, unsigned short pad_chord) {
    if (key_chord != 0 && atmt::KeyDown(atmt::ChordKey(key_chord))) {
        const unsigned want = atmt::ChordModifiers(key_chord);
        if ((atmt::ModifiersDown() & want) == want) return true;
    }
    return pad_chord != 0 && (PadButtonsDown() & pad_chord) == pad_chord;
}

// A section header ("[General]") is not a key: it has no '=' and is skipped by the reader.
Options LoadOptions(const std::wstring& ini_file) {
    Options o;
    FILE* f = _wfopen(ini_file.c_str(), L"rb");
    if (f == nullptr) return o;
    char line[512];
    while (fgets(line, sizeof(line), f)) {
        char* p = line;
        while (*p == ' ' || *p == '\t') ++p;
        if (*p == ';' || *p == '#' || *p == '\0' || *p == '\r' || *p == '\n') continue;
        char* eq = strchr(p, '=');
        if (eq == nullptr) continue;
        *eq = '\0';
        char* key = p;
        char* val = eq + 1;
        while (*val == ' ' || *val == '\t') ++val;
        size_t klen = strlen(key);
        while (klen > 0 && (key[klen - 1] == ' ' || key[klen - 1] == '\t')) key[--klen] = '\0';
        size_t vlen = strlen(val);
        while (vlen > 0 && (val[vlen - 1] == '\r' || val[vlen - 1] == '\n' || val[vlen - 1] == ' ')) {
            val[--vlen] = '\0';
        }
        if (char* comment = strchr(val, ';')) {      // inline comments are documentation here
            *comment = '\0';
            vlen = strlen(val);
            while (vlen > 0 && (val[vlen - 1] == ' ' || val[vlen - 1] == '\t')) val[--vlen] = '\0';
        }

        if (_stricmp(key, "Enabled") == 0) {
            o.enabled = _stricmp(val, "false") != 0 && strcmp(val, "0") != 0;
        } else if (_stricmp(key, "Autoload") == 0) {
            for (int i = 0; i < 3; ++i) {   // an unknown token keeps the default (always)
                if (_stricmp(val, kAutoloadTokens[i]) == 0) o.autoload = static_cast<Autoload>(i);
            }
        } else if (_stricmp(key, "IncludeAutosaves") == 0) {
            o.include_autosaves = _stricmp(val, "false") != 0 && strcmp(val, "0") != 0;
        } else if (_stricmp(key, "Diagnostics") == 0) {
            o.diagnostics = _stricmp(val, "true") == 0 || strcmp(val, "1") == 0;
        } else if (_stricmp(key, "ProbeFileApis") == 0) {
            o.probe_file_apis = _stricmp(val, "true") == 0 || strcmp(val, "1") == 0;
        } else if (_stricmp(key, "ProbeReader") == 0) {
            o.probe_reader = _stricmp(val, "true") == 0 || strcmp(val, "1") == 0;
        } else if (_stricmp(key, "ProbeWriters") == 0) {
            o.probe_writers = _stricmp(val, "true") == 0 || strcmp(val, "1") == 0;
        } else if (_stricmp(key, "ProbeCallbacks") == 0) {
            o.probe_callbacks = _stricmp(val, "true") == 0 || strcmp(val, "1") == 0;
        } else if (_stricmp(key, "ProbeFopen") == 0) {
            o.probe_fopen = _stricmp(val, "true") == 0 || strcmp(val, "1") == 0;
        } else if (_stricmp(key, "ProbeFade") == 0) {
            o.probe_fade = _stricmp(val, "true") == 0 || strcmp(val, "1") == 0;
        } else if (_stricmp(key, "ProbeMemset") == 0) {
            o.probe_memset = _stricmp(val, "true") == 0 || strcmp(val, "1") == 0;
        } else if (_stricmp(key, "ProbeRequest") == 0) {
            o.probe_request = _stricmp(val, "true") == 0 || strcmp(val, "1") == 0;
        } else if (_stricmp(key, "OverrideKey") == 0) {
            o.override_key = val;   // empty = no key
        } else if (_stricmp(key, "OverridePad") == 0) {
            o.override_pad = val;   // empty = no pad chord
        } else if (_stricmp(key, "PadScript") == 0) {
            o.pad_script = val;   // a file name in the mod folder; empty disables the injector
        } else if (_stricmp(key, "SaveDir") == 0 && val[0] != '\0') {
            // ASCII in practice; a wide path from the ini would need UTF-8 decoding, which the
            // loader's own reader does the same way for every mod.
            std::wstring wide;
            for (const char* c = val; *c != '\0'; ++c) wide.push_back(static_cast<wchar_t>(*c));
            o.save_dir = wide;
        }
    }
    fclose(f);
    return o;
}

}  // namespace al
