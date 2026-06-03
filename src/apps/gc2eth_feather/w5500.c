// w5500.c — see w5500.h for the API rationale.
//
// W5500 SPI access uses a 3-byte command header:
//   [addr-hi] [addr-lo] [control: BSB<<3 | R/W<<2 | OM]
// followed by data bytes. We use OM=0 (variable-length data mode).
//
// Block Select Bits (BSB) identify which register block the address is in:
//   0x00 = common registers
//   0x01 = socket 0 registers
//   0x02 = socket 0 TX buffer
//   0x03 = socket 0 RX buffer
//
// Reference: WizNet W5500 datasheet v1.0.2

#include "w5500.h"
#include "hardware/gpio.h"
#include "hardware/spi.h"
#include "pico/time.h"
#include <stdio.h>
#include <string.h>

// ============================================================================
// Register addresses
// ============================================================================
// Common
#define MR        0x0000  // Mode
#define GAR       0x0001  // Gateway IP (4 bytes)
#define SUBR      0x0005  // Subnet mask
#define SHAR      0x0009  // Source MAC (6 bytes)
#define SIPR      0x000F  // Source IP
#define INTLEVEL  0x0013
#define IR        0x0015
#define IMR       0x0016
#define SIR       0x0017
#define SIMR      0x0018
#define RTR       0x0019
#define RCR       0x001B
#define PHYCFGR   0x002E
#define VERSIONR  0x0039

// Socket 0 — same offsets in any socket's register block
#define Sn_MR      0x0000
#define Sn_CR      0x0001
#define Sn_IR      0x0002
#define Sn_SR      0x0003
#define Sn_PORT    0x0004
#define Sn_DHAR    0x0006   // dest MAC (auto-filled by ARP after CONNECT)
#define Sn_DIPR    0x000C   // dest IP (4 bytes)
#define Sn_DPORT   0x0010   // dest port (2 bytes)
#define Sn_RXBUF_SIZE 0x001E
#define Sn_TXBUF_SIZE 0x001F
#define Sn_TX_FSR  0x0020
#define Sn_TX_RD   0x0022
#define Sn_TX_WR   0x0024
#define Sn_RX_RSR  0x0026
#define Sn_RX_RD   0x0028
#define Sn_RX_WR   0x002A

// Modes
#define Sn_MR_TCP  0x01
// Bit 5 of Sn_MR in TCP mode = "No Delayed-ACK": chip acks every segment
// immediately instead of waiting up to 200ms. Halves single-round-trip
// latency for our small-message workload.
#define Sn_MR_TCP_NODELAY (Sn_MR_TCP | 0x20)

// Commands
#define Sn_CR_OPEN     0x01
#define Sn_CR_LISTEN   0x02
#define Sn_CR_CONNECT  0x04
#define Sn_CR_DISCON   0x08
#define Sn_CR_CLOSE    0x10
#define Sn_CR_SEND     0x20
#define Sn_CR_RECV     0x40

// Statuses
#define SOCK_CLOSED      0x00
#define SOCK_INIT        0x13
#define SOCK_LISTEN      0x14
#define SOCK_ESTABLISHED 0x17
#define SOCK_CLOSE_WAIT  0x1C

// Block Select values (pre-shifted into control byte position)
#define BSB_COMMON  (0x00 << 3)
#define BSB_S0_REG  (0x01 << 3)
#define BSB_S0_TX   (0x02 << 3)
#define BSB_S0_RX   (0x03 << 3)
// Socket 1 register blocks (used for Dolphin clock-sync connection).
#define BSB_S1_REG  (0x05 << 3)
#define BSB_S1_TX   (0x06 << 3)
#define BSB_S1_RX   (0x07 << 3)

#define RWB_WRITE 0x04
#define RWB_READ  0x00

// ============================================================================
// SPI plumbing
// ============================================================================
static const w5500_pins_t* g_pins = NULL;

static inline void cs_low(void)  { gpio_put(g_pins->pin_cs, 0); }
static inline void cs_high(void) { gpio_put(g_pins->pin_cs, 1); }

static void w5500_xfer(uint16_t addr, uint8_t bsb_rwb,
                       const uint8_t* tx, uint8_t* rx, size_t len)
{
    uint8_t hdr[3] = { (uint8_t)(addr >> 8), (uint8_t)(addr & 0xFF), bsb_rwb };
    cs_low();
    spi_write_blocking(g_pins->spi, hdr, 3);
    if (rx) {
        // Read: send 0x00 dummies, capture rx
        if (tx) spi_write_read_blocking(g_pins->spi, tx, rx, len);
        else    spi_read_blocking(g_pins->spi, 0x00, rx, len);
    } else {
        spi_write_blocking(g_pins->spi, tx, len);
    }
    cs_high();
}

