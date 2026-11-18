// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* HID classes the application can expose. The bitmap is persisted in NVS and
 * read at boot: it decides which interfaces the USB/BLE descriptors carry. */
#define APP_CLASS_GAMEPAD (1u << 0)
#define APP_CLASS_MOUSE (1u << 1)
#define APP_CLASS_KEYBOARD (1u << 2)
/* Generic classes for USB and Bluetooth. */
#define APP_CLASS_ALL (APP_CLASS_GAMEPAD | APP_CLASS_MOUSE | APP_CLASS_KEYBOARD)
/* Extra USB-only identities: one Sony-compatible gamepad interface, added on
 * top of whichever generic classes are enabled. They are mutually exclusive. */
#define APP_CLASS_DS4 (1u << 3)
#define APP_CLASS_DS5 (1u << 4)
#define APP_CLASS_USB_ALL (APP_CLASS_ALL | APP_CLASS_DS4 | APP_CLASS_DS5)

/** Bluetooth transports; persisted and applied when the controller starts. */
typedef enum {
    APP_BT_OFF = 0,
    APP_BT_BLE,
    APP_BT_CLASSIC,
    APP_BT_BOTH,
} app_bt_mode_t;

/** Sony-compatible interface selected by the usb_hid mask, if any. */
typedef enum {
    APP_USB_HID_GENERIC = 0, /* no Sony interface */
    APP_USB_HID_DS4,
    APP_USB_HID_DS5,
} app_usb_hid_mode_t;

/** Sony-compatible interface implied by APP_CLASS_DS4/DS5 in the mask. When
 * both bits are set the configuration is rejected and generic is returned. */
app_usb_hid_mode_t app_cfg_usb_screen_mode(void);

/** Stable lowercase name for @p mode. */
const char *app_cfg_usb_hid_mode_name(app_usb_hid_mode_t mode);

/** USB vendor/product IDs to present: an explicit NVS override wins, otherwise
 * the identity follows the Sony bit (Espressif IDs when no Sony bit is set). */
void app_cfg_usb_ids(uint16_t *vid, uint16_t *pid);

/** Classic audio functions. */
#define APP_BT_AUDIO_SPEAKER (1u << 0)
#define APP_BT_AUDIO_MIC (1u << 1)
#define APP_BT_AUDIO_ALL (APP_BT_AUDIO_SPEAKER | APP_BT_AUDIO_MIC)

/** Load the persisted class bitmap and register the Iris RPC service.
 * Call once early in app_main, before the USB HID interfaces are registered. */
esp_err_t app_cfg_init(void);

/** Register the configuration RPC handlers. Call only after ESP-Iris has
 * started (its RPC registry is created with the service). */
esp_err_t app_cfg_register_rpc(void);

/** Enabled class bitmap for USB (default: gamepad only). */
uint32_t app_cfg_classes(void);

/** True when the UAC2 sound card should be registered on USB (default true). */
bool app_cfg_usb_audio(void);

/** Enabled class bitmap for the Bluetooth HID transports (default all). */
uint32_t app_cfg_bt_hid(void);

/** Bluetooth transport selection (default APP_BT_BOTH). */
app_bt_mode_t app_cfg_bt_mode(void);

/** Classic audio functions (default speaker + microphone). */
uint32_t app_cfg_bt_audio(void);

/** Stable lowercase name for @p mode. */
const char *app_cfg_bt_mode_name(app_bt_mode_t mode);

/** True when @p class_mask is enabled. */
bool app_cfg_has(uint32_t class_mask);

/** Persist a new USB HID mask (listener: the settings page). */
esp_err_t app_cfg_set_usb_hid(uint32_t mask);

/** Persist the Bluetooth HID mask. */
esp_err_t app_cfg_set_bt_hid(uint32_t mask);

/** Persist the USB audio switch. */
esp_err_t app_cfg_set_usb_audio(bool enabled);

/** Persist the Bluetooth audio mask. */
esp_err_t app_cfg_set_bt_audio(uint32_t mask);

/** RPC service id used for the configuration commands. */
#define APP_CFG_RPC_SERVICE 0x4700

#ifdef __cplusplus
}
#endif
