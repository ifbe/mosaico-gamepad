// SPDX-License-Identifier: Apache-2.0

/* Page semantics for the controller surface.
 *
 * This file owns everything that interprets a physical sample as an action:
 * BOOT key cycling, the mouse/keyboard/gamepad surface state machines, the
 * settings chips, and the touch overlay chrome. It reads device events from
 * app_ui_touch and renders through app_ui.h. Page switching is only the BOOT
 * key and the layout RPC; tapping a screen corner no longer changes pages. */

#include "app_input.h"
#include "app_ui_touch.h"
#include "ui_internal.h"

#include "app_cfg.h"
#include "app_ui.h"
#include "app_sensor.h"
#include "app_usb_hid.h"

#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

static const char *TAG = "app_ui";

#define TOUCH_MAX_POINTS 2
#define INPUT_PERIOD_MS 16
#define INPUT_TASK_STACK 4096
#define INPUT_TASK_PRIO 6

/* Overlay chrome: one translucent circle per contact on the idle screen and a
 * press highlight per contact on the keyboard. */
#define CIRCLE_DIAMETER 64
#define CIRCLE_OPACITY 220
#define KEY_GLOW_OPACITY 110
#define OVERLAY_MIN_PERIOD_MS 50

#define RGB565(r, g, b) ((uint16_t)((((r) & 0xF8u) << 8) | (((g) & 0xFCu) << 3) | ((b) >> 3)))
static const uint16_t s_circle_color[TOUCH_MAX_POINTS] = {
    RGB565(0x00, 0xE5, 0xFF),
    RGB565(0xFF, 0x7A, 0x00),
};
static const uint16_t s_key_color = RGB565(0x00, 0xE5, 0xFF);

#define TAP_MAX_MS 250
#define TAP_MAX_DIST 10

/* Gamepad layout quadrants: top-left/right sticks, bottom-left d-pad,
 * bottom-right ABXY. */
typedef enum {
    ROLE_NONE = 0,
    ROLE_LSTICK,
    ROLE_RSTICK,
    ROLE_DPAD,
    ROLE_ABXY,
} role_t;

#define STICK_L_CX 120
#define STICK_L_CY 120
#define STICK_R_CX 360
#define STICK_R_CY 120
#define STICK_RADIUS 60
#define KNOB_SIZE 44

static const struct {
    int32_t x, y, w, h;
    uint16_t bit;
} s_abxy[] = {
    {330, 274, 60, 60, 1u << 2}, /* X, centre (360,304) */
    {386, 330, 60, 60, 1u << 0}, /* A, centre (416,360) */
    {330, 386, 60, 60, 1u << 1}, /* B, centre (360,416) */
    {274, 330, 60, 60, 1u << 3}, /* Y, centre (304,360) */
};
#define ABXY_COUNT (sizeof(s_abxy) / sizeof(s_abxy[0]))

/* Controls around the two sticks (scene coordinates from ui/main.json).
 * trigger != 0 turns the rectangle into an analog slider for that trigger: the
 * value follows the contact position along the bar, top = 0, bottom = 255.
 *
 * A contact is *latched* to whatever control it landed on: dragging from LT
 * over the stick keeps driving LT and never moves the stick. */
static const struct {
    int32_t x, y, w, h;
    uint16_t bit;
    uint8_t trigger; /* 0 = none, 1 = LT, 2 = RT */
    app_ui_side_t glow;
} s_side[] = {
    {60, 12, 120, 34, 1u << 8, 0, APP_UI_SIDE_LB},    /* bar above the left stick */
    {188, 60, 40, 120, 1u << 10, 0, APP_UI_SIDE_LS},  /* bar right of the left stick */
    {60, 192, 120, 34, 1u << 12, 0, APP_UI_SIDE_SELECT}, /* bar under the left stick */
    {8, 60, 40, 120, 0, 1, APP_UI_SIDE_LT},           /* left analog slider */
    {300, 12, 120, 34, 1u << 9, 0, APP_UI_SIDE_RB},   /* bar above the right stick */
    {252, 60, 40, 120, 1u << 11, 0, APP_UI_SIDE_RS},  /* bar left of the right stick */
    {300, 192, 120, 34, 1u << 13, 0, APP_UI_SIDE_START}, /* bar under the right stick */
    {432, 60, 40, 120, 0, 2, APP_UI_SIDE_RT},         /* right analog slider */
};
#define SIDE_COUNT (sizeof(s_side) / sizeof(s_side[0]))
#define SIDE_NONE ((int8_t)-1)

static int8_t side_at(int32_t x, int32_t y)
{
    for (size_t i = 0; i < SIDE_COUNT; ++i) {
        if (x >= s_side[i].x && x < s_side[i].x + s_side[i].w &&
            y >= s_side[i].y && y < s_side[i].y + s_side[i].h) {
            return (int8_t)i;
        }
    }
    return SIDE_NONE;
}

/* Slider position -> 0..255, clamped to the bar. */
static uint8_t side_analog(size_t i, int32_t y)
{
    if (s_side[i].h <= 1) {
        return 0;
    }
    int32_t rel = y - s_side[i].y;
    if (rel < 0) {
        rel = 0;
    }
    if (rel > s_side[i].h - 1) {
        rel = s_side[i].h - 1;
    }
    return (uint8_t)(rel * 255 / (s_side[i].h - 1));
}

static const struct {
    int32_t x, y, w, h;
    uint16_t bit;
    unsigned glow;
} s_dpad[] = {
    {96, 288, 48, 48, 1u << 4, 0}, /* up */
    {96, 384, 48, 48, 1u << 5, 1}, /* down */
    {48, 336, 48, 48, 1u << 6, 2}, /* left */
    {144, 336, 48, 48, 1u << 7, 3}, /* right */
};
#define DPAD_COUNT (sizeof(s_dpad) / sizeof(s_dpad[0]))

