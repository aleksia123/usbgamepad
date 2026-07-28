// pad_config_store.h - flash persistence for a single pad_config_t.
//
// Reserves the LAST flash sector (see CMakeLists.txt's PAD_CONFIG_FLASH_SIZE_BYTES,
// which feeds both this offset and the linker's flash region length, so the
// two can never drift apart). Writes go through pico-sdk's flash_safe_execute(),
// which pauses core1 (usbh_task) for the duration - see flash_safe_execute_core_init()
// in main.c, without which writes are refused rather than unsafe.
//
// Fails safe in both directions: a missing/corrupt/never-written blob makes
// pad_config_store_load() return false and leave the caller's struct
// (already the compiled-in PAD_CONFIG_DEFAULTS) untouched; a failed
// pad_config_store_save() is logged and skipped, never retried or asserted -
// worst case an edit doesn't survive the next power cycle.
#ifndef PAD_CONFIG_STORE_H
#define PAD_CONFIG_STORE_H
#include <stdbool.h>
#include "pad_config.h"

// Reads and validates the stored blob. Returns true and fills *out only if
// the magic/version/checksum all check out; otherwise returns false and
// leaves *out untouched.
bool pad_config_store_load(pad_config_t* out);

// Erases and reprograms the reserved sector with *cfg. Safe to call from
// core0 only (asserts nothing - just returns on failure).
int pad_config_store_save(const pad_config_t* cfg);

#endif // PAD_CONFIG_STORE_H