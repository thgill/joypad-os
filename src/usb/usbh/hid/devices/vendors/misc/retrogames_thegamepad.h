// retrogames_thegamepad.h
// Driver for Retro Games Ltd "THEGamepad" (VID 0x1c59 PID 0x0026)
// As found in the A500 Mini, C64 Mini, etc.
//
// HID report layout (8 bytes, no report ID):
//   Byte 0:      X axis (0x00=left, 0x7f=center, 0xff=right)
//   Byte 1-4:    Y axis (0x00=up, 0x7f=center, 0xff=down) — bytes 2-4 are duplicates
//   Byte 5[3:0]: Padding (always 0xf)
//   Byte 5[4]:   Y (yellow)
//   Byte 5[5]:   B (blue)
//   Byte 5[6]:   A (red)
//   Byte 5[7]:   X (green)
//   Byte 6[0]:   L shoulder
//   Byte 6[1]:   R shoulder
//   Byte 6[2]:   Menu (Select)
//   Byte 6[3]:   Home (Start)
//   Byte 6[4:7]: Unused
//   Byte 7:      Vendor buttons (0x40 always set at baseline)

#pragma once

#include "../../../hid_device.h"
#include "../../../hid_utils.h"
#include "tusb.h"

typedef struct TU_ATTR_PACKED {
    uint8_t axis_x;     // byte 0
    uint8_t axis_y;     // byte 1 (bytes 2-4 are duplicates, ignored)
    uint8_t axis_dup1;  // byte 2
    uint8_t axis_dup2;  // byte 3
    uint8_t axis_dup3;  // byte 4
    struct {
        uint8_t padding  : 4;  // bits 0-3: always 0xf
        uint8_t btn_y    : 1;  // bit 4: Y (yellow)
        uint8_t btn_b    : 1;  // bit 5: B (blue)
        uint8_t btn_a    : 1;  // bit 6: A (red)
        uint8_t btn_x    : 1;  // bit 7: X (green)
    };
    struct {
        uint8_t btn_l    : 1;  // bit 0: L shoulder
        uint8_t btn_r    : 1;  // bit 1: R shoulder
        uint8_t btn_menu : 1;  // bit 2: Menu (Select)
        uint8_t btn_home : 1;  // bit 3: Home (Start)
        uint8_t unused   : 4;  // bits 4-7
    };
    uint8_t vendor_buttons; // byte 7
} thegamepad_report_t;

extern DeviceInterface retrogames_thegamepad_interface;
