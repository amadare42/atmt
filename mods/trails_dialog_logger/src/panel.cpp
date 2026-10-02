// panel.cpp - the panel's drawing: the game's dialog box, as far as ImGui can imitate it.
//
// Styling is the whole point of this file, so the colours and the geometry live here and nowhere
// else. The look is taken from the game's own message box (a dark navy sheet with a thin gold
// rule, the speaker on an amber "plate", warm white text) because the panel sits on top of that
// same box while the player reads.
#include "panel.h"

#include "imgui.h"
#include "atmt_theme.h"

#include <cstdio>
#include <string>

namespace atmt {

namespace {

// The palette and the metric scale are shared with the overlay's settings bar (shared/atmt_theme.h).
// UiScale follows the text size being drawn now - the panel pushes its own (see panel_client.cpp).
using namespace theme;

constexpr float kPad = 14.0f;          // the sheet's inner margin, at kBaseFontSize

// The footer does not scale with OverlayFontSize (that was the earlier bug report: a large log
// font pushed the hint line wide enough to run past the sheet and get clipped). It is drawn at a
// small, fixed size instead, and the strip reserved for it is just as fixed - not measured - so
// its own height never depends on the log's font either.
constexpr float kFooterFontPx = 13.0f;
constexpr float kFooterHeightPx = 22.0f;

// How much smaller than the log's own text the footer is drawn, this frame - relative, because
// ImGui only offers a window font *scale*, not a point size, but the scale is chosen so the result
// is close to kFooterFontPx across the whole OverlayFontSize range (8..128) and never ends up
// larger than the log text itself at the small end of that range.
float FooterFontScale() {
    const float base = ImGui::GetFontSize();
    if (base <= 0.0f) return 1.0f;
    float scale = kFooterFontPx / base;
    if (scale > 1.0f) scale = 1.0f;
    return scale;
}

// Where the sheet sits, per OverlayAnchor. A name the mod does not know is reported by the caller
// and lands on BottomLeft here.
ImVec2 PanelPosition(const std::string& anchor, const ImVec2& viewport, const ImVec2& size) {
    const float margin = 18.0f * UiScale();
    const float bottom = viewport.y - size.y - margin;
    if (anchor == "TopLeft") return ImVec2(margin, margin);
    if (anchor == "TopRight") return ImVec2(viewport.x - size.x - margin, margin);
    if (anchor == "Center") return ImVec2((viewport.x - size.x) * 0.5f, (viewport.y - size.y) * 0.5f);
    if (anchor == "BottomRight") return ImVec2(viewport.x - size.x - margin, bottom);
    return ImVec2(margin, bottom);   // BottomLeft (the default)
}

// One logged line: when it was said, who said it, what was said. Shared by both orders.
void DrawLine(const Config& cfg, const LogLine& line) {
    if (cfg.overlay_timestamps && !line.time.empty()) {
        ImGui::TextColored(kTime, "%s", line.time.c_str());
        ImGui::SameLine();
    }
    if (!line.speaker.empty()) {
        ImGui::TextColored(kName, "%s", line.speaker.c_str());
        ImGui::SameLine();
    }
    ImGui::TextWrapped("%s", line.text.c_str());
}

// The footer is the panel's own help: a player who just opened it cannot know what it answers to,
// and it has to say whether the game is still receiving the keys being pressed. Kept short on
// purpose - it is still wrapped rather than trusted to fit (see Draw), but a hint that wraps to
// four lines at a large font size is not a hint any more.
std::string FooterHint(const Config& cfg, bool capture, bool following) {
    char buf[300];
    _snprintf(buf, sizeof(buf),
              "%s   %s hide   scroll d-pad/stick, resize R-stick   B/Esc close   "
              "Ctrl+Up/Dn size %upx   %s",
              following ? "following" : "scrolled back",
              cfg.overlay_key.empty() ? "F3" : cfg.overlay_key.c_str(), cfg.overlay_font_size,
              capture ? "input held" : "input passthrough");
    buf[sizeof(buf) - 1] = '\0';
    return std::string(buf);
}

}  // namespace

void DialogPanel::Draw(const PanelView& view, const PanelScroll& scroll) {
    if (view.lines == nullptr || view.config == nullptr) return;
    const Config& cfg = *view.config;
    const std::vector<LogLine>& lines = *view.lines;
    const float alpha = view.alpha;

    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    const ImVec2 size(viewport->Size.x * static_cast<float>(cfg.overlay_width_pct) / 100.0f,
                      viewport->Size.y * static_cast<float>(cfg.overlay_height_pct) / 100.0f);
    ImGui::SetNextWindowPos(PanelPosition(cfg.overlay_anchor, viewport->Size, size));
    ImGui::SetNextWindowSize(size);
    ImGui::SetNextWindowBgAlpha(alpha * static_cast<float>(cfg.overlay_opacity_pct) / 100.0f);

    // No title bar (the header below says more), no resize/collapse, and above all
    // NoSavedSettings: a mod must not leave an imgui.ini in the player's game folder.
    // NoScrollbar: only the "##lines" child scrolls. Without it, a footer hint that wraps to more
    // than kFooterHeightPx of text overflows the outer window's fixed size and ImGui adds a second,
    // outer scrollbar next to the child's own - the sheet is a fixed frame, not a scrolling page.
    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize
                                   | ImGuiWindowFlags_NoCollapse
                                   | ImGuiWindowFlags_NoSavedSettings
                                   | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav
                                   | ImGuiWindowFlags_NoScrollbar;
    // The shared style is sized for the overlay's own text; the sheet's metrics follow the panel's
    // (OverlayFontSize), so a larger log text is a larger sheet, not cramped text in the same one.
    const float s = UiScale();
    ImGui::PushStyleVar(ImGuiStyleVar_Alpha, alpha);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(kPad * s, kPad * s));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 6.0f * s);
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(8.0f * s, 6.0f * s));
    ImGui::PushStyleVar(ImGuiStyleVar_ScrollbarSize, 12.0f * s);
    const bool shown = ImGui::Begin("##atmt_dialog_log", nullptr, flags);
    if (shown) {
        // ---- header: what this is, and how much of it there is
        ImGui::TextColored(kTitle, "DIALOG LOG");
        if (!lines.empty()) {
            char right[128];
            _snprintf(right, sizeof(right), "%llu line(s)   #%llu..#%llu",
                      static_cast<unsigned long long>(lines.size()),
                      static_cast<unsigned long long>(lines.front().seq),
                      static_cast<unsigned long long>(lines.back().seq));
            const float width = ImGui::CalcTextSize(right).x;
            ImGui::SameLine(ImGui::GetWindowWidth() - width - kPad * UiScale() * 2.0f);
            ImGui::TextColored(kHint, "%s", right);
        }
        ImGui::Separator();   // the gold rule: the style's separator colour is the rule colour

        const std::string footer = FooterHint(cfg, view.capture, follow_);

        const float line_height = ImGui::GetTextLineHeightWithSpacing();
        ImGui::BeginChild("##lines", ImVec2(0.0f, -kFooterHeightPx), false);
        {
            if (lines.empty()) {
                ImGui::TextColored(kHint,
                                   "nothing logged yet - the panel fills as the game shows dialog");
            }
            // The order is the player's (OverlayNewestFirst): oldest first is a chronicle that follows
            // the newest line at its bottom, newest first is a tail that follows at its top.
            const bool newest_first = cfg.overlay_newest_first;
            if (newest_first) {
                for (auto it = lines.rbegin(); it != lines.rend(); ++it) DrawLine(cfg, *it);
            } else {
                for (const LogLine& line : lines) DrawLine(cfg, line);
            }
            // A little breathing room below the last line, so it does not sit flush against the
            // child's own bottom edge - part of the scrollable content (counted in content_end/
            // max_scroll below), so following the newest line still leaves this gap visible rather
            // than scrolling past it.
            ImGui::Dummy(ImVec2(0.0f, 6.0f * UiScale()));

            // ---- the scroll, from whichever input arrived this frame
            //
            // Two facts about ImGui shape this. SetScrollY only sets a *target*, applied when the next
            // frame starts, and GetScrollMaxY() is the *previous* frame's content size - so
            // SetScrollY(GetScrollMaxY()) is always one frame of growth short: the newest line sat just
            // below the view, which is exactly what the player reported ("the latest line is not
            // scrolled to") - until the dialog paused or they scrolled down by hand, at which point the
            // content stopped growing and the numbers agreed. SetScrollHereY(1.0f) is computed from the
            // cursor *now*, so it lands on the newest line every frame, growing or not.
            //
            // And scrolling is the player's: an explicit scroll wins over following (the target no
            // longer fights it), and following resumes as soon as they are back at the newest line.
            const float content_end = ImGui::GetCursorPosY();   // where the last line ends
            float max_scroll = content_end - ImGui::GetWindowHeight();
            if (max_scroll < 0.0f) max_scroll = 0.0f;   // the log fits: there is nothing to scroll

            // The wheel is ImGui's own doing (it scrolls the child under the cursor); all it needs is
            // a meaning: away from the newest line is reading back, back to it starts following. Its
            // step is clamped by the previous frame's maximum, so here - and only here - the newest
            // line counts as reached with one line of slack.
            const float wheel = ImGui::GetIO().MouseWheel;
            if (wheel != 0.0f) {
                const bool towards_newest = newest_first ? wheel > 0.0f : wheel < 0.0f;
                const float position = ImGui::GetScrollY();
                if (!towards_newest) follow_ = false;
                else follow_ = newest_first ? position <= line_height
                                            : position >= max_scroll - line_height;
            }
            if (scroll.to_top) {
                follow_ = false;   // Home: the beginning of the log, whichever end that is here
                if (newest_first) ImGui::SetScrollHereY(1.0f);
                else ImGui::SetScrollY(0.0f);
            }
            if (scroll.to_bottom) {
                follow_ = true;    // End, or A on the pad: the newest line, and keep following it
            }
            if (scroll.lines != 0 || scroll.pages != 0) {
                float delta = static_cast<float>(scroll.lines) * line_height;
                if (scroll.pages != 0) {
                    delta += static_cast<float>(scroll.pages)
                             * (ImGui::GetWindowHeight() - line_height);
                }
                // Clamped here rather than left to ImGui: the position this scroll ends at is what
                // decides whether it was "reading back" or "back at the newest line". Exactly at the
                // end counts - a pixel, not a line, because scrolling back *one* line has to move the
                // view (with a line of slack here, reading back would be undone by the follow below).
                float wanted = ImGui::GetScrollY() + delta;
                if (wanted < 0.0f) wanted = 0.0f;
                if (wanted > max_scroll) wanted = max_scroll;
                ImGui::SetScrollY(wanted);
                follow_ = newest_first ? wanted <= 1.0f : wanted >= max_scroll - 1.0f;
            }
            if (follow_) {
                if (newest_first) ImGui::SetScrollY(0.0f);
                else ImGui::SetScrollHereY(1.0f);   // the last item, at the bottom of the sheet
            }
        }
        ImGui::EndChild();

        // ---- footer: what the panel answers to, and whether the game is getting the keys too.
        // Drawn small and fixed (see kFooterFontPx above) rather than at the log's own size, so its
        // own strip stays the constant kFooterHeightPx regardless of OverlayFontSize.
        ImGui::SetWindowFontScale(FooterFontScale());
        ImGui::PushStyleColor(ImGuiCol_Text, kHint);
        ImGui::TextWrapped("%s", footer.c_str());
        ImGui::PopStyleColor();
        ImGui::SetWindowFontScale(1.0f);
    }
    ImGui::End();
    ImGui::PopStyleVar(5);
}
}  // namespace atmt


