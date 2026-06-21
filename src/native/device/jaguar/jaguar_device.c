// jaguar_device.c - Atari Jaguar HD15 output driver for JoypadOS
//
// See jaguar_device.h for architecture and pin mapping notes.
//
// Core 1 approach mirrors BlueRetro's ESP32 implementation:
//   - Reads all strobe pins in a single GPIO register read
//   - Detects EDGES (transitions) rather than levels
//   - Writes all output pins in a single atomic GPIO register write
//   - Precomputes per-row GPIO set/clear masks so the hot path is
//     one array lookup + one register write

#include "jaguar_device.h"
#include "core/router/router.h"
#include "core/input_event.h"
#include "core/buttons.h"
#include "profiles.h"
#include "core/services/storage/flash.h"
#include "core/services/leds/leds.h"
#include "core/services/profiles/profile.h"
#include "hardware/gpio.h"
#include "hardware/structs/sio.h"
#include "hardware/structs/ioqspi.h"
#include "hardware/timer.h"
#include "hardware/sync.h"
#include "pico/stdlib.h"
#include "pico/multicore.h"
#include <stdio.h>
#include <string.h>

// ============================================================================
// FLASH STORAGE LAYOUT (uses reserved[] bytes in flash_t)
// reserved[0] = spinner divisor
// reserved[1] = spinner invert (0 = normal, 1 = inverted)
// ============================================================================

#define FLASH_DIVISOR_IDX  0
#define FLASH_INVERT_IDX   1

// ============================================================================
// GPIO MASKS — all output pin bitmasks in the 32-bit SIO GPIO register
// ============================================================================

#define GPIO_MASK_B0   (1u << JAG_PIN_B0)
#define GPIO_MASK_B1   (1u << JAG_PIN_B1)
#define GPIO_MASK_J8   (1u << JAG_PIN_J8)
#define GPIO_MASK_J9   (1u << JAG_PIN_J9)
#define GPIO_MASK_J10  (1u << JAG_PIN_J10)
#define GPIO_MASK_J11  (1u << JAG_PIN_J11)

// All output pins mask — used to release everything at once
#define GPIO_MASK_ALL_OUT  (GPIO_MASK_B0 | GPIO_MASK_B1 | GPIO_MASK_J8 | \
                            GPIO_MASK_J9 | GPIO_MASK_J10 | GPIO_MASK_J11)

// Strobe input masks
#define GPIO_MASK_J0   (1u << JAG_PIN_J0)
#define GPIO_MASK_J1   (1u << JAG_PIN_J1)
#define GPIO_MASK_J2   (1u << JAG_PIN_J2)
#define GPIO_MASK_J3   (1u << JAG_PIN_J3)

#define GPIO_MASK_STROBES  (GPIO_MASK_J0 | GPIO_MASK_J1 | GPIO_MASK_J2 | GPIO_MASK_J3)

// ============================================================================
// PHASE TABLE
// ============================================================================

// Precomputed GPIO SET masks for each phase step.
// Pins in the SET mask are HIGH (released). Absent = LOW (asserted).
// Active LOW: J10/J11 LOW = asserted = game reads L/R as pressed.
//
// CRITICAL: The game only uses rot_cum when dopad sees L or R pressed
// (pad_now bits 7-6 nonzero). Phase state where both J10 and J11 are
// HIGH causes dopad to read L=0 R=0 → stopclaw → rot_cum cleared.
//
// We use a 3-state sequence: conseq states 0→1→3→0 (CW) or 0→3→1→0 (CCW).
// All three have at least one pin LOW. State 2 (both HIGH) is never emitted.
//
// State 0 (00): J10 LOW,  J11 LOW  → SET neither → both asserted
// State 1 (01): J10 HIGH, J11 LOW  → SET J10     → R(J11) asserted
// State 3 (11): J10 LOW,  J11 HIGH → SET J11     → L(J10) asserted
// (State 2 skipped — would be both HIGH → stopclaw)
static const uint32_t phase_gpio_set[3] = {
    0,              // state 0: J10 LOW,  J11 LOW  (both asserted)
    GPIO_MASK_J10,  // state 1: J10 HIGH, J11 LOW  (R asserted)
    GPIO_MASK_J11,  // state 3: J10 LOW,  J11 HIGH (L asserted)
};

