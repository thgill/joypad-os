// dreamcast_display.c - OLED display state machine for USB2DC adapter
// SPDX-License-Identifier: Apache-2.0
//
// Layout (128x64 landscape):
//   Left panel  x=0..63  : VMU LCD graphics (48x32 scaled 1x, centered)
//   Divider     x=64     : vertical line
//   Right panel x=66..127: status info (slot, game, fill bar, SD label)
//
// All display calls go through display_flush_step() — one I2C page per
// Core 0 iteration, so no single call blocks for more than ~1ms.

#include "dreamcast_display.h"
#include "hardware/i2c.h"
#include "hardware/gpio.h"

#ifdef OLED_I2C_INST

#include "core/services/display/display.h"
#include "pico/stdlib.h"
#include <string.h>
#include <stdio.h>

// ============================================================================
// Layout constants
// ============================================================================

#define DIV_X       64      // Divider x position
#define RX          66      // Right panel x start
#define RW          62      // Right panel width (128 - 66)
#define VMU_LCD_X   8       // VMU LCD left offset (centers 48px in 64px)
#define VMU_LCD_Y   16      // VMU LCD top offset (centers 32px in 64px)

// Right panel row y positions (all fit within 0-63)
#define R_TITLE_Y   0
#define R_DIV1_Y    9
#define R_SLOT_Y    11
#define R_GAME_Y    20
#define R_GID_Y     29
#define R_DIV2_Y    37
#define R_BAR_Y     39
#define R_BLKS_Y    44
#define R_DIV3_Y    52
#define R_BOTTOM_Y  55

// ============================================================================
// State
// ============================================================================

static dc_display_state_t current_state = DC_DISPLAY_BOOT;
static bool display_present = false;
static bool needs_redraw = true;
static bool needs_update = false;  // Set when state or LCD data changes
static uint32_t boot_start_ms = 0;
#define BOOT_DURATION_MS 3000

// VMU LCD data (Core 1 sets pointer + flag, Core 0 copies)
static uint8_t vmu_lcd_buf[192];
static volatile const uint8_t* vmu_lcd_src = NULL;  // Core 1 sets this
static volatile bool vmu_lcd_dirty = false;

// Status fields
static char game_name[32] = "NO GAME";
static uint8_t active_slot = 1;
static uint16_t blocks_used = 0;
static uint16_t blocks_total = 200;
static char sd_label[16] = "SD:NONE";

// ============================================================================
// Init
// ============================================================================

void dc_display_init(void) {
#ifdef OLED_I2C_INST
    display_i2c_config_t cfg = {
        .i2c_inst = OLED_I2C_INST,
        .pin_sda  = OLED_I2C_SDA_PIN,
        .pin_scl  = OLED_I2C_SCL_PIN,
        .addr     = OLED_I2C_ADDR,
    };
    display_init_ssd1306_i2c(&cfg);
    display_present = display_is_initialized();
    if (!display_present) {
        printf("[dc-display] No OLED detected at I2C 0x%02X — display disabled\n",
               OLED_I2C_ADDR);
        return;
    }
    printf("[dc-display] OLED initialized\n");
    display_clear();
    display_update();
    // Boot timer starts when Maple Bus goes live (dc_display_start_boot)
    boot_start_ms = 0;
    current_state = DC_DISPLAY_BOOT;
    needs_redraw = false;  // Don't draw until boot timer starts
#endif
}

// ============================================================================
// Drawing helpers
// ============================================================================

// Draw text clipped to maxW pixels wide
static void draw_text_clipped(uint8_t x, uint8_t y, const char* text, uint8_t maxW) {
    // display_text uses 6px per char; calculate max chars that fit
    uint8_t max_chars = maxW / 6;
    char buf[22];
    uint8_t len = strlen(text);
    if (len <= max_chars) {
        display_text(x, y, text);
    } else {
        // Truncate with trailing dot
        strncpy(buf, text, max_chars - 1);
        buf[max_chars - 1] = '.';
        buf[max_chars] = '\0';
        display_text(x, y, buf);
    }
}

