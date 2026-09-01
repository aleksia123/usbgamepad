#include <string.h>

#include "pico/time.h"
#include "pico/sync.h"

#include "app_transport.h"
#include "xinput_host.h"   // XINPUT_GAMEPAD_* bit values
#include "Gamepad.h"        // gamepad(0)

//--------------------------------------------------------------------+
// HID report descriptor: two raw 12-byte reports (vendor usage page)
// under report IDs 0x11 (Input) and 0x10 (Output). Length MUST match
// wDescriptorLength in lib/tusb_gamepad/src/descriptors/XInputDescriptors.h's
// xinput_configuration_descriptor[] HID descriptor block.
//--------------------------------------------------------------------+

static const uint8_t app_transport_hid_report_descriptor[] =
{
    0x06, 0x00, 0xFF,             // Usage Page (Vendor Defined 0xFF00)
    0x09, 0x01,                   // Usage (0x01)
    0xA1, 0x01,                   // Collection (Application)
      0x85, APP_TRANSPORT_REPORT_ID_IN,  //   Report ID (0x11)
      0x09, 0x02,                 //   Usage (0x02)
      0x15, 0x00,                 //   Logical Minimum (0)
      0x26, 0xFF, 0x00,           //   Logical Maximum (255)
      0x75, 0x08,                 //   Report Size (8)
      0x95, 0x0C,                 //   Report Count (12)
      0x81, 0x02,                 //   Input (Data,Var,Abs)
      0x85, APP_TRANSPORT_REPORT_ID_OUT, //   Report ID (0x10)
      0x09, 0x03,                 //   Usage (0x03)
      0x75, 0x08,                 //   Report Size (8)
      0x95, 0x0C,                 //   Report Count (12)
      0x91, 0x02,                 //   Output (Data,Var,Abs)
    0xC0,                         // End Collection
};

_Static_assert(sizeof(app_transport_hid_report_descriptor) == 33,
               "update wDescriptorLength in XInputDescriptors.h to match");

uint8_t const *app_transport_report_descriptor(void)
{
    return app_transport_hid_report_descriptor;
}

uint16_t app_transport_report_descriptor_len(void)
{
    return sizeof(app_transport_hid_report_descriptor);
}

//--------------------------------------------------------------------+
// State
//--------------------------------------------------------------------+

static critical_section_t physical_lock;
static critical_section_t output_lock;

static app_controller_payload_t physical_state;
static app_controller_payload_t output_state;

static app_controller_payload_t last_sent_physical;
static bool                     have_sent_physical;

static absolute_time_t last_output_report_time;

void app_transport_init(void)
{
    critical_section_init(&physical_lock);
    critical_section_init(&output_lock);

    memset(&physical_state, 0, sizeof(physical_state));
    memset(&output_state, 0, sizeof(output_state));
    memset(&last_sent_physical, 0, sizeof(last_sent_physical));
    have_sent_physical = false;

    // Force the watchdog to be expired immediately: output starts neutral
    // until the app sends its first valid report.
    last_output_report_time = nil_time;
}

void app_transport_publish_physical(const app_controller_payload_t *state)
{
    critical_section_enter_blocking(&physical_lock);
    physical_state = *state;
    critical_section_exit(&physical_lock);
}

void app_transport_clear_physical(void)
{
    critical_section_enter_blocking(&physical_lock);
    memset(&physical_state, 0, sizeof(physical_state));
    critical_section_exit(&physical_lock);
}

void app_transport_force_neutral(void)
{
    last_output_report_time = nil_time;
}

void app_transport_on_set_report(uint8_t report_id, hid_report_type_t report_type,
                                  uint8_t const *buffer, uint16_t bufsize)
{
    (void)report_type;

    // Two delivery paths reach here with different framing:
    //   - Interrupt OUT endpoint (how Windows sends output reports for a
    //     collection that has one): report_id == 0 and the real report ID is
    //     still the first byte of buffer, so bufsize == 1 + payload.
    //   - Control SET_REPORT: TinyUSB has already split the ID out, so
    //     report_id == 0x10 and buffer is the bare payload.
    // Normalize the interrupt-OUT case to the stripped form.
    if (report_id == 0 && bufsize >= 1)
    {
        report_id = buffer[0];
        buffer++;
        bufsize--;
    }

    if (report_id != APP_TRANSPORT_REPORT_ID_OUT) return;
    if (bufsize != sizeof(app_controller_payload_t)) return;

    critical_section_enter_blocking(&output_lock);
    memcpy(&output_state, buffer, sizeof(app_controller_payload_t));
    critical_section_exit(&output_lock);

    last_output_report_time = get_absolute_time();
}

