# battle_load - opens the game's own load menu in the middle of a battle (a hotkey or a pad chord)
# and enters the save picked there. See src/battle_load.cpp for how, and what was measured.
add_library(battle_load SHARED src/battle_load.cpp)

target_include_directories(battle_load PRIVATE ${ATMT_SHARED_DIR})
target_compile_definitions(battle_load PRIVATE _WIN32_WINNT=0x0600)
if(MINGW)
    target_link_options(battle_load PRIVATE -static)
endif()

set_target_properties(battle_load PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${ATMT_MODS_DIST}
    PREFIX ""
    OUTPUT_NAME "battle_load"
    SUFFIX ".dll")
