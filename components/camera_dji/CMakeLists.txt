idf_component_register(
    SRCS "dji_driver.c" "dji_duml.c" "dji_crc.c" "dji_enums.c"
    INCLUDE_DIRS "include"
    REQUIRES camera common
)
