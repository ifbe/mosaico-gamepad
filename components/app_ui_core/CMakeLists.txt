idf_component_register(SRCS "app_ui.c" "app_ui_input.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "private"
    REQUIRES app_cfg app_log app_sensor app_usb_hid app_ui_display app_ui_touch esp-gsp
             esp_display_present esp_mosaico_gsp_bundle)
include("${MOSAICO_UTILS_ROOT}/mosaico-tools/cmake/gsp_bundle.cmake")
mosaico_gsp_add_ui_bundle(${COMPONENT_LIB} "ui/main.json")
