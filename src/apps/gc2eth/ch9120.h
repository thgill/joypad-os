// ch9120.h — driver for the CH9120 UART-to-Ethernet bridge chip
// (Waveshare RP2040-ETH variant). See .dev/docs/gc2eth-ch9120-bridge.md.
//
// Lifecycle:
//   ch9120_init() — pulse reset, enter config mode (CFG0 low @ 9600),
//   send config commands, exit to data mode (CFG0 high @ 921600).
//   After this, ch9120_recv_byte() / ch9120_send_byte() bridge to TCP.
//   ch9120_is_connected() tells you if a TCP peer is currently on.

#ifndef CH9120_H
#define CH9120_H

#include <stdint.h>
#include <stdbool.h>

// CH9120 work modes. Match the chip's command 0x10 parameter values.
typedef enum {
    CH9120_MODE_TCP_SERVER = 0,
    CH9120_MODE_TCP_CLIENT = 1,
    CH9120_MODE_UDP_SERVER = 2,
    CH9120_MODE_UDP_CLIENT = 3,
} ch9120_mode_t;

// Initialize CH9120.
//   mode:        TCP server / client (UDP variants compile but untested)
//   local_port:  port the chip binds locally (server mode) or sources from
//   dest_ip/_port: only used in client modes — Dolphin's host & port
//   baud_rate:   UART baud for the data channel (we use 921600 max)
//   dhcp=false:  use static_ip/mask/gw; DHCP overrides them when true
// Each address is a 4-byte array, e.g. {192,168,1,250} for 192.168.1.250.
bool ch9120_init(ch9120_mode_t mode,
                 uint16_t local_port,
                 const uint8_t dest_ip[4], uint16_t dest_port,
                 uint32_t baud_rate, bool dhcp,
                 const uint8_t static_ip[4],
                 const uint8_t static_mask[4],
                 const uint8_t static_gw[4]);

// Drop back into config mode briefly to read the chip's assigned
// IPv4. Useful for printing "we are reachable at X.X.X.X" after DHCP.
// Pass any 4-byte buffer; returns true on success.
// Side effect: chip is reset/re-execed, so call only when no TCP
// peer is connected (or accept that they'll be dropped).
bool ch9120_read_ip(uint8_t out[4]);

// True if a remote peer (Dolphin) currently has a TCP connection open.
// Reads the TCPCS pin from CH9120 directly — no UART traffic involved.
bool ch9120_is_connected(void);

// Returns the byte read, or -1 on timeout.
int ch9120_recv_byte_timeout(uint32_t timeout_us);

// Read up to `max` bytes; returns count actually read.
int ch9120_recv_bytes(uint8_t* dst, int max, uint32_t per_byte_timeout_us);

void ch9120_send_byte(uint8_t b);
void ch9120_send_bytes(const uint8_t* src, int n);

#endif // CH9120_H