// CW sequence index: 0→1→2→0  (maps to conseq states 0→1→3→0)
// CCW sequence index: 0→2→1→0 (maps to conseq states 0→3→1→0)
#define PHASE_SEQ_LEN 3

// ============================================================================
// ROW OUTPUT MASKS
// Precomputed per-row GPIO SET masks (pins to drive HIGH = released).
// Core 0 builds these; Core 1 reads them atomically.
// Pins absent from the set mask will be cleared (driven LOW = asserted).
// ============================================================================

// jag_row_gpio[row] = bitmask of output pins to SET (HIGH) for that row
// All output pins not in the mask are CLEARed (LOW)
volatile uint32_t jag_row_gpio[4] = {
    GPIO_MASK_ALL_OUT,  // row 0: all released
    GPIO_MASK_ALL_OUT,  // row 1: all released
    GPIO_MASK_ALL_OUT,  // row 2: all released (C2 HIGH = STDPAD)
    GPIO_MASK_ALL_OUT,  // row 3: all released (C3 HIGH = STDPAD)
};

// 16-entry strobe lookup table for Core 1 hot path.
// Indexed by 4-bit strobe value: bits = (J3<<3|J2<<2|J1<<1|J0<<0), active LOW.
// So index 0b1110 (0xE) = J0 active, 0b1101 (0xD) = J1, etc.
// Entry = GPIO SET mask to drive when that strobe combination is asserted.
// Rebuilt by Core 0 whenever row data changes via build_strobe_table().
// Index 0b1111 (all HIGH = no strobe) = release all outputs.
// Multi-strobe entries use lowest active row (defensive only, shouldn't occur).
volatile uint32_t jag_strobe_table[16];

// Shift offset to extract strobe bits from gpio_in into table index.
// Strobe pins are J0=GP2..J3=GP5, so shift right by JAG_PIN_J0.
#define STROBE_SHIFT  JAG_PIN_J0

// Exposed shared state (used by header declarations)
volatile uint8_t          jag_row_data[4] = { 0xFF, 0xFF, 0xFF, 0xFF };
volatile uint8_t          jag_phase_idx   = 0;  // 0..2 index into phase_gpio_set
volatile jag_input_mode_t jag_input_mode  = JAG_MODE_GAMEPAD;

// Pending phase steps: Core 0 accumulates signed steps from mouse deltas.
// Core 1 drains one step at a time at a timer-controlled rate.
// Positive = CW, negative = CCW.
volatile int16_t          jag_phase_pending = 0;

// Step interval in microseconds — controls spinner speed.
// Core 0 writes this; Core 1 reads it each drain cycle.
// Derived from spinner_divisor: divisor 1 = fastest, 8 = slowest.
volatile uint32_t         jag_step_interval_us = 4000;

// ============================================================================
// INTERNAL STATE (Core 0 only)
// ============================================================================

static uint8_t  spinner_divisor      = JAG_SPINNER_DIVISOR_DEFAULT;
static bool     spinner_invert       = false;
static bool     invert_led_flash     = false;
static int16_t  phase_accum          = 0;
static bool     device_connected     = false;
static bool     dpi_adjust_mode      = false;
static uint32_t last_gamepad_buttons = 0;

// ============================================================================
// FLASH HELPERS
// ============================================================================

static void load_settings(void) {
    flash_t* s = flash_get_settings();
    if (!s) return;
    uint8_t d = s->reserved[FLASH_DIVISOR_IDX];
    if (d >= JAG_SPINNER_DIVISOR_MIN && d <= JAG_SPINNER_DIVISOR_MAX)
        spinner_divisor = d;
    spinner_invert = s->reserved[FLASH_INVERT_IDX] != 0;
    printf("[jaguar] Loaded — divisor=%d invert=%d\n", spinner_divisor, spinner_invert);
}

static void save_settings(void) {
    flash_t* s = flash_get_settings();
    if (!s) return;
    s->reserved[FLASH_DIVISOR_IDX] = spinner_divisor;
    s->reserved[FLASH_INVERT_IDX]  = spinner_invert ? 1 : 0;
    flash_save(s);
    printf("[jaguar] Saved — divisor=%d invert=%d\n", spinner_divisor, spinner_invert);
}

