// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <stdbool.h>

#include "app_cfg.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Start the Bluetooth controller and Bluedroid with the transport selected by
 * the persisted configuration (off / ble / classic / both).
 *
 * This is the shared layer: the BLE HID, Classic HID and Classic audio
 * components all attach to it and must be started after this call. When the
 * configuration disables Bluetooth this returns ESP_OK with app_bt_core_ready()
 * false. */
esp_err_t app_bt_core_start(void);

/** True when controller and host are running. */
bool app_bt_core_ready(void);

/** True when the configured transport includes BLE. */
bool app_bt_core_wants_ble(void);

/** True when the configured transport includes Classic (BR/EDR). */
bool app_bt_core_wants_classic(void);

/** Build the Bluetooth HID report map (from the USB core's descriptors, filtered
 * by the persisted bt_hid mask), start the enabled transports and attach them
 * as the report sink of app_usb_core. Call after app_bt_core_start(). */
esp_err_t app_bt_core_hid_start(void);

/** True while any Bluetooth HID transport has a host connected. */
bool app_bt_core_hid_connected(void);

/** Forward one input report to every connected Bluetooth transport. */
void app_bt_core_hid_send(uint8_t report_id, const uint8_t *data, size_t len);

/** Take the Classic ACL link out of sniff mode.
 *
 * Anyone may call this at any time; calls are coalesced to one every
 * APP_BT_KICK_MIN_INTERVAL_US so several 100 Hz callers cannot flood the
 * controller. Returns true when this call actually kicked the link (false when
 * throttled, when Classic is disabled, or when no peer is known). Task context
 * only. */
bool app_bt_core_kick_acl_sniff(void);

/** Remember the peer of the Classic link so kicks do not have to enumerate the
 * bond database. Call it from the profile that learns the address. */
void app_bt_core_note_classic_peer(const uint8_t bda[6]);

/** Copy the cached Classic peer. False when no profile has reported one yet. */
bool app_bt_core_classic_peer(uint8_t out[6]);

/** Kick counters since boot, for the status line. Any pointer may be NULL. */
void app_bt_core_kick_stats(unsigned *kicks, unsigned *throttled, unsigned *age_ms);

/** LE advertising name (also the classic device name's counterpart). */
const char *app_bt_core_le_name(void);

/** Classic (BR/EDR) device name. */
const char *app_bt_core_classic_name(void);

#ifdef __cplusplus
}
#endif
