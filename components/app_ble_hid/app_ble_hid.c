// SPDX-License-Identifier: Apache-2.0

#include "app_ble_hid.h"

#include <inttypes.h>

#include <string.h>

#include "app_bt_core.h"
#include "esp_check.h"
#include "esp_gap_ble_api.h"
#include "esp_hidd_gatts.h"
#include "esp_timer.h"
#include "esp_hidd.h"
#include "esp_log.h"

static const char *TAG = "app_ble_hid";

/* Advertising is not a HID concern: the payload, its timing and the policy live
 * in app_bt_core (see app_bt_core_ble_advertise). This component only owns the
 * HOGP service, the pairing parameters and report delivery, and reports when the
 * GATT database is up so core knows it may advertise. */
static esp_hidd_dev_t *s_dev;
static bool s_connected;
static unsigned s_send_ok;
static unsigned s_send_fail;
static bool s_service_ready;

static void hidd_event_handler(void *handler_args, esp_event_base_t base, int32_t id,
                               void *event_data)
{
    (void)handler_args;
    (void)base;
    (void)event_data;
    switch ((esp_hidd_event_t)id) {
    case ESP_HIDD_CONNECT_EVENT:
        s_connected = true;
        ESP_LOGI(TAG, "host connected");
        break;
    case ESP_HIDD_DISCONNECT_EVENT:
        s_connected = false;
        ESP_LOGI(TAG, "host disconnected");
        break;
    case ESP_HIDD_START_EVENT:
        /* Emitted when the GATT service starts: the HID database exists now and
         * core can advertise without a host finding an empty service. */
        s_service_ready = true;
        ESP_LOGI(TAG, "GATT service ready");
        break;
    default:
        ESP_LOGI(TAG, "hidd event %d", (int)id);
        break;
    }
}

/* Report delivery state once a connection exists; the counters make a silent
 * notification path visible in the log. */
static void ble_status_task(void *arg)
{
    (void)arg;
    unsigned tick = 0;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        const bool connected = app_ble_hid_connected();
        s_connected = connected;
        if (++tick % 10 == 0) {
            ESP_LOGI(TAG, "state: connected=%d ok=%u fail=%u", (int)connected,
                     s_send_ok, s_send_fail);
        }
    }
}

