idf_component_register(SRCS "app_usb_hid.c"
    INCLUDE_DIRS "include"
    REQUIRES app_cfg app_sensor esp_iris esp_tinyusb esp_driver_gpio)
