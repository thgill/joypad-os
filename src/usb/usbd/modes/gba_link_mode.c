// gba_link_mode.c - USB Vendor-class GBA Link Cable bridge for Dolphin
// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Robert Dale Smith
//
// Mode driver for USB_OUTPUT_MODE_GBA_LINK. Exposes a vendor-class USB
// device that a forked Dolphin emulator talks to via libusb. Bridges
// raw joybus byte traffic between Dolphin and the connected GBA on
// joybus pin GP4 (or whatever GC_PIN_DATA is set to in the build).
//
// Wire protocol — same raw joybus bytes as the existing TCP path:
//   Dolphin → bridge: 1 byte cmd (or 5 bytes for WRITE = cmd + 4 data)
//   bridge → Dolphin: 3 bytes (RESET/STATUS reply), 5 bytes (READ),
//                     or 1 byte (WRITE jstat)
//
// Phase A skeleton: registers the descriptor + a no-op `send_report`
// hook so the rest of usbd.c can pick this mode without crashing. The
// actual joybus dispatch lives in `gba_link_mode_task()` (Step A2).

#include "tusb.h"
#include "device/usbd.h"

// Only build the dispatch path on apps that actually link the joybus
// bridge backend (gc2usb / gc2eth / gc2eth_feather). Other apps with
// CFG_TUD_VENDOR enabled but no joybus_bridge.c would get linker errors.
#if CFG_TUD_VENDOR && defined(CONFIG_JOYBUS_BRIDGE)

#include "../usbd_mode.h"
#include "../usbd.h"
#include "../descriptors/gba_link_descriptors.h"
#include "native/host/gc/joybus_bridge.h"
#include "native/host/gc/gc_host.h"
#include "pico/time.h"
#include <string.h>
#include <stdio.h>

// ============================================================================
// DESCRIPTOR ACCESSORS
// ============================================================================

static const uint8_t* gba_link_get_device_descriptor(void) {
    return (const uint8_t*)&gba_link_device_descriptor;
}

static const uint8_t* gba_link_get_config_descriptor(void) {
    return gba_link_config_descriptor;
}

// ============================================================================
// LIFECYCLE
// ============================================================================

static void gba_link_init(void) {
    // CRITICAL: usbd_init (our caller) runs in main.c BEFORE the input
    // interfaces' init loop, which is where gc_host_init normally fires.
    // joybus_bridge_start would silently fail (gc_host_gba_acquire_for_bridge
    // checks `initialized` and returns false) and every later
    // joybus_bridge_xfer would return -1 ("not active"), making it look
    // like every joybus transfer is timing out.
    //
    // Force gc_host_init now. It's idempotent — `if (initialized) return`
    // at the top — so the later inputs[i]->init() loop is a no-op.
    gc_host_init();

    if (!joybus_bridge_start()) {
        printf("[gba_link] joybus_bridge_start() FAILED — bus owned elsewhere\n");
    } else {
        printf("[gba_link] vendor mode active, joybus owned, awaiting Dolphin\n");
    }
    printf("[gba_link] joybus_bridge state=%d (1=ACTIVE)\n",
           (int)joybus_bridge_get_state());
}

// ============================================================================
// PROTOCOL DISPATCH
// ============================================================================
//
// Joybus command lengths — same table the TCP path uses. Mirrors
// dolphin_cmd_lengths() in apps/gc2eth_feather/app.c so behavior is
// identical, only the transport (USB vs TCP) differs.
static void gba_link_cmd_lengths(uint8_t cmd, int* tx_total, int* rx_len) {
    switch (cmd) {
        case 0xFF: case 0x00: *tx_total = 1; *rx_len = 3; break;  // RESET / STATUS
        case 0x14:            *tx_total = 1; *rx_len = 5; break;  // READ
        case 0x15:            *tx_total = 5; *rx_len = 1; break;  // WRITE
        default:              *tx_total = 1; *rx_len = 1; break;  // unknown
    }
}

// Statistics — exposed via CDC (`GBALINK?` / `GBALINK!`) for sanity /
// latency debugging. Atomic 32-bit reads on Cortex-M0+ are fine without
// locks.
static volatile uint32_t s_frames_seen   = 0;
static volatile uint32_t s_short_tx      = 0;
static volatile uint32_t s_joybus_to     = 0;
static volatile uint8_t  s_last_cmd      = 0;
static volatile int8_t   s_last_n        = 0;
static volatile uint8_t  s_last_rx[5]    = {0};

