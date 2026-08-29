idf_component_register(
    SRCS "app_main.c" "housekeeping.c"
    INCLUDE_DIRS "."
    REQUIRES common platform cfg msp camera camera_gopro camera_dji
             control osd portal
             nvs_flash esp_event esp_netif esp_timer freertos
)
