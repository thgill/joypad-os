// splash_image.h — bitmap-based splash for the gc2usb GBA payload.
// Replaces / augments the vector splash in splash.c with real PNG art
// converted to GBA Mode-3 (BGR555 direct-color) blits.
//
// Workflow:
//   1. Drop a PNG into  gba/joypad/assets/<name>.png
//   2. Run             gba/tools/png_to_splash.py
//   3. Reference       splash_img_<name> in splash_image_for()
//
// Mode-3 is single-page (no double buffer) and 16bpp, so the splash
// happens BEFORE display_init() puts us into Mode-4 for the eyes, or
// the renderer hot-swaps DISPCNT and we let display.c reclaim Mode-4
// when the splash hold ends.

#ifndef SPLASH_IMAGE_H
#define SPLASH_IMAGE_H

#include <stdint.h>
#include "mode_marker.h"

typedef struct {
    // 8bpp indexed pixels (1 byte per pixel, palette index).
    // Used when `palette` is non-NULL.
    const uint8_t*  pixels8;
    // 256-entry BGR555 palette for `pixels8`. NULL means use 16bpp pixels.
    const uint16_t* palette;
    // 16bpp BGR555 pixels — used when `palette` is NULL.
    const uint16_t* pixels;
    uint16_t        width;
    uint16_t        height;
    uint16_t        bg_color;  // BGR555 — fills any uncovered margin
} splash_image_t;

// Returns the image for `mode`, or NULL if no image is configured.
// NULL = caller should use splash_image_fallback() to get a generic
// image and overlay the mode-name text on it.
const splash_image_t* splash_image_for(joypad_mode_id_t mode);

// Returns a generic Joypad-branded image used as the fallback for any
// mode without a dedicated splash. NULL if no fallback is linked into
// this build (legacy path — caller would then use splash.c vector).
const splash_image_t* splash_image_fallback(void);

// Switch the GBA to Mode-3 and blit `img` centered with bg_color fill.
// VRAM at 0x06000000 is written directly. Caller is responsible for
// returning to Mode-4 (set DISPCNT to 0x0004 | 0x0400) before resuming
// the eye animation.
void splash_image_render(const splash_image_t* img);

// Overlay a NUL-terminated string onto the currently-rendered Mode-4
// 8bpp framebuffer using palette index 255 (reserved white text — see
// png_to_splash.py). Centered horizontally on cx; top of glyphs at y.
// Supports ' ', '0'-'9', 'A'-'Z', '+', '-', '/'. Other chars render as
// blank space.
void splash_image_overlay_text(int cx, int y, const char* text);

// Returns the human-readable mode name for the overlay (e.g. "SINPUT",
// "KEYBOARD MOUSE", "MODE 7" for unknown). Always returns a non-NULL
// pointer suitable for splash_image_overlay_text().
const char* splash_image_mode_name(joypad_mode_id_t mode);

#endif
