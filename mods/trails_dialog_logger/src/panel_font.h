// panel_font.h - the dialog log drawn with the game's own font (panel_font.cpp, itf_font.h).
#pragma once

struct ImFont;

namespace atmt {

class ItfFont;

// Any thread: hands over the font that was found. The render thread takes it at the next
// AddPanelFont.
void OfferPanelFont(ItfFont&& font);
// Render thread, between ImGui frames (the panel's update): adds the offered font to the current
// context's atlas once. Returns it, or null while there is none.
ImFont* AddPanelFont();
// The font once added, or null (the panel then draws with the overlay's).
ImFont* PanelFont();
// Render thread, between frames: takes the font out of the atlas again - before this dll goes away,
// since the atlas calls the loader's functions for as long as the font is in it.
void RemovePanelFont();

}  // namespace atmt
