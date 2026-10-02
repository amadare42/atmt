# atmt_overlay - the UI over the game, once per process, for every mod.
#
# It owns the Present hook, the ImGui context and backends, the mouse and its cursor, the hooks that
# take the pad/keyboard/mouse away from the game while something is open, and the settings bar (every
# mod's registered settings in one menu bar). Other mods draw in it through the interface it
# publishes (shared/overlay_api.h) - the dialog log's panel is one. See docs/OVERLAY.md.
#
# The dll is named atmt_overlay so it sorts, and so loads, before the mods that use it; they do not
# rely on that (they look the service up again until it is there), it only saves them the wait.
add_library(atmt_overlay SHARED
    src/mod_entry.cpp
    src/host.cpp
    src/input.cpp
    src/settings_bar.cpp)

target_include_directories(atmt_overlay PRIVATE src ${ATMT_SHARED_DIR})
target_compile_definitions(atmt_overlay PRIVATE _WIN32_WINNT=0x0600)
# imgui carries its own include dirs (and IMGUI_IMPL_WIN32_DISABLE_GAMEPAD: the pad is this mod's to
# read and filter, not the backend's); d3dcompiler is what the DX11 backend compiles its shaders
# with, dwmapi is what the win32 backend asks for ("MinGW will require linking with -ldwmapi").
target_link_libraries(atmt_overlay PRIVATE imgui d3d11 dxgi d3dcompiler dwmapi)
if(MINGW)
    target_link_options(atmt_overlay PRIVATE -static)
endif()
set_target_properties(atmt_overlay PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${ATMT_MODS_DIST}
    PREFIX ""
    OUTPUT_NAME "atmt_overlay"
    SUFFIX ".dll")
