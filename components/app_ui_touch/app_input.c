// SPDX-License-Identifier: Apache-2.0

#include "app_input.h"

#include <string.h>

#include "freertos/FreeRTOS.h"

#define APP_INPUT_QUEUE_LEN 16

/* Single producer (device task) and single consumer (UI task). The queue is a
 * plain ring guarded by a spinlock: publishing must stay cheap because the
 * producer samples hardware every 16 ms. */
static app_input_event_t s_queue[APP_INPUT_QUEUE_LEN];
static size_t s_head; /* next write */
static size_t s_tail; /* next read */
static uint32_t s_seq;
static uint32_t s_dropped;
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;

void app_input_publish(const app_input_event_t *event)
{
    if (event == NULL) {
        return;
    }
    taskENTER_CRITICAL(&s_mux);
    const size_t used = (s_head + APP_INPUT_QUEUE_LEN - s_tail) % APP_INPUT_QUEUE_LEN;
    if (used == APP_INPUT_QUEUE_LEN - 1) {
        /* Only motion may be lost: drop the oldest entry. */
        s_tail = (s_tail + 1) % APP_INPUT_QUEUE_LEN;
        ++s_dropped;
    }
    s_queue[s_head] = *event;
    s_queue[s_head].seq = ++s_seq;
    s_head = (s_head + 1) % APP_INPUT_QUEUE_LEN;
    taskEXIT_CRITICAL(&s_mux);
}

uint32_t app_input_dropped(void)
{
    uint32_t dropped;
    taskENTER_CRITICAL(&s_mux);
    dropped = s_dropped;
    taskEXIT_CRITICAL(&s_mux);
    return dropped;
}

bool app_input_pop(app_input_event_t *out)
{
    bool got = false;
    if (out == NULL) {
        return false;
    }
    taskENTER_CRITICAL(&s_mux);
    if (s_tail != s_head) {
        *out = s_queue[s_tail];
        s_tail = (s_tail + 1) % APP_INPUT_QUEUE_LEN;
        got = true;
    }
    taskEXIT_CRITICAL(&s_mux);
    return got;
}
