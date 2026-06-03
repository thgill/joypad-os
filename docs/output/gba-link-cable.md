# GBA Link Cable

> **Status:** the **GameCube → GBA Link Cable controller** mode (gc2usb autoboot of an embedded GBA payload, GBA reads back as a HID gamepad) is **production**. The **USB-vendor bridge to a forked Dolphin emulator** is **experimental** and **disabled by default in the firmware**.

The Nintendo DOL-011 GameCube-to-Game-Boy-Advance Link Cable connects an unmodified GBA to a GameCube's SI bus on Port 2. The GameCube can:

1. **Upload a multiboot ROM to the GBA's RAM** (the BIOS "multiboot" handshake, ~16 KB max, Kawasedo cipher).
2. **Talk to the running multiboot payload via joybus** (4-byte WRITE / 4-byte READ over the SIO link).

Joypad OS implements both halves on the bridge side (RP2040 + joybus PIO), and uses them in two different ways depending on what's plugged in to the bridge's USB-host port.

---

## 1. GameCube ↔ Real-GBA controller (production, default-on)

This is the default behavior of `gc2usb_feather_usbhost` and the other gc2usb targets:

1. On boot, the firmware initializes the joybus PIO state machine for the link-cable wire.
2. Once it sees a GBA on the link, it uploads an embedded multiboot ROM (`src/native/host/gc/gba_payload.c`, the "joypad eyes" payload) using the Kawasedo cipher.
3. The payload runs on the GBA and constantly publishes the GBA's button state back over joybus.
4. The firmware reads that button state every poll and routes it through the standard input event pipeline. The GBA appears as a HID gamepad on the host computer alongside any other connected controller.

Hardware:

* Adafruit Feather RP2040 USB Host (or Pico variants — see Makefile target list).
* DOL-011 link cable (or compatible bare wire) tapped to `GC_PIN_DATA` on the bridge. Default pin is `GP4` on Feather and `GP29` on the Pico variants.
* The GBA must be powered (battery or AC) and on the BIOS multiboot wait screen.

Caveats:

* The first joybus exchange after a GBA cold-start fails ~50% of the time (transient PIO/wire glitch), so the firmware retries `RESET` / `STATUS` (idempotent) on timeout. Steady-state reliability after the warm-up is effectively 100%.
* Both halves of the link cable share one wire (open-drain joybus), so the firmware also has to settle the bus 150 µs between back-to-back commands to avoid GBA-side overrun.

---

## 2. Dolphin GBA-link via USB vendor bridge (experimental, default-OFF)

> The idea: instead of having Dolphin talk to a TCP-based GBA proxy (the upstream `GBA (TCP)` mode), have it talk directly to the bridge over USB so a *real* GBA on a real link cable can be the GBA in a Dolphin GameCube session. Targets games like Madden 2003 (Madden Cards) and FFCC where the in-game GBA half is normally simulated via libmgba.

### What's implemented

**Firmware side** (`src/usb/usbd/modes/gba_link_mode.c`, gated on `CONFIG_JOYBUS_BRIDGE`):

* `USB_OUTPUT_MODE_GBA_LINK` (mode 14) — composite USB device, CDC + vendor-class. VID 0x057E / PID 0x0338 ("Nintendo / GameCube GBA Link Cable").
* Vendor bulk OUT receives raw joybus command bytes from the host (1 byte for `RESET`/`STATUS`/`READ`, 5 bytes for `WRITE`).
* Bridge forwards to the GBA over real joybus, reads the GBA's reply, returns it on vendor bulk IN.
* Per-cmd timeout/retry budget tuned per command type (STATUS 30 ms × 5, others 5 ms × 2) — see telemetry below.
* Diagnostic CDC commands: `JOYTEST` (single RESET probe), `JOYPIN?` (sample the data pin), `GBALINK?` / `GBALINK!` / `GBALINK0` (per-cmd telemetry + reset).

**Dolphin fork side** (`~/git/dolphin/`, branch `joypad-gba-usb`):

* `CSIDevice_GBA_USB` — libusb-bulk transport in `Source/Core/Core/HW/SI/SI_DeviceGBAUSB.{cpp,h}`.
* Dropdown entry "GBA (USB)" in `GamecubeControllersWidget.cpp`, gated on `HAS_GBA_USB` define (set when libusb is found).

### What works

