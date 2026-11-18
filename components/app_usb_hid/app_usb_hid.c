// SPDX-License-Identifier: Apache-2.0

#include "app_usb_hid.h"

#include <stdatomic.h>
#include <string.h>

#include "app_cfg.h"
#include "class/hid/hid_device.h"
#include "driver/gpio.h"
#include "esp_check.h"
#include "esp_iris_usb_hid.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "tusb.h"

static const char *TAG = "app_usb_hid";

static const uint8_t s_desc_gp[] = {
    0x05, 0x01, 0x09, 0x05, 0xA1, 0x01, 0x85, 0x01,
    /* 16 buttons */
    0x05, 0x09, 0x19, 0x01, 0x29, 0x10, 0x15, 0x00, 0x25, 0x01, 0x75, 0x01,
    0x95, 0x10, 0x81, 0x02,
    /* left stick X/Y, right stick Rx/Ry (0..255, centre 128) */
    0x05, 0x01, 0x09, 0x30, 0x09, 0x31, 0x09, 0x33, 0x09, 0x34,
    0x15, 0x00, 0x26, 0xFF, 0x00, 0x75, 0x08, 0x95, 0x04, 0x81, 0x02,
    0xC0,
};

static const uint8_t s_desc_mouse[] = {
    0x05, 0x01, 0x09, 0x02, 0xA1, 0x01, 0x85, 0x02, 0x09, 0x01, 0xA1, 0x00,
    0x05, 0x09, 0x19, 0x01, 0x29, 0x03, 0x15, 0x00, 0x25, 0x01, 0x95, 0x03,
    0x75, 0x01, 0x81, 0x02, 0x95, 0x01, 0x75, 0x05, 0x81, 0x03, 0x05, 0x01,
    0x09, 0x30, 0x09, 0x31, 0x09, 0x38, 0x15, 0x81, 0x25, 0x7F, 0x75, 0x08,
    0x95, 0x03, 0x81, 0x06,
    /* AC Pan (horizontal scroll) */
    0x05, 0x0C, 0x0A, 0x38, 0x02, 0x15, 0x81, 0x25, 0x7F, 0x75, 0x08,
    0x95, 0x01, 0x81, 0x06,
    0xC0, 0xC0,
};

static const uint8_t s_desc_kbd[] = {
    0x05, 0x01, 0x09, 0x06, 0xA1, 0x01, 0x85, 0x03, 0x05, 0x07, 0x19, 0xE0,
    0x29, 0xE7, 0x15, 0x00, 0x25, 0x01, 0x75, 0x01, 0x95, 0x08, 0x81, 0x02,
    0x95, 0x01, 0x75, 0x08, 0x81, 0x03, 0x05, 0x07, 0x19, 0x00, 0x29, 0x65,
    0x15, 0x00, 0x25, 0x65, 0x75, 0x08, 0x95, 0x06, 0x81, 0x00, 0xC0,
};


/* Sony-compatible gamepad: one 64-byte input report (report ID 1 + 63 bytes).
 * The first 4 bytes are the sticks, then 14 button bits, then vendor padding to
 * the fixed report length SDL/Steam expect for a DualShock/DualSense device. */
static const uint8_t s_sony_desc[] = {
    0x05, 0x01, 0x09, 0x05, 0xA1, 0x01, 0x85, 0x01,
    0x05, 0x01, 0x09, 0x30, 0x09, 0x31, 0x09, 0x32, 0x09, 0x33,
    0x15, 0x00, 0x26, 0xFF, 0x00, 0x75, 0x08, 0x95, 0x04, 0x81, 0x02,
    0x05, 0x09, 0x19, 0x01, 0x29, 0x0E,
    0x15, 0x00, 0x25, 0x01, 0x75, 0x01, 0x95, 0x0E, 0x81, 0x02,
    0x75, 0x02, 0x95, 0x01, 0x81, 0x03,
    0x06, 0x00, 0xFF, 0x09, 0x20, 0x75, 0x08, 0x95, 0x39, 0x81, 0x02,
    0xC0,
};

