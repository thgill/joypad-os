# USB4AMI User Guide

## What is USB4AMI?

USB4AMI is a small adapter that lets you use modern USB controllers and mice with classic computers including the Commodore Amiga, Commodore 64, Atari ST and Atari 8 bit computers. It connects to your computer's joystick/mouse port and translates input from any compatible USB device into the signals your retro computer understands.

---

## Supported Platforms

Amiga (All)
Commodore 64 (including Ultimate)
Commodore 128
Atari ST (all)
Atari Falcon
Atari 8 Bit Computers (All)
MEGA65


## What's in the Box

- USB4AMI adapter
- USB-A cable for firmware updates

---

## Connecting USB4AMI

1. Plug the USB4AMI into your retro computer's joystick port.
2. Plug your USB controller or mouse into the USB-A port on USB4AMI.
3. Power on your computer.

> **Note:** USB4AMI is powered by your computer. No separate power supply is needed.

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

USB4AMI needs to know which computer it's connected to. You select your platform by tapping the BOOTSEL button within the first **8 seconds** after powering on.

**How to do it:**
1. Power on your computer with USB4AMI connected.
2. Within 8 seconds, tap the BOOTSEL button to cycle through platforms:
   - First tap → C64 (blue)
   - Second tap → Atari ST (green)
   - Third tap → back to Amiga (amber)
3. Stop tapping when your platform's color appears.

The LED will show the selected platform color. Your choice is automatically saved and remembered next time you power on.

> **Tip:** If you don't tap anything within 8 seconds, USB4AMI stays on your last saved platform.

---

## Using a Gamepad or Joystick

USB4AMI works with most USB gamepads including Xbox, PlayStation, Nintendo Switch, and 8BitDo controllers, as well as most generic USB HID gamepads.

### Basic Controls
- **D-pad or left analog stick** → directional movement
- **B button (or equivalent)** → fire button

### Turbo Fire
Hold **Select** and press a face button to toggle turbo fire on that button. The LED will blink 2 times every 3 seconds to indicate that turbo is enabled. Press the same combination again to turn turbo off.

### CD32 Controllers (Amiga only)
By default USB4AMI automatically detects and enables the full CD32 button layout. No setup required. And just like an original CD32 controller, it will gracefully fall back to 1 or 2 button gamepad for games that don't take advantage of the CD32's extra buttons. 

---

## Using a Mouse

### Amiga
Connect any USB mouse and it will work as a standard quadrature mouse. Left and right buttons work as expected.

### Atari ST
Connect any USB mouse and it will work as a standard Atari ST quadrature mouse. Left and right buttons work as expected.

### Commodore 64 (C1351 Mode)
On C64, USB4AMI emulates a Commodore 1351 proportional mouse. This gives smooth, analog-style cursor movement in software that supports it, such as GEOS.

- **Left mouse button** → left button
- **Right mouse button** → right button

---

## Adjusting Mouse Speed (DPI)

If your mouse feels too fast or too slow, you can adjust the sensitivity without any tools or software. USB4AMI uses a DPI divisor to scale mouse movement — a lower divisor means faster movement, a higher divisor means slower movement. The divisor range is **1–8**, with a default of **2**.

**How to adjust:**
1. Connect your USB mouse (LED shows your platform color).
2. **Hold the middle mouse button for 2 seconds.** The LED turns **purple** to confirm you're in DPI adjustment mode.
3. While the LED is purple:
   - **Left mouse button** → faster (decreases the divisor)
   - **Right mouse button** → slower (increases the divisor)
4. **Tap the middle mouse button** to exit and save your setting.

The LED returns to your platform color and your new setting is saved automatically.

> **Tip:** DPI is saved separately for each platform, so you can have different speeds set for Amiga, C64, and Atari ST!

---

## Updating Firmware

1. Download the latest `.uf2` for USB4AMI from [Releases](https://github.com/thgill/joypad-os/releases)
2. **Disconnect USB4AMI from your retro computer first**
3. Hold the BOOTSEL button and connect the USB-A cable to your computer
4. Drag the `.uf2` file onto the `RP2350` drive that appears
5. Done — the drive ejects and your adapter is running the new firmware

> **Note:** USB4AMI automatically resets its settings to safe defaults after a firmware update. Your platform selection and DPI settings will need to be reconfigured.

---

## Troubleshooting

**The adapter doesn't seem to be doing anything.**
Make sure you're on the right platform. The LED color tells you which platform is active. If unsure, power cycle — USB4AMI will load your last saved platform automatically and then you can press the button to cycle to the desired platform. 

**My mouse or gamepad isn't being recognized.**
Try a different USB device. Some very old or unusual USB devices may not be supported. Most modern gamepads and mice work out of the box.

**The computer behaves strangely after a firmware update.**
This should not happen — USB4AMI automatically resets its settings to safe defaults the first time it boots after a firmware update. If you do experience issues, tap BOOTSEL (during first 8 seconds of power on) to cycle to your desired platform which will rewrite your settings cleanly.

**The cursor moves in the wrong direction.**
Check that you're in the correct platform mode. Amiga and Atari ST use different quadrature encodings and if the cursor misbehaves, make sure the LED color matches your computer.

**I accidentally changed the DPI and want to reset it.**
Enter DPI adjustment mode (hold MMB 2 seconds) and adjust left or right until the speed feels right, then tap MMB to save. The default divisor is 2.

---

## Technical Notes

- USB4AMI remembers your platform and DPI settings across power cycles.
- Settings are automatically reset to defaults the first time USB4AMI boots after a firmware update.
- The BOOTSEL button is only active for platform selection during the first 8 seconds after power-on.

---

## TODO List

Amiga Scroll Wheel - This will likely require a custom driver to be written (or perhaps we can piggyback off an existing one)
C64 Scroll Wheel for C64OS 
Investigate Atari 7800 Support



