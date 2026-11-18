idf_component_register(SRCS "app_bt_core.c"
    INCLUDE_DIRS "include"
    REQUIRES app_cfg app_usb_hid app_ble_hid app_btclassic_hid bt esp_hw_support log)
