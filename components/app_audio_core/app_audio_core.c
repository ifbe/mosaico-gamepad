// SPDX-License-Identifier: Apache-2.0

#include "app_audio_core.h"

#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "audio_core";

/* One queue per playback slot. The ring must be much larger than the playback
 * cushion (APP_AUDIO_CORE_LATENCY_MS, 20 ms): the cushion decides the latency,
 * the ring absorbs bursts and pauses on top of it. 4096 frames is ~85 ms at
 * 48 kHz, 93 ms at 44.1 kHz and 512 ms at 8 kHz — 16 KB of PSRAM per active
 * slot, allocated on first use and released when the link stops. */
#define PLAY_RING_FRAMES 4096
#define PLAY_RING_MASK (PLAY_RING_FRAMES - 1)

typedef struct {
    /* Allocated from PSRAM on first use: a link that is never enabled must not
     * cost 4 KB of internal RAM. */
    int16_t *queue; /* raw decoder output, PLAY_RING_FRAMES frames */
    uint8_t channels; /* 1 (SCO/LC3) or 2 (A2DP/USB) */
    volatile uint32_t head;              /* frames pushed */
    volatile uint32_t tail;              /* frames pulled */
    uint32_t phase_q16;                  /* fractional read position */
    uint32_t rate;
    int32_t gain_q16;
    bool started; /* cushion reached at least once since the last drop */
} play_slot_t;

static play_slot_t s_play[APP_AUDIO_SLOT_COUNT];
/* Frames each slot actually contributed to the mix (diagnostics). */
static volatile uint32_t s_play_delivered[APP_AUDIO_SLOT_COUNT];
/* Blocks each slot could not fill, i.e. silence had to be substituted. */
static volatile uint32_t s_play_starved[APP_AUDIO_SLOT_COUNT];
static portMUX_TYPE s_play_lock = portMUX_INITIALIZER_UNLOCKED;
static TaskHandle_t s_play_wake;

/* Microphone ring: mono frames at the capture rate, plus one cursor per
 * consumer. */
#define MIC_RING_FRAMES 960
static int16_t s_mic_ring[MIC_RING_FRAMES];
static volatile uint32_t s_mic_head;
static volatile uint32_t s_mic_rate;

