# app_hid

Turns the device into a USB HID gamepad that shares the ESP-Iris USB device.

## Responsibilities

- Publishes a HID input report (16 buttons, absolute X/Y 0..1023, one tip
  switch) over the composite USB device created by the ESP-Iris USB transport.
- Maps the AI button (GPIO7, active low) to Button 1.
- Maps GSP pointer samples to the absolute touch coordinates. The pointer
  observer runs on the render task and only publishes state; the report task
  runs at 100 Hz.

## Dependencies

- `esp_tinyusb`: `tud_hid_report()`, `tud_mounted()`, `tud_hid_ready()`.
- `esp-gsp`: `esp_gsp_set_pointer_observer()`.
- ESP-Iris USB transport: defines the HID interface in its configuration
  descriptor when `CONFIG_TINYUSB_HID_COUNT > 0`.

## Contract with the dependency patch

The ESP-Iris USB transport reserves **58 bytes** for the report descriptor
(`ESP_IRIS_USB_HID_REPORT_DESC_LEN`) and exposes the interface as endpoint
`0x85`, 8-byte reports, 4 ms interval. `s_report_desc[]` must keep exactly that
length; `_Static_assert` fails the build otherwise. The host fetches the bytes
through `tud_hid_descriptor_report_cb()`.

## Minimal usage

```c
ESP_ERROR_CHECK(app_hid_start(ui_handle)); /* after esp_gsp_esp_lcd_start() */
```

Call once. The task lives until reboot; teardown is not supported because GSP
retains the UI until restart.

## Known gaps

- Touch is single-contact (GSP pointer observer); a second contact needs the
  pinch observer or a dedicated input source.
- No output reports: host-to-device rumble/LED data is discarded.
