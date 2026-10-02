# ATMT Manager - installer and configurator (docs/MANAGER.md).
#
# A project of its own, next to the 32-bit game build: the manager is a normal 64-bit program for
# Windows (llvm-mingw x86_64 or MSVC x64) and Linux x86_64 (the Steam Deck). tools\build_manager.ps1
# builds it; the Linux build runs in Docker.
#
#   cmake -S manager -B build/manager-win -DCMAKE_BUILD_TYPE=Release
#   cmake --build build/manager-win
#
#   -DATMT_MANAGER_GUI=OFF        core + CLI only (no SDL2)
#   -DATMT_RELEASE_REPO=owner/x   the public GitHub repository releases come from: the update check asks
#                                 its latest release's manifest.json ("" = no update checks)
#   -DATMT_MANIFEST_URL=https://... another manifest.json instead
cmake_minimum_required(VERSION 3.20)
project(atmt_manager CXX C)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD 11)

set(ATMT_MANAGER_VERSION "0.1.0" CACHE STRING "the app's version")
option(ATMT_MANAGER_GUI "build the window (SDL2 + Dear ImGui)" ON)
set(ATMT_RELEASE_REPO "" CACHE STRING "owner/name of the public GitHub repository with the releases")
set(ATMT_MANIFEST_URL "" CACHE STRING "the manifest.json update checks ask (default: the repository's latest release's)")
if(ATMT_MANIFEST_URL STREQUAL "" AND NOT ATMT_RELEASE_REPO STREQUAL "")
    set(ATMT_MANIFEST_URL "https://github.com/${ATMT_RELEASE_REPO}/releases/latest/download/manifest.json")
endif()

set(ROOT ${CMAKE_CURRENT_SOURCE_DIR}/..)
set(THIRD ${ROOT}/third_party)

if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(WARNING "the manager is meant to be a 64-bit program")
endif()

# ---------------------------------------------------------------- release key
# manager/release_pubkey.txt is the minisign public key of the release signing key (the secret half
# never enters the repo: atmt_manager --cli keygen, docs/MANAGER.md "Releases").
set(ATMT_RELEASE_PUBKEY "")
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/release_pubkey.txt)
    file(STRINGS ${CMAKE_CURRENT_SOURCE_DIR}/release_pubkey.txt _key_lines)
    foreach(_line IN LISTS _key_lines)
        if(NOT _line MATCHES "^untrusted comment:" AND NOT _line STREQUAL "")
            set(ATMT_RELEASE_PUBKEY "${_line}")
        endif()
    endforeach()
endif()
configure_file(src/core/release_config.h.in ${CMAKE_CURRENT_BINARY_DIR}/generated/release_config.h @ONLY)

# ---------------------------------------------------------------- Monocypher (Ed25519, BLAKE2b)
set(MONO ${THIRD}/monocypher)
if(NOT EXISTS ${MONO}/src/monocypher.c)
    message(FATAL_ERROR "third_party/monocypher is missing. Get it with:\n"
        "  git submodule update --init --depth 1 third_party/monocypher")
endif()
add_library(monocypher STATIC ${MONO}/src/monocypher.c ${MONO}/src/optional/monocypher-ed25519.c)
target_include_directories(monocypher PUBLIC ${MONO}/src ${MONO}/src/optional)

# ---------------------------------------------------------------- zstd (the icon pack's archives)
# Only the library's common, compress and decompress parts; its xxhash.h gives XXH64 too. The
# assembly Huffman decoder is left out (ZSTD_DISABLE_ASM), so every compiler builds it the same.
set(ZSTD ${THIRD}/zstd/lib)
if(NOT EXISTS ${ZSTD}/zstd.h)
    message(FATAL_ERROR "third_party/zstd is missing. Get it with:\n"
        "  git submodule update --init --depth 1 third_party/zstd")
