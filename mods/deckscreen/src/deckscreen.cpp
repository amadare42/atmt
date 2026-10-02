// deckscreen.cpp - native-resolution rendering (1280x800 on the Steam Deck) with the UI laid out
// 1:1 exactly as on a 1280x720 screen, centered.
//
// Mechanism (docs/MODS.md, "Deckscreen"):
//   * The UI is authored for a 1280x720 canvas. Everything it draws - textured quads (FUN_005571b0 /
//     FUN_005581f0, both submitted by FUN_005558c0), UI meshes such as the minimap (FUN_00557860) and
//     text (FUN_005a1750) - is placed into an NDC space whose y spans the 720-tall canvas and whose x
//     carries a 16:9 factor that the shader divides back out with the camera's aspect.
//   * At a 16:10 size the engine keeps that mapping and lets the viewport stretch it: the UI comes out
//     uniformly 800/720 too big, centered, so it overflows (crops) left and right.
//   * The fix is one uniform scale in NDC, applied where each of those three paths builds its
//     transform: x and y are multiplied by fit = min(W/1280, H/720) / (H/720) (0.9 at 1280x800),
//     always on a private copy. Every UI element then lands at design size 1:1 and the 1280x720
//     canvas sits centered in the frame, with shapes and relative positions untouched - unlike the
//     per-constant patches of Rounds 14-18, which changed x and y (or position and size) by
//     different amounts. Verified pixel-exact against a true 720p frame (HUD, minimap, Camp Menu).
//   * The 3D scene is not touched at all: it keeps rendering at the native size with the native
//     aspect, so nothing is resampled and there are no black bars.
//   * Only draws into a full-screen target are rescaled: the same UI code also draws into offscreen
//     targets (the 266x266 minimap texture, 512x64 and 1024x1024 text caches), whose NDC must stay.
//   * Scene sprites (FUN_00592ec0 - the "Talk" bubble over an NPC) go through the same UI mesh path
//     but carry a world matrix projected by the 3D camera, so they are left alone as well.
#include "code_check.h"
#include "mod_api.h"
#include "text_prefilter.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace {

const AtmtModApi* g_api = nullptr;
volatile LONG g_started = 0;

void Log(const char* fmt, ...) {
    if (g_api == nullptr || g_api->log == nullptr) return;
    char buf[512];
    va_list args;
    va_start(args, fmt);
    _vsnprintf(buf, sizeof(buf) - 1, fmt, args);
    va_end(args);
    buf[sizeof(buf) - 1] = '\0';
    g_api->log(buf);
}

// A failure: written to atmt_loader.log unless LogLevel=off (Log is written with LogLevel=all only).
void LogError(const char* fmt, ...) {
    if (g_api == nullptr || g_api->log_error == nullptr) return;
    char buf[512];
    va_list args;
    va_start(args, fmt);
    _vsnprintf(buf, sizeof(buf) - 1, fmt, args);
    va_end(args);
    buf[sizeof(buf) - 1] = '\0';
    g_api->log_error(buf);
}

// ---------------------------------------------------------------- addresses (ed8.exe, no ASLR)
// The render context every draw is enqueued through (FUN_007daa00's `this`): +0x10/+0x14 is the size
// of the target currently being drawn into.
constexpr uintptr_t kRenderContext = 0x00c7d734;
constexpr uintptr_t kTargetWidth = kRenderContext + 0x10;
constexpr uintptr_t kTargetHeight = kRenderContext + 0x14;

constexpr float kDesignWidth = 1280.0f;
constexpr float kDesignHeight = 720.0f;

// The whole mod (Enabled): read once in AtmtModInit - off means nothing is hooked at all.
int32_t g_mod_enabled = 1;
// The letterbox (Letterbox), read live by CurrentUiFit: the hooks stay installed and simply pass the
// draw through untouched while it is 0, so the settings bar can switch the letterbox on and off with
// the game running.
int32_t g_enabled = 1;
volatile LONG g_rescaled = 0;

// ---------------------------------------------------------------- the window's size
// Refreshed by the poll thread from the game window's client rect - only logged: the fit follows the
// frame being drawn into (CurrentUiFit), which does not change with the window.
volatile LONG g_client_w = 0;
volatile LONG g_client_h = 0;

float FitFor(LONG w, LONG h) {
    const float by_height = static_cast<float>(h) / kDesignHeight;
    return std::min(static_cast<float>(w) / kDesignWidth, by_height) / by_height;
}

BOOL CALLBACK FindGameWindow(HWND hwnd, LPARAM out) {
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    char cls[64] = {};
    if (pid == GetCurrentProcessId() && GetClassNameA(hwnd, cls, sizeof(cls)) > 0 &&
        std::strcmp(cls, "PhyreFrameworkClass") == 0) {
        *reinterpret_cast<HWND*>(out) = hwnd;
        return FALSE;
    }
    return TRUE;
}

void RefreshClientSize() {
    static HWND window = nullptr;
    if (window == nullptr || !IsWindow(window)) {
        window = nullptr;
        EnumWindows(&FindGameWindow, reinterpret_cast<LPARAM>(&window));
        if (window == nullptr) return;
    }
    RECT rect;
    if (!GetClientRect(window, &rect) || rect.right <= 0 || rect.bottom <= 0) return;
    const LONG old_w = InterlockedExchange(&g_client_w, rect.right);
    const LONG old_h = InterlockedExchange(&g_client_h, rect.bottom);
    if (old_w != rect.right || old_h != rect.bottom) {
        Log("deckscreen: window client %ldx%ld", rect.right, rect.bottom);
    }
}


// ---------------------------------------------------------------- draw trace (diagnostics)
// With TraceDraws on, every distinct UI draw - which hook, which target size, whether it was
// rescaled, and the chain of game return addresses above it - is logged the first time it is seen
// and again whenever it comes back after a pause, so a play session maps each game screen or effect
// to the draws it makes. Game thread only (all three hooks are called while the frame is built).
int32_t g_trace = 0;
// Crisp small text (text_prefilter.cpp), live settings, read by it on both threads.
int32_t g_crisp_text = 1;
float g_text_weight = 1.0f;

uintptr_t g_text_begin = 0;
uintptr_t g_text_end = 0;

void FindGameText() {
    const auto base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    const IMAGE_SECTION_HEADER* section = IMAGE_FIRST_SECTION(nt);
    for (unsigned i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++section) {
        if (std::memcmp(section->Name, ".text", 5) == 0) {
            g_text_begin = base + section->VirtualAddress;
            g_text_end = g_text_begin + section->Misc.VirtualSize;
            return;
        }
    }
}