static uint8_t s_sony_instance = 0xFF;
static app_usb_hid_mode_t s_sony_mode = APP_USB_HID_GENERIC;
static uint16_t s_sony_counter;

/* Direction pad as the 8-way hat the Sony report uses (8 = released). */
static uint8_t sony_hat(uint16_t buttons)
{
    const bool up = (buttons & (1u << 4)) != 0;
    const bool down = (buttons & (1u << 5)) != 0;
    const bool left = (buttons & (1u << 6)) != 0;
    const bool right = (buttons & (1u << 7)) != 0;
    if (up && right) return 1;
    if (down && right) return 3;
    if (down && left) return 5;
    if (up && left) return 7;
    if (up) return 0;
    if (right) return 2;
    if (down) return 4;
    if (left) return 6;
    return 8;
}

static void sony_report(uint8_t *out, uint16_t buttons, const uint8_t sticks[4])
{
    memset(out, 0, 64);
    out[0] = 0x01;
    out[1] = sticks[0];
    out[2] = sticks[1];
    out[3] = sticks[2];
    out[4] = sticks[3];
    /* Y = square, A = cross, B = circle, X = triangle. */
    const uint8_t faces = (uint8_t)(((buttons & (1u << 3)) ? 0x10 : 0) |
                                    ((buttons & (1u << 0)) ? 0x20 : 0) |
                                    ((buttons & (1u << 1)) ? 0x40 : 0) |
                                    ((buttons & (1u << 2)) ? 0x80 : 0));
    if (s_sony_mode == APP_USB_HID_DS5) {
        out[7] = (uint8_t)(s_sony_counter & 0xFF);
        out[8] = (uint8_t)(sony_hat(buttons) | faces);
    } else {
        out[5] = (uint8_t)(sony_hat(buttons) | faces);
    }
    out[10] = (uint8_t)(s_sony_counter & 0xFF);
    out[11] = (uint8_t)(s_sony_counter >> 8);
}

#define HID_REPORT_GAMEPAD 1
#define HID_REPORT_MOUSE 2
#define HID_REPORT_KEYBOARD 3

#define GAMEPAD_REPORT_LEN 6
#define MOUSE_REPORT_LEN 5
#define KEYBOARD_REPORT_LEN 8

#define KEYBOARD_KEYS 6
#define REPORT_PERIOD_MS 8
/* The BR/EDR interrupt channel cannot drain 125 Hz; a mouse report that often
 * only builds a backlog, which is felt as growing cursor lag. Send motion every
 * second tick (~62 Hz) and keep the gamepad/keyboard at the full rate. */
#define MOUSE_EVERY_TICKS 2
#define KEEPALIVE_MS 250
#define APP_HID_AI_BUTTON_GPIO GPIO_NUM_7
#define TOUCH_MAX_COORD 1023
#define UI_SIZE_PX 480
#define USB_LAST_IN_EP 0x87

/* One registered HID interface per enabled class. Instance indices follow the
 * registration order; a disabled class keeps 0xFF. */
static struct {
    uint8_t instance[4];   /* by class bit index */
    size_t count;
    uint8_t report_map[512];
    size_t report_map_len;
} s_usb;

static atomic_int s_buttons;
static atomic_int s_stick_lx = 128, s_stick_ly = 128;
static atomic_int s_stick_rx = 128, s_stick_ry = 128;
static atomic_int s_mouse_buttons;
static atomic_int s_mouse_dx;
static atomic_int s_mouse_dy;
static atomic_int s_mouse_wheel;
static atomic_int s_mouse_pan;
static atomic_int s_kbd_mods;
static atomic_int s_kbd_keys[KEYBOARD_KEYS];

/* One HID interface per class, each with its own IN endpoint. The S31 USB core
 * exposes IN endpoints 0..7; ESP-Iris already spends 0x81..0x84 on its two CDC
 * pairs, so the HID functions use 0x85..0x87. */
