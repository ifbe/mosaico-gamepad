// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "app_audio_core.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Routing policy: when 1, a connected Bluetooth peer takes the codec and USB
 * audio received meanwhile is dropped (not buffered), so the moment Bluetooth
 * disconnects the USB stream is audible again without a backlog. Set to 0 to
 * prefer USB whenever it is streaming. */
#ifndef APP_AUDIO_BT_HAS_PRIORITY
#define APP_AUDIO_BT_HAS_PRIORITY 1
#endif

/* Which transport currently owns the codec: USB audio and Bluetooth never share
 * the speaker at the same time, and each needs its own sample rate. */
typedef enum {
    APP_AUDIO_SOURCE_NONE = 0,
    APP_AUDIO_SOURCE_USB,
    /* The three Bluetooth links can be active independently (BLE and Classic are
     * separate radios from the host's point of view), so each gets its own
     * stream, gain and queue slot. A2DP and HFP share the Classic link, but a
     * phone pauses media when a call starts. */
    APP_AUDIO_SOURCE_BT_A2DP,
    APP_AUDIO_SOURCE_BT_HFP,
    APP_AUDIO_SOURCE_BT_LE,
    /* Locally generated playback (file / network PCM). Nothing in this build
     * pushes to it yet; the slot and the API are ready for that source. */
    APP_AUDIO_SOURCE_LOCAL,
} app_audio_source_t;

/** Open the on-board ES8311 speaker path at 48 kHz / 16-bit / stereo. */
esp_err_t app_audio_codec_start(void);

/* Transports report their state and then read/write unconditionally. Which
 * streams are heard, mixed, or dropped is decided here, not by the transports.
 */

/** Report that @p source has audio to play at @p sample_rate. */
esp_err_t app_audio_codec_playback_start(app_audio_source_t source, uint32_t sample_rate);

/** Report that @p source stopped streaming. */
void app_audio_codec_playback_stop(app_audio_source_t source);

/** True when @p source is the stream currently being heard (policy result). */
bool app_audio_codec_is_heard(app_audio_source_t source);

/** Global audio processing switch (settings page). While disabled the codec
 *  keeps running but every playback write is dropped and the microphone reports
 *  silence, so the device stays enumerated without producing sound. */
void app_audio_codec_set_enabled(bool enabled);
bool app_audio_codec_enabled(void);

/** Current I2S rate (set by the playback source; capture follows it). A consumer
 *  registered with this rate needs no resampling at all. */
uint32_t app_audio_codec_i2s_rate(void);

/** Same as app_audio_codec_start(), opening the speaker at @p sample_rate. */
esp_err_t app_audio_codec_start_at(uint32_t sample_rate);

/** Open the I2S port (TX and RX) and the speaker once, at boot, before any
 *  stream exists. Callers that only ever stream on demand can skip this. */
esp_err_t app_audio_codec_preopen(uint32_t sample_rate);

/** Blocking write of interleaved 16-bit stereo PCM at 48 kHz. */
esp_err_t app_audio_codec_write(const void *data, size_t bytes);

/* ---- playback mixing ------------------------------------------------------
 * Every playback transport hands its PCM to the codec at its own rate; the codec
 * resamples to the fixed 48 kHz I2S rate, sums the sources and writes one
 * stream. Sources never need to agree on a rate.
 *
 * Per-source gain is reserved for a future mixing policy (e.g. ducking the
 * music while a call is active); the default is unity and nothing changes it.
 */
/* Upper bound of the dynamic mix rate. The actual rate is
 * min(APP_AUDIO_MIX_MAX_RATE, max(rate of the active sources)), so a
 * USB-only or dual-source setup runs at 48 kHz while a call-only setup can drop
 * to 16 kHz. */
#define APP_AUDIO_MIX_MAX_RATE 48000
/* Never go below this. The ES8311 microphone shares the I2S clock with the
 * speaker; dropping to the SCO rate (16 kHz) left the capture channel running
 * at 48 kHz while the ring was declared 16 kHz, and that 3x mismatch made the
 * HF uplink send three times the audio (robot voice on the phone). */
#ifndef APP_AUDIO_MIX_MIN_RATE
#define APP_AUDIO_MIX_MIN_RATE 48000
#endif

/** Submit the decoder's raw PCM from @p source at @p sample_rate.
 *
 * @p channels is the layout the decoder produced (1 for SCO/LC3, 2 for A2DP and
 * USB); @p frames counts frames, not samples. Never blocks on the I2S: the data
 * is queued, and the mixer resamples, expands mono to stereo and sums. */
esp_err_t app_audio_codec_playback_write(app_audio_source_t source, uint32_t sample_rate,
                                         uint8_t channels, const void *data, size_t frames);

/** Reserved mixing hook: per-source gain in Q16 (0x10000 = unity). */
esp_err_t app_audio_codec_set_gain_q16(app_audio_source_t source, int32_t gain_q16);

/** Stop mixing @p source (its queued audio is dropped). */
void app_audio_codec_playback_stop(app_audio_source_t source);

/** Output volume, 0..100 percent (out-of-range values are clamped). */
esp_err_t app_audio_codec_set_volume(int percent);

/** Microphone input gain in dB (ES8311 accepts roughly -96..+30). */
esp_err_t app_audio_codec_set_mic_gain_db(float gain_db);

/** Open the ES8311 capture path (48 kHz, 16-bit, stereo) with mic gain. */
esp_err_t app_audio_codec_mic_start(void);

/** Same as app_audio_codec_mic_start(), capturing at @p sample_rate. */
esp_err_t app_audio_codec_mic_start_at(uint32_t sample_rate);

/** Blocking read of interleaved 16-bit stereo PCM from the microphone. */
esp_err_t app_audio_codec_mic_read(void *buffer, size_t bytes, size_t *bytes_read);

/** Close the capture path. */
void app_audio_codec_mic_stop(void);

/* ---- microphone fan-out ---------------------------------------------------
 * One capture task feeds several consumers. Consumers read the same shared
 * block when their rate equals the capture rate (zero copy) and resample into
 * their own buffer otherwise. Each consumer keeps its own cursor.
 */
/* One cursor in the shared microphone ring (owned by app_audio_core). */
typedef app_audio_core_mic_reader_t app_audio_mic_consumer_t;

/** Register a consumer of the microphone stream. Returns ESP_OK on success. */
esp_err_t app_audio_codec_mic_consumer_add(app_audio_mic_consumer_t *consumer,
                                           app_audio_source_t source, uint32_t sample_rate);

/** Remove a consumer. */
void app_audio_codec_mic_consumer_remove(app_audio_mic_consumer_t *consumer);

/** Copy the next frames for @p consumer. Returns frames copied, or 0 when the
 *  consumer is behind/blocked. Silence is returned when another source owns the
 *  microphone and the rates differ. */
size_t app_audio_codec_mic_consumer_read(app_audio_mic_consumer_t *consumer,
                                         int16_t *mono_out, size_t frames);

#ifdef __cplusplus
}
#endif
