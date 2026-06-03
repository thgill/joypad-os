#!/usr/bin/env python3
"""
usbgba-multiboot.py — drive a real Kawasedo multiboot through the
USB vendor bridge, no Dolphin. Mirrors the working eth-multiboot.js
TCP driver byte-for-byte but talks libusb to the firmware's
USB_OUTPUT_MODE_GBA_LINK vendor endpoint instead.

Usage:
    python3.13 tools/gba-bridge/usbgba-multiboot.py [--rom PATH] \
        [--vid 0x057E] [--pid 0x0338] [--channel 0]
"""

import argparse
import struct
import sys
import time

try:
    import usb.core
    import usb.util
except ImportError:
    print("ERROR: pyusb not installed. Run: pip install pyusb", file=sys.stderr)
    sys.exit(1)


# ---------------------------------------------------------------------------
# Constants (lifted verbatim from eth-multiboot.js — the JS driver is the
# authoritative reference: it boots Madden/FFCC/Crystal Chronicles end-to-end
# over the TCP path).
# ---------------------------------------------------------------------------
JSTAT_PSF0       = 0x10
JSTAT_VALID_MASK = 0xC5
GBA_TYPE_ID      = 0x0400
MAGIC_SEDO       = 0x6F646573  # 'sedo'
MAGIC_KAWA       = 0x6177614B  # 'Kawa'
MAGIC_BY         = 0x20796220  # ' by '
CRC_POLY         = 0xA1C1
CRC_SEED         = 0x15A0

CMD_RESET  = 0xFF
CMD_STATUS = 0x00
CMD_READ   = 0x14
CMD_WRITE  = 0x15


def calc_gc_key(rom_len: int) -> int:
    size = (rom_len - 0x200) >> 3
    res1 = (size & 0x3F80) << 1
    res1 |= (size & 0x4000) << 2
    res1 |= (size & 0x7F)
    res1 |= 0x380000
    res1 &= 0xFFFFFFFF
    res2 = (res1 >> 8) & 0xFFFFFFFF
    res2 = (res2 + (res1 >> 16)) & 0xFFFFFFFF
    res2 = (res2 + res1) & 0xFFFFFFFF
    res2 = (res2 << 24) & 0xFFFFFFFF
    res2 = (res2 | res1) & 0xFFFFFFFF
    res2 = (res2 | 0x80808080) & 0xFFFFFFFF
    if (res2 & 0x200) == 0:
        res2 = (res2 ^ MAGIC_KAWA) & 0xFFFFFFFF
    else:
        res2 = (res2 ^ MAGIC_SEDO) & 0xFFFFFFFF
    return res2


def crc_step(crc: int, value: int) -> int:
    for _ in range(32):
        if ((crc ^ value) & 1) != 0:
            crc = ((crc >> 1) ^ CRC_POLY) & 0xFFFFFFFF
        else:
            crc = (crc >> 1) & 0xFFFFFFFF
        value >>= 1
    return crc


def imul32(a: int, b: int) -> int:
    # JavaScript Math.imul: 32x32 multiply, take low 32 bits, signed semantics
    # don't matter because we mask to 32 bits unsigned at the end.
    return (a * b) & 0xFFFFFFFF


# ---------------------------------------------------------------------------
# USB bulk helpers
# ---------------------------------------------------------------------------
def open_bridge(vid: int, pid: int):
    dev = usb.core.find(idVendor=vid, idProduct=pid)
    if dev is None:
        print(f"ERROR: no bridge at VID 0x{vid:04X} PID 0x{pid:04X}", file=sys.stderr)
        sys.exit(2)
    cfg = dev.get_active_configuration()
    iface = next((i for i in cfg if i.bInterfaceClass == 0xFF), None)
    if iface is None:
        print("ERROR: no vendor iface", file=sys.stderr)
        sys.exit(3)
    try:
        if dev.is_kernel_driver_active(iface.bInterfaceNumber):
            dev.detach_kernel_driver(iface.bInterfaceNumber)
    except (NotImplementedError, usb.core.USBError):
        pass
    usb.util.claim_interface(dev, iface.bInterfaceNumber)
    out_ep = next(ep for ep in iface
                  if usb.util.endpoint_direction(ep.bEndpointAddress)
                  == usb.util.ENDPOINT_OUT)
    in_ep = next(ep for ep in iface
                 if usb.util.endpoint_direction(ep.bEndpointAddress)
                 == usb.util.ENDPOINT_IN)
    return dev, iface, out_ep, in_ep


