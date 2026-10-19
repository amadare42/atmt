# auto_advance - turns dialog pages by itself after a reading time per character, never before the page's
# voice line ends, with a small pie timer in the overlay (shared/overlay_api.h). See src/auto_advance.cpp.
add_library(auto_advance SHARED src/auto_advance.cpp)
target_include_directories(auto_advance PRIVATE src ${ATMT_SHARED_DIR})
target_compile_definitions(auto_advance PRIVATE _WIN32_WINNT=0x0600)
# ImGui's core only: the timer draws into the overlay's context.
target_link_libraries(auto_advance PRIVATE imgui)
if(MINGW)
    target_link_options(auto_advance PRIVATE -static)
endif()
set_target_properties(auto_advance PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${ATMT_MODS_DIST}
    PREFIX ""
    OUTPUT_NAME "auto_advance"
    SUFFIX ".dll")
