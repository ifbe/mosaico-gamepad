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
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"

static const char *TAG = "app_bt_core";

/* Two independent identities, on purpose: the LE gamepad advertises on a static
 * random address while the Classic function keeps the public BD_ADDR, so hosts
 * list them as separate devices instead of merging them. */
#define BT_LE_NAME "Mosaico Gamepad BLE"
#define BT_CLASSIC_NAME "Mosaico Gamepad BTClassic"

static bool s_ready;
static app_bt_mode_t s_mode = APP_BT_OFF;

/* ---------------------------------------------------------- sniff kicking --- */

/* Bluedroid's own power management pushes the ACL link into sniff, and the
 * controller then delays or refuses whatever shares that link: HID reports wait
 * a sniff-to-active round trip (~40-50 ms), and the eSCO setup for a call is
 * rejected outright ("OLM_CONN: pend list full ... proc_type:0x800d").
 * bta_dm_pm_active() is the exported BTA entry that forces the link back to
 * active. It has no public header, so declare it here; see docs/gotcha.md. */
extern void bta_dm_pm_active(uint8_t peer_addr[6]);

/* Coalescing window: several callers (8 ms HID task, 100 Hz mic task, the
 * periodic timer below) collapse into at most one HCI command per second. */
#define APP_BT_KICK_MIN_INTERVAL_US (1000 * 1000)

static portMUX_TYPE s_kick_mux = portMUX_INITIALIZER_UNLOCKED;
static int64_t s_last_kick_us;
static unsigned s_kick_count;
static unsigned s_kick_throttled;
static uint8_t s_classic_peer[6];
static bool s_classic_peer_known;
static esp_timer_handle_t s_kick_timer;

void app_bt_core_note_classic_peer(const uint8_t bda[6])
{
    if (bda == NULL) {
        return;
    }
    portENTER_CRITICAL(&s_kick_mux);
    memcpy(s_classic_peer, bda, sizeof(s_classic_peer));
    s_classic_peer_known = true;
    portEXIT_CRITICAL(&s_kick_mux);
}

bool app_bt_core_classic_peer(uint8_t out[6])
{
    if (out == NULL) {
        return false;
    }
    portENTER_CRITICAL(&s_kick_mux);
    const bool known = s_classic_peer_known;
    if (known) {
        memcpy(out, s_classic_peer, sizeof(s_classic_peer));
    }
    portEXIT_CRITICAL(&s_kick_mux);
    return known;
}

/* Kick one peer address. Returns how many peers were kicked. */
static unsigned kick_peer(uint8_t bda[6])
{
    bta_dm_pm_active(bda);
    return 1;
}

bool app_bt_core_kick_acl_sniff(void)
{
    if (!app_bt_core_wants_classic()) {
        return false;
    }

    /* Claim the window first, in one critical section: two tasks arriving
     * together must not both kick, and the timestamp has to be set exactly once
     * per window. */
    const int64_t now = esp_timer_get_time();
    bool claimed = false;
    uint8_t peer[6];
    bool peer_known;
    portENTER_CRITICAL(&s_kick_mux);
    if ((now - s_last_kick_us) >= APP_BT_KICK_MIN_INTERVAL_US) {
        s_last_kick_us = now;
        claimed = true;
    } else {
        ++s_kick_throttled;
    }
    peer_known = s_classic_peer_known;
    memcpy(peer, s_classic_peer, sizeof(peer));
    portEXIT_CRITICAL(&s_kick_mux);
    if (!claimed) {
        return false;
    }

    unsigned kicked = 0;
    if (peer_known) {
        kicked = kick_peer(peer);
    } else {
        /* The classic HID connect event carries no peer address, so fall back to
         * the bond database until a profile reports one. */
        const int bonded = esp_bt_gap_get_bond_device_num();
        esp_bd_addr_t peers[4];
        int count = bonded < (int)(sizeof(peers) / sizeof(peers[0]))
                        ? bonded
                        : (int)(sizeof(peers) / sizeof(peers[0]));
        if (count > 0 && esp_bt_gap_get_bond_device_list(&count, peers) == ESP_OK) {
            for (int i = 0; i < count; ++i) {
                kicked += kick_peer(peers[i]);
            }
        }
    }
    if (kicked == 0) {
        return false;
    }

    portENTER_CRITICAL(&s_kick_mux);
    ++s_kick_count;
    portEXIT_CRITICAL(&s_kick_mux);
    return true;
}

