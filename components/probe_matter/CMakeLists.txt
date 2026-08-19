idf_component_register(
    SRCS "src/probe_matter.cpp"
    INCLUDE_DIRS "include"
    REQUIRES esp_matter openthread led_strip esp_driver_gpio probe_core
)

set_property(TARGET ${COMPONENT_LIB} PROPERTY CXX_STANDARD 20)