// Per-command-type latency telemetry. Tracks min/max/sum/count of
// successful joybus xfer wall-clock time (start of joybus_bridge_xfer
// to return) plus retry-exhaustion counts, broken out by command byte
// so we can see whether timeouts cluster around RESET/STATUS (cold-
// start glitches) or in the WRITE burst (sustained-load glitches).
//
// Index: 0=RESET 0xFF, 1=STATUS 0x00, 2=READ 0x14, 3=WRITE 0x15.
static volatile uint32_t s_t_min[4]   = {UINT32_MAX, UINT32_MAX, UINT32_MAX, UINT32_MAX};
static volatile uint32_t s_t_max[4]   = {0, 0, 0, 0};
static volatile uint64_t s_t_sum[4]   = {0, 0, 0, 0};
static volatile uint32_t s_t_count[4] = {0, 0, 0, 0};
static volatile uint32_t s_t_to[4]    = {0, 0, 0, 0};  // retry-exhausted timeouts
static volatile uint32_t s_t_retries[4] = {0, 0, 0, 0}; // total retries triggered

// Additional integrity tracking: count WRITEs where GBA returned a
// jstat with any of the JSTAT_VALID_MASK error bits set (0xC5). Madden
// retries the whole multiboot on any single bad-jstat WRITE, so a
// non-zero count here explains the "16-attempts-to-connect" loop.
static volatile uint32_t s_write_bad_jstat = 0;
static volatile uint8_t  s_write_last_bad_jstat = 0;

static int cmd_to_idx(uint8_t cmd) {
    switch (cmd) {
        case 0xFF: return 0;
        case 0x00: return 1;
        case 0x14: return 2;
        case 0x15: return 3;
        default:   return -1;
    }
}

uint32_t gba_link_mode_get_frames(void)    { return s_frames_seen; }
uint32_t gba_link_mode_get_short_tx(void)  { return s_short_tx; }
uint32_t gba_link_mode_get_joybus_to(void) { return s_joybus_to; }
uint8_t  gba_link_mode_get_last_cmd(void)  { return s_last_cmd; }
int      gba_link_mode_get_last_n(void)    { return (int)s_last_n; }
void     gba_link_mode_get_last_rx(uint8_t out[5]) {
    for (int i = 0; i < 5; i++) out[i] = s_last_rx[i];
}

// Per-cmd telemetry accessors for GBALINK! diagnostic.
void gba_link_mode_get_timing(int idx, uint32_t* min_us, uint32_t* max_us,
                              uint32_t* avg_us, uint32_t* count,
                              uint32_t* timeouts, uint32_t* retries) {
    if (idx < 0 || idx >= 4) {
        *min_us = *max_us = *avg_us = *count = *timeouts = *retries = 0;
        return;
    }
    *count    = s_t_count[idx];
    *timeouts = s_t_to[idx];
    *retries  = s_t_retries[idx];
    *min_us   = (s_t_count[idx] == 0) ? 0 : s_t_min[idx];
    *max_us   = s_t_max[idx];
    *avg_us   = (s_t_count[idx] == 0) ? 0 : (uint32_t)(s_t_sum[idx] / s_t_count[idx]);
}

void gba_link_mode_reset_timing(void) {
    for (int i = 0; i < 4; i++) {
        s_t_min[i] = UINT32_MAX;
        s_t_max[i] = 0;
        s_t_sum[i] = 0;
        s_t_count[i] = 0;
        s_t_to[i] = 0;
        s_t_retries[i] = 0;
    }
    s_frames_seen = 0;
    s_short_tx = 0;
    s_joybus_to = 0;
    s_write_bad_jstat = 0;
    s_write_last_bad_jstat = 0;
}

uint32_t gba_link_mode_get_write_bad_jstat(uint8_t* last_jstat) {
    if (last_jstat) *last_jstat = s_write_last_bad_jstat;
    return s_write_bad_jstat;
}

