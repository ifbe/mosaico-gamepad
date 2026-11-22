// SPDX-License-Identifier: Apache-2.0

#include "app_cfg.h"

#include "app_ble_hid.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_iris.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs.h"
#include "nvs_flash.h"

static const char *TAG = "app_cfg";
#define NVS_NAMESPACE "gamepad"

/* The key was renamed twice so that already-flashed devices pick up a new
 * default instead of the value stored under the old key: "classes" -> "usb_hid"
 * (endpoint-aware default) -> "usb_hid2" (motion interface in the default).
 * Bluetooth, audio and identity keys are unaffected. */
#define NVS_KEY_CLASSES "usb_hid2"

/* Default when NVS has no record: the generic classes, the motion interface and
 * the sound card. ESP-Iris runs console-only (data link disabled), so the free
 * IN endpoints are 0x83..0x87: keyboard/mouse/gamepad take 0x83..0x85, the
 * motion interface takes 0x86 and the UAC2 microphone 0x87. */
static uint32_t s_classes = APP_CLASS_USB_DEFAULT;
static bool s_usb_audio = true;
static uint32_t s_bt_hid = APP_CLASS_ALL;
static uint32_t s_ble_hid = APP_CLASS_ALL;
static app_bt_mode_t s_bt_mode = APP_BT_BOTH;
static uint32_t s_bt_audio = APP_BT_AUDIO_ALL;
static uint16_t s_usb_vid; /* 0 = derive from the HID mode */
static uint16_t s_usb_pid;
/* Registered by app_ui_core at startup: the UI owns the pages, this component
 * only relays the RPC. Never NULL once the UI started. */
static const app_cfg_layout_rpc_t *s_layout_rpc;

static const struct {
    const char *name;
    uint32_t mask;
} s_class_names[] = {
    {"gamepad", APP_CLASS_GAMEPAD},
    {"mouse", APP_CLASS_MOUSE},
    {"keyboard", APP_CLASS_KEYBOARD},
};

uint32_t app_cfg_classes(void)
{
    return s_classes;
}

bool app_cfg_usb_audio(void)
{
    return s_usb_audio;
}

app_usb_hid_mode_t app_cfg_usb_screen_mode(void)
{
    const bool ds4 = (s_classes & APP_CLASS_DS4) != 0;
    const bool ds5 = (s_classes & APP_CLASS_DS5) != 0;
    if (ds4 && ds5) {
        return APP_USB_HID_GENERIC; /* ambiguous: nothing Sony is registered */
    }
    if (ds4) {
        return APP_USB_HID_DS4;
    }
    if (ds5) {
        return APP_USB_HID_DS5;
    }
    return APP_USB_HID_GENERIC;
}

const char *app_cfg_usb_hid_mode_name(app_usb_hid_mode_t mode)
{
    switch (mode) {
    case APP_USB_HID_DS4:
        return "ds4";
    case APP_USB_HID_DS5:
        return "ds5";
    case APP_USB_HID_GENERIC:
    default:
        return "generic";
    }
}

void app_cfg_usb_ids(uint16_t *vid, uint16_t *pid)
{
    uint16_t v = s_usb_vid;
    uint16_t p = s_usb_pid;
    if (v == 0 || p == 0) {
        switch (app_cfg_usb_screen_mode()) {
        case APP_USB_HID_DS4:
            v = 0x054C;
            p = 0x09CC;
            break;
        case APP_USB_HID_DS5:
            v = 0x054C;
            p = 0x0CE6;
            break;
        case APP_USB_HID_GENERIC:
        default:
            v = 0x303A;
            p = 0x4002;
            break;
        }
    }
    if (vid != NULL) {
        *vid = v;
    }
    if (pid != NULL) {
        *pid = p;
    }
}

bool app_cfg_has(uint32_t class_mask)
{
    return (s_classes & class_mask) != 0;
}

uint32_t app_cfg_bt_hid(void)
{
    return s_bt_hid;
}

app_bt_mode_t app_cfg_bt_mode(void)
{
    return s_bt_mode;
}

