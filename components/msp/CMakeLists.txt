idf_component_register(
    SRCS "msp_codec.c" "msp_parse.c" "msp_sched.c" "msp_task.c" "fc_state.c"
    INCLUDE_DIRS "include"
    REQUIRES common cfg platform driver esp_driver_uart freertos
)
