# Dolphin Fork — `joypad-gba-usb` branch

Custom Dolphin fork that adds a USB-vendor-bulk SI device for the
joypad-os GBA-link bridge running in `USB_OUTPUT_MODE_GBA_LINK`.
Replaces the existing TCP-based GBA SI path (~1.5–2.5 ms per cmd
through the macOS kernel network stack) with direct libusb_bulk_transfer
to the bridge (~125–500 µs per cmd) so sustained-traffic GBA-link
games (FFCC, Crystal Chronicles, Pac-Man Vs.) run at full speed.

Lives at `~/git/dolphin/` on a branch named `joypad-gba-usb`.

## Setup

```bash
cd ~/git
git clone --depth=1 https://github.com/dolphin-emu/dolphin.git
cd dolphin
git checkout -b joypad-gba-usb
git apply <(curl -L <patch-bundle>) # or cherry-pick the commit
```

The patch adds:

* `Source/Core/Core/HW/SI/SI_DeviceGBAUSB.{cpp,h}` — new SI device
* `Source/Core/Core/HW/SI/SI_Device.{cpp,h}` — new `SIDEVICE_GC_GBA_USB`
  enum value + factory branch (gated on `HAS_GBA_USB`)
* `Source/Core/Core/CMakeLists.txt` — adds the sources + defines
  `HAS_GBA_USB` inside the existing `if(TARGET LibUSB::LibUSB)` block
* `Source/Core/DolphinQt/Config/GamecubeControllersWidget.cpp` — adds
  "GBA (USB - joypad-os)" to the SI port dropdown

## Build (macOS)

Standard Dolphin macOS build steps. Requires Homebrew and the usual
Dolphin deps (Qt, libusb, etc.):

```bash
brew install cmake qt@6 libusb ffmpeg sfml hidapi spirv-tools \
             enet bzip2 curl xz mbedtls zstd minizip-ng

cd ~/git/dolphin
mkdir -p build && cd build
cmake .. -GNinja \
  -DCMAKE_PREFIX_PATH="$(brew --prefix qt@6)" \
  -DCMAKE_BUILD_TYPE=Release
ninja
```

Output: `~/git/dolphin/build/Binaries/Dolphin.app`.

## Runtime requirements

* Bridge firmware in `USB_OUTPUT_MODE_GBA_LINK` mode (gc2usb build,
  flip via CDC `MODE=14` or whatever the enum resolves to)
* libusb 1.0 installed system-wide (`brew install libusb`)
* GBA powered on with no cartridge for multiboot games (BIOS multiboot
  listen state)

## Use

1. Launch the forked Dolphin
2. Controllers → GameCube Controllers → set the relevant port (Port 1
   for Madden, Port 2 or 4 for Pokémon Box / Colosseum, depends on
   the game) to **GBA (USB - joypad-os)**
3. Apply, boot the game

The Dolphin SI device opens libusb on first frame, looks for VID
`0x057E` PID `0x0338`, and starts driving the bridge. If the bridge
isn't found, retries every ~1 second (silent backoff to avoid log
spam). The `SerialInterface` log channel emits `INFO`/`WARN` lines
visible at log levels Info or higher.

## Why a fork instead of upstream

For now: keep iteration speed up. Once the design is stable and
demonstrably useful for the listed games, worth proposing upstream
to Dolphin since the patch is small (~300 lines) and additive.