// Draw right info panel
static void draw_right_panel(const char* title, const char* slot_str,
                              const char* game, const char* gid,
                              uint16_t used, uint16_t total,
                              const char* status, const char* sd) {
    draw_text_clipped(RX, R_TITLE_Y, title, RW);
    display_hline(RX, R_DIV1_Y, RW);
    draw_text_clipped(RX, R_SLOT_Y, slot_str, RW);
    draw_text_clipped(RX, R_GAME_Y, game, RW);
    if (gid && gid[0]) draw_text_clipped(RX, R_GID_Y, gid, RW);
    display_hline(RX, R_DIV2_Y, RW);
    // Fill bar
    display_rect(RX, R_BAR_Y, RW, 4);
    uint8_t fill = total > 0 ? (uint8_t)((uint32_t)(RW - 2) * used / total) : 0;
    if (fill > 0) display_fill_rect(RX + 1, R_BAR_Y + 1, fill, 2, true);
    // Block count
    char blk[16];
    snprintf(blk, sizeof(blk), "%u/%uBLK", used, total);
    draw_text_clipped(RX, R_BLKS_Y, blk, RW);
    display_hline(RX, R_DIV3_Y, RW);
    // Bottom row: status left, SD right
    if (status && status[0]) {
        uint8_t sd_w = sd ? (uint8_t)(strlen(sd) * 6) : 0;
        draw_text_clipped(RX, R_BOTTOM_Y, status, RW - sd_w - 2);
    }
    if (sd && sd[0]) {
        uint8_t sd_w = (uint8_t)(strlen(sd) * 6);
        uint8_t sd_x = (RX + RW - sd_w > RX) ? RX + RW - sd_w - 1 : RX;
        display_text(sd_x, R_BOTTOM_Y, sd);
    }
}

// Draw VMU LCD area (left panel)
static void draw_vmu_lcd_area(void) {
    // Border
    display_rect(0, 0, DIV_X, 64);
    // VMU LCD bitmap: 48x32, column-major, 6 bytes per column
    // Each byte = 8 vertical pixels, LSB=top
    for (uint8_t col = 0; col < 48; col++) {
        for (uint8_t byte_idx = 0; byte_idx < 4; byte_idx++) {
            uint8_t b = vmu_lcd_buf[col * 4 + byte_idx];
            for (uint8_t bit = 0; bit < 8; bit++) {
                if (b & (1u << bit)) {
                    uint8_t px = VMU_LCD_X + col;
                    uint8_t py = VMU_LCD_Y + byte_idx * 8 + bit;
                    if (px < DIV_X && py < 64)
                        display_pixel(px, py, true);
                }
            }
        }
    }
}

// Draw idle/logo in VMU area
static void draw_vmu_logo(void) {
    display_rect(0, 0, DIV_X, 64);
    display_text(4, 6,  "JOYPAD");
    display_text(4, 16, "OS");
    display_hline(4, 46, 56);
    display_text(4, 50, "V2.1.1");
}

// Slot string helper
static void slot_str(char* buf, uint8_t slot) {
    snprintf(buf, 12, "SLOT %u", slot);
}

// ============================================================================
// State renderers
// ============================================================================

static void render_boot(void) {
    display_clear();
    display_update();  // Push clear immediately before drawing
    display_clear();
    // Full screen: logo left, info right
    display_vline(DIV_X, 0, 64);
    // Left: logo
    display_text(4,  4,  "JOYPAD");
    display_text(4,  14, "OS");
    display_hline(0, 24, DIV_X);
    display_text(2,  27, "V2.1.1");
    // Right: system info
    display_text(RX, 0,  "JOYPAD OS");
    display_text(RX, 10, "V2.1.1");
    display_hline(RX, 20, RW);
    display_text(RX, 22, "SD CARD");
    draw_text_clipped(RX, 32, sd_label, RW);
    display_hline(RX, 43, RW);
    char slot_buf[12];
    slot_str(slot_buf, active_slot);
    draw_text_clipped(RX, 45, slot_buf, RW);
    display_text(RX, 55, "QSPI OK");
}

