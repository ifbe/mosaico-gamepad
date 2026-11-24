// SPDX-License-Identifier: Apache-2.0

#include "app_usb_audio.h"

#include "app_usb_audio_ops.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "tusb.h"

#include "app_audio_codec.h"
#include "app_usb_core.h"
#include "esp_iris.h"
#include "class/audio/audio_device.h"
#include "class/hid/hid_device.h"
#include "esp_check.h"
#include "esp_iris_usb_hid.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "usb_audio";

/* Implemented further down, next to the playback tasks. */
static void capture_stream_start(bool enable);

/* Entity identifiers inside the audio function. */
#define AUDIO_SKIP_TASK 0
#define AUDIO_BASE_IFACE 4
#define AUDIO_AS_IFACE 5
#define AUDIO_AS_CAPTURE_IFACE 6

/* Interface numbers are rewritten at startup (ESP-Iris may spend two extra
 * interfaces on its data link), so the alt-setting callback compares against the
 * runtime base instead of the descriptor constants above. */
static uint8_t s_as_itf, s_cap_itf;
#define AUDIO_SAMPLE_RATE 48000
#define UAC_ENTITY_CLOCK 1
/* Playback path: USB streaming in -> feature (volume) -> headphones out. */
#define UAC_ENTITY_IN_TERMINAL 2
#define UAC_ENTITY_FEATURE 3
#define UAC_ENTITY_OUT_TERMINAL 4
/* Capture path: microphone in -> feature (gain) -> USB streaming out. */
#define UAC_ENTITY_MIC_IN_TERMINAL 5
#define UAC_ENTITY_MIC_FEATURE 6
#define UAC_ENTITY_MIC_OUT_TERMINAL 7

/* Volume in 1/256 dB units: -60 dB .. 0 dB. */
#define UAC_VOL_MIN (-15360)
#define UAC_VOL_MAX 0

static int16_t s_volume = -3072; /* -12 dB */
static uint8_t s_mute;
/* Microphone gain in 1/256 dB units (0..30 dB), reported to the host. */
static int32_t s_mic_gain_db256 = 36 * 256;
static bool s_mic_muted;

/* ---------------------------------------------------------------- callbacks */

static bool uac2_set_itf(uint8_t rhport, tusb_control_request_t const *request)
{
    (void)rhport;
    /* SET_INTERFACE: wIndex = interface, wValue = alternate setting. */
    const uint8_t itf = (uint8_t)(request->wIndex & 0xFF);
    const uint8_t alt = (uint8_t)(request->wValue & 0xFF);
    ESP_LOGI(TAG, "stream itf=%u alt=%u", (unsigned)itf, (unsigned)alt);
    const uint8_t cap_itf = s_cap_itf ? s_cap_itf : AUDIO_AS_CAPTURE_IFACE;
    const uint8_t as_itf = s_as_itf ? s_as_itf : AUDIO_AS_IFACE;
    if (itf == cap_itf) {
        /* The host opened or closed the microphone stream. */
        capture_stream_start(alt != 0);
    } else if (itf == as_itf) {
        /* Playback only reports its state; writing never asks permission. */
        if (alt != 0) {
            (void)app_audio_codec_playback_start(APP_AUDIO_SOURCE_USB, AUDIO_SAMPLE_RATE);
        } else {
            app_audio_codec_playback_stop(APP_AUDIO_SOURCE_USB);
        }
    }
    return true;
}

static void uac2_feedback_params(uint8_t func_id, uint8_t alt_itf,
                                  audio_feedback_params_t *feedback_param)
{
    (void)func_id;
    (void)alt_itf;
    /* The official usb_device_uac component reports the FIFO level; that keeps
     * the host pacing the stream to this device instead of to a fixed rate. */
    feedback_param->method = AUDIO_FEEDBACK_METHOD_FIFO_COUNT;
    feedback_param->sample_freq = AUDIO_SAMPLE_RATE;
}

static bool uac2_set_itf_close(uint8_t rhport, tusb_control_request_t const *request)
{
    (void)rhport;
    ESP_LOGI(TAG, "stream stop itf=%u alt=%u", (unsigned)(request->wIndex & 0xFF),
             (unsigned)(request->wValue & 0xFF));
    return true;
}

static void apply_volume(void)
{
    const int span = UAC_VOL_MAX - UAC_VOL_MIN;
    int percent = (int)(((int32_t)(s_volume - UAC_VOL_MIN) * 100) / span);
    if (s_mute) {
        percent = 0;
    }
    ESP_LOGI(TAG, "codec volume -> %d%% (raw %d/256 dB, mute %u)", percent, s_volume,
             s_mute);
    (void)app_audio_codec_set_volume(percent);
}

