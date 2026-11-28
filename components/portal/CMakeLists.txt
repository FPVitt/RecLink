idf_component_register(
    SRCS "portal_task.c" "http_api.c" "captive_dns.c" "ota.c" "json_cfg.c"
    INCLUDE_DIRS "include"
    REQUIRES common cfg msp camera control osd platform rid
             esp_wifi esp_coex esp_http_server esp_netif app_update json lwip freertos
             esp_hw_support esp_system esp_app_format esp_timer
)

# Portal assets: comments dropped, then gzipped at build time and embedded into the app image
# (spec 10.3: "gzip-precompressed", budget 46,000 B transferred -- measured on these .gz
# outputs). Raw sources (with their comments) are checked in; the .gz artefacts are build
# outputs. www_pack.py: JS whole-line // comments and CSS /* */ blocks only.
set(WWW "${CMAKE_CURRENT_SOURCE_DIR}/www")
idf_build_get_property(python PYTHON)
set(GZS "")
foreach(f index.html app.js style.css)
    set(out "${CMAKE_CURRENT_BINARY_DIR}/${f}.gz")
    add_custom_command(
        OUTPUT "${out}"
        COMMAND ${python} "${CMAKE_CURRENT_SOURCE_DIR}/www_pack.py" "${WWW}/${f}" "${out}"
        DEPENDS "${WWW}/${f}" "${CMAKE_CURRENT_SOURCE_DIR}/www_pack.py"
        COMMENT "pack ${f}"
        VERBATIM)
    list(APPEND GZS "${out}")
endforeach()
add_custom_target(portal_assets DEPENDS ${GZS})
add_dependencies(${COMPONENT_LIB} portal_assets)

target_add_binary_data(${COMPONENT_LIB} "${CMAKE_CURRENT_BINARY_DIR}/index.html.gz" BINARY)
target_add_binary_data(${COMPONENT_LIB} "${CMAKE_CURRENT_BINARY_DIR}/app.js.gz" BINARY)
target_add_binary_data(${COMPONENT_LIB} "${CMAKE_CURRENT_BINARY_DIR}/style.css.gz" BINARY)
