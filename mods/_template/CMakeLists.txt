# _template - starter for a new mod.
#
# To create a mod:
#   1. copy this folder to mods/<your_mod> and rename it
#   2. rename src/my_mod.cpp, set the target/output name below
#   3. add_subdirectory(mods/<your_mod>) in the top-level CMakeLists.txt
#   4. build (tools\build.ps1) - the dll lands in dist/game/atmt_mods/; to ship it, list it in
#      manager/payload/components.json with its version (tools\deploy.ps1 then installs it with the
#      other mods; tools\release.ps1 publishes it)
#   5. see docs/ARCHITECTURE.md for the contract and this file for what you get
add_library(my_mod SHARED src/my_mod.cpp)

# mod_api.h and ini.h live in shared/
target_include_directories(my_mod PRIVATE ${ATMT_SHARED_DIR})
target_compile_definitions(my_mod PRIVATE _WIN32_WINNT=0x0600)
if(MINGW)
    # mods ship into a game folder, so keep them free of libc++/libunwind/
    # winpthread dependencies
    target_link_options(my_mod PRIVATE -static)
endif()

set_target_properties(my_mod PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${ATMT_MODS_DIST}
    PREFIX ""
    OUTPUT_NAME "my_mod"
    SUFFIX ".dll")
