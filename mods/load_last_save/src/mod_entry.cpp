// mod_entry.cpp - the mod's entry point (see shared/mod_api.h).
//
// What it does, in order, and nothing else:
//   1. does this start load? Autoload=always (the default), or =parameter with `-al` on the
//      process's command line. If not, say so and stop.
//   2. which save is newest (options: SaveDir, IncludeAutosaves)?
//   3. say what it chose and why (the chosen file, its time, how many candidates).
//   4. ask the game to load it (TriggerLoad - the single function that needs the game).
#include "al.h"

#include "code_check.h"
#include "load_last_save_api.h"
#include "mod_api.h"

#include <cstdio>
#include <string>

namespace {

const AtmtModApi* g_api = nullptr;
volatile LONG g_stop = 0;
volatile LONG g_started = 0;

void LogUtf8(const std::string& text) {
    if (g_api != nullptr && g_api->log != nullptr) g_api->log(text.c_str());
}

void LogErrorUtf8(const std::string& text) {
    if (g_api != nullptr && g_api->log_error != nullptr) g_api->log_error(text.c_str());
}

// Every piece of ed8.exe this mod hooks, calls or patches (the probes' targets included), by its
// first bytes in the supported exe. SenPatcher (v1.3.1 and its current sources) changes none of
// them; another build or patch that does gets no hook at all instead of a crash.
bool GameCodeMatches(std::string* why_not) {
    static const atmt_code::Expect kExpected[] = {
        {al::kGameSaveReader, "\x55\x8b\xec\x81\xec\x0c\x01\x00\x00", 9, "the save reader"},
        {al::kGameSaveRequest, "\x55\x8b\xec\x8b\x45\x08\x53\x56\x57", 9, "the save/load request"},
        {al::kGameFlowEntry, "\x55\x8b\xec\x81\xec\x84\x00\x00\x00", 9, "the load flow entry"},
        {0x00690B30, "\x55\x8b\xec\x83\xec\x48\xa1\x58", 8, "the title update"},
        {0x0064A720, "\x55\x8b\xec\x8b\x45\x08\xd9\x05", 8, "the save menu's open"},
        {0x0040896D, "\xe9\x7e\x77\x28\x00", 5, "the title's load callback"},
        {0x0063EEB0, "\x32\xc0\x66\x83\x79\x02\x00\x74", 8, "the overlay check"},
        {0x00702580, "\x66\x83\xb9\x08\x4e\x0b\x00\x00", 8, "the message box check"},
        {0x00702700, "\x55\x8b\xec\x8d\x81\x0c\x4e\x0b", 8, "the message input check"},
        {al::kFadeWidgetKick, "\x55\x8b\xec\x51\x53\x56\x8b\xf1", 8, "the widget fade-in"},
        {al::kFadeListBuild, "\x55\x8b\xec\x6a\xff\x68\x16\xc4", 8, "the save list build"},
        {al::kFadePrimeSetter, "\x55\x8b\xec\x8b\x45\x08\x89\x41\x1c", 9, "the fade prime setter"},
        {al::kFadeDurationGlobal, "\xcd\xcc\x4c\x3e", 4, "the fade duration (0.2f)"},
    };
    char why[256];
    if (atmt_code::CheckAll(kExpected, why, sizeof(why))) return true;
    if (why_not != nullptr) *why_not = why;
    return false;
}

// ---------------------------------------------------------------- the "Autoload" menu
// Registered with the loader, so the overlay's settings bar shows them and saves a change into this
// mod's ini. They all decide what happens at startup, so a change applies to the next start; the
// start itself still reads them through LoadOptions (which the offline tests cover).
int32_t g_set_autoload = static_cast<int32_t>(al::Autoload::kAlways);
int32_t g_set_include_autosaves = 1;
int32_t g_set_title_load = 1;
int32_t g_set_diagnostics = 0;
char g_set_save_dir[260] = "";

const AtmtSetting kSettings[] = {
    {ATMT_SETTING_ENUM, ATMT_SETTING_RESTART, "General", "Autoload", "Load the newest save",
     "always    - on every start\n"
     "parameter - only when the game is started with -al (Steam: launch options)\n"
     "disabled  - never",
     &g_set_autoload, 0, 0, 0, 0, al::kAutoloadTokens, 3},
    {ATMT_SETTING_BOOL, ATMT_SETTING_RESTART, "General", "IncludeAutosaves", "Autosaves count",
     "an autosave can be the newest save; off = only ever a manual slot", &g_set_include_autosaves},
    {ATMT_SETTING_STRING, ATMT_SETTING_RESTART | ATMT_SETTING_ADVANCED, "General", "SaveDir",
     "Save folder",
     "where the saves are; none = the game's own (%USERPROFILE%\\Saved Games\\Falcom\\ed8)",
     g_set_save_dir, sizeof(g_set_save_dir)},
    {ATMT_SETTING_BOOL, ATMT_SETTING_RESTART | ATMT_SETTING_ADVANCED, "General", "TitleLoad",
     "Title route", "load through the title's own save menu (the route that enters the game)",
     &g_set_title_load},
    {ATMT_SETTING_BOOL, ATMT_SETTING_RESTART | ATMT_SETTING_ADVANCED, "General", "Diagnostics",
     "Diagnostics", "list every candidate save in the log, not only the chosen one",
     &g_set_diagnostics},
};

// The log is the mod's only voice, so paths are narrowed (they are ASCII in practice).
std::string Narrow(const std::wstring& w) {
    std::string out;
    out.reserve(w.size());
    for (wchar_t c : w) out.push_back(c < 128 ? static_cast<char>(c) : '?');
    return out;
}

// The name alone, which is what the tests assert on (the folder is a fixture).
std::wstring LeafName(const std::wstring& path) {
    const size_t slash = path.find_last_of(L"\\/");
    return slash == std::wstring::npos ? path : path.substr(slash + 1);
}

// ---------------------------------------------------------------- the service (load_last_save_api.h)
// Another mod asks for a save to be loaded the next time the title is up. Only arming happens here;
// the title's own update does the rest and logs its verdict (title_load.cpp).
int __cdecl ServiceLoadAtTitle(const char* file_name, char* why_not, uint32_t why_not_size) {
    std::string why;
    const std::string leaf = file_name != nullptr ? file_name : "";
    const bool armed = !leaf.empty() && al::ArmTitleLoad(leaf, &why);
    if (armed) {
        LogUtf8("al: another mod asked for " + leaf + " at the title; the title route is armed");
    } else {
        if (why.empty()) why = "no file name";
        LogUtf8("al: another mod asked for '" + leaf + "' at the title - refused: " + why);
        if (why_not != nullptr && why_not_size > 0) {
            _snprintf(why_not, why_not_size - 1, "%s", why.c_str());
            why_not[why_not_size - 1] = '\0';
        }
    }
    return armed ? 1 : 0;
}

const AtmtLoadLastSaveApi kService = {ATMT_LOAD_LAST_SAVE_API_VERSION, sizeof(AtmtLoadLastSaveApi),
                                     &ServiceLoadAtTitle};

// The title route (src/title_load.cpp): arm it, then report what the game did. The steps run on the
// game's thread inside its title update; this thread only watches and logs the verdict. Evidence, in
// the order it appears: the title's own steps, the reader opening the chosen file (the reader hook's
// name), the save menu calling the title back with success, and the title scene going away.
bool RunTitleLoad(const std::string& leaf, std::string* why_not) {
    if (!al::ArmTitleLoad(leaf, why_not)) return false;
    LogUtf8("al: will load " + leaf + " through the title's save menu and the save manager");
    // The game pauses while unfocused, so this is a generous ceiling, not an expectation.
    const DWORD deadline = GetTickCount() + 10 * 60 * 1000;
    bool title_seen = false;
    bool read_seen = false;
    while (al::TitleLoadResult() == al::kTitleLoadPending) {
        if (g_stop != 0) return false;
        if (GetTickCount() > deadline) {
            al::CancelTitleLoad(al::kTitleLoadTimedOut);
            break;
        }
        if (!title_seen && al::TitleUpdateCalls() > 0) {
            title_seen = true;
            LogUtf8("al: the title screen is running");
        }
        if (!read_seen && al::LastGameReadName() == leaf) {
            read_seen = true;
            LogUtf8("al: the game opened " + leaf);
        }
        Sleep(100);
    }
    const al::TitleLoadOutcome outcome = al::TitleLoadResult();
    if (outcome != al::kTitleLoadEntered && outcome != al::kTitleLoadPrompt) {
        if (why_not != nullptr) {
            *why_not = outcome == al::kTitleLoadTimedOut
                           ? std::string("timed out (") + al::TitleLoadStageName() + ")"
                           : "the game's load menu did not report success";
        }
        return false;
    }
    if (!read_seen && al::LastGameReadName() == leaf) read_seen = true;
    // The title scene stops updating once the game has left it - the last, independent witness.
    long calls = al::TitleUpdateCalls();
    bool title_gone = false;
    for (int waited = 0; waited < 60000 && g_stop == 0; waited += 2000) {
        Sleep(2000);
        const long now = al::TitleUpdateCalls();
        if (now == calls) {
            title_gone = true;
            break;
        }
        calls = now;
    }
    char line[320];
    _snprintf(line, sizeof(line), "al: LOADED %s - %s; the game %s the file; the title scene %s",
              leaf.c_str(),
              outcome == al::kTitleLoadEntered ? "the load menu reported success"
                                               : "success, the title shows its own prompt",
              read_seen ? "opened" : "(reader not hooked?) was not seen opening",
              title_gone ? "has ended" : "is still updating after 60 s");
    LogUtf8(line);
    return true;
}

DWORD WINAPI Run(LPVOID) {
    const std::wstring config_path =
        (g_api != nullptr && g_api->config_path != nullptr) ? g_api->config_path : std::wstring();
    const al::Options options = al::LoadOptions(config_path);
    if (!options.enabled) {
        LogUtf8("al: disabled in its ini (Enabled=false)");
        return 0;
    }

    {
        std::string why_not;
        if (!GameCodeMatches(&why_not)) {
            LogErrorUtf8("al: " + why_not + " - not installed, no save is loaded automatically");
            return 0;
        }
    }

    const std::wstring command_line = al::ProcessCommandLine();
    const bool wants_load = al::WantsLoad(options, command_line);

    // The reader hook is what lets the mod call the game's own load function; it is needed both for
    // the probe diagnostics and for the real thing, so install it whenever either is wanted.
    if (options.probe_reader || wants_load) {
        std::string why_not;
        if (al::InstallReaderProbe(g_api, &why_not)) {
            LogUtf8(options.probe_reader ? "al: reader probe is on (ProbeReader=true)"
                                         : "al: reader hooked (needed to perform the load)");
        } else {
            LogUtf8("al: reader hook could not be installed: " + why_not);
        }
    }
    // The title route's hook goes in first thing: the title screen comes up a few seconds into the
    // game and its update is what performs the load (see src/title_load.cpp). It is installed with or
    // without -al: other mods arm it later through the service (battle_load sends the game back to the
    // title and has the save it picked loaded there). Idle, it costs one compare per title frame.
    bool title_hooked = false;
    if (options.title_load) {
        std::string why_not;
        title_hooked = al::InstallTitleHook(g_api, &why_not);
        LogUtf8(title_hooked ? "al: the title update hook is installed"
                             : "al: the title update hook could not be installed: " + why_not);
        if (title_hooked && g_api->service_publish != nullptr
            && g_api->service_publish(g_api, ATMT_LOAD_LAST_SAVE_SERVICE, &kService) == 0) {
            LogUtf8("al: the title route is offered to other mods (" ATMT_LOAD_LAST_SAVE_SERVICE ")");
        }
    }
    al::SetStartDelayMs(options.start_delay_ms);
    if (wants_load) {
        // Diagnostic: observes the save menu's "arm the manager" step (FUN_0064CAD0) and logs each
        // phase the game uses, once each - a title load shows up as phase 2 (see probe_flow.cpp).
        std::string flow_why;
        if (al::InstallFlowHook(g_api, &flow_why)) {
            LogUtf8("al: the game's load flow hook is installed");
        } else {
            LogUtf8("al: the load flow hook could not be installed: " + flow_why);
        }
    }
    if (options.probe_file_apis) {
        std::string why_not;
        if (al::InstallFileProbe(g_api, &why_not)) {
            LogUtf8("al: file probe is on (ProbeFileApis=true)");
        } else {
            LogUtf8("al: file probe could not be installed: " + why_not);
        }
    }
    if (options.probe_memset) {
        std::string why_not;
        if (al::InstallMemsetProbe(g_api, &why_not)) {
            LogUtf8("al: memset probe is on (ProbeMemset=true)");
        } else {
            LogUtf8("al: memset probe could not be installed: " + why_not);
        }
    }
    if (options.probe_request) {
        // Diagnostic: record the arguments of the game's own save/load request, so the mod can
        // replay a real load (see src/probe_request.cpp).
        std::string why_not;
        if (al::InstallRequestProbe(g_api, &why_not)) {
            LogUtf8("al: request probe is on (ProbeRequest=true)");
        } else {
            LogUtf8("al: request probe could not be installed: " + why_not);
        }
    }
    if (options.probe_writers) {
        // Diagnostic: which code writes the request state (the menu's load action, in practice).
        std::string why_not;
        if (al::InstallWriterProbe(g_api, &why_not)) {
            LogUtf8("al: writer probe is on (ProbeWriters=true)");
        } else {
            LogUtf8("al: writer probe could not be installed: " + why_not);
        }
    }
    if (options.probe_callbacks) {
        // Diagnostic: what the load menu installs as the save manager's completion callback. The
        // title screen leaves those fields zero, which is why a load driven from it only reads the
        // file (see src/probe_callbacks.cpp).
        std::string why_not;
        if (al::InstallCallbackProbe(g_api, &why_not)) {
            LogUtf8("al: callback probe is on (ProbeCallbacks=true)");
        } else {
            LogUtf8("al: callback probe could not be installed: " + why_not);
        }
    }
    if (options.probe_fopen) {
        // Diagnostic: which game function opens a save file (the CRT's fopen entry points).
        std::string why_not;
        if (al::InstallFopenProbe(g_api, &why_not)) {
            LogUtf8("al: fopen probe is on (ProbeFopen=true)");
        } else {
            LogUtf8("al: fopen probe could not be installed: " + why_not);
        }
    }
    if (options.probe_fade) {
        // Diagnostic: when the load menu's own fade/transition calls fire relative to ArmTitleLoad,
        // and the arguments that look like durations (src/probe_fade.cpp).
        std::string why_not;
        if (al::InstallFadeProbe(g_api, &why_not)) {
            LogUtf8("al: fade probe is on (ProbeFade=true)");
        } else {
            LogUtf8("al: fade probe could not be installed: " + why_not);
        }
    }
    if (!options.pad_script.empty() && g_api != nullptr && g_api->mod_dir != nullptr) {
        // Test-only input (see src/pad_inject.cpp). It exists because the game ignores injected
        // keystrokes, so reaching a menu screen without a human needs input at the API level.
        std::wstring script_path = g_api->mod_dir;
        if (!script_path.empty()) script_path += L"\\";
        std::wstring name;
        for (char c : options.pad_script) name.push_back(static_cast<wchar_t>(c));
        script_path += name;
        if (GetFileAttributesW(script_path.c_str()) != INVALID_FILE_ATTRIBUTES) {
            std::string why_not;
            if (al::InstallPadInjector(g_api, script_path, &why_not)) {
                LogUtf8("al: pad injector is on (" + options.pad_script + ")");
            } else {
                LogUtf8("al: pad injector could not be installed: " + why_not);
            }
        }
    }
    if (!wants_load) {
        // Not an error: the normal way the game is started. Say so, so a test can assert it.
        LogUtf8(options.autoload == al::Autoload::kDisabled
                    ? "al: Autoload=disabled, nothing to do"
                    : "al: no -al on the command line, nothing to do");
        return 0;
    }
    LogUtf8(options.autoload == al::Autoload::kAlways ? "al: Autoload=always - loading the newest save"
                                                      : "al: -al given - loading the newest save");

    const std::wstring dir = options.save_dir.empty() ? al::DefaultSaveDir() : options.save_dir;
    const std::vector<al::SaveFile> saves = al::FindSaves(dir, options.include_autosaves);
    if (dir.empty()) {
        LogUtf8("al: -al given, but no save folder could be determined");
        return 0;
    }
    if (saves.empty()) {
        LogUtf8("al: -al given, but no saves in " + Narrow(dir) + " - nothing to load");
        return 0;
    }

    const al::SaveFile& chosen = saves.front();
    {
        char line[512];
        _snprintf(line, sizeof(line), "al: chosen %s (%s, size %llu, newest of %u)%s",
                  Narrow(LeafName(chosen.path)).c_str(), al::FormatTime(chosen.time).c_str(),
                  chosen.size, static_cast<unsigned>(saves.size()),
                  options.include_autosaves ? "" : " [autosaves excluded]");
        LogUtf8(line);
    }
    if (options.diagnostics) {
        // Every candidate, so a wrong choice is visible in the log instead of being a mystery.
        for (size_t i = 0; i < saves.size(); ++i) {
            char line[512];
            _snprintf(line, sizeof(line), "al:   candidate %u/%u: %s (%s)", static_cast<unsigned>(i + 1),
                      static_cast<unsigned>(saves.size()), Narrow(LeafName(saves[i].path)).c_str(),
                      al::FormatTime(saves[i].time).c_str());
            LogUtf8(line);
        }
    }

    if (title_hooked) {
        std::string why_not;
        if (RunTitleLoad(Narrow(LeafName(chosen.path)), &why_not)) return 0;
        LogUtf8("al: the title route did not load " + Narrow(LeafName(chosen.path)) + " - " + why_not);
        return 0;
    }

    std::string why_not;
    const bool done = al::TriggerLoad(chosen.path, &why_not);
    if (done) {
        LogUtf8("al: asked the game to load " + Narrow(LeafName(chosen.path)));
    } else {
        LogUtf8("al: would load " + Narrow(LeafName(chosen.path)) + " - " + why_not);
    }
    return 0;
}

}  // namespace