// A stack value that points just past a call instruction in the game's code.
bool LooksLikeReturn(uintptr_t value) {
    if (value < g_text_begin + 8 || value >= g_text_end) return false;
    const auto* p = reinterpret_cast<const uint8_t*>(value);
    if (p[-5] == 0xE8) return true;                                         // call rel32
    if (p[-6] == 0xFF && (p[-5] & 0x38) == 0x10) return true;               // call [disp32] etc.
    if (p[-3] == 0xFF && (p[-2] & 0x38) == 0x10) return true;               // call [reg+disp8]
    if (p[-2] == 0xFF && (p[-1] & 0x38) == 0x10 && (p[-1] & 0xC0) == 0xC0) return true;  // call reg
    return false;
}

constexpr int kChain = 5;
struct TraceEntry {
    uint32_t hook;
    LONG w, h;
    uint32_t scaled;
    uintptr_t chain[kChain];
    DWORD last_seen;
    uint32_t count;
};
TraceEntry g_trace_table[1024];
// Set by the poll thread when <game>\deckscreen.dump appears: every draw is logged (not only
// new ones) until this many have been written - roughly one frame of the battle HUD.
volatile LONG g_dump_left = 0;
__thread const uint8_t* g_trace_sprite = nullptr;  // the FUN_00556a70 sprite being drawn, if any
__thread int g_trace_edge = 0;                      // the quad being traced is a letterbox bar (EdgeBar)
int g_trace_used = 0;
DWORD g_trace_start = 0;

// `frame` is the detour's first stack argument; the word below it is the hook's return address.
void TraceDraw(uint32_t hook, const void* frame, float fit, const float* m, const uint32_t* extra,
               int extra_count) {
    if (!g_trace || g_text_begin == 0) return;
    TraceEntry key = {};
    key.hook = hook;
    key.w = *reinterpret_cast<const LONG*>(kTargetWidth);
    key.h = *reinterpret_cast<const LONG*>(kTargetHeight);
    key.scaled = fit < 0.9999f ? 1 : 0;
    if (g_trace_sprite != nullptr) {
        const float x = *reinterpret_cast<const float*>(g_trace_sprite + 0x48);
        const float y = *reinterpret_cast<const float*>(g_trace_sprite + 0x4c);
        if (x < 0 || x > kDesignWidth || y < 0 || y > kDesignHeight) key.scaled |= 2;  // off-canvas
    }
    if (g_trace_edge != 0) key.scaled |= g_trace_edge > 0 ? 4 : 8;
    const uintptr_t* stack = reinterpret_cast<const uintptr_t*>(frame) - 1;
    int found = 0;
    for (int i = 0; i < 160 && found < kChain; ++i) {
        if (LooksLikeReturn(stack[i])) key.chain[found++] = stack[i];
    }
    const DWORD now = GetTickCount();
    if (g_trace_start == 0) g_trace_start = now;
    TraceEntry* entry = nullptr;
    for (int i = 0; i < g_trace_used; ++i) {
        TraceEntry& e = g_trace_table[i];
        if (e.hook == key.hook && e.w == key.w && e.h == key.h && e.scaled == key.scaled &&
            std::memcmp(e.chain, key.chain, sizeof(key.chain)) == 0) {
            entry = &e;
            break;
        }
    }
    const char* why = nullptr;
    if (g_dump_left > 0 && InterlockedDecrement(&g_dump_left) >= 0) why = "dump";
    if (entry == nullptr) {
        if (g_trace_used >= static_cast<int>(sizeof(g_trace_table) / sizeof(g_trace_table[0]))) return;
        entry = &g_trace_table[g_trace_used++];
        *entry = key;
        why = "new";
    } else if (why == nullptr && now - entry->last_seen > 3000) {
        why = "back";
    }
    entry->last_seen = now;
    ++entry->count;
    if (why == nullptr) return;
    static const char* const kHooks[] = {"quad", "mesh", "text", "wsprite", "CULLED"};
    char args[480] = "";
    int n = 0;
    for (int i = 0; i < extra_count && n < static_cast<int>(sizeof(args)) - 16; ++i) {
        n += std::snprintf(args + n, sizeof(args) - n, " %08x", extra[i]);
    }
    char mat[160] = "-";
    if (m != nullptr) {
        std::snprintf(mat, sizeof(mat), "[%.3f %.3f | %.3f %.3f | t %.3f %.3f]", m[0], m[1], m[4],
                      m[5], m[12], m[13]);
    }
    char sprite[160] = "";
    if (g_trace_sprite != nullptr) {
        const float* f = reinterpret_cast<const float*>(g_trace_sprite + 0x40);
        std::snprintf(sprite, sizeof(sprite), " sprite+40: %.1f %.1f %.1f %.1f | %.1f %.1f %.3f %.3f | %.3f %.3f %.3f %.3f",
                      f[0], f[1], f[2], f[3], f[4], f[5], f[6], f[7], f[8], f[9], f[10], f[11]);
    }
    const char* edge = (key.scaled & 4) ? "+TOPBAR" : (key.scaled & 8) ? "+BOTTOMBAR" : "";
    Log("deckscreen: trace %s #%d t=%.1fs %s %ldx%ld %s%s chain %06x %06x %06x %06x %06x m%s args%s%s",
        why, static_cast<int>(entry - g_trace_table), (now - g_trace_start) / 1000.0,
        kHooks[hook], key.w, key.h, (key.scaled & 2) ? ((key.scaled & 1) ? "SCALED-OFFCANVAS" : "kept-OFFCANVAS") : ((key.scaled & 1) ? "SCALED" : "kept"), edge, key.chain[0], key.chain[1],
        key.chain[2], key.chain[3], key.chain[4], mat, args, sprite);
}

