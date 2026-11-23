idf_component_register(
    SRCS "turntable_motor.c"
    INCLUDE_DIRS "include"
    REQUIRES board esp_driver_gpio esp_driver_ledc freertos
    PRIV_REQUIRES binbot_component_protocol
)
