// dreamcast_display.h - OLED display state machine for USB2DC adapter
// SPDX-License-Identifier: Apache-2.0
//
// Drives a 128x64 SSD1306 OLED showing VMU LCD graphics and adapter status.
// Display is entirely optional — all functions are no-ops if no display is
// detected at I2C address OLED_I2C_ADDR. Call dc_display_task() every Core 0
// loop iteration; it uses display_flush_step() for non-blocking I2C transfer.

#ifndef DREAMCAST_DISPLAY_H
#define DREAMCAST_DISPLAY_H

#include <stdint.h>
#include <stdbool.h>

// Display states
typedef enum {
    DC_DISPLAY_BOOT = 0,    // Boot splash: logo, version, SD info
    DC_DISPLAY_IDLE,        // No game: logo + slot info
    DC_DISPLAY_GAMEPLAY,    // Game running: VMU LCD + game info
    DC_DISPLAY_WRITE,       // VMU write pending: save indicator
    DC_DISPLAY_SLOT_SWITCH, // Manual slot selection UI
    DC_DISPLAY_GAMEID,      // GameID detection / VMU loading
    DC_DISPLAY_NO_SD,       // No SD card present
} dc_display_state_t;

// Start boot splash timer — call when Maple Bus goes live.
void dc_display_start_boot(void);

// Initialize display — call from dreamcast_init() or app_init().
// No-op if OLED_I2C_INST not defined or display not detected.
void dc_display_preinit(void);  // Call from app_init() — I2C hardware only, no display commands
void dc_display_init(void);

// Periodic task — call every Core 0 loop iteration.
// Handles incremental I2C flush and state transitions.
void dc_display_task(void);

// Set display state explicitly
void dc_display_set_state(dc_display_state_t state);

// Update VMU LCD bitmap (192 bytes, 48x32 pixels, column-major LSB=top).
// Called when the DC writes LCD data to the VMU. Core 1 safe — uses
// a double buffer with a volatile dirty flag.
void dc_display_set_vmu_lcd(const uint8_t* data);

// Set game name shown on display (from GameID or save file header).
// Truncated to fit display width; marquee scrolls if too long.
void dc_display_set_game_name(const char* name);

// Set active slot number (1-based)
void dc_display_set_slot(uint8_t slot);

// Set VMU block usage for fill bar
void dc_display_set_blocks(uint16_t used, uint16_t total);

// Set SD card size string (e.g. "SD:32GB" or "SD:NONE")
void dc_display_set_sd_label(const char* label);

// Returns true if display hardware was detected and initialized
bool dc_display_needs_update(void);
bool dc_display_is_present(void);

#endif // DREAMCAST_DISPLAY_H
