// gui.cpp - the window: SDL2 + Dear ImGui (SDL_Renderer backend), gamepad first.
//
// Laid out for the Steam Deck's 1280x800 (docs/MANAGER.md) and scaled from there:
// the text size follows the window's height, so the same layout works on a desktop monitor. In
// Game Mode (gamescope) the window is fullscreen; a pad, the touchscreen and a mouse all work.
#define SDL_MAIN_HANDLED   // main.cpp has the program's main (it is also the CLI)
#include <SDL.h>

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

#include "app.h"
#include "atmt_theme.h"
#include "core/platform.h"
#include "font_roboto.h"
#include "imgui.h"
#include "imgui_impl_sdl2.h"
#include "imgui_impl_sdlrenderer2.h"

namespace atmt {

namespace {

constexpr float kDesignHeight = 800.0f;
constexpr float kBaseFont = 26.0f;   // at 800 px: readable at arm's length on the Deck

void ApplyStyle(float font_px) {
    ImGuiStyle& style = ImGui::GetStyle();
    style = ImGuiStyle();
    theme::ApplyGameTheme(font_px);
    const float s = font_px / theme::kBaseFontSize;
    // larger targets than in the game: a thumb on a touchscreen, a pad's focus rectangle
    style.FramePadding = ImVec2(10.0f * s, 7.0f * s);
    style.ItemSpacing = ImVec2(10.0f * s, 9.0f * s);
    style.GrabMinSize = 18.0f * s;
    style.TabRounding = 4.0f * s;
    style.ScrollbarSize = 16.0f * s;
    style.WindowBorderSize = 0.0f;
    style.FontSizeBase = font_px;
    ImVec4* c = style.Colors;
    const ImVec4 rule = theme::kRule;
    c[ImGuiCol_FrameBg] = ImVec4(0.02f, 0.03f, 0.08f, 1.0f);
    c[ImGuiCol_FrameBgHovered] = ImVec4(rule.x, rule.y, rule.z, 0.20f);
    c[ImGuiCol_FrameBgActive] = ImVec4(rule.x, rule.y, rule.z, 0.35f);
    c[ImGuiCol_CheckMark] = theme::kName;
    c[ImGuiCol_SliderGrab] = rule;
    c[ImGuiCol_SliderGrabActive] = theme::kName;
    c[ImGuiCol_Tab] = ImVec4(rule.x, rule.y, rule.z, 0.12f);
    c[ImGuiCol_TabHovered] = ImVec4(rule.x, rule.y, rule.z, 0.40f);
    c[ImGuiCol_TabSelected] = ImVec4(rule.x, rule.y, rule.z, 0.55f);
    c[ImGuiCol_TabSelectedOverline] = theme::kName;
    c[ImGuiCol_PopupBg] = ImVec4(0.04f, 0.06f, 0.14f, 0.98f);
    c[ImGuiCol_ModalWindowDimBg] = ImVec4(0.0f, 0.0f, 0.0f, 0.55f);
    c[ImGuiCol_NavCursor] = theme::kName;
    c[ImGuiCol_ChildBg] = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);
    c[ImGuiCol_TableHeaderBg] = ImVec4(rule.x, rule.y, rule.z, 0.18f);
    c[ImGuiCol_TableRowBgAlt] = ImVec4(1.0f, 1.0f, 1.0f, 0.03f);
}

}  // namespace

int RunGui(const std::vector<std::string>& args) {
    SDL_SetMainReady();
    SDL_SetHint(SDL_HINT_VIDEO_HIGHDPI_DISABLED, "0");
    SDL_SetHint(SDL_HINT_IME_SHOW_UI, "1");
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMECONTROLLER | SDL_INIT_TIMER) != 0) {
        Log(std::string("cannot start the window: ") + SDL_GetError());
        return 1;
    }
    const bool game_mode = IsSteamGameMode();
    Uint32 flags = SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI;
    if (game_mode) flags |= SDL_WINDOW_FULLSCREEN_DESKTOP;
    SDL_Window* window = SDL_CreateWindow("ATMT Manager", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, 1280, 800, flags);
    if (window == nullptr) {
        Log(std::string("cannot open the window: ") + SDL_GetError());
        SDL_Quit();
        return 1;
    }
    SDL_SetWindowMinimumSize(window, 800, 500);
    SDL_Renderer* renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_PRESENTVSYNC | SDL_RENDERER_ACCELERATED);
    if (renderer == nullptr) renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_SOFTWARE);
    if (renderer == nullptr) {
        Log(std::string("cannot draw: ") + SDL_GetError());
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 1;
    }

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad;
    io.IniFilename = nullptr;   // the layout is fixed; nothing to remember
    io.ConfigNavCursorVisibleAuto = true;
    ImFontConfig cfg;
    cfg.FontDataOwnedByAtlas = false;
    io.Fonts->AddFontFromMemoryTTF(const_cast<unsigned char*>(kRobotoTtf), static_cast<int>(sizeof(kRobotoTtf)),
                                   kBaseFont, &cfg);
    ImGui_ImplSDL2_InitForSDLRenderer(window, renderer);
    ImGui_ImplSDLRenderer2_Init(renderer);

    App app(args);
    float applied_px = 0.0f;
    bool running = true;
    int hot_frames = 10;
    while (running) {
        SDL_Event e;
        // Idle without input or work: wait for an event (a 250 ms tick keeps "game running" fresh).
        // Right after input, and while work runs, frames follow at the display's rate.
        const int timeout = (hot_frames > 0 || app.busy()) ? 16 : 250;
        if (hot_frames > 0) --hot_frames;
        if (SDL_WaitEventTimeout(&e, timeout)) {
            hot_frames = 10;
            do {
                if (!app.HandleEvent(e)) ImGui_ImplSDL2_ProcessEvent(&e);
                if (e.type == SDL_QUIT) running = false;
                if (e.type == SDL_WINDOWEVENT && e.window.event == SDL_WINDOWEVENT_CLOSE
                    && e.window.windowID == SDL_GetWindowID(window)) {
                    running = false;
                }
            } while (SDL_PollEvent(&e));
        }
        int w = 0, h = 0;
        SDL_GetRendererOutputSize(renderer, &w, &h);
        int lw = 0, lh = 0;
        SDL_GetWindowSize(window, &lw, &lh);
        const float px = std::round(std::clamp(kBaseFont * static_cast<float>(lh) / kDesignHeight, 18.0f, 44.0f));
        if (px != applied_px) {
            ApplyStyle(px);
            applied_px = px;
        }
        if (app.PadScroll()) hot_frames = 10;
        ImGui_ImplSDLRenderer2_NewFrame();
        ImGui_ImplSDL2_NewFrame();
        ImGui::NewFrame();
        if (!app.Frame(static_cast<float>(lw), static_cast<float>(lh))) running = false;
        ImGui::Render();
        SDL_RenderSetScale(renderer, io.DisplayFramebufferScale.x, io.DisplayFramebufferScale.y);
        SDL_SetRenderDrawColor(renderer, 10, 13, 30, 255);
        SDL_RenderClear(renderer);
        ImGui_ImplSDLRenderer2_RenderDrawData(ImGui::GetDrawData(), renderer);
        SDL_RenderPresent(renderer);
        (void)w;
        (void)h;
    }

    ImGui_ImplSDLRenderer2_Shutdown();
    ImGui_ImplSDL2_Shutdown();
    ImGui::DestroyContext();
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 0;
}

}  // namespace atmt
