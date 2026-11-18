// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <stdbool.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Register the Classic audio profiles (A2DP sink + HFP hands-free).
 *
 * Call after the Bluetooth controller and Bluedroid are running (app_ble_start)
 * and only when the configured transport includes Classic. @p device_name may
 * be NULL to keep the default "Mosaico Classic". */
esp_err_t app_btclassic_audio_start(void);

/** Tear the profiles down; the controller itself stays with app_bt_core. */
void app_btclassic_audio_stop(void);

/** True while an A2DP source is connected. */
bool app_btclassic_audio_connected(void);

#ifdef __cplusplus
}
#endif
