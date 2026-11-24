// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Audio queues shared between the transports and the codec.
 *
 * Every transport pushes the PCM it received into the queue of its slot and
 * returns; a dedicated consumer (the codec's mixer thread) pulls from the
 * queues. The microphone side is the mirror image: the capture thread publishes
 * mono frames into one ring and every consumer keeps its own cursor.
 *
 * Nothing here touches I2S or knows about USB/Bluetooth, so the transports only
 * depend on this component's API, never on the codec internals. */

/* Playback slots. The order is the audio priority used by the codec when it has
 * to decide what "heard" means. */
#define APP_AUDIO_SLOT_A2DP 0
#define APP_AUDIO_SLOT_HFP 1
#define APP_AUDIO_SLOT_LE 2
#define APP_AUDIO_SLOT_USB 3
#define APP_AUDIO_SLOT_LOCAL 4
#define APP_AUDIO_SLOT_COUNT 5

/** Bit for @p slot in the mask passed to app_audio_core_mix(). */
#define APP_AUDIO_CORE_SLOT(slot) (1u << (slot))

/* Playback cushion per slot: the mixer emits silence until this much audio is
 * queued, then starts pulling. Bigger = tolerates more transport jitter. */
#ifndef APP_AUDIO_CORE_LATENCY_MS
#define APP_AUDIO_CORE_LATENCY_MS 20
#endif

/* ---- playback ------------------------------------------------------------ */

/** Wake @p task whenever new playback data is queued or a slot is dropped.
 * Pass NULL to detach. The task handle is not owned by this component. */
void app_audio_core_set_playback_wake(void *task);

/** Queue the decoder's raw PCM for @p slot at @p sample_rate.
 *
 * @p channels is the layout the decoder produced (1 for SCO/LC3, 2 for A2DP and
 * USB); the ring stores it unchanged. Channel expansion to the stereo mix
 * happens when the consumer pulls, so the producers never convert anything.
 *
 * The ring is allocated from PSRAM the first time the slot is used, so a link
 * that is never enabled costs nothing. Never blocks: when the queue is full the
 * oldest frame is dropped. */
esp_err_t app_audio_core_playback_push(int slot, uint32_t sample_rate, uint8_t channels,
                                       const int16_t *pcm, size_t frames);

/** Mix the requested slots into one block.
 *
 * For every slot selected by @p slot_mask the queue is resampled to @p out_rate,
 * expanded to @p out_channels and scaled by the slot gain, then summed with
 * saturation. A slot without enough queued audio contributes silence; the block
 * is always filled, so the caller can write it straight to the I2S.
 *
 * There is exactly one consumer of the playback queues (the codec's mixer
 * thread), so the read position is per queue, not per caller. */
size_t app_audio_core_mix(uint32_t slot_mask, uint32_t out_rate, uint8_t out_channels,
                          int16_t *out, size_t out_frames);

/** Drop everything queued for @p slot and stop its cushion. */
void app_audio_core_playback_drop(int slot);

/** True once the slot has data queued (rate known). */
bool app_audio_core_slot_active(int slot);

/** Sample rate last pushed for @p slot, 0 when unknown. */
uint32_t app_audio_core_slot_rate(int slot);

/** Frames currently queued for @p slot (0 when the slot is not allocated).
 * Diagnostic only: the consumer owns the read position, this is just a level. */
size_t app_audio_core_slot_queued(int slot);

/** Frames the consumer actually received from @p slot since the last call, and
 * resets the counter. Diagnostic only. */
uint32_t app_audio_core_slot_take_delivered(int slot);

/** Times @p slot was asked for a block but had less than one block queued since
 * the last call, and resets the counter. Each of those is a silent block, i.e.
 * an audible gap. Diagnostic only. */
uint32_t app_audio_core_slot_take_starved(int slot);

/** Per-slot gain in Q16 (0x10000 = unity), clamped to 0..1.0. */
void app_audio_core_set_gain_q16(int slot, int32_t gain_q16);

/* ---- microphone ---------------------------------------------------------- */

/** A consumer cursor in the shared microphone ring. Each consumer owns one and
 * reads independently; no consumer can starve another. */
typedef struct {
    bool used;
    uint32_t sample_rate; /* rate the consumer wants */
    uint64_t pos_q16;     /* absolute read position in the ring, Q16 */
} app_audio_core_mic_reader_t;

/* ---- recording ----------------------------------------------------------- */

/** Turn the record tap on/off. While off, the mixer does not copy its output. */
void app_audio_core_record_enable(bool enabled);
bool app_audio_core_record_enabled(void);

/** Copy one mixed block into the record ring (called by the I2S consumer right
 * after mixing, before/after writing the hardware). Never blocks; the oldest
 * frames are dropped when the recorder falls behind. */
void app_audio_core_record_push(const int16_t *stereo, size_t frames);

/** Read up to @p frames from the record ring; returns the frames copied.
 * @p frames must be stereo interleaved 16-bit, same format as the mixer. */
size_t app_audio_core_record_read(int16_t *out, size_t frames);

/** Register @p reader as a microphone consumer at @p sample_rate. */
esp_err_t app_audio_core_mic_reader_add(app_audio_core_mic_reader_t *reader,
                                        uint32_t sample_rate);

/** Unregister @p reader. The ring itself stays alive for the other consumers. */
void app_audio_core_mic_reader_remove(app_audio_core_mic_reader_t *reader);

/** Copy the mono frames @p reader has not seen yet, resampling from the ring rate
 * to the reader's rate, and return how many were produced.
 *
 * Bounded by what the capture side actually published: returns 0 when nothing is
 * new. Callers must wait a few ms on 0 and accumulate when they need fixed-size
 * blocks (a pad-with-silence read would turn one fresh frame into a whole block
 * and amplify the capture rate). */
size_t app_audio_core_mic_read(app_audio_core_mic_reader_t *reader, int16_t *mono_out,
                               size_t frames);

/** Publish @p frames mono samples from the capture thread. */
void app_audio_core_mic_publish(const int16_t *mono, size_t frames);

/** Rate the microphone is currently published at, 0 before the first publish. */
uint32_t app_audio_core_mic_rate(void);

/** Announce the capture rate (called by the capture thread when I2S changes). */
void app_audio_core_mic_set_rate(uint32_t sample_rate);

#ifdef __cplusplus
}
#endif
