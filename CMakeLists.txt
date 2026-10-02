cmake_minimum_required(VERSION 3.20)
project(atmt CXX C)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD 11)

if(NOT CMAKE_SIZEOF_VOID_P EQUAL 4)
    message(WARNING "ed8.exe is 32-bit; build with a 32-bit toolchain "
                    "(e.g. -DCMAKE_C_COMPILER=i686-w64-mingw32-gcc)")
endif()

# Reproducible dlls: no link time stamp, so an unchanged mod builds to the same bytes (the manager's
# releases tell a changed component from an unchanged one by its files' hashes, tools\release.ps1).
if(MINGW)
    add_link_options(-Wl,--no-insert-timestamp)
endif()

# The ASM thunks of the loader's proxy flavour (loader/src/proxy_gfsdk.s).
enable_language(ASM)

set(ATMT_SHARED_DIR ${CMAKE_CURRENT_SOURCE_DIR}/shared)   # mod_api.h, ini.h, ...

# Everything this tree builds that is meant to be used lands in dist/ (see docs/BUILD.md); the build
# tree itself only holds intermediates.
#   dist/game/                      what goes into the game folder: the loader and atmt_mods/
#   dist/tools/                     developer tools (the injectable loader flavour, atmt_inject)
set(ATMT_DIST ${CMAKE_CURRENT_SOURCE_DIR}/dist)
set(ATMT_GAME_DIST ${ATMT_DIST}/game)
set(ATMT_MODS_DIST ${ATMT_GAME_DIST}/atmt_mods)
set(ATMT_TOOLS_DIST ${ATMT_DIST}/tools)

# ---------------------------------------------------------------- MinHook
# Linked by the loader only: one hook engine per process, shared by all mods.
set(MINHOOK_DIR ${CMAKE_CURRENT_SOURCE_DIR}/third_party/minhook)
set(HDE_DIR ${MINHOOK_DIR}/src/hde)
file(GLOB HDE_SOURCES ${HDE_DIR}/*.c)

add_library(minhook STATIC
    ${MINHOOK_DIR}/src/buffer.c
    ${MINHOOK_DIR}/src/hook.c
    ${MINHOOK_DIR}/src/trampoline.c
    ${HDE_SOURCES})
target_include_directories(minhook PUBLIC ${MINHOOK_DIR}/include ${MINHOOK_DIR}/src)
target_compile_definitions(minhook PRIVATE _WIN32_WINNT=0x0600)

# ---------------------------------------------------------------- ImGui (the overlay panel)
# The overlay and the dialog log's panel draw with ImGui (docs/OVERLAY.md). A git submodule
# (pinned by commit; tools\build.ps1 initialises it when the folder is empty), and this
# fails loudly when it is (a build that silently produces a mod without a panel would be worse).
set(IMGUI_DIR ${CMAKE_CURRENT_SOURCE_DIR}/third_party/imgui)
if(NOT EXISTS ${IMGUI_DIR}/imgui.cpp)
    message(FATAL_ERROR
        "third_party/imgui is missing. Get it with:\n"
        "  git submodule update --init --depth 1 third_party/imgui")
endif()
add_library(imgui STATIC
    ${IMGUI_DIR}/imgui.cpp
    ${IMGUI_DIR}/imgui_draw.cpp
    ${IMGUI_DIR}/imgui_tables.cpp
    ${IMGUI_DIR}/imgui_widgets.cpp
    ${IMGUI_DIR}/backends/imgui_impl_dx11.cpp
    ${IMGUI_DIR}/backends/imgui_impl_win32.cpp)
target_include_directories(imgui PUBLIC ${IMGUI_DIR} ${IMGUI_DIR}/backends)
# The pad is read and swallowed by the mod itself (src/pad.cpp): ImGui's win32 backend must not
# take XInput over, or two things would be polling and filtering the same device.
target_compile_definitions(imgui PUBLIC IMGUI_IMPL_WIN32_DISABLE_GAMEPAD)

# ---------------------------------------------------------------- subprojects
# The loader (generic hook host) and one directory per mod. Adding a mod means
# copying mods/_template, renaming it and adding one line here (and an entry in
# manager/payload/components.json to ship it).
add_subdirectory(loader)
add_subdirectory(mods/overlay)
add_subdirectory(mods/trails_dialog_logger)
add_subdirectory(mods/load_last_save)
add_subdirectory(mods/deckscreen)
add_subdirectory(mods/stick_rotation_speed)
add_subdirectory(mods/battle_load)
# add_subdirectory(mods/my_next_mod)

# ---------------------------------------------------------------- tests + tools
# Console harnesses; EXCLUDE_FROM_ALL so a normal build stays quick and build.ps1
# decides what to build and run.
add_subdirectory(tests)