static const struct {
    uint32_t class_mask;
    const uint8_t *desc;
    uint16_t len;
    uint8_t ep_in;
} s_interfaces[] = {
    {APP_CLASS_GAMEPAD, s_desc_gp, sizeof(s_desc_gp), 0x85},
    {APP_CLASS_MOUSE, s_desc_mouse, sizeof(s_desc_mouse), 0x86},
    {APP_CLASS_KEYBOARD, s_desc_kbd, sizeof(s_desc_kbd), 0x87},
};
#define INTERFACE_COUNT (sizeof(s_interfaces) / sizeof(s_interfaces[0]))

/* TinyUSB asks for the report descriptor per HID instance. */
uint8_t const *tud_hid_descriptor_report_cb(uint8_t instance)
{
    for (size_t i = 0; i < INTERFACE_COUNT; ++i) {
        if (s_usb.instance[i] == instance) {
            return s_interfaces[i].desc;
        }
    }
    return NULL;
}

void tud_hid_set_report_cb(uint8_t instance, uint8_t report_id,
                           hid_report_type_t report_type, uint8_t const *buffer,
                           uint16_t bufsize)
{
    (void)instance; (void)report_id; (void)report_type; (void)buffer; (void)bufsize;
}

uint16_t tud_hid_get_report_cb(uint8_t instance, uint8_t report_id,
                               hid_report_type_t report_type, uint8_t *buffer,
                               uint16_t reqlen)
{
    if (instance == s_sony_instance && report_type == HID_REPORT_TYPE_FEATURE &&
        buffer != NULL) {
        /* Calibration/firmware feature reports: SDL and Steam read them but
         * accept zeroed defaults, so an empty answer keeps enumeration simple. */
        memset(buffer, 0, reqlen);
        return reqlen;
    }
    (void)report_id;
    return 0;
}

bool app_usb_hid_usb_connected(void)
{
    return tud_mounted();
}

size_t app_usb_hid_class_count(void)
{
    return INTERFACE_COUNT;
}

const uint8_t *app_usb_hid_class_desc(size_t index, size_t *len)
{
    if (index >= INTERFACE_COUNT) {
        return NULL;
    }
    if (len != NULL) {
        *len = s_interfaces[index].len;
    }
    return s_interfaces[index].desc;
}

/* Bluetooth transport hooks, attached by app_bt_core when its HID stack is up. */
static app_usb_hid_bt_connected_fn s_bt_connected_fn;
static app_usb_hid_bt_send_fn s_bt_send_fn;

void app_usb_hid_attach_bt(app_usb_hid_bt_connected_fn connected,
                            app_usb_hid_bt_send_fn send)
{
    s_bt_connected_fn = connected;
    s_bt_send_fn = send;
}

static bool bt_connected(void)
{
    return s_bt_send_fn != NULL && s_bt_connected_fn != NULL && s_bt_connected_fn();
}

static void bt_send(uint8_t report_id, const uint8_t *data, size_t len)
{
    if (s_bt_send_fn != NULL) {
        s_bt_send_fn(report_id, data, len);
    }
}

void app_usb_hid_set_buttons(uint16_t mask)
{
    atomic_store(&s_buttons, mask);
}

void app_usb_hid_set_sticks(uint8_t lx, uint8_t ly, uint8_t rx, uint8_t ry)
{
    atomic_store(&s_stick_lx, lx);
    atomic_store(&s_stick_ly, ly);
    atomic_store(&s_stick_rx, rx);
    atomic_store(&s_stick_ry, ry);
}

void app_usb_hid_mouse(uint8_t buttons, int8_t dx, int8_t dy, int8_t wheel, int8_t pan)
{
    if (dx != 0) atomic_fetch_add(&s_mouse_dx, dx);
    if (dy != 0) atomic_fetch_add(&s_mouse_dy, dy);
    if (wheel != 0) atomic_fetch_add(&s_mouse_wheel, wheel);
    if (pan != 0) atomic_fetch_add(&s_mouse_pan, pan);
    atomic_store(&s_mouse_buttons, buttons);
}