// ---------------------------------------------------------------- the hooks
// Scales x/y output columns of a row-major NDC transform (row vectors: column 0 is x, column 1 is y,
// translation included in row 3). A missing matrix means identity.
void ScaleNdc(const float* in, float fit, float out[16], bool x_only = false) {
    static const float kIdentity[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    std::memcpy(out, in != nullptr ? in : kIdentity, 16 * sizeof(float));
    for (int row = 0; row < 4; ++row) {
        out[row * 4 + 0] *= fit;
        if (!x_only) out[row * 4 + 1] *= fit;
    }
}

// Nonzero while the game draws scene sprites (FUN_00592ec0, see WorldSpriteDetour): their transform
// is a world matrix that the shader puts through the 3D camera, not a UI NDC transform.
__thread int g_world_depth = 0;
// Nonzero while the game draws a full-screen copy of the frame (FUN_005e7e30, see ScreenFlashDetour):
// it must fill the screen exactly, so only x is fitted.
__thread int g_screen_copy_depth = 0;
// Nonzero while a screen whose UI is anchored to the screen edges draws (the Battle Result,
// BattleResultDetour); AnchoredScreens is its live setting.
__thread int g_anchor_depth = 0;
int32_t g_anchor_screens = 1;
// Nonzero while the area map draws (FUN_006293d0, see MapDetour): its meshes are placed in the world.
__thread int g_map_depth = 0;

// Moves an already fitted transform by the 40 px band: up for an element in the canvas' top half, down
// for one in its bottom half, so that a screen framed by a header and a footer gets them at the screen
// edges as a native 16:10 layout would, instead of 40 px in. y' = y + shift * w (column 1 += shift *
// column 3).
void AnchorToEdge(float out[16], bool top, float fit) {
    const float shift = top ? 1.0f - fit : fit - 1.0f;
    for (int row = 0; row < 4; ++row) out[row * 4 + 1] += shift * out[row * 4 + 3];
}

// Whether a target of this size is the frame itself rather than one of the UI's offscreen targets
// (the 266x266 minimap texture, the 512x64 and 1024x1024 text caches), which keep their own NDC. It
// is told by its shape, not by comparing it to the window: the frame keeps the size the game was
// started at when the window is resized (a window 1 px taller than the frame used to switch the
// whole letterbox off and bring back the crop).
bool IsFrameTarget(LONG w, LONG h) {
    if (w < 640 || h < 360) return false;
    const float aspect = static_cast<float>(w) / static_cast<float>(h);
    return aspect >= 1.2f && aspect <= 2.5f;
}

// The scale to apply to a UI draw going into the current target right now, or 1.
float CurrentUiFit() {
    const LONG w = *reinterpret_cast<const LONG*>(kTargetWidth);
    const LONG h = *reinterpret_cast<const LONG*>(kTargetHeight);
    if (!g_enabled || g_world_depth > 0 || !IsFrameTarget(w, h)) return 1.0f;
    return FitFor(w, h);
}

// FUN_005558c0 (cdecl, 10 args): every textured UI quad - FUN_005571b0's design rects and
// FUN_005581f0's transformed ones. Its 9th argument is the quad's NDC transform.
constexpr uintptr_t kSubmitUiQuad = 0x005558c0;

using SubmitFn = uint32_t(__cdecl*)(uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t,
                                    uint32_t, uint32_t, const float*, uint32_t);
SubmitFn g_real_submit_quad = nullptr;

// The quad FUN_005571b0 submits (a1 == [0x00c7c0e0]) spans -0.5..0.5, so its transform alone says
// where it lands: 3.556 x 2 at the origin is exactly the 16:9 canvas.
const uint32_t* const kDesignQuad = reinterpret_cast<const uint32_t*>(0x00c7c0e0);

// Nonzero while FUN_00556a70 draws a sprite whose rect covers the whole canvas.
__thread int g_cover_sprite = 0;

// A draw that covers the whole 1280x720 canvas - a menu's dimmed backdrop, a fade - is meant to fill
// the screen. Scaled by 0.9 like the rest of the UI it would stop at the canvas, and in screens that
// no longer draw the 3D (the Camp Menu) nothing would repaint the 40 px bands, which then keep
// whatever the menu's slide-in animation left there. Such draws are fitted in x only, so they cover
// the screen exactly as in the unmodified game.
bool CoversCanvas(uint32_t quad, const float* m) {
    if (g_cover_sprite > 0 || g_screen_copy_depth > 0) return true;
    if (m == nullptr || quad != *kDesignQuad || m[1] != 0.0f || m[4] != 0.0f) return false;
    const float half_w = std::fabs(m[0]) * 0.5f;
    const float half_h = std::fabs(m[5]) * 0.5f;
    constexpr float kHalfCanvasW = kDesignWidth / kDesignHeight - 0.001f;  // 16:9 NDC
    return m[12] - half_w <= -kHalfCanvasW && m[12] + half_w >= kHalfCanvasW &&
           m[13] - half_h <= -0.999f && m[13] + half_h >= 0.999f;
}

// ---------------------------------------------------------------- the game's own letterbox bars
// Events put black bars over the top and bottom of the canvas. Scaled by 0.9 like the rest of the UI
// they would end 40 px short of the screen edges with the 3D showing past them, so a bar - a solid
// (untextured, a texture of a few texels, or one texel of any texture) axis-aligned draw spanning the
// canvas width and touching its top or bottom edge, but not both - reaches the screen edge one of two
// ways:
//   * an event bar (a design quad, FUN_005e0xxx) is moved out by the band, keeping its height
//     (AnchorToEdge): the picture between the bars is then taller than at 720p (736 rows for the
//     32 px bars) and shows at least as much of the scene vertically, though the 16:10 3D is 800/720
//     bigger.
//   * a bar that is part of a sprite layout (the main menu's, 1280x29 and 1280x36, with the copyright
//     and version text on the bottom one) keeps its inner edge, so what is laid out on it stays on
//     it, and is extended out to the screen edge (StretchToEdge): the bands are black, as in a
//     proper letterbox.
// Returns +1 for a top bar, -1 for a bottom bar, 0 otherwise; `inner` gets the bar's inner edge in
// canvas NDC (y up, +-1 at the canvas edges) and `layout` whether it is a sprite.
bool SpriteRect(const uint8_t* sprite, float* left, float* top, float* right, float* bottom);

// a3/a4 of FUN_005558c0 are the quad's UV size: the event bars (FUN_005e0xxx) sample one texel of a
// 32x32 texture, u, v size 0. The main menu's bars show the whole of a 32x32 plain texture (the one the
// title's full-screen fade uses too), so a texture of up to 32x32 counts as solid.
bool SolidTexture(uint32_t texture, uint32_t uv_width, uint32_t uv_height) {
    if (texture == 0) return true;  // the default texture
    if ((uv_width & 0x7fffffff) == 0 && (uv_height & 0x7fffffff) == 0) return true;  // +-0.0f
    MEMORY_BASIC_INFORMATION mbi;
    if (!VirtualQuery(reinterpret_cast<const void*>(texture), &mbi, sizeof(mbi)) ||
        mbi.State != MEM_COMMIT || (mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS)) != 0 ||
        texture + 0x24 > reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize) {
        return false;
    }
    // The Phyre texture's width and height are at +0x1c/+0x20 (as the text path reads them).
    const auto* header = reinterpret_cast<const uint32_t*>(texture);
    return header[7] > 0 && header[7] <= 32 && header[8] > 0 && header[8] <= 32;
}

