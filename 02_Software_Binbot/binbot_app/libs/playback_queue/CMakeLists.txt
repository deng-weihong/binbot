add_library(binbot_playback_queue STATIC src/playback_queue.c)
target_include_directories(binbot_playback_queue
    PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include"
)
target_link_libraries(binbot_playback_queue PUBLIC binbot_audio_ipc)
target_compile_options(binbot_playback_queue PRIVATE -Wall -Wextra -Wpedantic)

if(BUILD_TESTING AND NOT CMAKE_CROSSCOMPILING)
    add_executable(test_playback_queue tests/test_playback_queue.c)
    target_link_libraries(test_playback_queue PRIVATE binbot_playback_queue)
    target_compile_options(test_playback_queue PRIVATE -Wall -Wextra -Wpedantic)
    add_test(NAME playback_queue COMMAND test_playback_queue)
endif()
