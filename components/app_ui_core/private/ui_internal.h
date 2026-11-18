// SPDX-License-Identifier: Apache-2.0
#pragma once

/* Declarations shared by the files of app_ui_core only. */

#include "esp_err.h"
#include "esp_gsp.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Install the overlay contributor. Called on the render task before the first
 * frame, so registration cannot race the initial compose. */
void app_ui_input_attach_overlay(esp_gsp_handle_t gsp);

/** Start the device layer (touch + BOOT key) and the event consumer. */
esp_err_t app_ui_input_start(void);

#ifdef __cplusplus
}
#endif
