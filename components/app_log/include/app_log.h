// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Install a vprintf hook that mirrors ESP_LOG output into a ring buffer. */
void app_log_init(void);

/** Number of retained lines (bounded). */
size_t app_log_count(void);

/** Line text by age: 0 is the newest retained line, or NULL when absent. */
const char *app_log_line(size_t newest_index);

#ifdef __cplusplus
}
#endif
