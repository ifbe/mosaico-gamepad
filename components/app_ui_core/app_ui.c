// SPDX-License-Identifier: Apache-2.0

#include "app_ui.h"

#include "ui_internal.h"

#include "app_cfg.h"
#include "app_log.h"
#include "app_ui_display.h"
#include "esp_check.h"
#include "esp_gsp_esp_lcd.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <stddef.h>
#include <string.h>

#include "bundle_gsp.h"
#include "ui_bundle.h"

/* Marquee: one dot travelling around the bezel (dynamic GSP position). */
#define MARQUEE_DOT_SIZE 12
#define MARQUEE_STEP_PX 24
#define MARQUEE_TICK_MS 40
/* Terminal look follows the sibling ptt project: 15 px text, 22 px line pitch,
 * soft green on a near-black panel, 17 visible lines. */
#define TERMINAL_LINES 30

/* CO5300 QSPI requires both the X origin and width in multiples of 4 pixels.
 * GSP defaults to no expansion; the BSP rounder only covers LVGL. */
#define SCENE_ALIGN_PX 4

static const char *TAG = "app_ui";

static esp_gsp_err_t (*const s_term_set_text[TERMINAL_LINES])(esp_gsp_handle_t, const char *) = {
    gsp_gamepad_term_0_set_text,
    gsp_gamepad_term_1_set_text,
    gsp_gamepad_term_2_set_text,
    gsp_gamepad_term_3_set_text,
    gsp_gamepad_term_4_set_text,
    gsp_gamepad_term_5_set_text,
    gsp_gamepad_term_6_set_text,
    gsp_gamepad_term_7_set_text,
    gsp_gamepad_term_8_set_text,
    gsp_gamepad_term_9_set_text,
    gsp_gamepad_term_10_set_text,
    gsp_gamepad_term_11_set_text,
    gsp_gamepad_term_12_set_text,
    gsp_gamepad_term_13_set_text,
    gsp_gamepad_term_14_set_text,
    gsp_gamepad_term_15_set_text,
    gsp_gamepad_term_16_set_text,
    gsp_gamepad_term_17_set_text,
    gsp_gamepad_term_18_set_text,
    gsp_gamepad_term_19_set_text,
    gsp_gamepad_term_20_set_text,
    gsp_gamepad_term_21_set_text,
    gsp_gamepad_term_22_set_text,
    gsp_gamepad_term_23_set_text,
    gsp_gamepad_term_24_set_text,
    gsp_gamepad_term_25_set_text,
    gsp_gamepad_term_26_set_text,
    gsp_gamepad_term_27_set_text,
    gsp_gamepad_term_28_set_text,
    gsp_gamepad_term_29_set_text,
};

/* Chip label colours: the cyan fill needs dark text, an unselected chip keeps
 * the light label. There is no third "blocked" look: a chip is either selected
 * or not. */
#define CHIP_TEXT_ON 0x101010u   /* on the cyan fill */
#define CHIP_TEXT_OFF 0xE0E0E0u

#define CHIP_ROWS 4
#define CHIP_COLS 5

struct app_ui {
    esp_gsp_handle_t gsp;
    bool chip_on[CHIP_ROWS][CHIP_COLS];
    uint8_t setter_errors;   /* failed setters in the current refresh */
    uint8_t refresh_retries; /* bounded self-heal attempts */
    bool refresh_pending;
    bool repaint_pending;
    bool marquee_parked;
    void *timer;
    int32_t marquee_pos;   /* distance travelled along the perimeter, px */
    app_ui_layout_t layout;
    esp_gsp_err_t last_error;
};

static struct app_ui s_app;

#define SETTER_LOGS_MAX 3

/* GSP setters are queued to the render task and can be rejected while it is
 * busy. Silence would leave a stale screen, so the first few failures are
 * logged and the caller retries the whole refresh. */
