// auto_advance.cpp - advances dialog on its own once a page has been up long enough to read, and
// never before its voice line has finished; a small pie shows how much of the wait is left.
//
// How the game waits for a key after a message (docs/ENGINE_NOTES.md, "Dialog"): the text opcode
// sets the box's run and opens it, then a script "wait for a key" handler runs every frame until it
// returns 1. There are two of them - 0x00656EB0 (wait, then close the boxes) and 0x00657190 (wait,
// keep them) - and both ask the button query `FUN_00446240(ctx, 1, 1, 1)` ("confirm pressed, consume
// it") first. A press while the text still types finishes the typing; a press once the page is fully
// shown turns the page or ends the message. So auto-advance is exactly a confirm press, given to
// these two handlers only: their two `call`s of the query (through its thunk 0x0040DB7A) are pointed
// at WaitKeyQuery, which asks the real query and answers "pressed" when the page's time is up. Every
// other reader of the confirm button (menus, choices, battles) is untouched. The query itself is not
// hooked: battle_load hooks it, and one MinHook target takes one hook; calling the thunk keeps
// battle_load's filter in the chain.
//
// What the boxes show (all on the game's thread, in WaitKeyQuery): holder = [[0x00C7C598]+0x7BC];
// the message box at holder+0x19A4 and four speech bubbles at holder+0x1D48 + i*0x9C0, one class
// (PopupMsgWindow's base):
//   +0x6C   flags: open = 8 and 0x10 set, 2 and 4 clear (FUN_00701950)
//   +0x32C  the run; +0x330 how far it is typed; +0x334 where the page on screen starts
//   +0x340  typing progress: 10000.0 = the message is fully shown, 20000.0 = a page break is waiting
//   +0x40/+0x44 size, +0x48/+0x4C position, in the 1280x720 UI canvas
// The page's characters are the run's bytes from +0x334 to +0x330 (pacing.h).
//
// Voices: PlayVoice `FUN_005EA6B0(ctx, voice, character, ...)` is hooked to note each voice line as
// it starts (the game's own per-character table at ctx+0xE0C skips lines without a speaker); its
// length comes from the WAV's header (the path is the game's own, FUN_005E8780), and whether it still
// plays from the game (FUN_005E8620(ctx, voice), its "is playing"). A line is waited for until the
// game says it has stopped, whatever its header said.
//
// The pie and the toggle key are a window in the overlay (shared/overlay_api.h). The pie sits on the
// box's bottom edge, just right of the game's own advance prompt (whose place the game computes from the
// box's canvas rectangle and its text insets, FUN_007020C0); the canvas is mapped to the screen the way the game draws its UI -
// scaled by min(W/1280, H/720) and centred, which is also what deckscreen's letterbox does - or, when
// deckscreen is there with its letterbox switched off, by H/720 (the game's own stretch). deckscreen
// is not needed: its settings are read from the loader's registry when it has registered them.
#include "pacing.h"
#include "wav.h"

#include "atmt_input.h"
#include "atmt_theme.h"
#include "code_check.h"
#include "imgui.h"
#include "mod_api.h"
#include "overlay_api.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <unordered_map>