static bool uac2_get_req_entity(uint8_t rhport, tusb_control_request_t const *request)
{
    const audio_control_request_t *req = (audio_control_request_t const *)request;

    /* Clock source: the host asks for the sample rate while initialising, and
     * refuses the device if the request stalls. */
    if (req->bEntityID == UAC_ENTITY_CLOCK) {
        if (req->bControlSelector == AUDIO20_CS_CTRL_SAM_FREQ) {
            if (req->bRequest == AUDIO_CS_REQ_CUR) {
                uint32_t freq = AUDIO_SAMPLE_RATE;
                return tud_control_xfer(rhport, request, &freq, sizeof(freq));
            }
            if (req->bRequest == AUDIO_CS_REQ_RANGE) {
                audio20_control_range_4_n_t(1) range;
                range.wNumSubRanges = 1;
                range.subrange[0].bMin = AUDIO_SAMPLE_RATE;
                range.subrange[0].bMax = AUDIO_SAMPLE_RATE;
                range.subrange[0].bRes = 0;
                return tud_control_xfer(rhport, request, &range, sizeof(range));
            }
        }
        if (req->bControlSelector == AUDIO20_CS_CTRL_CLK_VALID) {
            uint8_t valid = 1;
            return tud_control_xfer(rhport, request, &valid, sizeof(valid));
        }
        return false;
    }

    if (req->bEntityID != UAC_ENTITY_FEATURE && req->bEntityID != UAC_ENTITY_MIC_FEATURE) {
        return false;
    }
    const bool capture = req->bEntityID == UAC_ENTITY_MIC_FEATURE;

    /* UAC2 replaces UAC1's GET_MIN/MAX/RES with a single RANGE request. */
    if (req->bControlSelector == AUDIO20_FU_CTRL_VOLUME) {
        if (req->bRequest == AUDIO_CS_REQ_CUR) {
            int16_t value = (int16_t)(capture ? s_mic_gain_db256 : s_volume);
            return tud_control_xfer(rhport, request, &value, sizeof(value));
        }
        if (req->bRequest == AUDIO_CS_REQ_RANGE) {
            audio20_control_range_2_n_t(1) range;
            range.wNumSubRanges = 1;
            /* Capture gain runs 0..+30 dB, playback volume -60..0 dB. */
            range.subrange[0].bMin = (int16_t)(capture ? 0 : UAC_VOL_MIN);
            range.subrange[0].bMax = (int16_t)(capture ? 30 * 256 : UAC_VOL_MAX);
            range.subrange[0].bRes = 1;
            return tud_control_xfer(rhport, request, &range, sizeof(range));
        }
        return false;
    }
    if (req->bControlSelector == AUDIO20_FU_CTRL_MUTE) {
        if (req->bRequest == AUDIO_CS_REQ_CUR) {
            uint8_t value = s_mute;
            return tud_control_xfer(rhport, request, &value, sizeof(value));
        }
        return false;
    }
    return false;
}

static bool uac2_set_req_entity(uint8_t rhport, tusb_control_request_t const *request,
                                 uint8_t *buffer)
{
    (void)rhport;
    const audio_control_request_t *req = (audio_control_request_t const *)request;

    /* The host sets the clock sample rate while initialising; leaving this
     * request unanswered stalls AppleUSBAudio during device bring-up. */
    if (req->bEntityID == UAC_ENTITY_CLOCK) {
        if (req->bControlSelector == AUDIO20_CS_CTRL_SAM_FREQ &&
            req->bRequest == AUDIO_CS_REQ_CUR) {
            uint32_t rate = 0;
            memcpy(&rate, buffer, sizeof(rate));
            if (rate != AUDIO_SAMPLE_RATE) {
                ESP_LOGW(TAG, "host requested %u Hz; the codec runs at %u Hz",
                         (unsigned)rate, (unsigned)AUDIO_SAMPLE_RATE);
            }
            ESP_LOGI(TAG, "host set sample rate %u Hz", (unsigned)rate);
            return true;
        }
        return false;
    }

    const uint8_t entity = (uint8_t)(request->wIndex >> 8);
    const uint8_t cs = (uint8_t)(request->wValue >> 8);
    if ((entity != UAC_ENTITY_FEATURE && entity != UAC_ENTITY_MIC_FEATURE) ||
        request->bRequest != AUDIO10_CS_REQ_SET_CUR) {
        return false;
    }
    if (entity == UAC_ENTITY_MIC_FEATURE) {
        if (cs == AUDIO20_FU_CTRL_VOLUME) {
            int16_t value;
            memcpy(&value, buffer, sizeof(value));
            if (value < 0) {
                value = 0;
            } else if (value > 30 * 256) {
                value = (int16_t)(30 * 256);
            }
            s_mic_gain_db256 = value;
            app_audio_codec_set_mic_gain_db((float)value / 256.0f);
            ESP_LOGI(TAG, "host mic gain %d/256 dB", (int)s_mic_gain_db256);
            return true;
        }
        if (cs == AUDIO20_FU_CTRL_MUTE) {
            s_mic_muted = buffer[0] ? true : false;
            app_audio_codec_set_mic_gain_db(s_mic_muted ? -96.0f
                                                        : (float)s_mic_gain_db256 / 256.0f);
            ESP_LOGI(TAG, "host mic mute %u", (unsigned)s_mic_muted);
            return true;
        }
        return false;
    }
    if (cs == AUDIO20_FU_CTRL_VOLUME) {
        int16_t value;
        memcpy(&value, buffer, sizeof(value));
        if (value < UAC_VOL_MIN) {
            value = (int16_t)UAC_VOL_MIN;
        } else if (value > UAC_VOL_MAX) {
            value = (int16_t)UAC_VOL_MAX;
        }
        s_volume = value;
        apply_volume();
        ESP_LOGI(TAG, "host volume %d/256 dB", s_volume);
        return true;
    }
    if (cs == AUDIO20_FU_CTRL_MUTE) {
        s_mute = buffer[0] ? 1 : 0;
        apply_volume();
        ESP_LOGI(TAG, "host mute %u", s_mute);
        return true;
    }
    return false;
}

