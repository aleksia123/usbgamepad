// boot_mode.h - which InputMode to boot into, chosen without touching flash.
//
// The device can only run ONE USB personality at a time (see DriverManager in
// lib/tusb_gamepad) - switching personalities means a reboot with a different
// enum InputMode passed to init_tusb_gamepad(). There is no way for a host to
// ask an already-running XInput session to switch, since XInput mode has no
// spare interface, so config mode (INPUT_MODE_USBSERIAL) is always the boot
// default; cdc_config.c is responsible for falling back to XInput if nothing
// claims the config session (see cdc_config.h).
//
// "Which mode to boot into" is carried across a *requested* reboot using the
// RP2040/RP2350 watchdog scratch registers, which survive a watchdog_reboot()
// but reset to 0 on a real power-on-reset (unplug/replug). That POR reset is
// used deliberately as the escape hatch: a bad boot_mode_request_switch() can
// only ever misroute a soft reboot, never survive a power cycle.
#ifndef BOOT_MODE_H
#define BOOT_MODE_H
#include "inputmodes.h"

// Reads and consumes (zeroes) the watchdog scratch boot request. Call exactly
// once, first thing in main(), before init_tusb_gamepad(). Returns
// INPUT_MODE_XINPUT only if scratch[0] holds a request this module itself
// wrote for that exact mode; every other case (power-on-reset, untouched
// scratch, garbage, a request for any other mode) returns
// INPUT_MODE_USBSERIAL. Deliberately does NOT gate on watchdog_caused_reboot()
// - see the comment in boot_mode.c for why that check is unreliable here on
// RP2350.
enum InputMode boot_mode_on_startup(void);

// Records "boot into `mode` next time" in watchdog scratch and reboots via
// watchdog_reboot(). Does not return. Only ever called with INPUT_MODE_XINPUT
// today (config mode is always the power-on default, so there's no reverse
// direction to request).
void boot_mode_request_switch(enum InputMode mode);

#endif // BOOT_MODE_H
