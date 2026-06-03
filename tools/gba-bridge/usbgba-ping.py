#!/usr/bin/env python3
"""
usbgba-ping.py — sanity test for the USB vendor GBA-link transport.

Talks to the firmware's USB_OUTPUT_MODE_GBA_LINK vendor endpoint.
Sends one each of RESET / STATUS / READ to the bridge and prints the
raw GBA reply. Use this to verify the USB pipe before wiring up the
forked Dolphin.

Setup:
    pip install pyusb
    # macOS: also `brew install libusb` so pyusb can find the backend.

Usage:
    python3 tools/gba-bridge/usbgba-ping.py [--vid 0x057E] [--pid 0x0338]

GBA must be powered on at the BIOS multiboot listen screen for RESET /
STATUS to return real data. Without a GBA on the joybus, replies will
be all-zero (the bridge's joybus-timeout fallback).
"""

import argparse
import sys
import time

try:
    import usb.core
    import usb.util
except ImportError:
    print("ERROR: pyusb not installed. Run: pip install pyusb", file=sys.stderr)
    sys.exit(1)


# Joybus command lengths — must match gba_link_mode.c's table.
CMD_LENGTHS = {
    0xFF: (1, 3),  # RESET → 3 bytes
    0x00: (1, 3),  # STATUS → 3 bytes
    0x14: (1, 5),  # READ → 5 bytes
    0x15: (5, 1),  # WRITE → 1 byte
}


def find_bridge(vid: int, pid: int):
    dev = usb.core.find(idVendor=vid, idProduct=pid)
    if dev is None:
        print(
            f"ERROR: no Joypad GBA Link bridge found at VID 0x{vid:04X} "
            f"PID 0x{pid:04X}.\n"
            f"Make sure the device is plugged in and in GBA Link mode "
            f"(MODE=14 over CDC, or whatever the enum value resolves to).",
            file=sys.stderr,
        )
        sys.exit(2)
    return dev


def claim_vendor_iface(dev):
    """Find the Vendor interface (bInterfaceClass=0xFF) and claim it.

    On macOS the kernel does not auto-attach a driver to vendor-class
    interfaces, so detach_kernel_driver isn't needed. On Linux it
    occasionally is — try and ignore if not supported.
    """
    cfg = dev.get_active_configuration()
    vendor_iface = None
    for iface in cfg:
        if iface.bInterfaceClass == 0xFF:
            vendor_iface = iface
            break
    if vendor_iface is None:
        print("ERROR: device has no vendor-class interface — wrong PID?",
              file=sys.stderr)
        sys.exit(3)

    try:
        if dev.is_kernel_driver_active(vendor_iface.bInterfaceNumber):
            dev.detach_kernel_driver(vendor_iface.bInterfaceNumber)
    except (NotImplementedError, usb.core.USBError):
        pass  # macOS / read-only devices

    usb.util.claim_interface(dev, vendor_iface.bInterfaceNumber)
    out_ep, in_ep = None, None
    for ep in vendor_iface:
        if usb.util.endpoint_direction(ep.bEndpointAddress) == usb.util.ENDPOINT_OUT:
            out_ep = ep
        else:
            in_ep = ep
    if out_ep is None or in_ep is None:
        print("ERROR: vendor interface missing bulk-in or bulk-out endpoint",
              file=sys.stderr)
        sys.exit(4)
    return vendor_iface, out_ep, in_ep


def xfer(out_ep, in_ep, tx: bytes, rx_len: int, timeout_ms: int = 100) -> bytes:
    """Send tx, read rx_len bytes back. Times the round-trip."""
    t0 = time.perf_counter_ns()
    out_ep.write(tx, timeout=timeout_ms)
    rx = in_ep.read(rx_len, timeout=timeout_ms)
    t1 = time.perf_counter_ns()
    return bytes(rx), (t1 - t0) / 1000.0  # microseconds


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--vid", type=lambda s: int(s, 0), default=0x057E)
    ap.add_argument("--pid", type=lambda s: int(s, 0), default=0x0338)
    ap.add_argument("--verbose", "-v", action="store_true")
    args = ap.parse_args()

    dev = find_bridge(args.vid, args.pid)
    print(f"[ping] Found Joypad GBA bridge VID 0x{args.vid:04X} "
          f"PID 0x{args.pid:04X}")
    if args.verbose:
        for s_idx, label in [(1, "manufacturer"), (2, "product")]:
            try:
                print(f"[ping]   {label}: {usb.util.get_string(dev, s_idx)}")
            except usb.core.USBError:
                pass

    iface, out_ep, in_ep = claim_vendor_iface(dev)
    print(f"[ping] Claimed vendor interface #{iface.bInterfaceNumber}: "
          f"OUT 0x{out_ep.bEndpointAddress:02X}, "
          f"IN  0x{in_ep.bEndpointAddress:02X}, "
          f"max-packet {out_ep.wMaxPacketSize}")

    # Probe RESET / STATUS / READ.
    for cmd in (0xFF, 0x00, 0x14):
        tx_len, rx_len = CMD_LENGTHS[cmd]
        tx = bytes([cmd] + [0] * (tx_len - 1))
        try:
            rx, us = xfer(out_ep, in_ep, tx, rx_len)
        except usb.core.USBError as e:
            print(f"[ping] cmd 0x{cmd:02X}: USB error {e}")
            continue
        all_zero = all(b == 0 for b in rx)
        flag = "  (joybus timeout / no GBA on line)" if all_zero else ""
        print(f"[ping] cmd 0x{cmd:02X}: rx={rx.hex()}  rtt={us:.1f}µs{flag}")

    usb.util.release_interface(dev, iface.bInterfaceNumber)
    print("[ping] done")


if __name__ == "__main__":
    main()
