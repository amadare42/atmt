# load_last_save - the "-al" mod: load the newest save on startup.
#
# A mod of the loader, deliberately separate from trails_dialog_logger: it shares the loader's
# services (paths, log, hooks) and nothing else. See docs/MODS.md.
set(MOD_SOURCES
    src/mod_entry.cpp
    src/options.cpp
    src/saves.cpp
    src/trigger.cpp
    src/title_load.cpp
    src/probe.cpp
    src/probe_reader.cpp
    src/probe_writers.cpp
    src/probe_callbacks.cpp
    src/probe_request.cpp
    src/probe_flow.cpp
    src/probe_fade.cpp
    src/probe_memset.cpp
    src/probe_fopen.cpp
    src/pad_inject.cpp)

add_library(load_last_save SHARED ${MOD_SOURCES})
target_include_directories(load_last_save PRIVATE src ${ATMT_SHARED_DIR})
target_compile_definitions(load_last_save PRIVATE _WIN32_WINNT=0x0600)
# shell32 for CommandLineToArgvW/SHGetKnownFolderPath, ole32 for CoTaskMemFree
target_link_libraries(load_last_save PRIVATE shell32 ole32)
if(MINGW)
    target_link_options(load_last_save PRIVATE -static)
endif()
set_target_properties(load_last_save PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${ATMT_MODS_DIST}
    PREFIX ""
    OUTPUT_NAME "load_last_save"
    SUFFIX ".dll")