static void remember(esp_gsp_err_t err)
{
    if (err == ESP_GSP_OK) {
        return;
    }
    s_app.last_error = err;
    ++s_app.setter_errors;
    if (s_app.setter_errors <= SETTER_LOGS_MAX) {
        ESP_LOGW(TAG, "GSP setter failed (0x%x)", err);
    }
}

static bool chip_valid(unsigned row, unsigned index)
{
    return row < CHIP_ROWS && index < (row == 1 ? 3u : CHIP_COLS);
}

/* One place decides the label colour, so the two refresh paths cannot fight
 * over it. */
static void apply_chip_color(unsigned row, unsigned index)
{
    static const gsp_component_key_t text_key[CHIP_ROWS][CHIP_COLS] = {
        {
            GSP_GAMEPAD_OBJ_KEY_CHIP_TEXT_USB_0, GSP_GAMEPAD_OBJ_KEY_CHIP_TEXT_USB_1,
            GSP_GAMEPAD_OBJ_KEY_CHIP_TEXT_USB_2, GSP_GAMEPAD_OBJ_KEY_CHIP_TEXT_USB_3,
            GSP_GAMEPAD_OBJ_KEY_CHIP_TEXT_USB_4,
        },
        {
            GSP_GAMEPAD_OBJ_KEY_CHIP_TEXT_MODE_0, GSP_GAMEPAD_OBJ_KEY_CHIP_TEXT_MODE_1,
            GSP_GAMEPAD_OBJ_KEY_CHIP_TEXT_MODE_2, GSP_GAMEPAD_OBJ_KEY_CHIP_TEXT_MODE_2,
            GSP_GAMEPAD_OBJ_KEY_CHIP_TEXT_MODE_2,
        },
        {
            GSP_GAMEPAD_OBJ_KEY_CHIP_TEXT_BT_0, GSP_GAMEPAD_OBJ_KEY_CHIP_TEXT_BT_1,
            GSP_GAMEPAD_OBJ_KEY_CHIP_TEXT_BT_2, GSP_GAMEPAD_OBJ_KEY_CHIP_TEXT_BT_3,
            GSP_GAMEPAD_OBJ_KEY_CHIP_TEXT_BT_4,
        },
        {
            GSP_GAMEPAD_OBJ_KEY_CHIP_TEXT_BLE_0, GSP_GAMEPAD_OBJ_KEY_CHIP_TEXT_BLE_1,
            GSP_GAMEPAD_OBJ_KEY_CHIP_TEXT_BLE_2, GSP_GAMEPAD_OBJ_KEY_CHIP_TEXT_BLE_3,
            GSP_GAMEPAD_OBJ_KEY_CHIP_TEXT_BLE_4,
        },
    };
    if (s_app.gsp == NULL || !chip_valid(row, index)) {
        return;
    }
    const uint32_t color = s_app.chip_on[row][index] ? CHIP_TEXT_ON : CHIP_TEXT_OFF;
    remember(esp_gsp_component_set_color_rgb888(s_app.gsp, text_key[row][index], color));
}

