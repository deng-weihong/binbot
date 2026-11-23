set(NCNN_SOURCE_DIR "${CMAKE_CURRENT_LIST_DIR}/../../build/kws-cpu/ncnn-src"
    CACHE PATH "ncnn 20250503 checkout (305837fd4a722ebc47c5d72e72d8ec9ae970e932)")
set(BINBOT_FBANK_ROOT "${PROJECT_SOURCE_DIR}/third_party/audio_fbank"
    CACHE PATH "Production C Fbank source directory")
if(NOT EXISTS "${NCNN_SOURCE_DIR}/CMakeLists.txt")
    message(FATAL_ERROR "Set NCNN_SOURCE_DIR to an ncnn 20250503 checkout")
endif()
if(EXISTS "${NCNN_SOURCE_DIR}/.git")
    execute_process(COMMAND git -C "${NCNN_SOURCE_DIR}" rev-parse HEAD
        OUTPUT_VARIABLE ncnn_revision OUTPUT_STRIP_TRAILING_WHITESPACE
        RESULT_VARIABLE ncnn_git_result)
    if(NOT ncnn_git_result EQUAL 0 OR
       NOT ncnn_revision STREQUAL "305837fd4a722ebc47c5d72e72d8ec9ae970e932")
        message(FATAL_ERROR "KWS requires the pinned ncnn 20250503 revision")
    endif()
endif()
foreach(option NCNN_SHARED_LIB NCNN_VULKAN NCNN_OPENMP NCNN_INT8 NCNN_BF16
        NCNN_PIXEL NCNN_BUILD_TOOLS NCNN_BUILD_EXAMPLES NCNN_BUILD_BENCHMARK
        NCNN_BUILD_TESTS NCNN_C_API NCNN_INSTALL_SDK)
    set(${option} OFF CACHE BOOL "FP32 CPU KWS" FORCE)
endforeach()
set(NCNN_VERSION 20250503)
# Only the exported DS-CNN layers and their internal helpers are needed.
file(GLOB layer_sources "${NCNN_SOURCE_DIR}/src/layer/*.cpp")
foreach(source IN LISTS layer_sources)
    get_filename_component(layer "${source}" NAME_WE)
    set(WITH_LAYER_${layer} OFF CACHE BOOL "" FORCE)
endforeach()
foreach(layer input convolution convolutiondepthwise pooling innerproduct
        padding packing reshape flatten relu)
    set(WITH_LAYER_${layer} ON CACHE BOOL "" FORCE)
endforeach()
add_subdirectory("${NCNN_SOURCE_DIR}" "${CMAKE_CURRENT_BINARY_DIR}/ncnn")

add_library(binbot_kws_fbank STATIC "${BINBOT_FBANK_ROOT}/src/fbank.c")
target_include_directories(binbot_kws_fbank PUBLIC "${BINBOT_FBANK_ROOT}/include")
target_link_libraries(binbot_kws_fbank PUBLIC m)
find_path(CJSON_INCLUDE_DIR cJSON.h PATH_SUFFIXES cjson)
find_library(CJSON_LIBRARY cjson)
if(NOT CJSON_INCLUDE_DIR OR NOT CJSON_LIBRARY)
    message(FATAL_ERROR "Hi Xiaowen KWS requires cJSON headers and library")
endif()
add_library(binbot_hixiaowen_kws STATIC src/model.cpp src/detector.cpp)
target_include_directories(binbot_hixiaowen_kws
    PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include"
    PRIVATE "${CJSON_INCLUDE_DIR}")
target_link_libraries(binbot_hixiaowen_kws PUBLIC ncnn binbot_kws_fbank
    PRIVATE "${CJSON_LIBRARY}")
target_compile_options(binbot_hixiaowen_kws PRIVATE -Wall -Wextra -Wpedantic)