static void render_idle(void) {
    display_clear();
    display_vline(DIV_X, 0, 64);
    draw_vmu_logo();
    char slot_buf[12];
    slot_str(slot_buf, active_slot);
    draw_right_panel("JOYPAD OS", slot_buf, "NO GAME", NULL,
                     blocks_used, blocks_total, NULL, sd_label);
}

static void render_gameplay(void) {
    display_clear();
    display_vline(DIV_X, 0, 64);
    draw_vmu_lcd_area();
    char slot_buf[12];
    slot_str(slot_buf, active_slot);
    draw_right_panel(game_name, slot_buf, game_name, NULL,
                     blocks_used, blocks_total, NULL, sd_label);
}

static void render_write(void) {
    display_clear();
    display_vline(DIV_X, 0, 64);
    draw_vmu_lcd_area();
    char slot_buf[12];
    slot_str(slot_buf, active_slot);
    draw_right_panel(game_name, slot_buf, game_name, NULL,
                     blocks_used, blocks_total, "SAVING...", sd_label);
}

static void render_slot_switch(void) {
    display_clear();
    display_vline(DIV_X, 0, 64);
    // Left: slot select prompt
    display_rect(0, 0, DIV_X, 64);
    display_text(4, 6,  "SLOT");
    display_text(4, 20, "SELECT");
    display_hline(4, 38, 56);
    display_text(4, 42, "L1 / R1");
    // Right: slot list placeholder (populated when multi-slot is implemented)
    draw_text_clipped(RX, 0, "SEL SLOT", RW);
    display_hline(RX, 9, RW);
    char slot_buf[12];
    slot_str(slot_buf, active_slot);
    // Highlight current slot
    display_fill_rect(RX, 11, RW, 10, true);
    display_text(RX + 2, 13, slot_buf);
}

static void render_gameid(void) {
    display_clear();
    display_vline(DIV_X, 0, 64);
    // Left: game detect
    display_rect(0, 0, DIV_X, 64);
    display_text(4, 4,  "GAME");
    display_text(4, 18, "DETECT");
    display_hline(4, 34, 56);
    display_text(4, 48, "LOADING");
    // Right: game info
    draw_text_clipped(RX, 0, "GAME DETECT", RW);
    display_hline(RX, 9, RW);
    draw_text_clipped(RX, 11, game_name, RW);
    display_hline(RX, 29, RW);
    draw_text_clipped(RX, 31, "LOADING VMU", RW);
    display_rect(RX, 40, RW, 4);
}

static void render_no_sd(void) {
    display_clear();
    display_vline(DIV_X, 0, 64);
    draw_vmu_logo();
    char slot_buf[12];
    slot_str(slot_buf, active_slot);
    draw_right_panel("JOYPAD OS", slot_buf, "NO GAME", NULL,
                     blocks_used, blocks_total, NULL, "SD:NONE");
}

// ============================================================================
// Pre-init — call from app_init() before Maple Bus starts
// Just sets up I2C hardware, no display commands sent
// ============================================================================

void dc_display_preinit(void) {
#ifdef OLED_I2C_INST
    i2c_inst_t* i2c = (OLED_I2C_INST == 0) ? i2c0 : i2c1;
    i2c_init(i2c, 400 * 1000);
    gpio_set_function(OLED_I2C_SDA_PIN, GPIO_FUNC_I2C);
    gpio_set_function(OLED_I2C_SCL_PIN, GPIO_FUNC_I2C);
    gpio_pull_up(OLED_I2C_SDA_PIN);
    gpio_pull_up(OLED_I2C_SCL_PIN);
    printf("[dc-display] I2C pre-initialized\n");
#endif
}

// ============================================================================
// Task
// ============================================================================

// Called when Maple Bus goes live — starts boot timer
void dc_display_start_boot(void) {
    if (!display_present) return;
    boot_start_ms = to_ms_since_boot(get_absolute_time());
    current_state = DC_DISPLAY_BOOT;
    needs_redraw = true;
}

