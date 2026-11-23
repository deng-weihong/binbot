add_library(binbot_ambient_display STATIC
    src/display.c
    src/theme_registry.c
    src/render_lvgl.c
    src/format.c
    src/themes/face.c
    src/themes/scene.c
    src/themes/reference.c
    src/themes/pixel.c
)
target_include_directories(binbot_ambient_display PUBLIC include PRIVATE src)
target_link_libraries(binbot_ambient_display PUBLIC lvgl_vendor PRIVATE Freetype::Freetype PkgConfig::CJSON m)
target_compile_options(binbot_ambient_display PRIVATE -Wall -Wextra -Wpedantic)

if(BUILD_TESTING AND NOT CMAKE_CROSSCOMPILING)
    add_executable(test_ambient_display tests/test_ambient_display.c)
    target_link_libraries(test_ambient_display PRIVATE binbot_ambient_display)
    target_compile_options(test_ambient_display PRIVATE -Wall -Wextra -Wpedantic -UNDEBUG)
    add_test(NAME ambient_display COMMAND test_ambient_display)
    set_tests_properties(ambient_display PROPERTIES ENVIRONMENT "BINBOT_ASSET_ROOT=${PROJECT_SOURCE_DIR}/assets")
endif()

