add_executable(binbot_slave src/main.c)
target_link_libraries(binbot_slave PRIVATE binbot_slave_protocol binbot_slave_ipc)
target_compile_options(binbot_slave PRIVATE -Wall -Wextra -Wpedantic)
