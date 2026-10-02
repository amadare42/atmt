// icon_pack.h - builds a SenPatcher .p3a that pre-shrinks cells of UI atlases to the size the game
// draws them (docs/MANAGER.md, "The icon pack").
//
// Generic: the payload's icon_pack.json says which cells of which uncompressed RGBA PhyreEngine
// atlases inside which .pkg packages to pre-shrink; nothing here knows a texture. The pack is made
// from the player's own files (often a third-party HD texture pack), so it cannot ship - the
// manager builds it on the player's machine:
//
//   * each package comes from where SenPatcher would load it: the first archive in mods/order.txt
//     (then the unlisted mods/*.p3a, SenPatcher's own zzz_senpatcher_* last) that has it, our own
//     pack left out, else the game's data folder;
//   * each configured cell is resampled to its draw size (Lanczos, as Pillow does it: premultiplied
//     alpha, separable, support 3, 22-bit fixed-point coefficients) and stretched back to the cell
//     with nearest-neighbour, so the game's bilinear sampling at the draw size lands exactly on the
//     clean downscale; "original_art" takes the cells' pictures from the game's own (unmodded)
//     package instead; a texture whose cells are drawn at less than half their size is left alone
//     (ratio < 2: no stored cell can reproduce the downscale), unless the target says "enlarge":
//     then the atlas is doubled first (nearest-neighbour, header patched) until the ratio is 2;
//   * the changed packages go into mods/atmt_icon_pack.p3a, first in mods/order.txt; what it was
//     built from goes into mods/atmt_icon_pack.json, so a texture mod changed later shows the
//     pack as out of date.
//
// Only SenPatcher reads mods/: without it the pack does nothing and is not built.
#pragma once

#include <string>
#include <utility>
#include <vector>

#include "json.h"
#include "util.h"

namespace atmt {

constexpr const char* kIconPackFile = "atmt_icon_pack.p3a";      // in <game>/mods
constexpr const char* kIconPackRecord = "atmt_icon_pack.json";   // in <game>/mods: what it was built from

struct IconPackTarget {
    std::string note;
    std::vector<std::string> pkgs;   // package paths as the game names them (each one that exists)
    std::string texture;             // the file inside the package
    int cell_grid = 0;               // cells per row: a cell is width / cell_grid texels
    int draw_size = 0;               // the size the game draws a cell at
    std::vector<int> rows, cols;     // which cells (from the first stored row); empty: all
    int texture_w = 0, texture_h = 0;   // 0: found from the file
    bool original_art = false;
    bool enlarge = false;            // double a too-small atlas until its cells are 2x draw_size
};

struct IconPackConfig {
    std::vector<IconPackTarget> targets;   // the enabled ones
    std::string hash;                      // of the whole "targets" list: a changed list rebuilds
    bool empty() const { return targets.empty(); }
    static bool FromJson(const Json& j, IconPackConfig* out, std::string* error = nullptr);
};

// ---- the pieces (tested on their own)
struct TextureLayout {
    int width = 0, height = 0;
    size_t offset = 0;   // of level 0
    bool mips = false;
};
// (width, height, offset of level 0, mips) of an uncompressed RGBA8/ARGB8 .phyre texture; w/h 0:
// try the usual sizes and use the header's adjacent height/width u32s to pick.
bool FindTextureLayout(const std::string& phyre, int w, int h, TextureLayout* out, std::string* error = nullptr);
// Pillow's Image.resize(LANCZOS) of an n x n RGBA picture to d x d (premultiplied in between).
std::vector<uint8_t> ResizeLanczos(const uint8_t* rgba, int n, int d);
// The d x d picture stretched to n x n with nearest-neighbour (round half to even, as numpy).
std::vector<uint8_t> StretchNearest(const std::vector<uint8_t>& small, int d, int n);
// The texture `factor` (a power of two) times larger, each texel repeated; the header's pixel byte
// count, height/width and log2 size are patched. Mipmapped textures and unknown headers fail.
bool EnlargeTexture(const std::string& phyre, const TextureLayout& lay, int factor, std::string* out,
                    TextureLayout* out_lay, std::string* error = nullptr);

// ---- sources and order.txt
// mods/order.txt's names (trimmed, no empty lines), then the other *.p3a in mods/ as SenPatcher adds them.
std::vector<std::string> ModLoadOrder(const fs::path& game);
bool PutFirstInModOrder(const fs::path& game, const std::string& name, std::string* error = nullptr);
bool RemoveFromModOrder(const fs::path& game, const std::string& name, std::string* error = nullptr);

// What the pack would be built from now (sources, config); its "hash" is what staleness compares.
Json IconPackFingerprint(const fs::path& game, const IconPackConfig& config);

// ---- building
struct IconPackResult {
    bool ok = false;
    int packages = 0;                 // 0: nothing to pack (no file written)
    std::string error;
    std::vector<std::string> notes;   // what was done / skipped, one line each
};
// Builds the pack into `out`; the game folder is only read.
IconPackResult BuildIconPackFile(const fs::path& game, const IconPackConfig& config, const fs::path& out,
                                 Json* fingerprint = nullptr);
// Builds mods/atmt_icon_pack.p3a, puts it first in mods/order.txt (both created when missing) and
// writes the record. With nothing to pack an older pack is removed (not an error). Refused while
// the game runs. The caller checks that SenPatcher is set up (SenPatcherSetUp).
IconPackResult InstallIconPack(const fs::path& game, const IconPackConfig& config);
// The pack, its record and its order.txt line.
bool RemoveIconPack(const fs::path& game, std::string* error = nullptr);

// Removed: the player removed it, so an install leaves it out until it is built again by hand.
enum class IconPackState { NoConfig, NeedsSenPatcher, NotBuilt, Built, NotNeeded, Stale, Removed };
struct IconPackStatus {
    IconPackState state = IconPackState::NoConfig;
    std::string text;   // "built", "out of date (texture mods changed)", ...
    bool CanBuild() const { return state != IconPackState::NoConfig && state != IconPackState::NeedsSenPatcher; }
};
IconPackStatus GetIconPackStatus(const fs::path& game, const IconPackConfig& config, bool senpatcher);

}  // namespace atmt
