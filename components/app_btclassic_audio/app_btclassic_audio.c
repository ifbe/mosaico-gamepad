// SPDX-License-Identifier: Apache-2.0

#include "app_btclassic_audio.h"

#include <string.h>

#include "app_audio_codec.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"

#if CONFIG_BT_ENABLED

#include "esp_a2dp_api.h"
#include "esp_bt.h"
#include "esp_bt_device.h"
#include "esp_bt_main.h"
#include "esp_gap_bt_api.h"
#include "esp_hf_client_api.h"
#include "esp_hf_client_legacy_api.h" /* esp_hf_client_outgoing_data_ready() */
#include "esp_pbac_api.h"

#endif /* CONFIG_BT_ENABLED */

static const char *TAG = "bt_classic";

#if CONFIG_BT_ENABLED

/* Mono <-> stereo scratch space. SCO audio is mono, the ES8311 runs stereo. */
#define HFP_FRAMES 160 /* 10 ms at 16 kHz */
#define MIC_READ_FRAMES (HFP_FRAMES * 3) /* 10 ms at 48 kHz */
static int16_t s_hfp_stereo[HFP_FRAMES * 6 * 2]; /* SCO 8/16 kHz -> 48 kHz */
static int16_t s_hfp_mono[HFP_FRAMES];

static bool s_started;
static bool s_connected;      /* A2DP link */
static bool s_paired;
static int s_hfp_state = -1;   /* esp_hf_client_connection_state_t */
/* Cumulative counters: a single status line then tells the whole story. */
static int s_sco_attempts;
static int s_sco_ok;
static int s_call_status = -1;
static volatile uint32_t s_sco_rx;   /* SCO frames received from the phone */
/* Legacy Voice-over-HCI callbacks: if the stack uses these instead of the newer
 * audio-data callback, the counters below prove it. */
static volatile uint32_t s_legacy_rx;
/* Actual SCO codec rate: 8 kHz CVSD or 16 kHz mSBC. The stack reports which one
 * through the audio-state event; resampling must follow it or the pitch is off. */
static uint32_t s_sco_rate = 8000;
/* Uplink ring: the legacy send() callback pulls from here. */
#define SCO_UP_BYTES 1024 /* ~64 ms at 8 kHz: low latency, room for jitter */
static uint8_t s_sco_up[SCO_UP_BYTES];
static volatile uint32_t s_sco_up_head;
static volatile uint32_t s_sco_up_tail;
static volatile uint32_t s_legacy_tx;
/* Uplink diagnostics: where does the microphone chain stop? */
static volatile uint32_t s_mic_frames;  /* frames read from the codec */
static volatile uint32_t s_mic_fails;   /* reads that returned nothing */
static volatile uint32_t s_send_errs;   /* audio_data_send failures */
static volatile uint32_t s_legacy_calls; /* send() invocations */
static volatile uint32_t s_legacy_empty; /* send() called with an empty ring */
static volatile uint32_t s_sco_tx;   /* SCO frames sent to the phone */
/* Uplink instrumentation: how the stack pulls data and how fast we refill. */
static volatile uint32_t s_pull_calls;      /* outgoing callback invocations */
static volatile uint32_t s_pull_req_bytes;  /* bytes the stack asked for */
static volatile uint32_t s_pull_out_bytes;  /* bytes actually handed over */
static volatile uint32_t s_pull_empty;      /* callbacks that found no data */
static volatile uint32_t s_ready_calls;     /* outgoing_data_ready() calls */
static volatile uint32_t s_mic_iters;       /* mic task iterations */
static volatile uint32_t s_mic_bytes;       /* bytes appended to the ring */
static volatile uint32_t s_ring_peak;       /* high-water mark of the ring */

/* 48 kHz <-> SCO rate ratio (6 for CVSD, 3 for mSBC). */
static uint32_t sco_factor(void)
{
    const uint32_t f = 48000 / s_sco_rate;
    return f == 0 ? 1 : f;
}

static bool s_a2dp_streaming;
static bool s_sco_active;
static uint32_t s_a2dp_rate = 48000;
static esp_hf_sync_conn_hdl_t s_sco_handle;
static esp_bd_addr_t s_hfp_peer;
static TaskHandle_t s_mic_task;
static app_audio_mic_consumer_t s_sco_mic;

