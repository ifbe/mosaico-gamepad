// SPDX-License-Identifier: Apache-2.0

#include "app_usb_audio.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "tusb.h"

#include "app_audio_codec.h"
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

/* The HID class is compiled in because the ESP-Iris USB patch refers to its
 * constants unconditionally. This component registers no HID interface: the
 * gamepad's app_usb_core owns the HID class callbacks. */

/* Interface numbers are decided at registration time (ESP-Iris reports the
 * first free interface, which depends on whether its data link CDC exists). */
static uint8_t s_as_capture_itf = 6;

bool tud_audio_set_itf_cb(uint8_t rhport, tusb_control_request_t const *request)
{
    (void)rhport;
    /* SET_INTERFACE: wIndex = interface, wValue = alternate setting. */
    const uint8_t itf = (uint8_t)(request->wIndex & 0xFF);
    const uint8_t alt = (uint8_t)(request->wValue & 0xFF);
    ESP_LOGI(TAG, "stream itf=%u alt=%u", (unsigned)itf, (unsigned)alt);
    if (itf == s_as_capture_itf) {
        /* The host opened or closed the microphone stream. */
        capture_stream_start(alt != 0);
    } else if (itf == AUDIO_AS_IFACE) {
        /* Playback only reports its state; writing never asks permission. */
        if (alt != 0) {
            (void)app_audio_codec_playback_start(APP_AUDIO_SOURCE_USB, AUDIO_SAMPLE_RATE);
        } else {
            app_audio_codec_playback_stop(APP_AUDIO_SOURCE_USB);
        }
    }
    return true;
}

void tud_audio_feedback_params_cb(uint8_t func_id, uint8_t alt_itf,
                                  audio_feedback_params_t *feedback_param)
{
    (void)func_id;
    (void)alt_itf;
    /* The official usb_device_uac component reports the FIFO level; that keeps
     * the host pacing the stream to this device instead of to a fixed rate. */
    feedback_param->method = AUDIO_FEEDBACK_METHOD_FIFO_COUNT;
    feedback_param->sample_freq = AUDIO_SAMPLE_RATE;
}

bool tud_audio_set_itf_close_ep_cb(uint8_t rhport, tusb_control_request_t const *request)
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

bool tud_audio_get_req_entity_cb(uint8_t rhport, tusb_control_request_t const *request)
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

