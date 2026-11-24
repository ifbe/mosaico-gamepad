// SPDX-License-Identifier: Apache-2.0

#include "app_audio_codec.h"

#include "app_audio_core.h"

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
/* Microphone fan-out: 1 = one capture task feeds all consumers through a shared
 * ring; 0 = each consumer reads the I2S microphone directly (single consumer at
 * a time). Kept as a switch while the capture path is being debugged. */
/* Digital gain applied to Bluetooth PCM (Q16). 0x20000 = +6 dB; the peer's own
 * volume stays authoritative, this only compensates for SBC decode level. */
#ifndef APP_AUDIO_BT_GAIN_Q16
#define APP_AUDIO_BT_GAIN_Q16 0x18000
#endif


/* Dynamic I2S rate = min(cap, max(active playback source rate)). Declared here
 * because both the transport API and the mixer use it. Starts at the fallback
 * rate: with nothing playing the mixer must keep the codec open, not reopen it
 * at 0 Hz (which the I2S driver rejects and leaves the capture side conflicting). */
static volatile uint32_t s_mix_rate = APP_AUDIO_MIX_MAX_RATE;
/* Defined with the playback mixer further down; the speaker bring-up starts it. */
static void i2s_out_task(void *arg);
static TaskHandle_t s_mix_task;
static void codec_watchdog_task(void *arg);
static bool s_watchdog_started;
/* Output volume in percent. USB sets it from the host; Bluetooth has no volume
 * channel, so while a peer is heard the DAC runs at 100% and the peer's own
 * volume control stays authoritative. */
static int s_out_vol_pct = 80;
static app_audio_source_t playback_selected(void);

static void audio_codec_apply_volume(void);
static bool is_bt_source(app_audio_source_t source);

static esp_codec_dev_handle_t s_speaker;
static uint32_t s_speaker_rate;
static bool s_slots_gain_ready;
static esp_codec_dev_handle_t s_microphone;
static uint32_t s_mic_rate;