/* ------------------------------------------------------------- descriptors */

#define AUDIO_IFACE_COUNT 3 /* AudioControl + playback AS + capture AS */
#define AUDIO_BASE_IFACE 4
#define AUDIO_AS_IFACE 5
#define AUDIO_CHANNELS 2
#define AUDIO_CAPTURE_CHANNELS 1 /* the board has one microphone capsule */
#define AUDIO_BYTES_PER_SAMPLE 2
#define AUDIO_BIT_RESOLUTION 16
#define AUDIO_EP_OUT_ADDR 0x06
#define AUDIO_EP_FB_ADDR 0x86
#define AUDIO_EP_IN_ADDR 0x85
#define AUDIO_EP_SIZE 24 /* one isochronous packet per microframe at 48 kHz */
/* Mono 48 kHz 16-bit needs 12 bytes per microframe; 16 leaves headroom for the
 * host's slight rate mismatch. */
#define AUDIO_EP_IN_SIZE 16

/* Volume and mute for master and both channels. */
#define AUDIO_FU_CTRL \
    (AUDIO20_CTRL_RW << AUDIO20_FEATURE_UNIT_CTRL_MUTE_POS | \
     AUDIO20_CTRL_RW << AUDIO20_FEATURE_UNIT_CTRL_VOLUME_POS)

/* AudioClass 2.0 playback function built from TinyUSB's own macros, which also
 * emit the mandatory protocol code 0x20 on the audio interfaces. Hand-written
 * bytes with protocol 0x00 are parsed as UAC1 and rejected with
 * "badADCVersion" because bcdADC says 2.0. */
