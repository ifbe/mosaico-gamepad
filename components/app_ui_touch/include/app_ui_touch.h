// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Start the input device layer: open the CST9220 touch controller through the
 * BSP, configure the BOOT key and publish app_input_event_t samples.
 *
 * The task polls every 16 ms and only reports hardware edges and motion; every
 * page, gesture and HID decision belongs to app_ui_core. GSP must be started
 * with touch = NULL so this is the single reader of the controller.
 *
 * Call once at boot after the display is initialized. Returns ESP_OK, an
 * initialization error, or ESP_ERR_NO_MEM when the task cannot be created. */
esp_err_t app_ui_touch_start(void);

#ifdef __cplusplus
}
#endif
