// w5500.h — minimal WizNet W5500 driver for the GC↔ETH bridge use case.
//
// Just enough to: init MAC/IP, open a TCP server socket on port N, accept
// the first incoming connection, recv/send raw bytes, and report
// connection state. No multi-socket, no UDP, no lwIP — the chip does TCP
// in hardware so we can poll status registers directly.
//
// API intentionally mirrors the CH9120 driver in src/apps/gc2eth/ so
// app.c can swap transports with minimal churn.

#ifndef GC2ETH_FEATHER_W5500_H
#define GC2ETH_FEATHER_W5500_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "hardware/spi.h"

// Caller supplies the SPI peripheral + pins.
typedef struct {
    spi_inst_t* spi;
    uint        pin_sck;
    uint        pin_mosi;
    uint        pin_miso;
    uint        pin_cs;
    uint        pin_rst;        // optional, 0xFF if not wired
    uint32_t    spi_hz;         // e.g. 30_000_000 — W5500 max is 33.3 MHz
} w5500_pins_t;

// One-shot init. mac=6 bytes, ip/subnet/gateway = 4 bytes each.
// Resets the chip, configures network, leaves socket 0 closed.
bool w5500_init(const w5500_pins_t* pins,
                const uint8_t mac[6],
                const uint8_t ip[4],
                const uint8_t subnet[4],
                const uint8_t gateway[4]);

// Open socket 0 as a TCP server listening on `port`. After this the
// status will go LISTEN until a peer connects (then ESTABLISHED).
bool w5500_sock0_listen_tcp(uint16_t port);

// Open socket 0 as a TCP client and dial out to dest_ip:dest_port.
// Non-blocking — returns true once Sn_CR_CONNECT was issued; check
// w5500_sock0_connected() to know when it's ESTABLISHED.
// `local_port` is our source port (use 0 to let the chip pick).
bool w5500_sock0_connect_tcp(const uint8_t dest_ip[4], uint16_t dest_port,
                             uint16_t local_port);

// --- Socket 1: same API as socket 0 but on the second socket. We
// use it for Dolphin's clock-sync TCP stream (port 0xC10C); the data
// itself we discard (we have real hardware doing cycle-accurate joybus
// timing). Without an established clock-sync connection Dolphin
// short-circuits the GBA emulator partway through multiboot. ----------
bool   w5500_sock1_connect_tcp(const uint8_t dest_ip[4], uint16_t dest_port,
                               uint16_t local_port);
uint8_t w5500_sock1_status(void);
bool    w5500_sock1_connected(void);
size_t  w5500_sock1_recv(uint8_t* dst, size_t max);
void    w5500_sock1_close(void);

// Reads Sn_SR. Values you care about:
//   0x00 CLOSED        — slot free
//   0x14 LISTEN        — waiting for a peer
//   0x17 ESTABLISHED   — peer connected, can recv/send
//   0x1C CLOSE_WAIT    — peer disconnected; call close + listen to recycle
uint8_t w5500_sock0_status(void);

// Convenience: true if a peer is currently connected.
bool w5500_sock0_connected(void);

// Drain any bytes from the RX buffer into `dst`. Returns bytes copied (0
// if no data, max bytes requested). Non-blocking.
size_t w5500_sock0_recv(uint8_t* dst, size_t max);

// Write `len` bytes to TX buffer and issue SEND. Blocks until SEND is
// accepted by the chip (microseconds). Returns bytes sent.
size_t w5500_sock0_send(const uint8_t* src, size_t len);

// Close + re-listen. Call after CLOSE_WAIT to accept a new connection.
void w5500_sock0_reopen(uint16_t port);

// ----- Diagnostics --------------------------------------------------------
// These read live state straight from the chip via SPI, useful for
// verifying that SPI is wired/clocked correctly without inserting
// printfs in timing-sensitive code paths. All are cheap (a few register
// reads each).
typedef struct {
    uint8_t versionr;            // VERSIONR — should be 0x04 on real W5500
    uint8_t phycfgr;             // PHY config / link-up bit
    uint8_t sipr[4];             // configured IP
    uint8_t shar[6];             // configured MAC
    uint8_t sn_mr;               // socket-0 mode (should be 0x01 = TCP)
    uint16_t sn_port;            // socket-0 port
    uint8_t  sn_sr;              // socket-0 status
} w5500_diag_t;

void w5500_get_diag(w5500_diag_t* out);

#endif