// ============================================================================
// LED
// ============================================================================

static void update_led(void) {
    switch (jag_input_mode) {
        case JAG_MODE_GAMEPAD: leds_set_color(0, 80,  0); break;  // green
        case JAG_MODE_SPINNER: leds_set_color(0,  0, 80); break;  // blue
    }
}

// ============================================================================
// STROBE LOOKUP TABLE BUILDER (Core 0)
// Rebuilds jag_strobe_table[16] from current jag_row_gpio values.
// Called after every row update so Core 1 always has current data.
//
// Table index = 4-bit strobe value from gpio_in bits [J3:J2:J1:J0].
// Strobe pins are active LOW: a 0 bit means that row is asserted.
// Index 0xF (1111) = no strobe = release all outputs.
// ============================================================================

static void build_strobe_table(void) {
    uint32_t release = (jag_input_mode == JAG_MODE_SPINNER)
        ? GPIO_MASK_ALL_OUT & ~(GPIO_MASK_J10 | GPIO_MASK_J11)
        : GPIO_MASK_ALL_OUT;

    for (uint8_t i = 0; i < 16; i++) {
        // Invert bits: active LOW strobe pins mean active bit = 0 in gpio_in
        // Bit 0 = J0, bit 1 = J1, bit 2 = J2, bit 3 = J3
        // Active when bit is 0 in gpio_in, so active = ~i & 0xF
        uint8_t active = (~i) & 0xF;

        if (active == 0) {
            // No strobe — release all
            jag_strobe_table[i] = release;
        } else if (active & 0x1) {
            // J0 active (Row 0) — lowest priority wins for multi-strobe
            uint32_t mask = jag_row_gpio[0];
            if (jag_input_mode == JAG_MODE_SPINNER)
                mask |= (GPIO_MASK_J10 | GPIO_MASK_J11);
            jag_strobe_table[i] = mask;
        } else if (active & 0x2) {
            // J1 active (Row 1)
            uint32_t mask = jag_row_gpio[1];
            if (jag_input_mode == JAG_MODE_SPINNER)
                mask |= (GPIO_MASK_J10 | GPIO_MASK_J11);
            jag_strobe_table[i] = mask;
        } else if (active & 0x4) {
            // J2 active (Row 2)
            uint32_t mask = jag_row_gpio[2];
            if (jag_input_mode == JAG_MODE_SPINNER)
                mask |= (GPIO_MASK_J10 | GPIO_MASK_J11);
            jag_strobe_table[i] = mask;
        } else {
            // J3 active (Row 3)
            uint32_t mask = jag_row_gpio[3];
            if (jag_input_mode == JAG_MODE_SPINNER)
                mask |= (GPIO_MASK_J10 | GPIO_MASK_J11);
            jag_strobe_table[i] = mask;
        }
    }
}


// Converts button state into per-row GPIO SET masks.
// A pin in the SET mask = HIGH (released). Absent = LOW (asserted).
// ============================================================================

static uint32_t __not_in_flash_func(build_row_mask)(
    uint32_t buttons,
    bool b0_pressed,   // B0 assertion override (Pause / type ID)
    bool b1_pressed,   // B1 assertion override (Fire / Option)
    bool j8_pressed,
    bool j9_pressed,
    bool j10_pressed,
    bool j11_pressed)
{
    (void)buttons;
    uint32_t mask = GPIO_MASK_ALL_OUT;  // start all released

    if (b0_pressed)  mask &= ~GPIO_MASK_B0;
    if (b1_pressed)  mask &= ~GPIO_MASK_B1;
    if (j8_pressed)  mask &= ~GPIO_MASK_J8;
    if (j9_pressed)  mask &= ~GPIO_MASK_J9;
    if (j10_pressed) mask &= ~GPIO_MASK_J10;
    if (j11_pressed) mask &= ~GPIO_MASK_J11;

    return mask;
}

