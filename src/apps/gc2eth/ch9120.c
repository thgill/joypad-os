// ch9120.c — see ch9120.h. Single UART (UART1) at runtime-switched
// baud (9600 in config mode, 921600 in data mode).
//
// Wire protocol from CH9120 datasheet (V1.1):
//   Each command frame is "0x57 0xAB <cmd> [params...]"
//   Chip acks each command with single byte 0xAA (or 0xA5 for the
//   serial-negotiation entry handshake we don't use).
//   All multi-byte values are little-endian.

#include "ch9120.h"
#include "app.h"
#include "pico/stdlib.h"
#include "hardware/uart.h"
#include "hardware/gpio.h"
#include <stdio.h>

// We use UART1. UART0 is reserved for stdio (GP0/GP1).
#define CH9120_UART          uart1
#define CH9120_CONFIG_BAUD   9600
#define CH9120_ACK           0xAA

// Cached data-mode baud so read_ip can switch back to it correctly
// regardless of what was passed to ch9120_init.
static uint32_t s_data_baud = 921600;
#define CH9120_CMD_HDR_0     0x57
#define CH9120_CMD_HDR_1     0xAB
// Commands (subset we actually use)
#define CH9120_CMD_SAVE      0x0D    // save params to EEPROM
#define CH9120_CMD_EXECUTE   0x0E    // apply running config + reset chip
#define CH9120_CMD_EXIT_CFG  0x5E    // exit serial-port config mode
#define CH9120_CMD_MODE      0x10    // 00=TCP server, 01=TCP client, 02=UDP server, 03=UDP client
#define CH9120_CMD_IP        0x11    // chip IP (4 bytes)
#define CH9120_CMD_MASK      0x12    // subnet mask (4 bytes)
#define CH9120_CMD_GATEWAY   0x13    // gateway (4 bytes)
#define CH9120_CMD_LOCAL_PORT 0x14   // 2 bytes LE
#define CH9120_CMD_DEST_IP   0x15    // 4 bytes (client mode)
#define CH9120_CMD_DEST_PORT 0x16    // 2 bytes LE (client mode)
#define CH9120_CMD_BAUD      0x21    // 4 bytes LE
#define CH9120_CMD_UART_FMT  0x22    // 3 bytes: stop, parity, databits
#define CH9120_CMD_PKT_TIMEOUT 0x23  // 4 bytes LE — UART-side idle timeout in 5ms units
#define CH9120_CMD_DISCONNECT 0x24   // 1 byte — disconnect TCP on serial inactivity
#define CH9120_CMD_PKT_LEN   0x25    // 4 bytes LE — UART RX packet length before forwarding to TCP
#define CH9120_CMD_CLEAR_ON_CONNECT 0x26  // 1 byte: 01=clear UART buf on TCP connect, 00=don't
#define CH9120_CMD_DHCP      0x33    // 1 byte: 01=on, 00=off

static bool s_uart_initialized = false;

static void uart_set_baud(uint baud)
{
    if (!s_uart_initialized) {
        // First call: full init. Subsequent calls just change baud.
        // Pull-up on RX BEFORE enabling UART function — fixes
        // pico-sdk issue #1144 phantom-null-byte startup interrupt
        // and prevents bytes from being lost when the RX line is
        // briefly floating during UART peripheral wakeup.
        gpio_init(CH9120_UART_RX_PIN);
        gpio_pull_up(CH9120_UART_RX_PIN);
        uart_init(CH9120_UART, baud);
        gpio_set_function(CH9120_UART_TX_PIN, GPIO_FUNC_UART);
        gpio_set_function(CH9120_UART_RX_PIN, GPIO_FUNC_UART);
        uart_set_format(CH9120_UART, 8, 1, UART_PARITY_NONE);
        uart_set_fifo_enabled(CH9120_UART, true);
        s_uart_initialized = true;
    } else {
        // Baud-only change. Matches Waveshare's demo: uart_set_baudrate
        // without a full uart_init reset, which preserves FIFO state
        // and avoids putting the UART in a transient bad state.
        uart_set_baudrate(CH9120_UART, baud);
    }
}

