find_package(Freetype REQUIRED)
find_package(ZLIB REQUIRED)
find_package(PkgConfig REQUIRED)
pkg_check_modules(CJSON REQUIRED IMPORTED_TARGET libcjson)
pkg_check_modules(DBUS REQUIRED IMPORTED_TARGET dbus-1)

set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${PROJECT_SOURCE_DIR}/version.txt")
file(STRINGS "${PROJECT_SOURCE_DIR}/version.txt" BINBOT_APP_VERSION LIMIT_COUNT 1)
if(NOT BINBOT_APP_VERSION MATCHES "^[0-9]+(\\.[0-9]+)*$")
    message(FATAL_ERROR "version.txt must contain the application firmware version")
endif()
string(TIMESTAMP BINBOT_APP_BUILD_DATE "%Y-%m-%d" UTC)
configure_file(include/binbot/build_info.h.in generated/binbot/build_info.h @ONLY)

if(BINBOT_HOST_SIMULATOR)
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(SDL2 REQUIRED IMPORTED_TARGET sdl2)
endif()

set(LVGL_DIR "${PROJECT_SOURCE_DIR}/third_party/lvgl")
file(GLOB_RECURSE LVGL_SOURCES CONFIGURE_DEPENDS "${LVGL_DIR}/src/*.c")

add_library(lvgl_vendor STATIC ${LVGL_SOURCES})
target_include_directories(lvgl_vendor PUBLIC
    "${PROJECT_SOURCE_DIR}/config"
    "${LVGL_DIR}"
    "${LVGL_DIR}/src"
)
target_compile_definitions(lvgl_vendor PUBLIC LV_CONF_INCLUDE_SIMPLE=1)
target_link_libraries(lvgl_vendor PUBLIC Freetype::Freetype)
if(BINBOT_HOST_SIMULATOR)
    target_compile_definitions(lvgl_vendor PUBLIC BINBOT_HOST_SIMULATOR=1)
    target_link_libraries(lvgl_vendor PUBLIC PkgConfig::SDL2)
endif()

add_subdirectory("${PROJECT_SOURCE_DIR}/libs/ambient_display" "${CMAKE_CURRENT_BINARY_DIR}/ambient_display")