static void build_gamepad_rows(uint32_t buttons) {
    // Row 0: Pause(B0), FireA(B1), Up(J8), Down(J9), Left(J10), Right(J11)
    jag_row_gpio[0] = build_row_mask(buttons,
        (buttons & JP_BUTTON_S1) != 0,
        (buttons & JP_BUTTON_B1) != 0,
        (buttons & JP_BUTTON_DU) != 0,
        (buttons & JP_BUTTON_DD) != 0,
        (buttons & JP_BUTTON_DL) != 0,
        (buttons & JP_BUTTON_DR) != 0);

    // Row 1: FireB(B1), *(J8), 7(J9), 4(J10), 1(J11)
    jag_row_gpio[1] = build_row_mask(buttons,
        false,
        (buttons & JP_BUTTON_B2)  != 0,
        false,                              // * not mapped
        (buttons & JAG_NUMPAD_7)  != 0,    // 7 on J9
        (buttons & JAG_NUMPAD_4)  != 0,    // 4 on J10
        false);

    // Row 2: FireC(B1), 0(J8), 8(J9), 5(J10), 2(J11) — C2 type ID on B0
    jag_row_gpio[2] = build_row_mask(buttons,
        false,
        (buttons & JP_BUTTON_B3)  != 0,
        false,                              // 0 not mapped
        (buttons & JAG_NUMPAD_8)  != 0,    // 8 on J9
        false,                              // 5 not mapped
        false);

    // Row 3: Option(B1), #(J8), 9(J9), 6(J10), 3(J11) — C3 type ID on B0
    jag_row_gpio[3] = build_row_mask(buttons,
        false,
        (buttons & JP_BUTTON_S2)  != 0,
        false,                              // # not mapped
        (buttons & JAG_NUMPAD_9)  != 0,    // 9 on J9
        (buttons & JAG_NUMPAD_6)  != 0,    // 6 on J10
        false);

    build_strobe_table(); __dmb();
}

static void build_spinner_rows(uint32_t buttons) {
    // Row 0: Pause(B0), FireA(B1) — J10/J11 driven by phase, leave HIGH here
    jag_row_gpio[0] = build_row_mask(buttons,
        (buttons & JP_BUTTON_S1) != 0,
        (buttons & JP_BUTTON_B1) != 0,
        false, false, false, false);

    // Row 1: FireB(B1)
    jag_row_gpio[1] = build_row_mask(buttons,
        false,
        (buttons & JP_BUTTON_B2) != 0,
        false, false, false, false);

    // Row 2: FireC(B1) — C2 HIGH (B0 released) — ROTARY type
    jag_row_gpio[2] = build_row_mask(buttons,
        false,
        (buttons & JP_BUTTON_B3) != 0,
        false, false, false, false);

    // Row 3: C3 LOW (B0 asserted) — ROTARY type
    jag_row_gpio[3] = GPIO_MASK_ALL_OUT & ~GPIO_MASK_B0;

    build_strobe_table(); __dmb();
}

// ============================================================================
// INPUT EVENT TAP (Core 0)
// ============================================================================

