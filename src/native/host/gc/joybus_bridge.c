// joybus_bridge.c — see joybus_bridge.h for API + design pointer.
//
// Intentionally tiny. The whole protocol layer (cipher, multiboot
// state machine, retries, timing tweaks) lives on the daemon side now;
// this just owns/yields the bus and forwards bytes.

#include "joybus_bridge.h"
#include "gba_multiboot.h"
#include "gc_host.h"
#include "joybus.h"
#include <stdio.h>
#include <string.h>

static joybus_bridge_state_t s_state = JOYBUS_BRIDGE_IDLE;

void joybus_bridge_init(void)
{
    s_state = JOYBUS_BRIDGE_IDLE;
}

joybus_bridge_state_t joybus_bridge_get_state(void)
{
    return s_state;
}

bool joybus_bridge_start(void)
{
    if (s_state == JOYBUS_BRIDGE_ACTIVE) return true;
    if (!gc_host_gba_acquire_for_bridge()) return false;
    s_state = JOYBUS_BRIDGE_ACTIVE;
    printf("[joybus_bridge] active — gc_host autopoll/autoboot paused\n");
    return true;
}

void joybus_bridge_stop(void)
{
    if (s_state == JOYBUS_BRIDGE_IDLE) return;
    s_state = JOYBUS_BRIDGE_IDLE;
    gc_host_gba_release_from_bridge();
    printf("[joybus_bridge] released — gc_host autopoll/autoboot resumed\n");
}

int joybus_bridge_xfer(const uint8_t* tx, uint16_t tx_len,
                       uint8_t* rx, uint16_t rx_max,
                       uint32_t timeout_us)
{
    if (s_state != JOYBUS_BRIDGE_ACTIVE) return -1;
    joybus_port_t* port = gc_host_get_gba_port();
    if (!port) return -2;
    if (tx_len > 0) {
        joybus_send_bytes(port, (uint8_t*)tx, tx_len);
    }
    if (rx_max == 0) return 0;
    uint32_t got = joybus_receive_bytes(port, rx, rx_max, timeout_us, true);
    if (got == 0) return -3;
    return (int)got;
}

// ============================================================================
// GBA Multiboot ROM staging — buffer in firmware, then run gba_mb_upload
// natively to keep the BIOS-side handshake timing tight.
// ============================================================================

#define MB_BUF_MAX (64 * 1024)
static uint8_t  s_mb_buf[MB_BUF_MAX];
static uint32_t s_mb_len = 0;

void joybus_bridge_mb_reset(void)
{
    s_mb_len = 0;
}

bool joybus_bridge_mb_append(const uint8_t* data, uint32_t len)
{
    if (s_mb_len + len > MB_BUF_MAX) return false;
    memcpy(&s_mb_buf[s_mb_len], data, len);
    s_mb_len += len;
    return true;
}

void joybus_bridge_mb_trim(uint32_t bytes)
{
    if (bytes >= s_mb_len) s_mb_len = 0;
    else s_mb_len -= bytes;
}

uint8_t* joybus_bridge_mb_buffer(void)
{
    return s_mb_buf;
}

uint32_t joybus_bridge_mb_size(void)
{
    return s_mb_len;
}

int joybus_bridge_mb_upload(int channel)
{
    if (s_state != JOYBUS_BRIDGE_ACTIVE) return -100;
    if (s_mb_len < 0x200) return -101;
    joybus_port_t* port = gc_host_get_gba_port();
    if (!port) return -102;
    return (int)gba_mb_upload(port, s_mb_buf, s_mb_len,
                              /*palette*/3, /*speed*/0, channel);
}
