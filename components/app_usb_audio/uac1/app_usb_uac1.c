// SPDX-License-Identifier: Apache-2.0

#include "app_usb_audio.h"

#include "app_usb_audio_ops.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "tusb.h"

#include "app_audio_codec.h"
#include "esp_iris.h"
#include "class/audio/audio_device.h"
#include "class/hid/hid_device.h"
#include "esp_check.h"
#include "app_usb_core.h"
#include "esp_iris_usb_hid.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "usb_audio";

/* Entity identifiers inside the audio function. */
#define AUDIO_SKIP_TASK 0
#define AUDIO_BASE_IFACE 4
#define AUDIO_AS_IFACE 5
#define AUDIO_SAMPLE_RATE 48000
/* UAC1 has no clock source entity. */
#define UAC_ENTITY_IN_TERMINAL 1
#define UAC_ENTITY_FEATURE 2
#define UAC_ENTITY_OUT_TERMINAL 3

/* Volume in 1/256 dB units: -60 dB .. 0 dB. */
#define UAC_VOL_MIN (-15360)
#define UAC_VOL_MAX 0

static int16_t s_volume = -3072; /* -12 dB */
static uint8_t s_mute;

/* ---------------------------------------------------------------- callbacks */

/* The HID class is compiled in because the ESP-Iris USB patch refers to its
 * constants unconditionally, but this application registers no HID interface.
 * TinyUSB still needs the three class callbacks to link. */
static bool uac1_set_itf(uint8_t rhport, tusb_control_request_t const *request)
{
    (void)rhport;
    /* SET_INTERFACE: wIndex = interface, wValue = alternate setting. */
    ESP_LOGI(TAG, "stream itf=%u alt=%u", (unsigned)(request->wIndex & 0xFF),
             (unsigned)(request->wValue & 0xFF));
    return true;
}


static bool uac1_set_itf_close(uint8_t rhport, tusb_control_request_t const *request)
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

static bool uac1_get_req_entity(uint8_t rhport, tusb_control_request_t const *request)
{
    const audio_control_request_t *req = (audio_control_request_t const *)request;

    if (req->bEntityID != UAC_ENTITY_FEATURE) {
        return false;
    }

    /* UAC1 uses separate GET_MIN / GET_MAX / GET_RES requests. */
    if (req->bControlSelector == AUDIO10_FU_CTRL_VOLUME) {
        switch (req->bRequest) {
        case AUDIO10_CS_REQ_GET_CUR: {
            int16_t value = s_volume;
            return tud_control_xfer(rhport, request, &value, sizeof(value));
        }
        case AUDIO10_CS_REQ_GET_MIN: {
            int16_t value = (int16_t)UAC_VOL_MIN;
            return tud_control_xfer(rhport, request, &value, sizeof(value));
        }
        case AUDIO10_CS_REQ_GET_MAX: {
            int16_t value = (int16_t)UAC_VOL_MAX;
            return tud_control_xfer(rhport, request, &value, sizeof(value));
        }
        case AUDIO10_CS_REQ_GET_RES: {
            int16_t value = 1;
            return tud_control_xfer(rhport, request, &value, sizeof(value));
        }
        default:
            return false;
        }
    }
    if (req->bControlSelector == AUDIO10_FU_CTRL_MUTE) {
        if (req->bRequest == AUDIO10_CS_REQ_GET_CUR) {
            uint8_t value = s_mute;
            return tud_control_xfer(rhport, request, &value, sizeof(value));
        }
        return false;
    }
    return false;
}

