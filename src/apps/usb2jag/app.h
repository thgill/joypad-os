// app.h - USB2JAG App Manifest
// USB to Atari Jaguar HD15 adapter

#ifndef APP_USB2JAG_H
#define APP_USB2JAG_H

#define APP_NAME        "USB2JAG"
#define APP_VERSION     "1.0.0"
#define APP_DESCRIPTION "USB to Atari Jaguar HD15 adapter"
#define APP_AUTHOR      "RetroFrog"

// Input drivers
#define REQUIRE_USB_HOST        1
#define MAX_USB_DEVICES         4

// Output drivers
#define REQUIRE_NATIVE_JAGUAR_OUTPUT 1

// Services
#define REQUIRE_FLASH_SETTINGS      1
#define REQUIRE_PROFILE_SYSTEM      1
#define REQUIRE_PLAYER_MANAGEMENT   1

// Routing
#define ROUTING_MODE    ROUTING_MODE_SIMPLE
#define MERGE_MODE      MERGE_ALL
#define APP_MAX_ROUTES  1
#define TRANSFORM_FLAGS 0

// Player management
#define PLAYER_SLOT_MODE        PLAYER_SLOT_FIXED
#define MAX_PLAYER_SLOTS        1
#define AUTO_ASSIGN_ON_PRESS    1

// Hardware
#define BOARD               "rp2040zero"
#define CPU_OVERCLOCK_KHZ   0
#define UART_DEBUG          0

void app_init(void);
void app_task(void);

#endif // APP_USB2JAG_H