namespace {

const AtmtModApi* g_api = nullptr;
const AtmtOverlayApi* g_overlay = nullptr;
volatile LONG g_stop = 0;
volatile LONG g_window_added = 0;

void LogV(void(__cdecl* sink)(const char*), const char* fmt, va_list args) {
    if (sink == nullptr) return;
    char buf[512];
    _vsnprintf(buf, sizeof(buf) - 1, fmt, args);
    buf[sizeof(buf) - 1] = '\0';
    sink(buf);
}
void Log(const char* fmt, ...) {
    if (g_api == nullptr) return;
    va_list args;
    va_start(args, fmt);
    LogV(g_api->log, fmt, args);
    va_end(args);
}
void LogError(const char* fmt, ...) {
    if (g_api == nullptr) return;
    va_list args;
    va_start(args, fmt);
    LogV(g_api->log_error, fmt, args);
    va_end(args);
}

double NowMs() {
    static LARGE_INTEGER freq = {};
    if (freq.QuadPart == 0) QueryPerformanceFrequency(&freq);
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return static_cast<double>(t.QuadPart) * 1000.0 / static_cast<double>(freq.QuadPart);
}

// ---------------------------------------------------------------- the game (ed8.exe EN, no ASLR)
typedef uint32_t(__attribute__((thiscall)) * ButtonQueryFn)(void* ctx, int button, int mode, int flag);
typedef int(__attribute__((thiscall)) * PlayVoiceFn)(void* ctx, int voice, int character, int a3, int a4,
                                                     int a5, int a6, int a7, int a8, int a9, int a10);
typedef uint32_t(__attribute__((thiscall)) * VoicePlayingFn)(void* ctx, int voice);
typedef const char*(__attribute__((thiscall)) * VoicePathFn)(void* ctx, int voice);

constexpr uintptr_t kGameCtxPtr = 0x00C7C598;      // g_ctx
constexpr uintptr_t kButtonQueryThunk = 0x0040DB7A;   // jmp FUN_00446240(ctx, button, mode, flag), ret 0xC
constexpr uintptr_t kWaitCloseCall = 0x00656F2F;   // FUN_00656EB0: wait for a key, then close the boxes
constexpr uintptr_t kWaitKeepCall = 0x00657201;    // FUN_00657190: wait for a key, keep them
constexpr uintptr_t kPlayVoice = 0x005EA6B0;       // (ctx, voice, character, 8 more), ret 0x28; 0 = playing
constexpr uintptr_t kVoicePlaying = 0x005E8620;    // (ctx, voice) -> al = 1 while it plays, ret 4
constexpr uintptr_t kVoicePath = 0x005E8780;       // (ctx, voice) -> "data/voice/wav/<name>.wav", ret 4

constexpr unsigned kCtxHolder = 0x7BC;
constexpr unsigned kHolderMainBox = 0x19A4;
constexpr unsigned kHolderBubbles = 0x1D48;
constexpr unsigned kBubbleStride = 0x9C0;
constexpr unsigned kBubbleCount = 4;
constexpr unsigned kHolderSaveMenu = 0x5A80;       // the save menu; [menu+4] != 0 while it is open

constexpr unsigned kBoxFlags = 0x6C;
constexpr unsigned kBoxRun = 0x32C;
constexpr unsigned kBoxTyped = 0x330;
constexpr unsigned kBoxPageStart = 0x334;
constexpr unsigned kBoxProgress = 0x340;
constexpr unsigned kBoxW = 0x40, kBoxH = 0x44, kBoxX = 0x48, kBoxY = 0x4C;
constexpr unsigned kBoxPadRight = 0xA0, kBoxPadBottom = 0xA4;   // the text's inset from the box's edges
// The game's own "advance" prompt (FUN_007020C0, vtable +0x34): a 32x32 icon centred at
// (x + w - pad_right + 4, y + h - pad_bottom) - the box's bottom edge, right of the text.
constexpr float kPromptShiftX = 4.0f;
constexpr float kPromptHalf = 16.0f;
constexpr float kProgressDone = 10000.0f;
constexpr float kProgressPageBreak = 20000.0f;

// The bytes the two call sites hold in the supported exe (call rel32 to the thunk).
constexpr uint8_t kWaitCloseOriginal[5] = {0xe8, 0x46, 0x6c, 0xdb, 0xff};
constexpr uint8_t kWaitKeepOriginal[5] = {0xe8, 0x74, 0x69, 0xdb, 0xff};

template <typename T>
T At(uintptr_t base, unsigned offset) {
    return *reinterpret_cast<const volatile T*>(base + offset);
}

// ---------------------------------------------------------------- settings
const char kMenu[] = "Auto-advance";
const char* const kPositions[] = {"Box", "TopRight", "TopLeft", "BottomRight", "BottomLeft"};
enum Position { kAtBox = 0, kTopRight, kTopLeft, kBottomRight, kBottomLeft };

int32_t g_enabled = 1;
int32_t g_ms_per_char = 20;
float g_base_seconds = 0.25f;
int32_t g_min_ms = 0;
int32_t g_wait_voice = 1;
int32_t g_start_typing = 0;
int32_t g_after_voice_ms = 500;
int32_t g_show_pie = 1;
int32_t g_pie_size = 16;
int32_t g_position = kAtBox;
int32_t g_opacity = 75;
char g_key_name[32] = "F7";
char g_pad_name[32] = "RB+R3";
char g_hold_key_name[32] = "E";
char g_hold_pad_name[32] = "Y";
int32_t g_diagnostics = 0;

atmt::KeyEdge g_key;
unsigned short g_pad_chord = 0;
bool g_pad_was_down = false;
atmt::KeyEdge g_hold_key;
unsigned short g_hold_pad_chord = 0;
bool g_hold_pad_was_down = false;

void __cdecl OnInputChanged(const AtmtSetting*, void*) {
    const unsigned chord = atmt::KeyChordFromName(g_key_name);
    if (g_key_name[0] != '\0' && chord == 0) {
        LogError("auto_advance: ToggleKey=%s is not a key this mod knows - no key", g_key_name);
    }
    if (chord != g_key.key()) {
        g_key.set_key(chord);
        g_key.Reset();
    }
    g_pad_chord = atmt::PadChordFromName(g_pad_name);
    if (g_pad_name[0] != '\0' && g_pad_chord == 0) {
        LogError("auto_advance: TogglePad=%s is not a pad chord - no pad button", g_pad_name);
    }
    const unsigned hold = atmt::KeyChordFromName(g_hold_key_name);
    if (g_hold_key_name[0] != '\0' && hold == 0) {
        LogError("auto_advance: HoldKey=%s is not a key this mod knows - no key", g_hold_key_name);
    }
    if (hold != g_hold_key.key()) {
        g_hold_key.set_key(hold);
        g_hold_key.Reset();
    }
    g_hold_pad_chord = atmt::PadChordFromName(g_hold_pad_name);
    if (g_hold_pad_name[0] != '\0' && g_hold_pad_chord == 0) {
        LogError("auto_advance: HoldPad=%s is not a pad chord - no pad button", g_hold_pad_name);
    }
}

const AtmtSetting kSettings[] = {
    {ATMT_SETTING_BOOL, ATMT_SETTING_LIVE, "General", "Enabled", "Auto-advance dialog",
     "turn the page by itself once it has been up long enough (also ToggleKey / TogglePad)", &g_enabled},
    {ATMT_SETTING_INT, ATMT_SETTING_LIVE, "General", "MsPerChar", "Time per character",
     "milliseconds of reading time for each character on the page (spaces not counted)",
     &g_ms_per_char, 0, 10.0f, 300.0f, 1.0f},
    {ATMT_SETTING_FLOAT, ATMT_SETTING_LIVE, "General", "BaseSeconds", "Time per page",
     "seconds every page stays up on top of its characters' time", &g_base_seconds, 0, 0.0f, 10.0f,
     0.1f},
    {ATMT_SETTING_INT, ATMT_SETTING_LIVE, "General", "MinMs", "Minimum time",
     "the least milliseconds a page stays up, however short it is (0 = no minimum)", &g_min_ms, 0, 0.0f,
     10000.0f, 10.0f},
    {ATMT_SETTING_BOOL, ATMT_SETTING_LIVE, "General", "StartWhileTyping", "Start while typing",
     "start the countdown when a page starts typing (the text is read as it appears) instead of\n"
     "when it is fully shown; the page still turns only once all of it is shown",
     &g_start_typing},
    {ATMT_SETTING_BOOL, ATMT_SETTING_LIVE, "General", "WaitForVoice", "Wait for the voice",
     "never turn the page while its voice line still plays", &g_wait_voice},
    {ATMT_SETTING_INT, ATMT_SETTING_LIVE, "General", "AfterVoiceMs", "Pause after the voice",
     "milliseconds to wait after a voice line ends", &g_after_voice_ms, 0, 0.0f, 3000.0f, 100.0f},
    {ATMT_SETTING_LABEL, 0, nullptr, nullptr, "Timer"},
    {ATMT_SETTING_BOOL, ATMT_SETTING_LIVE, "General", "ShowPie", "Show the timer",
     "a small pie by the dialog box with the time left before the page turns", &g_show_pie},
    {ATMT_SETTING_INT, ATMT_SETTING_LIVE, "General", "PieSize", "Timer size",
     "the pie's diameter in pixels at 720p (it scales with the screen)", &g_pie_size, 0, 10.0f, 80.0f,
     2.0f},
    {ATMT_SETTING_ENUM, ATMT_SETTING_LIVE, "General", "PiePosition", "Timer position",
     "Box = on the dialog box, right of its advance prompt; or a corner of the screen", &g_position, 0, 0, 0, 0,
     kPositions, 5},
    {ATMT_SETTING_INT, ATMT_SETTING_LIVE, "General", "OpacityPct", "Timer opacity",
     "the pie's background, percent", &g_opacity, 0, 0.0f, 100.0f, 5.0f},
    {ATMT_SETTING_LABEL, 0, nullptr, nullptr, "Controls"},
    {ATMT_SETTING_KEY, ATMT_SETTING_LIVE, "General", "ToggleKey", "Toggle key",
     "the key (or chord) that switches auto-advance on and off (none = no key)", g_key_name,
     sizeof(g_key_name), 0, 0, 0, nullptr, 0, &OnInputChanged},
    {ATMT_SETTING_PAD_BUTTON, ATMT_SETTING_LIVE, "General", "TogglePad", "Toggle pad chord",
     "a pad button or chord that switches auto-advance on and off (none = keyboard only)", g_pad_name,
     sizeof(g_pad_name), 0, 0, 0, nullptr, 0, &OnInputChanged},
    {ATMT_SETTING_KEY, ATMT_SETTING_LIVE, "General", "HoldKey", "Hold-this-line key",
     "the key (or chord) that holds the dialog line on screen: no auto-advance until it is pressed\n"
     "again or the line is gone (none = no key)",
     g_hold_key_name, sizeof(g_hold_key_name), 0, 0, 0, nullptr, 0, &OnInputChanged},
    {ATMT_SETTING_PAD_BUTTON, ATMT_SETTING_LIVE, "General", "HoldPad", "Hold-this-line pad button",
     "a pad button or chord that holds the dialog line on screen, as HoldKey (none = keyboard only)",
     g_hold_pad_name, sizeof(g_hold_pad_name), 0, 0, 0, nullptr, 0, &OnInputChanged},
    {ATMT_SETTING_BOOL, ATMT_SETTING_ADVANCED | ATMT_SETTING_LIVE, "General", "Diagnostics",
     "Diagnostics",
     "log every page (characters, reading time, voice) and every press (atmt_loader.log, LogLevel=all)",
     &g_diagnostics},
};

// ---------------------------------------------------------------- what the render thread draws
// Written on the game's thread at each poll of the wait handlers, read on the render thread.
struct PieState {
    double updated_ms = 0.0;   // 0 = never; stale after kPieStaleMs (the wait is over)
    float left = 0.0f;
    bool voice = false;        // the reading time is up; the voice holds the page
    bool held = false;         // the player holds the page (HoldKey / HoldPad)
    bool have_box = false;
    float prompt[2] = {};      // the centre of the box's advance prompt, in the 1280x720 canvas
};
constexpr double kPieStaleMs = 150.0;
CRITICAL_SECTION g_pie_lock;
PieState g_pie;

// Set by the overlay's update: someone (the settings bar, the dialog log) holds the input.
volatile LONG g_input_held = 0;

// The hold key, pressed (GetTickCount() of the press; 0 = none): read on the render thread, applied
// on the game's thread at its next poll - which can be a few hundred ms away while a box opens. A press
// nobody polls for within kHoldFreshMs (no dialog up) is dropped.
volatile LONG g_hold_pressed_at = 0;
constexpr DWORD kHoldFreshMs = 1000;

// A line of feedback at the top of the screen (the on/off toggle). The hold has none: the pie shows it.
const char* volatile g_toast_text = nullptr;
volatile LONG g_toast_until = 0;   // GetTickCount()
constexpr DWORD kToastMs = 1500;

void Toast(const char* text) {
    g_toast_text = text;
    InterlockedExchange(&g_toast_until, static_cast<LONG>(GetTickCount() + kToastMs));
}

bool ToastShowing(DWORD now) {
    const DWORD until = static_cast<DWORD>(InterlockedCompareExchange(&g_toast_until, 0, 0));
    return g_toast_text != nullptr && static_cast<LONG>(until - now) > 0;
}

// ---------------------------------------------------------------- voices (game thread)
struct Voice {
    int id = 0;
    double start_ms = 0.0;
    double length_ms = 0.0;   // 0 = unknown
};
constexpr int kVoiceSlots = 8;
constexpr double kVoiceCapMs = 60000.0;   // the longest a page waits for a voice that never ends
CRITICAL_SECTION g_voice_lock;
Voice g_voices[kVoiceSlots];
int g_voice_next = 0;
std::unordered_map<int, double>* g_lengths = nullptr;   // voice -> length (ms), 0 = unreadable

PlayVoiceFn g_play_voice = nullptr;
void* g_play_voice_hook = nullptr;

double VoiceLength(void* ctx, int id) {
    auto known = g_lengths->find(id);
    if (known != g_lengths->end()) return known->second;
    double length = 0.0;
    const char* path = reinterpret_cast<VoicePathFn>(kVoicePath)(ctx, id);
    if (path != nullptr && path[0] != '\0') {
        std::wstring full(g_api->game_dir);
        full += L'\\';
        for (const char* p = path; *p != '\0'; ++p) full += static_cast<wchar_t>(*p == '/' ? '\\' : *p);
        HANDLE file = CreateFileW(full.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                                  OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file != INVALID_HANDLE_VALUE) {
            uint8_t head[256];
            DWORD read = 0;
            if (ReadFile(file, head, sizeof(head), &read, nullptr) != 0) {
                length = auto_advance::WavDurationMs(head, read);
            }
            CloseHandle(file);
        }
        if (g_diagnostics != 0) Log("auto_advance: voice %d is %s, %.0f ms", id, path, length);
    }
    (*g_lengths)[id] = length;   // a SenPatcher archive's voice has no file here: "unknown" from now on
    return length;
}

int __attribute__((thiscall)) PlayVoiceDetour(void* ctx, int voice, int character, int a3, int a4, int a5,
                                             int a6, int a7, int a8, int a9, int a10) {
    const int result = g_play_voice(ctx, voice, character, a3, a4, a5, a6, a7, a8, a9, a10);
    const int id = static_cast<short>(voice);
    if (result == 0 && id != 0) {
        EnterCriticalSection(&g_voice_lock);
        Voice& v = g_voices[g_voice_next];
        g_voice_next = (g_voice_next + 1) % kVoiceSlots;
        v.id = id;
        v.start_ms = NowMs();
        v.length_ms = VoiceLength(ctx, id);
        LeaveCriticalSection(&g_voice_lock);
    }
    return result;
}

// Is a voice line playing, and when should the longest one end (0 = unknown)?
bool VoicePlaying(void* ctx, double now, double* end_ms) {
    *end_ms = 0.0;
    bool playing = false;
    bool unknown = false;
    EnterCriticalSection(&g_voice_lock);
    for (Voice& v : g_voices) {
        if (v.id == 0) continue;
        if (now - v.start_ms > kVoiceCapMs ||
            (reinterpret_cast<VoicePlayingFn>(kVoicePlaying)(ctx, v.id) & 0xFF) == 0) {
            v.id = 0;
            continue;
        }
        playing = true;
        if (v.length_ms <= 0.0) unknown = true;
        else if (v.start_ms + v.length_ms > *end_ms) *end_ms = v.start_ms + v.length_ms;
    }
    LeaveCriticalSection(&g_voice_lock);
    if (unknown) *end_ms = 0.0;
    return playing;
}

// ---------------------------------------------------------------- the wait (game thread)
auto_advance::Pacer g_pacer;
uint32_t g_logged_page = 0;

uint32_t Fnv(uint32_t h, uint32_t v) {
    for (int i = 0; i < 4; ++i) {
        h ^= (v >> (i * 8)) & 0xFFu;
        h *= 16777619u;
    }
    return h;
}

bool BoxOpen(uintptr_t box) {
    const uint32_t flags = At<uint32_t>(box, kBoxFlags);
    return (flags & 4u) == 0 && (flags & 2u) == 0 && (flags & 0x10u) != 0 && (flags & 8u) != 0;
}

struct Page {
    uint32_t key = 0;        // 0 = no page to count (none open, or one still types and `while_typing` is off)
    bool ready = false;      // every open box shows its whole page
    unsigned chars = 0;
    uintptr_t first_box = 0;
};

// The characters of a page that still types: from its start to its end (`02`), read in as large a
// piece as is readable - the run sits in the script's buffer, rarely near the end of its region.
unsigned TypingPageChars(uintptr_t text) {
    for (size_t reach : {size_t(4096), size_t(1024), size_t(256)}) {
        if (!atmt_code::Readable(text, reach)) continue;
        const uint8_t* p = reinterpret_cast<const uint8_t*>(text);
        return auto_advance::CountPageChars(p, auto_advance::PageEnd(p, reach));
    }
    return 0;
}

// The page(s) the open boxes show and their characters together. A page is known by its box, run and
// start, so the same page keeps its key from its first typed character to the press that turns it.
Page CurrentPage(uintptr_t holder, bool while_typing) {
    Page page;
    page.ready = true;
    uint32_t key = 2166136261u;
    bool any = false;
    for (unsigned i = 0; i <= kBubbleCount; ++i) {
        const uintptr_t box = i == 0 ? holder + kHolderMainBox : holder + kHolderBubbles + (i - 1) * kBubbleStride;
        if (!BoxOpen(box)) continue;
        const float progress = At<float>(box, kBoxProgress);
        const bool shown = progress == kProgressDone || progress == kProgressPageBreak;
        if (!shown && !while_typing) return Page();
        const uintptr_t run = At<uint32_t>(box, kBoxRun);
        const uint32_t typed = At<uint32_t>(box, kBoxTyped);
        const uint32_t start = At<uint32_t>(box, kBoxPageStart);
        if (run < 0x10000) continue;
        if (!shown) {
            page.ready = false;
            page.chars += TypingPageChars(run + start);
        } else if (typed > start && typed - start < 0x10000 && atmt_code::Readable(run + start, typed - start)) {
            page.chars += auto_advance::CountPageChars(reinterpret_cast<const uint8_t*>(run + start),
                                                       typed - start);
        }
        key = Fnv(Fnv(Fnv(key, static_cast<uint32_t>(box)), static_cast<uint32_t>(run)), start);
        if (!any) page.first_box = box;
        any = true;
    }
    if (any) page.key = key != 0 ? key : 1;
    else page.ready = false;
    return page;
}

void PublishPie(const auto_advance::Pacer::Output& out, uintptr_t box, double now) {
    EnterCriticalSection(&g_pie_lock);
    if (!out.counting) {
        g_pie.updated_ms = 0.0;
    } else {
        g_pie.updated_ms = now;
        g_pie.left = out.left;
        g_pie.voice = out.waiting_for_voice;
        g_pie.held = out.held;
        g_pie.have_box = box != 0;
        if (box != 0) {
            g_pie.prompt[0] = At<float>(box, kBoxX) + At<float>(box, kBoxW) - At<float>(box, kBoxPadRight) +
                              kPromptShiftX;
            g_pie.prompt[1] = At<float>(box, kBoxY) + At<float>(box, kBoxH) - At<float>(box, kBoxPadBottom);
        }
    }
    LeaveCriticalSection(&g_pie_lock);
}

// Should the wait handler see a confirm press it did not get from the player?
bool AutoPress(void* ctx_ptr) {
    const double now = NowMs();
    const uintptr_t ctx = reinterpret_cast<uintptr_t>(ctx_ptr);
    auto_advance::Pacer::Input in;
    in.now_ms = now;
    const uintptr_t holder = ctx >= 0x10000 ? At<uint32_t>(ctx, kCtxHolder) : 0;
    Page page;
    if (g_enabled != 0 && holder >= 0x10000) page = CurrentPage(holder, g_start_typing != 0);
    in.page = page.key;
    in.ready = page.ready;
    in.reading_ms = auto_advance::ReadingMs(page.chars, g_base_seconds * 1000.0, g_ms_per_char,
                                            g_min_ms);
    in.after_voice_ms = g_after_voice_ms;
    in.voice_cap_ms = kVoiceCapMs;
    if (page.key != 0 && g_wait_voice != 0) in.voice_playing = VoicePlaying(ctx_ptr, now, &in.voice_end_ms);
    // Someone else has the input: the overlay's bar or a window, or battle_load's save menu over the event.
    const uintptr_t save_menu = holder >= 0x10000 ? At<uint32_t>(holder, kHolderSaveMenu) : 0;
    in.paused = InterlockedCompareExchange(&g_input_held, 0, 0) != 0 ||
                (save_menu >= 0x10000 && At<uint32_t>(save_menu, 4) != 0);

    const LONG hold_at = InterlockedExchange(&g_hold_pressed_at, 0);
    in.toggle_hold = g_enabled != 0 &&
                     auto_advance::PressFresh(static_cast<uint32_t>(hold_at), GetTickCount(), kHoldFreshMs);

    const auto_advance::Pacer::Output out = g_pacer.Step(in);
    if (in.toggle_hold && g_diagnostics != 0) {
        Log("auto_advance: hold %s", out.held ? "on" : out.hold_pending ? "on (the page about to be shown)" : "off");
    }
    if (out.hold_dropped && g_diagnostics != 0) {
        Log("auto_advance: the held page went away (%s)", page.key != 0 ? "another page" : "no page");
    }
    PublishPie(out, page.first_box, now);
    if (g_diagnostics != 0 && page.key != 0 && page.key != g_logged_page) {
        g_logged_page = page.key;
        Log("auto_advance: page %08x %s: %u characters, %.0f ms to read%s", page.key,
            page.ready ? "shown" : "typing", page.chars, in.reading_ms,
            in.voice_playing ? ", a voice is playing" : "");
    }
    if (out.press && g_diagnostics != 0) Log("auto_advance: page %08x turned", page.key);
    return out.press;
}

// The player pressed confirm themselves: tell the pacer, so the page they turned is not taken up again
// where its countdown was if it comes back with the same key (Pacer::PlayerTurned).
void PlayerPressed(void* ctx_ptr) {
    if (g_enabled == 0) return;
    const uintptr_t ctx = reinterpret_cast<uintptr_t>(ctx_ptr);
    const uintptr_t holder = ctx >= 0x10000 ? At<uint32_t>(ctx, kCtxHolder) : 0;
    if (holder < 0x10000) return;
    const Page page = CurrentPage(holder, true);
    g_pacer.PlayerTurned(page.key, page.ready);
}

// Replaces the button query at the two wait handlers' call sites: same thiscall shape (ecx = g_ctx,
// three stack arguments, ret 0xC). The real query is asked first, so a player's press is the
// player's, and battle_load's filter (it hooks the query) still applies.
uint32_t __attribute__((thiscall)) WaitKeyQuery(void* ctx, int button, int mode, int flag) {
    const uint32_t pressed = reinterpret_cast<ButtonQueryFn>(kButtonQueryThunk)(ctx, button, mode, flag);
    if ((pressed & 0xFF) != 0) {
        PlayerPressed(ctx);
        return pressed;
    }
    return AutoPress(ctx) ? 1u : pressed;
}

// ---------------------------------------------------------------- patching the call sites
bool WriteCode(uintptr_t address, const void* bytes, size_t size) {
    DWORD old_protect = 0;
    void* at = reinterpret_cast<void*>(address);
    if (!VirtualProtect(at, size, PAGE_EXECUTE_READWRITE, &old_protect)) return false;
    std::memcpy(at, bytes, size);
    DWORD ignored = 0;
    VirtualProtect(at, size, old_protect, &ignored);
    FlushInstructionCache(GetCurrentProcess(), at, size);
    return true;
}

bool RedirectCall(uintptr_t site) {
    uint8_t call[5];
    call[0] = 0xe8;
    const uint32_t rel = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(&WaitKeyQuery) - (site + 5));
    std::memcpy(call + 1, &rel, sizeof(rel));
    return WriteCode(site, call, sizeof(call));
}