// Drive a GPIO output to `level` WITHOUT a transient glitch through
// the opposite level. Standard pattern (gpio_init → gpio_set_dir(OUT)
// → gpio_put(level)) briefly drives LOW between dir-set and put,
// because the output register defaults to 0. On the CH9120's CFG0
// pin that glitch puts the chip into config mode and breaks the
// TCP→UART forwarding direction permanently for that session.
static void gpio_drive_glitchless(uint pin, bool level)
{
    gpio_init(pin);
    gpio_put(pin, level);            // set output register FIRST
    gpio_set_dir(pin, GPIO_OUT);     // then enable as output
}

static void chip_reset(void)
{
    // Active-low reset, hold ~10 ms. Initial transition is HIGH (no
    // reset) — then we explicitly pulse low.
    gpio_drive_glitchless(CH9120_RSTI_PIN, 1);
    sleep_ms(1);
    gpio_put(CH9120_RSTI_PIN, 0);
    sleep_ms(10);
    gpio_put(CH9120_RSTI_PIN, 1);
    sleep_ms(150);  // chip boot/firmware-init time
}

static void cfg0_set(bool config_mode)
{
    // First call must use glitchless drive; subsequent calls just
    // toggle the output level (no direction change).
    static bool s_inited = false;
    if (!s_inited) {
        gpio_drive_glitchless(CH9120_CFG0_PIN, config_mode ? 0 : 1);
        s_inited = true;
    } else {
        gpio_put(CH9120_CFG0_PIN, config_mode ? 0 : 1);
    }
}

static void tcpcs_init(void)
{
    gpio_init(CH9120_TCPCS_PIN);
    gpio_set_dir(CH9120_TCPCS_PIN, GPIO_IN);
    // TCPCS is ACTIVE LOW: low = TCP peer connected, high = idle.
    // Pull-up keeps the line high (= no peer) if the CH9120 is
    // briefly tri-stated during reset/config transitions.
    gpio_pull_up(CH9120_TCPCS_PIN);
}

// Wait up to 200 ms for a single ACK byte.
static bool wait_ack(void)
{
    absolute_time_t deadline = make_timeout_time_ms(200);
    while (!time_reached(deadline)) {
        if (uart_is_readable(CH9120_UART)) {
            uint8_t b = uart_getc(CH9120_UART);
            if (b == CH9120_ACK) return true;
            printf("[ch9120] unexpected ack byte: 0x%02x\n", b);
            return false;
        }
        tight_loop_contents();
    }
    printf("[ch9120] ack timeout\n");
    return false;
}

static bool send_cmd(uint8_t cmd, const uint8_t* params, int param_len)
{
    // Match Waveshare's official RP2040-ETH demo's CH9120_TX_*_bytes
    // pattern exactly: 10ms before, write bytes byte-by-byte via
    // uart_putc, 10ms after. No drain — leftover 0xAA acks get
    // discarded by the final drain at the end of init.
    sleep_ms(10);
    uart_putc(CH9120_UART, CH9120_CMD_HDR_0);
    uart_putc(CH9120_UART, CH9120_CMD_HDR_1);
    uart_putc(CH9120_UART, cmd);
    for (int i = 0; i < param_len; i++) {
        uart_putc(CH9120_UART, params[i]);
    }
    sleep_ms(10);
    // The demo also adds 100ms post-call (in CH9120_SetXxx wrappers).
    sleep_ms(100);
    return true;
}

