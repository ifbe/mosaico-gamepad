// SPDX-License-Identifier: Apache-2.0

#include "app_audio_codec.h"

#include "bsp/audio.h"
#include "bsp/power.h"
#include "bsp/esp_mosaico.h"
#include "driver/i2s_std.h"
#include "esp_check.h"
#include "esp_codec_dev.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <string.h>

static const char *TAG = "audio_codec";

/* Master switch for the playback mixer. With 0 every transport writes straight
 * to the codec (single source, codec follows the source rate). Kept as a
 * compile-time switch while the mixer is being debugged. */
#ifndef APP_AUDIO_MIX_ENABLE
#define APP_AUDIO_MIX_ENABLE 0
#endif

/* Microphone fan-out: 1 = one capture task feeds all consumers through a shared
 * ring; 0 = each consumer reads the I2S microphone directly (single consumer at
 * a time). Kept as a switch while the capture path is being debugged. */
/* Digital gain applied to Bluetooth PCM (Q16). 0x20000 = +6 dB; the peer's own
 * volume stays authoritative, this only compensates for SBC decode level. */
#ifndef APP_AUDIO_BT_GAIN_Q16
#define APP_AUDIO_BT_GAIN_Q16 0x18000
#endif

#ifndef APP_AUDIO_MIC_FANOUT_ENABLE
#define APP_AUDIO_MIC_FANOUT_ENABLE 0
#endif


/* Dynamic I2S rate = min(cap, max(active playback source rate)). Declared here
 * because both the transport API and the mixer use it. */
static volatile uint32_t s_mix_rate;
/* Defined with the playback mixer further down; the speaker bring-up starts it. */
static void mix_task(void *arg);
static TaskHandle_t s_mix_task;
static void codec_watchdog_task(void *arg);
static bool s_watchdog_started;
static SemaphoreHandle_t s_mix_wake;
/* Output volume in percent. USB sets it from the host; Bluetooth has no volume
 * channel, so while a peer is heard the DAC runs at 100% and the peer's own
 * volume control stays authoritative. */
static int s_out_vol_pct = 80;
static app_audio_source_t playback_selected(void);

static void audio_codec_apply_volume(void);

static esp_codec_dev_handle_t s_speaker;
static uint32_t s_speaker_rate;
static esp_codec_dev_handle_t s_microphone;
static uint32_t s_mic_rate;

/* Single place that decides the DAC volume, so a codec reopen can never undo it. */
static void audio_codec_apply_volume(void)
{
    if (s_speaker == NULL) {
        return;
    }
    const int pct = playback_selected() == APP_AUDIO_SOURCE_BT ? 100 : s_out_vol_pct;
    (void)esp_codec_dev_set_out_vol(s_speaker, pct);
}

/* Diagnostic ladder for the ESP-Iris link regression:
 *   1 = power rails only, 2 = power + I2S, 0 = full codec bring-up. */
#define AUDIO_CODEC_DIAG 0

esp_err_t app_audio_codec_start_at(uint32_t sample_rate)
{
#if AUDIO_CODEC_DIAG == 1
    ESP_RETURN_ON_ERROR(bsp_power_set_vcc_3v3(true), TAG, "VCC_3V3");
    ESP_RETURN_ON_ERROR(bsp_power_set_codec_3v3(true), TAG, "CODEC_3V3");
    vTaskDelay(pdMS_TO_TICKS(10));
    ESP_LOGI(TAG, "diagnostic: power rails only");
    return ESP_OK;
#elif AUDIO_CODEC_DIAG == 2
    ESP_RETURN_ON_ERROR(bsp_audio_init(NULL), TAG, "I2S init");
    ESP_LOGI(TAG, "diagnostic: power + I2S");
    return ESP_OK;
#elif AUDIO_CODEC_DIAG == 8
    /* TX enabled with the lowest interrupt priority and a task that keeps the
     * DMA fed: separates "interrupt priority / underrun storm" from a real
     * DMA resource conflict. */
    ESP_RETURN_ON_ERROR(bsp_power_set_vcc_3v3(true), TAG, "VCC_3V3");
    ESP_RETURN_ON_ERROR(bsp_power_set_codec_3v3(true), TAG, "CODEC_3V3");
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    chan_cfg.auto_clear = true;
    chan_cfg.intr_priority = 1;
    i2s_chan_handle_t tx = NULL;
    ESP_RETURN_ON_ERROR(i2s_new_channel(&chan_cfg, &tx, NULL), TAG, "new channel");
    i2s_std_config_t cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(BSP_AUDIO_DEFAULT_SAMPLE_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(BSP_AUDIO_DEFAULT_BITS_PER_SAMPLE,
                                                       I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = GPIO_NUM_1,
            .ws = GPIO_NUM_2,
            .dout = GPIO_NUM_3,
            .din = I2S_GPIO_UNUSED,
        },
    };
    ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(tx, &cfg), TAG, "init std mode");
    ESP_RETURN_ON_ERROR(i2s_channel_enable(tx), TAG, "enable tx");
    static uint8_t silence[192];
    size_t written = 0;
    for (;;) {
        (void)i2s_channel_write(tx, silence, sizeof(silence), &written, portMAX_DELAY);
        vTaskDelay(pdMS_TO_TICKS(1));
    }
    return ESP_OK;
