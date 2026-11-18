idf_component_register(SRCS "main.c"
    PRIV_REQUIRES app_bt_core app_btclassic_audio app_cfg app_ui_display app_ui_touch
        app_usb_audio app_usb_core app_usb_hid app_log app_ui_core esp_iris
        esp_mosaico_app_recovery esp_mosaico_gsp_bundle esp_mosaico_gsp_iris
        nvs_flash log)
