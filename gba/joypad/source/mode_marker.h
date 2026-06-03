// mode_marker.h — host-patchable byte in the multiboot ROM that signals
// the gc2usb firmware's current USB output mode. The firmware finds
// `g_joypad_mode_marker` in the .gba binary by scanning for the magic
// prefix ("JPMD"), patches the `mode` byte to match usbd_get_mode()
// just before multiboot upload, and the splash code on the GBA side
// reads it at boot to pick which logo to render.
//
// Keep the layout stable — the firmware-side scanner in
// src/native/host/gc/gba_multiboot.c depends on the exact byte
// positions of magic + mode.
#ifndef JOYPAD_MODE_MARKER_H
#define JOYPAD_MODE_MARKER_H

#include <stdint.h>

#define JOYPAD_MODE_MAGIC0 'J'
#define JOYPAD_MODE_MAGIC1 'P'
#define JOYPAD_MODE_MAGIC2 'M'
#define JOYPAD_MODE_MAGIC3 'D'

// Mode IDs — MUST match usb_output_mode_t in src/usb/usbd/usbd.h.
// If you reorder that enum, update this one OR the firmware patches
// the wrong byte. Default value 0xFF = "uninitialized" (firmware
// hasn't patched this build); splash falls back to a neutral default.
typedef enum {
    JOYPAD_MODE_HID            = 0,
    JOYPAD_MODE_SINPUT         = 1,
    JOYPAD_MODE_XINPUT         = 2,
    JOYPAD_MODE_PS3            = 3,
    JOYPAD_MODE_PS4            = 4,
    JOYPAD_MODE_SWITCH         = 5,
    JOYPAD_MODE_PSCLASSIC      = 6,
    JOYPAD_MODE_XBOX_ORIGINAL  = 7,
    JOYPAD_MODE_XBONE          = 8,
    JOYPAD_MODE_XAC            = 9,
    JOYPAD_MODE_KEYBOARD_MOUSE = 10,
    JOYPAD_MODE_GC_ADAPTER     = 11,
    JOYPAD_MODE_PCEMINI        = 12,
    JOYPAD_MODE_CDC            = 13,
    JOYPAD_MODE_GBA_LINK       = 14,
    JOYPAD_MODE_UNKNOWN        = 0xFF,
} joypad_mode_id_t;

// Layout MUST stay: 4-byte magic, 1-byte mode, 3-byte reserved.
// firmware patches byte[offset + 4] to the mode value.
struct joypad_mode_marker {
    char    magic[4];
    uint8_t mode;
    uint8_t reserved[3];
};

extern volatile const struct joypad_mode_marker g_joypad_mode_marker;

#endif
