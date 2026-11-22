# ESP-Mosaico gamepad

ESP32-S31 handheld controller: a 480x480 touch surface that presents itself to a
host as HID over USB, Bluetooth Classic and BLE, with audio and hands-free calls.

## What each link provides

| Link | Host sees |
| --- | --- |
| USB (composite with ESP-Iris) | HID gamepad, mouse and keyboard, plus an optional motion interface; optional DualShock 4 / DualSense emulation; UAC2 speaker + microphone at 48 kHz |
| Bluetooth Classic | HID device (gamepad / mouse / keyboard, motion optional), A2DP sink, HFP hands-free for calls |
| BLE | HID over GATT (HOGP) with its own class mask; advertising while idle |

Every transport keeps its own class mask in NVS. The on-screen settings page and
the RPC service `0x4700` change them; a reboot applies the change.

## Screen

Seven pages: idle, mouse, keyboard, gamepad, sensor, terminal, settings. The BOOT key or
the layout RPC switches pages. Touch drives pointer, keyboard and gamepad input;
the gamepad page adds LB/RB, LS/RS, SELECT/START and analog LT/RT sliders around
the two sticks. The sensor page shows the live gyro/accelerometer/magnetometer
values and can turn device rotation into mouse movement. The settings page
toggles the class masks for USB, Classic Bluetooth and BLE.

## Build and install

```sh
. ../env.sh
python3 submodule/esp-mosaico-utils/mosaico-tools/skills/idf-low-noise-build/scripts/idf_low_noise_build.py \
        --project projects/gamepad build
python3 mosaico.py iris test enter-recovery --project projects/gamepad
python3 mosaico.py iris system-update --project projects/gamepad
python3 mosaico.py iris logs --snapshot --project projects/gamepad
```

The ESP-IDF checkout must stay unmodified (`git -C "$IDF_PATH" status` empty).

## Documentation

| Document | Contents |
| --- | --- |
| [docs/app_ui.md](docs/app_ui.md) | The whole UI: `app_ui_display` / `app_ui_touch` / `app_ui_core` contracts, input event model, startup order, page and settings semantics, gamepad layout, acceptance list |
| [docs/gotcha.md](docs/gotcha.md) | Subsystem notes and triage: USB endpoints, HID descriptors, Classic/BLE HID, call audio, display, memory |
| [docs/todo.md](docs/todo.md) | Open work and candidate features: LE Audio, dual role, two-device intercom |
| [docs/app_cfg.md](docs/app_cfg.md) | Class masks, NVS persistence, configuration RPC |
| [docs/app_usb.md](docs/app_usb.md) | USB interfaces: gamepad, mouse, keyboard, Sony and motion reports, UAC2 audio |
| [docs/app_btclassic.md](docs/app_btclassic.md) | Classic Bluetooth interfaces: HID device, A2DP sink, HFP hands-free |
| [docs/app_ble.md](docs/app_ble.md) | BLE interface: HID over GATT (HOGP) reports and connection behaviour |
| [docs/app_sensor.md](docs/app_sensor.md) | BMI270 / BMM150 motion source |