void app_usb_hid_keyboard(uint8_t modifiers, const uint8_t keys[KEYBOARD_KEYS])
{
    atomic_store(&s_kbd_mods, modifiers);
    for (int i = 0; i < KEYBOARD_KEYS; ++i) {
        atomic_store(&s_kbd_keys[i], keys[i]);
    }
}

static bool instance_for(uint32_t class_mask, uint8_t *out)
{
    for (size_t i = 0; i < INTERFACE_COUNT; ++i) {
        if (s_interfaces[i].class_mask == class_mask) {
            if (s_usb.instance[i] == 0xFF) {
                return false;
            }
            *out = s_usb.instance[i];
            return true;
        }
    }
    return false;
}

static int clamp_delta(int v)
{
    return v > 127 ? 127 : (v < -127 ? -127 : v);
}

static void mouse_report(uint8_t out[MOUSE_REPORT_LEN], int dx, int dy, int wheel,
                         int pan, uint8_t buttons)
{
    out[0] = buttons;
    out[1] = (uint8_t)(int8_t)dx;
    out[2] = (uint8_t)(int8_t)dy;
    out[3] = (uint8_t)(int8_t)wheel;
    out[4] = (uint8_t)(int8_t)pan;
}

static void hid_task(void *arg)
{
    (void)arg;
    uint8_t gamepad[GAMEPAD_REPORT_LEN];
    uint8_t mouse[MOUSE_REPORT_LEN];
    uint8_t keyboard[KEYBOARD_REPORT_LEN];
    uint8_t usb_last_gp[GAMEPAD_REPORT_LEN] = {0};
    uint8_t bt_last_gp[GAMEPAD_REPORT_LEN] = {0};
    uint8_t usb_last_kbd[KEYBOARD_REPORT_LEN] = {0};
    uint8_t bt_last_kbd[KEYBOARD_REPORT_LEN] = {0};
    int usb_sent_x = 0, usb_sent_y = 0, usb_sent_w = 0, usb_sent_p = 0;
    int bt_sent_x = 0, bt_sent_y = 0, bt_sent_w = 0, bt_sent_p = 0;
    uint8_t usb_buttons = 0, bt_buttons = 0;
    unsigned idle = 0;
    unsigned mouse_tick = 0;
    const unsigned keepalive_ticks = KEEPALIVE_MS / REPORT_PERIOD_MS;

    for (;;) {
        uint16_t buttons = (uint16_t)atomic_load(&s_buttons);
        if (gpio_get_level(APP_HID_AI_BUTTON_GPIO) == 0) {
            buttons |= 1u << 8; /* button 9 */
        }
        gamepad[0] = (uint8_t)(buttons & 0xFF);
        gamepad[1] = (uint8_t)(buttons >> 8);
        gamepad[2] = (uint8_t)atomic_load(&s_stick_lx);
        gamepad[3] = (uint8_t)atomic_load(&s_stick_ly);
        gamepad[4] = (uint8_t)atomic_load(&s_stick_rx);
        gamepad[5] = (uint8_t)atomic_load(&s_stick_ry);

        /* Layout is [modifier][reserved][6 keys]: the descriptor reserves the
         * second byte, so the keycodes start at index 2. */
        keyboard[0] = (uint8_t)atomic_load(&s_kbd_mods);
        keyboard[1] = 0;
        for (int i = 0; i < KEYBOARD_KEYS; ++i) {
            keyboard[2 + i] = (uint8_t)atomic_load(&s_kbd_keys[i]);
        }

        /* Keepalive: resend the current state to both transports so a host
         * that missed a report (or reconnected) catches up. */
        if (++idle >= keepalive_ticks) {
            idle = 0;
            memset(usb_last_gp, 0xEE, sizeof(usb_last_gp));
            memset(bt_last_gp, 0xEE, sizeof(bt_last_gp));
            memset(usb_last_kbd, 0xEE, sizeof(usb_last_kbd));
            memset(bt_last_kbd, 0xEE, sizeof(bt_last_kbd));
        }

        /* Each interface gates on its own endpoint readiness: the host may
         * open only one of them (a keyboard app never polls the gamepad). */
        const bool usb = tud_mounted();
        const bool bt = bt_connected();
        uint8_t inst = 0;

        /* USB and Bluetooth are serviced independently: each transport keeps
         * its own snapshot of the last report it accepted and its own mouse
         * delta cursor, so a slow/blocked link can never delay the other or
         * swallow its motion. */
        const int total_x = atomic_load(&s_mouse_dx);
        const int total_y = atomic_load(&s_mouse_dy);
        const int total_w = atomic_load(&s_mouse_wheel);
        const int total_p = atomic_load(&s_mouse_pan);
        const uint8_t mouse_buttons_now = (uint8_t)atomic_load(&s_mouse_buttons);
        ++mouse_tick;

        /* ---- USB ---------------------------------------------------- */
        if (usb) {
            if (memcmp(gamepad, usb_last_gp, sizeof(gamepad)) != 0 &&
                instance_for(APP_CLASS_GAMEPAD, &inst) && tud_hid_n_ready(inst)) {
                (void)tud_hid_n_report(inst, HID_REPORT_GAMEPAD, gamepad, sizeof(gamepad));
                memcpy(usb_last_gp, gamepad, sizeof(gamepad));
            }
            if (memcmp(keyboard, usb_last_kbd, sizeof(keyboard)) != 0 &&
                instance_for(APP_CLASS_KEYBOARD, &inst) && tud_hid_n_ready(inst)) {
                (void)tud_hid_n_report(inst, HID_REPORT_KEYBOARD, keyboard, sizeof(keyboard));
                memcpy(usb_last_kbd, keyboard, sizeof(keyboard));
            }
            if (instance_for(APP_CLASS_MOUSE, &inst) && tud_hid_n_ready(inst)) {
                const int rx = clamp_delta(total_x - usb_sent_x);
                const int ry = clamp_delta(total_y - usb_sent_y);
                const int rw = clamp_delta(total_w - usb_sent_w);
                const int rp = clamp_delta(total_p - usb_sent_p);
                if (rx != 0 || ry != 0 || rw != 0 || rp != 0 || mouse_buttons_now != usb_buttons) {
                    mouse_report(mouse, rx, ry, rw, rp, mouse_buttons_now);
                    if (tud_hid_n_report(inst, HID_REPORT_MOUSE, mouse, sizeof(mouse))) {
                        usb_sent_x += rx;
                        usb_sent_y += ry;
                        usb_sent_w += rw;
                        usb_sent_p += rp;
                        usb_buttons = mouse_buttons_now;
                    }
                }
            }
        }

        /* ---- Bluetooth ---------------------------------------------- */
        if (bt) {
            if (memcmp(gamepad, bt_last_gp, sizeof(gamepad)) != 0) {
                bt_send(HID_REPORT_GAMEPAD, gamepad, sizeof(gamepad));
                memcpy(bt_last_gp, gamepad, sizeof(gamepad));
            }
            if (memcmp(keyboard, bt_last_kbd, sizeof(keyboard)) != 0) {
                bt_send(HID_REPORT_KEYBOARD, keyboard, sizeof(keyboard));
                memcpy(bt_last_kbd, keyboard, sizeof(keyboard));
            }
            /* Classic HID cannot drain 125 Hz; ~62 Hz keeps the queue short. */
            if (mouse_tick % MOUSE_EVERY_TICKS == 0) {
                const int rx = clamp_delta(total_x - bt_sent_x);
                const int ry = clamp_delta(total_y - bt_sent_y);
                const int rw = clamp_delta(total_w - bt_sent_w);
                const int rp = clamp_delta(total_p - bt_sent_p);
                if (rx != 0 || ry != 0 || rw != 0 || rp != 0 || mouse_buttons_now != bt_buttons) {
                    mouse_report(mouse, rx, ry, rw, rp, mouse_buttons_now);
                    bt_send(HID_REPORT_MOUSE, mouse, sizeof(mouse));
                    bt_sent_x += rx;
                    bt_sent_y += ry;
                    bt_sent_w += rw;
                    bt_sent_p += rp;
                    bt_buttons = mouse_buttons_now;
                }
            }
        }

        if (s_sony_instance != 0xFF) {
            /* A DualShock/DualSense streams its input report continuously. */
            const uint8_t sticks[4] = {
                (uint8_t)atomic_load(&s_stick_lx), (uint8_t)atomic_load(&s_stick_ly),
                (uint8_t)atomic_load(&s_stick_rx), (uint8_t)atomic_load(&s_stick_ry),
            };
            uint8_t sony[64];
            sony_report(sony, (uint16_t)atomic_load(&s_buttons), sticks);
            ++s_sony_counter;
            if (usb && tud_hid_n_ready(s_sony_instance)) {
                (void)tud_hid_n_report(s_sony_instance, HID_REPORT_GAMEPAD, sony,
                                       sizeof(sony));
            }
            bt_send(HID_REPORT_GAMEPAD, sony, sizeof(sony));
        }

        vTaskDelay(pdMS_TO_TICKS(REPORT_PERIOD_MS));
    }
}

