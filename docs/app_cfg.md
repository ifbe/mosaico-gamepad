# app_cfg

Persists the per-transport HID class masks and serves the configuration RPC.

## Responsibilities

- Own the NVS record of what each link exposes: USB (`usb_hid2`), Classic
  Bluetooth (`bt_hid`), BLE (`ble_hid`), plus `bt_mode`, `bt_audio`,
  `usb_audio`, `usb_vid` and `usb_pid`.
- Normalise masks so the combination stays legal (see below).
- Relay the screen-layout RPC commands to the UI through two callbacks; this
  component has no UI dependency of its own.
- Register RPC service `0x4700` after Iris is up (about 1.5 s after boot).

## Class bits (persisted masks)

| Bit | `APP_CLASS_*` | Meaning |
| --- | --- | --- |
| 0 | `GAMEPAD` | generic gamepad interface |
| 1 | `MOUSE` | relative mouse |
| 2 | `KEYBOARD` | 6-key rollover keyboard |
| 3 | `DS4` | DualShock 4 emulation (USB only) |
| 4 | `DS5` | DualSense emulation (USB only) |
| 5 | `SENSOR` | motion interface / motion report |
| 6 | `XBOX` | reserved placeholder, not implemented |

`APP_CLASS_ALL` = gamepad|mouse|keyboard. `APP_CLASS_BT_ALL` adds `SENSOR` and
is the mask used by `bt_hid` / `ble_hid`. `APP_CLASS_USB_DEFAULT` is
`APP_CLASS_ALL | APP_CLASS_SENSOR` (the factory USB default); Bluetooth and BLE
default to `APP_CLASS_ALL`.

## Normalisation rules

`normalize_mask()` runs on every load and every store:

- At most one of `DS4`/`DS5`/`XBOX` may be set (lowest bit wins).
- An emulation identity clears `GAMEPAD` and `SENSOR`: one Sony interface takes
  the endpoint the generic gamepad and the motion interface would use.
- Without an emulation identity the emulation bits are cleared.

The on-screen settings page applies the same rules; see `docs/app_ui.md`.

## Public API

| Function | Contract |
| --- | --- |
| `app_cfg_init()` | Load the NVS record (or defaults). Call before anything reads a mask. |
| `app_cfg_classes()` / `app_cfg_usb_screen_mode()` | USB mask and the Sony type derived from it. |
| `app_cfg_set_usb_hid()` / `app_cfg_set_bt_hid()` / `app_cfg_set_ble_hid()` | Store a normalised mask in NVS. |
| `app_cfg_usb_audio()` / `app_cfg_set_usb_audio()`, `app_cfg_bt_audio()` / `app_cfg_set_bt_audio()` | Audio switches. |
| `app_cfg_bt_mode()` | `off` / `ble` / `classic` / `both`; change it through RPC method 6. |
| `app_cfg_usb_ids(vid, pid)` | USB identity actually used; the Sony modes derive their own VID/PID. |
| `app_cfg_has(mask)` | Bit test helper. |
| `app_cfg_reset_all()` | Restore every compiled default. |
| `app_cfg_set_layout_rpc()` | UI registers the layout name/apply callbacks used by RPC method 4. |
| `app_cfg_register_rpc()` | Start the RPC task; safe to call once. |

Mask and audio changes only take effect after a reboot because they change the
USB descriptors and the Bluetooth report maps.

## RPC service `0x4700`

| Method | Payload | Result |
| --- | --- | --- |
| 1 | – | `{usb_hid, usb_ds, usb_vid, usb_pid, usb_audio, bt_hid, bt_mode, bt_audio, layout}` |
| 2 / 3 | `{"class":"mouse"}` | enable / disable one class |
| 4 | `{"layout":"setting"}` | switch page (via the UI hooks) |
| 5 | – | reboot |
| 6 | `{"cmd":"set","key":"bt_mode","value":"both"}` | change one setting |
| 7 | – | clear Bluetooth bonds |
| 8 | – | restart BLE advertising |
| 9 | – | reset every setting to default |

## Dependencies

`app_ble_hid` (advertising restart), `esp_iris` (RPC), `nvs_flash`, `log`.
