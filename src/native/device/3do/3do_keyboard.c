// 3do_keyboard.c - USB HID keyboard → 3DO PBUS PS/2 Set 2 emulation
// See header for protocol overview.

#include "3do_keyboard.h"
#include "3do_device.h"
#include <string.h>

#define KB_QUEUE_SIZE 64        // per-slot ring buffer (PS/2 bytes)
#define HID_USAGE_TABLE_SIZE 0xE8

// HID Keyboard Page (0x07) usage ID → PS/2 Set 2 scancode.
// Encoding: low 8 bits = scancode, bit 8 = 1 → emit 0xE0 prefix first (extended).
// Zero = unmapped (skip silently).
//
// Reference: https://wiki.osdev.org/PS/2_Keyboard#Scan_Code_Set_2 cross-checked
// with the Linux kernel atkbd map. Set 2 specifically — Set 1 is XT-legacy,
// Set 3 is rarely supported. Pause and PrintScreen are weird multi-byte
// sequences in real PS/2; for v1 we emit a single byte that's enough for the
// host driverlet to recognize and not enough to confuse its prefix state.
static const uint16_t hid_to_ps2_set2[HID_USAGE_TABLE_SIZE] = {
    [0x04] = 0x001C, // A
    [0x05] = 0x0032, // B
    [0x06] = 0x0021, // C
    [0x07] = 0x0023, // D
    [0x08] = 0x0024, // E
    [0x09] = 0x002B, // F
    [0x0A] = 0x0034, // G
    [0x0B] = 0x0033, // H
    [0x0C] = 0x0043, // I
    [0x0D] = 0x003B, // J
    [0x0E] = 0x0042, // K
    [0x0F] = 0x004B, // L
    [0x10] = 0x003A, // M
    [0x11] = 0x0031, // N
    [0x12] = 0x0044, // O
    [0x13] = 0x004D, // P
    [0x14] = 0x0015, // Q
    [0x15] = 0x002D, // R
    [0x16] = 0x001B, // S
    [0x17] = 0x002C, // T
    [0x18] = 0x003C, // U
    [0x19] = 0x002A, // V
    [0x1A] = 0x001D, // W
    [0x1B] = 0x0022, // X
    [0x1C] = 0x0035, // Y
    [0x1D] = 0x001A, // Z
    [0x1E] = 0x0016, // 1
    [0x1F] = 0x001E, // 2
    [0x20] = 0x0026, // 3
    [0x21] = 0x0025, // 4
    [0x22] = 0x002E, // 5
    [0x23] = 0x0036, // 6
    [0x24] = 0x003D, // 7
    [0x25] = 0x003E, // 8
    [0x26] = 0x0046, // 9
    [0x27] = 0x0045, // 0
    [0x28] = 0x005A, // Enter
    [0x29] = 0x0076, // Esc
    [0x2A] = 0x0066, // Backspace
    [0x2B] = 0x000D, // Tab
    [0x2C] = 0x0029, // Space
    [0x2D] = 0x004E, // -_
    [0x2E] = 0x0055, // =+
    [0x2F] = 0x0054, // [{
    [0x30] = 0x005B, // ]}
    [0x31] = 0x005D, // \|
    [0x33] = 0x004C, // ;:
    [0x34] = 0x0052, // '"
    [0x35] = 0x000E, // `~
    [0x36] = 0x0041, // ,<
    [0x37] = 0x0049, // .>
    [0x38] = 0x004A, // /?
    [0x39] = 0x0058, // CapsLock
    [0x3A] = 0x0005, // F1
    [0x3B] = 0x0006, // F2
    [0x3C] = 0x0004, // F3
    [0x3D] = 0x000C, // F4
    [0x3E] = 0x0003, // F5
    [0x3F] = 0x000B, // F6
    [0x40] = 0x0083, // F7
    [0x41] = 0x000A, // F8
    [0x42] = 0x0001, // F9
    [0x43] = 0x0009, // F10
    [0x44] = 0x0078, // F11
    [0x45] = 0x0007, // F12
    [0x47] = 0x007E, // ScrollLock
    [0x49] = 0x0170, // Insert      (extended)
    [0x4A] = 0x016C, // Home        (extended)
    [0x4B] = 0x017D, // PageUp      (extended)
    [0x4C] = 0x0171, // Delete      (extended)
    [0x4D] = 0x0169, // End         (extended)
    [0x4E] = 0x017A, // PageDown    (extended)
    [0x4F] = 0x0174, // RightArrow  (extended)
    [0x50] = 0x016B, // LeftArrow   (extended)
    [0x51] = 0x0172, // DownArrow   (extended)
    [0x52] = 0x0175, // UpArrow     (extended)
    [0x53] = 0x0077, // NumLock
    [0x54] = 0x014A, // KP /        (extended)
    [0x55] = 0x007C, // KP *
    [0x56] = 0x007B, // KP -
    [0x57] = 0x0079, // KP +
    [0x58] = 0x015A, // KP Enter    (extended)
    [0x59] = 0x0069, // KP 1
    [0x5A] = 0x0072, // KP 2
    [0x5B] = 0x007A, // KP 3
    [0x5C] = 0x006B, // KP 4
    [0x5D] = 0x0073, // KP 5
    [0x5E] = 0x0074, // KP 6
    [0x5F] = 0x006C, // KP 7
    [0x60] = 0x0075, // KP 8
    [0x61] = 0x007D, // KP 9
    [0x62] = 0x0070, // KP 0
    [0x63] = 0x0071, // KP .
    [0xE0] = 0x0014, // LCtrl
    [0xE1] = 0x0012, // LShift
    [0xE2] = 0x0011, // LAlt
    [0xE3] = 0x011F, // LGUI        (extended)
    [0xE4] = 0x0114, // RCtrl       (extended)
    [0xE5] = 0x0059, // RShift
    [0xE6] = 0x0111, // RAlt        (extended)
    [0xE7] = 0x0127, // RGUI        (extended)
};

