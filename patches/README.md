# patches/

External patches this project needs but cannot express in application code.
Each `.patch` file is a `git diff` against the dependency it names, numbered in
apply order.

```bash
./patches/apply.sh --check     # is it applied?
./patches/apply.sh             # apply to the submodule checkout (idempotent)
./patches/apply.sh --reverse   # remove it again
```

| Patch | Target | Why it cannot live in the project |
| --- | --- | --- |
| `0001-esp-iris-usb-composite.patch` | `submodule/esp-mosaico-utils/ESP-Iris/components/esp_iris` (`src/esp_iris_transport_usb.c`, new `include/esp_iris_usb_hid.h`) | ESP-Iris owns the TinyUSB configuration descriptor. The gamepad appends HID interfaces (`esp_iris_usb_set_hid_ifaces()`) and a UAC2 function (`esp_iris_usb_set_extra_interfaces()`) after Iris' CDC pairs, and needs `esp_iris_usb_first_free_interface()` / `esp_iris_usb_set_ids()`. Upstream Iris (checked at `f58f4f9`) has no hook for extra interfaces and the descriptor is composed at init time, so the application cannot inject interfaces from outside. |

Notes:

- The patch is applied to the shared submodule checkout, not to a copy: vendoring
  the component would duplicate ~800 KB of source for ~200 lines of change.
- `git submodule update`, a fresh clone or a new ESP-Iris revision drops the
  change: re-run `./patches/apply.sh` before building (the build fails at once
  with missing `esp_iris_usb_*.h` / undefined `esp_iris_usb_set_hid_ifaces`).
- The same hooks are used by `projects/audio` and `projects/hid`, which keep their
  own copies of the relevant patches.
- Long term the change belongs upstream in `esp-mosaico/esp-mosaico-utils`; once it
  lands, delete this patch.
