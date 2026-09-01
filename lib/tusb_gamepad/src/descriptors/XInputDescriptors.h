/*
 * SPDX-License-Identifier: MIT
 * SPDX-FileCopyrightText: Copyright (c) 2021 Jason Skuby (mytechtoybox.com)
 */

#ifndef _XINPUT_DESCRIPTORS_H_
#define _XINPUT_DESCRIPTORS_H_

#include <stdint.h>

#define XINPUT_ENDPOINT_SIZE 20

// Buttons 1 (8 bits)
// TODO: Consider using an enum class here.
#define XBOX_MASK_UP    (1U << 0)
#define XBOX_MASK_DOWN  (1U << 1)
#define XBOX_MASK_LEFT  (1U << 2)
#define XBOX_MASK_RIGHT (1U << 3)
#define XBOX_MASK_START (1U << 4)
#define XBOX_MASK_BACK  (1U << 5)
#define XBOX_MASK_LS    (1U << 6)
#define XBOX_MASK_RS    (1U << 7)

// Buttons 2 (8 bits)
// TODO: Consider using an enum class here.
#define XBOX_MASK_LB    (1U << 0)
#define XBOX_MASK_RB    (1U << 1)
#define XBOX_MASK_HOME  (1U << 2)
//#define UNUSED        (1U << 3)
#define XBOX_MASK_A     (1U << 4)
#define XBOX_MASK_B     (1U << 5)
#define XBOX_MASK_X     (1U << 6)
#define XBOX_MASK_Y     (1U << 7)

#define XBOX_REPORT_TYPE_RUMBLE	0x00
#define XBOX_REPORT_TYPE_LED 	0x01

typedef struct __attribute((packed, aligned(1)))
{
	uint8_t report_id;
	uint8_t report_size;
	uint8_t buttons1;
	uint8_t buttons2;
	uint8_t lt;
	uint8_t rt;
	int16_t lx;
	int16_t ly;
	int16_t rx;
	int16_t ry;
	uint8_t _reserved[6];
} XInputReport;

typedef struct __attribute((packed, aligned(1)))
{
	uint8_t report_type;
	uint8_t report_size;
	uint8_t led;
	uint8_t lrumble;
	uint8_t rrumble;
	uint8_t reserved[3];
} XInputOutReport;

static const uint8_t xinput_string_language[]    = { 0x09, 0x04 };
static const uint8_t xinput_string_manfacturer[] = "Microsoft";
static const uint8_t xinput_string_product[]     = "XInput STANDARD GAMEPAD";
static const uint8_t xinput_string_version[]     = "1.0";

static const uint8_t *xinput_string_descriptors[] __attribute__((unused)) =
{
	xinput_string_language,
	xinput_string_manfacturer,
	xinput_string_product,
	xinput_string_version
};

static const uint8_t xinput_device_descriptor[] =
{
	0x12,       // bLength
	0x01,       // bDescriptorType (Device)
	0x00, 0x02, // bcdUSB 2.00
	0x00,	      // bDeviceClass    (composite: defined per interface)
	0x00,	      // bDeviceSubClass
	0x00,	      // bDeviceProtocol
	0x40,	      // bMaxPacketSize0 64
	0x09, 0x12, // idVendor 0x1209  (see xinput_ms_os_desc.h: a composite
	0x01, 0x00, // idProduct 0x0001  device cannot keep Microsoft's ids)
	0x14, 0x01, // bcdDevice 2.14
	0x01,       // iManufacturer (String Index)
	0x02,       // iProduct (String Index)
	0x03,       // iSerialNumber (String Index)
	0x01,       // bNumConfigurations 1
};

static const uint8_t xinput_configuration_descriptor[] =
{
	0x09,        // bLength
	0x02,        // bDescriptorType (Configuration)
	0x50, 0x00,  // wTotalLength 80  (48 XInput + 32 app transport)
	0x02,        // bNumInterfaces 2
	0x01,        // bConfigurationValue
	0x00,        // iConfiguration (String Index)
	0x80,        // bmAttributes
	0xFA,        // bMaxPower 500mA

	0x09,        // bLength
	0x04,        // bDescriptorType (Interface)
	0x00,        // bInterfaceNumber 0
	0x00,        // bAlternateSetting
	0x02,        // bNumEndpoints 2
	0xFF,        // bInterfaceClass
	0x5D,        // bInterfaceSubClass
	0x01,        // bInterfaceProtocol
	0x00,        // iInterface (String Index)

	0x10,        // bLength
	0x21,        // bDescriptorType (HID)
	0x10, 0x01,  // bcdHID 1.10
	0x01,        // bCountryCode
	0x24,        // bNumDescriptors
	0x81,        // bDescriptorType[0] (Unknown 0x81)
	0x14, 0x03,  // wDescriptorLength[0] 788
	0x00,        // bDescriptorType[1] (Unknown 0x00)
	0x03, 0x13,  // wDescriptorLength[1] 4867
	0x01,        // bDescriptorType[2] (Unknown 0x02)
	0x00, 0x03,  // wDescriptorLength[2] 768
	0x00,        // bDescriptorType[3] (Unknown 0x00)

	0x07,        // bLength
	0x05,        // bDescriptorType (Endpoint)
	0x81,        // bEndpointAddress (IN/D2H)
	0x03,        // bmAttributes (Interrupt)
	0x20, 0x00,  // wMaxPacketSize 32
	0x01,        // bInterval 1 (unit depends on device speed)

	0x07,        // bLength
	0x05,        // bDescriptorType (Endpoint)
	0x01,        // bEndpointAddress (OUT/H2D)
	0x03,        // bmAttributes (Interrupt)
	0x20, 0x00,  // wMaxPacketSize 32
	0x08,        // bInterval 8 (unit depends on device speed)

	// ---- Interface 1: app transport (see src/app_transport.h) ----
	// A plain HID interface carrying the ~1000 Hz pipe to the PC
	// application. Windows' built-in HID driver binds it with no help;
	// only interface 0 needs the MS OS descriptors.
	0x09,        // bLength
	0x04,        // bDescriptorType (Interface)
	0x01,        // bInterfaceNumber 1
	0x00,        // bAlternateSetting
	0x02,        // bNumEndpoints 2
	0x03,        // bInterfaceClass (HID)
	0x00,        // bInterfaceSubClass (none - not a boot device)
	0x00,        // bInterfaceProtocol (none)
	0x00,        // iInterface (String Index)

	0x09,        // bLength
	0x21,        // bDescriptorType (HID)
	0x11, 0x01,  // bcdHID 1.11
	0x00,        // bCountryCode
	0x01,        // bNumDescriptors
	0x22,        // bDescriptorType[0] (Report)
	0x21, 0x00,  // wDescriptorLength[0] 33 - must match the
	             // _Static_assert in src/app_transport.c

	0x07,        // bLength
	0x05,        // bDescriptorType (Endpoint)
	0x82,        // bEndpointAddress (IN/D2H)
	0x03,        // bmAttributes (Interrupt)
	0x40, 0x00,  // wMaxPacketSize 64
	0x01,        // bInterval 1 - the 1 ms slot this mode exists for

	0x07,        // bLength
	0x05,        // bDescriptorType (Endpoint)
	0x02,        // bEndpointAddress (OUT/H2D)
	0x03,        // bmAttributes (Interrupt)
	0x40, 0x00,  // wMaxPacketSize 64
	0x01,        // bInterval 1
};

#endif // _XINPUT_DESCRIPTORS_H_