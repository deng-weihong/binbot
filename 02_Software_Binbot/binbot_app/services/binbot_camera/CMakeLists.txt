set(BINBOT_RK_MEDIA_ROOT
    "${RV1106_SDK_ROOT}/output/out/media_out"
    CACHE PATH "RV1106 media SDK output directory"
)

foreach(_artifact
        "${BINBOT_RK_MEDIA_ROOT}/include/rk_mpi_sys.h"
        "${BINBOT_RK_MEDIA_ROOT}/include/rtsp_demo.h"
        "${BINBOT_RK_MEDIA_ROOT}/lib/librockit.so"
        "${BINBOT_RK_MEDIA_ROOT}/lib/librtsp.a")
    if(NOT EXISTS "${_artifact}")
        message(FATAL_ERROR "Required RK media artifact is missing: ${_artifact}")
    endif()
endforeach()

find_package(PkgConfig REQUIRED)
pkg_check_modules(JPEG REQUIRED IMPORTED_TARGET libjpeg)

add_executable(binbot_camera src/main.c)
set_target_properties(binbot_camera PROPERTIES SKIP_BUILD_RPATH TRUE)
target_include_directories(binbot_camera PRIVATE "${BINBOT_RK_MEDIA_ROOT}/include")
target_link_directories(binbot_camera PRIVATE "${BINBOT_RK_MEDIA_ROOT}/lib")
target_link_libraries(binbot_camera PRIVATE
    rtsp
    rockit
    rockchip_mpp
    rga
    Threads::Threads
    PkgConfig::JPEG
    stdc++
    m
)
target_link_options(binbot_camera PRIVATE
    "-Wl,-rpath-link,${BINBOT_RK_MEDIA_ROOT}/lib"
    "-Wl,--gc-sections"
    "-Wl,--as-needed"
)
target_compile_options(binbot_camera PRIVATE -Wall -Wextra -Wpedantic)