// Map each USB HID modifier bit (in event->kb_modifier) to its
// corresponding modifier-key HID usage ID — lets us reuse the
// hid_to_ps2_set2 table for modifier transitions.
//   bit 0 = LCtrl  → 0xE0   bit 4 = RCtrl  → 0xE4
//   bit 1 = LShift → 0xE1   bit 5 = RShift → 0xE5
//   bit 2 = LAlt   → 0xE2   bit 6 = RAlt   → 0xE6
//   bit 3 = LGUI   → 0xE3   bit 7 = RGUI   → 0xE7
static const uint8_t modifier_bit_to_hid[8] = {
    0xE0, 0xE1, 0xE2, 0xE3, 0xE4, 0xE5, 0xE6, 0xE7,
};

typedef struct {
    uint8_t queue[KB_QUEUE_SIZE];
    uint8_t qhead;
    uint8_t qtail;
    uint8_t last_byte_sent;     // For the alternation pattern
    bool initialized;           // Has INITOK (0xAA) been queued for this slot?
    uint8_t prev_modifier;
    uint8_t prev_keys[6];
} kb_state_t;

static kb_state_t kb_state[MAX_PLAYERS];

static void kb_push(uint8_t slot, uint8_t b)
{
    uint8_t next = (uint8_t)((kb_state[slot].qtail + 1) % KB_QUEUE_SIZE);
    if (next == kb_state[slot].qhead) {
        // Queue full — drop the byte. With KB_QUEUE_SIZE=64 and 60Hz drain
        // this only happens under bursts well beyond human typing speed.
        return;
    }
    kb_state[slot].queue[kb_state[slot].qtail] = b;
    kb_state[slot].qtail = next;
}

static bool kb_queue_empty(uint8_t slot)
{
    return kb_state[slot].qhead == kb_state[slot].qtail;
}

static uint8_t kb_pop(uint8_t slot)
{
    if (kb_queue_empty(slot)) return 0;
    uint8_t b = kb_state[slot].queue[kb_state[slot].qhead];
    kb_state[slot].qhead = (uint8_t)((kb_state[slot].qhead + 1) % KB_QUEUE_SIZE);
    return b;
}

static void kb_push_down(uint8_t slot, uint8_t hid_usage)
{
    if (hid_usage >= HID_USAGE_TABLE_SIZE) return;
    uint16_t enc = hid_to_ps2_set2[hid_usage];
    if (enc == 0) return;
    if (enc & 0x100) kb_push(slot, 0xE0);
    kb_push(slot, (uint8_t)(enc & 0xFF));
}

