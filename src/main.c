#include <stdio.h>
#include <stdlib.h>
#include "pico/stdlib.h"
#include "pico/multicore.h"
#include "hardware/gpio.h"
#include "hardware/clocks.h"

#include "tusb.h"
#include "host/usbh.h"
#include "bsp/board_api.h"
#include "pio_usb.h"
#include "tusb_gamepad.h"
#include "hardware/timer.h"
#include "pico/flash.h"
#include "boot_mode.h"
#include "cdc_config.h"
#include "pad_config.h"
#include "pad_config_store.h"

// ------------------------------------------------------------------ //
//  Board selection
//  Values don't matter as long as they're unique.
// ------------------------------------------------------------------ //
#define PI_PICO          1
#define ADAFRUIT_FEATHER 2
#define RP2350_USB_A     3   // Waveshare RP2350-USB-A

// >>> Choose your board here <<<
#define OGXM_BOARD RP2350_USB_A
// ----------------------------- //

#if   OGXM_BOARD == PI_PICO
    #define PIO_USB_DP_PIN  0          // D+ on GPIO0, D- on GPIO1
    #define SYS_CLOCK_KHZ   120000

#elif OGXM_BOARD == ADAFRUIT_FEATHER
    #define PIO_USB_DP_PIN  16         // D+ on GPIO16, D- on GPIO17
    #define VCC_EN_PIN      18         // Feather needs VBUS enabled on the host port
    #define SYS_CLOCK_KHZ   120000

#elif OGXM_BOARD == RP2350_USB_A
    #define PIO_USB_DP_PIN  12
    #define SYS_CLOCK_KHZ   240000     // RP2350 PIO USB host runs at 240 MHz

#else
    #error "No board selected"
#endif

// define pio config
#define PIO_USB_CONFIG {    \
    PIO_USB_DP_PIN,         \
    PIO_USB_TX_DEFAULT,     \
    PIO_SM_USB_TX_DEFAULT,  \
    PIO_USB_DMA_TX_DEFAULT, \
    PIO_USB_RX_DEFAULT,     \
    PIO_SM_USB_RX_DEFAULT,  \
    PIO_SM_USB_EOP_DEFAULT, \
    NULL,                   \
    PIO_USB_DEBUG_PIN_NONE, \
    PIO_USB_DEBUG_PIN_NONE, \
    false,                  \
    PIO_USB_PINOUT_DPDM }

extern void hid_app_task(void); // see hid_app.c

void usbh_task()
{
    #ifdef VCC_EN_PIN // Board needs VCC enabled on the USB host port
        gpio_init(VCC_EN_PIN);
        gpio_set_dir(VCC_EN_PIN, GPIO_OUT);
        gpio_put(VCC_EN_PIN, 1);
    #endif

    // Required before any flash_safe_execute() call (pad_config_store.c) can
    // succeed: it lets core0 safely lock out this core while it erases/
    // programs flash. Without it, flash writes are refused, not unsafe.
    flash_safe_execute_core_init();

    pio_usb_configuration_t pio_cfg = PIO_USB_CONFIG;
    tuh_configure(BOARD_TUH_RHPORT, TUH_CFGID_RPI_PIO_USB_CONFIGURATION, &pio_cfg);

    tuh_init(BOARD_TUH_RHPORT);

    while (1)
    {
        tuh_task();
        hid_app_task(); // updates gamepad with controller data
    }
}

int main(void)
{
    set_sys_clock_khz(SYS_CLOCK_KHZ, true);

    // RP2350 timer debug-pause bug: timers can stall even outside a debugger
    // in some configurations, breaking the PIO USB SOF alarm.
    // https://forums.raspberrypi.com/viewtopic.php?t=363914
    timer_hw->dbgpause = 0;

    board_init();

    // flash_safe_execute() (used by pad_config_store_save) must be called from
    // a core that has registered via flash_safe_execute_core_init(). Saves run
    // from THIS core (core0) inside cdc_config_task(), so core0 must init here
    // -- before the load and before core1 launches. core1 does its own init at
    // the top of usbh_task(); BOTH are required (the writer pauses the peer).
    flash_safe_execute_core_init();

    // Load any flash-persisted stick/trigger tuning before anything else, so
    // it's live regardless of which InputMode we end up booting into.
    pad_config_store_load(&g_pad_config);

    enum InputMode input_mode = boot_mode_on_startup();

    init_tusb_gamepad(input_mode); // initialize usb device with chosen input mode
    cdc_config_init(input_mode);   // arms the config-mode grace timer iff USBSERIAL

    multicore_reset_core1();
    multicore_launch_core1(usbh_task); // usb host stack on core 1

    while (1)
    {
        tud_task();
        tusb_gamepad_task();
        cdc_config_task();
    }

    return 0;
}