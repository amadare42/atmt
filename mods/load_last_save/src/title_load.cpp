// title_load.cpp - "-al" the way the title screen itself loads a save.
//
// Why this route exists (read out of ed8.exe on 2026-09-22, addresses are plain VAs, no ASLR):
//
// Every earlier route drove *parts* of a load - the save manager's fields (ManagerLoad), or the save
// menu's "arm the manager" step (FUN_0064CAD0, FlowLoad). Both get the file read into the game's own
// buffer and then stop, because neither is what enters the game. The pieces that do, in order:
//
//   title update   FUN_00690B30(this = title, float dt)       thiscall, ret 4, called once per frame
//                  [title+0x10] state: 2 = "press any button" (the logo screen), 3 = top menu,
//                  -1 = inactive. [title+0x14] = top-menu cursor, [title+0x1C] = "leaving the title"
//                  (-1 while it stays), [title+0x20] = press-any-button latch, [title+0x28] = sub-menu.
//                  While the save menu is open (menu+4 != 0) the title update only drives the menu.
//   "Load" entry   state 3, cursor 1, confirm (0x00691037):
//                      menu->Open(2, 0x0040896D, title)          -> FUN_0064A720
//   save menu      menu = *(*(g_ctx+0x7BC) + 0x5A80), g_ctx = *0x00C7C598
//                  Open(phase, done_cb, done_arg): thiscall, ret 0xC; menu+4 = 1, +0xC = phase,
//                  +0x14/+0x18 = the callback and its argument.
//                  Update (FUN_0064D2C0, switch on menu+4): 1 -> FUN_0064CAD0(phase) arms the save
//                  manager (vtable +0x64 = FUN_00484D00: kind 1, the game's buffer, cb1/cb2, arg =
//                  menu, state 1, slot 0, sub-state 0) -> 3: the slot list is shown while the manager
//                  waits in state 1 / sub-state 0 -> the list's confirm (0x0064D0D3) does
//                      if (autosave tab) manager+0x4C = 1;  manager->SetSlot(index) (+0x54);
//                      manager->SetSubState(2) (+0x58)
//                  -> the manager reads the file -> 4: phase 2 re-reads save511.dat -> 7: calls
//                  done_cb(ok, done_arg).
//   title's done   FUN_006900F0 (thunk 0x0040896D): on ok, title+0x1C = 1 and "FadeOutTitle";
//                  the title update then runs FUN_006905F0, which starts the game with the loaded data.
//
// So the title's phase is 2, not 1 (phase 1 is the in-game camp menu: its cb1 tears the running
// scene's tasks down), and the callback that enters the game is the *menu's* (+0x14), which none of
// the earlier routes installed - the menu was left with the startup's phase-5 call, whose callback is
// NULL. That is the whole reason every earlier load "read the file and stayed on the title".
//
// What this file does: it hooks the title update and, on the game's own thread between frames, drives
// the two state machines above - it starts the save menu's with the title's phase and callback (the
// same call the title makes), and advances the save manager's to "read slot N" as soon as the menu has
// armed it. No input is emulated, no screen is navigated; the title object is `this` of its own
// update, and the menu and manager are the game's own. Entering the session is the game's callback.
#include "al.h"
#include "mod_api.h"

#include <cstdarg>
#include <cstdio>

