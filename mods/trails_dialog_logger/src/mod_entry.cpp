// mod_entry.cpp - the dialog logger as a mod of the loader (see shared/mod_api.h).
//
// The mod does one thing: it hooks the game's own "set the message's run" call and writes
// what that call is given (the text) and what its object holds (the name plate) to
// atmt_dialogs.jsonl. There is no catalog, no cache and no memory scanning - see
// addr_hook.cpp for why that is possible.
#include "atmt.h"

#include "mod_api.h"

#include <cstdio>
#include <exception>
#include <string>

namespace atmt {
bool InstallAddressHooks(const Config& config, LogSink& sink);
}

namespace {

const AtmtModApi* g_api = nullptr;
atmt::LogSink* g_sink = nullptr;
volatile LONG g_stop = 0;
// Set by AtmtModInit: the mod only initialises once per process (see the guard there).
volatile LONG g_mod_initialised = 0;

// For log messages: paths are ASCII in practice.
std::string NarrowPath(const std::wstring& path) {
    std::string out;
    out.reserve(path.size());
    for (wchar_t c : path) out.push_back(c < 128 ? static_cast<char>(c) : '?');
    return out;
}

// The mod's own ini, loader-guaranteed to exist (<mod_dir>\trails_dialog_logger.ini -
// AtmtModApi::config_path); falls back to building that same path if the loader did not hand one
// over (only the offline self test does that). The one config file: both what is read at startup
// and where a live change from the panel is saved (through the loader's settings registry - see
// RegisterPanelSettings in overlay.cpp). This mod
// used to also accept a legacy <game>\atmt_config.ini, layered on top of this one - dropped because
// the only reason for it was a population of players upgrading from a pre-loader version of the
// project that this project does not have.
std::wstring ModConfigPath() {
    if (g_api != nullptr && g_api->config_path != nullptr) return g_api->config_path;
    return atmt::GetSelfDir() + L"\\trails_dialog_logger.ini";
}

// The logger thread's copy of the "may write files" setting, so the one place that writes an error
// report can honour it as well: with LogToFile=false a startup failure goes to the loader log
// (which the loader owns) instead of a file of ours.
volatile LONG g_write_files = 1;

void ReportStartupFailure(const char* what) {
    const std::wstring dir = atmt::GetGameDir();
    if (InterlockedCompareExchange(&g_write_files, 0, 0) != 0) {
        if (FILE* f = _wfopen((dir + L"\\atmt_startup_error.txt").c_str(), L"wb")) {
            std::fprintf(f, "atmt: startup failed: %s\n", what);
            std::fclose(f);
        }
    }
    atmt::HostLogError(what);
}

DWORD WINAPI FlushThread(LPVOID arg) {
    atmt::LogSink* s = static_cast<atmt::LogSink*>(arg);
    while (InterlockedCompareExchange(&g_stop, 0, 0) == 0) {
        Sleep(40);
        atmt::ReleaseHeldPages(false);
        s->FlushPending();
    }
    atmt::ReleaseHeldPages(true);
    s->FlushPending();   // last chance for lines that were still waiting
    return 0;
}

DWORD WINAPI InitThread(LPVOID) {
    using namespace atmt;
    try {
        const std::wstring game_dir = GetGameDir();
        const std::wstring config_path = ModConfigPath();
        // Static: the settings outlive this thread (the hook keeps its own copy, but nothing
        // here should depend on a stack frame that is about to disappear).
        static Config config;
        config = LoadConfig(config_path);
        {
            // Say which file was used and whether it existed: a config that is silently
            // ignored is the kind of thing that wastes hours (it did).
            char msg[640];
            _snprintf(msg, sizeof(msg), "config: %s (%s)", NarrowPath(config_path).c_str(),
                      GetFileAttributesW(config_path.c_str()) != INVALID_FILE_ATTRIBUTES
                          ? "read"
                          : "not found - using built-in defaults");
            HostLog(msg);
        }
        if (!config.unknown_keys.empty()) {
            const std::string note = "config: ignored unknown keys: " + config.unknown_keys;
            HostLogError(note.c_str());
        }
        if (!config.misplaced_keys.empty()) {
            const std::string note = "config: ignored keys outside their section: " + config.misplaced_keys;
            HostLogError(note.c_str());
        }
        if (!config.moved_keys.empty()) {
            const std::string note = "config: ignored " + config.moved_keys
                                     + " - the overlay's settings live in atmt_overlay.ini now "
                                       "(its \"Overlay\" menu)";
            HostLogError(note.c_str());
        }
        InterlockedExchange(&g_write_files, config.log_to_file ? 1 : 0);

        static LogSink sink;
        g_sink = &sink;

        if (!config.enabled) {
            HostLog("dialog logger: disabled by config");
            return 0;
        }
        const std::wstring log_dir = game_dir.empty() ? GetSelfDir() : game_dir;
        sink.set_write_files(config.log_to_file);
        if (!sink.Open(log_dir)) {
            ReportStartupFailure("could not open atmt_dialogs.jsonl (is the folder writable?)");
            return 0;
        }
        sink.set_pending_ms(config.pending_speaker_ms);

        // The lines the panel shows are the lines the sink writes: one history, bounded by the
        // same setting the panel scrolls within (see history.h).
        static History history;
        history.set_capacity(config.overlay_lines);
        sink.set_history(&history);

        char msg[512];
        _snprintf(msg, sizeof(msg),
                  "logger started: source = the game's own message setter (0x%08x), "
                  "plate = +0x%x, text = run + %u, pending = %u ms",
                  static_cast<unsigned>(config.message_hook_address),
                  config.message_plate_offset, config.message_text_skip,
                  config.pending_speaker_ms);
        sink.Note(msg);
        HostLog(msg);
        if (!config.log_to_file) {
            // The one setting that changes what the mod leaves behind, so it is reported loudly and
            // once: with it off, the arguments that ask for files are ignored on purpose.
            char quiet[256];
            _snprintf(quiet, sizeof(quiet),
                      "logger: LogToFile=false - no files are written (no atmt_dialogs.jsonl, no "
                      "atmt_latest.txt, and Diagnostics is ignored); the overlay panel "
                      "still shows every line%s",
                      config.overlay ? "" : " (but the panel is off too)");
            HostLog(quiet);
        }

        InstallAddressHooks(config, sink);

        // The panel: a view of what was just logged, drawn over the game (docs/OVERLAY.md). It is
        // deliberately unable to take the logger down with it - every failure of it is a note in
        // the log and nothing more.
        if (!InstallPanel(config, sink, history, g_api)) {
            sink.Note("panel: not available in this process (the log itself is unaffected)");
        }
        // Development only (DevReload=false by default): hand a running game over to a new build.
        if (config.dev_reload) StartDevReloadWatcher(config, g_api);

        // Writes the lines that are waiting for their name plate.
        CreateThread(nullptr, 0, FlushThread, &sink, 0, nullptr);
    } catch (const std::exception& e) {
        // never take the game down with us - but do not fail silently either: a startup
        // problem would otherwise look like "the mod does nothing"
        ReportStartupFailure(e.what());
    } catch (...) {
        ReportStartupFailure("unknown exception in the logger thread");
    }
    return 0;
}

}  // namespace