esp_err_t app_ble_hid_start(const uint8_t *report_map, size_t report_map_len)
{
    if (report_map == NULL || report_map_len == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    s_service_ready = false;

    /* Secure Connections + bonding, Just Works: no MITM bit, so neither side
     * asks for a passkey. The official esp_hid_device example uses
     * SC_MITM_BOND with IO_CAP_IO, which requires numeric comparison support on
     * the device; we have no display, so Just Works is the correct profile. */
    esp_ble_auth_req_t auth_req = ESP_LE_AUTH_REQ_SC_BOND;
    esp_ble_io_cap_t iocap = ESP_IO_CAP_NONE;
    uint8_t key_size = 16;
    uint8_t init_key = ESP_BLE_ENC_KEY_MASK | ESP_BLE_ID_KEY_MASK;
    uint8_t rsp_key = ESP_BLE_ENC_KEY_MASK | ESP_BLE_ID_KEY_MASK;
    (void)esp_ble_gap_set_security_param(ESP_BLE_SM_AUTHEN_REQ_MODE, &auth_req, 1);
    (void)esp_ble_gap_set_security_param(ESP_BLE_SM_IOCAP_MODE, &iocap, 1);
    (void)esp_ble_gap_set_security_param(ESP_BLE_SM_MAX_KEY_SIZE, &key_size, 1);
    (void)esp_ble_gap_set_security_param(ESP_BLE_SM_SET_INIT_KEY, &init_key, 1);
    (void)esp_ble_gap_set_security_param(ESP_BLE_SM_SET_RSP_KEY, &rsp_key, 1);

    esp_hid_raw_report_map_t maps[1] = {
        {.data = (uint8_t *)report_map, .len = (uint16_t)report_map_len},
    };
    esp_hid_device_config_t hid_config = {
        .vendor_id = 0x303A,
        .product_id = 0x4002,
        .version = 0x0100,
        .device_name = app_bt_core_le_name(),
        .manufacturer_name = "Espressif",
        .serial_number = "000000000000",
        .report_maps = maps,
        .report_maps_len = 1,
    };
    /* Bluedroid keeps ONE GATTS callback; esp_hid exposes the dispatcher that
     * feeds its own handlers. Without this registration no GATTS event ever
     * arrives, the attribute tables are never created, the HID service is
     * missing and hosts disconnect right after service discovery. */
    ESP_RETURN_ON_ERROR(esp_ble_gatts_register_callback(esp_hidd_gatts_event_handler),
                        TAG, "gatts cb");
    ESP_RETURN_ON_ERROR(esp_hidd_dev_init(&hid_config, ESP_HID_TRANSPORT_BLE,
                                          hidd_event_handler, &s_dev),
                        TAG, "hidd init");
    if (xTaskCreate(ble_status_task, "ble_status", 3072, NULL, 2, NULL) != pdPASS) {
        ESP_LOGW(TAG, "status task unavailable");
    }
    ESP_LOGI(TAG, "ready (%u-byte report map)", (unsigned)report_map_len);
    return ESP_OK;
}

bool app_ble_hid_service_ready(void)
{
    return s_service_ready;
}

void app_ble_hid_gap_event(esp_gap_ble_cb_event_t event, esp_ble_gap_cb_param_t *param)
{
    /* Core owns the GAP callback because advertising is transport-level; the
     * pairing and connection-parameter events below are still ours. */
    switch (event) {
    case ESP_GAP_BLE_SEC_REQ_EVT:
        (void)esp_ble_gap_security_rsp(param->ble_security.ble_req.bd_addr, true);
        break;
    case ESP_GAP_BLE_NC_REQ_EVT:
        /* Numeric comparison: only reachable if MITM were enabled. */
        ESP_LOGI(TAG, "numeric comparison %06" PRIu32, param->ble_security.key_notif.passkey);
        (void)esp_ble_confirm_reply(param->ble_security.key_notif.bd_addr, true);
        break;
    case ESP_GAP_BLE_PASSKEY_REQ_EVT:
        (void)esp_ble_passkey_reply(param->ble_security.ble_req.bd_addr, false, 0);
        break;
    case ESP_GAP_BLE_AUTH_CMPL_EVT:
        ESP_LOGI(TAG, "auth %s", param->ble_security.auth_cmpl.success ? "ok" : "failed");
        if (!param->ble_security.auth_cmpl.success) {
            ESP_LOGE(TAG, "auth fail reason 0x%x; removing stale bond",
                     param->ble_security.auth_cmpl.fail_reason);
            /* Keys that no longer match (security params changed, phone kept an
             * old record) otherwise make every retry fail with "PIN wrong". */
            (void)esp_ble_remove_bond_device(param->ble_security.auth_cmpl.bd_addr);
        }
        break;
    case ESP_GAP_BLE_KEY_EVT:
        ESP_LOGI(TAG, "key type %d", param->ble_security.ble_key.key_type);
        break;
    case ESP_GAP_BLE_UPDATE_CONN_PARAMS_EVT:
        ESP_LOGI(TAG, "conn params: status=%d interval=%d latency=%d timeout=%d",
                 (int)param->update_conn_params.status,
                 (int)param->update_conn_params.conn_int,
                 (int)param->update_conn_params.latency,
                 (int)param->update_conn_params.timeout);
        break;
    case ESP_GAP_BLE_PASSKEY_NOTIF_EVT:
        ESP_LOGI(TAG, "passkey %06" PRIu32, param->ble_security.key_notif.passkey);
        break;
    default:
        ESP_LOGI(TAG, "gap event %d", (int)event);
        break;
    }
}

unsigned app_ble_hid_clear_bonds(void)
{
    esp_ble_bond_dev_t peers[8];
    int count = 8;
    if (esp_ble_get_bond_device_list(&count, peers) != ESP_OK) {
        return 0;
    }
    for (int i = 0; i < count; ++i) {
        (void)esp_ble_remove_bond_device(peers[i].bd_addr);
    }
    ESP_LOGI(TAG, "cleared %d bond(s)", count);
    return (unsigned)count;
}

bool app_ble_hid_connected(void)
{
    /* Ask the stack instead of trusting our own event flag: the HIDD connect
     * event is not always delivered, but the internal state is accurate. */
    return s_dev != NULL && esp_hidd_dev_connected(s_dev);
}

void app_ble_hid_send(uint8_t report_id, const uint8_t *data, size_t len)
{
    if (!app_ble_hid_connected() || data == NULL) {
        return;
    }
    /* A 17-18 byte sensor report fits the default 20-byte notification payload,
     * but the phone negotiates a 30 ms connection interval, so at most one
     * motion notification per interval is worth sending. */
    if (report_id == 4) {
        static int64_t last_us;
        const int64_t now = esp_timer_get_time();
        if (last_us != 0 && now - last_us < 30000) {
            return;
        }
        last_us = now;
    }
    /* esp_hid sizes each Report characteristic from the report map
     * (input_len / 8, without the report ID) and announces the ID through the
     * Report Reference descriptor, so the payload is sent verbatim. Prepending
     * the ID shifted every field by one byte on the host. */
    const esp_err_t err = esp_hidd_dev_input_set(s_dev, 0, report_id,
                                                 (uint8_t *)data, (uint16_t)len);
    if (err == ESP_OK) {
        ++s_send_ok;
    } else {
        if ((s_send_fail++ % 32) == 0) {
            ESP_LOGW(TAG, "input_set failed (%s), ok=%u fail=%u", esp_err_to_name(err),
                     s_send_ok, s_send_fail);
        }
    }
}
