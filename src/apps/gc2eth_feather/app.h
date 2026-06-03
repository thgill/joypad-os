// app.h — gc2eth_feather pin map.
//
// Hardware: Adafruit Feather RP2040 USB Host + Adafruit FeatherWing
// Ethernet (W5500) connected via the standard FeatherWing pinout.

#ifndef GC2ETH_FEATHER_APP_H
#define GC2ETH_FEATHER_APP_H

#include <stdint.h>
#include <stdbool.h>
#include "app_config.h"

// Joybus data line to the GBA. Silkscreen "D4" on the Adafruit
// Feather RP2040 USB Host = GP4 (Adafruit's silkscreen convention is
// Dn = GPn for the digital pins on RP2040 boards). Free of SPI / I2C /
// USB-host (GP16/17/18) conflicts. Wire a 470Ω–1kΩ series resistor
// between this pin and the GBA JOY-Bus data line (middle pin on the
// GC↔GBA cable's GC end), and share GND.
#ifndef GC_DATA_PIN
#define GC_DATA_PIN              4
#endif

// W5500 SPI bus (board default SPI1) + chip-select. Silicognition's
// PoE-FeatherWing routes W5500's CS to Feather IO10 (= GP10 on this
// Feather) through the default SJCS solder jumper. See:
// https://silicognition.com/Products/poe-featherwing/
#define W5500_SPI                spi1
#define W5500_PIN_SCK            14
#define W5500_PIN_MOSI           15
#define W5500_PIN_MISO           8
#define W5500_PIN_CS             10
#define W5500_PIN_RST            0xFF   // RST not broken out to a dedicated Feather pin
#define W5500_SPI_HZ             20000000  // 20 MHz — 33 MHz hurt jitter on this PCB

// Static network config. Tied to a specific en10 subnet — keep en10
// manually configured to 192.168.1.159/24 in macOS Network settings
// so DHCP failures (or exo's mesh-discovery races) can't yank it
// to link-local while we're testing.
#define W5500_LOCAL_IP_A         192
#define W5500_LOCAL_IP_B         168
#define W5500_LOCAL_IP_C         1
#define W5500_LOCAL_IP_D         250
#define W5500_GATEWAY_A          192
#define W5500_GATEWAY_B          168
#define W5500_GATEWAY_C          1
#define W5500_GATEWAY_D          1
#define W5500_SUBNET             255, 255, 255, 0
#define W5500_LISTEN_PORT        54970   // 0xD6BA = "Dolphin GBA"

// TCP client mode: who we dial out to. Matches the CH9120 working setup
// where the bridge is the TCP CLIENT and Dolphin / fake-Dolphin is the
// server listening on port 54970. Set this to the IP of the host
// running Dolphin (or eth-multiboot.js).
#define W5500_DEST_IP_A          192
#define W5500_DEST_IP_B          168
#define W5500_DEST_IP_C          1
#define W5500_DEST_IP_D          159
#define W5500_DEST_PORT          54970

// Dolphin also exposes a clock-sync TCP listener at 0xC10C (49420).
// We open a second socket to it but discard the bytes — the connection
// itself is what unblocks Dolphin's GBA SI device, not the data on it.
#define W5500_CLOCK_PORT         49420
#define W5500_CLOCK_LOCAL_PORT   49420

// Diag struct exposed via CDC (FRAMES?) for sanity checking from the
// host. Mirrors gc2eth's diag.
typedef struct {
    uint32_t frames_seen;
    uint8_t  last_cmd;
    int      last_n;
    uint8_t  last_rx[5];
} gc2eth_diag_t;

void gc2eth_get_diag(gc2eth_diag_t* out);

// ----- Trace ring for intercept-replay profiling --------------------------
// Captures (cmd, n_returned, rx[5], delta_us_since_prev) per joybus
// exchange. Goal: figure out which commands are worth caching locally
// in the bridge (so we don't have to round-trip every poll).
//
// Workflow:
//   TRACE.START          → arm capture (also clears the ring)
//   <run game ~30 sec>
//   TRACE.STOP           → freeze the ring
//   TRACE.DUMP           → spit ASCII rows over CDC for offline analysis
//
// Capture is gated by gc2eth_trace_armed() so the hot-path overhead is
// near-zero when not tracing.
typedef struct {
    uint32_t delta_us;     // since previous record (0 for first)
    uint8_t  cmd;
    int8_t   n;            // bytes the GBA actually returned (or <0 = timeout)
    uint8_t  rx[5];
} gc2eth_trace_entry_t;

// Phase 2: STATUS-cache tunable. N=0 disables (every STATUS goes to
// joybus). N>0 means serve up to N consecutive STATUS polls from a
// local cache before forcing a refresh. Higher N = more speedup but
// up to N polls of latency before Dolphin sees a RECV-bit change.
void     gc2eth_status_cache_set_n(uint16_t n);
uint16_t gc2eth_status_cache_get_n(void);
uint32_t gc2eth_status_cache_hits(void);
uint32_t gc2eth_status_cache_miss(void);

// Phase 2.5: speculative STATUS pre-send. depth=0 disables; higher
// values push more 3-byte STATUS replies into Dolphin's TCP recv
// buffer ahead of demand so future STATUS requests complete with 0
// RTT on Dolphin's side. Risky if Dolphin sends unexpected
// non-STATUS commands (the pre-sent bytes corrupt that response);
// `corruption` counts how often that's happened.
void    gc2eth_spec_set_depth(uint8_t n);
uint8_t gc2eth_spec_get_depth(void);
uint8_t gc2eth_spec_get_outstanding(void);
uint32_t gc2eth_spec_get_corruption(void);

// Phase 3: WRITE-ack cache. N=0 disables. Useful for games where the
// WRITE jstat reply is constant across many commands (e.g. Madden's
// play-call loop, which always returns 0x32). Serves up to N WRITEs
// from cache before forcing a refresh. RESET invalidates immediately.
void     gc2eth_write_cache_set_n(uint16_t n);
uint16_t gc2eth_write_cache_get_n(void);
uint32_t gc2eth_write_cache_hits(void);
uint32_t gc2eth_write_cache_miss(void);

// Phase 4: READ-reply cache. RISKY — READ replies can carry actual
// game state (button presses, world updates). Caching them delays
// input responsiveness by up to N polls. Off by default.
void     gc2eth_read_cache_set_n(uint16_t n);
uint16_t gc2eth_read_cache_get_n(void);
uint32_t gc2eth_read_cache_hits(void);
uint32_t gc2eth_read_cache_miss(void);

bool     gc2eth_trace_armed(void);
void     gc2eth_trace_start(void);
void     gc2eth_trace_stop(void);
void     gc2eth_trace_record(uint8_t cmd, int n, const uint8_t* rx, int rx_len);
uint32_t gc2eth_trace_count(void);
bool     gc2eth_trace_get(uint32_t idx, gc2eth_trace_entry_t* out);

#endif
