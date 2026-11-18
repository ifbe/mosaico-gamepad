// SPDX-License-Identifier: Apache-2.0

#include "app_sensor.h"

#include <string.h>

#include "bsp/imu.h"
#include "bsp/magnetometer.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "app_sensor";

#define SENSOR_PERIOD_MS 10 /* 100 Hz, matches the configured BMI270 ODR */
#define SENSOR_LOG_PERIOD_MS 3000 /* one status line every 3 s */
#define SENSOR_LOG_DIV (SENSOR_LOG_PERIOD_MS / SENSOR_PERIOD_MS)

static app_sensor_sample_t s_sample;
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static volatile uint32_t s_sequence; /* incremented per published sample */
static TaskHandle_t s_task;
static bool s_mag_present;

static int16_t clamp_i16(float v)
{
    if (v > 32767.0f) {
        return 32767;
    }
    if (v < -32768.0f) {
        return -32768;
    }
    return (int16_t)(v >= 0.0f ? v + 0.5f : v - 0.5f);
}

static void sensor_task(void *arg)
{
    (void)arg;
    TickType_t last = xTaskGetTickCount();
    unsigned log_div = 0;
    for (;;) {
        float ax = 0, ay = 0, az = 0;
        float gx = 0, gy = 0, gz = 0;
        struct bmm150_mag_data mag = {0};
        const bool mag_ok = s_mag_present &&
                            bsp_magnetometer_read(BSP_MAGNETOMETER_0, &mag) == ESP_OK;
        if (bsp_imu_get_accel(&ax, &ay, &az) == ESP_OK &&
            bsp_imu_get_gyro(&gx, &gy, &gz) == ESP_OK) {
            taskENTER_CRITICAL(&s_lock);
            /* g -> mg, dps -> 0.1 dps */
            s_sample.accel[0] = clamp_i16(ax * 1000.0f);
            s_sample.accel[1] = clamp_i16(ay * 1000.0f);
            s_sample.accel[2] = clamp_i16(az * 1000.0f);
            s_sample.gyro[0] = clamp_i16(gx * 10.0f);
            s_sample.gyro[1] = clamp_i16(gy * 10.0f);
            s_sample.gyro[2] = clamp_i16(gz * 10.0f);
            if (mag_ok) {
                /* uT -> 0.1 uT */
                s_sample.mag[0] = clamp_i16(mag.x * 10.0f);
                s_sample.mag[1] = clamp_i16(mag.y * 10.0f);
                s_sample.mag[2] = clamp_i16(mag.z * 10.0f);
                s_sample.mag_valid = true;
            }
            ++s_sequence;
            taskEXIT_CRITICAL(&s_lock);

            /* Periodic proof that the IMU actually delivers: accel in mg,
             * gyro in 0.1 dps, magnetometer in 0.1 uT (zero when absent). */
            if (++log_div >= SENSOR_LOG_DIV) {
                log_div = 0;
                ESP_LOGI(TAG, "acc=%d,%d,%d mg gyro=%d,%d,%d 0.1dps mag=%d,%d,%d 0.1uT%s",
                         (int)s_sample.accel[0], (int)s_sample.accel[1],
                         (int)s_sample.accel[2], (int)s_sample.gyro[0],
                         (int)s_sample.gyro[1], (int)s_sample.gyro[2],
                         (int)s_sample.mag[0], (int)s_sample.mag[1],
                         (int)s_sample.mag[2], s_sample.mag_valid ? "" : " (no mag)");
            }
        }
        /* Fixed cadence: vTaskDelayUntil keeps the sampling rate independent of
         * how long the I2C transfers take. */
        vTaskDelayUntil(&last, pdMS_TO_TICKS(SENSOR_PERIOD_MS));
    }
}

esp_err_t app_sensor_start(void)
{
    if (s_task != NULL) {
        return ESP_OK;
    }
    const esp_err_t ierr = bsp_imu_init();
    if (ierr != ESP_OK) {
        ESP_LOGW(TAG, "IMU not available: %s", esp_err_to_name(ierr));
        return ierr;
    }
    const bsp_imu_config_t config = BSP_IMU_CONFIG_DEFAULT();
    const esp_err_t serr = bsp_imu_start(&config);
    if (serr != ESP_OK) {
        ESP_LOGW(TAG, "IMU start failed: %s", esp_err_to_name(serr));
        return serr;
    }
    /* The magnetometer is optional: probe both wired addresses and keep
     * accel/gyro only when it is absent. */
    s_mag_present = bsp_magnetometer_init_all() == ESP_OK;
    if (!s_mag_present) {
        ESP_LOGW(TAG, "no magnetometer; mag fields stay zero");
    }
    if (xTaskCreate(sensor_task, "app_sensor", 3072, NULL, 3, &s_task) != pdPASS) {
        (void)bsp_imu_stop();
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "BMI270 sampling at %d Hz", 1000 / SENSOR_PERIOD_MS);
    return ESP_OK;
}

bool app_sensor_read(app_sensor_sample_t *out)
{
    static uint32_t seen;
    if (out == NULL) {
        return false;
    }
    taskENTER_CRITICAL(&s_lock);
    const uint32_t seq = s_sequence;
    const bool fresh = seq != seen;
    if (fresh) {
        *out = s_sample;
        seen = seq;
    }
    taskEXIT_CRITICAL(&s_lock);
    return fresh;
}
