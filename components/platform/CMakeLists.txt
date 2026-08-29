idf_component_register(
    SRCS "platform.c" "led.c" "button.c" "twdt.c" "rtcnv.c"
    INCLUDE_DIRS "include"
    REQUIRES common driver esp_driver_gpio esp_driver_rmt
             esp_hw_support esp_system freertos
    PRIV_REQUIRES esp_timer led_strip
)
