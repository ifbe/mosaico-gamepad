// SPDX-License-Identifier: Apache-2.0

#include "app_btclassic_hid.h"

#include "app_bt_core.h"
#include "esp_check.h"
#include "esp_gap_bt_api.h"
#include "esp_hidd.h"
#include "esp_log.h"

static const char *TAG = "btclassic_hid";

static esp_hidd_dev_t *s_dev;
static bool s_connected;

static void hidd_event_handler(void *handler_args, esp_event_base_t base, int32_t id,
                               void *event_data)
{
    (void)handler_args;
    (void)base;
    (void)event_data;
    switch ((esp_hidd_event_t)id) {
    case ESP_HIDD_CONNECT_EVENT: {
        s_connected = true;
        ESP_LOGI(TAG, "host connected");
        /* Keep the ACL poll interval short so reports stay prompt. The event
         * carries no peer address, so apply it to the bonded peers (there is
         * exactly one in practice). This replaces the previous patch inside
         * esp_hid/src/bt_hidd.c and keeps the IDF checkout untouched. */
        const int bonded = esp_bt_gap_get_bond_device_num();
        esp_bd_addr_t peers[4];
        int count = bonded < (int)(sizeof(peers) / sizeof(peers[0]))
                        ? bonded
                        : (int)(sizeof(peers) / sizeof(peers[0]));
        if (count > 0 && esp_bt_gap_get_bond_device_list(&count, peers) == ESP_OK) {
            for (int i = 0; i < count; ++i) {
                (void)esp_bt_gap_set_qos(peers[i], 20);
            }
        }
        break;
    }
    case ESP_HIDD_DISCONNECT_EVENT:
        s_connected = false;
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
    return s_dev != NULL && esp_hidd_dev_connected(s_dev);
}

void app_btclassic_hid_send(uint8_t report_id, const uint8_t *data, size_t len)
{
    if (!app_btclassic_hid_connected()) {
        return;
    }
    (void)esp_hidd_dev_input_set(s_dev, 0, report_id, (uint8_t *)data, (uint16_t)len);
}
