# app_ble

BLE (LE) side of the device. It exposes exactly one interface — a HID over GATT
(HOGP) device — and the reports inside it follow the `ble_hid` mask.

## Interface

| Interface | Spec | Enabled by | Host sees |
| --- | --- | --- | --- |
| HID over GATT | HID service `0x1812`, appearance `0x03C4` (HID gamepad) | `bt_mode` includes LE and `ble_hid` mask | a BLE game controller |

Everything else (controller, Bluedroid, device names) belongs to `app_bt_core`;
`app_ble_hid` only runs the HOGP device on top. There is **no Sony emulation and
no audio** over BLE.

## Reports

The report map is the one `app_bt_core` builds for the `ble_hid` mask, so the
report IDs and byte layouts match USB and Classic:

| Report ID | Class | Payload |
| --- | --- | --- |
| 1 | gamepad | 8 B: buttons, sticks, triggers |
| 2 | mouse | 5 B: buttons, dx, dy, wheel, pan |
| 3 | keyboard | 8 B: modifiers, 6 keys |
| 4 | motion | 18 B: accel, gyro, magnetometer |

Byte layouts are in [app_usb.md](app_usb.md#hid-gamepad).
Each report becomes a Report characteristic with its own Input Report
notification; report ID 4 additionally needs the padding the 20-byte default
MTU allows.

## Connection behaviour

- Advertising carries flags `0x06` (LE General Discoverable, **BR/EDR not
  supported**), the 16-bit HID service UUID `0x1812` and the device name, from the
  LE-only static random address. Claiming BR/EDR support makes Android connect
  over Classic instead of GATT, so the flag is deliberate.
- Advertising stops while a host is subscribed and restarts on disconnect
  (`app_ble_hid_restart_advertising()`, also RPC method 8).
- Pairing is Secure Connections + bonding, Just Works; a bonded host reconnects
  without a passkey. `app_ble_hid_clear_bonds()` (RPC method 7) forgets peers.
- The phone negotiates a **30 ms** connection interval here. Input reports are
  event-driven; motion notifications are paced to one per interval and the rest
  are dropped rather than queued, because at 18 bytes only one fits anyway.

## Public API

| Function | Contract |
| --- | --- |
| `app_ble_hid_start(map, len)` | Start the HOGP device and advertising. Requires `app_bt_core_start()` with LE enabled; the map must outlive the device. |
| `app_ble_hid_connected()` | True while a host is subscribed to the input reports. |
| `app_ble_hid_send(report_id, data, len)` | Queue one input report; ignored while disconnected. |
| `app_ble_hid_restart_advertising()` | Force advertising again. |
| `app_ble_hid_clear_bonds()` | Forget bonded LE peers; returns how many were removed. |

Dependencies: `app_bt_core` (LE stack, names, report map), `app_cfg` (mask),
Bluedroid GATTS, `esp_hidd`.
