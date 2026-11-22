# app_ble

BLE (LE) side of the device. It exposes exactly one interface — a HID over GATT
(HOGP) device — and the reports inside it follow the `ble_hid` mask.

## Interface

| Interface | Spec | Enabled by | Host sees |
| --- | --- | --- | --- |
| HID over GATT | HID service `0x1812`, appearance `0x03C4` (HID gamepad) | `bt_mode` includes LE and `ble_hid` mask | a BLE game controller |

Everything else (controller, Bluedroid, device names, **advertising**) belongs to
`app_bt_core`; `app_ble_hid` only runs the HOGP device on top. There is **no Sony
emulation and no audio** over BLE.

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

### Advertising belongs to the transport, not to HID

`app_bt_core` owns the payload, the parameters and the decision to advertise,
next to the Classic scan mode, because the advertisement names whichever services
are up (HID today, LE Audio or custom services later). `app_ble_hid` only builds
the GATT service and reports `app_ble_hid_service_ready()` when the database
exists (the `ESP_HIDD_START` event).

Boot order:

1. `app_bt_core_start()` registers the single GAP callback (advertising events are
   handled there, everything else is forwarded to `app_ble_hid_gap_event()`), sets
   the LE static random address and the device name
2. `app_ble_hid_start()` creates the HOGP service
3. core polls `app_ble_hid_service_ready()` every 50 ms (up to ~3 s), then
   configures the payload
4. the payload-set-complete event starts advertising — only when the policy bit,
   the payload and the service are all ready, so a host never sees an
   advertisement for a service that does not exist yet
5. a 1 s watcher restarts advertising after the link drops

Payload (hand-built legacy advertisement; the Bluedroid `esp_ble_adv_data_t` path
only accepts 128-bit UUIDs, which does not fit):

| Field | Value |
| --- | --- |
| Flags | `0x06` = LE General Discoverable, **BR/EDR Not Supported** |
| Service UUID list | `0x1812` (HID over GATT) |
| Complete local name | `app_bt_core_le_name()` |

The BR/EDR flag is deliberate: advertising from the LE-only static random address
while claiming BR/EDR support makes Android treat the device as dual-mode and
connect over Classic instead of GATT (the "connect LE, land on Classic" symptom).

Parameters: `adv_int_min 0x20` / `adv_int_max 0x30` (20–30 ms), `ADV_TYPE_IND`,
random own address, all channels, no filter.

`app_bt_core_ble_advertise(bool)` is the switch (off stops, on starts when ready);
RPC method 8 (`ble_adv`) reaches it through an `app_cfg` hook so `app_cfg` keeps
no dependency on the transport components.

## Public API

| Function | Contract |
| --- | --- |
| `app_ble_hid_start(map, len)` | Start the HOGP device (no advertising). Requires `app_bt_core_start()` with LE enabled; the map must outlive the device. |
| `app_ble_hid_connected()` | True while a host is subscribed to the input reports. |
| `app_ble_hid_send(report_id, data, len)` | Queue one input report; ignored while disconnected. |
| `app_ble_hid_service_ready()` | True once the GATT service is up; core polls it before advertising. |
| `app_ble_hid_gap_event(event, param)` | Called by core for the GAP events it does not handle itself (pairing, connection parameters). |
| `app_bt_core_ble_advertise(on)` | The advertising switch (in `app_bt_core`). |
| `app_ble_hid_clear_bonds()` | Forget bonded LE peers; returns how many were removed. |

Dependencies: `app_bt_core` (LE stack, names, report map), `app_cfg` (mask),
Bluedroid GATTS, `esp_hidd`.