void app_ui_set_chip(unsigned row, unsigned index, bool on)
{
    if (s_app.gsp == NULL) {
        return;
    }
    static esp_gsp_err_t (*const usb[5])(esp_gsp_handle_t, bool) = {
        gsp_gamepad_chip_on_usb_0_set_visible, gsp_gamepad_chip_on_usb_1_set_visible,
        gsp_gamepad_chip_on_usb_2_set_visible, gsp_gamepad_chip_on_usb_3_set_visible,
        gsp_gamepad_chip_on_usb_4_set_visible,
    };
    static esp_gsp_err_t (*const mode[3])(esp_gsp_handle_t, bool) = {
        gsp_gamepad_chip_on_mode_0_set_visible, gsp_gamepad_chip_on_mode_1_set_visible,
        gsp_gamepad_chip_on_mode_2_set_visible,
    };
    static esp_gsp_err_t (*const bt[5])(esp_gsp_handle_t, bool) = {
        gsp_gamepad_chip_on_bt_0_set_visible, gsp_gamepad_chip_on_bt_1_set_visible,
        gsp_gamepad_chip_on_bt_2_set_visible, gsp_gamepad_chip_on_bt_3_set_visible,
        gsp_gamepad_chip_on_bt_4_set_visible,
    };
    static esp_gsp_err_t (*const ble[5])(esp_gsp_handle_t, bool) = {
        gsp_gamepad_chip_on_ble_0_set_visible, gsp_gamepad_chip_on_ble_1_set_visible,
        gsp_gamepad_chip_on_ble_2_set_visible, gsp_gamepad_chip_on_ble_3_set_visible,
        gsp_gamepad_chip_on_ble_4_set_visible,
    };
    if (!chip_valid(row, index)) {
        return;
    }
    s_app.chip_on[row][index] = on;
    remember((row == 0 ? usb : (row == 1 ? mode : (row == 2 ? bt : ble)))[index](
                 s_app.gsp, on));
    apply_chip_color(row, index);
}

void app_ui_refresh_chips(void)
{
    if (s_app.gsp == NULL) {
        return;
    }
    s_app.setter_errors = 0;
    const uint32_t usb = app_cfg_classes();
    const uint32_t bt = app_cfg_bt_hid();
    const uint32_t ble = app_cfg_ble_hid();
    const uint32_t bt_audio = app_cfg_bt_audio();

    app_ui_set_chip(0, 0, (usb & APP_CLASS_KEYBOARD) != 0);
    app_ui_set_chip(0, 1, (usb & APP_CLASS_MOUSE) != 0);
    app_ui_set_chip(0, 2, (usb & APP_CLASS_GAMEPAD) != 0);
    app_ui_set_chip(0, 3, (usb & APP_CLASS_SENSOR) != 0);
    app_ui_set_chip(0, 4, app_cfg_usb_audio());

    app_ui_set_chip(1, 0, (usb & APP_CLASS_DS4) != 0);
    app_ui_set_chip(1, 1, (usb & APP_CLASS_DS5) != 0);
    app_ui_set_chip(1, 2, (usb & APP_CLASS_XBOX) != 0);

    app_ui_set_chip(2, 0, (bt & APP_CLASS_KEYBOARD) != 0);
    app_ui_set_chip(2, 1, (bt & APP_CLASS_MOUSE) != 0);
    app_ui_set_chip(2, 2, (bt & APP_CLASS_GAMEPAD) != 0);
    app_ui_set_chip(2, 3, (bt & APP_CLASS_SENSOR) != 0);
    app_ui_set_chip(2, 4, bt_audio != 0);

    app_ui_set_chip(3, 0, (ble & APP_CLASS_KEYBOARD) != 0);
    app_ui_set_chip(3, 1, (ble & APP_CLASS_MOUSE) != 0);
    app_ui_set_chip(3, 2, (ble & APP_CLASS_GAMEPAD) != 0);
    app_ui_set_chip(3, 3, (ble & APP_CLASS_SENSOR) != 0);
    app_ui_set_chip(3, 4, false); /* reserved: LE Audio not implemented */

    if (s_app.setter_errors != 0 && s_app.refresh_retries < 3) {
        ++s_app.refresh_retries;
        s_app.refresh_pending = true;
    }
}

void app_ui_set_button_glow(unsigned button, bool on)
{
    static esp_gsp_err_t (*const setters[4])(esp_gsp_handle_t, bool) = {
        gsp_gamepad_glow_x_set_visible,
        gsp_gamepad_glow_a_set_visible,
        gsp_gamepad_glow_b_set_visible,
        gsp_gamepad_glow_y_set_visible,
    };
    if (button >= 4 || s_app.gsp == NULL) {
        return;
    }
    remember(setters[button](s_app.gsp, on));
}

