// icon_pack.cpp - see icon_pack.h.
#include "icon_pack.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <mutex>

#include "falcom_pkg.h"
#include "hash.h"
#include "p3a.h"
#include "payload.h"
#include "platform.h"

namespace atmt {

namespace {

bool Fail(std::string* error, const std::string& why) {
    if (error != nullptr) *error = why;
    return false;
}

std::string ModsRel(const std::string& name) { return "mods/" + name; }

// What one file is, for the fingerprint: size and modification time.
std::string Stat(const fs::path& p) {
    std::error_code ec;
    const auto size = fs::file_size(p, ec);
    if (ec) return "missing";
    return std::to_string(size) + " " + std::to_string(MTime(p));
}

// ---------------------------------------------------------------- Lanczos as Pillow does it
// Pillow's libImaging/Resample.c (the 8-bit path) and Convert.c (RGBA <-> RGBa).
constexpr int kPrecisionBits = 32 - 8 - 2;

double Sinc(double x) {
    if (x == 0.0) return 1.0;
    x = x * 3.14159265358979323846;   // M_PI
    return std::sin(x) / x;
}

double Lanczos(double x) {
    if (-3.0 <= x && x < 3.0) return Sinc(x) * Sinc(x / 3);
    return 0.0;
}

struct Coeffs {
    int ksize = 0;
    std::vector<int> bounds;   // xmin, count per output pixel
    std::vector<int32_t> k;    // ksize per output pixel, fixed point
};

Coeffs PrecomputeCoeffs(int in_size, int out_size) {
    const float in0 = 0.0f, in1 = static_cast<float>(in_size);
    double scale = static_cast<double>(in1 - in0) / out_size;
    double filterscale = scale;
    if (filterscale < 1.0) filterscale = 1.0;
    const double support = 3.0 * filterscale;
    Coeffs c;
    c.ksize = static_cast<int>(std::ceil(support)) * 2 + 1;
    c.bounds.resize(static_cast<size_t>(out_size) * 2);
    std::vector<double> kk(static_cast<size_t>(out_size) * c.ksize, 0.0);
    for (int xx = 0; xx < out_size; ++xx) {
        const double center = in0 + (xx + 0.5) * scale;
        double ww = 0.0;
        const double ss = 1.0 / filterscale;
        int xmin = static_cast<int>(center - support + 0.5);
        if (xmin < 0) xmin = 0;
        int xmax = static_cast<int>(center + support + 0.5);
        if (xmax > in_size) xmax = in_size;
        xmax -= xmin;
        double* k = &kk[static_cast<size_t>(xx) * c.ksize];
        for (int x = 0; x < xmax; ++x) {
            const double w = Lanczos((x + xmin - center + 0.5) * ss);
            k[x] = w;
            ww += w;
        }
        for (int x = 0; x < xmax; ++x) {
            if (ww != 0.0) k[x] /= ww;
        }
        c.bounds[xx * 2] = xmin;
        c.bounds[xx * 2 + 1] = xmax;
    }
    c.k.resize(kk.size());
    for (size_t i = 0; i < kk.size(); ++i) {
        c.k[i] = kk[i] < 0 ? static_cast<int32_t>(-0.5 + kk[i] * (1 << kPrecisionBits))
                           : static_cast<int32_t>(0.5 + kk[i] * (1 << kPrecisionBits));
    }
    return c;
}

uint8_t Clip8(int32_t in) {
    if (in >= (1 << kPrecisionBits << 8)) return 255;
    if (in <= 0) return 0;
    return static_cast<uint8_t>(in >> kPrecisionBits);
}

uint8_t MulDiv255(unsigned a, unsigned b) {
    const unsigned tmp = a * b + 128;
    return static_cast<uint8_t>(((tmp >> 8) + tmp) >> 8);
}

// ---------------------------------------------------------------- texture layout
size_t MipBytes(int width, int height, bool mips) {
    size_t total = 0;
    for (;;) {
        total += static_cast<size_t>(width) * height * 4;
        if (!mips || (width == 1 && height == 1)) return total;
        width = std::max(1, width / 2);
        height = std::max(1, height / 2);
    }
}

// ---------------------------------------------------------------- sources
struct Archive {
    std::string name;
    P3aReader reader;
    bool ok = false;
    std::string error;
};

std::vector<Archive> OpenArchives(const fs::path& game) {
    std::vector<Archive> out;
    for (const std::string& name : ModLoadOrder(game)) {
        if (IEquals(name, kIconPackFile)) continue;
        const fs::path p = game / "mods" / Path(name);
        if (!Exists(p)) continue;
        Archive a;
        a.name = name;
        a.ok = a.reader.Open(p, &a.error);
        out.push_back(std::move(a));
    }
    return out;
}

enum class Found { Yes, No, Unreadable };

// The package as SenPatcher would load it: from the first archive that has it, else from the data
// folder. An archive that has it but cannot be read stops the search - a lower source would put
// other art over the texture mod's.
Found Resolve(const fs::path& game, const std::vector<Archive>& archives, const std::string& path, std::string* raw,
              std::string* source, std::string* error) {
    for (const Archive& a : archives) {
        if (!a.ok || !a.reader.Has(path)) continue;
        *source = a.name;
        return a.reader.Read(path, raw, error) ? Found::Yes : Found::Unreadable;
    }
    const fs::path disk = game / Path(path);
    if (Exists(disk) && ReadFile(disk, raw)) {
        *source = "game data";
        return Found::Yes;
    }
    return Found::No;
}

// One target on one texture: the changed texture, or false (skipped; why in `note`).
bool ApplyTarget(const IconPackTarget& t, const std::string& phyre, const std::string* original, std::string* result,
                 std::string* note) {
    TextureLayout lay;
    std::string why;
    if (!FindTextureLayout(phyre, t.texture_w, t.texture_h, &lay, &why)) return Fail(note, "skipped: " + why);
    int cell = lay.width / t.cell_grid;
    const int draw = t.draw_size;
    // "enlarge": an atlas too small for the trick (the game's own) is doubled until it is not; the
    // pictures then come from its own cells as they were, not the doubled ones.
    std::string enlarged;
    int factor = 1;
    while (t.enlarge && cell > 0 && cell * factor < 2 * draw) factor *= 2;
    if (factor > 1) {
        TextureLayout big;
        if (!EnlargeTexture(phyre, lay, factor, &enlarged, &big, &why)) return Fail(note, "skipped: cannot enlarge: " + why);
        if (original == nullptr) original = &phyre;
        lay = big;
        cell *= factor;
    }
    TextureLayout olay;
    int source_cell = cell;
    const uint8_t* source = nullptr;
    if (original != nullptr) {
        if (!FindTextureLayout(*original, 0, 0, &olay, &why)) return Fail(note, "skipped: the game's own texture: " + why);
        source_cell = olay.width / t.cell_grid;
        source = reinterpret_cast<const uint8_t*>(original->data()) + olay.offset;
    }
    if (static_cast<double>(cell) / draw < 2) {
        // Below 2x the game's bilinear footprints of neighbouring output pixels share texels, so no
        // stored cell reproduces the downscale exactly; such a texture is left alone.
        char buf[160];
        std::snprintf(buf, sizeof(buf), "skipped: %dpx cells drawn at %dpx (ratio %.2f < 2)", cell, draw,
                      static_cast<double>(cell) / draw);
        return Fail(note, buf);
    }
    std::string out = factor > 1 ? std::move(enlarged) : phyre;
    auto* pixels = reinterpret_cast<uint8_t*>(&out[0]) + lay.offset;
    const int grid_w = lay.width / cell, grid_h = lay.height / cell;
    std::vector<int> rows = t.rows, cols = t.cols;
    if (rows.empty()) {
        for (int r = 0; r < grid_h; ++r) rows.push_back(r);
    }
    if (cols.empty()) {
        for (int c = 0; c < grid_w; ++c) cols.push_back(c);
    }
    std::vector<uint8_t> picture(static_cast<size_t>(source_cell) * source_cell * 4);
    int count = 0;
    for (int row : rows) {
        for (int col : cols) {
            if (row < 0 || col < 0 || row >= grid_h || col >= grid_w) return Fail(note, "skipped: a cell outside the atlas");
            const uint8_t* from;
            int stride;
            if (source != nullptr) {
                if ((row + 1) * source_cell > olay.height || (col + 1) * source_cell > olay.width) {
                    return Fail(note, "skipped: a cell outside the game's own atlas");
                }
                from = source + (static_cast<size_t>(row) * source_cell * olay.width + static_cast<size_t>(col) * source_cell) * 4;
                stride = olay.width;
            } else {
                from = pixels + (static_cast<size_t>(row) * cell * lay.width + static_cast<size_t>(col) * cell) * 4;
                stride = lay.width;
            }
            for (int y = 0; y < source_cell; ++y) {
                std::memcpy(&picture[static_cast<size_t>(y) * source_cell * 4], from + static_cast<size_t>(y) * stride * 4,
                            static_cast<size_t>(source_cell) * 4);
            }
            const std::vector<uint8_t> stored = StretchNearest(ResizeLanczos(picture.data(), source_cell, draw), draw, cell);
            uint8_t* to = pixels + (static_cast<size_t>(row) * cell * lay.width + static_cast<size_t>(col) * cell) * 4;
            for (int y = 0; y < cell; ++y) {
                std::memcpy(to + static_cast<size_t>(y) * lay.width * 4, &stored[static_cast<size_t>(y) * cell * 4],
                            static_cast<size_t>(cell) * 4);
            }
            ++count;
        }
    }
    *note = std::to_string(count) + " cells of " + std::to_string(cell) + "px -> " + std::to_string(draw) + "px in a "
            + std::to_string(lay.width) + "x" + std::to_string(lay.height) + " atlas"
            + (factor > 1 ? " (enlarged " + std::to_string(factor) + "x)" : std::string())
            + (source != nullptr && t.original_art ? " (pictures: the game's own " + std::to_string(source_cell) + "px cells)" : std::string())
            + (lay.mips ? " (mips kept as they were)" : "");
    *result = std::move(out);
    return true;
}

// The cheap part of the fingerprint: when none of this changed, neither did the sources.
std::string QuickKey(const fs::path& game, const IconPackConfig& config) {
    std::string key = config.hash + "\n";
    std::string order;
    ReadFile(game / "mods/order.txt", &order);
    key += order + "\n";
    std::vector<std::string> archives;
    std::error_code ec;
    for (fs::directory_iterator it(game / "mods", ec), end; !ec && it != end; it.increment(ec)) {
        const std::string n = U8(it->path().filename());
        if (EndsWith(Lower(n), ".p3a") && !IEquals(n, kIconPackFile)) archives.push_back(n + " " + Stat(it->path()));
    }
    std::sort(archives.begin(), archives.end());
    key += Join(archives, "\n") + "\n";
    for (const IconPackTarget& t : config.targets) {
        for (const std::string& p : t.pkgs) key += p + " " + Stat(game / Path(p)) + "\n";
    }
    return key;
}

std::mutex g_fingerprint_mutex;
std::map<std::string, Json> g_fingerprints;   // QuickKey (with the game folder) -> fingerprint

}  // namespace

// ---------------------------------------------------------------- config
bool IconPackConfig::FromJson(const Json& j, IconPackConfig* out, std::string* error) {
    IconPackConfig c;
    if (!j["targets"].is_array()) return Fail(error, "no \"targets\" list");
    c.hash = Md5Hex(j["targets"].Dump(false));
    for (const Json& t : j["targets"].elements()) {
        if (t["disabled"].AsBool(false)) continue;
        IconPackTarget target;
        target.note = t.Str("note");
        for (const Json& p : t["pkg"].elements()) {
            if (!p.AsString().empty()) target.pkgs.push_back(p.AsString());
        }
        target.texture = t.Str("texture");
        target.cell_grid = static_cast<int>(t["cell_grid"].AsInt(0));
        target.draw_size = static_cast<int>(t["draw_size"].AsInt(0));
        for (const Json& r : t["rows"].elements()) target.rows.push_back(static_cast<int>(r.AsInt()));
        for (const Json& r : t["cols"].elements()) target.cols.push_back(static_cast<int>(r.AsInt()));
        if (t["texture_size"].size() == 2) {
            target.texture_w = static_cast<int>(t["texture_size"].elements()[0].AsInt());
            target.texture_h = static_cast<int>(t["texture_size"].elements()[1].AsInt());
        }
        target.original_art = t["original_art"].AsBool(false);
        target.enlarge = t["enlarge"].AsBool(false);
        if (target.pkgs.empty() || target.texture.empty() || target.cell_grid <= 0 || target.draw_size <= 0) {
            return Fail(error, "a target needs pkg, texture, cell_grid and draw_size");
        }
        c.targets.push_back(target);
    }
    *out = std::move(c);
    return true;
}

// ---------------------------------------------------------------- pieces
bool FindTextureLayout(const std::string& phyre, int w, int h, TextureLayout* out, std::string* error) {
    const std::string head = phyre.substr(0, 4096);
    if (head.find("RGBA8") == std::string::npos && head.find("ARGB8") == std::string::npos) {
        return Fail(error, "not an uncompressed RGBA8/ARGB8 texture");
    }
    std::vector<std::pair<int, int>> sizes;
    if (w > 0 && h > 0) {
        sizes.emplace_back(w, h);
    } else {
        static const int kSizes[] = {4096, 2048, 1024, 512, 256, 128, 64};
        for (int sw : kSizes) {
            for (int sh : kSizes) sizes.emplace_back(sw, sh);
        }
    }
    std::vector<TextureLayout> found;
    for (const auto& s : sizes) {
        for (bool mips : {false, true}) {
            const size_t bytes = MipBytes(s.first, s.second, mips);
            if (bytes > phyre.size()) continue;
            const size_t offset = phyre.size() - bytes;
            if (offset >= 1500 && offset <= 4096) found.push_back({s.first, s.second, offset, mips});
        }
    }
    // The header stores height and width as adjacent u32s, ~140 bytes before the pixels; they pick
    // between candidates of the same byte count (1024x1024 vs 4096x256).
    std::vector<TextureLayout> exact;
    for (const TextureLayout& c : found) {
        uint32_t hw[2] = {static_cast<uint32_t>(c.height), static_cast<uint32_t>(c.width)};
        const std::string needle(reinterpret_cast<const char*>(hw), 8);
        if (phyre.substr(c.offset - 400, 400).find(needle) != std::string::npos) exact.push_back(c);
    }
    if (!exact.empty()) found = exact;
    if (found.size() != 1) {
        return Fail(error, "cannot tell the texture size (" + std::to_string(found.size()) + " candidates); set texture_size");
    }
    *out = found[0];
    return true;
}

std::vector<uint8_t> ResizeLanczos(const uint8_t* rgba, int n, int d) {
    const size_t count = static_cast<size_t>(n) * n;
    if (n == d) return std::vector<uint8_t>(rgba, rgba + count * 4);   // Pillow returns a copy
    // RGBA -> RGBa
    std::vector<uint8_t> pre(count * 4);
    for (size_t i = 0; i < count; ++i) {
        const unsigned a = rgba[i * 4 + 3];
        for (int c = 0; c < 3; ++c) pre[i * 4 + c] = MulDiv255(rgba[i * 4 + c], a);
        pre[i * 4 + 3] = static_cast<uint8_t>(a);
    }
    const Coeffs cx = PrecomputeCoeffs(n, d);
    const Coeffs cy = PrecomputeCoeffs(n, d);
    // The horizontal pass covers only the rows the vertical pass reads.
    const int ybox_first = cy.bounds[0];
    const int ybox_last = cy.bounds[d * 2 - 2] + cy.bounds[d * 2 - 1];
    const int temp_h = ybox_last - ybox_first;
    std::vector<uint8_t> temp(static_cast<size_t>(temp_h) * d * 4);
    for (int yy = 0; yy < temp_h; ++yy) {
        const uint8_t* in = &pre[static_cast<size_t>(yy + ybox_first) * n * 4];
        for (int xx = 0; xx < d; ++xx) {
            const int xmin = cx.bounds[xx * 2], xmax = cx.bounds[xx * 2 + 1];
            const int32_t* k = &cx.k[static_cast<size_t>(xx) * cx.ksize];
            int32_t ss[4] = {1 << (kPrecisionBits - 1), 1 << (kPrecisionBits - 1), 1 << (kPrecisionBits - 1),
                             1 << (kPrecisionBits - 1)};
            for (int x = 0; x < xmax; ++x) {
                for (int c = 0; c < 4; ++c) ss[c] += in[(x + xmin) * 4 + c] * k[x];
            }
            for (int c = 0; c < 4; ++c) temp[(static_cast<size_t>(yy) * d + xx) * 4 + c] = Clip8(ss[c]);
        }
    }
    std::vector<uint8_t> out(static_cast<size_t>(d) * d * 4);
    for (int yy = 0; yy < d; ++yy) {
        const int ymin = cy.bounds[yy * 2] - ybox_first, ymax = cy.bounds[yy * 2 + 1];
        const int32_t* k = &cy.k[static_cast<size_t>(yy) * cy.ksize];
        for (int xx = 0; xx < d; ++xx) {
            int32_t ss[4] = {1 << (kPrecisionBits - 1), 1 << (kPrecisionBits - 1), 1 << (kPrecisionBits - 1),
                             1 << (kPrecisionBits - 1)};
            for (int y = 0; y < ymax; ++y) {
                const uint8_t* in = &temp[(static_cast<size_t>(y + ymin) * d + xx) * 4];
                for (int c = 0; c < 4; ++c) ss[c] += in[c] * k[y];
            }
            for (int c = 0; c < 4; ++c) out[(static_cast<size_t>(yy) * d + xx) * 4 + c] = Clip8(ss[c]);
        }
    }
    // RGBa -> RGBA
    for (size_t i = 0; i < static_cast<size_t>(d) * d; ++i) {
        const unsigned a = out[i * 4 + 3];
        if (a == 255 || a == 0) continue;
        for (int c = 0; c < 3; ++c) {
            const unsigned v = (255u * out[i * 4 + c]) / a;
            out[i * 4 + c] = static_cast<uint8_t>(v > 255 ? 255 : v);
        }
    }
    return out;
}

std::vector<uint8_t> StretchNearest(const std::vector<uint8_t>& small, int d, int n) {
    std::vector<int> index(n);
    for (int i = 0; i < n; ++i) {
        const double v = std::nearbyint((i + 0.5) * d / n - 0.5);   // half to even, like np.round
        index[i] = std::min(std::max(static_cast<int>(v), 0), d - 1);
    }
    std::vector<uint8_t> out(static_cast<size_t>(n) * n * 4);
    for (int y = 0; y < n; ++y) {
        for (int x = 0; x < n; ++x) {
            std::memcpy(&out[(static_cast<size_t>(y) * n + x) * 4], &small[(static_cast<size_t>(index[y]) * d + index[x]) * 4], 4);
        }
    }
    return out;
}

bool EnlargeTexture(const std::string& phyre, const TextureLayout& lay, int factor, std::string* out,
                    TextureLayout* out_lay, std::string* error) {
    if (lay.mips) return Fail(error, "a mipmapped texture is not enlarged");
    if (factor < 2 || (factor & (factor - 1)) != 0) return Fail(error, "not a power of two");
    // The header fields that differ between the game's 512px icons.png and an HD pack's 1024px one:
    // the pixel byte count at 80, and log2 of the size, 2, 0, height, width (u32s, unaligned) before
    // the pixels. Anything else stays as it was.
    constexpr size_t kBytesAt = 80;
    auto u32 = [&](size_t at) {
        uint32_t v;
        std::memcpy(&v, &phyre[at], 4);
        return v;
    };
    const uint32_t bytes = static_cast<uint32_t>(lay.width) * lay.height * 4;
    if (lay.offset < 400 || u32(kBytesAt) != bytes) return Fail(error, "unknown texture header (pixel byte count)");
    const uint32_t hw[2] = {static_cast<uint32_t>(lay.height), static_cast<uint32_t>(lay.width)};
    const std::string needle(reinterpret_cast<const char*>(hw), 8);
    const size_t from = lay.offset - 400;
    const size_t at = phyre.find(needle, from);
    if (at == std::string::npos || at >= lay.offset || at < 12 || phyre.find(needle, at + 1) < lay.offset) {
        return Fail(error, "unknown texture header (height/width)");
    }
    int log2 = 0;
    while ((1 << log2) < std::max(lay.width, lay.height)) ++log2;
    if (u32(at - 12) != static_cast<uint32_t>(log2)) return Fail(error, "unknown texture header (log2 size)");
    int shift = 0;
    while ((1 << shift) < factor) ++shift;
    const int w = lay.width * factor, h = lay.height * factor;
    std::string result = phyre.substr(0, lay.offset);
    const uint32_t new_bytes = static_cast<uint32_t>(w) * h * 4, new_log2 = static_cast<uint32_t>(log2 + shift);
    const uint32_t new_hw[2] = {static_cast<uint32_t>(h), static_cast<uint32_t>(w)};
    std::memcpy(&result[kBytesAt], &new_bytes, 4);
    std::memcpy(&result[at - 12], &new_log2, 4);
    std::memcpy(&result[at], new_hw, 8);
    const size_t start = result.size();
    result.resize(start + new_bytes);
    const char* src = phyre.data() + lay.offset;
    char* dst = &result[start];
    for (int y = 0; y < h; ++y) {
        const char* row = src + static_cast<size_t>(y / factor) * lay.width * 4;
        char* to = dst + static_cast<size_t>(y) * w * 4;
        for (int x = 0; x < w; ++x) std::memcpy(to + static_cast<size_t>(x) * 4, row + static_cast<size_t>(x / factor) * 4, 4);
    }
    *out = std::move(result);
    *out_lay = {w, h, start, false};
    return true;
}

// ---------------------------------------------------------------- order.txt
std::vector<std::string> ModLoadOrder(const fs::path& game) {
    std::vector<std::string> out;
    std::string text;
    if (ReadFile(game / "mods/order.txt", &text)) {
        if (StartsWith(text, "\xef\xbb\xbf")) text = text.substr(3);
        for (const std::string& line : Lines(text)) {
            const std::string name = Trim(line);
            if (!name.empty()) out.push_back(name);
        }
    }
    // SenPatcher loads the archives order.txt does not name after the ones it does, and puts its
    // own zzz_senpatcher_* last.
    std::vector<std::string> extra;
    std::error_code ec;
    for (fs::directory_iterator it(game / "mods", ec), end; !ec && it != end; it.increment(ec)) {
        const std::string n = U8(it->path().filename());
        if (!it->is_regular_file() || !EndsWith(Lower(n), ".p3a")) continue;
        bool listed = false;
        for (const std::string& o : out) listed = listed || IEquals(o, n);
        if (!listed) extra.push_back(n);
    }
    if (!extra.empty()) {
        std::sort(extra.begin(), extra.end());
        out.insert(out.end(), extra.begin(), extra.end());
        std::stable_sort(out.begin(), out.end(), [](const std::string& a, const std::string& b) {
            return !StartsWith(Lower(a), "zzz_senpatcher_") && StartsWith(Lower(b), "zzz_senpatcher_");
        });
    }
    return out;
}

namespace {

bool WriteOrder(const fs::path& game, const std::string& first, const std::string& drop, std::string* error) {
    std::string text;
    ReadFile(game / "mods/order.txt", &text);
    if (StartsWith(text, "\xef\xbb\xbf")) text = text.substr(3);
    std::string updated = first.empty() ? std::string() : first + "\n";
    for (const std::string& line : Lines(text)) {
        const std::string name = Trim(line);
        if (name.empty() || IEquals(name, drop)) continue;
        updated += name + "\n";
    }
    std::error_code ec;
    fs::create_directories(game / "mods", ec);
    return WriteFileAtomic(game / "mods/order.txt", updated, error);
}

}  // namespace

bool PutFirstInModOrder(const fs::path& game, const std::string& name, std::string* error) {
    return WriteOrder(game, name, name, error);
}

bool RemoveFromModOrder(const fs::path& game, const std::string& name, std::string* error) {
    if (!Exists(game / "mods/order.txt")) return true;
    return WriteOrder(game, std::string(), name, error);
}

// ---------------------------------------------------------------- fingerprint
Json IconPackFingerprint(const fs::path& game, const IconPackConfig& config) {
    const std::string key = U8(game) + "\n" + QuickKey(game, config);
    {
        std::lock_guard<std::mutex> lock(g_fingerprint_mutex);
        const auto it = g_fingerprints.find(key);
        if (it != g_fingerprints.end()) return it->second;
    }
    const std::vector<Archive> archives = OpenArchives(game);
    Json sources = Json::MakeArray();
    std::vector<std::string> seen;
    for (const IconPackTarget& t : config.targets) {
        for (const std::string& pkg : t.pkgs) {
            if (std::find(seen.begin(), seen.end(), pkg) != seen.end()) continue;
            seen.push_back(pkg);
            std::string from;
            for (const Archive& a : archives) {
                if (a.ok && a.reader.Has(pkg)) {
                    from = a.name + " " + Stat(a.reader.path());
                    break;
                }
            }
            if (from.empty()) from = "game data " + Stat(game / Path(pkg));
            if (t.original_art) from += " | game data " + Stat(game / Path(pkg));
            sources.Push(pkg + " <- " + from);
        }
    }
    Json fp = Json::MakeObject();
    fp["config"] = config.hash;
    fp["sources"] = sources;
    fp["hash"] = Md5Hex(config.hash + sources.Dump(false));
    std::lock_guard<std::mutex> lock(g_fingerprint_mutex);
    if (g_fingerprints.size() > 16) g_fingerprints.clear();
    g_fingerprints[key] = fp;
    return fp;
}

// ---------------------------------------------------------------- build
IconPackResult BuildIconPackFile(const fs::path& game, const IconPackConfig& config, const fs::path& out, Json* fingerprint) {
    IconPackResult r;
    if (fingerprint != nullptr) *fingerprint = IconPackFingerprint(game, config);
    const std::vector<Archive> archives = OpenArchives(game);
    for (const Archive& a : archives) {
        if (!a.ok) {
            r.notes.push_back("mods/" + a.name + " cannot be read (" + a.error + ") - SenPatcher may not load it either");
        }
    }
    struct Changed {
        std::string path;
        Pkg pkg;
    };
    std::vector<Changed> changed;
    for (const IconPackTarget& t : config.targets) {
        for (const std::string& pkg_path : t.pkgs) {
            std::string raw, source, why;
            const Found found = Resolve(game, archives, pkg_path, &raw, &source, &why);
            if (found == Found::No) {
                r.notes.push_back(pkg_path + ": not found, skipped");
                continue;
            }
            if (found == Found::Unreadable) {
                r.notes.push_back(pkg_path + " (" + source + "): cannot be read (" + why + "), skipped");
                continue;
            }
            Changed* slot = nullptr;
            for (Changed& c : changed) {
                if (c.path == pkg_path) slot = &c;
            }
            Pkg pkg;
            if (slot != nullptr) {
                pkg = slot->pkg;
            } else if (!pkg.Parse(raw, &why)) {
                r.notes.push_back(pkg_path + " (" + source + "): " + why + ", skipped");
                continue;
            }
            raw.clear();
            const std::string head = pkg_path + " (" + source + ") " + t.texture + ": ";
            std::string original;
            bool have_original = false;
            if (t.original_art && source != "game data") {
                std::string disk;
                Pkg own;
                if (!ReadFile(game / Path(pkg_path), &disk) || !own.Parse(disk, &why) || !own.Read(t.texture, &original, &why)) {
                    r.notes.push_back(head + "skipped: the game's own package cannot be read (" + why + ")");
                    continue;
                }
                have_original = true;
            }
            std::string phyre, result, note;
            if (!pkg.Read(t.texture, &phyre, &why)) {
                r.notes.push_back(head + "skipped: " + why);
                continue;
            }
            if (!ApplyTarget(t, phyre, have_original ? &original : nullptr, &result, &note)) {
                r.notes.push_back(head + note);
                continue;
            }
            r.notes.push_back(head + note);
            pkg.Replace(t.texture, result);
            if (slot != nullptr) {
                slot->pkg = std::move(pkg);
            } else {
                changed.push_back({pkg_path, std::move(pkg)});
            }
        }
    }
    r.packages = static_cast<int>(changed.size());
    if (changed.empty()) {
        r.notes.push_back("nothing to pack");
        r.ok = true;
        return r;
    }
    std::vector<std::pair<std::string, std::string>> files;
    for (const Changed& c : changed) files.emplace_back(c.path, c.pkg.Build());
    changed.clear();
    // zstd at a modest level: a few seconds less on the Deck than Python's 19, a little larger.
    if (!WriteP3a(out, files, 6, &r.error)) return r;
    std::error_code ec;
    char size[32];
    std::snprintf(size, sizeof(size), "%.1f MB", static_cast<double>(fs::file_size(out, ec)) / 1e6);
    r.notes.push_back("wrote " + U8(out) + " (" + size + ", " + std::to_string(files.size()) + " package(s))");
    r.ok = true;
    return r;
}

IconPackResult InstallIconPack(const fs::path& game, const IconPackConfig& config) {
    IconPackResult r;
    if (GameRunning()) {
        r.error = "close the game first: the icon pack is not built while it runs";
        return r;
    }
    std::error_code ec;
    fs::create_directories(game / "mods", ec);
    const fs::path pack = game / "mods" / kIconPackFile;
    Json fp;
    r = BuildIconPackFile(game, config, pack, &fp);
    for (const std::string& n : r.notes) Log("  " + n);
    if (!r.ok) return r;
    std::string err;
    if (r.packages == 0) {
        // nothing to pre-shrink with these textures: an older pack would only hide them
        if (Exists(pack) && !RemoveFile(pack, &err)) {
            r.ok = false;
            r.error = err;
            return r;
        }
        RemoveFromModOrder(game, kIconPackFile);
    } else if (!PutFirstInModOrder(game, kIconPackFile, &err)) {
        r.ok = false;
        r.error = err;
        return r;
    }
    Json record = Json::MakeObject();
    record["built_at"] = IsoTimeUtc();
    record["manager_version"] = AppVersion();
    record["packages"] = r.packages;
    record["fingerprint"] = fp["hash"];
    record["config"] = fp["config"];
    record["sources"] = fp["sources"];
    if (!WriteFileAtomic(game / "mods" / kIconPackRecord, record.Dump(), &err)) {
        r.ok = false;
        r.error = err;
        return r;
    }
    Log(r.packages == 0 ? std::string("icon pack: nothing to pre-shrink with the textures in use")
                        : "icon pack: " + ModsRel(kIconPackFile) + ", first in mods/order.txt");
    return r;
}

bool RemoveIconPack(const fs::path& game, std::string* error) {
    if (GameRunning()) return Fail(error, "close the game first: its files are in use");
    bool ok = true;
    for (const char* f : {kIconPackFile, kIconPackRecord}) {
        const fs::path p = game / "mods" / f;
        if (!Exists(p)) continue;
        if (RemoveFile(p, error)) {
            Log("removed " + ModsRel(f));
        } else {
            ok = false;
        }
    }
    return RemoveFromModOrder(game, kIconPackFile, error) && ok;
}

IconPackStatus GetIconPackStatus(const fs::path& game, const IconPackConfig& config, bool senpatcher) {
    IconPackStatus s;
    if (config.empty()) return s;
    if (!senpatcher) {
        s.state = IconPackState::NeedsSenPatcher;
        s.text = "not available (needs SenPatcher)";
        return s;
    }
    std::string text;
    Json record;
    const bool has_pack = Exists(game / "mods" / kIconPackFile);
    if (!ReadFile(game / "mods" / kIconPackRecord, &text) || !Json::Parse(text, &record)) {
        s.state = has_pack ? IconPackState::Stale : IconPackState::NotBuilt;
        s.text = has_pack ? "out of date (built by another tool)" : "not built";
        return s;
    }
    const Json fp = IconPackFingerprint(game, config);
    s.state = IconPackState::Stale;
    if (record.Str("config") != config.hash) {
        s.text = "out of date (this version pre-shrinks other icons)";
    } else if (record.Str("fingerprint") != fp.Str("hash")) {
        s.text = "out of date (texture mods changed)";
    } else if (record["packages"].AsInt(0) == 0) {
        s.state = IconPackState::NotNeeded;
        s.text = "not needed (the textures in use are small enough)";
    } else if (!has_pack) {
        s.text = "out of date (the pack file is missing)";
    } else {
        const std::vector<std::string> order = ModLoadOrder(game);
        if (order.empty() || !IEquals(order[0], kIconPackFile)) {
            s.text = "out of date (not first in mods/order.txt)";
        } else {
            s.state = IconPackState::Built;
            s.text = "built";
        }
    }
    return s;
}

}  // namespace atmt
