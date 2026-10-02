// panel_font.cpp - the dialog log drawn with the game's own font (itf_font.h).
//
// An ImGui 1.92 font loader whose glyphs come from the .itf bitmaps: ImGui asks for a glyph at the
// size being drawn: box-filtered down from the font's cell (48 px for the HD Pack's font, 100 px for
// the loose HD file), or bilinear when the panel's size is above the cell. The font is added
// to the overlay's atlas (the panel draws in the overlay's context) and only the panel pushes it.
//
// Atlas changes happen in the panel's update() - on the render thread, between ImGui frames - never
// in draw(), and the font is taken out again before this dll goes away (RemovePanelFont), because
// the atlas keeps calling the loader's functions for as long as the font is in it.
#include "panel_font.h"

#include "imgui.h"
#include "imgui_internal.h"
#include "itf_font.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <cmath>
#include <vector>

namespace atmt {

namespace {

ItfFont g_itf;                // the render thread's: set before the font is added, read-only after
void* volatile g_offer = nullptr;   // an ItfFont* handed over by the thread that found it
ImFont* g_font = nullptr;
ImFontAtlas* g_atlas = nullptr;

// Where the baseline sits in the game's cell: measured on the HD font ('A' is 58 px tall from
// top 18, so its foot is at 76 of 100; descenders reach about 95). Only used for Ascent/Descent,
// which ImGui needs for little - the glyphs themselves are placed from the cell's top.
constexpr float kBaseline = 0.76f;

bool ItfSrcContainsGlyph(ImFontAtlas*, ImFontConfig*, ImWchar codepoint) {
    ItfGlyph g;
    return g_itf.Find(codepoint, &g);
}

bool ItfBakedInit(ImFontAtlas*, ImFontConfig* src, ImFontBaked* baked, void*) {
    if (!src->MergeMode) {
        baked->Ascent = std::ceil(baked->Size * kBaseline);
        baked->Descent = -std::floor(baked->Size * (1.0f - kBaseline));
    }
    return true;
}

// The glyph at baked->Size: a box filter over the 4-bit source, each output pixel the coverage-
// weighted mean of the source pixels under it.
bool ItfBakedLoadGlyph(ImFontAtlas* atlas, ImFontConfig* src, ImFontBaked* baked, void*,
                       ImWchar codepoint, ImFontGlyph* out_glyph, float* out_advance_x) {
    ItfGlyph g;
    if (!g_itf.Find(codepoint, &g)) return false;
    const float layout = baked->Size / static_cast<float>(g_itf.cell_size());
    // The game moves the pen by the advance *plus* the left bearing (the glyph is drawn at pen +
    // left, and the same distance is left after it). Fitted on a line of the game's own message box
    // at 2560x1440: that rule lands every glyph within a pixel at exactly 2x the 48 px cell, where
    // the bare advance drifted 5 px per character - the log looked cramped next to the box.
    const float advance = static_cast<float>(g.advance + g.left) * layout;
    if (out_advance_x != nullptr) {
        *out_advance_x = advance;
        return true;
    }
    out_glyph->Codepoint = codepoint;
    out_glyph->AdvanceX = advance;
    if (g.w <= 1 || g.h <= 1) return true;   // the space: an advance and nothing to draw

    const float density = src->RasterizerDensity * baked->RasterizerDensity;
    const float scale = layout * density;   // source pixels -> texture pixels
    const float x0 = static_cast<float>(g.left) * scale;
    const float y0 = static_cast<float>(g.top) * scale;
    // The output covers the scaled glyph, starting at a whole pixel: the fraction is folded into
    // the filter so the glyph does not shift by up to a pixel.
    const int ox = static_cast<int>(std::floor(x0));
    const int oy = static_cast<int>(std::floor(y0));
    const float fx = x0 - static_cast<float>(ox);
    const float fy = y0 - static_cast<float>(oy);
    const int w = static_cast<int>(std::ceil(fx + static_cast<float>(g.w) * scale)) + 1;
    const int h = static_cast<int>(std::ceil(fy + static_cast<float>(g.h) * scale)) + 1;
    if (w <= 0 || h <= 0) return true;

    ImFontAtlasRectId pack_id = ImFontAtlasPackAddRect(atlas, w, h);
    if (pack_id == ImFontAtlasRectId_Invalid) return false;
    ImTextureRect* r = ImFontAtlasPackGetRect(atlas, pack_id);
    ImFontAtlasBuilder* builder = atlas->Builder;
    builder->TempBuffer.resize(w * h);
    unsigned char* out = builder->TempBuffer.Data;

    const float inv = 1.0f / scale;
    if (scale > 1.0f) {
        // Enlarging (a size above the font's cell, e.g. 48 px cells drawn at 96): bilinear, as the
        // game's own box does it - a box filter here would only repeat source pixels, blocky.
        for (int dy = 0; dy < h; ++dy) {
            const float sy = (static_cast<float>(dy) + 0.5f - fy) * inv - 0.5f;
            const int y0i = static_cast<int>(std::floor(sy));
            const float ty = sy - static_cast<float>(y0i);
            for (int dx = 0; dx < w; ++dx) {
                const float sx = (static_cast<float>(dx) + 0.5f - fx) * inv - 0.5f;
                const int x0i = static_cast<int>(std::floor(sx));
                const float tx = sx - static_cast<float>(x0i);
                auto at = [&](int x, int y) -> float {
                    return (x < 0 || y < 0 || x >= g.w || y >= g.h)
                               ? 0.0f : static_cast<float>(ItfFont::Pixel(g, x, y));
                };
                const float top = at(x0i, y0i) * (1.0f - tx) + at(x0i + 1, y0i) * tx;
                const float bottom = at(x0i, y0i + 1) * (1.0f - tx) + at(x0i + 1, y0i + 1) * tx;
                const float v = (top * (1.0f - ty) + bottom * ty) * (255.0f / 15.0f);
                out[dy * w + dx] = static_cast<unsigned char>((v > 255.0f ? 255.0f : v) + 0.5f);
            }
        }
    } else {
        // Separable box filter: per output column/row, the source span it covers and the partial
        // weights at its ends. inv = source pixels per output pixel.
        std::vector<float> row(static_cast<size_t>(w) * static_cast<size_t>(g.h), 0.0f);
        for (int sy = 0; sy < g.h; ++sy) {
            for (int dx = 0; dx < w; ++dx) {
                const float a = (static_cast<float>(dx) - fx) * inv;
                const float b = a + inv;
                const int s0 = a < 0.0f ? 0 : static_cast<int>(a);
                const int s1 = static_cast<int>(std::ceil(b)) < g.w ? static_cast<int>(std::ceil(b)) : g.w;
                float sum = 0.0f;
                for (int sx = s0; sx < s1; ++sx) {
                    const float lo = static_cast<float>(sx) > a ? static_cast<float>(sx) : a;
                    const float hi = static_cast<float>(sx + 1) < b ? static_cast<float>(sx + 1) : b;
                    if (hi > lo) sum += (hi - lo) * static_cast<float>(ItfFont::Pixel(g, sx, sy));
                }
                row[static_cast<size_t>(sy) * w + dx] = sum;
            }
        }
        for (int dy = 0; dy < h; ++dy) {
            const float a = (static_cast<float>(dy) - fy) * inv;
            const float b = a + inv;
            const int s0 = a < 0.0f ? 0 : static_cast<int>(a);
            const int s1 = static_cast<int>(std::ceil(b)) < g.h ? static_cast<int>(std::ceil(b)) : g.h;
            for (int dx = 0; dx < w; ++dx) {
                float sum = 0.0f;
                for (int sy = s0; sy < s1; ++sy) {
                    const float lo = static_cast<float>(sy) > a ? static_cast<float>(sy) : a;
                    const float hi = static_cast<float>(sy + 1) < b ? static_cast<float>(sy + 1) : b;
                    if (hi > lo) sum += (hi - lo) * row[static_cast<size_t>(sy) * w + dx];
                }
                // sum is in source-pixel area x 0..15; the output pixel covers inv*inv of them
                float v = sum * scale * scale * (255.0f / 15.0f);
                if (v > 255.0f) v = 255.0f;
                out[dy * w + dx] = static_cast<unsigned char>(v + 0.5f);
            }
        }
    }

    const float recip = 1.0f / density;
    out_glyph->X0 = static_cast<float>(ox) * recip;
    out_glyph->Y0 = static_cast<float>(oy) * recip;
    out_glyph->X1 = static_cast<float>(ox + static_cast<int>(r->w)) * recip;
    out_glyph->Y1 = static_cast<float>(oy + static_cast<int>(r->h)) * recip;
    out_glyph->Visible = true;
    out_glyph->PackId = pack_id;
    ImFontAtlasBakedSetFontGlyphBitmap(atlas, baked, src, out_glyph, r, out, ImTextureFormat_Alpha8, w);
    return true;
}

const ImFontLoader* ItfLoader() {
    static ImFontLoader loader;
    loader.Name = "atmt_itf";
    loader.FontSrcContainsGlyph = ItfSrcContainsGlyph;
    loader.FontBakedInit = ItfBakedInit;
    loader.FontBakedLoadGlyph = ItfBakedLoadGlyph;
    return &loader;
}

}  // namespace

void OfferPanelFont(ItfFont&& font) {
    ItfFont* offered = new ItfFont(static_cast<ItfFont&&>(font));
    ItfFont* old = static_cast<ItfFont*>(InterlockedExchangePointer(&g_offer, offered));
    delete old;
}

ImFont* PanelFont() {
    return g_font;
}

ImFont* AddPanelFont() {
    if (g_font != nullptr) return g_font;
    if (ItfFont* offered = static_cast<ItfFont*>(InterlockedExchangePointer(&g_offer, nullptr))) {
        g_itf = static_cast<ItfFont&&>(*offered);
        delete offered;
    }
    if (g_itf.empty()) return nullptr;
    ImFontAtlas* atlas = ImGui::GetIO().Fonts;
    if (atlas == nullptr || atlas->Locked) return nullptr;
    ImFontConfig cfg;
    cfg.FontLoader = ItfLoader();
    cfg.SizePixels = static_cast<float>(g_itf.cell_size());
    cfg.FontDataOwnedByAtlas = false;
    ImFormatString(cfg.Name, IM_ARRAYSIZE(cfg.Name), "game font (%d px)", g_itf.cell_size());
    g_font = atlas->AddFont(&cfg);
    if (g_font != nullptr) {
        g_atlas = atlas;
        // The overlay's own font fills in what the game's lacks (rare: the footer's punctuation is
        // all there), instead of '?'.
        if (!atlas->Fonts.empty() && atlas->Fonts[0] != g_font) {
            ImFontConfig merge;
            merge.MergeMode = true;
            merge.DstFont = g_font;
            // a second source of the same font: the overlay's default font, by its own loader
            const ImFontConfig* base = atlas->Fonts[0]->Sources.empty() ? nullptr
                                                                          : atlas->Fonts[0]->Sources[0];
            if (base != nullptr && base->FontData != nullptr) {
                merge.FontData = base->FontData;
                merge.FontDataSize = base->FontDataSize;
                merge.FontDataOwnedByAtlas = false;
                merge.FontNo = base->FontNo;
                merge.FontLoader = base->FontLoader;
                ImFormatString(merge.Name, IM_ARRAYSIZE(merge.Name), "%s", base->Name);
                atlas->AddFont(&merge);
            }
        }
    }
    return g_font;
}

void RemovePanelFont() {
    if (g_font == nullptr) return;
    if (g_atlas != nullptr && !g_atlas->Locked) g_atlas->RemoveFont(g_font);
    g_font = nullptr;
    g_atlas = nullptr;
}

}  // namespace atmt
