// xid_mode.c - Original Xbox (XID) USB device mode
// SPDX-License-Identifier: Apache-2.0
// Copyright 2024 Robert Dale Smith

#include "tusb.h"
#include "../usbd_mode.h"
#include "../usbd.h"
#include "../drivers/tud_xid.h"
#include "descriptors/xbox_og_descriptors.h"
#include "core/buttons.h"
#include <string.h>

// ============================================================================
// STATE
// ============================================================================

static xbox_og_in_report_t xid_report;
static xbox_og_out_report_t xid_rumble;
static bool xid_rumble_available = false;

// ============================================================================
// CONVERSION HELPERS
// ============================================================================

// Convert Joypad buttons to Xbox OG digital buttons (byte 2)
static uint8_t convert_xid_digital_buttons(uint32_t buttons)
{
    uint8_t xog_buttons = 0;

    if (buttons & JP_BUTTON_DU) xog_buttons |= XBOX_OG_BTN_DPAD_UP;
    if (buttons & JP_BUTTON_DD) xog_buttons |= XBOX_OG_BTN_DPAD_DOWN;
    if (buttons & JP_BUTTON_DL) xog_buttons |= XBOX_OG_BTN_DPAD_LEFT;
    if (buttons & JP_BUTTON_DR) xog_buttons |= XBOX_OG_BTN_DPAD_RIGHT;
    if (buttons & JP_BUTTON_S2) xog_buttons |= XBOX_OG_BTN_START;
    if (buttons & JP_BUTTON_S1) xog_buttons |= XBOX_OG_BTN_BACK;
    if (buttons & JP_BUTTON_L3) xog_buttons |= XBOX_OG_BTN_L3;
    if (buttons & JP_BUTTON_R3) xog_buttons |= XBOX_OG_BTN_R3;

    return xog_buttons;
}

// Convert analog value from Joypad (0-255, center 128) to Xbox OG signed 16-bit
static int16_t convert_axis_to_s16(uint8_t value)
{
    int32_t scaled = ((int32_t)value - 128) * 256;
    if (scaled < -32768) scaled = -32768;
    if (scaled > 32767) scaled = 32767;
    return (int16_t)scaled;
}

// Same conversion but inverted for Y axes. Our core normalizes Y to HID
// convention (0=up, 255=down) but Xbox signed sticks are positive=up, so the Y
// axes must be flipped (mirrors OGX-Mini's Range::invert on joystick_ly/ry).
static int16_t convert_axis_to_s16_inv(uint8_t value)
{
    int32_t scaled = (128 - (int32_t)value) * 256;
    if (scaled < -32768) scaled = -32768;
    if (scaled > 32767) scaled = 32767;
    return (int16_t)scaled;
}

// ============================================================================
// MODE INTERFACE IMPLEMENTATION
// ============================================================================

static void xid_mode_init(void)
{
    // Initialize XID report to neutral state
    memset(&xid_report, 0, sizeof(xbox_og_in_report_t));
    xid_report.reserved1 = 0x00;
    xid_report.report_len = sizeof(xbox_og_in_report_t);
    memset(&xid_rumble, 0, sizeof(xbox_og_out_report_t));
    xid_rumble_available = false;
}

static bool xid_mode_is_ready(void)
{
    return tud_xid_ready();
}