bool ch9120_init(ch9120_mode_t mode,
                 uint16_t local_port,
                 const uint8_t dest_ip[4], uint16_t dest_port,
                 uint32_t baud_rate, bool dhcp,
                 const uint8_t static_ip[4],
                 const uint8_t static_mask[4],
                 const uint8_t static_gw[4])
{
    const char* mode_name = (mode == CH9120_MODE_TCP_SERVER) ? "TCP_SERVER" :
                            (mode == CH9120_MODE_TCP_CLIENT) ? "TCP_CLIENT" :
                            (mode == CH9120_MODE_UDP_SERVER) ? "UDP_SERVER" : "UDP_CLIENT";
    printf("[ch9120] init: mode=%s local_port=%u baud=%lu\n",
           mode_name, local_port, (unsigned long)baud_rate);
    if (mode == CH9120_MODE_TCP_CLIENT || mode == CH9120_MODE_UDP_CLIENT) {
        printf("[ch9120]   dest=%u.%u.%u.%u:%u\n",
               dest_ip[0], dest_ip[1], dest_ip[2], dest_ip[3], dest_port);
    }
    if (dhcp) {
        printf("[ch9120]   addressing=DHCP\n");
    } else {
        printf("[ch9120]   addressing=STATIC ip=%u.%u.%u.%u "
               "mask=%u.%u.%u.%u gw=%u.%u.%u.%u\n",
               static_ip[0], static_ip[1], static_ip[2], static_ip[3],
               static_mask[0], static_mask[1], static_mask[2], static_mask[3],
               static_gw[0], static_gw[1], static_gw[2], static_gw[3]);
    }

    // Mirror Waveshare's CH9120_init order EXACTLY:
    // 1) uart_init at 9600 + UART GPIO functions
    // 2) gpio_init CFG/RES + set_dir OUT
    // 3) gpio_put CFG=0, RES=1
    // 4) 100ms settle
    uart_set_baud(CH9120_CONFIG_BAUD);   // first call → uart_init + GPIO_FUNC_UART
    gpio_drive_glitchless(CH9120_RSTI_PIN, 1);   // RES high (no reset), glitchless
    cfg0_set(true);                              // CFG low (config mode), glitchless
    tcpcs_init();                                // GP17 read-only
    sleep_ms(100);                               // matches demo's pre-config settle

    // Drain any stale RX
    while (uart_is_readable(CH9120_UART)) (void)uart_getc(CH9120_UART);

    // Work mode
    uint8_t mode_byte = (uint8_t)mode;
    if (!send_cmd(CH9120_CMD_MODE, &mode_byte, 1)) return false;

    // IP, mask, gateway — always sent (matches the demo). The
    // demo never touches DHCP; default is whatever the chip's
    // EEPROM was last configured with.
    (void)dhcp;
    if (!send_cmd(CH9120_CMD_IP,      static_ip,   4)) return false;
    if (!send_cmd(CH9120_CMD_MASK,    static_mask, 4)) return false;
    if (!send_cmd(CH9120_CMD_GATEWAY, static_gw,   4)) return false;

    // Target IP (demo always sets this even in server mode).
    if (!send_cmd(CH9120_CMD_DEST_IP, dest_ip, 4)) return false;

    // Local port
    uint8_t port_le[2] = { (uint8_t)(local_port & 0xFF),
                           (uint8_t)((local_port >> 8) & 0xFF) };
    if (!send_cmd(CH9120_CMD_LOCAL_PORT, port_le, 2)) return false;

    // Target port
    uint8_t dport_le[2] = { (uint8_t)(dest_port & 0xFF),
                            (uint8_t)((dest_port >> 8) & 0xFF) };
    if (!send_cmd(CH9120_CMD_DEST_PORT, dport_le, 2)) return false;

    // Baud rate (LE)
    uint8_t baud_le[4] = { (uint8_t)(baud_rate & 0xFF),
                           (uint8_t)((baud_rate >> 8) & 0xFF),
                           (uint8_t)((baud_rate >> 16) & 0xFF),
                           (uint8_t)((baud_rate >> 24) & 0xFF) };
    if (!send_cmd(CH9120_CMD_BAUD, baud_le, 4)) return false;

    // Save + apply config via soft-reset. Same byte sequence as
    // Waveshare's CH9120_Eed: 0x0D save, 0x0E execute, 0x5E exit.
    if (!send_cmd(CH9120_CMD_SAVE,     NULL, 0)) return false;
    sleep_ms(100);
    if (!send_cmd(CH9120_CMD_EXECUTE,  NULL, 0)) return false;
    sleep_ms(100);
    if (!send_cmd(CH9120_CMD_EXIT_CFG, NULL, 0)) return false;
    sleep_ms(500);

    // Drive CFG high → chip transitions to data mode with new config.
    s_data_baud = baud_rate;
    cfg0_set(false);
    uart_set_baud(baud_rate);

    // Drain any garbage that landed during the baud-switch transition.
    while (uart_is_readable(CH9120_UART)) (void)uart_getc(CH9120_UART);

    printf("[ch9120] ready (data mode @ %lu baud)\n", (unsigned long)baud_rate);
    return true;
}

