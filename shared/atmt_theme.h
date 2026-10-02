// atmt_theme.h - the game's own palette, shared by everything drawn in the overlay.
//
// Sampled from the game's message box (a dark navy sheet with a thin gold rule, the speaker on an
// amber plate, warm white text): the overlay's settings bar and every window a mod draws in it (the
// dialog log's panel) sit on top of the game's own UI, so they borrow its look. Needs imgui.h.
#ifndef ATMT_THEME_H
#define ATMT_THEME_H

#include "imgui.h"

namespace atmt {
namespace theme {

const ImVec4 kSheet(0.055f, 0.075f, 0.170f, 1.00f);   // the box's fill
const ImVec4 kRule(0.66f, 0.55f, 0.28f, 1.00f);       // the gold rule at its edges
const ImVec4 kTitle(0.87f, 0.76f, 0.45f, 1.00f);      // headings ("DIALOG LOG", a menu's sections)
const ImVec4 kName(0.98f, 0.80f, 0.42f, 1.00f);       // the speaker / a highlighted value
const ImVec4 kText(0.94f, 0.93f, 0.88f, 1.00f);       // body text
const ImVec4 kTime(0.50f, 0.55f, 0.66f, 1.00f);       // quiet detail (timestamps)
const ImVec4 kHint(0.55f, 0.58f, 0.66f, 1.00f);       // hints and footers

// The metrics were tuned at this text size (17 px was the first panel); everything is scaled from
// the size of the text being drawn *now* (ImGui::GetFontSize, which follows a PushFont), so a window
// drawn at a larger size gets a larger sheet rather than cramped text in the same one.
constexpr float kBaseFontSize = 17.0f;

inline float UiScale() {
    const float size = ImGui::GetFontSize();
    return size > 0.0f ? size / kBaseFontSize : 1.0f;
}

// The shared style: colours, and the metrics at `font_size`. Set by the overlay whenever its own
// text size changes; a window drawn at another size pushes its own padding (see panel.cpp).
inline void ApplyGameTheme(float font_size) {
    ImGuiStyle& style = ImGui::GetStyle();
    const float s = font_size > 0.0f ? font_size / kBaseFontSize : 1.0f;
    style.WindowRounding = 6.0f * s;
    style.WindowBorderSize = 2.0f;
    style.WindowPadding = ImVec2(14.0f * s, 14.0f * s);
    style.FrameRounding = 4.0f * s;
    style.ScrollbarRounding = 6.0f * s;
    style.ScrollbarSize = 12.0f * s;
    style.ItemSpacing = ImVec2(8.0f * s, 6.0f * s);
    style.WindowTitleAlign = ImVec2(0.0f, 0.5f);

    ImVec4* colors = style.Colors;
    colors[ImGuiCol_WindowBg] = kSheet;
    colors[ImGuiCol_Border] = kRule;
    colors[ImGuiCol_Text] = kText;
    colors[ImGuiCol_TextDisabled] = kHint;
    colors[ImGuiCol_Separator] = kRule;
    colors[ImGuiCol_ScrollbarBg] = ImVec4(0.02f, 0.03f, 0.07f, 0.35f);
    colors[ImGuiCol_ScrollbarGrab] = ImVec4(kRule.x, kRule.y, kRule.z, 0.65f);
    colors[ImGuiCol_ScrollbarGrabHovered] = ImVec4(kRule.x, kRule.y, kRule.z, 0.85f);
    colors[ImGuiCol_ScrollbarGrabActive] = kRule;
    colors[ImGuiCol_TitleBg] = kSheet;
    colors[ImGuiCol_TitleBgActive] = kSheet;
    colors[ImGuiCol_Header] = ImVec4(kRule.x, kRule.y, kRule.z, 0.30f);
    colors[ImGuiCol_HeaderHovered] = ImVec4(kRule.x, kRule.y, kRule.z, 0.20f);
    colors[ImGuiCol_HeaderActive] = ImVec4(kRule.x, kRule.y, kRule.z, 0.40f);
    colors[ImGuiCol_Button] = ImVec4(kRule.x, kRule.y, kRule.z, 0.15f);
    colors[ImGuiCol_ButtonHovered] = ImVec4(kRule.x, kRule.y, kRule.z, 0.35f);
    colors[ImGuiCol_ButtonActive] = ImVec4(kRule.x, kRule.y, kRule.z, 0.50f);
}

}  // namespace theme
}  // namespace atmt

#endif  // ATMT_THEME_H
