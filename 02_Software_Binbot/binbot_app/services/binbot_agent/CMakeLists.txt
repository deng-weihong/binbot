find_package(PkgConfig REQUIRED)
pkg_check_modules(CJSON REQUIRED IMPORTED_TARGET libcjson)
pkg_check_modules(LIBWEBSOCKETS REQUIRED IMPORTED_TARGET libwebsockets)

add_executable(binbot_agent src/main.c)
target_link_libraries(binbot_agent PRIVATE
    binbot_agent_ipc
    PkgConfig::CJSON
    PkgConfig::LIBWEBSOCKETS
)
target_compile_options(binbot_agent PRIVATE -Wall -Wextra -Wpedantic)
