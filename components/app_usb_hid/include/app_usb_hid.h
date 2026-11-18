// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define APP_HID_KEYBOARD_KEYS 6

/** Register the enabled HID interfaces with ESP-Iris. Must run after
 * app_cfg_init() and before ESP-Iris installs its USB driver. */
esp_err_t app_usb_hid_register_usb(uint8_t first_ep_in);

/** Start the 8 ms report task. Call after the UI is up. */
esp_err_t app_usb_hid_start(void);

/** True while the USB host has the composite device mounted. */
bool app_usb_hid_usb_connected(void);

/* ---- report descriptors ---------------------------------------------------
 * The same descriptors feed the Bluetooth report map, so app_bt_core asks for
 * them instead of keeping a second copy.
 * Index order: 0 = gamepad, 1 = mouse, 2 = keyboard. */
size_t app_usb_hid_class_count(void);

/** Motion report descriptor (report ID 4), appended to the Bluetooth report
 * map when the corresponding transport enables APP_CLASS_SENSOR. */
const uint8_t *app_usb_hid_sensor_desc(size_t *len);
const uint8_t *app_usb_hid_class_desc(size_t index, size_t *len);

/* ---- Bluetooth sink -------------------------------------------------------
 * The report task lives here because USB is always present; Bluetooth is
 * optional. app_bt_core attaches itself once its transports are up, which keeps
 * the dependency one-way (Bluetooth knows about USB, not the reverse). */
typedef bool (*app_usb_hid_bt_connected_fn)(void);
typedef void (*app_usb_hid_bt_send_fn)(uint8_t report_id, const uint8_t *data, size_t len);

void app_usb_hid_attach_bt(app_usb_hid_bt_connected_fn connected,
                            app_usb_hid_bt_send_fn send);

/* ---- input state ---------------------------------------------------------- */

/** Gamepad button mask: bit0=X, bit1=A, bit2=B, bit3=Y, bit4..7=up/down/left/right,
 * bit8=LB, bit9=RB, bit10=LS, bit11=RS, bit12=SELECT, bit13=START. */
void app_usb_hid_set_buttons(uint16_t mask);

/** Analog triggers, 0 = released, 255 = fully pressed. */
void app_usb_hid_set_triggers(uint8_t lt, uint8_t rt);

/** Stick axes, 0..255 with 128 at centre. */
void app_usb_hid_set_sticks(uint8_t lx, uint8_t ly, uint8_t rx, uint8_t ry);

/** Relative mouse movement; deltas accumulate until the next report. */
void app_usb_hid_mouse(uint8_t buttons, int8_t dx, int8_t dy, int8_t wheel, int8_t pan);

/** Keyboard report: modifier bits plus up to six HID key codes (0 = empty). */
void app_usb_hid_keyboard(uint8_t modifiers, const uint8_t keys[APP_HID_KEYBOARD_KEYS]);

#ifdef __cplusplus
}
#endif