static void w5500_write(uint16_t addr, uint8_t bsb, const uint8_t* src, size_t len) {
    w5500_xfer(addr, bsb | RWB_WRITE, src, NULL, len);
}
static void w5500_read(uint16_t addr, uint8_t bsb, uint8_t* dst, size_t len) {
    w5500_xfer(addr, bsb | RWB_READ, NULL, dst, len);
}

static void w5500_write_u8(uint16_t addr, uint8_t bsb, uint8_t v) {
    w5500_write(addr, bsb, &v, 1);
}
static uint8_t w5500_read_u8(uint16_t addr, uint8_t bsb) {
    uint8_t v; w5500_read(addr, bsb, &v, 1); return v;
}
static void w5500_write_u16(uint16_t addr, uint8_t bsb, uint16_t v) {
    uint8_t buf[2] = { (uint8_t)(v >> 8), (uint8_t)(v & 0xFF) };
    w5500_write(addr, bsb, buf, 2);
}
static uint16_t w5500_read_u16(uint16_t addr, uint8_t bsb) {
    uint8_t buf[2]; w5500_read(addr, bsb, buf, 2);
    return ((uint16_t)buf[0] << 8) | buf[1];
}

// Sn_CR is "issue command, hardware self-clears". Wait for it to clear so
// we don't pipeline two commands before the chip has consumed the first.
// BSB is the socket-N register block (e.g. BSB_S0_REG, BSB_S1_REG).
static void sock_cmd_n(uint8_t bsb, uint8_t cmd) {
    w5500_write_u8(Sn_CR, bsb, cmd);
    absolute_time_t deadline = make_timeout_time_ms(100);
    while (w5500_read_u8(Sn_CR, bsb) != 0) {
        if (time_reached(deadline)) break;
        tight_loop_contents();
    }
}
// Back-compat shim: existing socket-0 callsites unchanged.
static inline void sock_cmd(uint8_t cmd) { sock_cmd_n(BSB_S0_REG, cmd); }

// ============================================================================
// Public API
// ============================================================================

