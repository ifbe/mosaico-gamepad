// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** One motion sample, scaled for a 16-bit HID report.
 *
 * accel: milligee (1 g = 1000), +-4000 for the +-4 g range.
 * gyro:  0.1 deg/s (1 dps = 10), +-20000 for the +-2000 dps range.
 * mag:   0.1 uT (1 uT = 10), +-13000 for the +-1300 uT BMM150 range.
 */
typedef struct {
    int16_t accel[3]; /* x, y, z in mg */
    int16_t gyro[3];  /* x, y, z in 0.1 dps */
    int16_t mag[3];   /* x, y, z in 0.1 uT (zero when no magnetometer) */
    bool mag_valid;   /* false when the board has no BMM150 or it read nothing */
} app_sensor_sample_t;

/** Bring the BMI270 up and start the sampling task (100 Hz).
 *
 * Non-fatal by design: boards without an IMU keep running, app_sensor_read()
 * then simply reports no samples. Safe to call once from app_main. */
esp_err_t app_sensor_start(void);

/** Copy the newest sample.
 *
 * @param[out] out  Receives the latest sample; untouched on failure.
 * @return true when a sample newer than the previous call is available.
 */
bool app_sensor_read(app_sensor_sample_t *out);

#ifdef __cplusplus
}
#endif
