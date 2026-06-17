# USB4AMI User Guide

## What is USB4AMI?

USB4AMI is a small adapter that lets you use modern USB controllers and mice with classic computers including the Commodore Amiga, Commodore 64, and Atari ST. It connects to your computer's joystick port and translates input from any compatible USB device into the signals your retro computer understands.

---

## What's in the Box

- USB4AMI adapter
- USB-A cable for firmware updates

---

## Connecting USB4AMI

1. Plug the DE9 cable into joystick port 1 on your retro computer.
2. Connect the other end to USB4AMI.
3. Plug your USB controller or mouse into the USB-A port on USB4AMI.
4. Power on your computer.

> **Note:** USB4AMI is powered by your USB device. No separate power supply is needed.

---

## LED Colors

The LED on USB4AMI tells you what's going on at a glance.

| Color | Meaning |
|-------|---------|
| Breathing (any color) | Waiting for a USB device to connect |
| **Amber** | Amiga mode active |
| **Blue** | Commodore 64 mode active |
| **Green** | Atari ST mode active |
| **Purple** | DPI adjustment mode active |

---

## Choosing Your Platform (Amiga, C64, or Atari ST)

USB4AMI needs to know which computer it's connected to. You select your platform by tapping the BOOTSEL button within the first **5 seconds** after powering on.

**How to do it:**
1. Power on your computer with USB4AMI connected.
2. Within 5 seconds, tap the BOOTSEL button to cycle through platforms:
   - Tap once → C64 (blue)
   - Tap again → Atari ST (green)
   - Tap again → back to Amiga (amber)
3. Stop tapping when your platform's color appears.

The LED will show the selected platform color. Your choice is automatically saved and remembered next time you power on.

> **Tip:** If you don't tap anything within 5 seconds, USB4AMI stays on your last saved platform.

---

## Using a Gamepad or Joystick

USB4AMI works with most USB gamepads including Xbox, PlayStation, and 8BitDo controllers.

### Basic Controls
- **D-pad or left analog stick** → directional movement
- **B button (or equivalent)** → fire button

### Turbo Fire
Hold **Select** and press a face button to toggle turbo fire on that button. The LED will blink briefly to confirm. Press the same combination again to turn turbo off.

### CD32 Controllers (Amiga only)
If you're using a CD32-compatible controller on the Amiga, USB4AMI automatically detects and enables the full CD32 button layout. No setup required.

---

## Using a Mouse

### Amiga and Atari ST
Connect any USB mouse and it will work as a standard quadrature mouse. Left and right buttons work as expected. On the Amiga, scroll wheel movement is also supported.

### Commodore 64 (C1351 Mode)
On C64, USB4AMI emulates a Commodore 1351 proportional mouse. This gives smooth, analog-style cursor movement in software that supports it, such as GEOS and Amiga ports of games.

- **Left mouse button** → fire/left button
- **Right mouse button** → second button (up pin)

---

## Adjusting Mouse Speed (DPI)

If your mouse feels too fast or too slow, you can adjust the sensitivity without any tools or software.

**How to adjust:**
1. Move your mouse to enter mouse mode (LED shows your platform color).
2. **Hold the middle mouse button for 2 seconds.** The LED turns **purple** to confirm you're in DPI adjust mode.
3. While the LED is purple:
   - **Left mouse button** → faster (less reduction)
   - **Right mouse button** → slower (more reduction)
4. **Tap the middle mouse button** to exit and save your setting.

The LED returns to your platform color and your new sensitivity is saved automatically.

> **Tip:** DPI is saved separately for each platform, so you can have different speeds for Amiga and C64.

---

## Troubleshooting

**The adapter doesn't seem to be doing anything.**
Make sure you're on the right platform. The LED color tells you which platform is active. If unsure, power cycle and let the 5-second window pass — it will load your last saved platform.

**My mouse or gamepad isn't being recognized.**
Try a different USB device. Some very old or unusual USB devices may not be supported. Most modern gamepads and mice work out of the box.

**The computer behaves strangely after switching firmware or experimenting.**
Cycle through all platforms using the BOOTSEL button and back to your desired platform. This writes fresh settings to flash and clears any leftover configuration.

**The cursor moves in the wrong direction.**
Check that you're in the correct platform mode. C64 mouse mode (C1351) and Amiga mouse mode handle movement differently.

**I accidentally changed the DPI and want to reset it.**
Enter DPI adjust mode (hold MMB 2s) and tap left or right until the speed feels right, then tap MMB to save.

---

## Technical Notes

- USB4AMI remembers your platform and DPI settings across power cycles.
- If you ever need to fully reset, cycle through all platforms — this rewrites settings to flash with fresh defaults.
- The BOOTSEL button is only active for platform selection during the first 5 seconds after power-on.
