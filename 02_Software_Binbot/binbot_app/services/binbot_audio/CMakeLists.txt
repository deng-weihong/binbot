find_package(ALSA REQUIRED)

add_subdirectory("${PROJECT_SOURCE_DIR}/third_party/speexdsp" "${CMAKE_CURRENT_BINARY_DIR}/speexdsp")
add_library(binbot_audio_echo STATIC src/echo.c)
target_include_directories(binbot_audio_echo PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_link_libraries(binbot_audio_echo PUBLIC binbot_speexdsp binbot_audio_ipc Threads::Threads)
target_compile_options(binbot_audio_echo PRIVATE -Wall -Wextra -Wpedantic)
if(CMAKE_CROSSCOMPILING)
    set(BINBOT_RKAUDIO_ALGORITHMS_ROOT
        "${RV1106_SDK_ROOT}/media/common_algorithm/common_algorithm/audio/rkaudio_algorithms"
        CACHE PATH "Rockchip microphone processing SDK")
    if(NOT EXISTS "${BINBOT_RKAUDIO_ALGORITHMS_ROOT}/include/rkaudio_preprocess.h" OR
       NOT EXISTS "${BINBOT_RKAUDIO_ALGORITHMS_ROOT}/uclibc/libaec_bf_process.a" OR
       NOT EXISTS "${BINBOT_RKAUDIO_ALGORITHMS_ROOT}/uclibc/librkaudio_common.a")
        message(FATAL_ERROR
            "RV1106 AEC SDK not found under ${BINBOT_RKAUDIO_ALGORITHMS_ROOT}")
    endif()
    target_include_directories(binbot_audio_echo SYSTEM PRIVATE "${BINBOT_RKAUDIO_ALGORITHMS_ROOT}/include")
    target_compile_definitions(binbot_audio_echo PRIVATE BINBOT_HAVE_RKAUDIO_AEC=1)
    target_link_libraries(binbot_audio_echo PUBLIC
        "${BINBOT_RKAUDIO_ALGORITHMS_ROOT}/uclibc/libaec_bf_process.a"
        "${BINBOT_RKAUDIO_ALGORITHMS_ROOT}/uclibc/librkaudio_common.a")
endif()

add_executable(binbot_audio
    src/service.c
    src/playback.c
    src/noise.c
)
target_include_directories(binbot_audio PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
    ${ALSA_INCLUDE_DIRS}
)
target_link_libraries(binbot_audio PRIVATE
    binbot_audio_ipc
    binbot_playback_queue
    binbot_audio_echo
    ${ALSA_LIBRARIES}
    m
    Threads::Threads
)

if(CMAKE_CROSSCOMPILING)
    set(BINBOT_RV1106_MEDIA_ROOT
        "${RV1106_SDK_ROOT}/output/out/media_out"
        CACHE PATH "RV1106 media common algorithm output")
    if(NOT EXISTS "${BINBOT_RV1106_MEDIA_ROOT}/include/mp3dec.h" OR
       NOT EXISTS "${BINBOT_RV1106_MEDIA_ROOT}/lib/librkaudio.a")
        message(FATAL_ERROR
            "RV1106 MP3 decoder not found under ${BINBOT_RV1106_MEDIA_ROOT}")
    endif()
    target_include_directories(binbot_audio PRIVATE
        "${BINBOT_RV1106_MEDIA_ROOT}/include")
    target_link_libraries(binbot_audio PRIVATE
        "${BINBOT_RV1106_MEDIA_ROOT}/lib/librkaudio.a")
    target_compile_definitions(binbot_audio PRIVATE BINBOT_HAVE_RKAUDIO_MP3=1)
endif()

add_executable(test_audio_echo EXCLUDE_FROM_ALL tests/test_audio_echo.c)
target_link_libraries(test_audio_echo PRIVATE binbot_audio_echo)
target_compile_options(test_audio_echo PRIVATE -Wall -Wextra -Wpedantic -UNDEBUG)
if(BUILD_TESTING AND NOT CMAKE_CROSSCOMPILING)
    add_test(NAME audio_echo COMMAND test_audio_echo)
endif()
add_executable(audio_echo_probe EXCLUDE_FROM_ALL "${PROJECT_SOURCE_DIR}/tools/audio_echo_probe.c")
target_link_libraries(audio_echo_probe PRIVATE binbot_audio_ipc m)
target_compile_options(audio_echo_probe PRIVATE -Wall -Wextra -Wpedantic)
target_compile_options(binbot_audio PRIVATE -Wall -Wextra -Wpedantic)

add_executable(test_audio_dialog EXCLUDE_FROM_ALL tests/test_audio_dialog.c src/playback.c src/noise.c)
get_target_property(_audio_includes binbot_audio INCLUDE_DIRECTORIES)
get_target_property(_audio_libraries binbot_audio LINK_LIBRARIES)
target_include_directories(test_audio_dialog PRIVATE ${_audio_includes})
target_link_libraries(test_audio_dialog PRIVATE ${_audio_libraries})
if(CMAKE_CROSSCOMPILING)
    target_compile_definitions(test_audio_dialog PRIVATE BINBOT_HAVE_RKAUDIO_MP3=1)
endif()
target_compile_options(test_audio_dialog PRIVATE -Wall -Wextra -Wpedantic -UNDEBUG)
if(BUILD_TESTING AND NOT CMAKE_CROSSCOMPILING)
    add_test(NAME audio_dialog COMMAND test_audio_dialog)
    add_executable(test_audio_noise tests/test_audio_noise.c src/playback.c src/noise.c)
    target_include_directories(test_audio_noise PRIVATE ${_audio_includes})
    target_link_libraries(test_audio_noise PRIVATE ${_audio_libraries})
    target_compile_options(test_audio_noise PRIVATE -Wall -Wextra -Wpedantic -UNDEBUG)
    add_test(NAME audio_noise COMMAND test_audio_noise)
endif()