uint32_t app_cfg_bt_audio(void)
{
    return s_bt_audio;
}

const char *app_cfg_bt_mode_name(app_bt_mode_t mode)
{
    switch (mode) {
    case APP_BT_OFF:
        return "off";
    case APP_BT_BLE:
        return "ble";
    case APP_BT_CLASSIC:
        return "classic";
    case APP_BT_BOTH:
    default:
        return "both";
    }
}

static void save_u32(const char *key, uint32_t value)
{
    nvs_handle_t handle;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle) != ESP_OK) {
        return;
    }
    (void)nvs_set_u32(handle, key, value);
    (void)nvs_commit(handle);
    nvs_close(handle);
}

/* Sony emulation replaces the generic gamepad, and the motion interface needs
 * the endpoint Sony would take, so the two groups are mutually exclusive. */
static uint32_t normalize_mask(uint32_t mask)
{
    mask &= APP_CLASS_USB_ALL;
    const uint32_t emu = APP_CLASS_DS4 | APP_CLASS_DS5 | APP_CLASS_XBOX;
    if ((mask & emu) != 0) {
        /* Exactly one emulation may be active, and it replaces the generic
         * gamepad and the motion interface (endpoint budget). */
        uint32_t one = mask & emu;
        one &= (~one + 1u); /* keep the lowest set bit */
        mask = (mask & ~emu) | one;
        mask &= ~(APP_CLASS_GAMEPAD | APP_CLASS_SENSOR);
    } else {
        mask &= ~emu;
    }
    return mask;
}

esp_err_t app_cfg_set_usb_hid(uint32_t mask)
{
    s_classes = normalize_mask(mask);
    save_u32(NVS_KEY_CLASSES, s_classes);
    return ESP_OK;
}

uint32_t app_cfg_ble_hid(void)
{
    return s_ble_hid;
}

esp_err_t app_cfg_set_ble_hid(uint32_t mask)
{
    s_ble_hid = mask & APP_CLASS_BT_ALL;
    save_u32("ble_hid", s_ble_hid);
    return ESP_OK;
}

esp_err_t app_cfg_reset_all(void)
{
    (void)app_cfg_set_usb_hid(APP_CLASS_USB_DEFAULT);
    (void)app_cfg_set_bt_hid(APP_CLASS_ALL);
    (void)app_cfg_set_ble_hid(APP_CLASS_ALL);
    (void)app_cfg_set_usb_audio(true);
    (void)app_cfg_set_bt_audio(APP_BT_AUDIO_ALL);
    save_u32("bt_mode", (uint32_t)APP_BT_BOTH);
    s_bt_mode = APP_BT_BOTH;
    return ESP_OK;
}

esp_err_t app_cfg_set_bt_hid(uint32_t mask)
{
    s_bt_hid = mask & APP_CLASS_BT_ALL;
    save_u32("bt_hid", s_bt_hid);
    return ESP_OK;
}

esp_err_t app_cfg_set_usb_audio(bool enabled)
{
    s_usb_audio = enabled;
    save_u32("usb_audio", enabled ? 1 : 0);
    return ESP_OK;
}

esp_err_t app_cfg_set_bt_audio(uint32_t mask)
{
    s_bt_audio = mask & APP_BT_AUDIO_ALL;
    save_u32("bt_audio", s_bt_audio);
    return ESP_OK;
}


static void save_classes(void)
{
    nvs_handle_t handle;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle) != ESP_OK) {
        return;
    }
    (void)nvs_set_u32(handle, NVS_KEY_CLASSES, s_classes);
    (void)nvs_commit(handle);
    nvs_close(handle);
}

static const char *payload_value(const char *payload, const char *key, char *out, size_t cap)
{
    char pattern[24];
    snprintf(pattern, sizeof(pattern), "\"%s\":\"", key);
    const char *p = strstr(payload, pattern);
    if (p == NULL) {
        return NULL;
    }
    p += strlen(pattern);
    size_t n = 0;
    while (p[n] != '\0' && p[n] != '"' && n + 1 < cap) {
        out[n] = p[n];
        ++n;
    }
    out[n] = '\0';
    return out;
}