void app_bt_core_kick_stats(unsigned *kicks, unsigned *throttled, unsigned *age_ms)
{
    const int64_t now = esp_timer_get_time();
    portENTER_CRITICAL(&s_kick_mux);
    if (kicks != NULL) {
        *kicks = s_kick_count;
    }
    if (throttled != NULL) {
        *throttled = s_kick_throttled;
    }
    if (age_ms != NULL) {
        *age_ms = s_kick_count == 0 ? 0 : (unsigned)((now - s_last_kick_us) / 1000);
    }
    portEXIT_CRITICAL(&s_kick_mux);
}

/* The periodic kick runs while the Classic link can be carrying something the
 * user notices: HID input or call audio.
 *
 * TODO: narrow this to the real condition - "Classic ACL connected AND (a HID
 * class is enabled/connected OR a call is actually up)". The components that
 * know those states live above app_bt_core, so for now the timer runs whenever
 * Classic is enabled and simply finds no peer to kick until one bonds. */
static bool acl_kick_needed(void)
{
    return app_bt_core_wants_classic();
}

static void kick_timer_cb(void *arg)
{
    (void)arg;
    if (acl_kick_needed()) {
        (void)app_bt_core_kick_acl_sniff();
    }
}

static void kick_timer_apply(void)
{
    const bool needed = acl_kick_needed();
    if (needed && s_kick_timer == NULL) {
        const esp_timer_create_args_t args = {
            .callback = kick_timer_cb,
            .name = "bt_acl_kick",
        };
        if (esp_timer_create(&args, &s_kick_timer) != ESP_OK) {
            ESP_LOGW(TAG, "ACL kick timer unavailable");
            return;
        }
        if (esp_timer_start_periodic(s_kick_timer, APP_BT_KICK_MIN_INTERVAL_US) != ESP_OK) {
            (void)esp_timer_delete(s_kick_timer);
            s_kick_timer = NULL;
        }
        return;
    }
    if (!needed && s_kick_timer != NULL) {
        (void)esp_timer_stop(s_kick_timer);
        (void)esp_timer_delete(s_kick_timer);
        s_kick_timer = NULL;
    }
}

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

/* Static random address: the LE identity gets its own BD_ADDR, separate from the
 * Classic public address. Sharing one address makes hosts merge both records and
 * the visible name alternates between the two. Derived from the base MAC, so it
 * is stable across boots. */
static esp_err_t ble_set_random_address(void)
{
    uint8_t random_addr[6];
    ESP_RETURN_ON_ERROR(esp_read_mac(random_addr, ESP_MAC_BT), TAG, "bt mac");
    random_addr[0] ^= 0xA5;
    /* The two most significant bits must be 11 for a static random address.
     * Bluedroid validates byte 0 while the LE spec talks about the most
     * significant byte, so set the type bits on both ends. */
    random_addr[0] = (uint8_t)((random_addr[0] & 0x3F) | 0xC0);
    random_addr[5] = (uint8_t)((random_addr[5] & 0x3F) | 0xC0);
    ESP_LOGI(TAG, "LE address %02X:%02X:%02X:%02X:%02X:%02X", random_addr[5],
             random_addr[4], random_addr[3], random_addr[2], random_addr[1],
             random_addr[0]);
    return esp_ble_gap_set_rand_addr(random_addr);
}

static void ble_identity(void)
{
    (void)ble_set_random_address();
    if (esp_ble_gap_set_device_name(BT_LE_NAME) != ESP_OK) {
        ESP_LOGW(TAG, "LE device name rejected");
    }
}

/* Class of device follows what is actually enabled, because hosts use it to
 * decide which profiles to probe:
 *
 * - audio on  -> Peripheral/Combo plus the audio service bits. macOS then binds
 *   both the HID record and A2DP/HFP; without the audio bits it never even looks
 *   at HID, and with a pure AV class it ignores HID entirely.
 * - audio off -> Peripheral with the minor class of the enabled HID set
 *   (keyboard / pointing / combo / gamepad). That is what makes an iPhone treat
 *   the device as a plain Bluetooth keyboard.
 * - nothing HID, nothing audio -> fall back to the AV/hands-free identity. */
