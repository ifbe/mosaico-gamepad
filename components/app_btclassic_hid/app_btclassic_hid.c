// SPDX-License-Identifier: Apache-2.0

#include "app_btclassic_hid.h"

#include "app_bt_core.h"
#include "esp_check.h"
#include "esp_gap_bt_api.h"
#include "esp_hidd.h"
#include "esp_log.h"
#include "esp_timer.h"

static const char *TAG = "btclassic_hid";

static esp_hidd_dev_t *s_dev;
static bool s_connected;
/* esp_hid only clears dev->connected on ESP_HIDD_CLOSE_EVT, and that event is
 * not always delivered: the flag then stays true and every report goes to a
 * dead link, which Bluedroid answers with a "HID device not connected!" line per
 * report (~62 per second). A failed send therefore mutes reports for a short
 * while instead of forever - the next attempt inside that window is skipped, and
 * once the link is healthy again the first retry resumes the stream. */
#define HID_SEND_RETRY_US (1000 * 1000)
static int64_t s_mute_until_us;
static bool s_mute_logged;

/* Apply the ACL poll interval once the link is out of sniff. The HID connect
 * event carries no peer address, so use the one app_bt_core cached from the HFP
 * stack-up event, falling back to the first bonded device. */
#define HID_QOS_DELAY_MS 200
#define HID_QOS_POLL 20 /* 12.5 ms */

static esp_timer_handle_t s_qos_timer;

static void qos_apply(void *arg)
{
    (void)arg;
    uint8_t peer[6];
    if (!app_bt_core_classic_peer(peer)) {
        const int bonded = esp_bt_gap_get_bond_device_num();
        esp_bd_addr_t list[1];
        int count = bonded > 0 ? 1 : 0;
        if (count == 0 || esp_bt_gap_get_bond_device_list(&count, list) != ESP_OK) {
            ESP_LOGW(TAG, "no peer for QoS");
            return;
        }
        memcpy(peer, list[0], sizeof(peer));
    }
    const esp_err_t err = esp_bt_gap_set_qos(peer, HID_QOS_POLL);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "qos: %s", esp_err_to_name(err));
    }
}

static void schedule_qos(void)
{
    if (s_qos_timer == NULL) {
        const esp_timer_create_args_t args = {
            .callback = qos_apply,
            .name = "hid_qos",
        };
        if (esp_timer_create(&args, &s_qos_timer) != ESP_OK) {
            ESP_LOGW(TAG, "qos timer unavailable");
            return;
        }
    }
    (void)esp_timer_stop(s_qos_timer);
    (void)esp_timer_start_once(s_qos_timer, HID_QOS_DELAY_MS * 1000);
}

static void hidd_event_handler(void *handler_args, esp_event_base_t base, int32_t id,
                               void *event_data)
{
    (void)handler_args;
    (void)base;
    (void)event_data;
    switch ((esp_hidd_event_t)id) {
    case ESP_HIDD_CONNECT_EVENT: {
        s_connected = true;
        s_mute_until_us = 0;
        s_mute_logged = false;
        ESP_LOGI(TAG, "host connected");
        /* Bluedroid asks for sniff on this link after every report, so take it
         * out of sniff now and let app_bt_core's periodic kick keep it out. */
        (void)app_bt_core_kick_acl_sniff();
        /* Short ACL poll interval keeps reports prompt. It has to be applied to
         * the connected peer only, and only once the link is active again: the
         * controller rejects HCI_QoS_Setup with 0x02 (invalid parameters) while
         * the link is still in sniff or not fully up, so defer it briefly. */
        schedule_qos();
        break;
    }
    case ESP_HIDD_DISCONNECT_EVENT:
        s_connected = false;
        s_mute_until_us = 0;
        s_mute_logged = false;
        ESP_LOGI(TAG, "host disconnected");
        break;
    case ESP_HIDD_START_EVENT:
        ESP_LOGI(TAG, "HID device started");
        break;
    default:
        break;
    }
}

esp_err_t app_btclassic_hid_start(const uint8_t *report_map, size_t report_map_len)
{
    if (report_map == NULL || report_map_len == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_hid_raw_report_map_t maps[1] = {
        {.data = (uint8_t *)report_map, .len = (uint16_t)report_map_len},
    };
    esp_hid_device_config_t hid_config = {
        .vendor_id = 0x303A,
        .product_id = 0x4002,
        .version = 0x0100,
        .device_name = app_bt_core_classic_name(),
        .manufacturer_name = "Espressif",
        .serial_number = "000000000000",
        .report_maps = maps,
        .report_maps_len = 1,
    };
    ESP_RETURN_ON_ERROR(esp_hidd_dev_init(&hid_config, ESP_HID_TRANSPORT_BT,
                                          hidd_event_handler, &s_dev),
                        TAG, "hidd init");

    /* HID over BR/EDR needs SDP plus an explicit discoverable request. */
    ESP_RETURN_ON_ERROR(esp_bt_gap_set_scan_mode(ESP_BT_CONNECTABLE,
                                                 ESP_BT_GENERAL_DISCOVERABLE),
                        TAG, "scan mode");
    ESP_LOGI(TAG, "ready (%u-byte report map)", (unsigned)report_map_len);
    return ESP_OK;
}

bool app_btclassic_hid_connected(void)
{
    if (s_dev == NULL || !esp_hidd_dev_connected(s_dev)) {
        return false;
    }
    return s_mute_until_us == 0 || esp_timer_get_time() >= s_mute_until_us;
}

void app_btclassic_hid_send(uint8_t report_id, const uint8_t *data, size_t len)
{
    if (s_dev == NULL || !esp_hidd_dev_connected(s_dev)) {
        return;
    }
    const int64_t now = esp_timer_get_time();
    if (s_mute_until_us != 0 && now < s_mute_until_us) {
        return; /* still inside the retry window */
    }
    const esp_err_t err = esp_hidd_dev_input_set(s_dev, 0, report_id,
                                                 (uint8_t *)data, (uint16_t)len);
    if (err != ESP_OK) {
        s_mute_until_us = now + HID_SEND_RETRY_US;
        if (!s_mute_logged) {
            s_mute_logged = true;
            ESP_LOGW(TAG, "send failed (%s); retrying in %d ms",
                     esp_err_to_name(err), (int)(HID_SEND_RETRY_US / 1000));
        }
    } else if (s_mute_logged) {
        s_mute_logged = false;
        s_mute_until_us = 0;
        ESP_LOGI(TAG, "reports flowing again");
    }
}
