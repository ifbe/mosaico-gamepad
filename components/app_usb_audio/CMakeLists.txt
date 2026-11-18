idf_component_register(SRCS "app_usb_audio.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_tinyusb esp_iris app_audio_codec)

# esp_tinyusb's Kconfig does not expose the TinyUSB audio class, so put our own
# tusb_config.h ahead of the component's copy for the tinyusb sources.
idf_component_get_property(_audio_tusb_lib espressif__tinyusb COMPONENT_LIB)
if(NOT _audio_tusb_lib)
    message(FATAL_ERROR "espressif__tinyusb component is missing")
endif()
target_include_directories(${_audio_tusb_lib} BEFORE PRIVATE
    "${CMAKE_CURRENT_LIST_DIR}/tusb_override")

# The application sources need the same enabled-class view as TinyUSB itself.
target_include_directories(${COMPONENT_LIB} BEFORE PRIVATE
    "${CMAKE_CURRENT_LIST_DIR}/tusb_override")