static const uint8_t s_audio_desc[] = {
    TUD_AUDIO20_DESC_IAD(AUDIO_BASE_IFACE, AUDIO_IFACE_COUNT, 0x00),
    TUD_AUDIO20_DESC_STD_AC(AUDIO_BASE_IFACE, 0x00, 0x00),
    TUD_AUDIO20_DESC_CS_AC(0x0200, AUDIO20_FUNC_HEADSET,
                           TUD_AUDIO20_DESC_CLK_SRC_LEN +
                               TUD_AUDIO20_DESC_INPUT_TERM_LEN * 2 +
                               TUD_AUDIO20_DESC_FEATURE_UNIT_LEN(AUDIO_CHANNELS) +
                               TUD_AUDIO20_DESC_FEATURE_UNIT_LEN(AUDIO_CAPTURE_CHANNELS) +
                               TUD_AUDIO20_DESC_OUTPUT_TERM_LEN * 2,
                           0x00), /* no latency control: not implemented */
    TUD_AUDIO20_DESC_CLK_SRC(UAC_ENTITY_CLOCK, 0x03, 0x07, 0x00, 0x00),
    TUD_AUDIO20_DESC_INPUT_TERM(UAC_ENTITY_IN_TERMINAL, AUDIO_TERM_TYPE_USB_STREAMING,
                                0x00, UAC_ENTITY_CLOCK, AUDIO_CHANNELS,
                                AUDIO20_CHANNEL_CONFIG_NON_PREDEFINED, 0x00, 0x00, 0x00),
    TUD_AUDIO20_DESC_FEATURE_UNIT(UAC_ENTITY_FEATURE, UAC_ENTITY_IN_TERMINAL, 0x00,
                                  AUDIO_FU_CTRL, AUDIO_FU_CTRL, AUDIO_FU_CTRL),
    TUD_AUDIO20_DESC_OUTPUT_TERM(UAC_ENTITY_OUT_TERMINAL, AUDIO_TERM_TYPE_OUT_HEADPHONES,
                                 0x00, UAC_ENTITY_FEATURE, UAC_ENTITY_CLOCK, 0x0000, 0x00),
    /* Capture chain: microphone -> gain/mute -> USB streaming. */
    TUD_AUDIO20_DESC_INPUT_TERM(UAC_ENTITY_MIC_IN_TERMINAL, AUDIO_TERM_TYPE_IN_GENERIC_MIC,
                                0x00, UAC_ENTITY_CLOCK, AUDIO_CAPTURE_CHANNELS,
                                AUDIO20_CHANNEL_CONFIG_NON_PREDEFINED, 0x00, 0x00, 0x00),
    TUD_AUDIO20_DESC_FEATURE_UNIT(UAC_ENTITY_MIC_FEATURE, UAC_ENTITY_MIC_IN_TERMINAL, 0x00,
                                  AUDIO_FU_CTRL, AUDIO_FU_CTRL),
    TUD_AUDIO20_DESC_OUTPUT_TERM(UAC_ENTITY_MIC_OUT_TERMINAL, AUDIO_TERM_TYPE_USB_STREAMING,
                                 0x00, UAC_ENTITY_MIC_FEATURE, UAC_ENTITY_CLOCK, 0x0000, 0x00),
    TUD_AUDIO20_DESC_STD_AS_INT(AUDIO_AS_IFACE, 0x00, 0x00, 0x00),
    TUD_AUDIO20_DESC_STD_AS_INT(AUDIO_AS_IFACE, 0x01, 0x01, 0x00),
    TUD_AUDIO20_DESC_CS_AS_INT(UAC_ENTITY_IN_TERMINAL, AUDIO20_CTRL_NONE,
                               AUDIO20_FORMAT_TYPE_I, AUDIO20_DATA_FORMAT_TYPE_I_PCM,
                               AUDIO_CHANNELS, AUDIO20_CHANNEL_CONFIG_NON_PREDEFINED, 0x00),
    TUD_AUDIO20_DESC_TYPE_I_FORMAT(AUDIO_BYTES_PER_SAMPLE, AUDIO_BIT_RESOLUTION),
    /* Adaptive endpoint without a feedback endpoint. Advertising a feedback
     * endpoint makes macOS restart the stream in a loop and never send audio
     * (TinyUSB discussion #3393). */
    TUD_AUDIO20_DESC_STD_AS_ISO_EP(
        AUDIO_EP_OUT_ADDR,
        (uint8_t)(TUSB_XFER_ISOCHRONOUS | TUSB_ISO_EP_ATT_ADAPTIVE | TUSB_ISO_EP_ATT_DATA),
        AUDIO_EP_SIZE, 0x01),
    TUD_AUDIO20_DESC_CS_AS_ISO_EP(AUDIO20_CS_AS_ISO_DATA_EP_ATT_NON_MAX_PACKETS_OK,
                                  AUDIO20_CTRL_NONE,
                                  AUDIO20_CS_AS_ISO_DATA_EP_LOCK_DELAY_UNIT_MILLISEC, 0x0001),
    /* Capture (microphone) AudioStreaming interface, alternate setting 0. */
    TUD_AUDIO20_DESC_STD_AS_INT(AUDIO_AS_CAPTURE_IFACE, 0x00, 0x00, 0x00),
    /* Alternate setting 1: one isochronous IN endpoint. */
    TUD_AUDIO20_DESC_STD_AS_INT(AUDIO_AS_CAPTURE_IFACE, 0x01, 0x01, 0x00),
    TUD_AUDIO20_DESC_CS_AS_INT(UAC_ENTITY_MIC_OUT_TERMINAL, AUDIO20_CTRL_NONE,
                               AUDIO20_FORMAT_TYPE_I, AUDIO20_DATA_FORMAT_TYPE_I_PCM,
                               AUDIO_CAPTURE_CHANNELS,
                               AUDIO20_CHANNEL_CONFIG_NON_PREDEFINED, 0x00),
    TUD_AUDIO20_DESC_TYPE_I_FORMAT(AUDIO_BYTES_PER_SAMPLE, AUDIO_BIT_RESOLUTION),
    /* Synchronous IN endpoint: no feedback path needed in the capture direction. */
    TUD_AUDIO20_DESC_STD_AS_ISO_EP(
        AUDIO_EP_IN_ADDR,
        (uint8_t)(TUSB_XFER_ISOCHRONOUS | TUSB_ISO_EP_ATT_SYNCHRONOUS | TUSB_ISO_EP_ATT_DATA),
        AUDIO_EP_IN_SIZE, 0x01),
    TUD_AUDIO20_DESC_CS_AS_ISO_EP(AUDIO20_CS_AS_ISO_DATA_EP_ATT_NON_MAX_PACKETS_OK,
                                  AUDIO20_CTRL_NONE,
                                  AUDIO20_CS_AS_ISO_DATA_EP_LOCK_DELAY_UNIT_MILLISEC, 0x0001),
};

