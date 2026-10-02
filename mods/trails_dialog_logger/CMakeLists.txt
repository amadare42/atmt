# trails_dialog_logger - the mod that records the game's dialog, and shows it in a panel.
#
# A mod is a dll in the loader's mod folder that exports AtmtModInit/AtmtModShutdown/
# AtmtModName (see shared/mod_api.h). It gets its paths, its log and its hooks from
# the loader, so it links no hook engine itself.
#
# The mod carries no data: it reads the line from the game's own message setter (see
# src/addr_hook.cpp), so there is no catalog, no cache and no embedded asset.
#
# The panel (src/panel_client.cpp, src/panel.cpp) is a window in the overlay mod's overlay
# (mods/overlay): it draws with ImGui into the overlay's context and owns no D3D, no input hook and
# no cursor of its own - see docs/OVERLAY.md.
set(MOD_SOURCES
    src/mod_entry.cpp
    src/addr_hook.cpp
    src/config.cpp
    src/logsink.cpp
    src/history.cpp
    src/host.cpp
    src/util.cpp
    src/pad.cpp
    src/panel.cpp
    src/panel_client.cpp
    src/panel_font.cpp
    src/itf_font.cpp
    src/dev_reload.cpp)

add_library(trails_dialog_logger SHARED ${MOD_SOURCES})
target_include_directories(trails_dialog_logger PRIVATE src ${ATMT_SHARED_DIR})
target_compile_definitions(trails_dialog_logger PRIVATE _WIN32_WINNT=0x0600)
# ImGui's core only: the panel draws into the overlay's context (shared/overlay_api.h), so none of
# the backends end up in this dll.
target_link_libraries(trails_dialog_logger PRIVATE imgui)
if(MINGW)
    target_link_options(trails_dialog_logger PRIVATE -static)
endif()
set_target_properties(trails_dialog_logger PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${ATMT_MODS_DIST}
    PREFIX ""
    OUTPUT_NAME "trails_dialog_logger"
    SUFFIX ".dll")