uint16_t app_transport_on_get_report(uint8_t report_id, hid_report_type_t report_type,
                                      uint8_t *buffer, uint16_t reqlen)
{
    (void)report_type;

    if (report_id != APP_TRANSPORT_REPORT_ID_IN) return 0;
    if (reqlen < sizeof(app_controller_payload_t)) return 0;

    critical_section_enter_blocking(&physical_lock);
    memcpy(buffer, &physical_state, sizeof(app_controller_payload_t));
    critical_section_exit(&physical_lock);

    return sizeof(app_controller_payload_t);
}

//--------------------------------------------------------------------+
// Core 0 task
//--------------------------------------------------------------------+

static void apply_output_to_gamepad(const app_controller_payload_t *payload)
{
    Gamepad *gp = gamepad(0);
    if (!gp) return;

    GamepadButtons btns = {0};
    if (payload->buttons & XINPUT_GAMEPAD_DPAD_UP)       btns.up    = 1;
    if (payload->buttons & XINPUT_GAMEPAD_DPAD_DOWN)     btns.down  = 1;
    if (payload->buttons & XINPUT_GAMEPAD_DPAD_LEFT)     btns.left  = 1;
    if (payload->buttons & XINPUT_GAMEPAD_DPAD_RIGHT)    btns.right = 1;
    if (payload->buttons & XINPUT_GAMEPAD_A)             btns.a = 1;
    if (payload->buttons & XINPUT_GAMEPAD_B)             btns.b = 1;
    if (payload->buttons & XINPUT_GAMEPAD_X)             btns.x = 1;
    if (payload->buttons & XINPUT_GAMEPAD_Y)             btns.y = 1;
    if (payload->buttons & XINPUT_GAMEPAD_LEFT_SHOULDER) btns.lb = 1;
    if (payload->buttons & XINPUT_GAMEPAD_RIGHT_SHOULDER)btns.rb = 1;
    if (payload->buttons & XINPUT_GAMEPAD_LEFT_THUMB)    btns.l3 = 1;
    if (payload->buttons & XINPUT_GAMEPAD_RIGHT_THUMB)   btns.r3 = 1;
    if (payload->buttons & XINPUT_GAMEPAD_BACK)          btns.back  = 1;
    if (payload->buttons & XINPUT_GAMEPAD_START)         btns.start = 1;
    if (payload->buttons & XINPUT_GAMEPAD_GUIDE)         btns.sys   = 1;
    if (payload->buttons & XINPUT_GAMEPAD_SHARE)         btns.misc  = 1;

    GamepadTriggers trig = { .l = payload->left_trigger, .r = payload->right_trigger };
    GamepadJoysticks joy = {
        .lx = payload->thumb_lx, .ly = payload->thumb_ly,
        .rx = payload->thumb_rx, .ry = payload->thumb_ry,
    };

    gp->buttons   = btns;
    gp->triggers  = trig;
    gp->joysticks = joy;
}

void app_transport_core0_task(void)
{
    static const app_controller_payload_t neutral = {0};

    // --- watchdog: fall back to neutral once output_state goes stale ---
    app_controller_payload_t local_output;
    bool expired = is_nil_time(last_output_report_time) ||
                   absolute_time_diff_us(last_output_report_time, get_absolute_time())
                       > (APP_TRANSPORT_WATCHDOG_MS * 1000);

    if (expired)
    {
        local_output = neutral;
    }
    else
    {
        critical_section_enter_blocking(&output_lock);
        local_output = output_state;
        critical_section_exit(&output_lock);
    }

    apply_output_to_gamepad(&local_output);

    // --- send physical_state as report 0x11 if it changed ---
    app_controller_payload_t local_physical;
    critical_section_enter_blocking(&physical_lock);
    local_physical = physical_state;
    critical_section_exit(&physical_lock);

    if (!have_sent_physical || memcmp(&local_physical, &last_sent_physical, sizeof(local_physical)) != 0)
    {
        if (tud_hid_n_ready(0))
        {
            if (tud_hid_n_report(0, APP_TRANSPORT_REPORT_ID_IN, &local_physical, sizeof(local_physical)))
            {
                last_sent_physical = local_physical;
                have_sent_physical = true;
            }
        }
    }
}
