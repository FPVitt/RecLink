idf_component_register(
    SRCS "app_main.c" "housekeeping.c"
    INCLUDE_DIRS "."
    REQUIRES common platform cfg msp camera control osd portal
             esp_littlefs nvs_flash esp_timer freertos
)
