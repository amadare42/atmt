// atmt.h - declarations for the Trails of Cold Steel dialog logger.
//
// One job, one source: the game's own "set the message's run" call (see addr_hook.cpp).
// Text and speaker are the game's own data, so there is no catalog, no watcher and no
// matching helper here - just the hook, the sink, the config and the host services.
#pragma once

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <cstdint>
#include <cstdio>
#include <deque>
#include <string>
#include <utility>
#include <vector>

#include "history.h"   // the lines the overlay panel shows
#include "keys.h"      // the names in "OverlayKey" and the pressed-once edge

struct AtmtModApi;      // shared/mod_api.h: the loader's service table

namespace atmt {

// ---------------------------------------------------------------- config
// The panel's text size is clamped to this range wherever it comes from (the ini, and the
// Ctrl+Up/Ctrl+Down that changes it while the panel is open): below it the log cannot be read, above
// it a single line fills the sheet. Kept here so the two places cannot drift apart.
constexpr unsigned kOverlayFontSizeMin = 8;
constexpr unsigned kOverlayFontSizeMax = 128;

struct Config {
    bool enabled = true;                 // master switch
    // With this off the mod writes *no* files at all: no atmt_dialogs.jsonl, no atmt_latest.txt and no
    // diagnostics. The lines still exist in memory, so the overlay panel shows exactly what it would
    // have shown (see logsink.cpp) - and the loader's own log, which the mod does not own, is written
    // according to the loader's own LogLevel. Off by default: nothing is left on disk unless asked for.
    bool log_to_file = false;
    // Diagnostics: one line per message in atmt_diagnostics.log, and a report whenever the
    // hook runs but the bytes it was handed are not a readable run - that is how a missed
    // line announces itself instead of just not appearing in the log.
    bool diagnostics = false;
    // The game's own message setter (0x00701E50 in the shipped ed8.exe): thiscall, ecx = the
    // message state (the name plate is a field of it), argument = the run pointer, whose text
    // is `11 <u32>` + the bytes (so it starts 5 bytes in) - or the run itself, which the hook
    // decides from the byte at the pointer.
    uintptr_t message_hook_address = 0x701E50;
    unsigned message_plate_offset = 0x358;
    unsigned message_text_skip = 5;
    // A line whose plate has not been written yet waits this long; the plate is then re-read
    // from the same object (the game sets it a moment after the line).
    unsigned pending_speaker_ms = 700;
    // ---------------------------------------------------------------- the dialog panel (F3)
    // The panel is a view of what this log already has, drawn over the game by the overlay mod
    // (mods/overlay, atmt_overlay.dll). It changes nothing about the capture: with it off the mod
    // behaves exactly as before. While it is open the game's own input is held (OverlayCapture), so
    // reading the log cannot advance the dialog.
    bool overlay = true;                          // master switch
    std::string overlay_key = "F3";               // the key (or chord, "Ctrl+F3") that opens and closes it
    unsigned overlay_key_chord = kDefaultOverlayKey; // resolved from the name (atmt::KeyChordFromName)
    bool overlay_key_known = true;                // false when OverlayKey named something unknown
    unsigned overlay_lines = 500;                 // how far back the panel can scroll
    bool overlay_newest_first = false;             // draw the newest line first (a tail) instead of last
    bool overlay_capture = true;                  // while open, swallow the input the panel uses
    bool overlay_timestamps = true;                // show when each line was logged
    bool overlay_game_font = true;                 // draw the log with the game's own font (itf_font.h)
    std::string overlay_anchor = "BottomLeft";    // TopLeft/TopRight/Center/BottomLeft/BottomRight
    int overlay_width_pct = 62;                   // percent of the game's client area
    int overlay_height_pct = 48;
    unsigned overlay_opacity_pct = 92;            // panel background, percent
    unsigned overlay_dim_pct = 30;                // dim the screen behind the panel, percent (0 = no)
    unsigned overlay_font_size = 20;              // pixels of the game's client area (see the limit below)
    std::string overlay_pad_close = "B";          // gamepad button that closes the panel
    std::string overlay_pad_toggle;               // gamepad button that opens/closes; empty = keyboard
    // Dev only, off by default: let a running game hand itself over to a freshly built dll
    // (<game>\atmt_reload.txt names it). See docs/OVERLAY.md - the loop that avoids reloading a save.
    bool dev_reload = false;
    // Keys the mod did not recognise. Reported once at startup, so a stale or misspelt setting
    // cannot sit in the file doing nothing (a config that was silently ignored cost hours once).
    std::string unknown_keys;
    // Keys that belong to the overlay mod now (atmt_overlay.ini): the mouse, the font file and the
    // settings bar's keys. Reported once, so an old line is not mistaken for one that still works.
    std::string moved_keys;
    // Keys of this mod outside their section ([General] or [Overlay]): not read, and reported - the
    // loader's settings registry only looks in a key's own section, so the two must agree.
    std::string misplaced_keys;
};

// Reads an ini file (keys are documented in dist/atmt_config.ini).
Config LoadConfig(const std::wstring& ini_file);

// ---------------------------------------------------------------- host services
void SetHostPaths(const wchar_t* game_dir, const wchar_t* mod_dir);
void SetHostLog(void (*fn)(const char* text));
void HostLog(const char* message);   // no-op when no loader is attached
void SetHostLogError(void (*fn)(const char* text));
void HostLogError(const char* message);   // a failure: logged unless LogLevel=off (HostLog: LogLevel=all)
std::wstring GetGameDir();           // directory of the host process (ed8.exe)
std::wstring GetSelfDir();           // directory of this dll
void SetSelfModule(void* module);    // called from DllMain
void* SelfModuleHandle();

// ---------------------------------------------------------------- text
// Removes the inline `#`-codes and flattens newlines: what the player reads.
std::string StripCodes(const std::string& in);
// True when the address is committed, executable memory. Hooks are only ever installed on
// code, so a stale setting cannot patch data.
bool IsExecutableCode(const void* p);

// ---------------------------------------------------------------- one line
// The bytes as the game stores them (`raw`) and the readable form (`text`). The speaker is not
// part of this: it comes from the message state object, not the run, and is passed to the sink
// separately (see LogSink::LineFromObject).
struct CatalogEntry {
    std::string raw;
    std::string text;
};

// ---------------------------------------------------------------- logging
class LogSink {
public:
    // Tell the sink where its files would go and whether it may write them at all (the LogToFile
    // setting). With write_files false, Open() touches nothing on disk and the lines are kept in
    // memory only - which is all the overlay panel needs.
    void set_write_files(bool on) { write_files_ = on; }
    bool Open(const std::wstring& dir);   // atmt_dialogs.jsonl + atmt_latest.txt
    // Writes one line, with the speaker exactly as given.
    void Line(const CatalogEntry& e, uintptr_t address, const std::string& speaker,
              const char* source);
    // Writes one line whose speaker is the game's own name plate: read from the message
    // state (obj + plate_offset) now, and once more when the wait is over if it is not set
    // yet. That plate is the name the game draws - it is not a guess.
    void LineFromObject(const CatalogEntry& e, uintptr_t obj, unsigned plate_offset,
                        uintptr_t address, const char* source);
    void FlushPending();                  // on the flush thread
    void set_pending_ms(unsigned ms) { pending_ms_ = ms; }
    // The same lines, recorded for the overlay panel as they are written. Set once at startup;
    // null (as in the offline self test) simply means the panel has nothing to show.
    void set_history(History* history) { history_ = history; }
    void Note(const std::string& message);
    // Diagnostics go to their own file (atmt_diagnostics.log), so the dialog log stays clean.
    // The hook decides when to write one (its own copy of the settings carries the switch),
    // so there is exactly one place that can get this wrong.
    void Diagnostic(const std::string& message);
    uint64_t count() const { return count_; }
    void Close();

private:
    static constexpr size_t kMaxLatestLines = 40;
    // LogToFile=false: no file is ever opened, so jsonl_/latest_path_/log_dir_ stay empty and every
    // write below is skipped - but the sink is "open" all the same, because the lines still have to
    // reach the history the panel reads. ready_ is what the early-outs check now; the file writes
    // check the handles themselves.
    bool ready_ = false;
    bool write_files_ = true;
    FILE* jsonl_ = nullptr;
    std::wstring latest_path_;
    std::deque<std::string> latest_lines_;
    CRITICAL_SECTION lock_{};
    uint64_t count_ = 0;
    History* history_ = nullptr;  // the same lines for the overlay panel (null = no panel)
    std::wstring log_dir_;      // where atmt_diagnostics.log goes

