// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "tusb.h"

/* Interface between the TinyUSB callback surface in app_usb_audio.c and one
 * UAC implementation.
 *
 * Both implementations are compiled in so the settings page can pick one at
 * boot; exactly one is registered with app_usb_core, and every TinyUSB callback
 * is forwarded here. Entries a variant does not use stay NULL. */
typedef struct {
    const uint8_t *desc;   /* interface block, numbered from interface 4 */
    size_t desc_len;
    uint8_t iface_count;
    const char *name;      /* "uac1" / "uac2", for logs */

    esp_err_t (*start)(void);
    bool (*streaming)(void);

    bool (*set_itf)(uint8_t rhport, tusb_control_request_t const *request);
    bool (*set_itf_close)(uint8_t rhport, tusb_control_request_t const *request);
    bool (*get_req_entity)(uint8_t rhport, tusb_control_request_t const *request);
    bool (*set_req_entity)(uint8_t rhport, tusb_control_request_t const *request,
                           uint8_t *buffer);
    bool (*get_req_ep)(uint8_t rhport, tusb_control_request_t const *request);
    bool (*set_req_ep)(uint8_t rhport, tusb_control_request_t const *request, uint8_t *buffer);
    void (*feedback_params)(uint8_t func_id, uint8_t alt_itf,
                            audio_feedback_params_t *feedback_param);
    bool (*rx_done_isr)(uint8_t rhport, uint16_t n_bytes_received, uint8_t func_id,
                        uint8_t ep_out, uint8_t cur_alt_setting);

    size_t (*build_config)(uint8_t *out, size_t cap, uint8_t base);
    void (*play_test_tone)(void);
    void (*start_on_demand)(void);
} app_usb_audio_ops_t;

extern const app_usb_audio_ops_t app_usb_uac1_ops;
extern const app_usb_audio_ops_t app_usb_uac2_ops;

/** Tell the UAC2 function the interface numbers it was registered with. */
void app_usb_uac2_set_itf(uint8_t as_itf, uint8_t cap_itf);
