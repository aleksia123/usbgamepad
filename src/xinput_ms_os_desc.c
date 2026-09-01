#include <string.h>

#include "xinput_ms_os_desc.h"

// The vendor request code Windows will use (learned from the string
// descriptor below) to fetch our OS feature descriptors. Any 0x00-0xFF value
// works as long as it doesn't collide with a standard USB request code.
#define MS_OS_VENDOR_CODE          0x01
#define MS_OS_EXT_COMPAT_ID_INDEX  0x0004

// GET_DESCRIPTOR(STRING, 0xEE) response: bLength(18), bDescriptorType(STRING),
// the fixed ASCII signature "MSFT100" as UTF-16LE, then bMS_VendorCode, bPad.
static const uint8_t ms_os_string_desc[] =
{
    0x12, 0x03,
    'M', 0x00, 'S', 0x00, 'F', 0x00, 'T', 0x00, '1', 0x00, '0', 0x00, '0', 0x00,
    MS_OS_VENDOR_CODE, 0x00,
};

_Static_assert(sizeof(ms_os_string_desc) == 18, "MS OS string descriptor must be 18 bytes");

uint16_t const *ms_os_string_descriptor(void)
{
    return (uint16_t const *)ms_os_string_desc;
}

// Extended Compat ID OS Feature Descriptor: a 16-byte header followed by one
// 24-byte "function section" per interface being described. We only need to
// describe interface 0 (XInput) - interface 1 is a real HID interface and
// Windows' built-in HID class driver already binds it without any of this.
typedef struct __attribute__((packed))
{
    uint32_t dwLength;
    uint16_t bcdVersion;
    uint16_t wIndex;
    uint8_t  bCount;
    uint8_t  reserved1[7];

    uint8_t  bFirstInterfaceNumber;
    uint8_t  reserved2; // always 0x01 per the MS OS descriptor spec
    uint8_t  compatibleID[8];
    uint8_t  subCompatibleID[8];
    uint8_t  reserved3[6];
} ms_ext_compat_id_desc_t;

_Static_assert(sizeof(ms_ext_compat_id_desc_t) == 40,
               "Extended Compat ID descriptor must be 40 bytes (16 header + 24 function)");

static const ms_ext_compat_id_desc_t ext_compat_id_desc =
{
    .dwLength = sizeof(ms_ext_compat_id_desc_t),
    .bcdVersion = 0x0100,
    .wIndex = MS_OS_EXT_COMPAT_ID_INDEX,
    .bCount = 1,
    .reserved1 = {0},

    .bFirstInterfaceNumber = 0, // XInput interface
    .reserved2 = 0x01,
    .compatibleID = "XUSB10",
    .subCompatibleID = {0},
    .reserved3 = {0},
};

bool ms_os_desc_vendor_control_xfer_cb(uint8_t rhport, uint8_t stage, tusb_control_request_t const *request)
{
    if (request->bRequest != MS_OS_VENDOR_CODE) return false;
    if (request->wIndex != MS_OS_EXT_COMPAT_ID_INDEX) return false;

    if (stage == CONTROL_STAGE_SETUP)
    {
        uint16_t len = request->wLength;
        if (len > sizeof(ext_compat_id_desc)) len = sizeof(ext_compat_id_desc);
        return tud_control_xfer(rhport, request, (void *)&ext_compat_id_desc, len);
    }

    return true; // DATA/ACK stages: nothing further to do
}