bool g_patched = false;

void RestoreCalls() {
    if (!g_patched) return;
    WriteCode(kWaitCloseCall, kWaitCloseOriginal, sizeof(kWaitCloseOriginal));
    WriteCode(kWaitKeepCall, kWaitKeepOriginal, sizeof(kWaitKeepOriginal));
    g_patched = false;
}

// ---------------------------------------------------------------- deckscreen (optional)
// How the game's UI canvas lands on the screen: scaled by min(W/1280, H/720) and centred - the game's
// own 16:9 handling and deckscreen's letterbox alike - unless deckscreen runs with its letterbox off,
// where the canvas is scaled by H/720 and overflows left and right. Read from the settings registry
// once a second on the render thread; without deckscreen nothing is read and nothing is needed.
int g_deck_stretch = 0;   // 1 = deckscreen is on with Letterbox off
DWORD g_deck_checked = 0;
int g_deck_enabled_at_start = -1;   // its Enabled takes effect at a restart: the value it started with

void CheckDeckscreen() {
    const DWORD tick = GetTickCount();
    if (g_deck_checked != 0 && tick - g_deck_checked < 1000) return;
    g_deck_checked = tick;
    if (g_api->settings_acquire == nullptr || g_api->settings_release == nullptr) return;
    uint32_t count = 0, generation = 0;
    const AtmtSettingsGroup* groups = g_api->settings_acquire(&count, &generation);
    int enabled = -1, letterbox = -1;
    for (uint32_t g = 0; groups != nullptr && g < count; ++g) {
        if (groups[g].mod_name == nullptr || std::strcmp(groups[g].mod_name, "deckscreen") != 0) continue;
        for (uint32_t i = 0; i < groups[g].count; ++i) {
            const AtmtSetting& s = groups[g].settings[i];
            if (s.type != ATMT_SETTING_BOOL || s.key == nullptr || s.value == nullptr) continue;
            if (std::strcmp(s.key, "Enabled") == 0) enabled = *static_cast<const int32_t*>(s.value);
            if (std::strcmp(s.key, "Letterbox") == 0) letterbox = *static_cast<const int32_t*>(s.value);
        }
    }
    g_api->settings_release();
    if (enabled >= 0 && g_deck_enabled_at_start < 0) g_deck_enabled_at_start = enabled;
    const int stretch = (g_deck_enabled_at_start == 1 && letterbox == 0) ? 1 : 0;
    if (stretch != g_deck_stretch) {
        Log("auto_advance: deckscreen's letterbox is %s - the timer follows", stretch ? "off" : "on");
    }
    g_deck_stretch = stretch;
}