static bool uac1_set_req_entity(uint8_t rhport, tusb_control_request_t const *request,
                                 uint8_t *buffer)
{
    (void)rhport;
    const uint8_t entity = (uint8_t)(request->wIndex >> 8);
    const uint8_t cs = (uint8_t)(request->wValue >> 8);
    if (entity != UAC_ENTITY_FEATURE || request->bRequest != AUDIO10_CS_REQ_SET_CUR) {
        return false;
    }
    if (cs == AUDIO10_FU_CTRL_VOLUME) {
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
    if (cs == AUDIO10_FU_CTRL_MUTE) {
        s_mute = buffer[0] ? 1 : 0;
        apply_volume();
        ESP_LOGI(TAG, "host mute %u", s_mute);
        return true;
    }
    return false;
}

/* ------------------------------------------------------- endpoint controls */

/* UAC1 exposes a sampling-frequency control on the streaming endpoint. macOS
 * queries it while starting the stream; stalling it makes the driver close the
 * stream immediately after SET_INTERFACE(alt=1). */
static bool uac1_get_req_ep(uint8_t rhport, tusb_control_request_t const *request)
{
    const audio_control_request_t *req = (audio_control_request_t const *)request;
    if (req->bControlSelector == AUDIO10_EP_CTRL_SAMPLING_FREQ) {
        if (req->bRequest == AUDIO10_CS_REQ_GET_CUR) {
            uint32_t freq = AUDIO_SAMPLE_RATE;
            return tud_control_xfer(rhport, request, &freq, sizeof(freq));
        }
        if (req->bRequest == AUDIO10_CS_REQ_GET_MIN || req->bRequest == AUDIO10_CS_REQ_GET_MAX) {
            uint32_t freq = AUDIO_SAMPLE_RATE;
            return tud_control_xfer(rhport, request, &freq, sizeof(freq));
        }
        if (req->bRequest == AUDIO10_CS_REQ_GET_RES) {
            uint32_t freq = 0;
            return tud_control_xfer(rhport, request, &freq, sizeof(freq));
        }
    }
    return false;
}

static bool uac1_set_req_ep(uint8_t rhport, tusb_control_request_t const *request,
                             uint8_t *buffer)
{
    (void)rhport;
    const audio_control_request_t *req = (audio_control_request_t const *)request;
    if (req->bControlSelector == AUDIO10_EP_CTRL_SAMPLING_FREQ &&
        req->bRequest == AUDIO10_CS_REQ_SET_CUR) {
        uint32_t freq = 0;
        memcpy(&freq, buffer, sizeof(freq));
        ESP_LOGI(TAG, "host set endpoint sample rate %u Hz", (unsigned)freq);
        return true;
    }
    return false;
}

/* ------------------------------------------------------------- descriptors */

#define AUDIO_IFACE_COUNT 2 /* AudioControl + AudioStreaming */
#define AUDIO_BASE_IFACE 4
#define AUDIO_AS_IFACE 5
#define AUDIO_CHANNELS 2
#define AUDIO_BYTES_PER_SAMPLE 2
#define AUDIO_BIT_RESOLUTION 16
#define AUDIO_EP_OUT_ADDR 0x06
#define AUDIO_EP_FB_ADDR 0x86
#define AUDIO_EP_SIZE 192 /* one millisecond of audio per packet */

/* Volume and mute for master and both channels. */
#define AUDIO_FU_CTRL \
    (AUDIO20_CTRL_RW << AUDIO20_FEATURE_UNIT_CTRL_MUTE_POS | \
     AUDIO20_CTRL_RW << AUDIO20_FEATURE_UNIT_CTRL_VOLUME_POS)

/* USB Audio Class 1.0 playback function. UAC1 has no clock-source entity and
 * carries the streaming interface number in the AC header, so the descriptor is
 * written as bytes instead of UAC2 macros. Protocol code 0x00 marks UAC1. */
static const uint8_t s_audio_desc[] = {
    /* No IAD: UAC1 predates the association descriptor and AppleUSBAudio
     * mis-parses the function when one is present. */
    /* AudioControl interface (class 1, subclass 1, protocol 0) */
    0x09, 0x04, AUDIO_BASE_IFACE, 0x00, 0x00, 0x01, 0x01, 0x00, 0x00,
    /* AC header: bcdADC 1.00, 40 bytes of class-specific data, one streaming
     * interface */
    0x09, 0x24, 0x01, 0x00, 0x01, 0x28, 0x00, 0x01, AUDIO_AS_IFACE,
    /* Input terminal: USB streaming, two channels front L/R, terminal id 1 */
    0x0C, 0x24, 0x02, UAC_ENTITY_IN_TERMINAL, 0x01, 0x01, 0x00, AUDIO_CHANNELS,
    0x03, 0x00, 0x00, 0x00,
    /* Feature unit id 2: master mute + volume */
    0x0A, 0x24, 0x06, UAC_ENTITY_FEATURE, UAC_ENTITY_IN_TERMINAL, 0x01, 0x03,
    0x00, 0x00, 0x00,
    /* Output terminal id 3: speaker, sourced from the feature unit */
    0x09, 0x24, 0x03, UAC_ENTITY_OUT_TERMINAL, 0x01, 0x03, 0x00,
    UAC_ENTITY_FEATURE, 0x00,
    /* AudioStreaming interface, alternate setting 0 */
    0x09, 0x04, AUDIO_AS_IFACE, 0x00, 0x00, 0x01, 0x02, 0x00, 0x00,
    0x07, 0x24, 0x01, UAC_ENTITY_IN_TERMINAL, 0x00, 0x01, 0x00,
    /* AudioStreaming interface, alternate setting 1 (isochronous OUT) */
    0x09, 0x04, AUDIO_AS_IFACE, 0x01, 0x01, 0x01, 0x02, 0x00, 0x00,
    0x07, 0x24, 0x01, UAC_ENTITY_IN_TERMINAL, 0x00, 0x01, 0x00,
    /* Format type I: 2 channels, 16-bit, 48000 Hz */
    0x0B, 0x24, 0x02, 0x01, AUDIO_CHANNELS, AUDIO_BYTES_PER_SAMPLE,
    AUDIO_BIT_RESOLUTION, 0x01, 0x80, 0xBB, 0x00,
    /* Standard isochronous OUT endpoint, adaptive, 192 bytes per millisecond.
     * bInterval 4 at high speed means one packet every 8 microframes (1 ms);
     * AppleUSBAudio only exposes output channels with this layout for UAC1. */
    0x09, 0x05, AUDIO_EP_OUT_ADDR, 0x09, (uint8_t)(AUDIO_EP_SIZE & 0xFF),
    (uint8_t)(AUDIO_EP_SIZE >> 8), 0x04, 0x00, 0x00,
    /* Class-specific isochronous data endpoint */
    0x07, 0x25, 0x01, 0x00, 0x00, 0x00, 0x00,
};

#define AUDIO_CHUNK_FRAMES 120 /* 2.5 ms */
#define AUDIO_CHUNK_BYTES (AUDIO_CHUNK_FRAMES * AUDIO_CHANNELS * 2)

static uint8_t s_rx[AUDIO_CHUNK_BYTES * 2];
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

static size_t ring_write(const uint8_t *src, size_t len)
{
    size_t written = 0;
    while (written < len && ring_count() < AUDIO_RING_BYTES) {
        s_ring[s_ring_head & AUDIO_RING_MASK] = src[written++];
        ++s_ring_head;
    }
    return written;
}

static size_t ring_read(uint8_t *dst, size_t len)
{
    size_t read = 0;
    while (read < len && ring_count() > 0) {
        dst[read++] = s_ring[s_ring_tail & AUDIO_RING_MASK];
        ++s_ring_tail;
    }
    return read;
}

static const int16_t s_silence[AUDIO_CHUNK_FRAMES * AUDIO_CHANNELS];

/* Counters kept for the diagnostics log. */
static volatile uint32_t s_stat_blocks;
static volatile uint32_t s_stat_bytes;
static volatile uint32_t s_stat_last_write_ms;
static volatile uint32_t s_stat_overflows;

static void diag_report(void)
{
    /* Never call TinyUSB from here: this task runs on the other core and the
     * stack is not thread safe. Only local counters are read. */
    ESP_LOGI(TAG, "ring=%u blocks=%u bytes=%u last=%ums ovf=%u stream=%d",
             (unsigned)ring_count(), (unsigned)s_stat_blocks, (unsigned)s_stat_bytes,
             (unsigned)s_stat_last_write_ms, (unsigned)s_stat_overflows, (int)s_streaming);
}

static void diag_task(void *arg)
{
    (void)arg;
    for (;;) {
        diag_report();
        vTaskDelay(pdMS_TO_TICKS(2000));
    }
}

static void audio_reader_task(void *arg)
{
    (void)arg;
    for (;;) {
        if (!tud_audio_n_mounted(0)) {
            s_streaming = false;
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }
        /* Keep reading as fast as the FIFO allows: the isochronous OUT transfer
         * is only re-armed by the stack while the application drains it. */
        bool got_any = false;
        for (;;) {
            const uint16_t available = tud_audio_n_available(0);
            if (available == 0) {
                break;
            }
            const uint16_t room = (uint16_t)(AUDIO_RING_BYTES - ring_count());
            if (room == 0) {
                ++s_stat_overflows;
                break;
            }
            const uint16_t got = tud_audio_n_read(0, s_rx, available < room ? available : room);
            if (got == 0) {
                break;
            }
            ring_write(s_rx, got);
            s_stat_bytes += got;
            got_any = true;
        }
        if (got_any && !s_streaming) {
            s_streaming = true;
            ESP_LOGI(TAG, "streaming host audio");
        }
        /* Keep the polling continuous: the isochronous OUT transfer is only
         * re-armed while the application keeps draining the FIFO. */
        vTaskDelay(pdMS_TO_TICKS(1));
    }
}

static void audio_writer_task(void *arg)
{
    (void)arg;
    for (;;) {
        if (!tud_audio_n_mounted(0)) {
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }
        if (ring_count() >= AUDIO_CHUNK_BYTES) {
            const TickType_t t0 = xTaskGetTickCount();
            const size_t n = ring_read(s_rx, AUDIO_CHUNK_BYTES);
            (void)app_audio_codec_playback_write(APP_AUDIO_SOURCE_USB, 48000, 2, s_rx, n / 4);
            s_stat_last_write_ms = (uint32_t)((xTaskGetTickCount() - t0) * portTICK_PERIOD_MS);
            ++s_stat_blocks;
        } else {
            /* Never let the I2S DMA starve: an underrun makes it replay the
             * previous block. */
            (void)app_audio_codec_playback_write(APP_AUDIO_SOURCE_USB, 48000, 2, s_silence,
                                                 sizeof(s_silence) / 4);
        }
        vTaskDelay(pdMS_TO_TICKS(1));
    }
}

/* ------------------------------------------------------------------- setup */


/* RPC 0x4800/1: bring the audio path up on demand. */
/* Local test tone: proves the I2S + codec path without any USB traffic. */
static esp_err_t uac1_start(void)
{
    ESP_RETURN_ON_ERROR(app_audio_codec_playback_start(APP_AUDIO_SOURCE_USB, 48000), TAG,
                        "speaker codec");
#if AUDIO_SKIP_TASK
    ESP_LOGI(TAG, "codec started without playback task (diagnostic)");
    return ESP_OK;
#else
    /* Pin the playback task away from the core that runs the USB stack and keep
     * it at a low priority: a starved USB task breaks ESP-Iris' device link. */
    if (xTaskCreatePinnedToCore(audio_reader_task, "audio_rd", 4096, NULL, 4, NULL, 1) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
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
const app_usb_audio_ops_t app_usb_uac1_ops = {
    .desc = s_audio_desc,
    .desc_len = sizeof(s_audio_desc),
    .iface_count = AUDIO_IFACE_COUNT,
    .name = "uac1",
    .set_itf = uac1_set_itf,
    .set_itf_close = uac1_set_itf_close,
    .get_req_entity = uac1_get_req_entity,
    .set_req_entity = uac1_set_req_entity,
    .get_req_ep = uac1_get_req_ep,
    .set_req_ep = uac1_set_req_ep,
    .start = uac1_start,
};