extern "C" {

__declspec(dllexport) uint32_t __cdecl AtmtModInit(const AtmtModApi* api) {
    if (api == nullptr || api->version < ATMT_MOD_API_VERSION) return 0;
    if (api->size < sizeof(AtmtModApi)) return 0;   // truncated service table
    if (InterlockedCompareExchange(&g_started, 1, 0) != 0) {
        if (api->log != nullptr) api->log("al: already initialised in this process, declining");
        return 0;
    }
    g_api = api;
    // Seeded with the defaults the start would use; the registry reads the ini over them (and adds
    // the keys the ini does not have yet).
    const al::Options defaults;
    g_set_autoload = static_cast<int32_t>(defaults.autoload);
    g_set_include_autosaves = defaults.include_autosaves ? 1 : 0;
    g_set_title_load = defaults.title_load ? 1 : 0;
    g_set_diagnostics = defaults.diagnostics ? 1 : 0;
    if (api->settings_register != nullptr) {
        api->settings_register(api, "Autoload", kSettings, sizeof(kSettings) / sizeof(kSettings[0]));
    }
    // Work on our own thread, so the loader's startup is never held up.
    HANDLE t = CreateThread(nullptr, 0, Run, nullptr, 0, nullptr);
    if (t == nullptr) return 0;
    CloseHandle(t);
    return ATMT_MOD_API_VERSION;
}

__declspec(dllexport) void __cdecl AtmtModShutdown(void) {
    InterlockedExchange(&g_stop, 1);
}

// One sentence for the overlay and the manager (shared/mod_api.h, AtmtModDescription).
__declspec(dllexport) const char* __cdecl AtmtModDescription(void) {
    return "Loads the newest save straight from the title screen, with the -al launch option or "
           "always.";
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) DisableThreadLibraryCalls(inst);
    return TRUE;
}

}  // extern "C"
