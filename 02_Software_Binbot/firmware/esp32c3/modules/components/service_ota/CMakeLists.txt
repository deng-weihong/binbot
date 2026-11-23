if(CONFIG_BINBOT_OTA_ENABLE)
    idf_component_register(
        SRCS "ota_service.c"
        INCLUDE_DIRS "include"
        REQUIRES app_update esp_https_ota esp_http_client esp_wifi esp_netif
                 esp_event nvs_flash lwip esp_timer service_storage
    )
else()
    idf_component_register(SRCS "ota_service_local.c" INCLUDE_DIRS "include"
                           REQUIRES app_update)
endif()
