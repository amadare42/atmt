# quality_tweaks - small gameplay conveniences, each behind its own setting. See src/quality_tweaks.cpp.
add_library(quality_tweaks SHARED src/quality_tweaks.cpp)
target_include_directories(quality_tweaks PRIVATE src ${ATMT_SHARED_DIR})
target_compile_definitions(quality_tweaks PRIVATE _WIN32_WINNT=0x0600)
if(MINGW)
    target_link_options(quality_tweaks PRIVATE -static)
endif()
set_target_properties(quality_tweaks PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${ATMT_MODS_DIST}
    PREFIX ""
    OUTPUT_NAME "quality_tweaks"
    SUFFIX ".dll")
