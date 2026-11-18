// SPDX-License-Identifier: Apache-2.0

#include "app_bt_core.h"

#include <string.h>

#include "app_ble_hid.h"
#include "app_btclassic_hid.h"
#include "app_features.h"
#include "app_usb_hid.h"
#include "esp_bt.h"
#include "esp_bt_main.h"
#include "esp_check.h"
#include "esp_gap_bt_api.h"
#include "esp_gap_ble_api.h"
#include "esp_log.h"
#include "esp_mac.h"

static const char *TAG = "app_bt_core";

/* Two independent identities, on purpose: the LE gamepad advertises on a static
 * random address while the Classic function keeps the public BD_ADDR, so hosts
 * list them as separate devices instead of merging them. */
#define BT_LE_NAME "Mosaico Gamepad BLE"
#define BT_CLASSIC_NAME "Mosaico Gamepad Classic"

static bool s_ready;
static app_bt_mode_t s_mode = APP_BT_OFF;

static esp_err_t controller_start(app_bt_mode_t mode)
{
    esp_bt_controller_config_t cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
    ESP_RETURN_ON_ERROR(esp_bt_controller_init(&cfg), TAG, "controller init");

    esp_bt_mode_t hw_mode;
    switch (mode) {
    case APP_BT_BLE:
        hw_mode = ESP_BT_MODE_BLE;
        break;
    case APP_BT_CLASSIC:
        hw_mode = ESP_BT_MODE_CLASSIC_BT;
        break;
    case APP_BT_BOTH:
    default:
        hw_mode = ESP_BT_MODE_BTDM;
        break;
    }
    ESP_RETURN_ON_ERROR(esp_bt_controller_enable(hw_mode), TAG, "controller enable");
    ESP_RETURN_ON_ERROR(esp_bluedroid_init(), TAG, "bluedroid init");
    return esp_bluedroid_enable();
}

static void ble_identity(void)
{
    if (esp_ble_gap_set_device_name(BT_LE_NAME) != ESP_OK) {
        ESP_LOGW(TAG, "LE device name rejected");
    }
}

static void classic_identity(void)
{
    (void)esp_bt_gap_set_device_name(BT_CLASSIC_NAME);
    /* Audio/Video major with the hands-free minor: phones only connect the
     * hands-free profile when the class of device advertises audio. */
    const esp_bt_cod_t cod = {
        .minor = 0x02, /* hands-free device */
        .major = ESP_BT_COD_MAJOR_DEV_AV,
        .service = ESP_BT_COD_SRVC_AUDIO | ESP_BT_COD_SRVC_RENDERING |
                   ESP_BT_COD_SRVC_CAPTURING,
    };
    (void)esp_bt_gap_set_cod(cod, ESP_BT_SET_COD_ALL);
    /* Discoverability is useful even before the profiles register. */
    (void)esp_bt_gap_set_scan_mode(ESP_BT_CONNECTABLE, ESP_BT_GENERAL_DISCOVERABLE);
}

esp_err_t app_bt_core_start(void)
{
    s_mode = app_cfg_bt_mode();
    if (s_mode == APP_BT_OFF) {
        ESP_LOGI(TAG, "Bluetooth disabled by configuration");
        return ESP_OK;
    }

    ESP_RETURN_ON_ERROR(controller_start(s_mode), TAG, "stack");
    if (s_mode == APP_BT_BLE || s_mode == APP_BT_BOTH) {
        ble_identity();
    }
    if (s_mode == APP_BT_CLASSIC || s_mode == APP_BT_BOTH) {
        classic_identity();
    }
    s_ready = true;
    ESP_LOGI(TAG, "ready (mode %s)", app_cfg_bt_mode_name(s_mode));
    return ESP_OK;
}

bool app_bt_core_ready(void)
{
    return s_ready;
}

bool app_bt_core_wants_ble(void)
{
    return s_ready && (s_mode == APP_BT_BLE || s_mode == APP_BT_BOTH);
}

bool app_bt_core_wants_classic(void)
{
    return s_ready && (s_mode == APP_BT_CLASSIC || s_mode == APP_BT_BOTH);
}

const char *app_bt_core_le_name(void)
{
    return BT_LE_NAME;
}

const char *app_bt_core_classic_name(void)
{
    return BT_CLASSIC_NAME;
}

/* ---------------------------------------------------------------- HID part */

/* The report descriptors live in app_usb_core; the Bluetooth map is assembled
 * here from the subset selected by the persisted bt_hid mask. */
/* The two transports have independent class masks, so each gets its own map. */
static uint8_t s_report_map_ble[192];
static size_t s_report_map_ble_len;
static uint8_t s_report_map_bt[192];
static size_t s_report_map_bt_len;