/* ------------------------------------------------------------------ A2DP --- */

static uint32_t a2dp_rate_from_mcc(const esp_a2d_mcc_t *mcc)
{
    if (mcc->type == ESP_A2D_MCT_SBC) {
        switch (mcc->cie.sbc_info.samp_freq) {
        case ESP_A2D_SBC_CIE_SF_16K:
            return 16000;
        case ESP_A2D_SBC_CIE_SF_32K:
            return 32000;
        case ESP_A2D_SBC_CIE_SF_44K:
            return 44100;
        case ESP_A2D_SBC_CIE_SF_48K:
        default:
            return 48000;
        }
    }
    /* AAC and friends: the stack always hands over 48 kHz linear PCM. */
    return 48000;
}

/* Interleaved 16-bit stereo PCM at the negotiated rate. */
static void a2dp_data_cb(const uint8_t *data, uint32_t len)
{
    /* No permission check: the codec decides whether this stream is heard. */
    if (app_audio_codec_playback_write(APP_AUDIO_SOURCE_BT, s_a2dp_rate, data, len) != ESP_OK) {
        ESP_LOGW(TAG, "a2dp write failed (%u bytes)", (unsigned)len);
    }
}

static void a2dp_start_streaming(uint32_t rate)
{
    if (s_a2dp_streaming && rate == s_a2dp_rate) {
        return;
    }
    s_a2dp_rate = rate;
    if (app_audio_codec_playback_start(APP_AUDIO_SOURCE_BT, rate) != ESP_OK) {
        ESP_LOGE(TAG, "codec cannot switch to %u Hz", (unsigned)rate);
        return;
    }
    s_a2dp_streaming = true;
    ESP_LOGI(TAG, "a2dp streaming at %u Hz", (unsigned)rate);
}

static void a2dp_stop_streaming(void)
{
    if (!s_a2dp_streaming) {
        return;
    }
    s_a2dp_streaming = false;
    /* Only report the stop: the codec falls back to whatever is still active. */
    app_audio_codec_playback_stop(APP_AUDIO_SOURCE_BT);
    ESP_LOGI(TAG, "a2dp stopped");
}

static void a2dp_cb(esp_a2d_cb_event_t event, esp_a2d_cb_param_t *param)
{
    switch (event) {
    case ESP_A2D_CONNECTION_STATE_EVT:
        s_connected = param->conn_stat.state == ESP_A2D_CONNECTION_STATE_CONNECTED;
        ESP_LOGI(TAG, "a2dp %s", s_connected ? "connected" : "disconnected");
        if (s_connected) {
            /* The phone often brings up only the media profile; pull in the
             * hands-free profile too, otherwise calls never reach SCO. */
            memcpy(s_hfp_peer, param->conn_stat.remote_bda, sizeof(s_hfp_peer));
            const esp_err_t herr = esp_hf_client_connect(s_hfp_peer);
            ESP_LOGI(TAG, "starting HFP: %s", esp_err_to_name(herr));
        } else {
            a2dp_stop_streaming();
        }
        break;
    case ESP_A2D_AUDIO_CFG_EVT:
        s_a2dp_rate = a2dp_rate_from_mcc(&param->audio_cfg.mcc);
        ESP_LOGI(TAG, "a2dp codec configured, %u Hz", (unsigned)s_a2dp_rate);
        break;
    case ESP_A2D_AUDIO_STATE_EVT: {
        const bool started = param->audio_stat.state == ESP_A2D_AUDIO_STATE_STARTED;
        ESP_LOGI(TAG, "a2dp audio %s", started ? "started" : "stopped");
        if (started) {
            a2dp_start_streaming(s_a2dp_rate);
        } else {
            a2dp_stop_streaming();
        }
        break;
    }
    default:
        break;
    }
}

/* ------------------------------------------------------------------- HFP --- */

