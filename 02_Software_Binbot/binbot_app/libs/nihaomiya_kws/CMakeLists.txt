set(BINBOT_MIYA_NATIVE_DIR "" CACHE PATH "Source directory for the optional Miya backend")
if(NOT TARGET miya_reference_native)
    if(NOT EXISTS "${BINBOT_MIYA_NATIVE_DIR}/CMakeLists.txt")
        message(FATAL_ERROR "Set BINBOT_MIYA_NATIVE_DIR to a Miya backend source directory")
    endif()
    set(MIYA_BUILD_TOOLS OFF CACHE BOOL "Build standalone Miya tools" FORCE)
    add_subdirectory("${BINBOT_MIYA_NATIVE_DIR}" "${CMAKE_CURRENT_BINARY_DIR}/miya-native")
endif()
add_library(binbot_nihaomiya_kws STATIC src/detector.cpp)
target_link_libraries(binbot_nihaomiya_kws PUBLIC miya_reference_native)

target_include_directories(binbot_nihaomiya_kws
    PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include"
)

target_compile_options(binbot_nihaomiya_kws PRIVATE -Wall -Wextra -Wpedantic)
