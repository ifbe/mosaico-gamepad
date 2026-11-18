// SPDX-License-Identifier: Apache-2.0

#include "app_usb_core.h"

#include "esp_iris_usb_hid.h"

/* Last IN endpoint number the application may use (EP0 + 7 data IN). */
#define USB_LAST_IN_ENDPOINT 0x87

uint8_t app_usb_core_first_in_endpoint(void)
{
    /* Interface numbers and IN endpoints advance together: ESP-Iris reports the
     * first free interface (2 with the console CDC only, 4 with the data link),
     * and its CDC pairs occupy 0x81/0x82 (plus 0x83/0x84 when present). */
    return (uint8_t)(0x81 + esp_iris_usb_first_free_interface());
}

unsigned app_usb_core_hid_interface_count(uint32_t class_mask)
{
    unsigned count = 0;
    for (unsigned i = 0; i < 3; ++i) { /* generic classes only */
        if ((class_mask & (1u << i)) != 0) {
            ++count;
        }
    }
    return count;
}

uint8_t app_usb_core_mic_endpoint(uint8_t first_ep, unsigned hid_count)
{
    const unsigned slot = (unsigned)first_ep + hid_count;
    return slot <= USB_LAST_IN_ENDPOINT ? (uint8_t)slot : 0;
}
