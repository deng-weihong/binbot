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

set(BINBOT_DISPLAY_IS_GC9A01A OFF)
set(BINBOT_DISPLAY_IS_ILI9342C OFF)
if(EXISTS "${BINBOT_SDKCONFIG_FILE}")
    file(STRINGS "${BINBOT_SDKCONFIG_FILE}" BINBOT_SDKCONFIG_LINES)
    foreach(BINBOT_CONFIG_LINE IN LISTS BINBOT_SDKCONFIG_LINES)
        if(BINBOT_CONFIG_LINE STREQUAL
           "CONFIG_BINBOT_DISPLAY_GC9A01A=y")
            set(BINBOT_DISPLAY_IS_GC9A01A ON)
        elseif(BINBOT_CONFIG_LINE STREQUAL
               "CONFIG_BINBOT_DISPLAY_ILI9342C=y")
            set(BINBOT_DISPLAY_IS_ILI9342C ON)
        endif()
    endforeach()
endif()

set(DISPLAY_SRCS "display.c")
if(BINBOT_DISPLAY_IS_GC9A01A)
    list(APPEND DISPLAY_SRCS "gc9a01.c")
elseif(BINBOT_DISPLAY_IS_ILI9342C)
    list(APPEND DISPLAY_SRCS "ili9342c.c")
endif()

idf_component_register(
    SRCS ${DISPLAY_SRCS}
    INCLUDE_DIRS "include"
    REQUIRES board esp_lcd esp_driver_gpio esp_driver_spi freertos heap
)
