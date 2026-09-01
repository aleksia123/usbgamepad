#include <stddef.h>

#include "pico/unique_id.h"

#include "usb_serial.h"

const char *usb_serial_string(void)
{
    static char serial[2 * PICO_UNIQUE_BOARD_ID_SIZE_BYTES + 1];
    static char const *cached = NULL;

    if (!cached)
    {
        pico_get_unique_board_id_string(serial, sizeof(serial));
        cached = serial;
    }

    return cached;
}
