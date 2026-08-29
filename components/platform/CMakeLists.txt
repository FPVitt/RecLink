idf_component_register(
    SRCS "platform.c" "led.c" "button.c" "twdt.c" "rtcnv.c"
    INCLUDE_DIRS "include"
    REQUIRES common driver esp_driver_gpio esp_driver_rmt esp_task_wdt esp_hw_support
    PRIV_REQUIRES esp_timer
)
