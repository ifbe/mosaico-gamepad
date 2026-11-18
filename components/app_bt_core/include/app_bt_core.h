// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <stdbool.h>

#include "app_cfg.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Start the Bluetooth controller and Bluedroid with the transport selected by
 * the persisted configuration (off / ble / classic / both).
 *
 * This is the shared layer: the BLE HID, Classic HID and Classic audio
 * components all attach to it and must be started after this call. When the
 * configuration disables Bluetooth this returns ESP_OK with app_bt_core_ready()
 * false. */
esp_err_t app_bt_core_start(void);

/** True when controller and host are running. */
bool app_bt_core_ready(void);

/** True when the configured transport includes BLE. */
bool app_bt_core_wants_ble(void);

/** True when the configured transport includes Classic (BR/EDR). */
bool app_bt_core_wants_classic(void);

/** Build the Bluetooth HID report map (from the USB core's descriptors, filtered
 * by the persisted bt_hid mask), start the enabled transports and attach them
 * as the report sink of app_usb_core. Call after app_bt_core_start(). */
esp_err_t app_bt_core_hid_start(void);

/** True while any Bluetooth HID transport has a host connected. */
bool app_bt_core_hid_connected(void);

/** Forward one input report to every connected Bluetooth transport. */
void app_bt_core_hid_send(uint8_t report_id, const uint8_t *data, size_t len);

/** LE advertising name (also the classic device name's counterpart). */
const char *app_bt_core_le_name(void);

/** Classic (BR/EDR) device name. */
const char *app_bt_core_classic_name(void);

#ifdef __cplusplus
}
#endif
