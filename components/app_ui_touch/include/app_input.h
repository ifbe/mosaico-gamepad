// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Physical input events published by app_ui_touch. The UI layer owns every
 * semantic decision; this API only reports what the hardware did.
 *
 * One producer (the device task, 16 ms) and one consumer (app_ui_core, which
 * drains the queue every 16 ms). Depth 16 is enough for several samples per
 * consumer period; when full, the oldest entry is dropped and counted, so a
 * slow consumer loses motion but never the queue itself. */
typedef enum {
    APP_INPUT_TOUCH_DOWN = 0,
    APP_INPUT_TOUCH_MOVE,
    APP_INPUT_TOUCH_UP,
    APP_INPUT_KEY_DOWN,
    APP_INPUT_KEY_UP,
} app_input_event_type_t;

typedef struct {
    app_input_event_type_t type;
    uint8_t id;  /**< contact index 0..1, 0 for key events */
    int16_t x;   /**< contact coordinates (px), last known on TOUCH_UP; 0 for keys */
    int16_t y;
    uint32_t seq; /**< monotonically increasing publish counter */
} app_input_event_t;

/** Publish one event from the device task (producer side). */
void app_input_publish(const app_input_event_t *event);

/** Number of events dropped because the consumer fell behind (MOVE only). */
uint32_t app_input_dropped(void);

/** Pop the oldest event; false when the queue is empty. Non-blocking. */
bool app_input_pop(app_input_event_t *out);

#ifdef __cplusplus
}
#endif
