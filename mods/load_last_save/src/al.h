// al.h - the "load the newest save" mod.
//
// A mod of the loader (see shared/mod_api.h), separate from the dialog logger: it does one
// thing, driven by the command line switch `-al` (and nothing happens without it).
#pragma once

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <cstdint>
#include <string>
#include <vector>

// The loader's service table (shared/mod_api.h). Only the pointer is needed here, and declaring
// it at global scope keeps it the same type the loader passes in.
struct AtmtModApi;

namespace al {
// The switch this mod implements. Exact match, case-insensitive: "-alan" is not it.
inline const wchar_t* const kSwitch = L"-al";

// ---------------------------------------------------------------- settings
// Read from the mod's own ini (<mod_dir>\load_last_save.ini), so the tests can point it at a
// scratch folder and the real save folder is never touched by a test run.
// When a start loads the newest save (Autoload= in the ini; the tokens are what the settings bar
// shows): on every start (the default), only when the game was started with -al, or never.
enum class Autoload { kAlways = 0, kParameter = 1, kDisabled = 2 };
inline const char* const kAutoloadTokens[] = {"always", "parameter", "disabled"};

struct Options {
    bool enabled = true;
    Autoload autoload = Autoload::kAlways;
    bool include_autosaves = true;   // autosaveNN.dat counts as a save
    bool diagnostics = false;        // list every candidate, not just the chosen one
    // Diagnostic: log every save-related file call the game makes (src/probe.cpp), and/or watch
    // the one function that actually reads a save file (src/probe_reader.cpp). Off by default.
    bool probe_file_apis = false;
    bool probe_reader = false;
    // Diagnostic: hardware write breakpoints on the two addresses that carry a load request.
    bool probe_writers = false;
    // Diagnostic: poll the save manager's callback fields (what the load menu installs).
    bool probe_callbacks = false;
    // Diagnostic: log the arguments the game passes to its own save/load request (FUN_00484870).
    // This is how the mod learns what a real load looks like - see src/probe_request.cpp.
    bool probe_request = false;
    // The title route (src/title_load.cpp) - the one "-al" uses. On the game's own thread it starts the
    // save menu's state machine the way the title's "Load" does (phase 2, the title's own completion
    // callback) and advances the save manager's state machine to the chosen slot once the menu has
    // armed it. The game then enters the session itself. If this is off, or its hook fails to install,
    // TriggerLoad() (trigger.cpp) is the fallback: it gets the file read but does not enter the game
    // (it never installs the menu's completion callback - only a real Menu::Open() call does that).
    bool title_load = true;
    // How long to wait after the save subsystem is up before the fallback route (trigger.cpp) requests
    // the load (ms). The title route does not use this - its own settle wait is in title_load.cpp.
    unsigned start_delay_ms = 1500;
    // Diagnostic: log suspicious memset calls (null destination / huge count) with their caller.
    bool probe_memset = false;
    // Diagnostic: which game function opens a save file (hooks the CRT's fopen).
    bool probe_fopen = false;
    // Diagnostic: log the calls the real load menu makes that show up on screen as fades/transitions
    // (src/probe_fade.cpp) - when they fire relative to ArmTitleLoad, and the arguments that look like
    // durations. This is how "can the fades be faster" gets an answer instead of a guess.
    bool probe_fade = false;
    // Test-only input: a pad script (relative to the mod folder) fed through XInputGetState. Empty =
    // no input hooking at all, which is the shipped default. Not used by the title route (it needs no
    // input) - kept as a standalone dev/test tool for driving a menu screen without a human, see
    // src/pad_inject.cpp.
    std::string pad_script;
    std::wstring save_dir;           // empty = the game's own folder (DefaultSaveDir())
};

// Parses a full Windows command line (the string GetCommandLineW returns, quotes and all).
bool HasSwitch(const std::wstring& command_line, const wchar_t* sw);

// Reads the mod's settings file (its own ini; see Options).
Options LoadOptions(const std::wstring& ini_file);

// Whether this start loads the newest save: the Autoload mode and the command line together
// (Enabled=false turns the whole mod off, whatever the mode says).
bool WantsLoad(const Options& options, const std::wstring& command_line);

// The command line of this process, as the game's own process would see it.
std::wstring ProcessCommandLine();

// ---------------------------------------------------------------- the game's save machinery
// Fixed addresses inside ed8.exe (it has no ASLR), all measured in the running game - see
// docs/ENGINE_NOTES.md, "Saving and loading". The name buffer is where the game keeps the slot's file name; the reader
// formats `<save dir>/<name>`, reads and parses it.
inline constexpr uintptr_t kGameSaveNameBuffer = 0x00C3E758;
inline constexpr uintptr_t kGameSaveReader = 0x00485000;
inline constexpr uintptr_t kGameSaveManagerPtr = 0x00C3E750;
// FUN_00484870: thiscall(manager) + 5 stack args, `ret 0x14`. It is the game's own "begin a save
// operation": it resolves the slot, copies the file path into 0x00C3E758, fills the state block
// (0xC3E750/54/5C/60/64) and raises the flag at 0x00C3E868. Writing the path and the flag by hand
// (what this mod did first) gets the file read and nothing more - the state block is what makes the
// game carry on, so this is the call to make.
inline constexpr uintptr_t kGameSaveRequest = 0x00484870;
// The two indirect calls that function makes; both are set by the game at runtime.
inline constexpr uintptr_t kGameSlotLookupPtr = 0x01369FF4;
inline constexpr uintptr_t kGameStrCopyPtr = 0x0136A194;

// ---------------------------------------------------------------- the load's destination buffer
// The game's own load menu does not allocate a buffer: it passes the save module's static buffer
// (measured live on a real menu load: 0x00D339B0, size 0x000708C0 = 460992). That is
// *[0x00C7C598] + 0xB6430, i.e. a member of the object the game's save code holds at 0x00C7C598 (the
// pointer was the same in every session measured, 21.-22.09) - the title route reads this object for
// the UI holder and the save menu (see title_load.cpp's Ctx()/UiHolder()/SaveMenu()).
inline constexpr uintptr_t kGameObjectPtr = 0x00C7C598;
// How long to wait after the save subsystem is up before the fallback route requests the load (ms).
void SetStartDelayMs(unsigned ms);
unsigned StartDelayMs();

// ---------------------------------------------------------------- the game's load *flow*
// FUN_0064CAD0(self, phase): the session module's own "start a load" entry, `__thiscall` + one stack
// argument. Every real load goes through it, title route included (its own Menu::Open() call ends up
// here via the menu's own update) - probe_flow.cpp watches it purely as a diagnostic: it logs the
// first time the game calls it with each phase (0-6), which is how phase 2 was confirmed as the
// title's own value and phase 5 as the game's startup registration.
inline constexpr uintptr_t kGameFlowEntry = 0x0064CAD0;
void SetFlowLog(void* log_function);
// Diagnostic only (installed unconditionally whenever "-al" is used): see src/probe_flow.cpp.
bool InstallFlowHook(const AtmtModApi* api, std::string* why_not);

// The reader spin-sleeps while this byte is zero; the mod waits for it before calling the reader.
inline constexpr uintptr_t kGameSaveReadyFlag = 0x00C3E868;
// How long to wait after the game's save subsystem is up before issuing a load.
inline constexpr unsigned kLoadDelayMs = 1500;

// Set by the reader probe: the original function (via the loader's trampoline) and a signal that
// the game's save subsystem is up, i.e. the game has read a file itself.
void SetSaveReaderTrampoline(void* trampoline);
void SetLoadLog(void* log_function);
void NotifySaveSystemReady();
bool SaveSystemReady();
// The last file name the game read through the reader.
void SetLastGameReadName(const std::string& name);
std::string LastGameReadName();

// ---------------------------------------------------------------- saves on disk
struct SaveFile {
    std::wstring path;
    FILETIME time{};
    unsigned long long size = 0;
};

// True for the game's save files and false for everything else that shares the folder:
// `saveNNN.dat` and `autosaveNN.dat` yes; the `_t` headers, `thumbNNN.bmp`, `sdslot.dat` and
// `steam_autocloud.vdf` no.
bool IsSaveFileName(const wchar_t* name);

// Lists the saves in `dir`, newest first (equal times break by name, so the order is fixed).
std::vector<SaveFile> FindSaves(const std::wstring& dir, bool include_autosaves);

// The game's own save folder: the user's Saved Games\Falcom\ed8.
std::wstring DefaultSaveDir();

// "2026-09-21 07:25" in local time.
std::string FormatTime(const FILETIME& ft);

// ---------------------------------------------------------------- the action
// Asks the game to load `path`. Outside the game this reports why it cannot, and the mod says what it *would* have done.
// Returning false is not an error: outside the game there is nothing to call.
bool TriggerLoad(const std::wstring& path, std::string* why_not);

// "save063.dat" -> 63, for the manager state machine (see trigger.cpp). kNoSlot means "not a slot
// the game's own format can produce" (autosaves, the system file's neighbours, anything else).
inline constexpr unsigned kNoSlot = 0xFFFFFFFFu;
unsigned SaveSlotNumber(const std::string& file_name);

// What the load menu has to be told to load a file: the slot number, and whether it is on the
// autosave tab. The game names the file from these two - "save%03d.dat" (0x00B3D9C0) or, with the
// manager's autosave flag, "%s%sautosave%02d%s.dat" (0x00B3B6D8) - so this is the exact inverse.
// Anything else (the system file save511.dat, headers, thumbnails) gives slot == kNoSlot.
struct SaveTarget {
    unsigned slot = kNoSlot;
    bool autosave = false;
};
SaveTarget ParseSaveTarget(const std::string& file_name);

// ---------------------------------------------------------------- the title route (title_load.cpp)
enum TitleLoadOutcome : long {
    kTitleLoadPending = 0,
    kTitleLoadEntered,       // the menu called the title back with success; the title fades into the game
    kTitleLoadPrompt,        // success, and the title shows its own follow-up prompt (its state 5)
    kTitleLoadFailed,        // the menu closed without success
    kTitleLoadMenuClosed,    // the menu closed before a slot was picked
    kTitleLoadRefused,       // the slot cannot be expressed to the menu
    kTitleLoadTimedOut,      // the title never got that far
};
bool InstallTitleHook(const AtmtModApi* api, std::string* why_not);
bool ArmTitleLoad(const std::string& file_name, std::string* why_not);
void CancelTitleLoad(TitleLoadOutcome outcome);
TitleLoadOutcome TitleLoadResult();
const char* TitleLoadStageName();
long TitleUpdateCalls();
// GetTickCount() at the moment ArmTitleLoad was called, 0 if it never was - the clock every
// probe_fade.cpp log line reports its call against, so "which fade comes first" is a diff, not a guess.
unsigned long TitleLoadStartTick();

// Diagnostic (ProbeFileApis=true): hooks CreateFileW/CreateFileA/ReadFile and logs every
// save-related path with the ed8.exe address that asked for it. This is how the game's save
// reading code is located - see the top of src/probe.cpp.
bool InstallFileProbe(const AtmtModApi* api, std::string* why_not);

// Diagnostic (ProbeReader=true): watches the game's own save-file reader (0x00485000) and logs
// its arguments, the file name it is about to open and the ed8.exe return addresses on the stack.
// That is how the caller of a load - the code the "-al" mod has to imitate - is found.
bool InstallReaderProbe(const AtmtModApi* api, std::string* why_not);

// Diagnostic (ProbeWriters=true): hardware write breakpoints on the two addresses that carry a load
// request (the slot's file name and the request flag), so the code that raises the request reports
// itself as an EIP. This is how the menu's load action is found - see src/probe_writers.cpp.
bool InstallWriterProbe(const AtmtModApi* api, std::string* why_not);

// Diagnostic (ProbeCallbacks=true): poll the save manager's callback fields and report changes. The
// manager's pointer is a global, so this patches nothing - it only reads.
bool InstallCallbackProbe(const AtmtModApi* api, std::string* why_not);

// Diagnostic (ProbeRequest=true): log the arguments of the game's own save/load request
// (FUN_00484870), so the mod can replay a real load with the newest save's path.
bool InstallRequestProbe(const AtmtModApi* api, std::string* why_not);
void* RequestTrampoline();

bool InstallMemsetProbe(const AtmtModApi* api, std::string* why_not);

// Test-only input: hooks XInputGetState and merges a scripted button sequence into the polled state
// (the game ignores injected keystrokes - see src/pad_inject.cpp). Installed only when the script file
// exists, so a normal install never touches input.
bool InstallPadInjector(const AtmtModApi* api, const std::wstring& script_path, std::string* why_not);

// Parsed form of one pad-script line ("1200 A" / "1200,A" / "9000 EXIT"). Pure, so the tests can
// exercise the parser without a game.
bool PadParseEntry(const std::string& text, unsigned* at_ms, unsigned* button, bool* is_exit);

// Diagnostic (ProbeFopen=true): hooks the CRT's fopen entry points, so a menu load reports the game
// function that opens a save file. See src/probe_fopen.cpp.
bool InstallFopenProbe(const AtmtModApi* api, std::string* why_not);

// ---------------------------------------------------------------- the visible fades (probe_fade.cpp)
// Three functions the real load menu's flow entry (FUN_0064CAD0) calls on every load, found by
// resolving the manager's vtable thunks offline (against the exe on disk: no ASLR, so
// the file's bytes are the runtime bytes) and decompiling what they land on:
//   kFadeWidgetKick   FUN_0064A7D0 - loops over ~20 UI widgets, and for each does
//                     vtable[0x84](0,0,0) then vtable[0x84](target, kFadeDurationGlobal, 2): a
//                     read-current-value/reset/animate-to-target pattern, i.e. this is what actually
//                     starts each widget's fade-in. Every call uses the same duration global.
//   kFadeListBuild    FUN_0064B940 - builds the visible save-slot list (thumbnails, date/time text);
//                     large and does file I/O (thumbNNN.bmp), so it is a candidate for the "list flash"
//                     being longer than a frame rather than a fade proper.
//   kFadePrimeSetter  FUN_00453f70 - vtable+0xc on the same object; writes self+0x1c with the 500 (or
//                     400) FUN_0064CAD0 passes for phase 2 (title) vs phase 0/1/6. Logged so a duration
//                     read from the game's own field is on record, not assumed from the decompile.
// kFadeDurationGlobal is .rdata data, not code - a compile-time float literal (measured offline: 0.2).
// It is not a runtime "current" value to poll; the probe logs it once per fade-kick call so a live
// read confirms the file's bytes are still what is running.
inline constexpr uintptr_t kFadeWidgetKick = 0x0064A7D0;
inline constexpr uintptr_t kFadeListBuild = 0x0064B940;
inline constexpr uintptr_t kFadePrimeSetter = 0x00453F70;
inline constexpr uintptr_t kFadeDurationGlobal = 0x00B3A164;
bool InstallFadeProbe(const AtmtModApi* api, std::string* why_not);

// The caller chain a hook can see from a register snapshot (layout documented in probe_reader.cpp).
// Exposed for the tests: an unguarded scan here once read past the end of a worker thread's stack
// and crashed the game, so the guarding is checked against a page boundary.
std::string HookStackFrames(const void* snapshot, unsigned max_frames);

}  // namespace al