* End-to-end `RESET → STATUS → READ session_key → WRITE our_key → stream 4170 body WRITEs → WRITE fcrc → READ crc_reply` multiboot of the embedded payload via `tools/gba-bridge/usbgba-multiboot.py` — reliable, ~3.8 s wall-clock (faster than real GameCube hardware's ~10 s).
* Madden 2003 multiboot via the fork — multiboots and reaches Madden Cards, GBA runs the Madden Cards mini-game ROM.
* FFCC — connects and plays.

### What's broken / why it's default-off

**Madden's menu / connect time via the bridge is many minutes vs ~1 minute with libmgba**, and the slowness is present from the moment "GBA (USB)" is selected (before any multiboot is even attempted). FFCC has similar in-game lag.

Root cause: **`libusb_bulk_transfer` in Dolphin's CPU thread blocks for the entire USB+joybus round-trip on every SI tick** (~750 µs/tick). With Madden doing dozens of SI ops per frame, that's milliseconds of frozen CPU per frame — game-wide slowness whenever the device is selected.

The original `CSIDevice_GBA` (TCP) doesn't have this issue because SFML socket `Receive()` is non-blocking. `CSIDevice_GBAEmu` (libmgba) doesn't have it because the GBA core runs in its own thread with shared-memory queue I/O. To match either pattern, `CSIDevice_GBA_USB` needs to move the libusb calls off Dolphin's CPU thread. Two attempts during the May 17 session both broke detection:

* **Worker thread + std::queue (commit `c3ba522`, reverted in `61bc6f2`)** — only 9 STATUS in 8 minutes; suspected state-machine race I couldn't bisect without live observability.
* **libusb async API + event-pump thread (commit `3a28891`, reverted in `7e6e8eb`)** — only 129 STATUS in many minutes; same failure mode. Likely libusb-event-loop wiring issue.

Both compiled cleanly and didn't crash; the failure mode was "Madden sees the device as broken." Dolphin already does async libusb correctly in `IOS::USB` — the fix is probably to crib that pattern wholesale rather than reinvent.

### Other dead ends explored

* **Switching Dolphin to DSP-HLE** instead of DSP-LLE — made things much worse. HLE's instant cipher math caused Dolphin to send SI commands faster than the real GBA could keep up; READ/WRITE timeouts appeared, Madden retried multiboot 100+ times. LLE's per-cycle pacing is exactly what the real GBA needs.
* **Bypassing Dolphin's `WaitTransferTime` SI-pacing for our device** — broke Madden in subtle ways. Madden's protocol expects commands at simulated SI-bus rate; arriving faster confused it.
* **Intercept-replay** (recognize Madden's multiboot pattern, capture encrypted bytes on-device, run native upload, fake replies to Dolphin) — was implemented in `gc2eth_feather` then disabled (`s_intercept_enabled = false`, `src/apps/gc2eth_feather/app.c:372`). Madden polls STATUS between WRITEs and watches the JSTAT.RECV bit toggle to confirm bytes are landing on the real GBA. Faking replies → Madden aborts within a few WRITEs.
* **Aggressive STATUS retries** (50 × 30 ms) — made connect *worse*: when joybus genuinely glitched, the long retry chain blocked the USB pipe for >150 ms per failure, longer than Madden's per-handshake-step budget; Madden gave up and issued 100+ RESETs.
* **Shorter joybus timeouts** (1 ms WRITE) — caused cipher desync (late GBA reply we falsely treated as timeout). Multiboot would complete but GBA booted to a black screen.

The settled sync config (commit `7c2eabf` in the fork) — 30 ms STATUS × 5 retries, 5 ms WRITE/READ × 2, 2 ms `libusb_bulk_transfer` poll on Receive — is the configuration that has actually been observed to let Madden complete multiboot in 1–3 attempts and reach the Cards screen. Just slowly, because every SI tick still blocks the CPU thread for the round trip.

### Telemetry from a real Madden session via the bridge

```
RESET   n=3      avg=145 µs  max=147 µs    retries=0      to=0    ← perfect
STATUS  n=680    avg=3 ms    max=91 ms     retries=474   to=82   ← intrinsic GBA-BIOS lag during multiboot bursts
READ    n=4681   avg=207 µs  max=235 µs    retries=0      to=0    ← perfect (includes steady-state polling)
WRITE   n=71269  avg=210 µs  max=218 µs    retries=0      to=0    ← perfect
WRITE bad_jstat = 0
```

The 750 µs/SI-tick block is not in any of those numbers — they only measure the joybus xfer wall-clock. The blocking lives in Dolphin's libusb call, not in the bridge.

### How to enable the experimental bridge (if you want to take a swing at the Dolphin side)

The firmware code is preserved behind `CONFIG_JOYBUS_BRIDGE`. To build a firmware with the GBA Link mode (mode 14) selectable:

```sh
cd src && rm -rf build
cmake -G "Unix Makefiles" -DFAMILY=rp2040 \
      -DPICO_BOARD=adafruit_feather_rp2040_usb_host \
      -DJOYPAD_ENABLE_GBA_LINK_BRIDGE=ON \
      -B build
cd build && make joypad_gc2usb_feather_usbhost -j8
```

The device will then enumerate as `GameCube GBA Link Cable` (VID 0x057E / PID 0x0338) when in mode 14, and the `MODE.LIST` CDC command will include mode 14.

To use it from Dolphin you need the fork (or to port the `CSIDevice_GBA_USB` patch to upstream Dolphin yourself). Branch is `joypad-gba-usb` on `github.com/RobertDaleSmith/dolphin` (or wherever the user's fork lives).

### Pointers for the next attempt

If you're picking this up to make Dolphin actually fast:

* The bottleneck is **not** USB, **not** joybus, **not** the bridge firmware — it's Dolphin's CPU thread blocking on `libusb_bulk_transfer` per SI tick. The bridge already runs faster than real hardware.
* Pattern to copy is **libmgba's** `CSIDevice_GBAEmu` — `SendJoybusCommand` queues async, `GetJoybusResponse` polls a queue. No blocking on the SI thread.
* libusb async API (`libusb_submit_transfer` + `libusb_handle_events`) is the right tool. Two attempts in May 17 session failed mysteriously without live debug logs; next attempt should turn on libusb's own debug logging (`LIBUSB_OPTION_LOG_LEVEL = LIBUSB_LOG_LEVEL_DEBUG`) and Dolphin's `SERIALINTERFACE` log channel before testing.
* Dolphin already does async libusb correctly in `Source/Core/Core/IOS/USB/` — read that pattern before writing a new one.
* The fork's `joypad-gba-usb` branch has the failed attempts in the reverted commits, useful as a starting point for what NOT to do.