static size_t build_report_map(uint32_t mask, uint8_t *out, size_t cap)
{
    static const uint32_t class_bit[3] = {
        APP_CLASS_GAMEPAD, APP_CLASS_MOUSE, APP_CLASS_KEYBOARD,
    };
    size_t used = 0;
    const size_t classes = app_usb_hid_class_count();
    for (size_t i = 0; i < classes && i < 3; ++i) {
        if ((mask & class_bit[i]) == 0) {
            continue;
        }
        size_t len = 0;
        const uint8_t *desc = app_usb_hid_class_desc(i, &len);
        if (desc == NULL || used + len > cap) {
            continue;
        }
        memcpy(&out[used], desc, len);
        used += len;
    }
    if ((mask & APP_CLASS_SENSOR) != 0) {
        size_t len = 0;
        const uint8_t *desc = app_usb_hid_sensor_desc(&len);
        if (desc != NULL && used + len <= cap) {
            memcpy(&out[used], desc, len);
            used += len;
        }
    }
    return used;
}

esp_err_t app_bt_core_hid_start(void)
{
    if (!s_ready) {
        return ESP_ERR_INVALID_STATE;
    }
    s_report_map_ble_len = build_report_map(app_cfg_ble_hid(), s_report_map_ble,
                                            sizeof(s_report_map_ble));
    s_report_map_bt_len = build_report_map(app_cfg_bt_hid(), s_report_map_bt,
                                           sizeof(s_report_map_bt));
    if (s_report_map_ble_len == 0 && s_report_map_bt_len == 0) {
        ESP_LOGI(TAG, "no Bluetooth HID class enabled");
        return ESP_OK;
    }
    /* Both transports share one controller and Bluedroid host, but their
     * bring-up must be fault isolated: a failure in one (for example the
     * classic HID SDP record) must not stop the other from running. */
    esp_err_t worst = ESP_OK;
    if (s_mode == APP_BT_BLE || s_mode == APP_BT_BOTH) {
#if APP_FEATURE_BLE_HID
        if (s_report_map_ble_len == 0) {
            ESP_LOGW(TAG, "BLE HID has no enabled classes");
        } else {
        const esp_err_t err = app_ble_hid_start(s_report_map_ble, s_report_map_ble_len);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "BLE HID unavailable: %s", esp_err_to_name(err));
            worst = err;
        }
        }
#endif
    }
    if (s_mode == APP_BT_CLASSIC || s_mode == APP_BT_BOTH) {
#if APP_FEATURE_BT_CLASSIC_HID
        if (s_report_map_bt_len == 0) {
            ESP_LOGW(TAG, "classic HID has no enabled classes");
        } else {
        const esp_err_t err = app_btclassic_hid_start(s_report_map_bt, s_report_map_bt_len);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Classic HID unavailable: %s", esp_err_to_name(err));
            worst = err;
        }
        }
#endif
    }
    /* Reports are routed whenever at least one transport came up. */
    app_usb_hid_attach_bt(app_bt_core_hid_connected, app_bt_core_hid_send);
    ESP_LOGI(TAG, "HID ready (ble %u B, classic %u B)%s",
             (unsigned)s_report_map_ble_len, (unsigned)s_report_map_bt_len,
             worst == ESP_OK ? "" : " (one transport failed)");
    return worst;
}

bool app_bt_core_hid_connected(void)
{
    bool connected = false;
#if APP_FEATURE_BLE_HID
    connected = connected || app_ble_hid_connected();
#endif
#if APP_FEATURE_BT_CLASSIC_HID
    connected = connected || app_btclassic_hid_connected();
#endif
    return connected;
}

/* Report id -> the class bit that owns it. A transport that did not enable the
 * class has no such report in its map, so sending would only log failures. */
static uint32_t class_for_report(uint8_t report_id)
{
    switch (report_id) {
    case 1: return APP_CLASS_GAMEPAD;
    case 2: return APP_CLASS_MOUSE;
    case 3: return APP_CLASS_KEYBOARD;
    case 4: return APP_CLASS_SENSOR;
    default: return 0;
    }
}

void app_bt_core_hid_send(uint8_t report_id, const uint8_t *data, size_t len)
{
    const uint32_t cls = class_for_report(report_id);
#if APP_FEATURE_BLE_HID
    if (cls == 0 || (app_cfg_ble_hid() & cls) != 0) {
        app_ble_hid_send(report_id, data, len);
    }
#endif
#if APP_FEATURE_BT_CLASSIC_HID
    if (cls == 0 || (app_cfg_bt_hid() & cls) != 0) {
        app_btclassic_hid_send(report_id, data, len);
    }
#endif
}
