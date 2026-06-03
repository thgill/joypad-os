// joybus_bridge.h — generic raw-joybus passthrough over CDC.
//
// When START'd, the firmware yields the joybus port to a host-side
// daemon: gc_host stops its autopoll (gba_input_read) AND its autoboot
// (gba_mb_upload of the embedded payload) while the bridge owns it.
// The daemon then drives every joybus exchange explicitly via
// JOYBUS.XFER — primitive enough to support GBA multiboot (Kawasedo
// cipher in JS), Dolphin live link traffic, and eventually GameCube
// controller polling all from the host side.
//
// Why on the host instead of in firmware: the GBA's BIOS state machine
// + Kawasedo handshake have a bunch of timing-sensitive edge cases
// (post-RESET PSF0 settle, etc.) that are much easier to iterate on in
// JS than to keep reflashing firmware to fix.

#ifndef JOYBUS_BRIDGE_H
#define JOYBUS_BRIDGE_H

#include <stdint.h>
#include <stdbool.h>

typedef enum {
    JOYBUS_BRIDGE_IDLE = 0,
    JOYBUS_BRIDGE_ACTIVE,
} joybus_bridge_state_t;

void joybus_bridge_init(void);
joybus_bridge_state_t joybus_bridge_get_state(void);

// CDC command entry points. Return true on success.
bool joybus_bridge_start(void);
void joybus_bridge_stop(void);

// Primitive xfer: send tx_len bytes, then read up to rx_max bytes
// (with the given microsecond timeout). Returns bytes received on
// success (0 if rx_max==0), or negative on error:
//   -1 = bridge not active
//   -2 = port not initialized
//   -3 = receive timeout
int joybus_bridge_xfer(const uint8_t* tx, uint16_t tx_len,
                       uint8_t* rx, uint16_t rx_max,
                       uint32_t timeout_us);

// ============================================================================
// GBA Multiboot ROM staging
// ============================================================================
// The pure-JS-driven multiboot (one JOYBUS.XFER/BATCH per WRITE) doesn't
// keep up with the BIOS's inter-WRITE timing tolerance — the ~2 ms CDC
// roundtrip between batches stalls the upload long enough that the BIOS
// silently rejects at CRC check time. So for multiboot specifically we
// stage the whole ROM into firmware first, then run gba_mb_upload here
// in a tight loop with its native sleep_us(70) per WRITE.
//
// Other joybus traffic (Dolphin live link, controller polling) keeps
// using the JOYBUS.XFER primitive — it doesn't have multiboot's
// timing constraint.

void joybus_bridge_mb_reset(void);                          // clear buffer
bool joybus_bridge_mb_append(const uint8_t* data, uint32_t len);   // accumulate
void joybus_bridge_mb_trim(uint32_t bytes);                 // drop last N bytes
uint32_t joybus_bridge_mb_size(void);                       // bytes buffered
int  joybus_bridge_mb_upload(int channel);                  // run gba_mb_upload, returns gba_mb_result_t

#endif // JOYBUS_BRIDGE_H