add_library(binbot_core STATIC
    src/core/app_calendar.c
    src/core/app_diagnostics.c
    src/core/app_state.c
    src/core/app_runtime.c
    src/core/app_modules.c
    src/core/app_workspace.c
    src/core/app_vocabulary_audio.c
    src/core/app_weather.c
    src/core/app_firmware.c
)
target_include_directories(binbot_core PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include" "${CMAKE_CURRENT_BINARY_DIR}/generated")
target_link_libraries(binbot_core PUBLIC binbot_component_client binbot_slave_ipc binbot_audio_ipc binbot_control_ipc PkgConfig::CJSON m Threads::Threads)
if(BINBOT_HOST_SIMULATOR)
    target_compile_definitions(binbot_core PRIVATE BINBOT_HOST_SIMULATOR=1)
endif()

add_library(binbot_platform STATIC
    src/platform/platform_linux.c
    src/platform/camera_renderer.c
    src/platform/wifi_manager.c
    src/platform/bluetooth_manager.c
)
target_include_directories(binbot_platform PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(binbot_platform PUBLIC lvgl_vendor PkgConfig::DBUS Threads::Threads)
if(CMAKE_CROSSCOMPILING)
    set(BINBOT_CAMERA_RGA_ROOT "${RV1106_SDK_ROOT}/output/out/media_out"
        CACHE PATH "RV1106 RGA SDK directory")
    target_compile_definitions(binbot_platform PRIVATE BINBOT_HAVE_RGA=1)
    target_include_directories(binbot_platform PRIVATE "${BINBOT_CAMERA_RGA_ROOT}/include")
    target_link_directories(binbot_platform PUBLIC "${BINBOT_CAMERA_RGA_ROOT}/lib")
    target_link_libraries(binbot_platform PUBLIC rga stdc++)
    target_link_options(binbot_platform INTERFACE "-Wl,-rpath-link,${BINBOT_CAMERA_RGA_ROOT}/lib")

    add_executable(binbot_camera_renderer_probe EXCLUDE_FROM_ALL
        "${PROJECT_SOURCE_DIR}/tools/camera_renderer_probe.c")
    target_link_libraries(binbot_camera_renderer_probe PRIVATE binbot_platform)
    target_compile_options(binbot_camera_renderer_probe PRIVATE -Wall -Wextra -Wpedantic -UNDEBUG)
endif()
if(BINBOT_HOST_SIMULATOR)
    target_compile_definitions(binbot_platform PRIVATE BINBOT_HOST_SIMULATOR=1)
endif()

add_library(binbot_presentation STATIC
    src/presentation/lvgl/ui_app.c
    src/presentation/lvgl/ui_icons.c
    src/presentation/lvgl/ui_theme.c
    src/presentation/lvgl/ui_reference.c
    src/presentation/lvgl/ui_resources.c
    src/presentation/lvgl/ui_gesture.c
    src/presentation/lvgl/screens/home_screen.c
    src/presentation/lvgl/screens/music_screen.c
    src/presentation/lvgl/screens/noise_screen.c
    src/presentation/lvgl/screens/apps_screen.c
    src/presentation/lvgl/screens/alarm_screen.c
    src/presentation/lvgl/screens/pomodoro_screen.c
    src/presentation/lvgl/screens/vocabulary_screen.c
    src/presentation/lvgl/screens/todo_screen.c
    src/presentation/lvgl/screens/calendar_screen.c
    src/presentation/lvgl/screens/devices_screen.c
    src/presentation/lvgl/screens/assistant_screen.c
    src/presentation/lvgl/screens/assistant_orb.c
    src/presentation/lvgl/screens/camera_screen.c
    src/presentation/lvgl/screens/lunch_screen.c
    src/presentation/lvgl/screens/settings_screen.c
    src/presentation/lvgl/screens/screensaver_settings_screen.c
    src/presentation/lvgl/screens/gestures_screen.c
    src/presentation/lvgl/screens/about_screen.c
    src/presentation/lvgl/screens/diagnostics_screen.c
    src/presentation/lvgl/screens/modules_screen.c
    src/presentation/lvgl/screens/wifi_screen.c
    src/presentation/lvgl/screens/bluetooth_screen.c
    src/presentation/lvgl/overlays/control_center.c
    src/presentation/lvgl/overlays/module_notice.c
    src/presentation/lvgl/overlays/calendar_notice.c
    src/presentation/lvgl/ambient_adapter.c
)
target_include_directories(binbot_presentation
    PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include"
    PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src/presentation/lvgl"
)
target_link_libraries(binbot_presentation PUBLIC
    binbot_core
    binbot_ambient_display
    binbot_audio_ipc
    binbot_agent_ipc
    binbot_vision
    lvgl_vendor
    binbot_platform
    ZLIB::ZLIB
)

add_executable(binbot_app src/main.c "${PROJECT_SOURCE_DIR}/services/binbot_dialog/src/camera_publish.c")
target_include_directories(binbot_app PRIVATE "${PROJECT_SOURCE_DIR}/services/binbot_dialog/src")
target_include_directories(binbot_app PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(binbot_app PRIVATE
    binbot_core
    binbot_component_client
    binbot_audio_ipc
    binbot_slave_ipc
    binbot_platform
    binbot_presentation
    m
    Threads::Threads
)

foreach(target binbot_core binbot_platform binbot_presentation binbot_app)
    target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic)
endforeach()

add_executable(binbot_ui_benchmark EXCLUDE_FROM_ALL tests/benchmark_control_center.c)
target_include_directories(binbot_ui_benchmark PRIVATE src/presentation/lvgl)
target_link_libraries(binbot_ui_benchmark PRIVATE binbot_presentation m Threads::Threads)
target_compile_options(binbot_ui_benchmark PRIVATE -Wall -Wextra -Wpedantic)

add_executable(binbot_companion_benchmark EXCLUDE_FROM_ALL tests/benchmark_companion_pages.c)
target_include_directories(binbot_companion_benchmark PRIVATE src/presentation/lvgl)
target_link_libraries(binbot_companion_benchmark PRIVATE binbot_presentation m Threads::Threads)
target_compile_options(binbot_companion_benchmark PRIVATE -Wall -Wextra -Wpedantic -UNDEBUG)

if(NOT CMAKE_CROSSCOMPILING)
    add_executable(binbot_generate_standby_previews EXCLUDE_FROM_ALL "${PROJECT_SOURCE_DIR}/tools/generate_standby_previews.c")
    target_include_directories(binbot_generate_standby_previews PRIVATE src/presentation/lvgl)
    target_link_libraries(binbot_generate_standby_previews PRIVATE binbot_presentation m Threads::Threads)
    target_compile_options(binbot_generate_standby_previews PRIVATE -Wall -Wextra -Wpedantic)
    add_custom_target(generate_standby_previews
        COMMAND $<TARGET_FILE:binbot_generate_standby_previews>
                "${PROJECT_SOURCE_DIR}/assets"
                "${PROJECT_SOURCE_DIR}/assets/standby/previews"
        DEPENDS binbot_generate_standby_previews
        COMMENT "Rendering fixed 152x152 RGB565 standby previews on the build host"
        VERBATIM)
endif()

if(BUILD_TESTING AND NOT CMAKE_CROSSCOMPILING)
    add_executable(test_app_vocabulary_audio tests/test_app_vocabulary_audio.c)
    target_include_directories(test_app_vocabulary_audio PRIVATE src/presentation/lvgl)
    target_link_libraries(test_app_vocabulary_audio PRIVATE binbot_presentation m Threads::Threads)
    target_compile_options(test_app_vocabulary_audio PRIVATE -Wall -Wextra -Wpedantic -UNDEBUG)
    add_test(NAME app_vocabulary_audio COMMAND test_app_vocabulary_audio)
    set_tests_properties(app_vocabulary_audio PROPERTIES ENVIRONMENT "BINBOT_ASSET_ROOT=${PROJECT_SOURCE_DIR}/assets;BINBOT_GESTURE_CAPTURE=0")
    add_executable(test_app_workspace tests/test_app_workspace.c)
    target_include_directories(test_app_workspace PRIVATE src/presentation/lvgl)
    target_link_libraries(test_app_workspace PRIVATE binbot_presentation m Threads::Threads)
    target_compile_options(test_app_workspace PRIVATE -Wall -Wextra -Wpedantic -UNDEBUG)
    add_test(NAME app_workspace COMMAND test_app_workspace)
    set_tests_properties(app_workspace PROPERTIES ENVIRONMENT "BINBOT_ASSET_ROOT=${PROJECT_SOURCE_DIR}/assets")
    add_executable(test_evdev_buffered_touch tests/test_evdev_buffered_touch.c)
    target_link_libraries(test_evdev_buffered_touch PRIVATE lvgl_vendor)
    target_compile_options(test_evdev_buffered_touch PRIVATE -Wall -Wextra -Wpedantic -UNDEBUG)
    add_test(NAME evdev_buffered_touch COMMAND test_evdev_buffered_touch)
    add_executable(test_app_resources tests/test_app_resources.c)
    target_include_directories(test_app_resources PRIVATE src/presentation/lvgl)
    target_link_libraries(test_app_resources PRIVATE binbot_presentation m Threads::Threads)
    target_compile_options(test_app_resources PRIVATE -Wall -Wextra -Wpedantic -UNDEBUG)
    add_test(NAME app_resources COMMAND test_app_resources)
    set_tests_properties(app_resources PROPERTIES ENVIRONMENT "BINBOT_ASSET_ROOT=${PROJECT_SOURCE_DIR}/assets;BINBOT_GESTURE_CAPTURE=0")
    add_executable(test_app_gestures tests/test_app_gestures.c)
    target_include_directories(test_app_gestures PRIVATE src/presentation/lvgl)
    target_link_libraries(test_app_gestures PRIVATE binbot_presentation m Threads::Threads)
    target_link_options(test_app_gestures PRIVATE -Wl,--wrap=clock_gettime)
    target_compile_options(test_app_gestures PRIVATE -Wall -Wextra -Wpedantic -UNDEBUG)
    add_test(NAME app_gestures COMMAND test_app_gestures)
    set_tests_properties(app_gestures PROPERTIES ENVIRONMENT "BINBOT_ASSET_ROOT=${PROJECT_SOURCE_DIR}/assets;BINBOT_GESTURE_CAPTURE=0")
    add_executable(test_app_diagnostics tests/test_app_diagnostics.c)
    target_link_libraries(test_app_diagnostics PRIVATE binbot_core)
    target_compile_options(test_app_diagnostics PRIVATE -Wall -Wextra -Wpedantic -UNDEBUG)
    add_test(NAME app_diagnostics COMMAND test_app_diagnostics)
    add_executable(test_app_diagnostics_ui tests/test_app_diagnostics_ui.c)
    target_include_directories(test_app_diagnostics_ui PRIVATE src/presentation/lvgl)
    target_link_libraries(test_app_diagnostics_ui PRIVATE binbot_presentation m Threads::Threads)
    target_compile_options(test_app_diagnostics_ui PRIVATE -Wall -Wextra -Wpedantic -UNDEBUG)
    add_test(NAME app_diagnostics_ui COMMAND test_app_diagnostics_ui)
    set_tests_properties(app_diagnostics_ui PROPERTIES ENVIRONMENT "BINBOT_ASSET_ROOT=${PROJECT_SOURCE_DIR}/assets")
    add_executable(test_app_noise tests/test_app_noise.c)
    target_include_directories(test_app_noise PRIVATE src/presentation/lvgl)
    target_link_libraries(test_app_noise PRIVATE binbot_presentation m Threads::Threads)
    target_compile_options(test_app_noise PRIVATE -Wall -Wextra -Wpedantic -UNDEBUG)
    add_test(NAME app_noise COMMAND test_app_noise)
    set_tests_properties(app_noise PROPERTIES ENVIRONMENT "BINBOT_ASSET_ROOT=${PROJECT_SOURCE_DIR}/assets")
    add_executable(test_app_calendar tests/test_app_calendar.c)
    target_include_directories(test_app_calendar PRIVATE src/presentation/lvgl)
    target_link_libraries(test_app_calendar PRIVATE binbot_presentation m Threads::Threads)
    target_link_options(test_app_calendar PRIVATE -Wl,--wrap=time)
    target_compile_options(test_app_calendar PRIVATE -Wall -Wextra -Wpedantic -UNDEBUG)
    add_test(NAME app_calendar COMMAND test_app_calendar)
    set_tests_properties(app_calendar PROPERTIES ENVIRONMENT "BINBOT_ASSET_ROOT=${PROJECT_SOURCE_DIR}/assets;TZ=CST-8")
    add_executable(test_camera_renderer tests/test_camera_renderer.c)
    target_link_libraries(test_camera_renderer PRIVATE binbot_platform)
    target_compile_options(test_camera_renderer PRIVATE -Wall -Wextra -Wpedantic -UNDEBUG)
    add_test(NAME camera_renderer COMMAND test_camera_renderer)
    add_executable(test_app_camera tests/test_app_camera.c)
    target_include_directories(test_app_camera PRIVATE src/presentation/lvgl)
    target_link_libraries(test_app_camera PRIVATE binbot_presentation m Threads::Threads)
    target_link_options(test_app_camera PRIVATE
        -Wl,--wrap=open -Wl,--wrap=ioctl -Wl,--wrap=mmap -Wl,--wrap=munmap -Wl,--wrap=madvise -Wl,--wrap=poll
        -Wl,--wrap=binbot_hand_detector_create -Wl,--wrap=binbot_hand_detector_destroy
        -Wl,--wrap=binbot_hand_detector_is_available -Wl,--wrap=binbot_hand_detector_submit_nv12
        -Wl,--wrap=binbot_hand_detector_latest)
    target_compile_options(test_app_camera PRIVATE -Wall -Wextra -Wpedantic -UNDEBUG)
    add_test(NAME app_camera COMMAND test_app_camera)
    set_tests_properties(app_camera PROPERTIES ENVIRONMENT "BINBOT_ASSET_ROOT=${PROJECT_SOURCE_DIR}/assets")
    add_executable(test_app_screensaver tests/test_app_screensaver.c)
    target_include_directories(test_app_screensaver PRIVATE src/presentation/lvgl)
    target_link_libraries(test_app_screensaver PRIVATE binbot_presentation m Threads::Threads)
    target_link_options(test_app_screensaver PRIVATE -Wl,--wrap=time)
    target_compile_options(test_app_screensaver PRIVATE -Wall -Wextra -Wpedantic -UNDEBUG)
    add_test(NAME app_screensaver COMMAND test_app_screensaver)
    set_tests_properties(app_screensaver PROPERTIES ENVIRONMENT "BINBOT_ASSET_ROOT=${PROJECT_SOURCE_DIR}/assets;TZ=Asia/Shanghai")
    add_executable(test_app_assistant tests/test_app_assistant.c)
    target_include_directories(test_app_assistant PRIVATE src/presentation/lvgl)
    target_link_libraries(test_app_assistant PRIVATE binbot_presentation m Threads::Threads)
    target_compile_options(test_app_assistant PRIVATE -Wall -Wextra -Wpedantic -UNDEBUG)
    add_test(NAME app_assistant COMMAND test_app_assistant)
    set_tests_properties(app_assistant PROPERTIES ENVIRONMENT "BINBOT_ASSET_ROOT=${PROJECT_SOURCE_DIR}/assets")
    add_executable(test_app_bluetooth tests/test_app_bluetooth.c)
    target_include_directories(test_app_bluetooth PRIVATE src/presentation/lvgl)
    target_link_libraries(test_app_bluetooth PRIVATE binbot_presentation m Threads::Threads)
    target_compile_options(test_app_bluetooth PRIVATE -Wall -Wextra -Wpedantic -UNDEBUG)
    add_test(NAME app_bluetooth COMMAND test_app_bluetooth)
    set_tests_properties(app_bluetooth PROPERTIES ENVIRONMENT "BINBOT_ASSET_ROOT=${PROJECT_SOURCE_DIR}/assets")
    if(BINBOT_HOST_SIMULATOR)
        add_executable(test_app_timezone tests/test_app_timezone.c)
        target_include_directories(test_app_timezone PRIVATE src/presentation/lvgl)
        target_link_libraries(test_app_timezone PRIVATE binbot_presentation m Threads::Threads)
        target_link_options(test_app_timezone PRIVATE -Wl,--wrap=time)
        target_compile_options(test_app_timezone PRIVATE -Wall -Wextra -Wpedantic -UNDEBUG)
        add_test(NAME app_timezone COMMAND test_app_timezone)
        add_test(NAME app_timezone_unset COMMAND test_app_timezone unset)
        set_tests_properties(app_timezone app_timezone_unset PROPERTIES
            ENVIRONMENT "BINBOT_ASSET_ROOT=${PROJECT_SOURCE_DIR}/assets;SDL_VIDEODRIVER=dummy;TZ=EST5EDT")
    endif()
    add_executable(test_app_module_notice tests/test_app_module_notice.c)
    target_include_directories(test_app_module_notice PRIVATE src/presentation/lvgl)
    target_link_libraries(test_app_module_notice PRIVATE binbot_presentation m Threads::Threads)
    target_compile_options(test_app_module_notice PRIVATE -Wall -Wextra -Wpedantic -UNDEBUG)
    add_test(NAME app_module_notice COMMAND test_app_module_notice)
    set_tests_properties(app_module_notice PROPERTIES ENVIRONMENT "BINBOT_ASSET_ROOT=${PROJECT_SOURCE_DIR}/assets")
    add_executable(test_app_calendar_notice tests/test_app_calendar_notice.c)
    target_include_directories(test_app_calendar_notice PRIVATE src/presentation/lvgl)
    target_link_libraries(test_app_calendar_notice PRIVATE binbot_presentation m Threads::Threads)
    target_link_options(test_app_calendar_notice PRIVATE -Wl,--wrap=time)
    target_compile_options(test_app_calendar_notice PRIVATE -Wall -Wextra -Wpedantic -UNDEBUG)
    add_test(NAME app_calendar_notice COMMAND test_app_calendar_notice)
    set_tests_properties(app_calendar_notice PROPERTIES ENVIRONMENT "BINBOT_ASSET_ROOT=${PROJECT_SOURCE_DIR}/assets;TZ=Asia/Shanghai")
    add_executable(test_app_devices tests/test_app_devices.c)
    target_include_directories(test_app_devices PRIVATE src/presentation/lvgl)
    target_link_libraries(test_app_devices PRIVATE binbot_presentation m Threads::Threads)
    target_compile_options(test_app_devices PRIVATE -Wall -Wextra -Wpedantic -UNDEBUG)
    add_test(NAME app_devices COMMAND test_app_devices)
    set_tests_properties(app_devices PROPERTIES ENVIRONMENT "BINBOT_ASSET_ROOT=${PROJECT_SOURCE_DIR}/assets")
    add_executable(test_app_about tests/test_app_about.c)
    target_include_directories(test_app_about PRIVATE src/presentation/lvgl)
    target_link_libraries(test_app_about PRIVATE binbot_presentation m Threads::Threads)
    target_compile_options(test_app_about PRIVATE -Wall -Wextra -Wpedantic -UNDEBUG)
    add_test(NAME app_about COMMAND test_app_about)
    set_tests_properties(app_about PROPERTIES ENVIRONMENT "BINBOT_ASSET_ROOT=${PROJECT_SOURCE_DIR}/assets")
    add_executable(test_app_state tests/test_app_state.c)
    target_link_libraries(test_app_state PRIVATE binbot_core)
    target_compile_options(test_app_state PRIVATE -Wall -Wextra -Wpedantic)
    add_test(NAME app_state COMMAND test_app_state)
    add_executable(test_app_random tests/test_app_random.c)
    target_link_libraries(test_app_random PRIVATE binbot_core)
    target_compile_options(test_app_random PRIVATE -Wall -Wextra -Wpedantic -UNDEBUG)
    add_test(NAME app_random COMMAND test_app_random)
    add_executable(test_app_modules tests/test_app_modules.c)
    target_link_libraries(test_app_modules PRIVATE binbot_core)
    target_compile_options(test_app_modules PRIVATE -Wall -Wextra -Wpedantic -UNDEBUG)
    add_test(NAME app_modules COMMAND test_app_modules)
    add_executable(test_app_motor_ui tests/test_app_motor_ui.c)
    target_include_directories(test_app_motor_ui PRIVATE src/presentation/lvgl)
    target_link_libraries(test_app_motor_ui PRIVATE binbot_presentation m Threads::Threads)
    target_compile_options(test_app_motor_ui PRIVATE -Wall -Wextra -Wpedantic -UNDEBUG)
    add_test(NAME app_motor_ui COMMAND test_app_motor_ui)
    set_tests_properties(app_motor_ui PROPERTIES ENVIRONMENT "BINBOT_ASSET_ROOT=${PROJECT_SOURCE_DIR}/assets")
    add_executable(test_app_pen_ui tests/test_app_pen_ui.c)
    target_include_directories(test_app_pen_ui PRIVATE src/presentation/lvgl)
    target_link_libraries(test_app_pen_ui PRIVATE binbot_presentation m Threads::Threads)
    target_compile_options(test_app_pen_ui PRIVATE -Wall -Wextra -Wpedantic -UNDEBUG)
    add_test(NAME app_pen_ui COMMAND test_app_pen_ui)
    set_tests_properties(app_pen_ui PROPERTIES ENVIRONMENT "BINBOT_ASSET_ROOT=${PROJECT_SOURCE_DIR}/assets")
    add_executable(test_app_home_weather tests/test_app_home_weather.c)
    target_include_directories(test_app_home_weather PRIVATE src/presentation/lvgl)
    target_link_libraries(test_app_home_weather PRIVATE binbot_presentation m Threads::Threads)
    target_link_options(test_app_home_weather PRIVATE -Wl,--wrap=time)
    target_compile_options(test_app_home_weather PRIVATE -Wall -Wextra -Wpedantic -UNDEBUG)
    add_test(NAME app_home_weather COMMAND test_app_home_weather)
    set_tests_properties(app_home_weather PROPERTIES ENVIRONMENT "BINBOT_ASSET_ROOT=${PROJECT_SOURCE_DIR}/assets")
    add_executable(test_app_light_ui tests/test_app_light_ui.c)
    target_include_directories(test_app_light_ui PRIVATE src/presentation/lvgl)
    target_link_libraries(test_app_light_ui PRIVATE binbot_presentation m Threads::Threads)
    target_compile_options(test_app_light_ui PRIVATE -Wall -Wextra -Wpedantic -UNDEBUG)
    add_test(NAME app_light_ui COMMAND test_app_light_ui)
    set_tests_properties(app_light_ui PROPERTIES ENVIRONMENT "BINBOT_ASSET_ROOT=${PROJECT_SOURCE_DIR}/assets")
    add_executable(test_lvgl_image_memory tests/test_lvgl_image_memory.c)
    target_link_libraries(test_lvgl_image_memory PRIVATE lvgl_vendor m)
    target_compile_options(test_lvgl_image_memory PRIVATE -Wall -Wextra -Wpedantic)
    add_test(NAME lvgl_image_memory COMMAND test_lvgl_image_memory)
endif()
