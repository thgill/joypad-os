# Joypad OS — WCH CH32V307 port (`wch/`)

`usb2usb` on the **CH32V307** (WCH RISC-V dual-USB MCU), built around TinyUSB.
Mirrors the `esp/` and `nrf/` ports: a top-level build directory that pulls the
shared Joypad sources from `../src` and supplies a thin platform HAL + stubs.

```
USBFS (PA11/PA12) ─ host in  ─► Joypad usbh registry ─► router ─► usbd modes ─► USBHS (PB6/PB7) ─ device out
```

- **Host input** = native **USBFS** (full speed), driven by the async TinyUSB
  CH32 HCD (`src/lib/tinyusb/src/portable/wch/hcd_ch32_usbfs.c`). The WCH-backed
  hybrid HCD is **not** used — see `.dev/docs/ch32v307/host-stack-decision.md`.
- **Device output** = native **USBHS** (high-speed PHY), WCH USBHS DCD.
- All Joypad output modes (HID/SInput/XInput/PS3/PS4/Switch/XID/Xbox One/…),
  the full USB-host device registry, router, players, and CDC config channel are
  compiled in — same shared code as RP2040/ESP32/nRF.

## Build

```bash
cd wch
make            # → _build/ch32v307v_r1_1v0/wch.elf  (BOARD=ch32v307v_r1_1v0)
make flash      # flash via WCH-LinkE (cargo install wlink)
make reset
make clean
```

Toolchain: **WCH gcc 8.2.0** (`riscv-none-embed-`). Newer GCC miscompiles the
WCH USB timing code, so it is pinned. Set `GCC8_BIN` if it lives somewhere other
than `/tmp/gcc8/...`. See `.dev/docs/ch32v307/BRINGUP.md` for how to obtain it.

Debug log: **USART1 / PA9 @115200** (bridged by the WCH-Link CDC). `make monitor`
prints the hint; use COMrade or `screen /dev/cu.usbmodem* 115200`.

## Layout

```
wch/
├── Makefile            # wraps TinyUSB's make build + full usb2usb manifest
└── src/
    ├── main.c          # entry: board_init → services → app → interface init → loop
    ├── tusb_config.h   # device (USBHS rhport 0) + host (USBFS rhport 1) config
    ├── flash_wch.c     # flash.h API, RAM-backed (no persistence yet)
    ├── button_wch.c    # button.h API (no user button wired yet)
    ├── ws2812_wch.c    # neopixel HAL no-op (no RGB LED wired)
    ├── stubs_peripheral.c  # weak BT/wiimote stubs (no Bluetooth on this target)
    └── pico/rand.h     # get_rand_32() shim for libxsm3 (Xbox 360 auth)
```

The platform HAL (time/identity/reboot) is shared at
`../src/platform/ch32/platform_ch32.c`.

## SRAM budget (64KB) — important

The full feature set is a tight fit on the CH32V307's **64KB SRAM**. To fit, this
build:

- sizes the CH32 HCD `usb_device_map` by `CFG_TUH_DEVICE_MAX` (was a fixed 128
  entries ≈ 10KB),
- overrides `MAX_PLAYERS_PER_OUTPUT=4` and `MAX_BLEND_DEVICES=4` (router arrays,
  `#ifndef`-guarded in the shared headers),
- trims `CFG_TUH_XINPUT=2` and `CFG_TUD_CDC_TX_BUFSIZE=512`.

Result: ~58KB used, ~5.6KB free above a 2KB stack. If more headroom is needed
(e.g. adding device modes), the CH32V307 can be reconfigured for a larger SRAM
split via its option bytes — not yet done here.

## Status

- ✅ Builds + links the full usb2usb app for `ch32v307v_r1_1v0`.
- ⏳ On-hardware bring-up (flash + enumerate a controller + present a device)
  pending — needs the WCH-LinkE probe connected. The host HCD is proven to
  enumerate keyboards/mice/gamepads standalone; this wires it into the Joypad
  pipeline.
