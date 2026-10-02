// settings_bar.cpp - the top bar's behaviour and drawing (see settings_bar.h).
#include "settings_bar.h"

#include "imgui.h"
#include "imgui_internal.h"   // BringWindowToDisplayFront: the bar stays above the mods' windows
#include "atmt_input.h"
#include "atmt_theme.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <cmath>
#include <cstdio>
#include <cstring>

namespace atmt_overlay {

namespace {

using namespace atmt::theme;
using atmt::KeyDown;
using atmt::PadChordName;

constexpr DWORD kPickTimeoutMs = 8000;   // a pick nobody finishes gives up on its own
constexpr float kMenuMinWidth = 460.0f;  // at kBaseFontSize
const char kAdvancedHelp[] = "diagnostics and development settings - leave them alone unless you "
                             "are working on the mod";

bool IsAdvanced(const AtmtSetting& s) { return (s.flags & ATMT_SETTING_ADVANCED) != 0; }
bool IsReadonly(const AtmtSetting& s) { return (s.flags & ATMT_SETTING_READONLY) != 0; }

// A row of the open menu: a setting, or (-1) the "advanced settings" switch at its end.
struct Row {
    int setting;
};

bool Selectable(const AtmtSettingsGroup& g, const Row& row) {
    return row.setting < 0 || g.settings[row.setting].type != ATMT_SETTING_LABEL;
}

const char* LabelOf(const AtmtSetting& s) {
    if (s.label != nullptr) return s.label;
    return s.key != nullptr ? s.key : "";
}

int FloatDecimals(float step) {
    if (step <= 0.0f) return 2;
    if (step < 0.01f) return 3;
    if (step < 0.1f) return 2;
    if (step < 1.0f) return 1;
    return 0;
}

std::string ValueText(const AtmtSetting& s) {
    char buf[160];
    switch (s.type) {
        case ATMT_SETTING_BOOL:
            return *static_cast<const int32_t*>(s.value) != 0 ? "On" : "Off";
        case ATMT_SETTING_INT:
            _snprintf(buf, sizeof(buf), "%d", static_cast<int>(*static_cast<const int32_t*>(s.value)));
            return buf;
        case ATMT_SETTING_FLOAT:
            _snprintf(buf, sizeof(buf), "%.*f", FloatDecimals(s.step),
                      *static_cast<const float*>(s.value));
            return buf;
        case ATMT_SETTING_ENUM: {
            const int32_t i = *static_cast<const int32_t*>(s.value);
            return (i >= 0 && static_cast<uint32_t>(i) < s.choice_count) ? s.choices[i] : "?";
        }
        case ATMT_SETTING_STRING:
        case ATMT_SETTING_KEY:
        case ATMT_SETTING_PAD_BUTTON: {
            const char* text = static_cast<const char*>(s.value);
            const size_t n = strnlen(text, s.capacity);
            return n == 0 ? std::string("(none)") : std::string(text, n);
        }
        default:
            return std::string();
    }
}

// Can left/right change it? (Everything with a value but a free-text string.)
bool Adjustable(const AtmtSetting& s) {
    if (IsReadonly(s)) return false;
    return s.type == ATMT_SETTING_BOOL || s.type == ATMT_SETTING_INT || s.type == ATMT_SETTING_FLOAT
           || s.type == ATMT_SETTING_ENUM;
}

}  // namespace

void SettingsBar::Open() {
    pick_ = Pick();
    scroll_to_row_ = true;
    tabs_followed_.clear();   // the menu showing is scrolled into view again,
    tabs_snap_ = true;        // at once
    tabs_dragging_ = false;
}

// The one place a setting changes: every input path (pad, keys, a click) ends here.
void SettingsBar::Change(const AtmtModApi* api, const AtmtSetting& s, int adjust, bool accept) {
    if (IsReadonly(s) || (adjust == 0 && !accept)) return;
    int result = 0;
    switch (s.type) {
        case ATMT_SETTING_BOOL: {
            const int32_t v = *static_cast<const int32_t*>(s.value) != 0 ? 0 : 1;
            result = api->settings_set(&s, &v);
            break;
        }
        case ATMT_SETTING_INT: {
            if (adjust == 0) return;
            const int step = s.step >= 1.0f ? static_cast<int>(s.step) : 1;
            const int32_t v = *static_cast<const int32_t*>(s.value) + adjust * step;
            result = api->settings_set(&s, &v);   // the registry clamps
            break;
        }
        case ATMT_SETTING_FLOAT: {
            if (adjust == 0) return;
            const float step = s.step > 0.0f ? s.step : 0.1f;
            float v = *static_cast<const float*>(s.value) + static_cast<float>(adjust) * step;
            // Snapped to the step's grid (from min when there is a range), so a value typed into the
            // ini as 0.37 lands on 0.40, not on 0.42 and every other odd value after it.
            const float origin = s.min < s.max ? s.min : 0.0f;
            v = origin + std::round((v - origin) / step) * step;
            result = api->settings_set(&s, &v);
            break;
        }
        case ATMT_SETTING_ENUM: {
            const int n = static_cast<int>(s.choice_count);
            const int dir = adjust < 0 ? -1 : 1;   // one choice at a time, however long it is held
            const int32_t v = (*static_cast<const int32_t*>(s.value) + dir + n) % n;
            result = api->settings_set(&s, &v);
            break;
        }
        case ATMT_SETTING_ACTION:
            if (accept) api->settings_set(&s, nullptr);
            return;
        case ATMT_SETTING_KEY:
        case ATMT_SETTING_PAD_BUTTON:
            if (!accept || s.key == nullptr) return;
            pick_ = Pick();
            pick_.active = true;
            pick_.pad = s.type == ATMT_SETTING_PAD_BUTTON;
            pick_.start = GetTickCount();
            pick_.key = s.key;
            return;
        default:
            return;
    }
    if (result == 1 && (s.flags & ATMT_SETTING_RESTART) != 0) restart_pending_ = true;
}

// A pick in progress: first everything has to be let go (the A / Enter that started it is still
// down), then the next key - or the next pad chord, taken when its buttons are released - is it.
// Esc (the keyboard's, for both) or a timeout gives up.
void SettingsBar::StepPick(const AtmtModApi* api, const AtmtSetting* s, const BarInput& in) {
    if (s == nullptr || GetTickCount() - pick_.start > kPickTimeoutMs) {
        pick_ = Pick();
        return;
    }
    if (pick_.pad) {
        if (pick_.armed && KeyDown(VK_ESCAPE)) {
            pick_ = Pick();
            return;
        }
        if (!pick_.armed) {
            if (in.pad_buttons == 0 && !KeyDown(VK_RETURN) && !KeyDown(VK_SPACE)) pick_.armed = true;
            return;
        }
        pick_.held = static_cast<unsigned short>(pick_.held | in.pad_buttons);
        if (pick_.held != 0 && in.pad_buttons == 0) {
            const std::string name = PadChordName(pick_.held);
            const int result = api->settings_set(s, name.c_str());
            if (result == 1 && (s->flags & ATMT_SETTING_RESTART) != 0) restart_pending_ = true;
            pick_ = Pick();
        }
        return;
    }
    // A key chord: the modifiers held when the first other key goes down, and that key. The
    // modifiers themselves (either side, and Win) are never the key, and the mouse buttons (below
    // 0x08) are not keys.
    unsigned down = 0;
    for (unsigned vk = 0x08; vk <= 0xFE && down == 0; ++vk) {
        if (atmt::IsModifierKey(vk)) continue;
        if (KeyDown(vk)) down = vk;
    }
    if (!pick_.armed) {
        if (down == 0 && in.pad_buttons == 0) pick_.armed = true;
        return;
    }
    if (down == 0) return;
    if (down == VK_ESCAPE) {
        pick_ = Pick();
        return;
    }
    const std::string name = atmt::KeyChordName(down | atmt::ModifiersDown());
    if (name.empty()) return;   // a key without a name: wait for one that has
    const int result = api->settings_set(s, name.c_str());
    if (result == 1 && (s->flags & ATMT_SETTING_RESTART) != 0) restart_pending_ = true;
    pick_ = Pick();
}

bool SettingsBar::Draw(const AtmtModApi* api, const BarInput& in, float alpha) {
    if (api == nullptr || api->settings_acquire == nullptr) return false;
    uint32_t count = 0;
    const AtmtSettingsGroup* groups = api->settings_acquire(&count, nullptr);
    // Released at the end of this function, on this thread (the registry lock is recursive, so the
    // settings_set calls below are fine while it is held).
    struct Release {
        const AtmtModApi* api;
        ~Release() { api->settings_release(); }
    } release{api};

    bool keep_open = true;

    // ---- which menu: found again by its identity, since tables come and go (a dev reload)
    int tab = 0;
    for (uint32_t i = 0; i < count; ++i) {
        if (tab_id_ == std::string(groups[i].mod_name) + "\x1f" + groups[i].title) {
            tab = static_cast<int>(i);
            break;
        }
    }
    if (count > 0 && in.menus != 0 && !pick_.active) {
        tab = ((tab + in.menus) % static_cast<int>(count) + static_cast<int>(count))
              % static_cast<int>(count);
        row_ = 0;
        show_advanced_ = false;
        scroll_to_row_ = true;
    }
    const AtmtSettingsGroup* group = count > 0 ? &groups[tab] : nullptr;
    if (group != nullptr) {
        tab_id_ = std::string(group->mod_name) + "\x1f" + group->title;
        current_title_ = group->title;
        current_mod_ = group->mod_name;
    }

    // ---- the rows of the open menu
    std::vector<Row> rows;
    int advanced = 0;
    if (group != nullptr) {
        for (uint32_t i = 0; i < group->count; ++i) {
            if (IsAdvanced(group->settings[i])) {
                ++advanced;
                if (!show_advanced_) continue;
            }
            rows.push_back(Row{static_cast<int>(i)});
        }
        if (advanced > 0) rows.push_back(Row{-1});
    }
    auto first_selectable = [&](int from, int dir) {
        for (int i = from; i >= 0 && i < static_cast<int>(rows.size()); i += dir) {
            if (Selectable(*group, rows[i])) return i;
        }
        return -1;
    };
    if (!rows.empty()) {
        if (row_ >= static_cast<int>(rows.size())) row_ = static_cast<int>(rows.size()) - 1;
        if (row_ < 0) row_ = 0;
        if (!Selectable(*group, rows[row_])) {
            int r = first_selectable(row_, +1);
            if (r < 0) r = first_selectable(row_, -1);
            row_ = r < 0 ? 0 : r;
        }
    }

    // ---- input
    const AtmtSetting* selected = (!rows.empty() && rows[row_].setting >= 0)
                                     ? &group->settings[rows[row_].setting]
                                     : nullptr;
    if (pick_.active) {
        const AtmtSetting* target = nullptr;
        for (uint32_t i = 0; group != nullptr && i < group->count; ++i) {
            const AtmtSetting& s = group->settings[i];
            if (s.key != nullptr && pick_.key == s.key) target = &s;
        }
        StepPick(api, target, in);
    } else {
        if (in.back) keep_open = false;
        if (!rows.empty() && in.rows != 0) {
            const int dir = in.rows < 0 ? -1 : 1;
            const int n = static_cast<int>(rows.size());
            int r = row_;
            for (int tries = 0; tries < n; ++tries) {   // wraps, skipping the captions
                r = (r + dir + n) % n;
                if (Selectable(*group, rows[r])) break;
            }
            row_ = r;
            scroll_to_row_ = true;
            selected = rows[row_].setting >= 0 ? &group->settings[rows[row_].setting] : nullptr;
        }
        if (!rows.empty() && rows[row_].setting < 0) {
            if (in.accept || in.adjust != 0) show_advanced_ = !show_advanced_;
        } else if (selected != nullptr) {
            Change(api, *selected, in.adjust, in.accept);
        }
    }

    // ---- the bar
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    const float s = UiScale();
    const float line = ImGui::GetTextLineHeight();
    const ImVec2 bar_pad(14.0f * s, 7.0f * s);
    const float bar_height = line + bar_pad.y * 2.0f;
    const ImGuiWindowFlags fixed = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove
                                   | ImGuiWindowFlags_NoSavedSettings
                                   | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav;

    ImGui::PushStyleVar(ImGuiStyleVar_Alpha, alpha);

    ImGui::SetNextWindowPos(viewport->Pos);
    ImGui::SetNextWindowSize(ImVec2(viewport->Size.x, bar_height));
    ImGui::SetNextWindowBgAlpha(0.96f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, bar_pad);
    // Every window shares one context, stacked by focus rather than by drawing order: a click on
    // the dialog log (or a window that appears later) would cover the bar and its menu. They are
    // put back on top each frame, the menu above the bar.
    const bool bar_shown = ImGui::Begin("##atmt_settings_bar", nullptr, fixed | ImGuiWindowFlags_NoScrollbar);
    ImGui::BringWindowToDisplayFront(ImGui::GetCurrentWindow());
    if (bar_shown) {
        ImGui::TextColored(kTitle, "MODS");
        ImDrawList* draw = ImGui::GetWindowDrawList();
        ImGuiIO& io = ImGui::GetIO();
        const float gap = 22.0f * s;
        ImGui::SameLine(0.0f, gap);

        // The strip the menus get: between "MODS" and the hint. Laid out in a row from 0; when the
        // row is wider than the strip, it scrolls, and arrows at both ends take some of its room.
        const float hint_width = hint_.empty() ? 0.0f : ImGui::CalcTextSize(hint_.c_str()).x;
        const float strip_y = ImGui::GetCursorScreenPos().y;
        const float strip_left = ImGui::GetCursorScreenPos().x;
        float strip_right = viewport->Pos.x + viewport->Size.x - bar_pad.x;
        if (!hint_.empty()) strip_right -= hint_width + gap;
        if (strip_right < strip_left) strip_right = strip_left;
        std::vector<float> tab_start(count), tab_width(count);
        float row_width = 0.0f;
        for (uint32_t i = 0; i < count; ++i) {
            if (i != 0) row_width += gap;
            tab_start[i] = row_width;
            tab_width[i] = ImGui::CalcTextSize(groups[i].title).x;
            row_width += tab_width[i];
        }
        const bool overflow = row_width > strip_right - strip_left;
        const float arrow_width = overflow ? line * 1.2f : 0.0f;
        const float view_left = strip_left + arrow_width;
        const float view_right = strip_right - arrow_width > view_left ? strip_right - arrow_width : view_left;
        const float view_width = view_right - view_left;
        const float max_scroll = overflow && row_width > view_width ? row_width - view_width : 0.0f;
        auto clamp_scroll = [&](float v) { return v < 0.0f ? 0.0f : (v > max_scroll ? max_scroll : v); };

        if (overflow) {
            // The menu showing comes into view whenever it changes (LB/RB, Tab, a click), with a
            // little of its neighbour beside it so it is clear the row goes on.
            if (count > 0 && tabs_followed_ != tab_id_) {
                const float margin = gap * 1.5f;
                float target = tabs_target_;
                if (tab_start[tab] - margin < target) target = tab_start[tab] - margin;
                if (tab_start[tab] + tab_width[tab] + margin > target + view_width) {
                    target = tab_start[tab] + tab_width[tab] + margin - view_width;
                }
                tabs_target_ = target;
                tabs_followed_ = tab_id_;
            }
            // The mouse: the wheel (either one) or a drag over the strip.
            const bool over_strip = ImGui::IsWindowHovered() && io.MousePos.x >= strip_left
                                    && io.MousePos.x < strip_right;
            if (over_strip && !pick_.active) {
                const float wheel = io.MouseWheel + io.MouseWheelH;   // up / left: back to the start
                if (wheel != 0.0f) tabs_target_ = tabs_scroll_ - wheel * line * 3.0f;
                if (ImGui::IsMouseClicked(0)) tabs_dragging_ = true;
            }
            if (tabs_dragging_ && ImGui::IsMouseDown(0)) {
                if (io.MouseDelta.x != 0.0f) {
                    tabs_scroll_ = clamp_scroll(tabs_scroll_ - io.MouseDelta.x);
                    tabs_target_ = tabs_scroll_;
                }
            } else {
                tabs_dragging_ = false;
            }
            tabs_target_ = clamp_scroll(tabs_target_);
            const float t = tabs_snap_ ? 1.0f : (io.DeltaTime * 14.0f < 1.0f ? io.DeltaTime * 14.0f : 1.0f);
            tabs_scroll_ += (tabs_target_ - tabs_scroll_) * t;
            if (std::fabs(tabs_target_ - tabs_scroll_) < 0.5f) tabs_scroll_ = tabs_target_;
        } else {
            tabs_scroll_ = tabs_target_ = 0.0f;
            tabs_followed_.clear();   // so the menu showing is followed once the row overflows
            tabs_dragging_ = false;
        }
        tabs_snap_ = false;

        // A click that was really a drag picks no menu.
        const float drag = io.MouseDragThreshold;
        const bool click_ok = io.MouseDragMaxDistanceSqr[0] < drag * drag;
        tab_x_.assign(count, 0.0f);
        ImGui::PushClipRect(ImVec2(view_left, viewport->Pos.y),
                            ImVec2(view_right, viewport->Pos.y + bar_height), true);
        for (uint32_t i = 0; i < count; ++i) {
            const float x = view_left - tabs_scroll_ + tab_start[i];
            // Where the menu hangs from: its tab, or the end of the strip it is scrolled past.
            tab_x_[i] = x < view_left ? view_left : (x > view_right ? view_right : x);
            if (x + tab_width[i] <= view_left || x >= view_right) continue;   // off the strip
            ImGui::SetCursorScreenPos(ImVec2(x, strip_y));
            const bool current = static_cast<int>(i) == tab;
            ImGui::PushID(static_cast<int>(i));
            ImGui::PushStyleColor(ImGuiCol_Text, current ? kName : kText);
            if (ImGui::Selectable(groups[i].title, current, 0, ImVec2(tab_width[i], line))
                && !pick_.active && !current && click_ok) {
                tab_id_ = std::string(groups[i].mod_name) + "\x1f" + groups[i].title;
                row_ = 0;
                show_advanced_ = false;
            }
            ImGui::PopStyleColor();
            ImGui::PopID();
        }
        ImGui::PopClipRect();

        if (overflow) {
            // Fades where the row runs under the ends, and an arrow at each end: bright while there
            // are menus beyond it (a click scrolls most of a strip's width that way), dim at the end.
            const ImVec4 bg = ImGui::GetStyleColorVec4(ImGuiCol_WindowBg);
            const ImU32 solid = ImGui::GetColorU32(ImVec4(bg.x, bg.y, bg.z, 0.96f));
            const ImU32 clear = ImGui::GetColorU32(ImVec4(bg.x, bg.y, bg.z, 0.0f));
            const float fade = line * 1.5f;
            const float top = viewport->Pos.y, bottom = viewport->Pos.y + bar_height - 2.0f;
            const bool more_left = tabs_scroll_ > 0.5f, more_right = tabs_scroll_ < max_scroll - 0.5f;
            if (more_left) {
                draw->AddRectFilledMultiColor(ImVec2(view_left, top), ImVec2(view_left + fade, bottom),
                                              solid, clear, clear, solid);
            }
            if (more_right) {
                draw->AddRectFilledMultiColor(ImVec2(view_right - fade, top), ImVec2(view_right, bottom),
                                              clear, solid, solid, clear);
            }
            const float cy = strip_y + line * 0.5f, half = line * 0.32f;
            for (int side = 0; side < 2; ++side) {
                const bool more = side == 0 ? more_left : more_right;
                const float x0 = side == 0 ? strip_left : strip_right - arrow_width;
                ImGui::SetCursorScreenPos(ImVec2(x0, strip_y));
                ImGui::PushID(side == 0 ? "##more_left" : "##more_right");
                if (ImGui::InvisibleButton("arrow", ImVec2(arrow_width, line)) && more && !pick_.active) {
                    tabs_target_ = clamp_scroll(tabs_target_ + (side == 0 ? -1.0f : 1.0f) * view_width * 0.7f);
                }
                ImGui::PopID();
                const float cx = x0 + arrow_width * 0.5f;
                const ImU32 color = ImGui::GetColorU32(more ? kName : kRule);
                if (side == 0) {
                    draw->AddTriangleFilled(ImVec2(cx - half * 0.6f, cy), ImVec2(cx + half * 0.6f, cy - half),
                                            ImVec2(cx + half * 0.6f, cy + half), color);
                } else {
                    draw->AddTriangleFilled(ImVec2(cx + half * 0.6f, cy), ImVec2(cx - half * 0.6f, cy + half),
                                            ImVec2(cx - half * 0.6f, cy - half), color);
                }
            }
        }
        if (count == 0) {
            ImGui::SetCursorScreenPos(ImVec2(strip_left, strip_y));
            ImGui::TextColored(kHint, "no mod has registered any settings");
        }
        if (!hint_.empty()) {
            ImGui::SetCursorScreenPos(ImVec2(viewport->Pos.x + viewport->Size.x - hint_width - bar_pad.x, strip_y));
            ImGui::TextColored(kHint, "%s", hint_.c_str());
        }
        const float y = viewport->Pos.y + bar_height - 1.0f;
        draw->AddLine(ImVec2(viewport->Pos.x, y), ImVec2(viewport->Pos.x + viewport->Size.x, y),
                      ImGui::GetColorU32(kRule), 2.0f);
    }
    ImGui::End();
    ImGui::PopStyleVar(3);

    // ---- the open menu, hanging from its tab
    if (group != nullptr) {
        const float min_width = kMenuMinWidth * s;
        float x = tab < static_cast<int>(tab_x_.size()) ? tab_x_[tab] - 14.0f * s : 0.0f;
        const float width = menu_width_ > min_width ? menu_width_ : min_width;
        if (x + width > viewport->Pos.x + viewport->Size.x) x = viewport->Pos.x + viewport->Size.x - width;
        if (x < viewport->Pos.x) x = viewport->Pos.x;
        ImGui::SetNextWindowPos(ImVec2(x, viewport->Pos.y + bar_height));
        ImGui::SetNextWindowSizeConstraints(
            ImVec2(min_width, 0.0f),
            ImVec2(viewport->Size.x, viewport->Size.y - bar_height - 12.0f * s));
        const bool menu_shown = ImGui::Begin("##atmt_settings_menu", nullptr, fixed | ImGuiWindowFlags_AlwaysAutoResize);
        ImGui::BringWindowToDisplayFront(ImGui::GetCurrentWindow());
        if (menu_shown) {
            menu_width_ = ImGui::GetWindowWidth();
            const float value_width = 190.0f * s;
            // ---- what the mod is for (its AtmtModDescription), above its settings
            if (group->description != nullptr && group->description[0] != '\0') {
                ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + min_width - 28.0f * s);
                ImGui::TextColored(kHint, "%s", group->description);
                ImGui::PopTextWrapPos();
                ImGui::Separator();
            }
            if (ImGui::BeginTable("##rows", 2, ImGuiTableFlags_None)) {
                ImGui::TableSetupColumn("label", ImGuiTableColumnFlags_WidthFixed,
                                        min_width - value_width - 40.0f * s);
                ImGui::TableSetupColumn("value", ImGuiTableColumnFlags_WidthFixed, value_width);
                for (int r = 0; r < static_cast<int>(rows.size()); ++r) {
                    ImGui::TableNextRow();
                    ImGui::TableSetColumnIndex(0);
                    ImGui::PushID(r);
                    const bool is_current = r == row_;
                    if (rows[r].setting < 0) {
                        char label[64];
                        _snprintf(label, sizeof(label), "%s advanced settings (%d)",
                                  show_advanced_ ? "Hide" : "Show", advanced);
                        ImGui::PushStyleColor(ImGuiCol_Text, kHint);
                        if (ImGui::Selectable(label, is_current,
                                              ImGuiSelectableFlags_SpanAllColumns)
                            && !pick_.active) {
                            row_ = r;
                            show_advanced_ = !show_advanced_;
                        }
                        ImGui::PopStyleColor();
                        if (is_current && scroll_to_row_) ImGui::SetScrollHereY(0.5f);
                        ImGui::PopID();
                        continue;
                    }
                    const AtmtSetting& setting = group->settings[rows[r].setting];
                    if (setting.type == ATMT_SETTING_LABEL) {
                        if (setting.label != nullptr) {
                            ImGui::Dummy(ImVec2(0.0f, 2.0f * s));
                            ImGui::TextColored(kTitle, "%s", setting.label);
                        } else {
                            ImGui::Separator();
                        }
                        ImGui::PopID();
                        continue;
                    }
                    const bool clicked = ImGui::Selectable(
                        LabelOf(setting), is_current,
                        ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowOverlap);
                    if (is_current && scroll_to_row_) ImGui::SetScrollHereY(0.5f);
                    if ((setting.flags & ATMT_SETTING_RESTART) != 0) {
                        ImGui::SameLine();
                        ImGui::TextColored(kTime, "(restart)");
                    }
                    if (clicked && !pick_.active) {
                        row_ = r;
                        Change(api, setting, 0, true);
                    }

                    ImGui::TableSetColumnIndex(1);
                    const bool picking_this =
                        pick_.active && setting.key != nullptr && pick_.key == setting.key;
                    if (picking_this) {
                        const bool blink = (GetTickCount() / 400) % 2 == 0;
                        // The modifiers held so far, so a chord being built shows ("Ctrl+...").
                        std::string prompt = "press a key...";
                        if (pick_.pad) {
                            prompt = "press buttons...";
                        } else if (const unsigned mods = atmt::ModifiersDown()) {
                            prompt = atmt::KeyChordName(mods | VK_F1);
                            prompt = prompt.substr(0, prompt.size() - 2) + "...";
                        }
                        ImGui::TextColored(blink ? kName : kHint, "%s", prompt.c_str());
                    } else if (Adjustable(setting)) {
                        if (ImGui::SmallButton("<") && !pick_.active) {
                            row_ = r;
                            Change(api, setting, -1, false);
                        }
                        ImGui::SameLine();
                        ImGui::TextColored(is_current ? kName : kText, "%s",
                                           ValueText(setting).c_str());
                        ImGui::SameLine();
                        if (ImGui::SmallButton(">") && !pick_.active) {
                            row_ = r;
                            Change(api, setting, +1, false);
                        }
                    } else if (setting.type != ATMT_SETTING_ACTION) {
                        ImGui::TextColored(IsReadonly(setting) || setting.type == ATMT_SETTING_STRING
                                               ? kHint
                                               : (is_current ? kName : kText),
                                           "%s", ValueText(setting).c_str());
                    }
                    ImGui::PopID();
                }
                ImGui::EndTable();
            }
            scroll_to_row_ = false;

            // ---- the help of the selected row: the pad has no hover, so this is its tooltip
            ImGui::Separator();
            ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + min_width - 28.0f * s);
            std::string help;
            if (!rows.empty() && rows[row_].setting < 0) {
                help = kAdvancedHelp;
            } else if (selected != nullptr) {
                if (selected->help != nullptr) help = selected->help;
                if (selected->type == ATMT_SETTING_STRING) {
                    help += help.empty() ? "" : "\n";
                    help += "(edit this one in the mod's ini)";
                } else if (selected->type == ATMT_SETTING_KEY) {
                    help += help.empty() ? "" : "\n";
                    help += "A / Enter, then press the key - hold Ctrl / Shift / Alt with it for a chord (Esc cancels)";
                } else if (selected->type == ATMT_SETTING_PAD_BUTTON) {
                    help += help.empty() ? "" : "\n";
                    help += "A / Enter, then press and release a button or a chord (Esc cancels)";
                }
            }
            if (!help.empty()) ImGui::TextColored(kHint, "%s", help.c_str());
            if (restart_pending_) {
                ImGui::TextColored(kTime, "changes marked (restart) apply the next time the game "
                                          "starts");
            }
            ImGui::PopTextWrapPos();
        }
        ImGui::End();
    }

    ImGui::PopStyleVar();
    return keep_open;
}

}  // namespace atmt_overlay
