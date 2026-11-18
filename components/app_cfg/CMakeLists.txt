idf_component_register(SRCS "app_cfg.c"
    INCLUDE_DIRS "include"
    REQUIRES app_ble_hid app_ui_core esp_iris nvs_flash log)
