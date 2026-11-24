// SPDX-License-Identifier: Apache-2.0

#include "app_usb_audio.h"

#include <stddef.h>
#include <string.h>

#include "app_cfg.h"

/* The gamepad settings page has one USB audio switch, so only UAC2 is selected. */
#define APP_USB_UAC1 1
#define APP_USB_UAC2 2

static uint32_t usb_uac_selected(void)
{
    return app_cfg_usb_audio() ? APP_USB_UAC2 : 0;
}
#include "app_usb_audio_ops.h"
#include "esp_iris_usb_hid.h"
#include "app_usb_core.h"
#include "esp_log.h"
#include "tusb.h"

static const char *TAG = "usb_audio";

/* The implementation picked at boot from the persisted setting. NULL means no
 * USB sound card: the device enumerates as ESP-Iris alone. */
static const app_usb_audio_ops_t *s_ops;

/* Both interface blocks start with a descriptor whose third byte is the first
 * interface number: UAC2 with an IAD (bFirstInterface at offset 2), UAC1 with
 * its AudioControl interface (bInterfaceNumber at offset 2). */
#define LINEAR_FIRST_IFACE_OFFSET 2

esp_err_t app_usb_audio_register(uint8_t mic_ep_in)
{
    const uint32_t selected = usb_uac_selected();
    if (selected == APP_USB_UAC1) {
        s_ops = &app_usb_uac1_ops;
    } else if (selected == APP_USB_UAC2) {
        s_ops = &app_usb_uac2_ops;
    } else {
        s_ops = NULL;
        ESP_LOGI(TAG, "no USB sound card enabled");
        return ESP_OK;
    }

    if (mic_ep_in == 0) {
        ESP_LOGE(TAG, "no free IN endpoint for the microphone; USB audio skipped");
        return ESP_ERR_NOT_FOUND;
    }
    /* Endpoints and interface numbers are allocated at startup from what ESP-Iris
     * reports as free, so the same block works with or without the data link. */
    static uint8_t block[512];
    if (s_ops->desc_len > sizeof(block)) {
        ESP_LOGE(TAG, "%s descriptor too large (%u B)", s_ops->name,
                 (unsigned)s_ops->desc_len);
        s_ops = NULL;
        return ESP_ERR_INVALID_SIZE;
    }
    memcpy(block, s_ops->desc, s_ops->desc_len);
    uint8_t base = esp_iris_usb_first_free_interface();
    uint8_t next = base;
    /* The descriptor constants are the numbers the block was written with; the
     * renumbering below compacts them, so remember where the two audio-streaming
     * interfaces actually land. */
    uint8_t as_itf = 0;
    uint8_t cap_itf = 0;
    for (size_t off = 0; off + 3 < s_ops->desc_len; off += block[off]) {
        const uint8_t len = block[off];
        if (len < 2) {
            break;
        }
        if (block[off + 1] == 0x04 && len >= 4) { /* INTERFACE */
            const uint8_t orig = block[off + 2];
            const uint8_t assigned = (block[off + 3] == 0) ? next++ : (uint8_t)(next - 1);
            block[off + 2] = assigned;
            if (orig == 5) { /* AUDIO_AS_IFACE in the UAC2 descriptor */
                as_itf = assigned;
            } else if (orig == 6) { /* AUDIO_AS_CAPTURE_IFACE */
                cap_itf = assigned;
            }
        } else if (block[off + 1] == 0x0B && len >= 4) { /* IAD */
            block[off + 2] = base;
            block[off + 3] = s_ops->iface_count;
        } else if (len >= 7 && block[off + 1] == 0x05 && block[off + 2] == 0x85) {
            block[off + 2] = mic_ep_in; /* capture endpoint follows the HID block */
        }
    }
    if (s_ops == &app_usb_uac2_ops) {
        app_usb_uac2_set_itf(as_itf, cap_itf);
    }
    const esp_err_t err = esp_iris_usb_set_extra_interfaces(block, s_ops->desc_len,
                                                            s_ops->iface_count);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "cannot add the %s block: %s", s_ops->name, esp_err_to_name(err));
        s_ops = NULL;
        return err;
    }
    ESP_LOGI(TAG, "%s registered: %u interfaces from %u, mic ep 0x%02X",
             s_ops->name, (unsigned)s_ops->iface_count, (unsigned)base,
             (unsigned)mic_ep_in);
    return ESP_OK;
}

