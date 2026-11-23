set(PROTOCOL_DIR "${CMAKE_CURRENT_LIST_DIR}/../..")
idf_component_register(
    SRCS "${PROTOCOL_DIR}/src/protocol.c" "${PROTOCOL_DIR}/src/ota_receiver.c"
          "${PROTOCOL_DIR}/src/file_receiver.c" "${PROTOCOL_DIR}/src/light.c"
          "${PROTOCOL_DIR}/src/pen.c" "${PROTOCOL_DIR}/src/pen_render.c"
    INCLUDE_DIRS "${PROTOCOL_DIR}/include"
)
target_compile_features(${COMPONENT_LIB} PUBLIC c_std_11)
target_compile_options(${COMPONENT_LIB} PRIVATE -Wall -Wextra -Wpedantic)
