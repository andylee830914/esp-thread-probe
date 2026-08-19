idf_component_register(SRCS "probe_main.c"
                       INCLUDE_DIRS "."
                       REQUIRES esp_coex probe_core probe_matter)