// Read chip IP via cmd 0x61. Returns 4 raw bytes (network order, but
// the chip already stores them as A.B.C.D so out[0]=A, out[1]=B, etc.).
static bool read_ip_in_config_mode(uint8_t out[4])
{
    uint8_t hdr[3] = { CH9120_CMD_HDR_0, CH9120_CMD_HDR_1, 0x61 };
    uart_write_blocking(CH9120_UART, hdr, sizeof(hdr));
    uart_tx_wait_blocking(CH9120_UART);

    int got = 0;
    absolute_time_t deadline = make_timeout_time_ms(200);
    while (got < 4 && !time_reached(deadline)) {
        if (uart_is_readable(CH9120_UART)) {
            out[got++] = uart_getc(CH9120_UART);
        }
    }
    return got == 4;
}

bool ch9120_read_ip(uint8_t out[4])
{
    // Re-enter config mode to query — drop CFG0, switch UART baud,
    // read, then release CFG0 back to data mode. CRITICALLY: do NOT
    // send 0x0E (execute + reset) on the way out — that would reset
    // the chip and abort any in-flight DHCP / TCP. Toggling CFG0 by
    // itself just suspends the data-mode parser; the network stack
    // and DHCP state machine keep running.
    cfg0_set(true);
    sleep_ms(20);
    uart_set_baud(CH9120_CONFIG_BAUD);
    while (uart_is_readable(CH9120_UART)) (void)uart_getc(CH9120_UART);

    bool ok = read_ip_in_config_mode(out);

    cfg0_set(false);
    sleep_ms(20);
    uart_set_baud(s_data_baud);
    while (uart_is_readable(CH9120_UART)) (void)uart_getc(CH9120_UART);
    return ok;
}

bool ch9120_is_connected(void)
{
    // TCPCS is active-LOW. Low = TCP peer connected.
    return gpio_get(CH9120_TCPCS_PIN) == 0;
}

int ch9120_recv_byte_timeout(uint32_t timeout_us)
{
    // BUGFIX: always do at least one uart_is_readable check before
    // bailing on timeout. make_timeout_time_us(0) returns a deadline
    // that's already past, so the original `while (!time_reached)`
    // loop never entered with timeout_us=0 — returning -1 without
    // ever polling UART. Hours of debugging time. Bug found 2026-05-11.
    if (uart_is_readable(CH9120_UART)) {
        return uart_getc(CH9120_UART);
    }
    if (timeout_us == 0) return -1;
    absolute_time_t deadline = make_timeout_time_us(timeout_us);
    while (!time_reached(deadline)) {
        if (uart_is_readable(CH9120_UART)) {
            return uart_getc(CH9120_UART);
        }
        tight_loop_contents();
    }
    return -1;
}

int ch9120_recv_bytes(uint8_t* dst, int max, uint32_t per_byte_timeout_us)
{
    int got = 0;
    while (got < max) {
        int b = ch9120_recv_byte_timeout(per_byte_timeout_us);
        if (b < 0) break;
        dst[got++] = (uint8_t)b;
    }
    return got;
}

void ch9120_send_byte(uint8_t b)
{
    uart_putc_raw(CH9120_UART, (char)b);
}

void ch9120_send_bytes(const uint8_t* src, int n)
{
    uart_write_blocking(CH9120_UART, src, (size_t)n);
}
