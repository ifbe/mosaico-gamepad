// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "esp_display_present_config.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Initialize the CO5300 panel and fill the presenter's hardware description.
 *
 * The panel, its IO and the PSRAM draw buffers stay owned by the BSP for the
 * lifetime of the program; this module never deinitializes them. The caller
 * (app_ui_core) supplies the GSP-side policy: touch adapter, render alignment,
 * PSRAM task stack.
 *
 * Call once at boot, before GSP starts. Returns ESP_OK, or an initialization
 * error with *out_target untouched. */
esp_err_t app_ui_display_init(esp_display_present_target_config_t *out_target);

#ifdef __cplusplus
}
#endif
