idf_component_register(SRCS "app_ui_touch.c" "app_input.c"
    INCLUDE_DIRS "include"
    REQUIRES esp-mosaico-bsp
    PRIV_REQUIRES esp_lcd_touch esp_driver_gpio log)
