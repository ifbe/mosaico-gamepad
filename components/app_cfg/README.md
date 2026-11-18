# app_cfg

Persists which HID classes are exposed and exposes the configuration RPC.

- NVS namespace `gamepad`, key `classes` (bit0 gamepad, bit1 touch, bit2 mouse,
  bit3 keyboard), default `0xF`.
- Iris RPC service `0x4700`:
  - `1` `{"cmd":"list"}`
  - `2` `{"cmd":"enable","class":"mouse"}`
  - `3` `{"cmd":"disable","class":"mouse"}`
  - `4` `{"cmd":"layout","layout":"keyboard"}`
  - `5` `{"cmd":"reboot"}`

Class changes apply after a reboot (the USB descriptor is built at boot). The
layout command applies immediately and only changes what the screen shows.
