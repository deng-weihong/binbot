set(BINBOT_UART_SHOWCASE OFF)
set(BINBOT_UART_LIGHT OFF)
set(BINBOT_UART_SDKCONFIG "${SDKCONFIG}")
if(NOT EXISTS "${BINBOT_UART_SDKCONFIG}")
    get_filename_component(BINBOT_UART_VARIANT "${CMAKE_BINARY_DIR}" NAME)
    set(BINBOT_UART_SDKCONFIG "${CMAKE_SOURCE_DIR}/sdkconfig.${BINBOT_UART_VARIANT}")
endif()
if(EXISTS "${BINBOT_UART_SDKCONFIG}")
    file(STRINGS "${BINBOT_UART_SDKCONFIG}" BINBOT_UART_LINES)
    foreach(BINBOT_UART_LINE IN LISTS BINBOT_UART_LINES)
        if(BINBOT_UART_LINE STREQUAL "CONFIG_BINBOT_APP_SHOWCASE_TURNTABLE=y")
            set(BINBOT_UART_SHOWCASE ON)
        elseif(BINBOT_UART_LINE STREQUAL "CONFIG_BINBOT_APP_AMBIENT_LIGHT=y")
            set(BINBOT_UART_LIGHT ON)
        endif()
    endforeach()
endif()
set(BINBOT_UART_REQUIRES board esp_driver_uart freertos app_update esp_timer esp_system
                         esp_app_format esp_hw_support mbedtls service_ota nvs_flash)
if(BINBOT_UART_SHOWCASE)
    list(APPEND BINBOT_UART_REQUIRES driver_motor)
elseif(BINBOT_UART_LIGHT)
    list(APPEND BINBOT_UART_REQUIRES driver_light)
elseif(NOT BINBOT_UART_LIGHT)
    list(APPEND BINBOT_UART_REQUIRES service_storage)
endif()

idf_component_register(
    SRCS "component_uart.c"
    INCLUDE_DIRS "include"
    REQUIRES ${BINBOT_UART_REQUIRES} binbot_component_protocol
)
