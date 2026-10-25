idf_component_register(
    SRCS "app_main.c" "dbg_ble.c" "evlog.c" "web.c" "wifi_ap.c"
    INCLUDE_DIRS "."
    REQUIRES bt nvs_flash esp_wifi esp_netif esp_event esp_http_server
             esp_timer app_update esp_app_format json lwip esp_driver_gpio esp_coex
)

# Web UI: gzip at build time, embed into the app image (as the original portal).
set(WWW "${CMAKE_CURRENT_SOURCE_DIR}/../www")
foreach(f index.html app.js ops.js catalog.js style.css)
    set(out "${CMAKE_CURRENT_BINARY_DIR}/${f}.gz")
    add_custom_command(
        OUTPUT "${out}"
        COMMAND gzip -9 -n -c "${WWW}/${f}" > "${out}"
        DEPENDS "${WWW}/${f}"
        COMMENT "gzip ${f}"
        VERBATIM)
    target_add_binary_data(${COMPONENT_LIB} "${out}" BINARY)
endforeach()