/* Keyboard keys: must match ui/main.json (12 columns x 40 px, 5 rows). */
static const struct {
    int16_t x, y, w, h;
    uint8_t code;
    uint8_t modifier;
} s_keys[] = {
    {0, 190, 40, 40, 0x1E, 0x00}, {40, 190, 40, 40, 0x1F, 0x00},
    {80, 190, 40, 40, 0x20, 0x00}, {120, 190, 40, 40, 0x21, 0x00},
    {160, 190, 40, 40, 0x22, 0x00}, {200, 190, 40, 40, 0x23, 0x00},
    {240, 190, 40, 40, 0x24, 0x00}, {280, 190, 40, 40, 0x25, 0x00},
    {320, 190, 40, 40, 0x26, 0x00}, {360, 190, 40, 40, 0x27, 0x00},
    {400, 190, 40, 40, 0x2D, 0x00}, {440, 190, 40, 40, 0x2E, 0x00}, /* - = */
    {0, 230, 40, 40, 0x14, 0x00}, {40, 230, 40, 40, 0x1A, 0x00},
    {80, 230, 40, 40, 0x08, 0x00}, {120, 230, 40, 40, 0x15, 0x00},
    {160, 230, 40, 40, 0x17, 0x00}, {200, 230, 40, 40, 0x1C, 0x00},
    {240, 230, 40, 40, 0x18, 0x00}, {280, 230, 40, 40, 0x0C, 0x00},
    {320, 230, 40, 40, 0x12, 0x00}, {360, 230, 40, 40, 0x13, 0x00},
    {400, 230, 40, 40, 0x2F, 0x00}, {440, 230, 40, 40, 0x30, 0x00}, /* [ ] */
    {0, 270, 40, 40, 0x04, 0x00}, {40, 270, 40, 40, 0x16, 0x00},
    {80, 270, 40, 40, 0x07, 0x00}, {120, 270, 40, 40, 0x09, 0x00},
    {160, 270, 40, 40, 0x0A, 0x00}, {200, 270, 40, 40, 0x0B, 0x00},
    {240, 270, 40, 40, 0x0D, 0x00}, {280, 270, 40, 40, 0x0E, 0x00},
    {320, 270, 40, 40, 0x0F, 0x00}, {360, 270, 40, 40, 0x33, 0x00}, /* ; */
    {400, 270, 40, 40, 0x34, 0x00}, {440, 270, 40, 40, 0x28, 0x00}, /* ' ENT */
    {0, 310, 40, 40, 0x1D, 0x00}, {40, 310, 40, 40, 0x1B, 0x00},
    {80, 310, 40, 40, 0x06, 0x00}, {120, 310, 40, 40, 0x19, 0x00},
    {160, 310, 40, 40, 0x05, 0x00}, {200, 310, 40, 40, 0x11, 0x00},
    {240, 310, 40, 40, 0x10, 0x00}, {280, 310, 40, 40, 0x36, 0x00},
    {320, 310, 40, 40, 0x37, 0x00}, {360, 310, 40, 40, 0x38, 0x00},
    {400, 310, 40, 40, 0x31, 0x00}, /* \ ; the last cell is empty */
    {0, 350, 40, 40, 0x2B, 0x00},   /* tab */
    {40, 350, 40, 40, 0x00, 0x02},  /* lshift */
    {80, 350, 40, 40, 0x00, 0x01},  /* lctrl */
    {120, 350, 40, 40, 0x00, 0x04}, /* lalt */
    {160, 350, 240, 40, 0x2C, 0x00}, /* space */
    {400, 350, 40, 40, 0x2A, 0x00}, /* backspace */
    {440, 350, 40, 40, 0x4C, 0x00}, /* delete */
};
#define KEY_COUNT (sizeof(s_keys) / sizeof(s_keys[0]))

typedef struct {
    int32_t x;
    int32_t y;
    bool active;
} point_t;

/* Contact state mirrors the device samples; it is written by the input task and
 * read by the overlay callback on the render task, so the samples stay volatile
 * to keep the compiler from caching them across the two tasks. */
static struct {
    volatile point_t points[TOUCH_MAX_POINTS];
    uint16_t button_mask;
    uint16_t dpad_mask;
    uint16_t side_mask;   /* side buttons currently held */
    int8_t capture[TOUCH_MAX_POINTS]; /* control the contact landed on, or SIDE_NONE */
    uint8_t trigger_l;    /* analog trigger levels derived from LT/RT bars */
    uint8_t trigger_r;
    role_t role[TOUCH_MAX_POINTS];
    uint8_t kbd_mods;
    uint8_t kbd_keys[APP_HID_KEYBOARD_KEYS];
    int key_idx[TOUCH_MAX_POINTS]; /* pressed key per contact, -1 = none */
    bool kbd_dirty;                /* key highlight set changed */
} s_in;

static struct {
    esp_gsp_handle_t gsp;
    bool registered;
    bool changed;   /* contact geometry changed since the last tick */
    bool released;  /* a contact lifted since the last tick */
    uint32_t last_ms;
} s_overlay;

static bool s_chip_touch;
static bool s_task_started;
static app_ui_layout_t s_last_layout = APP_UI_LAYOUT_BLANK;

static void overlay_cb(esp_gsp_handle_t gsp, esp_gsp_overlay_builder_t *builder, void *ctx)
{
    (void)gsp;
    (void)ctx;
    const app_ui_layout_t layout = app_ui_layout();

    if (layout == APP_UI_LAYOUT_KEYBOARD) {
        /* Highlight every pressed key, one per contact (up to two). */
        for (int i = 0; i < TOUCH_MAX_POINTS; ++i) {
            const int k = s_in.key_idx[i];
            if (k < 0) {
                continue;
            }
            (void)esp_gsp_overlay_builder_round_rect(builder, s_keys[k].x, s_keys[k].y,
                                                     (uint16_t)s_keys[k].w,
                                                     (uint16_t)s_keys[k].h,
                                                     s_key_color, 10, KEY_GLOW_OPACITY);
        }
        return;
    }

    if (layout != APP_UI_LAYOUT_BLANK) {
        return;
    }
    for (int i = 0; i < TOUCH_MAX_POINTS; ++i) {
        if (!s_in.points[i].active) {
            continue;
        }
        const int32_t x = s_in.points[i].x - CIRCLE_DIAMETER / 2;
        const int32_t y = s_in.points[i].y - CIRCLE_DIAMETER / 2;
        (void)esp_gsp_overlay_builder_round_rect(builder, x, y, CIRCLE_DIAMETER,
                                                 CIRCLE_DIAMETER, s_circle_color[i],
                                                 CIRCLE_DIAMETER / 2, CIRCLE_OPACITY);
    }
}