void app_ui_set_side_glow(app_ui_side_t button, bool on)
{
    static esp_gsp_err_t (*const setters[APP_UI_SIDE_COUNT])(esp_gsp_handle_t, bool) = {
        gsp_gamepad_gp_glow_lb_set_visible,   gsp_gamepad_gp_glow_rb_set_visible,
        gsp_gamepad_gp_glow_ls_set_visible,   gsp_gamepad_gp_glow_rs_set_visible,
        gsp_gamepad_gp_glow_sel_set_visible,  gsp_gamepad_gp_glow_start_set_visible,
        gsp_gamepad_gp_glow_lt_set_visible,   gsp_gamepad_gp_glow_rt_set_visible,
    };
    if (button >= APP_UI_SIDE_COUNT || s_app.gsp == NULL) {
        return;
    }
    remember(setters[button](s_app.gsp, on));
}

void app_ui_set_dpad_glow(unsigned direction, bool on)
{
    static esp_gsp_err_t (*const setters[4])(esp_gsp_handle_t, bool) = {
        gsp_gamepad_glow_up_set_visible,
        gsp_gamepad_glow_down_set_visible,
        gsp_gamepad_glow_left_set_visible,
        gsp_gamepad_glow_right_set_visible,
    };
    if (direction >= 4 || s_app.gsp == NULL) {
        return;
    }
    remember(setters[direction](s_app.gsp, on));
}

void app_ui_set_stick_knob(bool left, int32_t x, int32_t y)
{
    if (s_app.gsp == NULL) {
        return;
    }
    remember(esp_gsp_component_set_position(
        s_app.gsp, left ? GSP_GAMEPAD_OBJ_KEY_STICK_L_KNOB : GSP_GAMEPAD_OBJ_KEY_STICK_R_KNOB,
        x, y));
}

void app_ui_set_terminal_line(unsigned line, const char *text)
{
    if (line >= TERMINAL_LINES || s_app.gsp == NULL || text == NULL) {
        return;
    }
    remember(s_term_set_text[line](s_app.gsp, text));
}

const char *app_ui_layout_name(app_ui_layout_t layout)
{
    switch (layout) {
    case APP_UI_LAYOUT_GAMEPAD: return "gamepad";
    case APP_UI_LAYOUT_MOUSE: return "mouse";
    case APP_UI_LAYOUT_KEYBOARD: return "keyboard";
    case APP_UI_LAYOUT_TERMINAL: return "terminal";
    case APP_UI_LAYOUT_SETTING: return "setting";
    default: return "blank";
    }
}

app_ui_layout_t app_ui_layout_from_name(const char *name)
{
    for (int i = 0; i < APP_UI_LAYOUT_COUNT; ++i) {
        if (strcmp(name, app_ui_layout_name((app_ui_layout_t)i)) == 0) {
            return (app_ui_layout_t)i;
        }
    }
    return APP_UI_LAYOUT_COUNT;
}

app_ui_layout_t app_ui_layout(void)
{
    return s_app.layout;
}

void app_ui_set_layout(app_ui_layout_t layout)
{
    if (s_app.gsp == NULL || layout >= APP_UI_LAYOUT_COUNT) {
        return;
    }
    s_app.layout = layout;
    remember(gsp_gamepad_layer_gamepad_set_visible(s_app.gsp, layout == APP_UI_LAYOUT_GAMEPAD));
    remember(gsp_gamepad_layer_mouse_set_visible(s_app.gsp, layout == APP_UI_LAYOUT_MOUSE));
    remember(gsp_gamepad_layer_keyboard_set_visible(s_app.gsp, layout == APP_UI_LAYOUT_KEYBOARD));
    remember(gsp_gamepad_layer_terminal_set_visible(s_app.gsp, layout == APP_UI_LAYOUT_TERMINAL));
    /* Link state labels belong to the idle screen only. */
    remember(gsp_gamepad_layer_setting_set_visible(s_app.gsp,
                                                  layout == APP_UI_LAYOUT_SETTING));
    if (layout == APP_UI_LAYOUT_TERMINAL) {
        s_app.repaint_pending = true;
    }
}

