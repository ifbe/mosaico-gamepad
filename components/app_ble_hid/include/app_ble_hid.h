// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "esp_gap_ble_api.h"

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

/** True once the GATT service is up. app_bt_core polls this before it starts
 * advertising, so a host never finds an advertisement without the service. */
bool app_ble_hid_service_ready(void);

/** GAP events forwarded by app_bt_core: it owns the single GAP callback because
 * advertising is transport-level, but pairing and connection parameters still
 * belong to this component. */
void app_ble_hid_gap_event(esp_gap_ble_cb_event_t event, esp_ble_gap_cb_param_t *param);

/** Forget every bonded BLE peer (returns how many were removed). */
unsigned app_ble_hid_clear_bonds(void);

/** Queue one input report on the BLE HID device (ignored while disconnected). */
void app_ble_hid_send(uint8_t report_id, const uint8_t *data, size_t len);

#ifdef __cplusplus
}
#endif