static void hfp_legacy_recv(const uint8_t *buf, uint32_t len)
{
    s_legacy_rx += len;
    if (buf == NULL || len < 4) {
        return;
    }
    const int16_t *mono = (const int16_t *)buf;
    size_t frames = len / sizeof(int16_t);
    const uint32_t k = sco_factor();
    if (frames > HFP_FRAMES * 6 / k) {
        frames = HFP_FRAMES * 6 / k;
    }
    for (size_t i = 0; i < frames; ++i) {
        const int16_t s0 = mono[i];
        const int16_t s1 = (i + 1 < frames) ? mono[i + 1] : mono[i];
        for (uint32_t j = 0; j < k; ++j) {
            const int32_t v = s0 + (((int32_t)(s1 - s0) * (int32_t)j) / (int32_t)k);
            s_hfp_stereo[(i * k + j) * 2] = (int16_t)v;
            s_hfp_stereo[(i * k + j) * 2 + 1] = (int16_t)v;
        }
    }
    (void)app_audio_codec_playback_write(APP_AUDIO_SOURCE_BT, 48000, s_hfp_stereo,
                                         frames * k * 2 * sizeof(int16_t));
}

static uint32_t hfp_legacy_send(uint8_t *buf, uint32_t len)
{
    if (s_legacy_calls == 0) {
        ESP_LOGI(TAG, "sco pull packet %u bytes, codec rate %u",
                 (unsigned)len, (unsigned)s_sco_rate);
    }
    ++s_legacy_calls;
    ++s_pull_calls;
    s_pull_req_bytes += len;
    if (buf == NULL || len == 0) {
        return 0;
    }
    uint32_t n = 0;
    while (n < len && s_sco_up_tail != s_sco_up_head) {
        buf[n++] = s_sco_up[s_sco_up_tail % SCO_UP_BYTES];
        ++s_sco_up_tail;
    }
    if (n == 0) {
        ++s_legacy_empty;
        ++s_pull_empty;
    }
    s_legacy_tx += n;
    s_pull_out_bytes += n;
    return n;
}

static void hfp_mic_task(void *arg)
{
    (void)arg;
    static int16_t mic48[MIC_READ_FRAMES];
    while (s_sco_active) {
        const size_t frames = app_audio_codec_mic_consumer_read(&s_sco_mic, mic48,
                                                                MIC_READ_FRAMES);
        if (frames == 0) {
            ++s_mic_fails;
        } else {
            s_mic_frames += frames;
        }
        /* 48 kHz microphone -> SCO rate: average each group of `k` samples. */
        const uint32_t k = sco_factor();
        const size_t out = frames / k;
        for (size_t i = 0; i < out; ++i) {
            int32_t sum = 0;
            for (uint32_t j = 0; j < k; ++j) {
                sum += mic48[i * k + j];
            }
            s_hfp_mono[i] = (int16_t)(sum / (int32_t)k);
        }
        for (size_t i = 0; i < out; ++i) {
            if (s_sco_up_head - s_sco_up_tail >= SCO_UP_BYTES) {
                s_sco_up_tail = s_sco_up_head - SCO_UP_BYTES + 1; /* drop oldest */
            }
            s_sco_up[s_sco_up_head % SCO_UP_BYTES] = (uint8_t)((uint16_t)s_hfp_mono[i] & 0xFF);
            s_sco_up[(s_sco_up_head + 1) % SCO_UP_BYTES] =
                (uint8_t)((uint16_t)s_hfp_mono[i] >> 8);
            s_sco_up_head += 2;
        }
        ++s_mic_iters;
        s_mic_bytes += (uint32_t)(out * 2);
        const uint32_t level = s_sco_up_head - s_sco_up_tail;
        if (level > s_ring_peak) {
            s_ring_peak = level;
        }
        /* Voice-over-HCI is pull based: the stack only calls the outgoing
         * callback after this signal. */
        ++s_ready_calls;
        esp_hf_client_outgoing_data_ready();
        if (frames == 0) {
            vTaskDelay(pdMS_TO_TICKS(5));
            continue;
        }
        /* Also push through the newer API with the correct length (out frames,
         * not the 48 kHz input length). Whichever path the stack uses gets data. */
        const size_t bytes = out * sizeof(int16_t);
        esp_hf_audio_buff_t *buff = esp_hf_client_audio_buff_alloc((uint16_t)bytes);
        if (buff != NULL) {
            memcpy(buff->data, s_hfp_mono, bytes);
            buff->data_len = (uint16_t)bytes;
            if (esp_hf_client_audio_data_send(s_sco_handle, buff) != ESP_OK) {
                ++s_send_errs;
                esp_hf_client_audio_buff_free(buff);
            } else {
                s_sco_tx += (uint32_t)out;
            }
        }
    }
    s_mic_task = NULL;
    vTaskDelete(NULL);
}