bool tud_audio_set_req_entity_cb(uint8_t rhport, tusb_control_request_t const *request,
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
static uint8_t s_audio_desc[] = {
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
/* Fed to the codec when the host is streaming but momentarily has no packet
 * pending: keeps the I2S DMA from replaying the last block (audible buzz). */
static const int16_t s_silence[AUDIO_CHUNK_FRAMES * AUDIO_CHANNELS];
/* Woken by the OUT interrupt as soon as a packet is queued. */
static TaskHandle_t s_writer_task;
/* Consecutive idle wakes; silence is only injected after a real gap. */
static uint32_t s_idle_ticks;
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
bool tud_audio_rx_done_isr(uint8_t rhport, uint16_t n_bytes_received, uint8_t func_id,
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
static uint8_t s_capture_stereo[CAPTURE_STEREO_BYTES];
static uint8_t s_capture_mono[CAPTURE_MONO_BYTES];

/* The ES8311 delivers stereo; the descriptor advertises one channel, so the two
 * samples of each frame are averaged. */
static app_audio_mic_consumer_t s_cap_consumer;
static int16_t s_cap_mono[240]; /* 5 ms at 48 kHz */

static void capture_task(void *arg)
{
    (void)arg;
    while (s_capture_active) {
        const size_t frames = app_audio_codec_mic_consumer_read(
            &s_cap_consumer, s_cap_mono, sizeof(s_cap_mono) / sizeof(s_cap_mono[0]));
        if (frames == 0) {
            vTaskDelay(pdMS_TO_TICKS(2));
            continue;
        }
        const size_t bytes = frames * AUDIO_CAPTURE_CHANNELS * AUDIO_BYTES_PER_SAMPLE;
        /* Always answer the host: an isochronous IN endpoint with nothing queued
         * degenerates into zero-length packets and the host reports dropouts. */
        (void)tud_audio_n_write(0, s_cap_mono, (uint16_t)bytes);
        s_stat_capture_bytes += bytes;
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
    for (;;) {
        if (!tud_audio_n_mounted(0)) {
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }
        /* Batch at least one millisecond of audio before handing it on: the
         * isochronous packets arrive 24 bytes at a time. */
        const uint32_t avail = ring_count();
        if (avail >= AUDIO_CHUNK_BYTES / 2) {
            s_idle_ticks = 0;
            const size_t want = avail < AUDIO_CHUNK_BYTES ? avail : AUDIO_CHUNK_BYTES;
            const TickType_t t0 = xTaskGetTickCount();
            const size_t n = ring_read(s_rx, want);
            (void)app_audio_codec_playback_write(APP_AUDIO_SOURCE_USB, AUDIO_SAMPLE_RATE, s_rx, n);
            s_stat_last_write_ms = (uint32_t)((xTaskGetTickCount() - t0) * portTICK_PERIOD_MS);
            ++s_stat_blocks;
        } else if (s_streaming && ++s_idle_ticks > 5) {
            /* Only after ~5 ms without a packet: filling every single gap with a
             * silence block chopped the waveform and was audible as noise. */
            (void)app_audio_codec_playback_write(APP_AUDIO_SOURCE_USB, AUDIO_SAMPLE_RATE,
                                                 s_silence, sizeof(s_silence));
            s_idle_ticks = 0;
        } else {
            /* Sleep until the OUT interrupt pushes a packet (or 5 ms), instead of
             * a whole 10 ms tick: batching at tick granularity starves the I2S
             * between bursts. */
            (void)ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(5));
            continue;
        }
        (void)ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(5));
    }
}

/* ------------------------------------------------------------------- setup */

size_t app_usb_audio_build_config(uint8_t *out, size_t cap, uint8_t base)
{
    const size_t total = 9 + sizeof(s_audio_desc);
    if (out == NULL || cap < total) {
        return 0;
    }
    out[0] = 9;
    out[1] = 0x02;
    out[2] = (uint8_t)(total & 0xFF);
    out[3] = (uint8_t)(total >> 8);
    out[4] = AUDIO_IFACE_COUNT;
    out[5] = 1;
    out[6] = 0;
    out[7] = 0x80;
    out[8] = 50;
    memcpy(&out[9], s_audio_desc, sizeof(s_audio_desc));
    return total;
}

static bool s_started;

/* RPC 0x4800/1: bring the audio path up on demand. */
static esp_err_t audio_control_rpc(const esp_iris_rpc_request_t *request, uint8_t *response,
                                   size_t capacity, size_t *response_size, void *ctx)
{
    (void)request;
    (void)ctx;
    if (!s_started) {
        const esp_err_t err = app_usb_audio_start();
        *response_size = (size_t)snprintf((char *)response, capacity, "{\"start\":\"%s\"}",
                                          esp_err_to_name(err));
        if (err != ESP_OK) {
            return err;
        }
        s_started = true;
    } else {
        diag_report();
        *response_size = (size_t)snprintf((char *)response, capacity,
                                          "{\"ring\":%u,\"blocks\":%u,\"bytes\":%u,\"last_ms\":%u,\"ovf\":%u}",
                                          (unsigned)ring_count(), (unsigned)s_stat_blocks,
                                          (unsigned)s_stat_bytes, (unsigned)s_stat_last_write_ms,
                                          (unsigned)s_stat_overflows);
    }
    return ESP_OK;
}

/* Local test tone: proves the I2S + codec path without any USB traffic. */
void app_audio_play_test_tone(void)
{
    if (!s_started) {
        const esp_err_t err = app_usb_audio_start();
        ESP_LOGI(TAG, "tone: starting audio path: %s", esp_err_to_name(err));
        if (err != ESP_OK) {
            return;
        }
        s_started = true;
    }
    /* Test tone always at full volume so the hardware path can be judged. */
    (void)app_audio_codec_set_volume(100);
    ESP_LOGI(TAG, "test tone: volume 100%%");
    static int16_t tone[AUDIO_CHUNK_FRAMES * AUDIO_CHANNELS];
    const float step = 2.0f * 3.14159265f * 440.0f / (float)AUDIO_SAMPLE_RATE;
    float phase = 0;
    ESP_LOGI(TAG, "test tone: start");
    for (int block = 0; block < 800; ++block) { /* 800 * 2.5 ms = 2 s */
        for (size_t i = 0; i < AUDIO_CHUNK_FRAMES; ++i) {
            const int16_t v = (int16_t)(sinf(phase) * 26000.0f);
            tone[i * 2] = v;
            tone[i * 2 + 1] = v;
            phase += step;
            if (phase >= 6.2831853f) {
                phase -= 6.2831853f;
            }
        }
        const TickType_t t0 = xTaskGetTickCount();
        const esp_err_t werr = app_audio_codec_playback_write(
            APP_AUDIO_SOURCE_LOCAL, AUDIO_SAMPLE_RATE, tone, sizeof(tone));
        const uint32_t wms = (uint32_t)((xTaskGetTickCount() - t0) * portTICK_PERIOD_MS);
        if (block < 4 || werr != ESP_OK) {
            ESP_LOGW(TAG, "tone block %d took %ums err=%s", block, (unsigned)wms,
                     esp_err_to_name(werr));
        }
        if (werr != ESP_OK) {
            break;
        }
    }
    ESP_LOGI(TAG, "test tone finished");
}

void app_audio_start_on_demand(void)
{
    if (s_started) {
        diag_report();
        return;
    }
    const esp_err_t err = app_usb_audio_start();
    ESP_LOGI(TAG, "button start audio: %s", esp_err_to_name(err));
    if (err == ESP_OK) {
        s_started = true;
    }
}

static void rpc_register_task(void *arg)
{
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(3000));
    const esp_err_t err = esp_iris_rpc_register(0x4800, 1, audio_control_rpc, NULL);
    ESP_LOGI(TAG, "control rpc registration: %s", esp_err_to_name(err));
    vTaskDelete(NULL);
}

void app_audio_control_register(void)
{
    if (xTaskCreate(rpc_register_task, "rpc_reg", 3072, NULL, 2, NULL) != pdPASS) {
        ESP_LOGE(TAG, "cannot start rpc registration task");
    }
}

/* Move the capture endpoint to whichever IN endpoint the HID classes left free.
 * The descriptor is a byte template; the endpoint descriptor is the only place
 * carrying {0x07, 0x05, AUDIO_EP_IN_ADDR}. */
static void patch_capture_endpoint(uint8_t ep_in)
{
    for (size_t i = 0; i + 2 < sizeof(s_audio_desc); ++i) {
        if (s_audio_desc[i] == 0x07 && s_audio_desc[i + 1] == 0x05 &&
            s_audio_desc[i + 2] == AUDIO_EP_IN_ADDR) {
            s_audio_desc[i + 2] = ep_in;
            return;
        }
    }
    ESP_LOGW(TAG, "capture endpoint descriptor not found");
}

/* Rewrite the interface numbers in the template so the function can start at
 * whatever interface ESP-Iris reports (1 CDC pair -> 2, 2 pairs -> 4). */
static void patch_interface_numbers(uint8_t base, uint8_t count)
{
    uint8_t next = base;
    for (size_t off = 0; off + 3 < sizeof(s_audio_desc); off += s_audio_desc[off]) {
        const uint8_t len = s_audio_desc[off];
        if (len < 2) {
            break;
        }
        const uint8_t type = s_audio_desc[off + 1];
        if (type == 0x04 && len >= 4) { /* INTERFACE */
            if (s_audio_desc[off + 3] == 0) {
                s_audio_desc[off + 2] = next++;
            } else {
                s_audio_desc[off + 2] = (uint8_t)(next - 1);
            }
        } else if (type == 0x0B && len >= 4) { /* IAD */
            s_audio_desc[off + 2] = base;
            s_audio_desc[off + 3] = count;
        }
    }
}

esp_err_t app_usb_audio_register(uint8_t mic_ep_in)
{
    if (mic_ep_in == 0) {
        ESP_LOGE(TAG, "no free IN endpoint for the microphone; USB audio skipped");
        return ESP_ERR_NOT_FOUND;
    }
    const uint8_t base = esp_iris_usb_first_free_interface();
    patch_capture_endpoint(mic_ep_in);
    patch_interface_numbers(base, AUDIO_IFACE_COUNT);
    s_as_capture_itf = (uint8_t)(base + AUDIO_IFACE_COUNT - 1);
    ESP_RETURN_ON_ERROR(esp_iris_usb_set_extra_interfaces(
                            s_audio_desc, sizeof(s_audio_desc), AUDIO_IFACE_COUNT),
                        TAG, "register audio interfaces");
    ESP_LOGI(TAG, "UAC2 registered: %u bytes, mic ep 0x%02X, interfaces %u..%u",
             (unsigned)sizeof(s_audio_desc), (unsigned)mic_ep_in, base,
             (unsigned)(base + AUDIO_IFACE_COUNT - 1));
#if defined(CFG_TUD_DWC2_DMA_ENABLE) && CFG_TUD_DWC2_DMA_ENABLE
    ESP_LOGI(TAG, "usb backend: DMA");
#else
    ESP_LOGI(TAG, "usb backend: slave (non-DMA)");
#endif
#if defined(CFG_TUD_AUDIO_FUNC_1_EP_OUT_SW_BUF_SZ)
    ESP_LOGI(TAG, "tinyusb out fifo %d bytes, ep max %d",
             CFG_TUD_AUDIO_FUNC_1_EP_OUT_SW_BUF_SZ, CFG_TUD_AUDIO_FUNC_1_EP_OUT_SZ_MAX);
#else
    ESP_LOGW(TAG, "tinyusb out fifo size not visible from this component");
#endif
    return ESP_OK;
}

bool app_usb_audio_streaming(void)
{
    return s_streaming;
}

esp_err_t app_usb_audio_start(void)
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