static bool xid_mode_send_report(uint8_t player_index,
                                  const input_event_t* event,
                                  const profile_output_t* profile_out,
                                  uint32_t buttons)
{
    (void)player_index;
    (void)event;

    // Digital buttons (DPAD, Start, Back, L3, R3)
    xid_report.buttons = convert_xid_digital_buttons(buttons);

    // Analog face + black/white buttons (0 = released, 255 = fully pressed).
    // The Duke reports all six as 8-bit pressure; the console derives the
    // digital press from a threshold. Use real per-button pressure when the
    // source provides it (DualShock 2, OG Xbox via XInput, Wii drums), else
    // fall back to digital full-scale. pressure[] is DS3 order:
    // up,right,down,left,L2,R2,L1,R1,triangle,circle,cross,square.
    // White=L1, Black=R1 matches the codebase convention in xinput.c.
    if (profile_out->has_pressure) {
        xid_report.a     = profile_out->pressure[10];  // cross   (B1)
        xid_report.b     = profile_out->pressure[9];   // circle  (B2)
        xid_report.x     = profile_out->pressure[11];  // square  (B3)
        xid_report.y     = profile_out->pressure[8];   // triangle(B4)
        xid_report.white = profile_out->pressure[6];   // L1
        xid_report.black = profile_out->pressure[7];   // R1
    } else {
        xid_report.a     = (buttons & JP_BUTTON_B1) ? 0xFF : 0x00;
        xid_report.b     = (buttons & JP_BUTTON_B2) ? 0xFF : 0x00;
        xid_report.x     = (buttons & JP_BUTTON_B3) ? 0xFF : 0x00;
        xid_report.y     = (buttons & JP_BUTTON_B4) ? 0xFF : 0x00;
        xid_report.white = (buttons & JP_BUTTON_L1) ? 0xFF : 0x00;  // L1 -> White
        xid_report.black = (buttons & JP_BUTTON_R1) ? 0xFF : 0x00;  // R1 -> Black
    }

    // Analog triggers (0-255)
    // Use profile analog values, fall back to digital if analog is 0 but button pressed
    xid_report.trigger_l = profile_out->l2_analog;
    xid_report.trigger_r = profile_out->r2_analog;
    if (xid_report.trigger_l == 0 && (buttons & JP_BUTTON_L2)) xid_report.trigger_l = 0xFF;
    if (xid_report.trigger_r == 0 && (buttons & JP_BUTTON_R2)) xid_report.trigger_r = 0xFF;

    // Analog sticks (signed 16-bit, -32768 to +32767)
    xid_report.stick_lx = convert_axis_to_s16(profile_out->left_x);
    xid_report.stick_ly = convert_axis_to_s16_inv(profile_out->left_y);
    xid_report.stick_rx = convert_axis_to_s16(profile_out->right_x);
    xid_report.stick_ry = convert_axis_to_s16_inv(profile_out->right_y);

    return tud_xid_send_report(&xid_report);
}

static void xid_mode_task(void)
{
    // Check for rumble updates
    if (tud_xid_get_rumble(&xid_rumble)) {
        xid_rumble_available = true;
    }
}

static uint8_t xid_mode_get_rumble(void)
{
    // Xbox OG has two 16-bit motors - combine to single 8-bit value
    uint16_t max_rumble = (xid_rumble.rumble_l > xid_rumble.rumble_r)
                          ? xid_rumble.rumble_l : xid_rumble.rumble_r;
    return (uint8_t)(max_rumble >> 8);  // Scale 0-65535 to 0-255
}

static bool xid_mode_get_feedback(output_feedback_t* fb)
{
    // Xbox OG has two 16-bit motors
    fb->rumble_left = (uint8_t)(xid_rumble.rumble_l >> 8);
    fb->rumble_right = (uint8_t)(xid_rumble.rumble_r >> 8);
    fb->dirty = true;
    return true;
}

static const usbd_class_driver_t* xid_mode_get_class_driver(void)
{
    return tud_xid_class_driver();
}

static const uint8_t* xid_mode_get_device_descriptor(void)
{
    return (const uint8_t*)&xbox_og_device_descriptor;
}

static const uint8_t* xid_mode_get_config_descriptor(void)
{
    return xbox_og_config_descriptor;
}

// ============================================================================
// MODE EXPORT
// ============================================================================

const usbd_mode_t xid_mode = {
    .name = "Xbox OG",
    .mode = USB_OUTPUT_MODE_XBOX_ORIGINAL,

    .get_device_descriptor = xid_mode_get_device_descriptor,
    .get_config_descriptor = xid_mode_get_config_descriptor,
    .get_report_descriptor = NULL,  // XID is not HID-based

    .init = xid_mode_init,
    .send_report = xid_mode_send_report,
    .is_ready = xid_mode_is_ready,

    // Feedback support
    .handle_output = NULL,  // Handled via tud_xid_get_rumble in task
    .get_rumble = xid_mode_get_rumble,
    .get_feedback = xid_mode_get_feedback,
    .get_report = NULL,

    .get_class_driver = xid_mode_get_class_driver,
    .task = xid_mode_task,
};
