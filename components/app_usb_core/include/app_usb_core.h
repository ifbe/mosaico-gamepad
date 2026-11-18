// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* USB device-level plumbing shared by the USB functions.
 *
 * The S31 USB core exposes seven data IN endpoints. ESP-Iris spends its CDC
 * pair(s) from 0x81 upwards, so functions are handed endpoints sequentially
 * from whatever is left. Keeping the arithmetic here means app_usb_hid and
 * app_usb_audio cannot disagree about which endpoint is free. */

/** First IN endpoint available to application functions (0x83 with a console-
 * only Iris build, 0x85 when the data-link CDC is enabled). */
uint8_t app_usb_core_first_in_endpoint(void);

/** Number of HID interfaces the mask will register (one per enabled class). */
unsigned app_usb_core_hid_interface_count(uint32_t class_mask);

/** Endpoint for the UAC2 microphone given the HID allocation, or 0 when the
 * range is exhausted and USB audio cannot be registered. */
uint8_t app_usb_core_mic_endpoint(uint8_t first_ep, unsigned hid_count);

#ifdef __cplusplus
}
#endif
