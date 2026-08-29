idf_component_register(
    SRCS "cam_state.c" "cam_registry.c" "cam_task.c" "ble_central.c"
    INCLUDE_DIRS "include"
    REQUIRES common cfg platform bt freertos nvs_flash
)
