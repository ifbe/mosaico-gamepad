// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Start HID over BR/EDR (the classic "HID device" profile).
 *
 * Requires app_bt_core_start() with a transport that includes Classic. The device
 * name, class of device and discoverability are owned by app_bt. */
esp_err_t app_btclassic_hid_start(const uint8_t *report_map, size_t report_map_len);

/** True while a classic host is connected. */
bool app_btclassic_hid_connected(void);

/** Queue one input report on the classic HID device. */
void app_btclassic_hid_send(uint8_t report_id, const uint8_t *data, size_t len);

#ifdef __cplusplus
}
#endif
