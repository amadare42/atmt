// battle_load.cpp - the game's own load menu, in the middle of a battle (and on the field).
//
// The game has a load menu in two places: the title ("Load") and the camp menu. A battle has neither,
// so a lost fight means sitting through it (or the game-over screen) and then the title. This mod
// opens the same save/load menu over a running battle - a hotkey or a pad chord - and enters the save
// picked there. Everything was read out of ed8.exe (plain VAs, no ASLR, see docs/MODS.md);
// nothing is emulated input, every step is a call the game itself makes somewhere else.
//
// The per-frame scene update, FUN_005be250 (this = the UI/scene holder, [g_ctx+0x7BC]):
//   if [ctx+0xDE4] (a scene change is pending)  -> tear the scene down, load the next one, return
//   if [holder+0x5A90] (a battle is running)     -> r = FUN_004DB080(battle, dt); if r != 0 the battle
//        ends through FUN_005A7B00 (r == 0xD: "title", else back to the field); return
//   otherwise the field: ... the title update, the camp menu (FUN_0054EDE0), ...
// The save menu's update (FUN_0064D2C0) is only called by the title, the camp menu and the startup,
// never in battle. So the hook is the battle update: while this mod's menu is open the battle is not
// updated at all (it freezes, and gets none of the input), and the menu's update is called instead,
// with the same dt, exactly as the camp menu does it (FUN_0054EDE0: "menu open -> menu update, return").
//
// The camp menu's "Load" (FUN_0054E880, case 1): menu->Open(1, 0x0040D3D7, camp), then
//   the list confirm       -> the save manager reads the file into the live game state (g_ctx+0xB6430)
//   manager cb1, phase 1   -> FUN_005E3060: deletes the running scene's script tasks ([ctx+0x7B4])
//   menu, phase 1          -> re-reads save511.dat (system settings), applies it, closes
//   menu callback          -> camp+0x20 = 1; the camp update then (FUN_0054ED90):
//                               holder+0x5AAD = 1; FUN_005E5740(ctx, ctx+0xB683C, 0, 0, 1, 1)
// i.e. "change the scene to the map the save names, forget the current map" - the forgetting is what
// makes the scene teardown (FUN_005B9470) destroy everything, the battle object included.
//
// Two routes after the pick (Route=):
//   direct - the camp's own sequence above, from the battle (Open with phase 1, then the map change).
//   title  - the pick is recorded and the menu is cancelled before anything is read (the list confirm's
//            SetSubState(2) is hooked and turned into the cancel, SetSubState(1)); the battle then
//            leaves for the title the way a lost battle's "return to title" does (FUN_005A7B00:
//            FUN_005E5740(ctx, "title", "", 0, 0, 1)), and load_last_save's title route loads the save
//            there (shared/load_last_save_api.h). Slower, but every step is one the game already
//            makes from a battle.
//
// Measured in the game (2026-09-26): the menu renders over a battle, the game drives the save manager's
// state machine (vtable+0x74, FUN_004855B0) during a battle by itself, and the direct route enters the
// save. The manager is still hooked and counted: if it ever stalls while this mod waits for it, the mod
// drives it itself, once per battle frame.
//
// On the field the same key opens the camp's own Load. There is no single "field update" to freeze -
// the field pauses because a dozen systems check "is the camp menu open" (FUN_0053FC80) - so the camp
// is what gets opened, with the game's own calls:
//   the field's control handler FUN_005B7FC0(holder, dt) runs every field frame (the camp opens from it
//   on the menu button: FUN_005AF2B0(holder) -> FUN_0054C700(camp, 0, 0)). It is hooked; on the key, and
//   only when that handler itself would open the camp (the same checks), the camp opens, and once its
//   opening has played the save menu is opened exactly as the camp's System -> Load does:
//   menu->Open(1, 0x0040D3D7, camp). From there the camp does everything itself (its update runs the
//   menu; its callback + FUN_0054ED90 close the camp and change the scene). A cancel closes the camp
//   again (FUN_0054CCE0, the camp's own "close"), so the key lands on the load list and leaves from it.
//
// During an event (a dialog, a cutscene) the camp cannot open - the control handler bails out on
// [ctx+0x170C] bit 1 ("the player has no control", measured 0x86 with a dialog up) and on
// [holder+0x1418] (an event runs). There the menu is opened directly, like in a battle (phase 1, this
// mod's callback), and driven from the same field hook. Nothing freezes the event, so the event is kept
// from the menu's buttons instead: every button query goes through FUN_00446240(input, button, mode,
// flag) - mode 1 even consumes the press - and while this menu is open that answers "not pressed" to
// everyone but the menu's own update. A load is the battle's direct route (the phase 1 read deletes
// the event's script tasks, the scene change leaves); a cancel hands the dialog back where it was.
#include "code_check.h"
#include "load_last_save_api.h"
#include "mod_api.h"
#include "overlay_api.h"
#include "atmt_input.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

