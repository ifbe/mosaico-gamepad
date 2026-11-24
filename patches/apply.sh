#!/usr/bin/env bash
# Apply this project's ESP-Iris patch to the shared submodule checkout.
#
# Why: the gamepad appends USB composite interfaces (HID classes plus a UAC2
# function) after Iris' own CDC pairs. ESP-Iris owns the TinyUSB configuration
# descriptor and upstream has no hook for extra interfaces, so the transport
# source has to be extended. That change is kept here as a patch; it is applied to
# the submodule rather than forked into this repository, because vendoring the
# whole component (about 800 KB of source) is not worth it for ~200 lines.
#
#   ./patches/apply.sh            # apply if missing (idempotent)
#   ./patches/apply.sh --check    # report only
#   ./patches/apply.sh --reverse  # remove the patch again
#
# Re-run it after `git submodule update`, a fresh clone, or a new ESP-Iris
# revision. The build fails immediately without it (missing esp_iris_usb_*.h).
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
project="$(cd "$here/.." && pwd)"
submodule="$project/../../submodule/esp-mosaico-utils"
comp="$submodule/ESP-Iris/components/esp_iris"
patch_file="$here/0001-esp-iris-usb-composite.patch"

check_only=0
reverse=0
for arg in "$@"; do
    case "$arg" in
        --check) check_only=1 ;;
        --reverse) reverse=1 ;;
        *) echo "unknown argument: $arg" >&2; exit 2 ;;
    esac
done

if [[ ! -d "$comp" ]]; then
    echo "ESP-Iris component not found: $comp" >&2
    echo "Initialize the submodule first (git submodule update --init)." >&2
    exit 1
fi
if [[ ! -f "$patch_file" ]]; then
    echo "patch missing: $patch_file" >&2
    exit 1
fi

# Detection by marker rather than `patch -R --dry-run`: the patch creates a file,
# and patch(1) cannot reverse a file creation.
marker_header="$comp/include/esp_iris_usb_hid.h"
marker_src="$comp/src/esp_iris_transport_usb.c"
applied=0
if [[ -f "$marker_header" ]] && grep -q "esp_iris_usb_set_hid_ifaces" "$marker_src" 2>/dev/null; then
    applied=1
fi

if (( reverse )); then
    if (( applied == 0 )); then
        echo "not applied"
        exit 0
    fi
    (( check_only )) && { echo "would revert"; exit 0; }
    # The only local change to that file is this patch, so restore it from git and
    # drop the added header.
    git -C "$submodule" checkout -- ESP-Iris/components/esp_iris/src/esp_iris_transport_usb.c
    rm -f "$marker_header"
    echo "reverted: $comp"
    exit 0
fi

if (( applied == 1 )); then
    (( check_only )) && echo "up to date"
    exit 0
fi
(( check_only )) && { echo "would apply"; exit 0; }

# --forward keeps it from applying on top of a half-applied tree; a hard failure
# here means the submodule revision moved and the patch needs refreshing.
if ! patch -p1 --batch --forward -d "$comp" < "$patch_file" >/dev/null; then
    echo "patch failed to apply; refresh it against the current submodule" >&2
    exit 1
fi
echo "applied: $comp"
