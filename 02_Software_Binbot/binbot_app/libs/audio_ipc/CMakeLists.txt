add_library(binbot_audio_ipc STATIC src/client.c)

target_include_directories(binbot_audio_ipc
    PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include"
)

target_compile_options(binbot_audio_ipc PRIVATE -Wall -Wextra -Wpedantic)
