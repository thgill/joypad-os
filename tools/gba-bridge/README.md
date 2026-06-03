# gba-bridge

Host-side daemon that drives the **joybus bridge** mode of `gc2usb`
firmware via USB CDC. Two main use cases:

1. **Upload a GBA multiboot ROM** to a real GBA on the cable, from a
   `.gba` file on the host (no need to embed it in firmware).
2. **Bridge Dolphin's GBA TCP link-cable protocol** to a real GBA, so
   stock Dolphin can talk to physical hardware over the joybus cable.

> **Status:** experimental. Multiboot upload from host works reliably.
> Dolphin passthrough works end-to-end with games that tolerate slow
> multiboot (Madden 2003 verified) but is slow — see "Performance"
> below. This tool is a stepping stone to a future on-firmware
> Ethernet path (`gc2eth`) that won't have the USB FS latency ceiling.

## Setup

```bash
cd tools/gba-bridge
npm install
```

Requires Node 18+ and a `gc2usb` build of joypad-os flashed to a
KB2040 (or similar) with the joybus cable wired to a GBA.

## Usage

```bash
# Quick health check: confirm firmware sees a GBA in BIOS multiboot wait state
node index.js --probe-only

# Send joybus RESET (0xFF) to the GBA
node index.js --reset

# Upload a multiboot .gba ROM from the host
node index.js --multiboot path/to/rom_mb.gba

# Run the Dolphin GBA TCP bridge (CLIENT mode — connects to Dolphin)
node index.js --dolphin
```

## Dolphin setup

In Dolphin **before** loading a GameCube game:

- Config → GameCube → Slot A/B/C/D = `GBA (TCP)`
- Start the daemon: `node index.js --dolphin`
- Boot a game that supports GBA link (Madden 2003, Crystal Chronicles,
  Pokémon Colosseum, etc.)

The daemon connects as a client to Dolphin's GBASockServer
(`localhost:54970` joybus, `localhost:49420` clock).

## How it works

The daemon talks to firmware via three CDC command families exposed
when `JOYBUS.BRIDGE.START` is sent:

- `JOYBUS.XFER` — single primitive joybus exchange (tx bytes, rx
  bytes, timeout)
- `JOYBUS.BATCH` — multiple xfers in one CDC roundtrip; ~70 ops
  amortizes the USB FS frame cost
- `GBA.MB.{RESET,CHUNK,UPLOAD}` — stage a ROM in firmware RAM, then
  fire the multiboot upload natively (BIOS handshake is too timing-
  sensitive to drive over CDC)

While the bridge is active, firmware suspends gc_host's autopoll and
GBA-payload autoboot so its traffic doesn't race with daemon-driven
joybus exchanges.

## Performance

| Path | Madden 2003 multiboot |
|---|---|
| Native firmware autoboot (embedded payload) | ~6 s |
| Daemon → `GBA.MB.UPLOAD` (host-staged) | ~6 s |
| **Dolphin passthrough** (per-WRITE roundtrip) | **1-2 minutes** |

Dolphin passthrough is slow because every joybus WRITE Dolphin issues
becomes one USB CDC roundtrip (~3 ms minimum on USB Full-Speed) and a
typical multiboot is ~13 K WRITEs. The fundamental ceiling is the USB
FS 1 ms frame in each direction; nothing in software fixes it.

The disabled intercept-and-replay code in `index.js` (the
`AWAIT_OURKEY`/`CAPTURE`/`REPLAYING` branches) is a partial workaround
that decrypts Dolphin's WRITE storm in the daemon and runs the
firmware-native upload instead. It works for the cipher math but
games that poll JSTAT.RECV-bit transitions between WRITEs (Madden's
BIOS state machine) bail before the upload finishes. Pure passthrough
is slow but correct.

A future `gc2eth` firmware on a Waveshare RP2040-ETH or PoE
FeatherWing would put the Dolphin TCP server directly in firmware via
W5500 and drop per-cmd RTT from ~3 ms to ~500 µs — passthrough would
finish in ~12-15 s. That's the planned "real" solution.

## Wire protocol

CDC framing matches `src/usb/usbd/cdc/cdc_protocol.h`:

```
[0xAA][len lo][len hi][type][seq][payload bytes][crc lo][crc hi]
```

`type=0x01` CMD, `type=0x02` RSP. Bridge commands use JSON payloads.

## Firmware commands

| Command | Args | Returns |
|---------|------|---------|
| `JOYBUS.BRIDGE.START` | — | `{"ok":true}` (gc_host autopoll/autoboot paused) |
| `JOYBUS.BRIDGE.STOP` | — | `{"ok":true}` (autopoll/autoboot resumed) |
| `JOYBUS.BRIDGE.STATUS` | — | `{"ok":true,"state":"IDLE"\|"ACTIVE"}` |
| `JOYBUS.XFER` | `{tx, rx_len, timeout_us}` | `{"ok":true,"rx":hex,"got":N}` |
| `JOYBUS.BATCH` | `{ops, timeout_us}` | `{"ok":true,"out":hex}` |
| `GBA.MB.RESET` | — | `{"ok":true,"size":0}` |
| `GBA.MB.CHUNK` | `{data:hex}` | `{"ok":true,"size":N}` |
| `GBA.MB.UPLOAD` | `{channel}` | `{"ok":true,"size":N}` or `{"ok":false,"code":N}` |
