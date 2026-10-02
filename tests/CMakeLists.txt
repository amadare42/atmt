# tests + diagnostic tools.
#
# Everything here runs without the game where possible: the point is to catch a broken
# loader/mod before it reaches a game session (an earlier bug cost a real game start: a
# proxy dll that did not forward an export the NVIDIA driver needed).
set(MOD_SRC ${CMAKE_SOURCE_DIR}/mods/trails_dialog_logger/src)

function(atmt_console target)
    target_include_directories(${target} PRIVATE ${MOD_SRC} ${ATMT_SHARED_DIR})
    target_compile_definitions(${target} PRIVATE _WIN32_WINNT=0x0600)
    if(MINGW)
        target_link_options(${target} PRIVATE -static)
    endif()
endfunction()

# ---------------------------------------------------------------- self test
# What the logger still has: the ini reader, the text helpers and the log sink (including
# the one case that waits: a line whose name plate the game writes a moment later), plus the
# pieces the overlay panel decides with - the ring of lines it shows and the ini's key names.
add_executable(atmt_selftest EXCLUDE_FROM_ALL
    test_logger.cpp
    ${MOD_SRC}/config.cpp
    ${MOD_SRC}/logsink.cpp
    ${MOD_SRC}/history.cpp
    ${MOD_SRC}/pad.cpp
    ${MOD_SRC}/host.cpp
    ${MOD_SRC}/util.cpp
    ${MOD_SRC}/addr_hook.cpp
    ${MOD_SRC}/itf_font.cpp)
atmt_console(atmt_selftest)

# ---------------------------------------------------------------- the "-al" mod (docs/MODS.md)
# Everything that decides what the mod does, with no game and no loader involved: the command
# line switch, which files in the save folder are saves, and which one is newest.
add_executable(atmt_al_test EXCLUDE_FROM_ALL
    test_load_last_save.cpp
    ${CMAKE_SOURCE_DIR}/mods/load_last_save/src/options.cpp
    ${CMAKE_SOURCE_DIR}/mods/load_last_save/src/saves.cpp
    ${CMAKE_SOURCE_DIR}/mods/load_last_save/src/probe_reader.cpp
    ${CMAKE_SOURCE_DIR}/mods/load_last_save/src/probe_request.cpp
    ${CMAKE_SOURCE_DIR}/mods/load_last_save/src/probe_flow.cpp
    ${CMAKE_SOURCE_DIR}/mods/load_last_save/src/pad_inject.cpp
    ${CMAKE_SOURCE_DIR}/mods/load_last_save/src/trigger.cpp)
atmt_console(atmt_al_test)
target_link_libraries(atmt_al_test PRIVATE shell32 ole32)

# ---------------------------------------------------------------- settings registry
# The loader's settings registry and the in-place ini writer it saves through, linked
# straight from the loader's sources: no dll, no game.
set(LOADER_SRC ${CMAKE_SOURCE_DIR}/loader/src)
add_executable(atmt_settings_test EXCLUDE_FROM_ALL
    test_settings.cpp
    ${LOADER_SRC}/loader_settings.cpp
    ${LOADER_SRC}/loader_log.cpp
    ${LOADER_SRC}/loader_paths.cpp)
atmt_console(atmt_settings_test)
target_include_directories(atmt_settings_test PRIVATE ${LOADER_SRC})

# ---------------------------------------------------------------- loader test
# Loads the auto-load flavour of the loader exactly as the game would, checks that
# its exports forward to the renamed original, that it discovers the mod folder,
# starts the mod, and that the mod reports what it did. Run from a folder containing
# the loader, the renamed original (or atmt_dummy_orig) and atmt_mods\.
add_executable(atmt_loader_test EXCLUDE_FROM_ALL test_loader.cpp)
atmt_console(atmt_loader_test)

# A stand-in for GFSDK_SSAO_D3D11.win32.dll (the library the loader impersonates),
# for machines without the game: same two exports, recognisable answers.
add_library(atmt_dummy_orig SHARED dummy_orig.cpp)
target_compile_definitions(atmt_dummy_orig PRIVATE _WIN32_WINNT=0x0600)
if(MINGW)
    target_link_options(atmt_dummy_orig PRIVATE -static)
endif()
set_target_properties(atmt_dummy_orig PROPERTIES
    PREFIX ""
    OUTPUT_NAME "atmt_dummy_orig"
    SUFFIX ".dll")

# ---------------------------------------------------------------- diagnostics
# atmt_modules: which modules inside a live process import a given dll (the census
# that decides which dll a proxy may safely replace).
add_executable(atmt_modules EXCLUDE_FROM_ALL list_modules.cpp)
atmt_console(atmt_modules)
target_link_libraries(atmt_modules PRIVATE psapi)

# atmt_inject: inject a dll by path into a running 32-bit process.
add_executable(atmt_inject EXCLUDE_FROM_ALL injector32.cpp)
atmt_console(atmt_inject)
set_target_properties(atmt_inject PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${ATMT_TOOLS_DIST})

# ---------------------------------------------------------------- settings schema
# atmt_schema_dump: every mod's settings table as settings_schema.json, for the manager's generated
# settings pages (manager/, docs/MANAGER.md). tools\build.ps1 runs it over the built mods and writes
# dist/settings_schema.json.
add_executable(atmt_schema_dump EXCLUDE_FROM_ALL schema_dump.cpp)
atmt_console(atmt_schema_dump)
