// SPDX-License-Identifier: Apache-2.0

#include "app_ui_display.h"

#include "bsp/esp_mosaico.h"
#include "esp_check.h"
#include "esp_log.h"
#include "sdkconfig.h"

/* Device layer: bring up the CO5300 QSPI panel and describe it to the
 * presenter. Touch, GSP and every application concern belong elsewhere. */

static const char *TAG = "app_display";

esp_err_t app_ui_display_init(esp_display_present_target_config_t *out_target)
{
    ESP_RETURN_ON_FALSE(out_target != NULL, ESP_ERR_INVALID_ARG, TAG,
                        "present target is null");

    const bsp_display_config_t config = BSP_DISPLAY_DEFAULT_CONFIG();
    esp_lcd_panel_handle_t panel = NULL;
    ESP_RETURN_ON_ERROR(bsp_display_new(&config, &panel), TAG, "initialize CO5300");
    ESP_RETURN_ON_FALSE(panel != NULL && bsp_display_get_panel_io() != NULL,
                        ESP_ERR_INVALID_STATE, TAG, "panel/io missing");

    *out_target = (esp_display_present_target_config_t) {
        .hw = {
            .panel = panel,
            .io = bsp_display_get_panel_io(),
            .panel_type = ESP_DISPLAY_PRESENT_PANEL_IO,
            .input_pixel_format = ESP_DISPLAY_PRESENT_PIXEL_FORMAT_RGB565,
            .rotation = ESP_DISPLAY_PRESENT_ROTATE_0,
            .swap_bytes = true,
#if CONFIG_BSP_CO5300_ENABLE_TE
            .te_enabled = true,
            .te_sync = {
                .gpio_num = BSP_LCD_TE,
                .bus_freq_hz = BSP_LCD_PIXEL_CLOCK_HZ,
                .data_lines = BSP_LCD_DATA_WIDTH,
            },
#endif
        },
        .fb = {
            .mode = ESP_DISPLAY_PRESENT_MODE_AUTO,
        },
        /* Partition draw buffers go to PSRAM: the internal DMA pool has to stay
         * free for the Bluetooth controller and the SPI transaction queues. */
        .drawbuf = {
            .in_psram = true,
        },
    };
    const char *te_mode = "off";
    int te_gpio = -1;
#if CONFIG_BSP_CO5300_ENABLE_TE
    te_mode = "on";
    te_gpio = BSP_LCD_TE;
#endif
    ESP_LOGI(TAG, "present target %dx%d RGB565 TE=%s GPIO=%d",
             BSP_LCD_H_RES, BSP_LCD_V_RES, te_mode, te_gpio);
    return ESP_OK;
}