// ---------------------------------------------------------------- the overlay window (render thread)

void BindImGui(const AtmtOverlayFrame* frame) {
    ImGui::SetCurrentContext(static_cast<ImGuiContext*>(frame->imgui_context));
    ImGui::SetAllocatorFunctions(frame->imgui_alloc, frame->imgui_free, frame->imgui_alloc_user);
}

void Toggle() {
    const LONG now_on = g_enabled != 0 ? 0 : 1;
    InterlockedExchange(reinterpret_cast<volatile LONG*>(&g_enabled), now_on);
    Toast(now_on ? "Auto-advance: on" : "Auto-advance: off");
    Log("auto_advance: %s", now_on ? "on" : "off");
}

bool PieFresh(PieState* out) {
    EnterCriticalSection(&g_pie_lock);
    *out = g_pie;
    LeaveCriticalSection(&g_pie_lock);
    return out->updated_ms > 0.0 && NowMs() - out->updated_ms < kPieStaleMs;
}

// Is one of the game's windows in the foreground? GetAsyncKeyState and XInput see the keyboard and the
// pad whatever window has them, so a key typed in another program must not toggle anything here.
bool GameFocused() {
    HWND fg = GetForegroundWindow();
    if (fg == nullptr) return false;
    DWORD pid = 0;
    GetWindowThreadProcessId(fg, &pid);
    return pid == GetCurrentProcessId();
}

