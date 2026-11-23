set(BINBOT_SNOWBOY_ROOT "" CACHE PATH
    "Snowboy source directory containing include/ and lib/rpi/")
set(BINBOT_SNOWBOY_LIBRARY "${BINBOT_SNOWBOY_ROOT}/lib/rpi/libsnowboy-detect.a" CACHE FILEPATH
    "Snowboy ARMv7 static library")
set(BINBOT_SNOWBOY_BLAS_ROOT "${BINBOT_SNOWBOY_ROOT}/examples/C/uclibc_libs/lib" CACHE PATH
    "Directory containing the ARM OpenBLAS/CBLAS archive used by Snowboy")
set(BINBOT_SNOWBOY_BLAS_LIBRARY "${BINBOT_SNOWBOY_BLAS_ROOT}/libopenblas_armv7-r0.3.21.a" CACHE FILEPATH
    "ARM OpenBLAS archive used by Snowboy")

if(NOT EXISTS "${BINBOT_SNOWBOY_ROOT}/include/snowboy-detect.h")
    message(FATAL_ERROR "Snowboy headers not found at ${BINBOT_SNOWBOY_ROOT}")
endif()
if(NOT EXISTS "${BINBOT_SNOWBOY_LIBRARY}")
    message(FATAL_ERROR "Snowboy ARM library not found at ${BINBOT_SNOWBOY_LIBRARY}")
endif()
if(NOT EXISTS "${BINBOT_SNOWBOY_BLAS_LIBRARY}")
    message(FATAL_ERROR "Snowboy CBLAS/OpenBLAS archive not found at ${BINBOT_SNOWBOY_BLAS_LIBRARY}")
endif()

add_library(binbot_snowboy_kws STATIC src/detector.cpp)
target_include_directories(binbot_snowboy_kws
    PRIVATE "${BINBOT_SNOWBOY_ROOT}/include"
    PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include"
)
# The shipped Snowboy archive was built with the pre-C++11 libstdc++ ABI.
target_compile_definitions(binbot_snowboy_kws PRIVATE _GLIBCXX_USE_CXX11_ABI=0)
target_compile_options(binbot_snowboy_kws PRIVATE -Wall -Wextra -Wpedantic)
target_link_libraries(binbot_snowboy_kws PUBLIC
    "${BINBOT_SNOWBOY_LIBRARY}"
    "${BINBOT_SNOWBOY_BLAS_LIBRARY}"
    dl m pthread
)