static void __not_in_flash_func(jaguar_tap_callback)(
    output_target_t output,
    uint8_t player_index,
    const input_event_t* event)
{
    (void)output;
    (void)player_index;

    if (event->type == INPUT_TYPE_NONE) {
        device_connected      = false;
        jag_input_mode        = JAG_MODE_GAMEPAD;
        dpi_adjust_mode       = false;
        last_gamepad_buttons  = 0;
        jag_row_gpio[0]   = GPIO_MASK_ALL_OUT;
        jag_row_gpio[1]   = GPIO_MASK_ALL_OUT;
        jag_row_gpio[2]   = GPIO_MASK_ALL_OUT;
        jag_row_gpio[3]   = GPIO_MASK_ALL_OUT;
        jag_phase_idx     = 0;
        jag_phase_pending = 0;
        jag_step_interval_us = 0;
        leds_set_color(0, 0, 0);
        return;
    }

    if (event->type == INPUT_TYPE_MOUSE) {
        if (!device_connected || jag_input_mode != JAG_MODE_SPINNER) {
            device_connected = true;
            jag_input_mode   = JAG_MODE_SPINNER;
            update_led();
            printf("[jaguar] Spinner mode — divisor=%d\n", spinner_divisor);
        }

        // In DPI adjust mode: left click = faster (lower divisor),
        // right click = slower (higher divisor),
        // middle click = toggle direction invert.
        // Clicks consumed here, not passed to game.
        // Movement still passes through for live preview.
        if (dpi_adjust_mode) {
            static uint32_t last_dpi_change = 0;
            uint32_t now = to_ms_since_boot(get_absolute_time());
            if (now - last_dpi_change >= 300) {
                if (event->buttons & JP_BUTTON_B1) {
                    if (spinner_divisor > JAG_SPINNER_DIVISOR_MIN) {
                        spinner_divisor--;
                        last_dpi_change = now;
                        printf("[jaguar] Divisor: %d\n", spinner_divisor);
                    }
                } else if (event->buttons & JP_BUTTON_B2) {
                    if (spinner_divisor < JAG_SPINNER_DIVISOR_MAX) {
                        spinner_divisor++;
                        last_dpi_change = now;
                        printf("[jaguar] Divisor: %d\n", spinner_divisor);
                    }
                } else if (event->buttons & JP_BUTTON_S2) {
                    spinner_invert = !spinner_invert;
                    last_dpi_change = now;
                    leds_set_color(80, 80, 0);
                    invert_led_flash = true;
                    printf("[jaguar] Invert: %d\n", spinner_invert);
                }
            }
            // Fall through to phase accumulation for live preview
        }

        // Accumulate mouse delta into pending steps.
        // Each unit of delta_x past the divisor threshold = one phase step.
        // On direction change, flush pending to stop immediately.
        int16_t delta = (spinner_invert && jag_input_mode == JAG_MODE_SPINNER)
            ? -event->delta_x
            : event->delta_x;
        if (delta != 0) {
            int16_t new_steps = 0;
            phase_accum += delta;
            if (phase_accum >= spinner_divisor) {
                new_steps = 1;
                phase_accum = 0;
            } else if (phase_accum <= -spinner_divisor) {
                new_steps = -1;
                phase_accum = 0;
            }
            if (new_steps != 0) {
                if ((new_steps > 0 && jag_phase_pending < 0) ||
                    (new_steps < 0 && jag_phase_pending > 0)) {
                    jag_phase_pending = 0;
                }
                jag_phase_pending += new_steps;
                jag_step_interval_us = (uint32_t)spinner_divisor * 3000u;
                __dmb();
            }
        } else {
            phase_accum = 0;
        }

        // Pass buttons to game only when not in DPI adjust mode
        uint32_t btns = 0;
        if (!dpi_adjust_mode) {
            if (event->buttons & JP_BUTTON_B1) btns |= JP_BUTTON_B2;   // Left click  → B
            if (event->buttons & JP_BUTTON_B2) btns |= JP_BUTTON_B1;   // Right click → A
            if (event->buttons & JP_BUTTON_S2) btns |= JP_BUTTON_B3;   // Middle click → C
            if (event->buttons & JP_BUTTON_S1) btns |= JP_BUTTON_S1;   // Side → Pause (if present)
        }
        build_spinner_rows(btns);

    } else {
        if (!device_connected || jag_input_mode != JAG_MODE_GAMEPAD) {
            device_connected = true;
            jag_input_mode   = JAG_MODE_GAMEPAD;
            phase_accum      = 0;
            update_led();
            printf("[jaguar] Gamepad mode\n");
        }
        uint32_t raw_buttons = event->buttons;
        const profile_t* profile = profile_get_active(OUTPUT_TARGET_JAGUAR);
        uint32_t mapped_buttons = profile_apply_button_map(profile, raw_buttons);

        // Convert analog stick to dpad
        if (event->analog[ANALOG_LX] < 64)  mapped_buttons |= JP_BUTTON_DL;
        if (event->analog[ANALOG_LX] > 192) mapped_buttons |= JP_BUTTON_DR;
        if (event->analog[ANALOG_LY] < 64)  mapped_buttons |= JP_BUTTON_DU;
        if (event->analog[ANALOG_LY] > 192) mapped_buttons |= JP_BUTTON_DD;

        // M30 2.4G Genesis Mini: C button = ANALOG_R2, Right shoulder (R) = ANALOG_L2
        if (event->analog[ANALOG_R2] > 64) mapped_buttons |= JP_BUTTON_B1;
        if (event->analog[ANALOG_L2] > 64) mapped_buttons |= JAG_NUMPAD_6;

        last_gamepad_buttons = raw_buttons;
        build_gamepad_rows(mapped_buttons);
    }
}

