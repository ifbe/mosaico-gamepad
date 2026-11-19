# app_usb

The USB side of the device. ESP-Iris owns the composite device; the application
fills it with HID interfaces and a UAC2 audio function. This document describes
**what the host sees** for each interface.

## Interfaces

| Interface | USB class | Enabled by | Host sees |
| --- | --- | --- | --- |
| console | CDC-ACM | always (ESP-Iris) | serial console for logs/RPC |
| data link | CDC-ACM | ESP-Iris data transport | second serial port |
| gamepad | HID (report 1) | `usb_hid` bit 0 | a generic game controller |
| mouse | HID (report 2) | `usb_hid` bit 1 | a relative mouse |
| keyboard | HID (report 3) | `usb_hid` bit 2 | a 6-key-rollover keyboard |
| Sony | HID (report 1, 64 B) | `usb_hid` bit 3 (DS4) or 4 (DS5) | a DualShock 4 / DualSense |
| motion | HID (report 4) | `usb_hid` bit 5 | a HID sensor device (Motion) |
| audio | UAC2 | `usb_audio` | speakers + microphone |

Interfaces exist in the descriptor at boot; the mask decides which ones are
built, so changing it needs a reboot. The Sony identity and the motion interface
are mutually exclusive — both would need the same endpoint, and the Sony mask
clears gamepad and motion (`app_cfg` normalises this).

## HID gamepad

`report ID 1`, 8 bytes.

| Byte | Field | Range |
| --- | --- | --- |
| 0–1 | buttons, little endian | see below |
| 2 | left stick X | 0..255, 128 centre |
| 3 | left stick Y | 0..255, 128 centre |
| 4 | right stick X | 0..255, 128 centre |
| 5 | right stick Y | 0..255, 128 centre |
| 6 | LT | 0..255 |
| 7 | RT | 0..255 |

Button bits: 0 X, 1 A, 2 B, 3 Y, 4 up, 5 down, 6 left, 7 right, 8 LB, 9 RB,
10 LS, 11 RS, 12 SELECT, 13 START. The d-pad is reported as buttons, not as a
hat. The report is resent on change and at least every 250 ms.

## HID mouse

`report ID 2`, 5 bytes.

| Byte | Field |
| --- | --- |
| 0 | buttons (bit0 left, bit1 right, bit2 middle) |
| 1–2 | dx, dy (signed 8-bit) |
| 3 | wheel (signed) |
| 4 | AC pan (signed) |

## HID keyboard

`report ID 3`, 8 bytes.

| Byte | Field |
| --- | --- |
| 0 | modifiers (bit0 LCtrl … bit7 RGUI) |
| 1 | reserved, 0 |
| 2–7 | six simultaneous key codes, 0 when unused |

## HID motion

`report ID 4`, 18 bytes.

Standard HID Sensors usages (page `0x20`): one top-level Application collection
`Motion` containing `Accelerometer 3D`, `Gyrometer 3D` and `Compass 3D`, all
sharing report ID 4 so one report carries all nine axes:

| Bytes | Field | Unit |
| --- | --- | --- |
| 0–5 | acceleration X, Y, Z | mg (int16 LE) |
| 6–11 | angular velocity X, Y, Z | 0.1 °/s (int16 LE) |
| 12–17 | magnetic flux X, Y, Z | 0.1 µT (int16 LE) |

No Unit or property Feature reports are exported yet, so Windows' sensor class
driver may decline to bind the interface; it is still a valid HID device and any
host can read report ID 4 directly.

## HID Sony

`report ID 1`, 64 bytes.

A DS4-style report (`0x01`) used by both identities; DS5 only differs in where
the button byte and counter live.

| Byte | Field |
| --- | --- |
| 0 | `0x01` (report id, repeated in the payload by the DS4 convention) |
| 1–4 | left X/Y, right X/Y |
| 5 | buttons + 8-way hat (DS4); DS5 uses byte 8 |
| 7 (DS5) | counter |
| 10–11 | report counter, little endian |
| 13–18 | gyro X/Y/Z (≈1024 LSB per 90 °/s) |
| 19–24 | accel X/Y/Z (≈8192 LSB per g) |

Not filled: triggers, touchpad, battery and the calibration feature reports. The
motion values come from `app_sensor` and are scaled, not calibrated.

## UAC2 audio

| Direction | Format | Endpoint |
| --- | --- | --- |
| playback (host → device) | 2 ch, 48 kHz, 16 bit | OUT `0x06`, adaptive, no feedback endpoint |
| capture (device → host) | 1 ch, 48 kHz, 16 bit | IN, first free slot after the HID interfaces |

Entities: clock source → USB input terminal → feature unit (mute + volume,
`-15360..0` in 1/256 dB) → headphone output for playback; generic-mic terminal →
feature unit → USB streaming output for capture. Playback buffers 2.5 ms chunks
in an 8 KB ring; the host must keep the OUT FIFO drained or the stream stops, which
the ISR does. `CONFIG_FREERTOS_HZ=1000` is required for 1 ms audio timing.

### UAC1

Not implemented; the function is UAC2 only (`bcdADC 0x0200`, interface protocol
`0x20`). UAC1 differs in every descriptor: `bcdADC 0x0100`, protocol `0x00`,
sample rate as endpoint GET/SET_CUR, a 3-byte feedback packet on its own IN
endpoint and no explicit clock source. A descriptor written that way is parsed as
UAC1 and rejected with `badADCVersion` because `bcdADC` claims 2.0. UAC2 is
supported by every target host (macOS, Windows 10+, Linux, Android); adding UAC1
would mean a second descriptor set, not a tweak.

## Implementation notes

- Endpoint budget is full: console CDC `0x81/0x82`, four HID interfaces
  `0x83..0x86`, microphone `0x87` (the controller has eight IN endpoints with
  EP0). That ceiling drives the Sony/motion exclusivity.
- `tud_hid_descriptor_report_cb()` must return a descriptor for **every**
  registered instance; TinyUSB does not null-check it.
- Every Report ID must sit inside a top-level collection, or Windows rejects the
  whole interface.

## Public API

| Component | Function | Contract |
| --- | --- | --- |
| `app_usb_core` | `app_usb_core_first_in_endpoint()`, `app_usb_core_hid_interface_count(mask)`, `app_usb_core_mic_endpoint(first_ep, hid_count)` | Interface/endpoint arithmetic for the layout above. |
| `app_usb_hid` | `app_usb_hid_register_usb(first_ep_in)` / `app_usb_hid_start()` | Register interfaces with Iris, then start the 8 ms report task. |
| | `app_usb_hid_set_buttons()` / `_set_sticks()` / `_set_triggers()` / `_mouse()` / `_keyboard()` | Publish input state; any task. |
| | `app_usb_hid_attach_bt(connected, send)` | Attach the Bluetooth sink. |
| | `app_usb_hid_class_desc(i, &len)` / `app_usb_hid_sensor_desc(&len)` | Descriptors for the Bluetooth report maps. |
| `app_usb_audio` | `app_usb_audio_register(mic_ep_in)` / `app_usb_audio_start()` / `app_usb_audio_streaming()` | Register the UAC2 function, start its tasks, query streaming. |

Dependencies: `app_cfg`, `app_sensor`, `app_audio_codec`, `esp_iris`,
`esp_tinyusb`, `esp_driver_gpio`.