// Process one command from the vendor bulk-out endpoint. Returns true
// if a command was processed; false if no bytes were available. Called
// in a tight loop from gba_link_task().
static bool process_one_frame(void) {
    // Need at least 1 byte (the command byte) to know what to do.
    if (tud_vendor_n_available(0) < 1) return false;

    uint8_t cmd_byte = 0;
    if (tud_vendor_n_read(0, &cmd_byte, 1) != 1) return false;
    s_last_cmd = cmd_byte;

    int tx_total = 1, rx_len = 1;
    gba_link_cmd_lengths(cmd_byte, &tx_total, &rx_len);

    // Read the rest of the command payload (4 more bytes for WRITE).
    // Bounded wait — Dolphin should send the whole 5-byte WRITE in one
    // bulk packet so this should drain immediately, but we tolerate
    // brief gaps.
    uint8_t tx[5] = { cmd_byte };
    if (tx_total > 1) {
        int got = 0;
        absolute_time_t deadline = make_timeout_time_us(5000);
        while (got < tx_total - 1) {
            uint32_t n = tud_vendor_n_read(0, tx + 1 + got, tx_total - 1 - got);
            if (n > 0) { got += (int)n; continue; }
            if (time_reached(deadline)) break;
            tud_task();  // keep the USB stack ticking
        }
        if (got != tx_total - 1) {
            s_short_tx++;
            return false;
        }
    }

    // Forward to the GBA via joybus. Generous 5 ms timeout for ALL
    // commands — the GBA's BIOS has Kawasedo cipher latency on READs,
    // and under sustained burst load even STATUS / WRITE replies can
    // creep close to 1 ms. Tighter timeouts here cause spurious
    // failures during Madden multiboot. Joybus itself is still
    // ~250 µs/cmd in steady state; the extra budget only kicks in
    // when something is actually slow.
    // Joybus timeout, per-command-type:
    //   * WRITE / READ / RESET: 5 ms — generous; telemetry shows max
    //     observed wall-clock <250 µs on a healthy session.
    //   * STATUS: 30 ms with 5 retries (~150 ms worst case). Tried
    //     50 ms × 4 — Madden's body burst would interleave a STATUS
    //     that stalled for ~200 ms, the GBA's BIOS lost multiboot
    //     state during that gap, and the result was "chime plays then
    //     black screen" (cipher accepted header but body validation
    //     failed). 30 ms is the sweet spot that lets STATUS retries
    //     happen without breaking the GBA-side timing.
    uint8_t rx[5] = {0};
    const int t_idx = cmd_to_idx(cmd_byte);
    const uint32_t xfer_to_us = (cmd_byte == 0x00) ? 30000 : 5000;
    absolute_time_t t_start = get_absolute_time();
    int n = joybus_bridge_xfer(tx, (uint16_t)tx_total,
                               rx, (uint16_t)rx_len, xfer_to_us);

    // Retry on timeout for ALL commands, not just RESET/STATUS. A joybus
    // timeout means the GBA never sent a jstat reply, which in turn means
    // the GBA's BIOS / multiboot state never finished processing the
    // command — so it's safe to re-transmit. Cipher state only advances
    // when GBA actually acks the byte (returns a valid jstat).
    //
    // Why this matters: Madden 2003 multiboot sends ~4000 WRITEs. With
    // a 0.3% per-cmd timeout rate (observed) and no WRITE retries, the
    // probability of all 4000 WRITEs succeeding is ~0%; Madden has to
    // retry the entire multiboot sequence many times before getting
    // lucky enough to land an attempt with zero timeouts. Retrying
    // WRITEs internally collapses those retries from "minutes of full
    // multiboot replays" to "a few extra ms of inline retry."
    //
    // RESET (0xFF) and STATUS (0x00) get a modest 5-retry budget; very
    // first command after a GBA cold-start fails ~50% of the time even
    // here. Once joybus is "warm" the rate drops dramatically.
    //
    // Tried 50 retries → made things WORSE: when joybus genuinely
    // glitches (not just a one-off miss), the long retry chain stalled
    // the USB pipe for 50+ ms per failure and Madden saw the whole
    // command stream pause. Better to fail fast and let Madden's own
    // higher-level retry handle the rare exhausted case.
    int max_retries = (cmd_byte == 0xFF || cmd_byte == 0x00) ? 5 : 2;
    for (int retry = 0; retry < max_retries && n < 0; retry++) {
        busy_wait_us(300);
        if (t_idx >= 0) s_t_retries[t_idx]++;
        n = joybus_bridge_xfer(tx, (uint16_t)tx_total,
                               rx, (uint16_t)rx_len, xfer_to_us);
    }

    // Record timing — total wall-clock from start of first xfer through
    // whichever attempt finally succeeded (or all retries exhausted).
    if (t_idx >= 0) {
        uint64_t dt_us = absolute_time_diff_us(t_start, get_absolute_time());
        if (dt_us > UINT32_MAX) dt_us = UINT32_MAX;
        uint32_t dt = (uint32_t)dt_us;
        if (n >= 0) {
            // Success: feed into min/max/sum/count.
            s_t_count[t_idx]++;
            s_t_sum[t_idx] += dt;
            if (dt < s_t_min[t_idx]) s_t_min[t_idx] = dt;
            if (dt > s_t_max[t_idx]) s_t_max[t_idx] = dt;
        } else {
            // All retries exhausted.
            s_t_to[t_idx]++;
        }
    }

    // WRITE jstat-validity check. The GBA returns a single jstat byte
    // after each WRITE; any of the JSTAT_VALID_MASK bits (0xC5) being
    // set indicates the GBA flagged the byte as invalid (parity error,
    // overrun, etc). eth-multiboot.js bails on a single bad-jstat
    // WRITE; Madden likewise restarts the whole multiboot. Count these
    // so we can see if WRITE corruption is the real bottleneck.
    if (cmd_byte == 0x15 && n >= 1 && (rx[0] & 0xC5)) {
        s_write_bad_jstat++;
        s_write_last_bad_jstat = rx[0];
    }

    // Brief idle gap between xfers — gives the joybus PIO state
    // machine and the GBA's link IC time to fully settle before the
    // next command. Without it, Madden's back-to-back command bursts
    // had ~3% success rate; pyusb's naturally-paced testing was 100%.
    busy_wait_us(150);
    if (n < 0) {
        // Joybus retry-exhausted timeout.
        s_joybus_to++;
        if (cmd_byte == 0x00 && rx_len >= 3) {
            // STATUS: avoid zero-fill (Madden reads type=0x0000 as
            // "GBA disconnected" and aborts the connection). Return a
            // plausible "GBA present, multiboot-ready" placeholder so
            // Madden keeps polling instead of giving up. We don't fake
            // WRITE state — real STATUS replies that arrive in time
            // still pass through unchanged.
            rx[0] = 0x00; rx[1] = 0x04;  // type = 0x0400
            rx[2] = 0x18;                // jstat = PSF0 | RECV
            for (int i = 3; i < rx_len; i++) rx[i] = 0;
        } else {
            memset(rx, 0, rx_len);
        }
        n = rx_len;
    } else {
        for (int i = n; i < rx_len; i++) rx[i] = 0;
    }

    // Send the reply back to Dolphin.
    tud_vendor_n_write(0, rx, rx_len);
    tud_vendor_n_write_flush(0);

    s_frames_seen++;
    s_last_n = (int8_t)n;
    for (int i = 0; i < 5; i++) s_last_rx[i] = (i < rx_len) ? rx[i] : 0;
    return true;
}