esp_gsp_handle_t app_ui_handle(void)
{
    return s_app.gsp;
}

static void place_marquee(int32_t pos)
{
    const int32_t span = 480 - MARQUEE_DOT_SIZE;
    const int32_t perimeter = span * 4;
    pos %= perimeter;
    int32_t x, y;
    if (pos < span) {
        x = 4 + pos;
        y = 4;
    } else if (pos < span * 2) {
        x = 4 + span;
        y = 4 + (pos - span);
    } else if (pos < span * 3) {
        x = 4 + span - (pos - span * 2);
        y = 4 + span;
    } else {
        x = 4;
        y = 4 + span - (pos - span * 3);
    }
    remember(esp_gsp_component_set_position(s_app.gsp, GSP_GAMEPAD_OBJ_KEY_MARQUEE_DOT, x, y));
}


/* Terminal layout mirrors the newest log lines; refreshed at ~4 Hz from the
 * shared UI timer instead of a dedicated task. */
#define TERMINAL_COLS 60
#define TERMINAL_TICK_DIV 6 /* 6 x 40 ms */

static void refresh_terminal(void)
{
    if (s_app.gsp == NULL || s_app.layout != APP_UI_LAYOUT_TERMINAL) {
        return;
    }
    char clipped[TERMINAL_COLS + 1];
    for (unsigned i = 0; i < TERMINAL_LINES; ++i) {
        const char *line = app_log_line(TERMINAL_LINES - 1 - i);
        if (line == NULL) {
            app_ui_set_terminal_line(i, "");
            continue;
        }
        const size_t len = strlen(line);
        if (len <= TERMINAL_COLS) {
            app_ui_set_terminal_line(i, line);
            continue;
        }
        memcpy(clipped, line, TERMINAL_COLS);
        clipped[TERMINAL_COLS] = '\0';
        app_ui_set_terminal_line(i, clipped);
    }
}

static void marquee_tick(esp_gsp_handle_t gsp, void *user_ctx)
{
    (void)gsp;
    (void)user_ctx;
    static unsigned tick;
    static uint32_t heap_log_ms;
    static bool blanked;
    if (s_app.refresh_pending) {
        s_app.refresh_pending = false;
        app_ui_refresh_chips();
    }
    if (++tick % TERMINAL_TICK_DIV == 0) {
        refresh_terminal();
    }
    /* Track the internal DMA pool; the TE compose path fails when it drains. */
    const uint32_t now_ms = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
    if (now_ms - heap_log_ms >= 10000) {
        heap_log_ms = now_ms;
        ESP_LOGI("heap", "int %u/%u dma %u/%u extdma %u/%u psram %u",
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA),
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_DMA),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM | MALLOC_CAP_DMA),
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    }

    /* The panel occasionally keeps stale rows between partial updates; a full
     * repaint is forced by hiding and re-showing the terminal background. */
    if (s_app.layout == APP_UI_LAYOUT_TERMINAL) {
        if (!blanked && s_app.repaint_pending) {
            s_app.repaint_pending = false;
            blanked = true;
            remember(gsp_gamepad_term_bg_set_visible(s_app.gsp, false));
        } else if (blanked) {
            blanked = false;
            remember(gsp_gamepad_term_bg_set_visible(s_app.gsp, true));
        }
    } else {
        blanked = false;
        s_app.repaint_pending = false;
    }
    /* The dot belongs to the idle screen; park it off-canvas while a layout is
     * in use so it costs nothing and stays out of the way. */
    if (s_app.layout != APP_UI_LAYOUT_BLANK) {
        /* Park once. Re-issuing the same position every tick forced a full
         * redraw at 25 Hz and kept the TE compose path saturated. */
        if (!s_app.marquee_parked) {
            s_app.marquee_parked = true;
            remember(esp_gsp_component_set_position(s_app.gsp,
                                                    GSP_GAMEPAD_OBJ_KEY_MARQUEE_DOT,
                                                    -32, -32));
        }
        return;
    }
    s_app.marquee_parked = false;
    s_app.marquee_pos = (s_app.marquee_pos + MARQUEE_STEP_PX) % ((480 - MARQUEE_DOT_SIZE) * 4);
    place_marquee(s_app.marquee_pos);
}

