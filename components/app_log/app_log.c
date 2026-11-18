// SPDX-License-Identifier: Apache-2.0

#include "app_log.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"

#define LOG_LINES 48
#define LOG_LINE_CHARS 96

static char s_lines[LOG_LINES][LOG_LINE_CHARS];
static size_t s_count;   /* retained lines, <= LOG_LINES */
static size_t s_next;    /* slot for the next write */
static vprintf_like_t s_previous;
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;

static void push_line(const char *text)
{
    if (text == NULL || text[0] == '\0') {
        return;
    }
    taskENTER_CRITICAL(&s_lock);
    size_t n = 0;
    /* Keep the tail of the line: the newest characters matter most. */
    const size_t len = strlen(text);
    const char *src = text;
    if (len >= LOG_LINE_CHARS) {
        src += len - (LOG_LINE_CHARS - 1);
        n = LOG_LINE_CHARS - 1;
    } else {
        n = len;
    }
    memcpy(s_lines[s_next], src, n);
    s_lines[s_next][n] = '\0';
    s_next = (s_next + 1) % LOG_LINES;
    if (s_count < LOG_LINES) {
        ++s_count;
    }
    taskEXIT_CRITICAL(&s_lock);
}

static int log_vprintf(const char *fmt, va_list args)
{
    va_list copy;
    va_copy(copy, args);
    char buffer[LOG_LINE_CHARS];
    (void)vsnprintf(buffer, sizeof(buffer), fmt, copy);
    va_end(copy);

    /* GSP/panel errors repeat every frame when the display path is unhappy;
     * keeping them would push every other subsystem out of the log ring. */
    static const char *const dropped[] = {
        "TE schedule=",
        "lcd_panel.io.spi:",
        "co5300_spi:",
        "present_te_compose:",
        "gsp_frame_loop:",
        "gsp_esp: Display render failed",
    };
    for (size_t i = 0; i < sizeof(dropped) / sizeof(dropped[0]); ++i) {
        if (strstr(buffer, dropped[i]) != NULL) {
            return s_previous != NULL ? s_previous(fmt, args) : 0;
        }
    }

    /* Strip the trailing newline so a line occupies one display row. */
    char *newline = strchr(buffer, '\n');
    if (newline != NULL) {
        *newline = '\0';
    }
    push_line(buffer);

    return s_previous != NULL ? s_previous(fmt, args) : 0;
}

void app_log_init(void)
{
    s_previous = esp_log_set_vprintf(log_vprintf);
}

size_t app_log_count(void)
{
    return s_count;
}

const char *app_log_line(size_t newest_index)
{
    if (newest_index >= s_count) {
        return NULL;
    }
    taskENTER_CRITICAL(&s_lock);
    const size_t index = (s_next + LOG_LINES - 1 - newest_index) % LOG_LINES;
    taskEXIT_CRITICAL(&s_lock);
    return s_lines[index];
}
