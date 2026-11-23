idf_component_register(
    SRCS "app_main.c" "board_io.c" "network.c" "protocol.c"
         "tcp_server.c" "ota_update.c" "firmware_image.c"
    INCLUDE_DIRS "."
    REQUIRES console esp_driver_gpio esp_app_format esp_system esp_timer freertos
             esp_wifi esp_netif esp_event nvs_flash lwip esp_http_client app_update
             bootloader_support http_parser esp_hw_support espressif__cjson
)

# Requests are flat objects; bound parser recursion on the embedded task stack.
idf_component_get_property(cjson_library espressif__cjson COMPONENT_LIB)
target_compile_definitions(${cjson_library} PRIVATE CJSON_NESTING_LIMIT=8)