uint32_t __cdecl WindowUpdate(const AtmtOverlayFrame* frame, void*) {
    BindImGui(frame);
    InterlockedExchange(&g_input_held, (frame->flags & ATMT_OVERLAY_FRAME_INPUT) == 0 ? 1 : 0);
    CheckDeckscreen();

    // Sampled every frame so the edges stay true; acted on only when hotkeys may act.
    const bool key = g_key.PressedOnce();
    const bool pad_down = g_pad_chord != 0 && frame->input.have_pad != 0 &&
                          (frame->input.pad_buttons & g_pad_chord) == g_pad_chord;
    const bool pad = pad_down && !g_pad_was_down;
    g_pad_was_down = pad_down;
    const bool hold_key = g_hold_key.PressedOnce();
    const bool hold_pad_down = g_hold_pad_chord != 0 && frame->input.have_pad != 0 &&
                               (frame->input.pad_buttons & g_hold_pad_chord) == g_hold_pad_chord;
    const bool hold_pad = hold_pad_down && !g_hold_pad_was_down;
    g_hold_pad_was_down = hold_pad_down;
    if ((frame->flags & ATMT_OVERLAY_FRAME_HOTKEYS) != 0 && GameFocused()) {
        if (key || pad) Toggle();
        // Only while the game has the input (not the settings bar, not the dialog log).
        if ((hold_key || hold_pad) && (frame->flags & ATMT_OVERLAY_FRAME_INPUT) != 0) {
            const DWORD tick = GetTickCount();
            InterlockedExchange(&g_hold_pressed_at, static_cast<LONG>(tick != 0 ? tick : 1u));
        }
    }

    PieState pie;
    const bool counting = g_enabled != 0 && g_show_pie != 0 && PieFresh(&pie);
    const bool toast = ToastShowing(GetTickCount());
    if (counting || toast || (frame->flags & ATMT_OVERLAY_FRAME_PREVIEW) != 0) return ATMT_OVERLAY_WANT_DRAW;
    return 0;
}

