# deckscreen - run the game at its 16:9 resolution (1280x720) on a 16:10 panel (1280x800)
# without any scaling: the engine keeps a true 720p frame, and the swapchain shows it 1:1 with black
# bars. See src/deckscreen.cpp for how.
add_library(deckscreen SHARED src/deckscreen.cpp src/text_prefilter.cpp)

target_include_directories(deckscreen PRIVATE ${ATMT_SHARED_DIR})
target_compile_definitions(deckscreen PRIVATE _WIN32_WINNT=0x0600)
if(MINGW)
    target_link_options(deckscreen PRIVATE -static)
endif()

set_target_properties(deckscreen PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${ATMT_MODS_DIST}
    PREFIX ""
    OUTPUT_NAME "deckscreen"
    SUFFIX ".dll")
