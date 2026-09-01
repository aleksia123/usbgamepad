/* ===========================================================================
 *  app_transport.h  -  Custom HID "app transport" interface
 * ===========================================================================
 *
 *  Second composite USB interface (alongside the existing XInput device
 *  interface) that lets a Windows application read the raw physical
 *  controller state and write back a processed/filtered output state.
 *
 *  Report 0x11 (IN,  device -> host): physical_state (raw controller read)
 *  Report 0x10 (OUT, host -> device): output_state   (processed/filtered)
 *
 *  Both reports share the same 12-byte packed payload below. The report ID
 *  is NOT part of the payload - TinyUSB sends/receives it as a separate
 *  leading byte.
 *
 *  `buttons` reuses the exact bit values XINPUT_GAMEPAD_* already defines in
 *  xinput_host.h (kept here as a comment since a C# consumer can't include a
 *  C header):
 *
 *      DPAD_UP=0x0001  DPAD_DOWN=0x0002  DPAD_LEFT=0x0004  DPAD_RIGHT=0x0008
 *      START=0x0010    BACK=0x0020       LEFT_THUMB=0x0040 RIGHT_THUMB=0x0080
 *      LEFT_SHOULDER=0x0100  RIGHT_SHOULDER=0x0200  GUIDE=0x0400  SHARE=0x0800
 *      A=0x1000  B=0x2000  X=0x4000  Y=0x8000
 *
 *  The XInput device output ("XInput-compatible USB device" interface) is
 *  driven from output_state (via gamepad(0)), never directly from the
 *  physical controller. If no valid output_state report has been received
 *  within the last 50 ms (app not running, USB unplugged/suspended, physical
 *  controller disconnected, or firmware just started), output falls back to
 *  the neutral state - see app_transport_core0_task().
 * =========================================================================== */

#ifndef _APP_TRANSPORT_H_
#define _APP_TRANSPORT_H_

#include <stdint.h>
#include <stdbool.h>
#include "tusb.h"

#ifdef __cplusplus
extern "C" {
#endif

#define APP_TRANSPORT_ITF_NUM        1     // USB bInterfaceNumber in the config descriptor
// TinyUSB's tud_hid_*_cb callbacks are keyed by HID *instance index* (0-based
// among HID-class interfaces), NOT by USB interface number. The XInput
// interface is a vendor class (0xFF), so it is not a HID instance; the
// app-transport interface is therefore the one and only HID instance, index 0.
// usbdriver.cpp must route on this value, not APP_TRANSPORT_ITF_NUM.
#define APP_TRANSPORT_HID_INSTANCE   0
#define APP_TRANSPORT_REPORT_ID_OUT  0x10  // host -> device, output_state
#define APP_TRANSPORT_REPORT_ID_IN   0x11  // device -> host, physical_state
#define APP_TRANSPORT_WATCHDOG_MS    50

typedef struct __attribute__((packed))
{
    uint16_t buttons;
    uint8_t  left_trigger;
    uint8_t  right_trigger;
    int16_t  thumb_lx;
    int16_t  thumb_ly;
    int16_t  thumb_rx;
    int16_t  thumb_ry;
} app_controller_payload_t;

_Static_assert(sizeof(app_controller_payload_t) == 12,
               "Controller payload must be exactly 12 bytes");

// Must be called once at startup, after init_tusb_gamepad().
void app_transport_init(void);

// Core 1: publish a freshly-read physical controller sample.
void app_transport_publish_physical(const app_controller_payload_t *state);

// Core 1: zero physical_state (e.g. physical controller disconnected).
void app_transport_clear_physical(void);

// Force the watchdog to treat output_state as expired on the next
// app_transport_core0_task() call (mount/suspend/disconnect handlers).
void app_transport_force_neutral(void);

// Core 0, once per main loop iteration, BEFORE tusb_gamepad_task():
// runs the watchdog, applies output_state (or neutral) into gamepad(0),
// and sends physical_state as report 0x11 if it changed.
void app_transport_core0_task(void);

// Routing targets called from lib/tusb_gamepad/src/usbdriver.cpp for itf==1.
void     app_transport_on_set_report(uint8_t report_id, hid_report_type_t report_type,
                                      uint8_t const *buffer, uint16_t bufsize);
uint16_t app_transport_on_get_report(uint8_t report_id, hid_report_type_t report_type,
                                      uint8_t *buffer, uint16_t reqlen);

uint8_t const *app_transport_report_descriptor(void);
uint16_t       app_transport_report_descriptor_len(void);

#ifdef __cplusplus
}
#endif

#endif // _APP_TRANSPORT_H_