/* SCO audio from the phone: mono 16-bit, needs to be duplicated to stereo. */
static void hfp_audio_data_cb(esp_hf_sync_conn_hdl_t handle, esp_hf_audio_buff_t *audio_buf,
                              bool is_bad_frame)
{
    (void)handle;
    if (audio_buf == NULL) {
        return;
    }
    if (!is_bad_frame && audio_buf->data_len > 0) {
        s_sco_rx += audio_buf->data_len / sizeof(int16_t);
        const int16_t *mono = (const int16_t *)audio_buf->data;
        size_t frames = audio_buf->data_len / sizeof(int16_t);
        if (frames > HFP_FRAMES) {
            frames = HFP_FRAMES;
        }
        /* SCO is 16 kHz; the codec stays at 48 kHz. Linear 3x upsampling keeps
         * the I2S configuration untouched (16 kHz reopens were unreliable). */
        for (size_t i = 0; i < frames; ++i) {
            const int16_t s0 = mono[i];
            const int16_t s1 = (i + 1 < frames) ? mono[i + 1] : mono[i];
            for (int k = 0; k < 3; ++k) {
                const int32_t v = s0 + (((int32_t)(s1 - s0) * k) / 3);
                s_hfp_stereo[(i * 3 + k) * 2] = (int16_t)v;
                s_hfp_stereo[(i * 3 + k) * 2 + 1] = (int16_t)v;
            }
        }
        (void)app_audio_codec_playback_write(APP_AUDIO_SOURCE_BT, 48000, s_hfp_stereo,
                                             frames * 3 * 2 * sizeof(int16_t));
    }
    esp_hf_client_audio_buff_free(audio_buf);
}

static void hfp_sco_start(void)
{
    if (s_sco_active) {
        return;
    }
    const esp_err_t perr = app_audio_codec_playback_start(APP_AUDIO_SOURCE_BT, 48000);
    if (perr != ESP_OK) {
        ESP_LOGE(TAG, "codec cannot switch to 16 kHz for SCO: %s", esp_err_to_name(perr));
        return;
    }
    const esp_err_t merr = app_audio_codec_mic_consumer_add(&s_sco_mic, APP_AUDIO_SOURCE_BT, 48000);
    if (merr != ESP_OK) {
        ESP_LOGE(TAG, "microphone unavailable for SCO: %s", esp_err_to_name(merr));
        app_audio_codec_playback_stop(APP_AUDIO_SOURCE_BT);
        return;
    }
    s_sco_active = true;
    if (xTaskCreatePinnedToCore(hfp_mic_task, "bt_mic", 4096, NULL, 3, NULL, 0) != pdPASS) {
        s_sco_active = false;
        app_audio_codec_mic_consumer_remove(&s_sco_mic);
        app_audio_codec_playback_stop(APP_AUDIO_SOURCE_BT);
        return;
    }
    ESP_LOGI(TAG, "sco audio started (16 kHz mono)");
}

static void hfp_sco_stop(void)
{
    if (!s_sco_active) {
        return;
    }
    s_sco_active = false;
    while (s_mic_task != NULL) {
        vTaskDelay(pdMS_TO_TICKS(2));
    }
    /* Only drop this consumer: the capture path itself is owned by the fan-out
     * and stays open for the USB consumer. */
    app_audio_codec_mic_consumer_remove(&s_sco_mic);
    app_audio_codec_playback_stop(APP_AUDIO_SOURCE_BT);
    ESP_LOGI(TAG, "sco audio stopped");
}

