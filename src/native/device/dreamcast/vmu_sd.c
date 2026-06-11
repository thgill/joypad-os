// vmu_sd.c - SD card persistence for Dreamcast VMU emulation
// SPDX-License-Identifier: Apache-2.0
//
// Loads DC_1.VMU from the SD card into vmu_ram at startup.
// Flushes vmu_ram back to DC_1.VMU after VMU_SD_WRITEBACK_MS of write
// inactivity. All SD operations run on Core 0 via vmu_task().
//
// The dirty flag is set by Core 1 (vmu_handle_write_complete) via a
// volatile variable — no mutex needed since writes are idempotent and
// the only consequence of a torn read is a slightly early or late flush.

#include "vmu_sd.h"
#include "dreamcast_display.h"
#include "vmu.h"
#include "pico/stdlib.h"
#include <stdio.h>

// SD/fatfs are only used when CONFIG_SD is set. Targets that store the VMU in
// QSPI instead (CONFIG_VMU_QSPI, no CONFIG_SD) compile this file as a no-op and
// must not pull in ff.h / the SD headers (their include dirs aren't on those
// targets — see the joypad_dc vs joypad_dc_rp2040zero CMake targets).
#ifdef CONFIG_SD
#include "ff.h"
#include "core/services/sd/sd.h"
#include "platform/platform_sd.h"
#endif

// The 128KB VMU image buffer and dirty flag — defined in vmu.c.
extern uint8_t vmu_ram[];
extern volatile bool vmu_dirty_flag;

// Volatile writeback timer — updated when dirty flag is set
static volatile uint32_t vmu_last_write_ms = 0;

static bool sd_available = false;

// ---------------------------------------------------------------------------

bool vmu_sd_init(void)
{
#ifndef CONFIG_SD
    printf("[vmu-sd] CONFIG_SD not set — SD storage disabled\n");
    return false;
#else
    static const platform_sd_config_t sd_cfg = {
        .spi_inst    = SD_SPI_INST,
        .sck_pin     = SD_SCK_PIN,
        .mosi_pin    = SD_MOSI_PIN,
        .miso_pin    = SD_MISO_PIN,
        .cs_pin      = SD_CS_PIN,
        .cd_pin      = PLATFORM_SD_NO_CD,
        .init_freq_hz = 200000,
        .run_freq_hz  = 12500000,
    };

    platform_sd_t dev = platform_sd_init(&sd_cfg);
    if (!dev || !sd_init(dev)) {
        printf("[vmu-sd] SD not available — using pre-formatted RAM image\n");
        return false;
    }

    printf("[vmu-sd] SD mounted (%llu MB free)\n",
           (unsigned long long)(sd_free_bytes() / (1024ULL * 1024ULL)));

    // Try to load existing VMU image using chunked reads.
    // Some SD cards fail on a single 128KB f_read(); 512-byte chunks
    // match the SD sector size and are universally reliable.
    int got = 0;
    {
        FIL f;
        if (f_open(&f, VMU_SD_FILENAME, FA_READ) == FR_OK) {
            uint8_t* dst = (uint8_t*)vmu_ram;
            size_t remaining = VMU_IMAGE_SIZE;
            size_t offset = 0;
            bool ok = true;
            while (remaining > 0 && ok) {
                UINT chunk = (UINT)(remaining > 512 ? 512 : remaining);
                UINT rd = 0;
                if (f_read(&f, dst + offset, chunk, &rd) != FR_OK || rd == 0) {
                    ok = false;
                } else {
                    offset += rd;
                    remaining -= rd;
                }
            }
            f_close(&f);
            if (ok) got = (int)VMU_IMAGE_SIZE;
        }
    }
    if (got == (int)VMU_IMAGE_SIZE) {
        printf("[vmu-sd] Loaded %s\n", VMU_SD_FILENAME);
    } else {
        // File missing or wrong size — write the pre-formatted image.
        printf("[vmu-sd] %s not found — creating from pre-formatted image\n",
               VMU_SD_FILENAME);
        if (sd_write_file(VMU_SD_FILENAME, vmu_ram, VMU_IMAGE_SIZE) != 0) {
            printf("[vmu-sd] Failed to create %s\n", VMU_SD_FILENAME);
            return false;
        }
        printf("[vmu-sd] Created %s\n", VMU_SD_FILENAME);
    }

    sd_available = true;
    return true;
#endif
}

// Mount SD card without loading VMU file — used when QSPI is primary backend.
// QSPI image stays intact; first dirty writeback will sync QSPI -> SD.
bool vmu_sd_mount(void)
{
#ifndef CONFIG_SD
    return false;
#else
    static const platform_sd_config_t sd_cfg = {
        .spi_inst    = SD_SPI_INST,
        .sck_pin     = SD_SCK_PIN,
        .mosi_pin    = SD_MOSI_PIN,
        .miso_pin    = SD_MISO_PIN,
        .cs_pin      = SD_CS_PIN,
        .cd_pin      = PLATFORM_SD_NO_CD,
        .init_freq_hz = 200000,
        .run_freq_hz  = 12500000,
    };

    platform_sd_t dev = platform_sd_init(&sd_cfg);
    if (!dev || !sd_init(dev)) {
        printf("[vmu-sd] SD not available for backup\n");
        return false;
    }

    printf("[vmu-sd] SD mounted for backup (%llu MB free)\n",
           (unsigned long long)(sd_free_bytes() / (1024ULL * 1024ULL)));

    // Mark dirty so first vmu_sd_task() call syncs QSPI image to SD
    sd_available = true;
    vmu_dirty_flag = true;
    // Update display SD label
    char sd_label[16];
    uint64_t mb = sd_free_bytes() / (1024ULL * 1024ULL);
    if (mb >= 1024)
        snprintf(sd_label, sizeof(sd_label), "SD:%uGB", (unsigned)(mb / 1024));
    else
        snprintf(sd_label, sizeof(sd_label), "SD:%uMB", (unsigned)mb);
    dc_display_set_sd_label(sd_label);
    return true;
#endif
}

void vmu_sd_task(void)
{
#ifdef CONFIG_SD
    if (!sd_available) return;

    // Check if Core 1 has flagged a write
    if (!vmu_dirty_flag) return;

    // Start/update the writeback timer
    uint32_t now = to_ms_since_boot(get_absolute_time());
    if (vmu_last_write_ms == 0) vmu_last_write_ms = now;
    if (now - vmu_last_write_ms < VMU_SD_WRITEBACK_MS) return;

    // Timer expired — flush to SD and clear flag
    vmu_dirty_flag = false;
    vmu_last_write_ms = 0;
    if (sd_write_file(VMU_SD_FILENAME, vmu_ram, VMU_IMAGE_SIZE) != 0) {
        printf("[vmu-sd] Write failed — will retry\n");
        vmu_dirty_flag = true;  // retry next cycle
    }
#endif
}

void vmu_sd_mark_dirty(void)
{
    // Called externally if needed — Core 1 sets vmu_dirty_flag directly
    vmu_dirty_flag = true;
    vmu_last_write_ms = 0;
}

bool vmu_sd_is_available(void)
{
    return sd_available;
}