static void reboot_task(void *arg)
{
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(200));
    esp_restart();
}

static uint32_t class_from_name(const char *name)
{
    for (size_t i = 0; i < sizeof(s_class_names) / sizeof(s_class_names[0]); ++i) {
        if (strcmp(name, s_class_names[i].name) == 0) {
            return s_class_names[i].mask;
        }
    }
    return 0;
}

static esp_err_t rpc_handler(const esp_iris_rpc_request_t *request, uint8_t *response,
                             size_t response_capacity, size_t *response_size,
                             void *user_ctx)
{
    (void)user_ctx;
    char payload[64] = {0};
    const size_t n = request->payload_size < sizeof(payload) - 1
                         ? request->payload_size
                         : sizeof(payload) - 1;
    memcpy(payload, request->payload, n);
    payload[n] = '\0';

    char cmd[24] = {0};
    char arg[24] = {0};
    payload_value(payload, "cmd", cmd, sizeof(cmd));
    payload_value(payload, "class", arg, sizeof(arg));
    if (arg[0] == '\0') {
        payload_value(payload, "layout", arg, sizeof(arg));
    }

    int written = 0;
    switch (request->method_id) {
    case 1: /* list */
        written = snprintf((char *)response, response_capacity,
                           "{\"usb_hid\":%u,\"usb_ds\":\"%s\",\"usb_vid\":%u,"
                           "\"usb_pid\":%u,\"usb_audio\":%d,\"bt_hid\":%u,"
                           "\"bt_mode\":\"%s\",\"bt_audio\":%u,\"layout\":\"%s\"}",
                           (unsigned)s_classes,
                           app_cfg_usb_hid_mode_name(app_cfg_usb_screen_mode()),
                           (unsigned)s_usb_vid, (unsigned)s_usb_pid, (int)s_usb_audio,
                           (unsigned)s_bt_hid, app_cfg_bt_mode_name(s_bt_mode),
                           (unsigned)s_bt_audio,
                           s_layout_rpc != NULL && s_layout_rpc->name != NULL
                               ? s_layout_rpc->name()
                               : "none");
        break;
    case 2: /* enable */
    case 3: { /* disable */
        const uint32_t mask = class_from_name(arg);
        if (mask == 0) {
            written = snprintf((char *)response, response_capacity,
                               "{\"error\":\"unknown class\"}");
            break;
        }
        if (request->method_id == 2) {
            s_classes = normalize_mask(s_classes | mask);
        } else {
            s_classes = normalize_mask(s_classes & ~mask);
        }
        save_classes();
        written = snprintf((char *)response, response_capacity,
                           "{\"classes\":%u,\"reboot_required\":true}", (unsigned)s_classes);
        break;
    }
    case 4: { /* layout */
        const char *applied = (s_layout_rpc != NULL && s_layout_rpc->apply != NULL)
                                  ? s_layout_rpc->apply(arg)
                                  : NULL;
        if (applied == NULL) {
            written = snprintf((char *)response, response_capacity,
                               "{\"error\":\"unknown layout\"}");
            break;
        }
        written = snprintf((char *)response, response_capacity,
                           "{\"layout\":\"%s\"}", applied);
        break;
    }
    case 5: /* reboot */
        written = snprintf((char *)response, response_capacity, "{\"rebooting\":true}");
        if (xTaskCreate(reboot_task, "cfg_reboot", 2048, NULL, 5, NULL) != pdPASS) {
            esp_restart();
        }
        break;
    case 6: { /* set: {"cmd":"set","key":"bt_mode","value":"both"} */
        char key[16] = {0};
        char value[24] = {0};
        payload_value(payload, "key", key, sizeof(key));
        payload_value(payload, "value", value, sizeof(value));
        bool known = true;
        if (strcmp(key, "usb_hid") == 0 || strcmp(key, "bt_hid") == 0) {
            const bool bt = key[0] == 'b';
            const uint32_t limit = bt ? APP_CLASS_BT_ALL : APP_CLASS_USB_ALL;
            uint32_t mask = (uint32_t)strtoul(value, NULL, 0) & limit;
            if (strcmp(value, "all") == 0) {
                mask = limit;
            } else if (strcmp(value, "none") == 0) {
                mask = 0;
            }
            if (key[0] == 'u') {
                s_classes = normalize_mask(mask);
                save_u32(NVS_KEY_CLASSES, s_classes);
            } else {
                s_bt_hid = mask;
                save_u32("bt_hid", mask);
            }
        } else if (strcmp(key, "ble_hid") == 0) {
            /* The BLE mask may carry the motion bit as well, so clamp to the
             * same set app_cfg_set_ble_hid() accepts; clamping to APP_CLASS_ALL
             * silently dropped sensor here while the settings page accepted it. */
            uint32_t mask = (uint32_t)strtoul(value, NULL, 0) & APP_CLASS_BT_ALL;
            if (strcmp(value, "all") == 0) {
                mask = APP_CLASS_BT_ALL;
            } else if (strcmp(value, "none") == 0) {
                mask = 0;
            }
            (void)app_cfg_set_ble_hid(mask);
            known = true;
        } else if (strcmp(key, "usb_vid") == 0 || strcmp(key, "usb_pid") == 0) {
            const uint32_t id = (uint32_t)strtoul(value, NULL, 0) & 0xFFFFu;
            if (key[4] == 'v') {
                s_usb_vid = (uint16_t)id;
                save_u32("usb_vid", id);
            } else {
                s_usb_pid = (uint16_t)id;
                save_u32("usb_pid", id);
            }
        } else if (strcmp(key, "usb_audio") == 0) {
            const bool on = strcmp(value, "1") == 0 || strcmp(value, "on") == 0 ||
                            strcmp(value, "true") == 0;
            s_usb_audio = on;
            save_u32("usb_audio", on ? 1 : 0);
        } else if (strcmp(key, "bt_audio") == 0) {
            uint32_t mask = (uint32_t)strtoul(value, NULL, 0) & APP_BT_AUDIO_ALL;
            if (strcmp(value, "all") == 0) {
                mask = APP_BT_AUDIO_ALL;
            } else if (strcmp(value, "none") == 0) {
                mask = 0;
            } else if (strcmp(value, "speaker") == 0) {
                mask = APP_BT_AUDIO_SPEAKER;
            } else if (strcmp(value, "mic") == 0) {
                mask = APP_BT_AUDIO_MIC;
            }
            s_bt_audio = mask;
            save_u32("bt_audio", mask);
        } else if (strcmp(key, "bt_mode") == 0) {
            if (strcmp(value, "off") == 0) {
                s_bt_mode = APP_BT_OFF;
            } else if (strcmp(value, "ble") == 0) {
                s_bt_mode = APP_BT_BLE;
            } else if (strcmp(value, "classic") == 0) {
                s_bt_mode = APP_BT_CLASSIC;
            } else if (strcmp(value, "both") == 0) {
                s_bt_mode = APP_BT_BOTH;
            } else {
                known = false;
            }
            if (known) {
                save_u32("bt_mode", (uint32_t)s_bt_mode);
            }
        } else {
            known = false;
        }
        if (!known) {
            written = snprintf((char *)response, response_capacity,
                               "{\"error\":\"unknown key or value\"}");
            break;
        }
        written = snprintf((char *)response, response_capacity,
                           "{\"usb_hid\":%u,\"bt_hid\":%u,\"bt_mode\":\"%s\","
                           "\"bt_audio\":%u,\"reboot_required\":true}",
                           (unsigned)s_classes, (unsigned)s_bt_hid,
                           app_cfg_bt_mode_name(s_bt_mode), (unsigned)s_bt_audio);
        break;
    }
    case 9: { /* reset every setting: {"cmd":"reset"} */
        (void)app_cfg_reset_all();
        written = snprintf((char *)response, response_capacity,
                           "{\"reset\":true,\"reboot_required\":true}");
        break;
    }
    case 8: { /* restart BLE advertising: {"cmd":"ble_adv"} */
        app_ble_hid_restart_advertising();
        written = snprintf((char *)response, response_capacity, "{\"advertising\":true}");
        break;
    }
    case 7: { /* clear BLE bonds: {"cmd":"clear_bonds"} */
        const unsigned removed = app_ble_hid_clear_bonds();
        written = snprintf((char *)response, response_capacity,
                           "{\"cleared\":%u}", removed);
        break;
    }
    default:
        return ESP_ERR_NOT_SUPPORTED;
    }

    if (written < 0) {
        return ESP_ERR_INVALID_SIZE;
    }
    *response_size = (size_t)written < response_capacity ? (size_t)written : response_capacity;
    ESP_LOGI(TAG, "rpc %u -> %s", (unsigned)request->method_id, (char *)response);
    return ESP_OK;
}