/* Submitting the contributor both registers the callback and asks for a
 * rebuild, so it doubles as the redraw request. */
static bool overlay_submit(void)
{
    if (s_overlay.gsp == NULL) {
        return false;
    }
    const esp_gsp_err_t err =
        esp_gsp_set_overlay_contributor(s_overlay.gsp, overlay_cb, NULL);
    if (err != ESP_GSP_OK) {
        if (s_overlay.registered) {
            s_overlay.registered = false;
            ESP_LOGW(TAG, "overlay unavailable (0x%x); retrying", err);
        }
        return false;
    }
    if (!s_overlay.registered) {
        s_overlay.registered = true;
        ESP_LOGI(TAG, "overlay attached");
    }
    return true;
}

void app_ui_input_attach_overlay(esp_gsp_handle_t gsp)
{
    s_overlay.gsp = gsp;
    s_overlay.last_ms = 0;
    (void)overlay_submit();
}

/* The BOOT key cycles the pages in the same order the UI exposes them. */
static void cycle_layout(void)
{
    static const app_ui_layout_t pages[] = {
        APP_UI_LAYOUT_BLANK, APP_UI_LAYOUT_MOUSE, APP_UI_LAYOUT_KEYBOARD,
        APP_UI_LAYOUT_GAMEPAD, APP_UI_LAYOUT_SENSOR, APP_UI_LAYOUT_TERMINAL,
        APP_UI_LAYOUT_SETTING,
    };
    const app_ui_layout_t current = app_ui_layout();
    size_t next = 0;
    for (size_t i = 0; i < sizeof(pages) / sizeof(pages[0]); ++i) {
        if (pages[i] == current) {
            next = (i + 1) % (sizeof(pages) / sizeof(pages[0]));
            break;
        }
    }
    app_ui_set_layout(pages[next]);
    ESP_LOGI(TAG, "boot key -> %s", app_ui_layout_name(pages[next]));
}

/* Settings page: four chip rows plus one [default] per section. Geometry must
 * match ui/main.json exactly, otherwise a tap lands on the neighbouring chip:
 * chips are 86 px wide on a 92 px pitch, starting at x = 12. */
#define CHIP_X0 12
#define CHIP_PITCH 92
#define CHIP_W 86
#define CHIP_H 38
#define CHIP_X(i) (CHIP_X0 + (int32_t)(i) * CHIP_PITCH)
static const struct {
    int32_t y;
    unsigned chips;
} s_chip_rows[4] = {
    {84, 5},  /* usb: kbd mouse gamepad sensor audio */
    {128, 3}, /* usb emulation row: ds4 ds5 xbox */
    {218, 5}, /* classic Bluetooth row */
    {308, 5}, /* BLE row */
};
/* [default] buttons sit on the section title line, right after the section
 * name (scene coords: x 50, w 96, h 26). */
#define DEFAULT_X 50
#define DEFAULT_W 96
#define DEFAULT_H 30
static const struct {
    int32_t y;
} s_default_rows[3] = { {50}, {184}, {274} };

/* The Sony/Xbox identities replace the generic gamepad and the motion
 * interface in the USB descriptor, so they exclude each other. Generic input
 * (gamepad, sensor) cancels an emulation identity instead, and the two generic
 * chips coexist. */
#define EMU_CLASSES (APP_CLASS_DS4 | APP_CLASS_DS5 | APP_CLASS_XBOX)

static uint32_t toggle_generic(uint32_t mask, uint32_t bit)
{
    mask &= ~EMU_CLASSES; /* generic input cancels the emulation identity */
    return (mask & bit) != 0 ? (mask & ~bit) : (mask | bit);
}

static void settings_toggle(unsigned row, unsigned index)
{
    if (row == 0) {
        uint32_t mask = app_cfg_classes();
        switch (index) {
        case 0:
            mask ^= APP_CLASS_KEYBOARD;
            break;
        case 1:
            mask ^= APP_CLASS_MOUSE;
            break;
        case 2:
            mask = toggle_generic(mask, APP_CLASS_GAMEPAD);
            break;
        case 3:
            mask = toggle_generic(mask, APP_CLASS_SENSOR);
            break;
        case 4:
            (void)app_cfg_set_usb_audio(!app_cfg_usb_audio());
            app_ui_refresh_chips();
            ESP_LOGI(TAG, "settings: usb audio -> %d (reboot to apply)",
                     (int)app_cfg_usb_audio());
            return;
        default:
            return;
        }
        (void)app_cfg_set_usb_hid(mask);
    } else if (row == 3) {
        uint32_t mask = app_cfg_ble_hid();
        if (index == 0) mask ^= APP_CLASS_KEYBOARD;
        else if (index == 1) mask ^= APP_CLASS_MOUSE;
        else if (index == 2) mask ^= APP_CLASS_GAMEPAD;
        else if (index == 3) mask ^= APP_CLASS_SENSOR;
        else return;
        (void)app_cfg_set_ble_hid(mask);
        app_ui_refresh_chips();
        ESP_LOGI(TAG, "settings: ble=0x%X (reboot to apply)", (unsigned)app_cfg_ble_hid());
        return;
    } else if (row == 1) {
        uint32_t mask = app_cfg_classes();
        const uint32_t bit = index == 0 ? APP_CLASS_DS4
                                      : (index == 1 ? APP_CLASS_DS5 : APP_CLASS_XBOX);
        /* Tapping the active identity turns it off; any other identity replaces
         * it and clears the generic gamepad and motion interface. */
        mask = (mask & EMU_CLASSES) == bit
                   ? (mask & ~EMU_CLASSES)
                   : ((mask & ~(EMU_CLASSES | APP_CLASS_GAMEPAD | APP_CLASS_SENSOR)) | bit);
        (void)app_cfg_set_usb_hid(mask);
        app_ui_refresh_chips();
        ESP_LOGI(TAG, "settings: mode -> 0x%X (reboot to apply)", (unsigned)app_cfg_classes());
        return;
    } else {
        uint32_t mask = app_cfg_bt_hid();
        switch (index) {
        case 0:
            mask ^= APP_CLASS_KEYBOARD;
            break;
        case 1:
            mask ^= APP_CLASS_MOUSE;
            break;
        case 2:
            mask ^= APP_CLASS_GAMEPAD;
            break;
        case 3:
            mask ^= APP_CLASS_SENSOR;
            break;
        case 4:
            (void)app_cfg_set_bt_audio(app_cfg_bt_audio() != 0 ? 0 : APP_BT_AUDIO_ALL);
            app_ui_refresh_chips();
            ESP_LOGI(TAG, "settings: bt audio -> 0x%X (reboot to apply)",
                     (unsigned)app_cfg_bt_audio());
            return;
        default:
            return;
        }
        (void)app_cfg_set_bt_hid(mask);
    }
    app_ui_refresh_chips();
    ESP_LOGI(TAG, "settings: usb=0x%X bt=0x%X (reboot to apply)",
             (unsigned)app_cfg_classes(), (unsigned)app_cfg_bt_hid());
}