static esp_bt_cod_t classic_cod(void)
{
    const uint32_t hid = app_cfg_bt_hid();
    const bool audio = app_cfg_bt_audio() != 0;
    const bool kbd = (hid & APP_CLASS_KEYBOARD) != 0;
    const bool mouse = (hid & APP_CLASS_MOUSE) != 0;
    const bool pad = (hid & APP_CLASS_GAMEPAD) != 0;

    esp_bt_cod_t cod = {0};
    if (audio) {
        cod.service = ESP_BT_COD_SRVC_AUDIO | ESP_BT_COD_SRVC_RENDERING |
                      ESP_BT_COD_SRVC_CAPTURING;
    }

    uint32_t minor = 0;
    if (kbd && mouse) {
        minor = ESP_BT_COD_MINOR_PERIPHERAL_COMBO;
    } else if (kbd) {
        minor = ESP_BT_COD_MINOR_PERIPHERAL_KEYBOARD;
    } else if (mouse) {
        minor = ESP_BT_COD_MINOR_PERIPHERAL_POINTING;
    }
    if (pad) {
        minor |= ESP_BT_COD_MINOR_PERIPHERAL_GAMEPAD;
    }

    if (minor != 0) {
        cod.major = ESP_BT_COD_MAJOR_DEV_PERIPHERAL;
        cod.minor = (uint8_t)minor;
    } else {
        /* Nothing HID to claim; stay the audio device (or a bare hands-free). */
        cod.major = ESP_BT_COD_MAJOR_DEV_AV;
        cod.minor = 0x02; /* hands-free */
        if (!audio) {
            cod.service = 0;
        }
    }
    return cod;
}

static void classic_identity(void)
{
    (void)esp_bt_gap_set_device_name(BT_CLASSIC_NAME);
    const esp_bt_cod_t cod = classic_cod();
    ESP_LOGI(TAG, "class of device: major=%u minor=0x%02X service=0x%03X",
             (unsigned)cod.major, (unsigned)cod.minor, (unsigned)cod.service);
    (void)esp_bt_gap_set_cod(cod, ESP_BT_SET_COD_ALL);
    /* Discoverability is useful even before the profiles register. */
    (void)esp_bt_gap_set_scan_mode(ESP_BT_CONNECTABLE, ESP_BT_GENERAL_DISCOVERABLE);
}

/* ---------------------------------------------------------- BLE advertising --- */

/* Advertising belongs to the LE transport, not to any one GATT service: the
 * payload, the parameters and the decision to advertise all live here, next to
 * the Classic scan mode. Services (HID today, LE Audio later) only report when
 * their GATT database is up. */
#define BLE_ADV_POLL_US (50 * 1000)
/* How often the "still waiting for the service" line is repeated (poll ticks). */
#define BLE_ADV_POLL_LOG_EVERY 20
#define BLE_ADV_LINK_WATCH_US (1000 * 1000)

static bool s_adv_wanted;
static bool s_adv_data_ready;
static bool s_advertising;
static void adv_policy_enable(void);
static unsigned s_adv_polls;
static esp_timer_handle_t s_adv_timer;
static esp_timer_handle_t s_adv_watch;
static bool s_ble_link_was_up;

/* Hand-built advertisement: flags + service UUID + name must fit the 31-byte
 * legacy payload. The Bluedroid esp_ble_adv_data_t path only accepts 128-bit
 * UUIDs, which would not fit. */
static esp_err_t ble_adv_configure(void)
{
    const char *name = app_bt_core_le_name();
    const size_t name_len = strlen(name);
    uint8_t adv_raw[31];
    size_t o = 0;
    /* Flags: LE general discoverable + BR/EDR not supported. The advertisement
     * runs on the LE-only static random address, so claiming BR/EDR support
     * makes Android treat it as a dual-mode device and connect over Classic
     * instead of GATT (the "connect LE, land on Classic" symptom). */
    adv_raw[o++] = 0x02;
    adv_raw[o++] = 0x01;
    adv_raw[o++] = 0x06; /* LE General Discoverable | BR/EDR Not Supported */
    /* 16-bit service UUID list: HID over GATT (0x1812). */
    adv_raw[o++] = 0x03;
    adv_raw[o++] = 0x03;
    adv_raw[o++] = 0x12;
    adv_raw[o++] = 0x18;
    /* Complete local name (truncated if it would overflow the payload). */
    const size_t room = sizeof(adv_raw) - (o + 2);
    const size_t copy = name_len < room ? name_len : room;
    adv_raw[o++] = (uint8_t)(copy + 1);
    adv_raw[o++] = 0x09;
    memcpy(&adv_raw[o], name, copy);
    o += copy;

    s_adv_data_ready = false;
    const esp_err_t err = esp_ble_gap_config_adv_data_raw(adv_raw, (uint32_t)o);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "raw advertisement rejected: %s", esp_err_to_name(err));
    }
    return err;
}

