idf_component_register(
    SRCS "uplink_main.c"
    INCLUDE_DIRS "."
    REQUIRES
        esp_http_server
        esp_timer
        esp_wifi
        nvs_flash
        esp_driver_uart
)
