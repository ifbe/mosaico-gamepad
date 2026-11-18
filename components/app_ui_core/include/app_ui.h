// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <stdbool.h>
#include <stdint.h>

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

/** Bring up the whole UI: panel, GSP scene, layout layers, settings chips,
 * overlay, timer, then the input device and its consumer.
 *
 * Owns the display and input device lifecycles; the caller only wires IRIS
 * screen mirroring to app_ui_handle(). Call once at boot. Returns ESP_OK, or
 * the bundle/display/GSP error; the device can still boot headless on failure.
 * A missing touch controller is logged and does not fail the UI. */
esp_err_t app_ui_start(void);

/** GSP handle of the running scene, NULL before app_ui_start() succeeds. */
esp_gsp_handle_t app_ui_handle(void);

/** Show one layout (and hide the others). Safe from any task. */
void app_ui_set_layout(app_ui_layout_t layout);
app_ui_layout_t app_ui_layout(void);

const char *app_ui_layout_name(app_ui_layout_t layout);
app_ui_layout_t app_ui_layout_from_name(const char *name);

/** Enable/disable one settings chip: row 0 = USB HID classes, row 1 = USB
 * emulation (ds4/ds5/xbox), row 2 = classic Bluetooth, row 3 = BLE. */
void app_ui_set_chip(unsigned row, unsigned index, bool on);

/** Refresh every chip from the current configuration. */
void app_ui_refresh_chips(void);

/** Highlight one ABXY button: 0=X, 1=A, 2=B, 3=Y. */
void app_ui_set_button_glow(unsigned button, bool on);

/** Buttons placed around the two sticks. Mirror pairs: LB/RB (upper outer),
 * LS/RS (upper inner), SELECT/START (lower inner), LT/RT (outer bars). */
typedef enum {
    APP_UI_SIDE_LB = 0,
    APP_UI_SIDE_RB,
    APP_UI_SIDE_LS,
    APP_UI_SIDE_RS,
    APP_UI_SIDE_SELECT,
    APP_UI_SIDE_START,
    APP_UI_SIDE_LT,
    APP_UI_SIDE_RT,
    APP_UI_SIDE_COUNT
} app_ui_side_t;

/** Highlight one stick-side button while its contact is down. */
void app_ui_set_side_glow(app_ui_side_t button, bool on);

/** Highlight one d-pad direction: 0=up, 1=down, 2=left, 3=right. */
void app_ui_set_dpad_glow(unsigned direction, bool on);

/** Move a stick knob; coordinates are the knob's top-left corner in scene px. */
void app_ui_set_stick_knob(bool left, int32_t x, int32_t y);

/** Replace one terminal line (0..29). */
void app_ui_set_terminal_line(unsigned line, const char *text);

#ifdef __cplusplus
}
#endif
