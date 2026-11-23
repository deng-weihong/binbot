idf_component_register(
    SRCS "ambient_light.c" "led_strip_encoder.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_driver_gpio esp_driver_rmt freertos esp_timer binbot_component_protocol
)
