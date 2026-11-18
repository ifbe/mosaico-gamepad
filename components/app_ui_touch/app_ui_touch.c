// SPDX-License-Identifier: Apache-2.0

#include "app_ui_touch.h"

#include "bsp/esp_mosaico.h"
#include "driver/gpio.h"

#include <stddef.h>
#include <string.h>

#include "app_cfg.h"
#include "app_usb_hid.h"
#include "app_ui.h"
#include "esp_check.h"
#include "esp_lcd_touch.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "app_touch";

#define TOUCH_MAX_POINTS 2
#define POLL_PERIOD_MS 16
#define CIRCLE_DIAMETER 64
#define CIRCLE_OPACITY 220
#define CORNER_SIZE 64
#define TAP_MAX_MS 250
#define OVERLAY_MIN_PERIOD_MS 50
#define TAP_MAX_DIST 10

#define RGB565(r, g, b) ((uint16_t)((((r) & 0xF8u) << 8) | (((g) & 0xFCu) << 3) | ((b) >> 3)))
static const uint16_t s_circle_color[TOUCH_MAX_POINTS] = {
    RGB565(0x00, 0xE5, 0xFF),
    RGB565(0xFF, 0x7A, 0x00),
};
/* Keyboard press highlight (drawn on the overlay, one per contact). */
static const uint16_t s_key_color = RGB565(0x00, 0xE5, 0xFF);
#define KEY_GLOW_OPACITY 110

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

static bool s_chip_touch;

static struct {
    esp_gsp_handle_t gsp;
    esp_lcd_touch_handle_t touch;
    volatile point_t points[TOUCH_MAX_POINTS];
    uint16_t button_mask;
    uint16_t dpad_mask;
    role_t role[TOUCH_MAX_POINTS];
    uint8_t kbd_mods;
    uint8_t kbd_keys[APP_HID_KEYBOARD_KEYS];
    int key_idx[TOUCH_MAX_POINTS]; /* pressed key per contact, -1 = none */
    bool kbd_dirty;                /* key highlight set changed */
} s_in;

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

static int corner_at(int32_t x, int32_t y)
{
    if (x < CORNER_SIZE && y < CORNER_SIZE) return 0;                       /* TL: mouse */
    if (x >= 480 - CORNER_SIZE && y < CORNER_SIZE) return 1;                /* TR: gamepad */
    if (x < CORNER_SIZE && y >= 480 - CORNER_SIZE) return 2;                /* BL: keyboard */
    if (x >= 480 - CORNER_SIZE && y >= 480 - CORNER_SIZE) return 3;         /* BR: terminal */
    return -1;
}


/* The BOOT key cycles the pages in the same order the UI exposes them; corner
 * taps remain a shortcut. Presses are debounced over two polls. */

/* Settings page: two rows of chips. Layout matches ui/main.json:
 * rows at y=120 (USB) and y=200 (Bluetooth), chips start at x=70. */
#define CHIP_X(i) (12 + (i) * 92)
#define CHIP_W 86
#define CHIP_H 38
static const struct {
    int32_t y;
} s_chip_rows[2] = {
    {116}, /* USB row in ui/main.json */
    {228}, /* Bluetooth row */
};

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
            mask ^= APP_CLASS_GAMEPAD;
            break;
        case 3:
            mask = (mask & (APP_CLASS_DS4 | APP_CLASS_DS5)) != 0
                       ? (mask & ~(APP_CLASS_DS4 | APP_CLASS_DS5))
                       : (mask | APP_CLASS_DS4);
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
    } else {
        if (index == 3) {
            return; /* no Sony slot over Bluetooth */
        }
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
    for (unsigned row = 0; row < 2; ++row) {
        for (unsigned i = 0; i < 5; ++i) {
            if (x >= CHIP_X(i) && x < CHIP_X(i) + CHIP_W &&
                y >= s_chip_rows[row].y && y < s_chip_rows[row].y + CHIP_H) {
                settings_toggle(row, i);
                return true;
            }
        }
    }
    return false;
}