esp_err_t app_usb_hid_register_usb(uint8_t first_ep_in)
{
    for (size_t i = 0; i < INTERFACE_COUNT; ++i) {
        s_usb.instance[i] = 0xFF;
    }
    esp_iris_usb_hid_iface_t ifaces[INTERFACE_COUNT];
    size_t n = 0;
    s_usb.report_map_len = 0;
    for (size_t i = 0; i < INTERFACE_COUNT; ++i) {
        if ((app_cfg_classes() & s_interfaces[i].class_mask) == 0) {
            continue;
        }
        ifaces[n] = (esp_iris_usb_hid_iface_t){
            .report_desc = s_interfaces[i].desc,
            .report_desc_len = s_interfaces[i].len,
            /* Endpoints are handed out in registration order so they always
             * stay inside what ESP-Iris left free (0x83.. or 0x85..). */
            .ep_in = (uint8_t)(first_ep_in + n),
            .ep_size = 8,
            .interval_ms = 4,
        };
        s_usb.instance[i] = (uint8_t)n;
        ++n;
        memcpy(&s_usb.report_map[s_usb.report_map_len], s_interfaces[i].desc,
               s_interfaces[i].len);
        s_usb.report_map_len += s_interfaces[i].len;
    }
    /* Optional Sony-compatible interface: additive, never a replacement. */
    s_sony_instance = 0xFF;
    s_sony_mode = app_cfg_usb_screen_mode();
    if (s_sony_mode != APP_USB_HID_GENERIC) {
        if ((unsigned)first_ep_in + n <= USB_LAST_IN_EP) {
            ifaces[n] = (esp_iris_usb_hid_iface_t){
                .report_desc = s_sony_desc,
                .report_desc_len = sizeof(s_sony_desc),
                .ep_in = (uint8_t)(first_ep_in + n),
                .ep_size = 64,
                .interval_ms = 4,
            };
            s_sony_instance = (uint8_t)n;
            ++n;
        } else {
            ESP_LOGW(TAG, "no free IN endpoint for the %s interface",
                     app_cfg_usb_hid_mode_name(s_sony_mode));
        }
    }

    s_usb.count = n;
    ESP_LOGI(TAG, "USB HID interfaces: %u (classes 0x%X, mode %s)", (unsigned)n,
             (unsigned)app_cfg_classes(), app_cfg_usb_hid_mode_name(s_sony_mode));
    if (n == 0) {
        return ESP_OK;
    }
    return esp_iris_usb_set_hid_ifaces(ifaces, n);
}

esp_err_t app_usb_hid_start(void)
{
    const gpio_config_t ai = {
        .pin_bit_mask = 1ULL << APP_HID_AI_BUTTON_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&ai), TAG, "AI button GPIO");

    if (xTaskCreate(hid_task, "usb_core", 4096, NULL, 5, NULL) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "HID bridge started");
    return ESP_OK;
}