int EdgeBar(uint32_t quad, uint32_t texture, uint32_t uv_width, uint32_t uv_height, const float* m,
            float* inner, bool* layout) {
    if (g_cover_sprite > 0 || g_screen_copy_depth > 0 || !SolidTexture(texture, uv_width, uv_height)) {
        return 0;
    }
    float top, bottom;  // NDC, y up
    *layout = g_trace_sprite != nullptr;
    if (const uint8_t* sprite = g_trace_sprite) {  // set for every FUN_00556a70 sprite, traced or not
        float left, right, px_top, px_bottom;
        if (!SpriteRect(sprite, &left, &px_top, &right, &px_bottom) || left > 0.5f ||
            right < kDesignWidth - 0.5f) {
            return 0;
        }
        top = 1.0f - 2.0f * px_top / kDesignHeight;
        bottom = 1.0f - 2.0f * px_bottom / kDesignHeight;
    } else {
        if (m == nullptr || quad != *kDesignQuad || m[1] != 0.0f || m[4] != 0.0f) return 0;
        const float half_w = std::fabs(m[0]) * 0.5f;
        const float half_h = std::fabs(m[5]) * 0.5f;
        constexpr float kHalfCanvasW = kDesignWidth / kDesignHeight - 0.001f;
        if (m[12] - half_w > -kHalfCanvasW || m[12] + half_w < kHalfCanvasW) return 0;
        top = m[13] + half_h;
        bottom = m[13] - half_h;
    }
    const bool at_top = top >= 0.999f && bottom < 0.999f;
    const bool at_bottom = bottom <= -0.999f && top > -0.999f;
    if (at_top == at_bottom) return 0;  // neither, or the whole height (CoversCanvas' case)
    *inner = at_top ? bottom : top;
    return at_top ? 1 : -1;
}

// Rewrites y of an already fitted transform (`out`, x and y scaled by `fit`) so the bar's inner edge
// stays at inner * fit and its outer edge lands on the screen edge (NDC +-1): y' = a * y + b applied
// to the transform's output y, i.e. column 1 = a * column 1 + b * column 3.
void StretchToEdge(float out[16], int side, float inner, float fit) {
    const float from_outer = static_cast<float>(side) * fit;  // the fitted canvas edge
    const float from_inner = inner * fit;
    if (std::fabs(from_outer - from_inner) < 1e-6f) return;
    const float to_outer = static_cast<float>(side);
    const float a = (to_outer - from_inner) / (from_outer - from_inner);
    const float b = from_inner - a * from_inner;
    for (int row = 0; row < 4; ++row) {
        out[row * 4 + 1] = a * out[row * 4 + 1] + b * out[row * 4 + 3];
    }
}

// Which half of the canvas a quad belongs to: its sprite's rect when FUN_00556a70 draws it, else the
// transform's translation (the centre of a design quad).
bool InTopHalf(const float* m) {
    float left, top, right, bottom;
    if (g_trace_sprite != nullptr && SpriteRect(g_trace_sprite, &left, &top, &right, &bottom)) {
        return top + bottom < kDesignHeight;
    }
    return m == nullptr || m[13] >= 0.0f;
}

uint32_t __cdecl SubmitUiQuadDetour(uint32_t a1, uint32_t a2, uint32_t a3, uint32_t a4, uint32_t a5,
                                    uint32_t a6, uint32_t a7, uint32_t a8, const float* transform,
                                    uint32_t a10) {
    const float fit = CurrentUiFit();
    if (g_trace) {
        // The sampler index ([0x00c7c104]: 0 linear, 1 point, 2 mipmapped, 3 linear + repeat), then the
        // texture (a2; 0 means the default at [0x00c7c130]) and its first 24 dwords: +0x1c/+0x20 are
        // its width/height and +0x24 its mip count.
        uint32_t extra[6 + 1 + 24] = {a3, a4, a5, a6, a10, static_cast<uint32_t>(*reinterpret_cast<const LONG*>(0x00c7c104))};
        const uint32_t texture = a2 != 0 ? a2 : *reinterpret_cast<const uint32_t*>(0x00c7c130);
        extra[6] = texture;
        MEMORY_BASIC_INFORMATION mbi;
        int count = 7;
        if (texture != 0 && VirtualQuery(reinterpret_cast<const void*>(texture), &mbi, sizeof(mbi)) &&
            mbi.State == MEM_COMMIT && (mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS)) == 0 &&
            texture + 24 * 4 <= reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize) {
            std::memcpy(&extra[7], reinterpret_cast<const void*>(texture), 24 * 4);
            count = 31;
        }
        float inner;
        bool layout;
        g_trace_edge = fit < 0.9999f ? EdgeBar(a1, a2, a3, a4, transform, &inner, &layout) : 0;
        TraceDraw(0, &a1, fit, transform, extra, count);
        g_trace_edge = 0;
    }
    if (fit >= 0.9999f) return g_real_submit_quad(a1, a2, a3, a4, a5, a6, a7, a8, transform, a10);
    float scaled[16];
    const bool covers = CoversCanvas(a1, transform);
    ScaleNdc(transform, fit, scaled, covers);
    float inner = 0.0f;
    bool layout = false;
    const int side = EdgeBar(a1, a2, a3, a4, transform, &inner, &layout);
    if (side != 0 && layout) {
        StretchToEdge(scaled, side, inner, fit);
    } else if (side != 0) {
        AnchorToEdge(scaled, side > 0, fit);
    } else if (g_anchor_depth > 0 && !covers) {
        AnchorToEdge(scaled, InTopHalf(transform), fit);
    }
    InterlockedIncrement(&g_rescaled);
    return g_real_submit_quad(a1, a2, a3, a4, a5, a6, a7, a8, scaled, a10);
}

// FUN_00557860 (cdecl, 10 args): a UI mesh (the minimap ring, disc and marker). Its 9th argument is
// the mesh's NDC transform (size/rotation), and its screen position is not in that matrix but in the
// "inputScreenOffset" shader parameter, read from two globals that the function consumes (and zeroes)
// before it enqueues. The matrix and the offset's y are scaled here, where they are still unread.
constexpr uintptr_t kSubmitUiMesh = 0x00557860;
float* const kScreenOffset = reinterpret_cast<float*>(0x00c7c120);   // x, y (NDC)
// FUN_00628590, the minimap's on-screen draw (its calls to FUN_00557860 return inside this range), and
// the constant it scales its x nudge by (0x00b49b98, a double: 33/640).
constexpr uintptr_t kMinimapBegin = 0x00628590;
constexpr uintptr_t kMinimapEnd = 0x00628ab4;
constexpr float kMinimapNudge = 0.0515625f;

SubmitFn g_real_submit_mesh = nullptr;

