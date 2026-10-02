// itf_font.h - the game's own font (Falcom's .itf), read so the dialog log can draw with it.
//
// The layout, measured on data/fonts/font_us(_hd).itf (docs/ENGINE_NOTES.md, "Fonts"):
//
//   +0x00  u8  1, u8 1, u16 size     the cell size the glyphs are drawn for (100 HD, 32 SD)
//   +0x04  u32 count                 entries in the glyph table
//   +0x40  { u32 codepoint, u32 offset } x count     Unicode code points
//   glyph  u16 w, u16 h, i16 top, i16 left, u16 advance, u16 flags, then w*h 4-bit pixels,
//          packed without row padding, low nibble first
//
// The codes are Unicode, and the font carries Falcom's own symbols at codes the scripts use for
// them (U+3231 is the heart) - so drawing the text with this font shows those as the game does.
//
// Deliberately free of ImGui: the self test checks the parsing on a hand-made font.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace atmt {

struct ItfGlyph {
    int w = 0;
    int h = 0;
    int top = 0;       // from the top of the cell
    int left = 0;      // from the pen position
    int advance = 0;
    const uint8_t* pixels = nullptr;   // w*h nibbles, low nibble first
};

class ItfFont {
public:
    // Takes a copy of the bytes; false (and the font stays empty) when they are not an .itf.
    bool Load(const uint8_t* data, size_t size);
    bool empty() const { return glyphs_.empty(); }
    int cell_size() const { return cell_size_; }
    size_t glyph_count() const { return glyphs_.size(); }
    size_t byte_size() const { return bytes_.size(); }
    bool Find(uint32_t codepoint, ItfGlyph* out) const;
    // 0..15 at (x, y) of a glyph.
    static int Pixel(const ItfGlyph& g, int x, int y);

private:
    std::vector<uint8_t> bytes_;
    std::unordered_map<uint32_t, uint32_t> glyphs_;   // codepoint -> offset
    int cell_size_ = 0;
};

// The font the game loaded, found in its memory: the game reads the whole file into one buffer
// and keeps it (FUN_005a29d0), so these are the bytes it actually draws with - whatever SenPatcher
// or an HD pack put in place of data/fonts. Scans committed read/write memory once; the largest
// valid font wins (the HD font over the SD one). `where` receives a note for the log.
bool FindGameFontInMemory(ItfFont* out, std::string* where);

// A loose file, for when the game's copy is not found.
bool LoadItfFile(const std::wstring& path, ItfFont* out);

}  // namespace atmt