#define AUDIO_CHUNK_FRAMES 120 /* 2.5 ms */
#define AUDIO_CHUNK_BYTES (AUDIO_CHUNK_FRAMES * AUDIO_CHANNELS * 2)

static uint8_t s_rx[AUDIO_CHUNK_BYTES * 2];
/* Woken by the OUT interrupt as soon as a packet is queued. The writer never
 * invents audio: the mixer fills silence when a slot has nothing queued. */
static TaskHandle_t s_writer_task;
/* Scratch buffer used from the USB interrupt when draining the audio FIFO. */
static uint8_t s_isr_buf[AUDIO_CHUNK_BYTES * 2];
/* Ring buffer between the USB audio FIFO and the codec. The writer always
 * keeps the I2S DMA fed: an underrun makes the DMA replay the previous block
 * and storm the CPU with interrupts, which starves the USB stack. */
static volatile bool s_streaming;
/* Lock-free single-producer/single-consumer ring (size must be a power of 2). */
#define AUDIO_RING_BYTES 8192
#define AUDIO_RING_MASK (AUDIO_RING_BYTES - 1)

static uint8_t s_ring[AUDIO_RING_BYTES];
static volatile uint32_t s_ring_head; /* written by the reader only */
static volatile uint32_t s_ring_tail; /* written by the writer only */

static inline uint32_t ring_count(void)
{
    return (uint32_t)(s_ring_head - s_ring_tail) & AUDIO_RING_MASK;
}

/* Two memcpy segments per call: the byte-at-a-time version ran in the isochronous
 * interrupt and cost more than the copy itself. */
static size_t ring_write(const uint8_t *src, size_t len)
{
    const size_t space = AUDIO_RING_BYTES - ring_count();
    if (len > space) {
        len = space;
    }
    size_t first = AUDIO_RING_BYTES - (s_ring_head & AUDIO_RING_MASK);
    if (first > len) {
        first = len;
    }
    memcpy(&s_ring[s_ring_head & AUDIO_RING_MASK], src, first);
    if (len > first) {
        memcpy(&s_ring[0], src + first, len - first);
    }
    s_ring_head += len;
    return len;
}

static size_t ring_read(uint8_t *dst, size_t len)
{
    const size_t count = ring_count();
    if (len > count) {
        len = count;
    }
    size_t first = AUDIO_RING_BYTES - (s_ring_tail & AUDIO_RING_MASK);
    if (first > len) {
        first = len;
    }
    memcpy(dst, &s_ring[s_ring_tail & AUDIO_RING_MASK], first);
    if (len > first) {
        memcpy(dst + first, &s_ring[0], len - first);
    }
    s_ring_tail += len;
    return len;
}

/* Drop everything buffered: a restart must never replay stale PCM, which comes
 * out as a second or two of loud noise. */
static void ring_reset(void)
{
    s_ring_tail = s_ring_head;
}

/* Counters kept for the diagnostics log. */static volatile uint32_t s_stat_blocks;
static volatile uint32_t s_stat_bytes;
static volatile uint32_t s_stat_last_write_ms;
static volatile uint32_t s_stat_overflows;
/* Number of times the audio class called us for a received packet, and the
 * number of bytes we took per call. A steady 8 KB/s is either few calls with
 * many bytes (host pacing) or many calls with a few bytes (stack truncation). */
static volatile uint32_t s_stat_isr_calls;
/* Bytes of mono PCM handed to the USB microphone endpoint. */
static volatile uint32_t s_stat_capture_bytes;
static volatile bool s_capture_active;

/* Periodic diagnostics: one report every 10 s so the log stays quiet in
 * between. Key events (stream open/close, capture start/stop, errors) are logged
 * as they happen and are not rate limited. */
#define DIAG_PERIOD_MS 10000