uint32_t __cdecl SubmitUiMeshDetour(uint32_t a1, uint32_t a2, uint32_t a3, uint32_t a4, uint32_t a5,
                                    uint32_t a6, uint32_t a7, uint32_t a8, const float* transform,
                                    uint32_t a10) {
    const float fit = g_map_depth > 0 ? 1.0f : CurrentUiFit();
    if (g_trace) {
        const float offset[2] = {kScreenOffset[0], kScreenOffset[1]};
        uint32_t extra[3] = {a10};
        std::memcpy(&extra[1], offset, sizeof(offset));
        TraceDraw(1, &a1, fit, transform, extra, 3);
    }
    if (fit >= 0.9999f) return g_real_submit_mesh(a1, a2, a3, a4, a5, a6, a7, a8, transform, a10);
    float scaled[16];
    ScaleNdc(transform, fit, scaled);
    // Only y: the offset's x is plain NDC across the real width (no 16:9 factor to divide back out),
    // so it already lands 1:1; its y spans the stretched 720-tall canvas like everything else.
    kScreenOffset[1] *= fit;
    // The minimap (FUN_00628590) adds its own nudge to x: 33 px * (f - 1), with f the engine's aspect
    // relative to 16:9 (FUN_00483650: W / (H * 16/9), 0.9 at 1280x800, exactly 1 at 720p). With the
    // UI laid out as at 720p that nudge is 3.3 px off, so it is taken back out. (f itself cannot be
    // forced to 1: other consumers, the glyph caches among them, are built against its real value.)
    const uintptr_t ret = reinterpret_cast<uintptr_t>(__builtin_return_address(0));
    if (ret >= kMinimapBegin && ret < kMinimapEnd) {
        const float relative = static_cast<float>(*reinterpret_cast<const LONG*>(kTargetWidth)) /
                               (static_cast<float>(*reinterpret_cast<const LONG*>(kTargetHeight)) *
                                kDesignWidth / kDesignHeight);
        kScreenOffset[0] -= (relative - 1.0f) * kMinimapNudge;
    }
    InterlockedIncrement(&g_rescaled);
    return g_real_submit_mesh(a1, a2, a3, a4, a5, a6, a7, a8, scaled, a10);
}

// FUN_005a1750 (__thiscall, 5 stack args, ret 0x14): text. The glyph quad's vertices are laid out in
// the same NDC space as everything else and placed by a translation matrix built with FUN_00516e70
// (cdecl: out, &translation), which the function then converts into the instance's own transform
// format (not a plain 4x4, so it is not patched after the fact). While FUN_005a1750 runs for a
// full-screen target, FUN_00516e70's result becomes scale(fit) * translation(fit * t): the glyphs'
// extents and their position are scaled together, exactly like the quads above.
constexpr uintptr_t kDrawText = 0x005a1750;
constexpr uintptr_t kMakeTranslation = 0x00516e70;

__thread float g_text_fit = 1.0f;

using DrawTextFn = void(__fastcall*)(void* self, void* edx, uint32_t a2, uint32_t a3, uint32_t a4,
                                     uint32_t a5, uint32_t a6);
DrawTextFn g_real_draw_text = nullptr;

void __fastcall DrawTextDetour(void* self, void* edx, uint32_t a2, uint32_t a3, uint32_t a4,
                               uint32_t a5, uint32_t a6) {
    const float previous = g_text_fit;
    g_text_fit = CurrentUiFit();
    text_prefilter::OnTextDraw(reinterpret_cast<const void*>(a2), reinterpret_cast<const void*>(a3));
    if (g_trace) {
        // a2 is the text's texture (width/height at +0x1c/+0x20); a3 the quad, 4 vertices of 7 dwords:
        // u, v in texels (float), then x, y in canvas pixels (int) at +4/+5. Logged: the texture
        // size, the first and last vertex, and the first dwords of the sampler state at this+0xab50.
        const auto* texture = reinterpret_cast<const uint32_t*>(a2);
        const auto* quad = reinterpret_cast<const uint32_t*>(a3);
        const auto* sampler = reinterpret_cast<const uint32_t*>(static_cast<uint8_t*>(self) + 0xab50);
        uint32_t extra[12 + 12] = {texture[7], texture[8], quad[0], quad[1], quad[4], quad[5],
                                   quad[21], quad[22], quad[25], quad[26], sampler[0], sampler[1]};
        std::memcpy(&extra[12], texture, 12 * 4);  // the texture header, for its mip count
        TraceDraw(2, &a2, g_text_fit, nullptr, extra, 24);
    }
    g_real_draw_text(self, edx, a2, a3, a4, a5, a6);
    g_text_fit = previous;
}

using MakeTranslationFn = float*(__cdecl*)(float* out, const float* translation);
MakeTranslationFn g_real_make_translation = nullptr;

float* __cdecl MakeTranslationDetour(float* out, const float* translation) {
    float* result = g_real_make_translation(out, translation);
    const float fit = g_text_fit;
    if (fit < 0.9999f && result != nullptr) {
        float scaled[16];
        ScaleNdc(result, fit, scaled);
        if (g_anchor_depth > 0) AnchorToEdge(scaled, scaled[13] >= 0.0f, fit);
        std::memcpy(result, scaled, sizeof(scaled));
        InterlockedIncrement(&g_rescaled);
    }
    return result;
}

// FUN_00556a70 (__thiscall, 2 stack args, ret 8): a pixel-space sprite, submitted through
// FUN_005558c0. Its rect on the 1280x720 canvas: size at +0x40/+0x44, top-left at +0x48/+0x4c,
// scale at +0x50/+0x54. The game parks sprites it does not want seen just past the canvas edge -
// the battle turn order's bonus icons for turns below the list sit at y = 720 - where the cover-fit
// UI of the unmodified game (and a real 720p screen) never shows them. With the canvas letterboxed
// they would land in the 40 px bands, so a sprite wholly outside the canvas is not drawn. One that
// only overhangs it is drawn as before.
constexpr uintptr_t kDrawPixelSprite = 0x00556a70;

using PixelSpriteFn = void(__fastcall*)(void* self, void* edx, uint32_t a2, uint32_t a3);
PixelSpriteFn g_real_pixel_sprite = nullptr;

// The sprite's rect on the canvas, false when it has no size of its own (some sprites leave +0x40/+0x44
// at 0 and carry their extent in their mesh only).
bool SpriteRect(const uint8_t* sprite, float* left, float* top, float* right, float* bottom) {
    const float* f = reinterpret_cast<const float*>(sprite + 0x40);
    const float w = f[0] * f[4];
    const float h = f[1] * f[5];
    if (w == 0.0f || h == 0.0f) return false;
    *left = std::min(f[2], f[2] + w);
    *right = std::max(f[2], f[2] + w);
    *top = std::min(f[3], f[3] + h);
    *bottom = std::max(f[3], f[3] + h);
    return true;
}

bool OutsideCanvas(const uint8_t* sprite) {
    float left, top, right, bottom;
    if (!SpriteRect(sprite, &left, &top, &right, &bottom)) return false;
    return right <= 0.0f || left >= kDesignWidth || bottom <= 0.0f || top >= kDesignHeight;
}

bool CoversCanvasRect(const uint8_t* sprite) {
    float left, top, right, bottom;
    return SpriteRect(sprite, &left, &top, &right, &bottom) && left <= 0.0f && top <= 0.0f &&
           right >= kDesignWidth && bottom >= kDesignHeight;
}

