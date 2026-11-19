# app_btclassic

Bluetooth Classic (BR/EDR). The device registers three profiles, all over the
same ACL link; this document describes what each one presents to the host.

## Profiles

| Profile | Role | Enabled by | Host sees |
| --- | --- | --- | --- |
| HID device (HIDP) | device | `bt_hid` mask | gamepad / mouse / keyboard (+ motion reports), per mask |
| A2DP sink | sink | `bt_audio` bit 0 | a Bluetooth speaker |
| HFP hands-free | HF client | `bt_audio` bit 1 | a headset with microphone for calls |

`app_bt_core` brings up the controller and Bluedroid for the `bt_mode` setting
(`off` / `ble` / `classic` / `both`) and builds the report map; the profiles
themselves live in `app_btclassic_hid` and `app_btclassic_audio`.

## HID device (HIDP)

The report map is assembled at boot from the enabled classes, so the host sees the
same reports as over USB:

| Report ID | Class | Payload |
| --- | --- | --- |
| 1 | gamepad | 8 B: buttons, sticks, triggers |
| 2 | mouse | 5 B: buttons, dx, dy, wheel, pan |
| 3 | keyboard | 8 B: modifiers, 6 keys |
| 4 | motion | 18 B: accel, gyro, magnetometer |

The byte layouts are the ones documented in [app_usb.md](app_usb.md#hid-gamepad);
both transports share one descriptor set, so there is no separate classic format.

- The Sony emulation is **USB only** (`app_cfg` exposes it on the USB mask).
- The whole map is embedded in the HID SDP record, so the SDP buffers are raised
  to 1024 bytes; with the default 300 the service never registers.
- On connect the app raises the link QoS (`esp_bt_gap_set_qos(peer, 20)`, 12.5 ms
  poll) — the connect event carries no peer address, so every bonded peer is set.
- Reports are paced by the shared 8 ms task; mouse motion is decimated to every
  second tick (~62 Hz) because the interrupt channel cannot drain 125 Hz.

## A2DP sink

Standard SBC sink. The codec rate follows what the phone negotiates (44.1 or
48 kHz) and decoded audio is written to the ES8311 through
`app_audio_codec_playback_write()`. Volume policy: the Bluetooth source plays at
100 % (see `docs/gotcha.md` §5.7).

## HFP hands-free

Audio link is CVSD, 8 kHz, mono in both directions (wideband/mSBC is not used:
the phones here reject it with `invalid air mode: 255`).

| Direction | Format | Path |
| --- | --- | --- |
| host → device | 8 kHz mono 16 bit | `hfp_legacy_recv()`, duplicated to stereo and upsampled 3x to the codec |
| device → host | 8 kHz mono 16 bit | mic task, 10 ms chunks of 48 kHz downsampled, 1 KB ring, pulled by `hfp_legacy_send()` |

The uplink task runs at priority 5 and the ring is primed with 20 ms of silence
at SCO start; both were needed before the far end could hear the microphone.
`MIC_READ_FRAMES` must stay at 10 ms — shorter chunks produce silence.

### Keeping the link out of sniff

Bluedroid's power management pushes the ACL link into sniff by itself (after
every HID report with zero delay, and even while a SCO call is up). While the
link sleeps, HID reports wait a sniff-to-active round trip (~40-50 ms) and the
controller refuses the eSCO setup for a call, so **both symptoms are the same
cause**.

`app_bt_core` owns the countermeasure, because the link, the peer address and the
coalescing window all belong to it:

| Piece | Contract |
| --- | --- |
| `app_bt_core_kick_acl_sniff()` | Take the link out of sniff now. Anyone may call it at any time; calls inside a 1 s window are dropped (`APP_BT_KICK_MIN_INTERVAL_US`), so a 100 Hz caller cannot flood the controller. Returns true only when a kick actually happened. Task context. |
| `app_bt_core_note_classic_peer(bda)` | Cache the peer so kicks skip the bond lookup. The HFP stack-up event reports it; the classic HID connect event does not. |
| `app_bt_core_kick_stats()` | `kicks / throttled / age_ms` for the status line. |
| 1 s timer in `app_bt_core` | Keeps kicking while the link can matter. The start condition is currently a stub that is on whenever Classic is enabled; the real condition will be "Classic connected AND (a HID class enabled/connected OR a call up)". |

Inside, the kick calls `bta_dm_pm_active()` — an exported BTA symbol with no
public header (declared in `app_bt_core.c`). Call sites: HID connect, HFP
stack-up and every call/ring event, plus the periodic timer.

## Diagnostics

One `bt_classic: status` line every 10 s summarises every link and counter
(`paired`, `a2dp_link`, `hfp`, `sco`, `call`, `lx/lt`, `pull`, `mic`, `peak`,
`kick=<executed>/<throttled>/<age ms>`), and
`mic rate: N iters/s, B/s, amp=P` appears every 5 s during a call. Reading order:
`docs/gotcha.md` §5.8.

## Public API

| Component | Function | Contract |
| --- | --- | --- |
| `app_bt_core` | `app_bt_core_start()` | Controller + Bluedroid + identity, once after NVS. |
| | `app_bt_core_hid_start()` / `app_bt_core_hid_connected()` / `app_bt_core_hid_send(id, data, len)` | Build the map, start the transports, fan out reports. |
| | `app_bt_core_wants_ble()` / `app_bt_core_wants_classic()` | Mode queries. |
| `app_bt_core_kick_acl_sniff()` / `app_bt_core_note_classic_peer()` / `app_bt_core_kick_stats()` | Sniff countermeasure, see above. |
| `app_btclassic_hid` | `app_btclassic_hid_start(map, len)` / `_connected()` / `_send(...)` | Classic HID device. |
| `app_btclassic_audio` | `app_btclassic_audio_start()` / `_stop()` / `_connected()` | A2DP sink + HFP. |

Dependencies: `app_cfg`, `app_usb_hid` (descriptors), `app_audio_codec`,
Bluedroid (`esp_hidd`, `esp_a2d`, `esp_hf_client`).