static void boot_key_init(void)
{
    const gpio_config_t cfg = {
        .pin_bit_mask = 1ULL << BSP_BUTTON_BOOT_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = BSP_BUTTON_ACTIVE_LEVEL == 0 ? GPIO_PULLUP_ENABLE : GPIO_PULLDOWN_ENABLE,
        .pull_down_en = BSP_BUTTON_ACTIVE_LEVEL == 0 ? GPIO_PULLDOWN_DISABLE : GPIO_PULLUP_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    if (gpio_config(&cfg) != ESP_OK) {
        ESP_LOGW(TAG, "BOOT key unavailable");
    }
}

static bool boot_key_pressed(void)
{
    return gpio_get_level(BSP_BUTTON_BOOT_GPIO) == BSP_BUTTON_ACTIVE_LEVEL;
}

static void boot_key_poll(void)
{
    static int stable = -1;
    static int last = -1;
    const int now = boot_key_pressed() ? 1 : 0;
    if (now != last) {
        last = now;
        stable = -1;
        return;
    }
    if (stable == now) {
        return;
    }
    stable = now;
    if (now != 1) {
        return;
    }
    /* idle -> mouse -> keyboard -> gamepad -> console -> idle */
    static const app_ui_layout_t pages[] = {
        APP_UI_LAYOUT_BLANK, APP_UI_LAYOUT_MOUSE, APP_UI_LAYOUT_KEYBOARD,
        APP_UI_LAYOUT_GAMEPAD, APP_UI_LAYOUT_TERMINAL, APP_UI_LAYOUT_SETTING,
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

static void switch_corner_layout(int corner)
{
    static const app_ui_layout_t layouts[4] = {
        APP_UI_LAYOUT_MOUSE, APP_UI_LAYOUT_GAMEPAD,
        APP_UI_LAYOUT_KEYBOARD, APP_UI_LAYOUT_TERMINAL,
    };
    const app_ui_layout_t target = layouts[corner];
    app_ui_set_layout(app_ui_layout() == target ? APP_UI_LAYOUT_BLANK : target);
    ESP_LOGI(TAG, "layout -> %s", app_ui_layout_name(app_ui_layout()));
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

static void gamepad_update(void)
{
    uint16_t mask = 0;
    uint8_t lx = 128, ly = 128, rx = 128, ry = 128;
    int32_t lkx = STICK_L_CX - KNOB_SIZE / 2, lky = STICK_L_CY - KNOB_SIZE / 2;
    int32_t rkx = STICK_R_CX - KNOB_SIZE / 2, rky = STICK_R_CY - KNOB_SIZE / 2;
    uint16_t dpad_seen = 0;

    for (int c = 0; c < TOUCH_MAX_POINTS; ++c) {
        if (!s_in.points[c].active) {
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

static void touch_task(void *arg)
{
    (void)arg;
    esp_lcd_touch_point_data_t data[TOUCH_MAX_POINTS];
    bool corner_used = false;

    for (;;) {
        uint8_t count = 0;
        point_t next[TOUCH_MAX_POINTS] = {{0}};
        if (esp_lcd_touch_read_data(s_in.touch) == ESP_OK &&
            esp_lcd_touch_get_data(s_in.touch, data, &count, TOUCH_MAX_POINTS) == ESP_OK) {
            if (count > TOUCH_MAX_POINTS) count = TOUCH_MAX_POINTS;
            for (uint8_t i = 0; i < count; ++i) {
                next[i].x = data[i].x;
                next[i].y = data[i].y;
                next[i].active = true;
            }
        }

        bool changed = false;
        const bool was_active[TOUCH_MAX_POINTS] = {s_in.points[0].active, s_in.points[1].active};
        for (int i = 0; i < TOUCH_MAX_POINTS; ++i) {
            if (s_in.points[i].active != next[i].active ||
                s_in.points[i].x != next[i].x || s_in.points[i].y != next[i].y) {
                changed = true;
            }
            s_in.points[i] = next[i];
        }
        /* A lifted contact must clear its circle immediately; the throttle below
         * only limits redraws while fingers are moving. */
        const bool released = (was_active[0] && !next[0].active) ||
                              (was_active[1] && !next[1].active);

        /* A fresh contact in a corner switches layouts and is consumed. */
        if (next[0].active && !corner_used) {
            const int corner = corner_at(next[0].x, next[0].y);
            if (corner >= 0) {
                switch_corner_layout(corner);
                corner_used = true;
            }
        }
        if (!next[0].active) {
            corner_used = false;
        }

        boot_key_poll();
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
        case APP_UI_LAYOUT_SETTING:
            /* One tap per press: the chip is toggled on the initial contact. */
            if (next[0].active && !s_chip_touch) {
                s_chip_touch = settings_hit(next[0].x, next[0].y);
            } else if (!next[0].active) {
                s_chip_touch = false;
            }
            break;
        default:
            break;
        }

        /* Overlay redraws are expensive on this panel: throttle circle motion,
         * but always honour a lift or a keyboard highlight change. */
        const app_ui_layout_t layout = app_ui_layout();
        const bool kbd_redraw = layout == APP_UI_LAYOUT_KEYBOARD && s_in.kbd_dirty;
        if (((changed || released) && layout == APP_UI_LAYOUT_BLANK) || kbd_redraw) {
            static uint32_t last_overlay_ms;
            const uint32_t now_ms = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
            if (released || kbd_redraw || now_ms - last_overlay_ms >= OVERLAY_MIN_PERIOD_MS) {
                last_overlay_ms = now_ms;
                s_in.kbd_dirty = false;
                (void)esp_gsp_set_overlay_contributor(s_in.gsp, overlay_cb, NULL);
            }
        }
        vTaskDelay(pdMS_TO_TICKS(POLL_PERIOD_MS));
    }
}

esp_err_t app_ui_touch_start(esp_gsp_handle_t gsp, esp_lcd_touch_handle_t touch)
{
    ESP_RETURN_ON_FALSE(gsp != NULL && touch != NULL, ESP_ERR_INVALID_ARG, TAG,
                        "gsp/touch handle is null");
    boot_key_init();
    s_in.gsp = gsp;
    s_in.touch = touch;
    s_in.key_idx[0] = -1;
    s_in.key_idx[1] = -1;
    s_in.kbd_dirty = false;
    ESP_RETURN_ON_ERROR(esp_gsp_set_overlay_contributor(gsp, overlay_cb, NULL), TAG,
                        "overlay contributor");
    if (xTaskCreate(touch_task, "app_touch", 4096, NULL, 6, NULL) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "input started (%u keys)", (unsigned)KEY_COUNT);
    return ESP_OK;
}