void __fastcall PixelSpriteDetour(void* self, void* edx, uint32_t a2, uint32_t a3) {
    const auto* sprite = static_cast<const uint8_t*>(self);
    const uint8_t* previous = g_trace_sprite;
    g_trace_sprite = sprite;
    if (CurrentUiFit() < 0.9999f && OutsideCanvas(sprite)) {
        if (g_trace) TraceDraw(4, &a2, 1.0f, nullptr, nullptr, 0);
        g_trace_sprite = previous;
        return;
    }
    const int cover = CoversCanvasRect(sprite) ? 1 : 0;
    g_cover_sprite += cover;
    g_real_pixel_sprite(self, edx, a2, a3);
    g_cover_sprite -= cover;
    g_trace_sprite = previous;
}

// FUN_00592ec0 (__fastcall, the sprite in ecx): a sprite attached to the 3D scene - the "Talk"
// bubble over an NPC is one. It submits through the same UI mesh path as the HUD (FUN_00557860,
// directly or via FUN_00558690), but the transform it passes is the sprite's *world* matrix (its
// +0x28: billboard axes in world units, row 3 the world position) and the shader projects it with
// the scene camera, which this mod leaves at the native aspect. Scaling that matrix like a UI one
// would move the sprite in the world (the bubble drifted left and down), so nothing drawn from here
// is rescaled: it stays exactly where the unmodified game projects it.
constexpr uintptr_t kDrawWorldSprite = 0x00592ec0;

using WorldSpriteFn = void(__fastcall*)(void* self);
WorldSpriteFn g_real_world_sprite = nullptr;

// FUN_005e78f0 (__thiscall, one float, ret 4): the running speed lines. Each line is a UI mesh
// (FUN_00557860) but its transform comes from FUN_0044df50 with the field camera (object+0x7bc,
// +0x1d8), so like the scene sprites it is projected by the 3D camera and is not rescaled.
constexpr uintptr_t kDrawSpeedLines = 0x005e78f0;
// FUN_005e7e30 (__fastcall): drawn while running, a copy of the frame (object+0x178c) laid over it
// with a fading alpha (the motion ghost), on a quad sized to the 16:9 canvas: 1.778 x 1 through
// FUN_005558c0, or the 1344x756 rect through FUN_005581f0. At 16:10 the unmodified game stretches that
// quad 800/720 wide like any UI, so the copy overhangs the screen by 11% and ghosts sideways; the
// uniform 0.9 left it 720 rows tall instead. It has to cover the screen exactly, so only x is fitted.
constexpr uintptr_t kDrawScreenFlash = 0x005e7e30;

using SpeedLinesFn = void(__fastcall*)(void* self, void* edx, float t);
SpeedLinesFn g_real_speed_lines = nullptr;

void __fastcall SpeedLinesDetour(void* self, void* edx, float t) {
    ++g_world_depth;
    g_real_speed_lines(self, edx, t);
    --g_world_depth;
}

using ScreenFlashFn = void(__fastcall*)(void* self);
ScreenFlashFn g_real_screen_flash = nullptr;

void __fastcall ScreenFlashDetour(void* self) {
    ++g_screen_copy_depth;
    g_real_screen_flash(self);
    --g_screen_copy_depth;
}

// FUN_004e4110 (__fastcall, the screen in ecx): draws the whole Battle Result screen - its child
// elements (+0x1ac..+0x1c4, +0x474..+0x490) through their vtable +0xac, text included. It has a header
// frame on the canvas' top edge and a footer near its bottom, so its UI is anchored to the screen edges
// (AnchorToEdge) rather than centred with the canvas.
constexpr uintptr_t kDrawBattleResult = 0x004e4110;
WorldSpriteFn g_real_battle_result = nullptr;

void __fastcall BattleResultDetour(void* self) {
    const int anchored = g_anchor_screens ? 1 : 0;
    g_anchor_depth += anchored;
    g_real_battle_result(self);
    g_anchor_depth -= anchored;
}

// FUN_006293d0 (__fastcall, the map in ecx, returns int): the area map - the full-screen map and, into
// its 266x266 texture, the minimap. It points the main camera at the map, then draws the floor plane
// and every marker on it (path dots, chests, the current location, exits: FUN_00626ca0 ->
// FUN_00558690 -> FUN_00557860) with world matrices that the shader projects with that camera, like
// the scene sprites. Rescaled as UI they slid off the floor toward the centre on the full-screen map;
// the minimap was never affected, its target is not the frame. Its UI mesh draws are left alone; its
// quads (the canvas-sized backdrops) are fitted as usual.
constexpr uintptr_t kDrawMap = 0x006293d0;

using MapFn = int(__fastcall*)(void* self);
MapFn g_real_map = nullptr;

int __fastcall MapDetour(void* self) {
    ++g_map_depth;
    const int result = g_real_map(self);
    --g_map_depth;
    return result;
}

void __fastcall WorldSpriteDetour(void* self) {
    if (g_trace) TraceDraw(3, static_cast<char*>(__builtin_frame_address(0)) + 8, 1.0f, nullptr, nullptr, 0);
    ++g_world_depth;
    g_real_world_sprite(self);
    --g_world_depth;
}

// ---------------------------------------------------------------- the save thumbnail
// FUN_00442660 reads the frame back (W x H at this+0x2fc/+0x300) and fills the 320x176 thumbnail in
// a loop at 0x00442858..0x00442960 that samples 176 rows over the whole height, then writes source
// row r to thumbnail row r / f + (f - 1) * 88, f being the engine's aspect factor (FUN_004835e0, 0.9
// at 16:10). That is the unmodified game's own 16:10 handling: it crops the middle 16:9 by stretching
// rows apart, so every tenth thumbnail row is never written and stays black. With the letterbox on,
// the middle 1280x720 of the frame is exactly the 720p picture, so the loop is replaced by one that
// samples just that band, row for row. The loop's locals, off the game's ebp: [-0x2c] this,
// [-0x1c] the readback, [-0x20] its pitch, [-0x28] its rows, [-0x10] the 320x176 BGR output.
constexpr uintptr_t kThumbLoop = 0x00442858;       // mov dword [ebp-0x18], 0 - the loop's start
constexpr uintptr_t kThumbLoopDone = 0x00442960;   // first instruction after it
constexpr int kThumbWidth = 320;
constexpr int kThumbHeight = 176;

// ---------------------------------------------------------------- forced 1280x800 at start
// FUN_007bf0c0 (__fastcall, the 0x50-byte settings object in ecx, returns it) reads settings.xml
// once, into the singleton at [0x01304dd0] that FUN_007bf7c0 creates on first use; +0x10/+0x14 are
// the resolution (1280x720 when the file has none). The window, the swapchain and fullscreen's
// display mode (FUN_007bf840) are all created from those two fields, and the game never writes the
// file (only the launcher does), so overriding them in memory ignores the file's resolution without
// changing it.
constexpr uintptr_t kReadSettings = 0x007bf0c0;
constexpr uintptr_t kSettingsSingleton = 0x01304dd0;
constexpr int32_t kDeckWidth = 1280;
constexpr int32_t kDeckHeight = 800;

int32_t g_force_deck_resolution = 0;

