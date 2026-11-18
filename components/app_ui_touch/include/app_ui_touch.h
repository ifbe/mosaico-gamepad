// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "esp_err.h"
#include "esp_gsp.h"
#include "esp_lcd_touch.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Own the panel touch controller and route contacts by screen layout.
 *
 * GSP must be started without a touch handle (single reader). The task polls
 * at 16 ms, draws one circle per contact through the GSP overlay, switches
 * layouts on corner taps and feeds app_usb_core with gamepad/mouse/keyboard input.
 */
esp_err_t app_ui_touch_start(esp_gsp_handle_t gsp, esp_lcd_touch_handle_t touch);

#ifdef __cplusplus
}
#endif
