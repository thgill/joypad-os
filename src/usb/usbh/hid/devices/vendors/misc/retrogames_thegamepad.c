// retrogames_thegamepad.c
// Driver for Retro Games Ltd "THEGamepad" (VID 0x1c59 PID 0x0026)
// As found in the A500 Mini, C64 Mini, etc.
//
// The generic HID parser fails on this device because:
// 1. The dpad is encoded as X/Y axes (no hat switch)
// 2. The descriptor has 3 duplicate Y axis usages causing yLoc to
//    point to byte 4 instead of byte 1
//
// Button mapping verified from raw HID captures:
//   A (red)    byte5 bit6 -> JP_BUTTON_B1
//   B (blue)   byte5 bit5 -> JP_BUTTON_B2
//   X (green)  byte5 bit7 -> JP_BUTTON_B3
//   Y (yellow) byte5 bit4 -> JP_BUTTON_B4
//   L shoulder byte6 bit0 -> JP_BUTTON_L1
//   R shoulder byte6 bit1 -> JP_BUTTON_R1
//   Menu       byte6 bit2 -> JP_BUTTON_S1
//   Home       byte6 bit3 -> JP_BUTTON_S2

#include "retrogames_thegamepad.h"
#include "core/buttons.h"
#include "core/router/router.h"
#include "core/input_event.h"
#include <string.h>

#define THEGAMEPAD_VID   0x1c59
#define THEGAMEPAD_PID   0x0026
#define THEGAMEPAD_DEADZONE 28

static bool is_retrogames_thegamepad(uint16_t vid, uint16_t pid) {
    return (vid == THEGAMEPAD_VID && pid == THEGAMEPAD_PID);
}

static bool diff_report(thegamepad_report_t const* r1, thegamepad_report_t const* r2) {
    return (r1->axis_x    != r2->axis_x    ||
            r1->axis_y    != r2->axis_y    ||
            r1->btn_a     != r2->btn_a     ||
            r1->btn_b     != r2->btn_b     ||
            r1->btn_x     != r2->btn_x     ||
            r1->btn_y     != r2->btn_y     ||
            r1->btn_l     != r2->btn_l     ||
            r1->btn_r     != r2->btn_r     ||
            r1->btn_menu  != r2->btn_menu  ||
            r1->btn_home  != r2->btn_home);
}

static void process_retrogames_thegamepad(uint8_t dev_addr, uint8_t instance,
                                          uint8_t const* report, uint16_t len) {
    static thegamepad_report_t prev_report[5][5] = {0};

    if (len < sizeof(thegamepad_report_t)) return;

    thegamepad_report_t r;
    memcpy(&r, report, sizeof(r));

    if (!diff_report(&prev_report[dev_addr-1][instance], &r)) return;
    prev_report[dev_addr-1][instance] = r;

    // Dpad from X/Y axes
    bool dpad_left  = r.axis_x < (128 - THEGAMEPAD_DEADZONE);
    bool dpad_right = r.axis_x > (128 + THEGAMEPAD_DEADZONE);
    bool dpad_up    = r.axis_y < (128 - THEGAMEPAD_DEADZONE);
    bool dpad_down  = r.axis_y > (128 + THEGAMEPAD_DEADZONE);

    uint32_t buttons =
        (dpad_up       ? JP_BUTTON_DU : 0) |
        (dpad_down     ? JP_BUTTON_DD : 0) |
        (dpad_left     ? JP_BUTTON_DL : 0) |
        (dpad_right    ? JP_BUTTON_DR : 0) |
        (r.btn_a       ? JP_BUTTON_B1 : 0) |  // A (red)    — primary fire
        (r.btn_b       ? JP_BUTTON_B2 : 0) |  // B (blue)
        (r.btn_x       ? JP_BUTTON_B3 : 0) |  // X (green)
        (r.btn_y       ? JP_BUTTON_B4 : 0) |  // Y (yellow)
        (r.btn_l       ? JP_BUTTON_L1 : 0) |  // L shoulder
        (r.btn_r       ? JP_BUTTON_R1 : 0) |  // R shoulder
        (r.btn_menu    ? JP_BUTTON_S1 : 0) |  // Menu = Select
        (r.btn_home    ? JP_BUTTON_S2 : 0);   // Home = Start

    input_event_t event = {
        .dev_addr     = dev_addr,
        .instance     = instance,
        .type         = INPUT_TYPE_GAMEPAD,
        .transport    = INPUT_TRANSPORT_USB,
        .layout       = LAYOUT_UNKNOWN,
        .buttons      = buttons,
        .button_count = 8,
        .analog       = {128, 128, 128, 128, 0, 0},
        .keys         = 0,
    };

    router_submit_input(&event);
}

DeviceInterface retrogames_thegamepad_interface = {
    .name             = "Retro Games THEGamepad",
    .is_device        = is_retrogames_thegamepad,
    .check_descriptor = NULL,
    .process          = process_retrogames_thegamepad,
    .task             = NULL,
    .init             = NULL,
    .unmount          = NULL,
};
