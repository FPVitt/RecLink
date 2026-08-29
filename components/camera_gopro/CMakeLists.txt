idf_component_register(
    SRCS "gopro_driver.c" "gopro_tlv.c" "gopro_enums.c"
    INCLUDE_DIRS "include"
    REQUIRES camera common bt esp_hw_support
)