// ============================================================================
// CORE 1 TASK — tight loop, no flash, no interrupts
//
// Two responsibilities:
//
// 1. SPINNER PHASE OUTPUT (spinner mode only):
//    Drains jag_phase_pending one step at a time at jag_step_interval_us rate.
//    Uses a 3-state quadrature sequence (states 0,1,3 — skipping state 2 which
//    would cause stopclaw). Drives J10/J11 continuously so the game always reads
//    current phase state when it polls JOYIN at ~1000Hz.
//
// 2. BUTTON/DIRECTION OUTPUT (both modes):
//    Edge-detects J0-J3 strobe transitions, drives B0/B1/J8/J9 atomically
//    from precomputed row masks. In spinner mode, J10/J11 are excluded from
//    strobe-driven output — they are managed by the phase driver above.
// ============================================================================

// CW step: advance seq_idx forward through 3-state sequence
// CCW step: advance seq_idx backward through 3-state sequence
// seq_idx cycles 0→1→2→0 (CW) or 0→2→1→0 (CCW)
// Maps to conseq states: seq[0]=state0, seq[1]=state1, seq[2]=state3

void __not_in_flash_func(jaguar_core1_task)(void) {
    uint32_t last_step  = timer_hw->timerawl;
    uint8_t  seq_idx    = 0;

    while (true) {
        // ---- Spinner phase output (spinner mode only) ----
        if (jag_input_mode == JAG_MODE_SPINNER) {
            uint32_t now = timer_hw->timerawl;
            int16_t pending = jag_phase_pending;
            uint32_t interval = jag_step_interval_us;

            if (pending != 0 && interval > 0 && (now - last_step) >= interval) {
                if (pending > 0) {
                    seq_idx = (seq_idx + PHASE_SEQ_LEN - 1) % PHASE_SEQ_LEN;
                    jag_phase_pending = pending - 1;
                } else {
                    seq_idx = (seq_idx + 1) % PHASE_SEQ_LEN;
                    jag_phase_pending = pending + 1;
                }
                jag_phase_idx = seq_idx;
                last_step = now;
            }

            uint32_t phase_set = phase_gpio_set[seq_idx];
            uint32_t phase_clr = (GPIO_MASK_J10 | GPIO_MASK_J11) & ~phase_set;
            if (phase_set) sio_hw->gpio_set = phase_set;
            if (phase_clr) sio_hw->gpio_clr = phase_clr;
        }

        // ---- Button/direction output — single lookup table access ----
        // Extract 4 strobe bits, index into precomputed table.
        // One read, one table lookup, two writes — minimum possible overhead.
        uint32_t gpio_in = sio_hw->gpio_in;
        uint8_t  idx     = (gpio_in >> STROBE_SHIFT) & 0xF;
        uint32_t set     = jag_strobe_table[idx];
        uint32_t clr     = GPIO_MASK_ALL_OUT & ~set;
        if (jag_input_mode == JAG_MODE_SPINNER)
            clr &= ~(GPIO_MASK_J10 | GPIO_MASK_J11);
        sio_hw->gpio_set = set;
        if (clr) sio_hw->gpio_clr = clr;
    }
}

// ============================================================================
// BOOTSEL BUTTON READER
// Temporarily overrides QSPI CS to read the BOOTSEL button state.
// Disables interrupts briefly — safe on Core 0, not called from Core 1.
// ============================================================================

static bool __no_inline_not_in_flash_func(read_bootsel_button)(void) {
    const uint CS_PIN_INDEX = 1;
    uint32_t flags = save_and_disable_interrupts();
    hw_write_masked(&ioqspi_hw->io[CS_PIN_INDEX].ctrl,
                    2u << IO_QSPI_GPIO_QSPI_SS_CTRL_OEOVER_LSB,
                    IO_QSPI_GPIO_QSPI_SS_CTRL_OEOVER_BITS);
    for (volatile int i = 0; i < 10000; ++i);
    bool state = !(sio_hw->gpio_hi_in & (1u << CS_PIN_INDEX));
    hw_write_masked(&ioqspi_hw->io[CS_PIN_INDEX].ctrl,
                    0u << IO_QSPI_GPIO_QSPI_SS_CTRL_OEOVER_LSB,
                    IO_QSPI_GPIO_QSPI_SS_CTRL_OEOVER_BITS);
    restore_interrupts(flags);
    return state;
}

