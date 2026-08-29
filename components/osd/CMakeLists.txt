idf_component_register(
    SRCS "osd_task.c" "osd_format.c"
    INCLUDE_DIRS "include"
    REQUIRES common cfg msp camera freertos
)
