idf_component_register(
    SRCS "gopro_driver.c" "gopro_tlv.c" "gopro_enums.c" "gopro_presets.c"
    INCLUDE_DIRS "include"
    REQUIRES camera common bt esp_hw_support
)
