// SPDX-License-Identifier: Apache-2.0
#include "app_cfg.h"
#include "app_log.h"
#include "app_sensor.h"
#include "app_bt_core.h"
#include "app_btclassic_audio.h"
#include "app_btclassic_hid.h"
#include "app_features.h"
#include "app_ui.h"
#include "app_usb_audio.h"
#include "app_usb_core.h"
#include "app_usb_hid.h"
#include "esp_heap_caps.h"
#include "esp_iris.h"
#include "esp_iris_usb_hid.h"
#include "esp_log.h"
#include "iris_ota_support.h"
#include "iris_screen_mirror.h"
#include "nvs_flash.h"

static const char *TAG = "gamepad";

static void failed_alloc_cb(size_t size, uint32_t caps, const char *function_name)
{
    esp_rom_printf("ALLOC FAIL %u B caps=0x%x from %s\n", (unsigned)size,
                   (unsigned)caps, function_name != NULL ? function_name : "?");
}

static void log_internal_heap(const char *stage)
{
    ESP_LOGI(TAG, "%s: internal heap free %u B (largest block %u B)", stage,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
}

void app_main(void)
{
    app_log_init();
    /* GSP reports a present-timing warning on almost every frame. With the
     * UART console gone those lines still cost formatting CPU that the
     * isochronous audio tasks need, and they are not actionable here. */
    esp_log_level_set("present_te", ESP_LOG_NONE);
    /* "SCO xmit Q overflow" repeats per packet and evicts every other log line
     * while a call is up; it is counted by the bt_classic status instead. */
    esp_log_level_set("BT_BTM", ESP_LOG_ERROR);
    /* Report the exact request when any heap_caps allocation fails; the display
     * pipeline dies with a bare NO_MEM otherwise. */
    heap_caps_register_failed_alloc_callback(failed_alloc_cb);
    /* HID connection transitions are only logged at debug level; they are what
     * decides whether reports are routed to the host. */
    esp_log_level_set("BLE_HIDD", ESP_LOG_DEBUG);
    esp_log_level_set("BT_HIDD", ESP_LOG_DEBUG);
    ESP_LOGI(TAG, "boot: nvs");
    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_ERROR_CHECK(app_cfg_init());
    ESP_LOGI(TAG, "boot: cfg+hid");
    log_internal_heap("after config");

    /* The USB descriptor is built at install time, so register before Iris:
     * the audio function first, then the HID interfaces. */
    const uint8_t first_ep = app_usb_core_first_in_endpoint();
    unsigned hid_count = app_usb_core_hid_interface_count(app_cfg_classes());
    if (app_cfg_usb_screen_mode() != APP_USB_HID_GENERIC) {
        ++hid_count; /* the optional Sony-compatible interface takes one more */
    }
    if ((app_cfg_classes() & APP_CLASS_SENSOR) != 0) {
        ++hid_count; /* the motion interface takes one more */
    }
    const uint8_t mic_ep = app_usb_core_mic_endpoint(first_ep, hid_count);

    /* Optional USB identity override (for example a Sony-compatible gamepad).
     * Must run before ESP-Iris installs its USB driver. */
    uint16_t usb_vid = 0, usb_pid = 0;
    app_cfg_usb_ids(&usb_vid, &usb_pid);
    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_iris_usb_set_ids(usb_vid, usb_pid));
    bool usb_audio_ok = false;
#if APP_FEATURE_USB_AUDIO
    if (app_cfg_usb_audio()) {
        const esp_err_t audio_err = app_usb_audio_register(mic_ep);
        usb_audio_ok = audio_err == ESP_OK;
        if (!usb_audio_ok) {
            ESP_LOGE(TAG, "USB audio unavailable: %s", esp_err_to_name(audio_err));
        }
    }
#endif
#if APP_FEATURE_USB_HID
    ESP_ERROR_CHECK(app_usb_hid_register_usb(first_ep));
#endif
    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_iris_boot_probe());
    /* Recovery must remain reachable even when the UI bundle cannot be loaded. */
    iris_ota_support_start();
    ESP_LOGI(TAG, "boot: iris started");
    ESP_ERROR_CHECK(app_cfg_register_rpc());

#if APP_FEATURE_USB_AUDIO
    /* Codec, ring buffers and the USB audio tasks start with the device. */
    if (usb_audio_ok) {
        const esp_err_t start_err = app_usb_audio_start();
        if (start_err != ESP_OK) {
            ESP_LOGE(TAG, "audio path unavailable: %s", esp_err_to_name(start_err));
        }
        log_internal_heap("after audio");
    }
#endif

    /* The UI owns the panel, the scene and the input device. It is optional for
     * diagnostics: if it fails the device must still boot so the failure can be
     * inspected over RPC. */
    ESP_ERROR_CHECK(iris_screen_mirror_init());
    ESP_LOGI(TAG, "boot: ui");
    const esp_err_t ui_err = app_ui_start();
    if (ui_err != ESP_OK) {
        ESP_LOGE(TAG, "UI unavailable (0x%x); headless boot", ui_err);
        log_internal_heap("after ui failure");
    } else {
        ESP_ERROR_CHECK(iris_screen_mirror_attach(app_ui_handle()));
    }
    log_internal_heap("after ui");
    ESP_LOGI(TAG, "boot: hid");
#if APP_FEATURE_USB_HID || APP_FEATURE_ANY_BT_HID
    ESP_ERROR_CHECK(app_usb_hid_start());
#endif
#if APP_FEATURE_ANY_BT
    /* Bluetooth is an optional function: a failure here must not take the
     * gamepad down. The error is logged and the rest of the app keeps running. */
    const esp_err_t bt_err = app_bt_core_start();
    if (bt_err != ESP_OK) {
        ESP_LOGE(TAG, "Bluetooth unavailable: %s", esp_err_to_name(bt_err));
    } else {
        const esp_err_t hid_err = app_bt_core_hid_start();
        if (hid_err != ESP_OK) {
            ESP_LOGE(TAG, "Bluetooth HID unavailable: %s", esp_err_to_name(hid_err));
        }
#if APP_FEATURE_BT_CLASSIC_AUDIO
        if (app_bt_core_wants_classic()) {
            const esp_err_t err = app_btclassic_audio_start();
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "Classic audio unavailable: %s", esp_err_to_name(err));
            }
        }
#endif
    }
#endif
    log_internal_heap("after bluetooth");
    /* The IMU feeds the motion interface and the Sony reports; a board without
     * one only loses those, so failure stays non-fatal. */
    const esp_err_t sensor_err = app_sensor_start();
    if (sensor_err != ESP_OK) {
        ESP_LOGW(TAG, "IMU unavailable: %s", esp_err_to_name(sensor_err));
    }
    ESP_LOGI(TAG, "boot: complete");
    /* GSP retains the callback context; the UI, HID bridge and timers live
     * until reboot. Teardown would have to quiesce GSP before closing the UI. */
    ESP_LOGI(TAG, "gamepad UI ready");
}
