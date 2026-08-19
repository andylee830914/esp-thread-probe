idf_component_register(
    SRCS
        "src/probe_bridge.c"
        "src/probe_json.c"
        "src/probe_runtime.c"
        "src/probe_thread.c"
    INCLUDE_DIRS "include"
    REQUIRES
        esp_timer
        esp_hw_support
        esp_driver_uart
        openthread
)
