# stick_rotation_speed - multiplies how fast the right stick turns the field/dungeon camera, and
# controls how walking swings the camera around (AutoRotation=game|off|modern).
# See src/stick_rotation_speed.cpp for the exact instruction patched and why.
add_library(stick_rotation_speed SHARED src/stick_rotation_speed.cpp src/camera_test.cpp src/auto_rotation.cpp)

target_include_directories(stick_rotation_speed PRIVATE ${ATMT_SHARED_DIR})
target_compile_definitions(stick_rotation_speed PRIVATE _WIN32_WINNT=0x0600)
if(MINGW)
    target_link_options(stick_rotation_speed PRIVATE -static)
endif()

set_target_properties(stick_rotation_speed PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${ATMT_MODS_DIST}
    PREFIX ""
    OUTPUT_NAME "stick_rotation_speed"
    SUFFIX ".dll")