bool w5500_init(const w5500_pins_t* pins,
                const uint8_t mac[6], const uint8_t ip[4],
                const uint8_t subnet[4], const uint8_t gw[4])
{
    g_pins = pins;

    // SPI bring-up
    spi_init(pins->spi, pins->spi_hz);
    spi_set_format(pins->spi, 8, SPI_CPOL_0, SPI_CPHA_0, SPI_MSB_FIRST);
    gpio_set_function(pins->pin_sck,  GPIO_FUNC_SPI);
    gpio_set_function(pins->pin_mosi, GPIO_FUNC_SPI);
    gpio_set_function(pins->pin_miso, GPIO_FUNC_SPI);

    // CS as plain GPIO, idle high
    gpio_init(pins->pin_cs);
    gpio_put(pins->pin_cs, 1);
    gpio_set_dir(pins->pin_cs, GPIO_OUT);

    // RST: pulse low if wired
    if (pins->pin_rst != 0xFF) {
        gpio_init(pins->pin_rst);
        gpio_put(pins->pin_rst, 1);
        gpio_set_dir(pins->pin_rst, GPIO_OUT);
        gpio_put(pins->pin_rst, 0);
        sleep_us(500);
        gpio_put(pins->pin_rst, 1);
        sleep_ms(50);
    }

    // Software reset via MR.RST
    w5500_write_u8(MR, BSB_COMMON, 0x80);
    sleep_ms(10);

    // Sanity: VERSIONR should read 0x04 on W5500
    uint8_t ver = w5500_read_u8(VERSIONR, BSB_COMMON);
    printf("[w5500] chip version: 0x%02x (expect 0x04)\n", ver);
    if (ver != 0x04) {
        printf("[w5500] !! unexpected version — SPI wiring or chip not present\n");
        return false;
    }

    // Network config
    w5500_write(GAR,  BSB_COMMON, gw,     4);
    w5500_write(SUBR, BSB_COMMON, subnet, 4);
    w5500_write(SHAR, BSB_COMMON, mac,    6);
    w5500_write(SIPR, BSB_COMMON, ip,     4);

    printf("[w5500] ready: ip=%u.%u.%u.%u mac=%02x:%02x:%02x:%02x:%02x:%02x\n",
           ip[0], ip[1], ip[2], ip[3],
           mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    return true;
}

bool w5500_sock0_listen_tcp(uint16_t port)
{
    // Close anything that might be open
    sock_cmd(Sn_CR_CLOSE);

    // Configure socket 0: TCP mode, port, 16KB TX/RX buffers (default)
    w5500_write_u8(Sn_MR, BSB_S0_REG, Sn_MR_TCP_NODELAY);
    w5500_write_u16(Sn_PORT, BSB_S0_REG, port);

    sock_cmd(Sn_CR_OPEN);
    // Wait for SOCK_INIT
    absolute_time_t deadline = make_timeout_time_ms(100);
    while (w5500_read_u8(Sn_SR, BSB_S0_REG) != SOCK_INIT) {
        if (time_reached(deadline)) {
            printf("[w5500] sock0 OPEN timeout, status=0x%02x\n",
                   w5500_read_u8(Sn_SR, BSB_S0_REG));
            return false;
        }
        tight_loop_contents();
    }

    sock_cmd(Sn_CR_LISTEN);
    // Wait for SOCK_LISTEN
    deadline = make_timeout_time_ms(100);
    while (w5500_read_u8(Sn_SR, BSB_S0_REG) != SOCK_LISTEN) {
        if (time_reached(deadline)) {
            printf("[w5500] sock0 LISTEN timeout\n");
            return false;
        }
        tight_loop_contents();
    }

    printf("[w5500] sock0 listening on port %u\n", port);
    // Readback diagnostic: show what the chip actually accepted for its
    // network config (debugs CS-pin / SPI wiring issues — if these don't
    // match what we wrote, the writes aren't reaching the chip).
    uint8_t ip[4], mac[6];
    w5500_read(SIPR, BSB_COMMON, ip, 4);
    w5500_read(SHAR, BSB_COMMON, mac, 6);
    printf("[w5500]   readback: ip=%u.%u.%u.%u mac=%02x:%02x:%02x:%02x:%02x:%02x mode_reg=0x%02x port=%u\n",
           ip[0], ip[1], ip[2], ip[3],
           mac[0], mac[1], mac[2], mac[3], mac[4], mac[5],
           w5500_read_u8(Sn_MR, BSB_S0_REG),
           w5500_read_u16(Sn_PORT, BSB_S0_REG));
    return true;
}

uint8_t w5500_sock0_status(void) {
    return w5500_read_u8(Sn_SR, BSB_S0_REG);
}

bool w5500_sock0_connected(void) {
    return w5500_sock0_status() == SOCK_ESTABLISHED;
}

size_t w5500_sock0_recv(uint8_t* dst, size_t max)
{
    uint16_t avail = w5500_read_u16(Sn_RX_RSR, BSB_S0_REG);
    if (avail == 0) return 0;
    if (avail > max) avail = (uint16_t)max;

    // Read pointer wraps within the 16KB RX buffer; the chip handles the
    // wrap automatically when we use the buffer-block BSB. We just give it
    // the current read pointer as the address.
    uint16_t rd = w5500_read_u16(Sn_RX_RD, BSB_S0_REG);
    w5500_read(rd, BSB_S0_RX, dst, avail);

    // Advance read pointer + tell chip we consumed
    w5500_write_u16(Sn_RX_RD, BSB_S0_REG, rd + avail);
    sock_cmd(Sn_CR_RECV);
    return avail;
}

size_t w5500_sock0_send(const uint8_t* src, size_t len)
{
    // Wait for free space in TX buffer
    uint16_t free_sz;
    absolute_time_t deadline = make_timeout_time_ms(100);
    do {
        free_sz = w5500_read_u16(Sn_TX_FSR, BSB_S0_REG);
        if (free_sz >= len) break;
        if (time_reached(deadline)) {
            printf("[w5500] send: TX buffer full (free=%u, need=%u)\n",
                   free_sz, (unsigned)len);
            return 0;
        }
        tight_loop_contents();
    } while (1);

    uint16_t wr = w5500_read_u16(Sn_TX_WR, BSB_S0_REG);
    w5500_write(wr, BSB_S0_TX, src, len);
    w5500_write_u16(Sn_TX_WR, BSB_S0_REG, (uint16_t)(wr + len));
    sock_cmd(Sn_CR_SEND);
    return len;
}

void w5500_sock0_reopen(uint16_t port) {
    sock_cmd(Sn_CR_CLOSE);
    sleep_ms(2);
    w5500_sock0_listen_tcp(port);
}

bool w5500_sock0_connect_tcp(const uint8_t dest_ip[4], uint16_t dest_port,
                             uint16_t local_port)
{
    // Tear down any prior socket state
    sock_cmd(Sn_CR_CLOSE);

    w5500_write_u8(Sn_MR, BSB_S0_REG, Sn_MR_TCP_NODELAY);
    w5500_write_u16(Sn_PORT, BSB_S0_REG, local_port);

    sock_cmd(Sn_CR_OPEN);
    absolute_time_t deadline = make_timeout_time_ms(100);
    while (w5500_read_u8(Sn_SR, BSB_S0_REG) != SOCK_INIT) {
        if (time_reached(deadline)) {
            printf("[w5500] connect: OPEN timeout, sr=0x%02x\n",
                   w5500_read_u8(Sn_SR, BSB_S0_REG));
            return false;
        }
        tight_loop_contents();
    }

    // Set destination then issue CONNECT — the chip will ARP for the
    // dest MAC and complete the TCP three-way handshake on its own.
    w5500_write(Sn_DIPR, BSB_S0_REG, dest_ip, 4);
    w5500_write_u16(Sn_DPORT, BSB_S0_REG, dest_port);
    sock_cmd(Sn_CR_CONNECT);

    printf("[w5500] sock0 connecting to %u.%u.%u.%u:%u (src_port=%u)\n",
           dest_ip[0], dest_ip[1], dest_ip[2], dest_ip[3],
           dest_port, local_port);
    return true;
}

// ============================================================================
// Socket 1 — used for Dolphin's clock-sync TCP stream. Same protocol as
// socket 0; we just drain anything that arrives. The connection itself
// is what matters: Dolphin's GBA SI device won't progress past the
// initial multiboot handshake unless this second socket is open.
// ============================================================================
bool w5500_sock1_connect_tcp(const uint8_t dest_ip[4], uint16_t dest_port,
                             uint16_t local_port)
{
    sock_cmd_n(BSB_S1_REG, Sn_CR_CLOSE);

    w5500_write_u8(Sn_MR,   BSB_S1_REG, Sn_MR_TCP_NODELAY);
    w5500_write_u16(Sn_PORT, BSB_S1_REG, local_port);

    sock_cmd_n(BSB_S1_REG, Sn_CR_OPEN);
    absolute_time_t deadline = make_timeout_time_ms(100);
    while (w5500_read_u8(Sn_SR, BSB_S1_REG) != SOCK_INIT) {
        if (time_reached(deadline)) {
            printf("[w5500] sock1 OPEN timeout, sr=0x%02x\n",
                   w5500_read_u8(Sn_SR, BSB_S1_REG));
            return false;
        }
        tight_loop_contents();
    }

    w5500_write(Sn_DIPR, BSB_S1_REG, dest_ip, 4);
    w5500_write_u16(Sn_DPORT, BSB_S1_REG, dest_port);
    sock_cmd_n(BSB_S1_REG, Sn_CR_CONNECT);

    printf("[w5500] sock1 connecting to %u.%u.%u.%u:%u (src_port=%u)\n",
           dest_ip[0], dest_ip[1], dest_ip[2], dest_ip[3],
           dest_port, local_port);
    return true;
}

uint8_t w5500_sock1_status(void) {
    return w5500_read_u8(Sn_SR, BSB_S1_REG);
}

bool w5500_sock1_connected(void) {
    return w5500_sock1_status() == SOCK_ESTABLISHED;
}

size_t w5500_sock1_recv(uint8_t* dst, size_t max)
{
    uint16_t avail = w5500_read_u16(Sn_RX_RSR, BSB_S1_REG);
    if (avail == 0) return 0;
    if (avail > max) avail = (uint16_t)max;
    uint16_t rd = w5500_read_u16(Sn_RX_RD, BSB_S1_REG);
    w5500_read(rd, BSB_S1_RX, dst, avail);
    w5500_write_u16(Sn_RX_RD, BSB_S1_REG, rd + avail);
    sock_cmd_n(BSB_S1_REG, Sn_CR_RECV);
    return avail;
}

void w5500_sock1_close(void) {
    sock_cmd_n(BSB_S1_REG, Sn_CR_CLOSE);
}

void w5500_get_diag(w5500_diag_t* out) {
    if (!out) return;
    if (!g_pins) { memset(out, 0, sizeof(*out)); return; }
    out->versionr = w5500_read_u8(VERSIONR, BSB_COMMON);
    out->phycfgr  = w5500_read_u8(PHYCFGR,  BSB_COMMON);
    w5500_read(SIPR, BSB_COMMON, out->sipr, 4);
    w5500_read(SHAR, BSB_COMMON, out->shar, 6);
    out->sn_mr   = w5500_read_u8(Sn_MR,   BSB_S0_REG);
    out->sn_port = w5500_read_u16(Sn_PORT, BSB_S0_REG);
    out->sn_sr   = w5500_read_u8(Sn_SR,   BSB_S0_REG);
}