/* Periodic snapshot: HFP/SCO state changes are easy to miss in a rotating log. */
static void bt_status_task(void *arg)
{
    (void)arg;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(10000));
        ESP_LOGI(TAG,
                 "status: codec=%u paired=%d a2dp_link=%d a2dp_stream=%d hfp=%d sco=%d "
                 "sco_try=%d sco_ok=%d call=%d rx=%u tx=%u lx=%u lt=%u "
                 "mf=%u mfail=%u serr=%u lc=%u le=%u "
                 "pull=%u/%u/%u/%u ready=%u mic=%u/%u peak=%u",
                 (unsigned)s_sco_rate, (int)s_paired, (int)s_connected,
                 (int)s_a2dp_streaming, s_hfp_state,
                 (int)s_sco_active, s_sco_attempts, s_sco_ok, s_call_status,
                 (unsigned)s_sco_rx, (unsigned)s_sco_tx, (unsigned)s_legacy_rx,
                 (unsigned)s_legacy_tx, (unsigned)s_mic_frames, (unsigned)s_mic_fails,
                 (unsigned)s_send_errs, (unsigned)s_legacy_calls, (unsigned)s_legacy_empty,
                 (unsigned)s_pull_calls, (unsigned)s_pull_req_bytes,
                 (unsigned)s_pull_out_bytes, (unsigned)s_pull_empty,
                 (unsigned)s_ready_calls, (unsigned)s_mic_iters,
                 (unsigned)s_mic_bytes, (unsigned)s_ring_peak);
    }
}

static void hfp_cb(esp_hf_client_cb_event_t event, esp_hf_client_cb_param_t *param)
{
    switch (event) {
    case ESP_HF_CLIENT_CONNECTION_STATE_EVT:
        s_hfp_state = (int)param->conn_stat.state;
        ESP_LOGI(TAG, "hfp connection state %d", param->conn_stat.state);
        /* SLC up: ask for the audio link ourselves. Many phones only open SCO
         * when the call UI routes audio here, so waiting for them leaves calls
         * silent. */
        if (param->conn_stat.state == ESP_HF_CLIENT_CONNECTION_STATE_SLC_CONNECTED) {
            memcpy(s_hfp_peer, param->conn_stat.remote_bda, sizeof(s_hfp_peer));
            ESP_LOGI(TAG, "hfp ready (SCO is opened when a call is active)");
        } else if (param->conn_stat.state == ESP_HF_CLIENT_CONNECTION_STATE_DISCONNECTED) {
            hfp_sco_stop();
        }
        break;
    case ESP_HF_CLIENT_RING_IND_EVT:
        ESP_LOGI(TAG, "hfp ringing");
        /* A call is being set up: now the AG accepts an audio connection. */
        if (!s_sco_active) {
            const esp_err_t aerr = esp_hf_client_connect_audio(s_hfp_peer);
            ESP_LOGI(TAG, "ringing, requesting SCO: %s", esp_err_to_name(aerr));
        }
        break;
    case ESP_HF_CLIENT_CIND_CALL_EVT:
        s_call_status = (int)param->call.status;
        ESP_LOGI(TAG, "hfp call status %d", param->call.status);
        if (param->call.status != 0 && !s_sco_active) {
            const esp_err_t aerr = esp_hf_client_connect_audio(s_hfp_peer);
            ESP_LOGI(TAG, "call active, requesting SCO: %s", esp_err_to_name(aerr));
        }
        break;
    case ESP_HF_CLIENT_CLIP_EVT:
        ESP_LOGI(TAG, "hfp caller id");
        break;
    case ESP_HF_CLIENT_AT_RESPONSE_EVT:
        ESP_LOGD(TAG, "hfp at response");
        break;
    case ESP_HF_CLIENT_AUDIO_STATE_EVT: {
        const esp_hf_client_audio_state_t state = param->audio_stat.state;
        ESP_LOGI(TAG, "hfp audio state %d", state);
        if (state == ESP_HF_CLIENT_AUDIO_STATE_CONNECTING) {
            ++s_sco_attempts;
        } else if (state == ESP_HF_CLIENT_AUDIO_STATE_CONNECTED) {
            s_sco_rate = 8000; /* CVSD */
            ++s_sco_ok;
        } else if (state == ESP_HF_CLIENT_AUDIO_STATE_CONNECTED_MSBC) {
            s_sco_rate = 16000; /* mSBC wideband */
            ++s_sco_ok;
        }
        if (state == ESP_HF_CLIENT_AUDIO_STATE_CONNECTED ||
            state == ESP_HF_CLIENT_AUDIO_STATE_CONNECTED_MSBC) {
            s_sco_handle = param->audio_stat.sync_conn_handle;
            hfp_sco_start();
        } else {
            hfp_sco_stop();
        }
        break;
    }
    default:
        /* Log everything: a call that never produces RING/CIND here means the
         * phone did not route the call to this link. */
        ESP_LOGI(TAG, "hfp event %d", (int)event);
        break;
    }
}