/* Single place that decides the DAC volume, so a codec reopen can never undo it. */
static void audio_codec_apply_volume(void)
{
    if (s_speaker == NULL) {
        return;
    }
    const int pct = is_bt_source(playback_selected()) ? 100 : s_out_vol_pct;
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
    /* Level compensation per transport, applied by the mixer when it pulls.
     * Bluetooth decodes a few dB below full scale; USB and local features do
     * not. Producers always queue raw PCM. */
    if (s_slots_gain_ready == false) {
        app_audio_core_set_gain_q16(APP_AUDIO_SLOT_A2DP, APP_AUDIO_BT_GAIN_Q16);
        app_audio_core_set_gain_q16(APP_AUDIO_SLOT_HFP, APP_AUDIO_BT_GAIN_Q16);
        app_audio_core_set_gain_q16(APP_AUDIO_SLOT_LE, APP_AUDIO_BT_GAIN_Q16);
        app_audio_core_set_gain_q16(APP_AUDIO_SLOT_USB, 0x10000);
        app_audio_core_set_gain_q16(APP_AUDIO_SLOT_LOCAL, 0x10000);
        s_slots_gain_ready = true;
    }
    s_speaker_rate = sample_rate;
    /* The speaker and the microphone share one I2S port. Opening the speaker
     * (re)configures that port, so the capture side must be reopened as well -
     * otherwise RX stays disabled and every read fails with "channel is not
     * enabled". Forcing the rate to 0 defeats the "same rate, nothing to do"
     * shortcut in app_audio_codec_mic_start_at(). */
    if (s_microphone != NULL) {
        (void)esp_codec_dev_close(s_microphone);
        s_microphone = NULL;
        s_mic_rate = 0;
        const esp_err_t mic_err = app_audio_codec_mic_start_at(sample_rate);
        if (mic_err != ESP_OK) {
            ESP_LOGW(TAG, "capture reopen after speaker open failed: %s",
                     esp_err_to_name(mic_err));
        }
    }
    ESP_LOGI(TAG, "speaker ready (%u Hz, 16-bit, stereo)", (unsigned)sample_rate);
    /* The mixer runs from the moment the codec exists: with no active source it
     * writes silence, which keeps the I2S DMA from replaying a stale block. */
    if (s_watchdog_started == false) {
        if (xTaskCreatePinnedToCore(codec_watchdog_task, "codec_wd", 3072, NULL, 2, NULL, 0) ==
            pdPASS) {
            s_watchdog_started = true;
        }
    }
    if (s_mix_task == NULL) {
        if (xTaskCreatePinnedToCore(i2s_out_task, "i2s_out", 4096, NULL, 4, NULL, 0) == pdPASS) {
            s_mix_task = (TaskHandle_t)1; /* marker: running */
        } else {
            ESP_LOGE(TAG, "cannot start the i2s output task");
        }
    }
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

static bool is_bt_source(app_audio_source_t source)
{
    return source == APP_AUDIO_SOURCE_BT_A2DP || source == APP_AUDIO_SOURCE_BT_HFP ||
           source == APP_AUDIO_SOURCE_BT_LE;
}

/* Priority order: a call wins over music, then LE, then USB, then local. */
static codec_stream_t s_streams[] = {
    {APP_AUDIO_SOURCE_BT_HFP, false, 48000, 0},
    {APP_AUDIO_SOURCE_BT_A2DP, false, 48000, 0},
    {APP_AUDIO_SOURCE_BT_LE, false, 48000, 0},
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
    audio_codec_apply_volume();
    /* The mixer thread owns the I2S rate and every codec reopen: doing it here
     * would block the protocol callback that reported the stream. */
    ESP_LOGI(TAG, "playback start source %d (mix at %u Hz, heard: %d)", (int)source,
             (unsigned)s_mix_rate, (int)selected);
    return ESP_OK;
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
    if (s_speaker != NULL) {
        /* Restore the volume the USB host had set (Bluetooth had taken the DAC
         * to 100%). The mixer thread handles any rate change. */
        (void)esp_codec_dev_set_out_vol(s_speaker, s_out_vol_pct);
    }
    ESP_LOGI(TAG, "playback stop %d (heard: %d)", (int)source,
             (int)playback_selected());
}

/* Defined with the microphone fan-out further down; queried here so transports
 * can register a consumer at the rate the I2S is actually running. */
static volatile uint32_t s_mic_src_rate;

static bool s_enabled = true;

void app_audio_codec_set_enabled(bool enabled)
{
    s_enabled = enabled;
    ESP_LOGI(TAG, "audio processing %s", enabled ? "on" : "off");
}

bool app_audio_codec_enabled(void)
{
    return s_enabled;
}

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

/* ---- I2S output consumer ---------------------------------------------------
 * This task is the playback consumer itself (not an extra mixing layer): it
 * asks app_audio_core for a mixed block and writes it to the I2S. The queues
 * live in app_audio_core; this thread is their single consumer:
 * it pulls every active slot (already resampled to the I2S rate and scaled by
 * the slot gain), sums them and writes one block. Producers (transport
 * callbacks) only push and notify, so no callback is ever blocked by the DMA.
 */

/* A 5 ms block at 48 kHz. */
#define MIX_BLOCK_FRAMES 240
/* A queue that has not received anything for this long is treated as paused, so
 * its empty queue does not hold the block back. */
#define MIX_SOURCE_IDLE_US (150 * 1000)
/* Sum into this buffer, then saturate. Static: the mixer task stack is small. */
static int16_t s_mix_out[MIX_BLOCK_FRAMES * 2];

static int slot_for_source(app_audio_source_t source)
{
    switch (source) {
    case APP_AUDIO_SOURCE_BT_A2DP:
        return APP_AUDIO_SLOT_A2DP;
    case APP_AUDIO_SOURCE_BT_HFP:
        return APP_AUDIO_SLOT_HFP;
    case APP_AUDIO_SOURCE_BT_LE:
        return APP_AUDIO_SLOT_LE;
    case APP_AUDIO_SOURCE_USB:
        return APP_AUDIO_SLOT_USB;
    case APP_AUDIO_SOURCE_LOCAL:
        return APP_AUDIO_SLOT_LOCAL;
    default:
        return -1;
    }
}

/* Highest rate among the slots that are actually running; the I2S follows it. */
static void mix_rate_update(void)
{
    uint32_t target = 0;
    for (int i = 0; i < APP_AUDIO_SLOT_COUNT; ++i) {
        const uint32_t rate = app_audio_core_slot_rate(i);
        if (rate > target) {
            target = rate;
        }
    }
    if (target == 0 || target > APP_AUDIO_MIX_MAX_RATE) {
        target = APP_AUDIO_MIX_MAX_RATE;
    }
    if (target < APP_AUDIO_MIX_MIN_RATE) {
        /* See APP_AUDIO_MIX_MIN_RATE: the capture clock must stay in step with
         * what the microphone ring declares. */
        target = APP_AUDIO_MIX_MIN_RATE;
    }
    if (target != s_mix_rate) {
        s_mix_rate = target;
        ESP_LOGI(TAG, "mix rate -> %u Hz", (unsigned)target);
    }
}

/* Statistics of the playback consumer: one line every 10 s. */
#define I2S_OUT_STATS_US (10 * 1000 * 1000)

static void i2s_out_stats(uint32_t rate, uint32_t mask, uint32_t frames, int32_t peak,
                          uint32_t errors, uint32_t mix_calls, const uint32_t *starved)
{
    ESP_LOGI(TAG, "i2s_out: rate=%u mask=0x%02x frames=%u peak=%d wr_err=%u q=[%u,%u,%u,%u,%u]",
             (unsigned)rate, (unsigned)mask, (unsigned)frames, (int)peak, (unsigned)errors,
             (unsigned)app_audio_core_slot_queued(APP_AUDIO_SLOT_A2DP),
             (unsigned)app_audio_core_slot_queued(APP_AUDIO_SLOT_HFP),
             (unsigned)app_audio_core_slot_queued(APP_AUDIO_SLOT_LE),
             (unsigned)app_audio_core_slot_queued(APP_AUDIO_SLOT_USB),
             (unsigned)app_audio_core_slot_queued(APP_AUDIO_SLOT_LOCAL));
    uint32_t gaps = 0;
    for (int i = 0; i < APP_AUDIO_SLOT_COUNT; ++i) {
        gaps += starved[i];
    }
    ESP_LOGI(TAG, "i2s_out: pulled=[%u,%u,%u,%u,%u] starved=[%u,%u,%u,%u,%u] gaps=%u calls=%u",
             (unsigned)app_audio_core_slot_take_delivered(APP_AUDIO_SLOT_A2DP),
             (unsigned)app_audio_core_slot_take_delivered(APP_AUDIO_SLOT_HFP),
             (unsigned)app_audio_core_slot_take_delivered(APP_AUDIO_SLOT_LE),
             (unsigned)app_audio_core_slot_take_delivered(APP_AUDIO_SLOT_USB),
             (unsigned)app_audio_core_slot_take_delivered(APP_AUDIO_SLOT_LOCAL),
             (unsigned)starved[0], (unsigned)starved[1], (unsigned)starved[2],
             (unsigned)starved[3], (unsigned)starved[4], (unsigned)gaps,
             (unsigned)mix_calls);
}

static void i2s_out_task(void *arg)
{
    (void)arg;
    s_mix_task = xTaskGetCurrentTaskHandle();
    app_audio_core_set_playback_wake(s_mix_task);
    ESP_LOGI(TAG, "i2s_out consumer started (mix %u Hz)", (unsigned)s_mix_rate);

    int64_t window_us = esp_timer_get_time();
    uint32_t frames = 0, errors = 0, calls = 0;
    int32_t peak = 0;
    uint32_t last_mask = 0;
    /* Queue level extremes and underrun blocks, per slot, for the window. */
    uint32_t q_min[APP_AUDIO_SLOT_COUNT];
    uint32_t q_max[APP_AUDIO_SLOT_COUNT];
    for (int i = 0; i < APP_AUDIO_SLOT_COUNT; ++i) {
        q_min[i] = UINT32_MAX;
        q_max[i] = 0;
    }

    for (;;) {
        uint32_t rate = s_mix_rate;
        if (rate == 0) {
            /* Never reopen the codec at 0 Hz: the driver rejects the clock and
             * the capture channel ends up on a different rate than playback. */
            rate = s_speaker_rate != 0 ? s_speaker_rate : APP_AUDIO_MIX_MAX_RATE;
        }
        if (s_speaker_rate != rate && app_audio_codec_start_at(rate) == ESP_OK) {
            ESP_LOGI(TAG, "I2S rate -> %u Hz", (unsigned)rate);
        }

        uint32_t block = rate / 200; /* 5 ms */
        if (block > MIX_BLOCK_FRAMES) {
            block = MIX_BLOCK_FRAMES;
        }
        if (block == 0) {
            block = 1;
        }

        /* Policy: every active link participates in the mix. app_audio_core
         * mixes (and fills silence for the links that are still buffering), so
         * the block can go straight to the I2S. */
        uint32_t mask = 0;
        for (int slot = 0; slot < APP_AUDIO_SLOT_COUNT; ++slot) {
            if (app_audio_core_slot_active(slot)) {
                mask |= APP_AUDIO_CORE_SLOT(slot);
            }
        }
        (void)app_audio_core_mix(mask, rate, 2, s_mix_out, block);
        if (app_audio_codec_write(s_mix_out, block * 4) != ESP_OK) {
            ++errors;
        }
        /* Record tap: the same mixed block the speaker hears. */
        app_audio_core_record_push(s_mix_out, block);

        frames += (uint32_t)block;
        last_mask = mask;
        ++calls;
        /* Queue level right after the mix: the low-water mark is what decides
         * whether this block had enough audio or had to be filled with silence. */
        for (int slot = 0; slot < APP_AUDIO_SLOT_COUNT; ++slot) {
            if ((mask & APP_AUDIO_CORE_SLOT(slot)) == 0) {
                continue;
            }
            const uint32_t q = (uint32_t)app_audio_core_slot_queued(slot);
            if (q < q_min[slot]) {
                q_min[slot] = q;
            }
            if (q > q_max[slot]) {
                q_max[slot] = q;
            }
        }
        for (size_t i = 0; i < block * 2; ++i) {
            const int32_t v = s_mix_out[i];
            const int32_t a = v < 0 ? -v : v;
            if (a > peak) {
                peak = a;
            }
        }
        const int64_t now_us = esp_timer_get_time();
        if (now_us - window_us >= I2S_OUT_STATS_US) {
            const uint32_t starved[APP_AUDIO_SLOT_COUNT] = {
                app_audio_core_slot_take_starved(APP_AUDIO_SLOT_A2DP),
                app_audio_core_slot_take_starved(APP_AUDIO_SLOT_HFP),
                app_audio_core_slot_take_starved(APP_AUDIO_SLOT_LE),
                app_audio_core_slot_take_starved(APP_AUDIO_SLOT_USB),
                app_audio_core_slot_take_starved(APP_AUDIO_SLOT_LOCAL),
            };
            ESP_LOGI(TAG, "i2s_out: q_min=[%u,%u,%u,%u,%u] q_max=[%u,%u,%u,%u,%u]",
                     (unsigned)(q_min[0] == UINT32_MAX ? 0 : q_min[0]),
                     (unsigned)(q_min[1] == UINT32_MAX ? 0 : q_min[1]),
                     (unsigned)(q_min[2] == UINT32_MAX ? 0 : q_min[2]),
                     (unsigned)(q_min[3] == UINT32_MAX ? 0 : q_min[3]),
                     (unsigned)(q_min[4] == UINT32_MAX ? 0 : q_min[4]),
                     (unsigned)q_max[0], (unsigned)q_max[1], (unsigned)q_max[2],
                     (unsigned)q_max[3], (unsigned)q_max[4]);
            for (int i = 0; i < APP_AUDIO_SLOT_COUNT; ++i) {
                q_min[i] = UINT32_MAX;
                q_max[i] = 0;
            }
            i2s_out_stats(rate, last_mask, frames, peak, errors, calls, starved);
            window_us = now_us;
            frames = 0;
            errors = 0;
            calls = 0;
            peak = 0;
        }
        /* Wake on a push, but never sleep longer than one block. */
        (void)ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(5));
    }
}

