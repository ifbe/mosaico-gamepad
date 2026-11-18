idf_component_register(SRCS "app_ui_touch.c"
    INCLUDE_DIRS "include"
    REQUIRES app_usb_hid app_ui_core app_cfg esp-mosaico-bsp esp_driver_gpio esp-gsp esp_lcd_touch)