#elif AUDIO_CODEC_DIAG == 7
    /* TX channel enabled (DMA running) but no codec and no capture channel. */
    ESP_RETURN_ON_ERROR(bsp_power_set_vcc_3v3(true), TAG, "VCC_3V3");
    ESP_RETURN_ON_ERROR(bsp_power_set_codec_3v3(true), TAG, "CODEC_3V3");
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    chan_cfg.auto_clear = true;
    i2s_chan_handle_t tx = NULL;
    ESP_RETURN_ON_ERROR(i2s_new_channel(&chan_cfg, &tx, NULL), TAG, "new channel");
    i2s_std_config_t cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(BSP_AUDIO_DEFAULT_SAMPLE_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(BSP_AUDIO_DEFAULT_BITS_PER_SAMPLE,
                                                       I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = GPIO_NUM_1,
            .ws = GPIO_NUM_2,
            .dout = GPIO_NUM_3,
            .din = I2S_GPIO_UNUSED,
        },
    };
    ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(tx, &cfg), TAG, "init std mode");
    ESP_RETURN_ON_ERROR(i2s_channel_enable(tx), TAG, "enable tx");
    ESP_LOGI(TAG, "diagnostic: I2S TX enabled (no codec, no rx)");
    return ESP_OK;
#elif AUDIO_CODEC_DIAG == 6
    /* I2S channel created and configured, but never enabled: no DMA traffic. */
    ESP_RETURN_ON_ERROR(bsp_power_set_vcc_3v3(true), TAG, "VCC_3V3");
    ESP_RETURN_ON_ERROR(bsp_power_set_codec_3v3(true), TAG, "CODEC_3V3");
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    i2s_chan_handle_t tx = NULL;
    ESP_RETURN_ON_ERROR(i2s_new_channel(&chan_cfg, &tx, NULL), TAG, "new channel");
    i2s_std_config_t cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(BSP_AUDIO_DEFAULT_SAMPLE_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(BSP_AUDIO_DEFAULT_BITS_PER_SAMPLE,
                                                       I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = GPIO_NUM_1,
            .ws = GPIO_NUM_2,
            .dout = GPIO_NUM_3,
            .din = I2S_GPIO_UNUSED,
        },
    };
    ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(tx, &cfg), TAG, "init std mode");
    ESP_LOGI(TAG, "diagnostic: I2S configured but NOT enabled");
    return ESP_OK;
#elif AUDIO_CODEC_DIAG == 5
    /* Same driver setup but on pins that are definitely unused: separates a pin
     * conflict from an I2S-DMA conflict. */
    i2s_std_config_t cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(BSP_AUDIO_DEFAULT_SAMPLE_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(BSP_AUDIO_DEFAULT_BITS_PER_SAMPLE,
                                                       I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = GPIO_NUM_1,
            .ws = GPIO_NUM_2,
            .dout = GPIO_NUM_3,
            .din = I2S_GPIO_UNUSED,
        },
    };
    cfg.clk_cfg.mclk_multiple = I2S_MCLK_MULTIPLE_256;
    ESP_RETURN_ON_ERROR(bsp_audio_init(&cfg), TAG, "I2S init (test pins)");
    ESP_LOGI(TAG, "diagnostic: I2S on unused pins 1/2/3");
    return ESP_OK;
#elif AUDIO_CODEC_DIAG == 4
    /* Playback only and no MCLK pin: GPIO54 is the next suspect. */
    i2s_std_config_t cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(BSP_AUDIO_DEFAULT_SAMPLE_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(BSP_AUDIO_DEFAULT_BITS_PER_SAMPLE,
                                                       I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = BSP_AUDIO_I2S_SCLK,
            .ws = BSP_AUDIO_I2S_LRCLK,
            .dout = BSP_AUDIO_I2S_SDOUT,
            .din = I2S_GPIO_UNUSED,
        },
    };
    cfg.clk_cfg.mclk_multiple = I2S_MCLK_MULTIPLE_256;
    ESP_RETURN_ON_ERROR(bsp_audio_init(&cfg), TAG, "I2S init (no mclk)");
    ESP_LOGI(TAG, "diagnostic: power + I2S (tx only, no mclk)");
    return ESP_OK;
#elif AUDIO_CODEC_DIAG == 3
    /* Playback only: leave the capture pins alone. */
    i2s_std_config_t cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(BSP_AUDIO_DEFAULT_SAMPLE_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(BSP_AUDIO_DEFAULT_BITS_PER_SAMPLE,
                                                       I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = BSP_AUDIO_I2S_MCLK,
            .bclk = BSP_AUDIO_I2S_SCLK,
            .ws = BSP_AUDIO_I2S_LRCLK,
            .dout = BSP_AUDIO_I2S_SDOUT,
            .din = I2S_GPIO_UNUSED,
        },
    };
    cfg.clk_cfg.mclk_multiple = I2S_MCLK_MULTIPLE_256;
    ESP_RETURN_ON_ERROR(bsp_audio_init(&cfg), TAG, "I2S init (tx only)");
    ESP_LOGI(TAG, "diagnostic: power + I2S (tx only, no capture pins)");
    return ESP_OK;
