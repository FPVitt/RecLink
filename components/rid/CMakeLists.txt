# Remote ID receiver (F7, spec 8.8). opendroneid-core-c is vendored under opendroneid/ (Apache-2.0,
# pinned to 849c03adbe0cdd68997a462fafab397edcfe53f3 -- see opendroneid/LICENSE and
# THIRD_PARTY_NOTICES.md). Only its per-message decoders are used.
idf_component_register(
    SRCS "rid_task.c" "rid_match.c" "rid_table.c" "rid_geo.c" "rid_render.c" "rid_json.c"
         "opendroneid/opendroneid.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "opendroneid"
    REQUIRES common cfg msp platform freertos
)
# The vendored file is compiled as-is; drop its printf helpers (never used, never on target).
set_source_files_properties("opendroneid/opendroneid.c" PROPERTIES COMPILE_DEFINITIONS "ODID_DISABLE_PRINTF")