static bool settings_hit(int32_t x, int32_t y)
{
    /* Every chip is tappable; the exclusivity rules live in settings_toggle so
     * there is no blocked/greyed look. Section defaults: usb / bt / ble. */
    for (unsigned row = 0; row < 3; ++row) {
        if (x >= DEFAULT_X && x < DEFAULT_X + DEFAULT_W && y >= s_default_rows[row].y &&
            y < s_default_rows[row].y + DEFAULT_H) {
            if (row == 0) {
                (void)app_cfg_set_usb_hid(APP_CLASS_USB_DEFAULT);
                (void)app_cfg_set_usb_audio(true);
            } else if (row == 1) {
                (void)app_cfg_set_bt_hid(APP_CLASS_ALL);
                (void)app_cfg_set_bt_audio(APP_BT_AUDIO_ALL);
            } else {
                (void)app_cfg_set_ble_hid(APP_CLASS_ALL);
            }
            app_ui_refresh_chips();
            ESP_LOGI(TAG, "settings: section %u reset to defaults", row);
            return true;
        }
    }
    for (unsigned row = 0; row < 4; ++row) {
        for (unsigned i = 0; i < s_chip_rows[row].chips; ++i) {
            if (x >= CHIP_X(i) && x < CHIP_X(i) + CHIP_W &&
                y >= s_chip_rows[row].y && y < s_chip_rows[row].y + CHIP_H) {
                settings_toggle(row, i);
                return true;
            }
        }
    }
    return false;
}

/* One tap per press: the chip is toggled on the initial contact. */
static void settings_update(void)
{
    if (s_in.points[0].active && !s_chip_touch) {
        s_chip_touch = settings_hit(s_in.points[0].x, s_in.points[0].y);
    } else if (!s_in.points[0].active) {
        s_chip_touch = false;
    }
}

static int key_at(int32_t x, int32_t y)
{
    for (size_t i = 0; i < KEY_COUNT; ++i) {
        if (x >= s_keys[i].x && x < s_keys[i].x + s_keys[i].w &&
            y >= s_keys[i].y && y < s_keys[i].y + s_keys[i].h) {
            return (int)i;
        }
    }
    return -1;
}

static void keyboard_update(void)
{
    uint8_t mods = 0;
    uint8_t keys[APP_HID_KEYBOARD_KEYS] = {0};
    int pressed[TOUCH_MAX_POINTS] = {-1, -1};
    size_t used = 0;
    for (int c = 0; c < TOUCH_MAX_POINTS; ++c) {
        if (!s_in.points[c].active) {
            continue;
        }
        const int k = key_at(s_in.points[c].x, s_in.points[c].y);
        if (k < 0) {
            continue;
        }
        pressed[c] = k;
        if (s_keys[k].modifier != 0) {
            mods |= s_keys[k].modifier;
            continue;
        }
        bool duplicate = false;
        for (size_t i = 0; i < used; ++i) {
            if (keys[i] == s_keys[k].code) duplicate = true;
        }
        if (!duplicate && used < APP_HID_KEYBOARD_KEYS) {
            keys[used++] = s_keys[k].code;
        }
    }

    /* Highlight state feeds the overlay; a change forces a redraw. */
    if (pressed[0] != s_in.key_idx[0] || pressed[1] != s_in.key_idx[1]) {
        s_in.key_idx[0] = pressed[0];
        s_in.key_idx[1] = pressed[1];
        s_in.kbd_dirty = true;
    }

    bool changed = mods != s_in.kbd_mods;
    for (size_t i = 0; i < APP_HID_KEYBOARD_KEYS; ++i) {
        if (keys[i] != s_in.kbd_keys[i]) changed = true;
    }
    if (changed) {
        ESP_LOGI(TAG, "kbd mods=0x%02X key0=0x%02X", mods, keys[0]);
        s_in.kbd_mods = mods;
        memcpy(s_in.kbd_keys, keys, sizeof(keys));
        app_usb_hid_keyboard(mods, keys);
    }
}