    struct PendingLine {
        CatalogEntry entry;
        uintptr_t address = 0;
        std::string speaker;
        std::string source;
        DWORD deadline = 0;
        uintptr_t obj = 0;        // the plate is re-read from here
        unsigned plate_offset = 0;
    };
    static constexpr size_t kMaxPending = 32;
    PendingLine pending_[kMaxPending];
    size_t pending_count_ = 0;
    CRITICAL_SECTION pending_lock_{};
    unsigned pending_ms_ = 700;
};

// ---------------------------------------------------------------- hooks
// Hooking goes through the loader: one MinHook instance per process, and one place that
// refuses addresses that are not executable code.
struct HookApi {
    void* (*create)(void* target, void* detour, void** trampoline) = nullptr;
    int (*enable)(void* handle) = nullptr;
    int (*disable)(void* handle) = nullptr;
    int (*remove)(void* handle) = nullptr;
};
void SetHookApi(const HookApi& api);
HookApi& Hooks();
bool InstallAddressHooks(const Config& config, LogSink& sink);
// The settings the hook actually uses. They are a copy made by InstallAddressHooks, because
// the Config it is handed is usually a local (that was a real crash: the pointer outlived
// the thread it came from and the first message faulted dereferencing it).
const Config& HookConfig();
// A message's pages after the first are held until the game shows them (addr_hook.cpp, HeldPage):
// this logs the ones it has reached - or, with `all`, everything still held (shutdown, handover).
// Polled from the flush thread.
void ReleaseHeldPages(bool all);

// ---------------------------------------------------------------- the dialog panel (panel_client.cpp)
// The panel is a window in the overlay mod's overlay (shared/overlay_api.h): this registers its
// settings with the loader (so the settings bar shows them and a live change is saved into this
// mod's ini) and, once the overlay's service is there, adds the window. Returns false with a reason
// in the log when there is no overlay - the logger itself keeps working either way. `api` NULL (the
// offline self test) means neither.
bool InstallPanel(const Config& config, LogSink& sink, History& history, const AtmtModApi* api);
// Takes the window out of the overlay (waiting for a frame in progress) and drops the registered
// settings (saving what is unsaved): before this dll goes away - a dev reload, a shutdown.
void RemovePanel();

// ---------------------------------------------------------------- dev reload (dev_reload.cpp)
// Watches <game>\atmt_reload.txt for the path of a freshly built dll and hands the live game over
// to it - the loop that saves reloading a save on every iteration (docs/OVERLAY.md). Started only
// when DevReload=true, which is off by default: it is a development tool, not a feature.
void StartDevReloadWatcher(const Config& config, const AtmtModApi* api);
// The handover, in order: the panel leaves the overlay (RemovePanel), then RetireInstance stops this
// build writing to the log, then the message hook is released, so the build that takes over can
// install its own (MinHook refuses a second hook on the same address). RetireInstance lives in
// mod_entry.cpp; the others are next to what they own.
void RetireInstance();
bool UninstallAddressHooks();

}  // namespace atmt
