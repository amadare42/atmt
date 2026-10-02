# loader - the generic hook host (see shared/mod_api.h).
#
# Two flavours of the same dll:
#   GFSDK_SSAO_D3D11.win32.dll         auto-load: impersonates a dll the game imports from its own
#                                      folder and forwards it. This is what ships (dist/game/).
#   atmt_loader.dll                    injected by path with atmt_inject (dist/tools/) - for
#                                      development against a game without the proxy installed
#
# Neither knows anything about the game: that is what the mods are for.
set(LOADER_SOURCES
    src/loader_main.cpp
    src/loader_paths.cpp
    src/loader_log.cpp
    src/loader_config.cpp
    src/loader_hooks.cpp
    src/loader_settings.cpp)

function(atmt_loader_common target)
    target_include_directories(${target} PRIVATE src ${ATMT_SHARED_DIR})
    target_link_libraries(${target} PRIVATE minhook user32)
    target_compile_definitions(${target} PRIVATE _WIN32_WINNT=0x0600)
    if(MINGW)
        # the dll ships into a game folder, so it must not depend on libc++/
        # libunwind/winpthread dlls that only exist on a dev machine
        target_link_options(${target} PRIVATE -static)
    endif()
    set_target_properties(${target} PROPERTIES PREFIX "" SUFFIX ".dll")
endfunction()

add_library(atmt_loader SHARED ${LOADER_SOURCES})
atmt_loader_common(atmt_loader)
set_target_properties(atmt_loader PROPERTIES OUTPUT_NAME "atmt_loader"
    RUNTIME_OUTPUT_DIRECTORY ${ATMT_TOOLS_DIST})

add_library(atmt_loader_gfsdk SHARED ${LOADER_SOURCES} src/proxy_gfsdk.s src/gfsdk.def)
atmt_loader_common(atmt_loader_gfsdk)
target_compile_definitions(atmt_loader_gfsdk PRIVATE ATMT_LOADER_PROXY_GFSDK)
set_target_properties(atmt_loader_gfsdk PROPERTIES OUTPUT_NAME "GFSDK_SSAO_D3D11.win32"
    RUNTIME_OUTPUT_DIRECTORY ${ATMT_GAME_DIST})
