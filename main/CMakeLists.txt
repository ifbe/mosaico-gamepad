idf_component_register(SRCS "main.c"
    PRIV_REQUIRES app_bt_core app_sensor app_btclassic_audio app_cfg app_ui_core
        app_usb_audio app_usb_core app_usb_hid app_log esp_iris
        esp_mosaico_app_recovery esp_mosaico_gsp_iris
        nvs_flash log)
