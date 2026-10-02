// screens.cpp - Status, Mods, Settings, Presets, System, and the dialogs (docs/MANAGER.md). Everything is reachable with a pad: d-pad/stick to move, A to select, B to back out,
// LB/RB for the tabs; nothing is hover-only (the help line shows the focused item's text).
#include <SDL.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>

#include "app.h"
#include "atmt_theme.h"
#include "core/platform.h"
#include "core/steam_shortcuts.h"
#include "imgui.h"
#include "imgui_internal.h"   // ImGui::GetKeyData (the right stick's analog values)

namespace atmt {

namespace {

const ImVec4 kGood(0.45f, 0.85f, 0.45f, 1.0f);
const ImVec4 kWarn(0.98f, 0.70f, 0.25f, 1.0f);
const ImVec4 kBad(0.95f, 0.40f, 0.35f, 1.0f);

void Heading(const char* text) {
    ImGui::Spacing();
    ImGui::PushStyleColor(ImGuiCol_Text, theme::kTitle);
    ImGui::TextUnformatted(text);
    ImGui::PopStyleColor();
    ImGui::Separator();
}

void Row(const char* label, const std::string& value, const ImVec4* color = nullptr) {
    ImGui::TextDisabled("%s", label);
    ImGui::SameLine(ImGui::GetFontSize() * 9.0f);
    if (color != nullptr) ImGui::PushStyleColor(ImGuiCol_Text, *color);
    ImGui::TextWrapped("%s", value.c_str());
    if (color != nullptr) ImGui::PopStyleColor();
}

void Banner(const ImVec4& color, const std::string& text) {
    ImGui::PushStyleColor(ImGuiCol_Text, color);
    ImGui::TextWrapped("%s", text.c_str());
    ImGui::PopStyleColor();
}

// ---------------------------------------------------------------- recording key / pad settings
// Like the in-game settings bar (mods/overlay, SettingsBar::StepPick): A / Enter on the field, let
// go, then the next key chord (the modifiers held and one other key) or the next pad chord (the
// buttons held together, taken when they are all released). Esc, a click elsewhere or the timeout
// gives up; Clear sets none.
constexpr double kRecordTimeout = 8.0;

// The pad's buttons as ImGui reads them (the SDL backend fills these from SDL_GameController
// whatever the nav flags), named the way atmt::PadButtonName names XInput's, in PadChordName's order.
struct PadKey {
    ImGuiKey key;
    const char* name;
};
const PadKey kPadKeys[] = {
    {ImGuiKey_GamepadFaceDown, "A"},    {ImGuiKey_GamepadFaceRight, "B"}, {ImGuiKey_GamepadFaceLeft, "X"},
    {ImGuiKey_GamepadFaceUp, "Y"},      {ImGuiKey_GamepadL1, "LB"},       {ImGuiKey_GamepadR1, "RB"},
    {ImGuiKey_GamepadL3, "L3"},         {ImGuiKey_GamepadR3, "R3"},       {ImGuiKey_GamepadBack, "BACK"},
    {ImGuiKey_GamepadStart, "START"},   {ImGuiKey_GamepadDpadUp, "UP"},   {ImGuiKey_GamepadDpadDown, "DOWN"},
    {ImGuiKey_GamepadDpadLeft, "LEFT"}, {ImGuiKey_GamepadDpadRight, "RIGHT"},
};
constexpr int kPadKeyCount = static_cast<int>(sizeof(kPadKeys) / sizeof(kPadKeys[0]));

unsigned PadKeysDown() {
    unsigned bits = 0;
    for (int i = 0; i < kPadKeyCount; ++i) {
        if (ImGui::IsKeyDown(kPadKeys[i].key)) bits |= 1u << i;
    }
    return bits;
}

std::vector<std::string> PadKeyNames(unsigned bits) {
    std::vector<std::string> names;
    for (int i = 0; i < kPadKeyCount; ++i) {
        if ((bits & (1u << i)) != 0) names.push_back(kPadKeys[i].name);
    }
    return names;
}

// Anything that could have started the recording (or would act the moment it ends) still down.
bool AnythingHeld() {
    return PadKeysDown() != 0 || ImGui::IsKeyDown(ImGuiKey_Enter) || ImGui::IsKeyDown(ImGuiKey_KeypadEnter)
           || ImGui::IsKeyDown(ImGuiKey_Space) || ImGui::IsAnyMouseDown();
}

// The modifiers held, by the names atmt_input.h gives them ("Ctrl", "Shift", "Alt", "Win").
std::vector<std::string> ModifierNames(Uint16 mod) {
    std::vector<std::string> names;
    if ((mod & KMOD_CTRL) != 0) names.push_back("Ctrl");
    if ((mod & KMOD_SHIFT) != 0) names.push_back("Shift");
    if ((mod & KMOD_ALT) != 0) names.push_back("Alt");
    if ((mod & KMOD_GUI) != 0) names.push_back("Win");
    return names;
}

// An SDL key -> the name atmt::KeyFromName reads as the same Windows virtual key (atmt::KeyName's
// spelling); empty for a modifier on its own or a key the mods have no name for. Letters and digits
// go by the layout (like a virtual key does), the rest by position.
std::string SdlKeyName(const SDL_Keysym& k) {
    const SDL_Scancode sc = k.scancode;
    if (sc >= SDL_SCANCODE_KP_1 && sc <= SDL_SCANCODE_KP_9) return "Numpad" + std::to_string(sc - SDL_SCANCODE_KP_1 + 1);
    switch (sc) {
        case SDL_SCANCODE_KP_0: return "Numpad0";
        case SDL_SCANCODE_KP_MULTIPLY: return "Multiply";
        case SDL_SCANCODE_KP_PLUS: return "Add";
        case SDL_SCANCODE_KP_MINUS: return "Subtract";
        case SDL_SCANCODE_KP_PERIOD: return "Decimal";
        case SDL_SCANCODE_KP_DIVIDE: return "Divide";
        case SDL_SCANCODE_KP_ENTER: return "Enter";
        default: break;
    }
    if (k.sym >= SDLK_a && k.sym <= SDLK_z) return std::string(1, static_cast<char>('A' + (k.sym - SDLK_a)));
    if (k.sym >= SDLK_0 && k.sym <= SDLK_9) return std::string(1, static_cast<char>('0' + (k.sym - SDLK_0)));
    if (sc >= SDL_SCANCODE_A && sc <= SDL_SCANCODE_Z) return std::string(1, static_cast<char>('A' + (sc - SDL_SCANCODE_A)));
    if (sc >= SDL_SCANCODE_1 && sc <= SDL_SCANCODE_9) return std::string(1, static_cast<char>('1' + (sc - SDL_SCANCODE_1)));
    if (sc == SDL_SCANCODE_0) return "0";
    if (sc >= SDL_SCANCODE_F1 && sc <= SDL_SCANCODE_F12) return "F" + std::to_string(sc - SDL_SCANCODE_F1 + 1);
    if (sc >= SDL_SCANCODE_F13 && sc <= SDL_SCANCODE_F24) return "F" + std::to_string(sc - SDL_SCANCODE_F13 + 13);
    switch (sc) {
        case SDL_SCANCODE_TAB: return "Tab";
        case SDL_SCANCODE_BACKSPACE: return "Backspace";
        case SDL_SCANCODE_RETURN: return "Enter";
        case SDL_SCANCODE_SPACE: return "Space";
        case SDL_SCANCODE_PAGEUP: return "PageUp";
        case SDL_SCANCODE_PAGEDOWN: return "PageDown";
        case SDL_SCANCODE_END: return "End";
        case SDL_SCANCODE_HOME: return "Home";
        case SDL_SCANCODE_LEFT: return "Left";
        case SDL_SCANCODE_UP: return "Up";
        case SDL_SCANCODE_RIGHT: return "Right";
        case SDL_SCANCODE_DOWN: return "Down";
        case SDL_SCANCODE_INSERT: return "Insert";
        case SDL_SCANCODE_DELETE: return "Delete";
        case SDL_SCANCODE_GRAVE: return "Grave";
        case SDL_SCANCODE_MINUS: return "Minus";
        case SDL_SCANCODE_EQUALS: return "Equals";
        case SDL_SCANCODE_LEFTBRACKET: return "LBracket";
        case SDL_SCANCODE_RIGHTBRACKET: return "RBracket";
        case SDL_SCANCODE_BACKSLASH: return "Backslash";
        case SDL_SCANCODE_SEMICOLON: return "Semicolon";
        case SDL_SCANCODE_APOSTROPHE: return "Quote";
        case SDL_SCANCODE_COMMA: return "Comma";
        case SDL_SCANCODE_PERIOD: return "Period";
        case SDL_SCANCODE_SLASH: return "Slash";
        default: return std::string();
    }
}

}  // namespace

// ---------------------------------------------------------------- shared bits
void App::Help(const std::string& text) {
    if (!text.empty() && (ImGui::IsItemHovered() || ImGui::IsItemFocused())) help_ = text;
}

bool App::BigButton(const char* label, bool enabled) {
    ImGui::BeginDisabled(!enabled);
    const float pad = ImGui::GetFontSize() * 0.6f;
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(pad * 2.0f, pad));
    const bool pressed = ImGui::Button(label);
    ImGui::PopStyleVar();
    ImGui::EndDisabled();
    return pressed;
}