static int32_t clamp_i32(int32_t v, int32_t lo, int32_t hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

/* Map a contact position inside a stick circle to an axis value and a knob
 * offset. Radius 60 px maps onto 0..255 with 128 at the centre. */
static void stick_axis(int32_t x, int32_t y, int32_t cx, int32_t cy,
                       uint8_t *out_x, uint8_t *out_y,
                       int32_t *knob_x, int32_t *knob_y)
{
    int32_t dx = clamp_i32(x - cx, -STICK_RADIUS, STICK_RADIUS);
    int32_t dy = clamp_i32(y - cy, -STICK_RADIUS, STICK_RADIUS);
    const int32_t mag = abs((int)dx) + abs((int)dy);
    if (mag > STICK_RADIUS) {
        dx = dx * STICK_RADIUS / mag;
        dy = dy * STICK_RADIUS / mag;
    }
    *out_x = (uint8_t)(128 + dx * 127 / STICK_RADIUS);
    *out_y = (uint8_t)(128 + dy * 127 / STICK_RADIUS);
    *knob_x = cx + dx - KNOB_SIZE / 2;
    *knob_y = cy + dy - KNOB_SIZE / 2;
}

static role_t role_for_position(int32_t x, int32_t y)
{
    if (y < 240) {
        return x < 240 ? ROLE_LSTICK : ROLE_RSTICK;
    }
    return x < 240 ? ROLE_DPAD : ROLE_ABXY;
}

/* Leaving the page with a finger down would otherwise latch the last button,
 * stick and trigger values forever: release them all. */
static void gamepad_release_all(void)
{
    s_in.button_mask = 0;
    s_in.dpad_mask = 0;
    s_in.side_mask = 0;
    s_in.trigger_l = 0;
    s_in.trigger_r = 0;
    s_in.role[0] = ROLE_NONE;
    s_in.role[1] = ROLE_NONE;
    for (size_t i = 0; i < SIDE_COUNT; ++i) {
        app_ui_set_side_glow(s_side[i].glow, false);
    }
    for (size_t b = 0; b < ABXY_COUNT; ++b) {
        app_ui_set_button_glow((unsigned)b, false);
    }
    for (size_t i = 0; i < DPAD_COUNT; ++i) {
        app_ui_set_dpad_glow(s_dpad[i].glow, false);
    }
    app_usb_hid_set_buttons(0);
    app_usb_hid_set_triggers(0, 0);
    app_usb_hid_set_sticks(128, 128, 128, 128);
    app_ui_set_stick_knob(true, STICK_L_CX - KNOB_SIZE / 2, STICK_L_CY - KNOB_SIZE / 2);
    app_ui_set_stick_knob(false, STICK_R_CX - KNOB_SIZE / 2, STICK_R_CY - KNOB_SIZE / 2);
}

static void gamepad_update(void)
{
    uint16_t mask = 0;
    uint8_t lx = 128, ly = 128, rx = 128, ry = 128;
    int32_t lkx = STICK_L_CX - KNOB_SIZE / 2, lky = STICK_L_CY - KNOB_SIZE / 2;
    int32_t rkx = STICK_R_CX - KNOB_SIZE / 2, rky = STICK_R_CY - KNOB_SIZE / 2;
    uint16_t dpad_seen = 0;

    /* A contact that landed on a stick-side control keeps driving it, even when
     * the finger then slides over the stick: the stick stays where it was. */
    uint16_t side = 0;
    uint8_t trigger_l = 0, trigger_r = 0;
    bool on_side[TOUCH_MAX_POINTS] = {false, false};
    for (int c = 0; c < TOUCH_MAX_POINTS; ++c) {
        const int8_t cap = s_in.capture[c];
        if (!s_in.points[c].active || cap == SIDE_NONE) {
            continue;
        }
        on_side[c] = true;
        const size_t i = (size_t)cap;
        side |= (uint16_t)(1u << i);
        if (s_side[i].trigger != 0) {
            const uint8_t value = side_analog(i, s_in.points[c].y);
            if (s_side[i].trigger == 1) {
                if (value > trigger_l) {
                    trigger_l = value;
                }
            } else if (value > trigger_r) {
                trigger_r = value;
            }
        } else {
            mask |= s_side[i].bit;
        }
    }

    for (int c = 0; c < TOUCH_MAX_POINTS; ++c) {
        if (!s_in.points[c].active || on_side[c]) {
            s_in.role[c] = ROLE_NONE;
            continue;
        }
        if (s_in.role[c] == ROLE_NONE) {
            s_in.role[c] = role_for_position(s_in.points[c].x, s_in.points[c].y);
        }
        const int32_t px = s_in.points[c].x;
        const int32_t py = s_in.points[c].y;
        switch (s_in.role[c]) {
        case ROLE_LSTICK:
            stick_axis(px, py, STICK_L_CX, STICK_L_CY, &lx, &ly, &lkx, &lky);
            break;
        case ROLE_RSTICK:
            stick_axis(px, py, STICK_R_CX, STICK_R_CY, &rx, &ry, &rkx, &rky);
            break;
        case ROLE_DPAD:
            for (size_t i = 0; i < DPAD_COUNT; ++i) {
                if (px >= s_dpad[i].x && px < s_dpad[i].x + s_dpad[i].w &&
                    py >= s_dpad[i].y && py < s_dpad[i].y + s_dpad[i].h) {
                    mask |= s_dpad[i].bit;
                    dpad_seen |= (uint16_t)(1u << i);
                }
            }
            break;
        case ROLE_ABXY:
            for (size_t i = 0; i < ABXY_COUNT; ++i) {
                if (px >= s_abxy[i].x && px < s_abxy[i].x + s_abxy[i].w &&
                    py >= s_abxy[i].y && py < s_abxy[i].y + s_abxy[i].h) {
                    mask |= s_abxy[i].bit;
                }
            }
            break;
        default:
            break;
        }
    }

    for (size_t i = 0; i < SIDE_COUNT; ++i) {
        const bool pressed = (side & (1u << i)) != 0;
        const bool was = (s_in.side_mask & (1u << i)) != 0;
        if (pressed != was) {
            app_ui_set_side_glow(s_side[i].glow, pressed);
        }
    }
    s_in.side_mask = side;
    if (trigger_l != s_in.trigger_l || trigger_r != s_in.trigger_r) {
        /* Only the press/release edges are logged; sliding would flood the
         * small log ring. */
        if ((trigger_l == 0) != (s_in.trigger_l == 0) ||
            (trigger_r == 0) != (s_in.trigger_r == 0)) {
            ESP_LOGI(TAG, "trigger L=%u R=%u", trigger_l, trigger_r);
        }
        s_in.trigger_l = trigger_l;
        s_in.trigger_r = trigger_r;
        app_usb_hid_set_triggers(trigger_l, trigger_r);
    }

    app_usb_hid_set_sticks(lx, ly, rx, ry);
    app_ui_set_stick_knob(true, lkx, lky);
    app_ui_set_stick_knob(false, rkx, rky);

    if (mask != s_in.button_mask) {
        for (size_t b = 0; b < ABXY_COUNT; ++b) {
            const bool pressed = (mask & s_abxy[b].bit) != 0;
            const bool was = (s_in.button_mask & s_abxy[b].bit) != 0;
            if (pressed != was) {
                app_ui_set_button_glow((unsigned)b, pressed);
            }
        }
        s_in.button_mask = mask;
        app_usb_hid_set_buttons(mask);
    }
    for (size_t i = 0; i < DPAD_COUNT; ++i) {
        const bool pressed = (dpad_seen & (1u << i)) != 0;
        const bool was = (s_in.dpad_mask & (1u << i)) != 0;
        if (pressed != was) {
            app_ui_set_dpad_glow(s_dpad[i].glow, pressed);
        }
    }
    s_in.dpad_mask = dpad_seen;
}

/* Mouse layout state machine:
 *  - one finger drag -> pointer movement
 *  - one finger tap  -> left click
 *  - two finger tap  -> right click
 *  - two finger drag -> vertical wheel + horizontal pan
 */
static struct {
    bool tracking;
    int32_t last_x, last_y;
    int32_t press_x, press_y;
    TickType_t press_tick;
    bool two_active;
    bool two_candidate;
    TickType_t two_tick;
    int32_t two_x, two_y;
    int32_t scroll_x, scroll_y;
    int32_t residual_x, residual_y;
    bool suppress_single;
    uint8_t pending_button;
    TickType_t release_tick;
} s_mouse;

#define CLICK_HOLD_MS 40
#define TWO_TAP_MAX_MS 300
#define TWO_TAP_MAX_DIST 12
#define SCROLL_STEP_PX 10

static void emit_click(uint8_t button)
{
    ESP_LOGI(TAG, "click %u", button);
    app_usb_hid_mouse(button, 0, 0, 0, 0);
    s_mouse.pending_button = button;
    s_mouse.release_tick = xTaskGetTickCount() + pdMS_TO_TICKS(CLICK_HOLD_MS);
}

static void mouse_update(void)
{
    const TickType_t now = xTaskGetTickCount();
    const bool first = s_in.points[0].active;
    const bool second = s_in.points[1].active;

    if (s_mouse.pending_button != 0 && (int32_t)(now - s_mouse.release_tick) >= 0) {
        app_usb_hid_mouse(0, 0, 0, 0, 0);
        s_mouse.pending_button = 0;
    }

    if (first && second) {
        const int32_t ax = (s_in.points[0].x + s_in.points[1].x) / 2;
        const int32_t ay = (s_in.points[0].y + s_in.points[1].y) / 2;
        if (!s_mouse.two_active) {
            s_mouse.two_active = true;
            s_mouse.two_candidate = true;
            s_mouse.two_tick = now;
            s_mouse.two_x = ax;
            s_mouse.two_y = ay;
            s_mouse.scroll_x = ax;
            s_mouse.scroll_y = ay;
            s_mouse.residual_x = s_mouse.residual_y = 0;
            s_mouse.tracking = false;
            return;
        }

        /* Any meaningful travel turns the gesture into a scroll. */
        if (abs((int)(ax - s_mouse.two_x)) + abs((int)(ay - s_mouse.two_y)) > TWO_TAP_MAX_DIST) {
            s_mouse.two_candidate = false;
        }

        s_mouse.residual_x += s_mouse.scroll_x - ax;
        s_mouse.residual_y += s_mouse.scroll_y - ay;
        s_mouse.scroll_x = ax;
        s_mouse.scroll_y = ay;

        int8_t wheel = 0;
        int8_t pan = 0;
        while (s_mouse.residual_y >= SCROLL_STEP_PX) {
            s_mouse.residual_y -= SCROLL_STEP_PX;
            wheel = (int8_t)(wheel < 127 ? wheel + 1 : wheel);
        }
        while (s_mouse.residual_y <= -SCROLL_STEP_PX) {
            s_mouse.residual_y += SCROLL_STEP_PX;
            wheel = (int8_t)(wheel > -127 ? wheel - 1 : wheel);
        }
        while (s_mouse.residual_x >= SCROLL_STEP_PX) {
            s_mouse.residual_x -= SCROLL_STEP_PX;
            pan = (int8_t)(pan < 127 ? pan + 1 : pan);
        }
        while (s_mouse.residual_x <= -SCROLL_STEP_PX) {
            s_mouse.residual_x += SCROLL_STEP_PX;
            pan = (int8_t)(pan > -127 ? pan - 1 : pan);
        }
        if (wheel != 0 || pan != 0) {
            /* HID wheel positive scrolls away from the user; dragging the two
             * fingers down is expected to move the page up instead. */
            app_usb_hid_mouse(0, 0, 0, (int8_t)-wheel, pan);
        }
        return;
    }

    /* Two-finger gesture just ended (one contact lifted). */
    if (s_mouse.two_active) {
        s_mouse.two_active = false;
        s_mouse.suppress_single = true;
        s_mouse.tracking = false;
        const uint32_t held = (uint32_t)((now - s_mouse.two_tick) * portTICK_PERIOD_MS);
        if (s_mouse.two_candidate && held < TWO_TAP_MAX_MS) {
            s_mouse.two_candidate = false;
            emit_click(2); /* right button */
        }
        s_mouse.two_candidate = false;
        return;
    }

    if (first && !s_mouse.suppress_single && !s_mouse.tracking) {
        s_mouse.tracking = true;
        s_mouse.press_tick = now;
        s_mouse.press_x = s_in.points[0].x;
        s_mouse.press_y = s_in.points[0].y;
        s_mouse.last_x = s_in.points[0].x;
        s_mouse.last_y = s_in.points[0].y;
        return;
    }

    if (first && !s_mouse.suppress_single) {
        const int32_t dx = s_in.points[0].x - s_mouse.last_x;
        const int32_t dy = s_in.points[0].y - s_mouse.last_y;
        if (dx != 0 || dy != 0) {
            app_usb_hid_mouse(0,
                          (int8_t)(dx > 127 ? 127 : (dx < -127 ? -127 : dx)),
                          (int8_t)(dy > 127 ? 127 : (dy < -127 ? -127 : dy)), 0, 0);
            s_mouse.last_x = s_in.points[0].x;
            s_mouse.last_y = s_in.points[0].y;
        }
        return;
    }

    if (!first) {
        s_mouse.suppress_single = false;
        if (s_mouse.tracking) {
            s_mouse.tracking = false;
            const uint32_t held = (uint32_t)((now - s_mouse.press_tick) * portTICK_PERIOD_MS);
            /* The contact is already inactive here, so use the last tracked
             * position instead of the zeroed sample. */
            const int32_t moved = abs((int)(s_mouse.last_x - s_mouse.press_x)) +
                                  abs((int)(s_mouse.last_y - s_mouse.press_y));
            if (held < TAP_MAX_MS && moved < TAP_MAX_DIST) {
                emit_click(1); /* left button */
            }
        }
    }
}

/* Overlay redraws are expensive on this panel: throttle circle motion, but
 * always honour a lift or a keyboard highlight change. */
static void overlay_tick(void)
{
    if (!s_overlay.registered) {
        /* Registration may have been rejected before the first frame committed;
         * keep the callback installed as soon as the render task accepts it. */
        (void)overlay_submit();
    }
    const app_ui_layout_t layout = app_ui_layout();
    const bool kbd_redraw = layout == APP_UI_LAYOUT_KEYBOARD && s_in.kbd_dirty;
    if (((s_overlay.changed || s_overlay.released) && layout == APP_UI_LAYOUT_BLANK) ||
        kbd_redraw) {
        const uint32_t now_ms = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
        if (s_overlay.released || kbd_redraw ||
            now_ms - s_overlay.last_ms >= OVERLAY_MIN_PERIOD_MS) {
            s_overlay.last_ms = now_ms;
            s_in.kbd_dirty = false;
            (void)overlay_submit();
        }
    }
    s_overlay.changed = false;
    s_overlay.released = false;
}

/* ---------------------------------------------------------- sensor page --- */

/* Layout of the sensor page controls in scene pixels (must match ui/main.json). */
#define SENSOR_CLICK_Y 272
#define SENSOR_CLICK_W 176
#define SENSOR_CLICK_H 64
#define SENSOR_CLICK_L_X 48
#define SENSOR_CLICK_R_X 256
#define SENSOR_TOGGLE_X 48
#define SENSOR_TOGGLE_Y 356
#define SENSOR_TOGGLE_W 384
#define SENSOR_TOGGLE_H 64

/* Values refresh about 10 Hz: faster only floods the GSP render queue. */
#define SENSOR_LINE_DIV 6
/* Gyro noise at rest is about +-5 (0.5 deg/s); below this the pointer must not
 * drift. */
#define SENSOR_DEADZONE 20
/* Pointer speed: rate is in 0.1 deg/s, px/s = rate * 20 / 10, i.e. a wrist
 * turn of 100 deg/s moves about 2000 px/s. Remainder is kept in 1/1000 px. */
#define SENSOR_PX_NUM 20
#define SENSOR_PX_DEN 10

static bool s_sensor_mouse;
static bool s_sensor_btn_shown;
/* Mouse buttons held on this page, one bit each (bit0 left, bit1 right), plus
 * which button each contact grabbed so the right one releases. */
static uint8_t s_sensor_click;
static int8_t s_sensor_grab[TOUCH_MAX_POINTS] = {-1, -1};
/* GSP setters queue to the render task and can time out while it is busy (seen
 * as 0x107 right after a page switch), so repaint the button once more on the
 * next tick instead of leaving a stale highlight. */
static bool s_sensor_visual_dirty;
static unsigned s_sensor_div;
static int32_t s_sensor_rem_x;
static int32_t s_sensor_rem_y;

static bool sensor_mouse_available(void)
{
    const uint32_t masks = app_cfg_classes() | app_cfg_bt_hid() | app_cfg_ble_hid();
    return (masks & APP_CLASS_MOUSE) != 0;
}

/* Turn one gyro axis into whole pointer pixels, keeping the sub-pixel rest. */
static int32_t sensor_axis_to_pixels(int32_t rate, int32_t *remainder)
{
    if (rate > -SENSOR_DEADZONE && rate < SENSOR_DEADZONE) {
        return 0;
    }
    *remainder += rate * INPUT_PERIOD_MS * SENSOR_PX_NUM / SENSOR_PX_DEN;
    int32_t px = *remainder / 1000;
    *remainder -= px * 1000;
    if (px > 127) {
        px = 127; /* the report field is a signed byte */
    } else if (px < -127) {
        px = -127;
    }
    return px;
}

static void sensor_set_mouse(bool on)
{
    if (s_sensor_mouse == on) {
        return;
    }
    s_sensor_mouse = on;
    s_sensor_rem_x = 0;
    s_sensor_rem_y = 0;
    s_sensor_visual_dirty = true;
    app_ui_set_sensor_toggle(on);
    ESP_LOGI(TAG, "gyro mouse %s", on ? "on" : "off");
}

static void sensor_update(void)
{
    app_sensor_sample_t sample;
    if (s_sensor_visual_dirty) {
        s_sensor_visual_dirty = false;
        app_ui_set_sensor_button_visible(s_sensor_btn_shown);
        app_ui_set_sensor_toggle(s_sensor_mouse);
    }
    if (!app_sensor_latest(&sample)) {
        return;
    }
    if (++s_sensor_div >= SENSOR_LINE_DIV) {
        s_sensor_div = 0;
        char line[48];
        snprintf(line, sizeof(line), "gyro  %+5d %+5d %+5d",
                 (int)sample.gyro[0], (int)sample.gyro[1], (int)sample.gyro[2]);
        app_ui_set_sensor_line(0, line);
        snprintf(line, sizeof(line), "accl  %+5d %+5d %+5d",
                 (int)sample.accel[0], (int)sample.accel[1], (int)sample.accel[2]);
        app_ui_set_sensor_line(1, line);
        if (sample.mag_valid) {
            snprintf(line, sizeof(line), "mag   %+5d %+5d %+5d",
                     (int)sample.mag[0], (int)sample.mag[1], (int)sample.mag[2]);
        } else {
            snprintf(line, sizeof(line), "mag     n/a");
        }
        app_ui_set_sensor_line(2, line);
    }

    if (!s_sensor_mouse) {
        return;
    }
    /* yaw (Z) drives the horizontal axis, pitch (X) the vertical one. Both are
     * negated: on the bench a positive rate moves the cursor the other way. */
    const int32_t dx = sensor_axis_to_pixels(-sample.gyro[2], &s_sensor_rem_x);
    const int32_t dy = sensor_axis_to_pixels(-sample.gyro[0], &s_sensor_rem_y);
    if (dx != 0 || dy != 0) {
        app_usb_hid_mouse(s_sensor_click, (int8_t)dx, (int8_t)dy, 0, 0);
    }
}

/* Which mouse button a point lands on: 0 left, 1 right, -1 none. */
static int sensor_click_at(int32_t x, int32_t y)
{
    if (y < SENSOR_CLICK_Y || y >= SENSOR_CLICK_Y + SENSOR_CLICK_H) {
        return -1;
    }
    if (x >= SENSOR_CLICK_L_X && x < SENSOR_CLICK_L_X + SENSOR_CLICK_W) {
        return 0;
    }
    if (x >= SENSOR_CLICK_R_X && x < SENSOR_CLICK_R_X + SENSOR_CLICK_W) {
        return 1;
    }
    return -1;
}

static void sensor_publish_click(void)
{
    app_usb_hid_mouse(s_sensor_click, 0, 0, 0, 0);
}

/* Press/release one mouse button; two contacts may hold both at once. */
static void sensor_click_hold(int contact, int button, bool down)
{
    if (down) {
        s_sensor_grab[contact] = (int8_t)button;
        s_sensor_click |= (uint8_t)(1u << button);
        app_ui_set_sensor_click((unsigned)button, true);
    } else {
        const int8_t held = s_sensor_grab[contact];
        s_sensor_grab[contact] = -1;
        if (held >= 0) {
            s_sensor_click &= (uint8_t)~(1u << held);
            app_ui_set_sensor_click((unsigned)held, false);
        }
    }
    sensor_publish_click();
}

static void sensor_release_clicks(void)
{
    for (int i = 0; i < TOUCH_MAX_POINTS; ++i) {
        if (s_sensor_grab[i] >= 0) {
            sensor_click_hold(i, s_sensor_grab[i], false);
        }
    }
}

/* Tap on the gyro-mouse bar toggles the mode. */
static void sensor_tap(int32_t x, int32_t y)
{
    if (!s_sensor_btn_shown || !sensor_mouse_available()) {
        return;
    }
    if (x < SENSOR_TOGGLE_X || x >= SENSOR_TOGGLE_X + SENSOR_TOGGLE_W ||
        y < SENSOR_TOGGLE_Y || y >= SENSOR_TOGGLE_Y + SENSOR_TOGGLE_H) {
        return;
    }
    sensor_set_mouse(!s_sensor_mouse);
}

static void on_input_event(const app_input_event_t *event)
{
    if (event->type >= APP_INPUT_KEY_DOWN) {
        if (event->type == APP_INPUT_KEY_DOWN) {
            cycle_layout();
        }
        return;
    }
    const int id = event->id < TOUCH_MAX_POINTS ? event->id : 0;
    s_in.points[id] = (point_t){.x = event->x, .y = event->y,
                                .active = event->type != APP_INPUT_TOUCH_UP};
    s_overlay.changed = true;
    if (event->type == APP_INPUT_TOUCH_DOWN) {
        if (app_ui_layout() == APP_UI_LAYOUT_SENSOR) {
            const int button = sensor_click_at(event->x, event->y);
            if (button >= 0) {
                sensor_click_hold(id, button, true);
            } else if (id == 0) {
                sensor_tap(event->x, event->y);
            }
        }
        /* Latch the control this contact landed on; a later slide must not hand
         * the contact over to the stick underneath. */
        s_in.capture[id] = side_at(event->x, event->y);
    } else if (event->type == APP_INPUT_TOUCH_UP) {
        if (app_ui_layout() == APP_UI_LAYOUT_SENSOR && s_sensor_grab[id] >= 0) {
            sensor_click_hold(id, s_sensor_grab[id], false);
        }
        s_in.capture[id] = SIDE_NONE;
        s_overlay.released = true;
    }
}

static void input_task(void *arg)
{
    (void)arg;
    app_input_event_t event;

    for (;;) {
        while (app_input_pop(&event)) {
            on_input_event(&event);
        }
        switch (app_ui_layout()) {
        case APP_UI_LAYOUT_GAMEPAD:
            gamepad_update();
            break;
        case APP_UI_LAYOUT_MOUSE:
            mouse_update();
            break;
        case APP_UI_LAYOUT_KEYBOARD:
            keyboard_update();
            break;
        case APP_UI_LAYOUT_SENSOR:
            sensor_update();
            break;
        case APP_UI_LAYOUT_SETTING:
            settings_update();
            break;
        default:
            break;
        }
        const app_ui_layout_t layout = app_ui_layout();
        if (layout != s_last_layout) {
            if (s_last_layout == APP_UI_LAYOUT_GAMEPAD) {
                gamepad_release_all();
            }
            if (s_last_layout == APP_UI_LAYOUT_SENSOR) {
                /* The switch and the mouse buttons belong to this page only. */
                sensor_release_clicks();
                sensor_set_mouse(false);
            }
            if (layout == APP_UI_LAYOUT_SENSOR) {
                s_sensor_btn_shown = sensor_mouse_available();
                s_sensor_visual_dirty = true;
                app_ui_set_sensor_button_visible(s_sensor_btn_shown);
                s_sensor_div = SENSOR_LINE_DIV;
                ESP_LOGI(TAG, "sensor page%s", s_sensor_btn_shown ? "" : " (no mouse class)");
            } else {
                s_sensor_btn_shown = false;
            }
            s_last_layout = layout;
        }
        overlay_tick();
        vTaskDelay(pdMS_TO_TICKS(INPUT_PERIOD_MS));
    }
}

esp_err_t app_ui_input_start(void)
{
    if (s_task_started) {
        return ESP_OK;
    }
    ESP_RETURN_ON_ERROR(app_ui_touch_start(), TAG, "input device");
    s_in.key_idx[0] = -1;
    s_in.key_idx[1] = -1;
    s_in.capture[0] = SIDE_NONE;
    s_in.capture[1] = SIDE_NONE;
    if (xTaskCreate(input_task, "app_input", INPUT_TASK_STACK, NULL, INPUT_TASK_PRIO,
                    NULL) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    s_task_started = true;
    ESP_LOGI(TAG, "input started (%u keys)", (unsigned)KEY_COUNT);
    return ESP_OK;
}
