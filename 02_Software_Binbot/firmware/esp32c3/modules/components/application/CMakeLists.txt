set(BINBOT_SDKCONFIG_FILE "${SDKCONFIG}")
if(NOT EXISTS "${BINBOT_SDKCONFIG_FILE}")
    get_filename_component(BINBOT_BUILD_DIR "${CMAKE_BINARY_DIR}" NAME)
    if(BINBOT_BUILD_DIR STREQUAL "esp-idf")
        get_filename_component(BINBOT_BUILD_ROOT "${CMAKE_BINARY_DIR}"
                               DIRECTORY)
        get_filename_component(BINBOT_BUILD_VARIANT "${BINBOT_BUILD_ROOT}"
                               NAME)
    else()
        set(BINBOT_BUILD_VARIANT "${BINBOT_BUILD_DIR}")
    endif()
    set(BINBOT_SDKCONFIG_FILE
        "${CMAKE_SOURCE_DIR}/sdkconfig.${BINBOT_BUILD_VARIANT}")
endif()

set(BINBOT_APP_IS_SHOWCASE OFF)
set(BINBOT_APP_IS_LIGHT OFF)
if(EXISTS "${BINBOT_SDKCONFIG_FILE}")
    file(STRINGS "${BINBOT_SDKCONFIG_FILE}" BINBOT_SDKCONFIG_LINES)
    foreach(BINBOT_CONFIG_LINE IN LISTS BINBOT_SDKCONFIG_LINES)
        if(BINBOT_CONFIG_LINE STREQUAL
           "CONFIG_BINBOT_APP_SHOWCASE_TURNTABLE=y")
            set(BINBOT_APP_IS_SHOWCASE ON)
        elseif(BINBOT_CONFIG_LINE STREQUAL "CONFIG_BINBOT_APP_AMBIENT_LIGHT=y")
            set(BINBOT_APP_IS_LIGHT ON)
        endif()
    endforeach()
endif()

set(APP_REQUIRES freertos service_uart service_ota)
if(BINBOT_APP_IS_LIGHT)
    list(APPEND APP_REQUIRES driver_light)
elseif(BINBOT_APP_IS_SHOWCASE)
    list(APPEND APP_REQUIRES driver_motor)
else()
    list(APPEND APP_REQUIRES driver_display service_graphics esp_timer
         service_storage service_gif service_ota service_uart)
endif()

idf_component_register(
    SRCS "app_round_screen.c" "app_holographic_screen.c"
         "app_showcase_turntable.c" "app_ambient_light.c"
    INCLUDE_DIRS "include"
    REQUIRES ${APP_REQUIRES}
)

if(NOT BINBOT_APP_IS_SHOWCASE AND NOT BINBOT_APP_IS_LIGHT)
    include($ENV{IDF_PATH}/components/fatfs/project_include.cmake)
    # The runtime has one stable display slot. GIF playback reads this file
    # directly; uploaded assets replace it atomically at runtime.
    set(BINBOT_STORAGE_ASSETS "${CMAKE_BINARY_DIR}/storage_assets")
    file(MAKE_DIRECTORY "${BINBOT_STORAGE_ASSETS}")
    configure_file("${CMAKE_SOURCE_DIR}/assets/display.gif"
                   "${BINBOT_STORAGE_ASSETS}/display.gif" COPYONLY)
    # The device mounts this partition through esp_vfs_fat_spiflash_mount_rw_wl.
    # Generate the matching Wear Levelling metadata for a writable filesystem.
    fatfs_create_spiflash_image(storage "${BINBOT_STORAGE_ASSETS}"
                                FLASH_IN_PROJECT)
endif()