#else
    if (s_speaker != NULL && s_speaker_rate == sample_rate) {
        return ESP_OK;
    }
    if (s_speaker != NULL) {
        /* Different rate: reopen the codec at the new one. */
        (void)esp_codec_dev_close(s_speaker);
        s_speaker = NULL;
    }
    ESP_RETURN_ON_ERROR(bsp_audio_init(NULL), TAG, "I2S init");

    s_speaker = bsp_audio_codec_speaker_init();
    ESP_RETURN_ON_FALSE(s_speaker != NULL, ESP_FAIL, TAG, "speaker init");

    esp_codec_dev_sample_info_t info = {
        .sample_rate = sample_rate,
        .bits_per_sample = 16,
        .channel = 2,
    };
    const int ret = esp_codec_dev_open(s_speaker, &info);
    ESP_RETURN_ON_FALSE(ret == ESP_CODEC_DEV_OK, ESP_FAIL, TAG, "codec open (%d)", ret);
    audio_codec_apply_volume();
    s_speaker_rate = sample_rate;
    ESP_LOGI(TAG, "speaker ready (%u Hz, 16-bit, stereo)", (unsigned)sample_rate);
    /* The mixer runs from the moment the codec exists: with no active source it
     * writes silence, which keeps the I2S DMA from replaying a stale block. */
    if (s_watchdog_started == false) {
        if (xTaskCreatePinnedToCore(codec_watchdog_task, "codec_wd", 3072, NULL, 2, NULL, 0) ==
            pdPASS) {
            s_watchdog_started = true;
        }
    }
#if APP_AUDIO_MIX_ENABLE
    if (s_mix_wake == NULL) {
        s_mix_wake = xSemaphoreCreateBinary();
    }
    if (s_mix_task == NULL) {
        if (xTaskCreatePinnedToCore(mix_task, "audio_mix", 4096, NULL, 4, NULL, 0) == pdPASS) {
            s_mix_task = (TaskHandle_t)1; /* marker: running */
        } else {
            ESP_LOGE(TAG, "cannot start the playback mixer");
        }
    }
#endif
    return ESP_OK;
#endif /* AUDIO_CODEC_DIAG */
}

esp_err_t app_audio_codec_start(void)
{
    return app_audio_codec_start_at(48000);
}

esp_err_t app_audio_codec_preopen(uint32_t sample_rate)
{
    /* Open TX and RX once at boot: the capture channel must exist before any
     * USB capture stream is configured, otherwise i2s_channel_read() fails with
     * "The channel is not enabled" for the rest of the session. */
    const esp_err_t err = app_audio_codec_start_at(sample_rate);
    if (err != ESP_OK) {
        return err;
    }
    const esp_err_t mic_err = app_audio_codec_mic_start_at(sample_rate);
    if (mic_err != ESP_OK) {
        ESP_LOGW(TAG, "microphone preopen failed: %s", esp_err_to_name(mic_err));
    }
    return ESP_OK;
}

/* Streams are tracked by the codec, not claimed by the transports. Each source
 * reports start/stop and then reads or writes unconditionally; the policy below
 * decides what is actually heard. */
typedef struct {
    app_audio_source_t source;
    bool active;
    uint32_t rate;
    int64_t last_write_us; /* last time this stream handed data over */
} codec_stream_t;

static codec_stream_t s_streams[] = {
    {APP_AUDIO_SOURCE_BT, false, 48000, 0},
    {APP_AUDIO_SOURCE_USB, false, 48000, 0},
    {APP_AUDIO_SOURCE_LOCAL, false, 48000, 0},
};
#define CODEC_STREAM_COUNT (sizeof(s_streams) / sizeof(s_streams[0]))

static codec_stream_t *codec_stream(app_audio_source_t source)
{
    for (size_t i = 0; i < CODEC_STREAM_COUNT; ++i) {
        if (s_streams[i].source == source) {
            return &s_streams[i];
        }
    }
    return NULL;
}

/* With the mixer enabled every active source is summed; with it disabled the
 * highest-priority active stream wins (Bluetooth, then USB, then local features)
 * and the others are dropped so their data never piles up. */
static app_audio_source_t playback_selected(void)
{
    for (size_t i = 0; i < CODEC_STREAM_COUNT; ++i) {
        if (s_streams[i].active) {
            return s_streams[i].source;
        }
    }
    return APP_AUDIO_SOURCE_NONE;
}

static void mix_slot_clear(app_audio_source_t source);

/* A2DP reports "suspended" seconds after the host actually pauses, so decide by
 * starvation instead: no data for 400 ms means the stream is over. */
#define CODEC_STREAM_STALE_US (400 * 1000)

static void codec_watchdog_task(void *arg)
{
    (void)arg;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(100));
        const app_audio_source_t selected = playback_selected();
        codec_stream_t *st = codec_stream(selected);
        if (st == NULL || st->last_write_us == 0) {
            continue;
        }
        if (esp_timer_get_time() - st->last_write_us > CODEC_STREAM_STALE_US) {
            ESP_LOGW(TAG, "stream %d starved, falling back", (int)selected);
            app_audio_codec_playback_stop(selected);
        }
    }
}

