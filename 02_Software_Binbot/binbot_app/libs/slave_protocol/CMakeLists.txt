add_library(binbot_slave_protocol STATIC src/protocol.c)

target_include_directories(binbot_slave_protocol
    PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include"
)

target_compile_options(binbot_slave_protocol PRIVATE -Wall -Wextra -Wpedantic)

if(BUILD_TESTING AND NOT CMAKE_CROSSCOMPILING)
    add_executable(test_slave_protocol tests/test_protocol.c)
    target_link_libraries(test_slave_protocol PRIVATE binbot_slave_protocol)
    target_compile_options(test_slave_protocol PRIVATE -Wall -Wextra -Wpedantic)
    add_test(NAME slave_protocol COMMAND test_slave_protocol)
endif()
