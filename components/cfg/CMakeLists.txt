idf_component_register(
    SRCS "cfg.c" "cfg_defaults.c" "cfg_migrate.c" "cfg_stats.c"
    INCLUDE_DIRS "include"
    REQUIRES common nvs_flash
)
