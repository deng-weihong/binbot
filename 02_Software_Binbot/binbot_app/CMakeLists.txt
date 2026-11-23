cmake_minimum_required(VERSION 3.10)
project(binbot LANGUAGES C CXX)

include(CTest)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")

find_package(Threads REQUIRED)

set(RV1106_SDK_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../rv1106_deskscreen"
    CACHE PATH "Externally installed RV1106 SDK root")

set(_BINBOT_AUDIO_DEFAULT OFF)
if(CMAKE_CROSSCOMPILING)
    set(_BINBOT_AUDIO_DEFAULT ON)
endif()
option(BINBOT_BUILD_AUDIO "Build the RV1106 audio service" ${_BINBOT_AUDIO_DEFAULT})
option(BINBOT_BUILD_WAKE "Build keyword detection with an external Miya backend" OFF)
option(BINBOT_BUILD_DIALOG "Build the RV1106 dialog service" ${_BINBOT_AUDIO_DEFAULT})
option(BINBOT_BUILD_CONTROL "Build the independent MQTT control service (requires libmosquitto)" OFF)
option(BINBOT_BUILD_CAMERA "Build the RV1106 RK_MPI camera streaming service" ${_BINBOT_AUDIO_DEFAULT})
option(BINBOT_BUILD_AGENT "Build the RV1106 on-device multimodal agent" ${_BINBOT_AUDIO_DEFAULT})
option(BINBOT_BUILD_RKNN_PROBE "Build the RV1106 RKNN model probe" OFF)
option(BINBOT_HOST_SIMULATOR "Build the LVGL UI with an SDL2 desktop window" OFF)
if(BINBOT_HOST_SIMULATOR AND CMAKE_CROSSCOMPILING)
    message(FATAL_ERROR "BINBOT_HOST_SIMULATOR cannot be used with a cross compiler")
endif()
option(BINBOT_BUILD_SNOWBOY "Build the Snowboy wake adapter (ARM archive)" OFF)
option(BINBOT_BUILD_HIXIAOWEN "Build the Hi Xiaowen FP32 CPU wake adapter" OFF)

set(BINBOT_COMPONENT_PROTOCOL_DIR
    "${CMAKE_CURRENT_SOURCE_DIR}/../contracts/uart/binbot_component_protocol"
    CACHE PATH "BinBot component protocol library source directory"
)
if(NOT EXISTS "${BINBOT_COMPONENT_PROTOCOL_DIR}/CMakeLists.txt")
    message(FATAL_ERROR
        "Component protocol library not found. Set BINBOT_COMPONENT_PROTOCOL_DIR to binbot_component_protocol."
    )
endif()

add_subdirectory(libs/audio_ipc)
add_subdirectory(libs/control_ipc)
add_subdirectory(libs/agent_ipc)
add_subdirectory(libs/playback_queue)
add_subdirectory(libs/slave_protocol)
add_subdirectory("${BINBOT_COMPONENT_PROTOCOL_DIR}" "component_protocol")
add_subdirectory(libs/component_client)
add_subdirectory(libs/slave_ipc)
add_subdirectory(libs/vision)
if(BINBOT_BUILD_WAKE)
    add_subdirectory(libs/nihaomiya_kws)
endif()
if(BINBOT_BUILD_HIXIAOWEN)
    add_subdirectory(libs/hixiaowen_kws)
endif()
if(BINBOT_BUILD_SNOWBOY)
    if(NOT CMAKE_CROSSCOMPILING)
        message(FATAL_ERROR "BINBOT_BUILD_SNOWBOY requires an ARM cross build")
    endif()
    add_subdirectory(libs/snowboy_kws)
endif()
add_subdirectory(apps/binbot_app)
add_subdirectory(services/binbot_dialog)
if(BINBOT_BUILD_CONTROL)
    add_subdirectory(services/binbot_control)
endif()
add_subdirectory(services/binbot_slave)
add_subdirectory(services/binbot_component)

if(BINBOT_BUILD_AGENT)
    add_subdirectory(services/binbot_agent)
endif()

if(BINBOT_BUILD_CAMERA)
    if(NOT CMAKE_CROSSCOMPILING)
        message(FATAL_ERROR "BINBOT_BUILD_CAMERA requires an RV1106 cross build")
    endif()
    add_subdirectory(services/binbot_camera)
endif()

if(BINBOT_BUILD_RKNN_PROBE)
    if(NOT CMAKE_CROSSCOMPILING)
        message(FATAL_ERROR "BINBOT_BUILD_RKNN_PROBE requires an RV1106 cross build")
    endif()
    set(BINBOT_RKNN_SDK_ROOT
        "${RV1106_SDK_ROOT}/output/out/media_out"
        CACHE PATH "RV1106 RKNN SDK output directory"
    )
    add_executable(binbot_rknn_probe tools/rknn_model_probe.c)
    target_include_directories(binbot_rknn_probe PRIVATE
        "${RV1106_SDK_ROOT}/project/app/rk_smart_door/smart_door/common/face/algo"
    )
    target_link_directories(binbot_rknn_probe PRIVATE "${BINBOT_RKNN_SDK_ROOT}/lib")
    target_link_libraries(binbot_rknn_probe PRIVATE rknnmrt m)
    target_link_options(binbot_rknn_probe PRIVATE
        "-Wl,-rpath-link,${BINBOT_RKNN_SDK_ROOT}/lib"
    )
    target_compile_options(binbot_rknn_probe PRIVATE -Wall -Wextra -Wpedantic)
endif()

if(BINBOT_BUILD_AUDIO)
    add_subdirectory(services/binbot_audio)
endif()
if(BINBOT_BUILD_WAKE)
    add_subdirectory(services/binbot_wake)
endif()
