// bgm_info.cpp - "now playing": the title of each new piece of music, as a toast in the overlay.
//
// A port of FalcomBGMInfo (https://github.com/Zorkats/FalcomBGMInfo, third_party/FalcomBGMInfo) to
// the toolkit. Its idea and its data are kept as they are; what it did on its own is taken from
// the toolkit instead:
//
//   * the song: the game opens a track's file (data\bgm\wav\ed8NNN.wav) when it starts playing it,
//     so kernel32's CreateFileA/CreateFileW are hooked (through the loader's single MinHook, not a
//     copy of it) and the path is looked up in FalcomBGMInfo's map, embedded in the dll at build time
//     (bgm_map.h). No game address is touched, so there is nothing to check with code_check.h.
//   * the drawing: a window in the overlay (shared/overlay_api.h) - no Present hook, no ImGui context
//     and no D3D of its own. The note icon is drawn with ImGui shapes instead of the original's dds.
//   * the settings: the loader's registry (the overlay's bar, the manager), not a yaml file and an
//     F1 window of its own.
//   * the font: Japanese titles need a font that has them, and the overlay's has not. The original
//     shipped one; this mod uses one the system already has (FontPath, or the first of Meiryo, Yu
//     Gothic, MS Gothic in the fonts folder - Proton's prefix has an MS Gothic). Without one, the
//     Japanese line is left out rather than drawn as boxes.
//
// A file the game opens again while it is still the current song (a loop, a reload) is not a new
// song. A song that comes back (field -> battle -> field) is announced again only after
// CooldownMinutes, as in the original.
#include "bgm_map.h"

#include "atmt_input.h"
#include "atmt_theme.h"
#include "imgui.h"
#include "mod_api.h"
#include "overlay_api.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "bgm_map_cs1.inc"   // kBgmMapCs1: third_party/FalcomBGMInfo/assets/BgmMap_CS1.yaml (CMakeLists.txt)

