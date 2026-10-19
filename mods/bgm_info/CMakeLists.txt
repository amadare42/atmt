# bgm_info - "now playing": the title of each new piece of music, as a toast in the overlay.
#
# A port of FalcomBGMInfo (third_party/FalcomBGMInfo, a git submodule) to the toolkit: the song list
# is FalcomBGMInfo's own map for Cold Steel I, compiled into the dll from the submodule, and the
# toast is a window in the overlay mod (shared/overlay_api.h). See src/bgm_info.cpp.
set(BGM_INFO_MAP ${CMAKE_SOURCE_DIR}/third_party/FalcomBGMInfo/assets/BgmMap_CS1.yaml)
if(NOT EXISTS ${BGM_INFO_MAP})
    message(FATAL_ERROR
        "third_party/FalcomBGMInfo is missing. Get it with:\n"
        "  git submodule update --init --depth 1 third_party/FalcomBGMInfo")
endif()

set(BGM_INFO_GEN ${CMAKE_CURRENT_BINARY_DIR}/generated)
add_custom_command(
    OUTPUT ${BGM_INFO_GEN}/bgm_map_cs1.inc
    COMMAND ${CMAKE_COMMAND} -DIN=${BGM_INFO_MAP} -DOUT=${BGM_INFO_GEN}/bgm_map_cs1.inc -DNAME=kBgmMapCs1
            -P ${CMAKE_SOURCE_DIR}/manager/cmake/embed_file.cmake
    DEPENDS ${BGM_INFO_MAP} ${CMAKE_SOURCE_DIR}/manager/cmake/embed_file.cmake
    COMMENT "Embedding FalcomBGMInfo's Cold Steel I map")

# A target of its own, so the offline test (tests/, atmt_bgm_test) can wait for it as well.
add_custom_target(bgm_info_map DEPENDS ${BGM_INFO_GEN}/bgm_map_cs1.inc)
set(BGM_INFO_GEN ${BGM_INFO_GEN} PARENT_SCOPE)

add_library(bgm_info SHARED src/bgm_info.cpp src/bgm_map.cpp)
add_dependencies(bgm_info bgm_info_map)
target_include_directories(bgm_info PRIVATE src ${ATMT_SHARED_DIR} ${BGM_INFO_GEN})
target_compile_definitions(bgm_info PRIVATE _WIN32_WINNT=0x0600)
# ImGui's core only: the toast draws into the overlay's context.
target_link_libraries(bgm_info PRIVATE imgui)
if(MINGW)
    target_link_options(bgm_info PRIVATE -static)
endif()
set_target_properties(bgm_info PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${ATMT_MODS_DIST}
    PREFIX ""
    OUTPUT_NAME "bgm_info"
    SUFFIX ".dll")
