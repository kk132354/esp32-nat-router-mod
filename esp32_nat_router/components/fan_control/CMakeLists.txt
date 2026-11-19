idf_component_register(
    SRCS "fan_control.c"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES driver esp_driver_tsens nvs_flash esp_timer
)