// ============================================================================
// DEVICE TASK (Core 0 — called from main loop)
//
// DPI adjustment UX (spinner mode only):
//   Hold BOOTSEL 2s then release  — enter DPI adjust mode (LED turns purple)
//   Left mouse button              — increase speed (decrease divisor)
//   Right mouse button             — decrease speed (increase divisor)
//   Middle mouse button            — toggle direction invert (LED flashes yellow)
//   Mouse movement                 — live preview, passes through to game
//   Tap BOOTSEL                    — exit DPI adjust mode, save, LED returns to blue
//
// In DPI adjust mode, mouse button clicks are consumed here
// and do NOT reach the Jaguar.
// ============================================================================

void jaguar_device_task(void) {
    static uint32_t last_read        = 0;
    static bool     btn_was_pressed  = false;
    static uint32_t btn_press_start  = 0;
    static uint32_t last_dpi_change  = 0;
    static uint32_t led_restore_at   = 0;

    uint32_t now = to_ms_since_boot(get_absolute_time());

    // Restore purple LED after yellow invert flash
    if (invert_led_flash) {
        invert_led_flash = false;
        led_restore_at = now + 400;
    }
    if (led_restore_at && now >= led_restore_at) {
        led_restore_at = 0;
        leds_set_color(48, 0, 48);
    }

    // Profile switching: hold Select (S1) 2s then tap dpad up/down.
    if (device_connected && jag_input_mode == JAG_MODE_GAMEPAD) {
        static uint32_t select_hold_start = 0;
        static bool     select_was_held   = false;
        static bool     select_armed      = false;
        static uint32_t last_switch_ms    = 0;

        bool select_held = (last_gamepad_buttons & JP_BUTTON_S1) != 0;
        bool du_pressed  = (last_gamepad_buttons & JP_BUTTON_DU) != 0;
        bool dd_pressed  = (last_gamepad_buttons & JP_BUTTON_DD) != 0;

        if (!select_held) {
            if (select_armed) update_led();
            select_hold_start = 0;
            select_was_held   = false;
            select_armed      = false;
        } else {
            if (!select_was_held) {
                select_hold_start = now;
                select_was_held   = true;
            }
            if (!select_armed && (now - select_hold_start) >= 2000) {
                select_armed = true;
                leds_set_color(0, 80, 80);
            }
            if (select_armed && (now - last_switch_ms) >= 500) {
                if (du_pressed) {
                    uint8_t count = profile_get_count(OUTPUT_TARGET_JAGUAR);
                    uint8_t next = (profile_get_active_index(OUTPUT_TARGET_JAGUAR) + 1) % count;
                    profile_set_active(OUTPUT_TARGET_JAGUAR, next);
                    leds_set_color(0, 0, 0);
                    last_switch_ms = now;
                    printf("[jaguar] Profile: %d\n", next);
                } else if (dd_pressed) {
                    uint8_t count = profile_get_count(OUTPUT_TARGET_JAGUAR);
                    uint8_t prev = (profile_get_active_index(OUTPUT_TARGET_JAGUAR) + count - 1) % count;
                    profile_set_active(OUTPUT_TARGET_JAGUAR, prev);
                    leds_set_color(0, 0, 0);
                    last_switch_ms = now;
                    printf("[jaguar] Profile: %d\n", prev);
                }
            }
        }
    }

    // Poll BOOTSEL at 20Hz
    if (now - last_read < 50) return;
    last_read = now;

    bool pressed = read_bootsel_button();

    if (pressed && !btn_was_pressed) {
        btn_press_start = now;
        btn_was_pressed = true;
    } else if (!pressed && btn_was_pressed) {
        btn_was_pressed = false;
        uint32_t held = now - btn_press_start;

        if (dpi_adjust_mode) {
            // Any tap exits DPI adjust mode
            dpi_adjust_mode = false;
            update_led();
            save_settings();
            printf("[jaguar] DPI adjust: OFF (divisor=%d saved)\n", spinner_divisor);
        } else if (held >= 2000 && jag_input_mode == JAG_MODE_SPINNER) {
            // Hold 2s to enter DPI adjust mode
            dpi_adjust_mode = true;
            leds_set_color(48, 0, 48);
            printf("[jaguar] DPI adjust: ON (divisor=%d)\n", spinner_divisor);
        } else if (held < 2000 && jag_input_mode == JAG_MODE_SPINNER) {
            // Short tap in spinner mode = Pause
            uint32_t btns = JP_BUTTON_S1;
            build_spinner_rows(btns);
            // Hold Pause briefly then release
            busy_wait_ms(100);
            build_spinner_rows(0);
        }
    }
}