using ReadSettingsFn = void*(__fastcall*)(void* self);
ReadSettingsFn g_real_read_settings = nullptr;

void ForceResolution(uint8_t* settings) {
    auto* size = reinterpret_cast<int32_t*>(settings + 0x10);
    Log("deckscreen: resolution %dx%d from settings.xml forced to %dx%d", size[0], size[1],
        kDeckWidth, kDeckHeight);
    size[0] = kDeckWidth;
    size[1] = kDeckHeight;
}

void* __fastcall ReadSettingsDetour(void* self) {
    void* result = g_real_read_settings(self);
    if (g_force_deck_resolution && result != nullptr) {
        ForceResolution(static_cast<uint8_t*>(result));
    }
    return result;
}

// The size is only safe to change before the game uses it: it copies the two fields into its
// application object, creates the window and the swapchain from them, and much else reads them again
// later (the aspect factor, the speed lines), so a late override leaves the window at the file's size
// and the rest believing 1280x800. The loader holds the game's startup until the mods have
// initialised, so the hook gets there first; if the settings were read anyway, they are left alone.
void CheckNotReadYet() {
    if (!g_force_deck_resolution) return;
    const auto* settings = *reinterpret_cast<const uint8_t* const*>(kSettingsSingleton);
    if (settings == nullptr) return;
    const auto* size = reinterpret_cast<const int32_t*>(settings + 0x10);
    Log("deckscreen: settings.xml was read before this mod loaded - NOT forcing 1280x800, the "
        "game keeps %dx%d (is the loader's entry gate working?)", size[0], size[1]);
}

}  // namespace

extern "C" {
void* g_thumb_trampoline = nullptr;
int atmt_thumb_resample(uintptr_t ebp);

// Runs the replacement and skips the game's loop when it returns nonzero; otherwise the game's loop
// runs untouched (letterbox off, or a 16:9 screen).
__attribute__((naked, used)) void atmt_thumb_detour() {
    __asm__ volatile(
        "pushal\n\t"
        "pushl %ebp\n\t"
        "call _atmt_thumb_resample\n\t"
        "addl $4, %esp\n\t"
        "testl %eax, %eax\n\t"
        "popal\n\t"
        "jz 1f\n\t"
        "pushl $0x00442960\n\t"
        "ret\n\t"
        "1:\n\t"
        "jmp *_g_thumb_trampoline\n\t");
}

int atmt_thumb_resample(uintptr_t ebp) {
    if (!g_enabled) return 0;
    const auto* self = *reinterpret_cast<const uint8_t* const*>(ebp - 0x2c);
    const int w = *reinterpret_cast<const int*>(self + 0x2fc);
    const int h = *reinterpret_cast<const int*>(self + 0x300);
    const auto* src = *reinterpret_cast<const uint8_t* const*>(ebp - 0x1c);
    const int pitch = *reinterpret_cast<const int*>(ebp - 0x20);
    const int src_rows = *reinterpret_cast<const int*>(ebp - 0x28);
    auto* dst = *reinterpret_cast<uint8_t* const*>(ebp - 0x10);
    if (w <= 0 || h <= 0 || src == nullptr || dst == nullptr || pitch < w * 3) return 0;
    const float fit = FitFor(w, h);
    if (fit >= 0.9999f) return 0;
    const int band = static_cast<int>(h * fit + 0.5f);
    const int top = (h - band) / 2;
    for (int row = 0; row < kThumbHeight; ++row) {
        const int from_row = std::min(top + row * band / kThumbHeight, src_rows - 1);
        const uint8_t* in = src + from_row * pitch;
        uint8_t* out = dst + row * kThumbWidth * 3;
        for (int col = 0; col < kThumbWidth; ++col) {
            const uint8_t* pixel = in + (col * w / kThumbWidth) * 3;
            out[col * 3 + 0] = pixel[2];
            out[col * 3 + 1] = pixel[1];
            out[col * 3 + 2] = pixel[0];
        }
    }
    Log("deckscreen: save thumbnail taken from rows %d..%d of %dx%d", top, top + band - 1, w, h);
    return 1;
}
}  // extern "C"

namespace {

DWORD WINAPI PollThread(LPVOID) {
    for (int tick = 0;; ++tick) {
        RefreshClientSize();
        if (tick == 40) Log("deckscreen: %ld UI draws rescaled", g_rescaled);
        if (g_trace && g_api->game_dir != nullptr) {
            wchar_t path[MAX_PATH];
            _snwprintf(path, MAX_PATH - 1, L"%ls\\deckscreen.dump", g_api->game_dir);
            path[MAX_PATH - 1] = L'\0';
            if (DeleteFileW(path)) {
                Log("deckscreen: dumping the next 1500 draws");
                InterlockedExchange(&g_dump_left, 1500);
            }
        }
        Sleep(500);
    }
    return 0;
}

}  // namespace

