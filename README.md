<p align="center">
  <img src="docs/images/retro_frog_logo.png" alt="Retro Frog" width="300">
</p>

<p align="center">
  <strong>Firmware for Retro Frog USB controller adapters</strong>
</p>

<p align="center">
  Use modern USB HID controllers and mice on classic computers — no drivers, no configuration, no compromises.
</p>

<p align="center">
  <a href="https://retrofrog.net"><img src="https://img.shields.io/badge/Website-retrofrog.net-ff69b4?style=for-the-badge" alt="Website" /></a>
  <a href="https://github.com/thgill/joypad-os/releases"><img src="https://img.shields.io/github/downloads/thgill/joypad-os/total?style=for-the-badge&label=Downloads" alt="Downloads" /></a>
  <a href="https://github.com/thgill/joypad-os/blob/main/LICENSE"><img src="https://img.shields.io/github/license/thgill/joypad-os?style=for-the-badge" alt="License" /></a>
</p>

---

## Products

### USB4AMI — USB HID to Amiga / C64 / Atari ST

Use any modern USB gamepad or mouse with your Commodore Amiga, Commodore 64, or Atari ST. Plugs into the DE9 joystick port with no modification to your computer required.

**Supported input:**
- USB gamepads — Xbox, PlayStation, Nintendo Switch, 8BitDo, and most generic HID controllers
- USB mice — any standard USB mouse or trackball

**Output protocols:**
- Amiga joystick and quadrature mouse
- CD32 controller (auto-detected on Amiga)
- Commodore 64 joystick and C1351 proportional mouse
- Atari ST joystick and quadrature mouse


**[USB4AMI User Guide →](docs/usb4ami/user_guide.md)**

**[Firmware Releases →](https://github.com/thgill/joypad-os/releases)**

---

### USB4NEO — USB to Neo Geo *(coming soon)*

*Documentation coming soon.*

---

## Flashing Firmware

1. Download the latest `.uf2` for your adapter from [Releases](https://github.com/thgill/joypad-os/releases)
2. Hold the button and connect the USB-A cable to your computer
3. Drag the `.uf2` file onto the `RP2350` drive that appears
4. Done — the drive ejects and your adapter is running the new firmware
5. Note: Always disconnect the USB adapter from your console/vintage computer prior to updating!!

---

## Building from Source

Retro Frog firmware is built on the [Joypad OS](https://github.com/joypad-ai/joypad-os) open-source firmware platform by Robert Dale Smith.

### Prerequisites

```bash
# macOS
brew install --cask gcc-arm-embedded
brew install cmake git
```

### Build

```bash
git clone https://github.com/thgill/joypad-os.git
cd joypad-os
make init

# USB4AMI (Retro Frog RP2354A board)
make usb2ami_retrofrog
```

Output: `releases/joypad_<commit>_usb2ami_retrofrog.uf2`

---

## Support

- **Website:** [retrofrog.net](https://retrofrog.net)
- **Issues:** [GitHub Issues](https://github.com/thgill/joypad-os/issues)

---

## License & Attribution

Retro Frog firmware is built on [Joypad OS](https://github.com/joypad-ai/joypad-os), created by [Robert Dale Smith](https://github.com/joypad-ai).

Both are licensed under the **[Apache License 2.0](LICENSE)**.

Retro Frog product additions and modifications are copyright © 2026 Todd Gill/Retro Frog.