esp_err_t app_audio_codec_playback_start(app_audio_source_t source, uint32_t sample_rate)
{
    codec_stream_t *st = codec_stream(source);
    if (st == NULL || sample_rate == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    st->active = true;
    st->rate = sample_rate;
    const app_audio_source_t selected = playback_selected();
#if APP_AUDIO_MIX_ENABLE
    /* The mixer owns the I2S rate: sources are resampled into it. */
    (void)app_audio_codec_start_at(s_mix_rate);
#endif
    if (s_speaker != NULL && selected == APP_AUDIO_SOURCE_BT) {
    }
    /* Reapply after the (possible) reopen: reopen used to reset the volume to the
     * USB host value, which made Bluetooth quiet again. */
    audio_codec_apply_volume();
    const codec_stream_t *sel = codec_stream(selected);
#if APP_AUDIO_MIX_ENABLE
    const uint32_t rate = s_mix_rate;
#else
    const uint32_t rate = sel != NULL ? sel->rate : s_mix_rate;
#endif
    const esp_err_t err = app_audio_codec_start_at(rate);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "playback start %d Hz at %u Hz (heard: %d)", (int)source,
                 (unsigned)rate, (int)selected);
    }
    return err;
}

void app_audio_codec_playback_stop(app_audio_source_t source)
{
    codec_stream_t *st = codec_stream(source);
    if (st == NULL || !st->active) {
        return;
    }
    st->active = false;
    /* Mixer bookkeeping: drop whatever this source had queued. No-op when the
     * mixer is disabled (no slot was ever created). */
    mix_slot_clear(source);
    const app_audio_source_t selected = playback_selected();
    const codec_stream_t *sel = codec_stream(selected);
    const uint32_t rate = sel != NULL ? sel->rate : APP_AUDIO_MIX_MAX_RATE;
    if (app_audio_codec_start_at(rate) == ESP_OK) {
        if (s_speaker != NULL) {
            /* Restore the volume the USB host had set (Bluetooth had taken the
             * DAC to 100%). */
            (void)esp_codec_dev_set_out_vol(s_speaker, s_out_vol_pct);
        }
        ESP_LOGI(TAG, "playback stop %d (heard: %d)", (int)source, (int)selected);
    }
}

/* Defined with the microphone fan-out further down; queried here so transports
 * can register a consumer at the rate the I2S is actually running. */
static volatile uint32_t s_mic_src_rate;

uint32_t app_audio_codec_i2s_rate(void)
{
    /* Dynamic mixer rate: min(cap, max(rate of the active sources)). Capture
     * follows it because I2S shares one clock. */
    return s_mix_rate;
}

/* True when @p source is the one currently being heard (policy result). */
bool app_audio_codec_is_heard(app_audio_source_t source)
{
    return playback_selected() == source;
}

esp_err_t app_audio_codec_write(const void *data, size_t bytes)
{
    if (s_speaker == NULL || data == NULL || bytes == 0) {
        return ESP_ERR_INVALID_STATE;
    }
    const int ret = esp_codec_dev_write(s_speaker, (void *)data, (int)bytes);
    return ret == ESP_CODEC_DEV_OK ? ESP_OK : ESP_FAIL;
}

/* ---- playback mixing ------------------------------------------------------
 * Every transport owns a queue that stores its PCM *as received*, at its own
 * rate. When the I2S needs a block, the mixer takes the matching time span from
 * each queue, resamples it to the output rate and sums. Resampling therefore
 * happens at pull time, where the output block is known exactly.
 *
 * No drift compensation yet: a source that runs faster simply fills its queue
 * and the oldest frames are dropped.
 */

/* One slot per transport that can play at the same time: USB, Bluetooth,
 * local (BOOT loopback / test tone), plus headroom.
 * Each slot costs MIX_RING_FRAMES * 4 bytes of internal RAM. */
#define MIX_SOURCES 4
#define MIX_BLOCK_FRAMES 240 /* 5 ms at 48 kHz */
#define MIX_RING_FRAMES 2048 /* ~43 ms at 48 kHz of native-rate audio */

typedef struct {
    app_audio_source_t source;
    bool used;
    uint32_t rate;
    int16_t queue[MIX_RING_FRAMES * 2]; /* native rate, interleaved stereo */
    uint32_t head;                      /* frames pushed */
    uint32_t tail;                      /* frames consumed */
    uint32_t phase_q16;                 /* fractional position past tail */
    int32_t gain_q16;
    int64_t last_push_us;               /* last time this queue received data */
} mix_source_t;

static mix_source_t s_mix[MIX_SOURCES];
/* Running I2S rate: min(cap, max(active source rate)), recomputed whenever the
 * set of sources changes. */
static volatile uint32_t s_mix_rate = APP_AUDIO_MIX_MAX_RATE;
static portMUX_TYPE s_mix_lock = portMUX_INITIALIZER_UNLOCKED;
/* Given by every queue push so the mixer thread sleeps until there is work
 * (declared above, next to the other mixer forward declarations). */

/* A queue that has not received anything for this long is not part of the
 * "wait for all channels" set: it is paused, not slow. */
#define MIX_SOURCE_IDLE_US (150 * 1000)