ImU32 Color(const ImVec4& c, float alpha) {
    return ImGui::ColorConvertFloat4ToU32(ImVec4(c.x, c.y, c.z, c.w * alpha));
}

// The 1280x720 canvas on the screen: scale and origin.
void CanvasToScreen(const ImGuiViewport* vp, float* scale, ImVec2* origin) {
    const float w = vp->Size.x, h = vp->Size.y;
    float s = std::min(w / 1280.0f, h / 720.0f);
    if (g_deck_stretch != 0) s = h / 720.0f;
    *scale = s;
    *origin = ImVec2(vp->Pos.x + (w - 1280.0f * s) * 0.5f, vp->Pos.y + (h - 720.0f * s) * 0.5f);
}

// A pie of `left` (1 = full) at `c`, filled clockwise from 12 o'clock; in arcs of at most a half turn
// so every filled path stays convex.
void DrawPie(ImDrawList* dl, ImVec2 c, float r, float left, ImU32 fill, ImU32 back, ImU32 rim, float rim_w) {
    dl->AddCircleFilled(c, r, back, 0);
    const float inner = r - rim_w * 1.5f;
    const float pi = 3.14159265f;
    const float start = -pi * 0.5f;
    const float sweep = 2.0f * pi * left;
    for (float done = 0.0f; done < sweep - 1e-4f; done += pi) {
        const float part = std::min(pi, sweep - done);
        dl->PathLineTo(c);
        dl->PathArcTo(c, inner, start + done, start + done + part, 0);
        dl->PathFillConvex(fill);
    }
    dl->AddCircle(c, r, rim, 0, rim_w);
}

