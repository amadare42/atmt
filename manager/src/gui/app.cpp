// app.cpp - the window's state and the work it starts (see app.h); the screens are in screens.cpp.
#include "app.h"

#include <SDL.h>

#include "core/platform.h"
#include "core/steam_shortcuts.h"
#include "imgui.h"

namespace atmt {

namespace {

double Now() { return static_cast<double>(SDL_GetTicks64()) / 1000.0; }

}  // namespace

App::App(const std::vector<std::string>& args) {
    SetLog([this](const std::string& line) {
        std::lock_guard<std::mutex> lock(log_mutex_);
        log_.push_back(line);
        while (log_.size() > 2000) log_.pop_front();
        std::fputs((line + "\n").c_str(), stdout);
    });
    Log(std::string("ATMT Manager ") + AppVersion() + " (" + PlatformName() + ")");
    state_ = LoadAppState();
    game_mode_ = IsSteamGameMode();
    http_ = MakeHttp(&http_error_);
    LoadPayload();

    games_ = FindGames();
    const fs::path remembered = Path(state_.Str("game_dir"));
    if (!remembered.empty() && HasGameExe(remembered)) {
        SetGameDir(remembered);
    } else if (!games_.empty()) {
        SetGameDir(games_[0].dir);
    } else {
        OpenModal(Modal::PickGame);
    }
    if (games_.size() > 1 && remembered.empty()) OpenModal(Modal::PickGame);
    if (!state_["first_run_done"].AsBool(false) && PlatformName() == "linux" && modal_ == Modal::None) {
        OpenModal(Modal::FirstRun);
    }
    RefreshSteam();
    // on by default: at most once a day, quiet when it fails (System > Updates turns it off)
    if (state_["update"]["auto_check"].AsBool(true)) StartUpdateCheck(false);
    // --tab settings: open on that screen (a shortcut can point straight at the settings)
    static const char* const kNames[kTabCount] = {"status", "mods", "settings", "presets", "system"};
    for (size_t i = 0; i + 1 < args.size(); ++i) {
        if (args[i] != "--tab") continue;
        for (int t = 0; t < kTabCount; ++t) {
            if (IEquals(args[i + 1], kNames[t])) requested_tab_ = t;
        }
    }
}

App::~App() {
    if (worker_.joinable()) worker_.join();
    SetLog(nullptr);
}

// ---------------------------------------------------------------- state
void App::LoadPayload() {
    Payload p;
    std::string why;
    have_payload_ = FindBestPayload(&p, &why);
    if (have_payload_) {
        payload_ = std::move(p);
        payload_error_.clear();
        Log("payload: " + payload_.Summary());
        for (const Component& c : payload_.components) Log("  " + c.name + " " + c.version + " from " + U8(c.dir));
    } else {
        payload_error_ = why;
        Log("no payload: " + why);
    }
}

void App::SetGameDir(const fs::path& dir) {
    game_dir_ = dir;
    ChangeState([&](Json& s) { s["game_dir"] = U8(dir); });
    RefreshStatus();
    RefreshSenPatcher();
    ReloadSettings();
    loader_log_loaded_ = false;
}

void App::RefreshStatus() {
    if (game_dir_.empty()) return;
    const bool had_senpatcher = status_.senpatcher;
    status_ = Inspect(game_dir_, have_payload_ ? &payload_ : nullptr);
    if (status_.senpatcher != had_senpatcher) RefreshSenPatcher();
    game_running_ = status_.game_running;
    last_status_ = Now();
}

void App::ReloadSettings() {
    settings_loaded_ = false;
    if (game_dir_.empty() || !have_payload_) return;
    settings_.Load(game_dir_, Schema::FromJson(payload_.schema));
    settings_loaded_ = true;
    if (settings_group_ >= settings_.schema().groups.size()) settings_group_ = 0;
}

void App::ChangeState(const std::function<void(Json&)>& change) { state_ = UpdateAppState(change); }

// ---------------------------------------------------------------- the worker
namespace {
// On the worker thread: what its job posted, handed to the window when the job is over.
thread_local std::vector<std::function<void()>>* t_worker_posts = nullptr;
}  // namespace

void App::Run(const std::string& title, std::function<void()> work) {
    if (busy_) return;
    busy_title_ = title;
    progress_ = -1.0f;
    StartWorker(std::move(work));
}

void App::StartWorker(std::function<void()> work) {
    if (worker_.joinable()) worker_.join();
    busy_ = true;
    worker_ = std::thread([this, work]() {
        std::vector<std::function<void()>> posts;
        t_worker_posts = &posts;
        try {
            work();
        } catch (const std::exception& e) {
            const std::string what = e.what();
            Post([this, what]() { OpenModal(Modal::Message, "Something went wrong: " + what); });
        }
        t_worker_posts = nullptr;
        // Free before the results reach the window: a result that starts the next job (the
        // automatic install after an update check) must find the worker idle, or Run drops it.
        busy_ = false;
        std::lock_guard<std::mutex> lock(posted_mutex_);
        for (auto& fn : posts) posted_.push_back(std::move(fn));
    });
}

void App::Post(std::function<void()> fn) {
    if (t_worker_posts != nullptr) {
        t_worker_posts->push_back(std::move(fn));
        return;
    }
    std::lock_guard<std::mutex> lock(posted_mutex_);
    posted_.push_back(std::move(fn));
}

void App::DrainPosted() {
    std::vector<std::function<void()>> fns;
    {
        std::lock_guard<std::mutex> lock(posted_mutex_);
        fns.swap(posted_);
    }
    for (auto& fn : fns) fn();
}

void App::StartInstall(bool acknowledged, bool everything) {
    if (!have_payload_ || game_dir_.empty()) return;
    if (status_.verdict == ExeVerdict::Unverified && !status_.unverified_acknowledged && !acknowledged) {
        ack_everything_ = everything;
        OpenModal(Modal::AckUnverified);
        return;
    }
    const Payload payload = payload_;
    const fs::path game = game_dir_;
    const std::string action = everything ? "Installing everything" : ActionName(status_.SuggestedAction(&payload_));
    Run(std::string(action.empty() ? "Installing" : action) + " ...", [this, payload, game, acknowledged, everything]() {
        InstallOptions o;
        o.acknowledge_unverified = acknowledged;
        const InstallResult r = everything ? InstallEverything(game, payload, o) : Install(game, payload, o);
        Post([this, r]() {
            RefreshStatus();
            ReloadSettings();
            if (!r.ok) {
                OpenModal(Modal::Message, (r.rolled_back ? "Nothing was changed - the install was rolled back.\n\n" : "")
                                              + r.error);
            } else {
                std::string notes;
                for (const std::string& n : r.notes) notes += "\n" + n;
                OpenModal(Modal::Message, "Installed " + payload_.Summary() + "." + notes
                                              + "\n\nA backup of the previous state is in " + U8(r.backup_dir.filename()) + ".");
            }
        });
    });
}

void App::StartUninstall(bool purge) {
    const fs::path game = game_dir_;
    Run(purge ? "Removing everything ..." : "Uninstalling ...", [this, game, purge]() {
        UninstallOptions o;
        o.purge = purge;
        const InstallResult r = Uninstall(game, o);
        Post([this, r, purge]() {
            RefreshStatus();
            ReloadSettings();
            if (!r.ok) {
                OpenModal(Modal::Message, r.error);
            } else if (purge) {
                OpenModal(Modal::Message, "Everything of the mods is gone: the game folder is as the store installed it.");
            } else {
                OpenModal(Modal::Message, "Uninstalled. Everything removed is in " + U8(r.backup_dir.filename()) + ".");
            }
        });
    });
}

void App::StartIconPackBuild() {
    if (!have_payload_ || game_dir_.empty() || game_running_) return;
    const fs::path game = game_dir_;
    const IconPackConfig config = payload_.icon_pack;
    Run("Building the icon pack ...", [this, game, config]() {
        Log("building the icon pack from the textures in use ...");
        const IconPackResult r = BuildIconPackByPlayer(game, config);
        Post([this, r]() {
            RefreshStatus();
            if (!r.ok) {
                OpenModal(Modal::Message, "The icon pack was not built: " + r.error);
            } else if (r.packages == 0) {
                OpenModal(Modal::Message, "Nothing to pre-shrink: the textures in use are small enough, so no icon pack is needed.");
            } else {
                OpenModal(Modal::Message, "The icon pack is built from the textures in use. It is loaded on the game's next start.");
            }
        });
    });
}

void App::StartIconPackRemove() {
    if (game_dir_.empty() || game_running_) return;
    const fs::path game = game_dir_;
    Run("Removing the icon pack ...", [this, game]() {
        std::string why;
        const bool ok = RemoveIconPackByPlayer(game, &why);
        Log(ok ? std::string("the icon pack is removed") : "the icon pack could not be removed: " + why);
        Post([this, ok, why]() {
            RefreshStatus();
            OpenModal(Modal::Message, ok ? std::string("The icon pack is removed, and installs leave it out from now on. "
                                                       "\"Build icon pack\" adds it back.")
                                         : "The icon pack could not be removed: " + why);
        });
    });
}

void App::RefreshSenPatcher() {
    if (game_dir_.empty()) return;
    senpatcher_ = GetSenPatcherStatus(DescribeGameDir(game_dir_, games_));
}

void App::StartSenPatcher(bool override_only) {
    if (game_dir_.empty() || game_running_) return;
    const fs::path game = game_dir_;
    Http* http = http_.get();
    Run(override_only ? "Setting the dll override in the game's Proton prefix ..."
                      : "SenPatcher - press \"Patch game\" for Trails of Cold Steel there, then close it",
        [this, game, http, override_only]() {
            const SenPatcherRun r = RunSenPatcher(game, http, override_only, [this](uint64_t now, uint64_t total) {
                progress_ = total > 0 ? static_cast<float>(now) / static_cast<float>(total) : -1.0f;
                return true;
            });
            progress_ = -1.0f;
            for (const std::string& n : r.notes) Log(n);
            Post([this, r, override_only]() {
                RefreshStatus();
                RefreshSenPatcher();
                if (!r.ok) {
                    OpenModal(Modal::Message, "SenPatcher: " + r.error);
                    return;
                }
                std::string text;
                if (override_only) {
                    text = "The dll override dinput8=native,builtin is set in the game's Proton prefix: Proton now loads "
                           "SenPatcher's DINPUT8.dll.";
                } else if (r.installed) {
                    text = "SenPatcher " + r.tag + " is set up: it runs from the game's next start (its version shows on the "
                           "title screen).";
                    if (status_.icon_pack.CanBuild() && status_.icon_pack.state == IconPackState::NotBuilt) {
                        text += "\n\nThe icon pack can be built now (Status > Icon pack).";
                    }
                } else {
                    text = "SenPatcher was closed without patching the game.";
                }
                for (const std::string& n : r.notes) {
                    if (n.find("dll override") == std::string::npos) text += "\n\n" + n;
                }
                OpenModal(Modal::Message, text);
            });
        });
}

void App::RefreshSteam() {
    in_steam_ = false;
    steam_accounts_ = 0;
    for (const SteamAccount& a : SteamAccounts()) {
        ++steam_accounts_;
        in_steam_ = in_steam_ || a.has_shortcut;
    }
}

void App::StartSteamChange(bool add) {
    Run(add ? "Adding to the Steam library ..." : "Removing from the Steam library ...", [this, add]() {
        std::string why;
        const bool restart = SteamRunning();
        if (restart && !CloseSteam(60000, &why)) {
            Post([this, why]() { OpenModal(Modal::Message, why); });
            return;
        }
        int n = 0;
        const bool ok = add ? AddToSteam(SelfExe(), WriteAppIcon(), &n, &why) : RemoveFromSteam(&n, &why);
        if (restart) StartSteam();
        Post([this, ok, why, add]() {
            RefreshSteam();
            if (!ok) {
                OpenModal(Modal::Message, why);
            } else if (add) {
                OpenModal(Modal::Message, "ATMT Manager is in your Steam library (under Non-Steam).\n\nIn Game Mode its "
                                          "controller layout is \"Gamepad with Mouse Trackpad\": the right trackpad moves "
                                          "the pointer and pressing it clicks, the right stick scrolls.");
            } else {
                OpenModal(Modal::Message, "ATMT Manager was removed from your Steam library.");
            }
        });
    });
}

void App::StartUpdateCheck(bool force) {
    if (busy_) return;
    const std::map<std::string, std::string> local = have_payload_ ? payload_.Versions() : std::map<std::string, std::string>();
    Http* http = http_.get();
    // The check itself is quiet (no busy screen): a failure is a line in System, nothing more.
    busy_title_.clear();
    StartWorker([this, force, local, http]() {
        Updater updater(http);
        const UpdateCheck c = updater.Check(force, local);
        Post([this, c, force]() {
            state_ = LoadAppState();   // the check wrote its result (and last_error) there
            check_ = c;
            checked_ = true;
            if (c.status == UpdateCheck::Status::Failed && force) Log(c.message);
            // automatic installation, when the user asked for it (default: ask)
            if (!c.Installable().empty() && state_["update"]["auto_install"].AsBool(false) && status_.installed
                && !game_running_) {
                Log("update: installing the newer components automatically (System > Updates)");
                StartComponentUpdate();
            }
        });
    });
}

void App::StartComponentUpdate() {
    const std::vector<ComponentUpdate> todo = check_.Installable();
    const fs::path game = game_dir_;
    const bool install_after = status_.installed || status_.record_present;
    Http* http = http_.get();
    std::vector<std::string> names;
    for (const ComponentUpdate& u : todo) names.push_back(u.remote.title + " " + u.remote.version);
    const std::string what = Join(names, ", ");
    Run("Updating " + what + " ...", [this, todo, what, game, install_after, http]() {
        Updater updater(http);
        std::string why;
        if (!todo.empty() && !updater.DownloadComponents(todo, &why, [this](uint64_t now, uint64_t total) {
                progress_ = total > 0 ? static_cast<float>(now) / static_cast<float>(total) : -1.0f;
                return true;
            })) {
            Post([this, why]() {
                LoadPayload();   // the components downloaded before the failure are used
                RefreshStatus();
                OpenModal(Modal::Message, "The update was not installed:\n\n" + why);
            });
            return;
        }
        Payload fresh;
        if (!FindBestPayload(&fresh, &why)) {
            Post([this, why]() { OpenModal(Modal::Message, why); });
            return;
        }
        // If the game is running the update waits for it to close: the components are kept, and
        // the Status screen offers the install once it has exited.
        InstallResult r;
        const bool running = GameRunning();
        if (install_after && !running) {
            progress_ = -1.0f;
            InstallOptions o;
            r = Install(game, fresh, o);
        }
        Post([this, fresh, r, what, install_after, running]() {
            payload_ = fresh;
            have_payload_ = true;
            check_.Compare(payload_.Versions());
            RefreshStatus();
            ReloadSettings();
            if (!install_after) {
                OpenModal(Modal::Message, what + " downloaded - Install it from the Status screen.");
            } else if (running) {
                OpenModal(Modal::Message, what + " downloaded. Close the game, then press Update on the Status screen.");
            } else if (!r.ok) {
                OpenModal(Modal::Message, (r.rolled_back ? "Nothing was changed - the update was rolled back.\n\n" : "") + r.error);
            } else {
                OpenModal(Modal::Message, "Updated: " + what + ".");
            }
        });
    });
}

void App::StartAppUpdate() {
    const Manifest manifest = check_.manifest;
    Http* http = http_.get();
    Run("Downloading the app " + manifest.manager_version + " ...", [this, manifest, http]() {
        Updater updater(http);
        std::string why;
        const bool ok = updater.UpdateApp(manifest, &why, [this](uint64_t now, uint64_t total) {
            progress_ = total > 0 ? static_cast<float>(now) / static_cast<float>(total) : -1.0f;
            return true;
        });
        Post([this, ok, why, manifest]() {
            if (ok) {
                check_.app_newer = false;
                state_ = LoadAppState();
                OpenModal(Modal::Message, "The app was updated to " + manifest.manager_version
                                              + ". It is used from the next start - close and start it again.");
            } else {
                OpenModal(Modal::Message, "The app was not updated: " + why
                                              + (manifest.page.empty() ? "" : "\n\nRelease page: " + manifest.page));
            }
        });
    });
}

void App::ApplyQueuedIfIdle() {
    if (game_running_ || game_dir_.empty() || !have_payload_ || !HasQueuedSettings(game_dir_)) return;
    std::string why;
    const int n = ApplyQueuedSettings(game_dir_, Schema::FromJson(payload_.schema), &why);
    if (n < 0) Log("settings: the queued changes could not be written: " + why);
    settings_.Reload();
}

// ---------------------------------------------------------------- the frame
bool App::Frame(float width, float height) {
    DrainPosted();
    const double now = Now();
    if (!busy_ && now - last_status_ > 2.0) {
        RefreshStatus();
        if (was_running_ && !game_running_) ApplyQueuedIfIdle();
        was_running_ = game_running_;
        // a mod in the game (or a person) changed an ini: show it, unless there are edits of ours
        if (settings_loaded_ && !settings_.Dirty() && settings_.ChangedOnDisk()) settings_.Reload();
    }
    help_.clear();
    UpdateInputRecord();

    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(ImVec2(width, height));
    ImGui::Begin("##main", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings
                     | ImGuiWindowFlags_NoBringToFrontOnFocus);

    // shoulder buttons switch tabs (and Ctrl+Tab on a keyboard)
    if (modal_ == Modal::None && !ImGui::IsAnyItemActive() && !busy_ && record_.phase == InputRecord::kOff) {
        if (ImGui::IsKeyPressed(ImGuiKey_GamepadL1, false)) requested_tab_ = (tab_ + kTabCount - 1) % kTabCount;
        if (ImGui::IsKeyPressed(ImGuiKey_GamepadR1, false)) requested_tab_ = (tab_ + 1) % kTabCount;
    }

    const float help_height = ImGui::GetFrameHeightWithSpacing() * 2.0f;
    static const char* const kTabNames[kTabCount] = {"Status", "Mods", "Settings", "Presets", "System"};
    if (ImGui::BeginTabBar("##tabs", ImGuiTabBarFlags_FittingPolicyShrink)) {
        for (int i = 0; i < kTabCount; ++i) {
            const ImGuiTabItemFlags flags = requested_tab_ == i ? ImGuiTabItemFlags_SetSelected : 0;
            std::string label = std::string("  ") + kTabNames[i] + "  ";
            if (i == kSettings && settings_.Dirty()) label = "  Settings *  ";
            if (ImGui::BeginTabItem((label + "###tab" + std::to_string(i)).c_str(), nullptr, flags)) {
                tab_ = i;
                ImGui::BeginChild("##page", ImVec2(0, -help_height), ImGuiChildFlags_None);
                switch (i) {
                    case kStatus: DrawStatus(); break;
                    case kMods: DrawMods(); break;
                    case kSettings: DrawSettings(); break;
                    case kPresets: DrawPresets(); break;
                    default: DrawSystem(); break;
                }
                ImGui::EndChild();
                ImGui::EndTabItem();
            }
        }
        ImGui::EndTabBar();
    }
    requested_tab_ = -1;
    DrawHelpLine(help_height);
    DrawModals();
    DrawBusy();
    ImGui::End();
    return !quit_;
}

}  // namespace atmt
