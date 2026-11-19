# app_sensor

Motion source: BMI270 accelerometer + gyroscope at 100 Hz, plus an optional
BMM150 magnetometer.

## Responsibilities

- Bring up the IMU through the BSP (`bsp_imu_init` / `bsp_imu_start`) at 100 Hz.
- Probe the magnetometer once; a board without one keeps accel/gyro and reports
  `mag_valid = false`.
- Publish the newest sample through a small locked slot with a monotonic
  sequence number, so the consumer never blocks.
- Log one status line every 3 s (`acc=… mg gyro=… 0.1dps mag=… 0.1uT`) as a
  liveness check.
- The task runs at priority 3, one 10 ms period, and never allocates.

## Sample format (`app_sensor_sample_t`)

| Field | Unit | Notes |
| --- | --- | --- |
| `accel[3]` | mg | 1000 ≈ 1 g |
| `gyro[3]` | 0.1 dps | 10 = 1 °/s |
| `mag[3]` | 0.1 µT | zero when no magnetometer |
| `mag_valid` | – | true once a magnetometer read succeeded |

## Public API

| Function | Contract |
| --- | --- |
| `app_sensor_start()` | Initialise the buses and start the task. Returns the IMU error when the board has none; callers treat that as non-fatal. |
| `app_sensor_read(&sample)` | Copy the newest sample; returns false when nothing new arrived since the previous call. Any task. |

## Consumers

- `app_usb_hid` reads it every second 8 ms tick (~62 Hz) and sends report ID 4
  over USB, and through the Bluetooth sink when a motion report is enabled.
- The USB descriptor is the standard HID Sensors shape; see
  [app_usb.md](app_usb.md).

## Dependencies

`esp-mosaico-bsp` (I2C + BMI270/BMM150 drivers), `esp_driver_gpio`, `log`.
