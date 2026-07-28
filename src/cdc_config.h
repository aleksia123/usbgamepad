// cdc_config.h - newline-delimited JSON command protocol over CDC (tud_cdc_*),
// active only while the device is running in the USBSERIAL (config) InputMode.
//
// One JSON object per line, both directions. Command set:
//   PING                          -> {"ok":true,"cmd":"PING","pong":true}
//   INFO                          -> {"ok":true,"cmd":"INFO","fw":...,"version":...,"mode":...}
//   PAD_CONFIG.GET                -> {"ok":true,"cmd":"PAD_CONFIG.GET","cfg":{...12 fields...}}
//   PAD_CONFIG.SET {"cfg":{...}}  -> applies any subset of fields to the live
//                                     g_pad_config immediately, runs
//                                     pad_config_sanitize() over the result,
//                                     debounces a flash write, and replies
//                                     with the full resulting struct plus
//                                     "clamped":true|false. clamped=true means
//                                     at least one value was moved into its
//                                     stage's usable band, so the client
//                                     should take the echoed cfg as truth
//                                     rather than what it sent.
//   PAD_CONFIG.RESET              -> restores the compiled-in
//                                     PAD_CONFIG_DEFAULTS, debounces a flash
//                                     write, replies with the resulting
//                                     struct. Keeps "restore defaults" in the
//                                     firmware instead of duplicating the
//                                     default table in the web client.
//   MODE.GET                      -> {"ok":true,"cmd":"MODE.GET","mode":...}
//   MODE.SET {"mode":"XINPUT"}    -> acks, flushes any pending flash write,
//                                     then reboots into XInput mode
//   INPUT.STREAM {"enable":true}  -> {"ok":true,"cmd":"INPUT.STREAM","enable":true},
//                                     then a live event every ~33ms while enabled
//                                     and a client is attached (tud_cdc_connected()):
//                                     {"evt":"input","lx":..,"ly":..,"rx":..,"ry":..,
//                                      "lt":..,"rt":..,"btn":{"up":false,...}}
//                                     Values are the SHAPED output (post axial
//                                     deadzone + stick_radial correction), read
//                                     from gamepad(0) - the same struct the host
//                                     controller-reading side (hid_app.c, core1)
//                                     keeps live regardless of which InputMode
//                                     is active, so this works even though the
//                                     device isn't presenting as XInput right now.
// Anything else (malformed JSON, unknown cmd) -> {"ok":false,"error":"..."}
//
// Config mode is always the boot default (see boot_mode.h), so MODE.SET only
// ever needs to support switching TO XInput - there's no reverse direction
// over this protocol (unplug/replug or the grace-window timeout cover it).
//
// Grace window: if no complete line arrives from the host within 60s of boot,
// cdc_config_task() requests a reboot into XInput mode on its own, so a fully
// hands-off device still ends up as a working game controller even if no one
// is driving config mode. Receiving any line (even a malformed one) cancels
// the timer permanently for that boot.
#ifndef CDC_CONFIG_H
#define CDC_CONFIG_H
#include "inputmodes.h"

// Call once, right after init_tusb_gamepad(). Arms the 60s grace timer iff
// mode == INPUT_MODE_USBSERIAL; otherwise this module is fully inert for the
// rest of the boot (no CDC polling, no timer, no reboot path).
void cdc_config_init(enum InputMode mode);

// Call every main-loop iteration. No-op unless booted into USBSERIAL mode.
void cdc_config_task(void);

#endif // CDC_CONFIG_H