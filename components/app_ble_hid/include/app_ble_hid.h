// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Start the BLE HID (HOGP) device and advertising.
 *
 * Requires app_bt_core_start() with a transport that includes BLE. @p report_map
 * must stay alive (the HID device borrows it). */
esp_err_t app_ble_hid_start(const uint8_t *report_map, size_t report_map_len);

/** True while a BLE host has subscribed to the HID input reports. */
bool app_ble_hid_connected(void);

/** Force advertising to start again (recovers a device that went silent). */
void app_ble_hid_restart_advertising(void);

/** Forget every bonded BLE peer (returns how many were removed). */
unsigned app_ble_hid_clear_bonds(void);

/** Queue one input report on the BLE HID device (ignored while disconnected). */
void app_ble_hid_send(uint8_t report_id, const uint8_t *data, size_t len);

#ifdef __cplusplus
}
#endif