/* --------------------------------------------------------------- GAP/init --- */

static void gap_cb(esp_bt_gap_cb_event_t event, esp_bt_gap_cb_param_t *param)
{
    switch (event) {
    case ESP_BT_GAP_AUTH_CMPL_EVT:
        s_paired = param->auth_cmpl.stat == ESP_BT_STATUS_SUCCESS;
        ESP_LOGI(TAG, "pairing %s", s_paired ? "ok" : "failed");
        break;
    case ESP_BT_GAP_ACL_CONN_CMPL_STAT_EVT: {
        /* Start HFP as soon as the link is up: phones often connect only the
         * media profile after pairing, leaving calls silent. */
        ESP_LOGI(TAG, "acl connected, starting HFP");
        const esp_err_t herr = esp_hf_client_connect(param->acl_conn_cmpl_stat.bda);
        ESP_LOGI(TAG, "acl hfp connect: %s", esp_err_to_name(herr));
        break;
    }
    case ESP_BT_GAP_MODE_CHG_EVT:
        ESP_LOGI(TAG, "link mode %d", param->mode_chg.mode);
        break;
    default:
        break;
    }
}

esp_err_t app_btclassic_audio_start(void)
{
    if (s_started) {
        return ESP_OK;
    }

    /* The controller, Bluedroid, device name and class of device belong to
     * app_bt; this component only brings up the audio profiles. */
    ESP_ERROR_CHECK(esp_bt_gap_register_callback(gap_cb));

    /* Microphone and call audio first: the hands-free profile allocates the SCO
     * resource, and initialising it after A2DP has been observed to fail. */
    /* Same order as the IDF hfp_hf example: callbacks first, then init, then the
     * phonebook client (phones use it when deciding this is a real headset). */
    ESP_ERROR_CHECK(esp_hf_client_register_callback(hfp_cb));
    /* Register the Voice-over-HCI data pair *before* init, like the callback: the
     * stack pulls microphone PCM through `send` in internal-codec mode. */
    const esp_err_t rerr = esp_hf_client_register_data_callback(hfp_legacy_recv, hfp_legacy_send);
    esp_err_t err = esp_hf_client_init();
    const bool hfp_ok = err == ESP_OK;
    if (hfp_ok) {
        const esp_err_t aerr = esp_hf_client_register_audio_data_callback(hfp_audio_data_cb);
        ESP_LOGI(TAG, "hfp data callbacks: legacy=%s audio=%s", esp_err_to_name(rerr),
                 esp_err_to_name(aerr));
        (void)esp_pbac_init();
        ESP_LOGI(TAG, "HFP hands-free ready");
    } else {
        ESP_LOGE(TAG, "HFP init failed: %s (speaker only)", esp_err_to_name(err));
    }

    /* Speaker: A2DP sink. */
    ESP_ERROR_CHECK(esp_a2d_register_callback(a2dp_cb));
    ESP_ERROR_CHECK(esp_a2d_sink_register_data_callback(a2dp_data_cb));
    ESP_ERROR_CHECK(esp_a2d_sink_init());

    if (xTaskCreatePinnedToCore(bt_status_task, "bt_status", 3072, NULL, 1, NULL, 0) != pdPASS) {
        ESP_LOGW(TAG, "status task creation failed");
    }
    s_started = true;
    ESP_LOGI(TAG, "A2DP sink%s ready", hfp_ok ? " + HFP hands-free" : " only");
    return ESP_OK;
}

void app_btclassic_audio_stop(void)
{
    if (!s_started) {
        return;
    }
    hfp_sco_stop();
    a2dp_stop_streaming();
    esp_hf_client_deinit();
    esp_a2d_sink_deinit();
    s_started = false;
    s_connected = false;
}

bool app_btclassic_audio_connected(void)
{
    return s_connected;
}

#else /* !CONFIG_BT_ENABLED */

esp_err_t app_btclassic_audio_start(void)
{
    ESP_LOGW(TAG, "bluetooth disabled in this build (CONFIG_BT_ENABLED=n)");
    return ESP_ERR_NOT_SUPPORTED;
}

void app_btclassic_audio_stop(void)
{
}

bool app_btclassic_audio_connected(void)
{
    return false;
}

#endif /* CONFIG_BT_ENABLED */
