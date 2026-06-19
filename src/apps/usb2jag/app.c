// app.c - USB2JAG App Entry Point
// USB to Atari Jaguar HD15 adapter
//
// Initializes the Jaguar output driver and USB host input.
// Single player, simple 1:1 routing.

#include "app.h"
#include "profiles.h"
#include "core/router/router.h"
#include "core/services/players/manager.h"
#include "core/services/profiles/profile.h"
#include "core/services/storage/flash.h"
#include "core/input_interface.h"
#include "core/output_interface.h"
#include "native/device/jaguar/jaguar_device.h"
#include "usb/usbh/usbh.h"
#include <stdio.h>

// ============================================================================
// APP PROFILE CONFIGURATION
// ============================================================================

static const profile_config_t app_profile_config = {
    .output_profiles = {
        [OUTPUT_TARGET_JAGUAR] = &jag_profile_set,
    },
    .shared_profiles = NULL,
};

// ============================================================================
// APP INPUT INTERFACES
// ============================================================================

static const InputInterface* input_interfaces[] = {
    &usbh_input_interface,
};

const InputInterface** app_get_input_interfaces(uint8_t* count)
{
    *count = sizeof(input_interfaces) / sizeof(input_interfaces[0]);
    return input_interfaces;
}

// ============================================================================
// APP OUTPUT INTERFACES
// ============================================================================

static const OutputInterface* output_interfaces[] = {
    &jaguar_output_interface,
};

const OutputInterface** app_get_output_interfaces(uint8_t* count)
{
    *count = sizeof(output_interfaces) / sizeof(output_interfaces[0]);
    return output_interfaces;
}

// ============================================================================
// APP INITIALIZATION
// ============================================================================

void app_init(void)
{
    printf("[app:usb2jag] Initializing usb2jag v%s\n", APP_VERSION);

    // Configure router: single player USB -> Jaguar
    router_config_t router_cfg = {
        .mode = ROUTING_MODE,
        .merge_mode = MERGE_MODE,
        .max_players_per_output = {
            [OUTPUT_TARGET_JAGUAR] = 1,
        },
        .merge_all_inputs = false,
        .transform_flags = TRANSFORM_FLAGS,
        .mouse_drain_rate = 0,
    };
    router_init(&router_cfg);

    // Add route: USB -> Jaguar
    router_add_route(INPUT_SOURCE_USB_HOST, OUTPUT_TARGET_JAGUAR, 0);

    // Configure player management
    player_config_t player_cfg = {
        .slot_mode            = PLAYER_SLOT_MODE,
        .max_slots            = MAX_PLAYER_SLOTS,
        .auto_assign_on_press = AUTO_ASSIGN_ON_PRESS,
    };
    players_init_with_config(&player_cfg);

    // Initialize profile system
    profile_init(&app_profile_config);

    printf("[app:usb2jag] Init complete\n");
    printf("[app:usb2jag]   Board:  %s\n", BOARD);
}

// ============================================================================
// APP TASK
// ============================================================================

void app_task(void)
{
    // No app-specific periodic work needed
}