void __cdecl WindowDraw(const AtmtOverlayFrame* frame, void*) {
    BindImGui(frame);
    const bool preview = (frame->flags & ATMT_OVERLAY_FRAME_PREVIEW) != 0;
    PieState pie;
    bool counting = g_enabled != 0 && g_show_pie != 0 && PieFresh(&pie);
    float alpha = 1.0f;
    if (!counting && preview) {   // the settings bar shows this menu: a sample, where it would be
        pie = PieState();
        pie.left = 0.65f;
        pie.have_box = true;
        pie.prompt[0] = 1100.0f, pie.prompt[1] = 680.0f;   // where a bottom box's prompt is
        counting = true;
        alpha = frame->bar_alpha;
    }

    const ImGuiViewport* vp = ImGui::GetMainViewport();
    float s = 1.0f;
    ImVec2 origin;
    CanvasToScreen(vp, &s, &origin);
    ImDrawList* dl = ImGui::GetForegroundDrawList();
    const float r = std::max(4.0f, static_cast<float>(g_pie_size) * 0.5f * s);
    const float margin = 16.0f * s;

    if (counting && alpha > 0.0f) {
        ImVec2 c;
        if (g_position == kAtBox && pie.have_box) {
            // On the box's bottom edge, just right of the game's advance prompt (kept on screen).
            const ImVec2 prompt(origin.x + pie.prompt[0] * s, origin.y + pie.prompt[1] * s);
            c = ImVec2(prompt.x + kPromptHalf * s + 4.0f * s + r, prompt.y);
            const float max_x = vp->Pos.x + vp->Size.x - r - 2.0f;
            if (c.x > max_x) c.x = max_x;
        } else {
            const bool rgt = g_position != kTopLeft && g_position != kBottomLeft;
            const bool top = g_position != kBottomRight && g_position != kBottomLeft;
            c = ImVec2(rgt ? vp->Pos.x + vp->Size.x - margin - r : vp->Pos.x + margin + r,
                       top ? vp->Pos.y + margin + r : vp->Pos.y + vp->Size.y - margin - r);
        }
        const float opacity = static_cast<float>(g_opacity) / 100.0f;
        const ImVec4& fill = pie.held ? atmt::theme::kHint : pie.voice ? atmt::theme::kText : atmt::theme::kName;
        DrawPie(dl, c, r, pie.left, Color(fill, alpha), Color(atmt::theme::kSheet, opacity * alpha),
                Color(atmt::theme::kRule, alpha), std::max(1.0f, 1.5f * s));
        if (pie.held) {   // a pause sign over the dimmed pie
            const float bw = r * 0.22f, bh = r * 0.5f, gap = r * 0.14f;
            const ImU32 bar = Color(atmt::theme::kText, alpha);
            dl->AddRectFilled(ImVec2(c.x - gap - bw, c.y - bh), ImVec2(c.x - gap, c.y + bh), bar);
            dl->AddRectFilled(ImVec2(c.x + gap, c.y - bh), ImVec2(c.x + gap + bw, c.y + bh), bar);
        }
    }

    const DWORD now = GetTickCount();
    const char* text = g_toast_text;
    if (text != nullptr && ToastShowing(now)) {
        const DWORD until = static_cast<DWORD>(InterlockedCompareExchange(&g_toast_until, 0, 0));
        const float fade = std::min(1.0f, static_cast<float>(until - now) / 300.0f);
        const float size = std::max(12.0f, 22.0f * s);
        ImFont* font = ImGui::GetFont();
        const ImVec2 m = font->CalcTextSizeA(size, 1e9f, 0.0f, text);
        const float px = 14.0f * s, py = 8.0f * s;
        const ImVec2 a(vp->Pos.x + (vp->Size.x - m.x) * 0.5f - px, vp->Pos.y + margin);
        const ImVec2 b(a.x + m.x + px * 2.0f, a.y + m.y + py * 2.0f);
        dl->AddRectFilled(a, b, Color(atmt::theme::kSheet, 0.9f * fade), 6.0f * s);
        dl->AddRect(a, b, Color(atmt::theme::kRule, fade), 6.0f * s, 0, std::max(1.0f, 1.5f * s));
        dl->AddText(font, size, ImVec2(a.x + px, a.y + py),
                    Color(g_enabled != 0 ? atmt::theme::kName : atmt::theme::kHint, fade), text);
    }
}

