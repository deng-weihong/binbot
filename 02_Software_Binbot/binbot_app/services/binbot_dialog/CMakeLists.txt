add_subdirectory("${PROJECT_SOURCE_DIR}/third_party/libfvad" "${CMAKE_CURRENT_BINARY_DIR}/fvad")

add_library(binbot_dialog_core STATIC
    src/bbvp.c
    src/retry_policy.c
    src/camera_publish.c
    src/vad.c
)
target_include_directories(binbot_dialog_core PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_link_libraries(binbot_dialog_core PUBLIC binbot_fvad m)
target_compile_options(binbot_dialog_core PRIVATE -Wall -Wextra -Wpedantic)

if(BUILD_TESTING AND NOT CMAKE_CROSSCOMPILING)
    set(BINBOT_VOICE_CONTRACTS_DIR "${PROJECT_SOURCE_DIR}/../contracts/voice/v1"
        CACHE PATH "BBVP v1 contracts directory")
    add_executable(test_dialog_core tests/test_dialog_core.c)
    target_link_libraries(test_dialog_core PRIVATE binbot_dialog_core)
    target_compile_definitions(test_dialog_core PRIVATE
        BBVP_VECTOR_PATH="${BINBOT_VOICE_CONTRACTS_DIR}/vectors/uplink-audio-frame.hex"
        VAD_SPEECH_PATH="${CMAKE_CURRENT_SOURCE_DIR}/tests/data/quiet-speech-16k.pcm"
    )
    target_compile_options(test_dialog_core PRIVATE -Wall -Wextra -Wpedantic)
    add_test(NAME dialog_core COMMAND test_dialog_core)
endif()

if(BINBOT_BUILD_DIALOG)
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(OPUS REQUIRED IMPORTED_TARGET opus)
    pkg_check_modules(LIBWEBSOCKETS REQUIRED IMPORTED_TARGET libwebsockets)
    pkg_check_modules(CJSON REQUIRED IMPORTED_TARGET libcjson)
    add_executable(binbot_dialog src/main.c)
    target_link_libraries(binbot_dialog PRIVATE
        binbot_agent_ipc
        binbot_audio_ipc
        binbot_dialog_core
        PkgConfig::OPUS
        PkgConfig::LIBWEBSOCKETS
        PkgConfig::CJSON
        Threads::Threads
    )
    target_compile_options(binbot_dialog PRIVATE -Wall -Wextra -Wpedantic)

    add_executable(test_dialog_text EXCLUDE_FROM_ALL tests/test_dialog_text.c)
    target_link_libraries(test_dialog_text PRIVATE
        binbot_agent_ipc binbot_audio_ipc binbot_dialog_core
        PkgConfig::OPUS PkgConfig::LIBWEBSOCKETS PkgConfig::CJSON Threads::Threads)
    target_compile_options(test_dialog_text PRIVATE -Wall -Wextra -Wpedantic -UNDEBUG)
    if(BUILD_TESTING AND NOT CMAKE_CROSSCOMPILING)
        add_test(NAME dialog_text COMMAND test_dialog_text)
    endif()
endif()