esp_err_t app_cfg_init(void)
{
    nvs_handle_t handle;
    uint32_t value = 0;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle) == ESP_OK) {
        if (nvs_get_u32(handle, NVS_KEY_CLASSES, &value) == ESP_OK) {
            s_classes = normalize_mask(value);
        }
        if (nvs_get_u32(handle, "usb_audio", &value) == ESP_OK) {
            s_usb_audio = value != 0;
        }
        if (nvs_get_u32(handle, "bt_hid", &value) == ESP_OK) {
            s_bt_hid = value & APP_CLASS_BT_ALL;
        }
        if (nvs_get_u32(handle, "ble_hid", &value) == ESP_OK) {
            s_ble_hid = value & APP_CLASS_BT_ALL;
        }
        if (nvs_get_u32(handle, "bt_mode", &value) == ESP_OK && value <= APP_BT_BOTH) {
            s_bt_mode = (app_bt_mode_t)value;
        }
        if (nvs_get_u32(handle, "usb_vid", &value) == ESP_OK) {
            s_usb_vid = (uint16_t)value;
        }
        if (nvs_get_u32(handle, "usb_pid", &value) == ESP_OK) {
            s_usb_pid = (uint16_t)value;
        }
        if (nvs_get_u32(handle, "bt_audio", &value) == ESP_OK) {
            s_bt_audio = value & APP_BT_AUDIO_ALL;
        }
        nvs_close(handle);
    }
    uint16_t vid = 0, pid = 0;
    app_cfg_usb_ids(&vid, &pid);
    ESP_LOGI(TAG, "usb_hid=0x%X mode=%s id=%04X:%04X usb_audio=%d bt_hid=0x%X bt_mode=%s bt_audio=0x%X",
             (unsigned)s_classes, app_cfg_usb_hid_mode_name(app_cfg_usb_screen_mode()),
             (unsigned)vid, (unsigned)pid, (int)s_usb_audio, (unsigned)s_bt_hid,
             app_cfg_bt_mode_name(s_bt_mode), (unsigned)s_bt_audio);
    return ESP_OK;
}

static void rpc_task(void *arg)
{
    (void)arg;
    /* Iris must be fully started before its RPC registry accepts handlers; a
     * short delay also keeps a registry failure from blocking boot. */
    vTaskDelay(pdMS_TO_TICKS(1500));
    for (uint16_t method = 1; method <= 9; ++method) {
        const esp_err_t err = esp_iris_rpc_register(APP_CFG_RPC_SERVICE, method,
                                                    rpc_handler, NULL);
        ESP_LOGI(TAG, "rpc %u: %s", (unsigned)method, esp_err_to_name(err));
    }
    vTaskDelete(NULL);
}

void app_cfg_set_layout_rpc(const app_cfg_layout_rpc_t *rpc)
{
    s_layout_rpc = rpc;
}

esp_err_t app_cfg_register_rpc(void)
{
    if (xTaskCreate(rpc_task, "cfg_rpc", 3072, NULL, 4, NULL) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}
