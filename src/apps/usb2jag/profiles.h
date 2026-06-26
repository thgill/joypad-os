// profiles.h - USB2JAG Button Mapping Profiles
//
// Jaguar button layout (top to bottom): A (primary), B, C
// Pro controller adds top row (top to bottom): X, Y, Z
//
// Profile 0: Standard     — pass-through, no remapping
// Profile 1: M30          — maps M30 buttons to Jaguar layout
// Profile 2: Dual Analog  — left stick = dpad, right stick = numpad 2/4/6/8

#pragma once

#include "core/services/profiles/profile.h"
#include "core/buttons.h"

// ============================================================================
// Jaguar numpad aliases using unused JP_BUTTON slots
// These map to specific Jaguar matrix positions in build_gamepad_rows
// ============================================================================

#define JAG_NUMPAD_2  JP_BUTTON_R4   // Row 2, J11 (down in gun direction)
#define JAG_NUMPAD_4  JP_BUTTON_R3   // Row 1, J10 (L shoulder / left gun)
#define JAG_NUMPAD_6  JP_BUTTON_L4   // Row 3, J10 (R shoulder / right gun)
#define JAG_NUMPAD_7  JP_BUTTON_L2   // Row 1, J9  (Jaguar Z / Pro top-left)
#define JAG_NUMPAD_8  JP_BUTTON_R2   // Row 2, J9  (Jaguar Y / up gun)
#define JAG_NUMPAD_9  JP_BUTTON_L3   // Row 3, J9  (Jaguar X / Pro top-right)

// ============================================================================
// Profile 0: Standard — pass-through
// ============================================================================

static const profile_t jag_profile_standard = {
    .name             = "Standard",
    .button_map       = NULL,
    .button_map_count = 0,
};

// ============================================================================
// Profile 1: M30
//
// M30 2.4G Genesis Mini USB button mapping:
//   A → JP_BUTTON_B1
//   B → JP_BUTTON_B2
//   C → ANALOG_R2   (trigger — converted in jaguar_device.c → JP_BUTTON_B2 pre-remap)
//   X → JP_BUTTON_B3
//   Y → JP_BUTTON_B4
//   Z → JP_BUTTON_R1
//   L → JP_BUTTON_L1
//   R → ANALOG_L2   (trigger — mapped to numpad 6)
//
// Jaguar target (BlueRetro convention: B2/Circle=FireA, B1/Cross=FireB, B3/Square=FireC):
//   M30 A (B1)   → Jaguar C  (Fire C = JP_BUTTON_B3)
//   M30 B (B2)   → Jaguar B  (Fire B = JP_BUTTON_B1)
//   M30 C (trig) → Jaguar A  (Fire A = JP_BUTTON_B2, injected directly, no remap needed)
//   M30 X (B3)   → Numpad 7  (Jaguar Z)
//   M30 Y (B4)   → Numpad 8  (Jaguar Y)
//   M30 Z (R1)   → Numpad 9  (Jaguar X)
//   M30 L (L1)   → Numpad 4  (L shoulder)
// ============================================================================

static const button_map_entry_t jag_m30_map[] = {
    { .input = JP_BUTTON_B1, .output = JP_BUTTON_B3 },  // M30 A → Jaguar C (Fire C)
    { .input = JP_BUTTON_B2, .output = JP_BUTTON_B1 },  // M30 B → Jaguar B (Fire B)
    { .input = JP_BUTTON_B3, .output = JAG_NUMPAD_7  }, // M30 X → Numpad 7 (Jaguar Z)
    { .input = JP_BUTTON_B4, .output = JAG_NUMPAD_8  }, // M30 Y → Numpad 8 (Jaguar Y)
    { .input = JP_BUTTON_R1, .output = JAG_NUMPAD_9  }, // M30 Z → Numpad 9 (Jaguar X)
    { .input = JP_BUTTON_L1, .output = JAG_NUMPAD_4  }, // M30 L → Numpad 4
};

static const profile_t jag_profile_m30 = {
    .name             = "M30",
    .button_map       = jag_m30_map,
    .button_map_count = sizeof(jag_m30_map) / sizeof(jag_m30_map[0]),
};

// ============================================================================
// Profile 2: Dual Analog
//
// For games like Total Carnage that use the numpad for independent gun direction.
// Left analog stick → dpad (handled by analog-to-dpad in jaguar_device.c).
// Right analog stick → numpad 2/4/6/8 (handled in jaguar_device.c when active).
// Face buttons same as Standard (no remapping needed).
// ============================================================================

static const profile_t jag_profile_dual_analog = {
    .name             = "Dual Analog",
    .button_map       = NULL,
    .button_map_count = 0,
};

// ============================================================================
// Profile index constants
// ============================================================================

#define JAG_PROFILE_STANDARD     0
#define JAG_PROFILE_M30          1
#define JAG_PROFILE_DUAL_ANALOG  2

// ============================================================================
// Profile array and set
// ============================================================================

static const profile_t jag_profiles[] = {
    jag_profile_standard,
    jag_profile_m30,
    jag_profile_dual_analog,
};

static const profile_set_t jag_profile_set = {
    .profiles      = jag_profiles,
    .profile_count = sizeof(jag_profiles) / sizeof(jag_profiles[0]),
    .default_index = JAG_PROFILE_STANDARD,
};
