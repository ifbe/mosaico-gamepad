idf_component_register(SRCS "app_usb_audio.c"
                            "uac1/app_usb_uac1.c"
                            "uac2/app_usb_uac2.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "private"
    PRIV_REQUIRES esp_tinyusb esp_iris app_usb_core app_audio_codec app_cfg)

# esp_tinyusb's Kconfig does not expose the TinyUSB audio class, so put our own
# tusb_config.h ahead of the component's copy for the tinyusb sources.
idf_component_get_property(_audio_tusb_lib espressif__tinyusb COMPONENT_LIB)
if(NOT _audio_tusb_lib)
    message(FATAL_ERROR "espressif__tinyusb component is missing")
endif()
target_include_directories(${_audio_tusb_lib} BEFORE PRIVATE
    "${CMAKE_CURRENT_LIST_DIR}/tusb_override")
target_include_directories(${COMPONENT_LIB} BEFORE PRIVATE
    "${CMAKE_CURRENT_LIST_DIR}/tusb_override")