endif()
file(GLOB ZSTD_SOURCES ${ZSTD}/common/*.c ${ZSTD}/compress/*.c ${ZSTD}/decompress/*.c)
add_library(zstd_lib STATIC ${ZSTD_SOURCES})
target_include_directories(zstd_lib PUBLIC ${ZSTD} ${ZSTD}/common)
target_compile_definitions(zstd_lib PUBLIC ZSTD_DISABLE_ASM)

# ---------------------------------------------------------------- manager_core (no UI code)
# The app's icon, compiled in: the Steam library entry points at a copy the app writes out.
set(ICON_PNG ${CMAKE_CURRENT_SOURCE_DIR}/packaging/atmt-manager.png)
set(ICON_H ${CMAKE_CURRENT_BINARY_DIR}/generated/app_icon.h)
add_custom_command(OUTPUT ${ICON_H}
    COMMAND ${CMAKE_COMMAND} -DIN=${ICON_PNG} -DOUT=${ICON_H} -DNAME=kAppIconPng -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/embed_file.cmake
    DEPENDS ${ICON_PNG} ${CMAKE_CURRENT_SOURCE_DIR}/cmake/embed_file.cmake
    COMMENT "embedding the app's icon")

add_library(manager_core STATIC
    ${ICON_H}
    src/core/archive.cpp
    src/core/falcom_pkg.cpp
    src/core/hash.cpp
    src/core/icon_pack.cpp
    src/core/http.cpp
    src/core/install.cpp
    src/core/json.cpp
    src/core/locator.cpp
    src/core/p3a.cpp
    src/core/payload.cpp
    src/core/platform.cpp
    src/core/senpatcher.cpp
    src/core/settings.cpp
    src/core/sign.cpp
    src/core/steam_shortcuts.cpp
    src/core/updater.cpp
    src/core/util.cpp)
target_include_directories(manager_core PUBLIC src ${ROOT}/shared ${CMAKE_CURRENT_BINARY_DIR}/generated)
target_compile_definitions(manager_core PUBLIC ATMT_MANAGER_VERSION="${ATMT_MANAGER_VERSION}")
target_link_libraries(manager_core PUBLIC monocypher zstd_lib)
if(WIN32)
    target_compile_definitions(manager_core PUBLIC UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX _WIN32_WINNT=0x0601)
    target_link_libraries(manager_core PUBLIC winhttp bcrypt shell32 advapi32 ole32 uuid)
else()
    target_link_libraries(manager_core PUBLIC ${CMAKE_DL_LIBS})
endif()

# ---------------------------------------------------------------- the app
set(APP_SOURCES src/main.cpp src/cli.cpp)
if(ATMT_MANAGER_GUI)
    # SDL2, built from source and linked statically: the AppImage and the exe then need nothing but
    # the system's video/input libraries. A git submodule like the others.
    set(SDL2_DIR_SRC ${THIRD}/SDL2)
    if(NOT EXISTS ${SDL2_DIR_SRC}/CMakeLists.txt)
        message(FATAL_ERROR "third_party/SDL2 is missing. Get it with:\n"
            "  git submodule update --init --depth 1 third_party/SDL2")
    endif()
    set(SDL_SHARED OFF CACHE BOOL "" FORCE)
    set(SDL_STATIC ON CACHE BOOL "" FORCE)
    set(SDL_TEST OFF CACHE BOOL "" FORCE)
    set(SDL2_DISABLE_INSTALL ON CACHE BOOL "" FORCE)
    set(SDL2_DISABLE_UNINSTALL ON CACHE BOOL "" FORCE)
    add_subdirectory(${SDL2_DIR_SRC} ${CMAKE_CURRENT_BINARY_DIR}/SDL2 EXCLUDE_FROM_ALL)

    set(IMGUI_DIR ${THIRD}/imgui)
    if(NOT EXISTS ${IMGUI_DIR}/imgui.cpp)
        message(FATAL_ERROR "third_party/imgui is missing. Get it with:\n"
            "  git submodule update --init --depth 1 third_party/imgui")
    endif()
    add_library(manager_imgui STATIC
        ${IMGUI_DIR}/imgui.cpp
        ${IMGUI_DIR}/imgui_draw.cpp
        ${IMGUI_DIR}/imgui_tables.cpp
        ${IMGUI_DIR}/imgui_widgets.cpp
        ${IMGUI_DIR}/backends/imgui_impl_sdl2.cpp
        ${IMGUI_DIR}/backends/imgui_impl_sdlrenderer2.cpp)
    target_include_directories(manager_imgui PUBLIC ${IMGUI_DIR} ${IMGUI_DIR}/backends)
    target_link_libraries(manager_imgui PUBLIC SDL2::SDL2-static)

    # Roboto (ImGui's misc/fonts, Apache 2.0) compiled in: ImGui 1.92 scales a TTF to any size.
    set(FONT_TTF ${IMGUI_DIR}/misc/fonts/Roboto-Medium.ttf)
    set(FONT_H ${CMAKE_CURRENT_BINARY_DIR}/generated/font_roboto.h)
    add_custom_command(OUTPUT ${FONT_H}
        COMMAND ${CMAKE_COMMAND} -DIN=${FONT_TTF} -DOUT=${FONT_H} -DNAME=kRobotoTtf -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/embed_file.cmake
        DEPENDS ${FONT_TTF} ${CMAKE_CURRENT_SOURCE_DIR}/cmake/embed_file.cmake
        COMMENT "embedding the window's font")

    list(APPEND APP_SOURCES src/gui/gui.cpp src/gui/app.cpp src/gui/screens.cpp ${FONT_H})
endif()

add_executable(atmt_manager ${APP_SOURCES})
target_link_libraries(atmt_manager PRIVATE manager_core)
if(ATMT_MANAGER_GUI)
    target_compile_definitions(atmt_manager PRIVATE ATMT_MANAGER_GUI)
    target_link_libraries(atmt_manager PRIVATE manager_imgui)
endif()
if(MINGW)
    target_link_options(atmt_manager PRIVATE -static)
endif()
if(UNIX)
    # glibc stays dynamic (the Deck has it); the C++ runtime travels inside the binary
    target_link_options(atmt_manager PRIVATE -static-libstdc++ -static-libgcc)
endif()

# ---------------------------------------------------------------- tests
add_executable(atmt_manager_test tests/test_manager.cpp)
target_link_libraries(atmt_manager_test PRIVATE manager_core)
if(MINGW)
    target_link_options(atmt_manager_test PRIVATE -static)
endif()
enable_testing()
add_test(NAME manager COMMAND atmt_manager_test)
