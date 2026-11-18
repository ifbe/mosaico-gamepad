// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <stddef.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Register the UAC2 playback function with ESP-Iris.
 *
 * Must run before ESP-Iris installs its USB driver: the configuration
 * descriptor is composed at that point. */
esp_err_t app_usb_audio_register(uint8_t mic_ep_in);

/** Play a 2 s 440 Hz test tone through the codec (no USB involved). */
void app_audio_play_test_tone(void);

/** Start the audio path, or report counters when it is already running. */
void app_audio_start_on_demand(void);

/** Register the control RPC (0x4800/1) that starts the audio path on demand
 *  and reports counters. Must run after ESP-Iris is up. */
void app_audio_control_register(void);

/** Open the speaker codec and start the task that plays host audio. */
esp_err_t app_usb_audio_start(void);

/** Build a standalone configuration descriptor with only the audio function,
 * starting at interface @p base (0 for an audio-only device). */
size_t app_usb_audio_build_config(uint8_t *out, size_t cap, uint8_t base);

#ifdef __cplusplus
}
#endif

/** True while the host is actively streaming audio to this device. */
bool app_usb_audio_streaming(void);

/** Variant name, reported at boot. */
#define AUDIO_USB_CLASS_NAME "uac2"