static void gba_link_task(void) {
    // Drain everything that's pending on the vendor bulk-out endpoint.
    while (process_one_frame()) { /* keep going while data flows */ }
}

// ============================================================================
// REPORT SENDING — N/A for vendor mode (no controller HID)
// ============================================================================

static bool gba_link_send_report(uint8_t player_index,
                                 const input_event_t* event,
                                 const profile_output_t* profile_out,
                                 uint32_t buttons) {
    (void)player_index; (void)event; (void)profile_out; (void)buttons;
    // Not a HID gamepad — bridge mode does not consume controller events.
    return true;
}

static bool gba_link_is_ready(void) {
    return tud_vendor_n_mounted(0);
}

// ============================================================================
// MODE STRUCT
// ============================================================================

const usbd_mode_t gba_link_mode = {
    .name                  = "GBA Link (Dolphin)",
    .mode                  = USB_OUTPUT_MODE_GBA_LINK,
    .get_device_descriptor = gba_link_get_device_descriptor,
    .get_config_descriptor = gba_link_get_config_descriptor,
    .get_report_descriptor = NULL,                  // vendor class, no HID
    .init                  = gba_link_init,
    .send_report           = gba_link_send_report,
    .is_ready              = gba_link_is_ready,
    .handle_output         = NULL,
    .get_rumble            = NULL,
    .get_feedback          = NULL,
    .get_report            = NULL,
    .get_class_driver      = NULL,                  // tinyusb's built-in vendor class
    .task                  = gba_link_task,
};

#endif // CFG_TUD_VENDOR && CONFIG_JOYBUS_BRIDGE