namespace al {

namespace {

// ---------------------------------------------------------------- the game's functions (thiscall)
typedef void(__attribute__((thiscall)) * TitleUpdateFn)(void* self, float dt);
typedef void(__attribute__((thiscall)) * MenuOpenFn)(void* menu, int phase, void* done_cb,
                                                       void* done_arg);
typedef unsigned char(__attribute__((thiscall)) * QueryFn)(void* self);
typedef unsigned char(__attribute__((thiscall)) * QueryArgFn)(void* self, int arg);

constexpr uintptr_t kTitleUpdate = 0x00690B30;       // FUN_00690B30, per-frame title update
constexpr uintptr_t kMenuOpen = 0x0064A720;          // save menu Open(phase, cb, arg)
constexpr uintptr_t kTitleLoadDone = 0x0040896D;     // thunk -> FUN_006900F0, the title's callback
constexpr uintptr_t kOverlayActive = 0x0063EEB0;     // (g_ctx+0xDE8): a full-screen overlay is up
constexpr uintptr_t kMessageActive = 0x00702580;     // (g_ctx): a message box is up
constexpr uintptr_t kMessagePending = 0x00702700;    // (g_ctx, 0): a message box waits for input
constexpr int kTitlePhase = 2;                       // what the title passes (0x0069103D)

constexpr unsigned kUiHolderOffset = 0x7BC;          // g_ctx+0x7BC -> the UI holder
constexpr unsigned kSaveMenuOffset = 0x5A80;         // holder+0x5A80 -> the save/load menu
constexpr unsigned kOtherMenuOffset = 0x5A88;        // holder+0x5A88 -> a menu the title also defers to

// The title object's fields (see the top of the file).
constexpr unsigned kTitleState = 0x10;
constexpr unsigned kTitleLeaving = 0x1C;
constexpr unsigned kTitleSubMenu = 0x28;
constexpr int kTitlePressAnyButton = 2;
constexpr int kTitleTopMenu = 3;
constexpr int kTitlePrompt = 5;                      // the title's own post-load prompt branch

// The save menu's and the save manager's fields.
constexpr unsigned kMenuState = 0x04;                // 0 = closed, 3 = showing the slot list
constexpr unsigned kMenuPhase = 0x0C;
constexpr unsigned kMenuDoneCb = 0x14;
constexpr unsigned kMenuDoneArg = 0x18;
constexpr unsigned kMgrKind = 0x04;
constexpr unsigned kMgrArg = 0x18;
constexpr unsigned kMgrSlotBase = 0x20;
constexpr unsigned kMgrAutosave = 0x4C;              // byte: name the file autosaveNN.dat
constexpr unsigned kMgrState = 0x50;
constexpr unsigned kMgrSlot = 0x54;
constexpr unsigned kMgrSubState = 0x58;

// Frames to let the title settle before starting the menu (it animates in). Kept at the minimum
// (1, not 0) so the state read in Tick() is never the same frame Quiet() last rejected.
constexpr unsigned kSettleFrames = 1;

enum Stage : LONG {
    kIdle = 0,         // not armed (no -al, or already finished)
    kWaitTitle,        // waiting for the title (logo screen or top menu) to be up and quiet
    kWaitArmed,        // save menu started; waiting for it to arm the save manager
    kLoading,          // manager told the slot; waiting for the menu to call the title back
    kDone,
};

const AtmtModApi* g_api = nullptr;
TitleUpdateFn g_title_original = nullptr;
volatile LONG g_stage = kIdle;
volatile LONG g_title_calls = 0;
volatile uintptr_t g_title = 0;
volatile LONG g_outcome = 0;         // TitleLoadOutcome
unsigned g_settle = 0;               // game thread only
unsigned g_index = 0;                // the manager's slot index to pick (slot - base is done live)
unsigned g_slot = kNoSlot;
bool g_autosave = false;
char g_name[64] = {0};
volatile DWORD g_start_tick = 0;   // GetTickCount() at ArmTitleLoad; 0 = not armed yet

// Every line carries "[+Nms]" against ArmTitleLoad's own clock, so the log alone answers "how long
// between these two screens" without cross-referencing a timestamp column by hand.
void Log(const char* fmt, ...) {
    if (g_api == nullptr || g_api->log == nullptr) return;
    char buf[400];
    int used = 0;
    const DWORD start = g_start_tick;
    if (start != 0) {
        const int n = _snprintf(buf, sizeof(buf) - 1, "[+%lums] ", GetTickCount() - start);
        if (n > 0) used = n;
    }
    va_list args;
    va_start(args, fmt);
    _vsnprintf(buf + used, sizeof(buf) - 1 - static_cast<size_t>(used), fmt, args);
    va_end(args);
    buf[sizeof(buf) - 1] = '\0';
    g_api->log(buf);
}

// A failure: written to atmt_loader.log unless LogLevel=off (Log is written with LogLevel=all only).
void LogError(const char* fmt, ...) {
    if (g_api == nullptr || g_api->log_error == nullptr) return;
    char buf[400];
    int used = 0;
    const DWORD start = g_start_tick;
    if (start != 0) {
        const int n = _snprintf(buf, sizeof(buf) - 1, "[+%lums] ", GetTickCount() - start);
        if (n > 0) used = n;
    }
    va_list args;
    va_start(args, fmt);
    _vsnprintf(buf + used, sizeof(buf) - 1 - static_cast<size_t>(used), fmt, args);
    va_end(args);
    buf[sizeof(buf) - 1] = '\0';
    g_api->log_error(buf);
}

template <typename T>
T At(uintptr_t base, unsigned offset) {
    return *reinterpret_cast<volatile const T*>(base + offset);
}

template <typename T>
void Put(uintptr_t base, unsigned offset, T value) {
    *reinterpret_cast<volatile T*>(base + offset) = value;
}

uintptr_t Ctx() {
    return *reinterpret_cast<volatile const uint32_t*>(kGameObjectPtr);
}

uintptr_t UiHolder() {
    const uintptr_t ctx = Ctx();
    return ctx < 0x10000 ? 0 : At<uint32_t>(ctx, kUiHolderOffset);
}

uintptr_t SaveMenu() {
    const uintptr_t holder = UiHolder();
    return holder < 0x10000 ? 0 : At<uint32_t>(holder, kSaveMenuOffset);
}

// The same checks the title update makes before it reads any input: nothing modal is up.
bool Quiet(uintptr_t title) {
    const uintptr_t ctx = Ctx();
    const uintptr_t holder = UiHolder();
    if (ctx < 0x10000 || holder < 0x10000) return false;
    const uintptr_t other = At<uint32_t>(holder, kOtherMenuOffset);
    if (other >= 0x10000 && At<uint32_t>(other, 8) != 0) return false;
    if (reinterpret_cast<QueryFn>(kOverlayActive)(reinterpret_cast<void*>(ctx + 0xDE8)) != 0) {
        return false;
    }
    if (reinterpret_cast<QueryFn>(kMessageActive)(reinterpret_cast<void*>(ctx)) != 0) return false;
    if (reinterpret_cast<QueryArgFn>(kMessagePending)(reinterpret_cast<void*>(ctx), 0) != 0) {
        return false;
    }
    return At<int32_t>(title, kTitleLeaving) == -1;
}

// The widget fade-in's shared duration (probe_fade.cpp confirmed it live: 0.2, i.e. 200ms, on every
// real load). It is .rdata - a compile-time float literal, not a runtime "current" value - so patching
// it means flipping the page writable, writing, and putting the page's own protection back rather than
// leaving it writable for the rest of the process. Scoped to this one load: patched right before the
// menu opens (so the load menu's own fade-in is quick), restored the moment Finish() runs for any
// outcome, so every other screen the player later opens still gets the normal 200ms.
// 0.001, not 0.0: dividing by this duration is how the widget animation code (FUN_0064A7D0) computes
// its progress, and an exact 0 is the kind of edge case that has crashed this game before (see al.h's
// "0 in both fields... died inside memset"). A millisecond is imperceptible and avoids the guess.
constexpr float kFadeDurationPatched = 0.001f;
volatile LONG g_fade_patched = 0;   // guards patch/restore against the CancelTitleLoad race below
float g_fade_saved_duration = 0.0f;

bool WriteFloatAt(uintptr_t address, float value) {
    DWORD old_protect = 0;
    if (!VirtualProtect(reinterpret_cast<void*>(address), sizeof(float), PAGE_READWRITE, &old_protect)) {
        return false;
    }
    *reinterpret_cast<volatile float*>(address) = value;
    DWORD ignore = 0;
    VirtualProtect(reinterpret_cast<void*>(address), sizeof(float), old_protect, &ignore);
    return true;
}

void PatchFadeDuration() {
    if (InterlockedCompareExchange(&g_fade_patched, 1, 0) != 0) return;   // already patched
    g_fade_saved_duration = *reinterpret_cast<const volatile float*>(kFadeDurationGlobal);
    if (WriteFloatAt(kFadeDurationGlobal, kFadeDurationPatched)) {
        Log("al: sped up the load menu's fade-in for this load (%.3f -> %.3f)", g_fade_saved_duration,
            kFadeDurationPatched);
    } else {
        InterlockedExchange(&g_fade_patched, 0);   // nothing changed - nothing to restore later
        LogError("al: could not speed up the load menu's fade-in (VirtualProtect refused) - unpatched");
    }
}

void RestoreFadeDuration() {
    if (InterlockedCompareExchange(&g_fade_patched, 0, 1) != 1) return;   // never patched, or already restored
    if (WriteFloatAt(kFadeDurationGlobal, g_fade_saved_duration)) {
        Log("al: restored the load menu's fade-in duration (%.3f)", g_fade_saved_duration);
    } else {
        LogError("al: could not restore the load menu's fade-in duration - it may stay fast until restart");
    }
}

void Finish(TitleLoadOutcome outcome) {
    RestoreFadeDuration();
    InterlockedExchange(&g_outcome, outcome);
    InterlockedExchange(&g_stage, kDone);
}

// One step of the sequence, on the game's thread, before the title's own update for this frame. Two
// state machines are driven, never the input handlers:
//   1. the save menu's (menu+4): Open(2, title's callback, title) puts it in state 1; its own update -
//      which the title update runs whenever the menu is open, in any title state, the logo screen
//      included - arms the save manager and waits in state 3;
//   2. the save manager's (manager+0x50/+0x58): the moment it is armed (state 1, sub-state 0, kind 1,
//      arg = the menu), the slot and sub-state 2 are written - the transition that issues the read.
// From there the game runs to the menu's state 7, which calls the title back.
void Tick(uintptr_t title) {
    const LONG stage = g_stage;
    if (stage == kIdle || stage == kDone) return;
    const int state = At<int32_t>(title, kTitleState);
    if (state == -1) return;
    const uintptr_t menu = SaveMenu();
    if (menu < 0x10000) return;
    const bool menu_open = At<uint32_t>(menu, kMenuState) != 0;

    switch (stage) {
    case kWaitTitle:
        // The logo screen (2) or the top menu (3), nothing modal, the startup's own phase-5 use of the
        // menu finished, and a moment for the screen to settle.
        if ((state != kTitlePressAnyButton && state != kTitleTopMenu) || menu_open
            || (state == kTitleTopMenu && At<int32_t>(title, kTitleSubMenu) != 0) || !Quiet(title)) {
            g_settle = 0;
            return;
        }
        if (++g_settle < kSettleFrames) return;
        PatchFadeDuration();   // before Open(), which is what starts the widget fade-in
        reinterpret_cast<MenuOpenFn>(kMenuOpen)(reinterpret_cast<void*>(menu), kTitlePhase,
                                                reinterpret_cast<void*>(kTitleLoadDone),
                                                reinterpret_cast<void*>(title));
        g_settle = 0;
        InterlockedExchange(&g_stage, kWaitArmed);
        Log("al: title (state %d): save menu state machine started - phase=%d cb=0x%08x arg=0x%08x",
            state, At<int32_t>(menu, kMenuPhase), At<uint32_t>(menu, kMenuDoneCb),
            At<uint32_t>(menu, kMenuDoneArg));
        return;

    case kWaitArmed: {
        if (!menu_open) {
            Log("al: title: the save menu closed before the manager was armed");
            Finish(kTitleLoadMenuClosed);
            return;
        }
        const uintptr_t mgr = *reinterpret_cast<volatile const uint32_t*>(kGameSaveManagerPtr);
        const bool armed = mgr >= 0x10000 && At<uint32_t>(mgr, kMgrState) == 1
                           && At<uint32_t>(mgr, kMgrSubState) == 0 && At<uint32_t>(mgr, kMgrKind) == 1
                           && At<uint32_t>(mgr, kMgrArg) == menu;
        if (!armed) return;
        const unsigned base = At<uint32_t>(mgr, kMgrSlotBase);
        if (g_slot < base) {
            Log("al: title: slot %u is below the manager's slot base %u - not loading", g_slot, base);
            Finish(kTitleLoadRefused);
            return;
        }
        g_index = g_slot - base;
        Put<uint8_t>(mgr, kMgrAutosave, g_autosave ? 1 : 0);
        Put<uint32_t>(mgr, kMgrSlot, g_index);
        Put<uint32_t>(mgr, kMgrSubState, 2);
        InterlockedExchange(&g_stage, kLoading);
        Log("al: title: save manager armed by the menu; set %s slot %u (index %u), sub-state 2", g_name,
            g_slot, g_index);
        return;
    }

    case kLoading:
        if (menu_open) return;
        // The menu reached state 7 and called its callback: the title either leaves (+0x1C) or shows
        // its own prompt (state 5).
        if (At<int32_t>(title, kTitleLeaving) != -1) {
            Log("al: title: the save menu reported success and the title is fading into the game");
            Finish(kTitleLoadEntered);
        } else if (state == kTitlePrompt) {
            Log("al: title: the load succeeded and the title shows its own follow-up prompt");
            Finish(kTitleLoadPrompt);
        } else {
            Log("al: title: the save menu closed without success (title state %d)", state);
            Finish(kTitleLoadFailed);
        }
        return;
    }
}

void __attribute__((thiscall)) TitleUpdateDetour(void* self, float dt) {
    InterlockedIncrement(&g_title_calls);
    const uintptr_t title = reinterpret_cast<uintptr_t>(self);
    if (title >= 0x10000) {
        g_title = title;
        Tick(title);
    }
    g_title_original(self, dt);
}

}  // namespace

bool InstallTitleHook(const AtmtModApi* api, std::string* why_not) {
    if (api == nullptr || api->hook_create == nullptr || api->hook_enable == nullptr) {
        if (why_not != nullptr) *why_not = "the loader has no hook service";
        return false;
    }
    g_api = api;
    void* trampoline = nullptr;
    void* handle = api->hook_create(reinterpret_cast<void*>(kTitleUpdate),
                                    reinterpret_cast<void*>(&TitleUpdateDetour), &trampoline);
    if (handle == nullptr || trampoline == nullptr) {
        if (why_not != nullptr) *why_not = "could not hook the title update (0x00690B30)";
        return false;
    }
    g_title_original = reinterpret_cast<TitleUpdateFn>(trampoline);
    if (api->hook_enable(handle) != 0) {
        // MinHook's MH_OK is 0; anything else means the detour is not live.
        if (why_not != nullptr) *why_not = "the title update hook could not be enabled";
        return false;
    }
    return true;
}

bool ArmTitleLoad(const std::string& file_name, std::string* why_not) {
    const SaveTarget target = ParseSaveTarget(file_name);
    if (target.slot == kNoSlot) {
        if (why_not != nullptr) *why_not = "not a save the game's load menu can name";
        return false;
    }
    g_slot = target.slot;
    g_autosave = target.autosave;
    _snprintf(g_name, sizeof(g_name) - 1, "%s", file_name.c_str());
    g_name[sizeof(g_name) - 1] = '\0';
    g_start_tick = GetTickCount();     // set before the first Log() call so it dates itself too
    InterlockedExchange(&g_outcome, kTitleLoadPending);
    InterlockedExchange(&g_stage, kWaitTitle);
    return true;
}

void CancelTitleLoad(TitleLoadOutcome outcome) {
    if (InterlockedCompareExchange(&g_stage, kDone, kDone) != kDone) Finish(outcome);
}

TitleLoadOutcome TitleLoadResult() {
    return static_cast<TitleLoadOutcome>(g_outcome);
}

const char* TitleLoadStageName() {
    switch (g_stage) {
    case kIdle: return "idle";
    case kWaitTitle: return "waiting for the title";
    case kWaitArmed: return "waiting for the menu to arm the manager";
    case kLoading: return "loading";
    case kDone: return "done";
    }
    return "?";
}

long TitleUpdateCalls() {
    return g_title_calls;
}

unsigned long TitleLoadStartTick() {
    return g_start_tick;
}

}  // namespace al
