// mode_marker.c — single point of definition for the host-patchable
// USB output mode marker. See mode_marker.h for the contract.

#include "mode_marker.h"

// `volatile const` so the compiler can't constant-fold the initial
// value into call sites. `used` keeps the linker from dropping it as
// dead data. The initial mode = 0xFF is the sentinel "firmware has
// not patched me"; splash code treats it as "neutral default" and
// shows the boot eyes.
__attribute__((used))
volatile const struct joypad_mode_marker g_joypad_mode_marker = {
    .magic = { JOYPAD_MODE_MAGIC0, JOYPAD_MODE_MAGIC1,
               JOYPAD_MODE_MAGIC2, JOYPAD_MODE_MAGIC3 },
    .mode  = JOYPAD_MODE_UNKNOWN,
    .reserved = { 0, 0, 0 },
};