esp_err_t app_audio_codec_playback_write(app_audio_source_t source, uint32_t sample_rate,
                                         uint8_t channels, const void *data, size_t frames)
{
    if (data == NULL || frames == 0 || sample_rate == 0 || (channels != 1 && channels != 2)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_speaker == NULL) {
        /* Diagnostic (gamepad): a push before the speaker exists is dropped
         * silently, which looks exactly like "the transport sent nothing". */
        static int64_t last_warn_us;
        const int64_t now_us = esp_timer_get_time();
        if (now_us - last_warn_us > 2 * 1000 * 1000) {
            last_warn_us = now_us;
            ESP_LOGW(TAG, "push source %d dropped: codec not started yet", (int)source);
        }
        return ESP_ERR_INVALID_STATE;
    }
    if (!s_enabled) {
        return ESP_OK; /* muted by the settings page */
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

    const int slot = slot_for_source(source);
    if (slot < 0) {
        return ESP_ERR_INVALID_ARG;
    }
    /* Raw PCM goes into the ring: level policy lives in the consumer (per-slot
     * gain), so changing it never requires re-queuing audio. */
    const esp_err_t err = app_audio_core_playback_push(slot, sample_rate, channels,
                                                       (const int16_t *)data, frames);
    if (err == ESP_OK) {
        mix_rate_update();
    } else {
        /* A rejected push used to be invisible: the transports drop the return
         * value, so the path went silent without a single log line. */
        static int64_t last_warn_us;
        const int64_t now_us = esp_timer_get_time();
        if (now_us - last_warn_us > 5 * 1000 * 1000) {
            last_warn_us = now_us;
            ESP_LOGW(TAG, "playback queue rejected source %d: %s", (int)source,
                     esp_err_to_name(err));
        }
    }
    return err;
}

esp_err_t app_audio_codec_set_gain_q16(app_audio_source_t source, int32_t gain_q16)
{
    const int slot = slot_for_source(source);
    if (slot < 0) {
        return ESP_ERR_NOT_FOUND;
    }
    app_audio_core_set_gain_q16(slot, gain_q16);
    return ESP_OK;
}

/* Drop whatever a source had queued when it stops. */
static void mix_slot_clear(app_audio_source_t source)
{
    const int slot = slot_for_source(source);
    if (slot >= 0) {
        app_audio_core_playback_drop(slot);
        mix_rate_update();
    }
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
#define MIC_FANOUT_BATCH 240 /* stereo frames per capture read */

static int16_t s_mic_stereo[MIC_FANOUT_BATCH * 2];
static int16_t s_mic_mono[MIC_FANOUT_BATCH];
static volatile int s_mic_consumers;
static TaskHandle_t s_mic_task;
/* Capture rate diagnostics: published mono frames over a 5 s window. */
static uint32_t published;
static uint32_t window_ms;

static void i2s_in_task(void *arg)
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
            app_audio_core_mic_set_rate(want);
            ESP_LOGI(TAG, "microphone running at %u Hz", (unsigned)want);
        }
        size_t got = 0;
        if (app_audio_codec_mic_read(s_mic_stereo, sizeof(s_mic_stereo), &got) != ESP_OK) {
            /* The I2S RX side can be left disabled when the port is
             * reconfigured: force a real reopen instead of spinning on a dead
             * channel (the rate shortcut would otherwise return OK forever). */
            s_mic_rate = 0;
            (void)app_audio_codec_mic_start_at(s_mic_src_rate != 0 ? s_mic_src_rate : want);
            vTaskDelay(pdMS_TO_TICKS(5));
            continue;
        }
        if (got < 4) {
            vTaskDelay(pdMS_TO_TICKS(2));
            continue;
        }
        const int16_t *stereo = s_mic_stereo;
        const size_t frames = got / 4;
        /* The ES8311 runs in ADC mode with a single channel, so the I2S frame
         * carries one mono signal. Take the left slot instead of averaging: if
         * the codec leaves the right slot at zero, (L+R)/2 would cost 6 dB. */
        for (size_t i = 0; i < frames; ++i) {
            s_mic_mono[i] = stereo[i * 2];
        }
        /* Publish to the shared ring: consumers keep their own cursors. */
        app_audio_core_mic_publish(s_mic_mono, frames);
        published += (uint32_t)frames;
        const uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000);
        if (now_ms - window_ms >= 5000) {
            const uint32_t dt = now_ms - window_ms;
            ESP_LOGI(TAG, "mic capture %u frames/s", (unsigned)(published * 1000u / dt));
            window_ms = now_ms;
            published = 0;
        }
    }
    app_audio_codec_mic_stop();
    s_mic_src_rate = 0;
    app_audio_core_mic_set_rate(0); /* readers report "no data" until it restarts */
    s_mic_task = NULL;
    vTaskDelete(NULL);
}

