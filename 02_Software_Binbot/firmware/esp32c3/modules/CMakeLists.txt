cmake_minimum_required(VERSION 3.22)

set(CCACHE_ENABLE OFF CACHE BOOL "Disable ccache for reproducible builds" FORCE)

# Component CMake files are evaluated before all CONFIG_* values are exposed.
# Read the selected sdkconfig here so profile-specific dependencies are exact.
set(BINBOT_SDKCONFIG_FILE "${SDKCONFIG}")
get_filename_component(BINBOT_BUILD_VARIANT "${CMAKE_BINARY_DIR}" NAME)
if((NOT BINBOT_SDKCONFIG_FILE OR NOT EXISTS "${BINBOT_SDKCONFIG_FILE}")
   AND EXISTS "${CMAKE_SOURCE_DIR}/sdkconfig.${BINBOT_BUILD_VARIANT}")
    set(BINBOT_SDKCONFIG_FILE
        "${CMAKE_SOURCE_DIR}/sdkconfig.${BINBOT_BUILD_VARIANT}")
endif()
if(NOT BINBOT_SDKCONFIG_FILE OR NOT EXISTS "${BINBOT_SDKCONFIG_FILE}")
    set(BINBOT_SDKCONFIG_FILE "${CMAKE_SOURCE_DIR}/sdkconfig")
endif()

set(BINBOT_APP_IS_SHOWCASE OFF CACHE INTERNAL "Binbot showcase profile" FORCE)
set(BINBOT_APP_IS_LIGHT OFF CACHE INTERNAL "Binbot ambient light profile" FORCE)
set(BINBOT_DISPLAY_IS_GC9A01A OFF CACHE INTERNAL "Binbot GC9A01A profile" FORCE)
set(BINBOT_DISPLAY_IS_ILI9342C OFF CACHE INTERNAL "Binbot ILI9342C profile" FORCE)

if(EXISTS "${BINBOT_SDKCONFIG_FILE}")
    file(STRINGS "${BINBOT_SDKCONFIG_FILE}" BINBOT_SDKCONFIG_LINES)
    foreach(BINBOT_CONFIG_LINE IN LISTS BINBOT_SDKCONFIG_LINES)
        if(BINBOT_CONFIG_LINE STREQUAL "CONFIG_BINBOT_APP_SHOWCASE_TURNTABLE=y")
            set(BINBOT_APP_IS_SHOWCASE ON CACHE INTERNAL "Binbot showcase profile" FORCE)
        elseif(BINBOT_CONFIG_LINE STREQUAL "CONFIG_BINBOT_APP_AMBIENT_LIGHT=y")
            set(BINBOT_APP_IS_LIGHT ON CACHE INTERNAL "Binbot ambient light profile" FORCE)
        elseif(BINBOT_CONFIG_LINE STREQUAL "CONFIG_BINBOT_DISPLAY_GC9A01A=y")
            set(BINBOT_DISPLAY_IS_GC9A01A ON CACHE INTERNAL "Binbot GC9A01A profile" FORCE)
        elseif(BINBOT_CONFIG_LINE STREQUAL "CONFIG_BINBOT_DISPLAY_ILI9342C=y")
            set(BINBOT_DISPLAY_IS_ILI9342C ON CACHE INTERNAL "Binbot ILI9342C profile" FORCE)
        endif()
    endforeach()
endif()

set(BINBOT_COMPONENT_PROTOCOL_DIR
    "${CMAKE_CURRENT_SOURCE_DIR}/../../../contracts/uart/binbot_component_protocol"
    CACHE PATH "BinBot component protocol library source directory"
)
if(NOT EXISTS "${BINBOT_COMPONENT_PROTOCOL_DIR}/esp-idf/binbot_component_protocol/CMakeLists.txt")
    message(FATAL_ERROR
        "Component protocol library not found. Set BINBOT_COMPONENT_PROTOCOL_DIR to binbot_component_protocol."
    )
endif()
list(APPEND EXTRA_COMPONENT_DIRS
    "${BINBOT_COMPONENT_PROTOCOL_DIR}/esp-idf/binbot_component_protocol"
)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
idf_build_set_property(MINIMAL_BUILD ON)
if(BINBOT_APP_IS_LIGHT)
    project(binbot_ambient_light)
elseif(BINBOT_APP_IS_SHOWCASE)
    project(binbot_showcase_turntable)
elseif(BINBOT_DISPLAY_IS_ILI9342C)
    project(binbot_holographic_screen)
else()
    project(binbot_round_screen)
endif()
