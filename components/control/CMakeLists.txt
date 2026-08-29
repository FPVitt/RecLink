idf_component_register(
    SRCS "ctrl_task.c" "ctrl_record.c" "ctrl_preset.c" "ctrl_clock.c"
    INCLUDE_DIRS "include"
    REQUIRES common cfg msp camera osd platform freertos
)