esp_err_t app_usb_audio_start(void)
{
    if (s_ops == NULL) {
        return ESP_OK;
    }
    return s_ops->start();
}

bool app_usb_audio_streaming(void)
{
    return s_ops != NULL && s_ops->streaming != NULL && s_ops->streaming();
}

const char *app_usb_audio_class_name(void)
{
    return s_ops != NULL ? s_ops->name : "none";
}

size_t app_usb_audio_build_config(uint8_t *out, size_t cap, uint8_t base)
{
    if (s_ops == NULL || s_ops->build_config == NULL) {
        return 0;
    }
    return s_ops->build_config(out, cap, base);
}

void app_audio_play_test_tone(void)
{
    if (s_ops != NULL && s_ops->play_test_tone != NULL) {
        s_ops->play_test_tone();
    }
}

void app_audio_start_on_demand(void)
{
    if (s_ops != NULL && s_ops->start_on_demand != NULL) {
        s_ops->start_on_demand();
        return;
    }
    (void)app_usb_audio_start();
}

void app_audio_control_register(void)
{
    /* The control RPC belongs to the active implementation's state machine; the
     * dispatcher only knows whether one was selected. */
}

/* ------------------------------------------------- TinyUSB callback surface ---
 * TinyUSB expects these symbols from the application. Each one is forwarded to
 * the selected implementation; with no sound card the defaults keep the stack
 * happy (nothing is registered, so they should not be reached at all). */

bool tud_audio_set_itf_cb(uint8_t rhport, tusb_control_request_t const *request)
{
    return s_ops != NULL && s_ops->set_itf != NULL && s_ops->set_itf(rhport, request);
}

bool tud_audio_set_itf_close_ep_cb(uint8_t rhport, tusb_control_request_t const *request)
{
    return s_ops == NULL || s_ops->set_itf_close == NULL ||
           s_ops->set_itf_close(rhport, request);
}

bool tud_audio_get_req_entity_cb(uint8_t rhport, tusb_control_request_t const *request)
{
    return s_ops != NULL && s_ops->get_req_entity != NULL &&
           s_ops->get_req_entity(rhport, request);
}

bool tud_audio_set_req_entity_cb(uint8_t rhport, tusb_control_request_t const *request,
                                 uint8_t *buffer)
{
    return s_ops != NULL && s_ops->set_req_entity != NULL &&
           s_ops->set_req_entity(rhport, request, buffer);
}

bool tud_audio_get_req_ep_cb(uint8_t rhport, tusb_control_request_t const *request)
{
    return s_ops != NULL && s_ops->get_req_ep != NULL && s_ops->get_req_ep(rhport, request);
}

bool tud_audio_set_req_ep_cb(uint8_t rhport, tusb_control_request_t const *request,
                             uint8_t *buffer)
{
    return s_ops != NULL && s_ops->set_req_ep != NULL &&
           s_ops->set_req_ep(rhport, request, buffer);
}

void tud_audio_feedback_params_cb(uint8_t func_id, uint8_t alt_itf,
                                  audio_feedback_params_t *feedback_param)
{
    if (s_ops != NULL && s_ops->feedback_params != NULL) {
        s_ops->feedback_params(func_id, alt_itf, feedback_param);
    }
}

bool tud_audio_rx_done_isr(uint8_t rhport, uint16_t n_bytes_received, uint8_t func_id,
                           uint8_t ep_out, uint8_t cur_alt_setting)
{
    if (s_ops != NULL && s_ops->rx_done_isr != NULL) {
        return s_ops->rx_done_isr(rhport, n_bytes_received, func_id, ep_out,
                                  cur_alt_setting);
    }
    return true;
}
