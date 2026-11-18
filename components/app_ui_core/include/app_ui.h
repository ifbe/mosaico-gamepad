// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <stdbool.h>

#include "esp_err.h"
#include "esp_gsp.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Screen layouts. Only one is visible at a time; the rest are hidden GSP
 * layers. Corners switch layouts, they do not change the USB device. */
typedef enum {
    APP_UI_LAYOUT_BLANK = 0,
    APP_UI_LAYOUT_GAMEPAD,
    APP_UI_LAYOUT_MOUSE,
    APP_UI_LAYOUT_KEYBOARD,
    APP_UI_LAYOUT_TERMINAL,
    APP_UI_LAYOUT_SETTING,
    APP_UI_LAYOUT_COUNT
} app_ui_layout_t;

/** Start the UI: marquee, status labels, corner markers and layout layers. */
esp_err_t app_ui_start(esp_gsp_handle_t gsp);

/** Show one layout (and hide the others). Safe from any task. */
void app_ui_set_layout(app_ui_layout_t layout);
app_ui_layout_t app_ui_layout(void);

const char *app_ui_layout_name(app_ui_layout_t layout);
app_ui_layout_t app_ui_layout_from_name(const char *name);

/** Enable/disable one settings chip: row 0 = USB, row 1 = Bluetooth,
 * index 0..4 = kbd, mouse, gamepad, ds4/slot, audio. */
void app_ui_set_chip(unsigned row, unsigned index, bool on);

/** Refresh every chip from the current configuration. */
void app_ui_refresh_chips(void);

/** Highlight one ABXY button: 0=X, 1=A, 2=B, 3=Y. */
void app_ui_set_button_glow(unsigned button, bool on);

/** Highlight one d-pad direction: 0=up, 1=down, 2=left, 3=right. */
void app_ui_set_dpad_glow(unsigned direction, bool on);

/** Move a stick knob; coordinates are the knob's top-left corner in scene px. */
void app_ui_set_stick_knob(bool left, int32_t x, int32_t y);

/** Replace one terminal line (0..11). */
void app_ui_set_terminal_line(unsigned line, const char *text);

#ifdef __cplusplus
}
#endif
