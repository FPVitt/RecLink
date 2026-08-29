idf_component_register(
    SRCS "crc.c" "applog.c" "monoclock.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_timer freertos log
)