esp_err_t app_audio_codec_mic_consumer_add(app_audio_mic_consumer_t *consumer,
                                           app_audio_source_t source, uint32_t sample_rate)
{
    (void)source; /* the reader keeps its own rate; the ring is shared */
    if (consumer == NULL || sample_rate == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    ESP_RETURN_ON_ERROR(app_audio_core_mic_reader_add(consumer, sample_rate), TAG,
                        "mic reader");
    ++s_mic_consumers;
    if (s_mic_task == NULL) {
        if (xTaskCreatePinnedToCore(i2s_in_task, "i2s_in", 4096, NULL, 3, NULL, 0) !=
            pdPASS) {
            --s_mic_consumers;
            app_audio_core_mic_reader_remove(consumer);
            return ESP_ERR_NO_MEM;
        }
        ESP_LOGI(TAG, "microphone fan-out started, %u Hz capture to %u Hz",
                 (unsigned)(s_speaker_rate != 0 ? s_speaker_rate : 48000),
                 (unsigned)sample_rate);
    }
    return ESP_OK;
}

void app_audio_codec_mic_consumer_remove(app_audio_mic_consumer_t *consumer)
{
    app_audio_core_mic_reader_remove(consumer);
    if (s_mic_consumers > 0) {
        --s_mic_consumers;
    }
}

size_t app_audio_codec_mic_consumer_read(app_audio_mic_consumer_t *consumer,
                                         int16_t *mono_out, size_t frames)
{
    return app_audio_core_mic_read(consumer, mono_out, frames);
}