namespace {

// ---------------------------------------------------------------- the game (ed8.exe EN, no ASLR)
typedef int(__attribute__((thiscall)) * BattleUpdateFn)(void* battle, float dt);
typedef void(__attribute__((thiscall)) * MenuOpenFn)(void* menu, int phase, void* done_cb, void* done_arg);
typedef void(__attribute__((thiscall)) * MenuUpdateFn)(void* menu, float dt);
typedef void(__attribute__((thiscall)) * ManagerUpdateFn)(void* manager, float dt);
typedef void(__attribute__((thiscall)) * SetSubStateFn)(void* manager, int sub_state);
typedef void(__attribute__((thiscall)) * ChangeSceneFn)(void* ctx, const char* map, const char* entry,
                                                         int fade, char forget_map, char stop_bgm);
typedef void(__attribute__((thiscall)) * FieldControlFn)(void* holder, float dt);
typedef void(__attribute__((thiscall)) * CloseCampFn)(void* camp);
typedef uint32_t(__attribute__((thiscall)) * ButtonQueryFn)(void* input, int button, int mode, char flag);
typedef unsigned char(__attribute__((thiscall)) * QueryFn)(void* self);
typedef unsigned char(__attribute__((thiscall)) * QueryArgFn)(void* self, int arg);

constexpr uintptr_t kBattleUpdate = 0x004DB080;    // FUN_004DB080(battle, dt) -> 0 = keep fighting
constexpr uintptr_t kMenuOpen = 0x0064A720;        // save menu Open(phase, done_cb, done_arg), ret 0xC
constexpr uintptr_t kMenuUpdate = 0x0064D2C0;      // save menu update(dt), ret 4
constexpr uintptr_t kManagerUpdate = 0x004855B0;   // save manager state machine (vtable+0x74), ret 4
constexpr uintptr_t kSetSubState = 0x00484A30;     // manager SetSubState(n): [+0x58] = n, ret 4
constexpr uintptr_t kChangeScene = 0x005E5740;     // g_ctx: change the scene (map, entry, ...), ret 0x14
constexpr uintptr_t kOverlayActive = 0x0063EEB0;   // (g_ctx+0xDE8): a full-screen overlay is up
constexpr uintptr_t kMessageActive = 0x00702580;   // (g_ctx): a message box is up
constexpr uintptr_t kMessagePending = 0x00702700;  // (g_ctx, 0): a message box waits for input
constexpr uintptr_t kFieldControl = 0x005B7FC0;    // field control handler (holder, dt), ret 4
constexpr uintptr_t kButtonQuery = 0x00446240;     // (input, button, mode, flag) -> pressed, ret 0xC
constexpr uintptr_t kOpenCamp = 0x005AF2B0;        // holder: open the camp menu -> 1 when it did
constexpr uintptr_t kCloseCamp = 0x0054CCE0;       // camp: close it (its own "close")
constexpr uintptr_t kCampBusy = 0x00692290;        // [camp+8]: a UI animation is still playing
constexpr uintptr_t kCampLoadDone = 0x0040D3D7;    // the camp's load callback: camp+0x20 = 1 on success
constexpr uintptr_t kGameCtxPtr = 0x00C7C598;      // g_ctx
constexpr uintptr_t kSaveNameBuffer = 0x00C3E758;  // the file name the save reader opens

constexpr unsigned kCtxHolder = 0x7BC;             // g_ctx -> the UI/scene holder
constexpr unsigned kCtxSceneChange = 0xDE4;        // byte: a scene change is pending
constexpr unsigned kCtxSavedMap = 0xB683C;         // the loaded save's map name (inside the save data)
constexpr unsigned kCtxFlags = 0x170C;             // bit 1: a menu is up; bit 23: the camp is forbidden
constexpr unsigned kCtxRideA = 0x12AC74;           // the control handler's two "other mode" fields
constexpr unsigned kCtxRideB = 0x12AC78;
constexpr unsigned kCtxScript = 0x1C60;            // short: a script has the controls
constexpr uint32_t kFlagNoControl = 0x2;          // the camp sets it, and so does every event
constexpr uint32_t kFlagCampUp = 0x10;             // the camp (and its pages) set this one too
constexpr uint32_t kFlagCampForbidden = 0x800000;
constexpr unsigned kHolderSaveMenu = 0x5A80;
constexpr unsigned kHolderOtherMenu = 0x5A88;      // a menu the camp and the title also defer to
constexpr unsigned kHolderBattle = 0x5A90;
constexpr unsigned kHolderCamp = 0x5A74;
constexpr unsigned kHolderPlayer = 0xD1C;
constexpr unsigned kHolderEvent = 0x1418;
constexpr unsigned kHolderSavePoint = 0x5A84;      // the save point's menu: active when [+0x170] != 0
constexpr unsigned kCampPage = 0x10;               // -1 = closed
constexpr unsigned kCampUi = 0x08;
constexpr unsigned kCampLoaded = 0x20;             // set by the camp's load callback
constexpr unsigned kHolderBattleEnded = 0x5AAC;    // byte: set by a battle's end, cleared for "title"
constexpr unsigned kHolderLoaded = 0x5AAD;         // byte: set by the camp and the title after a load

constexpr unsigned kMenuState = 0x04;              // 0 = closed
constexpr unsigned kMenuPhase = 0x0C;
constexpr unsigned kMenuManager = 0x1C;
constexpr unsigned kMgrSlotBase = 0x20;
constexpr unsigned kMgrAutosave = 0x4C;            // byte: the autosave tab
constexpr unsigned kMgrState = 0x50;
constexpr unsigned kMgrSlot = 0x54;
constexpr unsigned kMgrSubState = 0x58;

constexpr int kPhaseCamp = 1;    // the camp menu's load: its cb1 deletes the scene's script tasks
constexpr int kPhaseTitle = 2;   // the title's load: the same read, without the task teardown
constexpr int kSubStateRead = 2;     // the list's confirm: read the slot
constexpr int kSubStateCancel = 1;   // the list's cancel

// Battle frames the manager may sit unpolled before this mod drives it itself.
constexpr unsigned kManagerStallFrames = 3;

// ---------------------------------------------------------------- settings
enum Route : int32_t { kRouteDirect = 0, kRouteTitle = 1 };
const char* const kRouteTokens[] = {"direct", "title"};
enum PhaseChoice : int32_t { kPhaseAuto = 0, kPhaseChoiceCamp = 1, kPhaseChoiceTitle = 2 };
const char* const kPhaseTokens[] = {"auto", "camp", "title"};

int32_t g_enabled = 1;
int32_t g_field = 1;
int32_t g_events = 1;
char g_key_name[32] = "F9";
char g_chord_name[32] = "BACK+START";
int32_t g_route = kRouteDirect;
int32_t g_phase_choice = kPhaseAuto;
int32_t g_diagnostics = 0;

volatile LONG g_key_vk = 0;      // the key chord parsed from g_key_name (0 = none)
volatile LONG g_chord = 0;       // parsed from g_chord_name (0 = none)

const AtmtModApi* g_api = nullptr;
volatile LONG g_started = 0;

void Log(const char* fmt, ...) {
    if (g_api == nullptr || g_api->log == nullptr) return;
    char buf[400];
    va_list args;
    va_start(args, fmt);
    _vsnprintf(buf, sizeof(buf) - 1, fmt, args);
    va_end(args);
    buf[sizeof(buf) - 1] = '\0';
    g_api->log(buf);
}

// A failure: written to atmt_loader.log unless LogLevel=off (Log is written with LogLevel=all only).
void LogError(const char* fmt, ...) {
    if (g_api == nullptr || g_api->log_error == nullptr) return;
    char buf[400];
    va_list args;
    va_start(args, fmt);
    _vsnprintf(buf, sizeof(buf) - 1, fmt, args);
    va_end(args);
    buf[sizeof(buf) - 1] = '\0';
    g_api->log_error(buf);
}

// ---------------------------------------------------------------- settings plumbing
void ParseInputs() {
    const unsigned vk = atmt::KeyChordFromName(g_key_name);
    const unsigned chord = atmt::PadChordFromName(g_chord_name);
    if (vk == 0 && g_key_name[0] != '\0') LogError("battle_load: Key=%s is not a key (or chord) name - no key", g_key_name);
    if (chord == 0 && g_chord_name[0] != '\0') {
        LogError("battle_load: PadChord=%s is not a pad chord - no chord", g_chord_name);
    }
    InterlockedExchange(&g_key_vk, static_cast<LONG>(vk));
    InterlockedExchange(&g_chord, static_cast<LONG>(chord));
    Log("battle_load: open key %s, pad chord %s", vk != 0 ? g_key_name : "(none)",
        chord != 0 ? atmt::PadChordName(static_cast<uint16_t>(chord)).c_str() : "(none)");
}

void __cdecl OnInputChanged(const AtmtSetting*, void*) { ParseInputs(); }

const AtmtSetting kSettings[] = {
    {ATMT_SETTING_BOOL, ATMT_SETTING_RESTART, "General", "Enabled", "Enabled",
     "the load menu in battle and on the field (hotkey / pad chord below)", &g_enabled},
    {ATMT_SETTING_BOOL, ATMT_SETTING_LIVE, "General", "Field", "On the field too",
     "the key also opens the camp's Load on the field (wherever the camp menu could open)", &g_field},
    {ATMT_SETTING_BOOL, ATMT_SETTING_LIVE, "General", "Events", "During dialogs too",
     "the key also opens the load menu during a dialog or cutscene (the event waits underneath and\n"
     "gets none of the menu's buttons; a load ends it)", &g_events},
    {ATMT_SETTING_KEY, ATMT_SETTING_LIVE, "General", "Key", "Open key",
     "the key (or chord, e.g. Ctrl+F9) that opens the load menu (none = the pad chord only)", g_key_name,
     sizeof(g_key_name), 0, 0, 0, nullptr, 0, &OnInputChanged},
    {ATMT_SETTING_PAD_BUTTON, ATMT_SETTING_LIVE, "General", "PadChord", "Open pad chord",
     "the pad chord that opens the load menu (e.g. BACK+START; none = the key only)",
     g_chord_name, sizeof(g_chord_name), 0, 0, 0, nullptr, 0, &OnInputChanged},
    {ATMT_SETTING_ENUM, ATMT_SETTING_LIVE, "General", "Route", "After the pick",
     "in battle only; the field always uses the camp's own Load\n"
     "direct - load in place and go straight to the save's map (what the camp menu's Load does)\n"
     "title  - go back to the title and load the save there (needs load_last_save; slower, but\n"
     "         every step is one the game already makes from a battle)",
     &g_route, 0, 0, 0, 0, kRouteTokens, 2},
    {ATMT_SETTING_ENUM, ATMT_SETTING_LIVE | ATMT_SETTING_ADVANCED, "General", "MenuPhase", "Menu phase",
     "which load the direct route asks the menu for: auto (= camp), camp (phase 1: deletes the\n"
     "scene's script tasks after the read), title (phase 2: the read alone)",
     &g_phase_choice, 0, 0, 0, 0, kPhaseTokens, 3},
    {ATMT_SETTING_BOOL, ATMT_SETTING_LIVE | ATMT_SETTING_ADVANCED, "General", "Diagnostics", "Diagnostics",
     "log every menu / save manager state change while the menu is open", &g_diagnostics},
};

// ---------------------------------------------------------------- reading the game
template <typename T>
T At(uintptr_t base, unsigned offset) {
    return *reinterpret_cast<volatile const T*>(base + offset);
}

template <typename T>
void Put(uintptr_t base, unsigned offset, T value) {
    *reinterpret_cast<volatile T*>(base + offset) = value;
}

bool Valid(uintptr_t p) { return p >= 0x10000; }

uintptr_t Ctx() { return *reinterpret_cast<volatile const uint32_t*>(kGameCtxPtr); }

uintptr_t Holder() {
    const uintptr_t ctx = Ctx();
    return Valid(ctx) ? At<uint32_t>(ctx, kCtxHolder) : 0;
}

uintptr_t SaveMenu() {
    const uintptr_t holder = Holder();
    return Valid(holder) ? At<uint32_t>(holder, kHolderSaveMenu) : 0;
}

uintptr_t Manager(uintptr_t menu) { return Valid(menu) ? At<uint32_t>(menu, kMenuManager) : 0; }

// ---------------------------------------------------------------- the pad
// The overlay owns the pad (its XInput hook, shared/overlay_api.h): read_pad is the same read its own
// chords use, and it says when the overlay holds the input - then neither the key nor the chord fires.
// Without the overlay, the pad is read straight from xinput1_3's export.
const AtmtOverlayApi* g_overlay = nullptr;
bool g_overlay_looked = false;
DWORD g_overlay_retry = 0;

const AtmtOverlayApi* Overlay() {
    if (g_overlay != nullptr) return g_overlay;
    const DWORD now = GetTickCount();
    if (g_overlay_looked && static_cast<LONG>(now - g_overlay_retry) < 0) return nullptr;
    g_overlay_looked = true;
    g_overlay_retry = now + 2000;   // mods start in no fixed order: ask again later
    const void* found = g_api->service_find != nullptr ? g_api->service_find(ATMT_OVERLAY_SERVICE) : nullptr;
    const AtmtOverlayApi* overlay = static_cast<const AtmtOverlayApi*>(found);
    if (overlay == nullptr) return nullptr;
    if (overlay->version < ATMT_OVERLAY_API_VERSION || overlay->size < sizeof(AtmtOverlayApi)
        || overlay->read_pad == nullptr) {
        Log("battle_load: the overlay is too old for read_pad - reading XInput directly");
        g_overlay_retry = now + 0x7FFFFFFF;
        return nullptr;
    }
    g_overlay = overlay;
    Log("battle_load: reading the pad through the overlay");
    return g_overlay;
}

struct PadState {
    uint32_t packet;
    uint16_t buttons;
    uint8_t lt, rt;
    int16_t lx, ly, rx, ry;
};
typedef uint32_t(__stdcall* XInputGetStateFn)(uint32_t index, PadState* state);
XInputGetStateFn g_xinput = nullptr;

uint16_t XInputButtons() {
    if (g_xinput == nullptr) {
        HMODULE xinput = GetModuleHandleW(L"xinput1_3.dll");
        if (xinput == nullptr) return 0;
        g_xinput = reinterpret_cast<XInputGetStateFn>(GetProcAddress(xinput, "XInputGetState"));
        if (g_xinput == nullptr) return 0;
    }
    uint16_t buttons = 0;
    for (uint32_t i = 0; i < 4; ++i) {
        PadState state{};
        if (g_xinput(i, &state) == 0) buttons = static_cast<uint16_t>(buttons | state.buttons);
    }
    return buttons;
}

// ---------------------------------------------------------------- the sequence (game thread)
enum Stage : LONG {
    kIdle = 0,   // a battle is running; watching the key and the chord
    kMenu,       // the save menu is open over the frozen battle
    kLeaving,    // the scene change is requested; nothing to do until the next battle
    kCampOpening,   // field: the camp was asked to open; waiting for its opening to play
    kCampMenu,      // field: the save menu is open over the camp
    kEventMenu,     // field, during an event: the save menu is open over it, driven by this mod
};

BattleUpdateFn g_battle_original = nullptr;
FieldControlFn g_field_original = nullptr;
ButtonQueryFn g_button_original = nullptr;
volatile LONG g_in_menu_update = 0;   // the menu's own update is running: its button queries pass
ManagerUpdateFn g_manager_original = nullptr;
SetSubStateFn g_set_sub_state_original = nullptr;
bool g_manager_hooked = false;
bool g_sub_state_hooked = false;

volatile LONG g_stage = kIdle;
uintptr_t g_battle = 0;              // the battle object the edges below belong to
atmt::KeyEdge g_key_edge;            // the open key (a chord); Reset: a key held right now is no press
bool g_chord_was_down = true;
int g_route_now = kRouteDirect;      // the route this menu was opened for (the setting may change)
int g_phase_now = kPhaseCamp;
volatile LONG g_done = 0;            // the menu called back
volatile LONG g_ok = 0;              // ... with success
volatile LONG g_manager_calls = 0;   // FUN_004855B0 calls, by anyone
LONG g_manager_calls_seen = 0;
unsigned g_manager_stall = 0;
bool g_driving_manager = false;
char g_file[64] = {0};               // the save the menu read / the player picked
bool g_picked = false;               // title route: the list's confirm was recorded (and cancelled)
unsigned g_frames = 0;
int g_last_menu_state = -1, g_last_mgr_state = -1, g_last_mgr_sub = -1;

// The menu's completion callback: plain cdecl (ok, arg), like the camp's FUN_0053FC90.
void __cdecl OnMenuDone(char ok, void*) {
    InterlockedExchange(&g_ok, ok != 0 ? 1 : 0);
    InterlockedExchange(&g_done, 1);
}

// The save file a slot names (the inverse of the manager's "save%03d.dat" / "autosave%02d.dat").
void NameSlot(unsigned slot, bool autosave) {
    _snprintf(g_file, sizeof(g_file) - 1, autosave ? "autosave%02u.dat" : "save%03u.dat", slot);
    g_file[sizeof(g_file) - 1] = '\0';
}

// The leaf of the name the save reader is opening, when it is a save (not the system file).
void WatchReadName() {
    char name[0x104];
    std::memcpy(name, reinterpret_cast<const void*>(kSaveNameBuffer), sizeof(name));
    name[sizeof(name) - 1] = '\0';
    const char* leaf = name;
    for (const char* p = name; *p != '\0'; ++p) {
        if (*p == '\\' || *p == '/') leaf = p + 1;
    }
    if (_stricmp(leaf, "save511.dat") == 0) return;
    if (_strnicmp(leaf, "save", 4) != 0 && _strnicmp(leaf, "autosave", 8) != 0) return;
    if (std::strcmp(leaf, g_file) == 0) return;
    _snprintf(g_file, sizeof(g_file) - 1, "%s", leaf);
    g_file[sizeof(g_file) - 1] = '\0';
    Log("battle_load: the game is reading %s", g_file);
}

// Why the menu cannot open right now ("" = it can). The camp and the title make the same checks
// before they read any input.
const char* WhyNotNow(uintptr_t ctx, uintptr_t holder, uintptr_t menu) {
    if (!Valid(ctx) || !Valid(holder) || !Valid(menu)) return "the game's menu objects are not there";
    if (At<uint32_t>(menu, kMenuState) != 0) return "the save menu is already open";
    const uintptr_t mgr = Manager(menu);
    if (!Valid(mgr)) return "the save manager is not there";
    if (At<uint32_t>(mgr, kMgrState) != 0) return "the save manager is busy";
    if (At<uint8_t>(ctx, kCtxSceneChange) != 0) return "a scene change is already pending";
    const uintptr_t other = At<uint32_t>(holder, kHolderOtherMenu);
    if (Valid(other) && At<uint32_t>(other, 8) != 0) return "another menu is open";
    if (reinterpret_cast<QueryFn>(kOverlayActive)(reinterpret_cast<void*>(ctx + 0xDE8)) != 0) {
        return "a full-screen overlay is up";
    }
    if (reinterpret_cast<QueryFn>(kMessageActive)(reinterpret_cast<void*>(ctx)) != 0) {
        return "a message box is up";
    }
    if (reinterpret_cast<QueryArgFn>(kMessagePending)(reinterpret_cast<void*>(ctx), 0) != 0) {
        return "a message box waits for input";
    }
    return "";
}

uint16_t g_last_buttons = 0;

// The pad's buttons now; *held = the overlay has the input (a window of it is open).
uint16_t PadButtons(bool* held) {
    *held = false;
    uint16_t buttons = 0;
    if (const AtmtOverlayApi* overlay = Overlay()) {
        AtmtOverlayInput pad{};
        int32_t overlay_held = 0;
        if (overlay->read_pad(&pad, &overlay_held) != 0) buttons = pad.pad_buttons;
        *held = overlay_held != 0;
    } else {
        buttons = XInputButtons();
    }
    if (g_diagnostics != 0 && buttons != g_last_buttons) {
        Log("battle_load: pad buttons %s (0x%04x)%s", atmt::PadChordName(buttons).c_str(), buttons,
            *held ? " - the overlay holds the input" : "");
    }
    g_last_buttons = buttons;
    return buttons;
}

bool TriggerPressed() {
    bool held = false;
    const uint16_t buttons = PadButtons(&held);
    const unsigned key = static_cast<unsigned>(g_key_vk);
    if (g_key_edge.key() != key) {   // first use, or the setting changed: a held key is no press
        g_key_edge.set_key(key);
        g_key_edge.Reset();
    }
    const bool key_pressed = g_key_edge.PressedOnce() && !held;
    const uint16_t chord = static_cast<uint16_t>(g_chord);
    const bool chord_down = chord != 0 && !held && (buttons & chord) == chord;
    const bool pressed = key_pressed || (chord_down && !g_chord_was_down);
    g_chord_was_down = chord_down;
    return pressed;
}

void OpenMenu(uintptr_t menu) {
    g_route_now = g_route;
    if (g_route_now == kRouteTitle) {
        g_phase_now = kPhaseTitle;   // nothing is read in battle on this route; phase 2 is the harmless one
    } else {
        g_phase_now = g_phase_choice == kPhaseChoiceTitle ? kPhaseTitle : kPhaseCamp;
    }
    InterlockedExchange(&g_done, 0);
    InterlockedExchange(&g_ok, 0);
    g_file[0] = '\0';
    g_picked = false;
    g_manager_calls_seen = g_manager_calls;
    g_manager_stall = 0;
    g_driving_manager = false;
    g_frames = 0;
    g_last_menu_state = g_last_mgr_state = g_last_mgr_sub = -1;
    InterlockedExchange(&g_stage, kMenu);
    reinterpret_cast<MenuOpenFn>(kMenuOpen)(reinterpret_cast<void*>(menu), g_phase_now,
                                            reinterpret_cast<void*>(&OnMenuDone), nullptr);
    Log("battle_load: opened the load menu over the battle (route %s, phase %d)",
        kRouteTokens[g_route_now], g_phase_now);
}

// Direct route: what the camp's update does once its load callback said yes (FUN_0054ED90).
void EnterLoadedSave(uintptr_t ctx, uintptr_t holder) {
    char map[16];
    std::memcpy(map, reinterpret_cast<const void*>(ctx + kCtxSavedMap), sizeof(map));
    map[sizeof(map) - 1] = '\0';
    Put<uint8_t>(holder, kHolderLoaded, 1);
    reinterpret_cast<ChangeSceneFn>(kChangeScene)(reinterpret_cast<void*>(ctx),
                                                  reinterpret_cast<const char*>(ctx + kCtxSavedMap),
                                                  nullptr, 0, 1, 1);
    Log("battle_load: %s is loaded; leaving the battle for its map '%s' (scene change %s)",
        g_file[0] != '\0' ? g_file : "the save", map,
        At<uint8_t>(ctx, kCtxSceneChange) != 0 ? "pending" : "NOT accepted");
}

// Title route: arm load_last_save's title route, then leave the way "return to title" does.
bool LeaveForTitle(uintptr_t ctx, uintptr_t holder) {
    const AtmtLoadLastSaveApi* service =
        g_api->service_find != nullptr
            ? static_cast<const AtmtLoadLastSaveApi*>(g_api->service_find(ATMT_LOAD_LAST_SAVE_SERVICE))
            : nullptr;
    if (service == nullptr || service->version < ATMT_LOAD_LAST_SAVE_API_VERSION
        || service->size < sizeof(AtmtLoadLastSaveApi)) {
        LogError("battle_load: the title route needs load_last_save (TitleLoad=true) - it is not there; the "
            "battle goes on");
        return false;
    }
    char why[160] = {0};
    if (service->load_at_title(g_file, why, sizeof(why)) == 0) {
        LogError("battle_load: load_last_save refused %s (%s); the battle goes on", g_file, why);
        return false;
    }
    Put<uint8_t>(holder, kHolderBattleEnded, 0);
    reinterpret_cast<ChangeSceneFn>(kChangeScene)(reinterpret_cast<void*>(ctx), "title", "", 0, 0, 1);
    Log("battle_load: leaving the battle for the title; load_last_save loads %s there (scene change %s)",
        g_file, At<uint8_t>(ctx, kCtxSceneChange) != 0 ? "pending" : "NOT accepted");
    return true;
}

void LogStates(uintptr_t menu) {
    const uintptr_t mgr = Manager(menu);
    const int menu_state = static_cast<int>(At<uint32_t>(menu, kMenuState));
    const int mgr_state = Valid(mgr) ? static_cast<int>(At<uint32_t>(mgr, kMgrState)) : -1;
    const int mgr_sub = Valid(mgr) ? static_cast<int>(At<uint32_t>(mgr, kMgrSubState)) : -1;
    if (menu_state == g_last_menu_state && mgr_state == g_last_mgr_state && mgr_sub == g_last_mgr_sub) {
        return;
    }
    g_last_menu_state = menu_state;
    g_last_mgr_state = mgr_state;
    g_last_mgr_sub = mgr_sub;
    Log("battle_load: [frame %u] menu state %d phase %d, manager state %d sub %d slot %u, manager calls %ld",
        g_frames, menu_state, static_cast<int>(At<uint32_t>(menu, kMenuPhase)), mgr_state, mgr_sub,
        Valid(mgr) ? At<uint32_t>(mgr, kMgrSlot) : 0u, static_cast<long>(g_manager_calls));
}

// One battle frame while the menu is open: the menu's update instead of the battle's.
void MenuFrame(uintptr_t menu, float dt) {
    ++g_frames;
    reinterpret_cast<MenuUpdateFn>(kMenuUpdate)(reinterpret_cast<void*>(menu), dt);

    // The save manager's own state machine: if nobody runs it while it has work, run it here - once
    // per battle frame, the way the menu's own screens see it run.
    const uintptr_t mgr = Manager(menu);
    if (Valid(mgr) && g_manager_hooked) {
        const LONG calls = g_manager_calls;
        if (At<uint32_t>(mgr, kMgrState) != 0 && calls == g_manager_calls_seen) {
            if (!g_driving_manager && ++g_manager_stall >= kManagerStallFrames) {
                g_driving_manager = true;
                Log("battle_load: the game does not run the save manager during a battle - running it "
                    "once per frame while the menu is open");
            }
        } else if (!g_driving_manager) {
            g_manager_stall = 0;
        }
        if (g_driving_manager) {
            reinterpret_cast<ManagerUpdateFn>(kManagerUpdate)(reinterpret_cast<void*>(mgr), dt);
        }
        g_manager_calls_seen = g_manager_calls;
    }
    WatchReadName();
    if (g_diagnostics != 0) LogStates(menu);
}

int __attribute__((thiscall)) BattleUpdateDetour(void* self, float dt) {
    const uintptr_t battle = reinterpret_cast<uintptr_t>(self);
    if (battle != g_battle) {
        // A new battle: forget the edges (a key held into it is no press) and any finished sequence.
        g_battle = battle;
        g_key_edge.Reset();
        g_chord_was_down = true;
        if (g_stage == kLeaving) InterlockedExchange(&g_stage, kIdle);
    }
    switch (g_stage) {
    case kIdle: {
        if (g_enabled == 0 || !TriggerPressed()) break;
        const uintptr_t ctx = Ctx();
        const uintptr_t holder = Holder();
        const uintptr_t menu = SaveMenu();
        const char* why = WhyNotNow(ctx, holder, menu);
        if (why[0] != '\0') {
            Log("battle_load: not now - %s", why);
            break;
        }
        OpenMenu(menu);
        return 0;
    }
    case kMenu: {
        const uintptr_t menu = SaveMenu();
        if (!Valid(menu)) {
            Log("battle_load: the save menu went away - the battle goes on");
            InterlockedExchange(&g_stage, kIdle);
            break;
        }
        MenuFrame(menu, dt);
        if (At<uint32_t>(menu, kMenuState) != 0) return 0;   // still open: the battle stays frozen

        const uintptr_t ctx = Ctx();
        const uintptr_t holder = Holder();
        g_key_edge.Reset();
        g_chord_was_down = true;
        if (g_route_now == kRouteTitle && g_picked) {
            if (LeaveForTitle(ctx, holder)) {
                InterlockedExchange(&g_stage, kLeaving);
                return 0;
            }
        } else if (g_route_now == kRouteDirect && g_done != 0 && g_ok != 0) {
            EnterLoadedSave(ctx, holder);
            InterlockedExchange(&g_stage, kLeaving);
            return 0;
        } else if (g_route_now == kRouteDirect && g_file[0] != '\0') {
            // The read started and did not succeed: the game state may be half the save's now.
            Log("battle_load: the menu closed without success after reading %s - the battle goes on, "
                "but the game state may no longer match it",
                g_file);
        } else {
            Log("battle_load: the load menu was closed - the battle goes on");
        }
        InterlockedExchange(&g_stage, kIdle);
        break;
    }
    default:   // a field sequence (a battle cannot start while the camp or this menu is open)
        break;
    case kLeaving:
        // The scene change was requested; while it is pending the holder does not call this at all.
        // Called again with no change pending means it never took - give the battle back.
        if (Valid(Ctx()) && At<uint8_t>(Ctx(), kCtxSceneChange) != 0) return 0;
        Log("battle_load: the scene change did not happen - the battle goes on");
        InterlockedExchange(&g_stage, kIdle);
        break;
    }
    return g_battle_original(self, dt);
}

// ---------------------------------------------------------------- the field (the camp's own Load)
// Why the camp cannot open right now: the control handler's own early-outs (FUN_005B7FC0) and the
// camp opener's (FUN_005AF2B0), in their order, after the checks every menu makes.
const char* WhyNotField(uintptr_t ctx, uintptr_t holder, uintptr_t menu) {
    const char* why = WhyNotNow(ctx, holder, menu);
    if (why[0] != '\0') return why;
    const uint32_t flags = At<uint32_t>(ctx, kCtxFlags);
    if ((flags & kFlagNoControl) != 0) return "the player has no control (an event or a menu)";
    if (!Valid(At<uint32_t>(holder, kHolderPlayer))) return "there is no party on the field";
    if (At<uint32_t>(holder, kHolderBattle) != 0) return "a battle is running";
    if (At<uint32_t>(ctx, kCtxRideA) == 0 && At<uint32_t>(ctx, kCtxRideB) == 0
        && At<uint32_t>(holder, kHolderEvent) != 0) {
        return "an event is running";
    }
    if (At<int16_t>(ctx, kCtxScript) != 0) return "a script has the controls";
    if ((flags & kFlagCampForbidden) != 0) return "the game does not allow the camp menu here";
    const uintptr_t camp = At<uint32_t>(holder, kHolderCamp);
    if (!Valid(camp)) return "there is no camp menu here";
    if (At<int32_t>(camp, kCampPage) != -1) return "the camp menu is already open";
    return "";
}

bool CampBusy(uintptr_t camp) {
    const uintptr_t ui = At<uint32_t>(camp, kCampUi);
    return Valid(ui) && reinterpret_cast<QueryFn>(kCampBusy)(reinterpret_cast<void*>(ui)) != 0;
}

// Why the menu cannot open over an event: the checks every menu makes, and nothing of the game's own
// menus up (the camp, the save point's menu) - only an event holds the controls.
const char* WhyNotEvent(uintptr_t ctx, uintptr_t holder, uintptr_t menu) {
    const char* why = WhyNotNow(ctx, holder, menu);
    if (why[0] != '\0') return why;
    if (At<uint32_t>(holder, kHolderBattle) != 0) return "a battle is running";
    if (!Valid(At<uint32_t>(holder, kHolderPlayer))) return "there is no party on the field";
    if ((At<uint32_t>(ctx, kCtxFlags) & kFlagCampUp) != 0) return "the camp menu is up";
    const uintptr_t camp = At<uint32_t>(holder, kHolderCamp);
    if (Valid(camp) && At<int32_t>(camp, kCampPage) != -1) return "the camp menu is open";
    const uintptr_t save_point = At<uint32_t>(holder, kHolderSavePoint);
    if (Valid(save_point) && At<int16_t>(save_point, 0x170) != 0) return "the save point's menu is open";
    return "";
}

void OpenEventMenu(uintptr_t menu) {
    g_route_now = kRouteDirect;
    g_phase_now = kPhaseCamp;
    InterlockedExchange(&g_done, 0);
    InterlockedExchange(&g_ok, 0);
    g_file[0] = '\0';
    g_frames = 0;
    g_last_menu_state = g_last_mgr_state = g_last_mgr_sub = -1;
    InterlockedExchange(&g_stage, kEventMenu);
    reinterpret_cast<MenuOpenFn>(kMenuOpen)(reinterpret_cast<void*>(menu), kPhaseCamp,
                                            reinterpret_cast<void*>(&OnMenuDone), nullptr);
    Log("battle_load: opened the load menu over the event (phase %d); the event gets no buttons meanwhile",
        kPhaseCamp);
}

// One field frame, before the control handler (which the holder runs before the camp's update, so a
// camp closed here never shows a frame of its own page).
void FieldFrame(uintptr_t holder, float dt) {
    const uintptr_t ctx = Ctx();
    const uintptr_t menu = SaveMenu();
    const uintptr_t camp = Valid(holder) ? At<uint32_t>(holder, kHolderCamp) : 0;
    switch (g_stage) {
    case kIdle: {
        if (g_enabled == 0 || g_field == 0) return;
        if (Valid(holder) && At<uint32_t>(holder, kHolderBattle) != 0) return;   // the battle hook's
        if (!TriggerPressed()) return;
        const char* why = WhyNotField(ctx, holder, menu);
        if (why[0] != '\0') {
            // The camp cannot open: over an event, the menu opens directly instead.
            const char* why_event = g_events != 0 && g_button_original != nullptr ? WhyNotEvent(ctx, holder, menu) : "Events=false";
            if (why_event[0] == '\0' && Valid(menu)) {
                OpenEventMenu(menu);
                return;
            }
            Log("battle_load: not now (field) - %s; over an event: %s", why, why_event);
            return;
        }
        if (reinterpret_cast<QueryFn>(kOpenCamp)(reinterpret_cast<void*>(holder)) == 0) {
            Log("battle_load: the game did not open the camp menu");
            return;
        }
        g_frames = 0;
        g_file[0] = '\0';
        InterlockedExchange(&g_stage, kCampOpening);
        Log("battle_load: opened the camp menu for its Load");
        return;
    }
    case kCampOpening:
        if (!Valid(camp) || At<int32_t>(camp, kCampPage) == -1) {
            Log("battle_load: the camp menu closed before its Load opened");
            InterlockedExchange(&g_stage, kIdle);
            return;
        }
        if (++g_frames < 2 || CampBusy(camp)) return;   // let "OpenCampMenu" play
        if (!Valid(menu) || At<uint32_t>(menu, kMenuState) != 0) {
            Log("battle_load: the save menu is not available - leaving the camp menu open");
            InterlockedExchange(&g_stage, kIdle);
            return;
        }
        g_frames = 0;
        g_last_menu_state = g_last_mgr_state = g_last_mgr_sub = -1;
        InterlockedExchange(&g_stage, kCampMenu);
        reinterpret_cast<MenuOpenFn>(kMenuOpen)(reinterpret_cast<void*>(menu), kPhaseCamp,
                                                reinterpret_cast<void*>(kCampLoadDone),
                                                reinterpret_cast<void*>(camp));
        Log("battle_load: opened the camp's Load (phase %d)", kPhaseCamp);
        return;
    case kCampMenu:
        if (Valid(menu) && At<uint32_t>(menu, kMenuState) != 0) {
            ++g_frames;
            WatchReadName();
            if (g_diagnostics != 0) LogStates(menu);
            return;
        }
        g_key_edge.Reset();
        g_chord_was_down = true;
        InterlockedExchange(&g_stage, kIdle);
        if (Valid(camp) && At<uint32_t>(camp, kCampLoaded) != 0) {
            Log("battle_load: %s is loaded; the camp menu takes it from here",
                g_file[0] != '\0' ? g_file : "the save");
            return;
        }
        if (Valid(camp) && At<int32_t>(camp, kCampPage) != -1) {
            reinterpret_cast<CloseCampFn>(kCloseCamp)(reinterpret_cast<void*>(camp));
        }
        Log("battle_load: the load menu was closed - back to the field");
        return;
    case kEventMenu: {
        if (!Valid(menu)) {
            Log("battle_load: the save menu went away - the event goes on");
            InterlockedExchange(&g_stage, kIdle);
            return;
        }
        ++g_frames;
        InterlockedExchange(&g_in_menu_update, 1);
        reinterpret_cast<MenuUpdateFn>(kMenuUpdate)(reinterpret_cast<void*>(menu), dt);
        InterlockedExchange(&g_in_menu_update, 0);
        WatchReadName();
        if (g_diagnostics != 0) LogStates(menu);
        if (At<uint32_t>(menu, kMenuState) != 0) return;
        g_key_edge.Reset();
        g_chord_was_down = true;
        if (g_done != 0 && g_ok != 0) {
            EnterLoadedSave(ctx, holder);
            InterlockedExchange(&g_stage, kLeaving);
            return;
        }
        InterlockedExchange(&g_stage, kIdle);
        if (g_file[0] != '\0') {
            Log("battle_load: the menu closed without success after reading %s - the event goes on, but "
                "the game state may no longer match it", g_file);
        } else {
            Log("battle_load: the load menu was closed - the event goes on");
        }
        return;
    }
    case kLeaving:
        // A load left for another scene; the field running again (no change pending) means it arrived
        // (or never happened) - either way the key is live again.
        if (Valid(ctx) && At<uint8_t>(ctx, kCtxSceneChange) == 0) InterlockedExchange(&g_stage, kIdle);
        return;
    default:
        return;
    }
}

void __attribute__((thiscall)) FieldControlDetour(void* self, float dt) {
    FieldFrame(reinterpret_cast<uintptr_t>(self), dt);
    g_field_original(self, dt);
}

// Every button query of the game. While the menu is open over an event, only the menu's own update is
// answered; the event (its message window, its scripts) is told nothing is pressed - and so consumes
// nothing the menu needs.
uint32_t __attribute__((thiscall)) ButtonQueryDetour(void* input, int button, int mode, char flag) {
    if (g_stage == kEventMenu && g_in_menu_update == 0) return 0;
    return g_button_original(input, button, mode, flag);
}

void __attribute__((thiscall)) ManagerUpdateDetour(void* self, float dt) {
    InterlockedIncrement(&g_manager_calls);
    g_manager_original(self, dt);
}

// The list's confirm (SetSubState(2) after SetSlot(index)). On the title route the pick is recorded
// and the menu told "cancel" instead, so nothing is read into the running battle's game state.
void __attribute__((thiscall)) SetSubStateDetour(void* self, int sub_state) {
    if (sub_state == kSubStateRead && g_stage == kMenu && g_route_now == kRouteTitle && !g_picked) {
        const uintptr_t mgr = reinterpret_cast<uintptr_t>(self);
        if (mgr == Manager(SaveMenu())) {
            const unsigned slot = At<uint32_t>(mgr, kMgrSlot) + At<uint32_t>(mgr, kMgrSlotBase);
            const bool autosave = At<uint8_t>(mgr, kMgrAutosave) != 0;
            NameSlot(slot, autosave);
            Put<uint8_t>(mgr, kMgrAutosave, 0);
            g_picked = true;
            Log("battle_load: picked %s (slot %u%s); cancelling the in-battle read, it happens at the title",
                g_file, slot, autosave ? ", autosave tab" : "");
            sub_state = kSubStateCancel;
        }
    }
    g_set_sub_state_original(self, sub_state);
}

void* g_hooks[5] = {};
unsigned g_hook_count = 0;

bool Hook(uintptr_t target, void* detour, void** original, const char* what) {
    void* handle = g_api->hook_create(reinterpret_cast<void*>(target), detour, original);
    if (handle == nullptr || *original == nullptr || g_api->hook_enable(handle) != 0) {
        LogError("battle_load: could not hook %s (0x%08x)", what, static_cast<unsigned>(target));
        return false;
    }
    if (g_hook_count < sizeof(g_hooks) / sizeof(g_hooks[0])) g_hooks[g_hook_count++] = handle;
    return true;
}

}  // namespace