static bool play_alloc(play_slot_t *slot, int slot_id, uint8_t channels)
{
    if (slot->queue != NULL && slot->channels == channels) {
        return true;
    }
    if (slot->queue != NULL) {
        /* Same slot, different layout (mono <-> stereo): rebuild the ring, the
         * queued frames use the old stride and cannot be reinterpreted. */
        app_audio_core_playback_drop(slot_id);
    }
    int16_t *ring = heap_caps_malloc((size_t)PLAY_RING_FRAMES * channels * sizeof(int16_t),
                                     MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (ring == NULL) {
        ESP_LOGE(TAG, "no PSRAM for playback slot %d", slot_id);
        return false;
    }
    taskENTER_CRITICAL(&s_play_lock);
    if (slot->queue == NULL) {
        slot->queue = ring;
        /* The stride every consumer uses comes from here: leaving it at 0 made
         * the second push of a slot fail (0 != 2) and the mixer read garbage. */
        slot->channels = channels;
        slot->head = 0;
        slot->tail = 0;
        slot->phase_q16 = 0;
        slot->started = false;
        ring = NULL;
    }
    taskEXIT_CRITICAL(&s_play_lock);
    if (ring != NULL) {
        heap_caps_free(ring); /* lost the race: another push allocated first */
    }
    ESP_LOGI(TAG, "playback slot %d: %u frames x %u ch in PSRAM", slot_id,
             (unsigned)PLAY_RING_FRAMES, (unsigned)channels);
    return true;
}

static uint32_t play_count(const play_slot_t *slot)
{
    return slot->head - slot->tail;
}

static play_slot_t *play_slot(int slot)
{
    if (slot < 0 || slot >= APP_AUDIO_SLOT_COUNT) {
        return NULL;
    }
    return &s_play[slot];
}

void app_audio_core_set_playback_wake(void *task)
{
    s_play_wake = (TaskHandle_t)task;
}

esp_err_t app_audio_core_playback_push(int slot_id, uint32_t sample_rate, uint8_t channels,
                                       const int16_t *pcm, size_t frames)
{
    play_slot_t *slot = play_slot(slot_id);
    if (slot == NULL || pcm == NULL || frames == 0 || (channels != 1 && channels != 2)) {
        /* Diagnostic (gamepad): a rejected push used to be invisible. */
        static uint32_t last_warn_ms;
        const uint32_t now_ms = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
        if (now_ms - last_warn_ms > 2000) {
            last_warn_ms = now_ms;
            ESP_LOGW(TAG, "push slot %d rejected: frames=%u rate=%u ch=%u pcm=%p",
                     slot_id, (unsigned)frames, (unsigned)sample_rate,
                     (unsigned)channels, pcm);
        }
        return ESP_ERR_INVALID_ARG;
    }
    if (!play_alloc(slot, slot_id, channels)) {
        return ESP_ERR_NO_MEM;
    }
    taskENTER_CRITICAL(&s_play_lock);
    int16_t *ring = slot->queue;
    if (ring == NULL) {
        /* The slot was released between the allocation and here: drop the packet
         * instead of writing into freed memory. */
        taskEXIT_CRITICAL(&s_play_lock);
        return ESP_ERR_INVALID_STATE;
    }
    slot->rate = sample_rate;
    if (slot->gain_q16 == 0) {
        slot->gain_q16 = 0x10000; /* first use: unity */
    }
    for (size_t i = 0; i < frames; ++i) {
        if (play_count(slot) >= PLAY_RING_FRAMES) {
            /* Overrun: keep the newest audio, drop the oldest frame. */
            ++slot->tail;
            slot->phase_q16 = 0;
        }
        const uint32_t pos = slot->head & PLAY_RING_MASK;
        for (uint8_t c = 0; c < channels; ++c) {
            ring[pos * channels + c] = pcm[i * channels + c];
        }
        ++slot->head;
    }
    taskEXIT_CRITICAL(&s_play_lock);
    if (s_play_wake != NULL) {
        xTaskNotifyGive(s_play_wake);
    }
    return ESP_OK;
}

static size_t pull_resampled(int slot_id, int16_t *out, size_t out_frames,
                             uint32_t out_rate)
{
    play_slot_t *slot = play_slot(slot_id);
    if (slot == NULL || out == NULL || out_frames == 0 || out_rate == 0) {
        return 0;
    }
    taskENTER_CRITICAL(&s_play_lock);
    const uint32_t src_rate = slot->rate;
    const bool ready = src_rate != 0 && slot->queue != NULL;
    taskEXIT_CRITICAL(&s_play_lock);
    if (!ready) {
        return 0;
    }
    const uint32_t step = (uint32_t)(((uint64_t)src_rate << 16) / out_rate);
    if (step == 0) {
        return 0;
    }
    /* Frames this block will consume at the native rate. */
    const uint32_t need = (((uint32_t)(out_frames - 1) * step) >> 16) + 2;
    const uint32_t available = play_count(slot);
    if (!slot->started) {
        /* Cushion: wait for APP_AUDIO_CORE_LATENCY_MS of audio before the first
         * block, otherwise every short burst plays immediately and crackles. */
        const uint32_t cushion = (src_rate / 1000u) * APP_AUDIO_CORE_LATENCY_MS;
        if (available < cushion || available < need) {
            return 0;
        }
        slot->started = true;
    }
    if (available < need) {
        return 0;
    }

    const int32_t gain = slot->gain_q16 != 0 ? slot->gain_q16 : 0x10000;
    const uint8_t ch = slot->channels;
    uint32_t pos = slot->phase_q16;
    taskENTER_CRITICAL(&s_play_lock);
    for (size_t i = 0; i < out_frames; ++i) {
        const uint32_t idx = pos >> 16;
        const uint32_t frac = pos & 0xFFFFu;
        const uint32_t i0 = (slot->tail + idx) & PLAY_RING_MASK;
        const uint32_t i1 = (slot->tail + idx + 1) & PLAY_RING_MASK;
        const int32_t l0 = slot->queue[i0 * ch];
        const int32_t l1 = slot->queue[i1 * ch];
        /* Mono sources (SCO, LC3) are expanded here, not in the producer. */
        const int32_t r0 = ch == 2 ? slot->queue[i0 * ch + 1] : l0;
        const int32_t r1 = ch == 2 ? slot->queue[i1 * ch + 1] : l1;
        const int32_t ls = l0 + (((l1 - l0) * (int32_t)frac) >> 16);
        const int32_t rs = r0 + (((r1 - r0) * (int32_t)frac) >> 16);
        int32_t l = (ls * gain) / 0x10000;
        int32_t r = (rs * gain) / 0x10000;
        if (l > 32767) {
            l = 32767;
        } else if (l < -32768) {
            l = -32768;
        }
        if (r > 32767) {
            r = 32767;
        } else if (r < -32768) {
            r = -32768;
        }
        out[i * 2] = (int16_t)l;
        out[i * 2 + 1] = (int16_t)r;
        pos += step;
    }
    slot->tail += pos >> 16;
    slot->phase_q16 = pos & 0xFFFFu;
    taskEXIT_CRITICAL(&s_play_lock);
    s_play_delivered[slot_id] += (uint32_t)out_frames;
    return out_frames;
}

size_t app_audio_core_mix(uint32_t slot_mask, uint32_t out_rate, uint8_t out_channels,
                          int16_t *out, size_t out_frames)
{
    if (out == NULL || out_frames == 0 || out_rate == 0 || out_channels != 2) {
        return 0;
    }
    /* MIX_BLOCK scratch: one slot at a time, summed into the caller's buffer. */
    static int16_t tmp[1024 * 2];
    if (out_frames > 1024) {
        return 0;
    }
    memset(out, 0, out_frames * 2 * sizeof(int16_t));
    for (int slot = 0; slot < APP_AUDIO_SLOT_COUNT; ++slot) {
        if ((slot_mask & APP_AUDIO_CORE_SLOT(slot)) == 0) {
            continue;
        }
        if (pull_resampled(slot, tmp, out_frames, out_rate) != out_frames) {
            /* Not enough queued audio for a whole block: this block is a gap for
             * that slot. Counted so the consumer can report real underruns
             * instead of only logging a queue level. */
            ++s_play_starved[slot];
            continue;
        }
        for (size_t i = 0; i < out_frames * 2; ++i) {
            int32_t v = out[i] + tmp[i];
            if (v > 32767) {
                v = 32767;
            } else if (v < -32768) {
                v = -32768;
            }
            out[i] = (int16_t)v;
        }
    }
    return out_frames;
}

void app_audio_core_playback_drop(int slot_id)
{
    play_slot_t *slot = play_slot(slot_id);
    if (slot == NULL) {
        return;
    }
    taskENTER_CRITICAL(&s_play_lock);
    int16_t *ring = slot->queue;
    slot->queue = NULL;
    slot->channels = 0;
    slot->head = 0;
    slot->tail = 0;
    slot->phase_q16 = 0;
    slot->rate = 0;
    slot->started = false;
    taskEXIT_CRITICAL(&s_play_lock);
    /* Released as soon as the link stops: only active links hold PSRAM. The
     * mixer re-checks the pointer under the same lock, so it cannot be using
     * the buffer now. */
    if (ring != NULL) {
        heap_caps_free(ring);
        ESP_LOGI(TAG, "playback slot %d released", slot_id);
    }
    if (s_play_wake != NULL) {
        xTaskNotifyGive(s_play_wake);
    }
}

bool app_audio_core_slot_active(int slot_id)
{
    const play_slot_t *slot = play_slot(slot_id);
    return slot != NULL && slot->rate != 0;
}

uint32_t app_audio_core_slot_rate(int slot_id)
{
    const play_slot_t *slot = play_slot(slot_id);
    return slot != NULL ? slot->rate : 0;
}

uint32_t app_audio_core_slot_take_starved(int slot_id)
{
    if (slot_id < 0 || slot_id >= APP_AUDIO_SLOT_COUNT) {
        return 0;
    }
    const uint32_t value = s_play_starved[slot_id];
    s_play_starved[slot_id] = 0;
    return value;
}

uint32_t app_audio_core_slot_take_delivered(int slot_id)
{
    if (slot_id < 0 || slot_id >= APP_AUDIO_SLOT_COUNT) {
        return 0;
    }
    const uint32_t value = s_play_delivered[slot_id];
    s_play_delivered[slot_id] = 0;
    return value;
}

size_t app_audio_core_slot_queued(int slot_id)
{
    play_slot_t *slot = play_slot(slot_id);
    if (slot == NULL) {
        return 0;
    }
    return play_count(slot);
}

void app_audio_core_set_gain_q16(int slot_id, int32_t gain_q16)
{
    play_slot_t *slot = play_slot(slot_id);
    if (slot == NULL) {
        return;
    }
    if (gain_q16 < 0) {
        gain_q16 = 0;
    } else if (gain_q16 > 0x10000) {
        gain_q16 = 0x10000;
    }
    slot->gain_q16 = gain_q16;
}

/* ---- recording ------------------------------------------------------------
 * The mixer is the only consumer of the playback queues, so the recorder does
 * not read them itself: it receives the already mixed block through this ring.
 * That keeps a single set of read cursors and gives the recorder exactly what
 * the speaker hears (gain and channel expansion included). */
#define RECORD_RING_FRAMES 8192 /* ~170 ms at 48 kHz stereo */

static int16_t *s_record_ring;
static volatile uint32_t s_record_head;
static volatile uint32_t s_record_tail;
static portMUX_TYPE s_record_lock = portMUX_INITIALIZER_UNLOCKED;
static volatile bool s_record_enabled;

void app_audio_core_record_enable(bool enabled)
{
    s_record_enabled = enabled;
    if (enabled && s_record_ring == NULL) {
        s_record_ring = heap_caps_malloc(RECORD_RING_FRAMES * 2 * sizeof(int16_t),
                                         MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (s_record_ring == NULL) {
            ESP_LOGE(TAG, "no PSRAM for the record ring");
            s_record_enabled = false;
            return;
        }
        s_record_head = 0;
        s_record_tail = 0;
        ESP_LOGI(TAG, "record ring: %u frames in PSRAM", (unsigned)RECORD_RING_FRAMES);
    }
    if (!enabled) {
        taskENTER_CRITICAL(&s_record_lock);
        s_record_head = 0;
        s_record_tail = 0;
        taskEXIT_CRITICAL(&s_record_lock);
    }
}

bool app_audio_core_record_enabled(void)
{
    return s_record_enabled;
}

void app_audio_core_record_push(const int16_t *stereo, size_t frames)
{
    if (!s_record_enabled || s_record_ring == NULL || stereo == NULL || frames == 0) {
        return;
    }
    taskENTER_CRITICAL(&s_record_lock);
    for (size_t i = 0; i < frames; ++i) {
        if (s_record_head - s_record_tail >= RECORD_RING_FRAMES) {
            ++s_record_tail; /* recorder too slow: drop the oldest */
        }
        const uint32_t pos = s_record_head % RECORD_RING_FRAMES;
        s_record_ring[pos * 2] = stereo[i * 2];
        s_record_ring[pos * 2 + 1] = stereo[i * 2 + 1];
        ++s_record_head;
    }
    taskEXIT_CRITICAL(&s_record_lock);
}

size_t app_audio_core_record_read(int16_t *out, size_t frames)
{
    if (s_record_ring == NULL || out == NULL || frames == 0) {
        return 0;
    }
    taskENTER_CRITICAL(&s_record_lock);
    const uint32_t available = s_record_head - s_record_tail;
    if (frames > available) {
        frames = available;
    }
    for (size_t i = 0; i < frames; ++i) {
        const uint32_t pos = (s_record_tail + i) % RECORD_RING_FRAMES;
        out[i * 2] = s_record_ring[pos * 2];
        out[i * 2 + 1] = s_record_ring[pos * 2 + 1];
    }
    s_record_tail += frames;
    taskEXIT_CRITICAL(&s_record_lock);
    return frames;
}

/* ---------------------------------------------------------------- microphone */

esp_err_t app_audio_core_mic_reader_add(app_audio_core_mic_reader_t *reader,
                                        uint32_t sample_rate)
{
    if (reader == NULL || sample_rate == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    reader->used = true;
    reader->sample_rate = sample_rate;
    reader->pos_q16 = (uint64_t)s_mic_head << 16;
    return ESP_OK;
}

void app_audio_core_mic_reader_remove(app_audio_core_mic_reader_t *reader)
{
    if (reader != NULL) {
        reader->used = false;
    }
}

size_t app_audio_core_mic_read(app_audio_core_mic_reader_t *reader, int16_t *mono_out,
                               size_t frames)
{
    if (reader == NULL || !reader->used || mono_out == NULL || frames == 0) {
        return 0;
    }
    const uint32_t src = s_mic_rate;
    const uint64_t head = s_mic_head;
    if (src == 0 || head < 2) {
        /* No capture yet: report "nothing" so the caller waits instead of
         * spinning on silence (that would also leave its cursor ahead of the
         * producer and turn the stream into permanent silence). */
        return 0;
    }
    /* Replaying stale audio sounds worse than skipping it. */
    /* 64-bit: a 32-bit Q16 cursor overflows after 65536 frames (~1.4 s at
     * 48 kHz) and then always looks "behind" the producer, which makes every
     * consumer spin at CPU speed instead of following the capture rate. */
    const uint64_t floor_frame = reader->pos_q16 >> 16;
    if (head > floor_frame && head - floor_frame > MIC_RING_FRAMES) {
        reader->pos_q16 = (head - MIC_RING_FRAMES) << 16;
    }
    const uint32_t step_q16 = (uint32_t)(((uint64_t)src << 16) / reader->sample_rate);
    size_t done = 0;
    for (; done < frames; ++done) {
        const uint32_t pos = (uint32_t)(reader->pos_q16 >> 16);
        if ((uint64_t)pos + 1 >= head) {
            break; /* no fresh frame: report only what the producer delivered */
        }
        const uint32_t frac = (uint32_t)(reader->pos_q16 & 0xFFFFu);
        const int32_t s0 = s_mic_ring[pos % MIC_RING_FRAMES];
        const int32_t s1 = s_mic_ring[(pos + 1) % MIC_RING_FRAMES];
        mono_out[done] = (int16_t)(s0 + (((s1 - s0) * (int32_t)frac) >> 16));
        reader->pos_q16 += step_q16;
    }
    /* Returns the number of frames really produced. Padding here would let a
     * fast consumer turn one fresh frame into a whole block, i.e. amplify the
     * capture rate by the block size; consumers that need fixed-size packets
     * accumulate instead. */
    return done;
}

void app_audio_core_mic_publish(const int16_t *mono, size_t frames)
{
    if (mono == NULL || frames == 0) {
        return;
    }
    for (size_t i = 0; i < frames; ++i) {
        s_mic_ring[(s_mic_head + i) % MIC_RING_FRAMES] = mono[i];
    }
    s_mic_head += (uint32_t)frames; /* single producer, publish last */
}

uint32_t app_audio_core_mic_rate(void)
{
    return s_mic_rate;
}

void app_audio_core_mic_set_rate(uint32_t sample_rate)
{
    s_mic_rate = sample_rate;
}