// ============================================================================
// DEVICE INIT
// ============================================================================

static uint8_t jaguar_get_player_count(void) {
    return (uint8_t)router_get_player_count(OUTPUT_TARGET_JAGUAR);
}

void jaguar_device_init(void) {
    // Strobe inputs — pull-up, input mode
    gpio_init(JAG_PIN_J0); gpio_set_dir(JAG_PIN_J0, GPIO_IN); gpio_pull_up(JAG_PIN_J0);
    gpio_init(JAG_PIN_J1); gpio_set_dir(JAG_PIN_J1, GPIO_IN); gpio_pull_up(JAG_PIN_J1);
    gpio_init(JAG_PIN_J2); gpio_set_dir(JAG_PIN_J2, GPIO_IN); gpio_pull_up(JAG_PIN_J2);
    gpio_init(JAG_PIN_J3); gpio_set_dir(JAG_PIN_J3, GPIO_IN); gpio_pull_up(JAG_PIN_J3);

    // Data outputs — all released (HIGH) at init
    gpio_init(JAG_PIN_B0);  gpio_put(JAG_PIN_B0,  1); gpio_set_dir(JAG_PIN_B0,  GPIO_OUT);
    gpio_init(JAG_PIN_B1);  gpio_put(JAG_PIN_B1,  1); gpio_set_dir(JAG_PIN_B1,  GPIO_OUT);
    gpio_init(JAG_PIN_J8);  gpio_put(JAG_PIN_J8,  1); gpio_set_dir(JAG_PIN_J8,  GPIO_OUT);
    gpio_init(JAG_PIN_J9);  gpio_put(JAG_PIN_J9,  1); gpio_set_dir(JAG_PIN_J9,  GPIO_OUT);
    gpio_init(JAG_PIN_J10); gpio_put(JAG_PIN_J10, 1); gpio_set_dir(JAG_PIN_J10, GPIO_OUT);
    gpio_init(JAG_PIN_J11); gpio_put(JAG_PIN_J11, 1); gpio_set_dir(JAG_PIN_J11, GPIO_OUT);

    // Row GPIO masks default to all released
    jag_row_gpio[0] = GPIO_MASK_ALL_OUT;
    jag_row_gpio[1] = GPIO_MASK_ALL_OUT;
    jag_row_gpio[2] = GPIO_MASK_ALL_OUT;
    jag_row_gpio[3] = GPIO_MASK_ALL_OUT;
    build_strobe_table(); __dmb();

    load_settings();

    // Register player count callback so profile_check_switch_combo
    // fires correctly (it guards on player count internally)
    profile_set_player_count_callback(jaguar_get_player_count);

    router_set_tap_exclusive(OUTPUT_TARGET_JAGUAR, jaguar_tap_callback);

    printf("[jaguar] Init complete — divisor=%d\n", spinner_divisor);
}

// ============================================================================
// PUBLIC API
// ============================================================================

jag_input_mode_t jaguar_get_mode(void)           { return jag_input_mode; }
uint8_t          jaguar_get_spinner_divisor(void) { return spinner_divisor; }

// ============================================================================
// OUTPUT INTERFACE
// ============================================================================

const OutputInterface jaguar_output_interface = {
    .name           = "Jaguar",
    .init           = jaguar_device_init,
    .core1_task     = jaguar_core1_task,
    .task           = jaguar_device_task,
    .get_rumble     = NULL,
    .get_player_led = NULL,
};
