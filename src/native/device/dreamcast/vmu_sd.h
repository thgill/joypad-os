// vmu_sd.h - SD card persistence for Dreamcast VMU emulation
// SPDX-License-Identifier: Apache-2.0
//
// Filename on the FAT volume (8.3, root directory).
// Compatible with Flycast, Redream, and other DC emulators.
#pragma once
#include <stdint.h>
#include <stdbool.h>

#define VMU_SD_FILENAME     "DC_1.VMU"

// Flush delay after last write (ms). Balances SD wear vs data safety.
#define VMU_SD_WRITEBACK_MS  1000

// Initialize SD and load DC_1.VMU into vmu_ram.
// Creates the file from the pre-formatted image if not found.
// Call once from Core 0 at startup. Returns true on success.
bool vmu_sd_init(void);
bool vmu_sd_mount(void);  // Mount SD without loading VMU file (QSPI-primary builds)

// Periodic flush task — call from Core 0 main loop (via vmu_task()).
void vmu_sd_task(void);

// Mark VMU RAM dirty. Safe to call from either core.
void vmu_sd_mark_dirty(void);

// Returns true if SD card is mounted and DC_1.VMU was loaded.
bool vmu_sd_is_available(void);