static mix_source_t *mix_slot(app_audio_source_t source, bool create)
{
    for (int i = 0; i < MIX_SOURCES; ++i) {
        if (s_mix[i].used && s_mix[i].source == source) {
            return &s_mix[i];
        }
    }
    if (!create) {
        return NULL;
    }
    for (int i = 0; i < MIX_SOURCES; ++i) {
        if (!s_mix[i].used) {
            memset(&s_mix[i], 0, sizeof(s_mix[i]));
            s_mix[i].used = true;
            s_mix[i].source = source;
            s_mix[i].gain_q16 = 0x10000; /* unity until a policy changes it */
            return &s_mix[i];
        }
    }
    return NULL;
}

/* min(cap, max(active source rate)); reopen the codec when it changes. */
static void mix_rate_update(void)
{
    uint32_t target = 0;
    for (int i = 0; i < MIX_SOURCES; ++i) {
        if (s_mix[i].used && s_mix[i].rate > target) {
            target = s_mix[i].rate;
        }
    }
    if (target == 0 || target > APP_AUDIO_MIX_MAX_RATE) {
        target = APP_AUDIO_MIX_MAX_RATE;
    }
    if (target == s_mix_rate) {
        return;
    }
#if !APP_AUDIO_MIX_ENABLE
    s_mix_rate = target;
    return;
#endif
    if (app_audio_codec_start_at(target) != ESP_OK) {
        ESP_LOGE(TAG, "cannot switch I2S to %u Hz", (unsigned)target);
        return;
    }
    s_mix_rate = target;
    ESP_LOGI(TAG, "mix rate -> %u Hz", (unsigned)target);
}