static void ble_adv_start(void)
{
    esp_ble_adv_params_t params = {
        .adv_int_min = 0x20,
        .adv_int_max = 0x30,
        .adv_type = ADV_TYPE_IND,
        .own_addr_type = BLE_ADDR_TYPE_RANDOM,
        .channel_map = ADV_CHNL_ALL,
        .adv_filter_policy = ADV_FILTER_ALLOW_SCAN_ANY_CON_ANY,
    };
    const esp_err_t err = esp_ble_gap_start_advertising(&params);
    ESP_LOGI(TAG, "start advertising: %s", esp_err_to_name(err));
}

/* Start only when the policy asked for it, the payload is accepted and the
 * service that the payload names already exists. */
static void ble_adv_apply(void)
{
    if (!s_adv_wanted) {
        return;
    }
    if (!s_adv_data_ready) {
        ESP_LOGI(TAG, "advertising deferred (payload not ready)");
        return;
    }
    if (!app_ble_hid_service_ready()) {
        ESP_LOGI(TAG, "advertising deferred (service not ready)");
        return;
    }
    ble_adv_start();
}

/* RPC method 8 asks for advertising again; the hook keeps app_cfg free of a
 * dependency on this component. */
static void adv_restart_hook(void)
{
    app_bt_core_ble_advertise(true);
}

void app_bt_core_ble_advertise(bool on)
{
    if (!app_bt_core_wants_ble()) {
        return;
    }
    s_adv_wanted = on;
    if (on) {
        if (!app_ble_hid_service_ready() && s_adv_timer == NULL) {
            /* Asked before the service was up: wait for it instead of dropping
             * the request. */
            adv_policy_enable();
        }
        ble_adv_apply();
        return;
    }
    s_advertising = false;
    const esp_err_t err = esp_ble_gap_stop_advertising();
    ESP_LOGI(TAG, "stop advertising: %s", esp_err_to_name(err));
}

/* Bluedroid stops advertising when a host connects; once the link drops the
 * device has to become discoverable again by itself. */
static void adv_link_watch_cb(void *arg)
{
    (void)arg;
    const bool up = app_ble_hid_connected();
    if (up) {
        /* A connected host means the controller stopped advertising. */
        s_advertising = false;
    } else if (s_ble_link_was_up) {
        ESP_LOGI(TAG, "BLE link dropped; advertising again");
    }
    /* Self-healing: whenever the payload and the service are ready, the policy
     * wants advertising and no host is connected, make sure it is on. There is
     * no timeout on the service check - a late service is picked up on the next
     * tick instead of being given up on. */
    if (!up && s_adv_wanted && s_adv_data_ready && app_ble_hid_service_ready() &&
        !s_advertising) {
        ble_adv_apply();
    }
    s_ble_link_was_up = up;
}

static void adv_policy_cb(void *arg)
{
    (void)arg;
    if (!app_ble_hid_service_ready()) {
        /* Keep waiting: the service comes up asynchronously and a late one is
         * just as good. Repeat the notice once a second instead of flooding. */
        if (++s_adv_polls % BLE_ADV_POLL_LOG_EVERY == 1) {
            ESP_LOGW(TAG, "waiting for the BLE service (%u ms)",
                     (unsigned)(s_adv_polls * (BLE_ADV_POLL_US / 1000)));
        }
        return;
    }
    s_adv_wanted = true;
    /* Configure once the service exists; the payload-set-complete event starts
     * advertising. A failed configuration is retried on the next tick. */
    if (ble_adv_configure() != ESP_OK) {
        s_adv_wanted = false;
        return;
    }
    (void)esp_timer_stop(s_adv_timer);
    (void)esp_timer_delete(s_adv_timer);
    s_adv_timer = NULL;
}

