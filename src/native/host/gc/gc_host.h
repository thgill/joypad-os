// gc_host.h - Native GameCube Controller Host Driver
//
// Polls native GameCube controllers via the joybus-pio library and submits
// input events to the router.

#ifndef GC_HOST_H
#define GC_HOST_H

#include <stdint.h>
#include <stdbool.h>
#include "core/input_interface.h"

// ============================================================================
// CONFIGURATION
// ============================================================================

// Default GPIO pin for GameCube controller data line
#ifndef GC_PIN_DATA
#define GC_PIN_DATA  2   // Data I/O (directly to controller)
#endif

// Default polling rate (Hz) - GameCube console polls at ~125Hz
#ifndef GC_POLLING_RATE
#define GC_POLLING_RATE  125
#endif

// Maximum number of GameCube controllers (1 for now, future: adapter/multitap)
#define GC_MAX_PORTS 1

// ============================================================================
// PUBLIC API
// ============================================================================

// Initialize GC host driver with default pin
void gc_host_init(void);

// Initialize with custom pin configuration
void gc_host_init_pin(uint8_t data_pin);

// Poll GC controllers and submit events to router
// Call this regularly from main loop (typically from app's task function)
void gc_host_task(void);

// Check if GC controller is connected
bool gc_host_is_connected(void);

// Get detected device type for a port
// Returns: -1=none, 0x0009=controller, 0x2008=keyboard
int16_t gc_host_get_device_type(uint8_t port);

// Set rumble state for a port
void gc_host_set_rumble(uint8_t port, bool enabled);

// GC input interface (implements InputInterface pattern for app declaration)
extern const InputInterface gc_input_interface;

// ============================================================================
// GBA BRIDGE COORDINATION
// ============================================================================
// gba_bridge.c uses these to take exclusive ownership of the joybus port
// so its CDC-driven traffic doesn't race with gc_host's per-task autopoll.
//
// joybus_port_t is an anonymous struct typedef upstream (no tag name) so
// it can't be forward-declared portably — must include the header.
#include "joybus.h"

// Returns NULL if the port hasn't been initialized yet.
joybus_port_t* gc_host_get_gba_port(void);

// Acquire = pause autopoll. Returns false if no GBA is currently booted
// (so the caller knows the bridge can't take useful actions).
bool gc_host_gba_acquire_for_bridge(void);

// Release = resume autopoll for the next gc_host_task() call.
void gc_host_gba_release_from_bridge(void);

#endif // GC_HOST_H
