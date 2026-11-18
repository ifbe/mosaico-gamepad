// SPDX-License-Identifier: Apache-2.0

#include "app_ui_touch.h"

#include "app_input.h"

#include "bsp/esp_mosaico.h"
#include "driver/gpio.h"

#include <stdbool.h>
#include <stdint.h>

#include "esp_check.h"
#include "esp_lcd_touch.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

/* Device layer: own the touch controller and the BOOT key, report what the
 * hardware did. No screen layout, no HID reports, no configuration: those are
 * the UI layer's decisions (see docs/ui.md). */

static const char *TAG = "app_touch";

#define TOUCH_MAX_POINTS 2
#define POLL_PERIOD_MS 16
#define TOUCH_TASK_STACK 3072
#define TOUCH_TASK_PRIO 6

typedef struct {
    int32_t x;
    int32_t y;
    bool active;
} point_t;

static esp_lcd_touch_handle_t s_touch;
static bool s_boot_key_ready;

static void publish(app_input_event_type_t type, uint8_t id, int16_t x, int16_t y)
{
    const app_input_event_t event = {.type = type, .id = id, .x = x, .y = y};
    app_input_publish(&event);
}

/* Publishes the edge samples. Coordinates of a released contact are the last
 * known ones so the UI can finish distance-based gestures. */
static void publish_contacts(const point_t next[TOUCH_MAX_POINTS],
                            const point_t prev[TOUCH_MAX_POINTS])
{
    for (int i = 0; i < TOUCH_MAX_POINTS; ++i) {
        const uint8_t id = (uint8_t)i;
        if (next[i].active && !prev[i].active) {
            publish(APP_INPUT_TOUCH_DOWN, id, (int16_t)next[i].x, (int16_t)next[i].y);
        } else if (!next[i].active && prev[i].active) {
            publish(APP_INPUT_TOUCH_UP, id, (int16_t)prev[i].x, (int16_t)prev[i].y);
        } else if (next[i].active &&
                   (next[i].x != prev[i].x || next[i].y != prev[i].y)) {
            publish(APP_INPUT_TOUCH_MOVE, id, (int16_t)next[i].x, (int16_t)next[i].y);
        }
    }
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
    s_boot_key_ready = gpio_config(&cfg) == ESP_OK;
    if (!s_boot_key_ready) {
        ESP_LOGW(TAG, "BOOT key unavailable");
    }
}

static bool boot_key_pressed(void)
{
    return gpio_get_level(BSP_BUTTON_BOOT_GPIO) == BSP_BUTTON_ACTIVE_LEVEL;
}

/* Two stable polls debounce the key; only edges are published. */
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
    publish(now == 1 ? APP_INPUT_KEY_DOWN : APP_INPUT_KEY_UP, 0, 0, 0);
}

static void read_contacts(point_t out[TOUCH_MAX_POINTS])
{
    esp_lcd_touch_point_data_t data[TOUCH_MAX_POINTS];
    uint8_t count = 0;
    if (esp_lcd_touch_read_data(s_touch) == ESP_OK &&
        esp_lcd_touch_get_data(s_touch, data, &count, TOUCH_MAX_POINTS) == ESP_OK) {
        if (count > TOUCH_MAX_POINTS) {
            count = TOUCH_MAX_POINTS;
        }
        for (uint8_t i = 0; i < count; ++i) {
            out[i] = (point_t){.x = data[i].x, .y = data[i].y, .active = true};
        }
    }
}

static void touch_task(void *arg)
{
    (void)arg;
    point_t prev[TOUCH_MAX_POINTS] = {{0}};

    for (;;) {
        point_t next[TOUCH_MAX_POINTS] = {{0}};
        read_contacts(next);
        publish_contacts(next, prev);
        for (int i = 0; i < TOUCH_MAX_POINTS; ++i) {
            prev[i] = next[i];
        }
        if (s_boot_key_ready) {
            boot_key_poll();
        }
        vTaskDelay(pdMS_TO_TICKS(POLL_PERIOD_MS));
    }
}

esp_err_t app_ui_touch_start(void)
{
    if (s_touch == NULL) {
        ESP_RETURN_ON_ERROR(bsp_touch_new(BSP_LCD_ROTATION_DEFAULT, &s_touch), TAG,
                            "touch controller");
    }
    boot_key_init();
    if (xTaskCreate(touch_task, "app_touch", TOUCH_TASK_STACK, NULL, TOUCH_TASK_PRIO,
                    NULL) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "input device started");
    return ESP_OK;
}
