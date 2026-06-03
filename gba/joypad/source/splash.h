// splash.h — boot-time splash overlay for the gc2usb GBA payload.
// Renders a per-USB-output-mode "you are now in MODE X" badge in the
// framebuffer for ~1 second after multiboot, before yielding to the
// normal eyes_anim main loop.
//
// All drawing uses the existing display.c primitives (hspan + pixel)
// so we stay in the same Mode-4 framebuffer the eyes use.
#ifndef SPLASH_H
#define SPLASH_H

#include <stdint.h>
#include "mode_marker.h"

// How long to hold the splash (in 60 Hz VBlank frames).
//   60 = ~1.0 s
//  120 = ~2.0 s
//  180 = ~3.0 s
#define SPLASH_FRAMES 180

// Render the splash for the given mode ID into the shadow buffer.
// Call display_present() afterwards to push to VRAM.
void splash_render(joypad_mode_id_t mode);

// Returns the eye-color (fg) and pupil-color the caller should call
// display_set_fg_color/display_set_pupil_color with for this mode,
// so the splash's color theme matches subsequent eye states.
void splash_palette(joypad_mode_id_t mode, uint16_t* fg, uint16_t* pupil);

#endif
