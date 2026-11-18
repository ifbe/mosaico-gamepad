idf_component_register(SRCS "app_btclassic_hid.c"
    INCLUDE_DIRS "include"
    REQUIRES app_bt_core bt esp_hid esp_event log)