void App::OpenModal(Modal m, const std::string& message) {
    modal_ = m;
    modal_message_ = message;
    modal_opened_ = false;
    if (m == Modal::PickGame) pick_path_ = U8(game_dir_);
}

void App::DrawHelpLine(float height) {
    ImGui::Separator();
    ImGui::BeginChild("##help", ImVec2(0, height - ImGui::GetStyle().ItemSpacing.y), ImGuiChildFlags_None,
                      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoNav);
    if (!help_.empty()) {
        std::string one = help_;
        for (char& c : one) {
            if (c == '\n') c = ' ';
        }
        ImGui::PushStyleColor(ImGuiCol_Text, theme::kHint);
        ImGui::TextWrapped("%s", one.c_str());
        ImGui::PopStyleColor();
    } else {
        ImGui::TextDisabled("LB / RB  switch screens     A  select     B  back     %s", status_.game_running ? "-  the game is running" : "");
    }
    ImGui::EndChild();
}

void App::DrawBusy() {
    if (!busy_ || busy_title_.empty()) return;
    ImGui::OpenPopup("##busy");
    const ImVec2 center = ImGui::GetMainViewport()->GetCenter();
    ImGui::SetNextWindowPos(center, ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(ImGui::GetMainViewport()->Size.x * 0.6f, 0));
    if (ImGui::BeginPopupModal("##busy", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextUnformatted(busy_title_.c_str());
        const float p = progress_;
        if (p >= 0.0f) {
            ImGui::ProgressBar(p, ImVec2(-1, 0));
        } else {
            ImGui::ProgressBar(-1.0f * static_cast<float>(ImGui::GetTime()), ImVec2(-1, 0), "");
        }
        std::lock_guard<std::mutex> lock(log_mutex_);
        if (!log_.empty()) ImGui::TextDisabled("%s", log_.back().c_str());
        if (!busy_) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
}

// ---------------------------------------------------------------- Status
void App::DrawStatus() {
    Heading("Game");
    Row("Folder", game_dir_.empty() ? std::string("not found") : U8(game_dir_));
    ImGui::SameLine();
    if (ImGui::SmallButton("Change...")) OpenModal(Modal::PickGame);
    Help("pick another install of the game, or a folder by hand");
    if (game_dir_.empty()) return;
    const GameInstall g = DescribeGameDir(game_dir_, games_);
    if (!g.source.empty()) Row("From", g.source);
    if (PlatformName() == "linux" && !g.prefix.empty()) {
        Row(g.IsSteam() ? "Proton prefix" : "Wine prefix",
            U8(g.prefix) + (g.prefix_exists ? "" : "  (created when the game first starts)"));
    }
    switch (status_.verdict) {
        case ExeVerdict::Supported: Row("ed8.exe", "supported game build", &kGood); break;
        case ExeVerdict::Unverified:
            Row("ed8.exe", "Unverified game build (md5 " + status_.exe_md5 + ")", &kWarn);
            Help("the mods were checked against another ed8.exe (a game patch, or a GOG build); it can crash "
                 "them or hide features. "
                 "Installing still works, and Uninstall restores everything.");
            if (status_.old_senpatcher) Help(kOldSenPatcherHint);
            break;
        default: Row("ed8.exe", "missing", &kBad); break;
    }
    if (status_.game_running) Banner(kWarn, "The game is running: installed files and settings are used from its next start.");

    Heading("Mods");
    Row("Installed", status_.installed ? status_.record.Str("summary", "yes (by a developer script)") : std::string("no"));
    Row("This app has", have_payload_ ? payload_.Summary() : "no payload: " + payload_error_);
    if (have_payload_ && (status_.installed || status_.record_present)) {
        for (const std::string& o : status_.Outdated(payload_)) Row("Newer here", o);
    }
    if (HasQueuedSettings(game_dir_)) Banner(kWarn, "Settings changes are queued: they are written when the game has exited.");
    DrawIconPack();
    DrawSenPatcher();

    if (checked_ && check_.status == UpdateCheck::Status::Available) {
        ImGui::Spacing();
        for (const ComponentUpdate& u : check_.components) {
            Banner(u.needs_newer_app ? kWarn : theme::kName,
                   u.remote.title + " " + u.remote.version + (u.local_version.empty() ? " (new)" : " (this app has " + u.local_version + ")")
                       + (u.needs_newer_app ? " - needs a newer app first (System > Updates)" : ""));
        }
        if (check_.app_newer) Banner(theme::kName, "The app " + check_.manifest.manager_version + " is available.");
        if (ImGui::TreeNode("What's new")) {
            if (check_.app_newer && !check_.manifest.manager_changelog.empty()) {
                ImGui::TextWrapped("The app %s:\n%s", check_.manifest.manager_version.c_str(), check_.manifest.manager_changelog.c_str());
            }
            for (const ComponentUpdate& u : check_.components) {
                if (!u.remote.changelog.empty()) ImGui::TextWrapped("%s %s:\n%s", u.remote.title.c_str(), u.remote.version.c_str(), u.remote.changelog.c_str());
            }
            ImGui::TreePop();
        }
        if (!check_.Installable().empty()) {
            if (BigButton("Download and install the updates", !busy_)) StartComponentUpdate();
            Help("downloads the newer components, checks them against the signed manifest, backs up, installs and verifies");
        } else if (check_.app_newer) {
            if (BigButton("Update the app", !busy_)) StartAppUpdate();
        }
    }

    for (const std::string& h : status_.hard_stops) Banner(kBad, h);
    for (const std::string& p : status_.problems) Banner(kWarn, p);

    ImGui::Spacing();
    const GameStatus::Action action = status_.SuggestedAction(have_payload_ ? &payload_ : nullptr);
    if (action != GameStatus::Action::None) {
        // The one button: everything on, the settings recommended for this system, nothing to decide.
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(theme::kRule.x, theme::kRule.y, theme::kRule.z, 0.55f));
        if (BigButton("Install everything (recommended)", !busy_)) StartInstall(false, true);
        ImGui::PopStyleColor();
        Help("every mod on and the settings recommended for this system - backed up first, verified, rolled back on a failure");
        std::vector<std::string> recommended, off;
        for (const Preset& p : payload_.presets) {
            if (std::find(p.recommended_on.begin(), p.recommended_on.end(), PlatformName()) == p.recommended_on.end()) continue;
            recommended.push_back(p.name);
            for (const auto& m : p.mods) {
                const ModInfo* info = payload_.FindMod(m.first);
                if (!m.second && info != nullptr) off.push_back(info->title);
            }
        }
        ImGui::TextDisabled("Every mod on%s%s.", off.empty() ? "" : (" but " + Join(off, ", ")).c_str(),
                            recommended.empty() ? "" : (", with " + Join(recommended, ", ")).c_str());
        if (status_.installed || status_.record_present) {
            ImGui::Spacing();
            std::string label = ActionName(action);
            label += " (" + payload_.Summary() + ", keeps your choices)";
            if (BigButton(label.c_str(), !busy_)) StartInstall(false, false);
            Help("installs this version and keeps which mods are on and every setting as they are");
        }
    }
}

// SenPatcher (core/senpatcher.h): its own window; on Linux run through the game's Proton.
void App::DrawSenPatcher() {
    const SenPatcherStatus& sp = senpatcher_;
    const bool missing_override = sp.dll && sp.needs_override && !sp.override_set;
    Row("SenPatcher", sp.Text(), sp.dll ? (missing_override ? &kWarn : &kGood) : nullptr);
    Help("SenPatcher fixes and extends the game and loads p3a mods (the icon pack among them). It is "
         "AdmiralCurtiss's, not part of this toolkit");
    if (!sp.can_run) {
        if (!sp.cannot_run.empty()) {
            ImGui::PushStyleColor(ImGuiCol_Text, theme::kHint);
            ImGui::TextWrapped("%s", sp.cannot_run.c_str());
            ImGui::PopStyleColor();
        }
        return;
    }
    ImGui::BeginDisabled(game_running_ || busy_);
    if (ImGui::SmallButton(sp.dll ? "Run SenPatcher (change its options, update, remove)" : "Run SenPatcher")) StartSenPatcher(false);
    Help(sp.needs_override
             ? "downloads SenPatcher's latest release (when it is newer than the copy here), sets the dll override "
               "dinput8=native,builtin in the game's Proton prefix and opens SenPatcher's own window through the game's "
               "Proton: press \"Patch game\" for Trails of Cold Steel there"
             : "downloads SenPatcher's latest release (when it is newer than the copy here) and opens it: press "
               "\"Patch game\" for Trails of Cold Steel there");
    if (missing_override) {
        ImGui::SameLine();
        if (ImGui::SmallButton("Set dll override")) StartSenPatcher(true);
        Help("without the override Proton loads its own dinput8, and SenPatcher never runs: sets dinput8=native,builtin "
             "in the game's Proton prefix");
    }
    ImGui::EndDisabled();
    if (game_running_) {
        ImGui::SameLine();
        ImGui::TextDisabled("(close the game first)");
    }
}

// The icon pack (core/icon_pack.h): built here from the textures in use, so it follows a texture
// mod; only SenPatcher reads it.
void App::DrawIconPack() {
    const IconPackStatus& ip = status_.icon_pack;
    if (ip.state == IconPackState::NoConfig) return;
    const ImVec4* color = ip.state == IconPackState::Built || ip.state == IconPackState::NotNeeded ? &kGood
                          : ip.state == IconPackState::NeedsSenPatcher || ip.state == IconPackState::NotBuilt
                                    || ip.state == IconPackState::Removed                       ? nullptr
                                                                                                : &kWarn;
    Row("Icon pack", ip.text, color);
    const char* why = "only needed on small screens (the Steam Deck's 1280x800, 720p): there the game shrinks "
                      "the small UI icons so far that they look rough (with an HD texture pack their letters lose "
                      "strokes). The pack "
                      "pre-shrinks them to the size they are drawn at, from the textures in use. On 1080p and "
                      "bigger screens the icons are drawn larger and the pack only makes them a little softer - "
                      "remove it there. SenPatcher loads it from mods/";
    Help(why);
    ImGui::PushStyleColor(ImGuiCol_Text, theme::kHint);
    ImGui::TextWrapped("Only needed on small screens (Steam Deck, 720p); the biggest gain is with an HD texture pack.");
    ImGui::PopStyleColor();
    if (!ip.CanBuild()) return;
    const bool rebuild = ip.state == IconPackState::Built || ip.state == IconPackState::Stale
                         || ip.state == IconPackState::NotNeeded;
    ImGui::BeginDisabled(game_running_ || busy_);
    if (ImGui::SmallButton(rebuild ? "Rebuild icon pack" : "Build icon pack")) StartIconPackBuild();
    Help("builds mods/atmt_icon_pack.p3a from the textures in use - again after a texture mod changed");
    if (ip.state != IconPackState::Removed && ip.state != IconPackState::NotBuilt) {
        ImGui::SameLine();
        if (ImGui::SmallButton("Remove icon pack")) StartIconPackRemove();
        Help("removes mods/atmt_icon_pack.p3a and its order.txt line; installs leave it out until you build it again");
    }
    ImGui::EndDisabled();
    if (game_running_) {
        ImGui::SameLine();
        ImGui::TextDisabled("(close the game to build it)");
    }
}

// ---------------------------------------------------------------- Mods
void App::DrawMods() {
    if (!status_.installed) {
        ImGui::TextWrapped("Install first (Status) - then every mod can be switched on and off here.");
        return;
    }
    ImGui::TextDisabled("Off means moved to atmt_mods/disabled, which the loader skips.");
    for (const ModState& m : status_.mods) {
        const ModInfo* info = have_payload_ ? payload_.FindMod(m.name) : nullptr;
        if (!m.enabled && !m.disabled) continue;   // not installed
        bool on = m.enabled;
        ImGui::PushID(m.name.c_str());
        const std::string title = info != nullptr ? info->title : m.name;
        if (ImGui::Checkbox(title.c_str(), &on)) {
            std::string why;
            if (!SetModEnabled(game_dir_, m.name, on, &why)) OpenModal(Modal::Message, why);
            RefreshStatus();
        }
        auto version = status_.installed_versions.find(m.name);
        if (version != status_.installed_versions.end()) {
            ImGui::SameLine();
            ImGui::TextDisabled("%s", version->second.c_str());
        }
        Help(info != nullptr ? info->description : "not part of this version");
        if (info != nullptr && !info->description.empty()) {
            ImGui::Indent(ImGui::GetFrameHeight() + ImGui::GetStyle().ItemInnerSpacing.x);
            ImGui::PushStyleColor(ImGuiCol_Text, theme::kHint);
            ImGui::TextWrapped("%s", info->description.c_str());
            ImGui::PopStyleColor();
            ImGui::Unindent(ImGui::GetFrameHeight() + ImGui::GetStyle().ItemInnerSpacing.x);
        }
        ImGui::PopID();
    }
    if (status_.game_running) Banner(kWarn, "The game is running: a change takes effect on its next start.");
}

// ---------------------------------------------------------------- Settings
void App::DrawSettingRow(const SettingsGroup& g, const SettingDef& d) {
    if (d.type == "label") {
        if (!d.label.empty()) Heading(d.label.c_str());
        return;
    }
    if (d.type == "action") return;   // an in-game button (e.g. "open the log") - nothing to set here
    if (d.advanced && !show_advanced_) return;
    ImGui::PushID((d.section + "/" + d.key).c_str());
    std::string value = settings_.Get(g.mod, d.section, d.key);
    const float label_w = ImGui::GetContentRegionAvail().x * 0.45f;
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(d.label.c_str());
    if (d.restart) {
        ImGui::SameLine();
        ImGui::TextDisabled("(restart)");
    }
    const std::string help = d.help + (d.restart ? "\n- takes effect when the game starts" : "")
                             + "\n[" + d.section + "] " + d.key + " = " + d.def + " by default";
    Help(help);
    ImGui::SameLine(label_w);
    ImGui::SetNextItemWidth(-1);
    std::string fresh = value;
    bool changed = false;
    ImGui::BeginDisabled(d.readonly);
    if (d.type == "bool") {
        bool b = value == "true";
        if (ImGui::Checkbox("##v", &b)) {
            fresh = b ? "true" : "false";
            changed = true;
        }
    } else if (d.type == "int") {
        int v = std::atoi(value.c_str());
        if (d.ranged && d.max - d.min <= 2000) {
            changed = ImGui::SliderInt("##v", &v, static_cast<int>(d.min), static_cast<int>(d.max));
        } else {
            changed = ImGui::InputInt("##v", &v, d.step > 0 ? static_cast<int>(d.step) : 1);
            if (d.ranged) v = std::max(static_cast<int>(d.min), std::min(static_cast<int>(d.max), v));
        }
        if (changed) fresh = std::to_string(v);
    } else if (d.type == "float") {
        float v = static_cast<float>(std::atof(value.c_str()));
        if (d.ranged) {
            changed = ImGui::SliderFloat("##v", &v, static_cast<float>(d.min), static_cast<float>(d.max), "%.2f");
        } else {
            changed = ImGui::InputFloat("##v", &v, d.step > 0 ? static_cast<float>(d.step) : 0.1f, 0.0f, "%.2f");
        }
        if (changed) {
            if (d.step > 0) v = static_cast<float>(std::round(v / d.step) * d.step);
            char buf[32];
            std::snprintf(buf, sizeof(buf), "%g", static_cast<double>(v));
            fresh = buf;
        }
    } else if (d.type == "enum") {
        if (ImGui::BeginCombo("##v", value.c_str())) {
            for (const std::string& c : d.choices) {
                const bool sel = IEquals(c, value);
                if (ImGui::Selectable(c.c_str(), sel)) {
                    fresh = c;
                    changed = true;
                }
                if (sel) ImGui::SetItemDefaultFocus();
            }
            ImGui::EndCombo();
        }
    } else if ((d.type == "key" && !settings_.schema().input_keys.empty())
               || (d.type == "pad_button" && !settings_.schema().input_pad_buttons.empty())) {
        changed = DrawInputField(g, d, value, help, &fresh);
    } else {
        char buf[512];
        std::snprintf(buf, sizeof(buf), "%s", value.c_str());
        if (ImGui::InputText("##v", buf, sizeof(buf), ImGuiInputTextFlags_EnterReturnsTrue) || ImGui::IsItemDeactivatedAfterEdit()) {
            fresh = buf;
            changed = true;
        }
        // Game Mode has no keyboard: ask Steam for its on-screen one
        if (ImGui::IsItemActivated() && game_mode_) OpenUrl("steam://open/keyboard");
    }
    ImGui::EndDisabled();
    Help(help);
    if (changed && fresh != value) {
        std::string why;
        if (!settings_.Set(g.mod, d.section, d.key, fresh, &why)) help_ = why;
    }
    ImGui::PopID();
}

// ---------------------------------------------------------------- recording key / pad settings
bool App::DrawInputField(const SettingsGroup& g, const SettingDef& d, const std::string& value, const std::string& help,
                         std::string* fresh) {
    const bool pad = d.type == "pad_button";
    const bool mine = IEquals(record_.mod, g.mod) && IEquals(record_.section, d.section) && IEquals(record_.key, d.key);
    const bool listening = mine && input_recording();
    const std::string shown = value.empty() ? "(none)" : value;
    std::string label;
    if (listening) {
        std::string what;
        if (record_.phase == InputRecord::kArming) {
            what = "let go...";
        } else if (pad) {
            const std::vector<std::string> held = PadKeyNames(record_.pad_held);
            what = held.empty() ? "press buttons..." : Join(held, settings_.schema().pad_chord_separator) + "...";
        } else {
            const std::vector<std::string> mods = ModifierNames(static_cast<Uint16>(SDL_GetModState()));
            what = mods.empty() ? "press a key..." : Join(mods, settings_.schema().key_chord_separator) + "+...";
        }
        const int left = std::max(0, static_cast<int>(std::ceil(kRecordTimeout - (ImGui::GetTime() - record_.start))));
        label = what + "   (now " + shown + "; Esc cancels, " + std::to_string(left) + " s)";
    } else {
        label = shown;
        if (mine && record_.done && !IEquals(value, record_.before)) {
            label += "   (was " + (record_.before.empty() ? std::string("(none)") : record_.before) + ")";
        }
    }
    const ImGuiStyle& style = ImGui::GetStyle();
    const float clear_w = ImGui::CalcTextSize("Clear").x + style.FramePadding.x * 2.0f;
    const float w = std::max(ImGui::GetFontSize() * 4.0f, ImGui::GetContentRegionAvail().x - clear_w - style.ItemSpacing.x);
    ImGui::PushStyleVar(ImGuiStyleVar_ButtonTextAlign, ImVec2(0.0f, 0.5f));
    if (listening) {
        const bool blink = std::fmod(ImGui::GetTime(), 0.8) < 0.4;
        ImGui::PushStyleColor(ImGuiCol_Text, blink ? theme::kName : theme::kHint);
    }
    const bool pressed = ImGui::Button((label + "###record").c_str(), ImVec2(w, 0.0f));
    if (listening) ImGui::PopStyleColor();
    ImGui::PopStyleVar();
    Help(help + (pad ? "\nA / Enter, then press and release a button or a chord (Esc cancels)"
                     : "\nA / Enter, then press the key - hold Ctrl / Shift / Alt with it for a chord (Esc cancels)"));
    if (pressed && record_.phase == InputRecord::kOff) StartInputRecord(g, d, value);

    bool changed = false;
    ImGui::SameLine();
    ImGui::BeginDisabled(value.empty() || record_.phase != InputRecord::kOff);
    if (ImGui::Button("Clear")) {
        fresh->clear();
        changed = true;
    }
    ImGui::EndDisabled();
    Help(pad ? "Clear: no pad button for this" : "Clear: no key for this");
    return changed;
}

void App::StartInputRecord(const SettingsGroup& g, const SettingDef& d, const std::string& value) {
    record_ = InputRecord();
    record_.phase = InputRecord::kArming;
    record_.pad = d.type == "pad_button";
    record_.mod = g.mod;
    record_.section = d.section;
    record_.key = d.key;
    record_.before = value;
    record_.start = ImGui::GetTime();
    // The pad is read straight from ImGui's gamepad state, and the keys are kept away from ImGui
    // (HandleEvent): its navigation is off until everything is let go again.
    ImGuiIO& io = ImGui::GetIO();
    record_.saved_nav = io.ConfigFlags & (ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad);
    io.ConfigFlags &= ~record_.saved_nav;
    // Game Mode has no keyboard: Steam's on-screen one sends keys too
    if (!record_.pad && game_mode_) OpenUrl("steam://open/keyboard");
}

void App::FinishInputRecord(const std::string& value, bool store) {
    if (store) {
        std::string why;
        if (settings_.Set(record_.mod, record_.section, record_.key, value, &why)) {
            record_.done = true;
        } else {
            help_ = why;
        }
    }
    record_.phase = InputRecord::kReleasing;
}

void App::UpdateInputRecord() {
    if (record_.phase == InputRecord::kOff) return;
    if (record_.phase == InputRecord::kReleasing) {
        if (!AnythingHeld()) {
            ImGui::GetIO().ConfigFlags |= record_.saved_nav;
            record_.saved_nav = 0;
            record_.phase = InputRecord::kOff;
        }
        return;
    }
    if (!settings_loaded_ || ImGui::GetTime() - record_.start > kRecordTimeout) {
        FinishInputRecord(std::string(), false);
        return;
    }
    if (record_.phase == InputRecord::kArming) {
        if (!AnythingHeld()) record_.phase = InputRecord::kListening;
        return;
    }
    if (ImGui::IsMouseClicked(ImGuiMouseButton_Left) || ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
        FinishInputRecord(std::string(), false);   // a click (or a tap) elsewhere gives up
        return;
    }
    if (!record_.pad) return;   // keys come through HandleEvent
    const unsigned down = PadKeysDown();
    record_.pad_held |= down;
    if (record_.pad_held != 0 && down == 0) {
        const std::string chord = settings_.schema().PadChordValue(PadKeyNames(record_.pad_held));
        FinishInputRecord(chord, !chord.empty());
    }
}

// The Deck's right trackpad as a mouse (app.h, pointer_active_): its press arrives as R3.
void App::HandlePointerEvent(const SDL_Event& e) {
    ImGuiIO& io = ImGui::GetIO();
    switch (e.type) {
        case SDL_MOUSEMOTION:
            if (e.motion.which != SDL_TOUCH_MOUSEID) pointer_active_ = true;
            break;
        case SDL_KEYDOWN:
            pointer_active_ = false;
            break;
        case SDL_CONTROLLERAXISMOTION:
            if ((e.caxis.axis == SDL_CONTROLLER_AXIS_LEFTX || e.caxis.axis == SDL_CONTROLLER_AXIS_LEFTY)
                && (e.caxis.value > 16000 || e.caxis.value < -16000)) {
                pointer_active_ = false;
            }
            break;
        case SDL_CONTROLLERBUTTONDOWN:
        case SDL_CONTROLLERBUTTONUP: {
            const bool down = e.type == SDL_CONTROLLERBUTTONDOWN;
            if (e.cbutton.button != SDL_CONTROLLER_BUTTON_RIGHTSTICK) {
                if (down) pointer_active_ = false;
                break;
            }
            if (down ? !pointer_active_ || pad_click_down_ : !pad_click_down_) break;
            pad_click_down_ = down;
            io.AddMouseSourceEvent(ImGuiMouseSource_Mouse);
            io.AddMouseButtonEvent(ImGuiMouseButton_Left, down);
            break;
        }
        default:
            break;
    }
}

bool App::PadScroll() {
    // ImGui's analog stick values (0..1 past the dead zone), from the last frame
    const float x = ImGui::GetKeyData(ImGuiKey_GamepadRStickRight)->AnalogValue - ImGui::GetKeyData(ImGuiKey_GamepadRStickLeft)->AnalogValue;
    const float y = ImGui::GetKeyData(ImGuiKey_GamepadRStickDown)->AnalogValue - ImGui::GetKeyData(ImGuiKey_GamepadRStickUp)->AnalogValue;
    if (x == 0.0f && y == 0.0f) return false;
    if (input_recording()) return true;
    // a wheel notch scrolls about five lines; full tilt is ~6 notches a second at 60 frames, finer near the centre
    constexpr float kNotchesPerFrame = 0.1f;
    ImGui::GetIO().AddMouseWheelEvent(-x * std::fabs(x) * kNotchesPerFrame, -y * std::fabs(y) * kNotchesPerFrame);
    return true;
}

bool App::HandleEvent(const SDL_Event& e) {
    if (!input_recording()) {
        HandlePointerEvent(e);
        return false;
    }
    // Key presses (and the text they type) are the recorder's; releases still reach ImGui, so a key
    // it saw go down before the recording started is not left held.
    if (e.type == SDL_TEXTINPUT || e.type == SDL_TEXTEDITING) return true;
    if (e.type != SDL_KEYDOWN) return false;
    if (e.key.repeat != 0) return true;
    if (e.key.keysym.scancode == SDL_SCANCODE_ESCAPE) {
        FinishInputRecord(std::string(), false);
        return true;
    }
    if (record_.pad || record_.phase != InputRecord::kListening) return true;
    const std::string name = SdlKeyName(e.key.keysym);
    if (name.empty()) return true;   // a modifier on its own, or a key the mods cannot name: wait
    const std::string chord = settings_.schema().KeyChordValue(ModifierNames(e.key.keysym.mod), name);
    if (!chord.empty()) FinishInputRecord(chord, true);
    return true;
}

void App::SaveSettings() {
    std::string why;
    if (game_running_) {
        // a mod may write these inis while the game runs: queue for its next start
        if (!settings_.Queue(&why)) OpenModal(Modal::Message, why);
        return;
    }
    switch (settings_.Save(&why, &conflicts_)) {
        case SettingsModel::SaveResult::Conflict: OpenModal(Modal::Conflict); break;
        case SettingsModel::SaveResult::Failed: OpenModal(Modal::Message, why); break;
        default: break;
    }
}

void App::DrawSettings() {
    if (!settings_loaded_) {
        ImGui::TextWrapped("%s", have_payload_ ? "Pick the game's folder first (Status)." : payload_error_.c_str());
        return;
    }
    const std::vector<SettingsGroup>& groups = settings_.schema().groups;
    const float list_w = ImGui::GetFontSize() * 9.0f;
    const float footer = ImGui::GetFrameHeightWithSpacing() * 1.6f;
    ImGui::BeginChild("##groups", ImVec2(list_w, -footer), ImGuiChildFlags_Borders);
    for (size_t i = 0; i < groups.size(); ++i) {
        if (ImGui::Selectable(groups[i].title.c_str(), settings_group_ == i)) settings_group_ = i;
        Help(groups[i].ini);
    }
    ImGui::EndChild();
    ImGui::SameLine();
    ImGui::BeginChild("##form", ImVec2(0, -footer), ImGuiChildFlags_Borders);
    if (settings_group_ < groups.size()) {
        const SettingsGroup& g = groups[settings_group_];
        ImGui::TextDisabled("%s", g.ini.c_str());
        for (const SettingDef& d : g.settings) DrawSettingRow(g, d);
        const std::vector<RawEntry> raw = settings_.Raw(g.mod);
        if (!raw.empty() && ImGui::TreeNode("Advanced (raw)")) {
            Help("keys in the ini that this version does not describe - edited as plain text");
            for (const RawEntry& r : raw) {
                ImGui::PushID((r.section + "/" + r.key).c_str());
                ImGui::AlignTextToFramePadding();
                ImGui::Text("[%s] %s", r.section.c_str(), r.key.c_str());
                ImGui::SameLine(ImGui::GetContentRegionAvail().x * 0.45f);
                ImGui::SetNextItemWidth(-1);
                char buf[512];
                std::snprintf(buf, sizeof(buf), "%s", r.value.c_str());
                if (ImGui::InputText("##raw", buf, sizeof(buf)) ) settings_.SetRaw(g.mod, r.section, r.key, buf);
                if (ImGui::IsItemActivated() && game_mode_) OpenUrl("steam://open/keyboard");
                ImGui::PopID();
            }
            ImGui::TreePop();
        }
    }
    ImGui::EndChild();
    const size_t pending = settings_.PendingCount();
    if (BigButton(game_running_ ? "Save for the next start" : "Save", pending > 0)) SaveSettings();
    Help(game_running_ ? "the game is running: the changes are written once it has exited (a mod in the game may write these files meanwhile)"
                       : "writes the changed values into the inis; comments and other lines stay");
    ImGui::SameLine();
    if (BigButton("Discard", pending > 0)) settings_.Discard();
    ImGui::SameLine();
    ImGui::Checkbox("Show advanced", &show_advanced_);
    Help("diagnostics and development settings");
    if (pending > 0) {
        ImGui::SameLine();
        ImGui::TextDisabled("%u unsaved change(s)", static_cast<unsigned>(pending));
    }
}

// ---------------------------------------------------------------- Presets
void App::DrawPresets() {
    if (!settings_loaded_) {
        ImGui::TextWrapped("Pick the game's folder first (Status).");
        return;
    }
    ImGui::TextDisabled("A preset only changes the keys listed under it; everything else stays as it is.");
    for (const Preset& p : payload_.presets) {
        ImGui::PushID(p.id.c_str());
        Heading(p.name.c_str());
        ImGui::TextWrapped("%s", p.description.c_str());
        const auto diff = settings_.PresetDiff(p);
        const auto mods = PresetModChanges(game_dir_, p);
        const bool icon_pack = PresetBuildsIconPack(p, status_.icon_pack);
        if (diff.empty() && mods.empty() && !icon_pack) {
            ImGui::TextDisabled("already in place");
        } else if (ImGui::BeginTable("##diff", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp)) {
            for (const auto& m : mods) {
                const ModInfo* info = have_payload_ ? payload_.FindMod(m.first) : nullptr;
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::TextUnformatted((info != nullptr ? info->title : m.first).c_str());
                ImGui::TableNextColumn();
                ImGui::TextUnformatted("Mod");
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(m.second ? "off  ->  on" : "on  ->  off");
            }
            if (icon_pack) {
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::TextUnformatted("Icon pack");
                ImGui::TableNextColumn();
                ImGui::TextUnformatted("mods/atmt_icon_pack.p3a");
                ImGui::TableNextColumn();
                ImGui::Text("%s  ->  built", status_.icon_pack.text.c_str());
            }
            for (const auto& d : diff) {
                const SettingsGroup* g = settings_.schema().Find(d.first.mod);
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::TextUnformatted((g != nullptr ? g->title : d.first.mod).c_str());
                ImGui::TableNextColumn();
                const SettingDef* def = settings_.schema().Find(d.first.mod, d.first.section, d.first.key);
                ImGui::TextUnformatted(def != nullptr ? def->label.c_str() : d.first.key.c_str());
                ImGui::TableNextColumn();
                ImGui::Text("%s  ->  %s", d.second.c_str(), d.first.value.c_str());
            }
            ImGui::EndTable();
        }
        if (BigButton("Apply", !(diff.empty() && mods.empty() && !icon_pack) && !busy_)) {
            std::string why;
            if (!settings_.ApplyPreset(p, &why) || !ApplyPresetMods(game_dir_, p, nullptr, &why)) {
                OpenModal(Modal::Message, why);
            } else {
                if (!diff.empty()) SaveSettings();
                RefreshStatus();
                if (icon_pack) StartIconPackBuild();
            }
        }
        Help(icon_pack && game_running_ ? "writes the changes listed above; close the game to build the icon pack (Status)"
                                        : "writes exactly the changes listed above");
        ImGui::PopID();
    }
    Heading("Defaults");
    if (BigButton("Reset every setting to its default", !busy_)) OpenModal(Modal::ConfirmReset);
}

// ---------------------------------------------------------------- System
void App::DrawSystem() {
    Heading("Steam library");
    ImGui::TextWrapped(in_steam_ ? "ATMT Manager is in your Steam library (Non-Steam), so it starts from Game Mode too."
                                 : "Add ATMT Manager to your Steam library to start it from Game Mode.");
    if (steam_accounts_ == 0) ImGui::TextDisabled("No Steam account found on this computer.");
    if (BigButton(in_steam_ ? "Remove from Steam" : "Add to Steam", !busy_ && steam_accounts_ > 0)) {
        steam_add_ = !in_steam_;
        if (SteamRunning()) {
            OpenModal(Modal::ConfirmSteam);
        } else {
            StartSteamChange(steam_add_);
        }
    }
    Help("Steam keeps its library in a file it rewrites when it closes, so it is closed for the change and started again");
    if (in_steam_) {
        ImGui::SameLine();
        if (BigButton("Update the entry", !busy_ && steam_accounts_ > 0)) {
            steam_add_ = true;
            if (SteamRunning()) {
                OpenModal(Modal::ConfirmSteam);
            } else {
                StartSteamChange(true);
            }
        }
        Help("points the entry at this copy of the app again (after moving it)");
    }

    Heading("Updates");
    ImGui::Text("App %s", AppVersion());
    if (have_payload_ && ImGui::TreeNode("Components")) {
        for (const Component& c : payload_.components) ImGui::TextDisabled("%-22s %s", c.title.c_str(), c.version.c_str());
        ImGui::TreePop();
    }
    if (!http_) ImGui::TextDisabled("%s", http_error_.c_str());
    if (checked_) {
        ImGui::TextDisabled("%s%s", check_.message.c_str(),
                            check_.checked_at.empty() ? "" : ("  (checked " + check_.checked_at + ")").c_str());
    }
    const std::string last_error = state_["update"].Str("last_error");
    if (!last_error.empty()) ImGui::TextDisabled("couldn't check: %s", last_error.c_str());
    if (ImGui::Button("Check now") && !busy_) StartUpdateCheck(true);
    Help("reads the newest release's manifest (only that: no identifiers, no telemetry)");
    if (checked_ && check_.app_newer) {
        ImGui::SameLine();
        if (ImGui::Button(("Update the app to " + check_.manifest.manager_version).c_str()) && !busy_) StartAppUpdate();
    }
    if (checked_ && !check_.manifest.page.empty()) {
        ImGui::SameLine();
        if (ImGui::Button("Release page")) OpenUrl(check_.manifest.page);
    }
    bool auto_check = state_["update"]["auto_check"].AsBool(true);
    if (ImGui::Checkbox("Check for updates automatically", &auto_check)) {
        ChangeState([&](Json& st) { st["update"]["auto_check"] = auto_check; });
    }
    Help("once a day when the app starts; offline is fine (it just says it could not check)");
    bool auto_install = state_["update"]["auto_install"].AsBool(false);
    if (ImGui::Checkbox("Install updates automatically", &auto_install)) {
        ChangeState([&](Json& st) { st["update"]["auto_install"] = auto_install; });
    }
    Help("off: a newer version is shown on the Status screen and installed when you press Update");
    if (ManifestUrl().empty()) ImGui::TextDisabled("This build has no release manifest configured: updates are off.");

    Heading("Backups");
    if (!game_dir_.empty()) {
        const std::vector<BackupInfo> backups = ListBackups(game_dir_);
        if (backups.empty()) ImGui::TextDisabled("none yet - every install and uninstall makes one");
        if (!backups.empty() && ImGui::BeginTable("##backups", 4, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp)) {
            for (const BackupInfo& b : backups) {
                ImGui::PushID(b.name.c_str());
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(b.name.substr(12).c_str());
                ImGui::TableNextColumn();
                ImGui::TextDisabled("%s", b.created.c_str());
                ImGui::TableNextColumn();
                ImGui::TextDisabled("before %s%s", b.reason.c_str(), b.installed.empty() ? "" : (" (" + b.installed + ")").c_str());
                ImGui::TableNextColumn();
                if (ImGui::SmallButton("Restore") && !busy_) {
                    restore_name_ = b.name;
                    OpenModal(Modal::ConfirmRestore);
                }
                Help("puts the loader, the mods and their settings back as they were (the current state is backed up first)");
                ImGui::PopID();
            }
            ImGui::EndTable();
        }
    }

    Heading("Uninstall");
    const bool anything = status_.installed || status_.record_present || IsDir(game_dir_ / kModDir);
    if (BigButton("Uninstall all mods and settings", !busy_ && anything)) OpenModal(Modal::ConfirmUninstall);
    Help("puts the game's own files back; every mod and its settings are removed (and backed up first); logs and backups stay");
    ImGui::SameLine();
    if (BigButton("Remove everything", !busy_)) OpenModal(Modal::ConfirmPurge);
    Help("also every log, output file and backup: the game folder as the store installed it - nothing is kept");

    Heading("Log");
    if (ImGui::BeginTabBar("##logs")) {
        if (ImGui::BeginTabItem("This app")) {
            ImGui::BeginChild("##applog", ImVec2(0, ImGui::GetFontSize() * 12.0f), ImGuiChildFlags_Borders);
            std::lock_guard<std::mutex> lock(log_mutex_);
            for (const std::string& line : log_) ImGui::TextUnformatted(line.c_str());
            if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY()) ImGui::SetScrollHereY(1.0f);
            ImGui::EndChild();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("atmt_loader.log")) {
            if (!loader_log_loaded_ || ImGui::Button("Reload")) {
                loader_log_.clear();
                if (!ReadFile(game_dir_ / "atmt_loader.log", &loader_log_)) loader_log_ = "(no atmt_loader.log yet - it is written when the game starts)";
                if (loader_log_.size() > 200000) loader_log_ = loader_log_.substr(loader_log_.size() - 200000);
                loader_log_loaded_ = true;
            }
            ImGui::BeginChild("##loaderlog", ImVec2(0, ImGui::GetFontSize() * 12.0f), ImGuiChildFlags_Borders,
                              ImGuiWindowFlags_HorizontalScrollbar);
            ImGui::TextUnformatted(loader_log_.c_str());
            ImGui::EndChild();
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }

    if (PlatformName() == "linux") {
        Heading("Game Mode");
        if (ImGui::Button("How to start this app in Game Mode")) OpenModal(Modal::FirstRun);
    }
    ImGui::Spacing();
    if (BigButton("Quit")) quit_ = true;
}

// ---------------------------------------------------------------- dialogs
void App::DrawModals() {
    if (modal_ == Modal::None) return;
    const char* id = "##modal";
    if (!modal_opened_) {
        ImGui::OpenPopup(id);
        modal_opened_ = true;
    }
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->GetCenter(), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(vp->Size.x * 0.75f, 0));
    if (!ImGui::BeginPopupModal(id, nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize)) {
        modal_ = Modal::None;
        return;
    }
    auto close = [this]() {
        modal_ = Modal::None;
        ImGui::CloseCurrentPopup();
    };
    // B (or Escape) backs out of every dialog but the busy screen
    const bool cancel = ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight, false) || ImGui::IsKeyPressed(ImGuiKey_Escape, false);
    switch (modal_) {
        case Modal::PickGame: {
            Heading("The game's folder");
            if (games_.empty()) {
                ImGui::TextWrapped("The game was not found in Steam, GOG, Heroic, Lutris or Bottles. Pick or type "
                                   "the folder that has ed8.exe in it:");
            }
            for (const GameInstall& g : games_) {
                const std::string label = U8(g.dir) + (g.source.empty() ? "" : "   (" + g.source + ")");
                if (ImGui::Selectable(label.c_str(), g.dir == game_dir_)) {
                    SetGameDir(g.dir);
                    close();
                }
            }
            ImGui::Spacing();
            char buf[1024];
            std::snprintf(buf, sizeof(buf), "%s", pick_path_.c_str());
            if (!game_mode_) {
                // the system's own folder picker (none in Game Mode: type the path there)
                if (ImGui::Button("Browse...")) {
                    std::string picked;
                    if (PickFolder("The game's folder (with ed8.exe)", pick_path_, &picked)) pick_path_ = picked;
                    std::snprintf(buf, sizeof(buf), "%s", pick_path_.c_str());
                }
                ImGui::SameLine();
            }
            ImGui::SetNextItemWidth(-1);
            if (ImGui::InputText("##path", buf, sizeof(buf))) pick_path_ = buf;
            if (ImGui::IsItemActivated() && game_mode_) OpenUrl("steam://open/keyboard");
            const fs::path picked_dir = ResolveGameDir(pick_path_);
            const bool ok = !picked_dir.empty();
            if (!pick_path_.empty() && !ok) ImGui::TextColored(kWarn, "no ed8.exe in that folder");
            if (ok && U8(picked_dir) != Trim(pick_path_)) ImGui::TextDisabled("%s", U8(picked_dir).c_str());
            ImGui::BeginDisabled(!ok);
            if (ImGui::Button("Use this folder")) {
                SetGameDir(picked_dir);
                close();
            }
            ImGui::EndDisabled();
            ImGui::SameLine();
            if (ImGui::Button("Cancel") || cancel) close();
            break;
        }
        case Modal::AckUnverified: {
            Heading("Unverified game build");
            ImGui::TextWrapped("This ed8.exe (md5 %s) is not a build the mods were checked against - a game patch, or "
                               "a GOG build. The mods hook fixed addresses in the game, so on "
                               "another build they can crash it or lose features. Uninstall restores everything.",
                               status_.exe_md5.c_str());
            for (const SupportedExe& e : payload_.supported) ImGui::TextDisabled("supported: %s  %s", e.md5.c_str(), e.note.c_str());
            ImGui::Spacing();
            if (BigButton("Install anyway")) {
                close();
                StartInstall(true, ack_everything_);
            }
            ImGui::SameLine();
            if (BigButton("Cancel") || cancel) close();
            break;
        }
        case Modal::ConfirmUninstall:
            Heading("Uninstall all mods and settings");
            ImGui::TextWrapped("The game's own files are put back, and every mod and its settings are removed. They are "
                               "backed up first (System > Backups); the logs stay.");
            if (status_.game_running) ImGui::TextColored(kWarn, "Close the game first.");
            if (BigButton("Uninstall", !status_.game_running)) {
                close();
                StartUninstall(false);
            }
            ImGui::SameLine();
            if (BigButton("Cancel") || cancel) close();
            break;
        case Modal::ConfirmPurge:
            Heading("Remove everything");
            ImGui::TextWrapped("The game's own files are put back, and everything the mods ever put into the game folder is "
                               "deleted: the mods, their settings, their logs and output files, and every backup. "
                               "Nothing is kept and it cannot be undone.");
            if (status_.game_running) ImGui::TextColored(kWarn, "Close the game first.");
            if (BigButton("Remove everything", !status_.game_running)) {
                close();
                StartUninstall(true);
            }
            ImGui::SameLine();
            if (BigButton("Cancel") || cancel) close();
            break;
        case Modal::ConfirmSteam:
            Heading(steam_add_ ? "Add to Steam" : "Remove from Steam");
            ImGui::TextWrapped("Steam is running. It keeps its library in a file it rewrites when it closes, so it has to "
                               "close for this change - the app closes it, makes the change and starts it again.");
            if (game_mode_) ImGui::TextColored(kWarn, "In Game Mode Steam cannot be closed: switch to Desktop Mode for this.");
            if (BigButton("Close Steam and continue", !game_mode_)) {
                close();
                StartSteamChange(steam_add_);
            }
            ImGui::SameLine();
            if (BigButton("Cancel") || cancel) close();
            break;
        case Modal::ConfirmRestore:
            Heading("Restore a backup");
            ImGui::TextWrapped("The loader, the mods and their settings go back to %s. The current state is backed up first.",
                               restore_name_.c_str());
            if (status_.game_running) ImGui::TextColored(kWarn, "Close the game first.");
            if (BigButton("Restore", !status_.game_running)) {
                close();
                const fs::path game = game_dir_;
                const std::string name = restore_name_;
                Run("Restoring ...", [this, game, name]() {
                    const InstallResult r = RestoreBackup(game, name);
                    Post([this, r]() {
                        RefreshStatus();
                        ReloadSettings();
                        OpenModal(Modal::Message, r.ok ? std::string("Restored.") : r.error);
                    });
                });
            }
            ImGui::SameLine();
            if (BigButton("Cancel") || cancel) close();
            break;
        case Modal::ConfirmReset:
            Heading("Reset to defaults");
            ImGui::TextWrapped("Every setting of every mod goes back to its default value.");
            if (BigButton("Reset")) {
                close();
                settings_.ResetToDefaults();
                SaveSettings();
            }
            ImGui::SameLine();
            if (BigButton("Cancel") || cancel) close();
            break;
        case Modal::Conflict:
            Heading("Changed meanwhile");
            ImGui::TextWrapped("These files were changed since they were read (by a mod in the game, or by hand):");
            for (const std::string& c : conflicts_) ImGui::BulletText("%s", c.c_str());
            if (BigButton("Keep my changes on top")) {
                settings_.Reload();
                close();
                SaveSettings();
            }
            Help("re-reads the files and writes only the values you changed");
            ImGui::SameLine();
            if (BigButton("Use theirs")) {
                settings_.Discard();
                settings_.Reload();
                close();
            }
            ImGui::SameLine();
            if (BigButton("Cancel") || cancel) close();
            break;
        case Modal::FirstRun:
            Heading("Using this app in Game Mode");
            ImGui::TextWrapped(
                "Add this app to your Steam library (Steam closes for a moment and starts again); in Game Mode it is "
                "then under Non-Steam, with the controller layout \"Gamepad with Mouse Trackpad\": the right trackpad "
                "moves the pointer and pressing it clicks, the right stick scrolls.\n\n"
                "Keep the AppImage where it is: app updates replace it in place, so the entry keeps working. "
                "System > Steam library adds or removes it later.");
            if (!in_steam_ && BigButton("Add to Steam", !busy_ && steam_accounts_ > 0)) {
                ChangeState([](Json& st) { st["first_run_done"] = true; });
                close();
                steam_add_ = true;
                if (SteamRunning()) {
                    OpenModal(Modal::ConfirmSteam);
                } else {
                    StartSteamChange(true);
                }
                break;
            }
            ImGui::SameLine();
            if (BigButton(in_steam_ ? "OK" : "Not now") || cancel) {
                ChangeState([](Json& st) { st["first_run_done"] = true; });
                close();
            }
            break;
        case Modal::Message:
        default:
            ImGui::TextWrapped("%s", modal_message_.c_str());
            ImGui::Spacing();
            if (BigButton("OK") || cancel) close();
            break;
    }
    ImGui::EndPopup();
}

}  // namespace atmt