namespace atmt {

// Called by the dev reload right before a newer build takes over (dev_reload.cpp): this build stops
// writing, so the two builds cannot interleave in atmt_dialogs.jsonl, and the flush thread stops
// waiting on a sink that is closed.
void RetireInstance() {
    InterlockedExchange(&g_stop, 1);
    if (g_sink != nullptr) {
        ReleaseHeldPages(true);   // pages held for the screen still belong in this build's log
        g_sink->Note("dialog logger: handing over to a new build");
        g_sink->Close();
        g_sink = nullptr;
    }
}

}  // namespace atmt

extern "C" {

// ---------------------------------------------------------------- mod interface
// See shared/mod_api.h for the contract. Returning 0 means "do not use me" and the loader
// unloads the dll again.
__declspec(dllexport) uint32_t __cdecl AtmtModInit(const AtmtModApi* api) {
    if (api == nullptr || api->version < 1) return 0;
    if (api->size < sizeof(AtmtModApi)) return 0;      // truncated service table
    // Once per process: with the loader injected a second time the already loaded dll would
    // otherwise be initialised again - two log sinks, two sets of hooks, duplicate lines.
    if (InterlockedCompareExchange(&g_mod_initialised, 1, 0) != 0) {
        if (api->log != nullptr) {
            api->log("mod: already initialised in this process, declining");
        }
        return 0;
    }
    g_api = api;

    atmt::SetHostPaths(api->game_dir, api->mod_dir);
    atmt::SetHostLog(api->log);
    atmt::SetHostLogError(api->log_error);
    atmt::HookApi hooks;
    hooks.create = api->hook_create;
    hooks.enable = api->hook_enable;
    hooks.disable = api->hook_disable;
    hooks.remove = api->hook_remove;
    atmt::SetHookApi(hooks);

    // The work happens on a thread of our own, so the loader's thread stays responsive.
    HANDLE thread = CreateThread(nullptr, 0, InitThread, nullptr, 0, nullptr);
    if (thread == nullptr) return 0;
    CloseHandle(thread);

    return ATMT_MOD_API_VERSION;
}

__declspec(dllexport) void __cdecl AtmtModShutdown(void) {
    atmt::RemovePanel();   // the overlay must not call into this dll once it is gone
    InterlockedExchange(&g_stop, 1);
    if (g_sink != nullptr) {
        g_sink->Note("dialog logger: stopping");
        g_sink->Close();
        g_sink = nullptr;
    }
}

// One sentence for the overlay and the manager (shared/mod_api.h, AtmtModDescription).
__declspec(dllexport) const char* __cdecl AtmtModDescription(void) {
    return "Keeps every line of dialog with its speaker and shows the log over the game (F3).";
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        // Remember our own module: every path is built from it, not from the host's.
        atmt::SetSelfModule(inst);
        DisableThreadLibraryCalls(inst);
    }
    return TRUE;
}

}  // extern "C"

