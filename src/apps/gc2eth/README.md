# gc2eth — GameCube/GBA Ethernet bridge for Waveshare RP2040-ETH

**Status: experimental example, not a shipping product.**

A working proof-of-concept that bridges Dolphin's GBA-link TCP protocol to a
real GBA over joybus using the [Waveshare RP2040-ETH](https://www.waveshare.com/wiki/RP2040-ETH)
board. The board pairs an RP2040 with the WCH **CH9120** UART-to-Ethernet chip.

## What works

End-to-end multiboot of a GBA via TCP:

```
Dolphin (Mac) ──TCP──► CH9120 ──UART──► RP2040 ──joybus──► GBA
```

The firmware implements an **intercept-replay** state machine: it watches
Dolphin's protocol pattern (STATUS-with-PSF0 → READ session_key → WRITE
our_key), captures the encrypted body to RAM while fake-acking Dolphin, then
fires the native `gba_mb_upload()` to actually boot the GBA at joybus rate.

Verified working with `tools/gba-bridge/eth-multiboot.js` — boots the embedded
joypad payload onto a GBA in BIOS multiboot wait state.

## What doesn't work (and why)

**Madden NFL 2003 multiboot specifically does not complete** through this
bridge. The CH9120 chip has a ~30 ms per-TCP-roundtrip latency floor
(measured with `tools/gba-bridge/eth-ping.py`). Madden's per-attempt
multiboot timeout is ~10 seconds, but with 4000+ WRITE commands the body
upload takes ~127 seconds end-to-end. Madden gives up and retries before
the upload can finish.

For comparison, the gc2usb (USB CDC) path has ~3 ms RTT — 10× faster —
which is why Madden multiboot can complete (slowly) over USB CDC but not
through this Ethernet bridge.

This is **not a firmware bug** and not fixable through chip configuration
(PKT_LEN, PKT_TIMEOUT, delayed-ACK on Mac, UDP_SERVER mode were all tried).
The CH9120's TCP stack is inherently high-latency.

## Recommended path for production

The CH9120 was selected for this experiment because it's a popular, cheap
UART-to-Ethernet bridge. For a production gc2eth that completes any
multiboot game (including Madden), use a **W5500-based RP2040 board** —
W5500 chips have sub-millisecond TCP stacks. The firmware port is mostly:

- Replace `wshare_ch9120.c` with a W5500 SPI driver
- Update `app.c` boot init order
- Update `Makefile`/CMake board target

The intercept-replay logic, joybus path, and Dolphin protocol handling
are all reusable.

## Build & test

```bash
make gc2eth_rp2040_eth                # build firmware UF2
make flash-gc2eth_rp2040_eth          # flash via BOOTSEL volume

# Test without Dolphin (uses /tmp/joypad_mb.gba):
python3 tools/gba-bridge/extract_payload.py
node tools/gba-bridge/eth-multiboot.js --rom /tmp/joypad_mb.gba
# Reset gc2eth, watch GBA boot the joypad payload.

# Latency benchmark:
python3 tools/gba-bridge/eth-ping.py
```

## Network config

- Chip IP: `192.168.1.250` (static)
- Chip mode: TCP_CLIENT
- Target: `192.168.1.159:54970` (Mac running Dolphin / fake-Dolphin listener)
- UART: 921600 baud, PKT_LEN=5, PKT_TIMEOUT=1 (5 ms)

Edit `wshare_ch9120.c` to change these for your network.

## Files

- `app.c` — App entry, intercept-replay state machine, Dolphin protocol handler
- `app.h` — Pin definitions (joybus on GP2, CH9120 on UART1/GP17–21)
- `app_config.h` — App version + name
- `ch9120.c/h` — Cleaner CH9120 driver (used for runtime status queries)
- `wshare_ch9120.c/h` — Waveshare's official demo CH9120 driver (used at init)

## Pins (Waveshare RP2040-ETH)

| RP2040 | CH9120 / function |
|---|---|
| GP2 | joybus data line to GBA |
| GP20 | UART1 TX → chip RXD |
| GP21 | UART1 RX ← chip TXD |
| GP17 | TCPCS (TCP connection status, active low) |
| GP18 | CFG0 (low = config mode, high = data mode) |
| GP19 | RES (active-low reset) |
