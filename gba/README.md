# GBA

Joypad-OS targets and tooling for the Game Boy Advance.

Today this contains a single multiboot payload (`joypad/`) that gets
embedded into RP2040 firmware and uploaded over the GC↔GBA link cable by
`gc2usb`. The directory is laid out as a top-level home (sibling of
`src/`, `esp/`, `nrf/`) so future GBA-side work — EXT-port host drivers,
a custom GBA cart running joypad-os core, etc. — has somewhere obvious
to live.

## Toolchain

Requires devkitPro / devkitARM:

```bash
# macOS
brew install --cask devkitpro-pacman
sudo dkp-pacman -S gba-dev

# Linux
# https://devkitpro.org/wiki/devkitPro_pacman
```

`$DEVKITPRO` should resolve to `/opt/devkitpro` (or wherever you installed).

## Building a payload

```bash
cd gba/joypad
make
# Produces: build/joypad_mb.gba    — the multiboot ROM
#           build/joypad_payload.c — same bytes as a C array, ready to drop
#                                    into src/native/host/gc/gba_payload.c
```

To use as the `gc2usb` payload:

```bash
cp gba/joypad/build/joypad_payload.c src/native/host/gc/gba_payload.c
make gc2usb_kb2040
```

## What's here

- `joypad/` — multiboot payload: animated cartoon eyes overlay (port of
  `src/core/services/display/eyes_anim.c`) on the GBA's 240×160
  framebuffer, with gaze tracking from the d-pad and emotion reactions
  from button presses. The joybus handshake + per-VBlank JOYTR write is
  Doridian's `gba-as-controller` reference
  (github.com/Doridian/Joybus-PIO) verbatim, so the GBA still works as a
  USB controller.
- `tools/bin2c.py` — converts a binary `_mb.gba` ROM to the
  `gba_payload[]` C array format that `gc2usb`'s `gba_multiboot` expects.

## Adding a new payload

Copy `joypad/` to `<your-payload>/`, edit `Makefile`'s `TARGET` and the
source. The Makefile uses devkitARM's stock `%_mb.elf` rule (via
`gba_mb.specs`) so the output is multiboot-format (loads at `0x02000000`
instead of `0x08000000`).

## Lineage

The joybus handshake + main loop in `joypad/source/main.c` (the
`0x30303030` exchange, `ResetHalt`, `REG_JSTAT` polls, SVC 0x26 BIOS
reset on `JOYCNTRL.RST`) are taken verbatim from Doridian's
[`Joybus-PIO`](https://github.com/Doridian/Joybus-PIO) `gba/source/main.c`
(2023). Touching that sequence breaks the cable's level-shifter MCU and
the host stops getting input — so it stays as-is. The eyes overlay runs
in the VBlank slot on top of it.

Chain of credit:

- **gbatek** (Martin Korth, <https://problemkaputt.de/gbatek.htm>) —
  authoritative GBA hardware reference, including SIO / Joybus / BIOS
  reset semantics.
- **VisualBoyAdvance** link-cable emulation, ported by Sage-of-Mirrors as
  [`libgbacom`](https://github.com/Sage-of-Mirrors/libgbacom) — original
  reverse-engineering of the joyboot stream cipher.
- **FIX94's
  [`gc-gba-link-cable-demo`](https://github.com/FIX94/gc-gba-link-cable-demo)** —
  the canonical GameCube-side reference for booting a GBA over the link
  cable.
- **Doridian's [`Joybus-PIO`](https://github.com/Doridian/Joybus-PIO)** —
  RP2040-PIO implementation of the GameCube side, plus the tiny GBA-side
  payload we forked here.
- **This repo** — eyes-anim render layer, per-emotion palette, idle
  wander, dpad-as-analog gaze on top of Doridian's payload.