/* The layout RPC lives in app_cfg (configuration transport); the UI registers
 * these two hooks so app_cfg never depends on this component. */
static const char *layout_hook_name(void)
{
    return app_ui_layout_name(app_ui_layout());
}

static const char *layout_hook_apply(const char *name)
{
    const app_ui_layout_t layout = app_ui_layout_from_name(name);
    if (layout == APP_UI_LAYOUT_COUNT) {
        return NULL;
    }
    app_ui_set_layout(layout);
    return app_ui_layout_name(layout);
}

static const app_cfg_layout_rpc_t s_layout_rpc = {
    .name = layout_hook_name,
    .apply = layout_hook_apply,
};

/* Runs on the render task before the first frame: registrations and setters
 * here cannot race the initial compose. This is why the overlay contributor is
 * installed here instead of from app_main (it used to time out). */
static void ui_prepare(esp_gsp_handle_t gsp, void *ctx)
{
    (void)ctx;
    s_app.gsp = gsp;
    s_app.layout = APP_UI_LAYOUT_BLANK;
    s_app.marquee_pos = 0;
    place_marquee(0);
    app_ui_set_layout(APP_UI_LAYOUT_BLANK);
    /* The settings chips are pushed from the first timer tick: issuing ~40
     * setters before the first frame competes with the initial compose and a
     * rejected setter would leave a chip stuck. */
    s_app.refresh_pending = true;
    app_ui_input_attach_overlay(gsp);
    s_app.timer = esp_gsp_timer_create(gsp, MARQUEE_TICK_MS, marquee_tick, NULL);
    if (s_app.timer == NULL) {
        ESP_LOGE(TAG, "UI timer unavailable");
    }
}

esp_err_t app_ui_start(void)
{
    app_cfg_set_layout_rpc(&s_layout_rpc);

    esp_gsp_config_t config;
    ESP_RETURN_ON_ERROR(ui_bundle_open(&config), TAG, "UI bundle");

    esp_gsp_esp_lcd_config_t lcd = ESP_GSP_ESP_LCD_CONFIG_INIT();
    ESP_RETURN_ON_ERROR(app_ui_display_init(&lcd.display), TAG, "display");
    /* GSP must not poll the controller: app_ui_touch is the single reader. */
    lcd.touch = NULL;
    lcd.task_stack_psram = true; /* keep internal RAM for the BT controller */
    lcd.render_alignment = (esp_display_present_render_alignment_t){
        .x_pixels = SCENE_ALIGN_PX,
        .width_pixels = SCENE_ALIGN_PX,
    };

    s_app.last_error = ESP_GSP_OK;
    const esp_err_t err = esp_gsp_esp_lcd_start_prepared(&config, &lcd, ui_prepare, NULL,
                                                        &s_app.gsp);
    if (s_app.gsp == NULL || s_app.timer == NULL) {
        return err != ESP_OK ? err : ESP_ERR_INVALID_STATE;
    }
    if (err != ESP_OK) {
        /* The render task reported a first-frame failure; the scene is still
         * installed and later frames may succeed. */
        ESP_LOGW(TAG, "first frame failed (0x%x); continuing", err);
    }

    /* Input is the last layer: the scene exists, so events have somewhere to
     * land. A missing touch controller leaves the screens working. */
    const esp_err_t input_err = app_ui_input_start();
    if (input_err != ESP_OK) {
        ESP_LOGE(TAG, "input unavailable: %s", esp_err_to_name(input_err));
    }
    return ESP_OK;
}
