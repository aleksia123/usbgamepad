/* ===========================================================================
 *  xinput_ms_os_desc.h  -  Microsoft OS 1.0 Descriptors ("WCID") for XInput
 * ===========================================================================
 *
 *  Windows' built-in XInput driver (xusb22.sys) does NOT bind to interface 0
 *  (class 0xFF/0x5D/0x01) purely by class/subclass/protocol matching under a
 *  third-party (non-Microsoft) VID/PID - its INF only lists specific known
 *  hardware IDs. Every third-party XInput-compatible pad instead uses the
 *  Microsoft OS 1.0 Descriptor mechanism to request the "XUSB10" compatible
 *  ID for interface 0 directly, which xusb22.sys DOES match generically:
 *
 *    1. Windows queries GET_DESCRIPTOR(STRING, index=0xEE) once during
 *       enumeration. We reply with the fixed "MSFT100" signature string,
 *       which also carries a vendor request code (see MS_OS_VENDOR_CODE).
 *    2. Windows then issues a vendor control request using that code
 *       (wIndex = 0x0004) to fetch the Extended Compat ID OS Feature
 *       Descriptor, which lists interface 0's compatible ID as "XUSB10".
 *
 *  Without this, Windows reports "no compatible drivers" (Code 28) for the
 *  XInput interface once a non-Microsoft VID/PID is used - which is exactly
 *  why interface 0 needs it once app_transport.h's second interface forced
 *  us off the real Xbox 360 VID/PID (see XInputDescriptors.h).
 * =========================================================================== */

#ifndef _XINPUT_MS_OS_DESC_H_
#define _XINPUT_MS_OS_DESC_H_

#include <stdint.h>
#include <stdbool.h>
#include "tusb.h"

#ifdef __cplusplus
extern "C" {
#endif

// Route from tud_descriptor_string_cb() when index == 0xEE.
uint16_t const *ms_os_string_descriptor(void);

// Route from tud_vendor_control_xfer_cb(). Returns true if this call
// recognized and handled the request (caller must not fall through to its
// own vendor request handling for this call), false otherwise.
bool ms_os_desc_vendor_control_xfer_cb(uint8_t rhport, uint8_t stage, tusb_control_request_t const *request);

#ifdef __cplusplus
}
#endif

#endif // _XINPUT_MS_OS_DESC_H_
