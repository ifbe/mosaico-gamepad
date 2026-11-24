// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Public API of the USB sound card function.
 *
 * Both UAC implementations are compiled in and the settings page selects one at
 * boot (UAC1 and UAC2 are mutually exclusive). main calls this interface
 * unconditionally; app_usb_audio.c forwards to the selected implementation. */

/** Register the audio function with ESP-Iris.
 *
 * Must run before ESP-Iris installs its USB driver: the configuration
 * descriptor is composed at that point. @p mic_ep_in is the IN endpoint the
 * capture path must use; it comes from the startup allocation that also gives
 * the HID interfaces their endpoints. 0 means no capture endpoint is free. */
esp_err_t app_usb_audio_register(uint8_t mic_ep_in);

/** Open the speaker codec and start the task(s) that play host audio. */
esp_err_t app_usb_audio_start(void);

/** True while the host is actively streaming audio to this device. */
bool app_usb_audio_streaming(void);

/** Implementation name, for the boot log ("uac1" or "uac2"). */
const char *app_usb_audio_class_name(void);

/** Build a standalone configuration descriptor with only the audio function,
 * starting at interface @p base (0 for an audio-only device). */
size_t app_usb_audio_build_config(uint8_t *out, size_t cap, uint8_t base);

/** Play a 2 s 440 Hz test tone through the codec (no USB involved). */
void app_audio_play_test_tone(void);

/** Start the audio path, or report counters when it is already running. */
void app_audio_start_on_demand(void);

/** Register the control RPC (0x4800/1) that starts the audio path on demand and
 * reports counters. Must run after ESP-Iris is up. */
void app_audio_control_register(void);

#ifdef __cplusplus
}
#endif
