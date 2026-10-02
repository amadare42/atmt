// itf_font.cpp - the game's .itf font: parsing, and finding the copy the game loaded (itf_font.h).
#include "itf_font.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <cstdio>
#include <cstring>

namespace atmt {

namespace {

constexpr size_t kTableAt = 0x40;
constexpr size_t kGlyphHeader = 12;
constexpr size_t kMaxFontBytes = 64u << 20;   // the HD font is 17 MB

uint16_t U16(const uint8_t* p) { return static_cast<uint16_t>(p[0] | (p[1] << 8)); }
uint32_t U32(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8)
           | (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}

// The header alone: cheap enough to try at every aligned address of the game's memory.
bool LooksLikeHeader(const uint8_t* p, size_t avail) {
    if (avail < kTableAt + 8) return false;
    if (p[0] != 1 || p[1] != 1) return false;
    const unsigned cell = U16(p + 2);
    if (cell < 8 || cell > 256) return false;
    const uint32_t count = U32(p + 4);
    if (count < 1 || count > 200000) return false;
    for (size_t i = 0x10; i < kTableAt; ++i) {
        if (p[i] != 0) return false;
    }
    return true;
}

// The whole font: every table entry points at a glyph that fits, and 'A' is there. Returns the size
// the font really occupies (the end of its last glyph), or 0.
size_t ValidFontSize(const uint8_t* p, size_t avail) {
    if (!LooksLikeHeader(p, avail)) return 0;
    const uint32_t count = U32(p + 4);
    const size_t table_end = kTableAt + static_cast<size_t>(count) * 8;
    if (table_end > avail) return 0;
    size_t end = table_end;
    bool have_a = false;
    for (uint32_t i = 0; i < count; ++i) {
        const uint8_t* e = p + kTableAt + static_cast<size_t>(i) * 8;
        const uint32_t cp = U32(e);
        const uint32_t off = U32(e + 4);
        if (cp == 0xFFFFFFFFu) continue;   // unused slots
        if (off < table_end || off + kGlyphHeader > avail) return 0;
        const size_t w = U16(p + off);
        const size_t h = U16(p + off + 2);
        if (w > 1024 || h > 1024) return 0;
        const size_t glyph_end = off + kGlyphHeader + (w * h + 1) / 2;
        if (glyph_end > avail) return 0;
        if (glyph_end > end) end = glyph_end;
        if (cp == 'A') have_a = true;
    }
    return have_a ? end : 0;
}

// The game's memory, read without risking a fault: a region can be freed by the game's own
// threads while this runs, so everything goes through ReadProcessMemory on ourselves.
bool ReadSelf(uintptr_t at, void* out, size_t n) {
    SIZE_T got = 0;
    return ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<const void*>(at), out, n, &got)
           && got == n;
}

bool ScannableRegion(const MEMORY_BASIC_INFORMATION& mbi) {
    if (mbi.State != MEM_COMMIT || mbi.Type != MEM_PRIVATE) return false;
    if ((mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS)) != 0) return false;
    const DWORD rw = PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
    return (mbi.Protect & rw) != 0;
}

}  // namespace

bool ItfFont::Load(const uint8_t* data, size_t size) {
    bytes_.clear();
    glyphs_.clear();
    cell_size_ = 0;
    if (data == nullptr) return false;
    const size_t used = ValidFontSize(data, size);
    if (used == 0) return false;
    bytes_.assign(data, data + used);
    const uint8_t* p = bytes_.data();
    cell_size_ = U16(p + 2);
    const uint32_t count = U32(p + 4);
    glyphs_.reserve(count);
    for (uint32_t i = 0; i < count; ++i) {
        const uint8_t* e = p + kTableAt + static_cast<size_t>(i) * 8;
        const uint32_t cp = U32(e);
        if (cp != 0xFFFFFFFFu) glyphs_[cp] = U32(e + 4);
    }
    return true;
}

bool ItfFont::Find(uint32_t codepoint, ItfGlyph* out) const {
    const auto it = glyphs_.find(codepoint);
    if (it == glyphs_.end()) return false;
    const uint8_t* g = bytes_.data() + it->second;
    out->w = U16(g);
    out->h = U16(g + 2);
    out->top = static_cast<int16_t>(U16(g + 4));
    out->left = static_cast<int16_t>(U16(g + 6));
    out->advance = U16(g + 8);
    out->pixels = g + kGlyphHeader;
    return true;
}

