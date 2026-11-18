# app_sensor

BMI270 motion source for the gamepad project.

## Responsibilities

- Bring the BMI270 up through the BSP (`bsp_imu_init` / `bsp_imu_start`).
- Sample accelerometer and gyroscope at 100 Hz in one task, independent of the
  HID report cadence.
- Publish the newest sample to whoever needs it (the USB sensor interface and,
  later, the Bluetooth report map).

It owns no USB/BLE resources: it only produces data. The HID transport decides
how to expose it.

## Dependencies

- `esp-mosaico-bsp` — `bsp/imu.h`, BMI270 driver
- FreeRTOS, `esp_log`

## API

```c
#include "app_sensor.h"

ESP_ERROR_CHECK(app_sensor_start());   /* non-fatal failure is fine */

app_sensor_sample_t sample;
if (app_sensor_read(&sample)) {
    /* sample.accel[3] in mg, sample.gyro[3] in 0.1 dps */
}
```

- `app_sensor_start()` is idempotent and safe to call once from `app_main`.
  It returns an error (and logs) when no IMU is fitted; the rest of the
  application keeps running.
- `app_sensor_read()` returns `true` only when the sample is newer than the
  previous successful read, so a slow consumer never re-sends the same data.

## Scaling

| Field | Unit | Range |
| --- | --- | --- |
| `accel[0..2]` | milligee (1 g = 1000) | ±4000 (±4 g) |
| `gyro[0..2]` | 0.1 deg/s (1 dps = 10) | ±20000 (±2000 dps) |

Both fit `int16_t` on purpose: the HID report carries them verbatim.

## Sampling

`SENSOR_PERIOD_MS = 10` (100 Hz) matches `BSP_IMU_CONFIG_DEFAULT()`
(`BMI2_*_ODR_100HZ`). `vTaskDelayUntil` keeps the rate steady even when the I2C
transfers take longer than usual.

## Shared state

One writer task and any number of readers. The sample and a sequence counter are
protected by a `portMUX` critical section: the copy is a handful of bytes, so a
spinlock is cheaper than a mutex here.