extern "C" {

__declspec(dllexport) uint32_t __cdecl AtmtModInit(const AtmtModApi* api) {
    if (api == nullptr || api->version < ATMT_MOD_API_VERSION) return 0;
    if (InterlockedCompareExchange(&g_started, 1, 0) != 0) {
        if (api->log != nullptr) api->log("deckscreen: already initialised in this process, declining");
        return 0;
    }
    g_api = api;
    static const AtmtSetting kSettings[] = {
        {ATMT_SETTING_BOOL, ATMT_SETTING_RESTART, "General", "Enabled", "Enabled",
         "the whole mod: the letterbox, crisp small text and the forced 1280x800", &g_mod_enabled},
        {ATMT_SETTING_BOOL, ATMT_SETTING_LIVE, "General", "Letterbox", "Letterbox the UI",
         "show the 1280x720 UI 1:1 and centered when the screen is taller than 16:9\n"
         "(the 3D keeps the full native resolution)",
         &g_enabled},
        {ATMT_SETTING_BOOL, ATMT_SETTING_LIVE, "General", "CrispText", "Crisp small text",
         "prefilter each line of text to the size it is drawn at, so small text (shrunk 2x or\n"
         "more from the game's 48-texel lines) keeps even, complete strokes",
         &g_crisp_text},
        {ATMT_SETTING_FLOAT, ATMT_SETTING_LIVE, "General", "CrispTextWeight", "Crisp text weight",
         "brightens thin strokes of crisp small text: 1 = exact coverage, higher = bolder",
         &g_text_weight, 0, 1.0f, 2.0f, 0.1f},
        {ATMT_SETTING_BOOL, ATMT_SETTING_RESTART, "General", "ForceDeckResolution",
         "Force 1280x800",
         "start the game at the Steam Deck's 1280x800 whatever settings.xml says\n"
         "(the file itself is left as it is)",
         &g_force_deck_resolution},
        {ATMT_SETTING_BOOL, ATMT_SETTING_LIVE, "General", "AnchoredScreens",
         "Header/footer screens at the edges",
         "screens framed by a header and a footer (Battle Result) keep their top half at the\n"
         "top of the screen and their bottom half at the bottom, instead of centred",
         &g_anchor_screens},
        {ATMT_SETTING_BOOL, ATMT_SETTING_LIVE | ATMT_SETTING_ADVANCED, "Debug", "TraceDraws",
         "Trace UI draws",
         "log every distinct UI draw (hook, target, call chain) to atmt_loader.log", &g_trace},
    };
    api->settings_register(api, "Deckscreen", kSettings, sizeof(kSettings) / sizeof(kSettings[0]));
    if (g_mod_enabled == 0) {
        Log("deckscreen: disabled by its ini");
        return ATMT_MOD_API_VERSION;
    }
    FindGameText();
    text_prefilter::Init(api, &Log, &g_crisp_text, &g_text_weight);
    if (g_enabled == 0) Log("deckscreen: letterbox off in its ini - hooks installed, drawing untouched");
    // The first bytes of every hook target (and the thumbnail loop's exit, which the replacement
    // loop returns to) as the supported ed8.exe has them. SenPatcher (v1.3.1 and its current
    // sources) patches none of these. Any mismatch: nothing is hooked.
    static const atmt_code::Expect kExpected[] = {
        {kSubmitUiQuad, "\x55\x8b\xec\x81\xec\xb4\x00\x00", 8, "FUN_005558c0"},
        {kSubmitUiMesh, "\x55\x8b\xec\x81\xec\xcc\x00\x00", 8, "FUN_00557860"},
        {kDrawText, "\x55\x8b\xec\x81\xec\x84\x01\x00", 8, "FUN_005a1750"},
        {kMakeTranslation, "\x55\x8b\xec\x83\xec\x10\x8b\x45", 8, "FUN_00516e70"},
        {kDrawBattleResult, "\x56\x8b\xf1\x80\xbe\x94\x04\x00", 8, "FUN_004e4110"},
        {kDrawWorldSprite, "\x55\x8b\xec\x6a\xff\x68\x14\x81", 8, "FUN_00592ec0"},
        {kDrawMap, "\x55\x8b\xec\x81\xec\x44\x01\x00", 8, "FUN_006293d0"},
        {kDrawPixelSprite, "\x55\x8b\xec\x81\xec\x08\x01\x00", 8, "FUN_00556a70"},
        {kDrawSpeedLines, "\x55\x8b\xec\x81\xec\x80\x01\x00", 8, "FUN_005e78f0"},
        {kDrawScreenFlash, "\x55\x8b\xec\x81\xec\xa8\x00\x00", 8, "FUN_005e7e30"},
        {kThumbLoop, "\xc7\x45\xe8\x00\x00\x00\x00\xeb", 8, "the save thumbnail loop"},
        {kThumbLoopDone, "\x8b\x4d\x14\x33\xff\x3b\xcf", 7, "the save thumbnail loop's end"},
        {kReadSettings, "\x55\x8b\xec\x6a\xff\x68\x7b\xd0", 8, "FUN_007bf0c0 (settings.xml)"},
    };
    char why_not[256];
    if (!atmt_code::CheckAll(kExpected, why_not, sizeof(why_not))) {
        LogError("deckscreen: %s - not installed, the UI is left as is", why_not);
        return ATMT_MOD_API_VERSION;
    }
    bool ok = true;
    struct {
        uintptr_t target;
        void* detour;
        void** trampoline;
        const char* name;
    } hooks[] = {
        {kSubmitUiQuad, reinterpret_cast<void*>(&SubmitUiQuadDetour),
         reinterpret_cast<void**>(&g_real_submit_quad), "FUN_005558c0"},
        {kSubmitUiMesh, reinterpret_cast<void*>(&SubmitUiMeshDetour),
         reinterpret_cast<void**>(&g_real_submit_mesh), "FUN_00557860"},
        {kDrawText, reinterpret_cast<void*>(&DrawTextDetour),
         reinterpret_cast<void**>(&g_real_draw_text), "FUN_005a1750"},
        {kMakeTranslation, reinterpret_cast<void*>(&MakeTranslationDetour),
         reinterpret_cast<void**>(&g_real_make_translation), "FUN_00516e70"},
        {kDrawBattleResult, reinterpret_cast<void*>(&BattleResultDetour),
         reinterpret_cast<void**>(&g_real_battle_result), "FUN_004e4110 (Battle Result)"},
        {kDrawWorldSprite, reinterpret_cast<void*>(&WorldSpriteDetour),
         reinterpret_cast<void**>(&g_real_world_sprite), "FUN_00592ec0"},
        {kDrawMap, reinterpret_cast<void*>(&MapDetour), reinterpret_cast<void**>(&g_real_map),
         "FUN_006293d0 (area map)"},
        {kDrawPixelSprite, reinterpret_cast<void*>(&PixelSpriteDetour),
         reinterpret_cast<void**>(&g_real_pixel_sprite), "FUN_00556a70"},
        {kDrawSpeedLines, reinterpret_cast<void*>(&SpeedLinesDetour),
         reinterpret_cast<void**>(&g_real_speed_lines), "FUN_005e78f0"},
        {kDrawScreenFlash, reinterpret_cast<void*>(&ScreenFlashDetour),
         reinterpret_cast<void**>(&g_real_screen_flash), "FUN_005e7e30"},
        {kThumbLoop, reinterpret_cast<void*>(&atmt_thumb_detour), &g_thumb_trampoline,
         "the save thumbnail loop (0x00442858)"},
        {kReadSettings, reinterpret_cast<void*>(&ReadSettingsDetour),
         reinterpret_cast<void**>(&g_real_read_settings), "FUN_007bf0c0 (settings.xml)"},
    };
    for (const auto& hook : hooks) {
        void* handle =
            api->hook_create(reinterpret_cast<void*>(hook.target), hook.detour, hook.trampoline);
        const bool hooked = handle != nullptr && api->hook_enable(handle) == 0;
        if (!hooked) LogError("deckscreen: could NOT hook %s", hook.name);
        ok = ok && hooked;
    }
    if (ok) {
        Log("deckscreen: UI submit functions hooked");
    } else {
        LogError("deckscreen: hooking failed - UI left as is");
    }
    CheckNotReadYet();
    if (ok) {
        HANDLE thread = CreateThread(nullptr, 0, PollThread, nullptr, 0, nullptr);
        if (thread != nullptr) CloseHandle(thread);
    }
    return ATMT_MOD_API_VERSION;
}

__declspec(dllexport) void __cdecl AtmtModShutdown(void) {}

// One sentence for the overlay and the manager (shared/mod_api.h, AtmtModDescription).
__declspec(dllexport) const char* __cdecl AtmtModDescription(void) {
    return "Runs the game at the screen's native 16:10 resolution (Steam Deck 1280x800) with the "
           "16:9 UI letterboxed and crisp small text.";
}

}  // extern "C"
