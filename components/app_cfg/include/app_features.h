// SPDX-License-Identifier: Apache-2.0
#pragma once

/* Compile-time feature switches.
 *
 * Every block is guarded with #if APP_FEATURE_* in the components, so setting a
 * macro to 0 removes that function from the firmware (code, tasks, RAM) without
 * touching the rest. The runtime configuration (app_cfg) then narrows what is
 * left; a runtime request for a feature that was compiled out is logged and
 * ignored.
 *
 * USB
 *   USB_HID    composite HID interfaces (gamepad, mouse, keyboard)
 *   USB_AUDIO  UAC2 speaker + microphone function
 * Bluetooth, split by transport and by function
 *   BLE_HID            HID over GATT (HOGP)
 *   BLE_AUDIO          LE Audio (BAP/ASCS) — placeholder, not implemented yet
 *   BT_CLASSIC_HID     HID over BR/EDR
 *   BT_CLASSIC_AUDIO   A2DP sink + HFP hands-free microphone
 */
#define APP_FEATURE_USB_HID         1
#define APP_FEATURE_USB_AUDIO       1
#define APP_FEATURE_BLE_HID         1
#define APP_FEATURE_BLE_AUDIO       0
#define APP_FEATURE_BT_CLASSIC_HID  1
#define APP_FEATURE_BT_CLASSIC_AUDIO 1

/* Anything that is not explicitly 1 is treated as disabled. */
#if APP_FEATURE_BT_CLASSIC_AUDIO && !APP_FEATURE_USB_AUDIO
/* Classic audio reuses the codec component, so it has to be compiled in. */
#undef APP_FEATURE_USB_AUDIO
#define APP_FEATURE_USB_AUDIO 1
#endif

#if APP_FEATURE_BLE_AUDIO && !APP_FEATURE_USB_AUDIO
/* LE Audio would reuse the same codec path. */
#undef APP_FEATURE_USB_AUDIO
#define APP_FEATURE_USB_AUDIO 1
#endif

/* True when at least one Bluetooth function is compiled in. */
#define APP_FEATURE_ANY_BT \
    (APP_FEATURE_BLE_HID || APP_FEATURE_BLE_AUDIO || \
     APP_FEATURE_BT_CLASSIC_HID || APP_FEATURE_BT_CLASSIC_AUDIO)

/* True when a Bluetooth transport carries HID classes. */
#define APP_FEATURE_ANY_BT_HID (APP_FEATURE_BLE_HID || APP_FEATURE_BT_CLASSIC_HID)