extern "C" {

__declspec(dllexport) uint32_t __cdecl AtmtModInit(const AtmtModApi* api) {
    if (api == nullptr || api->version < ATMT_MOD_API_VERSION || api->size < sizeof(AtmtModApi)) return 0;
    if (api->hook_create == nullptr || api->hook_enable == nullptr) return 0;
    if (InterlockedCompareExchange(&g_started, 1, 0) != 0) {
        if (api->log != nullptr) api->log("battle_load: already initialised in this process, declining");
        return 0;
    }
    g_api = api;
    if (api->settings_register != nullptr) {
        api->settings_register(api, "Battle load", kSettings, sizeof(kSettings) / sizeof(kSettings[0]));
    }
    ParseInputs();
    if (g_enabled == 0) {
        Log("battle_load: disabled by its ini");
        return ATMT_MOD_API_VERSION;
    }
    // Every function this mod hooks or calls, by its first bytes in the supported ed8.exe (a bare
    // `push ebp; mov ebp,esp` would match almost any function of another build). SenPatcher (v1.3.1
    // and its current sources) patches none of them.
    static const atmt_code::Expect kExpected[] = {
        {kSetSubState, "\x55\x8b\xec\x8b\x45\x08\x89\x41\x58", 9, "the save manager's SetSubState"},
        {kManagerUpdate, "\x55\x8b\xec\x81\xec\x24\x01\x00\x00", 9, "the save manager's update"},
        {kBattleUpdate, "\x55\x8b\xec\x51\xa1\x98\xc5\xc7\x00", 9, "the battle update"},
        {kFieldControl, "\x55\x8b\xec\x81\xec\xbc\x00\x00\x00", 9, "the field control handler"},
        {kButtonQuery, "\x55\x8b\xec\x53\x8b\x5d\x08\x56\x8b\xf1", 10, "the button query"},
        {kOpenCamp, "\x56\x8b\xf1\x8b\x86\x74\x08\x00\x00", 9, "the camp menu's open"},
        {kCloseCamp, "\x56\x57\x6a\xff\x8b\xf1\xe8", 7, "the camp menu's close"},
        {kCampBusy, "\x8d\x81\xdc\x00\x00\x00\x50\xe8", 8, "the camp's busy check"},
        {kCampLoadDone, "\xe9\xb4\x28\x13\x00", 5, "the camp's load callback"},
        {kMenuOpen, "\x55\x8b\xec\x8b\x45\x08\xd9\x05", 8, "the save menu's open"},
        {kMenuUpdate, "\x55\x8b\xec\x83\xec\x08\xa1\x58", 8, "the save menu's update"},
        {kChangeScene, "\x55\x8b\xec\x56\x8b\xf1\x80\xbe", 8, "the scene change"},
        {kOverlayActive, "\x32\xc0\x66\x83\x79\x02\x00\x74", 8, "the overlay check"},
        {kMessageActive, "\x66\x83\xb9\x08\x4e\x0b\x00\x00", 8, "the message box check"},
        {kMessagePending, "\x55\x8b\xec\x8d\x81\x0c\x4e\x0b", 8, "the message input check"},
    };
    char why_not[256];
    if (!atmt_code::CheckAll(kExpected, why_not, sizeof(why_not))) {
        LogError("battle_load: %s - not installed", why_not);
        return ATMT_MOD_API_VERSION;
    }
    g_manager_hooked = Hook(kManagerUpdate, reinterpret_cast<void*>(&ManagerUpdateDetour),
                            reinterpret_cast<void**>(&g_manager_original), "the save manager's update");
    g_sub_state_hooked = Hook(kSetSubState, reinterpret_cast<void*>(&SetSubStateDetour),
                              reinterpret_cast<void**>(&g_set_sub_state_original),
                              "the save manager's SetSubState");
    if (!Hook(kBattleUpdate, reinterpret_cast<void*>(&BattleUpdateDetour),
              reinterpret_cast<void**>(&g_battle_original), "the battle update")) {
        return ATMT_MOD_API_VERSION;
    }
    if (!Hook(kFieldControl, reinterpret_cast<void*>(&FieldControlDetour),
              reinterpret_cast<void**>(&g_field_original), "the field control handler")) {
        Log("battle_load: the field part is off (battle only)");
    } else if (!Hook(kButtonQuery, reinterpret_cast<void*>(&ButtonQueryDetour),
                     reinterpret_cast<void**>(&g_button_original), "the button query")) {
        g_events = 0;   // without the filter the event would read the menu's buttons too
        Log("battle_load: the load menu during events is off");
    }
    Log("battle_load: ready - %s%s%s opens the load menu in a battle%s (route %s)",
        g_key_name[0] != '\0' ? g_key_name : "", (g_key_name[0] != '\0' && g_chord_name[0] != '\0') ? " / " : "",
        g_chord_name, g_field != 0 ? " and on the field" : "",
        kRouteTokens[g_route == kRouteTitle ? 1 : 0]);
    return ATMT_MOD_API_VERSION;
}

__declspec(dllexport) void __cdecl AtmtModShutdown(void) {
    // Only this mod's hooks: NULL would disable every mod's.
    if (g_api == nullptr || g_api->hook_disable == nullptr) return;
    for (unsigned i = 0; i < g_hook_count; ++i) g_api->hook_disable(g_hooks[i]);
}

// One sentence for the overlay and the manager (shared/mod_api.h, AtmtModDescription).
__declspec(dllexport) const char* __cdecl AtmtModDescription(void) {
    return "Opens a load menu in battle and on the field (F9 / BACK+START).";
}

}  // extern "C"
