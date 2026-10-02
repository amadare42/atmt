// app.h - the window's state: what the screens show and the work they start (docs/MANAGER.md).
//
// Long work (install, update, the update check) runs on one worker thread; its results come back
// to the UI thread through Post(), so the screens only ever touch the state from one thread.
#pragma once

#include <atomic>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "core/install.h"
#include "core/locator.h"
#include "core/payload.h"
#include "core/senpatcher.h"
#include "core/settings.h"
#include "core/updater.h"

union SDL_Event;

namespace atmt {

class App {
public:
    explicit App(const std::vector<std::string>& args);
    ~App();

    // One frame of the UI (inside an ImGui frame). Returns false when the app should quit.
    bool Frame(float width, float height);

    bool game_mode() const { return game_mode_; }
    bool busy() const { return busy_; }

    // An SDL event before ImGui sees it: true when the app took it (a key press while a key setting
    // is being recorded), and ImGui must not get it - so the press moves no focus and activates
    // nothing.
    bool HandleEvent(const SDL_Event& e);
    // The right stick scrolls what is under the pointer (before each ImGui frame); true while it is
    // held, so frames keep coming.
    bool PadScroll();

private:
    enum Tab { kStatus, kMods, kSettings, kPresets, kSystem, kTabCount };
    enum class Modal {
        None, PickGame, AckUnverified, ConfirmUninstall, ConfirmPurge, ConfirmRestore, ConfirmReset, ConfirmSteam,
        Conflict, FirstRun, Message
    };

    // ---- state
    void LoadPayload();
    void SetGameDir(const fs::path& dir);
    void RefreshStatus();
    void ReloadSettings();
    // Changes state.json (UpdateAppState: only the keys `change` sets) and refreshes state_.
    void ChangeState(const std::function<void(Json&)>& change);

    // ---- work on the worker thread
    void Run(const std::string& title, std::function<void()> work);
    // Runs `work` on the worker thread; what it Posts reaches the window once the worker is idle.
    void StartWorker(std::function<void()> work);
    void Post(std::function<void()> fn);
    void DrainPosted();
    void StartInstall(bool acknowledged, bool everything);
    void StartUninstall(bool purge);
    void StartIconPackBuild();
    void StartIconPackRemove();
    void RefreshSenPatcher();
    void StartSenPatcher(bool override_only);
    void StartSteamChange(bool add);
    void RefreshSteam();
    void StartUpdateCheck(bool force);
    void StartComponentUpdate();
    void StartAppUpdate();
    void ApplyQueuedIfIdle();

    // ---- screens (screens.cpp)
    void DrawStatus();
    void DrawIconPack();
    void DrawSenPatcher();
    void DrawMods();
    void DrawSettings();
    void DrawPresets();
    void DrawSystem();
    void DrawModals();
    void DrawBusy();
    void DrawHelpLine(float height);
    void DrawSettingRow(const SettingsGroup& g, const SettingDef& d);
    // key / pad_button settings are recorded, like the in-game settings bar does it (screens.cpp)
    bool DrawInputField(const SettingsGroup& g, const SettingDef& d, const std::string& value, const std::string& help,
                        std::string* fresh);
    void StartInputRecord(const SettingsGroup& g, const SettingDef& d, const std::string& value);
    void UpdateInputRecord();
    void FinishInputRecord(const std::string& value, bool store);
    void HandlePointerEvent(const SDL_Event& e);
    bool input_recording() const { return record_.phase == InputRecord::kArming || record_.phase == InputRecord::kListening; }
    void SaveSettings();
    void Help(const std::string& text);   // the focused/hovered item's help line
    bool BigButton(const char* label, bool enabled = true);
    void OpenModal(Modal m, const std::string& message = std::string());

    // payload and game
    Payload payload_;
    bool have_payload_ = false;
    std::string payload_error_;
    std::vector<GameInstall> games_;
    fs::path game_dir_;
    GameStatus status_;
    SenPatcherStatus senpatcher_;   // read on a game change and after SenPatcher work (it reads Steam's configs)
    double last_status_ = -100;
    bool game_running_ = false;
    bool was_running_ = false;

    // settings
    SettingsModel settings_;
    bool settings_loaded_ = false;
    size_t settings_group_ = 0;
    bool show_advanced_ = false;
    std::vector<std::string> conflicts_;

    // recording a key / pad_button setting: arming (the A / Enter that started it is let go), then
    // listening (the next key chord, or the next pad chord taken on its release), then releasing
    // (the app's own navigation comes back once nothing is held)
    struct InputRecord {
        enum Phase { kOff, kArming, kListening, kReleasing } phase = kOff;
        bool pad = false;
        std::string mod, section, key;
        std::string before;          // the value when it started: shown as "was" after
        bool done = false;           // a value was recorded (for the "was" note)
        double start = 0;
        unsigned pad_held = 0;       // the buttons held so far (bits of the recorder's table)
        int saved_nav = 0;           // the ImGui nav flags it turned off
    } record_;

    // updates
    UpdateCheck check_;
    bool checked_ = false;
    std::unique_ptr<Http> http_;
    std::string http_error_;

    // the Steam library entry
    bool in_steam_ = false;
    bool steam_add_ = true;   // what ConfirmSteam is about
    int steam_accounts_ = 0;

    // install everything: the unverified-build question is about this one
    bool ack_everything_ = false;

    // app state (state.json)
    Json state_;

    // the worker
    std::thread worker_;
    std::atomic<bool> busy_{false};
    std::string busy_title_;
    std::atomic<float> progress_{-1.0f};
    std::mutex posted_mutex_;
    std::vector<std::function<void()>> posted_;

    // the session log (every core message)
    std::mutex log_mutex_;
    std::deque<std::string> log_;
    std::string loader_log_;
    bool loader_log_loaded_ = false;

    // UI
    int tab_ = kStatus;
    int requested_tab_ = -1;
    Modal modal_ = Modal::None;
    bool modal_opened_ = false;
    std::string modal_message_;
    std::string restore_name_;
    std::string help_;
    std::string pick_path_;
    bool game_mode_ = false;
    // Steam's "Gamepad with Mouse Trackpad" layout on the Deck: the right trackpad moves the pointer
    // and its press arrives as R3. While the pointer is what the player uses (it moved last), R3 is
    // a left click; a pad button, a key or the left stick hand control back to the pad's focus.
    bool pointer_active_ = false;
    bool pad_click_down_ = false;
    bool quit_ = false;
};

}  // namespace atmt