namespace {

const AtmtModApi* g_api = nullptr;
const AtmtOverlayApi* g_overlay = nullptr;
bgm::Map g_map;
volatile LONG g_map_ready = 0;
volatile LONG g_stop = 0;
volatile LONG g_window_added = 0;

void LogV(void(__cdecl* sink)(const char*), const char* fmt, va_list args) {
    if (sink == nullptr) return;
    char buf[768];
    _vsnprintf(buf, sizeof(buf) - 1, fmt, args);
    buf[sizeof(buf) - 1] = '\0';
    sink(buf);
}
void Log(const char* fmt, ...) {
    if (g_api == nullptr) return;
    va_list args;
    va_start(args, fmt);
    LogV(g_api->log, fmt, args);
    va_end(args);
}
void LogError(const char* fmt, ...) {
    if (g_api == nullptr) return;
    va_list args;
    va_start(args, fmt);
    LogV(g_api->log_error, fmt, args);
    va_end(args);
}

std::string Narrow(const wchar_t* text) {
    if (text == nullptr) return std::string();
    const int n = WideCharToMultiByte(CP_UTF8, 0, text, -1, nullptr, 0, nullptr, nullptr);
    if (n <= 1) return std::string();
    std::string out(static_cast<size_t>(n - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text, -1, &out[0], n, nullptr, nullptr);
    return out;
}

std::wstring Widen(const char* text) {
    if (text == nullptr || *text == '\0') return std::wstring();
    const int n = MultiByteToWideChar(CP_UTF8, 0, text, -1, nullptr, 0);
    if (n <= 1) return std::wstring();
    std::wstring out(static_cast<size_t>(n - 1), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text, -1, &out[0], n);
    return out;
}

bool ReadWholeFile(const std::wstring& path, std::vector<char>* out) {
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER size;
    bool ok = GetFileSizeEx(file, &size) != 0 && size.QuadPart > 0 && size.QuadPart < (64 << 20);
    if (ok) {
        out->resize(static_cast<size_t>(size.QuadPart));
        DWORD read = 0;
        ok = ReadFile(file, out->data(), static_cast<DWORD>(out->size()), &read, nullptr) != 0
             && read == out->size();
    }
    CloseHandle(file);
    return ok;
}

// ---------------------------------------------------------------- settings
const char kMenu[] = "Music";
const char* const kPositions[] = {"TopRight", "TopLeft", "BottomRight", "BottomLeft"};
enum Position { kTopRight = 0, kTopLeft, kBottomRight, kBottomLeft };

int32_t g_enabled = 1;
float g_seconds = 5.0f;
int32_t g_cooldown_minutes = 300;
int32_t g_show_japanese = 1;
int32_t g_show_album = 1;
int32_t g_text_size = 22;
int32_t g_position = kTopRight;
int32_t g_opacity = 85;
char g_key_name[32] = "F6";
char g_pad_name[32] = "";
char g_font_path[260] = "";
int32_t g_diagnostics = 0;

// Render thread state (update/draw), and what the settings bar changes there.
atmt::KeyEdge g_key;
unsigned short g_pad_chord = 0;
bool g_pad_was_down = false;
volatile LONG g_show_requested = 0;

void __cdecl OnInputChanged(const AtmtSetting*, void*) {
    const unsigned chord = atmt::KeyChordFromName(g_key_name);
    if (g_key_name[0] != '\0' && chord == 0) {
        LogError("bgm_info: ShowKey=%s is not a key this mod knows - no key", g_key_name);
    }
    if (chord != g_key.key()) {
        g_key.set_key(chord);
        g_key.Reset();
    }
    g_pad_chord = atmt::PadChordFromName(g_pad_name);
    if (g_pad_name[0] != '\0' && g_pad_chord == 0) {
        LogError("bgm_info: ShowPad=%s is not a pad chord - no pad button", g_pad_name);
    }
}

void __cdecl OnShowNow(const AtmtSetting*, void*) { InterlockedExchange(&g_show_requested, 1); }

const AtmtSetting kSettings[] = {
    {ATMT_SETTING_ACTION, 0, nullptr, nullptr, "Show the current song",
     "the toast for the music playing now (also ShowKey / ShowPad)", nullptr, 0, 0, 0, 0, nullptr, 0,
     &OnShowNow},
    {ATMT_SETTING_BOOL, ATMT_SETTING_LIVE, "General", "Enabled", "Announce new songs",
     "show the title when a new piece of music starts (off = only on ShowKey / ShowPad)", &g_enabled},
    {ATMT_SETTING_FLOAT, ATMT_SETTING_LIVE, "General", "ToastSeconds", "Shown for",
     "seconds the toast stays on screen", &g_seconds, 0, 1.0f, 20.0f, 0.5f},
    {ATMT_SETTING_INT, ATMT_SETTING_LIVE, "General", "CooldownMinutes", "Repeat after",
     "minutes before a song that comes back (after a battle, say) is announced again; 0 = every time",
     &g_cooldown_minutes, 0, 0.0f, 1440.0f, 15.0f},
    {ATMT_SETTING_LABEL, 0, nullptr, nullptr, "Toast"},
    {ATMT_SETTING_BOOL, ATMT_SETTING_LIVE, "General", "ShowJapanese", "Japanese title",
     "the original title under the English one (needs a Japanese font, see FontPath)", &g_show_japanese},
    {ATMT_SETTING_BOOL, ATMT_SETTING_LIVE, "General", "ShowAlbum", "Album and track",
     "the soundtrack album, disc and track number", &g_show_album},
    {ATMT_SETTING_INT, ATMT_SETTING_LIVE, "General", "TextSize", "Text size",
     "the title's size in pixels (the other lines are a little smaller)", &g_text_size, 0, 10.0f, 64.0f,
     1.0f},
    {ATMT_SETTING_ENUM, ATMT_SETTING_LIVE, "General", "Position", "Position",
     "the screen corner the toast slides into", &g_position, 0, 0, 0, 0, kPositions, 4},
    {ATMT_SETTING_INT, ATMT_SETTING_LIVE, "General", "OpacityPct", "Opacity",
     "the toast's background, percent", &g_opacity, 0, 0.0f, 100.0f, 5.0f},
    {ATMT_SETTING_LABEL, 0, nullptr, nullptr, "Controls"},
    {ATMT_SETTING_KEY, ATMT_SETTING_LIVE, "General", "ShowKey", "Show key",
     "the key (or chord) that shows the current song again (none = no key)", g_key_name,
     sizeof(g_key_name), 0, 0, 0, nullptr, 0, &OnInputChanged},
    {ATMT_SETTING_PAD_BUTTON, ATMT_SETTING_LIVE, "General", "ShowPad", "Show pad chord",
     "a pad button or chord that shows the current song again (none = keyboard only)", g_pad_name,
     sizeof(g_pad_name), 0, 0, 0, nullptr, 0, &OnInputChanged},
    {ATMT_SETTING_STRING, ATMT_SETTING_RESTART, "General", "FontPath", "Font",
     "a .ttf/.otf/.ttc with Japanese glyphs; empty = Meiryo, Yu Gothic or MS Gothic from the "
     "Windows fonts folder, whichever is there first",
     g_font_path, sizeof(g_font_path)},
    {ATMT_SETTING_BOOL, ATMT_SETTING_ADVANCED | ATMT_SETTING_LIVE, "General", "Diagnostics",
     "Diagnostics",
     "log every music file the game opens and whether the map knows it (atmt_loader.log, LogLevel=all)",
     &g_diagnostics},
};

// ---------------------------------------------------------------- the hooks
// On whatever thread opens a file: no allocation, no lock. The newest song wins (index + 1; 0 = none).
volatile LONG g_pending = 0;

typedef HANDLE(WINAPI* CreateFileAFn)(LPCSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE);
typedef HANDLE(WINAPI* CreateFileWFn)(LPCWSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE);
CreateFileAFn g_create_a = nullptr;
CreateFileWFn g_create_w = nullptr;
void* g_hook_a = nullptr;
void* g_hook_w = nullptr;

template <typename Char>
void Noticed(const Char* path) {
    if (InterlockedCompareExchange(&g_map_ready, 0, 0) == 0 || !bgm::IsAudioPath(path)) return;
    const int index = g_map.Find(path);
    if (index >= 0) InterlockedExchange(&g_pending, index + 1);
    if (g_diagnostics != 0) {
        std::string text;
        if (sizeof(Char) == 1) text = reinterpret_cast<const char*>(path);
        else text = Narrow(reinterpret_cast<const wchar_t*>(path));
        Log("bgm_info: opened %s -> %s", text.c_str(),
            index >= 0 ? g_map.at(index).title.c_str() : "(not in the map)");
    }
}

HANDLE WINAPI CreateFileADetour(LPCSTR name, DWORD access, DWORD share, LPSECURITY_ATTRIBUTES sa,
                                DWORD disposition, DWORD flags, HANDLE templ) {
    if (name != nullptr) Noticed(name);
    return g_create_a(name, access, share, sa, disposition, flags, templ);
}

HANDLE WINAPI CreateFileWDetour(LPCWSTR name, DWORD access, DWORD share, LPSECURITY_ATTRIBUTES sa,
                                DWORD disposition, DWORD flags, HANDLE templ) {
    if (name != nullptr) Noticed(name);
    return g_create_w(name, access, share, sa, disposition, flags, templ);
}

bool InstallHooks() {
    HMODULE kernel32 = GetModuleHandleW(L"kernel32.dll");
    void* a = kernel32 != nullptr ? reinterpret_cast<void*>(GetProcAddress(kernel32, "CreateFileA")) : nullptr;
    void* w = kernel32 != nullptr ? reinterpret_cast<void*>(GetProcAddress(kernel32, "CreateFileW")) : nullptr;
    if (a != nullptr) {
        g_hook_a = g_api->hook_create(a, reinterpret_cast<void*>(&CreateFileADetour),
                                      reinterpret_cast<void**>(&g_create_a));
    }
    if (w != nullptr) {
        g_hook_w = g_api->hook_create(w, reinterpret_cast<void*>(&CreateFileWDetour),
                                      reinterpret_cast<void**>(&g_create_w));
    }
    if (g_hook_a != nullptr) g_api->hook_enable(g_hook_a);
    if (g_hook_w != nullptr) g_api->hook_enable(g_hook_w);
    return g_hook_a != nullptr || g_hook_w != nullptr;
}

// ---------------------------------------------------------------- the font
// Read on the mod's thread (a CJK font is megabytes), added to the overlay's atlas on the render
// thread between frames, and taken out again before this dll goes away: the atlas points into
// g_font_data (FontDataOwnedByAtlas = false).
std::vector<char>* g_font_data = nullptr;
std::string g_font_name;
volatile LONG g_font_offered = 0;
volatile LONG g_font_remove = 0;   // 1 = asked, 2 = done
ImFont* g_font = nullptr;
ImFontAtlas* g_atlas = nullptr;

void FindFont() {
    std::vector<std::wstring> candidates;
    if (g_font_path[0] != '\0') {
        candidates.push_back(Widen(g_font_path));
    } else {
        wchar_t windows[MAX_PATH] = L"";
        const UINT n = GetWindowsDirectoryW(windows, MAX_PATH);
        const std::wstring fonts = std::wstring(windows, n < MAX_PATH ? n : 0) + L"\\Fonts\\";
        for (const wchar_t* file : {L"meiryo.ttc", L"YuGothM.ttc", L"YuGothR.ttc", L"msgothic.ttc"}) {
            candidates.push_back(fonts + file);
        }
    }
    for (const std::wstring& path : candidates) {
        std::vector<char>* data = new std::vector<char>();
        if (ReadWholeFile(path, data)) {
            g_font_data = data;
            g_font_name = Narrow(path.c_str());
            InterlockedExchange(&g_font_offered, 1);
            Log("bgm_info: font %s", g_font_name.c_str());
            return;
        }
        delete data;
    }
    if (g_font_path[0] != '\0') {
        LogError("bgm_info: could not read FontPath=%s - no Japanese titles", g_font_path);
    } else {
        Log("bgm_info: no Japanese font in the fonts folder - no Japanese titles (see FontPath)");
    }
}

void AddFont() {
    if (g_font != nullptr || g_font_data == nullptr) return;
    if (InterlockedCompareExchange(&g_font_offered, 0, 0) != 1) return;
    ImFontAtlas* atlas = ImGui::GetIO().Fonts;
    if (atlas == nullptr || atlas->Locked) return;
    InterlockedExchange(&g_font_offered, 2);   // one attempt: a font the loader refuses stays refused
    ImFontConfig cfg;
    cfg.FontDataOwnedByAtlas = false;
    cfg.Flags |= ImFontFlags_NoLoadError;
    _snprintf(cfg.Name, sizeof(cfg.Name) - 1, "bgm_info");
    g_font = atlas->AddFontFromMemoryTTF(g_font_data->data(), static_cast<int>(g_font_data->size()),
                                         static_cast<float>(g_text_size), &cfg);
    if (g_font == nullptr) {
        LogError("bgm_info: the font %s could not be used - no Japanese titles", g_font_name.c_str());
        return;
    }
    g_atlas = atlas;
    Log("bgm_info: font added to the overlay");
}

void RemoveFont() {
    if (g_font != nullptr && g_atlas != nullptr && !g_atlas->Locked) g_atlas->RemoveFont(g_font);
    g_font = nullptr;
    g_atlas = nullptr;
}

// ---------------------------------------------------------------- the toast
int g_current = -1;                       // the song playing now (index into g_map)
std::map<std::string, ULONGLONG> g_last_shown;   // by title: two files can be the same piece
ULONGLONG g_show_until = 0;
float g_slide = 0.0f;                     // 0 = off screen, 1 = in place
DWORD g_last_tick = 0;
constexpr float kSlideMs = 260.0f;

void Show() { g_show_until = GetTickCount64() + static_cast<ULONGLONG>(g_seconds * 1000.0f); }

void OnSong(int index) {
    if (index == g_current) return;   // the same file opened again: not a new song
    g_current = index;
    const bgm::Song& song = g_map.at(index);
    Log("bgm_info: now playing %s", song.title.c_str());
    if (g_enabled == 0) return;
    const ULONGLONG now = GetTickCount64();
    auto last = g_last_shown.find(song.title);
    const ULONGLONG cooldown = static_cast<ULONGLONG>(g_cooldown_minutes) * 60000ull;
    if (last != g_last_shown.end() && cooldown > 0 && now - last->second < cooldown) {
        Log("bgm_info: %s was shown less than CooldownMinutes ago", song.title.c_str());
        return;
    }
    g_last_shown[song.title] = now;
    Show();
}

void BindImGui(const AtmtOverlayFrame* frame) {
    ImGui::SetCurrentContext(static_cast<ImGuiContext*>(frame->imgui_context));
    ImGui::SetAllocatorFunctions(frame->imgui_alloc, frame->imgui_free, frame->imgui_alloc_user);
}

// Every frame on the render thread, before the overlay's ImGui frame (atlas changes belong here).
uint32_t __cdecl ToastUpdate(const AtmtOverlayFrame* frame, void*) {
    BindImGui(frame);
    if (InterlockedCompareExchange(&g_font_remove, 0, 0) == 1) {
        RemoveFont();
        InterlockedExchange(&g_font_remove, 2);
        return 0;
    }
    AddFont();

    const LONG pending = InterlockedExchange(&g_pending, 0);
    if (pending > 0) OnSong(static_cast<int>(pending - 1));

    // Sampled every frame so the edges stay true; acted on only when hotkeys may act.
    const bool key = g_key.PressedOnce();
    const bool pad_down = g_pad_chord != 0 && frame->input.have_pad != 0
                          && (frame->input.pad_buttons & g_pad_chord) == g_pad_chord;
    const bool pad = pad_down && !g_pad_was_down;
    g_pad_was_down = pad_down;
    const bool hotkeys = (frame->flags & ATMT_OVERLAY_FRAME_HOTKEYS) != 0;
    if (InterlockedExchange(&g_show_requested, 0) != 0 || (hotkeys && (key || pad))) Show();

    const DWORD tick = GetTickCount();
    const DWORD elapsed = g_last_tick == 0 ? 0 : tick - g_last_tick;
    g_last_tick = tick;
    const bool showing = GetTickCount64() < g_show_until;
    const float step = static_cast<float>(elapsed) / kSlideMs;
    g_slide += showing ? step : -step;
    if (g_slide > 1.0f) g_slide = 1.0f;
    if (g_slide < 0.0f) g_slide = 0.0f;

    static bool s_logged_preview = false;
    if ((frame->flags & ATMT_OVERLAY_FRAME_PREVIEW) != 0 && !s_logged_preview) {
        s_logged_preview = true;
        Log("bgm_info: previewing in the settings bar (bar alpha %.2f)", frame->bar_alpha);
    }

    // Drawn while it is on screen, and as a live preview while the bar shows this mod's menu.
    if (g_slide > 0.0f || (frame->flags & ATMT_OVERLAY_FRAME_PREVIEW) != 0) return ATMT_OVERLAY_WANT_DRAW;
    return 0;
}

ImU32 Color(const ImVec4& c, float alpha) {
    return ImGui::ColorConvertFloat4ToU32(ImVec4(c.x, c.y, c.z, c.w * alpha));
}

// A beamed pair of eighth notes in a w x h box.
void DrawNote(ImDrawList* dl, ImVec2 p, float w, float h, ImU32 color) {
    const float r = w * 0.13f;
    const ImVec2 head1(p.x + w * 0.30f, p.y + h * 0.74f);
    const ImVec2 head2(p.x + w * 0.72f, p.y + h * 0.66f);
    const float stem = (w * 0.05f > 1.5f) ? w * 0.05f : 1.5f;
    const float beam = h * 0.10f;
    const ImVec2 top1(head1.x + r - stem * 0.5f, p.y + h * 0.24f);
    const ImVec2 top2(head2.x + r - stem * 0.5f, p.y + h * 0.16f);
    dl->AddEllipseFilled(head1, ImVec2(r * 1.15f, r * 0.9f), color, -0.35f);
    dl->AddEllipseFilled(head2, ImVec2(r * 1.15f, r * 0.9f), color, -0.35f);
    dl->AddLine(ImVec2(top1.x, head1.y), top1, color, stem);
    dl->AddLine(ImVec2(top2.x, head2.y), top2, color, stem);
    dl->AddQuadFilled(ImVec2(top1.x - stem * 0.5f, top1.y), ImVec2(top2.x + stem * 0.5f, top2.y),
                      ImVec2(top2.x + stem * 0.5f, top2.y + beam), ImVec2(top1.x - stem * 0.5f, top1.y + beam),
                      color);
}

struct Line {
    std::string text;
    float size;
    ImVec4 color;
};

// Inside the overlay's ImGui frame.
void __cdecl ToastDraw(const AtmtOverlayFrame* frame, void*) {
    BindImGui(frame);
    const bool preview = (frame->flags & ATMT_OVERLAY_FRAME_PREVIEW) != 0;
    float alpha = g_slide;
    if (preview && frame->bar_alpha > alpha) alpha = frame->bar_alpha;
    if (alpha <= 0.0f) return;
    // ease out on the way in, ease in on the way out: it settles instead of stopping dead
    const float t = 1.0f - (1.0f - alpha) * (1.0f - alpha);

    const float size = static_cast<float>(g_text_size);
    const float small = std::floor(size * 0.78f + 0.5f);
    std::vector<Line> lines;
    if (g_current >= 0) {
        const bgm::Song& song = g_map.at(g_current);
        lines.push_back({song.title, size, atmt::theme::kName});
        if (g_show_japanese != 0 && g_font != nullptr && !song.japanese.empty()) {
            lines.push_back({song.japanese, small, atmt::theme::kText});
        }
        if (g_show_album != 0) {
            std::string where;
            if (!song.disc.empty()) where = "Disc " + song.disc;
            if (!song.track.empty()) where += (where.empty() ? "Track " : ", Track ") + song.track;
            if (!song.album.empty()) lines.push_back({song.album, small, atmt::theme::kHint});
            if (!where.empty()) lines.push_back({where, small, atmt::theme::kHint});
        }
    } else {
        lines.push_back({"Now playing", size, atmt::theme::kName});
        lines.push_back({"no song has started yet", small, atmt::theme::kHint});
    }

    ImGui::PushFont(g_font, size);   // NULL = the overlay's font, at this size
    ImFont* font = ImGui::GetFont();
    const float s = size / 22.0f;
    const float pad_x = 14.0f * s, pad_y = 10.0f * s, spacing = 3.0f * s, margin = 16.0f;
    float text_w = 0.0f, text_h = 0.0f;
    for (const Line& line : lines) {
        const ImVec2 m = font->CalcTextSizeA(line.size, 1e9f, 0.0f, line.text.c_str());
        if (m.x > text_w) text_w = m.x;
        text_h += m.y;
    }
    text_h += spacing * static_cast<float>(lines.size() - 1);
    const float h = text_h + pad_y * 2.0f;
    const float icon = h * 0.8f;
    const float w = pad_x + icon + pad_x * 0.8f + text_w + pad_x;

    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    const bool right = g_position == kTopRight || g_position == kBottomRight;
    const bool top = g_position == kTopRight || g_position == kTopLeft;
    float x = right ? viewport->Pos.x + viewport->Size.x - margin - w : viewport->Pos.x + margin;
    const float y = top ? viewport->Pos.y + margin : viewport->Pos.y + viewport->Size.y - margin - h;
    const float travel = (1.0f - t) * (w + margin);   // it slides in from its own edge
    x += right ? travel : -travel;

    static bool s_logged_draw = false;
    if (!s_logged_draw) {
        s_logged_draw = true;
        Log("bgm_info: first toast at %.0f,%.0f, %.0fx%.0f, alpha %.2f, screen %.0fx%.0f, font %s", x, y, w,
            h, alpha, viewport->Size.x, viewport->Size.y, g_font != nullptr ? "Japanese" : "overlay's");
    }

    ImDrawList* dl = ImGui::GetForegroundDrawList();
    const float rounding = 6.0f * s;
    const float opacity = static_cast<float>(g_opacity) / 100.0f;
    dl->AddRectFilled(ImVec2(x, y), ImVec2(x + w, y + h), Color(atmt::theme::kSheet, opacity * alpha), rounding);
    dl->AddRect(ImVec2(x, y), ImVec2(x + w, y + h), Color(atmt::theme::kRule, alpha), rounding, 0,
                s > 1.0f ? 1.5f * s : 1.5f);
    DrawNote(dl, ImVec2(x + pad_x, y + (h - icon) * 0.5f), icon, icon, Color(atmt::theme::kRule, alpha));

    float ty = y + pad_y;
    const float tx = x + pad_x + icon + pad_x * 0.8f;
    for (const Line& line : lines) {
        dl->AddText(font, line.size, ImVec2(tx, ty), Color(line.color, alpha), line.text.c_str());
        ty += font->CalcTextSizeA(line.size, 1e9f, 0.0f, line.text.c_str()).y + spacing;
    }
    ImGui::PopFont();
}

// ---------------------------------------------------------------- start-up
void LoadMap() {
    // A map of one's own (another release's file names, corrected titles) replaces the built-in one.
    const std::wstring own = std::wstring(g_api->mod_dir) + L"\\bgm_info_map.yaml";
    std::vector<char> text;
    size_t skipped = 0;
    size_t count = 0;
    if (ReadWholeFile(own, &text)) {
        count = g_map.Load(text.data(), text.size(), &skipped);
        Log("bgm_info: %u songs from %s (%u lines skipped)", static_cast<unsigned>(count),
            Narrow(own.c_str()).c_str(), static_cast<unsigned>(skipped));
        if (count == 0) LogError("bgm_info: bgm_info_map.yaml has no songs - using the built-in map");
    }
    if (count == 0) {
        count = g_map.Load(reinterpret_cast<const char*>(kBgmMapCs1), sizeof(kBgmMapCs1), &skipped);
        Log("bgm_info: %u songs in the built-in map", static_cast<unsigned>(count));
    }
    InterlockedExchange(&g_map_ready, 1);
}

DWORD WINAPI InitThread(LPVOID) {
    LoadMap();
    if (!InstallHooks()) {
        LogError("bgm_info: could not hook CreateFileA/CreateFileW - no song titles");
        return 0;
    }
    FindFont();

    // The overlay may load before or after this mod: look for its service until it is there.
    for (int i = 0; i < 300 && InterlockedCompareExchange(&g_stop, 0, 0) == 0; ++i) {
        const void* found = g_api->service_find(ATMT_OVERLAY_SERVICE);
        if (found == nullptr) {
            Sleep(100);
            continue;
        }
        const AtmtOverlayApi* overlay = static_cast<const AtmtOverlayApi*>(found);
        if (overlay->version < ATMT_OVERLAY_API_VERSION || overlay->size < sizeof(AtmtOverlayApi)) {
            LogError("bgm_info: the overlay mod is older than this one - no toast");
            return 0;
        }
        AtmtOverlayWindow window;
        memset(&window, 0, sizeof(window));
        window.name = kMenu;
        window.imgui_version = IMGUI_VERSION_NUM;
        window.update = &ToastUpdate;
        window.draw = &ToastDraw;
        if (overlay->add_window(g_api, &window) != 0) {
            LogError("bgm_info: the overlay refused the toast (see the line above)");
            return 0;
        }
        g_overlay = overlay;
        InterlockedExchange(&g_window_added, 1);
        Log("bgm_info: ready");
        return 0;
    }
    if (InterlockedCompareExchange(&g_stop, 0, 0) == 0) {
        LogError("bgm_info: no overlay mod (atmt_overlay.dll) in this process - no toast");
    }
    return 0;
}

}  // namespace

extern "C" {

__declspec(dllexport) uint32_t __cdecl AtmtModInit(const AtmtModApi* api) {
    if (api == nullptr || api->version < ATMT_MOD_API_VERSION || api->size < sizeof(AtmtModApi)) return 0;
    if (api->hook_create == nullptr || api->service_find == nullptr) return 0;
    g_api = api;
    if (api->settings_register != nullptr) {
        api->settings_register(api, kMenu, kSettings, sizeof(kSettings) / sizeof(kSettings[0]));
    }
    OnInputChanged(nullptr, nullptr);

    HANDLE thread = CreateThread(nullptr, 0, InitThread, nullptr, 0, nullptr);
    if (thread == nullptr) return 0;
    CloseHandle(thread);
    return ATMT_MOD_API_VERSION;
}

__declspec(dllexport) void __cdecl AtmtModShutdown(void) {
    InterlockedExchange(&g_stop, 1);
    if (g_api == nullptr) return;
    if (g_hook_a != nullptr) g_api->hook_remove(g_hook_a);
    if (g_hook_w != nullptr) g_api->hook_remove(g_hook_w);
    g_hook_a = g_hook_w = nullptr;
    bool font_out = true;
    if (InterlockedCompareExchange(&g_window_added, 0, 0) != 0) {
        // The font leaves the atlas on the render thread, in the next update.
        InterlockedExchange(&g_font_remove, 1);
        for (int i = 0; i < 100 && InterlockedCompareExchange(&g_font_remove, 0, 0) != 2; ++i) Sleep(10);
        font_out = InterlockedCompareExchange(&g_font_remove, 0, 0) == 2;
    }
    if (InterlockedExchange(&g_window_added, 0) != 0 && g_overlay != nullptr) {
        g_overlay->remove_window(g_api);   // waits for a frame in progress
    }
    // Still in an atlas that never ran another frame: its memory stays (a leak beats a dangling font).
    if (font_out) {
        delete g_font_data;
        g_font_data = nullptr;
    }
    g_api->settings_unregister(g_api);
}

__declspec(dllexport) const char* __cdecl AtmtModDescription(void) {
    return "A port of FalcomBGMInfo by Zorkats: shows the title, album and track of each new piece "
           "of music as it starts.";
}

}  // extern "C"