static void mix_task(void *arg)
{
    (void)arg;
    static int16_t out[MIX_BLOCK_FRAMES * 2];
    /* Prebuffer before the first block (and after a pause) so the DMA never
     * runs dry; then emit fixed 5 ms blocks. */
#define MIX_PREROLL_FRAMES 960 /* 20 ms at 48 kHz */
    bool running = false;

    for (;;) {
        const uint32_t rate = s_mix_rate;
        uint32_t block_max = rate / 200; /* 5 ms */
        if (block_max > MIX_BLOCK_FRAMES) {
            block_max = MIX_BLOCK_FRAMES;
        }

        /* How much can every live queue contribute right now? */
        uint32_t producible = UINT32_MAX;
        bool any = false;
        taskENTER_CRITICAL(&s_mix_lock);
        for (int s = 0; s < MIX_SOURCES; ++s) {
            mix_source_t *m = &s_mix[s];
            if (!m->used || m->rate == 0) {
                continue;
            }
            const uint32_t step = (uint32_t)(((uint64_t)m->rate << 16) / rate);
            if (step == 0) {
                continue;
            }
            const int64_t now_us = esp_timer_get_time();
            if (m->last_push_us != 0 && now_us - m->last_push_us > MIX_SOURCE_IDLE_US) {
                continue; /* paused stream: must not cap the block */
            }
            any = true;
            const uint32_t avail = m->head - m->tail;
            const uint32_t can = avail < 2 ? 0 : (uint32_t)(((uint64_t)(avail - 1) << 16) / step);
            if (can < producible) {
                producible = can;
            }
        }
        taskEXIT_CRITICAL(&s_mix_lock);

        if (!any) {
            /* Nothing active: keep the DMA fed with silence. */
            memset(out, 0, block_max * 4);
            (void)app_audio_codec_write(out, block_max * 4);
            xSemaphoreTake(s_mix_wake, pdMS_TO_TICKS(10));
            running = false;
            continue;
        }

        if (!running && producible < MIX_PREROLL_FRAMES) {
            /* Fill the cushion first; the DMA is still playing what we wrote. */
            xSemaphoreTake(s_mix_wake, pdMS_TO_TICKS(20));
            continue;
        }
        if (producible < block_max) {
            /* Low water: wait for the next push instead of emitting a tiny block. */
            if (xSemaphoreTake(s_mix_wake, pdMS_TO_TICKS(10)) == pdFALSE) {
                memset(out, 0, block_max * 4);
                (void)app_audio_codec_write(out, block_max * 4);
                running = false;
            }
            continue;
        }
        running = true;

        const uint32_t block = block_max;
        memset(out, 0, block * 4);
        taskENTER_CRITICAL(&s_mix_lock);
        for (int s = 0; s < MIX_SOURCES; ++s) {
            mix_source_t *m = &s_mix[s];
            if (!m->used || m->rate == 0) {
                continue;
            }
            const uint32_t step = (uint32_t)(((uint64_t)m->rate << 16) / rate);
            if (step == 0) {
                continue;
            }
            const uint32_t need = (((block - 1) * step) >> 16) + 2;
            if (m->head - m->tail < need) {
                continue; /* this source is silent for this block */
            }
            uint32_t pos = m->phase_q16;
            for (uint32_t i = 0; i < block; ++i) {
                const uint32_t idx = pos >> 16;
                const uint32_t frac = pos & 0xFFFFu;
                const uint32_t i0 = (m->tail + idx) % MIX_RING_FRAMES;
                const uint32_t i1 = (m->tail + idx + 1) % MIX_RING_FRAMES;
                const int32_t l0 = m->queue[i0 * 2];
                const int32_t l1 = m->queue[i1 * 2];
                const int32_t r0 = m->queue[i0 * 2 + 1];
                const int32_t r1 = m->queue[i1 * 2 + 1];
                const int32_t ls = l0 + (((l1 - l0) * (int32_t)frac) >> 16);
                const int32_t rs = r0 + (((r1 - r0) * (int32_t)frac) >> 16);
                int32_t l = out[i * 2] + (ls * m->gain_q16) / 0x10000;
                int32_t r = out[i * 2 + 1] + (rs * m->gain_q16) / 0x10000;
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
            m->tail += pos >> 16;
            m->phase_q16 = pos & 0xFFFFu;
        }
        taskEXIT_CRITICAL(&s_mix_lock);
        if (app_audio_codec_write(out, block * 4) != ESP_OK) {
            vTaskDelay(pdMS_TO_TICKS(5));
        }
    }
}

esp_err_t app_audio_codec_playback_write(app_audio_source_t source, uint32_t sample_rate,
                                         const void *data, size_t bytes)
{
    if (data == NULL || bytes < 4 || sample_rate == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_speaker == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    codec_stream_t *live = codec_stream(source);
    if (live != NULL) {
        if (!live->active) {
            /* Data is itself a start notification: an A2DP resume after a pause
             * does not emit a new audio-state event, so waiting for one would
             * keep this stream silent forever. */
            (void)app_audio_codec_playback_start(source, sample_rate);
        }
        live->last_write_us = esp_timer_get_time();
    }
#if !APP_AUDIO_MIX_ENABLE
    /* Single-stream policy: the highest-priority active source is heard, the
     * others still hand their data over and it is dropped here. */
    if (!app_audio_codec_is_heard(source)) {
        return ESP_OK;
    }
    const codec_stream_t *st = codec_stream(source);
    const uint32_t want = st != NULL ? st->rate : sample_rate;
    if (s_speaker_rate != want) {
        if (app_audio_codec_start_at(want) != ESP_OK) {
            return ESP_FAIL;
        }
    }
#if APP_AUDIO_BT_GAIN_Q16 != 0x10000
    if (source == APP_AUDIO_SOURCE_BT) {
        /* The peer sets its own loudness, but SBC decodes below full scale and
         * the ES8311 output is shared with USB; lift Bluetooth digitally so both
         * end up at a comparable level. Saturating, never wrapping. */
        static int16_t boosted[480 * 2];
        const int16_t *in = (const int16_t *)data;
        const size_t frames = bytes / 4;
        size_t done = 0;
        while (done < frames) {
            size_t n = frames - done;
            if (n > 480) {
                n = 480;
            }
            for (size_t i = 0; i < n; ++i) {
                int32_t l = ((int32_t)in[(done + i) * 2] * APP_AUDIO_BT_GAIN_Q16) >> 16;
                int32_t r = ((int32_t)in[(done + i) * 2 + 1] * APP_AUDIO_BT_GAIN_Q16) >> 16;
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
                boosted[i * 2] = (int16_t)l;
                boosted[i * 2 + 1] = (int16_t)r;
            }
            if (app_audio_codec_write(boosted, n * 4) != ESP_OK) {
                return ESP_FAIL;
            }
            done += n;
        }
        return ESP_OK;
    }
#endif
    return app_audio_codec_write(data, bytes);
#else
    mix_source_t *m = mix_slot(source, true);
    if (m == NULL) {
        return ESP_ERR_NO_MEM;
    }
    m->rate = sample_rate;
    mix_rate_update();

    /* Queue the PCM exactly as received: resampling happens when the mixer
     * pulls, where the output block and its time span are known. */
    const int16_t *in = (const int16_t *)data;
    const size_t in_frames = bytes / 4; /* stereo 16-bit */
    taskENTER_CRITICAL(&s_mix_lock);
    for (size_t i = 0; i < in_frames; ++i) {
        if (m->head - m->tail >= MIX_RING_FRAMES) {
            /* Overrun: keep the newest audio and drop the oldest. */
            m->tail = m->head - MIX_RING_FRAMES + 1;
            m->phase_q16 = 0;
        }
        const uint32_t pos = m->head % MIX_RING_FRAMES;
        m->queue[pos * 2] = in[i * 2];
        m->queue[pos * 2 + 1] = in[i * 2 + 1];
        ++m->head;
    }
    m->last_push_us = esp_timer_get_time();
    taskEXIT_CRITICAL(&s_mix_lock);
    if (s_mix_wake != NULL) {
        xSemaphoreGive(s_mix_wake);
    }
    return ESP_OK;
#endif /* APP_AUDIO_MIX_ENABLE */
}

esp_err_t app_audio_codec_set_gain_q16(app_audio_source_t source, int32_t gain_q16)
{
    mix_source_t *m = mix_slot(source, false);
    if (m == NULL) {
        return ESP_ERR_NOT_FOUND;
    }
    if (gain_q16 < 0) {
        gain_q16 = 0;
    } else if (gain_q16 > 0x10000) {
        gain_q16 = 0x10000;
    }
    m->gain_q16 = gain_q16;
    return ESP_OK;
}

/* Mixer bookkeeping when a source stops; the public wrapper below calls it. */
static void mix_slot_clear(app_audio_source_t source)
{
    mix_source_t *m = mix_slot(source, false);
    if (m == NULL) {
        return;
    }
    taskENTER_CRITICAL(&s_mix_lock);
    m->head = 0;
    m->tail = 0;
    m->phase_q16 = 0;
    m->used = false;
    taskEXIT_CRITICAL(&s_mix_lock);
    mix_rate_update();
}


esp_err_t app_audio_codec_mic_start_at(uint32_t sample_rate)
{
    if (s_microphone != NULL && s_mic_rate == sample_rate) {
        return ESP_OK;
    }
    if (s_microphone != NULL) {
        (void)esp_codec_dev_close(s_microphone);
        s_microphone = NULL;
    }
    s_microphone = bsp_audio_codec_microphone_init();
    ESP_RETURN_ON_FALSE(s_microphone != NULL, ESP_FAIL, TAG, "microphone init");

    esp_codec_dev_sample_info_t info = {
        .sample_rate = sample_rate,
        .bits_per_sample = 16,
        .channel = 2,
    };
    const int ret = esp_codec_dev_open(s_microphone, &info);
    ESP_RETURN_ON_FALSE(ret == ESP_CODEC_DEV_OK, ESP_FAIL, TAG, "mic open (%d)", ret);
    (void)esp_codec_dev_set_in_gain(s_microphone, 36.0f);
    s_mic_rate = sample_rate;
    ESP_LOGI(TAG, "microphone ready (%u Hz, 16-bit, stereo)", (unsigned)sample_rate);
    return ESP_OK;
}

esp_err_t app_audio_codec_mic_start(void)
{
    return app_audio_codec_mic_start_at(48000);
}

esp_err_t app_audio_codec_mic_read(void *buffer, size_t bytes, size_t *bytes_read)
{
    if (s_microphone == NULL || buffer == NULL || bytes_read == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    const int ret = esp_codec_dev_read(s_microphone, buffer, (int)bytes);
    if (ret != ESP_CODEC_DEV_OK) {
        return ESP_FAIL;
    }
    *bytes_read = bytes;
    return ESP_OK;
}

esp_err_t app_audio_codec_set_mic_gain_db(float gain_db)
{
    if (s_microphone == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    const int ret = esp_codec_dev_set_in_gain(s_microphone, gain_db);
    return ret == ESP_CODEC_DEV_OK ? ESP_OK : ESP_FAIL;
}

void app_audio_codec_mic_stop(void)
{
    if (s_microphone != NULL) {
        (void)esp_codec_dev_close(s_microphone);
        s_microphone = NULL;
    }
}

esp_err_t app_audio_codec_set_volume(int percent)
{
    if (s_speaker == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (percent < 0) {
        percent = 0;
    } else if (percent > 100) {
        percent = 100;
    }
    s_out_vol_pct = percent; /* reapplied whenever the codec is reopened */
    const int ret = esp_codec_dev_set_out_vol(s_speaker, percent);
    return ret == ESP_CODEC_DEV_OK ? ESP_OK : ESP_FAIL;
}

/* ---- microphone fan-out ---------------------------------------------------
 * A single capture task reads the I2S microphone at MIC_FANOUT_RATE and mixes it
 * down to mono, then publishes mono frames into a shared ring. Consumers keep an
 * independent cursor: same-rate consumers read the ring directly (no copy),
 * other rates decimate into their own buffer.
 */

/* Capture and playback share one I2S clock, so the microphone must run at the
 * same rate as the current speaker stream (s_mic_src_rate is declared above).
 * Consumers resample away from it. */
#define MIC_FANOUT_RING 960 /* 20 ms at 48 kHz, 44 ms at 22 kHz: plenty */
#define MIC_FANOUT_BATCH 240 /* stereo frames per capture read */

static int16_t s_mic_ring[MIC_FANOUT_RING];
static volatile uint32_t s_mic_head; /* mono frames published so far */
static int16_t s_mic_stereo[MIC_FANOUT_BATCH * 2];
static volatile int s_mic_consumers;
static TaskHandle_t s_mic_task;

static void mic_fanout_task(void *arg)
{
    (void)arg;
    while (s_mic_consumers > 0) {
        /* Capture follows the dynamic mixer rate: I2S shares one clock. */
        const uint32_t want = s_mix_rate;
        if (s_mic_src_rate != want) {
            if (app_audio_codec_mic_start_at(want) != ESP_OK) {
                vTaskDelay(pdMS_TO_TICKS(20));
                continue;
            }
            s_mic_src_rate = want;
            ESP_LOGI(TAG, "microphone running at %u Hz", (unsigned)want);
        }
        size_t got = 0;
        if (app_audio_codec_mic_read(s_mic_stereo, sizeof(s_mic_stereo), &got) != ESP_OK ||
            got < 4) {
            vTaskDelay(pdMS_TO_TICKS(2));
            continue;
        }
        const int16_t *stereo = s_mic_stereo;
        const size_t frames = got / 4;
        /* The ES8311 runs in ADC mode with a single channel, so the I2S frame
         * carries one mono signal. Take the left slot instead of averaging: if
         * the codec leaves the right slot at zero, (L+R)/2 would cost 6 dB. */
        for (size_t i = 0; i < frames; ++i) {
            s_mic_ring[(s_mic_head + i) % MIC_FANOUT_RING] = stereo[i * 2];
        }
        s_mic_head += (uint32_t)frames; /* single producer, publish last */
    }
    app_audio_codec_mic_stop();
    s_mic_src_rate = 0;
    s_mic_task = NULL;
    vTaskDelete(NULL);
}

esp_err_t app_audio_codec_mic_consumer_add(app_audio_mic_consumer_t *consumer,
                                           app_audio_source_t source, uint32_t sample_rate)
{
    if (consumer == NULL || sample_rate == 0) {
        return ESP_ERR_INVALID_ARG;
    }
#if !APP_AUDIO_MIC_FANOUT_ENABLE
    /* Direct mode: the consumer owns the capture path, no shared ring. */
    consumer->source = source;
    consumer->sample_rate = sample_rate;
    consumer->pos_q16 = 0;
    return app_audio_codec_mic_start_at(sample_rate);
#else
    consumer->source = source;
    consumer->sample_rate = sample_rate;
    consumer->pos_q16 = s_mic_head << 16;
    ++s_mic_consumers;
    if (s_mic_task == NULL) {
        if (xTaskCreatePinnedToCore(mic_fanout_task, "mic_fan", 4096, NULL, 3, NULL, 0) !=
            pdPASS) {
            --s_mic_consumers;
            return ESP_ERR_NO_MEM;
        }
        ESP_LOGI(TAG, "microphone fan-out started, %u Hz to %u Hz",
                 (unsigned)(s_speaker_rate != 0 ? s_speaker_rate : 48000),
                 (unsigned)sample_rate);
    }
    return ESP_OK;
#endif /* APP_AUDIO_MIC_FANOUT_ENABLE */
}

void app_audio_codec_mic_consumer_remove(app_audio_mic_consumer_t *consumer)
{
#if !APP_AUDIO_MIC_FANOUT_ENABLE
    (void)consumer;
    app_audio_codec_mic_stop();
#else
    if (consumer != NULL && s_mic_consumers > 0) {
        --s_mic_consumers;
    }
#endif
}

size_t app_audio_codec_mic_consumer_read(app_audio_mic_consumer_t *consumer,
                                         int16_t *mono_out, size_t frames)
{
    if (consumer == NULL || mono_out == NULL || frames == 0) {
        return 0;
    }
#if !APP_AUDIO_MIC_FANOUT_ENABLE
    /* Direct mode: read the I2S microphone, keep the left channel (the ES8311
     * ADC is mono). Chunked so a large request never needs a big stack buffer. */
    static int16_t stereo[240 * 2];
    const size_t chunk_max = sizeof(stereo) / (2 * sizeof(int16_t));
    size_t done = 0;
    while (done < frames) {
        size_t chunk = frames - done;
        if (chunk > chunk_max) {
            chunk = chunk_max;
        }
        size_t got = 0;
        if (app_audio_codec_mic_read(stereo, chunk * 4, &got) != ESP_OK || got < 4) {
            break;
        }
        const size_t n = got / 4;
        for (size_t i = 0; i < n; ++i) {
            mono_out[done + i] = stereo[i * 2];
        }
        done += n;
        if (n < chunk) {
            break;
        }
    }
    if (done < frames) {
        memset(&mono_out[done], 0, (frames - done) * sizeof(int16_t));
    }
    return frames;
#else
    const uint32_t src = s_mic_src_rate;
    const uint32_t head = s_mic_head;
    if (src == 0 || head < 2) {
        memset(mono_out, 0, frames * sizeof(int16_t));
        return frames;
    }

    /* Keep the consumer inside the ring: replaying stale audio sounds worse than
     * skipping it. */
    const uint32_t floor_frame = consumer->pos_q16 >> 16;
    if (head > floor_frame && head - floor_frame > MIC_FANOUT_RING) {
        consumer->pos_q16 = (head - MIC_FANOUT_RING) << 16;
    }

    /* Position advance per output frame, Q16. Linear interpolation in between
     * handles any ratio, integer (48k->16k) or fractional (44.1k->48k). */
    const uint32_t step_q16 = (src << 16) / consumer->sample_rate;

    for (size_t i = 0; i < frames; ++i) {
        const uint32_t pos = consumer->pos_q16 >> 16;
        const uint32_t frac = consumer->pos_q16 & 0xFFFFu;
        if (pos + 1 >= head) {
            mono_out[i] = 0; /* not enough fresh frames yet: keep the cadence */
        } else {
            const int32_t s0 = s_mic_ring[pos % MIC_FANOUT_RING];
            const int32_t s1 = s_mic_ring[(pos + 1) % MIC_FANOUT_RING];
            mono_out[i] = (int16_t)(s0 + (((s1 - s0) * (int32_t)frac) >> 16));
        }
        consumer->pos_q16 += step_q16;
    }
    return frames;
#endif /* APP_AUDIO_MIC_FANOUT_ENABLE */
}