def xfer(out_ep, in_ep, tx: bytes, rx_len: int, timeout_ms: int = 500) -> bytes:
    out_ep.write(tx, timeout=timeout_ms)
    return bytes(in_ep.read(rx_len, timeout=timeout_ms))


def write_word(out_ep, in_ep, word: int) -> bytes:
    return xfer(out_ep, in_ep,
                bytes([CMD_WRITE,
                       word & 0xFF, (word >> 8) & 0xFF,
                       (word >> 16) & 0xFF, (word >> 24) & 0xFF]),
                1)


# ---------------------------------------------------------------------------
# Driver — identical sequence to eth-multiboot.js's multiboot()
# ---------------------------------------------------------------------------
def multiboot(out_ep, in_ep, rom: bytes, channel: int = 0) -> bool:
    rom_len = len(rom)
    print(f"[mb] starting multiboot of {rom_len} bytes (0x{rom_len:X})")

    # Step 1: RESET — first cmd of a cold start is flaky (~50% miss rate
    # even with firmware-side retries). The GBA can take a few seconds
    # after power-cycle before joybus replies are stable. Hammer until we
    # get the expected 0x0400 type reply or 50 attempts pass (~5s).
    print("[mb] step 1: RESET")
    for attempt in range(50):
        rx = xfer(out_ep, in_ep, bytes([CMD_RESET]), 3)
        rst_type = rx[0] | (rx[1] << 8)
        if rst_type == GBA_TYPE_ID:
            print(f"[mb]   rx {rx.hex()} (got it on attempt {attempt})")
            break
        time.sleep(0.1)
    else:
        print(f"[mb] FAIL: expected type 0x0400 after 50 RESETs, "
              f"last rx={rx.hex()}")
        return False
    time.sleep(0.010)

    # Step 2: STATUS poll for PSF0
    print("[mb] step 2: STATUS (poll for PSF0)")
    status_jstat = 0
    sample = []
    for i in range(50):
        rx = xfer(out_ep, in_ep, bytes([CMD_STATUS]), 3)
        t = rx[0] | (rx[1] << 8)
        status_jstat = rx[2]
        if i < 5 or i % 10 == 0:
            sample.append(f"#{i}={rx.hex()}")
        if t == GBA_TYPE_ID and (status_jstat & JSTAT_PSF0):
            print(f"[mb]   PSF0 set on poll {i}: rx={rx.hex()}")
            break
        time.sleep(0.020)
    if not (status_jstat & JSTAT_PSF0):
        print(f"[mb] FAIL: PSF0 never set. Sample: {' '.join(sample)}")
        return False
    time.sleep(0.001)

    # Step 3: READ session_key
    print("[mb] step 3: READ session_key")
    session_key = 0
    for i in range(50):
        rx = xfer(out_ep, in_ep, bytes([CMD_READ]), 5)
        session_key = struct.unpack("<I", rx[:4])[0]
        if session_key != 0:
            print(f"[mb]   seed=0x{session_key:08X} jstat=0x{rx[4]:02X} (poll {i})")
            break
        time.sleep(0.005)
    if session_key == 0:
        print("[mb] FAIL: session_key stayed 0")
        return False
    session_key = (session_key ^ MAGIC_SEDO) & 0xFFFFFFFF
    time.sleep(0.002)

    # Step 4: WRITE our_key
    our_key = calc_gc_key(rom_len)
    print(f"[mb] step 4: WRITE our_key=0x{our_key:08X}")
    r = write_word(out_ep, in_ep, our_key)
    if r[0] & JSTAT_VALID_MASK:
        print(f"[mb] FAIL: our_key bad jstat 0x{r[0]:02X}")
        return False

    # Step 5: stream UNENCRYPTED header (0..0xBF)
    print("[mb] step 5: streaming header (192 bytes, plaintext)")
    for i in range(0, 0xC0, 4):
        word = struct.unpack("<I", rom[i:i+4])[0]
        r = write_word(out_ep, in_ep, word)
        if r[0] & JSTAT_VALID_MASK:
            print(f"[mb] FAIL: header WRITE bad jstat 0x{r[0]:02X} at i={i}")
            return False

    # Step 6: stream encrypted body
    print(f"[mb] step 6: streaming body ({rom_len - 0xC0} bytes, encrypted)")
    fcrc = CRC_SEED
    t0 = time.perf_counter()
    last_pct = -1
    i = 0xC0
    while i < rom_len:
        plaintext = struct.unpack("<I", rom[i:i+4])[0]
        if i == 0xC4:
            plaintext = ((channel & 0xFF) << 8) & 0xFFFFFFFF
        fcrc = crc_step(fcrc, plaintext)
        session_key = (imul32(session_key, MAGIC_KAWA) + 1) & 0xFFFFFFFF
        enc = (plaintext ^ session_key) & 0xFFFFFFFF
        enc = (enc ^ ((~(i + (0x20 << 20)) + 1) & 0xFFFFFFFF)) & 0xFFFFFFFF
        enc = (enc ^ MAGIC_BY) & 0xFFFFFFFF
        r = write_word(out_ep, in_ep, enc)
        if r[0] & JSTAT_VALID_MASK:
            print(f"[mb] FAIL: body WRITE bad jstat 0x{r[0]:02X} at i={i}")
            return False
        pct = ((i - 0xC0) * 100) // (rom_len - 0xC0)
        if pct // 25 != last_pct // 25:
            last_pct = pct
            elapsed = time.perf_counter() - t0
            rate = ((i - 0xC0) / 1024 / elapsed) if elapsed > 0 else 0
            print(f"[mb]   progress: 0x{i:X}/{rom_len:X} ({pct}%) {rate:.2f} KB/s")
        i += 4
    body_time = time.perf_counter() - t0
    print(f"[mb]   body done in {body_time:.2f}s "
          f"({(rom_len-0xC0)/1024/body_time:.2f} KB/s)")

    # Step 7: final fcrc word — packed as (fcrc & 0xFFFF) | (rom_len << 16)
    print(f"[mb] step 7: WRITE final fcrc=0x{fcrc & 0xFFFF:04X}")
    final_word = ((fcrc & 0xFFFF) | (rom_len << 16)) & 0xFFFFFFFF
    session_key = (imul32(session_key, MAGIC_KAWA) + 1) & 0xFFFFFFFF
    enc = (final_word ^ session_key) & 0xFFFFFFFF
    enc = (enc ^ ((~(i + (0x20 << 20)) + 1) & 0xFFFFFFFF)) & 0xFFFFFFFF
    enc = (enc ^ MAGIC_BY) & 0xFFFFFFFF
    r = write_word(out_ep, in_ep, enc)
    if r[0] & JSTAT_VALID_MASK:
        print(f"[mb] FAIL: fcrc WRITE bad jstat 0x{r[0]:02X}")
        return False

    # Step 8: READ crc_reply (BIOS's CRC check)
    print("[mb] step 8: READ crc_reply")
    rx = xfer(out_ep, in_ep, bytes([CMD_READ]), 5, timeout_ms=2000)
    print(f"[mb]   crc_reply: {rx.hex()}")

    print("[mb] ✓ multiboot complete — GBA should be running the uploaded ROM")
    return True


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--rom", default="/tmp/joypad_mb.gba")
    ap.add_argument("--vid", type=lambda s: int(s, 0), default=0x057E)
    ap.add_argument("--pid", type=lambda s: int(s, 0), default=0x0338)
    ap.add_argument("--channel", type=int, default=0)
    args = ap.parse_args()

    with open(args.rom, "rb") as f:
        rom = f.read()
    # Pad to multiple of 16
    pad = (-len(rom)) & 0xF
    if pad:
        rom = rom + bytes(pad)
    print(f"[main] loaded ROM {args.rom} ({len(rom)} bytes after {pad}B pad)")

    dev, iface, out_ep, in_ep = open_bridge(args.vid, args.pid)
    try:
        ok = multiboot(out_ep, in_ep, rom, args.channel)
        sys.exit(0 if ok else 1)
    finally:
        usb.util.release_interface(dev, iface.bInterfaceNumber)
        usb.util.dispose_resources(dev)


if __name__ == "__main__":
    main()