int ItfFont::Pixel(const ItfGlyph& g, int x, int y) {
    const size_t i = static_cast<size_t>(y) * static_cast<size_t>(g.w) + static_cast<size_t>(x);
    const uint8_t b = g.pixels[i / 2];
    return (i & 1) == 0 ? (b & 0x0F) : (b >> 4);
}

bool FindGameFontInMemory(ItfFont* out, std::string* where) {
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    const uintptr_t lo = reinterpret_cast<uintptr_t>(si.lpMinimumApplicationAddress);
    const uintptr_t hi = reinterpret_cast<uintptr_t>(si.lpMaximumApplicationAddress);
    std::vector<uint8_t> chunk(1u << 20);
    uintptr_t best_at = 0;
    size_t best_size = 0;
    int best_cell = 0;
    unsigned candidates = 0;

    for (uintptr_t addr = lo; addr < hi;) {
        MEMORY_BASIC_INFORMATION mbi;
        if (VirtualQuery(reinterpret_cast<const void*>(addr), &mbi, sizeof(mbi)) == 0) break;
        const uintptr_t base = reinterpret_cast<uintptr_t>(mbi.BaseAddress);
        const uintptr_t end = base + mbi.RegionSize;
        if (end <= addr) break;
        if (ScannableRegion(mbi)) {
            for (uintptr_t at = base; at < end; at += chunk.size()) {
                const size_t n = static_cast<size_t>(end - at) < chunk.size()
                                     ? static_cast<size_t>(end - at) : chunk.size();
                if (!ReadSelf(at, chunk.data(), n)) break;
                for (size_t i = 0; i + 4 <= n; i += 4) {
                    // `01 01` then the cell size: the first word of a font
                    if (chunk[i] != 1 || chunk[i + 1] != 1 || chunk[i + 3] != 0) continue;
                    const uintptr_t cand = at + i;
                    const size_t avail = static_cast<size_t>(end - cand) < kMaxFontBytes
                                             ? static_cast<size_t>(end - cand) : kMaxFontBytes;
                    uint8_t head[kTableAt + 8];
                    if (avail < sizeof(head) || !ReadSelf(cand, head, sizeof(head))) continue;
                    // a real font has thousands of glyphs: fewer is noise that happens to match
                    if (!LooksLikeHeader(head, avail) || U32(head + 4) < 256) continue;
                    ++candidates;
                    std::vector<uint8_t> whole(avail);
                    if (!ReadSelf(cand, whole.data(), avail)) continue;
                    const size_t size = ValidFontSize(whole.data(), avail);
                    const int cell = U16(head + 2);
                    if (size != 0 && (cell > best_cell || (cell == best_cell && size > best_size))) {
                        best_at = cand;
                        best_size = size;
                        best_cell = cell;
                    }
                }
            }
        }
        addr = end;
    }
    if (best_at == 0) {
        if (where != nullptr) {
            char note[128];
            _snprintf(note, sizeof(note), "no font in memory (%u header(s) looked like one)", candidates);
            note[sizeof(note) - 1] = '\0';
            *where = note;
        }
        return false;
    }
    std::vector<uint8_t> bytes(best_size);
    if (!ReadSelf(best_at, bytes.data(), best_size) || !out->Load(bytes.data(), best_size)) return false;
    if (where != nullptr) {
        char note[160];
        _snprintf(note, sizeof(note), "the game's font at 0x%08x: %u glyphs, %d px cells, %u bytes",
                  static_cast<unsigned>(best_at), static_cast<unsigned>(out->glyph_count()),
                  out->cell_size(), static_cast<unsigned>(best_size));
        note[sizeof(note) - 1] = '\0';
        *where = note;
    }
    return true;
}

bool LoadItfFile(const std::wstring& path, ItfFont* out) {
    FILE* f = _wfopen(path.c_str(), L"rb");
    if (f == nullptr) return false;
    std::vector<uint8_t> bytes;
    uint8_t buf[1 << 16];
    size_t n = 0;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) {
        bytes.insert(bytes.end(), buf, buf + n);
        if (bytes.size() > kMaxFontBytes) break;
    }
    fclose(f);
    return out->Load(bytes.data(), bytes.size());
}

}  // namespace atmt