static void diag_report(void)
{
    /* Never call TinyUSB from here: this task runs on the other core and the
     * stack is not thread safe. Only local counters are read. */
    static uint32_t last_bytes;
    static uint32_t last_calls;
    static uint32_t last_capture;
    static uint32_t rounds;
    const uint32_t seconds = DIAG_PERIOD_MS / 1000;
    const uint32_t now = s_stat_bytes;
    const uint32_t captured = s_stat_capture_bytes;
    const uint32_t byte_delta = now - last_bytes;
    const uint32_t capture_delta = captured - last_capture;
    const uint32_t calls = s_stat_isr_calls - last_calls;
    last_bytes = now;
    last_capture = captured;
    last_calls = s_stat_isr_calls;

    /* Playback: ring level, blocks written, total bytes, last write latency,
     * ring overflows, streaming flag and the resulting rate. */
    ESP_LOGI(TAG, "pb r=%u b=%u by=%u l=%ums o=%u s=%d kbs=%u isr/s=%u B/call=%u",
             (unsigned)ring_count(), (unsigned)s_stat_blocks, (unsigned)now,
             (unsigned)s_stat_last_write_ms, (unsigned)s_stat_overflows,
             (int)s_streaming, (unsigned)(byte_delta / seconds / 1024),
             (unsigned)(calls / seconds), (unsigned)(calls > 0 ? byte_delta / calls : 0));
    /* Capture: mono bytes handed to the USB IN endpoint and the resulting rate. */
    ESP_LOGI(TAG, "mic on=%d by=%u kbs=%u", (int)s_capture_active, (unsigned)captured,
             (unsigned)(capture_delta / seconds / 1024));

    /* Build configuration once a minute: which USB backend and buffer sizes are
     * actually compiled in. */
    if (rounds++ % 6 == 0) {
#if defined(CFG_TUD_DWC2_DMA_ENABLE) && CFG_TUD_DWC2_DMA_ENABLE
        const char *backend = "dma";
#else
        const char *backend = "slave";
#endif
#if defined(CFG_TUD_AUDIO_FUNC_1_EP_OUT_SW_BUF_SZ)
        ESP_LOGI(TAG, "cfg %s out=%d epmax=%d ep=%d in=%d", backend,
                 CFG_TUD_AUDIO_FUNC_1_EP_OUT_SW_BUF_SZ,
                 CFG_TUD_AUDIO_FUNC_1_EP_OUT_SZ_MAX, AUDIO_EP_SIZE, AUDIO_EP_IN_SIZE);
#else
        ESP_LOGW(TAG, "cfg %s fifo unknown", backend);
#endif
    }
}

static void diag_task(void *arg)
{
    (void)arg;
    for (;;) {
        diag_report();
        vTaskDelay(pdMS_TO_TICKS(DIAG_PERIOD_MS));
    }
}

/* Called by TinyUSB in interrupt context right after a packet landed in the
 * audio OUT FIFO. The stack re-arms the isochronous OUT transfer only when the
 * FIFO write succeeded (audio_device.c: TU_VERIFY(0 < tu_fifo_write_n(...))),
 * so a FIFO that ever fills up silently stops the stream forever. Draining it
 * here, packet by packet, is what the official usb_device_uac component does. */
static bool uac2_rx_done_isr(uint8_t rhport, uint16_t n_bytes_received, uint8_t func_id,
                           uint8_t ep_out, uint8_t cur_alt_setting)
{
    (void)rhport;
    (void)n_bytes_received;
    (void)func_id;
    (void)ep_out;
    (void)cur_alt_setting;

    ++s_stat_isr_calls;
    static int64_t last_us;
    static bool new_stream;
    const int64_t now = esp_timer_get_time();
    if (now - last_us > 100 * 1000) {
        /* A silence of 100 ms means the host started a new playback: drop the
         * stale FIFO and ring content and pre-buffer a few milliseconds. */
        new_stream = true;
        tud_audio_n_clear_ep_out_ff(0);
        ring_reset();
    }
    last_us = now;

    const uint16_t available = tud_audio_n_available(0);
    if (new_stream) {
        if (available < AUDIO_CHUNK_BYTES * 2) {
            return true;
        }
        new_stream = false;
    }
    const uint16_t room = (uint16_t)(AUDIO_RING_BYTES - ring_count());
    if (room == 0) {
        ++s_stat_overflows;
        return true;
    }
    uint16_t want = available < room ? available : room;
    if (want > sizeof(s_isr_buf)) {
        want = sizeof(s_isr_buf);
    }
    if (want == 0) {
        return true;
    }
    const uint16_t got = tud_audio_n_read(0, s_isr_buf, want);
    if (got > 0) {
        ring_write(s_isr_buf, got);
        if (s_writer_task != NULL) {
            BaseType_t woken = pdFALSE;
            vTaskNotifyGiveFromISR(s_writer_task, &woken);
            portYIELD_FROM_ISR(woken);
        }
        s_stat_bytes += got;
        s_streaming = true;
    }
    return true;
}

/* ------------------------------------------------------ microphone capture */

