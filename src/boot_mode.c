#include "boot_mode.h"
#include "hardware/watchdog.h"
#include "hardware/structs/watchdog.h"

// Top 24 bits are a fixed tag so any scratch content this module didn't write
// itself - stale bits, an unrelated future watchdog use, brown-out artifacts -
// is treated as "no request" rather than decoded into a mode. The mode itself
// lives in the low byte.
#define BOOT_MODE_TAG   0x50414400u
#define BOOT_MODE_MASK  0xFFFFFF00u

enum InputMode boot_mode_on_startup(void)
{
    // Deliberately NOT gated on watchdog_caused_reboot(): on RP2350 that
    // resolves to `watchdog_hw->reason && rom_get_last_boot_type() ==
    // BOOT_TYPE_NORMAL`, and a bare watchdog_reboot(0,0,delay) HW reset
    // (not routed through the ROM's reboot2 API) does not reliably classify
    // as BOOT_TYPE_NORMAL - that made this always fall back to USBSERIAL
    // even right after a legitimate boot_mode_request_switch(). The tag
    // check below is safe on its own: scratch is preserved across a
    // watchdog reset and zeroed by a real power-on-reset (see the header
    // comment), so a stray value matching our exact 32-bit tag by chance is
    // not a real risk.
    uint32_t word = watchdog_hw->scratch[0];
    watchdog_hw->scratch[0] = 0; // consume the request exactly once

    if ((word & BOOT_MODE_MASK) == BOOT_MODE_TAG &&
        (word & 0xFFu) == (uint32_t)INPUT_MODE_XINPUT) {
        return INPUT_MODE_XINPUT;
    }

    return INPUT_MODE_USBSERIAL;
}

void boot_mode_request_switch(enum InputMode mode)
{
    watchdog_hw->scratch[0] = BOOT_MODE_TAG | ((uint32_t)mode & 0xFFu);
    watchdog_reboot(0, 0, 10);
    while (1) { } // watchdog_reboot() does not return; park just in case
}
