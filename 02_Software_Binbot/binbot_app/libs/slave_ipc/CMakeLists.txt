add_library(binbot_slave_ipc STATIC src/client.c)

target_include_directories(binbot_slave_ipc
    PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include"
)

target_link_libraries(binbot_slave_ipc PUBLIC binbot_slave_protocol)
target_compile_options(binbot_slave_ipc PRIVATE -Wall -Wextra -Wpedantic)