#define CAPTURE_FRAMES 120 /* 2.5 ms at 48 kHz */
#define CAPTURE_STEREO_BYTES (CAPTURE_FRAMES * AUDIO_CHANNELS * AUDIO_BYTES_PER_SAMPLE)
#define CAPTURE_MONO_BYTES (CAPTURE_FRAMES * AUDIO_CAPTURE_CHANNELS * AUDIO_BYTES_PER_SAMPLE)

static TaskHandle_t s_capture_task;

/* The ES8311 delivers stereo; the descriptor advertises one channel, so the two
 * samples of each frame are averaged. */
static app_audio_mic_consumer_t s_cap_consumer;
static int16_t s_cap_mono[240]; /* 5 ms at 48 kHz */

static void capture_task(void *arg)
{
    (void)arg;
    /* The shared microphone reader returns whatever is new, never padded, so
     * accumulate a fixed 2.5 ms block: the isochronous IN endpoint wants one
     * packet per interval, and a short packet shows up as a host dropout. */
    size_t filled = 0;
    const size_t a_frame = AUDIO_CAPTURE_CHANNELS * AUDIO_BYTES_PER_SAMPLE;
    ESP_LOGI(TAG, "audio_cap consumer started (%d Hz mono)", AUDIO_SAMPLE_RATE);
    int64_t window_us = esp_timer_get_time();
    uint32_t stat_frames = 0, stat_writes = 0, stat_errors = 0, stat_rounds = 0;
    while (s_capture_active) {
        const size_t want = CAPTURE_FRAMES - filled;
        const size_t frames = app_audio_codec_mic_consumer_read(
            &s_cap_consumer, &s_cap_mono[filled * AUDIO_CAPTURE_CHANNELS],
            want);
        filled += frames;
        ++stat_rounds;
        if (frames == 0) {
            vTaskDelay(pdMS_TO_TICKS(1));
            continue;
        }
        if (filled < CAPTURE_FRAMES) {
            continue;
        }
        const size_t bytes = filled * a_frame;
        if (!tud_audio_n_write(0, s_cap_mono, (uint16_t)bytes)) {
            ++stat_errors;
        }
        s_stat_capture_bytes += bytes;
        stat_frames += (uint32_t)filled;
        ++stat_writes;
        filled = 0;

        const int64_t now_us = esp_timer_get_time();
        if (now_us - window_us >= 10 * 1000 * 1000) {
            const uint32_t dt_ms = (uint32_t)((now_us - window_us) / 1000);
            ESP_LOGI(TAG, "audio_cap: %u frames/s, %u writes/s, err=%u, rounds/s=%u",
                     (unsigned)(stat_frames * 1000u / dt_ms),
                     (unsigned)(stat_writes * 1000u / dt_ms), (unsigned)stat_errors,
                     (unsigned)(stat_rounds * 1000u / dt_ms));
            window_us = now_us;
            stat_frames = 0;
            stat_writes = 0;
            stat_errors = 0;
            stat_rounds = 0;
        }
    }
    s_capture_task = NULL;
    vTaskDelete(NULL);
}

static void capture_stream_start(bool enable)
{
    if (enable == s_capture_active) {
        return;
    }
    if (!enable) {
        s_capture_active = false;
        while (s_capture_task != NULL) {
            vTaskDelay(pdMS_TO_TICKS(2));
        }
        app_audio_codec_mic_consumer_remove(&s_cap_consumer);
        ESP_LOGI(TAG, "capture stopped, %u bytes sent", (unsigned)s_stat_capture_bytes);
        return;
    }
    if (app_audio_codec_mic_consumer_add(&s_cap_consumer, APP_AUDIO_SOURCE_USB,
                                         AUDIO_SAMPLE_RATE) != ESP_OK) {
        ESP_LOGE(TAG, "microphone consumer unavailable");
        return;
    }
    s_capture_active = true;
    if (xTaskCreatePinnedToCore(capture_task, "audio_cap", 4096, NULL, 3, NULL, 0) != pdPASS) {
        s_capture_active = false;
        app_audio_codec_mic_consumer_remove(&s_cap_consumer);
        ESP_LOGE(TAG, "capture task creation failed");
        return;
    }
    ESP_LOGI(TAG, "capture started (mono, 48 kHz)");
}

