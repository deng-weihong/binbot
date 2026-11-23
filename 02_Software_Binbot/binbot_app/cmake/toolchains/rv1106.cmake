set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR arm)

get_filename_component(BINBOT_PROJECT_ROOT "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)

set(RV1106_SDK_ROOT "${BINBOT_PROJECT_ROOT}/../rv1106_deskscreen"
    CACHE PATH "Externally installed RV1106 SDK root")
set(_DEFAULT_BUILDROOT_ROOT
    "${RV1106_SDK_ROOT}/sysdrv/source/buildroot/buildroot-2023.02.6"
)

if(DEFINED ENV{RV1106_TOOLCHAIN_ROOT} AND NOT "$ENV{RV1106_TOOLCHAIN_ROOT}" STREQUAL "")
    set(_TOOLCHAIN_ROOT "$ENV{RV1106_TOOLCHAIN_ROOT}")
else()
    set(_TOOLCHAIN_ROOT "")
endif()

if(DEFINED ENV{RV1106_BUILDROOT_ROOT} AND NOT "$ENV{RV1106_BUILDROOT_ROOT}" STREQUAL "")
    set(_BUILDROOT_ROOT "$ENV{RV1106_BUILDROOT_ROOT}")
else()
    set(_BUILDROOT_ROOT "${_DEFAULT_BUILDROOT_ROOT}")
endif()

set(RV1106_TOOLCHAIN_ROOT "${_TOOLCHAIN_ROOT}" CACHE PATH "RV1106 cross-toolchain root")
set(RV1106_BUILDROOT_ROOT "${_BUILDROOT_ROOT}" CACHE PATH "RV1106 Buildroot root")
set(TOOLCHAIN_PREFIX "arm-rockchip830-linux-uclibcgnueabihf")
if(NOT EXISTS "${RV1106_TOOLCHAIN_ROOT}/bin/${TOOLCHAIN_PREFIX}-gcc")
    message(FATAL_ERROR "Set RV1106_TOOLCHAIN_ROOT to your RV1106 cross-toolchain")
endif()

set(CMAKE_C_COMPILER
    "${RV1106_TOOLCHAIN_ROOT}/bin/${TOOLCHAIN_PREFIX}-gcc"
    CACHE FILEPATH "RV1106 C compiler"
)

set(CMAKE_CXX_COMPILER
    "${RV1106_TOOLCHAIN_ROOT}/bin/${TOOLCHAIN_PREFIX}-g++"
    CACHE FILEPATH "RV1106 C++ compiler"
)

set(CMAKE_SYSROOT
    "${RV1106_BUILDROOT_ROOT}/output/host/arm-buildroot-linux-uclibcgnueabihf/sysroot"
    CACHE PATH "RV1106 target sysroot"
)

# Buildroot's pkg-config is a host executable. Point it at the target metadata
# so CMake never resolves host Opus or libwebsockets libraries by accident.
set(ENV{PKG_CONFIG_SYSROOT_DIR} "${CMAKE_SYSROOT}")
set(ENV{PKG_CONFIG_LIBDIR}
    "${CMAKE_SYSROOT}/usr/lib/pkgconfig:${CMAKE_SYSROOT}/usr/share/pkgconfig"
)

set(PKG_CONFIG_EXECUTABLE
    "${RV1106_BUILDROOT_ROOT}/output/host/bin/pkg-config"
    CACHE FILEPATH "RV1106 target pkg-config"
)

set(CMAKE_FIND_ROOT_PATH "${CMAKE_SYSROOT}")
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)

set(CMAKE_C_FLAGS_INIT "-march=armv7-a -mtune=cortex-a7 -mfpu=neon-vfpv4 -mfloat-abi=hard")
set(CMAKE_CXX_FLAGS_INIT "-march=armv7-a -mtune=cortex-a7 -mfpu=neon-vfpv4 -mfloat-abi=hard")
set(CMAKE_C_FLAGS_RELEASE_INIT "-O2 -DNDEBUG")
set(CMAKE_CXX_FLAGS_RELEASE_INIT "-O2 -DNDEBUG")
