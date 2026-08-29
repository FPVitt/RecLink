idf_component_register(
    SRCS "portal_task.c" "http_api.c" "captive_dns.c" "ota.c" "json_cfg.c"
    INCLUDE_DIRS "include"
    REQUIRES common cfg msp camera control osd platform
             esp_wifi esp_http_server esp_netif app_update json lwip freertos
             esp_hw_support esp_system esp_app_format esp_timer
)

# Portal assets: gzip at build time, embed into the app image (spec 10.3:
# "gzip-precompressed", "< 40 KB total transferred"). Raw sources are checked in;
# the .gz artefacts are build outputs.
set(WWW "${CMAKE_CURRENT_SOURCE_DIR}/www")
set(GZS "")
foreach(f index.html app.js style.css)
    set(out "${CMAKE_CURRENT_BINARY_DIR}/${f}.gz")
    add_custom_command(
        OUTPUT "${out}"
        COMMAND gzip -9 -n -c "${WWW}/${f}" > "${out}"
        DEPENDS "${WWW}/${f}"
        COMMENT "gzip ${f}"
        VERBATIM)
    list(APPEND GZS "${out}")
endforeach()
add_custom_target(portal_assets DEPENDS ${GZS})
add_dependencies(${COMPONENT_LIB} portal_assets)

target_add_binary_data(${COMPONENT_LIB} "${CMAKE_CURRENT_BINARY_DIR}/index.html.gz" BINARY)
target_add_binary_data(${COMPONENT_LIB} "${CMAKE_CURRENT_BINARY_DIR}/app.js.gz" BINARY)
target_add_binary_data(${COMPONENT_LIB} "${CMAKE_CURRENT_BINARY_DIR}/style.css.gz" BINARY)