static void audio_writer_task(void *arg)
{
    (void)arg;
    s_writer_task = xTaskGetCurrentTaskHandle();
    ESP_LOGI(TAG, "audio_wr consumer started (%d Hz stereo)", AUDIO_SAMPLE_RATE);
    int64_t stat_window_us = esp_timer_get_time();
    int32_t stat_peak = 0;
    uint32_t stat_blocks_window = 0;
    uint32_t stat_silence = 0; /* silence blocks injected because the ring ran dry */
    for (;;) {
        if (!tud_audio_n_mounted(0)) {
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }
        /* Batch at least one millisecond of audio before handing it on: the
         * isochronous packets arrive 24 bytes at a time. */
        const uint32_t avail = ring_count();
        if (avail >= AUDIO_CHUNK_BYTES / 2) {
            const size_t want = avail < AUDIO_CHUNK_BYTES ? avail : AUDIO_CHUNK_BYTES;
            const TickType_t t0 = xTaskGetTickCount();
            const size_t n = ring_read(s_rx, want);
            (void)app_audio_codec_playback_write(APP_AUDIO_SOURCE_USB, AUDIO_SAMPLE_RATE, 2, s_rx,
                                                 n / 4);
            s_stat_last_write_ms = (uint32_t)((xTaskGetTickCount() - t0) * portTICK_PERIOD_MS);
            ++s_stat_blocks;
            /* Peak of what the host actually sent: distinguishes "no audio from
             * the PC" from "audio reaches the mixer but the speaker stays quiet". */
            for (size_t i = 0; i < n / 2; ++i) {
                const int32_t v = ((const int16_t *)s_rx)[i];
                const int32_t a = v < 0 ? -v : v;
                if (a > stat_peak) {
                    stat_peak = a;
                }
            }
            const int64_t now_us = esp_timer_get_time();
            if (now_us - stat_window_us >= 10 * 1000 * 1000) {
                ESP_LOGI(TAG, "audio_wr: peak=%d blocks/s=%u underruns=%u", (int)stat_peak,
                         (unsigned)(stat_blocks_window * 1000000u /
                                    (uint32_t)(now_us - stat_window_us)),
                         (unsigned)stat_silence);
                stat_window_us = now_us;
                stat_peak = 0;
                stat_blocks_window = 0;
                stat_silence = 0;
            }
            ++stat_blocks_window;
        } else {
            /* Nothing to hand on yet: wait for the OUT interrupt (or 5 ms) and
             * do NOT inject silence. A silence block here is consumed by the
             * mixer as if it were audio, so with 1 ms packets arriving slower
             * than the 2.5 ms threshold this wrote silence faster than real time:
             * the slot ring hit its full mark, dropped real audio, and the
             * speaker played a chopped mix (measured: ~800 silence blocks/s,
             * q_max = 4096 constantly). When the host really stops, the queue
             * simply drains and the mixer emits silence by itself. */
            (void)ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(5));
            continue;
        }
        (void)ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(5));
    }
}

/* ------------------------------------------------------------------- setup */


/* RPC 0x4800/1: bring the audio path up on demand. */
/* Local test tone: proves the I2S + codec path without any USB traffic. */
static esp_err_t uac2_start(void)
{
    /* I2S (TX + RX) is opened at boot, not on the first stream. */
    ESP_RETURN_ON_ERROR(app_audio_codec_preopen(AUDIO_SAMPLE_RATE), TAG, "codec preopen");
    ESP_RETURN_ON_ERROR(app_audio_codec_start_at(AUDIO_SAMPLE_RATE), TAG, "speaker codec");
#if AUDIO_SKIP_TASK
    ESP_LOGI(TAG, "codec started without playback task (diagnostic)");
    return ESP_OK;
#else
    /* The audio OUT FIFO is drained exclusively from tud_audio_rx_done_isr():
     * TinyUSB's FIFO is not thread safe, so a second polling reader would race
     * with the interrupt and corrupt the queue. */
    if (xTaskCreatePinnedToCore(audio_writer_task, "audio_wr", 4096, NULL, 3, NULL, 0) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    if (xTaskCreatePinnedToCore(diag_task, "audio_diag", 4096, NULL, 1, NULL, 0) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
#endif
}

/* The single TinyUSB callback surface lives in app_usb_audio.c and forwards
 * here; the settings page decides at boot which implementation is used. */
const app_usb_audio_ops_t app_usb_uac2_ops = {
    .desc = s_audio_desc,
    .desc_len = sizeof(s_audio_desc),
    .iface_count = AUDIO_IFACE_COUNT,
    .name = "uac2",
    .set_itf = uac2_set_itf,
    .set_itf_close = uac2_set_itf_close,
    .get_req_entity = uac2_get_req_entity,
    .set_req_entity = uac2_set_req_entity,
    .feedback_params = uac2_feedback_params,
    .rx_done_isr = uac2_rx_done_isr,
    .start = uac2_start,
};

void app_usb_uac2_set_itf(uint8_t as_itf, uint8_t cap_itf)
{
    s_as_itf = as_itf;
    s_cap_itf = cap_itf;
    ESP_LOGI(TAG, "audio interfaces: playback %u, capture %u", (unsigned)as_itf,
             (unsigned)cap_itf);
}
