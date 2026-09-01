/* ===========================================================================
 *  usb_serial.h  -  Per-chip USB serial number string
 * ===========================================================================
 *
 *  iSerialNumber must be unique per device: Windows keys its composite-driver
 *  cache (interface bindings, WCID state) off VID&PID\<serial>. Reusing a
 *  static string here means every firmware revision collides with whatever
 *  Windows cached for a previous, differently-shaped descriptor set under
 *  that same instance path - producing stale "device not started (usbccgp)"
 *  failures after a reflash. Using the RP2040/RP2350's factory-programmed
 *  flash unique ID keeps the serial stable per physical board but distinct
 *  across boards, which is what a real serial number is for.
 * =========================================================================== */

#ifndef _USB_SERIAL_H_
#define _USB_SERIAL_H_

#ifdef __cplusplus
extern "C" {
#endif

// Returns a cached, null-terminated hex string of the board's unique ID.
// Computed lazily on first call and reused after that.
const char *usb_serial_string(void);

#ifdef __cplusplus
}
#endif

#endif // _USB_SERIAL_H_