DWORD WINAPI OverlayThread(LPVOID) {
    // The overlay may load before or after this mod: look for its service until it is there.
    for (int i = 0; i < 300 && InterlockedCompareExchange(&g_stop, 0, 0) == 0; ++i) {
        const void* found = g_api->service_find(ATMT_OVERLAY_SERVICE);
        if (found == nullptr) {
            Sleep(100);
            continue;
        }
        const AtmtOverlayApi* overlay = static_cast<const AtmtOverlayApi*>(found);
        if (overlay->version < ATMT_OVERLAY_API_VERSION || overlay->size < sizeof(AtmtOverlayApi)) {
            LogError("auto_advance: the overlay mod is older than this one - no timer, no toggle key");
            return 0;
        }
        AtmtOverlayWindow window;
        std::memset(&window, 0, sizeof(window));
        window.name = kMenu;
        window.imgui_version = IMGUI_VERSION_NUM;
        window.update = &WindowUpdate;
        window.draw = &WindowDraw;
        if (overlay->add_window(g_api, &window) != 0) {
            LogError("auto_advance: the overlay refused the timer (see the line above)");
            return 0;
        }
        g_overlay = overlay;
        InterlockedExchange(&g_window_added, 1);
        Log("auto_advance: timer and toggle key ready");
        return 0;
    }
    if (InterlockedCompareExchange(&g_stop, 0, 0) == 0) {
        LogError("auto_advance: no overlay mod (atmt_overlay.dll) in this process - no timer, no toggle key");
    }
    return 0;
}

}  // namespace

extern "C" {

__declspec(dllexport) uint32_t __cdecl AtmtModInit(const AtmtModApi* api) {
    if (api == nullptr || api->version < ATMT_MOD_API_VERSION || api->size < sizeof(AtmtModApi)) return 0;
    if (api->hook_create == nullptr || api->hook_enable == nullptr || api->service_find == nullptr) return 0;
    g_api = api;
    InitializeCriticalSection(&g_pie_lock);
    InitializeCriticalSection(&g_voice_lock);
    g_lengths = new std::unordered_map<int, double>();
    if (api->settings_register != nullptr) {
        api->settings_register(api, kMenu, kSettings, sizeof(kSettings) / sizeof(kSettings[0]));
    }
    OnInputChanged(nullptr, nullptr);

    // Every address this mod patches, hooks or calls, by its bytes in the supported ed8.exe.
    static const atmt_code::Expect kExpected[] = {
        {kWaitCloseCall, reinterpret_cast<const char*>(kWaitCloseOriginal), 5, "the wait-and-close key query"},
        {kWaitKeepCall, reinterpret_cast<const char*>(kWaitKeepOriginal), 5, "the wait-and-keep key query"},
        {kButtonQueryThunk, "\xe9\xc1\x86\x03\x00", 5, "the button query's thunk"},
        {kPlayVoice, "\x55\x8b\xec\x51\xd9\xee\x8b\x45\x0c", 9, "PlayVoice"},
        {kVoicePlaying, "\x55\x8b\xec\x53\x56\x8b\xb1\x80\x63\x0b\x00", 11, "the voice's is-playing"},
        {kVoicePath, "\x55\x8b\xec\x8b\x91\x80\x63\x0b\x00", 9, "the voice's path"},
    };
    char why_not[256];
    if (!atmt_code::CheckAll(kExpected, sizeof(kExpected) / sizeof(kExpected[0]), why_not, sizeof(why_not))) {
        LogError("auto_advance: %s - not installed", why_not);
        return ATMT_MOD_API_VERSION;
    }

    g_play_voice_hook = api->hook_create(reinterpret_cast<void*>(kPlayVoice),
                                         reinterpret_cast<void*>(&PlayVoiceDetour),
                                         reinterpret_cast<void**>(&g_play_voice));
    if (g_play_voice_hook == nullptr || g_play_voice == nullptr || api->hook_enable(g_play_voice_hook) != 0) {
        LogError("auto_advance: could not hook PlayVoice (0x%08x) - pages will not wait for their voice",
                 static_cast<unsigned>(kPlayVoice));
        g_play_voice_hook = nullptr;
    }
    if (!RedirectCall(kWaitCloseCall) || !RedirectCall(kWaitKeepCall)) {
        g_patched = true;
        RestoreCalls();
        LogError("auto_advance: could not patch the dialog's key wait - not installed");
        return ATMT_MOD_API_VERSION;
    }
    g_patched = true;

    HANDLE thread = CreateThread(nullptr, 0, OverlayThread, nullptr, 0, nullptr);
    if (thread != nullptr) CloseHandle(thread);
    Log("auto_advance: ready (%s; %d ms per character, toggle %s%s%s)", g_enabled ? "on" : "off",
        g_ms_per_char, g_key_name, g_pad_name[0] != '\0' ? " / " : "", g_pad_name);
    return ATMT_MOD_API_VERSION;
}

__declspec(dllexport) void __cdecl AtmtModShutdown(void) {
    InterlockedExchange(&g_stop, 1);
    if (g_api == nullptr) return;
    RestoreCalls();
    if (g_play_voice_hook != nullptr) g_api->hook_remove(g_play_voice_hook);
    g_play_voice_hook = nullptr;
    if (InterlockedExchange(&g_window_added, 0) != 0 && g_overlay != nullptr) {
        g_overlay->remove_window(g_api);   // waits for a frame in progress
    }
    if (g_api->settings_unregister != nullptr) g_api->settings_unregister(g_api);
}

// One sentence for the overlay and the manager (shared/mod_api.h, AtmtModDescription).
__declspec(dllexport) const char* __cdecl AtmtModDescription(void) {
    return "Turns dialog pages by itself after a reading time per character, never before the voice line "
           "ends, with a small timer by the box (F7 switches it on and off; E / Y holds the current line).";
}

}  // extern "C"