static void adv_policy_enable(void)
{
    if (!app_bt_core_wants_ble() || app_cfg_ble_hid() == 0) {
        return;
    }
    if (s_adv_timer != NULL) {
        return;
    }
    s_adv_polls = 0;
    const esp_timer_create_args_t args = {
        .callback = adv_policy_cb,
        .name = "ble_adv_policy",
    };
    if (esp_timer_create(&args, &s_adv_timer) != ESP_OK) {
        ESP_LOGW(TAG, "advertising policy timer unavailable");
        return;
    }
    if (esp_timer_start_periodic(s_adv_timer, BLE_ADV_POLL_US) != ESP_OK) {
        (void)esp_timer_delete(s_adv_timer);
        s_adv_timer = NULL;
        return;
    }

    const esp_timer_create_args_t watch_args = {
        .callback = adv_link_watch_cb,
        .name = "ble_adv_watch",
    };
    if (esp_timer_create(&watch_args, &s_adv_watch) == ESP_OK) {
        (void)esp_timer_start_periodic(s_adv_watch, BLE_ADV_LINK_WATCH_US);
    }
}

/* Single GAP callback for the LE transport: advertising events are handled here,
 * everything else (pairing, connection parameters) is forwarded to the HID
 * service that still owns those semantics. */
static void ble_gap_cb(esp_gap_ble_cb_event_t event, esp_ble_gap_cb_param_t *param)
{
    switch (event) {
    case ESP_GAP_BLE_ADV_DATA_RAW_SET_COMPLETE_EVT:
    case ESP_GAP_BLE_ADV_DATA_SET_COMPLETE_EVT:
        s_adv_data_ready = true;
        ble_adv_apply();
        break;
    case ESP_GAP_BLE_ADV_START_COMPLETE_EVT:
        s_advertising = param->adv_start_cmpl.status == ESP_BT_STATUS_SUCCESS;
        ESP_LOGI(TAG, "advertising %s", s_advertising ? "on" : "failed");
        break;
    case ESP_GAP_BLE_ADV_STOP_COMPLETE_EVT:
        s_advertising = false;
        ESP_LOGI(TAG, "advertising stopped");
        break;
    default:
        app_ble_hid_gap_event(event, param);
        break;
    }
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
        /* The GAP callback first: identity setup and the advertisement both
         * report through it. */
        ESP_RETURN_ON_ERROR(esp_ble_gap_register_callback(ble_gap_cb), TAG, "gap cb");
        ble_identity();
    }
    if (s_mode == APP_BT_CLASSIC || s_mode == APP_BT_BOTH) {
        classic_identity();
    }
    s_ready = true;
    app_cfg_set_ble_adv_rpc(adv_restart_hook);
    kick_timer_apply();
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
/* The two transports have independent class masks, so each gets its own map.
 * Budget: gamepad 50 + mouse 69 + keyboard 83 + motion 58 = 260 bytes; 288
 * leaves 28 bytes of headroom. The keyboard descriptor carries the consumer
 * bitmap and the motion descriptor the three sensor collections, so both grew;
 * anything larger silently drops a class. */
static uint8_t s_report_map_ble[288];
static size_t s_report_map_ble_len;
static uint8_t s_report_map_bt[288];
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
        if (desc == NULL) {
            continue;
        }
        if (used + len > cap) {
            ESP_LOGE(TAG, "report map full: class %u needs %u B, only %u free",
                     (unsigned)i, (unsigned)len, (unsigned)(cap - used));
            continue;
        }
        memcpy(&out[used], desc, len);
        used += len;
    }
    if ((mask & APP_CLASS_SENSOR) != 0) {
        size_t len = 0;
        const uint8_t *desc = app_usb_hid_sensor_desc(&len);
        if (desc != NULL) {
            if (used + len > cap) {
                ESP_LOGE(TAG, "report map full: motion needs %u B, only %u free",
                         (unsigned)len, (unsigned)(cap - used));
            } else {
                memcpy(&out[used], desc, len);
                used += len;
            }
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
    /* BLE advertising is a policy decision: ask for it only after the BLE HID
     * service is up, and only when BLE is configured to expose classes. */
    adv_policy_enable();
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
