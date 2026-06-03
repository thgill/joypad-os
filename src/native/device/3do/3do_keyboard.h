// 3do_keyboard.h - USB HID keyboard → 3DO PBUS PS/2 Set 2 emulation
//
// The 3DO keyboard driverlet (Portfolio OS, never shipped publicly but
// fully implemented in joypad-ai/portfolio_os branch feature/keyboard-decoder)
// consumes a PS/2 Set 2 byte stream from byte 1 of each PBUS field
// (3-byte device class, ID 0x02 or 0x4B).
//
// Inputs: full USB HID kb state per slot (8-bit modifier + 6 keycodes).
// Output: per-slot PS/2 byte queue with one byte popped per PBUS poll.
//
// Spec: joypad-tester/.dev/docs/3do_keyboard_protocol.md

#ifndef _3DO_KEYBOARD_H
#define _3DO_KEYBOARD_H

#include <stdint.h>
#include <stdbool.h>
#include "core/input_event.h"

// Diff the slot's previous HID kb state against this event, push PS/2 down/up
// sequences (with 0xE0 extended prefixes and 0xF0 release prefixes) into the
// per-slot ring buffer. On first call for a slot, also enqueues 0xAA so the
// host driverlet sees the equivalent of a PS/2 self-test pass and clears its
// key matrix.
void tdo_kb_process_event(uint8_t slot, const input_event_t* event);

// Pop the next byte to put in the report's scancode slot. Implements the
// alternation pattern (every other PBUS frame is 0x00) so the driverlet's
// change-detection always sees a transition between consecutive bytes —
// required when the queue would otherwise emit the same scancode twice
// (e.g. press-release-press of one key).
uint8_t tdo_kb_next_byte(uint8_t slot);

#endif // _3DO_KEYBOARD_H