static void kb_push_up(uint8_t slot, uint8_t hid_usage)
{
    if (hid_usage >= HID_USAGE_TABLE_SIZE) return;
    uint16_t enc = hid_to_ps2_set2[hid_usage];
    if (enc == 0) return;
    if (enc & 0x100) kb_push(slot, 0xE0);
    kb_push(slot, 0xF0);
    kb_push(slot, (uint8_t)(enc & 0xFF));
}

void tdo_kb_process_event(uint8_t slot, const input_event_t* event)
{
    if (slot >= MAX_PLAYERS || event == NULL) return;

    // USB HID boot-protocol phantom roll-over: when 7+ keys are held the
    // keyboard sends {0x01,0x01,0x01,0x01,0x01,0x01} (KeyErrorRollOver) to
    // signal "too many keys, can't enumerate". Treat as no-change so the
    // matrix preserves whatever was last reported instead of dropping to 0.
    bool phantom = true;
    for (uint8_t i = 0; i < 6; i++) {
        if (event->kb_keys[i] != 0x01) { phantom = false; break; }
    }
    if (phantom) return;

    if (!kb_state[slot].initialized) {
        // First event for this slot — pretend we just finished a PS/2 self-test
        // so the host driverlet clears its 256-bit key matrix.
        kb_push(slot, 0xAA);
        kb_state[slot].initialized = true;
        kb_state[slot].prev_modifier = 0;
        memset(kb_state[slot].prev_keys, 0, sizeof(kb_state[slot].prev_keys));
    }

    // Modifier transitions
    uint8_t cur_mod = event->kb_modifier;
    uint8_t changed = (uint8_t)(cur_mod ^ kb_state[slot].prev_modifier);
    for (uint8_t bit = 0; bit < 8; bit++) {
        uint8_t mask = (uint8_t)(1u << bit);
        if (!(changed & mask)) continue;
        if (cur_mod & mask) {
            kb_push_down(slot, modifier_bit_to_hid[bit]);
        } else {
            kb_push_up(slot, modifier_bit_to_hid[bit]);
        }
    }
    kb_state[slot].prev_modifier = cur_mod;

    // New keys (in current but not in previous) → press
    for (uint8_t i = 0; i < 6; i++) {
        uint8_t k = event->kb_keys[i];
        if (k == 0) continue;
        bool was = false;
        for (uint8_t j = 0; j < 6; j++) {
            if (kb_state[slot].prev_keys[j] == k) { was = true; break; }
        }
        if (!was) kb_push_down(slot, k);
    }

    // Removed keys (in previous but not in current) → release
    for (uint8_t i = 0; i < 6; i++) {
        uint8_t k = kb_state[slot].prev_keys[i];
        if (k == 0) continue;
        bool still = false;
        for (uint8_t j = 0; j < 6; j++) {
            if (event->kb_keys[j] == k) { still = true; break; }
        }
        if (!still) kb_push_up(slot, k);
    }

    memcpy(kb_state[slot].prev_keys, event->kb_keys, sizeof(kb_state[slot].prev_keys));
}

uint8_t tdo_kb_next_byte(uint8_t slot)
{
    if (slot >= MAX_PLAYERS) return 0;

    uint8_t next;
    if (kb_queue_empty(slot)) {
        // No pending bytes — go idle. The driverlet's "inByte != 0 && inByte
        // != lastByte" guard treats 0 as "no event this frame", so this
        // doesn't disturb prefix state mid-sequence.
        next = 0x00;
    } else {
        uint8_t peek = kb_state[slot].queue[kb_state[slot].qhead];
        if (peek == kb_state[slot].last_byte_sent && peek != 0x00) {
            // Next queued byte equals what we just sent — the driverlet's
            // change-detection would skip it. Insert one 0x00 spacer.
            // Common case: press→release→press of the same key produces
            // [..., scancode, F0, scancode, scancode, F0, scancode] and
            // only the duplicate-scancode boundary needs the separator.
            // Mixed sequences (scancode, F0, scancode | E0, scancode | etc.)
            // flow back-to-back at one byte per PBUS poll (~16ms each).
            next = 0x00;
        } else {
            next = kb_pop(slot);
        }
    }
    kb_state[slot].last_byte_sent = next;
    return next;
}
