#include "pad_config_store.h"
#include <stdio.h>
#include <string.h>
#include <stddef.h>
#include "pico/flash.h"
#include "hardware/flash.h"
#include "hardware/sync.h"
#include "hardware/regs/addressmap.h" // XIP_BASE

#ifndef PAD_CONFIG_FLASH_SIZE_BYTES
#error "PAD_CONFIG_FLASH_SIZE_BYTES must be supplied by CMake (see CMakeLists.txt)"
#endif

// Reserve the last sector of flash. Derived from the same CMake variable that
// sizes the linker's FLASH region, so this offset can never drift from where
// the linker actually places code/rodata.
#define PAD_CONFIG_FLASH_OFFSET (PAD_CONFIG_FLASH_SIZE_BYTES - FLASH_SECTOR_SIZE)

#define PAD_CONFIG_BLOB_MAGIC   0x50434647u // 'PCFG'
#define PAD_CONFIG_BLOB_VERSION 6u  // struct layout changed: added dither stage (older blobs fall back to defaults)

typedef struct {
    uint32_t     magic;
    uint16_t     version;
    uint16_t     reserved;
    pad_config_t cfg;
    uint32_t     checksum;
} pad_config_flash_blob_t;

static uint32_t blob_checksum(const pad_config_flash_blob_t* b)
{
    // Simple FNV-1a over everything up to (not including) the checksum field
    // itself - good enough to catch torn/partial writes and bit rot, not a
    // cryptographic property we need here.
    uint32_t hash = 0x811c9dc5u;
    const uint8_t* p = (const uint8_t*)b;
    size_t len = offsetof(pad_config_flash_blob_t, checksum);
    for (size_t i = 0; i < len; i++) {
        hash ^= p[i];
        hash *= 0x01000193u;
    }
    return hash;
}

bool pad_config_store_load(pad_config_t* out)
{
    const pad_config_flash_blob_t* blob =
        (const pad_config_flash_blob_t*)(XIP_BASE + PAD_CONFIG_FLASH_OFFSET);

    if (blob->magic != PAD_CONFIG_BLOB_MAGIC) {
        printf("[flash] load: bad magic %08lx\n", (unsigned long)blob->magic);
        return false;
    }
    if (blob->version != PAD_CONFIG_BLOB_VERSION) {
        printf("[flash] load: version %u != %u (defaults)\n",
               (unsigned)blob->version, (unsigned)PAD_CONFIG_BLOB_VERSION);
        return false;
    }
    if (blob_checksum(blob) != blob->checksum) {
        printf("[flash] load: checksum mismatch (defaults)\n");
        return false;
    }

    *out = blob->cfg;

    // A stored blob is untrusted input: it may have been written by an older
    // build whose defaults allowed an enabled stage to sit at 0 (a stage that
    // is "on" but mathematically a no-op - the exact state that made toggles
    // look dead in the editor). Clamp it here so the rest of the firmware
    // never has to consider that case.
    if (pad_config_sanitize(out)) {
        printf("[flash] load: OK (values clamped to usable range)\n");
        return true;
    }

    printf("[flash] load: OK\n");
    return true;
}

typedef struct {
    uint8_t page[FLASH_PAGE_SIZE];
} save_params_t;

static void do_erase_and_program(void* param)
{
    save_params_t* p = (save_params_t*)param;
    flash_range_erase(PAD_CONFIG_FLASH_OFFSET, FLASH_SECTOR_SIZE);
    flash_range_program(PAD_CONFIG_FLASH_OFFSET, p->page, FLASH_PAGE_SIZE);
}

int pad_config_store_save(const pad_config_t* cfg)
{
    static save_params_t params; // static: keep off the small core0 stack

    memset(params.page, 0xFF, sizeof(params.page));

    pad_config_flash_blob_t blob = {
        .magic = PAD_CONFIG_BLOB_MAGIC,
        .version = PAD_CONFIG_BLOB_VERSION,
        .reserved = 0,
        .cfg = *cfg,
    };
    blob.checksum = blob_checksum(&blob);

    memcpy(params.page, &blob, sizeof(blob));

    // Returns PICO_OK (0) on success. Common failures:
    //   PICO_ERROR_NOT_PERMITTED (-19): the CALLING core never ran
    //     flash_safe_execute_core_init(), or the peer core isn't registered.
    //   PICO_ERROR_TIMEOUT (-1 / -5 depending on SDK): peer lockout didn't
    //     complete within the deadline.
    // On failure the RAM copy of g_pad_config stays correct for this session;
    // only the flash round-trip is missed (so the edit won't survive reboot).
    int rc = flash_safe_execute(do_erase_and_program, &params, 1000);
    printf("[flash] save rc=%d (0=OK; -19=not_permitted, core not init'd)\n", rc);
    return rc;
}