void dc_display_task(void) {
    if (!display_present) return;

    // Boot splash timeout
    if (current_state == DC_DISPLAY_BOOT) {
        if (boot_start_ms == 0) return;  // Not started yet
        uint32_t now = to_ms_since_boot(get_absolute_time());
        if (now - boot_start_ms >= BOOT_DURATION_MS) {
            current_state = DC_DISPLAY_IDLE;
            needs_redraw = true;
        }
    }

    // Latch VMU LCD data if updated (Core 1 -> Core 0 transfer)
    if (vmu_lcd_dirty && vmu_lcd_src != NULL) {
        vmu_lcd_dirty = false;
        memcpy(vmu_lcd_buf, (const uint8_t*)vmu_lcd_src, 192);
        // Switch to gameplay state when LCD data arrives
        if (current_state == DC_DISPLAY_IDLE || current_state == DC_DISPLAY_BOOT) {
            current_state = DC_DISPLAY_GAMEPLAY;
        }
        needs_redraw = true;
    }

    // Redraw if needed
    needs_update = false;
    if (needs_redraw) {
        needs_redraw = false;
        switch (current_state) {
            case DC_DISPLAY_BOOT:        render_boot();        break;
            case DC_DISPLAY_IDLE:        render_idle();        break;
            case DC_DISPLAY_GAMEPLAY:    render_gameplay();    break;
            case DC_DISPLAY_WRITE:       render_write();       break;
            case DC_DISPLAY_SLOT_SWITCH: render_slot_switch(); break;
            case DC_DISPLAY_GAMEID:      render_gameid();      break;
            case DC_DISPLAY_NO_SD:       render_no_sd();       break;
        }
        display_set_async(true);
        display_update();
    }

    // Incremental I2C flush — one page per call, non-blocking
    display_flush_step();
}

// ============================================================================
// Public API
// ============================================================================

void dc_display_set_state(dc_display_state_t state) {
    if (!display_present) return;
    if (state == current_state) return;
    current_state = state;
    needs_redraw = true;
    needs_update = true;
}

void dc_display_set_vmu_lcd(const uint8_t* data) {
    // Safe from Core 1 — just stores pointer, no memcpy
    // Core 0 does the actual copy in dc_display_task()
    vmu_lcd_src = data;
    vmu_lcd_dirty = true;
    needs_update = true;
}

void dc_display_set_game_name(const char* name) {
    if (!display_present) return;
    strncpy(game_name, name, sizeof(game_name) - 1);
    game_name[sizeof(game_name) - 1] = '\0';
    needs_redraw = true;
}

void dc_display_set_slot(uint8_t slot) {
    if (!display_present) return;
    active_slot = slot;
    needs_redraw = true;
}

void dc_display_set_blocks(uint16_t used, uint16_t total) {
    if (!display_present) return;
    blocks_used = used;
    blocks_total = total;
    needs_redraw = true;
}

void dc_display_set_sd_label(const char* label) {
    if (!display_present) return;
    strncpy(sd_label, label, sizeof(sd_label) - 1);
    sd_label[sizeof(sd_label) - 1] = '\0';
    needs_redraw = true;
}

bool dc_display_needs_update(void) {
    return needs_update;
}

bool dc_display_is_present(void) {
    return display_present;
}

#else  // No OLED_I2C_INST — stub everything out

void dc_display_preinit(void) {}
void dc_display_init(void) {}
void dc_display_task(void) {}
void dc_display_set_state(dc_display_state_t state) { (void)state; }
void dc_display_set_vmu_lcd(const uint8_t* data) { (void)data; }
void dc_display_set_game_name(const char* name) { (void)name; }
void dc_display_set_slot(uint8_t slot) { (void)slot; }
void dc_display_set_blocks(uint16_t used, uint16_t total) { (void)used; (void)total; }
void dc_display_set_sd_label(const char* label) { (void)label; }
bool dc_display_needs_update(void) { return false; }
bool dc_display_is_present(void) { return false; }

#endif // OLED_I2C_INST
